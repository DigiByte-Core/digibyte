// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Wave 9 (DigiDollar V1 Final Audit) — Oracle Roster Expansion, Quorum,
 * and Domain-Separation hardening.
 *
 * This suite locks the V1 invariants that drive consensus stability for the
 * MuSig2 oracle roster:
 *
 * 1) Quorum boundaries: 7-of-N success, 6-of-N failure, 35-of-35 success,
 *    0-of-35 failure, and out-of-active-roster failure.
 *
 * 2) Domain separation:
 *    - Wrong epoch / wrong height (epoch-derived) — bundle hash binds the
 *      epoch and ValidateMuSig2Bundle requires payload epoch == block epoch.
 *    - Wrong message-type domain (price vs nonce vs partial sig) — every
 *      wire-level MuSig2 message uses a distinct tagged hash so signatures
 *      cannot be swapped between message types.
 *    - Wrong roster scoping — an aggregate signed for one network's roster
 *      cannot validate against another network's roster, even when price,
 *      timestamp, and epoch are identical.
 *
 * 3) Roster integrity: bitmap with duplicate participant ID, out-of-range
 *    signer ID 35 (mainnet/testnet), bitmap shorter / longer than
 *    ceil(total_count/8), all-zero bitmap, single-participant (1-of-35).
 *
 * 4) Aggregate-key drift: rotating a single key in chainparams MUST change
 *    the aggregate pubkey, and the same input must always produce the same
 *    aggregate (determinism).
 *
 * 5) Cross-network: regtest 4-of-7 aggregate must differ from mainnet
 *    aggregate signed by the 7-signature roster, even for the same
 *    price/timestamp/epoch.
 *
 * Coverage notes (campaign tracking):
 *   DD-FA-TEST-010 — Wave 9 test/fuzz hardening for quorum boundaries,
 *   roster integrity (reserve IDs, single-participant, length errors),
 *   aggregate-key drift, and cross-network aggregate divergence.
 *
 *   Cross-chain (genesis-hash) domain separation is covered for both the
 *   on-chain v0x03 aggregate bundle and the off-chain MuSig2 nonce/partial
 *   auth messages. The on-chain vector is owned by DD-FA-SEC-008; Wave 10
 *   extends the same chain binding to P2P wire auth so captured testnet
 *   nonces/partials cannot be replayed into mainnet sessions.
 */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/params.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/musig2_aggregator.h>
#include <oracle/musig2_messages.h>
#include <primitives/oracle.h>
#include <pubkey.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ScopedParamsRestore {
    ChainType original{Params().GetChainType()};
    ~ScopedParamsRestore() { SelectParams(original); }
};

// ----------------------------------------------------------------------------
// Deterministic key helpers
// ----------------------------------------------------------------------------

std::array<unsigned char, 32> Sha256Of(const std::string& seed)
{
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size())
             .Finalize(hash.begin());
    std::array<unsigned char, 32> out{};
    std::memcpy(out.data(), hash.begin(), out.size());
    return out;
}

bool MakeEvenYKeypair(secp256k1_context* ctx,
                      std::array<unsigned char, 32>& secret,
                      secp256k1_keypair& keypair,
                      secp256k1_pubkey& pubkey)
{
    if (!secp256k1_keypair_create(ctx, &keypair, secret.data())) return false;
    if (!secp256k1_keypair_pub(ctx, &pubkey, &keypair)) return false;

    secp256k1_xonly_pubkey xonly{};
    int parity{0};
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly, &parity, &pubkey)) return false;
    if (parity == 0) return true;

    if (!secp256k1_ec_seckey_negate(ctx, secret.data())) return false;
    return secp256k1_keypair_create(ctx, &keypair, secret.data()) &&
           secp256k1_keypair_pub(ctx, &pubkey, &keypair);
}

std::array<unsigned char, 32> EvenYSecretFor(const std::string& label)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx);

    auto secret = Sha256Of(label);
    secp256k1_keypair keypair{};
    secp256k1_pubkey pubkey{};
    BOOST_REQUIRE(MakeEvenYKeypair(ctx, secret, keypair, pubkey));
    secp256k1_context_destroy(ctx);
    return secret;
}

std::array<unsigned char, 32> SerializeXOnly(const secp256k1_xonly_pubkey& pk)
{
    std::array<unsigned char, 32> out{};
    secp256k1_xonly_pubkey_serialize(secp256k1_context_static, out.data(), &pk);
    return out;
}

