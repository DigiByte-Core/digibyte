// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// What these tests cover.
//
// The wallet turns one transaction into one or more rows in the transaction
// list. For a DigiDollar mint the wallet used to produce a "sent" row and a
// "received" row for the same new DigiDollars, which reads as if the money had
// been sent away, and it folded the network fee into the amount shown as
// locked collateral. These tests pin the rows a mint, a transfer and a
// redemption produce, and the text of the details window for each.
//
// The mint is a real mint made by the wallet on a regtest chain. The transfer
// and the redemption are built here in the same shape the DigiDollar
// transaction builder produces, and handed to the wallet, because a real
// redemption needs the collateral lock to expire, which is hundreds of blocks.

#include <qt/test/ddtransactionrecordtests.h>
#include <qt/test/util.h>

#include <consensus/digidollar.h>
#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactiondesc.h>
#include <qt/transactionrecord.h>
#include <qt/walletmodel.h>
#include <script/script.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <memory>
#include <string>

#include <QCoreApplication>
#include <QRegularExpression>
#include <QString>
#include <QTextDocumentFragment>

using wallet::AddWallet;
using wallet::RemoveWallet;
using wallet::WalletContext;

namespace {

//! Everything a WalletModel needs, so a test can ask the wallet for a
//! transaction the way the GUI does.
struct MiniGui {
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;
    std::unique_ptr<const PlatformStyle> platformStyle;

    explicit MiniGui(interfaces::Node& node) : optionsModel(node)
    {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
        platformStyle.reset(PlatformStyle::instantiate("other"));
    }

