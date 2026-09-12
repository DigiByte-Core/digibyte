// Copyright (c) 2024-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <oracle/musig2_session.h>

#include <chainparams.h>
#include <hash.h>
#include <oracle/musig2_messages.h>
#include <logging.h>
#include <primitives/oracle.h>
#include <random.h>
#include <support/cleanse.h>

#include <secp256k1_musig.h>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace {

uint16_t ConfiguredMuSig2ActiveRosterSize()
{
    const Consensus::Params& consensus = Params().GetConsensus();
    int active = consensus.nOraclePubkeyCount > 0 ?
        consensus.nOraclePubkeyCount : consensus.nOracleTotalOracles;

    // Local mini-testnet harnesses intentionally run slots 0-15 and leave slot
    // 16 offline while preserving 17 total bitmap slots. Do not change the
    // consensus pubkey count (startup validation requires all configured keys);
    // only the local signing roster excludes the deliberately-unrun slot.
    if (consensus.fEasyPow && active == 17) {
        active = 16;
    }

    if (active <= 0 || active > 256) return 0;
    return static_cast<uint16_t>(active);
}

uint16_t ConfiguredMuSig2BitmapSlots()
{
    const Consensus::Params& consensus = Params().GetConsensus();
    const int configured_total = std::max(consensus.nOracleTotalOracles, consensus.nOraclePubkeyCount);
    if (configured_total <= 0 || configured_total > 256) return 0;
    return static_cast<uint16_t>(configured_total);
}

std::vector<unsigned char> BuildRawParticipationBitmap(const std::vector<uint8_t>& oracle_ids, uint16_t total_oracles)
{
    if (oracle_ids.empty()) return {};
    if (total_oracles == 0 || total_oracles > 256) return {};
    std::vector<unsigned char> bitmap((total_oracles + 7) / 8, 0);
    for (const uint8_t id : oracle_ids) {
        if (id >= total_oracles) return {};
        bitmap[id / 8] |= static_cast<unsigned char>(1U << (id % 8));
    }
    return bitmap;
}

} // namespace

MuSig2SigningSession::MuSig2SigningSession(int32_t epoch, uint8_t min_signers, uint32_t attempt_id)
    : m_epoch(epoch),
      m_attempt_id(attempt_id),
      m_min_signers(min_signers),
      m_state(MuSig2SessionState::CREATED),
      m_creation_height(0),
      m_timeout_blocks(100)
{
    m_epoch_selection_seed = Params().GetConsensus().hashGenesisBlock;
    m_ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    assert(m_ctx != nullptr);
    memset(&m_keyagg_cache, 0, sizeof(m_keyagg_cache));
    memset(&m_aggnonce, 0, sizeof(m_aggnonce));
    memset(&m_session, 0, sizeof(m_session));
}

MuSig2SigningSession::~MuSig2SigningSession()
{
    // Zero all secret nonces in destructor as safety net
    for (auto& [id, nonce] : m_secnonces) {
        memory_cleanse(&nonce, sizeof(nonce));
    }
    if (m_ctx) {
        secp256k1_context_destroy(m_ctx);
        m_ctx = nullptr;
    }
}

MuSig2SigningSession::MuSig2SigningSession(MuSig2SigningSession&& other) noexcept
    : m_epoch(other.m_epoch),
      m_attempt_id(other.m_attempt_id),
      m_min_signers(other.m_min_signers),
      m_ctx(nullptr)
{
    LOCK(other.m_mutex);
    m_state = other.m_state;
    m_ctx = other.m_ctx;
    other.m_ctx = nullptr;

    m_secnonces = std::move(other.m_secnonces);
    m_secnonces_used = std::move(other.m_secnonces_used);

    m_keyagg_cache = other.m_keyagg_cache;
    m_pubnonces = std::move(other.m_pubnonces);
    m_participants_frozen = other.m_participants_frozen;
    m_epoch_selection_seed = other.m_epoch_selection_seed;
    m_aggnonce = other.m_aggnonce;
    m_session = other.m_session;
    m_partial_sigs = std::move(other.m_partial_sigs);
    m_creation_height = other.m_creation_height;
    m_timeout_blocks = other.m_timeout_blocks;

    other.m_state = MuSig2SessionState::FAILED;
}

