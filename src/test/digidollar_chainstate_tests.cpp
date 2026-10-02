// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/digidollar_state.h>
#include <digidollar/health.h>
#include <digidollar/digidollar.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <txdb.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>

namespace {
DigiDollar::ChainstateHealth Health(const uint256& block)
{
    DigiDollar::ChainstateHealth health;
    health.genesis_hash = uint256S("01");
    health.best_block = block;
    health.history_checked = true;
    return health;
}

class FailedWriteView : public CCoinsView {
public:
    bool BatchWrite(CCoinsMap&, const uint256&, bool,
                    const std::optional<DigiDollar::ChainstateHealth>&) override { return false; }
};
}

BOOST_FIXTURE_TEST_SUITE(digidollar_chainstate_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(checked_vault_accounting_and_exact_inverse)
{
    auto health = Health(uint256S("02"));
    BOOST_REQUIRE(health.AddVault(100000, 1000 * COIN));
    BOOST_REQUIRE(health.AddVault(10000, 100 * COIN));
    const auto before = health;
    BOOST_REQUIRE(health.RemoveVault(10000, 100 * COIN));
    BOOST_CHECK_EQUAL(health.open_vault_principal, 100000);
    BOOST_CHECK_EQUAL(health.collateral, 1000 * COIN);
    BOOST_CHECK_EQUAL(health.active_vaults, 1U);
    BOOST_REQUIRE(health.AddVault(10000, 100 * COIN));
    BOOST_CHECK(health == before);
    BOOST_CHECK(!health.RemoveVault(110001, 100 * COIN));
    BOOST_CHECK(health == before);
    BOOST_CHECK(!health.AddVault(std::numeric_limits<CAmount>::max(), COIN));
    BOOST_CHECK(health == before);
    BOOST_CHECK(!health.AddVault(1, MAX_MONEY));
    BOOST_CHECK(health == before);
    BOOST_CHECK(!health.RemoveVault(1, -1));
    BOOST_CHECK(health == before);
}

BOOST_AUTO_TEST_CASE(nested_private_views_flush_and_sync)
{
    CCoinsViewDB db{{.path = m_args.GetDataDirNet() / "dd-coins", .cache_bytes = 1 << 20, .memory_only = true}, {}};
    CCoinsViewCache live{&db};
    const auto old_hash = uint256S("02");
    live.SetBestBlock(old_hash);
    live.SetDigiDollarState(Health(old_hash));
    BOOST_REQUIRE(live.Flush());
    {
        CCoinsViewCache rejected{&live};
        auto next = *rejected.GetDigiDollarState();
        BOOST_REQUIRE(next.AddVault(10000, 10 * COIN));
        next.best_block = uint256S("03");
        rejected.SetBestBlock(next.best_block);
        rejected.SetDigiDollarState(next);
        BOOST_CHECK_EQUAL(live.GetDigiDollarState()->active_vaults, 0U);
    }
    BOOST_CHECK_EQUAL(db.GetDigiDollarState()->active_vaults, 0U);
    CCoinsViewCache accepted{&live};
    auto next = *accepted.GetDigiDollarState();
    BOOST_REQUIRE(next.AddVault(10000, 10 * COIN));
    next.best_block = uint256S("04");
    accepted.SetBestBlock(next.best_block);
    accepted.SetDigiDollarState(next);
    BOOST_REQUIRE(accepted.Sync());
    BOOST_CHECK(live.GetDigiDollarState() == next);
    BOOST_CHECK(db.GetDigiDollarState()->best_block == old_hash);
    BOOST_REQUIRE(live.Flush());
    BOOST_CHECK(db.GetDigiDollarState() == next);
    live.SetDigiDollarState(std::nullopt);
    BOOST_REQUIRE(live.Sync());
    BOOST_CHECK(!db.GetDigiDollarState());
}

BOOST_AUTO_TEST_CASE(missing_mismatched_and_failed_write_state)
{
    FailedWriteView failing;
    CCoinsViewCache cache{&failing};
    BOOST_CHECK(!cache.GetDigiDollarState());
    cache.SetBestBlock(uint256S("02"));
    cache.SetDigiDollarState(Health(uint256S("02")));
    BOOST_CHECK(!cache.Sync());
    BOOST_CHECK(!cache.GetDigiDollarState());
    cache.SetDigiDollarState(Health(uint256S("02")));
    cache.SetBestBlock(uint256S("03"));
    BOOST_CHECK(!cache.GetDigiDollarState());
}

BOOST_AUTO_TEST_CASE(failed_flush_preserves_nonempty_cache_usage_and_allows_retry)
{
    class RetryView : public CCoinsViewBacked {
    public:
        enum class Result { FAIL, THROW, WRITE } result{Result::FAIL};
        explicit RetryView(CCoinsView* base) : CCoinsViewBacked(base) {}
        bool BatchWrite(CCoinsMap& coins, const uint256& hash, bool erase,
                        const std::optional<DigiDollar::ChainstateHealth>& health) override
        {
            if (result == Result::FAIL) return false;
            if (result == Result::THROW) throw dbwrapper_error("injected batch failure");
            return CCoinsViewBacked::BatchWrite(coins, hash, erase, health);
        }
    };
    CCoinsViewDB db{{.path = m_args.GetDataDirNet() / "dd-retry", .cache_bytes = 1 << 20, .memory_only = true}, {}};
    RetryView retry{&db};
    CCoinsViewCache cache{&retry};
    const auto hash = uint256S("20");
    const COutPoint out{uint256S("21"), 0};
    const CScript script = CScript{} << std::vector<unsigned char>(200, 1) << OP_DROP << OP_TRUE;
    cache.SetBestBlock(hash);
    cache.AddCoin(out, Coin{CTxOut{COIN, script}, 1, false}, false);
    cache.SetDigiDollarState(Health(hash));
    const auto usage = cache.DynamicMemoryUsage();
    BOOST_CHECK(!cache.Flush());
    BOOST_CHECK_EQUAL(cache.DynamicMemoryUsage(), usage);
    BOOST_CHECK(!cache.GetDigiDollarState());
    BOOST_CHECK(cache.HaveCoin(out));
    retry.result = RetryView::Result::THROW;
    cache.SetDigiDollarState(Health(hash));
    BOOST_CHECK_THROW(cache.Flush(), dbwrapper_error);
    BOOST_CHECK_EQUAL(cache.DynamicMemoryUsage(), usage);
    BOOST_CHECK(!cache.GetDigiDollarState());
    BOOST_REQUIRE(cache.SpendCoin(out));
    BOOST_CHECK_LT(cache.DynamicMemoryUsage(), usage);
    cache.AddCoin(out, Coin{CTxOut{COIN, script}, 1, false}, false);
    cache.SetDigiDollarState(Health(hash));
    retry.result = RetryView::Result::WRITE;
    BOOST_REQUIRE(cache.Flush());
    BOOST_CHECK(db.HaveCoin(out));
    BOOST_CHECK(db.GetDigiDollarState() == Health(hash));
}

BOOST_AUTO_TEST_CASE(canonical_health_uses_original_principal_and_checked_quote)
{
    auto health = Health(uint256S("30"));
    BOOST_CHECK(!DigiDollar::CalculateChainstateHealth(health, 0));
    BOOST_CHECK_EQUAL(*DigiDollar::CalculateChainstateHealth(health, 1000000), 30000);
    BOOST_REQUIRE(health.AddVault(10000, 100 * COIN));
    BOOST_CHECK_EQUAL(*DigiDollar::CalculateChainstateHealth(health, 1000000), 100);
    BOOST_CHECK_EQUAL(*DigiDollar::CalculateChainstateHealth(health, 999999), 99);
    BOOST_CHECK_EQUAL(*DigiDollar::CalculateChainstateHealth(health, 800000), 80);
    BOOST_REQUIRE(health.AddVault(10000, 100 * COIN));
    BOOST_CHECK_EQUAL(*DigiDollar::CalculateChainstateHealth(health, 800000), 80);
    BOOST_REQUIRE(health.RemoveVault(10000, 100 * COIN));
    BOOST_CHECK_EQUAL(*DigiDollar::CalculateChainstateHealth(health, 800000), 80);
}

BOOST_AUTO_TEST_CASE(cursor_includes_unflushed_nested_changes)
{
    CCoinsViewDB db{{.path = m_args.GetDataDirNet() / "dd-cursor", .cache_bytes = 1 << 20, .memory_only = true}, {}};
    CCoinsViewCache live{&db};
    const COutPoint spent{uint256S("01"), 0};
    const COutPoint kept{uint256S("01"), 1};
    const COutPoint added{uint256S("02"), 0};
    live.SetBestBlock(uint256S("04"));
    live.AddCoin(spent, Coin{CTxOut{10, CScript{} << OP_TRUE}, 1, false}, false);
    live.AddCoin(kept, Coin{CTxOut{20, CScript{} << OP_TRUE}, 1, false}, false);
    BOOST_REQUIRE(live.Flush());
    BOOST_REQUIRE(live.SpendCoin(spent));
    live.AddCoin(added, Coin{CTxOut{30, CScript{} << OP_TRUE}, 2, false}, false);
    CCoinsViewCache child{&live};
    auto cursor = child.Cursor();
    BOOST_REQUIRE(cursor);
    std::map<COutPoint, CAmount> amounts;
    for (; cursor->Valid(); cursor->Next()) {
        COutPoint out;
        Coin coin;
        BOOST_REQUIRE(cursor->GetKey(out));
        BOOST_REQUIRE(cursor->GetValue(coin));
        BOOST_CHECK(!coin.IsSpent());
        BOOST_REQUIRE(amounts.emplace(out, coin.out.nValue).second);
    }
    BOOST_CHECK_EQUAL(amounts.size(), 2U);
    BOOST_CHECK_EQUAL(amounts.at(kept), 20);
    BOOST_CHECK_EQUAL(amounts.at(added), 30);
    BOOST_CHECK(db.HaveCoin(spent));
    BOOST_CHECK(!db.HaveCoin(added));
}

BOOST_AUTO_TEST_CASE(database_markers_and_version_require_reconstruction)
{
    const DBParams db_params{.path = m_args.GetDataDirNet() / "dd-recovery", .cache_bytes = 1 << 20};
    const auto hash = uint256S("02");
    auto health = Health(hash);
    {
        CCoinsViewDB db{db_params, {.batch_write_bytes = 1}};
        CCoinsViewCache cache{&db};
        cache.SetBestBlock(hash);
        cache.SetDigiDollarState(health);
        for (uint32_t i = 0; i < 3; ++i) {
            cache.AddCoin(COutPoint{hash, i}, Coin{CTxOut{1, CScript{} << OP_TRUE}, 1, false}, false);
        }
        BOOST_REQUIRE(cache.Flush());
        BOOST_CHECK(db.GetDigiDollarState() == health);
        BOOST_CHECK(db.GetHeadBlocks().empty());
    }
    {
        CDBWrapper raw{db_params};
        BOOST_REQUIRE(raw.Write(uint8_t{'H'}, std::vector<uint256>{uint256S("03"), hash}));
    }
    {
        CCoinsViewDB db{db_params, {}};
        BOOST_CHECK(!db.GetDigiDollarState());
    }
    {
        CDBWrapper raw{db_params};
        BOOST_REQUIRE(raw.Erase(uint8_t{'H'}));
        health.format_version = 0;
        BOOST_REQUIRE(raw.Write(uint8_t{'D'}, health));
    }
    {
        CCoinsViewDB db{db_params, {}};
        BOOST_CHECK(!db.GetDigiDollarState());
    }
}

BOOST_AUTO_TEST_CASE(mint_accounting_respects_historical_creation_activation)
{
    auto params = Params().GetConsensus();
    params.DigiDollarHeight = 10;
    params.nDDThawDayHeight = 1;
    CMutableTransaction tx;
    tx.nVersion = MakeDigiDollarVersion(DD_TX_MINT);
    tx.vin.emplace_back(COutPoint{uint256S("41"), 0});
    tx.vout.emplace_back(100 * COIN, CScript{} << OP_1 << std::vector<unsigned char>(32, 1));
    tx.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                    << CScriptNum(1) << CScriptNum(10000) << CScriptNum(500) << CScriptNum(0));
    const CTransaction mint{tx};
    const std::vector<Coin> inputs{Coin{CTxOut{101 * COIN, CScript{} << OP_TRUE}, 1, false}};
    auto health = Health(uint256S("42"));
    const auto empty = health;
    std::string error;
    BOOST_REQUIRE(DigiDollar::UpdateChainstateHealth(mint, inputs, params, {}, health, false, error, 9));
    BOOST_CHECK(health == empty);
    BOOST_REQUIRE(DigiDollar::UpdateChainstateHealth(mint, inputs, params, {}, health, true, error, 9));
    BOOST_CHECK(health == empty);
    BOOST_REQUIRE(DigiDollar::UpdateChainstateHealth(mint, inputs, params, {}, health, false, error, 10));
    BOOST_CHECK_EQUAL(health.open_vault_principal, 10000);
    BOOST_REQUIRE(DigiDollar::UpdateChainstateHealth(mint, inputs, params, {}, health, true, error, 10));
    BOOST_CHECK(health == empty);
    params.DigiDollarHeight = std::numeric_limits<int>::max();
    BOOST_REQUIRE(DigiDollar::UpdateChainstateHealth(mint, inputs, params, {}, health, false, error, 10));
    BOOST_CHECK(health == empty);
}

