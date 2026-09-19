// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// DigiByte has several mining algorithms and each one keeps its own difficulty.
// To work out the difficulty for the next block of one algorithm, the node has
// to find the most recent earlier block of that same algorithm.
//
// GetLastBlockIndexForAlgo in src/pow.cpp does that lookup. It walks back one
// block at a time, ignores blocks of other algorithms, and ignores blocks mined
// at the minimum difficulty. Every difficulty rule uses it, and so does the
// difficulty the node reports over the wire.
//
// The difficulty is part of what makes a block valid, so the answer this lookup
// gives can never change: a different block would mean a different difficulty,
// and nodes would disagree about which blocks are valid. These tests build
// chains of block index entries in memory, the same way the node builds them,
// and hold the lookup to a fixed set of answers written down as plain numbers:
// a table of one short chain that a reader can check by hand, one number
// standing for every answer on each of the other chains, and the difficulty each
// rule produces at the end of each chain that crosses a rule boundary.
//
// Those numbers were recorded while the node still had a second, faster version
// of the same lookup, which followed a set of per-algorithm pointers carried by
// every block header in memory. Both were run over every chain here and agreed
// on every answer; then the pointers and the second version were removed to save
// the memory they cost. The numbers below are that agreed answer, so they still
// hold the surviving lookup to what both of them used to say.
//
// The delicate part is how the lookup steps past a block mined at the minimum
// difficulty. Those blocks are only possible where the chain parameters set
// fPowAllowMinDifficultyBlocks, which is regtest, signet, and testnet started
// with -easypow. Mainnet and the public testnet never allow them. The chains
// below cover that branch with chain parameters where it is actually switched
// on, and one case shows that a walk which steps back twice there instead of
// once gives different answers, and gives them only where those blocks are
// allowed.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <rpc/blockchain.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

//! The six algorithms DigiByte has mined blocks with.
const std::vector<int> ALGO_PALETTE{ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT, ALGO_ODO};

//! Every algorithm number a caller might hand to the lookup: the six real
//! algorithms, the two spare slots (5 and 6) that were reserved for algorithms
//! which were never switched on, and numbers outside the array altogether.
const std::vector<int> ALGO_ARGUMENTS{
    ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT, 5, 6, ALGO_ODO,
    ALGO_UNKNOWN, NUM_ALGOS_IMPL, 99, -7};

//! A gap between a block and its parent that is small enough never to count as
//! a minimum-difficulty block. The rule is more than twice nTargetSpacing, and
//! nTargetSpacing is 60 seconds on every DigiByte network.
const int NORMAL_GAP{15};

//! A gap large enough to make a block count as mined at the minimum difficulty,
//! on the networks that allow those at all.
const int MIN_DIFFICULTY_GAP{600};

/**
 * A small repeatable number generator, so the mixed chains below come out the
 * same on every run and on every machine. The recorded answers depend on it.
 */
class Repeatable
{
public:
    explicit Repeatable(uint64_t seed) : m_state{seed} {}
    uint32_t Next()
    {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(m_state >> 33);
    }
    uint32_t Below(uint32_t limit) { return Next() % limit; }

private:
    uint64_t m_state;
};

/**
 * A chain of block index entries held in memory, built the way a running node
 * builds them.
 */
class TestChain
{
public:
    TestChain(std::string name, const Consensus::Params& params, int start_height, uint32_t nbits)
        : m_name{std::move(name)}, m_params{params}, m_next_height{start_height}, m_nbits{nbits} {}

    /** Add a block mined with the given algorithm, stamped this many seconds after its parent. */
    void Add(int algo, int seconds_after_parent)
    {
        AddWithVersion(BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo), seconds_after_parent);
    }

    /** Add a block whose whole version field is given, for versions that name no known algorithm. */
    void AddWithVersion(int32_t nVersion, int seconds_after_parent)
    {
        CBlockHeader header;
        header.nVersion = nVersion;
        header.nBits = m_nbits;
        if (!m_blocks.empty()) m_next_time += seconds_after_parent;
        header.nTime = m_next_time;

        // The node creates the entry from the header with this constructor, and
        // the constructor fills in this block's own slot in the per-algorithm
        // array (src/chain.cpp).
        m_blocks.push_back(std::make_unique<CBlockIndex>(header));
        CBlockIndex& index = *m_blocks.back();
        index.nHeight = m_next_height++;
        index.nTimeMax = index.nTime;

        if (m_blocks.size() > 1) {
            index.pprev = m_blocks[m_blocks.size() - 2].get();
            index.nTimeMax = std::max(index.pprev->nTimeMax, index.nTime);
        }
    }

    /** Give every block a different difficulty, so a caller can tell from a reported
     *  difficulty which block a lookup returned. */
    void SpreadOutTheDifficulties()
    {
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            arith_uint256 target{UintToArith256(m_params.powLimit)};
            target >>= 1 + static_cast<unsigned int>(i % 16);
            m_blocks[i]->nBits = target.GetCompact();
        }
    }

    const std::string& Name() const { return m_name; }
    const Consensus::Params& Params() const { return m_params; }
    size_t Size() const { return m_blocks.size(); }
    const CBlockIndex& At(size_t i) const { return *m_blocks.at(i); }
    const CBlockIndex& Tip() const { return *m_blocks.back(); }

private:
    std::string m_name;
    const Consensus::Params& m_params;
    std::vector<std::unique_ptr<CBlockIndex>> m_blocks;
    int m_next_height;
    uint32_t m_next_time{1500000000};
    uint32_t m_nbits;
};

int HeightOrNone(const CBlockIndex* index) { return index == nullptr ? -1 : index->nHeight; }

std::string Describe(const CBlockIndex* index)
{
    return index == nullptr ? std::string{"no block"} : strprintf("the block at height %d", index->nHeight);
}

