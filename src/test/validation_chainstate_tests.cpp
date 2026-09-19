// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chainparams.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <node/utxo_snapshot.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <leveldb/env.h>
#include <leveldb/options.h>

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_chainstate_tests, ChainTestingSetup)

namespace {

using TestCoins = std::map<COutPoint, Coin>;

Coin StorageCoin(uint32_t number)
{
    CScript script;
    script.assign(4096, OP_TRUE);
    return Coin{CTxOut{1000 + number, script}, static_cast<int>(number + 1), false};
}

void SeedCoinsTables(const fs::path& path, const TestCoins& coins, const uint256& tip)
{
    {
        CCoinsViewDB database{{.path = path, .cache_bytes = 1 << 20, .obfuscate = true}, {}};
        CCoinsViewCache cache{&database};
        for (const auto& [outpoint, coin] : coins) cache.AddCoin(outpoint, Coin{coin}, false);
        cache.SetBestBlock(tip);
        BOOST_REQUIRE(cache.Flush());
    }
    // Put the existing coin records in table files before testing the reader.
    CDBWrapper compact{{.path = path, .cache_bytes = 1 << 20, .obfuscate = true,
                        .options = {.force_compact = true}}};
    bool table_found{false};
    for (const auto& entry : fs::directory_iterator{path}) {
        table_found |= entry.path().extension() == ".ldb" || entry.path().extension() == ".sst";
    }
    BOOST_REQUIRE(table_found);
}

#ifdef __linux__
size_t OpenCoinTableFiles(const fs::path& path)
{
    size_t count{0};
    for (const auto& entry : fs::directory_iterator{"/proc/self/fd"}) {
        std::error_code error;
        const auto target = fs::read_symlink(entry.path(), error);
        if (!error && target.parent_path() == path &&
            (target.extension() == ".ldb" || target.extension() == ".sst")) ++count;
    }
    return count;
}

bool MapsCoinsPath(const fs::path& path)
{
    std::ifstream mappings{"/proc/self/maps"};
    BOOST_REQUIRE(mappings.good());
    const auto prefix = fs::PathToString(path) + "/";
    std::string line;
    while (std::getline(mappings, line)) {
        if (line.find(prefix) != std::string::npos) return true;
    }
    return false;
}

struct StoredCoinKey {
    uint8_t prefix{'C'};
    uint256 txid{uint256::ONE};
    uint32_t number;
    explicit StoredCoinKey(uint32_t index) : number(index) {}
    SERIALIZE_METHODS(StoredCoinKey, obj) { READWRITE(obj.prefix, obj.txid, VARINT(obj.number)); }
};

void SeedManyCoinTables(const fs::path& path, const uint256& tip)
{
    CDBWrapper writer{{.path = path, .cache_bytes = 1 << 18, .obfuscate = true}};
    BOOST_REQUIRE(writer.Write(uint8_t{'B'}, tip));
    // Each batch fills a small write buffer with the next disjoint key range.
    // Rewriting the best-block key in every batch would make the ranges overlap
    // and compact them together, hiding descriptor growth from this test.
    for (uint32_t group = 0; group < 96; ++group) {
        CDBBatch batch{writer};
        for (uint32_t offset = 0; offset < 20; ++offset) {
            const uint32_t number = group * 20 + offset;
            batch.Write(StoredCoinKey{number}, StorageCoin(number));
        }
        BOOST_REQUIRE(writer.WriteBatch(batch));
    }
}
#endif

void CheckStoredCoins(CCoinsViewDB& database, const TestCoins& expected, const uint256& tip)
{
    BOOST_CHECK(database.GetBestBlock() == tip);
    BOOST_CHECK(database.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        Coin actual;
        BOOST_REQUIRE(database.GetCoin(outpoint, actual));
        BOOST_CHECK(actual.out == coin.out);
        BOOST_CHECK_EQUAL(actual.nHeight, coin.nHeight);
        BOOST_CHECK_EQUAL(actual.fCoinBase, coin.fCoinBase);
    }
    std::set<COutPoint> seen;
    auto cursor = database.Cursor();
    BOOST_REQUIRE(cursor);
    for (; cursor->Valid(); cursor->Next()) {
        COutPoint outpoint;
        Coin actual;
        BOOST_REQUIRE(cursor->GetKey(outpoint));
        BOOST_REQUIRE(cursor->GetValue(actual));
        const auto found = expected.find(outpoint);
        BOOST_REQUIRE(found != expected.end());
        BOOST_CHECK(seen.insert(outpoint).second);
        BOOST_CHECK(actual.out == found->second.out);
        BOOST_CHECK_EQUAL(actual.nHeight, found->second.nHeight);
        BOOST_CHECK_EQUAL(actual.fCoinBase, found->second.fCoinBase);
    }
    cursor->CheckStatus();
    BOOST_CHECK_EQUAL(seen.size(), expected.size());
#ifdef __linux__
    BOOST_REQUIRE(database.StoragePath());
    BOOST_CHECK(!MapsCoinsPath(*database.StoragePath()));
#endif
}

void CheckCoinsReaderLifecycle(ChainstateManager& manager, bool snapshot)
{
    LOCK(cs_main);
    const fs::path name{"coins-reader"};
    fs::path disk_name{name};
    if (snapshot) disk_name += node::SNAPSHOT_CHAINSTATE_SUFFIX;
    const auto path = manager.m_options.datadir / disk_name;
    TestCoins coins;
    for (uint32_t number = 0; number < 128; ++number) {
        coins.emplace(COutPoint{uint256::ONE, number}, StorageCoin(number));
    }
    uint256 tip{uint256S("10")};
    SeedCoinsTables(path, coins, tip);
    Chainstate chainstate{nullptr, nullptr, manager.m_blockman, manager,
                         snapshot ? std::optional<uint256>{uint256S("20")} : std::nullopt};
    chainstate.InitCoinsDB(1 << 20, false, false, name);
    BOOST_REQUIRE(chainstate.CoinsDB().StoragePath());
    BOOST_CHECK(*chainstate.CoinsDB().StoragePath() == path);
    BOOST_CHECK_EQUAL(chainstate.m_coinsdb_cache_size_bytes, 1U << 20);
    CheckStoredCoins(chainstate.CoinsDB(), coins, tip);

    chainstate.CoinsDB().ResizeCache(1 << 19);
    CheckStoredCoins(chainstate.CoinsDB(), coins, tip);
    {
        CCoinsViewCache cache{&chainstate.CoinsDB()};
        const COutPoint spent{uint256::ONE, 0};
        BOOST_REQUIRE(cache.SpendCoin(spent));
        coins.erase(spent);
        const COutPoint added{uint256::ONE, 128};
        coins.emplace(added, StorageCoin(128));
        cache.AddCoin(added, StorageCoin(128), false);
        tip = uint256S("11");
        cache.SetBestBlock(tip);
        BOOST_REQUIRE(cache.Flush());
    }
    chainstate.CoinsDB().ResizeCache(1 << 21);
    CheckStoredCoins(chainstate.CoinsDB(), coins, tip);
    chainstate.ResetCoinsViews();
    chainstate.InitCoinsDB(1 << 20, false, false, name);
    CheckStoredCoins(chainstate.CoinsDB(), coins, tip);
}

} // namespace

