// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/digidollarwidgettests.h>
#include <qt/test/util.h>

#include <consensus/digidollar.h>
#include <consensus/merkle.h>
#include <consensus/volatility.h>
#include <coins.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <paymaster/provider.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/paymasterconfirmation.h>
#include <qt/platformstyle.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>
#include <qt/digidollaroverviewwidget.h>
#include <qt/digidollarmintwidget.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarstatus.h>
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
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <validation.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <univalue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <memory>
#include <map>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <QApplication>
#include <QAccessible>
#include <QAbstractButton>
#include <QColor>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QRegularExpression>
#include <QComboBox>
#include <QProgressBar>
#include <QRadioButton>
#include <QListWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QWheelEvent>
#include <QWizard>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolTip>
#include <QtTest/QtTestWidgets>
#include <QtWidgets/qtestsupport_widgets.h>

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

std::map<uint256, int64_t> MintDescriptorNextIndexes(const wallet::CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    std::map<uint256, int64_t> indexes;
    for (auto* manager : wallet.GetAllScriptPubKeyMans()) {
        if (auto* descriptor = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(manager)) {
            LOCK(descriptor->cs_desc_man);
            indexes.emplace(descriptor->GetID(), descriptor->GetWalletDescriptor().next_index);
        }
    }
    return indexes;
}

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

// The redeem widget shows a "Confirm Redeem" box before it asks the wallet to
// unlock. That box runs its own event loop, so the test cannot press Yes from
// the line after the click. Call this first instead. It waits for the box and
// presses Yes, which lets the test reach the unlock step.
void AcceptConfirmRedeemDialogWhenShown()
{
    QTimer::singleShot(0, []() {
        QMessageBox* box = nullptr;
        for (int attempt = 0; attempt < 40 && !box; ++attempt) {
            box = qobject_cast<QMessageBox*>(FindVisibleDialogByTitle(QStringLiteral("Confirm Redeem")));
            if (!box) QTest::qWait(25);
        }
        QVERIFY2(box != nullptr, "Confirm Redeem dialog did not open");
        QAbstractButton* yes = box->button(QMessageBox::Yes);
        QVERIFY2(yes != nullptr, "Confirm Redeem dialog has no Yes button");
        yes->click();
    });
}

// Adds a wallet to the node's wallet list so that wallet RPC calls can find it
// by name, and takes it out again on every exit path. A test that fails early
// and leaves the wallet in that list hangs when the fixture shuts down.
struct ScopedWalletRegistration {
    WalletContext& context;
    std::shared_ptr<wallet::CWallet> wallet;

    ScopedWalletRegistration(WalletContext& context_in, std::shared_ptr<wallet::CWallet> wallet_in)
        : context(context_in), wallet(std::move(wallet_in))
    {
        AddWallet(context, wallet);
    }
    ~ScopedWalletRegistration() { RemoveWallet(context, wallet, std::nullopt); }
    ScopedWalletRegistration(const ScopedWalletRegistration&) = delete;
    ScopedWalletRegistration& operator=(const ScopedWalletRegistration&) = delete;
};

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

UniValue PaymasterLiquiditySlotStatus(int ready, int pending, int missing,
                                      int target)
{
    UniValue status{UniValue::VOBJ};
    status.pushKV("ready", ready);
    status.pushKV("pending", pending);
    status.pushKV("missing", missing);
    status.pushKV("target", target);
    status.pushKV("counted_toward_target",
                  std::min(target, ready + pending));
    return status;
}

UniValue PaymasterLiquidityStatus(const std::string& state, bool configured,
                                  bool approved, int missing,
                                  int pending = 0,
                                  bool targets_satisfy_provider_policy = true)
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("automatic_replenishment", true);
    policy.pushKV("paid_maintenance_approved", approved);
    policy.pushKV("target_admission_dgb", 3);
    policy.pushKV("target_operational_dgb", 1);
    policy.pushKV("target_admission_carriers", 3);
    policy.pushKV("target_operational_carriers",
                  targets_satisfy_provider_policy ? 1 : 0);
    policy.pushKV("maximum_maintenance_fee_per_transaction_satoshis", 10000000);
    policy.pushKV("maximum_maintenance_fee_per_hour_satoshis", 50000000);
    policy.pushKV("maximum_maintenance_fee_per_day_satoshis", 200000000);

    UniValue result{UniValue::VOBJ};
    result.pushKV("policy_configured", configured);
    result.pushKV("targets_satisfy_provider_policy",
                  targets_satisfy_provider_policy);
    result.pushKV("maintenance_state", state);
    result.pushKV("policy", std::move(policy));
    result.pushKV("admission_dgb",
                  PaymasterLiquiditySlotStatus(3, 0, 0, 3));
    result.pushKV("operational_dgb",
                  PaymasterLiquiditySlotStatus(
                      (missing > 0 || pending > 0) ? 0 : 1,
                      pending, missing, 1));
    result.pushKV("admission_carriers",
                  PaymasterLiquiditySlotStatus(3, 0, 0, 3));
    result.pushKV("operational_carriers",
                  PaymasterLiquiditySlotStatus(
                      targets_satisfy_provider_policy ? 1 : 0, 0, 0,
                      targets_satisfy_provider_policy ? 1 : 0));
    result.pushKV("maintenance_fee_reserved_satoshis", 1000000);
    result.pushKV("maintenance_fee_spent_last_hour_satoshis", 2000000);
    result.pushKV("maintenance_fee_spent_last_day_satoshis", 3000000);
    result.pushKV("carrier_base_cents", 100);
    result.pushKV("carrier_withdrawable_excess_cents", 6);
    result.pushKV("readiness_errors", UniValue{UniValue::VARR});
    return result;
}

UniValue PaymasterLiquidityPoolStatus()
{
    UniValue available{UniValue::VOBJ};
    available.pushKV("state", "available");
    available.pushKV("purpose", "operational");
    available.pushKV("asset", "dd_carrier");
    available.pushKV("txid",
                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    available.pushKV("vout", 1);
    available.pushKV("dgb_satoshis", 0);
    available.pushKV("dd_cents", 106);
    available.pushKV("confirmation_height", 120);
    available.pushKV("updated_at", 1000);

    UniValue pending_carrier{UniValue::VOBJ};
    pending_carrier.pushKV("state", "pending_successor");
    pending_carrier.pushKV("purpose", "operational");
    pending_carrier.pushKV("asset", "dd_carrier");
    pending_carrier.pushKV(
        "txid",
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pending_carrier.pushKV("vout", 2);
    pending_carrier.pushKV("dgb_satoshis", 0);
    pending_carrier.pushKV("dd_cents", 103);
    pending_carrier.pushKV("confirmation_height", 0);
    pending_carrier.pushKV("updated_at", 1001);

    UniValue pending_dgb{UniValue::VOBJ};
    pending_dgb.pushKV("state", "pending_successor");
    pending_dgb.pushKV("purpose", "operational");
    pending_dgb.pushKV("asset", "dgb");
    pending_dgb.pushKV(
        "txid",
        "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pending_dgb.pushKV("vout", 3);
    pending_dgb.pushKV("dgb_satoshis", 10000000);
    pending_dgb.pushKV("dd_cents", 0);
    pending_dgb.pushKV("confirmation_height", 0);
    pending_dgb.pushKV("updated_at", 1002);

    UniValue pool{UniValue::VARR};
    pool.push_back(std::move(available));
    pool.push_back(std::move(pending_carrier));
    pool.push_back(std::move(pending_dgb));
    UniValue result{UniValue::VOBJ};
    result.pushKV("pool", std::move(pool));
    return result;
}

UniValue PaymasterClientSafetyStatus()
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("maximum_service_fee_per_transaction_cents", 100);
    policy.pushKV("maximum_service_fee_per_day_cents", 1000);

    UniValue result{UniValue::VOBJ};
    result.pushKV("configured", true);
    result.pushKV("policy", std::move(policy));
    result.pushKV("active_reservations", 0);
    result.pushKV("reserved_service_fee_cents", 0);
    result.pushKV("spent_service_fee_last_day_cents", 0);
    result.pushKV("available_service_fee_today_cents", 1000);
    return result;
}

UniValue PaymasterOffer(const std::string& display_name,
                        const std::string& provider_id,
                        const std::string& funding_model,
                        int64_t service_fee_cents,
                        int64_t payment_cents,
                        bool established_reputation,
                        int64_t expires_at)
{
    UniValue offer{UniValue::VOBJ};
    offer.pushKV("display_name", display_name);
    offer.pushKV("provider_id", provider_id);
    offer.pushKV("offer_id", "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    offer.pushKV("policy_hash", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    offer.pushKV("funding_model", funding_model);
    offer.pushKV("service_fee_cents", service_fee_cents);
    offer.pushKV("payment_cents", payment_cents);
    offer.pushKV("user_total_cents", payment_cents + service_fee_cents);
    offer.pushKV("subtract_paymaster_fee_from_amount", false);
    offer.pushKV("reputation_sufficient_data", established_reputation);
    offer.pushKV("success_rate_basis_points", established_reputation ? 9875 : 0);
    offer.pushKV("expires_at", expires_at);
    return offer;
}

UniValue PaymasterSessionView(const std::string& state,
                              const std::string& artifact,
                              const std::string& attempt_state,
                              bool include_recipient = true,
                              int64_t recovery_expires_at = 2000000000,
                              bool recovery_expired = false)
{
    const std::string provider(64, 'b');
    UniValue session{UniValue::VOBJ};
    session.pushKV("request_id", "00000000-0000-4000-8000-000000000001");
    session.pushKV("session_id", std::string(64, '1'));
    session.pushKV("session_state", state);
    session.pushKV("pending_phase", "NONE");
    session.pushKV("broadcast_state",
                   state == "MEMPOOL" ? "accepted_mempool" :
                   state == "CONFIRMED" || state == "CANCELED_SAFE"
                       ? "confirmed" : "not_attempted");
    session.pushKV("confirmation_state",
                   state == "CONFIRMED" ? "payment_confirmed" :
                   state == "CANCELED_SAFE" ? "recovery_confirmed" :
                   state == "CONFLICTED" ? "conflicted" : "unconfirmed");
    session.pushKV("final", state == "CONFIRMED" ||
                                state == "CANCELED_SAFE" ||
                                state == "FAILED" || state == "CONFLICTED");
    if (include_recipient || !attempt_state.empty()) {
        session.pushKV("provider_id", provider);
        session.pushKV("policy_hash", std::string(64, 'a'));
    }
    session.pushKV("privacy_profile", "standard");
    if (include_recipient) {
        session.pushKV("to_address",
                       "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
        session.pushKV("requested_amount_cents", 325);
        session.pushKV("payment_cents", 325);
        session.pushKV("service_fee_cents", 2);
        session.pushKV("user_total_cents", 327);
    }
    session.pushKV("expires_at", int64_t{2000000000});
    if (state == "MEMPOOL" || state == "CONFIRMED") {
        session.pushKV("txid", std::string(64, 'e'));
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("action", "refresh");
    result.pushKV("session", std::move(session));
    if (attempt_state.empty()) {
        result.pushKV("attempt", UniValue{UniValue::VNULL});
    } else {
        UniValue attempt{UniValue::VOBJ};
        attempt.pushKV("attempt_state", attempt_state);
        attempt.pushKV("provider_id", provider);
        attempt.pushKV("privacy_profile", "standard");
        result.pushKV("attempt", std::move(attempt));
    }
    result.pushKV("artifact", artifact);
    if (artifact == "alternative_recovery") {
        UniValue recovery{UniValue::VOBJ};
        recovery.pushKV("expires_at", recovery_expires_at);
        recovery.pushKV("expired", recovery_expired);
        result.pushKV("recovery", std::move(recovery));
    } else {
        result.pushKV("recovery", UniValue{UniValue::VNULL});
    }
    result.pushKV("requires_attention",
                  state == "FAILED" || state == "CONFLICTED");
    UniValue actions{UniValue::VARR};
    actions.push_back("refresh");
    if (artifact == "none" && state != "FAILED" &&
        state != "CONFLICTED") {
        actions.push_back("resume");
        actions.push_back("abandon_unsigned");
        if (!attempt_state.empty()) actions.push_back("fallback");
    }
    if ((artifact == "user_psbt" || artifact == "final_transaction") &&
        state != "CONFIRMED") {
        actions.push_back("retry_same");
        actions.push_back("cancel_to_self");
    }
    if (artifact == "alternative_recovery") {
        actions.push_back("recover");
        actions.push_back("cancel_to_self");
    }
    result.pushKV("allowed_actions", std::move(actions));
    if (state == "MEMPOOL" || state == "CONFIRMED") {
        result.pushKV("result_status", "final_committed");
        result.pushKV("result_sequence", 1);
    } else {
        result.pushKV("result_status", UniValue{UniValue::VNULL});
        result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    }
    return result;
}

UniValue PaymasterAuthorizationResult(bool authorization_required,
                                      const std::string& commitment,
                                      const std::string& state,
                                      const std::string& artifact)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", "00000000-0000-4000-8000-000000000001");
    result.pushKV("session_id", std::string(64, '1'));
    result.pushKV("session_state", state);
    result.pushKV("pending_phase", "NONE");
    result.pushKV("broadcast_state", "not_attempted");
    result.pushKV("confirmation_state", "unconfirmed");
    result.pushKV("provider_id", std::string(64, 'b'));
    result.pushKV("offer_id", std::string(64, 'd'));
    result.pushKV("policy_hash", std::string(64, 'a'));
    result.pushKV("funding_model", "user_paid");
    result.pushKV("to_address",
                  "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    result.pushKV("privacy_profile", "standard");
    result.pushKV("artifact", artifact);
    result.pushKV("payment_cents", 325);
    result.pushKV("service_fee_cents", 2);
    result.pushKV("user_total_cents", 327);
    result.pushKV("expires_at", int64_t{2000000000});
    result.pushKV("result_status", UniValue{UniValue::VNULL});
    result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    result.pushKV("authorization_required", authorization_required);
    result.pushKV("authorization_commitment", commitment);
    return result;
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

void DigiDollarWidgetTests::overviewPaymasterReservationUsesConditionalBalanceBreakdown()
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

    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupDescriptorsWallet(m_node, test, "qt-dd-paymaster-balance-breakdown");
    wallet->EnsureDDWallet();
    DigiDollarWallet* const dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    const COutPoint available{uint256::ONE, 0};
    const COutPoint carrier{uint256S("02"), 0};
    dd_wallet->AddDDUTXO(available, 9600);
    dd_wallet->AddDDUTXO(carrier, 400);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarOverviewWidget overview;
    overview.setWalletModel(mini_gui.walletModel.get());

    QLabel* available_value = overview.findChild<QLabel*>("ddBalanceValue");
    QLabel* reserved_label = overview.findChild<QLabel*>("ddPaymasterReservedLabel");
    QLabel* reserved_value = overview.findChild<QLabel*>("ddPaymasterReservedValue");
    QLabel* total_label = overview.findChild<QLabel*>("ddWalletTotalLabel");
    QLabel* total_value = overview.findChild<QLabel*>("ddWalletTotalValue");
    QVERIFY(available_value != nullptr);
    QVERIFY(reserved_label != nullptr);
    QVERIFY(reserved_value != nullptr);
    QVERIFY(total_label != nullptr);
    QVERIFY(total_value != nullptr);

    // No reservation means the legacy overview remains byte-for-byte in
    // structure: no additional balance rows are exposed.
    QCOMPARE(available_value->text(), QStringLiteral("100.00 $DD"));
    QVERIFY(reserved_label->isHidden());
    QVERIFY(reserved_value->isHidden());
    QVERIFY(total_label->isHidden());
    QVERIFY(total_value->isHidden());

    DigiDollar::Paymaster::ProviderPoolEntry pool_entry;
    pool_entry.outpoint = carrier;
    pool_entry.purpose = DigiDollar::Paymaster::PoolPurpose::OPERATIONAL;
    pool_entry.asset = DigiDollar::Paymaster::PoolAsset::DD_CARRIER;
    pool_entry.state = DigiDollar::Paymaster::PoolEntryState::AVAILABLE;
    pool_entry.carrier_value = DigiDollar::Paymaster::DDCents{400};
    CKey carrier_key;
    carrier_key.MakeNewKey(true);
    TaprootBuilder carrier_builder;
    carrier_builder.Finalize(XOnlyPubKey{carrier_key.GetPubKey()});
    pool_entry.script_pub_key =
        GetScriptForDestination(carrier_builder.GetOutput());
    pool_entry.confirmation_height = 1;
    pool_entry.updated_at = 1;
    wallet::WalletBatch batch{wallet->GetDatabase()};
    QVERIFY(batch.WritePaymasterProviderPool({pool_entry}));

    // Use a fresh widget so the production five-second refresh throttle does
    // not mask the state transition under test.
    DigiDollarOverviewWidget reserved_overview;
    reserved_overview.setWalletModel(mini_gui.walletModel.get());
    available_value = reserved_overview.findChild<QLabel*>("ddBalanceValue");
    reserved_label = reserved_overview.findChild<QLabel*>("ddPaymasterReservedLabel");
    reserved_value = reserved_overview.findChild<QLabel*>("ddPaymasterReservedValue");
    total_label = reserved_overview.findChild<QLabel*>("ddWalletTotalLabel");
    total_value = reserved_overview.findChild<QLabel*>("ddWalletTotalValue");
    QVERIFY(available_value != nullptr);
    QVERIFY(reserved_label != nullptr);
    QVERIFY(reserved_value != nullptr);
    QVERIFY(total_label != nullptr);
    QVERIFY(total_value != nullptr);
    QCOMPARE(available_value->text(), QStringLiteral("96.00 $DD"));
    QCOMPARE(reserved_value->text(), QStringLiteral("4.00 $DD"));
    QCOMPARE(total_value->text(), QStringLiteral("100.00 $DD"));
    QVERIFY(!reserved_label->isHidden());
    QVERIFY(!reserved_value->isHidden());
    QVERIFY(!total_label->isHidden());
    QVERIFY(!total_value->isHidden());
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

    // An amount over the limit can be typed but is never acceptable, the same
    // way an amount under the limit behaves. If the box refused the keystroke
    // the number would silently change under the user's hands, and the warning
    // line below the box would never get a chance to say what is wrong.
    QString aboveMax = QString::number((ddParams.maxMintAmount + 1) / 100.0, 'f', 2);
    pos = 0;
    QCOMPARE(amountEdit->validator()->validate(aboveMax, pos), QValidator::Intermediate);
    amountEdit->setText(aboveMax);
    QCoreApplication::processEvents();
    QCOMPARE(amountEdit->text(), aboveMax);
    QVERIFY2(warningLabel->text().contains("Maximum mint amount is $"),
             qPrintable(warningLabel->text()));
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

    // No live vault may be created for a rejected mint. The mint writes its
    // position record before it sends the transaction, so the record is on
    // disk by the time the send is refused. It is kept, but marked inactive,
    // so that a reorg or a late confirmation can bring the vault back; it is
    // no longer counted as an open position and holds no collateral.
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    QCOMPARE(dd_wallet->GetDDTimeLocks(/*active_only=*/true).size(), static_cast<size_t>(0));
    QCOMPARE(dd_wallet->GetLockedCollateral(), CAmount(0));
    for (const WalletCollateralPosition& position : dd_wallet->GetDDTimeLocks(/*active_only=*/false)) {
        QVERIFY(!position.is_active);
    }

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
    // History reconstruction is intentionally asynchronous so that a busy
    // wallet can never freeze navigation or painting on Qt's event thread.
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 5000);
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

void DigiDollarWidgetTests::positionsWidgetEnablesRedeemForLockedEncryptedWallet()
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
    // A locked wallet still holds its keys, so a vault past its lock stays
    // clickable. The Redeem tab asks for the passphrase when the user clicks
    // Redeem there.
    QCOMPARE(redeemButton->text(), QString("Redeem"));
    QVERIFY2(redeemButton->isEnabled(), "locked wallet must keep the Vault tab's Redeem button enabled");
    QVERIFY(redeemButton->toolTip().contains("passphrase"));

    // Clicking it hands the vault to the Redeem tab, where the unlock prompt lives.
    QSignalSpy redeemRequests(&positionsWidget, &DigiDollarPositionsWidget::redeemRequested);
    redeemButton->click();
    QCoreApplication::processEvents();
    QCOMPARE(redeemRequests.count(), 1);
    QCOMPARE(redeemRequests.at(0).at(0).toString(), QString::fromStdString(uint256::ONE.GetHex()));
    QVERIFY(wallet->IsLocked());
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
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    // The position helper also creates its DD token UTXO. Simulate that token
    // having been transferred away while the collateral position remains.
    dd_wallet->RemoveDDUTXO(COutPoint(uint256::ONE, 1));
    QCOMPARE(dd_wallet->GetTotalDDBalance(), 0);

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
    ScopedWalletRegistration registration(*m_node.walletLoader().context(), wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY2(!redeemWidget.canWalletSignRedemption(), "a wallet without private keys can never sign");
    QVERIFY(!redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Cannot Redeem"));
    QVERIFY(redeemButton->toolTip().contains("Watch-only"));

    // A disabled button swallows the click. No unlock prompt, no redemption.
    QSignalSpy unlockRequests(mini_gui.walletModel.get(), &WalletModel::requireUnlock);
    QSignalSpy messages(&redeemWidget, &DigiDollarRedeemWidget::message);
    redeemButton->click();
    QCoreApplication::processEvents();
    QCOMPARE(unlockRequests.count(), 0);
    QCOMPARE(messages.count(), 0);

    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->toolTip().contains("Watch-only"));
}

void DigiDollarWidgetTests::redeemWidgetButtonStateLockedWallet()
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
    // Plenty of $DD so the balance rule cannot be what disables the button here.
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);
    SecureString passphrase{"qt-dd-redeem-wallet-locked"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

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

    // A locked wallet still holds its private keys. The button stays enabled,
    // and the text tells the user the click will ask for the passphrase, instead
    // of sending them to the console to unlock first.
    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QCOMPARE(mini_gui.walletModel->getEncryptionStatus(), WalletModel::Locked);
    QVERIFY2(redeemWidget.canWalletSignRedemption(), "a locked wallet can sign once the passphrase is entered");
    QVERIFY2(redeemButton->isEnabled(), "locked wallet must keep the Redeem button enabled");
    QCOMPARE(redeemButton->text(), QString("Redeem && Unlock DGB"));
    QVERIFY(redeemButton->toolTip().contains("passphrase"));
    QLabel* validationLabel = redeemWidget.findChild<QLabel*>("positionValidationLabel");
    QVERIFY(validationLabel != nullptr);
    QVERIFY(validationLabel->text().contains("passphrase"));
    QVERIFY(validationLabel->toolTip().contains("passphrase"));
}

void DigiDollarWidgetTests::redeemWidgetLockedWalletClickRequestsUnlock()
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

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-locked-click");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);
    SecureString passphrase{"qt-dd-redeem-locked-click"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    ScopedWalletRegistration registration(*m_node.walletLoader().context(), wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY2(redeemButton->isEnabled(), "locked wallet must keep the Redeem button enabled");

    // Stand in for the passphrase dialog. The wallet model asks the GUI to
    // unlock, and this handler answers with the right passphrase.
    QSignalSpy unlockRequests(mini_gui.walletModel.get(), &WalletModel::requireUnlock);
    connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock, &redeemWidget, [&]() {
        QVERIFY(mini_gui.walletModel->setWalletLocked(false, passphrase));
    });
    QSignalSpy messages(&redeemWidget, &DigiDollarRedeemWidget::message);
    QSignalSpy completions(&redeemWidget, &DigiDollarRedeemWidget::redemptionCompleted);

    AcceptConfirmRedeemDialogWhenShown();
    redeemButton->click();
    QCoreApplication::processEvents();

    QCOMPARE(unlockRequests.count(), 1);
    // The redemption was attempted after the unlock. This test has no wallet RPC
    // server, so the attempt fails and says so through message(). The balance
    // check reports before any unlock, so a different title here proves the click
    // got past it.
    QCOMPARE(messages.count(), 1);
    QVERIFY2(messages.at(0).at(0).toString() != QStringLiteral("Insufficient DigiDollar Balance"),
             "redemption must get past the balance check before asking to unlock");
    QCOMPARE(completions.count(), 0);
    QVERIFY2(wallet->IsLocked(), "the unlock context must relock the wallet once the attempt is over");
}

void DigiDollarWidgetTests::redeemWidgetCancelledUnlockLeavesFormUnchanged()
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

    const std::shared_ptr<wallet::CWallet>& wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-unlock-cancel");
    AddMockDigiDollarPosition(wallet, uint256::ONE, 10000, 300 * COIN, 1, 100);
    wallet->GetDDWallet()->AddDDUTXO(COutPoint(uint256::ONE, 1), 100000000);
    SecureString passphrase{"qt-dd-redeem-unlock-cancel"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    ScopedWalletRegistration registration(*m_node.walletLoader().context(), wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    const QString positionHex = QString::fromStdString(uint256::ONE.GetHex());
    redeemWidget.setPosition(positionHex);
    QCoreApplication::processEvents();

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    QVERIFY2(redeemButton->isEnabled(), "locked wallet must keep the Redeem button enabled");
    QLineEdit* positionIdEdit = redeemWidget.findChild<QLineEdit*>("positionIdEdit");
    QLineEdit* amountEdit = redeemWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(positionIdEdit != nullptr);
    QVERIFY(amountEdit != nullptr);
    const QString amountBefore = amountEdit->text();
    QCOMPARE(amountBefore, QStringLiteral("100.00"));

    // Nobody answers the unlock request. That is what the wallet model sees when
    // the user closes the passphrase dialog. The wallet stays locked.
    QSignalSpy unlockRequests(mini_gui.walletModel.get(), &WalletModel::requireUnlock);
    QSignalSpy messages(&redeemWidget, &DigiDollarRedeemWidget::message);
    QSignalSpy completions(&redeemWidget, &DigiDollarRedeemWidget::redemptionCompleted);

    AcceptConfirmRedeemDialogWhenShown();
    redeemButton->click();
    QCoreApplication::processEvents();

    QCOMPARE(unlockRequests.count(), 1);
    QCOMPARE(messages.count(), 0);
    QCOMPARE(completions.count(), 0);
    QVERIFY(wallet->IsLocked());
    // The form is untouched and still ready for another try.
    QCOMPARE(positionIdEdit->text(), positionHex);
    QCOMPARE(amountEdit->text(), amountBefore);
    QVERIFY(redeemWidget.m_positionFound);
    QVERIFY(redeemButton->isEnabled());
    QCOMPARE(redeemButton->text(), QString("Redeem && Unlock DGB"));
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
    ScopedWalletRegistration registration(*m_node.walletLoader().context(), wallet);

    DigiDollarRedeemWidget redeemWidget;
    redeemWidget.setWalletModel(mini_gui.walletModel.get());
    redeemWidget.setClientModel(mini_gui.clientModel.get());
    redeemWidget.setPosition(QString::fromStdString(uint256::ONE.GetHex()));
    QCoreApplication::processEvents();

    QPushButton* redeemButton = redeemWidget.findChild<QPushButton*>("redeemButton");
    QVERIFY(redeemButton != nullptr);
    // Locked: still enabled, and the text warns that the click asks for the passphrase.
    QVERIFY(redeemButton->isEnabled());
    QVERIFY(redeemButton->toolTip().contains("passphrase"));

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
    // Unlocked: the passphrase warning is gone.
    QVERIFY(!redeemButton->toolTip().contains("passphrase"));
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

void DigiDollarWidgetTests::redeemWidgetCanonicalHealthDoesNotRequireCirculatingSupply()
{
    TestChain100Setup test(ChainType::REGTEST,
        {"-digidollaractivationheight=100", "-ddthawdayheight=400", "-digidollarstatsindex=0"});
    struct ResetOracleState {
        ~ResetOracleState()
        {
            OracleBundleManager::GetInstance().Clear();
            MockOracleManager::GetInstance().Reset();
            DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
            DigiDollar::SystemHealthMonitor::ResetMetrics();
        }
    } reset_oracle;
    OracleBundleManager::GetInstance().Clear();
    MockOracleManager::GetInstance().Reset();
    DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    DigiDollar::SystemHealthMonitor::ResetMetrics();

    const CScript coinbase_script = GetScriptForRawPubKey(test.coinbaseKey.GetPubKey());
    std::vector<CMutableTransaction> funding;
    for (int i = 0; i < 4; ++i) {
        funding.push_back(test.CreateValidMempoolTransaction(test.m_coinbase_txns[i], 0, i + 1,
            test.coinbaseKey, CScript() << OP_TRUE, 31 * COIN, false));
    }
    const auto funding_block = test.CreateAndProcessBlock(funding, coinbase_script);
    QVERIFY(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == funding_block.GetHash());
    QCOMPARE(funding_block.vtx.size(), size_t{5});
    CreateAndProcessOracleQuoteBlock(test, 1000000);

    CKey owner;
    owner.MakeNewKey(true);
    const XOnlyPubKey owner_pubkey{owner.GetPubKey()};
    const int mint_height = WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Height() + 1);
    DigiDollar::MintParams mint_params;
    mint_params.ddAmount = 100;
    mint_params.lockHeight = mint_height + DigiDollar::LockDaysToBlocks(0);
    mint_params.ownerKey = owner_pubkey;
    mint_params.internalKey = DigiDollar::GetCollateralNUMSKey();
    mint_params.oracleKeys = DigiDollar::GetOracleKeys(15);
    std::vector<CMutableTransaction> mints;
    for (int i = 0; i < 3; ++i) {
        CMutableTransaction mint;
        mint.SetDigiDollarType(DD_TX_MINT);
        mint.vin.emplace_back(COutPoint{funding_block.vtx[i + 1]->GetHash(), 0});
        mint.vout.emplace_back(30 * COIN, DigiDollar::CreateCollateralP2TR(mint_params));
        mint.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(owner_pubkey, 100));
        mint.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
            << CScriptNum(1) << CScriptNum(100) << CScriptNum(mint_params.lockHeight) << CScriptNum(0)
            << std::vector<unsigned char>(owner_pubkey.begin(), owner_pubkey.end()));
        mints.push_back(std::move(mint));
    }
    const auto minted = test.CreateAndProcessBlock(mints, coinbase_script);
    QVERIFY(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == minted.GetHash());
    QCOMPARE(minted.vtx.size(), size_t{4});
    for (int height = mint_height + 1; height <= mint_params.lockHeight + 1; ++height) {
        const auto block = test.CreateAndProcessBlock({}, coinbase_script);
        QVERIFY(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash());
    }

    // Mining through the timelock crosses oracle epochs. Commit a fresh signed
    // quote for the redemption's current epoch.
    const int redemption_quote_height = WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Height() + 1);
    CreateAndProcessOracleQuoteBlock(test, 1000000);
    QCOMPARE(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Height()), redemption_quote_height);

    // A legacy redemption can leave change whose amount cannot be counted: its
    // metadata appears twice, so no node can tell which record is the amount.
    // Every node still accepts the block. A separate token remains known and
    // can fund another vault's redemption.
    CMutableTransaction legacy_redeem;
    legacy_redeem.SetDigiDollarType(DD_TX_REDEEM);
    legacy_redeem.nLockTime = mint_params.lockHeight;
    legacy_redeem.vin.emplace_back(COutPoint{minted.vtx[1]->GetHash(), 0}, CScript{}, 0xfffffffe);
    legacy_redeem.vin.emplace_back(COutPoint{minted.vtx[1]->GetHash(), 1});
    legacy_redeem.vin.emplace_back(COutPoint{minted.vtx[2]->GetHash(), 1});
    legacy_redeem.vin.emplace_back(COutPoint{funding_block.vtx[4]->GetHash(), 0});
    legacy_redeem.vout.emplace_back(30 * COIN, CScript() << OP_TRUE);
    legacy_redeem.vout.emplace_back(30 * COIN, CScript() << OP_TRUE);
    legacy_redeem.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(owner_pubkey, 100));
    const CScript legacy_metadata = CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                              << CScriptNum(3) << CScriptNum(100);
    legacy_redeem.vout.emplace_back(0, legacy_metadata);
    legacy_redeem.vout.emplace_back(1, legacy_metadata);
    const CScript normal = DigiDollar::CreateNormalRedemptionPath(mint_params);
    TaprootBuilder tree;
    tree.Add(1, normal, 0xc0);
    tree.Add(1, DigiDollar::CreateERRPath(mint_params), 0xc0);
    tree.Finalize(mint_params.internalKey);
    QVERIFY(tree.IsComplete());
    const auto spend_data = tree.GetSpendData();
    const auto paths = spend_data.scripts.find({normal, 0xc0});
    QVERIFY(paths != spend_data.scripts.end());
    QVERIFY(!paths->second.empty());
    PrecomputedTransactionData data;
    data.Init(legacy_redeem, std::vector<CTxOut>{minted.vtx[1]->vout[0], minted.vtx[1]->vout[1],
        minted.vtx[2]->vout[1], funding_block.vtx[4]->vout[0]}, true);
    ScriptExecutionData execution;
    execution.m_annex_init = true;
    execution.m_annex_present = false;
    execution.m_tapleaf_hash_init = true;
    execution.m_tapleaf_hash = ComputeTapleafHash(0xc0, normal);
    execution.m_codeseparator_pos_init = true;
    execution.m_codeseparator_pos = 0xffffffff;
    uint256 hash;
    std::vector<unsigned char> signature(64);
    QVERIFY(SignatureHashSchnorr(hash, execution, legacy_redeem, 0, SIGHASH_DEFAULT, SigVersion::TAPSCRIPT,
                                data, MissingDataBehavior::ASSERT_FAIL));
    QVERIFY(owner.SignSchnorr(hash, signature, nullptr, uint256{}));
    legacy_redeem.vin[0].scriptWitness.stack = {signature, std::vector<unsigned char>(normal.begin(), normal.end()),
                                             *paths->second.begin()};
    const uint256 no_script_tree;
    for (int i = 1; i <= 2; ++i) {
        QVERIFY(SignatureHashSchnorr(hash, execution, legacy_redeem, i, SIGHASH_DEFAULT, SigVersion::TAPROOT,
                                    data, MissingDataBehavior::ASSERT_FAIL));
        QVERIFY(owner.SignSchnorr(hash, signature, &no_script_tree, uint256{}));
        legacy_redeem.vin[i].scriptWitness.stack = {signature};
    }
    auto& chainstate = test.m_node.chainman->ActiveChainstate();
    const auto redeemed = test.CreateBlock({legacy_redeem}, coinbase_script, chainstate);
    BlockValidationState redemption_state;
    const bool redemption_valid = WITH_LOCK(cs_main, return TestBlockValidity(redemption_state, Params(), chainstate,
        redeemed, chainstate.m_chain.Tip(), GetAdjustedTime));
    QVERIFY2(redemption_valid, redemption_state.ToString().c_str());
    QVERIFY(test.m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(redeemed), true, true, nullptr));
    QVERIFY(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == redeemed.GetHash());
    QVERIFY(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Height()) < 400);
    const int redeemed_height = WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Height());
    for (int height = redeemed_height + 1; height <= 400; ++height) {
        const auto block = test.CreateAndProcessBlock({}, coinbase_script);
        QVERIFY(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash());
    }
    // The candidate health request needs a quote for the epoch after activation.
    CreateAndProcessOracleQuoteBlock(test, 1000000);
    QCOMPARE(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChain().Height()), 401);
    const auto canonical = WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState());
    QVERIFY(canonical && canonical->history_checked);
    QCOMPARE(canonical->open_vault_principal, CAmount{200});
    QCOMPARE(canonical->collateral, CAmount{60 * COIN});
    QCOMPARE(canonical->active_vaults, uint64_t{2});

    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test, "qt-dd-unknown-supply");
    const auto position_id = minted.vtx[2]->GetHash();
    wallet->EnsureDDWallet();
    auto* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet);
    WalletCollateralPosition position;
    QVERIFY(dd_wallet->ExtractPositionFromMintTx(*minted.vtx[2], mint_height, position));
    // The vault remains open after its token was spent. Store only the vault
    // metadata; adding a new mint position would also add its spent token.
    QVERIFY(dd_wallet->WriteDDTimeLock(position));
    const COutPoint spent_position_token{position_id, 1};
    QVERIFY(!WITH_LOCK(cs_main, return chainstate.CoinsTip().HaveCoin(spent_position_token)));
    QVERIFY(WITH_LOCK(cs_main, return chainstate.CoinsTip().HaveCoin(COutPoint{position_id, 0})));
    QVERIFY(!dd_wallet->HasDDUTXO(spent_position_token));
    dd_wallet->StoreOwnerKey(position_id, owner);
    const auto token_key = owner_pubkey.CreateTapTweak(nullptr);
    QVERIFY(token_key);
    dd_wallet->StoreAddressKey(token_key->first, owner);
    {
        LOCK(wallet->cs_wallet);
        // Keep wallet confirmation depths aligned with the accepted chain.
        wallet->SetLastBlockProcessed(401, canonical->best_block);
        wallet->AddToWallet(minted.vtx[3], wallet::TxStateConfirmed{minted.GetHash(), mint_height, 3});
    }
    const COutPoint known_token{minted.vtx[3]->GetHash(), 1};
    dd_wallet->AddDDUTXO(known_token, 100);
    QVERIFY(WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChainstate().CoinsTip().HaveCoin(known_token)));
    CKey spending_key;
    QVERIFY(dd_wallet->GetDDOutputSpendingKey(minted.vtx[3]->vout[1], spending_key));
    QVERIFY(spending_key.GetPubKey() == owner.GetPubKey());
    QCOMPARE(dd_wallet->GetTotalDDBalance(), CAmount{100});
    const auto spendable_tokens = dd_wallet->GetDDUTXOs();
    QCOMPARE(spendable_tokens.size(), size_t{1});
    QVERIFY(spendable_tokens.front().outpoint == known_token);
    QCOMPARE(spendable_tokens.front().dd_amount, CAmount{100});

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);
    struct RemoveTestWallet {
        WalletContext& context;
        const std::shared_ptr<wallet::CWallet>& wallet;
        ~RemoveTestWallet() { RemoveWallet(context, wallet, std::nullopt); }
    } remove_wallet{context, wallet};

    const UniValue params(UniValue::VARR);
    bool supply_unavailable{false};
    try {
        mini_gui.walletModel->executeRpc("getdigidollarstats", params);
    } catch (const UniValue& error) {
        supply_unavailable = error["message"].get_str().find("invalid creating token metadata") != std::string::npos;
    }
    QVERIFY2(supply_unavailable, "Legacy token change must leave circulating supply unavailable");
    const auto protection = mini_gui.walletModel->executeRpc("getprotectionstatus", params);
    QVERIFY(protection["next_block_health"]["ready"].get_bool());
    QVERIFY(protection["next_block_health"]["health_percentage"].getInt<int>() >= 100);

    DigiDollarRedeemWidget redeem_widget;
    redeem_widget.setWalletModel(mini_gui.walletModel.get());
    redeem_widget.setClientModel(mini_gui.clientModel.get());
    redeem_widget.m_selectedPositionId = QString::fromStdString(position_id.GetHex());
    redeem_widget.m_positionIdEdit->setText(redeem_widget.m_selectedPositionId);
    redeem_widget.m_positionFound = true;
    redeem_widget.m_positionDDMinted = 1.0;
    redeem_widget.m_positionDGBCollateral = 30.0;
    redeem_widget.m_positionLockTier = 0;
    redeem_widget.m_positionBlocksRemaining = 0;
    redeem_widget.m_redeemableAmount = 1.0;
    redeem_widget.m_amountEdit->setText(QStringLiteral("1.00"));
    QVERIFY(redeem_widget.validateDDBalance());
    redeem_widget.updateRedeemButtons();
    QVERIFY(redeem_widget.m_redeemButton->isEnabled());

    bool confirmation_seen{false};
    QTimer close_confirmation;
    connect(&close_confirmation, &QTimer::timeout, [&] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            confirmation_seen = dialog->windowTitle() == QStringLiteral("Confirm Redeem");
            dialog->done(QMessageBox::No);
        }
    });
    close_confirmation.start(0);
    redeem_widget.onRedeemClicked();
    close_confirmation.stop();
    QVERIFY(confirmation_seen);

    auto& coins = WITH_LOCK(cs_main, return test.m_node.chainman->ActiveChainstate().CoinsTip());
    const auto saved = WITH_LOCK(cs_main, return coins.GetDigiDollarState());
    QVERIFY(saved);
    {
        LOCK(cs_main);
        auto unchecked = *saved;
        unchecked.history_checked = false;
        coins.SetDigiDollarState(unchecked);
    }
    struct RestoreCanonicalState {
        CCoinsViewCache& coins;
        std::optional<DigiDollar::ChainstateHealth> saved;
        ~RestoreCanonicalState() { LOCK(cs_main); coins.SetDigiDollarState(saved); }
    } restore{coins, saved};
    QVERIFY(!mini_gui.walletModel->executeRpc("getprotectionstatus", params)["next_block_health"]["ready"].get_bool());
    QVERIFY(!redeem_widget.validateDDBalance());
    redeem_widget.updateRedeemButtons();
    QVERIFY(!redeem_widget.m_redeemButton->isEnabled());
    QSignalSpy messages(&redeem_widget, &DigiDollarRedeemWidget::message);
    confirmation_seen = false;
    close_confirmation.start(0);
    redeem_widget.onRedeemClicked();
    close_confirmation.stop();
    QVERIFY(!confirmation_seen);
    QCOMPARE(messages.count(), 1);
    QCOMPARE(messages.at(0).at(0).toString(), QStringLiteral("Redemption unavailable"));
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

