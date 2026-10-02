// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>

namespace {

static constexpr int ALL_ALGOS[NUM_ALGOS] = {ALGO_SHA256D, ALGO_SCRYPT, ALGO_SKEIN, ALGO_QUBIT, ALGO_ODO};

void initialize_multishield_v4()
{
    SelectParams(ChainType::REGTEST);
}

uint256 MakeHash(const uint64_t seed)
{
    uint256 h;
    std::memset(h.data(), 0, 32);
    std::memcpy(h.data(), &seed, sizeof(seed));
    return h;
}

} // namespace

FUZZ_TARGET(fuzz_multishield_v4_phase2a, .init = initialize_multishield_v4)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const Consensus::Params& params = Params().GetConsensus();
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);

    if (fdp.remaining_bytes() < 32) return;

    std::deque<CBlockIndex> chain;
    std::deque<uint256> hashes;

    const int base_height = static_cast<int>(params.workComputationChangeTarget) +
                            NUM_ALGOS * static_cast<int>(params.nAveragingInterval) + 8;
    const int blocks = fdp.ConsumeIntegralInRange<int>(
        NUM_ALGOS * static_cast<int>(params.nAveragingInterval) + 20,
        NUM_ALGOS * static_cast<int>(params.nAveragingInterval) + 120);

    // Seed/genesis node in this synthetic chain fragment.
    chain.emplace_back();
    hashes.emplace_back(MakeHash(1));
    chain.back().phashBlock = &hashes.back();
    chain.back().nHeight = base_height;
    chain.back().nTime = fdp.ConsumeIntegralInRange<uint32_t>(1'400'000'000, 1'900'000'000);
    chain.back().nBits = pow_limit.GetCompact();
    chain.back().nVersion = BLOCK_VERSION_DEFAULT | BLOCK_VERSION_SHA256D;
    chain.back().pprev = nullptr;

    for (int i = 1; i <= blocks && fdp.remaining_bytes() >= 3; ++i) {
        chain.emplace_back();
        hashes.emplace_back(MakeHash(fdp.ConsumeIntegral<uint64_t>()));
        CBlockIndex& prev = chain[chain.size() - 2];
        CBlockIndex& cur = chain.back();

        cur.phashBlock = &hashes.back();
        cur.pprev = &prev;
        cur.nHeight = base_height + i;
        cur.nTime = prev.nTime + fdp.ConsumeIntegralInRange<uint32_t>(1, 1200);

        const int algo_idx = fdp.ConsumeIntegralInRange<int>(0, NUM_ALGOS - 1);
        const int algo = ALL_ALGOS[algo_idx];
        cur.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);

        // Extreme compact target boundary injection.
        if (fdp.ConsumeBool()) {
            cur.nBits = fdp.PickValueInArray<unsigned int>({
                0U,
                1U,
                pow_limit.GetCompact(),
                0x207fffffU,
                0x1d00ffffU,
                std::numeric_limits<uint32_t>::max(),
            });
        } else {
            cur.nBits = pow_limit.GetCompact();
        }
    }

    if (chain.size() < 2) return;
    CBlockIndex* tip = &chain.back();

    CBlockHeader next_header;
    next_header.nTime = tip->nTime + fdp.ConsumeIntegralInRange<uint32_t>(1, 300);

    for (int i = 0; i < NUM_ALGOS; ++i) {
        const int algo = ALL_ALGOS[i];
        next_header.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);

        const unsigned int next_bits = GetNextWorkRequired(tip, &next_header, params, algo);

        arith_uint256 next_target;
        bool neg{false};
        bool ovf{false};
        next_target.SetCompact(next_bits, &neg, &ovf);

        // Invariants: no negative/overflow compact representation and never above powLimit.
        assert(!neg);
        assert(!ovf);
        assert(next_target > 0);
        assert(next_target <= pow_limit);
    }

    // Explicit boundary sanity: MAX_MONEY fence and MAX_TARGET (powLimit) fence.
    assert(MoneyRange(MAX_MONEY));
    assert(!MoneyRange(MAX_MONEY + 1));
    assert(pow_limit <= UintToArith256(params.powLimit));
}
