// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <headerssync.h>
#include <pow.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <util/chaintype.h>
#include <deque>
#include <limits>
#include <vector>

#include <boost/test/unit_test.hpp>

struct HeadersGeneratorSetup : public RegTestingSetup {
    /** Search for a nonce to meet (regtest) proof of work */
    void FindProofOfWork(CBlockHeader& starting_header);
    /**
     * Generate headers in a chain that build off a given starting hash, using
     * the given nVersion, advancing time by 1 second from the starting
     * prev_time, and with a fixed merkle root hash.
     */
    void GenerateHeaders(std::vector<CBlockHeader>& headers, size_t count,
            const uint256& starting_hash, const int nVersion, int prev_time,
            const uint256& merkle_root, const uint32_t nBits);
};

void HeadersGeneratorSetup::FindProofOfWork(CBlockHeader& starting_header)
{
    while (!CheckProofOfWork(GetPoWAlgoHash(starting_header), starting_header.nBits, Params().GetConsensus())) {
        ++(starting_header.nNonce);
    }
}

void HeadersGeneratorSetup::GenerateHeaders(std::vector<CBlockHeader>& headers,
        size_t count, const uint256& starting_hash, const int nVersion, int prev_time,
        const uint256& merkle_root, const uint32_t nBits)
{
    uint256 prev_hash = starting_hash;

    while (headers.size() < count) {
        headers.emplace_back();
        CBlockHeader& next_header = headers.back();;
        next_header.nVersion = nVersion;
        next_header.hashPrevBlock = prev_hash;
        next_header.hashMerkleRoot = merkle_root;
        next_header.nTime = prev_time+1;
        next_header.nBits = nBits;

        FindProofOfWork(next_header);
        prev_hash = next_header.GetHash();
        prev_time = next_header.nTime;
    }
    return;
}

BOOST_FIXTURE_TEST_SUITE(headers_sync_chainwork_tests, HeadersGeneratorSetup)

// In this test, we construct two sets of headers from genesis, one with
// sufficient proof of work and one without.
// 1. We deliver the first set of headers and verify that the headers sync state
//    updates to the REDOWNLOAD phase successfully.
// 2. Then we deliver the second set of headers and verify that they fail
//    processing (presumably due to commitments not matching).
// 3. Finally, we verify that repeating with the first set of headers in both
//    phases is successful.
BOOST_AUTO_TEST_CASE(headers_sync_state)
{
    std::vector<CBlockHeader> first_chain;
    std::vector<CBlockHeader> second_chain;

    std::unique_ptr<HeadersSyncState> hss;

    const int target_blocks = 15000;

    // Generate headers for two different chains (using differing merkle roots
    // to ensure the headers are different).
    GenerateHeaders(first_chain, target_blocks-1, Params().GenesisBlock().GetHash(),
            Params().GenesisBlock().nVersion, Params().GenesisBlock().nTime,
            ArithToUint256(0), Params().GenesisBlock().nBits);

    GenerateHeaders(second_chain, target_blocks-2, Params().GenesisBlock().GetHash(),
            Params().GenesisBlock().nVersion, Params().GenesisBlock().nTime,
            ArithToUint256(1), Params().GenesisBlock().nBits);

    const CBlockIndex* chain_start = WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(Params().GenesisBlock().GetHash()));
    const auto work = CalculateHeadersWork(first_chain, *chain_start);
    BOOST_REQUIRE(work);
    const arith_uint256 chain_work = chain_start->GetChainWork() + *work;
    std::vector<CBlockHeader> headers_batch;

    // Feed the first chain to HeadersSyncState, by delivering 1 header
    // initially and then the rest.
    headers_batch.insert(headers_batch.end(), std::next(first_chain.begin()), first_chain.end());

    hss.reset(new HeadersSyncState(0, Params().GetConsensus(), chain_start, chain_work));
    (void)hss->ProcessNextHeaders({first_chain.front()}, true);
    // Pretend the first header is still "full", so we don't abort.
    auto result = hss->ProcessNextHeaders(headers_batch, true);

    // This chain should look valid, and we should have met the proof-of-work
    // requirement.
    BOOST_CHECK(result.success);

    BOOST_CHECK(result.request_more);
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::REDOWNLOAD);

    // Try to sneakily feed back the second chain.
    result = hss->ProcessNextHeaders(second_chain, true);
    BOOST_CHECK(!result.success); // foiled!
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::FINAL);

    // Now try again, this time feeding the first chain twice.
    hss.reset(new HeadersSyncState(0, Params().GetConsensus(), chain_start, chain_work));
    (void)hss->ProcessNextHeaders(first_chain, true);
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::REDOWNLOAD);

    result = hss->ProcessNextHeaders(first_chain, true);
    BOOST_CHECK(result.success);
    BOOST_CHECK(!result.request_more);
    // All headers should be ready for acceptance:
    BOOST_CHECK(result.pow_validated_headers.size() == first_chain.size());
    // Nothing left for the sync logic to do:
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::FINAL);

    // Finally, verify that just trying to process the second chain would not
    // succeed (too little work)
    hss.reset(new HeadersSyncState(0, Params().GetConsensus(), chain_start, chain_work));
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::PRESYNC);
     // Pretend just the first message is "full", so we don't abort.
    (void)hss->ProcessNextHeaders({second_chain.front()}, true);
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::PRESYNC);

    headers_batch.clear();
    headers_batch.insert(headers_batch.end(), std::next(second_chain.begin(), 1), second_chain.end());
    // Tell the sync logic that the headers message was not full, implying no
    // more headers can be requested. For a low-work-chain, this should causes
    // the sync to end with no headers for acceptance.
    result = hss->ProcessNextHeaders(headers_batch, false);
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::FINAL);
    BOOST_CHECK(result.pow_validated_headers.empty());
    BOOST_CHECK(!result.request_more);
    // Nevertheless, no validation errors should have been detected with the
    // chain:
    BOOST_CHECK(result.success);

    // Neither pass may credit geometric work to an incorrect target.
    auto invalid_chain = first_chain;
    invalid_chain.at(Params().GetConsensus().workComputationChangeTarget - 1).nBits ^= 1;
    hss = std::make_unique<HeadersSyncState>(0, Params().GetConsensus(), chain_start, chain_work);
    result = hss->ProcessNextHeaders(invalid_chain, true);
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.pow_validated_headers.empty());
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::FINAL);

    hss = std::make_unique<HeadersSyncState>(0, Params().GetConsensus(), chain_start, chain_work);
    (void)hss->ProcessNextHeaders(first_chain, true);
    BOOST_REQUIRE(hss->GetState() == HeadersSyncState::State::REDOWNLOAD);
    result = hss->ProcessNextHeaders(invalid_chain, true);
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.pow_validated_headers.empty());
    BOOST_CHECK(hss->GetState() == HeadersSyncState::State::FINAL);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