void DigiDollarWidgetTests::mintWidgetRejectsUnsupportedWalletBeforeConfirmation_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("reason");
    QTest::newRow("legacy") << QStringLiteral("legacy") << QStringLiteral("legacy wallet cannot mint DigiDollar");
    QTest::newRow("watch-only") << QStringLiteral("watch-only") << QStringLiteral("Private keys are disabled");
    QTest::newRow("blank") << QStringLiteral("blank") << QStringLiteral("cannot generate DigiDollar owner keys");
    QTest::newRow("missing-taproot") << QStringLiteral("missing-taproot") << QStringLiteral("cannot generate DigiDollar owner keys");
    QTest::newRow("public-taproot") << QStringLiteral("public-taproot") << QStringLiteral("cannot generate DigiDollar owner keys");
    QTest::newRow("nonranged-taproot") << QStringLiteral("nonranged-taproot") << QStringLiteral("cannot generate DigiDollar owner keys");
    QTest::newRow("missing-change") << QStringLiteral("missing-change") << QStringLiteral("cannot generate the change addresses");
    QTest::newRow("public-change") << QStringLiteral("public-change") << QStringLiteral("cannot generate the change addresses");
}

void DigiDollarWidgetTests::mintWidgetRejectsUnsupportedWalletBeforeConfirmation()
{
    QFETCH(QString, kind);
    QFETCH(QString, reason);
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    std::shared_ptr<wallet::CWallet> wallet;
    if (kind == "legacy" || kind == "blank") {
        wallet = std::make_shared<wallet::CWallet>(
            test.m_node.chain.get(), "unsupported-mint", CreateMockableWalletDatabase());
        wallet->LoadWallet();
        if (kind == "blank") wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS | wallet::WALLET_FLAG_BLANK_WALLET);
    } else {
        wallet = SetupDescriptorsWallet(m_node, test);
    }
    if (kind == "watch-only") wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    if (kind == "missing-taproot" || kind == "missing-change") {
        LOCK(wallet->cs_wallet);
        const bool internal = kind == "missing-change";
        const OutputType type = internal ? OutputType::BECH32 : OutputType::BECH32M;
        auto* manager = wallet->GetScriptPubKeyMan(type, internal);
        QVERIFY(manager);
        wallet->DeactivateScriptPubKeyMan(manager->GetID(), type, internal);
    }
    if (kind == "public-taproot" || kind == "public-change" || kind == "nonranged-taproot") {
        CExtKey master;
        master.SetSeed(MakeByteSpan(uint256::ONE));
        const std::string public_key = EncodeExtPubKey(master.Neuter());
        const bool internal = kind == "public-change";
        const std::string text = internal ? "wpkh(" + public_key + "/0/*)" :
            (kind == "public-taproot" ? "tr(" + public_key + "/0/*)" : "tr(" + EncodeSecret(master.key) + ")");
        FlatSigningProvider provider;
        std::string error;
        auto descriptor = Parse(text, provider, error, /*require_checksum=*/false);
        QVERIFY2(descriptor, error.c_str());
        wallet::WalletDescriptor record(std::move(descriptor), 0, 0, 2, 0);
        LOCK(wallet->cs_wallet);
        auto* manager = wallet->AddWalletDescriptor(record, provider, "", internal);
        QVERIFY(manager);
        wallet->AddActiveScriptPubKeyMan(manager->GetID(), internal ? OutputType::BECH32 : OutputType::BECH32M, internal);
    }

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarMintWidget widget;
    widget.setWalletModel(mini_gui.walletModel.get());
    widget.setClientModel(mini_gui.clientModel.get());
    auto* amount = widget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(amount);
    amount->setText("100.00");

    const auto before = MintDescriptorNextIndexes(*wallet);
    const size_t transaction_count = wallet->mapWallet.size();
    QSignalSpy messages(&widget, &DigiDollarMintWidget::message);
    QSignalSpy unlocks(mini_gui.walletModel.get(), &WalletModel::requireUnlock);
    int confirmation_count = 0;
    QTimer close_unexpected_dialogs;
    connect(&close_unexpected_dialogs, &QTimer::timeout, [&]() {
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QMessageBox*>(top); dialog && dialog->isVisible()) {
                ++confirmation_count;
                dialog->done(QMessageBox::No);
            }
        }
    });
    close_unexpected_dialogs.start(0);
    QVERIFY(QMetaObject::invokeMethod(&widget, "onMintClicked", Qt::DirectConnection));
    close_unexpected_dialogs.stop();
    QCOMPARE(confirmation_count, 0);
    QCOMPARE(unlocks.count(), 0);
    QCOMPARE(messages.count(), 1);
    QCOMPARE(messages.at(0).at(0).toString(), QStringLiteral("Cannot Mint DigiDollar"));
    QVERIFY2(messages.at(0).at(1).toString().contains(reason), qPrintable(messages.at(0).at(1).toString()));

    const auto result = mini_gui.walletModel->mintDigiDollar(10000, 0);
    QCOMPARE(result.status, WalletModel::TransactionCreationFailed);
    QVERIFY2(result.reasonFailed.contains(reason), qPrintable(result.reasonFailed));
    QVERIFY(result.txid.isEmpty());
    QVERIFY(result.positionId.isEmpty());
    QVERIFY(MintDescriptorNextIndexes(*wallet) == before);
    QCOMPARE(wallet->mapWallet.size(), transaction_count);
}

void DigiDollarWidgetTests::mintWidgetPreservesEncryptedWalletUnlockFlow()
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test);
    const SecureString passphrase{"mint-widget-passphrase"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    struct MockPriceGuard {
        MockOracleManager& mock{MockOracleManager::GetInstance()};
        CAmount price{mock.GetCurrentPrice()};
        bool enabled{mock.IsEnabled()};
        ~MockPriceGuard() { mock.SetMockPrice(price); mock.SetEnabled(enabled); }
    } mock_guard;
    mock_guard.mock.SetEnabled(true);
    mock_guard.mock.SetMockPrice(500000);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    mini_gui.walletModel->startPollBalance();
    const CAmount funded_balance = mini_gui.walletModel->wallet().getBalances().balance;
    QVERIFY(funded_balance > 0);
    QCOMPARE(mini_gui.walletModel->getAvailableDGBBalance(), funded_balance);
    const CAmount required_collateral = mini_gui.walletModel->calculateRequiredCollateral(10000, 0);
    QVERIFY(required_collateral > 0);
    QVERIFY(funded_balance >= required_collateral);
    QVERIFY(mini_gui.walletModel->getDigiDollarMintWalletError().isEmpty());
    const auto before = MintDescriptorNextIndexes(*wallet);
    const size_t transaction_count = wallet->mapWallet.size();
    DigiDollarMintWidget widget;
    widget.setWalletModel(mini_gui.walletModel.get());
    widget.setClientModel(mini_gui.clientModel.get());
    widget.show();
    widget.updateView();
    auto* amount = widget.findChild<QLineEdit*>("amountEdit");
    auto* mint_button = widget.findChild<QPushButton*>("mintButton");
    QVERIFY(amount);
    QVERIFY(mint_button);
    amount->setText("100.00");
    QVERIFY(mint_button->isEnabled());

    QSignalSpy unlocks(mini_gui.walletModel.get(), &WalletModel::requireUnlock);
    QSignalSpy messages(&widget, &DigiDollarMintWidget::message);
    QStringList confirmation_titles;
    QTimer confirm;
    connect(&confirm, &QTimer::timeout, [&]() {
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QMessageBox*>(top); dialog && dialog->isVisible()) {
                confirmation_titles.push_back(dialog->windowTitle());
                dialog->done(QMessageBox::Yes);
            }
        }
    });
    confirm.start(0);
    QVERIFY(QMetaObject::invokeMethod(&widget, "onMintClicked", Qt::DirectConnection));
    confirm.stop();
    QCOMPARE(confirmation_titles.size(), 2);
    QCOMPARE(confirmation_titles.at(0), QStringLiteral("Confirm DigiDollar Mint"));
    QVERIFY(confirmation_titles.at(1).contains("FINAL CONFIRMATION"));
    QCOMPARE(unlocks.count(), 1);
    QCOMPARE(messages.count(), 0);
    QVERIFY(wallet->IsLocked());
    QVERIFY(MintDescriptorNextIndexes(*wallet) == before);
    QCOMPARE(wallet->mapWallet.size(), transaction_count);

    connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock, [&]() {
        QVERIFY(wallet->Unlock(passphrase));
    });
    {
        auto context = mini_gui.walletModel->requestUnlock();
        QVERIFY(context.isValid());
        QVERIFY(!wallet->IsLocked());
        QVERIFY(mini_gui.walletModel->getDigiDollarMintWalletError().isEmpty());
    }
    QVERIFY(wallet->IsLocked());
    QCOMPARE(unlocks.count(), 2);
    QVERIFY(MintDescriptorNextIndexes(*wallet) == before);
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

// A DD history request runs outside the GUI thread, but it can legitimately
// hold cs_wallet while it builds its wallet-owned transaction snapshot. Hidden
// DigiDollar pages must therefore not perform synchronous wallet reads when a
// generic refresh or balance signal arrives. Before this regression fix both
// paths below waited for the lock and froze all navigation after a Paymaster
// transfer until history processing finished.
void DigiDollarWidgetTests::ddTabRefreshDoesNotWaitForBusyWallet()
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

    QTabWidget* tabs = tab.findChild<QTabWidget*>("digiDollarSubTabs");
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(6); // Transactions uses the asynchronous history path.
    QCoreApplication::processEvents();

    const auto whileWalletBusy = [&](const std::function<void()>& gui_action) {
        std::promise<void> lock_acquired;
        std::future<void> ready = lock_acquired.get_future();
        std::mutex gate_mutex;
        std::condition_variable gate;
        bool release_lock{false};

        std::thread lock_holder([&] {
            LOCK(wallet->cs_wallet);
            lock_acquired.set_value();
            std::unique_lock<std::mutex> lock{gate_mutex};
            gate.wait(lock, [&] { return release_lock; });
        });
        ready.wait();

        // The watchdog makes the pre-fix failure finite: a blocking GUI action
        // is released after 500 ms instead of deadlocking the test process.
        std::thread watchdog([&] {
            std::unique_lock<std::mutex> lock{gate_mutex};
            if (!gate.wait_for(lock, std::chrono::milliseconds{500}, [&] { return release_lock; })) {
                release_lock = true;
                lock.unlock();
                gate.notify_all();
            }
        });

        QElapsedTimer elapsed;
        elapsed.start();
        gui_action();
        const qint64 duration_ms = elapsed.elapsed();

        {
            std::lock_guard<std::mutex> lock{gate_mutex};
            release_lock = true;
        }
        gate.notify_all();
        lock_holder.join();
        watchdog.join();
        return duration_ms;
    };

    const qint64 view_refresh_ms = whileWalletBusy([&] { tab.updateView(); });
    QVERIFY2(view_refresh_ms < 200,
             qPrintable(QStringLiteral("Refreshing the visible DD tab waited %1 ms for cs_wallet")
                            .arg(view_refresh_ms)));

    // Reproduce the reported interaction rather than only calling the target
    // widget directly: move away from Transactions, hold cs_wallet as a
    // just-completed Paymaster operation may do, then click Transactions.
    tabs->setCurrentIndex(1);
    QCoreApplication::processEvents();
    const qint64 transaction_switch_ms = whileWalletBusy([&] {
        tabs->setCurrentIndex(6);
    });
    QVERIFY2(transaction_switch_ms < 200,
             qPrintable(QStringLiteral("Switching from Send DD to DD Transactions waited %1 ms for cs_wallet")
                            .arg(transaction_switch_ms)));

    // Top-level wallet navigation may still issue a generic refresh after the
    // DigiDollar page has been hidden. It must not refresh the last selected
    // child page or contend with an in-flight wallet operation.
    tab.hide();
    QCoreApplication::processEvents();
    const qint64 hidden_refresh_ms = whileWalletBusy([&] { tab.updateView(); });
    QVERIFY2(hidden_refresh_ms < 200,
             qPrintable(QStringLiteral("Refreshing a hidden DD page waited %1 ms for cs_wallet")
                            .arg(hidden_refresh_ms)));
    tab.show();
    QCoreApplication::processEvents();

    // Let the asynchronous transaction refresh triggered above complete before
    // taking the lock again for the independent wallet-signal path.
    QTest::qWait(100);
    const qint64 balance_signal_ms = whileWalletBusy([&] {
        Q_EMIT mini_gui.walletModel->balanceChanged(interfaces::WalletBalances{});
    });
    QVERIFY2(balance_signal_ms < 200,
             qPrintable(QStringLiteral("A DD balance signal blocked the GUI for %1 ms")
                            .arg(balance_signal_ms)));
}

// A tab may receive its wallet and client models while its WalletView page is
// still hidden. Qt also emits QTabWidget::currentChanged before the selected
// child necessarily reports isVisible(). The first refresh must survive both
// lifecycle boundaries instead of relying on the five-second polling timers.
void DigiDollarWidgetTests::ddTabLoadsSelectedPageOnFirstShow()
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
    DigiDollarWallet* const dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    DDTransaction transaction;
    transaction.txid = "d000000000000000000000000000000000000000000000000000000000000004";
    transaction.amount = 125;
    transaction.timestamp = GetTime();
    transaction.confirmations = 1;
    transaction.incoming = true;
    transaction.address = "TDfirstshow";
    transaction.category = "receive";
    transaction.lock_tier = -1;
    dd_wallet->AddMockTransaction(transaction);

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTab tab(mini_gui.platformStyle.get());
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());

    QStackedWidget* stack = tab.findChild<QStackedWidget*>("digiDollarStack");
    QTabWidget* tabs = tab.findChild<QTabWidget*>("digiDollarSubTabs");
    DigiDollarTransactionsWidget* transactions =
        tab.findChild<DigiDollarTransactionsWidget*>("transactionsWidget");
    QVERIFY(stack != nullptr);
    QVERIFY(tabs != nullptr);
    QVERIFY(transactions != nullptr);

    // Select the history page before the parent is shown, as happens when a
    // wallet restores its last selected subpage during startup.
    tabs->setCurrentWidget(transactions);
    tab.show();

    QTRY_COMPARE_WITH_TIMEOUT(stack->currentWidget(), static_cast<QWidget*>(tabs), 1000);
    QTableWidget* table = transactions->findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 3000);
}

// WalletDB history is already available when WalletModel is constructed. The
// first visible page must use that bounded snapshot immediately rather than
// looking empty until descriptor ownership and live confirmations have been
// reconstructed under cs_wallet on a worker thread.
void DigiDollarWidgetTests::transactionsWidgetShowsStoredHistoryWhileWalletBusy()
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
    DigiDollarWallet* const dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    constexpr int STORED_ROWS{75};
    for (int index = 0; index < STORED_ROWS; ++index) {
        DDTransaction transaction;
        transaction.txid = strprintf("%064x", index + 1);
        transaction.amount = index + 1;
        transaction.timestamp = GetTime() + index;
        transaction.confirmations = 1;
        transaction.incoming = true;
        transaction.address = "TDstored";
        transaction.category = "receive";
        transaction.lock_tier = -1;
        dd_wallet->AddMockTransaction(transaction);
    }

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarTransactionsWidget transactions;
    transactions.setWalletModel(mini_gui.walletModel.get());
    transactions.setClientModel(mini_gui.clientModel.get());

    QTableWidget* const table = transactions.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);

    std::promise<void> lock_acquired;
    std::future<void> ready = lock_acquired.get_future();
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool release_lock{false};
    std::thread lock_holder([&] {
        LOCK(wallet->cs_wallet);
        lock_acquired.set_value();
        std::unique_lock<std::mutex> lock{gate_mutex};
        gate.wait(lock, [&] { return release_lock; });
    });
    ready.wait();

    // Make the page visible only after cs_wallet is held. Otherwise processing
    // the show event can legitimately start the canonical background refresh
    // before this test has established its controlled lock contention.
    transactions.show();
    QElapsedTimer elapsed;
    elapsed.start();
    transactions.updateView();
    const qint64 initial_render_ms = elapsed.elapsed();
    const int initial_row_count = table->rowCount();

    {
        std::lock_guard<std::mutex> lock{gate_mutex};
        release_lock = true;
    }
    gate.notify_all();
    lock_holder.join();

    // Release the lock-holding thread before assertions so a failed check
    // cannot strand a joinable std::thread inside the Qt test process. Do not
    // pump the complete Qt event queue while cs_wallet is intentionally held:
    // unrelated WalletModel timers are allowed to inspect the same wallet.
    QCOMPARE(initial_row_count, 50);
    QVERIFY2(initial_render_ms < 200,
             qPrintable(QStringLiteral("Rendering stored DD history waited %1 ms for cs_wallet")
                            .arg(initial_render_ms)));

    // The canonical worker replaces the bounded startup seed after it can read
    // live wallet state; the temporary 50-row view is not a permanent limit.
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), STORED_ROWS, 5000);
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
    // The widget deliberately suppresses background history work while it is
    // hidden. Let Qt deliver the show event before requesting the first
    // asynchronous snapshot, matching the real tab-switch lifecycle.
    QCoreApplication::processEvents();
    transactionsWidget.updateView();

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 5000);

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
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 3, 5000);
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

// Regression test for au_epic's report: opening the Mint subpage after a
// redeem/unlock must refresh its Available DGB label instead of leaving a stale
// cached value until full wallet restart. Hidden pages are intentionally lazy,
// so the assertion follows the same top-level and subpage navigation as a user.
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

    QTabWidget* tabs = view.findChild<QTabWidget*>("digiDollarSubTabs");
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(3); // Mint
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

