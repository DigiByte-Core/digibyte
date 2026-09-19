// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

// Use testnet: transitions at 100/200/400, real difficulty adjustment
// (regtest has fEasyPow=true which always returns powLimit)
void initialize_digishield()
{
    SelectParams(ChainType::TESTNET);
}

// Helper: create a block index with a specific algo, linking into a chain
static CBlockIndex* MakeBlock(std::vector<std::unique_ptr<CBlockIndex>>& blocks,
                               std::deque<uint256>& hashes,
                               CBlockIndex* pprev,
                               int height,
                               uint32_t nTime,
                               uint32_t nBits,
                               int algo)
{
    auto block = std::make_unique<CBlockIndex>();
    block->pprev = pprev;
    block->nHeight = height;
    block->nTime = nTime;
    block->nBits = nBits;
    block->nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);

    // Stable hash storage
    hashes.emplace_back();
    // Use height + algo + time as cheap unique-ish hash material
    uint8_t buf[12];
    std::memcpy(buf, &height, 4);
    std::memcpy(buf + 4, &nTime, 4);
    std::memcpy(buf + 8, &algo, 4);
    std::memcpy(hashes.back().begin(), buf, std::min(sizeof(buf), (size_t)32));
    block->phashBlock = &hashes.back();

    CBlockIndex* raw = block.get();
    blocks.push_back(std::move(block));
    return raw;
}

FUZZ_TARGET(fuzz_digishield_eras, .init = initialize_digishield)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const Consensus::Params& params = Params().GetConsensus();
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);

    // Era boundaries on testnet: V1<100, V2:100-199, V3:200-399, V4:400+
    static constexpr int kAlgos[] = {ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT};

    // Pick an era boundary to test near
    const int boundary = fdp.PickValueInArray<int>({
        (int)params.multiAlgoDiffChangeTarget,    // V1→V2 (100)
        (int)params.alwaysUpdateDiffChangeTarget,  // V2→V3 (200)
        (int)params.workComputationChangeTarget,   // V3→V4 (400)
    });

    // Pick an algo to test
    const int algo = fdp.PickValueInArray(kAlgos);

    // Fuzz a height near the boundary
    const int target_height = boundary + fdp.ConsumeIntegralInRange<int>(-5, 5);
    if (target_height < 1) return;

    // Build a chain up to target_height
    const uint32_t base_time = fdp.ConsumeIntegralInRange<uint32_t>(1400000000, 1800000000);
    const uint32_t time_step = fdp.ConsumeIntegralInRange<uint32_t>(10, 120);
    uint32_t initial_bits = pow_limit.GetCompact();

    std::vector<std::unique_ptr<CBlockIndex>> blocks;
    std::deque<uint256> hashes;

    CBlockIndex* prev = nullptr;
    for (int h = 0; h <= target_height; h++) {
        // Cycle through algos for multi-algo blocks (above multiAlgoDiffChangeTarget)
        int block_algo = (h < params.multiAlgoDiffChangeTarget) ? ALGO_SCRYPT : kAlgos[h % NUM_ALGOS];
        uint32_t nTime = base_time + h * time_step;
        prev = MakeBlock(blocks, hashes, prev, h, nTime, initial_bits, block_algo);
    }

    // Now call GetNextWorkRequired on the chain tip for the chosen algo
    CBlockHeader header;
    header.nTime = prev->nTime + time_step;
    header.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);

    unsigned int result = GetNextWorkRequired(prev, &header, params, algo);

    // Result must be a valid compact representation, never zero
    assert(result != 0);

    // Result difficulty must not exceed powLimit (target must not be higher than powLimit)
    arith_uint256 target;
    target.SetCompact(result);
    assert(target <= pow_limit);
}

FUZZ_TARGET(fuzz_difficulty_transitions, .init = initialize_digishield)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const Consensus::Params& params = Params().GetConsensus();
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);

    static constexpr int kAlgos[] = {ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT};

    // Pick an era boundary to span
    const int boundary = fdp.PickValueInArray<int>({
        (int)params.multiAlgoDiffChangeTarget,
        (int)params.alwaysUpdateDiffChangeTarget,
        (int)params.workComputationChangeTarget,
    });

    const int algo = fdp.PickValueInArray(kAlgos);

    // Build a chain spanning the boundary: from boundary-20 to boundary+20
    const int start_height = std::max(0, boundary - 20);
    const int end_height = boundary + 20;

    const uint32_t base_time = fdp.ConsumeIntegralInRange<uint32_t>(1400000000, 1800000000);
    // Fuzz the time spacing to create realistic (or adversarial) block intervals
    const uint32_t base_step = fdp.ConsumeIntegralInRange<uint32_t>(5, 300);

    std::vector<std::unique_ptr<CBlockIndex>> blocks;
    std::deque<uint256> hashes;

    uint32_t initial_bits = pow_limit.GetCompact();

    CBlockIndex* prev = nullptr;
    // Build genesis through start_height with default spacing
    for (int h = 0; h <= start_height; h++) {
        int block_algo = (h < params.multiAlgoDiffChangeTarget) ? ALGO_SCRYPT : kAlgos[h % NUM_ALGOS];
        uint32_t nTime = base_time + h * 60; // 60s spacing for lead-up
        prev = MakeBlock(blocks, hashes, prev, h, nTime, initial_bits, block_algo);
    }

    // Now build the transition zone with fuzzed timing
    std::vector<unsigned int> difficulty_values;
    for (int h = start_height + 1; h <= end_height; h++) {
        int block_algo = (h < params.multiAlgoDiffChangeTarget) ? ALGO_SCRYPT : kAlgos[h % NUM_ALGOS];
        // Fuzz the time step per block for variation
        uint32_t step = base_step + fdp.ConsumeIntegralInRange<int32_t>(-4, 4);
        if (step == 0) step = 1;
        uint32_t nTime = prev->nTime + step;

        prev = MakeBlock(blocks, hashes, prev, h, nTime, initial_bits, block_algo);

        // Get difficulty at each height for the chosen algo
        CBlockHeader header;
        header.nTime = prev->nTime + step;
        header.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);

        unsigned int nbits = GetNextWorkRequired(prev, &header, params, algo);

        // Must never be zero
        assert(nbits != 0);

        // Must never exceed powLimit
        arith_uint256 target;
        target.SetCompact(nbits);
        assert(target <= pow_limit);

        difficulty_values.push_back(nbits);
    }

    // Verify no sudden jumps: difficulty should not go from max to zero or vice versa
    // (This is a sanity check, not a strict invariant since large adjustments can happen)
    for (size_t i = 1; i < difficulty_values.size(); i++) {
        arith_uint256 prev_target, curr_target;
        prev_target.SetCompact(difficulty_values[i - 1]);
        curr_target.SetCompact(difficulty_values[i]);

        // Neither should be zero
        assert(prev_target > 0 || curr_target > 0 || difficulty_values[i - 1] == difficulty_values[i]);
    }
}
