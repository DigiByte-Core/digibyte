// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Wave 12 — Activation Gates and Consensus-Split Risks (Agent B)
 *
 * Pins the exact pre/post-activation contract enforced by the DigiDollar
 * runtime gates:
 *   - DigiDollar::IsDigiDollarEnabled(pindexPrev, ...)         (buried-height gate, BIP90)
 *   - Consensus::IsOracleActive(params, height)                (height gate)
 *   - Consensus::IsMuSig2Active(params, height)                (height gate)
 *   - OracleDataValidator::ValidateBlockOracleData             (block-level gate)
 *   - CheckMuSig2OracleBundleVersion                           (block-level gate)
 *
 * The contract under test:
 *   1. At height = nDDActivationHeight - 1, none of the height gates fire and
 *      ValidateBlockOracleData / CheckMuSig2OracleBundleVersion short-circuit
 *      true regardless of the block contents (no enforcement, backward
 *      compatible). This holds whether or not the block contains DD-marker
 *      transactions or oracle-shaped OP_RETURN outputs.
 *   2. At height = nDDActivationHeight, all gates fire and DD-touching blocks
 *      must include a valid v0x03 MuSig2 bundle. The first DD-touching block
 *      at activation MUST succeed when given a valid bundle, and MUST be
 *      rejected when missing/legacy/malformed.
 *   3. Non-DD blocks remain accepted on either side of the boundary, and
 *      regular DGB transactions never require oracle data.
 *   4. Pre-activation OP_ORACLE-looking outputs are ignored (no DD reject
 *      reason); post-activation the same script shape is enforced under V1
 *      MuSig2-only rules.
 *   5. DigiDollar is a buried deployment (BIP90): IsDigiDollarEnabled is a
 *      pure height comparison against
 *      DeploymentHeight(DEPLOYMENT_DIGIDOLLAR) — false for every block below
 *      the buried height, true at/after it, monotone along any chain.
 *
 * Coordinated with Agent A (security/exploit) and Agent C
 * (multi-node functional `digidollar_activation_multinode.py`). This suite
 * focuses on unit-level boundary enforcement that the functional test cannot
 * cheaply pin.
 */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <chain.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <crypto/sha256.h>
#include <deploymentstatus.h>
#include <digidollar/digidollar.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <kernel/chainparams.h>
#include <oracle/bundle_manager.h>
#include <primitives/block.h>
#include <primitives/oracle.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/time.h>
#include <validation.h>
#include <versionbits.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace wave12 {

constexpr uint32_t WAVE12_BITS = 0x207fffff;
constexpr uint64_t ORACLE_PRICE = 50000;

// Storage for synthetic block index chains used in this suite. Unique to
// Wave 12 to avoid leaking state into the existing
// digidollar_activation_tests g_test_blocks reservoir.
static std::vector<std::unique_ptr<CBlockIndex>> g_w12_blocks;

CBlockIndex* MakeIndex(CBlockIndex* prev, int height, uint32_t nTime, int32_t nVersion = VERSIONBITS_TOP_BITS)
{
    auto idx = std::make_unique<CBlockIndex>();
    idx->pprev = prev;
    idx->nHeight = height;
    idx->nTime = nTime;
    idx->nBits = WAVE12_BITS;
    idx->nVersion = nVersion;
    idx->BuildSkip();
    CBlockIndex* raw = idx.get();
    g_w12_blocks.push_back(std::move(idx));
    return raw;
}

// Builds a chain of `count` synthetic CBlockIndex starting at `start_height`
// with monotonically increasing timestamps. Each block carries the supplied
// nVersion (activation is buried, so nVersion no longer influences it).
CBlockIndex* MakeChain(int start_height, int count, int64_t base_time, int32_t nVersion)
{
    CBlockIndex* tip = nullptr;
    int64_t t = base_time;
    for (int i = 0; i < count; ++i) {
        tip = MakeIndex(tip, start_height + i, static_cast<uint32_t>(t), nVersion);
        t += 600;
    }
    return tip;
}

// MuSig2 signing helpers shared with digidollar_oracle_bundle_matrix_tests.
std::array<unsigned char, 32> OracleSecret(uint8_t oracle_id)
{
    const std::string seed = std::string{"digibyte_regtest_oracle_"} + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());
    std::array<unsigned char, 32> secret{};
    std::memcpy(secret.data(), hash.begin(), secret.size());
    return secret;
}

std::vector<unsigned char> EncodeBitmap(const std::vector<uint8_t>& oracle_ids, uint16_t total_oracles)
{
    std::vector<unsigned char> bitmap((total_oracles + 7) / 8, 0);
    for (uint8_t id : oracle_ids) bitmap[id / 8] |= static_cast<unsigned char>(1U << (id % 8));
    return bitmap;
}

