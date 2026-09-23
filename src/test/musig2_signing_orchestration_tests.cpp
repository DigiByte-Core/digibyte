// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <addrman.h>
#include <chainparams.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <net.h>
#include <netgroup.h>
#include <oracle/bundle_manager.h>
#include <oracle/musig2_aggregator.h>
#include <oracle/musig2_messages.h>
#include <oracle/node.h>
#include <oracle/signing_orchestrator.h>
#include <primitives/block.h>
#include <primitives/oracle.h>
#include <protocol.h>
#include <sync.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <util/strencodings.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(musig2_signing_orchestration_tests, RegTestingSetup)

static CKey GetRegtestMusigOracleKey(uint32_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    CKey key;
    key.Set(hash.begin(), hash.end(), true);
    return key;
}

static std::vector<uint8_t> GetActiveOracleIdsForMusigTest()
{
    std::vector<uint8_t> ids;
    const uint32_t total = static_cast<uint32_t>(Params().GetConsensus().nOracleTotalOracles);
    for (const auto& node : Params().GetOracleNodes()) {
        if (node.is_active && node.id < total) {
            ids.push_back(static_cast<uint8_t>(node.id));
        }
    }
    return ids;
}

static uint256 UniqueMusigTestHash(uint64_t value)
{
    uint256 hash;
    for (size_t i = 0; i < sizeof(value); ++i) {
        hash.begin()[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    }
    return hash;
}

static std::vector<uint8_t> SortMusigTestIdsBySeed(std::vector<uint8_t> ids,
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

static uint256 ComputeMusigTestQuoteSetHash(int32_t epoch,
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

static COraclePriceMessage MakeSignedMusigPriceEvidence(uint8_t oracle_id,
                                                        uint64_t price,
                                                        int64_t timestamp)
{
    CKey key = GetRegtestMusigOracleKey(oracle_id);
    COraclePriceMessage msg(oracle_id, price, timestamp);
    msg.oracle_pubkey = XOnlyPubKey(key.GetPubKey());
    BOOST_REQUIRE(msg.SignAttestation(key));
    BOOST_REQUIRE(msg.VerifyAttestation());
    return msg;
}

static OracleMusigNonceMsg MakeSignedMusigNonceMsg(int32_t epoch, uint8_t oracle_id)
{
    CKey key = GetRegtestMusigOracleKey(oracle_id);
    CPubKey pubkey = key.GetPubKey();

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx);

    secp256k1_pubkey secp_pubkey;
    BOOST_REQUIRE(secp256k1_ec_pubkey_parse(ctx, &secp_pubkey, pubkey.data(), pubkey.size()));

    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache keyagg_cache;
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(GetActiveOracleIdsForMusigTest(), agg_pk, keyagg_cache));

    MuSig2SigningSession temp_session(epoch, static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired));
    secp256k1_musig_pubnonce pubnonce;
    BOOST_REQUIRE(temp_session.GenerateNonce(oracle_id, key, secp_pubkey, keyagg_cache, pubnonce));

    OracleMusigNonceMsg msg;
    msg.epoch = epoch;
    msg.oracle_id = oracle_id;
    msg.pubnonce.resize(66);
    BOOST_REQUIRE(secp256k1_musig_pubnonce_serialize(ctx, msg.pubnonce.data(), &pubnonce));
    secp256k1_context_destroy(ctx);

    BOOST_REQUIRE(msg.Sign(key));
    return msg;
}

static std::vector<OracleMusigPartialSigMsg> MakeThresholdPartialSigMessages(
    int32_t epoch,
    uint64_t price,
    int64_t timestamp,
    MuSig2SigningSession& receiving_session)
{
    const uint8_t threshold = static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired);
    std::vector<uint8_t> participant_ids = GetActiveOracleIdsForMusigTest();
    BOOST_REQUIRE_GE(participant_ids.size(), threshold);
    participant_ids.resize(threshold);

    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey full_agg_pk;
    secp256k1_musig_keyagg_cache full_keyagg_cache;
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(GetActiveOracleIdsForMusigTest(),
                                                    full_agg_pk,
                                                    full_keyagg_cache));

    secp256k1_xonly_pubkey participant_agg_pk;
    secp256k1_musig_keyagg_cache participant_keyagg_cache;
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(participant_ids,
                                                    participant_agg_pk,
                                                    participant_keyagg_cache));

    MuSig2SigningSession signing_session(epoch, threshold);
    BOOST_REQUIRE(receiving_session.InitializePassive(full_keyagg_cache));

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx);

    for (uint8_t oracle_id : participant_ids) {
        CKey key = GetRegtestMusigOracleKey(oracle_id);
        CPubKey pubkey = key.GetPubKey();

        secp256k1_pubkey secp_pubkey;
        BOOST_REQUIRE(secp256k1_ec_pubkey_parse(ctx, &secp_pubkey, pubkey.data(), pubkey.size()));

        secp256k1_musig_pubnonce pubnonce;
        BOOST_REQUIRE(signing_session.GenerateNonce(oracle_id,
                                                    key,
                                                    secp_pubkey,
                                                    full_keyagg_cache,
                                                    pubnonce));
        BOOST_REQUIRE(signing_session.AddPubnonce(oracle_id, pubnonce));
        BOOST_REQUIRE(receiving_session.AddPubnonce(oracle_id, pubnonce));
    }

    unsigned char msg32[32];
    OracleSigningOrchestrator::ComputeOracleMessageHash(epoch, price, timestamp, msg32);

    signing_session.SetSignedValues(price, timestamp);
    signing_session.TrimNoncesToThreshold();
    signing_session.SetKeyAggCache(participant_keyagg_cache);
    BOOST_REQUIRE(signing_session.AggregateNonces(msg32));

    receiving_session.SetSignedValues(price, timestamp);
    receiving_session.TrimNoncesToThreshold();
    receiving_session.SetKeyAggCache(participant_keyagg_cache);

    std::vector<OracleMusigPartialSigMsg> partials;
    partials.reserve(participant_ids.size());

    for (uint8_t oracle_id : participant_ids) {
        CKey key = GetRegtestMusigOracleKey(oracle_id);

        secp256k1_musig_partial_sig partial_sig;
        BOOST_REQUIRE(signing_session.CreatePartialSignature(oracle_id, key, partial_sig));

        OracleMusigPartialSigMsg msg;
        msg.epoch = epoch;
        msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
        msg.session_context_id = signing_session.GetSessionContextId();
        msg.oracle_id = oracle_id;
        msg.partial_sig.resize(32);
        BOOST_REQUIRE(secp256k1_musig_partial_sig_serialize(ctx, msg.partial_sig.data(), &partial_sig));
        BOOST_REQUIRE(msg.Sign(key));

        partials.push_back(std::move(msg));
    }

    secp256k1_context_destroy(ctx);
    return partials;
}

struct PassiveMuSig2Transcript {
    std::vector<OracleMusigNonceMsg> nonce_msgs;
    OracleMusigContextMsg context_msg;
    std::vector<OracleMusigPartialSigMsg> partial_sig_msgs;
    uint64_t price{0};
    int64_t timestamp{0};
    int32_t tick_height{0};
};

