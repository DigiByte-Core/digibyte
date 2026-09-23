// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Oracle Block Validation Tests (Week 4: Block Validation Integration)
 *
 * RED PHASE (TDD) - These tests are EXPECTED TO FAIL until implementation is complete.
 *
 * Tests cover:
 * 1. CheckBlock() oracle bundle validation (3 tests)
 * 2. ContextualCheckBlock() timestamp validation (2 tests)
 * 3. ConnectBlock() oracle cache updates (3 tests)
 *
 * Specification: DIGIDOLLAR_ORACLE_PHASE_ONE_SPEC.md Section 5.4
 */

#include <boost/test/unit_test.hpp>
#include <logging.h>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <consensus/volatility.h>
#include <crypto/sha256.h>
#include <key.h>
#include <node/miner.h>
#include <oracle/bundle_manager.h>
#include <oracle/musig2_aggregator.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/oracle.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <random.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(oracle_block_validation_tests, TestChain100Setup)

//
// HELPER FUNCTIONS
//

/**
 * Create a valid oracle price message with Schnorr signature
 */
static COraclePriceMessage CreateValidOracleMessage(const CKey& oracle_key, uint64_t price_micro_usd, int64_t timestamp, int32_t block_height)
{
    COraclePriceMessage msg;
    msg.oracle_id = 0;
    msg.price_micro_usd = price_micro_usd;
    msg.timestamp = timestamp;
    msg.block_height = block_height;
    msg.nonce = GetRand(UINT64_MAX);

    // Set Schnorr public key
    msg.oracle_pubkey = XOnlyPubKey(oracle_key.GetPubKey());

    // Sign with Schnorr signature (BIP-340)
    BOOST_REQUIRE(msg.Sign(oracle_key));

    return msg;
}

static std::array<unsigned char, 32> RegtestOracleSecret(uint8_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    std::array<unsigned char, 32> secret{};
    std::memcpy(secret.data(), hash.begin(), secret.size());
    return secret;
}

static bool SignRegtestV03Bundle(COracleBundle& bundle, const std::vector<uint8_t>& oracle_ids)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    const size_t n_signers = oracle_ids.size();
    std::vector<std::array<unsigned char, 32>> seckeys(n_signers);
    std::vector<secp256k1_keypair> keypairs(n_signers);
    std::vector<secp256k1_pubkey> pubkeys(n_signers);

    for (size_t i = 0; i < n_signers; ++i) {
        seckeys[i] = RegtestOracleSecret(oracle_ids[i]);
        if (!secp256k1_keypair_create(ctx, &keypairs[i], seckeys[i].data()) ||
            !secp256k1_keypair_pub(ctx, &pubkeys[i], &keypairs[i])) {
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
        oracle_ids, static_cast<uint16_t>(Params().GetConsensus().nOracleTotalOracles));
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

static COracleBundle CreateValidMuSig2OracleBundle(uint64_t price_micro_usd, int64_t timestamp, int32_t block_height)
{
    COracleBundle bundle(GetCurrentEpoch(block_height));
    bundle.version = 3;
    bundle.median_price_micro_usd = price_micro_usd;
    bundle.timestamp = timestamp;

    std::vector<uint8_t> oracle_ids;
    const int required = Params().GetConsensus().nOracleConsensusRequired;
    for (uint8_t id = 0; id < required; ++id) {
        oracle_ids.push_back(id);
    }
    BOOST_REQUIRE_MESSAGE(SignRegtestV03Bundle(bundle, oracle_ids),
                          "failed to sign regtest MuSig2 oracle bundle");
    return bundle;
}

/**
 * Add oracle bundle to coinbase transaction OP_RETURN output
 */
static void AddOracleBundleToCoinbase(CMutableTransaction& coinbase, const COracleBundle& bundle)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    CScript oracle_script = manager.CreateOracleScript(bundle);

    // Add as second output (after coinbase reward)
    CTxOut oracle_output;
    oracle_output.nValue = 0;
    oracle_output.scriptPubKey = oracle_script;

    coinbase.vout.push_back(oracle_output);
}

