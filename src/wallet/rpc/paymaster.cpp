// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Wallet-scoped Paymaster RPC orchestration and automatic provider service.
 *
 * RPC parameters are untrusted input. This layer coordinates pure protocol
 * validation with PaymasterStore transactions and wallet signing, but does not
 * bypass either authorization manifest or persisted safety accounting. Every
 * retry reuses the exact durable request binding or fails closed.
 */

#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_internal.h>

#include <base58.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <paymaster/client.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/validation.h>
#include <random.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>

#include <univalue.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>

namespace wallet {
namespace paymaster_rpc::internal {

std::string PersistedVersionError(std::string_view record_type,
                                  uint16_t found,
                                  uint16_t expected,
                                  std::string_view invalid_error)
{
    if (found == expected) return std::string{invalid_error};
    return strprintf(
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=%s found=%u expected=%u",
        std::string{record_type}, found, expected);
}

template <typename T>
std::string PersistedReadError(DatabaseReadStatus status,
                               std::string_view record_type,
                               const T& record,
                               std::string_view missing_error,
                               std::string_view invalid_error)
{
    if (status == DatabaseReadStatus::NOT_FOUND) {
        return std::string{missing_error};
    }
    if (status == DatabaseReadStatus::UNSUPPORTED_VERSION) {
        return PersistedVersionError(record_type, record.version,
                                     T::CURRENT_VERSION, invalid_error);
    }
    return std::string{invalid_error};
}

std::string ProviderPoolReadError(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries)
{
    using DigiDollar::Paymaster::ProviderPoolEntry;
    const auto outdated = std::find_if(
        entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
            return entry.version != ProviderPoolEntry::CURRENT_VERSION;
        });
    if (outdated != entries.end()) {
        return PersistedVersionError(
            "ProviderPoolEntry", outdated->version,
            ProviderPoolEntry::CURRENT_VERSION,
            "PAYMASTER_INVALID_PROVIDER_POOL");
    }
    return "PAYMASTER_INVALID_PROVIDER_POOL";
}

bool ReadOptionalProviderPool(
    WalletBatch& batch,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    std::string& error)
{
    const DatabaseReadStatus status{
        batch.ReadPaymasterProviderPoolWithStatus(entries)};
    if (status == DatabaseReadStatus::FOUND) return true;
    if (status == DatabaseReadStatus::NOT_FOUND) {
        entries.clear();
        return true;
    }
    error = ProviderPoolReadError(entries);
    return false;
}

// -------------------------------------------------------------------------
// Automatic provider runtime and readiness
// -------------------------------------------------------------------------
// Internal method names are never exposed as user authorization shortcuts.
// They let the scheduler reuse the same RPC orchestration while the store,
// manifests, wallet locks, and budget checks remain authoritative.

const char* ProviderOperationModeName(
    DigiDollar::Paymaster::ProviderOperationMode mode)
{
    using DigiDollar::Paymaster::ProviderOperationMode;
    switch (mode) {
    case ProviderOperationMode::AUTOMATIC:
        return "automatic";
    case ProviderOperationMode::MANUAL:
        return "manual";
    }
    return "invalid";
}

const char* ProviderServiceStateName(
    DigiDollar::Paymaster::ProviderServiceState state)
{
    using DigiDollar::Paymaster::ProviderServiceState;
    switch (state) {
    case ProviderServiceState::STOPPED:
        return "stopped";
    case ProviderServiceState::WAITING_FOR_UNLOCK:
        return "waiting_for_unlock";
    case ProviderServiceState::WAITING_FOR_READINESS:
        return "waiting_for_readiness";
    case ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL:
        return "waiting_for_maintenance_approval";
    case ProviderServiceState::REPLENISHING_LIQUIDITY:
        return "replenishing_liquidity";
    case ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION:
        return "waiting_for_liquidity_confirmation";
    case ProviderServiceState::ACTIVE:
        return "active";
    case ProviderServiceState::MANUAL:
        return "manual";
    case ProviderServiceState::DRAIN_ONLY:
        return "drain_only";
    case ProviderServiceState::FAULT:
        return "error";
    }
    return "error";
}

std::string StableProviderServiceError(std::string error)
{
    if (error.rfind("PAYMASTER_", 0) != 0) {
        return "PAYMASTER_AUTOMATIC_SERVICE_ERROR";
    }
    const size_t separator = error.find_first_of(": \t\r\n");
    if (separator != std::string::npos) error.resize(separator);
    return error;
}

std::string WalletEndpointURI(const std::string& wallet_name)
{
    static constexpr char HEX[]{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(wallet_name.size());
    for (const unsigned char ch : wallet_name) {
        const bool unreserved =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(HEX[ch >> 4]);
            encoded.push_back(HEX[ch & 0x0f]);
        }
    }
    return "/wallet/" + encoded;
}

// Serialize automatic cycles, manual processing RPCs, and maintenance for one
// provider wallet. Without this guard two callers could lease the same logical
// work or independently decide that the same finite budget is available.

// -------------------------------------------------------------------------
// Liquidity reconciliation and bounded maintenance
// -------------------------------------------------------------------------
// Reconciliation is free and idempotent: it recognizes confirmed wallet-owned
// successors. Replenishment is a distinct spending operation and is permitted
// only by a persisted liquidity policy with finite transaction/hour/day caps.
bool ReconcileProviderMaintenance(CWallet& wallet,
                                  size_t& recovered,
                                  std::string& error);

/** The automatic provider service runs on the validation scheduler. It must
 * never call SyncWithValidationInterfaceQueue (directly or through a wallet
 * or index BlockUntilSyncedToCurrentChain call), because that queue is driven
 * by the same scheduler thread. Check the already-published wallet and index
 * tips instead and retry on the next bounded service cycle. */
bool AutomaticProviderStateIsSynchronized(CWallet& wallet)
{
    const std::optional<int> chain_height = wallet.chain().getHeight();
    if (!chain_height) return false;
    const uint256 chain_tip = wallet.chain().getBlockHash(*chain_height);

    int wallet_height{-1};
    uint256 wallet_tip;
    {
        LOCK(wallet.cs_wallet);
        wallet_height = wallet.GetLastBlockHeight();
        wallet_tip = wallet.GetLastBlockHash();
    }
    if (wallet_height != *chain_height || wallet_tip != chain_tip) return false;

    if (!g_txindex) return false;
    const IndexSummary txindex = g_txindex->GetSummary();
    if (!txindex.synced || txindex.best_block_height != *chain_height ||
        txindex.best_block_hash != chain_tip) {
        return false;
    }

    const std::optional<int> current_height = wallet.chain().getHeight();
    return current_height && *current_height == *chain_height &&
           wallet.chain().getBlockHash(*current_height) == chain_tip;
}

bool ProviderTxIndexIsReady(CWallet& wallet, bool wait_for_sync)
{
    if (!g_txindex) return false;
    if (wait_for_sync) return g_txindex->BlockUntilSyncedToCurrentChain();

    const std::optional<int> chain_height = wallet.chain().getHeight();
    if (!chain_height) return false;
    const uint256 chain_tip = wallet.chain().getBlockHash(*chain_height);
    const IndexSummary txindex = g_txindex->GetSummary();
    if (!txindex.synced || txindex.best_block_height != *chain_height ||
        txindex.best_block_hash != chain_tip) {
        return false;
    }

    const std::optional<int> current_height = wallet.chain().getHeight();
    return current_height && *current_height == *chain_height &&
           wallet.chain().getBlockHash(*current_height) == chain_tip;
}

bool CapacityContinuationMayProceed(const ProviderReadiness& readiness)
{
    if (readiness.ready) return true;

    // A successful Capacity-v5 proof atomically reserves the exact pool slots
    // and active-quote admission needed by its one follow-up request. Global
    // readiness consequently reports those resources as unavailable (and can
    // report the active-quote ceiling) even though no additional capacity is
    // consumed by promoting that same admission. All other readiness failures
    // remain fatal. The read-only admission preflight and atomic quote commit
    // then enforce the exact unexpired admission and re-evaluate the budget
    // after its transition from RESERVED to PROMOTED.
    return !readiness.errors.empty() &&
           std::all_of(readiness.errors.begin(), readiness.errors.end(),
                       [](const std::string& error) {
                           return error == "PAYMASTER_OPERATIONAL_SLOT_MISSING" ||
                                  error == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED";
                       });
}

bool CapacityRequestMayProceed(const ProviderReadiness& readiness,
                               bool have_stored_response)
{
    // Only an exact stored proof can continue without fresh capacity. Its
    // signature, request indexes, expiry and reservation are checked again by
    // ReserveAndBuildPaymasterCapacityProof before anything is returned.
    return readiness.ready ||
           (have_stored_response && CapacityContinuationMayProceed(readiness));
}

bool CheckPaymasterClientReadiness(CWallet& wallet, WalletContext& context,
                                   std::string& error)
{
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "DigiDollar Paymaster support is disabled";
        return false;
    }
    if (!context.args || context.args->GetIntArg("-prune", 0) != 0) {
        error = "PAYMASTER_REQUIRES_PRUNE_0";
        return false;
    }
    if (!context.args || !context.args->GetBoolArg("-txindex", DEFAULT_TXINDEX)) {
        error = "PAYMASTER_REQUIRES_TXINDEX";
        return false;
    }
    if (context.args && Params().GetChainType() == ChainType::MAIN &&
        context.args->GetBoolArg("-capturemessages", false)) {
        error = "PAYMASTER_MESSAGE_CAPTURE_ENABLED";
        return false;
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman ||
        !(node->connman->GetLocalServices() & NODE_P2P_V2)) {
        error = "PAYMASTER_REQUIRES_V2_TRANSPORT";
        return false;
    }
    if (!context.chain || context.chain->isInitialBlockDownload() ||
        !context.chain->isReadyToBroadcast()) {
        error = "PAYMASTER_NODE_NOT_READY";
        return false;
    }

    wallet.BlockUntilSyncedToCurrentChain();
    if (!g_txindex || !g_txindex->BlockUntilSyncedToCurrentChain()) {
        error = "PAYMASTER_REQUIRES_READY_TXINDEX";
        return false;
    }

    int32_t tip_height;
    {
        LOCK(wallet.cs_wallet);
        tip_height = wallet.GetLastBlockHeight();
    }
    const int activation_height =
        Params().GetConsensus().DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);
    if (tip_height + 1 < activation_height) {
        error = "PAYMASTER_DIGIDOLLAR_NOT_ACTIVE";
        return false;
    }
    return true;
}

DigiDollar::Paymaster::DDCents EffectiveClientServiceFeeCap(
    CWallet& wallet,
    DigiDollar::Paymaster::DDCents requested,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    ClientSafetyPolicy policy;
    if (!GetPaymasterClientSafetyPolicy(wallet, policy)) {
        error = "PAYMASTER_CLIENT_SAFETY_POLICY_REQUIRED";
        return DDCents{-1};
    }
    if (!ValidateClientSafetyPolicy(policy, error)) return DDCents{-1};
    if (requested.value < 0) {
        error = "PAYMASTER_CLIENT_FEE_LIMIT_INVALID";
        return DDCents{-1};
    }
    error.clear();
    return DDCents{std::min(requested.value,
                            policy.maximum_service_fee_per_transaction.value)};
}

bool CheckPaymasterPrivacyReadiness(DigiDollar::Paymaster::PrivacyProfile privacy,
                                    WalletContext& context, std::string& error)
{
    using DigiDollar::Paymaster::PrivacyProfile;
    error.clear();
    if (privacy != PrivacyProfile::HIGH) return true;
    if (context.args && context.args->GetBoolArg("-capturemessages", false)) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_CAPTUREMESSAGES_0";
        return false;
    }
    if (fLogIPs) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_LOGIPS_0";
        return false;
    }
    Proxy onion_proxy;
    if (!GetProxy(NET_ONION, onion_proxy)) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_ONION_PROXY";
        return false;
    }
    if (!onion_proxy.randomize_credentials) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_PROXY_ISOLATION";
        return false;
    }
    return true;
}

bool RefreshPoolConfirmationHeights(CWallet& wallet,
                                    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
                                    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger maintenance_ledger;
    std::set<COutPoint> active_maintenance_sources;
    if (batch.ReadPaymasterMaintenanceLedger(maintenance_ledger)) {
        std::string validation_error;
        if (!ValidateProviderMaintenanceLedger(
                maintenance_ledger, validation_error)) {
            error = validation_error.empty()
                ? "PAYMASTER_INVALID_MAINTENANCE_LEDGER"
                : validation_error;
            return false;
        }
        for (const ProviderMaintenanceRecord& record :
             maintenance_ledger.records) {
            if (record.state != ProviderMaintenanceState::PLANNED &&
                record.state != ProviderMaintenanceState::BROADCAST) {
                continue;
            }
            active_maintenance_sources.insert(
                record.source_inputs.begin(), record.source_inputs.end());
            if (record.source_inputs.empty() &&
                record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS) {
                // V1 withdrawal records did not serialize source_inputs. The
                // pool reservation remains an unambiguous binding; exclude
                // outputs of the maintenance transaction itself.
                for (const ProviderPoolEntry& entry : entries) {
                    if (entry.reservation_id == record.operation_id &&
                        (record.transaction_id.IsNull() ||
                         entry.outpoint.hash != record.transaction_id)) {
                        active_maintenance_sources.insert(entry.outpoint);
                    }
                }
            }
        }
    } else if (batch.HasPaymasterMaintenanceLedger()) {
        error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER";
        return false;
    }
    bool changed{false};
    for (auto& entry : entries) {
        const auto tx = wallet.mapWallet.find(entry.outpoint.hash);
        const bool active_maintenance_source =
            active_maintenance_sources.count(entry.outpoint) != 0;
        const bool origin_invalid =
            !entry.origin_commit_key.IsNull() &&
            tx != wallet.mapWallet.end() &&
            (tx->second.isAbandoned() || tx->second.isConflicted());
        if (origin_invalid &&
            IsActiveProviderPoolState(entry.state) &&
            !active_maintenance_source) {
            entry.state = PoolEntryState::INVALIDATED;
            entry.confirmation_height = 0;
            entry.reservation_id.SetNull();
            entry.updated_at = GetTime();
            changed = true;
            continue;
        }
        const bool spent = wallet.IsSpent(entry.outpoint);
        if (IsActiveProviderPoolState(entry.state) && spent &&
            !active_maintenance_source) {
            entry.state = entry.state == PoolEntryState::COMMITTED
                ? PoolEntryState::SPENT
                : PoolEntryState::INVALIDATED;
            entry.reservation_id.SetNull();
            entry.updated_at = GetTime();
            changed = true;
        }
        const auto* confirmed = tx == wallet.mapWallet.end() ? nullptr : tx->second.state<TxStateConfirmed>();
        const int32_t confirmation_height = confirmed ? confirmed->confirmed_block_height : 0;
        if (entry.confirmation_height != confirmation_height) {
            entry.confirmation_height = confirmation_height;
            changed = true;
        }
        if (!spent && entry.state == PoolEntryState::PENDING_SUCCESSOR &&
            confirmation_height > 0) {
            entry.state = PoolEntryState::AVAILABLE;
            entry.reservation_id.SetNull();
            entry.updated_at = GetTime();
            changed = true;
        } else if (!spent && entry.state == PoolEntryState::AVAILABLE &&
                   confirmation_height == 0 &&
                   !entry.origin_commit_key.IsNull()) {
            entry.state = PoolEntryState::PENDING_SUCCESSOR;
            entry.reservation_id = entry.origin_commit_key;
            entry.updated_at = GetTime();
            changed = true;
        }
    }
    if (changed && !batch.WritePaymasterProviderPool(entries)) {
        error = "PAYMASTER_POOL_CONFIRMATION_UPDATE_FAILED";
        return false;
    }
    return true;
}

ProviderReadiness GetProviderReadiness(CWallet& wallet, WalletContext& context,
                                       bool wait_for_sync)
{
    ProviderReadiness result;
    {
        PaymasterStore store{wallet};
        size_t recovered_successors{0};
        std::string reconciliation_error;
        if (!store.ReconcileProviderPoolSuccessors(
                recovered_successors, reconciliation_error)) {
            result.errors.push_back(
                reconciliation_error.empty()
                    ? "PAYMASTER_POOL_RECONCILIATION_FAILED"
                    : reconciliation_error);
        }
        size_t recovered_maintenance{0};
        reconciliation_error.clear();
        if (!ReconcileProviderMaintenance(
                wallet, recovered_maintenance, reconciliation_error)) {
            result.errors.push_back(
                reconciliation_error.empty()
                    ? "PAYMASTER_MAINTENANCE_RECONCILIATION_FAILED"
                    : reconciliation_error);
        }
        size_t expired_quotes{0};
        std::string expiry_error;
        if (!store.ExpireProviderQuotes(GetTime(), expired_quotes, expiry_error)) {
            result.errors.push_back(expiry_error.empty() ? "PAYMASTER_QUOTE_EXPIRY_FAILED" : expiry_error);
        }
        size_t expired_capacity_reservations{0};
        expiry_error.clear();
        if (!store.ExpireProviderCapacityReservations(
                GetTime(), expired_capacity_reservations, expiry_error)) {
            result.errors.push_back(
                expiry_error.empty() ? "PAYMASTER_CAPACITY_EXPIRY_FAILED" : expiry_error);
        }
        size_t expired_recoveries{0};
        expiry_error.clear();
        if (!store.ExpireAlternativeRecoveries(
                GetTime(), expired_recoveries, expiry_error)) {
            result.errors.push_back(
                expiry_error.empty() ? "PAYMASTER_RECOVERY_EXPIRY_FAILED" : expiry_error);
        }
    }
    std::string eligibility_error;
    result.wallet_eligible = CheckPaymasterProviderWallet(wallet, eligibility_error);
    result.have_settings = GetPaymasterProviderSettings(wallet, result.settings);
    result.have_identity = GetPaymasterIdentity(wallet, result.identity);
    result.have_policy = GetPaymasterProviderPolicy(wallet, result.policy);
    result.have_safety_policy = GetPaymasterProviderSafetyPolicy(wallet, result.safety_policy);
    result.have_budget_ledger = GetPaymasterProviderBudgetLedger(wallet, result.budget_ledger);
    result.have_liquidity_policy = GetPaymasterProviderLiquidityPolicy(
        wallet, result.liquidity_policy);
    result.have_maintenance_ledger = GetPaymasterProviderMaintenanceLedger(
        wallet, result.maintenance_ledger);
    result.have_pool = GetPaymasterProviderPoolEntries(wallet, result.pool_entries);
    int32_t tip_height;
    {
        LOCK(wallet.cs_wallet);
        tip_height = wallet.GetLastBlockHeight();
    }
    std::string refresh_error;
    if (!RefreshPoolConfirmationHeights(wallet, result.pool_entries, refresh_error)) {
        result.errors.push_back(refresh_error);
    }
    if (result.have_policy && result.have_pool) {
        result.pool = DigiDollar::Paymaster::EvaluateProviderPoolReadiness(
            result.pool_entries, result.policy, tip_height, 1);
        result.pool_ready = result.pool.ready;
    }
    if (result.have_policy && result.have_safety_policy && result.have_budget_ledger) {
        CScript status_recipient;
        status_recipient << OP_TRUE;
        const uint256 recipient_bucket =
            DigiDollar::Paymaster::GetRecipientBudgetBucket(
                result.budget_ledger, status_recipient);
        const auto model_available = [&](DigiDollar::Paymaster::FundingModel model,
                                         DigiDollar::Paymaster::SponsorshipScope scope) {
            return DigiDollar::Paymaster::EvaluateProviderSafetyStatus(
                       result.budget_ledger, result.safety_policy, model, scope,
                       recipient_bucket,
                       result.policy.maximum_network_fee, GetTime())
                .can_accept_quote;
        };
        if (DigiDollar::Paymaster::PolicyAllowsFundingModel(
                result.policy, DigiDollar::Paymaster::FundingModel::USER_PAID) &&
            model_available(DigiDollar::Paymaster::FundingModel::USER_PAID,
                            DigiDollar::Paymaster::SponsorshipScope::PUBLIC)) {
            result.available_funding_models |=
                DigiDollar::Paymaster::FUNDING_MODEL_USER_PAID;
        }
        if (DigiDollar::Paymaster::PolicyAllowsFundingModel(
                result.policy, DigiDollar::Paymaster::FundingModel::SPONSORED) &&
            model_available(DigiDollar::Paymaster::FundingModel::SPONSORED,
                            result.policy.sponsorship_scope)) {
            result.available_funding_models |=
                DigiDollar::Paymaster::FUNDING_MODEL_SPONSORED;
        }
        if (result.available_funding_models == 0) {
            result.errors.push_back("PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
        }
    }

    if (!context.paymaster || !context.paymaster->Enabled()) result.errors.push_back("PAYMASTER_DISABLED");
    if (!result.have_settings || !result.settings.enabled) result.errors.push_back("PAYMASTER_PROVIDER_NOT_ENABLED");
    if (!result.wallet_eligible) result.errors.push_back(eligibility_error);
    if (!result.have_identity) result.errors.push_back("PAYMASTER_IDENTITY_NOT_FOUND");
    if (!result.have_policy) result.errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
    if (!result.have_safety_policy) result.errors.push_back("PAYMASTER_SAFETY_POLICY_NOT_FOUND");
    if (!result.have_budget_ledger) result.errors.push_back("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND");
    if (result.have_settings && result.have_policy && result.settings.policy_hash != DigiDollar::Paymaster::GetProviderPolicyHash(result.policy)) {
        result.errors.push_back("PAYMASTER_POLICY_BINDING_MISMATCH");
    }
    if (result.have_policy && result.have_liquidity_policy &&
        !DigiDollar::Paymaster::ProviderLiquidityTargetsSatisfyPolicy(
            result.liquidity_policy, result.policy)) {
        // A carrier target may intentionally be reduced to zero by
        // release_slot. Keep that durable configuration valid, but surface a
        // distinct readiness gate instead of pretending that an automatic
        // start can restore capacity that the saved target does not request.
        result.errors.push_back("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE");
    }
    if (wallet.IsLocked()) result.errors.push_back("PAYMASTER_WALLET_LOCKED");
    if (!result.have_pool) {
        result.errors.push_back("PAYMASTER_POOLS_NOT_PREPARED");
    } else if (result.have_policy) {
        result.errors.insert(result.errors.end(), result.pool.errors.begin(), result.pool.errors.end());
    }
    if (!context.args || context.args->GetIntArg("-prune", 0) != 0) {
        result.errors.push_back("PAYMASTER_REQUIRES_PRUNE_0");
    }
    const bool txindex_configured =
        context.args && context.args->GetBoolArg("-txindex", DEFAULT_TXINDEX);
    if (!txindex_configured) {
        result.errors.push_back("PAYMASTER_REQUIRES_TXINDEX");
    } else if (!ProviderTxIndexIsReady(wallet, wait_for_sync)) {
        result.errors.push_back("PAYMASTER_REQUIRES_READY_TXINDEX");
    }
    if (context.args && Params().GetChainType() == ChainType::MAIN &&
        context.args->GetBoolArg("-capturemessages", false)) {
        result.errors.push_back("PAYMASTER_MESSAGE_CAPTURE_ENABLED");
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman ||
        !(node->connman->GetLocalServices() & NODE_P2P_V2)) {
        result.errors.push_back("PAYMASTER_REQUIRES_V2_TRANSPORT");
    }
    if (!context.args || !context.args->IsArgSet("-paymasterendpoint")) {
        result.errors.push_back("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED");
    } else {
        result.endpoint = LookupNumeric(context.args->GetArg("-paymasterendpoint", ""));
        const bool local_regtest = Params().GetChainType() == ChainType::REGTEST &&
                                   result.endpoint.IsLocal();
        if (!result.endpoint.IsValid() ||
            (!result.endpoint.IsRoutable() && !local_regtest)) {
            result.errors.push_back("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE");
        }
    }
    if (!context.chain || context.chain->isInitialBlockDownload() || !context.chain->isReadyToBroadcast()) {
        result.errors.push_back("PAYMASTER_NODE_NOT_READY");
    }
    const int activation_height = Params().GetConsensus().DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);
    if (tip_height + 1 < activation_height) result.errors.push_back("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE");
    result.ready = result.errors.empty();
    return result;
}

