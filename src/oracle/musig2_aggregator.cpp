// Copyright (c) 2024-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <oracle/musig2_aggregator.h>

#include <chainparams.h>
#include <hash.h>
#include <logging.h>
#include <primitives/oracle.h>
#include <pubkey.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>

namespace {

int ConfiguredMuSig2Threshold(const Consensus::Params& consensus)
{
    if (consensus.nOracleConsensusRequired > 0) return consensus.nOracleConsensusRequired;
    return ORACLE_CONSENSUS_REQUIRED;
}

uint16_t ConfiguredMuSig2BitmapSlots(const Consensus::Params& consensus)
{
    const int configured_total = std::max(consensus.nOracleTotalOracles, consensus.nOraclePubkeyCount);
    if (configured_total <= 0 || configured_total > 256) return 0;
    return static_cast<uint16_t>(configured_total);
}

std::vector<unsigned char> EncodeBitmapForCache(
    const std::vector<uint8_t>& oracle_ids,
    uint16_t total_oracles)
{
    if (oracle_ids.empty()) return {};
    if (total_oracles == 0 || total_oracles > 256) return {};

    size_t num_bytes = (total_oracles + 7) / 8;
    std::vector<unsigned char> bitmap(num_bytes, 0);

    for (uint8_t id : oracle_ids) {
        if (id >= total_oracles) return {};
        size_t byte_idx = id / 8;
        uint8_t bit_mask = 1 << (id % 8);
        if (bitmap[byte_idx] & bit_mask) return {};
        bitmap[byte_idx] |= bit_mask;
    }

    return bitmap;
}

} // namespace

MuSig2OracleAggregator::MuSig2OracleAggregator()
{
    m_ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    assert(m_ctx != nullptr);
}

MuSig2OracleAggregator::MuSig2OracleAggregator(size_t max_cache_entries)
    : m_max_cache_entries(max_cache_entries)
{
    m_ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    assert(m_ctx != nullptr);
}

MuSig2OracleAggregator::~MuSig2OracleAggregator()
{
    if (m_ctx) {
        secp256k1_context_destroy(m_ctx);
        m_ctx = nullptr;
    }
}

// ============================================================================
// Bitmap Operations
// ============================================================================

std::vector<unsigned char> MuSig2OracleAggregator::EncodeBitmap(
    const std::vector<uint8_t>& oracle_ids, uint16_t total_oracles)
{
    if (oracle_ids.empty()) return {};
    if (static_cast<int>(oracle_ids.size()) < ConfiguredMuSig2Threshold(Params().GetConsensus())) return {};
    if (total_oracles == 0 || total_oracles > 256) return {};

    size_t num_bytes = (total_oracles + 7) / 8;
    std::vector<unsigned char> bitmap(num_bytes, 0);

    for (uint8_t id : oracle_ids) {
        if (id >= total_oracles) return {};
        size_t byte_idx = id / 8;
        uint8_t bit_mask = 1 << (id % 8);
        if (bitmap[byte_idx] & bit_mask) return {}; // reject duplicate
        bitmap[byte_idx] |= bit_mask;
    }

    return bitmap;
}

std::vector<uint8_t> MuSig2OracleAggregator::DecodeBitmap(
    const std::vector<unsigned char>& bitmap, uint16_t total_oracles)
{
    if (bitmap.empty()) return {};
    if (total_oracles == 0 || total_oracles > 256) return {};

    size_t expected_bytes = (total_oracles + 7) / 8;
    if (bitmap.size() != expected_bytes) return {};

    const uint16_t used_bits_in_last_byte = total_oracles % 8;
    if (used_bits_in_last_byte != 0) {
        const unsigned char unused_mask = static_cast<unsigned char>(0xffU << used_bits_in_last_byte);
        if ((bitmap.back() & unused_mask) != 0) return {};
    }

    std::vector<uint8_t> oracle_ids;
    for (uint16_t i = 0; i < total_oracles; ++i) {
        if (bitmap[i / 8] & (1 << (i % 8))) {
            oracle_ids.push_back(static_cast<uint8_t>(i));
        }
    }

    return oracle_ids;
}

// ============================================================================
// Key Aggregation
// ============================================================================

