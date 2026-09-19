// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/walletdb.h>

#include <algorithm>
#include <ios>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wallet {
namespace DBKeys {
const std::string PAYMASTER_SESSION{"pmsession"};
const std::string PAYMASTER_RECOVERY{"pmrecovery"};
const std::string PAYMASTER_ALT_RECOVERY{"pmaltrecovery"};
const std::string PAYMASTER_ALT_RECOVERY_REQUEST{"pmaltrecoveryreq"};
const std::string PAYMASTER_ATTEMPT{"pmattempt"};
const std::string PAYMASTER_CAPACITY{"pmcapacity"};
const std::string PAYMASTER_CAPACITY_SLOT{"pmcapacityslot"};
const std::string PAYMASTER_CAPACITY_RESOURCE{"pmcapacityresource"};
const std::string PAYMASTER_CAPACITY_RESPONSE{"pmcapresponse"};
const std::string PAYMASTER_CAPACITY_NONCE{"pmcapnonce"};
const std::string PAYMASTER_CAPACITY_SESSION{"pmcapsession"};
const std::string PAYMASTER_CAPACITY_RELEASE{"pmcaprelease"};
const std::string PAYMASTER_TEMPLATE{"pmtemplate"};
const std::string PAYMASTER_UNSIGNED_TX{"pmtxid"};
const std::string PAYMASTER_SESSION_ID{"pmsessionid"};
const std::string PAYMASTER_RESERVATION{"pmreserve"};
const std::string PAYMASTER_TOMBSTONE{"pmtombstone"};
const std::string PAYMASTER_PROVIDER_COMMIT{"pmcommit"};
const std::string PAYMASTER_USER_AUTH{"pmauth"};
const std::string PAYMASTER_IDENTITY{"pmidentity"};
const std::string PAYMASTER_POLICY{"pmpolicy"};
const std::string PAYMASTER_SETTINGS{"pmsettings"};
const std::string PAYMASTER_PROVIDER_SAFETY{"pmprovidersafety"};
const std::string PAYMASTER_CLIENT_SAFETY{"pmclientsafety"};
const std::string PAYMASTER_PROVIDER_BUDGET{"pmproviderbudget"};
const std::string PAYMASTER_CLIENT_FEES{"pmclientfees"};
const std::string PAYMASTER_SPONSOR_AUTH{"pmsponsor"};
const std::string PAYMASTER_PROVIDER_POOL{"pmpool"};
const std::string PAYMASTER_LIQUIDITY_POLICY{"pmliquidity"};
const std::string PAYMASTER_MAINTENANCE_LEDGER{"pmmaintenance"};
const std::string PAYMASTER_CARRIER_WITHDRAWAL{"pmcarrierwithdraw"};
const std::string PAYMASTER_FINANCE_LEDGER{"pmfinance"};
const std::string PAYMASTER_BACKUP_STATUS{"pmbackup"};
const std::string PAYMASTER_ANNOUNCE_SEQ{"pmannounceseq"};
const std::string PAYMASTER_RESULT{"pmresult"};
const std::string PAYMASTER_RELIABILITY{"pmreliability"};
const std::string PAYMASTER_OUTCOME{"pmoutcome"};
const std::string PAYMASTER_EQUIVOCATION_PENDING{"pmequivocationpending"};
const std::string PAYMASTER_EQUIVOCATION{"pmequivocation"};
const std::string PAYMASTER_PROVIDER_BLOCK{"pmproviderblock"};
} // namespace DBKeys

// -------------------------------------------------------------------------
// Paymaster database codecs
// -------------------------------------------------------------------------
// WalletBatch provides record-level encoding and shape validation. Multi-record
// security transitions belong to PaymasterStore and must use one explicit DB
// transaction; callers must not compose a signing transition from independent
// WritePaymaster* calls.
template <typename K, typename T>
DatabaseReadStatus WalletBatch::ReadPaymasterVersionedRecord(const K& key,
                                                              T& value)
{
    value = T{};
    const DatabaseReadStatus status = m_batch->ReadWithStatus(key, value);
    // Deserialization of an authentic shortened old layout may stop after its
    // leading version field. Preserve that distinction instead of treating the
    // record as absent or as an unspecified storage failure.
    if ((status == DatabaseReadStatus::FOUND ||
         status == DatabaseReadStatus::READ_ERROR) &&
        value.version != T::CURRENT_VERSION) {
        return DatabaseReadStatus::UNSUPPORTED_VERSION;
    }
    return status;
}

bool WalletBatch::WritePaymasterSession(const DigiDollar::Paymaster::PaymentSession& session, bool overwrite)
{
    if (!DigiDollar::Paymaster::IsCanonicalRequestId(session.request_id) ||
        session.version != DigiDollar::Paymaster::PaymentSession::CURRENT_VERSION) {
        return false;
    }
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_SESSION, session.request_id), session, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSessionWithStatus(
    const std::string& request_id,
    DigiDollar::Paymaster::PaymentSession& session)
{
    using namespace DigiDollar::Paymaster;
    if (!IsCanonicalRequestId(request_id)) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_SESSION, request_id), session);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (session.request_id != request_id || session.session_id.IsNull() ||
        !CanTransition(session.state, session.state)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterSession(
    const std::string& request_id,
    DigiDollar::Paymaster::PaymentSession& session)
{
    return ReadPaymasterSessionWithStatus(request_id, session) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterSession(const std::string& request_id)
{
    return m_batch->Exists(
        std::make_pair(DBKeys::PAYMASTER_SESSION, request_id));
}

bool WalletBatch::ListPaymasterSessions(
    std::vector<DigiDollar::Paymaster::PaymentSession>& sessions)
{
    sessions.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_SESSION;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            std::string request_id;
            DigiDollar::Paymaster::PaymentSession session;
            key >> type >> request_id;
            value >> session;
            if (type != DBKeys::PAYMASTER_SESSION ||
                session.version !=
                    DigiDollar::Paymaster::PaymentSession::CURRENT_VERSION ||
                session.request_id != request_id ||
                !DigiDollar::Paymaster::IsCanonicalRequestId(request_id) ||
                session.session_id.IsNull()) {
                return false;
            }
            sessions.push_back(std::move(session));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterSession(const std::string& request_id)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_SESSION, request_id));
}

