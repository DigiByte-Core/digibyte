// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollaroverviewwidget.h>

#include <qt/digidollarstatus.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/digibyteunits.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactiontablemodel.h>
#include <oracle/mock_oracle.h>
#include <consensus/dca.h>
#include <digidollar/health.h>
#include <chainparams.h>
#include <wallet/digidollarwallet.h>
#include <uint256.h>
#include <interfaces/wallet.h>
#include <interfaces/node.h>
#include <univalue.h>
#include <rpc/util.h>

#include <algorithm>

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QProgressBar>
#include <QFont>
#include <QTimer>
#include <QDateTime>
#include <QSpacerItem>
#include <QListWidget>
#include <QListWidgetItem>
#include <QCursor>
#include <QBrush>
#include <QColor>
#include <QAbstractScrollArea>
#include <QAbstractItemView>
#include <QApplication>
#include <QPalette>
#include <QPointer>
#include <QLocale>
#include <QStatusTipEvent>
#include <QFontMetrics>

namespace {
static const QString MAX_EXPECTED_BLOCKCHAIN_DD_SUPPLY = QStringLiteral("11,000,000,000.00 $DD");
static const QString MAX_EXPECTED_BLOCKCHAIN_DGB_LOCKED = QStringLiteral("21,000,000,000.00 DGB");
static constexpr int TOTALS_VALUE_HORIZONTAL_PADDING = 36;
static constexpr int TOTALS_FRAME_HORIZONTAL_PADDING = 72;
static constexpr int RECENT_TX_AMOUNT_COLUMN_MIN_WIDTH = 128;
// Wide enough for "Redeem 180-day"; longer names widen it at run time.
static constexpr int RECENT_TX_CATEGORY_COLUMN_MIN_WIDTH = 130;
static constexpr int RECENT_TX_COLUMN_SPACING = 16;

enum RecentTransactionRole {
    RecentTxIdRole = Qt::UserRole + 1,
    RecentTypeRole,
    RecentAmountRole,
    RecentDateRole,
    RecentConfirmationsRole,
    RecentNoteRole,
};
} // namespace

DigiDollarOverviewWidget::DigiDollarOverviewWidget(QWidget *parent) :
    QWidget(parent),
    m_mainLayout(nullptr),
    m_balanceFrame(nullptr),
    m_balanceLayout(nullptr),
    m_ddBalanceLabel(nullptr),
    m_ddBalanceValue(nullptr),
    m_ddPaymasterReservedLabel(nullptr),
    m_ddPaymasterReservedValue(nullptr),
    m_ddWalletTotalLabel(nullptr),
    m_ddWalletTotalValue(nullptr),
    m_ddPendingLabel(nullptr),
    m_ddPendingValue(nullptr),
    m_dgbCollateralLabel(nullptr),
    m_dgbCollateralValue(nullptr),
    m_balanceSeparator(nullptr),
    m_usdValueLabel(nullptr),
    m_usdValueValue(nullptr),
    m_systemHealthFrame(nullptr),
    m_systemHealthLayout(nullptr),
    m_oraclePriceLabel(nullptr),
    m_oraclePriceValue(nullptr),
    m_networkTotalDDLabel(nullptr),
    m_networkTotalDDValue(nullptr),
    m_networkTotalCollateralLabel(nullptr),
    m_networkTotalCollateralValue(nullptr),
    m_systemHealthLabel(nullptr),
    m_systemHealthValue(nullptr),
    m_dcaLevelLabel(nullptr),
    m_dcaLevelValue(nullptr),
    m_errLevelLabel(nullptr),
    m_errLevelValue(nullptr),
    m_systemHealthBar(nullptr),
    m_transactionsFrame(nullptr),
    m_transactionsLayout(nullptr),
    m_transactionsTitle(nullptr),
    m_transactionsList(nullptr),
    m_recentTransactionsInfo(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr),
    m_ddBalance(0.0),
    m_ddWalletTotal(0.0),
    m_ddPaymasterReserved(0.0),
    m_ddPending(0.0),
    m_dgbCollateral(0.0),
    m_oraclePrice(0.0),
    m_systemHealthStatus("Loading..."),
    m_dcaLevel(0),
    m_errLevel(0)
{
    setupUI();
    connectSignals();
    // REMOVED: applyTheme() - Let CSS handle all theming

    // Add some demo transactions for display purposes
    addDemoTransactions();
}

DigiDollarOverviewWidget::~DigiDollarOverviewWidget()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarOverviewWidget::setupUI()
{
    // Create main layout - reduced spacing for compact UI
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setSpacing(6);
    m_mainLayout->setContentsMargins(8, 8, 8, 8);

    // Create horizontal layout for balance and system health side-by-side
    QHBoxLayout* topLayout = new QHBoxLayout();
    topLayout->setSpacing(10);

    // Setup sections
    setupBalanceSection();
    setupSystemHealthSection();

    // Add balance and system health frames to horizontal layout
    // Both frames stretch to fill the same height for symmetry
    topLayout->addWidget(m_balanceFrame, 1);
    topLayout->addWidget(m_systemHealthFrame, 1);

    // Add the horizontal layout to main layout
    m_mainLayout->addLayout(topLayout);

    // Add transactions section below
    setupRecentTransactionsSection();

    setLayout(m_mainLayout);
}