    void openWallet(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
    {
        WalletContext& context = *node.walletLoader().context();
        AddWallet(context, wallet);
        walletModel = std::make_unique<WalletModel>(
            interfaces::MakeWallet(context, wallet), *clientModel, platformStyle.get());
        RemoveWallet(context, wallet, std::nullopt);
    }
};

//! Mine one block that carries a signed oracle price, so the chain has a price
//! a mint can be built against.
void MineBlockWithOraclePrice(TestChain100Setup& test, CAmount price_micro_usd)
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

//! A zero value taproot output is how a DigiDollar token is held on chain.
CTxOut DDTokenOut(const CTxDestination& dest)
{
    return CTxOut(0, GetScriptForDestination(dest));
}

//! A taproot destination nobody in this test owns. The wallet only has to say
//! no to it, so the key does not have to be spendable by anyone.
CTxDestination ForeignTaprootDest(unsigned char seed)
{
    uint256 raw;
    raw.data()[0] = seed;
    raw.data()[31] = 0x42;
    return CTxDestination{WitnessV1Taproot(XOnlyPubKey(raw))};
}

//! The data output a transfer carries: the marker, the type, then one amount
//! for each DigiDollar output in the order those outputs appear.
CTxOut TransferDataOut(const std::vector<CAmount>& amounts_in_cents)
{
    CScript script;
    script << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(2);
    for (const CAmount amount : amounts_in_cents) script << CScriptNum(amount);
    return CTxOut(0, script);
}

//! The data output a redemption carries when it hands DigiDollars back.
CTxOut RedeemDataOut(CAmount change_in_cents)
{
    CScript script;
    script << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(3)
           << CScriptNum(change_in_cents);
    return CTxOut(0, script);
}

//! Put a transaction in the wallet as if it had been seen in the mempool.
void PutInWallet(wallet::CWallet& wallet, const CTransactionRef& tx)
{
    LOCK(wallet.cs_wallet);
    QVERIFY(wallet.AddToWallet(tx, wallet::TxStateInMempool{}) != nullptr);
}

int CountRowsOfType(const QList<TransactionRecord>& rows, TransactionRecord::Type type)
{
    int count = 0;
    for (const TransactionRecord& row : rows) {
        if (row.type == type) ++count;
    }
    return count;
}

const TransactionRecord* FindRow(const QList<TransactionRecord>& rows, TransactionRecord::Type type)
{
    for (const TransactionRecord& row : rows) {
        if (row.type == type) return &row;
    }
    return nullptr;
}

//! Add up the DGB on every row. For a transaction the wallet paid for, this
//! must equal the wallet's own net change for that transaction.
CAmount TotalDgbOnRows(const QList<TransactionRecord>& rows)
{
    CAmount total = 0;
    for (const TransactionRecord& row : rows) total += row.debit + row.credit;
    return total;
}

//! The text of the details window, down to where the developer dump starts.
//!
//! When the node is running with logging turned on, the details window ends
//! with a raw dump of the transaction, which lists every output including the
//! ones worth no DGB. That part is for developers and is not what people read,
//! and the tests here run with logging on, so the checks below look only at
//! the part above it.
//! Matches an amount of exactly no DGB, and not the tail of a real amount
//! such as 2020.00000000 DGB.
QRegularExpression EmptyDgbAmount()
{
    return QRegularExpression(QStringLiteral("(?<![0-9])0\\.00000000 DGB"));
}

QString DetailsPlainText(interfaces::Node& node, interfaces::Wallet& wallet, TransactionRecord& row)
{
    const QString html = TransactionDesc::toHTML(node, wallet, &row, DigiByteUnit::DGB);
    const QString text = QTextDocumentFragment::fromHtml(html).toPlainText();
    const int dump_start = text.indexOf(QStringLiteral("Debug information"));
    return dump_start < 0 ? text : text.left(dump_start);
}

} // namespace

void DDTransactionRecordTests::mintRowsShowMintedAmountCollateralAndFeeOnce()
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MineBlockWithOraclePrice(test, 500000);
    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);
    wallet->SetBroadcastTransactions(true);
    wallet->EnsureDDWallet();

    MiniGui gui(m_node);
    gui.openWallet(m_node, wallet);
    gui.walletModel->pollBalanceChanged();

    // Mint one hundred dollars on the shortest lock.
    const CAmount minted_cents = 10000;
    WalletModel::DigiDollarMintResult result = gui.walletModel->mintDigiDollar(minted_cents, 0);
    QVERIFY2(result.status == WalletModel::OK, result.reasonFailed.toUtf8().constData());

    uint256 mint_txid;
    mint_txid.SetHex(result.txid.toStdString());

    interfaces::WalletTx wtx = gui.walletModel->wallet().getWalletTx(mint_txid);
    QVERIFY(wtx.tx != nullptr);
    QCOMPARE(DigiDollar::GetDigiDollarTxType(*wtx.tx), DigiDollar::DD_TX_MINT);

    const CAmount collateral = wtx.tx->vout[0].nValue;
    const CAmount fee = wtx.debit - wtx.tx->GetValueOut();
    QVERIFY(collateral > 0);
    QVERIFY(fee > 0);

    const QList<TransactionRecord> rows = TransactionRecord::decomposeTransaction(wtx);

    // The new DigiDollars appear once, with no DGB on that row.
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::DDMint), 1);
    const TransactionRecord* mint_row = FindRow(rows, TransactionRecord::DDMint);
    QCOMPARE(mint_row->ddAmount, minted_cents);
    QCOMPARE(mint_row->debit, CAmount(0));
    QCOMPARE(mint_row->credit, CAmount(0));

    // The locked DGB appears once and is the collateral alone. Before this
    // work the fee was added to it, so the wallet said more was locked than
    // the vault actually holds.
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::DDTimeLockCollateral), 1);
    QCOMPARE(FindRow(rows, TransactionRecord::DDTimeLockCollateral)->debit, -collateral);

    // The fee appears once, on its own row.
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::DDSendFee), 1);
    QCOMPARE(FindRow(rows, TransactionRecord::DDSendFee)->debit, -fee);

    // A mint is not a transfer, so neither transfer row may appear.
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::DDSend), 0);
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::DDRecv), 0);

    // The DGB on the rows must add up to what the wallet actually lost.
    QCOMPARE(TotalDgbOnRows(rows), wtx.credit - wtx.debit);
    QCOMPARE(TotalDgbOnRows(rows), -(collateral + fee));

    MockOracleManager::GetInstance().Reset();
}