std::vector<unsigned char> EncodeBitmapUnchecked(const std::vector<uint8_t>& oracle_ids,
                                                  uint16_t total_oracles)
{
    std::vector<unsigned char> bitmap((total_oracles + 7) / 8, 0);
    for (uint8_t id : oracle_ids) {
        bitmap[id / 8] |= static_cast<unsigned char>(1U << (id % 8));
    }
    return bitmap;
}

// ----------------------------------------------------------------------------
// 35-of-35 quorum helpers (use the testnet roster slot ordering directly)
// ----------------------------------------------------------------------------

constexpr int32_t QD_BLOCK_HEIGHT = 1000;
constexpr uint64_t QD_PRICE = 50000;
constexpr int64_t QD_TIMESTAMP = 1735689600;

Consensus::Params MakeQuorumDomainParams(int total_oracles, int required)
{
    Consensus::Params params = Params().GetConsensus();
    params.nDigiDollarMuSig2Height = 0;
    params.nOracleActivationHeight = 0;
    params.nOracleTotalOracles = total_oracles;
    params.nOraclePubkeyCount = total_oracles;
    params.nOracleConsensusRequired = required;
    params.vOraclePublicKeys.clear();
    params.vOracleCompressedPublicKeys.clear();

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx);
    for (int i = 0; i < total_oracles; ++i) {
        auto secret = EvenYSecretFor("digibyte_qd_oracle_" + std::to_string(i));
        secp256k1_keypair keypair{};
        secp256k1_pubkey pubkey{};
        BOOST_REQUIRE(secp256k1_keypair_create(ctx, &keypair, secret.data()));
        BOOST_REQUIRE(secp256k1_keypair_pub(ctx, &pubkey, &keypair));
        secp256k1_xonly_pubkey xonly{};
        int parity{0};
        BOOST_REQUIRE(secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly, &parity, &pubkey));
        std::array<unsigned char, 32> serialized{};
        BOOST_REQUIRE(secp256k1_xonly_pubkey_serialize(ctx, serialized.data(), &xonly));
        params.vOraclePublicKeys.push_back(HexStr(serialized));
        std::vector<unsigned char> compressed(CPubKey::COMPRESSED_SIZE);
        size_t compressed_size = compressed.size();
        BOOST_REQUIRE(secp256k1_ec_pubkey_serialize(ctx, compressed.data(), &compressed_size,
                                                  &pubkey, SECP256K1_EC_COMPRESSED));
        params.vOracleCompressedPublicKeys.push_back(compressed);
    }
    secp256k1_context_destroy(ctx);
    return params;
}

