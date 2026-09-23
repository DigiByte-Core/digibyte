// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider request, submit, result, and recovery message processing. */

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

RPCHelpMan processpaymasterresult()
{
    return RPCHelpMan{
        "processpaymasterresult",
        "Validate and persist at most one PMRESULT for an existing client session.\n",
        {{"request_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Canonical request UUID"}},
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative result processing state", {
                                                                                         {RPCResult::Type::BOOL, "processed", "Whether a matching result was available"},
                                                                                         {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                         {RPCResult::Type::STR_HEX, "session_id", "Persistent session"},
                                                                                         {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                         {RPCResult::Type::STR_HEX, "offer_id", "Exact authorized offer"},
                                                                                         {RPCResult::Type::STR_HEX, "policy_hash", "Exact authorized provider policy"},
                                                                                         {RPCResult::Type::STR, "funding_model", "Exact authorized funding model"},
                                                                                         {RPCResult::Type::NUM, "payment_cents", "Exact recipient amount"},
                                                                                         {RPCResult::Type::NUM, "service_fee_cents", "Exact provider service fee"},
                                                                                         {RPCResult::Type::NUM, "user_total_cents", "Exact maximum wallet outflow"},
                                                                                         {RPCResult::Type::STR_HEX, "authorization_commitment", "Accepted client authorization manifest"},
                                                                                         {RPCResult::Type::BOOL, "authorization_accepted", "Whether the exact manifest remains accepted"},
                                                                                         {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                         {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                         {RPCResult::Type::STR, "result_status", /*optional=*/true, "Signed provider status"},
                                                                                         {RPCResult::Type::NUM, "result_sequence", /*optional=*/true, "Monotonic sequence"},
                                                                                         {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final transaction"},
                                                                                     }},
        RPCExamples{HelpExampleCli("processpaymasterresult", "\"550e8400-e29b-41d4-a716-446655440000\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            if (!context.paymaster || !context.paymaster->Enabled()) {
                throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar Paymaster support is disabled");
            }
            const std::string request_id = request.params[0].get_str();
            if (!IsCanonicalRequestId(request_id)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "request_id must be a canonical lowercase UUID");
            }
            PaymasterStore store{*wallet};
            PaymentSession session;
            if (!store.GetSessionByRequestId(request_id, session) || session.attempt_ids.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
            }
            ProviderAttempt attempt;
            if (!store.GetAttempt(session.attempt_ids.back(), attempt) ||
                !attempt.provider_identity_key.IsFullyValid()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_IDENTITY_NOT_PERSISTED");
            }
            const int64_t operation_now{GetTime()};
            std::string error;
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt, error,
                    EquivocationBlockPolicy::OBSERVE_ONLY)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            PaymentIntent authorized_intent;
            PaymasterQuote authorized_quote;
            CollaborativePSBTTemplate authorized_template;
            if (!HasDurableClientAuthorization(attempt, operation_now) ||
                attempt.client_manifest_accepted_at > session.updated_at ||
                !LoadAttemptAuthorizationArtifacts(
                    attempt, authorized_intent, authorized_quote,
                    authorized_template, error) ||
                !ValidateClientAuthorizationManifest(
                    attempt.client_manifest, authorized_intent,
                    authorized_quote, attempt.capacity_snapshot,
                    authorized_template, error) ||
                !ValidateClientAuthorizationOwnership(
                    *wallet, attempt.client_manifest, error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED" : error);
            }
            const auto messages = context.paymaster->TakeResults(
                request_id, session.session_id, attempt.provider_id, 1);
            std::optional<PaymasterResult> accepted_result;
            FinalTransactionPresence final_presence{FinalTransactionPresence::NONE};
            if (!messages.empty()) {
                const auto* message = std::get_if<PaymasterResultMessage>(&messages.front().payload);
                if (!message) throw JSONRPCError(RPC_INTERNAL_ERROR, "PAYMASTER_RESULT_QUEUE_CORRUPT");
                PaymasterResult previous;
                const uint64_t minimum_sequence = store.GetProviderResult(attempt.commit_key, previous) ? previous.result_sequence : 1;
                if (!ValidatePaymasterResult(message->result, Params().GenesisBlock().GetHash(),
                                             attempt.provider_id, attempt.commit_key,
                                             attempt.provider_identity_key, minimum_sequence, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                if (message->result.final_transaction) {
                    CollaborativePSBTTemplate trusted;
                    if (!LoadTrustedTemplate(attempt, trusted, error) ||
                        !ValidateFinalCollaborativeTransaction(
                            *message->result.final_transaction, *message->result.txid,
                            *message->result.raw_transaction_hash, trusted, error)) {
                        const std::string validation_error{error};
                        std::string persistence_error;
                        if (!MarkFinalValidationFailureRecoverable(
                                store, session, attempt, GetTime(), persistence_error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, persistence_error);
                        }
                        throw JSONRPCError(RPC_INVALID_PARAMETER, validation_error);
                    }
                    if (!PreflightFinalPaymasterTransaction(
                            *wallet, MakeTransactionRef(*message->result.final_transaction),
                            final_presence, error)) {
                        const std::string preflight_error{error};
                        const bool transient =
                            IsTransientPaymasterFinalizationError(preflight_error);
                        if (!transient) {
                            std::string persistence_error;
                            if (!MarkFinalValidationFailureRecoverable(
                                    store, session, attempt, GetTime(), persistence_error)) {
                                throw JSONRPCError(RPC_WALLET_ERROR, persistence_error);
                            }
                        }
                        throw JSONRPCError(
                            transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                            preflight_error);
                    }
                }
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, attempt, error,
                        EquivocationBlockPolicy::OBSERVE_ONLY)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!store.StoreClientResult(message->result, Params().GenesisBlock().GetHash(),
                                             attempt.attempt_id, GetTime(), error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                accepted_result = message->result;
                const bool final = message->result.final_transaction.has_value();
                if (final) {
                    const CMutableTransaction& transaction = *message->result.final_transaction;
                    const CTransaction exact_final{transaction};
                    const auto fail_after_final_store =
                        [&](int rpc_code, const std::string& failure) -> void {
                        std::string persistence_error;
                        if (!store.RecordClientFinalConflict(
                                request_id, attempt.attempt_id,
                                exact_final.GetHash(), exact_final.GetWitnessHash(),
                                GetTime(), persistence_error)) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                persistence_error.empty() ? "PAYMASTER_FINAL_CONFLICT_NOT_PERSISTED" : persistence_error);
                        }
                        throw JSONRPCError(
                            rpc_code,
                            failure.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : failure);
                    };
                    // StoreClientResult commits the signed result, final
                    // artifacts, user-authorization evidence, attempt and
                    // session transition in one wallet transaction.
                    if (!store.GetAttempt(attempt.attempt_id, attempt) ||
                        !store.GetSessionByRequestId(request_id, session)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_FINAL_STATE_RELOAD_FAILED");
                    }

                    // Result persistence and state reconciliation can perform
                    // database work. Reload and revalidate the exact authority
                    // and final bytes at the last possible point before the
                    // transaction is handed to the node for broadcast.
                    ProviderAttempt broadcast_attempt;
                    PaymentIntent broadcast_intent;
                    PaymasterQuote broadcast_quote;
                    CollaborativePSBTTemplate broadcast_template;
                    FinalTransactionPresence broadcast_presence{
                        FinalTransactionPresence::NONE};
                    if (!store.GetAttempt(attempt.attempt_id, broadcast_attempt)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_FINAL_STATE_RELOAD_FAILED");
                    }
                    if (!HasDurableClientAuthorization(
                            broadcast_attempt, GetTime()) ||
                        broadcast_attempt.client_manifest_accepted_at >
                            session.updated_at ||
                        !LoadAttemptAuthorizationArtifacts(
                            broadcast_attempt, broadcast_intent, broadcast_quote,
                            broadcast_template, error) ||
                        !ValidateClientAuthorizationManifest(
                            broadcast_attempt.client_manifest, broadcast_intent,
                            broadcast_quote, broadcast_attempt.capacity_snapshot,
                            broadcast_template, error) ||
                        !ValidateClientAuthorizationOwnership(
                            *wallet, broadcast_attempt.client_manifest, error) ||
                        !ValidateFinalCollaborativeTransaction(
                            transaction, CTransaction{transaction}.GetHash(),
                            CTransaction{transaction}.GetWitnessHash(),
                            broadcast_template, error) ||
                        broadcast_attempt.final_txid != CTransaction{transaction}.GetHash() ||
                        broadcast_attempt.final_transaction !=
                            SerializeTransaction(transaction)) {
                        fail_after_final_store(
                            RPC_TRANSACTION_REJECTED,
                            error.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : error);
                    }
                    if (!PreflightFinalPaymasterTransaction(
                            *wallet, MakeTransactionRef(transaction),
                            broadcast_presence, error)) {
                        if (IsTransientPaymasterFinalizationError(error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        fail_after_final_store(RPC_TRANSACTION_REJECTED, error);
                    }
                    if (!store.ValidateClientDurableFinalForBroadcast(
                            CTransaction{transaction}, GetTime(),
                            broadcast_presence != FinalTransactionPresence::NONE,
                            error)) {
                        if (IsTransientPaymasterFinalizationError(error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        fail_after_final_store(
                            RPC_TRANSACTION_REJECTED,
                            error.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : error);
                    }
                    if (!DrainClientAttemptEquivocations(
                            *context.paymaster, store, session,
                            broadcast_attempt, error,
                            EquivocationBlockPolicy::OBSERVE_ONLY)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    bool already_confirmed{false};
                    std::string broadcast_error;
                    if (InsertAndBroadcastPaymasterTransaction(
                            *wallet, MakeTransactionRef(transaction), already_confirmed,
                            broadcast_error)) {
                        if (!store.RecordClientFinalObservation(
                                request_id, attempt.attempt_id,
                                already_confirmed, GetTime(), error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                    } else {
                        if (IsTransientPaymasterFinalizationError(
                                broadcast_error)) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                broadcast_error.empty() ? "PAYMASTER_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
                        }
                        fail_after_final_store(
                            RPC_TRANSACTION_REJECTED,
                            broadcast_error.empty() ? "PAYMASTER_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
                    }
                } else if (message->result.status == PaymasterResultStatus::REJECTED ||
                           message->result.status == PaymasterResultStatus::SLOT_UNAVAILABLE) {
                    if (!store.GetAttempt(attempt.attempt_id, attempt) ||
                        !store.GetSessionByRequestId(request_id, session)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                }
            }
            store.GetSessionByRequestId(request_id, session);
            store.GetAttempt(session.attempt_ids.back(), attempt);

            // Collaborative Paymaster transactions bypass the ordinary
            // TransferDigiDollarMany() path that records an outgoing DD row.
            // Once exact final bytes are durable, repair that display-only
            // history idempotently. A history-write failure must never turn a
            // valid, possibly already broadcast payment into an RPC failure;
            // every later result poll will retry the same deterministic row.
            if (!attempt.final_txid.IsNull()) {
                if (DigiDollarWallet* dd_wallet = wallet->GetDDWallet()) {
                    std::string history_error;
                    const CAmount user_total =
                        authorized_intent.recipient_amount.value +
                        authorized_quote.service_fee.value;
                    if (!dd_wallet->RecordPaymasterSendHistory(
                            attempt.final_txid,
                            authorized_intent.recipient_script,
                            user_total,
                            history_error)) {
                        LogPrintf(
                            "Paymaster: unable to record client DD history: %s\n",
                            history_error);
                    }
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("processed", accepted_result.has_value());
            result.pushKV("request_id", request_id);
            result.pushKV("session_id", session.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            // Always echo the exact durable authorization, including on the
            // final result response. Callers must never reconstruct a funding
            // model or fee from a stale directory preview, and Qt needs to
            // distinguish a completed payment from a changed pre-signing
            // authorization without guessing missing fields.
            result.pushKV("offer_id", authorized_intent.offer_id.GetHex());
            result.pushKV("policy_hash", authorized_intent.policy_hash.GetHex());
            result.pushKV(
                "funding_model",
                authorized_intent.funding_model == FundingModel::SPONSORED
                    ? "sponsored"
                    : "user_paid");
            result.pushKV("payment_cents",
                          authorized_intent.recipient_amount.value);
            result.pushKV("service_fee_cents",
                          authorized_quote.service_fee.value);
            result.pushKV("user_total_cents",
                          authorized_intent.recipient_amount.value +
                              authorized_quote.service_fee.value);
            result.pushKV("authorization_commitment",
                          attempt.client_manifest.manifest_id.GetHex());
            result.pushKV(
                "authorization_accepted",
                attempt.accepted_client_manifest_id ==
                        attempt.client_manifest.manifest_id &&
                    attempt.client_manifest_accepted_at > 0);
            result.pushKV("session_state", std::string{SessionStateName(session.state)});
            result.pushKV("attempt_state", std::string{AttemptStateName(attempt.state)});
            if (accepted_result) {
                result.pushKV("result_status", ResultStatusName(accepted_result->status));
                result.pushKV("result_sequence", accepted_result->result_sequence);
                if (accepted_result->txid) result.pushKV("txid", accepted_result->txid->GetHex());
            }
            return result;
        },
    };
}

RPCHelpMan processpaymastersubmits()
{
    return RPCHelpMan{
        "processpaymastersubmits",
        "Process at most one addressed PMSUBMIT through the durable provider commit path.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider submit processing result", {
                                                                                     {RPCResult::Type::BOOL, "processed", "Whether a submit was available"},
                                                                                     {RPCResult::Type::BOOL, "queued", "Whether PMRESULT was queued"},
                                                                                     {RPCResult::Type::STR, "message_type", /*optional=*/true, "submit or recovery_submit"},
                                                                                     {RPCResult::Type::STR, "request_id", /*optional=*/true, "Canonical request UUID"},
                                                                                     {RPCResult::Type::STR_HEX, "session_id", /*optional=*/true, "Client session"},
                                                                                     {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                                     {RPCResult::Type::STR_HEX, "commit_key", /*optional=*/true, "Durable commit key"},
                                                                                     {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Final transaction"},
                                                                                     {RPCResult::Type::STR, "result_status", /*optional=*/true, "Signed provider result status"},
                                                                                     {RPCResult::Type::STR, "attempt_state", /*optional=*/true, "Authoritative provider attempt state"},
                                                                                     {RPCResult::Type::STR, "rejection_code", /*optional=*/true, "Stable local reason for a rejected stale submit"},
                                                                                     {RPCResult::Type::BOOL, "broadcast", /*optional=*/true, "Whether an alternative-recovery transaction was broadcast"},
                                                                                     {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Stable alternative-recovery broadcast error"},
                                                                                     {RPCResult::Type::OBJ, "recovery", /*optional=*/true, "Persistent alternative-recovery authority and state", {
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
                                                                                                                                                                                                  }},
                                                                                     {RPCResult::Type::OBJ, "commit", /*optional=*/true, "Durable provider commit and broadcast result", {
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
                                                                                 }},
        RPCExamples{HelpExampleCli("processpaymastersubmits", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const bool automatic_service =
                request.strMethod == INTERNAL_PAYMASTER_SERVICE_METHOD;
            const ExactFinalTxIndexMode service_txindex_mode =
                automatic_service ? ExactFinalTxIndexMode::NONBLOCKING
                                  : ExactFinalTxIndexMode::WAIT_FOR_SYNC;
            if (automatic_service) {
                if (!AutomaticProviderStateIsSynchronized(*wallet)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_SYNCING");
                }
            } else {
                wallet->BlockUntilSyncedToCurrentChain();
            }
            ProviderIdentityRecord identity;
            if (!GetPaymasterIdentity(*wallet, identity) || !context.paymaster ||
                !context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_NOT_RUNNING");
            }
            ProviderSettings runtime_settings;
            if (!GetPaymasterProviderSettings(*wallet, runtime_settings)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND");
            }
            if (runtime_settings.operation_mode ==
                    ProviderOperationMode::AUTOMATIC &&
                !automatic_service) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE");
            }
            ProviderWorkGuard work_guard{*context.paymaster,
                                         wallet->GetName(),
                                         identity.provider_id};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SERVICE_BUSY");
            }
            std::string error;
            const int64_t now = GetTime();
            const auto recovery_lease =
                context.paymaster->LeaseRecoverySubmits(identity.provider_id, 1);
            const auto& recovery_messages = recovery_lease.Messages();
            if (!recovery_messages.empty()) {
                const DirectMessage& direct = recovery_messages.front();
                ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                          direct};
                const auto* submit =
                    std::get_if<AlternativeRecoverySubmit>(&direct.payload);
                if (!submit || !ValidateAlternativeRecoverySubmitEnvelope(
                                   *submit, Params().GenesisBlock().GetHash(),
                                   error)) {
                    if (!AcknowledgeRejectedDirectMessage(
                            *context.paymaster, direct, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_INVALID_RECOVERY_SUBMIT" : error);
                }
                PaymasterStore store{*wallet};
                AlternativeRecoveryRecord recovery;
                if (!store.GetAlternativeRecoveryById(submit->recovery_id,
                                                      recovery) ||
                    !recovery.provider_side || recovery.expired ||
                    recovery.request_id != submit->request_id ||
                    recovery.session_id != submit->session_id ||
                    recovery.recovery_provider_id !=
                        submit->recovery_provider_id ||
                    recovery.recovery_request_hash !=
                        submit->recovery_request_hash ||
                    recovery.recovery_response.recovery_commit_key !=
                        submit->recovery_commit_key ||
                    recovery.recovery_response.manifest.template_commitment !=
                        submit->template_commitment) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "PAYMASTER_RECOVERY_SUBMIT_BINDING_MISMATCH");
                }

                const auto queue_result = [&](const AlternativeRecoveryRecord& record) {
                    AlternativeRecoveryResultMessage message;
                    message.request_id = record.request_id;
                    message.session_id = record.session_id;
                    message.recovery_id = record.recovery_id;
                    message.recovery_request_hash =
                        record.recovery_request_hash;
                    message.result = record.signed_result;
                    const std::vector<unsigned char> encoded =
                        SerializeRecoveryMessage(message);
                    const uint256 message_id{Hash(encoded)};
                    const bool queued =
                        context.paymaster->HasOutboundDirectMessage(
                            direct.peer_id, message_id) ||
                        context.paymaster->QueueOutboundDirectMessage(
                            direct.peer_id, message_id, encoded.size(),
                            DirectPayload{message}, now);
                    node::NodeContext* node_ctx = wallet->chain().context();
                    if (queued && node_ctx && node_ctx->connman) {
                        node_ctx->connman->WakeMessageHandler();
                    }
                    return queued;
                };

                if (recovery.phase ==
                    AlternativeRecoveryPhase::FINAL_COMMITTED) {
                    if (recovery.user_signed_psbt != submit->user_psbt ||
                        recovery.signed_result.status !=
                            PaymasterResultStatus::FINAL_COMMITTED) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
                    }
                    UniValue result{UniValue::VOBJ};
                    result.pushKV("processed", true);
                    result.pushKV("queued", queue_result(recovery));
                    result.pushKV("message_type", "recovery_submit");
                    result.pushKV("request_id", recovery.request_id);
                    result.pushKV("session_id", recovery.session_id.GetHex());
                    result.pushKV("provider_id",
                                  recovery.recovery_provider_id.GetHex());
                    result.pushKV("commit_key",
                                  recovery.recovery_response.recovery_commit_key
                                      .GetHex());
                    result.pushKV("txid",
                                  recovery.signed_result.txid->GetHex());
                    result.pushKV("result_status", "final_committed");
                    result.pushKV("recovery",
                                  AlternativeRecoveryToJSON(recovery));
                    return result;
                }

                node::NodeContext* node_ctx = wallet->chain().context();
                if (!node_ctx || !node_ctx->chainman) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                                       "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
                }
                // Receiving an exact USER-signed replay is not authority to
                // create a new provider signature after the response or
                // Capacity window expired. Only an already durable final may
                // use the separately named authorized-retry firewall.
                const int64_t authorization_time{now};
                AlternativeRecoveryParameters parameters;
                AlternativeRecoveryTemplate trusted;
                PartiallySignedTransaction unsigned_psbt;
                PartiallySignedTransaction user_psbt;
                if (!BuildRecoveryParametersFromRecord(
                        *wallet, recovery, parameters, error) ||
                    !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                        recovery.recovery_response,
                        recovery.recovery_request,
                        recovery.recovery_provider_identity_key, parameters,
                        Params(), *node_ctx->chainman, authorization_time, trusted,
                        unsigned_psbt, error) ||
                    !ValidateAlternativeRecoverySubmitAgainstChainstate(
                        *submit, recovery.recovery_response, trusted,
                        parameters, Params(), *node_ctx->chainman,
                        authorization_time,
                        user_psbt, error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_INVALID_RECOVERY_SUBMIT" : error);
                }

                if (recovery.phase ==
                    AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
                    recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
                    recovery.user_signed_psbt = submit->user_psbt;
                    recovery.updated_at = std::max(recovery.updated_at, now);
                    if (!store.UpdateAlternativeRecovery(recovery, error) ||
                        !store.GetAlternativeRecoveryById(
                            submit->recovery_id, recovery)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                } else if (recovery.phase !=
                               AlternativeRecoveryPhase::USER_SIGNED ||
                           recovery.user_signed_psbt != submit->user_psbt) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
                }
                if (wallet->IsLocked()) {
                    throw JSONRPCError(
                        RPC_WALLET_UNLOCK_NEEDED,
                        "Provider wallet unlock is required for a recovery signature");
                }

                // Reload and rebuild every local authority immediately before
                // exposing a provider signature. The peer-supplied PSBT is
                // never itself trusted as the signing policy.
                parameters = {};
                trusted = {};
                unsigned_psbt = {};
                user_psbt = {};
                AlternativeRecoverySubmit persisted_submit{*submit};
                persisted_submit.user_psbt = recovery.user_signed_psbt;
                if (!store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
                        recovery, Params().GenesisBlock().GetHash(), now,
                        error) ||
                    !BuildRecoveryParametersFromRecord(
                        *wallet, recovery, parameters, error) ||
                    !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                        recovery.recovery_response,
                        recovery.recovery_request,
                        recovery.recovery_provider_identity_key, parameters,
                        Params(), *node_ctx->chainman, authorization_time,
                        trusted,
                        unsigned_psbt, error) ||
                    !ValidateAlternativeRecoverySubmitAgainstChainstate(
                        persisted_submit, recovery.recovery_response, trusted,
                        parameters, Params(), *node_ctx->chainman,
                        authorization_time,
                        user_psbt, error)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_AUTHORIZATION_INVALID" : error);
                }
                const AlternativeRecoveryManifest& manifest =
                    recovery.recovery_response.manifest;
                if (!SignCollaborativePSBTForParty(
                        *wallet, user_psbt, trusted.trusted_template,
                        SigningParty::PROVIDER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                CMutableTransaction final_transaction;
                if (!FinalizeAndExtractPSBT(user_psbt, final_transaction)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FINALIZATION_FAILED");
                }
                const CTransaction final_tx{final_transaction};
                if (!ValidateFinalAlternativeRecoveryAgainstChainstate(
                        final_transaction, final_tx.GetWitnessHash(), trusted,
                        parameters, Params(), *node_ctx->chainman,
                        authorization_time,
                        /*exact_final_already_known=*/false, error)) {
                    throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
                }
                FinalTransactionPresence presence{FinalTransactionPresence::NONE};
                const CTransactionRef final_ref =
                    MakeTransactionRef(final_transaction);
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, final_ref, presence, error,
                        service_txindex_mode)) {
                    throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
                }

                ProviderCommitRecord commit;
                commit.commit_key =
                    recovery.recovery_response.recovery_commit_key;
                commit.provider_id = recovery.recovery_provider_id;
                commit.quote_id = recovery.recovery_id;
                commit.template_commitment = manifest.template_commitment;
                commit.final_txid = final_tx.GetHash();
                commit.final_transaction =
                    SerializeTransaction(final_transaction);
                commit.raw_transaction_hash =
                    Hash(commit.final_transaction);
                commit.provider_inputs =
                    manifest.recovery_provider_carrier_inputs;
                commit.provider_inputs.insert(
                    commit.provider_inputs.end(),
                    manifest.recovery_provider_dgb_inputs.begin(),
                    manifest.recovery_provider_dgb_inputs.end());
                commit.committed_at = now;
                commit.retry_until =
                    recovery.recovery_response.expires_at;
                PaymasterResult final_result;
                if (!BuildFinalProviderResult(
                        *wallet, commit, now, final_result, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
                recovery.final_transaction = commit.final_transaction;
                recovery.expected_wtxid = final_tx.GetWitnessHash();
                recovery.signed_result = final_result;
                recovery.updated_at = std::max(recovery.updated_at, now);
                if (!store.CommitProviderAlternativeRecoveryFinal(
                        recovery, commit, final_result,
                        Params().GenesisBlock().GetHash(), error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }

                // The commit above irrevocably consumes the provider budget
                // and pool slots because a valid provider signature now
                // exists. Reload those exact durable artifacts and repeat the
                // complete authority and mempool firewall. A failure leaves
                // FINAL_COMMITTED intact for safe retry; it must never roll
                // the economic reservation back to AVAILABLE.
                ProviderCommitRecord broadcast_commit;
                PaymasterResult broadcast_result;
                AlternativeRecoveryRecord broadcast_recovery;
                if (!store.GetProviderCommit(
                        commit.commit_key, broadcast_commit) ||
                    !store.GetProviderResult(
                        commit.commit_key, broadcast_result) ||
                    !store.GetProviderAlternativeRecoveryByCommit(
                        broadcast_commit, broadcast_recovery, error) ||
                    broadcast_commit.final_txid != commit.final_txid ||
                    broadcast_commit.raw_transaction_hash !=
                        commit.raw_transaction_hash ||
                    broadcast_commit.final_transaction !=
                        commit.final_transaction ||
                    !broadcast_result.txid ||
                    *broadcast_result.txid != commit.final_txid ||
                    !broadcast_result.raw_transaction_hash ||
                    *broadcast_result.raw_transaction_hash !=
                        final_tx.GetWitnessHash() ||
                    !broadcast_result.final_transaction ||
                    SerializeTransaction(
                        *broadcast_result.final_transaction) !=
                        commit.final_transaction) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_FINAL_RELOAD_FAILED" : error);
                }
                AlternativeRecoveryParameters broadcast_parameters;
                AlternativeRecoveryTemplate broadcast_trusted;
                PartiallySignedTransaction broadcast_unsigned_psbt;
                PartiallySignedTransaction broadcast_user_psbt;
                AlternativeRecoverySubmit broadcast_submit{*submit};
                broadcast_submit.user_psbt =
                    broadcast_recovery.user_signed_psbt;
                FinalTransactionPresence broadcast_presence{
                    FinalTransactionPresence::NONE};
                if (!BuildRecoveryParametersFromRecord(
                        *wallet, broadcast_recovery, broadcast_parameters,
                        error) ||
                    !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                        broadcast_recovery.recovery_response,
                        broadcast_recovery.recovery_request,
                        broadcast_recovery.recovery_provider_identity_key,
                        broadcast_parameters, Params(), *node_ctx->chainman,
                        authorization_time, broadcast_trusted,
                        broadcast_unsigned_psbt, error) ||
                    !ValidateAlternativeRecoverySubmitAgainstChainstate(
                        broadcast_submit,
                        broadcast_recovery.recovery_response,
                        broadcast_trusted, broadcast_parameters, Params(),
                        *node_ctx->chainman, authorization_time,
                        broadcast_user_psbt, error)) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_CONFLICT" : error);
                }
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, final_ref, broadcast_presence, error,
                        service_txindex_mode)) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_PREFLIGHT_FAILED" : error);
                }
                if (!ValidateFinalAlternativeRecoveryAgainstChainstate(
                        final_transaction,
                        broadcast_recovery.expected_wtxid,
                        broadcast_trusted, broadcast_parameters, Params(),
                        *node_ctx->chainman, authorization_time,
                        broadcast_presence != FinalTransactionPresence::NONE,
                        error)) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_CONFLICT" : error);
                }
                recovery = std::move(broadcast_recovery);
                commit = std::move(broadcast_commit);
                bool already_confirmed{false};
                std::string broadcast_error;
                const bool broadcast = InsertAndBroadcastPaymasterTransaction(
                    *wallet, final_ref, already_confirmed, broadcast_error);
                if (!broadcast) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(broadcast_error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        broadcast_error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_PROVIDER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("processed", true);
                result.pushKV("queued", queue_result(recovery));
                result.pushKV("message_type", "recovery_submit");
                result.pushKV("request_id", recovery.request_id);
                result.pushKV("session_id", recovery.session_id.GetHex());
                result.pushKV("provider_id",
                              recovery.recovery_provider_id.GetHex());
                result.pushKV("commit_key", commit.commit_key.GetHex());
                result.pushKV("txid", commit.final_txid.GetHex());
                result.pushKV("result_status", "final_committed");
                result.pushKV("broadcast", broadcast);
                if (!broadcast_error.empty()) {
                    result.pushKV("broadcast_error", broadcast_error);
                }
                result.pushKV("recovery",
                              AlternativeRecoveryToJSON(recovery));
                return result;
            }
            const auto submit_lease =
                context.paymaster->LeaseSubmits(identity.provider_id, 1);
            const auto& messages = submit_lease.Messages();
            UniValue result{UniValue::VOBJ};
            result.pushKV("processed", !messages.empty());
            result.pushKV("queued", false);
            if (messages.empty()) return result;
            const DirectMessage& direct = messages.front();
            ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                      direct};
            const auto* submit = std::get_if<PaymasterSubmit>(&direct.payload);
            if (!submit) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "PAYMASTER_SUBMIT_QUEUE_CORRUPT");
            }
            if (!ValidateSubmitEnvelope(*submit, Params().GenesisBlock().GetHash(), error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            PaymasterStore store{*wallet};
            ProviderAttempt attempt;
            if (!store.GetAttemptByTemplateCommitment(submit->template_commitment, attempt) ||
                attempt.provider_id != submit->provider_id || attempt.session_id != submit->session_id ||
                attempt.quote_id != submit->quote_id || attempt.commit_key != submit->commit_key) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SUBMIT_BINDING_MISMATCH");
            }
            PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session) ||
                session.request_id != submit->request_id) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SUBMIT_SESSION_MISMATCH");
            }

            PartiallySignedTransaction submitted_psbt;
            CollaborativePSBTTemplate trusted;
            if (!DecodeRawPSBT(submitted_psbt, MakeByteSpan(submit->user_psbt), error) ||
                !submitted_psbt.tx || !LoadTrustedTemplate(attempt, trusted, error) ||
                !ValidateCollaborativePSBT(submitted_psbt, trusted,
                                           CollaborativeSignatureStage::USER_SIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            PaymasterResult provider_result;
            bool rejected{false};
            if (!RejectUnavailableTemplateInputs(*wallet, store, attempt, *submitted_psbt.tx,
                                                 GetTime(), rejected, provider_result, error)) {
                throw JSONRPCError(
                    error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                    error);
            }

            UniValue committed;
            if (!rejected) {
                JSONRPCRequest nested{request};
                nested.params = UniValue{UniValue::VARR};
                nested.params.push_back(EncodeBase64(submit->user_psbt));
                committed = submitpaymasterdigidollar().HandleRequest(nested);
                if (!store.GetProviderResult(submit->commit_key, provider_result) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_RESULT_NOT_DURABLE");
                }
            }
            PaymasterResultMessage message;
            message.request_id = submit->request_id;
            message.session_id = submit->session_id;
            message.result = provider_result;
            CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
            stream << message;
            const auto bytes = MakeUCharSpan(stream);
            const std::vector<unsigned char> encoded{bytes.begin(), bytes.end()};
            const bool queued = context.paymaster->QueueOutboundDirectMessage(
                messages.front().peer_id, Hash(encoded), encoded.size(), DirectPayload{message}, GetTime());
            node::NodeContext* node = wallet->chain().context();
            if (queued && node && node->connman) node->connman->WakeMessageHandler();
            result.pushKV("queued", queued);
            result.pushKV("request_id", message.request_id);
            result.pushKV("session_id", message.session_id.GetHex());
            result.pushKV("provider_id", provider_result.provider_id.GetHex());
            result.pushKV("commit_key", provider_result.commit_key.GetHex());
            if (provider_result.txid) result.pushKV("txid", provider_result.txid->GetHex());
            result.pushKV("result_status", ResultStatusName(provider_result.status));
            result.pushKV("attempt_state", std::string{AttemptStateName(attempt.state)});
            if (rejected) {
                result.pushKV("rejection_code", "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE");
            } else {
                result.pushKV("commit", committed);
            }
            return result;
        },
    };
}

RPCHelpMan processpaymasterrequests()
{
    return RPCHelpMan{
        "processpaymasterrequests",
        "Process at most one addressed PMCAPREQ or PMQUOTEREQ for this running provider wallet. "
        "Capacity is proven before any payment intent is accepted. New proofs and quotes require "
        "an unlocked wallet; exact durable retries do not.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider quote processing result", {
                                                                                    {RPCResult::Type::BOOL, "processed", "Whether a request was available"},
                                                                                    {RPCResult::Type::BOOL, "queued", "Whether the signed response was queued"},
                                                                                    {RPCResult::Type::BOOL, "rejected", /*optional=*/true, "Whether a permanently invalid inbound continuation was discarded"},
                                                                                    {RPCResult::Type::STR, "rejection_reason", /*optional=*/true, "Stable reason for discarding a permanently invalid continuation"},
                                                                                    {RPCResult::Type::STR, "message_type", /*optional=*/true, "capacity, recovery_request, or quote"},
                                                                                    {RPCResult::Type::STR_HEX, "client_nonce", /*optional=*/true, "Capacity-bound client nonce"},
                                                                                    {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Signed capacity snapshot"},
                                                                                    {RPCResult::Type::STR, "request_id", /*optional=*/true, "Canonical request UUID"},
                                                                                    {RPCResult::Type::STR_HEX, "session_id", /*optional=*/true, "Client session"},
                                                                                    {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                                    {RPCResult::Type::STR_HEX, "attempt_id", /*optional=*/true, "Durable provider attempt"},
                                                                                    {RPCResult::Type::STR_HEX, "recovery_id", /*optional=*/true, "Stable alternative-recovery identifier"},
                                                                                    {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Signed quote"},
                                                                                    {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Exact template"},
                                                                                }},
        RPCExamples{HelpExampleCli("processpaymasterrequests", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const bool automatic_service =
                request.strMethod == INTERNAL_PAYMASTER_SERVICE_METHOD;
            if (automatic_service) {
                if (!AutomaticProviderStateIsSynchronized(*wallet)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_SYNCING");
                }
            } else {
                wallet->BlockUntilSyncedToCurrentChain();
            }
            const ProviderReadiness readiness =
                GetProviderReadiness(*wallet, context, !automatic_service);
            if (automatic_service &&
                std::find(readiness.errors.begin(), readiness.errors.end(),
                          "PAYMASTER_REQUIRES_READY_TXINDEX") !=
                    readiness.errors.end()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SYNCING");
            }
            if (!context.paymaster || !readiness.have_identity ||
                !context.paymaster->IsProviderRunning(wallet->GetName(), readiness.identity.provider_id)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_NOT_RUNNING");
            }
            if (readiness.settings.operation_mode ==
                    ProviderOperationMode::AUTOMATIC &&
                !automatic_service) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE");
            }
            ProviderWorkGuard work_guard{*context.paymaster,
                                         wallet->GetName(),
                                         readiness.identity.provider_id};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SERVICE_BUSY");
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("queued", false);
            const int64_t now = GetTime();
            std::string error;
            node::NodeContext* node = wallet->chain().context();
            if (!node || !node->chainman) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
            }

            // A reserved operational slot intentionally makes staged pool
            // readiness false for *new* clients. The same automatic service
            // must nevertheless continue the already capacity-bound quote
            // request behind it. Skip fresh capacity work while liquidity is
            // unavailable, then fall through to recovery/quote continuations.
            if (!automatic_service || readiness.ready) {
                const auto capacity_lease =
                    context.paymaster->LeaseCapacityRequests(
                        readiness.identity.provider_id, 1);
                const auto& capacity_messages = capacity_lease.Messages();
                if (!capacity_messages.empty()) {
                    result.pushKV("processed", true);
                    result.pushKV("message_type", "capacity");
                    const DirectMessage& direct = capacity_messages.front();
                    ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                              direct};
                    const auto* capacity_request = std::get_if<PaymasterCapacityRequest>(
                        &direct.payload);
                    if (!capacity_request || !ValidateCapacityRequestEnvelope(
                                                 *capacity_request,
                                                 Params().GenesisBlock().GetHash(),
                                                 now, error)) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            error.empty() ? "PAYMASTER_INVALID_CAPACITY_REQUEST" : error);
                    }
                    if (wallet->IsLocked()) {
                        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                           "Provider wallet unlock is required for a capacity proof");
                    }
                    bool have_stored_response{false};
                    if (!readiness.ready && CapacityContinuationMayProceed(readiness)) {
                        // An exact retry can arrive after its own proof reserved
                        // the last slot. Do not mistake it for fresh admission.
                        LOCK(wallet->cs_wallet);
                        WalletBatch batch{wallet->GetDatabase()};
                        std::vector<unsigned char> stored_response;
                        have_stored_response = batch.ReadPaymasterCapacityResponse(
                            Hash(SerializeCapacityRequest(*capacity_request)), stored_response);
                    }
                    if (!CapacityRequestMayProceed(readiness, have_stored_response)) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_PROVIDER_NOT_RUNNING");
                    }
                    uint256 reference_block;
                    int tip_height{-1};
                    {
                        LOCK(cs_main);
                        const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
                        if (tip) {
                            reference_block = tip->GetBlockHash();
                            tip_height = tip->nHeight;
                        }
                    }
                    if (reference_block.IsNull() || tip_height < 0) {
                        throw JSONRPCError(RPC_MISC_ERROR,
                                           "PAYMASTER_CAPACITY_CHAINSTATE_UNAVAILABLE");
                    }
                    if (capacity_messages.front().canonical_netgroup.empty()) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
                    }
                    PaymasterCapacityProof proof;
                    std::vector<ProviderPoolEntry> reserved;
                    if (!ReserveAndBuildPaymasterCapacityProof(
                            *wallet, readiness.identity, *capacity_request,
                            Params().GenesisBlock().GetHash(), reference_block,
                            tip_height, /*minimum_confirmations=*/1,
                            capacity_messages.front().canonical_netgroup, now,
                            proof, reserved, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    const std::vector<unsigned char> proof_bytes =
                        SerializeCapacityProof(proof);
                    const uint256 message_id = Hash(proof_bytes);
                    const bool queued = context.paymaster->QueueCapacityProof(
                        capacity_messages.front().peer_id, message_id,
                        proof_bytes.size(), proof, now);
                    if (queued && node->connman) node->connman->WakeMessageHandler();
                    result.pushKV("queued", queued);
                    result.pushKV("provider_id", proof.provider_id.GetHex());
                    result.pushKV("client_nonce", proof.client_nonce.GetHex());
                    result.pushKV("capacity_snapshot_id", proof.snapshot_id.GetHex());
                    return result;
                }
            }

            const auto recovery_lease =
                context.paymaster->LeaseRecoveryRequests(
                    readiness.identity.provider_id, 1);
            const auto& recovery_messages = recovery_lease.Messages();
            if (!recovery_messages.empty()) {
                result.pushKV("processed", true);
                result.pushKV("message_type", "recovery_request");
                const DirectMessage& direct = recovery_messages.front();
                ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                          direct};
                const auto* recovery_request =
                    std::get_if<AlternativeRecoveryRequest>(&direct.payload);
                if (!recovery_request ||
                    !ValidateAlternativeRecoveryRequestEnvelope(
                        *recovery_request, Params().GenesisBlock().GetHash(),
                        now, error)) {
                    if (!AcknowledgeRejectedDirectMessage(
                            *context.paymaster, direct, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_INVALID_RECOVERY_REQUEST" : error);
                }
                if (direct.canonical_netgroup.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
                }
                if (recovery_request->privacy_profile == PrivacyProfile::HIGH) {
                    bool onion_peer{false};
                    if (node->connman) {
                        node->connman->ForEachNode([&](CNode* peer) {
                            if (peer->GetId() == direct.peer_id &&
                                peer->addr.IsTor()) {
                                onion_peer = true;
                            }
                        });
                    }
                    if (!onion_peer) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RECOVERY_HIGH_PRIVACY_REQUIRES_ONION");
                    }
                }

                PaymasterStore store{*wallet};
                uint256 netgroup_bucket;
                const uint256 recovery_id = GetAlternativeRecoveryId(
                    recovery_request->request_id,
                    recovery_request->session_id,
                    recovery_request->recovery_provider_id,
                    recovery_request->client_nonce);
                AlternativeRecoveryRecord persisted;
                if (store.GetAlternativeRecoveryById(recovery_id, persisted)) {
                    if (!persisted.provider_side || persisted.expired ||
                        persisted.recovery_request_hash !=
                            GetAlternativeRecoveryRequestHash(*recovery_request) ||
                        SerializeRecoveryMessage(persisted.recovery_request) !=
                            SerializeRecoveryMessage(*recovery_request) ||
                        persisted.phase <
                            AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT");
                    }
                    const std::vector<unsigned char> encoded =
                        SerializeRecoveryMessage(persisted.recovery_response);
                    const bool queued =
                        context.paymaster->HasOutboundDirectMessage(
                            direct.peer_id, Hash(encoded)) ||
                        context.paymaster->QueueOutboundDirectMessage(
                            direct.peer_id, Hash(encoded), encoded.size(),
                            DirectPayload{persisted.recovery_response}, now);
                    if (queued && node->connman) {
                        node->connman->WakeMessageHandler();
                    }
                    result.pushKV("queued", queued);
                    result.pushKV("request_id", persisted.request_id);
                    result.pushKV("session_id",
                                  persisted.session_id.GetHex());
                    result.pushKV("provider_id",
                                  persisted.recovery_provider_id.GetHex());
                    result.pushKV("recovery_id",
                                  persisted.recovery_id.GetHex());
                    result.pushKV(
                        "template_commitment",
                        persisted.recovery_response.manifest
                            .template_commitment.GetHex());
                    return result;
                }

                if (wallet->IsLocked()) {
                    throw JSONRPCError(
                        RPC_WALLET_UNLOCK_NEEDED,
                        "Provider wallet unlock is required for a recovery quote");
                }
                const bool continuation_ready =
                    CapacityContinuationMayProceed(readiness);
                if (!continuation_ready ||
                    !ProviderTxIndexIsReady(*wallet, !automatic_service)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        !continuation_ready ? "PAYMASTER_PROVIDER_DRAIN_ONLY" : "PAYMASTER_REQUIRES_READY_TXINDEX");
                }
                HashWriter offer_hasher =
                    TaggedHash("DigiByte Paymaster Offer v1");
                offer_hasher << readiness.identity.provider_id
                             << recovery_request->policy_hash
                             << static_cast<uint8_t>(FundingModel::USER_PAID);
                const uint256 expected_offer_id = offer_hasher.GetSHA256();
                if (recovery_request->policy_hash !=
                        GetProviderPolicyHash(readiness.policy) ||
                    recovery_request->offer_id !=
                        expected_offer_id ||
                    !PolicyAllowsFundingModel(readiness.policy,
                                              FundingModel::USER_PAID)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "PAYMASTER_RECOVERY_POLICY_MISMATCH");
                }

                PaymasterCapacityProof capacity_proof;
                if (!store.GetProviderCapacityProof(
                        recovery_request->capacity_request, now,
                        capacity_proof, error) ||
                    capacity_proof.snapshot_id !=
                        recovery_request->capacity_snapshot_id ||
                    GetCapacityResourceCommitment(capacity_proof) !=
                        recovery_request->capacity_resource_commitment ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof,
                        recovery_request->capacity_request,
                        readiness.identity.identity_key, *node->chainman,
                        now, error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_RECOVERY_CAPACITY_BINDING_MISMATCH" : error);
                }

                std::vector<CollaborativeInput> user_inputs;
                for (const COutPoint& outpoint :
                     recovery_request->user_dd_inputs) {
                    uint256 block_hash;
                    CTransactionRef creating_tx;
                    if (!g_txindex->FindTx(outpoint.hash, block_hash,
                                           creating_tx) ||
                        !creating_tx) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RECOVERY_USER_PREVOUT_NOT_FOUND");
                    }
                    user_inputs.push_back(
                        {outpoint, creating_tx, InputRole::USER_DD});
                }
                ValidatedUserDDInputs validated_user_inputs;
                {
                    LOCK(cs_main);
                    if (!ValidateCollaborativeUserDDInputs(
                            user_inputs,
                            node->chainman->ActiveChainstate().CoinsTip(),
                            validated_user_inputs, error)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                    }
                }
                if (!ValidateAlternativeRecoveryRequestInputs(
                        *recovery_request, validated_user_inputs.output_keys,
                        error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                const CAmount total_dd{
                    validated_user_inputs.total_dd_amount};
                CAmount returned_dd{0};
                for (const AlternativeRecoveryReturn& output :
                     recovery_request->wallet_returns) {
                    if (output.amount.value >
                        std::numeric_limits<CAmount>::max() - returned_dd) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RECOVERY_AMOUNT_OVERFLOW");
                    }
                    returned_dd += output.amount.value;
                }
                const auto fee_plan = EvaluateServiceFee(
                    readiness.policy, FundingModel::USER_PAID,
                    DDCents{total_dd}, error);
                if (!fee_plan ||
                    !(fee_plan->service_fee ==
                      recovery_request->service_fee) ||
                    total_dd - returned_dd !=
                        recovery_request->service_fee.value) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_RECOVERY_SERVICE_FEE_MISMATCH" : error);
                }

                ValidatedCapacitySnapshot snapshot;
                snapshot.snapshot_id = capacity_proof.snapshot_id;
                snapshot.resource_commitment =
                    GetCapacityResourceCommitment(capacity_proof);
                snapshot.session_id = recovery_request->session_id;
                snapshot.attempt_id = recovery_id;
                snapshot.provider_id = capacity_proof.provider_id;
                snapshot.client_nonce = capacity_proof.client_nonce;
                snapshot.request_hash = Hash(
                    SerializeCapacityRequest(
                        recovery_request->capacity_request));
                snapshot.capacity_proof =
                    SerializeCapacityProof(capacity_proof);
                snapshot.created_at = capacity_proof.created_at;
                snapshot.expires_at = capacity_proof.expires_at;
                snapshot.validated_at = now;
                snapshot.funding_model = capacity_proof.funding_model;
                snapshot.requires_carrier = capacity_proof.requires_carrier;

                AlternativeRecoveryParameters parameters;
                parameters.genesis_hash = recovery_request->genesis_hash;
                parameters.request_id = recovery_request->request_id;
                parameters.session_id = recovery_request->session_id;
                parameters.original_provider_id =
                    recovery_request->original_provider_id;
                parameters.recovery_provider_id =
                    recovery_request->recovery_provider_id;
                parameters.privacy_profile =
                    recovery_request->privacy_profile;
                parameters.offer_id = recovery_request->offer_id;
                parameters.policy_hash = recovery_request->policy_hash;
                parameters.original_commit_key =
                    recovery_request->original_commit_key;
                parameters.original_template_commitment =
                    recovery_request->original_template_commitment;
                parameters.capacity_request =
                    recovery_request->capacity_request;
                parameters.capacity_snapshot = snapshot;
                parameters.recovery_provider_identity_key =
                    readiness.identity.identity_key;
                parameters.persisted_user_dd_inputs =
                    recovery_request->user_dd_inputs;
                parameters.user_dd_inputs = std::move(user_inputs);
                parameters.wallet_returns =
                    recovery_request->wallet_returns;
                for (const AlternativeRecoveryReturn& output :
                     parameters.wallet_returns) {
                    parameters.wallet_verified_fresh_return_scripts.push_back(
                        output.script_pub_key);
                }
                const PaymasterLiquiditySlot& slot =
                    capacity_proof.liquidity_slots.front();
                const CAmount network_fee{COIN / 10};
                CAmount provider_dgb{0};
                for (const CapacityDGBInput& input : slot.dgb_inputs) {
                    if (input.input.value.value >
                        std::numeric_limits<CAmount>::max() - provider_dgb) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_RECOVERY_AMOUNT_OVERFLOW");
                    }
                    provider_dgb += input.input.value.value;
                }
                if (network_fee > readiness.policy.maximum_network_fee.value ||
                    provider_dgb < network_fee) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW");
                }
                if (!store.CheckProviderRecoveryAdmission(
                        *recovery_request,
                        GetAlternativeRecoveryRequestHash(*recovery_request),
                        direct.canonical_netgroup, slot.carrier.has_value(),
                        DGBSatoshis{network_fee}, now, netgroup_bucket,
                        error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : error);
                }
                const bool need_dd_script =
                    slot.carrier.has_value() ||
                    recovery_request->service_fee.value > 0;
                if (need_dd_script) {
                    const auto destination = wallet->GetNewDestination(
                        OutputType::BECH32M,
                        "Paymaster recovery DD return");
                    if (!destination) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_RECOVERY_CHANGE_UNAVAILABLE");
                    }
                    parameters.recovery_provider_dd_script =
                        GetScriptForDestination(*destination);
                }
                if (provider_dgb > network_fee) {
                    const auto destination = wallet->GetNewDestination(
                        OutputType::BECH32M,
                        "Paymaster recovery DGB change");
                    if (!destination) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_RECOVERY_CHANGE_UNAVAILABLE");
                    }
                    parameters.recovery_provider_dgb_change_script =
                        GetScriptForDestination(*destination);
                }
                parameters.maximum_service_fee =
                    recovery_request->maximum_service_fee;
                parameters.service_fee = recovery_request->service_fee;
                parameters.network_fee = DGBSatoshis{network_fee};
                parameters.fee_rate = 1;
                parameters.expires_at = std::min(
                    recovery_request->expires_at,
                    SaturatingAddSeconds(now, readiness.policy.quote_ttl));

                AlternativeRecoveryTemplate built;
                if (!BuildAlternativeRecoveryTemplateAgainstChainstate(
                        parameters, Params(), *node->chainman, now, built,
                        error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                AlternativeRecoveryResponse response;
                response.genesis_hash = recovery_request->genesis_hash;
                response.request_id = recovery_request->request_id;
                response.session_id = recovery_request->session_id;
                response.recovery_id = recovery_id;
                response.recovery_provider_id =
                    recovery_request->recovery_provider_id;
                response.recovery_request_hash =
                    GetAlternativeRecoveryRequestHash(*recovery_request);
                response.manifest = built.manifest;
                response.unsigned_psbt =
                    SerializePSBT(built.trusted_template.psbt);
                response.created_at = now;
                response.expires_at = parameters.expires_at;
                response.recovery_commit_key =
                    GetAlternativeRecoveryCommitKey(response);
                CKey identity_key;
                ProviderIdentityRecord identity;
                response.identity_signature.resize(64);
                if (!GetPaymasterIdentityKey(*wallet, identity_key, identity,
                                             error) ||
                    identity.provider_id !=
                        response.recovery_provider_id ||
                    !identity_key.SignSchnorr(
                        GetAlternativeRecoveryResponseSignatureHash(response),
                        response.identity_signature, nullptr, GetRandHash())) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_RECOVERY_RESPONSE_SIGNING_FAILED" : error);
                }
                if (!ValidateAlternativeRecoveryResponse(
                        response, *recovery_request, identity.identity_key,
                        now, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }

                AlternativeRecoveryRecord recovery;
                recovery.provider_side = true;
                recovery.request_id = recovery_request->request_id;
                recovery.session_id = recovery_request->session_id;
                recovery.recovery_id = recovery_id;
                recovery.recovery_request_hash =
                    response.recovery_request_hash;
                recovery.original_provider_id =
                    recovery_request->original_provider_id;
                recovery.recovery_provider_id =
                    recovery_request->recovery_provider_id;
                recovery.privacy_profile =
                    recovery_request->privacy_profile;
                recovery.offer_id = recovery_request->offer_id;
                recovery.policy_hash = recovery_request->policy_hash;
                recovery.recovery_provider_identity_key =
                    identity.identity_key;
                recovery.recovery_provider_endpoint =
                    readiness.endpoint.ToStringAddrPort();
                recovery.original_commit_key =
                    recovery_request->original_commit_key;
                recovery.original_template_commitment =
                    recovery_request->original_template_commitment;
                recovery.selected_maximum_service_fee =
                    recovery_request->maximum_service_fee;
                recovery.selected_service_fee =
                    recovery_request->service_fee;
                recovery.client_nonce = recovery_request->client_nonce;
                recovery.capacity_request =
                    recovery_request->capacity_request;
                recovery.capacity_snapshot = snapshot;
                recovery.recovery_request = *recovery_request;
                recovery.recovery_response = response;
                recovery.provider_safety_policy_hash =
                    GetProviderSafetyPolicyHash(readiness.safety_policy);
                recovery.provider_budget_reservation_id =
                    response.recovery_commit_key;
                recovery.provider_netgroup_bucket = netgroup_bucket;
                recovery.provider_maximum_network_fee =
                    response.manifest.network_fee;
                if (!BuildRecoveryAuthorizationManifest(
                        *recovery_request, response,
                        recovery.recovery_authorization, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                recovery.phase =
                    AlternativeRecoveryPhase::RESPONSE_VALIDATED;
                recovery.created_at = recovery.updated_at = now;
                if (!store.CommitProviderAlternativeRecoveryQuote(
                        recovery, netgroup_bucket,
                        Params().GenesisBlock().GetHash(), now, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                const std::vector<unsigned char> encoded =
                    SerializeRecoveryMessage(response);
                const bool queued =
                    context.paymaster->QueueOutboundDirectMessage(
                        direct.peer_id, Hash(encoded), encoded.size(),
                        DirectPayload{response}, now);
                if (queued && node->connman) {
                    node->connman->WakeMessageHandler();
                }
                result.pushKV("queued", queued);
                result.pushKV("request_id", response.request_id);
                result.pushKV("session_id", response.session_id.GetHex());
                result.pushKV("provider_id",
                              response.recovery_provider_id.GetHex());
                result.pushKV("recovery_id", recovery_id.GetHex());
                result.pushKV("template_commitment",
                              response.manifest.template_commitment.GetHex());
                return result;
            }

            const auto quote_lease = context.paymaster->LeaseQuoteRequests(
                readiness.identity.provider_id, 1);
            const auto& messages = quote_lease.Messages();
            result.pushKV("processed", !messages.empty());
            if (messages.empty()) return result;
            result.pushKV("message_type", "quote");
            const DirectMessage& direct = messages.front();
            ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                      direct};
            const auto* quote_request =
                std::get_if<PaymasterQuoteRequest>(&direct.payload);
            if (!quote_request) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "PAYMASTER_REQUEST_QUEUE_CORRUPT");
            }

            if (!ValidateQuoteRequestEnvelope(*quote_request, Params().GenesisBlock().GetHash(),
                                              now, error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            std::optional<SponsorshipAuthorizationRecord> sponsorship;
            const bool restricted = quote_request->intent.funding_model == FundingModel::SPONSORED &&
                                    quote_request->intent.sponsorship_scope ==
                                        SponsorshipScope::RESTRICTED;
            const auto validate_restricted_request = [&] {
                if (!restricted) return;
                const auto& descriptor = *quote_request->restricted_descriptor;
                const auto& capability = *quote_request->restricted_capability;
                if (readiness.policy.sponsorship_scope != SponsorshipScope::RESTRICTED ||
                    readiness.policy.funding_models != FUNDING_MODEL_SPONSORED ||
                    descriptor.policy_hash != GetProviderPolicyHash(readiness.policy) ||
                    !ValidateRestrictedServiceDescriptor(descriptor, readiness.identity.identity_key,
                                                         Params().GenesisBlock().GetHash(), now, error,
                                                         Params().GetChainType() == ChainType::REGTEST) ||
                    !ValidateSponsorshipCapability(capability, descriptor,
                                                   quote_request->intent.recipient_script,
                                                   quote_request->intent.recipient_amount,
                                                   quote_request->intent.client_nonce,
                                                   Params().GenesisBlock().GetHash(), now, error) ||
                    !ValidateRestrictedAdmissionProofs(descriptor, readiness.identity.identity_key,
                                                       *node->chainman, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       error.empty() ? "PAYMASTER_RESTRICTED_POLICY_MISMATCH" : error);
                }
            };

            PaymasterStore store{*wallet};
            const std::vector<unsigned char> redacted_request =
                SerializeRedactedQuoteRequest(*quote_request);
            PaymentSession persisted_session;
            if (store.GetSessionByRequestId(quote_request->intent.request_id,
                                            persisted_session)) {
                ProviderAttempt persisted_attempt;
                const uint256 canonical_request_hash = Hash(redacted_request);
                if (!persisted_session.provider_side ||
                    persisted_session.session_id != quote_request->intent.session_id ||
                    persisted_session.canonical_request_hash != canonical_request_hash ||
                    persisted_session.attempt_ids.size() != 1 ||
                    !store.GetAttempt(persisted_session.attempt_ids.front(), persisted_attempt) ||
                    persisted_attempt.session_id != quote_request->intent.session_id ||
                    persisted_attempt.provider_id != readiness.identity.provider_id ||
                    persisted_attempt.client_nonce != quote_request->intent.client_nonce ||
                    persisted_attempt.intent_hash != GetPaymentIntentHash(quote_request->intent) ||
                    persisted_attempt.quote_request != redacted_request ||
                    persisted_attempt.signed_quote.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REQUEST_ID_CONFLICT");
                }
                if (persisted_attempt.quote_expires_at <= now ||
                    persisted_attempt.retry_until < now) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_RETRY_EXPIRED");
                }
                validate_restricted_request();

                PaymasterQuoteResponse response;
                try {
                    SpanReader stream{::PROTOCOL_VERSION, persisted_attempt.signed_quote};
                    stream >> response;
                    if (!stream.empty() || SerializeQuoteResponse(response) !=
                                               persisted_attempt.signed_quote) {
                        throw std::ios_base::failure("non-canonical persisted quote");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
                }
                if (response.request_id != quote_request->intent.request_id ||
                    response.session_id != quote_request->intent.session_id ||
                    response.quote.provider_id != readiness.identity.provider_id ||
                    response.quote.quote_id != persisted_attempt.quote_id ||
                    response.quote.template_commitment !=
                        persisted_attempt.template_commitment) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
                }

                const uint256 message_id = Hash(persisted_attempt.signed_quote);
                const bool queued = context.paymaster->QueueOutboundDirectMessage(
                    messages.front().peer_id, message_id,
                    persisted_attempt.signed_quote.size(), DirectPayload{response}, now);
                if (queued && node->connman) node->connman->WakeMessageHandler();
                result.pushKV("queued", queued);
                result.pushKV("request_id", response.request_id);
                result.pushKV("session_id", response.session_id.GetHex());
                result.pushKV("provider_id", response.quote.provider_id.GetHex());
                result.pushKV("attempt_id", persisted_attempt.attempt_id.GetHex());
                result.pushKV("quote_id", response.quote.quote_id.GetHex());
                result.pushKV("template_commitment",
                              response.quote.template_commitment.GetHex());
                return result;
            }

            if (messages.front().canonical_netgroup.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
            }
            if (!CapacityContinuationMayProceed(readiness)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_DRAIN_ONLY");
            }
            uint256 provider_netgroup_bucket;

            if (wallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                   "Provider wallet unlock is required for a new quote");
            }
            if (!ProviderTxIndexIsReady(*wallet, !automatic_service)) {
                throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_REQUIRES_READY_TXINDEX");
            }
            validate_restricted_request();

            std::vector<CollaborativeInput> user_inputs;
            for (const COutPoint& outpoint : quote_request->intent.user_dd_inputs) {
                uint256 block_hash;
                CTransactionRef creating_tx;
                if (!g_txindex->FindTx(outpoint.hash, block_hash, creating_tx) || !creating_tx) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PREVOUT_NOT_FOUND");
                }
                user_inputs.push_back({outpoint, creating_tx, InputRole::USER_DD});
            }
            ProviderQuotePreflightResult quote_preflight;
            {
                LOCK(cs_main);
                if (!PreflightProviderQuoteRequest(
                        *quote_request, readiness.policy, user_inputs, Params(),
                        node->chainman->ActiveChainstate().CoinsTip(), now,
                        quote_preflight, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
            }
            const ServiceFeePlan* const fee_plan{&quote_preflight.fee_plan};
            const bool needs_carrier = fee_plan->output_kind == FeeOutputKind::CARRIER_SUCCESSOR;
            std::vector<ProviderPoolEntry> available;
            if (!GetPaymasterProviderPoolEntries(*wallet, available)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOLS_NOT_PREPARED");
            }
            std::sort(available.begin(), available.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.outpoint < rhs.outpoint;
            });
            const auto usable = [&](const ProviderPoolEntry& entry, PoolAsset asset) {
                return entry.purpose == PoolPurpose::OPERATIONAL && entry.asset == asset &&
                       entry.state == PoolEntryState::RESERVED &&
                       entry.reservation_id == quote_request->intent.client_nonce &&
                       entry.confirmation_height > 0;
            };
            const auto dgb_it = std::find_if(available.begin(), available.end(), [&](const auto& entry) {
                return usable(entry, PoolAsset::DGB) &&
                       entry.dgb_value.value >= readiness.policy.maximum_network_fee.value;
            });
            const auto carrier_it = std::find_if(available.begin(), available.end(), [&](const auto& entry) {
                return usable(entry, PoolAsset::DD_CARRIER);
            });
            if (dgb_it == available.end() || (needs_carrier && carrier_it == available.end())) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_OPERATIONAL_SLOT_MISSING");
            }
            const CAmount network_fee{COIN / 10};
            if (readiness.policy.maximum_network_fee.value < network_fee ||
                dgb_it->dgb_value.value < network_fee) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW");
            }
            if (!store.CheckProviderQuoteAdmission(
                    *quote_request, Hash(redacted_request),
                    messages.front().canonical_netgroup, needs_carrier,
                    DGBSatoshis{network_fee}, now,
                    provider_netgroup_bucket, error)) {
                if (IsPermanentCapacityContinuationError(error)) {
                    // The exact quote can no longer be authorized because its
                    // preceding Capacity reservation is absent, expired, or
                    // conflicts with the persisted proof. Consume only this
                    // invalid inbox entry and leave the provider's maintenance
                    // state intact; a remote peer must not be able to turn a
                    // safely rejected continuation into a provider fault.
                    const std::string rejection_reason{error};
                    if (!AcknowledgeRejectedDirectMessage(
                            *context.paymaster, direct, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    result.pushKV("rejected", true);
                    result.pushKV("rejection_reason", rejection_reason);
                    return result;
                }
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    error.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : error);
            }
            CTransactionRef dgb_creating_tx;
            CTransactionRef carrier_creating_tx;
            {
                LOCK(wallet->cs_wallet);
                const CWalletTx* dgb_wallet_tx = wallet->GetWalletTx(dgb_it->outpoint.hash);
                if (dgb_wallet_tx) dgb_creating_tx = dgb_wallet_tx->tx;
                if (needs_carrier) {
                    const CWalletTx* carrier_wallet_tx = wallet->GetWalletTx(carrier_it->outpoint.hash);
                    if (carrier_wallet_tx) carrier_creating_tx = carrier_wallet_tx->tx;
                }
            }
            if (!dgb_creating_tx || (needs_carrier && !carrier_creating_tx)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_PREVOUT_NOT_FOUND");
            }

            ProviderQuoteBuildParameters parameters;
            do {
                parameters.quote_id = GetRandHash();
            } while (parameters.quote_id.IsNull());
            parameters.user_inputs = std::move(user_inputs);
            parameters.reserved_dgb_inputs.push_back(
                {dgb_it->outpoint, CMutableTransaction{*dgb_creating_tx}, dgb_it->dgb_value});
            if (needs_carrier) {
                parameters.reserved_carrier = VerifiedDDCarrier{
                    carrier_it->outpoint, CMutableTransaction{*carrier_creating_tx},
                    carrier_it->carrier_value};
                const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                   "Paymaster carrier successor");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                parameters.carrier_return_script = GetScriptForDestination(*destination);
            } else if (fee_plan->output_kind == FeeOutputKind::PROVIDER_OUTPUT) {
                const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                   "Paymaster service fee");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                parameters.provider_fee_script = GetScriptForDestination(*destination);
            }
            if (dgb_it->dgb_value.value > network_fee) {
                const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                   "Paymaster DGB change");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                parameters.dgb_change_script = GetScriptForDestination(*destination);
            }
            parameters.network_fee = DGBSatoshis{network_fee};
            parameters.created_at = now;
            parameters.expires_at = SaturatingAddSeconds(now, readiness.policy.quote_ttl);
            parameters.retry_until = SaturatingAddSeconds(now, DEFAULT_RETRY_SECONDS);
            ProviderQuoteBuildResult built;
            {
                LOCK(cs_main);
                if (!BuildUnsignedProviderQuote(*quote_request, readiness.policy, parameters,
                                                Params(), node->chainman->ActiveChainstate().CoinsTip(),
                                                built, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
            }
            ProviderAuthorizationManifest provider_manifest;
            if (!BuildProviderAuthorizationManifest(
                    quote_request->intent, built.quote, built.trusted_template,
                    GetProviderSafetyPolicyHash(readiness.safety_policy),
                    provider_manifest, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            if (!ValidateProviderAuthorizationOwnership(
                    *wallet, provider_manifest, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            CKey identity_key;
            ProviderIdentityRecord identity;
            built.quote.identity_signature.resize(64);
            if (!GetPaymasterIdentityKey(*wallet, identity_key, identity, error) ||
                !identity_key.SignSchnorr(GetPaymasterQuoteSignatureHash(built.quote),
                                          built.quote.identity_signature, nullptr,
                                          GetRandHash())) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "PAYMASTER_QUOTE_SIGNING_FAILED" : error);
            }
            PaymasterQuoteResponse response;
            response.request_id = quote_request->intent.request_id;
            response.session_id = quote_request->intent.session_id;
            response.quote = built.quote;
            ProviderAttempt attempt;
            do {
                attempt.attempt_id = GetRandHash();
            } while (attempt.attempt_id.IsNull());
            attempt.session_id = response.session_id;
            attempt.provider_id = response.quote.provider_id;
            attempt.provider_identity_key = identity.identity_key;
            attempt.state = AttemptState::QUOTED;
            attempt.client_nonce = quote_request->intent.client_nonce;
            attempt.intent_hash = response.quote.intent_hash;
            attempt.quote_id = response.quote.quote_id;
            attempt.unsigned_txid = response.quote.unsigned_txid;
            attempt.template_commitment = response.quote.template_commitment;
            attempt.commit_key = GetPaymasterCommitKey(attempt.provider_id, attempt.client_nonce,
                                                       attempt.intent_hash, attempt.quote_id,
                                                       attempt.template_commitment);
            if (restricted) {
                attempt.sponsorship_capability_hash =
                    GetSponsorshipCapabilityHash(*quote_request->restricted_capability);
                SponsorshipAuthorizationRecord authorization;
                authorization.capability_hash = attempt.sponsorship_capability_hash;
                authorization.payment_binding_hash = attempt.intent_hash;
                authorization.reservation_id = attempt.commit_key;
                authorization.reserved_at = now;
                authorization.expires_at = quote_request->restricted_capability->expires_at;
                sponsorship = std::move(authorization);
            }
            attempt.quote_request = redacted_request;
            attempt.signed_quote = SerializeQuoteResponse(response);
            attempt.provider_manifest = std::move(provider_manifest);
            attempt.provider_netgroup_bucket = provider_netgroup_bucket;
            attempt.unsigned_transaction = SerializeTransaction(response.quote.unsigned_transaction);
            attempt.unsigned_psbt = SerializePSBT(built.trusted_template.psbt);
            for (const InputRole role : built.trusted_template.input_roles) {
                attempt.input_roles.push_back(static_cast<ReservationRole>(role));
            }
            attempt.quote_expires_at = response.quote.expires_at;
            attempt.retry_until = response.quote.retry_until;
            attempt.created_at = attempt.updated_at = now;
            if (!store.CommitProviderQuote(
                    attempt, sponsorship, Params().GenesisBlock().GetHash(),
                    now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            const std::vector<unsigned char> response_bytes = SerializeQuoteResponse(response);
            const uint256 message_id = Hash(response_bytes);
            const bool queued = context.paymaster->QueueOutboundDirectMessage(
                messages.front().peer_id, message_id, response_bytes.size(), DirectPayload{response}, now);
            if (queued && node->connman) node->connman->WakeMessageHandler();
            result.pushKV("queued", queued);
            result.pushKV("request_id", response.request_id);
            result.pushKV("session_id", response.session_id.GetHex());
            result.pushKV("provider_id", response.quote.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", response.quote.quote_id.GetHex());
            result.pushKV("template_commitment", response.quote.template_commitment.GetHex());
            return result;
        },
    };
}

} // namespace wallet
