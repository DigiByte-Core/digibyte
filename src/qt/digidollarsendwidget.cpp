// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarsendwidget.h>
#include <qt/ddaddressbookpage.h>

#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/digibyteunits.h>
#include <qt/digidollarcoincontroldialog.h>
#include <qt/platformstyle.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <consensus/amount.h>
#include <base58.h>
#include <logging.h>
#include <pubkey.h>
#include <uint256.h>
#include <kernel/chainparams.h>
#include <oracle/mock_oracle.h>
#include <interfaces/node.h>
#include <univalue.h>

#include <chrono>
#include <cmath>

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QValidator>
#include <QFont>
#include <QRegularExpression>
#include <QMessageBox>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>
#include <QScrollArea>
#include <QSpacerItem>
#include <QSizePolicy>
#include <QPalette>
#include <QProgressDialog>
#include <QAbstractButton>

using namespace std::chrono_literals;

namespace {
// What this form will let a user send in one DigiDollar transfer. The amount
// box refuses to accept anything outside these, and the line under the box
// says which limit was hit, so the user is never left with a red box and no
// reason for it.
constexpr double DD_SEND_MIN_DOLLARS = 1.00;
constexpr double DD_SEND_MAX_DOLLARS = 100000.00;
constexpr int DD_SEND_DECIMAL_PLACES = 2;
} // namespace

