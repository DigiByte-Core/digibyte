// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <wallet/wallet.h>
#include <wallet/digidollarwallet.h>
#include <wallet/walletdb.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <addresstype.h>
#include <base58.h>
#include <hash.h>
#include <key.h>
#include <random.h>
#include <script/script.h>
#include <util/time.h>
#include <util/strencodings.h>
#include <streams.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <test/util/source_root.h>

namespace wallet {

/**
 * Helper: derive the database key for a CDigiDollarAddress the same way
 * DigiDollarWallet::WriteDDBalance does (Hash-based fallback for test addresses
 * whose ToString() returns empty).
 */
static std::string DDBalanceKey(const CDigiDollarAddress& addr)
{
    std::string key = addr.ToString();
    if (key.empty()) {
        CDataStream ss(SER_DISK, CLIENT_VERSION);
        ss << addr;
        uint256 hash = Hash(ss);
        key = "test_addr_" + hash.GetHex();
    }
    return key;
}

BOOST_FIXTURE_TEST_SUITE(digidollar_persistence_wallet_tests, WalletTestingSetup)

// =============================================================================
// PHASE 3 TASK 3.1: WriteDDBalance Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(digidollarwallet_write_balance_persists)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CDigiDollarAddress addr("DD1qtest123456789abcdefghijklmnopqrstuvwxyz");
    CAmount balance = 10000; // $100.00

    BOOST_CHECK(dd_wallet.WriteDDBalance(addr, balance));

    // Verify written to database using the same key derivation as the impl
    WalletBatch batch(m_wallet.GetDatabase());
    WalletDDBalance read_balance;
    std::string key = DDBalanceKey(addr);
    BOOST_CHECK(batch.ReadDDBalance(key, read_balance));
    BOOST_CHECK_EQUAL(read_balance.balance, balance);
    BOOST_CHECK(read_balance.last_updated > 0);

    // Verify in-memory cache updated
    BOOST_CHECK_EQUAL(dd_wallet.GetDDBalance(addr), balance);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_balance_updates_total)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CDigiDollarAddress addr1("DD1qtest111111111111111111111111111111111");
    CDigiDollarAddress addr2("DD1qtest222222222222222222222222222222222");

    BOOST_CHECK(dd_wallet.WriteDDBalance(addr1, 5000));
    BOOST_CHECK(dd_wallet.WriteDDBalance(addr2, 3000));

    BOOST_CHECK_EQUAL(dd_wallet.GetDDBalance(addr1), 5000);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDBalance(addr2), 3000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_balance_updates_existing)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CDigiDollarAddress addr("DD1qtest333333333333333333333333333333333");

    BOOST_CHECK(dd_wallet.WriteDDBalance(addr, 1000));
    BOOST_CHECK_EQUAL(dd_wallet.GetDDBalance(addr), 1000);

    BOOST_CHECK(dd_wallet.WriteDDBalance(addr, 2000));
    BOOST_CHECK_EQUAL(dd_wallet.GetDDBalance(addr), 2000);

    // Verify database has updated value
    WalletBatch batch(m_wallet.GetDatabase());
    WalletDDBalance read_balance;
    std::string key = DDBalanceKey(addr);
    BOOST_CHECK(batch.ReadDDBalance(key, read_balance));
    BOOST_CHECK_EQUAL(read_balance.balance, 2000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_balance_persists_total_metadata)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CDigiDollarAddress addr1("DD1qtest444444444444444444444444444444444");
    CDigiDollarAddress addr2("DD1qtest555555555555555555555555555555555");

    BOOST_CHECK(dd_wallet.WriteDDBalance(addr1, 7500));
    BOOST_CHECK(dd_wallet.WriteDDBalance(addr2, 2500));

    WalletBatch batch(m_wallet.GetDatabase());
    std::string total_str;
    BOOST_CHECK(batch.ReadDDMetadata("total_dd_balance", total_str));

    CAmount total = std::stoll(total_str);
    BOOST_CHECK_EQUAL(total, 10000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_balance_empty_address_uses_test_key)
{
    // The implementation does NOT reject empty addresses — it generates a
    // hash-based test key so mock addresses work.  Verify that behaviour.
    DigiDollarWallet dd_wallet(&m_wallet);

    CDigiDollarAddress empty_addr;
    BOOST_CHECK(dd_wallet.WriteDDBalance(empty_addr, 1000));
    BOOST_CHECK_EQUAL(dd_wallet.GetDDBalance(empty_addr), 1000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_balance_negative_amount)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CDigiDollarAddress addr("DD1qtest666666666666666666666666666666666");

    BOOST_CHECK(!dd_wallet.WriteDDBalance(addr, -1000));
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_balance_zero_amount)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CDigiDollarAddress addr("DD1qtest777777777777777777777777777777777");

    BOOST_CHECK(dd_wallet.WriteDDBalance(addr, 0));
    BOOST_CHECK_EQUAL(dd_wallet.GetDDBalance(addr), 0);
}

