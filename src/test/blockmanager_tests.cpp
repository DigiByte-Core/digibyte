// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <chainparams.h>
#include <clientversion.h>
#include <kernel/context.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/kernel_notifications.h>
#include <node/interface_ui.h>
#include <script/solver.h>
#include <primitives/block.h>
#include <timedata.h>
#include <util/chaintype.h>
#include <util/signalinterrupt.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>
#include <boost/signals2/connection.hpp>
#include <test/util/logging.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>

#include <fstream>
#include <functional>

using node::BLOCK_SERIALIZATION_HEADER_SIZE;
using node::BlockManager;
using node::KernelNotifications;
using node::MAX_BLOCKFILE_SIZE;

// use BasicTestingSetup here for the data directory configuration, setup, and cleanup
BOOST_FIXTURE_TEST_SUITE(blockmanager_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(block_index_progress_large_chain)
{
    constexpr int total{24203363};
    BOOST_CHECK_EQUAL(node::BlockIndexProgressPercent(0, total), 0);
    BOOST_CHECK_EQUAL(node::BlockIndexProgressPercent(20000000, total), 82);
    BOOST_CHECK_EQUAL(node::BlockIndexProgressPercent(22000000, total), 90);
    BOOST_CHECK_EQUAL(node::BlockIndexProgressPercent(24000000, total), 99);
    BOOST_CHECK_EQUAL(node::BlockIndexProgressPercent(total, total), 100);
    BOOST_CHECK_EQUAL(node::BlockIndexProgressPercent(0, 0), 100);
}

BOOST_AUTO_TEST_CASE(block_index_load_cancelled_before_empty_scan)
{
    const auto params{CreateChainParams(m_args, ChainType::REGTEST)};
    node::BlockTreeDB db{{.path = m_args.GetDataDirBase() / "cancelled-index", .cache_bytes = 1 << 20, .memory_only = true}};
    util::SignalInterrupt interrupt;
    interrupt();
    bool inserted{false};
    const auto insert = [&](const uint256&) -> CBlockIndex* {
        inserted = true;
        return nullptr;
    };
    LOCK(cs_main);
    BOOST_CHECK(!db.LoadBlockIndexGuts(params->GetConsensus(), insert, interrupt));
    BOOST_CHECK(!inserted);
}

BOOST_AUTO_TEST_CASE(block_index_load_cancelled_at_last_record)
{
    const auto params{CreateChainParams(m_args, ChainType::REGTEST)};
    node::BlockTreeDB db{{.path = m_args.GetDataDirBase() / "last-record-index", .cache_bytes = 1 << 20, .memory_only = true}};
    const auto genesis_hash = params->GenesisBlock().GetHash();
    CBlockIndex genesis{params->GenesisBlock()};
    genesis.phashBlock = &genesis_hash;
    util::SignalInterrupt interrupt;
    CBlockIndex loaded;
    loaded.phashBlock = &genesis_hash;
    std::vector<std::string> progress;
    boost::signals2::scoped_connection connection{uiInterface.InitMessage_connect([&](const std::string& message) {
        progress.push_back(message);
    })};
    const auto insert = [&](const uint256& hash) -> CBlockIndex* {
        if (hash.IsNull()) {
            interrupt();
            return nullptr;
        }
        BOOST_REQUIRE(hash == genesis_hash);
        return &loaded;
    };
    LOCK(cs_main);
    BOOST_REQUIRE(db.WriteBatchSync({}, 0, {&genesis}));
    BOOST_CHECK(!db.LoadBlockIndexGuts(params->GetConsensus(), insert, interrupt));
    for (const auto& message : progress) {
        BOOST_CHECK(message.find("Loaded block index") == std::string::npos);
    }
    // Cancellation preserves the loaded record and allows a complete retry.
    interrupt.reset();
    const auto retry_insert = [&](const uint256& hash) -> CBlockIndex* {
        return hash.IsNull() ? nullptr : &loaded;
    };
    BOOST_CHECK(db.LoadBlockIndexGuts(params->GetConsensus(), retry_insert, interrupt));
    BOOST_CHECK(loaded.GetBlockHeader().GetHash() == genesis_hash);
}

BOOST_AUTO_TEST_CASE(blockmanager_find_block_pos)
{
    const auto params {CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{m_node.exit_status};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
    };
    BlockManager blockman{m_node.kernel->interrupt, blockman_opts};
    // simulate adding a genesis block normally
    BOOST_CHECK_EQUAL(blockman.SaveBlockToDisk(params->GenesisBlock(), 0, nullptr).nPos, BLOCK_SERIALIZATION_HEADER_SIZE);
    // simulate what happens during reindex
    // simulate a well-formed genesis block being found at offset 8 in the blk00000.dat file
    // the block is found at offset 8 because there is an 8 byte serialization header
    // consisting of 4 magic bytes + 4 length bytes before each block in a well-formed blk file.
    FlatFilePos pos{0, BLOCK_SERIALIZATION_HEADER_SIZE};
    BOOST_CHECK_EQUAL(blockman.SaveBlockToDisk(params->GenesisBlock(), 0, &pos).nPos, BLOCK_SERIALIZATION_HEADER_SIZE);
    // now simulate what happens after reindex for the first new block processed
    // the actual block contents don't matter, just that it's a block.
    // verify that the write position is at offset 0x12d.
    // this is a check to make sure that https://github.com/digibyte/digibyte/issues/21379 does not recur
    // 8 bytes (for serialization header) + 285 (for serialized genesis block) = 293
    // add another 8 bytes for the second block's serialization header and we get 293 + 8 = 301
    FlatFilePos actual{blockman.SaveBlockToDisk(params->GenesisBlock(), 1, nullptr)};
    BOOST_CHECK_EQUAL(actual.nPos, BLOCK_SERIALIZATION_HEADER_SIZE + ::GetSerializeSize(params->GenesisBlock(), CLIENT_VERSION) + BLOCK_SERIALIZATION_HEADER_SIZE);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_scan_unlink_already_pruned_files, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    const auto& chainman = Assert(m_node.chainman);
    auto& blockman = chainman->m_blockman;
    const CBlockIndex* old_tip{WITH_LOCK(chainman->GetMutex(), return chainman->ActiveChain().Tip())};
    WITH_LOCK(chainman->GetMutex(), blockman.GetBlockFileInfo(old_tip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    // Prune the older block file, but don't unlink it
    int file_number;
    {
        LOCK(chainman->GetMutex());
        file_number = old_tip->GetBlockPos().nFile;
        blockman.PruneOneBlockFile(file_number);
    }

    const FlatFilePos pos(file_number, 0);

    // Check that the file is not unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // if m_have_pruned is not yet set
    WITH_LOCK(chainman->GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(!blockman.OpenBlockFile(pos, true).IsNull());

    // Check that the file is unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // once m_have_pruned is set
    blockman.m_have_pruned = true;
    WITH_LOCK(chainman->GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(blockman.OpenBlockFile(pos, true).IsNull());

    // Check that calling with already pruned files doesn't cause an error
    WITH_LOCK(chainman->GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());

    // Check that the new tip file has not been removed
    const CBlockIndex* new_tip{WITH_LOCK(chainman->GetMutex(), return chainman->ActiveChain().Tip())};
    BOOST_CHECK_NE(old_tip, new_tip);
    const int new_file_number{WITH_LOCK(chainman->GetMutex(), return new_tip->GetBlockPos().nFile)};
    const FlatFilePos new_pos(new_file_number, 0);
    BOOST_CHECK(!blockman.OpenBlockFile(new_pos, true).IsNull());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_availability, TestChain100Setup)
{
    // The goal of the function is to return the first not pruned block in the range [upper_block, lower_block].
    LOCK(::cs_main);
    auto& chainman = m_node.chainman;
    auto& blockman = chainman->m_blockman;
    const CBlockIndex& tip = *chainman->ActiveTip();

    // Function to prune all blocks from 'last_pruned_block' down to the genesis block
    const auto& func_prune_blocks = [&](CBlockIndex* last_pruned_block)
    {
        LOCK(::cs_main);
        CBlockIndex* it = last_pruned_block;
        while (it != nullptr && it->nStatus & BLOCK_HAVE_DATA) {
            it->nStatus &= ~BLOCK_HAVE_DATA;
            it = it->pprev;
        }
    };

    // 1) Return genesis block when all blocks are available
    BOOST_CHECK_EQUAL(blockman.GetFirstStoredBlock(tip), chainman->ActiveChain()[0]);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *chainman->ActiveChain()[0]));

    // 2) Check lower_block when all blocks are available
    CBlockIndex* lower_block = chainman->ActiveChain()[tip.nHeight / 2];
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *lower_block));

    // Prune half of the blocks
    int height_to_prune = tip.nHeight / 2;
    CBlockIndex* first_available_block = chainman->ActiveChain()[height_to_prune + 1];
    CBlockIndex* last_pruned_block = first_available_block->pprev;
    func_prune_blocks(last_pruned_block);

    // 3) The last block not pruned is in-between upper-block and the genesis block
    BOOST_CHECK_EQUAL(blockman.GetFirstStoredBlock(tip), first_available_block);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *first_available_block));
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *last_pruned_block));
}

