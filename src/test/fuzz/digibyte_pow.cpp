// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <validation.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>

// Map from algo index [0..4] to algo constant
static constexpr int kAlgos[] = {ALGO_SHA256D, ALGO_SCRYPT, ALGO_SKEIN, ALGO_QUBIT, ALGO_ODO};

static void initialize_digibyte_pow()
{
    SelectParams(ChainType::REGTEST);
}

// Helper: create a uint256 from a 64-bit seed (for unique hashes)
static uint256 MakeHash(uint64_t seed)
{
    uint256 h;
    std::memset(h.data(), 0, 32);
    std::memcpy(h.data(), &seed, sizeof(seed));
    return h;
}

// Helper: build a chain of CBlockIndex entries for a given base height,
// with fuzzed timestamps and cycling through algos.
// Returns pointer to the tip. hashes/chain must outlive usage.
static CBlockIndex* BuildChain(
    FuzzedDataProvider& fdp,
    std::deque<uint256>& hashes,
    std::deque<CBlockIndex>& chain,
    int base_height,
    int chain_len,
    const arith_uint256& powLimitArith,
    bool fuzz_algo_choice)
{
    // Genesis
    chain.emplace_back();
    CBlockIndex& genesis = chain.back();
    hashes.emplace_back();
    genesis.phashBlock = &hashes.back();
    genesis.nHeight = base_height;
    genesis.pprev = nullptr;
    genesis.nTime = 1389388394; // DigiByte genesis time
    genesis.nBits = powLimitArith.GetCompact();
    genesis.nVersion = BLOCK_VERSION_DEFAULT | BLOCK_VERSION_SCRYPT;

    for (int i = 1; i <= chain_len && fdp.remaining_bytes() >= 4; i++) {
        chain.emplace_back();
        CBlockIndex& blk = chain.back();
        CBlockIndex& prev = chain[chain.size() - 2];

        hashes.emplace_back(MakeHash(fdp.ConsumeIntegral<uint64_t>()));
        blk.phashBlock = &hashes.back();
        blk.pprev = &prev;
        blk.nHeight = base_height + i;

        // Fuzzed timestamp: previous time + [1..300] seconds
        uint32_t dt = fdp.ConsumeIntegralInRange<uint32_t>(1, 300);
        blk.nTime = prev.nTime + dt;

        // Rotate through algos, occasionally fuzz the choice
        int algo_idx;
        if (fuzz_algo_choice && fdp.ConsumeBool()) {
            algo_idx = fdp.ConsumeIntegralInRange<int>(0, NUM_ALGOS - 1);
        } else {
            algo_idx = i % NUM_ALGOS;
        }
        int algo = kAlgos[algo_idx];
        blk.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
        blk.nBits = powLimitArith.GetCompact();
    }

    return chain.size() >= 2 ? &chain.back() : nullptr;
}

// --------------------------------------------------------------------------
// fuzz_multishield_v4 — Exercise GetNextWorkRequired dispatching into V4
// with a fuzzed chain of CBlockIndex entries across all 5 algorithms.
// --------------------------------------------------------------------------
FUZZ_TARGET(fuzz_multishield_v4, .init = initialize_digibyte_pow)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const Consensus::Params& params = Params().GetConsensus();
    const arith_uint256 powLimitArith = UintToArith256(params.powLimit);

    if (fdp.remaining_bytes() < 16) return;

    std::deque<uint256> hashes;
    std::deque<CBlockIndex> chain;

    const int chain_len = fdp.ConsumeIntegralInRange<int>(
        NUM_ALGOS * (int)params.nAveragingInterval + 10,
        NUM_ALGOS * (int)params.nAveragingInterval + 100);

    // Build above workComputationChangeTarget so V4 is exercised
    const int base_height = (int)params.workComputationChangeTarget + 1;
    CBlockIndex* tip = BuildChain(fdp, hashes, chain, base_height, chain_len,
                                   powLimitArith, /*fuzz_algo_choice=*/true);
    if (!tip) return;

    // Call GetNextWorkRequired for each algo from the tip
    CBlockHeader header;
    header.nTime = tip->nTime + 15;

    for (int i = 0; i < NUM_ALGOS; i++) {
        int algo = kAlgos[i];
        header.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);

        unsigned int nBits = GetNextWorkRequired(tip, &header, params, algo);

        // Must not exceed powLimit
        arith_uint256 target;
        target.SetCompact(nBits);
        assert(target <= powLimitArith);
        assert(nBits != 0);
    }
}

// --------------------------------------------------------------------------
// fuzz_algo_selection — Fuzz GetAlgo() on CBlockHeader and IsAlgoActive()
// with arbitrary version bits and heights.
// --------------------------------------------------------------------------
FUZZ_TARGET(fuzz_algo_selection, .init = initialize_digibyte_pow)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const Consensus::Params& params = Params().GetConsensus();

    LIMITED_WHILE(fdp.remaining_bytes() >= 8, 5000) {
        int32_t version = fdp.ConsumeIntegral<int32_t>();
        int height = fdp.ConsumeIntegralInRange<int>(0, 20000000);

        // Test CBlockHeader::GetAlgo with arbitrary version
        CBlockHeader header;
        header.nVersion = version;
        int algo = header.GetAlgo();

        // algo should be one of the known constants or ALGO_UNKNOWN
        assert(algo == ALGO_UNKNOWN ||
               algo == ALGO_SHA256D ||
               algo == ALGO_SCRYPT ||
               algo == ALGO_GROESTL ||
               algo == ALGO_SKEIN ||
               algo == ALGO_QUBIT ||
               algo == ALGO_ODO);

        // For known algos, roundtrip: GetVersionForAlgo → GetAlgo should give same algo
        if (algo != ALGO_UNKNOWN && algo != ALGO_GROESTL) {
            CBlockHeader h2;
            h2.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
            assert(h2.GetAlgo() == algo);
        }

        // Test IsAlgoActive with a mock block index
        std::deque<uint256> hashes;
        CBlockIndex idx;
        hashes.emplace_back();
        idx.phashBlock = &hashes.back();
        idx.nHeight = height;
        idx.pprev = nullptr;
        idx.nVersion = BLOCK_VERSION_DEFAULT | BLOCK_VERSION_SCRYPT;

        // IsAlgoActive must not crash for any algo value
        for (int a = 0; a < NUM_ALGOS; a++) {
            (void)IsAlgoActive(&idx, params, kAlgos[a]);
        }
        (void)IsAlgoActive(&idx, params, ALGO_UNKNOWN);
        (void)IsAlgoActive(&idx, params, 99);
        (void)IsAlgoActive(nullptr, params, ALGO_SCRYPT);
    }
}

