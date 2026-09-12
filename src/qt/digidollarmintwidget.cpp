// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarmintwidget.h>

#include <qt/digidollarsendwidget.h> // For AmountValidator
#include <qt/digidollar_qt_translate.h> // DD-FA-FUNC-032 reject-reason translator
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/digibyteunits.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <digidollar/digidollar.h>
#include <digidollar/txbuilder.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <kernel/chainparams.h>
#include <oracle/mock_oracle.h>
#include <interfaces/node.h>
#include <univalue.h>

#include <algorithm>
#include <cmath>

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QProgressBar>
#include <QFont>
#include <QMessageBox>
#include <QApplication>
#include <QPalette>
#include <QTimer>

DigiDollarMintWidget::DigiDollarMintWidget(QWidget *parent) :
    QWidget(parent),
    m_mainLayout(nullptr),
    m_amountFrame(nullptr),
    m_amountLayout(nullptr),
    m_amountLabel(nullptr),
    m_amountEdit(nullptr),
    m_amountSuffix(nullptr),
    m_usdValueLabel(nullptr),
    m_usdValueValue(nullptr),
    m_amountWarningLabel(nullptr),
    m_lockTierFrame(nullptr),
    m_lockTierLayout(nullptr),
    m_lockTierLabel(nullptr),
    m_lockTierCombo(nullptr),
    m_lockTierInfoLabel(nullptr),
    m_lockTierInfoValue(nullptr),
    m_collateralFrame(nullptr),
    m_collateralLayout(nullptr),
    m_oraclePriceLabel(nullptr),
    m_oraclePriceValue(nullptr),
    m_collateralLabel(nullptr),
    m_collateralValue(nullptr),
    m_ratioLabel(nullptr),
    m_ratioValue(nullptr),
    m_ratioBar(nullptr),
    m_availableDGBLabel(nullptr),
    m_availableDGBValue(nullptr),
    m_buttonFrame(nullptr),
    m_buttonLayout(nullptr),
    m_mintButton(nullptr),
    m_clearButton(nullptr),
    m_amountValidator(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr),
    m_availableDGBBalance(0.0),
    m_oraclePrice(0.01), // Default DGB price in USD
    m_mintAmount(0.0),
    m_selectedTier(0),
    m_requiredCollateral(0.0),
    m_collateralRatio(0.0),
    m_lastDisplayedCollateral(0.0),
    m_lastDisplayedOraclePrice(0.0)
{
    setupUI();
    connectSignals();
    // Initialize tier selection to trigger calculation of default tier
    onLockTierChanged();
    // REMOVED: applyTheme() - Let CSS handle all theming
}

DigiDollarMintWidget::~DigiDollarMintWidget()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarMintWidget::setupUI()
{
    // Create main layout - compact like DGB tabs
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Create validators — DD amounts are in dollars with max 2 decimal places (cents).
    // Mint limits must follow active chain params, not stale UI constants.
    const auto& ddParams = Params().GetDigiDollarParams();
    const double minMintAmount = ddParams.minMintAmount / 100.0;
    const double maxMintAmount = ddParams.maxMintAmount / 100.0;
    m_amountValidator = new AmountValidator(minMintAmount, maxMintAmount, 2, this);

    // Create horizontal layout for mint amount and lock period side-by-side
    QHBoxLayout* topLayout = new QHBoxLayout();
    topLayout->setSpacing(6);

    // Setup sections
    setupMintAmountSection();
    setupLockTierSection();

    // Add mint amount and lock period to horizontal layout
    topLayout->addWidget(m_amountFrame);
    topLayout->addWidget(m_lockTierFrame);

    // Add horizontal layout to main layout
    m_mainLayout->addLayout(topLayout);

    // Add collateral section below (full width)
    setupCollateralSection();
    setupButtonSection();

    // Add stretch to push content to top
    m_mainLayout->addStretch();

    setLayout(m_mainLayout);
}

void DigiDollarMintWidget::setupMintAmountSection()
{
    // Create mint amount frame
    m_amountFrame = new QFrame(this);
    m_amountFrame->setFrameStyle(QFrame::StyledPanel);
    m_amountFrame->setFrameShadow(QFrame::Sunken);
    m_amountFrame->setObjectName("amountFrame");

    m_amountLayout = new QGridLayout(m_amountFrame);
    m_amountLayout->setSpacing(8);
    m_amountLayout->setContentsMargins(10, 10, 10, 10);
    m_amountLayout->setHorizontalSpacing(12);
    m_amountLayout->setVerticalSpacing(8);

    // Title
    QLabel* amountTitle = new QLabel(tr("Mint Amount"), this);
    QFont titleFont = amountTitle->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    amountTitle->setFont(titleFont);
    m_amountLayout->addWidget(amountTitle, 0, 0, 1, 3);

    // Amount input
    m_amountLabel = new QLabel(tr("Amount to Mint:"), this);
    m_amountLabel->setToolTip(tr("Enter the amount of DigiDollar to mint"));

    // Create horizontal layout for amount input and suffix
    QHBoxLayout* amountInputLayout = new QHBoxLayout();
    amountInputLayout->setSpacing(8);

    m_amountEdit = new QLineEdit(this);
    m_amountEdit->setObjectName("amountEdit");
    m_amountEdit->setValidator(m_amountValidator);
    m_amountEdit->setPlaceholderText("0.00");
    const auto& ddParams = Params().GetDigiDollarParams();
    const double minMintAmount = ddParams.minMintAmount / 100.0;
    const double maxMintAmount = ddParams.maxMintAmount / 100.0;
    m_amountEdit->setToolTip(tr("The amount of DigiDollar to mint.\n\n• Minimum: %1 $DD\n• Maximum: %2 $DD\n• Up to 2 decimal places (cents)")
        .arg(QString::number(minMintAmount, 'f', 2))
        .arg(QString::number(maxMintAmount, 'f', 2)));
    m_amountEdit->setFocusPolicy(Qt::StrongFocus);
    m_amountEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_amountEdit->setFont(monospaceFont);

    // Set buddy AFTER m_amountEdit is created
    m_amountLabel->setBuddy(m_amountEdit);

    m_amountSuffix = new QLabel("$DD", this);
    m_amountSuffix->setObjectName("amountSuffix");
    // Theme styling applied in applyTheme()

    amountInputLayout->addWidget(m_amountEdit, 1);
    amountInputLayout->addWidget(m_amountSuffix, 0);

    m_amountLayout->addWidget(m_amountLabel, 1, 0);
    m_amountLayout->addLayout(amountInputLayout, 1, 1);

    // USD value
    m_usdValueLabel = new QLabel(tr("$USD Equivalent:"), this);
    m_usdValueLabel->setObjectName("usdValueLabel");
    m_usdValueLabel->setToolTip(tr("Equivalent value in US Dollars (DigiDollar is pegged to $1 USD)"));
    m_usdValueValue = new QLabel("0.00 $USD", this);
    m_usdValueValue->setObjectName("usdValueValue");
    m_usdValueValue->setFont(monospaceFont);
    // Theme styling applied in applyTheme() and updateUSDEquivalent()
    m_usdValueValue->setToolTip(tr("USD value updates in real-time as you type"));

    m_amountLayout->addWidget(m_usdValueLabel, 2, 0);
    m_amountLayout->addWidget(m_usdValueValue, 2, 1);

    // Warning label for min/max amount limits
    m_amountWarningLabel = new QLabel(this);
    m_amountWarningLabel->setObjectName("amountWarningLabel");
    m_amountWarningLabel->setWordWrap(true);
    m_amountWarningLabel->setVisible(false); // Hidden by default
    m_amountLayout->addWidget(m_amountWarningLabel, 3, 0, 1, 2);

    // Add stretch to push everything left
    m_amountLayout->setColumnStretch(1, 0);
    m_amountLayout->setColumnStretch(2, 1);

    // Frame is added to horizontal layout in setupUI()
}