BOOST_AUTO_TEST_CASE(blockmanager_flush_block_file)
{
    KernelNotifications notifications{m_node.exit_status};
    node::BlockManager::Options blockman_opts{
        .chainparams = Params(),
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
    };
    BlockManager blockman{m_node.kernel->interrupt, blockman_opts};

    // Test blocks with no transactions, not even a coinbase
    CBlock block1;
    block1.nVersion = 1;
    CBlock block2;
    block2.nVersion = 2;
    CBlock block3;
    block3.nVersion = 3;

    // They are 80 bytes header + 1 byte 0x00 for vtx length
    constexpr int TEST_BLOCK_SIZE{81};

    // Blockstore is empty
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), 0);

    // Write the first block; dbp=nullptr means this block doesn't already have a disk
    // location, so allocate a free location and write it there.
    FlatFilePos pos1{blockman.SaveBlockToDisk(block1, /*nHeight=*/1, /*dbp=*/nullptr)};

    // Write second block
    FlatFilePos pos2{blockman.SaveBlockToDisk(block2, /*nHeight=*/2, /*dbp=*/nullptr)};

    // Two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + BLOCK_SERIALIZATION_HEADER_SIZE) * 2);

    // First two blocks are written as expected
    // Errors are expected because block data is junk, thrown AFTER successful read
    CBlock read_block;
    BOOST_CHECK_EQUAL(read_block.nVersion, 0);
    {
        ASSERT_DEBUG_LOG("ReadBlockFromDisk: Errors in block header");
        BOOST_CHECK(!blockman.ReadBlockFromDisk(read_block, pos1));
        BOOST_CHECK_EQUAL(read_block.nVersion, 1);
    }
    {
        ASSERT_DEBUG_LOG("ReadBlockFromDisk: Errors in block header");
        BOOST_CHECK(!blockman.ReadBlockFromDisk(read_block, pos2));
        BOOST_CHECK_EQUAL(read_block.nVersion, 2);
    }

    // When FlatFilePos* dbp is given, SaveBlockToDisk() will not write or
    // overwrite anything to the flat file block storage. It will, however,
    // update the blockfile metadata. This is to facilitate reindexing
    // when the user has the blocks on disk but the metadata is being rebuilt.
    // Verify this behavior by attempting (and failing) to write block 3 data
    // to block 2 location.
    CBlockFileInfo* block_data = blockman.GetBlockFileInfo(0);
    BOOST_CHECK_EQUAL(block_data->nBlocks, 2);
    BOOST_CHECK(blockman.SaveBlockToDisk(block3, /*nHeight=*/3, /*dbp=*/&pos2) == pos2);
    // Metadata is updated...
    BOOST_CHECK_EQUAL(block_data->nBlocks, 3);
    // ...but there are still only two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + BLOCK_SERIALIZATION_HEADER_SIZE) * 2);

    // Block 2 was not overwritten:
    //   SaveBlockToDisk() did not call WriteBlockToDisk() because `FlatFilePos* dbp` was non-null
    blockman.ReadBlockFromDisk(read_block, pos2);
    BOOST_CHECK_EQUAL(read_block.nVersion, 2);
}

