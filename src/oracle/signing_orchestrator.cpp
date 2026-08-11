// Copyright (c) 2024-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <oracle/signing_orchestrator.h>

#include <chainparams.h>
#include <hash.h>
#include <logging.h>
#include <net.h>
#include <netmessagemaker.h>
#include <oracle/bundle_manager.h>
#include <oracle/node.h>
#include <primitives/oracle.h>
#include <protocol.h>
#include <serialize.h>
#include <util/time.h>

#include <secp256k1.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

std::unique_ptr<OracleSigningOrchestrator> g_signing_orchestrator;

namespace {
constexpr size_t MAX_PENDING_PARTIALSIGS_PER_CONTEXT = 40;   // > any honest oracle count
constexpr size_t MAX_PENDING_PARTIALSIG_CONTEXTS_PER_EPOCH = 8;
constexpr size_t MAX_PENDING_PARTIALSIGS_PER_EPOCH = 128;
constexpr size_t MAX_PENDING_PARTIALSIG_EPOCHS = 8;           // +/- 4 epochs around current
constexpr size_t MAX_PENDING_CONTEXT_PROPOSALS_PER_EPOCH = 32;
constexpr int32_t CONTEXT_PROPOSAL_ROUND_BLOCKS = 2;
constexpr int32_t CONTEXT_NONCE_GRACE_BLOCKS = 2;

int32_t GetEpochStartHeight(int32_t epoch)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    int32_t epoch_length = consensus.nDDOracleEpochBlocks;
    if (epoch_length <= 0) epoch_length = 1440;
    return epoch * epoch_length;
}

std::optional<uint256> ResolveEpochSelectionSeed(const CBlockIndex* pindex, int32_t epoch)
{
    if (!pindex || epoch < 0) return std::nullopt;
    int32_t seed_height = GetEpochStartHeight(epoch) - 1;
    if (seed_height < 0) seed_height = 0;
    if (seed_height > pindex->nHeight) return std::nullopt;
    const CBlockIndex* seed_index = pindex->GetAncestor(seed_height);
    if (!seed_index) return std::nullopt;
    return seed_index->GetBlockHash();
}

std::vector<uint8_t> SortOracleIdsByEpochSeed(std::vector<uint8_t> ids,
                                              int32_t epoch,
                                              const uint256& seed)
{
    std::sort(ids.begin(), ids.end(), [&](uint8_t a, uint8_t b) {
        const uint256 score_a = GetOracleEpochSelectionHash(epoch, a, seed);
        const uint256 score_b = GetOracleEpochSelectionHash(epoch, b, seed);
        if (score_a == score_b) return a < b;
        return score_a < score_b;
    });
    return ids;
}

size_t CountPendingPartialSigsForEpoch(const std::map<uint256, std::vector<OracleMusigPartialSigMsg>>& contexts)
{
    size_t count = 0;
    for (const auto& [context_id, partials] : contexts) {
        count += partials.size();
    }
    return count;
}

uint256 ComputeOracleQuoteSetHash(int32_t epoch,
                                  const std::vector<COraclePriceMessage>& messages)
{
    std::vector<COraclePriceMessage> sorted = messages;
    std::sort(sorted.begin(), sorted.end(), [](const COraclePriceMessage& a,
                                               const COraclePriceMessage& b) {
        return a.oracle_id < b.oracle_id;
    });

    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/MuSig2QuoteSet/v1");
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch;
    for (const COraclePriceMessage& msg : sorted) {
        hasher << msg.oracle_id;
        hasher << msg.price_micro_usd;
        hasher << msg.timestamp;
        hasher << msg.oracle_pubkey;
        hasher << msg.schnorr_sig;
    }
    return hasher.GetHash();
}

bool ComputeConsensusValuesFromEvidence(const std::vector<COraclePriceMessage>& messages,
                                        uint64_t& consensus_price,
                                        int64_t& consensus_timestamp)
{
    if (messages.empty()) return false;

    COracleBundle temp;
    temp.messages = messages;
    const CAmount price = OracleBundleManager::CalculateConsensusPrice(
        temp, Params().GetConsensus());
    if (price <= 0) return false;
    consensus_price = static_cast<uint64_t>(price);

    std::vector<int64_t> timestamps;
    timestamps.reserve(messages.size());
    for (const COraclePriceMessage& msg : messages) {
        timestamps.push_back(msg.timestamp);
    }
    std::sort(timestamps.begin(), timestamps.end());
    const size_t mid = timestamps.size() / 2;
    consensus_timestamp = (timestamps.size() % 2 == 0) ?
        (timestamps[mid - 1] + timestamps[mid]) / 2 : timestamps[mid];
    return true;
}

bool VerifyOraclePriceEvidence(const OracleMusigContextMsg& msg)
{
    if (msg.price_evidence.size() != msg.participant_ids.size()) return false;

    std::set<uint8_t> participants(msg.participant_ids.begin(), msg.participant_ids.end());
    std::set<uint8_t> seen;
    const int64_t now = GetTime();
    for (const COraclePriceMessage& price_msg : msg.price_evidence) {
        if (price_msg.oracle_id > std::numeric_limits<uint8_t>::max()) return false;
        const uint8_t id = static_cast<uint8_t>(price_msg.oracle_id);
        if (!participants.count(id)) return false;
        if (!seen.insert(id).second) return false;
        if (!IsAuthorizedMuSig2OracleIdForRelay(Params(), id)) return false;

        const OracleNodeInfo* oracle_config = Params().GetOracleNode(id);
        if (!oracle_config) return false;
        if (XOnlyPubKey(oracle_config->pubkey) != price_msg.oracle_pubkey) return false;
        if (!price_msg.IsValid(now)) return false;
    }

    if (seen.size() != participants.size()) return false;

    uint64_t evidence_price = 0;
    int64_t evidence_timestamp = 0;
    if (!ComputeConsensusValuesFromEvidence(msg.price_evidence,
                                            evidence_price,
                                            evidence_timestamp)) {
        return false;
    }

    return evidence_price == msg.consensus_price &&
           evidence_timestamp == msg.consensus_timestamp &&
           ComputeOracleQuoteSetHash(msg.epoch, msg.price_evidence) == msg.quote_set_hash;
}
} // namespace

std::vector<uint8_t> OracleSigningOrchestrator::GetConsensusOracleIdsForSigning()
{
    std::vector<uint8_t> oracle_ids;
    const Consensus::Params& consensus = Params().GetConsensus();
    const int active_pubkeys = consensus.nOraclePubkeyCount;
    if (active_pubkeys <= 0) return oracle_ids;

    const auto& nodes = Params().GetOracleNodes();
    oracle_ids.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (!node.is_active) continue;
        // Enforce the consensus keyset bound even if node metadata is larger.
        if (node.id >= static_cast<uint32_t>(active_pubkeys)) continue;
        if (node.id > std::numeric_limits<uint8_t>::max()) continue;
        oracle_ids.push_back(static_cast<uint8_t>(node.id));
    }
    return oracle_ids;
}

// ============================================================================
// Construction / lifecycle
// ============================================================================

OracleSigningOrchestrator::OracleSigningOrchestrator() = default;

OracleSigningOrchestrator::~OracleSigningOrchestrator()
{
    Stop();
}

void OracleSigningOrchestrator::Start()
{
    if (m_started) return;
    RegisterValidationInterface(this);
    m_started = true;
    LogPrintf("Oracle: MuSig2 signing orchestrator started\n");
}

void OracleSigningOrchestrator::Stop()
{
    if (!m_started) return;
    UnregisterValidationInterface(this);
    m_started = false;
    LogPrintf("Oracle: MuSig2 signing orchestrator stopped\n");
}

