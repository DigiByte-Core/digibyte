// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Client session, PSBT, submit, result, and recovery RPCs. */

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

namespace {

struct ClientSessionView {
    std::optional<ProviderAttempt> attempt;
    std::optional<AlternativeRecoveryRecord> recovery;
    std::optional<PaymasterResult> provider_result;
    std::optional<std::string> recipient_address;
    std::string artifact{"none"};
    bool has_retriable_final{false};
    bool authorization_may_exist{false};
    bool requires_attention{false};
    std::vector<std::string> allowed_actions;
};

bool LoadClientIntentDisplayBinding(const PaymentSession& session,
                                    const ProviderAttempt& attempt,
                                    std::string& recipient_address,
                                    std::string& error)
{
    PaymentIntent intent;
    try {
        SpanReader stream{::PROTOCOL_VERSION, attempt.unsigned_intent};
        stream >> intent;
        if (!stream.empty() ||
            SerializePaymentIntent(intent) != attempt.unsigned_intent) {
            throw std::ios_base::failure(
                "non-canonical persisted payment intent");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PERSISTED_INTENT_CORRUPT";
        return false;
    }

    if (intent.version != PaymentIntent::CURRENT_VERSION ||
        intent.genesis_hash != Params().GenesisBlock().GetHash() ||
        attempt.provider_id.IsNull() ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) != attempt.provider_id ||
        intent.request_id != session.request_id ||
        intent.session_id != session.session_id ||
        intent.canonical_request_hash != session.canonical_request_hash ||
        intent.requested_fee_mode != session.fee_mode_requested ||
        intent.provider_id != attempt.provider_id ||
        intent.client_nonce != attempt.client_nonce ||
        intent.user_dd_inputs != session.user_inputs ||
        intent.privacy_profile != attempt.privacy_profile ||
        intent.sponsorship_authorization_hash !=
            attempt.sponsorship_capability_hash ||
        (intent.funding_model != FundingModel::USER_PAID &&
         intent.funding_model != FundingModel::SPONSORED) ||
        (intent.sponsorship_scope != SponsorshipScope::PUBLIC &&
         intent.sponsorship_scope != SponsorshipScope::RESTRICTED) ||
        (intent.requested_fee_mode != FeeMode::PAYMASTER &&
         intent.requested_fee_mode != FeeMode::AUTO) ||
        (intent.privacy_profile != PrivacyProfile::STANDARD &&
         intent.privacy_profile != PrivacyProfile::HIGH) ||
        (intent.selection_mode != SelectionMode::LOWEST_TOTAL_COST &&
         intent.selection_mode != SelectionMode::PRIVACY_WEIGHTED) ||
        intent.offer_id.IsNull() || intent.policy_hash.IsNull() ||
        intent.user_dd_inputs.empty() ||
        intent.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        !intent.user_input_proofs.empty() ||
        intent.recipient_amount.value < 100 ||
        intent.recipient_amount.value > MAX_DD_OUTPUT_CENTS ||
        attempt.created_at <= 0 || intent.expires_at <= attempt.created_at ||
        TimeDeltaExceeds(intent.expires_at, attempt.created_at,
                         MAX_DIRECT_MESSAGE_TTL_SECONDS)) {
        error = "PAYMASTER_PERSISTED_INTENT_BINDING_MISMATCH";
        return false;
    }
    std::set<COutPoint> unique_inputs;
    if (std::any_of(intent.user_dd_inputs.begin(), intent.user_dd_inputs.end(),
                    [&](const COutPoint& input) {
                        return input.IsNull() ||
                            !unique_inputs.insert(input).second;
                    })) {
        error = "PAYMASTER_PERSISTED_INTENT_BINDING_MISMATCH";
        return false;
    }
    std::string sponsorship_error;
    if (!ValidateSponsorshipBinding(
            intent.funding_model, intent.sponsorship_scope,
            /*fee_rate_bps=*/0, DDCents{0},
            intent.sponsorship_authorization_hash, sponsorship_error)) {
        error = sponsorship_error.empty()
            ? "PAYMASTER_PERSISTED_INTENT_BINDING_MISMATCH"
            : sponsorship_error;
        return false;
    }

    const ClientAuthorizationManifest& manifest = attempt.client_manifest;
    if (!manifest.manifest_id.IsNull() &&
        (manifest.version != ClientAuthorizationManifest::CURRENT_VERSION ||
         GetClientAuthorizationManifestId(manifest) != manifest.manifest_id ||
         manifest.request_id != intent.request_id ||
         manifest.session_id != intent.session_id ||
         manifest.provider_id != intent.provider_id ||
         manifest.offer_id != intent.offer_id ||
         manifest.policy_hash != intent.policy_hash ||
         manifest.user_dd_inputs != intent.user_dd_inputs ||
         manifest.recipient_script != intent.recipient_script ||
         manifest.recipient_amount != intent.recipient_amount ||
         manifest.canonical_request_hash != intent.canonical_request_hash ||
         manifest.requested_fee_mode != intent.requested_fee_mode ||
         manifest.privacy_profile != intent.privacy_profile)) {
        error = "PAYMASTER_PERSISTED_MANIFEST_BINDING_MISMATCH";
        return false;
    }

    const auto address = DigiDollarAddressForScript(intent.recipient_script);
    if (!address ||
        (!intent.user_dd_change_script.empty() &&
         !DigiDollarAddressForScript(intent.user_dd_change_script))) {
        error = "PAYMASTER_PERSISTED_RECIPIENT_INVALID";
        return false;
    }
    recipient_address = *address;
    return true;
}

bool IsUnsignedAttempt(const ProviderAttempt& attempt)
{
    return (attempt.state == AttemptState::CANDIDATE ||
            attempt.state == AttemptState::QUOTED ||
            attempt.state == AttemptState::REJECTED ||
            attempt.state == AttemptState::QUOTE_EXPIRED) &&
           attempt.user_signed_psbt.empty() &&
           attempt.final_transaction.empty() && attempt.final_txid.IsNull() &&
           attempt.provider_signed_at == 0 &&
           attempt.provider_signed_result.empty();
}

bool LoadClientSessionView(PaymasterStore& store,
                           const PaymentSession& session,
                           int64_t now,
                           ClientSessionView& view,
                           std::string& error)
{
    view = {};
    error.clear();
    if (session.provider_side) {
        error = "PAYMASTER_CLIENT_SESSION_REQUIRED";
        return false;
    }

    std::vector<ProviderAttempt> attempts;
    attempts.reserve(session.attempt_ids.size());
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!store.GetAttempt(attempt_id, attempt) ||
            attempt.attempt_id != attempt_id ||
            attempt.session_id != session.session_id) {
            error = "PAYMASTER_SESSION_ATTEMPT_DATABASE_READ";
            return false;
        }
        if (!IsUnsignedAttempt(attempt)) view.authorization_may_exist = true;
        attempts.push_back(std::move(attempt));
    }
    if (!attempts.empty()) view.attempt = attempts.back();

    for (const ProviderAttempt& attempt : attempts) {
        std::string recipient_address;
        if (!LoadClientIntentDisplayBinding(
                session, attempt, recipient_address, error)) {
            return false;
        }
        if (view.recipient_address &&
            *view.recipient_address != recipient_address) {
            error = "PAYMASTER_SESSION_RECIPIENT_CONFLICT";
            return false;
        }
        view.recipient_address = std::move(recipient_address);
    }

    if (view.attempt && !view.attempt->commit_key.IsNull()) {
        PaymasterResult provider_result;
        if (store.GetProviderResult(view.attempt->commit_key,
                                    provider_result)) {
            if (!ValidatePaymasterResult(
                    provider_result, Params().GenesisBlock().GetHash(),
                    view.attempt->provider_id,
                    view.attempt->commit_key,
                    view.attempt->provider_identity_key, 1, error)) {
                if (error.empty()) {
                    error = "PAYMASTER_PERSISTED_RESULT_CORRUPT";
                }
                return false;
            }
            view.provider_result = std::move(provider_result);
        }
    }

    bool has_live_reservations{false};
    if (!store.ClientSessionHasLiveReservations(
            session, has_live_reservations, error)) {
        return false;
    }
    if (has_live_reservations) view.authorization_may_exist = true;

    AlternativeRecoveryRecord recovery;
    if (store.GetAlternativeRecovery(session.request_id, recovery)) {
        if (recovery.session_id != session.session_id) {
            error = "PAYMASTER_RECOVERY_SESSION_CONFLICT";
            return false;
        }
        view.recovery = std::move(recovery);
        view.artifact = "alternative_recovery";
        view.authorization_may_exist = true;
    } else if (view.attempt &&
               (!view.attempt->final_transaction.empty() ||
                !view.attempt->final_txid.IsNull())) {
        // The client-side attempt is the durable authority for a received
        // provider result. ProviderCommitRecord belongs to the provider
        // wallet and is therefore not required for this classification.
        if (view.attempt->final_transaction.empty() ||
            view.attempt->final_txid.IsNull()) {
            error = "PAYMASTER_PERSISTED_FINAL_ARTIFACT_CORRUPT";
            return false;
        }
        try {
            CMutableTransaction decoded;
            SpanReader stream{::PROTOCOL_VERSION,
                              view.attempt->final_transaction};
            stream >> decoded;
            if (!stream.empty() ||
                SerializeTransaction(decoded) !=
                    view.attempt->final_transaction ||
                CTransaction{decoded}.GetHash() !=
                    view.attempt->final_txid) {
                error = "PAYMASTER_PERSISTED_FINAL_ARTIFACT_CORRUPT";
                return false;
            }
            const bool final_result_status =
                view.provider_result &&
                (view.provider_result->status ==
                     PaymasterResultStatus::FINAL_COMMITTED ||
                 view.provider_result->status ==
                     PaymasterResultStatus::BROADCAST_ATTEMPTED);
            view.has_retriable_final =
                final_result_status && view.provider_result->txid &&
                *view.provider_result->txid ==
                    view.attempt->final_txid &&
                view.provider_result->raw_transaction_hash &&
                *view.provider_result->raw_transaction_hash ==
                    CTransaction{decoded}.GetWitnessHash() &&
                view.provider_result->final_transaction &&
                SerializeTransaction(
                    *view.provider_result->final_transaction) ==
                    view.attempt->final_transaction;
        } catch (const std::ios_base::failure&) {
            error = "PAYMASTER_PERSISTED_FINAL_ARTIFACT_CORRUPT";
            return false;
        }
        view.artifact = "final_transaction";
        view.authorization_may_exist = true;
    } else if (view.attempt && !view.attempt->user_signed_psbt.empty()) {
        view.artifact = "user_psbt";
        view.authorization_may_exist = true;
    }

    view.allowed_actions.push_back("refresh");
    if (!IsTerminal(session.state) && !view.recovery) {
        const bool early_session =
            session.state == SessionState::CREATED ||
            session.state == SessionState::INPUTS_RESERVED ||
            session.state == SessionState::AWAITING_WALLET_UNLOCK ||
            session.state == SessionState::AWAITING_USER_SIGNATURE ||
            session.state == SessionState::AUTHORIZED;
        if (view.artifact == "none" && early_session) {
            // Resume is deliberately a capability token for the idempotent
            // send RPC, not a new resolvepaymastersession mutation.
            view.allowed_actions.push_back("resume");
        }
        if (view.attempt && view.artifact == "none" &&
            (view.attempt->state == AttemptState::CANDIDATE ||
             view.attempt->state == AttemptState::QUOTED ||
             view.attempt->state == AttemptState::QUOTE_EXPIRED) &&
            (session.state == SessionState::INPUTS_RESERVED ||
             session.state == SessionState::AWAITING_WALLET_UNLOCK ||
             session.state == SessionState::AWAITING_USER_SIGNATURE ||
             session.state == SessionState::AUTHORIZED)) {
            view.allowed_actions.push_back("fallback");
        }
        if (view.artifact == "none" &&
            (session.state == SessionState::CREATED ||
             session.state == SessionState::INPUTS_RESERVED ||
             session.state == SessionState::AWAITING_WALLET_UNLOCK ||
             session.state == SessionState::AWAITING_USER_SIGNATURE) &&
            std::all_of(attempts.begin(), attempts.end(), IsUnsignedAttempt)) {
            view.allowed_actions.push_back("abandon_unsigned");
        }
        if (view.attempt && view.artifact == "user_psbt" &&
            (view.attempt->retry_until <= 0 || now <= view.attempt->retry_until)) {
            view.allowed_actions.push_back("retry_same");
        } else if (view.has_retriable_final) {
            view.allowed_actions.push_back("retry_same");
        }
    }

    const bool resumable_recovery =
        view.recovery && !view.recovery->expired &&
        !IsTerminal(session.state);
    const bool recoverable_original =
        !view.recovery && view.attempt &&
        session.state == SessionState::PENDING_PROVIDER &&
        !view.attempt->commit_key.IsNull() &&
        !view.attempt->template_commitment.IsNull() &&
        ((view.attempt->state >= AttemptState::USER_SIGNED &&
          view.attempt->state <= AttemptState::MEMPOOL) ||
         view.attempt->state == AttemptState::AMBIGUOUS);
    if (resumable_recovery || recoverable_original) {
        view.allowed_actions.push_back("cancel_to_self");
    }

    view.requires_attention =
        session.state == SessionState::CONFLICTED ||
        (session.state == SessionState::FAILED &&
         view.authorization_may_exist) ||
        (view.artifact == "final_transaction" &&
         !view.has_retriable_final) ||
        (view.recovery && !IsTerminal(session.state));
    return true;
}