struct ZeroFloorPruneSetup : ChainTestingSetup {
    explicit ZeroFloorPruneSetup(uint64_t prune_target = BlockManager::PRUNE_TARGET_MANUAL)
        : ChainTestingSetup(ChainType::REGTEST, {"-fastprune"})
    {
        const auto chainman_options = m_node.chainman->m_options;
        const BlockManager::Options blockman_options{
            .chainparams = Params(),
            .prune_target = prune_target,
            .fast_prune = true,
            .blocks_dir = m_args.GetBlocksDirPath(),
            .notifications = *m_node.notifications,
        };
        m_node.chainman = std::make_unique<ChainstateManager>(
            m_node.kernel->interrupt, chainman_options, blockman_options);
        LoadVerifyActivateChainstate();
    }

    void CheckEarlyFileRetention(bool keep_lock, bool automatic = false)
    {
        auto& chainman = *m_node.chainman;
        auto& chainstate = chainman.ActiveChainstate();
        auto& blockman = chainman.m_blockman;
        BlockManagerTest prune_lock{blockman, "digidollar"};
        auto* genesis = WITH_LOCK(cs_main, return chainstate.m_chain.Genesis());
        BOOST_REQUIRE(genesis);
        const FlatFilePos early_pos = WITH_LOCK(cs_main, return genesis->GetBlockPos());
        BOOST_REQUIRE_EQUAL(early_pos.nFile, 0);

        // Close the first file before the next save, as the existing storage
        // fixture does. Keep its actual size and height range after that save.
        unsigned int early_size;
        {
            LOCK(cs_main);
            BOOST_REQUIRE_EQUAL(prune_lock.PruneLock().height_first, 0);
            auto* early = blockman.GetBlockFileInfo(early_pos.nFile);
            BOOST_REQUIRE_EQUAL(early->nHeightFirst, 0U);
            BOOST_REQUIRE_EQUAL(early->nHeightLast, 0U);
            early_size = early->nSize;
            early->nSize = 0x10000;
        }
        auto blocks = CreateBlockChain(automatic ? Params().PruneAfterHeight() + 1 : 1, Params());
        for (auto& block : blocks) BOOST_REQUIRE(!MineBlock(m_node, block).IsNull());
        LOCK(cs_main);
        const auto later_pos = chainstate.m_chain.Tip()->GetBlockPos();
        BOOST_REQUIRE_EQUAL(later_pos.nFile, 1);
        {
            blockman.GetBlockFileInfo(early_pos.nFile)->nSize = early_size;
        }

        CBlock read;
        BOOST_REQUIRE(blockman.ReadBlockFromDisk(read, *genesis));
        BOOST_REQUIRE(read.GetHash() == genesis->GetBlockHash());
        if (!keep_lock) blockman.UpdatePruneLock("digidollar", {});

        if (automatic) {
            // Account for disk pressure in the closed file's metadata without
            // allocating a large physical file for this storage test.
            struct RestoreSize {
                BlockManager& blockman;
                int file;
                unsigned int size;
                ~RestoreSize() {
                    auto* info = blockman.GetBlockFileInfo(file);
                    if (info->nSize) info->nSize = size;
                }
            } restore_size{blockman, early_pos.nFile, early_size};
            blockman.GetBlockFileInfo(early_pos.nFile)->nSize = MIN_DISK_SPACE_FOR_BLOCK_FILES;
            chainstate.PruneAndFlush();
        } else {
            BlockValidationState state;
            BOOST_REQUIRE_MESSAGE(chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS, 1), state.ToString());
        }
        BOOST_CHECK_EQUAL(bool(genesis->nStatus & BLOCK_HAVE_DATA), keep_lock);
        BOOST_CHECK_EQUAL(blockman.ReadBlockFromDisk(read, early_pos), keep_lock);
        BOOST_CHECK_EQUAL(fs::exists(m_args.GetBlocksDirPath() / "blk00000.dat"), keep_lock);
    }
};