/**
 * Create a block with oracle bundle in coinbase
 */
static CBlock CreateBlockWithOracleBundle(const CKey& oracle_key, uint64_t price_micro_usd, int64_t timestamp, int32_t block_height, const CScript& coinbase_script_sig)
{
    (void)oracle_key;

    CBlock block;
    block.nVersion = 1;
    block.nTime = timestamp;
    block.hashPrevBlock.SetNull();
    block.nBits = 0x207fffff; // Regtest difficulty
    block.nNonce = 0;

    // Create coinbase transaction
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = coinbase_script_sig;

    // Coinbase reward output
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 72000 * COIN; // DigiByte block reward
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;

    // Add V1 MuSig2 oracle bundle to coinbase. The oracle_key argument is kept
    // for the older Phase One call sites but no longer controls V1 signing.
    COracleBundle bundle = CreateValidMuSig2OracleBundle(price_micro_usd, timestamp, block_height);
    AddOracleBundleToCoinbase(coinbase, bundle);

    // Add coinbase to block
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));

    return block;
}

//
// CATEGORY 1: CheckBlock() TESTS (3 tests)
//

/**
 * RED TEST 1: CheckBlock() accepts valid oracle bundle
 *
 * EXPECTED TO FAIL:
 * - CheckBlock() does not yet validate oracle bundles
 * - OracleDataValidator::ValidateBlockOracleData() may not be implemented
 * - Oracle bundle extraction from coinbase OP_RETURN may not work
 */
BOOST_AUTO_TEST_CASE(checkblock_accepts_valid_oracle_bundle)
{
    // Enable oracle system for testing
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(1); // Phase One: 1-of-1 consensus

    // Generate oracle keypair
    CKey oracle_key;
    oracle_key.MakeNewKey(true);

    // Create block with valid oracle bundle
    uint64_t price = 50000; // 50000 micro-USD = $0.05 = 5 cents (valid range: 100 - 100000000 micro-USD)
    int64_t timestamp = GetTime();
    int32_t block_height = 50;  // Below Phase Two activation (100) for Phase One testing  // Above activation height (600)
    CScript coinbase_script_sig = CScript() << block_height << OP_0;

    CBlock block = CreateBlockWithOracleBundle(oracle_key, price, timestamp, block_height, coinbase_script_sig);

    // Test CheckBlock validation
    BlockValidationState state;
    const Consensus::Params& params = Params().GetConsensus();

    // EXPECTED FAILURE: CheckBlock does not yet validate oracle data properly
    bool checkblock_result = CheckBlock(block, state, params, false, false);

    if (!checkblock_result) {
        BOOST_TEST_MESSAGE("CheckBlock rejected: " << state.GetRejectReason() << " - " << state.GetDebugMessage());
    }

    BOOST_CHECK_MESSAGE(
        checkblock_result,
        "CheckBlock should accept block with valid oracle bundle"
    );

    if (!state.IsValid()) {
        LogPrintf("TEST FAILURE (EXPECTED): CheckBlock rejected valid oracle bundle: %s\n", state.GetRejectReason());
    }
}

/**
 * RED TEST 2: CheckBlock() rejects invalid Schnorr signature
 *
 * EXPECTED TO FAIL:
 * - CheckBlock() may not verify Schnorr signatures yet
 * - Signature validation may not be integrated
 * - Error handling for bad signatures may be incomplete
 */