DigiDollarSendWidget::DigiDollarSendWidget(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    m_mainLayout(nullptr),
    m_addressFrame(nullptr),
    m_addressLayout(nullptr),
    m_addressLabel(nullptr),
    m_addressEdit(nullptr),
    m_pasteAddressButton(nullptr),
    m_addressBookButton(nullptr),
    m_addressValidationLabel(nullptr),
    m_amountFrame(nullptr),
    m_amountLayout(nullptr),
    m_amountLabel(nullptr),
    m_amountEdit(nullptr),
    m_amountSuffix(nullptr),
    m_useAvailableBalanceButton(nullptr),
    m_amountValidationLabel(nullptr),
    m_usdEquivalentLabel(nullptr),
    m_usdEquivalentValue(nullptr),
    m_availableBalanceLabel(nullptr),
    m_availableBalanceValue(nullptr),
    m_noteFrame(nullptr),
    m_noteLayout(nullptr),
    m_noteLabel(nullptr),
    m_noteEdit(nullptr),
    m_feeFrame(nullptr),
    m_feeLayout(nullptr),
    m_feeLabel(nullptr),
    m_feeValue(nullptr),
    m_totalLabel(nullptr),
    m_totalValue(nullptr),
    m_buttonFrame(nullptr),
    m_buttonLayout(nullptr),
    m_sendButton(nullptr),
    m_clearButton(nullptr),
    m_coinControlFrame(nullptr),
    m_coinControlLayout(nullptr),
    m_coinControlButton(nullptr),
    m_coinControlQuantityLabel(nullptr),
    m_coinControlAmountLabel(nullptr),
    m_addressValidator(nullptr),
    m_amountValidator(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr),
    m_platformStyle(platformStyle),
    m_availableBalance(0.0),
    m_oraclePrice(1.0),
    m_estimatedFee(0.001)  // TODO: Implement dynamic fee estimation based on transaction size and network conditions
{
    setupUI();
    connectSignals();
    // Put the starting words under the amount box, so the form says what it
    // will take before anything has been typed.
    updateAmountValidation();
    // REMOVED: applyTheme() - Let CSS handle all theming
}

DigiDollarSendWidget::~DigiDollarSendWidget()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarSendWidget::setupUI()
{
    // Create main layout - compact like DGB tabs
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Create validators
    m_addressValidator = new DigiDollarAddressValidator(this);
    // DD amounts are in dollars with max 2 decimal places (cents precision)
    // Send limits: $1 minimum (dust threshold), $100,000 maximum
    m_amountValidator = new AmountValidator(DD_SEND_MIN_DOLLARS, DD_SEND_MAX_DOLLARS,
                                            DD_SEND_DECIMAL_PLACES, this);

    // Setup sections
    setupCoinControlSection();
    setupAddressSection();
    setupAmountSection();
    setupNoteSection();
    setupFeeSection();
    setupButtonSection();

    // Add stretch to push content to top
    m_mainLayout->addStretch();

    setLayout(m_mainLayout);
}

void DigiDollarSendWidget::setupCoinControlSection()
{
    // Create coin control frame
    m_coinControlFrame = new QFrame(this);
    m_coinControlFrame->setObjectName("coinControlFrame");
    m_coinControlFrame->setFrameStyle(QFrame::StyledPanel);
    m_coinControlFrame->setFrameShadow(QFrame::Sunken);

    m_coinControlLayout = new QHBoxLayout(m_coinControlFrame);
    m_coinControlLayout->setSpacing(10);
    m_coinControlLayout->setContentsMargins(10, 8, 10, 8);

    // "Inputs..." button to open coin control dialog
    m_coinControlButton = new QPushButton(tr("Inputs..."), this);
    m_coinControlButton->setObjectName("coinControlButton");
    m_coinControlButton->setToolTip(tr("Manually select $DD inputs to spend"));
    m_coinControlButton->setMinimumWidth(80);
    m_coinControlLayout->addWidget(m_coinControlButton);

    // Quantity label (number of selected inputs)
    m_coinControlQuantityLabel = new QLabel(this);
    m_coinControlQuantityLabel->setObjectName("coinControlQuantityLabel");
    m_coinControlQuantityLabel->setText(tr("automatically selected"));
    m_coinControlLayout->addWidget(m_coinControlQuantityLabel);

    // Amount label (total selected DD amount)
    m_coinControlAmountLabel = new QLabel(this);
    m_coinControlAmountLabel->setObjectName("coinControlAmountLabel");
    m_coinControlAmountLabel->setText(QString());
    m_coinControlLayout->addWidget(m_coinControlAmountLabel);

    m_coinControlLayout->addStretch();

    m_mainLayout->addWidget(m_coinControlFrame);

    // Initially visible - will be hidden if coin control is disabled in settings
    m_coinControlFrame->setVisible(true);
}

void DigiDollarSendWidget::setupAddressSection()
{
    // Create address frame
    m_addressFrame = new QFrame(this);
    m_addressFrame->setFrameStyle(QFrame::StyledPanel);
    m_addressFrame->setFrameShadow(QFrame::Sunken);
    m_addressFrame->setObjectName("addressFrame");

    m_addressLayout = new QGridLayout(m_addressFrame);
    m_addressLayout->setSpacing(8);
    m_addressLayout->setContentsMargins(10, 10, 10, 10);
    m_addressLayout->setHorizontalSpacing(12);
    m_addressLayout->setVerticalSpacing(8);

    // Address label and input with paste button
    m_addressLabel = new QLabel(tr("Pay To:"), this);
    m_addressLabel->setToolTip(tr("Enter the DigiDollar address of the recipient"));
    m_addressLabel->setBuddy(m_addressEdit);

    // Create horizontal layout for address input and paste button
    QHBoxLayout* addressInputLayout = new QHBoxLayout();
    addressInputLayout->setSpacing(0);

    m_addressEdit = new QLineEdit(this);
    m_addressEdit->setObjectName("addressEdit");
    m_addressEdit->setValidator(m_addressValidator);
    m_addressEdit->setPlaceholderText("DD1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");
    m_addressEdit->setToolTip(tr("The DigiDollar address to send the payment to.\n\nValid formats:\n• DD... (Mainnet)\n• TD... (Testnet)\n• RD... (Regtest)"));
    m_addressEdit->setFocusPolicy(Qt::StrongFocus);
    m_addressEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_addressEdit->setFont(monospaceFont);

    m_pasteAddressButton = new QToolButton(this);
    m_pasteAddressButton->setToolTip(tr("Paste address from clipboard (Alt+P)"));
    m_pasteAddressButton->setIconSize(QSize(22, 22));
    m_pasteAddressButton->setShortcut(QKeySequence("Alt+P"));
    m_pasteAddressButton->setIcon(m_platformStyle->SingleColorIcon(":/icons/editpaste"));

    m_addressBookButton = new QToolButton(this);
    m_addressBookButton->setToolTip(tr("Choose from address book (Alt+A)"));
    m_addressBookButton->setIconSize(QSize(22, 22));
    m_addressBookButton->setShortcut(QKeySequence("Alt+A"));
    m_addressBookButton->setIcon(m_platformStyle->SingleColorIcon(":/icons/address-book"));

    addressInputLayout->addWidget(m_addressEdit);
    addressInputLayout->addWidget(m_addressBookButton);
    addressInputLayout->addWidget(m_pasteAddressButton);

    m_addressLayout->addWidget(m_addressLabel, 0, 0);
    m_addressLayout->addLayout(addressInputLayout, 0, 1);

    // Address validation label
    m_addressValidationLabel = new QLabel(this);
    m_addressValidationLabel->setObjectName("addressValidationLabel");
    m_addressValidationLabel->setText(tr("Enter a valid DigiDollar address (DD, TD, or RD prefix)"));
    m_addressValidationLabel->setWordWrap(true);
    m_addressLayout->addWidget(m_addressValidationLabel, 1, 1);

    m_mainLayout->addWidget(m_addressFrame);
}

void DigiDollarSendWidget::setupAmountSection()
{
    // Create amount frame
    m_amountFrame = new QFrame(this);
    m_amountFrame->setFrameStyle(QFrame::StyledPanel);
    m_amountFrame->setFrameShadow(QFrame::Sunken);
    m_amountFrame->setObjectName("amountFrame");

    m_amountLayout = new QGridLayout(m_amountFrame);
    m_amountLayout->setSpacing(8);
    m_amountLayout->setContentsMargins(10, 10, 10, 10);
    m_amountLayout->setHorizontalSpacing(12);
    m_amountLayout->setVerticalSpacing(8);

    // Amount input with use available balance button
    m_amountLabel = new QLabel(tr("Amount:"), this);
    m_amountLabel->setToolTip(tr("Enter the amount of DigiDollar to send"));
    m_amountLabel->setBuddy(m_amountEdit);

    m_amountEdit = new QLineEdit(this);
    m_amountEdit->setObjectName("amountEdit");
    m_amountEdit->setValidator(m_amountValidator);
    m_amountEdit->setPlaceholderText("0.00");
    m_amountEdit->setToolTip(tr("The amount of DigiDollar to send.\n\n• Minimum: 1.00 $DD\n• Maximum: 100,000.00 $DD\n• Up to 2 decimal places (cents)"));
    m_amountEdit->setFocusPolicy(Qt::StrongFocus);
    m_amountEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_amountEdit->setFont(monospaceFont);

    // Add stretch to push the button to the right
    m_useAvailableBalanceButton = new QPushButton(tr("Use available balance"), this);
    m_useAvailableBalanceButton->setObjectName("useAvailableBalanceButton");
    m_useAvailableBalanceButton->setToolTip(tr("Use the full available DigiDollar balance (fees are paid in DGB)"));

    // Keep the input in the grid so its full themed height sets the row height.
    m_amountLayout->addWidget(m_amountLabel, 0, 0);
    m_amountLayout->addWidget(m_amountEdit, 0, 1);
    m_amountLayout->addWidget(m_useAvailableBalanceButton, 0, 2);

    // The line that says what is wrong with the amount, under the box it is
    // about. Without it a refused amount showed only a red border and the user
    // had no way to tell whether it was too small, too large, or more than the
    // balance.
    m_amountValidationLabel = new QLabel(this);
    m_amountValidationLabel->setObjectName("amountValidationLabel");
    m_amountValidationLabel->setWordWrap(true);
    m_amountLayout->addWidget(m_amountValidationLabel, 1, 1, 1, 2);

    // USD equivalent display
    m_usdEquivalentLabel = new QLabel(tr("$USD Equivalent:"), this);
    m_usdEquivalentLabel->setObjectName("usdEquivalentLabel");
    m_usdEquivalentLabel->setToolTip(tr("Equivalent value in US Dollars (DigiDollar is pegged to $1 USD)"));
    m_usdEquivalentValue = new QLabel("0.00 $USD", this);
    m_usdEquivalentValue->setObjectName("usdEquivalentValue");
    m_usdEquivalentValue->setFont(monospaceFont);
    m_usdEquivalentValue->setToolTip(tr("USD value updates in real-time as you type"));

    m_amountLayout->addWidget(m_usdEquivalentLabel, 2, 0);
    m_amountLayout->addWidget(m_usdEquivalentValue, 2, 1, 1, 2);

    // Available balance display
    m_availableBalanceLabel = new QLabel(tr("Available:"), this);
    m_availableBalanceLabel->setObjectName("availableBalanceLabel");
    m_availableBalanceLabel->setToolTip(tr("Your current available DigiDollar balance"));
    m_availableBalanceValue = new QLabel("0.00 $DD", this);
    m_availableBalanceValue->setObjectName("availableBalanceValue");
    m_availableBalanceValue->setFont(monospaceFont);
    m_availableBalanceValue->setToolTip(tr("Your current spendable DigiDollar balance"));

    m_amountLayout->addWidget(m_availableBalanceLabel, 3, 0);
    m_amountLayout->addWidget(m_availableBalanceValue, 3, 1, 1, 2);

    // Set column widths to prevent layout distortion on initial display
    m_amountLayout->setColumnMinimumWidth(0, 110);  // Label column
    m_amountLayout->setColumnStretch(0, 0);
    m_amountLayout->setColumnStretch(1, 0);
    m_amountLayout->setColumnStretch(2, 1);

    m_mainLayout->addWidget(m_amountFrame);
}

void DigiDollarSendWidget::setupNoteSection()
{
    m_noteFrame = new QFrame(this);
    m_noteFrame->setFrameStyle(QFrame::StyledPanel);
    m_noteFrame->setFrameShadow(QFrame::Sunken);
    m_noteFrame->setObjectName("noteFrame");

    m_noteLayout = new QGridLayout(m_noteFrame);
    m_noteLayout->setSpacing(8);
    m_noteLayout->setContentsMargins(10, 10, 10, 10);
    m_noteLayout->setHorizontalSpacing(12);
    m_noteLayout->setVerticalSpacing(8);

    m_noteLabel = new QLabel(tr("Note:"), this);
    m_noteLabel->setToolTip(tr("Enter a local note for this DigiDollar transaction"));

    m_noteEdit = new QLineEdit(this);
    m_noteEdit->setObjectName("noteEdit");
    m_noteEdit->setPlaceholderText(tr("Enter a local note for this transaction"));
    m_noteEdit->setMaxLength(256);
    m_noteEdit->setToolTip(tr("Saved locally with this DigiDollar transaction"));

    m_noteLayout->addWidget(m_noteLabel, 0, 0);
    m_noteLayout->addWidget(m_noteEdit, 0, 1);

    m_noteLayout->setColumnMinimumWidth(0, 110);
    m_noteLayout->setColumnStretch(0, 0);
    m_noteLayout->setColumnStretch(1, 1);

    m_mainLayout->addWidget(m_noteFrame);
}

void DigiDollarSendWidget::setupFeeSection()
{
    // Create fee frame
    m_feeFrame = new QFrame(this);
    m_feeFrame->setFrameStyle(QFrame::StyledPanel);
    m_feeFrame->setFrameShadow(QFrame::Sunken);
    m_feeFrame->setObjectName("feeFrame");

    m_feeLayout = new QGridLayout(m_feeFrame);
    m_feeLayout->setSpacing(8);
    m_feeLayout->setContentsMargins(10, 10, 10, 10);
    m_feeLayout->setHorizontalSpacing(12);
    m_feeLayout->setVerticalSpacing(8);

    // Fee display
    m_feeLabel = new QLabel(tr("Transaction fee:"), this);
    m_feeLabel->setObjectName("feeLabel");
    m_feeLabel->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_feeLabel->setToolTip(tr("Network fee paid in DGB (not deducted from $DD amount)"));
    m_feeValue = new QLabel("~0.1 DGB", this);
    m_feeValue->setObjectName("feeValue");
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_feeValue->setFont(monospaceFont);
    m_feeValue->setToolTip(tr("Estimated network fee paid in DGB from your DGB balance"));

    m_feeLayout->addWidget(m_feeLabel, 0, 0);
    m_feeLayout->addWidget(m_feeValue, 0, 1);

    // Total amount display
    m_totalLabel = new QLabel(tr("Total $DD:"), this);
    m_totalLabel->setObjectName("totalLabel");
    m_totalLabel->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_totalLabel->setToolTip(tr("Total DigiDollar amount to send (fee is paid separately in DGB)"));
    QFont boldFont = m_totalLabel->font();
    boldFont.setBold(true);
    m_totalLabel->setFont(boldFont);

    m_totalValue = new QLabel("0.00 $DD", this);
    m_totalValue->setObjectName("totalValue");
    m_totalValue->setFont(monospaceFont);
    m_totalValue->setToolTip(tr("Total $DD to send (DGB fee is paid from your DGB balance)"));

    m_feeLayout->addWidget(m_totalLabel, 1, 0);
    m_feeLayout->addWidget(m_totalValue, 1, 1);

    m_mainLayout->addWidget(m_feeFrame);
}

void DigiDollarSendWidget::setupButtonSection()
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
    // Removed minimum height - CSS handles button sizing
    m_buttonLayout->addWidget(m_clearButton);

    // Add stretch to push send button to the right
    m_buttonLayout->addStretch();

    // Send button - styled to match main wallet
    m_sendButton = new QPushButton(tr("S&end DigiDollar"), this);
    m_sendButton->setObjectName("sendButton");
    m_sendButton->setEnabled(false);
    m_sendButton->setDefault(true);
    m_sendButton->setAutoDefault(true);
    // Removed minimum height - CSS handles button sizing
    m_sendButton->setToolTip(tr("Confirm and send this DigiDollar transaction"));
    m_buttonLayout->addWidget(m_sendButton);

    m_mainLayout->addWidget(m_buttonFrame);
}