BOOST_FIXTURE_TEST_CASE(blockmanager_zero_floor_retains_early_file, ZeroFloorPruneSetup)
{
    CheckEarlyFileRetention(true);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_early_file_is_prunable_without_retention_lock, ZeroFloorPruneSetup)
{
    CheckEarlyFileRetention(false);
}

struct ZeroFloorAutomaticPruneSetup : ZeroFloorPruneSetup {
    ZeroFloorAutomaticPruneSetup() : ZeroFloorPruneSetup(MIN_DISK_SPACE_FOR_BLOCK_FILES) {}
};

BOOST_FIXTURE_TEST_CASE(blockmanager_zero_floor_retains_early_file_automatically, ZeroFloorAutomaticPruneSetup)
{
    CheckEarlyFileRetention(true, true);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_early_file_is_automatically_prunable_without_retention_lock, ZeroFloorAutomaticPruneSetup)
{
    CheckEarlyFileRetention(false, true);
}

namespace {
class FlushRecordingNotifications final : public kernel::Notifications {
public:
    unsigned flush_errors{0};
    unsigned fatal_errors{0};
    void flushError(const std::string&) override { ++flush_errors; }
    void fatalError(const std::string&, const bilingual_str&) override { ++fatal_errors; }
};

/** Make only one temporary test path unusable, restoring it on every exit. */
class ScopedPathObstruction {
    const fs::path m_path;
    const fs::path m_saved;
public:
    explicit ScopedPathObstruction(const fs::path& path)
        : m_path{path}, m_saved{path + ".saved"}
    {
        const bool directory = fs::is_directory(path);
        if (fs::exists(m_saved)) throw std::runtime_error("Test backup path already exists");
        fs::rename(m_path, m_saved);
        try {
            if (directory) {
                std::ofstream obstruction{m_path};
                if (!obstruction) throw std::runtime_error("Cannot obstruct test directory");
            } else {
                fs::create_directory(m_path);
            }
        } catch (...) {
            fs::remove(m_path);
            fs::rename(m_saved, m_path);
            throw;
        }
    }
    ~ScopedPathObstruction()
    {
        std::error_code error;
        fs::remove(m_path, error);
        fs::rename(m_saved, m_path, error);
        assert(!error);
    }
};

class ScopedBlockSaveListener final : public CValidationInterface {
    const std::function<void()> m_callback;
    void NewPoWValidBlock(const CBlockIndex*, const std::shared_ptr<const CBlock>&) override
    {
        m_callback();
    }
public:
    explicit ScopedBlockSaveListener(std::function<void()> callback) : m_callback{std::move(callback)}
    {
        RegisterValidationInterface(this);
    }
    ~ScopedBlockSaveListener() { UnregisterValidationInterface(this); }
};

struct BlockFileDurabilitySetup : ChainTestingSetup {
    FlushRecordingNotifications notifications;
    std::unique_ptr<ChainstateManager> manager;
    CBlockIndex* genesis{nullptr};
    CBlockIndex* assumed{nullptr};
    uint32_t next_nonce{1000000};