bool WalletBatch::WritePaymasterRecovery(
    const DigiDollar::Paymaster::SelfRecoveryRecord& recovery,
    bool overwrite)
{
    if (recovery.version != DigiDollar::Paymaster::SelfRecoveryRecord::CURRENT_VERSION ||
        !DigiDollar::Paymaster::IsCanonicalRequestId(recovery.request_id) ||
        recovery.session_id.IsNull() || recovery.user_inputs.empty() ||
        recovery.recovery_txid.IsNull() || recovery.raw_transaction_hash.IsNull() ||
        recovery.final_transaction.empty() || recovery.created_at <= 0) {
        return false;
    }
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_RECOVERY, recovery.request_id),
                   recovery, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterRecoveryWithStatus(
    const std::string& request_id,
    DigiDollar::Paymaster::SelfRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    if (!IsCanonicalRequestId(request_id)) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RECOVERY, request_id), recovery);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (recovery.request_id != request_id || recovery.session_id.IsNull() ||
        recovery.user_inputs.empty() || recovery.recovery_txid.IsNull() ||
        recovery.raw_transaction_hash.IsNull() ||
        recovery.final_transaction.empty() || recovery.created_at <= 0) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterRecovery(
    const std::string& request_id,
    DigiDollar::Paymaster::SelfRecoveryRecord& recovery)
{
    return ReadPaymasterRecoveryWithStatus(request_id, recovery) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterRecovery(const std::string& request_id)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_RECOVERY, request_id));
}

bool WalletBatch::WritePaymasterAlternativeRecovery(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    bool overwrite)
{
    using namespace DigiDollar::Paymaster;
    if (recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
        !IsCanonicalRequestId(recovery.request_id) ||
        recovery.session_id.IsNull() || recovery.recovery_id.IsNull() ||
        recovery.original_provider_id.IsNull() ||
        recovery.recovery_provider_id.IsNull() ||
        recovery.offer_id.IsNull() || recovery.policy_hash.IsNull() ||
        recovery.original_provider_id == recovery.recovery_provider_id ||
        !recovery.recovery_provider_identity_key.IsFullyValid() ||
        GetPaymasterId(recovery.recovery_provider_identity_key) !=
            recovery.recovery_provider_id ||
        recovery.original_commit_key.IsNull() ||
        recovery.original_template_commitment.IsNull() ||
        recovery.client_nonce.IsNull() || recovery.created_at <= 0 ||
        recovery.updated_at < recovery.created_at ||
        recovery.capacity_proof_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        recovery.recovery_id != GetAlternativeRecoveryId(
                                    recovery.request_id, recovery.session_id,
                                    recovery.recovery_provider_id, recovery.client_nonce)) {
        return false;
    }
    return WriteIC(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery.recovery_id),
        recovery, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterAlternativeRecoveryWithStatus(
    const uint256& recovery_id,
    DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    if (recovery_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery_id), recovery);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (recovery.recovery_id != recovery_id ||
        !IsCanonicalRequestId(recovery.request_id) ||
        recovery.session_id.IsNull() || recovery.original_provider_id.IsNull() ||
        recovery.recovery_provider_id.IsNull() || recovery.offer_id.IsNull() ||
        recovery.policy_hash.IsNull() ||
        recovery.original_provider_id == recovery.recovery_provider_id ||
        !recovery.recovery_provider_identity_key.IsFullyValid() ||
        GetPaymasterId(recovery.recovery_provider_identity_key) !=
            recovery.recovery_provider_id ||
        recovery.original_commit_key.IsNull() ||
        recovery.original_template_commitment.IsNull() ||
        recovery.client_nonce.IsNull() ||
        recovery.capacity_proof_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        recovery.recovery_id != GetAlternativeRecoveryId(
                                    recovery.request_id, recovery.session_id,
                                    recovery.recovery_provider_id,
                                    recovery.client_nonce) ||
        recovery.created_at <= 0 || recovery.updated_at < recovery.created_at) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterAlternativeRecovery(
    const uint256& recovery_id,
    DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    return ReadPaymasterAlternativeRecoveryWithStatus(recovery_id, recovery) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ListPaymasterAlternativeRecoveries(
    std::vector<DigiDollar::Paymaster::AlternativeRecoveryRecord>& recoveries)
{
    using DigiDollar::Paymaster::AlternativeRecoveryRecord;
    recoveries.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_ALT_RECOVERY;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 recovery_id;
            AlternativeRecoveryRecord recovery;
            key >> type >> recovery_id;
            CDataStream recovery_stream{MakeUCharSpan(value), SER_DISK,
                                        CLIENT_VERSION};
            recovery_stream >> recovery;
            if (type != DBKeys::PAYMASTER_ALT_RECOVERY ||
                !recovery_stream.empty() ||
                recovery_id.IsNull() || recovery.recovery_id != recovery_id ||
                recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
                recovery.capacity_proof_claim_candidate.size() >
                    DigiDollar::Paymaster::MAX_EQUIVOCATION_ARTIFACT_BYTES) {
                return false;
            }
            recoveries.push_back(std::move(recovery));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterAlternativeRecovery(
    const uint256& recovery_id)
{
    return !recovery_id.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY,
                                  recovery_id));
}

bool WalletBatch::WritePaymasterAlternativeRecoveryRequest(
    const std::string& request_id, const uint256& recovery_id,
    bool overwrite)
{
    return DigiDollar::Paymaster::IsCanonicalRequestId(request_id) &&
           !recovery_id.IsNull() &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST,
                                  request_id),
                   recovery_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterAlternativeRecoveryRequestWithStatus(
    const std::string& request_id, uint256& recovery_id)
{
    recovery_id.SetNull();
    if (!DigiDollar::Paymaster::IsCanonicalRequestId(request_id)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST, request_id),
        recovery_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return recovery_id.IsNull() ? DatabaseReadStatus::READ_ERROR
                                : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterAlternativeRecoveryRequest(
    const std::string& request_id, uint256& recovery_id)
{
    return ReadPaymasterAlternativeRecoveryRequestWithStatus(
               request_id, recovery_id) == DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterAlternativeRecoveryRequest(
    const std::string& request_id)
{
    return DigiDollar::Paymaster::IsCanonicalRequestId(request_id) &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST,
                                  request_id));
}

bool WalletBatch::WritePaymasterAttempt(const DigiDollar::Paymaster::ProviderAttempt& attempt, bool overwrite)
{
    if (attempt.version != DigiDollar::Paymaster::ProviderAttempt::CURRENT_VERSION || attempt.attempt_id.IsNull() ||
        attempt.capacity_proof_claim_candidate.size() >
            DigiDollar::Paymaster::MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        attempt.quote_response_claim_candidate.size() >
            DigiDollar::Paymaster::MAX_EQUIVOCATION_ARTIFACT_BYTES) {
        return false;
    }
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt.attempt_id), attempt, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterAttemptWithStatus(
    const uint256& attempt_id,
    DigiDollar::Paymaster::ProviderAttempt& attempt)
{
    using namespace DigiDollar::Paymaster;
    if (attempt_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt_id), attempt);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (attempt.attempt_id != attempt_id ||
        !CanTransition(attempt.state, attempt.state) ||
        attempt.capacity_proof_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        attempt.quote_response_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterAttempt(
    const uint256& attempt_id,
    DigiDollar::Paymaster::ProviderAttempt& attempt)
{
    return ReadPaymasterAttemptWithStatus(attempt_id, attempt) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterAttempt(const uint256& attempt_id)
{
    return !attempt_id.IsNull() &&
           m_batch->Exists(
               std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt_id));
}

bool WalletBatch::ErasePaymasterAttempt(const uint256& attempt_id)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt_id));
}