void DigiDollarSendWidget::connectSignals()
{
    // Connect address validation
    connect(m_addressEdit, &QLineEdit::textChanged,
            this, &DigiDollarSendWidget::onAddressChanged);

    // Connect amount validation
    connect(m_amountEdit, &QLineEdit::textChanged,
            this, &DigiDollarSendWidget::onAmountChanged);

    // Connect buttons
    connect(m_sendButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onSendClicked);
    connect(m_clearButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onClearClicked);
    connect(m_useAvailableBalanceButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onUseAvailableBalanceClicked);
    connect(m_pasteAddressButton, &QToolButton::clicked,
            this, &DigiDollarSendWidget::onPasteAddressClicked);
    connect(m_addressBookButton, &QToolButton::clicked,
            this, &DigiDollarSendWidget::onAddressBookClicked);

    // Connect coin control button
    connect(m_coinControlButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onCoinControlButtonClicked);
}

void DigiDollarSendWidget::setWalletModel(WalletModel* model)
{
    m_walletModel = model;

    if (m_walletModel) {
        // Connect wallet model signals
        updateBalance();
        // REMOVED: applyTheme() - Let CSS handle all theming
    }
}

void DigiDollarSendWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    if (m_clientModel) {
        // Connect client model signals
        updateOraclePrice();
        // REMOVED: applyTheme() - Let CSS handle all theming
    }
}

void DigiDollarSendWidget::updateView()
{
    updateBalance();
    updateOraclePrice();
    updateFeeDisplay();
}

void DigiDollarSendWidget::updateBalance()
{
    if (m_walletModel) {
        // Only confirmed DD is spendable. Pending DD, including wallet-created
        // transfer change, must confirm before another DD spend can use it.
        CAmount balanceCents = m_walletModel->getDigiDollarBalance();
        m_availableBalance = balanceCents / 100.0; // Convert cents to DD
    } else {
        m_availableBalance = 0.0;
    }

    if (m_privacy) {
        m_availableBalanceValue->setText(maskValue(formatDDAmount(0)));
    } else {
        m_availableBalanceValue->setText(formatDDAmount(m_availableBalance));
    }

    // Update button state - enable if user has any DD balance
    // Fees are paid in DGB, not DD, so no need to check for fee deduction
    m_useAvailableBalanceButton->setEnabled(m_availableBalance > 0);

    // Re-validate amount against updated balance so the border color
    // refreshes when pending DD confirms (fixes stale yellow warning).
    updateAmountValidation();
    updateSendButton();
}