bool SignV03Bundle(COracleBundle& bundle,
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
        seckeys[i] = OracleSecret(oracle_ids[i]);
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
        unsigned char rnd[32];
        GetStrongRandBytes(Span{rnd, 32});
        if (!secp256k1_musig_nonce_gen(ctx, &secnonces[i], &pubnonces[i],
                                       rnd, seckeys[i].data(), &pubkeys[i], nullptr, &cache, nullptr)) {
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
    std::vector<secp256k1_musig_partial_sig> partials(n_signers);
    std::vector<const secp256k1_musig_partial_sig*> partial_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        if (!secp256k1_musig_partial_sign(ctx, &partials[i], &secnonces[i],
                                          &keypairs[i], &cache, &session)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        partial_ptrs[i] = &partials[i];
    }
    bundle.participation_bitmap = EncodeBitmap(oracle_ids, total_oracles);
    bundle.aggregate_sig.assign(64, 0);
    if (!secp256k1_musig_partial_sig_agg(ctx, bundle.aggregate_sig.data(),
                                         &session, partial_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }
    const bool ok = secp256k1_schnorrsig_verify(ctx, bundle.aggregate_sig.data(), msg32, 32, &agg_pk);
    secp256k1_context_destroy(ctx);
    return ok;
}

COracleBundle MakeValidV03Bundle(int32_t height, int64_t timestamp)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    COracleBundle bundle;
    bundle.version = 3;
    bundle.epoch = GetCurrentEpoch(height);
    bundle.median_price_micro_usd = ORACLE_PRICE;
    bundle.timestamp = timestamp;
    std::vector<uint8_t> ids;
    for (uint8_t id = 0; id < consensus.nOracleConsensusRequired; ++id) ids.push_back(id);
    BOOST_REQUIRE(SignV03Bundle(bundle, ids, static_cast<uint16_t>(consensus.nOracleTotalOracles)));
    return bundle;
}

CScript MakeRawV03Script(const COracleBundle& bundle)
{
    CScript script;
    script << OP_RETURN << OP_ORACLE << std::vector<unsigned char>{0x03};
    script << bundle.SerializeV03Data();
    return script;
}

CScript MakeOracleLookalikeScript()
{
    // OP_RETURN OP_ORACLE with a single arbitrary push but no recognised
    // version byte. Pre-activation this must be ignored entirely. Post-
    // activation the validator extracts/dispatches and must reject.
    CScript s;
    s << OP_RETURN << OP_ORACLE << std::vector<unsigned char>{0xAB, 0xCD};
    return s;
}

CTransactionRef MakeDDTransaction(DigiDollarTxType type, uint8_t flags = 0)
{
    CMutableTransaction tx;
    tx.nVersion = MakeDigiDollarVersion(type, flags);
    tx.vin.emplace_back(COutPoint(uint256::ONE, 0));
    tx.vout.emplace_back(0, CScript() << OP_TRUE);
    return MakeTransactionRef(std::move(tx));
}

CBlock MakeBlock(int32_t bip34_height,
                 uint32_t block_time,
                 const std::vector<CScript>& oracle_scripts,
                 const std::vector<CTransactionRef>& extra_txs)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << static_cast<int64_t>(bip34_height);
    coinbase.vout.emplace_back(50 * COIN, CScript() << OP_TRUE);
    for (const auto& s : oracle_scripts) coinbase.vout.emplace_back(0, s);

    CBlock block;
    block.nVersion = 0x20000000;
    block.nTime = block_time;
    block.nBits = WAVE12_BITS;
    block.hashPrevBlock.SetNull();
    block.nNonce = 0;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.vtx.insert(block.vtx.end(), extra_txs.begin(), extra_txs.end());
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

struct OracleResult {
    bool ok;
    std::string reject_reason;
};

OracleResult RunValidator(const CBlock& block, const Consensus::Params& params, const CBlockIndex* pindex_prev = nullptr)
{
    BlockValidationState state;
    bool ok = OracleDataValidator::ValidateBlockOracleData(block, pindex_prev, params, state);
    return {ok, state.GetRejectReason()};
}

} // namespace wave12

BOOST_FIXTURE_TEST_SUITE(digidollar_activation_wave12_tests, RegTestingSetup)

// =============================================================================
// PART 1 — Activation predicate boundary (buried height, BIP90)
// =============================================================================

