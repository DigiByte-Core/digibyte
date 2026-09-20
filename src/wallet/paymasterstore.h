// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Atomic wallet persistence API for all security-relevant Paymaster state. */

#ifndef DIGIBYTE_WALLET_PAYMASTERSTORE_H
#define DIGIBYTE_WALLET_PAYMASTERSTORE_H

#include <paymaster/protocol.h>
#include <paymaster/recovery.h>
#include <paymaster/reputation.h>
#include <paymaster/reservation.h>
#include <paymaster/sponsorship.h>
#include <primitives/transaction.h>
#include <wallet/db.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wallet {

class CWallet;

/** Paymaster reservations are hard safety locks. Unlike ordinary lockunspent
 * hints they may not be bypassed by preset coin-control inputs.
 */
bool IsPaymasterInputReserved(const CWallet& wallet, const COutPoint& outpoint);

/** Snapshot every live provider-pool input for one coin-selection pass. A
 * non-success status must make the caller fail closed instead of treating an
 * unreadable safety record as an empty pool. */
DatabaseReadStatus GetPaymasterProviderPoolInputs(
    const CWallet& wallet, std::set<COutPoint>& inputs);

/** Bind one provider-side alternative-recovery record to exactly one local
 * budget reservation. Current-policy mode is the first USER_SIGNED firewall;
 * historical mode is reserved for exact durable retries after that boundary. */
bool ValidateProviderAlternativeRecoveryBudgetAuthorization(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const DigiDollar::Paymaster::ProviderSafetyPolicy* policy,
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    DigiDollar::Paymaster::BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error);

enum class CreatePaymasterSessionResult {
    CREATED,
    JOINED,
    FINAL_TOMBSTONE,
    CONFLICT,
    DATABASE_ERROR,
};

class PaymasterStore
{
public:
    explicit PaymasterStore(CWallet& wallet) : m_wallet{wallet} {}

    CreatePaymasterSessionResult CreateOrJoinSession(
        const std::string& request_id,
        const uint256& canonical_request_hash,
        DigiDollar::Paymaster::FeeMode requested_mode,
        int64_t now,
        DigiDollar::Paymaster::PaymentSession& session,
        std::string& error);

    /** Bind the wallet-local amount semantics before any USER_DD input is
     * reserved. Exact retries are idempotent; any material change fails
     * closed under the existing request id. */
    bool BindClientPaymentOrder(
        const std::string& request_id,
        DigiDollar::Paymaster::DDCents requested_amount,
        bool subtract_paymaster_fee_from_amount,
        bool send_all_spendable_dd,
        std::string& error);

    /** Only NOT_FOUND permits starting another payment. Read failures must
     * remain distinguishable when deciding whether an AUTO request is new. */
    DatabaseReadStatus GetSessionByRequestIdWithStatus(
        const std::string& request_id,
        DigiDollar::Paymaster::PaymentSession& session) const;
    bool GetSessionByRequestId(const std::string& request_id,
                               DigiDollar::Paymaster::PaymentSession& session) const;
    bool GetSessionBySessionId(const uint256& session_id,
                               DigiDollar::Paymaster::PaymentSession& session) const;
    /** Snapshot client-side sessions for periodic inbox maintenance. Provider
     * records are deliberately excluded so remote conflict evidence is never
     * interpreted using local provider authority. */
    bool ListClientSessions(
        std::vector<DigiDollar::Paymaster::PaymentSession>& sessions,
        std::string& error) const;
    /** Report whether a client session still owns any live USER_DD
     * reservation. This is a read-only inbox/status aid; mutation RPCs remain
     * the sole authority for deciding whether an input can be released. */
    bool ClientSessionHasLiveReservations(
        const DigiDollar::Paymaster::PaymentSession& session,
        bool& has_live_reservations,
        std::string& error) const;

    bool ReserveInputs(const std::string& request_id,
                       const std::vector<std::pair<COutPoint, DigiDollar::Paymaster::ReservationRole>>& inputs,
                       DigiDollar::Paymaster::FeeMode used_mode,
                       int64_t now,
                       std::string& error);

