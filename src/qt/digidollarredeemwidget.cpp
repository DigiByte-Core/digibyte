// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarredeemwidget.h>

#include <qt/digidollarsendwidget.h> // For AmountValidator
#include <qt/digidollarcoincontroldialog.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/digibyteunits.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <consensus/amount.h>
#include <consensus/err.h>
#include <univalue.h>
#include <logging.h>

#include <algorithm>
#include <cmath>

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QProgressBar>
#include <QFont>
#include <QMessageBox>
#include <QRegularExpression>
#include <QApplication>
#include <QPalette>
#include <QTimer>

namespace {

double CalculateRequiredDDBurnDisplayAmount(double original_dd, int system_health)
{
    if (system_health >= 100) return original_dd;

    const CAmount original_cents = static_cast<CAmount>(std::llround(original_dd * 100.0));
    const CAmount required_cents = DigiDollar::ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(
        original_cents, system_health);
    return static_cast<double>(required_cents) / 100.0;
}

// DD-FA-FUNC-030 (Wave 19): Redeem widget originally rendered the lock tier as
// "Tier 1" / "Tier 9", which prevents users from cross-checking the redemption
// confirmation against the human-readable label they originally selected at
// mint time ("30 days", "10 years"). The string set below MUST stay in sync
// with `digidollarmintwidget.cpp::getLockTierDisplayName` and the positions
// widget's `lockPeriodName` switch.
QString FormatLockTierLabelForRedeem(int tier)
{
    switch (tier) {
        case 0: return QObject::tr("1 hour");
        case 1: return QObject::tr("30 days");
        case 2: return QObject::tr("3 months");
        case 3: return QObject::tr("6 months");
        case 4: return QObject::tr("1 year");
        case 5: return QObject::tr("2 years");
        case 6: return QObject::tr("3 years");
        case 7: return QObject::tr("5 years");
        case 8: return QObject::tr("7 years");
        case 9: return QObject::tr("10 years");
        default: return QObject::tr("Unknown tier %1").arg(tier);
    }
}

} // namespace

DigiDollarRedeemWidget::DigiDollarRedeemWidget(QWidget *parent) :
    QWidget(parent),
    m_mainLayout(nullptr),
    m_coinControlFrame(nullptr),
    m_coinControlLayout(nullptr),
    m_coinControlButton(nullptr),
    m_coinControlQuantityLabel(nullptr),
    m_coinControlAmountLabel(nullptr),
    m_positionFrame(nullptr),
    m_positionLayout(nullptr),
    m_positionIdLabel(nullptr),
    m_positionIdEdit(nullptr),
    m_positionValidationLabel(nullptr),
    m_amountFrame(nullptr),
    m_amountLayout(nullptr),
    m_amountLabel(nullptr),
    m_amountEdit(nullptr),
    m_amountSuffix(nullptr),
    m_redeemableLabel(nullptr),
    m_redeemableValue(nullptr),
    m_positionInfoFrame(nullptr),
    m_positionInfoLayout(nullptr),
    m_positionInfoLabel(nullptr),
    m_ddMintedLabel(nullptr),
    m_ddMintedValue(nullptr),
    m_dgbCollateralLabel(nullptr),
    m_dgbCollateralValue(nullptr),
    m_lockTierLabel(nullptr),
    m_lockTierValue(nullptr),
    m_timeRemainingLabel(nullptr),
    m_timeRemainingValue(nullptr),
    m_healthStatusLabel(nullptr),
    m_healthStatusValue(nullptr),
    m_healthBar(nullptr),
    m_buttonFrame(nullptr),
    m_buttonLayout(nullptr),
    m_redeemButton(nullptr),
    // m_redeemAllButton(nullptr),  // REMOVED - exact-amount redemption only
    m_clearButton(nullptr),
    m_amountValidator(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr),
    m_selectedPositionId(""),
    m_positionDDMinted(0.0),
    m_positionDGBCollateral(0.0),
    m_positionLockTier(0),
    m_positionBlocksRemaining(0),
    m_positionHealth(100.0),
    m_redeemableAmount(0.0),
    m_positionFound(false)
{
    setupUI();
    connectSignals();
    // REMOVED: applyTheme() - Let CSS handle all theming
}

DigiDollarRedeemWidget::~DigiDollarRedeemWidget()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarRedeemWidget::setupUI()
{
    // Create main layout - compact like DGB tabs
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Create validators
    // DD amounts are in dollars with max 2 decimal places (cents precision)
    m_amountValidator = new AmountValidator(1.00, 100000.00, 2, this);

    // Setup sections
    setupCoinControlSection();
    setupPositionSection();
    setupAmountSection();
    setupPositionInfoSection();
    setupButtonSection();

    // Add stretch to push content to top
    m_mainLayout->addStretch();

    setLayout(m_mainLayout);
}

void DigiDollarRedeemWidget::setupCoinControlSection()
{
    // Create coin control frame
    m_coinControlFrame = new QFrame(this);
    m_coinControlFrame->setObjectName("coinControlFrame");
    m_coinControlFrame->setFrameStyle(QFrame::StyledPanel);
    m_coinControlFrame->setFrameShadow(QFrame::Sunken);

    m_coinControlLayout = new QHBoxLayout(m_coinControlFrame);
    m_coinControlLayout->setSpacing(6);
    m_coinControlLayout->setContentsMargins(6, 4, 6, 4);

    // "Inputs..." button to open coin control dialog
    m_coinControlButton = new QPushButton(tr("Inputs..."), this);
    m_coinControlButton->setObjectName("coinControlButton");
    m_coinControlButton->setToolTip(tr("Manually select $DD inputs to burn for redemption"));
    m_coinControlButton->setMinimumWidth(80);
    m_coinControlLayout->addWidget(m_coinControlButton);

    // Quantity label (number of selected inputs)
    m_coinControlQuantityLabel = new QLabel(this);
    m_coinControlQuantityLabel->setObjectName("coinControlQuantityLabel");
    m_coinControlQuantityLabel->setText(tr("Inputs: (auto)"));
    m_coinControlLayout->addWidget(m_coinControlQuantityLabel);

    // Amount label (total selected DD amount)
    m_coinControlAmountLabel = new QLabel(this);
    m_coinControlAmountLabel->setObjectName("coinControlAmountLabel");
    m_coinControlAmountLabel->setText(QString());
    m_coinControlLayout->addWidget(m_coinControlAmountLabel);

    m_coinControlLayout->addStretch();

    m_mainLayout->addWidget(m_coinControlFrame);

    // Initially visible
    m_coinControlFrame->setVisible(true);
}

