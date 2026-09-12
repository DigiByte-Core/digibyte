// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Oracle MuSig2 Consensus Threshold Tests — T3-05a Fix Verification
 *
 * Verifies that V1 oracle bundle paths use chainparams.nOracleConsensusRequired
 * instead of the compile-time ORACLE_CONSENSUS_REQUIRED constant or legacy
 * message-bundle thresholds.
 *
 * The bug: multiple call sites used old message-count thresholds. DigiDollar
 * V1 consensus-visible oracle data must be complete MuSig2 v0x03 bundles only.
 */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/err.h>
#include <consensus/params.h>
#include <crypto/sha256.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/musig2_aggregator.h>
#include <primitives/oracle.h>
#include <pubkey.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <array>
#include <cstring>

namespace {

std::array<unsigned char, 32> DeterministicOracleSecret(uint8_t oracle_id)
{
    const std::string seed = "oracle_consensus_threshold_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    std::array<unsigned char, 32> secret{};
    std::memcpy(secret.data(), hash.begin(), secret.size());
    return secret;
}

bool MakeEvenYOracleKeypair(secp256k1_context* ctx,
                            uint8_t oracle_id,
                            std::array<unsigned char, 32>& secret,
                            secp256k1_keypair& keypair,
                            secp256k1_pubkey& pubkey)
{
    secret = DeterministicOracleSecret(oracle_id);
    if (!secp256k1_keypair_create(ctx, &keypair, secret.data()) ||
        !secp256k1_keypair_pub(ctx, &pubkey, &keypair)) {
        return false;
    }

    secp256k1_xonly_pubkey xonly{};
    int parity = 0;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly, &parity, &pubkey)) {
        return false;
    }
    if (parity == 0) return true;

    if (!secp256k1_ec_seckey_negate(ctx, secret.data())) {
        return false;
    }
    return secp256k1_keypair_create(ctx, &keypair, secret.data()) &&
           secp256k1_keypair_pub(ctx, &pubkey, &keypair);
}

Consensus::Params MakeMuSig2TestParams(int total_oracles, int required)
{
    Consensus::Params params = Params().GetConsensus();
    params.nDigiDollarMuSig2Height = 0;
    params.nOracleTotalOracles = total_oracles;
    params.nOraclePubkeyCount = total_oracles;
    params.nOracleConsensusRequired = required;
    params.vOraclePublicKeys.clear();
    params.vOracleCompressedPublicKeys.clear();

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx != nullptr);

    for (int i = 0; i < total_oracles; ++i) {
        std::array<unsigned char, 32> secret{};
        secp256k1_keypair keypair{};
        secp256k1_pubkey pubkey{};
        secp256k1_xonly_pubkey xonly{};
        int parity = 0;
        BOOST_REQUIRE(MakeEvenYOracleKeypair(ctx, static_cast<uint8_t>(i), secret, keypair, pubkey));
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

COracleBundle CreateLegacyMessageBundle(int num_messages, uint64_t price = 50000, int32_t epoch = 0)
{
    COracleBundle bundle(epoch);
    for (int i = 0; i < num_messages; i++) {
        CKey key;
        key.MakeNewKey(true);
        COraclePriceMessage msg(i, price, GetTime());
        msg.oracle_pubkey = XOnlyPubKey(key.GetPubKey());
        msg.SignAttestation(key);
        bundle.messages.push_back(msg);
    }
    bundle.median_price_micro_usd = price;
    bundle.timestamp = GetTime();
    return bundle;
}

COracleBundle CreateCompleteMuSig2Bundle(int signers,
                                         const Consensus::Params& params,
                                         uint64_t price = 50000,
                                         int32_t epoch = 0,
                                         int64_t timestamp = GetTime())
{
    COracleBundle bundle(epoch);
    bundle.version = 3;
    bundle.median_price_micro_usd = price;
    bundle.timestamp = timestamp;
    bundle.aggregate_sig.assign(64, 0x42);

    std::vector<uint8_t> oracle_ids;
    for (int i = 0; i < signers; ++i) {
        oracle_ids.push_back(static_cast<uint8_t>(i));
    }
    bundle.participation_bitmap = MuSig2OracleAggregator::EncodeBitmap(
        oracle_ids, static_cast<uint16_t>(params.nOracleTotalOracles));
    return bundle;
}

bool SignMuSig2Bundle(COracleBundle& bundle,
                      const std::vector<uint8_t>& oracle_ids,
                      const Consensus::Params& params)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    const size_t n_signers = oracle_ids.size();
    std::vector<std::array<unsigned char, 32>> seckeys(n_signers);
    std::vector<secp256k1_keypair> keypairs(n_signers);
    std::vector<secp256k1_pubkey> pubkeys(n_signers);

    for (size_t i = 0; i < n_signers; ++i) {
        if (!MakeEvenYOracleKeypair(ctx, oracle_ids[i], seckeys[i], keypairs[i], pubkeys[i])) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_pubkey*> pubkey_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        pubkey_ptrs[i] = &pubkeys[i];
    }

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
    for (size_t i = 0; i < n_signers; ++i) {
        nonce_ptrs[i] = &pubnonces[i];
    }

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

    bundle.participation_bitmap = MuSig2OracleAggregator::EncodeBitmap(
        oracle_ids, static_cast<uint16_t>(params.nOracleTotalOracles));
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

COracleBundle CreateSignedMuSig2Bundle(const Consensus::Params& params,
                                        int signers,
                                        uint64_t price,
                                        int32_t block_height)
{
    COracleBundle bundle(GetCurrentEpoch(block_height));
    bundle.version = 3;
    bundle.median_price_micro_usd = price;
    bundle.timestamp = GetTime();

    std::vector<uint8_t> oracle_ids;
    for (int i = 0; i < signers; ++i) {
        oracle_ids.push_back(static_cast<uint8_t>(i));
    }
    BOOST_REQUIRE(SignMuSig2Bundle(bundle, oracle_ids, params));
    return bundle;
}

} // anonymous namespace