void DigiDollarMintWidget::setupLockTierSection()
{
    // Create lock tier frame
    m_lockTierFrame = new QFrame(this);
    m_lockTierFrame->setFrameStyle(QFrame::StyledPanel);
    m_lockTierFrame->setFrameShadow(QFrame::Sunken);
    m_lockTierFrame->setObjectName("lockTierFrame");

    m_lockTierLayout = new QGridLayout(m_lockTierFrame);
    m_lockTierLayout->setSpacing(8);
    m_lockTierLayout->setContentsMargins(10, 10, 10, 10);
    m_lockTierLayout->setHorizontalSpacing(12);
    m_lockTierLayout->setVerticalSpacing(8);

    // Title
    QLabel* tierTitle = new QLabel(tr("Lock Period"), this);
    QFont titleFont = tierTitle->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    tierTitle->setFont(titleFont);
    m_lockTierLayout->addWidget(tierTitle, 0, 0, 1, 2);

    // Lock period combo
    m_lockTierLabel = new QLabel(tr("Time Lock Period:"), this);
    m_lockTierLabel->setToolTip(tr("Select how long your DGB collateral will be locked"));
    m_lockTierCombo = new QComboBox(this);
    m_lockTierCombo->setObjectName("lockTierCombo");
    m_lockTierCombo->setToolTip(tr("WARNING: Your DGB will be locked for this period and cannot be accessed until the timelock expires.\nLonger locks require less collateral (30 days: 500%, 10 years: 200%)"));
    m_lockTierLabel->setBuddy(m_lockTierCombo);

    // Add all 10 canonical lock periods.
    for (int i = 0; i <= 9; ++i) {
        m_lockTierCombo->addItem(getLockTierDisplayName(i), i);
    }

    m_lockTierLayout->addWidget(m_lockTierLabel, 1, 0);
    m_lockTierLayout->addWidget(m_lockTierCombo, 1, 1);

    // Set default selection to tier 1 (30 days).
    m_lockTierCombo->setCurrentIndex(1);

    // Lock tier info
    m_lockTierInfoLabel = new QLabel(tr("Collateral Ratio:"), this);
    m_lockTierInfoLabel->setToolTip(tr("Required collateral ratio for the selected tier"));
    m_lockTierInfoValue = new QLabel("200%", this);
    m_lockTierInfoValue->setObjectName("lockTierInfoValue");
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_lockTierInfoValue->setFont(monospaceFont);
    // Theme styling applied in applyTheme()
    m_lockTierInfoValue->setToolTip(tr("This ratio determines how much DGB you need to lock"));

    m_lockTierLayout->addWidget(m_lockTierInfoLabel, 2, 0);
    m_lockTierLayout->addWidget(m_lockTierInfoValue, 2, 1);

    // Add stretch to push everything left
    m_lockTierLayout->setColumnStretch(1, 0);
    m_lockTierLayout->setColumnStretch(2, 1);

    // Frame is added to horizontal layout in setupUI()
}

