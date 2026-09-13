// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif

#include <digidollar/health.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <consensus/digidollar.h>
#include <consensus/volatility.h>
#include <consensus/dca.h>
#include <consensus/err.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <core_io.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/transaction.h>
#include <sync.h>
#include <txmempool.h>
#include <util/time.h>
#include <validation.h>

#include <atomic>
#include <limits>
#include <util/moneystr.h>
#include <validation.h>
#include <node/blockstorage.h>
#include <txdb.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#include <wallet/digidollarwallet.h>
#endif

#include <algorithm>
#include <memory>

namespace DigiDollar {

namespace {
bool IsCanonicalTaproot(const CScript& script)
{
    int version;
    std::vector<unsigned char> program;
    return script.IsWitnessProgram(version, program) && version == 1 && program.size() == 32;
}

bool CoinMayContainDigiDollar(const Coin& coin, const Consensus::Params& params)
{
    return !coin.IsSpent() && !coin.IsCoinBase() && params.DigiDollarHeight != std::numeric_limits<int>::max() &&
           coin.nHeight >= static_cast<uint32_t>(params.DigiDollarHeight) &&
           IsCanonicalTaproot(coin.out.scriptPubKey);
}
} // namespace

VaultLookupResult LookupCanonicalVault(const COutPoint& outpoint, const Coin& coin,
                                      const Consensus::Params& params,
                                      const CanonicalTxLookup& lookup,
                                      CanonicalVault& vault, std::string& error)
{
    vault = {};
    if (!CoinMayContainDigiDollar(coin, params) || coin.out.nValue <= 0) return VaultLookupResult::NOT_VAULT;
    CTransactionRef creating;
    if (!lookup || !lookup(outpoint.hash, coin.nHeight, creating) || !creating ||
        creating->GetHash() != outpoint.hash || outpoint.n >= creating->vout.size() ||
        creating->vout[outpoint.n] != coin.out) {
        error = strprintf("DigiDollar state not ready: restore the creating block at height %u for %s:%u",
                          coin.nHeight, outpoint.hash.ToString(), outpoint.n);
        return VaultLookupResult::NOT_READY;
    }
    if (!HasDigiDollarMarker(*creating) || creating->IsCoinBase() || DigiDollar::GetDigiDollarTxType(*creating) != DD_TX_MINT)
        return VaultLookupResult::NOT_VAULT;
    CAmount principal{0}, collateral{0};
    if (!ExtractMintAccountingAmounts(*creating, principal, collateral, false)) {
        // The creating block is present and readable; the mint inside it simply
        // does not record an amount and a single collateral output that can be
        // read back. Every node reading that same block reaches the same answer,
        // so this output is not a collateral vault and is left out of the totals.
        // That is different from the node having lost the block, which is the
        // readiness failure above: reporting this as missing data would stop
        // every node at the Thaw Day height for ever, asking the operator to
        // restore a block the node already has.
        LogPrintf("DigiDollar: mint %s does not record a readable vault amount, so output %s:%u "
                  "at height %u is not counted as collateral\n",
                  creating->GetHash().ToString(), outpoint.hash.ToString(), outpoint.n, coin.nHeight);
        return VaultLookupResult::NOT_VAULT;
    }
    // ExtractMintAccountingAmounts requires exactly one positive Taproot output,
    // and the outpoint checked above is that output, so the two amounts agree.
    // The check stays as a guard. If it ever fired the creating block would still
    // be present and readable, so the answer has to be the same on every node:
    // not a vault, rather than a halt.
    if (collateral != coin.out.nValue) {
        LogPrintf("DigiDollar: mint %s records collateral %d but its output %s:%u at height %u holds %d, "
                  "so it is not counted as collateral\n",
                  creating->GetHash().ToString(), collateral, outpoint.hash.ToString(), outpoint.n,
                  coin.nHeight, coin.out.nValue);
        return VaultLookupResult::NOT_VAULT;
    }
    vault = {outpoint, principal, collateral};
    return VaultLookupResult::VAULT;
}

std::optional<int> CalculateChainstateHealth(const ChainstateHealth& state, CAmount price_micro_usd)
{
    if (!state.IsValid() || price_micro_usd <= 0) return std::nullopt;
    return DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
        state.collateral, state.open_vault_principal, price_micro_usd / 10);
}

namespace {
// Counts the full walks of the coin set. See ChainstateHealthRebuildCount().
std::atomic<uint64_t> g_chainstate_health_rebuilds{0};
} // namespace

uint64_t ChainstateHealthRebuildCount() { return g_chainstate_health_rebuilds.load(std::memory_order_relaxed); }

bool ReconstructChainstateHealth(const CCoinsView& view, const Consensus::Params& params,
                                const CanonicalTxLookup& lookup,
                                ChainstateHealth& health, std::string& error,
                                const std::function<bool()>& interrupted,
                                CAmount* circulating_supply,
                                const std::function<void(uint64_t, uint64_t)>& progress,
                                bool* circulating_supply_known)
{
    g_chainstate_health_rebuilds.fetch_add(1, std::memory_order_relaxed);
    try {
        ChainstateHealth rebuilt;
        rebuilt.genesis_hash = params.hashGenesisBlock;
        rebuilt.activation_height = params.nDDThawDayHeight;
        rebuilt.digidollar_height = params.DigiDollarHeight;
        rebuilt.best_block = view.GetBestBlock();
        auto cursor = view.Cursor();
        if (!cursor || cursor->GetBestBlock() != rebuilt.best_block || rebuilt.best_block.IsNull()) {
            error = "DigiDollar state not ready: a consistent UTXO view is required";
            return false;
        }
        CAmount supply{0};
        bool supply_known{true};
        uint64_t scanned{0};
        auto last_progress = SteadyClock::now();
        if (progress) progress(0, 0);
        for (; cursor->Valid(); cursor->Next()) {
            if (interrupted && interrupted()) {
                error = "DigiDollar accounting reconstruction cancelled";
                return false;
            }
            COutPoint outpoint;
            Coin coin;
            if (!cursor->GetKey(outpoint) || !cursor->GetValue(coin)) {
                error = "DigiDollar state not ready: cannot read the UTXO snapshot";
                return false;
            }
            CanonicalVault vault;
            const auto result = LookupCanonicalVault(outpoint, coin, params, lookup, vault, error);
            if (result == VaultLookupResult::NOT_READY) return false;
            if (result == VaultLookupResult::VAULT && !rebuilt.AddVault(vault.principal, vault.collateral)) {
                error = "DigiDollar state not ready: reconstructed vault totals exceed the supported range";
                return false;
            }
            if (circulating_supply && CoinMayContainDigiDollar(coin, params) && coin.out.nValue == 0) {
                CTransactionRef creating;
                if (!lookup || !lookup(outpoint.hash, coin.nHeight, creating) || !creating ||
                    creating->GetHash() != outpoint.hash || outpoint.n >= creating->vout.size() ||
                    creating->vout[outpoint.n] != coin.out) {
                    error = strprintf("DigiDollar supply not ready: restore the creating block at height %u", coin.nHeight);
                    return false;
                }
                if (HasDigiDollarMarker(*creating)) {
                    CAmount amount{0};
                    if (!ExtractDDAmountFromBlockDb(outpoint, coin.nHeight, lookup, amount) || amount <= 0) {
                        if (!circulating_supply_known) {
                            error = "DigiDollar supply not ready: invalid creating token metadata";
                            return false;
                        }
                        supply_known = false;
                    } else if (amount > std::numeric_limits<CAmount>::max() - supply) {
                        error = "DigiDollar supply not ready: token total exceeds the supported range";
                        return false;
                    } else {
                        supply += amount;
                    }
                }
            }
            if (++scanned % 4096 == 0 && SteadyClock::now() - last_progress >= std::chrono::seconds{1}) {
                last_progress = SteadyClock::now();
                if (progress) progress(scanned, 0);
                else LogPrintf("DigiDollar accounting reconstruction: %u outputs checked, %u open vaults\n", scanned, rebuilt.active_vaults);
            }
        }
        cursor->CheckStatus();
        if (view.GetBestBlock() != rebuilt.best_block || !rebuilt.IsValid()) {
            error = "DigiDollar state not ready: UTXO state changed during reconstruction";
            return false;
        }
        if (progress) progress(scanned, scanned);
        if (interrupted && interrupted()) {
            error = "DigiDollar accounting reconstruction cancelled";
            return false;
        }
        health = rebuilt;
        if (circulating_supply && supply_known) *circulating_supply = supply;
        if (circulating_supply_known) *circulating_supply_known = supply_known;
        return true;
    } catch (const dbwrapper_error& exception) {
        error = std::string("DigiDollar state not ready: restore unreadable chainstate data: ") + exception.what();
        return false;
    }
}