// JSON conversion helpers below are presentation only. They must not be used to
// make authorization decisions; callers operate on the typed records above.
UniValue ReadinessErrorsToJSON(const std::vector<std::string>& errors)
{
    UniValue result{UniValue::VARR};
    for (const std::string& error : errors)
        result.push_back(error);
    return result;
}

std::string PoolPurposeName(DigiDollar::Paymaster::PoolPurpose purpose)
{
    return purpose == DigiDollar::Paymaster::PoolPurpose::ADMISSION ? "admission" : "operational";
}

std::string PoolAssetName(DigiDollar::Paymaster::PoolAsset asset)
{
    return asset == DigiDollar::Paymaster::PoolAsset::DGB ? "dgb" : "dd_carrier";
}

std::string PoolEntryStateName(DigiDollar::Paymaster::PoolEntryState state)
{
    using DigiDollar::Paymaster::PoolEntryState;
    switch (state) {
    case PoolEntryState::AVAILABLE: return "available";
    case PoolEntryState::RESERVED: return "reserved";
    case PoolEntryState::PENDING_SUCCESSOR: return "pending_successor";
    case PoolEntryState::SPENT: return "spent";
    case PoolEntryState::COMMITTED: return "committed";
    case PoolEntryState::RELEASED: return "released";
    case PoolEntryState::INVALIDATED: return "invalidated";
    }
    return "unknown";
}

UniValue PoolEntryToJSON(const DigiDollar::Paymaster::ProviderPoolEntry& entry)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("txid", entry.outpoint.hash.GetHex());
    result.pushKV("vout", entry.outpoint.n);
    result.pushKV("purpose", PoolPurposeName(entry.purpose));
    result.pushKV("asset", PoolAssetName(entry.asset));
    result.pushKV("state", PoolEntryStateName(entry.state));
    result.pushKV("dgb_satoshis", entry.dgb_value.value);
    result.pushKV("dd_cents", entry.carrier_value.value);
    result.pushKV("confirmation_height", entry.confirmation_height);
    if (!entry.reservation_id.IsNull()) result.pushKV("reservation_id", entry.reservation_id.GetHex());
    if (!entry.origin_commit_key.IsNull()) {
        result.pushKV("origin_commit_key", entry.origin_commit_key.GetHex());
    }
    result.pushKV("updated_at", entry.updated_at);
    return result;
}

// Announcements describe currently admissible service but grant no spending
// authority. Every later capacity proof, quote, and final transaction is still
// independently authenticated and validated.
bool PublishCurrentProviderAnnouncement(
    CWallet& wallet,
    WalletContext& context,
    const ProviderReadiness& readiness,
    int64_t now,
    DigiDollar::Paymaster::Announcement* published,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!readiness.ready) {
        error = readiness.errors.empty()
            ? "PAYMASTER_PROVIDER_NOT_READY"
            : readiness.errors.front();
        return false;
    }
    if (readiness.policy.sponsorship_scope == SponsorshipScope::RESTRICTED) {
        if (published) *published = Announcement{};
        return true;
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->chainman || !context.paymaster) {
        error = "PAYMASTER_NODE_NOT_READY";
        return false;
    }
    uint256 reference_block;
    {
        LOCK(cs_main);
        const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
        if (!tip) {
            error = "PAYMASTER_NODE_NOT_READY";
            return false;
        }
        reference_block = tip->GetBlockHash();
    }
    Announcement announcement;
    if (!BuildPaymasterAnnouncement(
            wallet, readiness.identity, readiness.policy,
            readiness.pool_entries, readiness.endpoint,
            Params().GenesisBlock().GetHash(), reference_block,
            now, readiness.available_funding_models,
            announcement, error) ||
        !ValidateAnnouncementEnvelope(
            announcement, Params().GenesisBlock().GetHash(), now, error,
            Params().GetChainType() == ChainType::REGTEST) ||
        !ValidateAdmissionProofs(announcement, *node->chainman, error) ||
        !context.paymaster->GetDirectory().AddValidated(announcement, now)) {
        if (error.empty()) error = "PAYMASTER_ANNOUNCEMENT_REJECTED";
        return false;
    }
    if (node->connman) {
        node->connman->ForEachNode([&announcement, node](CNode* peer) {
            if (!peer->fSuccessfullyConnected || peer->fDisconnect) return;
            const CNetMsgMaker maker{peer->GetCommonVersion()};
            node->connman->PushMessage(
                peer, maker.Make(NetMsgType::PMANNOUNCE, announcement));
        });
    }
    if (published) *published = std::move(announcement);
    return true;
}

LiquiditySlotCounts CountLiquiditySlots(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    DigiDollar::Paymaster::PoolPurpose purpose,
    DigiDollar::Paymaster::PoolAsset asset,
    size_t target,
    int64_t minimum_dgb_value)
{
    using namespace DigiDollar::Paymaster;
    LiquiditySlotCounts result;
    for (const ProviderPoolEntry& entry : entries) {
        const bool counts_toward_target =
            entry.state == PoolEntryState::AVAILABLE ||
            entry.state == PoolEntryState::RESERVED ||
            entry.state == PoolEntryState::PENDING_SUCCESSOR;
        if (entry.purpose != purpose || entry.asset != asset ||
            !counts_toward_target) continue;
        // An operational DGB slot that cannot cover the provider's current
        // advertised fee ceiling is not usable capacity. Do not let such a
        // stale or undersized output suppress automatic replenishment.
        if (asset == PoolAsset::DGB &&
            entry.dgb_value.value < minimum_dgb_value) {
            continue;
        }
        ++result.target_counted;
        if (entry.state == PoolEntryState::AVAILABLE &&
            entry.confirmation_height > 0) {
            ++result.ready;
        } else {
            ++result.pending;
        }
    }
    result.missing = target > result.target_counted
        ? target - result.target_counted
        : 0;
    return result;
}

UniValue LiquiditySlotCountsToJSON(const LiquiditySlotCounts& counts,
                                   size_t target)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("target", target);
    result.pushKV("ready", counts.ready);
    result.pushKV("pending", counts.pending);
    result.pushKV("counted_toward_target", counts.target_counted);
    result.pushKV("missing", counts.missing);
    return result;
}

UniValue LiquidityPolicyToJSON(
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("automatic_replenishment", policy.automatic_replenishment);
    result.pushKV("paid_maintenance_approved", policy.paid_maintenance_approved);
    result.pushKV("target_admission_dgb", policy.target_admission_dgb);
    result.pushKV("target_operational_dgb", policy.target_operational_dgb);
    result.pushKV("target_admission_carriers", policy.target_admission_carriers);
    result.pushKV("target_operational_carriers", policy.target_operational_carriers);
    result.pushKV("maximum_maintenance_fee_per_transaction_satoshis",
                  policy.maximum_maintenance_fee_per_transaction.value);
    result.pushKV("maximum_maintenance_fee_per_hour_satoshis",
                  policy.maximum_maintenance_fee_per_hour.value);
    result.pushKV("maximum_maintenance_fee_per_day_satoshis",
                  policy.maximum_maintenance_fee_per_day.value);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

DigiDollar::Paymaster::ProviderLiquidityPolicy SuggestedLiquidityPolicy(
    const ProviderReadiness& readiness,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    ProviderLiquidityPolicy result;
    const auto active_count = [&](PoolPurpose purpose, PoolAsset asset) {
        return static_cast<uint16_t>(std::min<size_t>(
            16, std::count_if(
                    readiness.pool_entries.begin(), readiness.pool_entries.end(),
                    [&](const ProviderPoolEntry& entry) {
                        return entry.purpose == purpose && entry.asset == asset &&
                               IsActiveProviderPoolState(entry.state);
                    })));
    };
    result.target_admission_dgb = std::max<uint16_t>(
        3, active_count(PoolPurpose::ADMISSION, PoolAsset::DGB));
    result.target_operational_dgb = std::max<uint16_t>(
        1, active_count(PoolPurpose::OPERATIONAL, PoolAsset::DGB));
    if (readiness.have_policy && PolicyAllowsFundingModel(
            readiness.policy, FundingModel::USER_PAID)) {
        result.target_admission_carriers = std::max<uint16_t>(
            3, active_count(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER));
        result.target_operational_carriers = std::max<uint16_t>(
            1, active_count(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER));
    }

    const auto include_limits = [&](const FundingSafetyLimits& limits) {
        const auto lower_positive = [](int64_t current, int64_t candidate) {
            if (candidate <= 0) return current;
            return current <= 0 ? candidate : std::min(current, candidate);
        };
        result.maximum_maintenance_fee_per_transaction.value = lower_positive(
            result.maximum_maintenance_fee_per_transaction.value,
            limits.maximum_network_fee_per_transaction.value);
        result.maximum_maintenance_fee_per_hour.value = lower_positive(
            result.maximum_maintenance_fee_per_hour.value,
            limits.maximum_network_fee_per_hour.value);
        result.maximum_maintenance_fee_per_day.value = lower_positive(
            result.maximum_maintenance_fee_per_day.value,
            limits.maximum_network_fee_per_day.value);
    };
    if (readiness.have_policy && readiness.have_safety_policy) {
        if (PolicyAllowsFundingModel(readiness.policy, FundingModel::USER_PAID)) {
            include_limits(GetFundingSafetyLimits(
                readiness.safety_policy, FundingModel::USER_PAID,
                SponsorshipScope::PUBLIC));
        }
        if (PolicyAllowsFundingModel(readiness.policy, FundingModel::SPONSORED)) {
            include_limits(GetFundingSafetyLimits(
                readiness.safety_policy, FundingModel::SPONSORED,
                readiness.policy.sponsorship_scope));
        }
    }
    result.updated_at = now;
    return result;
}

UniValue ProviderLiquidityStatusToJSON(const ProviderReadiness& readiness,
                                       int64_t now)
{
    using namespace DigiDollar::Paymaster;
    const ProviderLiquidityPolicy policy = readiness.have_liquidity_policy
        ? readiness.liquidity_policy
        : SuggestedLiquidityPolicy(readiness, now);
    const bool targets_satisfy_provider_policy =
        !readiness.have_policy ||
        ProviderLiquidityTargetsSatisfyPolicy(policy, readiness.policy);
    const auto admission_dgb = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DGB,
        policy.target_admission_dgb);
    const int64_t operational_dgb_minimum = readiness.have_policy
        ? readiness.policy.maximum_network_fee.value
        : std::numeric_limits<int64_t>::max();
    const auto operational_dgb = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
        policy.target_operational_dgb, operational_dgb_minimum);
    const auto admission_carriers = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::ADMISSION,
        PoolAsset::DD_CARRIER, policy.target_admission_carriers);
    const auto operational_carriers = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::OPERATIONAL,
        PoolAsset::DD_CARRIER, policy.target_operational_carriers);
    const size_t missing = admission_dgb.missing + operational_dgb.missing +
                           admission_carriers.missing +
                           operational_carriers.missing;
    const size_t pending = admission_dgb.pending + operational_dgb.pending +
                           admission_carriers.pending +
                           operational_carriers.pending;
    std::string state{"ready"};
    if (!targets_satisfy_provider_policy) {
        state = "waiting_for_target_configuration";
    } else if (!readiness.have_liquidity_policy ||
        (missing > 0 && !policy.paid_maintenance_approved)) {
        state = "waiting_for_maintenance_approval";
    } else if (pending > 0) {
        state = "waiting_for_liquidity_confirmation";
    } else if (missing > 0) {
        state = "replenishing_liquidity";
    }

    const auto checked_add = [](int64_t& total, int64_t value) {
        if (value < 0 ||
            total > std::numeric_limits<int64_t>::max() - value) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_LIQUIDITY_STATUS_OVERFLOW");
        }
        total += value;
    };
    int64_t carrier_base{0};
    int64_t carrier_excess{0};
    for (const ProviderPoolEntry& entry : readiness.pool_entries) {
        if (entry.asset != PoolAsset::DD_CARRIER ||
            entry.state != PoolEntryState::AVAILABLE ||
            entry.confirmation_height <= 0) {
            continue;
        }
        checked_add(
            carrier_base,
            std::min<int64_t>(100, entry.carrier_value.value));
        checked_add(
            carrier_excess,
            std::max<int64_t>(0, entry.carrier_value.value - 100));
    }

    int64_t reserved_fee{0};
    int64_t spent_hour{0};
    int64_t spent_day{0};
    if (readiness.have_maintenance_ledger) {
        for (const ProviderMaintenanceRecord& record :
             readiness.maintenance_ledger.records) {
            if (record.IsPreparation()) continue;
            if (record.state == ProviderMaintenanceState::PLANNED) {
                checked_add(reserved_fee, record.maximum_fee.value);
            } else if (record.state ==
                       ProviderMaintenanceState::BROADCAST) {
                // A live transaction remains economic exposure until it is
                // confirmed or fails, irrespective of its wall-clock age.
                checked_add(reserved_fee, record.actual_fee.value);
            } else if (record.state ==
                       ProviderMaintenanceState::CONFIRMED) {
                if (record.updated_at >= now - 60 * 60) {
                    checked_add(spent_hour, record.actual_fee.value);
                }
                if (record.updated_at >= now - 24 * 60 * 60) {
                    checked_add(spent_day, record.actual_fee.value);
                }
            }
        }
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("policy_configured", readiness.have_liquidity_policy);
    result.pushKV("targets_satisfy_provider_policy",
                  targets_satisfy_provider_policy);
    result.pushKV("maintenance_state", state);
    result.pushKV("policy", LiquidityPolicyToJSON(policy));
    result.pushKV("admission_dgb", LiquiditySlotCountsToJSON(
        admission_dgb, policy.target_admission_dgb));
    result.pushKV("operational_dgb", LiquiditySlotCountsToJSON(
        operational_dgb, policy.target_operational_dgb));
    result.pushKV("admission_carriers", LiquiditySlotCountsToJSON(
        admission_carriers, policy.target_admission_carriers));
    result.pushKV("operational_carriers", LiquiditySlotCountsToJSON(
        operational_carriers, policy.target_operational_carriers));
    result.pushKV("maintenance_fee_reserved_satoshis", reserved_fee);
    result.pushKV("maintenance_fee_spent_last_hour_satoshis", spent_hour);
    result.pushKV("maintenance_fee_spent_last_day_satoshis", spent_day);
    result.pushKV("carrier_base_cents", carrier_base);
    result.pushKV("carrier_withdrawable_excess_cents", carrier_excess);
    result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
    return result;
}

uint256 MaintenancePlanId(
    DigiDollar::Paymaster::ProviderMaintenanceKind kind,
    const std::vector<DigiDollar::Paymaster::ProviderMaintenanceOutput>& outputs,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Maintenance v1");
    hasher << static_cast<uint8_t>(kind) << outputs
           << policy.version
           << policy.automatic_replenishment
           << policy.paid_maintenance_approved
           << policy.target_admission_dgb
           << policy.target_operational_dgb
           << policy.target_admission_carriers
           << policy.target_operational_carriers
           << policy.maximum_maintenance_fee_per_transaction
           << policy.maximum_maintenance_fee_per_hour
           << policy.maximum_maintenance_fee_per_day
           << policy.updated_at;
    return hasher.GetSHA256();
}

uint256 CarrierWithdrawalPlanId(
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Carrier Withdrawal v1");
    hasher << static_cast<uint8_t>(plan.mode)
           << plan.operation_id << plan.source_carriers
           << plan.replacement_carriers
           << plan.excess_script_pub_key << plan.excess_amount
           << plan.estimated_fee
           << plan.target_operational_carriers_after_release
           << plan.liquidity_policy_updated_at
           << plan.created_at << plan.expires_at;
    return hasher.GetSHA256();
}

std::optional<uint32_t> FindMaintenanceOutputIndex(
    const CTransaction& transaction,
    const DigiDollar::Paymaster::ProviderMaintenanceOutput& output)
{
    using namespace DigiDollar::Paymaster;
    std::optional<uint32_t> result;
    for (uint32_t index{0}; index < transaction.vout.size(); ++index) {
        const CTxOut& txout = transaction.vout[index];
        const CAmount expected_value = output.asset == PoolAsset::DGB
            ? output.dgb_value.value : 0;
        if (txout.nValue != expected_value ||
            txout.scriptPubKey != output.script_pub_key) {
            continue;
        }
        if (output.asset == PoolAsset::DD_CARRIER) {
            CAmount dd_amount{0};
            if (!DigiDollar::ExtractDDAmountFromTransaction(
                    transaction,
                    COutPoint{transaction.GetHash(), index}, dd_amount) ||
                dd_amount != output.carrier_value.value) {
                continue;
            }
        }
        // Exact maintenance scripts are fresh and may occur only once. A
        // duplicate is ambiguous and must never be registered as a pool slot.
        if (result) return std::nullopt;
        result = index;
    }
    return result;
}

std::optional<uint32_t> FindExactDigiDollarOutputIndex(
    const CTransaction& transaction,
    const CScript& script_pub_key,
    DigiDollar::Paymaster::DDCents amount)
{
    std::optional<uint32_t> result;
    for (uint32_t index{0}; index < transaction.vout.size(); ++index) {
        const CTxOut& txout = transaction.vout[index];
        if (txout.nValue != 0 || txout.scriptPubKey != script_pub_key) {
            continue;
        }
        CAmount dd_amount{0};
        if (!DigiDollar::ExtractDDAmountFromTransaction(
                transaction,
                COutPoint{transaction.GetHash(), index}, dd_amount) ||
            dd_amount != amount.value) {
            continue;
        }
        if (result) return std::nullopt;
        result = index;
    }
    return result;
}

bool MakeProviderPoolRoomForMaintenance(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    size_t additional,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    while (entries.size() + additional > 256) {
        const auto removable = std::min_element(
            entries.begin(), entries.end(),
            [](const ProviderPoolEntry& lhs, const ProviderPoolEntry& rhs) {
                const bool lhs_active = IsActiveProviderPoolState(lhs.state);
                const bool rhs_active = IsActiveProviderPoolState(rhs.state);
                if (lhs_active != rhs_active) return !lhs_active;
                return lhs.updated_at < rhs.updated_at;
            });
        if (removable == entries.end() ||
            IsActiveProviderPoolState(removable->state)) {
            error = "PAYMASTER_POOL_TOO_LARGE";
            return false;
        }
        entries.erase(removable);
    }
    return true;
}