void DDTransactionRecordTests::mintDetailsShowDigiDollarFacts()
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MineBlockWithOraclePrice(test, 500000);
    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);
    wallet->SetBroadcastTransactions(true);
    wallet->EnsureDDWallet();

    MiniGui gui(m_node);
    gui.openWallet(m_node, wallet);
    gui.walletModel->pollBalanceChanged();

    WalletModel::DigiDollarMintResult result = gui.walletModel->mintDigiDollar(10000, 0);
    QVERIFY2(result.status == WalletModel::OK, result.reasonFailed.toUtf8().constData());

    uint256 mint_txid;
    mint_txid.SetHex(result.txid.toStdString());
    interfaces::WalletTx wtx = gui.walletModel->wallet().getWalletTx(mint_txid);
    QVERIFY(wtx.tx != nullptr);

    QList<TransactionRecord> rows = TransactionRecord::decomposeTransaction(wtx);
    QVERIFY(!rows.isEmpty());

    int64_t unlock_height{0};
    QVERIFY(DigiDollarWallet::ExtractUnlockHeightFromOpReturn(*wtx.tx, unlock_height));

    for (TransactionRecord& row : rows) {
        const QString text = DetailsPlainText(m_node, gui.walletModel->wallet(), row);
        QVERIFY2(!text.contains(QStringLiteral("Null")), qPrintable(text));
        QVERIFY2(!text.contains(EmptyDgbAmount()), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("$100.00")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("1 hour")), qPrintable(text));
        QVERIFY2(text.contains(QString::number(unlock_height)), qPrintable(text));
        QVERIFY2(text.contains(QString::fromStdString(mint_txid.ToString())), qPrintable(text));
    }

    MockOracleManager::GetInstance().Reset();
}