bool WalletBatch::WritePaymasterCapacitySnapshot(
    const DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot,
    bool overwrite)
{
    if (snapshot.version != DigiDollar::Paymaster::ValidatedCapacitySnapshot::CURRENT_VERSION ||
        snapshot.snapshot_id.IsNull() || snapshot.resource_commitment.IsNull() ||
        snapshot.session_id.IsNull() || snapshot.attempt_id.IsNull()) {
        return false;
    }
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY, snapshot.snapshot_id),
                   snapshot, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacitySnapshotWithStatus(
    const uint256& snapshot_id,
    DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot)
{
    using DigiDollar::Paymaster::ValidatedCapacitySnapshot;
    if (snapshot_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY, snapshot_id), snapshot);
    if (status != DatabaseReadStatus::FOUND) return status;
    return snapshot.snapshot_id == snapshot_id &&
                   !snapshot.resource_commitment.IsNull() &&
                   !snapshot.session_id.IsNull() && !snapshot.attempt_id.IsNull()
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCapacitySnapshot(
    const uint256& snapshot_id,
    DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot)
{
    return ReadPaymasterCapacitySnapshotWithStatus(snapshot_id, snapshot) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ListPaymasterCapacitySnapshots(
    std::vector<DigiDollar::Paymaster::ValidatedCapacitySnapshot>& snapshots)
{
    using DigiDollar::Paymaster::ValidatedCapacitySnapshot;
    snapshots.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_CAPACITY;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 snapshot_id;
            ValidatedCapacitySnapshot snapshot;
            key >> type >> snapshot_id;
            value >> snapshot;
            if (type != DBKeys::PAYMASTER_CAPACITY || snapshot_id.IsNull() ||
                snapshot.snapshot_id != snapshot_id ||
                snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
                snapshot.resource_commitment.IsNull() ||
                snapshot.provider_id.IsNull() || snapshot.session_id.IsNull() ||
                snapshot.attempt_id.IsNull()) {
                return false;
            }
            snapshots.push_back(std::move(snapshot));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterCapacitySnapshot(const uint256& snapshot_id)
{
    return !snapshot_id.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY, snapshot_id));
}

bool WalletBatch::WritePaymasterCapacitySlot(const uint256& resource_commitment,
                                             const uint256& snapshot_id,
                                             bool overwrite)
{
    return !resource_commitment.IsNull() && !snapshot_id.IsNull() &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SLOT, resource_commitment),
                   snapshot_id, overwrite);
}

bool WalletBatch::ReadPaymasterCapacitySlot(const uint256& resource_commitment,
                                            uint256& snapshot_id)
{
    return !resource_commitment.IsNull() &&
           m_batch->Read(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SLOT, resource_commitment),
                         snapshot_id) &&
           !snapshot_id.IsNull();
}

bool WalletBatch::ErasePaymasterCapacitySlot(
    const uint256& resource_commitment)
{
    return !resource_commitment.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SLOT,
                                  resource_commitment));
}

bool WalletBatch::WritePaymasterCapacityResource(
    const DigiDollar::Paymaster::CapacityResourceBinding& binding,
    bool overwrite)
{
    using DigiDollar::Paymaster::CapacityResourceBinding;
    if (binding.version != CapacityResourceBinding::CURRENT_VERSION ||
        binding.provider_id.IsNull() || binding.outpoint.IsNull() ||
        binding.snapshot_id.IsNull() || binding.resource_commitment.IsNull() ||
        binding.session_id.IsNull() || binding.attempt_id.IsNull() ||
        binding.proof_hash.IsNull() || binding.creating_txid.IsNull() ||
        binding.value <= 0 || binding.expires_at <= 0) {
        return false;
    }
    return WriteIC(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESOURCE,
                       std::make_pair(binding.provider_id, binding.outpoint)),
        binding, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacityResourceWithStatus(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const COutPoint& outpoint,
    DigiDollar::Paymaster::CapacityResourceBinding& binding)
{
    using DigiDollar::Paymaster::CapacityResourceBinding;
    if (provider_id.IsNull() || outpoint.IsNull()) {
        return DatabaseReadStatus::READ_ERROR;
    }
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESOURCE,
                       std::make_pair(provider_id, outpoint)),
        binding);
    if (status != DatabaseReadStatus::FOUND) return status;
    return binding.provider_id == provider_id && binding.outpoint == outpoint &&
                   !binding.snapshot_id.IsNull() &&
                   !binding.resource_commitment.IsNull() &&
                   !binding.session_id.IsNull() && !binding.attempt_id.IsNull() &&
                   !binding.proof_hash.IsNull() &&
                   !binding.creating_txid.IsNull() && binding.value > 0 &&
                   binding.expires_at > 0
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCapacityResource(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const COutPoint& outpoint,
    DigiDollar::Paymaster::CapacityResourceBinding& binding)
{
    return ReadPaymasterCapacityResourceWithStatus(provider_id, outpoint,
                                                    binding) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterCapacityResource(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const COutPoint& outpoint)
{
    return !provider_id.IsNull() && !outpoint.IsNull() &&
           EraseIC(std::make_pair(
               DBKeys::PAYMASTER_CAPACITY_RESOURCE,
               std::make_pair(provider_id, outpoint)));
}

bool WalletBatch::WritePaymasterCapacityResponse(
    const uint256& request_hash,
    const std::vector<unsigned char>& response,
    bool overwrite)
{
    return !request_hash.IsNull() && !response.empty() &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE, request_hash),
                   response, overwrite);
}

