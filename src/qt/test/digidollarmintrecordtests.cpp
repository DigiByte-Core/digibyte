// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// A mint started from the wallet window locks DGB in a vault and creates a
// DigiDollar token. The only things that let the owner redeem that vault
// later are the owner key and the position record in the wallet file. Once the
// mint transaction has been sent it can be mined whether or not this process
// survives, so both records have to be on disk before it is sent, and a write
// that fails has to stop the mint instead of being ignored.
//
// These tests drive WalletModel::mintDigiDollar directly, which is the
// function the Mint button calls.

#include <qt/test/digidollarmintrecordtests.h>

#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <util/chaintype.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <thread>

#include <QApplication>
#include <QCoreApplication>

using wallet::AddWallet;
using wallet::GetMockableDatabase;
using wallet::RemoveWallet;
using wallet::WalletContext;

namespace {

//! Mine a block that carries a valid mock oracle quote, so that a mint made
//! straight afterwards has a price to work from.
void CreateAndProcessOracleQuoteBlock(TestChain100Setup& test, CAmount price_micro_usd)
{
    MockOracleManager& mock_oracle = MockOracleManager::GetInstance();
    mock_oracle.SetEnabled(true);
    mock_oracle.SetMockPrice(price_micro_usd);

    OracleBundleManager& oracle_manager = OracleBundleManager::GetInstance();
    oracle_manager.SetEnabled(true);

    Chainstate& chainstate = Assert(test.m_node.chainman)->ActiveChainstate();
    const int block_height = WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->nHeight + 1);
    const CScript coinbase_script = GetScriptForRawPubKey(test.coinbaseKey.GetPubKey());
    CBlock block = test.CreateBlock({}, coinbase_script, chainstate);

    COracleBundle bundle = mock_oracle.CreateMockMuSig2Bundle(block_height, block.GetBlockTime());
    std::string error;
    QVERIFY2(OracleBundleManager::ValidateMuSig2Bundle(
                 bundle, block_height, Params().GetConsensus(), error),
             error.c_str());
    QVERIFY(oracle_manager.UpdateBundle(bundle));
    QVERIFY(oracle_manager.AddOracleBundleToBlock(block, block_height));

    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(GetPoWAlgoHash(block), block.nBits, Params().GetConsensus())) {
        ++block.nNonce;
    }

    std::shared_ptr<const CBlock> shared_block = std::make_shared<const CBlock>(block);
    QVERIFY(Assert(test.m_node.chainman)->ProcessNewBlock(shared_block, true, true, nullptr));
}

//! The smallest set of models the wallet window needs to run a mint.
struct DigiDollarMiniGUI {
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;
    std::unique_ptr<const PlatformStyle> platformStyle;

    explicit DigiDollarMiniGUI(interfaces::Node& node) : optionsModel(node)
    {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
        platformStyle.reset(PlatformStyle::instantiate("other"));
    }

    void initModelForWallet(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
    {
        WalletContext& context = *node.walletLoader().context();
        AddWallet(context, wallet);
        walletModel = std::make_unique<WalletModel>(
            interfaces::MakeWallet(context, wallet), *clientModel, platformStyle.get());
        RemoveWallet(context, wallet, std::nullopt);
    }
};

//! A funded descriptor wallet on a chain that has a fresh oracle quote.
std::shared_ptr<wallet::CWallet> PrepareMintableWallet(interfaces::Node& node, TestChain100Setup& test)
{
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    CreateAndProcessOracleQuoteBlock(test, 500000);

    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);
    wallet->SetBroadcastTransactions(true);
    wallet->EnsureDDWallet();
    return wallet;
}

//! How many DigiDollar mint transactions the wallet still holds that have not
//! been abandoned. These are the ones the transaction list would show.
size_t CountLiveMintTransactions(wallet::CWallet& wallet)
{
    size_t live = 0;
    LOCK(wallet.cs_wallet);
    for (const auto& entry : wallet.mapWallet) {
        const wallet::CWalletTx& wtx = entry.second;
        if (wtx.isAbandoned()) continue;
        if (GetDigiDollarTxType(*wtx.tx) == DigiDollarTxType::DD_TX_MINT) ++live;
    }
    return live;
}

//! True when a wallet database key belongs to a record of this kind. Keys start
//! with the record's name, for example "ddposition" or "ddownerkey".
bool KeyIsRecord(Span<const std::byte> key, std::string_view name)
{
    const std::string_view text{reinterpret_cast<const char*>(key.data()), key.size()};
    return text.find(name) != std::string_view::npos;
}