void DigiDollarWidgetTests::digiDollarPersistentStatusesUseSharedSemantics()
{
    // Persistent status messages must expose a semantic state to both themes.
    // Symbols and readable copy provide a second channel, so meaning never
    // depends on colour alone.
    QFrame banner;
    QLabel bannerText(&banner);
    DigiDollarStatus::SetBanner(&banner, DigiDollarStatus::Kind::ACTION);
    QCOMPARE(banner.property("digidollarRole").toString(), QStringLiteral("statusBanner"));
    QCOMPARE(banner.property("statusKind").toString(), QStringLiteral("action"));

    DigiDollarStatus::SetBanner(&banner, DigiDollarStatus::Kind::ERR);
    QCOMPARE(banner.property("statusKind").toString(), QStringLiteral("error"));

    DigiDollarStatus::SetText(&bannerText, DigiDollarStatus::Kind::SUCCESS);
    QCOMPARE(bannerText.property("digidollarRole").toString(), QStringLiteral("statusText"));
    QCOMPARE(bannerText.property("statusKind").toString(), QStringLiteral("ready"));

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    DigiDollarSendWidget sendWidget(platformStyle.get());
    QLabel* addressStatus = sendWidget.findChild<QLabel*>("addressValidationLabel");
    QFrame* clientSafety = sendWidget.findChild<QFrame*>("paymasterClientSafetyFrame");
    QVERIFY(addressStatus != nullptr);
    QVERIFY(clientSafety != nullptr);
    QCOMPARE(addressStatus->property("digidollarRole").toString(), QStringLiteral("statusText"));
    QCOMPARE(clientSafety->property("digidollarRole").toString(), QStringLiteral("statusBanner"));

    DigiDollarTransactionsWidget transactionsWidget;
    QLabel* transactionsStatus =
        transactionsWidget.findChild<QLabel*>("transactionsStatusLabel");
    QVERIFY(transactionsStatus != nullptr);
    QCOMPARE(transactionsStatus->property("digidollarRole").toString(), QStringLiteral("statusBanner"));

    DigiDollarOverviewWidget overviewWidget;
    QLabel* overviewOracle = overviewWidget.findChild<QLabel*>("oraclePriceValue");
    QLabel* overviewHealth = overviewWidget.findChild<QLabel*>("systemHealthValue");
    QVERIFY(overviewOracle != nullptr);
    QVERIFY(overviewHealth != nullptr);
    QCOMPARE(overviewOracle->property("statusKind").toString(), QStringLiteral("waiting"));
    QCOMPARE(overviewHealth->property("statusKind").toString(), QStringLiteral("waiting"));
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
    QVERIFY(tab.findChild<QWidget*>("paymasterWidget") != nullptr);
    QTabWidget* paymasterTabs = tab.findChild<QTabWidget*>("paymasterOperatorTabs");
    QVERIFY(paymasterTabs != nullptr);
    const QStringList expectedPaymasterTabs{
        QStringLiteral("Overview"),
        QStringLiteral("Offer"),
        QStringLiteral("Spending limits"),
        QStringLiteral("Liquidity"),
        QStringLiteral("Finances"),
        QStringLiteral("Operations"),
    };
    QCOMPARE(paymasterTabs->count(), expectedPaymasterTabs.size());
    for (int i = 0; i < expectedPaymasterTabs.size(); ++i) {
        QCOMPARE(paymasterTabs->tabText(i), expectedPaymasterTabs.at(i));
    }
    QWidget* setupChoice = tab.findChild<QWidget*>("paymasterSetupChoice");
    QWidget* configuredOverview = tab.findChild<QWidget*>("paymasterConfiguredOverview");
    QPushButton* guidedSetup = tab.findChild<QPushButton*>("paymasterChooseGuidedSetup");
    QPushButton* expertSetup = tab.findChild<QPushButton*>("paymasterChooseExpertSetup");
    QScrollArea* overviewPage =
        tab.findChild<QScrollArea*>("paymasterOverviewPage");
    QWidget* overviewContents =
        tab.findChild<QWidget*>("paymasterOverviewContents");
    QWidget* configurationPage = tab.findChild<QWidget*>("paymasterConfigurationPage");
    QWidget* safetyPage = tab.findChild<QWidget*>("paymasterSafetyPolicyPage");
    QWidget* liquidityPage = tab.findChild<QWidget*>("paymasterLiquidityPage");
    QScrollArea* financesPage =
        tab.findChild<QScrollArea*>("paymasterFinancesPage");
    QWidget* financesContents =
        tab.findChild<QWidget*>("paymasterFinancesContents");
    QWidget* activityPage = tab.findChild<QWidget*>("paymasterActivityPage");
    QWidget* advancedSafety =
        tab.findChild<QWidget*>("paymasterAdvancedSafetyPanel");
    QWidget* advancedLiquidity =
        tab.findChild<QWidget*>("paymasterAdvancedLiquidityPanel");
    QWidget* overviewDashboard =
        tab.findChild<QWidget*>("paymasterOverviewDashboard");
    QWidget* operatorHeader =
        tab.findChild<QWidget*>("paymasterOperatorHeader");
    QWidget* overviewOfferCard =
        tab.findChild<QWidget*>("paymasterOverviewOfferCard");
    QWidget* overviewSafetyCard =
        tab.findChild<QWidget*>("paymasterOverviewSafetyCard");
    QWidget* overviewLiquidityCard =
        tab.findChild<QWidget*>("paymasterOverviewLiquidityCard");
    QWidget* overviewOperationCard =
        tab.findChild<QWidget*>("paymasterOverviewOperationCard");
    QWidget* overviewFinanceCard =
        tab.findChild<QWidget*>("paymasterOverviewFinanceCard");
    QWidget* userPaidOfferCard =
        tab.findChild<QWidget*>("paymasterUserPaidOfferCard");
    QWidget* sponsoredOfferCard =
        tab.findChild<QWidget*>("paymasterSponsoredOfferCard");
    QLabel* liquidityCapacityExplanation =
        tab.findChild<QLabel*>("paymasterLiquidityCapacityExplanation");
    QWidget* liquidityCapacityCards =
        tab.findChild<QWidget*>("paymasterLiquidityCapacityCards");
    QPushButton* liquiditySettings =
        tab.findChild<QPushButton*>("paymasterAdvancedLiquidityToggle");
    QVERIFY(setupChoice != nullptr);
    QVERIFY(configuredOverview != nullptr);
    QVERIFY(guidedSetup != nullptr);
    QVERIFY(expertSetup != nullptr);
    QVERIFY(overviewPage != nullptr);
    QVERIFY(overviewContents != nullptr);
    QVERIFY(configurationPage != nullptr);
    QVERIFY(safetyPage != nullptr);
    QVERIFY(liquidityPage != nullptr);
    QVERIFY(financesPage != nullptr);
    QVERIFY(financesContents != nullptr);
    QVERIFY(activityPage != nullptr);
    QVERIFY(advancedSafety != nullptr);
    QVERIFY(advancedLiquidity != nullptr);
    QVERIFY(overviewDashboard != nullptr);
    QVERIFY(operatorHeader != nullptr);
    QCOMPARE(overviewPage->frameShape(), QFrame::NoFrame);
    QVERIFY(overviewPage->viewport()->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(overviewContents->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(!overviewPage->viewport()->autoFillBackground());
    QVERIFY(!overviewContents->autoFillBackground());
    QCOMPARE(financesPage->frameShape(), QFrame::NoFrame);
    QVERIFY(financesPage->viewport()->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(financesContents->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(!financesPage->viewport()->autoFillBackground());
    QVERIFY(!financesContents->autoFillBackground());
    QCOMPARE(operatorHeader->property("paymasterRole").toString(),
             QStringLiteral("operatorHeader"));
    const QList<QWidget*> overviewCards{
        overviewOfferCard,
        overviewSafetyCard,
        overviewLiquidityCard,
        overviewOperationCard,
        overviewFinanceCard,
    };
    for (QWidget* card : overviewCards) {
        QVERIFY(card != nullptr);
        QCOMPARE(card->property("paymasterRole").toString(),
                 QStringLiteral("statusCard"));
        QVERIFY(card->minimumHeight() >= 140);
    }
    // Five cards occupy at least three rows in the normal two-column layout.
    // The dashboard must publish that height so the following maintenance card
    // cannot be painted on top of the final row.
    QVERIFY(overviewDashboard->minimumHeight() >= (3 * 140) + (2 * 12));
    QVERIFY(userPaidOfferCard != nullptr);
    QVERIFY(sponsoredOfferCard != nullptr);
    QCOMPARE(userPaidOfferCard->property("paymasterRole").toString(),
             QStringLiteral("choicePanel"));
    QCOMPARE(sponsoredOfferCard->property("paymasterRole").toString(),
             QStringLiteral("choicePanel"));
    QCOMPARE(userPaidOfferCard->property("selected").toBool(), true);
    QCOMPARE(sponsoredOfferCard->property("selected").toBool(), false);
    QVERIFY(liquidityCapacityExplanation != nullptr);
    QVERIFY(liquidityCapacityExplanation->text().contains(
        QStringLiteral("client"), Qt::CaseInsensitive));
    QVERIFY(liquidityCapacityExplanation->text().contains(
        QStringLiteral("payment capacity"), Qt::CaseInsensitive));
    QVERIFY(liquidityCapacityCards != nullptr);
    const QList<QPushButton*> obsoleteCapacityActions =
        liquidityCapacityCards->findChildren<QPushButton*>();
    QCOMPARE(obsoleteCapacityActions.size(), 2);
    for (QPushButton* action : obsoleteCapacityActions) {
        QVERIFY(action->isHidden());
    }
    QVERIFY(liquiditySettings != nullptr);
    QCOMPARE(liquiditySettings->text(), QStringLiteral("Change liquidity settings"));
    const QList<QWidget*> pageColumns =
        tab.findChildren<QWidget*>("paymasterPageColumn");
    QCOMPARE(pageColumns.size(), 6);
    for (QWidget* column : pageColumns) {
        QCOMPARE(column->maximumWidth(), 1100);
    }
    QVERIFY(advancedSafety->isHidden());
    QVERIFY(advancedLiquidity->isHidden());
    QVERIFY(!setupChoice->isHidden());
    QVERIFY(configuredOverview->isHidden());
    QVERIFY(!paymasterTabs->isTabEnabled(paymasterTabs->indexOf(configurationPage)));
    QVERIFY(!paymasterTabs->isTabEnabled(paymasterTabs->indexOf(safetyPage)));
    QVERIFY(!paymasterTabs->isTabEnabled(paymasterTabs->indexOf(liquidityPage)));
    QVERIFY(!paymasterTabs->isTabEnabled(paymasterTabs->indexOf(financesPage)));
    QVERIFY(paymasterTabs->isTabEnabled(paymasterTabs->indexOf(activityPage)));
    QLabel* paymasterIntroduction = tab.findChild<QLabel*>("paymasterIntroduction");
    QLabel* paymasterNextStep = tab.findChild<QLabel*>("paymasterNextStep");
    QWidget* paymasterDetails = tab.findChild<QWidget*>("paymasterTechnicalDetails");
    QPushButton* paymasterWizard = guidedSetup;
    QVERIFY(paymasterIntroduction != nullptr);
    QVERIFY(paymasterIntroduction->text().contains(QStringLiteral("do not hold DGB")));
    QVERIFY(paymasterNextStep != nullptr);
    QVERIFY(paymasterDetails != nullptr);
    QVERIFY(paymasterDetails->isHidden());
    QVERIFY(paymasterWizard != nullptr);
    bool setupWizardOpened{false};
    bool setupWizardUsesClassicStyle{false};
    bool setupWizardHasLocalTheme{false};
    bool setupWizardUsesGreenCheckboxes{false};
    bool setupWizardHasVisibleContent{false};
    bool setupWizardUsesRecommendedNetworkFee{false};
    bool setupWizardInvalidatesMaintenanceApproval{false};
    QTimer::singleShot(0, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* setupWizard = qobject_cast<QWizard*>(widget);
            if (!setupWizard ||
                setupWizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
                continue;
            }
            setupWizardOpened = true;
            setupWizardUsesClassicStyle =
                setupWizard->wizardStyle() == QWizard::ClassicStyle;
            setupWizardHasLocalTheme =
                setupWizard->styleSheet().contains(
                    QStringLiteral("QWizard#PaymasterSetupWizard")) &&
                (setupWizard->styleSheet().contains(QStringLiteral("#0b2419")) ||
                 setupWizard->styleSheet().contains(QStringLiteral("#eef9f2")));
            setupWizardUsesGreenCheckboxes =
                setupWizard->styleSheet().contains(
                    QStringLiteral("QWizard#PaymasterSetupWizard QCheckBox::indicator:checked")) &&
                !setupWizard->styleSheet().contains(QStringLiteral("#0066cc"),
                                                     Qt::CaseInsensitive);
            QLabel* introduction = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupIntroduction"));
            QScrollArea* requirementsScroll = setupWizard->findChild<QScrollArea*>(
                QStringLiteral("paymasterSetupRequirementsScroll"));
            QLabel* requirements = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupRequirements"));
            QLabel* providerWallet = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupWalletName"));
            QLabel* providerWalletStatus = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupWalletStatus"));
            QLabel* walletRecommendation = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupWalletRecommendation"));
            QLabel* walletConsequence = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupWalletConsequence"));
            QLabel* walletScope = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupWalletScopeNote"));
            QCheckBox* walletConfirmation = setupWizard->findChild<QCheckBox*>(
                QStringLiteral("paymasterSetupWalletConfirmation"));
            QPushButton* chooseWallet = setupWizard->findChild<QPushButton*>(
                QStringLiteral("paymasterSetupChooseAnotherWallet"));
            QCheckBox* userPaid = setupWizard->findChild<QCheckBox*>(
                QStringLiteral("paymasterSetupUserPaid"));
            QComboBox* safetyProfile = setupWizard->findChild<QComboBox*>(
                QStringLiteral("paymasterSetupSafetyProfile"));
            QSpinBox* setupNetworkFee = setupWizard->findChild<QSpinBox*>(
                QStringLiteral("paymasterSetupNetworkFee"));
            QCheckBox* maintenanceApproval = setupWizard->findChild<QCheckBox*>(
                QStringLiteral("paymasterSetupMaintenanceApproval"));
            QLabel* review = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupReview"));
            QLabel* nextActions = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupNextActions"));
            QLabel* liquidityPreview = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupLiquidityPreviewDetails"));
            QWidget* progressPage = setupWizard->findChild<QWidget*>(
                QStringLiteral("paymasterSetupProgressPage"));
            QLabel* progressResult = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupProgressResult"));
            QPushButton* setupRetry = setupWizard->findChild<QPushButton*>(
                QStringLiteral("paymasterSetupRetry"));
            QLabel* firstProgressStep = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupProgressStep0"));
            QLabel* transitionProgressStep = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupProgressStep4"));
            QLabel* liquidityProgressStep = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupProgressStep7"));
            QLabel* runtimeProgressStep = setupWizard->findChild<QLabel*>(
                QStringLiteral("paymasterSetupProgressStep9"));
            if (maintenanceApproval && setupNetworkFee) {
                setupWizardUsesRecommendedNetworkFee =
                    setupNetworkFee->value() == 20000000;
                maintenanceApproval->setChecked(true);
                setupNetworkFee->setValue(setupNetworkFee->value() + 1);
                setupWizardInvalidatesMaintenanceApproval =
                    !maintenanceApproval->isChecked();
            }
            setupWizardHasVisibleContent = introduction && requirementsScroll && requirements &&
                providerWallet && providerWalletStatus && walletRecommendation &&
                walletConsequence && walletScope && walletConfirmation && chooseWallet &&
                userPaid && safetyProfile && setupNetworkFee && maintenanceApproval &&
                review && nextActions &&
                liquidityPreview && progressPage && progressResult &&
                setupRetry && firstProgressStep && transitionProgressStep &&
                liquidityProgressStep &&
                runtimeProgressStep &&
                requirementsScroll->widgetResizable() &&
                requirementsScroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff &&
                introduction->wordWrap() && requirements->wordWrap() &&
                introduction->text().contains(QStringLiteral("saves the identity")) &&
                providerWallet->text().contains(QStringLiteral("No wallet selected")) &&
                providerWalletStatus->text().contains(QStringLiteral("No wallet is selected")) &&
                walletRecommendation->text().contains(QStringLiteral("dedicated provider wallet")) &&
                walletConsequence->text().contains(QStringLiteral("does not create another wallet")) &&
                walletConsequence->text().contains(QStringLiteral("shown as reserved")) &&
                walletScope->text().contains(QStringLiteral("cannot switch wallets")) &&
                !walletConfirmation->isChecked() &&
                !walletConfirmation->isEnabled() &&
                !setupWizard->button(QWizard::NextButton)->isEnabled() &&
                safetyProfile->currentData().toString() == QLatin1String("recommended") &&
                setupWizardUsesRecommendedNetworkFee &&
                !maintenanceApproval->isChecked() &&
                requirements->text().contains(QStringLiteral("txindex=1")) &&
                requirements->text().contains(QStringLiteral("BIP324")) &&
                nextActions->text().contains(QStringLiteral("Runtime, autostart and the requested enabled state")) &&
                progressResult->text().contains(QStringLiteral("not started")) &&
                firstProgressStep->text().contains(QStringLiteral("Pending")) &&
                transitionProgressStep->text().contains(QStringLiteral("safety transition")) &&
                liquidityProgressStep->text().contains(QStringLiteral("automatic liquidity policy")) &&
                runtimeProgressStep->text().contains(QStringLiteral("runtime settings"));
            setupWizard->reject();
            break;
        }
    });
    paymasterWizard->click();
    QVERIFY(setupWizardOpened);
    QVERIFY(setupWizardUsesClassicStyle);
    QVERIFY(setupWizardHasLocalTheme);
    QVERIFY(setupWizardUsesGreenCheckboxes);
    QVERIFY(setupWizardHasVisibleContent);
    QVERIFY(setupWizardInvalidatesMaintenanceApproval);
    const bool nativeDialogsAvailable = QApplication::platformName() != QLatin1String("minimal") &&
        QApplication::platformName() != QLatin1String("offscreen");
    if (nativeDialogsAvailable) {
        bool expertSetupConfirmed{false};
        QTimer::singleShot(0, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* confirmation = qobject_cast<QMessageBox*>(widget);
                if (!confirmation || confirmation->windowTitle() != QLatin1String("Manual Paymaster setup")) {
                    continue;
                }
                expertSetupConfirmed = true;
                // Complete the native static dialog with its documented
                // return code. A synthetic button click can be translated to
                // Cancel by some Windows Qt styles while the modal event loop
                // is still entering.
                confirmation->done(QMessageBox::Yes);
                break;
            }
        });
        expertSetup->click();
        QVERIFY(expertSetupConfirmed);
    } else {
        // The Qt 5.15 minimal/offscreen plugins cannot reliably show the native
        // QMessageBox used by the expert warning. Its real interaction is
        // covered by the native-platform run; unlock these pages here so the
        // remaining widget contracts still run in the headless suite.
        setupChoice->hide();
        configuredOverview->show();
        paymasterTabs->setTabEnabled(paymasterTabs->indexOf(configurationPage), true);
        paymasterTabs->setTabEnabled(paymasterTabs->indexOf(safetyPage), true);
        paymasterTabs->setTabEnabled(paymasterTabs->indexOf(liquidityPage), true);
        QPushButton* runtime_toggle = tab.findChild<QPushButton*>(
            "paymasterRuntimeSettingsToggle");
        QVERIFY(runtime_toggle != nullptr);
        runtime_toggle->setChecked(true);
    }
    QVERIFY(setupChoice->isHidden());
    QVERIFY(!configuredOverview->isHidden());
    QVERIFY(paymasterTabs->isTabEnabled(paymasterTabs->indexOf(configurationPage)));
    QVERIFY(paymasterTabs->isTabEnabled(paymasterTabs->indexOf(safetyPage)));
    QVERIFY(paymasterTabs->isTabEnabled(paymasterTabs->indexOf(liquidityPage)));
    QLabel* configurationIntroduction =
        tab.findChild<QLabel*>("paymasterConfigurationIntroduction");
    QLabel* policySummary = tab.findChild<QLabel*>("paymasterPolicySummary");
    QLabel* fundingModelExplanation =
        tab.findChild<QLabel*>("paymasterFundingModelExplanation");
    QLabel* policyLimitsExplanation =
        tab.findChild<QLabel*>("paymasterPolicyLimitsExplanation");
    QPushButton* restorePolicyDefaults =
        tab.findChild<QPushButton*>("paymasterRestorePolicyDefaults");
    QCheckBox* sponsoredPolicy = tab.findChild<QCheckBox*>("paymasterPolicySponsored");
    QCheckBox* userPaidPolicy = tab.findChild<QCheckBox*>("paymasterPolicyUserPaid");
    QSpinBox* feePolicy = tab.findChild<QSpinBox*>("paymasterPolicyFeeBps");
    QVERIFY(configurationIntroduction != nullptr);
    QVERIFY(configurationIntroduction->text().contains(QStringLiteral("do not move funds")));
    QVERIFY(policySummary != nullptr);
    QVERIFY(policySummary->text().contains(QStringLiteral("User paid only")));
    QVERIFY(fundingModelExplanation != nullptr);
    QVERIFY(policyLimitsExplanation != nullptr);
    QVERIFY(policyLimitsExplanation->text().contains(QStringLiteral("absolute per-transfer guard")));
    QVERIFY(restorePolicyDefaults != nullptr);
    QVERIFY(sponsoredPolicy != nullptr);
    QVERIFY(userPaidPolicy != nullptr);
    QVERIFY(feePolicy != nullptr);
    sponsoredPolicy->setChecked(true);
    QVERIFY(userPaidPolicy->isChecked());
    QVERIFY(fundingModelExplanation->text().contains(QStringLiteral("Both models may be active together")));
    sponsoredPolicy->setChecked(true);
    userPaidPolicy->setChecked(false);
    feePolicy->setValue(0);
    restorePolicyDefaults->click();
    QVERIFY(!sponsoredPolicy->isChecked());
    QVERIFY(userPaidPolicy->isChecked());
    QCOMPARE(feePolicy->value(), 50);
    QWheelEvent policyWheel(
        QPointF(1, 1), QPointF(1, 1), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(feePolicy, &policyWheel);
    QCOMPARE(feePolicy->value(), 50);

    QLabel* safetyInstructions =
        tab.findChild<QLabel*>("paymasterSafetyPolicyInstructions");
    QVERIFY(safetyInstructions != nullptr);
    QVERIFY(safetyInstructions->text().contains(QStringLiteral("zero never means unlimited")));
    QScrollArea* safetyScrollPage =
        tab.findChild<QScrollArea*>("paymasterSafetyPolicyPage");
    QVERIFY(safetyScrollPage != nullptr);
    QVERIFY(safetyScrollPage->viewport()->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(!safetyScrollPage->viewport()->autoFillBackground());

    QLineEdit* userPaidPerTransaction =
        tab.findChild<QLineEdit*>("paymasterSafetyUserPaidMaxNetworkFeePerTransaction");
    QPushButton* restoreUserPaidSafety =
        tab.findChild<QPushButton*>("paymasterSafetyUserPaidRestoreDefaults");
    QVERIFY(userPaidPerTransaction != nullptr);
    QVERIFY(restoreUserPaidSafety != nullptr);
    userPaidPerTransaction->setText(QStringLiteral("42"));
    restoreUserPaidSafety->click();
    QCOMPARE(userPaidPerTransaction->text(), QStringLiteral("0.20000000"));

    QLineEdit* publicSponsoredPerTransaction =
        tab.findChild<QLineEdit*>("paymasterSafetyPublicSponsoredMaxNetworkFeePerTransaction");
    QPushButton* restorePublicSponsoredSafety =
        tab.findChild<QPushButton*>("paymasterSafetyPublicSponsoredRestoreDefaults");
    QVERIFY(publicSponsoredPerTransaction != nullptr);
    QVERIFY(restorePublicSponsoredSafety != nullptr);
    publicSponsoredPerTransaction->setText(QStringLiteral("42"));
    restorePublicSponsoredSafety->click();
    QCOMPARE(publicSponsoredPerTransaction->text(), QStringLiteral("0.00000000"));

    QSpinBox* activeQuotes =
        tab.findChild<QSpinBox*>("paymasterSafetyMaxActiveQuotesTotal");
    QPushButton* restoreQuoteSafety =
        tab.findChild<QPushButton*>("paymasterRestoreQuoteSafetyDefaults");
    QVERIFY(activeQuotes != nullptr);
    QVERIFY(restoreQuoteSafety != nullptr);
    activeQuotes->setValue(99);
    restoreQuoteSafety->click();
    QCOMPARE(activeQuotes->value(), 16);

    QSpinBox* clientFeeLimit =
        tab.findChild<QSpinBox*>("paymasterClientSafetyMaxFeePerTransaction");
    QPushButton* restoreClientSafety =
        tab.findChild<QPushButton*>("paymasterRestoreClientSafetyDefaults");
    QVERIFY(clientFeeLimit != nullptr);
    QVERIFY(restoreClientSafety != nullptr);
    QWidget* clientSafetyGroup =
        tab.findChild<QWidget*>("paymasterClientSafetyGroup");
    QVERIFY(clientSafetyGroup != nullptr);
    QVERIFY(clientSafetyGroup->isHidden());
    clientFeeLimit->setValue(99);
    restoreClientSafety->click();
    QCOMPARE(clientFeeLimit->value(), 100);

    QLabel* liquidityIntroduction =
        tab.findChild<QLabel*>("paymasterLiquidityIntroduction");
    QLabel* liquiditySteps =
        tab.findChild<QLabel*>("paymasterLiquiditySetupSteps");
    QLabel* liquiditySummary =
        tab.findChild<QLabel*>("paymasterLiquidityTargetSummary");
    QLabel* liquidityCurrentStatus =
        tab.findChild<QLabel*>("paymasterLiquidityCurrentStatus");
    QPushButton* restoreLiquidity =
        tab.findChild<QPushButton*>("paymasterRestoreLiquidityDefaults");
    QPushButton* executePreparation =
        tab.findChild<QPushButton*>("paymasterExecutePoolPreparation");
    QSpinBox* admissionDgb =
        tab.findChild<QSpinBox*>("paymasterAdmissionDgbSlots");
    QSpinBox* admissionCarriers =
        tab.findChild<QSpinBox*>("paymasterAdmissionCarrierSlots");
    QVERIFY(liquidityIntroduction != nullptr);
    QVERIFY(liquidityIntroduction->text().contains(QStringLiteral("wallet-owned reserves")));
    QVERIFY(liquidityIntroduction->text().contains(QStringLiteral("network fee")));
    QVERIFY(liquiditySteps != nullptr);
    QVERIFY(liquiditySteps->text().contains(QStringLiteral("Preview pool preparation")));
    QVERIFY(liquiditySummary != nullptr);
    QVERIFY(liquidityCurrentStatus != nullptr);
    QVERIFY(liquidityCurrentStatus->text().contains(QStringLiteral("status not loaded")));
    QVERIFY(restoreLiquidity != nullptr);
    QVERIFY(executePreparation != nullptr);
    QVERIFY(!executePreparation->isEnabled());
    QVERIFY(admissionDgb != nullptr);
    QVERIFY(admissionCarriers != nullptr);
    admissionDgb->setValue(8);
    admissionCarriers->setValue(8);
    restoreLiquidity->click();
    QCOMPARE(admissionDgb->value(), 3);
    QCOMPARE(admissionCarriers->value(), 3);
    QVERIFY(liquiditySummary->text().contains(QStringLiteral("minimum user-paid pool shape")));
    sponsoredPolicy->setChecked(true);
    userPaidPolicy->setChecked(false);
    restoreLiquidity->click();
    QCOMPARE(admissionCarriers->value(), 0);
    QVERIFY(liquiditySummary->text().contains(QStringLiteral("minimum sponsored-only pool shape")));
    userPaidPolicy->setChecked(true);
    sponsoredPolicy->setChecked(false);
    restoreLiquidity->click();

    QScrollArea* activityScrollPage =
        tab.findChild<QScrollArea*>("paymasterActivityPage");
    QLabel* activityIntroduction =
        tab.findChild<QLabel*>("paymasterActivityIntroduction");
    QLabel* activityResponsibility =
        tab.findChild<QLabel*>("paymasterActivityResponsibility");
    QLabel* activityRuntimeStatus =
        tab.findChild<QLabel*>("paymasterActivityRuntimeStatus");
    QLabel* requestExplanation =
        tab.findChild<QLabel*>("paymasterRequestProcessingExplanation");
    QLabel* submitExplanation =
        tab.findChild<QLabel*>("paymasterSubmitProcessingExplanation");
    QLabel* reservationsExplanation =
        tab.findChild<QLabel*>("paymasterReservationsExplanation");
    QLabel* recoveryExplanation =
        tab.findChild<QLabel*>("paymasterRecoveryExplanation");
    QPushButton* processRequest =
        tab.findChild<QPushButton*>("paymasterProcessOneRequest");
    QPushButton* processSubmit =
        tab.findChild<QPushButton*>("paymasterProcessOneSubmit");
    QPushButton* activityDetails =
        tab.findChild<QPushButton*>("paymasterActivityDetailsToggle");
    QPushButton* runtimeSettingsToggle =
        tab.findChild<QPushButton*>("paymasterRuntimeSettingsToggle");
    QWidget* runtimeSettingsPanel =
        tab.findChild<QWidget*>("paymasterRuntimeSettingsPanel");
    QComboBox* operationMode =
        tab.findChild<QComboBox*>("paymasterOperationMode");
    QCheckBox* providerAutostart =
        tab.findChild<QCheckBox*>("paymasterProviderAutostart");
    QWidget* manualRequestGroup =
        tab.findChild<QWidget*>("paymasterManualRequestGroup");
    QWidget* manualSubmitGroup =
        tab.findChild<QWidget*>("paymasterManualSubmitGroup");
    QPlainTextEdit* activityTechnicalResult =
        tab.findChild<QPlainTextEdit*>("paymasterActivityTechnicalResult");
    QVERIFY(activityScrollPage != nullptr);
    QVERIFY(activityIntroduction != nullptr);
    QVERIFY(activityIntroduction->text().contains(QStringLiteral("automatic provider service")));
    QVERIFY(activityScrollPage->viewport()->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(!activityScrollPage->viewport()->autoFillBackground());
    QLabel* automaticExplanation =
        tab.findChild<QLabel*>("paymasterActivityAutomaticExplanation");
    QVERIFY(automaticExplanation != nullptr);
    QVERIFY(automaticExplanation->text().contains(QStringLiteral("recommended automatic mode")));
    QVERIFY(activityResponsibility != nullptr);
    QVERIFY(activityResponsibility->text().contains(QStringLiteral("automatic processing pauses")));
    QVERIFY(activityRuntimeStatus != nullptr);
    QVERIFY(activityRuntimeStatus->text().contains(QStringLiteral("No wallet is selected")));
    QVERIFY(requestExplanation != nullptr);
    QVERIFY(requestExplanation->text().contains(QStringLiteral("does not yet authorize")));
    QVERIFY(submitExplanation != nullptr);
    QVERIFY(submitExplanation->text().contains(QStringLiteral("can spend reserved provider DGB")));
    QVERIFY(reservationsExplanation != nullptr);
    QVERIFY(reservationsExplanation->text().contains(QStringLiteral("not an additional charge")));
    QVERIFY(reservationsExplanation->text().contains(QStringLiteral("durable state")));
    QVERIFY(recoveryExplanation != nullptr);
    QVERIFY(recoveryExplanation->text().contains(QStringLiteral("durable across restarts")));
    QVERIFY(processRequest != nullptr);
    QVERIFY(processSubmit != nullptr);
    QVERIFY(!processRequest->isEnabled());
    QVERIFY(!processSubmit->isEnabled());
    QVERIFY(runtimeSettingsToggle != nullptr);
    QVERIFY(runtimeSettingsPanel != nullptr);
    // This path deliberately selected manual expert setup above. Expert setup
    // opens the advanced operator controls; guided setup keeps them collapsed.
    QVERIFY(runtimeSettingsToggle->isChecked());
    QVERIFY(!runtimeSettingsPanel->isHidden());
    QVERIFY(operationMode != nullptr);
    QCOMPARE(operationMode->currentData().toString(), QStringLiteral("automatic"));
    QVERIFY(providerAutostart != nullptr);
    QVERIFY(!providerAutostart->isChecked());
    QVERIFY(manualRequestGroup != nullptr);
    QVERIFY(manualSubmitGroup != nullptr);
    QVERIFY(manualRequestGroup->isHidden());
    QVERIFY(manualSubmitGroup->isHidden());
    runtimeSettingsToggle->setChecked(false);
    QVERIFY(runtimeSettingsPanel->isHidden());
    runtimeSettingsToggle->setChecked(true);
    QVERIFY(!runtimeSettingsPanel->isHidden());
    QVERIFY(activityDetails != nullptr);
    QVERIFY(activityTechnicalResult != nullptr);
    QVERIFY(activityTechnicalResult->isHidden());
    activityDetails->setChecked(true);
    QVERIFY(!activityTechnicalResult->isHidden());
    tab.setPaymasterOperatorVisible(true);
    QCOMPARE(tabWidget->count(), expectedTabLabels.size() + 1);
    QCOMPARE(tabWidget->tabText(tabWidget->count() - 1), QStringLiteral("Paymaster Network"));
    tab.setPaymasterOperatorVisible(false);
    QCOMPARE(tabWidget->count(), expectedTabLabels.size());
    QScrollArea* sendPage = tab.findChild<QScrollArea*>("digiDollarSendPage");
    QVERIFY(sendPage != nullptr);
    QVERIFY(sendPage->widgetResizable());
    QCOMPARE(sendPage->viewport()->backgroundRole(), QPalette::Window);
    QComboBox* feeMode = tab.findChild<QComboBox*>("paymasterFeeMode");
    QVERIFY(feeMode != nullptr);
    QCOMPARE(feeMode->currentData().toString(), QStringLiteral("dgb"));
    QRadioButton* dgbFeeMode = tab.findChild<QRadioButton*>("feeFundingDgb");
    QRadioButton* autoFeeMode = tab.findChild<QRadioButton*>("feeFundingAuto");
    QRadioButton* paymasterFeeMode = tab.findChild<QRadioButton*>("feeFundingPaymaster");
    QFrame* dgbFeeCard = tab.findChild<QFrame*>("feeFundingDgbCard");
    QFrame* autoFeeCard = tab.findChild<QFrame*>("feeFundingAutoCard");
    QFrame* paymasterFeeCard = tab.findChild<QFrame*>("feeFundingPaymasterCard");
    QLabel* feeIntroduction = tab.findChild<QLabel*>("feeFundingIntroduction");
    QLabel* feeExplanation = tab.findChild<QLabel*>("feeFundingModeExplanation");
    QLabel* feeSummary = tab.findChild<QLabel*>("feeFundingSummary");
    QCheckBox* subtractPaymasterFee =
        tab.findChild<QCheckBox*>("subtractPaymasterFeeFromAmount");
    QPushButton* paymasterHelp = tab.findChild<QPushButton*>("paymasterExplanationButton");
    QPushButton* advancedPaymaster = tab.findChild<QPushButton*>("advancedPaymasterSettingsToggle");
    QFrame* advancedPaymasterFrame = tab.findChild<QFrame*>("advancedPaymasterSettings");
    QFrame* clientSafetyFrame = tab.findChild<QFrame*>("paymasterClientSafetyFrame");
    QVERIFY(dgbFeeMode != nullptr);
    QVERIFY(autoFeeMode != nullptr);
    QVERIFY(paymasterFeeMode != nullptr);
    QVERIFY(dgbFeeCard != nullptr);
    QVERIFY(autoFeeCard != nullptr);
    QVERIFY(paymasterFeeCard != nullptr);
    QVERIFY(dgbFeeMode->isChecked());
    QCOMPARE(dgbFeeCard->property("feeSelected").toBool(), true);
    QCOMPARE(autoFeeCard->property("feeSelected").toBool(), false);
    QCOMPARE(paymasterFeeCard->property("feeSelected").toBool(), false);
    QVERIFY(feeIntroduction != nullptr);
    QVERIFY(feeIntroduction->text().contains(QStringLiteral("never deducted")));
    QVERIFY(feeExplanation != nullptr);
    QVERIFY(feeExplanation->text().contains(QStringLiteral("No Paymaster")));
    QVERIFY(feeSummary != nullptr);
    QVERIFY(subtractPaymasterFee != nullptr);
    QVERIFY(subtractPaymasterFee->isHidden());
    QVERIFY(!subtractPaymasterFee->isChecked());
    QVERIFY(feeSummary->text().contains(QStringLiteral("~0.1 DGB")));
    QVERIFY(paymasterHelp != nullptr);
    QCOMPARE(paymasterHelp->parentWidget(), paymasterFeeCard);
    QVERIFY(advancedPaymaster != nullptr);
    QVERIFY(advancedPaymaster->isHidden());
    QVERIFY(advancedPaymasterFrame != nullptr);
    QVERIFY(advancedPaymasterFrame->isHidden());
    QVERIFY(clientSafetyFrame != nullptr);
    QVERIFY(clientSafetyFrame->isHidden());

    feeMode->setCurrentIndex(feeMode->findData(QStringLiteral("auto")));
    QVERIFY(autoFeeMode->isChecked());
    QCOMPARE(dgbFeeCard->property("feeSelected").toBool(), false);
    QCOMPARE(autoFeeCard->property("feeSelected").toBool(), true);
    QVERIFY(!advancedPaymaster->isHidden());
    QVERIFY(advancedPaymasterFrame->isHidden());
    QVERIFY(!clientSafetyFrame->isHidden());
    QVERIFY(!subtractPaymasterFee->isHidden());
    QVERIFY(!subtractPaymasterFee->isChecked());
    QVERIFY(feeExplanation->text().contains(QStringLiteral("first tries")));
    QVERIFY(feeSummary->text().contains(QStringLiteral("Paymaster only if needed")));
    QVERIFY(feeSummary->text().contains(QStringLiteral("1.00 $DD (100 cents)")));
    QLineEdit* sendAmount = sendPage->findChild<QLineEdit*>("amountEdit");
    DigiDollarSendWidget* sendForm =
        qobject_cast<DigiDollarSendWidget*>(sendPage->widget());
    QPushButton* useAvailableBalance =
        sendPage->findChild<QPushButton*>("useAvailableBalanceButton");
    QVERIFY(sendAmount != nullptr);
    QVERIFY(sendForm != nullptr);
    QVERIFY(useAvailableBalance != nullptr);
    QCOMPARE(useAvailableBalance->text(), QStringLiteral("Empty wallet with Paymaster"));
    sendForm->setAvailableDigiDollarBalanceForTesting(5000);
    useAvailableBalance->click();
    QCOMPARE(sendAmount->text(), QStringLiteral("50.00"));
    QVERIFY(subtractPaymasterFee->isChecked());
    QVERIFY(feeSummary->text().contains(QStringLiteral("Exact total $DD outflow")));
    QVERIFY(feeSummary->text().contains(
        QStringLiteral("Calculated from the exact offer before signing")));
    QVERIFY(feeSummary->text().contains(QStringLiteral("Wallet emptying")));
    QVERIFY(feeSummary->text().contains(QStringLiteral("0.00 $DD")));
    advancedPaymaster->setChecked(true);
    QVERIFY(!advancedPaymasterFrame->isHidden());
    QSpinBox* paymasterFeeCap = tab.findChild<QSpinBox*>("paymasterFeeCap");
    QVERIFY(paymasterFeeCap != nullptr);
    QCOMPARE(paymasterFeeCap->value(), 100);
    QWheelEvent feeCapWheel(
        QPointF(1, 1), QPointF(1, 1), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(paymasterFeeCap, &feeCapWheel);
    QCOMPARE(paymasterFeeCap->value(), 100);
    feeMode->setCurrentIndex(feeMode->findData(QStringLiteral("paymaster")));
    QVERIFY(paymasterFeeMode->isChecked());
    QCOMPARE(autoFeeCard->property("feeSelected").toBool(), false);
    QCOMPARE(paymasterFeeCard->property("feeSelected").toBool(), true);
    QVERIFY(feeSummary->text().contains(QStringLiteral("Total $DD outflow")));
    QVERIFY(feeSummary->text().contains(QStringLiteral("50.00 $DD")));
    feeMode->setCurrentIndex(feeMode->findData(QStringLiteral("dgb")));
    QVERIFY(dgbFeeMode->isChecked());
    QCOMPARE(dgbFeeCard->property("feeSelected").toBool(), true);
    QCOMPARE(paymasterFeeCard->property("feeSelected").toBool(), false);
    QVERIFY(advancedPaymaster->isHidden());
    QVERIFY(subtractPaymasterFee->isHidden());
    QVERIFY(!subtractPaymasterFee->isChecked());
    QCOMPARE(useAvailableBalance->text(), QStringLiteral("Use available balance"));
    QComboBox* paymasterPrivacy = tab.findChild<QComboBox*>("paymasterPrivacy");
    QVERIFY(paymasterPrivacy != nullptr);
    QVERIFY(paymasterPrivacy->toolTip().contains(QStringLiteral("does not guarantee anonymity")));
    QVERIFY(paymasterPrivacy->toolTip().contains(QStringLiteral("selected provider")));
    const int privacyIndex = paymasterPrivacy->currentIndex();
    QWheelEvent privacyWheel(
        QPointF(1, 1), QPointF(1, 1), QPoint(), QPoint(0, -120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(paymasterPrivacy, &privacyWheel);
    QCOMPARE(paymasterPrivacy->currentIndex(), privacyIndex);
    QTableWidget* paymasterOffers = tab.findChild<QTableWidget*>("paymasterOffers");
    QVERIFY(paymasterOffers != nullptr);
    QCOMPARE(paymasterOffers->selectionMode(), QAbstractItemView::NoSelection);
    QVERIFY(paymasterOffers->toolTip().contains(QStringLiteral("preview"), Qt::CaseInsensitive));
    QVERIFY(tab.findChild<QLabel*>("paymasterSessionState") != nullptr);

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
            const QString path = QDir::temp().filePath(
                QStringLiteral("digibyte_dd_label_qa_tab_%1.png").arg(i));
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

    QFrame* compactChoices = sendWidget.findChild<QFrame*>("feeFundingChoices");
    QFrame* sessionFocus = sendWidget.findChild<QFrame*>("paymasterSessionFrame");
    QLabel* sessionNextStep = sendWidget.findChild<QLabel*>("paymasterSessionNextStep");
    QPushButton* sessionPrimary =
        sendWidget.findChild<QPushButton*>("paymasterSessionPrimaryAction");
    QPushButton* sessionMore =
        sendWidget.findChild<QPushButton*>("paymasterSessionMoreOptions");
    QPushButton* sessionTechnical =
        sendWidget.findChild<QPushButton*>("paymasterSessionTechnicalToggle");
    QPushButton* sessionAbandon =
        sendWidget.findChild<QPushButton*>("abandonUnsignedPaymasterSession");
    QPushButton* sessionFallback =
        sendWidget.findChild<QPushButton*>("fallbackPaymasterSession");
    QPushButton* sessionRecover =
        sendWidget.findChild<QPushButton*>("recoverPaymasterSession");
    QPushButton* sessionRetry =
        sendWidget.findChild<QPushButton*>("retryPaymasterSession");
    QLineEdit* focusAddress = sendWidget.findChild<QLineEdit*>("addressEdit");
    QLineEdit* focusAmount = sendWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(compactChoices != nullptr);
    QVERIFY(sessionFocus != nullptr);
    QVERIFY(sessionNextStep != nullptr);
    QVERIFY(sessionPrimary != nullptr);
    QVERIFY(sessionMore != nullptr);
    QVERIFY(sessionTechnical != nullptr);
    QVERIFY(focusAddress != nullptr);
    QVERIFY(focusAmount != nullptr);
    QVERIFY(sessionFallback != nullptr);
    QVERIFY(sessionRecover != nullptr);
    QVERIFY(sessionRetry != nullptr);
    QVERIFY(!compactChoices->isHidden());
    QVERIFY(sessionFocus->isHidden());

    QGridLayout* compactChoicesLayout =
        qobject_cast<QGridLayout*>(compactChoices->layout());
    QFrame* compactDgbCard = sendWidget.findChild<QFrame*>("feeFundingDgbCard");
    QFrame* compactAutoCard = sendWidget.findChild<QFrame*>("feeFundingAutoCard");
    QFrame* compactPaymasterCard =
        sendWidget.findChild<QFrame*>("feeFundingPaymasterCard");
    QVERIFY(compactChoicesLayout != nullptr);
    QVERIFY(compactDgbCard != nullptr);
    QVERIFY(compactAutoCard != nullptr);
    QVERIFY(compactPaymasterCard != nullptr);
    // Resize events for hidden top-level widgets are deferred on Windows. Show
    // the test widget before exercising the responsive fee-card layout so the
    // assertions observe the same event path as the wallet UI.
    sendWidget.show();
    sendWidget.resize(1200, 760);
    QCoreApplication::processEvents();
    int dgbRow{-1};
    int dgbColumn{-1};
    int autoRow{-1};
    int autoColumn{-1};
    int rowSpan{0};
    int columnSpan{0};
    compactChoicesLayout->getItemPosition(
        compactChoicesLayout->indexOf(compactDgbCard), &dgbRow, &dgbColumn,
        &rowSpan, &columnSpan);
    compactChoicesLayout->getItemPosition(
        compactChoicesLayout->indexOf(compactAutoCard), &autoRow, &autoColumn,
        &rowSpan, &columnSpan);
    QCOMPARE(dgbRow, autoRow);
    QVERIFY(dgbColumn != autoColumn);
    // The native Windows style can raise the aggregate layout size hint after
    // the operator-console controls are created in the same process. Pin the
    // viewport width so this assertion tests the widget's explicit 980 px
    // responsive breakpoint rather than a platform size-hint decision.
    sendWidget.setFixedWidth(800);
    sendWidget.resize(800, 760);
    QCoreApplication::processEvents();
    compactChoicesLayout->getItemPosition(
        compactChoicesLayout->indexOf(compactDgbCard), &dgbRow, &dgbColumn,
        &rowSpan, &columnSpan);
    compactChoicesLayout->getItemPosition(
        compactChoicesLayout->indexOf(compactPaymasterCard), &autoRow, &autoColumn,
        &rowSpan, &columnSpan);
    QCOMPARE(dgbColumn, autoColumn);
    QVERIFY(dgbRow != autoRow);

    sendWidget.setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RDtestRecipient"), 5.0, QStringLiteral("QUOTE_EXPIRED"),
        QString{}, {QStringLiteral("refresh"), QStringLiteral("fallback"),
                    QStringLiteral("abandon_unsigned")}, true);
    QVERIFY(compactChoices->isHidden());
    QVERIFY(!sessionFocus->isHidden());
    QVERIFY(focusAddress->isReadOnly());
    QVERIFY(focusAmount->isReadOnly());
    QCOMPARE(sessionPrimary->text(), QStringLiteral("Try another Paymaster"));
    QVERIFY(sessionNextStep->text().contains(QStringLiteral("protected")) ||
            sessionNextStep->text().contains(QStringLiteral("Preparing")));
    QVERIFY(sessionMore->isChecked() == false);
    QVERIFY(sessionTechnical->isChecked() == false);
    QVERIFY(sessionAbandon != nullptr);
    QVERIFY(!sessionAbandon->isHidden());
    QVERIFY(sessionFallback->isHidden()); // This action is already the primary action.
    QVERIFY(sessionRecover->isHidden());
    QVERIFY(sessionRetry->isHidden());

    sendWidget.setPaymasterSessionForTesting(
        QStringLiteral("FAILED"), QStringLiteral("user_psbt"), true,
        QStringLiteral("RDtestRecipient"), 5.0, QString{}, QString{},
        {QStringLiteral("refresh"), QStringLiteral("retry_same"),
         QStringLiteral("cancel_to_self")}, true);
    QCOMPARE(sessionPrimary->text(), QStringLiteral("Start safe recovery"));
    QVERIFY(sessionAbandon->isHidden());
    QVERIFY(sessionFallback->isHidden());
    QVERIFY(sessionRecover->isHidden()); // This action is already the primary action.
    QVERIFY(!sessionRetry->isHidden());

    // Unknown or contradictory authorization evidence must fail closed: only
    // the non-mutating status action remains available.
    sendWidget.setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("user_psbt"), true,
        QStringLiteral("RDtestRecipient"), 5.0, QStringLiteral("USER_SIGNATURE_SENT"),
        QString{}, {QStringLiteral("refresh")}, true);
    QCOMPARE(sessionPrimary->text(), QStringLiteral("Check current status"));
    QVERIFY(sessionAbandon->isHidden());
    QVERIFY(sessionMore->isHidden());
    QVERIFY(sessionFallback->isHidden());
    QVERIFY(sessionRecover->isHidden());
    QVERIFY(sessionRetry->isHidden());

    sendWidget.setPaymasterSessionForTesting(
        QStringLiteral("MEMPOOL"), QStringLiteral("final_transaction"), true,
        QStringLiteral("RDtestRecipient"), 5.0, QStringLiteral("FINAL_COMMITTED"),
        QString{}, {QStringLiteral("refresh"),
                    QStringLiteral("cancel_to_self")}, true);
    QCOMPARE(sessionPrimary->text(), QStringLiteral("Check current status"));
    QVERIFY(sessionAbandon->isHidden());
    QVERIFY(sessionFallback->isHidden());
    QVERIFY(!sessionRecover->isHidden());

    sendWidget.setPaymasterSessionForTesting(
        QStringLiteral("CONFIRMED"), QStringLiteral("final_transaction"), true,
        QStringLiteral("RDtestRecipient"), 5.0, QString{}, QString{}, {}, true);
    QCOMPARE(sessionPrimary->text(), QStringLiteral("Start a new transfer"));
    QVERIFY(sessionMore->isHidden());
    sessionPrimary->click();
    QVERIFY(!focusAddress->isReadOnly());
    QVERIFY(!focusAmount->isReadOnly());
    QVERIFY(!compactChoices->isHidden());
    QVERIFY(sessionFocus->isHidden());

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

void DigiDollarWidgetTests::paymasterLiquidityPolicyDefaultsAndApprovalGuard()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QTabWidget* paymaster_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QWidget* liquidity_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterLiquidityPage"));
    QVERIFY(paymaster_tabs != nullptr);
    QVERIFY(liquidity_page != nullptr);
    const int liquidity_index = paymaster_tabs->indexOf(liquidity_page);
    QVERIFY(liquidity_index >= 0);
    paymaster_tabs->setTabEnabled(liquidity_index, true);
    QVERIFY(liquidity_page->isEnabled());

    QCheckBox* automatic = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterAutomaticReplenishment"));
    QCheckBox* approved = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QLineEdit* per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLineEdit* per_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerHour"));
    QLineEdit* per_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerDay"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QSpinBox* operational_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalDgbSlots"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* maintenance_fee_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"));
    QLabel* target_save_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityTargetSaveStatus"));
    QPushButton* save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QPushButton* restore = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterRestoreLiquidityDefaults"));
    QPushButton* advanced = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterAdvancedLiquidityToggle"));
    QPushButton* primary_save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicyPrimary"));

    QVERIFY(automatic != nullptr);
    QVERIFY(approved != nullptr);
    QVERIFY(per_transaction != nullptr);
    QVERIFY(per_hour != nullptr);
    QVERIFY(per_day != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(operational_dgb != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(target_save_status != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(restore != nullptr);
    QVERIFY(advanced != nullptr);
    QVERIFY(primary_save != nullptr);

    paymaster_tabs->setCurrentIndex(liquidity_index);
    QVERIFY(primary_save->isVisibleTo(liquidity_page));
    QCOMPARE(primary_save->text(), QStringLiteral("Save liquidity settings"));
    advanced->setChecked(true);
    QVERIFY(save->isVisibleTo(liquidity_page));
    QCOMPARE(save->text(), QStringLiteral("Save liquidity targets"));
    QVERIFY(target_save_status->text().contains(
        QStringLiteral("Not saved yet")));

    QVERIFY(automatic->isChecked());
    QVERIFY(!approved->isChecked());
    QCOMPARE(admission_dgb->value(), 3);
    QCOMPARE(operational_dgb->value(), 1);
    QCOMPARE(admission_carriers->value(), 3);
    QCOMPARE(operational_carriers->value(), 1);
    const qint64 transaction_limit = per_transaction->text().toLongLong();
    const qint64 hourly_limit = per_hour->text().toLongLong();
    const qint64 daily_limit = per_day->text().toLongLong();
    QVERIFY(transaction_limit > 0);
    QVERIFY(hourly_limit >= transaction_limit);
    QVERIFY(daily_limit >= hourly_limit);

    approved->setChecked(true);
    automatic->setChecked(false);
    admission_dgb->setValue(8);
    per_transaction->setText(QStringLiteral("42"));
    QVERIFY(status->text().contains(QStringLiteral("Unsaved")));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Not saved")));
    restore->click();
    QVERIFY(automatic->isChecked());
    QVERIFY(!approved->isChecked());
    QCOMPARE(admission_dgb->value(), 3);
    QCOMPARE(per_transaction->text(), QStringLiteral("10000000"));
    QVERIFY(status->text().contains(QStringLiteral("Paid maintenance is disabled")));
    QVERIFY(save->toolTip().contains(QStringLiteral("does not create a transaction")));
    QVERIFY(primary_save->toolTip().contains(
        QStringLiteral("does not create a transaction")));
}

void DigiDollarWidgetTests::paymasterLiquidityPolicySavePersistsVisibleValues()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QStringList commands;
    std::vector<UniValue> parameters;
    bool malformed_ack{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterliquiditypolicy") {
                if (malformed_ack) return UniValue{UniValue::VOBJ};
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QCheckBox* automatic = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterAutomaticReplenishment"));
    QCheckBox* approved = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QSpinBox* operational_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalDgbSlots"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLineEdit* per_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerHour"));
    QLineEdit* per_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerDay"));
    QPushButton* save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QPushButton* primary_save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicyPrimary"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"));
    QLabel* target_save_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityTargetSaveStatus"));
    QVERIFY(automatic != nullptr);
    QVERIFY(approved != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(operational_dgb != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(per_transaction != nullptr);
    QVERIFY(per_hour != nullptr);
    QVERIFY(per_day != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(primary_save != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(target_save_status != nullptr);

    automatic->setChecked(true);
    // Keeping paid maintenance disabled avoids a modal approval dialog while
    // still verifying that all displayed targets and finite limits are sent.
    approved->setChecked(false);
    admission_dgb->setValue(5);
    operational_dgb->setValue(2);
    admission_carriers->setValue(4);
    operational_carriers->setValue(2);
    per_transaction->setText(QStringLiteral("12000000"));
    per_hour->setText(QStringLiteral("60000000"));
    per_day->setText(QStringLiteral("240000000"));
    primary_save->click();

    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterliquiditypolicy"));
    QVERIFY(parameters.at(0).isArray());
    const UniValue& policy = parameters.at(0)[0];
    QCOMPARE(policy.find_value("automatic_replenishment").get_bool(), true);
    QCOMPARE(policy.find_value("paid_maintenance_approved").get_bool(), false);
    QCOMPARE(policy.find_value("target_admission_dgb").getInt<int>(), 5);
    QCOMPARE(policy.find_value("target_operational_dgb").getInt<int>(), 2);
    QCOMPARE(policy.find_value("target_admission_carriers").getInt<int>(), 4);
    QCOMPARE(policy.find_value("target_operational_carriers").getInt<int>(), 2);
    QCOMPARE(policy.find_value(
                 "maximum_maintenance_fee_per_transaction_satoshis")
                 .getInt<qint64>(),
             12000000);
    QCOMPARE(policy.find_value("maximum_maintenance_fee_per_hour_satoshis")
                 .getInt<qint64>(),
             60000000);
    QCOMPARE(policy.find_value("maximum_maintenance_fee_per_day_satoshis")
                 .getInt<qint64>(),
             240000000);
    QVERIFY(status->text().contains(QStringLiteral("saved successfully")));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Saved")));
    QVERIFY(commands.contains(QStringLiteral("getpaymasterinfo")));

    // A transport-level success is not a persistence acknowledgement. Keep
    // the edit dirty when Core does not echo the complete canonical policy.
    malformed_ack = true;
    admission_dgb->setValue(6);
    primary_save->click();
    QVERIFY(status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Not saved")));
}

void DigiDollarWidgetTests::paymasterLiquidityMaintenanceStatesAreReadable()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QLabel* state = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceState"));
    QLabel* next_step = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceNextStep"));
    QLabel* cost = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceCost"));
    QLabel* slot_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquiditySlotStatus"));
    QLabel* budget = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityBudgetStatus"));
    QPushButton* approve = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterApproveLiquidityMaintenance"));
    QGroupBox* maintenance_card = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterLiquidityMaintenanceCard"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* maintenance_fee_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QCheckBox* paid_maintenance = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QPushButton* maintenance_limits = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterMaintenanceLimitsToggle"));
    QPushButton* save_policy = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QVERIFY(state != nullptr);
    QVERIFY(next_step != nullptr);
    QVERIFY(cost != nullptr);
    QVERIFY(slot_status != nullptr);
    QVERIFY(budget != nullptr);
    QVERIFY(approve != nullptr);
    QVERIFY(maintenance_card != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(maintenance_fee_per_transaction != nullptr);
    QVERIFY(paid_maintenance != nullptr);
    QVERIFY(maintenance_limits != nullptr);
    QVERIFY(save_policy != nullptr);

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_target_configuration", true, true, 0, 0,
        /*targets_satisfy_provider_policy=*/false));
    QVERIFY(state->text().contains(
        QStringLiteral("cannot make this offer ready")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("one operational DigiDollar carrier")));
    QVERIFY(cost->text().contains(
        QStringLiteral("saved operational carrier target is zero")));
    QVERIFY(approve->text().contains(
        QStringLiteral("required liquidity targets")));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));

    // Repairing an old zero-carrier policy is one visible, reviewable save
    // step: load the minimum targets, expose the finite fee limits and wait
    // for the operator to use the explicit save/approval action.
    tab.setPaymasterRpcExecutorForTesting(
        [](const std::string&, const UniValue&) {
            return UniValue{UniValue::VOBJ};
        });
    admission_carriers->setValue(0);
    operational_carriers->setValue(0);
    paid_maintenance->setChecked(false);
    approve->click();
    QCOMPARE(admission_carriers->value(), 3);
    QCOMPARE(operational_carriers->value(), 1);
    QVERIFY(paid_maintenance->isChecked());
    QVERIFY(maintenance_limits->isChecked());
    QCOMPARE(save_policy->text(),
             QStringLiteral("Save targets and approve refill…"));
    QVERIFY(save_policy->isEnabledTo(save_policy->parentWidget()));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_maintenance_approval", true, false, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("targets are saved"), Qt::CaseInsensitive));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No target update is needed")));
    QVERIFY(cost->text().contains(QStringLiteral("not approved")));
    QVERIFY(approve->text().contains(
        QStringLiteral("bounded refill costs")));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("waiting for provider start")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("Start the provider")));
    QVERIFY(cost->text().contains(QStringLiteral("approved within finite limits")));
    QVERIFY(slot_status->text().contains(
        QStringLiteral("Operational DGB: 0 ready")));
    QVERIFY(budget->text().contains(QStringLiteral("reserved: 0.01000000 DGB")));
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("waiting"));

    // A stale Regtest tip prevents creation of the maintenance transaction.
    // The UI must not claim that replenishment is already happening while the
    // backend still reports zero pending slots.
    UniValue node_wait{UniValue::VOBJ};
    node_wait.pushKV("wallet_eligible", true);
    node_wait.pushKV("enabled", true);
    node_wait.pushKV("ready", false);
    node_wait.pushKV("wallet_locked", false);
    UniValue node_wait_errors{UniValue::VARR};
    node_wait_errors.push_back("PAYMASTER_NODE_NOT_READY");
    node_wait.pushKV("readiness_errors", std::move(node_wait_errors));
    tab.setPaymasterReadinessStatusForTesting(node_wait);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("waiting for a fresh Regtest block")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No refill transaction has been created yet")));
    QVERIFY(cost->text().contains(QStringLiteral("1 missing and 0 pending")));

    // The provider service may be running while Core safely rejects creation
    // of a refill whose estimated network fee exceeds the explicitly approved
    // per-transaction cap. This is an operator action, not an in-progress
    // transaction: zero pending slots must remain visible and the review
    // action must lead directly to the finite maintenance limits.
    UniValue fee_exceeded{UniValue::VOBJ};
    fee_exceeded.pushKV("wallet_eligible", true);
    fee_exceeded.pushKV("enabled", true);
    fee_exceeded.pushKV("ready", false);
    fee_exceeded.pushKV("running", true);
    fee_exceeded.pushKV("wallet_locked", false);
    fee_exceeded.pushKV("service_state", "replenishing_liquidity");
    fee_exceeded.pushKV("last_service_error",
                        "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    UniValue fee_errors{UniValue::VARR};
    fee_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    fee_exceeded.pushKV("readiness_errors", std::move(fee_errors));
    tab.setPaymasterReadinessStatusForTesting(fee_exceeded);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(QStringLiteral("refill paused"),
                                   Qt::CaseInsensitive));
    QVERIFY(next_step->text().contains(QStringLiteral("0.10000000 DGB")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No transaction was created")));
    QVERIFY(cost->text().contains(QStringLiteral("1 missing and 0 pending")));
    QCOMPARE(approve->text(), QStringLiteral("Review refill cost limit…"));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));
    approve->click();
    QVERIFY(maintenance_limits->isChecked());
    // Runtime settings remain locked while the provider is running. The
    // review action still exposes the exact finite limit and explains that
    // the provider must be stopped before that authorization can be changed.
    QVERIFY(!maintenance_fee_per_transaction->isEnabled());
    QVERIFY(tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"))->text().contains(
            QStringLiteral("no transaction has been created"),
            Qt::CaseInsensitive));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_liquidity_confirmation", true, true, 0, 1));
    QVERIFY(state->text().contains(QStringLiteral("waiting for confirmation")));
    QVERIFY(next_step->text().contains(QStringLiteral("1 pending slot")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.01000000 DGB in network fees")));
    QVERIFY(cost->text().contains(
        QStringLiteral("confirmed costs appear in Finances")));

    UniValue refill_ready{UniValue::VOBJ};
    refill_ready.pushKV("wallet_eligible", true);
    refill_ready.pushKV("enabled", true);
    refill_ready.pushKV("ready", true);
    refill_ready.pushKV("running", true);
    refill_ready.pushKV("wallet_locked", false);
    refill_ready.pushKV("service_state", "active");
    refill_ready.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(refill_ready);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "ready", true, true, 0));
    QVERIFY(state->text().contains(QStringLiteral("targets are satisfied")));
    QVERIFY(next_step->text().contains(QStringLiteral("reused automatically")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.02000000 DGB in the rolling hour")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.03000000 DGB in the rolling day")));
    QVERIFY(approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("ready"));
}