MuSig2SigningSession& MuSig2SigningSession::operator=(MuSig2SigningSession&& other) noexcept
{
    if (this == &other) return *this;

    // Clean up current state
    for (auto& [id, nonce] : m_secnonces) {
        memory_cleanse(&nonce, sizeof(nonce));
    }
    m_secnonces.clear();
    m_secnonces_used.clear();
    if (m_ctx) {
        secp256k1_context_destroy(m_ctx);
        m_ctx = nullptr;
    }

    LOCK(other.m_mutex);
    m_epoch = other.m_epoch;
    m_attempt_id = other.m_attempt_id;
    m_min_signers = other.m_min_signers;
    m_state = other.m_state;
    m_ctx = other.m_ctx;
    other.m_ctx = nullptr;

    m_secnonces = std::move(other.m_secnonces);
    m_secnonces_used = std::move(other.m_secnonces_used);

    m_keyagg_cache = other.m_keyagg_cache;
    m_pubnonces = std::move(other.m_pubnonces);
    m_participants_frozen = other.m_participants_frozen;
    m_epoch_selection_seed = other.m_epoch_selection_seed;
    m_aggnonce = other.m_aggnonce;
    m_session = other.m_session;
    m_partial_sigs = std::move(other.m_partial_sigs);
    m_creation_height = other.m_creation_height;
    m_timeout_blocks = other.m_timeout_blocks;

    other.m_state = MuSig2SessionState::FAILED;
    return *this;
}

MuSig2SessionState MuSig2SigningSession::GetState() const
{
    LOCK(m_mutex);
    return m_state;
}

int32_t MuSig2SigningSession::GetEpoch() const
{
    return m_epoch; // immutable, no lock needed
}

uint32_t MuSig2SigningSession::GetAttemptId() const
{
    return m_attempt_id; // immutable, no lock needed
}

// ============================================================================
// Passive initialization (non-oracle nodes)
// ============================================================================

bool MuSig2SigningSession::InitializePassive(const secp256k1_musig_keyagg_cache& cache)
{
    LOCK(m_mutex);
    if (m_state == MuSig2SessionState::NONCES_COLLECTING ||
        m_state == MuSig2SessionState::NONCES_COMPLETE) {
        return true;
    }
    if (m_state != MuSig2SessionState::CREATED) return false;
    m_keyagg_cache = cache;
    // Passive nodes have no local secret nonces
    m_state = MuSig2SessionState::NONCES_COLLECTING;
    return true;
}

// ============================================================================
// Round 1a: Generate local nonce
// ============================================================================

bool MuSig2SigningSession::GenerateNonce(uint8_t oracle_id,
                                         const CKey& signing_key,
                                         const secp256k1_pubkey& pubkey,
                                         const secp256k1_musig_keyagg_cache& cache,
                                         secp256k1_musig_pubnonce& pubnonce_out)
{
    LOCK(m_mutex);

    // Allow CREATED (first call) or NONCES_COLLECTING (subsequent local oracles)
    if (m_state != MuSig2SessionState::CREATED &&
        m_state != MuSig2SessionState::NONCES_COLLECTING) return false;
    if (!signing_key.IsValid()) return false;
    const uint16_t active_oracles = ConfiguredMuSig2ActiveRosterSize();
    if (active_oracles == 0 || oracle_id >= active_oracles) return false;
    if (m_secnonces.count(oracle_id)) return false; // already generated for this oracle

    // Store the key aggregation cache (same for all local oracles)
    m_keyagg_cache = cache;

    // Generate session randomness
    unsigned char session_secrand[32];
    GetStrongRandBytes(Span{session_secrand, 32});

    // Generate secnonce + pubnonce
    secp256k1_musig_secnonce secnonce;
    if (!secp256k1_musig_nonce_gen(m_ctx,
                                    &secnonce,
                                    &pubnonce_out,
                                    session_secrand,
                                    signing_key.begin(),
                                    &pubkey,
                                    nullptr,  // msg32 — not known yet
                                    &m_keyagg_cache,
                                    nullptr)) {
        memory_cleanse(&secnonce, sizeof(secnonce));
        return false;
    }

    m_secnonces[oracle_id] = secnonce;
    memory_cleanse(&secnonce, sizeof(secnonce)); // clear stack copy
    m_state = MuSig2SessionState::NONCES_COLLECTING;
    return true;
}