/**
 * The mistake this whole test file exists to catch, written out so it can be
 * shown to fail. The second version of this lookup, the one that followed
 * per-algorithm pointers, had to take an extra step backwards when it met a
 * block mined at the minimum difficulty, because a block's own pointer points at
 * itself. A walk block by block that kept that extra step would go over two
 * blocks instead of one and return the wrong block.
 */
const CBlockIndex* WrongWalkThatStepsTwice(const CBlockIndex* pindex, const Consensus::Params& params, int algo)
{
    for (; pindex; pindex = pindex->pprev) {
        if (pindex->GetAlgo() != algo) continue;
        if (params.fPowAllowMinDifficultyBlocks &&
            pindex->pprev &&
            pindex->nTime > pindex->pprev->nTime + params.nTargetSpacing * 2) {
            pindex = pindex->pprev; // the extra step that makes this version wrong
            continue;
        }
        return pindex;
    }
    return nullptr;
}

/**
 * The answer the lookup has to give, worked out independently of src/pow.cpp:
 * the most recent block at or before this one that was mined with this
 * algorithm and was not stamped more than two block spacings after its parent.
 */
const CBlockIndex* ExpectedAnswer(const CBlockIndex* pindex, const Consensus::Params& params, int algo)
{
    while (pindex != nullptr) {
        const bool right_algorithm{pindex->GetAlgo() == algo};
        const bool minimum_difficulty{params.fPowAllowMinDifficultyBlocks &&
                                      pindex->pprev != nullptr &&
                                      pindex->nTime > pindex->pprev->nTime + params.nTargetSpacing * 2};
        if (right_algorithm && !minimum_difficulty) return pindex;
        pindex = pindex->pprev;
    }
    return nullptr;
}

/** Set POW_LOOKUP_RECORD in the environment to print the recorded answers in the
 *  form they are written down in this file instead of checking them. Every table
 *  in this file was produced that way, from a run in which both lookups were
 *  still present and agreed on every block of every chain. */
bool RecordingMode() { return std::getenv("POW_LOOKUP_RECORD") != nullptr; }

/** Which blocks of a chain to use as the starting point of a lookup. Short
 *  chains are swept completely; on the long ones every stride-th block is used,
 *  and the last block always is. */
bool UseAsStartingPoint(size_t i, size_t size, int stride)
{
    return i % static_cast<size_t>(stride) == 0 || i + 1 == size;
}

/** One number that stands for every answer the lookup gives on a chain. */
uint64_t AnswerChecksum(const TestChain& chain, int stride)
{
    uint64_t acc{1469598103934665603ULL};
    for (size_t i = 0; i < chain.Size(); ++i) {
        if (!UseAsStartingPoint(i, chain.Size(), stride)) continue;
        for (const int algo : ALGO_ARGUMENTS) {
            const CBlockIndex* found = GetLastBlockIndexForAlgo(&chain.At(i), chain.Params(), algo);
            acc = (acc ^ static_cast<uint64_t>(HeightOrNone(found) + 2)) * 1099511628211ULL;
        }
    }
    return acc;
}

/** Hold the lookup to the rule for every starting point and every algorithm. */
void CheckTheLookupMatchesTheRule(const TestChain& chain, int stride)
{
    size_t compared{0};
    for (size_t i = 0; i < chain.Size(); ++i) {
        if (!UseAsStartingPoint(i, chain.Size(), stride)) continue;
        for (const int algo : ALGO_ARGUMENTS) {
            const CBlockIndex* slow = GetLastBlockIndexForAlgo(&chain.At(i), chain.Params(), algo);
            ++compared;
            const CBlockIndex* expected = ExpectedAnswer(&chain.At(i), chain.Params(), algo);
            if (slow != expected) {
                BOOST_ERROR(strprintf(
                    "%s: starting at height %d, algorithm %d: the walk returned %s but the rule says %s",
                    chain.Name(), chain.At(i).nHeight, algo, Describe(slow), Describe(expected)));
                return;
            }
        }
    }
    BOOST_CHECK_MESSAGE(compared > 0, chain.Name() + ": nothing was compared");
}

/** How many starting-point-and-algorithm pairs the double-stepping rewrite gets
 *  wrong. On a chain that contains minimum-difficulty blocks this has to be more
 *  than zero, otherwise the chain is not covering the branch it was built for. */
size_t CountAnswersTheWrongWalkGetsWrong(const TestChain& chain, int stride)
{
    size_t wrong{0};
    for (size_t i = 0; i < chain.Size(); ++i) {
        if (!UseAsStartingPoint(i, chain.Size(), stride)) continue;
        for (const int algo : ALGO_ARGUMENTS) {
            const CBlockIndex* correct = GetLastBlockIndexForAlgo(&chain.At(i), chain.Params(), algo);
            if (correct != WrongWalkThatStepsTwice(&chain.At(i), chain.Params(), algo)) ++wrong;
        }
    }
    return wrong;
}

// ---------------------------------------------------------------------------
// The chains
// ---------------------------------------------------------------------------

/** Testnet as it is when the node is started with -easypow, which is a local
 *  test mode that switches minimum-difficulty blocks on. */
std::unique_ptr<const CChainParams> TestNetWithEasyPow()
{
    CChainParams::TestNetOptions options;
    options.easy_pow = true;
    return CChainParams::TestNet(options);
}

/** The chain parameter sets used below. Minimum-difficulty blocks are allowed on
 *  regtest, on signet, and on testnet started with -easypow; they are not
 *  allowed on mainnet or on the public testnet. */