// DigiDollar is a buried deployment: IsDigiDollarEnabled is a pure height
// comparison against DeploymentHeight(DEPLOYMENT_DIGIDOLLAR). Off-by-one on
// the boundary: the tip at height N-1 judges block N (the first active
// block) → true; the tip at N-2 judges block N-1 → false. The predicate is
// trivially monotone along any chain — pinned here so a future refactor that
// reintroduces state-dependent activation trips this walk.
BOOST_AUTO_TEST_CASE(wave12_buried_height_off_by_one_boundary)
{
    constexpr int BOUNDARY = 100;
    CChainParams::RegTestOptions opts;
    opts.digidollar_activation_height = BOUNDARY;
    const auto chainparams = CChainParams::RegTest(opts);
    const Consensus::Params& params = chainparams->GetConsensus();
    BOOST_REQUIRE_EQUAL(params.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR), BOUNDARY);

    const uint32_t t = static_cast<uint32_t>(GetTime());
    CBlockIndex* tip = wave12::MakeChain(0, BOUNDARY + 4, t - 600 * (BOUNDARY + 4), VERSIONBITS_TOP_BITS);

    // Collect the chain root..tip by walking pprev and reversing (synthetic
    // chains do not have pnext set).
    std::vector<CBlockIndex*> chain;
    for (CBlockIndex* p = tip; p != nullptr; p = p->pprev) chain.push_back(p);
    std::reverse(chain.begin(), chain.end());

    bool prev_state = false;
    int transitions = 0;
    bool seen_true = false;
    for (CBlockIndex* idx : chain) {
        const bool now = DigiDollar::IsDigiDollarEnabled(idx, params);
        BOOST_CHECK_EQUAL(now, idx->nHeight + 1 >= BOUNDARY);
        if (idx != chain.front() && now != prev_state) ++transitions;
        BOOST_CHECK_MESSAGE(!(seen_true && !now),
            "IsDigiDollarEnabled must be monotone — observed true→false at h=" << idx->nHeight);
        if (now) seen_true = true;
        prev_state = now;
    }
    BOOST_CHECK_EQUAL(transitions, 1);

    // Explicit boundary pins.
    BOOST_CHECK(!DigiDollar::IsDigiDollarEnabled(nullptr, params));                // judges block 0
    BOOST_CHECK(!DigiDollar::IsDigiDollarEnabled(chain[BOUNDARY - 2], params));    // judges block N-1
    BOOST_CHECK(DigiDollar::IsDigiDollarEnabled(chain[BOUNDARY - 1], params));     // judges block N
    BOOST_CHECK(DigiDollar::IsDigiDollarEnabled(tip, params));
}

// =============================================================================
// PART 2 — Height-gate predicates (oracle/MuSig2)
// =============================================================================

// Regtest intentionally has two activation surfaces:
//   - oracle P2P/height gate remains at nDDActivationHeight=650
//   - MuSig2 follows the effective DigiDollar activation boundary, which is
//     the buried DigiDollarHeight=0 on default regtest
// Off-by-one in either direction is asserted explicitly.
BOOST_AUTO_TEST_CASE(wave12_height_gates_off_by_one_regtest)
{
    const Consensus::Params& params = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(params.nDDActivationHeight, 650);
    BOOST_REQUIRE_EQUAL(params.nOracleActivationHeight, params.nDDActivationHeight);
    BOOST_REQUIRE_EQUAL(params.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR), 0);
    BOOST_REQUIRE_EQUAL(params.nDigiDollarMuSig2Height, 0);

    BOOST_CHECK(!Consensus::IsOracleActive(params, params.nDDActivationHeight - 1));
    BOOST_CHECK(Consensus::IsOracleActive(params, params.nDDActivationHeight));
    BOOST_CHECK(Consensus::IsOracleActive(params, params.nDDActivationHeight + 1));

    BOOST_CHECK(!Consensus::IsMuSig2Active(params, params.nDigiDollarMuSig2Height - 1));
    BOOST_CHECK(Consensus::IsMuSig2Active(params, params.nDigiDollarMuSig2Height));
    BOOST_CHECK(Consensus::IsMuSig2Active(params, params.nDDActivationHeight - 1));
    BOOST_CHECK(Consensus::IsMuSig2Active(params, params.nDDActivationHeight));
    BOOST_CHECK(Consensus::IsMuSig2Active(params, params.nDDActivationHeight + 100));
}

// =============================================================================
// PART 3 — ValidateBlockOracleData boundary (BIP34 height path, nullptr prev)
// =============================================================================

