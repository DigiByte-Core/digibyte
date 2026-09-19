// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollartab.h>
#include <qt/paymasterwidget.h>
#include <qt/optionsmodel.h>
#include <QScrollArea>
#include <QApplication>

#include <qt/digidollaroverviewwidget.h>
#include <qt/digidollarreceivewidget.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarmintwidget.h>
#include <qt/digidollarredeemwidget.h>
#include <qt/digidollarpositionswidget.h>
#include <qt/digidollartransactionswidget.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/platformstyle.h>

#include <QTabWidget>
#include <QTabBar>
#include <QVBoxLayout>
#include <QTimer>
#include <QLabel>
#include <QStackedWidget>
#include <QFont>

#include <chainparams.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <validation.h>
#include <versionbits.h>

DigiDollarTab::DigiDollarTab(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    m_tabWidget(nullptr),
    m_mainLayout(nullptr),
    m_overviewWidget(nullptr),
    m_receiveWidget(nullptr),
    m_sendWidget(nullptr),
    m_mintWidget(nullptr),
    m_redeemWidget(nullptr),
    m_positionsWidget(nullptr),
    m_transactionsWidget(nullptr),
    m_paymasterWidget(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr),
    m_platformStyle(platformStyle),
    m_stackedWidget(nullptr),
    m_activationLabel(nullptr),
    m_activationTimer(nullptr),
    m_activated(false)
{
    setupUI();
    connectSignals();
}

DigiDollarTab::~DigiDollarTab()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarTab::setupUI()
{
    setObjectName("digiDollarTab");

    // Create main layout
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Create tab widget
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setObjectName("digiDollarSubTabs");
    m_tabWidget->tabBar()->setElideMode(Qt::ElideNone);      // Don't truncate tab text
    m_tabWidget->tabBar()->setExpanding(true);               // Expand tabs to fill width
    m_tabWidget->tabBar()->setUsesScrollButtons(true);       // Use scroll if needed

    // Create sub-widgets
    m_overviewWidget = new DigiDollarOverviewWidget(this);
    m_overviewWidget->setObjectName("overviewWidget");

    m_receiveWidget = new DigiDollarReceiveWidget();
    m_receiveWidget->setObjectName("receiveWidget");

    auto* send_scroll = new QScrollArea(m_tabWidget);
    send_scroll->setObjectName("digiDollarSendPage");
    send_scroll->setWidgetResizable(true);
    m_sendWidget = new DigiDollarSendWidget(m_platformStyle, send_scroll);
    m_sendWidget->setObjectName("sendWidget");
    send_scroll->setWidget(m_sendWidget);
    ConfigurePaymasterScrollArea(send_scroll, m_sendWidget);

    m_mintWidget = new DigiDollarMintWidget(this);
    m_mintWidget->setObjectName("mintWidget");

    m_redeemWidget = new DigiDollarRedeemWidget(this);
    m_redeemWidget->setObjectName("redeemWidget");

    m_positionsWidget = new DigiDollarPositionsWidget(this);
    m_positionsWidget->setObjectName("positionsWidget");

    m_transactionsWidget = new DigiDollarTransactionsWidget(this);
    m_transactionsWidget->setObjectName("transactionsWidget");

    m_paymasterWidget = CreatePaymasterWidget(this);
    m_paymasterWidget->setObjectName("paymasterWidget");
    m_paymasterWidget->setProviderBackupRequestHandler([this] {
        Q_EMIT providerWalletBackupRequested();
    });

    // Add tabs in order: $DD Overview, Send $DD, Receive $DD, Mint $DD, Redeem $DD, $DD Vault, $DD Transactions
    m_tabWidget->addTab(m_overviewWidget, tr("$DD Overview"));
    m_tabWidget->addTab(send_scroll, tr("Send $DD"));
    m_tabWidget->addTab(m_receiveWidget, tr("Receive $DD"));
    m_tabWidget->addTab(m_mintWidget, tr("Mint $DD"));
    m_tabWidget->addTab(m_redeemWidget, tr("Redeem $DD"));
    m_tabWidget->addTab(m_positionsWidget, tr("$DD Vault"));
    m_tabWidget->addTab(m_transactionsWidget, tr("$DD Transactions"));

    // Create activation status overlay
    m_activationLabel = new QLabel(this);
    m_activationLabel->setAlignment(Qt::AlignCenter);
    m_activationLabel->setWordWrap(true);
    m_activationLabel->setTextFormat(Qt::RichText);
    QFont labelFont = m_activationLabel->font();
    labelFont.setPointSize(14);
    m_activationLabel->setFont(labelFont);
    m_activationLabel->setStyleSheet("QLabel { color: #CCCCCC; padding: 40px; }");

    // Use stacked widget to switch between activation message and DD tabs
    m_stackedWidget = new QStackedWidget(this);
    m_stackedWidget->setObjectName("digiDollarStack");
    m_stackedWidget->addWidget(m_activationLabel);  // index 0: activation message
    m_stackedWidget->addWidget(m_tabWidget);         // index 1: DD functionality

    // Add stacked widget to main layout
    m_mainLayout->addWidget(m_stackedWidget);

    setLayout(m_mainLayout);

    // Start activation check timer (every 5 seconds)
    m_activationTimer = new QTimer(this);
    connect(m_activationTimer, &QTimer::timeout, this, &DigiDollarTab::checkActivationStatus);
    m_activationTimer->start(5000);

    // Check immediately
    checkActivationStatus();
}

void DigiDollarTab::connectSignals()
{
    // Connect tab change signal
    connect(m_tabWidget, &QTabWidget::currentChanged,
            this, &DigiDollarTab::onTabChanged);

    // Connect sub-widget signals
    if (m_overviewWidget) {
        connect(m_overviewWidget, &DigiDollarOverviewWidget::message,
                this, &DigiDollarTab::message);
        connect(m_overviewWidget, &DigiDollarOverviewWidget::recentTransactionActivated,
                this, &DigiDollarTab::showTransaction);
    }

    if (m_receiveWidget) {
        connect(m_receiveWidget, &DigiDollarReceiveWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_sendWidget) {
        connect(m_sendWidget, &DigiDollarSendWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_mintWidget) {
        connect(m_mintWidget, &DigiDollarMintWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_redeemWidget) {
        connect(m_redeemWidget, &DigiDollarRedeemWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_positionsWidget) {
        connect(m_positionsWidget, &DigiDollarPositionsWidget::message,
                this, &DigiDollarTab::message);
        connect(m_positionsWidget, &DigiDollarPositionsWidget::redeemRequested,
                this, &DigiDollarTab::onRedeemRequested);
    }

    if (m_transactionsWidget) {
        connect(m_transactionsWidget, &DigiDollarTransactionsWidget::message,
                this, &DigiDollarTab::message);
    }
}

void DigiDollarTab::setWalletModel(WalletModel* model)
{
    if (m_walletModel && m_walletModel != model) {
        disconnect(m_walletModel, &WalletModel::balanceChanged,
                   this, &DigiDollarTab::updateBalance);
    }
    m_walletModel = model;

    // Pass wallet model to sub-widgets
    if (m_overviewWidget)
        m_overviewWidget->setWalletModel(model);
    if (m_receiveWidget)
        m_receiveWidget->setWalletModel(model);
    if (m_sendWidget)
        m_sendWidget->setWalletModel(model);
    if (m_mintWidget)
        m_mintWidget->setWalletModel(model);
    if (m_redeemWidget)
        m_redeemWidget->setWalletModel(model);
    if (m_positionsWidget)
        m_positionsWidget->setWalletModel(model);
    if (m_transactionsWidget)
        m_transactionsWidget->setWalletModel(model);
    if (m_paymasterWidget)
        m_paymasterWidget->setWalletModel(model);

    // Keep the tab's cached DGB/DD balance displays live while the user stays
    // on DigiDollar. The signal's WalletBalances payload is intentionally
    // discarded, and the slot re-reads each child widget's current balance.
    if (m_walletModel) {
        connect(m_walletModel, &WalletModel::balanceChanged,
                this, &DigiDollarTab::updateBalance,
                Qt::UniqueConnection);
    }

    // Update view when wallet model changes
    updateView();
}

void DigiDollarTab::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    // Pass client model to sub-widgets
    if (m_overviewWidget)
        m_overviewWidget->setClientModel(model);
    if (m_receiveWidget)
        m_receiveWidget->setClientModel(model);
    if (m_sendWidget)
        m_sendWidget->setClientModel(model);
    if (m_mintWidget)
        m_mintWidget->setClientModel(model);
    if (m_redeemWidget)
        m_redeemWidget->setClientModel(model);
    if (m_positionsWidget)
        m_positionsWidget->setClientModel(model);
    if (m_transactionsWidget)
        m_transactionsWidget->setClientModel(model);

    // The constructor cannot determine activation before ClientModel exists.
    // Waiting for the five-second activation timer left a newly opened wallet
    // showing placeholder or empty DD pages even when activation was already
    // complete. Evaluate it immediately once the node interface is available.
    checkActivationStatus();
}

void DigiDollarTab::updateView()
{
    // Some child refreshes need wallet or RPC locks. Refreshing every hidden
    // page made navigation wait behind unrelated history work. Queue the
    // selected page instead: this also guarantees that its own isVisible()
    // guard observes the final QStackedWidget/QTabWidget state.
    scheduleCurrentPageRefresh();
}

void DigiDollarTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    scheduleCurrentPageRefresh();
}

void DigiDollarTab::scheduleCurrentPageRefresh()
{
    if (m_currentPageRefreshScheduled || !isVisible()) return;

    m_currentPageRefreshScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_currentPageRefreshScheduled = false;
        if (!isVisible() || !m_stackedWidget ||
            m_stackedWidget->currentWidget() != m_tabWidget) {
            return;
        }
        refreshCurrentPage();
    });
}

void DigiDollarTab::refreshCurrentPage()
{
    if (!m_tabWidget) return;

    switch (m_tabWidget->currentIndex()) {
    case 0: // Overview
        if (m_overviewWidget) m_overviewWidget->updateView();
        break;
    case 1: // Send
        if (m_sendWidget) m_sendWidget->updateView();
        break;
    case 2: // Receive
        if (m_receiveWidget) m_receiveWidget->updateView();
        break;
    case 3: // Mint
        if (m_mintWidget) m_mintWidget->updateView();
        break;
    case 4: // Redeem
        if (m_redeemWidget) m_redeemWidget->updateView();
        break;
    case 5: // Vault
        if (m_positionsWidget) m_positionsWidget->updateView();
        break;
    case 6: // Transactions
        if (m_transactionsWidget) m_transactionsWidget->updateView();
        break;
    case 7: // Paymaster Network
        if (m_paymasterWidget) m_paymasterWidget->refreshStatus();
        break;
    default:
        break;
    }
}

void DigiDollarTab::setPaymasterLiquidityStatusForTesting(
    const UniValue& status)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setLiquidityStatusForTesting(status);
    }
}