BOOST_AUTO_TEST_CASE(reconstructs_actual_unspent_vault_outpoints)
{
    auto params = Params().GetConsensus();
    params.DigiDollarHeight = 1;
    const CScript taproot = CScript{} << OP_1 << std::vector<unsigned char>(32, 1);
    CMutableTransaction mint;
    mint.nVersion = MakeDigiDollarVersion(DD_TX_MINT);
    mint.vin.emplace_back(COutPoint{uint256S("09"), 0});
    mint.vout.emplace_back(5, CScript{} << OP_TRUE);
    mint.vout.emplace_back(100 * COIN, taproot);
    mint.vout.emplace_back(0, CScript{} << OP_1 << std::vector<unsigned char>(32, 2));
    mint.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                      << CScriptNum(1) << CScriptNum(10000) << CScriptNum(500) << CScriptNum(0));
    const auto creating = MakeTransactionRef(mint);
    const auto lookup = [&](const uint256& hash, uint32_t height, CTransactionRef& tx) {
        if (hash != creating->GetHash() || height != 2) return false;
        tx = creating;
        return true;
    };
    CCoinsViewDB db{{.path = m_args.GetDataDirNet() / "dd-reconstruct", .cache_bytes = 1 << 20, .memory_only = true}, {}};
    CCoinsViewCache cache{&db};
    cache.SetBestBlock(uint256S("10"));
    AddCoins(cache, *creating, 2);
    DigiDollar::ChainstateHealth health;
    std::string error;
    CAmount supply{-1};
    const auto untouched_health = health;
    bool cancel_at_completion{false};
    unsigned int progress_calls{0};
    BOOST_CHECK(!DigiDollar::ReconstructChainstateHealth(cache, params, lookup, health, error,
        [&] { return cancel_at_completion; }, &supply,
        [&](uint64_t completed, uint64_t total) {
            ++progress_calls;
            if (total != 0) {
                BOOST_CHECK_EQUAL(completed, total);
                cancel_at_completion = true;
            }
        }));
    BOOST_CHECK_EQUAL(progress_calls, 2U);
    BOOST_CHECK(health == untouched_health);
    BOOST_CHECK_EQUAL(supply, -1);
    BOOST_CHECK(error.find("cancelled") != std::string::npos);
    BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(cache, params, lookup, health, error, {}, &supply), error);
    BOOST_CHECK_EQUAL(health.open_vault_principal, 10000);
    BOOST_CHECK_EQUAL(health.collateral, 100 * COIN);
    BOOST_CHECK_EQUAL(health.active_vaults, 1U);
    BOOST_CHECK_EQUAL(supply, 10000);
    BOOST_CHECK(!health.history_checked);
    BOOST_REQUIRE(cache.SpendCoin(COutPoint{creating->GetHash(), 1}));
    BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(cache, params, lookup, health, error, {}, &supply), error);
    BOOST_CHECK_EQUAL(health.active_vaults, 0U);
    BOOST_CHECK_EQUAL(supply, 10000);
    BOOST_REQUIRE(cache.SpendCoin(COutPoint{creating->GetHash(), 2}));
    BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(cache, params, lookup, health, error, {}, &supply), error);
    BOOST_CHECK_EQUAL(supply, 0);
    cache.AddCoin(COutPoint{creating->GetHash(), 1}, Coin{creating->vout[1], 2, false}, false);
    const auto before = health;
    BOOST_CHECK(!DigiDollar::ReconstructChainstateHealth(cache, params, {}, health, error));
    BOOST_CHECK(health == before);
    BOOST_CHECK(error.find("height 2") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