bool SaveCarrierWithdrawalPreview(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    const DatabaseReadStatus ledger_status{
        batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
    if (ledger_status != DatabaseReadStatus::FOUND &&
        ledger_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            ledger_status, "ProviderMaintenanceLedger", ledger,
            "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
        return false;
    }
    const bool have_ledger{ledger_status == DatabaseReadStatus::FOUND};
    if (!ReadOptionalProviderPool(batch, pool, error)) return false;

    bool ledger_changed{false};
    bool pool_changed{false};
    ProviderCarrierWithdrawalPlan previous;
    const DatabaseReadStatus previous_status{
        batch.ReadPaymasterCarrierWithdrawalPlanWithStatus(previous)};
    if (previous_status == DatabaseReadStatus::FOUND &&
        previous.operation_id != plan.operation_id && have_ledger) {
        const auto record = std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [&](const ProviderMaintenanceRecord& candidate) {
                return candidate.operation_id == previous.operation_id;
            });
        if (record != ledger.records.end() &&
            record->state == ProviderMaintenanceState::PLANNED) {
            if (!ReleaseProviderMaintenanceBudget(
                    ledger, previous.operation_id, GetTime(), error)) {
                return false;
            }
            ledger_changed = true;
            for (ProviderPoolEntry& entry : pool) {
                if (entry.state == PoolEntryState::RESERVED &&
                    entry.reservation_id == previous.operation_id) {
                    entry.state = PoolEntryState::AVAILABLE;
                    entry.reservation_id.SetNull();
                    entry.updated_at = GetTime();
                    pool_changed = true;
                }
            }
        }
    } else if (previous_status != DatabaseReadStatus::FOUND &&
               previous_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            previous_status, "ProviderCarrierWithdrawalPlan", previous,
            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_NOT_FOUND",
            "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN");
        return false;
    }

    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if ((ledger_changed && !batch.WritePaymasterMaintenanceLedger(ledger)) ||
        (pool_changed && !batch.WritePaymasterProviderPool(pool)) ||
        !batch.WritePaymasterCarrierWithdrawalPlan(plan)) {
        batch.TxnAbort();
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool ReconcileProviderMaintenance(CWallet& wallet,
                                  size_t& recovered,
                                  std::string& error)
{
    using namespace DigiDollar::Paymaster;
    recovered = 0;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger ledger;
    const DatabaseReadStatus ledger_status{
        batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
    if (ledger_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (ledger_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            ledger_status, "ProviderMaintenanceLedger", ledger,
            "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
        return false;
    }
    std::vector<ProviderPoolEntry> pool;
    if (!ReadOptionalProviderPool(batch, pool, error)) return false;
    bool ledger_changed{false};
    bool pool_changed{false};
    const int64_t now = GetTime();
    ProviderCarrierWithdrawalPlan withdrawal_plan;
    const DatabaseReadStatus withdrawal_status{
        batch.ReadPaymasterCarrierWithdrawalPlanWithStatus(withdrawal_plan)};
    if (withdrawal_status != DatabaseReadStatus::FOUND &&
        withdrawal_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            withdrawal_status, "ProviderCarrierWithdrawalPlan",
            withdrawal_plan,
            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_NOT_FOUND",
            "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN");
        return false;
    }
    const bool have_withdrawal_plan{
        withdrawal_status == DatabaseReadStatus::FOUND};
    for (ProviderMaintenanceRecord& record : ledger.records) {
        if (record.state != ProviderMaintenanceState::PLANNED &&
            record.state != ProviderMaintenanceState::BROADCAST &&
            record.state != ProviderMaintenanceState::CONFIRMED &&
            !(record.IsPreparation() && record.state == ProviderMaintenanceState::FAILED &&
              !record.transaction_id.IsNull())) {
            continue;
        }
        // A conflict/abandon observation is reversible. Keep following the
        // exact saved setup transaction; never construct a replacement here.
        const auto matches = [&](const CWalletTx& wallet_tx) {
            if (!wallet_tx.tx) return false;
            for (const COutPoint& source : record.source_inputs) {
                if (std::none_of(
                        wallet_tx.tx->vin.begin(), wallet_tx.tx->vin.end(),
                        [&](const CTxIn& input) {
                            return input.prevout == source;
                        })) {
                    return false;
                }
            }
            for (const ProviderMaintenanceOutput& output : record.outputs) {
                if (!FindMaintenanceOutputIndex(*wallet_tx.tx, output)) return false;
            }
            if (record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS &&
                !FindExactDigiDollarOutputIndex(
                    *wallet_tx.tx,
                    record.withdrawal_excess_script_pub_key,
                    record.withdrawal_excess_amount)) {
                return false;
            }
            return true;
        };
        auto transaction = wallet.mapWallet.end();
        if (!record.transaction_id.IsNull()) {
            transaction = wallet.mapWallet.find(record.transaction_id);
            if (transaction != wallet.mapWallet.end() && !matches(transaction->second)) {
                error = "PAYMASTER_MAINTENANCE_TRANSACTION_CONFLICT";
                return false;
            }
        } else {
            for (auto candidate = wallet.mapWallet.begin(); candidate != wallet.mapWallet.end(); ++candidate) {
                if (!matches(candidate->second)) continue;
                if (transaction != wallet.mapWallet.end()) {
                    error = "PAYMASTER_MAINTENANCE_TRANSACTION_AMBIGUOUS";
                    return false;
                }
                transaction = candidate;
            }
        }
        if (transaction == wallet.mapWallet.end()) {
            if (record.state == ProviderMaintenanceState::PLANNED &&
                record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS &&
                record.transaction_id.IsNull()) {
                int64_t release_after =
                    record.updated_at >
                            std::numeric_limits<int64_t>::max() - 15 * 60
                        ? std::numeric_limits<int64_t>::max()
                        : record.updated_at + 15 * 60;
                if (have_withdrawal_plan &&
                    withdrawal_plan.operation_id == record.operation_id) {
                    release_after = withdrawal_plan.expires_at;
                }
                if (now > release_after) {
                    std::vector<ProviderPoolEntry*> sources;
                    for (ProviderPoolEntry& entry : pool) {
                        const bool explicitly_bound = std::find(
                            record.source_inputs.begin(),
                            record.source_inputs.end(), entry.outpoint) !=
                            record.source_inputs.end();
                        if (explicitly_bound) {
                            sources.push_back(&entry);
                        }
                    }
                    const bool can_release = !sources.empty() &&
                        std::all_of(
                            sources.begin(), sources.end(),
                            [&](const ProviderPoolEntry* source) {
                                return source != nullptr &&
                                       source->reservation_id ==
                                           record.operation_id &&
                                       (source->state ==
                                            PoolEntryState::RESERVED ||
                                        source->state ==
                                            PoolEntryState::COMMITTED) &&
                                       !wallet.IsSpent(source->outpoint);
                            }) &&
                        sources.size() == record.source_inputs.size();
                    if (can_release) {
                        if (!ReleaseProviderMaintenanceBudget(
                                ledger, record.operation_id, now, error)) {
                            return false;
                        }
                        ledger_changed = true;
                        for (ProviderPoolEntry* source : sources) {
                            source->state = PoolEntryState::AVAILABLE;
                            source->reservation_id.SetNull();
                            source->updated_at = now;
                            pool_changed = true;
                        }
                    }
                }
            }
            continue;
        }
        const uint256 txid = transaction->first;
        if (record.IsPreparation()) {
            const auto fee = GetProviderFinanceTransactionFee(wallet, transaction->second.tx);
            if (!fee || fee->value > record.maximum_fee.value) {
                error = "PAYMASTER_POOL_FEE_LIMIT";
                return false;
            }
            if (record.actual_fee != *fee) {
                record.actual_fee = *fee;
                ledger_changed = true;
            }
        }
        if (record.transaction_id.IsNull()) {
            record.transaction_id = txid;
            // Conservatively charge the entire reservation after recovering a
            // crash window; this can only reduce later maintenance capacity.
            if (!record.IsPreparation()) record.actual_fee = record.maximum_fee;
            record.state = ProviderMaintenanceState::BROADCAST;
            record.updated_at = now;
            ledger_changed = true;
            ++recovered;
        }

        if (transaction->second.isAbandoned() ||
            transaction->second.isConflicted()) {
            if (record.state != ProviderMaintenanceState::FAILED) {
                record.state = ProviderMaintenanceState::FAILED;
                record.updated_at = now;
                ledger_changed = true;
            }
            for (ProviderPoolEntry& entry : pool) {
                if (entry.outpoint.hash != txid ||
                    entry.origin_commit_key != record.operation_id ||
                    !IsActiveProviderPoolState(entry.state)) {
                    continue;
                }
                entry.state = PoolEntryState::INVALIDATED;
                entry.confirmation_height = 0;
                entry.reservation_id.SetNull();
                entry.updated_at = now;
                pool_changed = true;
            }
            for (const COutPoint& source_input : record.source_inputs) {
                const auto source = std::find_if(
                    pool.begin(), pool.end(),
                    [&](const ProviderPoolEntry& entry) {
                        return entry.outpoint == source_input;
                    });
                if (source == pool.end()) {
                    continue;
                }
                // A source may already be SPENT after a previously confirmed
                // maintenance transaction, or INVALIDATED by an older generic
                // refresh. Once the exact maintenance transaction is known to
                // have failed, chainstate is authoritative: restore an
                // unspent source, otherwise keep it unavailable. Never steal a
                // reservation that is already bound to another operation.
                if (!source->reservation_id.IsNull() &&
                    source->reservation_id != record.operation_id) {
                    continue;
                }
                const bool recoverable_state =
                    source->state == PoolEntryState::AVAILABLE ||
                    source->state == PoolEntryState::RESERVED ||
                    source->state == PoolEntryState::COMMITTED ||
                    source->state == PoolEntryState::SPENT ||
                    source->state == PoolEntryState::INVALIDATED;
                if (!recoverable_state) {
                    error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
                    return false;
                }
                const PoolEntryState restored_state = wallet.IsSpent(source_input)
                    ? PoolEntryState::INVALIDATED
                    : PoolEntryState::AVAILABLE;
                if (source->state != restored_state ||
                    !source->reservation_id.IsNull()) {
                    source->state = restored_state;
                    source->reservation_id.SetNull();
                    source->updated_at = now;
                    pool_changed = true;
                }
            }
            continue;
        }

        const auto* confirmed = transaction->second.state<TxStateConfirmed>();
        if (record.kind == ProviderMaintenanceKind::PREPARE_CARRIER &&
            (confirmed || transaction->second.InMempool())) {
            CAmount carrier_total{0};
            for (const auto& output : record.outputs) carrier_total += output.carrier_value.value;
            auto* dd_wallet = wallet.GetDDWallet();
            if (!dd_wallet || !dd_wallet->RecordPaymasterSendHistory(
                    txid, record.outputs.front().script_pub_key, carrier_total, error)) {
                if (error.empty()) error = "PAYMASTER_DD_WALLET_UNAVAILABLE";
                return false;
            }
        }
        const PoolEntryState expected_source_state = confirmed
            ? PoolEntryState::SPENT
            : PoolEntryState::COMMITTED;
        for (const COutPoint& source_input : record.source_inputs) {
            const auto source = std::find_if(
                pool.begin(), pool.end(),
                [&](const ProviderPoolEntry& entry) {
                    return entry.outpoint == source_input;
                });
            if (source == pool.end()) {
                error = "PAYMASTER_MAINTENANCE_SOURCE_MISSING";
                return false;
            }
            if (!source->reservation_id.IsNull() &&
                source->reservation_id != record.operation_id) {
                error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
                return false;
            }
            const bool recoverable_state =
                source->state == PoolEntryState::AVAILABLE ||
                source->state == PoolEntryState::RESERVED ||
                source->state == PoolEntryState::COMMITTED ||
                source->state == PoolEntryState::SPENT ||
                source->state == PoolEntryState::INVALIDATED;
            if (!recoverable_state) {
                error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
                return false;
            }
            const uint256 expected_reservation = confirmed
                ? uint256{}
                : record.operation_id;
            if (source->state != expected_source_state ||
                source->reservation_id != expected_reservation) {
                source->state = expected_source_state;
                source->reservation_id = expected_reservation;
                source->updated_at = now;
                pool_changed = true;
            }
        }

        const ProviderMaintenanceState expected_state = confirmed
            ? ProviderMaintenanceState::CONFIRMED
            : ProviderMaintenanceState::BROADCAST;
        if (record.state != expected_state) {
            record.state = expected_state;
            record.updated_at = now;
            ledger_changed = true;
        }
        for (const ProviderMaintenanceOutput& output : record.outputs) {
            const auto output_index = FindMaintenanceOutputIndex(
                *transaction->second.tx, output);
            if (!output_index) {
                error = "PAYMASTER_MAINTENANCE_OUTPUT_MISSING";
                return false;
            }
            ProviderPoolEntry expected;
            expected.outpoint = COutPoint{txid, *output_index};
            expected.purpose = output.purpose;
            expected.asset = output.asset;
            expected.state = confirmed ? PoolEntryState::AVAILABLE
                                       : PoolEntryState::PENDING_SUCCESSOR;
            expected.script_pub_key = output.script_pub_key;
            expected.dgb_value = output.dgb_value;
            expected.carrier_value = output.carrier_value;
            expected.confirmation_height = confirmed
                ? confirmed->confirmed_block_height : 0;
            expected.reservation_id = confirmed ? uint256{}
                                                : record.operation_id;
            expected.origin_commit_key = record.operation_id;
            expected.updated_at = record.created_at;
            const auto existing = std::find_if(
                pool.begin(), pool.end(), [&](const ProviderPoolEntry& entry) {
                    return entry.outpoint == expected.outpoint;
                });
            if (existing == pool.end()) {
                if (!MakeProviderPoolRoomForMaintenance(pool, 1, error)) {
                    return false;
                }
                pool.push_back(std::move(expected));
                pool_changed = true;
            } else if (existing->purpose != expected.purpose ||
                       existing->asset != expected.asset ||
                       existing->script_pub_key != expected.script_pub_key ||
                       existing->dgb_value != expected.dgb_value ||
                       existing->carrier_value != expected.carrier_value ||
                       (!existing->origin_commit_key.IsNull() &&
                        existing->origin_commit_key != record.operation_id)) {
                error = "PAYMASTER_MAINTENANCE_POOL_CONFLICT";
                return false;
            } else {
                if (existing->origin_commit_key.IsNull()) {
                    existing->origin_commit_key = record.operation_id;
                    pool_changed = true;
                }
                if (existing->confirmation_height !=
                    expected.confirmation_height) {
                    existing->confirmation_height =
                        expected.confirmation_height;
                    pool_changed = true;
                }
                if (record.IsPreparation() && existing->state == PoolEntryState::INVALIDATED &&
                    !wallet.IsSpent(existing->outpoint)) {
                    existing->state = expected.state;
                    existing->reservation_id = expected.reservation_id;
                    existing->updated_at = std::max(existing->updated_at, now);
                    pool_changed = true;
                } else if (confirmed &&
                    existing->state == PoolEntryState::PENDING_SUCCESSOR) {
                    existing->state = PoolEntryState::AVAILABLE;
                    existing->reservation_id.SetNull();
                    existing->updated_at = now;
                    pool_changed = true;
                } else if (!confirmed &&
                           existing->state == PoolEntryState::AVAILABLE) {
                    existing->state = PoolEntryState::PENDING_SUCCESSOR;
                    existing->reservation_id = record.operation_id;
                    existing->updated_at = now;
                    pool_changed = true;
                }
            }
        }
    }
    std::string validation_error;
    if (!ValidateProviderMaintenanceLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (!ledger_changed && !pool_changed) return true;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if ((ledger_changed && !batch.WritePaymasterMaintenanceLedger(ledger)) ||
        (pool_changed && !batch.WritePaymasterProviderPool(pool))) {
        batch.TxnAbort();
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool ReconcileProviderFinances(CWallet& wallet,
                               size_t& changed_events,
                               std::string& error)
{
    using namespace DigiDollar::Paymaster;
    changed_events = 0;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    const DatabaseReadStatus identity_status =
        batch.ReadPaymasterIdentityWithStatus(identity);
    if (identity_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (identity_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            identity_status, "ProviderIdentityRecord", identity,
            "PAYMASTER_IDENTITY_NOT_FOUND", "PAYMASTER_INVALID_IDENTITY");
        return false;
    }

    const uint256 genesis_hash = Params().GenesisBlock().GetHash();
    ProviderFinanceLedger finance;
    bool ledger_changed{false};
    const DatabaseReadStatus finance_status =
        batch.ReadPaymasterFinanceLedgerWithStatus(finance);
    if (finance_status != DatabaseReadStatus::FOUND) {
        if (finance_status != DatabaseReadStatus::NOT_FOUND) {
            error = PersistedReadError(
                finance_status, "ProviderFinanceLedger", finance,
                "PAYMASTER_FINANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_FINANCE_LEDGER");
            return false;
        }
        // Pre-feature wallets may have durable records that no longer carry
        // enough context for exact reconstruction. Never estimate them.
        finance.genesis_hash = genesis_hash;
        finance.provider_id = identity.provider_id;
        finance.history_complete_from = GetTime();
        finance.earlier_history_partial = true;
        finance.updated_at = finance.history_complete_from;
        if (!RebuildProviderFinanceDailyTotals(finance, error)) return false;
        ledger_changed = true;
    }
    if (finance.genesis_hash != genesis_hash ||
        finance.provider_id != identity.provider_id) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }

    const int64_t now = GetTime();
    const auto transaction_state = [&](const uint256& txid,
                                       int64_t created_at,
                                       ProviderFinanceEventState& state,
                                       int64_t& confirmed_at) {
        state = ProviderFinanceEventState::PENDING;
        confirmed_at = 0;
        const auto transaction = wallet.mapWallet.find(txid);
        if (transaction == wallet.mapWallet.end()) return;
        if (transaction->second.isAbandoned() ||
            transaction->second.isConflicted()) {
            state = ProviderFinanceEventState::INVALIDATED;
            return;
        }
        if (wallet.GetTxDepthInMainChain(transaction->second) > 0) {
            state = ProviderFinanceEventState::CONFIRMED;
            int64_t block_time{0};
            if (const auto* confirmed =
                    transaction->second.state<TxStateConfirmed>()) {
                wallet.chain().findBlock(
                    confirmed->confirmed_block_hash,
                    interfaces::FoundBlock().time(block_time));
            }
            // UTC totals follow the active-chain confirmation day. The wallet
            // observation time remains a conservative fallback for old or
            // temporarily unavailable block metadata.
            confirmed_at = std::max(
                created_at, block_time > 0
                    ? block_time : transaction->second.GetTxTime());
        }
    };
    const auto apply_event = [&](ProviderFinanceEvent event) {
        auto existing = std::find_if(
            finance.events.begin(), finance.events.end(),
            [&](const ProviderFinanceEvent& candidate) {
                return candidate.event_id == event.event_id;
            });
        if (existing != finance.events.end()) {
            const bool same_economic_event =
                existing->genesis_hash == event.genesis_hash &&
                existing->provider_id == event.provider_id &&
                existing->kind == event.kind &&
                existing->funding_model == event.funding_model &&
                existing->sponsorship_scope == event.sponsorship_scope &&
                existing->transaction_id == event.transaction_id &&
                existing->dd_income == event.dd_income &&
                existing->dgb_cost == event.dgb_cost &&
                existing->created_at == event.created_at;
            if (!same_economic_event) {
                error = "PAYMASTER_FINANCE_EVENT_CONFLICT";
                return false;
            }
        }
        if (existing != finance.events.end() &&
            existing->state == event.state &&
            existing->confirmed_at == event.confirmed_at) {
            event.updated_at = existing->updated_at;
        } else {
            // Regtest mock time and imported historical blocks can place the
            // active-chain confirmation after the current wall-clock value.
            // Keep the event chronology valid without replacing the actual
            // block confirmation time with a local observation time.
            event.updated_at = std::max(
                {event.created_at, event.confirmed_at, now});
            ledger_changed = true;
            ++changed_events;
        }
        if (!ValidateProviderFinanceEvent(event, error) ||
            event.genesis_hash != finance.genesis_hash ||
            event.provider_id != finance.provider_id) {
            if (error.empty()) {
                error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
            }
            return false;
        }
        if (existing == finance.events.end()) {
            // The shared upsert enforces the durable event bound. Existing
            // entries are updated directly below so reconciliation can rebuild
            // daily totals once, rather than once per historical event.
            return UpsertProviderFinanceEvent(finance, event, error);
        }
        *existing = std::move(event);
        return true;
    };

    // Events created directly by pool-management RPCs are not represented by
    // a provider commit or maintenance reservation. Reconcile their chain
    // state here before adding any newly discoverable records below.
    for (ProviderFinanceEvent& event : finance.events) {
        ProviderFinanceEventState state;
        int64_t confirmed_at;
        transaction_state(event.transaction_id, event.created_at,
                          state, confirmed_at);
        if (event.state == state && event.confirmed_at == confirmed_at) {
            continue;
        }
        event.state = state;
        event.confirmed_at = confirmed_at;
        event.updated_at = std::max(
            {event.created_at, event.confirmed_at, now});
        finance.updated_at = std::max(
            finance.updated_at, event.updated_at);
        ledger_changed = true;
        ++changed_events;
    }

    // Wallet transaction metadata is persisted before broadcast, so finance
    // failure or a crash after either retirement step cannot lose its fee.
    const auto recover_cost = [&](ProviderFinanceEventKind kind, const uint256& txid) {
        HashWriter event_hasher = TaggedHash(
            "DigiByte Paymaster Finance Event v1");
        event_hasher << identity.provider_id << static_cast<uint8_t>(kind) << txid;
        const uint256 event_id = event_hasher.GetSHA256();
        if (std::any_of(finance.events.begin(), finance.events.end(),
                        [&](const ProviderFinanceEvent& event) {
                            return event.event_id == event_id;
                        })) {
            return true;
        }
        const auto transaction = wallet.mapWallet.find(txid);
        if (transaction == wallet.mapWallet.end() ||
            !transaction->second.tx) {
            if (!finance.earlier_history_partial) {
                finance.earlier_history_partial = true;
                ledger_changed = true;
            }
            return true;
        }
        const CAmount debit = wallet.GetDebit(
            *transaction->second.tx, ISMINE_ALL);
        const CAmount value_out = transaction->second.tx->GetValueOut();
        if (debit < value_out || !MoneyRange(debit - value_out)) {
            if (!finance.earlier_history_partial) {
                finance.earlier_history_partial = true;
                ledger_changed = true;
            }
            return true;
        }
        ProviderFinanceEvent event;
        event.event_id = event_id;
        event.genesis_hash = genesis_hash;
        event.provider_id = identity.provider_id;
        event.kind = kind;
        event.transaction_id = txid;
        event.dgb_cost = DGBSatoshis{debit - value_out};
        event.created_at = std::max(
            identity.created_at, transaction->second.GetTxTime());
        transaction_state(event.transaction_id, event.created_at,
                          event.state, event.confirmed_at);
        return apply_event(std::move(event));
    };
    for (const auto& [txid, transaction] : wallet.mapWallet) {
        const auto marker = transaction.mapValue.find(PAYMASTER_RETIREMENT_PROVIDER_KEY);
        if (marker == transaction.mapValue.end()) continue;
        if (marker->second != identity.provider_id.GetHex()) {
            error = "PAYMASTER_RETIREMENT_IDENTITY_MISMATCH";
            return false;
        }
        if (!recover_cost(ProviderFinanceEventKind::POOL_RETIREMENT, txid)) return false;
    }

    // Initial pool preparation predates maintenance records and therefore has
    // no separate operation object to replay. The persisted pool still binds
    // each original slot to its creating transaction. When that transaction
    // remains in this wallet, its native DGB fee can be reconstructed exactly;
    // never infer a cost when either the transaction or its wallet debit is
    // unavailable.
    std::vector<ProviderPoolEntry> provider_pool;
    const DatabaseReadStatus provider_pool_status =
        batch.ReadPaymasterProviderPoolWithStatus(provider_pool);
    if (provider_pool_status == DatabaseReadStatus::FOUND) {
        std::set<uint256> setup_transactions;
        for (const ProviderPoolEntry& entry : provider_pool) {
            if (entry.origin_commit_key.IsNull() &&
                !entry.outpoint.hash.IsNull()) {
                setup_transactions.insert(entry.outpoint.hash);
            }
        }
        for (const uint256& txid : setup_transactions) {
            if (!recover_cost(ProviderFinanceEventKind::POOL_SETUP, txid)) return false;
        }
    } else if (provider_pool_status != DatabaseReadStatus::NOT_FOUND) {
        error = ProviderPoolReadError(provider_pool);
        return false;
    }

    std::vector<PaymentSession> sessions;
    std::map<uint256, ProviderAttempt> attempts_by_commit;
    if (!batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_FINANCE_SESSION_SCAN_FAILED";
        return false;
    }
    for (const PaymentSession& session : sessions) {
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            const DatabaseReadStatus attempt_status =
                batch.ReadPaymasterAttemptWithStatus(attempt_id, attempt);
            if (attempt_status != DatabaseReadStatus::FOUND) {
                error = PersistedReadError(
                    attempt_status, "ProviderAttempt", attempt,
                    "PAYMASTER_FINANCE_ATTEMPT_MISSING",
                    "PAYMASTER_FINANCE_ATTEMPT_READ_FAILED");
                return false;
            }
            if (attempt.provider_id != identity.provider_id ||
                attempt.commit_key.IsNull()) {
                continue;
            }
            attempts_by_commit.emplace(attempt.commit_key, std::move(attempt));
        }
    }
    std::vector<ProviderCommitRecord> commits;
    if (!batch.ListPaymasterProviderCommits(commits)) {
        error = "PAYMASTER_FINANCE_COMMIT_SCAN_FAILED";
        return false;
    }
    for (const ProviderCommitRecord& commit : commits) {
        if (commit.provider_id != identity.provider_id) continue;
        const auto attempt = attempts_by_commit.find(commit.commit_key);
        if (attempt == attempts_by_commit.end() ||
            attempt->second.provider_manifest.manifest_id.IsNull()) {
            if (!finance.earlier_history_partial) {
                finance.earlier_history_partial = true;
                ledger_changed = true;
            }
            continue;
        }
        const ProviderAuthorizationManifest& manifest =
            attempt->second.provider_manifest;
        ProviderFinanceEvent event;
        event.event_id = commit.commit_key;
        event.genesis_hash = genesis_hash;
        event.provider_id = identity.provider_id;
        event.kind = ProviderFinanceEventKind::TRANSFER;
        event.funding_model = manifest.funding_model;
        event.sponsorship_scope = manifest.sponsorship_scope;
        event.transaction_id = commit.final_txid;
        event.dd_income = manifest.service_fee;
        event.dgb_cost = manifest.network_fee;
        event.created_at = commit.committed_at;
        transaction_state(event.transaction_id, event.created_at,
                          event.state, event.confirmed_at);
        if (!apply_event(std::move(event))) return false;
    }

    ProviderMaintenanceLedger maintenance;
    const DatabaseReadStatus maintenance_status =
        batch.ReadPaymasterMaintenanceLedgerWithStatus(maintenance);
    if (maintenance_status == DatabaseReadStatus::FOUND) {
        for (const ProviderMaintenanceRecord& record : maintenance.records) {
            if (record.transaction_id.IsNull()) continue;
            ProviderFinanceEvent event;
            event.event_id = record.operation_id;
            event.genesis_hash = genesis_hash;
            event.provider_id = identity.provider_id;
            event.kind = record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS
                ? ProviderFinanceEventKind::CARRIER_WITHDRAWAL
                : record.IsPreparation() ? ProviderFinanceEventKind::POOL_SETUP
                                         : ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT;
            event.transaction_id = record.transaction_id;
            event.dgb_cost = record.actual_fee;
            event.created_at = record.created_at;
            if (record.IsPreparation()) {
                const auto transaction = wallet.mapWallet.find(record.transaction_id);
                if (transaction != wallet.mapWallet.end()) {
                    event.created_at = std::max(identity.created_at, transaction->second.GetTxTime());
                }
            }
            transaction_state(event.transaction_id, event.created_at,
                              event.state, event.confirmed_at);
            if (record.state == ProviderMaintenanceState::FAILED ||
                record.state == ProviderMaintenanceState::RELEASED) {
                event.state = ProviderFinanceEventState::INVALIDATED;
                event.confirmed_at = 0;
            }
            if (!apply_event(std::move(event))) return false;
        }
    } else if (maintenance_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            maintenance_status, "ProviderMaintenanceLedger", maintenance,
            "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
        return false;
    }

    if (!ledger_changed) return true;
    finance.updated_at = std::max(finance.updated_at, now);
    for (const ProviderFinanceEvent& event : finance.events) {
        finance.updated_at = std::max(finance.updated_at, event.updated_at);
    }
    if (!RebuildProviderFinanceDailyTotals(finance, error) ||
        !batch.WritePaymasterFinanceLedger(finance)) {
        if (error.empty()) error = "PAYMASTER_FINANCE_DATABASE_WRITE";
        return false;
    }
    return true;
}

/** Persist the exact native DGB cost of a wallet-created provider finance
 * transaction. The event id is derived from provider, category and txid, so a
 * retry is idempotent while two categories can never silently share an id. */
bool RecordProviderFinanceTransaction(
    CWallet& wallet,
    DigiDollar::Paymaster::ProviderFinanceEventKind kind,
    const CTransactionRef& transaction,
    DigiDollar::Paymaster::DGBSatoshis dgb_cost,
    int64_t now,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!transaction || dgb_cost.value < 0 || now <= 0) {
        error = "PAYMASTER_INVALID_FINANCE_TRANSACTION";
        return false;
    }
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_IDENTITY_NOT_FOUND";
        return false;
    }
    const uint256 genesis_hash = Params().GenesisBlock().GetHash();
    ProviderFinanceLedger ledger;
    if (!batch.ReadPaymasterFinanceLedger(ledger)) {
        if (batch.HasPaymasterFinanceLedger()) {
            error = "PAYMASTER_INVALID_FINANCE_LEDGER";
            return false;
        }
        ledger.genesis_hash = genesis_hash;
        ledger.provider_id = identity.provider_id;
        ledger.history_complete_from = now;
        ledger.earlier_history_partial = identity.created_at < now;
        ledger.updated_at = now;
        if (!RebuildProviderFinanceDailyTotals(ledger, error)) return false;
    }
    if (ledger.genesis_hash != genesis_hash ||
        ledger.provider_id != identity.provider_id) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }
    HashWriter event_hasher = TaggedHash(
        "DigiByte Paymaster Finance Event v1");
    event_hasher << identity.provider_id << static_cast<uint8_t>(kind)
                 << transaction->GetHash();
    ProviderFinanceEvent event;
    event.event_id = event_hasher.GetSHA256();
    event.genesis_hash = genesis_hash;
    event.provider_id = identity.provider_id;
    event.kind = kind;
    event.state = ProviderFinanceEventState::PENDING;
    event.transaction_id = transaction->GetHash();
    event.dgb_cost = dgb_cost;
    event.created_at = now;
    event.updated_at = now;
    if (!UpsertProviderFinanceEvent(ledger, event, error) ||
        !batch.WritePaymasterFinanceLedger(ledger)) {
        if (error.empty()) error = "PAYMASTER_FINANCE_DATABASE_WRITE";
        return false;
    }
    return true;
}