static PassiveMuSig2Transcript MakePassiveThresholdTranscript(int32_t epoch)
{
    const uint8_t threshold = static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired);
    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    const uint256 seed = Params().GetConsensus().hashGenesisBlock;
    const uint64_t price = 50000;
    const int64_t timestamp = GetTime();

    std::vector<uint8_t> all_ids = GetActiveOracleIdsForMusigTest();
    BOOST_REQUIRE_GE(all_ids.size(), threshold);
    std::vector<uint8_t> participant_ids = SortMusigTestIdsBySeed(all_ids, epoch, seed);
    participant_ids.resize(threshold);

    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey full_agg_pk;
    secp256k1_musig_keyagg_cache full_keyagg_cache;
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(all_ids, full_agg_pk, full_keyagg_cache));

    secp256k1_xonly_pubkey participant_agg_pk;
    secp256k1_musig_keyagg_cache participant_keyagg_cache;
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(participant_ids,
                                                    participant_agg_pk,
                                                    participant_keyagg_cache));

    MuSig2SigningSession signing_session(epoch, threshold);
    signing_session.SetEpochSelectionSeed(seed);

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx);

    std::vector<OracleMusigNonceMsg> nonce_msgs;
    nonce_msgs.reserve(participant_ids.size());
    for (uint8_t oracle_id : participant_ids) {
        CKey key = GetRegtestMusigOracleKey(oracle_id);
        CPubKey pubkey = key.GetPubKey();

        secp256k1_pubkey secp_pubkey;
        BOOST_REQUIRE(secp256k1_ec_pubkey_parse(ctx, &secp_pubkey, pubkey.data(), pubkey.size()));

        secp256k1_musig_pubnonce pubnonce;
        BOOST_REQUIRE(signing_session.GenerateNonce(oracle_id,
                                                    key,
                                                    secp_pubkey,
                                                    full_keyagg_cache,
                                                    pubnonce));
        BOOST_REQUIRE(signing_session.AddPubnonce(oracle_id, pubnonce));

        OracleMusigNonceMsg msg;
        msg.epoch = epoch;
        msg.attempt_id = static_cast<uint8_t>(signing_session.GetAttemptId());
        msg.oracle_id = oracle_id;
        msg.pubnonce.resize(66);
        BOOST_REQUIRE(secp256k1_musig_pubnonce_serialize(ctx, msg.pubnonce.data(), &pubnonce));
        BOOST_REQUIRE(msg.Sign(key));
        nonce_msgs.push_back(std::move(msg));
    }

    BOOST_REQUIRE_EQUAL(signing_session.GetState(), MuSig2SessionState::NONCES_COMPLETE);

    std::vector<COraclePriceMessage> price_evidence;
    price_evidence.reserve(participant_ids.size());
    for (uint8_t oracle_id : participant_ids) {
        price_evidence.push_back(MakeSignedMusigPriceEvidence(oracle_id, price, timestamp));
    }

    COracleBundle evidence_bundle;
    evidence_bundle.messages = price_evidence;
    const CAmount consensus_price =
        OracleBundleManager::CalculateConsensusPrice(evidence_bundle, Params().GetConsensus());
    BOOST_REQUIRE_EQUAL(consensus_price, static_cast<CAmount>(price));

    unsigned char msg32[32];
    OracleSigningOrchestrator::ComputeOracleMessageHash(epoch, price, timestamp, msg32);

    uint256 nonce_set_hash;
    uint256 context_id;
    BOOST_REQUIRE(signing_session.ComputeContextIdForParticipants(participant_ids,
                                                                  msg32,
                                                                  nonce_set_hash,
                                                                  context_id));

    const uint8_t proposer_id = SortMusigTestIdsBySeed(all_ids, epoch, seed).front();
    OracleMusigContextMsg context_msg;
    context_msg.epoch = epoch;
    context_msg.attempt_id = static_cast<uint8_t>(signing_session.GetAttemptId());
    context_msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
    context_msg.epoch_selection_seed = seed;
    context_msg.proposer_id = proposer_id;
    context_msg.participant_ids = participant_ids;
    context_msg.nonce_set_hash = nonce_set_hash;
    context_msg.quote_set_hash = ComputeMusigTestQuoteSetHash(epoch, price_evidence);
    context_msg.consensus_price = price;
    context_msg.consensus_timestamp = timestamp;
    context_msg.session_context_id = context_id;
    context_msg.nonce_evidence = nonce_msgs;
    context_msg.price_evidence = price_evidence;
    BOOST_REQUIRE(context_msg.Sign(GetRegtestMusigOracleKey(proposer_id)));
    BOOST_REQUIRE(context_msg.IsValid());

    signing_session.SetSignedValues(price, timestamp);
    BOOST_REQUIRE(signing_session.TrimNoncesToParticipants(participant_ids));
    signing_session.SetKeyAggCache(participant_keyagg_cache);
    BOOST_REQUIRE(signing_session.AggregateNonces(msg32));
    BOOST_REQUIRE(context_id == signing_session.GetSessionContextId());

    std::vector<OracleMusigPartialSigMsg> partials;
    partials.reserve(participant_ids.size());
    for (uint8_t oracle_id : participant_ids) {
        CKey key = GetRegtestMusigOracleKey(oracle_id);

        secp256k1_musig_partial_sig partial_sig;
        BOOST_REQUIRE(signing_session.CreatePartialSignature(oracle_id, key, partial_sig));

        OracleMusigPartialSigMsg msg;
        msg.epoch = epoch;
        msg.attempt_id = static_cast<uint8_t>(signing_session.GetAttemptId());
        msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
        msg.session_context_id = context_id;
        msg.oracle_id = oracle_id;
        msg.partial_sig.resize(32);
        BOOST_REQUIRE(secp256k1_musig_partial_sig_serialize(ctx, msg.partial_sig.data(), &partial_sig));
        BOOST_REQUIRE(msg.Sign(key));
        partials.push_back(std::move(msg));
    }

    secp256k1_context_destroy(ctx);

    PassiveMuSig2Transcript transcript;
    transcript.nonce_msgs = std::move(nonce_msgs);
    transcript.context_msg = std::move(context_msg);
    transcript.partial_sig_msgs = std::move(partials);
    transcript.price = price;
    transcript.timestamp = timestamp;
    transcript.tick_height = epoch * epoch_length + 2;
    return transcript;
}

static OracleMusigPartialSigMsg MakeSignedGarbagePartialSigMsg(int32_t epoch,
                                                               uint8_t oracle_id,
                                                               uint64_t seed)
{
    OracleMusigPartialSigMsg msg;
    msg.epoch = epoch;
    msg.oracle_id = oracle_id;
    msg.partial_sig.resize(32);
    for (size_t i = 0; i < msg.partial_sig.size(); ++i) {
        msg.partial_sig[i] = static_cast<unsigned char>((seed * 0x9E3779B97F4A7C15ULL) + i);
    }
    msg.partial_sig[0] &= 0x3F;
    BOOST_REQUIRE(msg.Sign(GetRegtestMusigOracleKey(oracle_id)));
    return msg;
}

