// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider identity, policy, liquidity, finance, and operator RPCs. */

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

RPCHelpMan createpaymasteridentity()
{
    return RPCHelpMan{
        "createpaymasteridentity",
        "Create or return this descriptor wallet's persistent BIP86/x-only Paymaster identity.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"display_name", RPCArg::Type::STR, RPCArg::Optional::NO, "Printable provider name, at most 32 bytes"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persistent public provider identity", {
                                                                                       {RPCResult::Type::STR_HEX, "provider_id", "Tagged hash of the identity key"},
                                                                                       {RPCResult::Type::STR_HEX, "identity_key", "Untweaked BIP86 x-only public key"},
                                                                                       {RPCResult::Type::STR, "display_name", "Persisted display name"},
                                                                                       {RPCResult::Type::NUM_TIME, "created_at", "Identity creation time"},
                                                                                   }},
        RPCExamples{HelpExampleCli("createpaymasteridentity", "\"Community Provider\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            DigiDollar::Paymaster::ProviderIdentityRecord identity;
            std::string error;
            if (!CreatePaymasterIdentity(*wallet, request.params[0].get_str(), GetTime(), identity, error)) {
                const int code = error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR;
                throw JSONRPCError(code, error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("provider_id", identity.provider_id.GetHex());
            result.pushKV("identity_key", HexStr(identity.identity_key));
            result.pushKV("display_name", identity.display_name);
            result.pushKV("created_at", identity.created_at);
            return result;
        },
    };
}

RPCHelpMan setpaymasterpolicy()
{
    return RPCHelpMan{
        "setpaymasterpolicy",
        "Validate and atomically persist this wallet's Paymaster operating policy. "
        "The provider must be stopped so it cannot accept new work under a previously announced policy while the wallet commit changes; the next start publishes a replacement announcement.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete provider policy", {
                                                                                                {"funding_models", RPCArg::Type::ARR, RPCArg::Optional::NO, "Non-empty funding model allowlist", {{"model", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "sponsored or user_paid"}}},
                                                                                                {"sponsorship_scope", RPCArg::Type::STR, RPCArg::Optional::NO, "public or restricted"},
                                                                                                {"fee_rate_bps", RPCArg::Type::NUM, RPCArg::Optional::NO, "User-paid rate in basis points"},
                                                                                                {"min_amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Minimum recipient amount"},
                                                                                                {"max_amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum recipient amount"},
                                                                                                {"quote_ttl", RPCArg::Type::NUM, RPCArg::Optional::NO, "Quote lifetime in seconds"},
                                                                                                {"maximum_network_fee_dgb_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Positive absolute provider network-fee cap"},
                                                                                            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", /*optional=*/false, "Persisted canonical policy", {
                                                                                                  {RPCResult::Type::ARR, "funding_models", "Enabled funding models", {{RPCResult::Type::STR, "", "sponsored or user_paid"}}},
                                                                                                  {RPCResult::Type::STR, "sponsorship_scope", "public or restricted"},
                                                                                                  {RPCResult::Type::NUM, "fee_rate_bps", "User-paid rate in basis points"},
                                                                                                  {RPCResult::Type::NUM, "min_amount_cents", "Minimum recipient amount"},
                                                                                                  {RPCResult::Type::NUM, "max_amount_cents", "Maximum recipient amount"},
                                                                                                  {RPCResult::Type::NUM, "quote_ttl", "Quote lifetime in seconds"},
                                                                                                  {RPCResult::Type::NUM, "maximum_network_fee_dgb_satoshis", "Absolute provider network-fee cap"},
                                                                                                  {RPCResult::Type::STR_HEX, "policy_hash", "Canonical provider policy hash"},
                                                                                              }},
        RPCExamples{HelpExampleCli("setpaymasterpolicy", "'{\"funding_models\":[\"sponsored\",\"user_paid\"],\"sponsorship_scope\":\"public\",\"fee_rate_bps\":50,\"min_amount_cents\":100,\"max_amount_cents\":100000,\"quote_ttl\":60,\"maximum_network_fee_dgb_satoshis\":20000000}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            DigiDollar::Paymaster::ProviderIdentityRecord identity;
            std::optional<ProviderWorkGuard> policy_guard;
            if (context.paymaster && context.paymaster->Enabled() &&
                GetPaymasterIdentity(*wallet, identity)) {
                // The stopped-state check and the database write must form one
                // scheduler exclusion window. Otherwise autostart could begin
                // the provider after IsProviderRunning() returned false but
                // before the new advertised policy reached the wallet.
                policy_guard.emplace(
                    *context.paymaster, wallet->GetName(), identity.provider_id,
                    /*require_running=*/false);
                if (!policy_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
                if (context.paymaster->IsProviderRunning(
                        wallet->GetName(), identity.provider_id)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_RUNNING");
                }
            }
            const DigiDollar::Paymaster::ProviderPolicy policy = ParseProviderPolicy(request.params[0].get_obj());
            std::string error;
            if (!SetPaymasterProviderPolicy(*wallet, policy, GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return ProviderPolicyToJSON(policy);
        },
    };
}

RPCHelpMan setpaymastersafetypolicy()
{
    return RPCHelpMan{
        "setpaymastersafetypolicy",
        "Persist finite wallet-local Paymaster provider loss and rate ceilings. "
        "All six values for an advertised funding model must be positive. "
        "All-zero limits disable only a model that is not advertised.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete local provider safety policy", {
                                                                                                             {"user_paid", RPCArg::Type::OBJ, RPCArg::Optional::NO, "USER_PAID limits", FundingSafetyLimitArgs()},
                                                                                                             {"public_sponsored", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Public SPONSORED limits", FundingSafetyLimitArgs()},
                                                                                                             {"restricted_sponsored", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Restricted SPONSORED limits", FundingSafetyLimitArgs()},
                                                                                                             {"maximum_active_quotes_total", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum active quotes across all peers"},
                                                                                                             {"maximum_active_quotes_per_netgroup", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum active quotes from one network group"},
                                                                                                             {"maximum_active_quotes_per_recipient", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum active quotes for one pseudonymous recipient bucket"},
                                                                                                             {"maximum_quote_requests_per_netgroup_per_minute", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum quote requests per network group per minute"},
                                                                                                         }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persisted wallet-local safety policy", ProviderSafetyPolicyResults()},
        RPCExamples{HelpExampleCli("setpaymastersafetypolicy", "'{\"user_paid\":{\"maximum_network_fee_per_transaction_satoshis\":1000000,\"maximum_reserved_network_fee_satoshis\":5000000,\"maximum_network_fee_per_hour_satoshis\":10000000,\"maximum_network_fee_per_day_satoshis\":50000000,\"maximum_completed_per_hour\":10,\"maximum_completed_per_day\":100},\"public_sponsored\":{\"maximum_network_fee_per_transaction_satoshis\":0,\"maximum_reserved_network_fee_satoshis\":0,\"maximum_network_fee_per_hour_satoshis\":0,\"maximum_network_fee_per_day_satoshis\":0,\"maximum_completed_per_hour\":0,\"maximum_completed_per_day\":0},\"restricted_sponsored\":{\"maximum_network_fee_per_transaction_satoshis\":0,\"maximum_reserved_network_fee_satoshis\":0,\"maximum_network_fee_per_hour_satoshis\":0,\"maximum_network_fee_per_day_satoshis\":0,\"maximum_completed_per_hour\":0,\"maximum_completed_per_day\":0},\"maximum_active_quotes_total\":16,\"maximum_active_quotes_per_netgroup\":4,\"maximum_active_quotes_per_recipient\":2,\"maximum_quote_requests_per_netgroup_per_minute\":10}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ProviderIdentityRecord identity;
            std::optional<ProviderWorkGuard> safety_guard;
            if (context.paymaster && context.paymaster->Enabled() &&
                GetPaymasterIdentity(*wallet, identity)) {
                // Safety updates may remain available while the provider is
                // online, but the authoritative read/commit must not overlap
                // a scheduler step that is reserving against the old limits.
                safety_guard.emplace(
                    *context.paymaster, wallet->GetName(),
                    identity.provider_id, /*require_running=*/false);
                if (!safety_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
            }
            auto policy = ParseProviderSafetyPolicy(request.params[0].get_obj());
            std::string error;
            if (!SetPaymasterProviderSafetyPolicy(*wallet, policy, GetTime(), error) ||
                !GetPaymasterProviderSafetyPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "PAYMASTER_SAFETY_POLICY_RELOAD_FAILED" : error);
            }
            return ProviderSafetyPolicyToJSON(policy);
        },
    };
}

RPCHelpMan getpaymastersafetystatus()
{
    return RPCHelpMan{
        "getpaymastersafetystatus",
        "Return the provider's local safety policy and durable fee-budget usage.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider safety status", {
                                                                          {RPCResult::Type::BOOL, "configured", "Whether both valid policy and ledger exist"},
                                                                          {RPCResult::Type::BOOL, "ledger_present", "Whether a valid durable ledger exists"},
                                                                          {RPCResult::Type::OBJ, "policy", /*optional=*/true, "Wallet-local provider safety policy", ProviderSafetyPolicyResults()},
                                                                          {RPCResult::Type::NUM_TIME, "accounting_time_high_water", /*optional=*/true, "Monotonic accounting time persisted against clock rollback"},
                                                                          {RPCResult::Type::OBJ, "user_paid", /*optional=*/true, "USER_PAID rolling counters", ProviderSafetyClassStatusResults()},
                                                                          {RPCResult::Type::OBJ, "public_sponsored", /*optional=*/true, "Public SPONSORED rolling counters", ProviderSafetyClassStatusResults()},
                                                                          {RPCResult::Type::OBJ, "restricted_sponsored", /*optional=*/true, "Restricted SPONSORED rolling counters", ProviderSafetyClassStatusResults()},
                                                                      }},
        RPCExamples{HelpExampleCli("getpaymastersafetystatus", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            {
                PaymasterStore store{*wallet};
                size_t expired_quotes{0};
                std::string expiry_error;
                if (!store.ExpireProviderQuotes(GetTime(), expired_quotes,
                                                expiry_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        expiry_error.empty() ? "PAYMASTER_QUOTE_EXPIRY_FAILED" : expiry_error);
                }
                size_t expired_capacity_reservations{0};
                expiry_error.clear();
                if (!store.ExpireProviderCapacityReservations(
                        GetTime(), expired_capacity_reservations,
                        expiry_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        expiry_error.empty() ? "PAYMASTER_CAPACITY_EXPIRY_FAILED" : expiry_error);
                }
            }
            DigiDollar::Paymaster::ProviderSafetyPolicy policy;
            DigiDollar::Paymaster::ProviderBudgetLedger ledger;
            const bool have_policy = GetPaymasterProviderSafetyPolicy(*wallet, policy);
            const bool have_ledger = GetPaymasterProviderBudgetLedger(*wallet, ledger);
            UniValue result{UniValue::VOBJ};
            result.pushKV("configured", have_policy && have_ledger);
            result.pushKV("ledger_present", have_ledger);
            if (have_policy) result.pushKV("policy", ProviderSafetyPolicyToJSON(policy));
            if (have_ledger) {
                result.pushKV("accounting_time_high_water", ledger.accounting_time_high_water);
            }
            if (have_policy && have_ledger) {
                const int64_t now = GetTime();
                using namespace DigiDollar::Paymaster;
                result.pushKV("user_paid", ProviderSafetyClassStatusToJSON(
                                               ledger, policy, FundingModel::USER_PAID, SponsorshipScope::PUBLIC, now));
                result.pushKV("public_sponsored", ProviderSafetyClassStatusToJSON(
                                                      ledger, policy, FundingModel::SPONSORED, SponsorshipScope::PUBLIC, now));
                result.pushKV("restricted_sponsored", ProviderSafetyClassStatusToJSON(
                                                          ledger, policy, FundingModel::SPONSORED, SponsorshipScope::RESTRICTED, now));
            }
            return result;
        },
    };
}

RPCHelpMan setpaymasterclientsafetypolicy()
{
    return RPCHelpMan{
        "setpaymasterclientsafetypolicy",
        "Persist wallet-local per-transfer and rolling-day Paymaster service-fee ceilings.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete local client safety policy", {
                                                                                                           {"maximum_service_fee_per_transaction_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum service fee for one transfer"},
                                                                                                           {"maximum_service_fee_per_day_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Rolling-day service-fee ceiling"},
                                                                                                       }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persisted client safety policy", {
                                                                                  {RPCResult::Type::NUM, "maximum_service_fee_per_transaction_cents", "Per-transfer service-fee ceiling"},
                                                                                  {RPCResult::Type::NUM, "maximum_service_fee_per_day_cents", "Rolling-day service-fee ceiling"},
                                                                                  {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
                                                                              }},
        RPCExamples{HelpExampleCli("setpaymasterclientsafetypolicy", "'{\"maximum_service_fee_per_transaction_cents\":100,\"maximum_service_fee_per_day_cents\":1000}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto policy = ParseClientSafetyPolicy(request.params[0].get_obj());
            std::string error;
            if (!SetPaymasterClientSafetyPolicy(*wallet, policy, GetTime(), error) ||
                !GetPaymasterClientSafetyPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "PAYMASTER_CLIENT_SAFETY_POLICY_RELOAD_FAILED" : error);
            }
            return ClientSafetyPolicyToJSON(policy);
        },
    };
}

RPCHelpMan getpaymasterclientsafetystatus()
{
    return RPCHelpMan{
        "getpaymasterclientsafetystatus",
        "Return the local client fee policy and durable rolling-day usage.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Client safety status", {
                                                                        {RPCResult::Type::BOOL, "configured", "Whether both valid policy and ledger exist"},
                                                                        {RPCResult::Type::BOOL, "ledger_present", "Whether a valid durable ledger exists"},
                                                                        {RPCResult::Type::OBJ, "policy", /*optional=*/true, "Client safety policy", {
                                                                                                                                                        {RPCResult::Type::NUM, "maximum_service_fee_per_transaction_cents", "Per-transfer ceiling"},
                                                                                                                                                        {RPCResult::Type::NUM, "maximum_service_fee_per_day_cents", "Rolling-day ceiling"},
                                                                                                                                                        {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
                                                                                                                                                    }},
                                                                        {RPCResult::Type::NUM_TIME, "accounting_time_high_water", /*optional=*/true, "Monotonic accounting time"},
                                                                        {RPCResult::Type::NUM, "active_reservations", /*optional=*/true, "Active fee reservations"},
                                                                        {RPCResult::Type::NUM, "reserved_service_fee_cents", /*optional=*/true, "Service fee held by active authorizations"},
                                                                        {RPCResult::Type::NUM, "spent_service_fee_last_day_cents", /*optional=*/true, "Committed service fees in the rolling day"},
                                                                        {RPCResult::Type::NUM, "available_service_fee_today_cents", /*optional=*/true, "Remaining rolling-day allowance after reservations"},
                                                                    }},
        RPCExamples{HelpExampleCli("getpaymasterclientsafetystatus", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ClientSafetyPolicy policy;
            ClientFeeLedger ledger;
            const bool have_policy = GetPaymasterClientSafetyPolicy(*wallet, policy);
            const bool have_ledger = GetPaymasterClientFeeLedger(*wallet, ledger);
            UniValue result{UniValue::VOBJ};
            result.pushKV("configured", have_policy && have_ledger);
            result.pushKV("ledger_present", have_ledger);
            if (have_policy) result.pushKV("policy", ClientSafetyPolicyToJSON(policy));
            if (have_ledger) {
                const int64_t effective_now = std::max(GetTime(), ledger.accounting_time_high_water);
                int64_t reserved{0};
                int64_t spent_day{0};
                uint64_t active{0};
                for (const ClientFeeReservation& reservation : ledger.reservations) {
                    if (reservation.state == BudgetReservationState::RESERVED) {
                        ++active;
                        reserved += reservation.service_fee.value;
                    } else if (reservation.state == BudgetReservationState::SPENT &&
                               !TimeDeltaExceeds(effective_now, reservation.updated_at, 24 * 60 * 60)) {
                        spent_day += reservation.service_fee.value;
                    }
                }
                result.pushKV("accounting_time_high_water", ledger.accounting_time_high_water);
                result.pushKV("active_reservations", active);
                result.pushKV("reserved_service_fee_cents", reserved);
                result.pushKV("spent_service_fee_last_day_cents", spent_day);
                if (have_policy) {
                    result.pushKV("available_service_fee_today_cents",
                                  std::max<int64_t>(0, policy.maximum_service_fee_per_day.value -
                                                           reserved - spent_day));
                }
            }
            return result;
        },
    };
}

RPCHelpMan createrestrictedpaymasterdescriptor()
{
    return RPCHelpMan{
        "createrestrictedpaymasterdescriptor",
        "Create a short-lived provider-signed restricted service descriptor. "
        "The descriptor is not announced or persisted by the node.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"sponsor_authorization_key", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Sponsor/app x-only Schnorr authorization key"},
            {"sponsor_display_name", RPCArg::Type::STR, RPCArg::Optional::NO,
             "Printable sponsor name, at most 32 bytes"},
            {"expires_in", RPCArg::Type::NUM, RPCArg::Default{300},
             "Descriptor lifetime in seconds, from 1 through 600"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Non-gossiped restricted descriptor", {
                                                                                      {RPCResult::Type::STR_HEX, "descriptor", "Canonical serialized descriptor"},
                                                                                      {RPCResult::Type::STR_HEX, "provider_id", "Bound provider identity"},
                                                                                      {RPCResult::Type::STR_HEX, "provider_identity_key", "Provider x-only verification key"},
                                                                                      {RPCResult::Type::STR_HEX, "offer_id", "Restricted offer identifier"},
                                                                                      {RPCResult::Type::STR_HEX, "policy_hash", "Restricted policy hash"},
                                                                                      {RPCResult::Type::STR, "endpoint", "Direct Paymaster endpoint"},
                                                                                      {RPCResult::Type::NUM_TIME, "expires_at", "Descriptor expiry"},
                                                                                  }},
        RPCExamples{HelpExampleCli("createrestrictedpaymasterdescriptor", "\"0123...\" \"Merchant Sponsor\" 300")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const std::vector<unsigned char> key_bytes =
                ParseHexV(request.params[0], "sponsor_authorization_key");
            if (key_bytes.size() != 32) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "sponsor_authorization_key must be exactly 32 bytes");
            }
            const XOnlyPubKey sponsor_key{MakeUCharSpan(key_bytes)};
            const int64_t ttl = request.params[2].isNull() ? 300 : request.params[2].getInt<int64_t>();
            if (!sponsor_key.IsFullyValid() || ttl < 1 || ttl > ANNOUNCEMENT_TTL_SECONDS) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Invalid sponsor key or expires_in outside 1..600");
            }
            const ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            if (!readiness.ready || readiness.policy.sponsorship_scope != SponsorshipScope::RESTRICTED) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   readiness.ready ? "PAYMASTER_POLICY_NOT_RESTRICTED" : "PAYMASTER_PROVIDER_NOT_READY");
            }
            node::NodeContext* node = wallet->chain().context();
            if (!node || !node->chainman) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
            }
            uint256 reference_block;
            {
                LOCK(cs_main);
                const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
                if (!tip) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_NODE_NOT_READY");
                reference_block = tip->GetBlockHash();
            }
            const int64_t now = GetTime();
            RestrictedServiceDescriptor descriptor;
            std::string error;
            if (!BuildRestrictedServiceDescriptor(
                    *wallet, readiness.identity, readiness.policy, readiness.pool_entries,
                    readiness.endpoint, sponsor_key, request.params[1].get_str(),
                    Params().GenesisBlock().GetHash(), reference_block, now,
                    SaturatingAddSeconds(now, ttl),
                    descriptor, error) ||
                !ValidateRestrictedServiceDescriptor(
                    descriptor, readiness.identity.identity_key,
                    Params().GenesisBlock().GetHash(), now, error,
                    Params().GetChainType() == ChainType::REGTEST) ||
                !ValidateRestrictedAdmissionProofs(descriptor, readiness.identity.identity_key,
                                                   *node->chainman, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
            stream << descriptor;
            UniValue result{UniValue::VOBJ};
            result.pushKV("descriptor", HexStr(MakeUCharSpan(stream)));
            result.pushKV("provider_id", descriptor.provider_id.GetHex());
            result.pushKV("provider_identity_key", HexStr(readiness.identity.identity_key));
            result.pushKV("offer_id", descriptor.offer_id.GetHex());
            result.pushKV("policy_hash", descriptor.policy_hash.GetHex());
            result.pushKV("endpoint", descriptor.p2p_endpoint.ToStringAddrPort());
            result.pushKV("expires_at", descriptor.expires_at);
            return result;
        },
    };
}

RPCHelpMan setpaymasterenabled()
{
    return RPCHelpMan{
        "setpaymasterenabled",
        "Persistently enable or disable Paymaster provider operation for this wallet.\n"
        "Enabling does not synchronously start the provider. A saved autostart setting may bring it online later when all readiness checks pass; otherwise use startpaymaster explicitly.\n",
        {
            {"enabled", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Provider enablement"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persistent provider configuration", {
                                                                                     {RPCResult::Type::BOOL, "enabled", "Configured provider enablement"},
                                                                                     {RPCResult::Type::BOOL, "running", "Whether the runtime provider is online"},
                                                                                     {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Bound provider policy"},
                                                                                 }},
        RPCExamples{HelpExampleCli("setpaymasterenabled", "true")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const bool requested_enabled = request.params[0].get_bool();
            DigiDollar::Paymaster::ProviderIdentityRecord identity;
            std::optional<ProviderWorkGuard> disable_guard;
            if (!requested_enabled && context.paymaster &&
                context.paymaster->Enabled() &&
                GetPaymasterIdentity(*wallet, identity)) {
                // Serialize the persistent disable and runtime stop with the
                // guarded start transition and all provider work. Whichever
                // operation owns the slot first determines a coherent final
                // state.
                disable_guard.emplace(
                    *context.paymaster, wallet->GetName(), identity.provider_id,
                    /*require_running=*/false);
                if (!disable_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
            }
            std::string error;
            if (!SetPaymasterProviderEnabled(
                    *wallet, requested_enabled, GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            DigiDollar::Paymaster::ProviderSettings settings;
            if (!GetPaymasterProviderSettings(*wallet, settings)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload Paymaster settings");
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("enabled", settings.enabled);
            if (!settings.enabled && context.paymaster) context.paymaster->StopProvider(wallet->GetName());
            const bool running = settings.enabled && context.paymaster &&
                                 GetPaymasterIdentity(*wallet, identity) &&
                                 context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id);
            result.pushKV("running", running);
            if (!settings.policy_hash.IsNull()) result.pushKV("policy_hash", settings.policy_hash.GetHex());
            return result;
        },
    };
}

RPCHelpMan setpaymasterruntimesettings()
{
    return RPCHelpMan{
        "setpaymasterruntimesettings",
        "Persist this wallet's Paymaster provider processing mode and optional autostart. "
        "Changing operation_mode is allowed only while the provider is stopped. "
        "Autostart never stores a wallet passphrase.\n",
        {
            {"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO,
             "Runtime settings; omitted members retain their current values",
             {
                 {"operation_mode", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                  "automatic (recommended) or manual (expert mode)"},
                 {"autostart", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Start the configured runtime after wallet load when readiness permits"},
             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persistent runtime settings",
                  {
                      {RPCResult::Type::STR, "operation_mode", "automatic or manual"},
                      {RPCResult::Type::BOOL, "autostart", "Whether optional runtime autostart is enabled"},
                      {RPCResult::Type::BOOL, "running", "Whether this wallet's provider runtime is active"},
                  }},
        RPCExamples{HelpExampleCli(
            "setpaymasterruntimesettings",
            "'{\"operation_mode\":\"automatic\",\"autostart\":false}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            ProviderIdentityRecord identity;
            const bool have_identity =
                GetPaymasterIdentity(*wallet, identity);
            std::optional<ProviderWorkGuard> runtime_guard;
            if (context.paymaster && context.paymaster->Enabled() &&
                have_identity) {
                // A mode change and a guarded start must be ordered against
                // each other. Otherwise both could observe the stopped state
                // before the database commit makes the new mode authoritative.
                runtime_guard.emplace(
                    *context.paymaster, wallet->GetName(),
                    identity.provider_id, /*require_running=*/false);
                if (!runtime_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
            }

            const UniValue& value = request.params[0].get_obj();
            RPCTypeCheckObj(
                value,
                {{"operation_mode", UniValueType(UniValue::VSTR)},
                 {"autostart", UniValueType(UniValue::VBOOL)}},
                /*fAllowNull=*/true, /*fStrict=*/true);
            if (value.find_value("operation_mode").isNull() &&
                value.find_value("autostart").isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "At least one runtime setting is required");
            }

            ProviderSettings current;
            if (!GetPaymasterProviderSettings(*wallet, current)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND");
            }
            ProviderOperationMode mode = current.operation_mode;
            const UniValue& mode_value = value.find_value("operation_mode");
            if (!mode_value.isNull()) {
                const std::string mode_name = mode_value.get_str();
                if (mode_name == "automatic") {
                    mode = ProviderOperationMode::AUTOMATIC;
                } else if (mode_name == "manual") {
                    mode = ProviderOperationMode::MANUAL;
                } else {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "operation_mode must be automatic or manual");
                }
            }
            const UniValue& autostart_value = value.find_value("autostart");
            const bool autostart = autostart_value.isNull()
                                       ? current.autostart
                                       : autostart_value.get_bool();

            const bool running = context.paymaster &&
                                 have_identity &&
                                 context.paymaster->IsProviderRunning(
                                     wallet->GetName(), identity.provider_id);
            if (running && mode != current.operation_mode) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_STOP_PROVIDER_BEFORE_MODE_CHANGE");
            }
            std::string error;
            if (!SetPaymasterProviderRuntimeSettings(
                    *wallet, mode, autostart, GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("operation_mode", ProviderOperationModeName(mode));
            result.pushKV("autostart", autostart);
            result.pushKV("running", running);
            return result;
        },
    };
}

RPCHelpMan setpaymasterliquiditypolicy()
{
    return RPCHelpMan{
        "setpaymasterliquiditypolicy",
        "Set wallet-local automatic Paymaster liquidity targets and finite maintenance-fee limits.\n"
        "Setting paid_maintenance_approved=true is the operator's explicit consent to paid replenishment.\n"
        "A zero maintenance limit disables paid maintenance; it never means unlimited.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Liquidity targets and maintenance budget", {
                {"automatic_replenishment", RPCArg::Type::BOOL, RPCArg::Default{true}, "Automatically restore missing slots while an automatic provider is running"},
                {"paid_maintenance_approved", RPCArg::Type::BOOL, RPCArg::Default{false}, "Explicitly approve paid maintenance transactions"},
                {"target_admission_dgb", RPCArg::Type::NUM, RPCArg::Optional::NO, "Admission DGB target (3-16)"},
                {"target_operational_dgb", RPCArg::Type::NUM, RPCArg::Optional::NO, "Operational DGB target (1-16)"},
                {"target_admission_carriers", RPCArg::Type::NUM, RPCArg::Default{0}, "Admission carrier target (0 or 3-16)"},
                {"target_operational_carriers", RPCArg::Type::NUM, RPCArg::Default{0}, "Operational carrier target (0 or 1-16)"},
                {"maximum_maintenance_fee_per_transaction_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum DGB fee for one maintenance transaction, in satoshis"},
                {"maximum_maintenance_fee_per_hour_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Rolling-hour maintenance fee ceiling, in satoshis"},
                {"maximum_maintenance_fee_per_day_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Rolling-day maintenance fee ceiling, in satoshis"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Saved policy",
                  ProviderLiquidityPolicyResults()},
        RPCExamples{HelpExampleCli("setpaymasterliquiditypolicy", "'{\"automatic_replenishment\":true,\"paid_maintenance_approved\":true,\"target_admission_dgb\":3,\"target_operational_dgb\":1,\"target_admission_carriers\":3,\"target_operational_carriers\":1,\"maximum_maintenance_fee_per_transaction_satoshis\":1000000,\"maximum_maintenance_fee_per_hour_satoshis\":5000000,\"maximum_maintenance_fee_per_day_satoshis\":20000000}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ProviderIdentityRecord identity;
            if (!context.paymaster ||
                !GetPaymasterIdentity(*wallet, identity)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
            }
            ProviderWorkGuard work_guard{
                *context.paymaster, wallet->GetName(), identity.provider_id,
                /*require_running=*/false};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_BUSY");
            }
            const UniValue& value = request.params[0];
            RPCTypeCheckObj(value,
                            {{"automatic_replenishment", UniValueType(UniValue::VBOOL)},
                             {"paid_maintenance_approved", UniValueType(UniValue::VBOOL)},
                             {"target_admission_dgb", UniValueType(UniValue::VNUM)},
                             {"target_operational_dgb", UniValueType(UniValue::VNUM)},
                             {"target_admission_carriers", UniValueType(UniValue::VNUM)},
                             {"target_operational_carriers", UniValueType(UniValue::VNUM)},
                             {"maximum_maintenance_fee_per_transaction_satoshis", UniValueType(UniValue::VNUM)},
                             {"maximum_maintenance_fee_per_hour_satoshis", UniValueType(UniValue::VNUM)},
                             {"maximum_maintenance_fee_per_day_satoshis", UniValueType(UniValue::VNUM)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            ProviderLiquidityPolicy policy;
            policy.automatic_replenishment = value.find_value("automatic_replenishment").isNull()
                ? true : value.find_value("automatic_replenishment").get_bool();
            policy.paid_maintenance_approved = value.find_value("paid_maintenance_approved").isNull()
                ? false : value.find_value("paid_maintenance_approved").get_bool();
            policy.target_admission_dgb = value.find_value("target_admission_dgb").getInt<uint16_t>();
            policy.target_operational_dgb = value.find_value("target_operational_dgb").getInt<uint16_t>();
            policy.target_admission_carriers = value.find_value("target_admission_carriers").isNull()
                ? 0 : value.find_value("target_admission_carriers").getInt<uint16_t>();
            policy.target_operational_carriers = value.find_value("target_operational_carriers").isNull()
                ? 0 : value.find_value("target_operational_carriers").getInt<uint16_t>();
            policy.maximum_maintenance_fee_per_transaction = DGBSatoshis{
                value.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>()};
            policy.maximum_maintenance_fee_per_hour = DGBSatoshis{
                value.find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>()};
            policy.maximum_maintenance_fee_per_day = DGBSatoshis{
                value.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>()};
            int64_t policy_time = GetTime();
            ProviderLiquidityPolicy current_policy;
            if (GetPaymasterProviderLiquidityPolicy(
                    *wallet, current_policy)) {
                if (current_policy.updated_at ==
                    std::numeric_limits<int64_t>::max()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_LIQUIDITY_POLICY_TIME_EXHAUSTED");
                }
                policy_time = std::max(
                    policy_time, current_policy.updated_at + 1);
            }
            policy.updated_at = policy_time;
            std::string error;
            if (!SetPaymasterProviderLiquidityPolicy(
                    *wallet, policy, policy.updated_at, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return LiquidityPolicyToJSON(policy);
        },
    };
}

RPCHelpMan getpaymasterliquiditystatus()
{
    return RPCHelpMan{
        "getpaymasterliquiditystatus",
        "Return Paymaster liquidity targets, ready/pending/missing slots, maintenance budget and recyclable carrier value.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Liquidity status",
                  ProviderLiquidityStatusResults()},
        RPCExamples{HelpExampleCli("getpaymasterliquiditystatus", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            return ProviderLiquidityStatusToJSON(readiness, GetTime());
        },
    };
}

RPCHelpMan withdrawpaymastercarrier()
{
    return RPCHelpMan{
        "withdrawpaymastercarrier",
        "Preview or execute a carrier excess withdrawal, or release one operational carrier slot.\n"
        "Execution requires the unchanged plan_id returned by the immediately preceding preview.\n"
        "all_excess keeps exactly 1.00 DD in every selected carrier and sends only the combined excess to a fresh wallet address.\n"
        "release_slot is wallet-local, costs no network fee, requires a stopped provider, and reduces the operational carrier target by one.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Withdrawal mode and preview binding", {
                {"mode", RPCArg::Type::STR, RPCArg::Optional::NO, "all_excess or release_slot"},
                {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Execute the previously reviewed plan"},
                {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged plan id required for execution"},
                {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Carrier txid for release_slot preview"},
                {"vout", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Carrier output index for release_slot preview"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Carrier withdrawal preview or execution", {
            {RPCResult::Type::BOOL, "executed", "Whether the plan was executed"},
            {RPCResult::Type::STR_HEX, "plan_id", "Plan id bound to exact inputs, outputs, limits, and expiry"},
            {RPCResult::Type::STR, "mode", "all_excess or release_slot"},
            {RPCResult::Type::NUM, "source_carriers", /*optional=*/true, "Number of carrier outputs bound to the plan"},
            {RPCResult::Type::NUM, "withdrawable_excess_cents", /*optional=*/true, "Combined DD excess"},
            {RPCResult::Type::NUM, "retained_carrier_cents", /*optional=*/true, "DD retained in carrier slots"},
            {RPCResult::Type::NUM, "estimated_network_fee_satoshis", /*optional=*/true, "Exact reviewed maintenance fee estimate"},
            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Executed transaction id"},
            {RPCResult::Type::NUM, "operational_carrier_target", /*optional=*/true, "Target after a release_slot operation"},
            {RPCResult::Type::NUM_TIME, "expires_at", /*optional=*/true, "Preview expiry"},
        }},
        RPCExamples{
            HelpExampleCli("withdrawpaymastercarrier", "'{\"mode\":\"all_excess\"}'") +
            HelpExampleCli("withdrawpaymastercarrier", "'{\"mode\":\"all_excess\",\"execute\":true,\"plan_id\":\"PLAN\"}'") +
            HelpExampleCli("withdrawpaymastercarrier", "'{\"mode\":\"release_slot\",\"txid\":\"TXID\",\"vout\":0}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(
                options,
                {{"mode", UniValueType(UniValue::VSTR)},
                 {"execute", UniValueType(UniValue::VBOOL)},
                 {"plan_id", UniValueType(UniValue::VSTR)},
                 {"txid", UniValueType(UniValue::VSTR)},
                 {"vout", UniValueType(UniValue::VNUM)}},
                /*fAllowNull=*/true, /*fStrict=*/true);
            const std::string mode_name = options.find_value("mode").get_str();
            CarrierWithdrawalMode mode;
            if (mode_name == "all_excess") {
                mode = CarrierWithdrawalMode::ALL_EXCESS;
            } else if (mode_name == "release_slot") {
                mode = CarrierWithdrawalMode::RELEASE_SLOT;
            } else {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_MODE");
            }
            const UniValue& execute_value = options.find_value("execute");
            const bool execute = !execute_value.isNull() && execute_value.get_bool();
            ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            if (!context.paymaster || !readiness.have_identity) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
            }
            ProviderWorkGuard work_guard{
                *context.paymaster, wallet->GetName(),
                readiness.identity.provider_id,
                /*require_running=*/false};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_BUSY");
            }
            if (!readiness.have_liquidity_policy) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND");
            }
            ProviderLiquidityPolicy liquidity_policy = readiness.liquidity_policy;
            const int64_t now = GetTime();

            const auto provider_is_running = [&] {
                return context.paymaster && readiness.have_identity &&
                       context.paymaster->IsProviderRunning(
                           wallet->GetName(), readiness.identity.provider_id);
            };

            if (!execute) {
                ProviderCarrierWithdrawalPlan plan;
                do {
                    plan.operation_id = GetRandHash();
                } while (plan.operation_id.IsNull());
                plan.mode = mode;
                plan.liquidity_policy_updated_at = liquidity_policy.updated_at;
                plan.created_at = now;
                plan.expires_at = SaturatingAddSeconds(now, 10 * 60);

                if (mode == CarrierWithdrawalMode::RELEASE_SLOT) {
                    if (provider_is_running()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_PROVIDER_MUST_BE_STOPPED");
                    }
                    const UniValue& txid_value = options.find_value("txid");
                    const UniValue& vout_value = options.find_value("vout");
                    if (txid_value.isNull() || vout_value.isNull()) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RELEASE_SLOT_OUTPOINT_REQUIRED");
                    }
                    const int64_t vout = vout_value.getInt<int64_t>();
                    if (vout < 0 ||
                        vout > std::numeric_limits<uint32_t>::max()) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_INVALID_CARRIER_OUTPOINT");
                    }
                    const COutPoint source{
                        ParseHashV(txid_value, "txid"),
                        static_cast<uint32_t>(vout)};
                    const auto found = std::find_if(
                        readiness.pool_entries.begin(),
                        readiness.pool_entries.end(),
                        [&](const ProviderPoolEntry& entry) {
                            return entry.outpoint == source;
                        });
                    if (found == readiness.pool_entries.end() ||
                        found->purpose != PoolPurpose::OPERATIONAL ||
                        found->asset != PoolAsset::DD_CARRIER ||
                        found->state != PoolEntryState::AVAILABLE ||
                        found->confirmation_height <= 0 ||
                        wallet->IsSpent(found->outpoint)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE");
                    }
                    if (liquidity_policy.target_operational_carriers == 0) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_OPERATIONAL_CARRIER_TARGET_ALREADY_ZERO");
                    }
                    plan.source_carriers.push_back(source);
                    plan.target_operational_carriers_after_release =
                        liquidity_policy.target_operational_carriers - 1;
                } else {
                    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
                    if (!dd_wallet) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DD_WALLET_UNAVAILABLE");
                    }
                    int64_t total_excess{0};
                    for (const ProviderPoolEntry& entry :
                         readiness.pool_entries) {
                        if (entry.asset != PoolAsset::DD_CARRIER ||
                            entry.state != PoolEntryState::AVAILABLE ||
                            entry.confirmation_height <= 0 ||
                            entry.carrier_value.value <= 100 ||
                            wallet->IsSpent(entry.outpoint)) {
                            continue;
                        }
                        if (plan.source_carriers.size() >= 64 ||
                            total_excess >
                                std::numeric_limits<int64_t>::max() -
                                    (entry.carrier_value.value - 100)) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                "PAYMASTER_CARRIER_WITHDRAWAL_TOO_LARGE");
                        }
                        const auto destination = wallet->GetNewDestination(
                            OutputType::BECH32M,
                            "Paymaster retained carrier");
                        if (!destination) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                        }
                        ProviderMaintenanceOutput replacement;
                        replacement.purpose = entry.purpose;
                        replacement.asset = PoolAsset::DD_CARRIER;
                        replacement.script_pub_key =
                            GetScriptForDestination(*destination);
                        replacement.carrier_value = DDCents{100};
                        plan.source_carriers.push_back(entry.outpoint);
                        plan.replacement_carriers.push_back(
                            std::move(replacement));
                        total_excess += entry.carrier_value.value - 100;
                    }
                    if (plan.source_carriers.empty() || total_excess <= 0) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_NO_WITHDRAWABLE_CARRIER_EXCESS");
                    }
                    const auto excess_destination = wallet->GetNewDestination(
                        OutputType::BECH32M,
                        "Paymaster carrier fee withdrawal");
                    if (!excess_destination) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                    }
                    plan.excess_script_pub_key =
                        GetScriptForDestination(*excess_destination);
                    plan.excess_amount = DDCents{total_excess};

                    std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
                    const auto append_recipient = [&](const CScript& script,
                                                      CAmount amount) {
                        CTxDestination destination;
                        if (!ExtractDestination(script, destination)) return false;
                        const std::string encoded =
                            EncodeDigiDollarAddress(destination);
                        CDigiDollarAddress address{encoded};
                        if (encoded.empty() || !address.IsValid()) return false;
                        recipients.emplace_back(std::move(address), amount);
                        return true;
                    };
                    for (const ProviderMaintenanceOutput& replacement :
                         plan.replacement_carriers) {
                        if (!append_recipient(
                                replacement.script_pub_key,
                                replacement.carrier_value.value)) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                        }
                    }
                    if (!append_recipient(
                            plan.excess_script_pub_key,
                            plan.excess_amount.value)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                    }
                    DDTransferPlan transfer_plan;
                    std::string plan_error;
                    if (!dd_wallet->PlanDigiDollarTransfer(
                            recipients, transfer_plan, plan_error,
                            &plan.source_carriers,
                            /*allow_paymaster_pool_inputs=*/true)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED: " +
                                plan_error);
                    }
                    plan.estimated_fee = DGBSatoshis{
                        transfer_plan.estimated_fee};
                    if (plan.estimated_fee.value <= 0 ||
                        plan.estimated_fee.value >
                            liquidity_policy.maximum_maintenance_fee_per_transaction.value) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
                    }
                }
                plan.plan_id = CarrierWithdrawalPlanId(plan);
                std::string plan_error;
                if (!ValidateProviderCarrierWithdrawalPlan(plan, plan_error) ||
                    !SaveCarrierWithdrawalPreview(*wallet, plan, plan_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        plan_error.empty()
                            ? "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED"
                            : plan_error);
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("executed", false);
                result.pushKV("plan_id", plan.plan_id.GetHex());
                result.pushKV("mode", mode_name);
                result.pushKV("source_carriers", plan.source_carriers.size());
                result.pushKV("expires_at", plan.expires_at);
                if (mode == CarrierWithdrawalMode::ALL_EXCESS) {
                    result.pushKV(
                        "withdrawable_excess_cents",
                        plan.excess_amount.value);
                    result.pushKV(
                        "retained_carrier_cents",
                        static_cast<int64_t>(
                            plan.replacement_carriers.size()) * 100);
                    result.pushKV(
                        "estimated_network_fee_satoshis",
                        plan.estimated_fee.value);
                } else {
                    result.pushKV(
                        "operational_carrier_target",
                        plan.target_operational_carriers_after_release);
                }
                return result;
            }

            const UniValue& plan_id_value = options.find_value("plan_id");
            if (plan_id_value.isNull()) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_ID_REQUIRED");
            }
            const uint256 supplied_plan_id =
                ParseHashV(plan_id_value, "plan_id");
            ProviderCarrierWithdrawalPlan plan;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (!batch.ReadPaymasterCarrierWithdrawalPlan(plan)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_NOT_FOUND");
                }
            }
            if (plan.plan_id != supplied_plan_id || plan.mode != mode ||
                plan.plan_id != CarrierWithdrawalPlanId(plan)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
            }

            // A repeated execution after a successful response loss must
            // return the same terminal result. Check the durable operation
            // before applying preview expiry or policy-revision guards.
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (mode == CarrierWithdrawalMode::ALL_EXCESS) {
                    ProviderMaintenanceLedger terminal_ledger;
                    if (batch.ReadPaymasterMaintenanceLedger(
                            terminal_ledger)) {
                        const auto existing = std::find_if(
                            terminal_ledger.records.begin(),
                            terminal_ledger.records.end(),
                            [&](const ProviderMaintenanceRecord& record) {
                                return record.operation_id ==
                                           plan.operation_id &&
                                       record.plan_id == plan.plan_id;
                            });
                        if (existing != terminal_ledger.records.end() &&
                            (existing->state ==
                                 ProviderMaintenanceState::BROADCAST ||
                             existing->state ==
                                 ProviderMaintenanceState::CONFIRMED)) {
                            UniValue result{UniValue::VOBJ};
                            result.pushKV("executed", true);
                            result.pushKV("plan_id", plan.plan_id.GetHex());
                            result.pushKV("mode", mode_name);
                            result.pushKV(
                                "txid",
                                existing->transaction_id.GetHex());
                            result.pushKV(
                                "withdrawable_excess_cents",
                                plan.excess_amount.value);
                            result.pushKV(
                                "retained_carrier_cents",
                                static_cast<int64_t>(
                                    plan.replacement_carriers.size()) *
                                    100);
                            result.pushKV(
                                "estimated_network_fee_satoshis",
                                existing->actual_fee.value);
                            return result;
                        }
                    } else if (batch.HasPaymasterMaintenanceLedger()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
                    }
                } else {
                    ProviderLiquidityPolicy terminal_policy;
                    std::vector<ProviderPoolEntry> terminal_pool;
                    if (batch.ReadPaymasterLiquidityPolicy(
                            terminal_policy) &&
                        batch.ReadPaymasterProviderPool(terminal_pool)) {
                        const auto source = std::find_if(
                            terminal_pool.begin(), terminal_pool.end(),
                            [&](const ProviderPoolEntry& entry) {
                                return entry.outpoint ==
                                           plan.source_carriers.front() &&
                                       entry.state ==
                                           PoolEntryState::RELEASED;
                            });
                        if (source != terminal_pool.end() &&
                            terminal_policy.target_operational_carriers ==
                                plan.target_operational_carriers_after_release) {
                            UniValue result{UniValue::VOBJ};
                            result.pushKV("executed", true);
                            result.pushKV("plan_id", plan.plan_id.GetHex());
                            result.pushKV("mode", mode_name);
                            result.pushKV(
                                "operational_carrier_target",
                                terminal_policy
                                    .target_operational_carriers);
                            return result;
                        }
                    }
                }
            }
            if (plan.expires_at < now ||
                plan.liquidity_policy_updated_at !=
                    liquidity_policy.updated_at) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
            }

            if (mode == CarrierWithdrawalMode::RELEASE_SLOT) {
                if (provider_is_running()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_PROVIDER_MUST_BE_STOPPED");
                }
                {
                    LOCK(wallet->cs_wallet);
                    WalletBatch batch{wallet->GetDatabase()};
                    ProviderLiquidityPolicy current_policy;
                    ProviderCarrierWithdrawalPlan current_plan;
                    std::vector<ProviderPoolEntry> pool;
                    if (!batch.ReadPaymasterLiquidityPolicy(current_policy) ||
                        !batch.ReadPaymasterCarrierWithdrawalPlan(current_plan) ||
                        !batch.ReadPaymasterProviderPool(pool) ||
                        current_plan.plan_id != plan.plan_id ||
                        current_policy.updated_at !=
                            plan.liquidity_policy_updated_at ||
                        current_policy.target_operational_carriers !=
                            plan.target_operational_carriers_after_release + 1) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
                    }
                    const auto source = std::find_if(
                        pool.begin(), pool.end(),
                        [&](const ProviderPoolEntry& entry) {
                            return entry.outpoint ==
                                   plan.source_carriers.front();
                        });
                    if (source == pool.end() ||
                        source->purpose != PoolPurpose::OPERATIONAL ||
                        source->asset != PoolAsset::DD_CARRIER ||
                        source->state != PoolEntryState::AVAILABLE ||
                        source->confirmation_height <= 0 ||
                        wallet->IsSpent(source->outpoint)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE");
                    }
                    source->state = PoolEntryState::RELEASED;
                    source->reservation_id.SetNull();
                    source->updated_at = now;
                    current_policy.target_operational_carriers =
                        plan.target_operational_carriers_after_release;
                    if (current_policy.updated_at ==
                        std::numeric_limits<int64_t>::max()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_LIQUIDITY_POLICY_TIME_EXHAUSTED");
                    }
                    current_policy.updated_at = std::max(
                        now, current_policy.updated_at + 1);
                    std::string policy_error;
                    if (!ValidateProviderLiquidityPolicy(
                            current_policy, policy_error)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR, policy_error);
                    }
                    if (!batch.TxnBegin()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DATABASE_BEGIN");
                    }
                    if (!batch.WritePaymasterProviderPool(pool) ||
                        !batch.WritePaymasterLiquidityPolicy(
                            current_policy)) {
                        batch.TxnAbort();
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DATABASE_WRITE");
                    }
                    if (!batch.TxnCommit()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DATABASE_COMMIT");
                    }
                    liquidity_policy = current_policy;
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("executed", true);
                result.pushKV("plan_id", plan.plan_id.GetHex());
                result.pushKV("mode", mode_name);
                result.pushKV(
                    "operational_carrier_target",
                    liquidity_policy.target_operational_carriers);
                return result;
            }

            EnsureWalletIsUnlocked(*wallet);
            DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_DD_WALLET_UNAVAILABLE");
            }
            ProviderMaintenanceLedger ledger;
            std::vector<ProviderPoolEntry> pool;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                ProviderLiquidityPolicy current_policy;
                ProviderCarrierWithdrawalPlan current_plan;
                if (!batch.ReadPaymasterLiquidityPolicy(current_policy) ||
                    !batch.ReadPaymasterCarrierWithdrawalPlan(current_plan) ||
                    !batch.ReadPaymasterProviderPool(pool) ||
                    current_plan.plan_id != plan.plan_id ||
                    current_policy.updated_at !=
                        plan.liquidity_policy_updated_at) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
                }
                liquidity_policy = current_policy;
                if (!batch.ReadPaymasterMaintenanceLedger(ledger)) {
                    if (batch.HasPaymasterMaintenanceLedger()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
                    }
                    ledger.accounting_time_high_water = now;
                }
                const auto existing = std::find_if(
                    ledger.records.begin(), ledger.records.end(),
                    [&](const ProviderMaintenanceRecord& record) {
                        return record.operation_id == plan.operation_id;
                    });
                if (existing != ledger.records.end() &&
                    (existing->state == ProviderMaintenanceState::BROADCAST ||
                     existing->state == ProviderMaintenanceState::CONFIRMED)) {
                    UniValue result{UniValue::VOBJ};
                    result.pushKV("executed", true);
                    result.pushKV("plan_id", plan.plan_id.GetHex());
                    result.pushKV("mode", mode_name);
                    result.pushKV(
                        "txid", existing->transaction_id.GetHex());
                    result.pushKV(
                        "withdrawable_excess_cents",
                        plan.excess_amount.value);
                    result.pushKV(
                        "estimated_network_fee_satoshis",
                        existing->actual_fee.value);
                    return result;
                }
            }

            std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
            const auto append_recipient = [&](const CScript& script,
                                              CAmount amount) {
                CTxDestination destination;
                if (!ExtractDestination(script, destination)) return false;
                const std::string encoded =
                    EncodeDigiDollarAddress(destination);
                CDigiDollarAddress address{encoded};
                if (encoded.empty() || !address.IsValid()) return false;
                recipients.emplace_back(std::move(address), amount);
                return true;
            };
            for (const ProviderMaintenanceOutput& replacement :
                 plan.replacement_carriers) {
                if (!append_recipient(
                        replacement.script_pub_key,
                        replacement.carrier_value.value)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                }
            }
            if (!append_recipient(
                    plan.excess_script_pub_key,
                    plan.excess_amount.value)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
            }
            DDTransferPlan transfer_plan;
            std::string transfer_error;
            if (!dd_wallet->PlanDigiDollarTransfer(
                    recipients, transfer_plan, transfer_error,
                    &plan.source_carriers,
                    /*allow_paymaster_pool_inputs=*/true) ||
                transfer_plan.estimated_fee != plan.estimated_fee.value) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    transfer_error.empty()
                        ? "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED"
                        : "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED: " +
                              transfer_error);
            }

            ProviderMaintenanceRecord operation;
            operation.operation_id = plan.operation_id;
            operation.plan_id = plan.plan_id;
            operation.kind =
                ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS;
            operation.outputs = plan.replacement_carriers;
            operation.source_inputs = plan.source_carriers;
            operation.withdrawal_excess_script_pub_key =
                plan.excess_script_pub_key;
            operation.withdrawal_excess_amount = plan.excess_amount;
            operation.maximum_fee =
                liquidity_policy.maximum_maintenance_fee_per_transaction;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                ProviderLiquidityPolicy current_policy;
                ProviderCarrierWithdrawalPlan current_plan;
                ProviderMaintenanceLedger current_ledger;
                std::vector<ProviderPoolEntry> current_pool;
                if (!batch.ReadPaymasterLiquidityPolicy(current_policy) ||
                    !batch.ReadPaymasterCarrierWithdrawalPlan(current_plan) ||
                    !batch.ReadPaymasterProviderPool(current_pool) ||
                    current_plan.plan_id != plan.plan_id ||
                    current_policy.updated_at !=
                        plan.liquidity_policy_updated_at) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
                }
                if (!batch.ReadPaymasterMaintenanceLedger(current_ledger)) {
                    if (batch.HasPaymasterMaintenanceLedger()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
                    }
                    current_ledger.accounting_time_high_water = now;
                }
                operation.maximum_fee =
                    current_policy
                        .maximum_maintenance_fee_per_transaction;
                if (!ReserveProviderMaintenanceBudget(
                        current_ledger, current_policy, operation, now,
                        transfer_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, transfer_error);
                }
                for (const COutPoint& source_outpoint :
                     plan.source_carriers) {
                    const auto source = std::find_if(
                        current_pool.begin(), current_pool.end(),
                        [&](const ProviderPoolEntry& entry) {
                            return entry.outpoint == source_outpoint;
                        });
                    const bool already_reserved =
                        source != current_pool.end() &&
                        source->state == PoolEntryState::RESERVED &&
                        source->reservation_id == plan.operation_id;
                    if (source == current_pool.end() ||
                        source->asset != PoolAsset::DD_CARRIER ||
                        source->confirmation_height <= 0 ||
                        wallet->IsSpent(source_outpoint) ||
                        (source->state != PoolEntryState::AVAILABLE &&
                         !already_reserved)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
                    }
                    source->state = PoolEntryState::RESERVED;
                    source->reservation_id = plan.operation_id;
                    source->updated_at = now;
                }
                if (!MakeProviderPoolRoomForMaintenance(
                        current_pool,
                        plan.replacement_carriers.size(),
                        transfer_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR, transfer_error);
                }
                if (!batch.TxnBegin()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.WritePaymasterMaintenanceLedger(
                        current_ledger) ||
                    !batch.WritePaymasterProviderPool(current_pool)) {
                    batch.TxnAbort();
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_WRITE");
                }
                if (!batch.TxnCommit()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_COMMIT");
                }
                liquidity_policy = current_policy;
                ledger = std::move(current_ledger);
                pool = std::move(current_pool);
            }

            std::string txid_string;
            if (!dd_wallet->TransferDigiDollarMany(
                    recipients, txid_string, transfer_error,
                    /*dd_change_out=*/nullptr,
                    &plan.source_carriers,
                    "Paymaster carrier excess withdrawal",
                    /*allow_paymaster_pool_inputs=*/true,
                    /*exact_plan=*/&transfer_plan)) {
                // The exact sources stay reserved. A retry first reconciles
                // any transaction that may have crossed the commit boundary.
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_CARRIER_WITHDRAWAL_FAILED: " +
                        transfer_error);
            }
            const uint256 txid = uint256S(txid_string);
            CTransactionRef transaction;
            {
                LOCK(wallet->cs_wallet);
                const auto found = wallet->mapWallet.find(txid);
                if (found != wallet->mapWallet.end()) {
                    transaction = found->second.tx;
                }
            }
            if (!transaction) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_MAINTENANCE_TRANSACTION_NOT_IN_WALLET");
            }
            CAmount actual_fee{0};
            {
                LOCK(wallet->cs_wallet);
                const CAmount debit =
                    wallet->GetDebit(*transaction, ISMINE_ALL);
                const CAmount value_out = transaction->GetValueOut();
                if (debit < value_out) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_MAINTENANCE_FEE_INVALID");
                }
                actual_fee = debit - value_out;
            }
            // Treat the reviewed estimate as a maximum.  Exact signing can
            // reduce the serialized size and therefore the actual fee; an
            // increase beyond the preview remains a hard failure.
            if (actual_fee <= 0 ||
                actual_fee > transfer_plan.estimated_fee ||
                actual_fee >
                    liquidity_policy.maximum_maintenance_fee_per_transaction.value) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_MAINTENANCE_FEE_CHANGED");
            }
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (!batch.ReadPaymasterMaintenanceLedger(ledger) ||
                    !batch.ReadPaymasterProviderPool(pool) ||
                    !SpendProviderMaintenanceBudget(
                        ledger, plan.operation_id, txid,
                        DGBSatoshis{actual_fee}, now, transfer_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        transfer_error.empty()
                            ? "PAYMASTER_MAINTENANCE_LEDGER_UPDATE_FAILED"
                            : transfer_error);
                }
                for (const COutPoint& source_outpoint :
                     plan.source_carriers) {
                    const auto source = std::find_if(
                        pool.begin(), pool.end(),
                        [&](const ProviderPoolEntry& entry) {
                            return entry.outpoint == source_outpoint;
                        });
                    if (source == pool.end() ||
                        source->state != PoolEntryState::RESERVED ||
                        source->reservation_id != plan.operation_id) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_WITHDRAWAL_POOL_CONFLICT");
                    }
                    source->state = PoolEntryState::COMMITTED;
                    source->updated_at = now;
                }
                for (const ProviderMaintenanceOutput& replacement :
                     plan.replacement_carriers) {
                    const auto output_index = FindMaintenanceOutputIndex(
                        *transaction, replacement);
                    if (!output_index) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_MAINTENANCE_OUTPUT_MISSING");
                    }
                    ProviderPoolEntry entry;
                    entry.outpoint = COutPoint{txid, *output_index};
                    entry.purpose = replacement.purpose;
                    entry.asset = PoolAsset::DD_CARRIER;
                    entry.state = PoolEntryState::PENDING_SUCCESSOR;
                    entry.script_pub_key = replacement.script_pub_key;
                    entry.carrier_value = replacement.carrier_value;
                    entry.reservation_id = plan.operation_id;
                    entry.origin_commit_key = plan.operation_id;
                    entry.updated_at = now;
                    if (std::none_of(
                            pool.begin(), pool.end(),
                            [&](const ProviderPoolEntry& existing) {
                                return existing.outpoint == entry.outpoint;
                            })) {
                        pool.push_back(std::move(entry));
                    }
                }
                if (!batch.TxnBegin()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.WritePaymasterMaintenanceLedger(ledger) ||
                    !batch.WritePaymasterProviderPool(pool)) {
                    batch.TxnAbort();
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_WRITE");
                }
                if (!batch.TxnCommit()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_COMMIT");
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("executed", true);
            result.pushKV("plan_id", plan.plan_id.GetHex());
            result.pushKV("mode", mode_name);
            result.pushKV("txid", txid.GetHex());
            result.pushKV(
                "withdrawable_excess_cents",
                plan.excess_amount.value);
            result.pushKV(
                "retained_carrier_cents",
                static_cast<int64_t>(
                    plan.replacement_carriers.size()) * 100);
            result.pushKV(
                "estimated_network_fee_satoshis",
                actual_fee);
            return result;
        },
    };
}