bool UpdateChainstateHealth(const CTransaction& tx, const std::vector<Coin>& inputs,
                           const Consensus::Params& params, const CanonicalTxLookup& lookup,
                           ChainstateHealth& health, bool undo, std::string& error, uint32_t creating_height)
{
    if (tx.IsCoinBase()) return true;
    if (inputs.size() != tx.vin.size()) {
        error = "DigiDollar accounting requires complete input coins";
        return false;
    }
    auto next = health;
    // Reverse output creation before input restoration, matching UTXO undo.
    auto mint = [&]() {
        if (params.DigiDollarHeight == std::numeric_limits<int>::max() ||
            creating_height < static_cast<uint32_t>(params.DigiDollarHeight)) return true;
        if (!HasDigiDollarMarker(tx) || DigiDollar::GetDigiDollarTxType(tx) != DD_TX_MINT) return true;
        CAmount principal{0}, collateral{0};
        if (!ExtractMintAccountingAmounts(tx, principal, collateral, false)) return false;
        return undo ? next.RemoveVault(principal, collateral) : next.AddVault(principal, collateral);
    };
    if (undo && !mint()) {
        error = "DigiDollar accounting cannot undo minted vault";
        return false;
    }
    for (size_t i = 0; i < inputs.size(); ++i) {
        CanonicalVault vault;
        const auto result = LookupCanonicalVault(tx.vin[i].prevout, inputs[i], params, lookup, vault, error);
        if (result == VaultLookupResult::NOT_READY) return false;
        if (result == VaultLookupResult::VAULT &&
            !(undo ? next.AddVault(vault.principal, vault.collateral) : next.RemoveVault(vault.principal, vault.collateral))) {
            error = "DigiDollar accounting cannot apply vault spend";
            return false;
        }
    }
    if (!undo && !mint()) {
        error = "DigiDollar accounting cannot add minted vault";
        return false;
    }
    health = next;
    return true;
}

SupplyChangeResult GetCirculatingSupplyChange(const CTransaction& tx, const std::vector<Coin>& inputs,
                                             uint32_t height, const Consensus::Params& params,
                                             const CanonicalTxLookup& lookup, CAmount& change, std::string& error)
{
    if (tx.IsCoinBase()) { change = 0; return SupplyChangeResult::KNOWN; }
    if (inputs.size() != tx.vin.size()) {
        error = "DigiDollar supply requires complete input coins";
        return SupplyChangeResult::FAILURE;
    }
    auto token_amount = [&](const COutPoint& outpoint, const Coin& coin, const CanonicalTxLookup& source,
                            CAmount& amount) {
        if (!CoinMayContainDigiDollar(coin, params) || coin.out.nValue != 0) {
            amount = 0;
            return SupplyChangeResult::KNOWN;
        }
        CTransactionRef creating;
        if (!source || !source(outpoint.hash, coin.nHeight, creating) || !creating ||
            creating->GetHash() != outpoint.hash || outpoint.n >= creating->vout.size() ||
            creating->vout[outpoint.n] != coin.out) {
            error = strprintf("DigiDollar supply not ready: restore the creating block at height %u", coin.nHeight);
            return SupplyChangeResult::FAILURE;
        }
        if (!HasDigiDollarMarker(*creating)) {
            amount = 0;
            return SupplyChangeResult::KNOWN;
        }
        const CanonicalTxLookup verified_source = [&](const uint256& hash, uint32_t height, CTransactionRef& out) {
            if (hash != outpoint.hash || height != coin.nHeight) return false;
            out = creating;
            return true;
        };
        if (!ExtractDDAmountFromBlockDb(outpoint, coin.nHeight, verified_source, amount) || amount <= 0) {
            return SupplyChangeResult::UNKNOWN_METADATA;
        }
        return SupplyChangeResult::KNOWN;
    };
    __int128 delta{0};
    bool unknown_metadata{false};
    for (size_t i = 0; i < inputs.size(); ++i) {
        CAmount amount{0};
        const auto result = token_amount(tx.vin[i].prevout, inputs[i], lookup, amount);
        if (result == SupplyChangeResult::FAILURE) return result;
        if (result == SupplyChangeResult::UNKNOWN_METADATA) unknown_metadata = true;
        else delta -= amount;
    }
    const auto created = MakeTransactionRef(tx);
    const CanonicalTxLookup created_lookup = [&](const uint256& txid, uint32_t coin_height, CTransactionRef& out) {
        if (txid != tx.GetHash() || coin_height != height) return false;
        out = created;
        return true;
    };
    for (uint32_t i = 0; i < tx.vout.size(); ++i) {
        CAmount amount{0};
        const auto result = token_amount(COutPoint{tx.GetHash(), i}, Coin{tx.vout[i], static_cast<int>(height), false}, created_lookup, amount);
        if (result == SupplyChangeResult::FAILURE) return result;
        if (result == SupplyChangeResult::UNKNOWN_METADATA) unknown_metadata = true;
        else delta += amount;
    }
    if (unknown_metadata) {
        error = "DigiDollar supply unavailable: creating token metadata does not establish an unambiguous amount";
        return SupplyChangeResult::UNKNOWN_METADATA;
    }
    if (delta < std::numeric_limits<CAmount>::min() || delta > std::numeric_limits<CAmount>::max()) {
        error = "DigiDollar circulating supply change exceeds the supported range";
        return SupplyChangeResult::FAILURE;
    }
    change = static_cast<CAmount>(delta);
    return SupplyChangeResult::KNOWN;
}

bool CalculateCirculatingSupplyChange(const CTransaction& tx, const std::vector<Coin>& inputs,
                                     uint32_t height, const Consensus::Params& params,
                                     const CanonicalTxLookup& lookup, CAmount& change, std::string& error)
{
    return GetCirculatingSupplyChange(tx, inputs, height, params, lookup, change, error) == SupplyChangeResult::KNOWN;
}

