// Copyright (c) 2024-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_ORACLE_SIGNING_ORCHESTRATOR_H
#define DIGIBYTE_ORACLE_SIGNING_ORCHESTRATOR_H

#include <chain.h>
#include <key.h>
#include <oracle/musig2_aggregator.h>
#include <oracle/musig2_messages.h>
#include <oracle/musig2_session.h>
#include <validationinterface.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

class CBlock;
class CConnman;

/**
 * MuSig2 Signing Orchestrator
 *
 * Drives the MuSig2 signing protocol on every block tick via
 * CValidationInterface::BlockConnected.
 *
 * Oracle nodes:  generate nonces, create partial sigs, broadcast.
 * Non-oracle:    collect nonces/sigs, aggregate final signature.
 */
class OracleSigningOrchestrator : public CValidationInterface
{
public:
    OracleSigningOrchestrator();
    ~OracleSigningOrchestrator();

    OracleSigningOrchestrator(const OracleSigningOrchestrator&) = delete;
    OracleSigningOrchestrator& operator=(const OracleSigningOrchestrator&) = delete;

    // Oracle detection
    bool IsOracleNode() const;
    const CKey* GetOracleSigningKey() const;
    uint8_t GetOracleId() const;

    // Session management
    MuSig2SigningSession* GetOrCreateSigningSession(int32_t epoch, int32_t block_height = 0);
    uint8_t GetActiveAttemptId(int32_t epoch) const;
    void CleanupOldSessions(int32_t current_epoch);
    /** Read-only session existence check — used by tests and diagnostics. */
    bool HasSession(int32_t epoch) const;

    // Block-tick orchestration
    void OnBlockConnected(const std::shared_ptr<const CBlock>& block, int32_t block_height);

    // P2P broadcast.
    //
    // Each of these returns the number of connected peers it handed the
    // message to. Zero means nobody got it, whether that is because there is
    // no connection manager or because no peer is connected yet. A caller that
    // records a message as sent must check for zero first.
    size_t BroadcastMusigNonce(const OracleMusigNonceMsg& msg);
    size_t BroadcastMusigContext(const OracleMusigContextMsg& msg);
    size_t BroadcastMusigPartialSig(const OracleMusigPartialSigMsg& msg);
    void SetConnman(CConnman* connman) { m_connman = connman; }

    // Query completed session for block assembly
    bool GetCompletedSession(int32_t epoch,
                             std::vector<unsigned char>& aggregate_sig_out,
                             std::vector<unsigned char>& participation_bitmap_out,
                             uint64_t& signed_price_out,
                             int64_t& signed_timestamp_out) const;

    /**
     * Wave 10 (Agent C) — operator/diagnostic visibility into MuSig2 session
     * state for an epoch. Operators reading the RPC surface need to know
     * whether the orchestrator has reached COMPLETE for the current epoch,
     * is stuck waiting for nonces, has timed out (FAILED), or has not yet
     * created a session at all.
     */
    struct SessionStatus {
        MuSig2SessionState state;
        size_t nonce_count;
        size_t partial_sig_count;
        int32_t creation_height;
    };
    std::optional<SessionStatus> GetSessionStateForEpoch(int32_t epoch) const;

    // Utilities
    static int32_t ComputeEpoch(int32_t block_height, int32_t epoch_length);
    static void ComputeOracleMessageHash(int32_t epoch, uint64_t price,
                                         int64_t timestamp,
                                         unsigned char hash32[32]);
    static std::vector<uint8_t> GetConsensusOracleIdsForSigning();

    // Lifecycle
    void Start();
    void Stop();
    void Clear();

    /** Inject a pre-built session (test use only). Takes ownership. */
    void InjectSession(int32_t epoch, std::unique_ptr<MuSig2SigningSession> session);
    /** Test/diagnostic visibility for bounded pending context proposals. */
    size_t GetPendingContextProposalCountForTesting(int32_t epoch) const;

    /** Ingest remote nonce from P2P (called from net_processing). */
    void IngestRemoteNonce(const OracleMusigNonceMsg& msg);
    /** Ingest remote context proposal from P2P (called from net_processing). */
    void IngestRemoteContext(const OracleMusigContextMsg& msg);
    /** Ingest remote partial sig from P2P (called from net_processing). */
    void IngestRemotePartialSig(const OracleMusigPartialSigMsg& msg);

    static OracleSigningOrchestrator& GetInstance();
    //! Build the one orchestrator, fully configured, and publish it. Pass the
    //! connection manager it should broadcast through, or nullptr in a test
    //! that never broadcasts. Call this before networking starts taking peers:
    //! the global is read without a lock from several threads.
    static void Initialize(CConnman* connman);

    /**
     * Stop delivering connected-block notifications to the orchestrator.
     * Does nothing if there is no orchestrator. Call this early in shutdown so
     * no new block work starts, then call Shutdown() once the scheduler thread
     * that delivers those notifications has stopped.
     */
    static void StopBlockNotifications();