bool WalletBatch::ReadPaymasterCapacityResponse(
    const uint256& request_hash,
    std::vector<unsigned char>& response)
{
    return !request_hash.IsNull() &&
           m_batch->Read(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE, request_hash),
                         response) &&
           !response.empty();
}

bool WalletBatch::ListPaymasterCapacityResponses(
    std::vector<std::pair<uint256, std::vector<unsigned char>>>& responses)
{
    responses.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_CAPACITY_RESPONSE;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 request_hash;
            std::vector<unsigned char> response;
            key >> type >> request_hash;
            value >> response;
            if (type != DBKeys::PAYMASTER_CAPACITY_RESPONSE ||
                request_hash.IsNull() || response.empty()) {
                return false;
            }
            responses.emplace_back(request_hash, std::move(response));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterCapacityResponse(const uint256& request_hash)
{
    return !request_hash.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE,
                                  request_hash));
}

bool WalletBatch::WritePaymasterCapacityNonce(const uint256& client_nonce,
                                              const uint256& request_hash,
                                              bool overwrite)
{
    return !client_nonce.IsNull() && !request_hash.IsNull() &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE, client_nonce),
                   request_hash, overwrite);
}

bool WalletBatch::ReadPaymasterCapacityNonce(const uint256& client_nonce,
                                             uint256& request_hash)
{
    return !client_nonce.IsNull() &&
           m_batch->Read(std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE, client_nonce),
                         request_hash) &&
           !request_hash.IsNull();
}

bool WalletBatch::ErasePaymasterCapacityNonce(const uint256& client_nonce)
{
    return !client_nonce.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE,
                                  client_nonce));
}

bool WalletBatch::WritePaymasterCapacitySession(const uint256& session_key,
                                                const uint256& request_hash,
                                                bool overwrite)
{
    return !session_key.IsNull() && !request_hash.IsNull() &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SESSION, session_key),
                   request_hash, overwrite);
}

bool WalletBatch::ReadPaymasterCapacitySession(const uint256& session_key,
                                               uint256& request_hash)
{
    return !session_key.IsNull() &&
           m_batch->Read(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SESSION, session_key),
                         request_hash) &&
           !request_hash.IsNull();
}

bool WalletBatch::ErasePaymasterCapacitySession(const uint256& session_key)
{
    return !session_key.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SESSION,
                                  session_key));
}

bool WalletBatch::WritePaymasterCapacityRelease(
    const DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release,
    bool overwrite)
{
    return release.version == DigiDollar::Paymaster::ProviderCapacityReleaseRecord::CURRENT_VERSION &&
           !release.request_hash.IsNull() && !release.client_nonce.IsNull() &&
           release.released_at > 0 &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE,
                                  release.request_hash),
                   release, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacityReleaseWithStatus(
    const uint256& request_hash,
    DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release)
{
    if (request_hash.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE, request_hash),
        release);
    if (status != DatabaseReadStatus::FOUND) return status;
    return release.request_hash == request_hash &&
                   !release.client_nonce.IsNull() && release.released_at > 0
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCapacityRelease(
    const uint256& request_hash,
    DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release)
{
    return ReadPaymasterCapacityReleaseWithStatus(request_hash, release) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterCapacityRelease(const uint256& request_hash)
{
    return !request_hash.IsNull() &&
           m_batch->Exists(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE,
                                          request_hash));
}

bool WalletBatch::ErasePaymasterCapacityRelease(const uint256& request_hash)
{
    return !request_hash.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE,
                                  request_hash));
}

bool WalletBatch::WritePaymasterTemplate(const uint256& template_commitment,
                                         const uint256& attempt_id,
                                         bool overwrite)
{
    if (template_commitment.IsNull() || attempt_id.IsNull()) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_TEMPLATE, template_commitment), attempt_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterTemplateWithStatus(
    const uint256& template_commitment, uint256& attempt_id)
{
    attempt_id.SetNull();
    if (template_commitment.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_TEMPLATE, template_commitment),
        attempt_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return attempt_id.IsNull() ? DatabaseReadStatus::READ_ERROR
                               : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterTemplate(const uint256& template_commitment,
                                        uint256& attempt_id)
{
    return ReadPaymasterTemplateWithStatus(template_commitment, attempt_id) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterTemplate(const uint256& template_commitment)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_TEMPLATE, template_commitment));
}

bool WalletBatch::WritePaymasterUnsignedTx(const uint256& unsigned_txid,
                                           const uint256& attempt_id,
                                           bool overwrite)
{
    if (unsigned_txid.IsNull() || attempt_id.IsNull()) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX, unsigned_txid), attempt_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterUnsignedTxWithStatus(
    const uint256& unsigned_txid, uint256& attempt_id)
{
    attempt_id.SetNull();
    if (unsigned_txid.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX, unsigned_txid),
        attempt_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return attempt_id.IsNull() ? DatabaseReadStatus::READ_ERROR
                               : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterUnsignedTx(const uint256& unsigned_txid,
                                          uint256& attempt_id)
{
    return ReadPaymasterUnsignedTxWithStatus(unsigned_txid, attempt_id) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterUnsignedTx(const uint256& unsigned_txid)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX, unsigned_txid));
}

bool WalletBatch::WritePaymasterSessionId(const uint256& session_id, const std::string& request_id, bool overwrite)
{
    if (session_id.IsNull() || !DigiDollar::Paymaster::IsCanonicalRequestId(request_id)) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_SESSION_ID, session_id), request_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSessionIdWithStatus(
    const uint256& session_id, std::string& request_id)
{
    request_id.clear();
    if (session_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_SESSION_ID, session_id), request_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::IsCanonicalRequestId(request_id)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterSessionId(const uint256& session_id,
                                         std::string& request_id)
{
    return ReadPaymasterSessionIdWithStatus(session_id, request_id) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterSessionId(const uint256& session_id)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_SESSION_ID, session_id));
}

