// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// A mint saves its owner key and its position record before it hands the
// transaction to the wallet. It has to: once the transaction is out it can be
// mined whether or not this process survives, and those records are the only
// things that let the owner redeem the vault later.
//
// Between that save and the commit the mint can still stop. A block can
// arrive, or the price the oracle would sign for the next block can change,
// and then the collateral and burn amounts the mint was built for no longer
// match the block it would go into. The mint gives up.
//
// Whatever was saved must not be left behind when that happens. The wallet
// counts a leftover token record towards its own DigiDollar balance and offers
// it for spending, so the owner is shown DigiDollars that cannot be spent,
// while no transaction exists anywhere.
//
// These tests run the real mint command and stop it at exactly that point.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/amount.h>
#include <digidollar/digidollar.h>
#include <interfaces/chain.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <primitives/transaction.h>
#include <rpc/digidollar.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <span.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/source_root.h>
#include <txmempool.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr CAmount kFirstPrice{500000};   // $0.50 per DGB
constexpr CAmount kLaterPrice{510000};   // $0.51 per DGB
constexpr CAmount kMintCents{10000};     // $100.00

//! The wallet keeps a position under a key made of the text "ddposition"
//! followed by the 32 bytes of the position id. Read the id back out of one.
//! Returns nothing when the key belongs to some other kind of record.
std::optional<uint256> PositionIdFromKey(Span<const std::byte> key)
{
    static const std::string name{"ddposition"};
    if (key.size() < name.size() + 32) return std::nullopt;
    const std::string_view text{reinterpret_cast<const char*>(key.data()), key.size()};
    const size_t at{text.find(name)};
    if (at == std::string_view::npos) return std::nullopt;
    if (at + name.size() + 32 > key.size()) return std::nullopt;
    uint256 id;
    std::memcpy(id.begin(), reinterpret_cast<const unsigned char*>(key.data()) + at + name.size(), 32);
    return id;
}

//! The part of a source file that holds one function, so a check reads only
//! that function and not the whole file.
std::string FunctionBody(const std::string& source, const std::string& starts_at, const std::string& ends_at)
{
    const size_t start{source.find(starts_at)};
    BOOST_REQUIRE_MESSAGE(start != std::string::npos, "cannot find " + starts_at);
    const size_t end{source.find(ends_at, start + starts_at.size())};
    BOOST_REQUIRE_MESSAGE(end != std::string::npos, "cannot find " + ends_at + " after " + starts_at);
    return source.substr(start, end - start);
}

//! Where a piece of text is in a function, with a clear failure when it is
//! missing at all.
size_t Where(const std::string& body, const std::string& text, const std::string& what)
{
    const size_t at{body.find(text)};
    BOOST_REQUIRE_MESSAGE(at != std::string::npos, what + " (looked for \"" + text + "\")");
    return at;
}

//! A funded wallet on a regtest chain where DigiDollar and the Thaw Day rules
//! are both active, with a signed oracle quote for the next block. This is the
//! smallest setting in which the mint command runs its candidate checks.
struct CancelledMintSetup : TestChain100Setup {
    wallet::WalletContext context;
    std::shared_ptr<wallet::CWallet> wallet;
    //! The position the wallet saved, seen as the record was being written.
    std::optional<uint256> saved_position_id;

