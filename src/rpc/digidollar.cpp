// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif

#include <rpc/server.h>
#include <rpc/util.h>
#include <rpc/server_util.h>
#include <rpc/blockchain.h>
#include <rpc/digidollar_transactions.h>
#include <random.h>
#include <oracle/bundle_manager.h>
#include <primitives/oracle.h>
#include <oracle/node.h>
#include <oracle/mock_oracle.h>
#include <oracle/signing_orchestrator.h>
#include <oracle/musig2_aggregator.h>
#include <consensus/digidollar.h>
#include <consensus/dca.h>
#include <consensus/err.h>
#include <consensus/volatility.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <index/digidollarstatsindex.h>
#include <chainparams.h>
#include <clientversion.h>
#include <kernel/chainparams.h>
#include <logging.h>
#include <node/context.h>
#include <core_io.h>
#include <util/strencodings.h>
#include <validation.h>
#include <versionbits.h>
#include <cmath>
#ifdef ENABLE_WALLET
#include <util/any.h>
#include <wallet/wallet.h>
#include <wallet/receive.h>
#include <wallet/context.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/coincontrol.h>
#include <wallet/coinselection.h>
#include <wallet/digidollarwallet.h>
#include <wallet/walletdb.h>
#include <wallet/scriptpubkeyman.h>
#include <interfaces/wallet.h>
#endif
#include <digidollar/txbuilder.h>
#include <node/transaction.h>
#include <base58.h>
#include <script/standard.h>
#include <script/signingprovider.h>
#include <rpc/protocol.h>
#include <oracle/musig2_messages.h>
#include <versionbits.h>
#include <deploymentstatus.h>
#include <key_io.h>
#include <policy/policy.h>
#include <version.h>

#include <util/time.h>
#include <univalue.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

using namespace DigiDollar;
using namespace DigiDollar::DCA;

// Mock utility functions for RPC-only implementation
namespace {
    // Aligned with consensus/digidollar.h (10 tiers, 0-9)
    int GetLockDaysForTier(uint32_t tier) {
        switch (tier) {
            case 0: return 0;     // Special: 1 hour (240 blocks) - handled separately
            case 1: return 30;    // 30 days
            case 2: return 90;    // 90 days (3 months)
            case 3: return 180;   // 180 days (6 months)
            case 4: return 365;   // 1 year
            case 5: return 730;   // 2 years
            case 6: return 1095;  // 3 years
            case 7: return 1825;  // 5 years
            case 8: return 2555;  // 7 years
            case 9: return 3650;  // 10 years
            default: return 0;
        }
    }

    int GetMinCollateralRatio(uint32_t tier) {
        const int lock_days = GetLockDaysForTier(tier);
        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(lock_days);
        const int ratio = DigiDollar::GetCollateralRatioForLockTime(lock_blocks, Params().GetDigiDollarParams());
        return ratio > 0 ? ratio : 0;
    }

#ifdef ENABLE_WALLET
    bool HasPendingDigiDollarRedeem(const wallet::CWallet& wallet, const uint256& position_id)
    {
        const COutPoint collateral_outpoint(position_id, 0);
        for (const auto& wallet_entry : wallet.mapWallet) {
            const wallet::CWalletTx& wtx = wallet_entry.second;
            if (!wtx.tx || ::GetDigiDollarTxType(*wtx.tx) != ::DD_TX_REDEEM) continue;
            if (wtx.isAbandoned()) continue;
            if (wallet.GetTxDepthInMainChain(wtx) != 0 || !wtx.isUnconfirmed()) continue;

            for (const auto& txin : wtx.tx->vin) {
                if (txin.prevout == collateral_outpoint) {
                    return true;
                }
            }
        }
        return false;
    }
#endif

#ifdef ENABLE_WALLET
    bool TryParseOraclePrivateKey(const std::string& private_key_hex, CKey& key_out, std::string& error_out)
    {
        if (!IsHex(private_key_hex)) {
            error_out = "private key is not valid hex";
            return false;
        }
        auto key_data_opt = TryParseHex<unsigned char>(private_key_hex);
        if (!key_data_opt || key_data_opt->size() != 32) {
            error_out = "private key must be 32 bytes";
            return false;
        }

        CKey key;
        key.Set(key_data_opt->begin(), key_data_opt->end(), true);
        if (!key.IsValid()) {
            error_out = "private key is invalid";
            return false;
        }

        key_out = key;
        return true;
    }

    uint32_t ParseConfiguredOracleId(const JSONRPCRequest& request, size_t param_index)
    {
        const int oracle_id_signed = request.params[param_index].getInt<int>();
        if (oracle_id_signed < 0 || oracle_id_signed >= ORACLE_TOTAL_COUNT) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                strprintf("Invalid oracle ID %d. Must be between 0 and %d",
                          oracle_id_signed, ORACLE_TOTAL_COUNT - 1));
        }
        if (Params().GetOracleNode(static_cast<uint32_t>(oracle_id_signed)) == nullptr) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                strprintf("Oracle ID %d not found in chain parameters", oracle_id_signed));
        }
        return static_cast<uint32_t>(oracle_id_signed);
    }

    void EnsureOracleWalletCanUsePrivateKeys(const wallet::CWallet& wallet)
    {
        if (wallet.IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
        }
    }

    void EnsureOracleWalletUnlocked(const wallet::CWallet& wallet, const std::string& action)
    {
        if (wallet.IsLocked()) {
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                strprintf("DigiDollar oracle key %s requires the wallet to be unlocked. "
                          "Error: Please enter the wallet passphrase with walletpassphrase first.",
                          action));
        }
    }

    bool OraclePubKeyMatchesChainparams(uint32_t oracle_id, const CPubKey& pubkey, std::string& message_out)
    {
        const OracleNodeInfo* oracle_config = Params().GetOracleNode(oracle_id);
        if (!oracle_config) {
            message_out = strprintf("Oracle ID %u not found in chain parameters", oracle_id);
            return false;
        }
        if (pubkey != oracle_config->pubkey) {
            message_out = strprintf(
                "Oracle key is configured but is not authorized for oracle ID %u: wallet/public key mismatch (wallet pubkey %s, chainparams pubkey %s)",
                oracle_id, HexStr(pubkey), HexStr(oracle_config->pubkey));
            return false;
        }
        return true;
    }

    bool OraclePrivateKeyMatchesChainparams(uint32_t oracle_id, const std::string& private_key_hex, std::string& message_out)
    {
        CKey key;
        std::string parse_error;
        if (!TryParseOraclePrivateKey(private_key_hex, key, parse_error)) {
            message_out = strprintf("Oracle private key is invalid: %s", parse_error);
            return false;
        }
        return OraclePubKeyMatchesChainparams(oracle_id, key.GetPubKey(), message_out);
    }

    bool TryStartOracleFromPrivateKey(OracleManager& oracle_manager, uint32_t oracle_id, const std::string& private_key_hex, const std::string& key_source, bool allow_initialized_without_running, std::string& status_message, bool* initialized_out = nullptr)
    {
        if (initialized_out) *initialized_out = false;
        if (!OraclePrivateKeyMatchesChainparams(oracle_id, private_key_hex, status_message)) {
            status_message = strprintf("%s from %s", status_message, key_source);
            return false;
        }

        bool initialized = false;
        OracleNode* oracle = oracle_manager.GetOracleNode(oracle_id);
        if (!oracle) {
            if (!oracle_manager.AddOracleNode(oracle_id, private_key_hex)) {
                status_message = strprintf("Failed to initialize oracle with %s", key_source);
                return false;
            }
            initialized = true;
            oracle_manager.EnableOracle(oracle_id, true);
            oracle = oracle_manager.GetOracleNode(oracle_id);
            if (!oracle) {
                status_message = strprintf("Oracle initialized with %s but manager returned no oracle instance", key_source);
                if (initialized_out) *initialized_out = initialized;
                return false;
            }
        } else {
            initialized = true;
            oracle_manager.EnableOracle(oracle_id, true);
        }
        if (initialized_out) *initialized_out = initialized;

        oracle->Start();
        if (oracle->IsRunning()) {
            status_message = strprintf("Oracle started with %s", key_source);
            return true;
        }

        if (allow_initialized_without_running) {
            status_message = strprintf("Oracle initialized with %s (price fetcher is not running)", key_source);
            return false;
        }

        status_message = strprintf("Oracle initialized with %s but failed to start price thread", key_source);
        return false;
    }
#endif

    const std::vector<std::string>& OracleDisplayNames()
    {
        static const std::vector<std::string> names = {
            "Jared", "Green Candle", "Bastian", "DanGB", "Shenger",
            "Ycagel", "Aussie", "LookInto", "JohnnyLawDGB", "Ogilvie",
            "ChopperBrian", "hallvardo", "DaPunzy", "DigiByteForce",
            "Neel", "DigiSwarm", "GTO90", "digibyte-maxi", "Anthony",
            "mbah_jambon", "Camden", "Twoface123", "LivingTheLife",
            "ChozenOne43", "ckunchained", "JMag", "HashedMax",
            "DennisPitallano", "DigiHash Mining Pool", "medgboracle3452",
            "DigibyteDaily", "Peer2Peer", "3DogsKanab",
            "LiberatedLark", "Manu_DGB_oracle"
        };
        return names;
    }

    std::string OracleDisplayName(uint32_t oracle_id)
    {
        const auto& names = OracleDisplayNames();
        return oracle_id < names.size() ? names[oracle_id] : strprintf("Oracle %u", oracle_id);
    }

#ifdef ENABLE_WALLET
    struct WalletOracleKeyLookup {
        bool wallet_resolved{false};
        bool key_found{false};
        bool selection_error{false};
        RPCErrorCode error_code{RPC_WALLET_ERROR};
        std::string error_message;
        std::string wallet_name;
        CPubKey pubkey;
    };

    WalletOracleKeyLookup LookupWalletOraclePubKey(const JSONRPCRequest& request, uint32_t oracle_id)
    {
        WalletOracleKeyLookup lookup;
        std::shared_ptr<wallet::CWallet> pwallet;

        if (util::AnyPtr<wallet::WalletContext>(request.context)) {
            try {
                pwallet = wallet::GetWalletForJSONRPCRequest(request);
            } catch (const UniValue& e) {
                lookup.selection_error = true;
                lookup.error_code = static_cast<RPCErrorCode>(e["code"].getInt<int>());
                lookup.error_message = e["message"].get_str();
                return lookup;
            }
        } else {
            node::NodeContext* node_ctx = util::AnyPtr<node::NodeContext>(request.context);
            wallet::WalletContext* wallet_ctx = node_ctx && node_ctx->wallet_loader ? node_ctx->wallet_loader->context() : nullptr;
            if (!wallet_ctx) {
                lookup.selection_error = true;
                lookup.error_code = RPC_WALLET_NOT_FOUND;
                lookup.error_message = "Wallet context not found; request this RPC through /wallet/<wallet_name> to inspect wallet-stored oracle keys";
                return lookup;
            }

            std::string wallet_name;
            if (wallet::GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
                pwallet = wallet::GetWallet(*wallet_ctx, wallet_name);
                if (!pwallet) {
                    lookup.selection_error = true;
                    lookup.error_code = RPC_WALLET_NOT_FOUND;
                    lookup.error_message = strprintf("Requested wallet '%s' does not exist or is not loaded", wallet_name);
                    return lookup;
                }
            } else {
                size_t wallet_count{0};
                pwallet = wallet::GetDefaultWallet(*wallet_ctx, wallet_count);
                if (!pwallet) {
                    lookup.selection_error = true;
                    if (wallet_count == 0) {
                        lookup.error_code = RPC_WALLET_NOT_FOUND;
                        lookup.error_message = "No wallet is loaded. Load a wallet or request /wallet/<wallet_name> to inspect wallet-stored oracle keys";
                    } else {
                        lookup.error_code = RPC_WALLET_NOT_SPECIFIED;
                        lookup.error_message = "Wallet file not specified; multiple wallets are loaded, so request /wallet/<wallet_name> to inspect wallet-stored oracle keys";
                    }
                    return lookup;
                }
            }
        }

        if (!pwallet) {
            lookup.selection_error = true;
            lookup.error_code = RPC_WALLET_NOT_FOUND;
            lookup.error_message = "No wallet is selected";
            return lookup;
        }

        lookup.wallet_resolved = true;
        lookup.wallet_name = pwallet->GetName();
        lookup.key_found = pwallet->GetOraclePubKey(oracle_id, lookup.pubkey);
        return lookup;
    }
#endif

    void PushWalletOraclePubKeyResult(UniValue& result, uint32_t oracle_id, const CPubKey& pubkey, const std::string& wallet_name, bool is_running)
    {
        XOnlyPubKey xonly_pubkey(pubkey);
        const OracleNodeInfo* oracle_config = Params().GetOracleNode(oracle_id);
        const bool authorized = oracle_config && pubkey == oracle_config->pubkey;

        result.pushKV("oracle_id", static_cast<int>(oracle_id));
        result.pushKV("pubkey", HexStr(xonly_pubkey));
        result.pushKV("pubkey_xonly", HexStr(xonly_pubkey));
        result.pushKV("pubkey_full", HexStr(pubkey));
        result.pushKV("valid", xonly_pubkey.IsFullyValid());
        result.pushKV("authorized", authorized);
        result.pushKV("is_running", is_running);
        result.pushKV("configured_in_wallet", true);
        result.pushKV("source", is_running ? "running_oracle" : "wallet");
        result.pushKV("wallet_name", wallet_name);
        if (!is_running) {
            result.pushKV("message", strprintf(
                "Oracle key is configured in wallet '%s' but the oracle is not running. Use 'startoracle %u' to start it.",
                wallet_name, oracle_id));
        }
    }

    void PublishRegtestMockMuSig2Quote(int32_t quote_height)
    {
        COracleBundle bundle = MockOracleManager::GetInstance().CreateMockMuSig2Bundle(quote_height, GetTime());
        std::string error;
        if (!OracleBundleManager::ValidateMuSig2Bundle(bundle, quote_height, Params().GetConsensus(), error)) {
            throw JSONRPCError(RPC_MISC_ERROR,
                strprintf("Failed to create regtest MuSig2 oracle quote: %s", error));
        }
        if (!OracleBundleManager::GetInstance().UpdateBundle(bundle)) {
            throw JSONRPCError(RPC_MISC_ERROR,
                "Failed to publish regtest MuSig2 oracle quote");
        }
    }

#ifdef ENABLE_WALLET
    void RefreshRegtestMockMuSig2QuoteForMempool(const wallet::CWallet& wallet)
    {
        if (Params().GetChainType() != ChainType::REGTEST) return;
        if (!MockOracleManager::GetInstance().IsEnabled()) return;
        if (MockOracleManager::GetInstance().GetCurrentPrice() <= 0) return;

        node::NodeContext* node_ctx = wallet.chain().context();
        if (!node_ctx || !node_ctx->chainman) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
        }

        const int current_height = WITH_LOCK(cs_main, {
            const CBlockIndex* tip = node_ctx->chainman->ActiveChain().Tip();
            return tip ? tip->nHeight : 0;
        });
        PublishRegtestMockMuSig2Quote(current_height + 1);
    }
#endif

    struct DigiDollarRpcTotals {
        CAmount total_collateral{0};
        CAmount total_dd{0};
    };

    DigiDollarRpcTotals GetDigiDollarRpcTotals(const JSONRPCRequest& request)
    {
        DigiDollarRpcTotals totals;

        const node::NodeContext& node = EnsureAnyNodeContext(request.context);
        ChainstateManager& chainman = EnsureChainman(node);
        if (g_digidollar_stats_index) {
            if (!g_digidollar_stats_index->BlockUntilSyncedToCurrentChain()) {
                const IndexSummary summary{g_digidollar_stats_index->GetSummary()};
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                    strprintf("DigiDollar stats index is syncing. Current height: %d", summary.best_block_height));
            }

            const CBlockIndex* pindex = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
            if (pindex) {
                auto stats = g_digidollar_stats_index->LookUpStats(*pindex);
                if (stats) {
                    totals.total_dd = stats->total_dd_supply;
                    totals.total_collateral = stats->total_collateral;
                }
            }
        } else {
            const SystemMetrics cached_metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
            const int chain_height = WITH_LOCK(cs_main, return chainman.ActiveChain().Height());
            if (chain_height <= 0 &&
                cached_metrics.hasCanonicalHealth &&
                cached_metrics.totalDDSupply > 0 &&
                cached_metrics.totalCollateral > 0) {
                totals.total_dd = cached_metrics.totalDDSupply;
                totals.total_collateral = cached_metrics.totalCollateral;
                return totals;
            }

            Chainstate& active_chainstate = chainman.ActiveChainstate();
            active_chainstate.ForceFlushStateToDisk();
            {
                LOCK(::cs_main);
                CCoinsView* coins_view = &active_chainstate.CoinsDB();
                node::BlockManager* blockman = &active_chainstate.m_blockman;
                const CTxMemPool* mempool = node.mempool.get();
                if (!DigiDollar::SystemHealthMonitor::ScanUTXOSet(
                        coins_view, &active_chainstate.CoinsTip(), blockman, mempool,
                        &active_chainstate.m_chain, &Params().GetConsensus())) {
                    throw JSONRPCError(RPC_MISC_ERROR,
                        "DigiDollar-era block data is incomplete or unreadable; restart with -reindex");
                }
            }
            DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();
            totals.total_collateral = metrics.totalCollateral;
            totals.total_dd = metrics.totalDDSupply;
        }

        return totals;
    }

#ifdef ENABLE_WALLET
    std::vector<COutPoint> ParseDigiDollarSelectedInputs(const UniValue& inputs)
    {
        std::vector<COutPoint> outpoints;
        for (const UniValue& input : inputs.get_array().getValues()) {
            const UniValue& obj = input.get_obj();
            const UniValue& txid_value = obj.find_value("txid");
            const UniValue& vout_value = obj.find_value("vout");
            if (txid_value.isNull() || vout_value.isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Each selected DigiDollar input must include txid and vout");
            }
            uint256 hash;
            const std::string txid = txid_value.get_str();
            if (!IsHex(txid) || txid.size() != 64) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid selected DigiDollar input txid");
            }
            hash.SetHex(txid);
            const int vout = vout_value.getInt<int>();
            if (vout < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Selected DigiDollar input vout must be non-negative");
            }
            outpoints.emplace_back(hash, static_cast<uint32_t>(vout));
        }
        return outpoints;
    }
#endif

    int GetDigiDollarRpcSystemHealth(const JSONRPCRequest& request,
                                     CAmount oracle_price_micro_usd,
                                     int empty_supply_health)
    {
        DigiDollarRpcTotals totals = GetDigiDollarRpcTotals(request);
        if (totals.total_dd == 0) {
            return empty_supply_health;
        }

        const CAmount oracle_price_millicents = oracle_price_micro_usd / 10;
        return DynamicCollateralAdjustment::CalculateSystemHealth(
            totals.total_collateral, totals.total_dd, oracle_price_millicents);
    }

#ifdef ENABLE_WALLET
    CAmount ParseDigiDollarRpcAmount(const UniValue& amount_param)
    {
        if (!amount_param.isStr() && !amount_param.isNum()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be a number (integer cents or decimal dollars)");
        }

        const std::string amount_str = amount_param.getValStr();
        const bool decimal_dollars = amount_str.find('.') != std::string::npos;
        int64_t amount = 0;
        if (!ParseFixedPoint(amount_str, decimal_dollars ? 2 : 0, &amount)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount is not a valid number");
        }
        return static_cast<CAmount>(amount);
    }
#endif

    bool OptionalParamIsSet(const JSONRPCRequest& request, size_t index)
    {
        return request.params.size() > index && !request.params[index].isNull();
    }

    bool IsFreshOracleTimestamp(int64_t timestamp, int64_t now)
    {
        return timestamp > 0 &&
               timestamp <= now + 60 &&
               now - timestamp <= ORACLE_MAX_AGE_SECONDS;
    }

    int LatestFreshOracleBundleHeight(const ChainstateManager& chainman, const OracleBundleManager& oracle_manager, int current_height)
    {
        LOCK(cs_main);
        const int64_t now = GetTime();
        for (int h = current_height; h >= std::max(0, current_height - 20); --h) {
            const CBlockIndex* pindex = chainman.ActiveChain()[h];
            if (!pindex) continue;
            CBlock block;
            if (!chainman.m_blockman.ReadBlockFromDisk(block, *pindex)) continue;
            if (block.vtx.empty()) continue;

            COracleBundle bundle;
            if (oracle_manager.ExtractOracleBundle(*block.vtx[0], bundle) &&
                IsFreshOracleTimestamp(bundle.timestamp, now)) {
                return h;
            }
        }
        return 0;
    }

    std::string ExpectedDigiDollarAddressPrefix()
    {
        switch (Params().GetChainType()) {
        case ChainType::REGTEST:
            return "RD";
        case ChainType::TESTNET:
            return "TD";
        case ChainType::MAIN:
        case ChainType::SIGNET:
            return "DD";
        }
        return "DD";
    }

    std::string DigiDollarAddressNetworkForPrefix(const std::string& prefix)
    {
        if (prefix == "DD") return "mainnet";
        if (prefix == "TD") return "testnet";
        if (prefix == "RD") return "regtest";
        return "unknown";
    }

    bool ValidateDigiDollarAddressForCurrentNetwork(const std::string& address, std::string& error)
    {
        CDigiDollarAddress dd_address(address);
        if (!dd_address.IsValid()) {
            error = "Invalid DigiDollar address";
            return false;
        }

        const std::string prefix = address.substr(0, 2);
        const std::string expected = ExpectedDigiDollarAddressPrefix();
        if (prefix != expected) {
            error = strprintf("DigiDollar address is for %s network (%s prefix), but this node expects %s prefix",
                              DigiDollarAddressNetworkForPrefix(prefix), prefix, expected);
            return false;
        }

        return true;
    }
}