void DigiDollarRedeemWidget::setupPositionSection()
{
    // Create position frame
    m_positionFrame = new QFrame(this);
    m_positionFrame->setFrameStyle(QFrame::StyledPanel);
    m_positionFrame->setObjectName("positionFrame");

    m_positionLayout = new QGridLayout(m_positionFrame);
    m_positionLayout->setSpacing(4);
    m_positionLayout->setContentsMargins(6, 6, 6, 6);

    // Title
    QLabel* positionTitle = new QLabel(tr("Select Time Lock"), this);
    QFont titleFont = positionTitle->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    positionTitle->setFont(titleFont);
    m_positionLayout->addWidget(positionTitle, 0, 0, 1, 2);

    // Vault ID input
    m_positionIdLabel = new QLabel(tr("Vault ID:"), this);
    m_positionIdEdit = new QLineEdit(this);
    m_positionIdEdit->setObjectName("positionIdEdit");
    m_positionIdEdit->setPlaceholderText("Enter Vault ID (e.g., a1b2c3d4...)");
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_positionIdEdit->setFont(monospaceFont);

    m_positionLayout->addWidget(m_positionIdLabel, 1, 0);
    m_positionLayout->addWidget(m_positionIdEdit, 1, 1);

    // Validation label
    m_positionValidationLabel = new QLabel(this);
    m_positionValidationLabel->setObjectName("positionValidationLabel");
    // Theme styling will be applied in applyTheme()
    m_positionValidationLabel->setText(tr("Enter a Vault ID to load details"));
    m_positionLayout->addWidget(m_positionValidationLabel, 2, 0, 1, 2);

    m_mainLayout->addWidget(m_positionFrame);
}

void DigiDollarRedeemWidget::setupAmountSection()
{
    // Create amount frame
    m_amountFrame = new QFrame(this);
    m_amountFrame->setFrameStyle(QFrame::StyledPanel);
    m_amountFrame->setObjectName("amountFrame");

    m_amountLayout = new QGridLayout(m_amountFrame);
    m_amountLayout->setSpacing(4);
    m_amountLayout->setContentsMargins(6, 6, 6, 6);

    // Title
    QLabel* amountTitle = new QLabel(tr("Redeem Amount"), this);
    QFont titleFont = amountTitle->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    amountTitle->setFont(titleFont);
    m_amountLayout->addWidget(amountTitle, 0, 0, 1, 3);

    // Amount input
    m_amountLabel = new QLabel(tr("$DD to Redeem:"), this);
    m_amountEdit = new QLineEdit(this);
    m_amountEdit->setObjectName("amountEdit");
    m_amountEdit->setReadOnly(true);  // Make read-only - exact amount only
    m_amountEdit->setPlaceholderText("Select a vault to auto-fill amount");
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_amountEdit->setFont(monospaceFont);

    m_amountSuffix = new QLabel("$DD", this);
    m_amountSuffix->setObjectName("amountSuffix");

    m_amountLayout->addWidget(m_amountLabel, 1, 0);
    m_amountLayout->addWidget(m_amountEdit, 1, 1);
    m_amountLayout->addWidget(m_amountSuffix, 1, 2);

    // Redeemable amount
    m_redeemableLabel = new QLabel(tr("Amount to Redeem:"), this);  // Changed from "Max Redeemable"
    m_redeemableLabel->setObjectName("redeemableLabel");
    m_redeemableValue = new QLabel("0.00 $DD", this);
    m_redeemableValue->setObjectName("redeemableValue");
    m_redeemableValue->setFont(monospaceFont);

    m_amountLayout->addWidget(m_redeemableLabel, 2, 0);
    m_amountLayout->addWidget(m_redeemableValue, 2, 1, 1, 2);

    m_mainLayout->addWidget(m_amountFrame);
}

