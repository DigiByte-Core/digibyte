// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Client sessions, reservations, Capacity snapshots, and fallback state. */

#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>

#include <chainparams.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <node/context.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/validation.h>
#include <paymaster/wire.h>
#include <random.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/time.h>
#include <version.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_store::internal;

CreatePaymasterSessionResult PaymasterStore::CreateOrJoinSession(
    const std::string& request_id,
    const uint256& canonical_request_hash,
    DigiDollar::Paymaster::FeeMode requested_mode,
    int64_t now,
    DigiDollar::Paymaster::PaymentSession& session,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!IsCanonicalRequestId(request_id) || canonical_request_hash.IsNull()) {
        error = "PAYMASTER_INVALID_REQUEST";
        return CreatePaymasterSessionResult::CONFLICT;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    if (batch.ReadPaymasterSession(request_id, session)) {
        if (session.canonical_request_hash != canonical_request_hash || session.fee_mode_requested != requested_mode) {
            error = "PAYMASTER_REQUEST_ID_CONFLICT";
            return CreatePaymasterSessionResult::CONFLICT;
        }
        return CreatePaymasterSessionResult::JOINED;
    }
    if (batch.HasPaymasterSession(request_id)) {
        error = PersistedVersionError(
            "PaymentSession", session.version,
            PaymentSession::CURRENT_VERSION,
            "PAYMASTER_INVALID_PERSISTED_SESSION");
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }

    IdempotencyTombstone tombstone;
    if (batch.ReadPaymasterTombstone(request_id, tombstone)) {
        if (tombstone.canonical_request_hash != canonical_request_hash) {
            error = "PAYMASTER_REQUEST_ID_CONFLICT";
            return CreatePaymasterSessionResult::CONFLICT;
        }
        session.request_id = tombstone.request_id;
        session.session_id = tombstone.session_id;
        session.canonical_request_hash = tombstone.canonical_request_hash;
        session.fee_mode_requested = tombstone.fee_mode_requested;
        session.fee_mode_used = tombstone.fee_mode_used;
        session.requested_amount = tombstone.requested_amount;
        session.subtract_paymaster_fee_from_amount =
            tombstone.subtract_paymaster_fee_from_amount;
        session.send_all_spendable_dd = tombstone.send_all_spendable_dd;
        session.state = tombstone.final_state;
        if (tombstone.final_state == SessionState::CANCELED_SAFE) {
            session.recovery_txid = tombstone.final_txid;
        } else {
            session.final_txid = tombstone.final_txid;
        }
        return CreatePaymasterSessionResult::FINAL_TOMBSTONE;
    }
    if (batch.HasPaymasterTombstone(request_id)) {
        error = PersistedVersionError(
            "IdempotencyTombstone", tombstone.version,
            IdempotencyTombstone::CURRENT_VERSION,
            "PAYMASTER_INVALID_PERSISTED_TOMBSTONE");
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }

    session = {};
    session.request_id = request_id;
    do {
        session.session_id = GetRandHash();
    } while (session.session_id.IsNull());
    session.canonical_request_hash = canonical_request_hash;
    session.fee_mode_requested = requested_mode;
    session.fee_mode_used = requested_mode;
    session.created_at = now;
    session.updated_at = now;

    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }
    if (!batch.WritePaymasterSession(session, false) ||
        !batch.WritePaymasterSessionId(session.session_id, request_id, false)) {
        Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }
    return CreatePaymasterSessionResult::CREATED;
}

bool PaymasterStore::BindClientPaymentOrder(
    const std::string& request_id,
    DDCents requested_amount,
    bool subtract_paymaster_fee_from_amount,
    bool send_all_spendable_dd,
    std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || requested_amount.value <= 0 ||
        requested_amount.value > MAX_DD_OUTPUT_CENTS ||
        (send_all_spendable_dd && !subtract_paymaster_fee_from_amount)) {
        error = "PAYMASTER_INVALID_CLIENT_PAYMENT_ORDER";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = batch.HasPaymasterSession(request_id)
                    ? PersistedVersionError(
                          "PaymentSession", session.version,
                          PaymentSession::CURRENT_VERSION,
                          "PAYMASTER_INVALID_PERSISTED_SESSION")
                    : "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (session.provider_side) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    const bool already_bound = session.requested_amount.value != 0;
    if (already_bound) {
        if (session.requested_amount != requested_amount ||
            session.subtract_paymaster_fee_from_amount !=
                subtract_paymaster_fee_from_amount ||
            session.send_all_spendable_dd != send_all_spendable_dd) {
            error = "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT";
            return false;
        }
        return true;
    }
    // Current sessions bind the local payment order before any attempt is
    // created. Never reconstruct missing authority after an attempt exists.
    if (!session.attempt_ids.empty()) {
        error = "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT";
        return false;
    }
    session.requested_amount = requested_amount;
    session.subtract_paymaster_fee_from_amount =
        subtract_paymaster_fee_from_amount;
    session.send_all_spendable_dd = send_all_spendable_dd;
    session.updated_at = std::max(session.updated_at, GetTime());
    if (!batch.WritePaymasterSession(session)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool PaymasterStore::GetSessionByRequestId(const std::string& request_id, PaymentSession& session) const
{
    return GetSessionByRequestIdWithStatus(request_id, session) == DatabaseReadStatus::FOUND;
}

DatabaseReadStatus PaymasterStore::GetSessionByRequestIdWithStatus(
    const std::string& request_id, PaymentSession& session) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    const DatabaseReadStatus session_status =
        batch.ReadPaymasterSessionWithStatus(request_id, session);
    if (session_status != DatabaseReadStatus::NOT_FOUND) return session_status;
    IdempotencyTombstone tombstone;
    const auto tombstone_status = batch.ReadPaymasterTombstoneWithStatus(request_id, tombstone);
    if (tombstone_status != DatabaseReadStatus::FOUND) return tombstone_status;
    session = {};
    session.request_id = tombstone.request_id;
    session.session_id = tombstone.session_id;
    session.canonical_request_hash = tombstone.canonical_request_hash;
    session.fee_mode_requested = tombstone.fee_mode_requested;
    session.fee_mode_used = tombstone.fee_mode_used;
    session.requested_amount = tombstone.requested_amount;
    session.subtract_paymaster_fee_from_amount =
        tombstone.subtract_paymaster_fee_from_amount;
    session.send_all_spendable_dd = tombstone.send_all_spendable_dd;
    session.state = tombstone.final_state;
    if (tombstone.final_state == SessionState::CANCELED_SAFE) {
        session.recovery_txid = tombstone.final_txid;
    } else {
        session.final_txid = tombstone.final_txid;
    }
    return DatabaseReadStatus::FOUND;
}

bool PaymasterStore::GetSessionBySessionId(const uint256& session_id, PaymentSession& session) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::string request_id;
    if (batch.ReadPaymasterSessionIdWithStatus(session_id, request_id) !=
        DatabaseReadStatus::FOUND) {
        return false;
    }
    const DatabaseReadStatus session_status =
        batch.ReadPaymasterSessionWithStatus(request_id, session);
    if (session_status == DatabaseReadStatus::FOUND) {
        return session.session_id == session_id;
    }
    // A present-but-unreadable live session must never be reinterpreted as its
    // tombstone. Only a genuinely absent live record permits the fallback.
    if (session_status != DatabaseReadStatus::NOT_FOUND) return false;
    IdempotencyTombstone tombstone;
    if (batch.ReadPaymasterTombstoneWithStatus(request_id, tombstone) !=
            DatabaseReadStatus::FOUND ||
        tombstone.session_id != session_id) {
        return false;
    }
    session = {};
    session.request_id = tombstone.request_id;
    session.session_id = tombstone.session_id;
    session.canonical_request_hash = tombstone.canonical_request_hash;
    session.fee_mode_requested = tombstone.fee_mode_requested;
    session.fee_mode_used = tombstone.fee_mode_used;
    session.requested_amount = tombstone.requested_amount;
    session.subtract_paymaster_fee_from_amount =
        tombstone.subtract_paymaster_fee_from_amount;
    session.send_all_spendable_dd = tombstone.send_all_spendable_dd;
    session.state = tombstone.final_state;
    if (tombstone.final_state == SessionState::CANCELED_SAFE) {
        session.recovery_txid = tombstone.final_txid;
    } else {
        session.final_txid = tombstone.final_txid;
    }
    return true;
}