    CancelledMintSetup()
        : TestChain100Setup(ChainType::REGTEST, {"-digidollaractivationheight=100", "-ddthawdayheight=120"})
    {
        MockOracleManager::GetInstance().Reset();
        OracleBundleManager::GetInstance().Clear();
        mineBlocks(20);

        MockOracleManager& mock{MockOracleManager::GetInstance()};
        mock.SetEnabled(true);
        mock.SetMockPrice(kFirstPrice);
        OracleBundleManager::GetInstance().SetEnabled(true);
        BOOST_REQUIRE(OracleBundleManager::GetInstance().UpdateBundle(mock.CreateMockMuSig2Bundle(MintHeight())));

        wallet = wallet::CreateSyncedWallet(
            *m_node.chain,
            WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()),
            coinbaseKey);
        wallet->SetBroadcastTransactions(true);
        wallet->EnsureDDWallet();
        BOOST_REQUIRE(wallet->GetDDWallet() != nullptr);
        context.args = m_node.args;
        context.chain = m_node.chain.get();
        wallet::AddWallet(context, wallet);
    }

    ~CancelledMintSetup()
    {
        wallet::GetMockableDatabase(*wallet).m_on_write = nullptr;
        wallet::RemoveWallet(context, wallet, std::nullopt);
        MockOracleManager::GetInstance().Reset();
        OracleBundleManager::GetInstance().Clear();
    }

    int MintHeight() const
    {
        return WITH_LOCK(cs_main, return Assert(m_node.chainman)->ActiveChain().Tip()->nHeight + 1);
    }

    //! Do one thing at the moment the mint's position record reaches the
    //! wallet file. That is inside the mint, after its records are saved and
    //! before its transaction is committed, which is the moment these tests
    //! are about. The position id is remembered either way, so a test can
    //! tell a mint that stopped in that window from one that never got there.
    void WhenTheMintIsSaved(std::function<void()> what_happens)
    {
        wallet::GetMockableDatabase(*wallet).m_on_write =
            [this, what_happens](Span<const std::byte> key) {
                if (saved_position_id.has_value()) return;
                const std::optional<uint256> id{PositionIdFromKey(key)};
                if (!id.has_value()) return;
                saved_position_id = id;
                if (what_happens) what_happens();
            };
    }

    //! Connect one more block, from another thread. The mint holds the wallet
    //! lock while this runs and connecting a block takes the chain lock, so
    //! doing it on this thread would take those two locks in the order that
    //! can leave a node stuck with no way out. This thread only waits.
    void MineOneBlockOnAnotherThread()
    {
        std::thread miner{[this] { mineBlocks(1); }};
        miner.join();
    }

    //! Run the mint command the way a user would. Returns the error it gave,
    //! or an empty string when the mint succeeded.
    std::string CallMint()
    {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = "mintdigidollar";
        request.params = UniValue(UniValue::VARR);
        request.params.push_back(int64_t{kMintCents});
        request.params.push_back(0);
        try {
            mintdigidollar().HandleRequest(request);
            return {};
        } catch (const UniValue& failure) {
            return failure.find_value("message").get_str();
        } catch (const std::exception& failure) {
            return failure.what();
        }
    }

    //! How many mint transactions this wallet holds that are not abandoned.
    //! These are the ones its transaction list would show.
    size_t LiveMintTransactions() const
    {
        LOCK(wallet->cs_wallet);
        size_t mints{0};
        for (const auto& [txid, wtx] : wallet->mapWallet) {
            if (!wtx.tx || wtx.isAbandoned()) continue;
            if (GetDigiDollarTxType(*wtx.tx) == DigiDollarTxType::DD_TX_MINT) ++mints;
        }
        return mints;
    }

    void CheckNothingIsLeftOfTheMint()
    {
        BOOST_REQUIRE_MESSAGE(saved_position_id.has_value(),
            "the mint never got as far as saving its records, so this test proved nothing");
        DigiDollarWallet& dd_wallet{*wallet->GetDDWallet()};

        // Nothing the wallet counts, shows or offers for spending is left.
        BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), CAmount{0});
        BOOST_CHECK_EQUAL(dd_wallet.GetDDUTXOs(/*include_unconfirmed=*/true).size(), 0U);
        BOOST_CHECK_EQUAL(dd_wallet.GetDDTimeLocks(/*active_only=*/false).size(), 0U);
        BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), CAmount{0});

        // Nothing was sent and no mint transaction is held here.
        BOOST_CHECK_EQUAL(LiveMintTransactions(), 0U);
        BOOST_CHECK_EQUAL(Assert(m_node.mempool)->size(), 0U);

        // The owner key stays. It costs nothing to keep and it is the one
        // thing that could not be worked out again if a signed transaction
        // somehow reached a block after all.
        CKey owner_key;
        BOOST_CHECK(dd_wallet.GetOwnerKey(*saved_position_id, owner_key));
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_mint_cancel_tests, CancelledMintSetup)

BOOST_AUTO_TEST_CASE(a_mint_stopped_by_a_changed_price_leaves_nothing_behind)
{
    // A new oracle price arrives while the mint is being built, so the
    // collateral it worked out is not what the next block would require. The
    // mint stops after its records are already on disk.
    WhenTheMintIsSaved(nullptr);
    MockOracleManager::GetInstance().SetMockPrice(kLaterPrice);

    const std::string error{CallMint()};
    BOOST_CHECK_MESSAGE(error.find("changed") != std::string::npos,
        "expected the mint to stop because the quote changed, it said: " + error);
    CheckNothingIsLeftOfTheMint();
}

BOOST_AUTO_TEST_CASE(a_mint_stopped_by_a_new_block_leaves_nothing_behind)
{
    // A block arrives while the mint is being built, so the mint was built for
    // a block that has already been decided. It stops after its records are
    // already on disk.
    WhenTheMintIsSaved([this] { MineOneBlockOnAnotherThread(); });

    const std::string error{CallMint()};
    BOOST_CHECK_MESSAGE(!error.empty(), "expected the mint to stop because the chain moved");
    SyncWithValidationInterfaceQueue();
    CheckNothingIsLeftOfTheMint();
}