void DigiDollarOverviewWidget::setupBalanceSection()
{
    // Create balance frame with styling matching main wallet
    m_balanceFrame = new QFrame(this);
    m_balanceFrame->setFrameShape(QFrame::StyledPanel);
    m_balanceFrame->setFrameShadow(QFrame::Raised);
    m_balanceFrame->setObjectName("balanceFrame");
    m_balanceFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout* frameVLayout = new QVBoxLayout(m_balanceFrame);
    frameVLayout->setObjectName("frameVLayout");

    // Add top stretch to center content vertically
    frameVLayout->addStretch(1);

    // Title with status indicator layout (similar to main wallet)
    QHBoxLayout* titleLayout = new QHBoxLayout();
    titleLayout->setObjectName("titleLayout");

    QLabel* balanceTitle = new QLabel(tr("DigiDollar Balances"), this);
    QFont titleFont = balanceTitle->font();
    titleFont.setBold(true);
    titleFont.setWeight(75); // Match main wallet weight
    balanceTitle->setFont(titleFont);
    titleLayout->addWidget(balanceTitle);

    // Add spacer to push content left (matching main wallet layout)
    QSpacerItem* titleSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    titleLayout->addItem(titleSpacer);

    frameVLayout->addLayout(titleLayout);

    // Grid layout for balance items - reduced spacing for compact UI
    m_balanceLayout = new QGridLayout();
    m_balanceLayout->setSpacing(6);
    m_balanceLayout->setObjectName("balanceGridLayout");

    // DD Balance (Available / Confirmed)
    m_ddBalanceLabel = new QLabel(tr("Available"), this);
    m_ddBalanceLabel->setObjectName("ddBalanceLabel");
    m_ddBalanceValue = new QLabel("0.00 $DD", this);
    m_ddBalanceValue->setObjectName("ddBalanceValue");
    m_ddBalanceValue->setCursor(QCursor(Qt::IBeamCursor));
    m_ddBalanceValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_ddBalanceValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_ddBalanceValue->setToolTip(tr("Your confirmed, spendable DigiDollar balance"));
    m_balanceLayout->addWidget(m_ddBalanceLabel, 1, 0);
    m_balanceLayout->addWidget(m_ddBalanceValue, 1, 1);

    // These rows are only shown while confirmed DD is protected by a
    // Paymaster session or provider carrier pool. Wallets not using
    // Paymaster retain the original compact balance layout unchanged.
    m_ddPaymasterReservedLabel = new QLabel(tr("Reserved for Paymaster"), this);
    m_ddPaymasterReservedLabel->setObjectName("ddPaymasterReservedLabel");
    m_ddPaymasterReservedValue = new QLabel("0.00 $DD", this);
    m_ddPaymasterReservedValue->setObjectName("ddPaymasterReservedValue");
    m_ddPaymasterReservedValue->setCursor(QCursor(Qt::IBeamCursor));
    m_ddPaymasterReservedValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_ddPaymasterReservedValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_ddPaymasterReservedValue->setToolTip(tr(
        "Confirmed DigiDollar protected for active Paymaster transfers or provider carrier liquidity. It remains yours but cannot be used by an unrelated send."));

    m_ddWalletTotalLabel = new QLabel(tr("Wallet total"), this);
    m_ddWalletTotalLabel->setObjectName("ddWalletTotalLabel");
    m_ddWalletTotalValue = new QLabel("0.00 $DD", this);
    m_ddWalletTotalValue->setObjectName("ddWalletTotalValue");
    m_ddWalletTotalValue->setCursor(QCursor(Qt::IBeamCursor));
    m_ddWalletTotalValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_ddWalletTotalValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_ddWalletTotalValue->setToolTip(tr(
        "All confirmed DigiDollar owned by this wallet: available plus Paymaster-reserved."));
    m_ddPaymasterReservedLabel->hide();
    m_ddPaymasterReservedValue->hide();
    m_ddWalletTotalLabel->hide();
    m_ddWalletTotalValue->hide();

    // DD Pending (Unconfirmed)
    m_ddPendingLabel = new QLabel(tr("Pending"), this);
    m_ddPendingLabel->setObjectName("ddPendingLabel");
    m_ddPendingValue = new QLabel("0.00 $DD", this);
    m_ddPendingValue->setObjectName("ddPendingValue");
    m_ddPendingValue->setCursor(QCursor(Qt::IBeamCursor));
    m_ddPendingValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_ddPendingValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_ddPendingValue->setToolTip(tr("DigiDollar balance from unconfirmed transactions (awaiting block confirmation)"));
    m_balanceLayout->addWidget(m_ddPendingLabel, 2, 0);
    m_balanceLayout->addWidget(m_ddPendingValue, 2, 1);

    // DGB Collateral (Locked)
    m_dgbCollateralLabel = new QLabel(tr("Locked Collateral"), this);
    m_dgbCollateralLabel->setObjectName("dgbCollateralLabel");
    m_dgbCollateralValue = new QLabel("0.00000000 DGB", this);
    m_dgbCollateralValue->setObjectName("dgbCollateralValue");
    m_dgbCollateralValue->setCursor(QCursor(Qt::IBeamCursor));
    m_dgbCollateralValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_dgbCollateralValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_dgbCollateralValue->setToolTip(tr("Your DGB locked as collateral for DigiDollars in your wallet"));
    m_balanceLayout->addWidget(m_dgbCollateralLabel, 3, 0);
    m_balanceLayout->addWidget(m_dgbCollateralValue, 3, 1);

    // Add separator line
    m_balanceSeparator = new QFrame(m_balanceFrame);
    m_balanceSeparator->setObjectName("line");
    m_balanceSeparator->setFrameShape(QFrame::HLine);
    m_balanceSeparator->setFrameShadow(QFrame::Sunken);
    m_balanceLayout->addWidget(m_balanceSeparator, 4, 0, 1, 2);

    // USD Value (Total — confirmed only)
    m_usdValueLabel = new QLabel(tr("Total $USD:"), this);
    m_usdValueLabel->setObjectName("usdValueLabel");
    m_usdValueValue = new QLabel(formatUSDAmount(0), this);
    m_usdValueValue->setObjectName("usdValueValue");
    m_usdValueValue->setCursor(QCursor(Qt::IBeamCursor));
    m_usdValueValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_usdValueValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_usdValueValue->setToolTip(tr("Your confirmed DigiDollar value in USD (excludes pending)"));
    m_balanceLayout->addWidget(m_usdValueLabel, 5, 0);
    m_balanceLayout->addWidget(m_usdValueValue, 5, 1);

    // Add horizontal spacer
    QSpacerItem* horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_balanceLayout->addItem(horizontalSpacer, 3, 2, 1, 1);

    frameVLayout->addLayout(m_balanceLayout);

    // Add bottom stretch to center content vertically
    frameVLayout->addStretch(1);

    // REMOVED: m_mainLayout->addWidget(m_balanceFrame);
    // Frame is now added to horizontal layout in setupUI()
}

void DigiDollarOverviewWidget::updateBalanceLayout(bool show_paymaster_breakdown)
{
    if (!m_balanceLayout) return;
    if (m_showPaymasterBreakdown == show_paymaster_breakdown) return;
    m_showPaymasterBreakdown = show_paymaster_breakdown;

    m_ddPaymasterReservedLabel->setVisible(show_paymaster_breakdown);
    m_ddPaymasterReservedValue->setVisible(show_paymaster_breakdown);
    m_ddWalletTotalLabel->setVisible(show_paymaster_breakdown);
    m_ddWalletTotalValue->setVisible(show_paymaster_breakdown);

    // This runs only when the presence of a reservation changes. Remove the
    // existing grid items explicitly before rebuilding so periodic balance
    // polling never accumulates overlapping layout entries.
    m_balanceLayout->removeWidget(m_ddBalanceLabel);
    m_balanceLayout->removeWidget(m_ddBalanceValue);
    m_balanceLayout->removeWidget(m_ddPaymasterReservedLabel);
    m_balanceLayout->removeWidget(m_ddPaymasterReservedValue);
    m_balanceLayout->removeWidget(m_ddWalletTotalLabel);
    m_balanceLayout->removeWidget(m_ddWalletTotalValue);
    m_balanceLayout->removeWidget(m_ddPendingLabel);
    m_balanceLayout->removeWidget(m_ddPendingValue);
    m_balanceLayout->removeWidget(m_dgbCollateralLabel);
    m_balanceLayout->removeWidget(m_dgbCollateralValue);
    m_balanceLayout->removeWidget(m_balanceSeparator);
    m_balanceLayout->removeWidget(m_usdValueLabel);
    m_balanceLayout->removeWidget(m_usdValueValue);

    // The false branch exactly restores the original non-Paymaster rows.
    m_balanceLayout->addWidget(m_ddBalanceLabel, 1, 0);
    m_balanceLayout->addWidget(m_ddBalanceValue, 1, 1);
    if (show_paymaster_breakdown) {
        m_balanceLayout->addWidget(m_ddPaymasterReservedLabel, 2, 0);
        m_balanceLayout->addWidget(m_ddPaymasterReservedValue, 2, 1);
        m_balanceLayout->addWidget(m_ddWalletTotalLabel, 3, 0);
        m_balanceLayout->addWidget(m_ddWalletTotalValue, 3, 1);
        m_balanceLayout->addWidget(m_ddPendingLabel, 4, 0);
        m_balanceLayout->addWidget(m_ddPendingValue, 4, 1);
        m_balanceLayout->addWidget(m_dgbCollateralLabel, 5, 0);
        m_balanceLayout->addWidget(m_dgbCollateralValue, 5, 1);
        m_balanceLayout->addWidget(m_balanceSeparator, 6, 0, 1, 2);
        m_balanceLayout->addWidget(m_usdValueLabel, 7, 0);
        m_balanceLayout->addWidget(m_usdValueValue, 7, 1);
    } else {
        m_balanceLayout->addWidget(m_ddPendingLabel, 2, 0);
        m_balanceLayout->addWidget(m_ddPendingValue, 2, 1);
        m_balanceLayout->addWidget(m_dgbCollateralLabel, 3, 0);
        m_balanceLayout->addWidget(m_dgbCollateralValue, 3, 1);
        m_balanceLayout->addWidget(m_balanceSeparator, 4, 0, 1, 2);
        m_balanceLayout->addWidget(m_usdValueLabel, 5, 0);
        m_balanceLayout->addWidget(m_usdValueValue, 5, 1);
    }
}

