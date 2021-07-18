// Copyright (c) 2009-2019 The Bitcoin Core developers
// Copyright (c) 2014-2019 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <validation.h>
#include <net.h>

#include <test/test_digibyte.h>

#include <boost/signals2/signal.hpp>
#include <boost/test/unit_test.hpp>

#define END_OF_SUPPLY_CURVE 110579600

// Set OUTPUT_SUPPLY_SAMPLES_ENABLED to 1 to output
// sampled Supply-curve data points (SAMPLE_INTERVAL)
#ifndef OUTPUT_SUPPLY_SAMPLES_ENABLED
    #define OUTPUT_SUPPLY_SAMPLES_ENABLED 0
#endif

#ifndef SAMPLE_INTERVAL
    #define SAMPLE_INTERVAL 100
#endif

#ifndef WHOLE_COIN
    #define WHOLE_COIN false
#endif

#define CALC_COIN(amount) (WHOLE_COIN ? (amount / COIN) : (amount))

#if OUTPUT_SUPPLY_SAMPLES_ENABLED 
    #define DEBUG(x,y) if (x % SAMPLE_INTERVAL == 0) { \
        std::cerr << x << ';' << CALC_COIN(y) << std::endl; \
    }
#else
    #define DEBUG(x,y) (void) (x)
#endif

#ifndef BLOCK_TIME_SECONDS
    #define BLOCK_TIME_SECONDS 15
#endif

#ifndef SECONDS_PER_MONTH
    #define SECONDS_PER_MONTH (60 * 60 * 24 * 365 / 12)
#endif    

#ifndef ENABLE_TESTNET_SUBSIDY_TESTS
    #define ENABLE_TESTNET_SUBSIDY_TESTS 0
#endif

BOOST_FIXTURE_TEST_SUITE(main_tests, TestingSetup)