RPCHelpMan getdigidollarstats()
{
    return RPCHelpMan{"getdigidollarstats",
                "\nGet current DigiDollar system health information.\n"
                "Returns the overall health of the DigiDollar stablecoin system,\n"
                "including collateralization ratio and DCA tier status.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "health_percentage", "System health as percentage (e.g., 150 = 150% collateralized)"},
                        {RPCResult::Type::STR, "health_status", "Health tier status: healthy, warning, critical, or emergency"},
                        {RPCResult::Type::NUM, "total_collateral_dgb", "Total DGB locked as collateral"},
                        {RPCResult::Type::NUM, "total_dd_supply", "Total DigiDollar supply in circulation (in cents)"},
                        {RPCResult::Type::NUM, "oracle_price_cents", "Current DGB/USD price from oracle (in cents per DGB)"},
                        {RPCResult::Type::NUM, "oracle_price_micro_usd", "Current DGB/USD price from oracle in micro-USD (1,000,000 = $1.00)"},
                        {RPCResult::Type::BOOL, "oracle_available", "True when a live oracle price is available"},
                        {RPCResult::Type::STR, "oracle_status", "Oracle availability status: available or unavailable"},
                        {RPCResult::Type::STR, "minting_restricted_reason", "Why minting is restricted: none, oracle_unavailable, or err_active"},
                        {RPCResult::Type::BOOL, "is_emergency", "True if system is in emergency state (<100% collateralized)"},
                        {RPCResult::Type::NUM, "system_collateral_ratio", "Alias for health_percentage (for backward compatibility)"},
                        {RPCResult::Type::NUM, "total_collateral_locked", "Alias for total_collateral_dgb (in satoshis)"},
                        {RPCResult::Type::NUM, "active_positions", "Number of active DD positions"},
                        {RPCResult::Type::NUM, "oracle_price_age", "Blocks since last oracle update"},
                        {RPCResult::Type::OBJ, "dca_tier", "Current DCA tier information",
                            {
                                {RPCResult::Type::NUM, "min_collateral", "Minimum collateral % for this tier"},
                                {RPCResult::Type::NUM, "max_collateral", "Maximum collateral % for this tier"},
                                {RPCResult::Type::NUM, "multiplier", "DCA multiplier for new mints in this tier"},
                                {RPCResult::Type::STR, "status", "Tier status description"}
                            }
                        },
                        {RPCResult::Type::OBJ, "err_tier", "Current Emergency Redemption Ratio (ERR) tier information",
                            {
                                {RPCResult::Type::NUM, "ratio", "ERR ratio (0.80-1.0) - lower = more DD burn required"},
                                {RPCResult::Type::NUM, "burn_multiplier", "DD burn multiplier (1.0-1.25x) - how much MORE DD to burn"},
                                {RPCResult::Type::STR, "description", "ERR tier description with burn multiplier"}
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("getdigidollarstats", "")
                    + HelpExampleRpc("getdigidollarstats", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            // NETWORK-WIDE TRACKING: Use DigiDollar stats index for efficient tracking
            // This ensures all nodes see identical stats regardless of which wallets are loaded
            CAmount totalCollateral = 0;
            CAmount totalDD = 0;

            // Get node context for chainstate access
            const node::NodeContext& node = EnsureAnyNodeContext(request.context);
            ChainstateManager& chainman = EnsureChainman(node);

            // Use the DigiDollar stats index for efficient network-wide tracking
            if (g_digidollar_stats_index) {
                if (!g_digidollar_stats_index->BlockUntilSyncedToCurrentChain()) {
                    const IndexSummary summary{g_digidollar_stats_index->GetSummary()};
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                        strprintf("DigiDollar stats index is syncing. Current height: %d", summary.best_block_height));
                }

                const CBlockIndex* pindex;
                {
                    LOCK(cs_main);
                    pindex = chainman.ActiveChain().Tip();
                }

                if (pindex) {
                    auto stats = g_digidollar_stats_index->LookUpStats(*pindex);
                    if (stats) {
                        totalDD = stats->total_dd_supply;
                        totalCollateral = stats->total_collateral;
                    }
                }
            } else {
                // Fallback: Use UTXO scanning (slow but works without index)
                LogPrintf("DigiDollar: getdigidollarstats - DigiDollar stats index not available, falling back to UTXO scan\n");

                // Access the UTXO set (like gettxoutsetinfo does)
                // CRITICAL: Must flush OUTSIDE the lock, then re-acquire lock for scanning
                Chainstate& active_chainstate = chainman.ActiveChainstate();

                // Step 1: Force flush all cached coins to disk (like gettxoutsetinfo does)
                LogPrintf("DigiDollar: getdigidollarstats - About to ForceFlushStateToDisk...\n");
                active_chainstate.ForceFlushStateToDisk();
                LogPrintf("DigiDollar: getdigidollarstats - ForceFlushStateToDisk completed\n");

                // Step 2: Now acquire lock and access the flushed CoinsDB
                // CRITICAL: Hold cs_main lock during ScanUTXOSet to prevent race conditions
                CCoinsView* coins_view;
                node::BlockManager* blockman;
                const CTxMemPool* mempool = node.mempool.get();
                {
                    LOCK(::cs_main);
                    coins_view = &active_chainstate.CoinsDB();
                    blockman = &active_chainstate.m_blockman;

                    // Scan UTXO set to find ALL DigiDollar vaults network-wide
                    // Pass BlockManager for full transaction access
                    // Pass both CoinsDB (for iteration) and CoinsTip (for validation)
                    LogPrintf("DigiDollar: getdigidollarstats - About to call ScanUTXOSet...\n");
                    if (!DigiDollar::SystemHealthMonitor::ScanUTXOSet(coins_view, &active_chainstate.CoinsTip(), blockman, mempool, &active_chainstate.m_chain, &Params().GetConsensus())) {
                        throw JSONRPCError(RPC_MISC_ERROR,
                            "DigiDollar-era block data is incomplete or unreadable; restart with -reindex");
                    }
                    LogPrintf("DigiDollar: getdigidollarstats - ScanUTXOSet completed\n");
                }

                // Get metrics from scanner
                DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();
                totalCollateral = metrics.totalCollateral;
                totalDD = metrics.totalDDSupply;
            }

            // Get current oracle price in micro-USD from the real oracle system
            // micro-USD format: 1,000,000 = $1.00, so 6310 = $0.00631
            OracleBundleManager& oracle_manager = OracleBundleManager::GetInstance();
            CAmount oraclePriceMicroUSD = oracle_manager.GetLatestPrice();

            // Fall back to MockOracleManager for regtest/testing if no real oracle data
            if (oraclePriceMicroUSD <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                MockOracleManager::GetInstance().IsEnabled()) {
                // MockOracleManager already returns micro-USD (see mock_oracle.cpp)
                oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
            }
            const bool oracleAvailable = oraclePriceMicroUSD > 0;

            // Convert micro-USD to millicents for CalculateSystemHealth
            // micro-USD / 10 = millicents (e.g., 6310 micro-USD / 10 = 631 millicents = $0.00631)
            CAmount oraclePriceMillicents = oraclePriceMicroUSD / 10;

            // Calculate cents for display (rounded). Allow 0 for sub-cent prices —
            // oracle_price_micro_usd and price_usd fields have full precision.
            CAmount oraclePriceCents = (oraclePriceMicroUSD + 5000) / 10000;

            // Calculate system health
            // IMPORTANT: Return 0% if no DD minted network-wide (instead of default 30000%)
            int systemHealth;
            if (totalDD == 0) {
                systemHealth = 0;  // No DD minted = 0% health, not 30000%
            } else {
                systemHealth = DynamicCollateralAdjustment::CalculateSystemHealth(
                    totalCollateral, totalDD, oraclePriceMillicents);
            }

            // Get current tier information
            auto tier = DynamicCollateralAdjustment::GetCurrentTier(systemHealth);

            // Check emergency status
            bool isEmergency = oracleAvailable && totalDD > 0 && DynamicCollateralAdjustment::IsSystemEmergency(systemHealth);
            const std::string mintingRestrictedReason = !oracleAvailable ? "oracle_unavailable" :
                (isEmergency ? "err_active" : "none");

            UniValue result(UniValue::VOBJ);
            result.pushKV("health_percentage", systemHealth);
            result.pushKV("health_status", tier.status);
            result.pushKV("total_collateral_dgb", ValueFromAmount(totalCollateral));
            result.pushKV("total_dd_supply", int64_t{totalDD});
            result.pushKV("oracle_price_cents", int64_t{oraclePriceCents});   // Rounded to cents for display
            result.pushKV("oracle_price_micro_usd", int64_t{oraclePriceMicroUSD}); // Full precision micro-USD
            result.pushKV("oracle_available", oracleAvailable);
            result.pushKV("oracle_status", oracleAvailable ? "available" : "unavailable");
            result.pushKV("minting_restricted_reason", mintingRestrictedReason);
            result.pushKV("is_emergency", isEmergency);

            // Add fields expected by tests
            result.pushKV("system_collateral_ratio", systemHealth);
            result.pushKV("total_collateral_locked", ValueFromAmount(totalCollateral));
            // Get active position count from the DigiDollar stats index.
            // The stats index tracks vault_count (incremented on mint,
            // decremented on redeem) so this reflects real network state.
            // Without the index (e.g. pruned nodes, where it is off), the
            // UTXO scan performed above in this call counted the live vaults.
            uint64_t activePositions = 0;
            if (g_digidollar_stats_index) {
                ChainstateManager& chainman = EnsureAnyChainman(request.context);
                LOCK(cs_main);
                const CBlockIndex* pindex = chainman.ActiveChain().Tip();
                if (pindex) {
                    auto ddstats = g_digidollar_stats_index->LookUpStats(*pindex);
                    if (ddstats) {
                        activePositions = ddstats->vault_count;
                    }
                }
            } else {
                activePositions = static_cast<uint64_t>(std::max(
                    0, DigiDollar::SystemHealthMonitor::GetSystemMetrics().totalActivePositions));
            }
            result.pushKV("active_positions", static_cast<int64_t>(activePositions));
            int oraclePriceAge = 0;
            const int currentHeight = WITH_LOCK(cs_main, {
                const CBlockIndex* tip = chainman.ActiveChain().Tip();
                return tip ? tip->nHeight : 0;
            });
            if (Params().GetChainType() == ChainType::REGTEST &&
                MockOracleManager::GetInstance().IsEnabled() &&
                MockOracleManager::GetInstance().GetCurrentPrice() > 0) {
                const int64_t lastUpdateHeight = MockOracleManager::GetInstance().GetLastUpdateHeight();
                if (lastUpdateHeight > 0 && lastUpdateHeight <= currentHeight) {
                    oraclePriceAge = currentHeight - lastUpdateHeight;
                }
            } else {
                const int lastBundleHeight = LatestFreshOracleBundleHeight(chainman, oracle_manager, currentHeight);
                if (lastBundleHeight > 0 && lastBundleHeight <= currentHeight) {
                    oraclePriceAge = currentHeight - lastBundleHeight;
                }
            }
            result.pushKV("oracle_price_age", oraclePriceAge);

            UniValue dcaTier(UniValue::VOBJ);
            dcaTier.pushKV("min_collateral", tier.minCollateral);
            dcaTier.pushKV("max_collateral", tier.maxCollateral);
            dcaTier.pushKV("multiplier", tier.multiplier);
            dcaTier.pushKV("status", tier.status);
            result.pushKV("dca_tier", dcaTier);

            // Add ERR (Emergency Redemption Ratio) tier information
            // ERR increases DD burn requirement, NOT reduces collateral!
            // ratio = how much of original DD is "worth" -> burn 1/ratio DD to get FULL collateral
            double errRatio = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(systemHealth);
            double burnMultiplier = 1.0;
            std::string errDescription;
            if (!oracleAvailable) {
                errDescription = "Oracle unavailable: ERR cannot be evaluated";
                errRatio = 1.0;
                burnMultiplier = 1.0;
            } else if (systemHealth >= 100) {
                errDescription = "Normal (1.0x burn)";
                errRatio = 1.0;
                burnMultiplier = 1.0;
            } else if (systemHealth >= 95) {
                errDescription = "95-100%: 1.05x DD burn";
                burnMultiplier = 1.0 / errRatio; // ~1.053x
            } else if (systemHealth >= 90) {
                errDescription = "90-95%: 1.11x DD burn";
                burnMultiplier = 1.0 / errRatio; // ~1.111x
            } else if (systemHealth >= 85) {
                errDescription = "85-90%: 1.18x DD burn";
                burnMultiplier = 1.0 / errRatio; // ~1.176x
            } else {
                errDescription = "<85%: 1.25x DD burn (max)";
                burnMultiplier = 1.0 / errRatio; // 1.25x
            }

            UniValue errTier(UniValue::VOBJ);
            errTier.pushKV("ratio", errRatio);
            errTier.pushKV("burn_multiplier", burnMultiplier);
            errTier.pushKV("description", errDescription);
            result.pushKV("err_tier", errTier);

            return result;
        },
    };
}

static RPCHelpMan getdcamultiplier()
{
    return RPCHelpMan{"getdcamultiplier",
                "\nGet current Dynamic Collateral Adjustment (DCA) multiplier.\n"
                "Returns the multiplier applied to base collateral ratios for new mints.\n",
                {
                    {"system_health", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Optional: calculate multiplier for specific health % (for testing)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "multiplier", "Current DCA multiplier (e.g., 1.0 = no adjustment, 2.0 = double collateral)"},
                        {RPCResult::Type::NUM, "system_health", "System health percentage used for calculation"},
                        {RPCResult::Type::STR, "tier_status", "Health tier: healthy, warning, critical, or emergency"},
                        {RPCResult::Type::STR, "description", "Human-readable description of DCA effect"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getdcamultiplier", "")
                    + HelpExampleCli("getdcamultiplier", "130")
                    + HelpExampleRpc("getdcamultiplier", "")
                    + HelpExampleRpc("getdcamultiplier", "130")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            int systemHealth;

            // Use provided health or calculate current
            if (OptionalParamIsSet(request, 0)) {
                systemHealth = request.params[0].getInt<int>();
                if (systemHealth < 0 || systemHealth > 30000) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "System health must be between 0 and 30000");
                }
            } else {
                CAmount oraclePriceMicroUSD = OracleBundleManager::GetInstance().GetLatestPrice();
                if (oraclePriceMicroUSD <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                    MockOracleManager::GetInstance().IsEnabled()) {
                    oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
                }
                systemHealth = GetDigiDollarRpcSystemHealth(request, oraclePriceMicroUSD, 0);
            }

            // Get DCA multiplier
            double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(systemHealth);
            auto tier = DynamicCollateralAdjustment::GetCurrentTier(systemHealth);

            // Create description
            std::string description;
            if (multiplier == 1.0) {
                description = "No additional collateral required (healthy system)";
            } else {
                description = strprintf("%.1fx base collateral required (%s system)",
                                      multiplier, tier.status);
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("multiplier", multiplier);
            result.pushKV("system_health", systemHealth);
            result.pushKV("tier_status", tier.status);
            result.pushKV("description", description);

            return result;
        },
    };
}


static RPCHelpMan calculatecollateralrequirement()
{
    return RPCHelpMan{"calculatecollateralrequirement",
                "\nCalculate DGB collateral requirement for a DigiDollar mint.\n"
                "Uses current system health and DCA multipliers to determine\n"
                "the exact amount of DGB needed for a given DD mint amount and lock period.\n",
                {
                    {"dd_amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "DigiDollar amount to mint in cents (e.g., 10000 = $100)"},
                    {"lock_days", RPCArg::Type::NUM, RPCArg::Optional::NO, "Lock period in days. Canonical tiers only: 0 (1 hour testing tier), 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650"},
                    {"oracle_price", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "DGB price in micro-USD per DGB (1,000,000 = $1.00; uses current price if omitted)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_AMOUNT, "required_dgb", "Minimum consensus DGB collateral amount"},
                        {RPCResult::Type::STR_AMOUNT, "minimum_required_dgb", "Minimum consensus DGB collateral amount"},
                        {RPCResult::Type::STR_AMOUNT, "wallet_collateral_dgb", "DGB collateral the wallet mint builder will lock, including safety margin"},
                        {RPCResult::Type::STR_AMOUNT, "collateral_safety_margin_dgb", "Extra DGB collateral added by the wallet safety margin"},
                        {RPCResult::Type::NUM, "dd_amount_cents", "DD amount being minted (in cents)"},
                        {RPCResult::Type::NUM, "dd_amount_usd", "DD amount being minted (in USD)"},
                        {RPCResult::Type::NUM, "lock_days", "Lock period in days"},
                        {RPCResult::Type::NUM, "lock_blocks", "Lock period in blocks"},
                        {RPCResult::Type::NUM, "base_ratio", "Base collateral ratio % for this lock period"},
                        {RPCResult::Type::NUM, "dca_multiplier", "DCA multiplier applied"},
                        {RPCResult::Type::NUM, "effective_ratio", "Final collateral ratio % (base * DCA)"},
                        {RPCResult::Type::NUM, "oracle_price_micro_usd", "DGB price used in micro-USD (1,000,000 = $1.00)"},
                        {RPCResult::Type::NUM, "oracle_price_usd", "DGB price used in USD"},
                        {RPCResult::Type::NUM, "system_health", "Current system health %"},
                        {RPCResult::Type::STR, "dca_tier", "Current DCA tier status"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("calculatecollateralrequirement", "10000 365")
                    + HelpExampleCli("calculatecollateralrequirement", "50000 1095 4000")
                    + HelpExampleRpc("calculatecollateralrequirement", "10000, 365")
                    + HelpExampleRpc("calculatecollateralrequirement", "50000, 1095, 4000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            // Parse parameters
            CAmount ddAmount = request.params[0].getInt<int64_t>();
            int lockDays = request.params[1].getInt<int>();

            // Get oracle price in micro-USD: use provided value or fetch from real oracle system
            CAmount oraclePriceMicroUSD;
            if (OptionalParamIsSet(request, 2)) {
                // User-provided value is in micro-USD (1,000,000 = $1.00)
                oraclePriceMicroUSD = request.params[2].getInt<int64_t>();
            } else {
                // Use real oracle price from OracleIntegration (returns micro-USD)
                oraclePriceMicroUSD = OracleIntegration::GetCurrentOraclePriceMicroUSD();
                if (oraclePriceMicroUSD <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                    MockOracleManager::GetInstance().IsEnabled()) {
                    // Fall back to mock oracle ONLY in regtest
                    oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
                }
                if (oraclePriceMicroUSD <= 0) {
                    throw JSONRPCError(RPC_MISC_ERROR, "No oracle price available. Start the oracle first with startoracle command.");
                }
            }

            // Validate parameters. lockDays = 0 is a valid request: it maps to
            // the 1-hour testing tier (240 blocks at 15 second blocks).
            // GetCollateralRatioForLockTime() rejects every other non-canonical
            // duration below, so we only guard against negative input here.
            if (ddAmount <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "DD amount must be positive");
            }
            if (lockDays < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Lock days must be non-negative");
            }
            if (oraclePriceMicroUSD <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Oracle price must be positive");
            }

            // Get system parameters
            const auto& params = Params();
            const auto& ddParams = params.GetDigiDollarParams();
            if (!DigiDollar::IsValidMintAmount(ddAmount, ddParams)) {
                if (ddAmount < ddParams.minMintAmount) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                        strprintf("Minimum mint amount is $%d (%d cents)",
                            ddParams.minMintAmount / 100, ddParams.minMintAmount));
                }
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Maximum mint amount is $%d (%d cents)",
                        ddParams.maxMintAmount / 100, ddParams.maxMintAmount));
            }

            // Convert lock days to blocks
            int64_t lockBlocks = DigiDollar::LockDaysToBlocks(lockDays);

            // Get base collateral ratio. The error message must list every
            // canonical lock period the consensus collateral map exposes, so
            // RPC users do not see drift between the help text, the error
            // message, and the consensus rule set.
            int baseRatio = DigiDollar::GetCollateralRatioForLockTime(lockBlocks, ddParams);
            if (baseRatio <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid lock period: %d days. Canonical periods: "
                              "0 (1 hour testing tier), 30, 90, 180, 365, 730, "
                              "1095, 1825, 2555, 3650",
                              lockDays));
            }

            // Use the same chain-derived health source as getdcamultiplier().
            // Empty supply is treated as healthy here so the first quote does
            // not inherit an emergency multiplier from the display-only 0% stat.
            int systemHealth = GetDigiDollarRpcSystemHealth(request, oraclePriceMicroUSD, 30000);
            double dcaMultiplier = DynamicCollateralAdjustment::GetDCAMultiplier(systemHealth);
            int effectiveRatio = DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemHealth);
            auto tier = DynamicCollateralAdjustment::GetCurrentTier(systemHealth);

            // Calculate required DGB using micro-USD precision
            // Formula: Required_DGB_sats = (DD_cents * COIN * ratio * 100) / oracle_micro_usd
            // Example: $100 DD at $0.00631 DGB with 150% ratio (oracle_micro_usd = 6310)
            //   = (10000 cents * 100000000 * 150 * 100) / 6310
            //   = 15,000,000,000,000,000 / 6310
            //   = 2,377,179,080,509 sats = ~23,772 DGB
            // Use __int128 to avoid uint64 overflow for large DD amounts
            __int128 numerator = static_cast<__int128>(ddAmount) * static_cast<__int128>(COIN) *
                                 static_cast<__int128>(effectiveRatio) * 100;
            __int128 denominator = static_cast<__int128>(oraclePriceMicroUSD);
            __int128 result128 = (numerator + denominator - 1) / denominator;
            if (result128 > static_cast<__int128>(MAX_MONEY)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Required collateral exceeds maximum money supply");
            }
            CAmount requiredDGB = static_cast<CAmount>(result128);
            const CAmount walletCollateralDGB = DigiDollar::ApplyCollateralSafetyMargin(requiredDGB);
            if (walletCollateralDGB <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Wallet collateral requirement exceeds maximum money supply");
            }
            const CAmount collateralSafetyMarginDGB = walletCollateralDGB - requiredDGB;

            UniValue result(UniValue::VOBJ);
            result.pushKV("required_dgb", ValueFromAmount(requiredDGB));
            result.pushKV("minimum_required_dgb", ValueFromAmount(requiredDGB));
            result.pushKV("wallet_collateral_dgb", ValueFromAmount(walletCollateralDGB));
            result.pushKV("collateral_safety_margin_dgb", ValueFromAmount(collateralSafetyMarginDGB));
            result.pushKV("dd_amount_cents", int64_t{ddAmount});
            result.pushKV("dd_amount_usd", ddAmount / 100.0);  // Convert cents to USD
            result.pushKV("lock_days", lockDays);
            result.pushKV("lock_blocks", int64_t{lockBlocks});
            result.pushKV("base_ratio", baseRatio);
            result.pushKV("dca_multiplier", dcaMultiplier);
            result.pushKV("effective_ratio", effectiveRatio);
            result.pushKV("oracle_price_micro_usd", int64_t{oraclePriceMicroUSD});
            result.pushKV("oracle_price_usd", oraclePriceMicroUSD / 1000000.0);
            result.pushKV("system_health", systemHealth);
            result.pushKV("dca_tier", tier.status);

            return result;
        },
    };
}

static RPCHelpMan getdigidollardeploymentinfo()
{
    return RPCHelpMan{"getdigidollardeploymentinfo",
                "\nGet DigiDollar deployment activation status and information.\n"
                "DigiDollar is a buried deployment (BIP90): it activated via BIP9 bit-23\n"
                "signaling and its activation height is now hardcoded per network\n"
                "(mainnet 23869440, testnet 600, default regtest 0).\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "enabled", "Whether DigiDollar is currently enabled/active"},
                        {RPCResult::Type::STR, "type", "Deployment type (always \"buried\")"},
                        {RPCResult::Type::STR, "status", "Deployment status (defined before the activation height, active at/after it)"},
                        {RPCResult::Type::NUM, "activation_height", /*optional=*/true, "Activation height of the buried deployment (omitted if the deployment is disabled on this network)"},
                        {RPCResult::Type::NUM, "oracle_activation_height", "Height at which oracle/DD block rules activate (nOracleActivationHeight)"},
                        {RPCResult::Type::NUM, "musig2_format_activation_height", "Height at which the MuSig2 v0x03 bundle format activates (nDigiDollarMuSig2Height)"},
                        {RPCResult::Type::NUM, "oracle_pubkey_count", "Number of consensus oracle public keys configured for MuSig2 (nOraclePubkeyCount)"},
                        {RPCResult::Type::NUM, "oracle_consensus_required", "MuSig2 quorum size required to satisfy a v0x03 bundle (nOracleConsensusRequired)"},
                        {RPCResult::Type::NUM, "oracle_total_slots", "Total oracle slots configured in chainparams (vOracleNodes.size); oracle_id values must be below oracle_pubkey_count to vote"},
                        {RPCResult::Type::ARR, "oracle_seed_peers", "Public mainnet peers operators can use to bootstrap DigiDollar oracle P2P connectivity",
                            {
                                {RPCResult::Type::STR, "peer", "Host:port seed peer"},
                            }
                        },
                        {RPCResult::Type::OBJ, "musig2_session", "Current MuSig2 signing session status (operator diagnostic)",
                            {
                                {RPCResult::Type::NUM, "epoch", "Current epoch number (block_height / nDDOracleEpochBlocks)"},
                                {RPCResult::Type::STR, "state", "Session state: none / created / nonces_collecting / nonces_complete / signing / complete / failed"},
                                {RPCResult::Type::NUM, "nonce_count", "Number of pubnonces collected for the current epoch's session"},
                                {RPCResult::Type::NUM, "partial_sig_count", "Number of partial signatures collected"},
                                {RPCResult::Type::NUM, "creation_height", /*optional=*/true, "Block height at which the current session was created (omitted when state=none)"}
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("getdigidollardeploymentinfo", "")
                    + HelpExampleRpc("getdigidollardeploymentinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // NOTE: getdigidollardeploymentinfo is intentionally NOT gated behind
            // activation. Users need this RPC to check when/whether DigiDollar
            // activates on their network.
            const ChainstateManager& chainman = EnsureAnyChainman(request.context);
            LOCK(cs_main);
            const Chainstate& active_chainstate = chainman.ActiveChainstate();
            const CBlockIndex* tip = active_chainstate.m_chain.Tip();

            UniValue result(UniValue::VOBJ);

            // Check if DigiDollar is currently enabled
            bool enabled = DigiDollar::IsDigiDollarEnabled(tip, chainman);
            result.pushKV("enabled", enabled);

            // DigiDollar is a buried deployment (BIP90): report the hardcoded
            // activation height instead of BIP9 signaling state. (The
            // pre-burial back-scan reported the last LOCKED_IN block, one
            // below the first active block; DeploymentHeight is exact.)
            const Consensus::Params& consensusParams = chainman.GetConsensus();
            const int activation_height = consensusParams.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);

            result.pushKV("type", "buried");
            result.pushKV("status", enabled ? "active" : "defined");
            if (activation_height != std::numeric_limits<int>::max()) {
                result.pushKV("activation_height", activation_height);
            }

            // Wave 12: keep the mandatory oracle/DD activation height distinct
            // from the MuSig2 format height. V1 sets the format gate to 0, but
            // oracle bundles become mandatory only when the DD/oracle block
            // rules activate for the network.
            result.pushKV("oracle_activation_height", consensusParams.nOracleActivationHeight);
            result.pushKV("musig2_format_activation_height", consensusParams.nDigiDollarMuSig2Height);
            result.pushKV("oracle_pubkey_count", consensusParams.nOraclePubkeyCount);
            result.pushKV("oracle_consensus_required", consensusParams.nOracleConsensusRequired);
            result.pushKV("oracle_total_slots", static_cast<int>(Params().GetOracleNodes().size()));

            UniValue oracle_seed_peers(UniValue::VARR);
            for (const auto& peer : Params().OracleSeedPeers()) {
                oracle_seed_peers.push_back(peer);
            }
            result.pushKV("oracle_seed_peers", oracle_seed_peers);

            // Wave 10 (Agent C): expose the orchestrator's MuSig2 session
            // status for the current epoch so operators can diagnose stuck
            // sessions (timeout / sub-quorum / liveness drift). The state
            // map is the orchestrator's private member; this RPC is the
            // only way to read it without grepping debug.log.
            UniValue session_obj(UniValue::VOBJ);
            const int32_t tip_height = tip ? tip->nHeight : 0;
            const int32_t current_epoch = GetCurrentEpoch(tip_height);
            session_obj.pushKV("epoch", current_epoch);
            std::string state_str = "none";
            int64_t nonce_count = 0;
            int64_t partial_sig_count = 0;
            std::optional<int32_t> creation_height;
            if (g_signing_orchestrator) {
                auto status = g_signing_orchestrator->GetSessionStateForEpoch(current_epoch);
                if (status.has_value()) {
                    nonce_count = static_cast<int64_t>(status->nonce_count);
                    partial_sig_count = static_cast<int64_t>(status->partial_sig_count);
                    creation_height = status->creation_height;
                    switch (status->state) {
                        case MuSig2SessionState::CREATED:           state_str = "created"; break;
                        case MuSig2SessionState::NONCES_COLLECTING: state_str = "nonces_collecting"; break;
                        case MuSig2SessionState::NONCES_COMPLETE:   state_str = "nonces_complete"; break;
                        case MuSig2SessionState::SIGNING:           state_str = "signing"; break;
                        case MuSig2SessionState::COMPLETE:          state_str = "complete"; break;
                        case MuSig2SessionState::FAILED:            state_str = "failed"; break;
                    }
                }
            }
            session_obj.pushKV("state", state_str);
            session_obj.pushKV("nonce_count", nonce_count);
            session_obj.pushKV("partial_sig_count", partial_sig_count);
            if (creation_height.has_value()) {
                session_obj.pushKV("creation_height", *creation_height);
            }
            result.pushKV("musig2_session", session_obj);

            return result;
        },
    };
}

// =============================================================================
// CORE RPC COMMANDS (Task 5.7)
// =============================================================================

#ifdef ENABLE_WALLET
RPCHelpMan mintdigidollar()
{
    return RPCHelpMan{"mintdigidollar",
                "\nMint new DigiDollar with DGB collateral.\n"
                "Creates a new DigiDollar position by locking DGB as collateral.\n"
                "The amount of collateral required depends on the lock period and current system health.\n",
                {
                    {"dd_amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "Amount of DigiDollar to mint in cents (min 10000/$100, max 10000000/$100K)", RPCArgOptions{.skip_type_check = true}},
                    {"lock_tier", RPCArg::Type::NUM, RPCArg::Optional::NO, "Lock tier 0-9 (0=1h, 1=30d, 2=90d, 3=180d, 4=1y, 5=2y, 6=3y, 7=5y, 8=7y, 9=10y)", RPCArgOptions{.skip_type_check = true}},
                    {"fee_rate", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Fee rate in sat/kB; values below 35000000 are floored to 35000000 to satisfy the DD fee floor", RPCArgOptions{.skip_type_check = true}}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "Transaction ID of the mint transaction"},
                        {RPCResult::Type::STR_AMOUNT, "dd_minted", "Amount of DigiDollar minted (in cents)"},
                        {RPCResult::Type::STR_AMOUNT, "dgb_collateral", "DGB locked as collateral"},
                        {RPCResult::Type::NUM, "lock_tier", "Lock tier used"},
                        {RPCResult::Type::NUM, "unlock_height", "Block height when collateral becomes unlockable"},
                        {RPCResult::Type::NUM, "collateral_ratio", "Effective collateral ratio percentage"},
                        {RPCResult::Type::STR_AMOUNT, "fee_paid", "Transaction fee paid"},
                        {RPCResult::Type::STR, "position_id", "Unique position identifier"},
                        {RPCResult::Type::STR_HEX, "consolidation_txid", /*optional=*/true, "TXID of auto-consolidation transaction (only present if UTXOs were consolidated)"},
                        {RPCResult::Type::BOOL, "utxos_consolidated", /*optional=*/true, "True if wallet UTXOs were auto-consolidated before minting"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("mintdigidollar", "10000 3") +
                    HelpExampleCli("mintdigidollar", "50000 5 35000000") +
                    HelpExampleRpc("mintdigidollar", "10000, 3") +
                    HelpExampleRpc("mintdigidollar", "50000, 5, 35000000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Get wallet first (wallet RPCs have WalletContext, not NodeContext)
            std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "No wallet is loaded");

            // Check DigiDollar activation via wallet's chain interface
            node::NodeContext* node_ctx = pwallet->chain().context();
            if (!node_ctx) throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
            int currentHeight = 0;
            {
                ChainstateManager& chainman = *node_ctx->chainman;
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
                currentHeight = tip ? tip->nHeight : 0;
            }

            // DD-FA-FUNC-028 (Wave 18 Agent C): surface a DigiDollar-flavored
            // locked-wallet hint that explicitly cites walletpassphrase so
            // wallet UIs can disambiguate this rejection from any other
            // locked-wallet failure. The legacy upstream substring
            // "Please enter the wallet passphrase with walletpassphrase first"
            // is preserved verbatim for backward compatibility with
            // digidollar_encrypted_wallet.py.
            if (pwallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                    "DigiDollar mint requires the wallet to be unlocked. "
                    "Error: Please enter the wallet passphrase with walletpassphrase first.");
            }
            if (pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
            }

            // Parse parameters
            CAmount ddAmount = request.params[0].getInt<int64_t>();
            int lockTier = request.params[1].getInt<int>();

            // DigiDollar transactions MUST pay at least 0.1 DGB fee to miners
            // Use a high fee rate to ensure the minimum is met for all transaction sizes
            // MIN_DD_TX_FEE = 10,000,000 satoshis = 0.1 DGB
            // For a typical 300-byte tx, we need feeRate = 10,000,000 / 300 * 1000 = 33,333,333 sat/kB
            // We use 35,000,000 sat/kB to ensure minimum is always met
            static const CAmount MIN_DD_FEE_RATE = 35000000; // 0.35 DGB/kB ensures min 0.1 DGB for typical tx
            CAmount feeRate = OptionalParamIsSet(request, 2) ?
                std::max(request.params[2].getInt<int64_t>(), MIN_DD_FEE_RATE) : MIN_DD_FEE_RATE;

            // Validate parameters
            if (ddAmount <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "DigiDollar amount must be positive");
            }
            
            // Get consensus parameters for mint amount validation
            const auto& chainParams = Params();
            const auto& ddParams = chainParams.GetDigiDollarParams();
            
            // Validate against consensus mint limits
            if (!DigiDollar::IsValidMintAmount(ddAmount, ddParams)) {
                if (ddAmount < ddParams.minMintAmount) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, 
                        strprintf("Minimum mint amount is $%d (%d cents)", 
                            ddParams.minMintAmount / 100, ddParams.minMintAmount));
                } else {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, 
                        strprintf("Maximum mint amount is $%d (%d cents)", 
                            ddParams.maxMintAmount / 100, ddParams.maxMintAmount));
                }
            }
            
            if (lockTier < 0 || lockTier > 9) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Lock tier must be between 0 and 9 (0 = 1 hour testing tier)");
            }

            // Mempool validation checks DD mints against the next block height.
            const int mintHeight = currentHeight + 1;

            // Get oracle price in micro-USD from real oracle system first, fall back to mock only in regtest
            CAmount oraclePriceMicroUSD = OracleIntegration::GetCurrentOraclePriceMicroUSD();
            if (oraclePriceMicroUSD <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                MockOracleManager::GetInstance().IsEnabled()) {
                oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
            }
            if (oraclePriceMicroUSD <= 0) {
                throw JSONRPCError(RPC_MISC_ERROR, "No oracle price available. Start the oracle first with startoracle command.");
            }

            // ERR CHECK: Block minting during emergency state.
            // Mempool and ConnectBlock both call ShouldBlockMintingDuringERR()
            // (validation.cpp:2569), which delegates to
            // ERR::EmergencyRedemptionRatio::ShouldBlockMinting() and reads the
            // canonical network-wide cached metrics plus the explicit ERR state.
            // The wallet RPC must use the same source so a user cannot pass a
            // local-only check while consensus refuses the broadcast — and so a
            // wallet with zero DD positions still respects network ERR.
            if (DigiDollar::ERR::EmergencyRedemptionRatio::ShouldBlockMinting(oraclePriceMicroUSD)) {
                const DigiDollar::SystemMetrics metrics =
                    DigiDollar::SystemHealthMonitor::GetCachedMetrics();
                CAmount priceMillicents = oraclePriceMicroUSD / 10;
                int reportedHealth = metrics.systemHealth;
                if (!metrics.hasCanonicalHealth || reportedHealth <= 0) {
                    if (metrics.totalDDSupply > 0 && metrics.totalCollateral > 0 &&
                        priceMillicents > 0) {
                        reportedHealth = DynamicCollateralAdjustment::CalculateSystemHealth(
                            metrics.totalCollateral, metrics.totalDDSupply, priceMillicents);
                    }
                }
                LogPrintf("DigiDollar RPC Mint: Network ERR active - DD=%lld, "
                          "collateral=%lld, price_micro_usd=%lld, health=%d%%\n",
                          static_cast<long long>(metrics.totalDDSupply),
                          static_cast<long long>(metrics.totalCollateral),
                          static_cast<long long>(oraclePriceMicroUSD),
                          reportedHealth);
                throw JSONRPCError(RPC_MISC_ERROR,
                    strprintf("Minting blocked: System is in emergency state (health: %d%%). "
                              "Wait for system health to recover above 100%% before minting new DigiDollars.",
                              reportedHealth));
            }

            // Convert lock tier to days
            int lockDays = GetLockDaysForTier(lockTier);

            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet state is not initialized");
            }

            // Serialize mint coin selection, signing, commit, and DD position
            // persistence. This prevents concurrent mint RPC workers from
            // selecting the same wallet inputs from stale AvailableCoins()
            // snapshots before the first mint is committed.
            LOCK2(pwallet->cs_wallet, dd_wallet->cs_dd_wallet);

            // Get available UTXOs from wallet and build value map
            std::vector<COutPoint> availableUtxos;
            std::map<COutPoint, CAmount> utxoValues;
            {
                LOCK(pwallet->cs_wallet);
                wallet::CoinsResult coins = wallet::AvailableCoins(*pwallet);
                for (const wallet::COutput& coin : coins.All()) {
                    availableUtxos.push_back(coin.outpoint);
                    utxoValues[coin.outpoint] = coin.txout.nValue;
                }
            }

            if (availableUtxos.empty()) {
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "No available UTXOs for collateral");
            }

            // Generate owner key from wallet using HD derivation
            // This allows the key to be recovered from wallet seed
            CKey ownerKey;
            {
                LOCK(pwallet->cs_wallet);
                ownerKey = pwallet->GetHDKeyForDigiDollar("dd-owner");
                if (!ownerKey.IsValid()) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar mint requires a descriptor/bech32m HD wallet with private keys enabled");
                }
            }

            // Create custom MintTxBuilder that can look up actual UTXO values
            // This is critical - without this, SelectCoins uses hardcoded placeholder values
            // and selects hundreds of UTXOs, wasting millions of DGB!
            class RpcMintTxBuilder : public DigiDollar::MintTxBuilder {
            private:
                const std::map<COutPoint, CAmount>& m_utxo_values;
            public:
                RpcMintTxBuilder(const CChainParams& params, int height, CAmount price,
                               const std::map<COutPoint, CAmount>& utxo_values)
                    : MintTxBuilder(params, height, price), m_utxo_values(utxo_values) {}

                CAmount GetDGBFromUTXO(const COutPoint& outpoint) const override {
                    auto it = m_utxo_values.find(outpoint);
                    if (it != m_utxo_values.end()) {
                        return it->second;
                    }
                    return 0; // UTXO not found
                }
            };

            // Build mint transaction using custom RpcMintTxBuilder with UTXO value lookup
            // Note: MintTxBuilder now expects micro-USD price
            RpcMintTxBuilder builder(Params(), mintHeight, oraclePriceMicroUSD, utxoValues);

            DigiDollar::TxBuilderMintParams params;
            params.ddAmount = ddAmount;  // Amount in cents (e.g., 5000 = $50.00)
            params.lockDays = lockDays;
            params.lockTier = lockTier;  // Store tier explicitly in OP_RETURN for exact reconstruction
            params.ownerKey = ownerKey;
            params.feeRate = feeRate;
            params.utxos = availableUtxos;

            // CRITICAL FIX: Get a proper change address from the wallet for DGB change output
            // This ensures the wallet recognizes the change output as its own!
            {
                LOCK(pwallet->cs_wallet);
                auto op_dest = pwallet->GetNewChangeDestination(OutputType::BECH32);
                if (op_dest) {
                    params.dgbChangeDest = *op_dest;
                    LogPrintf("DigiDollar RPC Mint: Using wallet change address for DGB change output\n");
                } else {
                    LogPrintf("DigiDollar RPC Mint: WARNING - Could not get change destination!\n");
                }
            }

            DigiDollar::TxBuilderResult result = builder.BuildMintTransaction(params);

            // Auto-consolidate if mint failed due to UTXO fragmentation
            std::string consolidation_txid;
            if (!result.success && result.error.find("Too many small UTXOs") != std::string::npos) {
                LogPrintf("DigiDollar RPC Mint: UTXO fragmentation detected (%zu UTXOs). Auto-consolidating...\n",
                          availableUtxos.size());

                if (!pwallet->GetBroadcastTransactions()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                        "Auto-consolidation requires wallet transaction broadcast to be enabled");
                }

                auto sort_available_utxos_by_value = [&]() {
                    std::sort(availableUtxos.begin(), availableUtxos.end(),
                        [&](const COutPoint& a, const COutPoint& b) {
                            const CAmount av = utxoValues.count(a) ? utxoValues.at(a) : 0;
                            const CAmount bv = utxoValues.count(b) ? utxoValues.at(b) : 0;
                            if (av != bv) return av > bv;
                            return a < b;
                        });
                };

                auto commit_consolidation = [&](const CTransactionRef& consolidation_tx) {
                    std::string commit_error;
                    bool commit_success = false;
                    {
                        LOCK(pwallet->cs_wallet);
                        commit_success = pwallet->CommitTransaction(consolidation_tx, {}, {}, &commit_error);
                    }
                    if (!commit_success) {
                        throw JSONRPCError(RPC_TRANSACTION_REJECTED,
                            strprintf("Auto-consolidation transaction rejected by mempool: %s", commit_error));
                    }
                };

                sort_available_utxos_by_value();

                CAmount totalAvailable = 0;
                for (const auto& [outpoint, value] : utxoValues) {
                    totalAvailable += value;
                }
                CAmount minRequired = result.collateralRequired + 20000000;
                if (totalAvailable < minRequired) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                        strprintf("Insufficient funds for collateral. Need %.2f DGB, have %.2f DGB.",
                                  result.collateralRequired / 100000000.0,
                                  totalAvailable / 100000000.0));
                }

                CTxDestination consolidationDest;
                {
                    LOCK(pwallet->cs_wallet);
                    auto op_dest = pwallet->GetNewChangeDestination(OutputType::BECH32);
                    if (!op_dest) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to get consolidation address");
                    }
                    consolidationDest = *op_dest;
                }

                // Multi-pass consolidation: MAX_STANDARD_TX_WEIGHT is 400k WU.
                // P2WPKH input ≈ 271 WU. Conservative limit: 1400 inputs per pass.
                static const size_t MAX_CONSOLIDATION_INPUTS = 1400;
                static const int MAX_CONSOLIDATION_PASSES = 10;
                int pass = 0;
                std::vector<COutPoint> consolidatedUtxos;
                std::map<COutPoint, CAmount> consolidatedValues;

                for (size_t offset = 0; offset < availableUtxos.size() && pass < MAX_CONSOLIDATION_PASSES;) {
                    ++pass;
                    size_t batch_size = std::min(availableUtxos.size() - offset, MAX_CONSOLIDATION_INPUTS);
                    LogPrintf("DigiDollar RPC Mint: Consolidation pass %d — sweeping %zu of %zu UTXOs\n",
                              pass, batch_size, availableUtxos.size());

                    wallet::CCoinControl coin_control;
                    CAmount batchTotal = 0;
                    for (size_t i = 0; i < batch_size; ++i) {
                        const COutPoint& utxo = availableUtxos[offset + i];
                        coin_control.Select(utxo);
                        batchTotal += utxoValues[utxo];
                    }
                    coin_control.m_allow_other_inputs = false;

                    wallet::CRecipient recipient{consolidationDest, batchTotal, /*subtract_fee=*/true};
                    std::vector<wallet::CRecipient> recipients = {recipient};

                    auto consolidation_result = wallet::CreateTransaction(*pwallet, recipients, /*change_pos=*/-1, coin_control, /*sign=*/true);
                    if (!consolidation_result) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                            strprintf("Auto-consolidation pass %d failed: %s. Try manually consolidating UTXOs.",
                                      pass, util::ErrorString(consolidation_result).original));
                    }

                    const CTransactionRef& consolidation_tx = consolidation_result->tx;
                    consolidation_txid = consolidation_tx->GetHash().GetHex();
                    commit_consolidation(consolidation_tx);

                    LogPrintf("DigiDollar RPC Mint: Consolidation pass %d tx: %s (swept %.2f DGB from %zu inputs)\n",
                              pass, consolidation_txid, batchTotal / 100000000.0, batch_size);

                    COutPoint consolidated_outpoint(consolidation_tx->GetHash(), 0);
                    consolidatedUtxos.push_back(consolidated_outpoint);
                    consolidatedValues[consolidated_outpoint] = consolidation_tx->vout[0].nValue;
                    offset += batch_size;
                }

                if (consolidatedUtxos.empty() || consolidatedUtxos.size() > MAX_CONSOLIDATION_PASSES) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                        "Auto-consolidation failed: too many fragmented UTXOs. Try manually consolidating UTXOs.");
                }

                availableUtxos = std::move(consolidatedUtxos);
                utxoValues = std::move(consolidatedValues);

                LogPrintf("DigiDollar RPC Mint: After consolidation: %zu UTXOs available (passes: %d)\n",
                          availableUtxos.size(), pass);

                RpcMintTxBuilder retryBuilder(Params(), mintHeight, oraclePriceMicroUSD, utxoValues);
                params.utxos = availableUtxos;
                result = retryBuilder.BuildMintTransaction(params);
            }

            if (!result.success) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to build mint transaction: " + result.error);
            }

            // Sign transaction
            bool signSuccess = false;
            {
                LOCK(pwallet->cs_wallet);
                signSuccess = pwallet->SignTransaction(result.tx);
            }

            if (!signSuccess) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign mint transaction");
            }

            // Create transaction reference for commitment
            CTransactionRef tx = MakeTransactionRef(result.tx);
            const uint256 positionId = tx->GetHash();

            const bool should_broadcast = pwallet->GetBroadcastTransactions();
            if (should_broadcast) {
                RefreshRegtestMockMuSig2QuoteForMempool(*pwallet);
            }

            // Commit through the wallet-owned relay path exactly once so the
            // wallet state transition and mempool submission stay in sync.
            // If wallet broadcasting is disabled (for example -blocksonly
            // soft-sets -walletbroadcast=0), CommitTransaction still records a
            // wallet-local transaction. Keep DD metadata for that local tx so
            // RPC/Qt can show it as confirming instead of losing the vault.
            std::string commit_error;
            bool commit_success = false;
            {
                LOCK(pwallet->cs_wallet);
                commit_success = pwallet->CommitTransaction(tx, {}, {}, &commit_error);
            }
            if (should_broadcast && !commit_success) {
                if (pwallet->TransactionCanBeAbandoned(positionId)) {
                    pwallet->AbandonTransaction(positionId);
                    LogPrintf("DigiDollar RPC Mint: Abandoned rejected local mint transaction %s\n",
                              positionId.ToString());
                }
                throw JSONRPCError(RPC_TRANSACTION_REJECTED,
                    strprintf("Mint transaction rejected by mempool: %s", commit_error));
            }
            if (!commit_success) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Mint transaction was not committed to the wallet: %s", commit_error));
            }

            // Calculate unlock height using consensus function (handles tier 0 special case)
            int64_t lockBlocks = DigiDollar::LockDaysToBlocks(lockDays);
            int unlockHeight = mintHeight + lockBlocks + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;

            // CRITICAL FIX: Persist DD position to DigiDollarWallet after the
            // wallet accepts the transaction, even when the mint is local-only
            // and waiting for manual broadcast/rebroadcast.
            if (commit_success && dd_wallet) {
                WalletCollateralPosition position;
                position.dd_timelock_id = positionId;
                position.dgb_collateral = result.collateralRequired;
                position.dd_minted = ddAmount;
                position.lock_tier = lockTier;
                position.unlock_height = unlockHeight;
                position.is_active = true;
                position.owner_keyid = ownerKey.GetPubKey().GetID();

                LOCK(pwallet->cs_wallet);
                dd_wallet->StoreOwnerKey(positionId, ownerKey);
                dd_wallet->AddCollateralPosition(position);

                // CRITICAL FIX: Track the DD UTXO so it can be found by GetDDUTXOs().
                COutPoint ddOutpoint(positionId, 1);
                dd_wallet->GetMintDDTokenOutpoint(positionId, ddOutpoint);
                dd_wallet->AddDDUTXO(ddOutpoint, ddAmount);

                // CRITICAL FIX #2: Persist DD UTXO to wallet database so it survives daemon restart
                {
                    wallet::WalletBatch batch(pwallet->GetDatabase());
                    if (batch.WriteDDUTXO(ddOutpoint, ddAmount)) {
                        LogPrintf("DigiDollar RPC: Persisted DD UTXO %s:%d to database (amount=%d)\n",
                                 ddOutpoint.hash.ToString(), ddOutpoint.n, ddAmount);
                    } else {
                        LogPrintf("DigiDollar RPC: WARNING - Failed to persist DD UTXO to database\n");
                    }
                }

                LogPrintf("DigiDollar RPC: Added position %s with %d DD cents, stored owner key, and tracked DD UTXO at vout %u\n",
                         position.dd_timelock_id.ToString(), ddAmount, ddOutpoint.n);
            } else {
                LogPrintf("DigiDollar RPC: DD position not persisted (committed=%d, broadcast=%d, ddwallet=%d)\n",
                          commit_success ? 1 : 0, should_broadcast ? 1 : 0, dd_wallet ? 1 : 0);
            }

            const int baseRatio = DigiDollar::GetCollateralRatioForLockTime(
                DigiDollar::LockDaysToBlocks(lockDays), ddParams);
            const DigiDollar::SystemMetrics ratioMetrics =
                DigiDollar::SystemHealthMonitor::GetCachedMetrics();
            int systemHealth = 30000;
            if (ratioMetrics.hasCanonicalHealth && ratioMetrics.systemHealth > 0) {
                systemHealth = ratioMetrics.systemHealth;
            } else if (ratioMetrics.totalDDSupply > 0 &&
                       ratioMetrics.totalCollateral > 0 &&
                       oraclePriceMicroUSD > 0) {
                systemHealth = DynamicCollateralAdjustment::CalculateSystemHealth(
                    ratioMetrics.totalCollateral,
                    ratioMetrics.totalDDSupply,
                    oraclePriceMicroUSD / 10);
            }
            const int collateralRatio = DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemHealth);
            if (collateralRatio <= 0 || collateralRatio == std::numeric_limits<int>::max()) {
                throw JSONRPCError(RPC_MISC_ERROR, "DCA collateral ratio calculation failed");
            }

            UniValue resultObj(UniValue::VOBJ);
            resultObj.pushKV("txid", positionId.GetHex());
            resultObj.pushKV("dd_minted", int64_t{ddAmount});
            resultObj.pushKV("dgb_collateral", ValueFromAmount(result.collateralRequired));
            resultObj.pushKV("lock_tier", lockTier);
            resultObj.pushKV("unlock_height", unlockHeight);
            resultObj.pushKV("collateral_ratio", collateralRatio);
            resultObj.pushKV("fee_paid", ValueFromAmount(result.totalFees));
            resultObj.pushKV("position_id", positionId.GetHex());
            if (!consolidation_txid.empty()) {
                resultObj.pushKV("consolidation_txid", consolidation_txid);
                resultObj.pushKV("utxos_consolidated", true);
            }

            return resultObj;
        },
    };
}