void DigiDollarMintWidget::setupCollateralSection()
{
    // Create collateral frame
    m_collateralFrame = new QFrame(this);
    m_collateralFrame->setFrameStyle(QFrame::StyledPanel);
    m_collateralFrame->setFrameShadow(QFrame::Sunken);
    m_collateralFrame->setObjectName("collateralFrame");

    m_collateralLayout = new QGridLayout(m_collateralFrame);
    m_collateralLayout->setSpacing(8);
    m_collateralLayout->setContentsMargins(10, 10, 10, 10);
    m_collateralLayout->setHorizontalSpacing(12);
    m_collateralLayout->setVerticalSpacing(8);

    // Title
    QLabel* collateralTitle = new QLabel(tr("Collateral Requirements"), this);
    QFont titleFont = collateralTitle->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    collateralTitle->setFont(titleFont);
    m_collateralLayout->addWidget(collateralTitle, 0, 0, 1, 2);

    // Oracle price
    m_oraclePriceLabel = new QLabel(tr("Oracle Price:"), this);
    m_oraclePriceLabel->setObjectName("oraclePriceLabel");
    m_oraclePriceLabel->setToolTip(tr("Current DGB price from oracle feed"));
    m_oraclePriceValue = new QLabel("0.01 $USD/DGB", this);
    m_oraclePriceValue->setObjectName("oraclePriceValue");
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_oraclePriceValue->setFont(monospaceFont);
    // Theme styling applied in applyTheme()
    m_oraclePriceValue->setToolTip(tr("Real-time DGB price used for collateral calculations"));

    m_collateralLayout->addWidget(m_oraclePriceLabel, 1, 0);
    m_collateralLayout->addWidget(m_oraclePriceValue, 1, 1);

    // Required collateral
    m_collateralLabel = new QLabel(tr("Required DGB:"), this);
    m_collateralLabel->setObjectName("collateralLabel");
    m_collateralLabel->setToolTip(tr("Amount of DGB that will be locked as collateral"));
    m_collateralValue = new QLabel("0.00000000 DGB", this);
    m_collateralValue->setObjectName("collateralValue");
    m_collateralValue->setFont(monospaceFont);
    // Theme styling applied dynamically in updateCollateralCalculation()
    m_collateralValue->setToolTip(tr("This DGB will be locked until the position is closed"));

    m_collateralLayout->addWidget(m_collateralLabel, 2, 0);
    m_collateralLayout->addWidget(m_collateralValue, 2, 1);

    // Collateral ratio
    m_ratioLabel = new QLabel(tr("Current Ratio:"), this);
    m_ratioLabel->setObjectName("ratioLabel");
    m_ratioLabel->setToolTip(tr("Actual collateral ratio for this mint"));
    m_ratioValue = new QLabel("200%", this);
    m_ratioValue->setObjectName("ratioValue");
    m_ratioValue->setFont(monospaceFont);
    // Theme styling applied dynamically in updateCollateralCalculation()
    m_ratioValue->setToolTip(tr("Higher ratios provide more safety margin"));

    m_collateralLayout->addWidget(m_ratioLabel, 3, 0);
    m_collateralLayout->addWidget(m_ratioValue, 3, 1);

    // Ratio progress bar
    m_ratioBar = new QProgressBar(this);
    m_ratioBar->setObjectName("ratioBar");
    m_ratioBar->setRange(200, 500);
    m_ratioBar->setValue(500);
    m_ratioBar->setTextVisible(false);
    m_ratioBar->setToolTip(tr("Visualizes the 200%-500% collateral safety band. The exact ratio is shown above."));
    m_collateralLayout->addWidget(m_ratioBar, 4, 0, 1, 2);

    // Available DGB
    m_availableDGBLabel = new QLabel(tr("Available DGB:"), this);
    m_availableDGBLabel->setObjectName("availableDGBLabel");
    m_availableDGBLabel->setToolTip(tr("Your current available DGB balance"));
    m_availableDGBValue = new QLabel("0.00000000 DGB", this);
    m_availableDGBValue->setObjectName("availableDGBValue");
    m_availableDGBValue->setFont(monospaceFont);
    // Theme styling applied in applyTheme()
    m_availableDGBValue->setToolTip(tr("Your current spendable DGB balance"));

    m_collateralLayout->addWidget(m_availableDGBLabel, 5, 0);
    m_collateralLayout->addWidget(m_availableDGBValue, 5, 1);

    m_mintStatusLabel = new QLabel(this);
    m_mintStatusLabel->setObjectName("mintVolatilityStatus");
    m_mintStatusLabel->setWordWrap(true);
    m_mintStatusLabel->hide();
    m_collateralLayout->addWidget(m_mintStatusLabel, 6, 0, 1, 2);

    m_mainLayout->addWidget(m_collateralFrame);
}

void DigiDollarMintWidget::setupButtonSection()
{
    // Create button frame
    m_buttonFrame = new QFrame(this);
    m_buttonFrame->setObjectName("buttonFrame");
    m_buttonFrame->setFrameStyle(QFrame::NoFrame);

    m_buttonLayout = new QHBoxLayout(m_buttonFrame);
    m_buttonLayout->setSpacing(10);
    m_buttonLayout->setContentsMargins(10, 10, 10, 10);

    // Clear button
    m_clearButton = new QPushButton(tr("&Clear"), this);
    m_clearButton->setObjectName("clearButton");
    m_clearButton->setToolTip(tr("Clear all fields"));
    m_clearButton->setAutoDefault(false);
    m_buttonLayout->addWidget(m_clearButton);

    // Add stretch to push mint button to the right
    m_buttonLayout->addStretch();

    // Mint button - styled to match main wallet
    m_mintButton = new QPushButton(tr("&Mint DigiDollar"), this);
    m_mintButton->setObjectName("mintButton");
    m_mintButton->setEnabled(false);
    m_mintButton->setDefault(true);
    m_mintButton->setAutoDefault(true);
    m_mintButton->setToolTip(tr("Confirm and create this DigiDollar mint transaction"));
    m_buttonLayout->addWidget(m_mintButton);

    m_mainLayout->addWidget(m_buttonFrame);
}

void DigiDollarMintWidget::connectSignals()
{
    // Connect amount validation
    connect(m_amountEdit, &QLineEdit::textChanged,
            this, &DigiDollarMintWidget::onAmountChanged);

    // Connect lock tier selection
    connect(m_lockTierCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DigiDollarMintWidget::onLockTierChanged);

    // Connect buttons
    connect(m_mintButton, &QPushButton::clicked,
            this, &DigiDollarMintWidget::onMintClicked);
    connect(m_clearButton, &QPushButton::clicked,
            this, &DigiDollarMintWidget::onClearClicked);

    // Auto-refresh oracle price every 12 seconds
    // This ensures the displayed collateral stays current as the oracle price moves.
    // More frequent than the old 30s to minimize surprise changes at mint time.
    QTimer* oraclePriceTimer = new QTimer(this);
    connect(oraclePriceTimer, &QTimer::timeout, this, &DigiDollarMintWidget::updateOraclePrice);
    oraclePriceTimer->start(12000); // 12 seconds
}

void DigiDollarMintWidget::setWalletModel(WalletModel* model)
{
    m_walletModel = model;

    if (m_walletModel) {
        // Connect wallet model signals
        updateBalance();
        // REMOVED: applyTheme() - Let CSS handle all theming
    }
}

void DigiDollarMintWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    if (m_clientModel) {
        // Connect client model signals
        updateOraclePrice();
        // REMOVED: applyTheme() - Let CSS handle all theming
    }
}

void DigiDollarMintWidget::updateView()
{
    updateBalance();
    updateOraclePrice();
    updateCollateralCalculation();
    updateAmountValidation();  // Update visual feedback based on current balance/collateral
    updateMintButton();
}

void DigiDollarMintWidget::updateBalance()
{
    if (m_walletModel) {
        // Get actual DGB balance from wallet (in satoshis)
        CAmount balanceSatoshis = m_walletModel->getAvailableDGBBalance();
        m_availableDGBBalance = balanceSatoshis / 100000000.0; // Convert satoshis to DGB
    } else {
        m_availableDGBBalance = 0.0;
    }

    if (m_privacy) {
        m_availableDGBValue->setText(maskValue(formatDGBAmount(0)));
    } else {
        m_availableDGBValue->setText(formatDGBAmount(m_availableDGBBalance));
    }

    // Re-validate amount field when balance changes (fixes stale yellow/green state
    // when receiving DGB while on the Mint tab)
    updateAmountValidation();
    updateMintButton();
}

