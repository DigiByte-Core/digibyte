// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Giving up on a mint that can no longer be mined releases what it reserved:
// the transaction is abandoned so its DGB inputs can be spent again, the coin
// locks on the collateral and on the token are removed, the token stops
// counting as DigiDollar, and the position is marked inactive.
//
// Two things must not happen while that runs.
//
// It must not remove those protections when it could not abandon the
// transaction, and it must not remove them when the mint has become live in
// the meantime. No wallet lock is held while the transaction is abandoned, so
// in that gap the mint can reach a block or this node's mempool, and then its
// collateral is committed and its token is real.
//
// It must also leave the wallet as it was when the wallet file refuses the
// change, because otherwise the wallet shows one thing and its file says
// another until the next restart.

#include <boost/test/unit_test.hpp>

#include <consensus/amount.h>
#include <key.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <support/allocators/zeroafterfree.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wallet {
namespace {

//! A transaction with the shape of a DigiDollar mint: a collateral output the
//! wallet can recognise and a DigiDollar token output paying the tweaked owner
//! key.
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

//! True when this is the wallet's own row for that transaction. The key is the
//! text "tx" followed by the 32 bytes of the transaction id.
bool IsWalletTransactionRow(Span<const std::byte> key, const uint256& txid)
{
    static const std::string name{"tx"};
    if (key.size() != 1 + name.size() + 32) return false;
    const unsigned char* bytes{reinterpret_cast<const unsigned char*>(key.data())};
    if (bytes[0] != name.size()) return false;
    if (std::memcmp(bytes + 1, name.data(), name.size()) != 0) return false;
    return std::memcmp(bytes + 1 + name.size(), txid.begin(), 32) == 0;
}

//! True when the wallet file holds a coin lock for this outpoint. Opening a
//! wallet puts back the coin locks of every active position, so a lock row that
//! outlives the position explaining it would keep that coin out of reach for
//! ever. The file is what this reads, not the list held in memory.
bool FileHoldsCoinLock(CWallet& wallet, const COutPoint& outpoint)
{
    DataStream row_key{};
    row_key << std::make_pair(DBKeys::LOCKED_UTXO, std::make_pair(outpoint.hash, outpoint.n));
    const SerializeData wanted{row_key.begin(), row_key.end()};
    return GetMockableDatabase(wallet).m_records.count(wanted) > 0;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_mint_cleanup_tests, WalletTestingSetup)

//! Put an unconfirmed mint in the wallet, with its collateral and token
//! outputs locked, and tell the wallet which height it is at. Returns the
//! position id, which is the mint transaction id.
static uint256 AddUnconfirmedMint(CWallet& wallet, DigiDollarWallet& dd_wallet, int last_block_height)
{
    CKey owner_key;
    owner_key.MakeNewKey(true);
    const CTransactionRef mint_tx = MakeMintShapedTx(owner_key);
    const uint256 position_id = mint_tx->GetHash();
    // Tier 0 locks collateral for 240 blocks and this position unlocks at
    // height 1000, so a block may include the mint up to height 760.
    WalletCollateralPosition pos(position_id, 10000, 5 * COIN, 0, 1000);
    pos.owner_keyid = owner_key.GetPubKey().GetID();
    std::string error;
    BOOST_REQUIRE(dd_wallet.RecordPendingMint(*mint_tx, pos, owner_key, error));

    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(wallet.AddToWallet(mint_tx, TxStateInactive{}));
        BOOST_REQUIRE(wallet.LockCoin(COutPoint(position_id, 0)));
        BOOST_REQUIRE(wallet.LockCoin(COutPoint(position_id, 1)));
        wallet.SetLastBlockProcessed(last_block_height, block_hash);
    }
    return position_id;
}