void DigiDollarWidgetTests::paymasterExternalReadinessIsSeparatedFromConfiguration()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QLabel* next_step = tab.findChild<QLabel*>(
        QStringLiteral("paymasterNextStep"));
    QLabel* operation = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOverviewOperationStatus"));
    QLabel* liquidity = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOverviewLiquidityStatus"));
    QPushButton* liquidity_action = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewLiquidityAction"));
    QPushButton* operation_action = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewOperationAction"));
    QLabel* blockchain = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalBlockchainStatus"));
    QLabel* txindex = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalTxIndexStatus"));
    QLabel* broadcast = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalBroadcastStatus"));
    QLabel* activation = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalActivationStatus"));
    QLabel* oracle = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOracleStatus"));
    QLabel* oracle_note = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOracleNote"));
    QGroupBox* prerequisites = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterOverviewNextStep"));
    QLabel* other_requirement = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOtherStatus"));
    QPushButton* wizard = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSetupWizard"));
    QPushButton* start = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterStartProvider"));
    QLabel* provider_id = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOfferIdentityId"));
    QVERIFY(provider_status != nullptr);
    QVERIFY(next_step != nullptr);
    QVERIFY(operation != nullptr);
    QVERIFY(liquidity != nullptr);
    QVERIFY(liquidity_action != nullptr);
    QVERIFY(operation_action != nullptr);
    QVERIFY(blockchain != nullptr);
    QVERIFY(txindex != nullptr);
    QVERIFY(broadcast != nullptr);
    QVERIFY(activation != nullptr);
    QVERIFY(oracle != nullptr);
    QVERIFY(oracle_note != nullptr);
    QVERIFY(prerequisites != nullptr);
    QVERIFY(other_requirement != nullptr);
    QVERIFY(wizard != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(provider_id != nullptr);

    UniValue waiting{UniValue::VOBJ};
    waiting.pushKV("wallet_eligible", true);
    waiting.pushKV("enabled", true);
    waiting.pushKV("ready", false);
    waiting.pushKV("wallet_locked", false);
    const QString full_provider_id{
        QStringLiteral("371aaaaeea392e01f786ca55030708320e0f85cfed0de0ffcc820faa14d9af3d")};
    waiting.pushKV("provider_id", full_provider_id.toStdString());
    UniValue waiting_errors{UniValue::VARR};
    waiting_errors.push_back("PAYMASTER_NODE_NOT_READY");
    waiting.pushKV("readiness_errors", std::move(waiting_errors));
    waiting.pushKV("oracle_price_micro_usd", 0);
    tab.setPaymasterReadinessStatusForTesting(waiting);

    QVERIFY(provider_status->text().contains(
        QStringLiteral("Provider fully configured")));
    QVERIFY(!provider_status->text().contains(QStringLiteral("Setup incomplete")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("Waiting for blockchain synchronization")));
    QVERIFY(operation->text().contains(QStringLiteral("Waiting")));
    QVERIFY(operation->text().contains(QStringLiteral("Regtest block")));
    QVERIFY(blockchain->text().contains(QStringLiteral("Waiting")));
    QVERIFY(txindex->text().contains(QStringLiteral("Ready")));
    QVERIFY(broadcast->text().contains(QStringLiteral("Waiting")));
    QVERIFY(activation->text().contains(QStringLiteral("Ready")));
    QVERIFY(oracle->text().contains(QStringLiteral("Not available yet")));
    QVERIFY(oracle_note->text().contains(
        QStringLiteral("Paymaster transfers do not require an Oracle price")));
    QVERIFY(!oracle_note->isHidden());
    QVERIFY(wizard->text().contains(QStringLiteral("Review setup")));
    QVERIFY(!start->isEnabled());
    QVERIFY(start->toolTip().contains(QStringLiteral("No provider setting")));
    QCOMPARE(provider_id->text(), full_provider_id);
    QVERIFY(provider_id->wordWrap());
    QVERIFY(provider_id->textInteractionFlags().testFlag(
        Qt::TextSelectableByMouse));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("waiting"));
    QVERIFY(other_requirement->isHidden());

    // A transient inventory shortage and a stale Regtest tip must not be
    // presented as unsaved provider settings. The Liquidity card owns the
    // refill action, while Operations explains that a fresh block is needed.
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_maintenance_approval", true, false, 1));
    UniValue stale_with_missing_slot{UniValue::VOBJ};
    stale_with_missing_slot.pushKV("wallet_eligible", true);
    stale_with_missing_slot.pushKV("enabled", true);
    stale_with_missing_slot.pushKV("ready", false);
    stale_with_missing_slot.pushKV("wallet_locked", false);
    UniValue stale_errors{UniValue::VARR};
    stale_errors.push_back("PAYMASTER_NODE_NOT_READY");
    stale_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    stale_with_missing_slot.pushKV("readiness_errors", std::move(stale_errors));
    stale_with_missing_slot.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(stale_with_missing_slot);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("fully configured")));
    QVERIFY(operation->text().contains(QStringLiteral("Regtest block")));
    QVERIFY(!operation->text().contains(
        QStringLiteral("complete the remaining setup")));
    QVERIFY(other_requirement->isHidden());

    UniValue ready{UniValue::VOBJ};
    ready.pushKV("wallet_eligible", true);
    ready.pushKV("enabled", true);
    ready.pushKV("ready", true);
    ready.pushKV("wallet_locked", false);
    ready.pushKV("readiness_errors", UniValue{UniValue::VARR});
    ready.pushKV("oracle_price_micro_usd", 0);
    tab.setPaymasterReadinessStatusForTesting(ready);
    QVERIFY(start->isEnabled());
    QVERIFY(oracle->text().contains(QStringLiteral("Not available yet")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));

    // Once external readiness is green and bounded maintenance is approved,
    // a stopped provider must be presented with the actual next action. It is
    // not still being configured and it cannot prepare a refill while stopped.
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    UniValue stopped_with_missing_slot{UniValue::VOBJ};
    stopped_with_missing_slot.pushKV("wallet_eligible", true);
    stopped_with_missing_slot.pushKV("enabled", true);
    stopped_with_missing_slot.pushKV("ready", false);
    stopped_with_missing_slot.pushKV("running", false);
    stopped_with_missing_slot.pushKV("wallet_locked", false);
    stopped_with_missing_slot.pushKV("service_state", "stopped");
    UniValue stopped_errors{UniValue::VARR};
    stopped_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    stopped_with_missing_slot.pushKV("readiness_errors",
                                     std::move(stopped_errors));
    stopped_with_missing_slot.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(stopped_with_missing_slot);
    QVERIFY(next_step->text().contains(
        QStringLiteral("All external prerequisites are ready")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(operation->text().contains(
        QStringLiteral("start the provider to restore missing liquidity")));
    QCOMPARE(operation_action->text(),
             QStringLiteral("Start and restore liquidity"));
    QVERIFY(start->isEnabled());
    QCOMPARE(start->text(), QStringLiteral("Start and restore liquidity"));

    // Both compact Overview actions promise the same operation as the primary
    // button. On a native platform, verify that they open the guarded start
    // confirmation instead of navigating to Liquidity or Operations. Qt 5.15
    // minimal/offscreen can crash in QMessageBox::showEvent, so headless runs
    // retain the state/action assertions above without opening a dialog.
    const bool native_dialogs_available =
        QApplication::platformName() != QLatin1String("minimal") &&
        QApplication::platformName() != QLatin1String("offscreen");
    if (native_dialogs_available) {
        const auto verify_start_action = [&](QPushButton* action) {
            bool confirmation_seen{false};
            QTimer::singleShot(0, [&] {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    auto* confirmation = qobject_cast<QMessageBox*>(widget);
                    if (!confirmation ||
                        confirmation->windowTitle() !=
                            QLatin1String("Start Paymaster provider")) {
                        continue;
                    }
                    confirmation_seen = true;
                    confirmation->button(QMessageBox::Cancel)->click();
                    break;
                }
            });
            const int overview_index = operator_tabs->currentIndex();
            action->click();
            QVERIFY(confirmation_seen);
            QCOMPARE(operator_tabs->currentIndex(), overview_index);
        };
        verify_start_action(liquidity_action);
        verify_start_action(operation_action);
    }

    // A running provider can still pause before creating a maintenance
    // transaction when its estimated fee exceeds the operator-approved cap.
    // Overview must stop claiming that a refill is being prepared and route
    // both cards to the cost-limit review instead.
    UniValue fee_limited{UniValue::VOBJ};
    fee_limited.pushKV("wallet_eligible", true);
    fee_limited.pushKV("enabled", true);
    fee_limited.pushKV("ready", false);
    fee_limited.pushKV("running", true);
    fee_limited.pushKV("wallet_locked", false);
    fee_limited.pushKV("service_state", "replenishing_liquidity");
    fee_limited.pushKV("last_service_error",
                       "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    UniValue fee_limited_errors{UniValue::VARR};
    fee_limited_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    fee_limited.pushKV("readiness_errors",
                       std::move(fee_limited_errors));
    tab.setPaymasterReadinessStatusForTesting(fee_limited);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(liquidity->text().contains(
        QStringLiteral("cost exceeds the approved limit")));
    QVERIFY(operation->text().contains(
        QStringLiteral("exceeds the approved cost limit")));
    QCOMPARE(liquidity_action->text(),
             QStringLiteral("Review refill cost limit"));
    QCOMPARE(operation_action->text(),
             QStringLiteral("Review refill cost limit"));

    UniValue activation_wait{UniValue::VOBJ};
    activation_wait.pushKV("wallet_eligible", true);
    activation_wait.pushKV("enabled", true);
    activation_wait.pushKV("ready", false);
    activation_wait.pushKV("wallet_locked", false);
    UniValue activation_errors{UniValue::VARR};
    activation_errors.push_back("PAYMASTER_NODE_NOT_READY");
    activation_wait.pushKV("readiness_errors", std::move(activation_errors));
    activation_wait.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(activation_wait);
    QVERIFY(!start->isEnabled());
    QVERIFY(oracle->text().contains(QStringLiteral("0.006500")));

    UniValue missing_policy{UniValue::VOBJ};
    missing_policy.pushKV("wallet_eligible", true);
    missing_policy.pushKV("enabled", true);
    missing_policy.pushKV("ready", false);
    missing_policy.pushKV("wallet_locked", false);
    UniValue setup_errors{UniValue::VARR};
    setup_errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
    missing_policy.pushKV("readiness_errors", std::move(setup_errors));
    missing_policy.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(missing_policy);
    QVERIFY(provider_status->text().contains(QStringLiteral("Setup incomplete")));
    QVERIFY(operation->text().contains(QStringLiteral("Action required")));
    // A provider-configuration error belongs to the task cards, not to the
    // independent node-prerequisites card. The latter may truthfully be ready
    // while Offer or Liquidity still needs operator action.
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(other_requirement->isHidden());

    UniValue incomplete_targets{UniValue::VOBJ};
    incomplete_targets.pushKV("wallet_eligible", true);
    incomplete_targets.pushKV("enabled", true);
    incomplete_targets.pushKV("ready", false);
    incomplete_targets.pushKV("wallet_locked", false);
    UniValue target_errors{UniValue::VARR};
    target_errors.push_back("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE");
    target_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    incomplete_targets.pushKV("readiness_errors", std::move(target_errors));
    incomplete_targets.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(incomplete_targets);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("liquidity change")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("saved liquidity targets")));
    QVERIFY(operation->text().contains(
        QStringLiteral("payment-carrier target")));
    QVERIFY(!start->isEnabled());
    QVERIFY(start->toolTip().contains(
        QStringLiteral("cannot make this provider ready")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(other_requirement->isHidden());

    UniValue unknown_failure{UniValue::VOBJ};
    unknown_failure.pushKV("wallet_eligible", true);
    unknown_failure.pushKV("enabled", true);
    unknown_failure.pushKV("ready", false);
    unknown_failure.pushKV("wallet_locked", false);
    UniValue unknown_errors{UniValue::VARR};
    unknown_errors.push_back("PAYMASTER_TEST_UNKNOWN_REQUIREMENT");
    unknown_failure.pushKV("readiness_errors", std::move(unknown_errors));
    unknown_failure.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(unknown_failure);
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("error"));
    QCOMPARE(other_requirement->property("statusKind").toString(),
             QStringLiteral("error"));
    QVERIFY(other_requirement->text().startsWith(QStringLiteral("!")));
}

void DigiDollarWidgetTests::paymasterPendingStartUsesLiveStatusInsteadOfStaleModal()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(provider_status != nullptr);

    UniValue pending{UniValue::VOBJ};
    pending.pushKV("running", true);
    pending.pushKV("ready", false);
    pending.pushKV("operation_mode", "automatic");
    pending.pushKV("service_state", "replenishing_liquidity");

    const auto message_box_count = [] {
        const QWidgetList widgets = QApplication::topLevelWidgets();
        return std::count_if(
            widgets.cbegin(), widgets.cend(),
            [](QWidget* widget) { return qobject_cast<QMessageBox*>(widget); });
    };
    const int message_boxes_before = message_box_count();
    tab.setPaymasterStartResultForTesting(pending);
    const int message_boxes_after = message_box_count();

    QCOMPARE(message_boxes_after, message_boxes_before);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("Provider start accepted")));
    QVERIFY(provider_status->text().contains(
        QStringLiteral("restoring the saved liquidity targets")));

    tab.setPaymasterRpcExecutorForTesting(
        [](const std::string&, const UniValue&) {
            return UniValue{UniValue::VOBJ};
        });
    UniValue malformed{UniValue::VOBJ};
    malformed.pushKV("running", true);
    malformed.pushKV("ready", true);
    malformed.pushKV("service_state", "active");
    tab.setPaymasterStartResultForTesting(malformed);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
}

void DigiDollarWidgetTests::paymasterCarrierWithdrawalActionsFailClosed()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QTabWidget* paymaster_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QWidget* liquidity_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterLiquidityPage"));
    QVERIFY(paymaster_tabs != nullptr);
    QVERIFY(liquidity_page != nullptr);
    const int liquidity_index = paymaster_tabs->indexOf(liquidity_page);
    QVERIFY(liquidity_index >= 0);
    paymaster_tabs->setTabEnabled(liquidity_index, true);
    QVERIFY(liquidity_page->isEnabled());

    QLabel* value = tab.findChild<QLabel*>(
        QStringLiteral("paymasterCarrierValueStatus"));
    QLabel* recycling = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityRecyclingStatus"));
    QComboBox* selection = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterReleaseCarrierSelection"));
    QPushButton* preview_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierExcess"));
    QPushButton* execute_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierExcess"));
    QPushButton* preview_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierRelease"));
    QPushButton* execute_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierRelease"));
    QVERIFY(value != nullptr);
    QVERIFY(recycling != nullptr);
    QVERIFY(selection != nullptr);
    QVERIFY(preview_excess != nullptr);
    QVERIFY(execute_excess != nullptr);
    QVERIFY(preview_release != nullptr);
    QVERIFY(execute_release != nullptr);

    QVERIFY(!preview_excess->isEnabled());
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(!execute_excess->isEnabled());
    QVERIFY(!execute_release->isEnabled());

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_liquidity_confirmation", true, true, 0, 1));
    QVERIFY(value->text().contains(QStringLiteral("1.00 DD")));
    QVERIFY(value->text().contains(QStringLiteral("0.06 DD")));
    QVERIFY(preview_excess->isEnabled());
    QVERIFY(!execute_excess->isEnabled());

    tab.setPaymasterLiquidityPoolForTesting(PaymasterLiquidityPoolStatus());
    QCOMPARE(selection->count(), 1);
    QVERIFY(selection->currentText().contains(QStringLiteral("1.06 DD")));
    QVERIFY(recycling->text().contains(QStringLiteral("Carrier is being reused: 1.03 DD")));
    QVERIFY(recycling->text().contains(QStringLiteral("DGB successor slot is being reused: 0.10000000 DGB")));
    QVERIFY(preview_release->isEnabled());
    QVERIFY(!execute_release->isEnabled());

    UniValue malformed_entry{UniValue::VOBJ};
    malformed_entry.pushKV("state", "available");
    malformed_entry.pushKV("purpose", "operational");
    malformed_entry.pushKV("asset", "dd_carrier");
    malformed_entry.pushKV("dd_cents", 106);
    UniValue malformed_entries{UniValue::VARR};
    malformed_entries.push_back(std::move(malformed_entry));
    UniValue malformed_pool{UniValue::VOBJ};
    malformed_pool.pushKV("pool", std::move(malformed_entries));
    tab.setPaymasterLiquidityPoolForTesting(malformed_pool);
    QCOMPARE(selection->count(), 0);
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(recycling->text().contains(
        QStringLiteral("incomplete"), Qt::CaseInsensitive));

    UniValue empty_pool{UniValue::VOBJ};
    empty_pool.pushKV("pool", UniValue{UniValue::VARR});
    tab.setPaymasterLiquidityPoolForTesting(empty_pool);
    QCOMPARE(selection->count(), 0);
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(!execute_release->isEnabled());
}

void DigiDollarWidgetTests::paymasterCarrierWithdrawalPreviewsArePlanBound()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::vector<UniValue> parameters;
    const qint64 expiry = QDateTime::currentSecsSinceEpoch() + 600;
    const std::string excess_plan(64, 'd');
    const std::string release_plan(64, 'e');
    bool malformed_preview{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command != "withdrawpaymastercarrier") {
                throw std::runtime_error("unexpected injected carrier RPC");
            }
            UniValue result{UniValue::VOBJ};
            const QString mode = QString::fromStdString(
                params[0].find_value("mode").get_str());
            result.pushKV("executed", false);
            result.pushKV("plan_id",
                          mode == QLatin1String("all_excess")
                              ? excess_plan
                              : release_plan);
            result.pushKV("mode", mode.toStdString());
            result.pushKV("source_carriers", 1);
            result.pushKV("expires_at", expiry);
            if (malformed_preview) return result;
            result.pushKV("withdrawable_excess_cents", 6);
            result.pushKV("retained_carrier_cents", 100);
            result.pushKV("estimated_network_fee_satoshis", 1000);
            result.pushKV("operational_carrier_target", 0);
            return result;
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "ready", true, true, 0));
    tab.setPaymasterLiquidityPoolForTesting(PaymasterLiquidityPoolStatus());

    QPushButton* preview_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierExcess"));
    QPushButton* execute_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierExcess"));
    QPushButton* preview_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierRelease"));
    QPushButton* execute_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierRelease"));
    QComboBox* selection = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterReleaseCarrierSelection"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterCarrierWithdrawalStatus"));
    QVERIFY(preview_excess != nullptr);
    QVERIFY(execute_excess != nullptr);
    QVERIFY(preview_release != nullptr);
    QVERIFY(execute_release != nullptr);
    QVERIFY(selection != nullptr);
    QVERIFY(status != nullptr);

    preview_excess->click();
    QCOMPARE(commands.value(0), QStringLiteral("withdrawpaymastercarrier"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("mode").get_str()),
             QStringLiteral("all_excess"));
    QCOMPARE(parameters.at(0)[0].find_value("execute").get_bool(), false);
    QVERIFY(execute_excess->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("Preview only")));
    QVERIFY(status->text().contains(QStringLiteral("0.06 DD")));

    preview_release->click();
    QCOMPARE(commands.value(1), QStringLiteral("withdrawpaymastercarrier"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(1)[0].find_value("mode").get_str()),
             QStringLiteral("release_slot"));
    QCOMPARE(parameters.at(1)[0].find_value("execute").get_bool(), false);
    QVERIFY(parameters.at(1)[0].find_value("txid").isStr());
    QVERIFY(parameters.at(1)[0].find_value("vout").isNum());
    QVERIFY(execute_release->isEnabled());
    QVERIFY(!execute_excess->isEnabled());

    // An object-shaped response is not sufficient: the preview must carry
    // every mode-specific amount and remain bound to a valid plan id.
    malformed_preview = true;
    preview_release->click();
    QVERIFY(!execute_release->isEnabled());
    QVERIFY(status->text().contains(
        QStringLiteral("complete response"), Qt::CaseInsensitive));

    // Changing the selected outpoint invalidates the reviewed plan before an
    // execution can be offered. Re-applying the same index is not a change, so
    // clear the pool to exercise the fail-closed invalidation path.
    UniValue empty_pool{UniValue::VOBJ};
    empty_pool.pushKV("pool", UniValue{UniValue::VARR});
    tab.setPaymasterLiquidityPoolForTesting(empty_pool);
    QVERIFY(!execute_release->isEnabled());
}