void OracleSigningOrchestrator::Clear()
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    m_signing_sessions.clear();
    m_nonce_broadcast_tracker.clear();
    m_context_broadcast_tracker.clear();
    m_partialsig_broadcast_tracker.clear();
    m_pending_contexts.clear();
    m_pending_partialsigs.clear();
    m_epoch_selection_seeds.clear();
    m_epoch_attempts.clear();
    m_nonce_evidence.clear();
    m_aggregator.reset();
    m_cached_oracle_key.reset();
    m_cached_oracle_id = 255;
    m_oracle_key_cached = false;
}

void OracleSigningOrchestrator::InjectSession(int32_t epoch, std::unique_ptr<MuSig2SigningSession> session)
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    if (session) {
        m_epoch_attempts[epoch] = static_cast<uint8_t>(session->GetAttemptId());
    }
    m_signing_sessions[epoch] = std::move(session);
}

size_t OracleSigningOrchestrator::GetPendingContextProposalCountForTesting(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    const auto it = m_pending_contexts.find(epoch);
    return it == m_pending_contexts.end() ? 0 : it->second.size();
}

uint8_t OracleSigningOrchestrator::GetActiveAttemptId(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    const auto it = m_epoch_attempts.find(epoch);
    if (it == m_epoch_attempts.end()) return 0;
    return it->second;
}

void OracleSigningOrchestrator::IngestRemoteNonce(const OracleMusigNonceMsg& msg)
{
    // RC30: auto-create a session for this epoch if we don't have one yet.
    // Early-arriving remote nonces used to be dropped as "unknown epoch",
    // which prevented threshold sessions from ever assembling enough nonces
    // when mining is fast. Sessions are tiny so lazy creation is cheap.
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    auto it = m_signing_sessions.find(msg.epoch);
    if (it == m_signing_sessions.end() || !it->second) {
        const auto attempt_it = m_epoch_attempts.find(msg.epoch);
        const uint8_t expected_attempt =
            attempt_it == m_epoch_attempts.end() ? 0 : attempt_it->second;
        if (msg.attempt_id != expected_attempt) return;

        const Consensus::Params& consensus = Params().GetConsensus();
        const uint8_t min_signers = static_cast<uint8_t>(std::max(1, consensus.nOracleConsensusRequired));
        auto session = std::make_unique<MuSig2SigningSession>(msg.epoch, min_signers, msg.attempt_id);
        m_epoch_attempts[msg.epoch] = msg.attempt_id;
        const auto seed_it = m_epoch_selection_seeds.find(msg.epoch);
        if (seed_it != m_epoch_selection_seeds.end() && !seed_it->second.IsNull()) {
            session->SetEpochSelectionSeed(seed_it->second);
        }
        session->SetCreationHeight(GetEpochStartHeight(msg.epoch));
        session->SetTimeoutBlocks(100);
        LogPrintf("Oracle: Lazily created MuSig2 session for epoch %d on remote nonce arrival\n", msg.epoch);
        it = m_signing_sessions.emplace(msg.epoch, std::move(session)).first;
    }
    if (it == m_signing_sessions.end() || !it->second) return;
    if (it->second->GetAttemptId() != msg.attempt_id) return;

    if (AddNonceEvidenceToSession(msg, *it->second)) {
        m_nonce_evidence[msg.epoch][msg.oracle_id] = msg;
        LogPrintf("Oracle: Ingested remote nonce for epoch %d attempt %u from oracle %d\n",
                 msg.epoch, msg.attempt_id, msg.oracle_id);
    }
}

bool OracleSigningOrchestrator::AddNonceEvidenceToSession(const OracleMusigNonceMsg& msg,
                                                          MuSig2SigningSession& session) const
{
    if (!msg.IsValid()) return false;
    if (msg.epoch != session.GetEpoch()) return false;
    if (msg.attempt_id != session.GetAttemptId()) return false;
    if (!IsAuthorizedMuSig2OracleIdForRelay(Params(), msg.oracle_id)) return false;

    const OracleNodeInfo* oracle_config = Params().GetOracleNode(msg.oracle_id);
    if (!oracle_config) return false;
    if (!msg.VerifySignature(XOnlyPubKey(oracle_config->pubkey))) return false;

    if (session.GetState() == MuSig2SessionState::CREATED) {
        std::vector<uint8_t> all_oracle_ids = GetConsensusOracleIdsForSigning();

        MuSig2OracleAggregator aggregator;
        secp256k1_xonly_pubkey agg_pk;
        secp256k1_musig_keyagg_cache cache;
        if (!aggregator.ComputeAggregatePubkey(all_oracle_ids, agg_pk, cache) ||
            !session.InitializePassive(cache)) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Failed to initialize passive MuSig2 session for epoch %d on remote nonce arrival\n",
                     msg.epoch);
            return false;
        }
    }

    // Deserialize pubnonce
    if (msg.pubnonce.size() != 66) return false;
    secp256k1_musig_pubnonce pubnonce;
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    bool accepted = false;
    if (secp256k1_musig_pubnonce_parse(ctx, &pubnonce, msg.pubnonce.data())) {
        accepted = session.AddPubnonce(msg.oracle_id, pubnonce);
    }
    secp256k1_context_destroy(ctx);
    if (accepted) return true;
    const std::vector<uint8_t> participants = session.GetNonceParticipants();
    return std::find(participants.begin(), participants.end(), msg.oracle_id) != participants.end();
}

void OracleSigningOrchestrator::IngestRemoteContext(const OracleMusigContextMsg& msg)
{
    if (!msg.IsValid()) return;

    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    const auto seed_it = m_epoch_selection_seeds.find(msg.epoch);
    const bool have_local_seed =
        seed_it != m_epoch_selection_seeds.end() && !seed_it->second.IsNull();
    if (have_local_seed && seed_it->second != msg.epoch_selection_seed) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Rejected MuSig2 context proposal epoch=%d proposer=%u: seed mismatch msg=%s local=%s\n",
                 msg.epoch, msg.proposer_id, msg.epoch_selection_seed.ToString(),
                 seed_it->second.ToString());
        return;
    }

    auto it = m_signing_sessions.find(msg.epoch);
    if (it == m_signing_sessions.end() || !it->second) {
        const auto attempt_it = m_epoch_attempts.find(msg.epoch);
        const uint8_t expected_attempt =
            attempt_it == m_epoch_attempts.end() ? 0 : attempt_it->second;
        if (msg.attempt_id != expected_attempt) return;

        const Consensus::Params& consensus = Params().GetConsensus();
        const uint8_t min_signers = static_cast<uint8_t>(std::max(1, consensus.nOracleConsensusRequired));
        auto session = std::make_unique<MuSig2SigningSession>(msg.epoch, min_signers, msg.attempt_id);
        m_epoch_attempts[msg.epoch] = msg.attempt_id;
        if (have_local_seed) {
            session->SetEpochSelectionSeed(seed_it->second);
        }
        session->SetCreationHeight(GetEpochStartHeight(msg.epoch));
        session->SetTimeoutBlocks(100);
        LogPrintf("Oracle: Lazily created MuSig2 session for epoch %d on context proposal arrival\n", msg.epoch);
        it = m_signing_sessions.emplace(msg.epoch, std::move(session)).first;
    } else if (have_local_seed) {
        it->second->SetEpochSelectionSeed(seed_it->second);
    }
    if (it == m_signing_sessions.end() || !it->second) return;
    if (it->second->GetAttemptId() != msg.attempt_id) return;

    for (const OracleMusigNonceMsg& nonce_msg : msg.nonce_evidence) {
        if (!AddNonceEvidenceToSession(nonce_msg, *it->second)) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Rejected MuSig2 context epoch=%d proposer=%u: invalid nonce evidence oracle=%u\n",
                     msg.epoch, msg.proposer_id, nonce_msg.oracle_id);
            return;
        }
        m_nonce_evidence[msg.epoch][nonce_msg.oracle_id] = nonce_msg;
    }

    auto& epoch_contexts = m_pending_contexts[msg.epoch];
    for (auto context_it = epoch_contexts.begin(); context_it != epoch_contexts.end(); ) {
        if (context_it->second.proposer_id == msg.proposer_id) {
            context_it = epoch_contexts.erase(context_it);
        } else {
            ++context_it;
        }
    }
    if (epoch_contexts.size() >= MAX_PENDING_CONTEXT_PROPOSALS_PER_EPOCH) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Dropping MuSig2 context proposal epoch=%d proposer=%u context=%s: pending context cap reached (%zu)\n",
                 msg.epoch, msg.proposer_id, msg.session_context_id.ToString(),
                 epoch_contexts.size());
        return;
    }
    epoch_contexts[msg.session_context_id] = msg;
    if (m_pending_contexts.size() > MAX_PENDING_PARTIALSIG_EPOCHS) {
        m_pending_contexts.erase(m_pending_contexts.begin());
    }
    LogPrint(BCLog::DIGIDOLLAR,
             "Oracle: Stored MuSig2 context proposal epoch=%d proposer=%u context=%s participants=%zu\n",
             msg.epoch, msg.proposer_id, msg.session_context_id.ToString(),
             msg.participant_ids.size());
}