// At h = nDDActivationHeight - 1, every shape of block (DD-touching, missing
// oracle, malformed oracle, multi-oracle, etc.) is accepted. The validator
// MUST short-circuit before any DD reject reason is emitted.
BOOST_AUTO_TEST_CASE(wave12_preactivation_validator_short_circuits_all_shapes)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_pre = params.nDDActivationHeight - 1;
    BOOST_REQUIRE_GE(h_pre, 1);
    const uint32_t t = static_cast<uint32_t>(GetTime());

    // Shape A: DD-touching block, no oracle output, pre-activation.
    {
        CBlock block = wave12::MakeBlock(h_pre, t, {}, {wave12::MakeDDTransaction(DD_TX_MINT)});
        wave12::OracleResult r = wave12::RunValidator(block, params);
        BOOST_TEST_MESSAGE("  pre-activation DD mint, no oracle: ok=" << r.ok << " reject='" << r.reject_reason << "'");
        BOOST_CHECK(r.ok);
        BOOST_CHECK_EQUAL(r.reject_reason, "");
    }

    // Shape B: non-DD block with two oracle outputs (would be rejected with
    // bad-oracle-multiple-outputs post-activation).
    {
        const COracleBundle good = wave12::MakeValidV03Bundle(params.nDDActivationHeight + 1, t);
        const CScript a = wave12::MakeRawV03Script(good);
        const CScript b = wave12::MakeRawV03Script(good);
        CBlock block = wave12::MakeBlock(h_pre, t, {a, b}, {});
        wave12::OracleResult r = wave12::RunValidator(block, params);
        BOOST_TEST_MESSAGE("  pre-activation multi-oracle, no DD: ok=" << r.ok << " reject='" << r.reject_reason << "'");
        BOOST_CHECK(r.ok);
        BOOST_CHECK_EQUAL(r.reject_reason, "");
    }

    // Shape C: pre-activation OP_ORACLE-shaped output with invalid version.
    {
        CBlock block = wave12::MakeBlock(h_pre, t, {wave12::MakeOracleLookalikeScript()}, {});
        wave12::OracleResult r = wave12::RunValidator(block, params);
        BOOST_CHECK(r.ok);
        BOOST_CHECK_EQUAL(r.reject_reason, "");
    }
}

// At h = nDDActivationHeight, the FIRST DD-touching block must succeed when
// the bundle is valid v0x03. This pins the activation-block success path:
// no off-by-one allows DD blocks to be rejected at the boundary.
BOOST_AUTO_TEST_CASE(wave12_first_dd_block_at_activation_succeeds_with_valid_bundle)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_act = params.nDDActivationHeight;
    const uint32_t t = static_cast<uint32_t>(GetTime());

    const COracleBundle good = wave12::MakeValidV03Bundle(h_act, t);
    const CScript good_script = wave12::MakeRawV03Script(good);
    CBlock block = wave12::MakeBlock(h_act, t, {good_script},
                                     {wave12::MakeDDTransaction(DD_TX_MINT)});
    wave12::OracleResult r = wave12::RunValidator(block, params);
    BOOST_TEST_MESSAGE("  first DD block at h=" << h_act << ": ok=" << r.ok << " reject='" << r.reject_reason << "'");
    BOOST_CHECK_MESSAGE(r.ok, "first DD-touching block at activation must accept valid v0x03 bundle, got reject='"
                              << r.reject_reason << "'");
    BOOST_CHECK_EQUAL(r.reject_reason, "");
}

// At h = nDDActivationHeight, a DD-touching block missing the bundle MUST be
// rejected (negative boundary). Mirrors Wave 8 acceptance matrix but pinned to
// the exact activation height.
BOOST_AUTO_TEST_CASE(wave12_first_dd_block_at_activation_missing_bundle_rejected)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_act = params.nDDActivationHeight;
    const uint32_t t = static_cast<uint32_t>(GetTime());

    CBlock block = wave12::MakeBlock(h_act, t, {}, {wave12::MakeDDTransaction(DD_TX_MINT)});
    wave12::OracleResult r = wave12::RunValidator(block, params);
    BOOST_TEST_MESSAGE("  first DD block at h=" << h_act << " no bundle: ok=" << r.ok << " reject='" << r.reject_reason << "'");
    BOOST_CHECK(!r.ok);
    BOOST_CHECK_EQUAL(r.reject_reason, "bad-oracle-missing");
}

// At h = nDDActivationHeight - 1, a DD-marker tx in a block ALONE does not
// trigger any DD reject reason from ValidateBlockOracleData. This is the
// "non-DD backward compatibility" surface for the validator hook.
BOOST_AUTO_TEST_CASE(wave12_pre_activation_dd_marker_block_not_rejected_for_dd)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_pre = params.nDDActivationHeight - 1;
    BOOST_REQUIRE_GE(h_pre, 1);
    const uint32_t t = static_cast<uint32_t>(GetTime());

    CBlock block = wave12::MakeBlock(h_pre, t, {}, {wave12::MakeDDTransaction(DD_TX_TRANSFER)});
    wave12::OracleResult r = wave12::RunValidator(block, params);

    // Even though the block touches DD, the pre-activation gate exits before
    // BlockTouchesDigiDollar is consulted. Reject reason must be empty.
    BOOST_CHECK(r.ok);
    BOOST_CHECK_EQUAL(r.reject_reason, "");
}

// =============================================================================
// PART 4 — ValidateBlockOracleData via pindex_prev (deployment path)
// =============================================================================

