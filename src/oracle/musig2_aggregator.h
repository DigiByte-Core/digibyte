// Copyright (c) 2024-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_ORACLE_MUSIG2_AGGREGATOR_H
#define DIGIBYTE_ORACLE_MUSIG2_AGGREGATOR_H

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_musig.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <vector>

namespace Consensus {
struct Params;
}

/**
 * MuSig2 Oracle Key Aggregator
 *
 * Wraps the secp256k1 MuSig2 API for oracle-specific key aggregation.
 * Given a subset of oracle IDs, sorts by ID, looks up their compressed
 * pubkeys from chainparams, and calls secp256k1_musig_pubkey_agg to
 * compute the aggregate x-only pubkey.
 *
 * Features:
 * - Variable-length bitmap encoding for oracle participation sets
 * - secp256k1_musig_pubkey_agg for BIP-327 compliant key aggregation
 * - Thread-safe cache bound to the supplied chain, roster, and bitmap
 *
 * This class ONLY handles key aggregation + bitmap encoding.
 * No signing, no sessions, no nonce management.
 */
class MuSig2OracleAggregator {
public:
    MuSig2OracleAggregator();
    explicit MuSig2OracleAggregator(size_t max_cache_entries);
    ~MuSig2OracleAggregator();

    MuSig2OracleAggregator(const MuSig2OracleAggregator&) = delete;
    MuSig2OracleAggregator& operator=(const MuSig2OracleAggregator&) = delete;

    /** Encode participating oracle IDs into a variable-length bitmap.
     *  @param[in] oracle_ids  IDs of participating oracles (0-255)
     *  @param[in] total_oracles  Total oracle slots (1-256)
     *  @return Bitmap bytes, or empty vector on validation failure
     *          (empty set, below ORACLE_CONSENSUS_REQUIRED, invalid ID, duplicate) */
    static std::vector<unsigned char> EncodeBitmap(const std::vector<uint8_t>& oracle_ids, uint16_t total_oracles);

    /** Decode bitmap back to sorted oracle IDs.
     *  @return Sorted oracle IDs, or empty vector if bitmap is malformed */
    static std::vector<uint8_t> DecodeBitmap(const std::vector<unsigned char>& bitmap, uint16_t total_oracles);

    /** Compute aggregate pubkey for a set of oracle IDs using active consensus parameters.
     *  Looks up the full compressed keys, sorts by oracle_id,
     *  calls secp256k1_musig_pubkey_agg.
     *  @return true on success */
    bool ComputeAggregatePubkey(const std::vector<uint8_t>& oracle_ids,
                                 secp256k1_xonly_pubkey& agg_pk,
                                 secp256k1_musig_keyagg_cache& cache);

    /** Aggregate using only the supplied roster, including its compressed key parity.
     *  IDs are sorted and deduplicated before the supplied quorum is checked. */
    bool ComputeAggregatePubkey(const std::vector<uint8_t>& oracle_ids,
                                const Consensus::Params& params,
                                secp256k1_xonly_pubkey& agg_pk,
                                secp256k1_musig_keyagg_cache& cache);

    /** Compute aggregate pubkey from a participation bitmap. */
    bool ComputeAggregatePubkeyFromBitmap(const std::vector<unsigned char>& bitmap,
                                           uint16_t total_oracles,
                                           secp256k1_xonly_pubkey& agg_pk,
                                           secp256k1_musig_keyagg_cache& cache);

    /** Decode and aggregate using the supplied slot count, quorum, and full keys. */
    bool ComputeAggregatePubkeyFromBitmap(const std::vector<unsigned char>& bitmap,
                                          const Consensus::Params& params,
                                          secp256k1_xonly_pubkey& agg_pk,
                                          secp256k1_musig_keyagg_cache& cache);

    /** Cache lookup by bitmap. Returns false if not cached. */
    bool GetCachedAggregatePubkey(const std::vector<unsigned char>& bitmap,
                                   secp256k1_xonly_pubkey& agg_pk);

    /** Cache lookup bound to the supplied chain and roster. */
    bool GetCachedAggregatePubkey(const std::vector<unsigned char>& bitmap,
                                  const Consensus::Params& params,
                                  secp256k1_xonly_pubkey& agg_pk);

    /** Aggregate explicitly-provided parsed pubkeys (no chainparams, no cache).
     *  Pubkeys are aggregated in the order given; caller must ensure ordering.
     *  @return true on success */
    bool AggregatePubkeys(const secp256k1_pubkey* const* pubkeys,
                           size_t n_pubkeys,
                           secp256k1_xonly_pubkey& agg_pk,
                           secp256k1_musig_keyagg_cache& cache);

    /** Clear the aggregate pubkey cache. */
    void ClearCache();

private:
    secp256k1_context* m_ctx;  //!< secp256k1 context for EC operations
    size_t m_max_cache_entries{1024};  //!< Maximum LRU cache size

    //! Cache: chain/roster/bitmap hash -> (aggregate_xonly_pk, keyagg_cache)
    std::map<uint256, std::pair<secp256k1_xonly_pubkey, secp256k1_musig_keyagg_cache>> m_cache GUARDED_BY(m_cache_mutex);
    mutable Mutex m_cache_mutex;

    /** Bind cached aggregates to all supplied key selection inputs. */
    static uint256 ComputeCacheHash(const std::vector<unsigned char>& bitmap,
                                    const Consensus::Params& params);
};

#endif // DIGIBYTE_ORACLE_MUSIG2_AGGREGATOR_H