void OracleSigningOrchestrator::IngestRemotePartialSig(const OracleMusigPartialSigMsg& msg)
{
    if (msg.context_version != ORACLE_MUSIG2_SESSION_CONTEXT_VERSION ||
        msg.session_context_id.IsNull()) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Ignoring MuSig2 partial sig for epoch %d oracle %u without valid session context\n",
                 msg.epoch, msg.oracle_id);
        return;
    }

    // RC30: auto-create the session so partial sigs arriving from faster peers
    // (who already progressed past NONCES_COMPLETE) aren't lost. Session is
    // also needed to hold a pending buffer if local side is still in
    // CREATED/NONCES_COLLECTING.
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    auto it = m_signing_sessions.find(msg.epoch);
    if (it == m_signing_sessions.end() || !it->second) {
        const auto attempt_it = m_epoch_attempts.find(msg.epoch);
        const uint8_t expected_attempt =
            attempt_it == m_epoch_attempts.end() ? 0 : attempt_it->second;
        if (msg.attempt_id != expected_attempt) return;

        const Consensus::Params& consensus = Params().GetConsensus();
        const uint8_t min_signers = static_cast<uint8_t>(std::max(1, consensus.nOracleConsensusRequired));
        auto session = std::make_unique<MuSig2SigningSession>(msg.epoch, min_signers, msg.attempt_id);
        m_epoch_attempts[msg.epoch] = msg.attempt_id;
        const auto seed_it = m_epoch_selection_seeds.find(msg.epoch);
        if (seed_it != m_epoch_selection_seeds.end() && !seed_it->second.IsNull()) {
            session->SetEpochSelectionSeed(seed_it->second);
        }
        session->SetCreationHeight(GetEpochStartHeight(msg.epoch));
        session->SetTimeoutBlocks(100);
        LogPrintf("Oracle: Lazily created MuSig2 session for epoch %d on remote partial sig arrival\n", msg.epoch);
        it = m_signing_sessions.emplace(msg.epoch, std::move(session)).first;
    }
    if (it == m_signing_sessions.end() || !it->second) return;
    if (it->second->GetAttemptId() != msg.attempt_id) return;

    if (TryApplyRemotePartialSig(msg, *it->second)) {
        LogPrintf("Oracle: Ingested remote partial sig for epoch %d from oracle %d\n",
                 msg.epoch, msg.oracle_id);

        // Auto-aggregate if threshold met
        if (it->second->HasEnoughPartialSigs()) {
            std::vector<unsigned char> final_sig;
            if (it->second->AggregateSignature(final_sig)) {
                LogPrintf("Oracle: MuSig2 auto-aggregated for epoch %d after remote partial sig, sig size=%zu\n",
                         msg.epoch, final_sig.size());
            }
        }
    } else if (it->second->GetState() != MuSig2SessionState::SIGNING &&
               it->second->GetState() != MuSig2SessionState::COMPLETE &&
               it->second->GetState() != MuSig2SessionState::FAILED) {
        BufferPendingPartialSig(msg);
    }
}

bool OracleSigningOrchestrator::TryApplyRemotePartialSig(const OracleMusigPartialSigMsg& msg,
                                                         MuSig2SigningSession& session) const
{
    if (msg.partial_sig.size() != 32) return false;
    if (msg.attempt_id != session.GetAttemptId()) return false;
    if (msg.context_version != ORACLE_MUSIG2_SESSION_CONTEXT_VERSION ||
        msg.session_context_id.IsNull()) return false;
    const uint256 session_context = session.GetSessionContextId();
    if (session_context.IsNull() || msg.session_context_id != session_context) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Rejected MuSig2 partial sig for epoch %d oracle %u: context mismatch msg=%s session=%s\n",
                 msg.epoch, msg.oracle_id, msg.session_context_id.ToString(),
                 session_context.ToString());
        return false;
    }

    secp256k1_musig_partial_sig partial_sig;
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    bool verified_and_added = false;
    if (secp256k1_musig_partial_sig_parse(ctx, &partial_sig, msg.partial_sig.data())) {
        const OracleNodeInfo* oracle_cfg = Params().GetOracleNode(msg.oracle_id);
        if (oracle_cfg) {
            secp256k1_pubkey signer_pk;
            if (secp256k1_ec_pubkey_parse(ctx, &signer_pk,
                                          oracle_cfg->pubkey.data(),
                                          oracle_cfg->pubkey.size())) {
                verified_and_added = session.AddPartialSignatureVerified(
                    msg.oracle_id, partial_sig, signer_pk);
            }
        }
    }

    secp256k1_context_destroy(ctx);
    return verified_and_added;
}

void OracleSigningOrchestrator::BufferPendingPartialSig(const OracleMusigPartialSigMsg& msg)
{
    // Keep early partial sig replay bounded. Honest epochs need one context
    // with at most one message per oracle. Context-level and epoch-level caps
    // prevent a peer from bypassing the per-context limit by grinding random
    // session_context_id values.
    auto& epoch_contexts = m_pending_partialsigs[msg.epoch];
    auto context_it = epoch_contexts.find(msg.session_context_id);
    if (context_it == epoch_contexts.end()) {
        const size_t total_pending = CountPendingPartialSigsForEpoch(epoch_contexts);
        if (epoch_contexts.size() >= MAX_PENDING_PARTIALSIG_CONTEXTS_PER_EPOCH ||
            total_pending >= MAX_PENDING_PARTIALSIGS_PER_EPOCH) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Dropping buffered partial sig for epoch %d oracle %d context=%s: pending epoch cap reached (contexts=%zu total=%zu)\n",
                     msg.epoch, msg.oracle_id, msg.session_context_id.ToString(),
                     epoch_contexts.size(), total_pending);
            return;
        }
        context_it = epoch_contexts.emplace(msg.session_context_id, std::vector<OracleMusigPartialSigMsg>{}).first;
    }

    auto& context_buf = context_it->second;
    auto existing = std::find_if(context_buf.begin(), context_buf.end(),
                                 [&msg](const OracleMusigPartialSigMsg& pending) {
                                     return pending.oracle_id == msg.oracle_id;
                                 });
    if (existing != context_buf.end()) {
        *existing = msg;
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Replaced buffered partial sig for epoch %d oracle %d context=%s (session not SIGNING yet)\n",
                 msg.epoch, msg.oracle_id, msg.session_context_id.ToString());
        return;
    }

    const size_t total_pending = CountPendingPartialSigsForEpoch(epoch_contexts);
    if (context_buf.size() >= MAX_PENDING_PARTIALSIGS_PER_CONTEXT ||
        total_pending >= MAX_PENDING_PARTIALSIGS_PER_EPOCH) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Dropping buffered partial sig for epoch %d oracle %d context=%s: pending context/epoch cap reached (context=%zu total=%zu)\n",
                 msg.epoch, msg.oracle_id, msg.session_context_id.ToString(),
                 context_buf.size(), total_pending);
        return;
    }

    context_buf.push_back(msg);
    if (m_pending_partialsigs.size() > MAX_PENDING_PARTIALSIG_EPOCHS) {
        m_pending_partialsigs.erase(m_pending_partialsigs.begin());
    }
    LogPrint(BCLog::DIGIDOLLAR,
             "Oracle: Buffered partial sig for epoch %d oracle %d context=%s (session not SIGNING yet)\n",
             msg.epoch, msg.oracle_id, msg.session_context_id.ToString());
}