void DigiDollarTab::setPaymasterLiquidityPoolForTesting(
    const UniValue& pool_info)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setLiquidityPoolForTesting(pool_info);
    }
}

void DigiDollarTab::setPaymasterReadinessStatusForTesting(
    const UniValue& status)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setReadinessStatusForTesting(status);
    }
}

void DigiDollarTab::setPaymasterMutationSnapshotsAvailableForTesting(
    bool provider_info, bool provider_safety, bool liquidity)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setMutationSnapshotsAvailableForTesting(
            provider_info, provider_safety, liquidity);
    }
}

void DigiDollarTab::setPaymasterStartResultForTesting(
    const UniValue& result)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setStartResultForTesting(result);
    }
}

void DigiDollarTab::setPaymasterRpcExecutorForTesting(
    PaymasterRpcExecutorForTesting executor)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setRpcExecutorForTesting(std::move(executor));
    }
}

void DigiDollarTab::setPaymasterFinanceExportFilenameForTesting(
    const QString& filename)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setFinanceExportFilenameForTesting(filename);
    }
}

void DigiDollarTab::incomingDDTransaction(const QString& date, const QString& amount,
                                         const QString& type, const QString& address)
{
    // Notify overview widget of incoming transaction
    if (m_overviewWidget) {
        m_overviewWidget->incomingDDTransaction(date, amount, type, address);
    }

    // Update balance displays
    updateBalance();
}