// Regression test for the "MuSig2 v0x02 fallback every block" bug.
//
// Before this fix, OracleSigningOrchestrator only created a session
// for epoch N when the first block of that epoch connected. Block
// template assembly (bundle_manager.cpp AddOracleBundleToBlock) runs
// BEFORE the block it is building connects, so when the miner
// templated block N*epoch_length and queried the orchestrator for
// epoch N's completed session, no session existed — and the miner
// fell through to the v0x02 individual-signature fallback. Even once
// the session was eventually created, the MuSig2 ceremony needs ~3
// block ticks (nonce, partial-sig, aggregate) to reach COMPLETE, so
// the first few blocks of every epoch also fell back.
//
// Fix: in the tail of epoch N (last K blocks), the orchestrator also
// creates/ticks a session for epoch N+1 so the ceremony has time to
// reach COMPLETE before block (N+1)*epoch_length is templated.
BOOST_AUTO_TEST_CASE(prestart_next_epoch_session_at_boundary)
{
    OracleSigningOrchestrator orch;

    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    BOOST_REQUIRE_GT(epoch_length, 0);

    // Pick a height 3 blocks before the next-epoch boundary. With
    // regtest epoch_length=10 that's height 7, current_epoch=0,
    // next_epoch=1.
    const int32_t tail_height = epoch_length - 3;
    const int32_t current_epoch = tail_height / epoch_length;
    const int32_t next_epoch = current_epoch + 1;

    BOOST_REQUIRE(!orch.HasSession(current_epoch));
    BOOST_REQUIRE(!orch.HasSession(next_epoch));

    std::shared_ptr<const CBlock> empty_block;
    orch.OnBlockConnected(empty_block, tail_height);

    // Current epoch's session always exists (pre-existing behavior).
    BOOST_CHECK(orch.HasSession(current_epoch));
    // Next epoch's session must also exist — this is what the fix
    // introduces. Without pre-start this would be false.
    BOOST_CHECK(orch.HasSession(next_epoch));
}

// Outside the pre-start window we should NOT create the next-epoch
// session — premature creation during the middle of an epoch wastes
// memory and noise.
BOOST_AUTO_TEST_CASE(no_prestart_mid_epoch)
{
    OracleSigningOrchestrator orch;

    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    BOOST_REQUIRE_GT(epoch_length, 0);

    // Mid-epoch: far from the boundary, no pre-start expected.
    // Use height 1 (position 1 within epoch 0) to be unambiguously
    // outside the pre-start window on any epoch_length (regtest=10,
    // testnet=50, mainnet=100+).
    const int32_t mid_height = 1;
    BOOST_REQUIRE_GT(epoch_length, mid_height + 5);
    const int32_t current_epoch = mid_height / epoch_length;
    const int32_t next_epoch = current_epoch + 1;

    std::shared_ptr<const CBlock> empty_block;
    orch.OnBlockConnected(empty_block, mid_height);

    BOOST_CHECK(orch.HasSession(current_epoch));
    BOOST_CHECK(!orch.HasSession(next_epoch));
}

BOOST_AUTO_TEST_CASE(remote_nonce_lazy_session_accepts_first_nonce)
{
    OracleSigningOrchestrator orch;
    const int32_t epoch = 42;
    const uint8_t oracle_id = 1;

    BOOST_REQUIRE(!orch.HasSession(epoch));

    OracleMusigNonceMsg msg = MakeSignedMusigNonceMsg(epoch, oracle_id);
    orch.IngestRemoteNonce(msg);

    BOOST_REQUIRE(orch.HasSession(epoch));
    MuSig2SigningSession* session = orch.GetOrCreateSigningSession(epoch, epoch * 10);
    BOOST_REQUIRE(session != nullptr);
    BOOST_CHECK_EQUAL(session->GetNonceCount(), 1U);
    BOOST_CHECK(session->GetState() == MuSig2SessionState::NONCES_COLLECTING ||
                session->GetState() == MuSig2SessionState::NONCES_COMPLETE);
}

BOOST_AUTO_TEST_CASE(remote_future_attempt_messages_do_not_preempt_lazy_session)
{
    OracleSigningOrchestrator orch;
    const int32_t epoch = 45;
    const uint8_t oracle_id = 1;
    const uint8_t future_attempt = 7;

    OracleMusigNonceMsg nonce_msg = MakeSignedMusigNonceMsg(epoch, oracle_id);
    nonce_msg.attempt_id = future_attempt;
    BOOST_REQUIRE(nonce_msg.Sign(GetRegtestMusigOracleKey(oracle_id)));
    orch.IngestRemoteNonce(nonce_msg);
    BOOST_CHECK_MESSAGE(!orch.HasSession(epoch),
        "a signed remote nonce must not choose a future attempt before the local epoch starts");

    OracleMusigPartialSigMsg partial_msg =
        MakeSignedGarbagePartialSigMsg(epoch, oracle_id, 0xA11CE);
    partial_msg.attempt_id = future_attempt;
    partial_msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
    partial_msg.session_context_id = uint256S("0x123");
    BOOST_REQUIRE(partial_msg.Sign(GetRegtestMusigOracleKey(oracle_id)));
    orch.IngestRemotePartialSig(partial_msg);
    BOOST_CHECK_MESSAGE(!orch.HasSession(epoch),
        "a signed remote partial sig must not choose a future attempt before the local epoch starts");

    OracleMusigContextMsg context_msg;
    context_msg.epoch = epoch;
    context_msg.attempt_id = future_attempt;
    context_msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
    context_msg.epoch_selection_seed = Params().GetConsensus().hashGenesisBlock;
    context_msg.proposer_id = oracle_id;
    context_msg.participant_ids = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    context_msg.nonce_set_hash = uint256S("0x456");
    context_msg.quote_set_hash = uint256S("0x789");
    context_msg.consensus_price = 123456789;
    context_msg.consensus_timestamp = 1710000000;
    context_msg.session_context_id = uint256S("0xabc");
    BOOST_REQUIRE(context_msg.Sign(GetRegtestMusigOracleKey(oracle_id)));
    BOOST_REQUIRE(context_msg.IsValid());

    orch.IngestRemoteContext(context_msg);
    BOOST_CHECK_MESSAGE(!orch.HasSession(epoch),
        "a signed remote context must not choose a future attempt before the local epoch starts");
}

BOOST_AUTO_TEST_CASE(remote_context_proposals_are_bounded_per_epoch)
{
    OracleSigningOrchestrator orch;
    const int32_t epoch = 46;
    const uint8_t oracle_id = 1;
    const CKey oracle_key = GetRegtestMusigOracleKey(oracle_id);
    const XOnlyPubKey oracle_pubkey(oracle_key.GetPubKey());

    for (size_t i = 0; i < 200; ++i) {
        OracleMusigContextMsg context_msg;
        context_msg.epoch = epoch;
        context_msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
        context_msg.epoch_selection_seed = Params().GetConsensus().hashGenesisBlock;
        context_msg.proposer_id = oracle_id;
        context_msg.participant_ids = {oracle_id};
        context_msg.nonce_set_hash = UniqueMusigTestHash(i + 1);
        context_msg.quote_set_hash = UniqueMusigTestHash(i + 1000);
        context_msg.consensus_price = 100000000 + i;
        context_msg.consensus_timestamp = 1710000000 + i;
        context_msg.session_context_id = UniqueMusigTestHash(i + 2000);
        BOOST_REQUIRE(context_msg.Sign(oracle_key));
        BOOST_REQUIRE(context_msg.VerifySignature(oracle_pubkey));
        BOOST_REQUIRE(context_msg.IsValid());

        orch.IngestRemoteContext(context_msg);
    }

    BOOST_CHECK_LE(orch.GetPendingContextProposalCountForTesting(epoch), 1U);
}

