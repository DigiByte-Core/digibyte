// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <index/digidollarstatsindex.h>
#include <interfaces/chain.h>
#include <pow.h>
#include <script/script.h>
#include <serialize.h>
#include <test/util/index.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace {

constexpr int DD_HEIGHT{80};
constexpr CAmount OLD_TIP_SUPPLY{987654321};
constexpr CAmount OLD_FORK_SUPPLY{123456789};

// These encodings describe the deployed rows being migrated. The test uses
// public lookups and sync progress for the replacement index format.
struct LegacyStatsValue {
    CAmount supply;
    CAmount collateral;
    uint64_t vaults;

    SERIALIZE_METHODS(LegacyStatsValue, obj)
    {
        READWRITE(obj.supply, obj.collateral, obj.vaults);
    }
};

struct LegacyHeightKey {
    int height;
    uint8_t prefix{'H'};

    template <typename Stream>
    void Serialize(Stream& stream) const
    {
        ser_writedata8(stream, prefix);
        ser_writedata32be(stream, height);
    }
};

struct OpenIndex {
    DigiDollarStatsIndex index;

    explicit OpenIndex(node::NodeContext& node)
        : index{interfaces::MakeChain(node), 1 << 20, false, false} {}

    ~OpenIndex()
    {
        SyncWithValidationInterfaceQueue();
        index.Interrupt();
        index.Stop();
    }
};

struct StatsMigrationSetup : TestChain100Setup {
    StatsMigrationSetup()
        : TestChain100Setup{ChainType::REGTEST,
              {"-digidollaractivationheight=80", "-digidollarstatsindex=0"}} {}

    fs::path IndexPath() const
    {
        return m_args.GetDataDirNet() / "indexes" / "digidollarstats" / "db";
    }

    const CBlockIndex* Tip() const
    {
        LOCK(cs_main);
        return m_node.chainman->ActiveChain().Tip();
    }

    const CBlockIndex* AddLegacyForkHeader(const CBlockIndex& tip)
    {
        CBlockHeader header = tip.GetBlockHeader();
        do {
            ++header.nNonce;
        } while (!CheckProofOfWork(GetPoWAlgoHash(header), header.nBits, Params().GetConsensus()));
        BlockValidationState state;
        const CBlockIndex* fork{nullptr};
        BOOST_REQUIRE_MESSAGE(m_node.chainman->ProcessNewBlockHeaders({header}, true, state, &fork), state.ToString());
        BOOST_REQUIRE(fork);
        BOOST_REQUIRE(fork->GetBlockHash() != tip.GetBlockHash());
        BOOST_REQUIRE_EQUAL(fork->nHeight, tip.nHeight);
        BOOST_REQUIRE(Tip() == &tip);
        return fork;
    }

    void SeedDeployedIndex(const CBlockIndex& tip, const CBlockIndex& fork, bool supply_format_two = false)
    {
        SyncWithValidationInterfaceQueue();
        fs::create_directories(IndexPath().parent_path());
        CDBWrapper old_db{{.path = IndexPath(), .cache_bytes = 1 << 20, .memory_only = false}};
        BOOST_REQUIRE(old_db.StoragePath().has_value());
        const LegacyStatsValue tip_value{OLD_TIP_SUPPLY, 2500 * COIN, 17};
        const LegacyStatsValue fork_value{OLD_FORK_SUPPLY, 1800 * COIN, 13};
        const uint8_t hash_prefix = supply_format_two ? 'd' : 'D';
        const uint8_t height_prefix = supply_format_two ? 'h' : 'H';
        const auto tip_key = std::make_pair(hash_prefix, tip.GetBlockHash());
        const auto fork_key = std::make_pair(hash_prefix, fork.GetBlockHash());
        BOOST_REQUIRE(old_db.Write(LegacyHeightKey{tip.nHeight, height_prefix}, std::make_pair(tip.GetBlockHash(), tip_value)));
        BOOST_REQUIRE(old_db.Write(tip_key, tip_value));
        BOOST_REQUIRE(old_db.Write(fork_key, fork_value));
        CBlockLocator locator;
        locator.vHave = {tip.GetBlockHash(), Params().GetConsensus().hashGenesisBlock};
        BOOST_REQUIRE(old_db.Write(uint8_t{'B'}, locator));
        if (supply_format_two) BOOST_REQUIRE(old_db.Write(uint8_t{'S'}, uint32_t{2}));
        LegacyStatsValue stored;
        BOOST_REQUIRE(old_db.Read(tip_key, stored));
        BOOST_CHECK_EQUAL(stored.supply, OLD_TIP_SUPPLY);
        BOOST_REQUIRE(old_db.Read(fork_key, stored));
        BOOST_CHECK_EQUAL(stored.supply, OLD_FORK_SUPPLY);
    }