bool MuSig2OracleAggregator::ComputeAggregatePubkey(
    const std::vector<uint8_t>& oracle_ids,
    secp256k1_xonly_pubkey& agg_pk,
    secp256k1_musig_keyagg_cache& cache)
{
    return ComputeAggregatePubkey(oracle_ids, Params().GetConsensus(), agg_pk, cache);
}

bool MuSig2OracleAggregator::ComputeAggregatePubkey(
    const std::vector<uint8_t>& oracle_ids,
    const Consensus::Params& consensus,
    secp256k1_xonly_pubkey& agg_pk,
    secp256k1_musig_keyagg_cache& cache)
{
    // Sort and deduplicate oracle IDs for deterministic aggregation
    std::vector<uint8_t> sorted_ids = oracle_ids;
    std::sort(sorted_ids.begin(), sorted_ids.end());
    sorted_ids.erase(std::unique(sorted_ids.begin(), sorted_ids.end()), sorted_ids.end());

    int required = std::max(1, ConfiguredMuSig2Threshold(consensus));
    if (static_cast<int>(sorted_ids.size()) < required) return false;

    uint16_t total = ConfiguredMuSig2BitmapSlots(consensus);
    if (total == 0) return false;
    if (consensus.vOracleCompressedPublicKeys.size() < static_cast<size_t>(total)) return false;
    if (consensus.nOraclePubkeyCount <= 0 ||
        consensus.vOraclePublicKeys.size() < static_cast<size_t>(consensus.nOraclePubkeyCount)) {
        return false;
    }

    // Encode bitmap for caching
    auto bitmap = EncodeBitmapForCache(sorted_ids, total);
    if (bitmap.empty()) return false;

    // Check cache first
    uint256 hash = ComputeCacheHash(bitmap, consensus);
    {
        LOCK(m_cache_mutex);
        auto it = m_cache.find(hash);
        if (it != m_cache.end()) {
            agg_pk = it->second.first;
            cache = it->second.second;
            return true;
        }
    }

    // Full points, including parity, belong to the supplied parameter snapshot.
    std::vector<secp256k1_pubkey> pubkeys;
    pubkeys.reserve(sorted_ids.size());
    for (uint8_t id : sorted_ids) {
        if (id >= consensus.nOraclePubkeyCount) {
            LogPrintf("Oracle: ComputeAggregatePubkey: oracle id %d outside active roster size %d\n",
                     id, consensus.nOraclePubkeyCount);
            return false;
        }
        const auto& compressed = consensus.vOracleCompressedPublicKeys[id];
        if (compressed.size() != CPubKey::COMPRESSED_SIZE ||
            (compressed[0] != 0x02 && compressed[0] != 0x03)) {
            LogPrintf("Oracle: ComputeAggregatePubkey: oracle %d compressed pubkey invalid\n", id);
            return false;
        }
        const auto xonly = ParseHex(consensus.vOraclePublicKeys[id]);
        if (xonly.size() != 32 || !std::equal(xonly.begin(), xonly.end(), compressed.begin() + 1)) {
            LogPrintf("Oracle: ComputeAggregatePubkey: oracle %d compressed and x-only keys differ\n", id);
            return false;
        }

        secp256k1_pubkey pk;
        if (!secp256k1_ec_pubkey_parse(m_ctx, &pk, compressed.data(), compressed.size())) {
            LogPrintf("Oracle: ComputeAggregatePubkey: secp256k1_ec_pubkey_parse failed for oracle %d, key=%s\n",
                     id, HexStr(compressed));
            return false;
        }
        pubkeys.push_back(pk);
    }

    // Build pointer array for secp256k1_musig_pubkey_agg
    std::vector<const secp256k1_pubkey*> pubkey_ptrs(pubkeys.size());
    for (size_t i = 0; i < pubkeys.size(); ++i) {
        pubkey_ptrs[i] = &pubkeys[i];
    }

    // BIP-327 key aggregation via secp256k1 MuSig2 module
    if (!secp256k1_musig_pubkey_agg(m_ctx, &agg_pk, &cache,
                                     pubkey_ptrs.data(), pubkey_ptrs.size())) {
        LogPrintf("Oracle: ComputeAggregatePubkey: secp256k1_musig_pubkey_agg failed for %zu keys\n",
                 pubkey_ptrs.size());
        return false;
    }
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: ComputeAggregatePubkey: SUCCESS for %zu oracles\n", sorted_ids.size());

    // Store in cache with LRU eviction
    {
        LOCK(m_cache_mutex);
        if (m_cache.size() >= m_max_cache_entries) {
            // Simple eviction: clear half the cache when full.
            // A proper LRU would track access order, but for oracle
            // aggregation the working set is small enough that clearing
            // half is acceptable. This prevents unbounded memory growth
            // from an attacker querying many oracle subsets.
            size_t to_remove = m_cache.size() / 2;
            auto it = m_cache.begin();
            while (to_remove > 0 && it != m_cache.end()) {
                it = m_cache.erase(it);
                --to_remove;
            }
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Aggregate pubkey cache evicted (size was %zu, max %zu)\n",
                     m_cache.size() + to_remove, m_max_cache_entries);
        }
        m_cache[hash] = {agg_pk, cache};
    }

    return true;
}

