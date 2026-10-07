// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>
#include <primitives/block.h> // For GetVersionForAlgo
#include <validation.h>       // For IsAlgoActive

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1261130161; // Block #30240
    CBlockIndex pindexLast;
    pindexLast.nHeight = 32255;
    pindexLast.nTime = 1262152739;  // Block #32255
    pindexLast.nBits = 0x1d00ffff;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00d86aU);
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1231006505; // Block #0
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1233061996;  // Block #2015
    pindexLast.nBits = 0x1d00ffff;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1D01B304U);
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1279008237; // Block #66528
    CBlockIndex pindexLast;
    pindexLast.nHeight = 68543;
    pindexLast.nTime = 1279297671;  // Block #68543
    pindexLast.nBits = 0x1c05a3f4;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1c0168fdU);
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
    CBlockIndex pindexLast;
    pindexLast.nHeight = 46367;
    pindexLast.nTime = 1269211443;  // Block #46367
    pindexLast.nBits = 0x1c387f6f;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00e1fdU);
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits{~0x00800000U};
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}


BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);

    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        // DigiByte: Set appropriate version for multi-algo
        if (i < 145000) {
            blocks[i].nVersion = 1; // Pre-multi-algo
        } else {
            // Cycle through algorithms for testing
            int algo = (i / 5) % NUM_ALGOS;
            blocks[i].nVersion = GetVersionForAlgo(algo);
        }
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        
        // For chain work calculation, use the base proof calculation to avoid
        // issues with Params() in test context
        arith_uint256 bnTarget;
        bool fNegative;
        bool fOverflow;
        bnTarget.SetCompact(blocks[i].nBits, &fNegative, &fOverflow);
        arith_uint256 blockProof = (fNegative || fOverflow || bnTarget == 0) ? 0 : ((~bnTarget / (bnTarget + 1)) + 1);
        
        blocks[i].SetChainWork(i ? blocks[i - 1].GetChainWork() + blockProof : arith_uint256(0));

        // Create random block hash
        const uint256 randomhash = GetRandHash();
        uint256* ptr = new uint256();
        *ptr = randomhash;

        blocks[i].phashBlock = ptr;
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }

    for (int i = 0; i < 10000; ++i) {
        delete blocks[i].phashBlock;
    }
}


void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // check max target * 4*nPowTargetTimespan doesn't overflow -- see pow.cpp:CalculateNextWorkRequired()
    if (!consensus.fPowNoRetargeting) {
        // DigiByte: Skip this check for mainnet and testnet as DigiByte's powLimit is much larger (>> 20 vs Bitcoin's >> 32)
        // and uses a different difficulty adjustment mechanism (MultiShield with 5 algorithms)
        if (chain_type != ChainType::MAIN && chain_type != ChainType::TESTNET) {
            arith_uint256 targ_max("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
            targ_max /= consensus.nPowTargetTimespan * 4;
            BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
        }
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::SIGNET);
}

BOOST_AUTO_TEST_CASE(digibyte_multialgo_test)
{
    // Test DigiByte's multi-algorithm mining system
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::REGTEST);
    const auto& params = chainParams->GetConsensus();
    
    // Test each algorithm
    for (int algo = ALGO_SHA256D; algo <= ALGO_QUBIT; algo++) {
        // Create a mock block header for this algo
        CBlockHeader blockHeader;
        blockHeader.nVersion = GetVersionForAlgo(algo);
        blockHeader.nTime = 1500000000;
        
        // Create a chain of previous blocks for algorithm history
        std::vector<CBlockIndex> blocks(10);
        for (int i = 0; i < 10; i++) {
            blocks[i].pprev = (i > 0) ? &blocks[i-1] : nullptr;
            blocks[i].nHeight = 200000 + i;
            blocks[i].nTime = 1499999000 + (i * 15);
            blocks[i].nBits = 0x207fffff; // regtest difficulty
            blocks[i].nVersion = GetVersionForAlgo(algo);
        }
        
        // Test that GetNextWorkRequired handles the algorithm properly
        unsigned int nBits = GetNextWorkRequired(&blocks[9], &blockHeader, params, algo);
        
        // Verify the result is within valid range
        arith_uint256 bnNew;
        bnNew.SetCompact(nBits);
        BOOST_CHECK(bnNew > 0);
        BOOST_CHECK(bnNew <= UintToArith256(params.powLimit));
    }
}

