// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The DigiDollar wallet asks the chain which collateral coins still exist.
// Asking the chain takes the chain's own lock, and the chain takes that same
// lock before it calls back into the wallet. A thread that holds the wallet
// lock and then waits for the chain, running against a thread going the other
// way, leaves the node stuck with no way out.
//
// Three places in the wallet run with the wallet lock already held and then
// touch DigiDollar state: abandoning a transaction, disconnecting a block
// during a reorg, and giving up a mint that can no longer be mined when a
// block is connected. None of them may reach the chain question.
//
// Each test here runs one of those routes with the wallet lock held. On a
// build with lock checking turned on (configure --enable-debug) the wallet
// stops the process if the chain question is reached with the lock held, so if
// anyone puts it back these tests fail loudly instead of leaving a node that
// hangs once in a while in production. The tests also check the replacement
// behaviour: the chain question is recorded as still owed, and it is answered
// later, when nothing is locked.

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

namespace wallet {
namespace {

//! Build a transaction with the shape of a DigiDollar mint: a collateral
//! output the wallet can recognise and a DigiDollar token output paying the
//! tweaked owner key.
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

BOOST_FIXTURE_TEST_SUITE(digidollar_wallet_lock_safety_tests, WalletTestingSetup)

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
        BOOST_CHECK(wallet.LockCoin(COutPoint(position_id, 0)));
        BOOST_CHECK(wallet.LockCoin(COutPoint(position_id, 1)));
        wallet.SetLastBlockProcessed(last_block_height, block_hash);
    }
    return position_id;
}

BOOST_AUTO_TEST_CASE(rebuilding_dd_utxos_under_the_wallet_lock_asks_the_chain_nothing)
{
    // This is the shape of a reorg: the wallet lock is held for the whole of
    // disconnecting a block, and the DigiDollar UTXO map is rebuilt inside it.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/500);

    {
        LOCK(m_wallet.cs_wallet);
        dd_wallet.RebuildDDUTXOs();
    }

    // The rebuild is wallet-local work only. The chain check that belongs with
    // it is left owed for a moment when no wallet lock is held.
    BOOST_CHECK(dd_wallet.HasPendingPositionStateValidation());
}

BOOST_AUTO_TEST_CASE(abandoning_a_transaction_under_the_wallet_lock_asks_the_chain_nothing)
{
    // CWallet::AbandonTransaction holds the wallet lock for its whole body and
    // rebuilds DigiDollar state inside it. The abandon RPC, a transaction
    // dropped from the mempool and the expired-mint cleanup all come through
    // here.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    const uint256 position_id = AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/500);

    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.TransactionCanBeAbandoned(position_id));
        BOOST_CHECK(m_wallet.AbandonTransaction(position_id));
    }

    {
        LOCK(m_wallet.cs_wallet);
        const CWalletTx* wtx = m_wallet.GetWalletTx(position_id);
        BOOST_REQUIRE(wtx != nullptr);
        BOOST_CHECK(wtx->isAbandoned());
    }
    BOOST_CHECK(dd_wallet.HasPendingPositionStateValidation());
}

BOOST_AUTO_TEST_CASE(releasing_an_expired_mint_under_the_wallet_lock_asks_the_chain_nothing)
{
    // This is the route that is new in this release. Every connected block
    // runs it for a wallet that holds a mint which can no longer be mined, and
    // it runs with the wallet lock held for the whole of connecting the block.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    // Height 760 is the last height at which a block could still include this
    // mint, so at 760 the wallet gives up on it.
    const uint256 position_id = AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/760);
    BOOST_REQUIRE(dd_wallet.GetMintAttemptState(position_id) == DigiDollarWallet::MintAttemptState::Expired);

    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 1u);
    }

    // The reservations the mint held are gone, so the release itself still did
    // its job while the lock was held.
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(COutPoint(position_id, 0)));
        BOOST_CHECK(!m_wallet.IsLockedCoin(COutPoint(position_id, 1)));
    }
    BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/true).size(), 0u);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), 0);
    BOOST_CHECK(dd_wallet.HasPendingPositionStateValidation());

    // Nothing is left to release on a second pass, under the lock or not.
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK_EQUAL(dd_wallet.ReconcileExpiredMintAttempts(), 0u);
    }
}

BOOST_AUTO_TEST_CASE(the_owed_chain_check_is_answered_when_no_wallet_lock_is_held)
{
    // What was put off must actually happen. The wallet runs the owed check
    // when the chain tip moves, which is a moment with no wallet lock held.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet& dd_wallet = *m_wallet.GetDDWallet();
    AddUnconfirmedMint(m_wallet, dd_wallet, /*last_block_height=*/500);

    {
        LOCK(m_wallet.cs_wallet);
        dd_wallet.RebuildDDUTXOs();
    }
    BOOST_REQUIRE(dd_wallet.HasPendingPositionStateValidation());

    // With no lock held the check may ask the chain. This test setup has only
    // the genesis block, so the node is still catching up and the check asks
    // to be tried again later rather than judging positions on an unfinished
    // chain. Either way it must run without stopping the process, which is
    // what a wallet lock held here would cause.
    dd_wallet.RetryPendingPositionStateValidation();
    BOOST_CHECK(dd_wallet.HasPendingPositionStateValidation());

    // The full scan is the same: it is only ever called with no wallet lock
    // held, and it takes the chain check with it.
    BOOST_CHECK_NO_THROW(dd_wallet.ScanForDDUTXOs());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