size_t OracleSigningOrchestrator::DrainPendingPartialSigsForEpoch(int32_t epoch,
                                                                  MuSig2SigningSession& session)
{
    if (session.GetState() != MuSig2SessionState::SIGNING) return 0;
    const uint256 session_context = session.GetSessionContextId();
    if (session_context.IsNull()) return 0;

    std::vector<OracleMusigPartialSigMsg> pending;
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        auto it = m_pending_partialsigs.find(epoch);
        if (it == m_pending_partialsigs.end()) return 0;
        auto context_it = it->second.find(session_context);
        if (context_it == it->second.end()) return 0;
        pending = std::move(context_it->second);
        it->second.erase(context_it);
        if (it->second.empty()) {
            m_pending_partialsigs.erase(it);
        }
    }

    size_t accepted = 0;
    for (const OracleMusigPartialSigMsg& msg : pending) {
        if (TryApplyRemotePartialSig(msg, session)) {
            ++accepted;
        }
    }

    if (accepted > 0) {
        LogPrintf("Oracle: Replayed %zu pending partial sigs for epoch %d\n", accepted, epoch);
    }
    return accepted;
}

bool OracleSigningOrchestrator::IsEligibleContextProposer(int32_t epoch,
                                                          uint8_t proposer_id,
                                                          int32_t block_height) const
{
    std::vector<uint8_t> order = SortOracleIdsByEpochSeed(
        GetConsensusOracleIdsForSigning(), epoch, GetSelectionSeedForEpoch(epoch));
    const auto it = std::find(order.begin(), order.end(), proposer_id);
    if (it == order.end()) return false;

    int32_t round_start_height = GetEpochStartHeight(epoch) - 1;
    if (round_start_height < 0) round_start_height = 0;
    if (block_height < round_start_height) return false;

    const int32_t elapsed = block_height - round_start_height;
    const size_t max_rank = static_cast<size_t>(elapsed / CONTEXT_PROPOSAL_ROUND_BLOCKS);
    const size_t proposer_rank = static_cast<size_t>(std::distance(order.begin(), it));
    return proposer_rank <= max_rank;
}

bool OracleSigningOrchestrator::ValidateContextProposal(const OracleMusigContextMsg& msg,
                                                        MuSig2SigningSession& session) const
{
    if (!msg.IsValid()) return false;
    if (msg.epoch != session.GetEpoch()) return false;
    if (msg.attempt_id != session.GetAttemptId()) return false;
    if (msg.epoch_selection_seed != session.GetEpochSelectionSeed()) return false;

    const OracleNodeInfo* proposer_config = Params().GetOracleNode(msg.proposer_id);
    if (!proposer_config) return false;
    XOnlyPubKey proposer_pubkey(proposer_config->pubkey);
    if (!msg.VerifySignature(proposer_pubkey)) return false;

    const Consensus::Params& consensus = Params().GetConsensus();
    if (msg.participant_ids.size() != static_cast<size_t>(consensus.nOracleConsensusRequired)) {
        return false;
    }

    std::set<uint8_t> unique_ids(msg.participant_ids.begin(), msg.participant_ids.end());
    if (unique_ids.size() != msg.participant_ids.size()) return false;
    for (uint8_t id : msg.participant_ids) {
        if (!IsAuthorizedMuSig2OracleIdForRelay(Params(), id)) return false;
    }

    std::vector<uint8_t> sorted_by_seed = SortOracleIdsByEpochSeed(
        msg.participant_ids, msg.epoch, msg.epoch_selection_seed);
    if (sorted_by_seed != msg.participant_ids) return false;
    if (!VerifyOraclePriceEvidence(msg)) return false;

    if (msg.nonce_evidence.size() < msg.participant_ids.size()) return false;
    std::set<uint8_t> nonce_seen;
    std::vector<uint8_t> evidence_ids;
    evidence_ids.reserve(msg.nonce_evidence.size());
    for (const OracleMusigNonceMsg& nonce_msg : msg.nonce_evidence) {
        if (nonce_msg.epoch != msg.epoch || nonce_msg.attempt_id != msg.attempt_id) return false;
        if (!nonce_seen.insert(nonce_msg.oracle_id).second) return false;
        if (!IsAuthorizedMuSig2OracleIdForRelay(Params(), nonce_msg.oracle_id)) return false;
        const OracleNodeInfo* oracle_config = Params().GetOracleNode(nonce_msg.oracle_id);
        if (!oracle_config || !nonce_msg.VerifySignature(XOnlyPubKey(oracle_config->pubkey))) {
            return false;
        }
        evidence_ids.push_back(nonce_msg.oracle_id);
    }
    const std::vector<uint8_t> expected_participants =
        SortOracleIdsByEpochSeed(evidence_ids, msg.epoch, msg.epoch_selection_seed);
    if (expected_participants.size() < msg.participant_ids.size()) return false;
    if (!std::equal(msg.participant_ids.begin(), msg.participant_ids.end(),
                    expected_participants.begin())) {
        return false;
    }
    if (session.GetNonceCount() >= msg.participant_ids.size() &&
        !session.MatchesRequiredParticipants(msg.participant_ids)) {
        return false;
    }

    unsigned char msg32[32];
    ComputeOracleMessageHash(msg.epoch, msg.consensus_price, msg.consensus_timestamp, msg32);
    uint256 nonce_set_hash;
    uint256 context_id;
    if (!session.ComputeContextIdForParticipants(msg.participant_ids, msg32,
                                                 nonce_set_hash, context_id)) {
        return false;
    }
    return nonce_set_hash == msg.nonce_set_hash &&
           context_id == msg.session_context_id;
}

std::optional<OracleMusigContextMsg> OracleSigningOrchestrator::SelectReadyContextProposal(
    int32_t epoch,
    int32_t block_height,
    MuSig2SigningSession& session) const
{
    std::vector<OracleMusigContextMsg> candidates;
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        const auto epoch_it = m_pending_contexts.find(epoch);
        if (epoch_it == m_pending_contexts.end()) return std::nullopt;
        for (const auto& [context_id, msg] : epoch_it->second) {
            candidates.push_back(msg);
        }
    }

    const uint256 seed = session.GetEpochSelectionSeed();
    std::vector<uint8_t> proposer_order = SortOracleIdsByEpochSeed(
        GetConsensusOracleIdsForSigning(), epoch, seed);

    std::optional<OracleMusigContextMsg> best;
    size_t best_rank = std::numeric_limits<size_t>::max();
    for (const OracleMusigContextMsg& msg : candidates) {
        if (!IsEligibleContextProposer(epoch, msg.proposer_id, block_height)) continue;
        if (!ValidateContextProposal(msg, session)) continue;

        const auto rank_it = std::find(proposer_order.begin(), proposer_order.end(), msg.proposer_id);
        if (rank_it == proposer_order.end()) continue;
        const size_t rank = static_cast<size_t>(std::distance(proposer_order.begin(), rank_it));
        if (!best || rank < best_rank ||
            (rank == best_rank && msg.session_context_id < best->session_context_id)) {
            best = msg;
            best_rank = rank;
        }
    }
    return best;
}