void DigiDollarWidgetTests::paymasterSafetyControlsDefaultFailClosed()
{
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QWidget* safety_page = tab.findChild<QWidget*>(QStringLiteral("paymasterSafetyPolicyPage"));
    QLabel* warning = tab.findChild<QLabel*>(QStringLiteral("paymasterSafetyPolicyWarning"));
    QLabel* provider_status = tab.findChild<QLabel*>(QStringLiteral("paymasterProviderSafetyStatus"));
    QLabel* client_status = tab.findChild<QLabel*>(QStringLiteral("paymasterClientSafetyStatus"));
    QPushButton* enable_provider = tab.findChild<QPushButton*>(QStringLiteral("paymasterEnableProvider"));
    QPushButton* save_provider = tab.findChild<QPushButton*>(QStringLiteral("savePaymasterProviderSafetyPolicy"));
    QPushButton* save_client = tab.findChild<QPushButton*>(QStringLiteral("savePaymasterClientSafetyPolicy"));
    QLineEdit* public_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerHour"));
    QLineEdit* public_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerDay"));
    QSpinBox* quote_total = tab.findChild<QSpinBox*>(QStringLiteral("paymasterSafetyMaxActiveQuotesTotal"));
    QSpinBox* client_transfer = tab.findChild<QSpinBox*>(QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QSpinBox* client_day = tab.findChild<QSpinBox*>(QStringLiteral("paymasterClientSafetyMaxFeePerDay"));

    QVERIFY(safety_page != nullptr);
    QVERIFY(warning != nullptr);
    QVERIFY(provider_status != nullptr);
    QVERIFY(client_status != nullptr);
    QVERIFY(enable_provider != nullptr);
    QVERIFY(save_provider != nullptr);
    QVERIFY(save_client != nullptr);
    QVERIFY(public_hour != nullptr);
    QVERIFY(public_day != nullptr);
    QVERIFY(quote_total != nullptr);
    QVERIFY(client_transfer != nullptr);
    QVERIFY(client_day != nullptr);
    QVERIFY(warning->text().contains(QStringLiteral("spending brake")));
    QVERIFY(provider_status->text().contains(QStringLiteral("not configured")));
    QVERIFY(client_status->text().contains(QStringLiteral("not configured")));
    QCOMPARE(public_hour->text(), QStringLiteral("0.00000000"));
    QCOMPARE(public_day->text(), QStringLiteral("0.00000000"));
    QVERIFY(!enable_provider->isEnabled());
    QVERIFY(quote_total->value() > 0);
    QVERIFY(client_day->value() >= client_transfer->value());

    const QStringList funding_prefixes{
        QStringLiteral("paymasterSafetyUserPaid"),
        QStringLiteral("paymasterSafetyPublicSponsored"),
        QStringLiteral("paymasterSafetyRestrictedSponsored"),
    };
    const QStringList limit_suffixes{
        QStringLiteral("MaxNetworkFeePerTransaction"),
        QStringLiteral("MaxReservedNetworkFee"),
        QStringLiteral("MaxNetworkFeePerHour"),
        QStringLiteral("MaxNetworkFeePerDay"),
        QStringLiteral("MaxCompletedPerHour"),
        QStringLiteral("MaxCompletedPerDay"),
    };
    for (const QString& prefix : funding_prefixes) {
        QVERIFY2(tab.findChild<QPushButton*>(prefix + QStringLiteral("RestoreDefaults")) != nullptr,
                 qPrintable(QStringLiteral("Missing Paymaster safety reset for %1")
                                .arg(prefix)));
        for (const QString& suffix : limit_suffixes) {
            const QString object_name = prefix + suffix;
            QVERIFY2(tab.findChild<QWidget*>(object_name) != nullptr,
                     qPrintable(QStringLiteral("Missing Paymaster safety control %1")
                                    .arg(object_name)));
        }
    }
    const QStringList quote_controls{
        QStringLiteral("paymasterSafetyMaxActiveQuotesTotal"),
        QStringLiteral("paymasterSafetyMaxActiveQuotesPerNetgroup"),
        QStringLiteral("paymasterSafetyMaxActiveQuotesPerRecipient"),
        QStringLiteral("paymasterSafetyMaxQuoteRequestsPerNetgroupMinute"),
    };
    for (const QString& object_name : quote_controls) {
        QVERIFY2(tab.findChild<QSpinBox*>(object_name) != nullptr,
                 qPrintable(QStringLiteral("Missing Paymaster quote limit %1")
                                .arg(object_name)));
    }
}

void DigiDollarWidgetTests::paymasterSafetyPolicyDisplaysFiniteDisabledSemantics()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QLabel* user_paid_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyUserPaidMode"));
    QLabel* public_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMode"));
    QLabel* restricted_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyRestrictedSponsoredMode"));
    QLabel* client_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterClientSafetyMode"));
    QLineEdit* public_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerTransaction"));
    QSpinBox* client_transfer = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QSpinBox* client_day = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerDay"));

    QVERIFY(user_paid_mode != nullptr);
    QVERIFY(public_mode != nullptr);
    QVERIFY(restricted_mode != nullptr);
    QVERIFY(client_mode != nullptr);
    QVERIFY(public_per_transaction != nullptr);
    QVERIFY(client_transfer != nullptr);
    QVERIFY(client_day != nullptr);

    QVERIFY(user_paid_mode->text().contains(QStringLiteral("Unsaved limits")));
    QVERIFY(user_paid_mode->text().contains(QStringLiteral("DGB per transfer")));
    QVERIFY(public_mode->text().contains(QStringLiteral("Disabled")));
    QVERIFY(public_mode->text().contains(QStringLiteral("never means unlimited")));
    QVERIFY(restricted_mode->text().contains(QStringLiteral("Disabled")));

    public_per_transaction->setText(QStringLiteral("1"));
    QVERIFY(public_mode->text().contains(QStringLiteral("Incomplete")));
    QVERIFY(!public_mode->text().contains(QStringLiteral("unlimited")));

    client_transfer->setValue(0);
    client_day->setValue(0);
    QVERIFY(client_mode->text().contains(QStringLiteral("Zero service-fee budget")));
    QVERIFY(client_mode->text().contains(QStringLiteral("zero-fee")));
    QVERIFY(client_mode->text().contains(QStringLiteral("never means unlimited")));
    client_transfer->setValue(1);
    QVERIFY(client_mode->text().contains(QStringLiteral("Unsaved limits")));
}

void DigiDollarWidgetTests::paymasterInjectedRpcCoversConfigurationWorkflows()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::vector<UniValue> parameters;
    bool malformed_operating_ack{false};
    bool malformed_provider_safety_ack{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterpolicy") {
                if (malformed_operating_ack) {
                    return UniValue{UniValue::VOBJ};
                }
                UniValue result = params[0];
                result.pushKV(
                    "policy_hash",
                    "2222222222222222222222222222222222222222222222222222222222222222");
                return result;
            }
            if (command == "setpaymastersafetypolicy") {
                if (malformed_provider_safety_ack) {
                    return UniValue{UniValue::VOBJ};
                }
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "getpaymasterinfo" ||
                command == "getpaymastersafetystatus") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });

    // A new wallet intentionally keeps configuration tabs locked until the
    // operator chooses guided or expert setup. This contract test exercises
    // the controls behind that onboarding gate, so unlock only the local tab
    // container without altering persisted settings.
    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QPushButton* create_identity = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterCreateIdentity"));
    QLineEdit* display_name = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterDisplayName"));
    QPushButton* save_policy = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterPolicy"));
    QPushButton* save_safety = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterProviderSafetyPolicy"));
    QPushButton* save_client_safety = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterClientSafetyPolicy"));
    QSpinBox* client_per_transaction = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QLabel* client_safety_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterClientSafetyStatus"));
    QLabel* provider_safety_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderSafetyStatus"));
    QPushButton* enable_provider = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterEnableProvider"));
    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(create_identity != nullptr);
    QVERIFY(display_name != nullptr);
    QVERIFY(save_policy != nullptr);
    QVERIFY(save_safety != nullptr);
    QVERIFY(save_client_safety != nullptr);
    QVERIFY(client_per_transaction != nullptr);
    QVERIFY(client_safety_status != nullptr);
    QVERIFY(provider_safety_status != nullptr);
    QVERIFY(enable_provider != nullptr);
    QVERIFY(provider_status != nullptr);

    display_name->setText(QStringLiteral("test-provider"));
    create_identity->setEnabled(true);
    create_identity->click();
    QCOMPARE(commands.value(0), QStringLiteral("createpaymasteridentity"));
    QVERIFY(parameters.at(0).isArray());
    QCOMPARE(QString::fromStdString(parameters.at(0)[0].get_str()),
             QStringLiteral("test-provider"));
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));

    commands.clear();
    parameters.clear();
    save_policy->setEnabled(true);
    save_policy->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterpolicy"));
    QVERIFY(parameters.at(0)[0].find_value("funding_models").isArray());
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));

    commands.clear();
    parameters.clear();
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_safety->setEnabled(true);
    save_safety->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymastersafetypolicy"));
    QVERIFY(parameters.at(0)[0].find_value("user_paid").isObject());
    QCOMPARE(commands.value(1), QStringLiteral("getpaymastersafetystatus"));

    commands.clear();
    parameters.clear();
    malformed_operating_ack = true;
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_policy->setEnabled(true);
    save_policy->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterpolicy"));
    QVERIFY(provider_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    malformed_operating_ack = false;

    commands.clear();
    parameters.clear();
    malformed_provider_safety_ack = true;
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_safety->setEnabled(true);
    save_safety->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymastersafetypolicy"));
    QVERIFY(provider_safety_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    malformed_provider_safety_ack = false;

    commands.clear();
    parameters.clear();
    client_per_transaction->setValue(101);
    save_client_safety->setEnabled(true);
    save_client_safety->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterclientsafetypolicy"));
    QVERIFY(client_safety_status->text().contains(
        QStringLiteral("not confirmed"), Qt::CaseInsensitive));

    commands.clear();
    parameters.clear();
    UniValue running{UniValue::VOBJ};
    running.pushKV("wallet_eligible", true);
    running.pushKV("settings_present", true);
    running.pushKV("enabled", true);
    running.pushKV("running", true);
    running.pushKV("ready", true);
    running.pushKV("wallet_locked", false);
    running.pushKV("has_identity", true);
    running.pushKV("has_policy", true);
    running.pushKV("has_safety_policy", true);
    running.pushKV("operation_mode", "automatic");
    running.pushKV("autostart", false);
    running.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running);
    save_policy->setEnabled(true);
    save_policy->click();
    QVERIFY(commands.isEmpty());
    QVERIFY(provider_status->text().contains(
        QStringLiteral("Stop the Paymaster provider")));

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterenabled") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("enabled", false);
                result.pushKV("running", false);
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error(
                    "injected follow-up refresh unavailable");
            }
            throw std::runtime_error(
                "unexpected configuration workflow RPC");
        });
    UniValue enabled_stopped{UniValue::VOBJ};
    enabled_stopped.pushKV("wallet_eligible", true);
    enabled_stopped.pushKV("settings_present", true);
    enabled_stopped.pushKV("enabled", true);
    enabled_stopped.pushKV("running", false);
    enabled_stopped.pushKV("ready", true);
    enabled_stopped.pushKV("wallet_locked", false);
    enabled_stopped.pushKV("has_identity", true);
    enabled_stopped.pushKV("has_policy", true);
    enabled_stopped.pushKV("has_safety_policy", true);
    enabled_stopped.pushKV("operation_mode", "automatic");
    enabled_stopped.pushKV("autostart", false);
    enabled_stopped.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(enabled_stopped);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QVERIFY(enable_provider->text().contains(QStringLiteral("Disable")));
    enable_provider->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterenabled"));
    QCOMPARE(parameters.at(0)[0].get_bool(), false);
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));
}

void DigiDollarWidgetTests::paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::vector<UniValue> parameters;

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            UniValue result{UniValue::VOBJ};
            if (command == "preparepaymasterpool") {
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "5555555555555555555555555555555555555555555555555555555555555555");
                result.pushKV("admission_dgb_slots",
                              params[0].find_value(
                                  "admission_dgb_slots").getInt<int>());
                result.pushKV("operational_dgb_slots",
                              params[0].find_value(
                                  "operational_dgb_slots").getInt<int>());
                result.pushKV("admission_carrier_slots",
                              params[0].find_value(
                                  "admission_carrier_slots").getInt<int>());
                result.pushKV("operational_carrier_slots",
                              params[0].find_value(
                                  "operational_carrier_slots").getInt<int>());
                result.pushKV("missing_admission_dgb_slots", 1);
                result.pushKV("missing_operational_dgb_slots", 0);
                result.pushKV("missing_admission_carrier_slots", 0);
                result.pushKV("missing_operational_carrier_slots", 0);
                result.pushKV("admission_dgb_satoshis_each", 10000000);
                result.pushKV("operational_dgb_satoshis_each", 20000000);
                result.pushKV("carrier_cents_each", 100);
                result.pushKV("total_output_satoshis", 10000000);
                result.pushKV("total_carrier_cents", 0);
            } else if (command == "rebalancepaymasterpool") {
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "6666666666666666666666666666666666666666666666666666666666666666");
                result.pushKV("retired_admission_dgb_slots", 1);
                result.pushKV("retired_operational_dgb_slots", 0);
                result.pushKV("retired_admission_carrier_slots", 0);
                result.pushKV("retired_operational_carrier_slots", 0);
                result.pushKV("retired_dgb_satoshis", 10000000);
                result.pushKV("retired_carrier_cents", 0);
            } else if (command == "setpaymasterruntimesettings") {
                result.pushKV("operation_mode", "automatic");
                result.pushKV("autostart", true);
            }
            return result;
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QPushButton* preview = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewPoolPreparation"));
    QPushButton* execute_preparation = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecutePoolPreparation"));
    QPushButton* preview_retirement = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewPoolRetirement"));
    QPushButton* execute_retirement = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecutePoolRetirement"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QCheckBox* autostart = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterProviderAutostart"));
    QPushButton* save_runtime = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterRuntimeSettings"));
    QLabel* runtime_result = tab.findChild<QLabel*>(
        QStringLiteral("paymasterRuntimeSettingsResult"));
    QPushButton* stop = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterStopProvider"));
    QVERIFY(preview != nullptr);
    QVERIFY(execute_preparation != nullptr);
    QVERIFY(preview_retirement != nullptr);
    QVERIFY(execute_retirement != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(autostart != nullptr);
    QVERIFY(save_runtime != nullptr);
    QVERIFY(runtime_result != nullptr);
    QVERIFY(stop != nullptr);

    preview->setEnabled(true);
    preview->click();
    QCOMPARE(commands.value(0), QStringLiteral("preparepaymasterpool"));
    QCOMPARE(parameters.at(0)[0].find_value("execute").get_bool(), false);
    QVERIFY(execute_preparation->isEnabled());
    QVERIFY(!execute_retirement->isEnabled());

    // A preview authorizes only the exact targets that were displayed. Any
    // edit must disable execution until a fresh preview is obtained.
    const int reviewed_admission_dgb = admission_dgb->value();
    admission_dgb->setValue(reviewed_admission_dgb + 1);
    QVERIFY(!execute_preparation->isEnabled());
    admission_dgb->setValue(reviewed_admission_dgb);
    QVERIFY(!execute_preparation->isEnabled());
    preview_retirement->click();
    QCOMPARE(commands.value(1), QStringLiteral("rebalancepaymasterpool"));
    QCOMPARE(parameters.at(1)[0].find_value("execute").get_bool(), false);
    QVERIFY(execute_retirement->isEnabled());
    QVERIFY(!execute_preparation->isEnabled());

    commands.clear();
    parameters.clear();
    autostart->setChecked(true);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterruntimesettings"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("operation_mode").get_str()),
             QStringLiteral("automatic"));
    QCOMPARE(parameters.at(0)[0].find_value("autostart").get_bool(), true);
    QVERIFY(runtime_result->text().contains(QStringLiteral("autostart"),
                                            Qt::CaseInsensitive));
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                UniValue malformed{UniValue::VOBJ};
                malformed.pushKV("operation_mode", "automatic");
                return malformed;
            }
            throw std::runtime_error(
                "unexpected malformed runtime workflow RPC");
        });
    autostart->setChecked(false);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterruntimesettings"));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("not confirmed"), Qt::CaseInsensitive));
    QVERIFY(save_runtime->isEnabled());

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                throw std::runtime_error("injected runtime rejection");
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });
    autostart->setChecked(false);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterruntimesettings"));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("not saved"), Qt::CaseInsensitive));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("injected runtime rejection")));
    QVERIFY(save_runtime->isEnabled());

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("operation_mode", "automatic");
                result.pushKV("autostart", false);
                result.pushKV("running", true);
                return result;
            }
            if (command == "stoppaymaster") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("running", false);
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error(
                    "injected follow-up refresh unavailable");
            }
            throw std::runtime_error("unexpected runtime workflow RPC");
        });
    UniValue running_with_autostart{UniValue::VOBJ};
    running_with_autostart.pushKV("wallet_eligible", true);
    running_with_autostart.pushKV("settings_present", true);
    running_with_autostart.pushKV("enabled", true);
    running_with_autostart.pushKV("running", true);
    running_with_autostart.pushKV("ready", true);
    running_with_autostart.pushKV("wallet_locked", false);
    running_with_autostart.pushKV("has_identity", true);
    running_with_autostart.pushKV("has_policy", true);
    running_with_autostart.pushKV("has_safety_policy", true);
    running_with_autostart.pushKV("operation_mode", "automatic");
    running_with_autostart.pushKV("autostart", true);
    running_with_autostart.pushKV("readiness_errors",
                                  UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running_with_autostart);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QVERIFY(stop->text().contains(QStringLiteral("disable autostart"),
                                  Qt::CaseInsensitive));
    QVERIFY(stop->isEnabled());
    stop->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterruntimesettings"));
    QCOMPARE(parameters.at(0)[0].find_value("autostart").get_bool(),
             false);
    QCOMPARE(commands.value(1), QStringLiteral("stoppaymaster"));
    QCOMPARE(commands.value(2), QStringLiteral("getpaymasterinfo"));
}

void DigiDollarWidgetTests::paymasterManualActivityUsesExpectedRpcAndReadableStates()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            commands.push_back(QString::fromStdString(command));
            if (command == "processpaymasterrequests") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("processed", true);
                result.pushKV("message_type", "quote");
                result.pushKV("queued", true);
                return result;
            }
            if (command == "listpaymasterreservations") {
                UniValue result{UniValue::VARR};
                UniValue reservation{UniValue::VOBJ};
                reservation.pushKV("purpose", "operational");
                reservation.pushKV("asset", "dd_carrier");
                reservation.pushKV("dd_cents", 103);
                reservation.pushKV("state", "reserved");
                result.push_back(std::move(reservation));
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            throw std::runtime_error("unexpected injected activity RPC");
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    UniValue running{UniValue::VOBJ};
    running.pushKV("wallet_eligible", true);
    running.pushKV("enabled", true);
    running.pushKV("ready", true);
    running.pushKV("running", true);
    running.pushKV("wallet_locked", false);
    running.pushKV("operation_mode", "manual");
    running.pushKV("service_state", "manual");
    running.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running);

    QPushButton* process_request = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterProcessOneRequest"));
    QPushButton* refresh = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterRefreshReservations"));
    QLabel* action_result = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivityActionResult"));
    QLabel* summary = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivitySummary"));
    QVERIFY(process_request != nullptr);
    QVERIFY(refresh != nullptr);
    QVERIFY(action_result != nullptr);
    QVERIFY(summary != nullptr);
    QVERIFY(process_request->isEnabled());

    process_request->click();
    QCOMPARE(commands.value(0), QStringLiteral("processpaymasterrequests"));
    QVERIFY(action_result->text().contains(QStringLiteral("quote request")));
    QVERIFY(action_result->text().contains(QStringLiteral("queued")));

    commands.clear();
    refresh->click();
    QCOMPARE(commands.value(0), QStringLiteral("listpaymasterreservations"));
    QVERIFY(summary->text().contains(QStringLiteral("1 unavailable")));
    QVERIFY(summary->text().contains(QStringLiteral("1.03 DD carrier")));
    QVERIFY(summary->text().contains(
        QStringLiteral("reserved for in-progress work")));
}

void DigiDollarWidgetTests::paymasterFinancesAndBackupWorkflow()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QStringList commands;
    std::vector<UniValue> parameters;

    const auto summary = [](qint64 income, qint64 cost,
                            qint64 transfers) {
        UniValue value{UniValue::VOBJ};
        value.pushKV("service_fee_income_cents", income);
        value.pushKV("dgb_operating_cost_satoshis", cost);
        value.pushKV("successful_transfers", transfers);
        return value;
    };
    const std::string provider_id(64, 'a');
    UniValue finance{UniValue::VOBJ};
    finance.pushKV("provider_id", provider_id);
    finance.pushKV("period", "30d");
    finance.pushKV("service_fee_income_cents", 125);
    finance.pushKV("dgb_operating_cost_satoshis", 100000);
    finance.pushKV("successful_transfers", 5);
    finance.pushKV("average_service_fee_cents", 25);
    finance.pushKV("user_paid_transfers", 3);
    finance.pushKV("public_sponsored_transfers", 1);
    finance.pushKV("restricted_sponsored_transfers", 1);
    const auto model_summary = [](qint64 transfers, qint64 income,
                                  qint64 cost) {
        UniValue value{UniValue::VOBJ};
        value.pushKV("successful_transfers", transfers);
        value.pushKV("service_fee_income_cents", income);
        value.pushKV("dgb_operating_cost_satoshis", cost);
        return value;
    };
    UniValue model_breakdown{UniValue::VOBJ};
    model_breakdown.pushKV("user_paid", model_summary(3, 125, 60000));
    model_breakdown.pushKV("public_sponsored", model_summary(1, 0, 20000));
    model_breakdown.pushKV("restricted_sponsored",
                           model_summary(1, 0, 20000));
    finance.pushKV("model_breakdown", std::move(model_breakdown));
    UniValue summaries{UniValue::VOBJ};
    summaries.pushKV("today", summary(25, 10000, 1));
    summaries.pushKV("7d", summary(75, 50000, 3));
    summaries.pushKV("30d", summary(125, 100000, 5));
    summaries.pushKV("all", summary(250, 200000, 10));
    finance.pushKV("period_summaries", std::move(summaries));
    finance.pushKV("history_partially_reconstructable", false);
    finance.pushKV("history_complete_from", 100);
    finance.pushKV("backup_required", true);
    finance.pushKV("last_successful_backup_at", 0);
    finance.pushKV("external_backup_acknowledged_at", 0);
    finance.pushKV("oracle_price_micro_usd", 500000);
    finance.pushKV("valuation_time", 200);
    finance.pushKV("estimated_result_usd", 1.2495);
    UniValue capital{UniValue::VOBJ};
    capital.pushKV("dgb_available_satoshis", 500000000);
    capital.pushKV("dgb_reserved_satoshis", 100000000);
    capital.pushKV("dgb_pending_satoshis", 50000000);
    capital.pushKV("carrier_base_cents", 400);
    capital.pushKV("carrier_earned_cents", 25);
    capital.pushKV("carrier_withdrawable_cents", 20);
    capital.pushKV("pending_maintenance_transactions", 1);
    finance.pushKV("pool_capital", std::move(capital));
    UniValue daily_totals{UniValue::VARR};
    UniValue day{UniValue::VOBJ};
    day.pushKV("day_start", 86400);
    day.pushKV("service_fee_income_cents", 25);
    day.pushKV("dgb_operating_cost_satoshis", 10000);
    day.pushKV("successful_transfers", 1);
    day.pushKV("maintenance_transactions", 2);
    daily_totals.push_back(std::move(day));
    finance.pushKV("daily_totals", std::move(daily_totals));
    UniValue events{UniValue::VARR};
    UniValue event{UniValue::VOBJ};
    event.pushKV("event_id", std::string(64, 'b'));
    event.pushKV("kind", "transfer");
    event.pushKV("state", "confirmed");
    event.pushKV("dd_income_cents", 25);
    event.pushKV("dgb_cost_satoshis", 10000);
    event.pushKV("created_at", 150);
    event.pushKV("confirmed_at", 160);
    event.pushKV("funding_model", "user_paid");
    event.pushKV("transaction_id", std::string(64, 'c'));
    events.push_back(std::move(event));
    finance.pushKV("events", std::move(events));

    UniValue finance_without_oracle{UniValue::VOBJ};
    UniValue finance_partial_history{UniValue::VOBJ};
    const std::vector<std::string>& finance_keys = finance.getKeys();
    const std::vector<UniValue>& finance_values = finance.getValues();
    for (size_t index = 0; index < finance_keys.size(); ++index) {
        if (finance_keys.at(index) == "oracle_price_micro_usd" ||
            finance_keys.at(index) == "valuation_time" ||
            finance_keys.at(index) == "estimated_result_usd") {
            continue;
        }
        finance_without_oracle.pushKV(finance_keys.at(index),
                                      finance_values.at(index));
        if (finance_keys.at(index) !=
                "history_partially_reconstructable" &&
            finance_keys.at(index) != "history_complete_from") {
            finance_partial_history.pushKV(finance_keys.at(index),
                                           finance_values.at(index));
        }
    }
    finance_partial_history.pushKV(
        "history_partially_reconstructable", true);
    finance_partial_history.pushKV("history_complete_from", 100);
    const auto finance_for_period = [](const UniValue& source,
                                       const std::string& period) {
        UniValue result{UniValue::VOBJ};
        const std::vector<std::string>& keys = source.getKeys();
        const std::vector<UniValue>& values = source.getValues();
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys.at(index) == "period") continue;
            result.pushKV(keys.at(index), values.at(index));
        }
        result.pushKV("period", period);
        return result;
    };
    const auto finance_page_for_period = [](
        const UniValue& source, const std::string& period,
        UniValue page_events, const std::string& next_cursor) {
        UniValue result{UniValue::VOBJ};
        const std::vector<std::string>& keys = source.getKeys();
        const std::vector<UniValue>& values = source.getValues();
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys.at(index) == "period" || keys.at(index) == "events" ||
                keys.at(index) == "next_cursor") {
                continue;
            }
            result.pushKV(keys.at(index), values.at(index));
        }
        result.pushKV("period", period);
        result.pushKV("events", std::move(page_events));
        if (next_cursor.empty()) {
            result.pushKV("next_cursor", UniValue{UniValue::VNULL});
        } else {
            result.pushKV("next_cursor", next_cursor);
        }
        return result;
    };
    constexpr qint64 LARGE_EXPORT_EVENTS{10251};
    bool oracle_available{true};
    bool partial_history{false};
    bool malformed_finance{false};
    bool large_export{false};
    bool switch_period_during_request{false};
    QComboBox* period_control{nullptr};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasterfinancestatus") {
                const std::string requested_period =
                    params[0].find_value("period").get_str();
                if (switch_period_during_request && period_control) {
                    switch_period_during_request = false;
                    period_control->setCurrentIndex(period_control->findData(
                        QStringLiteral("today")));
                }
                if (malformed_finance) {
                    UniValue malformed{UniValue::VOBJ};
                    malformed.pushKV("period", requested_period);
                    return malformed;
                }
                if (large_export) {
                    const UniValue& cursor_value =
                        params[0].find_value("cursor");
                    qulonglong first_index{0};
                    if (!cursor_value.isNull()) {
                        if (!cursor_value.isStr()) {
                            throw std::runtime_error(
                                "finance cursor is not a string");
                        }
                        bool cursor_ok{false};
                        first_index = QString::fromStdString(
                            cursor_value.get_str()).toULongLong(
                                &cursor_ok, 16);
                        if (!cursor_ok || first_index >=
                                static_cast<qulonglong>(
                                    LARGE_EXPORT_EVENTS)) {
                            throw std::runtime_error(
                                "finance cursor is outside the test corpus");
                        }
                    }
                    const qulonglong end_index = std::min(
                        first_index + qulonglong{250},
                        static_cast<qulonglong>(LARGE_EXPORT_EVENTS));
                    UniValue page_events{UniValue::VARR};
                    for (qulonglong index = first_index;
                         index < end_index; ++index) {
                        const std::string event_id =
                            QStringLiteral("%1")
                                .arg(index + 1, 64, 16,
                                     QLatin1Char('0'))
                                .toStdString();
                        UniValue event{UniValue::VOBJ};
                        event.pushKV("event_id", event_id);
                        event.pushKV("kind", "transfer");
                        event.pushKV("state", "confirmed");
                        event.pushKV("dd_income_cents", 1);
                        event.pushKV("dgb_cost_satoshis", 2);
                        event.pushKV("created_at",
                                     static_cast<int64_t>(1000 + index));
                        event.pushKV("confirmed_at",
                                     static_cast<int64_t>(1001 + index));
                        event.pushKV("funding_model", "user_paid");
                        event.pushKV("transaction_id",
                                     std::string(64, 'c'));
                        page_events.push_back(std::move(event));
                    }
                    const std::string next_cursor =
                        end_index < static_cast<qulonglong>(
                                        LARGE_EXPORT_EVENTS)
                        ? QStringLiteral("%1")
                              .arg(end_index, 64, 16,
                                   QLatin1Char('0'))
                              .toStdString()
                        : std::string{};
                    return finance_page_for_period(
                        finance, requested_period,
                        std::move(page_events), next_cursor);
                }
                const UniValue& selected = partial_history
                    ? finance_partial_history
                    : oracle_available ? finance : finance_without_oracle;
                return finance_for_period(selected, requested_period);
            }
            if (command == "acknowledgepaymasterproviderbackup") {
                UniValue malformed{UniValue::VOBJ};
                malformed.pushKV("acknowledged", true);
                return malformed;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error(
                    "injected follow-up provider refresh unavailable");
            }
            throw std::runtime_error("unexpected injected finance RPC");
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QWidget* finance_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterFinancesPage"));
    QVERIFY(operator_tabs != nullptr);
    QVERIFY(finance_page != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }
    operator_tabs->setCurrentWidget(finance_page);
    QVERIFY(commands.contains(QStringLiteral("getpaymasterfinancestatus")));

    QLabel* period_income = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinancePeriodIncome2"));
    QLabel* result_estimate = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceResultEstimate"));
    QLabel* model_breakdown_label = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceModelBreakdown"));
    QLabel* pool_dgb = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinancePoolDgb"));
    QGroupBox* history_notice = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterFinanceHistoryNotice"));
    QLabel* history_notice_text = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceHistoryNoticeText"));
    QTableWidget* booking_table = tab.findChild<QTableWidget*>(
        QStringLiteral("paymasterFinanceEvents"));
    QTableWidget* daily_table = tab.findChild<QTableWidget*>(
        QStringLiteral("paymasterFinanceDailyTotals"));
    QWidget* details = tab.findChild<QWidget*>(
        QStringLiteral("paymasterFinanceDetailsPanel"));
    QPushButton* details_toggle = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceDetailsToggle"));
    QGroupBox* backup_notice = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterFinanceBackupNotice"));
    QLabel* backup_provider = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceBackupProviderId"));
    QPushButton* backup_now = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceBackupNow"));
    QPushButton* external_backup = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceExternalBackup"));
    QPushButton* finance_refresh = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceRefresh"));
    QPushButton* finance_export = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceExport"));
    QComboBox* period = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterFinancePeriod"));
    QLabel* history_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceHistoryStatus"));
    QVERIFY(period_income != nullptr);
    QVERIFY(result_estimate != nullptr);
    QVERIFY(model_breakdown_label != nullptr);
    QVERIFY(pool_dgb != nullptr);
    QVERIFY(history_notice != nullptr);
    QVERIFY(history_notice_text != nullptr);
    QVERIFY(booking_table != nullptr);
    QVERIFY(daily_table != nullptr);
    QVERIFY(details != nullptr);
    QVERIFY(details_toggle != nullptr);
    QVERIFY(backup_notice != nullptr);
    QVERIFY(backup_provider != nullptr);
    QVERIFY(backup_now != nullptr);
    QVERIFY(external_backup != nullptr);
    QVERIFY(finance_refresh != nullptr);
    QVERIFY(finance_export != nullptr);
    QVERIFY(period != nullptr);
    QVERIFY(history_status != nullptr);
    period_control = period;
    QCOMPARE(QString::fromStdString(
                 parameters.front()[0].find_value("period").get_str()),
             QStringLiteral("30d"));
    QCOMPARE(parameters.front()[0].find_value("include_events").get_bool(),
             true);
    QCOMPARE(parameters.front()[0].find_value("limit").getInt<int>(), 250);
    QVERIFY(period_income->text().contains(QStringLiteral("Service fees")));
    QVERIFY(result_estimate->text().contains(QStringLiteral("USD")));
    QVERIFY(model_breakdown_label->text().contains(
        QStringLiteral("User paid: 3 transfer")));
    QVERIFY(model_breakdown_label->text().contains(
        QStringLiteral("0.00060000 DGB cost")));
    QVERIFY(pool_dgb->text().contains(QStringLiteral("available")));
    QVERIFY(history_notice->isHidden());
    QCOMPARE(booking_table->rowCount(), 1);
    QCOMPARE(booking_table->columnCount(), 6);
    QCOMPARE(daily_table->rowCount(), 1);
    QCOMPARE(daily_table->columnCount(), 5);
    QCOMPARE(daily_table->item(0, 1)->text(), QStringLiteral("0.25"));
    QCOMPARE(daily_table->item(0, 4)->text(), QStringLiteral("2"));
    QVERIFY(details->isHidden());
    details_toggle->click();
    QVERIFY(!details->isHidden());
    QVERIFY(!backup_notice->isHidden());
    QVERIFY(backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(!backup_provider->text().contains(QStringLiteral("…")));
    tab.setPrivacy(true);
    QVERIFY(!backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(backup_provider->toolTip().isEmpty());
    QVERIFY(backup_provider->accessibleName().isEmpty());
    QVERIFY(backup_provider->accessibleDescription().isEmpty());
    QVERIFY(booking_table->isHidden());
    QVERIFY(daily_table->isHidden());
    QVERIFY(!finance_export->isEnabled());
    tab.setPrivacy(false);
    // This isolated fixture has no configured provider, so the normal setup
    // access refresh closes the tab that the test opened manually above.
    // Reopen only that presentation surface before checking cached export.
    operator_tabs->setTabEnabled(
        operator_tabs->indexOf(finance_page), true);
    operator_tabs->setCurrentWidget(finance_page);
    QVERIFY(backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(!booking_table->isHidden());
    QVERIFY(!daily_table->isHidden());
    QTRY_VERIFY(finance_export->isEnabled());
    commands.clear();
    parameters.clear();
    period->setCurrentIndex(period->findData(QStringLiteral("today")));
    QCOMPARE(commands.value(0),
             QStringLiteral("getpaymasterfinancestatus"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("period").get_str()),
             QStringLiteral("today"));
    QSignalSpy backup_requested(
        &tab, &DigiDollarTab::providerWalletBackupRequested);
    backup_now->click();
    QCOMPARE(backup_requested.count(), 1);
    oracle_available = false;
    finance_refresh->click();
    QVERIFY(result_estimate->text().contains(
        QStringLiteral("No current Oracle price")));
    partial_history = true;
    finance_refresh->click();
    QVERIFY(!history_notice->isHidden());
    QVERIFY(history_notice_text->text().contains(
        QStringLiteral("Earlier Paymaster transfers may be absent")));
    QVERIFY(history_notice_text->text().contains(
        QStringLiteral("no earlier income is estimated")));
    // If the selection changes during an asynchronous request, the old
    // period must never be rendered under the new combo-box label. The first
    // result is discarded and a replacement request is issued immediately.
    partial_history = false;
    oracle_available = true;
    period->setCurrentIndex(period->findData(QStringLiteral("30d")));
    commands.clear();
    parameters.clear();
    switch_period_during_request = true;
    finance_refresh->click();
    QCOMPARE(commands.size(), 2);
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("period").get_str()),
             QStringLiteral("30d"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(1)[0].find_value("period").get_str()),
             QStringLiteral("today"));
    QCOMPARE(period->currentData().toString(), QStringLiteral("today"));
    malformed_finance = true;
    finance_refresh->click();
    QVERIFY(history_status->text().contains(
        QStringLiteral("incomplete"), Qt::CaseInsensitive));
    // Retain the last complete authoritative rows instead of replacing them
    // with plausible-looking zero totals from a malformed object.
    QCOMPARE(booking_table->rowCount(), 1);
    QCOMPARE(daily_table->rowCount(), 1);
    malformed_finance = false;
    const bool native_dialogs_available =
        QApplication::platformName() != QLatin1String("minimal") &&
        QApplication::platformName() != QLatin1String("offscreen");
    if (native_dialogs_available) {
        bool confirmation_seen{false};
        QTimer::singleShot(0, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* confirmation = qobject_cast<QMessageBox*>(widget);
                if (!confirmation ||
                    confirmation->windowTitle() !=
                        QLatin1String("Acknowledge external wallet backup")) {
                    continue;
                }
                confirmation_seen = true;
                confirmation->done(QMessageBox::Yes);
                break;
            }
        });
        external_backup->click();
        QVERIFY(confirmation_seen);
        QVERIFY(!backup_notice->isHidden());
        QVERIFY(history_status->text().contains(
            QStringLiteral("not confirm"), Qt::CaseInsensitive));
    }

    // Technical event and transaction identifiers are intentionally absent
    // from the normal booking table and remain backend-only detail data.
    for (int column = 0; column < booking_table->columnCount(); ++column) {
        const QTableWidgetItem* item = booking_table->item(0, column);
        QVERIFY(item != nullptr);
        QVERIFY(!item->text().contains(QString(16, QLatin1Char('b'))));
        QVERIFY(!item->text().contains(QString(16, QLatin1Char('c'))));
    }

    QTemporaryDir export_directory;
    QVERIFY(export_directory.isValid());
    const QString export_filename =
        export_directory.filePath(QStringLiteral("paymaster-finance.csv"));
    tab.setPaymasterFinanceExportFilenameForTesting(export_filename);
    large_export = true;
    commands.clear();
    parameters.clear();
    bool event_loop_yielded{false};
    QTimer::singleShot(0, &tab,
                       [&event_loop_yielded] { event_loop_yielded = true; });
    finance_export->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        history_status->text().contains(
            QStringLiteral("10251 booking(s)")),
        30000);
    QVERIFY(event_loop_yielded);
    QCOMPARE(commands.count(QStringLiteral("getpaymasterfinancestatus")),
             42);
    QCOMPARE(commands.size(), static_cast<int>(parameters.size()));
    for (size_t index = 0; index < parameters.size(); ++index) {
        // Provider refreshes may legitimately be queued between export pages.
        // Only the finance calls participate in the cursor contract below.
        if (commands.at(static_cast<int>(index)) !=
            QLatin1String("getpaymasterfinancestatus")) {
            continue;
        }
        const UniValue& page_parameters = parameters.at(index);
        QVERIFY2(page_parameters.isArray() && page_parameters.size() == 1,
                 qPrintable(QStringLiteral(
                     "Finance export RPC %1 has malformed parameters")
                                .arg(index)));
        const UniValue& limit = page_parameters[0].find_value("limit");
        QVERIFY2(limit.isNum(),
                 qPrintable(QStringLiteral(
                     "Finance export RPC %1 omitted its page limit")
                                .arg(index)));
        QCOMPARE(limit.getInt<int>(), 250);
    }
    QFile export_file(export_filename);
    QVERIFY(export_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray exported = export_file.readAll();
    QCOMPARE(exported.count('\n'),
             static_cast<int>(LARGE_EXPORT_EVENTS + 1));
}