// When the validator is given pindex_prev, it consults
// IsDigiDollarEnabled(pindex_prev, params) instead of the BIP34 height fall-
// back. On default regtest the buried DigiDollarHeight is 0, so even at
// heights below nDDActivationHeight the deployment gate fires — and a
// DD-touching block without a bundle is rejected. This documents the rh51
// split: the nullptr-path uses nDDActivationHeight as the height gate but the
// pindex_prev-path uses the buried deployment height directly, so the two
// paths diverge on regtest for blocks 0..649.
//
// The expected behaviour is therefore intentionally asymmetric: the
// nullptr-path test (PART 3) accepts pre-activation DD blocks; the prev-path
// test rejects them when the buried deployment already considers DD enabled.
// Pinning both halves prevents accidental refactors that flatten the split
// into one behaviour.
BOOST_AUTO_TEST_CASE(wave12_validator_prev_path_uses_deployment_not_height_gate)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_pre = params.nDDActivationHeight - 1;
    BOOST_REQUIRE_GE(h_pre, 1);
    BOOST_REQUIRE(DigiDollar::IsDigiDollarEnabled(/*pindexPrev=*/nullptr, params));
    const uint32_t t = static_cast<uint32_t>(GetTime());

    CBlockIndex* prev = wave12::MakeIndex(nullptr, h_pre - 1, t - 600);
    BOOST_REQUIRE(DigiDollar::IsDigiDollarEnabled(prev, params));
    CBlock block = wave12::MakeBlock(h_pre, t, {}, {wave12::MakeDDTransaction(DD_TX_MINT)});
    wave12::OracleResult r = wave12::RunValidator(block, params, prev);

    // On default regtest the buried deployment is active from height 0, so
    // the prev-path validator considers DD enabled and enforces the bundle
    // requirement. This is the rh51-documented asymmetry vs the nullptr-path
    // height short-circuit at h < 650 that PART 3 exercises.
    BOOST_TEST_MESSAGE("  regtest prev-path @ h=" << h_pre
                       << " (buried active): ok=" << r.ok
                       << " reject='" << r.reject_reason << "'");
    BOOST_CHECK(!r.ok);
    BOOST_CHECK_EQUAL(r.reject_reason, "bad-oracle-missing");
}

// The mirror of the prev-path test at exactly the boundary: prev->nHeight =
// nDDActivationHeight - 1, building block at nDDActivationHeight. Whether the
// height-gate or the buried deployment-gate fires, the FIRST DD-touching block at the
// activation height MUST be enforced. This is the canonical "first DD block"
// boundary check on the prev path.
BOOST_AUTO_TEST_CASE(wave12_validator_with_pindex_prev_at_activation_enforces)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_act = params.nDDActivationHeight;
    const uint32_t t = static_cast<uint32_t>(GetTime());

    CBlockIndex* prev = wave12::MakeIndex(nullptr, h_act - 1, t - 600);
    CBlock block = wave12::MakeBlock(h_act, t, {}, {wave12::MakeDDTransaction(DD_TX_MINT)});
    wave12::OracleResult r = wave12::RunValidator(block, params, prev);
    BOOST_CHECK(!r.ok);
    BOOST_CHECK_EQUAL(r.reject_reason, "bad-oracle-missing");
}

// =============================================================================
// PART 5 — Non-DD blocks remain valid across the boundary (no oracle work)
// =============================================================================

// At every height around the boundary, a plain DGB block (no DD tx, no oracle
// output) is accepted. Pins the "non-DD users still work" property called out
// in MVP invariant 15 / launch readiness clause.
BOOST_AUTO_TEST_CASE(wave12_non_dd_blocks_accepted_across_boundary)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_act = params.nDDActivationHeight;
    const uint32_t t = static_cast<uint32_t>(GetTime());

    for (int delta : {-2, -1, 0, +1, +2}) {
        const int32_t h = h_act + delta;
        if (h < 1) continue;
        CBlock block = wave12::MakeBlock(h, t, {}, {});
        wave12::OracleResult r = wave12::RunValidator(block, params);
        BOOST_TEST_MESSAGE("  non-DD block at h=" << h << " (delta=" << delta << "): ok=" << r.ok
                           << " reject='" << r.reject_reason << "'");
        BOOST_CHECK_MESSAGE(r.ok, "non-DD block at h=" << h << " must validate, reject='"
                                  << r.reject_reason << "'");
        BOOST_CHECK_EQUAL(r.reject_reason, "");
    }
}

// =============================================================================
// PART 6 — CheckMuSig2OracleBundleVersion boundary
// =============================================================================

// Forward-declare the static helper exported through validation.cpp's
// translation unit; we reach it via a thin wrapper rather than re-implementing
// it. Direct call isn't visible at link time because it's static, so we
// exercise its branch through ValidateBlockOracleData (which shares the
// nullptr/pindex_prev dispatch and deployment/height gate). Track DD-FA-TEST-014 for
// a future direct probe if we ever export the helper.