void DigiDollarOverviewWidget::setupSystemHealthSection()
{
    // Create system health frame with styling matching main wallet
    m_systemHealthFrame = new QFrame(this);
    m_systemHealthFrame->setFrameShape(QFrame::StyledPanel);
    m_systemHealthFrame->setFrameShadow(QFrame::Raised);
    m_systemHealthFrame->setObjectName("systemHealthFrame");
    m_systemHealthFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout* frameVLayout = new QVBoxLayout(m_systemHealthFrame);
    frameVLayout->setObjectName("healthFrameVLayout");

    // Title with status indicator layout
    QHBoxLayout* titleLayout = new QHBoxLayout();
    titleLayout->setObjectName("healthTitleLayout");

    QLabel* healthTitle = new QLabel(tr("Blockchain DigiDollar Status"), this);
    healthTitle->setObjectName("healthTitle");
    QFont titleFont = healthTitle->font();
    titleFont.setBold(true);
    titleFont.setWeight(75); // Match main wallet weight
    healthTitle->setFont(titleFont);
    titleLayout->addWidget(healthTitle);

    // Add spacer to push content left
    QSpacerItem* titleSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    titleLayout->addItem(titleSpacer);

    frameVLayout->addLayout(titleLayout);

    // ========================================
    // Split layout: Left stats | Right totals
    // ========================================
    QHBoxLayout* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(10);
    contentLayout->setObjectName("healthContentLayout");

    // --- LEFT SIDE: Other stats (DGB/USD, System Health, DCA, ERR) ---
    QFrame* leftStatsFrame = new QFrame(this);
    leftStatsFrame->setObjectName("leftStatsFrame");
    leftStatsFrame->setFrameShape(QFrame::NoFrame);
    leftStatsFrame->setMinimumWidth(170);

    m_systemHealthLayout = new QGridLayout(leftStatsFrame);
    m_systemHealthLayout->setSpacing(8);
    m_systemHealthLayout->setContentsMargins(0, 0, 0, 0);
    m_systemHealthLayout->setObjectName("healthGridLayout");

    // Oracle Price
    m_oraclePriceLabel = new QLabel(tr("DGB/$USD Price:"), this);
    m_oraclePriceLabel->setObjectName("oraclePriceLabel");
    m_oraclePriceValue = new QLabel("Loading...", this);
    m_oraclePriceValue->setObjectName("oraclePriceValue");
    DigiDollarStatus::SetText(m_oraclePriceValue, DigiDollarStatus::Kind::WAITING);
    m_oraclePriceValue->setCursor(QCursor(Qt::IBeamCursor));
    m_oraclePriceValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_oraclePriceValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_oraclePriceValue->setToolTip(tr("Current DigiByte to USD exchange rate from oracle"));
    m_systemHealthLayout->addWidget(m_oraclePriceLabel, 0, 0);
    m_systemHealthLayout->addWidget(m_oraclePriceValue, 0, 1);

    // System Health Status
    m_systemHealthLabel = new QLabel(tr("System Health:"), this);
    m_systemHealthLabel->setObjectName("systemHealthLabel");
    m_systemHealthValue = new QLabel("Loading...", this);
    m_systemHealthValue->setObjectName("systemHealthValue");
    DigiDollarStatus::SetText(m_systemHealthValue, DigiDollarStatus::Kind::WAITING);
    m_systemHealthValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_systemHealthValue->setToolTip(tr("Overall DigiDollar blockchain health status"));
    m_systemHealthLayout->addWidget(m_systemHealthLabel, 1, 0);
    m_systemHealthLayout->addWidget(m_systemHealthValue, 1, 1);

    // DCA Level
    m_dcaLevelLabel = new QLabel(tr("DCA Level:"), this);
    m_dcaLevelLabel->setObjectName("dcaLevelLabel");
    m_dcaLevelValue = new QLabel("0", this);
    m_dcaLevelValue->setObjectName("dcaLevelValue");
    DigiDollarStatus::SetText(m_dcaLevelValue, DigiDollarStatus::Kind::WAITING);
    m_dcaLevelValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_dcaLevelValue->setToolTip(tr("Current Dynamic Collateral Adjustment intervention level"));
    m_systemHealthLayout->addWidget(m_dcaLevelLabel, 2, 0);
    m_systemHealthLayout->addWidget(m_dcaLevelValue, 2, 1);

    // ERR Level
    m_errLevelLabel = new QLabel(tr("ERR Level:"), this);
    m_errLevelLabel->setObjectName("errLevelLabel");
    m_errLevelValue = new QLabel("0", this);
    m_errLevelValue->setObjectName("errLevelValue");
    DigiDollarStatus::SetText(m_errLevelValue, DigiDollarStatus::Kind::WAITING);
    m_errLevelValue->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_errLevelValue->setToolTip(tr("Current Emergency Redemption Ratio level"));
    m_systemHealthLayout->addWidget(m_errLevelLabel, 3, 0);
    m_systemHealthLayout->addWidget(m_errLevelValue, 3, 1);

    // --- RIGHT SIDE: Prominent Blockchain Totals (DD Supply + Collateral) ---
    QFrame* rightTotalsFrame = new QFrame(this);
    rightTotalsFrame->setObjectName("networkTotalsFrame");
    rightTotalsFrame->setFrameShape(QFrame::StyledPanel);
    rightTotalsFrame->setFrameShadow(QFrame::Raised);
    rightTotalsFrame->setMinimumWidth(300);

    QVBoxLayout* totalsLayout = new QVBoxLayout(rightTotalsFrame);
    totalsLayout->setSpacing(12);
    totalsLayout->setContentsMargins(15, 15, 15, 15);

    // Blockchain Total DD Supply (prominent)
    m_networkTotalDDLabel = new QLabel(tr("Blockchain $DD Supply"), this);
    m_networkTotalDDLabel->setObjectName("networkTotalDDLabel");
    m_networkTotalDDLabel->setAlignment(Qt::AlignCenter);
    m_networkTotalDDLabel->setWordWrap(false);
    totalsLayout->addWidget(m_networkTotalDDLabel);

    m_networkTotalDDValue = new QLabel("Loading...", this);
    m_networkTotalDDValue->setObjectName("networkTotalDDValue");
    m_networkTotalDDValue->setCursor(QCursor(Qt::IBeamCursor));
    m_networkTotalDDValue->setAlignment(Qt::AlignCenter);
    m_networkTotalDDValue->setWordWrap(false);
    m_networkTotalDDValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_networkTotalDDValue->setToolTip(tr("Total DigiDollar supply recorded on the blockchain"));
    totalsLayout->addWidget(m_networkTotalDDValue);

    // Separator between totals
    QFrame* separator = new QFrame(this);
    separator->setObjectName("totalsSeparator");
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    totalsLayout->addWidget(separator);

    // Blockchain Total Collateral (prominent)
    m_networkTotalCollateralLabel = new QLabel(tr("Blockchain DGB Locked"), this);
    m_networkTotalCollateralLabel->setObjectName("networkTotalCollateralLabel");
    m_networkTotalCollateralLabel->setAlignment(Qt::AlignCenter);
    m_networkTotalCollateralLabel->setWordWrap(false);
    totalsLayout->addWidget(m_networkTotalCollateralLabel);

    m_networkTotalCollateralValue = new QLabel("Loading...", this);
    m_networkTotalCollateralValue->setObjectName("networkTotalCollateralValue");
    m_networkTotalCollateralValue->setCursor(QCursor(Qt::IBeamCursor));
    m_networkTotalCollateralValue->setAlignment(Qt::AlignCenter);
    m_networkTotalCollateralValue->setWordWrap(false);
    m_networkTotalCollateralValue->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    m_networkTotalCollateralValue->setToolTip(tr("Total DGB locked as collateral on the blockchain"));
    totalsLayout->addWidget(m_networkTotalCollateralValue);

    updateBlockchainTotalsMinimumWidth();

    // Add left and right to horizontal content layout
    contentLayout->addWidget(leftStatsFrame, 2);
    contentLayout->addWidget(rightTotalsFrame, 3);

    frameVLayout->addLayout(contentLayout);

    // System Health Progress Bar at bottom - spans full width
    m_systemHealthBar = new QProgressBar(this);
    m_systemHealthBar->setObjectName("systemHealthBar");
    m_systemHealthBar->setRange(0, 100);
    m_systemHealthBar->setValue(0);
    m_systemHealthBar->setTextVisible(true);
    m_systemHealthBar->setFormat("0% Collateralization");
    m_systemHealthBar->setMinimumHeight(20);
    m_systemHealthBar->setToolTip(tr("Visual indicator of overall blockchain health"));
    frameVLayout->addWidget(m_systemHealthBar);

    // REMOVED: m_mainLayout->addWidget(m_systemHealthFrame);
    // Frame is now added to horizontal layout in setupUI()
}