// Static member definitions
SystemMetrics SystemHealthMonitor::s_currentMetrics;
std::mutex SystemHealthMonitor::s_metricsMutex;
std::map<int64_t, int> SystemHealthMonitor::s_healthHistory;
std::mutex SystemHealthMonitor::s_historyMutex;
bool SystemHealthMonitor::s_initialized = false;

// Standard tier definitions (lock days)
// NOTE: Tier 0 uses 0 to represent 1 hour (240 blocks) - special case handled by LockDaysToBlocks()
static const std::vector<int> TIER_LOCK_DAYS = {0, 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650}; // 1h, 30d to 10y (10 tiers)

SystemMetrics SystemHealthMonitor::GetSystemMetrics()
{
    if (!s_initialized) {
        Initialize();
    }

    // Update current metrics by scanning UTXO set
    // Note: Internal calls don't have chainstate access, metrics updated on-demand
    // ScanUTXOSet(nullptr);
    UpdateTierMetrics();
    UpdateProtectionStatus();
    UpdateOracleStatus();

    return s_currentMetrics;
}

std::vector<SystemMetrics::TierMetrics> SystemHealthMonitor::GetTierBreakdown()
{
    if (!s_initialized) {
        Initialize();
    }

    // Ensure metrics are current
    // Note: UTXO scan happens on-demand from RPC
    // ScanUTXOSet(nullptr);
    UpdateTierMetrics();

    return s_currentMetrics.tiers;
}

bool SystemHealthMonitor::ShouldAlert(const std::string& metric)
{
    if (!s_initialized) {
        Initialize();
    }

    SystemMetrics metrics = GetSystemMetrics();

    if (metric == "system_health") {
        return CheckHealthAlert(metrics);
    } else if (metric == "total_supply") {
        return CheckSupplyAlert(metrics);
    } else if (metric == "total_collateral") {
        return CheckCollateralAlert(metrics);
    } else if (metric == "oracle_status") {
        return CheckOracleAlert(metrics);
    } else if (metric == "volatility") {
        return CheckVolatilityAlert(metrics);
    } else if (metric == "position_count") {
        return CheckPositionAlert(metrics);
    }

    // Unknown metric
    return false;
}

std::vector<int> SystemHealthMonitor::GetHealthHistory(int blocks)
{
    if (!s_initialized) {
        Initialize();
    }

    std::vector<int> history;
    if (blocks <= 0) {
        return history;
    }

    // Cap the maximum history size to prevent excessive memory allocation
    const int MAX_HISTORY_REQUEST = 100000;
    int blocksToFetch = std::min(blocks, MAX_HISTORY_REQUEST);

    // TODO: Fix chainstate access - temporary mock implementation.
    // In a production system, this should receive a ChainstateManager
    // reference and read the actual tip via Active().Tip(). For now, use
    // mock data to prevent compilation errors. This should be replaced with
    // proper chainstate access.
    int64_t currentHeight = 1000000; // Mock current height

    // Collect history from most recent to oldest
    for (int i = 0; i < blocksToFetch && (currentHeight - i) >= 0; ++i) {
        int64_t height = currentHeight - i;
        auto it = s_healthHistory.find(height);
        if (it != s_healthHistory.end()) {
            history.push_back(it->second);
        } else {
            // If no recorded data, use current health as estimate
            history.push_back(s_currentMetrics.systemHealth);
        }
    }

    return history;
}

void SystemHealthMonitor::UpdateMetrics(const CBlock& block)
{
    if (!s_initialized) {
        Initialize();
    }

    // Update metrics with new block data
    // Note: UTXO scan requires chainstate access from caller
    // ScanUTXOSet(nullptr);
    UpdateTierMetrics();
    UpdateProtectionStatus();
    UpdateOracleStatus();

    // RH-36a: First real health update from block processing — unlock
    // ERR state so GetCurrentState() can read DCA cache again.
    DigiDollar::ERR::EmergencyRedemptionRatio::ClearStateReconstructed();

    // Record health history
    // TODO: Fix chainstate access - temporary mock implementation
    // For now, use mock height to prevent compilation errors
    int64_t mockHeight = 1000000; // This should be replaced with proper chainstate access
    RecordHealthHistory(mockHeight, s_currentMetrics.systemHealth);

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar health updated: %d%% health, %s DD supply, %s DGB collateral\n",
             s_currentMetrics.systemHealth,
             FormatMoney(s_currentMetrics.totalDDSupply),
             FormatMoney(s_currentMetrics.totalCollateral));
}

UniValue SystemHealthMonitor::GetHealthReport()
{
    if (!s_initialized) {
        Initialize();
    }

    SystemMetrics metrics = GetSystemMetrics();

    UniValue result(UniValue::VOBJ);

    // Overall system metrics
    result.pushKV("supply", ValueFromAmount(metrics.totalDDSupply));
    result.pushKV("collateral", ValueFromAmount(metrics.totalCollateral));
    result.pushKV("health", metrics.systemHealth);

    // Protection system status
    result.pushKV("dca_multiplier", metrics.dcaMultiplier);
    result.pushKV("err_active", metrics.errActive);
    result.pushKV("volatility", metrics.volatility);
    result.pushKV("minting_frozen", metrics.mintingFrozen);

    // Tier breakdown
    UniValue tiers(UniValue::VARR);
    for (const auto& tier : metrics.tiers) {
        UniValue t(UniValue::VOBJ);
        t.pushKV("lock_days", tier.lockDays);
        t.pushKV("dd_minted", ValueFromAmount(tier.ddMinted));
        t.pushKV("dgb_locked", ValueFromAmount(tier.dgbLocked));
        t.pushKV("positions", tier.positions);
        t.pushKV("health", tier.healthRatio);
        t.pushKV("status", HealthUtils::FormatHealthStatus(tier.healthRatio));
        t.pushKV("action", HealthUtils::GetRecommendedAction(tier.healthRatio));
        tiers.push_back(t);
    }
    result.pushKV("tiers", tiers);

    // Oracle status
    UniValue oracles(UniValue::VOBJ);
    oracles.pushKV("active_count", metrics.activeOracles);
    oracles.pushKV("last_price", int64_t{metrics.lastOraclePrice});
    oracles.pushKV("last_price_micro_usd", int64_t{metrics.lastOraclePrice});
    oracles.pushKV("last_update", metrics.lastOracleUpdate);

    // TODO: Fix chainstate access - temporary mock implementation
    // For now, use mock height to prevent compilation errors
    int64_t mockHeight = 1000000; // This should be replaced with proper chainstate access
    oracles.pushKV("blocks_since_update", mockHeight - metrics.lastOracleUpdate);
    oracles.pushKV("is_stale", (mockHeight - metrics.lastOracleUpdate) > AlertThresholds::STALE_ORACLE_BLOCKS);
    result.pushKV("oracles", oracles);

    // Alert summary
    UniValue alerts(UniValue::VARR);
    if (ShouldAlert("system_health")) alerts.push_back("system_health");
    if (ShouldAlert("total_supply")) alerts.push_back("total_supply");
    if (ShouldAlert("total_collateral")) alerts.push_back("total_collateral");
    if (ShouldAlert("oracle_status")) alerts.push_back("oracle_status");
    if (ShouldAlert("volatility")) alerts.push_back("volatility");
    if (ShouldAlert("position_count")) alerts.push_back("position_count");
    result.pushKV("active_alerts", alerts);

    // System recommendations
    result.pushKV("overall_status", HealthUtils::FormatHealthStatus(metrics.systemHealth));
    result.pushKV("recommended_action", HealthUtils::GetRecommendedAction(metrics.systemHealth));

    return result;
}