BOOST_AUTO_TEST_CASE(coins_database_buffered_reader_survives_reopen)
{
    CheckCoinsReaderLifecycle(*m_node.chainman, false);
}

BOOST_AUTO_TEST_CASE(snapshot_coins_database_buffered_reader_survives_reopen)
{
    CheckCoinsReaderLifecycle(*m_node.chainman, true);
}

#ifdef __linux__
BOOST_AUTO_TEST_CASE(coins_database_retains_bounded_table_descriptors)
{
    LOCK(cs_main);
    auto& manager = *m_node.chainman;
    const auto path = manager.m_options.datadir / "many-coin-tables";
    const uint256 tip{uint256S("40")};
    SeedManyCoinTables(path, tip);
    Chainstate chainstate{nullptr, nullptr, manager.m_blockman, manager};
    chainstate.InitCoinsDB(1 << 18, false, false, "many-coin-tables");
    size_t tables{0};
    for (const auto& entry : fs::directory_iterator{path}) {
        tables += entry.path().extension() == ".ldb" || entry.path().extension() == ".sst";
    }
    BOOST_REQUIRE_GT(tables, 74U);
    const auto check_reads_and_descriptors = [&] {
        BOOST_CHECK(chainstate.CoinsDB().GetBestBlock() == tip);
        for (uint32_t number = 0; number < 96 * 20; ++number) {
            Coin actual;
            BOOST_REQUIRE(chainstate.CoinsDB().GetCoin(COutPoint{uint256::ONE, number}, actual));
            BOOST_CHECK(actual.out == StorageCoin(number).out);
            BOOST_CHECK_EQUAL(actual.nHeight, number + 1);
        }
        const size_t open_tables = OpenCoinTableFiles(path);
        BOOST_CHECK_GT(open_tables, 0U);
        BOOST_CHECK_LE(open_tables, 64U);
        BOOST_CHECK(!MapsCoinsPath(path));
    };
    check_reads_and_descriptors();
    chainstate.CoinsDB().ResizeCache(1 << 19);
    check_reads_and_descriptors();
    chainstate.ResetCoinsViews();
    BOOST_CHECK_EQUAL(OpenCoinTableFiles(path), 0U);
    chainstate.InitCoinsDB(1 << 18, false, false, "many-coin-tables");
    check_reads_and_descriptors();
}
#endif