void DigiDollarRedeemWidget::setupPositionInfoSection()
{
    // Create position info frame
    m_positionInfoFrame = new QFrame(this);
    m_positionInfoFrame->setFrameStyle(QFrame::StyledPanel);
    m_positionInfoFrame->setObjectName("positionInfoFrame");

    m_positionInfoLayout = new QGridLayout(m_positionInfoFrame);
    m_positionInfoLayout->setSpacing(4);
    m_positionInfoLayout->setContentsMargins(6, 6, 6, 6);

    // Title
    m_positionInfoLabel = new QLabel(tr("Time Lock Details"), this);
    QFont titleFont = m_positionInfoLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    m_positionInfoLabel->setFont(titleFont);
    m_positionInfoLayout->addWidget(m_positionInfoLabel, 0, 0, 1, 2);

    QFont monospaceFont = GUIUtil::fixedPitchFont();

    // DD Minted
    m_ddMintedLabel = new QLabel(tr("$DD Minted:"), this);
    m_ddMintedValue = new QLabel("0.00 $DD", this);
    m_ddMintedValue->setObjectName("ddMintedValue");
    m_ddMintedValue->setFont(monospaceFont);
    m_positionInfoLayout->addWidget(m_ddMintedLabel, 1, 0);
    m_positionInfoLayout->addWidget(m_ddMintedValue, 1, 1);

    // DGB Collateral
    m_dgbCollateralLabel = new QLabel(tr("DGB Collateral:"), this);
    m_dgbCollateralValue = new QLabel("0.00000000 DGB", this);
    m_dgbCollateralValue->setFont(monospaceFont);
    m_positionInfoLayout->addWidget(m_dgbCollateralLabel, 2, 0);
    m_positionInfoLayout->addWidget(m_dgbCollateralValue, 2, 1);

    // Lock Tier
    m_lockTierLabel = new QLabel(tr("Lock Tier:"), this);
    m_lockTierValue = new QLabel("N/A", this);
    m_lockTierValue->setObjectName("lockTierValue");
    m_lockTierValue->setFont(monospaceFont);
    m_positionInfoLayout->addWidget(m_lockTierLabel, 3, 0);
    m_positionInfoLayout->addWidget(m_lockTierValue, 3, 1);

    // Time Remaining
    m_timeRemainingLabel = new QLabel(tr("Time Remaining:"), this);
    m_timeRemainingValue = new QLabel("N/A", this);
    m_timeRemainingValue->setFont(monospaceFont);
    m_positionInfoLayout->addWidget(m_timeRemainingLabel, 4, 0);
    m_positionInfoLayout->addWidget(m_timeRemainingValue, 4, 1);

    // Health Status
    m_healthStatusLabel = new QLabel(tr("Health:"), this);
    m_healthStatusValue = new QLabel("N/A", this);
    m_healthStatusValue->setFont(monospaceFont);
    m_positionInfoLayout->addWidget(m_healthStatusLabel, 5, 0);
    m_positionInfoLayout->addWidget(m_healthStatusValue, 5, 1);

    // Health progress bar
    m_healthBar = new QProgressBar(this);
    m_healthBar->setObjectName("healthBar");
    m_healthBar->setRange(0, 100);
    m_healthBar->setValue(0);
    m_healthBar->setFormat("%v%");
    m_positionInfoLayout->addWidget(m_healthBar, 6, 0, 1, 2);

    m_mainLayout->addWidget(m_positionInfoFrame);
}

void DigiDollarRedeemWidget::setupButtonSection()
{
    // Create button frame
    m_buttonFrame = new QFrame(this);
    m_buttonFrame->setObjectName("buttonFrame");

    m_buttonLayout = new QHBoxLayout(m_buttonFrame);
    m_buttonLayout->setSpacing(6);
    m_buttonLayout->setContentsMargins(6, 6, 6, 6);

    // Clear button
    m_clearButton = new QPushButton(tr("Clear"), this);
    m_clearButton->setObjectName("clearButton");
    m_buttonLayout->addWidget(m_clearButton);

    // Add stretch
    m_buttonLayout->addStretch();

    // Redeem all button - REMOVED (redundant with exact-amount redemption)
    // m_redeemAllButton = new QPushButton(tr("Redeem All"), this);
    // m_redeemAllButton->setObjectName("redeemAllButton");
    // m_redeemAllButton->setEnabled(false);
    // m_buttonLayout->addWidget(m_redeemAllButton);

    // Redeem button
    m_redeemButton = new QPushButton(tr("Cannot Redeem"), this);
    m_redeemButton->setObjectName("redeemButton");
    m_redeemButton->setEnabled(false);
    m_redeemButton->setToolTip(redeemDisabledReason());
    // Theme will be applied in applyTheme()
    m_buttonLayout->addWidget(m_redeemButton);

    m_mainLayout->addWidget(m_buttonFrame);
}

void DigiDollarRedeemWidget::connectSignals()
{
    // Connect position ID validation
    connect(m_positionIdEdit, &QLineEdit::textChanged,
            this, &DigiDollarRedeemWidget::onPositionIdChanged);

    // Connect amount validation
    connect(m_amountEdit, &QLineEdit::textChanged,
            this, &DigiDollarRedeemWidget::onAmountChanged);

    // Connect buttons
    connect(m_redeemButton, &QPushButton::clicked,
            this, &DigiDollarRedeemWidget::onRedeemClicked);
    // Redeem All button removed - exact-amount redemption only
    // connect(m_redeemAllButton, &QPushButton::clicked,
    //         this, &DigiDollarRedeemWidget::onRedeemAllClicked);
    connect(m_clearButton, &QPushButton::clicked,
            this, &DigiDollarRedeemWidget::onClearClicked);

    // Connect coin control button
    connect(m_coinControlButton, &QPushButton::clicked,
            this, &DigiDollarRedeemWidget::onCoinControlButtonClicked);

    // Auto-refresh position details every 15 seconds so health/status stays current
    QTimer* positionRefreshTimer = new QTimer(this);
    connect(positionRefreshTimer, &QTimer::timeout, this, [this]() {
        if (m_positionFound && !m_selectedPositionId.isEmpty()) {
            loadPositionDetails();
            updatePositionInfo();
            updateRedeemButtons();
        }
    });
    positionRefreshTimer->start(15000); // 15 seconds
}

void DigiDollarRedeemWidget::setWalletModel(WalletModel* model)
{
    m_walletModel = model;

    if (m_walletModel) {
        // Connect wallet model signals
        connect(m_walletModel, &WalletModel::encryptionStatusChanged, this, [this]() {
            updateRedeemButtons();
        });
        updateBalance();
        updatePositions();
        // REMOVED: applyTheme() - Let CSS handle all theming
    }
}

void DigiDollarRedeemWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    if (m_clientModel) {
        // Connect client model signals
        updatePositions();
        // REMOVED: applyTheme() - Let CSS handle all theming
    }
}

void DigiDollarRedeemWidget::updateView()
{
    updateBalance();
    updatePositions();
    updatePositionInfo();
}