    /**
     * Destroy the orchestrator.
     *
     * Only call this once nothing can still be inside it: the scheduler thread
     * that delivers block notifications must have stopped, and the remote
     * procedure call server and the connection manager must be stopped too,
     * because their threads also call in.
     */
    static void Shutdown();

protected:
    void BlockConnected(ChainstateRole role,
                        const std::shared_ptr<const CBlock>& block,
                        const CBlockIndex* pindex) override;

private:
    /**
     * Run one tick of the MuSig2 ceremony for a specific epoch's session.
     * Called by OnBlockConnected for both the current epoch and, in the
     * last K blocks of an epoch, for the upcoming epoch so the ceremony
     * reaches COMPLETE before the first block of the next epoch is
     * templated by miners.
     */
    void TickEpochSession(int32_t epoch, int32_t block_height);
    bool RestartEpochAttemptIfNeeded(int32_t epoch, int32_t block_height, MuSig2SigningSession*& session);
    bool HasSelectionSeedForEpoch(int32_t epoch) const;
    uint256 GetSelectionSeedForEpoch(int32_t epoch) const;
    void SetEpochSelectionSeed(int32_t epoch, const uint256& seed);
    bool IsEligibleContextProposer(int32_t epoch, uint8_t proposer_id, int32_t block_height) const;
    bool ValidateContextProposal(const OracleMusigContextMsg& msg,
                                 MuSig2SigningSession& session) const;
    bool AddNonceEvidenceToSession(const OracleMusigNonceMsg& msg,
                                   MuSig2SigningSession& session) const;
    std::optional<OracleMusigContextMsg> SelectReadyContextProposal(int32_t epoch,
                                                                    int32_t block_height,
                                                                    MuSig2SigningSession& session) const;
    std::optional<OracleMusigContextMsg> BuildLocalContextProposal(int32_t epoch,
                                                                   int32_t block_height,
                                                                   MuSig2SigningSession& session);
    bool TryApplyRemotePartialSig(const OracleMusigPartialSigMsg& msg,
                                  MuSig2SigningSession& session) const;
    void BufferPendingPartialSig(const OracleMusigPartialSigMsg& msg);
    size_t DrainPendingPartialSigsForEpoch(int32_t epoch, MuSig2SigningSession& session);
    /** Send again any nonce of ours for this epoch that no peer has yet. */
    void ResendUnsentNonces(int32_t epoch);
    /** Send our context proposal for this epoch again if no peer has it yet. */
    void ResendUnsentContexts(int32_t epoch);
    /** Send again any partial signature of ours for this epoch that no peer has. */
    void ResendUnsentPartialSigs(int32_t epoch);

    CConnman* m_connman{nullptr};
    std::map<int32_t, std::unique_ptr<MuSig2SigningSession>> m_signing_sessions;
    mutable std::mutex m_sessions_mutex;
    std::unique_ptr<MuSig2OracleAggregator> m_aggregator;
    std::map<int32_t, std::set<uint8_t>> m_nonce_broadcast_tracker;
    std::map<int32_t, std::set<uint8_t>> m_context_broadcast_tracker;
    std::map<int32_t, std::set<uint8_t>> m_partialsig_broadcast_tracker;
    std::map<int32_t, std::map<uint256, OracleMusigContextMsg>> m_pending_contexts;
    std::map<int32_t, uint256> m_epoch_selection_seeds;
    std::map<int32_t, uint8_t> m_epoch_attempts;
    std::map<int32_t, std::map<uint8_t, OracleMusigNonceMsg>> m_nonce_evidence;
    // Our own oracle ids whose nonce for that epoch was generated and stored
    // but has not reached a single peer yet. The session refuses to generate a
    // second nonce for an oracle it already holds one for, so the message kept
    // in m_nonce_evidence is the only copy that can still go out. An id leaves
    // this list as soon as one peer has it.
    std::map<int32_t, std::set<uint8_t>> m_unsent_nonces;
    // The context id of our own proposal for that epoch, when the send reached no
    // peer. The proposal itself stays in m_pending_contexts, so this holds only the
    // key. BuildLocalContextProposal marks the proposer before the send happens and
    // the proposer search skips anyone already marked, so without this the proposal
    // is never offered again.
    std::map<int32_t, uint256> m_unsent_contexts;
    // Our own partial signatures that reached no peer, kept whole. Nothing else
    // keeps them: the local path adds the signature to the session and sends the
    // message without storing a copy, and it cannot be made again because signing
    // consumes and removes the secret nonce.
    std::map<int32_t, std::map<uint8_t, OracleMusigPartialSigMsg>> m_unsent_partialsigs;
    // Buffer context-bound partial sigs that arrive before local session enters SIGNING.
    std::map<int32_t, std::map<uint256, std::vector<OracleMusigPartialSigMsg>>> m_pending_partialsigs;
    mutable std::unique_ptr<CKey> m_cached_oracle_key;
    mutable uint8_t m_cached_oracle_id{255};
    mutable bool m_oracle_key_cached{false};
    bool m_started{false};
};

extern std::unique_ptr<OracleSigningOrchestrator> g_signing_orchestrator;

#endif // DIGIBYTE_ORACLE_SIGNING_ORCHESTRATOR_H