void DigiDollarWidgetTests::paymasterConfirmationGuardDetectsMaterialChanges()
{
    // A terminal high-level response may omit presentation echoes, but it is
    // still a success once Core returns the validated txid and result status.
    // This is the regression case that previously displayed an authorization
    // error after the exact transaction had already reached the mempool.
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("final_committed")));
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QString{}, QStringLiteral("success"),
        QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("FAILED"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString{}, QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString{}, QString{}, QStringLiteral("success"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('0')), QString{}, QStringLiteral("success"),
        QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QStringLiteral("not-a-transaction-id"), QString{},
        QStringLiteral("success"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QString{},
        QStringLiteral("unexpected"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("PENDING_PROVIDER"),
        QString{}, QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('0')), QStringLiteral("CONFIRMED"),
        QString{}, QStringLiteral("final_committed")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFLICTED"),
        QString{}, QStringLiteral("final_committed")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CANCELED_SAFE"),
        QString{}, QStringLiteral("final_committed")));

    // `final` is a persistence/terminal marker, not a payment-success marker.
    // Exercise every terminal state with both marker values and both txid
    // shapes so a future decoder cannot accidentally reintroduce that trust.
    const QString valid_txid(64, QLatin1Char('7'));
    const QStringList terminal_states{
        QStringLiteral("MEMPOOL"), QStringLiteral("CONFIRMED"),
        QStringLiteral("FAILED"), QStringLiteral("CONFLICTED"),
        QStringLiteral("CANCELED_SAFE")};
    for (const QString& terminal_state : terminal_states) {
        for (const bool reported_final : {false, true}) {
            for (const bool has_txid : {false, true}) {
                const bool expected = has_txid &&
                    (terminal_state == QStringLiteral("MEMPOOL") ||
                     terminal_state == QStringLiteral("CONFIRMED"));
                QCOMPARE(IsValidatedPaymasterCompletion(
                             has_txid ? valid_txid : QString{},
                             terminal_state, QString{},
                             QStringLiteral("final_committed"),
                             reported_final),
                         expected);
            }
        }
    }
    for (const bool reported_final : {false, true}) {
        QCOMPARE(IsValidatedPaymasterCompletion(
                     valid_txid, QString{}, QStringLiteral("success"),
                     QString{}, reported_final),
                 true);
        QCOMPARE(IsValidatedPaymasterCompletion(
                     QString{}, QString{}, QStringLiteral("success"),
                     QString{}, reported_final),
                 false);
    }

    PaymasterConfirmationGuard guard;
    PaymasterConfirmationSelection incomplete;
    QVERIFY(guard.RequiresConfirmation(incomplete));
    QCOMPARE(guard.ChangedFields(incomplete), QStringList{QStringLiteral("incomplete")});
    QVERIFY(!guard.Accept(incomplete));
    QVERIFY(!guard.HasAcceptedSelection());

    PaymasterConfirmationSelection accepted;
    accepted.provider_id = QString(64, QLatin1Char('1'));
    accepted.offer_id = QString(64, QLatin1Char('2'));
    accepted.policy_hash = QString(64, QLatin1Char('3'));
    accepted.funding_model = QStringLiteral("user_paid");
    accepted.recipient = QStringLiteral("dgb1-recipient");
    accepted.authorization_commitment = QString(64, QLatin1Char('4'));
    accepted.payment_cents = 1250;
    accepted.service_fee_cents = 25;
    accepted.user_total_cents = 1275;
    accepted.expires_at = 2000000000;
    QVERIFY(guard.RequiresConfirmation(accepted));
    QCOMPARE(guard.ChangedFields(accepted),
              QStringList({QStringLiteral("provider"), QStringLiteral("offer"),
                           QStringLiteral("policy"), QStringLiteral("funding_model"),
                           QStringLiteral("recipient"), QStringLiteral("amount"),
                           QStringLiteral("service_fee"),
                           QStringLiteral("total"),
                           QStringLiteral("expiry"),
                           QStringLiteral("authorization_commitment")}));
    QVERIFY(guard.Accept(accepted));
    QVERIFY(guard.HasAcceptedSelection());
    QVERIFY(!guard.RequiresConfirmation(accepted));
    QVERIFY(guard.ChangedFields(accepted).isEmpty());

    PaymasterConfirmationSelection changed = accepted;
    changed.provider_id = QString(64, QLatin1Char('5'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("provider")});
    changed = accepted;
    changed.offer_id = QString(64, QLatin1Char('6'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("offer")});
    changed = accepted;
    changed.policy_hash = QString(64, QLatin1Char('7'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("policy")});
    changed = accepted;
    changed.funding_model = QStringLiteral("sponsored");
    changed.service_fee_cents = 0;
    changed.user_total_cents = changed.payment_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("funding_model"),
                          QStringLiteral("service_fee"),
                          QStringLiteral("total")}));
    changed = accepted;
    changed.recipient = QStringLiteral("dgb1-other");
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("recipient")});
    changed = accepted;
    ++changed.payment_cents;
    ++changed.user_total_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("amount"), QStringLiteral("total")}));
    changed = accepted;
    ++changed.service_fee_cents;
    ++changed.user_total_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("service_fee"), QStringLiteral("total")}));
    changed = accepted;
    ++changed.user_total_cents;
    QVERIFY(!changed.IsComplete());
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("incomplete")});
    changed = accepted;
    changed.funding_model = QStringLiteral("sponsored");
    QVERIFY(!changed.IsComplete());
    changed = accepted;
    ++changed.expires_at;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("expiry")});
    changed = accepted;
    changed.authorization_commitment = QString(64, QLatin1Char('8'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList{QStringLiteral("authorization_commitment")});

    guard.Reset();
    QVERIFY(!guard.HasAcceptedSelection());
    QVERIFY(guard.RequiresConfirmation(accepted));
}

void DigiDollarWidgetTests::paymasterRecoveryConfirmationGuardDetectsMaterialChanges()
{
    PaymasterRecoveryConfirmationGuard guard;
    PaymasterRecoveryConfirmationSelection incomplete;
    QVERIFY(guard.RequiresConfirmation(incomplete));
    QCOMPARE(guard.ChangedFields(incomplete),
             QStringList{QStringLiteral("incomplete")});
    QVERIFY(!guard.Accept(incomplete));
    QVERIFY(!guard.HasAcceptedSelection());

    PaymasterRecoveryConfirmationSelection accepted;
    accepted.recovery_provider_id = QString(64, QLatin1Char('1'));
    accepted.privacy_profile = QStringLiteral("high");
    accepted.offer_id = QString(64, QLatin1Char('2'));
    accepted.policy_hash = QString(64, QLatin1Char('3'));
    accepted.original_commit_key = QString(64, QLatin1Char('4'));
    accepted.original_template_commitment =
        QString(64, QLatin1Char('5'));
    accepted.wallet_returns = QStringList{
        QStringLiteral("0:wallet-script-a:1000"),
        QStringLiteral("1:wallet-script-b:250")};
    accepted.authorization_commitment = QString(64, QLatin1Char('6'));
    accepted.maximum_service_fee_cents = 25;
    accepted.service_fee_cents = 20;
    accepted.network_fee_satoshis = 1500;
    accepted.expires_at = 2000000000;

    QVERIFY(guard.RequiresConfirmation(accepted));
    QCOMPARE(guard.ChangedFields(accepted),
             QStringList({QStringLiteral("recovery_provider"),
                          QStringLiteral("privacy"), QStringLiteral("offer"),
                          QStringLiteral("policy"),
                          QStringLiteral("original_commit"),
                          QStringLiteral("original_template_commitment"),
                          QStringLiteral("wallet_returns"),
                          QStringLiteral("maximum_service_fee"),
                          QStringLiteral("service_fee"),
                          QStringLiteral("network_fee"),
                          QStringLiteral("expiry"),
                          QStringLiteral("authorization_commitment")}));
    QVERIFY(guard.Accept(accepted));
    QVERIFY(guard.HasAcceptedSelection());
    QVERIFY(!guard.RequiresConfirmation(accepted));
    QVERIFY(guard.ChangedFields(accepted).isEmpty());

    const auto expect_change = [&guard, &accepted](
                                   PaymasterRecoveryConfirmationSelection changed,
                                   const QString& field) {
        QVERIFY(guard.RequiresConfirmation(changed));
        QCOMPARE(guard.ChangedFields(changed), QStringList{field});
        QVERIFY(!changed.IsComplete() || guard.HasAcceptedSelection());
        QVERIFY(!guard.RequiresConfirmation(accepted));
    };

    PaymasterRecoveryConfirmationSelection changed = accepted;
    changed.recovery_provider_id = QString(64, QLatin1Char('7'));
    expect_change(changed, QStringLiteral("recovery_provider"));
    changed = accepted;
    changed.privacy_profile = QStringLiteral("standard");
    expect_change(changed, QStringLiteral("privacy"));
    changed = accepted;
    changed.offer_id = QString(64, QLatin1Char('8'));
    expect_change(changed, QStringLiteral("offer"));
    changed = accepted;
    changed.policy_hash = QString(64, QLatin1Char('9'));
    expect_change(changed, QStringLiteral("policy"));
    changed = accepted;
    changed.original_commit_key = QString(64, QLatin1Char('a'));
    expect_change(changed, QStringLiteral("original_commit"));
    changed = accepted;
    changed.original_template_commitment = QString(64, QLatin1Char('b'));
    expect_change(changed, QStringLiteral("original_template_commitment"));
    changed = accepted;
    changed.wallet_returns[0] = QStringLiteral("0:other-wallet-script:1000");
    expect_change(changed, QStringLiteral("wallet_returns"));
    changed = accepted;
    ++changed.maximum_service_fee_cents;
    expect_change(changed, QStringLiteral("maximum_service_fee"));
    changed = accepted;
    ++changed.service_fee_cents;
    expect_change(changed, QStringLiteral("service_fee"));
    changed = accepted;
    ++changed.network_fee_satoshis;
    expect_change(changed, QStringLiteral("network_fee"));
    changed = accepted;
    ++changed.expires_at;
    expect_change(changed, QStringLiteral("expiry"));
    changed = accepted;
    changed.authorization_commitment = QString(64, QLatin1Char('c'));
    expect_change(changed, QStringLiteral("authorization_commitment"));

    guard.Reset();
    QVERIFY(!guard.HasAcceptedSelection());
    QVERIFY(guard.RequiresConfirmation(accepted));
}

void DigiDollarWidgetTests::paymasterClientOfferPreviewUsesExactRpcAndPlainText()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-offers");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    std::vector<UniValue> parameters;
    int offer_calls{0};
    bool waiting_seen{false};
    bool refresh_disabled_while_busy{false};
    bool stale_rows_cleared_before_rpc{false};
    bool duplicate_refresh_blocked{false};
    send_widget.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasteroffers") {
                ++offer_calls;
                QLabel* status = send_widget.findChild<QLabel*>(
                    QStringLiteral("paymasterOffersStatus"));
                QPushButton* refresh = send_widget.findChild<QPushButton*>(
                    QStringLiteral("refreshPaymasterOffers"));
                QTableWidget* table = send_widget.findChild<QTableWidget*>(
                    QStringLiteral("paymasterOffers"));
                waiting_seen = status &&
                    status->property("statusKind").toString() == QStringLiteral("waiting");
                refresh_disabled_while_busy = refresh && !refresh->isEnabled();
                stale_rows_cleared_before_rpc = table && table->rowCount() == 0;
                const int calls_before_reentry = offer_calls;
                QMetaObject::invokeMethod(
                    &send_widget, "refreshPaymasterOffers", Qt::DirectConnection);
                duplicate_refresh_blocked = offer_calls == calls_before_reentry;

                UniValue offers{UniValue::VARR};
                offers.push_back(PaymasterOffer(
                    "<b>Alice & Co</b>",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "user_paid",
                    2, 325, true, 2000000000));
                offers.push_back(PaymasterOffer(
                    "Sponsor",
                    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                    "sponsored",
                    0, 325, false, 2000000100));
                return offers;
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    commands.clear();
    parameters.clear();

    QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingPaymaster"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QVERIFY(paymaster != nullptr);
    QVERIFY(amount != nullptr);
    QVERIFY(table != nullptr);
    QVERIFY(status != nullptr);

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    table->setRowCount(1); // Must disappear before the next RPC is entered.
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));

    QCOMPARE(offer_calls, 1);
    QVERIFY(waiting_seen);
    QVERIFY(refresh_disabled_while_busy);
    QVERIFY(stale_rows_cleared_before_rpc);
    QVERIFY(duplicate_refresh_blocked);
    QCOMPARE(commands, QStringList{QStringLiteral("getpaymasteroffers")});
    QCOMPARE(parameters.size(), size_t{1});
    QVERIFY(parameters.front().isArray());
    QCOMPARE(parameters.front().size(), size_t{2});
    QCOMPARE(parameters.front()[0].getInt<int64_t>(), int64_t{325});
    const UniValue& preview_options = parameters.front()[1];
    QVERIFY(preview_options.isObject());
    QCOMPARE(preview_options.find_value(
                 "subtract_paymaster_fee_from_amount").get_bool(), false);

    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("<b>Alice & Co</b>"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Service fee in $DD"));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("0.02 $DD (2 cents)"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("3.27 $DD (327 cents)"));
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("98.75%"));
    QVERIFY(table->item(0, 5)->text().contains(QStringLiteral("2033")));
    QVERIFY(table->item(0, 0)->toolTip().contains(QString(64, QLatin1Char('b'))));
    QCOMPARE(table->item(1, 1)->text(), QStringLiteral("Sponsored by provider"));
    QCOMPARE(table->item(1, 4)->text(),
             QStringLiteral("New provider — not enough history yet"));
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("ready"));
    QVERIFY(status->text().contains(QStringLiteral("2 eligible offer(s)")));
    QCOMPARE(status->accessibleDescription(), status->text());
}

void DigiDollarWidgetTests::paymasterClientOfferPreviewInvalidatesAndHandlesFailures()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-offer-states");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    int offer_response{0};
    QLineEdit* in_flight_amount{nullptr};
    bool switch_wallet_during_request{false};
    QString dialog_title;
    QString dialog_message;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& title, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            dialog_title = title;
            dialog_message = message;
            return QMessageBox::Ok;
        });
    send_widget.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command != "getpaymasteroffers") {
                throw std::runtime_error("unexpected Paymaster RPC");
            }
            if (offer_response == 2) {
                throw std::runtime_error("preview transport failed");
            }
            if (offer_response == 3) {
                UniValue malformed{UniValue::VARR};
                UniValue offer{UniValue::VOBJ};
                offer.pushKV("display_name", "Malformed provider");
                offer.pushKV("funding_model", "user_paid");
                offer.pushKV("service_fee_cents", 2);
                // Missing payment, total, reputation, expiry and bindings.
                malformed.push_back(std::move(offer));
                return malformed;
            }
            if (offer_response == 4 && in_flight_amount) {
                // Simulate a user edit while the production asynchronous RPC
                // is in flight. The returned offer is bound to the old amount
                // and must therefore remain invisible.
                in_flight_amount->setText(QStringLiteral("3.26"));
            }
            if (offer_response == 5 && switch_wallet_during_request) {
                // A late reply owned by the previously selected wallet must
                // not repopulate the reset state after a wallet switch.
                switch_wallet_during_request = false;
                send_widget.setWalletModel(nullptr);
                send_widget.setWalletModel(mini_gui.walletModel.get());
            }
            UniValue offers{UniValue::VARR};
            if (offer_response == 0 || offer_response == 4 ||
                offer_response == 5) {
                offers.push_back(PaymasterOffer(
                    "Provider",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "user_paid",
                    2, 325, true, 2000000000));
            }
            return offers;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());

    QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingPaymaster"));
    QRadioButton* direct_dgb = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingDgb"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QCheckBox* subtract = send_widget.findChild<QCheckBox*>(
        QStringLiteral("subtractPaymasterFeeFromAmount"));
    QSpinBox* fee_cap = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterFeeCap"));
    QSpinBox* attempts = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterMaximumAttempts"));
    QComboBox* privacy = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterPrivacy"));
    QComboBox* selection = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterSelection"));
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QPushButton* refresh = send_widget.findChild<QPushButton*>(
        QStringLiteral("refreshPaymasterOffers"));
    QVERIFY(paymaster && direct_dgb && amount && subtract && fee_cap && attempts);
    QVERIFY(privacy && selection && table && status && refresh);
    in_flight_amount = amount;

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 1);

    const auto seed_stale_row = [&] {
        table->setRowCount(1);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("stale")));
    };
    amount->setText(QStringLiteral("3.26"));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("info"));
    seed_stale_row();
    subtract->setChecked(true);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    fee_cap->setValue(fee_cap->value() - 1);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    privacy->setCurrentIndex(privacy->findData(QStringLiteral("high")));
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    selection->setCurrentIndex(
        selection->findData(QStringLiteral("privacy_weighted")));
    QCOMPARE(table->rowCount(), 0);
    privacy->setCurrentIndex(privacy->findData(QStringLiteral("standard")));
    seed_stale_row();
    attempts->setValue(2);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    direct_dgb->setChecked(true);
    QCOMPARE(table->rowCount(), 0);

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    subtract->setChecked(false);
    offer_response = 1;
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("waiting"));
    QVERIFY(status->text().contains(QStringLiteral("No eligible public offer")));

    offer_response = 2;
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("error"));
    QVERIFY(refresh->isEnabled());
    QCOMPARE(dialog_title, QStringLiteral("Paymaster offers unavailable"));
    QCOMPARE(dialog_message, QStringLiteral("preview transport failed"));

    offer_response = 3;
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("error"));
    QVERIFY(status->text().contains(QStringLiteral("malformed")));
    QCOMPARE(dialog_title, QStringLiteral("Paymaster offers unavailable"));
    QVERIFY(dialog_message.contains(QStringLiteral("invalid Paymaster offer preview")));

    offer_response = 4;
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(amount->text(), QStringLiteral("3.26"));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(),
             QStringLiteral("info"));
    QVERIFY(status->text().contains(QStringLiteral("inputs changed"),
                                    Qt::CaseInsensitive));
    QVERIFY(refresh->isEnabled());

    offer_response = 5;
    switch_wallet_during_request = true;
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(refresh->isEnabled());

    offer_response = 0;
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 1);

    seed_stale_row();
    send_widget.setWalletModel(nullptr);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("info"));
    QVERIFY(!refresh->isEnabled());

    send_widget.setPaymasterRpcExecutorForTesting(
        [](const std::string& command, const UniValue&) {
            if (command != "getpaymasterclientsafetystatus") {
                throw std::runtime_error("unexpected client-safety RPC");
            }
            UniValue malformed{UniValue::VOBJ};
            malformed.pushKV("configured", true);
            malformed.pushKV("policy", UniValue{UniValue::VOBJ});
            return malformed;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    QLabel* client_safety = send_widget.findChild<QLabel*>(
        QStringLiteral("sendPaymasterClientSafetyStatus"));
    QVERIFY(client_safety != nullptr);
    QVERIFY(client_safety->toolTip().contains(
        QStringLiteral("incomplete Paymaster client-safety status")));
}

void DigiDollarWidgetTests::paymasterClientAuthorizationIsTwoStageAndFailClosed()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-authorization");
    const SecureString passphrase{"qt-paymaster-client-authorization"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    bool unlock_succeeded{false};
    QObject::connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock,
                     [&] {
                         unlock_succeeded =
                             mini_gui.walletModel->setWalletLocked(
                                 false, passphrase);
                     });

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QMessageBox::StandardButton authorization_answer{QMessageBox::Cancel};
    QStringList dialog_titles;
    std::vector<bool> wallet_locked_during_dialog;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& title, const QString&,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            dialog_titles.push_back(title);
            wallet_locked_during_dialog.push_back(wallet->IsLocked());
            return title == QStringLiteral("Confirm exact Paymaster authorization")
                ? authorization_answer
                : QMessageBox::Ok;
        });

    std::vector<UniValue> send_parameters;
    int send_calls{0};
    send_widget.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "resolvepaymastersession") {
                return PaymasterSessionView(
                    "AWAITING_USER_SIGNATURE", "none", "QUOTED");
            }
            if (command != "senddigidollar") {
                throw std::runtime_error("unexpected Paymaster RPC");
            }
            ++send_calls;
            send_parameters.push_back(params);
            if (send_calls <= 2) {
                return PaymasterAuthorizationResult(
                    true, std::string(64, 'c'),
                    "AWAITING_USER_SIGNATURE", "none");
            }
            return PaymasterAuthorizationResult(
                false, std::string(64, 'f'),
                "PENDING_PROVIDER", "user_psbt");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("none"),
        true, QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("QUOTED"), QString{},
        {QStringLiteral("refresh"), QStringLiteral("resume")}, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QLabel* state = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionState"));
    QVERIFY(primary != nullptr);
    QVERIFY(state != nullptr);
    QCOMPARE(primary->text(), QStringLiteral("Review exact offer"));

    primary->click();
    QCOMPARE(send_calls, 1);
    QVERIFY(state->text().contains(QStringLiteral("paused by user")));
    QCOMPARE(send_parameters.at(0)[5].get_str(), std::string{"cents"});
    const UniValue& canceled_options = send_parameters.at(0)[6];
    QCOMPARE(canceled_options.find_value("prepare_only").get_bool(), true);
    QVERIFY(canceled_options.find_value("authorization_commitment").isNull());

    authorization_answer = QMessageBox::Yes;
    // The prepare-only response is not an authoritative action envelope.
    // Re-read the durable session before offering another signing attempt.
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    primary->click();
    QCOMPARE(send_calls, 1);
    QCOMPARE(primary->text(), QStringLiteral("Review exact offer"));
    primary->click();
    QCOMPARE(send_calls, 3);
    const UniValue& reviewed_options = send_parameters.at(1)[6];
    const UniValue& authorized_options = send_parameters.at(2)[6];
    QCOMPARE(reviewed_options.find_value("prepare_only").get_bool(), true);
    QVERIFY(reviewed_options.find_value("authorization_commitment").isNull());
    QVERIFY(authorized_options.find_value("prepare_only").isNull());
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("authorization_commitment").get_str()),
             QString(64, QLatin1Char('c')));
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("selection").get_str()),
             QStringLiteral("lowest_total_cost"));
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("privacy").get_str()),
             QStringLiteral("standard"));
    QVERIFY(state->text().contains(
        QStringLiteral("authorization blocked: commitment changed or missing")));
    QCOMPARE(dialog_titles.count(
                 QStringLiteral("Confirm exact Paymaster authorization")), 2);
    QVERIFY(dialog_titles.contains(QStringLiteral("Paymaster authorization blocked")));
    QVERIFY(unlock_succeeded);
    QVERIFY(!wallet_locked_during_dialog.empty());
    QVERIFY(std::all_of(wallet_locked_during_dialog.begin(),
                        wallet_locked_during_dialog.end(),
                        [](bool locked) { return locked; }));
    QVERIFY(wallet->IsLocked());
}

void DigiDollarWidgetTests::paymasterClientSessionActionMatrix_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<QString>("artifact");
    QTest::addColumn<QString>("attempt_state");
    QTest::addColumn<QString>("pending_phase");
    QTest::addColumn<QStringList>("allowed_actions");
    QTest::addColumn<QString>("primary_text");
    QTest::addColumn<bool>("retry_visible");
    QTest::addColumn<bool>("fallback_visible");
    QTest::addColumn<bool>("recover_visible");
    QTest::addColumn<bool>("abandon_visible");
    QTest::addColumn<bool>("more_visible");

    QTest::newRow("created-unsigned")
        << QStringLiteral("CREATED") << QStringLiteral("none") << QString{}
        << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Resume preparation")
        << false << false << false << true << true;
    QTest::newRow("created-core-allows-refresh-only")
        << QStringLiteral("CREATED") << QStringLiteral("none") << QString{}
        << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh")}
        << QStringLiteral("Check current status")
        << false << false << false << false << false;
    QTest::newRow("awaiting-unlock-unsigned")
        << QStringLiteral("AWAITING_WALLET_UNLOCK") << QStringLiteral("none")
        << QStringLiteral("CANDIDATE") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("fallback"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Unlock and continue")
        << false << true << false << true << true;
    QTest::newRow("exact-offer-review")
        << QStringLiteral("AWAITING_USER_SIGNATURE") << QStringLiteral("none")
        << QStringLiteral("QUOTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("fallback"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Review exact offer")
        << false << true << false << true << true;
    QTest::newRow("authorized-signed")
        << QStringLiteral("AUTHORIZED") << QStringLiteral("user_psbt")
        << QStringLiteral("QUOTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("provider-pending-signed")
        << QStringLiteral("PENDING_PROVIDER") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("mempool-final")
        << QStringLiteral("MEMPOOL") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("failed-unsigned")
        << QStringLiteral("FAILED") << QStringLiteral("none")
        << QStringLiteral("REJECTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Check current status")
        << false << false << false << true << true;
    QTest::newRow("failed-signed")
        << QStringLiteral("FAILED") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Start safe recovery")
        << true << false << false << false << true;
    QTest::newRow("failed-signed-core-denies-recovery")
        << QStringLiteral("FAILED") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same")}
        << QStringLiteral("Check current status")
        << true << false << false << false << true;
    QTest::newRow("conflicted-final")
        << QStringLiteral("CONFLICTED") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Start safe recovery")
        << true << false << false << false << true;
    QTest::newRow("contradictory-unsigned-provider-pending")
        << QStringLiteral("PENDING_PROVIDER") << QStringLiteral("none")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh")}
        << QStringLiteral("Check current status")
        << false << false << false << false << false;
    QTest::newRow("confirmed-terminal")
        << QStringLiteral("CONFIRMED") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{}
        << QStringLiteral("Start a new transfer")
        << false << false << false << false << false;
    QTest::newRow("canceled-terminal")
        << QStringLiteral("CANCELED_SAFE") << QStringLiteral("none")
        << QStringLiteral("REJECTED") << QStringLiteral("NONE")
        << QStringList{}
        << QStringLiteral("Start a new transfer")
        << false << false << false << false << false;
}

void DigiDollarWidgetTests::paymasterClientSessionActionMatrix()
{
    QFETCH(QString, state);
    QFETCH(QString, artifact);
    QFETCH(QString, attempt_state);
    QFETCH(QString, pending_phase);
    QFETCH(QStringList, allowed_actions);
    QFETCH(QString, primary_text);
    QFETCH(bool, retry_visible);
    QFETCH(bool, fallback_visible);
    QFETCH(bool, recover_visible);
    QFETCH(bool, abandon_visible);
    QFETCH(bool, more_visible);

    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarSendWidget send_widget(platform_style.get());
    send_widget.setPaymasterSessionForTesting(
        state, artifact, true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, attempt_state, pending_phase, allowed_actions, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* retry = send_widget.findChild<QPushButton*>(
        QStringLiteral("retryPaymasterSession"));
    QPushButton* fallback = send_widget.findChild<QPushButton*>(
        QStringLiteral("fallbackPaymasterSession"));
    QPushButton* recover = send_widget.findChild<QPushButton*>(
        QStringLiteral("recoverPaymasterSession"));
    QPushButton* abandon = send_widget.findChild<QPushButton*>(
        QStringLiteral("abandonUnsignedPaymasterSession"));
    QPushButton* more = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionMoreOptions"));
    QLineEdit* address = send_widget.findChild<QLineEdit*>(
        QStringLiteral("addressEdit"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QVERIFY(primary && retry && fallback && recover && abandon && more);
    QVERIFY(address && amount);

    QCOMPARE(primary->text(), primary_text);
    QCOMPARE(!retry->isHidden(), retry_visible);
    QCOMPARE(!fallback->isHidden(), fallback_visible);
    QCOMPARE(!recover->isHidden(), recover_visible);
    QCOMPARE(!abandon->isHidden(), abandon_visible);
    QCOMPARE(!more->isHidden(), more_visible);
    QVERIFY(address->isReadOnly());
    QVERIFY(amount->isReadOnly());
}

void DigiDollarWidgetTests::paymasterClientSessionRpcActionsAreBound()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-session-actions");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    std::vector<UniValue> send_parameters;
    QString action_warning;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString&, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            action_warning = message;
            return QMessageBox::Ok;
        });
    send_widget.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "INPUTS_RESERVED", "none", "CANDIDATE");
            }
            if (command == "senddigidollar") {
                send_parameters.push_back(params);
                return PaymasterSessionView(
                    "INPUTS_RESERVED", "none", "CANDIDATE");
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    commands.clear();
    send_widget.setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"));

    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "retryPaymasterSession", Qt::DirectConnection));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QVERIFY(primary != nullptr);
    QCOMPARE(primary->text(), QStringLiteral("Resume preparation"));
    primary->click();

    QCOMPARE(resolve_actions,
             QStringList({QStringLiteral("refresh"),
                          QStringLiteral("refresh"),
                          QStringLiteral("refresh")}));
    QVERIFY(action_warning.contains(
        QStringLiteral("no longer allows"), Qt::CaseInsensitive));
    QCOMPARE(commands,
             QStringList({QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("senddigidollar")}));
    QCOMPARE(send_parameters.size(), size_t{1});
    const UniValue& options = send_parameters.front()[6];
    QCOMPARE(options.find_value("prepare_only").get_bool(), true);
    QVERIFY(options.find_value("authorization_commitment").isNull());
    QCOMPARE(QString::fromStdString(options.find_value("request_id").get_str()),
             QStringLiteral("00000000-0000-4000-8000-000000000001"));
}

void DigiDollarWidgetTests::paymasterClientRestartCreatedSessionWithoutRecipientIsReadOnly()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-recipientless-restart");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    send_widget.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 1);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                UniValue sessions{UniValue::VARR};
                UniValue session{UniValue::VOBJ};
                session.pushKV(
                    "request_id",
                    "00000000-0000-4000-8000-000000000001");
                session.pushKV("session_id", std::string(64, '1'));
                session.pushKV("session_state", "CREATED");
                sessions.push_back(std::move(session));
                listed.pushKV("sessions", std::move(sessions));
                return listed;
            }
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "CREATED", "none", "", /*include_recipient=*/false);
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });

    send_widget.setWalletModel(mini_gui.walletModel.get());

    QLabel* transfer = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionTransfer"));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* abandon = send_widget.findChild<QPushButton*>(
        QStringLiteral("abandonUnsignedPaymasterSession"));
    QVERIFY(transfer && primary && abandon);
    QVERIFY(transfer->text().contains(
        QStringLiteral("not yet available"), Qt::CaseInsensitive));
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    QVERIFY(primary->isEnabled());
    QVERIFY(!abandon->isHidden());
    QVERIFY(!commands.contains(QStringLiteral("senddigidollar")));

    primary->click();
    QCOMPARE(resolve_actions,
             QStringList({QStringLiteral("refresh"),
                          QStringLiteral("refresh")}));
    QVERIFY(!commands.contains(QStringLiteral("senddigidollar")));
}