BOOST_AUTO_TEST_CASE(checkblock_rejects_invalid_bundle_signature)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.SetEnabled(true);

    // Test: V1 block oracle data with an invalid oracle price (0) should be
    // rejected contextually. CreateOracleScript still emits structurally valid
    // v0x03 data; consensus validation catches the bad price.
    COracleBundle bundle;
    bundle.version = 3;
    bundle.epoch = GetCurrentEpoch(700);
    bundle.median_price_micro_usd = 0;
    bundle.timestamp = GetTime();
    std::vector<uint8_t> oracle_ids;
    for (uint8_t id = 0; id < Params().GetConsensus().nOracleConsensusRequired; ++id) {
        oracle_ids.push_back(id);
    }
    bundle.participation_bitmap = MuSig2OracleAggregator::EncodeBitmap(
        oracle_ids, static_cast<uint16_t>(Params().GetConsensus().nOracleTotalOracles));
    bundle.aggregate_sig.assign(64, 0x01);

    // Create block at height 700 (above both activation heights)
    CBlock block;
    block.nVersion = 1;
    block.nTime = GetTime();
    block.hashPrevBlock.SetNull();
    block.nBits = 0x207fffff;
    block.nNonce = 0;

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << 700 << OP_0;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 72000 * COIN;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;

    AddOracleBundleToCoinbase(coinbase, bundle);
    BOOST_REQUIRE(!coinbase.vout.back().scriptPubKey.empty());
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));

    BlockValidationState state;
    const Consensus::Params& params = Params().GetConsensus();

    // CheckBlock is context-free and must not enforce oracle price rules.
    bool accepted = CheckBlock(block, state, params, false, false);
    BOOST_CHECK_MESSAGE(accepted,
        "CheckBlock should defer oracle price validation to contextual block checks");

    CBlockIndex prev_index;
    prev_index.nHeight = 699;
    prev_index.nTime = block.nTime - 15;

    BlockValidationState contextual_state;
    bool contextual_accepted = OracleDataValidator::ValidateBlockOracleData(
        block, &prev_index, params, contextual_state);

    BOOST_CHECK_MESSAGE(!contextual_accepted,
        "Contextual oracle validation should reject block with invalid oracle price (0)");
    std::string reason = contextual_state.GetRejectReason();
    BOOST_CHECK_MESSAGE(
        reason == "bad-oracle-musig2",
        "Expected oracle rejection reason, got: " + reason
    );
}

/**
 * RED TEST 3: CheckBlock() rejects bundle with wrong consensus (not 1-of-1)
 *
 * EXPECTED TO FAIL:
 * - CheckBlock() may not enforce Phase One 1-of-1 consensus requirement
 * - Bundle consensus validation may not be implemented
 */
BOOST_AUTO_TEST_CASE(checkblock_rejects_bundle_wrong_consensus)
{
    // Enable oracle system for testing
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(1); // Phase One: 1-of-1 consensus

    // Generate two oracle keypairs
    CKey oracle_key1, oracle_key2;
    oracle_key1.MakeNewKey(true);
    oracle_key2.MakeNewKey(true);

    int64_t timestamp = GetTime();
    int32_t block_height = 50;  // Below Phase Two activation (100) for Phase One testing  // Above activation height (600)

    // Create bundle with TWO messages (violates Phase One 1-of-1)
    COraclePriceMessage msg1 = CreateValidOracleMessage(oracle_key1, 5, timestamp, block_height);
    COraclePriceMessage msg2 = CreateValidOracleMessage(oracle_key2, 6, timestamp, block_height);

    COracleBundle bundle;
    bundle.messages.push_back(msg1);
    bundle.messages.push_back(msg2); // INVALID: Phase One requires exactly 1 message
    bundle.epoch = GetCurrentEpoch(block_height);
    bundle.median_price_micro_usd = 5; // Median: 5 cents ($0.05)
    bundle.timestamp = timestamp;

    // Create block
    CBlock block;
    block.nVersion = 1;
    block.nTime = timestamp;
    block.hashPrevBlock.SetNull();
    block.nBits = 0x207fffff;
    block.nNonce = 0;

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << block_height << OP_0;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 72000 * COIN;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;

    AddOracleBundleToCoinbase(coinbase, bundle);
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));

    // Test CheckBlock behavior with wrong consensus
    // Phase One: CreateOracleScript rejects bundles with multiple messages by returning empty script
    // This means the block will have NO oracle data (transition period behavior)
    BlockValidationState state;
    const Consensus::Params& params = Params().GetConsensus();

    // Phase One behavior: Block with invalid bundle structure (>1 message) results in NO oracle data
    // Since we're in transition period, blocks without oracle data are allowed
    // The proper fix is to test that CreateOracleScript returns empty for multi-message bundles

    // V1 no longer serializes legacy v0x02 bundles, even when old Phase Two
    // message consensus exists.
    OracleBundleManager& test_manager = OracleBundleManager::GetInstance();
    CScript oracle_script = test_manager.CreateOracleScript(bundle);

    BOOST_CHECK_MESSAGE(
        oracle_script.empty(),
        "CreateOracleScript must not produce legacy v0x02 scripts in V1"
    );

    // If script is empty, CheckBlock will pass (transition period)
    // This is correct Phase One behavior - invalid bundles are rejected at creation, not validation
    if (oracle_script.empty()) {
        BOOST_CHECK_MESSAGE(
            CheckBlock(block, state, params, false, false),
            "CheckBlock should pass when oracle bundle is rejected at creation (empty script)"
        );
    }
}