// Pre-activation, a coinbase that already carries an OP_ORACLE bundle of any
// shape must still leave block validation untouched at the ValidateBlockOracle
// hook. This exercises the same gate CheckMuSig2OracleBundleVersion uses.
BOOST_AUTO_TEST_CASE(wave12_preactivation_oracle_lookalike_coinbase_ignored)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_pre = params.nDDActivationHeight - 1;
    BOOST_REQUIRE_GE(h_pre, 1);

    // Even an oracle-shaped output looks like a valid v0x03 (which would be
    // post-activation enforceable) is ignored at h_pre because the gate
    // returns true before extraction.
    const COracleBundle good = wave12::MakeValidV03Bundle(params.nDDActivationHeight + 1, GetTime());
    const CScript good_script = wave12::MakeRawV03Script(good);
    CBlock block = wave12::MakeBlock(h_pre, static_cast<uint32_t>(GetTime()),
                                     {good_script}, {});
    wave12::OracleResult r = wave12::RunValidator(block, params);
    BOOST_CHECK(r.ok);
    BOOST_CHECK_EQUAL(r.reject_reason, "");
}

// =============================================================================
// PART 7 — Multi-version interactions (DD-marker tx + invalid OP_RETURN)
// =============================================================================

// Pre-activation: a block whose only DD-shaped artefact is the nVersion
// marker (no DD OP_RETURN, no oracle output) must NOT be rejected for any
// DD-related reason. The non-upgraded base chain semantics apply.
BOOST_AUTO_TEST_CASE(wave12_preactivation_dd_marker_only_not_rejected_for_dd)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_pre = params.nDDActivationHeight - 1;
    BOOST_REQUIRE_GE(h_pre, 1);
    const uint32_t t = static_cast<uint32_t>(GetTime());

    // DD-marker tx with absolutely no DD OP_RETURN. Pre-activation, the
    // ValidateBlockOracleData hook should never reach the
    // BlockTouchesDigiDollar branch. This pins the contract that the
    // activation gate is the ONLY boundary, not the marker.
    CBlock block = wave12::MakeBlock(h_pre, t, {}, {wave12::MakeDDTransaction(DD_TX_REDEEM)});
    wave12::OracleResult r = wave12::RunValidator(block, params);
    BOOST_CHECK(r.ok);
    BOOST_CHECK_NE(r.reject_reason, "bad-oracle-missing");
    BOOST_CHECK_NE(r.reject_reason, "bad-oracle-malformed");
    BOOST_CHECK_NE(r.reject_reason, "bad-oracle-legacy");
    BOOST_CHECK_NE(r.reject_reason, "bad-oracle-musig2");
}

// Post-activation: same DD-marker tx + missing DD OP_RETURN reaches the V1
// rejection (bad-oracle-missing — the validator never gets to OP_RETURN
// validation because the bundle gate fires first).
BOOST_AUTO_TEST_CASE(wave12_postactivation_dd_marker_no_bundle_rejected_for_oracle)
{
    OracleBundleManager::GetInstance().Clear();
    const Consensus::Params& params = Params().GetConsensus();
    const int32_t h_act = params.nDDActivationHeight;
    const uint32_t t = static_cast<uint32_t>(GetTime());

    CBlock block = wave12::MakeBlock(h_act, t, {}, {wave12::MakeDDTransaction(DD_TX_REDEEM)});
    wave12::OracleResult r = wave12::RunValidator(block, params);
    BOOST_CHECK(!r.ok);
    BOOST_CHECK_EQUAL(r.reject_reason, "bad-oracle-missing");
}

// =============================================================================
// PART 8 — Mainnet/testnet activation parameter sanity (no consensus split)
// =============================================================================