bool MuSig2OracleAggregator::ComputeAggregatePubkeyFromBitmap(
    const std::vector<unsigned char>& bitmap,
    uint16_t total_oracles,
    secp256k1_xonly_pubkey& agg_pk,
    secp256k1_musig_keyagg_cache& cache)
{
    auto oracle_ids = DecodeBitmap(bitmap, total_oracles);
    if (oracle_ids.empty()) return false;
    return ComputeAggregatePubkey(oracle_ids, agg_pk, cache);
}

bool MuSig2OracleAggregator::ComputeAggregatePubkeyFromBitmap(
    const std::vector<unsigned char>& bitmap,
    const Consensus::Params& params,
    secp256k1_xonly_pubkey& agg_pk,
    secp256k1_musig_keyagg_cache& cache)
{
    if (params.nOracleTotalOracles <= 0 || params.nOracleTotalOracles > 256) return false;
    const auto oracle_ids = DecodeBitmap(bitmap, static_cast<uint16_t>(params.nOracleTotalOracles));
    if (oracle_ids.empty()) return false;
    return ComputeAggregatePubkey(oracle_ids, params, agg_pk, cache);
}

bool MuSig2OracleAggregator::AggregatePubkeys(
    const secp256k1_pubkey* const* pubkeys,
    size_t n_pubkeys,
    secp256k1_xonly_pubkey& agg_pk,
    secp256k1_musig_keyagg_cache& cache)
{
    if (n_pubkeys == 0 || pubkeys == nullptr) return false;
    return secp256k1_musig_pubkey_agg(m_ctx, &agg_pk, &cache, pubkeys, n_pubkeys) == 1;
}

bool MuSig2OracleAggregator::GetCachedAggregatePubkey(
    const std::vector<unsigned char>& bitmap,
    secp256k1_xonly_pubkey& agg_pk)
{
    return GetCachedAggregatePubkey(bitmap, Params().GetConsensus(), agg_pk);
}

bool MuSig2OracleAggregator::GetCachedAggregatePubkey(
    const std::vector<unsigned char>& bitmap,
    const Consensus::Params& params,
    secp256k1_xonly_pubkey& agg_pk)
{
    uint256 hash = ComputeCacheHash(bitmap, params);
    LOCK(m_cache_mutex);
    auto it = m_cache.find(hash);
    if (it == m_cache.end()) return false;
    agg_pk = it->second.first;
    return true;
}

void MuSig2OracleAggregator::ClearCache()
{
    LOCK(m_cache_mutex);
    m_cache.clear();
}

uint256 MuSig2OracleAggregator::ComputeCacheHash(const std::vector<unsigned char>& bitmap,
                                               const Consensus::Params& params)
{
    CHashWriter hash(0);
    hash << params.hashGenesisBlock;
    hash << params.nOracleTotalOracles << params.nOraclePubkeyCount << params.nOracleConsensusRequired;
    hash << params.vOraclePublicKeys << params.vOracleCompressedPublicKeys;
    hash << bitmap;
    return hash.GetHash();
}