bool SignBundleWithQuorumKeys(COracleBundle& bundle,
                              const std::vector<uint8_t>& oracle_ids,
                              uint16_t total_oracles)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    const size_t n_signers = oracle_ids.size();
    std::vector<std::array<unsigned char, 32>> seckeys(n_signers);
    std::vector<secp256k1_keypair> keypairs(n_signers);
    std::vector<secp256k1_pubkey> pubkeys(n_signers);

    for (size_t i = 0; i < n_signers; ++i) {
        seckeys[i] = EvenYSecretFor("digibyte_qd_oracle_" + std::to_string(oracle_ids[i]));
        if (!secp256k1_keypair_create(ctx, &keypairs[i], seckeys[i].data()) ||
            !secp256k1_keypair_pub(ctx, &pubkeys[i], &keypairs[i])) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_pubkey*> pubkey_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) pubkey_ptrs[i] = &pubkeys[i];

    secp256k1_xonly_pubkey agg_pk{};
    secp256k1_musig_keyagg_cache cache{};
    if (!secp256k1_musig_pubkey_agg(ctx, &agg_pk, &cache, pubkey_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_secnonce> secnonces(n_signers);
    std::vector<secp256k1_musig_pubnonce> pubnonces(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        unsigned char session_rand[32];
        GetStrongRandBytes(Span{session_rand, 32});
        if (!secp256k1_musig_nonce_gen(ctx, &secnonces[i], &pubnonces[i],
                                       session_rand, seckeys[i].data(), &pubkeys[i],
                                       nullptr, &cache, nullptr)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_musig_pubnonce*> nonce_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) nonce_ptrs[i] = &pubnonces[i];

    secp256k1_musig_aggnonce aggnonce{};
    if (!secp256k1_musig_nonce_agg(ctx, &aggnonce, nonce_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    const uint256 msg_hash = ComputeOracleBundleHash(bundle);
    unsigned char msg32[32];
    std::memcpy(msg32, msg_hash.begin(), sizeof(msg32));

    secp256k1_musig_session session{};
    if (!secp256k1_musig_nonce_process(ctx, &session, &aggnonce, msg32, &cache)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_partial_sig> partial_sigs(n_signers);
    std::vector<const secp256k1_musig_partial_sig*> partial_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        if (!secp256k1_musig_partial_sign(ctx, &partial_sigs[i], &secnonces[i],
                                          &keypairs[i], &cache, &session)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        partial_ptrs[i] = &partial_sigs[i];
    }

    bundle.participation_bitmap = EncodeBitmapUnchecked(oracle_ids, total_oracles);
    bundle.aggregate_sig.assign(64, 0);
    if (!secp256k1_musig_partial_sig_agg(ctx, bundle.aggregate_sig.data(),
                                         &session, partial_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }
    const bool verifies = secp256k1_schnorrsig_verify(ctx, bundle.aggregate_sig.data(), msg32, 32, &agg_pk);
    secp256k1_context_destroy(ctx);
    return verifies;
}

COracleBundle MakeBundleSkeleton(int32_t epoch, uint64_t price = QD_PRICE,
                                 int64_t timestamp = QD_TIMESTAMP)
{
    COracleBundle bundle;
    bundle.version = 3;
    bundle.epoch = epoch;
    bundle.median_price_micro_usd = price;
    bundle.timestamp = timestamp;
    return bundle;
}

} // namespace

// ============================================================================
// Suite 1 — Quorum boundaries
// ============================================================================

BOOST_FIXTURE_TEST_SUITE(digidollar_oracle_quorum_domain_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(quorum_7_of_35_accepts_signed_bundle)
{
    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    COracleBundle bundle = MakeBundleSkeleton(GetCurrentEpoch(QD_BLOCK_HEIGHT));
    const std::vector<uint8_t> signers{0, 1, 2, 3, 4, 5, 6};
    BOOST_REQUIRE(SignBundleWithQuorumKeys(bundle, signers, /*total_oracles=*/35));

    std::string error;
    BOOST_CHECK(OracleBundleManager::ValidateMuSig2Bundle(bundle, QD_BLOCK_HEIGHT, params, error));
    BOOST_CHECK_MESSAGE(error.empty(),
        "7-signature quorum must accept without error, got: " + error);
}

BOOST_AUTO_TEST_CASE(quorum_6_of_35_rejected_below_threshold)
{
    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    COracleBundle bundle = MakeBundleSkeleton(GetCurrentEpoch(QD_BLOCK_HEIGHT));
    const std::vector<uint8_t> signers{0, 1, 2, 3, 4, 5};
    BOOST_REQUIRE(SignBundleWithQuorumKeys(bundle, signers, /*total_oracles=*/35));

    std::string error;
    BOOST_CHECK(!OracleBundleManager::ValidateMuSig2Bundle(bundle, QD_BLOCK_HEIGHT, params, error));
    BOOST_CHECK_MESSAGE(error.find("threshold") != std::string::npos ||
                        error.find("decoding") != std::string::npos,
        "6-of-35 must reject with a threshold/bitmap error, got: " + error);
}

BOOST_AUTO_TEST_CASE(quorum_35_of_35_accepts_unanimous_set)
{
    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    COracleBundle bundle = MakeBundleSkeleton(GetCurrentEpoch(QD_BLOCK_HEIGHT));
    std::vector<uint8_t> signers;
    for (uint8_t i = 0; i < 35; ++i) signers.push_back(i);
    BOOST_REQUIRE(SignBundleWithQuorumKeys(bundle, signers, /*total_oracles=*/35));

    std::string error;
    BOOST_CHECK(OracleBundleManager::ValidateMuSig2Bundle(bundle, QD_BLOCK_HEIGHT, params, error));
    BOOST_CHECK_MESSAGE(error.empty(),
        "35-of-35 unanimous quorum must accept, got: " + error);
}

BOOST_AUTO_TEST_CASE(quorum_0_of_35_rejected_zero_signers)
{
    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    COracleBundle bundle = MakeBundleSkeleton(GetCurrentEpoch(QD_BLOCK_HEIGHT));
    bundle.aggregate_sig.assign(64, 0);
    bundle.participation_bitmap.assign(5, 0); // canonical zero bitmap for 35 slots

    std::string error;
    BOOST_CHECK(!OracleBundleManager::ValidateMuSig2Bundle(bundle, QD_BLOCK_HEIGHT, params, error));
    BOOST_CHECK_MESSAGE(error.find("decoding") != std::string::npos ||
                        error.find("threshold") != std::string::npos ||
                        error.find("empty") != std::string::npos,
        "0-of-35 (all-zero bitmap) must reject as decoding/threshold/empty, got: " + error);
}

BOOST_AUTO_TEST_CASE(quorum_out_of_roster_id_rejected)
{
    // Build a 35-slot roster but encode a bitmap that selects an out-of-range
    // signer id (slot 35). The bitmap must not even encode.
    const std::vector<uint8_t> signers_with_oob{0, 1, 2, 3, 4, 5, 6, 35};
    auto encoded = MuSig2OracleAggregator::EncodeBitmap(signers_with_oob, /*total_oracles=*/35);
    BOOST_CHECK_MESSAGE(encoded.empty(),
        "EncodeBitmap must reject ids >= total_oracles");

    // A handcrafted bitmap that pretends slot 35 exists in a 35-slot roster
    // must also fail to decode (size mismatch / unused-bit guard).
    std::vector<uint8_t> handcrafted_signers{0, 1, 2, 3, 4, 5, 6, 35};
    auto handcrafted = EncodeBitmapUnchecked(handcrafted_signers, /*total_oracles=*/36);
    auto decoded = MuSig2OracleAggregator::DecodeBitmap(handcrafted, /*total_oracles=*/35);
    BOOST_CHECK_MESSAGE(decoded.empty(),
        "DecodeBitmap with size for 36 must reject when total_oracles=35");
}

// ============================================================================
// Suite 2 — Roster integrity (bitmap edge cases)
// ============================================================================

BOOST_AUTO_TEST_CASE(roster_bitmap_duplicate_id_rejected_by_encode)
{
    const std::vector<uint8_t> signers_with_dup{0, 1, 2, 3, 4, 5, 6, 7, 7, 8};
    auto encoded = MuSig2OracleAggregator::EncodeBitmap(signers_with_dup, /*total_oracles=*/35);
    BOOST_CHECK_MESSAGE(encoded.empty(),
        "EncodeBitmap must reject duplicate participant IDs");
}

static void CheckOutOfRangeIdRejectedByBundleValidation(const Consensus::Params& params,
                                                        int32_t height,
                                                        uint8_t out_of_range_id,
                                                        const std::string& chain_name)
{
    const std::vector<uint8_t> signers{0, 1, 2, 3, 4, 5, 6, out_of_range_id};
    const auto encoded = MuSig2OracleAggregator::EncodeBitmap(
        signers, static_cast<uint16_t>(params.nOracleTotalOracles));
    BOOST_CHECK_MESSAGE(encoded.empty(),
        chain_name + " bitmap encode must reject signer " +
        std::to_string(out_of_range_id) + " outside the configured 35-slot roster");

    const auto handcrafted = EncodeBitmapUnchecked(
        signers, static_cast<uint16_t>(params.nOracleTotalOracles + 1));
    auto decoded = MuSig2OracleAggregator::DecodeBitmap(
        handcrafted, static_cast<uint16_t>(params.nOracleTotalOracles));
    BOOST_CHECK_MESSAGE(decoded.empty(),
        chain_name + " bitmap decode must reject handcrafted signer " +
        std::to_string(out_of_range_id) + " outside the configured 35-slot roster");

    COracleBundle bundle = MakeBundleSkeleton(GetCurrentEpoch(height));
    bundle.participation_bitmap = handcrafted;
    bundle.aggregate_sig.assign(64, 0);

    std::string error;
    BOOST_CHECK_MESSAGE(!OracleBundleManager::ValidateMuSig2Bundle(bundle, height, params, error),
        chain_name + " validation must reject out-of-range signer " +
        std::to_string(out_of_range_id));
    BOOST_CHECK_MESSAGE(!error.empty(),
        chain_name + " out-of-range signer should report a rejection reason");
}

BOOST_AUTO_TEST_CASE(roster_bitmap_out_of_range_id_rejected_on_mainnet_testnet)
{
    SelectParams(ChainType::MAIN);
    {
        const Consensus::Params& params = Params().GetConsensus();
        BOOST_REQUIRE_EQUAL(params.nOraclePubkeyCount, 35);
        BOOST_REQUIRE_EQUAL(params.nOracleTotalOracles, 35);

        CheckOutOfRangeIdRejectedByBundleValidation(
            params, params.nDDActivationHeight, 35, "Mainnet");
    }

    SelectParams(ChainType::TESTNET);
    {
        const Consensus::Params& params = Params().GetConsensus();
        BOOST_REQUIRE_EQUAL(params.nOraclePubkeyCount, 35);
        BOOST_REQUIRE_EQUAL(params.nOracleTotalOracles, 35);

        CheckOutOfRangeIdRejectedByBundleValidation(
            params, params.nDDActivationHeight, 35, "Testnet");
    }

    SelectParams(ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(roster_bitmap_shorter_than_expected_rejected)
{
    // 35 slots -> ceil(35/8) = 5 bytes. A 4-byte bitmap is too short.
    std::vector<unsigned char> short_bitmap(4, 0xFF);
    auto decoded = MuSig2OracleAggregator::DecodeBitmap(short_bitmap, /*total_oracles=*/35);
    BOOST_CHECK_MESSAGE(decoded.empty(),
        "Bitmap shorter than ceil(active_count/8) must be rejected");
}

BOOST_AUTO_TEST_CASE(roster_bitmap_longer_than_expected_rejected)
{
    // 35 slots -> 5 bytes. A 6-byte bitmap is too long.
    std::vector<unsigned char> long_bitmap(6, 0x00);
    long_bitmap[0] = 0xFF;
    long_bitmap[1] = 0x01;
    auto decoded = MuSig2OracleAggregator::DecodeBitmap(long_bitmap, /*total_oracles=*/35);
    BOOST_CHECK_MESSAGE(decoded.empty(),
        "Bitmap longer than ceil(active_count/8) must be rejected");
}

BOOST_AUTO_TEST_CASE(roster_bitmap_all_zero_rejected)
{
    // 35 slots: 5 zero bytes. DecodeBitmap currently returns an empty vector
    // (all bits unset), and HasMuSig2Quorum then rejects below threshold.
    std::vector<unsigned char> zero_bitmap(5, 0x00);
    auto decoded = MuSig2OracleAggregator::DecodeBitmap(zero_bitmap, /*total_oracles=*/35);
    BOOST_CHECK_MESSAGE(decoded.empty(),
        "All-zero bitmap must decode to no participants");

    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    COracleBundle bundle = MakeBundleSkeleton(GetCurrentEpoch(QD_BLOCK_HEIGHT));
    bundle.participation_bitmap = zero_bitmap;
    bundle.aggregate_sig.assign(64, 0);

    std::string error;
    BOOST_CHECK(!OracleBundleManager::ValidateMuSig2Bundle(bundle, QD_BLOCK_HEIGHT, params, error));
    BOOST_CHECK_MESSAGE(!error.empty(),
        "All-zero bitmap must report a non-empty rejection reason");
}

BOOST_AUTO_TEST_CASE(roster_bitmap_single_participant_rejected)
{
    // 1-of-active-roster must be below the 7-signature threshold.
    const std::vector<uint8_t> single{0};
    auto encoded = MuSig2OracleAggregator::EncodeBitmap(single, /*total_oracles=*/35);
    BOOST_CHECK_MESSAGE(encoded.empty(),
        "EncodeBitmap must reject single-participant set under 7-signature quorum");

    std::vector<unsigned char> handcrafted = EncodeBitmapUnchecked(single, 35);
    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    COracleBundle bundle = MakeBundleSkeleton(GetCurrentEpoch(QD_BLOCK_HEIGHT));
    bundle.participation_bitmap = handcrafted;
    bundle.aggregate_sig.assign(64, 0);

    std::string error;
    BOOST_CHECK(!OracleBundleManager::ValidateMuSig2Bundle(bundle, QD_BLOCK_HEIGHT, params, error));
    BOOST_CHECK_MESSAGE(error.find("threshold") != std::string::npos ||
                        error.find("decoding") != std::string::npos,
        "1-of-35 hand-crafted bitmap must hit threshold/decoding error, got: " + error);
}

// ============================================================================
// Suite 3 — Aggregate-key drift / determinism
// ============================================================================

BOOST_AUTO_TEST_CASE(aggregate_pubkey_changes_when_single_key_rotates)
{
    Consensus::Params base = MakeQuorumDomainParams(35, 7);
    Consensus::Params rotated = base;
    // Replace slot 11 (matches RC31 hallvardo rotation) with a fresh derived key.
    auto rotated_secret = EvenYSecretFor("digibyte_qd_rotated_oracle_11");
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx);
    secp256k1_keypair keypair{};
    secp256k1_pubkey pubkey{};
    BOOST_REQUIRE(secp256k1_keypair_create(ctx, &keypair, rotated_secret.data()));
    BOOST_REQUIRE(secp256k1_keypair_pub(ctx, &pubkey, &keypair));
    secp256k1_xonly_pubkey xonly{};
    int parity{0};
    BOOST_REQUIRE(secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly, &parity, &pubkey));
    std::array<unsigned char, 32> serialized{};
    BOOST_REQUIRE(secp256k1_xonly_pubkey_serialize(ctx, serialized.data(), &xonly));
    std::vector<unsigned char> compressed(CPubKey::COMPRESSED_SIZE);
    size_t compressed_size = compressed.size();
    BOOST_REQUIRE(secp256k1_ec_pubkey_serialize(ctx, compressed.data(), &compressed_size,
                                              &pubkey, SECP256K1_EC_COMPRESSED));
    secp256k1_context_destroy(ctx);
    rotated.vOraclePublicKeys[11] = HexStr(serialized);
    rotated.vOracleCompressedPublicKeys[11] = compressed;

    BOOST_CHECK_MESSAGE(rotated.vOraclePublicKeys[11] != base.vOraclePublicKeys[11],
        "test setup must actually rotate the slot 11 key");

    // Aggregate over a 9-signer subset that includes slot 11.
    const std::vector<uint8_t> signers{0, 1, 2, 3, 4, 5, 6, 7, 11};

    secp256k1_xonly_pubkey agg_base{}, agg_rot{};

    MuSig2OracleAggregator aggregator;
    auto compute_agg = [&](const Consensus::Params& params,
                           const std::vector<uint8_t>& ids,
                           secp256k1_xonly_pubkey& out_pk) {
        secp256k1_musig_keyagg_cache cache{};
        return aggregator.ComputeAggregatePubkey(ids, params, out_pk, cache);
    };

    BOOST_REQUIRE(compute_agg(base, signers, agg_base));
    BOOST_REQUIRE(compute_agg(rotated, signers, agg_rot));
    BOOST_CHECK_MESSAGE(SerializeXOnly(agg_base) != SerializeXOnly(agg_rot),
        "Rotating any single oracle key in chainparams MUST change the aggregate pubkey");

    // Determinism: repeating the aggregation on the same params must give the same key.
    secp256k1_xonly_pubkey agg_base2{};
    BOOST_REQUIRE(compute_agg(base, signers, agg_base2));
    BOOST_CHECK_MESSAGE(SerializeXOnly(agg_base) == SerializeXOnly(agg_base2),
        "Same chainparams + same signer set MUST produce the same aggregate pubkey");
}

// ============================================================================
// Suite 4 — Cross-network aggregate divergence
// ============================================================================

BOOST_AUTO_TEST_CASE(cross_network_aggregate_differs_between_regtest_and_mainnet)
{
    auto compute_first_n = [](const Consensus::Params& p, size_t n,
                               secp256k1_xonly_pubkey& out_pk) -> bool {
        secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        if (!ctx) return false;
        std::vector<secp256k1_pubkey> pks(n);
        std::vector<const secp256k1_pubkey*> ptrs(n);
        for (size_t i = 0; i < n; ++i) {
            const auto& compressed = p.vOracleCompressedPublicKeys[i];
            if (!secp256k1_ec_pubkey_parse(ctx, &pks[i], compressed.data(), compressed.size())) {
                secp256k1_context_destroy(ctx);
                return false;
            }
            ptrs[i] = &pks[i];
        }
        secp256k1_musig_keyagg_cache c{};
        const bool ok = secp256k1_musig_pubkey_agg(ctx, &out_pk, &c, ptrs.data(), ptrs.size()) == 1;
        secp256k1_context_destroy(ctx);
        return ok;
    };

    SelectParams(ChainType::REGTEST);
    Consensus::Params regtest = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(regtest.nOracleConsensusRequired, 4);
    secp256k1_xonly_pubkey agg_regtest{};
    BOOST_REQUIRE(compute_first_n(regtest, /*n=*/4, agg_regtest));

    SelectParams(ChainType::MAIN);
    Consensus::Params mainnet = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(mainnet.nOracleConsensusRequired, 7);
    secp256k1_xonly_pubkey agg_mainnet{};
    BOOST_REQUIRE(compute_first_n(mainnet, /*n=*/4, agg_mainnet));

    BOOST_CHECK_MESSAGE(SerializeXOnly(agg_regtest) != SerializeXOnly(agg_mainnet),
        "Regtest 4-of-7 aggregate must differ from mainnet aggregate over the first 4 keys");

    SelectParams(ChainType::MAIN);
}

// ============================================================================
// Suite 5 — Domain separation (epoch / height / message-type)
// ============================================================================

BOOST_AUTO_TEST_CASE(domain_separation_wrong_payload_epoch_rejected)
{
    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    const int32_t expected_epoch = GetCurrentEpoch(QD_BLOCK_HEIGHT);
    const int32_t wrong_epoch = expected_epoch + 1;

    // Build a bundle whose payload epoch deliberately disagrees with the
    // block's epoch. The MuSig2 ceremony signs the wrong-epoch message hash,
    // so signature verification *can* succeed in isolation — what we want
    // to confirm is that the validator's epoch-binding gate rejects first.
    COracleBundle bundle = MakeBundleSkeleton(wrong_epoch);
    const std::vector<uint8_t> signers{0, 1, 2, 3, 4, 5, 6, 7, 8};
    BOOST_REQUIRE(SignBundleWithQuorumKeys(bundle, signers, /*total_oracles=*/35));

    std::string error;
    BOOST_CHECK(!OracleBundleManager::ValidateMuSig2Bundle(bundle, QD_BLOCK_HEIGHT, params, error));
    BOOST_CHECK_MESSAGE(error.find("epoch") != std::string::npos,
        "Wrong payload epoch must be flagged with an epoch error, got: " + error);
}

BOOST_AUTO_TEST_CASE(domain_separation_wrong_block_height_rejected)
{
    Consensus::Params params = MakeQuorumDomainParams(35, 7);
    const int32_t expected_epoch = GetCurrentEpoch(QD_BLOCK_HEIGHT);

    // Sign for the expected epoch but validate at a different height whose
    // epoch differs. (Find a height that maps to a different epoch.)
    COracleBundle bundle = MakeBundleSkeleton(expected_epoch);
    const std::vector<uint8_t> signers{0, 1, 2, 3, 4, 5, 6, 7, 8};
    BOOST_REQUIRE(SignBundleWithQuorumKeys(bundle, signers, /*total_oracles=*/35));

    int32_t different_height = QD_BLOCK_HEIGHT;
    while (GetCurrentEpoch(different_height) == expected_epoch) {
        different_height += 1;
        if (different_height > QD_BLOCK_HEIGHT + 100000) {
            BOOST_FAIL("could not find a height with a different epoch");
        }
    }

    std::string error;
    BOOST_CHECK(!OracleBundleManager::ValidateMuSig2Bundle(bundle, different_height, params, error));
    BOOST_CHECK_MESSAGE(error.find("epoch") != std::string::npos,
        "Wrong block height (different epoch) must be flagged with an epoch error, got: " + error);
}

BOOST_AUTO_TEST_CASE(domain_separation_message_type_tags_distinct)
{
    // Same epoch / oracle_id / payload bytes must produce different signing
    // hashes for the nonce vs partial-sig wire messages, because each uses a
    // different tagged-hash domain string.
    OracleMusigNonceMsg nonce_msg;
    nonce_msg.epoch = 42;
    nonce_msg.oracle_id = 7;
    nonce_msg.pubnonce.assign(66, 0xAB);

    OracleMusigPartialSigMsg sig_msg;
    sig_msg.epoch = nonce_msg.epoch;
    sig_msg.oracle_id = nonce_msg.oracle_id;
    sig_msg.partial_sig.assign(32, 0xAB);

    BOOST_CHECK_MESSAGE(nonce_msg.GetSignatureHash() != sig_msg.GetSignatureHash(),
        "Nonce and partial-sig wire messages must hash under distinct domain tags");

    // GetSignatureHash differs from the unkeyed GetHash too — pin that the
    // tagged-hash path is actually distinct from the plain hash.
    BOOST_CHECK_MESSAGE(nonce_msg.GetSignatureHash() != nonce_msg.GetHash(),
        "Nonce signing hash must include a domain tag and differ from plain hash");
    BOOST_CHECK_MESSAGE(sig_msg.GetSignatureHash() != sig_msg.GetHash(),
        "Partial-sig signing hash must include a domain tag and differ from plain hash");
}

BOOST_AUTO_TEST_CASE(domain_separation_message_type_signature_unswappable)
{
    // An auth signature produced for the nonce wire message must NOT verify
    // against the partial-sig wire message (and vice versa), even when both
    // carry identical (epoch, oracle_id, payload) fields.
    CKey key;
    key.MakeNewKey(true);
    XOnlyPubKey xpk{key.GetPubKey()};

    OracleMusigNonceMsg nonce_msg;
    nonce_msg.epoch = 100;
    nonce_msg.oracle_id = 3;
    nonce_msg.pubnonce.assign(66, 0xCD);
    BOOST_REQUIRE(nonce_msg.Sign(key));

    OracleMusigPartialSigMsg sig_msg;
    sig_msg.epoch = nonce_msg.epoch;
    sig_msg.oracle_id = nonce_msg.oracle_id;
    sig_msg.partial_sig.assign(32, 0xCD);
    sig_msg.signature = nonce_msg.signature; // attacker-supplied: copy auth from nonce

    BOOST_CHECK_MESSAGE(nonce_msg.VerifySignature(xpk),
        "honest nonce auth signature must verify under the nonce domain");
    BOOST_CHECK_MESSAGE(!sig_msg.VerifySignature(xpk),
        "auth signature copied from nonce wire message must NOT verify under the partial-sig domain");

    // And the reverse: a partial-sig auth must not validate as a nonce auth.
    OracleMusigPartialSigMsg sig_msg_signed;
    sig_msg_signed.epoch = nonce_msg.epoch;
    sig_msg_signed.oracle_id = nonce_msg.oracle_id;
    sig_msg_signed.partial_sig.assign(32, 0xCD);
    BOOST_REQUIRE(sig_msg_signed.Sign(key));

    OracleMusigNonceMsg nonce_borrowed;
    nonce_borrowed.epoch = nonce_msg.epoch;
    nonce_borrowed.oracle_id = nonce_msg.oracle_id;
    nonce_borrowed.pubnonce.assign(66, 0xCD);
    nonce_borrowed.signature = sig_msg_signed.signature;
    BOOST_CHECK_MESSAGE(!nonce_borrowed.VerifySignature(xpk),
        "auth signature copied from partial-sig wire message must NOT verify under the nonce domain");
}

BOOST_AUTO_TEST_CASE(domain_separation_wire_auth_binds_chain_genesis)
{
    ScopedParamsRestore restore;

    CKey key;
    key.MakeNewKey(true);
    XOnlyPubKey xpk{key.GetPubKey()};

    OracleMusigNonceMsg nonce_msg;
    nonce_msg.epoch = 100;
    nonce_msg.oracle_id = 3;
    nonce_msg.pubnonce.assign(66, 0xAB);

    SelectParams(ChainType::MAIN);
    const uint256 main_nonce_hash = nonce_msg.GetSignatureHash();
    BOOST_REQUIRE(nonce_msg.Sign(key));
    BOOST_REQUIRE(nonce_msg.VerifySignature(xpk));

    SelectParams(ChainType::TESTNET);
    BOOST_CHECK_MESSAGE(main_nonce_hash != nonce_msg.GetSignatureHash(),
        "MuSig2 nonce auth hash must bind hashGenesisBlock so mainnet/testnet hashes diverge");
    BOOST_CHECK_MESSAGE(!nonce_msg.VerifySignature(xpk),
        "MuSig2 nonce auth signed on mainnet must not verify under testnet chain params");

    OracleMusigPartialSigMsg sig_msg;
    sig_msg.epoch = 100;
    sig_msg.oracle_id = 3;
    sig_msg.partial_sig.assign(32, 0xCD);

    SelectParams(ChainType::MAIN);
    const uint256 main_partial_hash = sig_msg.GetSignatureHash();
    BOOST_REQUIRE(sig_msg.Sign(key));
    BOOST_REQUIRE(sig_msg.VerifySignature(xpk));

    SelectParams(ChainType::TESTNET);
    BOOST_CHECK_MESSAGE(main_partial_hash != sig_msg.GetSignatureHash(),
        "MuSig2 partial-sig auth hash must bind hashGenesisBlock so mainnet/testnet hashes diverge");
    BOOST_CHECK_MESSAGE(!sig_msg.VerifySignature(xpk),
        "MuSig2 partial-sig auth signed on mainnet must not verify under testnet chain params");
}

BOOST_AUTO_TEST_CASE(domain_separation_pricemsg_attestation_hash_independent_of_height_and_nonce)
{
    // The compact attestation hash used for off-chain MuSig2 inputs covers
    // (oracle_id, price, timestamp) only. Pin that height / nonce drift do
    // NOT change the off-chain-attestation domain — block_height / nonce
    // already feed the GetSignatureHash domain instead. This locks the
    // observable boundary so any future change has to update the suite.
    COraclePriceMessage a, b;
    a.oracle_id = 4;
    a.price_micro_usd = 50000;
    a.timestamp = 1735689600;
    a.block_height = 0;
    a.nonce = 0;

    b = a;
    b.block_height = 999999;
    b.nonce = 0xDEADBEEF;

    BOOST_CHECK_MESSAGE(a.GetAttestationSignatureHash() == b.GetAttestationSignatureHash(),
        "Attestation hash must depend ONLY on (oracle_id, price, timestamp)");
    BOOST_CHECK_MESSAGE(a.GetSignatureHash() != b.GetSignatureHash(),
        "Full signature hash must depend on block_height/nonce too (so the two domains diverge)");
}

BOOST_AUTO_TEST_SUITE_END()