//
// CATEGORY 2: ContextualCheckBlock() TESTS (2 tests)
//

/**
 * RED TEST 4: ContextualCheckBlock() validates timestamp within range
 *
 * EXPECTED TO FAIL:
 * - ContextualCheckBlock() may not exist or not check oracle timestamps
 * - Timestamp validation against block time may not be implemented
 */
BOOST_AUTO_TEST_CASE(contextual_checkblock_timestamp_validation)
{
    CKey oracle_key;
    oracle_key.MakeNewKey(true);

    int64_t block_time = GetTime();
    int64_t oracle_timestamp = block_time - 1800; // 30 minutes old (valid: < 1 hour)
    int32_t block_height = 700;  // Above DigiDollar activation so V1 oracle validation runs

    CBlock block = CreateBlockWithOracleBundle(
        oracle_key,
        50000,  // 50000 micro-USD = $0.05 = 5 cents (valid range: 100 - 100000000 micro-USD)
        oracle_timestamp,
        block_height,
        CScript() << block_height << OP_0
    );
    block.nTime = block_time;

    // Create mock previous block index
    CBlockIndex prev_index;
    prev_index.nHeight = block_height - 1;
    prev_index.nTime = block_time - 15; // DigiByte: 15 second blocks

    BlockValidationState state;
    const Consensus::Params& params = Params().GetConsensus();

    // First validate with CheckBlock
    bool check_result = CheckBlock(block, state, params, false, false);
    if (!check_result) {
        LogPrintf("CheckBlock FAILED: %s - %s\n", state.GetRejectReason(), state.GetDebugMessage());
    }
    BOOST_REQUIRE(check_result);

    // EXPECTED FAILURE: ContextualCheckBlock may not exist or validate oracle timestamps
    // Note: This is a static function in validation.cpp, may need to be exposed
    // For now, we test through OracleDataValidator::ValidateBlockOracleData

    bool valid = OracleDataValidator::ValidateBlockOracleData(block, &prev_index, params, state);

    BOOST_CHECK_MESSAGE(
        valid,
        "Oracle bundle with timestamp within 1 hour of block time should be accepted"
    );

    if (!valid) {
        LogPrintf("TEST FAILURE (EXPECTED): ContextualCheckBlock rejected oracle timestamp within valid range\n");
    }
}

/**
 * RED TEST 5: ContextualCheckBlock() rejects old oracle bundle
 *
 * EXPECTED TO FAIL:
 * - Timestamp validation may not enforce 1-hour maximum age
 * - ContextualCheckBlock oracle integration may be incomplete
 */