void DigiDollarRedeemWidget::setPosition(const QString& outpoint)
{
    m_selectedPositionId = outpoint;
    m_positionIdEdit->setText(outpoint);
    loadPositionDetails();
    updatePositionInfo();
    updateRedeemButtons();
}

void DigiDollarRedeemWidget::updateBalance()
{
    // In a real implementation, this would query the wallet for DigiDollar balance
    if (m_walletModel) {
        // TODO: Update balance displays if needed
    }
}

void DigiDollarRedeemWidget::updatePositions()
{
    if (!isVisible()) return;
    // In a real implementation, this would refresh position data
    if (m_walletModel && !m_selectedPositionId.isEmpty()) {
        loadPositionDetails();
    }
}

void DigiDollarRedeemWidget::onPositionIdChanged()
{
    QString positionId = m_positionIdEdit->text().trimmed();
    m_selectedPositionId = positionId;

    if (positionId.isEmpty()) {
        m_positionValidationLabel->setText(tr("Enter a position ID to load details"));
        updateValidationLabels();
        m_positionFound = false;
    } else if (validatePositionId()) {
        loadPositionDetails();
        if (m_positionFound) {
            m_positionValidationLabel->setText(tr("✓ Position found and loaded"));
            updateValidationLabels();
        } else {
            m_positionValidationLabel->setText(tr("✗ Position not found"));
            updateValidationLabels();
        }
    } else {
        m_positionValidationLabel->setText(tr("✗ Invalid position ID format"));
        updateValidationLabels();
        m_positionFound = false;
    }

    updatePositionInfo();
    updateRedeemButtons();
}

void DigiDollarRedeemWidget::onAmountChanged()
{
    updateAmountValidation();
    updateRedeemButtons();
}

void DigiDollarRedeemWidget::onRedeemClicked()
{
    if (!validatePositionId() || !validateAmount() || !validateRedeemable()) {
        return;
    }

    QString amountText = m_amountEdit->text();
    double amount = amountText.toDouble();

    // CRITICAL: Check if user has enough DD balance BEFORE showing confirmation dialog
    if (!m_walletModel) {
        Q_EMIT message(tr("Error"), tr("No wallet model available"), QMessageBox::Critical);
        return;
    }

    // Get user's current DD balance (in cents)
    CAmount ddBalanceCents = m_walletModel->getDigiDollarBalance();
    double ddBalance = ddBalanceCents / 100.0;

    // Calculate required DD burn based on system health (ERR check)
    // Need to query system health to determine if ERR is active
    double requiredDDBurn = m_positionDDMinted; // Start with original minted amount

    try {
        // Query system health status via RPC (using getdigidollarstats)
        UniValue params(UniValue::VARR);
        UniValue healthResult = m_walletModel->executeRpc("getdigidollarstats", params);

        if (healthResult.isObject()) {
            int systemHealth = healthResult.find_value("health_percentage").getInt<int>();

            // If system health < 100%, ERR is active and we need MORE DD to redeem
            if (systemHealth < 100) {
                requiredDDBurn = CalculateRequiredDDBurnDisplayAmount(m_positionDDMinted, systemHealth);

                LogPrintf("DigiDollar Qt: ERR active (health: %d%%), required DD burn: %.8f\n",
                         systemHealth, requiredDDBurn);
            }
        }
    } catch (const UniValue& objError) {
        LogPrintf("DigiDollar Qt: Failed to query system health (RPC error) - assuming normal redemption\n");
        // On error, proceed with normal redemption calculation
    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: Failed to query system health - %s (assuming normal redemption)\n", e.what());
        // On error, proceed with normal redemption calculation
    }

    // Check if user has enough DD balance to redeem
    if (ddBalance < requiredDDBurn) {
        QString errorMsg;
        if (requiredDDBurn > m_positionDDMinted) {
            // ERR mode active
            errorMsg = tr("Insufficient DigiDollars for Emergency Redemption.\n\n"
                         "You have: %1\n"
                         "Required: %2\n\n"
                         "System health is below 100%, requiring additional $DD to redeem.\n"
                         "You need %3 more to complete this redemption.")
                .arg(formatDDAmount(ddBalance))
                .arg(formatDDAmount(requiredDDBurn))
                .arg(formatDDAmount(requiredDDBurn - ddBalance));
        } else {
            // Normal redemption
            errorMsg = tr("Insufficient DigiDollars.\n\n"
                         "You have: %1\n"
                         "Required: %2\n\n"
                         "You need %3 more to redeem this position.")
                .arg(formatDDAmount(ddBalance))
                .arg(formatDDAmount(requiredDDBurn))
                .arg(formatDDAmount(requiredDDBurn - ddBalance));
        }

        Q_EMIT message(tr("Insufficient DigiDollar Balance"), errorMsg, QMessageBox::Warning);
        return; // STOP - Do not show confirmation dialog
    }

    // Balance check passed - show confirmation dialog
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Confirm Redeem"));

    QString confirmText;
    if (requiredDDBurn > m_positionDDMinted) {
        // ERR mode - show additional DD burn requirement
        confirmText = tr("Close vault %1 and redeem %2?\n\n"
                        "Emergency Redemption Ratio (ERR) is active.\n"
                        "Required $DD burn: %3")
            .arg(m_selectedPositionId)
            .arg(formatDDAmount(m_positionDDMinted))
            .arg(formatDDAmount(requiredDDBurn));
    } else {
        confirmText = tr("Close vault %1 and redeem %2?")
            .arg(m_selectedPositionId)
            .arg(formatDDAmount(m_positionDDMinted));
    }

    msgBox.setText(confirmText);
    msgBox.setInformativeText(tr("This will close the vault and release all locked DGB collateral."));
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);

    if (msgBox.exec() == QMessageBox::Yes) {
        if (!m_walletModel) {
            Q_EMIT message(tr("Error"), tr("No wallet model available"), QMessageBox::Critical);
            return;
        }

        // Unlock wallet if encrypted (shows passphrase dialog)
        WalletModel::UnlockContext ctx(m_walletModel->requestUnlock());
        if (!ctx.isValid()) {
            // User cancelled the unlock dialog
            return;
        }

        // Convert amount from double to CAmount (cents) — use llround to avoid truncation
        CAmount amountCents = static_cast<CAmount>(std::llround(amount * 100));

        // Call the wallet model to redeem DigiDollar
        WalletModel::DigiDollarRedeemResult result = m_walletModel->redeemDigiDollar(m_selectedPositionId, amountCents, "");

        if (result.status == WalletModel::OK) {
            Q_EMIT message(tr("Redeem Transaction Created"),
                        tr("DigiDollar redeem transaction created successfully!\n\nTransaction ID: %1")
                        .arg(result.txid),
                        QMessageBox::Information);
            Q_EMIT redemptionCompleted(); // Notify other widgets
            onClearClicked();
            updateBalance(); // Refresh balance displays
            updatePositions(); // Refresh positions
        } else {
            QString errorTitle;
            QString errorMessage = result.reasonFailed;

            switch (result.status) {
            case WalletModel::InvalidAddress:
                errorTitle = tr("Invalid Position");
                break;
            case WalletModel::InvalidAmount:
                errorTitle = tr("Invalid Amount");
                break;
            case WalletModel::AmountExceedsBalance:
                errorTitle = tr("Insufficient Redeemable Amount");
                break;
            case WalletModel::TransactionCreationFailed:
                errorTitle = tr("Transaction Failed");
                break;
            default:
                errorTitle = tr("Redeem Error");
                break;
            }

            Q_EMIT message(errorTitle, errorMessage, QMessageBox::Critical);
        }
    }
}