void DigiDollarOverviewWidget::setupRecentTransactionsSection()
{
    // Create recent transactions frame with styling matching main wallet
    m_transactionsFrame = new QFrame(this);
    m_transactionsFrame->setFrameShape(QFrame::StyledPanel);
    m_transactionsFrame->setFrameShadow(QFrame::Raised);
    m_transactionsFrame->setObjectName("transactionsFrame");

    QVBoxLayout* frameVLayout = new QVBoxLayout(m_transactionsFrame);
    frameVLayout->setObjectName("transactionsFrameVLayout");

    // Title with status indicator layout
    QHBoxLayout* titleLayout = new QHBoxLayout();
    titleLayout->setObjectName("transactionsTitleLayout");

    m_transactionsTitle = new QLabel(tr("20 Most Recent DigiDollar Transactions"), this);
    QFont titleFont = m_transactionsTitle->font();
    titleFont.setBold(true);
    titleFont.setWeight(75); // Match main wallet weight
    m_transactionsTitle->setFont(titleFont);
    titleLayout->addWidget(m_transactionsTitle);

    // Add spacer to push content left
    QSpacerItem* titleSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    titleLayout->addItem(titleSpacer);

    frameVLayout->addLayout(titleLayout);

    // Create list widget for transactions (similar to main wallet)
    m_transactionsList = new QListWidget(this);
    m_transactionsList->setObjectName("transactionsList");
    m_transactionsList->setFrameShape(QFrame::NoFrame);
    m_transactionsList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_transactionsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_transactionsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_transactionsList->setUniformItemSizes(true);
    // No minimum height - allow to shrink with window
    m_transactionsList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(m_transactionsList, &QListWidget::itemDoubleClicked,
            this, &DigiDollarOverviewWidget::activateRecentTransaction);
    connect(m_transactionsList, &QListWidget::itemActivated,
            this, &DigiDollarOverviewWidget::activateRecentTransaction);

    // Info label for when no transactions exist
    m_recentTransactionsInfo = new QLabel(tr("No recent DigiDollar transactions"), this);
    m_recentTransactionsInfo->setObjectName("recentTransactionsInfo");
    // Theme styling will be applied in applyTheme()
    m_recentTransactionsInfo->setAlignment(Qt::AlignCenter);
    m_recentTransactionsInfo->setVisible(true);

    frameVLayout->addWidget(m_transactionsList);
    frameVLayout->addWidget(m_recentTransactionsInfo);

    // Set transactions frame to expand vertically
    m_transactionsFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Add transactions frame with stretch factor to fill remaining space
    m_mainLayout->addWidget(m_transactionsFrame, 1); // stretch factor 1 = expand to fill
}

void DigiDollarOverviewWidget::connectSignals()
{
    // Connect update timer (every 5 seconds for better sync across wallets)
    QTimer* updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, &DigiDollarOverviewWidget::updateView);
    updateTimer->start(5000); // 5 seconds - faster updates for network sync

    // Note: Additional wallet and client model signals will be connected
    // in setWalletModel() and setClientModel() once models are available
}

void DigiDollarOverviewWidget::setWalletModel(WalletModel* model)
{
    if (m_walletModel == model) {
        updateRecentTransactions();
        return;
    }
    if (m_walletModel) disconnect(m_walletModel, nullptr, this, nullptr);

    m_walletModel = model;
    m_recentSnapshotShown = false;
    m_recentRefreshInFlight = false;
    m_recentRefreshPending = false;
    m_lastRecentTransactionsFingerprint.clear();
    m_lastTxUpdateTime = 0;
    if (m_transactionsList) m_transactionsList->clear();
    if (m_recentTransactionsInfo) m_recentTransactionsInfo->setVisible(true);

    if (m_walletModel) {
        // Hidden pages defer wallet reads until selected. In particular, a DD
        // history RPC may hold cs_wallet in a worker while the GUI receives a
        // balance signal; a hidden overview must not synchronously wait for it.
        connect(m_walletModel, &WalletModel::balanceChanged, this, [this] {
            if (isVisible()) updateBalance();
        });
        connect(m_walletModel, &WalletModel::balanceChanged, this, [this] {
            if (isVisible()) updateRecentTransactions();
        });
        connect(m_walletModel, &WalletModel::digiDollarChanged,
                this, &DigiDollarOverviewWidget::refreshDigiDollarState);

        // Initial updates
        updateBalance();
        updateRecentTransactions();

        // Connect to options model for font updates
        if (m_walletModel->getOptionsModel()) {
            connect(m_walletModel->getOptionsModel(), &OptionsModel::useEmbeddedMonospacedFontChanged,
                    this, &DigiDollarOverviewWidget::setMonospacedFont);
            setMonospacedFont(m_walletModel->getOptionsModel()->getUseEmbeddedMonospacedFont());
            // REMOVED: applyTheme() - Let CSS handle all theming
        }
    }
}

void DigiDollarOverviewWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    if (m_clientModel) {
        // NOTE: We do NOT connect to numBlocksChanged for DD updates!
        // DD updates should only happen when balance changes (like DGB).
        // Oracle price and system health are updated via the 30-second timer in updateView().
        // This prevents constant updates during block sync.

        // Initial updates
        updateOraclePrice();
        updateSystemHealthIfDue(/*force=*/true);

        // Connect to options model for font updates
        if (m_clientModel->getOptionsModel()) {
            connect(m_clientModel->getOptionsModel(), &OptionsModel::useEmbeddedMonospacedFontChanged,
                    this, &DigiDollarOverviewWidget::setMonospacedFont);
            setMonospacedFont(m_clientModel->getOptionsModel()->getUseEmbeddedMonospacedFont());
            // REMOVED: applyTheme() - Let CSS handle all theming
        }
    }
}

void DigiDollarOverviewWidget::updateView()
{
    // Don't poll RPCs if widget is hidden (DD not yet active)
    if (!isVisible()) return;
    updateBalance();
    updateOraclePrice();
    updateSystemHealthIfDue(/*force=*/false);
    updateRecentTransactions();
}

void DigiDollarOverviewWidget::updateSystemHealthIfDue(bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_lastSystemHealthUpdateTime > 0 &&
        now - m_lastSystemHealthUpdateTime < SYSTEM_HEALTH_UPDATE_INTERVAL_MS) {
        return;
    }
    m_lastSystemHealthUpdateTime = now;
    updateSystemHealth();
}