// Pin the chainparams so that MuSig2 follows the effective DigiDollar
// activation boundary on every network. On mainnet/testnet that is the same
// numeric height as nDDActivationHeight; default regtest is special because
// the buried DigiDollarHeight is 0 while the oracle P2P height gate remains
// at 650 for local testing.
BOOST_AUTO_TEST_CASE(wave12_chainparams_collapsed_activation_triggers)
{
    struct Expected {
        ChainType chain;
        int dd_height;
        int oracle_height;
        int musig2_height;
        int buried_height;
    } cases[] = {
        {ChainType::REGTEST, 650, 650, 0, 0},
        {ChainType::TESTNET, 600, 600, 600, 600},
        {ChainType::MAIN, 23627520, 23627520, 23627520, 23869440},
    };

    for (const auto& c : cases) {
        SelectParams(c.chain);
        const auto& p = Params().GetConsensus();
        BOOST_TEST_MESSAGE("  chain=" << static_cast<int>(c.chain)
                           << " dd=" << p.nDDActivationHeight
                           << " oracle=" << p.nOracleActivationHeight
                           << " musig2=" << p.nDigiDollarMuSig2Height
                           << " buried=" << p.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR));
        BOOST_CHECK_EQUAL(p.nDDActivationHeight, c.dd_height);
        BOOST_CHECK_EQUAL(p.nOracleActivationHeight, c.oracle_height);
        BOOST_CHECK_EQUAL(p.nDigiDollarMuSig2Height, c.musig2_height);
        BOOST_CHECK_EQUAL(p.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR), c.buried_height);
        // Oracle P2P height never lives below the DD height gate; otherwise
        // the oracle message surface could open before the static DD gate.
        BOOST_CHECK_GE(p.nOracleActivationHeight, p.nDDActivationHeight);
        if (c.chain == ChainType::REGTEST) {
            BOOST_CHECK_EQUAL(p.nDigiDollarMuSig2Height,
                              p.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR));
        } else {
            BOOST_CHECK_EQUAL(p.nDigiDollarMuSig2Height, p.nDDActivationHeight);
        }
    }

    // Restore regtest at the end of the case so the suite fixture's
    // expectations are not perturbed for subsequent cases.
    SelectParams(ChainType::REGTEST);
}

// On mainnet the buried DigiDollar activation height (BIP9 'since' =
// 23,869,440) sits ABOVE the static nDDActivationHeight floor (23,627,520 —
// the historical BIP9 min_activation_height): signaling locked in later than
// the earliest allowed window, so post-burial the two values are no longer
// equal. The floor must stay at its historical value (it drives
// EarliestActivationFloor and the prune-lock/coin-gate contract) and must
// never exceed the buried activation height on mainnet.
BOOST_AUTO_TEST_CASE(wave12_mainnet_buried_height_vs_static_floor)
{
    SelectParams(ChainType::MAIN);
    const auto& p = Params().GetConsensus();
    const int buried_height = p.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);
    BOOST_CHECK_EQUAL(buried_height, 23869440);
    BOOST_CHECK_EQUAL(p.nDDActivationHeight, 23627520);
    BOOST_CHECK_LE(p.nDDActivationHeight, buried_height);
    BOOST_CHECK_EQUAL(DigiDollar::EarliestActivationFloor(p), p.nDDActivationHeight);
    SelectParams(ChainType::REGTEST);
}

// On testnet the buried activation height and the static gates collapse to
// the same block (600): DigiDollar locked in at exactly the
// min_activation_height floor, so a node that boots with stale chainparams
// cannot straddle the boundary.
BOOST_AUTO_TEST_CASE(wave12_testnet_buried_height_matches_nDDActivationHeight)
{
    SelectParams(ChainType::TESTNET);
    const auto& p = Params().GetConsensus();
    BOOST_CHECK_EQUAL(p.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR), 600);
    BOOST_CHECK_EQUAL(p.nDDActivationHeight, 600);
    BOOST_CHECK_EQUAL(DigiDollar::EarliestActivationFloor(p), 600);
    SelectParams(ChainType::REGTEST);
}