RPCHelpMan senddigidollar()
{
    return RPCHelpMan{"senddigidollar",
                "\nSend DigiDollar to another DigiDollar address.\n"
                "Creates a transaction that transfers DigiDollar from your wallet to the specified address.\n"
                "Amounts may be integer cents (for example 10000 = $100.00) or decimal dollars (for example 100.00 = $100.00).\n"
                "A value written with a decimal point is always interpreted as dollars, so 10000.00 means $10,000.00, not $100.00.\n"
                "This is the primary RPC command for Phase 7.7 - DD transfers via API.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "DigiDollar address to send to (DD/TD/RD prefix)"},
                    {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "Amount to send: integer cents (e.g. 10000 = $100.00) OR decimal dollars (e.g. 100.00 = $100.00). A decimal point means dollars, so 10000.00 = $10,000.00.", RPCArgOptions{.skip_type_check = true}},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Optional comment for the transaction"},
                    {"fee_rate", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Deprecated compatibility argument; ignored because DigiDollar sends use the fixed DD fee policy", RPCArgOptions{.skip_type_check = true}},
                    {"selected_inputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Optional DigiDollar inputs to spend, matching listdigidollarunspent output",
                        {
                            {"input", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                },
                            },
                        },
                    },
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "Transaction ID"},
                        {RPCResult::Type::STR, "to_address", "Recipient DigiDollar address"},
                        {RPCResult::Type::NUM, "amount", "Amount sent (in cents)"},
                        {RPCResult::Type::STR, "status", "Transaction status (success/pending/failed)"},
                        {RPCResult::Type::STR_AMOUNT, "fee_paid", "Transaction fee paid in DGB (optional)"},
                        {RPCResult::Type::NUM, "inputs_used", "Number of DD inputs consumed (optional)"},
                        {RPCResult::Type::NUM, "change_amount", "DD change amount in cents if any (optional)"},
                        {RPCResult::Type::STR, "comment", /*optional=*/true, "Optional wallet comment"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("senddigidollar", "\"DDtestaddress123456789abcdef\" 5000") +
                    HelpExampleCli("senddigidollar", "\"DDtestaddress123456789abcdef\" 5000 \"Payment for services\"") +
                    HelpExampleRpc("senddigidollar", "\"DDtestaddress123456789abcdef\", 5000") +
                    HelpExampleRpc("senddigidollar", "\"DDtestaddress123456789abcdef\", 5000, \"Payment for services\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            LogPrintf("DigiDollar RPC: senddigidollar called\n");

            // Get wallet first (wallet RPCs have WalletContext, not NodeContext)
            std::shared_ptr<wallet::CWallet> const pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not found");
            }

            // Check DigiDollar activation via wallet's chain interface
            {
                node::NodeContext* node_ctx = pwallet->chain().context();
                if (!node_ctx) throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
                ChainstateManager& chainman = *node_ctx->chainman;
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }

            // DD-FA-FUNC-028 (Wave 18 Agent C): DD-flavored locked-wallet
            // hint, preserving the legacy "walletpassphrase" substring for
            // backward compatibility with digidollar_encrypted_wallet.py.
            if (pwallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                    "DigiDollar send requires the wallet to be unlocked. "
                    "Error: Please enter the wallet passphrase with walletpassphrase first.");
            }
            if (pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
            }

            LogPrintf("DigiDollar RPC: Got wallet\n");

            // Get DigiDollar wallet
            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            }
            LogPrintf("DigiDollar RPC: Got DD wallet\n");

            // Parse parameters
            std::string addressStr = request.params[0].get_str();

            // Bug #18 fix: Accept both integer cents and decimal dollars.
            // Integer values (e.g. 5000) are treated as cents.
            // Fractional values (e.g. 50.00) are treated as dollars and converted to cents.
            // String values are also handled gracefully.
            CAmount amount = ParseDigiDollarRpcAmount(request.params[1]);
            std::string comment = OptionalParamIsSet(request, 2) ? request.params[2].get_str() : "";
            std::vector<COutPoint> selected_inputs;
            const std::vector<COutPoint>* preset_dd_inputs = nullptr;
            if (OptionalParamIsSet(request, 4)) {
                selected_inputs = ParseDigiDollarSelectedInputs(request.params[4]);
                preset_dd_inputs = &selected_inputs;
            }
            LogPrintf("DigiDollar RPC: Parsed params - address=%s, amount=%d\n", addressStr, amount);

            // Validate amount
            if (amount <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be positive");
            }

            // Parse and validate DD address
            std::string address_error;
            if (!ValidateDigiDollarAddressForCurrentNetwork(addressStr, address_error)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, address_error);
            }
            CDigiDollarAddress dd_address(addressStr);
            LogPrintf("DigiDollar RPC: DD address validated\n");

            // Check balance
            LogPrintf("DigiDollar RPC: Calling GetTotalDDBalance()...\n");
            CAmount balance = dd_wallet->GetTotalDDBalance();
            LogPrintf("DigiDollar RPC: GetTotalDDBalance() returned %d\n", balance);
            if (amount > balance) {
                const CAmount pending_balance = dd_wallet->GetPendingDDBalance();
                if (amount <= balance + pending_balance) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                        "Insufficient confirmed DD balance; please wait for prior DigiDollar transfer confirmation and try again.");
                }
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                    strprintf("Insufficient DD balance (have %d cents, need %d cents)",
                             balance, amount));
            }

            RefreshRegtestMockMuSig2QuoteForMempool(*pwallet);

            // Execute transfer using the wallet backend.
            std::string txid;
            std::string error;
            CAmount dd_change = 0;
            LogPrintf("DigiDollar RPC: Calling TransferDigiDollar()...\n");
            bool success = dd_wallet->TransferDigiDollar(dd_address, amount, txid, error, &dd_change, preset_dd_inputs, comment);
            LogPrintf("DigiDollar RPC: TransferDigiDollar() returned success=%d\n", success);

            if (!success) {
                // Bug #10: Provide user-friendly message for unconfirmed DD input errors
                if (error.find("dd-input-amounts-unknown") != std::string::npos) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                        "Previous DigiDollar transfer has not confirmed yet. Please wait for confirmation and try again.");
                }
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Transfer failed: %s", error));
            }

            // Build result
            UniValue result(UniValue::VOBJ);
            result.pushKV("txid", txid);
            result.pushKV("to_address", addressStr);
            result.pushKV("amount", amount);  // Bug #11/25 fix: raw integer cents, not ValueFromAmount
            result.pushKV("status", "success");

            // Bug #11/25 fix: Compute actual fee, inputs, and change from the wallet transaction
            {
                uint256 hash;
                hash.SetHex(txid);
                LOCK(pwallet->cs_wallet);
                auto it = pwallet->mapWallet.find(hash);
                if (it != pwallet->mapWallet.end()) {
                    const wallet::CWalletTx& wtx = it->second;
                    CAmount debit = wallet::CachedTxGetDebit(*pwallet, wtx, wallet::ISMINE_ALL);
                    CAmount credit = wallet::CachedTxGetCredit(*pwallet, wtx, wallet::ISMINE_ALL);
                    CAmount fee = debit - credit;
                    result.pushKV("fee_paid", ValueFromAmount(fee > 0 ? fee : 0));
                    result.pushKV("inputs_used", static_cast<int>(wtx.tx->vin.size()));
                } else {
                    result.pushKV("fee_paid", ValueFromAmount(0));
                    result.pushKV("inputs_used", 0);
                }
            }
            result.pushKV("change_amount", dd_change);

            // Optional: Add comment to wallet transaction if provided
            if (!comment.empty()) {
                result.pushKV("comment", comment);
            }

            return result;
        },
	    };
}

RPCHelpMan sendmanydigidollar()
{
    return RPCHelpMan{"sendmanydigidollar",
                "\nSend DigiDollar to multiple DigiDollar addresses in one transaction.\n"
                "Amounts may be integer cents (for example 5000 = $50.00) or decimal dollars (for example 50.25).\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Default{"\"\""}, "Must be set to \"\" for compatibility with sendmany."},
                    {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "DigiDollar addresses and amounts",
                        {
                            {"address", RPCArg::Type::NUM, RPCArg::Optional::NO, "The DigiDollar address is the key; the amount is integer cents or decimal dollars", RPCArgOptions{.skip_type_check = true}},
                        },
                    },
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Optional comment for the transaction"},
                    {"selected_inputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Optional DigiDollar inputs to spend, matching listdigidollarunspent output",
                        {
                            {"input", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                },
                            },
                        },
                    },
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "Transaction ID"},
                        {RPCResult::Type::OBJ_DYN, "amounts", "Amounts sent by DigiDollar address",
                            {
                                {RPCResult::Type::NUM, "address", "Amount sent to this address in cents"},
                            },
                        },
                        {RPCResult::Type::NUM, "total_amount", "Total amount sent in cents"},
                        {RPCResult::Type::STR, "status", "Transaction status (success/pending/failed)"},
                        {RPCResult::Type::STR, "comment", /*optional=*/true, "Optional wallet comment"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("sendmanydigidollar", "\"\" \"{\\\"DDtestaddress123456789abcdef\\\":5000,\\\"DDtestaddressabcdef123456789\\\":2500}\"") +
                    HelpExampleRpc("sendmanydigidollar", "\"\", {\"DDtestaddress123456789abcdef\":5000,\"DDtestaddressabcdef123456789\":2500}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            LogPrintf("DigiDollar RPC: sendmanydigidollar called\n");

            std::shared_ptr<wallet::CWallet> const pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not found");
            }

            {
                node::NodeContext* node_ctx = pwallet->chain().context();
                if (!node_ctx) throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
                ChainstateManager& chainman = *node_ctx->chainman;
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }

            // DD-FA-FUNC-028 (Wave 18 Agent C): DD-flavored locked-wallet
            // hint for sendmanydigidollar (matches mintdigidollar /
            // senddigidollar / redeemdigidollar / getdigidollaraddress /
            // createoraclekey). Preserves the legacy "walletpassphrase"
            // substring for backward compatibility with
            // digidollar_encrypted_wallet.py.
            if (pwallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                    "DigiDollar send requires the wallet to be unlocked. "
                    "Error: Please enter the wallet passphrase with walletpassphrase first.");
            }
            if (pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
            }

            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            }

            if (OptionalParamIsSet(request, 0) && !request.params[0].get_str().empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
            }

            const UniValue& amounts = request.params[1].get_obj();
            if (amounts.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "No recipients specified");
            }

            std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
            UniValue result_amounts(UniValue::VOBJ);
            CAmount total_amount = 0;
            std::set<std::string> seen_addresses;

            const std::vector<std::string>& keys = amounts.getKeys();
            const std::vector<UniValue>& values = amounts.getValues();
            for (size_t i = 0; i < keys.size(); ++i) {
                std::string address_error;
                if (!ValidateDigiDollarAddressForCurrentNetwork(keys[i], address_error)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, address_error + ": " + keys[i]);
                }
                if (!seen_addresses.insert(keys[i]).second) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, duplicated address: " + keys[i]);
                }
                CDigiDollarAddress dd_address(keys[i]);

                CAmount amount = ParseDigiDollarRpcAmount(values[i]);
                if (amount <= 0) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be positive");
                }
                if (amount > 10000000) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount exceeds maximum transfer limit ($100,000)");
                }
                if (total_amount > std::numeric_limits<CAmount>::max() - amount) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Total amount overflow");
                }

                recipients.push_back({dd_address, amount});
                result_amounts.pushKV(keys[i], amount);
                total_amount += amount;
            }

            // Preflight OP_RETURN capacity before wallet coin selection/build.
            // Include a possible DD change amount so boundary behavior is deterministic
            // and users get an actionable invalid-parameter error instead of a late
            // mempool/wallet failure.
            {
                CScript metadata;
                metadata << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(2);
                for (const auto& [address, amount] : recipients) metadata << CScriptNum(amount);
                metadata << CScriptNum(1); // possible DD change output amount
                if (metadata.size() > MAX_OP_RETURN_RELAY) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                        strprintf("Too many DigiDollar recipients for one transaction: projected metadata is %u bytes, standard relay limit is %u bytes. Reduce recipients or split this into multiple sendmanydigidollar calls.",
                                  static_cast<unsigned>(metadata.size()), MAX_OP_RETURN_RELAY));
                }
            }

            CAmount balance = dd_wallet->GetTotalDDBalance();
            if (total_amount > balance) {
                const CAmount pending_balance = dd_wallet->GetPendingDDBalance();
                if (total_amount <= balance + pending_balance) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                        "Insufficient confirmed DD balance; please wait for prior DigiDollar transfer confirmation and try again.");
                }
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                    strprintf("Insufficient DD balance (have %d cents, need %d cents)",
                             balance, total_amount));
            }
            std::vector<COutPoint> selected_inputs;
            const std::vector<COutPoint>* preset_dd_inputs = nullptr;
            if (OptionalParamIsSet(request, 3)) {
                selected_inputs = ParseDigiDollarSelectedInputs(request.params[3]);
                preset_dd_inputs = &selected_inputs;
            }

            RefreshRegtestMockMuSig2QuoteForMempool(*pwallet);

            std::string txid;
            std::string error;
            std::string comment = OptionalParamIsSet(request, 2) ? request.params[2].get_str() : "";
            bool success = dd_wallet->TransferDigiDollarMany(recipients, txid, error, nullptr, preset_dd_inputs, comment);
            if (!success) {
                if (error.find("dd-input-amounts-unknown") != std::string::npos) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                        "Previous DigiDollar transfer has not confirmed yet. Please wait for confirmation and try again.");
                }
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Transfer failed: %s", error));
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("txid", txid);
            result.pushKV("amounts", result_amounts);
            result.pushKV("total_amount", total_amount);
            result.pushKV("status", "success");
            if (OptionalParamIsSet(request, 2) && !request.params[2].get_str().empty()) {
                result.pushKV("comment", request.params[2].get_str());
            }

            return result;
        },
    };
}

RPCHelpMan redeemdigidollar()
{
    return RPCHelpMan{"redeemdigidollar",
                "\nRedeem DigiDollar and unlock DGB collateral.\n"
                "Burns DigiDollar tokens and unlocks the corresponding DGB collateral.\n"
                "Only positions that have reached maturity can be redeemed.\n",
                {
                    {"position_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Position ID (transaction hash of mint)"},
                    {"dd_amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Amount of DD to redeem (in cents)"},
                    {"redemption_address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "DGB address to receive unlocked collateral (default: new address)"},
                    {"fee_rate", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Deprecated compatibility argument; ignored because DigiDollar redemptions use the fixed DD fee policy", RPCArgOptions{.skip_type_check = true}}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "Redemption transaction ID"},
                        {RPCResult::Type::STR, "position_id", "Original position ID"},
                        {RPCResult::Type::STR_AMOUNT, "dd_redeemed", "Amount of DD redeemed (burned)"},
                        {RPCResult::Type::STR_AMOUNT, "total_dd_minted", "Original DD minted by this position"},
                        {RPCResult::Type::STR_AMOUNT, "required_dd_burn", "DD amount required by normal or ERR redemption"},
                        {RPCResult::Type::STR_AMOUNT, "dgb_unlocked", "Amount of DGB unlocked"},
                        {RPCResult::Type::STR, "unlock_address", "DGB address that received unlocked collateral"},
                        {RPCResult::Type::STR_AMOUNT, "fee_paid", "Transaction fee paid"},
                        {RPCResult::Type::STR, "redemption_path", "Redemption path used (normal/emergency/liquidation)"},
                        {RPCResult::Type::BOOL, "err_active", "Whether ERR burn rules were active"},
                        {RPCResult::Type::NUM, "err_system_health", "System health percentage used for ERR"},
                        {RPCResult::Type::NUM, "err_ratio_bps", "ERR ratio in basis points"},
                        {RPCResult::Type::BOOL, "position_closed", "Whether the position was fully closed"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("redeemdigidollar", "\"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\" 5000") +
                    HelpExampleCli("redeemdigidollar", "\"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\" 5000 \"DGb1A2B3C4D5E6F7G8H9I0J1K2L3M4N5O6P7Q8R9S0\"") +
                    HelpExampleRpc("redeemdigidollar", "\"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\", 5000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Parse parameters
            std::string positionIdStr = request.params[0].get_str();
            CAmount ddAmount = ParseDigiDollarRpcAmount(request.params[1]);
            std::string redeemAddress = OptionalParamIsSet(request, 2) ? request.params[2].get_str() : "";

            // Validate parameters
            if (ddAmount <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Redemption amount must be positive");
            }

            if (!IsHex(positionIdStr) || positionIdStr.length() != 64) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid position ID format");
            }

            // Get wallet (wallet RPCs have WalletContext, not NodeContext)
            std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet not found");

            // Check DigiDollar activation via wallet's chain interface
            {
                node::NodeContext* node_ctx = pwallet->chain().context();
                if (!node_ctx) throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
                ChainstateManager& chainman = *node_ctx->chainman;
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }

            // DD-FA-FUNC-025 (Wave 17 Agent C): surface a DigiDollar-flavored
            // hint that explicitly cites walletpassphrase so wallet UIs can
            // disambiguate this failure from a generic locked-wallet
            // rejection. The substring "walletpassphrase" is preserved for
            // backward compatibility with existing tests.
            if (pwallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                    "DigiDollar redemption requires the wallet to be unlocked. "
                    "Please enter the wallet passphrase with walletpassphrase first.");
            }
            if (pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
            }

            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            pwallet->BlockUntilSyncedToCurrentChain();
            dd_wallet->ReconcilePositionStates();

            // Parse position ID
            uint256 positionId;
            if (!ParseHashStr(positionIdStr, positionId)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid position ID");
            }

            LogPrintf("DigiDollar: ====== REDEMPTION REQUEST ======\n");
            LogPrintf("DigiDollar: Position ID (mint txid): %s\n", positionId.ToString());
            LogPrintf("DigiDollar: Resolving mint collateral and DD token outpoints from wallet metadata\n");

            // Get position from wallet
            LOCK(pwallet->cs_wallet);
            WalletCollateralPosition foundPosition;
            bool found = false;

            for (const auto& pos : dd_wallet->GetDDTimeLocks(false)) {
                if (pos.dd_timelock_id == positionId) {
                    foundPosition = pos;
                    found = true;
                    break;
                }
            }

            if (!found) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Position not found");
            }

            if (!dd_wallet->RefreshPositionMetadataFromMintTx(positionId)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    "Cannot verify DigiDollar mint metadata for this position. "
                    "Rescan or restore the wallet before redeeming.");
            }
            found = false;
            for (const auto& pos : dd_wallet->GetDDTimeLocks(false)) {
                if (pos.dd_timelock_id == positionId) {
                    foundPosition = pos;
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Position not found after metadata repair");
            }

            COutPoint collateralOutpoint;
            if (!dd_wallet->GetMintCollateralOutpoint(positionId, collateralOutpoint)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    "Cannot resolve DigiDollar collateral output for this position. "
                    "Rescan or restore the wallet before redeeming.");
            }
            LogPrintf("DigiDollar: Will try to spend collateral %s:%u\n",
                      collateralOutpoint.hash.ToString(), collateralOutpoint.n);

            // Check if redeemable
            int currentHeight = pwallet->GetLastBlockHeight();
            if (foundPosition.unlock_height > currentHeight) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Position locked until block %d (current: %d, remaining: %d blocks)",
                              foundPosition.unlock_height, currentHeight, foundPosition.unlock_height - currentHeight));
            }

            // EXACT-AMOUNT REDEMPTION ENFORCEMENT: Must redeem full vault amount
            // Partial redemption is no longer supported - vault must be closed completely
            if (ddAmount != foundPosition.dd_minted) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Exact-amount redemption required: must redeem full vault amount of %d cents (requested: %d cents). "
                              "Partial redemption is not supported - the entire vault must be closed at once.",
                              foundPosition.dd_minted, ddAmount));
            }

            int redemptionSystemHealth = DynamicCollateralAdjustment::GetCurrentSystemHealth();
            auto errState = DigiDollar::ERR::EmergencyRedemptionRatio::GetCurrentState();
            if (errState.isActive && errState.systemHealth < 100) {
                redemptionSystemHealth = errState.systemHealth;
            }

            const bool errRedemptionActive = redemptionSystemHealth >= 0 && redemptionSystemHealth < 100;
            const CAmount requiredDDBurn = errRedemptionActive
                ? DigiDollar::ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(foundPosition.dd_minted, redemptionSystemHealth)
                : foundPosition.dd_minted;
            const int errRatioBps = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRRatioBps(redemptionSystemHealth);

            // CRITICAL FIX: DD tokens are fungible - any DD can be used to redeem a vault
            // Check if user has enough DD balance (from any source) to cover redemption
            std::vector<COutPoint> selectedDDUtxos;
            std::vector<CAmount> selectedDDAmounts;  // CRITICAL: Need amounts for DD change calculation
            CAmount selectedDDTotal = 0;
            if (!dd_wallet->SelectDDCoins(requiredDDBurn, selectedDDUtxos, selectedDDTotal, &selectedDDAmounts)) {
                CAmount walletBalance = dd_wallet->GetDDBalance();
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Insufficient DD balance for redemption. Need %d cents, have %d cents. "
                              "You can use DD from any source to redeem a vault.",
                              requiredDDBurn, walletBalance));
            }
            LogPrintf("DigiDollar: Selected %zu DD UTXOs totaling %d cents for redemption of %d cents\n",
                      selectedDDUtxos.size(), selectedDDTotal, requiredDDBurn);
            LogPrintf("DigiDollar: selectedDDAmounts.size() = %zu\n", selectedDDAmounts.size());

            // Get oracle price - use real oracle, fall back to mock only in regtest
            CAmount oraclePrice = OracleIntegration::GetCurrentOraclePriceMicroUSD();
            if (oraclePrice <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                MockOracleManager::GetInstance().IsEnabled()) {
                oraclePrice = MockOracleManager::GetInstance().GetCurrentPrice();
            }
            if (oraclePrice <= 0) {
                throw JSONRPCError(RPC_MISC_ERROR, "No oracle price available for redemption");
            }

            // Build redemption transaction using RedeemTxBuilder
            DigiDollar::RedeemTxBuilder redeemBuilder(Params(), currentHeight, oraclePrice);

            // Get the owner key for this position
            CKey ownerKey;
            if (!dd_wallet->GetOwnerKey(positionId, ownerKey)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Owner key not found for position");
            }

            DigiDollar::TxBuilderRedeemParams redeemParams;
            redeemParams.collateralOutpoint = collateralOutpoint;
            redeemParams.ddUtxos = selectedDDUtxos;  // Use any DD from wallet (fungible)
            redeemParams.ddAmounts = selectedDDAmounts;  // CRITICAL: Pass amounts for DD change calculation
            redeemParams.ddToRedeem = requiredDDBurn;
            redeemParams.path = errRedemptionActive ? DigiDollar::RedemptionPath::ERR : DigiDollar::RedemptionPath::NORMAL;
            redeemParams.ownerKey = ownerKey;  // BUG #10 FIX: Use position owner key directly
            // DigiDollar transactions MUST pay at least 0.1 DGB fee to miners
            static const CAmount MIN_DD_FEE_RATE = 35000000; // 0.35 DGB/kB ensures min 0.1 DGB for typical tx
            redeemParams.feeRate = MIN_DD_FEE_RATE;

            // Use the caller's requested DGB return address if supplied. If no
            // address is supplied, create a wallet destination so the returned
            // collateral remains visible to this wallet.
            std::string actualUnlockAddress;
            {
                LOCK(pwallet->cs_wallet);
                std::string label = "";  // Empty label

                if (!redeemAddress.empty()) {
                    CTxDestination requestedDest = DecodeDestination(redeemAddress);
                    if (!IsValidDestination(requestedDest)) {
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid redemption address");
                    }
                    redeemParams.collateralDest = requestedDest;
                    actualUnlockAddress = redeemAddress;
                    LogPrintf("DigiDollar: Using requested destination for returned collateral\n");
                } else {
                    auto op_dest = pwallet->GetNewDestination(OutputType::BECH32M, label);
                    if (!op_dest) {
                        // Legacy wallet fallback: try BECH32 (SegWit v0)
                        LogPrintf("DigiDollar: BECH32M not available, trying BECH32 for legacy wallet\n");
                        op_dest = pwallet->GetNewDestination(OutputType::BECH32, label);
                    }
                    if (op_dest) {
                        redeemParams.collateralDest = *op_dest;
                        actualUnlockAddress = EncodeDestination(*op_dest);
                        LogPrintf("DigiDollar: Using wallet destination for returned collateral\n");
                    } else {
                        CTxDestination ownerFallback{WitnessV1Taproot(XOnlyPubKey(ownerKey.GetPubKey()))};
                        actualUnlockAddress = EncodeDestination(ownerFallback);
                        LogPrintf("DigiDollar: WARNING - Could not get wallet address, using owner key (wallet may not recognize)\n");
                        LogPrintf("DigiDollar: Error: %s\n", util::ErrorString(op_dest).original);
                    }
                }

                // CRITICAL FIX: Get a SEPARATE address for DGB fee change
                // This ensures collateral and change go to DIFFERENT addresses
                auto op_change = pwallet->GetNewDestination(OutputType::BECH32M, label);
                if (!op_change) {
                    // Legacy wallet fallback: try BECH32 (SegWit v0)
                    LogPrintf("DigiDollar: BECH32M not available for change, trying BECH32 for legacy wallet\n");
                    op_change = pwallet->GetNewDestination(OutputType::BECH32, label);
                }
                if (op_change) {
                    redeemParams.dgbChangeDest = *op_change;
                    LogPrintf("DigiDollar: Using separate wallet destination for DGB change\n");
                } else {
                    LogPrintf("DigiDollar: WARNING - Could not get wallet address for DGB change, will use collateralDest (may merge outputs)\n");
                    LogPrintf("DigiDollar: Error: %s\n", util::ErrorString(op_change).original);
                }
            }

            // Use verified mint metadata only. The wallet cache may have been
            // repaired above, but fallback collateral-only metadata is not
            // enough to safely choose nLockTime, burn amount, or signing leaf.
            redeemParams.collateralAmount = foundPosition.dgb_collateral;
            redeemParams.ddMinted = foundPosition.dd_minted;
            redeemParams.unlockHeight = static_cast<uint32_t>(foundPosition.unlock_height);

            LogPrintf("DigiDollar: Using verified position metadata:\n");
            LogPrintf("  - Collateral: %d sats (%.8f DGB)\n", foundPosition.dgb_collateral, foundPosition.dgb_collateral / 100000000.0);
            LogPrintf("  - DD Minted: %d cents\n", foundPosition.dd_minted);
            LogPrintf("  - Unlock Height: %d\n", foundPosition.unlock_height);

            // Select fee UTXOs from wallet
            // CRITICAL: Build exclude list to prevent selecting collateral or DD UTXOs as fee inputs
            std::vector<COutPoint> exclude_utxos;
            exclude_utxos.push_back(redeemParams.collateralOutpoint);  // Don't select collateral
            exclude_utxos.insert(exclude_utxos.end(), redeemParams.ddUtxos.begin(), redeemParams.ddUtxos.end());  // Don't select DD UTXOs

            LogPrintf("DigiDollar: Building exclude list with %d UTXOs (1 collateral + %d DD)\n",
                      exclude_utxos.size(), redeemParams.ddUtxos.size());

            // Bug #9 fix: Calculate fee from feeRate and estimated tx size instead of hardcoding.
            // Redemption tx: ~3 inputs (collateral + DD + fee), ~2-3 outputs → ~400 vbytes.
            // Apply 50% safety margin for script-path spending variance.
            CAmount estimatedFee = (400 * redeemParams.feeRate) / 1000; // vsize * feeRate / 1000
            estimatedFee = estimatedFee + (estimatedFee / 2); // 50% safety margin
            if (estimatedFee < 10000000) estimatedFee = 10000000; // Floor at 0.1 DGB
            LogPrintf("DigiDollar: Estimated redemption fee: %lld sats (%.8f DGB)\n",
                      static_cast<long long>(estimatedFee), estimatedFee / 100000000.0);
            CAmount selectedFeeTotal = 0;
            std::vector<CAmount> feeAmounts;

            if (!dd_wallet->SelectFeeCoins(estimatedFee, redeemParams.feeUtxos, selectedFeeTotal, &feeAmounts, &exclude_utxos)) {
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient DGB balance for transaction fees");
            }

            redeemParams.feeAmounts = feeAmounts;
            LogPrintf("DigiDollar: Selected %d sats in fees from %d UTXOs for redemption\n",
                     selectedFeeTotal, redeemParams.feeUtxos.size());

            DigiDollar::TxBuilderResult redeemResult = redeemBuilder.BuildRedemptionTransaction(redeemParams);

            if (!redeemResult.success) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to build redemption transaction: " + redeemResult.error);
            }

            LogPrintf("DigiDollar: Redemption transaction built with %d inputs:\n", redeemResult.tx.vin.size());
            for (size_t i = 0; i < redeemResult.tx.vin.size(); i++) {
                LogPrintf("DigiDollar:   Input %d: %s:%d\n", i,
                         redeemResult.tx.vin[i].prevout.hash.ToString(),
                         redeemResult.tx.vin[i].prevout.n);
            }

            // Sign redemption transaction using specialized function that handles:
            // - Collateral (input 0): script-path spending with MAST tree
            // - DD tokens (input 1+): key-path spending (no MAST)
            // - Fee inputs: standard wallet signing
            bool signSuccess = dd_wallet->SignRedemptionTransaction(
                redeemResult.tx,
                redeemParams.collateralOutpoint,
                redeemParams.ddUtxos,
                redeemParams.feeUtxos,
                ownerKey);

            if (!signSuccess) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign redemption transaction with Schnorr signatures");
            }

            // Create transaction reference
            CTransactionRef redeemTx = MakeTransactionRef(redeemResult.tx);

            const bool should_broadcast = pwallet->GetBroadcastTransactions();
            if (should_broadcast) {
                RefreshRegtestMockMuSig2QuoteForMempool(*pwallet);
            }

            // Commit through the wallet-owned relay path exactly once so the
            // wallet state transition and mempool submission stay in sync.
            std::string commit_error;
            bool commit_success = false;
            {
                LOCK(pwallet->cs_wallet);
                commit_success = pwallet->CommitTransaction(redeemTx, {}, {}, &commit_error);
            }
            if (should_broadcast && !commit_success) {
                const uint256 redeem_txid = redeemTx->GetHash();
                if (pwallet->TransactionCanBeAbandoned(redeem_txid)) {
                    pwallet->AbandonTransaction(redeem_txid);
                    LogPrintf("DigiDollar RPC Redeem: Abandoned rejected local redemption transaction %s\n",
                              redeem_txid.ToString());
                }
                throw JSONRPCError(RPC_TRANSACTION_REJECTED,
                    strprintf("Redemption transaction rejected by mempool: %s", commit_error));
            }

            // Do not mutate persistent DD UTXO accounting while the redeem is
            // only in mempool. Selected DD inputs stay tracked and are hidden
            // from balances through wallet IsSpent(); confirmed removal and DD
            // change creation are applied by ProcessTransactionForDD when the
            // redeem is mined. This keeps restart/abandon/retry paths safe.
            if (redeemResult.ddChange > 0) {
                dd_wallet->StoreOwnerKey(redeemTx->GetHash(), ownerKey);
                LogPrintf("DigiDollar: Deferred DD change tracking for pending redemption %s (%d cents)\n",
                          redeemTx->GetHash().ToString(), redeemResult.ddChange);
            }
            for (const auto& spentUtxo : selectedDDUtxos) {
                LogPrintf("DigiDollar: DD UTXO %s:%d pending redemption spend (will be erased on block confirm)\n",
                          spentUtxo.hash.ToString(), spentUtxo.n);
            }

            // Full-vault redemption only: normal and ERR both return full collateral.
            CAmount dgbUnlocked = foundPosition.dgb_collateral;
            bool positionClosed = true;

            if (positionClosed) {
                // Mark position as inactive
                foundPosition.is_active = false;
                dd_wallet->WriteDDTimeLock(foundPosition);

                // Keep collateral and DD token outpoints locked while the redeem
                // is unconfirmed. They are spent if the redeem confirms, and they
                // must remain protected if the redeem leaves mempool or is reorged.
                LogPrintf("DigiDollar: Position %s pending redemption; collateral+DD-token locks remain until chain state resolves\n",
                          positionIdStr);
            } else {
                // Update position with remaining amounts
                CAmount remainingDD = foundPosition.dd_minted - ddAmount;
                CAmount remainingCollateral = foundPosition.dgb_collateral - dgbUnlocked;
                foundPosition.dd_minted = remainingDD;
                foundPosition.dgb_collateral = remainingCollateral;
                dd_wallet->WriteDDTimeLock(foundPosition);
            }

            // Add redemption transaction to history for GUI display
            DDTransaction redeemTxHistory;
            redeemTxHistory.txid = redeemTx->GetHash().GetHex();
            redeemTxHistory.amount = requiredDDBurn;  // DD amount redeemed (burned)
            redeemTxHistory.confirmations = 0;   // Pending confirmation
            redeemTxHistory.timestamp = GetTime();
            redeemTxHistory.incoming = false;    // Redemption = outgoing DD (burning)
            redeemTxHistory.address = actualUnlockAddress.empty() ? "self" : actualUnlockAddress;
            redeemTxHistory.category = "redeem";
            redeemTxHistory.fee = redeemResult.totalFees;  // Bug #17 fix: record actual fee, not 0

            // Add to history using proper method
            if (!dd_wallet->AddRedemptionToHistory(redeemTxHistory)) {
                LogPrintf("DigiDollar: WARNING - Failed to add redemption to history\n");
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("txid", redeemTx->GetHash().GetHex());
            result.pushKV("position_id", positionIdStr);
            result.pushKV("dd_redeemed", int64_t{requiredDDBurn});
            result.pushKV("total_dd_minted", int64_t{foundPosition.dd_minted});
            result.pushKV("required_dd_burn", int64_t{requiredDDBurn});
            result.pushKV("dgb_unlocked", ValueFromAmount(dgbUnlocked));
            result.pushKV("unlock_address", actualUnlockAddress.empty() ? "auto" : actualUnlockAddress);
            result.pushKV("fee_paid", ValueFromAmount(redeemResult.totalFees));
            result.pushKV("redemption_path", errRedemptionActive ? "emergency" : "normal");
            result.pushKV("err_active", errRedemptionActive);
            result.pushKV("err_system_health", redemptionSystemHealth);
            result.pushKV("err_ratio_bps", errRatioBps);
            result.pushKV("position_closed", positionClosed);

            return result;
        },
    };
}