/** Calculate the actual base-coin fee of a transaction already present in
 * this wallet. DigiDollar inputs carry zero CTxOut value, so this remains an
 * exact DGB fee calculation for carrier-management transactions as well. */
std::optional<DigiDollar::Paymaster::DGBSatoshis>
GetProviderFinanceTransactionFee(CWallet& wallet,
                                 const CTransactionRef& transaction)
{
    if (!transaction) return std::nullopt;
    LOCK(wallet.cs_wallet);
    const CAmount debit = wallet.GetDebit(*transaction, ISMINE_ALL);
    const CAmount value_out = transaction->GetValueOut();
    if (debit < value_out || !MoneyRange(debit - value_out)) {
        return std::nullopt;
    }
    return DigiDollar::Paymaster::DGBSatoshis{debit - value_out};
}

// Paid DGB replenishment reserves its worst-case maintenance fee before wallet
// construction. The durable plan record makes a crash after broadcast
// distinguishable from a request to create a second replacement transaction.
bool RunAutomaticDGBReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (missing_admission == 0 && missing_operational == 0) return true;
    if (!policy.automatic_replenishment || !policy.paid_maintenance_approved) {
        error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
        return false;
    }
    ProviderPolicy provider_policy;
    if (!GetPaymasterProviderPolicy(wallet, provider_policy)) {
        error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }

    ProviderMaintenanceLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    std::optional<ProviderMaintenanceRecord> operation;
    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status == DatabaseReadStatus::NOT_FOUND) {
            ledger.accounting_time_high_water = GetTime();
        } else if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        // Validate the pool before reserving budget, creating destinations, or
        // constructing a transaction. Re-read it after commit to cover a
        // concurrent change without ever overwriting an unreadable record.
        if (!ReadOptionalProviderPool(batch, pool, error)) return false;
        const auto planned = std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [](const ProviderMaintenanceRecord& record) {
                return record.kind == ProviderMaintenanceKind::REPLENISH_DGB &&
                       record.state == ProviderMaintenanceState::PLANNED;
            });
        if (planned != ledger.records.end()) operation = *planned;
    }

    const int64_t admission_value = MIN_ADMISSION_DGB_SATOSHIS;
    const int64_t operational_value = std::max<int64_t>(
        MIN_ADMISSION_DGB_SATOSHIS,
        provider_policy.maximum_network_fee.value);
    if (operation) {
        size_t planned_admission{0};
        size_t planned_operational{0};
        const bool outputs_match = std::all_of(
            operation->outputs.begin(), operation->outputs.end(),
            [&](const ProviderMaintenanceOutput& output) {
                if (output.asset != PoolAsset::DGB ||
                    output.carrier_value.value != 0) {
                    return false;
                }
                if (output.purpose == PoolPurpose::ADMISSION &&
                    output.dgb_value.value == admission_value) {
                    ++planned_admission;
                    return true;
                }
                if (output.purpose == PoolPurpose::OPERATIONAL &&
                    output.dgb_value.value == operational_value) {
                    ++planned_operational;
                    return true;
                }
                return false;
            });
        const bool plan_matches =
            outputs_match && planned_admission == missing_admission &&
            planned_operational == missing_operational &&
            operation->maximum_fee ==
                policy.maximum_maintenance_fee_per_transaction &&
            operation->plan_id ==
                MaintenancePlanId(operation->kind, operation->outputs, policy);
        if (!plan_matches) {
            if (!ReleaseProviderMaintenanceBudget(
                    ledger, operation->operation_id, GetTime(), error)) {
                return false;
            }
            {
                LOCK(wallet.cs_wallet);
                if (!WalletBatch{wallet.GetDatabase()}
                         .WritePaymasterMaintenanceLedger(ledger)) {
                    error = "PAYMASTER_DATABASE_WRITE";
                    return false;
                }
            }
            operation.reset();
        }
    }
    if (!operation) {
        ProviderMaintenanceRecord record;
        do {
            record.operation_id = GetRandHash();
        } while (record.operation_id.IsNull());
        record.kind = ProviderMaintenanceKind::REPLENISH_DGB;
        const auto append_outputs = [&](PoolPurpose purpose, size_t count,
                                        int64_t value, const char* label) {
            for (size_t index{0}; index < count; ++index) {
                const auto destination = wallet.GetNewDestination(
                    OutputType::BECH32M, label);
                if (!destination) return false;
                ProviderMaintenanceOutput output;
                output.purpose = purpose;
                output.asset = PoolAsset::DGB;
                output.script_pub_key = GetScriptForDestination(*destination);
                output.dgb_value = DGBSatoshis{value};
                record.outputs.push_back(std::move(output));
            }
            return true;
        };
        if (!append_outputs(PoolPurpose::ADMISSION, missing_admission,
                            admission_value, "Paymaster admission maintenance") ||
            !append_outputs(PoolPurpose::OPERATIONAL, missing_operational,
                            operational_value, "Paymaster operational maintenance")) {
            error = "PAYMASTER_POOL_DESTINATION_UNAVAILABLE";
            return false;
        }
        record.plan_id = MaintenancePlanId(record.kind, record.outputs, policy);
        record.maximum_fee = policy.maximum_maintenance_fee_per_transaction;
        if (!ReserveProviderMaintenanceBudget(
                ledger, policy, record, GetTime(), error)) {
            return false;
        }
        {
            LOCK(wallet.cs_wallet);
            if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterMaintenanceLedger(ledger)) {
                error = "PAYMASTER_DATABASE_WRITE";
                return false;
            }
        }
        operation = std::move(record);
    }

    std::vector<CRecipient> recipients;
    for (const ProviderMaintenanceOutput& output : operation->outputs) {
        CTxDestination destination;
        if (output.asset != PoolAsset::DGB ||
            !ExtractDestination(output.script_pub_key, destination)) {
            error = "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            return false;
        }
        recipients.push_back({destination, output.dgb_value.value, false});
    }
    CCoinControl coin_control;
    // As with finite setup, stem-only change is invisible to the ordinary
    // broadcast fee precheck. Wait for confirmed funding before signing.
    coin_control.m_min_depth = 1;
    auto created = CreateTransaction(wallet, recipients, /*change_pos=*/-1,
                                     coin_control, /*sign=*/true);
    if (!created) {
        error = util::ErrorString(created).original;
        return false;
    }
    if (created->fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
        return false;
    }
    std::string commit_error;
    if (!wallet.CommitTransaction(created->tx, {}, {}, &commit_error)) {
        error = "PAYMASTER_POOL_TRANSACTION_REJECTED: " + commit_error;
        return false;
    }

    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        if (!ReadOptionalProviderPool(batch, pool, error) ||
            !SpendProviderMaintenanceBudget(
                ledger, operation->operation_id, created->tx->GetHash(),
                DGBSatoshis{created->fee}, GetTime(), error)) {
            if (error.empty()) {
                error = "PAYMASTER_MAINTENANCE_LEDGER_UPDATE_FAILED";
            }
            return false;
        }
        const size_t missing_pool_entries = std::count_if(
            operation->outputs.begin(), operation->outputs.end(),
            [&](const ProviderMaintenanceOutput& output) {
                const auto output_index = FindMaintenanceOutputIndex(
                    *created->tx, output);
                return output_index &&
                       std::none_of(pool.begin(), pool.end(),
                                    [&](const ProviderPoolEntry& existing) {
                                        return existing.outpoint ==
                                               COutPoint{created->tx->GetHash(),
                                                         *output_index};
                                    });
            });
        if (!MakeProviderPoolRoomForMaintenance(
                pool, missing_pool_entries, error)) {
            return false;
        }
        for (const ProviderMaintenanceOutput& output : operation->outputs) {
            const auto output_index = FindMaintenanceOutputIndex(
                *created->tx, output);
            if (!output_index) {
                error = "PAYMASTER_MAINTENANCE_OUTPUT_MISSING";
                return false;
            }
            ProviderPoolEntry entry;
            entry.outpoint = COutPoint{created->tx->GetHash(), *output_index};
            entry.purpose = output.purpose;
            entry.asset = PoolAsset::DGB;
            entry.state = PoolEntryState::PENDING_SUCCESSOR;
            entry.script_pub_key = output.script_pub_key;
            entry.dgb_value = output.dgb_value;
            entry.reservation_id = operation->operation_id;
            entry.origin_commit_key = operation->operation_id;
            entry.updated_at = GetTime();
            if (std::none_of(pool.begin(), pool.end(), [&](const ProviderPoolEntry& existing) {
                    return existing.outpoint == entry.outpoint;
                })) pool.push_back(std::move(entry));
        }
        if (!batch.TxnBegin()) {
            error = "PAYMASTER_DATABASE_BEGIN";
            return false;
        }
        if (!batch.WritePaymasterMaintenanceLedger(ledger) ||
            !batch.WritePaymasterProviderPool(pool)) {
            batch.TxnAbort();
            error = "PAYMASTER_DATABASE_WRITE";
            return false;
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
    }
    return true;
}