    bool TransitionSession(const std::string& request_id,
                           DigiDollar::Paymaster::SessionState state,
                           DigiDollar::Paymaster::PendingPhase phase,
                           const uint256& final_txid,
                           int64_t now,
                           std::string& error);

    bool AddAttempt(const std::string& request_id,
                    DigiDollar::Paymaster::ProviderAttempt attempt,
                    std::string& error);
    /** Persist an unsigned intent and reserve its USER_DD inputs before an
     * encrypted wallet is unlocked. */
    bool PreparePaymentIntent(
        const std::string& request_id,
        DigiDollar::Paymaster::ProviderAttempt attempt,
        int64_t now,
        std::string& error);
    /** Persist the canonical capacity request before it is disclosed. No
     * signed payment intent, DD outpoint proof or restricted capability may be
     * attached at this stage. */
    bool PrepareCapacityRequest(
        const std::string& request_id,
        const uint256& attempt_id,
        const std::vector<unsigned char>& capacity_request,
        int64_t now,
        std::string& error);
    /** Atomically bind one fully validated capacity snapshot to this client
     * attempt and to its exact operational-resource commitment. */
    bool CommitValidatedCapacitySnapshot(
        const std::string& request_id,
        const uint256& attempt_id,
        DigiDollar::Paymaster::ValidatedCapacitySnapshot snapshot,
        int64_t now,
        std::string& error);
    /** Persist the first canonical identity-signed Capacity claim for this
     * exact client attempt before chainstate validation. The candidate is an
     * evidence-only replay barrier and never authorizes use of its resources.
     * An exact retry is idempotent. A different overlapping signed claim is
     * converted to durable two-phase equivocation evidence and sets
     * equivocation=true. */
    bool StageCapacityProofClaimCandidate(
        const uint256& attempt_id,
        const std::vector<unsigned char>& capacity_proof,
        int64_t observed_at,
        bool& equivocation,
        std::string& error);
    /** Verify and durably retain two provider-signed capacity proofs for the
     * same client-side request binding, then permanently block that provider
     * locally. Invalid, unsigned, or differently bound artifacts cannot
     * create a block. */
    bool RecordCapacityEquivocation(
        const uint256& attempt_id,
        const std::vector<unsigned char>& conflicting_capacity_proof,
        int64_t observed_at,
        std::string& error);
    /** Record two conflicting proofs observed before the first normal client
     * snapshot can be committed. Both artifacts are independently verified
     * against the exact persisted capacity request and provider identity. */
    bool RecordPendingCapacityEquivocation(
        const uint256& attempt_id,
        const std::vector<unsigned char>& first_capacity_proof,
        const std::vector<unsigned char>& conflicting_capacity_proof,
        int64_t observed_at,
        std::string& error);
    /** Capacity evidence for the mandatory handshake of an alternative
     * recovery provider. This has the same signature-only evidence boundary
     * as RecordCapacityEquivocation and never trusts peer-supplied state. */
    bool RecordAlternativeRecoveryCapacityEquivocation(
        const uint256& recovery_id,
        const std::vector<unsigned char>& conflicting_capacity_proof,
        int64_t observed_at,
        std::string& error);
    /** Record two conflicting proofs observed before the first recovery
     * snapshot can be committed. Both artifacts are independently verified
     * against the already persisted, non-payment-revealing capacity request. */
    bool RecordPendingAlternativeRecoveryCapacityEquivocation(
        const uint256& recovery_id,
        const std::vector<unsigned char>& first_capacity_proof,
        const std::vector<unsigned char>& conflicting_capacity_proof,
        int64_t observed_at,
        std::string& error);
    /** Recovery equivalent of StageCapacityProofClaimCandidate. The first
     * signed claim remains distinct from capacity_snapshot and every recovery
     * authorization field. */
    bool StageAlternativeRecoveryCapacityProofClaimCandidate(
        const uint256& recovery_id,
        const std::vector<unsigned char>& capacity_proof,
        int64_t observed_at,
        bool& equivocation,
        std::string& error);
    /** Reload the exact provider-side capacity proof previously committed for
     * this canonical request. This is the only authority an alternative
     * recovery request may use to bind the already reserved pool slot. */
    bool GetProviderCapacityProof(
        const DigiDollar::Paymaster::PaymasterCapacityRequest& request,
        int64_t now,
        DigiDollar::Paymaster::PaymasterCapacityProof& proof,
        std::string& error) const;
    /** Attach the exact signed request to an already reserved intent without
     * changing its provider, nonce, inputs, outputs, or policy binding. */
    bool FinalizePaymentIntent(
        const std::string& request_id,
        const uint256& attempt_id,
        const std::vector<unsigned char>& quote_request,
        int64_t now,
        std::string& error);
    /** Close the latest unsigned client attempt while retaining the exact
     * USER_DD reservations for a sequential provider fallback. This is
     * forbidden once a user PSBT or any final transaction may exist. */
    bool AbandonClientAttemptForFallback(
        const std::string& request_id,
        const uint256& attempt_id,
        int64_t now,
        std::string& error);
    /** Atomically terminate a client session for which no transaction
     * signature or final transaction can exist. Every USER_DD reservation and
     * still-reserved client fee is released. Exact retries are idempotent;
     * any durable authorization evidence makes the operation fail closed. */
    bool AbandonUnsignedClientSession(
        const std::string& request_id,
        int64_t now,
        std::string& error);
    /** Pure preflight for a quote that continues one exact durable Capacity
     * admission. canonical_netgroup is transient NetGroupManager output and
     * is wallet-HMACed without being persisted. The authoritative quote hash
     * binding, admission promotion, budget reservation and pool rebind occur
     * together in CommitProviderQuote. */
    bool CheckProviderQuoteAdmission(
        const DigiDollar::Paymaster::PaymasterQuoteRequest& request,
        const uint256& request_hash,
        const std::vector<unsigned char>& canonical_netgroup,
        bool requires_carrier,
        DigiDollar::Paymaster::DGBSatoshis proposed_network_fee,
        int64_t now,
        uint256& netgroup_bucket,
        std::string& error) const;
    /** Pure equivalent for an alternative-recovery quote. It consumes no
     * durable budget authority and exists only to reject stale, mismatched or
     * economically inadmissible work before key derivation and signing. */
    bool CheckProviderRecoveryAdmission(
        const DigiDollar::Paymaster::AlternativeRecoveryRequest& request,
        const uint256& request_hash,
        const std::vector<unsigned char>& canonical_netgroup,
        bool requires_carrier,
        DigiDollar::Paymaster::DGBSatoshis proposed_network_fee,
        int64_t now,
        uint256& netgroup_bucket,
        std::string& error) const;
    /** Atomically create the provider-side session, persist its signed quote,
     * indexes and bind every operational pool input to the commit key. */
    bool CommitProviderQuote(
        DigiDollar::Paymaster::ProviderAttempt attempt,
        const uint256& expected_genesis,
        int64_t now,
        std::string& error);
    bool CommitProviderQuote(
        DigiDollar::Paymaster::ProviderAttempt attempt,
        std::optional<DigiDollar::Paymaster::SponsorshipAuthorizationRecord> sponsorship,
        const uint256& expected_genesis,
        int64_t now,
        std::string& error);
    bool UpdateAttempt(const std::string& request_id,
                       const DigiDollar::Paymaster::ProviderAttempt& update,
                       std::string& error);
    /** Persist the user's explicit approval of the exact client manifest.
     * Only a client-side QUOTED attempt can acquire this immutable binding.
     * Exact retries are read-only and conflicting commitments fail closed. */
    bool AcceptClientAuthorization(const std::string& request_id,
                                   const uint256& attempt_id,
                                   const uint256& manifest_id,
                                   int64_t now,
                                   std::string& error);
    bool GetAttempt(const uint256& attempt_id,
                    DigiDollar::Paymaster::ProviderAttempt& attempt) const;
    bool GetAttemptByTemplateCommitment(
        const uint256& template_commitment,
        DigiDollar::Paymaster::ProviderAttempt& attempt) const;
    bool GetAttemptByUnsignedTxid(
        const uint256& unsigned_txid,
        DigiDollar::Paymaster::ProviderAttempt& attempt) const;