// REMOVED: onRedeemAllClicked() - Redundant with exact-amount redemption
// void DigiDollarRedeemWidget::onRedeemAllClicked()
// {
//     if (!m_positionFound || m_redeemableAmount <= 0) {
//         return;
//     }
//
//     // In a real implementation, this would create and broadcast the full redeem transaction
//     QMessageBox msgBox(this);
//     msgBox.setWindowTitle(tr("Confirm Redeem All"));
//     msgBox.setText(tr("Redeem entire position %1?")
//                   .arg(m_selectedPositionId));
//     msgBox.setInformativeText(tr("Amount: %1\nThis will close the position and release all collateral.")
//                              .arg(formatDDAmount(m_redeemableAmount)));
//     msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
//     msgBox.setDefaultButton(QMessageBox::No);
//
//     if (msgBox.exec() == QMessageBox::Yes) {
//         if (!m_walletModel) {
//             Q_EMIT message(tr("Error"), tr("No wallet model available"), QMessageBox::Critical);
//             return;
//         }
//
//         // Convert full redeemable amount from double to CAmount (cents)
//         CAmount amountCents = static_cast<CAmount>(m_redeemableAmount * 100);
//
//         // Call the wallet model to redeem the full position
//         WalletModel::DigiDollarRedeemResult result = m_walletModel->redeemDigiDollar(m_selectedPositionId, amountCents, "");
//
//         if (result.status == WalletModel::OK) {
//             Q_EMIT message(tr("Position Closed"),
//                         tr("DigiDollar position closed successfully!\n\nTransaction ID: %1")
//                         .arg(result.txid),
//                         QMessageBox::Information);
//             Q_EMIT redemptionCompleted(); // Notify other widgets
//             onClearClicked();
//             updateBalance(); // Refresh balance displays
//             updatePositions(); // Refresh positions
//         } else {
//             QString errorTitle;
//             QString errorMessage = result.reasonFailed;
//
//             switch (result.status) {
//             case WalletModel::InvalidAddress:
//                 errorTitle = tr("Invalid Position");
//                 break;
//             case WalletModel::InvalidAmount:
//                 errorTitle = tr("Invalid Amount");
//                 break;
//             case WalletModel::AmountExceedsBalance:
//                 errorTitle = tr("Insufficient Redeemable Amount");
//                 break;
//             case WalletModel::TransactionCreationFailed:
//                 errorTitle = tr("Transaction Failed");
//                 break;
//             default:
//                 errorTitle = tr("Redeem Error");
//                 break;
//             }
//
//             Q_EMIT message(errorTitle, errorMessage, QMessageBox::Critical);
//         }
//     }
// }

void DigiDollarRedeemWidget::onClearClicked()
{
    m_positionIdEdit->clear();
    m_amountEdit->clear();
    onPositionIdChanged(); // Reset position validation
    onAmountChanged();     // Reset amount validation
}

void DigiDollarRedeemWidget::updateRedeemButtons()
{
    const bool canRedeem =
        m_positionFound &&
        m_positionBlocksRemaining <= 0 &&
        validateAmount() &&
        validateRedeemable() &&
        validateDDBalance() &&
        canWalletSignRedemption();

    // A locked wallet keeps the action available: the redeem flow prompts for
    // the passphrase. Only advertise the extra step up front.
    const bool needsUnlock = canRedeem && walletNeedsUnlockToRedeem();

    m_redeemButton->setEnabled(canRedeem);
    if (canRedeem) {
        m_redeemButton->setText(needsUnlock ? tr("Unlock && Redeem") : tr("Redeem && Unlock DGB"));
        const QString readyText = needsUnlock
            ? tr("Wallet is locked.\nYou will be asked for your passphrase, then this DigiDollar vault is redeemed and the locked DGB collateral released.")
            : tr("Ready to redeem this DigiDollar vault and release the locked DGB collateral.");
        m_redeemButton->setToolTip(readyText);
        m_positionValidationLabel->setText(needsUnlock
            ? tr("Vault ready to redeem — you will be asked for your passphrase.")
            : tr("Vault ready to redeem."));
        m_positionValidationLabel->setToolTip(readyText);
    } else {
        const QString reason = redeemDisabledReason();
        m_redeemButton->setText(tr("Cannot Redeem"));
        m_redeemButton->setToolTip(reason);
        if (m_positionFound || !m_positionIdEdit->text().trimmed().isEmpty()) {
            m_positionValidationLabel->setText(reason.section('\n', 0, 0));
            m_positionValidationLabel->setToolTip(reason);
        }
    }
    // m_redeemAllButton removed - exact-amount redemption only
}