// DD carriers are ordinary wallet value with a dedicated provider role. Never
// select reserved/pending carrier value, and never silently turn normal client
// DD into provider liquidity without the configured target and preview binding.
bool RunAutomaticCarrierReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (missing_admission == 0 && missing_operational == 0) return true;
    if (!policy.automatic_replenishment || !policy.paid_maintenance_approved) {
        error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
        return false;
    }
    DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
    if (!dd_wallet) {
        error = "PAYMASTER_DD_WALLET_UNAVAILABLE";
        return false;
    }

    ProviderMaintenanceLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    std::optional<ProviderMaintenanceRecord> operation;
    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status == DatabaseReadStatus::NOT_FOUND) {
            ledger.accounting_time_high_water = GetTime();
        } else if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        if (!ReadOptionalProviderPool(batch, pool, error)) return false;
        const auto planned = std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [](const ProviderMaintenanceRecord& record) {
                return record.kind == ProviderMaintenanceKind::REPLENISH_CARRIER &&
                       record.state == ProviderMaintenanceState::PLANNED;
            });
        if (planned != ledger.records.end()) operation = *planned;
    }

    if (operation) {
        size_t planned_admission{0};
        size_t planned_operational{0};
        const bool outputs_match = std::all_of(
            operation->outputs.begin(), operation->outputs.end(),
            [&](const ProviderMaintenanceOutput& output) {
                if (output.asset != PoolAsset::DD_CARRIER ||
                    output.dgb_value.value != 0 ||
                    output.carrier_value.value != 100) {
                    return false;
                }
                if (output.purpose == PoolPurpose::ADMISSION) {
                    ++planned_admission;
                    return true;
                }
                if (output.purpose == PoolPurpose::OPERATIONAL) {
                    ++planned_operational;
                    return true;
                }
                return false;
            });
        const bool plan_matches =
            outputs_match && planned_admission == missing_admission &&
            planned_operational == missing_operational &&
            operation->maximum_fee ==
                policy.maximum_maintenance_fee_per_transaction &&
            operation->plan_id ==
                MaintenancePlanId(operation->kind, operation->outputs, policy);
        if (!plan_matches) {
            if (!ReleaseProviderMaintenanceBudget(
                    ledger, operation->operation_id, GetTime(), error)) {
                return false;
            }
            {
                LOCK(wallet.cs_wallet);
                if (!WalletBatch{wallet.GetDatabase()}
                         .WritePaymasterMaintenanceLedger(ledger)) {
                    error = "PAYMASTER_DATABASE_WRITE";
                    return false;
                }
            }
            operation.reset();
        }
    }

    if (!operation) {
        ProviderMaintenanceRecord record;
        do {
            record.operation_id = GetRandHash();
        } while (record.operation_id.IsNull());
        record.kind = ProviderMaintenanceKind::REPLENISH_CARRIER;
        const auto append_outputs = [&](PoolPurpose purpose, size_t count,
                                        const char* label) {
            for (size_t index{0}; index < count; ++index) {
                const auto destination = wallet.GetNewDestination(
                    OutputType::BECH32M, label);
                if (!destination) return false;
                ProviderMaintenanceOutput output;
                output.purpose = purpose;
                output.asset = PoolAsset::DD_CARRIER;
                output.script_pub_key = GetScriptForDestination(*destination);
                output.carrier_value = DDCents{100};
                record.outputs.push_back(std::move(output));
            }
            return true;
        };
        if (!append_outputs(PoolPurpose::ADMISSION, missing_admission,
                            "Paymaster admission carrier maintenance") ||
            !append_outputs(PoolPurpose::OPERATIONAL, missing_operational,
                            "Paymaster operational carrier maintenance")) {
            error = "PAYMASTER_POOL_DESTINATION_UNAVAILABLE";
            return false;
        }
        record.plan_id = MaintenancePlanId(record.kind, record.outputs, policy);
        record.maximum_fee = policy.maximum_maintenance_fee_per_transaction;
        if (!ReserveProviderMaintenanceBudget(
                ledger, policy, record, GetTime(), error)) {
            return false;
        }
        {
            LOCK(wallet.cs_wallet);
            if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterMaintenanceLedger(ledger)) {
                error = "PAYMASTER_DATABASE_WRITE";
                return false;
            }
        }
        operation = std::move(record);
    }

    std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
    recipients.reserve(operation->outputs.size());
    for (const ProviderMaintenanceOutput& output : operation->outputs) {
        CTxDestination destination;
        if (output.asset != PoolAsset::DD_CARRIER ||
            !ExtractDestination(output.script_pub_key, destination)) {
            error = "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            return false;
        }
        const std::string encoded = EncodeDigiDollarAddress(destination);
        CDigiDollarAddress address{encoded};
        if (encoded.empty() || !address.IsValid()) {
            error = "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE";
            return false;
        }
        recipients.emplace_back(std::move(address), output.carrier_value.value);
    }

    DDTransferPlan plan;
    std::string plan_error;
    if (!dd_wallet->PlanDigiDollarTransfer(
            recipients, plan, plan_error,
            /*preset_dd_inputs=*/nullptr,
            /*allow_paymaster_pool_inputs=*/false)) {
        error = "PAYMASTER_CARRIER_REPLENISHMENT_FAILED: " + plan_error;
        return false;
    }
    if (plan.estimated_fee <= 0 ||
        plan.estimated_fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
        return false;
    }

    std::string txid_string;
    std::string transfer_error;
    if (!dd_wallet->TransferDigiDollarMany(
            recipients, txid_string, transfer_error,
            /*dd_change_out=*/nullptr,
            /*preset_dd_inputs=*/nullptr,
            "Paymaster automatic carrier replenishment",
            /*allow_paymaster_pool_inputs=*/false,
            /*exact_plan=*/&plan)) {
        error = "PAYMASTER_CARRIER_REPLENISHMENT_FAILED: " + transfer_error;
        return false;
    }
    const uint256 txid = uint256S(txid_string);
    CTransactionRef transaction;
    {
        LOCK(wallet.cs_wallet);
        const auto found = wallet.mapWallet.find(txid);
        if (found != wallet.mapWallet.end()) transaction = found->second.tx;
    }
    if (!transaction) {
        error = "PAYMASTER_MAINTENANCE_TRANSACTION_NOT_IN_WALLET";
        return false;
    }
    CAmount actual_fee{0};
    {
        LOCK(wallet.cs_wallet);
        const CAmount debit = wallet.GetDebit(*transaction, ISMINE_ALL);
        const CAmount value_out = transaction->GetValueOut();
        if (debit < value_out) {
            error = "PAYMASTER_MAINTENANCE_FEE_INVALID";
            return false;
        }
        actual_fee = debit - value_out;
    }
    // The DD planner returns a conservative signed-size fee ceiling.  The
    // finalized transaction may pay less when its exact witness/change shape
    // is smaller, but it must never exceed the previewed ceiling or the
    // operator's absolute maintenance limit.
    if (actual_fee <= 0 || actual_fee > plan.estimated_fee ||
        actual_fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_CHANGED";
        return false;
    }

    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        if (!ReadOptionalProviderPool(batch, pool, error) ||
            !SpendProviderMaintenanceBudget(
                ledger, operation->operation_id, txid,
                DGBSatoshis{actual_fee}, GetTime(), error)) {
            if (error.empty()) {
                error = "PAYMASTER_MAINTENANCE_LEDGER_UPDATE_FAILED";
            }
            return false;
        }
        const size_t missing_pool_entries = std::count_if(
            operation->outputs.begin(), operation->outputs.end(),
            [&](const ProviderMaintenanceOutput& output) {
                const auto output_index = FindMaintenanceOutputIndex(
                    *transaction, output);
                return output_index &&
                       std::none_of(pool.begin(), pool.end(),
                                    [&](const ProviderPoolEntry& existing) {
                                        return existing.outpoint ==
                                               COutPoint{txid, *output_index};
                                    });
            });
        if (!MakeProviderPoolRoomForMaintenance(
                pool, missing_pool_entries, error)) {
            return false;
        }
        for (const ProviderMaintenanceOutput& output : operation->outputs) {
            const auto output_index = FindMaintenanceOutputIndex(
                *transaction, output);
            if (!output_index) {
                error = "PAYMASTER_MAINTENANCE_OUTPUT_MISSING";
                return false;
            }
            ProviderPoolEntry entry;
            entry.outpoint = COutPoint{txid, *output_index};
            entry.purpose = output.purpose;
            entry.asset = PoolAsset::DD_CARRIER;
            entry.state = PoolEntryState::PENDING_SUCCESSOR;
            entry.script_pub_key = output.script_pub_key;
            entry.carrier_value = output.carrier_value;
            entry.reservation_id = operation->operation_id;
            entry.origin_commit_key = operation->operation_id;
            entry.updated_at = GetTime();
            if (std::none_of(pool.begin(), pool.end(),
                             [&](const ProviderPoolEntry& existing) {
                                 return existing.outpoint == entry.outpoint;
                             })) {
                pool.push_back(std::move(entry));
            }
        }
        if (!batch.TxnBegin()) {
            error = "PAYMASTER_DATABASE_BEGIN";
            return false;
        }
        if (!batch.WritePaymasterMaintenanceLedger(ledger) ||
            !batch.WritePaymasterProviderPool(pool)) {
            batch.TxnAbort();
            error = "PAYMASTER_DATABASE_WRITE";
            return false;
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
    }
    return true;
}

UniValue ReliabilityToJSON(const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
                           int64_t now)
{
    const auto summary = DigiDollar::Paymaster::SummarizeReliability(record, now);
    UniValue result{UniValue::VOBJ};
    result.pushKV("provider_id", record.provider_id.GetHex());
    result.pushKV("successful_attempts", summary.successful_attempts);
    result.pushKV("provider_failures", summary.provider_failures);
    result.pushKV("neutral_failures", summary.neutral_failures);
    result.pushKV("availability_timeouts", summary.availability_timeouts);
    result.pushKV("sufficient_data", summary.sufficient_data);
    if (summary.sufficient_data) result.pushKV("success_rate_basis_points", summary.success_rate_basis_points);
    result.pushKV("latency_ewma_ms", record.latency_ewma_ms);
    result.pushKV("consecutive_provider_failures", record.consecutive_provider_failures);
    result.pushKV("last_observation_at", record.last_observation_at);
    result.pushKV("cooldown_until", record.cooldown_until);
    result.pushKV("cooldown_active", record.cooldown_until > now);
    return result;
}

std::string FeeModeName(DigiDollar::Paymaster::FeeMode mode)
{
    using DigiDollar::Paymaster::FeeMode;
    switch (mode) {
    case FeeMode::DGB: return "dgb";
    case FeeMode::PAYMASTER: return "paymaster";
    case FeeMode::AUTO: return "auto";
    }
    return "unknown";
}

std::string ResultStatusName(DigiDollar::Paymaster::PaymasterResultStatus status)
{
    using DigiDollar::Paymaster::PaymasterResultStatus;
    switch (status) {
    case PaymasterResultStatus::NO_FINAL_COMMIT: return "no_final_commit";
    case PaymasterResultStatus::USER_PSBT_ACCEPTED: return "user_psbt_accepted";
    case PaymasterResultStatus::FINAL_COMMITTED: return "final_committed";
    case PaymasterResultStatus::BROADCAST_ATTEMPTED: return "broadcast_attempted";
    case PaymasterResultStatus::SLOT_UNAVAILABLE: return "slot_unavailable";
    case PaymasterResultStatus::REJECTED: return "rejected";
    }
    return "unknown";
}

std::optional<std::string> DigiDollarAddressForScript(const CScript& script)
{
    CTxDestination destination;
    if (!ExtractDestination(script, destination)) return std::nullopt;
    const std::string address = EncodeDigiDollarAddress(destination);
    const CDigiDollarAddress decoded{address};
    if (address.empty() || !decoded.IsValid()) return std::nullopt;
    return address;
}

// -------------------------------------------------------------------------
// Client session serialization, replay, and equivocation handling
// -------------------------------------------------------------------------
UniValue SessionToJSON(
    const DigiDollar::Paymaster::PaymentSession& session,
    const PaymasterStore* store)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", session.request_id);
    result.pushKV("session_id", session.session_id.GetHex());
    result.pushKV("canonical_request_hash", session.canonical_request_hash.GetHex());
    if (session.requested_amount.value > 0) {
        result.pushKV("requested_amount_cents", session.requested_amount.value);
        result.pushKV("subtract_paymaster_fee_from_amount",
                      session.subtract_paymaster_fee_from_amount);
        result.pushKV("send_all_spendable_dd",
                      session.send_all_spendable_dd);
    }
    result.pushKV("requested_fee_mode", FeeModeName(session.fee_mode_requested));
    result.pushKV("fee_mode_used", FeeModeName(session.fee_mode_used));
    result.pushKV("session_state", std::string{SessionStateName(session.state)});
    if (session.pending_phase != PendingPhase::NONE) {
        result.pushKV("pending_phase", std::string{PendingPhaseName(session.pending_phase)});
    }
    result.pushKV("final", IsTerminal(session.state));
    const bool has_final_transaction = !session.final_txid.IsNull();
    std::string broadcast_state{"not_attempted"};
    if (session.state == SessionState::CONFIRMED ||
        session.state == SessionState::CANCELED_SAFE)
        broadcast_state = "confirmed";
    else if (session.pending_phase == PendingPhase::CANCEL_MEMPOOL)
        broadcast_state = "accepted_mempool";
    else if (session.state == SessionState::MEMPOOL)
        broadcast_state = "accepted_mempool";
    else if (session.state == SessionState::STEMPOOL)
        broadcast_state = "accepted_stempool";
    else if (session.state == SessionState::PENDING_PROVIDER || has_final_transaction)
        broadcast_state = "unknown";
    result.pushKV("broadcast_state", broadcast_state);
    const std::string confirmation_state = session.state == SessionState::CONFIRMED ? "payment_confirmed" : (session.state == SessionState::CANCELED_SAFE ? "recovery_confirmed" : (session.state == SessionState::CONFLICTED ? "conflicted" : "unconfirmed"));
    result.pushKV("confirmation_state", confirmation_state);
    result.pushKV("created_at", session.created_at);
    result.pushKV("updated_at", session.updated_at);
    if (!session.final_txid.IsNull()) result.pushKV("txid", session.final_txid.GetHex());
    if (!session.recovery_txid.IsNull()) {
        result.pushKV("recovery_txid", session.recovery_txid.GetHex());
    }

    UniValue inputs{UniValue::VARR};
    for (const auto& input : session.user_inputs) {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("txid", input.hash.GetHex());
        entry.pushKV("vout", input.n);
        inputs.push_back(std::move(entry));
    }
    result.pushKV("reserved_user_inputs", std::move(inputs));
    result.pushKV("provider_attempts", static_cast<uint64_t>(session.attempt_ids.size()));

    // The session owns the provider-independent gross order while its latest
    // client authorization manifest owns the exact provider-dependent split.
    // Surface both together when the durable attempt is still available so
    // status queries and exact retries never report the gross amount as the
    // recipient amount in subtract-fee mode.
    if (store && !session.provider_side) {
        for (auto it = session.attempt_ids.rbegin();
             it != session.attempt_ids.rend(); ++it) {
            ProviderAttempt attempt;
            if (!store->GetAttempt(*it, attempt)) continue;
            const ClientAuthorizationManifest& manifest =
                attempt.client_manifest;
            if (manifest.manifest_id.IsNull() ||
                manifest.version !=
                    ClientAuthorizationManifest::CURRENT_VERSION ||
                manifest.recipient_amount.value < 0 ||
                manifest.service_fee.value < 0 ||
                manifest.recipient_amount.value >
                    std::numeric_limits<int64_t>::max() -
                        manifest.service_fee.value) {
                continue;
            }
            result.pushKV("provider_id", manifest.provider_id.GetHex());
            result.pushKV(
                "privacy_profile",
                attempt.privacy_profile == PrivacyProfile::HIGH
                    ? "high" : "standard");
            result.pushKV("offer_id", manifest.offer_id.GetHex());
            result.pushKV("policy_hash", manifest.policy_hash.GetHex());
            result.pushKV(
                "funding_model",
                manifest.funding_model == FundingModel::SPONSORED
                    ? "sponsored"
                    : "user_paid");
            result.pushKV("payment_cents",
                          manifest.recipient_amount.value);
            result.pushKV("service_fee_cents",
                          manifest.service_fee.value);
            result.pushKV(
                "user_total_cents",
                manifest.recipient_amount.value +
                    manifest.service_fee.value);
            if (const auto address = DigiDollarAddressForScript(
                    manifest.recipient_script)) {
                result.pushKV("to_address", *address);
            }
            break;
        }
    }
    return result;
}

bool FindSession(const UniValue& lookup, PaymasterStore& store,
                 DigiDollar::Paymaster::PaymentSession& session)
{
    RPCTypeCheckObj(lookup,
                    {{"request_id", UniValueType(UniValue::VSTR)},
                     {"session_id", UniValueType(UniValue::VSTR)}},
                    /*fAllowNull=*/true, /*fStrict=*/true);
    const bool by_request = !lookup.find_value("request_id").isNull();
    const bool by_session = !lookup.find_value("session_id").isNull();
    if (by_request == by_session) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "lookup must contain exactly one of request_id or session_id");
    }
    if (by_request) {
        const std::string request_id = lookup.find_value("request_id").get_str();
        if (!DigiDollar::Paymaster::IsCanonicalRequestId(request_id)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "request_id must be a canonical lowercase UUID");
        }
        return store.GetSessionByRequestId(request_id, session);
    }
    return store.GetSessionBySessionId(ParseHashO(lookup, "session_id"), session);
}

UniValue AttemptToJSON(const DigiDollar::Paymaster::ProviderAttempt& attempt)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("attempt_id", attempt.attempt_id.GetHex());
    result.pushKV("provider_id", attempt.provider_id.GetHex());
    if (!attempt.provider_endpoint.empty()) {
        result.pushKV("provider_endpoint", attempt.provider_endpoint);
    }
    result.pushKV("attempt_state", std::string{AttemptStateName(attempt.state)});
    result.pushKV("privacy_profile",
                  attempt.privacy_profile == PrivacyProfile::HIGH
                      ? "high" : "standard");
    if (!attempt.quote_id.IsNull()) result.pushKV("quote_id", attempt.quote_id.GetHex());
    if (!attempt.template_commitment.IsNull()) {
        result.pushKV("template_commitment", attempt.template_commitment.GetHex());
    }
    if (!attempt.unsigned_txid.IsNull()) result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
    if (!attempt.commit_key.IsNull()) result.pushKV("commit_key", attempt.commit_key.GetHex());
    if (attempt.quote_expires_at > 0) result.pushKV("quote_expires_at", attempt.quote_expires_at);
    if (attempt.retry_until > 0) result.pushKV("retry_until", attempt.retry_until);
    if (!attempt.client_manifest.manifest_id.IsNull()) {
        result.pushKV("authorization_commitment",
                      attempt.client_manifest.manifest_id.GetHex());
        result.pushKV("authorization_accepted",
                      attempt.accepted_client_manifest_id ==
                              attempt.client_manifest.manifest_id &&
                          attempt.client_manifest_accepted_at > 0);
    }
    if (attempt.client_manifest_accepted_at > 0) {
        result.pushKV("authorization_accepted_at",
                      attempt.client_manifest_accepted_at);
    }
    return result;
}

std::vector<COutPoint> ParsePaymasterInputs(const UniValue& value)
{
    std::vector<COutPoint> inputs;
    std::set<COutPoint> unique;
    for (const UniValue& item : value.getValues()) {
        RPCTypeCheckObj(item,
                        {{"txid", UniValueType(UniValue::VSTR)},
                         {"vout", UniValueType(UniValue::VNUM)}},
                        /*fAllowNull=*/false, /*fStrict=*/true);
        const int64_t output_index = item.find_value("vout").getInt<int64_t>();
        if (output_index < 0 || output_index > std::numeric_limits<uint32_t>::max()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "selected input vout is out of range");
        }
        COutPoint outpoint{ParseHashO(item, "txid"), static_cast<uint32_t>(output_index)};
        if (outpoint.IsNull() || !unique.insert(outpoint).second) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "selected inputs contain an invalid duplicate");
        }
        inputs.push_back(outpoint);
    }
    if (inputs.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "selected_inputs must not be empty");
    return inputs;
}

std::vector<unsigned char> SerializeQuoteRequest(
    const DigiDollar::Paymaster::PaymasterQuoteRequest& quote_request)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << quote_request;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeCapacityRequest(
    const DigiDollar::Paymaster::PaymasterCapacityRequest& request)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << request;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeCapacityProof(
    const DigiDollar::Paymaster::PaymasterCapacityProof& proof)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << proof;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<DigiDollar::Paymaster::DirectMessage>
SelectCapacityProofMessages(
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const std::string& request_id,
    const uint256& session_id,
    const uint256& client_nonce)
{
    using namespace DigiDollar::Paymaster;
    std::vector<DirectMessage> selected;
    selected.reserve(messages.size());
    for (const DirectMessage& message : messages) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (proof && proof->provider_id == provider_id &&
            proof->request_id == request_id &&
            proof->session_id == session_id &&
            proof->client_nonce == client_nonce) {
            selected.push_back(message);
        }
    }
    return selected;
}

std::vector<unsigned char> SerializeRedactedQuoteRequest(
    DigiDollar::Paymaster::PaymasterQuoteRequest quote_request)
{
    quote_request.restricted_descriptor.reset();
    quote_request.restricted_capability.reset();
    return SerializeQuoteRequest(quote_request);
}

std::vector<unsigned char> SerializeQuoteResponse(
    const DigiDollar::Paymaster::PaymasterQuoteResponse& quote_response)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << quote_response;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

// Direct-message acknowledgement happens only after durable evidence or a
// durable rejection has been written. Removing queue entries first would lose
// the only proof of a conflicting response on crash.
bool AcknowledgeEquivocationMessages(
    DigiDollar::Paymaster::Manager& manager,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    std::string& error)
{
    if (messages.empty()) return true;
    std::vector<uint256> message_ids;
    message_ids.reserve(messages.size());
    for (const auto& message : messages) {
        message_ids.push_back(message.message_id);
    }
    if (!manager.AcknowledgeDirectMessagesIfPresent(message_ids)) {
        error = "PAYMASTER_EQUIVOCATION_INBOX_ACK_CONFLICT";
        return false;
    }
    return true;
}

bool AcknowledgeRejectedDirectMessage(
    DigiDollar::Paymaster::Manager& manager,
    const DigiDollar::Paymaster::DirectMessage& message,
    std::string& error)
{
    if (!manager.AcknowledgeDirectMessagesIfPresent({message.message_id})) {
        error = "PAYMASTER_DIRECT_INBOX_ACK_CONFLICT";
        return false;
    }
    return true;
}

/** Return whether an immutable quote continuation can never become valid on a
 * later scheduler pass. Capacity is durably reserved before its proof is sent,
 * so a quote that cannot reload that exact reservation is not a transient
 * liquidity or synchronization failure. Keeping such a message leased would
 * let one stale or malicious peer repeatedly fault an otherwise healthy
 * automatic provider service.
 */
bool IsPermanentCapacityContinuationError(const std::string& error)
{
    return error == "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH" ||
           error == "PAYMASTER_CAPACITY_RESERVATION_MISSING" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_MISSING" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_EXPIRED" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_CONFLICT" ||
           error == "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH" ||
           error == "PAYMASTER_CAPACITY_RESPONSE_ENCODING" ||
           error == "PAYMASTER_CAPACITY_RESPONSE_INDEX_MISMATCH" ||
           error == "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
}

/** Keep one provider-addressed inbound message leased until the durable
 * handler finishes. A normal return acknowledges the message; unwinding from
 * a local failure releases only the lease so the scheduler can retry it. The
 * caller must explicitly acknowledge permanently invalid messages before
 * throwing.
 */