bool PaymasterStore::ListClientSessions(
    std::vector<PaymentSession>& sessions,
    std::string& error) const
{
    sessions.clear();
    error.clear();
    LOCK(m_wallet.cs_wallet);
    if (!WalletBatch{m_wallet.GetDatabase()}.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_SESSION_DATABASE_READ";
        return false;
    }
    sessions.erase(
        std::remove_if(sessions.begin(), sessions.end(),
                       [](const PaymentSession& session) {
                           return session.provider_side;
                       }),
        sessions.end());
    return true;
}

bool PaymasterStore::ClientSessionHasLiveReservations(
    const PaymentSession& session,
    bool& has_live_reservations,
    std::string& error) const
{
    has_live_reservations = false;
    error.clear();
    if (session.provider_side || !IsCanonicalRequestId(session.request_id) ||
        session.session_id.IsNull()) {
        error = "PAYMASTER_CLIENT_SESSION_REQUIRED";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        const DatabaseReadStatus status =
            batch.ReadPaymasterReservationWithStatus(outpoint, reservation);
        if (status == DatabaseReadStatus::NOT_FOUND) continue;
        if (status != DatabaseReadStatus::FOUND) {
            error = "PAYMASTER_RESERVATION_DATABASE_READ";
            return false;
        }
        if (reservation.request_id != session.request_id ||
            reservation.session_id != session.session_id ||
            reservation.outpoint != outpoint ||
            reservation.role != ReservationRole::USER_DD) {
            error = "PAYMASTER_RESERVATION_SESSION_CONFLICT";
            return false;
        }
        has_live_reservations = true;
    }
    return true;
}

