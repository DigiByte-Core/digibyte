// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <kernel/context.h>
#include <memusage.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace {

struct BlockMapTestingSetup : BasicTestingSetup {
    node::KernelNotifications m_notifications{m_node.exit_status};

    BlockMapTestingSetup() : BasicTestingSetup{ChainType::REGTEST} {}

    std::unique_ptr<node::BlockManager> NewBlockManager()
    {
        return std::make_unique<node::BlockManager>(m_node.kernel->interrupt, node::BlockManager::Options{
            .chainparams = Params(),
            .blocks_dir = m_args.GetBlocksDirPath(),
            .notifications = m_notifications,
        });
    }
};

uint256 IndexHash(uint64_t number)
{
    uint256 hash;
    WriteLE64(hash.begin(), number);
    WriteLE64(hash.begin() + 24, ~number);
    return hash;
}

std::string DiskIndexBytes(const CBlockIndex& index)
{
    DataStream stream;
    stream << CDiskBlockIndex{&index};
    return HexStr(stream);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(blockmap_tests, BlockMapTestingSetup)

BOOST_AUTO_TEST_CASE(hash_function_does_not_throw)
{
    // A nonthrowing hash lets the standard library omit a cached hash per node.
    BOOST_CHECK((std::is_nothrow_invocable_r_v<size_t, const BlockHasher&, const uint256&>));
    uint256 hash;
    for (size_t i = 0; i < hash.size(); ++i) hash.begin()[i] = static_cast<uint8_t>(i + 1);
    BOOST_CHECK_EQUAL(BlockHasher{}(hash), static_cast<size_t>(UINT64_C(0x0807060504030201)));
}

BOOST_AUTO_TEST_CASE(map_allocation_budget)
{
    auto blockman = NewBlockManager();
    std::unordered_map<uint256, CBlockIndex, BlockHasher> individually_allocated;
    LOCK(cs_main);
    constexpr size_t count{32768};
    for (size_t i = 0; i < count; ++i) {
        const auto hash = IndexHash(i + 1);
        blockman->InsertBlockIndex(hash);
        individually_allocated.try_emplace(hash);
    }

    const auto pooled_usage = memusage::DynamicUsage(blockman->m_block_index);
    const auto individual_usage = memusage::DynamicUsage(individually_allocated);
    // Include buckets and unused space in the last pool chunk in the budget.
    BOOST_CHECK_MESSAGE(pooled_usage <= individual_usage * 95 / 100,
                        "Block map allocation estimate " << pooled_usage
                        << " exceeds 95% of separate allocations " << individual_usage);
}

BOOST_AUTO_TEST_CASE(hash_collisions_and_growth_preserve_references)
{
    auto blockman = NewBlockManager();
    LOCK(cs_main);
    auto& map = blockman->m_block_index;
    const auto hash = IndexHash(1);
    auto colliding_hash = hash;
    colliding_hash.begin()[31] ^= 1;
    BOOST_REQUIRE_EQUAL(BlockHasher{}(hash), BlockHasher{}(colliding_hash));

    CBlockIndex* parent = blockman->InsertBlockIndex(hash);
    CBlockIndex* child = blockman->InsertBlockIndex(colliding_hash);
    BOOST_REQUIRE(parent != child);
    child->pprev = parent;
    child->pskip = parent;
    child->nHeight = 1;
    parent->SetChainWork(arith_uint256{1} << 200);
    child->SetChainWork(parent->GetChainWork());
    const auto* parent_hash = parent->phashBlock;
    const auto* child_hash = child->phashBlock;
    const std::string parent_bytes = DiskIndexBytes(*parent);
    const std::string child_bytes = DiskIndexBytes(*child);
    std::set<CBlockIndex*, node::CBlockIndexWorkComparator> candidates{parent, child};
    const auto best = *candidates.rbegin();

    const size_t original_buckets = map.bucket_count();
    for (uint64_t i = 2; i <= 8192; ++i) blockman->InsertBlockIndex(IndexHash(i));
    BOOST_REQUIRE(map.bucket_count() > original_buckets);
    map.rehash(map.bucket_count() * 2);

    BOOST_CHECK(blockman->LookupBlockIndex(hash) == parent);
    BOOST_CHECK(blockman->LookupBlockIndex(colliding_hash) == child);
    BOOST_CHECK(blockman->InsertBlockIndex(hash) == parent);
    BOOST_CHECK(blockman->InsertBlockIndex(colliding_hash) == child);
    BOOST_CHECK(parent->phashBlock == parent_hash);
    BOOST_CHECK(child->phashBlock == child_hash);
    BOOST_CHECK(parent->GetBlockHash() == hash);
    BOOST_CHECK(child->GetBlockHash() == colliding_hash);
    BOOST_CHECK(child->GetAncestor(0) == parent);
    BOOST_CHECK(child->GetChainWork() == (arith_uint256{1} << 200));
    BOOST_CHECK_EQUAL(DiskIndexBytes(*parent), parent_bytes);
    BOOST_CHECK_EQUAL(DiskIndexBytes(*child), child_bytes);
    BOOST_CHECK(*candidates.rbegin() == best);
}

BOOST_AUTO_TEST_CASE(managers_own_independent_storage)
{
    auto survivor = NewBlockManager();
    LOCK(cs_main);
    const auto hash = IndexHash(1);
    CBlockIndex* survivor_index = survivor->InsertBlockIndex(hash);
    for (int pass = 0; pass < 3; ++pass) {
        auto temporary = NewBlockManager();
        for (uint64_t i = 1; i <= 4096; ++i) temporary->InsertBlockIndex(IndexHash(i));
        BOOST_CHECK(temporary->LookupBlockIndex(hash) != survivor_index);
        temporary.reset();
        BOOST_CHECK(survivor->LookupBlockIndex(hash) == survivor_index);
        BOOST_CHECK(survivor_index->GetBlockHash() == hash);
        BOOST_CHECK(survivor->InsertBlockIndex(IndexHash(pass + 2)) != nullptr);
    }
}

BOOST_AUTO_TEST_CASE(chainwork_arithmetic_crosses_inline_boundary)
{
    const arith_uint256 first_large = arith_uint256{1} << 112;
    CBlockIndex parent{Params().GenesisBlock()};
    CBlockIndex child{Params().GenesisBlock()};
    child.pprev = &parent;
    child.nHeight = 1;
    const arith_uint256 proof = GetBlockProof(child);
    BOOST_REQUIRE(proof > 0);
    BOOST_REQUIRE(proof < first_large);
    parent.SetChainWork(first_large - proof);
    child.SetChainWork(parent.GetChainWork() + proof);
    BOOST_CHECK(child.GetChainWork() == first_large);
    BOOST_CHECK(child.GetChainWork() - parent.GetChainWork() == proof);

    child.SetChainWork(child.GetChainWork() - proof);
    BOOST_CHECK(child.GetChainWork() == parent.GetChainWork());
    child.SetChainWork(~arith_uint256{0});
    child.SetChainWork(child.GetChainWork() + 1);
    BOOST_CHECK(child.GetChainWork() == 0);
}

BOOST_AUTO_TEST_CASE(candidate_order_preserves_full_chainwork)
{
    const arith_uint256 first_large = arith_uint256{1} << 112;
    const std::array<arith_uint256, 6> values{arith_uint256{0}, first_large - 1, first_large,
                                             first_large + 1, arith_uint256{1} << 200, ~arith_uint256{0}};
    std::array<CBlockIndex, 6> blocks;
    std::set<CBlockIndex*, node::CBlockIndexWorkComparator> candidates;
    const node::CBlockIndexWorkComparator compare;
    for (size_t i = 0; i < blocks.size(); ++i) {
        blocks[i].SetChainWork(values[i]);
        blocks[i].nSequenceId = static_cast<int32_t>(i + 1);
        candidates.insert(&blocks[i]);
    }
    BOOST_REQUIRE_EQUAL(candidates.size(), blocks.size());
    BOOST_CHECK(*candidates.rbegin() == &blocks.back());
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (size_t j = 0; j < blocks.size(); ++j) {
            BOOST_CHECK_EQUAL(compare(&blocks[i], &blocks[j]), i < j);
        }
    }

    CBlockIndex later;
    later.SetChainWork(blocks.back().GetChainWork());
    later.nSequenceId = blocks.back().nSequenceId + 1;
    BOOST_CHECK(compare(&later, &blocks.back()));
    BOOST_CHECK(!compare(&blocks.back(), &later));
}