// =============================================================================
// PHASE 3 TASK 3.2: WriteDDTimeLock Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(digidollarwallet_write_position_persists)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 pos_id = uint256S("0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    WalletCollateralPosition pos(pos_id, 10000, 500000, 3, 100000);

    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos));

    WalletBatch batch(m_wallet.GetDatabase());
    WalletCollateralPosition read_pos;
    BOOST_CHECK(batch.ReadDDTimeLock(pos_id, read_pos));
    BOOST_CHECK_EQUAL(read_pos.dd_minted, 10000);
    BOOST_CHECK_EQUAL(read_pos.dgb_collateral, 500000);

    auto positions = dd_wallet.GetDDTimeLocks(false);
    BOOST_CHECK_EQUAL(positions.size(), 1);
    BOOST_CHECK_EQUAL(positions[0].dd_minted, 10000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_position_updates_locked_collateral)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 id1 = uint256S("0x1111111111111111111111111111111111111111111111111111111111111111");
    uint256 id2 = uint256S("0x2222222222222222222222222222222222222222222222222222222222222222");

    WalletCollateralPosition pos1(id1, 5000, 250000, 2, 50000);
    WalletCollateralPosition pos2(id2, 3000, 150000, 1, 30000);

    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos1));
    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos2));

    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 400000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_position_updates_existing)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 pos_id = uint256S("0x3333333333333333333333333333333333333333333333333333333333333333");

    WalletCollateralPosition pos1(pos_id, 1000, 50000, 1, 10000);
    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos1));

    auto positions = dd_wallet.GetDDTimeLocks(false);
    BOOST_CHECK_EQUAL(positions.size(), 1);
    BOOST_CHECK_EQUAL(positions[0].dd_minted, 1000);

    WalletCollateralPosition pos2(pos_id, 2000, 100000, 2, 20000);
    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos2));

    positions = dd_wallet.GetDDTimeLocks(false);
    BOOST_CHECK_EQUAL(positions.size(), 1);
    BOOST_CHECK_EQUAL(positions[0].dd_minted, 2000);
    BOOST_CHECK_EQUAL(positions[0].dgb_collateral, 100000);

    WalletBatch batch(m_wallet.GetDatabase());
    WalletCollateralPosition read_pos;
    BOOST_CHECK(batch.ReadDDTimeLock(pos_id, read_pos));
    BOOST_CHECK_EQUAL(read_pos.dd_minted, 2000);
    BOOST_CHECK_EQUAL(read_pos.dgb_collateral, 100000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_position_persists_metadata)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 pos_id = uint256S("0x4444444444444444444444444444444444444444444444444444444444444444");
    WalletCollateralPosition pos(pos_id, 7500, 375000, 3, 75000);

    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos));

    WalletBatch batch(m_wallet.GetDatabase());
    std::string locked_str;
    BOOST_CHECK(batch.ReadDDMetadata("locked_collateral", locked_str));

    CAmount locked = std::stoll(locked_str);
    BOOST_CHECK_EQUAL(locked, 375000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_position_invalid_id)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 null_id;
    WalletCollateralPosition pos(null_id, 1000, 50000, 1, 10000);

    BOOST_CHECK(!dd_wallet.WriteDDTimeLock(pos));
}

BOOST_AUTO_TEST_CASE(digidollarwallet_write_position_inactive_no_locked)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 pos_id = uint256S("0x5555555555555555555555555555555555555555555555555555555555555555");
    WalletCollateralPosition pos(pos_id, 1000, 50000, 1, 10000);
    pos.is_active = false;

    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos));

    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 0);
}

// =============================================================================
// PHASE 3 TASK 3.3: UpdatePositionStatus Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(digidollarwallet_update_position_status)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 pos_id = uint256S("0x3333333333333333333333333333333333333333333333333333333333333333");
    WalletCollateralPosition pos(pos_id, 10000, 500000, 3, 100000);
    BOOST_REQUIRE(dd_wallet.WriteDDTimeLock(pos));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 500000);

    BOOST_CHECK(dd_wallet.UpdatePositionStatus(pos_id, false));

    WalletBatch batch(m_wallet.GetDatabase());
    WalletCollateralPosition read_pos;
    BOOST_CHECK(batch.ReadDDTimeLock(pos_id, read_pos));
    BOOST_CHECK(!read_pos.is_active);

    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 0);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_update_position_status_multiple)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 id1 = uint256S("0x4444444444444444444444444444444444444444444444444444444444444444");
    uint256 id2 = uint256S("0x5555555555555555555555555555555555555555555555555555555555555555");
    uint256 id3 = uint256S("0x6666666666666666666666666666666666666666666666666666666666666666");

    dd_wallet.WriteDDTimeLock(WalletCollateralPosition(id1, 5000, 250000, 2, 50000));
    dd_wallet.WriteDDTimeLock(WalletCollateralPosition(id2, 3000, 150000, 1, 30000));
    dd_wallet.WriteDDTimeLock(WalletCollateralPosition(id3, 2000, 100000, 1, 20000));

    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 500000);

    dd_wallet.UpdatePositionStatus(id2, false);

    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 350000);

    WalletBatch batch(m_wallet.GetDatabase());
    WalletCollateralPosition read_pos;
    BOOST_CHECK(batch.ReadDDTimeLock(id2, read_pos));
    BOOST_CHECK(!read_pos.is_active);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_update_position_status_not_found)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 fake_id = uint256S("0x7777777777777777777777777777777777777777777777777777777777777777");
    BOOST_CHECK(!dd_wallet.UpdatePositionStatus(fake_id, false));
}