    /** Re-read the wallet-local safety policy and ledger and require the
     * attempt's provider manifest to bind exactly one reservation in the
     * requested state. This is the last database-backed firewall before a
     * provider signature, exact retry, recovery, or broadcast. */
    bool ValidateProviderBudgetAuthorization(
        const DigiDollar::Paymaster::ProviderAttempt& attempt,
        DigiDollar::Paymaster::BudgetReservationState expected_state,
        bool allow_historical_policy,
        std::string& error) const;

    /** Last read-only database firewall before creating a normal provider
     * input signature. The exact durable USER authorization, Capacity
     * admission/proof, pool reservation and budget reservation are reloaded
     * under one wallet lock. On success the returned Capacity artifacts are
     * the canonical wallet records that the caller must revalidate against
     * the current chainstate immediately before signing. */
    bool ValidateProviderPreSignatureAuthorization(
        const DigiDollar::Paymaster::ProviderAttempt& attempt,
        const uint256& expected_genesis,
        int64_t now,
        DigiDollar::Paymaster::PaymasterCapacityRequest& capacity_request,
        DigiDollar::Paymaster::PaymasterCapacityProof& capacity_proof,
        std::string& error) const;

    /** Atomically reject an unsigned quote and release only provider pool
     * entries bound to its commit key. Any possible user authorization makes
     * cancellation unsafe and is rejected. */
    bool CancelProviderQuote(const uint256& attempt_id,
                             int64_t now,
                             DigiDollar::Paymaster::ProviderAttempt& attempt,
                             std::string& error);
    /** Atomically expire every quote whose TTL elapsed before a signature
     * could exist. Provider quotes release pool/budget reservations; client
     * quotes release fee and USER_DD reservations in the same transaction.
     * Any durable signature/final artifact always wins over timeout. */
    bool ExpireProviderQuotes(int64_t now,
                              size_t& expired_quotes,
                              std::string& error);