void DigiDollarMintWidget::updateOraclePrice()
{
    if (!isVisible()) return;
    LogPrintf("DigiDollar Mint: updateOraclePrice() called\n");

    // Get oracle price from RPC for testnet/mainnet, MockOracleManager for regtest
    ChainType chainType = Params().GetChainType();
    LogPrintf("DigiDollar Mint: ChainType = %d (REGTEST=%d, TESTNET=%d, MAIN=%d)\n",
              (int)chainType, (int)ChainType::REGTEST, (int)ChainType::TESTNET, (int)ChainType::MAIN);

    m_mintVolatilityAllowed = true;
    m_mintStatusLabel->hide();
    if (m_clientModel && DigiDollar::IsThawDayActive(Params().GetConsensus(), m_clientModel->getNumBlocks() + 1)) {
        m_mintVolatilityAllowed = false;
        m_oraclePrice = 0;
        m_mintStatusLabel->show();
        try {
            UniValue rpc_params(UniValue::VARR);
            const UniValue result = m_clientModel->node().executeRpc("getoracleprice", rpc_params, "");
            const UniValue& status = result.find_value("mint_volatility");
            const bool ready = status.find_value("ready").get_bool();
            const bool quoted = status.find_value("quote_available").get_bool();
            const bool restricted = status.find_value("minting_restricted").get_bool();
            m_oraclePrice = status.find_value("candidate_price_micro_usd").getInt<int64_t>() / 1000000.0;
            const UniValue protection = m_clientModel->node().executeRpc("getprotectionstatus", rpc_params, "");
            const UniValue& health = protection.find_value("next_block_health");
            const bool healthReady = health.find_value("ready").get_bool();
            const bool emergency = healthReady && health.find_value("health_percentage").getInt<int>() < 100;
            m_mintVolatilityAllowed = ready && quoted && !restricted && healthReady && !emergency;
            const QString reason = !quoted ? tr("No valid oracle quote; minting is paused.") :
                (!ready ? tr("Required price history is unavailable; restore or download the missing blocks.") :
                (restricted ? tr("Minting is paused: the quote differs by at least 20% from the ancestor reference.") :
                 tr("Quote is within the mint volatility limit. Health, collateral and fees must also pass.")));
            m_mintStatusLabel->setText(!healthReady ? tr("Canonical health is unavailable; wait for synchronization and retry.") :
                (emergency ? tr("Minting is paused by emergency health calculated from open vaults.") : reason));
            m_mintStatusLabel->setToolTip(tr("Candidate height: %1\nRule version: %2\nReference: %3 micro-USD\nSamples: %4\nWindow: %5 through %6\nDeviation: %7 basis points\nConfirmation conditions may change.")
                .arg(status.find_value("candidate_height").getInt<int>())
                .arg(status.find_value("rule_version").getInt<int>())
                .arg(status.find_value("reference_price_micro_usd").getInt<int64_t>())
                .arg(status.find_value("sample_count").getInt<int>())
                .arg(status.find_value("window_start_height").getInt<int>())
                .arg(status.find_value("window_end_height").getInt<int>())
                .arg(status.find_value("deviation_bps").getInt<int64_t>()));
        } catch (...) {
            m_mintStatusLabel->setText(tr("Mint eligibility is unavailable. Wait for synchronization and try again."));
        }
    } else if (chainType == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        // BUG #6 FIX: GetCurrentPrice() returns micro-USD, not cents
        CAmount priceMicroUsd = MockOracleManager::GetInstance().GetCurrentPrice();
        m_oraclePrice = priceMicroUsd / 1000000.0;
        LogPrintf("DigiDollar Mint: Using MockOracle, price = %f USD\n", m_oraclePrice);
    } else if (m_clientModel) {
        // Get actual oracle price from RPC
        LogPrintf("DigiDollar Mint: Using RPC (m_clientModel is valid)\n");
        try {
            UniValue params(UniValue::VARR);
            UniValue result = m_clientModel->node().executeRpc("getoracleprice", params, "");
            LogPrintf("DigiDollar Mint: RPC call succeeded\n");

            // Price is returned in micro-USD (1,000,000 = $1.00)
            const UniValue& priceVal = result.find_value("price_micro_usd");
            if (priceVal.isNull()) {
                LogPrintf("DigiDollar Mint: price_micro_usd is NULL in response!\n");
                m_oraclePrice = 0.0;
            } else {
                int64_t priceMicroUsd = priceVal.getInt<int64_t>();
                m_oraclePrice = priceMicroUsd / 1000000.0; // Convert micro-USD to dollars
                LogPrintf("DigiDollar Mint: Got price_micro_usd=%ld, m_oraclePrice=%f\n", priceMicroUsd, m_oraclePrice);
            }
        } catch (const UniValue& e) {
            LogPrintf("DigiDollar Mint: updateOraclePrice RPC error - %s\n", e.write());
            m_oraclePrice = 0.0;
        } catch (const std::exception& e) {
            LogPrintf("DigiDollar Mint: updateOraclePrice error - %s\n", e.what());
            m_oraclePrice = 0.0;
        } catch (...) {
            LogPrintf("DigiDollar Mint: updateOraclePrice unknown error\n");
            m_oraclePrice = 0.0;
        }
    } else {
        LogPrintf("DigiDollar Mint: m_clientModel is NULL!\n");
        m_oraclePrice = 0.0;
    }

    LogPrintf("DigiDollar Mint: Final m_oraclePrice = %f\n", m_oraclePrice);

    if (m_oraclePrice > 0) {
        m_oraclePriceValue->setText(formatUSDAmount(m_oraclePrice) + "/DGB");
    } else {
        m_oraclePriceValue->setText(tr("Oracle unavailable"));
    }
    updateCollateralCalculation();
}

void DigiDollarMintWidget::onAmountChanged()
{
    QString amountText = m_amountEdit->text();
    LogPrintf("DigiDollar Mint: onAmountChanged called - text='%s', isEnabled=%d, isReadOnly=%d\n",
              amountText.toStdString().c_str(), m_amountEdit->isEnabled(), m_amountEdit->isReadOnly());
    if (!amountText.isEmpty()) {
        m_mintAmount = amountText.toDouble();
        double usdValue = m_mintAmount * 1.0; // DD should be pegged to $1
        m_usdValueValue->setText(formatDigiDollarUSDEquivalent(usdValue));
        updateUSDEquivalent();
    } else {
        m_mintAmount = 0.0;
        m_usdValueValue->setText(formatDigiDollarUSDEquivalent(0));
        updateUSDEquivalent();
    }

    // IMPORTANT: Calculate collateral FIRST, then validate
    // This ensures m_requiredCollateral is up-to-date when validateCollateral() is called
    updateCollateralCalculation();
    updateAmountValidation();
    updateMintButton();
}