void DigiDollarTab::updateBalance()
{
    if (!isVisible() || !m_tabWidget) return;

    // The Mint balance comes from WalletModel's already cached DGB balance and
    // is safe to keep current while hidden. DD balance queries may acquire
    // cs_wallet, so perform those only for the page the user can see.
    if (m_mintWidget) m_mintWidget->updateBalance();
    switch (m_tabWidget->currentIndex()) {
    case 0:
        if (m_overviewWidget) m_overviewWidget->updateBalance();
        break;
    case 1:
        if (m_sendWidget) m_sendWidget->updateBalance();
        break;
    case 4:
        if (m_redeemWidget) m_redeemWidget->updateBalance();
        break;
    default:
        break;
    }
}

void DigiDollarTab::updateOraclePrice()
{
    if (!isVisible() || !m_tabWidget) return;
    switch (m_tabWidget->currentIndex()) {
    case 0:
        if (m_overviewWidget) m_overviewWidget->updateOraclePrice();
        break;
    case 1:
        if (m_sendWidget) m_sendWidget->updateOraclePrice();
        break;
    case 3:
        if (m_mintWidget) m_mintWidget->updateOraclePrice();
        break;
    default:
        break;
    }
}

void DigiDollarTab::updateSystemHealth()
{
    if (isVisible() && m_tabWidget && m_tabWidget->currentIndex() == 0 && m_overviewWidget) {
        m_overviewWidget->updateSystemHealth();
    }
}

void DigiDollarTab::updatePositions()
{
    if (!isVisible() || !m_tabWidget) return;
    if (m_tabWidget->currentIndex() == 5 && m_positionsWidget) {
        m_positionsWidget->updatePositions();
    } else if (m_tabWidget->currentIndex() == 4 && m_redeemWidget) {
        m_redeemWidget->updatePositions();
    }
}