// ============================================================================
// Round 1b: Collect pubnonces
// ============================================================================

bool MuSig2SigningSession::AddPubnonce(uint8_t oracle_id,
                                       const secp256k1_musig_pubnonce& pubnonce)
{
    LOCK(m_mutex);

    if (m_state != MuSig2SessionState::NONCES_COLLECTING &&
        m_state != MuSig2SessionState::NONCES_COMPLETE) {
        return false;
    }

    // Reject duplicate oracle ID
    if (m_pubnonces.count(oracle_id)) return false;
    if (m_participants_frozen) return false;

    // Reject oracle IDs outside configured active set to prevent malformed
    // participation bitmaps and invalid signer transitions.
    const uint16_t active_oracles = ConfiguredMuSig2ActiveRosterSize();
    if (active_oracles == 0 || oracle_id >= active_oracles) return false;

    // Validate pubnonce by checking secp256k1 internal magic bytes.
    // secp256k1_musig_pubnonce_serialize calls abort() via ARG_CHECK on
    // invalid data instead of returning false, so check magic directly.
    static const unsigned char pubnonce_magic[4] = {0xf5, 0x7a, 0x3d, 0xa0};
    if (memcmp(pubnonce.data, pubnonce_magic, 4) != 0) {
        return false;
    }

    m_pubnonces[oracle_id] = pubnonce;

    // The session is ready once it has the deterministic signing committee's
    // nonces. For 1-of-N local signing sessions, prefer the locally-generated
    // secnonce owner; otherwise a remote injected nonce can prematurely complete
    // the session and exclude the local signer. Passive sessions without a local
    // secnonce may still converge on the lowest collected nonce.
    const std::vector<uint8_t> required = GetRequiredParticipantsUnsafe();
    bool have_required = required.size() >= m_min_signers;
    for (uint8_t id : required) {
        if (m_pubnonces.find(id) == m_pubnonces.end()) {
            have_required = false;
            break;
        }
    }
    if (have_required) {
        m_state = MuSig2SessionState::NONCES_COMPLETE;
    }

    return true;
}

bool MuSig2SigningSession::HasEnoughNonces() const
{
    LOCK(m_mutex);
    return m_min_signers > 0 && m_pubnonces.size() >= m_min_signers;
}

size_t MuSig2SigningSession::GetNonceCount() const
{
    LOCK(m_mutex);
    return m_pubnonces.size();
}

// ============================================================================
// Aggregate nonces + initialize signing session
// ============================================================================

void MuSig2SigningSession::SetKeyAggCache(const secp256k1_musig_keyagg_cache& cache)
{
    LOCK(m_mutex);
    m_keyagg_cache = cache;
}

void MuSig2SigningSession::SetEpochSelectionSeed(const uint256& seed)
{
    LOCK(m_mutex);
    m_epoch_selection_seed = seed;
}

uint256 MuSig2SigningSession::GetEpochSelectionSeed() const
{
    LOCK(m_mutex);
    return m_epoch_selection_seed;
}

std::vector<uint8_t> MuSig2SigningSession::GetRequiredParticipants() const
{
    LOCK(m_mutex);
    return GetRequiredParticipantsUnsafe();
}

std::vector<uint8_t> MuSig2SigningSession::GetRequiredParticipantsUnsafe() const
{
    std::vector<uint8_t> ids;
    const uint16_t active_oracles = ConfiguredMuSig2ActiveRosterSize();
    if (m_min_signers == 0 || active_oracles < m_min_signers) {
        return ids;
    }

    ids.reserve(m_min_signers);
    if (m_min_signers == 1) {
        if (!m_secnonces.empty()) {
            for (const auto& [id, secnonce] : m_secnonces) {
                if (m_pubnonces.count(id)) {
                    ids.push_back(id);
                    break;
                }
            }
        } else if (!m_pubnonces.empty()) {
            ids.push_back(m_pubnonces.begin()->first);
        }
        return ids;
    }

    // Rank all nonce-submitting oracle IDs by hash(epoch, oracle_id).
    // the same scoring used by SelectOraclesForEpoch. A different epoch
    // produces a different ordering, so the signing committee rotates
    // across epochs rather than always locking in the lowest sequential IDs.
    std::vector<std::pair<uint256, uint8_t>> scored;
    scored.reserve(m_pubnonces.size());
    for (const auto& [id, nonce] : m_pubnonces) {
        scored.emplace_back(GetOracleEpochSelectionHash(m_epoch, id, m_epoch_selection_seed), id);
    }
    std::sort(scored.begin(), scored.end());
    for (const auto& [score, id] : scored) {
        if (ids.size() >= static_cast<size_t>(m_min_signers)) break;
        ids.push_back(id);
    }
    return ids;
}

