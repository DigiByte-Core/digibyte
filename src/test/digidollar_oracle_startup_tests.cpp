// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Consensus-safety tests for the DigiDollar startup oracle price-cache scan
// (OracleBundleManager::LoadPricesFromChain / ShouldLoadStartupOraclePriceForBlock).
//
// The per-block startup gate was changed to evaluate the BIP9 DigiDollar
// activation predicate through the SHARED, memoized versionbits cache
// (chainman.m_versionbitscache) instead of allocating a throwaway
// VersionBitsCache on every one of the up-to ~172,800 iterations. That is a
// pure performance change: it MUST compute the identical activation boolean, so
// it can neither load a price from a block that would previously have been
// skipped (a bypass) nor skip a block that would previously have been loaded (a
// consensus divergence). These tests pin exactly that invariant.
//
// End-to-end loader correctness (a real MuSig2 v0x03 coinbase bundle -> price
// cache) is already covered by rh66_startup_oracle_price_loading_tests in
// rh61_coinbase_price_cache_poisoning_tests.cpp; the exhaustive BIP9 State()
// machine is covered by versionbits_tests.cpp. Here we prove only that the
// startup gate tracks that predicate exactly, on a real connected chain.

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <consensus/volatility.h>
#include <digidollar/digidollar.h>
#include <oracle/bundle_manager.h>
#include <sync.h>
#include <util/time.h>
#include <validation.h>

#include <limits>
#include <vector>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(digidollar_oracle_startup_tests, TestChain100Setup)

// The two DigiDollar::IsDigiDollarEnabled overloads — the shared-cache one
// (const ChainstateManager&) the fix switches TO, and the throwaway-cache one
// (const Consensus::Params&) it switches AWAY from — must return the SAME
// activation boolean for every block on a real chain. Any divergence here would
// be a consensus divergence hiding behind a "performance" change.
BOOST_AUTO_TEST_CASE(startup_predicate_overloads_agree_on_real_chain)
{
    // Extend the pre-mined 100-block regtest chain so the scan spans a
    // substantial run of genuinely-connected blocks.
    mineBlocks(600);

    const Consensus::Params& consensus = m_node.chainman->GetConsensus();

    LOCK(cs_main);
    const CChain& chain = m_node.chainman->ActiveChain();
    BOOST_REQUIRE(chain.Height() > 200);

    for (int h = 0; h <= chain.Height(); ++h) {
        const CBlockIndex* index = chain[h];
        BOOST_REQUIRE(index != nullptr);
        const bool shared = DigiDollar::IsDigiDollarEnabled(index, *m_node.chainman);
        const bool throwaway = DigiDollar::IsDigiDollarEnabled(index, consensus);
        BOOST_CHECK_MESSAGE(shared == throwaway,
            "IsDigiDollarEnabled overloads diverged at height " << h
            << " (shared=" << shared << " throwaway=" << throwaway << ")");
    }
}