    BlockFileDurabilitySetup() : ChainTestingSetup{ChainType::REGTEST}
    {
        const ChainstateManager::Options chain_options{
            .chainparams = Params(),
            .datadir = m_args.GetDataDirNet() / "flush-durability",
            .adjusted_time_callback = GetAdjustedTime,
            .check_block_index = false,
            .notifications = notifications,
        };
        const BlockManager::Options block_options{
            .chainparams = Params(),
            .blocks_dir = chain_options.datadir / "blocks",
            .notifications = notifications,
        };
        fs::create_directories(block_options.blocks_dir);
        manager = std::make_unique<ChainstateManager>(m_node.kernel->interrupt, chain_options, block_options);
        auto& blockman = manager->m_blockman;
        LOCK(cs_main);
        blockman.m_block_tree_db = std::make_unique<node::BlockTreeDB>(DBParams{
            .path = block_options.blocks_dir / "index", .cache_bytes = 1 << 20});
        auto& chainstate = manager->InitializeChainstate(nullptr, nullptr);
        chainstate.InitCoinsDB(1 << 20, /*in_memory=*/true, /*should_wipe=*/true);
        chainstate.InitCoinsCache(1 << 20);
        BOOST_REQUIRE(chainstate.LoadGenesisBlock());
        genesis = blockman.LookupBlockIndex(Params().GenesisBlock().GetHash());
        BOOST_REQUIRE(genesis);
        chainstate.CoinsTip().SetBestBlock(genesis->GetBlockHash());
        BOOST_REQUIRE(chainstate.LoadChainTip());
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS));
    }

    void AddAssumedFile() EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        auto& blockman = manager->m_blockman;
        blockman.m_snapshot_height = 1;
        // These are storage records. Block acceptance is not under test here.
        CBlock block = Params().GenesisBlock();
        block.hashPrevBlock = genesis->GetBlockHash();
        ++block.nTime;
        ++block.nNonce;
        const auto pos = blockman.SaveBlockToDisk(block, 1, nullptr);
        BOOST_REQUIRE(!pos.IsNull());
        BOOST_REQUIRE_NE(pos.nFile, genesis->nFile);
        assumed = blockman.AddToBlockIndex(block, manager->m_best_header);
        manager->ReceivedBlockTransactions(block, assumed, pos);
        SelectTip(true);
        BlockValidationState state;
        BOOST_REQUIRE(manager->ActiveChainstate().FlushStateToDisk(state, FlushStateMode::ALWAYS));
    }

    void SelectTip(bool use_assumed) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        auto& chainstate = manager->ActiveChainstate();
        auto* tip = use_assumed ? assumed : genesis;
        chainstate.m_chain.SetTip(*tip);
        chainstate.CoinsTip().SetBestBlock(tip->GetBlockHash());
    }

    CBlockHeader StageWrites(unsigned header_count = 1) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        auto& blockman = manager->m_blockman;
        BOOST_REQUIRE(!blockman.SaveBlockToDisk(Params().GenesisBlock(), 0, nullptr).IsNull());
        CBlockHeader header = Params().GenesisBlock();
        header.hashPrevBlock = genesis->GetBlockHash();
        ++header.nTime;
        for (unsigned i = 0; i < header_count; ++i) {
            header.nNonce = next_nonce++;
            blockman.AddToBlockIndex(header, manager->m_best_header);
        }
        return header;
    }

    void CheckFileFailure(bool assumed_tip, bool fail_assumed, bool undo, FlushStateMode mode,
                          unsigned header_count = 1) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        SelectTip(assumed_tip);
        auto& blockman = manager->m_blockman;
        auto& db = *blockman.m_block_tree_db;
        const CBlockHeader header = StageWrites(header_count);
        const uint256 hash = header.GetHash();
        CBlockFileInfo before;
        BOOST_REQUIRE(db.ReadBlockFileInfo(genesis->nFile, before));
        const int file = fail_assumed ? assumed->nFile : genesis->nFile;
        const auto path = undo ? fs::path{blockman.GetBlockPosFilename(FlatFilePos{file, 0}).parent_path()} / fs::u8path(strprintf("rev%05u.dat", file)) :
                                 blockman.GetBlockPosFilename(FlatFilePos{file, 0});
        BOOST_REQUIRE(fs::is_regular_file(path));
        notifications.flush_errors = notifications.fatal_errors = 0;
        {
            ScopedPathObstruction obstruction{path};
            BlockValidationState state;
            BOOST_CHECK(!manager->ActiveChainstate().FlushStateToDisk(state, mode));
            BOOST_CHECK(state.IsError());
            BOOST_CHECK(!state.IsInvalid());
            BOOST_CHECK_GT(notifications.flush_errors, 0U);
            BOOST_CHECK_EQUAL(notifications.fatal_errors, 1U);
            BOOST_CHECK(!db.Exists(std::make_pair(uint8_t{'b'}, hash)));
            CBlockFileInfo after;
            BOOST_REQUIRE(db.ReadBlockFileInfo(genesis->nFile, after));
            BOOST_CHECK_EQUAL(after.nBlocks, before.nBlocks);
            BOOST_CHECK(blockman.LookupBlockIndex(hash)->GetBlockHeader().GetHash() == hash);
            if (mode == FlushStateMode::NONE) BOOST_CHECK(blockman.HeaderCacheNeedsFlush());
        }
        BlockValidationState retry;
        BOOST_REQUIRE(manager->ActiveChainstate().FlushStateToDisk(retry, mode));
        BOOST_CHECK(db.ReadBlockHeader(hash).GetHash() == hash);
        CBlockFileInfo persisted;
        BOOST_REQUIRE(db.ReadBlockFileInfo(genesis->nFile, persisted));
        BOOST_CHECK_EQUAL(persisted.nBlocks, blockman.GetBlockFileInfo(genesis->nFile)->nBlocks);
        BOOST_CHECK(!blockman.HeaderCacheNeedsFlush());
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(blockmanager_flush_both_cursors_before_shared_index, BlockFileDurabilitySetup)
{
    LOCK(cs_main);
    AddAssumedFile();
    for (bool undo : {false, true}) {
        CheckFileFailure(/*assumed_tip=*/true, /*fail_assumed=*/false, undo, FlushStateMode::ALWAYS);
        CheckFileFailure(/*assumed_tip=*/false, /*fail_assumed=*/true, undo, FlushStateMode::ALWAYS);
        CheckFileFailure(/*assumed_tip=*/true, /*fail_assumed=*/true, undo, FlushStateMode::ALWAYS);
    }
}