BOOST_AUTO_TEST_CASE(digidollarwallet_update_position_status_null_id)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 null_id;
    BOOST_CHECK(!dd_wallet.UpdatePositionStatus(null_id, false));
}

BOOST_AUTO_TEST_CASE(digidollarwallet_update_position_status_reactivate)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    uint256 pos_id = uint256S("0x8888888888888888888888888888888888888888888888888888888888888888");
    WalletCollateralPosition pos(pos_id, 10000, 500000, 3, 100000);
    BOOST_REQUIRE(dd_wallet.WriteDDTimeLock(pos));

    BOOST_CHECK(dd_wallet.UpdatePositionStatus(pos_id, false));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 0);

    BOOST_CHECK(dd_wallet.UpdatePositionStatus(pos_id, true));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 500000);

    WalletBatch batch(m_wallet.GetDatabase());
    WalletCollateralPosition read_pos;
    BOOST_CHECK(batch.ReadDDTimeLock(pos_id, read_pos));
    BOOST_CHECK(read_pos.is_active);
}

// =============================================================================
// PHASE 4 TASK 4.1: LoadFromDatabase Tests - THE CRITICAL METHOD
// =============================================================================

BOOST_AUTO_TEST_CASE(digidollarwallet_load_from_database_positions)
{
    wallet::WalletBatch batch(m_wallet.GetDatabase());
    uint256 id1 = uint256S("0x8888888888888888888888888888888888888888888888888888888888888888");
    uint256 id2 = uint256S("0x9999999999999999999999999999999999999999999999999999999999999999");

    WalletCollateralPosition pos1(id1, 10000, 500000, 3, 100000);
    WalletCollateralPosition pos2(id2, 5000, 250000, 2, 50000);
    batch.WriteDDTimeLock(pos1);
    batch.WriteDDTimeLock(pos2);

    DigiDollarWallet fresh_wallet(&m_wallet);

    size_t loaded = fresh_wallet.LoadFromDatabase();
    BOOST_CHECK_EQUAL(loaded, 2);

    auto positions = fresh_wallet.GetDDTimeLocks();
    BOOST_CHECK_EQUAL(positions.size(), 2);
    BOOST_CHECK_EQUAL(fresh_wallet.GetLockedCollateral(), 750000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_load_from_database_balances)
{
    // Write balances using the same key derivation as the implementation
    wallet::WalletBatch batch(m_wallet.GetDatabase());

    CDigiDollarAddress addr1("DD1qtest111111111111111111111111111111111");
    CDigiDollarAddress addr2("DD1qtest222222222222222222222222222222222");

    WalletDDBalance bal1(addr1, 10000);
    bal1.last_updated = GetTime();
    WalletDDBalance bal2(addr2, 5000);
    bal2.last_updated = GetTime();

    batch.WriteDDBalance(DDBalanceKey(addr1), bal1);
    batch.WriteDDBalance(DDBalanceKey(addr2), bal2);

    DigiDollarWallet fresh_wallet(&m_wallet);

    size_t loaded = fresh_wallet.LoadFromDatabase();
    BOOST_CHECK_EQUAL(loaded, 2);

    BOOST_CHECK_EQUAL(fresh_wallet.GetBalanceCount(), 2);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_load_from_database_mixed)
{
    wallet::WalletBatch batch(m_wallet.GetDatabase());

    uint256 pos_id = uint256S("0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    WalletCollateralPosition pos(pos_id, 10000, 500000, 3, 100000);
    batch.WriteDDTimeLock(pos);

    CDigiDollarAddress addr("DD1qtest333333333333333333333333333333333");
    WalletDDBalance bal(addr, 7500);
    bal.last_updated = GetTime();
    batch.WriteDDBalance(DDBalanceKey(addr), bal);

    DigiDollarWallet fresh_wallet(&m_wallet);

    size_t loaded = fresh_wallet.LoadFromDatabase();
    BOOST_CHECK_EQUAL(loaded, 2);

    BOOST_CHECK_EQUAL(fresh_wallet.GetPositionCount(), 1);
    BOOST_CHECK_EQUAL(fresh_wallet.GetBalanceCount(), 1);
    BOOST_CHECK_EQUAL(fresh_wallet.GetLockedCollateral(), 500000);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_load_from_database_empty)
{
    DigiDollarWallet empty_wallet(&m_wallet);

    size_t loaded = empty_wallet.LoadFromDatabase();
    BOOST_CHECK_EQUAL(loaded, 0);

    BOOST_CHECK_EQUAL(empty_wallet.GetPositionCount(), 0);
    BOOST_CHECK_EQUAL(empty_wallet.GetBalanceCount(), 0);
    BOOST_CHECK_EQUAL(empty_wallet.GetLockedCollateral(), 0);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_load_from_database_recalculates_totals)
{
    wallet::WalletBatch batch(m_wallet.GetDatabase());

    uint256 id1 = uint256S("0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    uint256 id2 = uint256S("0xcccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    uint256 id3 = uint256S("0xdddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");

    WalletCollateralPosition pos1(id1, 10000, 500000, 3, 100000);
    pos1.is_active = true;

    WalletCollateralPosition pos2(id2, 5000, 250000, 2, 50000);
    pos2.is_active = false;

    WalletCollateralPosition pos3(id3, 3000, 150000, 1, 30000);
    pos3.is_active = true;

    batch.WriteDDTimeLock(pos1);
    batch.WriteDDTimeLock(pos2);
    batch.WriteDDTimeLock(pos3);

    DigiDollarWallet fresh_wallet(&m_wallet);
    fresh_wallet.LoadFromDatabase();

    BOOST_CHECK_EQUAL(fresh_wallet.GetLockedCollateral(), 650000);

    auto all_positions = fresh_wallet.GetDDTimeLocks(false);
    BOOST_CHECK_EQUAL(all_positions.size(), 3);

    auto active_positions = fresh_wallet.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(active_positions.size(), 2);
}

BOOST_AUTO_TEST_CASE(digidollarwallet_persistence_integration_test)
{
    // THE KEY TEST: Write → Clear → Load → Data restored

    DigiDollarWallet wallet1(&m_wallet);

    uint256 pos_id = uint256S("0xeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    WalletCollateralPosition pos(pos_id, 10000, 500000, 3, 100000);
    wallet1.WriteDDTimeLock(pos);

    CDigiDollarAddress addr("DD1qtest999999999999999999999999999999999");
    wallet1.WriteDDBalance(addr, 10000);

    BOOST_CHECK_EQUAL(wallet1.GetPositionCount(), 1);
    BOOST_CHECK_EQUAL(wallet1.GetBalanceCount(), 1);
    BOOST_CHECK_EQUAL(wallet1.GetLockedCollateral(), 500000);

    wallet1.ClearWalletData();
    BOOST_CHECK_EQUAL(wallet1.GetPositionCount(), 0);
    BOOST_CHECK_EQUAL(wallet1.GetBalanceCount(), 0);
    BOOST_CHECK_EQUAL(wallet1.GetLockedCollateral(), 0);

    DigiDollarWallet wallet2(&m_wallet);
    size_t loaded = wallet2.LoadFromDatabase();
    BOOST_CHECK_EQUAL(loaded, 2);

    BOOST_CHECK_EQUAL(wallet2.GetPositionCount(), 1);
    BOOST_CHECK_EQUAL(wallet2.GetBalanceCount(), 1);
    BOOST_CHECK_EQUAL(wallet2.GetLockedCollateral(), 500000);

    auto positions = wallet2.GetDDTimeLocks();
    BOOST_CHECK_EQUAL(positions.size(), 1);
    BOOST_CHECK_EQUAL(positions[0].dd_timelock_id, pos_id);
    BOOST_CHECK_EQUAL(positions[0].dd_minted, 10000);
    BOOST_CHECK_EQUAL(positions[0].dgb_collateral, 500000);
}

// =============================================================================
// Mint attempt records: writes report failure, and a released attempt frees
// only what it reserved while keeping its key and history.
// =============================================================================

namespace {

/** A transaction shaped like a mint: DD version, one funded P2TR collateral
 *  output and one zero-value P2TR token output tweaked from owner_key. */
CTransactionRef MakeMintShapedTx(const CKey& owner_key)
{
    CKey collateral_key;
    collateral_key.MakeNewKey(true);
    const XOnlyPubKey collateral_xonly(collateral_key.GetPubKey());
    const XOnlyPubKey owner_xonly(owner_key.GetPubKey());
    const auto tweaked = owner_xonly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());

    CMutableTransaction mtx;
    mtx.SetDigiDollarType(::DD_TX_MINT);
    uint256 prev;
    GetRandBytes(prev);
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(prev, 0);
    mtx.vout.resize(2);
    mtx.vout[0].nValue = 5 * COIN;
    mtx.vout[0].scriptPubKey << OP_1 << ToByteVector(collateral_xonly);
    mtx.vout[1].nValue = 0;
    mtx.vout[1].scriptPubKey << OP_1 << ToByteVector(tweaked->first);
    return MakeTransactionRef(std::move(mtx));
}

} // namespace

BOOST_AUTO_TEST_CASE(mint_records_are_not_cached_when_the_database_refuses_them)
{
    // A position or key that only exists in memory looks fine until the next
    // restart and then vanishes; a refused write must leave no trace so the
    // caller (a mint about to be broadcast) can stop.
    DigiDollarWallet dd_wallet(&m_wallet);
    const uint256 pos_id = uint256S("0x1111111111111111111111111111111111111111111111111111111111111111");
    WalletCollateralPosition pos(pos_id, 10000, 500000, 0, 1000);
    CKey owner_key;
    owner_key.MakeNewKey(true);

    GetMockableDatabase(m_wallet).m_pass = false;

    BOOST_CHECK(!dd_wallet.WriteDDTimeLock(pos));
    BOOST_CHECK_EQUAL(dd_wallet.GetPositionCount(), 0);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 0);

    dd_wallet.StoreOwnerKey(pos_id, owner_key);
    CKey reloaded;
    BOOST_CHECK(!dd_wallet.GetOwnerKey(pos_id, reloaded));

    dd_wallet.AddCollateralPosition(pos);
    BOOST_CHECK_EQUAL(dd_wallet.GetPositionCount(), 0);
    BOOST_CHECK(!dd_wallet.HasDDUTXO(COutPoint(pos_id, 1)));

    // Once the database accepts writes again everything is saved and visible.
    GetMockableDatabase(m_wallet).m_pass = true;
    BOOST_CHECK(dd_wallet.WriteDDTimeLock(pos));
    BOOST_CHECK_EQUAL(dd_wallet.GetPositionCount(), 1);
    BOOST_CHECK(dd_wallet.StoreOwnerKey(pos_id, owner_key));
    BOOST_CHECK(dd_wallet.GetOwnerKey(pos_id, reloaded));
    BOOST_CHECK(reloaded.GetPubKey() == owner_key.GetPubKey());

    DigiDollarWallet fresh(&m_wallet);
    BOOST_CHECK_EQUAL(fresh.GetPositionCount(), 1);
    BOOST_CHECK(fresh.GetOwnerKey(pos_id, reloaded));
}

BOOST_AUTO_TEST_CASE(pending_mint_record_reports_failure_and_release_keeps_recovery_data)
{
    DigiDollarWallet dd_wallet(&m_wallet);
    CKey owner_key;
    owner_key.MakeNewKey(true);
    const CTransactionRef mint_tx = MakeMintShapedTx(owner_key);
    const uint256 pos_id = mint_tx->GetHash();
    const COutPoint collateral(pos_id, 0);
    const COutPoint token(pos_id, 1);
    WalletCollateralPosition pos(pos_id, 10000, 5 * COIN, 0, 1000);
    pos.owner_keyid = owner_key.GetPubKey().GetID();
    std::string error;

    // A refused database leaves nothing behind and says so.
    GetMockableDatabase(m_wallet).m_pass = false;
    BOOST_CHECK(!dd_wallet.RecordPendingMint(*mint_tx, pos, owner_key, error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(dd_wallet.GetPositionCount(), 0);
    CKey reloaded;
    BOOST_CHECK(!dd_wallet.GetOwnerKey(pos_id, reloaded));
    GetMockableDatabase(m_wallet).m_pass = true;

    // A working database records key, position, token output and history
    // before the transaction exists anywhere in the wallet.
    BOOST_REQUIRE(dd_wallet.RecordPendingMint(*mint_tx, pos, owner_key, error));
    BOOST_CHECK(dd_wallet.GetOwnerKey(pos_id, reloaded));
    BOOST_CHECK_EQUAL(dd_wallet.GetPositionCount(), 1);
    BOOST_CHECK(dd_wallet.HasDDUTXO(token));
    BOOST_CHECK(dd_wallet.GetMintAttemptState(pos_id) == DigiDollarWallet::MintAttemptState::NotInWallet);

    // The transaction is committed locally and, as a restart would, its
    // collateral and token outputs get locked. The wallet is told it is at
    // height 500, which a wallet on a running node always knows; tier 0 locks
    // 240 blocks and the unlock height is 1000, so the window is still open.
    uint256 chain_tip;
    GetRandBytes(chain_tip);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(500, chain_tip);
        BOOST_REQUIRE(m_wallet.AddToWallet(mint_tx, TxStateInactive{}));
        BOOST_CHECK(m_wallet.LockCoin(collateral));
        BOOST_CHECK(m_wallet.LockCoin(token));
    }
    BOOST_CHECK(dd_wallet.GetMintAttemptState(pos_id) == DigiDollarWallet::MintAttemptState::Local);

    // Releasing the attempt frees the reservations but keeps the key, the
    // transaction and the (now inactive) position record.
    BOOST_CHECK(dd_wallet.ReleaseMintAttempt(pos_id, error));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(!m_wallet.IsLockedCoin(token));
        const CWalletTx* wtx = m_wallet.GetWalletTx(pos_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(wtx->isAbandoned());
    }
    BOOST_CHECK(!dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 0);
    const auto all = dd_wallet.GetDDTimeLocks(/*active_only=*/false);
    BOOST_REQUIRE_EQUAL(all.size(), 1);
    BOOST_CHECK(!all[0].is_active);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 0);
    BOOST_CHECK(dd_wallet.GetOwnerKey(pos_id, reloaded));
    BOOST_CHECK(dd_wallet.GetMintAttemptState(pos_id) == DigiDollarWallet::MintAttemptState::Abandoned);

    // The released state is what a reload sees.
    DigiDollarWallet fresh(&m_wallet);
    const auto reloaded_positions = fresh.GetDDTimeLocks(/*active_only=*/false);
    BOOST_REQUIRE_EQUAL(reloaded_positions.size(), 1);
    BOOST_CHECK(!reloaded_positions[0].is_active);
    BOOST_CHECK(!fresh.HasDDUTXO(token));
    BOOST_CHECK(fresh.GetOwnerKey(pos_id, reloaded));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(collateral));
    }
}