    /** Atomically release provider-pool slots held only by an expired signed
     * PMCAPREQ response. Responses and semantic indexes remain replay barriers
     * through the bounded evidence horizon and are compacted afterwards. A
     * quote/authorization/signature/final binding always wins and leaves the
     * slots untouched. */
    bool ExpireProviderCapacityReservations(int64_t now,
                                            size_t& expired_reservations,
                                            std::string& error);

    /** Compact expired client-side capacity snapshots and their matching
     * slot/outpoint indexes after the replay/evidence horizon. Attempts and
     * recovery records retain their exact embedded authorization artifacts;
     * separately persisted equivocation evidence is never removed here. */
    bool CompactClientCapacitySnapshots(int64_t now,
                                        size_t& compacted_snapshots,
                                        std::string& error);

    /** Verify and durably retain two provider-signed quote responses for the
     * same client-side semantic binding, then permanently block that provider
     * locally. Invalid or differently bound artifacts cannot create a block. */
    bool RecordQuoteEquivocation(
        const uint256& attempt_id,
        const std::vector<unsigned char>& conflicting_signed_quote,
        int64_t now,
        std::string& error);
    /** Record two conflicting provider-signed responses received before either
     * quote can become the attempt's durable accepted quote. */
    bool RecordPendingQuoteEquivocation(
        const uint256& attempt_id,
        const std::vector<unsigned char>& first_signed_quote,
        const std::vector<unsigned char>& conflicting_signed_quote,
        int64_t observed_at,
        std::string& error);
    /** Persist the first canonical identity-signed quote claim for this exact
     * client attempt without treating it as a validated quote. Exact retries
     * are idempotent; a different overlapping claim produces durable
     * two-phase evidence and sets equivocation=true. */
    bool StageQuoteResponseClaimCandidate(
        const uint256& attempt_id,
        const std::vector<unsigned char>& signed_quote,
        int64_t observed_at,
        bool& equivocation,
        std::string& error);