std::optional<OracleMusigContextMsg> OracleSigningOrchestrator::BuildLocalContextProposal(
    int32_t epoch,
    int32_t block_height,
    MuSig2SigningSession& session)
{
    OracleManager& om = OracleManager::GetInstance();
    std::vector<uint32_t> local_ids = om.GetActiveOracleIds();
    if (local_ids.empty()) return std::nullopt;

    const uint256 seed = GetSelectionSeedForEpoch(epoch);
    session.SetEpochSelectionSeed(seed);

    std::vector<uint8_t> proposer_order = SortOracleIdsByEpochSeed(
        GetConsensusOracleIdsForSigning(), epoch, seed);

    uint8_t proposer_id = 255;
    OracleNode* proposer_node = nullptr;
    for (uint8_t candidate : proposer_order) {
        if (!IsEligibleContextProposer(epoch, candidate, block_height)) continue;
        {
            std::lock_guard<std::mutex> lock(m_sessions_mutex);
            const auto tracker_it = m_context_broadcast_tracker.find(epoch);
            if (tracker_it != m_context_broadcast_tracker.end() &&
                tracker_it->second.count(candidate)) {
                continue;
            }
        }
        for (uint32_t local_id : local_ids) {
            if (local_id != candidate) continue;
            OracleNode* node = om.GetOracleNode(local_id);
            if (!node) continue;
            proposer_id = candidate;
            proposer_node = node;
            break;
        }
        if (proposer_node) break;
    }
    if (!proposer_node || proposer_id == 255) return std::nullopt;

    std::vector<uint8_t> participant_ids = session.GetRequiredParticipants();
    const Consensus::Params& consensus = Params().GetConsensus();
    if (participant_ids.size() != static_cast<size_t>(consensus.nOracleConsensusRequired)) {
        return std::nullopt;
    }

    std::vector<COraclePriceMessage> price_evidence;
    if (!g_oracle_bundle_manager) {
        return std::nullopt;
    }
    {
        const std::vector<COraclePriceMessage> pending =
            OracleBundleManager::GetInstance().GetPendingMessages();
        std::map<uint32_t, COraclePriceMessage> by_id;
        for (const COraclePriceMessage& pending_msg : pending) {
            by_id[pending_msg.oracle_id] = pending_msg;
        }
        for (uint8_t participant_id : participant_ids) {
            const auto it = by_id.find(participant_id);
            if (it == by_id.end()) return std::nullopt;
            price_evidence.push_back(it->second);
        }
    }

    uint64_t consensus_price = 0;
    int64_t consensus_timestamp = 0;
    if (!ComputeConsensusValuesFromEvidence(price_evidence,
                                            consensus_price,
                                            consensus_timestamp)) {
        return std::nullopt;
    }

    std::vector<OracleMusigNonceMsg> nonce_evidence;
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        const auto epoch_it = m_nonce_evidence.find(epoch);
        if (epoch_it == m_nonce_evidence.end()) return std::nullopt;
        for (uint8_t participant_id : participant_ids) {
            const auto it = epoch_it->second.find(participant_id);
            if (it == epoch_it->second.end()) return std::nullopt;
            if (it->second.attempt_id != session.GetAttemptId()) return std::nullopt;
            nonce_evidence.push_back(it->second);
        }
    }

    unsigned char msg32[32];
    ComputeOracleMessageHash(epoch, consensus_price, consensus_timestamp, msg32);
    uint256 nonce_set_hash;
    uint256 context_id;
    if (!session.ComputeContextIdForParticipants(participant_ids, msg32,
                                                 nonce_set_hash, context_id)) {
        return std::nullopt;
    }

    CKey key = proposer_node->GetOraclePrivateKey();
    if (!key.IsValid()) return std::nullopt;

    OracleMusigContextMsg msg;
    msg.epoch = epoch;
    msg.attempt_id = static_cast<uint8_t>(session.GetAttemptId());
    msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
    msg.epoch_selection_seed = seed;
    msg.proposer_id = proposer_id;
    msg.participant_ids = participant_ids;
    msg.nonce_set_hash = nonce_set_hash;
    msg.quote_set_hash = ComputeOracleQuoteSetHash(epoch, price_evidence);
    msg.consensus_price = consensus_price;
    msg.consensus_timestamp = consensus_timestamp;
    msg.session_context_id = context_id;
    msg.nonce_evidence = nonce_evidence;
    msg.price_evidence = price_evidence;
    if (!msg.Sign(key)) return std::nullopt;

    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        m_pending_contexts[epoch][msg.session_context_id] = msg;
        m_context_broadcast_tracker[epoch].insert(proposer_id);
    }
    return msg;
}

OracleSigningOrchestrator& OracleSigningOrchestrator::GetInstance()
{
    assert(g_signing_orchestrator);
    return *g_signing_orchestrator;
}

void OracleSigningOrchestrator::Initialize()
{
    assert(!g_signing_orchestrator);
    g_signing_orchestrator = std::make_unique<OracleSigningOrchestrator>();
    g_signing_orchestrator->Start();
    LogPrintf("Oracle: MuSig2 signing orchestrator initialized and started\n");
}

void OracleSigningOrchestrator::Shutdown()
{
    if (g_signing_orchestrator) {
        g_signing_orchestrator->Stop();
        // Stop() only unregisters, which is non-blocking and can return while a
        // notification is still in flight on the scheduler thread. Drain the
        // queue before destroying the object that callback is about to touch,
        // otherwise it locks m_sessions_mutex on freed memory: the scheduler
        // thread then blocks forever (and Shutdown() blocks behind it in
        // CScheduler::stop) or aborts, depending on the C++ runtime.
        SyncWithValidationInterfaceQueue();
        g_signing_orchestrator.reset();
    }
}

// ============================================================================
// Oracle detection
// ============================================================================

bool OracleSigningOrchestrator::IsOracleNode() const
{
    if (!g_oracle_manager) return false;
    OracleManager& om = OracleManager::GetInstance();
    return om.GetActiveOracleCount() > 0;
}

const CKey* OracleSigningOrchestrator::GetOracleSigningKey() const
{
    if (m_oracle_key_cached) {
        return m_cached_oracle_key ? m_cached_oracle_key.get() : nullptr;
    }

    m_oracle_key_cached = true;
    if (!g_oracle_manager) {
        m_cached_oracle_key.reset();
        return nullptr;
    }

    OracleManager& om = OracleManager::GetInstance();
    auto ids = om.GetActiveOracleIds();
    if (ids.empty()) {
        m_cached_oracle_key.reset();
        return nullptr;
    }

    OracleNode* node = om.GetOracleNode(ids[0]);
    if (!node) {
        m_cached_oracle_key.reset();
        return nullptr;
    }

    CKey key = node->GetOraclePrivateKey();
    if (!key.IsValid()) {
        m_cached_oracle_key.reset();
        return nullptr;
    }

    m_cached_oracle_id = static_cast<uint8_t>(ids[0]);
    m_cached_oracle_key = std::make_unique<CKey>(key);
    return m_cached_oracle_key.get();
}

uint8_t OracleSigningOrchestrator::GetOracleId() const
{
    if (!m_oracle_key_cached) {
        GetOracleSigningKey();
    }
    return m_cached_oracle_id;
}

// ============================================================================
// Session management
// ============================================================================

MuSig2SigningSession* OracleSigningOrchestrator::GetOrCreateSigningSession(int32_t epoch, int32_t block_height)
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);

    auto it = m_signing_sessions.find(epoch);
    if (it != m_signing_sessions.end()) {
        return it->second.get();
    }

    const Consensus::Params& consensus = Params().GetConsensus();
    const uint8_t min_signers = static_cast<uint8_t>(std::max(1, consensus.nOracleConsensusRequired));
    const uint8_t attempt_id = m_epoch_attempts[epoch];
    auto session = std::make_unique<MuSig2SigningSession>(
        epoch, min_signers, attempt_id);
    const auto seed_it = m_epoch_selection_seeds.find(epoch);
    if (seed_it != m_epoch_selection_seeds.end() && !seed_it->second.IsNull()) {
        session->SetEpochSelectionSeed(seed_it->second);
    }
    session->SetCreationHeight(block_height > 0 ? block_height : GetEpochStartHeight(epoch));
    session->SetTimeoutBlocks(100);

    MuSig2SigningSession* ptr = session.get();
    m_signing_sessions[epoch] = std::move(session);

    LogPrintf("Oracle: Created MuSig2 signing session for epoch %d (creation_height=%d)\n",
             epoch, block_height);
    return ptr;
}