void SystemHealthMonitor::Initialize()
{
    if (s_initialized) {
        LogPrint(BCLog::DIGIDOLLAR, "Initialize: Already initialized, skipping\n");
        return;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Initialize: Initializing DigiDollar health monitoring system (totalDDSupply=%lld)\n",
             static_cast<long long>(s_currentMetrics.totalDDSupply));

    // Don't reset metrics if they've already been populated by ScanUTXOSet
    if (s_currentMetrics.totalDDSupply == 0 && s_currentMetrics.totalCollateral == 0) {
        LogPrint(BCLog::DIGIDOLLAR, "Initialize: No existing data, initializing fresh metrics structure\n");
        // Initialize metrics structure
        s_currentMetrics = SystemMetrics();

        // Initialize tier breakdown
        s_currentMetrics.tiers.clear();
        for (int lockDays : TIER_LOCK_DAYS) {
            s_currentMetrics.tiers.emplace_back(lockDays, 0, 0, 0, 0);
        }
    } else {
        LogPrint(BCLog::DIGIDOLLAR, "Initialize: Metrics already populated (totalDDSupply=%lld, totalCollateral=%lld), preserving data\n",
                 static_cast<long long>(s_currentMetrics.totalDDSupply), static_cast<long long>(s_currentMetrics.totalCollateral));
        // Just ensure tiers are initialized if empty
        if (s_currentMetrics.tiers.empty()) {
            for (int lockDays : TIER_LOCK_DAYS) {
                s_currentMetrics.tiers.emplace_back(lockDays, 0, 0, 0, 0);
            }
        }
    }

    // Clear health history
    s_healthHistory.clear();

    // Perform initial scan
    // Note: UTXO scan happens on-demand from RPC with chainstate access
    // ScanUTXOSet(nullptr);
    UpdateTierMetrics();
    UpdateProtectionStatus();
    UpdateOracleStatus();

    s_initialized = true;

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar health monitoring initialized: %d%% initial health\n",
             s_currentMetrics.systemHealth);
}

void SystemHealthMonitor::Shutdown()
{
    if (!s_initialized) {
        return;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Shutting down DigiDollar health monitoring system\n");

    // Clear data structures
    s_currentMetrics = SystemMetrics();
    s_healthHistory.clear();

    s_initialized = false;
}

bool SystemHealthMonitor::ScanUTXOSet(CCoinsView* view, CCoinsView* validation_view, const node::BlockManager* blockman, const CTxMemPool* mempool, const CChain* chain, const Consensus::Params* consensus)
{
    return ScanUTXOSet(view, validation_view, blockman, mempool, chain, consensus, {}).status == HealthScanResult::Status::COMPLETE;
}

HealthScanResult SystemHealthMonitor::ScanUTXOSet(CCoinsView* view, CCoinsView* validation_view,
                                                const node::BlockManager* blockman, const CTxMemPool* mempool,
                                                const CChain* chain, const Consensus::Params* consensus,
                                                const HealthScanCallbacks& callbacks)
{
    // DigiDollar activation floor: a DD vault output can only be created at/after
    // activation, which cannot happen below the deployment's minimum activation height.
    // Below the floor there are no DD vaults, so we skip those coins without reading a
    // block — this is what lets the seed run identically on a pruned node (pre-floor
    // blocks are gone) and a full node (pre-floor blocks are present but hold no vaults).
    const int dd_floor = consensus ? DigiDollar::EarliestActivationFloor(*consensus) : 0;

    // Keep a complete snapshot available until the replacement scan succeeds.
    auto rebuilt = GetCachedMetrics();
    rebuilt.totalDDSupply = 0;
    rebuilt.totalCollateral = 0;
    rebuilt.totalActivePositions = 0;
    rebuilt.systemHealth = 0;
    rebuilt.hasCanonicalHealth = false;

    uint64_t outputs_checked{0};
    auto last_progress = SteadyClock::now();
    if (callbacks.progress) callbacks.progress(0, 0);
    const auto cancelled = [&] { return callbacks.cancelled && callbacks.cancelled(); };
    const auto cancellation = [] {
        LogPrintf("DigiDollar legacy health reconstruction cancelled; cached metrics were not replaced\n");
        return HealthScanResult{HealthScanResult::Status::CANCELLED, "DigiDollar legacy health reconstruction cancelled"};
    };
    const auto publish = [&]() -> HealthScanResult {
        if (callbacks.progress) callbacks.progress(outputs_checked, outputs_checked);
        if (cancelled()) return cancellation();
        std::lock_guard<std::mutex> lock(s_metricsMutex);
        s_currentMetrics.totalDDSupply = rebuilt.totalDDSupply;
        s_currentMetrics.totalCollateral = rebuilt.totalCollateral;
        s_currentMetrics.totalActivePositions = rebuilt.totalActivePositions;
        s_currentMetrics.systemHealth = rebuilt.systemHealth;
        s_currentMetrics.hasCanonicalHealth = rebuilt.hasCanonicalHealth;
        for (auto& tier : s_currentMetrics.tiers) {
            tier.ddMinted = 0;
            tier.dgbLocked = 0;
            tier.positions = 0;
            tier.healthRatio = 0;
        }
        return {};
    };
    if (cancelled()) return cancellation();

    if (!view) {
        LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: No coins view provided\n");
        return publish();
    }

    if (!blockman) {
        LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: No block manager provided - skipping UTXO scan (unit test mode?)\n");
        return publish();
    }

    // Create cursor to iterate all UTXOs (similar to gettxoutsetinfo)
    std::unique_ptr<CCoinsViewCursor> pcursor(view->Cursor());
    if (!pcursor) {
        LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Unable to create UTXO cursor\n");
        return publish();
    }

    LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Starting blockchain-wide UTXO scan with full transaction access\n");
    LogPrintf("DigiDollar legacy health reconstruction: checking unspent outputs\n");

    bool complete = true;
    std::string error;
    size_t vaults_found = 0;
    size_t dd_amount_extracted = 0;
    size_t dd_amount_estimated = 0;
    size_t utxos_scanned = 0;
    size_t vault_candidates_checked = 0;
    size_t p2tr_found = 0;

    // Track which transactions we've seen to avoid double-counting
    std::set<uint256> processed_txids;

    // Iterate through ALL UTXOs in the blockchain
    while (pcursor->Valid()) {
        if (cancelled()) return cancellation();
        if (++outputs_checked % 4096 == 0 && SteadyClock::now() - last_progress >= std::chrono::seconds{1}) {
            last_progress = SteadyClock::now();
            LogPrintf("DigiDollar legacy health reconstruction: %u outputs checked\n", outputs_checked);
            if (callbacks.progress) callbacks.progress(outputs_checked, 0);
        }
        COutPoint key;
        Coin coin;

        if (!pcursor->GetKey(key) || !pcursor->GetValue(coin)) {
            LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Error reading UTXO\n");
            break;
        }

        const uint256& txid = key.hash;

        //  Skip spent coins - they are marked for deletion but haven't been pruned yet
        if (coin.IsSpent()) {
            pcursor->Next();
            continue;
        }

        utxos_scanned++;

        if (processed_txids.find(txid) != processed_txids.end()) {
            pcursor->Next();
            continue;
        }

        // DigiDollar mint collateral is a positive P2TR output, but consensus
        // validation does not require it to be vout[0].
        if (coin.out.scriptPubKey.size() == 34 &&
            coin.out.scriptPubKey[0] == OP_1 && coin.out.nValue > 0) {

            // Skip P2TR coins created before the DigiDollar floor — they cannot be DD
            // vaults, so there is no need to read their (possibly pruned) creating block.
            if (chain && dd_floor > 0 && static_cast<int>(coin.nHeight) < dd_floor) {
                processed_txids.insert(txid);
                pcursor->Next();
                continue;
            }

            vault_candidates_checked++;
            p2tr_found++;
            LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Found P2TR output with value at %s:%d, fetching full transaction...\n",
                     txid.ToString(), key.n);

            // CRITICAL: Before processing, validate this UTXO still exists in current chainstate
            // CoinsDB may contain spent-but-not-pruned coins
            if (validation_view) {
                Coin validation_coin;
                if (!validation_view->GetCoin(key, validation_coin) || validation_coin.IsSpent()) {
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Skipping spent DD vault candidate: %s:%d\n", txid.ToString(), key.n);
                    processed_txids.insert(txid);
                    pcursor->Next();
                    continue;
                }
            }

            CAmount collateral = 0;
            CAmount ddAmount = 0;

            // Get the full transaction that created this coin. On a full node with a
            // transaction index GetTransaction finds it via txindex; on a pruned node
            // (no txindex) we hand it the block index at the coin's creation height so it
            // reads the creating tx from the retained block instead. Same transaction,
            // same result — just a different source.
            uint256 hashBlock;
            const CBlockIndex* creating_block = chain ? (*chain)[coin.nHeight] : nullptr;
            CTransactionRef tx = node::GetTransaction(creating_block, mempool, txid, hashBlock, *blockman);

            if (tx) {
                if (!DigiDollar::ExtractMintAccountingAmounts(*tx, ddAmount, collateral)) {
                    processed_txids.insert(txid);
                    pcursor->Next();
                    continue;
                }
                dd_amount_extracted++;
                LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Extracted exact DD amount %s from tx %s\n",
                         FormatMoney(ddAmount), txid.ToString());
            } else {
                // A coin at/above the DigiDollar activation floor sits in a block the
                // node is REQUIRED to be able to read (retained window on a pruned node,
                // full history otherwise). Failing to read it means the block data is
                // damaged (e.g. a truncated/partially-restored blk file that the index
                // still marks as present). The metrics seeded here feed consensus
                // DCA/ERR health, so this must fail CLOSED — report incomplete instead
                // of silently undercounting supply/collateral.
                if (chain && dd_floor > 0 && static_cast<int>(coin.nHeight) >= dd_floor) {
                    error = strprintf("DigiDollar legacy health is unavailable: cannot read transaction %s at height %u (block %s). Restore the required retained block data and retry startup.",
                                      txid.ToString(), coin.nHeight,
                                      creating_block ? creating_block->GetBlockHash().ToString() : "unknown");
                    LogPrintf("%s\n", error);
                    complete = false;
                } else {
                    LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Could not fetch tx %s, skipping unverified DD vault candidate\n",
                             txid.ToString());
                }
                processed_txids.insert(txid);
                pcursor->Next();
                continue;
            }

            // Add to totals
            rebuilt.totalCollateral += collateral;
            rebuilt.totalDDSupply += ddAmount;
            rebuilt.totalActivePositions++;
            vaults_found++;
            processed_txids.insert(txid);

            LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Found DD vault - collateral=%s, DD=%s (exact)\n",
                     FormatMoney(collateral), FormatMoney(ddAmount));

            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: UTXO Scanner found vault: %s:%d - Collateral=%s DGB, DD=%s cents\n",
                     txid.ToString(), key.n, FormatMoney(collateral), FormatMoney(ddAmount));
        }

        pcursor->Next();
    }

    LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Scan complete - %d UTXOs scanned, %d vault candidates checked, %d P2TR found\n",
             utxos_scanned, vault_candidates_checked, p2tr_found);
    LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Results - Found %d vaults, %s DGB collateral, %s DD supply\n",
             vaults_found, FormatMoney(rebuilt.totalCollateral),
             FormatMoney(rebuilt.totalDDSupply));
    LogPrint(BCLog::DIGIDOLLAR, "ScanUTXOSet: Exact amounts: %d, Estimated amounts: %d\n",
             dd_amount_extracted, dd_amount_estimated);

    if (!complete) return {HealthScanResult::Status::READ_ERROR, error};
    const auto result = publish();
    if (result.status == HealthScanResult::Status::COMPLETE) {
        LogPrintf("DigiDollar legacy health reconstruction complete: %zu vaults, collateral %s DGB, legacy DD total %d cents\n",
                  vaults_found, FormatMoney(rebuilt.totalCollateral), rebuilt.totalDDSupply);
    }
    return result;
}