RPCHelpMan listdigidollarpositions()
{
    return RPCHelpMan{"listdigidollarpositions",
                "\nList all DigiDollar collateral positions in the wallet.\n"
                "Shows active and inactive positions with their current status.\n",
                {
                    {"active_only", RPCArg::Type::BOOL, RPCArg::Default{true}, "Only show active positions"},
                    {"tier_filter", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Filter by specific lock tier (0-9)"},
                    {"min_amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "Minimum DD amount filter"},
                    // DD-FA-FUNC-034 (Wave 21 Agent C): per-call paging
                    // window so the RPC body stays bounded for wallets
                    // with thousands of positions. Default 0 keeps the
                    // historical "return all matching positions" body.
                    {"count", RPCArg::Type::NUM, RPCArg::Default{0}, "Maximum positions to return (0 = no limit, max 1000)"},
                    {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "Number of matching positions to skip before returning results"}
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "position_id", "Unique position identifier"},
                                {RPCResult::Type::STR_AMOUNT, "dd_minted", "DigiDollar amount minted"},
                                {RPCResult::Type::STR_AMOUNT, "dgb_collateral", "DGB locked as collateral"},
                                {RPCResult::Type::NUM, "lock_tier", "Lock tier (0-9, 0=1h testing)"},
                                {RPCResult::Type::NUM, "lock_days", "Lock period in days"},
                                {RPCResult::Type::NUM, "unlock_height", "Block height when unlockable"},
                                {RPCResult::Type::NUM, "blocks_remaining", "Blocks until unlock (0 if unlocked)"},
                                {RPCResult::Type::NUM, "confirmations", "Number of confirmations for the mint transaction"},
                                {RPCResult::Type::STR, "status", "Position status (pending/active/unlocked/redeemed)"},
                                {RPCResult::Type::NUM, "health_ratio", "Current collateral health ratio (%)"},
                                {RPCResult::Type::BOOL, "can_redeem", "Whether position can be redeemed now"},
                                {RPCResult::Type::BOOL, "spendable", "Whether this wallet can spend the position"},
                                {RPCResult::Type::BOOL, "iswatchonly", "Whether the position is watch-only"},
                                {RPCResult::Type::STR, "created_date", "ISO date when position was created"},
                                {RPCResult::Type::STR, "unlock_date", "ISO date when position unlocks"}
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("listdigidollarpositions", "") +
                    HelpExampleCli("listdigidollarpositions", "false") +
                    HelpExampleCli("listdigidollarpositions", "true 3") +
                    HelpExampleCli("listdigidollarpositions", "false null 0 50 0") +
                    HelpExampleRpc("listdigidollarpositions", "") +
                    HelpExampleRpc("listdigidollarpositions", "false, 3") +
                    HelpExampleRpc("listdigidollarpositions", "false, null, 0, 50, 0")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                std::shared_ptr<wallet::CWallet> pwallet_check = wallet::GetWalletForJSONRPCRequest(request);
                if (pwallet_check) {
                    node::NodeContext* node_ctx = pwallet_check->chain().context();
                    if (node_ctx) {
                        ChainstateManager& chainman = *node_ctx->chainman;
                        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                        if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                            throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                        }
                    }
                }
            }
            // Parse parameters
            bool activeOnly = OptionalParamIsSet(request, 0) ? request.params[0].get_bool() : true;
            int tierFilter = OptionalParamIsSet(request, 1) ?
                            request.params[1].getInt<int>() : -1;
            CAmount minAmount = OptionalParamIsSet(request, 2) ?
                               ParseDigiDollarRpcAmount(request.params[2]) : 0;
            // DD-FA-FUNC-034 (Wave 21 Agent C): bound the response body so a
            // wallet with thousands of positions cannot trivially DoS its own
            // RPC clients. The default count=0 preserves the historical
            // "return all matching positions" behaviour for compatibility
            // with existing scripts; positive counts clamp to the same 1000
            // ceiling that listdigidollartxs already enforces.
            int countLimit = OptionalParamIsSet(request, 3) ? request.params[3].getInt<int>() : 0;
            int skipCount = OptionalParamIsSet(request, 4) ? request.params[4].getInt<int>() : 0;
            if (tierFilter < -1 || tierFilter > 9) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    "Tier filter must be between 0 and 9, or -1 for no filter");
            }
            if (countLimit < 0 || countLimit > 1000) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Count must be between 0 and 1000");
            }
            if (skipCount < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Skip must be non-negative");
            }

            // Get wallet
            std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet not found");

            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            pwallet->BlockUntilSyncedToCurrentChain();
            // A reindex restart can load the wallet and abandon stale DD redeem
            // transactions before chainstate is ready, causing the startup DD
            // scan to skip active-flag reconciliation. Reconcile on read, after
            // sync, so listdigidollarpositions reflects the active chain rather
            // than a stale pre-reindex pending-spend reservation.
            dd_wallet->ReconcilePositionStates();

            // Get all positions
            LOCK(pwallet->cs_wallet);
            std::vector<WalletCollateralPosition> positions = dd_wallet->GetDDTimeLocks(false);
            int currentHeight = pwallet->GetLastBlockHeight();
            const bool walletPrivateKeysDisabled = pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);
            // DD-FA-FUNC-022 (Wave 17 Agent C): a locked encrypted wallet
            // physically cannot construct a redemption witness even though
            // the WALLET_FLAG_DISABLE_PRIVATE_KEYS flag is unset. Treat the
            // locked state the same as private-keys-disabled when computing
            // the spendable / can_redeem flags so RPC clients are not
            // deceived into telling the user they can redeem now.
            const bool walletLocked = pwallet->IsLocked();
            const bool walletCannotSign = walletPrivateKeysDisabled || walletLocked;

            UniValue result(UniValue::VARR);

            // DD-FA-FUNC-034: paging counters apply *after* the filter chain
            // so skip/count refer to the matching subset the caller already
            // narrowed to (mirrors the listdigidollartxs paging contract).
            int skipped = 0;
            int processed = 0;
            auto has_pending_redeem = [&](const WalletCollateralPosition& pos) {
                return HasPendingDigiDollarRedeem(*pwallet, pos.dd_timelock_id);
            };
            for (const auto& pos : positions) {
                // Apply filters
                if (activeOnly && !pos.is_active) continue;
                if (tierFilter >= 0 && pos.lock_tier != static_cast<uint32_t>(tierFilter)) continue;
                if (minAmount > 0 && pos.dd_minted < minAmount) continue;

                if (skipped < skipCount) {
                    ++skipped;
                    continue;
                }
                if (countLimit > 0 && processed >= countLimit) break;

                UniValue position(UniValue::VOBJ);
                position.pushKV("position_id", pos.dd_timelock_id.GetHex());
                position.pushKV("dd_minted", int64_t{pos.dd_minted});
                position.pushKV("dgb_collateral", ValueFromAmount(pos.dgb_collateral));
                position.pushKV("lock_tier", static_cast<int>(pos.lock_tier));
                position.pushKV("lock_days", GetLockDaysForTier(pos.lock_tier));
                position.pushKV("unlock_height", pos.unlock_height);

                // Calculate remaining blocks
                int blocksRemaining = std::max(0, static_cast<int>(pos.unlock_height - currentHeight));
                position.pushKV("blocks_remaining", blocksRemaining);
                const int confirmations = dd_wallet->GetDDTransactionConfirmations(pos.dd_timelock_id);
                position.pushKV("confirmations", confirmations);

                // Status
                std::string status;
                if (!pos.is_active) {
                    status = has_pending_redeem(pos) ? "pending_redeem" : "redeemed";
                } else if (confirmations <= 0) {
                    status = "pending";
                } else {
                    status = blocksRemaining == 0 ? "unlocked" : "active";
                }
                position.pushKV("status", status);

                // Health ratio: (dgb_collateral_value_in_usd / dd_minted_value_in_usd) * 100
                // dd_minted is in cents (100 = $1), so dd_minted_micro_usd = dd_minted * 10000
                // dgb_collateral is in satoshis, oracle price is micro-USD per 1 DGB (COIN satoshis)
                // collateral_value_micro_usd = (dgb_collateral * oraclePriceMicroUSD) / COIN
                // health = (collateral_value / dd_value) * 100
                int healthRatio = 0;
                if (pos.dgb_collateral > 0 && pos.dd_minted > 0) {
                    CAmount oraclePriceMicroUSD = OracleIntegration::GetCurrentOraclePriceMicroUSD();
                    if (oraclePriceMicroUSD <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                        MockOracleManager::GetInstance().IsEnabled()) {
                        oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
                    }
                    if (oraclePriceMicroUSD > 0) {
                        // Use __int128 to prevent overflow: collateral can be large
                        __int128 collateralMicroUSD = (static_cast<__int128>(pos.dgb_collateral) * oraclePriceMicroUSD) / COIN;
                        __int128 ddMicroUSD = static_cast<__int128>(pos.dd_minted) * 10000; // cents to micro-USD
                        healthRatio = static_cast<int>((collateralMicroUSD * 100) / ddMicroUSD);
                    }
                }
                position.pushKV("health_ratio", healthRatio);

                // can_redeem requires: confirmed, unlocked, active, AND has collateral
                // Received DD (dgb_collateral=0) cannot be redeemed - only spent/transferred.
                // Wave 17 / DD-FA-FUNC-022: also gate on walletCannotSign so a
                // locked encrypted wallet does not falsely advertise can_redeem.
                bool canRedeem = confirmations > 0 && blocksRemaining == 0 && pos.is_active && pos.dgb_collateral > 0 && !walletCannotSign;
                position.pushKV("can_redeem", canRedeem);
                position.pushKV("spendable", !walletCannotSign);
                position.pushKV("iswatchonly", walletPrivateKeysDisabled);

                // Dates: estimate from block heights using 15-second block time
                int64_t now = GetTime();
                int lockDays = GetLockDaysForTier(pos.lock_tier);
                int64_t lockBlocks = DigiDollar::LockDaysToBlocks(lockDays);
                int64_t createdHeight = pos.unlock_height - lockBlocks;
                // created_date: current_time - (currentHeight - createdHeight) * 15
                int64_t createdTimestamp = now - (static_cast<int64_t>(currentHeight) - createdHeight) * 15;
                position.pushKV("created_date", FormatISO8601DateTime(createdTimestamp));
                // unlock_date: if already unlocked, show the past unlock time; otherwise future
                if (blocksRemaining == 0) {
                    int64_t unlockTimestamp = now - (static_cast<int64_t>(currentHeight) - pos.unlock_height) * 15;
                    position.pushKV("unlock_date", FormatISO8601DateTime(unlockTimestamp));
                } else {
                    int64_t unlockTimestamp = now + static_cast<int64_t>(blocksRemaining) * 15;
                    position.pushKV("unlock_date", FormatISO8601DateTime(unlockTimestamp));
                }

                result.push_back(position);
                ++processed;
            }

            return result;
        },
    };
}

// =============================================================================
// DD ADDRESS COMMANDS (Task 5.7)
// =============================================================================
// NOTE: getdigidollaraddress is OBSOLETE - actual implementation is now in
// src/wallet/rpcwallet.cpp as a static function for proper wallet context.
// This version is kept for reference only and is not registered.
// =============================================================================

RPCHelpMan getdigidollaraddress()
{
    return RPCHelpMan{"getdigidollaraddress",
                "\nGenerate a new DigiDollar address for receiving DD.\n"
                "Creates a new address with the proper DD prefix for the current network.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Default{""}, "Optional label for the address"}
                },
                RPCResult{
                    RPCResult::Type::STR, "address", "The new DigiDollar address"
                },
                RPCExamples{
                    HelpExampleCli("getdigidollaraddress", "") +
                    HelpExampleCli("getdigidollaraddress", "\"savings\"") +
                    HelpExampleRpc("getdigidollaraddress", "") +
                    HelpExampleRpc("getdigidollaraddress", "\"savings\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                std::shared_ptr<wallet::CWallet> pwallet_check = wallet::GetWalletForJSONRPCRequest(request);
                if (pwallet_check) {
                    node::NodeContext* node_ctx = pwallet_check->chain().context();
                    if (node_ctx) {
                        ChainstateManager& chainman = *node_ctx->chainman;
                        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                        if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                            throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                        }
                    }
                }
            }
            std::shared_ptr<wallet::CWallet> const pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            // DD-FA-FUNC-028 (Wave 18 Agent C): DD-flavored locked-wallet
            // hint, preserving the legacy "walletpassphrase" substring for
            // backward compatibility with digidollar_encrypted_wallet.py.
            if (pwallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                    "DigiDollar address generation requires the wallet to be unlocked. "
                    "Error: Please enter the wallet passphrase with walletpassphrase first.");
            }
            if (pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
            }
            if (!pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DESCRIPTORS)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar address generation requires a descriptor/bech32m HD wallet with private keys enabled");
            }

            LOCK(pwallet->cs_wallet);

            if (!pwallet->CanGetAddresses()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
            }

            // Parse parameters
            std::string label = OptionalParamIsSet(request, 0) ? request.params[0].get_str() : "";

            // Generate an HD-derived key for DD addresses
            // This allows the key to be recovered from wallet seed
            LogPrintf("DigiDollar: getdigidollaraddress - generating HD key for DD address\n");

            CKey dd_key = pwallet->GetHDKeyForDigiDollar(label);
            if (!dd_key.IsValid()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar address generation requires a descriptor/bech32m HD wallet with private keys enabled");
            }

            // Create the P2TR output key from this key (key-path only, no script tree)
            CPubKey dd_pubkey = dd_key.GetPubKey();
            XOnlyPubKey internal_key(dd_pubkey);

            // Compute the taptweak to get the output key
            auto tweaked = internal_key.CreateTapTweak(nullptr); // No merkle root for key-path only
            if (!tweaked) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to create taproot tweaked key");
            }
            XOnlyPubKey output_key = tweaked->first;
            bool output_parity = tweaked->second;

            LogPrintf("DigiDollar: Generated internal_key=%s, output_key=%s, parity=%d\n",
                     HexStr(Span<const unsigned char>(internal_key.begin(), internal_key.end())),
                     HexStr(Span<const unsigned char>(output_key.begin(), output_key.end())),
                     output_parity);

            // Note: Parity adjustment for Schnorr signing is handled automatically by
            // the secp256k1 library in SignSchnorr - we store the internal key as-is

            // Store the key in DigiDollarWallet for later spending
            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (dd_wallet) {
                // Store by output_key (what we'll see in the UTXO) with the internal key
                // The signing code will handle the taproot tweak adjustment
                dd_wallet->StoreAddressKey(output_key, dd_key);
                LogPrintf("DigiDollar: Stored DD address key (output_key=%s)\n",
                         HexStr(Span<const unsigned char>(output_key.begin(), output_key.end())));
            } else {
                LogPrintf("DigiDollar: ERROR - GetDDWallet returned nullptr\n");
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not available");
            }

            // Create the destination from the output_key
            CTxDestination dest = WitnessV1Taproot(output_key);

            // Import the address WITH private key so wallet can sign spending transactions
            // This allows wallet to automatically sign DD transfers like normal DGB transactions
            {
                // CRITICAL FIX: Use tr(INTERNAL_KEY) instead of rawtr(OUTPUT_KEY)
                // rawtr() is watch-only and cannot provide signing information
                // tr() with the internal key creates a proper signable descriptor
                std::string internal_key_hex = HexStr(Span<const unsigned char>(internal_key.begin(), internal_key.end()));
                std::string descriptor_str = "tr(" + internal_key_hex + ")";

                // Parse the descriptor - this creates a TRDescriptor that will populate tr_trees
                FlatSigningProvider provider;
                std::string error;
                auto parsed_desc = Parse(descriptor_str, provider, error, /*require_checksum=*/false);

                if (parsed_desc) {
                    // CRITICAL: Add the private key to the provider so wallet can sign
                    // The key must be indexed by CKeyID (Hash160 of compressed pubkey)
                    provider.keys[dd_pubkey.GetID()] = dd_key;
                    provider.pubkeys[dd_pubkey.GetID()] = dd_pubkey;

                    LogPrintf("DigiDollar: Added private key to provider, keyid=%s\n",
                             dd_pubkey.GetID().ToString());

                    // Create import request
                    wallet::WalletDescriptor wallet_desc(std::move(parsed_desc), /*timestamp=*/0, /*range_start=*/0, /*range_end=*/0, /*next_index=*/0);

                    // Import as active (non-internal) for receiving
                    LOCK(pwallet->cs_wallet);
                    if (pwallet->AddWalletDescriptor(wallet_desc, provider, "", /*internal=*/false)) {
                        LogPrintf("DigiDollar: Imported DD address as tr() descriptor WITH private key\n");
                    } else {
                        LogPrintf("DigiDollar: WARNING - Failed to import DD address descriptor (may already exist)\n");
                    }
                } else {
                    LogPrintf("DigiDollar: WARNING - Failed to parse DD address descriptor: %s\n", error);
                }
            }

            // AddWalletDescriptor records non-internal descriptors as regular
            // receive entries. Reclassify this destination so DD receive labels
            // do not pollute the normal DGB address book.
            pwallet->SetAddressBook(dest, label, wallet::AddressPurpose::DIGIDOLLAR);

            // Encode as DigiDollar address
            std::string newAddress = EncodeDigiDollarAddress(dest);

            if (newAddress.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to encode DigiDollar address");
            }

            return newAddress;
        },
    };
}

RPCHelpMan validateddaddress()
{
    return RPCHelpMan{"validateddaddress",
                "\nValidate a DigiDollar address format and return detailed information.\n"
                "Checks if the address has the correct prefix, encoding, and checksum.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "DigiDollar address to validate"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "isvalid", "Whether the address is valid"},
                        {RPCResult::Type::STR, "address", "The validated address (if valid)"},
                        {RPCResult::Type::STR, "network", "Network type (mainnet/testnet/regtest)"},
                        {RPCResult::Type::STR, "prefix", "Address prefix (DD/TD/RD)"},
                        {RPCResult::Type::BOOL, "ismine", "Whether address belongs to this wallet"},
                        {RPCResult::Type::BOOL, "iswatchonly", "Whether address is watch-only"},
                        {RPCResult::Type::BOOL, "solvable", "If we know how to spend coins sent to this address, ignoring the possible lack of private keys (matches validateaddress semantics)"},
                        {RPCResult::Type::STR, "error", "Error description (if invalid)"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("validateddaddress", "\"DDtestaddress123456789abcdef\"") +
                    HelpExampleCli("validateddaddress", "\"TDtestnet123456789abcdef\"") +
                    HelpExampleRpc("validateddaddress", "\"DDtestaddress123456789abcdef\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation via wallet chain context
            std::shared_ptr<wallet::CWallet> pwallet_check = wallet::GetWalletForJSONRPCRequest(request);
            if (pwallet_check) {
                node::NodeContext* node_ctx = pwallet_check->chain().context();
                if (node_ctx) {
                    ChainstateManager& chainman = *node_ctx->chainman;
                    const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                    if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                        throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active");
                    }
                }
            }
            std::string addressStr = request.params[0].get_str();

            UniValue result(UniValue::VOBJ);

            bool isValid = true;
            std::string error;
            std::string network;
            std::string prefix;

            CDigiDollarAddress ddAddress(addressStr);
            isValid = ddAddress.IsValid();
            if (isValid) {
                prefix = addressStr.substr(0, 2);
                if (prefix == "DD") network = "mainnet";
                else if (prefix == "TD") network = "testnet";
                else if (prefix == "RD") network = "regtest";
                else network = "unknown";
                const std::string expected = ExpectedDigiDollarAddressPrefix();
                if (prefix != expected) {
                    isValid = false;
                    error = strprintf("DigiDollar address is for %s network (%s prefix), but this node expects %s prefix",
                                      network, prefix, expected);
                }
            } else {
                error = "Invalid DigiDollar address";
            }

            result.pushKV("isvalid", isValid);
            // DD-FA-FUNC-019 (Wave 15): echo the canonical re-encoded form
            // (ddAddress.ToString()) instead of the raw user input. Now that
            // CDigiDollarAddress rejects whitespace-padded strings up front
            // this is a defense-in-depth: any future decoder that silently
            // canonicalises (e.g. case folding) cannot leak the pre-canonical
            // form through the validateddaddress reply.
            result.pushKV("address", isValid ? ddAddress.ToString() : std::string{});
            result.pushKV("network", network);
            result.pushKV("prefix", prefix);
            bool isMine = false;
            bool isWatchOnly = false;
            // DD-FA-FUNC-023 (Wave 17 Agent C): expose `solvable` with the
            // same semantics as the upstream `validateaddress` RPC. A DD
            // address is solvable when the wallet holds the descriptor /
            // address-key required to construct a spending witness, even if
            // the wallet is currently locked. Foreign DD addresses and any
            // invalid input are solvable=false.
            bool solvable = false;
            if (isValid) {
                try {
                    auto pw = wallet::GetWalletForJSONRPCRequest(request);
                    if (pw) {
                        const bool privateKeysDisabled = pw->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);
                        DigiDollarWallet* ddw = pw->GetDDWallet();
                        if (ddw && ddw->IsMyDDAddress(addressStr)) {
                            isWatchOnly = privateKeysDisabled;
                            isMine = !isWatchOnly;
                            // Wallet-known DD address always implies the
                            // descriptor / DD address-key has been recorded,
                            // so we know how to construct a witness.
                            solvable = true;
                        }
                    }
                } catch (...) {}
            }
            result.pushKV("ismine", isMine);
            result.pushKV("iswatchonly", isWatchOnly);
            result.pushKV("solvable", solvable);
            result.pushKV("error", error);

            return result;
        },
    };
}

RPCHelpMan listdigidollaraddresses()
{
    return RPCHelpMan{"listdigidollaraddresses",
                "\nList all DigiDollar addresses in the wallet.\n"
                "Returns both owned and watch-only DD addresses with their balances and labels.\n",
                {
                    {"include_watchonly", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include watch-only addresses"},
                    {"min_balance", RPCArg::Type::AMOUNT, RPCArg::Default{0}, "Minimum balance filter (in cents)"},
                    {"include_empty", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include wallet-generated DD addresses with zero balance (DD-FA-FUNC-024 default-false to avoid leaking the size of the keypool)"}
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "address", "DigiDollar address"},
                                {RPCResult::Type::STR, "label", "Address label"},
                                {RPCResult::Type::STR_AMOUNT, "balance", "DD balance (in cents)"},
                                {RPCResult::Type::BOOL, "ismine", "Whether address is owned by wallet"},
                                {RPCResult::Type::BOOL, "iswatchonly", "Whether address is watch-only"},
                                {RPCResult::Type::NUM, "txcount", "Number of transactions involving this address"},
                                {RPCResult::Type::STR, "created_date", "Date when address was created"},
                                {RPCResult::Type::STR, "last_used", "Date of last transaction"}
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("listdigidollaraddresses", "") +
                    HelpExampleCli("listdigidollaraddresses", "true") +
                    HelpExampleCli("listdigidollaraddresses", "true 1000") +
                    HelpExampleRpc("listdigidollaraddresses", "") +
                    HelpExampleRpc("listdigidollaraddresses", "true, 1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Get wallet first (needed for activation check via wallet chain interface)
            std::shared_ptr<wallet::CWallet> const pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_NOT_FOUND, "No wallet is loaded");
            }

            // Check DigiDollar activation via wallet's chain context
            {
                node::NodeContext* node_ctx = pwallet->chain().context();
                if (node_ctx) {
                    ChainstateManager& chainman = *node_ctx->chainman;
                    const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                    if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                        throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                    }
                }
            }

            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not available");
            }

            // Parse parameters
            bool includeWatchOnly = OptionalParamIsSet(request, 0) ? request.params[0].get_bool() : false;
            CAmount minBalance = OptionalParamIsSet(request, 1) ? ParseDigiDollarRpcAmount(request.params[1]) : 0;
            // DD-FA-FUNC-024 (Wave 17 Agent C): omit empty addresses by
            // default to avoid leaking the keypool size to RPC observers.
            // include_empty=true preserves the prior default behaviour for
            // explicit operator queries.
            bool includeEmpty = OptionalParamIsSet(request, 2) ? request.params[2].get_bool() : false;
            const bool privateKeysDisabled = pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);

            UniValue result(UniValue::VARR);

            LOCK2(pwallet->cs_wallet, dd_wallet->cs_dd_wallet);

            // Build address->balance map from DD UTXOs.
            std::map<std::string, CAmount> addressBalances;
            std::vector<DDUtxo> utxos = dd_wallet->GetDDUTXOs();
            for (const auto& utxo : utxos) {
                // Look up the prevout to get the scriptPubKey
                const wallet::CWalletTx* wtx = pwallet->GetWalletTx(utxo.outpoint.hash);
                if (!wtx || utxo.outpoint.n >= wtx->tx->vout.size()) continue;

                const CTxOut& txout = wtx->tx->vout[utxo.outpoint.n];
                CTxDestination dest;
                if (!ExtractDestination(txout.scriptPubKey, dest)) continue;

                // Encode as network-aware DD address (DD/TD/RD prefix)
                std::string ddAddr = EncodeDigiDollarAddress(dest);
                if (ddAddr.empty()) continue;

                addressBalances[ddAddr] += utxo.dd_amount;
            }

            for (const std::string& addr : dd_wallet->GetKnownDDAddresses()) {
                addressBalances.try_emplace(addr, 0);
            }

            for (const auto& [addr, balance] : addressBalances) {
                if (balance < minBalance) continue;
                // DD-FA-FUNC-024: hide zero-balance addresses by default.
                if (!includeEmpty && balance == 0) continue;

                bool isWatchOnly = privateKeysDisabled;
                bool isMine = !isWatchOnly;

                if (!includeWatchOnly && isWatchOnly) continue;

                UniValue addrInfo(UniValue::VOBJ);
                addrInfo.pushKV("address", addr);
                addrInfo.pushKV("label", "");
                addrInfo.pushKV("balance", balance);
                addrInfo.pushKV("ismine", isMine);
                addrInfo.pushKV("iswatchonly", isWatchOnly);
                addrInfo.pushKV("txcount", 0);
                addrInfo.pushKV("created_date", "");
                addrInfo.pushKV("last_used", "");

                result.push_back(addrInfo);
            }

            return result;
        },
    };
}
#endif