BOOST_AUTO_TEST_CASE(remote_nonce_lazy_session_timeout_uses_chain_epoch_length)
{
    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    BOOST_REQUIRE_GT(epoch_length, 0);
    BOOST_REQUIRE_NE(epoch_length, 50);

    OracleSigningOrchestrator orch;
    const int32_t epoch = 3;
    const int32_t epoch_start_height = epoch * epoch_length;

    OracleMusigNonceMsg msg = MakeSignedMusigNonceMsg(epoch, 1);
    orch.IngestRemoteNonce(msg);

    MuSig2SigningSession* session = orch.GetOrCreateSigningSession(epoch, epoch_start_height);
    BOOST_REQUIRE(session != nullptr);
    BOOST_REQUIRE_EQUAL(session->GetNonceCount(), 1U);

    std::shared_ptr<const CBlock> empty_block;
    orch.OnBlockConnected(empty_block, epoch_start_height);

    session = orch.GetOrCreateSigningSession(epoch, epoch_start_height);
    BOOST_REQUIRE(session != nullptr);
    BOOST_CHECK_MESSAGE(session->GetState() != MuSig2SessionState::FAILED,
        "lazy-created MuSig2 session must use the active chain epoch length for timeout binding");
}

BOOST_AUTO_TEST_CASE(early_partial_sigs_replay_when_session_enters_signing)
{
    OracleSigningOrchestrator orch;

    const int32_t epoch = 43;
    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    BOOST_REQUIRE_GT(epoch_length, 0);
    const uint8_t threshold = static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired);
    const uint64_t price = 123456789;
    const int64_t timestamp = 1710000000;

    auto receiving_session = std::make_unique<MuSig2SigningSession>(epoch, threshold);
    MuSig2SigningSession* receiving_ptr = receiving_session.get();
    std::vector<OracleMusigPartialSigMsg> partials =
        MakeThresholdPartialSigMessages(epoch, price, timestamp, *receiving_session);

    BOOST_REQUIRE_EQUAL(receiving_ptr->GetState(), MuSig2SessionState::NONCES_COMPLETE);
    orch.InjectSession(epoch, std::move(receiving_session));

    for (const OracleMusigPartialSigMsg& msg : partials) {
        orch.IngestRemotePartialSig(msg);
    }

    BOOST_REQUIRE_EQUAL(receiving_ptr->GetPartialSigCount(), 0U);

    unsigned char msg32[32];
    OracleSigningOrchestrator::ComputeOracleMessageHash(epoch, price, timestamp, msg32);
    BOOST_REQUIRE(receiving_ptr->AggregateNonces(msg32));
    BOOST_REQUIRE_EQUAL(receiving_ptr->GetState(), MuSig2SessionState::SIGNING);

    std::shared_ptr<const CBlock> empty_block;
    orch.OnBlockConnected(empty_block, epoch * epoch_length);

    BOOST_CHECK_EQUAL(receiving_ptr->GetPartialSigCount(), partials.size());

    std::vector<unsigned char> aggregate_sig;
    std::vector<unsigned char> participation_bitmap;
    uint64_t signed_price = 0;
    int64_t signed_timestamp = 0;
    BOOST_CHECK(orch.GetCompletedSession(epoch,
                                         aggregate_sig,
                                         participation_bitmap,
                                         signed_price,
                                         signed_timestamp));
    BOOST_CHECK_EQUAL(signed_price, price);
    BOOST_CHECK_EQUAL(signed_timestamp, timestamp);
}

BOOST_AUTO_TEST_CASE(passive_non_oracle_template_node_selects_remote_context_and_completes)
{
    OracleSigningOrchestrator orch;

    const int32_t epoch = 47;
    PassiveMuSig2Transcript transcript = MakePassiveThresholdTranscript(epoch);

    for (const OracleMusigNonceMsg& msg : transcript.nonce_msgs) {
        orch.IngestRemoteNonce(msg);
    }

    MuSig2SigningSession* session = orch.GetOrCreateSigningSession(epoch, transcript.tick_height);
    BOOST_REQUIRE(session != nullptr);
    BOOST_REQUIRE_EQUAL(session->GetState(), MuSig2SessionState::NONCES_COMPLETE);
    BOOST_REQUIRE_EQUAL(session->GetPartialSigCount(), 0U);

    orch.IngestRemoteContext(transcript.context_msg);
    BOOST_REQUIRE_EQUAL(orch.GetPendingContextProposalCountForTesting(epoch), 1U);

    for (const OracleMusigPartialSigMsg& msg : transcript.partial_sig_msgs) {
        orch.IngestRemotePartialSig(msg);
    }
    BOOST_REQUIRE_EQUAL(session->GetState(), MuSig2SessionState::NONCES_COMPLETE);
    BOOST_REQUIRE_EQUAL(session->GetPartialSigCount(), 0U);

    std::shared_ptr<const CBlock> empty_block;
    orch.OnBlockConnected(empty_block, transcript.tick_height);

    BOOST_CHECK_EQUAL(session->GetState(), MuSig2SessionState::COMPLETE);

    std::vector<unsigned char> aggregate_sig;
    std::vector<unsigned char> participation_bitmap;
    uint64_t signed_price = 0;
    int64_t signed_timestamp = 0;
    BOOST_REQUIRE(orch.GetCompletedSession(epoch,
                                           aggregate_sig,
                                           participation_bitmap,
                                           signed_price,
                                           signed_timestamp));
    BOOST_CHECK_EQUAL(aggregate_sig.size(), 64U);
    BOOST_CHECK(!participation_bitmap.empty());
    BOOST_CHECK_EQUAL(signed_price, transcript.price);
    BOOST_CHECK_EQUAL(signed_timestamp, transcript.timestamp);
}

BOOST_AUTO_TEST_CASE(early_partial_buffer_dedups_by_oracle_before_capacity)
{
    OracleSigningOrchestrator orch;

    const int32_t epoch = 44;
    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    BOOST_REQUIRE_GT(epoch_length, 0);
    const uint8_t threshold = static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired);
    const uint64_t price = 123456789;
    const int64_t timestamp = 1710000000;

    auto receiving_session = std::make_unique<MuSig2SigningSession>(epoch, threshold);
    MuSig2SigningSession* receiving_ptr = receiving_session.get();
    std::vector<OracleMusigPartialSigMsg> honest_partials =
        MakeThresholdPartialSigMessages(epoch, price, timestamp, *receiving_session);

    BOOST_REQUIRE_EQUAL(receiving_ptr->GetState(), MuSig2SessionState::NONCES_COMPLETE);
    orch.InjectSession(epoch, std::move(receiving_session));

    const uint8_t attacker_id = static_cast<uint8_t>(Params().GetConsensus().nOracleTotalOracles - 1);
    for (size_t i = 0; i < 32; ++i) {
        orch.IngestRemotePartialSig(MakeSignedGarbagePartialSigMsg(epoch, attacker_id, i + 1));
    }

    for (const OracleMusigPartialSigMsg& msg : honest_partials) {
        orch.IngestRemotePartialSig(msg);
    }

    BOOST_REQUIRE_EQUAL(receiving_ptr->GetPartialSigCount(), 0U);

    unsigned char msg32[32];
    OracleSigningOrchestrator::ComputeOracleMessageHash(epoch, price, timestamp, msg32);
    BOOST_REQUIRE(receiving_ptr->AggregateNonces(msg32));
    BOOST_REQUIRE_EQUAL(receiving_ptr->GetState(), MuSig2SessionState::SIGNING);

    std::shared_ptr<const CBlock> empty_block;
    orch.OnBlockConnected(empty_block, epoch * epoch_length);

    BOOST_CHECK_EQUAL(receiving_ptr->GetPartialSigCount(), honest_partials.size());
}