struct ParamSets {
    std::unique_ptr<const CChainParams> main{CChainParams::Main()};
    std::unique_ptr<const CChainParams> testnet{CChainParams::TestNet()};
    std::unique_ptr<const CChainParams> testnet_easypow{TestNetWithEasyPow()};
    std::unique_ptr<const CChainParams> signet{CChainParams::SigNet(CChainParams::SigNetOptions{})};
    std::unique_ptr<const CChainParams> regtest{CChainParams::RegTest(CChainParams::RegTestOptions{})};
};

/** A chain to be swept, with how densely to sweep it and the answer checksum
 *  recorded from the run where both lookups were still present. */
struct Catalogued {
    std::unique_ptr<TestChain> chain;
    int stride;
    uint64_t expected_checksum;
    size_t expected_wrong_walk_mistakes;
};

/** A block mined at the minimum difficulty is one stamped more than two block
 *  spacings after its parent, so the gap of the block itself is what matters. */
void AddMixedRun(TestChain& chain, Repeatable& rng, int blocks, int min_difficulty_every)
{
    for (int i = 0; i < blocks; ++i) {
        const int algo = ALGO_PALETTE[rng.Below(static_cast<uint32_t>(ALGO_PALETTE.size()))];
        const bool minimum{min_difficulty_every > 0 && (i % min_difficulty_every) == 0};
        chain.Add(algo, minimum ? MIN_DIFFICULTY_GAP : NORMAL_GAP);
    }
}

std::vector<Catalogued> BuildCatalogue(const ParamSets& p)
{
    std::vector<Catalogued> out;
    const uint32_t easy_bits{0x207fffff};

    auto add = [&out](std::unique_ptr<TestChain> chain, int stride, uint64_t checksum, size_t wrong) {
        out.push_back(Catalogued{std::move(chain), stride, checksum, wrong});
    };

    // The first blocks of a chain: for most algorithms there is no earlier block
    // of that algorithm at all.
    {
        auto c = std::make_unique<TestChain>("first blocks of a chain", p.regtest->GetConsensus(), 0, easy_bits);
        c->Add(ALGO_SCRYPT, NORMAL_GAP);
        c->Add(ALGO_SHA256D, NORMAL_GAP);
        c->Add(ALGO_ODO, NORMAL_GAP);
        c->Add(ALGO_SCRYPT, NORMAL_GAP);
        c->Add(ALGO_QUBIT, NORMAL_GAP);
        c->Add(ALGO_SKEIN, NORMAL_GAP);
        add(std::move(c), 1, 12159372343494205426ULL, 0);
    }

    // A chain that only ever used two algorithms, so four of the six and both
    // spare slots never appear in it.
    {
        auto c = std::make_unique<TestChain>("two algorithms only", p.regtest->GetConsensus(), 0, easy_bits);
        Repeatable rng{11};
        for (int i = 0; i < 300; ++i) c->Add(rng.Below(2) == 0 ? ALGO_SCRYPT : ALGO_SHA256D, NORMAL_GAP);
        add(std::move(c), 1, 7538463257209060691ULL, 0);
    }

    // All five algorithms in irregular order, no minimum-difficulty blocks.
    {
        auto c = std::make_unique<TestChain>("mixed algorithms, irregular order", p.regtest->GetConsensus(), 0, easy_bits);
        Repeatable rng{2026};
        AddMixedRun(*c, rng, 900, 0);
        add(std::move(c), 1, 11122992391270840513ULL, 0);
    }

    // One algorithm for thousands of blocks, so the walk goes back a long way and
    // the answer for every other algorithm is "no block" after 5000 steps.
    {
        auto c = std::make_unique<TestChain>("one algorithm for 5000 blocks", p.regtest->GetConsensus(), 0, easy_bits);
        for (int i = 0; i < 5000; ++i) c->Add(ALGO_SCRYPT, NORMAL_GAP);
        add(std::move(c), 250, 5088834376324124375ULL, 0);
    }

    // One algorithm for thousands of blocks where every seventh block was mined at
    // the minimum difficulty, so the walk has to skip repeatedly over a long way.
    {
        auto c = std::make_unique<TestChain>("one algorithm for 3000 blocks, every seventh at minimum difficulty",
                                            p.regtest->GetConsensus(), 0, easy_bits);
        for (int i = 0; i < 3000; ++i) c->Add(ALGO_SCRYPT, (i % 7) == 0 ? MIN_DIFFICULTY_GAP : NORMAL_GAP);
        add(std::move(c), 150, 14489072749659973251ULL, 2);
    }

    // Minimum-difficulty blocks scattered through a mixed chain, on each of the
    // three configurations that allow them.
    {
        auto c = std::make_unique<TestChain>("regtest, mixed with minimum-difficulty blocks",
                                             p.regtest->GetConsensus(), 0, easy_bits);
        Repeatable rng{7};
        AddMixedRun(*c, rng, 600, 5);
        add(std::move(c), 1, 17813545191235853087ULL, 201);
    }
    {
        auto c = std::make_unique<TestChain>("signet, mixed with minimum-difficulty blocks",
                                             p.signet->GetConsensus(), 0, easy_bits);
        Repeatable rng{8};
        AddMixedRun(*c, rng, 700, 4);
        add(std::move(c), 1, 9615197062291813233ULL, 193);
    }
    {
        auto c = std::make_unique<TestChain>("testnet started with -easypow, mixed with minimum-difficulty blocks",
                                             p.testnet_easypow->GetConsensus(), 0, easy_bits);
        Repeatable rng{9};
        AddMixedRun(*c, rng, 500, 3);
        add(std::move(c), 1, 3021070253178294729ULL, 184);
    }

    // Runs of minimum-difficulty blocks of one and of several in a row, including
    // a run that reaches the start of the chain and an algorithm whose only block
    // was mined at the minimum difficulty.
    {
        auto c = std::make_unique<TestChain>("runs of minimum-difficulty blocks", p.regtest->GetConsensus(), 0, easy_bits);
        c->Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);  // 0: no parent, so never a minimum-difficulty block
        c->Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);  // 1: a run that starts at the beginning of the chain
        c->Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);  // 2
        c->Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);  // 3
        c->Add(ALGO_SHA256D, NORMAL_GAP);         // 4
        c->Add(ALGO_SCRYPT, NORMAL_GAP);          // 5
        c->Add(ALGO_QUBIT, MIN_DIFFICULTY_GAP);   // 6: the only qubit block, and at minimum difficulty
        c->Add(ALGO_SKEIN, MIN_DIFFICULTY_GAP);   // 7
        c->Add(ALGO_SKEIN, MIN_DIFFICULTY_GAP);   // 8: two skein blocks in a row at minimum difficulty
        c->Add(ALGO_SKEIN, NORMAL_GAP);           // 9
        c->Add(ALGO_GROESTL, MIN_DIFFICULTY_GAP); // 10
        c->Add(ALGO_GROESTL, MIN_DIFFICULTY_GAP); // 11
        c->Add(ALGO_GROESTL, MIN_DIFFICULTY_GAP); // 12: three groestl blocks in a row
        c->Add(ALGO_SHA256D, NORMAL_GAP);         // 13
        c->Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);  // 14
        c->Add(ALGO_ODO, NORMAL_GAP);             // 15
        c->Add(ALGO_SCRYPT, NORMAL_GAP);          // 16
        add(std::move(c), 1, 18019388214622607542ULL, 3);
    }

    // The same large time gaps on the two networks that never allow
    // minimum-difficulty blocks. There the branch cannot run at all, which is
    // why a test on mainnet parameters alone would prove nothing about it.
    {
        auto c = std::make_unique<TestChain>("mainnet, mixed with large time gaps", p.main->GetConsensus(), 145000, easy_bits);
        Repeatable rng{21};
        AddMixedRun(*c, rng, 500, 4);
        add(std::move(c), 1, 12518050998602076757ULL, 0);
    }
    {
        auto c = std::make_unique<TestChain>("public testnet, mixed with large time gaps", p.testnet->GetConsensus(), 1000, easy_bits);
        Repeatable rng{22};
        AddMixedRun(*c, rng, 500, 4);
        add(std::move(c), 1, 11410159724892831983ULL, 0);
    }

    return out;
}