static RPCHelpMan importdigidollaraddress()
{
    return RPCHelpMan{"importdigidollaraddress",
                "\nValidate a DigiDollar address for a future watch-only import flow.\n"
                "Watch-only DigiDollar address import is explicitly unsupported in V1; this RPC does not change wallet state.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "DigiDollar address to validate"},
                    {"label", RPCArg::Type::STR, RPCArg::Default{""}, "Optional label echoed in the response; not stored"},
                    {"rescan", RPCArg::Type::BOOL, RPCArg::Default{false}, "Ignored; rescan is not performed"},
                    {"p2sh", RPCArg::Type::BOOL, RPCArg::Default{false}, "Ignored; P2SH import is not supported"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "Validated DigiDollar address"},
                        {RPCResult::Type::STR, "label", "Echoed label"},
                        {RPCResult::Type::BOOL, "success", "Always false because V1 import is unsupported"},
                        {RPCResult::Type::BOOL, "rescan_performed", "Always false because no rescan is performed"},
                        {RPCResult::Type::NUM, "transactions_found", "Always 0 because no import/rescan is performed"},
                        {RPCResult::Type::STR, "warning", "Unsupported-import warning"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("importdigidollaraddress", "\"DDexternaladdress123456789abc\"") +
                    HelpExampleCli("importdigidollaraddress", "\"DDexternaladdress123456789abc\" \"external_wallet\"") +
                    HelpExampleCli("importdigidollaraddress", "\"DDexternaladdress123456789abc\" \"external_wallet\" true") +
                    HelpExampleRpc("importdigidollaraddress", "\"DDexternaladdress123456789abc\", \"external_wallet\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            // Parse parameters
            std::string addressStr = request.params[0].get_str();
            std::string label = OptionalParamIsSet(request, 1) ? request.params[1].get_str() : "";
            bool rescan = OptionalParamIsSet(request, 2) ? request.params[2].get_bool() : false;
            bool p2sh = OptionalParamIsSet(request, 3) ? request.params[3].get_bool() : false;

            std::string address_error;
            if (!ValidateDigiDollarAddressForCurrentNetwork(addressStr, address_error)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, address_error);
            }

            bool success = false;
            int transactionsFound = 0;
            std::string warning = "DigiDollar watch-only import is explicitly unsupported in this build; no wallet state was changed";

            if (rescan) {
                warning += "; rescan was not performed";
            }

            if (p2sh) {
                warning += "; P2SH import is not supported";
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("address", addressStr);
            result.pushKV("label", label);
            result.pushKV("success", success);
            result.pushKV("rescan_performed", false);
            result.pushKV("transactions_found", transactionsFound);
            result.pushKV("warning", warning);

            return result;
        },
    };
}

// =============================================================================
// UTILITY RPC COMMANDS (Task 5.8)
// =============================================================================

#ifdef ENABLE_WALLET
RPCHelpMan getdigidollarbalance()
{
    return RPCHelpMan{"getdigidollarbalance",
                "\nGet DigiDollar balance for a specific address or total wallet balance.\n"
                "Returns the confirmed and unconfirmed DD balance.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "DigiDollar address (omit for total wallet balance)"},
                    {"minconf", RPCArg::Type::NUM, RPCArg::Default{1}, "Minimum number of confirmations"},
                    {"include_watchonly", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include watch-only addresses"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_AMOUNT, "confirmed", "Confirmed DD balance (in cents)"},
                        {RPCResult::Type::STR_AMOUNT, "unconfirmed", "Unconfirmed DD balance (in cents)"},
                        {RPCResult::Type::STR_AMOUNT, "total", "Total DD balance (confirmed + unconfirmed)"},
                        {RPCResult::Type::STR, "address", /*optional=*/true, "Address queried (if specific address)"},
                        {RPCResult::Type::NUM, "address_count", /*optional=*/true, "Number of addresses included (for wallet total)"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getdigidollarbalance", "") +
                    HelpExampleCli("getdigidollarbalance", "\"DDtestaddress123456789abcdef\"") +
                    HelpExampleCli("getdigidollarbalance", "\"\" 6 true") +
                    HelpExampleRpc("getdigidollarbalance", "") +
                    HelpExampleRpc("getdigidollarbalance", "\"DDtestaddress123456789abcdef\", 6")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                std::shared_ptr<wallet::CWallet> pwallet_check = wallet::GetWalletForJSONRPCRequest(request);
                if (pwallet_check) {
                    node::NodeContext* node_ctx = pwallet_check->chain().context();
                    if (node_ctx) {
                        ChainstateManager& chainman = *node_ctx->chainman;
                        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                        if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                            throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                        }
                    }
                }
            }
            // PHASE 7.7: Integration with DigiDollarWallet backend

            // Get wallet
            std::shared_ptr<wallet::CWallet> const pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not found");
            }

            // Get DigiDollar wallet
            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            }

            // Parse parameters
            std::string addressStr = OptionalParamIsSet(request, 0) ?
                                   request.params[0].get_str() : "";
            int minConf = OptionalParamIsSet(request, 1) ? request.params[1].getInt<int>() : 1;
            bool includeWatchOnly = OptionalParamIsSet(request, 2) ? request.params[2].get_bool() : false;

            // Validate parameters
            if (minConf < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Minimum confirmations must be non-negative");
            }

            CAmount confirmedBalance = 0;
            CAmount unconfirmedBalance = 0;
            int addressCount = 0;
            const int confirmedMinConf = std::max(1, minConf);

            if (!addressStr.empty()) {
                // Get balance for specific address
                std::string address_error;
                if (!ValidateDigiDollarAddressForCurrentNetwork(addressStr, address_error)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, address_error);
                }

                if (includeWatchOnly || !pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                    std::vector<DDUtxo> utxos = dd_wallet->GetDDUTXOs(/*include_unconfirmed=*/minConf == 0);
                    for (const auto& utxo : utxos) {
                        CTxOut txout;
                        int depth = 0;
                        bool inMempool = false;
                        {
                            LOCK(pwallet->cs_wallet);
                            const wallet::CWalletTx* wtx = pwallet->GetWalletTx(utxo.outpoint.hash);
                            if (!wtx || utxo.outpoint.n >= wtx->tx->vout.size()) continue;
                            depth = pwallet->GetTxDepthInMainChain(*wtx);
                            inMempool = wtx->InMempool();
                            txout = wtx->tx->vout[utxo.outpoint.n];
                        }
                        CTxDestination dest;
                        if (!ExtractDestination(txout.scriptPubKey, dest)) continue;
                        if (EncodeDigiDollarAddress(dest) != addressStr) continue;

                        if (depth >= confirmedMinConf) {
                            confirmedBalance += utxo.dd_amount;
                        } else if (minConf == 0 && depth == 0 && inMempool) {
                            unconfirmedBalance += utxo.dd_amount;
                        }
                    }
                    addressCount = (confirmedBalance + unconfirmedBalance) > 0 ? 1 : 0;
                }
            } else {
                // Get total wallet balance
                if (includeWatchOnly || !pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                    std::map<std::string, CAmount> confirmedAddressBalances;
                    std::vector<DDUtxo> utxos = dd_wallet->GetDDUTXOs(/*include_unconfirmed=*/minConf == 0);
                    for (const auto& utxo : utxos) {
                        CTxOut txout;
                        int depth = 0;
                        bool inMempool = false;
                        {
                            LOCK(pwallet->cs_wallet);
                            const wallet::CWalletTx* wtx = pwallet->GetWalletTx(utxo.outpoint.hash);
                            if (!wtx || utxo.outpoint.n >= wtx->tx->vout.size()) continue;
                            depth = pwallet->GetTxDepthInMainChain(*wtx);
                            inMempool = wtx->InMempool();
                            txout = wtx->tx->vout[utxo.outpoint.n];
                        }
                        CTxDestination dest;
                        if (!ExtractDestination(txout.scriptPubKey, dest)) continue;
                        const std::string ddAddr = EncodeDigiDollarAddress(dest);
                        if (ddAddr.empty()) continue;

                        if (depth >= confirmedMinConf) {
                            confirmedBalance += utxo.dd_amount;
                            confirmedAddressBalances[ddAddr] += utxo.dd_amount;
                        } else if (minConf == 0 && depth == 0 && inMempool) {
                            unconfirmedBalance += utxo.dd_amount;
                            confirmedAddressBalances.try_emplace(ddAddr, 0);
                        }
                    }
                    addressCount = confirmedAddressBalances.size();
                }
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("confirmed", int64_t{confirmedBalance});
            result.pushKV("unconfirmed", int64_t{unconfirmedBalance});
            result.pushKV("total", int64_t{confirmedBalance + unconfirmedBalance});
            if (!addressStr.empty()) {
                result.pushKV("address", addressStr);
            }
            result.pushKV("address_count", addressCount);

            return result;
        },
    };
}

static RPCHelpMan ListDigiDollarUnspentRpc(const std::string& rpc_name)
{
    return RPCHelpMan{rpc_name,
                "\nReturns array of unspent DigiDollar transaction outputs\n"
                "with between minconf and maxconf (inclusive) confirmations.\n"
                "Optionally filter to only include txouts paid to specified DigiDollar addresses.\n",
                {
                    {"minconf", RPCArg::Type::NUM, RPCArg::Default{1}, "The minimum confirmations to filter"},
                    {"maxconf", RPCArg::Type::NUM, RPCArg::Default{9999999}, "The maximum confirmations to filter"},
                    {"addresses", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "The DigiDollar addresses to filter",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "DigiDollar address"},
                        },
                    },
                    {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{true}, "Include outputs that are not safe to spend"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "txid", "the transaction id"},
                            {RPCResult::Type::NUM, "vout", "the vout value"},
                            {RPCResult::Type::STR, "address", "the DigiDollar address"},
                            {RPCResult::Type::STR, "scriptPubKey", "the script key"},
                            {RPCResult::Type::NUM, "amount", "the DigiDollar amount in cents"},
                            {RPCResult::Type::NUM, "confirmations", "The number of confirmations"},
                            {RPCResult::Type::BOOL, "spendable", "Whether the wallet can spend this output"},
                            {RPCResult::Type::BOOL, "safe", "Whether this output is considered safe to spend"},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli(rpc_name, "") +
                    HelpExampleCli(rpc_name, "0 9999999 '[\"RD...\"]'") +
                    HelpExampleRpc(rpc_name, "0, 9999999, [\"RD...\"]")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<wallet::CWallet> const pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not found");
            }

            {
                node::NodeContext* node_ctx = pwallet->chain().context();
                if (node_ctx) {
                    ChainstateManager& chainman = *node_ctx->chainman;
                    const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                    if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                        throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                    }
                }
            }

            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            }

            const int min_depth = OptionalParamIsSet(request, 0) ? request.params[0].getInt<int>() : 1;
            const int max_depth = OptionalParamIsSet(request, 1) ? request.params[1].getInt<int>() : 9999999;
            if (min_depth < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Minimum confirmations must be non-negative");
            }
            if (max_depth < min_depth) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Maximum confirmations must be greater or equal to minimum confirmations");
            }

            std::set<std::string> destinations;
            if (OptionalParamIsSet(request, 2)) {
                UniValue inputs = request.params[2].get_array();
                for (unsigned int idx = 0; idx < inputs.size(); ++idx) {
                    const std::string address = inputs[idx].get_str();
                    std::string address_error;
                    if (!ValidateDigiDollarAddressForCurrentNetwork(address, address_error)) {
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, address_error + ": " + address);
                    }
                    if (!destinations.insert(address).second) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, duplicated address: " + address);
                    }
                }
            }

            const bool include_unsafe = OptionalParamIsSet(request, 3) ? request.params[3].get_bool() : true;
            const bool wallet_can_sign_dd =
                !pwallet->IsLocked() &&
                !pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);

            pwallet->BlockUntilSyncedToCurrentChain();

            UniValue results(UniValue::VARR);
            std::set<uint256> trusted_parents;
            for (const DDUtxo& dd_utxo : dd_wallet->GetDDUTXOs(/*include_unconfirmed=*/true)) {
                CTxOut txout;
                int depth = 0;
                bool safe = false;
                {
                    LOCK(pwallet->cs_wallet);
                    const wallet::CWalletTx* wtx = pwallet->GetWalletTx(dd_utxo.outpoint.hash);
                    if (!wtx || dd_utxo.outpoint.n >= wtx->tx->vout.size()) continue;
                    if (pwallet->IsSpent(dd_utxo.outpoint)) continue;
                    depth = pwallet->GetTxDepthInMainChain(*wtx);
                    if (depth < 0) continue;
                    if (depth == 0 && !wtx->InMempool()) continue;
                    safe = wallet::CachedTxIsTrusted(*pwallet, *wtx, trusted_parents);
                    if (depth == 0 && (wtx->mapValue.count("replaces_txid") || wtx->mapValue.count("replaced_by_txid"))) {
                        safe = false;
                    }
                    txout = wtx->tx->vout[dd_utxo.outpoint.n];
                }

                if (depth < min_depth || depth > max_depth) continue;

                CTxDestination dest;
                if (!ExtractDestination(txout.scriptPubKey, dest)) continue;
                const std::string address = EncodeDigiDollarAddress(dest);
                if (address.empty()) continue;
                if (!destinations.empty() && !destinations.count(address)) continue;

                if (!include_unsafe && !safe) continue;

                UniValue entry(UniValue::VOBJ);
                entry.pushKV("txid", dd_utxo.outpoint.hash.GetHex());
                entry.pushKV("vout", static_cast<int>(dd_utxo.outpoint.n));
                entry.pushKV("address", address);
                entry.pushKV("scriptPubKey", HexStr(txout.scriptPubKey));
                entry.pushKV("amount", int64_t{dd_utxo.dd_amount});
                entry.pushKV("confirmations", depth);
                entry.pushKV("spendable", dd_utxo.is_spendable && wallet_can_sign_dd);
                entry.pushKV("safe", safe);
                results.push_back(entry);
            }

            return results;
        },
    };
}

RPCHelpMan listdigidollarunspent()
{
    return ListDigiDollarUnspentRpc("listdigidollarunspent");
}

RPCHelpMan listdigidollarutxos()
{
    return ListDigiDollarUnspentRpc("listdigidollarutxos");
}
#endif

static RPCHelpMan estimatecollateral()
{
    return RPCHelpMan{"estimatecollateral",
                "\nEstimate DGB collateral requirement for minting DigiDollar.\n"
                "Calculates the required DGB amount based on DD amount, lock tier, and current system conditions.\n",
                {
                    {"dd_amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "DigiDollar amount to mint in cents (min 10000/$100, max 10000000/$100K)"},
                    {"lock_tier", RPCArg::Type::NUM, RPCArg::Optional::NO, "Lock tier 0-9 (0=1h testing, 1=30d, 2=90d, 3=180d, 4=1y, 5=2y, 6=3y, 7=5y, 8=7y, 9=10y)"},
                    {"oracle_price_micro_usd", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Custom DGB price in micro-USD (1,000,000 = $1.00). Uses current oracle if omitted."}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_AMOUNT, "required_dgb", "Minimum consensus DGB collateral amount"},
                        {RPCResult::Type::STR_AMOUNT, "minimum_required_dgb", "Minimum consensus DGB collateral amount"},
                        {RPCResult::Type::STR_AMOUNT, "wallet_collateral_dgb", "DGB collateral the wallet mint builder will lock, including safety margin"},
                        {RPCResult::Type::STR_AMOUNT, "collateral_safety_margin_dgb", "Extra DGB collateral added by the wallet safety margin"},
                        {RPCResult::Type::STR_AMOUNT, "dd_amount", "DigiDollar amount to mint (in cents)"},
                        {RPCResult::Type::NUM, "lock_tier", "Lock tier used"},
                        {RPCResult::Type::NUM, "lock_days", "Lock period in days"},
                        {RPCResult::Type::NUM, "base_ratio", "Base collateral ratio percentage"},
                        {RPCResult::Type::NUM, "dca_multiplier", "DCA multiplier applied"},
                        {RPCResult::Type::NUM, "effective_ratio", "Final collateral ratio (base * DCA)"},
                        {RPCResult::Type::NUM, "oracle_price_micro_usd", "DGB price in micro-USD (1,000,000 = $1.00)"},
                        {RPCResult::Type::NUM, "oracle_price_usd", "DGB price in USD"},
                        {RPCResult::Type::NUM, "system_health", "Current system health percentage"},
                        {RPCResult::Type::STR, "health_tier", "System health tier"},
                        {RPCResult::Type::NUM, "usd_value", "DigiDollar face value in USD"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("estimatecollateral", "10000 3") +
                    HelpExampleCli("estimatecollateral", "50000 5 6500") +
                    HelpExampleRpc("estimatecollateral", "10000, 3") +
                    HelpExampleRpc("estimatecollateral", "50000, 5, 6500")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            CAmount ddAmount = request.params[0].getInt<int64_t>();
            int lockTier = request.params[1].getInt<int>();

            // Validate parameters early (before oracle fetch)
            if (ddAmount <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "DD amount must be positive");
            }

            // Validate against consensus mint limits
            {
                const auto& chainParams = Params();
                const auto& ddParams = chainParams.GetDigiDollarParams();
                if (!DigiDollar::IsValidMintAmount(ddAmount, ddParams)) {
                    if (ddAmount < ddParams.minMintAmount) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER,
                            strprintf("Minimum mint amount is $%d (%d cents)",
                                ddParams.minMintAmount / 100, ddParams.minMintAmount));
                    } else {
                        throw JSONRPCError(RPC_INVALID_PARAMETER,
                            strprintf("Maximum mint amount is $%d (%d cents)",
                                ddParams.maxMintAmount / 100, ddParams.maxMintAmount));
                    }
                }
            }

            if (lockTier < 0 || lockTier > 9) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Lock tier must be between 0 and 9 (0 = 1 hour testing tier)");
            }

            // Get oracle price in micro-USD: use provided value or fetch from real oracle system
            CAmount oraclePriceMicroUSD;
            if (OptionalParamIsSet(request, 2)) {
                // User-provided value is in micro-USD (1,000,000 = $1.00)
                oraclePriceMicroUSD = request.params[2].getInt<int64_t>();
            } else {
                // Use real oracle price from OracleIntegration (returns micro-USD)
                oraclePriceMicroUSD = OracleIntegration::GetCurrentOraclePriceMicroUSD();
                if (oraclePriceMicroUSD <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                    MockOracleManager::GetInstance().IsEnabled()) {
                    oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
                }
                if (oraclePriceMicroUSD <= 0) {
                    throw JSONRPCError(RPC_MISC_ERROR, "Oracle price not available. Start oracle with 'startoracle' or provide price as third parameter.");
                }
            }

            if (oraclePriceMicroUSD <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Oracle price must be positive");
            }

            // Calculate collateral requirements
            int lockDays = GetLockDaysForTier(lockTier);
            int baseRatio = GetMinCollateralRatio(lockTier);

            // Use the same chain-derived health source and empty-supply behavior
            // as calculatecollateralrequirement().
            int systemHealth = GetDigiDollarRpcSystemHealth(request, oraclePriceMicroUSD, 30000);
            auto healthTier = DynamicCollateralAdjustment::GetCurrentTier(systemHealth);
            double dcaMultiplier = healthTier.multiplier;
            int effectiveRatio = DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemHealth);
            if (effectiveRatio <= 0 || effectiveRatio == std::numeric_limits<int>::max()) {
                throw JSONRPCError(RPC_MISC_ERROR, "DCA collateral ratio calculation failed");
            }

            // Calculate required DGB using micro-USD precision
            // Formula: DGB_sats = (DD_cents * COIN * ratio * 100) / oracle_micro_usd
            // Example: $100 DD at $0.00631 DGB with 150% ratio (oracle_micro_usd = 6310)
            //   = (10000 cents * 100000000 * 150 * 100) / 6310
            //   = 15,000,000,000,000,000 / 6310
            //   = 2,377,179,080,509 sats = ~23,772 DGB
            // Use __int128 to avoid uint64 overflow for large DD amounts
            __int128 numerator = static_cast<__int128>(ddAmount) * static_cast<__int128>(COIN) *
                                 static_cast<__int128>(effectiveRatio) * 100;
            __int128 denominator = static_cast<__int128>(oraclePriceMicroUSD);
            __int128 result128_est = (numerator + denominator - 1) / denominator;
            if (result128_est > static_cast<__int128>(MAX_MONEY)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Required collateral exceeds maximum money supply");
            }
            CAmount requiredDGB = static_cast<CAmount>(result128_est);
            const CAmount walletCollateralDGB = DigiDollar::ApplyCollateralSafetyMargin(requiredDGB);
            if (walletCollateralDGB <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Wallet collateral requirement exceeds maximum money supply");
            }
            const CAmount collateralSafetyMarginDGB = walletCollateralDGB - requiredDGB;

            UniValue result(UniValue::VOBJ);
            result.pushKV("required_dgb", ValueFromAmount(requiredDGB));
            result.pushKV("minimum_required_dgb", ValueFromAmount(requiredDGB));
            result.pushKV("wallet_collateral_dgb", ValueFromAmount(walletCollateralDGB));
            result.pushKV("collateral_safety_margin_dgb", ValueFromAmount(collateralSafetyMarginDGB));
            result.pushKV("dd_amount", int64_t{ddAmount});
            result.pushKV("lock_tier", lockTier);
            result.pushKV("lock_days", lockDays);
            result.pushKV("base_ratio", baseRatio);
            result.pushKV("dca_multiplier", dcaMultiplier);
            result.pushKV("effective_ratio", effectiveRatio);
            result.pushKV("oracle_price_micro_usd", int64_t{oraclePriceMicroUSD});
            result.pushKV("oracle_price_usd", oraclePriceMicroUSD / 1000000.0);
            result.pushKV("system_health", systemHealth);
            result.pushKV("health_tier", healthTier.status);
            // Fix: ddAmount is in cents, so USD value = ddAmount / 100.0
            // Previously this path treated cents as satoshis, producing a
            // value ~100,000x too small (e.g., $0.001 instead of $100).
            result.pushKV("usd_value", ddAmount / 100.0);

            return result;
        },
    };
}

#ifdef ENABLE_WALLET
RPCHelpMan getredemptioninfo()
{
    return RPCHelpMan{"getredemptioninfo",
                "\nGet redemption information for a specific DigiDollar position.\n"
                "Shows whether position can be redeemed and potential return amounts.\n",
                {
                    {"position_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Position ID (transaction hash of mint)"},
                    {"dd_amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "Amount of DD to redeem. If provided, it must equal the full position amount because partial redemption is not supported."}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "position_id", "Position identifier"},
                        {RPCResult::Type::BOOL, "can_redeem", "Whether position can be redeemed now"},
                        {RPCResult::Type::STR, "redemption_path", "Available redemption path (normal/emergency)"},
                        {RPCResult::Type::STR_AMOUNT, "total_dd_minted", "Total DD minted in this position"},
                        {RPCResult::Type::STR_AMOUNT, "redeemable_dd", "DD amount that can be redeemed"},
                        {RPCResult::Type::STR_AMOUNT, "required_dd_burn", "DD amount that must be burned to redeem the position"},
                        {RPCResult::Type::BOOL, "err_active", "Whether ERR burn rules are active"},
                        {RPCResult::Type::NUM, "err_system_health", "System health percentage used for ERR"},
                        {RPCResult::Type::NUM, "err_ratio_bps", "ERR ratio in basis points"},
                        {RPCResult::Type::STR_AMOUNT, "dgb_return", "Estimated DGB return amount"},
                        {RPCResult::Type::NUM, "unlock_height", "Block height when position unlocks"},
                        {RPCResult::Type::NUM, "timelock_remaining", "Blocks until unlock (0 if unlocked)"},
                        {RPCResult::Type::STR_AMOUNT, "penalty_amount", "Penalty amount (if early redemption)"},
                        {RPCResult::Type::STR, "status", "Position status (pending/active/unlocked/pending_redeem/redeemed)"},
                        {RPCResult::Type::STR, "unlock_date", "Estimated unlock date"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getredemptioninfo", "\"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\"") +
                    HelpExampleCli("getredemptioninfo", "\"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\" 10000") +
                    HelpExampleRpc("getredemptioninfo", "\"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Get wallet (needed for position lookup and activation check)
            std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet not found");

            // Check DigiDollar activation via wallet's chain context
            {
                node::NodeContext* node_ctx = pwallet->chain().context();
                if (node_ctx) {
                    ChainstateManager& chainman = *node_ctx->chainman;
                    const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                    if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                        throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                    }
                }
            }

            // Parse parameters
            std::string positionIdStr = request.params[0].get_str();
            CAmount ddAmount = OptionalParamIsSet(request, 1) ?
                              ParseDigiDollarRpcAmount(request.params[1]) : 0;

            // Validate position ID format
            if (!IsHex(positionIdStr) || positionIdStr.length() != 64) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid position ID format");
            }

            uint256 positionId;
            positionId.SetHex(positionIdStr);

            // Get DD wallet and look up the real position
            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            pwallet->BlockUntilSyncedToCurrentChain();
            dd_wallet->ReconcilePositionStates();

            LOCK(pwallet->cs_wallet);
            WalletCollateralPosition foundPosition;
            bool found = false;

            for (const auto& pos : dd_wallet->GetDDTimeLocks(false)) {
                if (pos.dd_timelock_id == positionId) {
                    foundPosition = pos;
                    found = true;
                    break;
                }
            }

            if (!found) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Position %s not found in wallet", positionIdStr));
            }

            int currentHeight = pwallet->GetLastBlockHeight();
            int blocksRemaining = std::max(0, static_cast<int>(foundPosition.unlock_height - currentHeight));
            const int confirmations = dd_wallet->GetDDTransactionConfirmations(positionId);
            const bool walletPrivateKeysDisabled = pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);
            const bool pendingRedeem = !foundPosition.is_active &&
                                       HasPendingDigiDollarRedeem(*pwallet, foundPosition.dd_timelock_id);

            // Determine status
            std::string status;
            if (pendingRedeem) {
                status = "pending_redeem";
            } else if (!foundPosition.is_active) {
                status = "redeemed";
            } else if (confirmations <= 0) {
                status = "pending";
            } else if (blocksRemaining == 0) {
                status = "unlocked";
            } else {
                status = "active";
            }

            // Determine redemption path based on system health
            std::string redemptionPath = "normal";
            CAmount penaltyAmount = 0;
            int redemptionSystemHealth = DynamicCollateralAdjustment::GetCurrentSystemHealth();
            auto errState = DigiDollar::ERR::EmergencyRedemptionRatio::GetCurrentState();
            if (errState.isActive && errState.systemHealth < 100) {
                redemptionSystemHealth = errState.systemHealth;
            }
            const bool errActive = redemptionSystemHealth >= 0 && redemptionSystemHealth < 100;
            CAmount requiredDDBurn = foundPosition.dd_minted;
            int errRatioBps = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRRatioBps(redemptionSystemHealth);
            if (errActive) {
                redemptionPath = "emergency";
                requiredDDBurn = DigiDollar::ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(
                    foundPosition.dd_minted, redemptionSystemHealth);
                penaltyAmount = requiredDDBurn - foundPosition.dd_minted;
            }

            if (ddAmount > 0 && ddAmount != foundPosition.dd_minted) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Exact-amount redemption required: must redeem full vault amount of %d cents (requested: %d cents). "
                              "Partial redemption is not supported - the entire vault must be closed at once.",
                              foundPosition.dd_minted, ddAmount));
            }

            // Determine if position can be redeemed. Read-only/watch-only and
            // locked encrypted wallets can monitor positions, but cannot sign
            // a redemption.
            const bool walletLocked = pwallet->IsLocked();
            const bool walletCannotSign = walletPrivateKeysDisabled || walletLocked;
            bool canRedeem = confirmations > 0 && foundPosition.is_active && blocksRemaining == 0 &&
                             foundPosition.dgb_collateral > 0 && !walletCannotSign;

            // Redeemable amount is always the full vault amount; required burn
            // may be higher during ERR.
            CAmount redeemableDD = foundPosition.dd_minted;

            // Estimate DGB return: normal and ERR both return full locked collateral.
            // Fees are paid from separate DGB fee inputs and are not a collateral haircut.
            CAmount dgbReturn = foundPosition.dgb_collateral;

            // Compute dates from block heights using 15-second block time
            int64_t now = GetTime();
            // Unlock date
            std::string unlockDateStr;
            if (blocksRemaining > 0) {
                int64_t unlockTimestamp = now + static_cast<int64_t>(blocksRemaining) * 15;
                unlockDateStr = FormatISO8601DateTime(unlockTimestamp);
            } else {
                // Already unlocked — compute when it unlocked
                int64_t unlockTimestamp = now - (static_cast<int64_t>(currentHeight) - foundPosition.unlock_height) * 15;
                unlockDateStr = FormatISO8601DateTime(unlockTimestamp);
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("position_id", positionIdStr);
            result.pushKV("can_redeem", canRedeem);
            result.pushKV("redemption_path", redemptionPath);
            result.pushKV("total_dd_minted", int64_t{foundPosition.dd_minted});
            result.pushKV("redeemable_dd", int64_t{redeemableDD});
            result.pushKV("required_dd_burn", int64_t{requiredDDBurn});
            result.pushKV("err_active", errActive);
            result.pushKV("err_system_health", redemptionSystemHealth);
            result.pushKV("err_ratio_bps", errRatioBps);
            result.pushKV("dgb_return", ValueFromAmount(dgbReturn));
            result.pushKV("unlock_height", static_cast<int>(foundPosition.unlock_height));
            result.pushKV("timelock_remaining", blocksRemaining);
            result.pushKV("penalty_amount", int64_t{penaltyAmount});
            result.pushKV("status", status);
            result.pushKV("unlock_date", unlockDateStr);

            return result;
        },
    };
}