// checks for:
// 1. block subsidy in the past hasn't been altered (The past is fixed but the future is malleable)
// 2. block subsidy never increases, so subsidy(n) <= subsidy(n+1), n is a block height
// 3. block subsidy eventually hits 0
static void TestBlockSubsidy(const Consensus::Params& consensusParams, int nMaxBlocks, CAmount* nSumOut)
{
    const CAmount BlockHeightVerbatimCheck = 14394800; // ~(2022-01-21)
    CAmount nSum = 0;
    CAmount nInitialSubsidy = 72000 * COIN;

    CAmount nPreviousSubsidy = nInitialSubsidy * 2; // for height == 0
    BOOST_CHECK_EQUAL(nPreviousSubsidy, nInitialSubsidy * 2);
    BOOST_REQUIRE(BlockHeightVerbatimCheck <= nMaxBlocks);  // sanity check

    /* Before first hard fork */

    // 72000 reward for the first 1440 blocks
    for (int nBlocks = 0; nBlocks < 1440 && nBlocks < consensusParams.nDiffChangeTarget; ++nBlocks)
    {
        int nHeight = nBlocks;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_REQUIRE_EQUAL(nSubsidy, nInitialSubsidy);

        nSum += nSubsidy;
        DEBUG(nBlocks, nSubsidy);
    }

    // 16000 rewards until block height 5760
    for (int nBlocks = 1440; nBlocks < 5760 && nBlocks < consensusParams.nDiffChangeTarget; ++nBlocks)
    {
        int nHeight = nBlocks;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_REQUIRE_EQUAL(nSubsidy, 16000 * COIN);

        nSum += nSubsidy;
        DEBUG(nBlocks, nSubsidy);
    }

    // 8000 mining rewards until block height 67,200
    for (int nBlocks = 5760; nBlocks < consensusParams.nDiffChangeTarget; ++nBlocks)
    {
        int nHeight = nBlocks;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_REQUIRE_EQUAL(nSubsidy, 8000 * COIN);

        nSum += nSubsidy;
        DEBUG(nBlocks, nSubsidy);
    }

    // Dynamic mining rewards from block height 67,200 to block height 400,000 
    for (int nBlocks = consensusParams.nDiffChangeTarget; nBlocks < consensusParams.alwaysUpdateDiffChangeTarget; ++nBlocks)
    {
        int nHeight = nBlocks;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);

        CAmount nExpectedSubsidy = 8000 * COIN;
        int nHeightWithinFork = (nHeight - consensusParams.nDiffChangeTarget);

        for (int i = 0; i < (nHeightWithinFork / consensusParams.patchBlockRewardDuration) + 1; ++i) {
            nExpectedSubsidy -= nExpectedSubsidy / 200; // dec by 0.5%
        }

        BOOST_REQUIRE_EQUAL(nSubsidy, nExpectedSubsidy);

        nSum += nSubsidy;
        DEBUG(nBlocks, nSubsidy);
    }

    // Updated dynamic mining rewards from block height 400,000 to block height 1,430,000
    for (int nBlocks = consensusParams.alwaysUpdateDiffChangeTarget; nBlocks < consensusParams.workComputationChangeTarget; ++nBlocks)
    {
        int nHeight = nBlocks;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);

        CAmount nExpectedSubsidy = 2459 * COIN;
        int nHeightWithinFork = (nHeight - consensusParams.alwaysUpdateDiffChangeTarget);

        for (int i = 0; i < (nHeightWithinFork / consensusParams.patchBlockRewardDuration2) + 1; ++i) {
            nExpectedSubsidy -= nExpectedSubsidy / 100; // dec by 1% per month
        }

        BOOST_REQUIRE_EQUAL(nSubsidy, nExpectedSubsidy);

        nSum += nSubsidy;
        DEBUG(nBlocks, nSubsidy);
    }

    {
        // Updated dynamic mining rewards from block height 1,430,000 to max block height.
        // Intended blockheight: 41.6 million
        CAmount nExpectedSubsidyStart = 2157 * COIN / 2;
        CAmount nExpectedSubsidy = nExpectedSubsidyStart;
        int nMonthsConsidered = 0;

        for (int nBlocks = consensusParams.workComputationChangeTarget; nBlocks < BlockHeightVerbatimCheck; ++nBlocks) {
            int nHeight = nBlocks;
            CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);

            int nHeightWithinFork = (nHeight - consensusParams.workComputationChangeTarget);
            int nMonths = nHeightWithinFork * BLOCK_TIME_SECONDS / SECONDS_PER_MONTH;

            if (nMonthsConsidered < nMonths) {
                // Calculate new subsidy for number of months `nMonths`.
                // This is a major optimization in order to reduce the
                // number of inner loops

                // Recalculate subsidy
                for (int i = nMonthsConsidered; i < nMonths; ++i) {
                    // Decay factor: 98884/100000
                    nExpectedSubsidy *= 98884; 
                    nExpectedSubsidy /= 100000; 
                    ++nMonthsConsidered;
                }
            }

            BOOST_REQUIRE_EQUAL(nSubsidy, nExpectedSubsidy);

            nSum += nSubsidy;
            nPreviousSubsidy = nSubsidy;
            DEBUG(nBlocks, nSubsidy);
        }
    }

    {
        // Future block checks which don't need to exactly match anything
        for (int nBlocks = BlockHeightVerbatimCheck; nBlocks < nMaxBlocks; ++nBlocks) {
            int nHeight = nBlocks;
            CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);

            BOOST_REQUIRE_LE(nSubsidy, nPreviousSubsidy);

            nSum += nSubsidy;
            nPreviousSubsidy = nSubsidy;
            DEBUG(nBlocks, nSubsidy);
        }
    }

    CAmount nSubsidy = GetBlockSubsidy(nMaxBlocks, consensusParams);
    BOOST_REQUIRE_EQUAL(nSubsidy, 0);

    if (nSumOut != NULL) {
        *nSumOut = nSum;
    }
}

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    CAmount sum;
    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
    const auto testChainParams = CreateChainParams(CBaseChainParams::TESTNET);
    TestBlockSubsidy(chainParams->GetConsensus(), END_OF_SUPPLY_CURVE, &sum); // Mainnet

    CAmount nExpectedTotalSupply = 2100024577003013036ULL;  // this is the maximum supply, not the actual supply
    BOOST_CHECK_EQUAL(sum, nExpectedTotalSupply);

#if OUTPUT_SUPPLY_SAMPLES_ENABLED
    // Output the accumulated supply until END_OF_SUPPLY_CURVE
    std::cout << "(mainnet): MAXIMUM SUPPLY: " << sum << " dgbSATS (" << (sum / COIN) << " DGB)";
#elif ENABLE_TESTNET_SUBSIDY_TESTS != 0
    // Only perform test on TESTNET too if requested so.
    TestBlockSubsidy(testChainParams->GetConsensus(), END_OF_SUPPLY_CURVE, NULL); // Testnet
#endif
}

static bool ReturnFalse() { return false; }
static bool ReturnTrue() { return true; }

BOOST_AUTO_TEST_CASE(test_combiner_all)
{
    boost::signals2::signal<bool (), CombinerAll> Test;
    BOOST_CHECK(Test());
    Test.connect(&ReturnFalse);
    BOOST_CHECK(!Test());
    Test.connect(&ReturnTrue);
    BOOST_CHECK(!Test());
    Test.disconnect(&ReturnFalse);
    BOOST_CHECK(Test());
    Test.disconnect(&ReturnTrue);
    BOOST_CHECK(Test());
}
BOOST_AUTO_TEST_SUITE_END()