BOOST_AUTO_TEST_CASE(contextual_checkblock_rejects_old_bundle)
{
    // Enable oracle system for testing
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(1); // Phase One: 1-of-1 consensus

    CKey oracle_key;
    oracle_key.MakeNewKey(true);

    int64_t block_time = GetTime();
    int64_t oracle_timestamp = block_time - 7200; // 2 hours old (invalid: > 1 hour)
    int32_t block_height = 700;  // Above DigiDollar activation height (650) so validation kicks in

    CBlock block = CreateBlockWithOracleBundle(
        oracle_key,
        50000,  // 50000 micro-USD = $0.05 = 5 cents (valid range: 100 - 100000000 micro-USD)
        oracle_timestamp,
        block_height,
        CScript() << block_height << OP_0
    );
    block.nTime = block_time;

    CBlockIndex prev_index;
    prev_index.nHeight = block_height - 1;
    prev_index.nTime = block_time - 15;

    BlockValidationState state;
    const Consensus::Params& params = Params().GetConsensus();

    // EXPECTED FAILURE: ContextualCheckBlock may not reject old bundles
    bool valid = OracleDataValidator::ValidateBlockOracleData(block, &prev_index, params, state);

    BOOST_CHECK_MESSAGE(
        !valid,
        "Oracle bundle older than 1 hour should be rejected"
    );

    if (valid) {
        LogPrintf("TEST FAILURE (EXPECTED): ContextualCheckBlock accepted oracle bundle older than 1 hour\n");
    }
}

BOOST_AUTO_TEST_CASE(validated_bundle_output_tracks_only_the_checked_block)
{
    LOCK(cs_main);
    const Consensus::Params params = Params().GetConsensus();
    const CBlockIndex* parent = m_node.chainman->ActiveChain().Tip();
    const int height = parent->nHeight + 1;
    const int64_t timestamp = GetTime();
    const auto expected = CreateValidMuSig2OracleBundle(50000, timestamp, height);
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << height << OP_0;
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    AddOracleBundleToCoinbase(coinbase, expected);
    CBlock valid;
    valid.nTime = timestamp;
    valid.vtx.push_back(MakeTransactionRef(coinbase));
    std::optional<COracleBundle> output;

    const auto check = [&](const CBlock& block, const Consensus::Params& rules,
                           bool accepted, const std::string& reason, bool has_bundle) {
        output = expected;
        BlockValidationState plain_state, output_state;
        const bool plain = OracleDataValidator::ValidateBlockOracleData(block, parent, rules, plain_state);
        const bool with_output = OracleDataValidator::ValidateBlockOracleData(block, parent, rules, output_state, &output);
        BOOST_CHECK_EQUAL(plain, accepted);
        BOOST_CHECK_EQUAL(with_output, accepted);
        BOOST_CHECK_EQUAL(plain_state.GetRejectReason(), reason);
        BOOST_CHECK_EQUAL(output_state.GetRejectReason(), reason);
        BOOST_CHECK(plain_state.GetResult() == output_state.GetResult());
        if (!accepted) BOOST_CHECK(output_state.GetResult() == BlockValidationResult::BLOCK_CONSENSUS);
        BOOST_CHECK_EQUAL(plain_state.GetDebugMessage(), output_state.GetDebugMessage());
        BOOST_CHECK_EQUAL(output.has_value(), has_bundle);
        if (has_bundle) {
            BOOST_REQUIRE(output);
            BOOST_CHECK_EQUAL(output->median_price_micro_usd, expected.median_price_micro_usd);
            BOOST_CHECK_EQUAL(output->timestamp, expected.timestamp);
            BOOST_CHECK_EQUAL(output->epoch, expected.epoch);
            BOOST_CHECK(output->aggregate_sig == expected.aggregate_sig);
            BOOST_CHECK(output->participation_bitmap == expected.participation_bitmap);
        }
    };
    check(valid, params, true, "", true);
    check(CBlock{}, params, true, "", false);
    auto inactive = params;
    inactive.DigiDollarHeight = height + 1;
    check(valid, inactive, true, "", false);

    CBlock absent = valid;
    CMutableTransaction absent_coinbase(coinbase);
    absent_coinbase.vout.pop_back();
    absent.vtx[0] = MakeTransactionRef(absent_coinbase);
    check(absent, params, true, "", false);

    CMutableTransaction mint;
    mint.nVersion = MakeDigiDollarVersion(DD_TX_MINT);
    mint.vin.emplace_back(COutPoint(uint256::ONE, 0));
    mint.vout.emplace_back(0, CScript() << OP_TRUE);
    CBlock missing_required = absent;
    missing_required.vtx.push_back(MakeTransactionRef(mint));
    check(missing_required, params, false, "bad-oracle-missing", false);

    CBlock duplicate = valid;
    CMutableTransaction duplicate_coinbase(coinbase);
    duplicate_coinbase.vout.push_back(coinbase.vout.back());
    duplicate.vtx[0] = MakeTransactionRef(duplicate_coinbase);
    check(duplicate, params, false, "bad-oracle-multiple-outputs", false);

    CBlock malformed = valid;
    CMutableTransaction malformed_coinbase(coinbase);
    malformed_coinbase.vout.back().scriptPubKey = CScript() << OP_RETURN << OP_ORACLE;
    malformed.vtx[0] = MakeTransactionRef(malformed_coinbase);
    check(malformed, params, false, "bad-oracle-malformed", false);

    CBlock stale = valid;
    stale.nTime += ORACLE_MAX_AGE_SECONDS;
    check(stale, params, true, "", true);
    ++stale.nTime;
    check(stale, params, false, "bad-oracle-timestamp", false);
    CBlock future = valid;
    future.nTime -= 60;
    check(future, params, true, "", true);
    --future.nTime;
    check(future, params, false, "bad-oracle-timestamp", false);
    auto wrong_chain = params;
    wrong_chain.hashGenesisBlock = uint256::ONE;
    check(valid, wrong_chain, false, "bad-oracle-musig2", false);
    check(valid, params, true, "", true);

    auto bad_bundle = expected;
    ++bad_bundle.epoch;
    CMutableTransaction wrong_epoch_coinbase(absent_coinbase);
    AddOracleBundleToCoinbase(wrong_epoch_coinbase, bad_bundle);
    CBlock wrong_epoch = valid;
    wrong_epoch.vtx[0] = MakeTransactionRef(wrong_epoch_coinbase);
    check(wrong_epoch, params, false, "bad-oracle-musig2", false);

    bad_bundle = expected;
    bad_bundle.aggregate_sig[0] ^= 1;
    CMutableTransaction bad_signature_coinbase(absent_coinbase);
    AddOracleBundleToCoinbase(bad_signature_coinbase, bad_bundle);
    CBlock bad_signature = valid;
    bad_signature.vtx[0] = MakeTransactionRef(bad_signature_coinbase);
    check(bad_signature, params, false, "bad-oracle-musig2", false);
}