// Wave 10 (Agent C) — operator status visibility: the orchestrator's
// `m_signing_sessions` map is a private member. Operators have no
// programmatic way to see whether a session for the current epoch is
// COMPLETE / SIGNING / FAILED / etc. without reading debug logs. This
// test pins the new public accessor `GetSessionStateForEpoch` that the
// RPC surface (`getdigidollardeploymentinfo`) uses to expose the
// current session state, nonce count, and partial-sig count.
BOOST_AUTO_TEST_CASE(get_session_state_for_epoch_reports_in_progress_then_failed)
{
    OracleSigningOrchestrator orch;
    const int32_t epoch = 200;

    // Before any session exists the accessor returns std::nullopt.
    BOOST_CHECK(!orch.GetSessionStateForEpoch(epoch).has_value());

    // Create the session at epoch_start_height; CREATED state is reported.
    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    BOOST_REQUIRE_GT(epoch_length, 0);
    const int32_t creation_height = epoch * epoch_length;
    MuSig2SigningSession* session = orch.GetOrCreateSigningSession(epoch, creation_height);
    BOOST_REQUIRE(session != nullptr);

    auto info = orch.GetSessionStateForEpoch(epoch);
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(static_cast<int>(info->state), static_cast<int>(MuSig2SessionState::CREATED));
    BOOST_CHECK_EQUAL(info->nonce_count, 0U);
    BOOST_CHECK_EQUAL(info->partial_sig_count, 0U);
    BOOST_CHECK_EQUAL(info->creation_height, creation_height);

    // Drive the session to FAILED via timeout: trigger CheckTimeout at a
    // height beyond creation_height + timeout_blocks (default 100).
    session->CheckTimeout(creation_height + 200);

    info = orch.GetSessionStateForEpoch(epoch);
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(static_cast<int>(info->state), static_cast<int>(MuSig2SessionState::FAILED));
}

// Wave 10 (Agent C) — restart liveness: after `Clear()` (simulated
// daemon restart) any existing session state is dropped, and the
// orchestrator must be able to lazily create fresh sessions for the
// current epoch on the next remote nonce arrival or block tick. The
// session map is in-memory only (no on-disk persistence): secp256k1
// secnonces are non-copyable and must never be persisted. After
// Clear(), HasSession() is false for every previously-known epoch and
// the session is not silently resurrected.
BOOST_AUTO_TEST_CASE(clear_drops_all_sessions_simulating_restart)
{
    OracleSigningOrchestrator orch;
    const int32_t epoch_a = 300;
    const int32_t epoch_b = 301;
    const int32_t epoch_length = Params().GetConsensus().nDDOracleEpochBlocks;
    BOOST_REQUIRE_GT(epoch_length, 0);

    // Pre-populate two sessions (current + next via pre-start window).
    BOOST_REQUIRE(orch.GetOrCreateSigningSession(epoch_a, epoch_a * epoch_length) != nullptr);
    BOOST_REQUIRE(orch.GetOrCreateSigningSession(epoch_b, epoch_b * epoch_length) != nullptr);
    BOOST_REQUIRE(orch.HasSession(epoch_a));
    BOOST_REQUIRE(orch.HasSession(epoch_b));

    // Simulate restart: Clear() wipes session map, broadcast trackers,
    // pending partial-sig buffer, cached oracle key.
    orch.Clear();

    // Both sessions are gone — the new (post-restart) orchestrator state
    // is empty, not silently resurrected from a global map.
    BOOST_CHECK(!orch.HasSession(epoch_a));
    BOOST_CHECK(!orch.HasSession(epoch_b));
    BOOST_CHECK(!orch.GetSessionStateForEpoch(epoch_a).has_value());
    BOOST_CHECK(!orch.GetSessionStateForEpoch(epoch_b).has_value());

    // Lazy session creation still works on the next remote nonce.
    OracleMusigNonceMsg msg = MakeSignedMusigNonceMsg(epoch_a, /*oracle_id=*/1);
    orch.IngestRemoteNonce(msg);
    BOOST_CHECK(orch.HasSession(epoch_a));

    auto info = orch.GetSessionStateForEpoch(epoch_a);
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK(info->state == MuSig2SessionState::CREATED ||
                info->state == MuSig2SessionState::NONCES_COLLECTING ||
                info->state == MuSig2SessionState::NONCES_COMPLETE);
}

// Wave 10 (Agent C) — sub-quorum graceful handling: if fewer than the
// configured threshold of oracle peers ever respond with a nonce, the
// session can never reach NONCES_COMPLETE → SIGNING → COMPLETE. The
// miner queries `GetCompletedSession` and must return false (not crash,
// not return a half-built bundle), and the bundle manager strips DD
// txs from the block template via `AddOracleBundleToBlock` returning
// false for DD-touching blocks. This test pins the
// `GetCompletedSession` contract for an under-quorum session.
BOOST_AUTO_TEST_CASE(sub_quorum_session_does_not_complete)
{
    OracleSigningOrchestrator orch;
    const int32_t epoch = 400;
    const uint8_t threshold = static_cast<uint8_t>(Params().GetConsensus().nOracleConsensusRequired);
    BOOST_REQUIRE_GT(threshold, 1);

    // Inject a session and add only (threshold - 1) nonces — sub-quorum.
    auto receiving_session = std::make_unique<MuSig2SigningSession>(epoch, threshold);

    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey full_agg_pk;
    secp256k1_musig_keyagg_cache full_keyagg_cache;
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(GetActiveOracleIdsForMusigTest(),
                                                    full_agg_pk,
                                                    full_keyagg_cache));
    BOOST_REQUIRE(receiving_session->InitializePassive(full_keyagg_cache));

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    BOOST_REQUIRE(ctx);

    const std::vector<uint8_t> all_ids = GetActiveOracleIdsForMusigTest();
    BOOST_REQUIRE_GE(all_ids.size(), threshold);
    const size_t n_below_quorum = static_cast<size_t>(threshold) - 1;
    for (size_t i = 0; i < n_below_quorum; ++i) {
        const uint8_t oracle_id = all_ids[i];
        CKey key = GetRegtestMusigOracleKey(oracle_id);
        CPubKey pubkey = key.GetPubKey();
        secp256k1_pubkey secp_pubkey;
        BOOST_REQUIRE(secp256k1_ec_pubkey_parse(ctx, &secp_pubkey, pubkey.data(), pubkey.size()));
        secp256k1_musig_pubnonce pubnonce;
        // Use a temp signing session to generate a valid pubnonce; we
        // only need the pubnonce bytes, not the secnonce.
        MuSig2SigningSession tmp(epoch, threshold);
        BOOST_REQUIRE(tmp.GenerateNonce(oracle_id, key, secp_pubkey, full_keyagg_cache, pubnonce));
        BOOST_REQUIRE(receiving_session->AddPubnonce(oracle_id, pubnonce));
    }
    secp256k1_context_destroy(ctx);

    BOOST_CHECK_EQUAL(receiving_session->GetNonceCount(), n_below_quorum);
    BOOST_CHECK(!receiving_session->HasEnoughNonces());
    BOOST_CHECK_EQUAL(static_cast<int>(receiving_session->GetState()),
                      static_cast<int>(MuSig2SessionState::NONCES_COLLECTING));

    orch.InjectSession(epoch, std::move(receiving_session));

    // Miner-path query: GetCompletedSession must report false for an
    // under-quorum session. The miner does not crash; it falls through
    // to "no MuSig2 bundle ready" and strips DD txs (or omits the
    // oracle output for non-DD blocks).
    std::vector<unsigned char> sig, bitmap;
    uint64_t price = 0;
    int64_t ts = 0;
    BOOST_CHECK(!orch.GetCompletedSession(epoch, sig, bitmap, price, ts));

    // Operator-status query reports the under-quorum state with exact
    // counts so an operator can diagnose the stuck session.
    auto info = orch.GetSessionStateForEpoch(epoch);
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(static_cast<int>(info->state),
                      static_cast<int>(MuSig2SessionState::NONCES_COLLECTING));
    BOOST_CHECK_EQUAL(info->nonce_count, n_below_quorum);
    BOOST_CHECK_EQUAL(info->partial_sig_count, 0U);
}