void DigiDollarSendWidget::updateOraclePrice()
{
    if (!isVisible()) return;
    // Get oracle price from RPC for testnet/mainnet, MockOracleManager for regtest
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        // BUG #6 FIX: GetCurrentPrice() returns micro-USD, not cents
        CAmount priceMicroUsd = MockOracleManager::GetInstance().GetCurrentPrice();
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
            LogPrintf("DigiDollar Send: updateOraclePrice RPC error - %s\n", e.write());
            m_oraclePrice = 0.0;
        } catch (const std::exception& e) {
            LogPrintf("DigiDollar Send: updateOraclePrice error - %s\n", e.what());
            m_oraclePrice = 0.0;
        } catch (...) {
            LogPrintf("DigiDollar Send: updateOraclePrice unknown error\n");
            m_oraclePrice = 0.0;
        }
    } else {
        m_oraclePrice = 0.0;
    }

    updateUSDEquivalent();
}

void DigiDollarSendWidget::onAddressChanged()
{
    QString address = m_addressEdit->text();

    updateAddressValidation();

    updateSendButton();
}

void DigiDollarSendWidget::onAmountChanged()
{
    QString amountText = m_amountEdit->text();

    // Validate amount format
    updateAmountValidation();

    updateUSDEquivalent();
    updateFeeDisplay();
    updateSendButton();
}

void DigiDollarSendWidget::onSendClicked()
{
    // PHASE 7.3: Comprehensive input validation with user-friendly messages
    QString address = m_addressEdit->text().trimmed();
    QString amountText = m_amountEdit->text().trimmed();

    // Error: Empty fields
    if (address.isEmpty()) {
        showError(tr("Missing Address"),
                  tr("Please enter a DigiDollar address.\n\nThe recipient's DD address is required to send DigiDollar."));
        m_addressEdit->setFocus();
        return;
    }

    if (amountText.isEmpty()) {
        showError(tr("Missing Amount"),
                  tr("Please enter an amount to send.\n\nSpecify how much DigiDollar you want to send (e.g., 100.00)."));
        m_amountEdit->setFocus();
        return;
    }

    // Error: Invalid address format
    if (!validateAddress()) {
        showError(tr("Invalid DigiDollar Address"),
                  tr("The address format is invalid.\n\n"
                     "Enter a valid DigiDollar address for the active network.\n\n"
                     "Please check the address and try again."));
        m_addressEdit->setFocus();
        return;
    }

    // Error: Invalid amount format
    if (!validateAmount()) {
        showError(tr("Invalid Amount"),
                  tr("The amount is invalid.\n\n"
                     "Valid amount format:\n"
                     "• Positive number\n"
                     "• Maximum 2 decimal places\n"
                     "• Between 1.00 $DD and 100,000.00 $DD\n\n"
                     "Please enter a valid amount."));
        m_amountEdit->setFocus();
        return;
    }

    double amount = amountText.toDouble();

    // Error: Insufficient balance
    if (!validateBalance()) {
        showError(tr("Insufficient DigiDollar Balance"),
                  tr("You don't have enough DigiDollar for this transfer.\n\n"
                     "Available balance: %1\n"
                     "Amount to send: %2\n\n"
                     "Note: Transaction fees are paid in DGB (not $DD).\n\n"
                     "Please enter a smaller amount or add more $DD to your wallet.")
                  .arg(formatDDAmount(m_availableBalance))
                  .arg(formatDDAmount(amount)));
        m_amountEdit->setFocus();
        return;
    }

    if (m_coinControl && m_coinControl->HasSelected()) {
        const CAmount amount_cents = static_cast<CAmount>(std::llround(amount * 100));
        const CAmount selected_amount = selectedDigiDollarAmount();
        if (selected_amount < amount_cents) {
            showError(tr("Insufficient Selected DigiDollar Inputs"),
                      tr("The selected DigiDollar inputs total %1, but this send requires %2.\n\n"
                         "Select more inputs or clear manual input selection.")
                          .arg(formatDDAmount(selected_amount / 100.0))
                          .arg(formatDDAmount(amount)));
            return;
        }
    }

    // PHASE 7.3: Wallet state validation
    if (!checkWalletState()) {
        return; // Error already displayed by checkWalletState()
    }

    // PHASE 7.2: Enhanced confirmation dialog with fee display
    if (!showConfirmationDialog(address, amount)) {
        return; // User cancelled
    }

    // Unlock wallet if encrypted — UnlockContext MUST stay in scope through executeTransfer()
    WalletModel::UnlockContext ctx(m_walletModel->requestUnlock());
    if (!ctx.isValid()) {
        // User cancelled the unlock dialog
        return;
    }

    // PHASE 7.3: Execute transfer with progress indicator
    executeTransfer(address, amount);
}

void DigiDollarSendWidget::onClearClicked()
{
    m_addressEdit->clear();
    m_amountEdit->clear();
    if (m_noteEdit) m_noteEdit->clear();
    onAddressChanged();
    onAmountChanged();
}

void DigiDollarSendWidget::onUseAvailableBalanceClicked()
{
    // FIXED: Fees are paid in DGB, not DD!
    // Users can send their ENTIRE DD balance without any deduction
    if (m_availableBalance > 0) {
        // Set amount to full available balance (fees are paid separately in DGB)
        m_amountEdit->setText(QString::number(m_availableBalance, 'f', 2));
        onAmountChanged();
    }
}

void DigiDollarSendWidget::onPasteAddressClicked()
{
    m_addressEdit->setText(QApplication::clipboard()->text());
    onAddressChanged();
}

void DigiDollarSendWidget::onAddressBookClicked()
{
    if (!m_walletModel) return;

    DDAddressBookPage dlg(m_platformStyle, DDAddressBookPage::ForSelection, this);
    dlg.setWalletModel(m_walletModel);
    if (dlg.exec() == QDialog::Accepted) {
        QString address = dlg.getReturnValue();
        if (!address.isEmpty()) {
            m_addressEdit->setText(address);
            onAddressChanged();
        }
    }
}

// REMOVED: applyTheme() method
// All theming is now handled by light.css and dark.css files
// This allows the DigiByte blue theme to work properly

void DigiDollarSendWidget::updateSendButton()
{
    bool addressValid = validateAddress();
    bool amountValid = validateAmount();
    bool balanceValid = validateBalance();

    m_sendButton->setEnabled(addressValid && amountValid && balanceValid);
}