BOOST_AUTO_TEST_CASE(reused_bundle_verifier_respects_parameter_snapshots)
{
    LOCK(cs_main);
    const Consensus::Params params = Params().GetConsensus();
    const int height = m_node.chainman->ActiveChain().Height() + 1;
    const auto bundle = CreateValidMuSig2OracleBundle(50000, GetTime(), height);
    const auto verify = [&](const Consensus::Params& rules, bool expected, const std::string& reason) {
        std::string error;
        BOOST_CHECK_EQUAL(OracleBundleManager::ValidateMuSig2Bundle(bundle, height, rules, error), expected);
        BOOST_CHECK_EQUAL(error, reason);
    };
    verify(params, true, "");
    auto changed = params;
    changed.hashGenesisBlock = uint256::ONE;
    verify(changed, false, "v0x03 aggregate signature verification failed");
    verify(params, true, "");
    changed = params;
    changed.vOracleCompressedPublicKeys[0][0] ^= 1;
    verify(changed, false, "v0x03 aggregate signature verification failed");
    verify(params, true, "");
    changed = params;
    changed.vOraclePublicKeys[0] = std::string(64, '0');
    verify(changed, false, "Failed to compute aggregate pubkey from bitmap");
    verify(params, true, "");
    changed = params;
    changed.nOracleConsensusRequired = 5;
    verify(changed, false, "v0x03 bundle below minimum oracle threshold (4 signers, need 5)");
    verify(params, true, "");
    changed = params;
    changed.nOraclePubkeyCount = 3;
    verify(changed, false, "v0x03 signer outside active oracle roster (id=3)");
    verify(params, true, "");
}

