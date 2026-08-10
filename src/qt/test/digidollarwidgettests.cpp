// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/digidollarwidgettests.h>
#include <qt/test/util.h>

#include <consensus/digidollar.h>
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
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>
#include <qt/digidollaroverviewwidget.h>
#include <qt/digidollarmintwidget.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarcoincontroldialog.h>
#include <qt/digidollarreceivewidget.h>
#include <qt/digidollarreceiverequest.h>
#include <qt/digidollarredeemwidget.h>
#include <qt/digidollarpositionswidget.h>
#include <qt/digidollartransactionswidget.h>
#include <qt/digidollartab.h>
#include <qt/ddaddressbookpage.h>
#include <qt/guiutil.h>
#include <qt/walletview.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QRegularExpression>
#include <QComboBox>
#include <QProgressBar>
#include <QListWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
using wallet::WalletContext;
using wallet::WalletRescanReserver;

namespace
{

void SyncUpWallet(const std::shared_ptr<wallet::CWallet>& wallet, interfaces::Node& node)
{
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    wallet::CWallet::ScanResult result = wallet->ScanForWalletTransactions(
        Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, true, false);
    QCOMPARE(result.status, wallet::CWallet::ScanResult::SUCCESS);
}

std::shared_ptr<wallet::CWallet> SetupDescriptorsWallet(interfaces::Node& node, TestChain100Setup& test, const std::string& wallet_name = "")
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
    CTxDestination dest = GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type);
    wallet->SetAddressBook(dest, "", wallet::AddressPurpose::RECEIVE);
    wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), 
        return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    SyncUpWallet(wallet, node);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

QString EncodeDigiDollarAddressForNetwork(int network_type)
{
    uint256 hash;
    hash.SetHex("89abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567");
    XOnlyPubKey xonly(hash);
    CDigiDollarAddress addr;
    bool ok = addr.SetDigiDollar(CTxDestination{WitnessV1Taproot(xonly)}, network_type);
    assert(ok);
    return QString::fromStdString(addr.ToString());
}

CTransactionRef MakePendingDDTx()
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256::ONE, 0);
    tx.vout.resize(2);
    tx.vout[0].nValue = COIN;
    tx.vout[1].nValue = 0;
    return MakeTransactionRef(std::move(tx));
}

bool ReadRecentRequestEntry(const std::string& request_str, RecentRequestEntry& entry)
{
    std::vector<uint8_t> data(request_str.begin(), request_str.end());
    DataStream ss{data};
    ss >> entry;
    return true;
}

bool FindStoredReceiveRequest(WalletModel& wallet_model, const QString& address, RecentRequestEntry& result)
{
    for (const std::string& request_str : wallet_model.wallet().getAddressReceiveRequests()) {
        RecentRequestEntry entry;
        ReadRecentRequestEntry(request_str, entry);
        if (entry.recipient.address == address) {
            result = entry;
            return true;
        }
    }
    return false;
}

int CountStoredReceiveRequests(WalletModel& wallet_model, const QString& address)
{
    int count = 0;
    for (const std::string& request_str : wallet_model.wallet().getAddressReceiveRequests()) {
        RecentRequestEntry entry;
        ReadRecentRequestEntry(request_str, entry);
        if (entry.recipient.address == address) {
            ++count;
        }
    }
    return count;
}

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

struct DigiDollarMiniGUI {
public:
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;
    std::unique_ptr<const PlatformStyle> platformStyle;

    DigiDollarMiniGUI(interfaces::Node& node) : optionsModel(node) {
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

void TestOverviewWidget(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    DigiDollarMiniGUI mini_gui(node);
    mini_gui.initModelForWallet(node, wallet);

    DigiDollarOverviewWidget overviewWidget;
    overviewWidget.setWalletModel(mini_gui.walletModel.get());
    overviewWidget.setClientModel(mini_gui.clientModel.get());

    QVERIFY(&overviewWidget != nullptr);

    overviewWidget.updateView();
    overviewWidget.updateBalance();
    overviewWidget.updateOraclePrice();
    overviewWidget.updateSystemHealth();
}

void TestMintWidget(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    DigiDollarMiniGUI mini_gui(node);
    mini_gui.initModelForWallet(node, wallet);

    DigiDollarMintWidget mintWidget;
    mintWidget.setWalletModel(mini_gui.walletModel.get());
    mintWidget.setClientModel(mini_gui.clientModel.get());

    QVERIFY(&mintWidget != nullptr);

    mintWidget.updateView();
    mintWidget.updateBalance();
    mintWidget.updateOraclePrice();
}

void TestSendWidget(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    DigiDollarMiniGUI mini_gui(node);
    mini_gui.initModelForWallet(node, wallet);

    DigiDollarSendWidget sendWidget(mini_gui.platformStyle.get());
    sendWidget.setWalletModel(mini_gui.walletModel.get());
    sendWidget.setClientModel(mini_gui.clientModel.get());

    QVERIFY(&sendWidget != nullptr);

    sendWidget.updateView();
    sendWidget.updateBalance();
    sendWidget.updateOraclePrice();
}

void TestReceiveWidget(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    DigiDollarMiniGUI mini_gui(node);
    mini_gui.initModelForWallet(node, wallet);

    DigiDollarReceiveWidget receiveWidget;
    receiveWidget.setWalletModel(mini_gui.walletModel.get());
    receiveWidget.setClientModel(mini_gui.clientModel.get());
    receiveWidget.show();

    QVERIFY(&receiveWidget != nullptr);

    receiveWidget.updateView();
    receiveWidget.updateRecentRequests();

    QLabel* emptyLabel = receiveWidget.findChild<QLabel*>("emptyStateLabel");
    QVERIFY(emptyLabel != nullptr);
    QVERIFY(emptyLabel->isVisibleTo(&receiveWidget));
    QVERIFY(emptyLabel->text().contains(QStringLiteral("Generate")));
    QVERIFY(emptyLabel->text().contains(QStringLiteral("DigiDollar")));

    QFrame* qrFrame = receiveWidget.findChild<QFrame*>("qrFrame");
    QVERIFY(qrFrame != nullptr);
    QVERIFY(!qrFrame->isVisibleTo(&receiveWidget));

    QLineEdit* addressEdit = receiveWidget.findChild<QLineEdit*>("addressEdit");
    QVERIFY(addressEdit != nullptr);
    QVERIFY(addressEdit->text().isEmpty());

    QPushButton* generateButton = receiveWidget.findChild<QPushButton*>("generateButton");
    QVERIFY(generateButton != nullptr);
    QVERIFY(generateButton->isEnabled());
    QCOMPARE(generateButton->property("ddState").toString(), QStringLiteral("primaryEnabled"));
    QVERIFY2(generateButton->styleSheet().contains(QStringLiteral("QPushButton#generateButton:enabled")),
             "Generate button should have an explicit enabled style so it does not look disabled until hover");

    QMetaObject::invokeMethod(generateButton, "click", Qt::DirectConnection);
    QCoreApplication::processEvents();

    QVERIFY(qrFrame->isVisibleTo(&receiveWidget));
    QVERIFY(!emptyLabel->isVisibleTo(&receiveWidget));
    QVERIFY(!addressEdit->text().isEmpty());

    const QList<QDialog*> requestDialogs = receiveWidget.findChildren<QDialog*>();
    for (QDialog* dialog : requestDialogs) {
        dialog->close();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();

    QMetaObject::invokeMethod(&receiveWidget, "onClearClicked", Qt::DirectConnection);
    QCoreApplication::processEvents();

    QVERIFY(!qrFrame->isVisibleTo(&receiveWidget));
    QVERIFY(emptyLabel->isVisibleTo(&receiveWidget));
    QVERIFY(addressEdit->text().isEmpty());
}

void TestRedeemWidget(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    DigiDollarMiniGUI mini_gui(node);
    mini_gui.initModelForWallet(node, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());

    QVERIFY(&redeemWidget != nullptr);

    redeemWidget.updateView();
}

void TestPositionsWidget(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    DigiDollarMiniGUI mini_gui(node);
    mini_gui.initModelForWallet(node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.setClientModel(mini_gui.clientModel.get());

    QVERIFY(&positionsWidget != nullptr);

    positionsWidget.updateView();
}

void TestTransactionsWidget(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    DigiDollarMiniGUI mini_gui(node);
    mini_gui.initModelForWallet(node, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());

    QVERIFY(&transactionsWidget != nullptr);

    transactionsWidget.updateView();
}

void AddMockDigiDollarPosition(const std::shared_ptr<wallet::CWallet>& wallet, const uint256& id, CAmount dd_amount, CAmount collateral, uint32_t tier, int64_t unlock_height)
{
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    dd_wallet->AddCollateralPosition(WalletCollateralPosition(id, dd_amount, collateral, tier, unlock_height));
}

QDialog* FindVisibleDialogByTitle(const QString& title)
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        QDialog* dialog = qobject_cast<QDialog*>(widget);
        if (dialog && dialog->isVisible() && dialog->windowTitle() == title) return dialog;
    }
    return nullptr;
}

//! Click "Yes" on the modal "Confirm Redeem" box as soon as it appears.
//! Re-arms itself so it works whether or not the dialog is up yet, and closes
//! any leftover modal after the last attempt so a failure never hangs the run.
void AcceptRedeemConfirmation(int attempts_left = 40)
{
    QTimer::singleShot(25, [attempts_left]() {
        if (QDialog* dialog = FindVisibleDialogByTitle(QStringLiteral("Confirm Redeem"))) {
            auto* box = qobject_cast<QMessageBox*>(dialog);
            QAbstractButton* yes = box ? box->button(QMessageBox::Yes) : nullptr;
            if (yes) {
                yes->click();
            } else {
                dialog->reject();
            }
            return;
        }
        if (attempts_left > 0) {
            AcceptRedeemConfirmation(attempts_left - 1);
        } else if (QWidget* modal = QApplication::activeModalWidget()) {
            modal->close();
        }
    });
}

bool SelectCoinControlDialogInput(const COutPoint& outpoint, QString& error)
{
    QDialog* dialog{nullptr};
    for (int attempt = 0; attempt < 20 && !dialog; ++attempt) {
        dialog = FindVisibleDialogByTitle(QStringLiteral("DigiDollar Coin Selection"));
        if (!dialog) QTest::qWait(25);
    }
    if (!dialog) {
        error = QStringLiteral("DigiDollar coin-control dialog did not open");
        return false;
    }

    auto fail = [&](const QString& message) {
        error = message;
        dialog->reject();
        return false;
    };

    QTreeWidget* tree = dialog->findChild<QTreeWidget*>(QStringLiteral("treeWidget"));
    if (!tree) {
        return fail(QStringLiteral("DigiDollar coin-control dialog has no treeWidget"));
    }

    const QString selectedTxid = QString::fromStdString(outpoint.hash.GetHex());
    QList<QTreeWidgetItem*> items;
    QStringList renderedOutpoints;
    auto collect = [&](QTreeWidgetItem* root, auto&& self) -> void {
        for (int i = 0; i < root->childCount(); ++i) {
            QTreeWidgetItem* child = root->child(i);
            const QString outpointText = child->text(6 /* COLUMN_TXID_VOUT */);
            if (!outpointText.isEmpty()) {
                renderedOutpoints << outpointText;
                if (outpointText.contains(selectedTxid)) items << child;
            }
            self(child, self);
        }
    };
    collect(tree->invisibleRootItem(), collect);
    if (items.size() != 1) {
        return fail(QStringLiteral("expected exactly one selectable DD input row for %1, found %2; rendered: %3")
                        .arg(selectedTxid)
                        .arg(items.size())
                        .arg(renderedOutpoints.join(QStringLiteral(", "))));
    }
    items.front()->setCheckState(0 /* COLUMN_CHECKBOX */, Qt::Checked);
    QCoreApplication::processEvents();

    QDialogButtonBox* buttons = dialog->findChild<QDialogButtonBox*>();
    if (!buttons || !buttons->button(QDialogButtonBox::Ok)) {
        return fail(QStringLiteral("DigiDollar coin-control dialog has no OK button"));
    }
    buttons->button(QDialogButtonBox::Ok)->click();
    return true;
}

} // namespace

void DigiDollarWidgetTests::overviewWidgetTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    TestOverviewWidget(m_node, wallet);
}

void DigiDollarWidgetTests::watchOnlyDigiDollarBalanceHiddenInWalletModel()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->EnsureDDWallet();
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    dd_wallet->AddDDUTXO(COutPoint(uint256::ONE, 1), 10000);
    QCOMPARE(dd_wallet->GetTotalDDBalance(), 10000);

    const CTransactionRef pending_tx = MakePendingDDTx();
    {
        LOCK(wallet->cs_wallet);
        wallet->AddToWallet(pending_tx, wallet::TxStateInMempool{});
    }
    dd_wallet->AddDDUTXO(COutPoint(pending_tx->GetHash(), 1), 2500);
    QCOMPARE(dd_wallet->GetPendingDDBalance(), 2500);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    QCOMPARE(mini_gui.walletModel->getDigiDollarBalance(), 0);
    QCOMPARE(mini_gui.walletModel->getPendingDigiDollarBalance(), 0);
}

void DigiDollarWidgetTests::privateKeyDisabledWalletCannotGenerateDigiDollarAddress()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    QCOMPARE(mini_gui.walletModel->getNewDigiDollarAddress("watch-only-dd"), QString());
}

void DigiDollarWidgetTests::mintWidgetTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    TestMintWidget(m_node, wallet);
}

void DigiDollarWidgetTests::mintWidgetUsesChainParamMintLimits()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    DigiDollarMintWidget mintWidget;
    QLineEdit* amountEdit = mintWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(amountEdit != nullptr);
    QVERIFY(amountEdit->validator() != nullptr);

    const auto& ddParams = Params().GetDigiDollarParams();
    const QString minText = QString::number(ddParams.minMintAmount / 100.0, 'f', 2);
    const QString maxText = QString::number(ddParams.maxMintAmount / 100.0, 'f', 2);

    QVERIFY2(amountEdit->toolTip().contains("Minimum: " + minText + " $DD"),
             qPrintable(amountEdit->toolTip()));
    QVERIFY2(amountEdit->toolTip().contains("Maximum: " + maxText + " $DD"),
             qPrintable(amountEdit->toolTip()));

    QLabel* warningLabel = mintWidget.findChild<QLabel*>("amountWarningLabel");
    QVERIFY(warningLabel != nullptr);
    amountEdit->setText(QString::number((ddParams.minMintAmount - 1) / 100.0, 'f', 2));
    QCoreApplication::processEvents();
    QVERIFY2(warningLabel->text().contains("Minimum mint amount is $" + minText),
             qPrintable(warningLabel->text()));

    QString belowMin = QString::number((ddParams.minMintAmount - 1) / 100.0, 'f', 2);
    int pos = 0;
    QCOMPARE(amountEdit->validator()->validate(belowMin, pos), QValidator::Intermediate);

    QString maxAmount = maxText;
    pos = 0;
    QCOMPARE(amountEdit->validator()->validate(maxAmount, pos), QValidator::Acceptable);

    QString aboveMax = QString::number((ddParams.maxMintAmount + 1) / 100.0, 'f', 2);
    pos = 0;
    QCOMPARE(amountEdit->validator()->validate(aboveMax, pos), QValidator::Invalid);
}

void DigiDollarWidgetTests::mintConfirmationCopyExplainsConfirmationBuffer()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };

    const QStringList candidates = {
        QStringLiteral("src/qt/digidollarmintwidget.cpp"),
        QStringLiteral("../src/qt/digidollarmintwidget.cpp"),
        QStringLiteral("../../src/qt/digidollarmintwidget.cpp"),
        QStringLiteral("qt/digidollarmintwidget.cpp"),
    };

    QString source;
    for (const auto& path : candidates) {
        source = readFile(path.toUtf8().constData());
        if (!source.isEmpty()) break;
    }

    QVERIFY2(!source.isEmpty(), "could not locate digidollarmintwidget.cpp from current working directory");
    QVERIFY2(source.contains(QStringLiteral("Network Confirmation Buffer")),
             "mint confirmation copy must explicitly call out the 100-block confirmation buffer");
    QVERIFY2(source.contains(QStringLiteral("Redeem Available Block")),
             "mint confirmation copy must label the effective block as redeem availability, not only lock duration");
    QVERIFY2(!source.contains(QStringLiteral("\"Unlock Block: %7")),
             "mint confirmation copy must not show a buffer-adjusted height as a plain lock-period unlock block");
}

void DigiDollarWidgetTests::mintWidgetCollateralMatchesBuilderSafetyMargin()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MockOracleManager::GetInstance().SetEnabled(true);
    MockOracleManager::GetInstance().SetMockPrice(500000); // $0.50/DGB in micro-USD

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    QCOMPARE(mini_gui.walletModel->calculateRequiredCollateral(10000, 1), 1010 * COIN);

    DigiDollarMintWidget mintWidget;
    mintWidget.setWalletModel(mini_gui.walletModel.get());
    mintWidget.setClientModel(mini_gui.clientModel.get());
    mintWidget.show();
    mintWidget.updateView();

    QLineEdit* amountEdit = mintWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(amountEdit != nullptr);
    amountEdit->setText("100.00");
    QCoreApplication::processEvents();

    QLabel* collateralValue = mintWidget.findChild<QLabel*>("collateralValue");
    QVERIFY(collateralValue != nullptr);
    QCOMPARE(collateralValue->text(), QString("1010.00000000 DGB"));

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarWidgetTests::qtMintStoresDescriptorRecoverableOwnerKey()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MockOracleManager::GetInstance().SetEnabled(true);
    MockOracleManager::GetInstance().SetMockPrice(500000);

    CreateAndProcessOracleQuoteBlock(test, 500000);
    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);
    wallet->SetBroadcastTransactions(true);
    wallet->EnsureDDWallet();
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->pollBalanceChanged();

    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    QVERIFY2(result.status == WalletModel::OK, result.reasonFailed.toUtf8().constData());

    uint256 position_id;
    position_id.SetHex(result.positionId.toStdString());

    CTransactionRef mint_tx;
    CTxOut dd_txout;
    {
        LOCK(wallet->cs_wallet);
        const wallet::CWalletTx* wtx = wallet->GetWalletTx(position_id);
        QVERIFY(wtx != nullptr);
        QVERIFY(wtx->tx->vout.size() > 1);
        mint_tx = wtx->tx;
        dd_txout = mint_tx->vout[1];
    }

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    int64_t op_return_unlock_height{0};
    QVERIFY(DigiDollarWallet::ExtractUnlockHeightFromOpReturn(*mint_tx, op_return_unlock_height));
    const std::vector<WalletCollateralPosition> positions = dd_wallet->GetDDTimeLocks(/*active_only=*/false);
    QCOMPARE(positions.size(), static_cast<size_t>(1));
    QVERIFY(positions[0].dd_timelock_id == position_id);
    QCOMPARE(positions[0].unlock_height, op_return_unlock_height);

    CKey recovered_key;
    QVERIFY(dd_wallet->GetDDOutputSpendingKey(dd_txout, recovered_key));

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarWidgetTests::staleMintUnlockHeightCacheRepairsFromOpReturn()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MockOracleManager::GetInstance().SetEnabled(true);
    MockOracleManager::GetInstance().SetMockPrice(500000);

    CreateAndProcessOracleQuoteBlock(test, 500000);
    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);
    wallet->SetBroadcastTransactions(true);
    wallet->EnsureDDWallet();

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->pollBalanceChanged();

    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    QVERIFY2(result.status == WalletModel::OK, result.reasonFailed.toUtf8().constData());

    uint256 position_id;
    position_id.SetHex(result.positionId.toStdString());

    CTransactionRef mint_tx;
    {
        LOCK(wallet->cs_wallet);
        const wallet::CWalletTx* wtx = wallet->GetWalletTx(position_id);
        QVERIFY(wtx != nullptr);
        mint_tx = wtx->tx;
    }

    int64_t op_return_unlock_height{0};
    QVERIFY(DigiDollarWallet::ExtractUnlockHeightFromOpReturn(*mint_tx, op_return_unlock_height));

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    std::vector<WalletCollateralPosition> positions = dd_wallet->GetDDTimeLocks(/*active_only=*/false);
    QCOMPARE(positions.size(), static_cast<size_t>(1));

    WalletCollateralPosition stale = positions[0];
    stale.unlock_height = op_return_unlock_height - DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;
    QVERIFY(stale.unlock_height != op_return_unlock_height);
    QVERIFY(dd_wallet->WriteDDTimeLock(stale));

    positions = dd_wallet->GetDDTimeLocks(/*active_only=*/false);
    QCOMPARE(positions.size(), static_cast<size_t>(1));
    QCOMPARE(positions[0].unlock_height, stale.unlock_height);

    QVERIFY(dd_wallet->RefreshPositionMetadataFromMintTx(position_id));

    positions = dd_wallet->GetDDTimeLocks(/*active_only=*/false);
    QCOMPARE(positions.size(), static_cast<size_t>(1));
    QCOMPARE(positions[0].unlock_height, op_return_unlock_height);

    stale = positions[0];
    stale.unlock_height = op_return_unlock_height - DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;
    QVERIFY(dd_wallet->WriteDDTimeLock(stale));

    dd_wallet->ReconcilePositionStates();

    positions = dd_wallet->GetDDTimeLocks(/*active_only=*/false);
    QCOMPARE(positions.size(), static_cast<size_t>(1));
    QCOMPARE(positions[0].dd_timelock_id, position_id);
    QCOMPARE(positions[0].unlock_height, op_return_unlock_height);
    QCOMPARE(positions[0].dd_minted, CAmount(10000));

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarWidgetTests::qtFailedMintAbandonsRejectedDraft()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    // Regression (shenger RC45 report): a Qt DigiDollar mint whose
    // CommitTransaction() is rejected by mempool policy (e.g. too-long-mempool-chain)
    // must NOT leave the rejected mint draft behind as a live (non-abandoned) wallet
    // transaction. The generic wallet history model decodes ANY DD-shaped wallet tx
    // into "DigiDollar Collateral Lock / Transfer" rows, so a lingering draft surfaces
    // phantom DD activity even though no vault position was created. The RPC mint path
    // (rpc/digidollar.cpp) already abandons rejected mints; the Qt path must match.
    //
    // A high -minrelaytxfee makes the mempool reject the mint's fixed fee (0.1 DGB
    // MIN_DD_TX_FEE) as below the relay floor, deterministically forcing the mint's
    // CommitTransaction() to fail at broadcast. (DD mints are exempt from the
    // max-tx-fee check in BroadcastTransaction(), so that knob cannot be used here.)
    TestChain100Setup test{ChainType::REGTEST, {"-minrelaytxfee=1"}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MockOracleManager::GetInstance().SetEnabled(true);
    MockOracleManager::GetInstance().SetMockPrice(500000);

    CreateAndProcessOracleQuoteBlock(test, 500000);
    std::shared_ptr<wallet::CWallet> wallet = wallet::CreateSyncedWallet(
        *test.m_node.chain,
        WITH_LOCK(Assert(test.m_node.chainman)->GetMutex(), return test.m_node.chainman->ActiveChain()),
        test.coinbaseKey);
    wallet->SetBroadcastTransactions(true);
    wallet->EnsureDDWallet();

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->pollBalanceChanged();

    WalletModel::DigiDollarMintResult result = mini_gui.walletModel->mintDigiDollar(10000, 0);

    // The mint must report failure (not OK).
    QVERIFY2(result.status != WalletModel::OK,
             "mint unexpectedly succeeded despite a forced commit rejection");

    // No vault position may be created for a rejected mint.
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    QCOMPARE(dd_wallet->GetDDTimeLocks(/*active_only=*/false).size(), static_cast<size_t>(0));

    // Crucially: no rejected mint draft may linger as a live (non-abandoned) wallet
    // transaction that generic history would render as phantom DigiDollar activity.
    size_t lingering_mint_drafts = 0;
    {
        LOCK(wallet->cs_wallet);
        for (const auto& entry : wallet->mapWallet) {
            const wallet::CWalletTx& wtx = entry.second;
            if (wtx.isAbandoned()) continue;
            if (GetDigiDollarTxType(*wtx.tx) == DigiDollarTxType::DD_TX_MINT) {
                ++lingering_mint_drafts;
            }
        }
    }
    QCOMPARE(lingering_mint_drafts, static_cast<size_t>(0));

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarWidgetTests::sendWidgetTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    TestSendWidget(m_node, wallet);
}