    void CheckRebuiltHistory(const DigiDollarStatsIndex& index, const CBlockIndex& tip,
                             const CBlockIndex& fork) const
    {
        std::vector<const CBlockIndex*> blocks;
        {
            LOCK(cs_main);
            BOOST_REQUIRE_EQUAL(Params().GetConsensus().DigiDollarHeight, DD_HEIGHT);
            for (int height = DD_HEIGHT; height <= tip.nHeight; ++height) {
                blocks.push_back(m_node.chainman->ActiveChain()[height]);
            }
        }
        for (const auto* block : blocks) {
            BOOST_REQUIRE(block);
            const auto stats = index.LookUpStats(*block);
            BOOST_REQUIRE(stats);
            BOOST_CHECK_EQUAL(stats->total_dd_supply, 0);
            BOOST_CHECK_EQUAL(stats->total_collateral, 0);
            BOOST_CHECK_EQUAL(stats->vault_count, 0U);
            BOOST_CHECK_EQUAL(stats->height, block->nHeight);
            BOOST_CHECK(stats->block_hash == block->GetBlockHash());
        }
        BOOST_CHECK(!index.LookUpStats(fork));
        BOOST_CHECK(index.GetSummary().best_block_hash == tip.GetBlockHash());
        BOOST_CHECK_EQUAL(index.GetSummary().best_block_height, tip.nHeight);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_stats_migration_tests, StatsMigrationSetup)

BOOST_AUTO_TEST_CASE(deployed_rows_rebuild_from_retained_history_and_reopen)
{
    const CBlockIndex* old_tip = Tip();
    BOOST_REQUIRE(old_tip);
    const CBlockIndex* fork = AddLegacyForkHeader(*old_tip);
    SeedDeployedIndex(*old_tip, *fork);
    const CBlockIndex* new_tip{nullptr};
    {
        OpenIndex opened{m_node};
        auto& index = opened.index;
        BOOST_REQUIRE(index.Init());
        BOOST_CHECK(!index.LookUpStats(*old_tip));
        BOOST_CHECK(!index.LookUpStats(*fork));
        BOOST_CHECK(!index.BlockUntilSyncedToCurrentChain());
        BOOST_CHECK_EQUAL(index.GetSummary().best_block_height, DD_HEIGHT - 1);
        BOOST_REQUIRE(index.StartBackgroundSync());
        IndexWaitSynced(index);
        CheckRebuiltHistory(index, *old_tip, *fork);

        const CScript script{CScript{} << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG};
        CreateAndProcessBlock({}, script);
        BOOST_REQUIRE(index.BlockUntilSyncedToCurrentChain());
        new_tip = Tip();
        BOOST_REQUIRE(new_tip);
        BOOST_REQUIRE(new_tip != old_tip);
        CheckRebuiltHistory(index, *new_tip, *fork);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        index.Stop();
    }
    {
        OpenIndex reopened{m_node};
        BOOST_REQUIRE(reopened.index.Init());
        BOOST_REQUIRE(reopened.index.StartBackgroundSync());
        IndexWaitSynced(reopened.index);
        CheckRebuiltHistory(reopened.index, *new_tip, *fork);
    }
    BOOST_CHECK(Tip() == new_tip);
}

BOOST_AUTO_TEST_CASE(previous_supply_schema_does_not_reuse_unversioned_fork_rows)
{
    const CBlockIndex* tip = Tip();
    BOOST_REQUIRE(tip);
    const CBlockIndex* fork = AddLegacyForkHeader(*tip);
    SeedDeployedIndex(*tip, *fork, true);
    {
        OpenIndex opened{m_node};
        auto& index = opened.index;
        BOOST_REQUIRE(index.Init());
        BOOST_CHECK_EQUAL(index.GetSummary().best_block_height, DD_HEIGHT - 1);
        BOOST_CHECK(!index.LookUpStats(*tip));
        BOOST_CHECK(!index.LookUpStats(*fork));
        BOOST_REQUIRE(index.StartBackgroundSync());
        IndexWaitSynced(index);
        CheckRebuiltHistory(index, *tip, *fork);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        index.Stop();
    }
    {
        OpenIndex reopened{m_node};
        BOOST_REQUIRE(reopened.index.Init());
        BOOST_CHECK_EQUAL(reopened.index.GetSummary().best_block_height, tip->nHeight);
        BOOST_REQUIRE(reopened.index.StartBackgroundSync());
        IndexWaitSynced(reopened.index);
        CheckRebuiltHistory(reopened.index, *tip, *fork);
    }
}

BOOST_AUTO_TEST_CASE(cancelled_initial_migration_resumes_without_legacy_values)
{
    const CBlockIndex* tip = Tip();
    BOOST_REQUIRE(tip);
    const CBlockIndex* fork = AddLegacyForkHeader(*tip);
    SeedDeployedIndex(*tip, *fork);
    int saved_height{-1};
    {
        OpenIndex opened{m_node};
        auto& index = opened.index;
        BOOST_REQUIRE(index.Init());
        BOOST_CHECK_EQUAL(index.GetSummary().best_block_height, DD_HEIGHT - 1);
        {
            // Prevent the worker from traversing the chain until interruption
            // is requested. It can complete at most one append after unlock.
            LOCK(cs_main);
            BOOST_REQUIRE(index.StartBackgroundSync());
            index.Interrupt();
        }
        index.Stop();
        saved_height = index.GetSummary().best_block_height;
        BOOST_CHECK(saved_height >= DD_HEIGHT - 1 && saved_height <= DD_HEIGHT);
        BOOST_CHECK(!index.GetSummary().synced);
        BOOST_CHECK(!index.LookUpStats(*tip));
        BOOST_CHECK(!index.LookUpStats(*fork));
    }
    {
        OpenIndex reopened{m_node};
        BOOST_REQUIRE(reopened.index.Init());
        BOOST_CHECK_EQUAL(reopened.index.GetSummary().best_block_height, saved_height);
        BOOST_CHECK(!reopened.index.LookUpStats(*tip));
        BOOST_CHECK(!reopened.index.LookUpStats(*fork));
        BOOST_REQUIRE(reopened.index.StartBackgroundSync());
        IndexWaitSynced(reopened.index);
        CheckRebuiltHistory(reopened.index, *tip, *fork);
    }
    BOOST_CHECK(Tip() == tip);
}

BOOST_AUTO_TEST_SUITE_END()