//! Save a mint's records the way a mint does before it hands the transaction to
//! the wallet, and stop there. This is the state a mint is in when it gives up
//! between saving its records and sending anything: the records exist, the
//! transaction does not.
static uint256 AddSavedButUnsentMint(CWallet& wallet, DigiDollarWallet& dd_wallet)
{
    CKey owner_key;
    owner_key.MakeNewKey(true);
    const CTransactionRef mint_tx = MakeMintShapedTx(owner_key);
    const uint256 position_id = mint_tx->GetHash();
    WalletCollateralPosition pos(position_id, 10000, 5 * COIN, 0, 1000);
    pos.owner_keyid = owner_key.GetPubKey().GetID();
    std::string error;
    BOOST_REQUIRE(dd_wallet.RecordPendingMint(*mint_tx, pos, owner_key, error));

    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(500, block_hash);
    }
    return position_id;
}

BOOST_AUTO_TEST_CASE(a_mint_that_goes_live_while_it_is_released_keeps_everything)
{
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/500)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Local);

    // The mint reaches a block while it is being released. The wallet writes
    // the transaction's own row while it abandons it, so confirming the mint
    // from inside that write puts it live at exactly the moment the release
    // has decided to let go and is about to remove its protections.
    uint256 block_hash;
    GetRandBytes(block_hash);
    bool went_live{false};
    GetMockableDatabase(m_wallet).m_on_write = [&](Span<const std::byte> key) {
        if (went_live || !IsWalletTransactionRow(key, position_id)) return;
        went_live = true;
        LOCK(m_wallet.cs_wallet);
        auto entry = m_wallet.mapWallet.find(position_id);
        if (entry == m_wallet.mapWallet.end()) return;
        entry->second.m_state = TxStateConfirmed{block_hash, /*height=*/500, /*index=*/0};
    };

    std::string error;
    const bool released{dd_wallet.ReleaseMintAttempt(position_id, error)};
    GetMockableDatabase(m_wallet).m_on_write = nullptr;

    BOOST_REQUIRE_MESSAGE(went_live, "the mint never went live, so this test proved nothing");
    BOOST_CHECK(!released);
    BOOST_CHECK(!error.empty());

    // The mint is in a block, so its collateral is committed and its token is
    // real. Everything that protects them stays.
    BOOST_CHECK(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Confirmed);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 5 * COIN);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(m_wallet.IsLockedCoin(token));
    }

    // The token output is still in the wallet file, so the release wrote
    // nothing. The file is what this checks, not the list held in memory:
    // abandoning a transaction rebuilds that list by reading the mint payload
    // of every live wallet transaction, and the transaction here is only mint
    // shaped and carries no payload, so it cannot come back into the list in a
    // test. A reload reads the file.
    DigiDollarWallet reloaded(&m_wallet);
    BOOST_CHECK(reloaded.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(reloaded.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
}

BOOST_AUTO_TEST_CASE(a_release_the_wallet_file_refuses_changes_nothing)
{
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/500)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};
    COutPoint mint_input;
    {
        LOCK(m_wallet.cs_wallet);
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        mint_input = wtx->tx->vin[0].prevout;
        BOOST_REQUIRE(m_wallet.IsSpent(mint_input));
    }

    // A wallet file that cannot be written to: a full disk, or a file that has
    // gone read only.
    GetMockableDatabase(m_wallet).m_pass = false;
    std::string error;
    const bool released{dd_wallet.ReleaseMintAttempt(position_id, error)};
    GetMockableDatabase(m_wallet).m_pass = true;

    BOOST_CHECK(!released);
    BOOST_CHECK(!error.empty());

    // Nothing was released, so what the wallet shows still matches what its
    // file says, and the release can be tried again later. The token output is
    // checked in the file below rather than in the list held in memory, because
    // abandoning the transaction rebuilds that list from the mint payload of
    // every live wallet transaction and the transaction here carries none.
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 5 * COIN);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(m_wallet.IsLockedCoin(token));
        // The release starts by abandoning the mint transaction, and that has to
        // reach the wallet file as well. Nothing was saved, so the mint is still
        // live here and the coin it spends is still committed to it. A wallet
        // that thinks that coin is free will spend it again.
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(!wtx->isAbandoned());
        BOOST_CHECK(m_wallet.IsSpent(mint_input));
    }

    // Reading the same wallet file again shows the same thing.
    DigiDollarWallet reloaded(&m_wallet);
    BOOST_CHECK_EQUAL(reloaded.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
    BOOST_CHECK(reloaded.HasDDUTXO(token));
}