bool SystemHealthMonitor::ReconstructFromChain(ChainstateManager& chainman)
{
    return ReconstructFromChain(chainman, {}).status == HealthScanResult::Status::COMPLETE;
}

HealthScanResult SystemHealthMonitor::ReconstructFromChain(ChainstateManager& chainman, const HealthScanCallbacks& callbacks)
{
    // DD-FINAL-003 / AR-CONSENSUS-1: seed the cached system-health metrics from
    // the on-chain UTXO set at startup so consensus DCA/ERR health is identical
    // on every node regardless of restart history. Mirrors the oracle price-cache
    // reconstruction (OracleBundleManager::LoadPricesFromChain) called alongside
    // this at node init.
    // During a reindex / reindex-chainstate the node replays every block, so the
    // incremental OnMint/OnRedeemConnected hooks rebuild the metrics on their own;
    // a startup scan would be redundant. Skipping it also avoids touching the
    // CoinsDB while the block files may be read-only mid-reindex.
    if (node::fReindex) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Health: skipping startup reconstruction during reindex (block replay rebuilds metrics)\n");
        return {};
    }
    // The tip, flushed cursor and validation view must describe the same chain.
    LOCK(::cs_main);
    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    if (tip == nullptr) {
        return {};
    }
    if (DigiDollar::IsThawDayActive(chainman.GetConsensus(), tip->nHeight)) {
        const auto state = chainman.ActiveChainstate().CoinsTip().GetDigiDollarState();
        // Canonical initialization precedes import and verification. This late
        // display initialization must never overwrite its inputs with globals.
        if (state && state->Matches(chainman.GetConsensus().hashGenesisBlock, tip->GetBlockHash(), chainman.GetConsensus().nDDThawDayHeight, chainman.GetConsensus().DigiDollarHeight) && state->history_checked) return {};
        return {HealthScanResult::Status::READ_ERROR,
                strprintf("DigiDollar canonical health is unavailable at height %d (block %s). Restore the required chainstate history and retry startup.",
                          tip->nHeight, tip->GetBlockHash().ToString())};
    }
    // Skip the (potentially expensive) full UTXO scan unless DigiDollar is active
    // at the current tip — pre-activation and non-DD chains have no DD vaults.
    if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Health: skipping startup reconstruction (DigiDollar not active at tip)\n");
        return {};
    }

    // Flush the in-memory coins cache to disk so the CoinsDB cursor below sees
    // every UTXO — matching the getdigidollarstats scan path. On an unclean restart
    // or -loadblock the background importer may connect blocks whose coins are not
    // yet flushed; without this the seed could undercount and (after DD-FINAL-004,
    // which recomputes health from the seeded supply/collateral) diverge between
    // nodes. The reindex path returned above, so by here the datadir is writable.
    Chainstate& active = chainman.ActiveChainstate();
    if (callbacks.cancelled && callbacks.cancelled()) {
        return {HealthScanResult::Status::CANCELLED, "DigiDollar legacy health reconstruction cancelled"};
    }
    active.ForceFlushStateToDisk();
    return ScanUTXOSet(&active.CoinsDB(), &active.CoinsTip(), &active.m_blockman, /*mempool=*/nullptr,
                      &active.m_chain, &chainman.GetConsensus(), callbacks);
}