bool WalletBatch::WritePaymasterReservation(const DigiDollar::Paymaster::InputReservation& reservation, bool overwrite)
{
    if (reservation.version != DigiDollar::Paymaster::InputReservation::CURRENT_VERSION ||
        reservation.outpoint.IsNull() || reservation.session_id.IsNull() ||
        !DigiDollar::Paymaster::IsCanonicalRequestId(reservation.request_id)) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_RESERVATION, reservation.outpoint), reservation, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterReservationWithStatus(
    const COutPoint& outpoint,
    DigiDollar::Paymaster::InputReservation& reservation)
{
    if (outpoint.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RESERVATION, outpoint), reservation);
    if (status != DatabaseReadStatus::FOUND) return status;
    return reservation.outpoint == outpoint &&
                   !reservation.session_id.IsNull() &&
                   DigiDollar::Paymaster::IsCanonicalRequestId(
                       reservation.request_id)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterReservation(
    const COutPoint& outpoint,
    DigiDollar::Paymaster::InputReservation& reservation)
{
    return ReadPaymasterReservationWithStatus(outpoint, reservation) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterReservation(const COutPoint& outpoint)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_RESERVATION, outpoint));
}

bool WalletBatch::WritePaymasterTombstone(const DigiDollar::Paymaster::IdempotencyTombstone& tombstone, bool overwrite)
{
    if (tombstone.version != DigiDollar::Paymaster::IdempotencyTombstone::CURRENT_VERSION ||
        !DigiDollar::Paymaster::IsCanonicalRequestId(tombstone.request_id) || tombstone.session_id.IsNull() ||
        !DigiDollar::Paymaster::IsTerminal(tombstone.final_state)) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_TOMBSTONE, tombstone.request_id), tombstone, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterTombstoneWithStatus(
    const std::string& request_id,
    DigiDollar::Paymaster::IdempotencyTombstone& tombstone)
{
    using namespace DigiDollar::Paymaster;
    if (!IsCanonicalRequestId(request_id)) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_TOMBSTONE, request_id), tombstone);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (tombstone.request_id != request_id || tombstone.session_id.IsNull() ||
        !IsTerminal(tombstone.final_state)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterTombstone(
    const std::string& request_id,
    DigiDollar::Paymaster::IdempotencyTombstone& tombstone)
{
    return ReadPaymasterTombstoneWithStatus(request_id, tombstone) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterProviderCommit(const DigiDollar::Paymaster::ProviderCommitRecord& commit, bool overwrite)
{
    if (commit.version != DigiDollar::Paymaster::ProviderCommitRecord::CURRENT_VERSION ||
        commit.commit_key.IsNull() || commit.provider_id.IsNull() || commit.quote_id.IsNull() ||
        commit.template_commitment.IsNull() || commit.final_txid.IsNull() ||
        commit.raw_transaction_hash.IsNull() || commit.final_transaction.empty() ||
        commit.provider_inputs.empty()) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit.commit_key), commit, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderCommitWithStatus(
    const uint256& commit_key,
    DigiDollar::Paymaster::ProviderCommitRecord& commit)
{
    if (commit_key.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit_key), commit);
    if (status != DatabaseReadStatus::FOUND) return status;
    return commit.commit_key == commit_key && !commit.provider_id.IsNull() &&
                   !commit.quote_id.IsNull() &&
                   !commit.template_commitment.IsNull() &&
                   !commit.final_txid.IsNull() &&
                   !commit.raw_transaction_hash.IsNull() &&
                   !commit.final_transaction.empty() &&
                   !commit.provider_inputs.empty()
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterProviderCommit(
    const uint256& commit_key,
    DigiDollar::Paymaster::ProviderCommitRecord& commit)
{
    return ReadPaymasterProviderCommitWithStatus(commit_key, commit) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterProviderCommit(const uint256& commit_key)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit_key));
}

bool WalletBatch::ListPaymasterProviderCommits(
    std::vector<DigiDollar::Paymaster::ProviderCommitRecord>& commits)
{
    commits.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_PROVIDER_COMMIT;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 commit_key;
            DigiDollar::Paymaster::ProviderCommitRecord commit;
            key >> type >> commit_key;
            value >> commit;
            if (type != DBKeys::PAYMASTER_PROVIDER_COMMIT ||
                commit.version != DigiDollar::Paymaster::ProviderCommitRecord::CURRENT_VERSION ||
                commit.commit_key != commit_key || commit.provider_id.IsNull() ||
                commit.quote_id.IsNull() || commit.template_commitment.IsNull() ||
                commit.final_txid.IsNull() || commit.raw_transaction_hash.IsNull() ||
                commit.final_transaction.empty() || commit.provider_inputs.empty()) {
                return false;
            }
            commits.push_back(std::move(commit));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::WritePaymasterUserAuthorization(
    const DigiDollar::Paymaster::UserAuthorizationRecord& authorization,
    bool overwrite)
{
    if (authorization.version != DigiDollar::Paymaster::UserAuthorizationRecord::CURRENT_VERSION ||
        authorization.commit_key.IsNull() || authorization.attempt_id.IsNull() ||
        authorization.canonical_psbt_hash.IsNull() || authorization.accepted_at <= 0 ||
        authorization.retry_until < authorization.accepted_at) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_USER_AUTH, authorization.commit_key),
                   authorization, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterUserAuthorizationWithStatus(
    const uint256& commit_key,
    DigiDollar::Paymaster::UserAuthorizationRecord& authorization)
{
    if (commit_key.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_USER_AUTH, commit_key), authorization);
    if (status != DatabaseReadStatus::FOUND) return status;
    return authorization.commit_key == commit_key &&
                   !authorization.attempt_id.IsNull() &&
                   !authorization.canonical_psbt_hash.IsNull() &&
                   authorization.accepted_at > 0 &&
                   authorization.retry_until >= authorization.accepted_at
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterUserAuthorization(
    const uint256& commit_key,
    DigiDollar::Paymaster::UserAuthorizationRecord& authorization)
{
    return ReadPaymasterUserAuthorizationWithStatus(commit_key, authorization) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterUserAuthorization(const uint256& commit_key)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_USER_AUTH, commit_key));
}

