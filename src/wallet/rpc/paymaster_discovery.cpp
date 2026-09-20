// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Offer discovery, quote selection, reputation, and reservation RPCs. */

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

RPCHelpMan getpaymasteroffers()
{
    return RPCHelpMan{
        "getpaymasteroffers",
        "Return a locally filtered and deterministically sorted snapshot of public Paymaster offers.\n"
        "This does not contact a provider or reserve liquidity.\n",
        {
            {"amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Recipient amount, or exact total DD outflow when subtract_paymaster_fee_from_amount is true"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Optional local offer-preview semantics", {
                {"subtract_paymaster_fee_from_amount", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Treat amount_cents as exact total DD outflow and derive the recipient amount"},
            }},
        },
        RPCResult{RPCResult::Type::ARR, "", "Eligible public offers", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Sorted offer", {
                                                                                                                                          {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                                                                          {RPCResult::Type::STR, "display_name", "Untrusted display label"},
                                                                                                                                          {RPCResult::Type::STR, "endpoint", "Authenticated P2P endpoint"},
                                                                                                                                          {RPCResult::Type::STR_HEX, "offer_id", "Public offer identifier"},
                                                                                                                                          {RPCResult::Type::STR_HEX, "policy_hash", "Bound provider policy"},
                                                                                                                                          {RPCResult::Type::STR, "funding_model", "sponsored or user_paid"},
                                                                                                                                          {RPCResult::Type::NUM, "fee_rate_bps", "Service-fee rate in basis points"},
                                                                                                                                          {RPCResult::Type::NUM, "payment_cents", "Requested recipient amount"},
                                                                                                                                          {RPCResult::Type::NUM, "service_fee_cents", "Exact rounded service fee"},
                                                                                                                                          {RPCResult::Type::NUM, "user_total_cents", "Exact total charged to the user"},
                                                                                                                                          {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", "Whether amount_cents was treated as an exact total outflow"},
                                                                                                                                          {RPCResult::Type::NUM_TIME, "expires_at", "Announcement expiration"},
                                                                                                                                          {RPCResult::Type::BOOL, "reputation_sufficient_data", "Whether enough local observations exist"},
                                                                                                                                          {RPCResult::Type::NUM, "success_rate_basis_points", /*optional=*/true, "Observed success rate when statistically meaningful"},
                                                                                                                                          {RPCResult::Type::NUM, "latency_ewma_ms", "Locally observed latency estimate"},
                                                                                                                                      }}}},
        RPCExamples{HelpExampleCli("getpaymasteroffers", "1000") +
                    HelpExampleCli("getpaymasteroffers", "5000 '{\"subtract_paymaster_fee_from_amount\":true}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            if (!context.paymaster || !context.paymaster->Enabled()) {
                throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar Paymaster support is disabled");
            }
            PaymasterStore store{*wallet};
            std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord> records;
            if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            std::map<DigiDollar::Paymaster::PaymasterId,
                     DigiDollar::Paymaster::PaymasterReliabilityRecord>
                reputation;
            for (auto& record : records)
                reputation.emplace(record.provider_id, std::move(record));
            std::string error;
            const int64_t now = GetTime();
            bool subtract_fee{false};
            if (!request.params[1].isNull()) {
                const UniValue& options = request.params[1].get_obj();
                RPCTypeCheckObj(
                    options,
                    {{"subtract_paymaster_fee_from_amount",
                      UniValueType(UniValue::VBOOL)}},
                    /*fAllowNull=*/true, /*fStrict=*/true);
                if (!options.find_value(
                        "subtract_paymaster_fee_from_amount").isNull()) {
                    subtract_fee = options.find_value(
                        "subtract_paymaster_fee_from_amount").get_bool();
                }
            }
            const DigiDollar::Paymaster::DDCents requested_amount{
                request.params[0].getInt<int64_t>()};
            const auto candidates = subtract_fee
                ? DigiDollar::Paymaster::BuildGrossOfferCandidates(
                      context.paymaster->GetDirectory().List(now),
                      requested_amount,
                      DigiDollar::Paymaster::FUNDING_MODEL_ALL,
                      DigiDollar::Paymaster::DDCents{
                          DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS},
                      reputation, now, error)
                : DigiDollar::Paymaster::BuildOfferCandidates(
                      context.paymaster->GetDirectory().List(now),
                      requested_amount,
                      DigiDollar::Paymaster::FUNDING_MODEL_ALL,
                      DigiDollar::Paymaster::DDCents{
                          DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS},
                      reputation, now, error);
            if (!error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            UniValue result{UniValue::VARR};
            for (const auto& candidate : candidates) {
                UniValue offer{UniValue::VOBJ};
                offer.pushKV("provider_id", candidate.provider_id.GetHex());
                offer.pushKV("display_name", candidate.display_name);
                offer.pushKV("endpoint", candidate.endpoint.ToStringAddrPort());
                offer.pushKV("offer_id", candidate.terms.offer_id.GetHex());
                offer.pushKV("policy_hash", candidate.terms.policy_hash.GetHex());
                offer.pushKV("funding_model", candidate.terms.funding_model == DigiDollar::Paymaster::FundingModel::SPONSORED ? "sponsored" : "user_paid");
                offer.pushKV("fee_rate_bps", candidate.terms.fee_rate_bps);
                offer.pushKV("payment_cents", candidate.payment.value);
                offer.pushKV("service_fee_cents", candidate.service_fee.value);
                offer.pushKV("user_total_cents", candidate.user_total.value);
                offer.pushKV("subtract_paymaster_fee_from_amount", subtract_fee);
                offer.pushKV("expires_at", candidate.announcement_expires_at);
                offer.pushKV("reputation_sufficient_data", candidate.reliability.sufficient_data);
                if (candidate.reliability.sufficient_data) {
                    offer.pushKV("success_rate_basis_points", candidate.reliability.success_rate_basis_points);
                }
                offer.pushKV("latency_ewma_ms", candidate.latency_ewma_ms);
                result.push_back(std::move(offer));
            }
            return result;
        },
    };
}