// ---------------------------------------------------------------------------
// The chains that cross the boundaries between the difficulty rules
// ---------------------------------------------------------------------------

// src/pow.cpp picks between four difficulty rules by the height of the block
// being extended: below multiAlgoDiffChangeTarget the first rule, below
// alwaysUpdateDiffChangeTarget the second, below workComputationChangeTarget the
// third, and the fourth after that. The first rule has a further height check of
// its own at nDiffChangeTarget, and the whole thing is skipped where the chain
// parameters allow minimum-difficulty blocks and either set fEasyPow or the
// candidate block is stamped more than two block spacings after the tip. Each
// chain below crosses one of those boundaries. Only the fourth rule uses the
// pointer-following lookup; the first three already use the walk, and have done
// on every node that ever synced from the beginning of the chain.

struct EraChain {
    std::unique_ptr<TestChain> chain;
    //! Difficulty in compact form that GetNextWorkRequired must return for the
    //! last block of this chain. Rows are sha256d, scrypt, groestl, skein,
    //! qubit, odo. The first column is a candidate block stamped 15 seconds
    //! after the tip, the second one stamped 600 seconds after it.
    uint32_t expected_nbits[6][2];
    //! One number standing for the difficulty of every algorithm at every block
    //! of the chain from the 150th on, with both candidate stamps.
    uint64_t expected_checksum;
};

uint64_t DifficultyChecksum(const TestChain& chain)
{
    uint64_t acc{1469598103934665603ULL};
    // Every block from the 150th on. Starting there leaves enough earlier blocks
    // for the deepest walk any of the rules makes, and sweeping every block means
    // the grid lands on the heights where the first rule actually retargets.
    for (size_t i = 150; i < chain.Size(); ++i) {
        for (const int algo : ALGO_PALETTE) {
            for (const int gap : {NORMAL_GAP, MIN_DIFFICULTY_GAP}) {
                CBlockHeader candidate;
                candidate.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
                candidate.nTime = chain.At(i).nTime + gap;
                const unsigned int bits = GetNextWorkRequired(&chain.At(i), &candidate, chain.Params(), algo);
                acc = (acc ^ static_cast<uint64_t>(bits)) * 1099511628211ULL;
            }
        }
    }
    return acc;
}