BOOST_AUTO_TEST_CASE(coins_database_policy_preserves_memory_and_ordinary_databases)
{
    LOCK(cs_main);
    auto& manager = *m_node.chainman;
    Chainstate chainstate{nullptr, nullptr, manager.m_blockman, manager};
    chainstate.InitCoinsDB(1 << 20, true, false, "memory-coins-reader");
    auto& database = chainstate.CoinsDB();
    const COutPoint outpoint{uint256::ONE, 0};
    const uint256 tip{uint256S("30")};
    {
        CCoinsViewCache cache{&database};
        cache.AddCoin(outpoint, StorageCoin(0), false);
        cache.SetBestBlock(tip);
        BOOST_REQUIRE(cache.Flush());
    }
    database.ResizeCache(1 << 19);
    Coin actual;
    BOOST_REQUIRE(database.GetCoin(outpoint, actual));
    BOOST_CHECK(actual.out == StorageCoin(0).out);
    BOOST_CHECK(database.GetBestBlock() == tip);
    BOOST_CHECK(!database.StoragePath());
    BOOST_CHECK(!fs::exists(manager.m_options.datadir / "memory-coins-reader"));

    const auto ordinary_path = manager.m_options.datadir / "ordinary-reader";
    SeedCoinsTables(ordinary_path, {{outpoint, StorageCoin(0)}}, tip);
    CDBWrapper ordinary{{.path = ordinary_path, .cache_bytes = 1 << 20, .obfuscate = true}};
    BOOST_CHECK(dbwrapper_private::GetOptions(ordinary).env == leveldb::Env::Default());
    uint256 stored_tip;
    BOOST_REQUIRE(ordinary.Read(uint8_t{'B'}, stored_tip));
    BOOST_CHECK(stored_tip == tip);
#ifdef __linux__
    if (sizeof(void*) >= 8) BOOST_CHECK(MapsCoinsPath(ordinary_path));
#endif
}