//! How many DigiDollar owner keys the wallet file holds. A mint that is given
//! up on keeps its owner key, because that is the one thing that could not be
//! worked out again if the transaction somehow reached a block after all.
size_t CountOwnerKeysInWalletFile(wallet::CWallet& wallet)
{
    size_t rows = 0;
    for (const auto& [key, value] : GetMockableDatabase(wallet).m_records) {
        if (KeyIsRecord(Span{key.data(), key.size()}, "ddownerkey")) ++rows;
    }
    return rows;
}

//! Nothing may be left of a mint that was never sent. A leftover token record
//! counts towards this wallet's DigiDollar balance and is offered for spending,
//! so the owner would be shown DigiDollars that cannot be spent.
void CheckNothingIsLeftOfTheMint(wallet::CWallet& wallet, CTxMemPool& mempool)
{
    DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    QCOMPARE(dd_wallet->GetDDTimeLocks(/*active_only=*/false).size(), static_cast<size_t>(0));
    QCOMPARE(dd_wallet->GetTotalDDBalance(), CAmount(0));
    QCOMPARE(dd_wallet->GetDDUTXOs(/*include_unconfirmed=*/true).size(), static_cast<size_t>(0));
    QCOMPARE(dd_wallet->GetLockedCollateral(), CAmount(0));
    QCOMPARE(CountLiveMintTransactions(wallet), static_cast<size_t>(0));
    QCOMPARE(mempool.size(), static_cast<size_t>(0));
    QCOMPARE(CountOwnerKeysInWalletFile(wallet), static_cast<size_t>(1));
}

} // namespace