BOOST_AUTO_TEST_CASE(remote_context_proposal_does_not_define_epoch_seed)
{
    OracleSigningOrchestrator orch;
    const int32_t epoch = 401;
    const uint256 remote_seed = uint256(123);
    BOOST_REQUIRE(remote_seed != Params().GetConsensus().hashGenesisBlock);

    OracleMusigContextMsg msg;
    msg.epoch = epoch;
    msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
    msg.epoch_selection_seed = remote_seed;
    msg.proposer_id = 1;
    msg.participant_ids = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    msg.consensus_price = 123456789;
    msg.consensus_timestamp = 1710000000;
    msg.session_context_id = uint256S("0x456");
    msg.signature.assign(64, 0xAA);
    BOOST_REQUIRE(msg.IsValid());

    orch.IngestRemoteContext(msg);
    BOOST_REQUIRE(orch.HasSession(epoch));

    MuSig2SigningSession* session = orch.GetOrCreateSigningSession(epoch, epoch * Params().GetConsensus().nDDOracleEpochBlocks);
    BOOST_REQUIRE(session != nullptr);
    BOOST_CHECK(session->GetEpochSelectionSeed() == Params().GetConsensus().hashGenesisBlock);
    BOOST_CHECK(session->GetEpochSelectionSeed() != remote_seed);
}

// Message types waiting for a test peer: the one already handed to the
// transport plus everything still in the send queue.
static std::vector<std::string> QueuedMessageTypes(CNode& node)
{
    std::vector<std::string> types;
    LOCK(node.cs_vSend);
    const auto& [to_send, _more, msg_type] = node.m_transport->GetBytesToSend(false);
    if (!to_send.empty()) types.push_back(msg_type);
    for (const auto& msg : node.vSendMsg) types.push_back(msg.m_type);
    return types;
}

// A nonce that reached nobody must not be reported as sent.
//
// BroadcastMusigNonce answers with the number of peers it handed the message
// to. Reaching nobody has two causes and they mean the same thing to the
// caller: there is no connection manager at all, or there is one and no peer
// is connected yet. The second happens on a real node at startup, where a
// wallet can start a local oracle before the connection manager has taken any
// peers. The old code returned true for it, the caller wrote the nonce down as
// sent, and because a session refuses to make a second nonce for the same
// oracle, that oracle then had no nonce in the epoch at all.
BOOST_AUTO_TEST_CASE(nonce_broadcast_reports_how_many_peers_it_reached)
{
    OracleSigningOrchestrator orch;
    const OracleMusigNonceMsg msg = MakeSignedMusigNonceMsg(/*epoch=*/7, /*oracle_id=*/0);

    // A send that failed: nothing to send through.
    orch.SetConnman(nullptr);
    BOOST_CHECK_EQUAL(orch.BroadcastMusigNonce(msg), 0U);

    // A connection manager with no peers reaches nobody, same as above.
    auto connman = std::make_unique<ConnmanTestMsg>(0x1337, 0x1337, *m_node.addrman,
                                                    *m_node.netgroupman, Params());
    orch.SetConnman(connman.get());
    BOOST_CHECK_EQUAL(orch.BroadcastMusigNonce(msg), 0U);

    // The same nonce, once a peer is connected, goes to that peer. The
    // connection manager deletes this node in ClearTestNodes() below.
    CNode* peer = new CNode(/*id=*/0,
                            /*sock=*/nullptr,
                            CAddress(),
                            /*nKeyedNetGroupIn=*/0,
                            /*nLocalHostNonceIn=*/0,
                            CAddress(),
                            /*pszDest=*/std::string{},
                            ConnectionType::INBOUND,
                            /*inbound_onion=*/false);
    peer->fSuccessfullyConnected = true;
    connman->AddTestNode(*peer);
    BOOST_CHECK_EQUAL(orch.BroadcastMusigNonce(msg), 1U);

    // And it is a real nonce message waiting for that peer, not just a count.
    const std::vector<std::string> queued = QueuedMessageTypes(*peer);
    BOOST_CHECK(std::find(queued.begin(), queued.end(),
                          std::string(NetMsgType::ORACLEMUSIGNONCE)) != queued.end());

    orch.SetConnman(nullptr);
    connman->ClearTestNodes();
}

class SigningRetrySetup
{
    std::array<CBlockIndex, 16> m_indexes;
    const uint256 m_seed{Params().GetConsensus().hashGenesisBlock};

public:
    OracleSigningOrchestrator orchestrator;
    ConnmanTestMsg connman;
    std::vector<uint8_t> oracle_ids;
    CNode* peer{nullptr};

    SigningRetrySetup(AddrMan& addrman, const NetGroupManager& netgroupman)
        : connman(0x1337, 0x1337, addrman, netgroupman, Params())
    {
        OracleManager::StopOracleService();
        OracleBundleManager::Initialize();
        OracleBundleManager::GetInstance().Clear();
        OracleBundleManager::GetInstance().SetConnman(nullptr);
        // Synthetic indexes provide ancestry to the real scheduled callback.
        for (size_t i = 0; i < m_indexes.size(); ++i) {
            m_indexes[i].nHeight = i;
            m_indexes[i].phashBlock = &m_seed;
            if (i > 0) m_indexes[i].pprev = &m_indexes[i - 1];
            m_indexes[i].BuildSkip();
        }
    }

    ~SigningRetrySetup()
    {
        orchestrator.Stop();
        SyncWithValidationInterfaceQueue();
        OracleManager::StopOracleService();
        orchestrator.SetConnman(nullptr);
        connman.ClearTestNodes();
        OracleBundleManager::GetInstance().Clear();
    }