// Compare the bounded download history with the unchanged full-chain lookup.
// Old algorithm records deliberately fall outside the recent timestamp window.
void CheckBoundedHeaderWork(int event_height)
{
    const auto& params = Params().GetConsensus();
    constexpr int algorithms[]{ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT, ALGO_ODO};
    std::deque<CBlockIndex> full_chain;
    for (int i = 0; i < 97; ++i) {
        auto* previous = full_chain.empty() ? nullptr : &full_chain.back();
        full_chain.emplace_back();
        auto& block = full_chain.back();
        block.pprev = previous;
        block.nHeight = event_height - 100 + i;
        block.nVersion = 4 | GetVersionForAlgo(algorithms[i % std::size(algorithms)]);
        block.nBits = 0x1900ffff;
        block.nTime = 1500000000 + i * 15;
    }
    HeadersWorkState work_state(params, full_chain.back());
    for (int i = 0; i < 450; ++i) {
        CBlockHeader header;
        int algo = i < 250 ? ALGO_SCRYPT : algorithms[i % std::size(algorithms)];
        if (!IsAlgoActive(&full_chain.back(), params, algo)) algo = ALGO_SCRYPT;
        header.nVersion = 4 | GetVersionForAlgo(algo);
        // Include both sides of the testnet minimum-difficulty exception.
        const int spacing = 2 * params.nTargetSpacing;
        const int delta = i % 7 < 2 ? spacing + 1 : (i % 7 == 2 ? spacing : spacing - 1);
        header.nTime = full_chain.back().nTime + delta;
        header.nBits = GetNextWorkRequired(&full_chain.back(), &header, params, algo);
        auto* previous = &full_chain.back();
        full_chain.emplace_back(header);
        auto& next = full_chain.back();
        next.pprev = previous;
        next.nHeight = previous->nHeight + 1;
        const auto work = work_state.AddHeader(header);
        BOOST_REQUIRE(work);
        BOOST_CHECK(*work == GetBlockProof(next));
        BOOST_CHECK_LE(work_state.GetHistorySize(), NUM_ALGOS * params.nAveragingInterval + CBlockIndex::nMedianTimeSpan);
    }
    CBlockHeader next;
    next.nVersion = 4 | GetVersionForAlgo(ALGO_SCRYPT);
    next.nTime = full_chain.back().nTime + 15;
    next.nBits = GetNextWorkRequired(&full_chain.back(), &next, params, ALGO_SCRYPT);
    CBlockHeader wrong = next;
    wrong.nBits ^= 1;
    BOOST_CHECK(!work_state.AddHeader(wrong));
    // A rejected header must not alter the history used by the next header.
    auto* previous = &full_chain.back();
    full_chain.emplace_back(next);
    full_chain.back().pprev = previous;
    full_chain.back().nHeight = previous->nHeight + 1;
    const auto work = work_state.AddHeader(next);
    BOOST_REQUIRE(work);
    BOOST_CHECK(*work == GetBlockProof(full_chain.back()));
}