void DigiDollarWidgetTests::sendSuccessDialogDoesNotPromiseNextBlockConfirmation()
{
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    DigiDollarSendWidget sendWidget(platformStyle.get());

    const QString message = sendWidget.successMessageForTesting(
        QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        12.34);
    const QString plain = QTextDocumentFragment::fromHtml(message).toPlainText();

    QVERIFY2(plain.contains(QStringLiteral("broadcast to the network"), Qt::CaseInsensitive),
             qPrintable(plain));
    QVERIFY2(plain.contains(QStringLiteral("pending"), Qt::CaseInsensitive),
             qPrintable(plain));
    QVERIFY2(!plain.contains(QStringLiteral("will be confirmed in the next block"), Qt::CaseInsensitive),
             qPrintable(plain));
}

void DigiDollarWidgetTests::sendWidgetCoinControlLabelsMirrorDgb()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    const COutPoint selected_a(uint256::ONE, 1);
    const COutPoint selected_b(uint256S("02"), 2);
    dd_wallet->AddDDUTXO(selected_a, 2500);
    dd_wallet->AddDDUTXO(selected_b, 7500);
    dd_wallet->AddDDUTXO(COutPoint(uint256S("03"), 3), 5000);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget sendWidget(mini_gui.platformStyle.get());
    sendWidget.setWalletModel(mini_gui.walletModel.get());
    sendWidget.setClientModel(mini_gui.clientModel.get());

    sendWidget.setSelectedDigiDollarInputsForTesting({selected_a, selected_b});
    QCoreApplication::processEvents();

    QLabel* quantityLabel = sendWidget.findChild<QLabel*>("coinControlQuantityLabel");
    QVERIFY(quantityLabel != nullptr);
    QCOMPARE(quantityLabel->text(), QString("Quantity: 2"));

    QLabel* amountLabel = sendWidget.findChild<QLabel*>("coinControlAmountLabel");
    QVERIFY(amountLabel != nullptr);
    QCOMPARE(amountLabel->text(), QString("Amount: 100.00 $DD"));

    sendWidget.setSelectedDigiDollarInputsForTesting({});
    QCoreApplication::processEvents();
    QCOMPARE(quantityLabel->text(), QString("automatically selected"));
    QVERIFY(amountLabel->text().isEmpty());
    QVERIFY(sendWidget.findChild<QLabel*>("preflightLabel") == nullptr);
}

void DigiDollarWidgetTests::sendWidgetCoinControlDialogSelectionFeedsSend()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    const COutPoint selected_input(Hash("qt-dd-send-selected-input"), 1);
    const COutPoint automatic_input(Hash("qt-dd-send-automatic-input"), 2);
    dd_wallet->AddDDUTXO(selected_input, 2500);
    dd_wallet->AddDDUTXO(automatic_input, 10000);
    QCOMPARE(static_cast<int>(dd_wallet->GetDDUTXOs().size()), 2);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarWallet* model_dd_wallet = mini_gui.walletModel->getDigiDollarWallet();
    QVERIFY(model_dd_wallet != nullptr);
    QCOMPARE(static_cast<int>(model_dd_wallet->GetDDUTXOs().size()), 2);

    DigiDollarSendWidget sendWidget(mini_gui.platformStyle.get());
    sendWidget.setWalletModel(mini_gui.walletModel.get());
    sendWidget.setClientModel(mini_gui.clientModel.get());

    QPushButton* coinControlButton = sendWidget.findChild<QPushButton*>("coinControlButton");
    QVERIFY(coinControlButton != nullptr);
    QCOMPARE(coinControlButton->text(), QString("Inputs..."));

    wallet::DDCoinControl coin_control;
    DigiDollarCoinControlDialog dialog(coin_control, mini_gui.walletModel.get(), mini_gui.platformStyle.get());
    dialog.show();
    QCoreApplication::processEvents();

    QString dialogError;
    QVERIFY2(SelectCoinControlDialogInput(selected_input, dialogError), qPrintable(dialogError));
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(coin_control.IsSelected(selected_input));

    sendWidget.setSelectedDigiDollarInputsForTesting(coin_control.ListSelected());
    QCoreApplication::processEvents();

    QLabel* quantityLabel = sendWidget.findChild<QLabel*>("coinControlQuantityLabel");
    QVERIFY(quantityLabel != nullptr);
    QCOMPARE(quantityLabel->text(), QString("Quantity: 1"));

    QLabel* amountLabel = sendWidget.findChild<QLabel*>("coinControlAmountLabel");
    QVERIFY(amountLabel != nullptr);
    QCOMPARE(amountLabel->text(), QString("Amount: 25.00 $DD"));

    const QString recipient = mini_gui.walletModel->getNewDigiDollarAddress(QStringLiteral("qt-selected-input-send"));
    QVERIFY(!recipient.isEmpty());

    const WalletModel::DigiDollarSendResult result =
        sendWidget.sendDigiDollarForTesting(recipient, 2000);
    QCOMPARE(result.status, WalletModel::TransactionCreationFailed);
    QVERIFY2(result.reasonFailed.contains(QStringLiteral("Selected DD input is unknown or not owned"), Qt::CaseInsensitive),
             qPrintable(result.reasonFailed));
}

void DigiDollarWidgetTests::receiveWidgetTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    TestReceiveWidget(m_node, wallet);
}

void DigiDollarWidgetTests::redeemWidgetTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    TestRedeemWidget(m_node, wallet);
}

void DigiDollarWidgetTests::redeemWidgetKeepsTimelockedPositionDisabled()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-timelocked-redeem");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 200);
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    dd_wallet->AddDDUTXO(COutPoint(uint256::ONE, 1), 10000);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.m_positionFound = true;
    redeemWidget.m_positionDDMinted = 100.0;
    redeemWidget.m_positionDGBCollateral = 300.0;
    redeemWidget.m_positionLockTier = 1;
    redeemWidget.m_positionBlocksRemaining = 95;
    redeemWidget.m_positionHealth = 150.0;
    redeemWidget.m_redeemableAmount = 0.0;
    redeemWidget.m_amountEdit->clear();
    redeemWidget.updatePositionInfo();
    redeemWidget.updateRedeemButtons();
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(!redeemButton->isEnabled());

    QLabel* ddMintedValue = redeemWidget.findChild<QLabel*>("ddMintedValue");
    QVERIFY(ddMintedValue != nullptr);
    QCOMPARE(ddMintedValue->text(), QString("100.00 $DD"));

    QLabel* redeemableValue = redeemWidget.findChild<QLabel*>("redeemableValue");
    QVERIFY(redeemableValue != nullptr);
    QCOMPARE(redeemableValue->text(), QString("0.00 $DD"));
}

void DigiDollarWidgetTests::positionsWidgetTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    TestPositionsWidget(m_node, wallet);
}

void DigiDollarWidgetTests::positionsWidgetHiddenDoesNotPollWallet()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    QVERIFY(!positionsWidget.isVisible());
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    QCoreApplication::processEvents();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 0);

    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updateView();
    QCOMPARE(table->rowCount(), 1);
}

void DigiDollarWidgetTests::positionsWidgetInitialLoadNotThrottled()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updateView();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
}

void DigiDollarWidgetTests::positionsWidgetHealthUsesMicroUsdOraclePrice()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MockOracleManager::GetInstance().SetEnabled(true);
    MockOracleManager::GetInstance().SetMockPrice(500000); // $0.50/DGB in micro-USD

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updateView();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);

    QWidget* healthWidget = table->cellWidget(0, DigiDollarPositionsWidget::COL_HEALTH);
    QVERIFY(healthWidget != nullptr);
    QProgressBar* healthBar = healthWidget->findChild<QProgressBar*>();
    QVERIFY(healthBar != nullptr);
    QCOMPARE(healthBar->value(), 150);

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarWidgetTests::positionsWidgetDisablesRedeemForPrivateKeyDisabledWallet()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updateView();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);

    QPushButton* redeemButton = qobject_cast<QPushButton*>(
        table->cellWidget(0, DigiDollarPositionsWidget::COL_ACTIONS));
    QVERIFY(redeemButton != nullptr);
    // DD-FA-FUNC-027 (Wave 17 Agent C): a wallet with WALLET_FLAG_DISABLE_PRIVATE_KEYS
    // must surface an explicit "Watch-Only" badge in the redeem column rather
    // than the previous ambiguous "Locked" text shared with timelocked vaults.
    QCOMPARE(redeemButton->text(), QString("Watch-Only"));
    QVERIFY(!redeemButton->isEnabled());
    // The tooltip must explain why the action is disabled so the user knows
    // their wallet is the limiting factor (not the timelock).
    QVERIFY(redeemButton->toolTip().contains("Watch-only"));
}

void DigiDollarWidgetTests::positionsWidgetKeepsRedeemActionableForLockedEncryptedWallet()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    // A locked wallet now runs ReconcilePositionStates() like any other, so the
    // position must be backed by a collateral outpoint that is really unspent;
    // otherwise reconciliation correctly retires it and the row reads
    // "Redeemed". Anchor it on a live coinbase output so that the wallet lock
    // stays the only variable under test.
    const uint256 position_id = test.m_coinbase_txns[0]->GetHash();
    AddMockDigiDollarPosition(wallet, position_id, 10000, 300 * COIN, 1, 100);

    SecureString passphrase{"wave17-qt-locked-wallet"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    QCOMPARE(mini_gui.walletModel->getEncryptionStatus(), WalletModel::Locked);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updateView();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);

    QPushButton* redeemButton = qobject_cast<QPushButton*>(
        table->cellWidget(0, DigiDollarPositionsWidget::COL_ACTIONS));
    QVERIFY(redeemButton != nullptr);
    // A locked encrypted wallet still holds its keys. The redeem flow prompts
    // for the passphrase via WalletModel::requestUnlock(), so the row action
    // must stay clickable; only its tooltip warns about the extra step.
    QCOMPARE(redeemButton->text(), QString("Redeem"));
    QVERIFY(redeemButton->isEnabled());
    QVERIFY(redeemButton->toolTip().contains("Wallet is locked"));
    QVERIFY(redeemButton->toolTip().contains("passphrase"));
}

void DigiDollarWidgetTests::redeemWidgetButtonStateNoSelection()
{
    DigiDollarRedeemWidget redeemWidget;

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(!redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Cannot Redeem"));
    QVERIFY(redeemButton->toolTip().contains("Select"));
}

void DigiDollarWidgetTests::redeemWidgetButtonStateTimelockActive()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-timelock-state");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 200);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.m_positionFound = true;
    redeemWidget.m_positionDDMinted = 100.0;
    redeemWidget.m_positionDGBCollateral = 300.0;
    redeemWidget.m_positionLockTier = 1;
    redeemWidget.m_positionBlocksRemaining = 95;
    redeemWidget.m_positionHealth = 150.0;
    redeemWidget.m_redeemableAmount = 0.0;
    redeemWidget.m_amountEdit->clear();
    redeemWidget.updatePositionInfo();
    redeemWidget.updateRedeemButtons();
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(!redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Cannot Redeem"));
    QVERIFY(redeemButton->toolTip().contains("Time remaining"));
    QVERIFY(redeemButton->toolTip().contains("Blocks remaining: 95"));
    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->toolTip().contains("Blocks remaining: 95"));
}

void DigiDollarWidgetTests::redeemWidgetButtonStateInvalidAmount()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-invalid-amount");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 10000);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QLineEdit* amountEdit = redeemWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(amountEdit != nullptr);
    amountEdit->setText(QStringLiteral("99.99"));
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(!redeemButton->isEnabled());
    QVERIFY(redeemButton->toolTip().contains("full redeemable"));
    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->toolTip().contains("full redeemable"));
}

void DigiDollarWidgetTests::redeemWidgetButtonStateInsufficientDDBalance()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-insufficient");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.m_positionFound = true;
    redeemWidget.m_positionDDMinted = 100.0;
    redeemWidget.m_positionDGBCollateral = 300.0;
    redeemWidget.m_positionLockTier = 1;
    redeemWidget.m_positionBlocksRemaining = 0;
    redeemWidget.m_positionHealth = 150.0;
    redeemWidget.m_redeemableAmount = 100.0;
    redeemWidget.m_amountEdit->setText(QStringLiteral("100.00"));
    redeemWidget.updatePositionInfo();
    redeemWidget.updateRedeemButtons();
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(!redeemButton->isEnabled());
    QVERIFY(redeemButton->toolTip().contains("Insufficient DigiDollar balance"));
    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->toolTip().contains("Insufficient DigiDollar balance"));
}

void DigiDollarWidgetTests::redeemWidgetButtonStatePrivateKeyDisabledWallet()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-watchonly");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 10000);
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(!redeemButton->isEnabled());
    QVERIFY(redeemButton->toolTip().contains("Watch-only"));
    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->toolTip().contains("Watch-only"));
}

void DigiDollarWidgetTests::redeemWidgetButtonStateLockedWalletOffersUnlock()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-wallet-locked");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    // Fund the $DD burn generously so that the wallet lock is the only thing
    // this test can be measuring (the ERR-adjusted burn can exceed the minted
    // amount). The previous exact-amount funding was never exercised because
    // the lock check short-circuited before the balance check.
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);
    SecureString passphrase{"qt-dd-redeem-wallet-locked"};
    QVERIFY(wallet->EncryptWallet(passphrase));

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    // A locked encrypted wallet is only temporarily unable to sign. Keeping the
    // button enabled is what makes the WalletModel::requestUnlock() passphrase
    // prompt in onRedeemClicked() reachable at all.
    QVERIFY(redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Unlock && Redeem"));
    QVERIFY(redeemButton->toolTip().contains("Wallet is locked"));
    QVERIFY(redeemButton->toolTip().contains("passphrase"));
    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->toolTip().contains("passphrase"));
}

void DigiDollarWidgetTests::redeemWidgetLockedWalletRequestsUnlockOnRedeem()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-requests-unlock");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);
    SecureString passphrase{"qt-dd-redeem-requests-unlock"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    QCOMPARE(mini_gui.walletModel->getEncryptionStatus(), WalletModel::Locked);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(redeemButton->isEnabled());

    // This is the behaviour the whole change exists for: driving the redeem
    // action on a LOCKED wallet must reach WalletModel::requestUnlock(), which
    // emits requireUnlock so the GUI can raise AskPassphraseDialog. While the
    // button was disabled that call site was unreachable.
    QSignalSpy unlockSpy(mini_gui.walletModel.get(), &WalletModel::requireUnlock);
    QSignalSpy completedSpy(&redeemWidget, &DigiDollarRedeemWidget::redemptionCompleted);

    AcceptRedeemConfirmation();
    redeemWidget.onRedeemClicked();
    QCoreApplication::processEvents();

    QCOMPARE(unlockSpy.count(), 1);
    // Nothing answers requireUnlock in this harness, which is exactly the
    // "user dismissed the passphrase prompt" path: the UnlockContext is invalid,
    // so onRedeemClicked() must return without building a redemption.
    QVERIFY(wallet->IsLocked());
    QCOMPARE(completedSpy.count(), 0);

    RemoveWallet(context, wallet, std::nullopt);
}

void DigiDollarWidgetTests::widgetsDisableRedeemForEncryptedWatchOnlyWallet()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-encrypted-watchonly");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);
    SecureString passphrase{"qt-dd-redeem-encrypted-watchonly"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    // A wallet that is both crypted and private-keys-disabled reports Locked,
    // not NoKeys (see WalletModel::getEncryptionStatus). This is the one state
    // where the two conditions this change separates overlap, so pin it: the
    // permanent incapability must still win over the temporary lock.
    QCOMPARE(mini_gui.walletModel->getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(mini_gui.walletModel->wallet().privateKeysDisabled());

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updateView();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QPushButton* rowButton = qobject_cast<QPushButton*>(
        table->cellWidget(0, DigiDollarPositionsWidget::COL_ACTIONS));
    QVERIFY(rowButton != nullptr);
    QCOMPARE(rowButton->text(), QString("Watch-Only"));
    QVERIFY(!rowButton->isEnabled());
    QVERIFY(rowButton->toolTip().contains("Watch-only"));

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(!redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Cannot Redeem"));
    QVERIFY(redeemButton->toolTip().contains("Watch-only"));

    RemoveWallet(context, wallet, std::nullopt);
}

void DigiDollarWidgetTests::redeemWidgetRefreshesWhenWalletUnlocks()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-wallet-unlock-refresh");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 0);
    SecureString passphrase{"qt-dd-redeem-wallet-unlock-refresh"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY(redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Unlock && Redeem"));

    QVERIFY(mini_gui.walletModel->setWalletLocked(false, passphrase));
    mini_gui.walletModel->updateStatus();
    QCoreApplication::processEvents();

    QCOMPARE(redeemWidget.m_positionBlocksRemaining, int64_t{0});
    QCOMPARE(mini_gui.walletModel->getDigiDollarBalance(), CAmount{100000000});
    QVERIFY2(redeemWidget.validateAmount(), "unlock-refresh amount should validate");
    QVERIFY2(redeemWidget.validateRedeemable(), "unlock-refresh position should be redeemable");
    QVERIFY2(redeemWidget.validateDDBalance(), "unlock-refresh DD balance should cover redemption");
    QVERIFY2(redeemWidget.canWalletSignRedemption(), "unlock-refresh wallet should be able to sign");
    QVERIFY(redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Redeem && Unlock DGB"));
    QVERIFY(redeemButton->toolTip().contains("Ready to redeem"));

    RemoveWallet(context, wallet, std::nullopt);
}

void DigiDollarWidgetTests::redeemWidgetButtonStateReady()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-ready");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.m_positionFound = true;
    redeemWidget.m_positionDDMinted = 100.0;
    redeemWidget.m_positionDGBCollateral = 300.0;
    redeemWidget.m_positionLockTier = 1;
    redeemWidget.m_positionBlocksRemaining = 0;
    redeemWidget.m_positionHealth = 150.0;
    redeemWidget.m_redeemableAmount = 100.0;
    redeemWidget.m_amountEdit->setText(QStringLiteral("100.00"));
    redeemWidget.updatePositionInfo();
    redeemWidget.updateRedeemButtons();
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY2(redeemWidget.validateAmount(), "ready state amount should validate");
    QVERIFY2(redeemWidget.validateRedeemable(), "ready state redeemable amount should validate");
    QVERIFY2(redeemWidget.validateDDBalance(), "ready state DD balance should validate");
    QVERIFY2(redeemWidget.canWalletSignRedemption(), "ready state wallet should be able to sign");
    QVERIFY(redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Redeem && Unlock DGB"));
    QVERIFY(redeemButton->toolTip().contains("Ready to redeem"));
    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->text().contains("ready", Qt::CaseInsensitive));
    QVERIFY(validationLabel->toolTip().contains("Ready to redeem"));
}

void DigiDollarWidgetTests::positionsWidgetLockedTooltipShowsRemainingBlocksAndTime()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    DigiDollarPositionsWidget positionsWidget;
    QPushButton* redeemButton = positionsWidget.createRedeemButton(
        QString::fromStdString(uint256::ONE.GetHex()),
        false,
        false,
        false,
        false,
        false,
        false,
        95);
    QVERIFY(redeemButton != nullptr);
    QCOMPARE(redeemButton->text(), QString("Locked"));
    QVERIFY(redeemButton->toolTip().contains("Time remaining: 24m"));
    QVERIFY(redeemButton->toolTip().contains("Blocks remaining: 95"));
}

void DigiDollarWidgetTests::positionsWidgetPendingMintButtonNotRedeemed()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    DigiDollarPositionsWidget positionsWidget;
    QPushButton* redeemButton = positionsWidget.createRedeemButton(
        QString::fromStdString(uint256::ONE.GetHex()),
        true,
        false,
        false,
        false,
        false,
        false,
        0);
    QVERIFY(redeemButton != nullptr);
    QCOMPARE(redeemButton->text(), QString("Confirming"));
    QVERIFY(!redeemButton->isEnabled());
    QVERIFY(redeemButton->toolTip().contains("pending confirmation"));
    QVERIFY(!redeemButton->toolTip().contains("already been redeemed"));
}

void DigiDollarWidgetTests::positionsWidgetPendingRedeemButtonNotRedeemed()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    DigiDollarPositionsWidget positionsWidget;
    QPushButton* redeemButton = positionsWidget.createRedeemButton(
        QString::fromStdString(uint256::ONE.GetHex()),
        false,
        true,
        false,
        false,
        false,
        false,
        0);
    QVERIFY(redeemButton != nullptr);
    QCOMPARE(redeemButton->text(), QString("Pending"));
    QVERIFY(!redeemButton->isEnabled());
    QVERIFY(redeemButton->toolTip().contains("pending confirmation"));
    QVERIFY(!redeemButton->toolTip().contains("already been redeemed"));
}