void DigiDollarMintWidget::onLockTierChanged()
{
    int newTier = m_lockTierCombo->currentData().toInt();
    LogPrintf("DigiDollar Qt: Lock tier changed to: %d\n", newTier);

    // Show warning for long lock periods (1 year or more = tier 4+)
    if (newTier >= 4) {
        QString lockPeriodStr = getLockTierDisplayName(newTier);
        QString periodName;
        // IMPORTANT: These names MUST match getLockTierDisplayName() and consensus tier definitions
        switch (newTier) {
            case 4: periodName = "1 YEAR"; break;
            case 5: periodName = "2 YEARS"; break;
            case 6: periodName = "3 YEARS"; break;
            case 7: periodName = "5 YEARS"; break;
            case 8: periodName = "7 YEARS"; break;
            case 9: periodName = "10 YEARS"; break;
            default: periodName = "EXTENDED PERIOD"; break;
        }

        QMessageBox warningBox(this);
        warningBox.setWindowTitle(tr("⚠️ Long-Term Lock Warning"));
        warningBox.setIcon(QMessageBox::Warning);
        warningBox.setText(tr("<span style='font-size:16pt; font-weight:bold; color:#ff6600;'>⚠️ ATTENTION: %1 LOCK ⚠️</span>").arg(periodName));
        warningBox.setInformativeText(tr(
            "<p style='font-size:12pt;'><b>You are selecting a <span style='color:#ff0000;'>%1</span> time lock!</b></p>"
            "<p style='font-size:11pt;'>This means:</p>"
            "<ul style='font-size:11pt;'>"
            "<li>Your DGB collateral will be <b>LOCKED</b> for %2</li>"
            "<li>You will <b>NOT</b> be able to access your DGB during this time</li>"
            "<li>There is <b>NO WAY</b> to unlock early under <b>ANY</b> condition</li>"
            "</ul>"
            "<p style='font-size:12pt; font-weight:bold; color:#ff6600;'>Do you understand and wish to continue with this lock period?</p>")
            .arg(periodName)
            .arg(periodName));
        warningBox.setTextFormat(Qt::RichText);
        warningBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        warningBox.setDefaultButton(QMessageBox::No);

        // Style the buttons
        warningBox.button(QMessageBox::No)->setStyleSheet("QPushButton { background-color: #d32f2f; color: white; font-weight: bold; padding: 8px 16px; }");
        warningBox.button(QMessageBox::Yes)->setStyleSheet("QPushButton { background-color: #1976d2; color: white; font-weight: bold; padding: 8px 16px; }");

        if (warningBox.exec() != QMessageBox::Yes) {
            // User cancelled - revert to previous safer tier (30 days)
            m_lockTierCombo->blockSignals(true);
            m_lockTierCombo->setCurrentIndex(1); // 30 days
            m_lockTierCombo->blockSignals(false);
            m_selectedTier = 1;
            LogPrintf("DigiDollar Qt: User cancelled long lock, reverting to tier 1\n");
        } else {
            m_selectedTier = newTier;
        }
    } else {
        m_selectedTier = newTier;
    }

    double ratio = getCollateralRatioForTier(m_selectedTier);
    m_lockTierInfoValue->setText(formatRatio(ratio));

    // Update collateral first, then validation to ensure m_requiredCollateral is current
    updateCollateralCalculation();
    updateAmountValidation();  // Update border color/warning based on new collateral requirement
    updateMintButton();
}