BOOST_AUTO_TEST_CASE(a_mint_that_can_no_longer_be_mined_is_released_in_full)
{
    // The ordinary case, to show the two checks above did not stop the work
    // that is supposed to happen.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/500)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};

    std::string error;
    BOOST_CHECK(dd_wallet.ReleaseMintAttempt(position_id, error));

    BOOST_CHECK(!dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 0U);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), CAmount{0});
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(!m_wallet.IsLockedCoin(token));
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(wtx->isAbandoned());
    }
    // The record of the attempt and its owner key stay, so a late
    // confirmation or a reorg can bring the vault back.
    const std::vector<WalletCollateralPosition> all{dd_wallet.GetDDTimeLocks(/*active_only=*/false)};
    BOOST_REQUIRE_EQUAL(all.size(), 1U);
    BOOST_CHECK(!all[0].is_active);
    CKey owner_key;
    BOOST_CHECK(dd_wallet.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(a_mint_that_was_never_sent_can_be_cleaned_up_again_after_a_refused_write)
{
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddSavedButUnsentMint(m_wallet, dd_wallet)};
    const COutPoint token{position_id, 1};
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::NotInWallet);
    BOOST_REQUIRE(dd_wallet.HasDDUTXO(token));
    BOOST_REQUIRE_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/false).size(), 1U);

    // The wallet file refuses the change, so the cleanup has to leave every
    // record where it is and say that it failed.
    GetMockableDatabase(m_wallet).m_pass = false;
    std::string error;
    const bool released{dd_wallet.ReleaseMintAttempt(position_id, error)};
    GetMockableDatabase(m_wallet).m_pass = true;
    BOOST_CHECK(!released);
    BOOST_CHECK(!error.empty());

    // Every record is still here, so this wallet still matches its file.
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/false).size(), 1U);
    BOOST_CHECK(dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 5 * COIN);
    {
        // And the file still holds them, so a restart shows the same thing.
        DigiDollarWallet reloaded(&m_wallet);
        BOOST_CHECK_EQUAL(reloaded.GetDDTimeLocks(/*active_only=*/false).size(), 1U);
        BOOST_CHECK(reloaded.HasDDUTXO(token));
    }

    // The same cleanup, tried again once the file works, finishes the job.
    error.clear();
    BOOST_CHECK_MESSAGE(dd_wallet.ReleaseMintAttempt(position_id, error), error);
    BOOST_CHECK(dd_wallet.GetDDTimeLocks(/*active_only=*/false).empty());
    BOOST_CHECK(!dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), CAmount{0});
    // The owner key stays. It is how this wallet reaches the collateral of a
    // vault, and a signed copy of the mint could still reach a block.
    CKey kept_owner_key;
    BOOST_CHECK(dd_wallet.GetOwnerKey(position_id, kept_owner_key));

    // Nothing is left in the file either.
    DigiDollarWallet reloaded(&m_wallet);
    BOOST_CHECK(reloaded.GetDDTimeLocks(/*active_only=*/false).empty());
    BOOST_CHECK(!reloaded.HasDDUTXO(token));
    CKey owner_key_from_file;
    BOOST_CHECK(reloaded.GetOwnerKey(position_id, owner_key_from_file));
}