namespace {

uint256 PoolPreparationAuthorization(CWallet& wallet)
{
    ProviderIdentityRecord identity;
    ProviderPolicy policy;
    if (!GetPaymasterIdentity(wallet, identity) || !GetPaymasterProviderPolicy(wallet, policy)) return {};
    HashWriter hash = TaggedHash("DigiByte Paymaster Pool Authorization v1");
    hash << Params().GenesisBlock().GetHash() << identity.provider_id << GetProviderPolicyHash(policy);
    return hash.GetSHA256();
}

ProviderMaintenanceLedger ReadPreparationLedger(CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    ProviderMaintenanceLedger ledger;
    const auto status = WalletBatch{wallet.GetDatabase()}.ReadPaymasterMaintenanceLedgerWithStatus(ledger);
    if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
    }
    return ledger;
}

void WritePreparationLedger(CWallet& wallet, const ProviderMaintenanceLedger& ledger)
{
    LOCK(wallet.cs_wallet);
    if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterMaintenanceLedger(ledger)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_DATABASE_WRITE");
    }
}

bool PreparationInputsConfirmed(CWallet& wallet, const CTransaction& tx)
{
    AssertLockHeld(wallet.cs_wallet);
    return std::all_of(tx.vin.begin(), tx.vin.end(), [&](const CTxIn& input) {
        const auto parent = wallet.mapWallet.find(input.prevout.hash);
        return parent != wallet.mapWallet.end() && wallet.GetTxDepthInMainChain(parent->second) > 0;
    });
}