void DigiDollarMintRecordTests::mintSavesItsRecordBeforeSendingTheTransaction()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarMintRecordTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    // The mempool is made to refuse the mint by setting a relay fee far above
    // the fixed fee a DigiDollar mint pays. The send therefore fails at the
    // last possible moment, after everything else has been done. If the wallet
    // still holds the position record and the owner key at that point, they
    // must have been written before the transaction was sent, which is what
    // this test is here to prove.
    TestChain100Setup test{ChainType::REGTEST, {"-minrelaytxfee=1"}};
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = PrepareMintableWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->pollBalanceChanged();

    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    QVERIFY2(result.status != WalletModel::OK,
             "the mint was expected to fail because the mempool refuses its fee");

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    // The record of the attempt survives, so a vault that did get mined
    // somewhere could still be redeemed.
    const std::vector<WalletCollateralPosition> positions = dd_wallet->GetDDTimeLocks(/*active_only=*/false);
    QCOMPARE(positions.size(), static_cast<size_t>(1));
    const uint256 position_id = positions[0].dd_timelock_id;
    QCOMPARE(positions[0].dd_minted, CAmount(10000));

    CKey owner_key;
    QVERIFY(dd_wallet->GetOwnerKey(position_id, owner_key));
    QVERIFY(owner_key.IsValid());
    QCOMPARE(positions[0].owner_keyid, owner_key.GetPubKey().GetID());

    // Reading the same wallet file again shows the record, so it reached disk
    // rather than only memory.
    DigiDollarWallet reloaded(&*wallet);
    const std::vector<WalletCollateralPosition> reloaded_positions =
        reloaded.GetDDTimeLocks(/*active_only=*/false);
    QCOMPARE(reloaded_positions.size(), static_cast<size_t>(1));
    QCOMPARE(reloaded_positions[0].dd_timelock_id, position_id);
    CKey reloaded_key;
    QVERIFY(reloaded.GetOwnerKey(position_id, reloaded_key));

    // Nothing was sent, so nothing is held: no active vault, no DigiDollar
    // balance from the token output, no DGB tied up, and the mint transaction
    // is abandoned so its inputs can be spent again.
    QCOMPARE(dd_wallet->GetDDTimeLocks(/*active_only=*/true).size(), static_cast<size_t>(0));
    QVERIFY(!positions[0].is_active);
    QCOMPARE(dd_wallet->GetLockedCollateral(), CAmount(0));
    QCOMPARE(CountLiveMintTransactions(*wallet), static_cast<size_t>(0));
    {
        LOCK(wallet->cs_wallet);
        const wallet::CWalletTx* wtx = wallet->GetWalletTx(position_id);
        QVERIFY(wtx != nullptr);
        QVERIFY(wtx->isAbandoned());
        for (unsigned int n = 0; n < wtx->tx->vout.size(); ++n) {
            QVERIFY(!wallet->IsLockedCoin(COutPoint(position_id, n)));
        }
    }

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarMintRecordTests::mintDoesNotSendWhenTheWalletCannotSaveIt()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarMintRecordTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    // A wallet file that cannot be written to (a full disk, a broken file) must
    // stop the mint. Sending a transaction the wallet cannot describe is how a
    // vault ends up with nothing to redeem it with.
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = PrepareMintableWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->pollBalanceChanged();

    // Keep safety-record reads available so coin selection can reach the mint
    // persistence boundary. A global database failure correctly blocks inputs
    // earlier, when Paymaster reservations cannot be read.
    bool owner_key_write_refused{false};
    auto& database = GetMockableDatabase(*wallet);
    database.m_refuse_write = [&](Span<const std::byte> key) {
        if (!KeyIsRecord(key, "ddownerkey")) return false;
        owner_key_write_refused = true;
        return true;
    };
    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    database.m_refuse_write = {};

    QVERIFY2(owner_key_write_refused, "the mint must reach the owner-key persistence boundary");
    QVERIFY2(result.status != WalletModel::OK,
             "the mint was expected to fail because the wallet database refuses writes");
    // The window has to say the transaction was not sent, otherwise the user
    // cannot tell a failed save from a failed broadcast.
    QVERIFY2(result.reasonFailed.contains("Nothing was sent"),
             result.reasonFailed.toUtf8().constData());

    // Nothing reached the mempool and nothing was recorded as a mint.
    QCOMPARE(Assert(test.m_node.mempool)->size(), static_cast<size_t>(0));
    QCOMPARE(CountLiveMintTransactions(*wallet), static_cast<size_t>(0));

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    QCOMPARE(dd_wallet->GetDDTimeLocks(/*active_only=*/false).size(), static_cast<size_t>(0));

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarMintRecordTests::mintStoppedByAChangedPriceLeavesNothingBehind()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarMintRecordTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    // The mint writes its owner key and its position record before it sends
    // the transaction, and only then checks that the chain has not moved under
    // it. A new oracle price arriving in between means the collateral the mint
    // worked out is not what the next block would require, so the mint gives
    // up. Nothing may be left behind for a transaction that was never sent.
    //
    // The new price is published at the moment the wallet writes the position
    // record, which is inside the mint, after the save and before the send.
    TestChain100Setup test{ChainType::REGTEST, {"-digidollaractivationheight=100", "-ddthawdayheight=100"}};
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = PrepareMintableWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->pollBalanceChanged();

    const int next_height = WITH_LOCK(cs_main, return Assert(test.m_node.chainman)->ActiveChain().Tip()->nHeight + 1);
    bool records_were_saved = false;
    GetMockableDatabase(*wallet).m_on_write = [&](Span<const std::byte> key) {
        if (records_were_saved || !KeyIsRecord(key, "ddposition")) return;
        records_were_saved = true;
        MockOracleManager& mock_oracle = MockOracleManager::GetInstance();
        mock_oracle.SetMockPrice(510000);
        OracleBundleManager::GetInstance().UpdateBundle(mock_oracle.CreateMockMuSig2Bundle(next_height));
    };

    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    GetMockableDatabase(*wallet).m_on_write = nullptr;

    QVERIFY2(records_were_saved,
             qPrintable(QString("the mint never saved its records, so this test proved nothing: %1")
                            .arg(result.reasonFailed)));
    QVERIFY2(result.status != WalletModel::OK, "the mint was expected to stop because the price changed");
    CheckNothingIsLeftOfTheMint(*wallet, *Assert(test.m_node.mempool));

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarMintRecordTests::mintStoppedByANewBlockLeavesNothingBehind()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarMintRecordTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    // The same window, reached the other way: a block arrives while the mint is
    // being built, so the mint was built for a block that has already been
    // decided and it gives up.
    //
    // The block is connected from another thread. The mint holds the wallet
    // lock while the wallet writes its records, and connecting a block takes
    // the chain lock, so connecting it on this thread would take those two
    // locks in the order that can leave a node stuck with no way out.
    TestChain100Setup test{ChainType::REGTEST, {"-digidollaractivationheight=100", "-ddthawdayheight=100"}};
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = PrepareMintableWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->pollBalanceChanged();

    bool records_were_saved = false;
    GetMockableDatabase(*wallet).m_on_write = [&](Span<const std::byte> key) {
        if (records_were_saved || !KeyIsRecord(key, "ddposition")) return;
        records_were_saved = true;
        std::thread miner{[&test] {
            test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
        }};
        miner.join();
    };

    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    GetMockableDatabase(*wallet).m_on_write = nullptr;
    SyncWithValidationInterfaceQueue();

    QVERIFY2(records_were_saved,
             qPrintable(QString("the mint never saved its records, so this test proved nothing: %1")
                            .arg(result.reasonFailed)));
    QVERIFY2(result.status != WalletModel::OK, "the mint was expected to stop because a block arrived");
    CheckNothingIsLeftOfTheMint(*wallet, *Assert(test.m_node.mempool));

    MockOracleManager::GetInstance().Reset();
}