RPCHelpMan listdigidollartxs()
{
    return RPCHelpMan{"listdigidollartxs",
                "\nList DigiDollar transactions from the wallet.\n"
                "Returns recent DD transactions including mints, sends, receives, redemptions, and redemption change.\n",
                {
                    {"count", RPCArg::Type::NUM, RPCArg::Default{10}, "Number of transactions to return"},
                    {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "Number of transactions to skip"},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Filter by specific DD address"},
                    {"category", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Filter by category (mint/send/receive/redeem/redeem_change)"}
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "Transaction ID"},
                                {RPCResult::Type::STR, "category", "Transaction category (mint/send/receive/redeem/redeem_change)"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "DD amount (positive for receives, negative for sends)"},
                                {RPCResult::Type::STR, "address", "DigiDollar address involved"},
                                {RPCResult::Type::NUM, "confirmations", "Number of confirmations"},
                                {RPCResult::Type::NUM, "blockheight", "Block height (if confirmed)"},
                                {RPCResult::Type::STR, "blockhash", "Block hash (if confirmed)"},
                                {RPCResult::Type::NUM, "time", "Transaction timestamp"},
                                {RPCResult::Type::STR_AMOUNT, "fee", "Transaction fee paid (if applicable)"},
                                {RPCResult::Type::STR, "comment", "Transaction comment (if any)"},
                                {RPCResult::Type::BOOL, "abandoned", "Whether transaction was abandoned"},
                                {RPCResult::Type::NUM, "lock_tier", "Collateral lock tier for mint transactions (0-9)"},
                                {RPCResult::Type::BOOL, "in_mempool", "Whether the wallet currently sees the transaction in mempool/stempool"},
                                {RPCResult::Type::STR, "wallet_state", "DD wallet display state: local, pending, confirmed, conflicted, or abandoned"}
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("listdigidollartxs", "") +
                    HelpExampleCli("listdigidollartxs", "20 10") +
                    HelpExampleCli("listdigidollartxs", "10 0 \"DDtestaddress123456789abcdef\"") +
                    HelpExampleCli("listdigidollartxs", "10 0 \"\" \"mint\"") +
                    HelpExampleRpc("listdigidollartxs", "20, 10") +
                    HelpExampleRpc("listdigidollartxs", "10, 0, \"DDtestaddress123456789abcdef\", \"mint\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                std::shared_ptr<wallet::CWallet> pwallet_check = wallet::GetWalletForJSONRPCRequest(request);
                if (pwallet_check) {
                    node::NodeContext* node_ctx = pwallet_check->chain().context();
                    if (node_ctx) {
                        ChainstateManager& chainman = *node_ctx->chainman;
                        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                        if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                            throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                        }
                    }
                }
            }
            // PHASE 7.7: Integration with DigiDollarWallet backend

            // Get wallet
            std::shared_ptr<wallet::CWallet> const pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not found");
            }

            // Get DigiDollar wallet
            DigiDollarWallet* dd_wallet = pwallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
            }

            // Parse parameters
            int count = OptionalParamIsSet(request, 0) ? request.params[0].getInt<int>() : 10;
            int skip = OptionalParamIsSet(request, 1) ? request.params[1].getInt<int>() : 0;
            std::string addressFilter = OptionalParamIsSet(request, 2) ?
                                       request.params[2].get_str() : "";
            std::string categoryFilter = OptionalParamIsSet(request, 3) ?
                                        request.params[3].get_str() : "";

            // Validate parameters
            if (count < 0 || count > 1000) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Count must be between 0 and 1000");
            }
            if (skip < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Skip must be non-negative");
            }

            // Get transaction history from wallet
            std::vector<DDTransaction> transactions = dd_wallet->GetDDTransactionHistory();

            UniValue result(UniValue::VARR);
            int processed = 0;
            int skipped = 0;

            for (const auto& tx : transactions) {
                // Apply filters
                if (!addressFilter.empty() && tx.address != addressFilter) continue;
                if (!categoryFilter.empty() && tx.category != categoryFilter) continue;

                // Apply skip
                if (skipped < skip) {
                    skipped++;
                    continue;
                }

                // Apply count limit
                if (processed >= count) break;

                UniValue txInfo(UniValue::VOBJ);
                txInfo.pushKV("txid", tx.txid);
                txInfo.pushKV("category", tx.category);
                txInfo.pushKV("amount", tx.incoming ? tx.amount : -tx.amount);
                txInfo.pushKV("address", tx.address);
                txInfo.pushKV("confirmations", tx.confirmations);
                txInfo.pushKV("blockheight", tx.blockheight);
                txInfo.pushKV("blockhash", tx.blockhash);
                txInfo.pushKV("time", static_cast<int64_t>(tx.timestamp));
                txInfo.pushKV("fee", ValueFromAmount(tx.fee));
                txInfo.pushKV("comment", tx.comment);
                txInfo.pushKV("abandoned", tx.abandoned);
                txInfo.pushKV("lock_tier", tx.lock_tier);
                txInfo.pushKV("in_mempool", tx.in_mempool);
                txInfo.pushKV("wallet_state", tx.is_local ? "local" :
                    (tx.abandoned ? "abandoned" :
                     (tx.confirmations < 0 ? "conflicted" :
                      (tx.confirmations > 0 ? "confirmed" : "pending"))));

                result.push_back(txInfo);
                processed++;
            }

            return result;
        },
    };
}
#endif

static RPCHelpMan getoracleprice()
{
    return RPCHelpMan{"getoracleprice",
                "\nGet current DGB/USD price from the oracle system.\n"
                "Returns the latest price data used for DigiDollar calculations.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "price_micro_usd", "Current DGB price in micro-USD (1,000,000 = $1.00)"},
                        {RPCResult::Type::NUM, "price_cents", "Current DGB price in cents per DGB"},
                        {RPCResult::Type::NUM, "price_usd", "Current DGB price in USD (full precision)"},
                        {RPCResult::Type::NUM, "last_update_height", "Block height of last price update"},
                        {RPCResult::Type::NUM, "last_update_time", "Timestamp of last update"},
                        {RPCResult::Type::NUM, "validity_blocks", "Blocks remaining until price expires"},
                        {RPCResult::Type::BOOL, "is_stale", "Whether price data is considered stale"},
                        {RPCResult::Type::NUM, "oracle_count", "Number of active oracles"},
                        {RPCResult::Type::STR, "status", "Oracle system status (active/warning/error)"},
                        {RPCResult::Type::NUM, "24h_high", "24-hour high price in cents"},
                        {RPCResult::Type::NUM, "24h_low", "24-hour low price in cents"},
                        {RPCResult::Type::NUM, "volatility", "Current price volatility percentage"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getoracleprice", "") +
                    HelpExampleRpc("getoracleprice", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            // Get chainman for blockchain info
            const ChainstateManager& chainman = EnsureAnyChainman(request.context);

            // Get real oracle data from the oracle system
            OracleBundleManager& oracle_manager = OracleBundleManager::GetInstance();
            OracleBundleManager::OracleStats stats = oracle_manager.GetStats();

            int currentHeight = chainman.ActiveChain().Height();

            // In RegTest mode, check MockOracleManager first
            bool usingMockOracle = false;
            CAmount priceMicroUSD = 0;
            int64_t priceCents = 0;
            double priceUSD = 0.0;
            int lastBundleHeight = 0;
            int64_t lastBundleTime = 0;
            int64_t freshestPendingTime = 0;
            std::set<uint32_t> reportingOracleIds;

            if (Params().GetChainType() == ChainType::REGTEST) {
                MockOracleManager& mock = MockOracleManager::GetInstance();
                if (mock.IsEnabled()) {
                    CAmount mockPrice = mock.GetCurrentPrice();
                    if (mockPrice > 0) {
                        usingMockOracle = true;
                        priceMicroUSD = mockPrice;
                        // Convert micro-USD to cents with full precision
                        priceCents = priceMicroUSD / 10000;  // integer division: micro-USD to cents
                        priceUSD = static_cast<double>(priceMicroUSD) / 1000000.0;
                        // Mock oracle is always "current" - use current time
                        lastBundleTime = GetTime();
                        lastBundleHeight = currentHeight;
                        // Mock uses 7 test oracles in regtest
                        for (uint32_t i = 0; i < 7; i++) {
                            reportingOracleIds.insert(i);
                        }
                    }
                }
            }

            if (!usingMockOracle) {
                // Get the raw micro-USD price from the oracle (full precision)
                priceMicroUSD = oracle_manager.GetLatestPrice();
                // Derive cents from the same micro-USD source (integer division)
                priceCents = priceMicroUSD / 10000;
                // Calculate true USD price from micro-USD (full precision)
                priceUSD = static_cast<double>(priceMicroUSD) / 1000000.0;

                // Scan last 20 blocks to find the actual last oracle bundle height
                // and count unique reporting oracles (same approach as getalloracleprices)
                {
                    LOCK(cs_main);
                    const int64_t now = GetTime();
                    for (int h = currentHeight; h >= std::max(0, currentHeight - 19); --h) {
                        CBlockIndex* pindex = chainman.ActiveChain()[h];
                        if (!pindex) continue;
                        CBlock block;
                        if (!chainman.m_blockman.ReadBlockFromDisk(block, *pindex)) continue;
                        if (block.vtx.empty()) continue;
                        COracleBundle bundle;
                        if (oracle_manager.ExtractOracleBundle(*block.vtx[0], bundle)) {
                            if (!IsFreshOracleTimestamp(bundle.timestamp, now)) continue;
                            if (h > lastBundleHeight) {
                                lastBundleHeight = h;
                                lastBundleTime = bundle.timestamp;
                            }
                            for (const auto& msg : bundle.messages) {
                                if (!IsFreshOracleTimestamp(msg.timestamp, now)) continue;
                                reportingOracleIds.insert(msg.oracle_id);
                            }
                        }
                    }
                }

                // Also count oracles with pending P2P messages not yet on-chain.
                // Track the freshest pending timestamp for time-based staleness.
                {
                    int64_t now = GetTime();
                    std::vector<COraclePriceMessage> pending = oracle_manager.GetPendingMessages();
                    for (const auto& msg : pending) {
                        if (!IsFreshOracleTimestamp(msg.timestamp, now)) continue;
                        reportingOracleIds.insert(msg.oracle_id);
                        if (msg.timestamp > freshestPendingTime) {
                            freshestPendingTime = msg.timestamp;
                        }
                    }
                }
            }

            // Calculate validity and staleness using dual threshold:
            //
            // Block-based: stale if no oracle bundle within N blocks of chain tip.
            // Time-based:  stale if no oracle data (on-chain or pending) within
            //              ORACLE_MAX_AGE_SECONDS (1 hour).
            //
            // A recent block height only helps if the scanned bundle timestamp is
            // still fresh. Otherwise an oracle outage with no new blocks can leave
            // an old bundle close to the tip while the usable price has expired.
            int validityBlocks = 20; // Oracle data valid for 20 blocks
            // Use on-chain bundle height if available, otherwise use current
            // height when oracles are actively reporting via P2P pending messages.
            int lastUpdateHeight = lastBundleHeight > 0 ? lastBundleHeight :
                (freshestPendingTime > 0 ? currentHeight : 0);
            int64_t freshestDataTime = std::max(lastBundleTime, freshestPendingTime);
            int64_t lastUpdateTime = freshestDataTime > 0 ? freshestDataTime : (stats.last_update > 0 ? stats.last_update : 0);

            bool blockBasedFresh = lastBundleHeight > 0 && (currentHeight - lastBundleHeight) <= validityBlocks;
            bool timeBasedFresh = freshestDataTime > 0 && (GetTime() - freshestDataTime) <= ORACLE_MAX_AGE_SECONDS;
            bool isStale = !usingMockOracle && !(blockBasedFresh || timeBasedFresh);

            // Oracle count from actual unique reporting oracles (on-chain + pending)
            size_t activeOracleCount = reportingOracleIds.size();
            std::string status = stats.has_consensus ? "active" : (activeOracleCount > 0 ? "warning" : "error");

            // Compute real 24h high/low by scanning oracle price history
            double high24h = priceCents;
            double low24h = priceCents;
            double volatility = 0.0;
            {
                const int scanBlocks = 5760; // ~24h at 15s blocks
                int startH = std::max(0, currentHeight - scanBlocks);
                std::vector<double> samples;
                double histHigh = 0.0;
                double histLow = std::numeric_limits<double>::max();
                bool hasHistory = false;

                for (int h = startH; h <= currentHeight; ++h) {
                    CAmount hp = oracle_manager.GetOraclePriceForHeight(h);
                    if (hp > 0) {
                        double hCents = static_cast<double>(hp) / 10000.0;
                        if (hCents > histHigh) histHigh = hCents;
                        if (hCents < histLow) histLow = hCents;
                        hasHistory = true;
                        // Sample every ~288 blocks for volatility (up to 20 samples)
                        if (samples.size() < 20 && (h == startH || (h - startH) % std::max(1, scanBlocks / 20) == 0)) {
                            samples.push_back(hCents);
                        }
                    }
                }

                if (hasHistory) {
                    high24h = histHigh;
                    low24h = histLow;
                }

                // Compute volatility as coefficient of variation (std-dev/mean * 100)
                if (samples.size() >= 2) {
                    double sum = 0.0;
                    for (double s : samples) sum += s;
                    double mean = sum / samples.size();
                    if (mean > 0.0) {
                        double sqSum = 0.0;
                        for (double s : samples) sqSum += (s - mean) * (s - mean);
                        double stddev = std::sqrt(sqSum / samples.size());
                        volatility = (stddev / mean) * 100.0;
                    }
                }
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("price_micro_usd", priceMicroUSD);
            result.pushKV("price_cents", priceCents);
            result.pushKV("price_usd", priceUSD);
            result.pushKV("last_update_height", lastUpdateHeight);
            result.pushKV("last_update_time", lastUpdateTime);
            result.pushKV("validity_blocks", validityBlocks);
            result.pushKV("is_stale", isStale);
            result.pushKV("oracle_count", static_cast<int>(activeOracleCount));
            result.pushKV("status", status);
            result.pushKV("24h_high", high24h);
            result.pushKV("24h_low", low24h);
            result.pushKV("volatility", volatility);

            return result;
        },
    };
}

static RPCHelpMan getprotectionstatus()
{
    return RPCHelpMan{"getprotectionstatus",
                "\nGet status of DigiDollar protection systems.\n"
                "Returns information about DCA, ERR, volatility protection, and other safeguards.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::OBJ, "oracle", "Oracle availability and fail-closed minting status",
                            {
                                {RPCResult::Type::BOOL, "available", "Whether a live oracle price is available"},
                                {RPCResult::Type::STR, "status", "Oracle status: available or unavailable"},
                                {RPCResult::Type::BOOL, "minting_restricted", "Whether minting is restricted because the oracle is unavailable"},
                                {RPCResult::Type::STR, "minting_restricted_reason", "Reason for minting restriction"}
                            }
                        },
                        {RPCResult::Type::OBJ, "dca", "Dynamic Collateral Adjustment status",
                            {
                                {RPCResult::Type::BOOL, "active", "Whether DCA is currently active"},
                                {RPCResult::Type::NUM, "current_multiplier", "Current DCA multiplier"},
                                {RPCResult::Type::STR, "tier", "Current DCA tier"},
                                {RPCResult::Type::NUM, "system_health", "System health percentage"},
                                {RPCResult::Type::STR, "trend", "Health trend (improving/stable/declining)"}
                            }
                        },
                        {RPCResult::Type::OBJ, "err", "Emergency Redemption Ratio status",
                            {
                                {RPCResult::Type::BOOL, "active", "Whether ERR is currently active"},
                                {RPCResult::Type::NUM, "threshold", "ERR activation threshold (%)"},
                                {RPCResult::Type::NUM, "current_ratio", "Current system ratio (%)"},
                                {RPCResult::Type::NUM, "err_ratio_bps", "ERR ratio in basis points"},
                                {RPCResult::Type::NUM, "required_burn_per_10000", "DD burn required for 10000 cents under current ERR state"},
                                {RPCResult::Type::STR, "status", "ERR status (normal/warning/active)"},
                                {RPCResult::Type::STR, "evaluation_status", "priced or oracle_unavailable"}
                            }
                        },
                        {RPCResult::Type::OBJ, "volatility", "Volatility protection status",
                            {
                                {RPCResult::Type::BOOL, "protection_active", "Whether volatility protection is active"},
                                {RPCResult::Type::NUM, "current_volatility", "Current volatility percentage"},
                                {RPCResult::Type::NUM, "protection_threshold", "Volatility protection threshold"},
                                {RPCResult::Type::BOOL, "minting_restricted", "Whether minting is restricted due to volatility"},
                                {RPCResult::Type::NUM, "anchor_price_micro_usd", "Chain-derived lagged-median mint anchor price in micro-USD (0 = no in-window bundle samples)"},
                                {RPCResult::Type::NUM, "anchor_deviation_bps", "Absolute deviation of the current oracle price from the anchor, in basis points"},
                                {RPCResult::Type::BOOL, "minting_paused", "Whether the volatility-anchor rule currently pauses minting (always false below volatility_fix_height)"},
                                {RPCResult::Type::NUM, "volatility_fix_height", "Activation height of the chain-derived volatility-anchor mint rule"}
                            }
                        },
                        {RPCResult::Type::OBJ, "overall", "Overall protection status",
                            {
                                {RPCResult::Type::STR, "status", "Overall system status (secure/warning/critical)"},
                                {RPCResult::Type::ARR, "active_protections", "List of currently active protections",
                                    {
                                        {RPCResult::Type::STR, "", "Protection name"}
                                    }
                                },
                                {RPCResult::Type::ARR, "warnings", "Current system warnings",
                                    {
                                        {RPCResult::Type::STR, "", "Warning message"}
                                    }
                                }
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("getprotectionstatus", "") +
                    HelpExampleRpc("getprotectionstatus", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            // Compute real system health (same approach as getdigidollarstats)
            CAmount totalCollateral = 0;
            CAmount totalDD = 0;

            const node::NodeContext& node = EnsureAnyNodeContext(request.context);
            ChainstateManager& chainman = EnsureChainman(node);

            if (g_digidollar_stats_index) {
                if (!g_digidollar_stats_index->BlockUntilSyncedToCurrentChain()) {
                    const IndexSummary summary{g_digidollar_stats_index->GetSummary()};
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                        strprintf("DigiDollar stats index is syncing. Current height: %d", summary.best_block_height));
                }
                const CBlockIndex* pindex;
                {
                    LOCK(cs_main);
                    pindex = chainman.ActiveChain().Tip();
                }
                if (pindex) {
                    auto stats = g_digidollar_stats_index->LookUpStats(*pindex);
                    if (stats) {
                        totalDD = stats->total_dd_supply;
                        totalCollateral = stats->total_collateral;
                    }
                }
            } else {
                // Fallback: UTXO scanning
                Chainstate& active_chainstate = chainman.ActiveChainstate();
                active_chainstate.ForceFlushStateToDisk();
                CCoinsView* coins_view;
                node::BlockManager* blockman;
                const CTxMemPool* mempool = node.mempool.get();
                {
                    LOCK(::cs_main);
                    coins_view = &active_chainstate.CoinsDB();
                    blockman = &active_chainstate.m_blockman;
                    if (!DigiDollar::SystemHealthMonitor::ScanUTXOSet(coins_view, &active_chainstate.CoinsTip(), blockman, mempool, &active_chainstate.m_chain, &Params().GetConsensus())) {
                        throw JSONRPCError(RPC_MISC_ERROR,
                            "DigiDollar-era block data is incomplete or unreadable; restart with -reindex");
                    }
                }
                DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();
                totalCollateral = metrics.totalCollateral;
                totalDD = metrics.totalDDSupply;
            }

            // Oracle price
            OracleBundleManager& oracle_manager = OracleBundleManager::GetInstance();
            CAmount oraclePriceMicroUSD = oracle_manager.GetLatestPrice();
            if (oraclePriceMicroUSD <= 0 && Params().GetChainType() == ChainType::REGTEST &&
                MockOracleManager::GetInstance().IsEnabled()) {
                oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
            }
            const bool oracleAvailable = oraclePriceMicroUSD > 0;
            CAmount oraclePriceMillicents = oraclePriceMicroUSD / 10;

            // System health
            int systemHealth;
            if (totalDD == 0) {
                systemHealth = 0;
            } else {
                systemHealth = DynamicCollateralAdjustment::CalculateSystemHealth(
                    totalCollateral, totalDD, oraclePriceMillicents);
            }

            auto tier = DynamicCollateralAdjustment::GetCurrentTier(systemHealth);
            bool isEmergency = oracleAvailable && totalDD > 0 && DynamicCollateralAdjustment::IsSystemEmergency(systemHealth);

            UniValue result(UniValue::VOBJ);

            UniValue oracle(UniValue::VOBJ);
            oracle.pushKV("available", oracleAvailable);
            oracle.pushKV("status", oracleAvailable ? "available" : "unavailable");
            oracle.pushKV("minting_restricted", !oracleAvailable);
            oracle.pushKV("minting_restricted_reason", oracleAvailable ? "none" : "oracle_unavailable");
            result.pushKV("oracle", oracle);

            // DCA status
            UniValue dca(UniValue::VOBJ);
            dca.pushKV("active", true);
            dca.pushKV("current_multiplier", tier.multiplier);
            dca.pushKV("tier", tier.status);
            dca.pushKV("system_health", systemHealth);
            dca.pushKV("trend", "stable");
            result.pushKV("dca", dca);

            // ERR status
            UniValue err(UniValue::VOBJ);
            err.pushKV("active", isEmergency);
            err.pushKV("threshold", 100);
            err.pushKV("current_ratio", systemHealth);
            err.pushKV("err_ratio_bps", oracleAvailable ?
                DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRRatioBps(systemHealth) : 10000);
            err.pushKV("required_burn_per_10000", int64_t{oracleAvailable ?
                DigiDollar::ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(10000, systemHealth) : 10000});
            std::string errStatus;
            if (!oracleAvailable || totalDD == 0 || systemHealth >= 100) {
                errStatus = "normal";
            } else if (systemHealth >= 95) {
                errStatus = "warning";
            } else if (systemHealth >= 85) {
                errStatus = "active";
            } else {
                errStatus = "critical";
            }
            err.pushKV("status", errStatus);
            err.pushKV("evaluation_status", oracleAvailable ? "priced" : "oracle_unavailable");
            result.pushKV("err", err);

            // Volatility protection
            auto volatilityState = Volatility::VolatilityMonitor::GetCurrentState();
            double currentVolatility = std::max({
                volatilityState.hourlyVolatility,
                volatilityState.dailyVolatility,
                volatilityState.weeklyVolatility});
            bool mintingRestricted = Volatility::VolatilityMonitor::ShouldFreezeMinting();
            bool allOperationsRestricted = Volatility::VolatilityMonitor::ShouldFreezeAll();

            // Chain-derived mint volatility anchor (nDDVolatilityFixHeight rule).
            // Computed from the same tip-derived helper consensus uses; no new
            // state. A disk-read failure (nullopt) is reported as anchor 0.
            const Consensus::Params& consensusParams = chainman.GetConsensus();
            CAmount anchorPriceMicroUSD{0};
            int nextBlockHeight{0};
            {
                LOCK(cs_main);
                const CBlockIndex* tip = chainman.ActiveChain().Tip();
                nextBlockHeight = tip ? tip->nHeight + 1 : 0;
                anchorPriceMicroUSD = GetDDMintAnchorPrice(tip, chainman.m_blockman,
                                                           consensusParams).value_or(0);
            }
            int64_t anchorDeviationBps{0};
            if (anchorPriceMicroUSD > 0 && oraclePriceMicroUSD > 0) {
                anchorDeviationBps = Volatility::CalculatePercentageChangeBps(anchorPriceMicroUSD,
                                                                              oraclePriceMicroUSD);
                if (anchorDeviationBps < 0) anchorDeviationBps = -anchorDeviationBps;
            }
            const bool anchorMintingPaused =
                nextBlockHeight >= consensusParams.nDDVolatilityFixHeight &&
                anchorPriceMicroUSD > 0 && oraclePriceMicroUSD > 0 &&
                Volatility::ExceedsThresholdBps(anchorPriceMicroUSD, oraclePriceMicroUSD,
                                                Volatility::VolatilityThresholds::FREEZE_MINT_1H_BPS);

            UniValue volatility(UniValue::VOBJ);
            volatility.pushKV("protection_active", mintingRestricted || allOperationsRestricted);
            volatility.pushKV("current_volatility", currentVolatility);
            volatility.pushKV("protection_threshold", Volatility::VolatilityThresholds::FREEZE_MINT_1H);
            volatility.pushKV("minting_restricted", mintingRestricted);
            volatility.pushKV("anchor_price_micro_usd", anchorPriceMicroUSD);
            volatility.pushKV("anchor_deviation_bps", anchorDeviationBps);
            volatility.pushKV("minting_paused", anchorMintingPaused);
            volatility.pushKV("volatility_fix_height", consensusParams.nDDVolatilityFixHeight);
            result.pushKV("volatility", volatility);

            // Overall status
            UniValue overall(UniValue::VOBJ);
            std::string overallStatus;
            if (!oracleAvailable) {
                overallStatus = totalDD > 0 ? "critical" : "warning";
            } else if (totalDD == 0) {
                overallStatus = "secure";
            } else if (isEmergency && systemHealth < 85) {
                overallStatus = "emergency";
            } else if (isEmergency) {
                overallStatus = "critical";
            } else if (systemHealth >= 150) {
                overallStatus = "secure";
            } else if (systemHealth >= 100) {
                overallStatus = "warning";
            } else {
                overallStatus = "critical";
            }
            overall.pushKV("status", overallStatus);

            UniValue activeProtections(UniValue::VARR);
            activeProtections.push_back("dca");
            if (!oracleAvailable) {
                activeProtections.push_back("oracle_fail_closed");
            }
            if (isEmergency) {
                activeProtections.push_back("err");
            }
            overall.pushKV("active_protections", activeProtections);

            UniValue warnings(UniValue::VARR);
            if (!oracleAvailable) {
                warnings.push_back("Oracle price unavailable; minting is paused");
            }
            if (systemHealth > 0 && systemHealth < 150) {
                warnings.push_back("System health below optimal threshold");
            }
            if (isEmergency) {
                warnings.push_back("Emergency redemption ratio active");
            }
            overall.pushKV("warnings", warnings);

            result.pushKV("overall", overall);

            return result;
        },
    };
}

// ---------- Shared oracle-data scanning helper (Bug #15) ----------
// Used by both getalloracleprices and getoracles so they report
// identical prices, timestamps, and statuses for every oracle.
struct ScannedOracleData {
    uint64_t price_micro_usd = 0;
    int64_t  timestamp = 0;
    int32_t  block_height = 0;   // 0 = not yet on-chain (pending/local)
    bool     signature_valid = false;
    bool     has_data = false;
    std::string price_source;    // "local", "on-chain", "pending", "none"
};

struct OracleScanResult {
    std::map<uint32_t, ScannedOracleData> oracle_data;
    int      last_bundle_height = 0;
    int64_t  last_bundle_time = 0;
    uint64_t consensus_price = 0;  // from most-recent bundle
};

/** Scan recent blocks + pending P2P + local runtime for oracle data.
 *  Both RPCs call this with the same parameters so results are identical. */
static OracleScanResult ScanOracleDataFromChain(
    const ChainstateManager& chainman,
    OracleBundleManager& bundle_manager,
    OracleManager& oracle_manager,
    int scan_blocks)
{
    OracleScanResult res;

    int tip_height = chainman.ActiveChain().Height();
    const int64_t now = GetTime();

    // 1. On-chain: scan recent blocks for oracle bundles
    {
        LOCK(cs_main);
        for (int h = tip_height; h >= std::max(0, tip_height - scan_blocks + 1); --h) {
            CBlockIndex* pindex = chainman.ActiveChain()[h];
            if (!pindex) continue;

            CBlock block;
            if (!chainman.m_blockman.ReadBlockFromDisk(block, *pindex)) continue;
            if (block.vtx.empty()) continue;

            COracleBundle bundle;
            if (bundle_manager.ExtractOracleBundle(*block.vtx[0], bundle)) {
                if (!IsFreshOracleTimestamp(bundle.timestamp, now)) continue;
                // v0x03 stores one aggregate MuSig2 signature; extracted
                // participant messages do not carry individual attestations.
                bool bundle_signature_valid = false;
                if (bundle.IsMuSig2()) {
                    std::string error;
                    bundle_signature_valid = OracleBundleManager::ValidateMuSig2Bundle(
                        bundle, h, Params().GetConsensus(), error);
                }
                if (h > res.last_bundle_height) {
                    res.last_bundle_height = h;
                    res.last_bundle_time = bundle.timestamp;
                    res.consensus_price = bundle.median_price_micro_usd;
                }

                for (const auto& msg : bundle.messages) {
                    if (!IsFreshOracleTimestamp(msg.timestamp, now)) continue;
                    if (res.oracle_data.find(msg.oracle_id) == res.oracle_data.end() ||
                        !res.oracle_data[msg.oracle_id].has_data) {
                        auto& od = res.oracle_data[msg.oracle_id];
                        od.price_micro_usd = msg.price_micro_usd;
                        od.timestamp = msg.timestamp;
                        od.block_height = h;
                        od.signature_valid = bundle.IsMuSig2()
                            ? bundle_signature_valid
                            : msg.VerifyAttestation();
                        od.has_data = true;
                        od.price_source = "on-chain";
                    }
                }
            }
        }
    }

    // 2. Pending P2P messages (for oracles not yet on-chain)
    {
        std::vector<COraclePriceMessage> pending = bundle_manager.GetPendingMessages();
        for (const auto& msg : pending) {
            // Skip stale pending messages
            if (!IsFreshOracleTimestamp(msg.timestamp, now)) continue;

            if (res.oracle_data.find(msg.oracle_id) == res.oracle_data.end() ||
                !res.oracle_data[msg.oracle_id].has_data) {
                auto& od = res.oracle_data[msg.oracle_id];
                od.price_micro_usd = msg.price_micro_usd;
                od.timestamp = msg.timestamp;
                od.block_height = 0;
                od.signature_valid = msg.VerifyAttestation();
                od.has_data = true;
                od.price_source = "pending";
            }
        }
    }

    // 3. Local runtime oracle nodes (highest priority — override if available)
    {
        const std::vector<OracleNodeInfo>& all_oracles = Params().GetOracleNodes();
        for (const auto& oc : all_oracles) {
            OracleNode* runtime = oracle_manager.GetOracleNode(oc.id);
            if (runtime && runtime->HasValidPrice()) {
                auto& od = res.oracle_data[oc.id];
                int32_t existing_height = od.block_height;
                od.price_micro_usd = runtime->GetCurrentPrice();
                od.timestamp = runtime->GetLastUpdateTime();
                od.block_height = (existing_height > 0) ? existing_height : 0;
                od.signature_valid = true;
                od.has_data = true;
                od.price_source = "local";
            }
        }
    }

    return res;
}

/** Return a consistent status string for an oracle given scan results. */
static std::string GetOracleStatus(const ScannedOracleData& od, uint64_t consensus_price)
{
    if (!od.has_data) return "no_data";
    // Outlier: deviation > 10% from consensus
    if (consensus_price > 0 && od.price_micro_usd > 0) {
        int64_t diff = (int64_t)od.price_micro_usd - (int64_t)consensus_price;
        if (diff < 0) diff = -diff;
        // Use integer math: diff * 100 / consensus_price > 10  →  diff * 10 > consensus_price
        if ((uint64_t)diff * 10 > consensus_price) return "outlier";
    }
    return "reporting";
}
// ---------- End shared oracle helper ----------

static RPCHelpMan getalloracleprices()
{
    return RPCHelpMan{"getalloracleprices",
                "\nGet the individual price reported by each oracle.\n"
                "Scans recent blocks for on-chain oracle bundles and shows each oracle's\n"
                "submitted price, deviation from median, and status. Essential for monitoring\n"
                "oracle health and detecting misbehaving oracles.\n",
                {
                    {"blocks", RPCArg::Type::NUM, RPCArg::Default{20}, "Number of recent blocks to scan (default: 20)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "block_height", "Current block height"},
                        {RPCResult::Type::NUM, "consensus_price_micro_usd", "Current consensus price in micro-USD"},
                        {RPCResult::Type::NUM, "consensus_price_usd", "Current consensus price in USD"},
                        {RPCResult::Type::NUM, "oracle_count", "Number of oracles that submitted prices"},
                        {RPCResult::Type::NUM, "required", "Minimum oracles required for consensus"},
                        {RPCResult::Type::NUM, "total_oracles", "Total configured oracles"},
                        {RPCResult::Type::ARR, "oracles", "Per-oracle price data",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID"},
                                        {RPCResult::Type::STR, "name", "Oracle operator name"},
                                        {RPCResult::Type::STR, "endpoint", "Oracle endpoint"},
                                        {RPCResult::Type::NUM, "price_micro_usd", "Price reported by this oracle (micro-USD)"},
                                        {RPCResult::Type::NUM, "price_usd", "Price reported by this oracle (USD)"},
                                        {RPCResult::Type::NUM, "timestamp", "Timestamp of price submission"},
                                        {RPCResult::Type::NUM, "block_height", "Block height where price was included"},
                                        {RPCResult::Type::NUM, "deviation_pct", "Deviation from consensus median (%)"},
                                        {RPCResult::Type::BOOL, "signature_valid", "Whether Schnorr signature is valid"},
                                        {RPCResult::Type::STR, "price_source", "Where price came from: local/on-chain/pending/none"},
                                        {RPCResult::Type::STR, "status", "Oracle status: reporting/no_data/outlier"},
                                    }
                                }
                            }
                        },
                        {RPCResult::Type::NUM, "last_bundle_height", "Block height of most recent oracle bundle"},
                        {RPCResult::Type::NUM, "last_bundle_time", "Timestamp of most recent oracle bundle"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getalloracleprices", "") +
                    HelpExampleCli("getalloracleprices", "50") +
                    HelpExampleRpc("getalloracleprices", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            const ChainstateManager& chainman = EnsureAnyChainman(request.context);
            const Consensus::Params& consensus = Params().GetConsensus();
            OracleBundleManager& bundle_manager = OracleBundleManager::GetInstance();
            OracleManager& oracle_manager = OracleManager::GetInstance();

            int scan_blocks = request.params.size() > 0 ? request.params[0].getInt<int>() : 20;
            if (scan_blocks < 1) scan_blocks = 1;
            if (scan_blocks > 1000) scan_blocks = 1000;

            int tip_height = chainman.ActiveChain().Height();

            // Use shared scanner (Bug #15: consistent with getoracles)
            OracleScanResult scan = ScanOracleDataFromChain(chainman, bundle_manager, oracle_manager, scan_blocks);

            const std::vector<OracleNodeInfo>& oracle_nodes = Params().GetOracleNodes();

            // Build result
            UniValue result(UniValue::VOBJ);
            result.pushKV("block_height", tip_height);
            result.pushKV("consensus_price_micro_usd", (int64_t)scan.consensus_price);
            result.pushKV("consensus_price_usd", static_cast<double>(scan.consensus_price) / 1000000.0);

            int reporting_count = 0;
            UniValue oracles_arr(UniValue::VARR);

            for (size_t i = 0; i < oracle_nodes.size(); ++i) {
                UniValue oracle_obj(UniValue::VOBJ);
                oracle_obj.pushKV("oracle_id", (int)oracle_nodes[i].id);
                oracle_obj.pushKV("name", OracleDisplayName(oracle_nodes[i].id));
                oracle_obj.pushKV("endpoint", oracle_nodes[i].endpoint);

                auto it = scan.oracle_data.find(oracle_nodes[i].id);
                if (it != scan.oracle_data.end() && it->second.has_data) {
                    const ScannedOracleData& od = it->second;
                    oracle_obj.pushKV("price_micro_usd", (int64_t)od.price_micro_usd);
                    oracle_obj.pushKV("price_usd", static_cast<double>(od.price_micro_usd) / 1000000.0);
                    oracle_obj.pushKV("timestamp", od.timestamp);
                    oracle_obj.pushKV("block_height", od.block_height);

                    // Calculate deviation from consensus
                    double deviation_pct = 0.0;
                    if (scan.consensus_price > 0) {
                        deviation_pct = ((double)od.price_micro_usd - (double)scan.consensus_price) / (double)scan.consensus_price * 100.0;
                    }
                    oracle_obj.pushKV("deviation_pct", deviation_pct);
                    oracle_obj.pushKV("signature_valid", od.signature_valid);
                    oracle_obj.pushKV("price_source", od.price_source.empty() ? "none" : od.price_source);

                    std::string status = GetOracleStatus(od, scan.consensus_price);
                    oracle_obj.pushKV("status", status);
                    if (status == "reporting") reporting_count++;
                } else {
                    oracle_obj.pushKV("price_micro_usd", 0);
                    oracle_obj.pushKV("price_usd", 0.0);
                    oracle_obj.pushKV("timestamp", 0);
                    oracle_obj.pushKV("block_height", 0);
                    oracle_obj.pushKV("deviation_pct", 0.0);
                    oracle_obj.pushKV("signature_valid", false);
                    oracle_obj.pushKV("price_source", "none");
                    oracle_obj.pushKV("status", "no_data");
                }

                oracles_arr.push_back(oracle_obj);
            }

            result.pushKV("oracle_count", reporting_count);
            result.pushKV("required", consensus.nOracleRequiredMessages);
            result.pushKV("total_oracles", (int)oracle_nodes.size());
            result.pushKV("oracles", oracles_arr);
            result.pushKV("last_bundle_height", scan.last_bundle_height);
            result.pushKV("last_bundle_time", scan.last_bundle_time);

            return result;
        },
    };
}


static RPCHelpMan getoraclesigners()
{
    return RPCHelpMan{"getoraclesigners",
                "\nShow which oracles signed recent on-chain MuSig2 oracle bundles.\n"
                "This read-only RPC scans recent active-chain blocks, extracts DigiDollar\n"
                "oracle bundles, decodes each v0x03 participation bitmap, and returns the\n"
                "actual signer IDs plus configured oracle metadata. It does not require a\n"
                "wallet and does not change oracle, wallet, or consensus state.\n",
                {
                    {"blocks", RPCArg::Type::NUM, RPCArg::Default{100}, "Number of recent blocks to scan (clamped to 1..1000)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "chain_height", "Current active-chain height"},
                        {RPCResult::Type::NUM, "scan_blocks", "Number of recent blocks scanned"},
                        {RPCResult::Type::NUM, "required_signers", "Minimum MuSig2 signer count required by consensus"},
                        {RPCResult::Type::NUM, "total_oracle_slots", "Total configured oracle bitmap slots"},
                        {RPCResult::Type::NUM, "active_oracle_slots", "Number of oracle public keys currently active in consensus"},
                        {RPCResult::Type::NUM, "bundle_count", "Number of bundles found in the scan window"},
                        {RPCResult::Type::ARR, "bundles", "Recent on-chain oracle bundles, newest first",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::NUM, "height", "Block height containing the bundle"},
                                        {RPCResult::Type::STR_HEX, "blockhash", "Block hash containing the bundle"},
                                        {RPCResult::Type::NUM, "epoch", "Oracle epoch covered by the bundle"},
                                        {RPCResult::Type::NUM, "version", "Oracle bundle version"},
                                        {RPCResult::Type::NUM, "price_micro_usd", "Consensus DGB/USD price in micro-USD"},
                                        {RPCResult::Type::NUM, "price_usd", "Consensus DGB/USD price in USD"},
                                        {RPCResult::Type::NUM, "timestamp", "Bundle timestamp"},
                                        {RPCResult::Type::STR_HEX, "participation_bitmap", "Raw v0x03 participation bitmap"},
                                        {RPCResult::Type::BOOL, "bitmap_valid", "Whether the bitmap decoded cleanly"},
                                        {RPCResult::Type::NUM, "signer_count", "Number of signer IDs decoded from the bitmap"},
                                        {RPCResult::Type::ARR, "signer_ids", "Decoded signer oracle IDs",
                                            {
                                                {RPCResult::Type::NUM, "", "Oracle ID"},
                                            }
                                        },
                                        {RPCResult::Type::ARR, "signers", "Decoded signer metadata",
                                            {
                                                {RPCResult::Type::OBJ, "", "",
                                                    {
                                                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID"},
                                                        {RPCResult::Type::STR, "name", "Oracle display name"},
                                                        {RPCResult::Type::BOOL, "configured", "Whether this oracle ID exists in chainparams"},
                                                        {RPCResult::Type::BOOL, "in_consensus", "Whether this oracle ID is within the active consensus pubkey set"},
                                                        {RPCResult::Type::BOOL, "is_active", "Whether chainparams marks this oracle node active"},
                                                        {RPCResult::Type::STR_HEX, "pubkey", "Configured compressed oracle public key"},
                                                        {RPCResult::Type::STR, "endpoint", "Configured oracle endpoint"},
                                                    }
                                                }
                                            }
                                        },
                                    }
                                }
                            }
                        },
                    }
                },
                RPCExamples{
                    HelpExampleCli("getoraclesigners", "") +
                    HelpExampleCli("getoraclesigners", "200") +
                    HelpExampleRpc("getoraclesigners", "20")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const ChainstateManager& chainman = EnsureAnyChainman(request.context);
            const CChainParams& params = Params();
            const Consensus::Params& consensus = params.GetConsensus();
            OracleBundleManager& bundle_manager = OracleBundleManager::GetInstance();

            int scan_blocks = OptionalParamIsSet(request, 0) ? request.params[0].getInt<int>() : 100;
            if (scan_blocks < 1) scan_blocks = 1;
            if (scan_blocks > 1000) scan_blocks = 1000;

            const int tip_height = WITH_LOCK(cs_main, return chainman.ActiveChain().Height());
            UniValue bundles(UniValue::VARR);
            int bundle_count = 0;

            {
                LOCK(cs_main);
                const int first_height = std::max(0, tip_height - scan_blocks + 1);
                for (int h = tip_height; h >= first_height; --h) {
                    CBlockIndex* pindex = chainman.ActiveChain()[h];
                    if (!pindex) continue;

                    CBlock block;
                    if (!chainman.m_blockman.ReadBlockFromDisk(block, *pindex)) continue;
                    if (block.vtx.empty()) continue;

                    COracleBundle bundle;
                    if (!bundle_manager.ExtractOracleBundle(*block.vtx[0], bundle)) continue;

                    std::vector<uint8_t> signer_ids;
                    bool bitmap_valid = false;
                    if (bundle.IsMuSig2() && !bundle.participation_bitmap.empty()) {
                        signer_ids = MuSig2OracleAggregator::DecodeBitmap(
                            bundle.participation_bitmap,
                            static_cast<uint16_t>(consensus.nOracleTotalOracles));
                        bitmap_valid = !signer_ids.empty();
                    }

                    UniValue signer_id_array(UniValue::VARR);
                    UniValue signer_array(UniValue::VARR);
                    for (uint8_t signer_id : signer_ids) {
                        signer_id_array.push_back(static_cast<int>(signer_id));

                        const OracleNodeInfo* oracle = params.GetOracleNode(signer_id);
                        UniValue signer(UniValue::VOBJ);
                        signer.pushKV("oracle_id", static_cast<int>(signer_id));
                        signer.pushKV("name", OracleDisplayName(signer_id));
                        signer.pushKV("configured", oracle != nullptr);
                        signer.pushKV("in_consensus", static_cast<int>(signer_id) < consensus.nOraclePubkeyCount);
                        signer.pushKV("is_active", oracle ? oracle->is_active : false);
                        signer.pushKV("pubkey", oracle ? HexStr(oracle->pubkey) : std::string{});
                        signer.pushKV("endpoint", oracle ? oracle->endpoint : std::string{});
                        signer_array.push_back(signer);
                    }

                    UniValue bundle_obj(UniValue::VOBJ);
                    bundle_obj.pushKV("height", h);
                    bundle_obj.pushKV("blockhash", pindex->GetBlockHash().GetHex());
                    bundle_obj.pushKV("epoch", bundle.epoch);
                    bundle_obj.pushKV("version", static_cast<int>(bundle.version));
                    bundle_obj.pushKV("price_micro_usd", static_cast<int64_t>(bundle.median_price_micro_usd));
                    bundle_obj.pushKV("price_usd", static_cast<double>(bundle.median_price_micro_usd) / 1000000.0);
                    bundle_obj.pushKV("timestamp", bundle.timestamp);
                    bundle_obj.pushKV("participation_bitmap", HexStr(bundle.participation_bitmap));
                    bundle_obj.pushKV("bitmap_valid", bitmap_valid);
                    bundle_obj.pushKV("signer_count", static_cast<int>(signer_ids.size()));
                    bundle_obj.pushKV("signer_ids", signer_id_array);
                    bundle_obj.pushKV("signers", signer_array);
                    bundles.push_back(bundle_obj);
                    ++bundle_count;
                }
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("chain_height", tip_height);
            result.pushKV("scan_blocks", scan_blocks);
            result.pushKV("required_signers", consensus.nOracleConsensusRequired);
            result.pushKV("total_oracle_slots", consensus.nOracleTotalOracles);
            result.pushKV("active_oracle_slots", consensus.nOraclePubkeyCount);
            result.pushKV("bundle_count", bundle_count);
            result.pushKV("bundles", bundles);
            return result;
        },
    };
}