void DigiDollarOverviewWidget::incomingDDTransaction(const QString& date, const QString& amount,
                                                    const QString& type, const QString& address)
{
    // Add transaction to list widget
    QListWidgetItem* item = new QListWidgetItem(m_transactionsList);
    QString transactionText = QString("%1 %2 $DD - %3")
                                .arg(type)
                                .arg(amount)
                                .arg(date);
    item->setText(transactionText);
    item->setToolTip(QString("Address: %1").arg(address));

    // Color code transaction types with theme support
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    if (type.contains("Received") || type.contains("Minted")) {
        QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
        item->setForeground(QBrush(QColor(successColor)));
    } else if (type.contains("Sent") || type.contains("Redeemed")) {
        QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";
        item->setForeground(QBrush(QColor(errorColor)));
    }

    // Insert at top (most recent first)
    m_transactionsList->insertItem(0, item);

    // Limit to 5 most recent transactions
    while (m_transactionsList->count() > 5) {
        delete m_transactionsList->takeItem(m_transactionsList->count() - 1);
    }

    // Hide the "no transactions" label and show the list
    m_recentTransactionsInfo->setVisible(false);
    m_transactionsList->setVisible(true);

    // Update balance after a short delay to allow for processing
    QTimer::singleShot(1000, this, &DigiDollarOverviewWidget::updateBalance);
}

void DigiDollarOverviewWidget::updateBalance()
{
    // Skip updates during Initial Block Download - balance only matters when synced
    if (m_clientModel && m_clientModel->node().isInitialBlockDownload()) {
        return;
    }

    // Throttle updates - skip if less than 5 seconds since last update
    // This prevents UI freeze when balance changes flood in after IBD ends
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastBalanceUpdateTime < UPDATE_THROTTLE_MS) {
        return; // Skip this update, too soon
    }
    m_lastBalanceUpdateTime = now;

    // Get actual DigiDollar balance from wallet
    if (m_walletModel) {
        // "Available" is ordinary spendable DD. The total remains useful for
        // ownership/accounting but is only exposed as a separate row when a
        // Paymaster reservation actually exists.
        const WalletModel::DigiDollarBalanceSummary balances =
            m_walletModel->getDigiDollarBalanceSummary();
        m_ddBalance = balances.available / 100.0;
        m_ddWalletTotal = balances.confirmed_total / 100.0;
        m_ddPaymasterReserved = balances.paymaster_reserved / 100.0;
        m_ddPending = balances.pending / 100.0;

        // Get locked collateral from wallet positions (in satoshis)
        CAmount collateralSats = m_walletModel->getLockedCollateral();
        m_dgbCollateral = collateralSats / 100000000.0; // Convert satoshis to DGB
    } else {
        // No wallet connected
        m_ddBalance = 0.0;
        m_ddWalletTotal = 0.0;
        m_ddPaymasterReserved = 0.0;
        m_ddPending = 0.0;
        m_dgbCollateral = 0.0;
    }

    updateBalanceLayout(m_ddPaymasterReserved > 0.0);

    // Update display — Available (confirmed only)
    if (m_privacy) {
        m_ddBalanceValue->setText(maskValue(formatDDAmount(0)));
        m_ddPaymasterReservedValue->setText(maskValue(formatDDAmount(0)));
        m_ddWalletTotalValue->setText(maskValue(formatDDAmount(0)));
        m_ddPendingValue->setText(maskValue(formatDDAmount(0)));
        m_ddPendingLabel->setVisible(false);
        m_ddPendingValue->setVisible(false);
        m_dgbCollateralValue->setText(maskValue(formatDGBAmount(0)));
        m_usdValueValue->setText(maskValue(formatUSDAmount(0)));
    } else {
        m_ddBalanceValue->setText(formatDDAmount(m_ddBalance));
        m_ddPaymasterReservedValue->setText(formatDDAmount(m_ddPaymasterReserved));
        m_ddWalletTotalValue->setText(formatDDAmount(m_ddWalletTotal));

        // Pending (unconfirmed but trusted) — hide row when zero for clean UI
        m_ddPendingValue->setText(formatDDAmount(m_ddPending));
        const bool hasPending = m_ddPending > 0.0;
        m_ddPendingLabel->setVisible(hasPending);
        m_ddPendingValue->setVisible(hasPending);

        m_dgbCollateralValue->setText(formatDGBAmount(m_dgbCollateral));

        // Paymaster-reserved DD remains wallet-owned, so wallet valuation uses
        // the confirmed total rather than only the immediately spendable part.
        const double usdValue = m_ddWalletTotal;
        m_usdValueValue->setText(formatUSDAmount(usdValue));
    }
}

void DigiDollarOverviewWidget::refreshDigiDollarState()
{
    m_lastBalanceUpdateTime = 0;
    m_lastTxUpdateTime = 0;
    if (!isVisible()) return;
    updateBalance();
    updateRecentTransactions();
}

void DigiDollarOverviewWidget::updateOraclePrice()
{
    // Skip updates during Initial Block Download - oracle price only matters when synced
    if (m_clientModel && m_clientModel->node().isInitialBlockDownload()) {
        return;
    }

    // Get price from MockOracleManager if in RegTest, otherwise use real oracle via RPC
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        // Get regtest mock oracle price in micro-USD.
        // 1,000,000 micro-USD = $1.00
        CAmount priceMicroUsd = MockOracleManager::GetInstance().GetCurrentPrice();

        // Convert micro-USD to dollars
        m_oraclePrice = priceMicroUsd / 1000000.0;
    } else if (m_clientModel) {
        // Get actual oracle price from RPC
        try {
            UniValue params(UniValue::VARR);
            UniValue result = m_clientModel->node().executeRpc("getoracleprice", params, "");

            // Price is returned in micro-USD (1,000,000 = $1.00)
            int64_t priceMicroUsd = result.find_value("price_micro_usd").getInt<int64_t>();
            m_oraclePrice = priceMicroUsd / 1000000.0; // Convert micro-USD to dollars
        } catch (const UniValue& e) {
            LogPrintf("DigiDollar: updateOraclePrice RPC error - %s\n", e.write());
            m_oraclePrice = 0.0;
        } catch (const std::exception& e) {
            LogPrintf("DigiDollar: updateOraclePrice error - %s\n", e.what());
            m_oraclePrice = 0.0;
        } catch (...) {
            LogPrintf("DigiDollar: updateOraclePrice unknown error\n");
            m_oraclePrice = 0.0;
        }
    } else {
        m_oraclePrice = 0.0; // No client model available
    }

    if (m_oraclePrice > 0) {
        DigiDollarStatus::SetText(m_oraclePriceValue, DigiDollarStatus::Kind::SUCCESS);
        m_oraclePriceValue->setText(QString("%1 $USD").arg(QString::number(m_oraclePrice, 'f', 6)));
    } else {
        DigiDollarStatus::SetText(m_oraclePriceValue, DigiDollarStatus::Kind::WAITING);
        m_oraclePriceValue->setText(tr("… Oracle unavailable"));
    }
}