void DigiDollarMintWidget::onMintClicked()
{
    if (!validateAmount() || !validateCollateral()) {
        return;
    }

    // --- Oracle price drift check ---
    // Re-fetch the current oracle price and recalculate collateral.
    // If the price moved since the user last saw the estimate, warn them
    // so they can review before committing.
    double previousCollateral = m_lastDisplayedCollateral;
    double previousOraclePrice = m_lastDisplayedOraclePrice;

    // Refresh oracle price (updates m_oraclePrice)
    updateOraclePrice(); // also calls updateCollateralCalculation()

    // Re-validate after refresh — balance may no longer cover new collateral
    if (!validateCollateral()) {
        Q_EMIT message(tr("Insufficient Collateral"),
                       tr("The oracle price has changed and you no longer have "
                          "enough DGB to cover the required collateral.\n\n"
                          "Required: %1\nAvailable: %2")
                       .arg(formatDGBAmount(m_requiredCollateral))
                       .arg(formatDGBAmount(m_availableDGBBalance)),
                       QMessageBox::Warning);
        return;
    }

    // Use a small tolerance (0.001 DGB ≈ 100k satoshis) to avoid nagging on rounding noise
    const double collateralDrift = std::abs(m_requiredCollateral - previousCollateral);
    if (previousCollateral > 0 && collateralDrift > 0.001) {
        QMessageBox priceChangeBox(this);
        priceChangeBox.setWindowTitle(tr("Oracle Price Updated"));
        priceChangeBox.setIcon(QMessageBox::Information);
        priceChangeBox.setText(
            tr("The oracle price changed while you were reviewing.\n\n"
               "Required collateral is now %1 (was %2).\n"
               "Oracle price: %3/DGB (was %4/DGB)\n\n"
               "Would you like to continue with the updated amount?")
            .arg(formatDGBAmount(m_requiredCollateral))
            .arg(formatDGBAmount(previousCollateral))
            .arg(formatUSDAmount(m_oraclePrice))
            .arg(formatUSDAmount(previousOraclePrice)));
        priceChangeBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        priceChangeBox.setDefaultButton(QMessageBox::No);

        if (priceChangeBox.exec() != QMessageBox::Yes) {
            // User chose to review — form is already updated with new values
            LogPrintf("DigiDollar Qt: User declined mint after oracle price change "
                      "(collateral %.8f -> %.8f)\n", previousCollateral, m_requiredCollateral);
            return;
        }
    }
    // --- End oracle price drift check ---

    auto refreshAndRevalidateMintInputs = [this](const QString& title) -> bool {
        const double previousCollateral = m_requiredCollateral;
        const double previousOraclePrice = m_oraclePrice;

        updateOraclePrice(); // also recalculates collateral from current oracle data
        updateAmountValidation();
        updateMintButton();

        if (!validateAmount()) {
            Q_EMIT message(tr("Invalid Amount"),
                           tr("The mint amount is no longer valid under the active chain limits."),
                           QMessageBox::Warning);
            return false;
        }

        if (!validateCollateral()) {
            Q_EMIT message(tr("Insufficient Collateral"),
                           tr("The oracle price or wallet balance changed and you no longer have "
                              "enough DGB to cover the required collateral.\n\n"
                              "Required: %1\nAvailable: %2")
                           .arg(formatDGBAmount(m_requiredCollateral))
                           .arg(formatDGBAmount(m_availableDGBBalance)),
                           QMessageBox::Warning);
            return false;
        }

        const double collateralDrift = std::abs(m_requiredCollateral - previousCollateral);
        if (previousCollateral > 0 && collateralDrift > 0.001) {
            Q_EMIT message(title,
                           tr("The oracle price changed during confirmation, so the mint details were refreshed.\n\n"
                              "Required collateral is now %1 (was %2).\n"
                              "Oracle price: %3/DGB (was %4/DGB)\n\n"
                              "Please review the updated values and click Mint again if they look correct.")
                           .arg(formatDGBAmount(m_requiredCollateral))
                           .arg(formatDGBAmount(previousCollateral))
                           .arg(formatUSDAmount(m_oraclePrice))
                           .arg(formatUSDAmount(previousOraclePrice)),
                           QMessageBox::Information);
            return false;
        }

        return true;
    };

    // Calculate unlock details for user warning
    int lockBlocks = getLockTierBlocks(m_selectedTier);
    const int bufferBlocks = DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;
    const int bufferMinutes = (bufferBlocks * 15 + 59) / 60;
    int currentHeight = m_clientModel ? m_clientModel->getNumBlocks() : 0;
    int unlockHeight = currentHeight + lockBlocks + bufferBlocks;
    QString lockPeriodStr = getLockTierDisplayName(m_selectedTier);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Confirm DigiDollar Mint"));
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setText(tr("Mint %1 by locking %2 DGB?")
                  .arg(formatDDAmount(m_mintAmount))
                  .arg(formatDGBAmount(m_requiredCollateral)));

    QString warningText = tr(
        "⚠️ TIMELOCK WARNING ⚠️\n\n"
        "Your DGB will be LOCKED for %1\n"
        "You will NOT be able to access this DGB until block %2\n\n"
        "Lock Period: %3 (%4 blocks)\n"
        "Network Confirmation Buffer: %5 blocks (~%6 minutes)\n"
        "Collateral Ratio: %7\n"
        "Current Block: %8\n"
        "Redeem Available Block: %9\n\n"
        "Make sure you understand this commitment before proceeding!")
        .arg(lockPeriodStr)
        .arg(unlockHeight)
        .arg(lockPeriodStr)
        .arg(lockBlocks)
        .arg(bufferBlocks)
        .arg(bufferMinutes)
        .arg(formatRatio(m_collateralRatio))
        .arg(currentHeight)
        .arg(unlockHeight);

    msgBox.setInformativeText(warningText);
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);

    if (msgBox.exec() == QMessageBox::Yes) {
        if (!refreshAndRevalidateMintInputs(tr("Oracle Price Updated"))) {
            return;
        }

        // Refresh unlock details too; height may have advanced while the dialog was open.
        lockBlocks = getLockTierBlocks(m_selectedTier);
        currentHeight = m_clientModel ? m_clientModel->getNumBlocks() : 0;
        unlockHeight = currentHeight + lockBlocks + bufferBlocks;
        lockPeriodStr = getLockTierDisplayName(m_selectedTier);

        // SECOND WARNING - Final "Are you ABSOLUTELY sure?" confirmation
        // IMPORTANT: These names MUST match getLockTierDisplayName() and consensus tier definitions
        QString periodName;
        switch (m_selectedTier) {
            case 0: periodName = "1 HOUR"; break;
            case 1: periodName = "30 DAYS"; break;
            case 2: periodName = "3 MONTHS"; break;
            case 3: periodName = "6 MONTHS"; break;
            case 4: periodName = "1 YEAR"; break;
            case 5: periodName = "2 YEARS"; break;
            case 6: periodName = "3 YEARS"; break;
            case 7: periodName = "5 YEARS"; break;
            case 8: periodName = "7 YEARS"; break;
            case 9: periodName = "10 YEARS"; break;
            default: periodName = "SELECTED PERIOD"; break;
        }

        QMessageBox finalWarning(this);
        finalWarning.setWindowTitle(tr("🚨 FINAL CONFIRMATION 🚨"));
        finalWarning.setIcon(QMessageBox::Critical);
        finalWarning.setText(tr("<span style='font-size:18pt; font-weight:bold; color:#ff0000;'>🚨 ARE YOU ABSOLUTELY SURE? 🚨</span>"));
        finalWarning.setInformativeText(tr(
            "<p style='font-size:14pt; font-weight:bold; color:#ff0000;'>YOUR DGB WILL BE LOCKED FOR %1!</p>"
            "<p style='font-size:12pt;'><b>Amount to Lock:</b> <span style='color:#ff6600;'>%2</span></p>"
            "<p style='font-size:12pt;'><b>$DD to Receive:</b> <span style='color:#00aa00;'>%3</span></p>"
            "<hr>"
            "<p style='font-size:11pt;'>Once confirmed, this action <b>CANNOT BE UNDONE</b>.</p>"
            "<p style='font-size:11pt;'>Your DGB collateral becomes redeemable at block <b>%4</b>, after the lock period plus the %5-block network confirmation buffer.</p>"
            "<p style='font-size:13pt; font-weight:bold; color:#ff0000;'>Click 'No' if you have ANY doubts!</p>")
            .arg(periodName)
            .arg(formatDGBAmount(m_requiredCollateral))
            .arg(formatDDAmount(m_mintAmount))
            .arg(unlockHeight)
            .arg(bufferBlocks));
        finalWarning.setTextFormat(Qt::RichText);
        finalWarning.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        finalWarning.setDefaultButton(QMessageBox::No);

        // Style buttons - No is RED and prominent, Yes is less prominent
        finalWarning.button(QMessageBox::No)->setText(tr("❌ No, Cancel"));
        finalWarning.button(QMessageBox::No)->setStyleSheet("QPushButton { background-color: #d32f2f; color: white; font-weight: bold; font-size: 12pt; padding: 10px 20px; }");
        finalWarning.button(QMessageBox::Yes)->setText(tr("✓ Yes, I'm Sure"));
        finalWarning.button(QMessageBox::Yes)->setStyleSheet("QPushButton { background-color: #388e3c; color: white; font-weight: bold; font-size: 11pt; padding: 8px 16px; }");

        if (finalWarning.exec() != QMessageBox::Yes) {
            LogPrintf("DigiDollar Qt: User cancelled at final confirmation\n");
            return;
        }

        if (!refreshAndRevalidateMintInputs(tr("Mint Details Refreshed"))) {
            return;
        }

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
        CAmount ddAmountCents = static_cast<CAmount>(std::llround(m_mintAmount * 100));

        // Call the wallet model to mint DigiDollar
        WalletModel::DigiDollarMintResult result = m_walletModel->mintDigiDollar(ddAmountCents, m_selectedTier);

        if (result.status == WalletModel::OK) {
            // Format collateral amount
            QString collateralStr = QString::number(result.collateralLocked / 100000000.0, 'f', 8) + " DGB";
            QString ddAmountStr = formatDDAmount(ddAmountCents / 100.0);

            QString successMessage = tr("DigiDollar mint transaction created successfully!\n\n"
                           "Transaction ID:\n%1\n\n"
                           "Amount Minted: %2\n"
                           "Collateral Locked: %3\n"
                           "Lock Tier: %4")
                        .arg(result.txid)
                        .arg(ddAmountStr)
                        .arg(collateralStr)
                        .arg(m_selectedTier);

            LogPrintf("DigiDollar Qt: Showing success message dialog\n");

            // Show modal message box directly
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Mint Successful"));
            msgBox.setText(successMessage);
            msgBox.setIcon(QMessageBox::Information);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();

            LogPrintf("DigiDollar Qt: Success message shown\n");

            onClearClicked();
            updateBalance(); // Refresh balance displays
        } else {
            QString errorTitle;
            // DD-FA-FUNC-032 (Wave 19 Agent C): Qt mint broadcasts directly via
            // node().broadcastTransaction so consensus reject reasons such as
            // "minting-blocked-during-err" or "bad-tx-no-musig2-quote" reach
            // the user verbatim. Translate known DD/oracle tags to plain
            // English with a remediation hint before display; unknown reasons
            // pass through unchanged. The translator is unit-tested by
            // src/test/digidollar_qt_translate_tests.cpp without Qt.
            const QString rawReason = result.reasonFailed;
            QString errorMessage = QString::fromStdString(
                TranslateMintRejectReasonForUser(rawReason.toStdString()));

            switch (result.status) {
            case WalletModel::InvalidAmount:
                errorTitle = tr("Invalid Amount");
                break;
            case WalletModel::AmountExceedsBalance:
                errorTitle = tr("Insufficient Collateral");
                break;
            case WalletModel::TransactionCreationFailed:
                errorTitle = tr("Transaction Failed");
                break;
            default:
                errorTitle = tr("Mint Error");
                break;
            }

            Q_EMIT message(errorTitle, errorMessage, QMessageBox::Critical);
        }
    }
}