std::string ExecutePreparationStep(CWallet& wallet, const ProviderMaintenanceRecord& record)
{
    LOCK(wallet.cs_wallet);
    ProviderSettings settings;
    if (!GetPaymasterProviderSettings(wallet, settings) || !settings.enabled) return "PAYMASTER_PROVIDER_DISABLED";
    if (record.preparation_authorization != PoolPreparationAuthorization(wallet)) return "PAYMASTER_POOL_POLICY_CHANGED";
    if (!wallet.GetBroadcastTransactions()) return "PAYMASTER_WALLET_BROADCAST_DISABLED";
    if (!record.transaction_id.IsNull()) {
        const auto found = wallet.mapWallet.find(record.transaction_id);
        if (found == wallet.mapWallet.end()) return "PAYMASTER_POOL_TRANSACTION_NOT_IN_WALLET";
        const auto& wtx = found->second;
        if (wtx.isAbandoned() || wtx.isConflicted()) return "PAYMASTER_POOL_TRANSACTION_CONFLICT";
        if (wallet.GetTxDepthInMainChain(wtx) > 0) return {};
        if (wtx.InMempool() || !PreparationInputsConfirmed(wallet, *wtx.tx)) return "PAYMASTER_POOL_WAITING_CONFIRMATION";
        const auto fee = GetProviderFinanceTransactionFee(wallet, wtx.tx);
        if (!fee || fee->value > record.maximum_fee.value) return "PAYMASTER_POOL_FEE_LIMIT";
        std::string error;
        // Retry only the exact transaction already saved by CommitTransaction.
        if (!wallet.chain().broadcastTransaction(wtx.tx, std::min(record.maximum_fee.value, wallet.m_default_max_tx_fee), true, error)) {
            return "PAYMASTER_POOL_TRANSACTION_REJECTED: " + error;
        }
        return "PAYMASTER_POOL_WAITING_CONFIRMATION";
    }
    if (wallet.IsLocked()) return "PAYMASTER_WALLET_LOCKED";
    if (record.kind == ProviderMaintenanceKind::PREPARE_DGB) {
        std::vector<CRecipient> recipients;
        for (const auto& output : record.outputs) {
            CTxDestination destination;
            if (!ExtractDestination(output.script_pub_key, destination)) return "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            recipients.push_back({destination, output.dgb_value.value, false});
        }
        // A stempool-only parent is invisible to the normal fee precheck.
        // Constrain setup funding without changing ordinary wallet selection.
        CCoinControl control;
        control.m_min_depth = 1;
        auto created = CreateTransaction(wallet, recipients, -1, control, true);
        if (!created) return "PAYMASTER_POOL_WAITING_DGB: " + util::ErrorString(created).original;
        if (created->fee > record.maximum_fee.value) return "PAYMASTER_POOL_FEE_LIMIT";
        std::string error;
        if (!wallet.CommitTransaction(created->tx, {}, {}, &error)) return "PAYMASTER_POOL_TRANSACTION_REJECTED: " + error;
    } else {
        auto* dd_wallet = wallet.GetDDWallet();
        if (!dd_wallet) return "PAYMASTER_DD_WALLET_UNAVAILABLE";
        std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
        for (const auto& output : record.outputs) {
            CTxDestination destination;
            if (!ExtractDestination(output.script_pub_key, destination)) return "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            recipients.emplace_back(CDigiDollarAddress{EncodeDigiDollarAddress(destination)}, output.carrier_value.value);
        }
        DDTransferPlan plan;
        std::string error;
        if (!dd_wallet->PlanDigiDollarTransfer(recipients, plan, error)) return "PAYMASTER_POOL_WAITING_FUNDS: " + error;
        if (plan.estimated_fee <= 0 || plan.estimated_fee > record.maximum_fee.value) return "PAYMASTER_POOL_FEE_LIMIT";
        CMutableTransaction inputs;
        for (const auto& input : plan.dd_utxos) inputs.vin.emplace_back(input);
        for (const auto& input : plan.fee_utxos) inputs.vin.emplace_back(input);
        if (!PreparationInputsConfirmed(wallet, CTransaction{inputs})) return "PAYMASTER_POOL_WAITING_CONFIRMATION";
        CMutableTransaction transaction;
        if (!dd_wallet->BuildDigiDollarTransfer(plan, transaction, error)) {
            return "PAYMASTER_CARRIER_TRANSACTION_FAILED: " + error;
        }
        const CTransactionRef tx = MakeTransactionRef(std::move(transaction));
        const auto fee = GetProviderFinanceTransactionFee(wallet, tx);
        if (!fee || fee->value <= 0 || fee->value > record.maximum_fee.value || fee->value > plan.estimated_fee) {
            return "PAYMASTER_POOL_FEE_LIMIT";
        }
        // Check the exact signed fee before commit. Use the wallet primitive
        // directly: a rejected pool transaction must remain recoverable, not
        // enter the ordinary DD send helper's automatic-abandon path.
        if (!wallet.CommitTransaction(tx, {{"comment", "Paymaster carrier pool"}}, {}, &error)) {
            return "PAYMASTER_CARRIER_TRANSACTION_FAILED: " + error;
        }
    }
    return {};
}