bool OracleSigningOrchestrator::HasSession(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    return m_signing_sessions.find(epoch) != m_signing_sessions.end();
}

bool OracleSigningOrchestrator::RestartEpochAttemptIfNeeded(int32_t epoch,
                                                            int32_t block_height,
                                                            MuSig2SigningSession*& session)
{
    if (!session || session->GetState() != MuSig2SessionState::FAILED) return false;

    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    const uint8_t current_attempt = m_epoch_attempts[epoch];
    static constexpr uint8_t MAX_MUSIG2_ATTEMPTS_PER_EPOCH = 8;
    if (current_attempt + 1 >= MAX_MUSIG2_ATTEMPTS_PER_EPOCH) return false;

    const uint8_t next_attempt = static_cast<uint8_t>(current_attempt + 1);
    m_signing_sessions.erase(epoch);
    m_nonce_broadcast_tracker.erase(epoch);
    m_context_broadcast_tracker.erase(epoch);
    m_partialsig_broadcast_tracker.erase(epoch);
    m_pending_contexts.erase(epoch);
    m_pending_partialsigs.erase(epoch);
    m_nonce_evidence.erase(epoch);
    m_epoch_attempts[epoch] = next_attempt;

    const Consensus::Params& consensus = Params().GetConsensus();
    const uint8_t min_signers = static_cast<uint8_t>(std::max(1, consensus.nOracleConsensusRequired));
    auto replacement = std::make_unique<MuSig2SigningSession>(epoch, min_signers, next_attempt);
    const auto seed_it = m_epoch_selection_seeds.find(epoch);
    if (seed_it != m_epoch_selection_seeds.end() && !seed_it->second.IsNull()) {
        replacement->SetEpochSelectionSeed(seed_it->second);
    }
    replacement->SetCreationHeight(block_height > 0 ? block_height : GetEpochStartHeight(epoch));
    replacement->SetTimeoutBlocks(100);
    session = replacement.get();
    m_signing_sessions[epoch] = std::move(replacement);

    LogPrintf("Oracle: Restarted MuSig2 epoch %d with fresh attempt %u at height %d\n",
              epoch, next_attempt, block_height);
    return true;
}

uint256 OracleSigningOrchestrator::GetSelectionSeedForEpoch(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    const auto it = m_epoch_selection_seeds.find(epoch);
    if (it != m_epoch_selection_seeds.end() && !it->second.IsNull()) {
        return it->second;
    }
    return Params().GetConsensus().hashGenesisBlock;
}

bool OracleSigningOrchestrator::HasSelectionSeedForEpoch(int32_t epoch) const
{
    if (epoch == 0) return true;
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    const auto it = m_epoch_selection_seeds.find(epoch);
    return it != m_epoch_selection_seeds.end() && !it->second.IsNull();
}

void OracleSigningOrchestrator::SetEpochSelectionSeed(int32_t epoch, const uint256& seed)
{
    if (seed.IsNull()) return;
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    m_epoch_selection_seeds[epoch] = seed;
    const auto it = m_signing_sessions.find(epoch);
    if (it != m_signing_sessions.end() && it->second) {
        it->second->SetEpochSelectionSeed(seed);
    }
}

void OracleSigningOrchestrator::CleanupOldSessions(int32_t current_epoch)
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);

    for (auto it = m_signing_sessions.begin(); it != m_signing_sessions.end(); ) {
        if (it->first < current_epoch - 2) {
            it = m_signing_sessions.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_nonce_broadcast_tracker.begin(); it != m_nonce_broadcast_tracker.end(); ) {
        it = (it->first < current_epoch - 2) ? m_nonce_broadcast_tracker.erase(it) : std::next(it);
    }
    for (auto it = m_context_broadcast_tracker.begin(); it != m_context_broadcast_tracker.end(); ) {
        it = (it->first < current_epoch - 2) ? m_context_broadcast_tracker.erase(it) : std::next(it);
    }
    for (auto it = m_partialsig_broadcast_tracker.begin(); it != m_partialsig_broadcast_tracker.end(); ) {
        it = (it->first < current_epoch - 2) ? m_partialsig_broadcast_tracker.erase(it) : std::next(it);
    }
    for (auto it = m_pending_contexts.begin(); it != m_pending_contexts.end(); ) {
        it = (it->first < current_epoch - 2) ? m_pending_contexts.erase(it) : std::next(it);
    }
    // W6-H-01: prune stale buffered partial sigs along with sessions.
    for (auto it = m_pending_partialsigs.begin(); it != m_pending_partialsigs.end(); ) {
        it = (it->first < current_epoch - 2) ? m_pending_partialsigs.erase(it) : std::next(it);
    }
    for (auto it = m_epoch_selection_seeds.begin(); it != m_epoch_selection_seeds.end(); ) {
        it = (it->first < current_epoch - 2) ? m_epoch_selection_seeds.erase(it) : std::next(it);
    }
    for (auto it = m_epoch_attempts.begin(); it != m_epoch_attempts.end(); ) {
        it = (it->first < current_epoch - 2) ? m_epoch_attempts.erase(it) : std::next(it);
    }
    for (auto it = m_nonce_evidence.begin(); it != m_nonce_evidence.end(); ) {
        it = (it->first < current_epoch - 2) ? m_nonce_evidence.erase(it) : std::next(it);
    }
}

// ============================================================================
// ValidationInterface callback
// ============================================================================

void OracleSigningOrchestrator::BlockConnected(
    ChainstateRole role,
    const std::shared_ptr<const CBlock>& block,
    const CBlockIndex* pindex)
{
    if (role != ChainstateRole::NORMAL) return;
    if (!pindex) return;
    const int32_t current_epoch = GetCurrentEpoch(pindex->nHeight);
    if (const auto seed = ResolveEpochSelectionSeed(pindex, current_epoch)) {
        SetEpochSelectionSeed(current_epoch, *seed);
    }
    if (const auto seed = ResolveEpochSelectionSeed(pindex, current_epoch + 1)) {
        SetEpochSelectionSeed(current_epoch + 1, *seed);
    }
    OnBlockConnected(block, pindex->nHeight);
}

// ============================================================================
// Block-tick orchestration
// ============================================================================

void OracleSigningOrchestrator::OnBlockConnected(
    const std::shared_ptr<const CBlock>& /*block*/,
    int32_t block_height)
{
    const int32_t current_epoch = GetCurrentEpoch(block_height);

    CleanupOldSessions(current_epoch);

    TickEpochSession(current_epoch, block_height);

    // Pre-start next epoch's ceremony in the tail of the current one.
    //
    // The MuSig2 ceremony needs ~3 block ticks to reach COMPLETE
    // (nonce -> partial-sig -> aggregate). Without pre-start, the
    // session for epoch N+1 is only created when block (N+1)*L
    // first connects, which can leave the next block template without
    // the mandatory v0x03 bundle. Starting the ceremony K blocks early
    // lets it reach COMPLETE before the first block of the next epoch
    // is templated.
    const Consensus::Params& p = Params().GetConsensus();
    int32_t epoch_length = p.nDDOracleEpochBlocks;
    if (epoch_length <= 0) epoch_length = 1440;
    const int32_t pos_in_epoch = block_height % epoch_length;
    const int32_t blocks_until_next = epoch_length - pos_in_epoch;
    constexpr int32_t kPreStartWindow = 5;
    if (blocks_until_next > 0 && blocks_until_next <= kPreStartWindow) {
        TickEpochSession(current_epoch + 1, block_height);
    }
}