void DigiDollarOverviewWidget::updateSystemHealth()
{
    // Skip updates during Initial Block Download - system health only matters when synced
    if (m_clientModel && m_clientModel->node().isInitialBlockDownload()) {
        return;
    }

    // BLOCKCHAIN-WIDE TRACKING: Call RPC to get blockchain-wide system health
    // This ensures Bob and Alice both see identical stats across the chain.

    if (!m_clientModel) {
        DigiDollarStatus::SetText(m_systemHealthValue, DigiDollarStatus::Kind::ERR);
        m_systemHealthValue->setText(tr("✕ No connection"));
        m_networkTotalDDValue->setText("N/A");
        m_networkTotalCollateralValue->setText("N/A");
        m_dcaLevelValue->setText("N/A");
        m_errLevelValue->setText("N/A");
        m_systemHealthBar->setValue(0);
        return;
    }

    try {
        // Execute RPC call to get blockchain-wide system health
        UniValue params(UniValue::VARR); // No parameters needed
        UniValue result = m_clientModel->node().executeRpc("getdigidollarstats", params, "");

        // Extract values from RPC result
        int healthPercentage = result.find_value("health_percentage").getInt<int>();
        std::string healthStatus = result.find_value("health_status").get_str();
        CAmount totalCollateralSats = AmountFromValue(result.find_value("total_collateral_dgb"));
        CAmount totalDDCents = result.find_value("total_dd_supply").getInt<int64_t>();
        bool isEmergency = result.find_value("is_emergency").get_bool();
        const UniValue& oracleAvailableValue = result.find_value("oracle_available");
        const bool oracleAvailable = oracleAvailableValue.isNull() ? true : oracleAvailableValue.get_bool();

        // Get DCA tier info
        const UniValue& dcaTier = result.find_value("dca_tier");
        double dcaMultiplier = dcaTier.find_value("multiplier").get_real();

        // Get ERR tier info
        const UniValue& errTier = result.find_value("err_tier");
        double errRatio = errTier.find_value("ratio").get_real();
        double errBurnMultiplier = errTier.find_value("burn_multiplier").get_real();

        // Update blockchain-wide stats
        double totalDD = totalDDCents / 100.0; // Convert cents to DD
        double totalCollateralDGB = totalCollateralSats / 100000000.0; // Convert satoshis to DGB

        // Format with commas for readability
        QLocale locale(QLocale::English);
        QString ddFormatted = locale.toString(totalDD, 'f', 2);
        QString dgbFormatted = locale.toString(totalCollateralDGB, 'f', 2);

        // Use rich text for colored formatting: green numbers and white $DD label
        m_networkTotalDDValue->setTextFormat(Qt::RichText);
        m_networkTotalDDValue->setText(QString("<span style='color: #00FF88; font-weight: bold;'>%1</span><span style='color: white;'> $DD</span>").arg(ddFormatted));

        m_networkTotalCollateralValue->setTextFormat(Qt::RichText);
        m_networkTotalCollateralValue->setText(QString("<span style='color: #00FF88; font-weight: bold;'>%1</span><span style='color: white;'> DGB</span>").arg(dgbFormatted));

        if (!oracleAvailable) {
            DigiDollarStatus::SetText(m_systemHealthValue, DigiDollarStatus::Kind::WAITING);
            DigiDollarStatus::SetText(m_dcaLevelValue, DigiDollarStatus::Kind::WAITING);
            DigiDollarStatus::SetText(m_errLevelValue, DigiDollarStatus::Kind::WAITING);
            m_systemHealthValue->setText(tr("… Oracle unavailable"));
            m_dcaLevelValue->setText(tr("Paused"));
            m_errLevelValue->setText(tr("Not Evaluated"));
            m_systemHealthBar->setValue(0);
            m_systemHealthBar->setFormat(tr("Oracle price unavailable"));
            return;
        }

        // Update system health display
        // RPC returns health_percentage as actual percentage (e.g., 151 = 151%)
        double healthPercent = static_cast<double>(healthPercentage);

        DigiDollarStatus::SetText(
            m_systemHealthValue,
            isEmergency ? DigiDollarStatus::Kind::ERR : DigiDollarStatus::Kind::SUCCESS);
        DigiDollarStatus::SetText(
            m_dcaLevelValue,
            isEmergency ? DigiDollarStatus::Kind::ACTION : DigiDollarStatus::Kind::SUCCESS);
        DigiDollarStatus::SetText(
            m_errLevelValue,
            isEmergency ? DigiDollarStatus::Kind::ERR : DigiDollarStatus::Kind::SUCCESS);
        m_systemHealthValue->setText(QString("%1% Collateralized").arg(healthPercentage));

        // Update DCA and ERR levels
        m_dcaLevelValue->setText(QString("%1x").arg(QString::number(dcaMultiplier, 'f', 1)));

        // Show ERR as percentage ratio and burn multiplier
        // Normal: "100% (1.0x)" | Emergency: "80% (1.25x burn)"
        if (isEmergency) {
            int errPercent = static_cast<int>(errRatio * 100);
            m_errLevelValue->setText(QString("%1% (%2x burn)")
                .arg(errPercent)
                .arg(QString::number(errBurnMultiplier, 'f', 2)));
        } else {
            m_errLevelValue->setText("100% (Normal)");
        }

        // Update progress bar (scale 0-500% to 0-100%)
        int barValue = std::min(100, static_cast<int>((healthPercent * 100) / 500));
        m_systemHealthBar->setValue(barValue);
        m_systemHealthBar->setFormat(QString("%1% Collateralization").arg(QString::number(healthPercent, 'f', 1)));

    } catch (const UniValue& e) {
        LogPrintf("DigiDollar: updateSystemHealth RPC error - %s\n", e.write());
        DigiDollarStatus::SetText(m_systemHealthValue, DigiDollarStatus::Kind::WAITING);
        DigiDollarStatus::SetText(m_dcaLevelValue, DigiDollarStatus::Kind::WAITING);
        DigiDollarStatus::SetText(m_errLevelValue, DigiDollarStatus::Kind::WAITING);
        m_systemHealthValue->setText(tr("… Loading…"));
        m_networkTotalDDValue->setText("Loading...");
        m_networkTotalCollateralValue->setText("Loading...");
        m_dcaLevelValue->setText("Loading...");
        m_errLevelValue->setText("Loading...");
        m_systemHealthBar->setValue(0);
    } catch (const std::exception& e) {
        DigiDollarStatus::SetText(m_systemHealthValue, DigiDollarStatus::Kind::ERR);
        DigiDollarStatus::SetText(m_dcaLevelValue, DigiDollarStatus::Kind::ERR);
        DigiDollarStatus::SetText(m_errLevelValue, DigiDollarStatus::Kind::ERR);
        m_systemHealthValue->setText(tr("✕ Status unavailable"));
        m_networkTotalDDValue->setText("Error");
        m_networkTotalCollateralValue->setText("Error");
        m_dcaLevelValue->setText("Unknown");
        m_errLevelValue->setText("Unknown");
        m_systemHealthBar->setValue(0);
        LogPrintf("DigiDollar: updateSystemHealth RPC error - %s\n", e.what());
    }
}