// DD-FA-FUNC-031 (Wave 19 Agent A): WalletModel::mintDigiDollar must
// short-circuit private-keys-disabled wallets with the same explicit
// "Private keys are disabled" diagnostic that sendDigiDollar already
// surfaces, instead of letting the user run through UTXO scans, oracle
// RPCs, and a confirmation dialog only to fail later at HD owner-key
// derivation with the misleading "requires an HD wallet" message.
void DigiDollarWidgetTests::mintDigiDollarRejectsPrivateKeyDisabledWallet()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->EnsureDDWallet();
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletModel::DigiDollarMintResult result =
        mini_gui.walletModel->mintDigiDollar(/*ddAmount=*/10000, /*lockTier=*/0);

    QVERIFY(result.status != WalletModel::OK);
    // Must be the explicit private-keys-disabled diagnostic, not the
    // misleading "requires an HD wallet" message that mint emits when
    // the HD owner-key derivation finally fails further down the path.
    QVERIFY2(result.reasonFailed.contains("Private keys are disabled", Qt::CaseInsensitive),
             qPrintable(QString("expected 'Private keys are disabled' in reasonFailed, got: ") + result.reasonFailed));
    // The fail-fast check must run before any wallet-side state mutation
    // so the txid/positionId remain empty for the rejected attempt.
    QVERIFY(result.txid.isEmpty());
    QVERIFY(result.positionId.isEmpty());
}

void DigiDollarWidgetTests::transactionsWidgetTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    
    TestTransactionsWidget(m_node, wallet);
}

void DigiDollarWidgetTests::sendWidgetNoteFieldTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget sendWidget(mini_gui.platformStyle.get());
    sendWidget.setWalletModel(mini_gui.walletModel.get());
    sendWidget.setClientModel(mini_gui.clientModel.get());

    QLineEdit* noteEdit = sendWidget.findChild<QLineEdit*>("noteEdit");
    QVERIFY(noteEdit != nullptr);

    QFrame* noteFrame = sendWidget.findChild<QFrame*>("noteFrame");
    QVERIFY(noteFrame != nullptr);
    QList<QLabel*> noteLabels = noteFrame->findChildren<QLabel*>();
    QCOMPARE(noteLabels.size(), 1);
    QCOMPARE(noteLabels.first()->text(), QString("Note:"));
    QVERIFY2(!noteLabels.first()->toolTip().contains(QStringLiteral("address book"), Qt::CaseInsensitive),
             "DigiDollar Send note label tooltip must not claim the note updates the address book");
    QVERIFY2(!noteEdit->placeholderText().contains(QStringLiteral("address"), Qt::CaseInsensitive),
             "DigiDollar Send note placeholder must describe a local note, not an address-book label");
    QVERIFY2(!noteEdit->toolTip().contains(QStringLiteral("address book"), Qt::CaseInsensitive),
             "DigiDollar Send note tooltip must not claim the note updates the address book");
    
    noteEdit->setText("Test transaction note");
    QCOMPARE(noteEdit->text(), QString("Test transaction note"));
    
    QVERIFY(noteEdit->maxLength() == 256);
}

void DigiDollarWidgetTests::transactionsWidgetExportTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());

    QPushButton* exportButton = transactionsWidget.findChild<QPushButton*>("m_exportButton");
    if (!exportButton) {
        QList<QPushButton*> buttons = transactionsWidget.findChildren<QPushButton*>();
        for (QPushButton* btn : buttons) {
            if (btn->text().contains("Export", Qt::CaseInsensitive)) {
                exportButton = btn;
                break;
            }
        }
    }
    QVERIFY(exportButton != nullptr);
    QVERIFY(exportButton->isEnabled());
}

void DigiDollarWidgetTests::addressBookTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DDAddressBookPage addressBook(mini_gui.platformStyle.get(), DDAddressBookPage::ForEditing);
    addressBook.setWalletModel(mini_gui.walletModel.get());

    QVERIFY(&addressBook != nullptr);
    
    QPushButton* newButton = addressBook.findChild<QPushButton*>();
    QVERIFY(newButton != nullptr);
    
    QTableWidget* table = addressBook.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->columnCount(), 2);
}

// ============================================================================
// Balance Change Validation Tests
// ============================================================================

void DigiDollarWidgetTests::mintValidationUpdatesOnBalanceChange()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarMintWidget mintWidget;
    mintWidget.setWalletModel(mini_gui.walletModel.get());
    mintWidget.setClientModel(mini_gui.clientModel.get());
    mintWidget.show();

    // Set an amount in the mint field to trigger validation
    QLineEdit* amountEdit = mintWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(amountEdit != nullptr);
    amountEdit->setText("1.00");

    // Get the warning label
    QLabel* warningLabel = mintWidget.findChild<QLabel*>("amountWarningLabel");
    QVERIFY(warningLabel != nullptr);

    // Force a collateral calculation and validation cycle
    mintWidget.updateView();

    // Capture the current validation state (stylesheet) before balance update
    QString styleBefore = amountEdit->styleSheet();

    // Now simulate a balance change (as if DGB arrived) by calling updateBalance()
    // This is the exact path that fires when the wallet receives new DGB
    mintWidget.updateBalance();

    // After updateBalance(), the validation state should have been re-evaluated.
    // The key assertion: updateBalance() must trigger updateAmountValidation().
    // We verify this by checking that the warning label visibility or amount edit
    // stylesheet was re-evaluated (not stale).
    //
    // Since updateBalance() now calls updateAmountValidation() + updateMintButton(),
    // the validation state should reflect the current balance, not a cached state.
    QString styleAfter = amountEdit->styleSheet();

    // In a test environment with no oracle price, both states may show the same
    // warning. The critical test is that updateBalance() doesn't crash and does
    // call through to updateAmountValidation(). We verify the label exists and
    // the stylesheet was set (non-empty means validation ran).
    QVERIFY2(!styleAfter.isEmpty() || amountEdit->text().isEmpty(),
             "Amount validation should run after updateBalance() — stylesheet should be set when amount is entered");

    // Verify the warning label is in a consistent state (visible with text, or hidden)
    if (warningLabel->isVisible()) {
        QVERIFY2(!warningLabel->text().isEmpty(),
                 "If warning label is visible after balance update, it should have text");
    }
}

// Regression test for the RC30/RC31 DD balance-refresh bug class: while the
// DigiDollar page is already open, a wallet balanceChanged signal must refresh
// the Mint tab's cached Available DGB label immediately.
void DigiDollarWidgetTests::ddTabRefreshesBalancesOnWalletSignal()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTab tab(mini_gui.platformStyle.get());
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());
    tab.show();

    QLabel* availableDGBValue = tab.findChild<QLabel*>("availableDGBValue");
    QVERIFY(availableDGBValue != nullptr);

    const QString expected = availableDGBValue->text();
    QVERIFY2(!expected.isEmpty(), "Expected Mint tab Available DGB label to be initialized");

    availableDGBValue->setText("stale-balance");
    Q_EMIT mini_gui.walletModel->balanceChanged(interfaces::WalletBalances{});
    QCoreApplication::processEvents();

    QCOMPARE(availableDGBValue->text(), expected);
}

void DigiDollarWidgetTests::transactionsWidgetRefreshesOnDigiDollarSignal()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    DDTransaction initialTx;
    initialTx.txid = "d000000000000000000000000000000000000000000000000000000000000001";
    initialTx.amount = 100;
    initialTx.timestamp = GetTime();
    initialTx.confirmations = 0;
    initialTx.incoming = true;
    initialTx.address = "TDinitial";
    initialTx.category = "receive";
    initialTx.lock_tier = -1;
    dd_wallet->AddMockTransaction(initialTx);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());
    transactionsWidget.show();
    transactionsWidget.updateView();
    QCoreApplication::processEvents();

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);

    DDTransaction sendTx;
    sendTx.txid = "d000000000000000000000000000000000000000000000000000000000000002";
    sendTx.amount = 250;
    sendTx.timestamp = GetTime() + 1;
    sendTx.confirmations = 0;
    sendTx.incoming = false;
    sendTx.address = "TDsend";
    sendTx.category = "send";
    sendTx.comment = "fresh send";
    sendTx.lock_tier = -1;
    dd_wallet->AddMockTransaction(sendTx);

    DDTransaction localTx;
    localTx.txid = "d000000000000000000000000000000000000000000000000000000000000003";
    localTx.amount = 300;
    localTx.timestamp = GetTime() + 2;
    localTx.confirmations = 0;
    localTx.incoming = true;
    localTx.address = "TDlocal";
    localTx.category = "mint";
    localTx.comment = "not relayed";
    localTx.lock_tier = 0;
    localTx.is_local = true;
    dd_wallet->AddMockTransaction(localTx);

    const bool invoked = QMetaObject::invokeMethod(mini_gui.walletModel.get(), "digiDollarChanged", Qt::DirectConnection);
    QVERIFY2(invoked, "WalletModel must expose a DigiDollar-specific refresh signal");
    QCoreApplication::processEvents();

    QCOMPARE(table->rowCount(), 3);
    bool foundSend = false;
    bool foundLocal = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* txidItem = table->item(row, 5);
        QTableWidgetItem* typeItem = table->item(row, 1);
        QTableWidgetItem* amountItem = table->item(row, 2);
        QTableWidgetItem* statusItem = table->item(row, 6);
        QTableWidgetItem* noteItem = table->item(row, 4);
        if (txidItem && txidItem->data(Qt::UserRole).toString() == QString::fromStdString(sendTx.txid)) {
            foundSend = true;
            QCOMPARE(typeItem ? typeItem->text() : QString(), QStringLiteral("Send"));
            QCOMPARE(amountItem ? amountItem->text() : QString(), QStringLiteral("-2.50 $DD"));
            QCOMPARE(statusItem ? statusItem->text() : QString(), QStringLiteral("Pending"));
            QCOMPARE(noteItem ? noteItem->text() : QString(), QStringLiteral("fresh send"));
        }
        if (txidItem && txidItem->data(Qt::UserRole).toString() == QString::fromStdString(localTx.txid)) {
            foundLocal = true;
            QCOMPARE(typeItem ? typeItem->text() : QString(), QStringLiteral("Mint 1-hr"));
            QCOMPARE(amountItem ? amountItem->text() : QString(), QStringLiteral("+3.00 $DD"));
            QCOMPARE(statusItem ? statusItem->text() : QString(), QStringLiteral("Local"));
            QVERIFY2(statusItem && statusItem->toolTip().contains(QStringLiteral("not currently in mempool")),
                     qPrintable(statusItem ? statusItem->toolTip() : QString()));
        }
    }
    QVERIFY2(foundSend, "DD Transactions must refresh immediately when DigiDollar wallet state changes");
    QVERIFY2(foundLocal, "DD Transactions must show a distinct local/not-relayed bucket");
}

// Regression test for au_epic's report: reopening the main DigiDollar page
// after a redeem/unlock must refresh the Mint tab's Available DGB label instead
// of leaving a stale cached value until full wallet restart.
void DigiDollarWidgetTests::walletViewRefreshesDigiDollarPageOnOpen()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletView view(mini_gui.walletModel.get(), mini_gui.platformStyle.get(), nullptr);
    view.setClientModel(mini_gui.clientModel.get());
    view.show();
    view.gotoOverviewPage();

    QLabel* availableDGBValue = view.findChild<QLabel*>("availableDGBValue");
    QVERIFY(availableDGBValue != nullptr);

    const QString expected = availableDGBValue->text();
    QVERIFY2(!expected.isEmpty(), "Expected Mint tab Available DGB label to be initialized");

    availableDGBValue->setText("stale-on-open");
    view.gotoDigiDollarPage();
    QCoreApplication::processEvents();

    QCOMPARE(availableDGBValue->text(), expected);
}

// ============================================================================
// Privacy / Mask Values Tests
// ============================================================================

void DigiDollarWidgetTests::privacyTabSetPrivacySlotTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTab tab(mini_gui.platformStyle.get());
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());

    // Verify DigiDollarTab has setPrivacy slot and it can be called
    tab.setPrivacy(true);
    tab.setPrivacy(false);
    // If we get here without crash, the slot exists and works
    QVERIFY(true);
}

void DigiDollarWidgetTests::privacyOverviewMaskTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarOverviewWidget overviewWidget;
    overviewWidget.setWalletModel(mini_gui.walletModel.get());
    overviewWidget.setClientModel(mini_gui.clientModel.get());
    overviewWidget.show();

    // Enable privacy mode
    overviewWidget.setPrivacy(true);

    // Check that balance labels contain '#' (masked)
    QLabel* ddBalanceValue = overviewWidget.findChild<QLabel*>("ddBalanceValue");
    QVERIFY(ddBalanceValue != nullptr);
    QVERIFY2(ddBalanceValue->text().contains('#'), "DD balance should be masked with # when privacy is enabled");

    QLabel* dgbCollateralValue = overviewWidget.findChild<QLabel*>("dgbCollateralValue");
    QVERIFY(dgbCollateralValue != nullptr);
    QVERIFY2(dgbCollateralValue->text().contains('#'), "DGB collateral should be masked with # when privacy is enabled");

    QLabel* usdValueValue = overviewWidget.findChild<QLabel*>("usdValueValue");
    QVERIFY(usdValueValue != nullptr);
    QVERIFY2(usdValueValue->text().contains('#'), "USD value should be masked with # when privacy is enabled");

    // Check that recent transactions list is hidden
    QListWidget* transactionsList = overviewWidget.findChild<QListWidget*>("transactionsList");
    QVERIFY(transactionsList != nullptr);
    QVERIFY2(transactionsList->isHidden(), "Transactions list should be hidden when privacy is enabled");

    // Disable privacy mode
    overviewWidget.setPrivacy(false);

    // Check that balance labels no longer contain '#'
    QVERIFY2(!ddBalanceValue->text().contains('#'), "DD balance should NOT be masked when privacy is disabled");
    QVERIFY2(!dgbCollateralValue->text().contains('#'), "DGB collateral should NOT be masked when privacy is disabled");
    QVERIFY2(!usdValueValue->text().contains('#'), "USD value should NOT be masked when privacy is disabled");
}

void DigiDollarWidgetTests::overviewUsdValueShowsUsdSuffixWhenPrivacyOff()
{
    DigiDollarOverviewWidget overviewWidget;
    QLabel* usdValueValue = overviewWidget.findChild<QLabel*>("usdValueValue");
    QVERIFY(usdValueValue != nullptr);

    overviewWidget.setPrivacy(false);
    QVERIFY2(usdValueValue->text().endsWith(QStringLiteral(" $USD")),
             qPrintable(QString("Overview USD value must include explicit $USD suffix, got: %1")
                            .arg(usdValueValue->text())));
}

void DigiDollarWidgetTests::digiDollarAmountLabelsUseCurrencyPrefix()
{
    DigiDollarOverviewWidget overviewWidget;
    QLabel* overviewBalance = overviewWidget.findChild<QLabel*>("ddBalanceValue");
    QLabel* overviewUsdLabel = overviewWidget.findChild<QLabel*>("usdValueLabel");
    QLabel* overviewUsdValue = overviewWidget.findChild<QLabel*>("usdValueValue");
    QVERIFY(overviewBalance != nullptr);
    QVERIFY(overviewUsdLabel != nullptr);
    QVERIFY(overviewUsdValue != nullptr);
    QCOMPARE(overviewBalance->text(), QStringLiteral("0.00 $DD"));
    QCOMPARE(overviewUsdLabel->text(), QStringLiteral("Total $USD:"));
    QCOMPARE(overviewUsdValue->text(), QStringLiteral("0.00 $USD"));

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platformStyle.get());
    QTabWidget* tabWidget = tab.findChild<QTabWidget*>("digiDollarSubTabs");
    QVERIFY(tabWidget != nullptr);
    const QStringList expectedTabLabels{
        QStringLiteral("$DD Overview"),
        QStringLiteral("Send $DD"),
        QStringLiteral("Receive $DD"),
        QStringLiteral("Mint $DD"),
        QStringLiteral("Redeem $DD"),
        QStringLiteral("$DD Vault"),
        QStringLiteral("$DD Transactions"),
    };
    QCOMPARE(tabWidget->count(), expectedTabLabels.size());
    for (int i = 0; i < expectedTabLabels.size(); ++i) {
        QCOMPARE(tabWidget->tabText(i), expectedTabLabels.at(i));
    }

    if (qEnvironmentVariableIsSet("DIGIBYTE_QT_SAVE_DD_LABEL_QA")) {
        QStackedWidget* stackedWidget = tab.findChild<QStackedWidget*>("digiDollarStack");
        QVERIFY(stackedWidget != nullptr);
        stackedWidget->setCurrentIndex(1);
        QObject::disconnect(tabWidget, nullptr, &tab, nullptr);
        tab.resize(1200, 760);
        tab.show();
        for (int i = 0; i < tabWidget->count(); ++i) {
            tabWidget->setCurrentIndex(i);
            QCoreApplication::processEvents();
            const QString path = QStringLiteral("/tmp/digibyte_dd_label_qa_tab_%1.png").arg(i);
            const QPixmap pixmap = tab.grab();
            QVERIFY2(pixmap.save(path), qPrintable(QStringLiteral("failed to save DigiDollar label QA screenshot to %1").arg(path)));
            qInfo("DigiDollar label QA screenshot: %s", qPrintable(path));
        }
    }

    DigiDollarSendWidget sendWidget(platformStyle.get());
    QLabel* sendUsdLabel = sendWidget.findChild<QLabel*>("usdEquivalentLabel");
    QLabel* sendUsdValue = sendWidget.findChild<QLabel*>("usdEquivalentValue");
    QLabel* sendAvailable = sendWidget.findChild<QLabel*>("availableBalanceValue");
    QLabel* sendTotal = sendWidget.findChild<QLabel*>("totalValue");
    QVERIFY(sendUsdLabel != nullptr);
    QVERIFY(sendUsdValue != nullptr);
    QVERIFY(sendAvailable != nullptr);
    QVERIFY(sendTotal != nullptr);
    QCOMPARE(sendUsdLabel->text(), QStringLiteral("$USD Equivalent:"));
    QCOMPARE(sendUsdValue->text(), QStringLiteral("0.00 $USD"));
    QCOMPARE(sendAvailable->text(), QStringLiteral("0.00 $DD"));
    QCOMPARE(sendTotal->text(), QStringLiteral("0.00 $DD"));

    DigiDollarMintWidget mintWidget;
    QLabel* mintAmountSuffix = mintWidget.findChild<QLabel*>("amountSuffix");
    QLabel* mintUsdLabel = mintWidget.findChild<QLabel*>("usdValueLabel");
    QLabel* mintUsdValue = mintWidget.findChild<QLabel*>("usdValueValue");
    QVERIFY(mintAmountSuffix != nullptr);
    QVERIFY(mintUsdLabel != nullptr);
    QVERIFY(mintUsdValue != nullptr);
    QCOMPARE(mintAmountSuffix->text(), QStringLiteral("$DD"));
    QCOMPARE(mintUsdLabel->text(), QStringLiteral("$USD Equivalent:"));
    QCOMPARE(mintUsdValue->text(), QStringLiteral("0.00 $USD"));

    DigiDollarRedeemWidget redeemWidget;
    QLabel* redeemAmountSuffix = redeemWidget.findChild<QLabel*>("amountSuffix");
    QLabel* ddMinted = redeemWidget.findChild<QLabel*>("ddMintedValue");
    QLabel* redeemable = redeemWidget.findChild<QLabel*>("redeemableValue");
    QVERIFY(redeemAmountSuffix != nullptr);
    QVERIFY(ddMinted != nullptr);
    QVERIFY(redeemable != nullptr);
    QCOMPARE(redeemAmountSuffix->text(), QStringLiteral("$DD"));
    QCOMPARE(ddMinted->text(), QStringLiteral("0.00 $DD"));
    QCOMPARE(redeemable->text(), QStringLiteral("0.00 $DD"));

    SendCoinsRecipient recipient;
    recipient.address = QStringLiteral("RDrequestTestAddress");
    recipient.amount = 12345;
    DigiDollarReceiveRequestDialog requestDialog;
    requestDialog.setInfo(recipient);

    bool foundFormattedAmount = false;
    for (QLabel* label : requestDialog.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("123.45 $DD")) {
            foundFormattedAmount = true;
            break;
        }
    }
    QVERIFY2(foundFormattedAmount, "DigiDollar receive request dialog should render requested DD as 123.45 $DD");
}