// sendoracleprice RPC REMOVED — Security vulnerability.
// Oracle operators must NOT be able to inject arbitrary prices.
// Oracle prices come exclusively from live exchange aggregation.
// See OracleNode::PriceThreadFunc() and MultiExchangeAggregator.

static RPCHelpMan getoracles()
{
    return RPCHelpMan{"getoracles",
                "\nGet all oracle nodes with their config, status, and network-reported prices.\n"
                "Shows what the network sees — prices come from on-chain oracle bundles,\n"
                "not just the local node. Use this for monitoring all oracle health.\n",
                {
                    {"active_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Only show active oracles"},
                    {"blocks", RPCArg::Type::NUM, RPCArg::Default{20}, "Number of recent blocks to scan (default: 20)"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::NUM, "oracle_id", "Oracle ID"},
                                {RPCResult::Type::STR, "name", "Oracle operator name"},
                                {RPCResult::Type::STR_HEX, "pubkey", "Oracle public key"},
                                {RPCResult::Type::STR, "endpoint", "Oracle network endpoint"},
                                {RPCResult::Type::BOOL, "is_active", "Whether oracle is configured as active"},
                                {RPCResult::Type::NUM, "active_oracle_count", "Active MuSig2 oracle key count"},
                                {RPCResult::Type::NUM, "total_oracle_slots", "Total configured oracle slots"},
                                {RPCResult::Type::NUM, "consensus_threshold", "Required MuSig2 oracle signatures"},
                                {RPCResult::Type::BOOL, "in_consensus", "Whether oracle slot is in the active MuSig2 quorum (true if oracle_id < oracle_pubkey_count)."},
                                {RPCResult::Type::NUM, "last_price_micro_usd", "Last reported price in micro-USD"},
                                {RPCResult::Type::NUM, "last_price_usd", "Last reported price in USD"},
                                {RPCResult::Type::NUM, "last_update", "Timestamp of last price"},
                                {RPCResult::Type::STR, "price_source", "Where price came from: local/on-chain/pending/none"},
                                {RPCResult::Type::STR, "status", "Oracle status: reporting/no_data/outlier"},
                                {RPCResult::Type::BOOL, "selected_for_epoch", "Whether oracle is selected for current epoch"},
                                {RPCResult::Type::BOOL, "is_running_locally", "Whether this oracle is running on YOUR node"},
                                {RPCResult::Type::STR, "heartbeat_status", "Latest signed operator-version heartbeat status: fresh/stale/unknown/invalid_signature"},
                                {RPCResult::Type::STR, "software_version", "Software version reported by the oracle heartbeat"},
                                {RPCResult::Type::NUM, "client_version", "Integer client version reported by the oracle heartbeat"},
                                {RPCResult::Type::NUM, "p2p_protocol_version", "P2P protocol version reported by the oracle heartbeat"},
                                {RPCResult::Type::NUM, "oracle_protocol_version", "Oracle off-chain protocol version reported by the heartbeat"},
                                {RPCResult::Type::NUM, "musig2_context_version", "MuSig2 context protocol version reported by the heartbeat"},
                                {RPCResult::Type::NUM, "heartbeat_timestamp", "Unix timestamp of the latest heartbeat"},
                                {RPCResult::Type::NUM, "heartbeat_age_seconds", "Age of the latest heartbeat in seconds, or -1 if unknown"},
                                {RPCResult::Type::BOOL, "heartbeat_signature_valid", "Whether the stored heartbeat signature verifies against chainparams"}
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("getoracles", "") +
                    HelpExampleCli("getoracles", "true") +
                    HelpExampleRpc("getoracles", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            bool activeOnly = OptionalParamIsSet(request, 0) ? request.params[0].get_bool() : false;

            int scan_blocks = OptionalParamIsSet(request, 1) ? request.params[1].getInt<int>() : 20;
            if (scan_blocks < 1) scan_blocks = 1;
            if (scan_blocks > 1000) scan_blocks = 1000;

            const ChainstateManager& chainman = EnsureAnyChainman(request.context);
            const CChainParams& params = Params();
            const std::vector<OracleNodeInfo>& all_oracles = params.GetOracleNodes();
            OracleBundleManager& bundle_manager = OracleBundleManager::GetInstance();
            OracleManager& oracle_manager = OracleManager::GetInstance();

            int32_t current_height = chainman.ActiveChain().Height();
            int32_t current_epoch = GetCurrentEpoch(current_height);
            std::vector<OracleNodeInfo> selected_oracles = SelectOraclesForEpoch(all_oracles, current_epoch);
            std::set<uint32_t> selected_ids;
            for (const auto& oracle : selected_oracles) {
                selected_ids.insert(oracle.id);
            }
            const int64_t now = GetTime();

            // Use shared scanner (Bug #15: consistent with getalloracleprices)
            OracleScanResult scan = ScanOracleDataFromChain(chainman, bundle_manager, oracle_manager, scan_blocks);

            UniValue result(UniValue::VARR);
            for (size_t i = 0; i < all_oracles.size(); ++i) {
                const auto& oc = all_oracles[i];
                if (activeOnly && !oc.is_active) continue;

                bool is_selected = selected_ids.count(oc.id) > 0;
                bool is_running = oracle_manager.IsOracleRunning(oc.id);

                UniValue info(UniValue::VOBJ);
                info.pushKV("oracle_id", static_cast<int>(oc.id));
                info.pushKV("name", OracleDisplayName(oc.id));
                info.pushKV("pubkey", HexStr(oc.pubkey));
                info.pushKV("endpoint", oc.endpoint);
                info.pushKV("is_active", oc.is_active);
                info.pushKV("active_oracle_count", params.GetConsensus().nOraclePubkeyCount);
                info.pushKV("total_oracle_slots", params.GetConsensus().nOracleTotalOracles);
                info.pushKV("consensus_threshold", params.GetConsensus().nOracleConsensusRequired);
                // Wave 9 (Agent C): expose whether this slot can vote in
                // consensus. The MuSig2 aggregator (`ValidateMuSig2Bundle`)
                // rejects signers whose id >= nOraclePubkeyCount, so a
                // slot is in the active quorum iff its id is below that
                // count. Reserve slots (mainnet/testnet 17-34) display as
                // is_active=false and in_consensus=false
                // (cannot sign a v0x03 bundle that consensus accepts).
                const int pubkey_count = params.GetConsensus().nOraclePubkeyCount;
                info.pushKV("in_consensus", static_cast<int>(oc.id) < pubkey_count);

                auto it = scan.oracle_data.find(oc.id);
                if (it != scan.oracle_data.end() && it->second.has_data) {
                    const ScannedOracleData& od = it->second;
                    info.pushKV("last_price_micro_usd", (int64_t)od.price_micro_usd);
                    info.pushKV("last_price_usd", static_cast<double>(od.price_micro_usd) / 1000000.0);
                    info.pushKV("last_update", od.timestamp);
                    info.pushKV("price_source", od.price_source);
                    info.pushKV("status", GetOracleStatus(od, scan.consensus_price));
                } else {
                    info.pushKV("last_price_micro_usd", (int64_t)0);
                    info.pushKV("last_price_usd", 0.0);
                    info.pushKV("last_update", (int64_t)0);
                    info.pushKV("price_source", "none");
                    info.pushKV("status", "no_data");
                }

                info.pushKV("selected_for_epoch", is_selected);
                info.pushKV("is_running_locally", is_running);

                OracleVersionHeartbeatMsg heartbeat;
                const bool has_heartbeat = bundle_manager.GetVersionHeartbeat(oc.id, heartbeat);
                bool heartbeat_signature_valid = false;
                int64_t heartbeat_age = -1;
                std::string heartbeat_status = "unknown";
                if (has_heartbeat) {
                    XOnlyPubKey oracle_pubkey(oc.pubkey);
                    heartbeat_signature_valid = heartbeat.VerifySignature(oracle_pubkey);
                    heartbeat_age = now - heartbeat.timestamp;
                    if (!heartbeat_signature_valid) {
                        heartbeat_status = "invalid_signature";
                    } else if (heartbeat_age >= 0 && heartbeat_age <= 1800) {
                        heartbeat_status = "fresh";
                    } else {
                        heartbeat_status = "stale";
                    }
                }

                info.pushKV("heartbeat_status", heartbeat_status);
                info.pushKV("software_version", has_heartbeat ? heartbeat.software_version : "");
                info.pushKV("client_version", has_heartbeat ? heartbeat.client_version : 0);
                info.pushKV("p2p_protocol_version", has_heartbeat ? heartbeat.p2p_protocol_version : 0);
                info.pushKV("oracle_protocol_version", has_heartbeat ? heartbeat.oracle_protocol_version : 0);
                info.pushKV("musig2_context_version", has_heartbeat ? heartbeat.musig2_context_version : 0);
                info.pushKV("heartbeat_timestamp", has_heartbeat ? heartbeat.timestamp : 0);
                info.pushKV("heartbeat_age_seconds", heartbeat_age);
                info.pushKV("heartbeat_signature_valid", heartbeat_signature_valid);

                result.push_back(info);
            }
            return result;
        },
    };
}

static RPCHelpMan listoracle()
{
    return RPCHelpMan{"listoracle",
                "\nShow the status of the oracle running on this local node.\n"
                "If no oracle is running, returns a message with instructions.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "running", "Whether an oracle is running locally"},
                        {RPCResult::Type::BOOL, "configured", /*optional=*/ true, "Whether an oracle key is configured in the selected wallet"},
                        {RPCResult::Type::NUM, "oracle_id", /*optional=*/ true, "Oracle ID (if running)"},
                        {RPCResult::Type::STR, "name", /*optional=*/ true, "Oracle operator name"},
                        {RPCResult::Type::STR_HEX, "pubkey", /*optional=*/ true, "Oracle public key"},
                        {RPCResult::Type::STR_HEX, "pubkey_xonly", /*optional=*/ true, "Oracle x-only public key"},
                        {RPCResult::Type::BOOL, "authorized", /*optional=*/ true, "Whether the configured key matches chain parameters"},
                        {RPCResult::Type::STR, "wallet_name", /*optional=*/ true, "Wallet containing the configured oracle key"},
                        {RPCResult::Type::NUM, "price_micro_usd", /*optional=*/ true, "Current price being reported"},
                        {RPCResult::Type::NUM, "price_usd", /*optional=*/ true, "Current price in USD"},
                        {RPCResult::Type::STR, "price_source", /*optional=*/ true, "Where price came from: local/on-chain/pending/none"},
                        {RPCResult::Type::NUM, "last_update", /*optional=*/ true, "Last update timestamp"},
                        {RPCResult::Type::STR, "software_version", /*optional=*/ true, "Local node software version"},
                        {RPCResult::Type::NUM, "client_version", /*optional=*/ true, "Local integer client version"},
                        {RPCResult::Type::NUM, "p2p_protocol_version", /*optional=*/ true, "Local P2P protocol version"},
                        {RPCResult::Type::NUM, "oracle_protocol_version", /*optional=*/ true, "Local oracle off-chain protocol version"},
                        {RPCResult::Type::NUM, "musig2_context_version", /*optional=*/ true, "Local MuSig2 context protocol version"},
                        {RPCResult::Type::NUM, "last_heartbeat_time", /*optional=*/ true, "Last local heartbeat broadcast timestamp"},
                        {RPCResult::Type::BOOL, "enabled", /*optional=*/ true, "Whether oracle is enabled"},
                        {RPCResult::Type::STR, "message", /*optional=*/ true, "Status message"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("listoracle", "") +
                    HelpExampleRpc("listoracle", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            OracleManager& oracle_manager = OracleManager::GetInstance();

            UniValue result(UniValue::VOBJ);

            // Check all oracle IDs for a running instance
            const CChainParams& chainparams = Params();
            const std::vector<OracleNodeInfo>& all_oracles = chainparams.GetOracleNodes();
            for (uint32_t id = 0; id < all_oracles.size(); ++id) {
                if (oracle_manager.IsOracleRunning(id)) {
                    OracleNode* node = oracle_manager.GetOracleNode(id);
                    if (!node) continue;

                    result.pushKV("running", true);
                    result.pushKV("oracle_id", (int)id);
                    result.pushKV("name", OracleDisplayName(id));

                    const CChainParams& params = Params();
                    const std::vector<OracleNodeInfo>& oracles = params.GetOracleNodes();
                    bool configured_in_wallet = false;
                    std::string wallet_name;
                    if (id < oracles.size()) {
                        result.pushKV("pubkey", HexStr(oracles[id].pubkey));
#ifdef ENABLE_WALLET
                        WalletOracleKeyLookup lookup = LookupWalletOraclePubKey(request, id);
                        configured_in_wallet = lookup.key_found && lookup.pubkey == oracles[id].pubkey;
                        if (configured_in_wallet) {
                            wallet_name = lookup.wallet_name;
                        }
#endif
                        result.pushKV("authorized", true);
                    }
                    result.pushKV("configured", configured_in_wallet);
                    if (configured_in_wallet) {
                        result.pushKV("wallet_name", wallet_name);
                    }

                    // Price: prefer local runtime, fall back to pending P2P, then on-chain
                    uint64_t price = 0;
                    int64_t update_time = 0;
                    std::string price_source = "none";

                    if (node->HasValidPrice()) {
                        price = node->GetCurrentPrice();
                        update_time = node->GetLastUpdateTime();
                        price_source = "local";
                    } else {
                        // Check pending P2P messages (our own broadcast may be there)
                        OracleBundleManager& bundle_manager = OracleBundleManager::GetInstance();
                        int64_t now = GetTime();
                        std::vector<COraclePriceMessage> pending = bundle_manager.GetPendingMessages();
                        for (const auto& msg : pending) {
                            if (!IsFreshOracleTimestamp(msg.timestamp, now)) continue;
                            if (msg.oracle_id == id) {
                                price = msg.price_micro_usd;
                                update_time = msg.timestamp;
                                price_source = "pending";
                                break;
                            }
                        }

                        // Fall back to on-chain data
                        if (price == 0) {
                            const ChainstateManager& chainman = EnsureAnyChainman(request.context);
                            LOCK(cs_main);
                            int32_t current_height = chainman.ActiveChain().Height();
                            for (int h = current_height; h >= std::max(0, current_height - 19); --h) {
                                CBlockIndex* pindex = chainman.ActiveChain()[h];
                                if (!pindex) continue;
                                CBlock block;
                                if (!chainman.m_blockman.ReadBlockFromDisk(block, *pindex)) continue;
                                if (block.vtx.empty()) continue;
                                COracleBundle bundle;
                                if (bundle_manager.ExtractOracleBundle(*block.vtx[0], bundle)) {
                                    if (!IsFreshOracleTimestamp(bundle.timestamp, now)) continue;
                                    for (const auto& msg : bundle.messages) {
                                        if (!IsFreshOracleTimestamp(msg.timestamp, now)) continue;
                                        if (msg.oracle_id == id) {
                                            price = msg.price_micro_usd;
                                            update_time = msg.timestamp;
                                            price_source = "on-chain";
                                            break;
                                        }
                                    }
                                    if (price > 0) break;
                                }
                            }
                        }
                    }

                    result.pushKV("price_micro_usd", (int64_t)price);
                    result.pushKV("price_usd", static_cast<double>(price) / 1000000.0);
                    result.pushKV("last_update", update_time);
                    result.pushKV("price_source", price_source);
                    result.pushKV("software_version", FormatFullVersion());
                    result.pushKV("client_version", CLIENT_VERSION);
                    result.pushKV("p2p_protocol_version", PROTOCOL_VERSION);
                    result.pushKV("oracle_protocol_version", 1);
                    result.pushKV("musig2_context_version", ORACLE_MUSIG2_SESSION_CONTEXT_VERSION);
                    result.pushKV("last_heartbeat_time", node->GetLastHeartbeatTime());

                    result.pushKV("enabled", node->IsEnabled());
                    result.pushKV("message", "Oracle is running");
                    return result;
                }
            }

#ifdef ENABLE_WALLET
            bool wallet_was_checked = false;
            std::string checked_wallet_name;
            bool wallet_selection_error = false;
            std::string wallet_selection_error_message;
            for (uint32_t id = 0; id < all_oracles.size(); ++id) {
                WalletOracleKeyLookup lookup = LookupWalletOraclePubKey(request, id);
                if (lookup.selection_error) {
                    wallet_selection_error = true;
                    wallet_selection_error_message = lookup.error_message;
                    break;
                }
                if (lookup.wallet_resolved) {
                    wallet_was_checked = true;
                    checked_wallet_name = lookup.wallet_name;
                }
                if (!lookup.key_found) {
                    continue;
                }

                XOnlyPubKey xonly_pubkey(lookup.pubkey);
                result.pushKV("running", false);
                result.pushKV("configured", true);
                result.pushKV("oracle_id", (int)id);
                result.pushKV("name", OracleDisplayName(id));
                result.pushKV("pubkey", HexStr(lookup.pubkey));
                result.pushKV("pubkey_xonly", HexStr(xonly_pubkey));
                result.pushKV("authorized", lookup.pubkey == all_oracles[id].pubkey);
                result.pushKV("wallet_name", lookup.wallet_name);
                result.pushKV("message", strprintf(
                    "Oracle key is configured in wallet '%s' but the oracle is not running. Use 'startoracle %u' to start it.",
                    lookup.wallet_name, id));
                return result;
            }
            if (wallet_was_checked) {
                result.pushKV("running", false);
                result.pushKV("configured", false);
                result.pushKV("wallet_name", checked_wallet_name);
                result.pushKV("message", strprintf(
                    "Selected wallet '%s' has no stored oracle key for oracle ID 0 or any configured oracle ID; no oracle is running.",
                    checked_wallet_name));
                return result;
            }
            if (wallet_selection_error) {
                result.pushKV("running", false);
                result.pushKV("configured", false);
                result.pushKV("message", strprintf(
                    "No oracle is running on this node, and wallet-stored oracle keys could not be checked: %s",
                    wallet_selection_error_message));
                return result;
            }
#endif

            result.pushKV("running", false);
            result.pushKV("configured", false);
            result.pushKV("message", "No oracle is running on this node. Use 'startoracle <id>' to start one.");
            return result;
        },
    };
}

#ifdef ENABLE_WALLET
RPCHelpMan createoraclekey()
{
    return RPCHelpMan{"createoraclekey",
                "\nGenerate an oracle keypair and store it in the loaded descriptor wallet.\n"
                "The private key is stored securely in the wallet database, mapped to the oracle_id.\n"
                "This is local wallet key management and is allowed before DigiDollar activation.\n"
                "It does not start an oracle, sign prices, relay oracle data, or change consensus state.\n"
                "\nTwo public key formats are returned from the same keypair:\n"
                "  - pubkey: 33-byte compressed key (02/03 prefix) — SEND THIS to the maintainer\n"
                "  - pubkey_xonly: 32-byte x-only key (prefix stripped) — used internally for Schnorr signatures\n"
                "\nThe maintainer uses your pubkey to populate both chainparams locations:\n"
                "  - vOracleNodes: uses the full 33-byte compressed key as-is\n"
                "  - consensus.vOraclePublicKeys: uses the 32-byte x-only version (02/03 prefix stripped)\n"
                "\nAs an operator, you only need to share your pubkey. Never share your private key.\n",
                {
                    {"oracle_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "Oracle ID slot (0-34) to generate key for"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID the key was generated for"},
                        {RPCResult::Type::STR_HEX, "pubkey", "Compressed public key (33-byte, 02/03 prefix) — SHARE THIS with the maintainer for chainparams inclusion"},
                        {RPCResult::Type::STR_HEX, "pubkey_xonly", "X-only public key (32-byte, no prefix) — derived from pubkey, used internally for Schnorr signature verification. Do not share separately; the maintainer derives this from pubkey."},
                        {RPCResult::Type::BOOL, "stored_in_wallet", "Whether key was stored in wallet"},
                        {RPCResult::Type::STR, "message", "Instructions for the operator"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("createoraclekey", "5") +
                    HelpExampleRpc("createoraclekey", "5")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Get wallet
            std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "No wallet is loaded. A descriptor wallet is required.");

            if (pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
            }

            // DD-FA-FUNC-028 (Wave 18 Agent C): DD-flavored locked-wallet
            // hint for createoraclekey (matches mintdigidollar /
            // senddigidollar / sendmanydigidollar / getdigidollaraddress /
            // redeemdigidollar). Preserves the legacy "walletpassphrase"
            // substring for backward compatibility with
            // digidollar_encrypted_wallet.py.
            if (pwallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                    "DigiDollar oracle key generation requires the wallet to be unlocked. "
                    "Error: Please enter the wallet passphrase with walletpassphrase first.");
            }

            // Parse and validate oracle_id (DD-FA-FUNC-029).
            //
            // The previous implementation read the parameter into a
            // uint32_t before bounds-checking, so a negative input
            // wrapped to 4294967295 and produced a confusing diagnostic
            // (the rejection still happened, but the error text disagreed
            // with the user input). Mirror startoracle / stoporacle /
            // getoraclepubkey by reading into a signed int and rejecting
            // negatives explicitly.
            int oracle_id_signed = request.params[0].getInt<int>();
            if (oracle_id_signed < 0 || oracle_id_signed >= ORACLE_TOTAL_COUNT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid oracle ID %d. Must be between 0 and %d",
                              oracle_id_signed, ORACLE_TOTAL_COUNT - 1));
            }
            // Reject oracle_ids that have no slot in the active chain
            // params roster. Without this, regtest (which only publishes
            // 7 slots) would let createoraclekey persist an unusable key
            // for unconfigured slots that startoracle later refuses with
            // "Oracle ID N not found in chain parameters". Doing the
            // check up front keeps wallet state consistent with what the
            // rest of the oracle CRUD surface accepts.
            if (Params().GetOracleNode(static_cast<uint32_t>(oracle_id_signed)) == nullptr) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Oracle ID %d not found in chain parameters", oracle_id_signed));
            }
            uint32_t oracle_id = static_cast<uint32_t>(oracle_id_signed);

            // Check if key already exists for this oracle_id
            CKey existing_key;
            if (pwallet->GetOracleKey(oracle_id, existing_key)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Oracle key already exists in wallet for oracle_id %u. "
                              "Use the existing key or remove it first.", oracle_id));
            }

            // Generate new compressed keypair
            CKey key;
            key.MakeNewKey(true); // compressed = true

            CPubKey pubkey = key.GetPubKey();
            assert(pubkey.IsCompressed());
            assert(key.VerifyPubKey(pubkey));

            // Store in wallet
            bool stored = pwallet->StoreOracleKey(oracle_id, key);
            if (!stored) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to store oracle key in wallet database");
            }

            // Get x-only pubkey (32 bytes, strip the 02/03 prefix)
            XOnlyPubKey xonly(pubkey);

            LogPrintf("Oracle: Generated oracle key for oracle_id %u, pubkey=%s\n",
                     oracle_id, HexStr(pubkey));

            UniValue result(UniValue::VOBJ);
            result.pushKV("oracle_id", (int)oracle_id);
            result.pushKV("pubkey", HexStr(pubkey));
            result.pushKV("pubkey_xonly", HexStr(xonly));
            result.pushKV("stored_in_wallet", stored);
            result.pushKV("message", strprintf(
                "Oracle key generated and stored in wallet. "
                "Share ONLY the pubkey (33-byte compressed, starting with 02/03) with the DigiByte Core maintainer for chainparams inclusion. "
                "The pubkey_xonly is derived from it automatically — you do not need to send it separately. "
                "This pre-activation setup does not start oracle operation. "
                "Run 'startoracle %u' only after DigiDollar is active and your key is added to chainparams.",
                oracle_id));

            return result;
        },
    };
}

