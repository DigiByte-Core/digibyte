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
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <memory>

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

    GetMockableDatabase(*wallet).m_pass = false;
    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    GetMockableDatabase(*wallet).m_pass = true;

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