BOOST_AUTO_TEST_CASE(mint_attempt_state_follows_the_lock_window)
{
    DigiDollarWallet dd_wallet(&m_wallet);
    CKey owner_key;
    owner_key.MakeNewKey(true);
    const CTransactionRef mint_tx = MakeMintShapedTx(owner_key);
    const uint256 pos_id = mint_tx->GetHash();
    // Tier 0 locks 240 blocks; with unlock height 1000 a block may include
    // the mint only while at least 240 blocks remain, i.e. up to height 760.
    WalletCollateralPosition pos(pos_id, 10000, 5 * COIN, 0, 1000);
    std::string error;
    BOOST_REQUIRE(dd_wallet.RecordPendingMint(*mint_tx, pos, owner_key, error));

    BOOST_CHECK(!DigiDollarWallet::MintLockWindowHasPassed(pos, 760));
    BOOST_CHECK(DigiDollarWallet::MintLockWindowHasPassed(pos, 761));

    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(mint_tx, TxStateInactive{}));
        m_wallet.SetLastBlockProcessed(759, block_hash);
    }
    BOOST_CHECK(dd_wallet.GetMintAttemptState(pos_id) == DigiDollarWallet::MintAttemptState::Local);
    BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 0);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 1);

    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(760, block_hash);
    }
    BOOST_CHECK(dd_wallet.GetMintAttemptState(pos_id) == DigiDollarWallet::MintAttemptState::Expired);
    BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 1);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 0);
    BOOST_CHECK(dd_wallet.GetMintAttemptState(pos_id) == DigiDollarWallet::MintAttemptState::Expired);
    {
        LOCK(m_wallet.cs_wallet);
        const CWalletTx* wtx = m_wallet.GetWalletTx(pos_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(wtx->isAbandoned());
    }
    // Nothing more to release on a second pass.
    BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 0);

    // A shorter chain reopens the window: the attempt stays abandoned, but it
    // is no longer reported as expired.
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(700, block_hash);
    }
    BOOST_CHECK(dd_wallet.GetMintAttemptState(pos_id) == DigiDollarWallet::MintAttemptState::Abandoned);

    // A confirmed mint is never released.
    CKey other_owner;
    other_owner.MakeNewKey(true);
    const CTransactionRef confirmed_tx = MakeMintShapedTx(other_owner);
    const uint256 confirmed_id = confirmed_tx->GetHash();
    WalletCollateralPosition confirmed_pos(confirmed_id, 10000, 5 * COIN, 0, 1000);
    BOOST_REQUIRE(dd_wallet.RecordPendingMint(*confirmed_tx, confirmed_pos, other_owner, error));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(confirmed_tx, TxStateConfirmed{block_hash, 650, /*index=*/1}));
    }
    BOOST_CHECK(dd_wallet.GetMintAttemptState(confirmed_id) == DigiDollarWallet::MintAttemptState::Confirmed);
    BOOST_CHECK(!dd_wallet.ReleaseMintAttempt(confirmed_id, error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 0);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 1);
}