UniValue AllowedActionsToJSON(const ClientSessionView& view)
{
    UniValue actions{UniValue::VARR};
    for (const std::string& action : view.allowed_actions) {
        actions.push_back(action);
    }
    return actions;
}

UniValue ClientSessionSnapshotToJSON(PaymasterStore& store,
                                     const PaymentSession& session,
                                     std::string_view action)
{
    ClientSessionView view;
    std::string error;
    if (!LoadClientSessionView(store, session, GetTime(), view, error)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("action", std::string{action});
    UniValue session_result = SessionToJSON(session, &store);
    if (view.recipient_address && !session_result.exists("to_address")) {
        session_result.pushKV("to_address", *view.recipient_address);
    }
    result.pushKV("session", std::move(session_result));
    if (view.attempt) {
        result.pushKV("attempt", AttemptToJSON(*view.attempt));
    } else {
        result.pushKV("attempt", UniValue{UniValue::VNULL});
    }
    result.pushKV("artifact", view.artifact);
    if (view.recovery) {
        result.pushKV("recovery", AlternativeRecoveryToJSON(*view.recovery));
    } else {
        result.pushKV("recovery", UniValue{UniValue::VNULL});
    }
    if (view.provider_result) {
        result.pushKV("result_status",
                      ResultStatusName(view.provider_result->status));
        result.pushKV("result_sequence",
                      view.provider_result->result_sequence);
    } else {
        result.pushKV("result_status", UniValue{UniValue::VNULL});
        result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    }
    result.pushKV("requires_attention", view.requires_attention);
    result.pushKV("allowed_actions", AllowedActionsToJSON(view));
    return result;
}

UniValue ClientSessionSummaryToJSON(const PaymentSession& session,
                                    const ClientSessionView& view)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", session.request_id);
    result.pushKV("session_id", session.session_id.GetHex());
    if (session.requested_amount.value > 0) {
        result.pushKV("requested_amount_cents", session.requested_amount.value);
    }
    result.pushKV("requested_fee_mode", FeeModeName(session.fee_mode_requested));
    result.pushKV("fee_mode_used", FeeModeName(session.fee_mode_used));
    result.pushKV("session_state", std::string{SessionStateName(session.state)});
    if (session.pending_phase != PendingPhase::NONE) {
        result.pushKV("pending_phase", std::string{PendingPhaseName(session.pending_phase)});
    }
    result.pushKV("created_at", session.created_at);
    result.pushKV("updated_at", session.updated_at);
    result.pushKV("provider_attempts",
                  static_cast<uint64_t>(session.attempt_ids.size()));
    if (!session.final_txid.IsNull()) {
        result.pushKV("txid", session.final_txid.GetHex());
    }
    if (!session.recovery_txid.IsNull()) {
        result.pushKV("recovery_txid", session.recovery_txid.GetHex());
    }
    result.pushKV("artifact", view.artifact);
    result.pushKV("requires_attention", view.requires_attention);
    result.pushKV("allowed_actions", AllowedActionsToJSON(view));
    return result;
}

bool IsActiveClientSession(const PaymentSession& session,
                           const ClientSessionView& view)
{
    if (session.state == SessionState::CONFIRMED ||
        session.state == SessionState::CANCELED_SAFE) {
        return false;
    }
    if (session.state == SessionState::FAILED &&
        !view.authorization_may_exist) {
        return false;
    }
    return true;
}