// --------------------------------------------------------------------------
// fuzz_pow_all_algos — Fuzz CheckProofOfWork for all 5 algorithms
// with fuzzed hashes and nBits values.
// --------------------------------------------------------------------------
FUZZ_TARGET(fuzz_pow_all_algos, .init = initialize_digibyte_pow)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const Consensus::Params& params = Params().GetConsensus();

    LIMITED_WHILE(fdp.remaining_bytes() >= 36, 10000) {
        // Consume a 32-byte hash
        auto hash_bytes = fdp.ConsumeBytes<uint8_t>(32);
        if (hash_bytes.size() < 32) break;
        uint256 hash;
        std::memcpy(hash.data(), hash_bytes.data(), 32);

        unsigned int nBits = fdp.ConsumeIntegral<uint32_t>();

        // CheckProofOfWork must not crash regardless of inputs
        bool valid = CheckProofOfWork(hash, nBits, params);

        // If nBits encodes a target > powLimit, it must be rejected
        arith_uint256 target;
        bool fNeg, fOvf;
        target.SetCompact(nBits, &fNeg, &fOvf);
        if (fNeg || fOvf || target == 0 || target > UintToArith256(params.powLimit)) {
            assert(!valid);
        }

        // If valid, hash must be <= target
        if (valid) {
            assert(UintToArith256(hash) <= target);
        }
    }
}

// --------------------------------------------------------------------------
// fuzz_difficulty_clamping — Test per-algo difficulty clamping bounds in
// V3 and V4 with extreme time deltas.
// --------------------------------------------------------------------------
FUZZ_TARGET(fuzz_difficulty_clamping, .init = initialize_digibyte_pow)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const Consensus::Params& params = Params().GetConsensus();
    const arith_uint256 powLimitArith = UintToArith256(params.powLimit);

    if (fdp.remaining_bytes() < 20) return;

    // Pick which era to test: V3 or V4
    bool test_v4 = fdp.ConsumeBool();
    int base_height = test_v4
        ? (int)params.workComputationChangeTarget + 1
        : (int)params.alwaysUpdateDiffChangeTarget + 1;

    const int window = NUM_ALGOS * (int)params.nAveragingInterval + 20;
    std::deque<uint256> hashes;
    std::deque<CBlockIndex> chain;

    // Fuzzed base time and extreme time delta
    uint32_t base_time = fdp.ConsumeIntegralInRange<uint32_t>(1400000000u, 1800000000u);
    int32_t extreme_dt = fdp.ConsumeIntegralInRange<int32_t>(0, 86400);

    // Genesis
    {
        chain.emplace_back();
        CBlockIndex& genesis = chain.back();
        hashes.emplace_back();
        genesis.phashBlock = &hashes.back();
        genesis.nHeight = base_height;
        genesis.pprev = nullptr;
        genesis.nTime = base_time;
        genesis.nBits = powLimitArith.GetCompact();
        genesis.nVersion = BLOCK_VERSION_DEFAULT | BLOCK_VERSION_SCRYPT;
    }

    // Build the chain with fuzzed timestamps
    for (int i = 1; i <= window && fdp.remaining_bytes() >= 2; i++) {
        chain.emplace_back();
        CBlockIndex& blk = chain.back();
        CBlockIndex& prev = chain[chain.size() - 2];

        hashes.emplace_back(MakeHash((uint64_t)(i * 17 + 3)));
        blk.phashBlock = &hashes.back();
        blk.pprev = &prev;
        blk.nHeight = base_height + i;

        // Time: either extreme_dt or a fuzzed small value
        uint32_t dt = fdp.ConsumeBool()
            ? (uint32_t)extreme_dt
            : fdp.ConsumeIntegralInRange<uint32_t>(1, 15);
        blk.nTime = prev.nTime + dt;

        // Cycle through algos
        int algo_idx = i % NUM_ALGOS;
        int algo = kAlgos[algo_idx];
        blk.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
        blk.nBits = powLimitArith.GetCompact();
    }

    if (chain.size() < 2) return;
    CBlockIndex* tip = &chain.back();

    // Use GetNextWorkRequired which dispatches to V3 or V4 based on height
    CBlockHeader header;
    header.nTime = tip->nTime + 15;

    for (int i = 0; i < NUM_ALGOS; i++) {
        int algo = kAlgos[i];
        header.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);

        unsigned int nBits = GetNextWorkRequired(tip, &header, params, algo);

        // Must not exceed powLimit
        arith_uint256 target;
        target.SetCompact(nBits);
        assert(target <= powLimitArith);
        assert(nBits != 0);
    }
}