void DigiDollarSendWidget::updateUSDEquivalent()
{
    QString amountText = m_amountEdit->text();
    if (!amountText.isEmpty()) {
        double amount = amountText.toDouble();
        double usdValue = amount * 1.0; // DD should be pegged to $1
        m_usdEquivalentValue->setText(formatUSDAmount(usdValue));
    } else {
        m_usdEquivalentValue->setText("0.00 $USD");
    }
    // REMOVED: All programmatic styling - Let CSS handle theming
}

void DigiDollarSendWidget::updateFeeDisplay()
{
    // FIXED: Display fee in DGB, not DD
    // Fee is paid from DGB balance, not deducted from DD amount
    m_feeValue->setText(QString("~0.1 DGB"));

    QString amountText = m_amountEdit->text();
    if (!amountText.isEmpty()) {
        double amount = amountText.toDouble();
        // Total DD sent equals the amount entered (fees don't reduce DD amount)
        m_totalValue->setText(formatDDAmount(amount));
    } else {
        m_totalValue->setText(formatDDAmount(0));
    }

}

bool DigiDollarSendWidget::validateAddress() const
{
    QString address = m_addressEdit->text();
    int pos = 0;
    QString addressCopy = address;
    return m_addressValidator->validate(addressCopy, pos) == QValidator::Acceptable;
}

bool DigiDollarSendWidget::validateAmount() const
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return false;

    int pos = 0;
    QString amountCopy = amountText;
    return m_amountValidator->validate(amountCopy, pos) == QValidator::Acceptable;
}

bool DigiDollarSendWidget::validateBalance() const
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return true; // Empty is valid for enabling/disabling

    double amount = amountText.toDouble();
    // FIXED: Fees are paid in DGB, not DD!
    // Only check if the DD amount is available, don't add fee to the check
    bool valid = amount <= m_availableBalance && amount > 0;

    // Note: Cannot call updateAmountValidation() from const method

    return valid;
}

QString DigiDollarSendWidget::formatDDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $DD";
}

QString DigiDollarSendWidget::formatUSDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $USD";
}

// PHASE 7.3: Error display helper
void DigiDollarSendWidget::showError(const QString& title, const QString& message)
{
    QMessageBox msgBox(this);
    msgBox.setIcon(QMessageBox::Critical);
    msgBox.setWindowTitle(title);
    msgBox.setText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();

    // Log for debugging
    LogPrintf("DigiDollar GUI Error: %s - %s\n",
              title.toStdString(), message.toStdString());
}

// PHASE 7.3: Warning display helper
void DigiDollarSendWidget::showWarning(const QString& title, const QString& message)
{
    QMessageBox msgBox(this);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle(title);
    msgBox.setText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();

    // Log for debugging
    LogPrintf("DigiDollar GUI Warning: %s - %s\n",
              title.toStdString(), message.toStdString());
}

// PHASE 7.3: Wallet state validation
bool DigiDollarSendWidget::checkWalletState()
{
    if (!m_walletModel) {
        showError(tr("Wallet Error"),
                  tr("Wallet is not available.\n\nPlease ensure your wallet is properly loaded."));
        return false;
    }

    // NOTE: Wallet unlock is handled in onSendClicked() where the UnlockContext
    // stays in scope through executeTransfer(). Do NOT unlock here — the context
    // would die when checkWalletState() returns, re-locking before the transaction.

    // Check if DigiDollar wallet initialized (balance check serves as proxy)
    if (m_availableBalance < 0) {
        showError(tr("DigiDollar Not Available"),
                 tr("DigiDollar wallet is not initialized.\n\n"
                    "This may indicate a wallet initialization error."));
        return false;
    }

    return true;
}

// PHASE 7.2: Enhanced confirmation dialog with 3-second countdown
// This matches the DGB send confirmation flow exactly
bool DigiDollarSendWidget::showConfirmationDialog(const QString& address, double amount)
{
    double usdEquivalent = amount * 1.0; // DD should be pegged to $1

    // Create confirmation title - matches DGB "Confirm send coins"
    QString title = tr("Confirm send DigiDollar");

    // Build confirmation message following DGB format exactly
    QString question_string;

    // Main question - matches DGB
    question_string.append(tr("Do you want to send this DigiDollar transaction?"));
    question_string.append("<br /><span style='font-size:10pt;'>");
    question_string.append(tr("Please, review your transaction."));
    question_string.append("</span>%1");

    if (m_coinControl && m_coinControl->HasSelected()) {
        const int selected_count = static_cast<int>(m_coinControl->ListSelected().size());
        const CAmount selected_amount = selectedDigiDollarAmount();
        question_string.append("<hr /><b>");
        question_string.append(tr("Selected $DD inputs"));
        question_string.append("</b>: ");
        question_string.append(tr("%1 input(s), %2 selected")
            .arg(selected_count)
            .arg(formatDDAmount(selected_amount / 100.0)));
    }

    // Transaction fee section - matches DGB format exactly
    question_string.append("<hr /><b>");
    question_string.append(tr("Transaction fee"));
    question_string.append("</b>");

    // Append fee estimate - note: DD transactions are similar size to DGB, ~0.1 DGB fee
    question_string.append(" (~250 vB): ");

    // Fee value in red bold - matches DGB styling exactly
    question_string.append("<span style='color:#aa0000; font-weight:bold;'>");
    question_string.append("~0.1 DGB");
    question_string.append("</span><br />");

    // Total amount section - matches DGB format
    question_string.append("<hr />");
    question_string.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
        .arg(formatDDAmount(amount)));

    // USD equivalent as alternative unit - matches DGB's "or" format
    question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
        .arg(formatUSDAmount(usdEquivalent)));

    // Recipient details - formatted to match DGB's recipient format
    QString recipientElement;
    recipientElement.append(tr("%1 to %2").arg(formatDDAmount(amount), address));

    // Insert recipient details into placeholder
    question_string = question_string.arg("<br /><br />" + recipientElement);

    // Informative text - empty for single recipient like DGB
    QString informative_text = "";

    // Create confirmation dialog with 3-second countdown - matches DGB exactly
    auto confirmationDialog = new DDSendConfirmationDialog(
        title,
        question_string,
        informative_text,
        DD_SEND_CONFIRM_DELAY,
        this
    );
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);

    int result = confirmationDialog->exec();

    if (result == QMessageBox::Yes) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: User confirmed transfer of %f DD to %s after 3-second review\n",
                  amount, address.toStdString());
        return true;
    } else {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: User cancelled transfer\n");
        return false;
    }
}