// Share the durable session schema between status and recovery RPCs.
RPCResult ClientSessionResult(std::string key)
{
    return RPCResult{RPCResult::Type::OBJ, std::move(key), "The authoritative persistent session", {
        {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
        {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
        {RPCResult::Type::STR_HEX, "canonical_request_hash", "Hash of the canonical authorized request"},
        {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount; exact total outflow in subtract mode"},
        {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the service fee is deducted from requested_amount_cents"},
        {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the session is bound to all ordinary spendable DD"},
        {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
        {RPCResult::Type::STR, "fee_mode_used", "Persisted effective fee mode"},
        {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider bound by the latest durable client authorization"},
        {RPCResult::Type::STR, "privacy_profile", /*optional=*/true, "standard or high privacy profile of the latest durable attempt"},
        {RPCResult::Type::STR_HEX, "offer_id", /*optional=*/true, "Offer bound by the latest durable client authorization"},
        {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Provider policy bound by the latest durable client authorization"},
        {RPCResult::Type::STR, "funding_model", /*optional=*/true, "Exact sponsored or user_paid funding model"},
        {RPCResult::Type::NUM, "payment_cents", /*optional=*/true, "Exact recipient amount from the latest durable client authorization"},
        {RPCResult::Type::NUM, "service_fee_cents", /*optional=*/true, "Exact rounded provider service fee"},
        {RPCResult::Type::NUM, "user_total_cents", /*optional=*/true, "Exact recipient amount plus service fee"},
        {RPCResult::Type::STR, "to_address", /*optional=*/true, "Canonical DigiDollar recipient from the bound persisted intent or durable authorization"},
        {RPCResult::Type::STR, "session_state", "Authoritative session state"},
        {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Persisted phase for PENDING_PROVIDER"},
        {RPCResult::Type::BOOL, "final", "Whether the state is terminal"},
        {RPCResult::Type::STR, "broadcast_state", "not_attempted, unknown, accepted_mempool, accepted_stempool, or confirmed"},
        {RPCResult::Type::STR, "confirmation_state", "unconfirmed, payment_confirmed, recovery_confirmed, or conflicted"},
        {RPCResult::Type::NUM_TIME, "created_at", "Session creation time"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last persisted transition time"},
        {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final transaction id"},
        {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known idempotent self-recovery transaction id"},
        {RPCResult::Type::ARR, "reserved_user_inputs", "Immutable user input set", {
            {RPCResult::Type::OBJ, "", "A reserved user input", {
                {RPCResult::Type::STR_HEX, "txid", "Creating transaction id"},
                {RPCResult::Type::NUM, "vout", "Output index"},
            }},
        }},
        {RPCResult::Type::NUM, "provider_attempts", "Number of persistent provider attempts"},
    }};
}

} // namespace

RPCHelpMan getdigidollarsendsession()
{
    return RPCHelpMan{
        "getdigidollarsendsession",
        "Return one persistent DigiDollar send session without creating a new payment attempt.\n",
        {
            {"lookup", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exactly one persistent session identifier", {
                                                                                                                 {"request_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lowercase UUID"},
                                                                                                                 {"session_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Persistent 256-bit session identifier"},
                                                                                                             }},
        },
        ClientSessionResult(""),
        RPCExamples{HelpExampleCli("getdigidollarsendsession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::PaymentSession session;
            if (!FindSession(request.params[0].get_obj(), store, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            return SessionToJSON(session, &store);
        },
    };
}

RPCHelpMan listdigidollarsendsessions()
{
    return RPCHelpMan{
        "listdigidollarsendsessions",
        "List bounded wallet-local Paymaster client session summaries without "
        "creating, signing, retrying, broadcasting, or recovering a payment.\n"
        "The cursor is the exclusive request_id of the previous page. "
        "active_only retains conflicted and otherwise terminal sessions whenever "
        "durable authorization evidence may still require attention.\n",
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Read-only pagination options", {
                                                                                                           {"active_only", RPCArg::Type::BOOL, RPCArg::Default{true}, "Exclude completed and harmless unsigned-failed sessions"},
                                                                                                           {"limit", RPCArg::Type::NUM, RPCArg::Default{20}, "Maximum summaries to return, from 1 through 100"},
                                                                                                           {"cursor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Exclusive canonical request_id cursor"},
                                                                                                       }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "A bounded read-only session page", {
                                                                                       {RPCResult::Type::BOOL, "active_only", "Applied active-session filter"},
                                                                                       {RPCResult::Type::NUM, "count", "Number of summaries in this page"},
                                                                                       {RPCResult::Type::ARR, "sessions", "Wallet-local client session summaries", {
                                                                                                                                                                         {RPCResult::Type::OBJ, "", "One persistent session summary", {
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "fee_mode_used", "Effective fee mode"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Pending provider phase"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM_TIME, "created_at", "Session creation time"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM_TIME, "updated_at", "Last persisted transition time"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM, "provider_attempts", "Persistent provider attempt count"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known payment transaction"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known recovery transaction"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "artifact", "none, user_psbt, final_transaction, or alternative_recovery"},
                                                                                                                                                                                                                                        {RPCResult::Type::BOOL, "requires_attention", "Whether durable state requires explicit user attention"},
                                                                                                                                                                                                                                        {RPCResult::Type::ARR, "allowed_actions", "Core-derived actions; Qt may only remove entries", {{RPCResult::Type::STR, "", "Action name"}}},
                                                                                                                                                                                                                                    }},
                                                                                                                                                                     }},
                                                                                       {RPCResult::Type::STR, "next_cursor", /*optional=*/true, "Exclusive cursor for the next page"},
                                                                                   }},
        RPCExamples{HelpExampleCli("listdigidollarsendsessions", "'{\"active_only\":true,\"limit\":20}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            const UniValue options = request.params[0].isNull()
                ? UniValue{UniValue::VOBJ} : request.params[0].get_obj();
            RPCTypeCheckObj(options,
                            {{"active_only", UniValueType(UniValue::VBOOL)},
                             {"limit", UniValueType(UniValue::VNUM)},
                             {"cursor", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const bool active_only = options.find_value("active_only").isNull()
                ? true : options.find_value("active_only").get_bool();
            const int64_t limit = options.find_value("limit").isNull()
                ? 20 : options.find_value("limit").getInt<int64_t>();
            const std::string cursor = options.find_value("cursor").isNull()
                ? std::string{} : options.find_value("cursor").get_str();
            if (limit < 1 || limit > 100) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "limit must be between 1 and 100");
            }
            if (!cursor.empty() && !IsCanonicalRequestId(cursor)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "cursor must be a canonical lowercase request UUID");
            }

            // Keep the live-session list and each derived attempt/recovery
            // view on one wallet database snapshot. PaymasterStore uses the
            // same recursive wallet lock internally.
            LOCK(wallet->cs_wallet);
            PaymasterStore store{*wallet};
            std::vector<PaymentSession> sessions;
            std::string error;
            if (!store.ListClientSessions(sessions, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            std::sort(sessions.begin(), sessions.end(),
                      [](const PaymentSession& lhs,
                         const PaymentSession& rhs) {
                          return lhs.request_id < rhs.request_id;
                      });

            struct ListedSession {
                PaymentSession session;
                ClientSessionView view;
            };
            std::vector<ListedSession> listed;
            listed.reserve(static_cast<size_t>(limit) + 1);
            for (PaymentSession& session : sessions) {
                if (!cursor.empty() && session.request_id <= cursor) {
                    continue;
                }
                if (active_only &&
                    (session.state == SessionState::CONFIRMED ||
                     session.state == SessionState::CANCELED_SAFE)) {
                    continue;
                }
                ClientSessionView view;
                if (!LoadClientSessionView(
                        store, session, GetTime(), view, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (active_only && !IsActiveClientSession(session, view)) {
                    continue;
                }
                listed.push_back({std::move(session), std::move(view)});
                if (listed.size() > static_cast<size_t>(limit)) break;
            }

            const size_t page_size = std::min(
                listed.size(), static_cast<size_t>(limit));
            UniValue page{UniValue::VARR};
            for (size_t index = 0; index < page_size; ++index) {
                page.push_back(ClientSessionSummaryToJSON(
                    listed[index].session, listed[index].view));
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("active_only", active_only);
            result.pushKV("count", static_cast<uint64_t>(page_size));
            result.pushKV("sessions", std::move(page));
            if (page_size < listed.size() && page_size > 0) {
                result.pushKV("next_cursor",
                              listed[page_size - 1].session.request_id);
            }
            return result;
        },
    };
}

UniValue ResolveAlternativePaymasterRecovery(
    WalletContext& context,
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& original_attempt,
    const UniValue& options)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    const int64_t now{GetTime()};
    if (!CheckPaymasterClientReadiness(wallet, context, error) ||
        !CheckPaymasterPrivacyReadiness(
            original_attempt.privacy_profile, context, error)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman || !node->chainman || !context.paymaster) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
    }

    const UniValue& provider_value =
        options.find_value("recovery_provider_id");
    const UniValue& offer_value = options.find_value("recovery_offer_id");
    const std::optional<PaymasterId> requested_provider =
        provider_value.isNull() ? std::nullopt : std::optional<PaymasterId>{ParseHashO(options, "recovery_provider_id")};
    const std::optional<uint256> requested_offer =
        offer_value.isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "recovery_offer_id")};
    const bool prepare_only =
        !options.find_value("prepare_only").isNull() &&
        options.find_value("prepare_only").get_bool();
    const UniValue& commitment_value =
        options.find_value("recovery_authorization_commitment");
    const std::optional<uint256> accepted_commitment =
        commitment_value.isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "recovery_authorization_commitment")};

    AlternativeRecoveryRecord recovery;
    if (!store.GetAlternativeRecovery(session.request_id, recovery)) {
        if (session.state != SessionState::PENDING_PROVIDER ||
            original_attempt.provider_id.IsNull() ||
            original_attempt.commit_key.IsNull() ||
            original_attempt.template_commitment.IsNull() ||
            !((original_attempt.state >= AttemptState::USER_SIGNED &&
               original_attempt.state <= AttemptState::MEMPOOL) ||
              original_attempt.state == AttemptState::AMBIGUOUS)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_ALLOWED");
        }
        DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
        if (!dd_wallet) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "DigiDollar wallet not initialized");
        }
        CAmount total_dd{0};
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        const UniValue& maximum_value =
            options.find_value("maximum_recovery_service_fee_cents");
        const DDCents requested_maximum{
            maximum_value.isNull() ? MAX_DD_OUTPUT_CENTS : maximum_value.getInt<int64_t>()};
        const DDCents effective_maximum =
            EffectiveClientServiceFeeCap(wallet, requested_maximum, error);
        if (effective_maximum.value < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, error);
        }

        std::vector<PaymasterReliabilityRecord> reliability_records;
        if (!store.ListProviderReliability(reliability_records)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_REPUTATION_DATABASE_READ");
        }
        std::map<PaymasterId, PaymasterReliabilityRecord> reliability;
        for (auto& record : reliability_records) {
            reliability.emplace(record.provider_id, std::move(record));
        }
        const auto candidates = BuildOfferCandidates(
            context.paymaster->GetDirectory().List(now), DDCents{total_dd},
            FUNDING_MODEL_USER_PAID, effective_maximum, reliability, now,
            error);
        if (!error.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, error);
        }
        const auto selected = std::find_if(
            candidates.begin(), candidates.end(),
            [&](const OfferCandidate& candidate) {
                return candidate.provider_id !=
                           original_attempt.provider_id &&
                       candidate.terms.funding_model ==
                           FundingModel::USER_PAID &&
                       (!requested_provider ||
                        candidate.provider_id == *requested_provider) &&
                       (!requested_offer ||
                        candidate.terms.offer_id == *requested_offer) &&
                       (original_attempt.privacy_profile !=
                            PrivacyProfile::HIGH ||
                        candidate.endpoint.IsTor()) &&
                       total_dd - candidate.service_fee.value >= 100;
            });
        if (selected == candidates.end()) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_NO_ELIGIBLE_RECOVERY_PROVIDER");
        }

        do {
            recovery.client_nonce = GetRandHash();
        } while (recovery.client_nonce.IsNull());
        recovery.provider_side = false;
        recovery.request_id = session.request_id;
        recovery.session_id = session.session_id;
        recovery.original_provider_id = original_attempt.provider_id;
        recovery.recovery_provider_id = selected->provider_id;
        recovery.privacy_profile = original_attempt.privacy_profile;
        recovery.offer_id = selected->terms.offer_id;
        recovery.policy_hash = selected->terms.policy_hash;
        recovery.recovery_provider_identity_key = selected->identity_key;
        recovery.recovery_provider_endpoint =
            selected->endpoint.ToStringAddrPort();
        recovery.original_commit_key = original_attempt.commit_key;
        recovery.original_template_commitment =
            original_attempt.template_commitment;
        recovery.selected_maximum_service_fee = effective_maximum;
        recovery.selected_service_fee = selected->service_fee;
        recovery.recovery_id = GetAlternativeRecoveryId(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, recovery.client_nonce);
        recovery.capacity_request.genesis_hash =
            Params().GenesisBlock().GetHash();
        recovery.capacity_request.provider_id =
            recovery.recovery_provider_id;
        recovery.capacity_request.request_id = recovery.request_id;
        recovery.capacity_request.session_id = recovery.session_id;
        recovery.capacity_request.client_nonce = recovery.client_nonce;
        recovery.capacity_request.funding_model = FundingModel::USER_PAID;
        recovery.capacity_request.requires_carrier =
            recovery.selected_service_fee.value > 0 &&
            recovery.selected_service_fee.value < 100;
        recovery.capacity_request.requested_slots = 1;
        recovery.capacity_request.created_at = now;
        recovery.capacity_request.expires_at = std::min(
            selected->announcement_expires_at,
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        recovery.phase = AlternativeRecoveryPhase::CAPACITY_PENDING;
        recovery.created_at = recovery.updated_at = now;
        if (recovery.capacity_request.expires_at <= now ||
            !store.PrepareAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                error.empty() ? "PAYMASTER_CAPACITY_REQUEST_EXPIRED" : error);
        }
    } else {
        if (recovery.expired || recovery.provider_side ||
            recovery.original_provider_id != original_attempt.provider_id ||
            recovery.original_commit_key != original_attempt.commit_key ||
            recovery.original_template_commitment !=
                original_attempt.template_commitment ||
            (requested_provider &&
             recovery.recovery_provider_id != *requested_provider) ||
            (requested_offer && recovery.offer_id != *requested_offer)) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT");
        }
        const UniValue& maximum_value =
            options.find_value("maximum_recovery_service_fee_cents");
        const bool durable_authorization =
            !recovery.accepted_recovery_authorization_commitment.IsNull() &&
            recovery.accepted_recovery_authorization_commitment ==
                recovery.recovery_authorization.authorization_commitment &&
            recovery.recovery_authorization_accepted_at > 0;
        const bool inconsistent_authorization =
            recovery.accepted_recovery_authorization_commitment.IsNull() !=
                (recovery.recovery_authorization_accepted_at == 0) ||
            (!recovery.accepted_recovery_authorization_commitment.IsNull() &&
             recovery.accepted_recovery_authorization_commitment !=
                 recovery.recovery_authorization.authorization_commitment);
        if (inconsistent_authorization) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_RECOVERY_AUTHORIZATION_CONFLICT");
        }
        if (!maximum_value.isNull() && !durable_authorization) {
            const DDCents effective = EffectiveClientServiceFeeCap(
                wallet, DDCents{maximum_value.getInt<int64_t>()}, error);
            if (effective.value < 0 ||
                !(effective == recovery.selected_maximum_service_fee)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_RECOVERY_FEE_LIMIT_CHANGED");
            }
        }
    }

    const EquivocationBlockPolicy recovery_block_policy =
        static_cast<uint8_t>(recovery.phase) <
                static_cast<uint8_t>(AlternativeRecoveryPhase::USER_SIGNED) ?
            EquivocationBlockPolicy::ENFORCE :
            EquivocationBlockPolicy::OBSERVE_ONLY;
    if (!DrainAlternativeRecoveryCapacityEquivocations(
            *context.paymaster, store, recovery, error,
            recovery_block_policy)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }

    const auto make_result = [&](const AlternativeRecoveryRecord& record,
                                 const RecoveryQueueState& queue,
                                 std::optional<bool> broadcast = std::nullopt,
                                 const std::string& broadcast_error = {}) {
        PaymentSession current_session;
        if (!store.GetSessionByRequestId(record.request_id,
                                         current_session)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_SESSION_NOT_FOUND");
        }
        // Every resolve mutation returns the same authoritative envelope as a
        // refresh. Callers can therefore replace the complete snapshot and
        // never retain an artifact from an older recovery phase.
        UniValue result = ClientSessionSnapshotToJSON(
            store, current_session, "cancel_to_self");
        result.pushKV("queued", queue.queued);
        result.pushKV("connection_pending", queue.connection_pending);
        result.pushKV("route_available", queue.route_available);
        if (broadcast) result.pushKV("broadcast", *broadcast);
        if (!broadcast_error.empty()) {
            result.pushKV("broadcast_error", broadcast_error);
        }
        return result;
    };
    const auto fail_after_recovery_store =
        [&](const AlternativeRecoveryRecord& durable_recovery,
            const CTransaction& exact_final, int rpc_code,
            const std::string& failure) -> void {
        std::string persistence_error;
        if (!store.RecordClientAlternativeRecoveryConflict(
                durable_recovery.request_id, durable_recovery.recovery_id,
                exact_final.GetHash(), exact_final.GetWitnessHash(), now,
                persistence_error)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                persistence_error.empty() ? "PAYMASTER_RECOVERY_FINAL_CONFLICT_NOT_PERSISTED" : persistence_error);
        }
        throw JSONRPCError(
            rpc_code,
            failure.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : failure);
    };

    if (recovery.phase == AlternativeRecoveryPhase::CAPACITY_PENDING) {
        auto proof_lease = context.paymaster->LeaseCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
        const auto proofs = SelectCapacityProofMessages(
            proof_lease.Messages(), recovery.recovery_provider_id,
            recovery.capacity_request.request_id,
            recovery.capacity_request.session_id, recovery.client_nonce);
        if (proofs.empty()) {
            RecoveryQueueState queue;
            if (!QueueRecoveryMessage(
                    context, wallet, recovery, recovery.capacity_request, now,
                    queue, error)) {
                throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
            }
            return make_result(recovery, queue);
        }
        bool capacity_equivocation{false};
        if (!PersistPendingAlternativeRecoveryCapacityEquivocationPair(
                store, recovery.recovery_id, proofs, capacity_equivocation,
                error)) {
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
        std::optional<PaymasterCapacityProof> accepted_proof;
        std::string rejected_proof_error;
        for (const DirectMessage& direct : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&direct.payload);
            if (!proof) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                rejected_proof_error = "PAYMASTER_INVALID_CAPACITY_PROOF";
                continue;
            }

            bool claim_equivocation{false};
            const std::vector<unsigned char> encoded_proof =
                SerializeCapacityProof(*proof);
            if (!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
                    recovery.recovery_id, encoded_proof, direct.received_at,
                    claim_equivocation, error)) {
                if (error != "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
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
            if (!store.GetAlternativeRecoveryById(recovery.recovery_id,
                                                  recovery)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND");
            }

            std::string proof_error;
            if (!ValidateCapacityProofAgainstChainstate(
                    *proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key, *node->chainman,
                    now, proof_error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                rejected_proof_error =
                    proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : std::move(proof_error);
                continue;
            }
            accepted_proof = *proof;
            break;
        }
        if (!accepted_proof) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                rejected_proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : rejected_proof_error);
        }
        const PaymasterCapacityProof& proof = *accepted_proof;
        const std::vector<unsigned char> first_capacity_proof =
            SerializeCapacityProof(proof);
        recovery.capacity_snapshot.snapshot_id = proof.snapshot_id;
        recovery.capacity_snapshot.resource_commitment =
            GetCapacityResourceCommitment(proof);
        recovery.capacity_snapshot.session_id = recovery.session_id;
        recovery.capacity_snapshot.attempt_id = recovery.recovery_id;
        recovery.capacity_snapshot.provider_id = proof.provider_id;
        recovery.capacity_snapshot.client_nonce = proof.client_nonce;
        recovery.capacity_snapshot.funding_model = proof.funding_model;
        recovery.capacity_snapshot.requires_carrier =
            proof.requires_carrier;
        recovery.capacity_snapshot.request_hash =
            Hash(SerializeCapacityRequest(recovery.capacity_request));
        recovery.capacity_snapshot.capacity_proof = first_capacity_proof;
        recovery.capacity_snapshot.created_at = proof.created_at;
        recovery.capacity_snapshot.expires_at = proof.expires_at;
        recovery.capacity_snapshot.validated_at = now;

        DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
        CAmount total_dd{0};
        if (!dd_wallet) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "DigiDollar wallet not initialized");
        }
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        CAmount remaining = total_dd - recovery.selected_service_fee.value;
        if (remaining < 100) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_RECOVERY_RETURN_TOO_SMALL");
        }
        std::vector<AlternativeRecoveryReturn> returns;
        while (remaining > 0) {
            CAmount chunk = std::min<CAmount>(remaining,
                                              MAX_DD_OUTPUT_CENTS);
            if (remaining > MAX_DD_OUTPUT_CENTS &&
                remaining - chunk < 100) {
                chunk = remaining - 100;
            }
            const auto destination = wallet.GetNewDestination(
                OutputType::BECH32M, "Paymaster alternative recovery");
            if (!destination || chunk < 100 ||
                chunk > MAX_DD_OUTPUT_CENTS) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RECOVERY_DESTINATION_UNAVAILABLE");
            }
            returns.push_back(
                {GetScriptForDestination(*destination), DDCents{chunk}});
            remaining -= chunk;
        }
        if (returns.size() > MAX_PAYMENT_INTENT_INPUTS) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_RECOVERY_TOO_MANY_RETURNS");
        }

        AlternativeRecoveryRequest recovery_request;
        recovery_request.genesis_hash =
            Params().GenesisBlock().GetHash();
        recovery_request.request_id = recovery.request_id;
        recovery_request.session_id = recovery.session_id;
        recovery_request.original_provider_id =
            recovery.original_provider_id;
        recovery_request.recovery_provider_id =
            recovery.recovery_provider_id;
        recovery_request.privacy_profile = recovery.privacy_profile;
        recovery_request.offer_id = recovery.offer_id;
        recovery_request.policy_hash = recovery.policy_hash;
        recovery_request.original_commit_key =
            recovery.original_commit_key;
        recovery_request.original_template_commitment =
            recovery.original_template_commitment;
        recovery_request.client_nonce = recovery.client_nonce;
        recovery_request.capacity_request = recovery.capacity_request;
        recovery_request.capacity_snapshot_id = proof.snapshot_id;
        recovery_request.capacity_resource_commitment =
            recovery.capacity_snapshot.resource_commitment;
        recovery_request.user_dd_inputs = session.user_inputs;
        recovery_request.wallet_returns = std::move(returns);
        recovery_request.maximum_service_fee =
            recovery.selected_maximum_service_fee;
        recovery_request.service_fee = recovery.selected_service_fee;
        recovery_request.created_at = now;
        recovery_request.expires_at = std::min(
            proof.expires_at,
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        // Recheck the capacity proof at the last possible point before the
        // first payment-revealing signatures are produced.
        if (wallet.IsLocked()) {
            throw JSONRPCError(
                RPC_WALLET_UNLOCK_NEEDED,
                "Wallet unlock is required for alternative recovery");
        }
        if (!DrainPendingAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, first_capacity_proof,
                error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        std::vector<XOnlyPubKey> output_keys;
        if (!ValidateCapacityProofAgainstChainstate(
                proof, recovery.capacity_request,
                recovery.recovery_provider_identity_key,
                *node->chainman, now, error) ||
            !SignAlternativeRecoveryRequestInputs(
                wallet, recovery_request, now, output_keys, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        recovery.recovery_request = std::move(recovery_request);
        recovery.recovery_request_hash =
            GetAlternativeRecoveryRequestHash(recovery.recovery_request);
        recovery.phase = AlternativeRecoveryPhase::REQUEST_READY;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::REQUEST_READY) {
        const auto responses = context.paymaster->TakeRecoveryResponses(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        if (responses.empty()) {
            PaymasterCapacityProof proof;
            if (!DeserializeCanonicalRecoveryMessage(
                    recovery.capacity_snapshot.capacity_proof, proof, error) ||
                !ValidateCapacityProofAgainstChainstate(
                    proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key,
                    *node->chainman, now, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            RecoveryQueueState queue;
            if (!QueueRecoveryMessage(
                    context, wallet, recovery, recovery.recovery_request, now,
                    queue, error)) {
                throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
            }
            return make_result(recovery, queue);
        }
        const auto* response =
            std::get_if<AlternativeRecoveryResponse>(
                &responses.front().payload);
        AlternativeRecoveryRecord candidate{recovery};
        if (!response ||
            !ValidateAlternativeRecoveryResponse(
                *response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, now, error)) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESPONSE" : error);
        }
        candidate.recovery_response = *response;
        candidate.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
        candidate.updated_at = std::max(candidate.updated_at, now);
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, candidate, parameters, error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                *response, candidate.recovery_request,
                candidate.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error) ||
            !BuildRecoveryAuthorizationManifest(
                candidate.recovery_request, *response,
                candidate.recovery_authorization, error) ||
            !store.UpdateAlternativeRecovery(candidate, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateRecoveryAuthorizationManifest(
                recovery.recovery_authorization,
                recovery.recovery_request, recovery.recovery_response, now,
                error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error)) {
            throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
        }
        if (prepare_only) {
            return make_result(recovery, RecoveryQueueState{});
        }
        if (!accepted_commitment) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_REQUIRED");
        }
        if (*accepted_commitment !=
            recovery.recovery_authorization.authorization_commitment) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_MISMATCH");
        }
        if (wallet.IsLocked()) {
            throw JSONRPCError(
                RPC_WALLET_UNLOCK_NEEDED,
                "Wallet unlock is required for alternative recovery");
        }
        // Persist the exact user authorization and reserve its client fee
        // before invoking any wallet signer. Reload and revalidate the
        // immutable recovery template after that transaction commits.
        if (!store.AcceptAlternativeRecoveryAuthorization(
                session.request_id, *accepted_commitment, now, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery) ||
            !BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateRecoveryAuthorizationManifest(
                recovery.recovery_authorization,
                recovery.recovery_request, recovery.recovery_response, now,
                error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                error.empty() ? "PAYMASTER_RECOVERY_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!SignCollaborativePSBTForParty(
                wallet, unsigned_psbt, trusted.trusted_template,
                SigningParty::USER, error) ||
            !ValidateAlternativeRecoveryPSBTAgainstChainstate(
                unsigned_psbt, trusted, parameters, Params(),
                *node->chainman, now,
                CollaborativeSignatureStage::USER_SIGNED, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        recovery.user_signed_psbt = SerializePSBT(unsigned_psbt);
        recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::USER_SIGNED) {
        const int64_t authorization_time =
            recovery.recovery_authorization_accepted_at;
        if (authorization_time <= 0) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_RECOVERY_AUTHORIZATION_REQUIRED");
        }
        const int64_t observation_time{
            std::max(now, authorization_time)};
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        AlternativeRecoverySubmit submit;
        submit.genesis_hash = Params().GenesisBlock().GetHash();
        submit.request_id = recovery.request_id;
        submit.session_id = recovery.session_id;
        submit.recovery_id = recovery.recovery_id;
        submit.recovery_provider_id = recovery.recovery_provider_id;
        submit.recovery_request_hash = recovery.recovery_request_hash;
        submit.recovery_commit_key =
            recovery.recovery_response.recovery_commit_key;
        submit.template_commitment =
            recovery.recovery_response.manifest.template_commitment;
        submit.user_psbt = recovery.user_signed_psbt;
        const auto messages = context.paymaster->TakeRecoveryResults(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        const AlternativeRecoveryResultMessage* message{nullptr};
        CTransactionRef observed_final;
        FinalTransactionPresence observed_presence{
            FinalTransactionPresence::NONE};
        if (!messages.empty()) {
            message = std::get_if<AlternativeRecoveryResultMessage>(
                &messages.front().payload);
            if (!message || !ValidateAlternativeRecoveryResult(*message, recovery.recovery_response, recovery.recovery_provider_identity_key, Params().GenesisBlock().GetHash(), 1, now, error) ||
                !message->result.final_transaction ||
                !message->result.raw_transaction_hash) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESULT" : error);
            }
            observed_final = MakeTransactionRef(
                *message->result.final_transaction);
            if (!PreflightFinalPaymasterTransaction(
                    wallet, observed_final, observed_presence, error)) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
            }
        }
        const AuthorizedCapacityResourceMode resource_mode{
            observed_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT};
        const CTransaction* const known_final{
            observed_presence != FinalTransactionPresence::NONE ? observed_final.get() : nullptr};
        PartiallySignedTransaction user_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                resource_mode, trusted, unsigned_psbt, error, known_final) ||
            !ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
                submit, recovery.recovery_response, trusted, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                resource_mode, user_psbt, error, known_final)) {
            throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
        }

        if (message) {
            const CMutableTransaction& final_transaction =
                *message->result.final_transaction;
            const CTransactionRef& final_ref = observed_final;
            const FinalTransactionPresence presence{observed_presence};
            if (!ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                    final_transaction, *message->result.raw_transaction_hash,
                    trusted, parameters, Params(), *node->chainman,
                    authorization_time, observation_time,
                    resource_mode, error)) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
            }
            recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
            recovery.final_transaction =
                SerializeTransaction(final_transaction);
            recovery.expected_wtxid =
                CTransaction{final_transaction}.GetWitnessHash();
            recovery.signed_result = message->result;
            recovery.updated_at = std::max(recovery.updated_at, now);
            SelfRecoveryRecord final_recovery;
            final_recovery.request_id = recovery.request_id;
            final_recovery.session_id = recovery.session_id;
            final_recovery.user_inputs =
                recovery.recovery_request.user_dd_inputs;
            final_recovery.recovery_txid =
                CTransaction{final_transaction}.GetHash();
            final_recovery.raw_transaction_hash =
                Hash(recovery.final_transaction);
            final_recovery.final_transaction = recovery.final_transaction;
            final_recovery.created_at = now;
            if (!store.CommitClientAlternativeRecoveryFinal(
                    recovery, final_recovery, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }

            // The final commit above performs database work and spends the
            // client fee reservation. Rebuild every authority from the exact
            // durable bytes and repeat the chainstate/mempool firewall before
            // handing the transaction to the node.
            AlternativeRecoveryRecord broadcast_recovery;
            AlternativeRecoveryParameters broadcast_parameters;
            AlternativeRecoveryTemplate broadcast_trusted;
            PartiallySignedTransaction broadcast_unsigned_psbt;
            FinalTransactionPresence broadcast_presence{
                FinalTransactionPresence::NONE};
            if (!store.GetAlternativeRecovery(
                    recovery.request_id, broadcast_recovery)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RECOVERY_FINAL_STATE_RELOAD_FAILED");
            }
            if (broadcast_recovery.phase !=
                    AlternativeRecoveryPhase::FINAL_COMMITTED ||
                broadcast_recovery.recovery_id != recovery.recovery_id ||
                broadcast_recovery.final_transaction !=
                    SerializeTransaction(final_transaction) ||
                broadcast_recovery.expected_wtxid !=
                    CTransaction{final_transaction}.GetWitnessHash()) {
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID");
            }
            if (!PreflightFinalPaymasterTransaction(
                    wallet, final_ref, broadcast_presence, error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED, error);
            }
            if (!BuildRecoveryParametersFromRecord(
                    wallet, broadcast_recovery, broadcast_parameters, error) ||
                !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                    broadcast_recovery.recovery_response,
                    broadcast_recovery.recovery_request,
                    broadcast_recovery.recovery_provider_identity_key,
                    broadcast_parameters, Params(), *node->chainman,
                    authorization_time, observation_time,
                    broadcast_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                    broadcast_trusted,
                    broadcast_unsigned_psbt, error,
                    broadcast_presence != FinalTransactionPresence::NONE ? final_ref.get() : nullptr)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    error.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            if (!ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                    final_transaction, broadcast_recovery.expected_wtxid,
                    broadcast_trusted, broadcast_parameters, Params(),
                    *node->chainman, authorization_time, observation_time,
                    broadcast_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                    error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    error.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            recovery = std::move(broadcast_recovery);
            bool already_confirmed{false};
            std::string broadcast_error;
            const bool broadcast = InsertAndBroadcastPaymasterTransaction(
                wallet, final_ref, already_confirmed, broadcast_error);
            if (!broadcast) {
                if (IsTransientPaymasterFinalizationError(broadcast_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
            }
            if (broadcast && !store.ReconcileFinalTransaction(
                                 *final_ref, already_confirmed ? 1 : 0,
                                 !already_confirmed, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return make_result(recovery, RecoveryQueueState{}, broadcast,
                               broadcast_error);
        }
        RecoveryQueueState queue;
        if (!QueueRecoveryMessage(context, wallet, recovery, submit, now,
                                  queue, error)) {
            throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
        }
        return make_result(recovery, queue);
    }

    if (recovery.phase == AlternativeRecoveryPhase::FINAL_COMMITTED) {
        CMutableTransaction final_transaction;
        try {
            SpanReader stream{::PROTOCOL_VERSION,
                              recovery.final_transaction};
            stream >> final_transaction;
            if (!stream.empty()) {
                throw std::ios_base::failure("trailing recovery transaction");
            }
        } catch (const std::ios_base::failure&) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_PERSISTED_RECOVERY_CORRUPT");
        }
        const CTransactionRef final_ref =
            MakeTransactionRef(final_transaction);
        FinalTransactionPresence presence{FinalTransactionPresence::NONE};
        if (!PreflightFinalPaymasterTransaction(
                wallet, final_ref, presence, error)) {
            if (IsTransientPaymasterFinalizationError(error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED, error);
        }
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        const int64_t authorization_time =
            recovery.recovery_authorization_accepted_at;
        const int64_t observation_time{
            std::max(now, authorization_time)};
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            authorization_time <= 0 ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                trusted,
                unsigned_psbt, error,
                presence != FinalTransactionPresence::NONE ? final_ref.get() : nullptr) ||
            !ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                final_transaction, recovery.expected_wtxid, trusted,
                parameters, Params(), *node->chainman,
                authorization_time, observation_time,
                presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                error)) {
            if (IsTransientPaymasterFinalizationError(error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED, error);
        }
        bool already_confirmed{false};
        std::string broadcast_error;
        const bool broadcast = InsertAndBroadcastPaymasterTransaction(
            wallet, final_ref, already_confirmed, broadcast_error);
        if (!broadcast) {
            if (IsTransientPaymasterFinalizationError(broadcast_error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED,
                broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
        }
        if (broadcast && !store.ReconcileFinalTransaction(
                             *final_ref, already_confirmed ? 1 : 0,
                             !already_confirmed, now, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        return make_result(recovery, RecoveryQueueState{}, broadcast,
                           broadcast_error);
    }
    throw JSONRPCError(RPC_WALLET_ERROR,
                       "PAYMASTER_ALTERNATIVE_RECOVERY_STATE_INVALID");
}

RPCHelpMan resolvepaymastersession()
{
    // RPCResult cannot express nullable fields. Use the existing per-field
    // escape hatch (as getwalletinfo does for scanning) only for the four
    // object/string/number-or-null fields; retain their documented schemas.
    // Functional tests check both absent and persisted session artifacts.
    return RPCHelpMan{
        "resolvepaymastersession",
        "Inspect or recover an existing Paymaster session.\n"
        "retry_same never creates a quote, attempt, reservation, or signature. "
        "fallback is allowed only before a user PSBT exists. abandon_unsigned "
        "atomically releases a session for which no transaction authorization can exist. "
        "cancel_to_self durably creates or resumes one exact same-input recovery transaction.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"lookup", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exactly one persistent session identifier", {
                                                                                                                 {"request_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lowercase UUID"},
                                                                                                                 {"session_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Persistent session identifier"},
                                                                                                             }},
            {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self; mutations must be present in the current Core-derived allowed_actions"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Exact recovery selection and authorization", {
                                                                                                                        {"attempt_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Existing original attempt identifier"},
                                                                                                                        {"recovery_provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact distinct recovery provider"},
                                                                                                                        {"recovery_offer_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact USER_PAID recovery offer"},
                                                                                                                        {"maximum_recovery_service_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Local recovery service-fee ceiling"},
                                                                                                                        {"prepare_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Stop after validating and persisting the exact authorization commitment"},
                                                                                                                        {"recovery_authorization_commitment", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Exact commitment required before signing user inputs"},
                                                                                                                    }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative local recovery state", {
                                                                                      {RPCResult::Type::STR, "action", "Performed local action"},
                                                                                      ClientSessionResult("session"),
                                                                                       {RPCResult::Type::OBJ, "attempt", /*optional=*/false, "Selected persistent attempt, or null when none exists", {
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "attempt_id", "Persistent attempt identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "provider_id", "Provider identifier"},
                                                                                                                                                                              {RPCResult::Type::STR, "provider_endpoint", /*optional=*/true, "Direct provider endpoint"},
                                                                                                                                                                              {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                                                                                                              {RPCResult::Type::STR, "privacy_profile", "standard or high privacy profile"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Provider quote identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Bound transaction template"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "unsigned_txid", /*optional=*/true, "Unsigned transaction identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "commit_key", /*optional=*/true, "Provider commit key"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "quote_expires_at", /*optional=*/true, "Quote expiration time"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "retry_until", /*optional=*/true, "Exact retry deadline"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact client authorization"},
                                                                                                                                                                              {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                          }, /*skip_type_check=*/true},
                                                                                       {RPCResult::Type::STR, "artifact", "none, user_psbt, final_transaction, or alternative_recovery"},
                                                                                       {RPCResult::Type::BOOL, "requires_attention", "Whether durable state requires explicit user attention"},
                                                                                       {RPCResult::Type::ARR, "allowed_actions", "Core-derived actions returned for every resolve action; Qt may only remove entries", {{RPCResult::Type::STR, "", "Action name"}}},
                                                                                       {RPCResult::Type::OBJ, "recovery", /*optional=*/false, "Persistent alternative-recovery authority and state, or null when none exists", {
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "recovery_id", "Stable recovery identifier"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "recovery_provider_id", "Distinct recovery provider"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "offer_id", "Exact USER_PAID offer"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "policy_hash", "Exact provider policy"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "original_commit_key", "Ambiguous original provider commit"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "original_template_commitment", "Ambiguous original transaction template"},
                                                                                                                                                                                                       {RPCResult::Type::STR, "privacy_profile", "Inherited standard or high privacy profile"},
                                                                                                                                                                                                       {RPCResult::Type::STR, "phase", "capacity_pending, request_ready, response_validated, user_signed, or final_committed"},
                                                                                                                                                                                                       {RPCResult::Type::BOOL, "expired", "Whether a safely expirable unsigned recovery expired"},
                                                                                                                                                                                                       {RPCResult::Type::ARR, "user_dd_inputs", "Canonical original user inputs", {{RPCResult::Type::OBJ, "", "User DD input", {
                                                                                                                                                                                                                                                                                                                                   {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                                                                                                                                                   {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                                                                                                               }}}},
                                                                                                                                                                                                       {RPCResult::Type::ARR, "wallet_returns", "Canonical wallet-owned recovery outputs", {{RPCResult::Type::OBJ, "", "Wallet return", {
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::STR_HEX, "script_pub_key", "Exact return script"},
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::STR, "address", /*optional=*/true, "Display address when decodable"},
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::NUM, "amount_cents", "Exact DD amount"},
                                                                                                                                                                                                                                                                                                                                        }}}},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Validated Capacity-v5 snapshot"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "capacity_resource_commitment", /*optional=*/true, "Exact capacity resources"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact recovery authorization"},
                                                                                                                                                                                                       {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                                                       {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "maximum_service_fee_cents", "Effective local fee ceiling"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "service_fee_cents", "Exact recovery service fee"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "Exact provider-paid network fee"},
                                                                                                                                                                                                       {RPCResult::Type::NUM_TIME, "expires_at", "Recovery authorization expiry"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact final recovery transaction"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Final recovery txid"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "wtxid", /*optional=*/true, "Final recovery wtxid"},
                                                                                                                                                                                                   }, /*skip_type_check=*/true},
                                                                                      {RPCResult::Type::STR, "psbt", /*optional=*/true, "Exact persisted user-signed PSBT"},
                                                                                      {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact durably committed transaction"},
                                                                                      {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Committed transaction id"},
                                                                                      {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Idempotent self-recovery transaction id"},
                                                                                      {RPCResult::Type::BOOL, "broadcast", /*optional=*/true, "Whether local broadcast succeeded"},
                                                                                      {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Local broadcast error; exact retry remains available"},
                                                                                      {RPCResult::Type::BOOL, "queued", /*optional=*/true, "Whether the exact persisted submit is queued"},
                                                                                      {RPCResult::Type::BOOL, "connection_pending", /*optional=*/true, "Whether a dedicated provider reconnection was requested"},
                                                                                      {RPCResult::Type::BOOL, "route_available", /*optional=*/true, "Whether the persisted provider route is available"},
                                                                                       {RPCResult::Type::STR, "result_status", /*optional=*/false, "Latest signed provider result, or null when none exists", {}, /*skip_type_check=*/true},
                                                                                       {RPCResult::Type::NUM, "result_sequence", /*optional=*/false, "Latest monotonic result sequence, or null when none exists", {}, /*skip_type_check=*/true},
                                                                                  }},
        RPCExamples{HelpExampleCli("resolvepaymastersession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}' retry_same")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const std::string action = request.params[1].get_str();
            if (action != "refresh" && action != "retry_same" && action != "fallback" &&
                action != "abandon_unsigned" && action != "cancel_to_self") {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "action must be refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self");
            }

            UniValue options{UniValue::VOBJ};
            if (!request.params[2].isNull()) options = request.params[2].get_obj();
            RPCTypeCheckObj(options,
                            {{"attempt_id", UniValueType(UniValue::VSTR)},
                             {"recovery_provider_id", UniValueType(UniValue::VSTR)},
                             {"recovery_offer_id", UniValueType(UniValue::VSTR)},
                             {"maximum_recovery_service_fee_cents", UniValueType(UniValue::VNUM)},
                             {"prepare_only", UniValueType(UniValue::VBOOL)},
                             {"recovery_authorization_commitment", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);

            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::PaymentSession session;
            if (!FindSession(request.params[0].get_obj(), store, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            if (context.paymaster && context.paymaster->Enabled()) {
                std::string equivocation_error;
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session,
                        equivocation_error,
                        EquivocationBlockPolicy::OBSERVE_ONLY)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       equivocation_error);
                }
            }
            {
                // Re-read under one recursive wallet lock after inbox
                // observation. The same authoritative action set exposed to
                // Qt is also the fail-closed gate for direct CLI mutations.
                LOCK(wallet->cs_wallet);
                if (!FindSession(request.params[0].get_obj(), store,
                                 session)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "Paymaster session not found");
                }
                if (action == "refresh") {
                    return ClientSessionSnapshotToJSON(store, session, action);
                }
                ClientSessionView view;
                std::string view_error;
                if (!LoadClientSessionView(
                        store, session, GetTime(), view, view_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, view_error);
                }
                if (std::find(view.allowed_actions.begin(),
                              view.allowed_actions.end(), action) ==
                    view.allowed_actions.end()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_SESSION_ACTION_NOT_ALLOWED");
                }
            }
            if (action == "abandon_unsigned") {
                std::string error;
                if (!store.AbandonUnsignedClientSession(
                        session.request_id, GetTime(), error) ||
                    !store.GetSessionByRequestId(session.request_id, session)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_UNSIGNED_ABANDON_FAILED" : error);
                }
                return ClientSessionSnapshotToJSON(store, session, action);
            }

            if (session.attempt_ids.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session has no provider attempt");
            }
            uint256 attempt_id = session.attempt_ids.back();
            if (!options.find_value("attempt_id").isNull()) {
                attempt_id = ParseHashO(options, "attempt_id");
                if (std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
                    session.attempt_ids.end()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "attempt_id does not belong to this session");
                }
            }
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttempt(attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster attempt not found");
            }
            if (action == "cancel_to_self") {
                return ResolveAlternativePaymasterRecovery(
                    context, *wallet, store, session, attempt, options);
            }
            if (action == "fallback") {
                std::string error;
                if (!store.AbandonClientAttemptForFallback(session.request_id, attempt_id,
                                                           GetTime(), error) ||
                    !store.GetSessionByRequestId(session.request_id, session) ||
                    !store.GetAttempt(attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       error.empty() ? "PAYMASTER_FALLBACK_FAILED" : error);
                }
                return ClientSessionSnapshotToJSON(store, session, action);
            }

            if (!attempt.final_transaction.empty() &&
                !attempt.final_txid.IsNull()) {
                bool broadcast{false};
                bool already_confirmed{false};
                std::string broadcast_error;
                if (action == "retry_same") {
                    const int64_t now{GetTime()};
                    CMutableTransaction decoded;
                    try {
                        SpanReader stream{::PROTOCOL_VERSION,
                                          attempt.final_transaction};
                        stream >> decoded;
                        if (!stream.empty() ||
                            SerializeTransaction(decoded) !=
                                attempt.final_transaction ||
                            CTransaction{decoded}.GetHash() !=
                                attempt.final_txid) {
                            throw std::ios_base::failure(
                                "non-canonical client final transaction");
                        }
                    } catch (const std::ios_base::failure&) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_PERSISTED_FINAL_ARTIFACT_CORRUPT");
                    }
                    const CTransactionRef exact_transaction =
                        MakeTransactionRef(decoded);
                    FinalTransactionPresence presence{
                        FinalTransactionPresence::NONE};
                    std::string validation_error;
                    if (!PreflightFinalPaymasterTransaction(
                            *wallet, exact_transaction, presence,
                            validation_error) ||
                        !store.ValidateClientDurableFinalForBroadcast(
                            *exact_transaction, now,
                            presence != FinalTransactionPresence::NONE,
                            validation_error)) {
                        throw JSONRPCError(
                            RPC_TRANSACTION_REJECTED,
                            validation_error.empty()
                                ? "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED"
                                : validation_error);
                    }
                    broadcast = InsertAndBroadcastPaymasterTransaction(
                        *wallet, exact_transaction, already_confirmed,
                        broadcast_error);
                    std::string reconcile_error;
                    if (broadcast && !store.ReconcileFinalTransaction(
                                         *exact_transaction,
                                         already_confirmed ? 1 : 0,
                                         !already_confirmed, now, reconcile_error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, reconcile_error);
                    }
                }
                if (!store.GetSessionByRequestId(session.request_id,
                                                 session)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_SESSION_DATABASE_READ");
                }
                UniValue result = ClientSessionSnapshotToJSON(
                    store, session, action);
                result.pushKV("raw_transaction",
                              HexStr(attempt.final_transaction));
                result.pushKV("txid", attempt.final_txid.GetHex());
                result.pushKV("broadcast", broadcast);
                if (!broadcast_error.empty()) {
                    result.pushKV("broadcast_error", broadcast_error);
                }
                return result;
            }
            if (attempt.retry_until > 0 && GetTime() > attempt.retry_until) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
            }
            if (!attempt.user_signed_psbt.empty()) {
                PaymasterSubmitQueueState queue_state;
                if (action == "retry_same") {
                    std::string error;
                    if (!QueuePersistedPaymasterSubmit(context, *wallet, session, attempt,
                                                       GetTime(), queue_state, error)) {
                        throw JSONRPCError(
                            error == "PAYMASTER_DIRECT_QUEUE_FULL" ? RPC_CLIENT_NODE_CAPACITY_REACHED : RPC_WALLET_ERROR,
                            error);
                    }
                }
                if (!store.GetSessionByRequestId(session.request_id,
                                                 session)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_SESSION_DATABASE_READ");
                }
                UniValue result = ClientSessionSnapshotToJSON(
                    store, session, action);
                result.pushKV("psbt", EncodeBase64(attempt.user_signed_psbt));
                if (action == "retry_same") {
                    result.pushKV("queued", queue_state.queued);
                    result.pushKV("connection_pending",
                                  queue_state.connection_pending);
                    result.pushKV("route_available",
                                  queue_state.route_available);
                }
                return result;
            } else {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RETRY_ARTIFACT_UNAVAILABLE");
            }
        },
    };
}

RPCHelpMan walletprocesspaymasterpsbt()
{
    return RPCHelpMan{
        "walletprocesspaymasterpsbt",
        "Validate an unsigned Paymaster PSBT against its exact persistent wallet template and sign only USER inputs.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "Base64-encoded unsigned Paymaster PSBT"},
            {"authorization_commitment", RPCArg::Type::STR_HEX,
             RPCArg::Optional::OMITTED,
             "Exact client authorization commitment. Required before a new USER signature; omitted only for an exact already-signed retry"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The durable user authorization artifact", {
                                                                                           {RPCResult::Type::STR, "psbt", "Canonical base64-encoded user-signed PSBT"},
                                                                                           {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                           {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                           {RPCResult::Type::STR_HEX, "attempt_id", "Persistent provider attempt"},
                                                                                           {RPCResult::Type::STR_HEX, "quote_id", "Bound quote identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "unsigned_txid", "Witness-free unsigned transaction identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "template_commitment", "Bound unsigned transaction commitment"},
                                                                                           {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Accepted client authorization commitment"},
                                                                                           {RPCResult::Type::BOOL, "authorization_accepted", "Whether the exact commitment is durably accepted"},
                                                                                           {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Monotonic durable acceptance time"},
                                                                                           {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                           {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                           {RPCResult::Type::NUM_TIME, "expires_at", "Provider retry deadline"},
                                                                                           {RPCResult::Type::BOOL, "queued", "Whether PMSUBMIT is queued to a connected v2 peer"},
                                                                                           {RPCResult::Type::BOOL, "connection_pending", "Whether reconnecting to the provider was requested"},
                                                                                           {RPCResult::Type::BOOL, "route_available", "Whether a current provider announcement supplies the retry route"},
                                                                                       }},
        RPCExamples{HelpExampleCli("walletprocesspaymasterpsbt", "\"cHNidP8...\" \"0123...\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::string readiness_error;
            if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
                throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
            }

            PartiallySignedTransaction candidate;
            std::string error;
            if (!DecodeBase64PSBT(candidate, request.params[0].get_str(), error) || !candidate.tx) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                   strprintf("Paymaster PSBT decode failed: %s", error));
            }
            const uint256 unsigned_txid = CTransaction{*candidate.tx}.GetHash();
            std::optional<uint256> supplied_authorization_commitment;
            if (request.params.size() > 1 && !request.params[1].isNull()) {
                supplied_authorization_commitment =
                    ParseHashV(request.params[1], "authorization_commitment");
            }
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "No persistent Paymaster template matches this PSBT");
            }
            DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
            if (!LoadTrustedTemplate(attempt, trusted, error) ||
                !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                    candidate, trusted,
                    DigiDollar::Paymaster::CollaborativeSignatureStage::UNSIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, session, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            const int64_t now = GetTime();
            PaymentIntent authorized_intent;
            PaymasterQuote authorized_quote;
            CollaborativePSBTTemplate authorized_template;
            if (!LoadAttemptAuthorizationArtifacts(
                    attempt, authorized_intent, authorized_quote,
                    authorized_template, error) ||
                attempt.client_manifest.manifest_id.IsNull() ||
                now > attempt.client_manifest.expires_at ||
                !ValidateClientAuthorizationManifest(
                    attempt.client_manifest, authorized_intent, authorized_quote,
                    attempt.capacity_snapshot, authorized_template, error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
            }
            if (attempt.state == AttemptState::QUOTED) {
                if (!supplied_authorization_commitment) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_AUTHORIZATION_COMMITMENT_REQUIRED");
                }
                if (*supplied_authorization_commitment !=
                    attempt.client_manifest.manifest_id) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
                }
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
                        throw std::ios_base::failure("trailing capacity artifact data");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
                }
                node::NodeContext* node = wallet->chain().context();
                if (!node || !node->chainman ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof, capacity_request,
                        attempt.provider_identity_key, *node->chainman, now,
                        error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
                }
                if (!store.AcceptClientAuthorization(
                        session.request_id, attempt.attempt_id,
                        *supplied_authorization_commitment, now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
                }
            }
            const int64_t operation_now = std::max(
                now, std::max(session.updated_at, attempt.updated_at));
            if (attempt.state != DigiDollar::Paymaster::AttemptState::QUOTED) {
                if (attempt.state >= DigiDollar::Paymaster::AttemptState::USER_SIGNED &&
                    attempt.state <= DigiDollar::Paymaster::AttemptState::MEMPOOL &&
                    !attempt.user_signed_psbt.empty()) {
                    if (operation_now > attempt.retry_until) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
                    }
                    candidate = PartiallySignedTransaction{};
                } else {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster attempt is not awaiting a user signature");
                }
            } else if (operation_now > attempt.quote_expires_at) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster quote expired");
            }

            if (candidate.tx &&
                session.state != DigiDollar::Paymaster::SessionState::INPUTS_RESERVED &&
                session.state != DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK &&
                session.state != DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE &&
                session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "Paymaster session is not awaiting a user authorization");
            }

            if (!candidate.tx) {
                if (!DecodeRawPSBT(candidate, MakeByteSpan(attempt.user_signed_psbt), error) ||
                    !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                        candidate, trusted,
                        DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Persisted Paymaster PSBT is corrupt");
                }
            } else {
                if (wallet->IsLocked()) {
                    if ((session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                         session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
                         session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) &&
                        !store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                       "Wallet unlock is required to sign Paymaster USER inputs");
                }
                if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK) {
                    if (!store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                }
                // Re-read the immutable authorization immediately before the
                // only operation that can sign client inputs. Earlier RPC
                // validation is deliberately not treated as signing authority.
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                ProviderAttempt signing_attempt;
                PaymentIntent signing_intent;
                PaymasterQuote signing_quote;
                CollaborativePSBTTemplate signing_template;
                if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
                    signing_attempt.state != AttemptState::QUOTED ||
                    !supplied_authorization_commitment ||
                    signing_attempt.accepted_client_manifest_id !=
                        *supplied_authorization_commitment ||
                    signing_attempt.accepted_client_manifest_id !=
                        signing_attempt.client_manifest.manifest_id ||
                    signing_attempt.client_manifest_accepted_at <= 0 ||
                    std::max(operation_now, signing_attempt.updated_at) >
                        signing_attempt.client_manifest.expires_at ||
                    !LoadAttemptAuthorizationArtifacts(
                        signing_attempt, signing_intent, signing_quote,
                        signing_template, error) ||
                    !ValidateClientAuthorizationManifest(
                        signing_attempt.client_manifest, signing_intent,
                        signing_quote, signing_attempt.capacity_snapshot,
                        signing_template, error) ||
                    !ValidateCollaborativePSBT(
                        candidate, signing_template,
                        CollaborativeSignatureStage::UNSIGNED, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
                }
                if (signing_intent.canonical_request_hash !=
                        session.canonical_request_hash ||
                    signing_intent.requested_fee_mode !=
                        session.fee_mode_requested ||
                    signing_intent.privacy_profile !=
                        signing_attempt.privacy_profile) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CLIENT_ORDER_BINDING_MISMATCH");
                }
                PaymasterCapacityRequest signing_capacity_request;
                PaymasterCapacityProof signing_capacity_proof;
                try {
                    SpanReader request_stream{::PROTOCOL_VERSION,
                                              signing_attempt.capacity_request};
                    request_stream >> signing_capacity_request;
                    SpanReader proof_stream{
                        ::PROTOCOL_VERSION,
                        signing_attempt.capacity_snapshot.capacity_proof};
                    proof_stream >> signing_capacity_proof;
                    if (!request_stream.empty() || !proof_stream.empty()) {
                        throw std::ios_base::failure(
                            "trailing capacity artifact data");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
                }
                node::NodeContext* signing_node = wallet->chain().context();
                if (!signing_node || !signing_node->chainman ||
                    !ValidateCapacityProofAgainstChainstate(
                        signing_capacity_proof, signing_capacity_request,
                        signing_attempt.provider_identity_key,
                        *signing_node->chainman,
                        std::max(operation_now, signing_attempt.updated_at), error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
                }
                if (!ValidateClientAuthorizationOwnership(
                        *wallet, signing_attempt.client_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, signing_attempt,
                        error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                attempt = std::move(signing_attempt);
                if (!SignCollaborativePSBTForParty(*wallet, candidate,
                                                   signing_template,
                                                   DigiDollar::Paymaster::SigningParty::USER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                const std::vector<unsigned char> signed_psbt = SerializePSBT(candidate);
                if (!store.TransitionSession(session.request_id,
                                             DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                             DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
                attempt.user_signed_psbt = signed_psbt;
                attempt.updated_at = std::max(attempt.updated_at, operation_now);
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            DigiDollar::Paymaster::PaymentSession authoritative;
            if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
                !store.GetAttempt(attempt.attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload durable Paymaster authorization");
            }

            PaymasterSubmitQueueState queue_state;
            if (!QueuePersistedPaymasterSubmit(context, *wallet, authoritative,
                                               attempt, operation_now, queue_state, error)) {
                throw JSONRPCError(
                    error == "PAYMASTER_DIRECT_QUEUE_FULL" ? RPC_CLIENT_NODE_CAPACITY_REACHED : RPC_WALLET_ERROR,
                    error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("psbt", EncodeBase64(attempt.user_signed_psbt));
            result.pushKV("request_id", authoritative.request_id);
            result.pushKV("session_id", authoritative.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", attempt.quote_id.GetHex());
            result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
            result.pushKV("template_commitment", attempt.template_commitment.GetHex());
            if (!attempt.client_manifest.manifest_id.IsNull()) {
                result.pushKV("authorization_commitment",
                              attempt.client_manifest.manifest_id.GetHex());
            }
            const bool authorization_accepted =
                attempt.accepted_client_manifest_id ==
                    attempt.client_manifest.manifest_id &&
                !attempt.accepted_client_manifest_id.IsNull() &&
                attempt.client_manifest_accepted_at > 0;
            result.pushKV("authorization_accepted",
                          authorization_accepted);
            if (authorization_accepted) {
                result.pushKV("authorization_accepted_at",
                              attempt.client_manifest_accepted_at);
            }
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(authoritative.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(attempt.state)});
            result.pushKV("expires_at", attempt.retry_until);
            result.pushKV("queued", queue_state.queued);
            result.pushKV("connection_pending", queue_state.connection_pending);
            result.pushKV("route_available", queue_state.route_available);
            return result;
        },
    };
}

RPCHelpMan submitpaymasterdigidollar()
{
    return RPCHelpMan{
        "submitpaymasterdigidollar",
        "Validate a user-signed Paymaster PSBT, sign only PROVIDER inputs, durably commit the final transaction, then broadcast it.\n"
        "An already committed transaction is inserted and broadcast idempotently without requiring another provider signature.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "Base64-encoded user-signed Paymaster PSBT"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The durable provider commit", {
                                                                               {RPCResult::Type::STR, "psbt", /*optional=*/true, "Canonical fully signed PSBT when produced by this call"},
                                                                               {RPCResult::Type::STR_HEX, "hex", "Exact durably committed final transaction"},
                                                                               {RPCResult::Type::STR_HEX, "txid", "Final transaction identifier"},
                                                                               {RPCResult::Type::BOOL, "broadcast", "Whether the committed transaction is in a local pool or already confirmed"},
                                                                               {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Stable recovery error when the durable commit could not yet be broadcast"},
                                                                               {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                               {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                               {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                               {RPCResult::Type::STR_HEX, "attempt_id", "Persistent provider attempt"},
                                                                               {RPCResult::Type::STR_HEX, "quote_id", "Bound quote identifier"},
                                                                               {RPCResult::Type::STR_HEX, "unsigned_txid", "Witness-free unsigned transaction identifier"},
                                                                               {RPCResult::Type::STR_HEX, "template_commitment", "Bound unsigned transaction commitment"},
                                                                               {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                               {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                               {RPCResult::Type::STR, "result_status", "Signed provider result status"},
                                                                               {RPCResult::Type::NUM, "result_sequence", "Monotonic result sequence"},
                                                                               {RPCResult::Type::NUM_TIME, "expires_at", "Exact retry deadline"},
                                                                           }},
        RPCExamples{HelpExampleCli("submitpaymasterdigidollar", "\"cHNidP8...\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            PartiallySignedTransaction user_psbt;
            std::string error;
            if (!DecodeBase64PSBT(user_psbt, request.params[0].get_str(), error) || !user_psbt.tx) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                   strprintf("Paymaster PSBT decode failed: %s", error));
            }
            const uint256 unsigned_txid = CTransaction{*user_psbt.tx}.GetHash();
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "No persistent Paymaster template matches this PSBT");
            }
            DigiDollar::Paymaster::ProviderIdentityRecord provider_identity;
            if (!CheckPaymasterProviderWallet(*wallet, error) ||
                !GetPaymasterIdentity(*wallet, provider_identity) ||
                provider_identity.provider_id != attempt.provider_id) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "Paymaster provider identity does not match the quote" : error);
            }
            DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
            if (!LoadTrustedTemplate(attempt, trusted, error) ||
                !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                    user_psbt, trusted,
                    DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            const std::vector<unsigned char> canonical_user_psbt = SerializePSBT(user_psbt);
            if (!attempt.user_signed_psbt.empty() && attempt.user_signed_psbt != canonical_user_psbt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PSBT_CONFLICT");
            }

            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            const int64_t now = GetTime();
            DigiDollar::Paymaster::ProviderCommitRecord committed;
            const bool already_committed = !attempt.commit_key.IsNull() &&
                                           store.GetProviderCommit(attempt.commit_key, committed);
            const bool exact_persisted_provider_signature =
                attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
                !attempt.final_txid.IsNull() &&
                !attempt.final_transaction.empty() &&
                attempt.provider_signed_at > 0 &&
                attempt.provider_signed_at <= attempt.retry_until &&
                !attempt.provider_signed_result.empty();
            if (!already_committed && !exact_persisted_provider_signature &&
                now > attempt.retry_until) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
            }
            if (!already_committed && !exact_persisted_provider_signature) {
                PaymentIntent authorized_intent;
                PaymasterQuote authorized_quote;
                CollaborativePSBTTemplate authorized_template;
                if (!LoadAttemptAuthorizationArtifacts(
                        attempt, authorized_intent, authorized_quote,
                        authorized_template, error) ||
                    attempt.provider_manifest.manifest_id.IsNull() ||
                    now > attempt.provider_manifest.expires_at ||
                    !ValidateProviderAuthorizationManifest(
                        attempt.provider_manifest, authorized_intent,
                        authorized_quote, authorized_template, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_EXPIRED" : error);
                }
                if (!ValidateProviderAuthorizationOwnership(
                        *wallet, attempt.provider_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (!already_committed &&
                attempt.state != DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
                wallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                   "Provider wallet unlock is required for a new Paymaster signature");
            }

            if (!already_committed) {
                bool rejected{false};
                DigiDollar::Paymaster::PaymasterResult rejection;
                if (!RejectUnavailableTemplateInputs(*wallet, store, attempt, *user_psbt.tx,
                                                     now, rejected, rejection, error)) {
                    throw JSONRPCError(
                        error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                        error);
                }
                if (rejected) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE");
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::QUOTED) {
                if (now > attempt.quote_expires_at) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster quote expired");
                }
                if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) {
                    if (!store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                } else if (session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED &&
                           session.state != DigiDollar::Paymaster::SessionState::PENDING_PROVIDER) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "Paymaster session cannot accept a user authorization");
                }
                attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
                attempt.user_signed_psbt = canonical_user_psbt;
                attempt.updated_at = std::max(attempt.updated_at, now);
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_SIGNED) {
                if (!store.AcceptUserAuthorization(session.request_id, attempt.attempt_id,
                                                   Hash(canonical_user_psbt), now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            PartiallySignedTransaction full_psbt{user_psbt};
            bool signed_now{false};
            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_PSBT_ACCEPTED) {
                ProviderAttempt signing_attempt;
                CollaborativePSBTTemplate signing_template;
                if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
                    signing_attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
                    !ValidateDurableProviderUserAuthorization(
                        store, signing_attempt, error) ||
                    !ValidateProviderAuthorizationForExecution(
                        signing_attempt, now, signing_template, error) ||
                    !ValidateCollaborativePSBT(
                        full_psbt, signing_template,
                        CollaborativeSignatureStage::USER_SIGNED, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_STATE" : error);
                }
                attempt = std::move(signing_attempt);
                node::NodeContext* node_ctx = wallet->chain().context();
                PaymasterCapacityRequest capacity_request;
                PaymasterCapacityProof capacity_proof;
                if (!node_ctx || !node_ctx->chainman) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
                }
                if (!store.ValidateProviderPreSignatureAuthorization(
                        attempt, Params().GenesisBlock().GetHash(), now,
                        capacity_request, capacity_proof, error) ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof, capacity_request,
                        attempt.provider_identity_key, *node_ctx->chainman,
                        now, error) ||
                    !TemplateInputsAvailable(*wallet, *full_psbt.tx)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE" : error);
                }
                if (!ValidateProviderAuthorizationOwnership(
                        *wallet, attempt.provider_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!SignCollaborativePSBTForParty(*wallet, full_psbt,
                                                   signing_template,
                                                   DigiDollar::Paymaster::SigningParty::PROVIDER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                CMutableTransaction final_transaction;
                if (!FinalizeAndExtractPSBT(full_psbt, final_transaction)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_FINALIZATION_FAILED");
                }
                const CTransaction signed_transaction{final_transaction};
                if (!ValidateFinalCollaborativeTransaction(
                        final_transaction, signed_transaction.GetHash(),
                        signed_transaction.GetWitnessHash(), signing_template,
                        error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                // The signed result below is private crash-recovery authority.
                // Do not persist even that envelope until the exact transaction
                // has passed the shared script, witness, chain and mempool
                // firewall. The second preflight before the atomic commit stays
                // in place to close the race after this observation.
                FinalTransactionPresence signed_presence{
                    FinalTransactionPresence::NONE};
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, MakeTransactionRef(final_transaction),
                        signed_presence, error)) {
                    const bool transient =
                        error == "PAYMASTER_NODE_CONTEXT_UNAVAILABLE" ||
                        error == "PAYMASTER_TXINDEX_NOT_READY";
                    throw JSONRPCError(
                        transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error);
                }
                signed_now = true;
                attempt.state = DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED;
                attempt.final_transaction = SerializeTransaction(final_transaction);
                attempt.final_txid = CTransaction{final_transaction}.GetHash();
                const int64_t provider_signed_at =
                    std::max(attempt.updated_at, now);
                DigiDollar::Paymaster::ProviderCommitRecord signed_commit;
                signed_commit.commit_key = attempt.commit_key;
                signed_commit.provider_id = attempt.provider_id;
                signed_commit.quote_id = attempt.quote_id;
                signed_commit.template_commitment =
                    attempt.template_commitment;
                signed_commit.final_txid = attempt.final_txid;
                signed_commit.raw_transaction_hash =
                    Hash(attempt.final_transaction);
                signed_commit.final_transaction =
                    attempt.final_transaction;
                signed_commit.provider_inputs = ProviderInputs(
                    *full_psbt.tx, attempt.input_roles);
                signed_commit.committed_at = provider_signed_at;
                signed_commit.retry_until = attempt.retry_until;
                DigiDollar::Paymaster::PaymasterResult signed_result;
                if (!BuildFinalProviderResult(
                        *wallet, signed_commit, provider_signed_at,
                        signed_result, error)) {
                    throw JSONRPCError(
                        error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                        error);
                }
                attempt.provider_signed_at = provider_signed_at;
                attempt.provider_signed_result =
                    SerializeRecoveryMessage(signed_result);
                attempt.updated_at = provider_signed_at;
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED) {
                DigiDollar::Paymaster::ProviderCommitRecord commit;
                commit.commit_key = attempt.commit_key;
                commit.provider_id = attempt.provider_id;
                commit.quote_id = attempt.quote_id;
                commit.template_commitment = attempt.template_commitment;
                commit.final_txid = attempt.final_txid;
                commit.raw_transaction_hash = Hash(attempt.final_transaction);
                commit.final_transaction = attempt.final_transaction;
                commit.provider_inputs = ProviderInputs(*user_psbt.tx, attempt.input_roles);
                // A crash may leave the complete provider-signed transaction
                // durable without its atomic ProviderCommit. Preserve the
                // signature boundary while recording the actual (monotonic)
                // commit time, even when the retry window has since elapsed.
                const int64_t commit_time =
                    std::max(now, attempt.provider_signed_at);
                commit.committed_at = commit_time;
                commit.retry_until = attempt.retry_until;
                const std::optional<uint256> sponsorship_hash =
                    attempt.sponsorship_capability_hash.IsNull() ? std::nullopt : std::optional<uint256>{attempt.sponsorship_capability_hash};
                DigiDollar::Paymaster::PaymasterResult final_result;
                if (!DeserializeCanonicalRecoveryMessage(
                        attempt.provider_signed_result, final_result,
                        error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID");
                }

                // Reload the durable PROVIDER_SIGNED attempt and re-run both
                // the provider authorization firewall and the full final
                // transaction verifier at the last possible point before the
                // atomic FINAL_COMMITTED transition.
                ProviderAttempt committing_attempt;
                CMutableTransaction validated_final;
                FinalTransactionPresence final_presence{
                    FinalTransactionPresence::NONE};
                if (!store.GetAttempt(attempt.attempt_id, committing_attempt) ||
                    committing_attempt.state != AttemptState::PROVIDER_SIGNED ||
                    !ValidateProviderCommitForExecution(
                        committing_attempt, commit, commit_time, validated_final,
                        error) ||
                    !store.ValidateProviderBudgetAuthorization(
                        committing_attempt,
                        BudgetReservationState::RESERVED,
                        /*allow_historical_policy=*/true, error)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_INVALID" : error);
                }
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, MakeTransactionRef(validated_final),
                        final_presence, error)) {
                    const bool transient =
                        error == "PAYMASTER_NODE_CONTEXT_UNAVAILABLE" ||
                        error == "PAYMASTER_TXINDEX_NOT_READY";
                    throw JSONRPCError(
                        transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error);
                }
                attempt = std::move(committing_attempt);
                if (!store.CommitProviderFinalTransaction(session.request_id, attempt.attempt_id,
                                                          commit, sponsorship_hash,
                                                          final_result,
                                                          Params().GenesisBlock().GetHash(),
                                                          error) ||
                    !store.GetProviderCommit(attempt.commit_key, committed)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (!already_committed && committed.final_transaction.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "Paymaster attempt is not recoverable by exact provider retry");
            }

            ProviderCommitRecoveryResult recovery =
                RecoverProviderCommit(
                    *wallet, store, committed,
                    std::max(now, committed.committed_at));
            if (recovery.provider_result.commit_key.IsNull()) {
                const int code = recovery.error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR;
                throw JSONRPCError(code, recovery.error);
            }

            DigiDollar::Paymaster::PaymentSession authoritative;
            if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
                !store.GetAttempt(attempt.attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload durable provider commit");
            }
            UniValue result{UniValue::VOBJ};
            if (signed_now) result.pushKV("psbt", EncodeBase64(SerializePSBT(full_psbt)));
            result.pushKV("hex", HexStr(committed.final_transaction));
            result.pushKV("txid", committed.final_txid.GetHex());
            result.pushKV("broadcast", recovery.broadcast);
            if (!recovery.error.empty()) result.pushKV("broadcast_error", recovery.error);
            result.pushKV("request_id", authoritative.request_id);
            result.pushKV("session_id", authoritative.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", attempt.quote_id.GetHex());
            result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
            result.pushKV("template_commitment", attempt.template_commitment.GetHex());
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(authoritative.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(attempt.state)});
            result.pushKV("result_status", ResultStatusName(recovery.provider_result.status));
            result.pushKV("result_sequence", recovery.provider_result.result_sequence);
            result.pushKV("expires_at", attempt.retry_until);
            return result;
        },
    };
}

} // namespace wallet