// ============================================================================
// Incremental metrics tracking (T5-06)
// Called from ConnectBlock/DisconnectBlock under cs_main.
// ============================================================================

static void AddClampedAmount(CAmount& total, CAmount amount, const char* label)
{
    if (amount <= 0) return;
    if (total <= std::numeric_limits<CAmount>::max() - amount) {
        total += amount;
        return;
    }
    LogPrintf("Health: WARNING - %s would overflow, capping\n", label);
    total = std::numeric_limits<CAmount>::max();
}

void SystemHealthMonitor::OnMintConnected(CAmount ddAmount, CAmount dgbCollateral)
{
    std::lock_guard<std::mutex> lock(s_metricsMutex); // RH-44: thread safety
    AddClampedAmount(s_currentMetrics.totalDDSupply, ddAmount, "totalDDSupply");
    AddClampedAmount(s_currentMetrics.totalCollateral, dgbCollateral, "totalCollateral");
    s_currentMetrics.systemHealth = 0;
    s_currentMetrics.hasCanonicalHealth = false;
    LogPrint(BCLog::DIGIDOLLAR, "Health: Mint connected - DD +%s, Collateral +%s (totals: DD=%s, Collateral=%s)\n",
             FormatMoney(ddAmount), FormatMoney(dgbCollateral),
             FormatMoney(s_currentMetrics.totalDDSupply), FormatMoney(s_currentMetrics.totalCollateral));
}

void SystemHealthMonitor::OnRedeemConnected(CAmount ddAmount, CAmount dgbCollateral)
{
    std::lock_guard<std::mutex> lock(s_metricsMutex); // RH-44: thread safety
    s_currentMetrics.totalDDSupply = std::max<CAmount>(0, s_currentMetrics.totalDDSupply - ddAmount);
    s_currentMetrics.totalCollateral = std::max<CAmount>(0, s_currentMetrics.totalCollateral - dgbCollateral);
    s_currentMetrics.systemHealth = 0;
    s_currentMetrics.hasCanonicalHealth = false;
    LogPrint(BCLog::DIGIDOLLAR, "Health: Redeem connected - DD -%s, Collateral -%s (totals: DD=%s, Collateral=%s)\n",
             FormatMoney(ddAmount), FormatMoney(dgbCollateral),
             FormatMoney(s_currentMetrics.totalDDSupply), FormatMoney(s_currentMetrics.totalCollateral));
}

void SystemHealthMonitor::OnMintDisconnected(CAmount ddAmount, CAmount dgbCollateral)
{
    std::lock_guard<std::mutex> lock(s_metricsMutex); // RH-44: thread safety
    s_currentMetrics.totalDDSupply = std::max<CAmount>(0, s_currentMetrics.totalDDSupply - ddAmount);
    s_currentMetrics.totalCollateral = std::max<CAmount>(0, s_currentMetrics.totalCollateral - dgbCollateral);
    s_currentMetrics.systemHealth = 0;
    s_currentMetrics.hasCanonicalHealth = false;
    LogPrint(BCLog::DIGIDOLLAR, "Health: Mint disconnected - DD -%s, Collateral -%s (totals: DD=%s, Collateral=%s)\n",
             FormatMoney(ddAmount), FormatMoney(dgbCollateral),
             FormatMoney(s_currentMetrics.totalDDSupply), FormatMoney(s_currentMetrics.totalCollateral));
}

void SystemHealthMonitor::OnRedeemDisconnected(CAmount ddAmount, CAmount dgbCollateral)
{
    std::lock_guard<std::mutex> lock(s_metricsMutex); // RH-44: thread safety
    AddClampedAmount(s_currentMetrics.totalDDSupply, ddAmount, "totalDDSupply");
    AddClampedAmount(s_currentMetrics.totalCollateral, dgbCollateral, "totalCollateral");
    s_currentMetrics.systemHealth = 0;
    s_currentMetrics.hasCanonicalHealth = false;
    LogPrint(BCLog::DIGIDOLLAR, "Health: Redeem disconnected - DD +%s, Collateral +%s (totals: DD=%s, Collateral=%s)\n",
             FormatMoney(ddAmount), FormatMoney(dgbCollateral),
             FormatMoney(s_currentMetrics.totalDDSupply), FormatMoney(s_currentMetrics.totalCollateral));
}

#ifdef ENABLE_WALLET
void SystemHealthMonitor::AggregateWalletStats(
    const std::vector<std::shared_ptr<wallet::CWallet>>& wallets,
    CAmount& totalDDSupply,
    CAmount& totalCollateral)
{
    // Reset output parameters
    totalDDSupply = 0;
    totalCollateral = 0;

    // Aggregate across all loaded wallets
    for (const auto& wallet : wallets) {
        if (!wallet) continue;

        // Get DigiDollar wallet interface
        DigiDollarWallet* ddWallet = wallet->GetDDWallet();
        if (!ddWallet) continue;

        // Get all active positions from this wallet
        std::vector<WalletCollateralPosition> positions = ddWallet->GetDDTimeLocks(true);

        // Sum up collateral and DD minted from all positions
        for (const auto& pos : positions) {
            totalCollateral += pos.dgb_collateral;
            totalDDSupply += pos.dd_minted;
        }
    }

    LogPrint(BCLog::DIGIDOLLAR, "AggregateWalletStats: Aggregated %d wallets -> %s DD supply, %s DGB collateral\n",
             wallets.size(),
             FormatMoney(totalDDSupply),
             FormatMoney(totalCollateral));
}
#else
void SystemHealthMonitor::AggregateWalletStats(
    const std::vector<std::shared_ptr<wallet::CWallet>>& wallets,
    CAmount& totalDDSupply,
    CAmount& totalCollateral)
{
    totalDDSupply = 0;
    totalCollateral = 0;
    LogPrint(BCLog::DIGIDOLLAR, "AggregateWalletStats: wallet support disabled, skipped %zu wallets\n",
             wallets.size());
}
#endif