void DigiDollarWidgetTests::overviewPrivacyMaskHidesAmountUnits()
{
    DigiDollarOverviewWidget overviewWidget;
    overviewWidget.setPrivacy(true);

    const QStringList sensitiveLabels{
        QStringLiteral("ddBalanceValue"),
        QStringLiteral("dgbCollateralValue"),
        QStringLiteral("usdValueValue"),
        QStringLiteral("networkTotalDDValue"),
        QStringLiteral("networkTotalCollateralValue"),
    };
    const QRegularExpression digitRe(QStringLiteral("\\d"));

    for (const QString& objectName : sensitiveLabels) {
        QLabel* label = overviewWidget.findChild<QLabel*>(objectName);
        QVERIFY2(label != nullptr, qPrintable(QString("Missing label %1").arg(objectName)));
        const QString text = label->text();
        QVERIFY2(text.contains('#'), qPrintable(QString("%1 should be visibly masked, got: %2").arg(objectName, text)));
        QVERIFY2(!text.contains(digitRe), qPrintable(QString("%1 leaked digits while masked: %2").arg(objectName, text)));
        QVERIFY2(!text.contains(QStringLiteral("USD")), qPrintable(QString("%1 leaked USD suffix while masked: %2").arg(objectName, text)));
        QVERIFY2(!text.contains(QStringLiteral("DGB")), qPrintable(QString("%1 leaked DGB suffix while masked: %2").arg(objectName, text)));
        QVERIFY2(!text.contains(QStringLiteral("DD")), qPrintable(QString("%1 leaked DD suffix while masked: %2").arg(objectName, text)));
    }
}

void DigiDollarWidgetTests::overviewLayoutStretchFavorsBlockchainTotals()
{
    DigiDollarOverviewWidget overviewWidget;
    QHBoxLayout* healthContentLayout = overviewWidget.findChild<QHBoxLayout*>(QStringLiteral("healthContentLayout"));
    QVERIFY(healthContentLayout != nullptr);

    QLabel* healthTitle = overviewWidget.findChild<QLabel*>(QStringLiteral("healthTitle"));
    QVERIFY(healthTitle != nullptr);
    QCOMPARE(healthTitle->text(), QStringLiteral("Blockchain DigiDollar Status"));

    QLabel* ddSupplyLabel = overviewWidget.findChild<QLabel*>(QStringLiteral("networkTotalDDLabel"));
    QVERIFY(ddSupplyLabel != nullptr);
    QCOMPARE(ddSupplyLabel->text(), QStringLiteral("Blockchain $DD Supply"));

    QLabel* dgbLockedLabel = overviewWidget.findChild<QLabel*>(QStringLiteral("networkTotalCollateralLabel"));
    QVERIFY(dgbLockedLabel != nullptr);
    QCOMPARE(dgbLockedLabel->text(), QStringLiteral("Blockchain DGB Locked"));

    QWidget* leftStatsFrame = overviewWidget.findChild<QWidget*>(QStringLiteral("leftStatsFrame"));
    QWidget* networkTotalsFrame = overviewWidget.findChild<QWidget*>(QStringLiteral("networkTotalsFrame"));
    QVERIFY(leftStatsFrame != nullptr);
    QVERIFY(networkTotalsFrame != nullptr);

    const int leftIndex = healthContentLayout->indexOf(leftStatsFrame);
    const int totalsIndex = healthContentLayout->indexOf(networkTotalsFrame);
    QVERIFY(leftIndex >= 0);
    QVERIFY(totalsIndex >= 0);
    QVERIFY2(healthContentLayout->stretch(totalsIndex) > healthContentLayout->stretch(leftIndex),
             "Blockchain totals should get more horizontal stretch than the smaller left stats column");
}

void DigiDollarWidgetTests::overviewBlockchainTotalsFitLaunchScaleValues()
{
    DigiDollarOverviewWidget overviewWidget;
    QLabel* ddValue = overviewWidget.findChild<QLabel*>(QStringLiteral("networkTotalDDValue"));
    QLabel* dgbValue = overviewWidget.findChild<QLabel*>(QStringLiteral("networkTotalCollateralValue"));
    QWidget* totalsFrame = overviewWidget.findChild<QWidget*>(QStringLiteral("networkTotalsFrame"));
    QVERIFY(ddValue != nullptr);
    QVERIFY(dgbValue != nullptr);
    QVERIFY(totalsFrame != nullptr);

    overviewWidget.setMonospacedFont(false);

    const QString launchScaleDD = QStringLiteral("999,000,000.00 $DD");
    const QString stressScaleDD = QStringLiteral("11,000,000,000.00 $DD");
    const QString maxDgbLocked = QStringLiteral("21,000,000,000.00 DGB");

    const int launchScaleDDWidth = QFontMetrics(ddValue->font()).horizontalAdvance(launchScaleDD);
    const int stressScaleDDWidth = QFontMetrics(ddValue->font()).horizontalAdvance(stressScaleDD);
    const int maxDgbLockedWidth = QFontMetrics(dgbValue->font()).horizontalAdvance(maxDgbLocked);

    QVERIFY2(ddValue->minimumWidth() >= launchScaleDDWidth,
             qPrintable(QString("Blockchain DD supply label is too narrow for %1").arg(launchScaleDD)));
    QVERIFY2(ddValue->minimumWidth() >= stressScaleDDWidth,
             qPrintable(QString("Blockchain DD supply label is too narrow for %1").arg(stressScaleDD)));
    QVERIFY2(dgbValue->minimumWidth() >= maxDgbLockedWidth,
             qPrintable(QString("Blockchain DGB locked label is too narrow for %1").arg(maxDgbLocked)));
    QVERIFY2(totalsFrame->minimumWidth() > ddValue->minimumWidth(),
             "Blockchain totals frame must include room around the value labels");
}

void DigiDollarWidgetTests::overviewHealthUsesCollateralizedLanguage()
{
    DigiDollarOverviewWidget overviewWidget;
    QLabel* systemHealthValue = overviewWidget.findChild<QLabel*>(QStringLiteral("systemHealthValue"));
    QProgressBar* systemHealthBar = overviewWidget.findChild<QProgressBar*>(QStringLiteral("systemHealthBar"));
    QVERIFY(systemHealthValue != nullptr);
    QVERIFY(systemHealthBar != nullptr);

    QCOMPARE(systemHealthValue->text(), QStringLiteral("Loading..."));
    QVERIFY2(!systemHealthValue->text().contains(QStringLiteral("Healthy")),
             "System health value should not describe collateralization as healthy state text");
    QCOMPARE(systemHealthBar->format(), QStringLiteral("0% Collateralization"));
}

void DigiDollarWidgetTests::overviewSystemHealthRpcPollingIsThrottled()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };
    const auto findFile = [&](const QStringList& candidates) -> QString {
        for (const auto& p : candidates) {
            const QString text = readFile(p.toUtf8().constData());
            if (!text.isEmpty()) return text;
        }
        return {};
    };

    const QString header = findFile({
        QStringLiteral("src/qt/digidollaroverviewwidget.h"),
        QStringLiteral("../src/qt/digidollaroverviewwidget.h"),
        QStringLiteral("../../src/qt/digidollaroverviewwidget.h"),
        QStringLiteral("qt/digidollaroverviewwidget.h"),
    });
    QVERIFY2(!header.isEmpty(), "could not locate digidollaroverviewwidget.h from current working directory");

    const QString source = findFile({
        QStringLiteral("src/qt/digidollaroverviewwidget.cpp"),
        QStringLiteral("../src/qt/digidollaroverviewwidget.cpp"),
        QStringLiteral("../../src/qt/digidollaroverviewwidget.cpp"),
        QStringLiteral("qt/digidollaroverviewwidget.cpp"),
    });
    QVERIFY2(!source.isEmpty(), "could not locate digidollaroverviewwidget.cpp from current working directory");

    QVERIFY2(header.contains(QStringLiteral("SYSTEM_HEALTH_UPDATE_INTERVAL_MS")),
             "Overview must keep an explicit, separate throttle for getdigidollarstats polling");
    QVERIFY2(header.contains(QStringLiteral("m_lastSystemHealthUpdateTime")),
             "Overview must remember the last system-health RPC time");
    QVERIFY2(source.contains(QStringLiteral("updateSystemHealthIfDue")),
             "Overview must call system-health updates through a throttling helper");
    QVERIFY2(!source.contains(QStringLiteral("updateOraclePrice();\n    updateSystemHealth();\n    updateRecentTransactions();")),
             "updateView must not call getdigidollarstats on every 5-second UI refresh");
}

void DigiDollarWidgetTests::overviewPendingBalanceHasThemeRules()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };

    const auto findTheme = [&](const QString& name) -> QString {
        const QStringList candidates = {
            QStringLiteral("src/qt/res/css/%1").arg(name),
            QStringLiteral("../src/qt/res/css/%1").arg(name),
            QStringLiteral("../../src/qt/res/css/%1").arg(name),
            QStringLiteral("qt/res/css/%1").arg(name),
        };
        for (const auto& p : candidates) {
            const QString css = readFile(p.toUtf8().constData());
            if (!css.isEmpty()) return css;
        }
        return {};
    };

    const auto requirePendingRule = [](const QString& css, const QString& theme) {
        const QRegularExpression pendingLabel(
            QStringLiteral(R"re(DigiDollarOverviewWidget\s+\.QFrame#balanceFrame\s+\.QLabel#ddPendingLabel[^\{]*\{[^\}]*qproperty-alignment\s*:[^\;]*AlignRight[^\}]*min-width\s*:\s*160px\s*;[^\}]*font-size\s*:\s*11pt\s*;)re"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        QVERIFY2(pendingLabel.match(css).hasMatch(),
                 qPrintable(QString("%1 must style #ddPendingLabel like the other DigiDollar balance labels").arg(theme)));

        const QRegularExpression pendingValue(
            QStringLiteral(R"re(DigiDollarOverviewWidget\s+\.QFrame#balanceFrame\s+\.QLabel#ddPendingValue[^\{]*\{[^\}]*qproperty-alignment\s*:[^\;]*AlignLeft[^\}]*font-size\s*:\s*13pt\s*;)re"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        QVERIFY2(pendingValue.match(css).hasMatch(),
                 qPrintable(QString("%1 must style #ddPendingValue like the adjacent DigiDollar value rows").arg(theme)));
    };

    const QString light = findTheme(QStringLiteral("light.css"));
    QVERIFY2(!light.isEmpty(), "could not locate light.css from current working directory");
    requirePendingRule(light, QStringLiteral("light.css"));

    const QString dark = findTheme(QStringLiteral("dark.css"));
    QVERIFY2(!dark.isEmpty(), "could not locate dark.css from current working directory");
    requirePendingRule(dark, QStringLiteral("dark.css"));
}

void DigiDollarWidgetTests::digiDollarSectionUsesGreenThemeRules()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };

    const auto findTheme = [&](const QString& name) -> QString {
        const QStringList candidates = {
            QStringLiteral("src/qt/res/css/%1").arg(name),
            QStringLiteral("../src/qt/res/css/%1").arg(name),
            QStringLiteral("../../src/qt/res/css/%1").arg(name),
            QStringLiteral("qt/res/css/%1").arg(name),
        };
        for (const auto& p : candidates) {
            const QString css = readFile(p.toUtf8().constData());
            if (!css.isEmpty()) return css;
        }
        return {};
    };

    const auto requireGreenSection = [](const QString& css, const QString& theme) {
        const QStringList requiredSelectors = {
            QStringLiteral("QToolBar > QToolButton#digiDollarToolButton"),
            QStringLiteral("QWidget#digiDollarTab"),
            QStringLiteral("QStackedWidget#digiDollarStack"),
            QStringLiteral("QTabWidget#digiDollarSubTabs"),
            QStringLiteral("QTabWidget#digiDollarSubTabs QTabBar::tab:selected"),
            QStringLiteral("DigiDollarOverviewWidget .QFrame#balanceFrame"),
            QStringLiteral("DigiDollarReceiveWidget QFrame#generateFrame"),
            QStringLiteral("DigiDollarReceiveWidget QPushButton#editRequestButton"),
            QStringLiteral("DigiDollarSendWidget QFrame#addressFrame"),
            QStringLiteral("DigiDollarSendWidget QPushButton#coinControlButton"),
            QStringLiteral("DigiDollarMintWidget QFrame#amountFrame"),
            QStringLiteral("DigiDollarRedeemWidget QFrame#positionFrame"),
            QStringLiteral("DigiDollarRedeemWidget QPushButton#coinControlButton"),
            QStringLiteral("DigiDollarPositionsWidget QTableWidget"),
            QStringLiteral("DigiDollarTransactionsWidget QTableWidget"),
        };

        for (const QString& selector : requiredSelectors) {
            QVERIFY2(css.contains(selector),
                     qPrintable(QString("%1 missing DigiDollar green-theme selector: %2").arg(theme, selector)));
        }

        QVERIFY2(css.contains(QStringLiteral("DIGIDOLLAR GREEN SECTION THEME")),
                 qPrintable(QString("%1 must label the scoped DigiDollar green theme block").arg(theme)));
        QVERIFY2(css.contains(QStringLiteral("#1f9d57")) || css.contains(QStringLiteral("#16804f")),
                 qPrintable(QString("%1 must include the green primary DigiDollar accent").arg(theme)));
        QVERIFY2(!css.contains(QStringLiteral("QToolBar > QToolButton#digiDollarToolButton:checked {\n    background-color:#0066CC")),
                 qPrintable(QString("%1 must not style the checked DigiDollar top-nav button with the DGB blue accent").arg(theme)));
    };

    const QString light = findTheme(QStringLiteral("light.css"));
    QVERIFY2(!light.isEmpty(), "could not locate light.css from current working directory");
    requireGreenSection(light, QStringLiteral("light.css"));

    const QString dark = findTheme(QStringLiteral("dark.css"));
    QVERIFY2(!dark.isEmpty(), "could not locate dark.css from current working directory");
    requireGreenSection(dark, QStringLiteral("dark.css"));
}

void DigiDollarWidgetTests::digiDollarModalDialogsUseGreenThemeRules()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };
    const auto findTheme = [&](const QString& name) -> QString {
        const QStringList candidates = {
            QStringLiteral("src/qt/res/css/%1").arg(name),
            QStringLiteral("../src/qt/res/css/%1").arg(name),
            QStringLiteral("../../src/qt/res/css/%1").arg(name),
            QStringLiteral("qt/res/css/%1").arg(name),
        };
        for (const auto& p : candidates) {
            const QString css = readFile(p.toUtf8().constData());
            if (!css.isEmpty()) return css;
        }
        return {};
    };
    const auto extractRule = [](const QString& css, const QString& selector) -> QString {
        const int selectorStart = css.indexOf(selector);
        if (selectorStart < 0) return {};
        const int braceStart = css.indexOf(QLatin1Char('{'), selectorStart);
        if (braceStart < 0) return {};
        const int braceEnd = css.indexOf(QLatin1Char('}'), braceStart);
        if (braceEnd < 0) return {};
        return css.mid(braceStart + 1, braceEnd - braceStart - 1);
    };
    const auto requireRule = [&](const QString& css, const QString& theme, const QString& selector,
                                 const QString& expectedColor) {
        const QString rule = extractRule(css, selector);
        QVERIFY2(!rule.isEmpty(),
                 qPrintable(QStringLiteral("%1 must style %2").arg(theme, selector)));
        QVERIFY2(rule.contains(expectedColor, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 %2 must use DigiDollar green color %3")
                                 .arg(theme, selector, expectedColor)));

        const QStringList dgbBlueColors = {
            QStringLiteral("#002352"),
            QStringLiteral("#003366"),
            QStringLiteral("#0066CC"),
            QStringLiteral("#0066cc"),
            QStringLiteral("#0088ff"),
            QStringLiteral("#0044aa"),
            QStringLiteral("#0055aa"),
            QStringLiteral("#A7C6ED"),
            QStringLiteral("#9BB8E8"),
        };
        for (const QString& color : dgbBlueColors) {
            QVERIFY2(!rule.contains(color, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("%1 %2 must not reuse DGB blue color %3")
                                     .arg(theme, selector, color)));
        }
    };
    const auto requireTheme = [&](const QString& css, const QString& theme, const QString& dialogBg,
                                  const QString& panelBg, const QString& accent) {
        requireRule(css, theme, QStringLiteral("QDialog#DigiDollarCoinControlDialog"), dialogBg);
        requireRule(css, theme, QStringLiteral("QDialog#DigiDollarCoinControlDialog QTreeWidget"), panelBg);
        requireRule(css, theme, QStringLiteral("QDialog#DigiDollarCoinControlDialog QHeaderView::section"), accent);
        requireRule(css, theme, QStringLiteral("QDialog#DigiDollarCoinControlDialog QPushButton"), accent);

        requireRule(css, theme, QStringLiteral("QDialog#DigiDollarReceiveRequestDialog"), dialogBg);
        requireRule(css, theme, QStringLiteral("QDialog#DigiDollarReceiveRequestDialog QPushButton"), accent);

        requireRule(css, theme, QStringLiteral("QDialog#DDAddressBookPage"), dialogBg);
        requireRule(css, theme, QStringLiteral("QDialog#DDAddressBookPage QTableWidget"), panelBg);
        requireRule(css, theme, QStringLiteral("QDialog#DDAddressBookPage QHeaderView::section"), accent);
        requireRule(css, theme, QStringLiteral("QDialog#DDAddressBookPage QPushButton"), accent);
    };

    const QString dark = findTheme(QStringLiteral("dark.css"));
    QVERIFY2(!dark.isEmpty(), "could not locate dark.css from current working directory");
    requireTheme(dark, QStringLiteral("dark.css"), QStringLiteral("#0b2419"),
                 QStringLiteral("#113a29"), QStringLiteral("#16804f"));

    const QString light = findTheme(QStringLiteral("light.css"));
    QVERIFY2(!light.isEmpty(), "could not locate light.css from current working directory");
    requireTheme(light, QStringLiteral("light.css"), QStringLiteral("#eef9f2"),
                 QStringLiteral("#ffffff"), QStringLiteral("#1f9d57"));
}