std::vector<EraChain> BuildEraChains(const ParamSets& p)
{
    std::vector<EraChain> out;

    // Difficulty below the ceiling of each network, so the arithmetic of the
    // rules has room to move in both directions.
    auto headroom = [](const Consensus::Params& params) {
        arith_uint256 target{UintToArith256(params.powLimit)};
        target >>= 4;
        return target.GetCompact();
    };

    auto build = [](const char* name, const Consensus::Params& params, int start_height, int blocks,
                    uint32_t nbits, uint64_t seed, int min_difficulty_every) {
        auto c = std::make_unique<TestChain>(name, params, start_height, nbits);
        Repeatable rng{seed};
        AddMixedRun(*c, rng, blocks, min_difficulty_every);
        // Every block gets a different difficulty, so the difficulty that comes
        // out of the rules depends on which block the lookup returned and not
        // only on how far back it was.
        c->SpreadOutTheDifficulties();
        return c;
    };

    // Mainnet, across the height where the first rule changes its own retarget
    // interval (67,200).
    out.push_back(EraChain{
        build("mainnet across the DigiShield height", p.main->GetConsensus(), 66900, 500, headroom(p.main->GetConsensus()), 31, 0),
        {{0x1e00bfffU, 0x1e00bfffU}, {0x1e00bfffU, 0x1e00bfffU}, {0x1e00bfffU, 0x1e00bfffU},
         {0x1e00bfffU, 0x1e00bfffU}, {0x1e00bfffU, 0x1e00bfffU}, {0x1e00bfffU, 0x1e00bfffU}},
        11571575791293113127ULL});

    // Mainnet, across the height where the second rule takes over (145,000).
    out.push_back(EraChain{
        build("mainnet across the MultiAlgo height", p.main->GetConsensus(), 144800, 500, headroom(p.main->GetConsensus()), 32, 0),
        {{0x1e019998U, 0x1e019998U}, {0x1d00ccccU, 0x1d00ccccU}, {0x1e033332U, 0x1e033332U},
         {0x1c0cccccU, 0x1c0cccccU}, {0x1e00ccccU, 0x1e00ccccU}, {0x1c333332U, 0x1c333332U}},
        7042509408755104919ULL});

    // Mainnet, across the height where the third rule takes over (400,000).
    out.push_back(EraChain{
        build("mainnet across the MultiShield height", p.main->GetConsensus(), 399800, 500, headroom(p.main->GetConsensus()), 33, 0),
        {{0x1c3faf41U, 0x1c3faf41U}, {0x1d043161U, 0x1d043161U}, {0x1e00c952U, 0x1e00c952U},
         {0x1e0713b0U, 0x1e0713b0U}, {0x1e036700U, 0x1e036700U}, {0x1d023d16U, 0x1d023d16U}},
        17424758129621491835ULL});

    // Mainnet, across the height where the fourth rule takes over (1,430,000).
    // This is the rule that uses the pointer-following lookup.
    out.push_back(EraChain{
        build("mainnet across the DigiSpeed height", p.main->GetConsensus(), 1429800, 500, headroom(p.main->GetConsensus()), 34, 0),
        {{0x1e03b2bdU, 0x1e03b2bdU}, {0x1e01c729U, 0x1e01c729U}, {0x1d050faeU, 0x1d050faeU},
         {0x1d0a8705U, 0x1d0a8705U}, {0x1c2147adU, 0x1c2147adU}, {0x1e00dad3U, 0x1e00dad3U}},
        7947229432914557361ULL});

    // Signet is the one configuration where minimum-difficulty blocks are
    // allowed and the difficulty rules still run, because it does not set
    // fEasyPow. These two chains cross its rule boundaries (100 and 400, then
    // 1,430) with minimum-difficulty blocks in them.
    out.push_back(EraChain{
        build("signet across the MultiAlgo and MultiShield heights, with minimum-difficulty blocks",
              p.signet->GetConsensus(), 0, 500, headroom(p.signet->GetConsensus()), 35, 6),
        {{0x1c3f1e97U, 0x1e0377aeU}, {0x1c03beb1U, 0x1e0377aeU}, {0x1e00dd96U, 0x1e0377aeU},
         {0x1d052009U, 0x1e0377aeU}, {0x1e01cce9U, 0x1e0377aeU}, {0x1d3337eeU, 0x1e0377aeU}},
        11423939312034159409ULL});
    out.push_back(EraChain{
        build("signet across the DigiSpeed height, with minimum-difficulty blocks",
              p.signet->GetConsensus(), 1300, 400, headroom(p.signet->GetConsensus()), 36, 5),
        {{0x1c03337eU, 0x1e0377aeU}, {0x1d021b33U, 0x1e0377aeU}, {0x1d52009fU, 0x1e0377aeU},
         {0x1c0dd977U, 0x1e0377aeU}, {0x1c06a88dU, 0x1e0377aeU}, {0x1c7ca156U, 0x1e0377aeU}},
        12198795262167177552ULL});

    // Regtest sets fEasyPow, so every block there gets the easiest allowed
    // difficulty and none of the four rules runs. Recorded so that stays true.
    out.push_back(EraChain{
        build("regtest across all of its rule heights, with minimum-difficulty blocks",
              p.regtest->GetConsensus(), 0, 700, headroom(p.regtest->GetConsensus()), 37, 6),
        {{0x207fffffU, 0x207fffffU}, {0x207fffffU, 0x207fffffU}, {0x207fffffU, 0x207fffffU},
         {0x207fffffU, 0x207fffffU}, {0x207fffffU, 0x207fffffU}, {0x207fffffU, 0x207fffffU}},
        10980763907835891611ULL});

    return out;
}

/** The regtest parameters selected globally, which is what the difficulty
 *  reported over the wire is computed against. */