bool PersistPendingCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < proofs.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterCapacityProof>(&proofs[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < proofs.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterCapacityProof>(
                &proofs[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingCapacityEquivocation(
                    attempt_id, SerializeCapacityProof(*first),
                    SerializeCapacityProof(*second),
                    std::max(proofs[first_index].received_at,
                             proofs[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

bool PersistPendingQuoteEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& quotes,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < quotes.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterQuoteResponse>(&quotes[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < quotes.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterQuoteResponse>(
                &quotes[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingQuoteEquivocation(
                    attempt_id, SerializeQuoteResponse(*first),
                    SerializeQuoteResponse(*second),
                    std::max(quotes[first_index].received_at,
                             quotes[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_QUOTE_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

bool PersistPendingAlternativeRecoveryCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& recovery_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < proofs.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterCapacityProof>(&proofs[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < proofs.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterCapacityProof>(
                &proofs[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingAlternativeRecoveryCapacityEquivocation(
                    recovery_id, SerializeCapacityProof(*first),
                    SerializeCapacityProof(*second),
                    std::max(proofs[first_index].received_at,
                             proofs[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

/** Drain provider-signed conflicts for one exact client attempt. Before the
 * first capacity snapshot or quote is accepted, every bounded pair is checked
 * and the first individually valid signed claim is durably staged. This keeps
 * sequential reconnect/restart conflicts provable even when neither artifact
 * passes the deeper local spend firewall. Staged claims are evidence-only and
 * never become authorization baselines. */
bool DrainClientAttemptEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error,
    EquivocationBlockPolicy block_policy,
    bool lease_messages)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (session.provider_side || attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt.attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(store, attempt.provider_id,
                                              error)) {
        return false;
    }

    std::optional<DirectMessageLease> proof_lease;
    std::vector<DirectMessage> unleased_proofs;
    if (lease_messages) {
        proof_lease.emplace(manager.LeaseCapacityProofs(
            attempt.provider_id, attempt.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_proofs = manager.PeekCapacityProofs(
            attempt.provider_id, attempt.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    }
    const std::vector<DirectMessage>& raw_proofs =
        proof_lease ? proof_lease->Messages() : unleased_proofs;
    const std::vector<DirectMessage> proofs = SelectCapacityProofMessages(
        raw_proofs, attempt.provider_id, session.request_id,
        session.session_id, attempt.client_nonce);
    if (attempt.capacity_snapshot.capacity_proof.empty()) {
        bool capacity_equivocation{false};
        if (!PersistPendingCapacityEquivocationPair(
                store, attempt.attempt_id, proofs, capacity_equivocation,
                error)) {
            return false;
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                return false;
            }
        } else {
            for (const DirectMessage& message : proofs) {
                const auto* proof =
                    std::get_if<PaymasterCapacityProof>(&message.payload);
                if (!proof) continue;
                bool claim_equivocation{false};
                if (!store.StageCapacityProofClaimCandidate(
                        attempt.attempt_id, SerializeCapacityProof(*proof),
                        message.received_at, claim_equivocation, error)) {
                    if (error !=
                        "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                        return false;
                    }
                    if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                          error)) {
                        return false;
                    }
                    continue;
                }
                if (claim_equivocation) {
                    if (!AcknowledgeEquivocationMessages(manager, proofs,
                                                         error)) {
                        return false;
                    }
                    if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                        return false;
                    }
                    break;
                }
                // Maintenance has no in-flight acceptance scope. Once the
                // signed candidate is durable it can consume the inbox copy;
                // active RPCs retain it for Chainstate validation below.
                if (!lease_messages &&
                    !AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
            }
        }
    } else {
        bool capacity_equivocation{false};
        for (const DirectMessage& message : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&message.payload);
            if (!proof) continue;
            const std::vector<unsigned char> encoded =
                SerializeCapacityProof(*proof);
            if (encoded == attempt.capacity_snapshot.capacity_proof) continue;

            std::string evidence_error;
            if (store.RecordCapacityEquivocation(
                    attempt.attempt_id, encoded, message.received_at,
                    evidence_error)) {
                capacity_equivocation = true;
                continue;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
        if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
            return false;
        }
        if (capacity_equivocation &&
            block_policy == EquivocationBlockPolicy::ENFORCE) {
            error = "PAYMASTER_CAPACITY_EQUIVOCATION";
            return false;
        }
    }

    std::optional<DirectMessageLease> quote_lease;
    std::vector<DirectMessage> unleased_quotes;
    if (lease_messages) {
        quote_lease.emplace(manager.LeaseQuoteResponses(
            session.request_id, session.session_id, attempt.provider_id,
            attempt.intent_hash, MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_quotes = manager.PeekQuoteResponses(
            session.request_id, session.session_id, attempt.provider_id,
            attempt.intent_hash, MAX_DIRECT_INBOX_MESSAGES_PER_SESSION,
            /*lease=*/false);
    }
    const std::vector<DirectMessage>& quotes =
        quote_lease ? quote_lease->Messages() : unleased_quotes;
    if (attempt.signed_quote.empty()) {
        bool quote_equivocation{false};
        if (!PersistPendingQuoteEquivocationPair(
                store, attempt.attempt_id, quotes, quote_equivocation, error)) {
            return false;
        }
        if (quote_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, quotes, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_QUOTE_EQUIVOCATION";
                return false;
            }
        } else {
            for (const DirectMessage& message : quotes) {
                const auto* response =
                    std::get_if<PaymasterQuoteResponse>(&message.payload);
                if (!response) continue;
                bool claim_equivocation{false};
                if (!store.StageQuoteResponseClaimCandidate(
                        attempt.attempt_id,
                        SerializeQuoteResponse(*response), message.received_at,
                        claim_equivocation, error)) {
                    if (error != "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE") {
                        return false;
                    }
                    if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                          error)) {
                        return false;
                    }
                    continue;
                }
                if (claim_equivocation) {
                    if (!AcknowledgeEquivocationMessages(manager, quotes,
                                                         error)) {
                        return false;
                    }
                    if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                        error = "PAYMASTER_QUOTE_EQUIVOCATION";
                        return false;
                    }
                    break;
                }
                if (!lease_messages &&
                    !AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
            }
        }
    } else {
        bool quote_equivocation{false};
        for (const DirectMessage& message : quotes) {
            const auto* response =
                std::get_if<PaymasterQuoteResponse>(&message.payload);
            if (!response) continue;
            const std::vector<unsigned char> encoded =
                SerializeQuoteResponse(*response);
            if (encoded == attempt.signed_quote) continue;

            std::string evidence_error;
            if (store.RecordQuoteEquivocation(
                    attempt.attempt_id, encoded, message.received_at,
                    evidence_error)) {
                quote_equivocation = true;
                continue;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_QUOTE_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
        if (!AcknowledgeEquivocationMessages(manager, quotes, error)) {
            return false;
        }
        if (quote_equivocation &&
            block_policy == EquivocationBlockPolicy::ENFORCE) {
            error = "PAYMASTER_QUOTE_EQUIVOCATION";
            return false;
        }
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(store, attempt.provider_id,
                                              error)) {
        return false;
    }
    return true;
}

bool DrainClientSessionEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    std::string& error,
    EquivocationBlockPolicy active_block_policy)
{
    using namespace DigiDollar::Paymaster;
    if (session.provider_side) return true;
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!store.GetAttempt(attempt_id, attempt)) {
            error = "PAYMASTER_ATTEMPT_NOT_FOUND";
            return false;
        }
        const EquivocationBlockPolicy attempt_policy =
            attempt_id == session.attempt_ids.back() ? active_block_policy : EquivocationBlockPolicy::OBSERVE_ONLY;
        if (!DrainClientAttemptEquivocations(
                manager, store, session, attempt, error, attempt_policy)) {
            return false;
        }
    }
    error.clear();
    return true;
}

bool DrainAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    std::string& error,
    EquivocationBlockPolicy block_policy,
    bool lease_messages)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (recovery.provider_side) {
        return true;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    std::optional<DirectMessageLease> proof_lease;
    std::vector<DirectMessage> unleased_proofs;
    if (lease_messages) {
        proof_lease.emplace(manager.LeaseCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_proofs = manager.PeekCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    }
    const std::vector<DirectMessage>& raw_proofs =
        proof_lease ? proof_lease->Messages() : unleased_proofs;
    const std::vector<DirectMessage> proofs = SelectCapacityProofMessages(
        raw_proofs, recovery.recovery_provider_id,
        recovery.capacity_request.request_id,
        recovery.capacity_request.session_id, recovery.client_nonce);
    if (recovery.capacity_snapshot.capacity_proof.empty()) {
        if (recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING) {
            return true;
        }
        bool capacity_equivocation{false};
        if (!PersistPendingAlternativeRecoveryCapacityEquivocationPair(
                store, recovery.recovery_id, proofs, capacity_equivocation,
                error)) {
            return false;
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                return false;
            }
            return true;
        }
        for (const DirectMessage& message : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&message.payload);
            if (!proof) continue;
            bool claim_equivocation{false};
            if (!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
                    recovery.recovery_id, SerializeCapacityProof(*proof),
                    message.received_at, claim_equivocation, error)) {
                if (error != "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                    return false;
                }
                if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
                continue;
            }
            if (claim_equivocation) {
                if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                    return false;
                }
                if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                    error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                    return false;
                }
                return true;
            }
            if (!lease_messages &&
                !AcknowledgeRejectedDirectMessage(manager, message, error)) {
                return false;
            }
        }
        return true;
    }
    bool capacity_equivocation{false};
    for (const DirectMessage& message : proofs) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (!proof) continue;
        const std::vector<unsigned char> encoded =
            SerializeCapacityProof(*proof);
        if (encoded == recovery.capacity_snapshot.capacity_proof) continue;

        std::string evidence_error;
        if (store.RecordAlternativeRecoveryCapacityEquivocation(
                recovery.recovery_id, encoded, message.received_at,
                evidence_error)) {
            capacity_equivocation = true;
            continue;
        }
        if (!evidence_error.empty() &&
            evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
            error = std::move(evidence_error);
            return false;
        }
    }
    if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
        return false;
    }
    if (capacity_equivocation &&
        block_policy == EquivocationBlockPolicy::ENFORCE) {
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    return true;
}

bool DrainPendingAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const std::vector<unsigned char>& first_capacity_proof,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (recovery.provider_side || first_capacity_proof.empty() ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING) {
        error = "PAYMASTER_INVALID_RECOVERY_CAPACITY_STATE";
        return false;
    }
    if (!EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    const auto raw_proofs = manager.PeekCapacityProofs(
        recovery.recovery_provider_id, recovery.client_nonce,
        MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    const auto proofs = SelectCapacityProofMessages(
        raw_proofs, recovery.recovery_provider_id,
        recovery.capacity_request.request_id,
        recovery.capacity_request.session_id, recovery.client_nonce);
    bool capacity_equivocation{false};
    for (const DirectMessage& message : proofs) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (!proof) continue;
        const std::vector<unsigned char> encoded =
            SerializeCapacityProof(*proof);
        if (encoded == first_capacity_proof) continue;
        std::string evidence_error;
        if (store.RecordPendingAlternativeRecoveryCapacityEquivocation(
                recovery.recovery_id, first_capacity_proof, encoded,
                message.received_at, evidence_error)) {
            capacity_equivocation = true;
            continue;
        }
        if (!evidence_error.empty() &&
            evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
            error = std::move(evidence_error);
            return false;
        }
    }
    if (capacity_equivocation) {
        if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
            return false;
        }
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    // The calling acceptance scope owns the prune lease until the recovery
    // snapshot and signed request commit. This comparison must not acquire a
    // second, unscoped reference that could survive an early return.
    return EnsureProviderNotEquivocationBlocked(
        store, recovery.recovery_provider_id, error);
}

std::vector<unsigned char> SerializeSubmit(
    const DigiDollar::Paymaster::PaymasterSubmit& submit)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << submit;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename T>
// Recovery messages use the same bounded direct-message transport as normal
// Paymaster traffic, but their semantic replay key is tied to the durable
// session rather than the transient peer connection.
bool QueueRecoveryMessage(WalletContext& context,
                          CWallet& wallet,
                          const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
                          const T& message,
                          int64_t now,
                          RecoveryQueueState& state,
                          std::string& error)
{
    using namespace DigiDollar::Paymaster;
    state = {};
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "PAYMASTER_DISABLED";
        return false;
    }
    const bool allow_local_endpoint{
        Params().GetChainType() == ChainType::REGTEST};
    CService endpoint = LookupNumeric(recovery.recovery_provider_endpoint);
    state.route_available = IsEndpointAllowedForPrivacyProfile(
        endpoint, recovery.privacy_profile, allow_local_endpoint);
    if (!state.route_available) {
        for (const Announcement& announcement :
             context.paymaster->GetDirectory().List(now)) {
            if (GetPaymasterId(announcement.identity_key) ==
                    recovery.recovery_provider_id &&
                IsEndpointAllowedForPrivacyProfile(
                    announcement.endpoint, recovery.privacy_profile,
                    allow_local_endpoint)) {
                endpoint = announcement.endpoint;
                state.route_available = true;
                break;
            }
        }
    }
    if (!state.route_available) return true;
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    NodeId peer_id{-1};
    node->connman->ForEachNode([&](CNode* peer) {
        if (peer_id == -1 && peer->IsPaymasterConn() &&
            peer->addr == endpoint) {
            peer_id = peer->GetId();
        }
    });
    if (peer_id == -1) {
        state.connection_pending = node->connman->AddConnection(
            endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
            /*paymaster_high_privacy=*/
            recovery.privacy_profile == PrivacyProfile::HIGH);
        return true;
    }
    const std::vector<unsigned char> encoded =
        SerializeRecoveryMessage(message);
    const uint256 message_id{Hash(encoded)};
    state.queued =
        context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
        context.paymaster->QueueOutboundDirectMessage(
            peer_id, message_id, encoded.size(), DirectPayload{message}, now);
    if (!state.queued) {
        error = "PAYMASTER_DIRECT_QUEUE_FULL";
        return false;
    }
    node->connman->WakeMessageHandler();
    return true;
}

template bool QueueRecoveryMessage<DigiDollar::Paymaster::PaymasterCapacityRequest>(
    WalletContext&,
    CWallet&,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord&,
    const DigiDollar::Paymaster::PaymasterCapacityRequest&,
    int64_t,
    RecoveryQueueState&,
    std::string&);
template bool QueueRecoveryMessage<DigiDollar::Paymaster::AlternativeRecoveryRequest>(
    WalletContext&,
    CWallet&,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord&,
    const DigiDollar::Paymaster::AlternativeRecoveryRequest&,
    int64_t,
    RecoveryQueueState&,
    std::string&);
template bool QueueRecoveryMessage<DigiDollar::Paymaster::AlternativeRecoverySubmit>(
    WalletContext&,
    CWallet&,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord&,
    const DigiDollar::Paymaster::AlternativeRecoverySubmit&,
    int64_t,
    RecoveryQueueState&,
    std::string&);

std::string AlternativeRecoveryPhaseName(
    DigiDollar::Paymaster::AlternativeRecoveryPhase phase)
{
    using DigiDollar::Paymaster::AlternativeRecoveryPhase;
    switch (phase) {
    case AlternativeRecoveryPhase::CAPACITY_PENDING: return "capacity_pending";
    case AlternativeRecoveryPhase::REQUEST_READY: return "request_ready";
    case AlternativeRecoveryPhase::RESPONSE_VALIDATED: return "response_validated";
    case AlternativeRecoveryPhase::USER_SIGNED: return "user_signed";
    case AlternativeRecoveryPhase::FINAL_COMMITTED: return "final_committed";
    }
    return "unknown";
}

UniValue AlternativeRecoveryToJSON(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("recovery_id", recovery.recovery_id.GetHex());
    result.pushKV("recovery_provider_id",
                  recovery.recovery_provider_id.GetHex());
    result.pushKV("offer_id", recovery.offer_id.GetHex());
    result.pushKV("policy_hash", recovery.policy_hash.GetHex());
    result.pushKV("original_commit_key",
                  recovery.original_commit_key.GetHex());
    result.pushKV("original_template_commitment",
                  recovery.original_template_commitment.GetHex());
    result.pushKV("privacy_profile",
                  recovery.privacy_profile == PrivacyProfile::HIGH ? "high" : "standard");
    result.pushKV("phase", AlternativeRecoveryPhaseName(recovery.phase));
    result.pushKV("expired", recovery.expired);
    UniValue user_inputs{UniValue::VARR};
    for (const COutPoint& outpoint : recovery.recovery_request.user_dd_inputs) {
        UniValue input{UniValue::VOBJ};
        input.pushKV("txid", outpoint.hash.GetHex());
        input.pushKV("vout", outpoint.n);
        user_inputs.push_back(std::move(input));
    }
    result.pushKV("user_dd_inputs", std::move(user_inputs));
    UniValue wallet_returns{UniValue::VARR};
    for (const AlternativeRecoveryReturn& wallet_return :
         recovery.recovery_request.wallet_returns) {
        UniValue output{UniValue::VOBJ};
        output.pushKV("script_pub_key", HexStr(wallet_return.script_pub_key));
        CTxDestination destination;
        if (ExtractDestination(wallet_return.script_pub_key, destination)) {
            output.pushKV("address", EncodeDestination(destination));
        }
        output.pushKV("amount_cents", wallet_return.amount.value);
        wallet_returns.push_back(std::move(output));
    }
    result.pushKV("wallet_returns", std::move(wallet_returns));
    if (!recovery.capacity_snapshot.snapshot_id.IsNull()) {
        result.pushKV("capacity_snapshot_id",
                      recovery.capacity_snapshot.snapshot_id.GetHex());
        result.pushKV("capacity_resource_commitment",
                      recovery.capacity_snapshot.resource_commitment.GetHex());
    }
    if (!recovery.recovery_authorization.authorization_commitment.IsNull()) {
        result.pushKV(
            "authorization_commitment",
            recovery.recovery_authorization.authorization_commitment.GetHex());
        result.pushKV(
            "authorization_accepted",
            !recovery.provider_side &&
                recovery.accepted_recovery_authorization_commitment ==
                    recovery.recovery_authorization.authorization_commitment &&
                recovery.recovery_authorization_accepted_at > 0);
        result.pushKV("maximum_service_fee_cents",
                      recovery.recovery_authorization.maximum_service_fee.value);
        result.pushKV("service_fee_cents",
                      recovery.recovery_authorization.service_fee.value);
        result.pushKV("network_fee_satoshis",
                      recovery.recovery_authorization.network_fee.value);
    } else {
        result.pushKV("maximum_service_fee_cents",
                      recovery.selected_maximum_service_fee.value);
        result.pushKV("service_fee_cents",
                      recovery.selected_service_fee.value);
    }
    if (recovery.recovery_authorization_accepted_at > 0) {
        result.pushKV("authorization_accepted_at",
                      recovery.recovery_authorization_accepted_at);
    }
    if (!recovery.final_transaction.empty()) {
        result.pushKV("raw_transaction", HexStr(recovery.final_transaction));
        if (recovery.signed_result.txid) {
            result.pushKV("txid", recovery.signed_result.txid->GetHex());
        }
        result.pushKV("wtxid", recovery.expected_wtxid.GetHex());
    }
    result.pushKV("expires_at",
                  recovery.phase >= AlternativeRecoveryPhase::RESPONSE_VALIDATED ? recovery.recovery_response.expires_at : recovery.capacity_request.expires_at);
    return result;
}

bool BuildRecoveryParametersFromRecord(
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    DigiDollar::Paymaster::AlternativeRecoveryParameters& parameters,
    std::string& error)
{
    // Keep RPC, background recovery and restart recovery on the same ownership
    // and immutable-template firewall. In particular, the shared builder also
    // proves that every locally supplied provider input and every wallet-return
    // script belongs to the wallet before either side can sign.
    return BuildAlternativeRecoveryParametersFromRecord(
        wallet, recovery, parameters, error);
}

bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error);

bool HasDurableClientAuthorization(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t observation_time)
{
    using namespace DigiDollar::Paymaster;
    // A persisted signature or later attempt state is never a substitute for
    // the wallet's durable acceptance of this exact, current manifest.
    return attempt.client_manifest.version ==
               ClientAuthorizationManifest::CURRENT_VERSION &&
           !attempt.client_manifest.manifest_id.IsNull() &&
           !attempt.accepted_client_manifest_id.IsNull() &&
           attempt.accepted_client_manifest_id ==
               attempt.client_manifest.manifest_id &&
           attempt.created_at > 0 &&
           attempt.client_manifest_accepted_at > 0 &&
           attempt.client_manifest_accepted_at >= attempt.created_at &&
           attempt.client_manifest_accepted_at <= observation_time &&
           attempt.client_manifest_accepted_at <= attempt.updated_at &&
           attempt.retry_until > 0 &&
           attempt.client_manifest_accepted_at <= attempt.retry_until &&
           attempt.client_manifest_accepted_at <=
               attempt.client_manifest.expires_at &&
           attempt.capacity_snapshot.version ==
               ValidatedCapacitySnapshot::CURRENT_VERSION &&
           attempt.capacity_snapshot.validated_at > 0 &&
           attempt.capacity_snapshot.expires_at > 0 &&
           attempt.client_manifest_accepted_at >=
               attempt.capacity_snapshot.validated_at &&
           attempt.client_manifest_accepted_at <=
               attempt.capacity_snapshot.expires_at;
}

bool QueuePersistedPaymasterSubmit(
    WalletContext& context,
    CWallet& wallet,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    PaymasterSubmitQueueState& state,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    state = {};
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "PAYMASTER_DISABLED";
        return false;
    }
    if (attempt.user_signed_psbt.empty() || attempt.retry_until <= 0 ||
        now > attempt.retry_until) {
        error = "PAYMASTER_RETRY_UNAVAILABLE";
        return false;
    }
    PaymentIntent authorized_intent;
    PaymasterQuote authorized_quote;
    CollaborativePSBTTemplate authorized_template;
    if (!HasDurableClientAuthorization(attempt, now) ||
        attempt.client_manifest_accepted_at > session.updated_at) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
        return false;
    }
    if (!LoadAttemptAuthorizationArtifacts(
            attempt, authorized_intent, authorized_quote, authorized_template, error) ||
        now > attempt.client_manifest.expires_at ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, authorized_intent, authorized_quote,
            attempt.capacity_snapshot, authorized_template, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }

    // A persisted USER signature is not authority to reuse stale or replaced
    // provider liquidity. Reconstruct the exact capacity artifacts and apply
    // the same identity/control-proof/chainstate firewall used immediately
    // before the first signature on every retry.
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    try {
        SpanReader request_stream{::PROTOCOL_VERSION,
                                  attempt.capacity_request};
        request_stream >> capacity_request;
        SpanReader proof_stream{
            ::PROTOCOL_VERSION,
            attempt.capacity_snapshot.capacity_proof};
        proof_stream >> capacity_proof;
        if (!request_stream.empty() || !proof_stream.empty()) {
            throw std::ios_base::failure(
                "trailing persisted capacity artifact data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_ARTIFACT_ENCODING";
        return false;
    }
    CDataStream canonical_request{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_request << capacity_request;
    CDataStream canonical_proof{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_proof << capacity_proof;
    const auto canonical_request_span = MakeUCharSpan(canonical_request);
    const auto canonical_proof_span = MakeUCharSpan(canonical_proof);
    if (std::vector<unsigned char>{canonical_request_span.begin(),
                                   canonical_request_span.end()} !=
            attempt.capacity_request ||
        std::vector<unsigned char>{canonical_proof_span.begin(),
                                   canonical_proof_span.end()} !=
            attempt.capacity_snapshot.capacity_proof ||
        Hash(attempt.capacity_request) !=
            attempt.capacity_snapshot.request_hash) {
        error = "PAYMASTER_CAPACITY_ARTIFACT_NONCANONICAL";
        return false;
    }
    node::NodeContext* node = wallet.chain().context();
    const int64_t observation_time =
        std::max(now, attempt.client_manifest_accepted_at);
    if (!node || !node->chainman ||
        !ValidateAuthorizedCapacityRetryAgainstChainstate(
            capacity_proof, capacity_request,
            attempt.provider_identity_key, *node->chainman,
            attempt.client_manifest_accepted_at, observation_time,
            AuthorizedCapacityResourceMode::REQUIRE_UNSPENT, error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_NOT_CURRENT";
        return false;
    }
    PartiallySignedTransaction persisted_user_psbt;
    if (!DecodeRawPSBT(persisted_user_psbt,
                       MakeByteSpan(attempt.user_signed_psbt), error) ||
        !ValidateCollaborativePSBT(
            persisted_user_psbt, authorized_template,
            CollaborativeSignatureStage::USER_SIGNED, error)) {
        if (error.empty()) error = "PAYMASTER_PERSISTED_USER_PSBT_INVALID";
        return false;
    }
    CDataStream canonical_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_stream << persisted_user_psbt;
    const auto canonical_span = MakeUCharSpan(canonical_stream);
    if (std::vector<unsigned char>{canonical_span.begin(), canonical_span.end()} !=
        attempt.user_signed_psbt) {
        error = "PAYMASTER_PERSISTED_USER_PSBT_NONCANONICAL";
        return false;
    }
    if (!ValidateClientAuthorizationOwnership(
            wallet, attempt.client_manifest, error)) {
        return false;
    }

    PaymasterSubmit submit;
    submit.genesis_hash = Params().GenesisBlock().GetHash();
    submit.provider_id = attempt.provider_id;
    submit.request_id = session.request_id;
    submit.session_id = session.session_id;
    submit.quote_id = attempt.quote_id;
    submit.commit_key = attempt.commit_key;
    submit.template_commitment = attempt.template_commitment;
    submit.user_psbt = attempt.user_signed_psbt;
    if (!ValidateSubmitEnvelope(submit, submit.genesis_hash, error)) return false;

    CService endpoint;
    const bool allow_local_endpoint{
        Params().GetChainType() == ChainType::REGTEST};
    if (!attempt.provider_endpoint.empty()) {
        endpoint = LookupNumeric(attempt.provider_endpoint);
        state.route_available = IsEndpointAllowedForPrivacyProfile(
            endpoint, attempt.privacy_profile, allow_local_endpoint);
    }
    if (!state.route_available) {
        for (const Announcement& announcement : context.paymaster->GetDirectory().List(now)) {
            if (GetPaymasterId(announcement.identity_key) == attempt.provider_id &&
                IsEndpointAllowedForPrivacyProfile(
                    announcement.endpoint, attempt.privacy_profile,
                    allow_local_endpoint)) {
                endpoint = announcement.endpoint;
                state.route_available = true;
                break;
            }
        }
    }
    if (!state.route_available) return true;

    if (!node || !node->connman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    NodeId peer_id{-1};
    node->connman->ForEachNode([&](CNode* peer) {
        if (peer_id == -1 && peer->IsPaymasterConn() && peer->addr == endpoint) {
            peer_id = peer->GetId();
        }
    });
    if (peer_id == -1) {
        state.connection_pending = node->connman->AddConnection(
            endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
            /*paymaster_high_privacy=*/attempt.privacy_profile == PrivacyProfile::HIGH);
        return true;
    }

    const std::vector<unsigned char> submit_bytes = SerializeSubmit(submit);
    const uint256 message_id = Hash(submit_bytes);
    state.queued = context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                   context.paymaster->QueueOutboundDirectMessage(
                       peer_id, message_id, submit_bytes.size(), DirectPayload{submit}, now);
    if (!state.queued) {
        error = "PAYMASTER_DIRECT_QUEUE_FULL";
        return false;
    }
    node->connman->WakeMessageHandler();
    return true;
}

std::vector<unsigned char> SerializePaymentIntent(
    const DigiDollar::Paymaster::PaymentIntent& intent)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << intent;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

// -------------------------------------------------------------------------
// Final transaction validation and broadcast
// -------------------------------------------------------------------------
// The persisted PSBT/template is the trust anchor. Peer-returned raw
// transactions are inserted into that template and fully checked before the
// wallet records a terminal state or broadcasts anything.
bool LoadTrustedTemplate(const DigiDollar::Paymaster::ProviderAttempt& attempt,
                         DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
                         std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (!DecodeRawPSBT(trusted.psbt, MakeByteSpan(attempt.unsigned_psbt), error)) return false;
    if (!trusted.psbt.tx || trusted.psbt.inputs.size() != attempt.input_roles.size()) {
        error = "PAYMASTER_PERSISTED_TEMPLATE_SHAPE";
        return false;
    }
    static_assert(static_cast<uint8_t>(ReservationRole::USER_DD) == static_cast<uint8_t>(InputRole::USER_DD));
    static_assert(static_cast<uint8_t>(ReservationRole::USER_DGB) == static_cast<uint8_t>(InputRole::USER_DGB));
    static_assert(static_cast<uint8_t>(ReservationRole::PROVIDER_CARRIER) == static_cast<uint8_t>(InputRole::PROVIDER_CARRIER));
    static_assert(static_cast<uint8_t>(ReservationRole::PROVIDER_DGB) == static_cast<uint8_t>(InputRole::PROVIDER_DGB));
    trusted.input_roles.reserve(attempt.input_roles.size());
    for (const ReservationRole role : attempt.input_roles) {
        trusted.input_roles.push_back(static_cast<InputRole>(role));
    }
    return true;
}

bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    try {
        CDataStream request_stream{attempt.quote_request, SER_NETWORK,
                                   ::PROTOCOL_VERSION};
        request_stream >> request;
        SpanReader response_stream{::PROTOCOL_VERSION, attempt.signed_quote};
        response_stream >> response;
        if (!request_stream.empty() || !response_stream.empty()) {
            throw std::ios_base::failure("trailing authorization artifact data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_AUTHORIZATION_ARTIFACT_ENCODING";
        return false;
    }
    if (request.intent.request_id != response.request_id ||
        request.intent.session_id != response.session_id ||
        request.intent.provider_id != response.quote.provider_id ||
        GetPaymentIntentHash(request.intent) != response.quote.intent_hash ||
        response.quote.quote_id != attempt.quote_id ||
        response.quote.template_commitment != attempt.template_commitment ||
        response.quote.unsigned_txid != attempt.unsigned_txid) {
        error = "PAYMASTER_AUTHORIZATION_ARTIFACT_CONFLICT";
        return false;
    }
    if (!LoadTrustedTemplate(attempt, trusted, error)) return false;
    intent = std::move(request.intent);
    quote = std::move(response.quote);
    return true;
}

bool ValidateDurableProviderUserAuthorization(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    UserAuthorizationRecord authorization;
    if (attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
        attempt.commit_key.IsNull() || attempt.user_signed_psbt.empty() ||
        !store.GetUserAuthorization(attempt.commit_key, authorization) ||
        authorization.version != UserAuthorizationRecord::CURRENT_VERSION ||
        authorization.commit_key != attempt.commit_key ||
        authorization.attempt_id != attempt.attempt_id ||
        authorization.canonical_psbt_hash !=
            Hash(attempt.user_signed_psbt) ||
        authorization.accepted_at <= 0 ||
        authorization.retry_until != attempt.retry_until) {
        error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
        return false;
    }
    return true;
}

bool FindDurableClientAuthorizationAttempt(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    int64_t observation_time,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (session.provider_side) return false;
    for (auto it = session.attempt_ids.rbegin();
         it != session.attempt_ids.rend(); ++it) {
        ProviderAttempt candidate;
        if (!store.GetAttempt(*it, candidate)) {
            error = "PAYMASTER_ATTEMPT_NOT_FOUND";
            return false;
        }
        if (!HasDurableClientAuthorization(candidate, observation_time) ||
            candidate.client_manifest_accepted_at > session.updated_at) {
            const bool has_authorization_evidence =
                !candidate.accepted_client_manifest_id.IsNull() ||
                candidate.client_manifest_accepted_at != 0 ||
                !candidate.user_signed_psbt.empty() ||
                (candidate.state >= AttemptState::USER_SIGNED &&
                 candidate.state <= AttemptState::MEMPOOL) ||
                candidate.state == AttemptState::AMBIGUOUS;
            if (has_authorization_evidence) {
                error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
                return false;
            }
            continue;
        }
        CollaborativePSBTTemplate trusted;
        if (!LoadAttemptAuthorizationArtifacts(
                candidate, intent, quote, trusted, error) ||
            !ValidateClientAuthorizationManifest(
                candidate.client_manifest, intent, quote,
                candidate.capacity_snapshot, trusted, error) ||
            !ValidateClientAuthorizationOwnership(
                wallet, candidate.client_manifest, error)) {
            if (error.empty()) {
                error = "PAYMASTER_CLIENT_AUTHORIZATION_INVALID";
            }
            return false;
        }
        attempt = std::move(candidate);
        return true;
    }
    error.clear();
    return false;
}

UniValue DurableClientAuthorizationToJSON(
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    const DigiDollar::Paymaster::PaymentIntent& intent,
    const DigiDollar::Paymaster::PaymasterQuote& quote)
{
    using namespace DigiDollar::Paymaster;
    UniValue result = SessionToJSON(session);
    result.pushKV("provider_id", attempt.provider_id.GetHex());
    result.pushKV("offer_id", intent.offer_id.GetHex());
    result.pushKV("policy_hash", intent.policy_hash.GetHex());
    result.pushKV("funding_model",
                  intent.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
    result.pushKV("payment_cents", intent.recipient_amount.value);
    result.pushKV("service_fee_cents", quote.service_fee.value);
    result.pushKV("user_total_cents",
                  intent.recipient_amount.value + quote.service_fee.value);
    if (const auto address = DigiDollarAddressForScript(
            intent.recipient_script)) {
        result.pushKV("to_address", *address);
    }
    result.pushKV("expires_at", intent.expires_at);
    result.pushKV("queued", false);
    result.pushKV("connection_pending", false);
    result.pushKV("capacity_pending", false);
    result.pushKV("capacity_snapshot_id",
                  attempt.capacity_snapshot.snapshot_id.GetHex());
    result.pushKV("quote_id", attempt.quote_id.GetHex());
    result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
    result.pushKV("template_commitment",
                  attempt.template_commitment.GetHex());
    result.pushKV("authorization_commitment",
                  attempt.client_manifest.manifest_id.GetHex());
    const bool authorization_accepted =
        attempt.accepted_client_manifest_id ==
            attempt.client_manifest.manifest_id &&
        attempt.client_manifest_accepted_at > 0;
    result.pushKV("authorization_accepted", authorization_accepted);
    if (authorization_accepted) {
        result.pushKV("authorization_accepted_at",
                      attempt.client_manifest_accepted_at);
    }
    result.pushKV("psbt", EncodeBase64(attempt.unsigned_psbt));
    return result;
}

bool GetValidatedClientAuthorizationCommitment(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    uint256& commitment,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymentIntent intent;
    PaymasterQuote quote;
    CollaborativePSBTTemplate trusted;
    if (!LoadAttemptAuthorizationArtifacts(attempt, intent, quote, trusted, error) ||
        attempt.client_manifest.manifest_id.IsNull() ||
        (attempt.state == AttemptState::QUOTED && now > attempt.quote_expires_at) ||
        now > attempt.client_manifest.expires_at ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, intent, quote, attempt.capacity_snapshot,
            trusted, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }
    commitment = attempt.client_manifest.manifest_id;
    return true;
}

std::vector<unsigned char> SerializePSBT(const PartiallySignedTransaction& psbt)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << psbt;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeTransaction(const CMutableTransaction& transaction)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << transaction;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

bool PreflightFinalPaymasterTransaction(CWallet& wallet,
                                        const CTransactionRef& transaction,
                                        FinalTransactionPresence& presence,
                                        std::string& error,
                                        ExactFinalTxIndexMode txindex_mode)
{
    ExactFinalTransactionPreflight preflight;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, transaction, txindex_mode,
            preflight, error)) {
        presence = FinalTransactionPresence::NONE;
        return false;
    }
    presence = preflight.presence;
    return true;
}

bool MarkFinalValidationFailureRecoverable(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    std::string& error)
{
    return store.MarkClientFinalValidationFailureRecoverable(
        session.request_id, attempt.attempt_id, now, error);
}

std::vector<COutPoint> ProviderInputs(const CMutableTransaction& transaction,
                                      const std::vector<DigiDollar::Paymaster::ReservationRole>& roles)
{
    using namespace DigiDollar::Paymaster;
    std::vector<COutPoint> result;
    if (transaction.vin.size() != roles.size()) return result;
    for (size_t index = 0; index < roles.size(); ++index) {
        if (roles[index] == ReservationRole::PROVIDER_CARRIER ||
            roles[index] == ReservationRole::PROVIDER_DGB) {
            result.push_back(transaction.vin[index].prevout);
        }
    }
    return result;
}

bool TemplateInputsAvailable(CWallet& wallet, const CMutableTransaction& transaction)
{
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& input : transaction.vin)
        coins.emplace(input.prevout, Coin{});
    wallet.chain().findCoins(coins);
    return std::all_of(coins.begin(), coins.end(), [](const auto& entry) {
        return !entry.second.IsSpent();
    });
}

bool RejectUnavailableTemplateInputs(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    const CMutableTransaction& transaction,
    int64_t now,
    bool& rejected,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    rejected = false;
    if (attempt.state == AttemptState::REJECTED) {
        if (!store.GetProviderResult(attempt.commit_key, result) ||
            result.status != PaymasterResultStatus::REJECTED) {
            error = "PAYMASTER_REJECTED_RESULT_MISSING";
            return false;
        }
        rejected = true;
        return true;
    }
    if (attempt.state != AttemptState::QUOTED &&
        attempt.state != AttemptState::USER_SIGNED &&
        attempt.state != AttemptState::USER_PSBT_ACCEPTED) {
        return true;
    }
    if (TemplateInputsAvailable(wallet, transaction)) return true;

    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != attempt.provider_id ||
        identity.identity_key != attempt.provider_identity_key) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = attempt.provider_id;
    result.commit_key = attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::REJECTED;
    result.updated_at = now;
    result.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    if (!store.RejectUnavailableProviderSubmit(
            attempt.attempt_id, result, result.genesis_hash, attempt, error)) {
        return false;
    }
    rejected = true;
    return true;
}

bool BuildFinalProviderResult(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    result = PaymasterResult{};
    CMutableTransaction transaction;
    try {
        SpanReader stream{::PROTOCOL_VERSION, commit.final_transaction};
        stream >> transaction;
        if (!stream.empty()) {
            error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }
    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != commit.provider_id) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = commit.provider_id;
    result.commit_key = commit.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = CTransaction{transaction}.GetHash();
    result.raw_transaction_hash = CTransaction{transaction}.GetWitnessHash();
    result.final_transaction = transaction;
    result.updated_at = now;
    result.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    error.clear();
    return true;
}

bool GetOrCreateFinalProviderResult(CWallet& wallet,
                                    PaymasterStore& store,
                                    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
                                    int64_t now,
                                    DigiDollar::Paymaster::PaymasterResult& result,
                                    std::string& error)
{
    if (store.GetProviderResult(commit.commit_key, result)) return true;
    if (!BuildFinalProviderResult(wallet, commit, now, result, error)) return false;
    return store.StoreProviderResult(result, result.genesis_hash, error);
}

bool InsertAndBroadcastPaymasterTransaction(CWallet& wallet,
                                            const CTransactionRef& transaction,
                                            bool& already_confirmed,
                                            std::string& error)
{
    return InsertAndBroadcastExactPaymasterTransaction(
        wallet, transaction, already_confirmed, error);
}

// Wallet insertion precedes network broadcast so restart recovery can find the
// exact authorized transaction even if the process exits between the two.
bool InsertAndBroadcastProviderCommit(CWallet& wallet,
                                      PaymasterStore& store,
                                      const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    bool& already_confirmed,
    std::string& error,
    CTransactionRef* exact_transaction)
{
    using namespace DigiDollar::Paymaster;
    ProviderAttempt attempt;
    if (!store.GetAttemptByTemplateCommitment(commit.template_commitment, attempt)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    CMutableTransaction decoded;
    if (!ValidateProviderCommitForExecution(attempt, commit, now, decoded, error) ||
        !store.ValidateProviderBudgetAuthorization(
            attempt, BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error)) {
        return false;
    }
    if (attempt.provider_manifest.manifest_id.IsNull() ||
        !ValidateProviderAuthorizationOwnership(
            wallet, attempt.provider_manifest, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED";
        }
        return false;
    }
    const CTransactionRef transaction = MakeTransactionRef(decoded);
    if (exact_transaction) *exact_transaction = transaction;
    FinalTransactionPresence presence{FinalTransactionPresence::NONE};
    if (!PreflightFinalPaymasterTransaction(wallet, transaction, presence, error)) {
        return false;
    }
    return InsertAndBroadcastPaymasterTransaction(
        wallet, transaction, already_confirmed, error);
}

bool MarkProviderResultBroadcastAttempted(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::PaymasterResult& result,
    int64_t now,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (result.status == PaymasterResultStatus::BROADCAST_ATTEMPTED) return true;
    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != result.provider_id) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.status = PaymasterResultStatus::BROADCAST_ATTEMPTED;
    ++result.result_sequence;
    result.updated_at = std::max(result.updated_at, now);
    result.identity_signature.assign(64, 0);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    return store.StoreProviderResult(result, result.genesis_hash, error);
}

bool IsProviderNetworkStateAtLeast(
    DigiDollar::Paymaster::AttemptState state,
    DigiDollar::Paymaster::AttemptState minimum)
{
    using DigiDollar::Paymaster::AttemptState;
    switch (minimum) {
    case AttemptState::BROADCAST:
        return state == AttemptState::BROADCAST ||
               state == AttemptState::STEMPOOL ||
               state == AttemptState::MEMPOOL;
    case AttemptState::STEMPOOL:
        return state == AttemptState::STEMPOOL ||
               state == AttemptState::MEMPOOL;
    case AttemptState::MEMPOOL:
        return state == AttemptState::MEMPOOL;
    default:
        return false;
    }
}

bool IsProviderSessionNetworkStateAtLeast(
    DigiDollar::Paymaster::SessionState state,
    DigiDollar::Paymaster::SessionState minimum)
{
    using DigiDollar::Paymaster::SessionState;
    switch (minimum) {
    case SessionState::STEMPOOL:
        return state == SessionState::STEMPOOL ||
               state == SessionState::MEMPOOL ||
               state == SessionState::CONFIRMED;
    case SessionState::MEMPOOL:
        return state == SessionState::MEMPOOL ||
               state == SessionState::CONFIRMED;
    case SessionState::CONFIRMED:
        return state == SessionState::CONFIRMED;
    default:
        return false;
    }
}

bool ReloadExactProviderCommitAttempt(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    const uint256& expected_attempt_id,
    const uint256& expected_session_id,
    int64_t now,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    ProviderAttempt reloaded;
    CMutableTransaction validated;
    if (!store.GetAttemptByTemplateCommitment(
            commit.template_commitment, reloaded)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (reloaded.attempt_id != expected_attempt_id ||
        reloaded.session_id != expected_session_id ||
        !ValidateProviderCommitForExecution(
            reloaded, commit, now, validated, error) ||
        !store.ValidateProviderBudgetAuthorization(
            reloaded, BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error)) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return false;
    }
    attempt = std::move(reloaded);
    return true;
}

bool ReloadExactProviderCommitSession(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    const std::string& expected_request_id,
    const uint256& expected_session_id,
    const uint256& expected_attempt_id,
    DigiDollar::Paymaster::PaymentSession& session,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymentSession reloaded;
    if (!store.GetSessionBySessionId(expected_session_id, reloaded)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (reloaded.request_id != expected_request_id ||
        reloaded.session_id != expected_session_id ||
        !reloaded.provider_side ||
        (!reloaded.final_txid.IsNull() &&
         reloaded.final_txid != commit.final_txid) ||
        std::find(reloaded.attempt_ids.begin(), reloaded.attempt_ids.end(),
                  expected_attempt_id) == reloaded.attempt_ids.end()) {
        error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return false;
    }
    session = std::move(reloaded);
    return true;
}

ProviderCommitRecoveryResult RecoverProviderCommit(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    ProviderCommitRecoveryResult result;
    ProviderCommitRecord persisted_commit;
    if (commit.commit_key.IsNull() ||
        !store.GetProviderCommit(commit.commit_key, persisted_commit) ||
        persisted_commit.version != commit.version ||
        persisted_commit.commit_key != commit.commit_key ||
        persisted_commit.provider_id != commit.provider_id ||
        persisted_commit.quote_id != commit.quote_id ||
        persisted_commit.template_commitment != commit.template_commitment ||
        persisted_commit.final_txid != commit.final_txid ||
        persisted_commit.raw_transaction_hash != commit.raw_transaction_hash ||
        persisted_commit.final_transaction != commit.final_transaction ||
        persisted_commit.provider_inputs != commit.provider_inputs ||
        persisted_commit.committed_at != commit.committed_at ||
        persisted_commit.retry_until != commit.retry_until) {
        result.error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return result;
    }
    ProviderAttempt attempt;
    if (!store.GetAttemptByTemplateCommitment(commit.template_commitment, attempt)) {
        result.error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return result;
    }
    PaymentSession session;
    if (!store.GetSessionBySessionId(attempt.session_id, session)) {
        result.error = "PAYMASTER_SESSION_NOT_FOUND";
        return result;
    }
    // The caller supplied the exact durable ProviderCommitRecord. The
    // broadcast helper reloads its current attempt and applies the complete
    // commit, manifest, ownership, and budget firewall.
    result.broadcast = InsertAndBroadcastProviderCommit(
        wallet, store, commit, now, result.already_confirmed, result.error);
    if (!result.broadcast) return result;

    const uint256 expected_attempt_id{attempt.attempt_id};
    const uint256 expected_session_id{attempt.session_id};
    if (!ReloadExactProviderCommitAttempt(
            store, commit, expected_attempt_id, expected_session_id, now,
            attempt, result.error)) {
        return result;
    }
    const auto advance_attempt = [&](AttemptState target) {
        if (IsProviderNetworkStateAtLeast(attempt.state, target)) return true;

        ProviderAttempt update{attempt};
        update.state = target;
        update.updated_at = std::max(update.updated_at, now);
        std::string update_error;
        if (store.UpdateAttempt(session.request_id, update, update_error)) {
            attempt = std::move(update);
            return true;
        }
        if (update_error != "PAYMASTER_INVALID_ATTEMPT_TRANSITION") {
            result.error = std::move(update_error);
            return false;
        }

        // Broadcast can synchronously notify the wallet. That callback may
        // atomically advance the durable attempt to STEMPOOL/MEMPOOL while
        // this RPC still holds its pre-callback copy. Treat the stale update
        // as idempotent only after reloading and revalidating the exact commit
        // binding, and only when the callback reached this target or a later
        // network-observation state.
        ProviderAttempt raced;
        std::string reload_error;
        if (!ReloadExactProviderCommitAttempt(
                store, commit, expected_attempt_id, expected_session_id, now,
                raced, reload_error)) {
            result.error = std::move(reload_error);
            return false;
        }
        if (!IsProviderNetworkStateAtLeast(raced.state, target)) {
            result.error = std::move(update_error);
            return false;
        }
        attempt = std::move(raced);
        return true;
    };
    if (!advance_attempt(AttemptState::BROADCAST)) return result;
    if (!result.already_confirmed &&
        !advance_attempt(gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION) ? AttemptState::STEMPOOL : AttemptState::MEMPOOL)) {
        return result;
    }
    const SessionState session_state = result.already_confirmed ? SessionState::CONFIRMED : (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION) ? SessionState::STEMPOOL : SessionState::MEMPOOL);
    if (!ReloadExactProviderCommitSession(
            store, commit, session.request_id, expected_session_id,
            expected_attempt_id, session, result.error)) {
        return result;
    }
    if (session.final_txid != commit.final_txid ||
        !IsProviderSessionNetworkStateAtLeast(session.state, session_state)) {
        std::string transition_error;
        if (!store.TransitionSession(
                session.request_id, session_state, PendingPhase::NONE,
                commit.final_txid, now, transition_error)) {
            if (transition_error != "PAYMASTER_INVALID_SESSION_TRANSITION") {
                result.error = std::move(transition_error);
                return result;
            }
            PaymentSession raced;
            std::string reload_error;
            if (!ReloadExactProviderCommitSession(
                    store, commit, session.request_id, expected_session_id,
                    expected_attempt_id, raced, reload_error)) {
                result.error = std::move(reload_error);
                return result;
            }
            if (raced.final_txid != commit.final_txid ||
                !IsProviderSessionNetworkStateAtLeast(
                    raced.state, session_state)) {
                result.error = std::move(transition_error);
                return result;
            }
            session = std::move(raced);
        }
    }
    if (!GetOrCreateFinalProviderResult(
            wallet, store, commit, now, result.provider_result, result.error)) {
        return result;
    }
    std::string result_error;
    if (!MarkProviderResultBroadcastAttempted(
            wallet, store, result.provider_result, now, result_error)) {
        result.error = result_error;
    }
    return result;
}

// -------------------------------------------------------------------------
// Strict policy parsing
// -------------------------------------------------------------------------
// Parsers reject unknown or nonsensical values rather than inheriting permissive
// defaults. In particular, zero safety budgets mean disabled, never unlimited.
UniValue ProviderPolicyToJSON(const DigiDollar::Paymaster::ProviderPolicy& policy)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    UniValue models{UniValue::VARR};
    if (PolicyAllowsFundingModel(policy, FundingModel::SPONSORED)) models.push_back("sponsored");
    if (PolicyAllowsFundingModel(policy, FundingModel::USER_PAID)) models.push_back("user_paid");
    result.pushKV("funding_models", std::move(models));
    result.pushKV("sponsorship_scope",
                  policy.sponsorship_scope == SponsorshipScope::PUBLIC ? "public" : "restricted");
    result.pushKV("fee_rate_bps", policy.fee_rate_bps);
    result.pushKV("min_amount_cents", policy.min_payment.value);
    result.pushKV("max_amount_cents", policy.max_payment.value);
    result.pushKV("quote_ttl", policy.quote_ttl);
    result.pushKV("maximum_network_fee_dgb_satoshis", policy.maximum_network_fee.value);
    result.pushKV("policy_hash", GetProviderPolicyHash(policy).GetHex());
    return result;
}

DigiDollar::Paymaster::ProviderPolicy ParseProviderPolicy(const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(value,
                    {{"funding_models", UniValueType(UniValue::VARR)},
                     {"sponsorship_scope", UniValueType(UniValue::VSTR)},
                     {"fee_rate_bps", UniValueType(UniValue::VNUM)},
                     {"min_amount_cents", UniValueType(UniValue::VNUM)},
                     {"max_amount_cents", UniValueType(UniValue::VNUM)},
                     {"quote_ttl", UniValueType(UniValue::VNUM)},
                     {"maximum_network_fee_dgb_satoshis", UniValueType(UniValue::VNUM)}},
                    /*fAllowNull=*/false, /*fStrict=*/true);
    ProviderPolicy policy;
    for (const UniValue& model : value.find_value("funding_models").get_array().getValues()) {
        if (!model.isStr()) throw JSONRPCError(RPC_INVALID_PARAMETER, "funding_models entries must be strings");
        uint8_t bit{0};
        if (model.get_str() == "sponsored")
            bit = FUNDING_MODEL_SPONSORED;
        else if (model.get_str() == "user_paid")
            bit = FUNDING_MODEL_USER_PAID;
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown Paymaster funding model");
        if ((policy.funding_models & bit) != 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Duplicate Paymaster funding model");
        }
        policy.funding_models |= bit;
    }
    const std::string scope = value.find_value("sponsorship_scope").get_str();
    if (scope == "public")
        policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    else if (scope == "restricted")
        policy.sponsorship_scope = SponsorshipScope::RESTRICTED;
    else
        throw JSONRPCError(RPC_INVALID_PARAMETER, "sponsorship_scope must be public or restricted");
    policy.fee_rate_bps = value.find_value("fee_rate_bps").getInt<uint32_t>();
    policy.min_payment = DDCents{value.find_value("min_amount_cents").getInt<int64_t>()};
    policy.max_payment = DDCents{value.find_value("max_amount_cents").getInt<int64_t>()};
    policy.quote_ttl = value.find_value("quote_ttl").getInt<int64_t>();
    policy.maximum_network_fee = DGBSatoshis{
        value.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>()};
    std::string error;
    if (!ValidateProviderPolicy(policy, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    return policy;
}

UniValue FundingSafetyLimitsToJSON(
    const DigiDollar::Paymaster::FundingSafetyLimits& limits)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_network_fee_per_transaction_satoshis",
                  limits.maximum_network_fee_per_transaction.value);
    result.pushKV("maximum_reserved_network_fee_satoshis",
                  limits.maximum_reserved_network_fee.value);
    result.pushKV("maximum_network_fee_per_hour_satoshis",
                  limits.maximum_network_fee_per_hour.value);
    result.pushKV("maximum_network_fee_per_day_satoshis",
                  limits.maximum_network_fee_per_day.value);
    result.pushKV("maximum_completed_per_hour", limits.maximum_completed_per_hour);
    result.pushKV("maximum_completed_per_day", limits.maximum_completed_per_day);
    return result;
}

DigiDollar::Paymaster::FundingSafetyLimits ParseFundingSafetyLimits(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"maximum_network_fee_per_transaction_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_reserved_network_fee_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_network_fee_per_hour_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_network_fee_per_day_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_completed_per_hour", UniValueType(UniValue::VNUM)},
         {"maximum_completed_per_day", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{
        value.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>()};
    limits.maximum_reserved_network_fee = DGBSatoshis{
        value.find_value("maximum_reserved_network_fee_satoshis").getInt<int64_t>()};
    limits.maximum_network_fee_per_hour = DGBSatoshis{
        value.find_value("maximum_network_fee_per_hour_satoshis").getInt<int64_t>()};
    limits.maximum_network_fee_per_day = DGBSatoshis{
        value.find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>()};
    limits.maximum_completed_per_hour =
        value.find_value("maximum_completed_per_hour").getInt<uint32_t>();
    limits.maximum_completed_per_day =
        value.find_value("maximum_completed_per_day").getInt<uint32_t>();
    return limits;
}

UniValue ProviderSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("user_paid", FundingSafetyLimitsToJSON(policy.user_paid));
    result.pushKV("public_sponsored", FundingSafetyLimitsToJSON(policy.public_sponsored));
    result.pushKV("restricted_sponsored", FundingSafetyLimitsToJSON(policy.restricted_sponsored));
    result.pushKV("maximum_active_quotes_total", policy.maximum_active_quotes_total);
    result.pushKV("maximum_active_quotes_per_netgroup",
                  policy.maximum_active_quotes_per_netgroup);
    result.pushKV("maximum_active_quotes_per_recipient",
                  policy.maximum_active_quotes_per_recipient);
    result.pushKV("maximum_quote_requests_per_netgroup_per_minute",
                  policy.maximum_quote_requests_per_netgroup_per_minute);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

DigiDollar::Paymaster::ProviderSafetyPolicy ParseProviderSafetyPolicy(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"user_paid", UniValueType(UniValue::VOBJ)},
         {"public_sponsored", UniValueType(UniValue::VOBJ)},
         {"restricted_sponsored", UniValueType(UniValue::VOBJ)},
         {"maximum_active_quotes_total", UniValueType(UniValue::VNUM)},
         {"maximum_active_quotes_per_netgroup", UniValueType(UniValue::VNUM)},
         {"maximum_active_quotes_per_recipient", UniValueType(UniValue::VNUM)},
         {"maximum_quote_requests_per_netgroup_per_minute", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    ProviderSafetyPolicy policy;
    policy.user_paid = ParseFundingSafetyLimits(value.find_value("user_paid"));
    policy.public_sponsored = ParseFundingSafetyLimits(value.find_value("public_sponsored"));
    policy.restricted_sponsored = ParseFundingSafetyLimits(value.find_value("restricted_sponsored"));
    policy.maximum_active_quotes_total =
        value.find_value("maximum_active_quotes_total").getInt<uint32_t>();
    policy.maximum_active_quotes_per_netgroup =
        value.find_value("maximum_active_quotes_per_netgroup").getInt<uint32_t>();
    policy.maximum_active_quotes_per_recipient =
        value.find_value("maximum_active_quotes_per_recipient").getInt<uint32_t>();
    policy.maximum_quote_requests_per_netgroup_per_minute =
        value.find_value("maximum_quote_requests_per_netgroup_per_minute").getInt<uint32_t>();
    return policy;
}

UniValue ProviderSafetyClassStatusToJSON(
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy,
    DigiDollar::Paymaster::FundingModel model,
    DigiDollar::Paymaster::SponsorshipScope scope,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    CScript status_script;
    status_script << OP_TRUE;
    const ProviderSafetyStatus status = EvaluateProviderSafetyStatus(
        ledger, policy, model, scope, GetRecipientBudgetBucket(ledger, status_script),
        DGBSatoshis{1}, now);
    UniValue result{UniValue::VOBJ};
    result.pushKV("reserved_network_fee_satoshis", status.reserved_network_fee.value);
    result.pushKV("spent_network_fee_last_hour_satoshis",
                  status.spent_network_fee_last_hour.value);
    result.pushKV("spent_network_fee_last_day_satoshis",
                  status.spent_network_fee_last_day.value);
    result.pushKV("active_quotes", status.active_quotes);
    result.pushKV("completed_last_hour", status.completed_last_hour);
    result.pushKV("completed_last_day", status.completed_last_day);
    result.pushKV("can_accept_minimum_quote", status.can_accept_quote);
    result.pushKV("errors", ReadinessErrorsToJSON(status.errors));
    return result;
}

UniValue ClientSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_service_fee_per_transaction_cents",
                  policy.maximum_service_fee_per_transaction.value);
    result.pushKV("maximum_service_fee_per_day_cents",
                  policy.maximum_service_fee_per_day.value);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

DigiDollar::Paymaster::ClientSafetyPolicy ParseClientSafetyPolicy(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"maximum_service_fee_per_transaction_cents", UniValueType(UniValue::VNUM)},
         {"maximum_service_fee_per_day_cents", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    ClientSafetyPolicy policy;
    policy.maximum_service_fee_per_transaction = DDCents{
        value.find_value("maximum_service_fee_per_transaction_cents").getInt<int64_t>()};
    policy.maximum_service_fee_per_day = DDCents{
        value.find_value("maximum_service_fee_per_day_cents").getInt<int64_t>()};
    return policy;
}

std::vector<RPCArg> FundingSafetyLimitArgs()
{
    return {
        {"maximum_network_fee_per_transaction_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Maximum miner fee for one transaction; zero only disables an unused model"},
        {"maximum_reserved_network_fee_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Maximum miner fee reserved by concurrent quotes"},
        {"maximum_network_fee_per_hour_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-hour miner-fee ceiling"},
        {"maximum_network_fee_per_day_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-day miner-fee ceiling"},
        {"maximum_completed_per_hour", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-hour transaction ceiling"},
        {"maximum_completed_per_day", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-day transaction ceiling"},
    };
}

std::vector<RPCResult> FundingSafetyLimitResults()
{
    return {
        {RPCResult::Type::NUM, "maximum_network_fee_per_transaction_satoshis", "Per-transaction miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_reserved_network_fee_satoshis", "Concurrent reserved miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_network_fee_per_hour_satoshis", "Rolling-hour miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_network_fee_per_day_satoshis", "Rolling-day miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_completed_per_hour", "Rolling-hour transaction ceiling"},
        {RPCResult::Type::NUM, "maximum_completed_per_day", "Rolling-day transaction ceiling"},
    };
}

std::vector<RPCResult> ProviderSafetyClassStatusResults()
{
    return {
        {RPCResult::Type::NUM, "reserved_network_fee_satoshis", "Miner fee held by active quotes"},
        {RPCResult::Type::NUM, "spent_network_fee_last_hour_satoshis", "Miner fee committed during the rolling hour"},
        {RPCResult::Type::NUM, "spent_network_fee_last_day_satoshis", "Miner fee committed during the rolling day"},
        {RPCResult::Type::NUM, "active_quotes", "All active provider quote reservations"},
        {RPCResult::Type::NUM, "completed_last_hour", "Completed transactions during the rolling hour"},
        {RPCResult::Type::NUM, "completed_last_day", "Completed transactions during the rolling day"},
        {RPCResult::Type::BOOL, "can_accept_minimum_quote", "Whether a one-satoshi hypothetical quote fits the limits"},
        {RPCResult::Type::ARR, "errors", "Safety failures", {{RPCResult::Type::STR, "", "Stable Paymaster safety error"}}},
    };
}

std::vector<RPCResult> ProviderSafetyPolicyResults()
{
    return {
        {RPCResult::Type::OBJ, "user_paid", "USER_PAID loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::OBJ, "public_sponsored", "Public SPONSORED loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::OBJ, "restricted_sponsored", "Restricted SPONSORED loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::NUM, "maximum_active_quotes_total", "Maximum concurrent active quotes"},
        {RPCResult::Type::NUM, "maximum_active_quotes_per_netgroup", "Maximum concurrent quotes per network group"},
        {RPCResult::Type::NUM, "maximum_active_quotes_per_recipient", "Maximum concurrent quotes per pseudonymous recipient bucket"},
        {RPCResult::Type::NUM, "maximum_quote_requests_per_netgroup_per_minute", "Quote request rate per network group"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
    };
}

std::vector<RPCResult> ProviderLiquidityPolicyResults()
{
    return {
        {RPCResult::Type::BOOL, "automatic_replenishment", "Whether automatic maintenance is enabled"},
        {RPCResult::Type::BOOL, "paid_maintenance_approved", "Whether finite paid maintenance was approved"},
        {RPCResult::Type::NUM, "target_admission_dgb", "Target number of admission DGB slots"},
        {RPCResult::Type::NUM, "target_operational_dgb", "Target number of operational DGB slots"},
        {RPCResult::Type::NUM, "target_admission_carriers", "Target number of admission DigiDollar carriers"},
        {RPCResult::Type::NUM, "target_operational_carriers", "Target number of operational DigiDollar carriers"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_transaction_satoshis", "Per-transaction maintenance-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_hour_satoshis", "Rolling-hour maintenance-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_day_satoshis", "Rolling-day maintenance-fee ceiling"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last liquidity-policy update"},
    };
}

std::vector<RPCResult> ProviderLiquiditySlotResults()
{
    return {
        {RPCResult::Type::NUM, "target", "Configured slot target"},
        {RPCResult::Type::NUM, "ready", "Confirmed available slots"},
        {RPCResult::Type::NUM, "pending", "Unconfirmed successor or maintenance slots"},
        {RPCResult::Type::NUM, "counted_toward_target", "Available, reserved, or pending slots counted toward the target"},
        {RPCResult::Type::NUM, "missing", "Slots still missing from the target"},
    };
}

std::vector<RPCResult> ProviderLiquidityStatusResults()
{
    return {
        {RPCResult::Type::BOOL, "policy_configured", "Whether the operator saved the policy"},
        {RPCResult::Type::BOOL, "targets_satisfy_provider_policy", "Whether the saved targets can satisfy the active provider offer"},
        {RPCResult::Type::STR, "maintenance_state", "Current automatic liquidity state"},
        {RPCResult::Type::OBJ, "policy", "Saved or suggested liquidity policy",
         ProviderLiquidityPolicyResults()},
        {RPCResult::Type::OBJ, "admission_dgb", "Admission DGB slot status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "operational_dgb", "Operational DGB slot status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "admission_carriers", "Admission DigiDollar carrier status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "operational_carriers", "Operational DigiDollar carrier status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::NUM, "maintenance_fee_reserved_satoshis", "Maintenance fee exposed by planned or broadcast transactions"},
        {RPCResult::Type::NUM, "maintenance_fee_spent_last_hour_satoshis", "Confirmed maintenance fees in the rolling hour"},
        {RPCResult::Type::NUM, "maintenance_fee_spent_last_day_satoshis", "Confirmed maintenance fees in the rolling day"},
        {RPCResult::Type::NUM, "carrier_base_cents", "DigiDollar base value retained in available carriers"},
        {RPCResult::Type::NUM, "carrier_withdrawable_excess_cents", "DigiDollar carrier value available for excess withdrawal"},
        {RPCResult::Type::ARR, "readiness_errors", "Outstanding liquidity readiness gates",
         {{RPCResult::Type::STR, "", "Stable Paymaster readiness error"}}},
    };
}

} // namespace paymaster_rpc::internal

} // namespace wallet
