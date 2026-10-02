// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/params.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/musig2_aggregator.h>
#include <primitives/oracle.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int32_t ROSTER_ACTIVE_HEIGHT = 650;
constexpr uint64_t ROSTER_PRICE = 51000;
constexpr int64_t ROSTER_TIMESTAMP = 1735689600;
constexpr uint16_t ROSTER_TOTAL_SLOTS = 35;
constexpr int ROSTER_ACTIVE_OPERATORS = 24;
constexpr int ROSTER_QUORUM = 7;

struct LocalMiniTestnetSetup : public BasicTestingSetup {
    LocalMiniTestnetSetup()
        : BasicTestingSetup{ChainType::TESTNET, {"-easypow=1"}}
    {
    }
};

std::array<unsigned char, 32> TestnetOracleSecret(uint8_t oracle_id)
{
    const std::string seed = "digibyte_testnet_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    std::array<unsigned char, 32> secret{};
    std::memcpy(secret.data(), hash.begin(), secret.size());

    return secret;
}

std::string TestnetOracleXOnlyHex(uint8_t oracle_id)
{
    const auto secret = TestnetOracleSecret(oracle_id);
    CKey key;
    key.Set(secret.begin(), secret.end(), true);
    const CPubKey pubkey = key.GetPubKey();
    std::vector<unsigned char> xonly(pubkey.begin() + 1, pubkey.end());
    return HexStr(xonly);
}

std::vector<unsigned char> EncodeBitmapUnchecked(const std::vector<uint8_t>& oracle_ids, uint16_t total_oracles)
{
    std::vector<unsigned char> bitmap((total_oracles + 7) / 8, 0);
    for (uint8_t id : oracle_ids) {
        bitmap[id / 8] |= static_cast<unsigned char>(1U << (id % 8));
    }
    return bitmap;
}

Consensus::Params ExpandedRosterParams(int32_t activation_height)
{
    Consensus::Params params = Params().GetConsensus();
    params.nOracleTotalOracles = ROSTER_TOTAL_SLOTS;
    params.nOraclePubkeyCount = ROSTER_ACTIVE_OPERATORS;
    params.nOracleRequiredMessages = ROSTER_QUORUM;
    params.nOracleConsensusRequired = ROSTER_QUORUM;
    params.nOracleActivationHeight = activation_height;
    params.nDigiDollarMuSig2Height = activation_height;

    params.vOraclePublicKeys.clear();
    for (uint8_t id = 0; id < ROSTER_ACTIVE_OPERATORS; ++id) {
        params.vOraclePublicKeys.push_back(TestnetOracleXOnlyHex(id));
    }
    return params;
}