void DDTransactionRecordTests::transferRowsUnchanged()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);

    MiniGui gui(m_node);
    gui.openWallet(m_node, wallet);

    const CTxDestination our_dd = *Assert(wallet->GetNewDestination(OutputType::BECH32M, ""));
    const CTxDestination our_dgb = *Assert(wallet->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination their_dd = ForeignTaprootDest(0x11);
    const CAmount fee = COIN / 10;

    // Two shapes of transfer, because the wallet has two ways of breaking one
    // up. Which way it takes depends on whether the wallet can spend the
    // DigiDollar input itself.
    //
    // First shape: the DigiDollar being moved is held on a taproot output the
    // wallet cannot spend, and only the DGB paying the fee is the wallet's.
    CMutableTransaction funding_a;
    funding_a.vin.resize(1);
    funding_a.vin[0].prevout = COutPoint(uint256::ONE, 0);
    funding_a.vout.push_back(DDTokenOut(their_dd));
    funding_a.vout.push_back(CTxOut(10 * COIN, GetScriptForDestination(our_dgb)));
    const CTransactionRef funding_a_tx = MakeTransactionRef(funding_a);
    PutInWallet(*wallet, funding_a_tx);

    CMutableTransaction transfer_a;
    transfer_a.SetDigiDollarType(DD_TX_TRANSFER);
    transfer_a.vin.push_back(CTxIn(COutPoint(funding_a_tx->GetHash(), 0)));
    transfer_a.vin.push_back(CTxIn(COutPoint(funding_a_tx->GetHash(), 1)));
    transfer_a.vout.push_back(DDTokenOut(their_dd));
    transfer_a.vout.push_back(CTxOut(10 * COIN - fee,
                                     GetScriptForDestination(*Assert(wallet->GetNewChangeDestination(OutputType::BECH32)))));
    transfer_a.vout.push_back(TransferDataOut({1000}));
    const CTransactionRef transfer_a_tx = MakeTransactionRef(transfer_a);
    PutInWallet(*wallet, transfer_a_tx);

    interfaces::WalletTx wtx_a = gui.walletModel->wallet().getWalletTx(transfer_a_tx->GetHash());
    QVERIFY(wtx_a.tx != nullptr);
    QCOMPARE(DigiDollar::GetDigiDollarTxType(*wtx_a.tx), DigiDollar::DD_TX_TRANSFER);
    QVERIFY(wtx_a.txout_is_change[1]);

    QList<TransactionRecord> rows_a = TransactionRecord::decomposeTransaction(wtx_a);
    QCOMPARE(rows_a.size(), 2);
    QCOMPARE(CountRowsOfType(rows_a, TransactionRecord::DDSend), 1);
    QCOMPARE(FindRow(rows_a, TransactionRecord::DDSend)->ddAmount, CAmount(-1000));
    QCOMPARE(FindRow(rows_a, TransactionRecord::DDSend)->debit, CAmount(0));
    QCOMPARE(CountRowsOfType(rows_a, TransactionRecord::DDSendFee), 1);
    QCOMPARE(FindRow(rows_a, TransactionRecord::DDSendFee)->debit, -(10 * COIN));
    QCOMPARE(FindRow(rows_a, TransactionRecord::DDSendFee)->credit, 10 * COIN - fee);
    QCOMPARE(CountRowsOfType(rows_a, TransactionRecord::DDMint), 0);
    QCOMPARE(TotalDgbOnRows(rows_a), wtx_a.credit - wtx_a.debit);
    QCOMPARE(TotalDgbOnRows(rows_a), -fee);

    // Second shape: the wallet holds the DigiDollar itself, so every input is
    // its own. Here the fee rides on the send row and there is no fee row.
    CMutableTransaction funding_b;
    funding_b.vin.resize(1);
    funding_b.vin[0].prevout = COutPoint(uint256::ONE, 1);
    funding_b.vout.push_back(DDTokenOut(our_dd));
    funding_b.vout.push_back(CTxOut(10 * COIN, GetScriptForDestination(our_dgb)));
    const CTransactionRef funding_b_tx = MakeTransactionRef(funding_b);
    PutInWallet(*wallet, funding_b_tx);

    CMutableTransaction transfer_b;
    transfer_b.SetDigiDollarType(DD_TX_TRANSFER);
    transfer_b.vin.push_back(CTxIn(COutPoint(funding_b_tx->GetHash(), 0)));
    transfer_b.vin.push_back(CTxIn(COutPoint(funding_b_tx->GetHash(), 1)));
    transfer_b.vout.push_back(DDTokenOut(their_dd));
    transfer_b.vout.push_back(CTxOut(10 * COIN - fee,
                                     GetScriptForDestination(*Assert(wallet->GetNewChangeDestination(OutputType::BECH32)))));
    transfer_b.vout.push_back(TransferDataOut({1000}));
    const CTransactionRef transfer_b_tx = MakeTransactionRef(transfer_b);
    PutInWallet(*wallet, transfer_b_tx);

    interfaces::WalletTx wtx_b = gui.walletModel->wallet().getWalletTx(transfer_b_tx->GetHash());
    QVERIFY(wtx_b.tx != nullptr);
    QVERIFY(wtx_b.txout_is_change[1]);

    QList<TransactionRecord> rows_b = TransactionRecord::decomposeTransaction(wtx_b);
    QCOMPARE(rows_b.size(), 1);
    QCOMPARE(CountRowsOfType(rows_b, TransactionRecord::DDSend), 1);
    QCOMPARE(FindRow(rows_b, TransactionRecord::DDSend)->ddAmount, CAmount(-1000));
    QCOMPARE(FindRow(rows_b, TransactionRecord::DDSend)->debit, -fee);
    QCOMPARE(CountRowsOfType(rows_b, TransactionRecord::DDMint), 0);
    QCOMPARE(TotalDgbOnRows(rows_b), wtx_b.credit - wtx_b.debit);
}