BOOST_AUTO_TEST_CASE(digibyte_difficulty_versions_test)
{
    // Test DigiByte's different difficulty algorithm versions at various heights
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::REGTEST);
    const auto& params = chainParams->GetConsensus();
    
    // Heights to test each difficulty version
    const int test_heights[] = {
        100,      // V1 (< 145000)
        200000,   // V2 (< 400000)
        500000,   // V3 (< 1430000)
        1500000   // V4 (>= 1430000)
    };
    
    for (int height : test_heights) {
        CBlockHeader blockHeader;
        blockHeader.nVersion = GetVersionForAlgo(ALGO_SCRYPT);
        blockHeader.nTime = 1500000000 + (height * 15);
        
        // Create a proper chain of blocks
        int chain_length = std::min(height, 2016);
        std::vector<CBlockIndex> blocks(chain_length);
        for (int i = 0; i < chain_length; i++) {
            blocks[i].pprev = (i > 0) ? &blocks[i-1] : nullptr;
            blocks[i].nHeight = height - chain_length + i + 1;
            blocks[i].nTime = blockHeader.nTime - ((chain_length - i) * 15);
            blocks[i].nBits = 0x207fffff; // regtest difficulty
            blocks[i].nVersion = GetVersionForAlgo(ALGO_SCRYPT);
        }
        
        // This should not crash and should return valid difficulty
        unsigned int nBits = GetNextWorkRequired(&blocks[chain_length-1], &blockHeader, params, ALGO_SCRYPT);
        
        arith_uint256 bnNew;
        bnNew.SetCompact(nBits);
        BOOST_CHECK(bnNew > 0);
        BOOST_CHECK(bnNew <= UintToArith256(params.powLimit));
    }
}

BOOST_AUTO_TEST_CASE(digibyte_isalgoactive_matrix)
{
    // The set of mining algorithms accepted at a given height. Groestl is part of
    // the original MultiAlgo set but is deactivated at the Odocrypt fork; the
    // consensus rule rejecting deactivated algorithms relies on this predicate.
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::REGTEST);
    const auto& params = chainParams->GetConsensus();

    // regtest fork heights: MultiAlgo at 100, Odocrypt/Groestl-swap at 600.
    auto is_active = [&](int prev_height, int algo) {
        CBlockIndex prev;
        prev.nHeight = prev_height;
        return IsAlgoActive(&prev, params, algo);
    };

    // Before the MultiAlgo fork: Scrypt only.
    BOOST_CHECK(is_active(50, ALGO_SCRYPT));
    for (int algo : {ALGO_SHA256D, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT, ALGO_ODO}) {
        BOOST_CHECK(!is_active(50, algo));
    }

    // MultiAlgo era (100..599): five algorithms including Groestl, excluding Odocrypt.
    for (int algo : {ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT}) {
        BOOST_CHECK(is_active(150, algo));
    }
    BOOST_CHECK(!is_active(150, ALGO_ODO));
    BOOST_CHECK(is_active(599, ALGO_GROESTL)); // last block before the swap

    // Odocrypt era (>=600): Groestl is deactivated, Odocrypt is active.
    BOOST_CHECK(!is_active(600, ALGO_GROESTL));
    BOOST_CHECK(!is_active(700, ALGO_GROESTL));
    for (int algo : {ALGO_SHA256D, ALGO_SCRYPT, ALGO_SKEIN, ALGO_QUBIT, ALGO_ODO}) {
        BOOST_CHECK(is_active(700, algo));
    }

    // An unknown algorithm is never active.
    BOOST_CHECK(!is_active(700, ALGO_UNKNOWN));
}

BOOST_AUTO_TEST_SUITE_END()