// PHASE 7.3: Execute transfer with progress indicator and error handling
void DigiDollarSendWidget::executeTransfer(const QString& address, double amount)
{
    // Show progress dialog
    QProgressDialog progress(tr("Sending DigiDollar..."),
                            tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0); // Show immediately
    progress.setCancelButton(nullptr); // No cancel during transfer
    progress.show();
    QApplication::processEvents(); // Force update

    // Disable UI during transfer
    m_sendButton->setEnabled(false);
    m_addressEdit->setEnabled(false);
    m_amountEdit->setEnabled(false);
    m_clearButton->setEnabled(false);
    m_useAvailableBalanceButton->setEnabled(false);

    CAmount amountCents = static_cast<CAmount>(std::llround(amount * 100));
    QString note = m_noteEdit ? m_noteEdit->text().trimmed() : QString();

    std::vector<COutPoint> selectedInputs;
    const std::vector<COutPoint>* presetInputs = nullptr;
    if (m_coinControl && m_coinControl->HasSelected()) {
        selectedInputs = m_coinControl->ListSelected();
        presetInputs = &selectedInputs;
    }
    WalletModel::DigiDollarSendResult result = m_walletModel->sendDigiDollar(address, amountCents, note, presetInputs);

    // Close progress dialog
    progress.close();

    // Re-enable UI
    m_sendButton->setEnabled(true);
    m_addressEdit->setEnabled(true);
    m_amountEdit->setEnabled(true);
    m_clearButton->setEnabled(true);
    m_useAvailableBalanceButton->setEnabled(true);

    // Handle result
    if (result.status == WalletModel::OK) {
        // Success!
        showSuccess(result.txid, amount);
        onClearClicked();
        updateBalance(); // Refresh balance display
    } else {
        // Error occurred - map to user-friendly message
        showBackendError(static_cast<int>(result.status), result.reasonFailed);
    }
}

// PHASE 7.3: Success notification
QString DigiDollarSendWidget::buildSuccessMessage(const QString& txid, double amount) const
{
    return tr(
        "<b style='font-size: 14px; color: green;'>✓ DigiDollar Transfer Broadcast</b><br/><br/>"
        "<table cellpadding='4' style='font-size: 12px;'>"
        "<tr><td><b>Amount Sent:</b></td><td align='right'>%1</td></tr>"
        "<tr><td><b>Transaction ID:</b></td><td style='font-family: monospace; font-size: 10px;'>%2</td></tr>"
        "</table><br/>"
        "<span style='color: #666; font-size: 11px;'>"
        "Your transaction has been broadcast to the network.<br/>"
        "It is pending until miners include it in a block and the block confirms."
        "</span>"
    ).arg(formatDDAmount(amount))
     .arg(txid);
}

void DigiDollarSendWidget::showSuccess(const QString& txid, double amount)
{
    QString successMsg = buildSuccessMessage(txid, amount);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Transfer Broadcast"));
    msgBox.setText(successMsg);
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Transfer broadcast - txid: %s\n", txid.toStdString());
}

// PHASE 7.3: Backend error message mapping
void DigiDollarSendWidget::showBackendError(int status, const QString& reasonFailed)
{
    QString errorTitle;
    QString errorMessage;

    // Map backend errors to user-friendly messages
    switch (status) {
    case WalletModel::InvalidAddress:
        errorTitle = tr("Invalid Address");
        errorMessage = tr(
            "The recipient address is invalid.\n\n"
            "Please check the address format and try again.\n\n"
            "Technical details: %1"
        ).arg(reasonFailed);
        break;

    case WalletModel::InvalidAmount:
        errorTitle = tr("Invalid Amount");
        errorMessage = tr(
            "The transfer amount is invalid.\n\n"
            "Please ensure the amount is:\n"
            "• Greater than 0\n"
            "• Within the maximum limit\n"
            "• Properly formatted\n\n"
            "Technical details: %1"
        ).arg(reasonFailed);
        break;

    case WalletModel::AmountExceedsBalance:
        errorTitle = tr("Insufficient Balance");
        errorMessage = tr(
            "You don't have enough DigiDollar for this transfer.\n\n"
            "%1\n\n"
            "Please:\n"
            "• Enter a smaller amount, or\n"
            "• Add more $DD to your wallet"
        ).arg(reasonFailed);
        break;

    case WalletModel::TransactionCreationFailed:
        errorTitle = tr("Transaction Failed");

        // Parse specific error types
        if (reasonFailed.contains("locked", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Your wallet is locked.\n\n"
                "Please unlock your wallet to send DigiDollar.\n\n"
                "Go to Settings > Unlock Wallet"
            );
        } else if (reasonFailed.contains("coin selection", Qt::CaseInsensitive) ||
                   reasonFailed.contains("SelectDDCoins", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Unable to select DigiDollar for transfer.\n\n"
                "This may occur if:\n"
                "• Your $DD is locked in pending transactions\n"
                "• The requested amount requires too many inputs\n\n"
                "Please try:\n"
                "• Waiting for pending transactions to confirm\n"
                "• Sending a smaller amount\n\n"
                "Technical details: %1"
            ).arg(reasonFailed);
        } else if (reasonFailed.contains("signing", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Transaction signing failed.\n\n"
                "Please ensure:\n"
                "• Your wallet is unlocked\n"
                "• You have the required private keys\n\n"
                "Technical details: %1"
            ).arg(reasonFailed);
        } else if (reasonFailed.contains("mempool", Qt::CaseInsensitive) ||
                   reasonFailed.contains("broadcast", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Transaction was rejected by the network.\n\n"
                "This may occur if:\n"
                "• Network fees are too low\n"
                "• The transaction conflicts with another\n"
                "• Network connectivity issues\n\n"
                "Please try:\n"
                "• Waiting a few moments and trying again\n"
                "• Checking your network connection\n\n"
                "Technical details: %1"
            ).arg(reasonFailed);
        } else {
            // Generic transaction failure
            errorMessage = tr(
                "Failed to create or send the transaction.\n\n"
                "Technical details: %1\n\n"
                "If this problem persists, please check:\n"
                "• Wallet synchronization status\n"
                "• Network connection\n"
                "• Available DigiDollar balance"
            ).arg(reasonFailed);
        }
        break;

    default:
        errorTitle = tr("Transfer Error");
        errorMessage = tr(
            "An unexpected error occurred during transfer.\n\n"
            "Error details: %1\n\n"
            "Please try again or contact support if the issue persists."
        ).arg(reasonFailed);
        break;
    }

    showError(errorTitle, errorMessage);
}

void DigiDollarSendWidget::updateAddressValidation()
{
    QString address = m_addressEdit->text();
    const QPalette palette = this->palette();
    QString midColor = palette.color(QPalette::WindowText).name();
    const bool isDarkTheme = palette.color(QPalette::Window).lightness() < 128;

    QString successColor = isDarkTheme ? "#8fe3b1" : "#147a42";
    QString errorColor = isDarkTheme ? "#ff9090" : "#b42318";

    if (address.isEmpty()) {
        m_addressValidationLabel->setText(tr("Enter a valid DigiDollar address for this network"));
        m_addressValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; }").arg(midColor));
        m_addressEdit->setStyleSheet("");
    } else if (validateAddress()) {
        m_addressValidationLabel->setText(tr("✓ Valid DigiDollar address"));
        m_addressValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(successColor));
        m_addressEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
    } else {
        m_addressValidationLabel->setText(tr("✗ Invalid DigiDollar address for this network"));
        m_addressValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(errorColor));
        m_addressEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(errorColor));
    }
}