    /** Atomically reject a submit whose unsigned template can no longer be
     * mined, release its still-unused provider pool entries, and persist the
     * signed terminal result. This is deliberately separate from quote
     * cancellation: a user authorization may already exist, but no provider
     * signature or durable final commit may exist. */
    bool RejectUnavailableProviderSubmit(
        const uint256& attempt_id,
        const DigiDollar::Paymaster::PaymasterResult& result,
        const uint256& expected_genesis,
        DigiDollar::Paymaster::ProviderAttempt& attempt,
        std::string& error);

    bool AcceptUserAuthorization(
        const std::string& request_id,
        const uint256& attempt_id,
        const uint256& canonical_psbt_hash,
        int64_t now,
        std::string& error);
    bool GetUserAuthorization(
        const uint256& commit_key,
        DigiDollar::Paymaster::UserAuthorizationRecord& authorization) const;

    bool ReserveSponsorshipAuthorization(
        const DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization,
        std::string& error);
    bool GetSponsorshipAuthorization(
        const uint256& capability_hash,
        DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization) const;

    bool CommitProviderFinalTransaction(
        const std::string& request_id,
        const uint256& attempt_id,
        const DigiDollar::Paymaster::ProviderCommitRecord& commit,
        const DigiDollar::Paymaster::PaymasterResult& result,
        const uint256& expected_genesis,
        std::string& error);
    bool CommitProviderFinalTransaction(
        const std::string& request_id,
        const uint256& attempt_id,
        const DigiDollar::Paymaster::ProviderCommitRecord& commit,
        const std::optional<uint256>& sponsorship_capability_hash,
        const DigiDollar::Paymaster::PaymasterResult& result,
        const uint256& expected_genesis,
        std::string& error);
    bool GetProviderCommit(
        const uint256& commit_key,
        DigiDollar::Paymaster::ProviderCommitRecord& commit) const;
    bool ListProviderCommits(
        std::vector<DigiDollar::Paymaster::ProviderCommitRecord>& commits) const;
    /** Idempotently reconstruct wallet-owned pool successors for durable
     * provider commits created by older builds before successor persistence
     * was atomic. */
    bool ReconcileProviderPoolSuccessors(size_t& recovered,
                                         std::string& error);
    /** List provider-side attempts whose complete provider signature and
     * private signed-result envelope are durable but whose atomic commit was
     * interrupted. Malformed individual entries are skipped fail-closed so
     * they cannot block recovery of independent valid attempts. */
    bool ListProviderSignedAttemptsWithoutCommit(
        std::vector<DigiDollar::Paymaster::ProviderAttempt>& attempts) const;

    /** Persist a provider result monotonically. Once a final transaction is
     * durable, every later result must retain its exact TXID, witness and raw
     * transaction. */
    bool StoreProviderResult(const DigiDollar::Paymaster::PaymasterResult& result,
                             const uint256& expected_genesis,
                             std::string& error);
    /** Validate a client result against its durable attempt/session and commit
     * the result plus newly learned final artifacts as one database unit. */
    bool StoreClientResult(const DigiDollar::Paymaster::PaymasterResult& result,
                           const uint256& expected_genesis,
                           const uint256& attempt_id,
                           int64_t now,
                           std::string& error);
    /** Atomically retain recovery authority when an untrusted final result
     * fails validation before it can be stored. The attempt, session,
     * user-input reservations and durable wallet flag advance together.
     * Network-observed and terminal safe states are never regressed. */
    bool MarkClientFinalValidationFailureRecoverable(
        const std::string& request_id,
        const uint256& attempt_id,
        int64_t now,
        std::string& error);
    /** Atomically record that the exact durable final transaction failed the
     * last authorization/preflight/broadcast firewall. The exact final result,
     * transaction bytes, user-input reservations and fee accounting are kept
     * intact so chain reconciliation or explicit recovery remains possible.
     * Already confirmed payments and safely confirmed cancellations are
     * immutable and make this operation an idempotent no-op. */
    bool RecordClientFinalConflict(const std::string& request_id,
                                   const uint256& attempt_id,
                                   const uint256& expected_txid,
                                   const uint256& expected_wtxid,
                                   int64_t now,
                                   std::string& error);
    /** Atomically publish the attempt and session state observed after the
     * exact final transaction has been accepted by the node or chain. */
    bool RecordClientFinalObservation(const std::string& request_id,
                                      const uint256& attempt_id,
                                      bool confirmed,
                                      int64_t now,
                                      std::string& error);
    bool GetProviderResult(const uint256& commit_key,
                           DigiDollar::Paymaster::PaymasterResult& result) const;