void DigiDollarRedeemWidget::updatePositionInfo()
{
    if (m_positionFound) {
        if (m_privacy) {
            m_ddMintedValue->setText(maskValue(formatDDAmount(0)));
            m_dgbCollateralValue->setText(maskValue(formatDGBAmount(0)));
            m_lockTierValue->setText(FormatLockTierLabelForRedeem(m_positionLockTier)); // Tier is not sensitive
            m_timeRemainingValue->setText(formatBlockTime(m_positionBlocksRemaining)); // Time is not sensitive
            m_healthStatusValue->setText(QString("%1%").arg(QString::number(m_positionHealth, 'f', 1))); // Health is not sensitive
            m_redeemableValue->setText(maskValue(formatDDAmount(0)));
        } else {
            m_ddMintedValue->setText(formatDDAmount(m_positionDDMinted));
            m_dgbCollateralValue->setText(formatDGBAmount(m_positionDGBCollateral));
            m_lockTierValue->setText(FormatLockTierLabelForRedeem(m_positionLockTier));
            m_timeRemainingValue->setText(formatBlockTime(m_positionBlocksRemaining));
            m_healthStatusValue->setText(QString("%1%").arg(QString::number(m_positionHealth, 'f', 1)));
            m_redeemableValue->setText(formatDDAmount(m_redeemableAmount));
        }

        // Update health bar
        m_healthBar->setValue(static_cast<int>(m_positionHealth));

        // REMOVED: All health bar and status color coding - Let CSS handle theming
    } else {
        // Reset to default values
        if (m_privacy) {
            m_ddMintedValue->setText(maskValue(formatDDAmount(0)));
            m_dgbCollateralValue->setText(maskValue(formatDGBAmount(0)));
            m_redeemableValue->setText(maskValue(formatDDAmount(0)));
        } else {
            m_ddMintedValue->setText(formatDDAmount(0));
            m_dgbCollateralValue->setText(formatDGBAmount(0));
            m_redeemableValue->setText(formatDDAmount(0));
        }
        m_lockTierValue->setText("N/A");
        m_timeRemainingValue->setText("N/A");
        m_healthStatusValue->setText("N/A");
        m_healthBar->setValue(0);
    }
}

void DigiDollarRedeemWidget::loadPositionDetails()
{
    // Query the wallet for position details
    if (!m_walletModel || m_selectedPositionId.isEmpty()) {
        m_positionFound = false;
        m_positionDDMinted = 0.0;
        m_positionDGBCollateral = 0.0;
        m_positionLockTier = 0;
        m_positionBlocksRemaining = 0;
        m_positionHealth = 0.0;
        m_redeemableAmount = 0.0;
        return;
    }

    // Convert position ID string to uint256
    uint256 positionId;
    positionId.SetHex(m_selectedPositionId.toStdString());
    if (positionId.IsNull()) {
        m_positionFound = false;
        return;
    }

    auto loadPositionFromWallet = [&]() -> bool {
        DigiDollarWallet* ddWallet = m_walletModel->wallet().getDigiDollarWallet();
        if (!ddWallet) return false;
        // ReconcilePositionStates() only reads the wallet's transactions and the
        // chain UTXO set and rewrites the is_active flag; it never touches key
        // material, so a locked wallet must still refresh.
        const bool walletIsWatchOnly = m_walletModel->wallet().privateKeysDisabled();
        if (!walletIsWatchOnly) {
            ddWallet->ReconcilePositionStates();
        }

        const int currentHeight = m_clientModel ? m_clientModel->getNumBlocks() : 0;
        for (const auto& pos : ddWallet->GetDDTimeLocks(false)) {
            if (pos.dd_timelock_id != positionId) continue;

            m_positionFound = true;
            m_positionDDMinted = pos.dd_minted / 100.0;
            m_positionDGBCollateral = pos.dgb_collateral / static_cast<double>(COIN);
            m_positionLockTier = static_cast<int>(pos.lock_tier);
            m_positionBlocksRemaining = std::max<int64_t>(0, pos.unlock_height - currentHeight);
            m_positionHealth = 0.0;
            m_redeemableAmount = m_positionBlocksRemaining <= 0 ? m_positionDDMinted : 0.0;
            m_amountEdit->setText(m_positionBlocksRemaining <= 0 ? QString::number(m_positionDDMinted, 'f', 2) : QString());
            return true;
        }
        return false;
    };

    // Query position from RPC
    try {
        UniValue params(UniValue::VARR);
        params.push_back(false); // active_only = false (show all positions)

        UniValue result = m_walletModel->executeRpc("listdigidollarpositions", params);

        if (result.isArray()) {
            m_positionFound = false;
            for (size_t i = 0; i < result.size(); i++) {
                const UniValue& pos = result[i];
                std::string pid = pos.find_value("position_id").get_str();

                if (pid == m_selectedPositionId.toStdString()) {
                    // Found the position!
                    m_positionFound = true;
                    m_positionDDMinted = pos.find_value("dd_minted").getInt<int64_t>() / 100.0; // cents to DD
                    const UniValue& collateral = pos.find_value("dgb_collateral");
                    if (collateral.isStr()) {
                        bool ok = false;
                        m_positionDGBCollateral = QString::fromStdString(collateral.get_str()).toDouble(&ok);
                        if (!ok) throw std::runtime_error("invalid dgb_collateral amount");
                    } else {
                        m_positionDGBCollateral = collateral.get_real();
                    }
                    m_positionLockTier = pos.find_value("lock_tier").getInt<int>();
                    m_positionBlocksRemaining = pos.find_value("blocks_remaining").getInt<int>();
                    m_positionHealth = pos.find_value("health_ratio").get_real();
                    m_redeemableAmount = m_positionBlocksRemaining <= 0 ? m_positionDDMinted : 0.0;
                    // Auto-fill only when normal redemption is actually available.
                    m_amountEdit->setText(m_positionBlocksRemaining <= 0 ? QString::number(m_positionDDMinted, 'f', 2) : QString());
                    break;
                }
            }

            if (!m_positionFound) {
                if (!loadPositionFromWallet()) {
                    // Position not found in list
                    m_positionDDMinted = 0.0;
                    m_positionDGBCollateral = 0.0;
                    m_positionLockTier = 0;
                    m_positionBlocksRemaining = 0;
                    m_positionHealth = 0.0;
                    m_redeemableAmount = 0.0;
                }
            }
        }
    } catch (const UniValue& e) {
        LogPrintf("DigiDollar Qt: Failed to load position details (RPC) - %s\n", e.write());
        if (!loadPositionFromWallet()) {
            m_positionFound = false;
            m_positionDDMinted = 0.0;
            m_positionDGBCollateral = 0.0;
            m_positionLockTier = 0;
            m_positionBlocksRemaining = 0;
            m_positionHealth = 0.0;
            m_redeemableAmount = 0.0;
        }
    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: Failed to load position details - %s\n", e.what());
        if (!loadPositionFromWallet()) {
            m_positionFound = false;
            m_positionDDMinted = 0.0;
            m_positionDGBCollateral = 0.0;
            m_positionLockTier = 0;
            m_positionBlocksRemaining = 0;
            m_positionHealth = 0.0;
            m_redeemableAmount = 0.0;
        }
    }
}