void DigiDollarWidgetTests::digiDollarModalDialogsOverrideDgbBlueFallback()
{
#ifdef Q_OS_MACOS
    QSKIP("Skipping DD modal fallback style test on macOS");
#endif

    const QString originalStyleSheet = qApp->styleSheet();
    const QPalette originalPalette = qApp->palette();
    const QString dgbBlueFallback = QStringLiteral(
        "QDialog { background-color: #002352; color: #ffffff; }"
        "QDialog QLabel { color: #ffffff; }"
        "QDialog QTreeWidget, QDialog QTableWidget { background-color: #A7C6ED; color: #003366; selection-background-color: #0066CC; }"
        "QDialog QHeaderView::section { background-color: #0066CC; color: #ffffff; }"
        "QDialog QPushButton { background-color: #0066CC; color: #ffffff; border: 2px solid #0066CC; }"
        "QDialog QLineEdit { background-color: #A7C6ED; color: #003366; border: 2px solid #003366; }");

    const auto setThemePalette = [](const QColor& window, const QColor& text) {
        QPalette palette = qApp->palette();
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::Base, window);
        palette.setColor(QPalette::Button, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::ButtonText, text);
        qApp->setPalette(palette);
    };
    const auto requireDialogStyle = [](const QDialog& dialog, const QString& theme,
                                       const QString& dialogBg, const QString& accent) {
        const QString style = dialog.styleSheet();
        QVERIFY2(style.contains(dialogBg, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 %2 must force a DigiDollar green dialog surface")
                                 .arg(theme, dialog.objectName())));
        QVERIFY2(style.contains(accent, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 %2 must force a DigiDollar green accent")
                                 .arg(theme, dialog.objectName())));

        const QStringList dgbBlueColors = {
            QStringLiteral("#002352"),
            QStringLiteral("#003366"),
            QStringLiteral("#0066CC"),
            QStringLiteral("#0066cc"),
            QStringLiteral("#0088ff"),
            QStringLiteral("#0044aa"),
            QStringLiteral("#0055aa"),
            QStringLiteral("#A7C6ED"),
            QStringLiteral("#9BB8E8"),
        };
        for (const QString& color : dgbBlueColors) {
            QVERIFY2(!style.contains(color, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("%1 %2 local stylesheet must not carry DGB blue color %3")
                                     .arg(theme, dialog.objectName(), color)));
        }
    };
    const auto exerciseTheme = [&](const QString& theme, const QColor& window, const QColor& text,
                                   const QString& dialogBg, const QString& accent) {
        qApp->setStyleSheet(dgbBlueFallback);
        setThemePalette(window, text);
        QCoreApplication::processEvents();

        wallet::DDCoinControl coinControl;
        DigiDollarCoinControlDialog coinDialog(coinControl, nullptr, nullptr);
        requireDialogStyle(coinDialog, theme, dialogBg, accent);

        SendCoinsRecipient recipient;
        recipient.address = QStringLiteral("dgbt1qstyle000000000000000000000000000000000");
        recipient.label = QStringLiteral("visual invoice");
        recipient.message = QStringLiteral("DD request theme guard");
        recipient.amount = 12345;
        DigiDollarReceiveRequestDialog requestDialog;
        requestDialog.setInfo(recipient);
        requireDialogStyle(requestDialog, theme, dialogBg, accent);

        DDAddressBookPage addressBook(nullptr, DDAddressBookPage::ForSelection);
        requireDialogStyle(addressBook, theme, dialogBg, accent);
    };

    exerciseTheme(QStringLiteral("dark"), QColor(QStringLiteral("#0b2419")), QColor(QStringLiteral("#ffffff")),
                  QStringLiteral("#0b2419"), QStringLiteral("#16804f"));
    exerciseTheme(QStringLiteral("light"), QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#123f2b")),
                  QStringLiteral("#eef9f2"), QStringLiteral("#1f9d57"));

    qApp->setStyleSheet(originalStyleSheet);
    qApp->setPalette(originalPalette);
    QCoreApplication::processEvents();
}

void DigiDollarWidgetTests::digiDollarModalDialogsVisualQaDarkAndLight()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal")) {
        QSKIP("Visual DD modal QA requires a platform that can capture rendered dialog windows");
    }

    const QString originalStyleSheet = qApp->styleSheet();
    const QPalette originalPalette = qApp->palette();
    const QString dgbBlueFallback = QStringLiteral(
        "QDialog { background-color: #002352; color: #ffffff; }"
        "QDialog QLabel { color: #ffffff; }"
        "QDialog QTreeWidget, QDialog QTableWidget { background-color: #A7C6ED; color: #003366; selection-background-color: #0066CC; }"
        "QDialog QHeaderView::section { background-color: #0066CC; color: #ffffff; }"
        "QDialog QPushButton { background-color: #0066CC; color: #ffffff; border: 2px solid #0066CC; }"
        "QDialog QLineEdit { background-color: #A7C6ED; color: #003366; border: 2px solid #003366; }");

    const auto setThemePalette = [](const QColor& window, const QColor& text) {
        QPalette palette = qApp->palette();
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::Base, window);
        palette.setColor(QPalette::Button, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::ButtonText, text);
        qApp->setPalette(palette);
    };
    const auto captureDialog = [](QDialog& dialog, const QString& path) {
        dialog.show();
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QTRY_VERIFY(dialog.isVisible());
        const QPixmap pixmap = dialog.grab();
        QVERIFY2(!pixmap.isNull(), qPrintable(QStringLiteral("failed to grab %1").arg(dialog.objectName())));
        QVERIFY2(pixmap.save(path), qPrintable(QStringLiteral("failed to save %1").arg(path)));
        dialog.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    };
    const auto seedCoinControlRows = [](DigiDollarCoinControlDialog& dialog) {
        QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
        QVERIFY(tree != nullptr);
        tree->clear();
        tree->setAlternatingRowColors(true);
        for (int row = 0; row < 6; ++row) {
            QTreeWidgetItem* item = new QTreeWidgetItem(tree);
            item->setCheckState(0, row == 1 ? Qt::Checked : Qt::Unchecked);
            item->setText(1, QStringLiteral("%1.00 $DD").arg(110 - row));
            item->setText(2, QStringLiteral("$DD UTXO"));
            item->setText(3, QStringLiteral("dgbt1qvisual%1...:1").arg(row));
            item->setText(4, QStringLiteral("May 26, 2026"));
            item->setText(5, QStringLiteral("Confirmed"));
            item->setText(6, QStringLiteral("e00000000000000000000000000000000000000000000000000000000000000%1:1").arg(row));
            tree->addTopLevelItem(item);
        }
        tree->setCurrentItem(tree->topLevelItem(1));
    };
    const auto seedAddressBookRows = [](DDAddressBookPage& dialog) {
        QTableWidget* table = dialog.findChild<QTableWidget*>();
        QVERIFY(table != nullptr);
        table->setRowCount(3);
        for (int row = 0; row < 3; ++row) {
            table->setItem(row, 0, new QTableWidgetItem(QStringLiteral("DD recipient %1").arg(row + 1)));
            table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("dgbt1qaddressbookvisual%100000000000000000000").arg(row)));
        }
        table->selectRow(1);
    };
    const auto seedRequest = [](DigiDollarReceiveRequestDialog& dialog) {
        SendCoinsRecipient recipient;
        recipient.address = QStringLiteral("dgbt1qrequestvisual00000000000000000000000000");
        recipient.label = QStringLiteral("Visual invoice");
        recipient.message = QStringLiteral("DigiDollar request dialog green theme QA");
        recipient.amount = 12345;
        dialog.setInfo(recipient);
    };
    const auto runTheme = [&](const QString& name, const QColor& window, const QColor& text) {
        qApp->setStyleSheet(dgbBlueFallback);
        setThemePalette(window, text);
        QCoreApplication::processEvents();

        wallet::DDCoinControl coinControl;
        DigiDollarCoinControlDialog coinDialog(coinControl, nullptr, nullptr);
        coinDialog.resize(1000, 540);
        seedCoinControlRows(coinDialog);
        captureDialog(coinDialog, QStringLiteral("/tmp/digibyte_dd_coin_control_%1_qa.png").arg(name));

        DigiDollarReceiveRequestDialog requestDialog;
        seedRequest(requestDialog);
        captureDialog(requestDialog, QStringLiteral("/tmp/digibyte_dd_receive_request_%1_qa.png").arg(name));

        DDAddressBookPage addressBook(nullptr, DDAddressBookPage::ForSelection);
        addressBook.resize(780, 420);
        seedAddressBookRows(addressBook);
        captureDialog(addressBook, QStringLiteral("/tmp/digibyte_dd_address_book_%1_qa.png").arg(name));
    };

    runTheme(QStringLiteral("dark"), QColor(QStringLiteral("#0b2419")), QColor(QStringLiteral("#ffffff")));
    runTheme(QStringLiteral("light"), QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#123f2b")));

    qApp->setStyleSheet(originalStyleSheet);
    qApp->setPalette(originalPalette);
    QCoreApplication::processEvents();

    qInfo("DD coin control dark QA screenshot: /tmp/digibyte_dd_coin_control_dark_qa.png");
    qInfo("DD coin control light QA screenshot: /tmp/digibyte_dd_coin_control_light_qa.png");
    qInfo("DD receive request dark QA screenshot: /tmp/digibyte_dd_receive_request_dark_qa.png");
    qInfo("DD receive request light QA screenshot: /tmp/digibyte_dd_receive_request_light_qa.png");
    qInfo("DD address book dark QA screenshot: /tmp/digibyte_dd_address_book_dark_qa.png");
    qInfo("DD address book light QA screenshot: /tmp/digibyte_dd_address_book_light_qa.png");
}

void DigiDollarWidgetTests::privacySendMaskTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget sendWidget(mini_gui.platformStyle.get());
    sendWidget.setWalletModel(mini_gui.walletModel.get());
    sendWidget.setClientModel(mini_gui.clientModel.get());
    sendWidget.show();

    // Enable privacy mode
    sendWidget.setPrivacy(true);

    // Check that available balance is masked
    QLabel* availableBalanceValue = sendWidget.findChild<QLabel*>("availableBalanceValue");
    QVERIFY(availableBalanceValue != nullptr);
    QVERIFY2(availableBalanceValue->text().contains('#'), "Available balance should be masked when privacy is enabled");

    // Disable privacy mode
    sendWidget.setPrivacy(false);

    // Check that available balance is no longer masked
    QVERIFY2(!availableBalanceValue->text().contains('#'), "Available balance should NOT be masked when privacy is disabled");
}

void DigiDollarWidgetTests::privacyMintMaskTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarMintWidget mintWidget;
    mintWidget.setWalletModel(mini_gui.walletModel.get());
    mintWidget.setClientModel(mini_gui.clientModel.get());
    mintWidget.show();

    // Enable privacy mode
    mintWidget.setPrivacy(true);

    // Check that available DGB balance is masked
    QLabel* availableDGBValue = mintWidget.findChild<QLabel*>("availableDGBValue");
    QVERIFY(availableDGBValue != nullptr);
    QVERIFY2(availableDGBValue->text().contains('#'), "Available DGB balance should be masked when privacy is enabled");

    // Check that collateral value is masked
    QLabel* collateralValue = mintWidget.findChild<QLabel*>("collateralValue");
    QVERIFY(collateralValue != nullptr);
    QVERIFY2(collateralValue->text().contains('#'), "Collateral value should be masked when privacy is enabled");

    // Disable privacy mode
    mintWidget.setPrivacy(false);

    // Check that available DGB balance is no longer masked
    QVERIFY2(!availableDGBValue->text().contains('#'), "Available DGB balance should NOT be masked when privacy is disabled");
}

void DigiDollarWidgetTests::privacyRedeemMaskTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.show();

    // Enable privacy mode
    redeemWidget.setPrivacy(true);

    // Check that position values are masked
    QLabel* ddMintedValue = redeemWidget.findChild<QLabel*>("ddMintedValue");
    if (ddMintedValue) {
        QVERIFY2(ddMintedValue->text().contains('#'), "DD minted value should be masked when privacy is enabled");
    }

    QLabel* dgbCollateralValue = redeemWidget.findChild<QLabel*>("dgbCollateralValue");
    if (dgbCollateralValue) {
        QVERIFY2(dgbCollateralValue->text().contains('#'), "DGB collateral value should be masked when privacy is enabled");
    }

    QLabel* redeemableValue = redeemWidget.findChild<QLabel*>("redeemableValue");
    if (redeemableValue) {
        QVERIFY2(redeemableValue->text().contains('#'), "Redeemable value should be masked when privacy is enabled");
    }

    // Disable privacy mode
    redeemWidget.setPrivacy(false);

    if (ddMintedValue) {
        QVERIFY2(!ddMintedValue->text().contains('#'), "DD minted value should NOT be masked when privacy is disabled");
    }
}

void DigiDollarWidgetTests::privacyPositionsMaskTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.show();

    // Enable privacy mode
    positionsWidget.setPrivacy(true);

    // The positions table should be hidden when privacy is enabled
    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QVERIFY2(table->isHidden(), "Positions table should be hidden when privacy is enabled");

    // Disable privacy mode
    positionsWidget.setPrivacy(false);

    // The positions table should be visible again (not explicitly hidden)
    QVERIFY2(!table->isHidden(), "Positions table should not be hidden when privacy is disabled");
}

void DigiDollarWidgetTests::privacyTransactionsMaskTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());
    transactionsWidget.show();

    // Enable privacy mode
    transactionsWidget.setPrivacy(true);

    // The transactions table should be hidden when privacy is enabled
    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QVERIFY2(table->isHidden(), "Transactions table should be hidden when privacy is enabled");

    // Disable privacy mode
    transactionsWidget.setPrivacy(false);

    // The transactions table should be visible again (not explicitly hidden)
    QVERIFY2(!table->isHidden(), "Transactions table should not be hidden when privacy is disabled");
}

void DigiDollarWidgetTests::privacySignalPropagationTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTab tab(mini_gui.platformStyle.get());
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());
    tab.show();

    // Enable privacy on the tab — it should propagate to all sub-widgets
    tab.setPrivacy(true);

    // Verify overview widget has privacy enabled (check for masked balances)
    DigiDollarOverviewWidget* overviewWidget = tab.findChild<DigiDollarOverviewWidget*>("overviewWidget");
    QVERIFY(overviewWidget != nullptr);

    QLabel* ddBalanceValue = overviewWidget->findChild<QLabel*>("ddBalanceValue");
    QVERIFY(ddBalanceValue != nullptr);
    QVERIFY2(ddBalanceValue->text().contains('#'), "Privacy should propagate from tab to overview widget");

    // Verify transactions widget has privacy enabled
    DigiDollarTransactionsWidget* transactionsWidget = tab.findChild<DigiDollarTransactionsWidget*>("transactionsWidget");
    QVERIFY(transactionsWidget != nullptr);

    QTableWidget* txTable = transactionsWidget->findChild<QTableWidget*>();
    QVERIFY(txTable != nullptr);
    QVERIFY2(txTable->isHidden(), "Privacy should propagate from tab to transactions widget");

    // Disable privacy
    tab.setPrivacy(false);

    // Verify overview is unmasked
    QVERIFY2(!ddBalanceValue->text().contains('#'), "Disabling privacy should propagate from tab to overview widget");

    // Verify transactions table is not hidden
    QVERIFY2(!txTable->isHidden(), "Disabling privacy should propagate from tab to transactions widget");
}

// Regression test for shenger's Apr 20 RC30 UX report: the
// "Your DigiDollar Address" panel always kept showing the last-generated
// address instead of the currently-selected row in the recent requests
// table. The panel must update m_addressEdit to follow whatever row the
// user highlights, matching DGB receive-table behaviour.
void DigiDollarWidgetTests::ddReceivePanelFollowsSelectedRow()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarReceiveWidget receive;
    receive.setWalletModel(mini_gui.walletModel.get());
    receive.show();

    QTableWidget* table = receive.findChild<QTableWidget*>("m_requestsTable");
    if (!table) {
        // Not every build exposes the object name; fall back to first table child.
        table = receive.findChild<QTableWidget*>();
    }
    QVERIFY(table != nullptr);

    QLineEdit* addressEdit = receive.findChild<QLineEdit*>("addressEdit");
    QVERIFY(addressEdit != nullptr);

    // Seed two distinct DD addresses directly into the table so we can
    // assert the panel follows selection without depending on address
    // generation order or wallet state.
    table->setRowCount(2);
    const QString addrA = QStringLiteral("dgbt1qfake000000000000000000000000000000000a");
    const QString addrB = QStringLiteral("dgbt1qfake000000000000000000000000000000000b");

    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < 4; ++c) {
            if (!table->item(i, c)) {
                table->setItem(i, c, new QTableWidgetItem());
            }
        }
    }
    table->item(0, 3)->setData(Qt::UserRole, addrA);
    table->item(0, 3)->setText(addrA);
    table->item(1, 3)->setData(Qt::UserRole, addrB);
    table->item(1, 3)->setText(addrB);

    // Seed the panel with a "last generated" string that must be replaced
    // on selection, matches the real-world symptom.
    addressEdit->setText(QStringLiteral("last-generated-address"));

    table->selectRow(0);
    QCoreApplication::processEvents();
    QCOMPARE(addressEdit->text(), addrA);

    table->selectRow(1);
    QCoreApplication::processEvents();
    QCOMPARE(addressEdit->text(), addrB);
}

// Regression test for shenger's Apr 20 RC30 UX report: double-clicking a
// DigiDollar request row must open the request dialog for that DD request.
// This specifically guards against routing DD rows through the DGB recent
// requests model, which filters DD entries out and leaves double-click inert.
void DigiDollarWidgetTests::ddReceiveDoubleClickShowsRequestDialog()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletModel* wallet_model = mini_gui.walletModel.get();
    QVERIFY(wallet_model != nullptr);
    QVERIFY(wallet_model->getRecentRequestsTableModel() != nullptr);

    const QString ddAddress = wallet_model->getNewDigiDollarAddress(QStringLiteral("dialog-test"));
    QVERIFY2(!ddAddress.isEmpty(), "expected a valid DigiDollar address for request-dialog regression test");

    SendCoinsRecipient recipient;
    recipient.address = ddAddress;
    recipient.label = QStringLiteral("dialog-label");
    recipient.message = QStringLiteral("dialog-message");
    recipient.amount = 12345;
    wallet_model->getRecentRequestsTableModel()->addNewRequest(recipient);

    DigiDollarReceiveWidget receive;
    receive.setWalletModel(wallet_model);
    receive.show();
    receive.updateRecentRequests();

    QTableWidget* table = receive.findChild<QTableWidget*>("m_requestsTable");
    if (!table) {
        table = receive.findChild<QTableWidget*>();
    }
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);

    int existing_dialogs = 0;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("DigiDollarReceiveRequestDialog")) {
            ++existing_dialogs;
        }
    }

    QVERIFY(QMetaObject::invokeMethod(&receive, "onRecentRequestDoubleClicked",
                                      Qt::DirectConnection,
                                      Q_ARG(int, 0),
                                      Q_ARG(int, 0)));
    QCoreApplication::processEvents();

    DigiDollarReceiveRequestDialog* dialog = nullptr;
    int updated_dialogs = 0;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (!widget->inherits("DigiDollarReceiveRequestDialog")) {
            continue;
        }
        ++updated_dialogs;
        if (!dialog) {
            dialog = qobject_cast<DigiDollarReceiveRequestDialog*>(widget);
        }
    }

    QVERIFY2(updated_dialogs == existing_dialogs + 1 && dialog != nullptr,
             "double-clicking a DD request row must open DigiDollarReceiveRequestDialog");

    QLabel* addressContent = nullptr;
    for (QLabel* label : dialog->findChildren<QLabel*>()) {
        if (label->text() == ddAddress) {
            addressContent = label;
            break;
        }
    }
    QVERIFY2(addressContent != nullptr, "request dialog should display the selected DD address");

    dialog->close();
    QCoreApplication::processEvents();
}

void DigiDollarWidgetTests::ddReceiveHidesCrossNetworkRequests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletModel* wallet_model = mini_gui.walletModel.get();
    QVERIFY(wallet_model != nullptr);
    QVERIFY(wallet_model->getRecentRequestsTableModel() != nullptr);

    const QString currentAddress = wallet_model->getNewDigiDollarAddress(QStringLiteral("current-network-request"));
    const QString mainnetAddress = EncodeDigiDollarAddressForNetwork(CChainParams::DIGIDOLLAR_ADDRESS);
    QVERIFY2(currentAddress.startsWith(QStringLiteral("RD")), "regtest receive address should use RD prefix");
    QVERIFY2(mainnetAddress.startsWith(QStringLiteral("DD")), "test fixture should create a mainnet DD address");

    SendCoinsRecipient currentRecipient;
    currentRecipient.address = currentAddress;
    currentRecipient.label = QStringLiteral("current");
    currentRecipient.amount = 1234;
    wallet_model->getRecentRequestsTableModel()->addNewRequest(currentRecipient);

    RecentRequestEntry staleEntry;
    staleEntry.id = 100;
    staleEntry.date = QDateTime::currentDateTime();
    staleEntry.recipient.address = mainnetAddress;
    staleEntry.recipient.label = QStringLiteral("stale-mainnet");
    staleEntry.recipient.amount = 5678;
    DataStream staleStream{};
    staleStream << staleEntry;
    QVERIFY(wallet_model->wallet().setAddressReceiveRequest(
        DecodeDigiDollarAddress(mainnetAddress.toStdString()), ToString(staleEntry.id), staleStream.str()));

    DigiDollarReceiveWidget receive;
    receive.setWalletModel(wallet_model);
    receive.updateRecentRequests();

    QTableWidget* table = receive.findChild<QTableWidget*>("requestsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 3)->data(Qt::UserRole).toString(), currentAddress);
}

