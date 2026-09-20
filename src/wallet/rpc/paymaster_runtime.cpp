// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider runtime start, stop, and automatic service scheduling. */

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
using namespace DigiDollar::Paymaster;
using namespace paymaster_rpc::internal;

RPCHelpMan startpaymaster()
{
    return RPCHelpMan{
        "startpaymaster",
        "Start normal Paymaster provider operation after all readiness gates pass. "
        "When safety limits are exhausted, a non-announcing drain-only runtime "
        "is started only if durable quotes or signatures still require exact completion.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider runtime result", {
                                                                           {RPCResult::Type::BOOL, "running", "Whether the provider is now online"},
                                                                           {RPCResult::Type::BOOL, "ready", "Whether every readiness gate passed"},
                                                                           {RPCResult::Type::BOOL, "drain_only", "Whether only durable existing work may be completed"},
                                                                           {RPCResult::Type::STR, "operation_mode", "automatic or manual"},
                                                                           {RPCResult::Type::BOOL, "autostart", "Whether optional provider autostart is enabled"},
                                                                           {RPCResult::Type::STR, "service_state", "Effective provider service state"},
                                                                           {RPCResult::Type::OBJ, "safety_policy", /*optional=*/true, "Effective finite provider budgets", ProviderSafetyPolicyResults()},
                                                                           {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                           {RPCResult::Type::STR, "endpoint", /*optional=*/true, "Published provider endpoint"},
                                                                           {RPCResult::Type::NUM, "announcement_sequence", /*optional=*/true, "Published monotonic announcement sequence"},
                                                                           {RPCResult::Type::NUM, "recovery_candidates", "Durable provider commits and client finals considered for exact recovery"},
                                                                           {RPCResult::Type::NUM, "recovered_broadcasts", "Exact transactions reconciled or broadcast idempotently"},
                                                                           {RPCResult::Type::NUM, "expired_commits", "Deprecated compatibility field; durable signatures are never released merely because a local retry time elapsed"},
                                                                           {RPCResult::Type::ARR, "recovery_errors", "Stable per-transaction recovery failures", {{RPCResult::Type::STR, "", "Recovery failure"}}},
                                                                           {RPCResult::Type::ARR, "readiness_errors", "Stable reasons preventing start", {{RPCResult::Type::STR, "", "Readiness error"}}},
                                                                       }},
        RPCExamples{HelpExampleCli("startpaymaster", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const bool automatic_autostart =
                request.strMethod == INTERNAL_PAYMASTER_AUTOSTART_METHOD;
            if (automatic_autostart) {
                if (!AutomaticProviderStateIsSynchronized(*wallet)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_SYNCING");
                }
            } else {
                wallet->BlockUntilSyncedToCurrentChain();
            }
            const int64_t now = GetTime();
            const DurablePaymasterRecoveryReport recovery =
                RecoverDurablePaymasterCommits(*wallet, now);
            UniValue recovery_errors{UniValue::VARR};
            for (const std::string& recovery_error : recovery.errors) {
                recovery_errors.push_back(recovery_error);
            }
            ProviderIdentityRecord guarded_identity;
            std::optional<ProviderWorkGuard> start_guard;
            if (context.paymaster && context.paymaster->Enabled() &&
                GetPaymasterIdentity(*wallet, guarded_identity)) {
                // Hold the same wallet-local transition slot used by disable
                // and policy updates while taking the authoritative readiness
                // snapshot. Completion below atomically converts this slot to
                // the running state.
                start_guard.emplace(
                    *context.paymaster, wallet->GetName(),
                    guarded_identity.provider_id,
                    /*require_running=*/false);
                if (!start_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
            }
            const ProviderReadiness readiness =
                GetProviderReadiness(*wallet, context, !automatic_autostart);
            bool running{false};
            bool drain_work{false};
            if (!readiness.identity.provider_id.IsNull()) {
                std::string drain_error;
                PaymasterStore store{*wallet};
                if (!store.HasProviderDrainWork(
                        readiness.identity.provider_id, drain_work,
                        drain_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, drain_error);
                }
            }
            const bool drain_only{!readiness.ready && drain_work};
            const auto is_liquidity_error = [](const std::string& value) {
                return value == "PAYMASTER_POOLS_NOT_PREPARED" ||
                       value == "PAYMASTER_ADMISSION_DGB_MISSING" ||
                       value == "PAYMASTER_ADMISSION_CARRIERS_MISSING" ||
                       value == "PAYMASTER_OPERATIONAL_SLOT_MISSING";
            };
            const bool maintenance_start =
                !readiness.ready && !readiness.errors.empty() &&
                readiness.settings.operation_mode == ProviderOperationMode::AUTOMATIC &&
                readiness.have_policy && readiness.have_liquidity_policy &&
                readiness.liquidity_policy.automatic_replenishment &&
                ProviderLiquidityTargetsSatisfyPolicy(
                    readiness.liquidity_policy, readiness.policy) &&
                std::all_of(readiness.errors.begin(), readiness.errors.end(),
                            is_liquidity_error);
            Announcement announcement;
            if (readiness.ready || drain_only || maintenance_start) {
                if (!start_guard ||
                    guarded_identity.provider_id !=
                        readiness.identity.provider_id) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_PROVIDER_RUNTIME_CONFLICT");
                }
                node::NodeContext* node = wallet->chain().context();
                if (!node || !node->chainman) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
                }
                std::string error;
                const bool restricted = readiness.policy.sponsorship_scope ==
                                        SponsorshipScope::RESTRICTED;
                if (readiness.ready && !restricted) {
                    uint256 reference_block;
                    {
                        LOCK(cs_main);
                        const CBlockIndex* tip =
                            node->chainman->ActiveChain().Tip();
                        if (!tip) {
                            throw JSONRPCError(RPC_MISC_ERROR,
                                               "PAYMASTER_NODE_NOT_READY");
                        }
                        reference_block = tip->GetBlockHash();
                    }
                    if (!BuildPaymasterAnnouncement(
                            *wallet, readiness.identity, readiness.policy,
                            readiness.pool_entries, readiness.endpoint,
                            Params().GenesisBlock().GetHash(), reference_block,
                            now, readiness.available_funding_models,
                            announcement, error) ||
                        !ValidateAnnouncementEnvelope(
                            announcement, Params().GenesisBlock().GetHash(),
                            now, error,
                            Params().GetChainType() == ChainType::REGTEST) ||
                        !ValidateAdmissionProofs(
                            announcement, *node->chainman, error) ||
                        !context.paymaster->GetDirectory().AddValidated(
                            announcement, now)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            error.empty() ? "PAYMASTER_ANNOUNCEMENT_REJECTED" : error);
                    }
                }
                running = start_guard->CompleteProviderStart();
                if (!running) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_RUNTIME_CONFLICT");
                ProviderServiceState initial_state{ProviderServiceState::ACTIVE};
                if (drain_only) {
                    initial_state = ProviderServiceState::DRAIN_ONLY;
                } else if (maintenance_start) {
                    initial_state = !readiness.have_liquidity_policy ||
                                            !readiness.liquidity_policy.paid_maintenance_approved
                        ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                        : ProviderServiceState::REPLENISHING_LIQUIDITY;
                } else if (readiness.settings.operation_mode ==
                           ProviderOperationMode::MANUAL) {
                    initial_state = ProviderServiceState::MANUAL;
                }
                context.paymaster->SetProviderServiceStatus(
                    wallet->GetName(), initial_state);
                if (readiness.ready && !restricted && node->connman) {
                    node->connman->ForEachNode([&announcement, node](CNode* peer) {
                        if (!peer->fSuccessfullyConnected || peer->fDisconnect) return;
                        const CNetMsgMaker maker{peer->GetCommonVersion()};
                        node->connman->PushMessage(peer, maker.Make(NetMsgType::PMANNOUNCE,
                                                                    announcement));
                    });
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("running", running);
            result.pushKV("ready", readiness.ready);
            result.pushKV("drain_only", drain_only && running);
            result.pushKV("operation_mode", ProviderOperationModeName(
                                                readiness.settings.operation_mode));
            result.pushKV("autostart", readiness.settings.autostart);
            ProviderServiceState service_state{ProviderServiceState::STOPPED};
            if (running) {
                if (drain_only) {
                    service_state = ProviderServiceState::DRAIN_ONLY;
                } else if (maintenance_start) {
                    service_state = !readiness.have_liquidity_policy ||
                                            !readiness.liquidity_policy.paid_maintenance_approved
                        ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                        : ProviderServiceState::REPLENISHING_LIQUIDITY;
                } else if (readiness.settings.operation_mode ==
                           ProviderOperationMode::MANUAL) {
                    service_state = ProviderServiceState::MANUAL;
                } else {
                    service_state = ProviderServiceState::ACTIVE;
                }
            } else if (readiness.settings.autostart && wallet->IsLocked()) {
                service_state = ProviderServiceState::WAITING_FOR_UNLOCK;
            } else if (readiness.settings.autostart && !readiness.ready) {
                service_state = ProviderServiceState::WAITING_FOR_READINESS;
            }
            if (!running && context.paymaster) {
                context.paymaster->SetProviderServiceStatus(
                    wallet->GetName(), service_state,
                    readiness.errors.empty() ||
                            service_state == ProviderServiceState::STOPPED
                        ? std::string{}
                        : StableProviderServiceError(
                              readiness.errors.front()));
            }
            result.pushKV("service_state",
                          ProviderServiceStateName(service_state));
            if (readiness.have_safety_policy) {
                result.pushKV("safety_policy",
                              ProviderSafetyPolicyToJSON(
                                  readiness.safety_policy));
            }
            if (!readiness.identity.provider_id.IsNull()) result.pushKV("provider_id", readiness.identity.provider_id.GetHex());
            if (running) {
                result.pushKV("endpoint", readiness.endpoint.ToStringAddrPort());
                if (announcement.sequence != 0) {
                    result.pushKV("announcement_sequence", announcement.sequence);
                }
            }
            result.pushKV("recovery_candidates", recovery.candidates);
            result.pushKV("recovered_broadcasts", recovery.recovered);
            result.pushKV("expired_commits", 0);
            result.pushKV("recovery_errors", std::move(recovery_errors));
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            return result;
        },
    };
}

RPCHelpMan stoppaymaster()
{
    return RPCHelpMan{
        "stoppaymaster",
        "Stop the ephemeral Paymaster provider runtime for this wallet. Durable recovery records are retained. "
        "A saved autostart setting may start it again later; disable autostart or the provider configuration when the stop must persist.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider runtime result", {{RPCResult::Type::BOOL, "running", "Always false after a successful stop"}}},
        RPCExamples{HelpExampleCli("stoppaymaster", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ProviderIdentityRecord identity;
            std::optional<ProviderWorkGuard> stop_guard;
            if (context.paymaster && context.paymaster->Enabled() &&
                GetPaymasterIdentity(*wallet, identity)) {
                stop_guard.emplace(
                    *context.paymaster, wallet->GetName(),
                    identity.provider_id, /*require_running=*/false);
                if (!stop_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
            }
            if (context.paymaster) {
                context.paymaster->StopProvider(wallet->GetName());
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("running", false);
            return result;
        },
    };
}

// One bounded unit of work per queue keeps the validation scheduler responsive
// and preserves fairness across wallets/providers. Temporary local failures
// retain their lease for retry; permanent protocol failures are durably rejected
// before the message is acknowledged.
void RunPaymasterProviderServiceCycle(WalletContext& context, CWallet& wallet)
{
    using namespace DigiDollar::Paymaster;
    Manager* const manager = context.paymaster;
    if (!manager || !manager->Enabled()) return;

    ProviderSettings settings;
    ProviderIdentityRecord identity;
    if (!GetPaymasterProviderSettings(wallet, settings) ||
        !GetPaymasterIdentity(wallet, identity) || !settings.enabled) {
        manager->SetProviderServiceStatus(wallet.GetName(),
                                          ProviderServiceState::STOPPED);
        return;
    }

    bool running = manager->IsProviderRunning(wallet.GetName(),
                                              identity.provider_id);
    if (!running) {
        if (!settings.autostart) {
            manager->SetProviderServiceStatus(wallet.GetName(),
                                              ProviderServiceState::STOPPED);
            return;
        }
        if (wallet.IsLocked()) {
            manager->SetProviderServiceStatus(
                wallet.GetName(), ProviderServiceState::WAITING_FOR_UNLOCK,
                "PAYMASTER_WALLET_LOCKED");
            return;
        }

        JSONRPCRequest start_request;
        start_request.strMethod = std::string{INTERNAL_PAYMASTER_AUTOSTART_METHOD};
        start_request.params = UniValue{UniValue::VARR};
        start_request.URI = WalletEndpointURI(wallet.GetName());
        start_request.context = &context;
        try {
            const UniValue result = startpaymaster().HandleRequest(start_request);
            running = result.find_value("running").isTrue();
            if (!running) {
                std::string error{"PAYMASTER_PROVIDER_NOT_READY"};
                const UniValue& readiness_errors =
                    result.find_value("readiness_errors");
                if (readiness_errors.isArray() &&
                    !readiness_errors.getValues().empty() &&
                    readiness_errors[0].isStr()) {
                    error = StableProviderServiceError(
                        readiness_errors[0].get_str());
                }
                manager->SetProviderServiceStatus(
                    wallet.GetName(),
                    wallet.IsLocked()
                        ? ProviderServiceState::WAITING_FOR_UNLOCK
                        : ProviderServiceState::WAITING_FOR_READINESS,
                    std::move(error));
                return;
            }
        } catch (const UniValue& rpc_error) {
            const UniValue& message = rpc_error.find_value("message");
            const std::string error = StableProviderServiceError(
                message.isStr() ? message.get_str()
                                : "PAYMASTER_AUTOSTART_FAILED");
            manager->SetProviderServiceStatus(
                wallet.GetName(),
                error == "PAYMASTER_WALLET_LOCKED"
                    ? ProviderServiceState::WAITING_FOR_UNLOCK
                    : ProviderServiceState::WAITING_FOR_READINESS,
                error);
            return;
        } catch (const std::exception&) {
            manager->SetProviderServiceStatus(
                wallet.GetName(), ProviderServiceState::FAULT,
                "PAYMASTER_AUTOSTART_FAILED");
            return;
        }
    }

    if (settings.operation_mode == ProviderOperationMode::MANUAL) {
        manager->SetProviderServiceStatus(wallet.GetName(),
                                          ProviderServiceState::MANUAL);
        return;
    }
    if (wallet.IsLocked()) {
        // Keep the explicitly started runtime registered, but never consume or
        // sign queued work until the operator unlocks the wallet again.
        manager->SetProviderServiceStatus(
            wallet.GetName(), ProviderServiceState::WAITING_FOR_UNLOCK,
            "PAYMASTER_WALLET_LOCKED");
        return;
    }

    const ProviderServiceStatus previous =
        manager->GetProviderServiceStatus(wallet.GetName());
    ProviderServiceState state =
        previous.state == ProviderServiceState::DRAIN_ONLY
            ? ProviderServiceState::DRAIN_ONLY
            : ProviderServiceState::ACTIVE;
    std::string first_error;
    bool accept_new_requests{state != ProviderServiceState::DRAIN_ONLY};

    if (accept_new_requests) {
        ProviderWorkGuard maintenance_guard{
            *manager, wallet.GetName(), identity.provider_id};
        if (!maintenance_guard.Acquired()) {
            // A manual expert action or another bounded provider step owns
            // the wallet. Leave its service state untouched and retry next
            // scheduler tick.
            return;
        }
        bool setup_pending{false};
        try {
            setup_pending = HasPendingPoolPreparation(wallet);
        } catch (const UniValue&) {
            manager->SetProviderServiceStatus(wallet.GetName(), ProviderServiceState::WAITING_FOR_READINESS,
                                              "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return;
        }
        // Do not replenish the same outputs twice. Existing signed payment
        // submissions below can still finish while setup waits for funding.
        if (setup_pending) {
            accept_new_requests = false;
            state = ProviderServiceState::WAITING_FOR_READINESS;
            first_error = "PAYMASTER_POOL_PREPARATION_PENDING";
        } else {
            ProviderReadiness liquidity = GetProviderReadiness(
                wallet, context, /*wait_for_sync=*/false);
            const ProviderLiquidityPolicy liquidity_policy =
                liquidity.have_liquidity_policy
                    ? liquidity.liquidity_policy
                    : SuggestedLiquidityPolicy(liquidity, GetTime());
            const bool targets_satisfy_provider_policy =
                !liquidity.have_policy ||
                ProviderLiquidityTargetsSatisfyPolicy(
                    liquidity_policy, liquidity.policy);
            const auto admission_dgb = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DGB,
                liquidity_policy.target_admission_dgb);
            const int64_t operational_dgb_minimum = liquidity.have_policy
                ? liquidity.policy.maximum_network_fee.value
                : std::numeric_limits<int64_t>::max();
            const auto operational_dgb = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
                liquidity_policy.target_operational_dgb,
                operational_dgb_minimum);
            const auto admission_carriers = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER,
                liquidity_policy.target_admission_carriers);
            const auto operational_carriers = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER,
                liquidity_policy.target_operational_carriers);
            const size_t missing_dgb = admission_dgb.missing + operational_dgb.missing;
            const size_t missing_carriers = admission_carriers.missing +
                                            operational_carriers.missing;
            const bool targets_confirmed =
                admission_dgb.ready >= liquidity_policy.target_admission_dgb &&
                operational_dgb.ready >= liquidity_policy.target_operational_dgb &&
                admission_carriers.ready >= liquidity_policy.target_admission_carriers &&
                operational_carriers.ready >= liquidity_policy.target_operational_carriers;
            if (!targets_satisfy_provider_policy) {
                // A zero operational carrier target can be a deliberate result of
                // release_slot. It is valid to persist, but automatic maintenance
                // must not claim success or silently recreate capital the operator
                // explicitly released. Require an explicit target change first.
                accept_new_requests = false;
                state = ProviderServiceState::WAITING_FOR_READINESS;
                first_error = "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE";
            } else if (missing_dgb + missing_carriers > 0) {
                accept_new_requests = false;
                if (!liquidity.have_liquidity_policy ||
                    !liquidity_policy.paid_maintenance_approved) {
                    state = ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL;
                    first_error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
                } else if (!liquidity_policy.automatic_replenishment) {
                    state = ProviderServiceState::WAITING_FOR_READINESS;
                    first_error = "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED";
                } else {
                    std::string maintenance_error;
                    const bool replenished = missing_carriers > 0
                        ? RunAutomaticCarrierReplenishment(
                              wallet, liquidity_policy,
                              admission_carriers.missing,
                              operational_carriers.missing,
                              maintenance_error)
                        : RunAutomaticDGBReplenishment(
                              wallet, liquidity_policy,
                              admission_dgb.missing,
                              operational_dgb.missing,
                              maintenance_error);
                    if (!replenished) {
                        first_error = StableProviderServiceError(
                            maintenance_error.empty()
                                ? "PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"
                                : maintenance_error);
                        state = first_error == "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED" ||
                                        first_error == "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"
                            ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                            : ProviderServiceState::REPLENISHING_LIQUIDITY;
                    } else {
                        state = ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
                        first_error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
                    }
                }
            } else if (!targets_confirmed) {
                accept_new_requests = false;
                state = ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
                first_error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
            }
        }
    }

    const bool announcement_refresh_required =
        previous.state == ProviderServiceState::WAITING_FOR_UNLOCK ||
        previous.state == ProviderServiceState::WAITING_FOR_READINESS ||
        previous.state == ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL ||
        previous.state == ProviderServiceState::REPLENISHING_LIQUIDITY ||
        previous.state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION ||
        previous.state == ProviderServiceState::FAULT;
    if (accept_new_requests && announcement_refresh_required) {
        ProviderReadiness resumed = GetProviderReadiness(
            wallet, context, /*wait_for_sync=*/false);
        std::string announcement_error;
        if (!PublishCurrentProviderAnnouncement(
                wallet, context, resumed, GetTime(), nullptr,
                announcement_error)) {
            accept_new_requests = false;
            state = ProviderServiceState::WAITING_FOR_READINESS;
            first_error = StableProviderServiceError(
                announcement_error.empty()
                    ? "PAYMASTER_ANNOUNCEMENT_REJECTED"
                    : announcement_error);
        }
    }

    JSONRPCRequest service_request;
    service_request.strMethod = std::string{INTERNAL_PAYMASTER_SERVICE_METHOD};
    service_request.params = UniValue{UniValue::VARR};
    service_request.URI = WalletEndpointURI(wallet.GetName());
    service_request.context = &context;

    const auto run_step = [&](const RPCHelpMan& handler) {
        try {
            handler.HandleRequest(service_request);
        } catch (const UniValue& rpc_error) {
            const UniValue& message = rpc_error.find_value("message");
            const std::string error = StableProviderServiceError(
                message.isStr() ? message.get_str()
                                : "PAYMASTER_AUTOMATIC_SERVICE_ERROR");
            if (error == "PAYMASTER_PROVIDER_SERVICE_BUSY") return;
            if (first_error.empty()) first_error = error;
            if (error == "PAYMASTER_WALLET_LOCKED") {
                state = ProviderServiceState::WAITING_FOR_UNLOCK;
            } else if (error == "PAYMASTER_PROVIDER_SYNCING" ||
                       error == "PAYMASTER_REQUIRES_READY_TXINDEX" ||
                       error == "PAYMASTER_TXINDEX_NOT_READY" ||
                       error == "PAYMASTER_NODE_NOT_READY") {
                state = ProviderServiceState::WAITING_FOR_READINESS;
            } else if (error == "PAYMASTER_PROVIDER_DRAIN_ONLY" ||
                       error == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED") {
                state = ProviderServiceState::DRAIN_ONLY;
            } else if (error == "PAYMASTER_PROVIDER_NOT_RUNNING") {
                state = ProviderServiceState::STOPPED;
            } else {
                state = ProviderServiceState::FAULT;
            }
        } catch (const std::exception&) {
            if (first_error.empty()) {
                first_error = "PAYMASTER_AUTOMATIC_SERVICE_ERROR";
            }
            state = ProviderServiceState::FAULT;
        }
    };

    // A liquidity pause blocks new capacity proofs but must not strand the
    // quote/recovery continuation that caused the reservation. The request
    // handler itself skips fresh capacity messages unless readiness is fully
    // restored. Drain-only providers still finish only already-authorized
    // submissions and never create a new quote.
    const bool process_request_continuations =
        state == ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL ||
        state == ProviderServiceState::REPLENISHING_LIQUIDITY ||
        state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
    if (accept_new_requests || process_request_continuations) {
        run_step(processpaymasterrequests());
    }
    if (state != ProviderServiceState::WAITING_FOR_UNLOCK &&
        state != ProviderServiceState::WAITING_FOR_READINESS &&
        state != ProviderServiceState::FAULT &&
        state != ProviderServiceState::STOPPED) {
        run_step(processpaymastersubmits());
    }
    if (!first_error.empty() &&
        (previous.state != state || previous.last_error != first_error)) {
        wallet.WalletLogPrintf(
            "Paymaster automatic provider service paused: %s\n",
            first_error);
    }
    manager->SetProviderServiceStatus(wallet.GetName(), state,
                                      std::move(first_error));
}

} // namespace wallet