void SystemHealthMonitor::UpdateTierMetrics()
{
    // Calculate current DGB price for health calculations. Oracle and
    // volatility paths store prices in micro-USD; DCA health math consumes
    // millicents, so convert at this boundary instead of treating micro-USD
    // as cents and inflating canonical health.
    const CAmount currentPriceMicroUSD = GetLastOraclePrice();
    const CAmount currentPriceMillicents = currentPriceMicroUSD > 0
        ? currentPriceMicroUSD / 10
        : 0;

    // Update per-tier metrics
    // Note: In real implementation, this would analyze actual positions by tier
    // MOCK MODE DISABLED - Always use actual on-chain data from ScanUTXOSet
    LogPrint(BCLog::DIGIDOLLAR, "UpdateTierMetrics: Using actual on-chain data (tiers.size()=%zu, totalDDSupply=%lld)\n",
             s_currentMetrics.tiers.size(), static_cast<long long>(s_currentMetrics.totalDDSupply));

    // Mock mode completely disabled per user requirement: "do not fall back to mock data!!!"
    // When scanner finds 0 vaults, stats should correctly show 0, not mock data
    /*
    if (s_currentMetrics.tiers.size() >= 6 && s_currentMetrics.totalDDSupply == 0) {
        LogPrint(BCLog::DIGIDOLLAR, "UpdateTierMetrics: MOCK MODE TRIGGERED!\n");
        // Mock mode - populate with test data
        // Tier 0: 30-day (mock data) - 150% ratio
        s_currentMetrics.tiers[0].ddMinted = 3600; // $36.00
        s_currentMetrics.tiers[0].dgbLocked = 10800000000; // 108 DGB worth $54
        s_currentMetrics.tiers[0].positions = 2;
        s_currentMetrics.tiers[0].healthRatio = HealthUtils::CalculateHealthRatio(
            s_currentMetrics.tiers[0].ddMinted,
            s_currentMetrics.tiers[0].dgbLocked,
            currentPriceMicroUSD
        );

        // Tier 1: 90-day (mock data) - 125% ratio
        s_currentMetrics.tiers[1].ddMinted = 5000; // $50.00
        s_currentMetrics.tiers[1].dgbLocked = 12500000000; // 125 DGB worth $62.50
        s_currentMetrics.tiers[1].positions = 2;
        s_currentMetrics.tiers[1].healthRatio = HealthUtils::CalculateHealthRatio(
            s_currentMetrics.tiers[1].ddMinted,
            s_currentMetrics.tiers[1].dgbLocked,
            currentPriceMicroUSD
        );

        // Tier 2: 180-day (mock data) - 120% ratio
        s_currentMetrics.tiers[2].ddMinted = 4166; // $41.66
        s_currentMetrics.tiers[2].dgbLocked = 10000000000; // 100 DGB worth $50
        s_currentMetrics.tiers[2].positions = 2;
        s_currentMetrics.tiers[2].healthRatio = HealthUtils::CalculateHealthRatio(
            s_currentMetrics.tiers[2].ddMinted,
            s_currentMetrics.tiers[2].dgbLocked,
            currentPriceMicroUSD
        );

        // Tier 3: 365-day (mock data) - 250% ratio
        s_currentMetrics.tiers[3].ddMinted = 2000; // $20.00
        s_currentMetrics.tiers[3].dgbLocked = 10000000000; // 100 DGB worth $50
        s_currentMetrics.tiers[3].positions = 2;
        s_currentMetrics.tiers[3].healthRatio = HealthUtils::CalculateHealthRatio(
            s_currentMetrics.tiers[3].ddMinted,
            s_currentMetrics.tiers[3].dgbLocked,
            currentPriceMicroUSD
        );

        // Tier 4: 365-day (mock data) - 300% ratio
        s_currentMetrics.tiers[4].ddMinted = 2500; // $25.00
        s_currentMetrics.tiers[4].dgbLocked = 10000000000; // 100 DGB worth $50
        s_currentMetrics.tiers[4].positions = 2;
        s_currentMetrics.tiers[4].healthRatio = HealthUtils::CalculateHealthRatio(
            s_currentMetrics.tiers[4].ddMinted,
            s_currentMetrics.tiers[4].dgbLocked,
            currentPriceMicroUSD
        );

        // Tier 5: 1825-day (mock data) - 180% ratio
        s_currentMetrics.tiers[5].ddMinted = 2777; // $27.77
        s_currentMetrics.tiers[5].dgbLocked = 10000000000; // 100 DGB worth $50
        s_currentMetrics.tiers[5].positions = 2;
        s_currentMetrics.tiers[5].healthRatio = HealthUtils::CalculateHealthRatio(
            s_currentMetrics.tiers[5].ddMinted,
            s_currentMetrics.tiers[5].dgbLocked,
            currentPriceMicroUSD
        );

        // Calculate totals from tier data (for mock mode)
        s_currentMetrics.totalDDSupply = 0;
        s_currentMetrics.totalCollateral = 0;
        for (const auto& tier : s_currentMetrics.tiers) {
            s_currentMetrics.totalDDSupply += tier.ddMinted;
            s_currentMetrics.totalCollateral += tier.dgbLocked;
        }
    }
    */

    // Update overall system health. Use the same DCA helper as validation so
    // sub-cent DGB prices keep precision and missing oracle data with active
    // DD supply fails closed to 0% health.
    s_currentMetrics.systemHealth = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
        s_currentMetrics.totalCollateral,
        s_currentMetrics.totalDDSupply,
        currentPriceMillicents
    );
    s_currentMetrics.hasCanonicalHealth = true;

    LogPrint(BCLog::DIGIDOLLAR, "Tier metrics updated: %zu tiers analyzed, total DD=%s, total collateral=%s\n",
             s_currentMetrics.tiers.size(),
             FormatMoney(s_currentMetrics.totalDDSupply),
             FormatMoney(s_currentMetrics.totalCollateral));
}

void SystemHealthMonitor::UpdateProtectionStatus()
{
    s_currentMetrics.dcaMultiplier = GetCurrentDCAMultiplier();
    s_currentMetrics.errActive = IsERRActive();
    s_currentMetrics.volatility = GetCurrentVolatility();
    s_currentMetrics.mintingFrozen = IsMintingFrozen();

    LogPrint(BCLog::DIGIDOLLAR, "Protection status updated: DCA=%.2f, ERR=%s, Vol=%.1f%%, Frozen=%s\n",
             s_currentMetrics.dcaMultiplier,
             s_currentMetrics.errActive ? "YES" : "NO",
             s_currentMetrics.volatility,
             s_currentMetrics.mintingFrozen ? "YES" : "NO");
}

void SystemHealthMonitor::UpdateOracleStatus()
{
    s_currentMetrics.activeOracles = GetActiveOracleCount();
    s_currentMetrics.lastOraclePrice = GetLastOraclePrice();
    s_currentMetrics.lastOracleUpdate = GetLastOracleUpdate();

    LogPrint(BCLog::DIGIDOLLAR, "Oracle status updated: %d active, price=%lld micro-USD ($%.6f), last_update=%lld\n",
             s_currentMetrics.activeOracles,
             static_cast<long long>(s_currentMetrics.lastOraclePrice),
             static_cast<double>(s_currentMetrics.lastOraclePrice) / 1000000.0,
             static_cast<long long>(s_currentMetrics.lastOracleUpdate));
}

void SystemHealthMonitor::RecordHealthHistory(int64_t height, int health)
{
    s_healthHistory[height] = health;

    // Limit history size to prevent memory bloat
    const size_t MAX_HISTORY = 100000; // Keep last 100k blocks
    if (s_healthHistory.size() > MAX_HISTORY) {
        // Remove oldest entries
        auto it = s_healthHistory.begin();
        size_t toRemove = s_healthHistory.size() - MAX_HISTORY;
        for (size_t i = 0; i < toRemove && it != s_healthHistory.end(); ++i) {
            it = s_healthHistory.erase(it);
        }
    }
}