// =============================================================================
// Paying DigiDollar to an address in your own wallet
// =============================================================================

namespace {

/** A zero value taproot output whose key is the taproot tweak of owner_key. */
CTxOut MakeDDTokenOutput(const CKey& owner_key)
{
    const XOnlyPubKey owner_xonly(owner_key.GetPubKey());
    const auto tweaked = owner_xonly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    CTxOut out;
    out.nValue = 0;
    out.scriptPubKey << OP_1 << ToByteVector(tweaked->first);
    return out;
}

/** The key that can spend one DigiDollar output, found the way the wallet finds
 *  it when it signs: by the output's own key first, then by the key saved for
 *  the transaction, which is only used when it really does control the output. */
bool FindSpendingKey(DigiDollarWallet& dd_wallet, const CTxOut& out, const uint256& txid, CKey& found)
{
    const std::vector<unsigned char> output_key_bytes(out.scriptPubKey.begin() + 2, out.scriptPubKey.end());
    const XOnlyPubKey output_key(output_key_bytes);

    if (dd_wallet.GetAddressKey(output_key, found)) {
        return true;
    }
    CKey candidate;
    if (dd_wallet.GetOwnerKey(txid, candidate)) {
        const XOnlyPubKey candidate_xonly(candidate.GetPubKey());
        const auto tweaked = candidate_xonly.CreateTapTweak(nullptr);
        if (tweaked && tweaked->first == output_key) {
            found = candidate;
            return true;
        }
    }
    return false;
}

} // namespace