RPCHelpMan exportoracleprivkey()
{
    return RPCHelpMan{"exportoracleprivkey",
                "\nExport a wallet-stored DigiDollar oracle private key as 32-byte hex.\n"
                "This is local wallet key management and is allowed before DigiDollar activation.\n"
                "The returned private_key is sensitive oracle signing material; store it offline and never share it.\n",
                {
                    {"oracle_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "Oracle ID slot (0-34) to export"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID the key belongs to"},
                        {RPCResult::Type::STR_HEX, "private_key", "Sensitive 32-byte oracle private key hex"},
                        {RPCResult::Type::STR_HEX, "pubkey", "Compressed public key (33-byte, 02/03 prefix)"},
                        {RPCResult::Type::STR_HEX, "pubkey_xonly", "X-only public key (32-byte, no prefix)"},
                        {RPCResult::Type::BOOL, "authorized", "Whether the key currently matches chainparams for this oracle ID"},
                        {RPCResult::Type::STR, "wallet_name", "Wallet the key was exported from"},
                        {RPCResult::Type::STR, "warning", "Secret-handling warning"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("exportoracleprivkey", "5") +
                    HelpExampleRpc("exportoracleprivkey", "5")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_NOT_FOUND, "No wallet is loaded. A descriptor wallet is required.");
            }

            EnsureOracleWalletCanUsePrivateKeys(*pwallet);
            const uint32_t oracle_id = ParseConfiguredOracleId(request, 0);

            if (!pwallet->HasOracleKey(oracle_id)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Wallet '%s' has no stored oracle private key for oracle ID %u",
                              pwallet->GetName(), oracle_id));
            }
            EnsureOracleWalletUnlocked(*pwallet, "export");
            wallet::EnsureWalletIsUnlocked(*pwallet);

            CKey key;
            if (!pwallet->GetOracleKey(oracle_id, key)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Wallet '%s' has a stored oracle key for oracle ID %u, but the private key could not be loaded",
                              pwallet->GetName(), oracle_id));
            }

            const CPubKey pubkey = key.GetPubKey();
            const XOnlyPubKey xonly(pubkey);
            const OracleNodeInfo* oracle_config = Params().GetOracleNode(oracle_id);

            UniValue result(UniValue::VOBJ);
            result.pushKV("oracle_id", static_cast<int>(oracle_id));
            result.pushKV("private_key", HexStr(Span<const unsigned char>(key.begin(), key.end())));
            result.pushKV("pubkey", HexStr(pubkey));
            result.pushKV("pubkey_xonly", HexStr(xonly));
            result.pushKV("authorized", oracle_config && pubkey == oracle_config->pubkey);
            result.pushKV("wallet_name", pwallet->GetName());
            result.pushKV("warning", "This is sensitive oracle signing key material. Store it offline and never share it.");
            return result;
        },
    };
}

RPCHelpMan importoracleprivkey()
{
    return RPCHelpMan{"importoracleprivkey",
                "\nImport a DigiDollar oracle private key into the loaded wallet.\n"
                "This stores the key for later startoracle use, but does not start an oracle, sign prices, or relay data.\n"
                "The private key must be the 32-byte hex value returned by exportoracleprivkey.\n",
                {
                    {"oracle_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "Oracle ID slot (0-34) to import for"},
                    {"private_key", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Sensitive 32-byte oracle private key hex"},
                    {"replace", RPCArg::Type::BOOL, RPCArg::Default{false}, "Replace an existing wallet-stored oracle key for this oracle_id"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID the key was imported for"},
                        {RPCResult::Type::STR_HEX, "pubkey", "Compressed public key (33-byte, 02/03 prefix)"},
                        {RPCResult::Type::STR_HEX, "pubkey_xonly", "X-only public key (32-byte, no prefix)"},
                        {RPCResult::Type::BOOL, "stored_in_wallet", "Whether key was stored in wallet"},
                        {RPCResult::Type::BOOL, "replaced", "Whether an existing key was replaced"},
                        {RPCResult::Type::BOOL, "authorized", "Whether the key currently matches chainparams for this oracle ID"},
                        {RPCResult::Type::STR, "wallet_name", "Wallet the key was imported into"},
                        {RPCResult::Type::STR, "message", "Status message"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("importoracleprivkey", "5 \"001122...\"") +
                    HelpExampleRpc("importoracleprivkey", "5, \"001122...\", true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) {
                throw JSONRPCError(RPC_WALLET_NOT_FOUND, "No wallet is loaded. A descriptor wallet is required.");
            }

            EnsureOracleWalletCanUsePrivateKeys(*pwallet);
            const uint32_t oracle_id = ParseConfiguredOracleId(request, 0);
            const bool replace = OptionalParamIsSet(request, 2) ? request.params[2].get_bool() : false;

            CKey key;
            std::string parse_error;
            if (!TryParseOraclePrivateKey(request.params[1].get_str(), key, parse_error)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                    strprintf("Oracle private key is invalid: %s", parse_error));
            }

            const bool had_existing_key = pwallet->HasOracleKey(oracle_id);
            if (had_existing_key && !replace) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Wallet '%s' already has an oracle key for oracle ID %u; pass replace=true to overwrite it",
                              pwallet->GetName(), oracle_id));
            }

            EnsureOracleWalletUnlocked(*pwallet, "import");
            wallet::EnsureWalletIsUnlocked(*pwallet);

            if (!pwallet->StoreOracleKey(oracle_id, key)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to store oracle key in wallet database");
            }

            const CPubKey pubkey = key.GetPubKey();
            const XOnlyPubKey xonly(pubkey);
            const OracleNodeInfo* oracle_config = Params().GetOracleNode(oracle_id);
            const bool authorized = oracle_config && pubkey == oracle_config->pubkey;

            UniValue result(UniValue::VOBJ);
            result.pushKV("oracle_id", static_cast<int>(oracle_id));
            result.pushKV("pubkey", HexStr(pubkey));
            result.pushKV("pubkey_xonly", HexStr(xonly));
            result.pushKV("stored_in_wallet", true);
            result.pushKV("replaced", had_existing_key);
            result.pushKV("authorized", authorized);
            result.pushKV("wallet_name", pwallet->GetName());
            result.pushKV("message", authorized
                ? "Oracle key imported and matches the current chainparams slot."
                : "Oracle key imported. It is stored for this slot but does not currently match chainparams; startoracle will not run until the public key is authorized for this oracle ID.");
            return result;
        },
    };
}

RPCHelpMan startoracle()
{
    return RPCHelpMan{"startoracle",
                "\nStart a local oracle node if configured.\n"
                "Requires oracle private key to be configured for this node.\n",
                {
                    {"oracle_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "Oracle ID to start (0-34)"},
                    {"private_key", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Legacy emergency recovery only. Prefer the key stored by createoraclekey; passing secrets via CLI can expose them to shell history and process listings."}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "success", "Whether oracle was started successfully"},
                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID that was started"},
                        {RPCResult::Type::STR, "status", "Oracle status after start attempt"},
                        {RPCResult::Type::STR, "message", "Status message or error description"},
                        {RPCResult::Type::BOOL, "was_already_running", "Whether oracle was already running"},
                        {RPCResult::Type::BOOL, "initialized", "Whether an oracle instance/key was initialized on this node"},
                        {RPCResult::Type::STR, "warning", /*optional=*/ true, "Any warnings about the operation"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("startoracle", "5") +
                    HelpExampleRpc("startoracle", "5")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<wallet::CWallet> request_wallet;
            // Check DigiDollar activation via wallet's chain interface
            // (startoracle is registered in wallet RPC table, so request.context is WalletContext)
            {
                request_wallet = wallet::GetWalletForJSONRPCRequest(request);
                if (!request_wallet) {
                    throw JSONRPCError(RPC_WALLET_NOT_FOUND,
                        "No wallet is loaded. Load a wallet or request startoracle through /wallet/<wallet_name>.");
                }
                if (request_wallet) {
                    node::NodeContext* node_ctx = request_wallet->chain().context();
                    if (node_ctx) {
                        ChainstateManager& chainman = *node_ctx->chainman;
                        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                        if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                            throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                        }
                    }
                }
            }

            int oracle_id = request.params[0].getInt<int>();
            std::string private_key_hex = OptionalParamIsSet(request, 1) ? request.params[1].get_str() : "";

            // Validate oracle ID
            if (oracle_id < 0 || oracle_id >= ORACLE_TOTAL_COUNT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid oracle ID %d. Must be between 0 and %d", oracle_id, ORACLE_TOTAL_COUNT - 1));
            }

            // Verify oracle exists in chainparams
            const CChainParams& params = Params();
            const OracleNodeInfo* oracle_config = params.GetOracleNode(oracle_id);
            if (!oracle_config) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Oracle ID %d not found in chain parameters", oracle_id));
            }

            OracleManager& oracle_manager = OracleManager::GetInstance();
            bool was_already_running = oracle_manager.IsOracleRunning(oracle_id);
            bool success = false;
            bool initialized = was_already_running;
            std::string status_message;
            std::string warning;

            if (!was_already_running && private_key_hex.empty()) {
                if (request_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
                }
                if (request_wallet->IsLocked()) {
                    throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                        "DigiDollar oracle start requires the wallet to be unlocked. "
                        "Error: Please enter the wallet passphrase with walletpassphrase first.");
                }
            }

            try {
                if (was_already_running) {
                    success = true;
                    status_message = "Oracle was already running";
                } else {
                    // Try to start oracle
                        if (!private_key_hex.empty()) {
                            success = TryStartOracleFromPrivateKey(oracle_manager, oracle_id, private_key_hex, "provided private key", /*allow_initialized_without_running=*/false, status_message, &initialized);
                        } else {
                            // Try to start existing oracle (if already configured)
                            OracleNode* existing_oracle = oracle_manager.GetOracleNode(oracle_id);
                            if (existing_oracle) {
                                initialized = true;
                                if (!existing_oracle->ValidateOracleKey()) {
                                    success = false;
                                    status_message = strprintf(
                                        "Existing oracle key is not authorized for oracle ID %d: public key mismatch with chain parameters",
                                        oracle_id);
                                } else if (Params().GetChainType() != ChainType::TESTNET) {
                                    existing_oracle->Start();
                                    success = existing_oracle->IsRunning();
                                    if (success) {
                                        status_message = "Existing oracle started";
                                    } else {
                                        status_message = "Oracle initialized (price fetcher is not running)";
                                    }
                                } else {
                                    existing_oracle->Start();
                                    success = existing_oracle->IsRunning();
                                    status_message = success ? "Existing oracle started" : "Failed to start existing oracle";
                                }
                            } else {
                            // Try to load oracle key from wallet
                            if (!request_wallet->HasOracleKey(oracle_id)) {
                                status_message = strprintf(
                                    "selected wallet '%s' has no stored oracle key for oracle ID %d",
                                    request_wallet->GetName(), oracle_id);
                                warning = "Start failed because the selected wallet has no stored oracle key for the requested slot";
                            } else if (request_wallet->IsLocked()) {
                                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                    "DigiDollar oracle start requires the wallet to be unlocked. "
                                    "Error: Please enter the wallet passphrase with walletpassphrase first.");
                            } else {
                                wallet::EnsureWalletIsUnlocked(*request_wallet);

                                CPubKey wallet_pubkey;
                                if (!request_wallet->GetOraclePubKey(oracle_id, wallet_pubkey)) {
                                    status_message = strprintf(
                                        "selected wallet '%s' has a stored oracle key for oracle ID %d, but its public key could not be read",
                                        request_wallet->GetName(), oracle_id);
                                    warning = "Start failed because the selected wallet's stored oracle key could not be decoded";
                                } else {
                                    std::string authorization_error;
                                    if (!OraclePubKeyMatchesChainparams(oracle_id, wallet_pubkey, authorization_error)) {
                                        status_message = strprintf("%s from selected wallet '%s'", authorization_error, request_wallet->GetName());
                                    } else {
                                        CKey wallet_key;
                                        if (!request_wallet->GetOracleKey(oracle_id, wallet_key)) {
                                            status_message = strprintf(
                                                "selected wallet '%s' has a stored oracle key for oracle ID %d, but the private key could not be loaded",
                                                request_wallet->GetName(), oracle_id);
                                            warning = "Start failed because the selected wallet's oracle private key could not be loaded";
                                        } else {
                                            const std::string wallet_key_hex = HexStr(Span<const unsigned char>(wallet_key.begin(), wallet_key.end()));
                                            const std::string key_source = strprintf("key loaded from wallet '%s'", request_wallet->GetName());
                                            bool wallet_initialized = false;
                                            success = TryStartOracleFromPrivateKey(oracle_manager, oracle_id, wallet_key_hex, key_source, /*allow_initialized_without_running=*/true, status_message, &wallet_initialized);
                                            initialized = initialized || wallet_initialized;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                status_message = strprintf("Exception starting oracle: %s", e.what());
                success = false;
            }

            std::string final_status = "stopped";
            if (oracle_manager.IsOracleRunning(oracle_id)) {
                OracleNode* oracle = oracle_manager.GetOracleNode(oracle_id);
                final_status = oracle && oracle->IsEnabled() ? "running" : "disabled";
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("success", success);
            result.pushKV("oracle_id", oracle_id);
            result.pushKV("status", final_status);
            result.pushKV("message", status_message);
            result.pushKV("was_already_running", was_already_running);
            result.pushKV("initialized", initialized);
            if (!warning.empty()) {
                result.pushKV("warning", warning);
            }

            return result;
        },
    };
}
#endif

static RPCHelpMan stoporacle()
{
    return RPCHelpMan{"stoporacle",
                "\nStop a running oracle node.\n"
                "Stops the oracle daemon and price fetching for the specified oracle.\n",
                {
                    {"oracle_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "Oracle ID to stop (0-34)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "success", "Whether oracle was stopped successfully"},
                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID that was stopped"},
                        {RPCResult::Type::STR, "status", "Oracle status after stop attempt"},
                        {RPCResult::Type::STR, "message", "Status message"},
                        {RPCResult::Type::BOOL, "was_running", "Whether oracle was running before stop"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("stoporacle", "5") +
                    HelpExampleRpc("stoporacle", "5")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            int oracle_id = request.params[0].getInt<int>();

            // Validate oracle ID
            if (oracle_id < 0 || oracle_id >= ORACLE_TOTAL_COUNT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid oracle ID %d. Must be between 0 and %d", oracle_id, ORACLE_TOTAL_COUNT - 1));
            }
            if (Params().GetOracleNode(static_cast<uint32_t>(oracle_id)) == nullptr) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Oracle ID %d not found in chain parameters", oracle_id));
            }

            OracleManager& oracle_manager = OracleManager::GetInstance();
            bool was_running = oracle_manager.IsOracleRunning(oracle_id);
            bool success = false;
            std::string status_message;

            if (!was_running) {
                success = true;
                status_message = "Oracle was not running";
            } else {
                try {
                    OracleNode* oracle = oracle_manager.GetOracleNode(oracle_id);
                    if (oracle) {
                        oracle->Stop();
                        success = !oracle->IsRunning();
                        status_message = success ? "Oracle stopped successfully" : "Failed to stop oracle";

                        // Clear stale oracle messaging state to break potential deadlocks.
                        // The seen_message_hashes, pending_messages, and pending_attestations
                        // can hold stale entries that prevent consensus recovery after restart.
                        // ClearPendingMessages() resets the duplicate filter, allowing fresh
                        // messages to be accepted when the oracle is restarted.
                        if (success) {
                            OracleBundleManager& bundleManager = OracleBundleManager::GetInstance();
                            bundleManager.ClearPendingMessages();
                            status_message += " (messaging state cleared)";
                        }
                    } else {
                        status_message = "Oracle not found in manager";
                    }
                }
                catch (const std::exception& e) {
                    status_message = strprintf("Exception stopping oracle: %s", e.what());
                    success = false;
                }
            }

            std::string final_status = oracle_manager.IsOracleRunning(oracle_id) ? "running" : "stopped";

            UniValue result(UniValue::VOBJ);
            result.pushKV("success", success);
            result.pushKV("oracle_id", oracle_id);
            result.pushKV("status", final_status);
            result.pushKV("message", status_message);
            result.pushKV("was_running", was_running);

            return result;
        },
    };
}

static RPCHelpMan getoraclepubkey()
{
    return RPCHelpMan{"getoraclepubkey",
                "\nGet the oracle node's public key for verification.\n"
                "Returns the XOnlyPubKey (32-byte Schnorr public key) used for signing oracle price messages.\n",
                {
                    {"oracle_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "Oracle ID to query (0-34)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "oracle_id", "Oracle ID"},
                        {RPCResult::Type::STR_HEX, "pubkey", "Oracle public key (XOnlyPubKey, 32 bytes hex)"},
                        {RPCResult::Type::STR_HEX, "pubkey_xonly", /*optional=*/ true, "Oracle public key (XOnlyPubKey, 32 bytes hex; explicit alias for pubkey)"},
                        {RPCResult::Type::STR_HEX, "pubkey_full", "Full compressed public key (CPubKey, 33 bytes hex)"},
                        {RPCResult::Type::BOOL, "valid", "Whether the public key is valid"},
                        {RPCResult::Type::BOOL, "authorized", "Whether key is authorized in consensus parameters"},
                        {RPCResult::Type::BOOL, "is_running", "Whether oracle node is currently running"},
                        {RPCResult::Type::BOOL, "configured_in_wallet", /*optional=*/ true, "Whether the public key was found in the selected wallet"},
                        {RPCResult::Type::STR, "source", /*optional=*/ true, "Where the public key was found"},
                        {RPCResult::Type::STR, "wallet_name", /*optional=*/ true, "Wallet containing the configured oracle key"},
                        {RPCResult::Type::STR, "message", /*optional=*/ true, "Status message"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getoraclepubkey", "0") +
                    HelpExampleRpc("getoraclepubkey", "0")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            int oracle_id = request.params[0].getInt<int>();

            // Validate oracle ID
            if (oracle_id < 0 || oracle_id >= ORACLE_TOTAL_COUNT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid oracle ID %d. Must be between 0 and %d", oracle_id, ORACLE_TOTAL_COUNT - 1));
            }
            if (Params().GetOracleNode(static_cast<uint32_t>(oracle_id)) == nullptr) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Oracle ID %d not found in chain parameters", oracle_id));
            }

            // Get oracle manager
            OracleManager& oracle_manager = OracleManager::GetInstance();
            OracleNode* oracle = oracle_manager.GetOracleNode(oracle_id);

            if (!oracle) {
#ifdef ENABLE_WALLET
                WalletOracleKeyLookup lookup = LookupWalletOraclePubKey(request, static_cast<uint32_t>(oracle_id));
                if (lookup.key_found) {
                    UniValue result(UniValue::VOBJ);
                    PushWalletOraclePubKeyResult(result, static_cast<uint32_t>(oracle_id), lookup.pubkey, lookup.wallet_name, /*is_running=*/false);
                    return result;
                }
                if (lookup.selection_error) {
                    throw JSONRPCError(lookup.error_code, lookup.error_message);
                }
                if (lookup.wallet_resolved) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                        strprintf("selected wallet '%s' has no stored oracle key for oracle ID %d",
                                  lookup.wallet_name, oracle_id));
                }
#endif
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Oracle %d is not running and no selected wallet oracle key could be checked. Request this RPC through /wallet/<wallet_name> to inspect wallet-stored oracle keys.", oracle_id));
            }

            // Get public keys
            XOnlyPubKey xonly_pubkey = oracle->GetOraclePublicKey();
            CPubKey full_pubkey = oracle->GetPublicKey();
            bool is_authorized = oracle->ValidateOracleKey();
            bool is_running = oracle->IsRunning();
            bool configured_in_wallet = false;
            std::string wallet_name;
#ifdef ENABLE_WALLET
            WalletOracleKeyLookup lookup = LookupWalletOraclePubKey(request, static_cast<uint32_t>(oracle_id));
            configured_in_wallet = lookup.key_found && lookup.pubkey == full_pubkey;
            if (configured_in_wallet) {
                wallet_name = lookup.wallet_name;
            }
#endif

            UniValue result(UniValue::VOBJ);
            result.pushKV("oracle_id", oracle_id);
            result.pushKV("pubkey", HexStr(xonly_pubkey));
            result.pushKV("pubkey_xonly", HexStr(xonly_pubkey));
            result.pushKV("pubkey_full", HexStr(full_pubkey));
            result.pushKV("valid", xonly_pubkey.IsFullyValid());
            result.pushKV("authorized", is_authorized);
            result.pushKV("is_running", is_running);
            result.pushKV("configured_in_wallet", configured_in_wallet);
            result.pushKV("source", "running_oracle");
            if (configured_in_wallet) {
                result.pushKV("wallet_name", wallet_name);
            }

            return result;
        },
    };
}

// =============================================================================
// Mock Oracle RPC Commands (RegTest only)
// =============================================================================

static RPCHelpMan setmockoracleprice()
{
    return RPCHelpMan{"setmockoracleprice",
                "\nSet mock oracle price for testing (RegTest only).\n"
                "This command allows setting a custom DGB/USD price for testing DigiDollar\n"
                "functionality in RegTest mode without requiring real oracle nodes.\n"
                "Price is specified in micro-USD (1,000,000 = $1.00) for sub-cent precision.\n",
                {
                    {"price", RPCArg::Type::NUM, RPCArg::Optional::NO, "Price in micro-USD per DGB (e.g., 6500 = $0.0065/DGB, 1000000 = $1.00/DGB)", RPCArgOptions{.skip_type_check = true}}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "price_micro_usd", "New mock oracle price in micro-USD per DGB"},
                        {RPCResult::Type::STR, "price_usd", "Price formatted as USD per DGB"},
                        {RPCResult::Type::NUM, "update_height", "Block height of update"},
                        {RPCResult::Type::BOOL, "enabled", "Whether mock oracle is enabled"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("setmockoracleprice", "6500") +
                    "\nSet price to $0.0065 per DGB (realistic DGB price)\n" +
                    HelpExampleCli("setmockoracleprice", "1000000") +
                    "\nSet price to $1.00 per DGB\n" +
                    HelpExampleRpc("setmockoracleprice", "6500")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            int currentHeight = 0;

            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
                currentHeight = tip ? tip->nHeight : 0;
            }
            // Only allow in RegTest mode
            if (Params().GetChainType() != ChainType::REGTEST) {
                throw JSONRPCError(RPC_METHOD_NOT_FOUND,
                    "setmockoracleprice is only available in RegTest mode");
            }

            CAmount price_micro_usd = request.params[0].getInt<int64_t>();

            if (price_micro_usd <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    "Price must be positive");
            }

            // Price should be reasonable (between $0.0001 and $1000 per DGB in micro-USD)
            const CAmount MIN_PRICE = 100;              // 100 micro-USD = $0.0001 per DGB
            const CAmount MAX_PRICE = 1000000000;       // 1,000,000,000 micro-USD = $1000 per DGB

            if (price_micro_usd < MIN_PRICE || price_micro_usd > MAX_PRICE) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Price must be between %lld and %lld micro-USD per DGB", MIN_PRICE, MAX_PRICE));
            }

            // Set the mock price (mock oracle now accepts micro-USD directly)
            MockOracleManager::GetInstance().SetMockPrice(price_micro_usd, currentHeight);
            PublishRegtestMockMuSig2Quote(currentHeight + 1);

            // Build result
            UniValue result(UniValue::VOBJ);
            result.pushKV("price_micro_usd", price_micro_usd);
            // Format as dollars (divide micro-USD by 1,000,000)
            result.pushKV("price_usd", strprintf("$%.6f", price_micro_usd / 1000000.0));
            result.pushKV("update_height", MockOracleManager::GetInstance().GetLastUpdateHeight());
            result.pushKV("enabled", MockOracleManager::GetInstance().IsEnabled());

            return result;
        },
    };
}

static RPCHelpMan getmockoracleprice()
{
    return RPCHelpMan{"getmockoracleprice",
                "\nGet current mock oracle price (RegTest only).\n"
                "Returns the current mock oracle price used for testing DigiDollar\n"
                "functionality in RegTest mode.\n"
                "Price is in micro-USD (1,000,000 = $1.00) for sub-cent precision.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "price_micro_usd", "Current mock oracle price in micro-USD per DGB"},
                        {RPCResult::Type::STR, "price_usd", "Price formatted as USD per DGB"},
                        {RPCResult::Type::NUM, "last_update_height", "Block height of last update"},
                        {RPCResult::Type::BOOL, "enabled", "Whether mock oracle is enabled"},
                        {RPCResult::Type::NUM, "current_height", "Current blockchain height"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getmockoracleprice", "") +
                    HelpExampleRpc("getmockoracleprice", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            // Only allow in RegTest mode
            if (Params().GetChainType() != ChainType::REGTEST) {
                throw JSONRPCError(RPC_METHOD_NOT_FOUND,
                    "getmockoracleprice is only available in RegTest mode");
            }

            CAmount price_micro_usd = MockOracleManager::GetInstance().GetCurrentPrice();
            int64_t lastHeight = MockOracleManager::GetInstance().GetLastUpdateHeight();
            bool enabled = MockOracleManager::GetInstance().IsEnabled();

            // Get current height from chain state (optional for mock oracle)
            int currentHeight = 0;
            try {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                if (node.chainman) {
                    LOCK(node.chainman->GetMutex());
                    currentHeight = node.chainman->ActiveHeight();
                }
            } catch (...) {
                // Height tracking is optional for mock oracle
                currentHeight = 0;
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("price_micro_usd", price_micro_usd);
            // Format as dollars (divide micro-USD by 1,000,000)
            result.pushKV("price_usd", strprintf("$%.6f", price_micro_usd / 1000000.0));
            result.pushKV("last_update_height", lastHeight);
            result.pushKV("enabled", enabled);
            result.pushKV("current_height", currentHeight);

            return result;
        },
    };
}

static RPCHelpMan simulatepricevolatility()
{
    return RPCHelpMan{"simulatepricevolatility",
                "\nSimulate price volatility for testing (RegTest only).\n"
                "Adjusts the mock oracle price by a given percentage to test\n"
                "DigiDollar system responses to price changes.\n",
                {
                    {"percent_change", RPCArg::Type::NUM, RPCArg::Optional::NO, "Percentage change (positive or negative, e.g., 50 for +50%, -20 for -20%)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "old_price", "Previous mock oracle price"},
                        {RPCResult::Type::NUM, "new_price", "New mock oracle price after volatility"},
                        {RPCResult::Type::NUM, "percent_change", "Percentage change applied"},
                        {RPCResult::Type::NUM, "update_height", "Block height of update"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("simulatepricevolatility", "50") +
                    "\nIncrease price by 50%\n" +
                    HelpExampleCli("simulatepricevolatility", "-80") +
                    "\nDecrease price by 80%\n" +
                    HelpExampleRpc("simulatepricevolatility", "50")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            int currentHeight = 0;

            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
                currentHeight = tip ? tip->nHeight : 0;
            }
            // Only allow in RegTest mode
            if (Params().GetChainType() != ChainType::REGTEST) {
                throw JSONRPCError(RPC_METHOD_NOT_FOUND,
                    "simulatepricevolatility is only available in RegTest mode");
            }

            int percentChange = request.params[0].getInt<int>();

            if (percentChange < -100 || percentChange > 1000) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    "Percentage change must be between -100 and 1000");
            }

            CAmount oldPrice = MockOracleManager::GetInstance().GetCurrentPrice();
            MockOracleManager::GetInstance().SimulateVolatility(percentChange, currentHeight);
            CAmount newPrice = MockOracleManager::GetInstance().GetCurrentPrice();
            PublishRegtestMockMuSig2Quote(currentHeight + 1);

            UniValue result(UniValue::VOBJ);
            result.pushKV("old_price", oldPrice);
            result.pushKV("new_price", newPrice);
            result.pushKV("percent_change", percentChange);
            result.pushKV("update_height", MockOracleManager::GetInstance().GetLastUpdateHeight());

            return result;
        },
    };
}

static RPCHelpMan enablemockoracle()
{
    return RPCHelpMan{"enablemockoracle",
                "\nEnable or disable mock oracle (RegTest only).\n"
                "Controls whether the mock oracle is active for testing.\n",
                {
                    {"enable", RPCArg::Type::BOOL, RPCArg::Optional::NO, "true to enable, false to disable"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "enabled", "New enabled status"},
                        {RPCResult::Type::NUM, "current_price", "Current mock oracle price"},
                        {RPCResult::Type::NUM, "current_height", "Current blockchain height"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("enablemockoracle", "true") +
                    HelpExampleCli("enablemockoracle", "false") +
                    HelpExampleRpc("enablemockoracle", "true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            // Check DigiDollar activation
            {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                ChainstateManager& chainman = EnsureChainman(node);
                const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
                if (!DigiDollar::IsDigiDollarEnabled(tip, chainman)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar is not yet active on this blockchain");
                }
            }
            // Only allow in RegTest mode
            if (Params().GetChainType() != ChainType::REGTEST) {
                throw JSONRPCError(RPC_METHOD_NOT_FOUND,
                    "enablemockoracle is only available in RegTest mode");
            }

            bool enable = request.params[0].get_bool();
            MockOracleManager::GetInstance().SetEnabled(enable);

            // Get current height from chain state (optional for mock oracle)
            int currentHeight = 0;
            try {
                const node::NodeContext& node = EnsureAnyNodeContext(request.context);
                if (node.chainman) {
                    LOCK(node.chainman->GetMutex());
                    currentHeight = node.chainman->ActiveHeight();
                }
            } catch (...) {
                // Height tracking is optional for mock oracle
                currentHeight = 0;
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("enabled", MockOracleManager::GetInstance().IsEnabled());
            result.pushKV("current_price", MockOracleManager::GetInstance().GetCurrentPrice());
            result.pushKV("current_height", currentHeight);

            return result;
        },
    };
}

void RegisterDigiDollarRPCCommands(CRPCTable &t)
{
    static const CRPCCommand commands[] = {
        // System monitoring commands
        {"digidollar", &getdigidollarstats},
        {"digidollar", &getdcamultiplier},
        {"digidollar", &calculatecollateralrequirement},
        {"digidollar", &getdigidollardeploymentinfo},

        // Core transaction commands (moved to wallet RPC table)
        // {"digidollar", &mintdigidollar},
        // {"digidollar", &senddigidollar},
        // {"digidollar", &redeemdigidollar},
        // {"digidollar", &listdigidollarpositions},

        // Address management commands
        // {"digidollar", &getdigidollaraddress},  // Moved to wallet RPC commands for proper wallet context
        // {"digidollar", &validateddaddress},  // Moved to wallet RPC table (Bug #17)
        // {"digidollar", &listdigidollaraddresses},  // Moved to wallet RPC commands for proper wallet context (Bug #12)
        {"digidollar", &importdigidollaraddress},

        // Utility commands (moved to wallet RPC table)
        // {"digidollar", &getdigidollarbalance},
        {"digidollar", &estimatecollateral},
        // {"digidollar", &getredemptioninfo},  // Moved to wallet RPC table for proper wallet context

        // {"digidollar", &listdigidollartxs},
        {"digidollar", &getoracleprice},
        {"oracle", &getalloracleprices},
        {"digidollar", &getprotectionstatus},

        // Oracle management commands
        // sendoracleprice REMOVED — security vulnerability (fake price injection)
        {"oracle", &getoracles},
        {"oracle", &getoraclesigners},
        {"oracle", &listoracle},
        // {"oracle", &startoracle},  // Moved to wallet RPC table for wallet key loading
        {"oracle", &stoporacle},
        {"oracle", &getoraclepubkey},

        // Mock Oracle commands (RegTest only)
        {"digidollar", &setmockoracleprice},
        {"digidollar", &getmockoracleprice},
        {"digidollar", &simulatepricevolatility},
        {"digidollar", &enablemockoracle}
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }

    // DigiDollar wallet-based transaction commands are registered via
    // GetWalletRPCCommands() in wallet/rpc/wallet.cpp for proper wallet context.
    // Do NOT register them here - they won't have wallet context and will fail
    // with "Wallet context not found".
}