int SystemHealthMonitor::CalculateSystemHealth(CAmount ddSupply, CAmount collateral, CAmount price)
{
    if (ddSupply == 0) {
        return 300; // Perfect health if no DD issued
    }

    // Guard against invalid price (same pattern as DCA::CalculateSystemHealth)
    if (price <= 0) {
        return 0; // Cannot calculate without valid price
    }

    __int128 numerator = static_cast<__int128>(collateral) *
                         static_cast<__int128>(price) * 100;
    __int128 denominator = static_cast<__int128>(COIN) *
                           static_cast<__int128>(ddSupply);
    __int128 health = numerator / denominator;

    if (health < 0) return 0;
    if (health > 300) return 300;
    return static_cast<int>(health);
}

double SystemHealthMonitor::GetCurrentVolatility()
{
    using namespace DigiDollar::Volatility;
    VolatilityState state = VolatilityMonitor::GetCurrentState();
    // Return the most severe volatility metric for health assessment
    return std::max({state.hourlyVolatility, state.dailyVolatility, state.weeklyVolatility});
}

double SystemHealthMonitor::GetCurrentDCAMultiplier()
{
    using namespace DigiDollar::DCA;
    // Get actual DCA multiplier from the DCA system
    return DynamicCollateralAdjustment::GetDCAMultiplier(s_currentMetrics.systemHealth);
}

bool SystemHealthMonitor::IsERRActive()
{
    using namespace DigiDollar::ERR;
    // Get actual ERR state from the ERR system
    ERRState state = EmergencyRedemptionRatio::GetCurrentState();
    return state.isActive;
}

bool SystemHealthMonitor::IsMintingFrozen()
{
    using namespace DigiDollar::Volatility;
    // Check both volatility freeze and ERR freeze conditions
    bool volatilityFrozen = VolatilityMonitor::ShouldFreezeMinting() || VolatilityMonitor::ShouldFreezeAll();
    bool errFrozen = IsERRActive(); // ERR may also freeze minting
    return volatilityFrozen || errFrozen;
}

int SystemHealthMonitor::GetActiveOracleCount()
{
    // Get actual oracle count from oracle system
    // TODO: Implement proper oracle system integration
    // For now, return a reasonable default until oracle system is fully implemented
    return 8; // Conservative estimate until proper integration
}

CAmount SystemHealthMonitor::GetLastOraclePrice()
{
    // Get actual price from oracle system in micro-USD.
    // TODO: Implement proper oracle system integration
    // For now, check if we have volatility data which implies oracle data
    using namespace DigiDollar::Volatility;
    if (VolatilityMonitor::IsInitialized()) {
        auto history = VolatilityMonitor::GetPriceHistory();
        if (!history.empty()) {
            return history.back().price;
        }
    }
    return 0;
}

int64_t SystemHealthMonitor::GetLastOracleUpdate()
{
    // Get actual oracle update time from oracle system
    // TODO: Implement proper oracle system integration
    // For now, check if we have volatility data which implies oracle data
    using namespace DigiDollar::Volatility;
    if (VolatilityMonitor::IsInitialized()) {
        auto history = VolatilityMonitor::GetPriceHistory();
        if (!history.empty()) {
            return history.back().height;
        }
    }
    // Fallback to mock recent height
    return 1000000 - 5; // Conservative estimate
}

// Alert checking implementations
bool SystemHealthMonitor::CheckSupplyAlert(const SystemMetrics& metrics)
{
    return metrics.totalDDSupply > AlertThresholds::ALERT_DD_SUPPLY;
}

bool SystemHealthMonitor::CheckHealthAlert(const SystemMetrics& metrics)
{
    return metrics.systemHealth < AlertThresholds::MIN_HEALTH_RATIO;
}

bool SystemHealthMonitor::CheckCollateralAlert(const SystemMetrics& metrics)
{
    // Alert if collateral is insufficient for current supply
    return metrics.systemHealth < AlertThresholds::CRITICAL_HEALTH_RATIO;
}

bool SystemHealthMonitor::CheckOracleAlert(const SystemMetrics& metrics)
{
    // TODO: Fix chainstate access - temporary mock implementation
    // For now, use mock height to prevent compilation errors
    int64_t mockHeight = 1000000; // This should be replaced with proper chainstate access
    bool staleData = (mockHeight - metrics.lastOracleUpdate) > AlertThresholds::STALE_ORACLE_BLOCKS;
    bool lowCount = metrics.activeOracles < AlertThresholds::MIN_ORACLES;
    return staleData || lowCount;
}

bool SystemHealthMonitor::CheckVolatilityAlert(const SystemMetrics& metrics)
{
    return metrics.volatility > AlertThresholds::MAX_VOLATILITY;
}

bool SystemHealthMonitor::CheckPositionAlert(const SystemMetrics& metrics)
{
    int totalPositions = 0;
    for (const auto& tier : metrics.tiers) {
        totalPositions += tier.positions;
    }
    return totalPositions > AlertThresholds::MAX_POSITIONS;
}

// Health utility implementations
namespace HealthUtils {

int GetTierIndex(int lockDays)
{
    for (size_t i = 0; i < TIER_LOCK_DAYS.size(); ++i) {
        if (lockDays <= TIER_LOCK_DAYS[i]) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(TIER_LOCK_DAYS.size() - 1); // Longest tier
}

int GetTierLockDays(int tierIndex)
{
    if (tierIndex >= 0 && tierIndex < static_cast<int>(TIER_LOCK_DAYS.size())) {
        return TIER_LOCK_DAYS[tierIndex];
    }
    return TIER_LOCK_DAYS.back(); // Default to longest
}

int CalculateHealthRatio(CAmount ddAmount, CAmount dgbAmount, CAmount dgbPrice)
{
    if (ddAmount <= 0) {
        return 300; // Perfect if no DD issued
    }

    if (dgbPrice <= 0 || dgbAmount <= 0) {
        return 0; // Cannot calculate without valid price/amount
    }

    // Calculate DGB value in cents using __int128 to prevent overflow.
    // dgbPrice is in cents (100 = $1.00 DGB price)
    // dgbAmount is in satoshis
    // Formula: (satoshis * price_cents) / COIN = value_in_cents
    // Then health = (value_in_cents * 100) / ddAmount
    //
    // Using __int128 is safe here because this is a monitoring/display
    // function, not consensus-critical code. The consensus equivalent
    // (CalculateSystemHealth) uses a divide-first pattern, but __int128
    // is simpler and handles all edge cases without precision loss.
    __int128 dgbValue128 = static_cast<__int128>(dgbAmount) * static_cast<__int128>(dgbPrice);
    dgbValue128 /= COIN;

    // Health = (Collateral Value / DD Value) * 100
    __int128 health128 = (dgbValue128 * 100) / static_cast<__int128>(ddAmount);

    // Clamp to [0, 300]
    if (health128 < 0) return 0;
    if (health128 > 300) return 300;
    return static_cast<int>(health128);
}

std::string FormatHealthStatus(int health)
{
    if (health >= AlertThresholds::MIN_HEALTH_RATIO) {
        return "Healthy";
    } else if (health >= AlertThresholds::CRITICAL_HEALTH_RATIO) {
        return "Warning";
    } else {
        return "Critical";
    }
}

std::string GetRecommendedAction(int health)
{
    if (health >= AlertThresholds::MIN_HEALTH_RATIO) {
        return "Monitor";
    } else if (health >= AlertThresholds::CRITICAL_HEALTH_RATIO) {
        return "Add Collateral";
    } else {
        return "Emergency Action Required";
    }
}

} // namespace HealthUtils

} // namespace DigiDollar