void DigiDollarMintWidget::onClearClicked()
{
    m_amountEdit->clear();
    m_lockTierCombo->setCurrentIndex(0);
    onAmountChanged(); // Reset amounts
    onLockTierChanged(); // Reset tier
}

void DigiDollarMintWidget::updateMintButton()
{
    bool amountValid = validateAmount();
    bool collateralValid = validateCollateral();

    LogPrintf("DigiDollar Mint: updateMintButton - amountValid=%d, collateralValid=%d, m_requiredCollateral=%f, m_availableDGBBalance=%f, m_oraclePrice=%f\n",
              amountValid, collateralValid, m_requiredCollateral, m_availableDGBBalance, m_oraclePrice);

    m_mintButton->setEnabled(amountValid && collateralValid && m_mintVolatilityAllowed);
}

void DigiDollarMintWidget::updateCollateralCalculation()
{
    calculateRequiredCollateral();
    const int visualRatio = std::clamp(static_cast<int>(m_collateralRatio), 200, 500);

    // Update displays
    if (m_privacy) {
        m_collateralValue->setText(maskValue(formatDGBAmount(0)));
        m_ratioValue->setText(formatRatio(m_collateralRatio)); // Ratio is not sensitive
        m_ratioBar->setValue(visualRatio);
    } else {
        m_collateralValue->setText(formatDGBAmount(m_requiredCollateral));
        m_ratioValue->setText(formatRatio(m_collateralRatio));
        m_ratioBar->setValue(visualRatio);
    }

    // Track what the user is currently seeing so we can detect drift at mint time
    m_lastDisplayedCollateral = m_requiredCollateral;
    m_lastDisplayedOraclePrice = m_oraclePrice;

    // REMOVED: Color-coded collateral displays - Let CSS handle theming
}

void DigiDollarMintWidget::calculateRequiredCollateral()
{
    if (m_mintAmount > 0 && m_oraclePrice > 0) {
        // Use the already-fetched oracle price (from RPC on testnet/mainnet, mock on regtest)
        // and the same builder math as the actual mint.
        m_collateralRatio = getCollateralRatioForTier(m_selectedTier);

        static constexpr int LOCK_DAYS_FOR_TIER[10] = {0, 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650};
        const CAmount ddAmountCents = static_cast<CAmount>(std::llround(m_mintAmount * 100));
        const CAmount oraclePriceMicroUSD = static_cast<CAmount>(std::llround(m_oraclePrice * 1000000.0));
        const int currentHeight = m_clientModel ? m_clientModel->getNumBlocks() : 0;
        if (DigiDollar::IsThawDayActive(Params().GetConsensus(), currentHeight + 1)) {
            m_requiredCollateral = 0;
            try {
                UniValue params(UniValue::VARR);
                params.push_back(ddAmountCents);
                params.push_back(LOCK_DAYS_FOR_TIER[m_selectedTier]);
                const UniValue quote = m_clientModel->node().executeRpc("calculatecollateralrequirement", params, "");
                m_requiredCollateral = quote.find_value("wallet_collateral_dgb").get_real();
                m_collateralRatio = quote.find_value("effective_ratio").getInt<int>();
            } catch (...) { m_mintVolatilityAllowed = false; }
            return;
        }
        DigiDollar::MintTxBuilder builder(Params(), currentHeight, oraclePriceMicroUSD);
        const CAmount requiredCollateralSats = builder.CalculateRequiredCollateral(ddAmountCents, LOCK_DAYS_FOR_TIER[m_selectedTier]);
        m_requiredCollateral = requiredCollateralSats > 0 ? requiredCollateralSats / static_cast<double>(COIN) : 0.0;
    } else {
        m_requiredCollateral = 0.0;
        m_collateralRatio = getCollateralRatioForTier(m_selectedTier);
    }
}

bool DigiDollarMintWidget::validateAmount() const
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return false;

    int pos = 0;
    QString amountCopy = amountText;
    if (m_amountValidator->validate(amountCopy, pos) != QValidator::Acceptable) {
        return false;
    }

    // Check min/max limits from chain params (amounts in cents)
    const auto& ddParams = Params().GetDigiDollarParams();
    double minAmount = ddParams.minMintAmount / 100.0;  // Convert cents to dollars
    double maxAmount = ddParams.maxMintAmount / 100.0;  // Convert cents to dollars

    double amount = amountText.toDouble();
    return amount >= minAmount && amount <= maxAmount;
}