bool MuSig2SigningSession::MatchesRequiredParticipants(const std::vector<uint8_t>& participants) const
{
    LOCK(m_mutex);
    return participants == GetRequiredParticipantsUnsafe();
}

std::vector<uint8_t> MuSig2SigningSession::GetNonceParticipants() const
{
    LOCK(m_mutex);
    std::vector<uint8_t> ids;
    ids.reserve(m_pubnonces.size());
    for (const auto& [id, nonce] : m_pubnonces) {
        ids.push_back(id);
    }
    return ids; // already sorted (std::map)
}

void MuSig2SigningSession::TrimNoncesToThreshold()
{
    LOCK(m_mutex);
    const std::vector<uint8_t> required = GetRequiredParticipantsUnsafe();
    const size_t before = m_pubnonces.size();

    for (auto it = m_pubnonces.begin(); it != m_pubnonces.end(); ) {
        if (std::find(required.begin(), required.end(), it->first) == required.end()) {
            it = m_pubnonces.erase(it);
        } else {
            ++it;
        }
    }

    m_participants_frozen = true;
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Trimmed nonces from %zu to %zu deterministic participants (threshold=%zu, epoch=%d)\n",
             before, m_pubnonces.size(), m_min_signers, m_epoch);
}

bool MuSig2SigningSession::TrimNoncesToParticipants(const std::vector<uint8_t>& participants)
{
    LOCK(m_mutex);

    if (m_state != MuSig2SessionState::NONCES_COMPLETE &&
        m_state != MuSig2SessionState::SIGNING) {
        return false;
    }
    if (participants.size() < static_cast<size_t>(m_min_signers)) return false;

    std::set<uint8_t> keep(participants.begin(), participants.end());
    if (keep.size() != participants.size()) return false;
    for (uint8_t id : keep) {
        if (m_pubnonces.find(id) == m_pubnonces.end()) return false;
    }

    if (m_participants_frozen) {
        if (m_pubnonces.size() != keep.size()) return false;
        for (const auto& [id, nonce] : m_pubnonces) {
            if (!keep.count(id)) return false;
        }
        return true;
    }

    const size_t before = m_pubnonces.size();
    for (auto it = m_pubnonces.begin(); it != m_pubnonces.end(); ) {
        if (!keep.count(it->first)) {
            it = m_pubnonces.erase(it);
        } else {
            ++it;
        }
    }

    m_participants_frozen = true;
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Trimmed nonces from %zu to %zu proposed participants (threshold=%zu, epoch=%d)\n",
             before, m_pubnonces.size(), m_min_signers, m_epoch);
    return m_pubnonces.size() == keep.size();
}

bool MuSig2SigningSession::ComputeContextIdForParticipants(const std::vector<uint8_t>& participants,
                                                           const unsigned char* msg32,
                                                           uint256& nonce_set_hash_out,
                                                           uint256& context_id_out) const
{
    LOCK(m_mutex);

    nonce_set_hash_out.SetNull();
    context_id_out.SetNull();
    if (!msg32) return false;
    if (participants.size() < static_cast<size_t>(m_min_signers)) return false;

    std::set<uint8_t> keep(participants.begin(), participants.end());
    if (keep.size() != participants.size()) return false;

    CHashWriter nonce_hasher(0);
    nonce_hasher << std::string("DigiDollar/MuSig2NonceSet/v1");
    nonce_hasher << Params().GetConsensus().hashGenesisBlock;
    nonce_hasher << m_epoch;
    nonce_hasher << m_attempt_id;
    for (uint8_t id : keep) {
        const auto it = m_pubnonces.find(id);
        if (it == m_pubnonces.end()) return false;
        unsigned char ser_nonce[66];
        if (!secp256k1_musig_pubnonce_serialize(m_ctx, ser_nonce, &it->second)) {
            return false;
        }
        std::vector<unsigned char> nonce_bytes(ser_nonce, ser_nonce + 66);
        nonce_hasher << id;
        nonce_hasher << nonce_bytes;
    }

    uint256 message_hash;
    std::memcpy(message_hash.begin(), msg32, 32);

    std::vector<uint8_t> sorted_participants(keep.begin(), keep.end());
    const std::vector<unsigned char> bitmap =
        BuildRawParticipationBitmap(sorted_participants, ConfiguredMuSig2BitmapSlots());
    if (bitmap.empty()) return false;

    nonce_set_hash_out = nonce_hasher.GetHash();

    CHashWriter context_hasher(0);
    context_hasher << std::string("DigiDollar/MuSig2SessionContext/v1");
    context_hasher << Params().GetConsensus().hashGenesisBlock;
    context_hasher << m_epoch;
    context_hasher << m_attempt_id;
    context_hasher << m_epoch_selection_seed;
    context_hasher << static_cast<uint8_t>(ORACLE_MUSIG2_SESSION_CONTEXT_VERSION);
    context_hasher << message_hash;
    context_hasher << bitmap;
    context_hasher << nonce_set_hash_out;
    context_id_out = context_hasher.GetHash();
    return true;
}