struct MainHeaderWorkSetup : BasicTestingSetup {
    MainHeaderWorkSetup() : BasicTestingSetup(ChainType::MAIN) {}
};
struct TestnetHeaderWorkSetup : BasicTestingSetup {
    TestnetHeaderWorkSetup() : BasicTestingSetup(ChainType::TESTNET) {}
};
struct SignetHeaderWorkSetup : BasicTestingSetup {
    SignetHeaderWorkSetup() : BasicTestingSetup(ChainType::SIGNET) {}
};
struct RegtestHeaderWorkSetup : BasicTestingSetup {
    RegtestHeaderWorkSetup() : BasicTestingSetup(ChainType::REGTEST) {}
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(headers_work_main_tests, MainHeaderWorkSetup)
BOOST_AUTO_TEST_CASE(full_history_equivalence)
{
    CheckBoundedHeaderWork(Params().GetConsensus().workComputationChangeTarget);
    CheckBoundedHeaderWork(Params().GetConsensus().OdoHeight);
}
BOOST_AUTO_TEST_CASE(historical_algorithm_gate)
{
    const auto& params = Params().GetConsensus();
    const int gate = std::min(params.nGroestlDeactivationHeight, params.AlgoLockHeight);
    std::deque<CBlockIndex> history;
    for (int i = 0; i < 80; ++i) {
        auto* previous = history.empty() ? nullptr : &history.back();
        history.emplace_back();
        auto& block = history.back();
        block.pprev = previous;
        block.nHeight = gate - 83 + i;
        block.nVersion = 4 | GetVersionForAlgo(i % NUM_ALGOS);
        block.nBits = 0x1900ffff;
        block.nTime = 1500000000 + i * 15;
    }
    HeadersWorkState state(params, history.back());
    const auto append = [&](int version, bool accepted) {
        CBlockHeader header;
        header.nVersion = version;
        header.nTime = history.back().nTime + 15;
        header.nBits = GetNextWorkRequired(&history.back(), &header, params, header.GetAlgo());
        const auto work = state.AddHeader(header);
        BOOST_REQUIRE_EQUAL(work.has_value(), accepted);
        if (!accepted) return;
        auto* previous = &history.back();
        history.emplace_back(header);
        history.back().pprev = previous;
        history.back().nHeight = previous->nHeight + 1;
        BOOST_CHECK(*work == GetBlockProof(history.back()));
    };
    append(4 | GetVersionForAlgo(ALGO_GROESTL), true);
    append(4 | (10 << 8), true); // Unknown bits were not banned at this height.
    append(4 | GetVersionForAlgo(ALGO_GROESTL), true);
    append(4 | GetVersionForAlgo(ALGO_GROESTL), false);
    append(4 | (10 << 8), false);
    append(4 | GetVersionForAlgo(ALGO_SCRYPT), true);
}
BOOST_AUTO_TEST_CASE(height_limit)
{
    CBlockIndex last;
    last.nHeight = std::numeric_limits<int>::max();
    HeadersWorkState state(Params().GetConsensus(), last);
    BOOST_CHECK(!state.AddHeader(CBlockHeader{}));
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(headers_work_signet_tests, SignetHeaderWorkSetup)
BOOST_AUTO_TEST_CASE(full_history_equivalence)
{
    CheckBoundedHeaderWork(Params().GetConsensus().workComputationChangeTarget);
    CheckBoundedHeaderWork(Params().GetConsensus().algoSwapChangeTarget);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(headers_work_regtest_tests, RegtestHeaderWorkSetup)
BOOST_AUTO_TEST_CASE(invalid_targets_before_geometric_work)
{
    const auto& params = Params().GetConsensus();
    CBlockIndex parent(Params().GenesisBlock());
    HeadersWorkState work_state(params, parent);
    CBlockHeader header;
    header.nVersion = BLOCK_VERSION_DEFAULT;
    header.nTime = parent.nTime + 1;
    for (const uint32_t bits : {0U, 0x20800001U, 0x23000001U, 0x2100ffffU}) {
        header.nBits = bits;
        BOOST_CHECK(!work_state.AddHeader(header));
        BOOST_CHECK_EQUAL(work_state.GetHistorySize(), 1U);
    }
    // Rejected targets must not change the history used by a valid header.
    header.nBits = GetNextWorkRequired(&parent, &header, params, header.GetAlgo());
    BOOST_CHECK(work_state.AddHeader(header));
    BOOST_CHECK_EQUAL(work_state.GetHistorySize(), 2U);
}
BOOST_AUTO_TEST_CASE(full_history_equivalence)
{
    CheckBoundedHeaderWork(Params().GetConsensus().workComputationChangeTarget);
    CheckBoundedHeaderWork(Params().GetConsensus().OdoHeight);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(headers_work_testnet_tests, TestnetHeaderWorkSetup)
BOOST_AUTO_TEST_CASE(full_history_equivalence)
{
    CheckBoundedHeaderWork(Params().GetConsensus().workComputationChangeTarget);
    CheckBoundedHeaderWork(Params().GetConsensus().OdoHeight);
}
BOOST_AUTO_TEST_SUITE_END()