// =============================================================================
// PART 9 — DD-FA-SEC-010: SpendsDigiDollarCollateralVault activation-height
// optimization must not falsely skip vaults that the buried deployment
// already considers post-activation.
// =============================================================================
//
// `SpendsDigiDollarCollateralVault` short-circuits when `coin.nHeight <
// nDDActivationHeight` as a performance optimization (skip the
// `IsMintCollateralOutput` tx-db lookup for coins minted before DigiDollar
// could possibly exist).
//
// On mainnet/testnet the static floor never exceeds the buried
// DigiDollarHeight, so the optimization is equivalent to "the deployment was
// not yet active for this coin" and is safe.
//
// On regtest with default settings the buried DigiDollarHeight is 0 but
// `nDDActivationHeight=650`. A vault minted during the IBD/catch-up window
// where ConnectBlock skips oracle validation could land at coin.nHeight <
// 650. The optimization then *skips* the vault detection for that coin, and
// a later non-DD spend bypasses the `bad-collateral-spend-missing-dd-burn`
// consensus check.
//
// The fix floors the optimization at EarliestActivationFloor (min of the
// static gate and the buried height), so it never crosses the deployment
// boundary. This test pins the post-fix behaviour: a registered vault at
// coin.nHeight = 100 (well below nDDActivationHeight=650, but
// deployment-active on regtest) MUST be detected as a vault by
// SpendsDigiDollarCollateralVault.
BOOST_AUTO_TEST_CASE(wave12_regtest_low_height_vault_detection_consistent_with_burial)
{
    SelectParams(ChainType::REGTEST);
    const CChainParams& chainparams = Params();
    const Consensus::Params& consensus = chainparams.GetConsensus();
    BOOST_REQUIRE_EQUAL(consensus.nDDActivationHeight, 650);
    BOOST_REQUIRE_EQUAL(consensus.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR), 0);
    BOOST_REQUIRE(DigiDollar::IsDigiDollarEnabled(/*pindexPrev=*/nullptr, consensus));

    // Forge the mint that created the vault. Consensus only trusts the
    // creating transaction, never the script metadata registry, so the test
    // hands that transaction to SpendsDigiDollarCollateralVault through the
    // transaction lookup.
    CScript vault_script;
    vault_script << OP_1 << std::vector<unsigned char>(32, 0xAB);
    CMutableTransaction creating_mint;
    creating_mint.nVersion = 0x01000770; // DD_TX_MINT
    creating_mint.vin.emplace_back(COutPoint(uint256::ONE, 1));
    creating_mint.vout.emplace_back(10 * COIN, vault_script);
    creating_mint.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                                 << CScriptNum(1) << CScriptNum(100'000)
                                                 << CScriptNum(200) << CScriptNum(1));
    const CTransactionRef creating_ref = MakeTransactionRef(creating_mint);
    const DigiDollar::TxLookupFn lookup = [creating_ref](const uint256& txid, uint32_t, CTransactionRef& out) {
        if (txid != creating_ref->GetHash()) return false;
        out = creating_ref;
        return true;
    };

    constexpr uint32_t COIN_HEIGHT = 100;
    BOOST_REQUIRE_LT(COIN_HEIGHT, static_cast<uint32_t>(consensus.nDDActivationHeight));

    CCoinsView dummy;
    CCoinsViewCache coins(&dummy);
    const COutPoint vault_outpoint(creating_ref->GetHash(), 0);
    CTxOut vault_out(10 * COIN, vault_script);
    coins.AddCoin(vault_outpoint, Coin(vault_out, COIN_HEIGHT, /*fCoinBaseIn=*/false), false);

    CMutableTransaction non_dd_spend;
    non_dd_spend.nVersion = 2;  // Plain DGB tx, no DD marker
    non_dd_spend.vin.emplace_back(vault_outpoint);
    non_dd_spend.vout.emplace_back(9 * COIN, CScript() << OP_TRUE);
    CTransactionRef ref = MakeTransactionRef(std::move(non_dd_spend));

    DigiDollar::ValidationContext ctx(
        /*nHeight=*/1000,
        /*oraclePriceMicroUSD=*/0,
        /*systemCollateral=*/100,
        chainparams,
        &coins,
        /*skipOracle=*/true,
        lookup);

    const bool spends_vault = DigiDollar::SpendsDigiDollarCollateralVault(*ref, ctx);
    BOOST_TEST_MESSAGE("  regtest deployment-active vault @ coin.nHeight=" << COIN_HEIGHT
                       << " (nDDActivationHeight=" << consensus.nDDActivationHeight
                       << ") detected_as_vault=" << spends_vault);

    BOOST_CHECK_MESSAGE(spends_vault,
        "DD-FA-SEC-010: registered collateral vault at deployment-active height "
        << COIN_HEIGHT << " must be detected as a vault even when "
           "coin.nHeight < nDDActivationHeight; got false (production gates "
           "would let a non-DD tx spend the vault without burning DD).");

    const bool requires_dd = DigiDollar::RequiresDigiDollarValidation(*ref, ctx);
    BOOST_CHECK_MESSAGE(requires_dd,
        "DD-FA-SEC-010: RequiresDigiDollarValidation must return true for a "
        "non-DD tx that spends a real vault, otherwise mempool/miner/"
        "ConnectBlock skip the vault burn-enforcement gate.");
}

BOOST_AUTO_TEST_CASE(wave26_regtest_startup_oracle_cache_uses_activation_boundary)
{
    SelectParams(ChainType::REGTEST);
    const Consensus::Params& consensus = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(consensus.nDDActivationHeight, 650);
    BOOST_REQUIRE_EQUAL(consensus.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR), 0);

    const uint32_t t = static_cast<uint32_t>(GetTime());
    CBlockIndex* block = wave12::MakeChain(0, 101, t - 600 * 100, VERSIONBITS_TOP_BITS);
    CBlockIndex* prev = block->pprev;
    BOOST_REQUIRE(DigiDollar::IsDigiDollarEnabled(prev, consensus));
    BOOST_REQUIRE_LT(block->nHeight, consensus.nDDActivationHeight);

    BOOST_CHECK_MESSAGE(
        OracleBundleManager::ShouldLoadStartupOraclePriceForBlock(block->nHeight, block, consensus),
        "Wave 26: startup oracle cache reconstruction must follow the buried "
        "activation predicate used by ConnectBlock, not raw nDDActivationHeight; "
        "otherwise default-regtest blocks accepted while the buried deployment "
        "is active from genesis are skipped on restart/reindex.");
}

BOOST_AUTO_TEST_SUITE_END()