bool DigiDollarRedeemWidget::validatePositionId() const
{
    QString positionId = m_positionIdEdit->text().trimmed();
    if (positionId.isEmpty()) return false;

    // Basic validation for position ID format (hexadecimal, at least 8 characters)
    QRegularExpression hexRegex("^[0-9a-fA-F]{8,}$");
    return hexRegex.match(positionId).hasMatch();
}

bool DigiDollarRedeemWidget::validateAmount() const
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return false;

    int pos = 0;
    QString amountCopy = amountText;
    return m_amountValidator->validate(amountCopy, pos) == QValidator::Acceptable;
}

bool DigiDollarRedeemWidget::validateRedeemable() const
{
    if (!m_positionFound) return false;
    if (m_positionBlocksRemaining > 0 || m_redeemableAmount <= 0.0) return false;

    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return false;

    double amount = amountText.toDouble();
    // ENFORCE EXACT MATCH - allow tiny floating point tolerance
    double tolerance = 0.00000001;  // 1 satoshi tolerance
    return std::abs(amount - m_redeemableAmount) < tolerance;
}

bool DigiDollarRedeemWidget::validateDDBalance() const
{
    if (!m_positionFound || !m_walletModel) {
        return false;
    }
    if (m_positionBlocksRemaining > 0) {
        return false;
    }

    // Get user's current DD balance (in cents)
    CAmount ddBalanceCents = m_walletModel->getDigiDollarBalance();
    double ddBalance = ddBalanceCents / 100.0;

    // Calculate required DD burn based on system health
    double requiredDDBurn = m_positionDDMinted; // Default: normal redemption

    try {
        // Query system health status via RPC (using getdigidollarstats)
        UniValue params(UniValue::VARR);
        UniValue healthResult = m_walletModel->executeRpc("getdigidollarstats", params);

        if (healthResult.isObject()) {
            int systemHealth = healthResult.find_value("health_percentage").getInt<int>();

            // If system health < 100%, ERR is active and we need MORE DD to redeem
            if (systemHealth < 100) {
                requiredDDBurn = CalculateRequiredDDBurnDisplayAmount(m_positionDDMinted, systemHealth);
            }
        }
    } catch (const UniValue& objError) {
        // On error, assume normal redemption and allow validation to proceed
        LogPrintf("DigiDollar Qt: Failed to query system health in validateDDBalance (RPC error)\n");
    } catch (const std::exception& e) {
        // On error, assume normal redemption and allow validation to proceed
        LogPrintf("DigiDollar Qt: Failed to query system health in validateDDBalance - %s\n", e.what());
    }

    // Check if user has enough DD balance
    return ddBalance >= requiredDDBurn;
}

bool DigiDollarRedeemWidget::canWalletSignRedemption() const
{
    if (!m_walletModel) {
        return false;
    }
    // Private keys disabled (watch-only) is a permanent incapability: no
    // passphrase can make this wallet produce a redemption witness.
    //
    // A locked but encrypted wallet is NOT in that category. It still holds the
    // keys, and onRedeemClicked() asks for the passphrase through
    // WalletModel::requestUnlock() before building the transaction, exactly
    // like the send and mint flows do.
    if (m_walletModel->wallet().privateKeysDisabled()) {
        return false;
    }
    return true;
}

bool DigiDollarRedeemWidget::walletNeedsUnlockToRedeem() const
{
    return m_walletModel && m_walletModel->getEncryptionStatus() == WalletModel::Locked;
}

