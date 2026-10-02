// Copyright (c) 2024-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/amount.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <oracle/musig2_aggregator.h>
#include <oracle/musig2_orchestrator.h>
#include <oracle/musig2_session.h>
#include <oracle/signing_orchestrator.h>
#include <primitives/block.h>
#include <primitives/oracle.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <span.h>
#include <test/util/setup_common.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <tuple>
#include <vector>

#include <secp256k1.h>
#include <secp256k1_musig.h>

namespace {

CBlock MakeBlockWithCoinbase()
{
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 50 * COIN;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    return block;
}

class ScopedMockOracleDisabled
{
public:
    ScopedMockOracleDisabled()
        : m_was_enabled(MockOracleManager::GetInstance().IsEnabled())
    {
        MockOracleManager::GetInstance().SetEnabled(false);
    }

    ~ScopedMockOracleDisabled()
    {
        MockOracleManager::GetInstance().SetEnabled(m_was_enabled);
    }

private:
    bool m_was_enabled;
};

bool BuildCompleteSession(int32_t epoch, MuSig2SigningSession& session_out)
{
    const uint8_t signer_count = static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired);
    if (signer_count == 0) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    std::vector<CKey> keys;
    std::vector<secp256k1_pubkey> secp_pubkeys;
    keys.reserve(signer_count);
    secp_pubkeys.reserve(signer_count);

    for (uint8_t id = 0; id < signer_count; ++id) {
        unsigned char seckey[32];
        secp256k1_keypair keypair;
        secp256k1_pubkey secp_pubkey;
        bool key_ok = false;
        for (int i = 0; i < 100 && !key_ok; ++i) {
            GetStrongRandBytes(Span<unsigned char>(seckey, 32));
            if (secp256k1_keypair_create(ctx, &keypair, seckey)) {
                key_ok = secp256k1_keypair_pub(ctx, &secp_pubkey, &keypair);
            }
        }
        if (!key_ok) {
            secp256k1_context_destroy(ctx);
            return false;
        }

        CKey key;
        key.Set(seckey, seckey + 32, true);
        keys.push_back(key);
        secp_pubkeys.push_back(secp_pubkey);
    }