BOOST_AUTO_TEST_CASE(self_send_saves_one_owner_key_and_keeps_both_outputs_spendable)
{
    // Paying DigiDollar to an address in your own wallet makes two outputs the
    // wallet owns: what was sent, and the change. The wallet walks both and
    // offers the same key for the transaction each time. The second offer is a
    // repeat of a record already made, so it must succeed quietly instead of
    // failing and writing errors into the log.
    DigiDollarWallet dd_wallet(&m_wallet);

    CKey recipient_key;
    recipient_key.MakeNewKey(true);
    CKey change_key;
    change_key.MakeNewKey(true);

    const CTxOut recipient_out = MakeDDTokenOutput(recipient_key);
    const CTxOut change_out = MakeDDTokenOutput(change_key);

    // The address the payment went to is one this wallet handed out, so its key
    // is held against the output, not against the transaction.
    const std::vector<unsigned char> recipient_key_bytes(recipient_out.scriptPubKey.begin() + 2,
                                                         recipient_out.scriptPubKey.end());
    BOOST_REQUIRE(dd_wallet.StoreAddressKey(XOnlyPubKey(recipient_key_bytes), recipient_key));

    uint256 txid;
    GetRandBytes(txid);

    // Both offers of the same key succeed, and only one record is kept.
    BOOST_CHECK(dd_wallet.StoreOwnerKey(txid, change_key));
    BOOST_CHECK_MESSAGE(dd_wallet.StoreOwnerKey(txid, change_key),
                        "saving the same owner key again for the second owned output must not fail");

    CKey saved;
    BOOST_REQUIRE(dd_wallet.GetOwnerKey(txid, saved));
    BOOST_CHECK(saved.GetPubKey() == change_key.GetPubKey());

    // The wallet still knows both outputs are its own.
    BOOST_CHECK(dd_wallet.IsDDOutputMine(recipient_out, txid));
    BOOST_CHECK(dd_wallet.IsDDOutputMine(change_out, txid));

    // And it can still produce the right private key for each of them, which is
    // what it needs to spend or redeem them.
    CKey for_recipient;
    BOOST_REQUIRE(FindSpendingKey(dd_wallet, recipient_out, txid, for_recipient));
    BOOST_CHECK(for_recipient.GetPubKey() == recipient_key.GetPubKey());

    CKey for_change;
    BOOST_REQUIRE(FindSpendingKey(dd_wallet, change_out, txid, for_change));
    BOOST_CHECK(for_change.GetPubKey() == change_key.GetPubKey());

    // A different key under the same transaction id would throw away the record
    // of who owns that vault, so it is still refused.
    CKey stranger;
    stranger.MakeNewKey(true);
    BOOST_CHECK(!dd_wallet.StoreOwnerKey(txid, stranger));
    BOOST_REQUIRE(dd_wallet.GetOwnerKey(txid, saved));
    BOOST_CHECK(saved.GetPubKey() == change_key.GetPubKey());

    // Everything survives a reload from the wallet database.
    DigiDollarWallet reloaded(&m_wallet);
    CKey after_restart;
    BOOST_REQUIRE(reloaded.GetOwnerKey(txid, after_restart));
    BOOST_CHECK(after_restart.GetPubKey() == change_key.GetPubKey());
    CKey recipient_after_restart;
    BOOST_REQUIRE(FindSpendingKey(reloaded, recipient_out, txid, recipient_after_restart));
    BOOST_CHECK(recipient_after_restart.GetPubKey() == recipient_key.GetPubKey());
}