// Caller owns the work guard shared with recurring maintenance. Recover wallet
// bytes before every construction, including a commit that failed to broadcast.
void ContinuePoolPreparation(CWallet& wallet)
{
    // Readiness queries also reconcile this journal. Keep their updates out of
    // the read-modify-write interval even though they do not take the work guard.
    LOCK(wallet.cs_wallet);
    size_t recovered{0};
    std::string error;
    if (!ReconcileProviderMaintenance(wallet, recovered, error)) throw JSONRPCError(RPC_WALLET_ERROR, error);
    const auto snapshot = ReadPreparationLedger(wallet);
    for (const auto& record : snapshot.records) {
        if (!record.IsPreparation() || record.state == ProviderMaintenanceState::CONFIRMED || record.state == ProviderMaintenanceState::RELEASED) continue;
        const bool conflict = std::any_of(snapshot.records.begin(), snapshot.records.end(), [&](const auto& other) {
            return other.IsPreparation() && other.plan_id == record.plan_id && other.state == ProviderMaintenanceState::FAILED;
        });
        error = conflict ? "PAYMASTER_POOL_TRANSACTION_CONFLICT" : ExecutePreparationStep(wallet, record);
        size_t reconciled{0};
        std::string reconcile_error;
        if (!ReconcileProviderMaintenance(wallet, reconciled, reconcile_error)) throw JSONRPCError(RPC_WALLET_ERROR, reconcile_error);
        auto ledger = ReadPreparationLedger(wallet);
        for (auto& current : ledger.records) {
            if (current.operation_id == record.operation_id && current.preparation_error != error.substr(0, 256)) {
                current.preparation_error = error.substr(0, 256);
                current.updated_at = std::max(current.updated_at, GetTime());
                WritePreparationLedger(wallet, ledger);
                break;
            }
        }
    }
}

} // namespace

bool paymaster_rpc::internal::HasPendingPoolPreparation(CWallet& wallet)
{
    const auto ledger = ReadPreparationLedger(wallet);
    return std::any_of(ledger.records.begin(), ledger.records.end(), [](const auto& record) {
        return record.IsPreparation() && record.state != ProviderMaintenanceState::CONFIRMED &&
               record.state != ProviderMaintenanceState::RELEASED;
    });
}

RPCResult paymaster_rpc::internal::PoolPreparationResult()
{
    return {RPCResult::Type::ARR, "preparation", "Durable finite setup steps; no provider start is implied", {
        {RPCResult::Type::OBJ, "", "Setup step", {
            {RPCResult::Type::STR_HEX, "plan_id", "Approved plan"},
            {RPCResult::Type::STR_HEX, "operation_id", "Durable step identity"},
            {RPCResult::Type::STR, "asset", "dgb or dd_carrier"},
            {RPCResult::Type::STR, "state", "pending_creation, pending_confirmation, complete, conflict or cancelled"},
            {RPCResult::Type::STR, "error", "Last execution diagnostic; empty when complete"},
            {RPCResult::Type::NUM, "maximum_fee_satoshis", "Approved maximum transaction fee"},
            {RPCResult::Type::NUM, "actual_fee_satoshis", "Saved transaction fee, zero before construction"},
            {RPCResult::Type::STR_HEX, "txid", true, "Existing transaction; never replaced by a retry"},
            {RPCResult::Type::NUM, "required_output_satoshis", true, "Uncreated DGB pool principal"},
            {RPCResult::Type::NUM, "confirmed_available_satoshis", true, "Eligible confirmed DGB funding"},
            {RPCResult::Type::NUM, "available_including_unconfirmed_satoshis", true, "Eligible funding including safe unconfirmed change"},
            {RPCResult::Type::NUM, "funding_shortfall_upper_bound_satoshis", true, "Shortfall including the full fee ceiling, after unconfirmed change is counted; actual fee may be lower"},
        }},
    }};
}

UniValue paymaster_rpc::internal::PoolPreparationToJSON(CWallet& wallet)
{
    UniValue result{UniValue::VARR};
    for (const auto& record : ReadPreparationLedger(wallet).records) {
        if (!record.IsPreparation()) continue;
        UniValue item{UniValue::VOBJ};
        item.pushKV("plan_id", record.plan_id.GetHex());
        item.pushKV("operation_id", record.operation_id.GetHex());
        item.pushKV("asset", record.kind == ProviderMaintenanceKind::PREPARE_DGB ? "dgb" : "dd_carrier");
        item.pushKV("state", record.state == ProviderMaintenanceState::CONFIRMED ? "complete" : record.state == ProviderMaintenanceState::FAILED ? "conflict" : record.state == ProviderMaintenanceState::RELEASED ? "cancelled" : record.transaction_id.IsNull() ? "pending_creation" : "pending_confirmation");
        item.pushKV("error", record.state == ProviderMaintenanceState::CONFIRMED ? "" : record.preparation_error);
        item.pushKV("maximum_fee_satoshis", record.maximum_fee.value);
        item.pushKV("actual_fee_satoshis", record.actual_fee.value);
        if (!record.transaction_id.IsNull()) item.pushKV("txid", record.transaction_id.GetHex());
        if (record.kind == ProviderMaintenanceKind::PREPARE_DGB && record.transaction_id.IsNull()) {
            LOCK(wallet.cs_wallet);
            CAmount required{0};
            for (const auto& output : record.outputs) required += output.dgb_value.value;
            CCoinControl control;
            control.m_min_depth = 1;
            const CAmount confirmed = AvailableCoins(wallet, &control).GetTotalAmount();
            control.m_min_depth = 0;
            const CAmount available = AvailableCoins(wallet, &control).GetTotalAmount();
            item.pushKV("required_output_satoshis", required);
            item.pushKV("confirmed_available_satoshis", confirmed);
            item.pushKV("available_including_unconfirmed_satoshis", available);
            item.pushKV("funding_shortfall_upper_bound_satoshis", std::max<CAmount>(0, required + record.maximum_fee.value - available));
        }
        result.push_back(std::move(item));
    }
    return result;
}

void RunPaymasterPoolPreparation(WalletContext& context, CWallet& wallet)
{
    if (!context.paymaster || !context.paymaster->Enabled()) return;
    ProviderIdentityRecord identity;
    ProviderSettings settings;
    if (!GetPaymasterIdentity(wallet, identity) || !GetPaymasterProviderSettings(wallet, settings) || !settings.enabled || !AutomaticProviderStateIsSynchronized(wallet)) return;
    ProviderWorkGuard guard{*context.paymaster, wallet.GetName(), identity.provider_id, false};
    if (!guard.Acquired()) return;
    try {
        ContinuePoolPreparation(wallet);
    } catch (const UniValue& error) {
        wallet.WalletLogPrintf("Paymaster pool preparation paused: %s\n", error.write());
    } catch (const std::exception& error) {
        wallet.WalletLogPrintf("Paymaster pool preparation paused: %s\n", error.what());
    }
}