BOOST_AUTO_TEST_CASE(a_mint_that_is_not_stopped_keeps_its_records)
{
    // The other way round. Nothing interrupts this mint, so everything it
    // saved stays: this is what tells the cleanup to stand down. Wallet
    // broadcasting is off, so the transaction is recorded here and not sent,
    // which is the same wallet state a mint gets with -walletbroadcast=0.
    wallet->SetBroadcastTransactions(false);
    WhenTheMintIsSaved(nullptr);

    const std::string error{CallMint()};
    BOOST_REQUIRE_MESSAGE(error.empty(), "expected the mint to succeed, it said: " + error);
    BOOST_REQUIRE(saved_position_id.has_value());

    DigiDollarWallet& dd_wallet{*wallet->GetDDWallet()};
    const std::vector<WalletCollateralPosition> positions{dd_wallet.GetDDTimeLocks(/*active_only=*/true)};
    BOOST_REQUIRE_EQUAL(positions.size(), 1U);
    BOOST_CHECK_EQUAL(positions[0].dd_minted, kMintCents);
    BOOST_CHECK(positions[0].dgb_collateral > 0);
    BOOST_CHECK_EQUAL(dd_wallet.GetLockedCollateral(), positions[0].dgb_collateral);
    BOOST_CHECK_EQUAL(LiveMintTransactions(), 1U);
    CKey owner_key;
    BOOST_CHECK(dd_wallet.GetOwnerKey(*saved_position_id, owner_key));
}

BOOST_AUTO_TEST_CASE(both_mint_paths_cover_the_whole_window_with_cleanup)
{
    // What keeps this from coming back is a scope guard. It releases the saved
    // records when the mint leaves that part of the code for any reason, so
    // another check added there later, or another error thrown there later, is
    // safe without anyone having to remember to clean up. Both mint paths must
    // create it as soon as they have saved the records, which has to be before
    // the checks that can stop the mint, and stand it down only after the
    // transaction has been committed.
    const std::string rpc_mint{FunctionBody(ReadRepositoryFile("src/rpc/digidollar.cpp"),
                                            "RPCHelpMan mintdigidollar()", "RPCHelpMan senddigidollar()")};
    const std::string qt_mint{FunctionBody(ReadRepositoryFile("src/qt/walletmodel.cpp"),
                                           "WalletModel::mintDigiDollar", "WalletModel::redeemDigiDollar")};

    const size_t rpc_save{Where(rpc_mint, "RecordPendingMint(", "the mint command no longer saves its records")};
    const size_t rpc_cleanup{Where(rpc_mint, "SavedMintCleanup", "the mint command does not arrange to release a mint it does not send")};
    const size_t rpc_recheck{Where(rpc_mint, "RecheckCandidateHealth", "the mint command no longer rechecks the chain before committing")};
    const size_t rpc_commit{Where(rpc_mint, "CommitTransaction(tx", "the mint command no longer commits the transaction")};
    const size_t rpc_keep{Where(rpc_mint, "KeepRecords()", "the mint command never stands the cleanup down")};

    BOOST_CHECK_MESSAGE(rpc_save < rpc_cleanup && rpc_cleanup < rpc_recheck,
        "the mint command can stop between saving its records and arranging to release them");
    BOOST_CHECK_MESSAGE(rpc_cleanup < rpc_commit, "the mint command arranges the release after committing");
    BOOST_CHECK_MESSAGE(rpc_commit < rpc_keep, "the mint command keeps its records before the transaction is committed");

    const size_t qt_save{Where(qt_mint, "RecordPendingMint(", "the wallet window no longer saves its records")};
    const size_t qt_cleanup{Where(qt_mint, "SavedMintCleanup", "the wallet window does not arrange to release a mint it does not send")};
    const size_t qt_recheck{Where(qt_mint, "candidateHealthSnapshot.write()", "the wallet window no longer rechecks the chain before committing")};
    const size_t qt_commit{Where(qt_mint, "CommitTransaction(txRef", "the wallet window no longer commits the transaction")};
    const size_t qt_keep{Where(qt_mint, "KeepRecords()", "the wallet window never stands the cleanup down")};

    BOOST_CHECK_MESSAGE(qt_save < qt_cleanup && qt_cleanup < qt_recheck,
        "the wallet window can stop between saving its records and arranging to release them");
    BOOST_CHECK_MESSAGE(qt_cleanup < qt_commit, "the wallet window arranges the release after committing");
    BOOST_CHECK_MESSAGE(qt_commit < qt_keep, "the wallet window keeps its records before the transaction is committed");
}

BOOST_AUTO_TEST_SUITE_END()