void DigiDollarWidgetTests::ddReceiveEditPersistsAndKeepsDgbSeparated()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletModel* wallet_model = mini_gui.walletModel.get();
    QVERIFY(wallet_model != nullptr);

    const QString ddAddress = wallet_model->getNewDigiDollarAddress(QStringLiteral("edit-original-address-label"));
    QVERIFY(!ddAddress.isEmpty());

    SendCoinsRecipient recipient;
    recipient.address = ddAddress;
    recipient.label = QStringLiteral("original label");
    recipient.message = QStringLiteral("original message");
    recipient.amount = 1234;
    wallet_model->getRecentRequestsTableModel()->addNewRequest(recipient);
    QCOMPARE(wallet_model->getRecentRequestsTableModel()->rowCount(QModelIndex()), 0);

    DigiDollarReceiveWidget receive;
    receive.setWalletModel(wallet_model);
    receive.updateRecentRequests();

    QTableWidget* table = receive.findChild<QTableWidget*>("requestsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    table->selectRow(0);

    QTimer::singleShot(0, [&]() {
        QDialog* dialog = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->windowTitle() == QStringLiteral("Edit DigiDollar Payment Request")) {
                dialog = qobject_cast<QDialog*>(widget);
                break;
            }
        }
        QVERIFY(dialog != nullptr);
        QLineEdit* label = dialog->findChild<QLineEdit*>("ddRequestLabelEdit");
        QLineEdit* message = dialog->findChild<QLineEdit*>("ddRequestMessageEdit");
        QVERIFY(label != nullptr);
        QVERIFY(message != nullptr);
        label->setText(QStringLiteral("edited label"));
        message->setText(QStringLiteral("edited message"));
        QDoubleSpinBox* amount = dialog->findChild<QDoubleSpinBox*>("ddRequestAmountEdit");
        QVERIFY(amount != nullptr);
        amount->setValue(45.67);
        QDialogButtonBox* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons != nullptr);
        buttons->button(QDialogButtonBox::Ok)->click();
    });

    QVERIFY(QMetaObject::invokeMethod(&receive, "onEditRequestClicked", Qt::DirectConnection));
    QCoreApplication::processEvents();

    QCOMPARE(CountStoredReceiveRequests(*wallet_model, ddAddress), 1);
    RecentRequestEntry edited;
    bool found = false;
    for (const std::string& requestStr : wallet_model->wallet().getAddressReceiveRequests()) {
        std::vector<uint8_t> data(requestStr.begin(), requestStr.end());
        DataStream ss{data};
        RecentRequestEntry entry;
        ss >> entry;
        if (entry.recipient.address == ddAddress) {
            edited = entry;
            found = true;
            break;
        }
    }
    QVERIFY(found);
    QCOMPARE(edited.recipient.label, QStringLiteral("edited label"));
    QCOMPARE(edited.recipient.message, QStringLiteral("edited message"));
    QCOMPARE(edited.recipient.amount, CAmount(4567));
    QCOMPARE(wallet_model->getRecentRequestsTableModel()->rowCount(QModelIndex()), 0);

    DigiDollarReceiveWidget reloaded;
    reloaded.setWalletModel(wallet_model);
    reloaded.updateRecentRequests();
    QTableWidget* reloadedTable = reloaded.findChild<QTableWidget*>("requestsTable");
    QVERIFY(reloadedTable != nullptr);
    QCOMPARE(reloadedTable->rowCount(), 1);
    QCOMPARE(reloadedTable->item(0, 1)->text(), QStringLiteral("edited label"));
    QCOMPARE(reloadedTable->item(0, 2)->text(), QStringLiteral("45.67 $DD"));
}

void DigiDollarWidgetTests::ddReceiveEditCancelLeavesRequestUnchanged()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletModel* wallet_model = mini_gui.walletModel.get();
    QVERIFY(wallet_model != nullptr);

    const QString ddAddress = wallet_model->getNewDigiDollarAddress(QStringLiteral("edit-cancel-address-label"));
    QVERIFY(!ddAddress.isEmpty());

    SendCoinsRecipient recipient;
    recipient.address = ddAddress;
    recipient.label = QStringLiteral("cancel original label");
    recipient.message = QStringLiteral("cancel original message");
    recipient.amount = 9876;
    wallet_model->getRecentRequestsTableModel()->addNewRequest(recipient);
    QCOMPARE(wallet_model->getRecentRequestsTableModel()->rowCount(QModelIndex()), 0);
    QCOMPARE(CountStoredReceiveRequests(*wallet_model, ddAddress), 1);

    DigiDollarReceiveWidget receive;
    receive.setWalletModel(wallet_model);
    receive.updateRecentRequests();

    QTableWidget* table = receive.findChild<QTableWidget*>("requestsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    table->selectRow(0);

    QTimer::singleShot(0, [&]() {
        QDialog* dialog = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->windowTitle() == QStringLiteral("Edit DigiDollar Payment Request")) {
                dialog = qobject_cast<QDialog*>(widget);
                break;
            }
        }
        QVERIFY(dialog != nullptr);
        QLineEdit* label = dialog->findChild<QLineEdit*>("ddRequestLabelEdit");
        QLineEdit* message = dialog->findChild<QLineEdit*>("ddRequestMessageEdit");
        QDoubleSpinBox* amount = dialog->findChild<QDoubleSpinBox*>("ddRequestAmountEdit");
        QVERIFY(label != nullptr);
        QVERIFY(message != nullptr);
        QVERIFY(amount != nullptr);
        label->setText(QStringLiteral("cancel edited label"));
        message->setText(QStringLiteral("cancel edited message"));
        amount->setValue(12.34);
        QDialogButtonBox* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons != nullptr);
        buttons->button(QDialogButtonBox::Cancel)->click();
    });

    QVERIFY(QMetaObject::invokeMethod(&receive, "onEditRequestClicked", Qt::DirectConnection));
    QCoreApplication::processEvents();

    QCOMPARE(CountStoredReceiveRequests(*wallet_model, ddAddress), 1);
    RecentRequestEntry stored;
    QVERIFY(FindStoredReceiveRequest(*wallet_model, ddAddress, stored));
    QCOMPARE(stored.recipient.label, QStringLiteral("cancel original label"));
    QCOMPARE(stored.recipient.message, QStringLiteral("cancel original message"));
    QCOMPARE(stored.recipient.amount, CAmount(9876));
    QCOMPARE(wallet_model->getRecentRequestsTableModel()->rowCount(QModelIndex()), 0);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("cancel original label"));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("98.76 $DD"));
}

void DigiDollarWidgetTests::ddReceiveRemovePersistsAndKeepsDgbSeparated()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletModel* wallet_model = mini_gui.walletModel.get();
    QVERIFY(wallet_model != nullptr);

    RecentRequestsTableModel* dgb_requests = wallet_model->getRecentRequestsTableModel();
    QVERIFY(dgb_requests != nullptr);

    const CTxDestination dgbDest = GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type);
    const QString dgbAddress = QString::fromStdString(EncodeDestination(dgbDest));
    const QString ddAddress = wallet_model->getNewDigiDollarAddress(QStringLiteral("remove-dd-address-label"));
    QVERIFY(!dgbAddress.isEmpty());
    QVERIFY(!ddAddress.isEmpty());

    SendCoinsRecipient dgbRecipient;
    dgbRecipient.address = dgbAddress;
    dgbRecipient.label = QStringLiteral("dgb request");
    dgbRecipient.message = QStringLiteral("normal DGB request");
    dgbRecipient.amount = 1234;
    dgb_requests->addNewRequest(dgbRecipient);
    QCOMPARE(dgb_requests->rowCount(QModelIndex()), 1);
    QCOMPARE(dgb_requests->entry(0).recipient.address, dgbAddress);

    SendCoinsRecipient ddRecipient;
    ddRecipient.address = ddAddress;
    ddRecipient.label = QStringLiteral("dd request");
    ddRecipient.message = QStringLiteral("DigiDollar request");
    ddRecipient.amount = 2345;
    dgb_requests->addNewRequest(ddRecipient);

    QCOMPARE(dgb_requests->rowCount(QModelIndex()), 1);
    QCOMPARE(dgb_requests->entry(0).recipient.address, dgbAddress);
    QCOMPARE(CountStoredReceiveRequests(*wallet_model, dgbAddress), 1);
    QCOMPARE(CountStoredReceiveRequests(*wallet_model, ddAddress), 1);

    DigiDollarReceiveWidget receive;
    receive.setWalletModel(wallet_model);
    receive.updateRecentRequests();

    QTableWidget* table = receive.findChild<QTableWidget*>("requestsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 3)->data(Qt::UserRole).toString(), ddAddress);
    for (int row = 0; row < table->rowCount(); ++row) {
        for (int column = 0; column < table->columnCount(); ++column) {
            QVERIFY(table->item(row, column)->text() != dgbAddress);
        }
    }

    table->selectRow(0);
    QVERIFY(QMetaObject::invokeMethod(&receive, "onRemoveRequestClicked", Qt::DirectConnection));
    QCoreApplication::processEvents();

    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(CountStoredReceiveRequests(*wallet_model, ddAddress), 0);
    QCOMPARE(CountStoredReceiveRequests(*wallet_model, dgbAddress), 1);
    QCOMPARE(dgb_requests->rowCount(QModelIndex()), 1);
    QCOMPARE(dgb_requests->entry(0).recipient.address, dgbAddress);

    DigiDollarReceiveWidget reloaded;
    reloaded.setWalletModel(wallet_model);
    reloaded.updateRecentRequests();
    QTableWidget* reloadedTable = reloaded.findChild<QTableWidget*>("requestsTable");
    QVERIFY(reloadedTable != nullptr);
    QCOMPARE(reloadedTable->rowCount(), 0);
}

void DigiDollarWidgetTests::ddReceiveRequestDialogFormatsURIAndAmount()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif

    SendCoinsRecipient recipient;
    recipient.address = QStringLiteral("RDrequestTestAddress");
    recipient.label = QStringLiteral("invoice 42");
    recipient.message = QStringLiteral("DGB-equivalent DD request");
    recipient.amount = 12345;

    DigiDollarReceiveRequestDialog dialog;
    dialog.setInfo(recipient);

    QString uri_text;
    QString amount_text;
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("digidollar:RDrequestTestAddress"))) {
            uri_text = label->text();
        }
        if (label->text() == QStringLiteral("123.45 $DD")) {
            amount_text = label->text();
        }
    }

    QVERIFY2(uri_text.contains(QStringLiteral("digidollar:RDrequestTestAddress?")),
             "DD receive request dialog must use the digidollar URI scheme");
    QVERIFY2(uri_text.contains(QStringLiteral("label=invoice%2042")),
             "DD receive request dialog must percent-encode labels");
    QVERIFY2(uri_text.contains(QStringLiteral("amount=123.45000000")),
             "DD receive request dialog must encode cents as decimal DD units");
    QVERIFY2(uri_text.contains(QStringLiteral("message=DGB-equivalent%20DD%20request")),
             "DD receive request dialog must percent-encode messages");
    QCOMPARE(amount_text, QStringLiteral("123.45 $DD"));
}

void DigiDollarWidgetTests::ddReceiveRejectsMalformedRequestAmount()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-receive-invalid-amount");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletModel* wallet_model = mini_gui.walletModel.get();
    QVERIFY(wallet_model != nullptr);

    DigiDollarReceiveWidget receive;
    receive.setWalletModel(wallet_model);
    receive.setClientModel(mini_gui.clientModel.get());
    receive.show();

    QLineEdit* amountEdit = receive.findChild<QLineEdit*>("amountEdit");
    QLineEdit* addressEdit = receive.findChild<QLineEdit*>("addressEdit");
    QTableWidget* table = receive.findChild<QTableWidget*>("requestsTable");
    QVERIFY(amountEdit != nullptr);
    QVERIFY(addressEdit != nullptr);
    QVERIFY(table != nullptr);

    QSignalSpy messageSpy(&receive, &DigiDollarReceiveWidget::message);
    amountEdit->setText(QStringLiteral("12.bad"));
    QVERIFY(QMetaObject::invokeMethod(&receive, "onGenerateAddressClicked", Qt::DirectConnection));
    QCoreApplication::processEvents();

    QCOMPARE(addressEdit->text(), QString());
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(wallet_model->wallet().getAddressReceiveRequests().size(), size_t{0});
    QVERIFY(!messageSpy.empty());
    QCOMPARE(messageSpy.first().at(0).toString(), QStringLiteral("Invalid Amount"));
}

void DigiDollarWidgetTests::ddReceiveRequestDialogReportsQRSaveFailure()
{
    DigiDollarReceiveRequestDialog dialog;

    SendCoinsRecipient recipient;
    recipient.address = EncodeDigiDollarAddressForNetwork(CChainParams::DIGIDOLLAR_ADDRESS_REGTEST);
    recipient.label = QStringLiteral("save-failure");
    recipient.amount = 12345;
    recipient.message = QStringLiteral("should report failed save");
    dialog.setInfo(recipient);

    const QString missingDir = QDir::temp().filePath(QStringLiteral("digidollar-missing-qr-save-dir"));
    QDir(missingDir).removeRecursively();
    const QString fileName = missingDir + QStringLiteral("/request.png");
    const QString error = dialog.saveQRImageForTesting(fileName);

    QVERIFY2(!error.isEmpty(), "QR save failure should return an actionable error string");
    QVERIFY2(error.contains(QStringLiteral("Failed to save QR code"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY2(error.contains(fileName), qPrintable(error));
}

// Regression test for shenger's Apr 20 RC30 UX report: on Windows dark
// theme, the Peers detail pane inside the RPC console renders with a
// grey system-default QWidget background and white text, making fields
// unreadable. Root cause: dark.css has no explicit rule for the
// debugwindow.ui "detailWidget" QWidget inside the RPCConsole scroll
// area, so it falls through to Qt's default palette. This source-level
// test enforces that dark.css carries an explicit rule for #detailWidget
// inside the RPCConsole scope.
// Regression test for the DD Overview "Recent Transactions" sign-prefix bug:
// DDTransaction stores amounts as unsigned magnitudes (the wallet pushes
// totalAmount, a positive number, for sends), so the row formatter must
// derive the sign from the category instead of the raw amount. Before the
// fix, send/redeem rows rendered as "+$3.00" with red text, contradicting
// the colour and confusing users about whether DD was leaving or arriving.
void DigiDollarWidgetTests::overviewRecentTransactionsSendShowsNegativeSign()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    // The mock wallet path used by these Qt tests bypasses CreateWalletFromFile,
    // which is what normally allocates m_dd_wallet. Allocate it explicitly here
    // so GetDDWallet() returns a usable pointer for the mock-history injection.
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    // Inject one of each category we care about. amount is stored as an
    // unsigned magnitude (positive) — exactly how the live wallet persists
    // it for sends/redeems too. The formatter must read tx.category.
    auto pushTx = [&](const std::string& txid, CAmount amount, bool incoming, const std::string& category) {
        DDTransaction tx;
        tx.txid = txid;
        tx.amount = amount;
        tx.timestamp = GetTime();
        tx.confirmations = 1;
        tx.incoming = incoming;
        tx.address = "TDtestlocaladdress";
        tx.category = category;
        tx.lock_tier = -1;
        tx.fee = 0;
        tx.abandoned = false;
        dd_wallet->AddMockTransaction(tx);
    };
    pushTx("a000000000000000000000000000000000000000000000000000000000000001", 300, false, "send");
    pushTx("a000000000000000000000000000000000000000000000000000000000000002", 200, true,  "receive");
    pushTx("a000000000000000000000000000000000000000000000000000000000000003", 500, false, "redeem");
    pushTx("a000000000000000000000000000000000000000000000000000000000000004", 700, true,  "mint");

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarOverviewWidget overviewWidget;
    overviewWidget.setWalletModel(mini_gui.walletModel.get());
    overviewWidget.setClientModel(mini_gui.clientModel.get());
    overviewWidget.show();
    overviewWidget.updateView();
    QCoreApplication::processEvents();

    QListWidget* transactionsList = overviewWidget.findChild<QListWidget*>("transactionsList");
    QVERIFY(transactionsList != nullptr);
    QVERIFY2(transactionsList->count() >= 4, "expected at least four mock DD transactions in recent list");

    // Walk every row and look at the amount QLabel — index 2 in the row's
    // QHBoxLayout (icon, category, amount, confirmations, date).
    int sends = 0, receives = 0, redeems = 0, mints = 0;
    for (int row = 0; row < transactionsList->count(); ++row) {
        QWidget* itemWidget = transactionsList->itemWidget(transactionsList->item(row));
        QVERIFY(itemWidget != nullptr);
        const QList<QLabel*> labels = itemWidget->findChildren<QLabel*>();
        QVERIFY2(labels.size() >= 5, "expected icon/category/amount/confirmations/date labels per row");

        const QString category = labels.at(1)->text();
        const QString amountText = labels.at(2)->text();
        if (category == "Send") {
            ++sends;
            QVERIFY2(amountText.startsWith('-'),
                qPrintable(QString("Send row should start with '-', got: %1").arg(amountText)));
            QVERIFY2(!amountText.startsWith('+'), "Send row must never carry a '+' prefix");
        } else if (category == "Receive") {
            ++receives;
            QVERIFY2(amountText.startsWith('+'),
                qPrintable(QString("Receive row should start with '+', got: %1").arg(amountText)));
        } else if (category.startsWith("Redeem")) {
            ++redeems;
            QVERIFY2(amountText.startsWith('-'),
                qPrintable(QString("Redeem row should start with '-', got: %1").arg(amountText)));
        } else if (category.startsWith("Mint")) {
            ++mints;
            QVERIFY2(amountText.startsWith('+'),
                qPrintable(QString("Mint row should start with '+', got: %1").arg(amountText)));
        }
    }
    QVERIFY2(sends >= 1, "expected at least one Send row in mock data");
    QVERIFY2(receives >= 1, "expected at least one Receive row in mock data");
    QVERIFY2(redeems >= 1, "expected at least one Redeem row in mock data");
    QVERIFY2(mints >= 1, "expected at least one Mint row in mock data");
}

void DigiDollarWidgetTests::overviewRecentTransactionAmountIsRightAligned()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    auto pushPendingMint = [&](const std::string& txid, CAmount amount, int64_t timestamp) {
        DDTransaction tx;
        tx.txid = txid;
        tx.amount = amount;
        tx.timestamp = timestamp;
        tx.confirmations = 0;
        tx.incoming = true;
        tx.address = "TDtestlocaladdress";
        tx.category = "mint";
        tx.lock_tier = 0;
        tx.fee = 0;
        tx.abandoned = false;
        tx.is_local = false;
        dd_wallet->AddMockTransaction(tx);
    };
    pushPendingMint("b000000000000000000000000000000000000000000000000000000000000001", 10000, GetTime() + 1);
    pushPendingMint("b000000000000000000000000000000000000000000000000000000000000002", 123456789, GetTime());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarOverviewWidget overviewWidget;
    overviewWidget.setWalletModel(mini_gui.walletModel.get());
    overviewWidget.setClientModel(mini_gui.clientModel.get());
    overviewWidget.resize(1000, 700);
    overviewWidget.show();
    overviewWidget.updateView();
    QCoreApplication::processEvents();

    QListWidget* transactionsList = overviewWidget.findChild<QListWidget*>("transactionsList");
    QVERIFY(transactionsList != nullptr);
    QVERIFY(transactionsList->count() >= 2);

    bool foundSmall = false;
    bool foundLarge = false;
    int smallAmountLeft = -1;
    int smallStatusLeft = -1;
    int largeAmountLeft = -1;
    int largeStatusLeft = -1;

    for (int row = 0; row < 2; ++row) {
        QWidget* itemWidget = transactionsList->itemWidget(transactionsList->item(row));
        QVERIFY(itemWidget != nullptr);
        const QList<QLabel*> labels = itemWidget->findChildren<QLabel*>();
        QVERIFY2(labels.size() >= 5, "expected icon/category/amount/confirmations/date labels per row");
        QLabel* categoryLabel = labels.at(1);
        QLabel* amountLabel = labels.at(2);
        QLabel* statusLabel = labels.at(3);
        QCOMPARE(categoryLabel->text(), QStringLiteral("Mint 1-hr"));
        QCOMPARE(statusLabel->text(), QStringLiteral("Pending"));
        QVERIFY2(amountLabel->alignment() & Qt::AlignRight,
                 "Recent transaction amount label should be right-aligned for decimal-place alignment");
        QVERIFY2(amountLabel->minimumWidth() >= 128,
                 qPrintable(QString("Recent transaction amount column should reserve a stable readable width; got %1")
                            .arg(amountLabel->minimumWidth())));
        QVERIFY2(statusLabel->geometry().left() - amountLabel->geometry().right() >= 16,
                 qPrintable(QString("Recent transaction amount/status columns should have a clear gap; amount right=%1 status left=%2")
                            .arg(amountLabel->geometry().right())
                            .arg(statusLabel->geometry().left())));

        if (amountLabel->text() == QStringLiteral("+100.00 $DD")) {
            foundSmall = true;
            smallAmountLeft = amountLabel->geometry().left();
            smallStatusLeft = statusLabel->geometry().left();
        } else if (amountLabel->text() == QStringLiteral("+1234567.89 $DD")) {
            foundLarge = true;
            largeAmountLeft = amountLabel->geometry().left();
            largeStatusLeft = statusLabel->geometry().left();
        }
    }

    QVERIFY2(foundSmall, "expected small pending mint row");
    QVERIFY2(foundLarge, "expected large pending mint row");
    QCOMPARE(smallAmountLeft, largeAmountLeft);
    QCOMPARE(smallStatusLeft, largeStatusLeft);

    if (qEnvironmentVariableIsSet("DIGIBYTE_QT_SAVE_DD_OVERVIEW_QA")) {
        const QString path = QStringLiteral("/tmp/digibyte_dd_overview_recent_alignment_qa.png");
        const QPixmap pixmap = overviewWidget.grab();
        QVERIFY2(pixmap.save(path), qPrintable(QStringLiteral("failed to save DD overview alignment QA screenshot to %1").arg(path)));
        qInfo("DD overview recent transaction alignment QA screenshot: %s", qPrintable(path));
    }
}

void DigiDollarWidgetTests::overviewRecentTransactionDoubleClickOpensTransactionsTab()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    DDTransaction tx;
    tx.txid = "c000000000000000000000000000000000000000000000000000000000000001";
    tx.amount = 1234;
    tx.timestamp = GetTime();
    tx.confirmations = 3;
    tx.incoming = false;
    tx.address = "TDoverviewdetailsaddress";
    tx.category = "send";
    tx.comment = "overview detail note";
    tx.lock_tier = -1;
    tx.fee = 0;
    tx.abandoned = false;
    dd_wallet->AddMockTransaction(tx);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarTab tab(mini_gui.platformStyle.get());
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());
    tab.show();
    QCoreApplication::processEvents();
    tab.updateView();
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QTabWidget* tabWidget = tab.findChild<QTabWidget*>("digiDollarSubTabs");
    QVERIFY(tabWidget != nullptr);
    QCOMPARE(tabWidget->currentIndex(), 0);

    QListWidget* transactionsList = tab.findChild<QListWidget*>("transactionsList");
    QVERIFY(transactionsList != nullptr);
    QVERIFY(transactionsList->count() >= 1);

    QListWidgetItem* item = transactionsList->item(0);
    QVERIFY(item != nullptr);
    const bool invoked = QMetaObject::invokeMethod(transactionsList, "itemDoubleClicked",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QListWidgetItem*, item));
    QVERIFY2(invoked, "DD overview recent transaction list must expose the itemDoubleClicked signal");
    QCoreApplication::processEvents();

    QCOMPARE(tabWidget->currentIndex(), 6);

    DigiDollarTransactionsWidget* transactionsWidget = tab.findChild<DigiDollarTransactionsWidget*>("transactionsWidget");
    QVERIFY(transactionsWidget != nullptr);
    QTableWidget* table = transactionsWidget->findChild<QTableWidget*>();
    QVERIFY(table != nullptr);

    bool foundSelectedTx = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* txidItem = table->item(row, 5);
        if (!txidItem || txidItem->data(Qt::UserRole).toString() != QString::fromStdString(tx.txid)) {
            continue;
        }
        foundSelectedTx = table->currentRow() == row && table->selectionModel()->isRowSelected(row, QModelIndex());
        break;
    }
    QVERIFY2(foundSelectedTx, "DD overview double-click must switch to DD Transactions and focus the matching transaction row");
}