void OracleSigningOrchestrator::TickEpochSession(int32_t epoch, int32_t block_height)
{
    MuSig2SigningSession* session = GetOrCreateSigningSession(epoch, block_height);
    if (!session) return;
    RestartEpochAttemptIfNeeded(epoch, block_height, session);
    if (!session) return;

    const int32_t current_epoch = GetCurrentEpoch(block_height);
    if (HasSelectionSeedForEpoch(epoch)) {
        session->SetEpochSelectionSeed(GetSelectionSeedForEpoch(epoch));
    }

    MuSig2SessionState state = session->GetState();
    bool is_oracle = IsOracleNode();

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: TickEpochSession h=%d epoch=%d state=%d is_oracle=%d\n",
             block_height, epoch, static_cast<int>(state), is_oracle);

    if (!m_aggregator) {
        m_aggregator = std::make_unique<MuSig2OracleAggregator>();
    }

    // ── Step 1: Oracle generates nonces for ALL local oracle IDs on new epoch ──
    if (is_oracle && (state == MuSig2SessionState::CREATED || state == MuSig2SessionState::NONCES_COLLECTING)) {
        OracleManager& om = OracleManager::GetInstance();
        const std::vector<uint32_t> local_ids = om.GetActiveOracleIds();

        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Step 1 - local_ids.size()=%zu for epoch %d\n", local_ids.size(), epoch);

        // Build full oracle ID list for key aggregation
        std::vector<uint8_t> all_oracle_ids = GetConsensusOracleIdsForSigning();

        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Step 1 - all_oracle_ids.size()=%zu\n", all_oracle_ids.size());

        secp256k1_xonly_pubkey agg_pk;
        secp256k1_musig_keyagg_cache cache;
        if (m_aggregator->ComputeAggregatePubkey(all_oracle_ids, agg_pk, cache)) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Step 1 - key aggregation succeeded for %zu oracle IDs\n", all_oracle_ids.size());
            secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);

            for (uint32_t oid : local_ids) {
                uint8_t oid8 = static_cast<uint8_t>(oid);
                // Skip if already generated nonce for this oracle
                if (m_nonce_broadcast_tracker[epoch].count(oid8)) {
                    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Skipping oracle %d epoch %d - already broadcast\n", oid8, epoch);
                    continue;
                }

                OracleNode* onode = om.GetOracleNode(oid);
                if (!onode) {
                    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Skipping oracle %d - GetOracleNode returned null\n", oid8);
                    continue;
                }
                CKey key = onode->GetOraclePrivateKey();
                if (!key.IsValid()) {
                    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Skipping oracle %d - invalid private key\n", oid8);
                    continue;
                }
                CPubKey cpk = key.GetPubKey();
                LogPrint(BCLog::DIGIDOLLAR, "Oracle: oracle %d pubkey size=%d hex=%s\n", oid8, cpk.size(), HexStr(cpk));

                secp256k1_pubkey secp_pk;
                if (!secp256k1_ec_pubkey_parse(ctx, &secp_pk, cpk.data(), cpk.size())) {
                    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Skipping oracle %d - secp256k1_ec_pubkey_parse failed\n", oid8);
                    continue;
                }

                secp256k1_musig_pubnonce pubnonce;
                if (session->GenerateNonce(oid8, key, secp_pk, cache, pubnonce)) {
                    session->AddPubnonce(oid8, pubnonce);

                    unsigned char ser_nonce[66];
                    if (secp256k1_musig_pubnonce_serialize(ctx, ser_nonce, &pubnonce)) {
                        OracleMusigNonceMsg nonce_msg;
                        nonce_msg.epoch = epoch;
                        nonce_msg.attempt_id = static_cast<uint8_t>(session->GetAttemptId());
                        nonce_msg.oracle_id = oid8;
                        nonce_msg.pubnonce.assign(ser_nonce, ser_nonce + 66);

                        // RC30: sign the nonce message so peers accept it.
                        // RH-24 added Schnorr auth on the receive side; the
                        // sender must also sign or every peer drops the msg
                        // as "invalid MuSig2 nonce signature".
                        if (!nonce_msg.Sign(key)) {
                            LogPrintf("Oracle: Sign() failed for MuSig2 nonce oracle=%d epoch=%d\n",
                                     oid8, epoch);
                            continue;
                        }

                        BroadcastMusigNonce(nonce_msg);
                        m_nonce_broadcast_tracker[epoch].insert(oid8);
                        m_nonce_evidence[epoch][oid8] = nonce_msg;

                        LogPrintf("Oracle: Generated and broadcast nonce for epoch %d attempt %u (oracle_id=%d)\n",
                                 epoch, nonce_msg.attempt_id, oid8);
                    }
                } else {
                    LogPrintf("Oracle: GenerateNonce FAILED for oracle %d epoch %d\n", oid8, epoch);
                }
            }
            secp256k1_context_destroy(ctx);
        } else {
            LogPrintf("Oracle: Failed to compute aggregate pubkey for epoch %d\n", epoch);
        }
    }

    state = session->GetState();

    // ── Step 2: converge on one context, then selected local oracles sign it ──
    //
    // Context convergence must run on every node that may build block
    // templates. Only local context proposal/signature creation requires a
    // local oracle key. Passive template nodes still need to select the
    // authenticated remote context, enter SIGNING, replay remote partial sigs,
    // and expose the completed session to generatetoaddress/getblocktemplate.
    if (state == MuSig2SessionState::NONCES_COMPLETE) {
        if (epoch > current_epoch && !HasSelectionSeedForEpoch(epoch)) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Step 2 waiting for epoch selection seed epoch=%d h=%d\n",
                     epoch, block_height);
            return;
        }
        session->SetEpochSelectionSeed(GetSelectionSeedForEpoch(epoch));

        if (block_height < session->GetCreationHeight() + CONTEXT_NONCE_GRACE_BLOCKS) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Step 2 waiting for nonce grace window epoch %d h=%d creation=%d\n",
                     epoch, block_height, session->GetCreationHeight());
            return;
        }

        std::optional<OracleMusigContextMsg> chosen_context =
            SelectReadyContextProposal(epoch, block_height, *session);

        if (!chosen_context) {
            if (is_oracle) {
                std::optional<OracleMusigContextMsg> local_context =
                    BuildLocalContextProposal(epoch, block_height, *session);
                if (local_context) {
                    BroadcastMusigContext(*local_context);
                    LogPrintf("Oracle: Broadcast MuSig2 context proposal epoch=%d proposer=%u context=%s\n",
                             epoch, local_context->proposer_id,
                             local_context->session_context_id.ToString());
                    return;
                }
            }
        }

        if (chosen_context) {
            std::vector<uint8_t> participant_ids = chosen_context->participant_ids;
            const uint64_t consensus_price = chosen_context->consensus_price;
            const int64_t consensus_timestamp = chosen_context->consensus_timestamp;

            unsigned char msg32[32];
            ComputeOracleMessageHash(epoch, consensus_price, consensus_timestamp, msg32);

            // Store the exact values we're signing so the miner embeds
            // them in the bundle (must match for verification).
            session->SetSignedValues(consensus_price, consensus_timestamp);

            // Threshold MuSig2: trim to exactly threshold nonces, then
            // recompute key aggregation for ONLY those oracles. The session,
            // partial sigs, and aggregate signature are all bound to this
            // threshold-sized participant set. This ensures the validator can
            // reconstruct the same aggregate key from the bitmap.
            if (!session->TrimNoncesToParticipants(participant_ids)) {
                LogPrintf("Oracle: Step 2 - failed to freeze participants for epoch %d\n", epoch);
                return;
            }
            participant_ids = session->GetNonceParticipants();
            secp256k1_xonly_pubkey part_agg_pk;
            secp256k1_musig_keyagg_cache part_cache;
            if (!m_aggregator->ComputeAggregatePubkey(participant_ids, part_agg_pk, part_cache)) {
                LogPrintf("Oracle: Step 2 - failed to compute participants-only aggregate for epoch %d (%zu participants)\n",
                         epoch, participant_ids.size());
            } else {
                session->SetKeyAggCache(part_cache);
                LogPrintf("Oracle: Step 2 - recomputed keyagg for %zu participants (epoch %d)\n",
                         participant_ids.size(), epoch);
            }

            if (session->AggregateNonces(msg32)) {
                const uint256 session_context = session->GetSessionContextId();
                LogPrintf("Oracle: Nonces aggregated for epoch %d, SIGNING context=%s\n",
                         epoch, session_context.ToString());
                if (session_context.IsNull()) {
                    LogPrintf("Oracle: Step 2 - refusing to broadcast partial sigs for epoch %d with null context\n",
                             epoch);
                    return;
                }

                if (!is_oracle) {
                    LogPrintf("Oracle: Passive MuSig2 context selected for epoch %d context=%s\n",
                             epoch, session_context.ToString());
                } else {
                    OracleManager& om = OracleManager::GetInstance();
                    const std::vector<uint32_t> local_ids = om.GetActiveOracleIds();
                    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);

                    for (uint32_t oid : local_ids) {
                        uint8_t oid8 = static_cast<uint8_t>(oid);
                        if (m_partialsig_broadcast_tracker[epoch].count(oid8)) continue;

                        OracleNode* onode = om.GetOracleNode(oid);
                        if (!onode) continue;
                        CKey key = onode->GetOraclePrivateKey();
                        if (!key.IsValid()) continue;

                        if (std::find(participant_ids.begin(), participant_ids.end(), oid8) == participant_ids.end()) {
                            continue;
                        }

                        secp256k1_musig_partial_sig partial_sig;
                        if (session->CreatePartialSignature(oid8, key, partial_sig)) {
                            session->AddPartialSignature(oid8, partial_sig);

                            unsigned char ser_psig[32];
                            if (secp256k1_musig_partial_sig_serialize(ctx, ser_psig, &partial_sig)) {
                                OracleMusigPartialSigMsg psig_msg;
                                psig_msg.epoch = epoch;
                                psig_msg.attempt_id = static_cast<uint8_t>(session->GetAttemptId());
                                psig_msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
                                psig_msg.session_context_id = session_context;
                                psig_msg.oracle_id = oid8;
                                psig_msg.partial_sig.assign(ser_psig, ser_psig + 32);

                                // RC30: sign the partial-sig message so peers accept it
                                // (same pattern as nonce_msg — RH-24 auth requirement).
                                if (!psig_msg.Sign(key)) {
                                    LogPrintf("Oracle: Sign() failed for MuSig2 partial sig oracle=%d epoch=%d\n",
                                             oid8, epoch);
                                    continue;
                                }

                                BroadcastMusigPartialSig(psig_msg);
                                m_partialsig_broadcast_tracker[epoch].insert(oid8);

                                LogPrintf("Oracle: Broadcast partial sig for epoch %d (oracle_id=%d)\n",
                                         epoch, oid8);
                                continue;
                            }
                        }
                    }
                    secp256k1_context_destroy(ctx);
                }
            }
        }
    }

    state = session->GetState();
    if (state == MuSig2SessionState::SIGNING) {
        DrainPendingPartialSigsForEpoch(epoch, *session);
    }

    // ── Step 3: All nodes aggregate when enough partial sigs ──
    if (state == MuSig2SessionState::SIGNING && session->HasEnoughPartialSigs()) {
        std::vector<unsigned char> final_sig;
        if (session->AggregateSignature(final_sig)) {
            LogPrintf("Oracle: MuSig2 COMPLETE for epoch %d, sig size=%zu\n",
                     epoch, final_sig.size());
        }
    }

    // ── Step 4: Timeout check ──
    session->CheckTimeout(block_height);
}