RPCHelpMan requestpaymasterquote()
{
    return RPCHelpMan{
        "requestpaymasterquote",
        "Prepare or resume one idempotent Paymaster quote request. The call never waits for the network.\n",
        {
            {"provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Provider identity from getpaymasteroffers"},
            {"intent", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Locally authorized payment intent", {
                                                                                                         {"request_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Canonical lowercase UUID"},
                                                                                                         {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "DigiDollar recipient address"},
                                                                                                         {"amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Recipient amount in integer cents"},
                                                                                                         {"requested_amount_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Original wallet-local amount; exact total outflow when subtract_paymaster_fee_from_amount is true"},
                                                                                                         {"subtract_paymaster_fee_from_amount", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Bind recipient plus service fee to requested_amount_cents"},
                                                                                                         {"send_all_spendable_dd", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Bind this request to an exact all-spendable-DD input snapshot"},
                                                                                                         {"maximum_paymaster_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Hard service-fee cap"},
                                                                                                         {"fee_mode", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Calling mode: paymaster or auto"},
                                                                                                         {"privacy", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Privacy profile: standard or high"},
                                                                                                         {"selection", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selection policy: lowest_total_cost or privacy_weighted"},
                                                                                                         {"maximum_provider_attempts", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Strict sequential attempt limit"},
                                                                                                         {"offer_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact public offer"},
                                                                                                         {"provider_identity_key", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Required x-only provider identity for a restricted offer"},
                                                                                                         {"restricted_service_descriptor", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Canonical provider-signed restricted descriptor"},
                                                                                                         {"sponsorship_capability", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "One-payment sponsor capability; never persist or place in shell history"},
                                                                                                         {"selected_inputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "Exact USER_DD input set", {{"input", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "", {{"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Input transaction"}, {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "Output index"}}}}},
                                                                                                     }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative asynchronous request state", {
                                                                                            {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                             {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                             {RPCResult::Type::STR_HEX, "canonical_request_hash", "Canonical idempotent request binding"},
                                                                                             {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount; exact total outflow in subtract mode"},
                                                                                             {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the service fee is deducted from requested_amount_cents"},
                                                                                             {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the request is bound to all ordinary spendable DD"},
                                                                                             {RPCResult::Type::STR, "requested_fee_mode", "Requested funding mode"},
                                                                                            {RPCResult::Type::STR, "fee_mode_used", "Authoritative funding mode"},
                                                                                        {RPCResult::Type::STR, "privacy_profile", /*optional=*/true, "Persisted standard or high privacy profile"},
                                                                                        {RPCResult::Type::STR, "to_address", /*optional=*/true, "Canonical recipient from the durable client authorization"},
                                                                                            {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider from the current or persisted authorization"},
                                                                                            {RPCResult::Type::STR_HEX, "offer_id", /*optional=*/true, "Offer from the current or persisted authorization"},
                                                                                            {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Policy from the current or persisted authorization"},
                                                                                            {RPCResult::Type::STR, "funding_model", /*optional=*/true, "Exact sponsored or user_paid funding model"},
                                                                                            {RPCResult::Type::STR, "session_state", "Authoritative wallet session state"},
                                                                                            {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Outstanding asynchronous phase"},
                                                                                            {RPCResult::Type::BOOL, "final", "Whether the session is terminal"},
                                                                                            {RPCResult::Type::STR, "broadcast_state", "not_attempted, unknown, accepted_mempool, accepted_stempool, or confirmed"},
                                                                                            {RPCResult::Type::STR, "confirmation_state", "unconfirmed, payment_confirmed, recovery_confirmed, or conflicted"},
                                                                                            {RPCResult::Type::NUM_TIME, "created_at", "Persistent session creation time"},
                                                                                            {RPCResult::Type::NUM_TIME, "updated_at", "Last durable session update"},
                                                                                            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final payment transaction id"},
                                                                                            {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known idempotent self-recovery transaction id"},
                                                                                            {RPCResult::Type::ARR, "reserved_user_inputs", "Hard-reserved USER_DD inputs", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Reserved input", {
                                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "txid", "Input transaction"},
                                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "vout", "Input output index"},
                                                                                                                                                                                                                                             }}}},
                                                                                            {RPCResult::Type::NUM, "provider_attempts", "Number of persistent provider attempts"},
                                                                                            {RPCResult::Type::NUM, "payment_cents", /*optional=*/true, "Recipient amount from the current or persisted authorization"},
                                                                                            {RPCResult::Type::NUM, "service_fee_cents", /*optional=*/true, "Exact service fee from the current or persisted authorization"},
                                                                                            {RPCResult::Type::NUM, "user_total_cents", /*optional=*/true, "Total DD payment and fee from the current or persisted authorization"},
                                                                                            {RPCResult::Type::NUM_TIME, "expires_at", /*optional=*/true, "Intent expiration; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::BOOL, "queued", /*optional=*/true, "Whether the request is queued to a connected v2 peer"},
                                                                                            {RPCResult::Type::BOOL, "connection_pending", /*optional=*/true, "Whether a short-lived connection was requested"},
                                                                                            {RPCResult::Type::BOOL, "capacity_pending", /*optional=*/true, "Whether only the privacy-preserving capacity handshake is pending"},
                                                                                            {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Fully validated provider capacity snapshot"},
                                                                                            {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Validated provider quote"},
                                                                                            {RPCResult::Type::STR_HEX, "unsigned_txid", /*optional=*/true, "Validated unsigned transaction"},
                                                                                            {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Validated exact template"},
                                                                                            {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Opaque commitment to the exact validated client authorization manifest"},
                                                                                            {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment has been durably accepted"},
                                                                                            {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Monotonic durable acceptance time"},
                                                                                            {RPCResult::Type::STR, "psbt", /*optional=*/true, "Canonical unsigned collaborative PSBT"},
                                                                                        }},
        RPCExamples{HelpExampleCli("requestpaymasterquote", "\"0123...\" '{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\","
                                                            "\"address\":\"DD...\",\"amount_cents\":1000,"
                                                            "\"maximum_paymaster_fee_cents\":25,"
                                                            "\"selected_inputs\":[{\"txid\":\"abcd...\",\"vout\":0}]}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::string readiness_error;
            if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
                throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
            }
            const UniValue& options = request.params[1];
            RPCTypeCheckObj(options,
                            {{"request_id", UniValueType(UniValue::VSTR)},
                             {"address", UniValueType(UniValue::VSTR)},
                             {"amount_cents", UniValueType(UniValue::VNUM)},
                             {"requested_amount_cents", UniValueType(UniValue::VNUM)},
                             {"subtract_paymaster_fee_from_amount", UniValueType(UniValue::VBOOL)},
                             {"send_all_spendable_dd", UniValueType(UniValue::VBOOL)},
                             {"maximum_paymaster_fee_cents", UniValueType(UniValue::VNUM)},
                             {"fee_mode", UniValueType(UniValue::VSTR)},
                             {"privacy", UniValueType(UniValue::VSTR)},
                             {"selection", UniValueType(UniValue::VSTR)},
                             {"maximum_provider_attempts", UniValueType(UniValue::VNUM)},
                             {"offer_id", UniValueType(UniValue::VSTR)},
                             {"provider_identity_key", UniValueType(UniValue::VSTR)},
                             {"restricted_service_descriptor", UniValueType(UniValue::VSTR)},
                             {"sponsorship_capability", UniValueType(UniValue::VSTR)},
                             {"selected_inputs", UniValueType(UniValue::VARR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const PaymasterId provider_id = ParseHashV(request.params[0], "provider_id");
            const std::string request_id = options.find_value("request_id").get_str();
            if (!IsCanonicalRequestId(request_id)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "request_id must be a canonical lowercase UUID");
            }
            const int64_t payment = options.find_value("amount_cents").getInt<int64_t>();
            const bool subtract_fee =
                !options.find_value("subtract_paymaster_fee_from_amount").isNull() &&
                options.find_value("subtract_paymaster_fee_from_amount").get_bool();
            const bool send_all =
                !options.find_value("send_all_spendable_dd").isNull() &&
                options.find_value("send_all_spendable_dd").get_bool();
            const int64_t requested_amount =
                options.find_value("requested_amount_cents").isNull()
                    ? payment
                    : options.find_value("requested_amount_cents").getInt<int64_t>();
            const int64_t fee_cap = options.find_value("maximum_paymaster_fee_cents").getInt<int64_t>();
            if (payment < 100 || payment > MAX_DD_OUTPUT_CENTS ||
                requested_amount < 100 || requested_amount > MAX_DD_OUTPUT_CENTS ||
                fee_cap < 0 || fee_cap > MAX_DD_OUTPUT_CENTS) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Paymaster amount or fee cap is out of range");
            }
            if (send_all && !subtract_fee) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "send_all_spendable_dd requires subtract_paymaster_fee_from_amount");
            }
            if ((!subtract_fee && requested_amount != payment) ||
                (subtract_fee && requested_amount < payment)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_INVALID_CLIENT_PAYMENT_ORDER");
            }
            FeeMode requested_mode{FeeMode::PAYMASTER};
            if (!options.find_value("fee_mode").isNull()) {
                const std::string mode = options.find_value("fee_mode").get_str();
                if (mode == "auto")
                    requested_mode = FeeMode::AUTO;
                else if (mode != "paymaster") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_mode must be paymaster or auto");
                }
            }
            PrivacyProfile privacy{PrivacyProfile::STANDARD};
            if (!options.find_value("privacy").isNull()) {
                const std::string profile = options.find_value("privacy").get_str();
                if (profile == "high")
                    privacy = PrivacyProfile::HIGH;
                else if (profile != "standard") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "privacy must be standard or high");
                }
            }
            SelectionMode selection{SelectionMode::LOWEST_TOTAL_COST};
            if (!options.find_value("selection").isNull()) {
                const std::string policy = options.find_value("selection").get_str();
                if (policy == "privacy_weighted")
                    selection = SelectionMode::PRIVACY_WEIGHTED;
                else if (policy != "lowest_total_cost") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "selection must be lowest_total_cost or privacy_weighted");
                }
            }
            const int maximum_attempts = options.find_value("maximum_provider_attempts").isNull() ? ((privacy == PrivacyProfile::HIGH ||
                                                                                                      !options.find_value("sponsorship_capability").isNull()) ?
                                                                                                         1 :
                                                                                                         3) :
                                                                                                    options.find_value("maximum_provider_attempts").getInt<int>();
            if (maximum_attempts < 1 || maximum_attempts > 16 ||
                (privacy == PrivacyProfile::HIGH && maximum_attempts != 1)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy");
            }
            if (!CheckPaymasterPrivacyReadiness(privacy, context, readiness_error)) {
                throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
            }
            const std::vector<COutPoint> requested_inputs =
                ParsePaymasterInputs(options.find_value("selected_inputs"));
            const std::string address = options.find_value("address").get_str();
            const CDigiDollarAddress dd_address{address};
            if (!dd_address.IsValidForCurrentNetwork()) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid DigiDollar address for this network");
            }
            const CScript recipient_script = GetScriptForDestination(dd_address.GetDigiDollarDestination());
            const std::optional<uint256> requested_offer = options.find_value("offer_id").isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "offer_id")};

            const bool have_restricted_key = !options.find_value("provider_identity_key").isNull();
            const bool have_restricted_descriptor =
                !options.find_value("restricted_service_descriptor").isNull();
            const bool have_restricted_capability =
                !options.find_value("sponsorship_capability").isNull();
            const bool restricted = have_restricted_key || have_restricted_descriptor ||
                                    have_restricted_capability;
            if (restricted && !(have_restricted_key && have_restricted_descriptor &&
                                have_restricted_capability)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "provider_identity_key, restricted_service_descriptor, and sponsorship_capability "
                                   "must be supplied together");
            }
            if (restricted && maximum_attempts != 1) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Restricted sponsorship requires maximum_provider_attempts=1");
            }
            std::optional<XOnlyPubKey> restricted_identity_key;
            std::optional<RestrictedServiceDescriptor> restricted_descriptor;
            std::optional<SponsorshipCapability> restricted_capability;
            std::string error;
            PaymasterStore store{*wallet};
            const int64_t now = GetTime();
            PaymentSession durable_session;
            const bool have_durable_session =
                store.GetSessionByRequestId(request_id, durable_session);
            if (have_durable_session && !IsTerminal(durable_session.state)) {
                ProviderAttempt durable_attempt;
                PaymentIntent durable_intent;
                PaymasterQuote durable_quote;
                if (FindDurableClientAuthorizationAttempt(
                        *wallet, store, durable_session, GetTime(), durable_attempt,
                        durable_intent, durable_quote, error)) {
                    if (durable_attempt.provider_id != provider_id ||
                        durable_intent.request_id != request_id ||
                        durable_intent.session_id != durable_session.session_id ||
                        durable_intent.canonical_request_hash !=
                            durable_session.canonical_request_hash ||
                        durable_intent.requested_fee_mode != requested_mode ||
                        durable_intent.privacy_profile != privacy ||
                        durable_intent.selection_mode != selection ||
                        durable_attempt.privacy_profile != privacy ||
                        durable_intent.recipient_script != recipient_script ||
                        durable_intent.recipient_amount != DDCents{payment} ||
                        (durable_session.requested_amount.value == 0
                             ? (subtract_fee || send_all || requested_amount != payment)
                             : (durable_session.requested_amount != DDCents{requested_amount} ||
                                durable_session.subtract_paymaster_fee_from_amount != subtract_fee ||
                                durable_session.send_all_spendable_dd != send_all)) ||
                        durable_intent.user_dd_inputs != requested_inputs ||
                        durable_quote.service_fee.value > fee_cap ||
                        (requested_offer &&
                         durable_intent.offer_id != *requested_offer)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
                    }
                    if (!DrainClientSessionEquivocations(
                            *context.paymaster, store, durable_session,
                            error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    return DurableClientAuthorizationToJSON(
                        durable_session, durable_attempt, durable_intent,
                        durable_quote);
                }
                if (!error.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, durable_session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (restricted) {
                const std::vector<unsigned char> key_bytes =
                    ParseHexV(options.find_value("provider_identity_key"), "provider_identity_key");
                if (key_bytes.size() != 32) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "provider_identity_key must be exactly 32 bytes");
                }
                restricted_identity_key.emplace(MakeUCharSpan(key_bytes));
                RestrictedServiceDescriptor descriptor;
                SponsorshipCapability capability;
                if (!restricted_identity_key->IsFullyValid() ||
                    !DeserializePaymasterHex(options.find_value("restricted_service_descriptor"),
                                             "RESTRICTED_DESCRIPTOR", descriptor, error) ||
                    !DeserializePaymasterHex(options.find_value("sponsorship_capability"),
                                             "SPONSORSHIP_CAPABILITY", capability, error)) {
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                       error.empty() ? "PAYMASTER_INVALID_RESTRICTED_IDENTITY" : error);
                }
                if (provider_id != descriptor.provider_id ||
                    (requested_offer && *requested_offer != descriptor.offer_id) ||
                    !ValidateRestrictedServiceDescriptor(descriptor, *restricted_identity_key,
                                                         Params().GenesisBlock().GetHash(), GetTime(), error,
                                                         Params().GetChainType() == ChainType::REGTEST) ||
                    !ValidateSponsorshipCapability(capability, descriptor, recipient_script,
                                                   DDCents{payment}, capability.payment_request_nonce,
                                                   Params().GenesisBlock().GetHash(), GetTime(), error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       error.empty() ? "PAYMASTER_RESTRICTED_BINDING_MISMATCH" : error);
                }
                node::NodeContext* node = wallet->chain().context();
                if (!node || !node->chainman ||
                    !ValidateRestrictedAdmissionProofs(descriptor, *restricted_identity_key,
                                                       *node->chainman, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       error.empty() ? "PAYMASTER_RESTRICTED_ADMISSION_UNAVAILABLE" : error);
                }
                if (privacy == PrivacyProfile::HIGH && !descriptor.p2p_endpoint.IsTor()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "High privacy requires an onion restricted provider");
                }
                restricted_descriptor = std::move(descriptor);
                restricted_capability = std::move(capability);
            }

            // Resolve an exact terminal retry from local durable authority
            // before consulting the live directory. The provider resources
            // used by the completed transaction are expected to be spent, so
            // requiring a fresh announcement/capacity check here would make
            // the idempotent tombstone dependent on remote state. All fields
            // which define the local order are still committed below; a hash
            // mismatch remains a hard conflict in CreateOrJoinSession().
            HashWriter request_hasher = TaggedHash("DigiByte Paymaster RPC Request v1");
            // The session hash commits the provider-independent local order.
            // In subtract mode the exact provider-dependent recipient and fee
            // remain bound by each signed intent/quote/authorization manifest,
            // allowing a safe pre-signature provider fallback to produce a
            // new exact confirmation under the same gross order.
            request_hasher << request_id << recipient_script
                           << DDCents{subtract_fee ? requested_amount : payment}
                           << DDCents{fee_cap} << requested_inputs
                           << static_cast<uint8_t>(requested_mode)
                           << static_cast<uint8_t>(privacy)
                           << static_cast<uint8_t>(selection) << maximum_attempts;
            if (subtract_fee || send_all) {
                request_hasher << std::string{"gross-amount-v1"}
                               << DDCents{requested_amount}
                               << subtract_fee << send_all;
            }
            if (restricted) {
                request_hasher << *restricted_identity_key
                               << GetRestrictedDescriptorSignatureHash(*restricted_descriptor)
                               << GetSponsorshipCapabilityHash(*restricted_capability);
            }
            const uint256 canonical_request_hash = request_hasher.GetSHA256();
            if (have_durable_session && IsTerminal(durable_session.state)) {
                if (durable_session.canonical_request_hash !=
                        canonical_request_hash ||
                    durable_session.fee_mode_requested != requested_mode) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_REQUEST_ID_CONFLICT");
                }
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, durable_session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                return SessionToJSON(durable_session, &store);
            }

            const DDCents effective_fee_cap =
                EffectiveClientServiceFeeCap(*wallet, DDCents{fee_cap}, error);
            if (effective_fee_cap.value < 0) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            std::vector<PaymasterReliabilityRecord> records;
            if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            std::map<PaymasterId, PaymasterReliabilityRecord> reputation;
            for (auto& record : records)
                reputation.emplace(record.provider_id, std::move(record));
            OfferCandidate candidate;
            if (restricted) {
                const auto& descriptor = *restricted_descriptor;
                candidate.provider_id = descriptor.provider_id;
                candidate.identity_key = *restricted_identity_key;
                candidate.display_name = descriptor.sponsor_display_name;
                candidate.endpoint = descriptor.p2p_endpoint;
                candidate.announcement_sequence = descriptor.admission_sequence;
                candidate.announcement_expires_at = descriptor.expires_at;
                candidate.terms = OfferTerms{descriptor.offer_id, descriptor.policy_hash,
                                             FundingModel::SPONSORED,
                                             SponsorshipScope::RESTRICTED, 0,
                                             DDCents{payment}, DDCents{payment}};
                candidate.payment = DDCents{payment};
                candidate.service_fee = DDCents{0};
                candidate.user_total = DDCents{payment};
            } else {
                const auto candidates = BuildOfferCandidates(
                    context.paymaster->GetDirectory().List(now), DDCents{payment}, FUNDING_MODEL_ALL,
                    effective_fee_cap, reputation, now, error);
                if (!error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                const auto candidate_it = std::find_if(candidates.begin(), candidates.end(),
                                                       [&](const OfferCandidate& entry) {
                                                           return entry.provider_id == provider_id &&
                                                                  (privacy != PrivacyProfile::HIGH || entry.endpoint.IsTor()) &&
                                                                  (!requested_offer || entry.terms.offer_id == *requested_offer);
                                                       });
                if (candidate_it == candidates.end()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_REQUESTED_OFFER_UNAVAILABLE");
                }
                candidate = *candidate_it;
            }
            if ((subtract_fee &&
                 candidate.user_total != DDCents{requested_amount}) ||
                (!subtract_fee && candidate.payment != DDCents{requested_amount})) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_NO_EXACT_GROSS_OFFER");
            }

            PaymentSession session;
            const auto create_result = store.CreateOrJoinSession(
                request_id, canonical_request_hash, requested_mode, now, session, error);
            if (create_result == CreatePaymasterSessionResult::CONFLICT ||
                create_result == CreatePaymasterSessionResult::DATABASE_ERROR) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            if (create_result == CreatePaymasterSessionResult::FINAL_TOMBSTONE) {
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                return SessionToJSON(session, &store);
            }
            if (!store.BindClientPaymentOrder(
                    request_id, DDCents{requested_amount}, subtract_fee,
                    send_all, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            session.requested_amount = DDCents{requested_amount};
            session.subtract_paymaster_fee_from_amount = subtract_fee;
            session.send_all_spendable_dd = send_all;

            ProviderAttempt attempt;
            PaymentIntent intent;
            bool resume_attempt{false};
            for (auto it = session.attempt_ids.rbegin(); it != session.attempt_ids.rend(); ++it) {
                ProviderAttempt existing;
                if (!store.GetAttempt(*it, existing) || existing.unsigned_intent.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                PaymentIntent existing_intent;
                try {
                    SpanReader stream{::PROTOCOL_VERSION, existing.unsigned_intent};
                    stream >> existing_intent;
                    if (!stream.empty()) throw std::ios_base::failure("trailing intent data");
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                if (existing.provider_id == provider_id &&
                    existing_intent.offer_id == candidate.terms.offer_id &&
                    existing.state != AttemptState::REJECTED &&
                    existing.state != AttemptState::QUOTE_EXPIRED) {
                    if (existing_intent.canonical_request_hash != canonical_request_hash ||
                        existing_intent.requested_fee_mode != requested_mode ||
                        existing_intent.privacy_profile != privacy ||
                        existing_intent.selection_mode != selection ||
                        existing.privacy_profile != privacy) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
                    }
                    attempt = std::move(existing);
                    intent = std::move(existing_intent);
                    resume_attempt = true;
                    break;
                }
            }
            if (!resume_attempt) {
                if (session.attempt_ids.size() >= static_cast<size_t>(maximum_attempts)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_MAXIMUM_PROVIDER_ATTEMPTS_REACHED");
                }
                if (!session.attempt_ids.empty() &&
                    session.state != SessionState::INPUTS_RESERVED) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FALLBACK_REQUIRES_EXPLICIT_ABANDON");
                }
                DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
                if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
                std::vector<COutPoint> selected_inputs;
                std::vector<CAmount> selected_amounts;
                CAmount selected_total{0};
                if (!session.attempt_ids.empty()) {
                    if (requested_inputs != session.user_inputs) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_FALLBACK_INPUT_CONFLICT");
                    }
                    selected_inputs = session.user_inputs;
                    for (const COutPoint& outpoint : selected_inputs) {
                        const CAmount input_amount = dd_wallet->GetDDFromUTXO(outpoint);
                        if (input_amount <= 0 ||
                            input_amount > std::numeric_limits<CAmount>::max() - selected_total) {
                            throw JSONRPCError(RPC_WALLET_ERROR,
                                               "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
                        }
                        selected_amounts.push_back(input_amount);
                        selected_total += input_amount;
                    }
                    if (selected_total < candidate.user_total.value) {
                        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                                           "PAYMASTER_FALLBACK_FEE_EXCEEDS_RESERVED_INPUTS");
                    }
                } else if (!dd_wallet->SelectDDCoins(candidate.user_total.value, requested_inputs,
                                                     selected_inputs, selected_total,
                                                     &selected_amounts, &error)) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, error);
                }
                CScript change_script;
                if (selected_total > candidate.user_total.value) {
                    const auto change = wallet->GetNewDestination(OutputType::BECH32M,
                                                                  "Paymaster DD change");
                    if (!change) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                    change_script = GetScriptForDestination(*change);
                }
                PaymentIntentParameters parameters;
                parameters.genesis_hash = Params().GenesisBlock().GetHash();
                parameters.request_id = request_id;
                parameters.session_id = session.session_id;
                parameters.canonical_request_hash = canonical_request_hash;
                parameters.requested_fee_mode = requested_mode;
                parameters.privacy_profile = privacy;
                parameters.selection_mode = selection;
                if (restricted) {
                    parameters.client_nonce = restricted_capability->payment_request_nonce;
                    parameters.sponsorship_authorization_hash =
                        GetSponsorshipCapabilityHash(*restricted_capability);
                } else {
                    do {
                        parameters.client_nonce = GetRandHash();
                    } while (parameters.client_nonce.IsNull());
                }
                parameters.user_dd_inputs = selected_inputs;
                parameters.recipient_script = recipient_script;
                parameters.user_dd_change_script = change_script;
                parameters.expires_at = std::min(
                    SaturatingAddSeconds(now, DEFAULT_QUOTE_TTL_SECONDS),
                    candidate.announcement_expires_at);
                auto unsigned_intent = BuildUnsignedPaymentIntent(candidate, parameters, now, error);
                if (!unsigned_intent) throw JSONRPCError(RPC_WALLET_ERROR, error);
                intent = std::move(*unsigned_intent);
                do {
                    attempt.attempt_id = GetRandHash();
                } while (attempt.attempt_id.IsNull());
                attempt.provider_id = provider_id;
                attempt.provider_identity_key = candidate.identity_key;
                attempt.provider_endpoint = candidate.endpoint.ToStringAddrPort();
                attempt.privacy_profile = privacy;
                attempt.client_nonce = intent.client_nonce;
                attempt.sponsorship_capability_hash = intent.sponsorship_authorization_hash;
                attempt.unsigned_intent = SerializePaymentIntent(intent);
                attempt.created_at = attempt.updated_at = now;
                if (!store.PreparePaymentIntent(request_id, attempt, now, error) ||
                    !store.GetSessionByRequestId(request_id, session)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            node::NodeContext* node = wallet->chain().context();
            if (!node || !node->connman || !node->chainman) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
            }
            PaymasterCapacityRequest capacity_request;
            if (attempt.capacity_request.empty()) {
                capacity_request.genesis_hash = Params().GenesisBlock().GetHash();
                capacity_request.provider_id = provider_id;
                capacity_request.request_id = request_id;
                capacity_request.session_id = session.session_id;
                capacity_request.client_nonce = attempt.client_nonce;
                capacity_request.funding_model = intent.funding_model;
                capacity_request.requires_carrier =
                    intent.funding_model == FundingModel::USER_PAID &&
                    candidate.service_fee.value > 0 &&
                    candidate.service_fee.value < 100;
                capacity_request.requested_slots = 1;
                capacity_request.created_at = now;
                capacity_request.expires_at = std::min(
                    intent.expires_at,
                    SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
                if (capacity_request.expires_at <= now) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_CAPACITY_REQUEST_EXPIRED");
                }
                const std::vector<unsigned char> capacity_bytes =
                    SerializeCapacityRequest(capacity_request);
                if (!store.PrepareCapacityRequest(request_id, attempt.attempt_id,
                                                  capacity_bytes, now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       error.empty() ? "PAYMASTER_ATTEMPT_NOT_FOUND" : error);
                }
            } else {
                try {
                    CDataStream stream{attempt.capacity_request, SER_NETWORK,
                                       ::PROTOCOL_VERSION};
                    stream >> capacity_request;
                    if (!stream.empty() ||
                        SerializeCapacityRequest(capacity_request) !=
                            attempt.capacity_request) {
                        throw std::ios_base::failure("non-canonical capacity request");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_CAPACITY_REQUEST_CORRUPT");
                }
            }

            PaymasterCapacityProof capacity_proof;
            bool capacity_ready = !attempt.capacity_snapshot.snapshot_id.IsNull();
            if (!capacity_ready) {
                // Persist a simultaneously queued signed conflict before the
                // acceptance loop can discard either locally invalid proof.
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                auto proof_lease = context.paymaster->LeaseCapacityProofs(
                    provider_id, attempt.client_nonce,
                    MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
                const auto proofs = SelectCapacityProofMessages(
                    proof_lease.Messages(), provider_id, request_id,
                    session.session_id, attempt.client_nonce);
                bool capacity_equivocation{false};
                if (!PersistPendingCapacityEquivocationPair(
                        store, attempt.attempt_id, proofs,
                        capacity_equivocation, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (capacity_equivocation) {
                    if (!AcknowledgeEquivocationMessages(
                            *context.paymaster, proofs, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_CAPACITY_EQUIVOCATION");
                }
                std::string rejected_proof_error;
                for (const DirectMessage& direct : proofs) {
                    const auto* received =
                        std::get_if<PaymasterCapacityProof>(&direct.payload);
                    if (!received) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_proof_error =
                            "PAYMASTER_INVALID_CAPACITY_PROOF";
                        continue;
                    }

                    bool claim_equivocation{false};
                    const std::vector<unsigned char> encoded_proof =
                        SerializeCapacityProof(*received);
                    if (!store.StageCapacityProofClaimCandidate(
                            attempt.attempt_id, encoded_proof,
                            direct.received_at, claim_equivocation, error)) {
                        if (error !=
                            "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_proof_error =
                            "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
                        continue;
                    }
                    if (claim_equivocation) {
                        if (!AcknowledgeEquivocationMessages(
                                *context.paymaster, proofs, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_CAPACITY_EQUIVOCATION");
                    }
                    if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_ATTEMPT_NOT_FOUND");
                    }

                    std::string proof_error;
                    if (!ValidateCapacityProofAgainstChainstate(
                            *received, capacity_request,
                            attempt.provider_identity_key, *node->chainman,
                            now, proof_error)) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_proof_error =
                            proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : std::move(proof_error);
                        continue;
                    }
                    capacity_proof = *received;
                    ValidatedCapacitySnapshot snapshot;
                    snapshot.snapshot_id = capacity_proof.snapshot_id;
                    snapshot.resource_commitment =
                        GetCapacityResourceCommitment(capacity_proof);
                    snapshot.provider_id = capacity_proof.provider_id;
                    snapshot.client_nonce = capacity_proof.client_nonce;
                    snapshot.funding_model = capacity_proof.funding_model;
                    snapshot.requires_carrier = capacity_proof.requires_carrier;
                    snapshot.request_hash = Hash(attempt.capacity_request);
                    snapshot.capacity_proof = SerializeCapacityProof(capacity_proof);
                    snapshot.created_at = capacity_proof.created_at;
                    snapshot.expires_at = capacity_proof.expires_at;
                    snapshot.validated_at = now;
                    if (!store.CommitValidatedCapacitySnapshot(
                            request_id, attempt.attempt_id, snapshot, now, error) ||
                        !store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    capacity_ready = true;
                    break;
                }
                if (!capacity_ready && !rejected_proof_error.empty()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       rejected_proof_error);
                }
            }
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            if (!capacity_ready) {
                NodeId peer_id{-1};
                node->connman->ForEachNode([&](CNode* peer) {
                    if (peer_id == -1 && peer->IsPaymasterConn() &&
                        peer->addr == candidate.endpoint) {
                        peer_id = peer->GetId();
                    }
                });
                bool capacity_queued{false};
                bool capacity_connection_pending{false};
                if (peer_id == -1) {
                    capacity_connection_pending = node->connman->AddConnection(
                        candidate.endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
                        /*paymaster_high_privacy=*/privacy == PrivacyProfile::HIGH);
                } else {
                    const uint256 message_id = Hash(attempt.capacity_request);
                    capacity_queued =
                        context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                        context.paymaster->QueueCapacityRequest(
                            peer_id, message_id, attempt.capacity_request.size(),
                            capacity_request, now);
                    if (!capacity_queued) {
                        throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED,
                                           "PAYMASTER_DIRECT_QUEUE_FULL");
                    }
                    node->connman->WakeMessageHandler();
                }
                if (!store.GetSessionByRequestId(request_id, session)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_SESSION_NOT_FOUND");
                }
                UniValue result = SessionToJSON(session);
                result.pushKV("provider_id", provider_id.GetHex());
                result.pushKV("offer_id", candidate.terms.offer_id.GetHex());
                result.pushKV("policy_hash", candidate.terms.policy_hash.GetHex());
                result.pushKV("funding_model",
                              intent.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
                result.pushKV("payment_cents", candidate.payment.value);
                result.pushKV("service_fee_cents", candidate.service_fee.value);
                result.pushKV("user_total_cents", candidate.user_total.value);
                result.pushKV("expires_at", intent.expires_at);
                result.pushKV("queued", capacity_queued);
                result.pushKV("connection_pending", capacity_connection_pending);
                result.pushKV("capacity_pending", true);
                return result;
            }
            try {
                CDataStream stream{attempt.capacity_snapshot.capacity_proof,
                                   SER_NETWORK, ::PROTOCOL_VERSION};
                stream >> capacity_proof;
                if (!stream.empty() ||
                    SerializeCapacityProof(capacity_proof) !=
                        attempt.capacity_snapshot.capacity_proof) {
                    throw std::ios_base::failure("non-canonical capacity proof");
                }
            } catch (const std::ios_base::failure&) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PERSISTED_CAPACITY_PROOF_CORRUPT");
            }
            // Re-run the same full firewall immediately before any intent
            // signature or network disclosure. A now-spent capacity outpoint
            // invalidates the attempt without exposing the user's intent.
            if (!ValidateCapacityProofAgainstChainstate(
                    capacity_proof, capacity_request, attempt.provider_identity_key,
                    *node->chainman, now, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            bool queued{false};
            bool connection_pending{false};
            bool quote_received{false};
            PaymasterQuoteRequest quote_request;
            std::vector<unsigned char> request_bytes;
            if (attempt.quote_request.empty()) {
                if (wallet->IsLocked()) {
                    if (session.state == SessionState::INPUTS_RESERVED &&
                        !store.TransitionSession(request_id, SessionState::AWAITING_WALLET_UNLOCK,
                                                 PendingPhase::NONE, {}, now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                } else {
                    if ((session.state == SessionState::INPUTS_RESERVED ||
                         session.state == SessionState::AWAITING_WALLET_UNLOCK) &&
                        !store.TransitionSession(request_id, SessionState::AWAITING_USER_SIGNATURE,
                                                 PendingPhase::NONE, {}, now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    if (!DrainClientAttemptEquivocations(
                            *context.paymaster, store, session, attempt,
                            error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    std::vector<XOnlyPubKey> output_keys;
                    if (!SignPaymentIntentInputs(*wallet, intent, now, output_keys, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    std::vector<std::vector<unsigned char>> signatures;
                    for (const auto& proof : intent.user_input_proofs)
                        signatures.push_back(proof.signature);
                    intent.user_input_proofs.clear();
                    auto finalized = FinalizeQuoteRequest(
                        intent, output_keys, signatures, restricted_descriptor,
                        restricted_capability, now, error);
                    if (!finalized) throw JSONRPCError(RPC_WALLET_ERROR, error);
                    quote_request = std::move(*finalized);
                    request_bytes = SerializeQuoteRequest(quote_request);
                    if (!store.FinalizePaymentIntent(request_id, attempt.attempt_id,
                                                     SerializeRedactedQuoteRequest(quote_request),
                                                     now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_ATTEMPT_NOT_FOUND");
                    }
                }
            } else {
                try {
                    CDataStream stream{attempt.quote_request, SER_NETWORK, ::PROTOCOL_VERSION};
                    stream >> quote_request;
                    if (!stream.empty()) throw std::ios_base::failure("trailing request data");
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PERSISTED_REQUEST_CORRUPT");
                }
                quote_request.restricted_descriptor = restricted_descriptor;
                quote_request.restricted_capability = restricted_capability;
                if (!ValidateQuoteRequestEnvelope(quote_request, Params().GenesisBlock().GetHash(),
                                                  now, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                request_bytes = SerializeQuoteRequest(quote_request);
            }

            if (!request_bytes.empty()) {
                // Pairwise equivocation is signature/binding evidence, not an
                // authorization decision. Record it before any rejected quote
                // is acknowledged by the deeper local spend firewall.
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                auto response_lease = context.paymaster->LeaseQuoteResponses(
                    request_id, session.session_id, provider_id,
                    GetPaymentIntentHash(quote_request.intent),
                    MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
                const auto& responses = response_lease.Messages();
                bool quote_equivocation{false};
                if (!PersistPendingQuoteEquivocationPair(
                        store, attempt.attempt_id, responses,
                        quote_equivocation, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (quote_equivocation) {
                    if (!AcknowledgeEquivocationMessages(
                            *context.paymaster, responses, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_QUOTE_EQUIVOCATION");
                }
                std::string rejected_quote_error;
                int rejected_quote_code{RPC_INVALID_PARAMETER};
                for (const DirectMessage& direct : responses) {
                    const auto* response =
                        std::get_if<PaymasterQuoteResponse>(&direct.payload);
                    if (!response) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_quote_error =
                            "PAYMASTER_INVALID_QUOTE_RESPONSE";
                        continue;
                    }

                    bool claim_equivocation{false};
                    const std::vector<unsigned char> encoded_quote =
                        SerializeQuoteResponse(*response);
                    if (!store.StageQuoteResponseClaimCandidate(
                            attempt.attempt_id, encoded_quote,
                            direct.received_at, claim_equivocation, error)) {
                        if (error !=
                            "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE") {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_quote_error =
                            "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
                        continue;
                    }
                    if (claim_equivocation) {
                        if (!AcknowledgeEquivocationMessages(
                                *context.paymaster, responses, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_QUOTE_EQUIVOCATION");
                    }
                    if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_ATTEMPT_NOT_FOUND");
                    }

                    CollaborativePSBTTemplate trusted;
                    ClientAuthorizationManifest client_manifest;
                    std::string response_error;
                    int response_error_code{RPC_INVALID_PARAMETER};
                    bool valid_response{
                        ValidateQuoteResponseForRequest(*response, quote_request, candidate.terms, candidate.identity_key, effective_fee_cap, now, response_error) &&
                        ValidateQuoteAgainstCapacitySnapshot(
                            response->quote, attempt.capacity_snapshot,
                            response_error) &&
                        BuildTrustedQuoteTemplate(
                            *wallet, quote_request.intent, response->quote,
                            Params(), *node->chainman, trusted,
                            response_error) &&
                        BuildClientAuthorizationManifest(
                            quote_request.intent, response->quote,
                            attempt.capacity_snapshot, trusted,
                            effective_fee_cap,
                            session.requested_amount.value > 0
                                ? session.requested_amount
                                : quote_request.intent.recipient_amount,
                            session.subtract_paymaster_fee_from_amount,
                            session.send_all_spendable_dd, client_manifest,
                            response_error)};
                    if (valid_response &&
                        !ValidateClientAuthorizationOwnership(
                            *wallet, client_manifest, response_error)) {
                        valid_response = false;
                        response_error_code = RPC_WALLET_ERROR;
                    }
                    if (!valid_response) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_quote_error =
                            response_error.empty() ? "PAYMASTER_INVALID_QUOTE_RESPONSE" : std::move(response_error);
                        rejected_quote_code = response_error_code;
                        continue;
                    }
                    attempt.state = AttemptState::QUOTED;
                    attempt.intent_hash = response->quote.intent_hash;
                    attempt.quote_id = response->quote.quote_id;
                    attempt.unsigned_txid = response->quote.unsigned_txid;
                    attempt.template_commitment = response->quote.template_commitment;
                    attempt.commit_key = GetPaymasterCommitKey(
                        provider_id, attempt.client_nonce, attempt.intent_hash,
                        attempt.quote_id, attempt.template_commitment);
                    attempt.signed_quote = SerializeQuoteResponse(*response);
                    attempt.client_manifest = std::move(client_manifest);
                    attempt.unsigned_transaction = SerializeTransaction(response->quote.unsigned_transaction);
                    attempt.unsigned_psbt = SerializePSBT(trusted.psbt);
                    attempt.input_roles.clear();
                    attempt.input_roles.reserve(trusted.input_roles.size());
                    static_assert(static_cast<uint8_t>(InputRole::USER_DD) ==
                                  static_cast<uint8_t>(ReservationRole::USER_DD));
                    static_assert(static_cast<uint8_t>(InputRole::PROVIDER_DGB) ==
                                  static_cast<uint8_t>(ReservationRole::PROVIDER_DGB));
                    for (const InputRole role : trusted.input_roles) {
                        attempt.input_roles.push_back(static_cast<ReservationRole>(role));
                    }
                    attempt.quote_expires_at = response->quote.expires_at;
                    attempt.retry_until = response->quote.retry_until;
                    attempt.updated_at = std::max(attempt.updated_at, now);
                    if (!store.UpdateAttempt(request_id, attempt, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    quote_received = true;
                    if (!DrainClientAttemptEquivocations(
                            *context.paymaster, store, session, attempt,
                            error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    break;
                }
                if (!quote_received && !rejected_quote_error.empty()) {
                    throw JSONRPCError(rejected_quote_code,
                                       rejected_quote_error);
                }

                NodeId peer_id{-1};
                if (!quote_received && attempt.state == AttemptState::CANDIDATE) {
                    node->connman->ForEachNode([&](CNode* peer) {
                        if (peer_id == -1 && peer->IsPaymasterConn() && peer->addr == candidate.endpoint) {
                            peer_id = peer->GetId();
                        }
                    });
                    if (peer_id == -1) {
                        connection_pending = node->connman->AddConnection(
                            candidate.endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
                            /*paymaster_high_privacy=*/privacy == PrivacyProfile::HIGH);
                    } else {
                        const uint256 message_id = Hash(request_bytes);
                        queued = context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                                 context.paymaster->QueueOutboundDirectMessage(
                                     peer_id, message_id, request_bytes.size(), DirectPayload{quote_request}, now);
                        if (!queued) throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED,
                                                        "PAYMASTER_DIRECT_QUEUE_FULL");
                        node->connman->WakeMessageHandler();
                    }
                }
            }
            if (!store.GetSessionByRequestId(request_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
            }
            UniValue result = SessionToJSON(session);
            result.pushKV("provider_id", provider_id.GetHex());
            result.pushKV("offer_id", candidate.terms.offer_id.GetHex());
            result.pushKV("policy_hash", candidate.terms.policy_hash.GetHex());
            result.pushKV("funding_model",
                          intent.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
            result.pushKV("payment_cents", candidate.payment.value);
            result.pushKV("service_fee_cents", candidate.service_fee.value);
            result.pushKV("user_total_cents", candidate.user_total.value);
            result.pushKV("expires_at", intent.expires_at);
            result.pushKV("queued", queued);
            result.pushKV("connection_pending", connection_pending);
            result.pushKV("capacity_pending", false);
            result.pushKV("capacity_snapshot_id",
                          attempt.capacity_snapshot.snapshot_id.GetHex());
            if (!attempt.quote_id.IsNull()) {
                result.pushKV("quote_id", attempt.quote_id.GetHex());
                result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
                result.pushKV("template_commitment", attempt.template_commitment.GetHex());
                result.pushKV("authorization_commitment",
                              attempt.client_manifest.manifest_id.GetHex());
                const bool authorization_accepted =
                    attempt.accepted_client_manifest_id ==
                        attempt.client_manifest.manifest_id &&
                    attempt.client_manifest_accepted_at > 0;
                result.pushKV("authorization_accepted",
                              authorization_accepted);
                if (authorization_accepted) {
                    result.pushKV("authorization_accepted_at",
                                  attempt.client_manifest_accepted_at);
                }
                result.pushKV("psbt", EncodeBase64(attempt.unsigned_psbt));
            }
            return result;
        },
    };
}

UniValue RequestAutomaticPaymasterQuote(const JSONRPCRequest& request,
                                        const std::string& address,
                                        CAmount amount,
                                        const UniValue& options,
                                        const std::vector<COutPoint>* preset_inputs)
{
    using namespace DigiDollar::Paymaster;
    WalletContext& context = EnsureWalletContext(request.context);
    std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return UniValue::VNULL;
    std::string readiness_error;
    if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
        throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
    }

    const std::string request_id = options.find_value("request_id").get_str();
    const int64_t fee_cap = options.find_value("maximum_paymaster_fee_cents").getInt<int64_t>();
    const std::string fee_mode = options.find_value("fee_mode").get_str();
    const FeeMode requested_mode =
        fee_mode == "auto" ? FeeMode::AUTO : FeeMode::PAYMASTER;
    const bool subtract_fee =
        !options.find_value("subtract_paymaster_fee_from_amount").isNull() &&
        options.find_value("subtract_paymaster_fee_from_amount").get_bool();
    const bool send_all =
        !options.find_value("send_all_spendable_dd").isNull() &&
        options.find_value("send_all_spendable_dd").get_bool();
    if (send_all && !subtract_fee) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "send_all_spendable_dd requires subtract_paymaster_fee_from_amount");
    }
    const bool prepare_only = !options.find_value("prepare_only").isNull() &&
                              options.find_value("prepare_only").get_bool();
    std::optional<uint256> supplied_authorization_commitment;
    if (!options.find_value("authorization_commitment").isNull()) {
        supplied_authorization_commitment = ParseHashV(
            options.find_value("authorization_commitment"),
            "authorization_commitment");
    }
    if (prepare_only && supplied_authorization_commitment) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "prepare_only and authorization_commitment are mutually exclusive");
    }
    const std::string privacy_name = options.find_value("privacy").isNull() ? "standard" : options.find_value("privacy").get_str();
    const std::string selection_name = options.find_value("selection").isNull() ? "lowest_total_cost" : options.find_value("selection").get_str();
    const PrivacyProfile privacy = privacy_name == "high" ? PrivacyProfile::HIGH : PrivacyProfile::STANDARD;
    const SelectionMode selection = selection_name == "privacy_weighted" ? SelectionMode::PRIVACY_WEIGHTED : SelectionMode::LOWEST_TOTAL_COST;
    const int maximum_attempts = options.find_value("maximum_provider_attempts").isNull() ? (privacy == PrivacyProfile::HIGH ? 1 : 3) : options.find_value("maximum_provider_attempts").getInt<int>();
    if (maximum_attempts < 1 || maximum_attempts > 16 ||
        (privacy == PrivacyProfile::HIGH && maximum_attempts != 1)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy");
    }
    if (!CheckPaymasterPrivacyReadiness(privacy, context, readiness_error)) {
        throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
    }

    const bool have_restricted_key = !options.find_value("provider_identity_key").isNull();
    const bool have_restricted_descriptor =
        !options.find_value("restricted_service_descriptor").isNull();
    const bool have_restricted_capability =
        !options.find_value("sponsorship_capability").isNull();
    const bool restricted = have_restricted_key || have_restricted_descriptor ||
                            have_restricted_capability;
    if (restricted && !(have_restricted_key && have_restricted_descriptor &&
                        have_restricted_capability)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "provider_identity_key, restricted_service_descriptor, and sponsorship_capability "
                           "must be supplied together");
    }
    if (restricted && maximum_attempts != 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "Restricted sponsorship requires maximum_provider_attempts=1");
    }
    std::optional<XOnlyPubKey> restricted_identity_key;
    std::optional<RestrictedServiceDescriptor> restricted_descriptor;
    std::optional<SponsorshipCapability> restricted_capability;
    OfferCandidate restricted_candidate;
    std::string error;
    PaymasterStore store{*wallet};
    const int64_t now = GetTime();
    PaymentSession durable_session;
    if (store.GetSessionByRequestId(request_id, durable_session) &&
        !IsTerminal(durable_session.state)) {
        ProviderAttempt durable_attempt;
        PaymentIntent durable_intent;
        PaymasterQuote durable_quote;
        if (FindDurableClientAuthorizationAttempt(
                *wallet, store, durable_session, now, durable_attempt,
                durable_intent, durable_quote, error)) {
            const CDigiDollarAddress dd_address{address};
            const CScript recipient_script = GetScriptForDestination(
                dd_address.GetDigiDollarDestination());
            if (durable_intent.request_id != request_id ||
                durable_intent.session_id != durable_session.session_id ||
                durable_intent.canonical_request_hash !=
                    durable_session.canonical_request_hash ||
                durable_intent.requested_fee_mode !=
                    durable_session.fee_mode_requested ||
                durable_intent.requested_fee_mode != requested_mode ||
                durable_intent.privacy_profile != privacy ||
                durable_intent.selection_mode != selection ||
                durable_attempt.privacy_profile != privacy ||
                durable_intent.recipient_script != recipient_script ||
                (subtract_fee
                     ? (durable_intent.recipient_amount.value >
                            std::numeric_limits<int64_t>::max() -
                                durable_quote.service_fee.value ||
                        durable_intent.recipient_amount.value +
                                durable_quote.service_fee.value !=
                            amount)
                     : durable_intent.recipient_amount != DDCents{amount}) ||
                (durable_session.requested_amount.value == 0
                     ? (subtract_fee || send_all)
                     : (durable_session.requested_amount != DDCents{amount} ||
                        durable_session.subtract_paymaster_fee_from_amount !=
                            subtract_fee ||
                        durable_session.send_all_spendable_dd != send_all)) ||
                durable_quote.service_fee.value > fee_cap ||
                (preset_inputs &&
                 *preset_inputs != durable_session.user_inputs)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, durable_session, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue resume = DurableClientAuthorizationToJSON(
                durable_session, durable_attempt, durable_intent,
                durable_quote);
            resume.pushKV("authorization_required",
                          !supplied_authorization_commitment.has_value());
            if (!supplied_authorization_commitment) return resume;
            if (*supplied_authorization_commitment !=
                durable_attempt.client_manifest.manifest_id) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
            }

            static constexpr const char* RESUME_FIELDS[]{
                "requested_fee_mode", "fee_mode_used", "provider_id",
                "offer_id", "policy_hash", "funding_model",
                "payment_cents", "service_fee_cents", "user_total_cents",
                "requested_amount_cents",
                "subtract_paymaster_fee_from_amount",
                "send_all_spendable_dd",
                "authorization_commitment", "authorization_accepted",
                "authorization_accepted_at"};
            const auto copy_resume_fields = [&resume](UniValue& target) {
                for (const char* field : RESUME_FIELDS) {
                    const UniValue& value = resume.find_value(field);
                    if (!value.isNull() && target.find_value(field).isNull()) {
                        target.pushKV(field, value);
                    }
                }
            };

            // A provider can commit and broadcast the exact transaction
            // between two client polls. Consume and validate that signed
            // result before attempting to queue the already-authorized USER
            // PSBT again. Otherwise the retry firewall correctly observes
            // the provider Capacity outpoint as spent, but reports a false
            // failure even though it was spent by the exact final transaction
            // that is waiting in the result inbox.
            JSONRPCRequest nested{request};
            nested.params = UniValue{UniValue::VARR};
            nested.params.push_back(request_id);
            UniValue provider_result =
                processpaymasterresult().HandleRequest(nested);
            if (provider_result.find_value("processed").isTrue()) {
                copy_resume_fields(provider_result);
                return provider_result;
            }

            // The exact final transaction may already be in stem relay or
            // the mempool even when there is no unread result envelope left
            // (for example, immediately after the first successful automatic
            // poll or after a restart).  Its Capacity inputs are necessarily
            // spent by that transaction.  Return the durable status instead
            // of re-running walletprocesspaymasterpsbt, which would otherwise
            // misclassify the known exact spend as a foreign Capacity spend.
            if (durable_session.state == SessionState::STEMPOOL ||
                durable_session.state == SessionState::MEMPOOL ||
                !durable_session.final_txid.IsNull()) {
                UniValue committed = SessionToJSON(durable_session, &store);
                copy_resume_fields(committed);
                return committed;
            }

            if (wallet->IsLocked() &&
                durable_attempt.state == AttemptState::QUOTED) {
                return resume;
            }

            nested.params = UniValue{UniValue::VARR};
            nested.params.push_back(EncodeBase64(durable_attempt.unsigned_psbt));
            nested.params.push_back(
                supplied_authorization_commitment->GetHex());
            UniValue authorization =
                walletprocesspaymasterpsbt().HandleRequest(nested);
            copy_resume_fields(authorization);
            nested.params = UniValue{UniValue::VARR};
            nested.params.push_back(request_id);
            provider_result =
                processpaymasterresult().HandleRequest(nested);
            if (provider_result.find_value("processed").isTrue()) {
                copy_resume_fields(provider_result);
                return provider_result;
            }
            return authorization;
        }
        if (!error.empty()) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, durable_session, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }
    const DDCents effective_fee_cap =
        EffectiveClientServiceFeeCap(*wallet, DDCents{fee_cap}, error);
    if (effective_fee_cap.value < 0) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }
    if (restricted) {
        const std::vector<unsigned char> key_bytes =
            ParseHexV(options.find_value("provider_identity_key"), "provider_identity_key");
        if (key_bytes.size() != 32) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "provider_identity_key must be exactly 32 bytes");
        }
        restricted_identity_key.emplace(MakeUCharSpan(key_bytes));
        RestrictedServiceDescriptor descriptor;
        SponsorshipCapability capability;
        const CDigiDollarAddress dd_address{address};
        const CScript recipient_script = GetScriptForDestination(dd_address.GetDigiDollarDestination());
        if (!restricted_identity_key->IsFullyValid() ||
            !DeserializePaymasterHex(options.find_value("restricted_service_descriptor"),
                                     "RESTRICTED_DESCRIPTOR", descriptor, error) ||
            !DeserializePaymasterHex(options.find_value("sponsorship_capability"),
                                     "SPONSORSHIP_CAPABILITY", capability, error) ||
            !ValidateRestrictedServiceDescriptor(
                descriptor, *restricted_identity_key, Params().GenesisBlock().GetHash(),
                GetTime(), error, Params().GetChainType() == ChainType::REGTEST) ||
            !ValidateSponsorshipCapability(
                capability, descriptor, recipient_script, DDCents{amount},
                capability.payment_request_nonce, Params().GenesisBlock().GetHash(),
                GetTime(), error)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               error.empty() ? "PAYMASTER_INVALID_RESTRICTED_CONTEXT" : error);
        }
        node::NodeContext* node = wallet->chain().context();
        if (!node || !node->chainman ||
            !ValidateRestrictedAdmissionProofs(descriptor, *restricted_identity_key,
                                               *node->chainman, error)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               error.empty() ? "PAYMASTER_RESTRICTED_ADMISSION_UNAVAILABLE" : error);
        }
        if (privacy == PrivacyProfile::HIGH && !descriptor.p2p_endpoint.IsTor()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "High privacy requires an onion restricted provider");
        }
        restricted_candidate.provider_id = descriptor.provider_id;
        restricted_candidate.identity_key = *restricted_identity_key;
        restricted_candidate.display_name = descriptor.sponsor_display_name;
        restricted_candidate.endpoint = descriptor.p2p_endpoint;
        restricted_candidate.announcement_sequence = descriptor.admission_sequence;
        restricted_candidate.announcement_expires_at = descriptor.expires_at;
        restricted_candidate.terms = OfferTerms{descriptor.offer_id, descriptor.policy_hash,
                                                FundingModel::SPONSORED,
                                                SponsorshipScope::RESTRICTED, 0,
                                                DDCents{amount}, DDCents{amount}};
        restricted_candidate.payment = DDCents{amount};
        restricted_candidate.service_fee = DDCents{0};
        restricted_candidate.user_total = DDCents{amount};
        restricted_descriptor = std::move(descriptor);
        restricted_capability = std::move(capability);
    }

    std::optional<std::pair<PaymasterId, uint256>> persisted_selection;
    std::optional<std::vector<COutPoint>> persisted_inputs;
    std::set<PaymasterId> attempted_providers;
    std::optional<uint256> active_attempt_id;
    PaymentSession persisted_session;
    const bool have_persisted_session =
        store.GetSessionByRequestId(request_id, persisted_session);
    if (have_persisted_session) {
        if (IsTerminal(persisted_session.state)) {
            if (persisted_session.requested_amount.value == 0
                    ? (subtract_fee || send_all)
                    : (persisted_session.requested_amount != DDCents{amount} ||
                       persisted_session.subtract_paymaster_fee_from_amount !=
                           subtract_fee ||
                       persisted_session.send_all_spendable_dd != send_all)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_REQUEST_ID_CONFLICT");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, persisted_session, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return SessionToJSON(persisted_session, &store);
        }
        persisted_inputs = persisted_session.user_inputs;
        if (!persisted_session.attempt_ids.empty()) {
            for (const uint256& attempt_id : persisted_session.attempt_ids) {
                ProviderAttempt persisted_attempt;
                if (!store.GetAttempt(attempt_id, persisted_attempt) ||
                    persisted_attempt.unsigned_intent.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                attempted_providers.insert(persisted_attempt.provider_id);
                if (attempt_id != persisted_session.attempt_ids.back() ||
                    persisted_attempt.state == AttemptState::REJECTED ||
                    persisted_attempt.state == AttemptState::QUOTE_EXPIRED) {
                    continue;
                }
                PaymentIntent persisted_intent;
                try {
                    SpanReader stream{::PROTOCOL_VERSION, persisted_attempt.unsigned_intent};
                    stream >> persisted_intent;
                    if (!stream.empty()) throw std::ios_base::failure("trailing intent data");
                    if (persisted_intent.user_dd_inputs != persisted_session.user_inputs) {
                        throw std::ios_base::failure("input binding mismatch");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                if (persisted_intent.expires_at <= now) {
                    std::string abandon_error;
                    if (!store.AbandonClientAttemptForFallback(
                            request_id, attempt_id, now, abandon_error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, abandon_error);
                    }
                } else {
                    persisted_selection = std::pair{persisted_attempt.provider_id,
                                                    persisted_intent.offer_id};
                    active_attempt_id = attempt_id;
                }
            }
        }
    }
    std::vector<PaymasterReliabilityRecord> records;
    if (!store.ListProviderReliability(records)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
    }
    std::map<PaymasterId, PaymasterReliabilityRecord> reputation;
    for (auto& record : records)
        reputation.emplace(record.provider_id, std::move(record));
    std::vector<OfferCandidate> candidates;
    if (restricted) {
        if (!EnsureProviderNotEquivocationBlocked(
                store, restricted_candidate.provider_id, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        candidates.push_back(restricted_candidate);
    } else {
        candidates = subtract_fee
            ? BuildGrossOfferCandidates(
                  context.paymaster->GetDirectory().List(now),
                  DDCents{amount}, FUNDING_MODEL_ALL, effective_fee_cap,
                  reputation, now, error)
            : BuildOfferCandidates(
                  context.paymaster->GetDirectory().List(now),
                  DDCents{amount}, FUNDING_MODEL_ALL, effective_fee_cap,
                  reputation, now, error);
        if (!error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    }
    if (privacy == PrivacyProfile::HIGH) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [](const OfferCandidate& candidate) { return !candidate.endpoint.IsTor(); }),
                         candidates.end());
    }
    std::optional<OfferCandidate> selected;
    if (persisted_selection) {
        const auto candidate = std::find_if(candidates.begin(), candidates.end(),
                                            [&](const OfferCandidate& entry) {
                                                return entry.provider_id == persisted_selection->first &&
                                                       entry.terms.offer_id == persisted_selection->second;
                                            });
        if (candidate != candidates.end())
            selected = *candidate;
        else if (active_attempt_id) {
            if (!store.AbandonClientAttemptForFallback(
                    request_id, *active_attempt_id, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            persisted_selection.reset();
        }
    }
    if (!persisted_selection) {
        if (persisted_session.attempt_ids.size() >= static_cast<size_t>(maximum_attempts)) {
            if (!persisted_session.attempt_ids.empty()) {
                std::string abandon_error;
                if (!store.AbandonUnsignedClientSession(
                        request_id, now, abandon_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, abandon_error);
                }
            }
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_MAXIMUM_PROVIDER_ATTEMPTS_REACHED");
        }
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const OfferCandidate& candidate) {
                                            return attempted_providers.count(candidate.provider_id) != 0;
                                        }),
                         candidates.end());
        FastRandomContext rng;
        selected = SelectOfferCandidate(candidates, selection,
                                        effective_fee_cap, rng, error,
                                        subtract_fee);
    }
    if (!selected) {
        const std::string selection_error =
            subtract_fee && error == "PAYMASTER_NO_ELIGIBLE_OFFER"
                ? "PAYMASTER_NO_EXACT_GROSS_OFFER"
                : error;
        if (have_persisted_session) {
            std::string abandon_error;
            if (!store.AbandonUnsignedClientSession(
                    request_id, now, abandon_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, abandon_error);
            }
        }
        throw JSONRPCError(RPC_WALLET_ERROR, selection_error);
    }

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
    std::vector<COutPoint> selected_inputs;
    std::vector<CAmount> selected_amounts;
    CAmount selected_total{0};
    bool inputs_ok{false};
    if (persisted_inputs && !persisted_inputs->empty()) {
        // This request already owns hard reservations for these exact inputs.
        // Reusing the persisted binding is the only safe automatic resume;
        // ordinary coin selection must continue to hide every reservation.
        if (preset_inputs && *preset_inputs != *persisted_inputs) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_PERSISTED_INPUT_CONFLICT");
        }
        selected_inputs = *persisted_inputs;
        inputs_ok = !selected_inputs.empty();
    } else if (send_all) {
        if (dd_wallet->GetSpendableDDBalance() != amount) {
            error = "PAYMASTER_SWEEP_BALANCE_CHANGED";
        } else if (preset_inputs) {
            inputs_ok = dd_wallet->SelectDDCoins(
                selected->user_total.value, *preset_inputs, selected_inputs,
                selected_total, &selected_amounts, &error);
            if (inputs_ok && selected_total != amount) {
                inputs_ok = false;
                error = "PAYMASTER_SWEEP_BALANCE_CHANGED";
            }
        } else {
            inputs_ok = dd_wallet->SelectAllSpendableDDCoins(
                amount, selected_inputs, selected_total, &selected_amounts,
                error);
        }
    } else if (preset_inputs) {
        inputs_ok = dd_wallet->SelectDDCoins(selected->user_total.value, *preset_inputs,
                                             selected_inputs, selected_total,
                                             &selected_amounts, &error);
    } else {
        inputs_ok = dd_wallet->SelectDDCoins(selected->user_total.value, selected_inputs,
                                             selected_total, &selected_amounts);
    }
    if (!inputs_ok) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                           error.empty() ? "Insufficient confirmed DigiDollar inputs" : error);
    }

    UniValue inputs{UniValue::VARR};
    for (const COutPoint& outpoint : selected_inputs) {
        UniValue input{UniValue::VOBJ};
        input.pushKV("txid", outpoint.hash.GetHex());
        input.pushKV("vout", outpoint.n);
        inputs.push_back(std::move(input));
    }
    UniValue intent{UniValue::VOBJ};
    intent.pushKV("request_id", request_id);
    intent.pushKV("address", address);
    intent.pushKV("amount_cents", selected->payment.value);
    intent.pushKV("requested_amount_cents", amount);
    intent.pushKV("subtract_paymaster_fee_from_amount", subtract_fee);
    intent.pushKV("send_all_spendable_dd", send_all);
    intent.pushKV("maximum_paymaster_fee_cents", fee_cap);
    intent.pushKV("fee_mode", fee_mode);
    intent.pushKV("privacy", privacy_name);
    intent.pushKV("selection", selection_name);
    intent.pushKV("maximum_provider_attempts", maximum_attempts);
    intent.pushKV("offer_id", selected->terms.offer_id.GetHex());
    if (restricted) {
        intent.pushKV("provider_identity_key", options.find_value("provider_identity_key"));
        intent.pushKV("restricted_service_descriptor",
                      options.find_value("restricted_service_descriptor"));
        intent.pushKV("sponsorship_capability", options.find_value("sponsorship_capability"));
    }
    intent.pushKV("selected_inputs", std::move(inputs));

    JSONRPCRequest nested{request};
    nested.params = UniValue{UniValue::VARR};
    nested.params.push_back(selected->provider_id.GetHex());
    nested.params.push_back(std::move(intent));
    UniValue quote_result = requestpaymasterquote().HandleRequest(nested);
    if (quote_result.find_value("funding_model").isNull()) {
        quote_result.pushKV(
            "funding_model",
            selected->terms.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
    }

    const auto copy_quote_fields = [&quote_result](UniValue& target) {
        static constexpr const char* FIELDS[]{
            "requested_fee_mode", "fee_mode_used", "provider_id", "offer_id",
            "policy_hash", "funding_model",
            "payment_cents", "service_fee_cents", "user_total_cents", "expires_at",
            "requested_amount_cents",
            "subtract_paymaster_fee_from_amount", "send_all_spendable_dd",
            "quote_id", "unsigned_txid", "template_commitment",
            "authorization_required", "authorization_commitment"};
        for (const char* field : FIELDS) {
            const UniValue& value = quote_result.find_value(field);
            if (!value.isNull() && target.find_value(field).isNull()) target.pushKV(field, value);
        }
    };

    const UniValue& psbt = quote_result.find_value("psbt");
    if (psbt.isNull()) {
        if (supplied_authorization_commitment) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_AUTHORIZATION_NOT_READY");
        }
        return quote_result;
    }

    const UniValue& unsigned_txid_value = quote_result.find_value("unsigned_txid");
    if (!unsigned_txid_value.isStr()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "PAYMASTER_AUTHORIZATION_TEMPLATE_MISSING");
    }
    ProviderAttempt authorization_attempt;
    const uint256 unsigned_txid = ParseHashV(unsigned_txid_value, "unsigned_txid");
    if (!store.GetAttemptByUnsignedTxid(unsigned_txid, authorization_attempt)) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "PAYMASTER_AUTHORIZATION_ATTEMPT_MISSING");
    }
    uint256 commitment;
    if (!GetValidatedClientAuthorizationCommitment(
            authorization_attempt, GetTime(), commitment, error)) {
        throw JSONRPCError(
            RPC_WALLET_ERROR,
            error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_INVALID" : error);
    }
    if (supplied_authorization_commitment &&
        *supplied_authorization_commitment != commitment) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
    }
    quote_result.pushKV("authorization_required",
                        !supplied_authorization_commitment.has_value());
    if (quote_result.find_value("authorization_commitment").isNull()) {
        quote_result.pushKV("authorization_commitment", commitment.GetHex());
    }
    // A high-level call can never silently turn a freshly received quote into
    // a USER signature. The caller must round-trip the exact manifest hash.
    if (!supplied_authorization_commitment) return quote_result;

    if (authorization_attempt.state == AttemptState::QUOTED) {
        if (!store.AcceptClientAuthorization(
                request_id, authorization_attempt.attempt_id, commitment,
                GetTime(), error) ||
            !store.GetAttempt(authorization_attempt.attempt_id,
                              authorization_attempt)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
    }
    if (wallet->IsLocked()) {
        PaymentSession session;
        if (!store.GetSessionByRequestId(request_id, session)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
        }
        if (session.state == SessionState::AWAITING_USER_SIGNATURE &&
            !store.TransitionSession(request_id, SessionState::AWAITING_WALLET_UNLOCK,
                                     PendingPhase::NONE, {},
                                     std::max(GetTime(),
                                              authorization_attempt.updated_at),
                                     error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!store.GetSessionByRequestId(request_id, session)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
        }
        UniValue paused = SessionToJSON(session);
        copy_quote_fields(paused);
        const bool authorization_accepted =
            authorization_attempt.accepted_client_manifest_id == commitment &&
            authorization_attempt.client_manifest_accepted_at > 0;
        paused.pushKV("authorization_accepted", authorization_accepted);
        if (authorization_accepted) {
            paused.pushKV("authorization_accepted_at",
                          authorization_attempt.client_manifest_accepted_at);
        }
        return paused;
    }

    nested.params = UniValue{UniValue::VARR};
    nested.params.push_back(psbt);
    nested.params.push_back(supplied_authorization_commitment->GetHex());
    UniValue authorization = walletprocesspaymasterpsbt().HandleRequest(nested);
    copy_quote_fields(authorization);

    nested.params = UniValue{UniValue::VARR};
    nested.params.push_back(request_id);
    UniValue provider_result = processpaymasterresult().HandleRequest(nested);
    if (provider_result.find_value("processed").isTrue()) {
        copy_quote_fields(provider_result);
        return provider_result;
    }
    return authorization;
}

RPCHelpMan getpaymasterreputation()
{
    return RPCHelpMan{
        "getpaymasterreputation",
        "Return privacy-preserving local reliability aggregates. No payment details are stored.\n",
        {
            {"provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional provider identity; omit to list all local records"},
        },
        RPCResult{RPCResult::Type::ARR, "", "Local provider reliability records", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Reliability aggregate", {}}}},
        RPCExamples{HelpExampleCli("getpaymasterreputation", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            PaymasterStore store{*wallet};
            std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord> records;
            if (!request.params[0].isNull()) {
                DigiDollar::Paymaster::PaymasterReliabilityRecord record;
                const auto provider_id = ParseHashV(request.params[0], "provider_id");
                if (store.GetProviderReliability(provider_id, record)) records.push_back(std::move(record));
            } else if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.provider_id < rhs.provider_id;
            });
            const int64_t now = GetTime();
            UniValue result{UniValue::VARR};
            for (const auto& record : records)
                result.push_back(ReliabilityToJSON(record, now));
            return result;
        },
    };
}

RPCHelpMan clearpaymasterreputation()
{
    return RPCHelpMan{
        "clearpaymasterreputation",
        "Clear local aggregate reliability history for one provider or all providers.\n"
        "Minimal per-attempt outcome markers remain to prevent duplicate accounting.\n",
        {
            {"provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional provider identity; omit to clear all aggregates"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Clear result", {{RPCResult::Type::NUM, "cleared", "Number of aggregate records removed"}}},
        RPCExamples{HelpExampleCli("clearpaymasterreputation", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            PaymasterStore store{*wallet};
            std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord> records;
            if (!request.params[0].isNull()) {
                DigiDollar::Paymaster::PaymasterReliabilityRecord record;
                const auto provider_id = ParseHashV(request.params[0], "provider_id");
                if (store.GetProviderReliability(provider_id, record)) records.push_back(std::move(record));
            } else if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            uint64_t cleared{0};
            std::string error;
            for (const auto& record : records) {
                if (!store.ClearProviderReliability(record.provider_id, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                ++cleared;
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("cleared", cleared);
            return result;
        },
    };
}

RPCHelpMan getpaymasterpoolinfo()
{
    return RPCHelpMan{
        "getpaymasterpoolinfo",
        "Return the persisted admission and operational Paymaster pool without modifying it.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider pool information", {
            PoolPreparationResult(),
                                                                             {RPCResult::Type::BOOL, "prepared", "Whether a valid pool record exists"},
                                                                             {RPCResult::Type::BOOL, "ready", "Whether admission proof and an operational slot are ready"},
                                                                             {RPCResult::Type::NUM, "entries", "Number of pool entries"},
                                                                             {RPCResult::Type::NUM, "complete_operational_slots", "Complete usable operational slots"},
                                                                             {RPCResult::Type::ARR, "readiness_errors", "Pool readiness errors", {{RPCResult::Type::STR, "", "Stable pool error"}}},
                                                                             {RPCResult::Type::ARR, "pool", "Persisted entries", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Pool entry", {
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
        RPCExamples{HelpExampleCli("getpaymasterpoolinfo", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            std::vector<DigiDollar::Paymaster::ProviderPoolEntry> entries;
            const bool prepared = GetPaymasterProviderPoolEntries(*wallet, entries);
            std::string refresh_error;
            if (prepared && !RefreshPoolConfirmationHeights(*wallet, entries, refresh_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, refresh_error);
            }
            DigiDollar::Paymaster::ProviderPolicy policy;
            const bool have_policy = GetPaymasterProviderPolicy(*wallet, policy);
            int32_t tip_height;
            {
                LOCK(wallet->cs_wallet);
                tip_height = wallet->GetLastBlockHeight();
            }
            DigiDollar::Paymaster::ProviderPoolReadiness readiness;
            if (prepared && have_policy) {
                readiness = DigiDollar::Paymaster::EvaluateProviderPoolReadiness(entries, policy, tip_height, 1);
            } else if (!prepared) {
                readiness.errors.push_back("PAYMASTER_POOLS_NOT_PREPARED");
            } else {
                readiness.errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
            }
            UniValue pool{UniValue::VARR};
            for (const auto& entry : entries)
                pool.push_back(PoolEntryToJSON(entry));
            UniValue result{UniValue::VOBJ};
            result.pushKV("prepared", prepared);
            result.pushKV("preparation", PoolPreparationToJSON(*wallet));
            result.pushKV("ready", prepared && have_policy && readiness.ready);
            result.pushKV("entries", static_cast<uint64_t>(entries.size()));
            result.pushKV("complete_operational_slots", static_cast<uint64_t>(readiness.complete_operational_slots));
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            result.pushKV("pool", std::move(pool));
            return result;
        },
    };
}

RPCHelpMan listpaymasterreservations()
{
    return RPCHelpMan{
        "listpaymasterreservations",
        "List non-available provider pool entries and their durable reservation identifiers.\n",
        {},
        RPCResult{RPCResult::Type::ARR, "", "Provider pool reservations", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Reserved, committed, pending-successor, or spent entry", {
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
        RPCExamples{HelpExampleCli("listpaymasterreservations", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::vector<DigiDollar::Paymaster::ProviderPoolEntry> entries;
            if (!GetPaymasterProviderPoolEntries(*wallet, entries)) return UniValue{UniValue::VARR};
            UniValue result{UniValue::VARR};
            for (const auto& entry : entries) {
                if (entry.state != DigiDollar::Paymaster::PoolEntryState::AVAILABLE) {
                    result.push_back(PoolEntryToJSON(entry));
                }
            }
            return result;
        },
    };
}

RPCHelpMan cancelpaymasterquote()
{
    return RPCHelpMan{
        "cancelpaymasterquote",
        "Atomically reject an unsigned provider quote and release its operational pool slot.\n"
        "Cancellation is refused once a user signature may exist.\n",
        {
            {"attempt_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Persistent provider attempt identifier"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative canceled quote state", {
                                                                                      {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                      {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                      {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                      {RPCResult::Type::STR_HEX, "attempt_id", "Canceled provider attempt"},
                                                                                      {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Bound quote identifier"},
                                                                                      {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Current provider policy hash"},
                                                                                      {RPCResult::Type::STR, "fee_mode", "Effective fee mode"},
                                                                                      {RPCResult::Type::NUM_TIME, "expires_at", /*optional=*/true, "Quote expiration time"},
                                                                                      {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                      {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                  }},
        RPCExamples{HelpExampleCli("cancelpaymasterquote", "\"0123...\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const uint256 attempt_id = ParseHashV(request.params[0], "attempt_id");
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt existing;
            if (!store.GetAttempt(attempt_id, existing)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_ATTEMPT_NOT_FOUND");
            }
            DigiDollar::Paymaster::ProviderIdentityRecord identity;
            if (!GetPaymasterIdentity(*wallet, identity) || identity.provider_id != existing.provider_id) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_IDENTITY_MISMATCH");
            }
            DigiDollar::Paymaster::ProviderAttempt canceled;
            std::string error;
            if (!store.CancelProviderQuote(attempt_id, GetTime(), canceled, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(canceled.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
            }
            DigiDollar::Paymaster::ProviderSettings settings;
            const bool have_settings = GetPaymasterProviderSettings(*wallet, settings);
            UniValue result{UniValue::VOBJ};
            result.pushKV("request_id", session.request_id);
            result.pushKV("session_id", session.session_id.GetHex());
            result.pushKV("provider_id", canceled.provider_id.GetHex());
            result.pushKV("attempt_id", canceled.attempt_id.GetHex());
            if (!canceled.quote_id.IsNull()) result.pushKV("quote_id", canceled.quote_id.GetHex());
            if (have_settings && !settings.policy_hash.IsNull()) result.pushKV("policy_hash", settings.policy_hash.GetHex());
            result.pushKV("fee_mode", FeeModeName(session.fee_mode_used));
            if (canceled.quote_expires_at > 0) result.pushKV("expires_at", canceled.quote_expires_at);
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(session.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(canceled.state)});
            return result;
        },
    };
}

} // namespace wallet