    bool CommitSelfRecovery(
        const std::string& request_id,
        const DigiDollar::Paymaster::SelfRecoveryRecord& recovery,
        std::string& error);
    bool GetSelfRecovery(
        const std::string& request_id,
        DigiDollar::Paymaster::SelfRecoveryRecord& recovery) const;

    /** Persist the recovery provider/capacity choice before any DD outpoint or
     * return script is disclosed. Existing USER_DD reservations must belong
     * to the ambiguous original session and remain authorization-risk locked. */
    bool PrepareAlternativeRecovery(
        const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        std::string& error);
    bool UpdateAlternativeRecovery(
        const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        std::string& error);
    /** Atomically bind the exact recovery authorization and reserve its
     * client fee before any USER signing operation is invoked. Exact retries
     * are idempotent; a different commitment or exhausted fee budget fails
     * without changing the recovery record. */
    bool AcceptAlternativeRecoveryAuthorization(
        const std::string& request_id,
        const uint256& authorization_commitment,
        int64_t now,
        std::string& error);
    bool GetAlternativeRecovery(
        const std::string& request_id,
        DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery) const;
    bool GetAlternativeRecoveryById(
        const uint256& recovery_id,
        DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery) const;
    bool ListClientAlternativeRecoveries(
        std::vector<DigiDollar::Paymaster::AlternativeRecoveryRecord>& recoveries,
        std::string& error) const;
    /** Resolve and revalidate the exact provider-side alternative-recovery
     * authority behind a durable commit. This is the startup/retry fallback
     * when no ordinary ProviderAttempt template index exists. */
    bool GetProviderAlternativeRecoveryByCommit(
        const DigiDollar::Paymaster::ProviderCommitRecord& commit,
        DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        std::string& error) const;

    /** Detect durable provider work, including exact unexpired capacity
     * continuations, that must remain drainable even when a safety limit or
     * the corresponding pool reservation prevents announcing new quotes. */
    bool HasProviderDrainWork(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        bool& has_work,
        std::string& error) const;

    /** Atomically expire recovery quotes that never received a user PSBT.
     * Provider pool/budget and client fee reservations are released in the
     * same database transaction. USER_SIGNED/final records are never freed. */
    bool ExpireAlternativeRecoveries(int64_t now,
                                     size_t& expired_recoveries,
                                     std::string& error);

    /** Atomically bind the capacity-reserved provider pool and worst-case DGB
     * budget to an authenticated recovery response. */
    bool CommitProviderAlternativeRecoveryQuote(
        const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        const uint256& netgroup_bucket,
        const uint256& expected_genesis,
        int64_t now,
        std::string& error);