BOOST_AUTO_TEST_CASE(a_cleanup_that_cannot_group_its_changes_changes_nothing)
{
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddSavedButUnsentMint(m_wallet, dd_wallet)};
    const COutPoint token{position_id, 1};
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::NotInWallet);

    // A wallet file that takes single writes but will not group them. The
    // cleanup has several rows to remove and must not leave some of them
    // behind, so it does nothing at all and says so. Carrying on here was how
    // the file ended up holding part of the change with nothing to say which
    // part, while this wallet still showed all of it.
    GetMockableDatabase(m_wallet).m_txn_pass = false;
    std::string error;
    const bool released{dd_wallet.ReleaseMintAttempt(position_id, error)};
    GetMockableDatabase(m_wallet).m_txn_pass = true;
    BOOST_CHECK(!released);
    BOOST_CHECK(!error.empty());

    // Every record is still here, so this wallet still matches its file.
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/false).size(), 1U);
    BOOST_CHECK(dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 5 * COIN);
    {
        DigiDollarWallet reloaded(&m_wallet);
        BOOST_CHECK_EQUAL(reloaded.GetDDTimeLocks(/*active_only=*/false).size(), 1U);
        BOOST_CHECK(reloaded.HasDDUTXO(token));
    }

    // The same cleanup finishes the job once the file can group the changes.
    // Reading the file above put the coin locks back, so this also removes
    // those.
    error.clear();
    BOOST_CHECK_MESSAGE(dd_wallet.ReleaseMintAttempt(position_id, error), error);
    BOOST_CHECK(dd_wallet.GetDDTimeLocks(/*active_only=*/false).empty());
    BOOST_CHECK(!dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), CAmount{0});
    BOOST_CHECK(!FileHoldsCoinLock(m_wallet, COutPoint(position_id, 0)));
    BOOST_CHECK(!FileHoldsCoinLock(m_wallet, token));
    CKey owner_key;
    BOOST_CHECK(dd_wallet.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(records_of_a_mint_this_run_is_building_are_never_swept)
{
    // A mint saves its records before it hands the transaction to the wallet,
    // so while it is being built it looks exactly like a mint that was never
    // sent: a position record and no transaction. The sweep that runs on every
    // block must leave that alone. The mint can still be sent and mined, and
    // its collateral is committed to it; releasing it here would hand those
    // coins back while a block can still include the mint.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddSavedButUnsentMint(m_wallet, dd_wallet)};
    const COutPoint token{position_id, 1};

    // Tier 0 locks 240 blocks and this position unlocks at height 1000, so at
    // height 760 not even the lock window is left. The sweep still leaves the
    // records alone, because this run is the one that wrote them.
    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(760, block_hash);
    }

    BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 0U);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/false).size(), 1U);
    BOOST_CHECK(dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 5 * COIN);
}