// The startup per-block gate must equal the BIP9 activation predicate applied to
// the block's parent — exactly the predicate ConnectBlock uses. Proving equality
// proves there is NO bypass (it never accepts a price from an un-activated block)
// and NO over-skip (it never drops an activated block's price).
BOOST_AUTO_TEST_CASE(should_load_startup_matches_bip9_predicate)
{
    mineBlocks(600);

    LOCK(cs_main);
    const CChain& chain = m_node.chainman->ActiveChain();
    BOOST_REQUIRE(chain.Height() > 200);

    for (int h = 1; h <= chain.Height(); ++h) {
        const CBlockIndex* index = chain[h];
        BOOST_REQUIRE(index != nullptr);
        BOOST_REQUIRE(index->pprev != nullptr);
        const bool gate = OracleBundleManager::ShouldLoadStartupOraclePriceForBlock(
            h, index, *m_node.chainman);
        const bool predicate = DigiDollar::IsDigiDollarEnabled(index->pprev, *m_node.chainman);
        BOOST_CHECK_MESSAGE(gate == predicate,
            "ShouldLoadStartupOraclePriceForBlock diverged from the BIP9 predicate at height "
            << h << " (gate=" << gate << " predicate=" << predicate << ")");
    }
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
struct RetainedOracleHistorySetup : TestChain100Setup {
    RetainedOracleHistorySetup()
        : TestChain100Setup(ChainType::REGTEST, {"-digidollaractivationheight=1"}) {}

    ~RetainedOracleHistorySetup()
    {
        OracleBundleManager::GetInstance().Clear();
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    }
};

struct UnreadableOracleBlock {
    unsigned int& position;
    const unsigned int saved;
    explicit UnreadableOracleBlock(unsigned int& value) : position(value), saved(value)
    {
        AssertLockHeld(cs_main);
        position = std::numeric_limits<unsigned int>::max();
    }
    ~UnreadableOracleBlock()
    {
        AssertLockHeld(cs_main);
        position = saved;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_oracle_reconstruction_tests, RetainedOracleHistorySetup)

BOOST_AUTO_TEST_CASE(required_block_read_failure_keeps_state_and_retry_succeeds)
{
    LOCK(cs_main);
    auto& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.UpdatePriceCache(100, 70000, GetTime());
    DigiDollar::Volatility::VolatilityMonitor::ReconstructFromBlockData({{70000, GetTime(), 100}}, 100);
    const auto saved = manager.GetStats();
    const auto history = DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory();
    BOOST_REQUIRE_EQUAL(history.size(), 1U);
    const uint256 tip = m_node.chainman->ActiveChain().Tip()->GetBlockHash();
    CBlockIndex* unreadable = m_node.chainman->ActiveChain()[50];
    BOOST_REQUIRE(unreadable);
    std::vector<std::pair<uint64_t, uint64_t>> updates;
    OracleBundleManager::LoadCallbacks callbacks;
    callbacks.progress = [&](uint64_t complete, uint64_t total) { updates.emplace_back(complete, total); };
    {
        UnreadableOracleBlock unavailable(unreadable->nDataPos);
        const auto result = OracleBundleManager::LoadPricesFromChain(*m_node.chainman, callbacks);
        BOOST_CHECK(result.status == OracleBundleManager::LoadStatus::READ_ERROR);
        BOOST_CHECK_EQUAL(result.height, 50);
        BOOST_CHECK(result.block_hash == unreadable->GetBlockHash());
        BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(100), 70000U);
        BOOST_CHECK_EQUAL(manager.GetStats().latest_price, saved.latest_price);
        BOOST_CHECK_EQUAL(manager.GetStats().last_update, saved.last_update);
        const auto unchanged = DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory();
        BOOST_REQUIRE_EQUAL(unchanged.size(), history.size());
        BOOST_CHECK_EQUAL(unchanged[0].price, history[0].price);
        BOOST_CHECK_EQUAL(unchanged[0].timestamp, history[0].timestamp);
        BOOST_CHECK_EQUAL(unchanged[0].height, history[0].height);
        BOOST_REQUIRE(!updates.empty());
        BOOST_CHECK_LT(updates.back().first, updates.back().second);
    }
    updates.clear();
    const auto complete = OracleBundleManager::LoadPricesFromChain(*m_node.chainman, callbacks);
    BOOST_CHECK(complete.status == OracleBundleManager::LoadStatus::COMPLETE);
    BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() == tip);
    BOOST_CHECK(DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().empty());
    BOOST_REQUIRE(!updates.empty());
    BOOST_CHECK_EQUAL(updates.front().first, 0U);
    BOOST_CHECK_EQUAL(updates.back().first, 100U);
    BOOST_CHECK_EQUAL(updates.back().second, 100U);
    BOOST_CHECK_LE(updates.size(), 102U);
    uint64_t previous = 0;
    for (const auto& [processed, total] : updates) {
        BOOST_CHECK_GE(processed, previous);
        BOOST_CHECK_EQUAL(total, 100U);
        previous = processed;
    }
    manager.Clear();
    DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
}

BOOST_AUTO_TEST_CASE(cancel_before_reading_and_mid_scan_never_reports_completion)
{
    auto& manager = OracleBundleManager::GetInstance();
    for (const int cancel_after : {0, 50}) {
        manager.Clear();
        manager.UpdatePriceCache(100, 70000, GetTime());
        const auto saved = manager.GetStats();
        int polls = 0;
        std::vector<std::pair<uint64_t, uint64_t>> updates;
        OracleBundleManager::LoadCallbacks callbacks;
        callbacks.cancelled = [&] { return polls++ == cancel_after; };
        callbacks.progress = [&](uint64_t complete, uint64_t total) { updates.emplace_back(complete, total); };
        const auto result = OracleBundleManager::LoadPricesFromChain(*m_node.chainman, callbacks);
        BOOST_CHECK(result.status == OracleBundleManager::LoadStatus::CANCELLED);
        BOOST_CHECK_EQUAL(polls, cancel_after + 1);
        BOOST_CHECK_EQUAL(manager.GetStats().latest_price, saved.latest_price);
        BOOST_CHECK_EQUAL(manager.GetStats().last_update, saved.last_update);
        BOOST_REQUIRE(!updates.empty());
        BOOST_CHECK_EQUAL(updates.front().first, 0U);
        BOOST_CHECK_LT(updates.back().first, 100U);
    }
    manager.Clear();
}

BOOST_AUTO_TEST_SUITE_END()