//! Test resizing coins-related Chainstate caches during runtime.
//!
BOOST_AUTO_TEST_CASE(validation_chainstate_resize_caches)
{
    ChainstateManager& manager = *Assert(m_node.chainman);
    CTxMemPool& mempool = *Assert(m_node.mempool);
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool, nullptr));
    c1.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(1 << 23));
    BOOST_REQUIRE(c1.LoadGenesisBlock()); // Need at least one block loaded to be able to flush caches

    // Add a coin to the in-memory cache, upsize once, then downsize.
    {
        LOCK(::cs_main);
        const auto outpoint = AddTestCoin(c1.CoinsTip());

        // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
        // flush during ResizecoinsCaches() and will subsequently hit an assertion.
        c1.CoinsTip().SetBestBlock(InsecureRand256());

        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            1 << 24,  // upsizing the coinsview cache
            1 << 22  // downsizing the coinsdb cache
        );

        // View should still have the coin cached, since we haven't destructed the cache on upsize.
        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            1 << 22,  // downsizing the coinsview cache
            1 << 23  // upsizing the coinsdb cache
        );

        // The view cache should be empty since we had to destruct to downsize.
        BOOST_CHECK(!c1.CoinsTip().HaveCoinInCache(outpoint));
    }
}

//! Test UpdateTip behavior for both active and background chainstates.
//!
//! When run on the background chainstate, UpdateTip should do a subset
//! of what it does for the active chainstate.
// FIXME: This test is temporarily disabled because it relies on assumeutxo data
// that contains Bitcoin-specific block hashes. DigiByte generates different
// block hashes due to using ALGO_SCRYPT instead of SHA256D for mining.
// To fix this test:
// 1. Run digibyte-cli in regtest mode
// 2. Generate blocks to heights 110 and 299
// 3. Use dumptxoutset RPC to get the UTXO set hashes
// 4. Update the assumeutxo data in chainparams.cpp with DigiByte values
// See VALIDATION_TEST_FIX.md for detailed instructions
BOOST_FIXTURE_TEST_CASE(chainstate_update_tip, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    uint256 curr_tip = ::g_best_block;

    // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
    // be found.
    mineBlocks(10);

    // After adding some blocks to the tip, best block should have changed.
    BOOST_CHECK(::g_best_block != curr_tip);

    // Grab block 1 from disk; we'll add it to the background chain later.
    std::shared_ptr<CBlock> pblockone = std::make_shared<CBlock>();
    {
        LOCK(::cs_main);
        chainman.m_blockman.ReadBlockFromDisk(*pblockone, *chainman.ActiveChain()[1]);
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(
        this, NoMalleation, /*reset_chainstate=*/ true));

    // Ensure our active chain is the snapshot chainstate.
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.IsSnapshotActive()));

    curr_tip = ::g_best_block;

    // Mine a new block on top of the activated snapshot chainstate.
    mineBlocks(1);  // Defined in TestChain100Setup.

    // After adding some blocks to the snapshot tip, best block should have changed.
    BOOST_CHECK(::g_best_block != curr_tip);

    curr_tip = ::g_best_block;

    BOOST_CHECK_EQUAL(chainman.GetAll().size(), 2);

    Chainstate& background_cs{*[&] {
        for (Chainstate* cs : chainman.GetAll()) {
            if (cs != &chainman.ActiveChainstate()) {
                return cs;
            }
        }
        assert(false);
    }()};

    // Append the first block to the background chain.
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    const CChainParams& chainparams = Params();
    bool newblock = false;

    // TODO: much of this is inlined from ProcessNewBlock(); just reuse PNB()
    // once it is changed to support multiple chainstates.
    {
        LOCK(::cs_main);
        bool checked = CheckBlock(*pblockone, state, chainparams.GetConsensus());
        BOOST_CHECK(checked);
        bool accepted = chainman.AcceptBlock(
            pblockone, state, &pindex, true, nullptr, &newblock, true);
        BOOST_CHECK(accepted);
    }

    // UpdateTip is called here
    bool block_added = background_cs.ActivateBestChain(state, pblockone);

    // Ensure tip is as expected
    BOOST_CHECK_EQUAL(background_cs.m_chain.Tip()->GetBlockHash(), pblockone->GetHash());

    // g_best_block should be unchanged after adding a block to the background
    // validation chain.
    BOOST_CHECK(block_added);
    BOOST_CHECK_EQUAL(curr_tip, ::g_best_block);
}

BOOST_AUTO_TEST_SUITE_END()