struct RegtestParamsSetup : public BasicTestingSetup {
    RegtestParamsSetup() : BasicTestingSetup{ChainType::REGTEST} {}
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(pow_algo_lookup_tests, BasicTestingSetup)

/**
 * A chain short enough to check by hand. The table below says which block the
 * lookup has to return for each algorithm, starting from each block of the
 * chain. It is the same answer both functions must give, written down as
 * heights so it still holds if one of them is removed.
 *
 * The chain, with the gap in seconds from the parent. A gap over 120 seconds
 * makes the block count as mined at the minimum difficulty, which the lookup
 * steps past, and regtest is one of the configurations that allows those:
 *
 *   height  algorithm   gap   minimum difficulty?
 *      0    scrypt        -   no, it has no parent
 *      1    sha256d      15   no
 *      2    scrypt       15   no
 *      3    scrypt      600   yes, one on its own
 *      4    qubit        15   no
 *      5    scrypt       15   no
 *      6    skein       600   yes, and the only skein block so far
 *      7    skein       600   yes, two in a row
 *      8    groestl      15   no
 *      9    scrypt      600   yes
 *     10    scrypt      600   yes
 *     11    scrypt      600   yes, three in a row
 *     12    sha256d      15   no
 *     13    odo         600   yes, and the only odo block in the chain
 *     14    qubit        15   no
 *     15    scrypt       15   no
 *     16    groestl     600   yes
 *     17    skein        15   no
 *     18    scrypt      600   yes
 *     19    sha256d      15   no
 */
BOOST_AUTO_TEST_CASE(worked_example_that_can_be_checked_by_hand)
{
    const auto regtest = CChainParams::RegTest(CChainParams::RegTestOptions{});
    const Consensus::Params& params = regtest->GetConsensus();
    BOOST_REQUIRE(params.fPowAllowMinDifficultyBlocks); // the whole point of using regtest here

    TestChain chain{"worked example", params, 0, 0x207fffff};
    chain.Add(ALGO_SCRYPT, NORMAL_GAP);
    chain.Add(ALGO_SHA256D, NORMAL_GAP);
    chain.Add(ALGO_SCRYPT, NORMAL_GAP);
    chain.Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_QUBIT, NORMAL_GAP);
    chain.Add(ALGO_SCRYPT, NORMAL_GAP);
    chain.Add(ALGO_SKEIN, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_SKEIN, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_GROESTL, NORMAL_GAP);
    chain.Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_SHA256D, NORMAL_GAP);
    chain.Add(ALGO_ODO, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_QUBIT, NORMAL_GAP);
    chain.Add(ALGO_SCRYPT, NORMAL_GAP);
    chain.Add(ALGO_GROESTL, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_SKEIN, NORMAL_GAP);
    chain.Add(ALGO_SCRYPT, MIN_DIFFICULTY_GAP);
    chain.Add(ALGO_SHA256D, NORMAL_GAP);
    BOOST_REQUIRE_EQUAL(chain.Size(), 20U);

    // Rows are the block the lookup starts from, heights 0 to 19. Columns are
    // sha256d, scrypt, groestl, skein, qubit, odo. Each value is the height of
    // the block the lookup must return, or -1 for "no block of that algorithm".
    const int expected[20][6] = {
        /* from  0 */ {-1,  0, -1, -1, -1, -1},
        /* from  1 */ { 1,  0, -1, -1, -1, -1},
        /* from  2 */ { 1,  2, -1, -1, -1, -1},
        /* from  3 */ { 1,  2, -1, -1, -1, -1},
        /* from  4 */ { 1,  2, -1, -1,  4, -1},
        /* from  5 */ { 1,  5, -1, -1,  4, -1},
        /* from  6 */ { 1,  5, -1, -1,  4, -1},
        /* from  7 */ { 1,  5, -1, -1,  4, -1},
        /* from  8 */ { 1,  5,  8, -1,  4, -1},
        /* from  9 */ { 1,  5,  8, -1,  4, -1},
        /* from 10 */ { 1,  5,  8, -1,  4, -1},
        /* from 11 */ { 1,  5,  8, -1,  4, -1},
        /* from 12 */ {12,  5,  8, -1,  4, -1},
        /* from 13 */ {12,  5,  8, -1,  4, -1},
        /* from 14 */ {12,  5,  8, -1, 14, -1},
        /* from 15 */ {12, 15,  8, -1, 14, -1},
        /* from 16 */ {12, 15,  8, -1, 14, -1},
        /* from 17 */ {12, 15,  8, 17, 14, -1},
        /* from 18 */ {12, 15,  8, 17, 14, -1},
        /* from 19 */ {19, 15,  8, 17, 14, -1},
    };

    if (RecordingMode()) {
        for (size_t i = 0; i < chain.Size(); ++i) {
            std::string row;
            for (const int algo : ALGO_PALETTE) {
                row += strprintf("%s%d", row.empty() ? "" : ", ",
                                 HeightOrNone(GetLastBlockIndexForAlgo(&chain.At(i), params, algo)));
            }
            BOOST_TEST_MESSAGE(strprintf("/* from %2d */ {%s},", chain.At(i).nHeight, row));
        }
    }

    for (size_t i = 0; i < chain.Size(); ++i) {
        for (size_t column = 0; column < ALGO_PALETTE.size(); ++column) {
            const int algo = ALGO_PALETTE[column];
            const CBlockIndex* slow = GetLastBlockIndexForAlgo(&chain.At(i), params, algo);
            BOOST_CHECK_MESSAGE(HeightOrNone(slow) == expected[i][column],
                                strprintf("from height %d, algorithm %d: expected height %d, the walk returned %s",
                                          chain.At(i).nHeight, algo, expected[i][column], Describe(slow)));
        }
    }

    // The block mined at the minimum difficulty is the one place the two
    // functions could differ, so this chain has to contain some.
    BOOST_CHECK(CountAnswersTheWrongWalkGetsWrong(chain, 1) > 0);
}

/** The lookup gives the answer the rule says, on every chain, for every block and
 *  every algorithm, including algorithms that never appear in the chain and
 *  algorithm numbers no block could ever have. */
BOOST_AUTO_TEST_CASE(the_lookup_matches_the_rule_on_every_chain)
{
    const ParamSets params;
    for (const auto& entry : BuildCatalogue(params)) {
        CheckTheLookupMatchesTheRule(*entry.chain, entry.stride);
    }
}

/** The answers written down as numbers, recorded while the node still had a
 *  second version of this lookup and the two agreed on every one of them. */