BOOST_AUTO_TEST_CASE(records_left_by_an_earlier_run_are_swept_when_the_wallet_is_opened_again)
{
    // A mint saved its records and the node stopped before the transaction was
    // sent. Nothing in that run can clean up after it, so the records sit in
    // the wallet file: a token this wallet counts but cannot spend, and
    // collateral held for a mint that does not exist. The next time the wallet
    // is opened the sweep finishes the job. This is also the retry for a
    // cleanup the file refused.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddSavedButUnsentMint(m_wallet, dd_wallet)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};

    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(760, block_hash);
    }

    // Opening the wallet again reads the records from the file and puts the
    // coin locks back, which is how locks come to exist for a position that is
    // about to be removed.
    DigiDollarWallet reopened(&m_wallet);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.IsLockedCoin(collateral));
        BOOST_REQUIRE(m_wallet.IsLockedCoin(token));
    }
    BOOST_REQUIRE(FileHoldsCoinLock(m_wallet, collateral));
    BOOST_REQUIRE(FileHoldsCoinLock(m_wallet, token));
    BOOST_REQUIRE(reopened.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::NotInWallet);

    BOOST_CHECK_EQUAL(reopened.ReconcileExpiredMintAttempts(), 1U);

    // Nothing of the mint is left, and no coin lock outlived the position that
    // explained it.
    BOOST_CHECK(reopened.GetDDTimeLocks(/*active_only=*/false).empty());
    BOOST_CHECK(!reopened.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(reopened.GetLockedCollateral(), CAmount{0});
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(!m_wallet.IsLockedCoin(token));
    }
    BOOST_CHECK(!FileHoldsCoinLock(m_wallet, collateral));
    BOOST_CHECK(!FileHoldsCoinLock(m_wallet, token));

    // The file says the same, and there is nothing left for a later sweep.
    DigiDollarWallet reloaded(&m_wallet);
    BOOST_CHECK(reloaded.GetDDTimeLocks(/*active_only=*/false).empty());
    BOOST_CHECK(!reloaded.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(reloaded.ReconcileExpiredMintAttempts(), 0U);
    // The owner key stays. A signed copy of the mint could still reach a block.
    CKey owner_key;
    BOOST_CHECK(reloaded.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(startup_marking_a_position_inactive_does_not_hide_it_from_the_sweep)
{
    // The case the earlier test missed, and the reason the sweep did nothing on a
    // real node. Startup asks the chain about the collateral of every position. A
    // mint that was never sent has no such coin, so ValidatePositionStates marks
    // the position inactive and saves that. The sweep then skipped every inactive
    // position, so from that moment on the records it exists to remove were
    // invisible to it, at that start and at every start afterwards.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddSavedButUnsentMint(m_wallet, dd_wallet)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};

    // Open the wallet once with the position still active. That is what puts the
    // coin locks on its two outputs and writes them to the file, so the locks
    // exist for the same reason they do on a real node.
    {
        DigiDollarWallet first_open(&m_wallet);
        BOOST_REQUIRE_EQUAL(first_open.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
    }
    BOOST_REQUIRE(FileHoldsCoinLock(m_wallet, collateral));
    BOOST_REQUIRE(FileHoldsCoinLock(m_wallet, token));

    // Write the position back inactive, which is exactly what startup does when
    // it cannot find the collateral in the coin set.
    {
        const std::vector<WalletCollateralPosition> all{dd_wallet.GetDDTimeLocks(/*active_only=*/false)};
        BOOST_REQUIRE_EQUAL(all.size(), 1U);
        WalletCollateralPosition inactive = all[0];
        inactive.is_active = false;
        WalletBatch batch(m_wallet.GetDatabase());
        BOOST_REQUIRE(batch.WriteDDTimeLock(inactive));
    }

    // The lock window must have passed or the sweep is right to leave it alone.
    // Tier 0 locks 240 blocks (LockDaysToBlocks(0)) and this position unlocks at
    // height 1000, so the block after 760 leaves less than the tier's full length.
    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(760, block_hash);
    }

    // Open it again. This is the state the sweep really sees at startup.
    DigiDollarWallet reopened(&m_wallet);
    const std::vector<WalletCollateralPosition> loaded{reopened.GetDDTimeLocks(/*active_only=*/false)};
    BOOST_REQUIRE_EQUAL(loaded.size(), 1U);
    BOOST_REQUIRE_MESSAGE(!loaded[0].is_active,
                          "this case only means anything while the position is inactive, which is what startup does");
    BOOST_REQUIRE(reopened.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::NotInWallet);

    // Before this was fixed the answer here was zero and the records stayed for ever.
    BOOST_CHECK_EQUAL(reopened.ReconcileExpiredMintAttempts(), 1U);

    BOOST_CHECK(reopened.GetDDTimeLocks(/*active_only=*/false).empty());
    BOOST_CHECK(!reopened.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(reopened.GetLockedCollateral(), CAmount{0});
    BOOST_CHECK_MESSAGE(!FileHoldsCoinLock(m_wallet, collateral), "a coin lock outlived the position that explained it");
    BOOST_CHECK_MESSAGE(!FileHoldsCoinLock(m_wallet, token), "a coin lock outlived the position that explained it");

    // The owner key stays. A signed copy of the mint could still reach a block.
    CKey owner_key;
    BOOST_CHECK(reopened.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(a_disconnect_between_the_choice_and_the_release_stops_the_release)
{
    // The sweep chooses which mints to give up and then releases them, and the
    // locks are let go in between. A block can disconnect in that gap. That
    // puts back the height the lock window is measured against, so a mint that
    // was finished when it was chosen can be mined after all when the release
    // runs. Releasing it then would abandon a transaction a block can still
    // include and hand back the coins it spends.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    // Tier 0 locks 240 blocks and this position unlocks at height 1000, so at
    // height 760 no block can include the mint any more and the sweep picks it.
    const uint256 position_id{AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/760)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Expired);
    BOOST_REQUIRE(dd_wallet.MintAttemptCanBeSwept(position_id, DigiDollarWallet::MintAttemptState::Expired));

    COutPoint mint_input;
    {
        LOCK(m_wallet.cs_wallet);
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        mint_input = wtx->tx->vin[0].prevout;
        BOOST_REQUIRE(m_wallet.IsSpent(mint_input));
    }

    // The disconnect. One block is enough: at height 759 the whole lock window
    // is left again, which is all a single reorged block has to do.
    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(759, block_hash);
    }
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Local);

    // The release the sweep decided on, run now.
    std::string error;
    const bool released{dd_wallet.ReleaseMintAttempt(
        position_id, error, DigiDollarWallet::MintReleaseCaller::AutomaticSweep)};
    BOOST_CHECK(!released);
    BOOST_CHECK(!error.empty());

    // Nothing was given up. The mint is still live, so the coin it spends is
    // still committed to it, and everything protecting the vault stays.
    {
        LOCK(m_wallet.cs_wallet);
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(!wtx->isAbandoned());
        BOOST_CHECK(m_wallet.IsSpent(mint_input));
        BOOST_CHECK(m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(m_wallet.IsLockedCoin(token));
    }
    BOOST_CHECK(dd_wallet.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 5 * COIN);

    // Waiting is all that happened. Once the block is back the sweep gives the
    // mint up, so refusing costs a retry and nothing else.
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(760, block_hash);
    }
    BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 1U);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 0U);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), CAmount{0});
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(!m_wallet.IsLockedCoin(token));
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(wtx->isAbandoned());
    }
    CKey owner_key;
    BOOST_CHECK(dd_wallet.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(a_disconnect_while_the_mint_is_abandoned_keeps_what_protects_the_vault)
{
    // The release abandons the mint transaction with the DigiDollar lock let
    // go, so the worst case is a block disconnecting during the abandon
    // itself. The main wallet lock is held across it on every path that sweeps,
    // and a block cannot connect or disconnect without that lock, so this
    // cannot happen on a running node. It is forced here because what the
    // release does next is what protects the vault: the transaction is
    // abandoned and its coins are free again, and from that point on the
    // release must keep the coin locks, the token and the active position, and
    // say that it released nothing more.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/760)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Expired);

    // The wallet writes the transaction's own row while it abandons it, so the
    // disconnect is delivered from inside that write.
    uint256 block_hash;
    GetRandBytes(block_hash);
    bool disconnected{false};
    GetMockableDatabase(m_wallet).m_on_write = [&](Span<const std::byte> key) {
        if (disconnected || !IsWalletTransactionRow(key, position_id)) return;
        disconnected = true;
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(759, block_hash);
    };

    std::string error;
    const bool released{dd_wallet.ReleaseMintAttempt(
        position_id, error, DigiDollarWallet::MintReleaseCaller::AutomaticSweep)};
    GetMockableDatabase(m_wallet).m_on_write = nullptr;

    BOOST_REQUIRE_MESSAGE(disconnected, "no block disconnected, so this test proved nothing");
    BOOST_CHECK(!released);
    BOOST_CHECK(!error.empty());

    // The mint is no longer the finished thing the sweep chose, so the release
    // stopped.
    BOOST_CHECK(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Abandoned);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 5 * COIN);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(m_wallet.IsLockedCoin(token));
    }
    // There is nothing to check in the file here, and asserting there was is why
    // this case failed when it was written. The helper that sets this mint up
    // locks the two coins with no batch, and LockCoin writes a row only when it
    // is given one, so these locks are in memory only and the file never held
    // them. The other cases in this file do check the file, because they reopen
    // the wallet first and reopening is what writes the lock rows. What proves
    // the vault survived here is the reload below.

    // The token output is checked in the file, not in the list held in memory.
    // Abandoning the transaction rebuilds that list from the mint payload of
    // every live wallet transaction, and the transaction here is only mint
    // shaped and carries no payload, so it cannot come back into the list in a
    // test. The file is what a restart reads, and it still holds the vault.
    DigiDollarWallet reloaded(&m_wallet);
    BOOST_CHECK(reloaded.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(reloaded.GetDDTimeLocks(/*active_only=*/true).size(), 1U);
    CKey owner_key;
    BOOST_CHECK(reloaded.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(a_mint_giving_up_its_own_records_is_never_held_back)
{
    // The brakes belong on the sweep, which is guessing that a mint is
    // finished. A mint that has stopped knows it, and it is the only thing
    // that could still send the transaction, so its own cleanup goes through
    // at once even while a block could still include the mint.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/500)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Local);

    // The sweep leaves it alone: at this height a block can still include it.
    std::string error;
    BOOST_CHECK(!dd_wallet.ReleaseMintAttempt(
        position_id, error, DigiDollarWallet::MintReleaseCaller::AutomaticSweep));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 0U);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 1U);

    // The mint's own cleanup, at the same height, releases it in full.
    error.clear();
    BOOST_CHECK_MESSAGE(dd_wallet.ReleaseMintAttempt(position_id, error), error);
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 0U);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), CAmount{0});
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(collateral));
        BOOST_CHECK(!m_wallet.IsLockedCoin(token));
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(wtx->isAbandoned());
    }
    CKey owner_key;
    BOOST_CHECK(dd_wallet.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(records_of_a_mint_that_could_be_sent_again_are_not_swept)
{
    // The records of a mint that was never sent are only given up once no block
    // could include the mint any more. A disconnect between the choice and the
    // release puts that height back, and then the records have to stay: a
    // signed copy of the mint could still reach a block, and these records are
    // how this wallet would find the vault.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id{AddSavedButUnsentMint(m_wallet, dd_wallet)};
    const COutPoint collateral{position_id, 0};
    const COutPoint token{position_id, 1};

    uint256 block_hash;
    GetRandBytes(block_hash);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(760, block_hash);
    }

    // Open the wallet again, so these are records an earlier run left behind
    // and the sweep may touch them at all. Opening also puts their coin locks
    // back, the same as on a real node.
    DigiDollarWallet reopened(&m_wallet);
    BOOST_REQUIRE(reopened.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::NotInWallet);
    BOOST_REQUIRE(reopened.UnsentMintRecordsCanBeReleased(position_id));
    BOOST_REQUIRE(FileHoldsCoinLock(m_wallet, collateral));

    // The disconnect.
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(759, block_hash);
    }
    BOOST_REQUIRE(!reopened.UnsentMintRecordsCanBeReleased(position_id));

    std::string error;
    BOOST_CHECK(!reopened.ReleaseMintAttempt(
        position_id, error, DigiDollarWallet::MintReleaseCaller::AutomaticSweep));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(reopened.ReconcileExpiredMintAttempts(), 0U);

    // Every record is still here, and the file says the same.
    BOOST_CHECK_EQUAL(reopened.GetDDTimeLocks(/*active_only=*/false).size(), 1U);
    BOOST_CHECK(reopened.HasDDUTXO(token));
    BOOST_CHECK_EQUAL(reopened.GetLockedCollateral(), 5 * COIN);
    BOOST_CHECK(FileHoldsCoinLock(m_wallet, collateral));
    BOOST_CHECK(FileHoldsCoinLock(m_wallet, token));
    {
        DigiDollarWallet reloaded(&m_wallet);
        BOOST_CHECK_EQUAL(reloaded.GetDDTimeLocks(/*active_only=*/false).size(), 1U);
        BOOST_CHECK(reloaded.HasDDUTXO(token));
    }

    // The mint's own cleanup is not held back here either.
    error.clear();
    BOOST_CHECK_MESSAGE(reopened.ReleaseMintAttempt(position_id, error), error);
    BOOST_CHECK(reopened.GetDDTimeLocks(/*active_only=*/false).empty());
    BOOST_CHECK(!reopened.HasDDUTXO(token));
    BOOST_CHECK(!FileHoldsCoinLock(m_wallet, collateral));
    BOOST_CHECK(!FileHoldsCoinLock(m_wallet, token));
    // The owner key stays. A signed copy of the mint could still reach a block.
    CKey owner_key;
    BOOST_CHECK(reopened.GetOwnerKey(position_id, owner_key));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