QString DigiDollarSendWidget::amountProblem() const
{
    const QString amountText = m_amountEdit->text().trimmed();
    if (amountText.isEmpty()) {
        return QString();
    }

    bool parsed = false;
    const double amount = amountText.toDouble(&parsed);
    if (!parsed) {
        return tr("Enter the amount in $DD, for example 25.00.");
    }

    const int decimalPoint = amountText.indexOf(QLatin1Char('.'));
    if (decimalPoint >= 0 && amountText.length() - decimalPoint - 1 > DD_SEND_DECIMAL_PLACES) {
        return tr("DigiDollar goes down to cents, so use at most two decimal places.");
    }
    if (amount < DD_SEND_MIN_DOLLARS) {
        return tr("The smallest amount you can send is %1.").arg(formatDDAmount(DD_SEND_MIN_DOLLARS));
    }
    if (amount > DD_SEND_MAX_DOLLARS) {
        return tr("The most you can send in one transfer is %1.").arg(formatDDAmount(DD_SEND_MAX_DOLLARS));
    }
    if (amount > m_availableBalance) {
        // Privacy mode hides the balance everywhere else on this form, so it
        // must stay hidden here too.
        const QString available = m_privacy ? maskValue(formatDDAmount(0)) : formatDDAmount(m_availableBalance);
        return tr("You only have %1 to send.").arg(available);
    }
    return QString();
}

void DigiDollarSendWidget::updateAmountValidation()
{
    QString amountText = m_amountEdit->text();
    const QPalette palette = this->palette();
    QString midColor = palette.color(QPalette::WindowText).name();
    const bool isDarkTheme = palette.color(QPalette::Window).lightness() < 128;

    QString successColor = isDarkTheme ? "#8fe3b1" : "#147a42";
    QString warningColor = isDarkTheme ? "#ffd166" : "#805500";
    QString errorColor = isDarkTheme ? "#ff9090" : "#b42318";

    const QString problem = amountProblem();

    if (amountText.trimmed().isEmpty()) {
        // Nothing typed yet: say what the box will accept.
        m_amountEdit->setStyleSheet("");
        m_amountValidationLabel->setText(tr("Enter an amount between %1 and %2")
                                             .arg(formatDDAmount(DD_SEND_MIN_DOLLARS))
                                             .arg(formatDDAmount(DD_SEND_MAX_DOLLARS)));
        m_amountValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; }").arg(midColor));
        return;
    }

    if (problem.isEmpty()) {
        m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
        m_amountValidationLabel->setText(tr("✓ Amount can be sent"));
        m_amountValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(successColor));
        return;
    }

    // Too little, too much or badly written is a rule of the form itself, so it
    // is shown as an error. Having less than you asked to send is shown as a
    // warning, because the balance can change and then the same amount is fine.
    const bool overBalance = validateAmount() && !validateBalance();
    const QString colour = overBalance ? warningColor : errorColor;
    m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(colour));
    m_amountValidationLabel->setText(QStringLiteral("✗ ") + problem);
    m_amountValidationLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(colour));
}