BOOST_AUTO_TEST_CASE(recorded_answers_for_every_chain)
{
    const ParamSets params;
    for (const auto& entry : BuildCatalogue(params)) {
        const uint64_t checksum = AnswerChecksum(*entry.chain, entry.stride);
        const size_t wrong = CountAnswersTheWrongWalkGetsWrong(*entry.chain, entry.stride);
        if (RecordingMode()) {
            BOOST_TEST_MESSAGE(strprintf("%s: checksum %uULL, mistakes by the double-stepping walk %u",
                                         entry.chain->Name(), checksum, wrong));
            continue;
        }
        BOOST_CHECK_MESSAGE(checksum == entry.expected_checksum,
                            strprintf("%s: the lookup answers changed: expected checksum %u, got %u",
                                      entry.chain->Name(), entry.expected_checksum, checksum));
        BOOST_CHECK_MESSAGE(wrong == entry.expected_wrong_walk_mistakes,
                            strprintf("%s: the double-stepping walk got %u answers wrong, expected %u",
                                      entry.chain->Name(), wrong, entry.expected_wrong_walk_mistakes));
    }
}

/**
 * The trap that was avoided by deleting the second version of this lookup rather
 * than rewriting it. A walk that keeps the extra step backwards gets answers
 * wrong, and only where minimum-difficulty blocks are allowed. A check run on
 * mainnet parameters alone would not notice it, which is why the chains above
 * include configurations where those blocks are switched on.
 */
BOOST_AUTO_TEST_CASE(the_double_stepping_rewrite_is_caught)
{
    const ParamSets params;
    size_t chains_with_the_branch_switched_on{0};
    size_t chains_with_mistakes{0};

    for (const auto& entry : BuildCatalogue(params)) {
        const size_t wrong = CountAnswersTheWrongWalkGetsWrong(*entry.chain, entry.stride);
        if (entry.chain->Params().fPowAllowMinDifficultyBlocks) {
            ++chains_with_the_branch_switched_on;
            if (wrong > 0) ++chains_with_mistakes;
        } else {
            // Mainnet and the public testnet never allow those blocks, so the
            // extra step is unreachable there and even the wrong rewrite agrees.
            BOOST_CHECK_MESSAGE(wrong == 0,
                                strprintf("%s: minimum-difficulty blocks are not allowed here, so nothing should differ",
                                          entry.chain->Name()));
        }
    }

    BOOST_CHECK(chains_with_the_branch_switched_on >= 6);
    BOOST_CHECK_MESSAGE(chains_with_mistakes >= 5,
                        strprintf("only %u of the %u chains that allow minimum-difficulty blocks catch the wrong rewrite",
                                  chains_with_mistakes, chains_with_the_branch_switched_on));
}

/**
 * The difficulty itself, across every boundary between the four difficulty
 * rules. The lookups are compared on these chains too, and the difficulty each
 * rule produces is written down as a number, so the recorded values fail if a
 * lookup ever returns a different block.
 */
BOOST_AUTO_TEST_CASE(difficulty_across_the_rule_boundaries)
{
    const ParamSets params;
    for (const auto& era : BuildEraChains(params)) {
        const TestChain& chain = *era.chain;
        CheckTheLookupMatchesTheRule(chain, 1);

        const uint64_t checksum = DifficultyChecksum(chain);
        if (RecordingMode()) {
            std::string rows;
            for (const int algo : ALGO_PALETTE) {
                std::string cells;
                for (const int gap : {NORMAL_GAP, MIN_DIFFICULTY_GAP}) {
                    CBlockHeader candidate;
                    candidate.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
                    candidate.nTime = chain.Tip().nTime + gap;
                    cells += strprintf("%s0x%08xU", cells.empty() ? "" : ", ",
                                       GetNextWorkRequired(&chain.Tip(), &candidate, chain.Params(), algo));
                }
                rows += strprintf("%s{%s}", rows.empty() ? "" : ", ", cells);
            }
            BOOST_TEST_MESSAGE(strprintf("%s:\n  {%s},\n  checksum %uULL", chain.Name(), rows, checksum));
            continue;
        }

        for (size_t row = 0; row < ALGO_PALETTE.size(); ++row) {
            const int algo = ALGO_PALETTE[row];
            for (size_t column = 0; column < 2; ++column) {
                CBlockHeader candidate;
                candidate.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
                candidate.nTime = chain.Tip().nTime + (column == 0 ? NORMAL_GAP : MIN_DIFFICULTY_GAP);
                const unsigned int bits = GetNextWorkRequired(&chain.Tip(), &candidate, chain.Params(), algo);
                BOOST_CHECK_MESSAGE(bits == era.expected_nbits[row][column],
                                    strprintf("%s: algorithm %d with a candidate %d seconds after the tip: expected difficulty 0x%08x, got 0x%08x",
                                              chain.Name(), algo, column == 0 ? NORMAL_GAP : MIN_DIFFICULTY_GAP,
                                              era.expected_nbits[row][column], bits));
            }
        }
        BOOST_CHECK_MESSAGE(checksum == era.expected_checksum,
                            strprintf("%s: the difficulties changed: expected checksum %u, got %u",
                                      chain.Name(), era.expected_checksum, checksum));
    }
}

/**
 * The difficulty the node reports over the wire comes from the same lookup
 * (GetDifficulty in src/rpc/blockchain.cpp, used by getdifficulty and
 * getmininginfo). Here every block is given a different difficulty, so the
 * number reported says which block the lookup found. This check does not name
 * either lookup, so it keeps working when one of them is removed.
 */