BOOST_FIXTURE_TEST_SUITE(oracle_consensus_threshold_tests, BasicTestingSetup)

/**
 * Test 1: MuSig2 quorum uses chainparams threshold
 *
 * Consensus-visible oracle data is a v0x03 bitmap and aggregate signature.
 * The bitmap must contain at least nOracleConsensusRequired active signers.
 */
BOOST_AUTO_TEST_CASE(has_consensus_uses_chainparams_value)
{
    const Consensus::Params& params = Params().GetConsensus();
    int required = params.nOracleConsensusRequired;

    BOOST_TEST_MESSAGE("Network requires " << required << " MuSig2 oracle signers for consensus");

    COracleBundle exact_bundle = CreateCompleteMuSig2Bundle(required, params);
    BOOST_CHECK(OracleBundleManager::ValidateBundle(exact_bundle, 0, params));

    if (required > 1) {
        COracleBundle insufficient_bundle = CreateCompleteMuSig2Bundle(required - 1, params);
        BOOST_CHECK(!OracleBundleManager::ValidateBundle(insufficient_bundle, 0, params));
    }

    COracleBundle excess_bundle = CreateCompleteMuSig2Bundle(required + 1, params);
    BOOST_CHECK(OracleBundleManager::ValidateBundle(excess_bundle, 0, params));

    COracleBundle empty_bundle(0);
    BOOST_CHECK(!OracleBundleManager::ValidateBundle(empty_bundle, 0, params));
}

/**
 * Test 2: Manager price lookup requires MuSig2 quorum
 *
 * A complete-looking v0x03 bundle below quorum is not a usable price source.
 */
BOOST_AUTO_TEST_CASE(get_consensus_price_uses_chainparams_value)
{
    const Consensus::Params& params = Params().GetConsensus();
    int required = params.nOracleConsensusRequired;
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    COracleBundle valid_bundle = CreateCompleteMuSig2Bundle(required, params, 50000, 11);
    manager.UpdateBundle(valid_bundle);
    BOOST_CHECK_EQUAL(manager.GetConsensusPrice(11), 50000);

    if (required > 1) {
        COracleBundle invalid_bundle = CreateCompleteMuSig2Bundle(required - 1, params, 50000, 12);
        manager.UpdateBundle(invalid_bundle);
        BOOST_CHECK_EQUAL(manager.GetConsensusPrice(12), 0);
    }

    manager.Clear();
}