bool MuSig2SigningSession::AggregateNonces(const unsigned char* msg32)
{
    LOCK(m_mutex);

    if (m_state != MuSig2SessionState::NONCES_COMPLETE) return false;
    if (!msg32) return false;

    // Build sorted array of pubnonce pointers (sorted by oracle_id via std::map)
    std::vector<const secp256k1_musig_pubnonce*> pubnonce_ptrs;
    pubnonce_ptrs.reserve(m_pubnonces.size());
    for (const auto& [id, nonce] : m_pubnonces) {
        pubnonce_ptrs.push_back(&nonce);
    }

    // Aggregate all pubnonces
    if (!secp256k1_musig_nonce_agg(m_ctx, &m_aggnonce,
                                    pubnonce_ptrs.data(), pubnonce_ptrs.size())) {
        m_state = MuSig2SessionState::FAILED;
        return false;
    }

    // Initialize the MuSig2 session with the aggregate nonce and message.
    // m_keyagg_cache MUST have been set to the participants-only aggregate
    // before this call (via SetKeyAggCache from the orchestrator).
    if (!secp256k1_musig_nonce_process(m_ctx, &m_session, &m_aggnonce,
                                        msg32, &m_keyagg_cache)) {
        m_state = MuSig2SessionState::FAILED;
        return false;
    }

    std::vector<uint8_t> participants;
    participants.reserve(m_pubnonces.size());
    CHashWriter nonce_hasher(0);
    nonce_hasher << std::string("DigiDollar/MuSig2NonceSet/v1");
    nonce_hasher << Params().GetConsensus().hashGenesisBlock;
    nonce_hasher << m_epoch;
    nonce_hasher << m_attempt_id;
    for (const auto& [id, nonce] : m_pubnonces) {
        participants.push_back(id);
        unsigned char ser_nonce[66];
        if (!secp256k1_musig_pubnonce_serialize(m_ctx, ser_nonce, &nonce)) {
            m_state = MuSig2SessionState::FAILED;
            return false;
        }
        std::vector<unsigned char> nonce_bytes(ser_nonce, ser_nonce + 66);
        nonce_hasher << id;
        nonce_hasher << nonce_bytes;
    }

    m_message_hash.SetNull();
    std::memcpy(m_message_hash.begin(), msg32, 32);
    m_nonce_set_hash = nonce_hasher.GetHash();

    const std::vector<unsigned char> bitmap =
        BuildRawParticipationBitmap(participants, ConfiguredMuSig2BitmapSlots());
    if (bitmap.empty()) {
        m_state = MuSig2SessionState::FAILED;
        return false;
    }

    CHashWriter context_hasher(0);
    context_hasher << std::string("DigiDollar/MuSig2SessionContext/v1");
    context_hasher << Params().GetConsensus().hashGenesisBlock;
    context_hasher << m_epoch;
    context_hasher << m_attempt_id;
    context_hasher << m_epoch_selection_seed;
    context_hasher << static_cast<uint8_t>(ORACLE_MUSIG2_SESSION_CONTEXT_VERSION);
    context_hasher << m_message_hash;
    context_hasher << bitmap;
    context_hasher << m_nonce_set_hash;
    m_session_context_id = context_hasher.GetHash();

    m_state = MuSig2SessionState::SIGNING;
    return true;
}