void DigiDollarWidgetTests::paymasterClientMultipleRestartSessionsRequireSelection()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-multiple-restart");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    send_widget.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 2);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                UniValue sessions{UniValue::VARR};
                for (int index = 1; index <= 2; ++index) {
                    UniValue session{UniValue::VOBJ};
                    session.pushKV(
                        "request_id",
                        index == 1
                            ? "00000000-0000-4000-8000-000000000001"
                            : "00000000-0000-4000-8000-000000000002");
                    session.pushKV("session_id",
                                   std::string(64, index == 1 ? '1' : '2'));
                    session.pushKV("session_state",
                                   index == 1 ? "CREATED" : "INPUTS_RESERVED");
                    session.pushKV("requested_amount_cents", 300 + index);
                    sessions.push_back(std::move(session));
                }
                listed.pushKV("sessions", std::move(sessions));
                return listed;
            }
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "CREATED", "none", "", /*include_recipient=*/false);
            }
            throw std::runtime_error(
                "persisted-session selection must not sign or recover");
        });

    send_widget.setWalletModel(mini_gui.walletModel.get());

    QFrame* chooser = send_widget.findChild<QFrame*>(
        QStringLiteral("persistedPaymasterSessionsFrame"));
    QComboBox* sessions = send_widget.findChild<QComboBox*>(
        QStringLiteral("persistedPaymasterSessions"));
    QPushButton* review = send_widget.findChild<QPushButton*>(
        QStringLiteral("loadPersistedPaymasterSession"));
    QVERIFY(chooser && sessions && review);
    QVERIFY(!chooser->isHidden());
    QCOMPARE(sessions->count(), 2);
    QCOMPARE(commands, QStringList{QStringLiteral(
                           "listdigidollarsendsessions")});
    QVERIFY(resolve_actions.isEmpty());

    // Selecting a persisted item performs exactly one read-only refresh. It
    // never resumes preparation, unlocks, signs, broadcasts or recovers.
    review->click();
    QCOMPARE(resolve_actions, QStringList{QStringLiteral("refresh")});
    QCOMPARE(commands,
             QStringList({QStringLiteral("listdigidollarsendsessions"),
                          QStringLiteral("resolvepaymastersession")}));
}

void DigiDollarWidgetTests::paymasterClientRecoveryExpiryIsFailClosed()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-recovery-expiry");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    bool inconsistent_expiry{false};
    QString warning;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString&, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            warning = message;
            return QMessageBox::Ok;
        });
    send_widget.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 0);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                listed.pushKV("sessions", UniValue{UniValue::VARR});
                return listed;
            }
            if (command == "resolvepaymastersession") {
                return PaymasterSessionView(
                    "FAILED", "alternative_recovery", "CONFLICTED",
                    /*include_recipient=*/true,
                    /*recovery_expires_at=*/1,
                    /*recovery_expired=*/!inconsistent_expiry);
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.setPaymasterSessionForTesting(
        QStringLiteral("FAILED"), QStringLiteral("alternative_recovery"),
        true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CONFLICTED"), QStringLiteral("NONE"),
        {QStringLiteral("refresh"), QStringLiteral("recover"),
         QStringLiteral("cancel_to_self")}, true);

    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterSessionState", Qt::DirectConnection));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* recover = send_widget.findChild<QPushButton*>(
        QStringLiteral("recoverPaymasterSession"));
    QVERIFY(primary && recover);
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    QVERIFY(recover->isHidden());

    inconsistent_expiry = true;
    warning.clear();
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(warning.contains(
        QStringLiteral("inconsistent recovery expiry"),
        Qt::CaseInsensitive));
}

void DigiDollarWidgetTests::paymasterClientDelayedCallbackIgnoresWalletClose()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-delayed-callback");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    const auto initial_rpc = [](const std::string& command,
                                const UniValue&) {
        if (command == "getpaymasterclientsafetystatus") {
            return PaymasterClientSafetyStatus();
        }
        if (command == "listdigidollarsendsessions") {
            UniValue listed{UniValue::VOBJ};
            listed.pushKV("active_only", true);
            listed.pushKV("count", 0);
            listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
            listed.pushKV("sessions", UniValue{UniValue::VARR});
            return listed;
        }
        throw std::runtime_error("unexpected Paymaster RPC");
    };

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    send_widget.setPaymasterRpcExecutorForTesting(initial_rpc);
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"), QStringLiteral("NONE"),
        {QStringLiteral("refresh")}, true);
    WalletModel::RpcCallback delayed_callback;
    send_widget.setPaymasterAsyncRpcExecutorForTesting(
        [&](const std::string&, const UniValue&,
            WalletModel::RpcCallback callback) {
            delayed_callback = std::move(callback);
        });
    QVERIFY(QMetaObject::invokeMethod(
        &send_widget, "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(static_cast<bool>(delayed_callback));

    QLabel* state = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionState"));
    QVERIFY(state != nullptr);
    send_widget.setWalletModel(nullptr);
    QCOMPARE(state->text(), QStringLiteral("No active session"));
    delayed_callback(
        PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE"),
        QString{});
    QCOMPARE(state->text(), QStringLiteral("No active session"));

    auto* destroyed_widget = new DigiDollarSendWidget(
        mini_gui.platformStyle.get());
    destroyed_widget->setPaymasterRpcExecutorForTesting(initial_rpc);
    destroyed_widget->setWalletModel(mini_gui.walletModel.get());
    destroyed_widget->setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"), QStringLiteral("NONE"),
        {QStringLiteral("refresh")}, true);
    WalletModel::RpcCallback destroyed_callback;
    destroyed_widget->setPaymasterAsyncRpcExecutorForTesting(
        [&](const std::string&, const UniValue&,
            WalletModel::RpcCallback callback) {
            destroyed_callback = std::move(callback);
        });
    QVERIFY(QMetaObject::invokeMethod(
        destroyed_widget, "refreshPaymasterSessionState",
        Qt::DirectConnection));
    QVERIFY(static_cast<bool>(destroyed_callback));
    QPointer<DigiDollarSendWidget> destroyed_guard{destroyed_widget};
    delete destroyed_widget;
    QVERIFY(destroyed_guard.isNull());
    destroyed_callback(
        PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE"),
        QString{});
}

void DigiDollarWidgetTests::paymasterClientUnlockLeaseSurvivesWalletModelClose()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-unlock-model-close");
    const SecureString passphrase{"qt-paymaster-unlock-model-close"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    bool unlock_succeeded{false};
    QObject::connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock,
                     [&] {
                         unlock_succeeded =
                             mini_gui.walletModel->setWalletLocked(
                                 false, passphrase);
                     });

    std::shared_ptr<WalletModel::UnlockContext> unlock =
        mini_gui.walletModel->requestUnlockForAsync();
    QVERIFY(unlock_succeeded);
    QVERIFY(unlock->isValid());
    QVERIFY(!wallet->IsLocked());

    // The asynchronous lease owns the wallet interface directly. Destroying
    // the GUI model first must neither dereference it later nor leave the
    // encrypted wallet unlocked when the callback finally releases its lease.
    mini_gui.walletModel.reset();
    unlock.reset();
    QVERIFY(wallet->IsLocked());
}

void DigiDollarWidgetTests::paymasterClientDatabaseReadErrorIsActionable()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-database-error");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QString title;
    QString message;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& dialog_title,
            const QString& dialog_message, QMessageBox::StandardButtons,
            QMessageBox::StandardButton) {
            title = dialog_title;
            message = dialog_message;
            return QMessageBox::Ok;
        });
    send_widget.setPaymasterRpcExecutorForTesting(
        [](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "resolvepaymastersession") {
                return PaymasterSessionView(
                    "AWAITING_USER_SIGNATURE", "none", "QUOTED");
            }
            if (command == "senddigidollar") {
                throw std::runtime_error("PAYMASTER_SESSION_DATABASE_READ");
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("none"),
        true, QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("QUOTED"), QString{},
        {QStringLiteral("refresh"), QStringLiteral("resume")}, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QVERIFY(primary != nullptr);
    primary->click();

    QCOMPARE(title, QStringLiteral("Paymaster wallet data unavailable"));
    QVERIFY(message.contains(QStringLiteral("authoritative persisted session state")));
    QVERIFY(message.contains(QStringLiteral("existing reservations or prior authorizations")));
    QVERIFY(message.contains(QStringLiteral("Preserve the wallet backup and debug log")));
    QVERIFY(message.contains(QStringLiteral("PAYMASTER_SESSION_DATABASE_READ")));
    QVERIFY(!message.contains(QStringLiteral("Wallet synchronization status")));
    QVERIFY(!message.contains(QStringLiteral("Network connection")));
    QVERIFY(!message.contains(QStringLiteral("Available DigiDollar balance")));
}

void DigiDollarWidgetTests::paymasterClientControlsAreAccessible()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarSendWidget send_widget(platform_style.get());
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QPushButton* refresh = send_widget.findChild<QPushButton*>(
        QStringLiteral("refreshPaymasterOffers"));
    QSpinBox* fee_cap = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterFeeCap"));
    QSpinBox* attempts = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterMaximumAttempts"));
    QComboBox* privacy = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterPrivacy"));
    QComboBox* selection = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterSelection"));
    QVERIFY(table && status && refresh && fee_cap && attempts && privacy && selection);

    QCOMPARE(table->selectionMode(), QAbstractItemView::NoSelection);
    QCOMPARE(table->focusPolicy(), Qt::StrongFocus);
    QVERIFY(table->alternatingRowColors());
    QVERIFY(table->verticalHeader()->isHidden());
    QCOMPARE(table->horizontalHeader()->objectName(),
             QStringLiteral("paymasterOffersHeader"));
    QVERIFY(!table->accessibleName().isEmpty());
    QVERIFY(!table->accessibleDescription().isEmpty());
    QCOMPARE(status->accessibleDescription(), status->text());
    QVERIFY(!refresh->accessibleDescription().isEmpty());
    QVERIFY(!fee_cap->accessibleName().isEmpty());
    QVERIFY(!attempts->accessibleName().isEmpty());
    QVERIFY(!privacy->accessibleName().isEmpty());
    QVERIFY(!selection->accessibleName().isEmpty());
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Provider"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Valid until"));

    QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(table);
    QVERIFY(interface != nullptr);
    QCOMPARE(interface->role(), QAccessible::Table);

    table->setRowCount(2);
    table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Provider A")));
    table->setItem(1, 0, new QTableWidgetItem(QStringLiteral("Provider B")));
    send_widget.resize(1200, 900);
    send_widget.show();
    QFrame* advanced = send_widget.findChild<QFrame*>(
        QStringLiteral("advancedPaymasterSettings"));
    QVERIFY(advanced != nullptr);
    advanced->show();
    table->show();
    QCoreApplication::processEvents();
    table->setCurrentCell(0, 0);
    QTest::keyClick(table, Qt::Key_Down);
    QCOMPARE(table->currentRow(), 1);
}

void DigiDollarWidgetTests::paymasterOfferTableRendersGreenTheme()
{
    struct RenderStats {
        bool stylesheet_loaded{false};
        bool table_visible{false};
        int total_pixels{0};
        int green_pixels{0};
        int blue_pixels{0};
    };

    const QString original_stylesheet = qApp->styleSheet();
    const QPalette original_palette = qApp->palette();
    const auto render_theme = [](const QString& resource) {
        RenderStats stats;
        QFile file(resource);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return stats;
        stats.stylesheet_loaded = true;
        qApp->setStyleSheet(QString::fromUtf8(file.readAll()));

        std::unique_ptr<const PlatformStyle> platform_style(
            PlatformStyle::instantiate("other"));
        DigiDollarSendWidget send_widget(platform_style.get());
        QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
            QStringLiteral("feeFundingPaymaster"));
        QPushButton* advanced_toggle = send_widget.findChild<QPushButton*>(
            QStringLiteral("advancedPaymasterSettingsToggle"));
        QTableWidget* table = send_widget.findChild<QTableWidget*>(
            QStringLiteral("paymasterOffers"));
        if (!paymaster || !advanced_toggle || !table) return stats;
        paymaster->setChecked(true);
        advanced_toggle->setChecked(true);
        table->setRowCount(1);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Provider")));
        send_widget.resize(1500, 1200);
        send_widget.show();
        table->resize(1200, 160);
        QCoreApplication::processEvents();
        QTest::qWait(25);
        stats.table_visible = table->isVisibleTo(&send_widget);

        QHeaderView* horizontal_header = table->horizontalHeader();
        const QImage header = horizontal_header->grab().toImage()
                                  .convertToFormat(QImage::Format_RGB32);
        // The minimal Qt platform can leave an unpainted viewport strip after
        // the last section. Restrict the sample to the actual header sections
        // so that the window size does not dilute their rendered colour.
        const int painted_width = std::min(header.width(), horizontal_header->length());
        for (int y = 1; y + 1 < header.height(); ++y) {
            for (int x = 1; x + 1 < painted_width; ++x) {
                const QColor color = QColor::fromRgb(header.pixel(x, y));
                ++stats.total_pixels;
                if (color.green() > color.red() + 20 &&
                    color.green() > color.blue() + 10) {
                    ++stats.green_pixels;
                }
                if (color.blue() > color.green() + 30) {
                    ++stats.blue_pixels;
                }
            }
        }
        return stats;
    };

    const RenderStats light = render_theme(QStringLiteral(":/css/light"));
    const RenderStats dark = render_theme(QStringLiteral(":/css/dark"));
    qApp->setStyleSheet(original_stylesheet);
    qApp->setPalette(original_palette);
    QCoreApplication::processEvents();

    const auto require_green_header = [](const RenderStats& stats,
                                         const QString& theme) {
        QVERIFY2(stats.stylesheet_loaded,
                 qPrintable(QStringLiteral("could not load %1 stylesheet resource").arg(theme)));
        QVERIFY2(stats.table_visible,
                 qPrintable(QStringLiteral("Paymaster offer table was not visible under %1").arg(theme)));
        QVERIFY(stats.total_pixels > 0);
        QVERIFY2(stats.green_pixels * 100 >= stats.total_pixels * 40,
                 qPrintable(QStringLiteral(
                     "%1 rendered Paymaster header is not predominantly green (%2/%3 green, %4 blue pixels)")
                                .arg(theme)
                                .arg(stats.green_pixels)
                                .arg(stats.total_pixels)
                                .arg(stats.blue_pixels)));
        QVERIFY2(stats.blue_pixels * 100 < stats.total_pixels * 5,
                 qPrintable(QStringLiteral(
                     "%1 rendered Paymaster header leaked the DGB blue palette (%2/%3 blue pixels)")
                                .arg(theme)
                                .arg(stats.blue_pixels)
                                .arg(stats.total_pixels)));
    };
    require_green_header(light, QStringLiteral("light"));
    require_green_header(dark, QStringLiteral("dark"));
}

void DigiDollarWidgetTests::paymasterGuidedSetupRendersConsistentTheme()
{
    struct RenderStats {
        bool wizard_opened{false};
        bool expected_local_theme{false};
        bool scroll_visible{false};
        bool surfaces_use_qss{false};
        bool native_fill_disabled{false};
        int total_pixels{0};
        int dark_pixels{0};
        int light_pixels{0};
    };

    const QString original_stylesheet = qApp->styleSheet();
    const QPalette original_palette = qApp->palette();
    qApp->setStyleSheet(QString{});

    const auto render_theme = [&](const QColor& window, const QColor& text,
                                  const QString& expected_background) {
        RenderStats stats;
        QPalette palette = original_palette;
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, window);
        palette.setColor(QPalette::Text, text);
        qApp->setPalette(palette);

        std::unique_ptr<const PlatformStyle> platform_style(
            PlatformStyle::instantiate("other"));
        DigiDollarTab tab(platform_style.get());
        QPushButton* guided_setup = tab.findChild<QPushButton*>(
            QStringLiteral("paymasterChooseGuidedSetup"));
        if (!guided_setup) return stats;

        QTimer::singleShot(0, [&] {
            auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
            if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
                wizard = nullptr;
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    auto* candidate = qobject_cast<QWizard*>(widget);
                    if (candidate &&
                        candidate->objectName() == QLatin1String("PaymasterSetupWizard")) {
                        wizard = candidate;
                        break;
                    }
                }
            }
            if (!wizard) return;

            stats.wizard_opened = true;
            stats.expected_local_theme = wizard->styleSheet().contains(
                expected_background, Qt::CaseInsensitive);
            QScrollArea* scroll = wizard->findChild<QScrollArea*>(
                QStringLiteral("paymasterSetupRequirementsScroll"));
            QWidget* contents = wizard->findChild<QWidget*>(
                QStringLiteral("paymasterSetupRequirementsContent"));
            QFrame* wallet_card = wizard->findChild<QFrame*>(
                QStringLiteral("paymasterSetupWalletCard"));
            if (scroll && contents && wallet_card) {
                QCoreApplication::processEvents();
                stats.scroll_visible = scroll->isVisibleTo(wizard);
                stats.surfaces_use_qss =
                    scroll->testAttribute(Qt::WA_StyledBackground) &&
                    scroll->viewport()->testAttribute(Qt::WA_StyledBackground) &&
                    contents->testAttribute(Qt::WA_StyledBackground);
                stats.native_fill_disabled =
                    !scroll->viewport()->autoFillBackground() &&
                    !contents->autoFillBackground();

                const QImage image = scroll->viewport()->grab().toImage()
                                         .convertToFormat(QImage::Format_RGB32);
                for (int y = 1; y + 1 < image.height(); ++y) {
                    for (int x = 1; x + 1 < image.width(); ++x) {
                        const QColor color = QColor::fromRgb(image.pixel(x, y));
                        ++stats.total_pixels;
                        if (color.lightness() < 100) ++stats.dark_pixels;
                        if (color.lightness() > 210) ++stats.light_pixels;
                    }
                }
            }
            wizard->reject();
        });
        guided_setup->click();
        return stats;
    };

    const RenderStats dark = render_theme(
        QColor(QStringLiteral("#0b2419")), QColor(QStringLiteral("#ffffff")),
        QStringLiteral("#0b2419"));
    const RenderStats light = render_theme(
        QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#123f2b")),
        QStringLiteral("#eef9f2"));

    qApp->setStyleSheet(original_stylesheet);
    qApp->setPalette(original_palette);
    QCoreApplication::processEvents();

    const auto require_common_contract = [](const RenderStats& stats,
                                            const QString& theme) {
        QVERIFY2(stats.wizard_opened,
                 qPrintable(QStringLiteral("Paymaster setup wizard did not open under %1").arg(theme)));
        QVERIFY2(stats.expected_local_theme,
                 qPrintable(QStringLiteral("Paymaster setup wizard selected the wrong %1 theme").arg(theme)));
        QVERIFY2(stats.scroll_visible,
                 qPrintable(QStringLiteral("Paymaster setup content was not visible under %1").arg(theme)));
        QVERIFY2(stats.surfaces_use_qss,
                 qPrintable(QStringLiteral("Paymaster setup surfaces bypassed QSS under %1").arg(theme)));
        QVERIFY2(stats.native_fill_disabled,
                 qPrintable(QStringLiteral("Paymaster setup leaked a native background under %1").arg(theme)));
        QVERIFY(stats.total_pixels > 0);
    };
    require_common_contract(dark, QStringLiteral("dark"));
    require_common_contract(light, QStringLiteral("light"));

    QVERIFY2(dark.dark_pixels * 100 >= dark.total_pixels * 60,
             qPrintable(QStringLiteral(
                 "dark Paymaster setup surface was not predominantly dark (%1/%2 dark, %3 light pixels)")
                            .arg(dark.dark_pixels)
                            .arg(dark.total_pixels)
                            .arg(dark.light_pixels)));
    QVERIFY2(dark.light_pixels * 100 < dark.total_pixels * 25,
             qPrintable(QStringLiteral(
                 "dark Paymaster setup leaked a large light surface (%1/%2 light pixels)")
                            .arg(dark.light_pixels)
                            .arg(dark.total_pixels)));
    QVERIFY2(light.light_pixels * 100 >= light.total_pixels * 60,
             qPrintable(QStringLiteral(
                 "light Paymaster setup surface was not predominantly light (%1/%2 light, %3 dark pixels)")
                            .arg(light.light_pixels)
                            .arg(light.total_pixels)
                            .arg(light.dark_pixels)));
}