RPCHelpMan preparepaymasterpool()
{
    return RPCHelpMan{
        "preparepaymasterpool",
        "Preview or create dedicated P2TR DGB and DD-carrier outputs for the Paymaster admission and operational pools.\n"
        "The default is a side-effect-free preview. Execution requires the unchanged plan_id returned by that preview.\n"
        "Existing live entries count toward the requested totals, so an interrupted preparation can be resumed safely.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Pool target and explicit execution approval", {
                                                                                                                    {"admission_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Three to sixteen admission DGB slots"},
                                                                                                                    {"operational_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "One to sixteen operational DGB slots"},
                                                                                                                    {"admission_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Three to sixteen admission carriers for USER_PAID"},
                                                                                                                    {"operational_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "One to sixteen operational carriers for USER_PAID"},
                                                                                                                    {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Persist the reviewed finite setup and automatically continue it when funding is confirmed"},
                                                                                                                    {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged preview plan id required for execution"},
                {"cancel", RPCArg::Type::BOOL, RPCArg::Default{false}, "With execute and the original plan_id, cancel only steps with no saved transaction; signed transactions remain tracked"},
                {"maximum_fee_satoshis", RPCArg::Type::NUM, RPCArg::Default{20000000}, "Maximum fee per setup transaction; at most one DGB and one DD transaction are authorized"},
                {"recover_dgb_txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Explicitly adopt an existing wallet DGB transaction; never discover it from labels"},
                {"recover_dgb_outputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Exact outputs of the recovery transaction", {
                    {"output", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Pool output", {
                        {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "Output index"},
                        {"purpose", RPCArg::Type::STR, RPCArg::Optional::NO, "admission or operational"},
                    }},
                }},
                                                                                                                }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Pool preparation preview or transaction", {
                PoolPreparationResult(),
                                                                                           {RPCResult::Type::BOOL, "executed", "Whether this call committed a wallet transaction"},
                                                                                           {RPCResult::Type::BOOL, "cancelled", "Whether unsigned steps were cancelled"},
                {RPCResult::Type::BOOL, "accepted", "Whether a finite automatic setup was authorized"},
                {RPCResult::Type::NUM, "maximum_fee_satoshis", "Maximum approved fee per transaction"},
                {RPCResult::Type::NUM, "maximum_total_fee_satoshis", "Maximum approved fees for the new setup steps"},
                {RPCResult::Type::STR_HEX, "plan_id", "Plan bound to the policy, targets, current deficits, and output values"},
                                                                                           {RPCResult::Type::NUM, "admission_dgb_slots", "Admission DGB outputs"},
                                                                                           {RPCResult::Type::NUM, "operational_dgb_slots", "Operational DGB outputs"},
                                                                                           {RPCResult::Type::NUM, "admission_carrier_slots", "Admission DD carriers"},
                                                                                           {RPCResult::Type::NUM, "operational_carrier_slots", "Operational DD carriers"},
                                                                                           {RPCResult::Type::NUM, "missing_admission_dgb_slots", "Admission DGB outputs still to create"},
                                                                                           {RPCResult::Type::NUM, "missing_operational_dgb_slots", "Operational DGB outputs still to create"},
                                                                                           {RPCResult::Type::NUM, "missing_admission_carrier_slots", "Admission carriers still to create"},
                                                                                           {RPCResult::Type::NUM, "missing_operational_carrier_slots", "Operational carriers still to create"},
                                                                                           {RPCResult::Type::NUM, "admission_dgb_satoshis_each", "Value of every admission output"},
                                                                                           {RPCResult::Type::NUM, "operational_dgb_satoshis_each", "Value of every operational output"},
                                                                                           {RPCResult::Type::NUM, "carrier_cents_each", "Value of every DD carrier"},
                                                                                           {RPCResult::Type::NUM, "total_output_satoshis", "Total value assigned to pool outputs"},
                                                                                           {RPCResult::Type::NUM, "total_carrier_cents", "Total DD assigned to new carriers"},
                                                                                           {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Committed pool transaction"},
                                                                                           {RPCResult::Type::STR_HEX, "dd_txid", /*optional=*/true, "Committed DD carrier transaction"},
                                                                                           {RPCResult::Type::STR_HEX, "dgb_txid", /*optional=*/true, "Committed DGB pool transaction"},
                                                                                           {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "Operator-paid transaction fee"},
                                                                                           {RPCResult::Type::ARR, "pool", /*optional=*/true, "Persisted unconfirmed pool entries", {{RPCResult::Type::OBJ, "", "Pool entry", {
                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR, "purpose", "admission or operational"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR, "asset", "dgb or dd_carrier"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR, "state", "available"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "dgb_satoshis", "Output value"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "dd_cents", "Carrier value or zero for DGB entries"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "confirmation_height", "Zero until confirmed"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "reservation_id", /*optional=*/true, "Durable reservation"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "origin_commit_key", /*optional=*/true, "Commit or maintenance operation that created this successor"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM_TIME, "updated_at", "Persistence time"},
                                                                                                                                                                                                                             }}}},
                                                                                       }},
        RPCExamples{HelpExampleCli("preparepaymasterpool", "'{\"admission_dgb_slots\":3,\"operational_dgb_slots\":1,\"admission_carrier_slots\":3,\"operational_carrier_slots\":1}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            ProviderIdentityRecord identity;
            std::optional<ProviderWorkGuard> pool_guard;
            if (context.paymaster && context.paymaster->Enabled() &&
                GetPaymasterIdentity(*wallet, identity)) {
                // Manual previews and executions share the scheduler's
                // wallet-local exclusion slot. This prevents two callers, or
                // automatic replenishment and an expert action, from deriving
                // and committing changes from the same stale pool snapshot.
                pool_guard.emplace(
                    *context.paymaster, wallet->GetName(),
                    identity.provider_id, /*require_running=*/false);
                if (!pool_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
            }
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"admission_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"operational_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"admission_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"operational_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"execute", UniValueType(UniValue::VBOOL)},
                             {"plan_id", UniValueType(UniValue::VSTR)},
                             {"cancel", UniValueType(UniValue::VBOOL)},
                             {"maximum_fee_satoshis", UniValueType(UniValue::VNUM)},
                             {"recover_dgb_txid", UniValueType(UniValue::VSTR)},
                             {"recover_dgb_outputs", UniValueType(UniValue::VARR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const int admission = options.find_value("admission_dgb_slots").getInt<int>();
            const int operational = options.find_value("operational_dgb_slots").getInt<int>();
            const int admission_carriers = options.find_value("admission_carrier_slots").isNull() ? 0 : options.find_value("admission_carrier_slots").getInt<int>();
            const int operational_carriers = options.find_value("operational_carrier_slots").isNull() ? 0 : options.find_value("operational_carrier_slots").getInt<int>();
            const bool execute = !options.find_value("execute").isNull() &&
                                 options.find_value("execute").get_bool();
            if (admission < static_cast<int>(REQUIRED_ADMISSION_SLOTS) || admission > 16 ||
                operational < 1 || operational > 16 || admission_carriers < 0 ||
                admission_carriers > 16 || operational_carriers < 0 || operational_carriers > 16) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_INVALID_POOL_TARGET");
            }
            const int64_t fee_limit = options.find_value("maximum_fee_satoshis").isNull()
                ? 20000000 : options.find_value("maximum_fee_satoshis").getInt<int64_t>();
            if (fee_limit <= 0 || !MoneyRange(fee_limit) || fee_limit > MAX_MONEY / 2) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_INVALID_FEE_LIMIT");
            }
            ProviderPolicy policy;
            if (!GetPaymasterProviderPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POLICY_NOT_FOUND");
            }
            const bool user_paid = PolicyAllowsFundingModel(policy, FundingModel::USER_PAID);
            if (user_paid && (admission_carriers < static_cast<int>(REQUIRED_ADMISSION_SLOTS) ||
                              operational_carriers < 1)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
            }
            if (!user_paid && (admission_carriers != 0 || operational_carriers != 0)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SPONSORED_POOL_HAS_CARRIERS");
            }
            std::vector<ProviderPoolEntry> existing;
            std::string refresh_error;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (!ReadOptionalProviderPool(batch, existing, refresh_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, refresh_error);
                }
            }
            if (!existing.empty() &&
                !RefreshPoolConfirmationHeights(*wallet, existing, refresh_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, refresh_error);
            }
            constexpr int64_t admission_value{MIN_ADMISSION_DGB_SATOSHIS};
            constexpr int64_t carrier_value{100};
            const int64_t operational_value = std::max<int64_t>(
                MIN_ADMISSION_DGB_SATOSHIS, policy.maximum_network_fee.value);
            const auto live_count = [&](PoolPurpose purpose, PoolAsset asset,
                                        int64_t minimum_dgb_value = 0) {
                return static_cast<int>(std::count_if(existing.begin(), existing.end(),
                                                      [&](const ProviderPoolEntry& entry) {
                                                          return entry.purpose == purpose && entry.asset == asset &&
                                                                 (asset != PoolAsset::DGB ||
                                                                  entry.dgb_value.value >= minimum_dgb_value) &&
                                                                 (entry.state == PoolEntryState::AVAILABLE ||
                                                                  entry.state == PoolEntryState::RESERVED ||
                                                                  entry.state == PoolEntryState::PENDING_SUCCESSOR);
                                                      }));
            };
            const int missing_admission = std::max(
                0, admission - live_count(PoolPurpose::ADMISSION,
                                          PoolAsset::DGB, admission_value));
            const int missing_operational = std::max(
                0, operational - live_count(PoolPurpose::OPERATIONAL,
                                            PoolAsset::DGB,
                                            operational_value));
            const int missing_admission_carriers = std::max(0, admission_carriers - live_count(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER));
            const int missing_operational_carriers = std::max(0, operational_carriers - live_count(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER));
            const auto checked_product = [](int64_t value, int count) {
                if (value < 0 || count < 0 ||
                    (count != 0 &&
                     value > std::numeric_limits<int64_t>::max() / count)) {
                    return std::optional<int64_t>{};
                }
                return std::optional<int64_t>{value * count};
            };
            const auto admission_total = checked_product(
                admission_value, missing_admission);
            const auto operational_total = checked_product(
                operational_value, missing_operational);
            std::optional<int64_t> total;
            if (admission_total && operational_total) {
                total = CheckedAdd(*admission_total, *operational_total);
            }
            // This preview feeds one wallet transaction. Reject arithmetic
            // overflow and aggregate values outside DigiByte's monetary range
            // before deriving destinations or unlocking the wallet.
            if (!total || !MoneyRange(*total)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_POOL_VALUE_OUT_OF_RANGE");
            }
            // Index synchronization may drain wallet validation callbacks.
            // Never wait for that queue while holding cs_wallet.
            const bool txindex_ready = !execute || ProviderTxIndexIsReady(*wallet, /*wait_for_sync=*/true);
            LOCK(wallet->cs_wallet);
            auto ledger = ReadPreparationLedger(*wallet);
            const uint256 authorization = PoolPreparationAuthorization(*wallet);
            if (authorization.IsNull()) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_IDENTITY_NOT_FOUND");
            const bool pending = std::any_of(ledger.records.begin(), ledger.records.end(), [](const auto& record) {
                return record.IsPreparation() && record.state != ProviderMaintenanceState::CONFIRMED &&
                       record.state != ProviderMaintenanceState::RELEASED;
            });
            HashWriter request_hasher = TaggedHash("DigiByte Paymaster Pool Request v1");
            request_hasher << admission << operational << admission_carriers << operational_carriers << fee_limit
                           << options.find_value("recover_dgb_txid").write()
                           << options.find_value("recover_dgb_outputs").write();
            const uint256 request_hash = request_hasher.GetSHA256();
            const uint256 requested_plan = options.find_value("plan_id").isNull()
                ? uint256{} : ParseHashV(options.find_value("plan_id"), "plan_id");
            const bool replay = execute && std::any_of(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
                return record.IsPreparation() && record.plan_id == requested_plan && record.preparation_request == request_hash &&
                       record.preparation_authorization == authorization;
            });
            CTransactionRef recovery_tx;
            std::vector<ProviderMaintenanceOutput> recovery_outputs;
            const auto& recovery_id = options.find_value("recover_dgb_txid");
            const auto& recovery_mapping = options.find_value("recover_dgb_outputs");
            if (recovery_id.isNull() != recovery_mapping.isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_MAPPING_REQUIRED");
            }
            if (!recovery_id.isNull() && !replay) {
                LOCK(wallet->cs_wallet);
                const auto found = wallet->mapWallet.find(ParseHashV(recovery_id, "recover_dgb_txid"));
                if (found == wallet->mapWallet.end() || !found->second.tx ||
                    found->second.isAbandoned() || found->second.isConflicted() ||
                    IsDigiDollarTransaction(*found->second.tx)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_TRANSACTION_INVALID");
                }
                recovery_tx = found->second.tx;
                // Adoption authorizes only a wallet-owned self-transfer. It must
                // not turn a foreign, client, or provider payment into setup.
                for (const auto& input : recovery_tx->vin) {
                    if (!(wallet->IsMine(input.prevout) & ISMINE_SPENDABLE) || IsPaymasterInputReserved(*wallet, input.prevout) ||
                        std::any_of(existing.begin(), existing.end(), [&](const auto& entry) { return entry.outpoint == input.prevout; })) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_INPUT_INVALID");
                    }
                }
                for (const auto& output : recovery_tx->vout) {
                    if (!(wallet->IsMine(output) & ISMINE_SPENDABLE)) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_OUTPUT_INVALID");
                }
                int recovered_admission{0}, recovered_operational{0};
                std::set<uint32_t> indices;
                for (const auto& item : recovery_mapping.getValues()) {
                    RPCTypeCheckObj(item, {{"vout", UniValueType(UniValue::VNUM)}, {"purpose", UniValueType(UniValue::VSTR)}}, false, true);
                    const auto index = item.find_value("vout").getInt<uint32_t>();
                    const auto purpose = item.find_value("purpose").get_str();
                    if (index >= recovery_tx->vout.size() || !indices.insert(index).second ||
                        (purpose != "admission" && purpose != "operational") ||
                        wallet->IsSpent(COutPoint{recovery_tx->GetHash(), index})) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_OUTPUT_INVALID");
                    }
                    const auto& output = recovery_tx->vout[index];
                    const bool admission_output = purpose == "admission";
                    if (output.nValue != (admission_output ? admission_value : operational_value)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_VALUE_MISMATCH");
                    }
                    ProviderMaintenanceOutput mapped;
                    mapped.purpose = admission_output ? PoolPurpose::ADMISSION : PoolPurpose::OPERATIONAL;
                    mapped.asset = PoolAsset::DGB;
                    mapped.script_pub_key = output.scriptPubKey;
                    mapped.dgb_value = DGBSatoshis{output.nValue};
                    recovery_outputs.push_back(std::move(mapped));
                    admission_output ? ++recovered_admission : ++recovered_operational;
                }
                if (recovered_admission != missing_admission || recovered_operational != missing_operational || recovery_outputs.empty()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_TARGET_MISMATCH");
                }
                const auto fee = GetProviderFinanceTransactionFee(*wallet, recovery_tx);
                if (!fee || fee->value > fee_limit) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_FEE_LIMIT");
            }
            const int64_t total_carriers = carrier_value *
                                           (missing_admission_carriers + missing_operational_carriers);
            HashWriter plan_hasher = TaggedHash(
                "DigiByte Paymaster Pool Preparation Plan v1");
            plan_hasher << Params().GenesisBlock().GetHash()
                        << GetProviderPolicyHash(policy)
                        << admission << operational
                        << admission_carriers << operational_carriers
                        << missing_admission << missing_operational
                        << missing_admission_carriers
                        << missing_operational_carriers
                        << admission_value << operational_value
                        << carrier_value << *total << total_carriers;
            // Bind the existing inventory as well as deficit counts. After
            // slots are spent, a new explicit setup must not alias an old,
            // completed command with the same targets and numeric deficits.
            for (const auto& entry : existing) {
                plan_hasher << entry.outpoint << static_cast<uint8_t>(entry.state)
                            << static_cast<uint8_t>(entry.purpose) << static_cast<uint8_t>(entry.asset)
                            << entry.dgb_value.value << entry.carrier_value.value;
            }
            for (const auto& record : ledger.records) {
                if (record.IsPreparation()) plan_hasher << record.operation_id << static_cast<uint8_t>(record.state);
            }
            plan_hasher << fee_limit << authorization << recovery_outputs;
            if (recovery_tx) plan_hasher << recovery_tx->GetWitnessHash();
            // A lost execute response must be recoverable by another preview.
            // Reuse only an outstanding plan with the same request and current
            // authorization; changed targets/policy never inherit its approval.
            const auto pending_match = std::find_if(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
                return record.IsPreparation() && record.state != ProviderMaintenanceState::CONFIRMED &&
                       record.state != ProviderMaintenanceState::RELEASED &&
                       record.preparation_request == request_hash && record.preparation_authorization == authorization;
            });
            const bool resume_preview = !execute && pending_match != ledger.records.end();
            const uint256 plan_id = resume_preview ? pending_match->plan_id : plan_hasher.GetSHA256();
            UniValue result{UniValue::VOBJ};
            result.pushKV("plan_id", plan_id.GetHex());
            result.pushKV("admission_dgb_slots", admission);
            result.pushKV("operational_dgb_slots", operational);
            result.pushKV("admission_carrier_slots", admission_carriers);
            result.pushKV("operational_carrier_slots", operational_carriers);
            result.pushKV("missing_admission_dgb_slots", missing_admission);
            result.pushKV("missing_operational_dgb_slots", missing_operational);
            result.pushKV("missing_admission_carrier_slots", missing_admission_carriers);
            result.pushKV("missing_operational_carrier_slots", missing_operational_carriers);
            result.pushKV("admission_dgb_satoshis_each", admission_value);
            result.pushKV("operational_dgb_satoshis_each", operational_value);
            result.pushKV("carrier_cents_each", carrier_value);
            result.pushKV("total_output_satoshis", *total);
            result.pushKV("total_carrier_cents", total_carriers);
            result.pushKV("maximum_fee_satoshis", fee_limit);
            int64_t total_fee_limit = fee_limit *
                ((missing_admission + missing_operational > 0) +
                 (missing_admission_carriers + missing_operational_carriers > 0));
            if (replay || resume_preview) {
                // Report the original authorization, even after one or both
                // asset steps no longer appear in the current deficit counts.
                total_fee_limit = 0;
                for (const auto& record : ledger.records) {
                    if (!record.IsPreparation() || record.plan_id != (replay ? requested_plan : plan_id)) continue;
                    const auto combined = CheckedAdd(total_fee_limit, record.maximum_fee.value);
                    if (!combined || !MoneyRange(*combined)) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
                    total_fee_limit = *combined;
                }
            }
            result.pushKV("maximum_total_fee_satoshis", total_fee_limit);
            result.pushKV("accepted", false);
            result.pushKV("cancelled", false);
            result.pushKV("preparation", PoolPreparationToJSON(*wallet));
            if (!execute) {
                result.pushKV("executed", false);
                return result;
            }

            const UniValue& supplied_plan = options.find_value("plan_id");
            if (supplied_plan.isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_POOL_PLAN_REQUIRED");
            }
            const uint256 supplied_id = ParseHashV(supplied_plan, "plan_id");
            if (options.find_value("cancel").isTrue()) {
                size_t recovered{0};
                std::string error;
                if (!ReconcileProviderMaintenance(*wallet, recovered, error)) throw JSONRPCError(RPC_WALLET_ERROR, error);
                ledger = ReadPreparationLedger(*wallet);
                bool found{false}, cancelled{false};
                for (auto& record : ledger.records) {
                    if (!record.IsPreparation() || record.plan_id != supplied_id) continue;
                    found = true;
                    // A wallet-saved signature is never cancelled or replaced,
                    // even if its transaction is absent from both pools.
                    if (record.state == ProviderMaintenanceState::PLANNED && record.transaction_id.IsNull()) {
                        record.state = ProviderMaintenanceState::RELEASED;
                        record.updated_at = std::max(record.updated_at, GetTime());
                        record.preparation_error.clear();
                        cancelled = true;
                    }
                }
                if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_PLAN_NOT_FOUND");
                if (cancelled) WritePreparationLedger(*wallet, ledger);
                result.pushKV("cancelled", cancelled);
                result.pushKV("executed", false);
                result.pushKV("plan_id", supplied_id.GetHex());
                result.pushKV("preparation", PoolPreparationToJSON(*wallet));
                return result;
            }
            const bool already_authorized = replay;
            if (already_authorized && std::any_of(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
                    return record.IsPreparation() && record.plan_id == supplied_id && record.state == ProviderMaintenanceState::RELEASED;
                })) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_PLAN_CANCELLED");
            if (!already_authorized && supplied_id != plan_id) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_PLAN_CHANGED");
            if (pending && !already_authorized) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_PREPARATION_PENDING");
            if (!context.paymaster || !context.paymaster->Enabled() || !pool_guard || !pool_guard->Acquired()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_DISABLED");
            }
            ProviderSettings settings;
            if (!GetPaymasterProviderSettings(*wallet, settings)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_SETTINGS_UNAVAILABLE");
            }
            // Approval is durable while disabled; execution still checks the
            // enabled flag. The setup wizard saves this approval before it
            // applies the operator's final runtime and enabled settings.
            if (!already_authorized) {
                const size_t new_steps = (missing_admission + missing_operational > 0) +
                                         (missing_admission_carriers + missing_operational_carriers > 0);
                if (ledger.records.size() + new_steps > 256) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_MAINTENANCE_RECORD_LIMIT");
                EnsureWalletIsUnlocked(*wallet);
                const auto append = [&](PoolAsset asset, int admission_count, int operational_count) {
                    if (admission_count + operational_count == 0) return;
                    ProviderMaintenanceRecord record;
                    do { record.operation_id = GetRandHash(); } while (record.operation_id.IsNull());
                    record.plan_id = plan_id;
                    record.kind = asset == PoolAsset::DGB ? ProviderMaintenanceKind::PREPARE_DGB : ProviderMaintenanceKind::PREPARE_CARRIER;
                    record.maximum_fee = DGBSatoshis{fee_limit};
                    record.preparation_authorization = authorization;
                    record.preparation_request = request_hash;
                    if (!settings.enabled) record.preparation_error = "PAYMASTER_PROVIDER_DISABLED";
                    record.created_at = record.updated_at = GetTime();
                    if (asset == PoolAsset::DGB && recovery_tx) {
                        record.outputs = recovery_outputs;
                        record.transaction_id = recovery_tx->GetHash();
                        record.actual_fee = *GetProviderFinanceTransactionFee(*wallet, recovery_tx);
                        record.state = ProviderMaintenanceState::BROADCAST;
                    } else {
                        for (int i = 0; i < admission_count + operational_count; ++i) {
                            const bool admission_output = i < admission_count;
                            auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                asset == PoolAsset::DGB ? "Paymaster pool setup" : "Paymaster carrier pool setup");
                            if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                            ProviderMaintenanceOutput output;
                            output.asset = asset;
                            output.purpose = admission_output ? PoolPurpose::ADMISSION : PoolPurpose::OPERATIONAL;
                            output.script_pub_key = GetScriptForDestination(*destination);
                            if (asset == PoolAsset::DGB) output.dgb_value = DGBSatoshis{admission_output ? admission_value : operational_value};
                            else output.carrier_value = DDCents{carrier_value};
                            record.outputs.push_back(std::move(output));
                        }
                    }
                    ledger.records.push_back(std::move(record));
                };
                // Save both exact output plans before either transaction can be
                // committed. Only the existing wallet stores signed bytes.
                append(PoolAsset::DD_CARRIER, missing_admission_carriers, missing_operational_carriers);
                append(PoolAsset::DGB, missing_admission, missing_operational);
                WritePreparationLedger(*wallet, ledger);
            }
            std::set<uint256> prior_transactions;
            for (const auto& record : ledger.records) {
                if (!record.transaction_id.IsNull()) prior_transactions.insert(record.transaction_id);
            }
            if (settings.enabled && txindex_ready && AutomaticProviderStateIsSynchronized(*wallet)) ContinuePoolPreparation(*wallet);
            bool executed{false};
            for (const auto& record : ReadPreparationLedger(*wallet).records) {
                if (!record.IsPreparation() || record.plan_id != supplied_id || record.transaction_id.IsNull()) continue;
                executed |= prior_transactions.count(record.transaction_id) == 0;
                if (record.kind == ProviderMaintenanceKind::PREPARE_DGB) {
                    result.pushKV("dgb_txid", record.transaction_id.GetHex());
                    result.pushKV("txid", record.transaction_id.GetHex());
                    result.pushKV("network_fee_satoshis", record.actual_fee.value);
                } else {
                    result.pushKV("dd_txid", record.transaction_id.GetHex());
                }
            }
            if (!result.exists("txid") && result.exists("dd_txid")) result.pushKV("txid", result.find_value("dd_txid"));
            result.pushKV("accepted", true);
            result.pushKV("preparation", PoolPreparationToJSON(*wallet));
            result.pushKV("executed", executed);
            result.pushKV("plan_id", supplied_id.GetHex());
            existing.clear();
            GetPaymasterProviderPoolEntries(*wallet, existing);
            UniValue pool{UniValue::VARR};
            for (const auto& entry : existing) pool.push_back(PoolEntryToJSON(entry));
            result.pushKV("pool", std::move(pool));
            return result;
        },
    };
}

