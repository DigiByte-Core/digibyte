// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/digidollar_state.h>
#include <dbwrapper.h>
#include <digidollar/health.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <txdb.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr uint8_t COIN_KEY{'C'};
constexpr uint8_t BEST_BLOCK_KEY{'B'};
constexpr uint8_t HEAD_BLOCKS_KEY{'H'};
constexpr uint8_t HEALTH_KEY{'D'};

DBParams DiskParams(const fs::path& path)
{
    return {.path = path, .cache_bytes = 1 << 20, .memory_only = false, .obfuscate = true};
}

DigiDollar::ChainstateHealth HealthAt(const uint256& block)
{
    DigiDollar::ChainstateHealth health;
    health.genesis_hash = Params().GetConsensus().hashGenesisBlock;
    health.best_block = block;
    return health;
}

void CheckCoin(const CCoinsView& view, const COutPoint& outpoint, const Coin& expected)
{
    Coin actual;
    BOOST_REQUIRE(view.GetCoin(outpoint, actual));
    BOOST_CHECK(actual.out == expected.out);
    BOOST_CHECK_EQUAL(actual.nHeight, expected.nHeight);
    BOOST_CHECK_EQUAL(actual.fCoinBase, expected.fCoinBase);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_chainstate_db_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(health_and_coins_survive_close_and_reopen)
{
    const auto db_params = DiskParams(m_args.GetDataDirNet() / "dd-health-roundtrip");
    const auto tip = uint256S("12");
    const COutPoint outpoint{uint256S("01"), 3};
    const Coin expected{CTxOut{50 * COIN, CScript{} << OP_TRUE}, 123, false};
    auto health = HealthAt(tip);
    BOOST_REQUIRE(health.AddVault(120000, expected.out.nValue));
    for (const bool checked : {false, true}) {
        health.history_checked = checked;
        {
            CCoinsViewDB db{db_params, {}};
            BOOST_REQUIRE(db.StoragePath().has_value());
            CCoinsViewCache cache{&db};
            if (!cache.HaveCoin(outpoint)) cache.AddCoin(outpoint, Coin{expected}, false);
            cache.SetBestBlock(tip);
            cache.SetDigiDollarState(health);
            BOOST_REQUIRE(cache.Flush());
        }
        {
            CCoinsViewDB reopened{db_params, {}};
            BOOST_CHECK(reopened.GetBestBlock() == tip);
            BOOST_CHECK(reopened.GetHeadBlocks().empty());
            const auto saved = reopened.GetDigiDollarState();
            BOOST_REQUIRE(saved);
            BOOST_CHECK(*saved == health);
            BOOST_CHECK_EQUAL(saved->history_checked, checked);
            CheckCoin(reopened, outpoint, expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(tiny_batches_finish_with_matching_coins_tip_and_health)
{
    const auto db_params = DiskParams(m_args.GetDataDirNet() / "dd-health-batches");
    const auto old_tip = uint256S("20");
    const auto new_tip = uint256S("21");
    const COutPoint spent{uint256S("01"), 0};
    auto old_health = HealthAt(old_tip);
    BOOST_REQUIRE(old_health.AddVault(10000, 5 * COIN));
    auto health = HealthAt(new_tip);
    health.history_checked = true;
    constexpr uint32_t COUNT{8};
    {
        // One byte forces every coin mutation into a partial batch. The
        // final batch must finish the tip and health transition together.
        CCoinsViewDB db{db_params, {.batch_write_bytes = 1}};
        CCoinsViewCache cache{&db};
        cache.SetBestBlock(old_tip);
        cache.SetDigiDollarState(old_health);
        cache.AddCoin(spent, Coin{CTxOut{5 * COIN, CScript{} << OP_TRUE}, 1, false}, false);
        BOOST_REQUIRE(cache.Flush());
        BOOST_REQUIRE(cache.SpendCoin(spent));
        for (uint32_t i = 0; i < COUNT; ++i) {
            const CAmount value = (i + 1) * COIN;
            cache.AddCoin(COutPoint{new_tip, i}, Coin{CTxOut{value, CScript{} << OP_TRUE}, 2, false}, false);
            BOOST_REQUIRE(health.AddVault(10000 + i, value));
        }
        cache.SetBestBlock(new_tip);
        cache.SetDigiDollarState(health);
        BOOST_REQUIRE(cache.Flush());
    }
    {
        CDBWrapper raw{db_params};
        uint256 saved_tip;
        DigiDollar::ChainstateHealth saved;
        BOOST_REQUIRE(raw.Read(BEST_BLOCK_KEY, saved_tip));
        BOOST_REQUIRE(raw.Read(HEALTH_KEY, saved));
        BOOST_CHECK(saved_tip == new_tip);
        BOOST_CHECK(saved == health);
        BOOST_CHECK(saved.best_block == saved_tip);
        BOOST_CHECK(!raw.Exists(HEAD_BLOCKS_KEY));
    }
    {
        CCoinsViewDB reopened{db_params, {}};
        BOOST_CHECK(reopened.GetBestBlock() == new_tip);
        BOOST_CHECK(reopened.GetDigiDollarState() == health);
        BOOST_CHECK(!reopened.HaveCoin(spent));
        for (uint32_t i = 0; i < COUNT; ++i) {
            CheckCoin(reopened, COutPoint{new_tip, i},
                      Coin{CTxOut{(i + 1) * COIN, CScript{} << OP_TRUE}, 2, false});
        }
        auto cursor = reopened.Cursor();
        BOOST_REQUIRE(cursor);
        uint32_t count{0};
        for (; cursor->Valid(); cursor->Next()) ++count;
        BOOST_CHECK_NO_THROW(cursor->CheckStatus());
        BOOST_CHECK_EQUAL(count, COUNT);
    }
}

BOOST_AUTO_TEST_CASE(transition_marker_hides_an_otherwise_valid_saved_record)
{
    const auto db_params = DiskParams(m_args.GetDataDirNet() / "dd-health-heads");
    const auto old_tip = uint256S("30");
    const auto next_tip = uint256S("31");
    auto health = HealthAt(old_tip);
    health.history_checked = true;
    {
        CCoinsViewDB db{db_params, {}};
        CCoinsMapMemoryResource resource;
        CCoinsMap coins{0, CCoinsMap::hasher{}, CCoinsMap::key_equal{}, &resource};
        BOOST_REQUIRE(db.BatchWrite(coins, old_tip, true, health));
    }
    for (int marker_kind : {0, 1, 2}) {
        {
            CDBWrapper raw{db_params};
            if (marker_kind == 0) {
                BOOST_REQUIRE(raw.Write(HEAD_BLOCKS_KEY, std::vector<uint256>{next_tip, old_tip}));
            } else if (marker_kind == 1) {
                BOOST_REQUIRE(raw.Write(HEAD_BLOCKS_KEY, std::vector<uint256>{}));
            } else {
                BOOST_REQUIRE(raw.Write(HEAD_BLOCKS_KEY, uint8_t{2}));
            }
            DigiDollar::ChainstateHealth saved;
            BOOST_REQUIRE(raw.Read(HEALTH_KEY, saved));
            BOOST_CHECK(saved == health);
        }
        {
            CCoinsViewDB reopened{db_params, {}};
            BOOST_CHECK(reopened.GetBestBlock() == old_tip);
            BOOST_CHECK(!reopened.GetDigiDollarState());
            CCoinsViewCache cache{&reopened};
            BOOST_CHECK(!cache.GetDigiDollarState());
            if (marker_kind == 0) BOOST_CHECK(reopened.GetHeadBlocks() == std::vector<uint256>({next_tip, old_tip}));
        }
        {
            CDBWrapper raw{db_params};
            BOOST_REQUIRE(raw.Erase(HEAD_BLOCKS_KEY));
        }
        {
            CCoinsViewDB reopened{db_params, {}};
            BOOST_CHECK(reopened.GetDigiDollarState() == health);
        }
    }
}

BOOST_AUTO_TEST_CASE(absent_and_invalid_records_are_not_verified_zero)
{
    const auto db_params = DiskParams(m_args.GetDataDirNet() / "dd-health-invalid");
    const auto tip = uint256S("40");
    const COutPoint outpoint{uint256S("01"), 0};
    const Coin coin{CTxOut{COIN, CScript{} << OP_TRUE}, 1, false};
    auto empty = HealthAt(tip);
    empty.history_checked = true;
    {
        CCoinsViewDB db{db_params, {}};
        CCoinsViewCache cache{&db};
        cache.SetBestBlock(tip);
        cache.SetDigiDollarState(empty);
        cache.AddCoin(outpoint, Coin{coin}, false);
        BOOST_REQUIRE(cache.Flush());
    }
    {
        CCoinsViewDB reopened{db_params, {}};
        const auto saved = reopened.GetDigiDollarState();
        BOOST_REQUIRE(saved);
        BOOST_CHECK(*saved == empty);
        BOOST_CHECK_EQUAL(saved->active_vaults, 0U);
        BOOST_CHECK(saved->history_checked);
    }
    for (const std::string kind : {"missing", "old_format", "unknown_rules", "malformed", "wrong_tip", "invalid_totals"}) {
        BOOST_TEST_CONTEXT(kind) {
            {
                CDBWrapper raw{db_params};
                auto saved = empty;
                if (kind == "missing") {
                    BOOST_REQUIRE(raw.Erase(HEALTH_KEY));
                } else if (kind == "malformed") {
                    BOOST_REQUIRE(raw.Write(HEALTH_KEY, uint8_t{1}));
                } else {
                    if (kind == "old_format") saved.format_version = 0;
                    if (kind == "unknown_rules") ++saved.rules_version;
                    if (kind == "wrong_tip") saved.best_block = uint256S("41");
                    if (kind == "invalid_totals") saved.open_vault_principal = 1;
                    BOOST_REQUIRE(raw.Write(HEALTH_KEY, saved));
                }
            }
            {
                CCoinsViewDB reopened{db_params, {}};
                BOOST_CHECK(reopened.GetBestBlock() == tip);
                BOOST_CHECK(!reopened.GetDigiDollarState());
                CCoinsViewCache cache{&reopened};
                BOOST_CHECK(!cache.GetDigiDollarState());
                CheckCoin(reopened, outpoint, coin);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(health_record_is_normal_end_of_coin_iteration)
{
    const auto db_params = DiskParams(m_args.GetDataDirNet() / "dd-health-cursor-end");
    const auto tip = uint256S("50");
    auto saved = HealthAt(tip);
    saved.history_checked = true;
    {
        CCoinsViewDB db{db_params, {}};
        CCoinsMapMemoryResource resource;
        CCoinsMap coins{0, CCoinsMap::hasher{}, CCoinsMap::key_equal{}, &resource};
        BOOST_REQUIRE(db.BatchWrite(coins, tip, true, saved));
    }
    {
        CCoinsViewDB reopened{db_params, {}};
        auto cursor = reopened.Cursor();
        BOOST_REQUIRE(cursor);
        BOOST_CHECK(!cursor->Valid());
        BOOST_CHECK_NO_THROW(cursor->CheckStatus());
        auto rebuilt = HealthAt(uint256S("51"));
        CAmount supply{-1};
        std::string error;
        BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(
            reopened, Params().GetConsensus(), {}, rebuilt, error, {}, &supply), error);
        BOOST_CHECK(rebuilt.Matches(saved.genesis_hash, tip));
        BOOST_CHECK_EQUAL(rebuilt.active_vaults, 0U);
        BOOST_CHECK_EQUAL(supply, 0);
        BOOST_CHECK(!rebuilt.history_checked);
    }
}

BOOST_AUTO_TEST_CASE(malformed_coin_keys_do_not_publish_partial_reconstruction)
{
    for (const bool first_key : {true, false}) {
        const auto db_params = DiskParams(m_args.GetDataDirNet() /
            (first_key ? "dd-health-first-key" : "dd-health-later-key"));
        const auto tip = uint256S("60");
        const COutPoint outpoint{uint256S("01"), 0};
        const Coin coin{CTxOut{COIN, CScript{} << OP_TRUE}, 1, false};
        auto saved = HealthAt(tip);
        {
            CCoinsViewDB db{db_params, {}};
            CCoinsViewCache cache{&db};
            cache.SetBestBlock(tip);
            cache.SetDigiDollarState(saved);
            cache.AddCoin(outpoint, Coin{coin}, false);
            BOOST_REQUIRE(cache.Flush());
        }
        {
            CDBWrapper raw{db_params};
            if (first_key) {
                BOOST_REQUIRE(raw.Write(COIN_KEY, coin));
            } else {
                // A fixed array writes raw bytes without a length prefix.
                // This truncated key sorts after the valid coin and before D.
                BOOST_REQUIRE(raw.Write(std::array<uint8_t, 2>{COIN_KEY, 0xff}, coin));
            }
        }
        {
            CCoinsViewDB reopened{db_params, {}};
            auto cursor = reopened.Cursor();
            BOOST_REQUIRE(cursor);
            if (!first_key) {
                BOOST_REQUIRE(cursor->Valid());
                COutPoint found;
                BOOST_REQUIRE(cursor->GetKey(found));
                BOOST_CHECK(found == outpoint);
                cursor->Next();
            }
            BOOST_CHECK(!cursor->Valid());
            BOOST_CHECK_THROW(cursor->CheckStatus(), dbwrapper_error);
            auto unchanged = HealthAt(uint256S("61"));
            unchanged.history_checked = true;
            BOOST_REQUIRE(unchanged.AddVault(12345, 2 * COIN));
            auto rebuilt = unchanged;
            CAmount supply{6789};
            std::string error;
            BOOST_CHECK(!DigiDollar::ReconstructChainstateHealth(
                reopened, Params().GetConsensus(), {}, rebuilt, error, {}, &supply));
            BOOST_CHECK(rebuilt == unchanged);
            BOOST_CHECK_EQUAL(supply, 6789);
            BOOST_CHECK(error.find("coin key") != std::string::npos);

            // A valid overlay entry must not hide the backing cursor's error.
            CCoinsViewCache parent{&reopened};
            parent.AddCoin(COutPoint{uint256S("02"), 0}, Coin{coin}, false);
            CCoinsViewCache child{&parent};
            error.clear();
            BOOST_CHECK(!DigiDollar::ReconstructChainstateHealth(
                child, Params().GetConsensus(), {}, rebuilt, error, {}, &supply));
            BOOST_CHECK(rebuilt == unchanged);
            BOOST_CHECK_EQUAL(supply, 6789);
            BOOST_CHECK(error.find("coin key") != std::string::npos);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