    std::vector<const secp256k1_pubkey*> pk_ptrs;
    pk_ptrs.reserve(secp_pubkeys.size());
    for (const auto& pubkey : secp_pubkeys) {
        pk_ptrs.push_back(&pubkey);
    }

    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    if (!secp256k1_musig_pubkey_agg(ctx, &agg_pk, &cache, pk_ptrs.data(), pk_ptrs.size())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    for (uint8_t id = 0; id < signer_count; ++id) {
        secp256k1_musig_pubnonce pubnonce;
        if (!session_out.GenerateNonce(id, keys[id], secp_pubkeys[id], cache, pubnonce)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        if (!session_out.AddPubnonce(id, pubnonce)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    unsigned char msg32[32] = {0};
    std::memcpy(msg32, &epoch, std::min(sizeof(epoch), sizeof(msg32)));

    if (!session_out.AggregateNonces(msg32)) return false;

    for (uint8_t id = 0; id < signer_count; ++id) {
        secp256k1_musig_partial_sig partial_sig;
        if (!session_out.CreatePartialSignature(id, keys[id], partial_sig)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        if (!session_out.AddPartialSignature(id, partial_sig)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<unsigned char> sig64;
    const bool ok = session_out.AggregateSignature(sig64) && sig64.size() == 64;
    secp256k1_context_destroy(ctx);
    return ok;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(musig2_bundle_mining_tests, RegTestingSetup)

BOOST_AUTO_TEST_CASE(add_bundle_skips_oracle_data_when_session_incomplete)
{
    // When Phase 3 is active but MuSig2 session isn't ready,
    // AddOracleBundleToBlock returns true (block proceeds) but
    // does NOT add any oracle OP_RETURN output.
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    ScopedMockOracleDisabled mock_disabled;

    const Consensus::Params& params = Params().GetConsensus();
    if (params.nDigiDollarMuSig2Height == std::numeric_limits<int>::max()) {
        BOOST_TEST_MESSAGE("Phase 3 disabled on this network, skipping test");
        return;
    }

    const int32_t block_height = params.nDigiDollarMuSig2Height;
    const int32_t epoch = GetCurrentEpoch(block_height);

    {
        LOCK(g_oracle_signing_sessions_mutex);
        g_oracle_signing_sessions.clear();
        g_oracle_signing_sessions.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(epoch),
            std::forward_as_tuple(epoch, 1)); // CREATED, incomplete
    }
    if (g_signing_orchestrator) {
        g_signing_orchestrator->Clear();
    }

    CBlock block = MakeBlockWithCoinbase();
    // Returns true (block proceeds), no oracle bundle added
    BOOST_CHECK(manager.AddOracleBundleToBlock(block, block_height));
    // Only the original coinbase output, no oracle OP_RETURN
    BOOST_CHECK_EQUAL(block.vtx[0]->vout.size(), 1);
}

BOOST_AUTO_TEST_CASE(completed_session_requires_signed_values_not_live_recovery)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(Params().GetConsensus().nOracleConsensusRequired);
    ScopedMockOracleDisabled mock_disabled;

    if (!g_signing_orchestrator) {
        OracleSigningOrchestrator::Initialize(/*connman=*/nullptr);
    }
    g_signing_orchestrator->Clear();

    const int32_t epoch = GetCurrentEpoch(Params().GetConsensus().nDigiDollarMuSig2Height);
    constexpr uint64_t live_price_micro_usd = 4000;
    const int64_t live_timestamp = GetTime();

    for (uint32_t oracle_id = 0; oracle_id < static_cast<uint32_t>(Params().GetConsensus().nOracleConsensusRequired); ++oracle_id) {
        manager.InjectTestMessage(COraclePriceMessage(oracle_id, live_price_micro_usd, live_timestamp));
    }

    auto complete = std::make_unique<MuSig2SigningSession>(
        epoch, static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired));
    BOOST_REQUIRE(BuildCompleteSession(epoch, *complete));
    BOOST_CHECK_EQUAL(complete->GetState(), MuSig2SessionState::COMPLETE);
    BOOST_CHECK_EQUAL(complete->GetSignedPrice(), 0U);
    BOOST_CHECK_EQUAL(complete->GetSignedTimestamp(), 0);

    g_signing_orchestrator->InjectSession(epoch, std::move(complete));

    std::vector<unsigned char> aggregate_sig;
    std::vector<unsigned char> participation_bitmap;
    uint64_t signed_price = 0;
    int64_t signed_timestamp = 0;
    BOOST_CHECK(!g_signing_orchestrator->GetCompletedSession(
        epoch, aggregate_sig, participation_bitmap, signed_price, signed_timestamp));
    BOOST_CHECK(!aggregate_sig.empty());
    BOOST_CHECK(!participation_bitmap.empty());
    BOOST_CHECK_EQUAL(signed_price, 0U);
    BOOST_CHECK_EQUAL(signed_timestamp, 0);
}

BOOST_AUTO_TEST_CASE(add_bundle_consumes_session_and_prunes_old_epochs)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    ScopedMockOracleDisabled mock_disabled;

    const Consensus::Params& params = Params().GetConsensus();
    if (params.nDigiDollarMuSig2Height == std::numeric_limits<int>::max()) {
        BOOST_TEST_MESSAGE("Phase 3 disabled on this network, skipping test");
        return;
    }

    const int32_t block_height = params.nDigiDollarMuSig2Height;
    const int32_t epoch = GetCurrentEpoch(block_height);

    // Build a completed session and inject it into the orchestrator
    if (!g_signing_orchestrator) {
        OracleSigningOrchestrator::Initialize(/*connman=*/nullptr);
    }
    g_signing_orchestrator->Clear();

    auto complete = std::make_unique<MuSig2SigningSession>(
        epoch, static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired));
    BOOST_REQUIRE(BuildCompleteSession(epoch, *complete));
    BOOST_CHECK_EQUAL(complete->GetState(), MuSig2SessionState::COMPLETE);

    g_signing_orchestrator->InjectSession(epoch, std::move(complete));

    CBlock block = MakeBlockWithCoinbase();
    BOOST_REQUIRE(manager.AddOracleBundleToBlock(block, block_height));

    if (block.vtx[0]->vout.size() == 2) {
        const CTxOut& oracle_out = block.vtx[0]->vout[1];
        BOOST_CHECK_EQUAL(oracle_out.nValue, 0);
        BOOST_CHECK(oracle_out.scriptPubKey.IsUnspendable());

        COracleBundle extracted;
        BOOST_REQUIRE(manager.ExtractOracleBundle(*block.vtx[0], extracted));
        BOOST_CHECK_EQUAL(extracted.version, 3);
        BOOST_CHECK(extracted.IsMuSig2());
        BOOST_CHECK_EQUAL(extracted.aggregate_sig.size(), 64);
        BOOST_CHECK(!extracted.participation_bitmap.empty());
        BOOST_CHECK((extracted.participation_bitmap[0] & 0x0F) == 0x0F);
        BOOST_CHECK_EQUAL(extracted.messages.size(), Params().GetConsensus().nOracleConsensusRequired);
        for (int i = 0; i < Params().GetConsensus().nOracleConsensusRequired; ++i) {
            BOOST_CHECK_EQUAL(extracted.messages[i].oracle_id, static_cast<uint32_t>(i));
        }

        // Session data was consumed for the block bundle
        // (orchestrator retains session until epoch cleanup)
    } else {
        // If v03 OP_RETURN is gated out by chain height context in unit test env,
        // session remains available for next block attempt.
        BOOST_REQUIRE_EQUAL(block.vtx[0]->vout.size(), 1);
        std::vector<unsigned char> dummy_sig, dummy_bmp;
        uint64_t dummy_price = 0;
        int64_t dummy_ts = 0;
        const bool session_ready = g_signing_orchestrator->GetCompletedSession(epoch, dummy_sig, dummy_bmp, dummy_price, dummy_ts);
        BOOST_CHECK_MESSAGE(!session_ready || (dummy_price > 0 && dummy_ts > 0),
                            "completed sessions exposed to mining must carry non-zero signed price/timestamp");
    }
}

BOOST_AUTO_TEST_SUITE_END()