    void StartSigners()
    {
        oracle_ids = SortMusigTestIdsBySeed(GetActiveOracleIdsForMusigTest(), 0, m_seed);
        oracle_ids.resize(Params().GetConsensus().nOracleConsensusRequired);
        auto& manager = OracleManager::GetInstance();
        for (uint8_t id : oracle_ids) {
            const CKey key = GetRegtestMusigOracleKey(id);
            BOOST_REQUIRE(manager.AddOracleNode(id, HexStr(Span{key.begin(), key.size()})));
            OracleNode* oracle = manager.GetOracleNode(id);
            BOOST_REQUIRE(oracle);
            oracle->SetSkipExchangeFetchForTesting(true);
            oracle->Start();
            BOOST_REQUIRE(oracle->IsRunning());
            BOOST_REQUIRE(OracleBundleManager::GetInstance().AddOracleMessage(
                MakeSignedMusigPriceEvidence(id, 6000, GetTime())));
        }
        orchestrator.Start();
    }

    void Tick(int height)
    {
        BOOST_REQUIRE_LT(height, static_cast<int>(m_indexes.size()));
        GetMainSignals().BlockConnected(ChainstateRole::NORMAL,
                                       std::make_shared<const CBlock>(), &m_indexes[height]);
        SyncWithValidationInterfaceQueue();
    }

    void ConnectPeer()
    {
        orchestrator.SetConnman(&connman);
        peer = new CNode(0, nullptr, CAddress(), 0, 0, CAddress(), {},
                         ConnectionType::INBOUND, false);
        peer->fSuccessfullyConnected = true;
        connman.AddTestNode(*peer);
    }

    std::vector<CSerializedNetMsg> TakeMessages()
    {
        std::vector<CSerializedNetMsg> result;
        V1Transport receiver{peer->GetId(), SER_NETWORK, PROTOCOL_VERSION};
        {
            LOCK(peer->cs_vSend);
            // PushMessage may have handed the first message to the transport.
            // Read its actual bytes, then copy messages still in the send queue.
            while (true) {
                const auto& [bytes, more, type] = peer->m_transport->GetBytesToSend(false);
                if (bytes.empty()) break;
                auto remaining = bytes;
                while (!remaining.empty()) BOOST_REQUIRE(receiver.ReceivedBytes(remaining));
                peer->m_transport->MarkBytesSent(bytes.size());
                if (receiver.ReceivedMessageComplete()) {
                    bool rejected{false};
                    auto received = receiver.GetReceivedMessage(std::chrono::microseconds{0}, rejected);
                    BOOST_REQUIRE(!rejected);
                    CSerializedNetMsg message;
                    message.m_type = received.m_type;
                    const auto payload = MakeUCharSpan(received.m_recv);
                    message.data.assign(payload.begin(), payload.end());
                    result.push_back(std::move(message));
                }
            }
            for (const auto& message : peer->vSendMsg) result.push_back(message.Copy());
        }
        connman.FlushSendBuffer(*peer);
        return result;
    }

    void CheckCompletedBundle()
    {
        COracleBundle bundle{0};
        BOOST_REQUIRE(orchestrator.GetCompletedSession(0, bundle.aggregate_sig,
            bundle.participation_bitmap, bundle.median_price_micro_usd, bundle.timestamp));
        std::string error;
        BOOST_CHECK_MESSAGE(OracleBundleManager::ValidateMuSig2Bundle(
            bundle, 10, Params().GetConsensus(), error), error);
    }
};

BOOST_AUTO_TEST_CASE(scheduled_signing_retries_after_peer_connects)
{
    SigningRetrySetup setup{*m_node.addrman, *m_node.netgroupman};
    setup.StartSigners();
    setup.Tick(0); // Nonces are held with no connection manager.
    setup.orchestrator.SetConnman(&setup.connman);
    setup.Tick(2); // Context is held with a connection manager but no peers.
    setup.Tick(3); // Partial signatures are held after their secret nonces are consumed.
    auto* session = setup.orchestrator.GetOrCreateSigningSession(0);
    BOOST_REQUIRE_EQUAL(session->GetState(), MuSig2SessionState::COMPLETE);
    const uint256 context_id = session->GetSessionContextId();
    const uint256 nonce_set_hash = session->GetNonceSetHash();
    setup.CheckCompletedBundle();
    OracleManager::StopOracleService();
    OracleSigningOrchestrator receiver;
    BOOST_REQUIRE(!receiver.IsOracleNode());

    setup.ConnectPeer();
    setup.Tick(4);
    std::map<uint8_t, uint256> delivered_nonces;
    std::vector<OracleMusigContextMsg> contexts;
    std::set<uint8_t> partial_signers;
    for (const auto& message : setup.TakeMessages()) {
        CDataStream stream{message.data, SER_NETWORK, PROTOCOL_VERSION};
        if (message.m_type == NetMsgType::ORACLEMUSIGNONCE) {
            OracleMusigNonceMsg nonce;
            stream >> nonce;
            BOOST_CHECK_EQUAL(nonce.attempt_id, 0);
            BOOST_CHECK(nonce.VerifySignature(XOnlyPubKey(GetRegtestMusigOracleKey(nonce.oracle_id).GetPubKey())));
            BOOST_CHECK(delivered_nonces.emplace(nonce.oracle_id, nonce.GetHash()).second);
            receiver.IngestRemoteNonce(nonce);
        } else if (message.m_type == NetMsgType::ORACLEMUSIGCONTEXT) {
            OracleMusigContextMsg context;
            stream >> context;
            BOOST_CHECK_EQUAL(context.attempt_id, 0);
            BOOST_CHECK(context.session_context_id == context_id);
            BOOST_CHECK(context.nonce_set_hash == nonce_set_hash);
            BOOST_CHECK(context.VerifySignature(XOnlyPubKey(GetRegtestMusigOracleKey(context.proposer_id).GetPubKey())));
            contexts.push_back(context);
            receiver.IngestRemoteContext(context);
        } else if (message.m_type == NetMsgType::ORACLEMUSIGPARTIALSIG) {
            OracleMusigPartialSigMsg partial;
            stream >> partial;
            BOOST_CHECK_EQUAL(partial.attempt_id, 0);
            BOOST_CHECK(partial.session_context_id == context_id);
            BOOST_CHECK(partial.VerifySignature(XOnlyPubKey(GetRegtestMusigOracleKey(partial.oracle_id).GetPubKey())));
            BOOST_CHECK(partial_signers.insert(partial.oracle_id).second);
            receiver.IngestRemotePartialSig(partial);
        } else {
            BOOST_ERROR("unexpected queued message: " << message.m_type);
        }
    }
    BOOST_CHECK_EQUAL(delivered_nonces.size(), setup.oracle_ids.size());
    BOOST_CHECK_EQUAL(partial_signers.size(), setup.oracle_ids.size());
    BOOST_REQUIRE_EQUAL(contexts.size(), 1U);
    for (const auto& nonce : contexts.front().nonce_evidence) {
        BOOST_REQUIRE(delivered_nonces.count(nonce.oracle_id));
        BOOST_CHECK(delivered_nonces.at(nonce.oracle_id) == nonce.GetHash());
    }
    receiver.OnBlockConnected(std::make_shared<const CBlock>(), 4);
    COracleBundle received_bundle{0};
    BOOST_REQUIRE(receiver.GetCompletedSession(0, received_bundle.aggregate_sig,
        received_bundle.participation_bitmap, received_bundle.median_price_micro_usd,
        received_bundle.timestamp));
    std::string error;
    BOOST_CHECK_MESSAGE(OracleBundleManager::ValidateMuSig2Bundle(
        received_bundle, 4, Params().GetConsensus(), error), error);
    setup.Tick(5);
    BOOST_CHECK(setup.TakeMessages().empty());
    BOOST_CHECK(session->GetSessionContextId() == context_id);
    setup.CheckCompletedBundle();
}