void DDTransactionRecordTests::transferDetailsShowDigiDollarAmounts()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);

    MiniGui gui(m_node);
    gui.openWallet(m_node, wallet);

    const CTxDestination our_dd = *Assert(wallet->GetNewDestination(OutputType::BECH32M, ""));
    const CTxDestination our_dgb = *Assert(wallet->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination our_dgb_change = *Assert(wallet->GetNewChangeDestination(OutputType::BECH32));
    const CTxDestination their_dd = ForeignTaprootDest(0x21);

    CMutableTransaction funding;
    funding.vin.resize(1);
    funding.vin[0].prevout = COutPoint(uint256::ONE, 0);
    funding.vout.push_back(DDTokenOut(our_dd));
    funding.vout.push_back(CTxOut(10 * COIN, GetScriptForDestination(our_dgb)));
    const CTransactionRef funding_tx = MakeTransactionRef(funding);
    PutInWallet(*wallet, funding_tx);

    CMutableTransaction transfer;
    transfer.SetDigiDollarType(DD_TX_TRANSFER);
    transfer.vin.push_back(CTxIn(COutPoint(funding_tx->GetHash(), 0)));
    transfer.vin.push_back(CTxIn(COutPoint(funding_tx->GetHash(), 1)));
    transfer.vout.push_back(DDTokenOut(their_dd));
    transfer.vout.push_back(CTxOut(10 * COIN - COIN / 10, GetScriptForDestination(our_dgb_change)));
    transfer.vout.push_back(TransferDataOut({1000}));
    const CTransactionRef transfer_tx = MakeTransactionRef(transfer);
    PutInWallet(*wallet, transfer_tx);

    interfaces::WalletTx wtx = gui.walletModel->wallet().getWalletTx(transfer_tx->GetHash());
    QList<TransactionRecord> rows = TransactionRecord::decomposeTransaction(wtx);
    QVERIFY(!rows.isEmpty());

    for (TransactionRecord& row : rows) {
        const QString text = DetailsPlainText(m_node, gui.walletModel->wallet(), row);
        QVERIFY2(!text.contains(QStringLiteral("Null")), qPrintable(text));
        QVERIFY2(!text.contains(EmptyDgbAmount()), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("$10.00")), qPrintable(text));
    }
}

void DDTransactionRecordTests::redeemRowsKeepDgbRowsAndAddReturnedChange()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);

    MiniGui gui(m_node);
    gui.openWallet(m_node, wallet);

    const CTxDestination our_dd = *Assert(wallet->GetNewDestination(OutputType::BECH32M, ""));
    const CTxDestination our_dd_change = *Assert(wallet->GetNewDestination(OutputType::BECH32M, ""));
    const CTxDestination our_dgb = *Assert(wallet->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination collateral_return = *Assert(wallet->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination fee_change = *Assert(wallet->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination vault = ForeignTaprootDest(0x31);

    const CAmount collateral = 500 * COIN;

    // The mint being closed: output zero is the locked vault, which the wallet
    // never owns, and output one is the DigiDollar token it created.
    CMutableTransaction mint;
    mint.SetDigiDollarType(DD_TX_MINT);
    mint.vin.resize(1);
    mint.vin[0].prevout = COutPoint(uint256::ONE, 0);
    mint.vout.push_back(CTxOut(collateral, GetScriptForDestination(vault)));
    mint.vout.push_back(DDTokenOut(our_dd));
    const CTransactionRef mint_tx = MakeTransactionRef(mint);
    PutInWallet(*wallet, mint_tx);

    CMutableTransaction funding;
    funding.vin.resize(1);
    funding.vin[0].prevout = COutPoint(uint256::ONE, 1);
    funding.vout.push_back(CTxOut(COIN, GetScriptForDestination(our_dgb)));
    const CTransactionRef funding_tx = MakeTransactionRef(funding);
    PutInWallet(*wallet, funding_tx);

    // Close the vault, burning eight dollars of a ten dollar holding and
    // taking two dollars back as change.
    CMutableTransaction redeem;
    redeem.SetDigiDollarType(DD_TX_REDEEM);
    redeem.vin.push_back(CTxIn(COutPoint(mint_tx->GetHash(), 0), CScript(), 0xFFFFFFFE));
    redeem.vin.push_back(CTxIn(COutPoint(mint_tx->GetHash(), 1), CScript(), 0xFFFFFFFE));
    redeem.vin.push_back(CTxIn(COutPoint(funding_tx->GetHash(), 0)));
    redeem.vout.push_back(CTxOut(collateral, GetScriptForDestination(collateral_return)));  // 0
    redeem.vout.push_back(DDTokenOut(our_dd_change));                                       // 1
    redeem.vout.push_back(RedeemDataOut(200));                                              // 2
    redeem.vout.push_back(CTxOut(COIN - COIN / 10, GetScriptForDestination(fee_change)));   // 3
    const CTransactionRef redeem_tx = MakeTransactionRef(redeem);
    PutInWallet(*wallet, redeem_tx);

    interfaces::WalletTx wtx = gui.walletModel->wallet().getWalletTx(redeem_tx->GetHash());
    QVERIFY(wtx.tx != nullptr);
    QCOMPARE(DigiDollar::GetDigiDollarTxType(*wtx.tx), DigiDollar::DD_TX_REDEEM);

    const QList<TransactionRecord> rows = TransactionRecord::decomposeTransaction(wtx);

    // The two DGB rows are exactly what they were before: the collateral comes
    // back whole on its own row, and the leftover fee money on another.
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::DDCollateralReturn), 1);
    QCOMPARE(FindRow(rows, TransactionRecord::DDCollateralReturn)->credit, collateral);
    QCOMPARE(FindRow(rows, TransactionRecord::DDCollateralReturn)->ddAmount, CAmount(0));
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::RecvWithAddress), 1);
    QCOMPARE(FindRow(rows, TransactionRecord::RecvWithAddress)->credit, COIN - COIN / 10);

    // New row: the two dollars the redemption handed back.
    QCOMPARE(CountRowsOfType(rows, TransactionRecord::DDChangeReturned), 1);
    const TransactionRecord* change_row = FindRow(rows, TransactionRecord::DDChangeReturned);
    QCOMPARE(change_row->ddAmount, CAmount(200));
    QCOMPARE(change_row->debit, CAmount(0));
    QCOMPARE(change_row->credit, CAmount(0));

    QCOMPARE(rows.size(), 3);
}