bool PaymasterStore::ReserveInputs(
    const std::string& request_id,
    const std::vector<std::pair<COutPoint, ReservationRole>>& inputs,
    FeeMode used_mode,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (inputs.empty()) {
        error = "PAYMASTER_NO_RESERVED_INPUTS";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }

    std::set<COutPoint> unique;
    std::vector<InputReservation> reservations;
    reservations.reserve(inputs.size());
    std::vector<COutPoint> user_inputs;
    for (const auto& [outpoint, role] : inputs) {
        if (outpoint.IsNull() || !unique.insert(outpoint).second) {
            error = "PAYMASTER_INVALID_RESERVED_INPUT";
            return false;
        }
        InputReservation existing;
        const bool occupied = batch.ReadPaymasterReservation(outpoint, existing);
        const bool ours = occupied && existing.session_id == session.session_id;
        if ((occupied && !ours) || (m_wallet.IsLockedCoin(outpoint) && !ours)) {
            error = "PAYMASTER_INPUT_ALREADY_RESERVED";
            return false;
        }
        reservations.push_back({InputReservation::CURRENT_VERSION, outpoint, request_id, session.session_id, role, false, now});
        if (role == ReservationRole::USER_DD || role == ReservationRole::USER_DGB) user_inputs.push_back(outpoint);
    }

    if (session.state == SessionState::INPUTS_RESERVED) {
        if (session.user_inputs == user_inputs && session.fee_mode_used == used_mode) return true;
        error = "PAYMASTER_SESSION_INPUT_CONFLICT";
        return false;
    }
    if (!CanTransition(session.state, SessionState::INPUTS_RESERVED)) {
        error = "PAYMASTER_INVALID_SESSION_TRANSITION";
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    for (const auto& reservation : reservations) {
        InputReservation existing;
        if (!batch.ReadPaymasterReservation(reservation.outpoint, existing) &&
            !batch.WritePaymasterReservation(reservation, false)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.WriteLockedUTXO(reservation.outpoint)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.user_inputs = std::move(user_inputs);
    session.fee_mode_used = used_mode;
    session.state = SessionState::INPUTS_RESERVED;
    session.updated_at = now;
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    for (const auto& reservation : reservations)
        m_wallet.LockCoin(reservation.outpoint);
    return true;
}

bool PaymasterStore::TransitionSession(const std::string& request_id, SessionState state,
                                       PendingPhase phase, const uint256& final_txid,
                                       int64_t now, std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (!CanTransition(session.state, state)) {
        error = "PAYMASTER_INVALID_SESSION_TRANSITION";
        return false;
    }
    if ((state == SessionState::PENDING_PROVIDER) != (phase != PendingPhase::NONE)) {
        error = "PAYMASTER_INVALID_PENDING_PHASE";
        return false;
    }
    if (!session.final_txid.IsNull() && !final_txid.IsNull() && session.final_txid != final_txid) {
        error = "PAYMASTER_FINAL_TX_CONFLICT";
        return false;
    }

    const bool authorization_risk = HasAuthorizationRisk(state);
    const uint64_t current_wallet_flags = m_wallet.GetWalletFlags();
    const uint64_t protected_wallet_flags =
        current_wallet_flags | WALLET_FLAG_PAYMASTER_AUTHORIZATION;
    const bool protect_wallet = authorization_risk &&
                                protected_wallet_flags != current_wallet_flags;
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    session.state = state;
    session.pending_phase = phase;
    session.updated_at = now;
    if (!final_txid.IsNull()) session.final_txid = final_txid;
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (protect_wallet && !batch.WriteWalletFlags(protected_wallet_flags)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (authorization_risk && !session.provider_side) {
        for (const auto& outpoint : session.user_inputs) {
            InputReservation reservation;
            if (!batch.ReadPaymasterReservation(outpoint, reservation)) {
                return Abort(batch, error, "PAYMASTER_RESERVATION_MISSING");
            }
            reservation.authorization_may_exist = true;
            if (!batch.WritePaymasterReservation(reservation)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    // The durable flag was committed with the session. Publish exactly that
    // value to memory without issuing a second, fallible database write.
    if (protect_wallet && !m_wallet.LoadWalletFlags(protected_wallet_flags)) {
        assert(false);
    }
    return true;
}

bool PaymasterStore::AddAttempt(const std::string& request_id, ProviderAttempt attempt, std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (attempt.version != ProviderAttempt::CURRENT_VERSION || attempt.attempt_id.IsNull() ||
        attempt.provider_id.IsNull() || attempt.state != AttemptState::CANDIDATE ||
        attempt.created_at <= 0 || attempt.updated_at != attempt.created_at ||
        !attempt.commit_key.IsNull() || !attempt.client_nonce.IsNull() ||
        !attempt.intent_hash.IsNull() || !attempt.quote_id.IsNull() || !attempt.unsigned_txid.IsNull() ||
        !attempt.template_commitment.IsNull() || !attempt.unsigned_intent.empty() ||
        !attempt.client_manifest.manifest_id.IsNull() ||
        !attempt.provider_manifest.manifest_id.IsNull() ||
        !attempt.accepted_client_manifest_id.IsNull() ||
        attempt.client_manifest_accepted_at != 0 ||
        attempt.provider_signed_at != 0 ||
        !attempt.provider_signed_result.empty() ||
        !attempt.capacity_proof_claim_candidate.empty() ||
        !attempt.quote_response_claim_candidate.empty() ||
        !attempt.quote_request.empty() ||
        !attempt.signed_quote.empty() ||
        !attempt.unsigned_transaction.empty() || !attempt.unsigned_psbt.empty() ||
        !attempt.input_roles.empty() || !attempt.user_signed_psbt.empty() ||
        !attempt.final_transaction.empty() || !attempt.final_txid.IsNull() ||
        attempt.quote_expires_at != 0 || attempt.retry_until != 0) {
        error = "PAYMASTER_INVALID_ATTEMPT";
        return false;
    }
    if (!attempt.session_id.IsNull() && attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
        return false;
    }
    if (std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt.attempt_id) != session.attempt_ids.end()) {
        ProviderAttempt existing;
        if (batch.ReadPaymasterAttempt(attempt.attempt_id, existing) &&
            existing.session_id == session.session_id && existing.provider_id == attempt.provider_id) return true;
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }
    ProviderAttempt collision;
    if (batch.ReadPaymasterAttempt(attempt.attempt_id, collision)) {
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }

    attempt.session_id = session.session_id;
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAttempt(attempt, false)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    session.attempt_ids.push_back(attempt.attempt_id);
    session.updated_at = std::max(session.updated_at, attempt.created_at);
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::PreparePaymentIntent(const std::string& request_id,
                                          ProviderAttempt attempt,
                                          int64_t now,
                                          std::string& error)
{
    error.clear();
    PaymentIntent intent;
    if (!DecodeUnsignedIntent(attempt.unsigned_intent, intent, now, error)) return false;
    if (attempt.version != ProviderAttempt::CURRENT_VERSION || attempt.attempt_id.IsNull() ||
        attempt.provider_id != intent.provider_id || attempt.state != AttemptState::CANDIDATE ||
        attempt.created_at <= 0 || attempt.created_at > now ||
        attempt.updated_at != attempt.created_at || attempt.client_nonce != intent.client_nonce ||
        !attempt.intent_hash.IsNull() || !attempt.commit_key.IsNull() ||
        !attempt.quote_id.IsNull() || !attempt.unsigned_txid.IsNull() ||
        !attempt.template_commitment.IsNull() || !attempt.capacity_request.empty() ||
        !attempt.capacity_snapshot.snapshot_id.IsNull() || !attempt.quote_request.empty() ||
        !attempt.client_manifest.manifest_id.IsNull() ||
        !attempt.provider_manifest.manifest_id.IsNull() ||
        !attempt.accepted_client_manifest_id.IsNull() ||
        attempt.client_manifest_accepted_at != 0 ||
        attempt.provider_signed_at != 0 ||
        !attempt.provider_signed_result.empty() ||
        !attempt.capacity_proof_claim_candidate.empty() ||
        !attempt.quote_response_claim_candidate.empty() ||
        !attempt.signed_quote.empty() || !attempt.unsigned_transaction.empty() ||
        !attempt.unsigned_psbt.empty() || !attempt.input_roles.empty() ||
        !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
        !attempt.final_txid.IsNull() || attempt.quote_expires_at != 0 || attempt.retry_until != 0) {
        error = "PAYMASTER_INVALID_INTENT_ATTEMPT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (intent.request_id != request_id || intent.session_id != session.session_id ||
        intent.canonical_request_hash != session.canonical_request_hash ||
        intent.requested_fee_mode != session.fee_mode_requested ||
        intent.privacy_profile != attempt.privacy_profile ||
        (!attempt.session_id.IsNull() && attempt.session_id != session.session_id)) {
        error = "PAYMASTER_INTENT_SESSION_CONFLICT";
        return false;
    }
    attempt.session_id = session.session_id;

    ProviderAttempt existing_attempt;
    if (batch.ReadPaymasterAttempt(attempt.attempt_id, existing_attempt)) {
        if (existing_attempt.session_id == attempt.session_id &&
            existing_attempt.provider_id == attempt.provider_id &&
            existing_attempt.client_nonce == attempt.client_nonce &&
            existing_attempt.unsigned_intent == attempt.unsigned_intent &&
            session.user_inputs == intent.user_dd_inputs &&
            session.fee_mode_used == FeeMode::PAYMASTER) {
            return true;
        }
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }
    const bool initial_reservation = session.state == SessionState::CREATED &&
                                     session.attempt_ids.empty();
    bool reuse_reservation = session.state == SessionState::INPUTS_RESERVED &&
                             session.user_inputs == intent.user_dd_inputs &&
                             session.fee_mode_used == FeeMode::PAYMASTER;
    if (reuse_reservation && !session.attempt_ids.empty()) {
        ProviderAttempt previous;
        reuse_reservation = batch.ReadPaymasterAttempt(session.attempt_ids.back(), previous) &&
                            (previous.state == AttemptState::REJECTED ||
                             previous.state == AttemptState::QUOTE_EXPIRED) &&
                            previous.user_signed_psbt.empty() &&
                            previous.final_transaction.empty() &&
                            previous.final_txid.IsNull();
    }
    if (!initial_reservation && !reuse_reservation) {
        error = "PAYMASTER_SESSION_INPUT_CONFLICT";
        return false;
    }

    std::vector<InputReservation> reservations;
    for (const COutPoint& outpoint : intent.user_dd_inputs) {
        InputReservation occupied;
        const bool is_occupied = batch.ReadPaymasterReservation(outpoint, occupied);
        const bool ours = is_occupied && occupied.session_id == session.session_id &&
                          occupied.role == ReservationRole::USER_DD;
        if ((is_occupied && !ours) || (m_wallet.IsLockedCoin(outpoint) && !ours)) {
            error = "PAYMASTER_INPUT_ALREADY_RESERVED";
            return false;
        }
        if (!ours) {
            reservations.push_back({InputReservation::CURRENT_VERSION, outpoint, request_id,
                                    session.session_id, ReservationRole::USER_DD, false, now});
        }
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    for (const InputReservation& reservation : reservations) {
        if (!batch.WritePaymasterReservation(reservation, false) ||
            !batch.WriteLockedUTXO(reservation.outpoint)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (!batch.WritePaymasterAttempt(attempt, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (initial_reservation) session.user_inputs = intent.user_dd_inputs;
    session.attempt_ids.push_back(attempt.attempt_id);
    session.fee_mode_used = FeeMode::PAYMASTER;
    session.state = SessionState::INPUTS_RESERVED;
    session.updated_at = now;
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    for (const InputReservation& reservation : reservations)
        m_wallet.LockCoin(reservation.outpoint);
    return true;
}

bool PaymasterStore::PrepareCapacityRequest(
    const std::string& request_id,
    const uint256& attempt_id,
    const std::vector<unsigned char>& capacity_request,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || capacity_request.empty() ||
        capacity_request.size() > MAX_DIRECT_MESSAGE_BYTES || now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_REQUEST";
        return false;
    }
    PaymasterCapacityRequest decoded;
    try {
        CDataStream stream{capacity_request, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> decoded;
        if (!stream.empty()) throw std::ios_base::failure("trailing capacity request");
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_REQUEST_ENCODING";
        return false;
    }
    CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical << decoded;
    const auto canonical_bytes = MakeUCharSpan(canonical);
    if (!std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                    capacity_request.begin(), capacity_request.end()) ||
        !ValidateCapacityRequestEnvelope(decoded, decoded.genesis_hash, now, error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_REQUEST_NONCANONICAL";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymentIntent intent;
    std::string intent_error;
    if (!DecodeUnsignedIntent(attempt.unsigned_intent, intent, attempt.created_at,
                              intent_error)) {
        error = "PAYMASTER_PERSISTED_INTENT_CORRUPT";
        return false;
    }
    if (attempt.state != AttemptState::CANDIDATE || !attempt.quote_request.empty() ||
        !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
        !attempt.capacity_snapshot.snapshot_id.IsNull() ||
        decoded.request_id != request_id || decoded.session_id != session.session_id ||
        decoded.provider_id != attempt.provider_id ||
        decoded.client_nonce != attempt.client_nonce ||
        decoded.genesis_hash != intent.genesis_hash ||
        decoded.funding_model != intent.funding_model ||
        (intent.funding_model == FundingModel::SPONSORED &&
         decoded.requires_carrier) ||
        decoded.expires_at > intent.expires_at) {
        error = "PAYMASTER_CAPACITY_REQUEST_BINDING_MISMATCH";
        return false;
    }
    if (!attempt.capacity_request.empty()) {
        if (attempt.capacity_request == capacity_request) return true;
        error = "PAYMASTER_CAPACITY_REQUEST_CONFLICT";
        return false;
    }
    attempt.capacity_request = capacity_request;
    attempt.updated_at = std::max(attempt.updated_at, now);
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAttempt(attempt) || !batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::CommitValidatedCapacitySnapshot(
    const std::string& request_id,
    const uint256& attempt_id,
    ValidatedCapacitySnapshot snapshot,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
        snapshot.snapshot_id.IsNull() || snapshot.resource_commitment.IsNull() ||
        snapshot.capacity_proof.empty() || snapshot.validated_at != now || now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_SNAPSHOT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id || attempt.capacity_request.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof proof;
    try {
        CDataStream request_stream{attempt.capacity_request, SER_NETWORK, ::PROTOCOL_VERSION};
        CDataStream proof_stream{snapshot.capacity_proof, SER_NETWORK, ::PROTOCOL_VERSION};
        request_stream >> capacity_request;
        proof_stream >> proof;
        if (!request_stream.empty() || !proof_stream.empty()) {
            throw std::ios_base::failure("trailing capacity data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
        return false;
    }
    CDataStream canonical_proof{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_proof << proof;
    const auto proof_bytes = MakeUCharSpan(canonical_proof);
    if (!std::equal(proof_bytes.begin(), proof_bytes.end(),
                    snapshot.capacity_proof.begin(), snapshot.capacity_proof.end()) ||
        !ValidateCapacityProofEnvelope(proof, capacity_request, now, error) ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) != proof.provider_id ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_SNAPSHOT";
        return false;
    }
    snapshot.session_id = session.session_id;
    snapshot.attempt_id = attempt.attempt_id;
    if (snapshot.snapshot_id != proof.snapshot_id ||
        snapshot.resource_commitment != GetCapacityResourceCommitment(proof) ||
        snapshot.provider_id != proof.provider_id ||
        snapshot.provider_id != attempt.provider_id ||
        snapshot.client_nonce != proof.client_nonce ||
        snapshot.client_nonce != attempt.client_nonce ||
        snapshot.funding_model != proof.funding_model ||
        snapshot.funding_model != capacity_request.funding_model ||
        snapshot.requires_carrier != proof.requires_carrier ||
        snapshot.requires_carrier != capacity_request.requires_carrier ||
        snapshot.request_hash != Hash(attempt.capacity_request) ||
        snapshot.created_at != proof.created_at || snapshot.expires_at != proof.expires_at ||
        snapshot.expires_at <= now) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
        return false;
    }
    PaymasterCapacityRequest authoritative_request;
    if (!attempt.capacity_proof_claim_candidate.empty() &&
        (!ValidatePersistedCapacityRequestAuthority(
             session, attempt, authoritative_request, error) ||
         !ValidatePersistedCapacityClaimCandidate(
             attempt.capacity_proof_claim_candidate, authoritative_request,
             attempt.provider_identity_key,
             "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
             error))) {
        return false;
    }
    if (!attempt.capacity_snapshot.snapshot_id.IsNull()) {
        if (SameCapacitySnapshot(attempt.capacity_snapshot, snapshot)) return true;
        std::string evidence_error;
        if (RecordCapacityEquivocationLocked(
                batch, capacity_request, attempt.provider_identity_key,
                attempt.capacity_snapshot.capacity_proof,
                snapshot.capacity_proof, snapshot.request_hash, now,
                evidence_error)) {
            error = std::move(evidence_error);
            return false;
        }
        if (!evidence_error.empty()) {
            error = std::move(evidence_error);
            return false;
        }
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }

    if (attempt.capacity_proof_claim_candidate.empty()) {
        error = "PAYMASTER_CAPACITY_CLAIM_CANDIDATE_MISSING";
        return false;
    }
    if (attempt.capacity_proof_claim_candidate != snapshot.capacity_proof) {
        std::string evidence_error;
        if (RecordCapacityEquivocationLocked(
                batch, authoritative_request, attempt.provider_identity_key,
                attempt.capacity_proof_claim_candidate,
                snapshot.capacity_proof, snapshot.request_hash, now,
                evidence_error)) {
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_EQUIVOCATION" : std::move(evidence_error);
            return false;
        }
        error = evidence_error.empty() ? "PAYMASTER_CAPACITY_CLAIM_CANDIDATE_CONFLICT" : std::move(evidence_error);
        return false;
    }

    ValidatedCapacitySnapshot by_id;
    if (batch.ReadPaymasterCapacitySnapshot(snapshot.snapshot_id, by_id) &&
        !SameCapacitySnapshot(by_id, snapshot)) {
        std::string evidence_error;
        if (by_id.provider_id == snapshot.provider_id &&
            by_id.request_hash == snapshot.request_hash &&
            RecordCapacityEquivocationLocked(
                batch, capacity_request, attempt.provider_identity_key,
                by_id.capacity_proof, snapshot.capacity_proof,
                snapshot.request_hash, now, evidence_error)) {
            error = std::move(evidence_error);
            return false;
        }
        if (!evidence_error.empty()) {
            error = std::move(evidence_error);
            return false;
        }
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    std::vector<CapacityResourceBinding> resource_bindings;
    if (!BuildCapacityResourceBindings(snapshot, proof, resource_bindings,
                                       error)) {
        return false;
    }

    // Compatibility scan for wallets created before the per-outpoint index.
    // It also provides a fail-closed cross-check if an index entry was lost or
    // corrupted. Only live snapshots can conflict; expired authority may be
    // replaced but remains available as historical evidence.
    std::vector<ValidatedCapacitySnapshot> persisted_snapshots;
    if (!batch.ListPaymasterCapacitySnapshots(persisted_snapshots)) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_DATABASE_READ";
        return false;
    }
    for (const ValidatedCapacitySnapshot& existing : persisted_snapshots) {
        if (existing.snapshot_id == snapshot.snapshot_id ||
            existing.provider_id != snapshot.provider_id ||
            existing.expires_at <= now) {
            continue;
        }
        PaymasterCapacityProof existing_proof;
        if (!DecodeCanonicalCapacityProof(existing.capacity_proof,
                                          existing_proof)) {
            error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
            return false;
        }
        for (const CapacityResourceBinding& binding : resource_bindings) {
            if (!CapacityProofContainsOutpoint(existing_proof,
                                               binding.outpoint)) {
                continue;
            }
            std::string evidence_error;
            if (RecordCapacityResourceEquivocationLocked(
                    batch, snapshot.provider_id,
                    attempt.provider_identity_key, binding.outpoint,
                    existing.capacity_proof, snapshot.capacity_proof, now,
                    evidence_error)) {
                error = std::move(evidence_error);
                return false;
            }
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND" : std::move(evidence_error);
            return false;
        }
    }

    std::vector<bool> replace_resources;
    replace_resources.reserve(resource_bindings.size());
    for (const CapacityResourceBinding& binding : resource_bindings) {
        CapacityResourceBinding existing;
        const bool have_existing = batch.ReadPaymasterCapacityResource(
            binding.provider_id, binding.outpoint, existing);
        if (!have_existing) {
            replace_resources.push_back(false);
            continue;
        }
        if (SameCapacityResourceBinding(existing, binding)) {
            replace_resources.push_back(true);
            continue;
        }
        if (existing.expires_at > now) {
            ValidatedCapacitySnapshot existing_snapshot;
            std::string evidence_error;
            if (batch.ReadPaymasterCapacitySnapshot(existing.snapshot_id,
                                                    existing_snapshot) &&
                RecordCapacityResourceEquivocationLocked(
                    batch, snapshot.provider_id,
                    attempt.provider_identity_key, binding.outpoint,
                    existing_snapshot.capacity_proof,
                    snapshot.capacity_proof, now, evidence_error)) {
                error = std::move(evidence_error);
                return false;
            }
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND" : std::move(evidence_error);
            return false;
        }
        replace_resources.push_back(true);
    }

    uint256 bound_snapshot_id;
    bool replace_slot{false};
    if (batch.ReadPaymasterCapacitySlot(snapshot.resource_commitment,
                                        bound_snapshot_id) &&
        bound_snapshot_id != snapshot.snapshot_id) {
        ValidatedCapacitySnapshot existing;
        if (!batch.ReadPaymasterCapacitySnapshot(bound_snapshot_id, existing)) {
            error = "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND";
            return false;
        }
        // A live aggregate collision necessarily overlaps every resource and
        // has already been rejected above. Reaching here is therefore only a
        // safe expired replacement.
        if (existing.expires_at > now) {
            error = "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND";
            return false;
        }
        replace_slot = true;
    }

    attempt.capacity_snapshot = snapshot;
    attempt.updated_at = std::max(attempt.updated_at, now);
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterCapacitySnapshot(snapshot, false) ||
        !batch.WritePaymasterCapacitySlot(snapshot.resource_commitment,
                                          snapshot.snapshot_id, replace_slot) ||
        !batch.WritePaymasterAttempt(attempt) || !batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    for (size_t index = 0; index < resource_bindings.size(); ++index) {
        if (!batch.WritePaymasterCapacityResource(
                resource_bindings[index], replace_resources[index])) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::RecordCapacityEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || conflicting_capacity_proof.empty() ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt_id) == session.attempt_ids.end() ||
        attempt.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }

    PaymasterCapacityRequest request;
    if (!ValidatePersistedCapacityRequestAuthority(
            session, attempt, request, error)) {
        return false;
    }
    PaymasterCapacityProof persisted_proof;
    if (!VerifyCapacityEvidenceArtifact(
            attempt.capacity_snapshot.capacity_proof, request,
            attempt.provider_identity_key, persisted_proof)) {
        error = "PAYMASTER_PERSISTED_CAPACITY_SNAPSHOT_CORRUPT";
        return false;
    }
    if (!RecordCapacityEquivocationLocked(
            batch, request, attempt.provider_identity_key,
            attempt.capacity_snapshot.capacity_proof,
            conflicting_capacity_proof, Hash(attempt.capacity_request),
            observed_at, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::RecordPendingCapacityEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || first_capacity_proof.empty() ||
        conflicting_capacity_proof.empty() || observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    const ValidatedCapacitySnapshot empty_snapshot;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt)) {
        error = batch.HasPaymasterAttempt(attempt_id)
                    ? PersistedVersionError(
                          "ProviderAttempt", attempt.version,
                          ProviderAttempt::CURRENT_VERSION,
                          "PAYMASTER_INVALID_PERSISTED_ATTEMPT")
                    : "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || session.request_id != request_id ||
        attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !SameCapacitySnapshot(attempt.capacity_snapshot, empty_snapshot)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }

    PaymasterCapacityRequest request;
    if (!ValidatePersistedCapacityRequestAuthority(
            session, attempt, request, error)) {
        return false;
    }
    if (!RecordCapacityEquivocationLocked(
            batch, request, attempt.provider_identity_key,
            first_capacity_proof, conflicting_capacity_proof,
            Hash(attempt.capacity_request), observed_at, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::StageCapacityProofClaimCandidate(
    const uint256& attempt_id,
    const std::vector<unsigned char>& capacity_proof,
    int64_t observed_at,
    bool& equivocation,
    std::string& error)
{
    error.clear();
    equivocation = false;
    if (attempt_id.IsNull() || capacity_proof.empty() ||
        capacity_proof.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    PaymentSession session;
    std::string request_id;
    const ValidatedCapacitySnapshot empty_snapshot;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt)) {
        error = batch.HasPaymasterAttempt(attempt_id)
                    ? PersistedVersionError(
                          "ProviderAttempt", attempt.version,
                          ProviderAttempt::CURRENT_VERSION,
                          "PAYMASTER_INVALID_PERSISTED_ATTEMPT")
                    : "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !SameCapacitySnapshot(attempt.capacity_snapshot, empty_snapshot)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }

    PaymasterCapacityRequest request;
    PaymasterCapacityProof proof;
    std::string validation_error;
    if (!ValidatePersistedCapacityRequestAuthority(
            session, attempt, request, error)) {
        return false;
    }
    if (!attempt.capacity_proof_claim_candidate.empty() &&
        !ValidatePersistedCapacityClaimCandidate(
            attempt.capacity_proof_claim_candidate, request,
            attempt.provider_identity_key,
            "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
            error)) {
        return false;
    }
    if (!DecodeCanonicalCapacityProof(capacity_proof, proof) ||
        !ValidateCapacityProofEnvelope(
            proof, request, proof.created_at, validation_error) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    if (!attempt.capacity_proof_claim_candidate.empty()) {
        if (attempt.capacity_proof_claim_candidate == capacity_proof) {
            return true;
        }
        if (!RecordCapacityEquivocationLocked(
                batch, request, attempt.provider_identity_key,
                attempt.capacity_proof_claim_candidate, capacity_proof,
                Hash(attempt.capacity_request), observed_at, error)) {
            if (error.empty()) {
                error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
            }
            return false;
        }
        equivocation = true;
        error.clear();
        return true;
    }

    if (!ValidateCapacityRequestEnvelope(
            request, request.genesis_hash, observed_at, validation_error) ||
        !ValidateCapacityProofEnvelope(
            proof, request, observed_at, validation_error)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    attempt.version = ProviderAttempt::CURRENT_VERSION;
    attempt.capacity_proof_claim_candidate = capacity_proof;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterAttempt(attempt)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::RecordAlternativeRecoveryCapacityEquivocation(
    const uint256& recovery_id,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (recovery_id.IsNull() || conflicting_capacity_proof.empty() ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord recovery;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        recovery.provider_side || recovery.recovery_id != recovery_id ||
        recovery.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }
    if (!ValidatePersistedRecoveryCapacityRequestAuthority(recovery,
                                                           error)) {
        return false;
    }
    PaymasterCapacityProof persisted_proof;
    if (!VerifyCapacityEvidenceArtifact(
            recovery.capacity_snapshot.capacity_proof,
            recovery.capacity_request,
            recovery.recovery_provider_identity_key, persisted_proof)) {
        error = "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_SNAPSHOT_CORRUPT";
        return false;
    }
    const std::vector<unsigned char> request_bytes =
        CanonicalBytes(recovery.capacity_request);
    if (!RecordCapacityEquivocationLocked(
            batch, recovery.capacity_request,
            recovery.recovery_provider_identity_key,
            recovery.capacity_snapshot.capacity_proof,
            conflicting_capacity_proof, Hash(request_bytes), observed_at,
            error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::RecordPendingAlternativeRecoveryCapacityEquivocation(
    const uint256& recovery_id,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (recovery_id.IsNull() || first_capacity_proof.empty() ||
        conflicting_capacity_proof.empty() || observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord recovery;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        recovery.provider_side || recovery.recovery_id != recovery_id ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING ||
        !recovery.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }
    if (!ValidatePersistedRecoveryCapacityRequestAuthority(recovery,
                                                           error)) {
        return false;
    }
    const std::vector<unsigned char> request_bytes =
        CanonicalBytes(recovery.capacity_request);
    if (!RecordCapacityEquivocationLocked(
            batch, recovery.capacity_request,
            recovery.recovery_provider_identity_key, first_capacity_proof,
            conflicting_capacity_proof, Hash(request_bytes), observed_at,
            error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::StageAlternativeRecoveryCapacityProofClaimCandidate(
    const uint256& recovery_id,
    const std::vector<unsigned char>& capacity_proof,
    int64_t observed_at,
    bool& equivocation,
    std::string& error)
{
    error.clear();
    equivocation = false;
    if (recovery_id.IsNull() || capacity_proof.empty() ||
        capacity_proof.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord recovery;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        recovery.provider_side || recovery.recovery_id != recovery_id ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING ||
        !recovery.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }

    PaymasterCapacityProof proof;
    std::string validation_error;
    if (!ValidatePersistedRecoveryCapacityRequestAuthority(recovery,
                                                           error)) {
        return false;
    }
    if (!recovery.capacity_proof_claim_candidate.empty() &&
        !ValidatePersistedCapacityClaimCandidate(
            recovery.capacity_proof_claim_candidate,
            recovery.capacity_request,
            recovery.recovery_provider_identity_key,
            "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
            error)) {
        return false;
    }
    if (!DecodeCanonicalCapacityProof(capacity_proof, proof) ||
        !ValidateCapacityProofEnvelope(
            proof, recovery.capacity_request, proof.created_at,
            validation_error) ||
        !recovery.recovery_provider_identity_key.VerifySchnorr(
            GetCapacityProofSignatureHash(proof),
            proof.identity_signature)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    if (!recovery.capacity_proof_claim_candidate.empty()) {
        if (recovery.capacity_proof_claim_candidate == capacity_proof) {
            return true;
        }
        const std::vector<unsigned char> request_bytes =
            CanonicalBytes(recovery.capacity_request);
        if (!RecordCapacityEquivocationLocked(
                batch, recovery.capacity_request,
                recovery.recovery_provider_identity_key,
                recovery.capacity_proof_claim_candidate, capacity_proof,
                Hash(request_bytes), observed_at, error)) {
            if (error.empty()) {
                error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
            }
            return false;
        }
        equivocation = true;
        error.clear();
        return true;
    }

    if (!ValidateCapacityRequestEnvelope(
            recovery.capacity_request,
            recovery.capacity_request.genesis_hash, observed_at,
            validation_error) ||
        !ValidateCapacityProofEnvelope(
            proof, recovery.capacity_request, observed_at,
            validation_error)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    recovery.version = AlternativeRecoveryRecord::CURRENT_VERSION;
    recovery.capacity_proof_claim_candidate = capacity_proof;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterAlternativeRecovery(recovery)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetProviderCapacityProof(
    const PaymasterCapacityRequest& request,
    int64_t now,
    PaymasterCapacityProof& proof,
    std::string& error) const
{
    error.clear();
    proof = {};
    if (now <= 0 ||
        !ValidateCapacityRequestEnvelope(request, request.genesis_hash, now,
                                         error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_REQUEST";
        return false;
    }
    const uint256 request_hash{Hash(CanonicalBytes(request))};
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 indexed_hash;
    std::vector<unsigned char> encoded;
    ProviderCapacityReleaseRecord release;
    if (!batch.ReadPaymasterCapacityNonce(request.client_nonce, indexed_hash) ||
        indexed_hash != request_hash ||
        !batch.ReadPaymasterCapacityResponse(request_hash, encoded) ||
        batch.ReadPaymasterCapacityRelease(request_hash, release) ||
        !DecodeCanonicalCapacityProof(encoded, proof) ||
        !ValidateCapacityProofEnvelope(proof, request, now, error)) {
        proof = {};
        if (error.empty()) error = "PAYMASTER_CAPACITY_RESERVATION_MISSING";
        return false;
    }
    return true;
}

bool PaymasterStore::FinalizePaymentIntent(const std::string& request_id,
                                           const uint256& attempt_id,
                                           const std::vector<unsigned char>& quote_request,
                                           int64_t now,
                                           std::string& error)
{
    error.clear();
    if (quote_request.empty() || quote_request.size() > MAX_DIRECT_MESSAGE_BYTES) {
        error = "PAYMASTER_QUOTE_REQUEST_ENCODING";
        return false;
    }
    PaymasterQuoteRequest request;
    try {
        CDataStream stream{quote_request, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> request;
        if (!stream.empty()) {
            error = "PAYMASTER_QUOTE_REQUEST_ENCODING";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_QUOTE_REQUEST_ENCODING";
        return false;
    }
    CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical << request;
    const auto canonical_bytes = MakeUCharSpan(canonical);
    if (!std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                    quote_request.begin(), quote_request.end()) ||
        !ValidateRedactedQuoteRequestEnvelope(request, request.intent.genesis_hash, now, error)) {
        if (error.empty()) error = "PAYMASTER_QUOTE_REQUEST_NONCANONICAL";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymentIntent draft;
    std::string draft_error;
    if (!DecodeUnsignedIntent(attempt.unsigned_intent, draft, attempt.created_at, draft_error)) {
        error = "PAYMASTER_PERSISTED_INTENT_CORRUPT";
        return false;
    }
    if (attempt.capacity_request.empty() || attempt.capacity_snapshot.snapshot_id.IsNull() ||
        attempt.capacity_snapshot.request_hash != Hash(attempt.capacity_request) ||
        attempt.capacity_snapshot.provider_id != attempt.provider_id ||
        attempt.capacity_snapshot.client_nonce != attempt.client_nonce ||
        attempt.capacity_snapshot.expires_at <= now) {
        error = "PAYMASTER_CAPACITY_PROOF_REQUIRED";
        return false;
    }
    if (request.intent.request_id != request_id || request.intent.session_id != session.session_id ||
        request.intent.canonical_request_hash != session.canonical_request_hash ||
        request.intent.requested_fee_mode != session.fee_mode_requested ||
        request.intent.privacy_profile != attempt.privacy_profile ||
        request.intent.provider_id != attempt.provider_id ||
        request.intent.client_nonce != attempt.client_nonce ||
        attempt.sponsorship_capability_hash != request.intent.sponsorship_authorization_hash ||
        GetPaymentIntentCoreHash(request.intent) != GetPaymentIntentCoreHash(draft) ||
        session.user_inputs != request.intent.user_dd_inputs ||
        (session.state != SessionState::INPUTS_RESERVED &&
         session.state != SessionState::AWAITING_WALLET_UNLOCK &&
         session.state != SessionState::AWAITING_USER_SIGNATURE)) {
        error = "PAYMASTER_SIGNED_INTENT_CONFLICT";
        return false;
    }
    const uint256 intent_hash = GetPaymentIntentHash(request.intent);
    if (!attempt.quote_request.empty()) {
        if (attempt.quote_request == quote_request && attempt.intent_hash == intent_hash) return true;
        error = "PAYMASTER_QUOTE_REQUEST_CONFLICT";
        return false;
    }
    if (!attempt.intent_hash.IsNull()) {
        error = "PAYMASTER_INTENT_CONFLICT";
        return false;
    }
    attempt.quote_request = quote_request;
    attempt.intent_hash = intent_hash;
    attempt.updated_at = std::max(attempt.updated_at, now);
    if (!batch.WritePaymasterAttempt(attempt)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool PaymasterStore::AbandonClientAttemptForFallback(const std::string& request_id,
                                                     const uint256& attempt_id,
                                                     int64_t now,
                                                     std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_FALLBACK";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        session.attempt_ids.empty() || session.attempt_ids.back() != attempt_id ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (attempt.state == AttemptState::REJECTED &&
        session.state == SessionState::INPUTS_RESERVED) {
        return true;
    }
    if ((attempt.state != AttemptState::CANDIDATE &&
         attempt.state != AttemptState::QUOTED &&
         attempt.state != AttemptState::QUOTE_EXPIRED) ||
        !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
        !attempt.final_txid.IsNull() ||
        (session.state != SessionState::INPUTS_RESERVED &&
         session.state != SessionState::AWAITING_WALLET_UNLOCK &&
         session.state != SessionState::AWAITING_USER_SIGNATURE &&
         session.state != SessionState::AUTHORIZED)) {
        error = "PAYMASTER_FALLBACK_AUTHORIZATION_MAY_EXIST";
        return false;
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD) {
            error = "PAYMASTER_RESERVATION_MISSING";
            return false;
        }
        if (reservation.authorization_may_exist) {
            error = "PAYMASTER_FALLBACK_AUTHORIZATION_MAY_EXIST";
            return false;
        }
    }

    ClientFeeLedger client_fee_ledger;
    bool client_fee_changed{false};
    if (!attempt.accepted_client_manifest_id.IsNull()) {
        ClientSafetyPolicy client_policy;
        const bool have_client_policy =
            batch.ReadPaymasterClientSafetyPolicy(client_policy);
        const bool have_client_ledger =
            batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
        if ((!have_client_policy &&
             batch.HasPaymasterClientSafetyPolicy()) ||
            (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
            have_client_policy != have_client_ledger) {
            error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
            return false;
        }
        PaymasterQuoteResponse quote_response;
        try {
            SpanReader stream{::PROTOCOL_VERSION, attempt.signed_quote};
            stream >> quote_response;
            if (!stream.empty()) {
                throw std::ios_base::failure(
                    "trailing paymaster quote data");
            }
        } catch (const std::ios_base::failure&) {
            error = "PAYMASTER_QUOTE_ENCODING";
            return false;
        }
        if (have_client_ledger) {
            const auto reservation = std::find_if(
                client_fee_ledger.reservations.begin(),
                client_fee_ledger.reservations.end(),
                [&](const ClientFeeReservation& entry) {
                    return entry.commit_key == attempt.commit_key;
                });
            if (reservation == client_fee_ledger.reservations.end() ||
                reservation->service_fee !=
                    quote_response.quote.service_fee ||
                reservation->state != BudgetReservationState::RESERVED ||
                !ReleaseClientFee(client_fee_ledger,
                                  attempt.commit_key, now, error)) {
                if (error.empty()) {
                    error =
                        "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                }
                return false;
            }
            client_fee_changed = true;
        } else if (quote_response.quote.service_fee.value != 0) {
            error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
            return false;
        }
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    attempt.state = AttemptState::REJECTED;
    attempt.updated_at = std::max(attempt.updated_at, now);
    if (!batch.WritePaymasterAttempt(attempt)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.state = SessionState::INPUTS_RESERVED;
    session.pending_phase = PendingPhase::NONE;
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (client_fee_changed &&
        !batch.WritePaymasterClientFeeLedger(client_fee_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::AbandonUnsignedClientSession(const std::string& request_id,
                                                  int64_t now,
                                                  std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || now <= 0) {
        error = "PAYMASTER_INVALID_UNSIGNED_ABANDON";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (session.state == SessionState::FAILED) {
        for (const COutPoint& outpoint : session.user_inputs) {
            InputReservation reservation;
            if (batch.ReadPaymasterReservation(outpoint, reservation)) {
                error = "PAYMASTER_FAILED_SESSION_STILL_RESERVED";
                return false;
            }
        }
        for (const COutPoint& outpoint : session.user_inputs) {
            m_wallet.UnlockCoin(outpoint);
        }
        return true;
    }
    if ((session.state != SessionState::CREATED &&
         session.state != SessionState::INPUTS_RESERVED &&
         session.state != SessionState::AWAITING_WALLET_UNLOCK &&
         session.state != SessionState::AWAITING_USER_SIGNATURE) ||
        session.pending_phase != PendingPhase::NONE ||
        !session.final_txid.IsNull() || !session.recovery_txid.IsNull()) {
        error = "PAYMASTER_UNSIGNED_ABANDON_AUTHORIZATION_MAY_EXIST";
        return false;
    }

    std::vector<ProviderAttempt> attempts;
    attempts.reserve(session.attempt_ids.size());
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
            attempt.session_id != session.session_id) {
            error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
            return false;
        }
        if ((attempt.state != AttemptState::CANDIDATE &&
             attempt.state != AttemptState::QUOTED &&
             attempt.state != AttemptState::REJECTED &&
             attempt.state != AttemptState::QUOTE_EXPIRED) ||
            !attempt.user_signed_psbt.empty() ||
            !attempt.final_transaction.empty() ||
            !attempt.final_txid.IsNull() || attempt.provider_signed_at > 0 ||
            !attempt.provider_signed_result.empty()) {
            error = "PAYMASTER_UNSIGNED_ABANDON_AUTHORIZATION_MAY_EXIST";
            return false;
        }
        attempts.push_back(std::move(attempt));
    }

    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.request_id != request_id ||
            reservation.role != ReservationRole::USER_DD ||
            reservation.authorization_may_exist) {
            error = "PAYMASTER_RESERVATION_NOT_CANCELABLE";
            return false;
        }
    }

    ClientSafetyPolicy client_policy;
    ClientFeeLedger client_fee_ledger;
    const bool have_client_policy =
        batch.ReadPaymasterClientSafetyPolicy(client_policy);
    const bool have_client_ledger =
        batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
    if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
        (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
        have_client_policy != have_client_ledger) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
    }

    bool client_fee_changed{false};
    for (const ProviderAttempt& attempt : attempts) {
        if (attempt.commit_key.IsNull()) continue;
        if (!have_client_ledger) {
            if (!attempt.accepted_client_manifest_id.IsNull()) {
                PaymasterQuoteResponse response;
                try {
                    SpanReader stream{::PROTOCOL_VERSION,
                                      attempt.signed_quote};
                    stream >> response;
                    if (!stream.empty()) {
                        throw std::ios_base::failure(
                            "trailing paymaster quote data");
                    }
                } catch (const std::ios_base::failure&) {
                    error = "PAYMASTER_QUOTE_ENCODING";
                    return false;
                }
                if (response.quote.service_fee.value != 0) {
                    error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                    return false;
                }
            }
            continue;
        }
        const auto reservation = std::find_if(
            client_fee_ledger.reservations.begin(),
            client_fee_ledger.reservations.end(),
            [&](const ClientFeeReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation == client_fee_ledger.reservations.end()) {
            if (!attempt.accepted_client_manifest_id.IsNull()) {
                error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                return false;
            }
            continue;
        }
        if (reservation->state == BudgetReservationState::SPENT) {
            error = "PAYMASTER_UNSIGNED_ABANDON_AUTHORIZATION_MAY_EXIST";
            return false;
        }
        if (reservation->state == BudgetReservationState::RESERVED) {
            if (!ReleaseClientFee(client_fee_ledger, attempt.commit_key,
                                  now, error)) {
                return false;
            }
            client_fee_changed = true;
        }
    }

    if (!batch.TxnBegin()) {
        return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    }
    for (ProviderAttempt& attempt : attempts) {
        if (attempt.state != AttemptState::QUOTE_EXPIRED) {
            attempt.state = AttemptState::REJECTED;
        }
        attempt.updated_at = std::max(attempt.updated_at, now);
        if (!batch.WritePaymasterAttempt(attempt)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    session.state = SessionState::FAILED;
    session.pending_phase = PendingPhase::NONE;
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.WritePaymasterSession(session) ||
        (client_fee_changed &&
         !batch.WritePaymasterClientFeeLedger(client_fee_ledger))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        if (!batch.ErasePaymasterReservation(outpoint) ||
            !batch.EraseLockedUTXO(outpoint)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        m_wallet.UnlockCoin(outpoint);
    }
    return true;
}

} // namespace wallet