// ============================================================================
// P2P broadcast
// ============================================================================

bool OracleSigningOrchestrator::BroadcastMusigNonce(const OracleMusigNonceMsg& msg)
{
    if (!m_connman) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Cannot broadcast MuSig2 nonce - no P2P\n");
        return false;
    }

    m_connman->ForEachNode([this, &msg](CNode* node) {
        m_connman->PushMessage(node,
            CNetMsgMaker(node->GetCommonVersion()).Make(
                NetMsgType::ORACLEMUSIGNONCE, msg));
    });

    return true;
}

bool OracleSigningOrchestrator::BroadcastMusigContext(const OracleMusigContextMsg& msg)
{
    if (!m_connman) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Cannot broadcast MuSig2 context proposal - no P2P\n");
        return false;
    }

    m_connman->ForEachNode([this, &msg](CNode* node) {
        m_connman->PushMessage(node,
            CNetMsgMaker(node->GetCommonVersion()).Make(
                NetMsgType::ORACLEMUSIGCONTEXT, msg));
    });

    return true;
}

bool OracleSigningOrchestrator::BroadcastMusigPartialSig(const OracleMusigPartialSigMsg& msg)
{
    if (!m_connman) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Cannot broadcast MuSig2 partial sig - no P2P\n");
        return false;
    }

    m_connman->ForEachNode([this, &msg](CNode* node) {
        m_connman->PushMessage(node,
            CNetMsgMaker(node->GetCommonVersion()).Make(
                NetMsgType::ORACLEMUSIGPARTIALSIG, msg));
    });

    return true;
}

// ============================================================================
// Query completed session for block assembly
// ============================================================================

bool OracleSigningOrchestrator::GetCompletedSession(
    int32_t epoch,
    std::vector<unsigned char>& aggregate_sig_out,
    std::vector<unsigned char>& participation_bitmap_out,
    uint64_t& signed_price_out,
    int64_t& signed_timestamp_out) const
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    auto it = m_signing_sessions.find(epoch);
    if (it == m_signing_sessions.end()) return false;

    const MuSig2SigningSession& session = *it->second;
    if (session.GetState() != MuSig2SessionState::COMPLETE) return false;

    aggregate_sig_out = session.GetAggregateSig();
    participation_bitmap_out = session.GetParticipationBitmap();
    signed_price_out = session.GetSignedPrice();
    signed_timestamp_out = session.GetSignedTimestamp();

    return !aggregate_sig_out.empty() && signed_price_out > 0 && signed_timestamp_out > 0;
}

std::optional<OracleSigningOrchestrator::SessionStatus>
OracleSigningOrchestrator::GetSessionStateForEpoch(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    auto it = m_signing_sessions.find(epoch);
    if (it == m_signing_sessions.end() || !it->second) return std::nullopt;
    const MuSig2SigningSession& session = *it->second;
    SessionStatus status;
    status.state = session.GetState();
    status.nonce_count = session.GetNonceCount();
    status.partial_sig_count = session.GetPartialSigCount();
    status.creation_height = session.GetCreationHeight();
    return status;
}

// ============================================================================
// Utilities
// ============================================================================

int32_t OracleSigningOrchestrator::ComputeEpoch(int32_t block_height, int32_t epoch_length)
{
    if (epoch_length <= 0) return 0;
    return block_height / epoch_length;
}

void OracleSigningOrchestrator::ComputeOracleMessageHash(
    int32_t epoch, uint64_t price, int64_t timestamp,
    unsigned char hash32[32])
{
    // DD-FA-SEC-008 — domain-separate the v0x03 MuSig2 message so that the
    // same (epoch, price, timestamp) signed for one DigiByte chain cannot
    // be replayed on a different chain that shares the oracle roster.
    // The hash binds:
    //   tag           — labeled prefix for defense-in-depth
    //   chain_hash    — Params().GetConsensus().hashGenesisBlock (chain id)
    //   epoch         — RC30 cross-epoch replay binding
    //   price         — consensus DGB/USD price (micro-USD)
    //   timestamp     — bundle timestamp
    //
    // Validator computes the same hash via ComputeOracleBundleHash, which
    // MUST stay byte-identical to this construction for verification to
    // succeed.
    CHashWriter hasher(0);
    hasher << std::string{"DigiDollar/OracleBundle"};
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch;
    hasher << price;
    hasher << timestamp;
    uint256 result = hasher.GetHash();
    memcpy(hash32, result.data(), 32);
}