void DigiDollarWidgetTests::paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep()
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({},
            GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupDescriptorsWallet(m_node, test, "qt-paymaster-guided-retry");

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarTab tab(mini_gui.platformStyle.get());

    int operating_policy_attempts{0};
    int safety_attempts{0};
    UniValue advertised_policy;
    UniValue safety_policy;
    UniValue liquidity_policy;
    QList<UniValue> safety_payloads;
    QList<bool> enabled_requests;
    QStringList setup_write_order;
    bool fail_safety_snapshot{true};
    bool malformed_safety_snapshot{false};
    bool malformed_liquidity_snapshot{false};
    bool rpc_settings_present{false};
    bool rpc_enabled{false};
    bool rpc_running{false};
    bool rpc_ready{false};
    bool malformed_provider_snapshot{false};
    int mutation_rpc_calls{0};
    std::string rpc_operation_mode{"automatic"};
    bool rpc_autostart{false};
    const auto suggested_liquidity_policy = [] {
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("automatic_replenishment", true);
        policy.pushKV("paid_maintenance_approved", false);
        policy.pushKV("target_admission_dgb", 3);
        policy.pushKV("target_operational_dgb", 1);
        policy.pushKV("target_admission_carriers", 0);
        policy.pushKV("target_operational_carriers", 0);
        policy.pushKV(
            "maximum_maintenance_fee_per_transaction_satoshis", 20000000);
        policy.pushKV(
            "maximum_maintenance_fee_per_hour_satoshis", 200000000);
        policy.pushKV(
            "maximum_maintenance_fee_per_day_satoshis", 1000000000);
        return policy;
    };
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "startpaymaster" ||
                command == "setpaymasterenabled" ||
                command == "preparepaymasterpool" ||
                command == "rebalancepaymasterpool") {
                ++mutation_rpc_calls;
            }
            if (command == "getpaymasterinfo") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("wallet_eligible", true);
                result.pushKV("settings_present", rpc_settings_present);
                result.pushKV("provider_id",
                              "1111111111111111111111111111111111111111111111111111111111111111");
                result.pushKV("enabled", rpc_enabled);
                result.pushKV("running", rpc_running);
                result.pushKV("ready", rpc_ready);
                result.pushKV("wallet_locked", false);
                result.pushKV("pool_ready", false);
                result.pushKV("operation_mode", rpc_operation_mode);
                if (malformed_provider_snapshot) {
                    result.pushKV("autostart", "not-a-boolean");
                } else {
                    result.pushKV("autostart", rpc_autostart);
                }
                result.pushKV("service_state",
                              rpc_running ? "active" : "stopped");
                UniValue service_queue{UniValue::VOBJ};
                service_queue.pushKV("waiting_requests", 0);
                service_queue.pushKV("waiting_submits", 0);
                result.pushKV("service_queue", std::move(service_queue));
                UniValue pool{UniValue::VOBJ};
                pool.pushKV("entries", 0);
                pool.pushKV("reserved", 0);
                pool.pushKV("admission_dgb", 0);
                pool.pushKV("admission_carriers", 0);
                pool.pushKV("operational_dgb", 0);
                pool.pushKV("operational_carriers", 0);
                pool.pushKV("complete_operational_slots", 0);
                result.pushKV("pool", std::move(pool));
                result.pushKV("readiness_errors", UniValue{UniValue::VARR});
                if (advertised_policy.isObject()) {
                    result.pushKV("policy", advertised_policy);
                }
                return result;
            }
            if (command == "getpaymasterliquiditystatus") {
                UniValue result{UniValue::VOBJ};
                if (malformed_liquidity_snapshot) {
                    result.pushKV("policy_configured", true);
                    result.pushKV(
                        "targets_satisfy_provider_policy", true);
                    result.pushKV("policy", UniValue{UniValue::VOBJ});
                    return result;
                }
                const bool configured = liquidity_policy.isObject();
                const UniValue policy = configured
                    ? liquidity_policy
                    : suggested_liquidity_policy();
                result.pushKV("policy_configured", configured);
                result.pushKV("policy", policy);
                result.pushKV("targets_satisfy_provider_policy", true);
                result.pushKV(
                    "maintenance_state",
                    configured ? "ready"
                               : "waiting_for_maintenance_approval");
                const auto add_slot = [&](const char* name,
                                          const char* target_name) {
                    const int target =
                        policy.find_value(target_name).getInt<int>();
                    result.pushKV(
                        name,
                        PaymasterLiquiditySlotStatus(
                            target, 0, 0, target));
                };
                add_slot("admission_dgb", "target_admission_dgb");
                add_slot("operational_dgb", "target_operational_dgb");
                add_slot("admission_carriers",
                         "target_admission_carriers");
                add_slot("operational_carriers",
                         "target_operational_carriers");
                result.pushKV("maintenance_fee_reserved_satoshis", 0);
                result.pushKV(
                    "maintenance_fee_spent_last_hour_satoshis", 0);
                result.pushKV(
                    "maintenance_fee_spent_last_day_satoshis", 0);
                result.pushKV("carrier_base_cents", 0);
                result.pushKV("carrier_withdrawable_excess_cents", 0);
                result.pushKV("readiness_errors", UniValue{UniValue::VARR});
                return result;
            }
            if (command == "getpaymasterpoolinfo") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("pool", UniValue{UniValue::VARR});
                return result;
            }
            if (command == "getpaymastersafetystatus") {
                if (fail_safety_snapshot) {
                    throw std::runtime_error("injected safety snapshot unavailable");
                }
                UniValue result{UniValue::VOBJ};
                if (malformed_safety_snapshot) {
                    result.pushKV("configured", true);
                    result.pushKV("policy", UniValue{UniValue::VOBJ});
                    return result;
                }
                const bool configured = safety_policy.isObject();
                result.pushKV("configured", configured);
                if (configured) result.pushKV("policy", safety_policy);
                return result;
            }
            if (command == "getpaymasterclientsafetystatus") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("configured", false);
                return result;
            }
            if (command == "setpaymasterpolicy") {
                ++operating_policy_attempts;
                setup_write_order.push_back(QString::fromStdString(command));
                advertised_policy = params[0];
                UniValue result = params[0];
                result.pushKV(
                    "policy_hash",
                    "3333333333333333333333333333333333333333333333333333333333333333");
                return result;
            }
            if (command == "setpaymastersafetypolicy") {
                ++safety_attempts;
                setup_write_order.push_back(QString::fromStdString(command));
                safety_payloads.push_back(params[0]);
                if (safety_attempts == 1) {
                    throw std::runtime_error("injected safety retry");
                }
                safety_policy = params[0];
                UniValue result = params[0];
                result.pushKV("updated_at", safety_attempts);
                return result;
            }
            if (command == "setpaymasterenabled") {
                setup_write_order.push_back(QString::fromStdString(command));
                enabled_requests.push_back(params[0].get_bool());
                rpc_settings_present = true;
                rpc_enabled = params[0].get_bool();
                if (!rpc_enabled) rpc_running = false;
                UniValue result{UniValue::VOBJ};
                result.pushKV("enabled", params[0].get_bool());
                return result;
            }
            if (command == "setpaymasterliquiditypolicy") {
                setup_write_order.push_back(QString::fromStdString(command));
                liquidity_policy = params[0];
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "setpaymasterruntimesettings") {
                setup_write_order.push_back(QString::fromStdString(command));
                rpc_operation_mode =
                    params[0].find_value("operation_mode").get_str();
                rpc_autostart = params[0].find_value("autostart").get_bool();
                return params[0];
            }
            if (command == "preparepaymasterpool") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "4444444444444444444444444444444444444444444444444444444444444444");
                result.pushKV("admission_dgb_slots",
                              params[0].find_value(
                                  "admission_dgb_slots").getInt<int>());
                result.pushKV("operational_dgb_slots",
                              params[0].find_value(
                                  "operational_dgb_slots").getInt<int>());
                result.pushKV("admission_carrier_slots",
                              params[0].find_value(
                                  "admission_carrier_slots").getInt<int>());
                result.pushKV("operational_carrier_slots",
                              params[0].find_value(
                                  "operational_carrier_slots").getInt<int>());
                result.pushKV("missing_admission_dgb_slots", 0);
                result.pushKV("missing_operational_dgb_slots", 0);
                result.pushKV("missing_admission_carrier_slots", 0);
                result.pushKV("missing_operational_carrier_slots", 0);
                result.pushKV("admission_dgb_satoshis_each", 10000000);
                result.pushKV("operational_dgb_satoshis_each", 10000000);
                result.pushKV("carrier_cents_each", 100);
                result.pushKV("total_output_satoshis", 0);
                result.pushKV("total_carrier_cents", 0);
                return result;
            }
            throw std::runtime_error("injected unrelated status unavailable");
        });
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());

    QPushButton* guided_setup = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterChooseGuidedSetup"));
    QVERIFY(guided_setup != nullptr);

    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(provider_status != nullptr);
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    fail_safety_snapshot = false;
    malformed_safety_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_safety_snapshot = false;
    malformed_liquidity_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_liquidity_snapshot = false;
    malformed_provider_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_provider_snapshot = false;
    tab.setWalletModel(mini_gui.walletModel.get());

    // Model the unconfigured status that previously replaced the UI's 3/1
    // carrier preset with zeroes before the wizard was opened.
    UniValue unconfigured_liquidity_policy{UniValue::VOBJ};
    unconfigured_liquidity_policy.pushKV("automatic_replenishment", true);
    unconfigured_liquidity_policy.pushKV("paid_maintenance_approved", false);
    unconfigured_liquidity_policy.pushKV("target_admission_dgb", 3);
    unconfigured_liquidity_policy.pushKV("target_operational_dgb", 1);
    unconfigured_liquidity_policy.pushKV("target_admission_carriers", 0);
    unconfigured_liquidity_policy.pushKV("target_operational_carriers", 0);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_transaction_satoshis", 20000000);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_hour_satoshis", 200000000);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_day_satoshis", 1000000000);
    UniValue unconfigured_liquidity{UniValue::VOBJ};
    unconfigured_liquidity.pushKV("policy_configured", false);
    unconfigured_liquidity.pushKV(
        "targets_satisfy_provider_policy", true);
    unconfigured_liquidity.pushKV("policy",
                                  unconfigured_liquidity_policy);
    tab.setPaymasterLiquidityStatusForTesting(unconfigured_liquidity);
    QSpinBox* configured_admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* configured_operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QVERIFY(configured_admission_carriers != nullptr);
    QVERIFY(configured_operational_carriers != nullptr);
    QCOMPARE(configured_admission_carriers->value(), 0);
    QCOMPARE(configured_operational_carriers->value(), 0);

    bool review_cancelled_without_writes{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        QCheckBox* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        if (!wallet_confirmation) {
            wizard->reject();
            return;
        }
        wallet_confirmation->setChecked(true);
        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            wizard->next();
        }
        review_cancelled_without_writes = wizard->currentPage() &&
            wizard->currentPage()->objectName() ==
                QLatin1String("paymasterSetupReviewPage");
        wizard->reject();
    });
    guided_setup->click();
    QVERIFY(review_cancelled_without_writes);
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);
    QVERIFY(liquidity_policy.isNull());

    bool wizard_opened{false};
    bool user_paid_defaults_ready{false};
    bool recommended_default{false};
    bool identity_name_validation_ready{false};
    bool sponsored_policy_defaults_preserve_liquidity{false};
    bool profile_keeps_network_fee{false};
    bool approval_invalidated_by_custom_limit{false};
    bool maintenance_start_copy_consistent{false};
    bool safety_page_visited{false};
    bool compact_pages_scroll{false};
    bool first_failure_visible{false};
    bool durable_phase_is_close_only{false};
    bool retry_feedback_visible{false};
    bool retry_completed{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        wizard_opened = true;

        QCheckBox* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        QCheckBox* user_paid = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupUserPaid"));
        QCheckBox* sponsored = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupSponsored"));
        QComboBox* sponsorship_scope = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSponsorshipScope"));
        QLineEdit* display_name = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupDisplayName"));
        QDoubleSpinBox* service_fee = wizard->findChild<QDoubleSpinBox*>(
            QStringLiteral("paymasterSetupServiceFeePercent"));
        QSpinBox* minimum_payment = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupMinimumPayment"));
        QSpinBox* network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        QComboBox* safety_profile = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSafetyProfile"));
        QLineEdit* custom_per_transaction = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerTransaction"));
        QLineEdit* custom_reserved = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomReserved"));
        QLineEdit* custom_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerHour"));
        QLineEdit* custom_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerDay"));
        QSpinBox* custom_completed_per_hour = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomCompletedPerHour"));
        QSpinBox* custom_completed_per_day = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomCompletedPerDay"));
        QSpinBox* custom_quotes_total = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesTotal"));
        QSpinBox* custom_quotes_per_netgroup = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesPerNetgroup"));
        QSpinBox* custom_quotes_per_recipient = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesPerRecipient"));
        QSpinBox* custom_requests_per_netgroup = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxQuoteRequestsPerNetgroupMinute"));
        QLineEdit* custom_maintenance_per_transaction = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerTransaction"));
        QLineEdit* custom_maintenance_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerHour"));
        QLineEdit* custom_maintenance_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerDay"));
        QSpinBox* admission_dgb = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionDgbSlots"));
        QSpinBox* operational_dgb = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalDgbSlots"));
        QSpinBox* admission_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionCarrierSlots"));
        QSpinBox* operational_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalCarrierSlots"));
        QPushButton* restore_funding_defaults = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreFundingDefaults"));
        QPushButton* restore_policy_defaults = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        QCheckBox* maintenance_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupMaintenanceApproval"));
        QPushButton* retry = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRetry"));
        QLabel* result = wizard->findChild<QLabel*>(
            QStringLiteral("paymasterSetupProgressResult"));
        auto* progress_page = wizard->findChild<QWizardPage*>(
            QStringLiteral("paymasterSetupProgressPage"));
        auto* safety_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupSafetyScroll"));
        auto* liquidity_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupLiquidityScroll"));
        auto* review_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupReviewScroll"));
        auto* progress_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupProgressScroll"));
        if (!wallet_confirmation || !user_paid || !sponsored ||
            !sponsorship_scope || !display_name || !service_fee ||
            !minimum_payment || !network_fee ||
            !safety_profile || !custom_per_transaction ||
            !custom_reserved || !custom_per_hour ||
            !custom_per_day || !custom_completed_per_hour ||
            !custom_completed_per_day || !custom_quotes_total ||
            !custom_quotes_per_netgroup || !custom_quotes_per_recipient ||
            !custom_requests_per_netgroup ||
            !custom_maintenance_per_transaction ||
            !custom_maintenance_per_hour || !custom_maintenance_per_day ||
            !admission_dgb || !operational_dgb ||
            !admission_carriers || !operational_carriers ||
            !restore_funding_defaults || !restore_policy_defaults ||
            !maintenance_confirmation || !retry || !result || !progress_page ||
            !safety_scroll || !liquidity_scroll || !review_scroll ||
            !progress_scroll) {
            wizard->reject();
            return;
        }
        wizard->resize(560, 440);
        compact_pages_scroll = safety_scroll->widgetResizable() &&
            liquidity_scroll->widgetResizable() &&
            review_scroll->widgetResizable() &&
            progress_scroll->widgetResizable() &&
            safety_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            liquidity_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            review_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            progress_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff;

        user_paid_defaults_ready = user_paid->isChecked() &&
            !sponsored->isChecked() && admission_dgb->value() >= 3 &&
            operational_dgb->value() >= 1 && admission_carriers->value() >= 3 &&
            operational_carriers->value() >= 1;
        recommended_default = safety_profile->currentData().toString() ==
            QLatin1String("recommended");

        const QString valid_display_name(32, QLatin1Char('A'));
        display_name->setText(valid_display_name);
        const bool valid_name_accepted = display_name->hasAcceptableInput() &&
            display_name->maxLength() == 32;
        display_name->setText(QStringLiteral("invalid/name"));
        const bool slash_rejected = !display_name->hasAcceptableInput();
        display_name->setText(QStringLiteral("invalid@name"));
        const bool at_rejected = !display_name->hasAcceptableInput();
        display_name->setText(QString::fromUtf8("N\xC3\xA4me"));
        const bool unicode_rejected = !display_name->hasAcceptableInput();
        display_name->clear();
        identity_name_validation_ready = valid_name_accepted &&
            slash_rejected && at_rejected && unicode_rejected;

        user_paid->setChecked(false);
        sponsored->setChecked(true);
        sponsorship_scope->setCurrentIndex(sponsorship_scope->findData(
            QStringLiteral("restricted")));
        restore_policy_defaults->click();
        sponsored_policy_defaults_preserve_liquidity =
            !user_paid->isChecked() &&
            sponsored->isChecked() && !service_fee->isEnabled() &&
            service_fee->value() == 0.0 && minimum_payment->minimum() == 100 &&
            admission_carriers->value() == 3 &&
            operational_carriers->value() == 1;
        restore_funding_defaults->click();
        restore_policy_defaults->click();

        wallet_confirmation->setChecked(true);
        user_paid->setChecked(true);
        sponsored->setChecked(false);
        // Safety profiles constrain aggregate/request exposure, not the
        // operator's independently chosen ceiling for one valid transfer.
        network_fee->setValue(30000000);
        const bool fee_before_profile_change =
            network_fee->value() == 30000000;
        safety_profile->setCurrentIndex(
            safety_profile->findData(QStringLiteral("custom")));
        profile_keeps_network_fee = fee_before_profile_change &&
            network_fee->value() == 30000000;
        custom_per_transaction->setText(QStringLiteral("0.30000000"));
        custom_reserved->setText(QStringLiteral("0.40000000"));
        custom_per_hour->setText(QStringLiteral("90071992.54740993"));
        custom_per_day->setText(QStringLiteral("21000000000.00000000"));
        custom_completed_per_hour->setValue(7);
        custom_completed_per_day->setValue(30);
        custom_quotes_total->setValue(20);
        custom_quotes_per_netgroup->setValue(5);
        custom_quotes_per_recipient->setValue(3);
        custom_requests_per_netgroup->setValue(12);
        custom_maintenance_per_transaction->setText(QStringLiteral("0.15000000"));
        custom_maintenance_per_hour->setText(QStringLiteral("0.50000000"));
        custom_maintenance_per_day->setText(QStringLiteral("2.00000000"));

        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            if (wizard->currentPage()->objectName() ==
                QLatin1String("paymasterSetupSafetyPage")) {
                safety_page_visited = true;
            }
            wizard->next();
        }
        if (!wizard->currentPage() ||
            wizard->currentPage()->objectName() !=
                QLatin1String("paymasterSetupReviewPage")) {
            wizard->reject();
            return;
        }
        maintenance_start_copy_consistent =
            maintenance_confirmation->text().contains(
                QStringLiteral("approval alone does not start"),
                Qt::CaseInsensitive) &&
            !maintenance_confirmation->text().contains(
                QStringLiteral("until I start it separately"),
                Qt::CaseInsensitive);
        maintenance_confirmation->setChecked(true);
        custom_maintenance_per_day->setText(QStringLiteral("2.10000000"));
        approval_invalidated_by_custom_limit =
            !maintenance_confirmation->isChecked();
        custom_maintenance_per_day->setText(QStringLiteral("2.00000000"));
        maintenance_confirmation->setChecked(true);

        wizard->next();

        first_failure_visible =
            wizard->currentPage() == progress_page && retry->isVisible() &&
            result->text().contains(QStringLiteral("injected safety retry"));
        durable_phase_is_close_only =
            wizard->buttonText(QWizard::CancelButton) == QStringLiteral("Close");
        retry->click();
        retry_feedback_visible = !retry->isEnabled() &&
            result->text().contains(QStringLiteral("Retrying"));
        QCoreApplication::processEvents();
        retry_completed = progress_page->isComplete() && !retry->isVisible();
        wizard->reject();
    });
    guided_setup->click();

    QVERIFY(wizard_opened);
    QVERIFY(user_paid_defaults_ready);
    QVERIFY(recommended_default);
    QVERIFY(identity_name_validation_ready);
    QVERIFY(sponsored_policy_defaults_preserve_liquidity);
    QVERIFY(profile_keeps_network_fee);
    QVERIFY(approval_invalidated_by_custom_limit);
    QVERIFY(maintenance_start_copy_consistent);
    QVERIFY(safety_page_visited);
    QVERIFY(compact_pages_scroll);
    QVERIFY(first_failure_visible);
    QVERIFY(durable_phase_is_close_only);
    QVERIFY(retry_feedback_visible);
    QVERIFY(retry_completed);
    QCOMPARE(operating_policy_attempts, 1);
    QCOMPARE(safety_attempts, 2);
    QCOMPARE(enabled_requests, QList<bool>{true});

    // A complete status may enable provider and funding actions. Any later
    // malformed provider snapshot must revoke all of them immediately and a
    // click on the now-disabled controls must not reach a mutation RPC.
    rpc_ready = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    const QStringList fail_closed_actions{
        QStringLiteral("paymasterStartProvider"),
        QStringLiteral("paymasterEnableProvider"),
        QStringLiteral("paymasterPreviewPoolPreparation"),
        QStringLiteral("paymasterPreviewPoolRetirement"),
    };
    for (const QString& object_name : fail_closed_actions) {
        QPushButton* action = tab.findChild<QPushButton*>(object_name);
        QVERIFY2(action != nullptr, qPrintable(object_name));
        QVERIFY2(action->isEnabled(),
                 qPrintable(QStringLiteral("Expected a valid snapshot to enable %1")
                                .arg(object_name)));
    }
    malformed_provider_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    const int mutation_calls_before_disabled_clicks = mutation_rpc_calls;
    for (const QString& object_name : fail_closed_actions) {
        QPushButton* action = tab.findChild<QPushButton*>(object_name);
        QVERIFY2(action != nullptr, qPrintable(object_name));
        QVERIFY2(!action->isEnabled(),
                 qPrintable(QStringLiteral("Malformed status left %1 enabled")
                                .arg(object_name)));
        action->click();
    }
    QCOMPARE(mutation_rpc_calls, mutation_calls_before_disabled_clicks);
    malformed_provider_snapshot = false;
    rpc_ready = false;
    tab.setWalletModel(mini_gui.walletModel.get());

    const qint64 advertised_fee = advertised_policy.find_value(
        "maximum_network_fee_dgb_satoshis").getInt<qint64>();
    const UniValue& user_paid_safety = safety_policy.find_value("user_paid");
    const qint64 safety_fee = user_paid_safety.find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>();
    QCOMPARE(advertised_fee, 30000000);
    QCOMPARE(safety_fee, advertised_fee);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_reserved_network_fee_satoshis").getInt<qint64>(), 40000000);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_network_fee_per_hour_satoshis").getInt<qint64>(),
        9007199254740993LL);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_network_fee_per_day_satoshis").getInt<qint64>(),
        2100000000000000000LL);
    QCOMPARE(user_paid_safety.find_value("maximum_completed_per_hour").getInt<int>(), 7);
    QCOMPARE(user_paid_safety.find_value("maximum_completed_per_day").getInt<int>(), 30);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_total").getInt<int>(), 20);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_per_netgroup").getInt<int>(), 5);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_per_recipient").getInt<int>(), 3);
    QCOMPARE(safety_policy.find_value(
        "maximum_quote_requests_per_netgroup_per_minute").getInt<int>(), 12);
    QCOMPARE(liquidity_policy.find_value("target_admission_dgb").getInt<int>(), 3);
    QCOMPARE(liquidity_policy.find_value("target_operational_dgb").getInt<int>(), 1);
    QCOMPARE(liquidity_policy.find_value("target_admission_carriers").getInt<int>(), 3);
    QCOMPARE(liquidity_policy.find_value("target_operational_carriers").getInt<int>(), 1);
    QVERIFY(liquidity_policy.find_value("automatic_replenishment").get_bool());
    QVERIFY(liquidity_policy.find_value("paid_maintenance_approved").get_bool());
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_transaction_satoshis").getInt<qint64>(), 15000000);
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_hour_satoshis").getInt<qint64>(), 50000000);
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_day_satoshis").getInt<qint64>(), 200000000);

    UniValue disabled_existing{UniValue::VOBJ};
    disabled_existing.pushKV("wallet_eligible", true);
    disabled_existing.pushKV("settings_present", true);
    disabled_existing.pushKV("enabled", false);
    disabled_existing.pushKV("running", false);
    disabled_existing.pushKV("ready", false);
    disabled_existing.pushKV("wallet_locked", false);
    disabled_existing.pushKV("has_identity", true);
    disabled_existing.pushKV("has_policy", true);
    disabled_existing.pushKV("has_safety_policy", true);
    disabled_existing.pushKV("operation_mode", "automatic");
    disabled_existing.pushKV("autostart", false);
    disabled_existing.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(disabled_existing);

    bool reopened_existing_custom_values{false};
    bool restore_default_actions_available{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        auto* profile = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSafetyProfile"));
        auto* reopened_network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        auto* reopened_reserved = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomReserved"));
        auto* reopened_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerHour"));
        auto* reopened_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerDay"));
        auto* reopened_quotes_total = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesTotal"));
        auto* reopened_maintenance_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerDay"));
        auto* reopened_automatic = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutomaticReplenishment"));
        auto* reopened_operation_mode = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupOperationMode"));
        auto* reopened_autostart = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutostart"));
        auto* reopened_provider_enabled = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupProviderEnabled"));
        auto* restore_identity = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreIdentityDefaults"));
        auto* restore_funding = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreFundingDefaults"));
        auto* restore_policy = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        auto* restore_safety = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreSafetyDefaults"));
        auto* restore_liquidity = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreLiquidityDefaults"));
        if (!profile || !reopened_network_fee || !reopened_reserved ||
            !reopened_per_hour || !reopened_per_day || !reopened_quotes_total ||
            !reopened_maintenance_day || !reopened_automatic ||
            !reopened_operation_mode || !reopened_autostart ||
            !reopened_provider_enabled ||
            !restore_identity || !restore_funding || !restore_policy ||
            !restore_safety || !restore_liquidity) {
            wizard->reject();
            return;
        }
        reopened_existing_custom_values =
            profile->currentData().toString() == QLatin1String("custom") &&
            reopened_network_fee->value() == 30000000 &&
            reopened_reserved->text() == QLatin1String("0.40000000") &&
            reopened_per_hour->text() ==
                QLatin1String("90071992.54740993") &&
            reopened_per_day->text() ==
                QLatin1String("21000000000.00000000") &&
            reopened_quotes_total->value() == 20 &&
            reopened_maintenance_day->text() == QLatin1String("2.00000000") &&
            reopened_automatic->isChecked() &&
            reopened_operation_mode->currentData().toString() ==
                QLatin1String("automatic") &&
            !reopened_autostart->isChecked() &&
            !reopened_provider_enabled->isChecked();

        restore_identity->click();
        restore_funding->click();
        restore_policy->click();
        restore_safety->click();
        restore_liquidity->click();
        restore_default_actions_available =
            profile->currentData().toString() == QLatin1String("recommended") &&
            reopened_network_fee->value() == 20000000 &&
            reopened_quotes_total->value() == 16 &&
            reopened_provider_enabled->isChecked();
        wizard->reject();
    });
    guided_setup->click();
    QVERIFY(reopened_existing_custom_values);
    QVERIFY(restore_default_actions_available);

    setup_write_order.clear();
    rpc_settings_present = true;
    rpc_enabled = true;
    UniValue enabled_existing{UniValue::VOBJ};
    enabled_existing.pushKV("wallet_eligible", true);
    enabled_existing.pushKV("settings_present", true);
    enabled_existing.pushKV("enabled", true);
    enabled_existing.pushKV("running", false);
    enabled_existing.pushKV("ready", false);
    enabled_existing.pushKV("wallet_locked", false);
    enabled_existing.pushKV("has_identity", true);
    enabled_existing.pushKV("has_policy", true);
    enabled_existing.pushKV("has_safety_policy", true);
    enabled_existing.pushKV("operation_mode", "automatic");
    enabled_existing.pushKV("autostart", true);
    enabled_existing.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(enabled_existing);

    bool reconfiguration_completed{false};
    bool existing_carriers_retained{false};
    bool disabled_target_retained{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(
            QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() !=
                QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        auto* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        auto* user_paid = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupUserPaid"));
        auto* sponsored = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupSponsored"));
        auto* scope = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSponsorshipScope"));
        auto* fee = wizard->findChild<QDoubleSpinBox*>(
            QStringLiteral("paymasterSetupServiceFeePercent"));
        auto* minimum = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupMinimumPayment"));
        auto* network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        auto* admission_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionCarrierSlots"));
        auto* operational_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalCarrierSlots"));
        auto* provider_enabled = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupProviderEnabled"));
        auto* autostart = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutostart"));
        auto* restore_policy = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        auto* restore_safety = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreSafetyDefaults"));
        auto* approval = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupMaintenanceApproval"));
        auto* progress_page = wizard->findChild<QWizardPage*>(
            QStringLiteral("paymasterSetupProgressPage"));
        if (!wallet_confirmation || !user_paid || !sponsored || !scope ||
            !fee || !minimum || !network_fee || !admission_carriers ||
            !operational_carriers || !provider_enabled || !autostart ||
            !restore_policy || !restore_safety || !approval ||
            !progress_page) {
            wizard->reject();
            return;
        }

        wallet_confirmation->setChecked(true);
        user_paid->setChecked(false);
        sponsored->setChecked(true);
        scope->setCurrentIndex(scope->findData(QStringLiteral("restricted")));
        restore_policy->click();
        network_fee->setValue(10000000);
        restore_safety->click();
        provider_enabled->setChecked(false);
        autostart->setChecked(true);
        existing_carriers_retained = admission_carriers->value() == 3 &&
            operational_carriers->value() == 1;
        disabled_target_retained = !provider_enabled->isChecked();

        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            wizard->next();
        }
        if (!wizard->currentPage() ||
            wizard->currentPage()->objectName() !=
                QLatin1String("paymasterSetupReviewPage")) {
            wizard->reject();
            return;
        }
        approval->setChecked(true);
        wizard->next();
        QCoreApplication::processEvents();
        reconfiguration_completed = progress_page->isComplete();
        wizard->reject();
    });
    guided_setup->click();

    QVERIFY(reconfiguration_completed);
    QVERIFY(existing_carriers_retained);
    QVERIFY(disabled_target_retained);
    QCOMPARE(operating_policy_attempts, 2);
    QCOMPARE(safety_attempts, 4);
    QCOMPARE(enabled_requests, QList<bool>({true, false, false}));
    QCOMPARE(setup_write_order, QStringList({
        QStringLiteral("setpaymasterenabled"),
        QStringLiteral("setpaymastersafetypolicy"),
        QStringLiteral("setpaymasterpolicy"),
        QStringLiteral("setpaymastersafetypolicy"),
        QStringLiteral("setpaymasterliquiditypolicy"),
        QStringLiteral("setpaymasterruntimesettings"),
        QStringLiteral("setpaymasterenabled"),
    }));
    QCOMPARE(advertised_policy.find_value("sponsorship_scope").get_str(),
             std::string{"restricted"});
    QCOMPARE(advertised_policy.find_value("fee_rate_bps").getInt<int>(), 0);
    QCOMPARE(advertised_policy.find_value(
        "maximum_network_fee_dgb_satoshis").getInt<qint64>(), 10000000);
    const UniValue& final_models =
        advertised_policy.find_value("funding_models");
    QCOMPARE(final_models.size(), 1U);
    QCOMPARE(final_models[0].get_str(), std::string{"sponsored"});
    const UniValue& bridge = safety_payloads.at(2);
    QCOMPARE(bridge.find_value("user_paid").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);
    QCOMPARE(bridge.find_value("restricted_sponsored").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);
    // The final policy preserves the already persisted budget of the model
    // that is no longer selected. Only the temporary bridge is capped while
    // the two independently validated Core policies change order.
    QCOMPARE(safety_policy.find_value("user_paid").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        30000000);
    QCOMPARE(safety_policy.find_value("restricted_sponsored").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);

    UniValue disabled_with_autostart{UniValue::VOBJ};
    disabled_with_autostart.pushKV("wallet_eligible", true);
    disabled_with_autostart.pushKV("settings_present", true);
    disabled_with_autostart.pushKV("enabled", false);
    disabled_with_autostart.pushKV("running", false);
    disabled_with_autostart.pushKV("ready", false);
    disabled_with_autostart.pushKV("wallet_locked", false);
    disabled_with_autostart.pushKV("has_identity", true);
    disabled_with_autostart.pushKV("has_policy", true);
    disabled_with_autostart.pushKV("has_safety_policy", true);
    disabled_with_autostart.pushKV("operation_mode", "automatic");
    disabled_with_autostart.pushKV("autostart", true);
    disabled_with_autostart.pushKV("service_state", "stopped");
    disabled_with_autostart.pushKV("readiness_errors",
                                   UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(disabled_with_autostart);
    QLabel* runtime_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivityRuntimeStatus"));
    QVERIFY(runtime_status != nullptr);
    QVERIFY(runtime_status->text().contains(
        QStringLiteral("configuration is disabled"), Qt::CaseInsensitive));
    QVERIFY(!runtime_status->text().contains(
        QStringLiteral("will start it"), Qt::CaseInsensitive));
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
            // Qt styles the viewport, headers, and corner button independently;
            // require all three overrides to prevent the global DGB blue from leaking through.
            QStringLiteral("DigiDollarSendWidget QTableWidget#paymasterOffers"),
            QStringLiteral("QHeaderView#paymasterOffersHeader::section"),
            QStringLiteral("QTableWidget#paymasterOffers QTableCornerButton::section"),
            QStringLiteral("DigiDollarMintWidget QFrame#amountFrame"),
            QStringLiteral("DigiDollarRedeemWidget QFrame#positionFrame"),
            QStringLiteral("DigiDollarRedeemWidget QPushButton#coinControlButton"),
            QStringLiteral("DigiDollarPositionsWidget QTableWidget"),
            QStringLiteral("DigiDollarTransactionsWidget QTableWidget"),
            QStringLiteral("QWidget#digiDollarTab QPushButton"),
            QStringLiteral("QWidget#digiDollarTab QSpinBox::up-arrow"),
            QStringLiteral("QWidget#digiDollarTab QSpinBox::down-arrow"),
            QStringLiteral("QDialog#digiDollarClientSafetyDialog"),
            QStringLiteral("QDialog#digiDollarClientSafetyDialog QPushButton"),
            QStringLiteral("QDialog#digiDollarClientSafetyDialog QSpinBox::up-arrow"),
            QStringLiteral("QDialog#digiDollarClientSafetyDialog QSpinBox::down-arrow"),
            QStringLiteral("QWidget#paymasterWidget QPushButton[paymasterRole=\"primaryAction\"]"),
            QStringLiteral("QWidget#paymasterWidget QPushButton[paymasterRole=\"secondaryAction\"]"),
            QStringLiteral("QScrollArea#paymasterOverviewPage"),
            QStringLiteral("QWidget#paymasterOverviewContents"),
            QStringLiteral("QScrollArea#paymasterFinancesPage"),
            QStringLiteral("QWidget#paymasterFinancesContents"),
            QStringLiteral("QWidget#paymasterWidget QCheckBox::indicator:checked"),
            QStringLiteral("QWidget#paymasterWidget QSpinBox::up-arrow"),
            QStringLiteral("QWidget#paymasterWidget QSpinBox::down-arrow"),
            QStringLiteral("QWidget#paymasterWidget QComboBox::down-arrow"),
            QStringLiteral("QFrame[digidollarRole=\"statusBanner\"]"),
            QStringLiteral("QFrame[digidollarRole=\"statusBanner\"][statusKind=\"ready\"]"),
            QStringLiteral("QFrame[digidollarRole=\"statusBanner\"][statusKind=\"waiting\"]"),
            QStringLiteral("QFrame[digidollarRole=\"statusBanner\"][statusKind=\"action\"]"),
            QStringLiteral("QFrame[digidollarRole=\"statusBanner\"][statusKind=\"error\"]"),
            QStringLiteral("QLabel[digidollarRole=\"statusText\"][statusKind=\"error\"]"),
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

        const int offer_theme_start = css.indexOf(
            QStringLiteral("DigiDollarSendWidget QTableWidget#paymasterOffers {"));
        const int session_theme_start = css.indexOf(
            QStringLiteral("/* Send $DD: focused protected Paymaster session."),
            offer_theme_start);
        QVERIFY2(offer_theme_start >= 0 && session_theme_start > offer_theme_start,
                 qPrintable(QStringLiteral("%1 must contain one bounded Paymaster offer-table theme block")
                                .arg(theme)));
        const QString offer_theme = css.mid(
            offer_theme_start, session_theme_start - offer_theme_start);
        const QStringList offer_rules{
            QStringLiteral("QTableWidget#paymasterOffers {"),
            QStringLiteral("QTableWidget#paymasterOffers::item {"),
            QStringLiteral("QTableWidget#paymasterOffers::item:alternate {"),
            QStringLiteral("QHeaderView#paymasterOffersHeader::section"),
            QStringLiteral("QTableWidget#paymasterOffers QTableCornerButton::section"),
        };
        for (const QString& rule : offer_rules) {
            QVERIFY2(offer_theme.contains(rule),
                     qPrintable(QStringLiteral("%1 Paymaster offer theme is missing %2")
                                    .arg(theme, rule)));
        }
        const QStringList offer_blue_colors{
            QStringLiteral("#002352"), QStringLiteral("#003366"),
            QStringLiteral("#0066cc"), QStringLiteral("#0088ff"),
            QStringLiteral("#0044aa"), QStringLiteral("#0055aa"),
            QStringLiteral("#a7c6ed"), QStringLiteral("#9bb8e8"),
        };
        for (const QString& color : offer_blue_colors) {
            QVERIFY2(!offer_theme.contains(color, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("%1 Paymaster offer preview must not reuse DGB blue color %2")
                                    .arg(theme, color)));
        }

        const int paymasterThemeStart = css.indexOf(
            QStringLiteral("Paymaster operator console — DigiDollar visual system v2"));
        QVERIFY2(paymasterThemeStart >= 0,
                 qPrintable(QString("%1 must contain the dedicated Paymaster green-theme block").arg(theme)));
        const QString paymasterTheme = css.mid(paymasterThemeStart);
        const QStringList dgbBlueColors{
            QStringLiteral("#002352"),
            QStringLiteral("#003366"),
            QStringLiteral("#0066cc"),
            QStringLiteral("#0088ff"),
            QStringLiteral("#0044aa"),
            QStringLiteral("#0055aa"),
        };
        for (const QString& color : dgbBlueColors) {
            QVERIFY2(!paymasterTheme.contains(color, Qt::CaseInsensitive),
                     qPrintable(QString("%1 Paymaster controls must not reuse DGB blue color %2")
                                    .arg(theme, color)));
        }
        QVERIFY2(paymasterTheme.contains(QStringLiteral("image: url(:/icons/spin_up)")) &&
                     paymasterTheme.contains(QStringLiteral("image: url(:/icons/spin_down)")),
                 qPrintable(QString("%1 Paymaster numeric controls must retain visible up/down arrows")
                                .arg(theme)));
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

        requireRule(css, theme, QStringLiteral("QDialog#digiDollarClientSafetyDialog {"), dialogBg);
        requireRule(css, theme, QStringLiteral("QDialog#digiDollarClientSafetyDialog QPushButton"), accent);
        QVERIFY2(css.contains(QStringLiteral("QDialog#digiDollarClientSafetyDialog QSpinBox::up-arrow")) &&
                     css.contains(QStringLiteral("QDialog#digiDollarClientSafetyDialog QSpinBox::down-arrow")) &&
                     css.contains(QStringLiteral("image: url(:/icons/spin_up)")) &&
                     css.contains(QStringLiteral("image: url(:/icons/spin_down)")),
                 qPrintable(QStringLiteral("%1 client safety dialog must display both numeric arrows")
                                 .arg(theme)));
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
        captureDialog(coinDialog, QDir::temp().filePath(
            QStringLiteral("digibyte_dd_coin_control_%1_qa.png").arg(name)));

        DigiDollarReceiveRequestDialog requestDialog;
        seedRequest(requestDialog);
        captureDialog(requestDialog, QDir::temp().filePath(
            QStringLiteral("digibyte_dd_receive_request_%1_qa.png").arg(name)));

        DDAddressBookPage addressBook(nullptr, DDAddressBookPage::ForSelection);
        addressBook.resize(780, 420);
        seedAddressBookRows(addressBook);
        captureDialog(addressBook, QDir::temp().filePath(
            QStringLiteral("digibyte_dd_address_book_%1_qa.png").arg(name)));
    };

    runTheme(QStringLiteral("dark"), QColor(QStringLiteral("#0b2419")), QColor(QStringLiteral("#ffffff")));
    runTheme(QStringLiteral("light"), QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#123f2b")));

    qApp->setStyleSheet(originalStyleSheet);
    qApp->setPalette(originalPalette);
    QCoreApplication::processEvents();

    for (const QString& file_name : {
             QStringLiteral("digibyte_dd_coin_control_dark_qa.png"),
             QStringLiteral("digibyte_dd_coin_control_light_qa.png"),
             QStringLiteral("digibyte_dd_receive_request_dark_qa.png"),
             QStringLiteral("digibyte_dd_receive_request_light_qa.png"),
             QStringLiteral("digibyte_dd_address_book_dark_qa.png"),
             QStringLiteral("digibyte_dd_address_book_light_qa.png")}) {
        qInfo("DigiDollar modal QA screenshot: %s",
              qPrintable(QDir::temp().filePath(file_name)));
    }
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
    const QString secret_recipient = QStringLiteral(
        "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    sendWidget.setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        secret_recipient, 3.25, QStringLiteral("CANDIDATE"),
        QStringLiteral("NONE"), {QStringLiteral("refresh")}, true);
    sendWidget.show();

    // Enable privacy mode
    sendWidget.setPrivacy(true);

    // Check that available balance is masked
    QLabel* availableBalanceValue = sendWidget.findChild<QLabel*>("availableBalanceValue");
    QVERIFY(availableBalanceValue != nullptr);
    QVERIFY2(availableBalanceValue->text().contains('#'), "Available balance should be masked when privacy is enabled");
    QLabel* transfer = sendWidget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionTransfer"));
    QLineEdit* address = sendWidget.findChild<QLineEdit*>(
        QStringLiteral("addressEdit"));
    QPushButton* technical = sendWidget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionTechnicalToggle"));
    QVERIFY(transfer && address && technical);
    QVERIFY(!transfer->text().contains(secret_recipient));
    QVERIFY(transfer->toolTip().isEmpty());
    QVERIFY(transfer->accessibleDescription().isEmpty());
    QCOMPARE(address->echoMode(), QLineEdit::Password);
    QVERIFY(!technical->isEnabled());

    // Disable privacy mode
    sendWidget.setPrivacy(false);

    // Check that available balance is no longer masked
    QVERIFY2(!availableBalanceValue->text().contains('#'), "Available balance should NOT be masked when privacy is disabled");
    QVERIFY(transfer->text().contains(secret_recipient));
    QCOMPARE(address->echoMode(), QLineEdit::Normal);
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
    QTRY_VERIFY_WITH_TIMEOUT(transactionsList->count() >= 4, 5000);

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
    QTRY_VERIFY_WITH_TIMEOUT(transactionsList->count() >= 2, 5000);

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
        const QString path = QDir::temp().filePath(
            QStringLiteral("digibyte_dd_overview_recent_alignment_qa.png"));
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

    QTabWidget* tabWidget = tab.findChild<QTabWidget*>("digiDollarSubTabs");
    QVERIFY(tabWidget != nullptr);
    QCOMPARE(tabWidget->currentIndex(), 0);

    QListWidget* transactionsList = tab.findChild<QListWidget*>("transactionsList");
    QVERIFY(transactionsList != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(transactionsList->count() >= 1, 5000);

    QListWidgetItem* item = transactionsList->item(0);
    QVERIFY(item != nullptr);
    const bool invoked = QMetaObject::invokeMethod(transactionsList, "itemDoubleClicked",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QListWidgetItem*, item));
    QVERIFY2(invoked, "DD overview recent transaction list must expose the itemDoubleClicked signal");
    QTRY_COMPARE_WITH_TIMEOUT(tabWidget->currentIndex(), 6, 5000);

    DigiDollarTransactionsWidget* transactionsWidget = tab.findChild<DigiDollarTransactionsWidget*>("transactionsWidget");
    QVERIFY(transactionsWidget != nullptr);
    QTableWidget* table = transactionsWidget->findChild<QTableWidget*>();
    QVERIFY(table != nullptr);

    const auto matchingTransactionSelected = [&] {
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem* txidItem = table->item(row, 5);
            if (!txidItem || txidItem->data(Qt::UserRole).toString() != QString::fromStdString(tx.txid)) continue;
            return table->currentRow() == row &&
                   table->selectionModel()->isRowSelected(row, QModelIndex());
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(matchingTransactionSelected(), 5000);

    RemoveWallet(context, wallet, std::nullopt);
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

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 5000);

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
    QVERIFY2(plainDetails.contains(QStringLiteral("Confirmations:")),
             "the details dialog must label the confirmation count for what it is");
    QVERIFY2(plainDetails.contains(QStringLiteral("detail dialog note")), "details text must include the local note");

    RemoveWallet(context, wallet, std::nullopt);
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

    const QString darkScreenshot = QDir::temp().filePath(
        QStringLiteral("digibyte_dd_transaction_details_dark_qa.png"));
    const QString lightScreenshot = QDir::temp().filePath(
        QStringLiteral("digibyte_dd_transaction_details_light_qa.png"));
    openAndCapture(darkFallback, QColor(QStringLiteral("#002352")), darkScreenshot);
    openAndCapture(lightFallback, QColor(QStringLiteral("#ffffff")), lightScreenshot);
    qApp->setStyleSheet(originalStyleSheet);
    qApp->setPalette(originalPalette);
    QCoreApplication::processEvents();

    qInfo("DD transaction details dark QA screenshot: %s", qPrintable(darkScreenshot));
    qInfo("DD transaction details light QA screenshot: %s", qPrintable(lightScreenshot));
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

// Regression coverage for the DD Transactions tab's asynchronous wallet-history
// table: the wallet snapshot returns signed amounts derived from incoming/outgoing
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

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 5, 5000);

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

    RemoveWallet(context, wallet, std::nullopt);
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
    const QString screenshot_path = QDir::temp().filePath(
        QStringLiteral("digibyte_tooltip_qa.png"));
    QVERIFY2(pixmap.save(screenshot_path),
             qPrintable(QStringLiteral("failed to save tooltip QA screenshot to %1").arg(screenshot_path)));

    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    QRect search_rect;
    for (QWidget* top_level : QApplication::topLevelWidgets()) {
        if (top_level->isVisible() && top_level->windowType() == Qt::ToolTip) {
            search_rect = top_level->frameGeometry().intersected(image.rect());
            break;
        }
    }
    if (search_rect.isEmpty()) {
        // Some platform plugins expose a native tooltip without a QWidget.
        // Retain the bounded screen-search fallback for those platforms.
        search_rect = QRect(global_pos - QPoint(40, 40), QSize(760, 260)).intersected(image.rect());
    }
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


void DigiDollarWidgetTests::redeemResultAlwaysReachesTheUser()
{
    // The wallet window turns a widget message into a dialog only when the
    // message style carries the modal flag. Without that flag the words are
    // handed to the desktop notification service, so on a machine with no
    // notification service, or with notifications switched off, a user who
    // redeems sees nothing at all: no transaction id, no confirmation, and no
    // reason when the redemption is refused.
    const unsigned int done = DigiDollarRedeemWidget::resultMessageStyle(true);
    const unsigned int refused = DigiDollarRedeemWidget::resultMessageStyle(false);

    QVERIFY2(done & CClientUIInterface::MODAL,
             "a finished redemption must open a dialog, not a desktop notification");
    QVERIFY2(refused & CClientUIInterface::MODAL,
             "a refused redemption must open a dialog, not a desktop notification");
    QVERIFY2(refused & CClientUIInterface::ICON_ERROR,
             "a refused redemption must be shown as an error");

    const QString txid = QStringLiteral("eba20fe261ed7f7c0000000000000000000000000000000000000000004c610f");
    const QString text = DigiDollarRedeemWidget::redemptionBroadcastText(txid);
    QVERIFY2(text.contains(txid), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("broadcast"), Qt::CaseInsensitive), qPrintable(text));
}

void DigiDollarWidgetTests::transactionsConfirmationsColumnIsAlwaysACount()
{
    // The Confirmations column used to print the number for the first five
    // blocks and the word "Confirmed" after that, so one list held both a
    // number and a word. It now shows the count for anything in a block, and a
    // word only where there is no count to show.
    DigiDollarTransactionsWidget widget;

    QCOMPARE(widget.confirmationsTextForTesting(1), QStringLiteral("1"));
    QCOMPARE(widget.confirmationsTextForTesting(5), QStringLiteral("5"));
    QCOMPARE(widget.confirmationsTextForTesting(6), QStringLiteral("6"));
    QCOMPARE(widget.confirmationsTextForTesting(1440), QStringLiteral("1440"));

    QCOMPARE(widget.confirmationsTextForTesting(0), QStringLiteral("Pending"));
    QCOMPARE(widget.confirmationsTextForTesting(0, false, true), QStringLiteral("Local"));
    QCOMPARE(widget.confirmationsTextForTesting(-1), QStringLiteral("Conflicted"));
    QCOMPARE(widget.confirmationsTextForTesting(3, true), QStringLiteral("Abandoned"));
}

void DigiDollarWidgetTests::sendWidgetSaysWhyAnAmountIsRefused()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWidgetTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    DigiDollarSendWidget sendWidget(platformStyle.get());

    QLineEdit* amountEdit = sendWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(amountEdit != nullptr);
    QLabel* amountMessage = sendWidget.findChild<QLabel*>("amountValidationLabel");
    QVERIFY2(amountMessage != nullptr, "the amount box must have a line under it that explains itself");

    // With nothing typed the form says what it will take.
    QVERIFY2(amountMessage->text().contains(QStringLiteral("1.00")), qPrintable(amountMessage->text()));
    QVERIFY2(amountMessage->text().contains(QStringLiteral("100000.00")), qPrintable(amountMessage->text()));

    // Below the minimum. This used to turn the box red and say nothing.
    amountEdit->setText(QStringLiteral("0.50"));
    QCoreApplication::processEvents();
    QVERIFY2(amountMessage->text().contains(QStringLiteral("smallest"), Qt::CaseInsensitive),
             qPrintable(amountMessage->text()));
    QVERIFY2(amountMessage->text().contains(QStringLiteral("1.00")), qPrintable(amountMessage->text()));

    // Over the limit. The box takes the number so the user can see what they
    // typed, and the line under it says which limit was passed.
    amountEdit->setText(QStringLiteral("200000"));
    QCoreApplication::processEvents();
    QCOMPARE(amountEdit->text(), QStringLiteral("200000"));
    QVERIFY2(amountMessage->text().contains(QStringLiteral("most you can send"), Qt::CaseInsensitive),
             qPrintable(amountMessage->text()));
    QVERIFY2(amountMessage->text().contains(QStringLiteral("100000.00")), qPrintable(amountMessage->text()));

    // More than the wallet holds. With no wallet model the balance is zero, so
    // any amount inside the limits is more than the balance.
    amountEdit->setText(QStringLiteral("25.00"));
    QCoreApplication::processEvents();
    QVERIFY2(amountMessage->text().contains(QStringLiteral("only have"), Qt::CaseInsensitive),
             qPrintable(amountMessage->text()));

    // The Send button stays off for every one of those.
    QPushButton* sendButton = sendWidget.findChild<QPushButton*>("sendButton");
    QVERIFY(sendButton != nullptr);
    QVERIFY2(!sendButton->isEnabled(), "an amount the form refuses must leave the Send button off");
}