BOOST_AUTO_TEST_CASE(self_send_owner_key_is_saved_once_on_an_encrypted_wallet)
{
    // On an encrypted wallet the database refuses to replace an owner key that
    // is already there. A payment to one of your own addresses offers the same
    // key twice, once for each output the wallet owns, so the second offer used
    // to come back as a database failure and put two error lines in the log for
    // what is an ordinary, successful payment.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet* dd_wallet = m_wallet.GetDDWallet();
    BOOST_REQUIRE(dd_wallet != nullptr);

    SecureString passphrase{"self-send-owner-key-passphrase"};
    BOOST_REQUIRE(m_wallet.EncryptWallet(passphrase));
    BOOST_REQUIRE(m_wallet.IsCrypted());
    BOOST_REQUIRE(m_wallet.Unlock(passphrase));

    CKey change_key;
    change_key.MakeNewKey(true);
    uint256 txid;
    GetRandBytes(txid);

    BOOST_CHECK(dd_wallet->StoreOwnerKey(txid, change_key));
    BOOST_CHECK_MESSAGE(dd_wallet->StoreOwnerKey(txid, change_key),
                        "saving the same owner key again for the second owned output must not fail");

    CKey saved;
    BOOST_REQUIRE(dd_wallet->GetOwnerKey(txid, saved));
    BOOST_CHECK(saved.GetPubKey() == change_key.GetPubKey());

    // A different key would throw away the record of who owns that vault.
    CKey stranger;
    stranger.MakeNewKey(true);
    BOOST_CHECK(!dd_wallet->StoreOwnerKey(txid, stranger));
    BOOST_REQUIRE(dd_wallet->GetOwnerKey(txid, saved));
    BOOST_CHECK(saved.GetPubKey() == change_key.GetPubKey());
}