void DigiDollarOverviewWidget::updateRecentTransactions()
{
    if (!isVisible() || !m_walletModel) {
        return;
    }

    // WalletDB history is available before live wallet reconstruction and does
    // not depend on chain synchronization. Paint it once even during IBD so a
    // freshly opened wallet never presents an apparently empty account.
    if (!m_recentSnapshotShown) {
        const UniValue initial =
            m_walletModel->getCachedDigiDollarTransactionHistory(20, 0);
        if (initial.isArray() && !initial.empty()) {
            m_recentSnapshotShown = true;
            m_lastRecentTransactionsFingerprint = QString::fromStdString(initial.write());
            populateRecentTransactions(initial);
        }
    }

    // During IBD retain the persisted rows but defer the expensive canonical
    // reconstruction until chain and wallet confirmation state are meaningful.
    if (m_clientModel && m_clientModel->node().isInitialBlockDownload()) {
        return;
    }

    // Throttle updates - skip if less than 5 seconds since last update
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastTxUpdateTime < UPDATE_THROTTLE_MS) {
        return; // Skip this update, too soon
    }
    m_lastTxUpdateTime = now;

    if (m_recentRefreshInFlight) {
        m_recentRefreshPending = true;
        return;
    }
    m_recentRefreshInFlight = true;

    WalletModel* const requested_model = m_walletModel;
    QPointer<DigiDollarOverviewWidget> guard{this};
    m_walletModel->getDigiDollarTransactionHistoryAsync(
        20, 0,
        [guard, requested_model](UniValue result, QString error) mutable {
            if (!guard || guard->m_walletModel != requested_model) return;
            guard->m_recentRefreshInFlight = false;

            if (!error.isEmpty()) {
                // A failed worker request must not fall back to a blocking
                // wallet history read on Qt's event thread. Keep the previous
                // list visible and retry on the next scheduled refresh.
                LogPrintf("DigiDollar Overview: asynchronous history error - %s\n",
                          error.toStdString());
            }
            if (result.isArray()) {
                const QString fingerprint = QString::fromStdString(result.write());
                if (fingerprint != guard->m_lastRecentTransactionsFingerprint) {
                    guard->m_lastRecentTransactionsFingerprint = fingerprint;
                    guard->populateRecentTransactions(result);
                }
            }
            if (guard->m_recentRefreshPending) {
                guard->m_recentRefreshPending = false;
                guard->m_lastTxUpdateTime = 0;
                QTimer::singleShot(0, guard, &DigiDollarOverviewWidget::updateRecentTransactions);
            }
        });
}

void DigiDollarOverviewWidget::populateRecentTransactions(const UniValue& result)
{
    std::vector<DDTransaction> transactions;
    if (!result.isArray()) return;
    transactions.reserve(result.size());
    for (size_t index = 0; index < result.size(); ++index) {
        const UniValue& value = result[index];
        if (!value.isObject()) continue;
        DDTransaction tx;
        tx.txid = value.find_value("txid").get_str();
        tx.category = value.find_value("category").get_str();
        const CAmount signed_amount = value.find_value("amount").getInt<int64_t>();
        tx.amount = signed_amount < 0 ? -signed_amount : signed_amount;
        tx.incoming = signed_amount >= 0;
        tx.timestamp = value.find_value("time").getInt<uint64_t>();
        tx.confirmations = value.find_value("confirmations").getInt<int>();
        tx.comment = value.find_value("comment").get_str();
        tx.abandoned = value.find_value("abandoned").get_bool();
        tx.lock_tier = value.find_value("lock_tier").getInt<int>();
        const UniValue& wallet_state = value.find_value("wallet_state");
        tx.is_local = wallet_state.isStr() && wallet_state.get_str() == "local";
        transactions.push_back(std::move(tx));
    }

    // Sort transactions by timestamp descending (newest first)
    std::sort(transactions.begin(), transactions.end(),
        [](const DDTransaction& a, const DDTransaction& b) {
            return a.timestamp > b.timestamp;
        });

    // NOTE: Removed expensive per-transaction getWalletTxDetails loop
    // Confirmations are now calculated on-demand in GetDDTransactionHistory()

    auto formatRecentAmount = [](const DDTransaction& tx) {
        const CAmount absAmount = tx.amount < 0 ? -tx.amount : tx.amount;
        const bool is_outflow = (tx.category == "send" || tx.category == "redeem") || tx.amount < 0;
        const bool is_inflow = (tx.category == "receive" || tx.category == "mint") || tx.amount > 0;
        const QString amountPrefix = absAmount == 0 ? "" : (is_outflow ? "-" : (is_inflow ? "+" : ""));
        QString amount = QString("%1 $DD").arg(static_cast<double>(absAmount) / 100.0, 0, 'f', 2);
        amount.prepend(amountPrefix);
        return amount;
    };

    QFont monospaceFont = GUIUtil::fixedPitchFont();
    QFontMetrics amountMetrics(monospaceFont);
    // The type column has to fit the longest name a row can carry. Measure it
    // with the font the row labels really use, which comes from the theme
    // stylesheet and not from the list.
    QLabel categoryProbe(this);
    categoryProbe.setObjectName("recentTxCategoryLabel");
    categoryProbe.hide();
    categoryProbe.ensurePolished();
    const int categoryColumnWidth = std::max(
        RECENT_TX_CATEGORY_COLUMN_MIN_WIDTH,
        categoryProbe.fontMetrics().horizontalAdvance(DigiDollarLabels::ChangeReturned()) + 16);
    int amountColumnWidth = RECENT_TX_AMOUNT_COLUMN_MIN_WIDTH;
    int measuredCount = 0;
    for (const auto& tx : transactions) {
        if (measuredCount >= 20) break;
        ++measuredCount;
        amountColumnWidth = std::max(amountColumnWidth,
                                     amountMetrics.horizontalAdvance(formatRecentAmount(tx)) + 12);
    }

    // Clear existing items
    m_transactionsList->clear();

    // Show recent transactions (first 20 - already sorted newest first)
    int count = 0;
    for (const auto& tx : transactions) {
        if (count >= 20) break;
        ++count;

        // Create transaction item widget
        QWidget* itemWidget = new QWidget();
        QHBoxLayout* layout = new QHBoxLayout(itemWidget);
        layout->setContentsMargins(10, 5, 10, 5);
        layout->setSpacing(RECENT_TX_COLUMN_SPACING);

        // Transaction type icon and category with lock period for mints
        QString icon;
        QString categoryText;
        QString lockPeriodStr;

        // Format lock period based on tier (0-9, matches consensus/digidollar.h)
        switch (tx.lock_tier) {
            case 0:  lockPeriodStr = tr("1-hr"); break;
            case 1:  lockPeriodStr = tr("30-day"); break;
            case 2:  lockPeriodStr = tr("3-mo"); break;
            case 3:  lockPeriodStr = tr("6-mo"); break;
            case 4:  lockPeriodStr = tr("1-yr"); break;
            case 5:  lockPeriodStr = tr("2-yr"); break;
            case 6:  lockPeriodStr = tr("3-yr"); break;
            case 7:  lockPeriodStr = tr("5-yr"); break;
            case 8:  lockPeriodStr = tr("7-yr"); break;
            case 9:  lockPeriodStr = tr("10-yr"); break;
            default: lockPeriodStr = ""; break;
        }

        if (tx.category == "mint") {
            icon = "🏦";
            categoryText = lockPeriodStr.isEmpty() ? tr("Mint") : tr("Mint %1").arg(lockPeriodStr);
        } else if (tx.category == "redeem") {
            icon = "💰";
            categoryText = lockPeriodStr.isEmpty() ? tr("Redeem") : tr("Redeem %1").arg(lockPeriodStr);
        } else if (tx.category == "redeem_change") {
            icon = "💰";
            categoryText = DigiDollarLabels::ChangeReturned();
        } else if (tx.category == "send") {
            icon = "📤";
            categoryText = tr("Send");
        } else if (tx.category == "receive") {
            icon = "📥";
            categoryText = tr("Receive");
        } else {
            icon = "💵";
            categoryText = QString::fromStdString(tx.category);
        }

        QLabel* iconLabel = new QLabel(icon);
        iconLabel->setObjectName("recentTxIconLabel");
        iconLabel->setFixedWidth(30);
        iconLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(iconLabel);

        QLabel* categoryLabel = new QLabel(categoryText);
        categoryLabel->setObjectName("recentTxCategoryLabel");
        categoryLabel->setFixedWidth(categoryColumnWidth);
        if (tx.category == "redeem_change") {
            categoryLabel->setToolTip(DigiDollarLabels::ChangeReturnedExplanation());
        }
        layout->addWidget(categoryLabel);

        // Amount — DDTransaction stores unsigned magnitudes, so derive sign from
        // category (send/redeem are outflows, receive/mint are inflows). This
        // matches listdigidollartxs RPC behaviour and the row colour logic below.
        QLabel* amountLabel = new QLabel(formatRecentAmount(tx));
        amountLabel->setObjectName("recentTxAmountLabel");
        amountLabel->setFont(monospaceFont);
        amountLabel->setFixedWidth(amountColumnWidth);
        amountLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        amountLabel->setAlignment(Qt::AlignRight);
        if (tx.amount < 0 || tx.category == "send" || tx.category == "redeem") {
            amountLabel->setStyleSheet("color: #ff4646;");
        } else if (tx.amount > 0 || tx.category == "receive" || tx.category == "mint") {
            amountLabel->setStyleSheet("color: #64ff64;");
        }
        layout->addWidget(amountLabel);

        // Confirmations - also check for abandoned status
        QString confirmText;
        if (tx.abandoned) {
            confirmText = tr("Abandoned");
        } else if (tx.confirmations < 0) {
            confirmText = tr("Conflicted");
        } else if (tx.confirmations == 0) {
            confirmText = tx.is_local ? tr("Local") : tr("Pending");
        } else if (tx.confirmations < 6) {
            confirmText = QString("%1 conf").arg(tx.confirmations);
        } else {
            confirmText = tr("Confirmed");
        }
        QLabel* confirmLabel = new QLabel(confirmText);
        confirmLabel->setObjectName("recentTxStatusLabel");
        if (tx.is_local) {
            confirmLabel->setToolTip(tr("Created locally but not currently in mempool. It may need rebroadcast or may have been rejected."));
        }
        confirmLabel->setFixedWidth(100);
        layout->addWidget(confirmLabel);

        // Date/time
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(tx.timestamp);
        QLabel* dateLabel = new QLabel(dateTime.toString("MMM dd, yyyy"));
        dateLabel->setObjectName("recentTxDateLabel");
        dateLabel->setAlignment(Qt::AlignRight);
        layout->addWidget(dateLabel);

        layout->addStretch();

        // Add to list
        QListWidgetItem* item = new QListWidgetItem(m_transactionsList);
        item->setSizeHint(itemWidget->sizeHint());
        item->setData(RecentTxIdRole, QString::fromStdString(tx.txid));
        item->setData(RecentTypeRole, categoryText);
        item->setData(RecentAmountRole, amountLabel->text());
        item->setData(RecentDateRole, dateLabel->text());
        item->setData(RecentConfirmationsRole, confirmText);
        item->setData(RecentNoteRole, QString::fromStdString(tx.comment));
        m_transactionsList->setItemWidget(item, itemWidget);
    }

    // Show/hide based on whether we have transactions
    bool hasTransactions = (m_transactionsList->count() > 0);
    m_recentTransactionsInfo->setVisible(!hasTransactions);
    m_transactionsList->setVisible(hasTransactions);
}