BOOST_AUTO_TEST_CASE(database_reload_preserves_existing_nodes)
{
    auto blockman = NewBlockManager();
    LOCK(cs_main);
    blockman->m_block_tree_db = std::make_unique<node::BlockTreeDB>(DBParams{
        .path = m_path_root / "blockmap",
        .cache_bytes = 1 << 20,
        .memory_only = true,
    });
    CBlockIndex* best_header{nullptr};
    CBlockIndex* genesis = blockman->AddToBlockIndex(Params().GenesisBlock(), best_header);
    const auto hash = genesis->GetBlockHash();
    const auto* hash_address = genesis->phashBlock;
    const auto bytes = DiskIndexBytes(*genesis);
    BOOST_REQUIRE(blockman->WriteBlockIndexDB());

    for (int pass = 0; pass < 3; ++pass) {
        BOOST_REQUIRE(blockman->LoadBlockIndexDB(std::nullopt));
        BOOST_CHECK_EQUAL(blockman->m_block_index.size(), 1);
        BOOST_CHECK(blockman->LookupBlockIndex(hash) == genesis);
        BOOST_CHECK(genesis->phashBlock == hash_address);
        BOOST_CHECK_EQUAL(DiskIndexBytes(*genesis), bytes);
    }
}

BOOST_AUTO_TEST_SUITE_END()