bool DigiDollarMintWidget::validateCollateral() const
{
    return m_requiredCollateral > 0 && m_requiredCollateral <= m_availableDGBBalance;
}

QString DigiDollarMintWidget::formatDDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $DD";
}

QString DigiDollarMintWidget::formatDGBAmount(double amount) const
{
    return QString::number(amount, 'f', 8) + " DGB";
}

QString DigiDollarMintWidget::formatUSDAmount(double amount) const
{
    return QString::number(amount, 'f', 6) + " $USD";
}

QString DigiDollarMintWidget::formatDigiDollarUSDEquivalent(double amount) const
{
    return QString::number(amount, 'f', 2) + " $USD";
}

QString DigiDollarMintWidget::formatRatio(double ratio) const
{
    return QString::number(ratio, 'f', 0) + "%";
}

double DigiDollarMintWidget::getCollateralRatioForTier(int tier) const
{
    const int lock_days = DigiDollar::BlocksToLockDays(getLockTierBlocks(tier));
    const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(lock_days);
    const int ratio = DigiDollar::GetCollateralRatioForLockTime(lock_blocks, Params().GetDigiDollarParams());
    return ratio > 0 ? static_cast<double>(ratio) : 0.0;
}

QString DigiDollarMintWidget::getLockTierDisplayName(int tier) const
{
    const QString ratio = formatRatio(getCollateralRatioForTier(tier));
    switch (tier) {
    case 0: return tr("1 hour (%1 collateral)").arg(ratio);
    case 1: return tr("30 days (%1 collateral)").arg(ratio);
    case 2: return tr("3 months (%1 collateral)").arg(ratio);
    case 3: return tr("6 months (%1 collateral)").arg(ratio);
    case 4: return tr("1 year (%1 collateral)").arg(ratio);
    case 5: return tr("2 years (%1 collateral)").arg(ratio);
    case 6: return tr("3 years (%1 collateral)").arg(ratio);
    case 7: return tr("5 years (%1 collateral)").arg(ratio);
    case 8: return tr("7 years (%1 collateral)").arg(ratio);
    case 9: return tr("10 years (%1 collateral)").arg(ratio);
    default: return tr("1 year (%1 collateral)").arg(ratio);
    }
}

int DigiDollarMintWidget::getLockTierBlocks(int tier) const
{
    // Lock periods in blocks (15 second blocks)
    // 1 hour = 240 blocks, 1 day = 5760 blocks, 1 month = 172800 blocks, 1 year = 2102400 blocks
    switch (tier) {
    case 0: return 240;         // 1 hour
    case 1: return 172800;      // 30 days
    case 2: return 518400;      // 3 months (90 days)
    case 3: return 1036800;     // 6 months (180 days)
    case 4: return 2102400;     // 1 year (365 days)
    case 5: return 4204800;     // 2 years (730 days)
    case 6: return 6307200;     // 3 years
    case 7: return 10512000;    // 5 years
    case 8: return 14716800;    // 7 years
    case 9: return 21024000;    // 10 years
    default: return 2102400;    // 1 year
    }
}

// REMOVED: applyTheme() method
// All theming is now handled by light.css and dark.css files
// This allows the DigiByte blue theme to work properly

void DigiDollarMintWidget::updateAmountValidation()
{
    QString amountText = m_amountEdit->text();
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
    QString warningColor = isDarkTheme ? "#ff9800" : "#ffc107";
    QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";

    // Get min/max limits from chain params
    const auto& ddParams = Params().GetDigiDollarParams();
    double minAmount = ddParams.minMintAmount / 100.0;  // Convert cents to dollars
    double maxAmount = ddParams.maxMintAmount / 100.0;  // Convert cents to dollars

    if (!amountText.isEmpty()) {
        int pos = 0;
        QString amountCopy = amountText;
        const QValidator::State validationState = m_amountValidator->validate(amountCopy, pos);
        bool numericAmount = false;
        double amount = amountText.toDouble(&numericAmount);
        bool hasCollateral = validateCollateral();

        if (!numericAmount || validationState == QValidator::Invalid) {
            // Invalid format - only border color, let system handle background
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(errorColor));
            m_amountWarningLabel->setText(tr("Invalid amount format"));
            m_amountWarningLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(errorColor));
            m_amountWarningLabel->setVisible(true);
        } else if (amount < minAmount) {
            // Below minimum
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(errorColor));
            m_amountWarningLabel->setText(tr("⚠️ Minimum mint amount is $%1").arg(QString::number(minAmount, 'f', 2)));
            m_amountWarningLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(errorColor));
            m_amountWarningLabel->setVisible(true);
        } else if (amount > maxAmount) {
            // Above maximum
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(errorColor));
            m_amountWarningLabel->setText(tr("⚠️ Maximum mint amount is $%1").arg(QString::number(maxAmount, 'f', 0)));
            m_amountWarningLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(errorColor));
            m_amountWarningLabel->setVisible(true);
        } else if (m_oraclePrice <= 0) {
            // Oracle price is unavailable; minting must remain fail-closed.
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(warningColor));
            m_amountWarningLabel->setText(tr("Oracle price unavailable - minting is paused"));
            m_amountWarningLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(warningColor));
            m_amountWarningLabel->setVisible(true);
        } else if (!hasCollateral) {
            // Valid format but insufficient collateral
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(warningColor));
            m_amountWarningLabel->setText(tr("⚠️ Insufficient DGB collateral for this amount"));
            m_amountWarningLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(warningColor));
            m_amountWarningLabel->setVisible(true);
        } else {
            // Valid and sufficient collateral
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
            m_amountWarningLabel->setVisible(false);
        }
    } else {
        m_amountEdit->setStyleSheet("");
        m_amountWarningLabel->setVisible(false);
    }
}

void DigiDollarMintWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    updateBalance();
    updateCollateralCalculation();
    if (m_privacy) {
        m_usdValueValue->setText(maskValue(formatDigiDollarUSDEquivalent(0)));
        m_oraclePriceValue->setText(maskValue(formatUSDAmount(0) + "/DGB"));
    } else {
        updateOraclePrice();
    }
}

QString DigiDollarMintWidget::maskValue(const QString& value) const
{
    QString masked = value;
    for (int i = 0; i < masked.size(); ++i) {
        if (masked[i].isDigit()) {
            masked[i] = '#';
        }
    }
    return masked;
}

void DigiDollarMintWidget::updateUSDEquivalent()
{
    // REMOVED: All programmatic styling - Let CSS handle theming
    // USD value updates are handled by the CSS theme files
}
