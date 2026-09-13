// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/ddtransactiontabletests.h>

#include <consensus/digidollar.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <kernel/chainparams.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/csvmodelwriter.h>
#include <qt/digibyteunits.h>
#include <qt/digidollartransactionswidget.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <memory>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStringList>
#include <QTableWidget>
#include <QHeaderView>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextStream>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WalletContext;
using wallet::WalletRescanReserver;

namespace {

//! Height the test wallet believes the chain is at.
constexpr int kWalletTipHeight = 105;
//! Height the test transactions are recorded at, far enough back that the
//! wallet trusts them and the history shows the amounts without brackets.
constexpr int kTxHeight = 101;
//! Fee every test transaction pays, in satoshis.
constexpr CAmount kFee = 100000;

void SyncUpWallet(const std::shared_ptr<wallet::CWallet>& wallet)
{
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    wallet::CWallet::ScanResult result = wallet->ScanForWalletTransactions(
        Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, true, false);
    QCOMPARE(result.status, wallet::CWallet::ScanResult::SUCCESS);
}

std::shared_ptr<wallet::CWallet> SetupWallet(interfaces::Node& node,
                                             TestChain100Setup& test,
                                             const std::string& wallet_name)
{
    std::shared_ptr<wallet::CWallet> wallet = std::make_shared<wallet::CWallet>(
        node.context()->chain.get(), wallet_name, CreateMockableWalletDatabase());
    wallet->LoadWallet();
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet->SetupDescriptorScriptPubKeyMans();

    FlatSigningProvider provider;
    std::string error;
    std::unique_ptr<Descriptor> desc = Parse(
        "combo(" + EncodeSecret(test.coinbaseKey) + ")", provider, error, false);
    assert(desc);
    wallet::WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    if (!wallet->AddWalletDescriptor(w_desc, provider, "", false)) assert(false);
    CTxDestination dest = GetDestinationForKey(test.coinbaseKey.GetPubKey(),
                                               wallet->m_default_address_type);
    wallet->SetAddressBook(dest, "", wallet::AddressPurpose::RECEIVE);
    wallet->SetLastBlockProcessed(kWalletTipHeight,
        WITH_LOCK(node.context()->chainman->GetMutex(),
                  return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    SyncUpWallet(wallet);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

struct MiniGUI {
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;
    std::unique_ptr<const PlatformStyle> platformStyle;

    explicit MiniGUI(interfaces::Node& node) : optionsModel(node)
    {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
        platformStyle.reset(PlatformStyle::instantiate("other"));
    }

    void initModelForWallet(interfaces::Node& node,
                            const std::shared_ptr<wallet::CWallet>& wallet)
    {
        WalletContext& context = *node.walletLoader().context();
        AddWallet(context, wallet);
        walletModel = std::make_unique<WalletModel>(
            interfaces::MakeWallet(context, wallet), *clientModel,
            platformStyle.get());
        RemoveWallet(context, wallet, std::nullopt);
    }
};

//! Version field of a DigiDollar transaction of the given type.
int32_t DDVersion(uint8_t dd_tx_type)
{
    return static_cast<int32_t>((static_cast<uint32_t>(dd_tx_type) << 24) | 0x0770u);
}

//! A pay-to-taproot script. The wallet in these tests does not own any of
//! these, which is also true of a real collateral lock and of DigiDollars
//! sent to somebody else.
CScript TaprootScript(unsigned char fill)
{
    std::vector<unsigned char> program(32, fill);
    return CScript() << OP_1 << program;
}

//! The OP_RETURN output a DigiDollar transaction carries, built the same way
//! the transaction builder builds it.
CScript DDMetadata(uint8_t dd_tx_type, const std::vector<CAmount>& amounts)
{
    CScript script;
    script << OP_RETURN
           << std::vector<unsigned char>{'D', 'D'}
           << CScriptNum(dd_tx_type);
    for (CAmount amount : amounts) {
        script << CScriptNum(amount);
    }
    return script;
}

//! An outpoint that belongs to nobody the wallet knows about. A DigiDollar
//! token input and a locked collateral input both look like this to the
//! wallet, because neither script is in its descriptor.
COutPoint ForeignOutPoint(unsigned char fill)
{
    uint256 hash;
    std::vector<unsigned char> bytes(32, fill);
    hash = uint256(bytes);
    return COutPoint(hash, 0);
}

void RecordTransaction(const std::shared_ptr<wallet::CWallet>& wallet,
                       const CMutableTransaction& tx,
                       interfaces::Node& node)
{
    const uint256 block_hash = WITH_LOCK(node.context()->chainman->GetMutex(),
        return node.context()->chainman->ActiveChain()[kTxHeight]->GetBlockHash());
    LOCK(wallet->cs_wallet);
    wallet->AddToWallet(MakeTransactionRef(tx),
                        wallet::TxStateConfirmed{block_hash, kTxHeight, 1});
}

//! Column whose heading contains the given text, or -1.
int ColumnWithHeading(const QAbstractItemModel& model, const QString& needle)
{
    for (int column = 0; column < model.columnCount(QModelIndex()); ++column) {
        const QString heading =
            model.headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
        if (heading.contains(needle)) return column;
    }
    return -1;
}

//! Row numbers belonging to one transaction, in model order.
QList<int> RowsOfTransaction(const QAbstractItemModel& model, const uint256& txid)
{
    QList<int> rows;
    const QString wanted = QString::fromStdString(txid.ToString());
    for (int row = 0; row < model.rowCount(QModelIndex()); ++row) {
        const QModelIndex index = model.index(row, 0, QModelIndex());
        if (index.data(TransactionTableModel::TxHashRole).toString() == wanted) {
            rows.append(row);
        }
    }
    return rows;
}

//! Keeps a wallet registered for as long as the test needs it. A failed
//! comparison returns from the test function straight away, so removing the
//! wallet has to happen on the way out rather than at the end of the body.
struct RegisteredWallet {
    WalletContext& context;
    std::shared_ptr<wallet::CWallet> wallet;

    RegisteredWallet(WalletContext& c, std::shared_ptr<wallet::CWallet> w)
        : context(c), wallet(std::move(w))
    {
        AddWallet(context, wallet);
    }
    ~RegisteredWallet() { RemoveWallet(context, wallet, std::nullopt); }
};

bool MaybeSkipMacMinimal()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DDTransactionTableTests on mac minimal platform.");
        return true;
    }
#endif
    return false;
}

//! Builds one wallet holding a DigiDollar mint, transfer and redemption, and
//! keeps the three transaction ids so the tests can find their rows.
struct DDHistory {
    uint256 mint_txid;
    uint256 transfer_txid;
    uint256 redeem_txid;
    CAmount collateral{0};
    CAmount spent_input{0};

    void build(const std::shared_ptr<wallet::CWallet>& wallet,
               TestChain100Setup& test,
               interfaces::Node& node)
    {
        // Mint: one DigiByte input of ours pays the locked collateral and the
        // fee, and creates 100.00 DigiDollars in a token output.
        const CTransactionRef& mint_input = test.m_coinbase_txns[0];
        spent_input = mint_input->vout[0].nValue;
        collateral = spent_input - kFee;

        CMutableTransaction mint;
        mint.nVersion = DDVersion(DigiDollar::DD_TX_MINT);
        mint.vin.emplace_back(COutPoint(mint_input->GetHash(), 0));
        mint.vout.emplace_back(collateral, TaprootScript(0x11));
        mint.vout.emplace_back(0, TaprootScript(0x22));
        mint.vout.emplace_back(0, DDMetadata(DigiDollar::DD_TX_MINT,
                                             {10000, 200, 1}));
        mint_txid = mint.GetHash();
        RecordTransaction(wallet, mint, node);

        // Transfer: a DigiDollar token input we cannot see plus one DigiByte
        // input of ours for the fee. 25.00 DigiDollars go to someone else and
        // the leftover DigiByte comes back to us.
        const CTransactionRef& transfer_input = test.m_coinbase_txns[1];
        const CAmount transfer_in = transfer_input->vout[0].nValue;

        CMutableTransaction transfer;
        transfer.nVersion = DDVersion(DigiDollar::DD_TX_TRANSFER);
        transfer.vin.emplace_back(ForeignOutPoint(0x33));
        transfer.vin.emplace_back(COutPoint(transfer_input->GetHash(), 0));
        transfer.vout.emplace_back(0, TaprootScript(0x44));
        transfer.vout.emplace_back(0, DDMetadata(DigiDollar::DD_TX_TRANSFER, {2500}));
        transfer.vout.emplace_back(transfer_in - kFee,
                                   GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
        transfer_txid = transfer.GetHash();
        RecordTransaction(wallet, transfer, node);

        // Redemption: the locked collateral comes back to us, and so does the
        // DigiByte left over from the fee input.
        const CTransactionRef& redeem_input = test.m_coinbase_txns[2];
        const CAmount redeem_in = redeem_input->vout[0].nValue;

        CMutableTransaction redeem;
        redeem.nVersion = DDVersion(DigiDollar::DD_TX_REDEEM);
        redeem.vin.emplace_back(ForeignOutPoint(0x55));
        redeem.vin.emplace_back(COutPoint(redeem_input->GetHash(), 0));
        redeem.vout.emplace_back(collateral,
                                 GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
        redeem.vout.emplace_back(redeem_in - kFee,
                                 GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
        redeem.vout.emplace_back(0, DDMetadata(DigiDollar::DD_TX_REDEEM, {1500}));
        redeem.vout.emplace_back(0, TaprootScript(0x66));
        redeem_txid = redeem.GetHash();
        RecordTransaction(wallet, redeem, node);
    }
};

} // namespace

void DDTransactionTableTests::amountColumnsAreSeparate()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupWallet(m_node, test, "qt-dd-amount-columns");
    DDHistory history;
    history.build(wallet, test, m_node);

    MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    TransactionTableModel* model = mini_gui.walletModel->getTransactionTableModel();
    QVERIFY(model != nullptr);

    // There are two amount columns, not one mixed column.
    const int dgb_column = ColumnWithHeading(*model, "DGB");
    const int dd_column = ColumnWithHeading(*model, "$DD");
    QVERIFY2(dgb_column >= 0, "no DigiByte amount column");
    QVERIFY2(dd_column >= 0, "no DigiDollar amount column");
    QVERIFY2(dgb_column != dd_column,
             qPrintable(QString("DigiByte and DigiDollar amounts share column %1 "
                                "with heading '%2'")
                            .arg(dgb_column)
                            .arg(model->headerData(dgb_column, Qt::Horizontal,
                                                   Qt::DisplayRole).toString())));

    // These three transactions build their rows through the DigiDollar paths,
    // which give the DigiByte and the DigiDollars a row each. No row here puts
    // a number in both columns, and no DigiByte cell anywhere prints dollars.
    for (int row = 0; row < model->rowCount(QModelIndex()); ++row) {
        const QString dgb = model->index(row, dgb_column, QModelIndex()).data(Qt::DisplayRole).toString();
        const QString dd = model->index(row, dd_column, QModelIndex()).data(Qt::DisplayRole).toString();
        QVERIFY2(dgb.isEmpty() || dd.isEmpty(),
                 qPrintable(QString("row %1 shows '%2' and '%3' at the same time")
                                .arg(row).arg(dgb).arg(dd)));
        QVERIFY2(!dgb.contains("$DD"),
                 qPrintable(QString("row %1 printed dollars in the DigiByte column: '%2'")
                                .arg(row).arg(dgb)));
        QVERIFY2(dd.isEmpty() || dd.endsWith("$DD"),
                 qPrintable(QString("row %1 printed '%2' in the DigiDollar column")
                                .arg(row).arg(dd)));
    }

    const DigiByteUnit unit = mini_gui.walletModel->getOptionsModel()->getDisplayUnit();

    // The mint locks collateral in DigiByte and creates DigiDollars. The
    // DigiByte side is two rows, the collateral locked and the fee, and the
    // new money is in the DigiDollar column. The collateral row carries the
    // collateral alone, because that is the figure the owner needs when
    // deciding whether to redeem, and the fee is shown once on its own row.
    // Together the two DigiByte rows account for everything that left the
    // wallet.
    const QList<int> mint_rows = RowsOfTransaction(*model, history.mint_txid);
    QVERIFY2(!mint_rows.isEmpty(), "the mint produced no rows");
    int mint_dgb_rows = 0;
    int mint_dd_rows = 0;
    QStringList mint_dgb_cells;
    for (int row : mint_rows) {
        const QString dgb = model->index(row, dgb_column, QModelIndex()).data(Qt::DisplayRole).toString();
        const QString dd = model->index(row, dd_column, QModelIndex()).data(Qt::DisplayRole).toString();
        if (!dgb.isEmpty()) {
            ++mint_dgb_rows;
            mint_dgb_cells.append(dgb);
        }
        if (!dd.isEmpty()) {
            ++mint_dd_rows;
            QVERIFY2(dd.contains("100.00 $DD"),
                     qPrintable(QString("mint DigiDollar cell reads '%1'").arg(dd)));
        }
    }
    QCOMPARE(mint_dgb_rows, 2);
    QVERIFY2(mint_dd_rows >= 1, "the mint showed no DigiDollar amount");
    const QString collateral_cell = DigiByteUnits::format(unit, -history.collateral, false,
                                                          DigiByteUnits::SeparatorStyle::ALWAYS);
    const QString fee_cell = DigiByteUnits::format(unit, -kFee, false,
                                                   DigiByteUnits::SeparatorStyle::ALWAYS);
    QVERIFY2(mint_dgb_cells.contains(collateral_cell),
             qPrintable(QString("no mint row shows the collateral %1; rows were %2")
                            .arg(collateral_cell).arg(mint_dgb_cells.join(", "))));
    QVERIFY2(mint_dgb_cells.contains(fee_cell),
             qPrintable(QString("no mint row shows the fee %1; rows were %2")
                            .arg(fee_cell).arg(mint_dgb_cells.join(", "))));
    QCOMPARE(history.collateral + kFee, history.spent_input);

    // The transfer sends 25.00 DigiDollars and pays a DigiByte fee. The two
    // belong in different columns.
    const QList<int> transfer_rows = RowsOfTransaction(*model, history.transfer_txid);
    QCOMPARE(transfer_rows.size(), 2);
    QStringList transfer_dgb;
    QStringList transfer_dd;
    for (int row : transfer_rows) {
        const QString dgb = model->index(row, dgb_column, QModelIndex()).data(Qt::DisplayRole).toString();
        const QString dd = model->index(row, dd_column, QModelIndex()).data(Qt::DisplayRole).toString();
        if (!dgb.isEmpty()) transfer_dgb.append(dgb);
        if (!dd.isEmpty()) transfer_dd.append(dd);
    }
    QCOMPARE(transfer_dgb.size(), 1);
    QCOMPARE(transfer_dd.size(), 1);
    QCOMPARE(transfer_dgb.at(0), DigiByteUnits::format(unit, -kFee, false,
                                                       DigiByteUnits::SeparatorStyle::ALWAYS));
    QCOMPARE(transfer_dd.at(0), QString("-25.00 $DD"));

    // The redemption returns locked DigiByte. That is a DigiByte amount.
    const QList<int> redeem_rows = RowsOfTransaction(*model, history.redeem_txid);
    QVERIFY2(!redeem_rows.isEmpty(), "the redemption produced no rows");
    bool saw_collateral_return = false;
    for (int row : redeem_rows) {
        const QString dgb = model->index(row, dgb_column, QModelIndex()).data(Qt::DisplayRole).toString();
        if (dgb == DigiByteUnits::format(unit, history.collateral, false,
                                         DigiByteUnits::SeparatorStyle::ALWAYS)) {
            saw_collateral_return = true;
            QVERIFY2(model->index(row, dd_column, QModelIndex()).data(Qt::DisplayRole).toString().isEmpty(),
                     "the returned collateral was also counted as DigiDollars");
        }
    }
    QVERIFY2(saw_collateral_return, "the returned collateral was not shown");

    // Sorting uses each column's own number, so a DigiDollar row does not
    // pretend to hold a DigiByte amount.
    for (int row : transfer_rows) {
        const QString dd = model->index(row, dd_column, QModelIndex()).data(Qt::DisplayRole).toString();
        if (dd.isEmpty()) continue;
        QCOMPARE(model->index(row, dgb_column, QModelIndex()).data(Qt::EditRole).toLongLong(), qint64(0));
        QCOMPARE(model->index(row, dd_column, QModelIndex()).data(Qt::EditRole).toLongLong(), qint64(-2500));
    }
}

void DDTransactionTableTests::sendFromOwnTokenShowsTheFeeOnItsOwnRow()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupWallet(m_node, test, "qt-dd-own-token-send");

    // A send where every input belongs to the wallet. This used to build a
    // single row and hang the DigiByte fee on the row that shows dollars, so
    // the user could not tell that the DigiByte figure was the fee. It now
    // builds two rows: the dollars that left, and the fee. Each column must
    // still show its own figure and nothing else.
    const CTransactionRef& fee_input = test.m_coinbase_txns[0];
    const CAmount input_value = fee_input->vout[0].nValue;

    CMutableTransaction send;
    send.nVersion = DDVersion(DigiDollar::DD_TX_TRANSFER);
    send.vin.emplace_back(COutPoint(fee_input->GetHash(), 0));
    send.vout.emplace_back(0, TaprootScript(0x77));
    send.vout.emplace_back(0, DDMetadata(DigiDollar::DD_TX_TRANSFER, {2500}));
    send.vout.emplace_back(input_value - kFee,
                           GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    const uint256 send_txid = send.GetHash();
    RecordTransaction(wallet, send, m_node);

    MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    TransactionTableModel* model = mini_gui.walletModel->getTransactionTableModel();
    QVERIFY(model != nullptr);

    const int dgb_column = ColumnWithHeading(*model, "DGB");
    const int dd_column = ColumnWithHeading(*model, "$DD");
    QVERIFY(dgb_column >= 0 && dd_column >= 0 && dgb_column != dd_column);

    const QList<int> rows = RowsOfTransaction(*model, send_txid);
    QCOMPARE(rows.size(), 2);

    const DigiByteUnit unit = mini_gui.walletModel->getOptionsModel()->getDisplayUnit();
    const QString fee_text = DigiByteUnits::format(unit, -kFee, false,
                                                   DigiByteUnits::SeparatorStyle::ALWAYS);
    bool saw_dollar_row = false;
    bool saw_fee_row = false;
    for (int row : rows) {
        const QString dgb = model->index(row, dgb_column, QModelIndex()).data(Qt::DisplayRole).toString();
        const QString dd = model->index(row, dd_column, QModelIndex()).data(Qt::DisplayRole).toString();
        QVERIFY2(!dgb.contains("$DD"), "the DigiByte column printed a dollar amount");

        if (dd == QString("-25.00 $DD")) {
            saw_dollar_row = true;
            // No DigiByte at all on the row that shows dollars.
            QVERIFY2(dgb.isEmpty(), "the row showing dollars also showed a DigiByte amount");
            QCOMPARE(model->index(row, dgb_column, QModelIndex()).data(Qt::EditRole).toLongLong(),
                     qint64(0));
            QCOMPARE(model->index(row, dd_column, QModelIndex()).data(Qt::EditRole).toLongLong(),
                     qint64(-2500));
        } else if (dgb == fee_text) {
            saw_fee_row = true;
            // The fee row carries DigiByte and no dollars.
            QVERIFY2(dd.isEmpty(), "the fee row also showed a dollar amount");
            QCOMPARE(model->index(row, dgb_column, QModelIndex()).data(Qt::EditRole).toLongLong(),
                     qint64(-kFee));
            QCOMPARE(model->index(row, dd_column, QModelIndex()).data(Qt::EditRole).toLongLong(),
                     qint64(0));
        }
    }
    QVERIFY2(saw_dollar_row, "the dollars that left were not shown");
    QVERIFY2(saw_fee_row, "the fee was not shown on a row of its own");
}

void DDTransactionTableTests::csvExportHasSeparateAmountColumns()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupWallet(m_node, test, "qt-dd-csv-columns");
    DDHistory history;
    history.build(wallet, test, m_node);

    MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    TransactionTableModel* model = mini_gui.walletModel->getTransactionTableModel();
    QVERIFY(model != nullptr);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("transactions.csv");

    {
        CSVModelWriter writer(path);
        writer.setModel(model);
        // The same columns the Export button writes.
        TransactionView::addExportColumns(
            writer, mini_gui.walletModel->getOptionsModel()->getDisplayUnit(),
            /*have_watch_only=*/false);
        QVERIFY(writer.write());
    }

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream in(&file);
    const QStringList lines = in.readAll().split('\n', Qt::SkipEmptyParts);
    file.close();
    QVERIFY(!lines.isEmpty());

    const QString header = lines.first();
    QVERIFY2(header.contains("(DGB)"),
             qPrintable(QString("CSV header has no DigiByte amount column: %1").arg(header)));
    QVERIFY2(header.contains("$DD"),
             qPrintable(QString("CSV header has no DigiDollar amount column: %1").arg(header)));

    // Every data line must carry both fields, and no field may hold both a
    // DigiByte number and a DigiDollar number.
    bool saw_dd_value = false;
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList fields = lines.at(i).split("\",\"");
        for (const QString& field : fields) {
            QVERIFY2(!(field.contains("$DD") && field.contains(".") && field.count('.') > 1),
                     qPrintable(QString("CSV field mixes amounts: %1").arg(field)));
        }
        if (lines.at(i).contains("$DD")) saw_dd_value = true;
    }
    QVERIFY2(saw_dd_value, "no DigiDollar amount reached the CSV file");
}

void DDTransactionTableTests::digiDollarRowTypesAreLabelled()
{
    if (MaybeSkipMacMinimal()) return;

    // A row the wallet trusts, so the amounts print without brackets.
    auto make_row = [](TransactionRecord::Type type, CAmount debit, CAmount credit,
                       CAmount dd_cents) {
        TransactionRecord rec(uint256::ONE, 1600000000);
        rec.type = type;
        rec.debit = debit;
        rec.credit = credit;
        rec.ddAmount = dd_cents;
        rec.status.countsForBalance = true;
        return rec;
    };

    const DigiByteUnit unit = DigiByteUnit::DGB;

    // Minted DigiDollars: the new money only, no DigiByte. The DigiByte side
    // of a mint is the collateral row and the fee row.
    const TransactionRecord mint = make_row(TransactionRecord::DDMint, 0, 0, 10000);
    QCOMPARE(TransactionTableModel::formatTxType(&mint), QString("DigiDollar Mint"));
    QCOMPARE(TransactionTableModel::formatAmountDGB(&mint, unit), QString());
    QCOMPARE(TransactionTableModel::formatAmountDD(&mint), QString("+100.00 $DD"));
    QCOMPARE(TransactionTableModel::txTypeIconPath(&mint), QString(":/icons/tx_input"));

    // Returned DigiDollar change uses the same words as the DigiDollar tab.
    const TransactionRecord change = make_row(TransactionRecord::DDChangeReturned, 0, 0, 1500);
    QCOMPARE(TransactionTableModel::formatTxType(&change), DigiDollarLabels::ChangeReturned());
    QCOMPARE(TransactionTableModel::formatTxType(&change), QString("DigiDollar change returned"));
    QCOMPARE(TransactionTableModel::formatAmountDGB(&change, unit), QString());
    QCOMPARE(TransactionTableModel::formatAmountDD(&change), QString("+15.00 $DD"));
    QCOMPARE(TransactionTableModel::txTypeIconPath(&change), QString(":/icons/tx_input"));

    // A mint pays a fee too, so the fee row does not say transfer.
    const TransactionRecord fee = make_row(TransactionRecord::DDSendFee, -100000, 0, 0);
    QCOMPARE(TransactionTableModel::formatTxType(&fee), QString("DigiDollar Fee"));
    QVERIFY2(!TransactionTableModel::formatTxType(&fee).contains("Transfer"),
             "the fee row still says transfer");
    QCOMPARE(TransactionTableModel::formatAmountDGB(&fee, unit),
             DigiByteUnits::format(unit, -100000, false, DigiByteUnits::SeparatorStyle::STANDARD));
    QCOMPARE(TransactionTableModel::formatAmountDD(&fee), QString());

    // The locked collateral is DigiByte, and it stays in the DigiByte column.
    const TransactionRecord collateral =
        make_row(TransactionRecord::DDTimeLockCollateral, -300 * COIN, 0, 0);
    QCOMPARE(TransactionTableModel::formatAmountDD(&collateral), QString());
    QVERIFY(!TransactionTableModel::formatAmountDGB(&collateral, unit).isEmpty());

    // No row type shows an empty name, which is what sends a row to the
    // catch-all branch and prints "(n/a)" beside it.
    const TransactionRecord::Type all_types[] = {
        TransactionRecord::Generated,
        TransactionRecord::SendToAddress,
        TransactionRecord::SendToOther,
        TransactionRecord::RecvWithAddress,
        TransactionRecord::RecvFromOther,
        TransactionRecord::DDTimeLockCollateral,
        TransactionRecord::DDCollateralReturn,
        TransactionRecord::DDSend,
        TransactionRecord::DDRecv,
        TransactionRecord::DDSendFee,
        TransactionRecord::DDMint,
        TransactionRecord::DDChangeReturned,
    };
    for (TransactionRecord::Type type : all_types) {
        const TransactionRecord row = make_row(type, -1, 0, 0);
        QVERIFY2(!TransactionTableModel::formatTxType(&row).isEmpty(),
                 qPrintable(QString("row type %1 has no name").arg(int(type))));
    }
}

void DDTransactionTableTests::theSingleAmountIsTheOneTheRowHas()
{
    // The pop-up that announces a new transaction, and the short list on the
    // main overview, have room for one figure per row. Both used to read the
    // DigiByte figure. A DigiDollar row holds no DigiByte, so a payment of
    // $100 was announced as 0.00000000 DGB. Both now read the figure the row
    // actually has.
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupWallet(m_node, test, "qt-dd-single-amount");
    DDHistory history;
    history.build(wallet, test, m_node);

    MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    TransactionTableModel* model = mini_gui.walletModel->getTransactionTableModel();
    QVERIFY(model != nullptr);

    const int dgb_column = ColumnWithHeading(*model, "DGB");
    const int dd_column = ColumnWithHeading(*model, "$DD");
    QVERIFY(dgb_column >= 0 && dd_column >= 0);

    bool saw_a_dollar_row = false;
    bool saw_a_digibyte_row = false;
    for (int row = 0; row < model->rowCount(QModelIndex()); ++row) {
        const QModelIndex idx = model->index(row, 0, QModelIndex());
        const QString single = idx.data(TransactionTableModel::FormattedSingleAmountRole).toString();
        const qint64 single_number = idx.data(TransactionTableModel::SingleAmountRole).toLongLong();
        const qint64 dgb_number = idx.data(TransactionTableModel::AmountRole).toLongLong();
        const qint64 dd_number = idx.data(TransactionTableModel::AmountDDRole).toLongLong();

        QVERIFY2(!single.isEmpty(), "every row must have one figure to show");
        if (dgb_number == 0 && dd_number != 0) {
            saw_a_dollar_row = true;
            // The dollar figure, not a DigiByte zero.
            QCOMPARE(single, idx.data(TransactionTableModel::FormattedAmountDDRole).toString());
            QVERIFY2(single.contains("$DD"), "a DigiDollar row must show the dollar figure");
            QCOMPARE(single_number, dd_number);
        } else {
            saw_a_digibyte_row = true;
            QVERIFY2(!single.contains("$DD"), "a DigiByte row must not show a dollar figure");
            QCOMPARE(single_number, dgb_number);
        }
    }
    QVERIFY2(saw_a_dollar_row, "the history had no DigiDollar row to check");
    QVERIFY2(saw_a_digibyte_row, "the history had no DigiByte row to check");
}

void DDTransactionTableTests::transactionViewAppliesAndRemembersColumnWidths()
{
    // The transaction view saves its column widths when it closes and puts them
    // back when it opens. Putting them back used to happen before the view had
    // a model. A header with no model has no columns, so every width call did
    // nothing and the saved layout was thrown away when the model arrived.
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupWallet(m_node, test, "qt-dd-column-widths");

    MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    QVERIFY(mini_gui.walletModel != nullptr);

    // A width nobody would arrive at by accident.
    const int chosen_width = 173;

    QSettings settings;
    const QVariant saved_before = settings.value("TransactionViewHeaderState");
    settings.remove("TransactionViewHeaderState");
    settings.sync();

    // With nothing saved, the window must open with the widths the program
    // chooses for each column.
    {
        TransactionView view(mini_gui.platformStyle.get());
        view.setModel(mini_gui.walletModel.get());
        QTableView* table = view.findChild<QTableView*>();
        QVERIFY(table != nullptr);
        QVERIFY2(table->horizontalHeader()->count() > 0,
                 "the view has a model, so its header must have columns");
        QCOMPARE(table->columnWidth(TransactionTableModel::Status),
                 int(TransactionView::STATUS_COLUMN_WIDTH));
        QCOMPARE(table->columnWidth(TransactionTableModel::Date),
                 int(TransactionView::DATE_COLUMN_WIDTH));
        QCOMPARE(table->columnWidth(TransactionTableModel::Type),
                 int(TransactionView::TYPE_COLUMN_WIDTH));
        QCOMPARE(table->columnWidth(TransactionTableModel::AmountDD),
                 int(TransactionView::AMOUNT_DD_COLUMN_WIDTH));

        // Now widen a column, as a user would.
        table->setColumnWidth(TransactionTableModel::Date, chosen_width);
        QCOMPARE(table->columnWidth(TransactionTableModel::Date), chosen_width);
    }
    // Closing the view saved the layout. Opening another one must bring it back.
    {
        TransactionView view(mini_gui.platformStyle.get());
        view.setModel(mini_gui.walletModel.get());
        QTableView* table = view.findChild<QTableView*>();
        QVERIFY(table != nullptr);
        QCOMPARE(table->columnWidth(TransactionTableModel::Date), chosen_width);
        // The DigiDollar column is still on screen with a width of its own.
        QVERIFY2(!table->isColumnHidden(TransactionTableModel::AmountDD),
                 "the DigiDollar amount column was hidden");
        QVERIFY(table->columnWidth(TransactionTableModel::AmountDD) > 0);
    }

    if (saved_before.isValid()) {
        settings.setValue("TransactionViewHeaderState", saved_before);
    } else {
        settings.remove("TransactionViewHeaderState");
    }
}

void DDTransactionTableTests::changeReturnedLabelExplainsItself()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupWallet(m_node, test, "qt-dd-change-returned");
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    DDTransaction tx;
    tx.txid = "d000000000000000000000000000000000000000000000000000000000000001";
    tx.amount = 1500;
    tx.timestamp = GetTime();
    tx.confirmations = 3;
    tx.incoming = true;
    tx.address = "TDchangereturnedaddress";
    tx.category = "redeem_change";
    tx.lock_tier = -1;
    tx.fee = 0;
    tx.abandoned = false;
    dd_wallet->AddMockTransaction(tx);

    MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    RegisteredWallet registered(*m_node.walletLoader().context(), wallet);

    DigiDollarTransactionsWidget widget;
    widget.setWalletModel(mini_gui.walletModel.get());
    widget.setClientModel(mini_gui.clientModel.get());
    widget.show();
    QCoreApplication::processEvents();
    widget.updateView();
    QCoreApplication::processEvents();

    QComboBox* type_filter = widget.findChild<QComboBox*>();
    QVERIFY(type_filter != nullptr);
    const int filter_index = type_filter->findData(QString("redeem_change"));
    QVERIFY2(filter_index >= 0, "no filter entry for returned DigiDollar change");
    QCOMPARE(type_filter->itemText(filter_index), QString("DigiDollar change returned"));

    QTableWidget* table = widget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QTableWidgetItem* type_item = table->item(0, 1);
    QVERIFY(type_item != nullptr);
    QCOMPARE(type_item->text(), QString("DigiDollar change returned"));

    const QString explanation = type_item->toolTip();
    QVERIFY2(explanation.contains("comes back to you"),
             qPrintable(QString("returned change is not explained: '%1'").arg(explanation)));
    QVERIFY2(explanation.contains("closed in full"),
             qPrintable(QString("returned change does not say the vault is closed in full: '%1'")
                            .arg(explanation)));
}