/**
 * Test 3: IsValid accepts only complete MuSig2 v0x03 structure
 *
 * Legacy message bundles are not valid V1 oracle bundles.
 */
BOOST_AUTO_TEST_CASE(is_valid_uses_chainparams_value)
{
    const Consensus::Params& params = Params().GetConsensus();
    int required = params.nOracleConsensusRequired;

    COracleBundle valid_bundle = CreateCompleteMuSig2Bundle(required, params, 50000, 0, GetTime());
    BOOST_CHECK(valid_bundle.IsValid(required, GetTime()));

    COracleBundle legacy_message_bundle = CreateLegacyMessageBundle(required, 50000);
    BOOST_CHECK(!legacy_message_bundle.IsValid(required, GetTime()));
}

/**
 * Test 4: OracleBundleManager uses min_oracle_count from chainparams
 *
 * The manager's min_oracle_count is set from consensus.nOracleConsensusRequired
 * during initialization. All internal methods should use this value.
 */
BOOST_AUTO_TEST_CASE(bundle_manager_uses_chainparams_threshold)
{
    const Consensus::Params& params = Params().GetConsensus();
    int required = params.nOracleConsensusRequired;

    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(required);

    int32_t epoch = 5;
    COracleBundle bundle = CreateCompleteMuSig2Bundle(required, params, 50000, epoch);
    manager.UpdateBundle(bundle);

    bool updated = manager.UpdateCachedPrice(epoch);
    BOOST_CHECK_MESSAGE(updated,
        "UpdateCachedPrice should succeed with " << required << " MuSig2 signers meeting chainparams threshold");

    CAmount cached = manager.GetLatestPrice();
    BOOST_CHECK(cached > 0);

    if (required > 1) {
        manager.Clear();
        manager.SetEnabled(true);
        manager.SetMinOracleCount(required);
        COracleBundle below = CreateCompleteMuSig2Bundle(required - 1, params, 50000, epoch + 1);
        manager.UpdateBundle(below);
        BOOST_CHECK(!manager.UpdateCachedPrice(epoch + 1));
        BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 0);
    }

    manager.Clear();
}

/**
 * Test 5: OracleDataValidator uses chainparams threshold
 *
 * ValidateOracleBundle should use params.nOracleConsensusRequired and verify
 * a real MuSig2 aggregate signature, not a legacy message bundle.
 */
BOOST_AUTO_TEST_CASE(validator_uses_chainparams_threshold)
{
    const Consensus::Params& active_params = Params().GetConsensus();
    int required = active_params.nOracleConsensusRequired;
    Consensus::Params params = MakeMuSig2TestParams(active_params.nOracleTotalOracles, required);

    int32_t block_height = 1000;
    COracleBundle valid_bundle = CreateSignedMuSig2Bundle(params, required, 50000, block_height);

    bool valid = OracleDataValidator::ValidateOracleBundle(valid_bundle, block_height, params);
    BOOST_CHECK_MESSAGE(valid,
        "ValidateOracleBundle should accept " << required << "-signer MuSig2 bundle (chainparams threshold)");

    if (required > 1) {
        COracleBundle below = CreateSignedMuSig2Bundle(params, required - 1, 50000, block_height);
        BOOST_CHECK(!OracleDataValidator::ValidateOracleBundle(below, block_height, params));
    }
}

/**
 * Test 6: ERR HasOracleConsensus uses chainparams threshold
 *
 * EmergencyRedemptionRatio::HasOracleConsensus should use chainparams,
 * not the hardcoded ORACLE_CONSENSUS_REQUIRED.
 */