// ============================================================================
// Round 2a: Create local partial signature
// ============================================================================

bool MuSig2SigningSession::CreatePartialSignature(uint8_t oracle_id,
                                                   const CKey& signing_key,
                                                   secp256k1_musig_partial_sig& partial_sig_out)
{
    LOCK(m_mutex);

    if (m_state != MuSig2SessionState::SIGNING) return false;
    if (!signing_key.IsValid()) return false;

    // Check we have a secnonce for this oracle and it hasn't been used
    auto nonce_it = m_secnonces.find(oracle_id);
    if (nonce_it == m_secnonces.end()) return false;
    if (m_pubnonces.find(oracle_id) == m_pubnonces.end()) return false;
    if (m_secnonces_used.count(oracle_id)) return false;

    // Create keypair from CKey for secp256k1_musig_partial_sign
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(m_ctx, &keypair, signing_key.begin())) {
        return false;
    }

    // Sign — this ZEROES the secnonce on success (secp256k1 guarantee)
    int ret = secp256k1_musig_partial_sign(m_ctx,
                                            &partial_sig_out,
                                            &nonce_it->second,
                                            &keypair,
                                            &m_keyagg_cache,
                                            &m_session);

    // Zero the keypair regardless of success
    memory_cleanse(&keypair, sizeof(keypair));

    // Mark this oracle's nonce as consumed and cleanse
    memory_cleanse(&nonce_it->second, sizeof(nonce_it->second));
    m_secnonces.erase(nonce_it);
    m_secnonces_used.insert(oracle_id);

    return ret != 0;
}

// ============================================================================
// Round 2b: Collect partial signatures
// ============================================================================

bool MuSig2SigningSession::AddPartialSignature(uint8_t oracle_id,
                                                const secp256k1_musig_partial_sig& partial_sig)
{
    LOCK(m_mutex);

    if (m_state != MuSig2SessionState::SIGNING) return false;

    // Validate secp256k1 internal magic bytes before storing the struct.
    // libsecp256k1 uses ARG_CHECK and aborts on malformed partial_sig objects,
    // so reject obviously invalid data here instead of letting aggregation die.
    static const unsigned char partial_sig_magic[4] = {0xeb, 0xfb, 0x1a, 0x32};
    if (memcmp(partial_sig.data, partial_sig_magic, 4) != 0) return false;

    // Reject duplicate oracle ID
    if (m_partial_sigs.count(oracle_id)) return false;

    // Only accept sigs from oracles in the nonce participant set.
    // After TrimNoncesToThreshold(), m_pubnonces contains exactly the
    // threshold oracles whose keys were aggregated. Sigs from other
    // oracles would not verify against the aggregate key.
    if (m_pubnonces.find(oracle_id) == m_pubnonces.end()) return false;

    m_partial_sigs[oracle_id] = partial_sig;
    return true;
}

// RC30: verifying wrapper — used by orchestrator to reject partial sigs
// signed under a different keyagg_cache (participant-set divergence).
// Tests use AddPartialSignature directly without verification.
bool MuSig2SigningSession::AddPartialSignatureVerified(uint8_t oracle_id,
                                                       const secp256k1_musig_partial_sig& partial_sig,
                                                       const secp256k1_pubkey& signer_pk)
{
    LOCK(m_mutex);

    if (m_state != MuSig2SessionState::SIGNING) return false;

    static const unsigned char partial_sig_magic[4] = {0xeb, 0xfb, 0x1a, 0x32};
    if (memcmp(partial_sig.data, partial_sig_magic, 4) != 0) return false;

    if (m_partial_sigs.count(oracle_id)) return false;

    auto pubnonce_it = m_pubnonces.find(oracle_id);
    if (pubnonce_it == m_pubnonces.end()) return false;

    if (!secp256k1_musig_partial_sig_verify(m_ctx,
                                            &partial_sig,
                                            &pubnonce_it->second,
                                            &signer_pk,
                                            &m_keyagg_cache,
                                            &m_session)) {
        return false;
    }

    m_partial_sigs[oracle_id] = partial_sig;
    return true;
}