BOOST_FIXTURE_TEST_CASE(the_reported_difficulty_comes_from_the_right_block, RegtestParamsSetup)
{
    const Consensus::Params& params = Params().GetConsensus();
    BOOST_REQUIRE(params.fPowAllowMinDifficultyBlocks);

    TestChain chain{"reported difficulty", params, 0, 0x207fffff};
    Repeatable rng{99};
    AddMixedRun(chain, rng, 400, 5);
    chain.SpreadOutTheDifficulties();

    for (size_t i = 0; i < chain.Size(); ++i) {
        for (const int algo : ALGO_PALETTE) {
            const CBlockIndex* expected = ExpectedAnswer(&chain.At(i), params, algo);
            const double reported = GetDifficulty(&chain.At(i), nullptr, algo);
            const double wanted = expected == nullptr
                                      ? GetDifficulty(nullptr, nullptr, algo)
                                      : GetDifficulty(nullptr, expected, algo);
            BOOST_CHECK_MESSAGE(reported == wanted,
                                strprintf("from height %d, algorithm %d: the node reported difficulty %.8f but %s gives %.8f",
                                          chain.At(i).nHeight, algo, reported, Describe(expected), wanted));
        }
    }
}

/**
 * A block whose version names no algorithm at all. The block index reports
 * scrypt for such a block and the per-algorithm pointers are filled in the same
 * way, so both lookups treat it as a scrypt block and agree.
 *
 * The walk decides this from the block index entry, so a block like that counts
 * as a scrypt block for the difficulty of later scrypt blocks. No DigiByte
 * network has ever had one: every version a miner can produce names an
 * algorithm, and the first block of every network is its genesis block, which
 * carries scrypt.
 */
BOOST_AUTO_TEST_CASE(a_block_whose_version_names_no_algorithm)
{
    const auto regtest = CChainParams::RegTest(CChainParams::RegTestOptions{});
    const Consensus::Params& params = regtest->GetConsensus();

    TestChain chain{"a version that names no algorithm", params, 0, 0x207fffff};
    chain.Add(ALGO_SCRYPT, NORMAL_GAP);
    chain.Add(ALGO_QUBIT, NORMAL_GAP);
    chain.AddWithVersion(BLOCK_VERSION_DEFAULT | (10 << 8), NORMAL_GAP); // a slot no algorithm uses
    chain.Add(ALGO_SKEIN, NORMAL_GAP);
    chain.AddWithVersion(BLOCK_VERSION_DEFAULT | (12 << 8), NORMAL_GAP); // another one
    chain.Add(ALGO_SHA256D, NORMAL_GAP);

    // The block index reads those two versions as scrypt.
    BOOST_CHECK_EQUAL(chain.At(2).GetAlgo(), ALGO_SCRYPT);
    BOOST_CHECK_EQUAL(chain.At(4).GetAlgo(), ALGO_SCRYPT);

    // The most recent block read as scrypt, starting from the last block, is the
    // second of the two unreadable ones.
    BOOST_CHECK_EQUAL(HeightOrNone(GetLastBlockIndexForAlgo(&chain.Tip(), params, ALGO_SCRYPT)), 4);
}

/**
 * The recorded difficulties above can tell that a lookup returned a different
 * block. Two chains are built with the same timestamps and the same
 * difficulties, differing only in the algorithm of the last block, so the
 * lookup for scrypt returns the last block on one chain and the one before it on
 * the other. The difficulty that comes out differs.
 *
 * Together with the count of answers the double-stepping walk gets wrong, this
 * is the whole argument: stepping past a minimum-difficulty block wrongly
 * changes which block is returned, and which block is returned changes the
 * difficulty, which is part of what makes a block valid.
 */
BOOST_AUTO_TEST_CASE(the_recorded_difficulties_notice_a_different_block)
{
    const auto signet = CChainParams::SigNet(CChainParams::SigNetOptions{});
    const Consensus::Params& params = signet->GetConsensus();

    auto make = [&params](int algorithm_of_the_last_block) {
        auto chain = std::make_unique<TestChain>("sensitivity", params, 1500, 0x1d00ffff);
        Repeatable rng{5};
        AddMixedRun(*chain, rng, 199, 0);
        chain->Add(algorithm_of_the_last_block, NORMAL_GAP);
        chain->SpreadOutTheDifficulties();
        return chain;
    };
    const auto with_scrypt_last = make(ALGO_SCRYPT);
    const auto with_qubit_last = make(ALGO_QUBIT);

    // The two chains are the same length and carry the same timestamps.
    BOOST_REQUIRE_EQUAL(with_scrypt_last->Size(), with_qubit_last->Size());
    for (size_t i = 0; i < with_scrypt_last->Size(); ++i) {
        BOOST_REQUIRE_EQUAL(with_scrypt_last->At(i).nTime, with_qubit_last->At(i).nTime);
        BOOST_REQUIRE_EQUAL(with_scrypt_last->At(i).nBits, with_qubit_last->At(i).nBits);
    }

    // The lookup for scrypt lands on a different block on each of them.
    const CBlockIndex* found_on_first = GetLastBlockIndexForAlgo(&with_scrypt_last->Tip(), params, ALGO_SCRYPT);
    const CBlockIndex* found_on_second = GetLastBlockIndexForAlgo(&with_qubit_last->Tip(), params, ALGO_SCRYPT);
    BOOST_REQUIRE(found_on_first != nullptr && found_on_second != nullptr);
    BOOST_CHECK(found_on_first->nHeight != found_on_second->nHeight);

    CBlockHeader candidate;
    candidate.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(ALGO_SCRYPT);
    candidate.nTime = with_scrypt_last->Tip().nTime + NORMAL_GAP;
    const unsigned int first = GetNextWorkRequired(&with_scrypt_last->Tip(), &candidate, params, ALGO_SCRYPT);
    const unsigned int second = GetNextWorkRequired(&with_qubit_last->Tip(), &candidate, params, ALGO_SCRYPT);
    BOOST_CHECK_MESSAGE(first != second,
                        strprintf("both chains gave difficulty 0x%08x, so a difficulty check could not tell the two lookups apart", first));
}

BOOST_AUTO_TEST_SUITE_END()