RPCHelpMan rebalancepaymasterpool()
{
    return RPCHelpMan{
        "rebalancepaymasterpool",
        "Preview or retire excess available Paymaster pool entries.\n"
        "The default is side-effect-free. Execution requires the unchanged plan_id returned by that preview.\n"
        "Active reservations are never touched. Increasing a target remains an additive preparepaymasterpool operation.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Pool target and explicit execution approval", {
                                                                                                                    {"admission_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Remaining admission DGB slots"},
                                                                                                                    {"operational_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Remaining operational DGB slots"},
                                                                                                                    {"admission_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Remaining admission carriers"},
                                                                                                                    {"operational_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Remaining operational carriers"},
                                                                                                                    {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Commit the reviewed retirement transactions"},
                                                                                                                    {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged preview plan id required for execution"},
                                                                                                                }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Pool rebalance preview or result", {
                                                                                    {RPCResult::Type::BOOL, "executed", "Whether a retirement transaction was committed"},
                                                                                    {RPCResult::Type::STR_HEX, "plan_id", "Plan bound to the policy, targets, and exact retired pool inputs"},
                                                                                    {RPCResult::Type::NUM, "retired_admission_dgb_slots", "Admission DGB entries selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_operational_dgb_slots", "Operational DGB entries selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_admission_carrier_slots", "Admission carriers selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_operational_carrier_slots", "Operational carriers selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_dgb_satoshis", "DGB returned to ordinary wallet liquidity before network fee"},
                                                                                    {RPCResult::Type::NUM, "retired_carrier_cents", "DD returned to ordinary wallet liquidity"},
                                                                                    {RPCResult::Type::STR_HEX, "dd_txid", /*optional=*/true, "DD carrier retirement transaction"},
                                                                                    {RPCResult::Type::STR_HEX, "dgb_txid", /*optional=*/true, "DGB retirement transaction"},
                                                                                    {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "DGB retirement transaction fee"},
                                                                                    {RPCResult::Type::ARR, "pool", /*optional=*/true, "Authoritative persisted pool entries", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Pool entry", {
                                                                                                                                                                                                                                                {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                                                                {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                                {RPCResult::Type::STR, "purpose", "admission or operational"},
                                                                                                                                                                                                                                                {RPCResult::Type::STR, "asset", "dgb or dd_carrier"},
                                                                                                                                                                                                                                                {RPCResult::Type::STR, "state", "Pool entry state"},
                                                                                                                                                                                                                                                {RPCResult::Type::NUM, "dgb_satoshis", "DGB value"},
                                                                                                                                                                                                                                                {RPCResult::Type::NUM, "dd_cents", "Carrier value"},
                                                                                                                                                                                                                                                {RPCResult::Type::NUM, "confirmation_height", "Confirmation height or zero"},
                                                                                                                                                                                                                                                {RPCResult::Type::STR_HEX, "reservation_id", /*optional=*/true, "Durable reservation"},
                                                                                                                                                                                                                                                {RPCResult::Type::STR_HEX, "origin_commit_key", /*optional=*/true, "Commit or maintenance operation that created this successor"},
                                                                                                                                                                                                                                                {RPCResult::Type::NUM_TIME, "updated_at", "Last persistent update"},
                                                                                                                                                                                                                                            }}}},
                                                                                }},
        RPCExamples{HelpExampleCli("rebalancepaymasterpool", "'{\"admission_dgb_slots\":3,\"operational_dgb_slots\":1,\"admission_carrier_slots\":3,\"operational_carrier_slots\":1}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            ProviderIdentityRecord identity;
            std::optional<ProviderWorkGuard> pool_guard;
            if (context.paymaster && context.paymaster->Enabled() &&
                GetPaymasterIdentity(*wallet, identity)) {
                pool_guard.emplace(
                    *context.paymaster, wallet->GetName(),
                    identity.provider_id, /*require_running=*/false);
                if (!pool_guard->Acquired()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_BUSY");
                }
            }
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"admission_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"operational_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"admission_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"operational_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"execute", UniValueType(UniValue::VBOOL)},
                             {"plan_id", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const int admission = options.find_value("admission_dgb_slots").getInt<int>();
            const int operational = options.find_value("operational_dgb_slots").getInt<int>();
            const int admission_carriers = options.find_value("admission_carrier_slots").isNull() ? 0 : options.find_value("admission_carrier_slots").getInt<int>();
            const int operational_carriers = options.find_value("operational_carrier_slots").isNull() ? 0 : options.find_value("operational_carrier_slots").getInt<int>();
            const bool execute = !options.find_value("execute").isNull() &&
                                 options.find_value("execute").get_bool();
            if (admission < static_cast<int>(REQUIRED_ADMISSION_SLOTS) || admission > 16 ||
                operational < 1 || operational > 16 || admission_carriers < 0 ||
                admission_carriers > 16 || operational_carriers < 0 || operational_carriers > 16) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_INVALID_POOL_TARGET");
            }
            ProviderPolicy policy;
            if (!GetPaymasterProviderPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POLICY_NOT_FOUND");
            }
            const bool user_paid = PolicyAllowsFundingModel(policy, FundingModel::USER_PAID);
            if (user_paid && (admission_carriers < static_cast<int>(REQUIRED_ADMISSION_SLOTS) ||
                              operational_carriers < 1)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
            }
            if (!user_paid && (admission_carriers != 0 || operational_carriers != 0)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SPONSORED_POOL_HAS_CARRIERS");
            }
            std::vector<ProviderPoolEntry> entries;
            if (!GetPaymasterProviderPoolEntries(*wallet, entries)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOLS_NOT_PREPARED");
            }
            std::string refresh_error;
            if (!RefreshPoolConfirmationHeights(*wallet, entries, refresh_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, refresh_error);
            }
            if (std::any_of(entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
                    return entry.state == PoolEntryState::RESERVED ||
                           entry.state == PoolEntryState::PENDING_SUCCESSOR ||
                           entry.state == PoolEntryState::COMMITTED;
                })) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REBALANCE_ACTIVE_RESERVATIONS");
            }

            const auto candidates = [&](PoolPurpose purpose, PoolAsset asset) {
                std::vector<const ProviderPoolEntry*> result;
                for (const ProviderPoolEntry& entry : entries) {
                    if (entry.purpose == purpose && entry.asset == asset &&
                        entry.state == PoolEntryState::AVAILABLE) {
                        result.push_back(&entry);
                    }
                }
                // Retain the most useful entries when reducing a target. In
                // particular, a policy increase can leave old operational DGB
                // outputs below the new advertised fee ceiling; those must be
                // retired before larger, still-ready slots.
                std::sort(result.begin(), result.end(), [asset](const auto* a, const auto* b) {
                    const int64_t a_value = asset == PoolAsset::DGB
                        ? a->dgb_value.value : a->carrier_value.value;
                    const int64_t b_value = asset == PoolAsset::DGB
                        ? b->dgb_value.value : b->carrier_value.value;
                    if (a_value != b_value) return a_value > b_value;
                    return a->outpoint < b->outpoint;
                });
                return result;
            };
            const auto admission_dgb = candidates(PoolPurpose::ADMISSION, PoolAsset::DGB);
            const auto operational_dgb = candidates(PoolPurpose::OPERATIONAL, PoolAsset::DGB);
            const auto admission_dd = candidates(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER);
            const auto operational_dd = candidates(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER);
            if (admission > static_cast<int>(admission_dgb.size()) ||
                operational > static_cast<int>(operational_dgb.size()) ||
                admission_carriers > static_cast<int>(admission_dd.size()) ||
                operational_carriers > static_cast<int>(operational_dd.size())) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_REBALANCE_INCREASE_USE_PREPARE");
            }

            std::vector<const ProviderPoolEntry*> retire_dgb;
            std::vector<const ProviderPoolEntry*> retire_dd;
            const auto retire_tail = [](const auto& available, int keep, auto& retired) {
                retired.insert(retired.end(), available.begin() + keep, available.end());
            };
            retire_tail(admission_dgb, admission, retire_dgb);
            retire_tail(operational_dgb, operational, retire_dgb);
            retire_tail(admission_dd, admission_carriers, retire_dd);
            retire_tail(operational_dd, operational_carriers, retire_dd);
            if (std::any_of(retire_dgb.begin(), retire_dgb.end(), [](const auto* entry) {
                    return entry->confirmation_height <= 0;
                }) ||
                std::any_of(retire_dd.begin(), retire_dd.end(), [](const auto* entry) {
                    return entry->confirmation_height <= 0;
                })) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REBALANCE_REQUIRES_CONFIRMED_INPUTS");
            }
            const auto count_retired = [](const auto& retired, PoolPurpose purpose) {
                return static_cast<int>(std::count_if(retired.begin(), retired.end(),
                                                      [&](const auto* entry) { return entry->purpose == purpose; }));
            };
            const int retired_admission_dgb = count_retired(retire_dgb, PoolPurpose::ADMISSION);
            const int retired_operational_dgb = count_retired(retire_dgb, PoolPurpose::OPERATIONAL);
            const int retired_admission_dd = count_retired(retire_dd, PoolPurpose::ADMISSION);
            const int retired_operational_dd = count_retired(retire_dd, PoolPurpose::OPERATIONAL);
            const auto checked_pool_sum = [](const auto& selected,
                                             const auto& amount) {
                std::optional<int64_t> total{int64_t{0}};
                for (const ProviderPoolEntry* entry : selected) {
                    total = CheckedAdd(*total, amount(*entry));
                    if (!total) break;
                }
                return total;
            };
            const auto retired_dgb_value = checked_pool_sum(
                retire_dgb,
                [](const ProviderPoolEntry& entry) {
                    return entry.dgb_value.value;
                });
            const auto retired_dd_value = checked_pool_sum(
                retire_dd,
                [](const ProviderPoolEntry& entry) {
                    return entry.carrier_value.value;
                });
            if (!retired_dgb_value || !MoneyRange(*retired_dgb_value) ||
                !retired_dd_value || *retired_dd_value < 0) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_REBALANCE_VALUE_OUT_OF_RANGE");
            }

            HashWriter plan_hasher = TaggedHash(
                "DigiByte Paymaster Pool Rebalance Plan v1");
            plan_hasher << Params().GenesisBlock().GetHash()
                        << GetProviderPolicyHash(policy)
                        << admission << operational
                        << admission_carriers << operational_carriers;
            for (const ProviderPoolEntry* entry : retire_dgb) {
                plan_hasher << entry->outpoint
                            << static_cast<uint8_t>(entry->purpose)
                            << entry->dgb_value.value;
            }
            for (const ProviderPoolEntry* entry : retire_dd) {
                plan_hasher << entry->outpoint
                            << static_cast<uint8_t>(entry->purpose)
                            << entry->carrier_value.value;
            }
            const uint256 plan_id = plan_hasher.GetSHA256();

            UniValue result{UniValue::VOBJ};
            result.pushKV("plan_id", plan_id.GetHex());
            result.pushKV("retired_admission_dgb_slots", retired_admission_dgb);
            result.pushKV("retired_operational_dgb_slots", retired_operational_dgb);
            result.pushKV("retired_admission_carrier_slots", retired_admission_dd);
            result.pushKV("retired_operational_carrier_slots", retired_operational_dd);
            result.pushKV("retired_dgb_satoshis", *retired_dgb_value);
            result.pushKV("retired_carrier_cents", *retired_dd_value);
            if (!execute) {
                result.pushKV("executed", false);
                return result;
            }

            const UniValue& supplied_plan = options.find_value("plan_id");
            if (supplied_plan.isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_POOL_PLAN_REQUIRED");
            }
            if (ParseHashV(supplied_plan, "plan_id") != plan_id) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_POOL_PLAN_CHANGED");
            }

            if (!GetPaymasterIdentity(*wallet, identity)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_IDENTITY_NOT_FOUND");
            }
            // Reconcile confirmed setup transactions before checking the pending
            // journal; pool confirmation refresh alone does not advance it.
            size_t recovered{0};
            std::string reconcile_error;
            if (!ReconcileProviderMaintenance(*wallet, recovered, reconcile_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, reconcile_error);
            }
            // A later refill from an older approved plan must not undo this
            // explicit pool reduction. Cancel unsigned setup work first.
            if (HasPendingPoolPreparation(*wallet)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_PREPARATION_PENDING");
            }
            EnsureWalletIsUnlocked(*wallet);
            bool executed{false};
            std::string dd_txid;
            CTransactionRef dd_tx;
            if (!retire_dd.empty()) {
                DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
                if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_DD_WALLET_UNAVAILABLE");
                std::vector<COutPoint> inputs;
                std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
                for (const ProviderPoolEntry* entry : retire_dd) {
                    auto destination = wallet->GetNewDestination(OutputType::BECH32M, "Retired Paymaster carrier");
                    if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                    CDigiDollarAddress address{EncodeDigiDollarAddress(*destination)};
                    if (!address.IsValid()) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                    inputs.push_back(entry->outpoint);
                    recipients.emplace_back(std::move(address), entry->carrier_value.value);
                }
                std::string transfer_error;
                DDTransferPlan transfer_plan;
                CMutableTransaction transaction;
                if (!dd_wallet->PlanDigiDollarTransfer(recipients, transfer_plan, transfer_error,
                                                       &inputs, /*allow_paymaster_pool_inputs=*/true) ||
                    !dd_wallet->BuildDigiDollarTransfer(transfer_plan, transaction, transfer_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_REBALANCE_FAILED: " + transfer_error);
                }
                dd_tx = MakeTransactionRef(std::move(transaction));
                const auto fee = GetProviderFinanceTransactionFee(*wallet, dd_tx);
                if (!fee || fee->value <= 0 || fee->value > transfer_plan.estimated_fee ||
                    fee->value > wallet->m_default_max_tx_fee) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_RETIREMENT_FEE_LIMIT");
                }
                // The marker and signed bytes share the existing wallet write.
                // Rejection must not abandon an already saved transaction.
                if (!wallet->CommitTransaction(dd_tx,
                        {{"comment", "Paymaster carrier retirement"},
                         {PAYMASTER_RETIREMENT_PROVIDER_KEY, identity.provider_id.GetHex()}},
                        {}, &transfer_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_REBALANCE_FAILED: " + transfer_error);
                }
                dd_txid = dd_tx->GetHash().GetHex();
                if (!dd_wallet->RecordPaymasterSendHistory(dd_tx->GetHash(),
                        GetScriptForDestination(recipients.front().first.GetDigiDollarDestination()),
                        *retired_dd_value, transfer_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, transfer_error);
                }
                const int64_t now = GetTime();
                for (const COutPoint& input : inputs) {
                    auto entry = std::find_if(entries.begin(), entries.end(),
                                              [&](const ProviderPoolEntry& candidate) { return candidate.outpoint == input; });
                    if (entry != entries.end()) {
                        entry->state = PoolEntryState::SPENT;
                        entry->updated_at = now;
                    }
                }
                std::string persist_error;
                if (!SetPaymasterProviderPoolEntries(*wallet, entries, persist_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       persist_error + "; retired carrier inputs are recoverable from wallet history");
                }
                executed = true;
            }

            CTransactionRef dgb_tx;
            CAmount dgb_fee{0};
            if (!retire_dgb.empty()) {
                auto destination = wallet->GetNewDestination(OutputType::BECH32M, "Retired Paymaster DGB");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                CCoinControl coin_control;
                coin_control.m_allow_other_inputs = false;
                coin_control.m_allow_paymaster_pool_inputs = true;
                coin_control.m_min_depth = 1;
                for (const ProviderPoolEntry* entry : retire_dgb)
                    coin_control.Select(entry->outpoint);
                std::vector<CRecipient> recipients{{*destination, *retired_dgb_value, /*subtract_fee=*/true}};
                auto created = CreateTransaction(*wallet, recipients, /*change_pos=*/-1,
                                                 coin_control, /*sign=*/true);
                if (!created) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_DGB_REBALANCE_FAILED: " + util::ErrorString(created).original);
                }
                std::string commit_error;
                if (!wallet->CommitTransaction(created->tx,
                        {{PAYMASTER_RETIREMENT_PROVIDER_KEY, identity.provider_id.GetHex()}},
                        {}, &commit_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_DGB_REBALANCE_REJECTED: " + commit_error);
                }
                const int64_t now = GetTime();
                for (const ProviderPoolEntry* selected : retire_dgb) {
                    auto entry = std::find_if(entries.begin(), entries.end(),
                                              [&](const ProviderPoolEntry& candidate) { return candidate.outpoint == selected->outpoint; });
                    if (entry != entries.end()) {
                        entry->state = PoolEntryState::SPENT;
                        entry->updated_at = now;
                    }
                }
                std::string persist_error;
                if (!SetPaymasterProviderPoolEntries(*wallet, entries, persist_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       persist_error + "; retired DGB inputs are recoverable from wallet history");
                }
                dgb_tx = created->tx;
                dgb_fee = created->fee;
                executed = true;
            }

            size_t changed_events{0};
            std::string finance_error;
            if (!ReconcilePaymasterProviderFinances(*wallet, changed_events, finance_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, finance_error);
            }

            UniValue pool{UniValue::VARR};
            for (const auto& entry : entries)
                pool.push_back(PoolEntryToJSON(entry));
            result.pushKV("executed", executed);
            if (!dd_txid.empty()) result.pushKV("dd_txid", dd_txid);
            if (dgb_tx) {
                result.pushKV("dgb_txid", dgb_tx->GetHash().GetHex());
                result.pushKV("network_fee_satoshis", dgb_fee);
            }
            result.pushKV("pool", std::move(pool));
            return result;
        },
    };
}