bool MuSig2SigningSession::HasEnoughPartialSigs() const
{
    LOCK(m_mutex);
    return m_partial_sigs.size() >= m_min_signers;
}

size_t MuSig2SigningSession::GetPartialSigCount() const
{
    LOCK(m_mutex);
    return m_partial_sigs.size();
}

// ============================================================================
// Aggregate partial signatures → final 64-byte Schnorr signature
// ============================================================================

bool MuSig2SigningSession::AggregateSignature(std::vector<unsigned char>& sig64_out)
{
    LOCK(m_mutex);

    if (m_state != MuSig2SessionState::SIGNING) return false;
    if (m_partial_sigs.size() < m_min_signers) return false;

    // Build sorted array of partial sig pointers (sorted by oracle_id via std::map)
    std::vector<const secp256k1_musig_partial_sig*> psig_ptrs;
    psig_ptrs.reserve(m_partial_sigs.size());
    for (const auto& [id, sig] : m_partial_sigs) {
        psig_ptrs.push_back(&sig);
    }

    sig64_out.resize(64);
    if (!secp256k1_musig_partial_sig_agg(m_ctx, sig64_out.data(),
                                          &m_session, psig_ptrs.data(),
                                          psig_ptrs.size())) {
        m_state = MuSig2SessionState::FAILED;
        return false;
    }

    m_aggregate_sig = sig64_out;
    m_state = MuSig2SessionState::COMPLETE;
    return true;
}

std::vector<unsigned char> MuSig2SigningSession::GetAggregateSig() const
{
    LOCK(m_mutex);
    if (m_state != MuSig2SessionState::COMPLETE) return {};
    return m_aggregate_sig;
}

std::vector<unsigned char> MuSig2SigningSession::GetParticipationBitmap() const
{
    LOCK(m_mutex);
    if (m_state != MuSig2SessionState::COMPLETE) return {};
    if (m_partial_sigs.empty()) return {};

    // Bitmap must be sized for total oracle count, not just max participating ID.
    // DecodeBitmap expects exactly (nOracleTotalOracles + 7) / 8 bytes.
    const uint16_t total_oracles = ConfiguredMuSig2BitmapSlots();
    if (total_oracles == 0) return {};
    size_t bitmap_bytes = (total_oracles + 7) / 8;
    std::vector<unsigned char> bitmap(bitmap_bytes, 0);
    for (const auto& [id, sig] : m_partial_sigs) {
        if (id < total_oracles) {
            bitmap[id / 8] |= (1 << (id % 8));
        }
    }
    return bitmap;
}

void MuSig2SigningSession::SetSignedValues(uint64_t price, int64_t timestamp)
{
    LOCK(m_mutex);
    m_signed_price = price;
    m_signed_timestamp = timestamp;
}

uint64_t MuSig2SigningSession::GetSignedPrice() const
{
    LOCK(m_mutex);
    return m_signed_price;
}

int64_t MuSig2SigningSession::GetSignedTimestamp() const
{
    LOCK(m_mutex);
    return m_signed_timestamp;
}

uint256 MuSig2SigningSession::GetSessionContextId() const
{
    LOCK(m_mutex);
    return m_session_context_id;
}

uint256 MuSig2SigningSession::GetNonceSetHash() const
{
    LOCK(m_mutex);
    return m_nonce_set_hash;
}

uint256 MuSig2SigningSession::GetMessageHash() const
{
    LOCK(m_mutex);
    return m_message_hash;
}

// ============================================================================
// Timeout management
// ============================================================================

void MuSig2SigningSession::CheckTimeout(int32_t current_height)
{
    LOCK(m_mutex);

    if (m_state == MuSig2SessionState::COMPLETE ||
        m_state == MuSig2SessionState::FAILED) {
        return;
    }

    if (current_height >= m_creation_height + m_timeout_blocks) {
        // Zero all held secret nonces on timeout
        for (auto& [id, nonce] : m_secnonces) {
            memory_cleanse(&nonce, sizeof(nonce));
        }
        m_secnonces.clear();
        m_state = MuSig2SessionState::FAILED;
    }
}

void MuSig2SigningSession::SetTimeoutBlocks(int32_t blocks)
{
    LOCK(m_mutex);
    m_timeout_blocks = blocks;
}

int32_t MuSig2SigningSession::GetCreationHeight() const
{
    LOCK(m_mutex);
    return m_creation_height;
}