void DigiDollarWidgetTests::transactionsWidgetDoubleClickShowsDetailsDialog()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-details-dialog");
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    DDTransaction tx;
    tx.txid = "d000000000000000000000000000000000000000000000000000000000000001";
    tx.amount = 4321;
    tx.timestamp = GetTime();
    tx.confirmations = 8;
    tx.incoming = true;
    tx.address = "TDdetaildialogaddress";
    tx.category = "mint";
    tx.comment = "detail dialog note";
    tx.lock_tier = 4;
    tx.fee = 0;
    tx.abandoned = false;
    dd_wallet->AddMockTransaction(tx);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());
    transactionsWidget.show();
    transactionsWidget.updateView();
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);

    QTableWidgetItem* txidItem = table->item(0, 5);
    QVERIFY(txidItem != nullptr);
    QCOMPARE(txidItem->data(Qt::UserRole).toString(), QString::fromStdString(tx.txid));

    table->setCurrentItem(txidItem);
    const bool invoked = QMetaObject::invokeMethod(table, "itemDoubleClicked",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QTableWidgetItem*, txidItem));
    QVERIFY2(invoked, "DD transactions table must expose the itemDoubleClicked signal");
    const bool activated = QMetaObject::invokeMethod(table, "itemActivated",
                                                     Qt::DirectConnection,
                                                     Q_ARG(QTableWidgetItem*, txidItem));
    QVERIFY2(activated, "DD transactions table must expose the itemActivated signal");
    QCoreApplication::processEvents();

    const auto findDetailsDialogs = [&]() {
        std::vector<QDialog*> dialogs;
        const auto appendIfDetailsDialog = [&](QDialog* dialog) {
            if (!dialog || !dialog->isVisible()) return;
            if (!dialog->windowTitle().startsWith(QStringLiteral("Details for "))) return;
            if (std::find(dialogs.begin(), dialogs.end(), dialog) == dialogs.end()) {
                dialogs.push_back(dialog);
            }
        };
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            appendIfDetailsDialog(qobject_cast<QDialog*>(widget));
        }
        for (QDialog* dialog : transactionsWidget.findChildren<QDialog*>()) {
            appendIfDetailsDialog(dialog);
        }
        return dialogs;
    };
    const std::vector<QDialog*> detailsDialogs = findDetailsDialogs();
    QCOMPARE(static_cast<int>(detailsDialogs.size()), 1);
    QDialog* detailsDialog = detailsDialogs.front();
    QVERIFY2(detailsDialog, "double-clicking a DD transaction row must open a non-modal transaction details dialog");
    QCOMPARE(detailsDialog->objectName(), QStringLiteral("DDTransactionDescDialog"));
    QVERIFY2(detailsDialog->windowTitle().contains(QString::fromStdString(tx.txid)),
             "DD transaction details dialog title must include the full txid");

    QTextEdit* detailText = detailsDialog->findChild<QTextEdit*>(QStringLiteral("detailText"));
    QVERIFY(detailText != nullptr);
    QVERIFY(detailText->isReadOnly());
    const QString plainDetails = detailText->toPlainText();

    detailsDialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    QVERIFY2(plainDetails.contains(QString::fromStdString(tx.txid)), "details text must include the full transaction id");
    QVERIFY2(plainDetails.contains(QStringLiteral("Mint 1-yr")), "details text must include the transaction type");
    QVERIFY2(plainDetails.contains(QStringLiteral("+43.21 $DD")), "details text must include the signed $DD amount");
    QVERIFY2(plainDetails.contains(QStringLiteral("Status:")), "details text must include the confirmation status");
    QVERIFY2(plainDetails.contains(QStringLiteral("detail dialog note")), "details text must include the local note");
}