RPCHelpMan getpaymasterfinancestatus()
{
    return RPCHelpMan{
        "getpaymasterfinancestatus",
        "Return wallet-local provider income, DGB costs, pool capital, and optional accounting events.\n"
        "Native DD and DGB values are authoritative. Any USD result uses only the current oracle price.\n",
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Finance query", {
                {"period", RPCArg::Type::STR, RPCArg::Default{"30d"}, "today, 7d, 30d, or all"},
                {"include_events", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include paginated accounting events"},
                {"limit", RPCArg::Type::NUM, RPCArg::Default{100}, "Maximum events returned (1-10000)"},
                {"cursor", RPCArg::Type::STR, RPCArg::Default{""}, "Last event id from the previous page"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Provider finance status", {
            {RPCResult::Type::STR_HEX, "provider_id", "Provider identity owning this ledger"},
            {RPCResult::Type::STR, "period", "Selected accounting period"},
            {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
            {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
            {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
            {RPCResult::Type::NUM, "average_service_fee_cents", "Average DD income per confirmed transfer"},
            {RPCResult::Type::NUM, "user_paid_transfers", "Confirmed user-paid transfers"},
            {RPCResult::Type::NUM, "public_sponsored_transfers", "Confirmed public sponsored transfers"},
            {RPCResult::Type::NUM, "restricted_sponsored_transfers", "Confirmed restricted sponsored transfers"},
            {RPCResult::Type::OBJ_DYN, "model_breakdown", "Selected-period transfer economics by funding model", {
                {RPCResult::Type::OBJ, "model", "One funding-model summary", {
                    {RPCResult::Type::NUM, "successful_transfers", "Confirmed transfers"},
                    {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                    {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB transfer costs"},
                }},
            }},
            {RPCResult::Type::OBJ_DYN, "period_summaries", "Keys are today, 7d, 30d, and all", {
                {RPCResult::Type::OBJ, "period", "Confirmed totals for one dashboard period", {
                    {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                    {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
                    {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
                    {RPCResult::Type::NUM, "average_service_fee_cents", "Average DD income per confirmed transfer"},
                    {RPCResult::Type::NUM, "user_paid_transfers", "Confirmed user-paid transfers"},
                    {RPCResult::Type::NUM, "public_sponsored_transfers", "Confirmed public sponsored transfers"},
                    {RPCResult::Type::NUM, "restricted_sponsored_transfers", "Confirmed restricted sponsored transfers"},
                }},
            }},
            {RPCResult::Type::BOOL, "history_partially_reconstructable", "Whether earlier exact history is unavailable"},
            {RPCResult::Type::NUM_TIME, "history_complete_from", "Start of guaranteed complete accounting"},
            {RPCResult::Type::BOOL, "backup_required", "Whether the provider wallet should be backed up"},
            {RPCResult::Type::NUM_TIME, "last_successful_backup_at", "Last successful backupwallet completion"},
            {RPCResult::Type::NUM_TIME, "external_backup_acknowledged_at", "Last acknowledged external full-wallet backup"},
            {RPCResult::Type::NUM, "oracle_price_micro_usd", /*optional=*/true, "Current DGB/USD oracle price"},
            {RPCResult::Type::NUM_TIME, "valuation_time", /*optional=*/true, "Time of the current-price estimate"},
            {RPCResult::Type::NUM, "estimated_result_usd", /*optional=*/true, "Current-price estimate, not historical accounting"},
            {RPCResult::Type::OBJ, "pool_capital", "Current wallet-owned provider capital", {
                {RPCResult::Type::NUM, "dgb_available_satoshis", "Available DGB pool capital"},
                {RPCResult::Type::NUM, "dgb_reserved_satoshis", "Reserved or committed DGB pool capital"},
                {RPCResult::Type::NUM, "dgb_pending_satoshis", "Unconfirmed DGB successor capital"},
                {RPCResult::Type::NUM, "carrier_base_cents", "Reserved DD carrier base capital"},
                {RPCResult::Type::NUM, "carrier_earned_cents", "Service fees accumulated above carrier bases"},
                {RPCResult::Type::NUM, "carrier_withdrawable_cents", "Confirmed available carrier surplus"},
                {RPCResult::Type::NUM, "pending_maintenance_transactions", "Pending maintenance or withdrawal transactions"},
            }},
            {RPCResult::Type::ARR, "daily_totals", /*optional=*/true, "Confirmed UTC-day totals for the selected period, limited to the latest 366 days", {
                {RPCResult::Type::OBJ, "", "One UTC accounting day", {
                    {RPCResult::Type::NUM_TIME, "day_start", "UTC start time of this day"},
                    {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                    {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
                    {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
                    {RPCResult::Type::NUM, "maintenance_transactions", "Confirmed setup, maintenance, retirement, or withdrawal transactions"},
                }},
            }},
            {RPCResult::Type::ARR, "events", /*optional=*/true, "Accounting events", {
                {RPCResult::Type::OBJ, "", "Finance event", {
                    {RPCResult::Type::STR_HEX, "event_id", "Stable idempotency key"},
                    {RPCResult::Type::STR, "kind", "transfer, setup, replenishment, retirement, or withdrawal"},
                    {RPCResult::Type::STR, "state", "pending, confirmed, or invalidated"},
                    {RPCResult::Type::NUM, "dd_income_cents", "Native DD income"},
                    {RPCResult::Type::NUM, "dgb_cost_satoshis", "Native DGB cost"},
                    {RPCResult::Type::NUM_TIME, "created_at", "Event creation time"},
                    {RPCResult::Type::NUM_TIME, "confirmed_at", /*optional=*/true, "Confirmation time"},
                    {RPCResult::Type::STR, "funding_model", /*optional=*/true, "user_paid or sponsored for transfer events"},
                    {RPCResult::Type::STR, "sponsorship_scope", /*optional=*/true, "public or restricted for sponsored transfer events"},
                    {RPCResult::Type::STR_HEX, "transaction_id", "Wallet transaction represented by this event"},
                }},
            }},
            {RPCResult::Type::STR, "next_cursor", /*optional=*/true, "Cursor for the next event page"},
        }},
        RPCExamples{HelpExampleCli("getpaymasterfinancestatus", "'{\"period\":\"30d\",\"include_events\":true}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue options = request.params[0].isNull()
                ? UniValue{UniValue::VOBJ} : request.params[0];
            RPCTypeCheckObj(options,
                            {{"period", UniValueType(UniValue::VSTR)},
                             {"include_events", UniValueType(UniValue::VBOOL)},
                             {"limit", UniValueType(UniValue::VNUM)},
                             {"cursor", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const std::string period = options.find_value("period").isNull()
                ? "30d" : options.find_value("period").get_str();
            const bool include_events =
                !options.find_value("include_events").isNull() &&
                options.find_value("include_events").get_bool();
            const int limit = options.find_value("limit").isNull()
                ? 100 : options.find_value("limit").getInt<int>();
            const std::string cursor = options.find_value("cursor").isNull()
                ? std::string{} : options.find_value("cursor").get_str();
            if ((period != "today" && period != "7d" && period != "30d" &&
                 period != "all") || limit < 1 || limit > 10000) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_INVALID_FINANCE_QUERY");
            }

            // Finance reconciliation classifies wallet transactions against
            // the active chain. Wait for the wallet notification queue first
            // so a just-confirmed payment cannot be reported as pending while
            // the rest of this RPC already observes the newer chain tip.
            wallet->BlockUntilSyncedToCurrentChain();
            size_t reconciled{0};
            std::string error;
            if (!ReconcilePaymasterProviderFinances(*wallet, reconciled, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            ProviderFinanceLedger ledger;
            ProviderIdentityRecord identity;
            ProviderBackupStatus backup;
            std::vector<ProviderPoolEntry> pool;
            ProviderMaintenanceLedger maintenance;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (!batch.ReadPaymasterIdentity(identity) ||
                    !batch.ReadPaymasterFinanceLedger(ledger)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FINANCE_LEDGER_NOT_FOUND");
                }
                if (!batch.ReadPaymasterProviderPool(pool) &&
                    batch.HasPaymasterProviderPool()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_INVALID_PROVIDER_POOL");
                }
                if (!batch.ReadPaymasterMaintenanceLedger(maintenance) &&
                    batch.HasPaymasterMaintenanceLedger()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
                }
            }
            if (!GetPaymasterProviderBackupStatus(
                    *wallet, backup, GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }

            const int64_t now = GetTime();
            const int64_t utc_day = (now / (24 * 60 * 60)) * (24 * 60 * 60);
            int64_t cutoff{0};
            if (period == "today") cutoff = utc_day;
            if (period == "7d") cutoff = now - 7 * 24 * 60 * 60;
            if (period == "30d") cutoff = now - 30 * 24 * 60 * 60;
            int64_t dd_income{0};
            int64_t dgb_cost{0};
            uint64_t transfers{0};
            uint64_t user_paid{0};
            uint64_t public_sponsored{0};
            uint64_t restricted_sponsored{0};
            int64_t user_paid_income{0};
            int64_t user_paid_cost{0};
            int64_t public_sponsored_income{0};
            int64_t public_sponsored_cost{0};
            int64_t restricted_sponsored_income{0};
            int64_t restricted_sponsored_cost{0};
            const auto add_amount = [&](int64_t& total, int64_t amount) {
                if (amount < 0 || total > std::numeric_limits<int64_t>::max() - amount) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FINANCE_TOTAL_OVERFLOW");
                }
                total += amount;
            };
            std::vector<const ProviderFinanceEvent*> matching_events;
            for (const ProviderFinanceEvent& event : ledger.events) {
                // A confirmed booking belongs to the UTC period in which it
                // actually became part of the active chain. Pending and
                // invalidated records have no confirmation time, so their
                // creation time remains the only useful ordering key.
                const int64_t accounting_time =
                    event.state == ProviderFinanceEventState::CONFIRMED
                    ? event.confirmed_at
                    : event.created_at;
                if (cutoff > 0 && accounting_time < cutoff) continue;
                matching_events.push_back(&event);
                if (event.state != ProviderFinanceEventState::CONFIRMED) continue;
                add_amount(dd_income, event.dd_income.value);
                add_amount(dgb_cost, event.dgb_cost.value);
                if (event.kind != ProviderFinanceEventKind::TRANSFER) continue;
                ++transfers;
                if (event.funding_model == FundingModel::USER_PAID) {
                    ++user_paid;
                    add_amount(user_paid_income, event.dd_income.value);
                    add_amount(user_paid_cost, event.dgb_cost.value);
                } else if (event.sponsorship_scope == SponsorshipScope::PUBLIC) {
                    ++public_sponsored;
                    add_amount(public_sponsored_income, event.dd_income.value);
                    add_amount(public_sponsored_cost, event.dgb_cost.value);
                } else {
                    ++restricted_sponsored;
                    add_amount(restricted_sponsored_income,
                               event.dd_income.value);
                    add_amount(restricted_sponsored_cost,
                               event.dgb_cost.value);
                }
            }

            int64_t dgb_available{0};
            int64_t dgb_reserved{0};
            int64_t dgb_pending{0};
            int64_t carrier_base{0};
            int64_t carrier_earned{0};
            int64_t carrier_withdrawable{0};
            for (const ProviderPoolEntry& entry : pool) {
                if (!IsActiveProviderPoolState(entry.state)) continue;
                if (entry.asset == PoolAsset::DGB) {
                    if (entry.state == PoolEntryState::AVAILABLE) {
                        add_amount(dgb_available, entry.dgb_value.value);
                    } else if (entry.state == PoolEntryState::PENDING_SUCCESSOR) {
                        add_amount(dgb_pending, entry.dgb_value.value);
                    } else {
                        add_amount(dgb_reserved, entry.dgb_value.value);
                    }
                    continue;
                }
                const int64_t base = std::min<int64_t>(100, entry.carrier_value.value);
                const int64_t excess = std::max<int64_t>(0, entry.carrier_value.value - base);
                add_amount(carrier_base, base);
                add_amount(carrier_earned, excess);
                if (entry.state == PoolEntryState::AVAILABLE &&
                    entry.confirmation_height > 0) {
                    add_amount(carrier_withdrawable, excess);
                }
            }
            const int64_t pending_maintenance = std::count_if(
                maintenance.records.begin(), maintenance.records.end(),
                [](const ProviderMaintenanceRecord& record) {
                    return record.state == ProviderMaintenanceState::PLANNED ||
                           record.state == ProviderMaintenanceState::BROADCAST;
                });

            UniValue result{UniValue::VOBJ};
            result.pushKV("provider_id", identity.provider_id.GetHex());
            result.pushKV("period", period);
            result.pushKV("service_fee_income_cents", dd_income);
            result.pushKV("dgb_operating_cost_satoshis", dgb_cost);
            result.pushKV("successful_transfers", transfers);
            result.pushKV("average_service_fee_cents",
                          transfers == 0 ? 0 : dd_income / static_cast<int64_t>(transfers));
            result.pushKV("user_paid_transfers", user_paid);
            result.pushKV("public_sponsored_transfers", public_sponsored);
            result.pushKV("restricted_sponsored_transfers", restricted_sponsored);
            const auto make_model_summary = [](uint64_t model_transfers,
                                               int64_t model_income,
                                               int64_t model_cost) {
                UniValue summary{UniValue::VOBJ};
                summary.pushKV("successful_transfers", model_transfers);
                summary.pushKV("service_fee_income_cents", model_income);
                summary.pushKV("dgb_operating_cost_satoshis", model_cost);
                return summary;
            };
            UniValue model_breakdown{UniValue::VOBJ};
            model_breakdown.pushKV(
                "user_paid",
                make_model_summary(user_paid, user_paid_income,
                                   user_paid_cost));
            model_breakdown.pushKV(
                "public_sponsored",
                make_model_summary(public_sponsored,
                                   public_sponsored_income,
                                   public_sponsored_cost));
            model_breakdown.pushKV(
                "restricted_sponsored",
                make_model_summary(restricted_sponsored,
                                   restricted_sponsored_income,
                                   restricted_sponsored_cost));
            result.pushKV("model_breakdown", std::move(model_breakdown));

            // The operator dashboard displays all standard periods together.
            // Compute them from the authoritative event stream in one wallet
            // snapshot so the four cards cannot disagree because of separate
            // RPC calls crossing a new block or day boundary.
            const auto make_period_summary = [&](int64_t summary_cutoff) {
                int64_t summary_income{0};
                int64_t summary_cost{0};
                uint64_t summary_transfers{0};
                uint64_t summary_user_paid{0};
                uint64_t summary_public_sponsored{0};
                uint64_t summary_restricted_sponsored{0};
                for (const ProviderFinanceEvent& event : ledger.events) {
                    if (event.state != ProviderFinanceEventState::CONFIRMED) continue;
                    if (summary_cutoff > 0 &&
                        event.confirmed_at < summary_cutoff) continue;
                    add_amount(summary_income, event.dd_income.value);
                    add_amount(summary_cost, event.dgb_cost.value);
                    if (event.kind != ProviderFinanceEventKind::TRANSFER) continue;
                    ++summary_transfers;
                    if (event.funding_model == FundingModel::USER_PAID) {
                        ++summary_user_paid;
                    } else if (event.sponsorship_scope == SponsorshipScope::PUBLIC) {
                        ++summary_public_sponsored;
                    } else {
                        ++summary_restricted_sponsored;
                    }
                }
                UniValue summary{UniValue::VOBJ};
                summary.pushKV("service_fee_income_cents", summary_income);
                summary.pushKV("dgb_operating_cost_satoshis", summary_cost);
                summary.pushKV("successful_transfers", summary_transfers);
                summary.pushKV(
                    "average_service_fee_cents",
                    summary_transfers == 0
                        ? 0
                        : summary_income / static_cast<int64_t>(summary_transfers));
                summary.pushKV("user_paid_transfers", summary_user_paid);
                summary.pushKV("public_sponsored_transfers", summary_public_sponsored);
                summary.pushKV("restricted_sponsored_transfers", summary_restricted_sponsored);
                return summary;
            };
            UniValue period_summaries{UniValue::VOBJ};
            period_summaries.pushKV("today", make_period_summary(utc_day));
            period_summaries.pushKV("7d", make_period_summary(now - 7 * 24 * 60 * 60));
            period_summaries.pushKV("30d", make_period_summary(now - 30 * 24 * 60 * 60));
            period_summaries.pushKV("all", make_period_summary(0));
            result.pushKV("period_summaries", std::move(period_summaries));
            result.pushKV("history_partially_reconstructable",
                          ledger.earlier_history_partial);
            result.pushKV("history_complete_from", ledger.history_complete_from);
            result.pushKV("backup_required", ProviderBackupRequired(backup));
            result.pushKV("last_successful_backup_at",
                          backup.last_successful_backup_at);
            result.pushKV("external_backup_acknowledged_at",
                          backup.external_backup_acknowledged_at);

            const CAmount oracle_price =
                OracleIntegration::GetCurrentOraclePriceMicroUSD();
            if (oracle_price > 0) {
                const long double income_usd =
                    static_cast<long double>(dd_income) / 100.0L;
                const long double cost_usd =
                    static_cast<long double>(dgb_cost) *
                    static_cast<long double>(oracle_price) /
                    static_cast<long double>(COIN) / 1000000.0L;
                result.pushKV("oracle_price_micro_usd", oracle_price);
                result.pushKV("valuation_time", now);
                result.pushKV("estimated_result_usd",
                              static_cast<double>(income_usd - cost_usd));
            }

            UniValue pool_capital{UniValue::VOBJ};
            pool_capital.pushKV("dgb_available_satoshis", dgb_available);
            pool_capital.pushKV("dgb_reserved_satoshis", dgb_reserved);
            pool_capital.pushKV("dgb_pending_satoshis", dgb_pending);
            pool_capital.pushKV("carrier_base_cents", carrier_base);
            pool_capital.pushKV("carrier_earned_cents", carrier_earned);
            pool_capital.pushKV("carrier_withdrawable_cents", carrier_withdrawable);
            pool_capital.pushKV("pending_maintenance_transactions", pending_maintenance);
            result.pushKV("pool_capital", std::move(pool_capital));

            if (include_events) {
                // A compact daily progression lets operator UIs graph a year
                // without exposing transaction identifiers or loading the
                // complete event ledger. The selected-period filter is still
                // respected; an all-history request deliberately returns only
                // the most recent 366 daily materializations.
                std::vector<const ProviderFinanceDailyTotals*> daily_totals;
                for (const ProviderFinanceDailyTotals& total :
                     ledger.daily_totals) {
                    if (cutoff > 0 && total.day_start < cutoff) continue;
                    daily_totals.push_back(&total);
                }
                std::sort(
                    daily_totals.begin(), daily_totals.end(),
                    [](const ProviderFinanceDailyTotals* lhs,
                       const ProviderFinanceDailyTotals* rhs) {
                        return lhs->day_start > rhs->day_start;
                    });
                if (daily_totals.size() > 366) daily_totals.resize(366);
                UniValue days{UniValue::VARR};
                for (const ProviderFinanceDailyTotals* total : daily_totals) {
                    UniValue day{UniValue::VOBJ};
                    day.pushKV("day_start", total->day_start);
                    day.pushKV("service_fee_income_cents",
                               total->dd_income.value);
                    day.pushKV("dgb_operating_cost_satoshis",
                               total->dgb_cost.value);
                    day.pushKV("successful_transfers",
                               total->successful_transfers);
                    day.pushKV("maintenance_transactions",
                               total->maintenance_transactions);
                    days.push_back(std::move(day));
                }
                result.pushKV("daily_totals", std::move(days));

                std::sort(matching_events.begin(), matching_events.end(),
                          [](const ProviderFinanceEvent* lhs,
                             const ProviderFinanceEvent* rhs) {
                              if (lhs->created_at != rhs->created_at) {
                                  return lhs->created_at > rhs->created_at;
                              }
                              return rhs->event_id < lhs->event_id;
                          });
                size_t start{0};
                if (!cursor.empty()) {
                    const auto previous = std::find_if(
                        matching_events.begin(), matching_events.end(),
                        [&](const ProviderFinanceEvent* event) {
                            return event->event_id.GetHex() == cursor;
                        });
                    if (previous == matching_events.end()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER,
                                           "PAYMASTER_INVALID_FINANCE_CURSOR");
                    }
                    start = static_cast<size_t>(
                        std::distance(matching_events.begin(), previous)) + 1;
                }
                UniValue events{UniValue::VARR};
                const size_t end = std::min(
                    matching_events.size(), start + static_cast<size_t>(limit));
                const auto kind_name = [](ProviderFinanceEventKind kind) {
                    switch (kind) {
                    case ProviderFinanceEventKind::TRANSFER: return "transfer";
                    case ProviderFinanceEventKind::POOL_SETUP: return "setup";
                    case ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT: return "replenishment";
                    case ProviderFinanceEventKind::POOL_RETIREMENT: return "retirement";
                    case ProviderFinanceEventKind::CARRIER_WITHDRAWAL: return "withdrawal";
                    }
                    return "invalid";
                };
                const auto state_name = [](ProviderFinanceEventState state) {
                    switch (state) {
                    case ProviderFinanceEventState::PENDING: return "pending";
                    case ProviderFinanceEventState::CONFIRMED: return "confirmed";
                    case ProviderFinanceEventState::INVALIDATED: return "invalidated";
                    }
                    return "invalid";
                };
                for (size_t index = start; index < end; ++index) {
                    const ProviderFinanceEvent& event = *matching_events[index];
                    UniValue item{UniValue::VOBJ};
                    item.pushKV("event_id", event.event_id.GetHex());
                    item.pushKV("kind", kind_name(event.kind));
                    item.pushKV("state", state_name(event.state));
                    item.pushKV("dd_income_cents", event.dd_income.value);
                    item.pushKV("dgb_cost_satoshis", event.dgb_cost.value);
                    item.pushKV("created_at", event.created_at);
                    if (event.confirmed_at > 0) {
                        item.pushKV("confirmed_at", event.confirmed_at);
                    }
                    if (event.kind == ProviderFinanceEventKind::TRANSFER) {
                        item.pushKV("funding_model",
                                    event.funding_model == FundingModel::USER_PAID
                                        ? "user_paid" : "sponsored");
                        if (event.funding_model == FundingModel::SPONSORED) {
                            item.pushKV("sponsorship_scope",
                                        event.sponsorship_scope == SponsorshipScope::PUBLIC
                                            ? "public" : "restricted");
                        }
                    }
                    item.pushKV("transaction_id", event.transaction_id.GetHex());
                    events.push_back(std::move(item));
                }
                result.pushKV("events", std::move(events));
                if (end < matching_events.size() && end > start) {
                    result.pushKV("next_cursor",
                                  matching_events[end - 1]->event_id.GetHex());
                }
            }
            return result;
        },
    };
}

RPCHelpMan acknowledgepaymasterproviderbackup()
{
    return RPCHelpMan{
        "acknowledgepaymasterproviderbackup",
        "Acknowledge an external full-wallet backup procedure for this provider wallet.\n"
        "This does not create a backup and must not be used for seed-only or descriptor-only exports.\n",
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Backup acknowledgement", {
                {"external_backup", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Must be true after an external full-wallet backup"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Updated backup reminder", {
            {RPCResult::Type::BOOL, "acknowledged", "Whether the external backup was acknowledged"},
            {RPCResult::Type::BOOL, "backup_required", "Whether another backup reminder remains"},
            {RPCResult::Type::NUM_TIME, "acknowledged_at", "Acknowledgement time"},
        }},
        RPCExamples{HelpExampleCli("acknowledgepaymasterproviderbackup", "'{\"external_backup\":true}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"external_backup", UniValueType(UniValue::VBOOL)}},
                            /*fAllowNull=*/false, /*fStrict=*/true);
            if (!options.find_value("external_backup").get_bool()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_EXTERNAL_BACKUP_NOT_CONFIRMED");
            }
            const int64_t now = GetTime();
            std::string error;
            if (!AcknowledgePaymasterProviderExternalBackup(
                    *wallet, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            DigiDollar::Paymaster::ProviderBackupStatus status;
            if (!GetPaymasterProviderBackupStatus(
                    *wallet, status, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("acknowledged", true);
            result.pushKV("backup_required",
                          DigiDollar::Paymaster::ProviderBackupRequired(status));
            result.pushKV("acknowledged_at",
                          status.external_backup_acknowledged_at);
            return result;
        },
    };
}

RPCHelpMan getpaymasterinfo()
{
    return RPCHelpMan{
        "getpaymasterinfo",
        "Return this wallet's authoritative Paymaster provider configuration and staged readiness.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider information", {
                                                                        {RPCResult::Type::BOOL, "settings_present", "Whether this wallet has a persisted provider settings record"},
                                                                        {RPCResult::Type::BOOL, "enabled", "Configured provider enablement"},
                                                                        {RPCResult::Type::BOOL, "running", "Runtime provider state"},
                                                                        {RPCResult::Type::STR, "operation_mode", "automatic or manual"},
                                                                        {RPCResult::Type::BOOL, "autostart", "Whether optional provider autostart is enabled"},
                                                                        {RPCResult::Type::STR, "service_state", "stopped, waiting_for_unlock, waiting_for_readiness, active, manual, drain_only, or error"},
                                                                        {RPCResult::Type::STR, "last_service_error", /*optional=*/true, "Stable privacy-neutral automatic-service error"},
                                                                        {RPCResult::Type::OBJ, "service_queue", "Provider-addressed in-memory queue depth", {
                                                                                                                                                              {RPCResult::Type::NUM, "waiting_requests", "Capacity, quote, and recovery requests waiting for this provider"},
                                                                                                                                                              {RPCResult::Type::NUM, "waiting_submits", "Payment and recovery submissions waiting for this provider"},
                                                                                                                                                          }},
                                                                        {RPCResult::Type::BOOL, "ready", "Whether all V1 readiness gates currently pass"},
                                                                        {RPCResult::Type::BOOL, "wallet_eligible", "Descriptor/local-key eligibility"},
                                                                        {RPCResult::Type::BOOL, "wallet_locked", "Wallet lock state"},
                                                                        {RPCResult::Type::BOOL, "pool_ready", "Whether the persisted admission and operational pools pass policy checks"},
                                                                        {RPCResult::Type::OBJ, "pool", "Persisted pool summary", {
                                                                                                                                     {RPCResult::Type::NUM, "entries", "Total validated entries"},
                                                                                                                                     {RPCResult::Type::NUM, "reserved", "Entries reserved or durably committed to a request"},
                                                                                                                                     {RPCResult::Type::NUM, "admission_dgb", "Confirmed available admission DGB slots"},
                                                                                                                                     {RPCResult::Type::NUM, "admission_carriers", "Confirmed available admission DD carriers"},
                                                                                                                                     {RPCResult::Type::NUM, "operational_dgb", "Confirmed available operational DGB entries"},
                                                                                                                                     {RPCResult::Type::NUM, "operational_carriers", "Confirmed available operational DD carriers"},
                                                                                                                                     {RPCResult::Type::NUM, "complete_operational_slots", "Usable complete operational slots"},
                                                                                                                                 }},
                                                                         {RPCResult::Type::OBJ, "liquidity", "Persisted liquidity targets, confirmed/pending/missing slots, and finite maintenance budget",
                                                                          ProviderLiquidityStatusResults()},
                                                                        {RPCResult::Type::OBJ, "backup_status", /*optional=*/true, "Provider-wallet backup reminder", {
                                                                            {RPCResult::Type::BOOL, "required", "Whether a fresh full-wallet backup or external-backup acknowledgement is recommended"},
                                                                            {RPCResult::Type::NUM_TIME, "reminder_updated_at", "Time of the latest identity or material configuration reminder"},
                                                                            {RPCResult::Type::NUM_TIME, "last_successful_backup_at", "Last backupwallet completion recorded by this wallet"},
                                                                            {RPCResult::Type::NUM_TIME, "external_backup_acknowledged_at", "Last acknowledged external full-wallet backup"},
                                                                        }},
                                                                        {RPCResult::Type::OBJ, "finance_summary", /*optional=*/true, "Confirmed native all-time provider accounting", {
                                                                            {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                                                                            {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
                                                                            {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
                                                                            {RPCResult::Type::BOOL, "history_partially_reconstructable", "Whether older exact history is unavailable"},
                                                                        }},
                                                                        {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                        {RPCResult::Type::STR, "endpoint", /*optional=*/true, "Published provider endpoint"},
                                                                        {RPCResult::Type::NUM, "announcement_sequence", /*optional=*/true, "Published monotonic announcement sequence"},
                                                                        {RPCResult::Type::STR_HEX, "identity_key", /*optional=*/true, "BIP86 x-only identity key"},
                                                                        {RPCResult::Type::STR, "display_name", /*optional=*/true, "Provider display name"},
                                                                        {RPCResult::Type::OBJ, "policy", /*optional=*/true, "Persisted policy", {
                                                                                                                                                    {RPCResult::Type::ARR, "funding_models", "Enabled funding models", {{RPCResult::Type::STR, "", "sponsored or user_paid"}}},
                                                                                                                                                    {RPCResult::Type::STR, "sponsorship_scope", "public or restricted"},
                                                                                                                                                    {RPCResult::Type::NUM, "fee_rate_bps", "User-paid rate in basis points"},
                                                                                                                                                    {RPCResult::Type::NUM, "min_amount_cents", "Minimum recipient amount"},
                                                                                                                                                    {RPCResult::Type::NUM, "max_amount_cents", "Maximum recipient amount"},
                                                                                                                                                    {RPCResult::Type::NUM, "quote_ttl", "Quote lifetime in seconds"},
                                                                                                                                                    {RPCResult::Type::NUM, "maximum_network_fee_dgb_satoshis", "Absolute provider network-fee cap"},
                                                                                                                                                    {RPCResult::Type::STR_HEX, "policy_hash", "Canonical provider policy hash"},
                                                                                                                                                }},
                                                                        {RPCResult::Type::ARR, "readiness_errors", "Outstanding readiness gates", {{RPCResult::Type::STR, "", "Stable readiness error"}}},
                                                                    }},
        RPCExamples{HelpExampleCli("getpaymasterinfo", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            size_t finance_changes{0};
            std::string finance_error;
            if (!ReconcilePaymasterProviderFinances(
                    *wallet, finance_changes, finance_error)) {
                LogPrint(BCLog::DIGIDOLLAR,
                         "Paymaster finance reconciliation skipped in "
                         "getpaymasterinfo: %s\n",
                         finance_error);
            }
            const ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            const bool running = readiness.have_identity && context.paymaster &&
                                 context.paymaster->IsProviderRunning(wallet->GetName(), readiness.identity.provider_id);
            ProviderServiceStatus service_status;
            if (context.paymaster) {
                service_status = context.paymaster->GetProviderServiceStatus(
                    wallet->GetName());
            }
            if (!running) {
                if (readiness.have_settings && readiness.settings.autostart &&
                    wallet->IsLocked()) {
                    service_status.state = ProviderServiceState::WAITING_FOR_UNLOCK;
                } else if (readiness.have_settings &&
                           readiness.settings.autostart && !readiness.ready) {
                    service_status.state = ProviderServiceState::WAITING_FOR_READINESS;
                } else {
                    service_status.state = ProviderServiceState::STOPPED;
                }
            } else if (readiness.settings.operation_mode ==
                       ProviderOperationMode::MANUAL) {
                service_status.state = ProviderServiceState::MANUAL;
            } else if (service_status.state == ProviderServiceState::STOPPED) {
                service_status.state = ProviderServiceState::ACTIVE;
            }

            UniValue pool{UniValue::VOBJ};
            pool.pushKV("entries", static_cast<uint64_t>(readiness.pool_entries.size()));
            pool.pushKV("reserved", static_cast<uint64_t>(std::count_if(
                                        readiness.pool_entries.begin(), readiness.pool_entries.end(), [](const auto& entry) {
                                            return entry.state == DigiDollar::Paymaster::PoolEntryState::RESERVED ||
                                                   entry.state == DigiDollar::Paymaster::PoolEntryState::PENDING_SUCCESSOR ||
                                                   entry.state == DigiDollar::Paymaster::PoolEntryState::COMMITTED;
                                        })));
            pool.pushKV("admission_dgb", static_cast<uint64_t>(readiness.pool.admission_dgb));
            pool.pushKV("admission_carriers", static_cast<uint64_t>(readiness.pool.admission_carriers));
            pool.pushKV("operational_dgb", static_cast<uint64_t>(readiness.pool.operational_dgb));
            pool.pushKV("operational_carriers", static_cast<uint64_t>(readiness.pool.operational_carriers));
            pool.pushKV("complete_operational_slots",
                        static_cast<uint64_t>(readiness.pool.complete_operational_slots));

            UniValue result{UniValue::VOBJ};
            result.pushKV("settings_present", readiness.have_settings);
            result.pushKV("enabled", readiness.settings.enabled);
            result.pushKV("running", running);
            result.pushKV("operation_mode",
                          ProviderOperationModeName(
                              readiness.settings.operation_mode));
            result.pushKV("autostart", readiness.settings.autostart);
            result.pushKV("service_state",
                          ProviderServiceStateName(service_status.state));
            if (!service_status.last_error.empty()) {
                result.pushKV("last_service_error",
                              service_status.last_error);
            }
            ProviderQueueStatus queue_status;
            if (context.paymaster && readiness.have_identity) {
                queue_status = context.paymaster->GetProviderQueueStatus(
                    readiness.identity.provider_id);
            }
            UniValue service_queue{UniValue::VOBJ};
            service_queue.pushKV(
                "waiting_requests",
                static_cast<uint64_t>(queue_status.waiting_requests));
            service_queue.pushKV(
                "waiting_submits",
                static_cast<uint64_t>(queue_status.waiting_submits));
            result.pushKV("service_queue", std::move(service_queue));
            result.pushKV("ready", readiness.ready);
            result.pushKV("wallet_eligible", readiness.wallet_eligible);
            result.pushKV("wallet_locked", wallet->IsLocked());
            result.pushKV("pool_ready", readiness.pool_ready);
            result.pushKV("pool", std::move(pool));
            result.pushKV(
                "liquidity",
                ProviderLiquidityStatusToJSON(readiness, GetTime()));
            if (readiness.have_identity) {
                result.pushKV("provider_id", readiness.identity.provider_id.GetHex());
                result.pushKV("identity_key", HexStr(readiness.identity.identity_key));
                result.pushKV("display_name", readiness.identity.display_name);

                ProviderBackupStatus backup;
                std::string backup_error;
                if (GetPaymasterProviderBackupStatus(
                        *wallet, backup, GetTime(), backup_error)) {
                    UniValue backup_status{UniValue::VOBJ};
                    backup_status.pushKV(
                        "required", ProviderBackupRequired(backup));
                    backup_status.pushKV(
                        "reminder_updated_at", backup.reminder_updated_at);
                    backup_status.pushKV(
                        "last_successful_backup_at",
                        backup.last_successful_backup_at);
                    backup_status.pushKV(
                        "external_backup_acknowledged_at",
                        backup.external_backup_acknowledged_at);
                    result.pushKV("backup_status", std::move(backup_status));
                }

                ProviderFinanceLedger finance;
                if (WalletBatch{wallet->GetDatabase()}
                        .ReadPaymasterFinanceLedger(finance)) {
                    int64_t income{0};
                    int64_t cost{0};
                    uint64_t transfers{0};
                    bool overflow{false};
                    for (const ProviderFinanceDailyTotals& daily :
                         finance.daily_totals) {
                        if (daily.dd_income.value < 0 || daily.dgb_cost.value < 0 ||
                            income > std::numeric_limits<int64_t>::max() -
                                         daily.dd_income.value ||
                            cost > std::numeric_limits<int64_t>::max() -
                                       daily.dgb_cost.value ||
                            transfers > std::numeric_limits<uint64_t>::max() -
                                            daily.successful_transfers) {
                            overflow = true;
                            break;
                        }
                        income += daily.dd_income.value;
                        cost += daily.dgb_cost.value;
                        transfers += daily.successful_transfers;
                    }
                    if (!overflow) {
                        UniValue summary{UniValue::VOBJ};
                        summary.pushKV("service_fee_income_cents", income);
                        summary.pushKV("dgb_operating_cost_satoshis", cost);
                        summary.pushKV("successful_transfers", transfers);
                        summary.pushKV("history_partially_reconstructable",
                                       finance.earlier_history_partial);
                        result.pushKV("finance_summary", std::move(summary));
                    }
                }
            }
            if (readiness.endpoint.IsValid()) {
                result.pushKV("endpoint", readiness.endpoint.ToStringAddrPort());
            }
            uint64_t announcement_sequence{0};
            if (WalletBatch{wallet->GetDatabase()}.ReadPaymasterAnnouncementSequence(
                    announcement_sequence)) {
                result.pushKV("announcement_sequence", announcement_sequence);
            }
            if (readiness.have_policy) result.pushKV("policy", ProviderPolicyToJSON(readiness.policy));
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            return result;
        },
    };
}

} // namespace wallet