void DDTransactionRecordTests::redeemDetailsShowDigiDollarFacts()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);

    MiniGui gui(m_node);
    gui.openWallet(m_node, wallet);

    const CTxDestination our_dd = *Assert(wallet->GetNewDestination(OutputType::BECH32M, ""));
    const CTxDestination our_dd_change = *Assert(wallet->GetNewDestination(OutputType::BECH32M, ""));
    const CTxDestination our_dgb = *Assert(wallet->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination collateral_return = *Assert(wallet->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination vault = ForeignTaprootDest(0x41);

    const CAmount collateral = 500 * COIN;

    CMutableTransaction mint;
    mint.SetDigiDollarType(DD_TX_MINT);
    mint.vin.resize(1);
    mint.vin[0].prevout = COutPoint(uint256::ONE, 0);
    mint.vout.push_back(CTxOut(collateral, GetScriptForDestination(vault)));
    mint.vout.push_back(DDTokenOut(our_dd));
    const CTransactionRef mint_tx = MakeTransactionRef(mint);
    PutInWallet(*wallet, mint_tx);

    CMutableTransaction funding;
    funding.vin.resize(1);
    funding.vin[0].prevout = COutPoint(uint256::ONE, 1);
    funding.vout.push_back(CTxOut(COIN, GetScriptForDestination(our_dgb)));
    const CTransactionRef funding_tx = MakeTransactionRef(funding);
    PutInWallet(*wallet, funding_tx);

    CMutableTransaction redeem;
    redeem.SetDigiDollarType(DD_TX_REDEEM);
    redeem.vin.push_back(CTxIn(COutPoint(mint_tx->GetHash(), 0), CScript(), 0xFFFFFFFE));
    redeem.vin.push_back(CTxIn(COutPoint(mint_tx->GetHash(), 1), CScript(), 0xFFFFFFFE));
    redeem.vin.push_back(CTxIn(COutPoint(funding_tx->GetHash(), 0)));
    redeem.vout.push_back(CTxOut(collateral, GetScriptForDestination(collateral_return)));
    redeem.vout.push_back(DDTokenOut(our_dd_change));
    redeem.vout.push_back(RedeemDataOut(200));
    const CTransactionRef redeem_tx = MakeTransactionRef(redeem);
    PutInWallet(*wallet, redeem_tx);

    interfaces::WalletTx wtx = gui.walletModel->wallet().getWalletTx(redeem_tx->GetHash());
    QList<TransactionRecord> rows = TransactionRecord::decomposeTransaction(wtx);
    QVERIFY(!rows.isEmpty());

    for (TransactionRecord& row : rows) {
        const QString text = DetailsPlainText(m_node, gui.walletModel->wallet(), row);
        QVERIFY2(!text.contains(QStringLiteral("Null")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("$2.00")), qPrintable(text));
        QVERIFY2(text.contains(QString::fromStdString(mint_tx->GetHash().ToString())), qPrintable(text));
    }
}