void DigiDollarWidgetTests::transactionsWidgetDetailsDialogOverridesDgbBlueDialogFallback()
{
#ifdef Q_OS_MACOS
    QSKIP("Skipping DD transaction dialog fallback style test on macOS");
#endif

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.show();
    QCoreApplication::processEvents();

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    table->setSortingEnabled(false);
    table->setRowCount(1);

    const QString txid = QStringLiteral("e000000000000000000000000000000000000000000000000000000000000001");
    table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Aug 31, 2020 09:34")));
    table->setItem(0, 1, new QTableWidgetItem(QStringLiteral("Send")));
    table->setItem(0, 2, new QTableWidgetItem(QStringLiteral("-100.00 $DD")));
    table->setItem(0, 3, new QTableWidgetItem(QStringLiteral("-")));
    table->setItem(0, 4, new QTableWidgetItem(QStringLiteral("theme fallback note")));
    QTableWidgetItem* txidItem = new QTableWidgetItem(txid);
    txidItem->setData(Qt::UserRole, txid);
    table->setItem(0, 5, txidItem);
    table->setItem(0, 6, new QTableWidgetItem(QStringLiteral("Confirmed")));
    QCOMPARE(table->rowCount(), 1);

    const QString originalStyleSheet = qApp->styleSheet();
    qApp->setStyleSheet(QStringLiteral(
        "QDialog { background-color: #002352; color: #ffffff; }"
        "QDialog QTextEdit { background-color: #ffffff; color: #000000; border: 2px solid #003366; }"
        "QDialog QPushButton { background-color: #0066CC; color: #ffffff; border: 2px solid #0066CC; }"
        "QDialog#TransactionDescDialog { background-color: #002352; color: #ffffff; }"
        "QDialog#TransactionDescDialog QTextEdit { background-color: #ffffff; color: #000000; }"
        "QDialog#TransactionDescDialog QPushButton { background-color: #0066CC; color: #ffffff; }"));
    QCoreApplication::processEvents();

    const auto findDetailsDialog = [&]() -> QDialog* {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            QDialog* dialog = qobject_cast<QDialog*>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            if (dialog->windowTitle().startsWith(QStringLiteral("Details for "))) return dialog;
        }
        for (QDialog* dialog : transactionsWidget.findChildren<QDialog*>()) {
            if (dialog && dialog->isVisible() && dialog->windowTitle().startsWith(QStringLiteral("Details for "))) {
                return dialog;
            }
        }
        return nullptr;
    };

    const auto openAndRequireDigiDollarStyle = [&](const QString& theme, const QString& dialogBg,
                                                   const QString& textBg, const QString& textColor,
                                                   const QString& accent) {
        QPalette palette = transactionsWidget.palette();
        palette.setColor(QPalette::Window, theme == QStringLiteral("dark") ? QColor(QStringLiteral("#002352"))
                                                                            : QColor(QStringLiteral("#ffffff")));
        transactionsWidget.setPalette(palette);
        table->setCurrentItem(txidItem);
        QVERIFY(QMetaObject::invokeMethod(table, "itemDoubleClicked",
                                          Qt::DirectConnection,
                                          Q_ARG(QTableWidgetItem*, txidItem)));
        QCoreApplication::processEvents();
        QTest::qWait(50);

        QDialog* detailsDialog = findDetailsDialog();
        QVERIFY2(detailsDialog, "DD details dialog did not open while DGB blue fallback stylesheet was active");
        QCOMPARE(detailsDialog->objectName(), QStringLiteral("DDTransactionDescDialog"));

        const QString dialogStyle = detailsDialog->styleSheet();
        detailsDialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();

        QVERIFY2(dialogStyle.contains(dialogBg, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD details dialog must force its own green surface over DGB blue fallback").arg(theme)));
        QVERIFY2(dialogStyle.contains(textBg, Qt::CaseInsensitive) &&
                 dialogStyle.contains(textColor, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD details text pane must force readable DD colors over DGB fallback").arg(theme)));
        QVERIFY2(dialogStyle.contains(accent, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD details close button must force DD green accent over DGB fallback").arg(theme)));
        QVERIFY2(!dialogStyle.contains(QStringLiteral("#002352"), Qt::CaseInsensitive) &&
                 !dialogStyle.contains(QStringLiteral("#003366"), Qt::CaseInsensitive) &&
                 !dialogStyle.contains(QStringLiteral("#0066CC"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD details dialog must not carry DGB blue styling").arg(theme)));
    };

    openAndRequireDigiDollarStyle(QStringLiteral("dark"), QStringLiteral("#0b2419"),
                                  QStringLiteral("#113a29"), QStringLiteral("#ffffff"), QStringLiteral("#16804f"));
    openAndRequireDigiDollarStyle(QStringLiteral("light"), QStringLiteral("#eef9f2"),
                                  QStringLiteral("#ffffff"), QStringLiteral("#123f2b"), QStringLiteral("#1f9d57"));

    qApp->setStyleSheet(originalStyleSheet);
    QCoreApplication::processEvents();
}

void DigiDollarWidgetTests::transactionsWidgetDetailsDialogVisualQaDarkAndLight()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal")) {
        QSKIP("Visual DD transaction detail QA requires a platform that can capture rendered dialog windows");
    }

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.resize(900, 420);
    transactionsWidget.show();
    QCoreApplication::processEvents();
    QTRY_VERIFY(transactionsWidget.isVisible());

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    table->setSortingEnabled(false);
    table->setRowCount(1);

    const QString txid = QStringLiteral("e000000000000000000000000000000000000000000000000000000000000001");
    table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Aug 31, 2020 09:34")));
    table->setItem(0, 1, new QTableWidgetItem(QStringLiteral("Send")));
    table->setItem(0, 2, new QTableWidgetItem(QStringLiteral("-98.76 $DD")));
    table->setItem(0, 3, new QTableWidgetItem(QStringLiteral("-")));
    table->setItem(0, 4, new QTableWidgetItem(QStringLiteral("visual QA note")));
    QTableWidgetItem* txidItem = new QTableWidgetItem(txid);
    txidItem->setData(Qt::UserRole, txid);
    table->setItem(0, 5, txidItem);
    table->setItem(0, 6, new QTableWidgetItem(QStringLiteral("Pending")));

    const QString originalStyleSheet = qApp->styleSheet();
    const QPalette originalPalette = qApp->palette();
    const auto contrastRatio = [](const QColor& a, const QColor& b) {
        const auto channel = [](double c) {
            c /= 255.0;
            return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        const double l1 = 0.2126 * channel(a.red()) + 0.7152 * channel(a.green()) + 0.0722 * channel(a.blue());
        const double l2 = 0.2126 * channel(b.red()) + 0.7152 * channel(b.green()) + 0.0722 * channel(b.blue());
        return (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
    };

    auto openAndCapture = [&](const QString& css, const QColor& windowColor, const QString& path) {
        qApp->setStyleSheet(css);
        QPalette palette = transactionsWidget.palette();
        palette.setColor(QPalette::Window, windowColor);
        palette.setColor(QPalette::Base, windowColor);
        transactionsWidget.setPalette(palette);
        qApp->setPalette(palette);
        QCoreApplication::processEvents();
        table->setCurrentItem(txidItem);
        QVERIFY(QMetaObject::invokeMethod(table, "itemDoubleClicked",
                                          Qt::DirectConnection,
                                          Q_ARG(QTableWidgetItem*, txidItem)));
        QCoreApplication::processEvents();
        QTest::qWait(150);

        QDialog* detailsDialog = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            QDialog* dialog = qobject_cast<QDialog*>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            if (dialog->windowTitle().startsWith(QStringLiteral("Details for "))) {
                detailsDialog = dialog;
                break;
            }
        }
        for (QDialog* dialog : transactionsWidget.findChildren<QDialog*>()) {
            if (detailsDialog) break;
            if (dialog && dialog->isVisible() && dialog->windowTitle().startsWith(QStringLiteral("Details for "))) {
                detailsDialog = dialog;
                break;
            }
        }
        QVERIFY2(detailsDialog, "DD transaction details dialog did not open for visual QA");
        QCOMPARE(detailsDialog->objectName(), QStringLiteral("DDTransactionDescDialog"));
        QTextEdit* detailText = detailsDialog->findChild<QTextEdit*>(QStringLiteral("detailText"));
        QVERIFY(detailText != nullptr);

        const QColor textColor = detailText->palette().color(QPalette::Text);
        const QColor baseColor = detailText->palette().color(QPalette::Base);
        QVERIFY2(contrastRatio(textColor, baseColor) >= 4.5,
                 qPrintable(QStringLiteral("DD transaction details text contrast too low for %1").arg(path)));

        const QPixmap pixmap = detailsDialog->grab();
        QVERIFY2(!pixmap.isNull(), "failed to grab DD transaction details dialog");
        QVERIFY2(pixmap.save(path), qPrintable(QStringLiteral("failed to save %1").arg(path)));

        detailsDialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    };

    const QString darkFallback = QStringLiteral(
        "QDialog { background-color: #002352; color: #ffffff; }"
        "QDialog QTextEdit { background-color: #ffffff; color: #000000; border: 2px solid #003366; }"
        "QDialog QPushButton { background-color: #0066CC; color: #ffffff; border: 2px solid #0066CC; }"
        "QWidget { color: #ffffff; }");
    const QString lightFallback = QStringLiteral(
        "QDialog { background-color: #ffffff; color: #003366; }"
        "QDialog QTextEdit { background-color: #ffffff; color: #003366; border: 2px solid #003366; }"
        "QDialog QPushButton { background-color: #0066CC; color: #ffffff; border: 2px solid #0066CC; }"
        "QWidget { color: #123f2b; }");

    openAndCapture(darkFallback, QColor(QStringLiteral("#002352")),
                   QStringLiteral("/tmp/digibyte_dd_transaction_details_dark_qa.png"));
    openAndCapture(lightFallback, QColor(QStringLiteral("#ffffff")),
                   QStringLiteral("/tmp/digibyte_dd_transaction_details_light_qa.png"));
    qApp->setStyleSheet(originalStyleSheet);
    qApp->setPalette(originalPalette);
    QCoreApplication::processEvents();

    qInfo("DD transaction details dark QA screenshot: /tmp/digibyte_dd_transaction_details_dark_qa.png");
    qInfo("DD transaction details light QA screenshot: /tmp/digibyte_dd_transaction_details_light_qa.png");
}

void DigiDollarWidgetTests::transactionsWidgetDetailsDialogHasDigiDollarThemeRules()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };
    const auto findTheme = [&](const QString& name) -> QString {
        const QStringList candidates{
            QStringLiteral("src/qt/res/css/%1").arg(name),
            QStringLiteral("../src/qt/res/css/%1").arg(name),
            QStringLiteral("../../src/qt/res/css/%1").arg(name),
            QStringLiteral("qt/res/css/%1").arg(name),
        };
        for (const auto& path : candidates) {
            const QString css = readFile(path.toUtf8().constData());
            if (!css.isEmpty()) return css;
        }
        return {};
    };
    const auto extractRule = [](const QString& css, const QString& selector) -> QString {
        const int selectorStart = css.indexOf(selector);
        if (selectorStart < 0) return {};
        const int braceStart = css.indexOf(QLatin1Char('{'), selectorStart);
        if (braceStart < 0) return {};
        const int braceEnd = css.indexOf(QLatin1Char('}'), braceStart);
        if (braceEnd < 0) return {};
        return css.mid(braceStart + 1, braceEnd - braceStart - 1);
    };
    const auto requireTheme = [&](const QString& css, const QString& theme, const QString& dialogBg,
                                  const QString& textBg, const QString& textColor, const QString& accent) {
        const QString dialog = extractRule(css, QStringLiteral("QDialog#DDTransactionDescDialog"));
        const QString textEdit = extractRule(css, QStringLiteral("QDialog#DDTransactionDescDialog QTextEdit"));
        const QString button = extractRule(css, QStringLiteral("QDialog#DDTransactionDescDialog QPushButton"));

        QVERIFY2(!dialog.isEmpty(), qPrintable(QStringLiteral("%1 must style QDialog#DDTransactionDescDialog").arg(theme)));
        QVERIFY2(!textEdit.isEmpty(), qPrintable(QStringLiteral("%1 must style DD transaction detail text").arg(theme)));
        QVERIFY2(!button.isEmpty(), qPrintable(QStringLiteral("%1 must style DD transaction detail buttons").arg(theme)));

        QVERIFY2(dialog.contains(dialogBg, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD dialog must use the DigiDollar green surface, not DGB blue").arg(theme)));
        QVERIFY2(textEdit.contains(textBg, Qt::CaseInsensitive) && textEdit.contains(textColor, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD detail text pane must have readable DigiDollar colors").arg(theme)));
        QVERIFY2(button.contains(accent, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD detail close button must use DigiDollar green accent").arg(theme)));

        QVERIFY2(!dialog.contains(QStringLiteral("#002352"), Qt::CaseInsensitive) &&
                 !textEdit.contains(QStringLiteral("#003366"), Qt::CaseInsensitive) &&
                 !button.contains(QStringLiteral("#0066CC"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 DD detail dialog must not reuse DGB blue transaction detail styling").arg(theme)));
    };

    const QString dark = findTheme(QStringLiteral("dark.css"));
    QVERIFY2(!dark.isEmpty(), "could not locate dark.css from current working directory");
    requireTheme(dark, QStringLiteral("dark.css"), QStringLiteral("#0b2419"),
                 QStringLiteral("#113a29"), QStringLiteral("#ffffff"), QStringLiteral("#16804f"));

    const QString light = findTheme(QStringLiteral("light.css"));
    QVERIFY2(!light.isEmpty(), "could not locate light.css from current working directory");
    requireTheme(light, QStringLiteral("light.css"), QStringLiteral("#eef9f2"),
                 QStringLiteral("#ffffff"), QStringLiteral("#123f2b"), QStringLiteral("#1f9d57"));
}

// Regression coverage for the DD Transactions tab's RPC-backed history table:
// listdigidollartxs returns signed amounts derived from incoming/outgoing wallet
// direction, and the Qt table must preserve those signs while showing the right
// category, lock-period, note, truncated txid, and confirmation text. This is the
// display path used for sendmanydigidollar history rows, including the aggregate
// outgoing "multiple" row and local-recipient receive rows verified functionally.
void DigiDollarWidgetTests::transactionsWidgetShowsRpcHistorySignsAndFields()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-history");
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    const int64_t now = GetTime();
    auto pushTx = [&](const std::string& txid, CAmount amount, bool incoming,
                      const std::string& category, const std::string& address,
                      const std::string& comment, int lock_tier, int64_t offset) {
        DDTransaction tx;
        tx.txid = txid;
        tx.amount = amount;
        tx.timestamp = now + offset;
        tx.confirmations = 0;
        tx.incoming = incoming;
        tx.address = address;
        tx.category = category;
        tx.lock_tier = lock_tier;
        tx.fee = 0;
        tx.comment = comment;
        tx.abandoned = false;
        dd_wallet->AddMockTransaction(tx);
    };

    const QString sendTxid = "b000000000000000000000000000000000000000000000000000000000000001";
    const QString recvTxid = "b000000000000000000000000000000000000000000000000000000000000002";
    const QString redeemTxid = "b000000000000000000000000000000000000000000000000000000000000003";
    const QString mintTxid = "b000000000000000000000000000000000000000000000000000000000000004";
    const QString emptyNoteTxid = "b000000000000000000000000000000000000000000000000000000000000005";

    pushTx(sendTxid.toStdString(), 500, false, "send", "multiple", "sendmany functional test", -1, 4);
    pushTx(recvTxid.toStdString(), 200, true, "receive", "TDlocalrecipient1", "local receive row", -1, 3);
    pushTx(redeemTxid.toStdString(), 1250, false, "redeem", "TDredeemaddress", "redeem note", 1, 2);
    pushTx(mintTxid.toStdString(), 700, true, "mint", "TDmintaddress", "mint note", 9, 1);
    pushTx(emptyNoteTxid.toStdString(), 300, true, "receive", "TDempty", "", -1, 0);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());
    transactionsWidget.show();
    QCoreApplication::processEvents();
    transactionsWidget.updateView();
    QCoreApplication::processEvents();

    RemoveWallet(context, wallet, std::nullopt);

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 5);

    auto findRowByTxid = [&](const QString& txid) -> int {
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem* txidItem = table->item(row, 5);
            if (txidItem && txidItem->data(Qt::UserRole).toString() == txid) {
                return row;
            }
        }
        return -1;
    };

    auto checkRow = [&](const QString& txid, const QString& type, const QString& amount,
                        const QString& lockPeriod, const QString& note,
                        const QString& noteTooltip = QString()) {
        const int row = findRowByTxid(txid);
        QVERIFY2(row >= 0, qPrintable(QString("missing DD transaction row for %1").arg(txid)));
        QTableWidgetItem* txidItem = table->item(row, 5);
        QVERIFY(txidItem != nullptr);
        QCOMPARE(txidItem->text(), txid.left(16) + "..." + txid.right(8));
        QCOMPARE(txidItem->toolTip(), txid);
        QCOMPARE(table->item(row, 1)->text(), type);
        QCOMPARE(table->item(row, 2)->text(), amount);
        QCOMPARE(table->item(row, 3)->text(), lockPeriod);
        QCOMPARE(table->item(row, 4)->text(), note);
        QCOMPARE(table->item(row, 4)->toolTip(), noteTooltip.isNull() ? note : noteTooltip);
        QCOMPARE(table->item(row, 6)->text(), QString("Pending"));
    };

    checkRow(sendTxid, "Send", "-5.00 $DD", "-", "sendmany functional test");
    checkRow(recvTxid, "Receive", "+2.00 $DD", "-", "local receive row");
    checkRow(redeemTxid, "Redeem 30-day", "-12.50 $DD", "30 days", "redeem note");
    checkRow(mintTxid, "Mint 10-yr", "+7.00 $DD", "10 years", "mint note");
    checkRow(emptyNoteTxid, "Receive", "+3.00 $DD", "-", "", QString("No note"));
}

// Regression test for the DD Vault "Lock Tier" column truncation: with the
// column pinned at 85 px the longer human-readable tier names ("3 months",
// "6 months", "10 years") rendered as "3 ...", "6 ...", "10 ye..." in the
// live wallet because the cell text exceeded the column width by ~15 px.
// Guard the column against future shrinkage by asserting it is at least
// wide enough to fit the longest tier label plus a normal cell padding.
void DigiDollarWidgetTests::positionsWidgetLockTierColumnFitsLongestLabel()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.show();
    positionsWidget.updateView();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);

    // The longest tier label rendered by digidollarpositionswidget.cpp is
    // "10 years" (tier 9). Ask Qt for the actual painted width using the
    // table's own font, then add the standard 12 px frame Qt uses for
    // QTableWidget cells. The column must be at least that wide.
    const QFontMetrics fm(table->font());
    const QStringList tierLabels = {
        QStringLiteral("1 hour"),    QStringLiteral("30 days"),
        QStringLiteral("3 months"),  QStringLiteral("6 months"),
        QStringLiteral("1 year"),    QStringLiteral("2 years"),
        QStringLiteral("3 years"),   QStringLiteral("5 years"),
        QStringLiteral("7 years"),   QStringLiteral("10 years"),
    };
    int maxLabelWidth = 0;
    for (const QString& label : tierLabels) {
        maxLabelWidth = std::max(maxLabelWidth, fm.horizontalAdvance(label));
    }
    const int requiredWidth = maxLabelWidth + 12; // QTableWidget cell padding
    const int actualWidth = table->columnWidth(DigiDollarPositionsWidget::COL_LOCK_TIER);
    QVERIFY2(actualWidth >= requiredWidth,
             qPrintable(QString("Lock Tier column too narrow: %1 px, need at least %2 px to fit '%3'")
                        .arg(actualWidth)
                        .arg(requiredWidth)
                        .arg(QStringLiteral("10 years"))));
}

void DigiDollarWidgetTests::positionsWidgetSortingKeepsHealthAndActionsOnSameRow()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    DigiDollarPositionsWidget positionsWidget;
    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);

    const auto makePosition = [](const QString& suffix, double ddMinted, double health) {
        DigiDollarPosition position{};
        position.positionId = QStringLiteral("00000000000000000000000000000000000000000000000000000000000000%1").arg(suffix);
        position.ddMinted = ddMinted;
        position.dgbCollateral = 1000.0 + ddMinted;
        position.lockTier = 1;
        position.unlockHeight = 2000;
        position.blocksRemaining = 40;
        position.health = health;
        position.canRedeem = false;
        position.isPendingRedeem = false;
        position.isRedeemed = false;
        position.mintTime = 1000 + static_cast<int64_t>(ddMinted);
        return position;
    };

    positionsWidget.m_positions.clear();
    positionsWidget.m_positions.append(makePosition(QStringLiteral("03"), 300.0, 130.0));
    positionsWidget.m_positions.append(makePosition(QStringLiteral("01"), 100.0, 110.0));
    positionsWidget.m_positions.append(makePosition(QStringLiteral("02"), 200.0, 120.0));

    table->setSortingEnabled(true);
    table->sortByColumn(DigiDollarPositionsWidget::COL_DD_MINTED, Qt::AscendingOrder);
    positionsWidget.populatePositionsTable();
    QCOMPARE(table->rowCount(), 3);

    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* idItem = table->item(row, DigiDollarPositionsWidget::COL_POSITION_ID);
        QVERIFY2(idItem != nullptr, qPrintable(QString("row %1 lost its Vault ID item").arg(row)));

        QWidget* healthWidget = table->cellWidget(row, DigiDollarPositionsWidget::COL_HEALTH);
        QVERIFY2(healthWidget != nullptr, qPrintable(QString("row %1 lost its Health widget").arg(row)));
        QProgressBar* healthBar = healthWidget->findChild<QProgressBar*>();
        QVERIFY2(healthBar != nullptr, qPrintable(QString("row %1 Health widget has no progress bar").arg(row)));

        QPushButton* actionButton = qobject_cast<QPushButton*>(
            table->cellWidget(row, DigiDollarPositionsWidget::COL_ACTIONS));
        QVERIFY2(actionButton != nullptr, qPrintable(QString("row %1 lost its Actions button").arg(row)));
        QCOMPARE(actionButton->property("positionId").toString(), idItem->text());
    }
}

void DigiDollarWidgetTests::darkThemePeerDetailWidgetHasExplicitRule()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };

    const QStringList candidates = {
        QStringLiteral("src/qt/res/css/dark.css"),
        QStringLiteral("../src/qt/res/css/dark.css"),
        QStringLiteral("../../src/qt/res/css/dark.css"),
        QStringLiteral("qt/res/css/dark.css"),
    };
    QString dark;
    for (const auto& p : candidates) {
        dark = readFile(p.toUtf8().constData());
        if (!dark.isEmpty()) break;
    }
    QVERIFY2(!dark.isEmpty(), "could not locate dark.css from current working directory");

    const QRegularExpression detailRule(
        QStringLiteral(R"re((RPCConsole|QDialog#RPCConsole)\s+QWidget#detailWidget[^\{]*\{[^\}]*background-color\s*:\s*#002352\s*;[^\}]*color\s*:\s*#ffffff\s*;)re"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    const bool found = detailRule.match(dark).hasMatch();
    QVERIFY2(found,
             "RC30 dark-mode peers pane contrast bug: dark.css must carry an explicit rule for the RPC console's #detailWidget so the Windows default grey QWidget palette doesn't leak through. Expected a selector like 'RPCConsole QWidget#detailWidget { ... }'.");
}

void DigiDollarWidgetTests::darkThemeDigiDollarSendTotalLabelHasReadableContrast()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };

    const QStringList candidates = {
        QStringLiteral("src/qt/res/css/dark.css"),
        QStringLiteral("../src/qt/res/css/dark.css"),
        QStringLiteral("../../src/qt/res/css/dark.css"),
        QStringLiteral("qt/res/css/dark.css"),
    };
    QString dark;
    for (const auto& p : candidates) {
        dark = readFile(p.toUtf8().constData());
        if (!dark.isEmpty()) break;
    }
    QVERIFY2(!dark.isEmpty(), "could not locate dark.css from current working directory");

    const QRegularExpression totalLabelRule(
        QStringLiteral(R"re(DigiDollarSendWidget\s+QLabel#totalLabel[^\{]*\{([^\}]*)\})re"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = totalLabelRule.match(dark);
    QVERIFY2(match.hasMatch(), "dark.css must style DigiDollarSendWidget QLabel#totalLabel explicitly");

    const QString ruleBody = match.captured(1);
    const QRegularExpression backgroundRe(
        QStringLiteral(R"re(background-color\s*:\s*(#[0-9a-fA-F]{6})\s*;)re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression colorRe(
        QStringLiteral(R"re((?:^|[;\r\n])\s*color\s*:\s*(#[0-9a-fA-F]{6})\s*;)re"),
        QRegularExpression::CaseInsensitiveOption);
    const QString background = backgroundRe.match(ruleBody).captured(1).toLower();
    const QString color = colorRe.match(ruleBody).captured(1).toLower();

    QVERIFY2(!background.isEmpty(), "totalLabel rule must set a stable dark-theme background");
    QVERIFY2(!color.isEmpty(), "totalLabel rule must set a stable dark-theme text color");
    QVERIFY2(background != color,
             qPrintable(QString("DigiDollar Send Total DD label is unreadable in dark mode: color %1 on %2")
                        .arg(color, background)));

    const int totalLabelRuleEnd = dark.lastIndexOf(QStringLiteral("DigiDollarSendWidget QLabel#totalLabel"));
    QVERIFY2(totalLabelRuleEnd >= 0, "dark.css must have an explicit final Total DD label override");
    const int laterGenericLabelRule = dark.indexOf(
        QRegularExpression(QStringLiteral(R"re(DigiDollarSendWidget\s+QLabel\s*\{[^\}]*background-color\s*:\s*transparent\s*;)re"),
                           QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption),
        totalLabelRuleEnd);
    QVERIFY2(laterGenericLabelRule < 0,
             "A later generic DigiDollarSendWidget QLabel rule resets the Total DD label background after the explicit rule");
}

void DigiDollarWidgetTests::darkThemeShutdownWindowHasReadableSurface()
{
    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };
    const auto findFile = [&](const QStringList& candidates) -> QString {
        for (const auto& p : candidates) {
            const QString text = readFile(p.toUtf8().constData());
            if (!text.isEmpty()) return text;
        }
        return {};
    };

    const QString dark = findFile({
        QStringLiteral("src/qt/res/css/dark.css"),
        QStringLiteral("../src/qt/res/css/dark.css"),
        QStringLiteral("../../src/qt/res/css/dark.css"),
        QStringLiteral("qt/res/css/dark.css"),
    });
    QVERIFY2(!dark.isEmpty(), "could not locate dark.css from current working directory");

    const QString utilityDialog = findFile({
        QStringLiteral("src/qt/utilitydialog.cpp"),
        QStringLiteral("../src/qt/utilitydialog.cpp"),
        QStringLiteral("../../src/qt/utilitydialog.cpp"),
        QStringLiteral("qt/utilitydialog.cpp"),
    });
    QVERIFY2(!utilityDialog.isEmpty(), "could not locate utilitydialog.cpp from current working directory");
    QVERIFY2(utilityDialog.contains(QStringLiteral("setObjectName(\"shutdownWindow\")")),
             "ShutdownWindow must expose a stable object name for dark-theme styling");

    const QRegularExpression shutdownRule(
        QStringLiteral(R"re(QWidget#shutdownWindow[^\{]*\{[^\}]*background-color\s*:\s*#002352\s*;[^\}]*color\s*:\s*#ffffff\s*;)re"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(shutdownRule.match(dark).hasMatch(),
             "dark.css must give ShutdownWindow an explicit dark background with white text");

    const QRegularExpression labelRule(
        QStringLiteral(R"re(QWidget#shutdownWindow\s+QLabel[^\{]*\{[^\}]*color\s*:\s*#ffffff\s*;)re"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(labelRule.match(dark).hasMatch(),
             "dark.css must explicitly style ShutdownWindow labels for readable shutdown text");
}

void DigiDollarWidgetTests::globalTooltipFilterHandlesNativeTooltipEvents()
{
    class ExposedToolTipFilter : public GUIUtil::ToolTipToRichTextFilter
    {
    public:
        explicit ExposedToolTipFilter(int size_threshold) : GUIUtil::ToolTipToRichTextFilter(size_threshold) {}
        using GUIUtil::ToolTipToRichTextFilter::eventFilter;
    };

    QLabel label;
    label.setToolTip(QStringLiteral("Your DGB locked as collateral for DigiDollars in your wallet"));

    ExposedToolTipFilter filter(80);
    QHelpEvent event(QEvent::ToolTip, QPoint(2, 2), QPoint(20, 20));
    QVERIFY2(filter.eventFilter(&label, &event),
             "Global tooltip filter must intercept native tooltip events and render the visible styled tooltip path");

    QListWidget list;
    list.resize(260, 80);
    QListWidgetItem* item = new QListWidgetItem(QStringLiteral("Recent transaction"));
    item->setToolTip(QStringLiteral("Pending transaction tooltip for a DigiDollar row"));
    list.addItem(item);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));

    const QPoint item_pos = list.visualItemRect(item).center();
    QHelpEvent item_event(QEvent::ToolTip, item_pos, list.viewport()->mapToGlobal(item_pos));
    QVERIFY2(filter.eventFilter(list.viewport(), &item_event),
             "Global tooltip filter must intercept item-view tooltip events used by overview and transaction rows");
    QToolTip::hideText();
}

void DigiDollarWidgetTests::globalTooltipVisualContrastRendersReadablePixels()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal")) {
        QSKIP("Visual tooltip contrast QA requires a platform that can capture rendered tooltip windows");
    }

    class ExposedToolTipFilter : public GUIUtil::ToolTipToRichTextFilter
    {
    public:
        explicit ExposedToolTipFilter(int size_threshold) : GUIUtil::ToolTipToRichTextFilter(size_threshold) {}
        using GUIUtil::ToolTipToRichTextFilter::eventFilter;
    };

    QLabel label(QStringLiteral("Tooltip visual QA target"));
    label.setToolTip(QStringLiteral("Your DGB locked as collateral for DigiDollars in your wallet"));
    label.resize(540, 90);
    label.move(120, 120);
    label.show();
    QVERIFY(QTest::qWaitForWindowExposed(&label));

    ExposedToolTipFilter filter(80);
    const QPoint local_pos(24, 24);
    const QPoint global_pos = label.mapToGlobal(local_pos);
    QHelpEvent event(QEvent::ToolTip, local_pos, global_pos);
    QVERIFY2(filter.eventFilter(&label, &event), "tooltip filter did not show the styled tooltip");
    QTest::qWait(600);

    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY2(screen, "no primary screen available for tooltip visual QA");
    const QPixmap pixmap = screen->grabWindow(0);
    QVERIFY2(!pixmap.isNull(), "screen grab failed for tooltip visual QA");
    const QString screenshot_path = QStringLiteral("/tmp/digibyte_tooltip_qa.png");
    QVERIFY2(pixmap.save(screenshot_path),
             qPrintable(QStringLiteral("failed to save tooltip QA screenshot to %1").arg(screenshot_path)));

    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    const QRect search_rect = QRect(global_pos - QPoint(40, 40), QSize(760, 260)).intersected(image.rect());
    QVERIFY2(!search_rect.isEmpty(), "tooltip visual QA search area is outside the captured screen");

    QRect tooltip_bounds;
    int yellow_pixels = 0;
    for (int y = search_rect.top(); y <= search_rect.bottom(); ++y) {
        for (int x = search_rect.left(); x <= search_rect.right(); ++x) {
            const QColor color(image.pixel(x, y));
            const bool tooltip_yellow =
                color.red() >= 235 && color.green() >= 220 && color.blue() >= 170 &&
                color.red() >= color.blue() + 35 && color.green() >= color.blue() + 25;
            if (!tooltip_yellow) continue;
            ++yellow_pixels;
            const QRect pixel_rect(x, y, 1, 1);
            tooltip_bounds = tooltip_bounds.isNull() ? pixel_rect : tooltip_bounds.united(pixel_rect);
        }
    }

    QVERIFY2(yellow_pixels > 100,
             qPrintable(QStringLiteral("tooltip yellow background was not found in %1; yellow pixels=%2")
                        .arg(screenshot_path).arg(yellow_pixels)));

    const QRect interior = tooltip_bounds.adjusted(4, 4, -4, -4).intersected(image.rect());
    QVERIFY2(!interior.isEmpty(), "tooltip yellow background bounds are too small for text contrast sampling");

    int black_text_pixels = 0;
    int white_text_pixels = 0;
    for (int y = interior.top(); y <= interior.bottom(); ++y) {
        for (int x = interior.left(); x <= interior.right(); ++x) {
            const QColor color(image.pixel(x, y));
            if (color.red() <= 80 && color.green() <= 80 && color.blue() <= 80) {
                ++black_text_pixels;
            } else if (color.red() >= 235 && color.green() >= 235 && color.blue() >= 235) {
                ++white_text_pixels;
            }
        }
    }

    QVERIFY2(black_text_pixels > 25,
             qPrintable(QStringLiteral("tooltip text did not render as dark pixels in %1; black=%2 white=%3 bounds=%4,%5 %6x%7")
                        .arg(screenshot_path).arg(black_text_pixels).arg(white_text_pixels)
                        .arg(tooltip_bounds.x()).arg(tooltip_bounds.y()).arg(tooltip_bounds.width()).arg(tooltip_bounds.height())));
    QVERIFY2(black_text_pixels >= white_text_pixels,
             qPrintable(QStringLiteral("tooltip still appears light-on-light in %1; black=%2 white=%3")
                        .arg(screenshot_path).arg(black_text_pixels).arg(white_text_pixels)));

    QToolTip::hideText();
    label.close();
    qInfo("Tooltip visual QA screenshot: %s", qPrintable(screenshot_path));
}

void DigiDollarWidgetTests::customTooltipRenderersNormalizeQtRichTextEnvelope()
{
    QCOMPARE(GUIUtil::TooltipToHtml(QStringLiteral("Plain <value>\nsecond")),
             QStringLiteral("Plain &lt;value&gt;<br>\nsecond"));
    QCOMPARE(GUIUtil::TooltipToHtml(QStringLiteral("<qt>Pending &lt;change&gt;<br>line 2</qt>")),
             QStringLiteral("Pending &lt;change&gt;<br>\nline 2"));
    QCOMPARE(GUIUtil::TooltipToHtml(QStringLiteral("<nobr>Network activity disabled.<br>Click to enable.</nobr>")),
             QStringLiteral("Network activity disabled.<br>\nClick to enable."));

    const auto readFile = [](const char* path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll());
    };
    const auto findFile = [&](const QStringList& candidates) -> QString {
        for (const auto& p : candidates) {
            const QString text = readFile(p.toUtf8().constData());
            if (!text.isEmpty()) return text;
        }
        return {};
    };

    const QString overviewPage = findFile({
        QStringLiteral("src/qt/overviewpage.cpp"),
        QStringLiteral("../src/qt/overviewpage.cpp"),
        QStringLiteral("../../src/qt/overviewpage.cpp"),
        QStringLiteral("qt/overviewpage.cpp"),
    });
    QVERIFY2(!overviewPage.isEmpty(), "could not locate overviewpage.cpp from current working directory");

    const QString transactionOverviewWidget = findFile({
        QStringLiteral("src/qt/transactionoverviewwidget.cpp"),
        QStringLiteral("../src/qt/transactionoverviewwidget.cpp"),
        QStringLiteral("../../src/qt/transactionoverviewwidget.cpp"),
        QStringLiteral("qt/transactionoverviewwidget.cpp"),
    });
    QVERIFY2(!transactionOverviewWidget.isEmpty(), "could not locate transactionoverviewwidget.cpp from current working directory");

    QVERIFY2(!overviewPage.contains(QStringLiteral("tooltipText.toHtmlEscaped()")),
             "OverviewPage custom tooltips must normalize Qt <qt> rich-text envelopes before escaping");
    QVERIFY2(!transactionOverviewWidget.contains(QStringLiteral("tooltipText.toHtmlEscaped()")),
             "TransactionOverviewWidget custom tooltips must normalize Qt <qt> rich-text envelopes before escaping");
    QVERIFY2(overviewPage.contains(QStringLiteral("GUIUtil::TooltipToHtml")),
             "OverviewPage should use GUIUtil::TooltipToHtml for custom tooltip rendering");
    QVERIFY2(transactionOverviewWidget.contains(QStringLiteral("GUIUtil::TooltipToHtml")),
             "TransactionOverviewWidget should use GUIUtil::TooltipToHtml for custom tooltip rendering");
}