//! A payment and a redemption that cannot go ahead now say why.
//!
//! Both of these used to answer a caller with nothing but "false". The reason
//! went to the node log and nowhere else, so whatever asked for the payment had
//! nothing to put in front of the user.
BOOST_AUTO_TEST_CASE(transfer_and_redeem_say_why_they_were_refused)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CKey recipient_key;
    recipient_key.MakeNewKey(true);
    const XOnlyPubKey recipient_xonly(recipient_key.GetPubKey());
    const CDigiDollarAddress to(EncodeDigiDollarAddress(CTxDestination{WitnessV1Taproot(recipient_xonly)}));
    BOOST_REQUIRE(to.IsValidForCurrentNetwork());

    CTransactionRef tx_out;
    std::string error{"not touched yet"};

    // The wallet holds no DigiDollars, so a payment of $100 cannot be made and
    // the reason says both what was asked for and what is there.
    BOOST_CHECK(!dd_wallet.TransferDigiDollar(to, 10000, tx_out, &error));
    BOOST_CHECK_MESSAGE(error.find("cannot send 10000 cents") != std::string::npos,
                        "a refused payment must say how much it was asked for, got: " << error);
    BOOST_CHECK_MESSAGE(error.find("holds 0 cents") != std::string::npos,
                        "a refused payment must say how much the wallet holds, got: " << error);

    // An address from another network is refused by name.
    const CDigiDollarAddress not_ours("DD1qtest123456789abcdefghijklmnopqrstuvwxyz");
    error = "not touched yet";
    BOOST_CHECK(!dd_wallet.TransferDigiDollar(not_ours, 10000, tx_out, &error));
    BOOST_CHECK_EQUAL(error, "That is not a DigiDollar address for this network.");

    // The wallet has no vault with this identifier, so it cannot redeem one.
    const uint256 unknown_vault = uint256::ONE;
    error = "not touched yet";
    BOOST_CHECK(!dd_wallet.RedeemDigiDollar(unknown_vault, 10000, tx_out, &error));
    BOOST_CHECK_MESSAGE(!error.empty(), "a refused redemption must say why");
    BOOST_CHECK(error.find(unknown_vault.ToString()) != std::string::npos);

    // A caller that does not want the reason can still leave it out.
    BOOST_CHECK(!dd_wallet.TransferDigiDollar(to, 10000, tx_out));
    BOOST_CHECK(!dd_wallet.RedeemDigiDollar(unknown_vault, 10000, tx_out));
}

//! Saving the same DigiDollar owner key a second time is the ordinary case.
//!
//! A payment to one of your own addresses has two outputs the wallet owns, and
//! the wallet offers the same owner key for each of them. The second offer is a
//! repeat of a record already made, which is normal and not a fault, so the
//! message that says so must not print on a node running with default logging.
//! Its neighbours in the same function are behind the DigiDollar log category
//! and this one has to be too.
BOOST_AUTO_TEST_CASE(the_repeat_owner_key_message_stays_off_by_default)
{
    const std::string source = ReadRepositoryFile("src/wallet/digidollarwallet.cpp");
    const std::string message = "DD owner key for timelock %s is already saved";

    size_t at = source.find(message);
    BOOST_REQUIRE_MESSAGE(at != std::string::npos, "the message itself has gone: " << message);
    while (at != std::string::npos) {
        const size_t line_start = source.rfind('\n', at);
        const std::string line = source.substr(line_start + 1, at - line_start - 1);
        BOOST_CHECK_MESSAGE(line.find("LogPrint(BCLog::DIGIDOLLAR,") != std::string::npos,
                            "this message is the ordinary case, so it must go through the DigiDollar "
                            "log category instead of printing on every node. Line reads: " << line);
        at = source.find(message, at + 1);
    }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