void DigiDollarOverviewWidget::activateRecentTransaction(QListWidgetItem* item)
{
    if (!item) {
        return;
    }

    const QString txid = item->data(RecentTxIdRole).toString();
    if (txid.isEmpty()) {
        return;
    }
    Q_EMIT recentTransactionActivated(txid);
}

QString DigiDollarOverviewWidget::formatDDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $DD";
}

QString DigiDollarOverviewWidget::formatDGBAmount(double amount) const
{
    return QString::number(amount, 'f', 8) + " DGB";
}

QString DigiDollarOverviewWidget::formatUSDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $USD";
}

void DigiDollarOverviewWidget::setMonospacedFont(bool use_embedded_font)
{
    QFont f = GUIUtil::fixedPitchFont(use_embedded_font);
    f.setWeight(QFont::Bold);

    // Apply to all value labels
    m_ddBalanceValue->setFont(f);
    m_ddPaymasterReservedValue->setFont(f);
    m_ddWalletTotalValue->setFont(f);
    m_ddPendingValue->setFont(f);
    m_dgbCollateralValue->setFont(f);
    m_usdValueValue->setFont(f);
    m_oraclePriceValue->setFont(f);
    m_networkTotalDDValue->setFont(f);
    m_networkTotalCollateralValue->setFont(f);
    updateBlockchainTotalsMinimumWidth();
}

void DigiDollarOverviewWidget::updateBlockchainTotalsMinimumWidth()
{
    if (!m_networkTotalDDValue || !m_networkTotalCollateralValue) return;

    const int ddWidth = QFontMetrics(m_networkTotalDDValue->font()).horizontalAdvance(MAX_EXPECTED_BLOCKCHAIN_DD_SUPPLY);
    const int dgbWidth = QFontMetrics(m_networkTotalCollateralValue->font()).horizontalAdvance(MAX_EXPECTED_BLOCKCHAIN_DGB_LOCKED);
    const int valueWidth = std::max(ddWidth, dgbWidth) + TOTALS_VALUE_HORIZONTAL_PADDING;

    m_networkTotalDDValue->setMinimumWidth(valueWidth);
    m_networkTotalCollateralValue->setMinimumWidth(valueWidth);

    if (QWidget* totalsFrame = m_networkTotalDDValue->parentWidget()) {
        totalsFrame->setMinimumWidth(valueWidth + TOTALS_FRAME_HORIZONTAL_PADDING);
    }
}

// REMOVED: updateTheme() and applyTheme() methods
// All theming is now handled by light.css and dark.css files
// This allows the DigiByte blue theme to work properly

void DigiDollarOverviewWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;

    if (m_privacy) {
        // Directly mask all balance labels (bypass updateBalance's IBD/throttle checks)
        m_ddBalanceValue->setText(maskValue(formatDDAmount(0)));
        m_ddPaymasterReservedValue->setText(maskValue(formatDDAmount(0)));
        m_ddWalletTotalValue->setText(maskValue(formatDDAmount(0)));
        m_ddPendingValue->setText(maskValue(formatDDAmount(0)));
        m_ddPendingLabel->setVisible(false);
        m_ddPendingValue->setVisible(false);
        m_dgbCollateralValue->setText(maskValue(formatDGBAmount(0)));
        m_usdValueValue->setText(maskValue(formatUSDAmount(0)));

        // Mask blockchain status values
        m_networkTotalDDValue->setTextFormat(Qt::PlainText);
        m_networkTotalDDValue->setText(maskValue(formatDDAmount(0)));
        m_networkTotalCollateralValue->setTextFormat(Qt::PlainText);
        m_networkTotalCollateralValue->setText(maskValue(formatDGBAmount(0)));
    } else {
        // Directly set real values (bypass updateBalance's IBD/throttle checks)
        m_ddBalanceValue->setText(formatDDAmount(m_ddBalance));
        m_ddPaymasterReservedValue->setText(formatDDAmount(m_ddPaymasterReserved));
        m_ddWalletTotalValue->setText(formatDDAmount(m_ddWalletTotal));
        m_ddPendingValue->setText(formatDDAmount(m_ddPending));
        const bool has_pending = m_ddPending > 0.0;
        m_ddPendingLabel->setVisible(has_pending);
        m_ddPendingValue->setVisible(has_pending);
        m_dgbCollateralValue->setText(formatDGBAmount(m_dgbCollateral));
        const double usdValue = m_ddWalletTotal;
        m_usdValueValue->setText(formatUSDAmount(usdValue));
        // Refresh blockchain stats
        updateSystemHealth();
    }

    // Hide recent transactions list when masked
    m_transactionsList->setVisible(!m_privacy);
    m_transactionsFrame->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the DigiDollar Overview tab. To unmask the values, uncheck Settings->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

QString DigiDollarOverviewWidget::maskValue(const QString& value) const
{
    Q_UNUSED(value);
    return QStringLiteral("########");
}

void DigiDollarOverviewWidget::addDemoTransactions()
{
    // Demo transactions removed - only real transactions from wallet will be shown
    // Real transactions are loaded via wallet notification system
}