    /** Last read-only database firewall before an alternative recovery
     * provider signs. This rejects any drift in the durable recovery record,
     * promoted Capacity admission, exact pool slots or reserved budget. */
    bool ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
        const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        const uint256& expected_genesis,
        int64_t now,
        std::string& error) const;

    /** Atomically persist the provider's fully signed recovery, consume its
     * safety budget and make the committed pool entries unavailable forever. */
    bool CommitProviderAlternativeRecoveryFinal(
        const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        const DigiDollar::Paymaster::ProviderCommitRecord& commit,
        const DigiDollar::Paymaster::PaymasterResult& result,
        const uint256& expected_genesis,
        std::string& error);

    /** Commit a fully verified remote recovery and bind it to recovery_txid in
     * the original client session in one database transaction. */
    bool CommitClientAlternativeRecoveryFinal(
        const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        const DigiDollar::Paymaster::SelfRecoveryRecord& final_recovery,
        std::string& error);
    /** Mark a durably committed alternative recovery as conflicted after its
     * last pre-broadcast firewall fails. Exact recovery bytes, the spent fee
     * reservation and every original USER_DD lock remain intact. A confirmed
     * payment or CANCELED_SAFE recovery is never overwritten. */
    bool RecordClientAlternativeRecoveryConflict(
        const std::string& request_id,
        const uint256& recovery_id,
        const uint256& expected_txid,
        const uint256& expected_wtxid,
        int64_t now,
        std::string& error);

    bool RecordProviderOutcome(const uint256& attempt_id,
                               DigiDollar::Paymaster::ReliabilityOutcome outcome,
                               int64_t observed_at,
                               int64_t successful_latency_ms,
                               std::string& error);
    bool GetProviderReliability(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        DigiDollar::Paymaster::PaymasterReliabilityRecord& record) const;
    bool ListProviderReliability(
        std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord>& records) const;
    bool ClearProviderReliability(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        std::string& error);
    /** Stage already verified, canonical signed conflict evidence in an
     * independent transaction. The Record*Equivocation entry points perform
     * the required cryptographic and semantic validation before calling this
     * persistence boundary. */
    bool StagePendingEquivocation(
        const DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
        std::string& error);
    /** Atomically promote one staged record to final evidence plus a
     * permanent local provider block. A failed promotion retains Pending. */
    bool PromotePendingEquivocation(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        std::string& error);
    /** Promote every valid staged record, including records recovered after
     * restart before any corresponding Manager message is available. */
    bool PromotePendingEquivocations(std::string& error);
    DatabaseReadStatus GetPendingEquivocation(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
        std::string& error) const;
    bool GetEquivocationEvidence(
        const uint256& evidence_id,
        DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence) const;
    DatabaseReadStatus GetProviderBlock(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        DigiDollar::Paymaster::PaymasterProviderBlock& block,
        std::string& error) const;

    /** Return the one exact final that a client session is currently allowed
     * to retry. A committed cancel-to-self recovery takes precedence over the
     * original provider final because broadcasting both would create a local
     * double-spend race. */
    bool ListClientDurableFinalTransactions(
        std::vector<CTransactionRef>& transactions,
        std::string& error) const;

    /** Reconstruct and revalidate the exact current client authority for one
     * durable final immediately before a startup/retry broadcast. This checks
     * ordinary payment and alternative-recovery manifests, the accepted user
     * PSBT, capacity/control proofs, all final witnesses, and current
     * chainstate. exact_final_already_known may be true only after an exact
     * txid/wtxid/byte match in the active chain or a local transaction pool.
     * A separate mempool preflight remains mandatory immediately before
     * insertion or broadcast. */
    bool ValidateClientDurableFinalForBroadcast(
        const CTransaction& transaction,
        int64_t now,
        bool exact_final_already_known,
        std::string& error) const;

    /** Reconcile every session bound to an exact wallet transaction
     * observation. The complete witness serialization is compared with the
     * durable authorized artifact before any state transition. Confirmation
     * is reversible on reorg; unconfirmed disappearance returns the session
     * to its durable pending-network state. */
    bool ReconcileFinalTransaction(const CTransaction& transaction,
                                   int confirmation_depth,
                                   bool in_mempool,
                                   int64_t now,
                                   std::string& error);

    /** Deterministically re-evaluate all durable payment/recovery finals from
     * the current wallet view. Intended for startup and updated-tip hooks so
     * reorg handling does not depend on notification order. */
    bool ReconcileFinalSessionsAtTip(int64_t now, std::string& error);

    bool PruneFinalSession(const std::string& request_id, std::string& error);

private:
    CWallet& m_wallet;
};

} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERSTORE_H