bool SignTestnetV03Bundle(COracleBundle& bundle,
                          const std::vector<uint8_t>& oracle_ids,
                          uint16_t total_oracles,
                          const std::optional<uint256>& override_hash = std::nullopt)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    const size_t n_signers = oracle_ids.size();
    std::vector<std::array<unsigned char, 32>> seckeys(n_signers);
    std::vector<secp256k1_keypair> keypairs(n_signers);
    std::vector<secp256k1_pubkey> pubkeys(n_signers);

    for (size_t i = 0; i < n_signers; ++i) {
        seckeys[i] = TestnetOracleSecret(oracle_ids[i]);
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

    const uint256 msg_hash = override_hash ? *override_hash : ComputeOracleBundleHash(bundle);
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

COracleBundle MakeRosterBundle()
{
    COracleBundle bundle;
    bundle.version = 3;
    bundle.epoch = GetCurrentEpoch(ROSTER_ACTIVE_HEIGHT);
    bundle.median_price_micro_usd = ROSTER_PRICE;
    bundle.timestamp = ROSTER_TIMESTAMP;
    return bundle;
}

uint256 WrongRosterDomainHash(const COracleBundle& bundle)
{
    CHashWriter ss(0);
    ss << std::string{"DigiDollar/RosterWrongDomain"};
    ss << bundle.epoch;
    ss << bundle.median_price_micro_usd;
    ss << bundle.timestamp;
    return ss.GetHash();
}

struct PhaseThreeResult {
    bool ok;
    std::string error;
};

PhaseThreeResult ValidatePhaseThree(const COracleBundle& bundle,
                                    int32_t height,
                                    const Consensus::Params& params)
{
    std::string error;
    const bool ok = OracleBundleManager::ValidateMuSig2Bundle(bundle, height, params, error);
    return {ok, error};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_oracle_roster_tests, LocalMiniTestnetSetup)

BOOST_AUTO_TEST_CASE(local_mini_testnet_uses_small_assumed_storage_size)
{
    BOOST_CHECK_LE(Params().AssumedBlockchainSize(), 1U);
    BOOST_CHECK_EQUAL(Params().AssumedChainStateSize(), 0U);
}

BOOST_AUTO_TEST_CASE(expanded_roster_accepts_valid_7_sig_bundle_after_activation)
{
    const Consensus::Params params = ExpandedRosterParams(ROSTER_ACTIVE_HEIGHT);
    BOOST_REQUIRE_EQUAL(params.nOraclePubkeyCount, ROSTER_ACTIVE_OPERATORS);
    BOOST_REQUIRE_GT(params.nOraclePubkeyCount, 17);

    COracleBundle bundle = MakeRosterBundle();
    const std::vector<uint8_t> signer_ids{0, 1, 2, 3, 4, 5, 6};
    BOOST_REQUIRE(SignTestnetV03Bundle(bundle, signer_ids, ROSTER_TOTAL_SLOTS));

    PhaseThreeResult result = ValidatePhaseThree(bundle, ROSTER_ACTIVE_HEIGHT, params);
    BOOST_TEST_MESSAGE("expanded roster after activation: ok=" << result.ok
                       << " error='" << result.error << "'");

    BOOST_CHECK_MESSAGE(result.ok,
        "a valid 7-sig MuSig2 bundle from the active operator roster must verify after activation");
}

BOOST_AUTO_TEST_CASE(expanded_roster_rejects_same_bundle_before_activation)
{
    const Consensus::Params params = ExpandedRosterParams(ROSTER_ACTIVE_HEIGHT + 1);

    COracleBundle bundle = MakeRosterBundle();
    const std::vector<uint8_t> signer_ids{0, 1, 2, 3, 4, 5, 6};
    BOOST_REQUIRE(SignTestnetV03Bundle(bundle, signer_ids, ROSTER_TOTAL_SLOTS));

    PhaseThreeResult result = ValidatePhaseThree(bundle, ROSTER_ACTIVE_HEIGHT, params);
    BOOST_TEST_MESSAGE("expanded roster before activation: ok=" << result.ok
                       << " error='" << result.error << "'");

    BOOST_CHECK_MESSAGE(!result.ok,
        "the same active roster bundle must be rejected before roster activation");
    BOOST_CHECK_MESSAGE(result.error.find("activation") != std::string::npos,
        "pre-activation rejection should identify the roster activation gate, got: " + result.error);
}

BOOST_AUTO_TEST_CASE(expanded_roster_rejects_out_of_roster_signer)
{
    const Consensus::Params params = ExpandedRosterParams(ROSTER_ACTIVE_HEIGHT);

    COracleBundle bundle = MakeRosterBundle();
    const uint8_t first_reserve_id = static_cast<uint8_t>(params.nOraclePubkeyCount);
    const std::vector<uint8_t> signer_ids{0, 1, 2, 3, 4, 5, first_reserve_id};
    BOOST_REQUIRE(SignTestnetV03Bundle(bundle, signer_ids, ROSTER_TOTAL_SLOTS));

    PhaseThreeResult result = ValidatePhaseThree(bundle, ROSTER_ACTIVE_HEIGHT, params);
    BOOST_TEST_MESSAGE("out-of-roster signer: ok=" << result.ok
                       << " error='" << result.error << "'");

    BOOST_CHECK_MESSAGE(!result.ok,
        "first reserve signer id " + std::to_string(first_reserve_id) +
            " is outside the active operator roster and must reject");
    BOOST_CHECK_MESSAGE(!result.error.empty(), "out-of-roster rejection must report an error");
}

BOOST_AUTO_TEST_CASE(expanded_roster_rejects_duplicate_signer_bitmap_input)
{
    const std::vector<uint8_t> duplicate_signers{0, 1, 2, 3, 4, 5, 6, 6};
    const std::vector<unsigned char> bitmap =
        MuSig2OracleAggregator::EncodeBitmap(duplicate_signers, ROSTER_TOTAL_SLOTS);

    BOOST_CHECK_MESSAGE(bitmap.empty(),
        "duplicate signer ids must not produce an encodable MuSig2 participation bitmap");
}

BOOST_AUTO_TEST_CASE(expanded_roster_rejects_wrong_domain_signature)
{
    const Consensus::Params params = ExpandedRosterParams(ROSTER_ACTIVE_HEIGHT);

    COracleBundle bundle = MakeRosterBundle();
    const std::vector<uint8_t> signer_ids{0, 1, 2, 3, 4, 5, 6};
    BOOST_REQUIRE(SignTestnetV03Bundle(bundle, signer_ids, ROSTER_TOTAL_SLOTS,
                                       WrongRosterDomainHash(bundle)));

    PhaseThreeResult result = ValidatePhaseThree(bundle, ROSTER_ACTIVE_HEIGHT, params);
    BOOST_TEST_MESSAGE("wrong-domain signature: ok=" << result.ok
                       << " error='" << result.error << "'");

    BOOST_CHECK_MESSAGE(!result.ok,
        "aggregate signatures from a different signing domain must reject");
    BOOST_CHECK_MESSAGE(result.error.find("signature") != std::string::npos,
        "wrong-domain rejection should be an aggregate signature failure, got: " + result.error);
}

BOOST_AUTO_TEST_SUITE_END()