BOOST_FIXTURE_TEST_CASE(blockmanager_header_budget_flush_preserves_dirty_data_on_failure, BlockFileDurabilitySetup)
{
    LOCK(cs_main);
    AddAssumedFile();
    CheckFileFailure(/*assumed_tip=*/true, /*fail_assumed=*/false, /*undo=*/false,
                     FlushStateMode::NONE, /*header_count=*/65536);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_flush_without_assumed_cursor, BlockFileDurabilitySetup)
{
    LOCK(cs_main);
    // A snapshot can exist before its first block file has been created.
    manager->m_blockman.m_snapshot_height = 1;
    const auto header = StageWrites();
    auto& chainstate = manager->ActiveChainstate();
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS));
    BOOST_CHECK(manager->m_blockman.m_block_tree_db->ReadBlockHeader(header.GetHash()).GetHash() == header.GetHash());
    BOOST_CHECK_EQUAL(notifications.flush_errors, 0U);
    BOOST_CHECK_EQUAL(notifications.fatal_errors, 0U);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_accept_block_reports_final_flush_failure, BlockFileDurabilitySetup)
{
    LOCK(cs_main);
    auto& blockman = manager->m_blockman;
    auto block = CreateBlockChain(1, Params()).front();
    const auto hash = block->GetHash();
    // Exercise the block notification used after initial download.
    manager->m_cached_finished_ibd = true;
    BOOST_REQUIRE(!blockman.HeaderCacheNeedsFlush());
    bool notified{false};
    const auto undo_path = fs::path{blockman.GetBlockPosFilename(genesis->GetBlockPos()).parent_path()} / "rev00000.dat";
    BOOST_REQUIRE(fs::is_regular_file(undo_path));
    {
        ScopedPathObstruction obstruction{undo_path};
        ScopedBlockSaveListener listener{[&] {
            // This synchronous notification follows the early header-budget
            // check. Fill the cache here so only the final flush can fail.
            notified = true;
            BOOST_REQUIRE(!blockman.HeaderCacheNeedsFlush());
            StageWrites(65535);
            BOOST_REQUIRE(blockman.HeaderCacheNeedsFlush());
        }};
        BlockValidationState state;
        CBlockIndex* index{nullptr};
        bool new_block{false};
        BOOST_CHECK(!manager->AcceptBlock(block, state, &index, /*fRequested=*/true,
                                         /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
        BOOST_REQUIRE(notified);
        BOOST_REQUIRE(new_block);
        BOOST_REQUIRE(index);
        BOOST_CHECK(index->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_MASK));
        BOOST_CHECK(state.IsError());
        BOOST_CHECK(!state.IsInvalid());
        BOOST_CHECK_EQUAL(notifications.fatal_errors, 1U);
        BOOST_CHECK(!blockman.m_block_tree_db->Exists(std::make_pair(uint8_t{'b'}, hash)));
        BOOST_CHECK(blockman.LookupBlockIndex(hash)->GetBlockHeader().GetHash() == hash);
    }
    BlockValidationState retry;
    BOOST_REQUIRE(manager->ActiveChainstate().FlushStateToDisk(retry, FlushStateMode::NONE));
    BOOST_CHECK(blockman.m_block_tree_db->ReadBlockHeader(hash).GetHash() == hash);
}

#ifndef WIN32
BOOST_FIXTURE_TEST_CASE(blockmanager_failed_index_batch_preserves_dirty_data, BlockFileDurabilitySetup)
{
    LOCK(cs_main);
    auto& blockman = manager->m_blockman;
    auto& db = *blockman.m_block_tree_db;
    const auto header = StageWrites();
    const uint256 hash = header.GetHash();
    // Fill the 256 KiB write buffer. The next batch must create a new log.
    BOOST_REQUIRE(db.Write(uint8_t{'z'}, std::string(1 << 20, 'x'), /*fSync=*/true));
    const auto db_path = db.StoragePath();
    BOOST_REQUIRE(db_path);
    {
        // POSIX permits moving this open directory. Replacing it with a file
        // makes creation of the next log fail without a global I/O hook.
        ScopedPathObstruction obstruction{*db_path};
        BOOST_CHECK_THROW(blockman.WriteBlockIndexDB(), dbwrapper_error);
        BOOST_CHECK(!db.Exists(std::make_pair(uint8_t{'b'}, hash)));
        BOOST_CHECK(blockman.LookupBlockIndex(hash)->GetBlockHeader().GetHash() == hash);
    }
    BOOST_REQUIRE(blockman.WriteBlockIndexDB());
    BOOST_CHECK(db.ReadBlockHeader(hash).GetHash() == hash);
    CBlockFileInfo persisted;
    BOOST_REQUIRE(db.ReadBlockFileInfo(genesis->nFile, persisted));
    BOOST_CHECK_EQUAL(persisted.nBlocks, blockman.GetBlockFileInfo(genesis->nFile)->nBlocks);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