BOOST_AUTO_TEST_CASE(startup_cancellation_after_scanning_preserves_existing_state)
{
    auto& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(false);
    int height;
    {
        LOCK(cs_main);
        height = m_node.chainman->ActiveChain().Height() + 1;
    }
    CBlock block = CreateBlock({}, CScript() << OP_TRUE, m_node.chainman->ActiveChainstate());
    CMutableTransaction coinbase(*block.vtx[0]);
    const auto quote = CreateValidMuSig2OracleBundle(50000, block.nTime, height);
    AddOracleBundleToCoinbase(coinbase, quote);
    block.vtx[0] = MakeTransactionRef(coinbase);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block.nNonce = 0;
    while (!CheckProofOfWork(GetPoWAlgoHash(block), block.nBits, m_node.chainman->GetConsensus())) {
        ++block.nNonce;
    }
    bool new_block = false;
    BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block),
                                                  /*force_processing=*/true,
                                                  /*min_pow_checked=*/true, &new_block));
    BOOST_REQUIRE(new_block);
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(m_node.chainman->ActiveChain().Height(), height);
    }

    manager.Clear();
    manager.UpdatePriceCache(height, 70000, GetTime());
    DigiDollar::Volatility::VolatilityMonitor::ReconstructFromBlockData({{70000, GetTime(), uint32_t(height)}}, height);
    const auto before = manager.GetStats();
    const auto history = DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory();
    BOOST_REQUIRE_EQUAL(history.size(), 1U);
    int polls = 0;
    OracleBundleManager::LoadCallbacks callbacks;
    callbacks.cancelled = [&] { return ++polls > height; };
    std::vector<std::pair<uint64_t, uint64_t>> progress;
    callbacks.progress = [&](uint64_t completed, uint64_t total) { progress.emplace_back(completed, total); };
    const auto cancelled = OracleBundleManager::LoadPricesFromChain(*m_node.chainman, callbacks);
    BOOST_CHECK(cancelled.status == OracleBundleManager::LoadStatus::CANCELLED);
    BOOST_CHECK_EQUAL(polls, height + 1);
    BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(height), 70000U);
    BOOST_CHECK_EQUAL(manager.GetStats().latest_price, before.latest_price);
    BOOST_CHECK_EQUAL(manager.GetStats().last_update, before.last_update);
    const auto unchanged = DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory();
    BOOST_REQUIRE_EQUAL(unchanged.size(), history.size());
    BOOST_CHECK_EQUAL(unchanged[0].price, history[0].price);
    BOOST_CHECK_EQUAL(unchanged[0].timestamp, history[0].timestamp);
    BOOST_CHECK_EQUAL(unchanged[0].height, history[0].height);
    BOOST_REQUIRE(!progress.empty());
    BOOST_CHECK_EQUAL(progress.front().first, 0U);
    BOOST_CHECK_LT(progress.back().first, uint64_t(height));

    polls = 0;
    callbacks.cancelled = [&] {
        if (++polls == height / 2) manager.UpdatePriceCache(1, 60000, GetTime());
        return false;
    };
    progress.clear();
    const auto complete = OracleBundleManager::LoadPricesFromChain(*m_node.chainman, callbacks);
    BOOST_CHECK(complete.status == OracleBundleManager::LoadStatus::COMPLETE);
    BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(1), 60000U);
    BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(height), quote.median_price_micro_usd);
    BOOST_CHECK_EQUAL(manager.GetStats().last_update, quote.timestamp);
    BOOST_CHECK(DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().empty());
    BOOST_REQUIRE(!progress.empty());
    BOOST_CHECK_EQUAL(progress.front().first, 0U);
    BOOST_CHECK_EQUAL(progress.back().first, uint64_t(height));
    BOOST_CHECK_EQUAL(progress.back().second, uint64_t(height));
    BOOST_CHECK_LE(progress.size(), 102U);
    manager.Clear();
    DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
}

BOOST_AUTO_TEST_SUITE_END()