BOOST_AUTO_TEST_CASE(err_has_oracle_consensus_uses_chainparams)
{
    const Consensus::Params& params = Params().GetConsensus();
    int required = params.nOracleConsensusRequired;

    COracleBundle valid_bundle = CreateCompleteMuSig2Bundle(required, params, 50000);
    bool consensus = DigiDollar::ERR::EmergencyRedemptionRatio::HasOracleConsensus(valid_bundle, params);
    BOOST_CHECK_MESSAGE(consensus,
        "ERR::HasOracleConsensus should accept " << required << "-signer MuSig2 bundle");

    if (required > 1) {
        COracleBundle insufficient_bundle = CreateCompleteMuSig2Bundle(required - 1, params, 50000);
        bool no_consensus = DigiDollar::ERR::EmergencyRedemptionRatio::HasOracleConsensus(insufficient_bundle, params);
        BOOST_CHECK_MESSAGE(!no_consensus,
            "ERR::HasOracleConsensus should reject " << (required - 1) << "-signer MuSig2 bundle");
    }

    COracleBundle legacy_bundle = CreateLegacyMessageBundle(required, 50000);
    BOOST_CHECK(!DigiDollar::ERR::EmergencyRedemptionRatio::HasOracleConsensus(legacy_bundle, params));
}

/**
 * Test 7: Different networks have different thresholds
 *
 * Verify the chainparams values are configured correctly per network.
 */
BOOST_AUTO_TEST_CASE(network_specific_thresholds)
{
    // Regtest (current test environment)
    const Consensus::Params& regtest_params = Params().GetConsensus();
    BOOST_CHECK(regtest_params.nOracleConsensusRequired > 0);
    BOOST_CHECK(regtest_params.nOracleConsensusRequired <= regtest_params.nOracleTotalOracles);

    BOOST_TEST_MESSAGE("Regtest: " << regtest_params.nOracleConsensusRequired
        << " of " << regtest_params.nOracleTotalOracles << " required");
}

/**
 * Test 8: Boundary conditions — exactly at threshold
 *
 * Verify behavior at the exact threshold boundary.
 */
BOOST_AUTO_TEST_CASE(consensus_threshold_boundary)
{
    const Consensus::Params& params = Params().GetConsensus();
    int required = params.nOracleConsensusRequired;

    COracleBundle at_threshold = CreateCompleteMuSig2Bundle(required, params, 50000);
    BOOST_CHECK(OracleBundleManager::ValidateBundle(at_threshold, 0, params));

    if (required > 0) {
        COracleBundle below_threshold = CreateCompleteMuSig2Bundle(required - 1, params, 50000);
        BOOST_CHECK(!OracleBundleManager::ValidateBundle(below_threshold, 0, params));
    }

    COracleBundle above_threshold = CreateCompleteMuSig2Bundle(required + 1, params, 50000);
    BOOST_CHECK(OracleBundleManager::ValidateBundle(above_threshold, 0, params));
}

BOOST_AUTO_TEST_CASE(duplicate_oracle_ids_do_not_count_toward_consensus)
{
    const Consensus::Params& params = Params().GetConsensus();
    int required = params.nOracleConsensusRequired;
    BOOST_REQUIRE(required > 1);

    COracleBundle duplicate_bundle(0);
    COracleBundle single_oracle_bundle = CreateLegacyMessageBundle(1, 50000);
    const COraclePriceMessage duplicate_msg = single_oracle_bundle.messages.front();

    for (int i = 0; i < required; ++i) {
        duplicate_bundle.messages.push_back(duplicate_msg);
    }
    duplicate_bundle.median_price_micro_usd = 50000;
    duplicate_bundle.timestamp = GetTime();

    BOOST_CHECK_MESSAGE(!duplicate_bundle.HasConsensus(required),
        "Consensus threshold must count unique oracle IDs, not repeated copies of one signed message");
    BOOST_CHECK_EQUAL(duplicate_bundle.GetConsensusPrice(required), 0);

    COracleBundle unique_plus_duplicate = CreateLegacyMessageBundle(required, 50000);
    unique_plus_duplicate.messages.push_back(unique_plus_duplicate.messages.front());
    BOOST_CHECK_MESSAGE(!unique_plus_duplicate.HasConsensus(required),
        "A bundle with any duplicate oracle ID is malformed and must not satisfy consensus");
    BOOST_CHECK_EQUAL(unique_plus_duplicate.GetConsensusPrice(required), 0);
}

BOOST_AUTO_TEST_SUITE_END()