void DigiDollarTab::onTabChanged(int index)
{
    Q_UNUSED(index);
    // currentChanged is emitted before the newly selected child necessarily
    // reports isVisible(). A queued refresh avoids losing the request and
    // prevents the five-second child timers from becoming the accidental
    // first-load mechanism.
    scheduleCurrentPageRefresh();
}

void DigiDollarTab::setPrivacy(bool privacy)
{
    m_privacy = privacy;

    // Relay privacy setting to all sub-widgets
    if (m_overviewWidget)
        m_overviewWidget->setPrivacy(privacy);
    if (m_sendWidget)
        m_sendWidget->setPrivacy(privacy);
    if (m_mintWidget)
        m_mintWidget->setPrivacy(privacy);
    if (m_redeemWidget)
        m_redeemWidget->setPrivacy(privacy);
    if (m_positionsWidget)
        m_positionsWidget->setPrivacy(privacy);
    if (m_transactionsWidget)
        m_transactionsWidget->setPrivacy(privacy);
    if (m_paymasterWidget)
        m_paymasterWidget->setPrivacy(privacy);
}

void DigiDollarTab::setPaymasterOperatorVisible(bool visible)
{
    const int index = m_tabWidget->indexOf(m_paymasterWidget);
    if (visible && index < 0) {
        m_tabWidget->addTab(m_paymasterWidget, tr("Paymaster Network"));
    } else if (!visible && index >= 0) {
        m_tabWidget->removeTab(index);
    }
}

void DigiDollarTab::onRedeemRequested(const QString &positionId)
{
    // Switch to Redeem tab (index 4) and populate the position ID
    if (m_redeemWidget) {
        m_redeemWidget->setPosition(positionId);
        m_tabWidget->setCurrentIndex(4); // Redeem tab
    }
}

void DigiDollarTab::showTransaction(const QString& txid)
{
    if (!m_tabWidget || !m_transactionsWidget || txid.isEmpty()) {
        return;
    }

    if (m_stackedWidget) {
        m_stackedWidget->setCurrentWidget(m_tabWidget);
    }
    m_tabWidget->setCurrentWidget(m_transactionsWidget);
    m_transactionsWidget->updateView();
    m_transactionsWidget->focusTransaction(txid);
}

void DigiDollarTab::checkActivationStatus()
{
    if (m_activated) {
        // Already activated, stop checking
        if (m_activationTimer) m_activationTimer->stop();
        return;
    }

    QString status = getDeploymentStatus();
    bool active = isDigiDollarActive();

    if (active) {
        m_activated = true;
        m_stackedWidget->setCurrentIndex(1); // Show DD tabs
        if (m_activationTimer) m_activationTimer->stop();
        // The tab page becomes visible only after the stack transition. Queue
        // its complete initial load instead of issuing child updates too early.
        scheduleCurrentPageRefresh();
        return;
    }

    // Build activation status message
    QString msg = QString(
        "<div style='text-align: center;'>"
        "<h2 style='color: #0066CC;'>💎 DigiDollar</h2>"
        "<p style='font-size: 16px; margin: 20px 0;'>"
        "DigiDollar is not yet active on this blockchain.</p>"
        "<p style='font-size: 13px; color: #999999;'>"
        "Deployment Status: <b>%1</b></p>"
        "<p style='font-size: 12px; color: #777777; margin-top: 20px;'>"
        "DigiDollar activates at a fixed block height on this network.<br>"
        "Use <code>getdigidollardeploymentinfo</code> for details.</p>"
        "</div>"
    ).arg(status.toUpper());

    m_activationLabel->setText(msg);
    m_stackedWidget->setCurrentIndex(0); // Show activation message
}

QString DigiDollarTab::getDeploymentStatus() const
{
    if (!m_clientModel) return "unknown";

    try {
        interfaces::Node& node = m_clientModel->node();
        node::NodeContext* ctx = node.context();
        if (!ctx || !ctx->chainman) return "unknown";

        ChainstateManager& chainman = *ctx->chainman;
        // DigiDollar is a buried deployment (BIP90): status is a pure height
        // comparison against the hardcoded activation height.
        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
        return DigiDollar::IsDigiDollarEnabled(tip, chainman) ? "active" : "defined";
    } catch (...) {
        return "unknown";
    }
}

bool DigiDollarTab::isDigiDollarActive() const
{
    if (!m_clientModel) return false;

    try {
        interfaces::Node& node = m_clientModel->node();
        // Use the node's context to check actual BIP9 status
        node::NodeContext* ctx = node.context();
        if (!ctx || !ctx->chainman) return false;

        ChainstateManager& chainman = *ctx->chainman;
        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
        if (!tip) return false;

        return DigiDollar::IsDigiDollarEnabled(tip, chainman);
    } catch (...) {
        return false;
    }
}