// DDSendConfirmationDialog implementation
// Matches the DGB send confirmation dialog with 3-second countdown exactly
DDSendConfirmationDialog::DDSendConfirmationDialog(const QString& title, const QString& text,
                                                     const QString& informative_text,
                                                     int secDelay, QWidget* parent)
    : QMessageBox(parent), secDelay(secDelay)
{
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setText(text);
    setInformativeText(informative_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    confirmButtonText = yesButton->text();  // Get standard button text (like DGB)
    updateButtons();
    connect(&countDownTimer, &QTimer::timeout, this, &DDSendConfirmationDialog::countDown);
}

int DDSendConfirmationDialog::exec()
{
    updateButtons();
    countDownTimer.start(1s);
    return QMessageBox::exec();
}

void DDSendConfirmationDialog::countDown()
{
    secDelay--;
    updateButtons();

    if (secDelay <= 0) {
        countDownTimer.stop();
    }
}

void DDSendConfirmationDialog::updateButtons()
{
    if (secDelay > 0) {
        // Disable button and show countdown
        yesButton->setEnabled(false);
        yesButton->setText(confirmButtonText + QString(" (%1)").arg(secDelay));
    } else {
        // Enable button and remove countdown
        yesButton->setEnabled(true);
        yesButton->setText(confirmButtonText);
    }
}

// DigiDollarAddressValidator implementation
DigiDollarAddressValidator::DigiDollarAddressValidator(QObject* parent) :
    QValidator(parent)
{
}

namespace {

// How long a DigiDollar address is. Every one is the same length: two version
// bytes and a 32 byte key, written in base58 with a four byte checksum. That
// comes out the same on mainnet, testnet and regtest. Measure a real address
// rather than writing the number down here, so this stays right if the address
// format ever changes. Returns 0 if an address cannot be made, and then no
// length limit is applied.
int DigiDollarAddressLength()
{
    uint256 sampleKey;
    sampleKey.SetHex("0101010101010101010101010101010101010101010101010101010101010101");
    const std::string sample =
        EncodeDigiDollarAddress(CTxDestination{WitnessV1Taproot(XOnlyPubKey(sampleKey))});
    return static_cast<int>(sample.size());
}

// The first two letters say which network an address is for: DD for mainnet,
// TD for testnet, RD for regtest. Text that does not start that way, or that
// is not itself the start of one of those, can never become an address.
bool CouldBeStartOfDigiDollarAddress(const QString& input)
{
    for (const QString& prefix : {QStringLiteral("DD"), QStringLiteral("TD"), QStringLiteral("RD")}) {
        // Shorter than the prefix means the text is still being typed, so it
        // only has to match as far as it goes.
        const bool matches = input.length() < prefix.length() ? prefix.startsWith(input)
                                                              : input.startsWith(prefix);
        if (matches) return true;
    }
    return false;
}

} // namespace

QValidator::State DigiDollarAddressValidator::validate(QString& input, int& pos) const
{
    Q_UNUSED(pos)

    if (input.isEmpty()) {
        return QValidator::Intermediate;
    }

    if (isValidDDAddress(input)) {
        return QValidator::Acceptable;
    }

    // The text is not an address for this network. Refuse the keystroke only
    // when the text can never become one: it does not start the way an address
    // starts, or it is longer than an address. Anything else is kept, so a
    // whole address can be typed a character at a time and a mistake can be
    // corrected in place. The form's own message says whether what is in the
    // box is an address for this network, and the Send button stays off until
    // it is.
    const int fullLength = DigiDollarAddressLength();
    if (fullLength > 0 && input.length() > fullLength) {
        return QValidator::Invalid;
    }
    if (!CouldBeStartOfDigiDollarAddress(input)) {
        return QValidator::Invalid;
    }

    return QValidator::Intermediate;
}

bool DigiDollarAddressValidator::isValidDDAddress(const QString& address) const
{
    return CDigiDollarAddress::IsValidDigiDollarAddressForCurrentNetwork(address.toStdString());
}

void DigiDollarSendWidget::onCoinControlButtonClicked()
{
    if (!m_walletModel) {
        return;
    }

    // Initialize coin control if not already done
    if (!m_coinControl) {
        m_coinControl = std::make_unique<wallet::DDCoinControl>();
    }

    // Open the coin control dialog
    // Note: platformStyle would typically come from the main window
    DigiDollarCoinControlDialog dlg(*m_coinControl, m_walletModel, nullptr, this);
    dlg.exec();

    // Update labels after dialog closes
    updateCoinControlLabels();
}

void DigiDollarSendWidget::updateCoinControlLabels()
{
    if (!m_coinControl || !m_walletModel) {
        // No coin control active, show automatic selection state.
        if (m_coinControlQuantityLabel) {
            m_coinControlQuantityLabel->setText(tr("automatically selected"));
            m_coinControlQuantityLabel->setVisible(true);
        }
        if (m_coinControlAmountLabel) {
            m_coinControlAmountLabel->clear();
            m_coinControlAmountLabel->setVisible(true);
        }
        updateFeeDisplay();
        return;
    }

    if (!m_coinControl->HasSelected()) {
        if (m_coinControlQuantityLabel) {
            m_coinControlQuantityLabel->setText(tr("automatically selected"));
            m_coinControlQuantityLabel->setVisible(true);
        }
        if (m_coinControlAmountLabel) {
            m_coinControlAmountLabel->clear();
            m_coinControlAmountLabel->setVisible(true);
        }
        updateFeeDisplay();
        return;
    }

    // Count selected inputs and calculate total amount
    std::vector<COutPoint> selectedInputs = m_coinControl->ListSelected();
    int nQuantity = selectedInputs.size();
    const CAmount selectedAmount = selectedDigiDollarAmount();

    // Show quantity label
    if (m_coinControlQuantityLabel) {
        m_coinControlQuantityLabel->setText(tr("Quantity: %1").arg(nQuantity));
        m_coinControlQuantityLabel->setVisible(true);
    }

    if (m_coinControlAmountLabel) {
        m_coinControlAmountLabel->setText(tr("Amount: %1").arg(formatDDAmount(selectedAmount / 100.0)));
        m_coinControlAmountLabel->setVisible(true);
    }

    updateFeeDisplay();
}

CAmount DigiDollarSendWidget::selectedDigiDollarAmount() const
{
    if (!m_coinControl || !m_walletModel) return 0;

    DigiDollarWallet* ddWallet = m_walletModel->getDigiDollarWallet();
    if (!ddWallet) return 0;

    CAmount amount = 0;
    for (const COutPoint& outpoint : m_coinControl->ListSelected()) {
        const CAmount dd_amount = ddWallet->GetDDFromUTXO(outpoint);
        if (dd_amount > 0) amount += dd_amount;
    }
    return amount;
}

void DigiDollarSendWidget::setSelectedDigiDollarInputsForTesting(const std::vector<COutPoint>& inputs)
{
    if (!m_coinControl) {
        m_coinControl = std::make_unique<wallet::DDCoinControl>();
    }
    m_coinControl->UnSelectAll();
    for (const COutPoint& input : inputs) {
        m_coinControl->Select(input);
    }
    updateCoinControlLabels();
}

WalletModel::DigiDollarSendResult DigiDollarSendWidget::sendDigiDollarForTesting(const QString& address, CAmount amount, const QString& comment)
{
    std::vector<COutPoint> selectedInputs;
    const std::vector<COutPoint>* presetInputs = nullptr;
    if (m_coinControl && m_coinControl->HasSelected()) {
        selectedInputs = m_coinControl->ListSelected();
        presetInputs = &selectedInputs;
    }
    return m_walletModel->sendDigiDollar(address, amount, comment, presetInputs);
}

QString DigiDollarSendWidget::successMessageForTesting(const QString& txid, double amount) const
{
    return buildSuccessMessage(txid, amount);
}

void DigiDollarSendWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    updateBalance();
    if (m_privacy) {
        m_usdEquivalentValue->setText(maskValue(formatUSDAmount(0)));
        m_feeValue->setText(maskValue(QString("~0.1 DGB")));
        m_totalValue->setText(maskValue(formatDDAmount(0)));
    } else {
        updateUSDEquivalent();
        updateFeeDisplay();
    }
}

QString DigiDollarSendWidget::maskValue(const QString& value) const
{
    QString masked = value;
    for (int i = 0; i < masked.size(); ++i) {
        if (masked[i].isDigit()) {
            masked[i] = '#';
        }
    }
    return masked;
}

// AmountValidator implementation
AmountValidator::AmountValidator(double min, double max, int maxDecimals, QObject* parent) :
    QValidator(parent), m_min(min), m_max(max), m_maxDecimals(maxDecimals)
{
}

QValidator::State AmountValidator::validate(QString& input, int& pos) const
{
    Q_UNUSED(pos)

    if (input.isEmpty()) {
        return QValidator::Intermediate;
    }

    // Check for negative numbers
    if (input.startsWith("-")) {
        return QValidator::Invalid;
    }

    // Check for valid number format
    QRegularExpression numRegex("^\\d*\\.?\\d*$");
    if (!numRegex.match(input).hasMatch()) {
        return QValidator::Invalid;
    }

    // Check decimal places (max m_maxDecimals, default 8)
    int decimalPos = input.indexOf('.');
    if (decimalPos != -1) {
        if (input.length() - decimalPos - 1 > m_maxDecimals) {
            return QValidator::Invalid;
        }
    }

    // Convert to double and check range
    bool ok;
    double value = input.toDouble(&ok);
    if (!ok) {
        return QValidator::Intermediate;
    }

    // An amount outside the allowed range is accepted into the box but never
    // acceptable. Refusing the keystroke instead would silently drop it: typing
    // 200000 would leave 20000 in the box, a tenth of what the user meant, with
    // nothing on screen to say why. The form keeps the Send button disabled and
    // the line under the box says which limit was passed.
    if (value > m_max) {
        return QValidator::Intermediate;
    }

    if (value < m_min) {
        // User may still be typing (e.g. "1" on the way to "100")
        // Return Intermediate so Qt allows the keystroke but the
        // submit button stays disabled until the value is in range.
        return QValidator::Intermediate;
    }

    return QValidator::Acceptable;
}