BOOST_AUTO_TEST_CASE(scheduled_signing_replacement_discards_old_retries)
{
    SigningRetrySetup setup{*m_node.addrman, *m_node.netgroupman};
    setup.StartSigners();
    setup.orchestrator.SetConnman(&setup.connman);
    setup.Tick(0);
    // Keep one selected signer offline after its nonce, leaving the other
    // signers' context and partial signatures waiting for delivery.
    OracleManager::GetInstance().EnableOracle(setup.oracle_ids.back(), false);
    setup.Tick(2);
    setup.Tick(3);
    auto* session = setup.orchestrator.GetOrCreateSigningSession(0);
    BOOST_REQUIRE_EQUAL(session->GetState(), MuSig2SessionState::SIGNING);
    BOOST_REQUIRE_EQUAL(session->GetPartialSigCount(), setup.oracle_ids.size() - 1);
    const uint256 old_context = session->GetSessionContextId();
    secp256k1_musig_partial_sig duplicate;
    BOOST_CHECK(!session->CreatePartialSignature(setup.oracle_ids.front(),
        GetRegtestMusigOracleKey(setup.oracle_ids.front()), duplicate));
    session->SetTimeoutBlocks(4);
    setup.Tick(4);
    BOOST_REQUIRE_EQUAL(session->GetState(), MuSig2SessionState::FAILED);

    setup.ConnectPeer();
    setup.Tick(5);
    BOOST_REQUIRE_EQUAL(setup.orchestrator.GetActiveAttemptId(0), 1);
    const auto messages = setup.TakeMessages();
    BOOST_REQUIRE_EQUAL(messages.size(), setup.oracle_ids.size() - 1);
    for (const auto& message : messages) {
        BOOST_REQUIRE_EQUAL(message.m_type, NetMsgType::ORACLEMUSIGNONCE);
        CDataStream stream{message.data, SER_NETWORK, PROTOCOL_VERSION};
        OracleMusigNonceMsg nonce;
        stream >> nonce;
        BOOST_CHECK_EQUAL(nonce.attempt_id, 1);
        BOOST_CHECK(nonce.VerifySignature(XOnlyPubKey(GetRegtestMusigOracleKey(nonce.oracle_id).GetPubKey())));
    }

    OracleManager::GetInstance().EnableOracle(setup.oracle_ids.back(), true);
    setup.Tick(6);
    setup.Tick(7);
    setup.Tick(8);
    session = setup.orchestrator.GetOrCreateSigningSession(0);
    BOOST_CHECK_EQUAL(session->GetState(), MuSig2SessionState::COMPLETE);
    BOOST_CHECK(session->GetSessionContextId() != old_context);
    setup.CheckCompletedBundle();
    size_t nonce_count{0}, context_count{0}, partial_count{0};
    for (const auto& message : setup.TakeMessages()) {
        CDataStream stream{message.data, SER_NETWORK, PROTOCOL_VERSION};
        if (message.m_type == NetMsgType::ORACLEMUSIGNONCE) {
            OracleMusigNonceMsg nonce;
            stream >> nonce;
            BOOST_CHECK_EQUAL(nonce.attempt_id, 1);
            ++nonce_count;
        } else if (message.m_type == NetMsgType::ORACLEMUSIGCONTEXT) {
            OracleMusigContextMsg context;
            stream >> context;
            BOOST_CHECK_EQUAL(context.attempt_id, 1);
            BOOST_CHECK(context.session_context_id == session->GetSessionContextId());
            ++context_count;
        } else if (message.m_type == NetMsgType::ORACLEMUSIGPARTIALSIG) {
            OracleMusigPartialSigMsg partial;
            stream >> partial;
            BOOST_CHECK_EQUAL(partial.attempt_id, 1);
            BOOST_CHECK(partial.session_context_id == session->GetSessionContextId());
            ++partial_count;
        } else {
            BOOST_ERROR("unexpected queued message: " << message.m_type);
        }
    }
    BOOST_CHECK_EQUAL(nonce_count, 1U);
    BOOST_CHECK_EQUAL(context_count, 1U);
    BOOST_CHECK_EQUAL(partial_count, setup.oracle_ids.size());
}

BOOST_AUTO_TEST_CASE(scheduled_signing_retries_after_peer_reconnects)
{
    SigningRetrySetup setup{*m_node.addrman, *m_node.netgroupman};
    setup.StartSigners();
    setup.ConnectPeer();
    setup.Tick(0);
    const auto nonces = setup.TakeMessages();
    BOOST_REQUIRE_EQUAL(nonces.size(), setup.oracle_ids.size());
    for (const auto& message : nonces) {
        BOOST_CHECK_EQUAL(message.m_type, NetMsgType::ORACLEMUSIGNONCE);
    }
    setup.connman.ClearTestNodes();
    setup.peer = nullptr;
    setup.Tick(2);
    setup.Tick(3);
    setup.CheckCompletedBundle();

    setup.ConnectPeer();
    setup.Tick(4);
    const auto retries = setup.TakeMessages();
    BOOST_REQUIRE_EQUAL(retries.size(), setup.oracle_ids.size() + 1);
    size_t contexts{0}, partials{0};
    for (const auto& message : retries) {
        if (message.m_type == NetMsgType::ORACLEMUSIGCONTEXT) ++contexts;
        else if (message.m_type == NetMsgType::ORACLEMUSIGPARTIALSIG) ++partials;
        else BOOST_ERROR("unexpected retry after reconnect: " << message.m_type);
    }
    BOOST_CHECK_EQUAL(contexts, 1U);
    BOOST_CHECK_EQUAL(partials, setup.oracle_ids.size());
    setup.Tick(5);
    BOOST_CHECK(setup.TakeMessages().empty());
}

BOOST_AUTO_TEST_SUITE_END()

struct MainParamsMuSig2Setup : public BasicTestingSetup {
    MainParamsMuSig2Setup() : BasicTestingSetup(ChainType::MAIN) {}
};

BOOST_FIXTURE_TEST_SUITE(musig2_signing_orchestration_mainnet_tests, MainParamsMuSig2Setup)

BOOST_AUTO_TEST_CASE(mainnet_signing_roster_covers_full_35_slot_keyset)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(consensus.nOraclePubkeyCount, 35);
    BOOST_REQUIRE_EQUAL(Params().GetOracleNodes().size(),
                        static_cast<size_t>(consensus.nOraclePubkeyCount));

    const std::vector<uint8_t> signing_ids =
        OracleSigningOrchestrator::GetConsensusOracleIdsForSigning();
    BOOST_CHECK_EQUAL(signing_ids.size(),
                      static_cast<size_t>(consensus.nOraclePubkeyCount));
    for (uint8_t oracle_id = 0; oracle_id < consensus.nOraclePubkeyCount; ++oracle_id) {
        BOOST_CHECK_EQUAL(signing_ids[oracle_id], oracle_id);
    }

    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;
    BOOST_CHECK_MESSAGE(aggregator.ComputeAggregatePubkey(signing_ids, agg_pk, cache),
                        "mainnet signing roster must aggregate every RC44 slot");
}

BOOST_AUTO_TEST_SUITE_END()