bool WalletBatch::WritePaymasterIdentity(
    const DigiDollar::Paymaster::ProviderIdentityRecord& identity,
    bool overwrite)
{
    if (!DigiDollar::Paymaster::ValidateProviderIdentityRecord(identity)) return false;
    return WriteIC(DBKeys::PAYMASTER_IDENTITY, identity, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterIdentityWithStatus(
    DigiDollar::Paymaster::ProviderIdentityRecord& identity)
{
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_IDENTITY, identity);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderIdentityRecord(identity)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterIdentity(
    DigiDollar::Paymaster::ProviderIdentityRecord& identity)
{
    return ReadPaymasterIdentityWithStatus(identity) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterPolicy(const DigiDollar::Paymaster::ProviderPolicy& policy,
                                       bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidateProviderPolicy(policy, error)) return false;
    return WriteIC(DBKeys::PAYMASTER_POLICY, policy, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterPolicyWithStatus(
    DigiDollar::Paymaster::ProviderPolicy& policy)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_POLICY, policy);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderPolicy(policy, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterPolicy(
    DigiDollar::Paymaster::ProviderPolicy& policy)
{
    return ReadPaymasterPolicyWithStatus(policy) == DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterSettings(const DigiDollar::Paymaster::ProviderSettings& settings,
                                         bool overwrite)
{
    if (settings.version != DigiDollar::Paymaster::ProviderSettings::CURRENT_VERSION ||
        settings.updated_at <= 0 || (settings.enabled && settings.policy_hash.IsNull())) return false;
    return WriteIC(DBKeys::PAYMASTER_SETTINGS, settings, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSettingsWithStatus(
    DigiDollar::Paymaster::ProviderSettings& settings)
{
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_SETTINGS, settings);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (settings.updated_at <= 0 ||
        (settings.enabled && settings.policy_hash.IsNull()) ||
        static_cast<uint8_t>(settings.operation_mode) >
            static_cast<uint8_t>(
                DigiDollar::Paymaster::ProviderOperationMode::MANUAL)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterSettings(
    DigiDollar::Paymaster::ProviderSettings& settings)
{
    return ReadPaymasterSettingsWithStatus(settings) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterTombstone(const std::string& request_id)
{
    return m_batch->Exists(
        std::make_pair(DBKeys::PAYMASTER_TOMBSTONE, request_id));
}

bool WalletBatch::HasPaymasterSettings()
{
    return m_batch->Exists(DBKeys::PAYMASTER_SETTINGS);
}

bool WalletBatch::WritePaymasterProviderSafetyPolicy(
    const DigiDollar::Paymaster::ProviderSafetyPolicy& safety,
    bool overwrite)
{
    DigiDollar::Paymaster::ProviderPolicy advertised;
    std::string error;
    if (!ReadPaymasterPolicy(advertised) ||
        !DigiDollar::Paymaster::ValidateProviderSafetyPolicy(safety, advertised, error)) {
        return false;
    }
    return WriteIC(DBKeys::PAYMASTER_PROVIDER_SAFETY, safety, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderSafetyPolicyWithStatus(
    DigiDollar::Paymaster::ProviderSafetyPolicy& safety)
{
    DigiDollar::Paymaster::ProviderPolicy advertised;
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_PROVIDER_SAFETY, safety);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (ReadPaymasterPolicyWithStatus(advertised) !=
            DatabaseReadStatus::FOUND ||
        !DigiDollar::Paymaster::ValidateProviderSafetyPolicy(
            safety, advertised, error)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterProviderSafetyPolicy(
    DigiDollar::Paymaster::ProviderSafetyPolicy& safety)
{
    return ReadPaymasterProviderSafetyPolicyWithStatus(safety) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterProviderSafetyPolicy()
{
    return m_batch->Exists(DBKeys::PAYMASTER_PROVIDER_SAFETY);
}

bool WalletBatch::WritePaymasterClientSafetyPolicy(
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateClientSafetyPolicy(policy, error) &&
           WriteIC(DBKeys::PAYMASTER_CLIENT_SAFETY, policy, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterClientSafetyPolicyWithStatus(
    DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_CLIENT_SAFETY, policy);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateClientSafetyPolicy(policy, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterClientSafetyPolicy(
    DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    return ReadPaymasterClientSafetyPolicyWithStatus(policy) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterClientSafetyPolicy()
{
    return m_batch->Exists(DBKeys::PAYMASTER_CLIENT_SAFETY);
}

bool WalletBatch::WritePaymasterProviderBudgetLedger(
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateProviderBudgetLedger(ledger, error) &&
           WriteIC(DBKeys::PAYMASTER_PROVIDER_BUDGET, ledger, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderBudgetLedgerWithStatus(
    DigiDollar::Paymaster::ProviderBudgetLedger& ledger)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_PROVIDER_BUDGET, ledger);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderBudgetLedger(ledger, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterProviderBudgetLedger(
    DigiDollar::Paymaster::ProviderBudgetLedger& ledger)
{
    return ReadPaymasterProviderBudgetLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterProviderBudgetLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_PROVIDER_BUDGET);
}

bool WalletBatch::WritePaymasterClientFeeLedger(
    const DigiDollar::Paymaster::ClientFeeLedger& ledger,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateClientFeeLedger(ledger, error) &&
           WriteIC(DBKeys::PAYMASTER_CLIENT_FEES, ledger, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterClientFeeLedgerWithStatus(
    DigiDollar::Paymaster::ClientFeeLedger& ledger)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_CLIENT_FEES, ledger);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateClientFeeLedger(ledger, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterClientFeeLedger(
    DigiDollar::Paymaster::ClientFeeLedger& ledger)
{
    return ReadPaymasterClientFeeLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterClientFeeLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_CLIENT_FEES);
}

bool WalletBatch::WritePaymasterSponsorshipAuthorization(
    const DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization,
    bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidateSponsorshipAuthorizationRecord(authorization, error)) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_SPONSOR_AUTH, authorization.capability_hash),
                   authorization, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSponsorshipAuthorizationWithStatus(
    const uint256& capability_hash,
    DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization)
{
    if (capability_hash.IsNull()) return DatabaseReadStatus::READ_ERROR;
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_SPONSOR_AUTH, capability_hash),
        authorization);
    if (status != DatabaseReadStatus::FOUND) return status;
    return authorization.capability_hash == capability_hash &&
                   DigiDollar::Paymaster::ValidateSponsorshipAuthorizationRecord(
                       authorization, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterSponsorshipAuthorization(
    const uint256& capability_hash,
    DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization)
{
    return ReadPaymasterSponsorshipAuthorizationWithStatus(
               capability_hash, authorization) == DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterProviderPool(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    bool overwrite)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    if (!ValidateProviderPoolEntries(entries, error)) return false;
    return WriteIC(DBKeys::PAYMASTER_PROVIDER_POOL, entries, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderPoolWithStatus(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries)
{
    using namespace DigiDollar::Paymaster;
    entries.clear();
    std::string error;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        DBKeys::PAYMASTER_PROVIDER_POOL, entries);
    const auto outdated = std::find_if(
        entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
            return entry.version != ProviderPoolEntry::CURRENT_VERSION;
        });
    if (outdated != entries.end()) {
        return DatabaseReadStatus::UNSUPPORTED_VERSION;
    }
    if (status != DatabaseReadStatus::FOUND) return status;
    return ValidateProviderPoolEntries(entries, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterProviderPool(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries)
{
    return ReadPaymasterProviderPoolWithStatus(entries) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterProviderPool()
{
    return m_batch->Exists(DBKeys::PAYMASTER_PROVIDER_POOL);
}

bool WalletBatch::WritePaymasterLiquidityPolicy(
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidateProviderLiquidityPolicy(policy, error)) {
        return false;
    }
    return WriteIC(DBKeys::PAYMASTER_LIQUIDITY_POLICY, policy, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterLiquidityPolicyWithStatus(
    DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_LIQUIDITY_POLICY, policy);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderLiquidityPolicy(policy,
                                                                   error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterLiquidityPolicy(
    DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    return ReadPaymasterLiquidityPolicyWithStatus(policy) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterLiquidityPolicy()
{
    return m_batch->Exists(DBKeys::PAYMASTER_LIQUIDITY_POLICY);
}

bool WalletBatch::WritePaymasterMaintenanceLedger(
    const DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger,
    bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidateProviderMaintenanceLedger(ledger, error)) {
        return false;
    }
    return WriteIC(DBKeys::PAYMASTER_MAINTENANCE_LEDGER, ledger, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterMaintenanceLedgerWithStatus(
    DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_MAINTENANCE_LEDGER, ledger);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderMaintenanceLedger(ledger,
                                                                    error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterMaintenanceLedger(
    DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger)
{
    return ReadPaymasterMaintenanceLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterMaintenanceLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_MAINTENANCE_LEDGER);
}

bool WalletBatch::WritePaymasterFinanceLedger(
    const DigiDollar::Paymaster::ProviderFinanceLedger& ledger,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateProviderFinanceLedger(ledger, error) &&
           WriteIC(DBKeys::PAYMASTER_FINANCE_LEDGER, ledger, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterFinanceLedgerWithStatus(
    DigiDollar::Paymaster::ProviderFinanceLedger& ledger)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_FINANCE_LEDGER, ledger);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderFinanceLedger(ledger, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterFinanceLedger(
    DigiDollar::Paymaster::ProviderFinanceLedger& ledger)
{
    return ReadPaymasterFinanceLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterFinanceLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_FINANCE_LEDGER);
}

bool WalletBatch::WritePaymasterBackupStatus(
    const DigiDollar::Paymaster::ProviderBackupStatus& status,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateProviderBackupStatus(status, error) &&
           WriteIC(DBKeys::PAYMASTER_BACKUP_STATUS, status, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterBackupStatusWithStatus(
    DigiDollar::Paymaster::ProviderBackupStatus& status)
{
    std::string error;
    const DatabaseReadStatus read_status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_BACKUP_STATUS, status);
    if (read_status != DatabaseReadStatus::FOUND) return read_status;
    return DigiDollar::Paymaster::ValidateProviderBackupStatus(status, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterBackupStatus(
    DigiDollar::Paymaster::ProviderBackupStatus& status)
{
    return ReadPaymasterBackupStatusWithStatus(status) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterBackupStatus()
{
    return m_batch->Exists(DBKeys::PAYMASTER_BACKUP_STATUS);
}

bool WalletBatch::WritePaymasterCarrierWithdrawalPlan(
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateProviderCarrierWithdrawalPlan(
               plan, error) &&
           WriteIC(DBKeys::PAYMASTER_CARRIER_WITHDRAWAL, plan, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCarrierWithdrawalPlanWithStatus(
    DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_CARRIER_WITHDRAWAL, plan);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderCarrierWithdrawalPlan(
               plan, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCarrierWithdrawalPlan(
    DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan)
{
    return ReadPaymasterCarrierWithdrawalPlanWithStatus(plan) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterCarrierWithdrawalPlan()
{
    return m_batch->Exists(DBKeys::PAYMASTER_CARRIER_WITHDRAWAL);
}

bool WalletBatch::ErasePaymasterCarrierWithdrawalPlan()
{
    return m_batch->Erase(DBKeys::PAYMASTER_CARRIER_WITHDRAWAL);
}

bool WalletBatch::WritePaymasterAnnouncementSequence(uint64_t sequence, bool overwrite)
{
    if (sequence == 0) return false;
    return WriteIC(DBKeys::PAYMASTER_ANNOUNCE_SEQ, sequence, overwrite);
}

bool WalletBatch::ReadPaymasterAnnouncementSequence(uint64_t& sequence)
{
    return m_batch->Read(DBKeys::PAYMASTER_ANNOUNCE_SEQ, sequence) && sequence != 0;
}

bool WalletBatch::WritePaymasterResult(const DigiDollar::Paymaster::PaymasterResult& result,
                                       bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidatePaymasterResultShape(result, error)) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_RESULT, result.commit_key), result, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterResultWithStatus(
    const uint256& commit_key,
    DigiDollar::Paymaster::PaymasterResult& result)
{
    std::string error;
    if (commit_key.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RESULT, commit_key), result);
    if (status != DatabaseReadStatus::FOUND) return status;
    return result.commit_key == commit_key &&
                   DigiDollar::Paymaster::ValidatePaymasterResultShape(result,
                                                                       error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterResult(
    const uint256& commit_key,
    DigiDollar::Paymaster::PaymasterResult& result)
{
    return ReadPaymasterResultWithStatus(commit_key, result) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterResult(const uint256& commit_key)
{
    return !commit_key.IsNull() &&
           m_batch->Exists(std::make_pair(DBKeys::PAYMASTER_RESULT, commit_key));
}

bool WalletBatch::ErasePaymasterResult(const uint256& commit_key)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_RESULT, commit_key));
}

bool WalletBatch::WritePaymasterReliability(
    const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
    bool overwrite)
{
    if (!DigiDollar::Paymaster::ValidateReliabilityRecord(record)) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_RELIABILITY, record.provider_id), record, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterReliabilityWithStatus(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterReliabilityRecord& record)
{
    if (provider_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RELIABILITY, provider_id), record);
    if (status != DatabaseReadStatus::FOUND) return status;
    return record.provider_id == provider_id &&
                   DigiDollar::Paymaster::ValidateReliabilityRecord(record)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterReliability(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterReliabilityRecord& record)
{
    return ReadPaymasterReliabilityWithStatus(provider_id, record) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ListPaymasterReliability(
    std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord>& records)
{
    records.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_RELIABILITY;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            DigiDollar::Paymaster::PaymasterId provider_id;
            DigiDollar::Paymaster::PaymasterReliabilityRecord record;
            key >> type >> provider_id;
            value >> record;
            if (type != DBKeys::PAYMASTER_RELIABILITY || record.provider_id != provider_id ||
                !DigiDollar::Paymaster::ValidateReliabilityRecord(record)) return false;
            records.push_back(std::move(record));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterReliability(const DigiDollar::Paymaster::PaymasterId& provider_id)
{
    return !provider_id.IsNull() && EraseIC(std::make_pair(DBKeys::PAYMASTER_RELIABILITY, provider_id));
}

bool WalletBatch::WritePaymasterEquivocationEvidence(
    const DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
    bool overwrite)
{
    return DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence) &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION, evidence.evidence_id),
                   evidence, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterEquivocationEvidence(
    const uint256& evidence_id,
    DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence)
{
    if (evidence_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION, evidence_id), evidence);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (evidence.evidence_id != evidence_id ||
        !DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterPendingEquivocation(
    const DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
    bool overwrite)
{
    return DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence) &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING,
                                  evidence.provider_id),
                   evidence, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterPendingEquivocation(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence)
{
    if (provider_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING, provider_id),
        evidence);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (evidence.provider_id != provider_id ||
        !DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ListPaymasterPendingEquivocations(
    std::vector<DigiDollar::Paymaster::PaymasterEquivocationEvidence>& evidence)
{
    using DigiDollar::Paymaster::PaymasterEquivocationEvidence;
    evidence.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_EQUIVOCATION_PENDING;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return DatabaseReadStatus::READ_ERROR;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) {
            evidence.clear();
            return DatabaseReadStatus::READ_ERROR;
        }
        try {
            std::string type;
            DigiDollar::Paymaster::PaymasterId provider_id;
            PaymasterEquivocationEvidence candidate;
            key >> type >> provider_id;
            value >> candidate;
            if (!key.empty() || !value.empty() ||
                type != DBKeys::PAYMASTER_EQUIVOCATION_PENDING ||
                provider_id.IsNull() || candidate.provider_id != provider_id ||
                !DigiDollar::Paymaster::ValidateEquivocationEvidence(candidate)) {
                evidence.clear();
                return DatabaseReadStatus::READ_ERROR;
            }
            evidence.push_back(std::move(candidate));
        } catch (const std::ios_base::failure&) {
            evidence.clear();
            return DatabaseReadStatus::READ_ERROR;
        }
    }
    return evidence.empty() ? DatabaseReadStatus::NOT_FOUND : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterPendingEquivocation(
    const DigiDollar::Paymaster::PaymasterId& provider_id)
{
    return !provider_id.IsNull() &&
           EraseIC(std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING,
                                  provider_id));
}

bool WalletBatch::WritePaymasterProviderBlock(
    const DigiDollar::Paymaster::PaymasterProviderBlock& block,
    bool overwrite)
{
    return DigiDollar::Paymaster::ValidateProviderBlock(block) &&
           WriteIC(std::make_pair(DBKeys::PAYMASTER_PROVIDER_BLOCK, block.provider_id),
                   block, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderBlock(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterProviderBlock& block)
{
    if (provider_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_PROVIDER_BLOCK, provider_id), block);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (block.provider_id != provider_id ||
        !DigiDollar::Paymaster::ValidateProviderBlock(block)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ListPaymasterProviderBlocks(
    std::vector<DigiDollar::Paymaster::PaymasterProviderBlock>& blocks)
{
    blocks.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_PROVIDER_BLOCK;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            DigiDollar::Paymaster::PaymasterId provider_id;
            DigiDollar::Paymaster::PaymasterProviderBlock block;
            key >> type >> provider_id;
            value >> block;
            if (type != DBKeys::PAYMASTER_PROVIDER_BLOCK || block.provider_id != provider_id ||
                !DigiDollar::Paymaster::ValidateProviderBlock(block)) return false;
            blocks.push_back(std::move(block));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::WritePaymasterOutcomeMarker(
    const DigiDollar::Paymaster::PaymasterOutcomeMarker& marker,
    bool overwrite)
{
    if (marker.version != DigiDollar::Paymaster::PaymasterOutcomeMarker::CURRENT_VERSION ||
        marker.attempt_id.IsNull() || marker.provider_id.IsNull() || marker.observed_at <= 0 ||
        static_cast<uint8_t>(marker.outcome) >
            static_cast<uint8_t>(DigiDollar::Paymaster::ReliabilityOutcome::AVAILABILITY_TIMEOUT)) return false;
    return WriteIC(std::make_pair(DBKeys::PAYMASTER_OUTCOME, marker.attempt_id), marker, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterOutcomeMarkerWithStatus(
    const uint256& attempt_id,
    DigiDollar::Paymaster::PaymasterOutcomeMarker& marker)
{
    using namespace DigiDollar::Paymaster;
    if (attempt_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_OUTCOME, attempt_id), marker);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (marker.attempt_id != attempt_id || marker.provider_id.IsNull() ||
        marker.observed_at <= 0 ||
        static_cast<uint8_t>(marker.outcome) >
            static_cast<uint8_t>(ReliabilityOutcome::AVAILABILITY_TIMEOUT)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterOutcomeMarker(
    const uint256& attempt_id,
    DigiDollar::Paymaster::PaymasterOutcomeMarker& marker)
{
    return ReadPaymasterOutcomeMarkerWithStatus(attempt_id, marker) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterOutcomeMarker(const uint256& attempt_id)
{
    return EraseIC(std::make_pair(DBKeys::PAYMASTER_OUTCOME, attempt_id));
}

} // namespace wallet