QString DigiDollarRedeemWidget::redeemDisabledReason() const
{
    if (!m_positionFound) {
        return tr("Select a DigiDollar vault to redeem.");
    }
    if (m_positionBlocksRemaining > 0) {
        return tr("Vault is still locked.\nTime remaining: %1\nBlocks remaining: %2")
            .arg(formatBlockTime(m_positionBlocksRemaining))
            .arg(m_positionBlocksRemaining);
    }
    if (!m_walletModel) {
        return tr("Wallet is not available.");
    }
    if (m_walletModel->wallet().privateKeysDisabled()) {
        return tr("Watch-only wallet.\nThis wallet cannot sign DigiDollar redemptions because private keys are disabled.");
    }
    if (!validateAmount()) {
        return tr("Invalid redeem amount.\nDigiDollar redemptions must use the exact vault amount.");
    }
    if (!validateRedeemable()) {
        return tr("Amount must match the full redeemable DigiDollar amount for this vault.");
    }
    if (!validateDDBalance()) {
        return tr("Insufficient DigiDollar balance to burn the required $DD for this redemption.");
    }
    return tr("This DigiDollar vault cannot be redeemed yet.");
}

QString DigiDollarRedeemWidget::formatDDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $DD";
}

QString DigiDollarRedeemWidget::formatDGBAmount(double amount) const
{
    return QString::number(amount, 'f', 8) + " DGB";
}

QString DigiDollarRedeemWidget::formatBlockTime(int blocks) const
{
    if (blocks <= 0) return "Expired";

    // Estimate time remaining (15 seconds per block)
    int totalSeconds = blocks * 15;
    int days = totalSeconds / (24 * 3600);
    int hours = (totalSeconds % (24 * 3600)) / 3600;
    int minutes = (totalSeconds % 3600) / 60;

    if (days > 0) {
        return QString("%1d %2h").arg(days).arg(hours);
    } else if (hours > 0) {
        return QString("%1h %2m").arg(hours).arg(minutes);
    } else {
        return QString("%1m").arg(minutes);
    }
}

void DigiDollarRedeemWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    updatePositionInfo();
}

QString DigiDollarRedeemWidget::maskValue(const QString& value) const
{
    QString masked = value;
    for (int i = 0; i < masked.size(); ++i) {
        if (masked[i].isDigit()) {
            masked[i] = '#';
        }
    }
    return masked;
}

// REMOVED: applyTheme() - All styling now handled by CSS files (light.css/dark.css)
// This method was overriding the CSS theme with programmatic styling
void DigiDollarRedeemWidget::applyTheme()
{
    // Method disabled - CSS handles all theming now
}


void DigiDollarRedeemWidget::updateValidationLabels()
{
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
    QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";
    QString infoColor = palette.color(QPalette::Mid).name();

    // Update position validation styling
    QString validationText = m_positionValidationLabel->text();
    if (validationText.contains("✓")) {
        m_positionValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(successColor));
        m_positionIdEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
    } else if (validationText.contains("✗")) {
        m_positionValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(errorColor));
        m_positionIdEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(errorColor));
    } else {
        m_positionValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; }").arg(infoColor));
        m_positionIdEdit->setStyleSheet("");
    }

    // Update amount input styling based on validation
    updateAmountValidation();
}

void DigiDollarRedeemWidget::updateAmountValidation()
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) {
        m_amountEdit->setStyleSheet("");
        return;
    }

    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
    QString warningColor = isDarkTheme ? "#ff9800" : "#ffc107";
    QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";
    QString errorBg = isDarkTheme ? "#4a2c2c" : "#ffeaea";
    QString warningBg = isDarkTheme ? "#4a3d2a" : "#fff3cd";

    bool isValidFormat = validateAmount();
    bool isRedeemable = validateRedeemable();

    if (!isValidFormat) {
        // Invalid format
        m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; background-color: %2; }").arg(errorColor).arg(errorBg));
    } else if (m_positionFound && !isRedeemable) {
        // Valid format but exceeds redeemable amount
        m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; background-color: %2; }").arg(warningColor).arg(warningBg));
    } else if (isValidFormat && isRedeemable) {
        // Valid and redeemable
        m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
    } else {
        m_amountEdit->setStyleSheet("");
    }
}

void DigiDollarRedeemWidget::onCoinControlButtonClicked()
{
    if (!m_walletModel) {
        return;
    }

    // Initialize coin control if not already done
    if (!m_coinControl) {
        m_coinControl = std::make_unique<wallet::DDCoinControl>();
    }

    // Open the coin control dialog
    DigiDollarCoinControlDialog dlg(*m_coinControl, m_walletModel, nullptr, this);
    dlg.exec();

    // Update labels after dialog closes
    updateCoinControlLabels();
}

void DigiDollarRedeemWidget::updateCoinControlLabels()
{
    if (!m_coinControl || !m_walletModel) {
        // No coin control active, show auto message
        if (m_coinControlQuantityLabel) {
            m_coinControlQuantityLabel->setText(tr("Inputs: (auto)"));
            m_coinControlQuantityLabel->setVisible(true);
        }
        if (m_coinControlAmountLabel) {
            m_coinControlAmountLabel->setText(QString());
            m_coinControlAmountLabel->setVisible(false);
        }
        return;
    }

    if (!m_coinControl->HasSelected()) {
        // No inputs selected, show auto message
        if (m_coinControlQuantityLabel) {
            m_coinControlQuantityLabel->setText(tr("Inputs: (auto)"));
            m_coinControlQuantityLabel->setVisible(true);
        }
        if (m_coinControlAmountLabel) {
            m_coinControlAmountLabel->setText(QString());
            m_coinControlAmountLabel->setVisible(false);
        }
        return;
    }

    // Count selected inputs and calculate total amount
    std::vector<COutPoint> selectedInputs = m_coinControl->ListSelected();
    int nQuantity = selectedInputs.size();

    // Show quantity label
    if (m_coinControlQuantityLabel) {
        m_coinControlQuantityLabel->setText(tr("Inputs: %1").arg(nQuantity));
        m_coinControlQuantityLabel->setVisible(true);
    }

    // Show manual selection indicator
    if (m_coinControlAmountLabel) {
        m_coinControlAmountLabel->setText(tr("(manual selection active)"));
        m_coinControlAmountLabel->setVisible(true);
    }
}
