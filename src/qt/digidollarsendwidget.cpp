// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarsendwidget.h>
#include <qt/paymastersendwidget.h>
#include <qt/ddaddressbookpage.h>
#include <qt/digidollarstatus.h>

#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
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
#include <util/strencodings.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

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
#include <QPalette>
#include <QProgressDialog>
#include <QPointer>
#include <QDialog>

using namespace std::chrono_literals;

namespace {
bool ParseDigiDollarCents(const QString& text, CAmount& cents)
{
    static const QRegularExpression DECIMAL_PATTERN{
        QStringLiteral("^(0|[1-9][0-9]*)(?:\\.([0-9]{1,2}))?$")};
    const QRegularExpressionMatch match =
        DECIMAL_PATTERN.match(text.trimmed());
    if (!match.hasMatch()) return false;

    bool whole_ok{false};
    const qint64 whole = match.captured(1).toLongLong(&whole_ok);
    if (!whole_ok || whole >
            DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS / 100) {
        return false;
    }
    QString fraction = match.captured(2);
    if (fraction.size() == 1) fraction.append(QLatin1Char('0'));
    const qint64 fractional = fraction.isEmpty() ? 0 : fraction.toInt();
    if (whole > (std::numeric_limits<qint64>::max() - fractional) / 100) {
        return false;
    }
    cents = whole * 100 + fractional;
    return cents >= 100 &&
        cents <= DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS;
}
} // namespace

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
    m_paymaster = new PaymasterSendWidget(*this);
    m_mainLayout->addWidget(m_paymaster);
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
    DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::INFO);
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

    // Create horizontal layout for amount input and buttons
    QHBoxLayout* amountInputLayout = new QHBoxLayout();
    amountInputLayout->setSpacing(8);

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

    amountInputLayout->addWidget(m_amountEdit, 0);
    amountInputLayout->addWidget(m_useAvailableBalanceButton, 1);

    m_amountLayout->addWidget(m_amountLabel, 0, 0);
    m_amountLayout->addLayout(amountInputLayout, 0, 1);

    // The line that says what is wrong with the amount, under the box it is
    // about. Without it a refused amount showed only a red border and the user
    // had no way to tell whether it was too small, too large, or more than the
    // balance.
    m_amountValidationLabel = new QLabel(this);
    m_amountValidationLabel->setObjectName("amountValidationLabel");
    m_amountValidationLabel->setWordWrap(true);
    // The amount box is drawn taller than the row it is given, because its
    // height comes from the stylesheet. Without a gap here the bottom edge of
    // the box runs through this line of text.
    m_amountValidationLabel->setContentsMargins(0, 14, 0, 0);
    m_amountLayout->addWidget(m_amountValidationLabel, 1, 1);

    // USD equivalent display
    m_usdEquivalentLabel = new QLabel(tr("$USD Equivalent:"), this);
    m_usdEquivalentLabel->setObjectName("usdEquivalentLabel");
    m_usdEquivalentLabel->setToolTip(tr("Equivalent value in US Dollars (DigiDollar is pegged to $1 USD)"));
    m_usdEquivalentValue = new QLabel("0.00 $USD", this);
    m_usdEquivalentValue->setObjectName("usdEquivalentValue");
    m_usdEquivalentValue->setFont(monospaceFont);
    m_usdEquivalentValue->setToolTip(tr("USD value updates in real-time as you type"));

    m_amountLayout->addWidget(m_usdEquivalentLabel, 2, 0);
    m_amountLayout->addWidget(m_usdEquivalentValue, 2, 1);

    // Available balance display
    m_availableBalanceLabel = new QLabel(tr("Available:"), this);
    m_availableBalanceLabel->setObjectName("availableBalanceLabel");
    m_availableBalanceLabel->setToolTip(tr("Your current available DigiDollar balance"));
    m_availableBalanceValue = new QLabel("0.00 $DD", this);
    m_availableBalanceValue->setObjectName("availableBalanceValue");
    m_availableBalanceValue->setFont(monospaceFont);
    m_availableBalanceValue->setToolTip(tr("Your current spendable DigiDollar balance"));

    m_amountLayout->addWidget(m_availableBalanceLabel, 3, 0);
    m_amountLayout->addWidget(m_availableBalanceValue, 3, 1);

    // Set column widths to prevent layout distortion on initial display
    m_amountLayout->setColumnMinimumWidth(0, 110);  // Label column
    m_amountLayout->setColumnStretch(0, 0);
    m_amountLayout->setColumnStretch(1, 1);

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
    m_paymaster->updateFeeDisplay();
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

    // The amount field is the recipient amount. Direct funding uses DGB;
    // an exact Paymaster service fee is validated separately before signing.
    m_useAvailableBalanceButton->setEnabled(m_availableBalance > 0);

    // Re-validate amount against updated balance so the border color
    // refreshes when pending DD confirms (fixes stale yellow warning).
    updateAmountValidation();
    updateSendButton();
}

void DigiDollarSendWidget::updateOraclePrice()
{
    if (!isVisible()) return;
    const uint64_t request_generation = ++m_oraclePriceRequestGeneration;
    // Get oracle price from RPC for testnet/mainnet, MockOracleManager for regtest
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        // BUG #6 FIX: GetCurrentPrice() returns micro-USD, not cents
        CAmount priceMicroUsd = MockOracleManager::GetInstance().GetCurrentPrice();
        m_oraclePrice = priceMicroUsd / 1000000.0;
    } else if (m_walletModel) {
        UniValue params{UniValue::VARR};
        QPointer<DigiDollarSendWidget> guard{this};
        m_walletModel->executeRpcAsync(
            "getoracleprice", std::move(params),
            [guard, request_generation](UniValue result, QString error) {
                if (!guard || guard->m_oraclePriceRequestGeneration !=
                                  request_generation) {
                    return;
                }
                qint64 price_micro_usd{0};
                const UniValue& price = result.find_value("price_micro_usd");
                if (error.isEmpty() && price.isNum()) {
                    try {
                        price_micro_usd = price.getInt<qint64>();
                    } catch (const std::exception&) {
                        price_micro_usd = 0;
                    }
                }
                guard->m_oraclePrice = price_micro_usd > 0
                    ? price_micro_usd / 1000000.0 : 0.0;
                guard->updateUSDEquivalent();
            });
        return;
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
    m_paymaster->amountChanged(m_settingSweepAmount);

    // Validate amount format
    updateAmountValidation();

    updateUSDEquivalent();
    m_paymaster->updateFeeDisplay();
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

    CAmount amount_cents{0};
    if (!ParseDigiDollarCents(amountText, amount_cents)) {
        // validateAmount() uses the same parser, so reaching this branch means
        // the text changed during a nested UI event. Fail closed instead of
        // converting a different floating-point value.
        showError(tr("Invalid Amount"),
                  tr("The DigiDollar amount changed while it was being validated. Please review it and try again."));
        return;
    }
    const double amount = amount_cents / 100.0;

    // Error: Insufficient balance
    if (!validateBalance()) {
        const bool subtract_fee = m_paymaster->paymasterModeSelected() &&
            m_paymaster->subtractFee();
        const QString fee_note = m_paymaster->paymasterModeSelected()
            ? (subtract_fee
                   ? tr("The entered amount is the exact maximum $DD outflow; the provider fee is deducted within it.")
                   : tr("A user-paid Paymaster offer may additionally require a $DD service fee. "
                        "The exact fee is checked before signing."))
            : tr("The network fee is paid separately from your DGB balance.");
        showError(tr("Insufficient DigiDollar Balance"),
                  tr("You don't have enough DigiDollar for this transfer.\n\n"
                     "Available balance: %1\n"
                     "Amount to send: %2\n\n"
                     "%3\n\n"
                     "Please enter a smaller amount or add more $DD to your wallet.")
                  .arg(formatDDAmount(m_availableBalance))
                  .arg(formatDDAmount(amount))
                  .arg(fee_note));
        m_amountEdit->setFocus();
        return;
    }

    if (m_coinControl && m_coinControl->HasSelected()) {
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

    if (m_paymaster->paymasterModeSelected()) {
        m_paymaster->send(address, amount_cents);
    } else {
        // PHASE 7.2: Enhanced confirmation dialog with fee display. The direct
        // path has only one signing/broadcast confirmation.
        if (!m_paymaster->showConfirmationDialog(address, amount)) {
            return;
        }
        // The legacy direct path stays synchronous and keeps its unlock only
        // for local transaction creation/signing.
        WalletModel::UnlockContext ctx(m_walletModel->requestUnlock());
        if (!ctx.isValid()) return;
        executeTransfer(address, amount);
    }
}

void DigiDollarSendWidget::onClearClicked()
{
    clearInputFields();
    m_paymaster->resetEntry();
    onAddressChanged();
    onAmountChanged();
}


void DigiDollarSendWidget::onUseAvailableBalanceClicked()
{
    if (m_availableBalance <= 0) return;
    if (m_paymaster->paymasterModeSelected() && m_coinControl && m_coinControl->HasSelected()) {
        showWarning(
            tr("Wallet emptying needs automatic input selection"),
            tr("Clear the manually selected DigiDollar inputs before emptying the wallet. "
               "Core must bind every confirmed, ordinary spendable input to one exact snapshot."));
        return;
    }
    m_paymaster->beginSweep();
    m_settingSweepAmount = true;
    m_amountEdit->setText(QString::number(m_availableBalance, 'f', 2));
    m_settingSweepAmount = false;
    m_paymaster->updateFeeDisplay();
    updateSendButton();
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
    // setupFeeSection() selects the safe default before setupButtonSection()
    // creates the action buttons. Fee-mode updates during that construction
    // phase must not dereference the not-yet-created send button.
    if (!m_sendButton) return;

    bool addressValid = validateAddress();
    bool amountValid = validateAmount();
    bool balanceValid = validateBalance();
    const bool paymaster_ready = !m_paymaster->paymasterModeSelected() ||
        (m_paymaster->isReady());

    m_sendButton->setEnabled(!m_paymaster->isBusy() && addressValid && amountValid &&
                             balanceValid && paymaster_ready);
    if (m_paymaster->isBusy()) {
        m_sendButton->setToolTip(tr("A Paymaster request is currently being processed"));
    } else if (!addressValid) {
        m_sendButton->setToolTip(tr("Enter a valid DigiDollar recipient address"));
    } else if (!amountValid) {
        m_sendButton->setToolTip(tr("Enter a valid DigiDollar amount greater than zero"));
    } else if (!balanceValid) {
        m_sendButton->setToolTip(tr(
            "Insufficient spendable DigiDollar: this wallet currently has %1 available. A Paymaster supplies only the DGB network fee, not the DigiDollar being sent.")
            .arg(formatDDAmount(m_availableBalance)));
    } else if (m_paymaster->paymasterModeSelected() && !paymaster_ready) {
        m_sendButton->setToolTip(m_paymaster->safetyStatusKnown()
            ? tr("Set positive wallet-local Paymaster service-fee limits before sending")
            : tr("Waiting for the wallet's Paymaster service-fee limits"));
    } else {
        m_sendButton->setToolTip(tr("Confirm and send this DigiDollar transaction"));
    }
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


bool DigiDollarSendWidget::validateAddress() const
{
    QString address = m_addressEdit->text();
    int pos = 0;
    QString addressCopy = address;
    return m_addressValidator->validate(addressCopy, pos) == QValidator::Acceptable;
}

bool DigiDollarSendWidget::validateAmount() const
{
    CAmount amount_cents{0};
    return ParseDigiDollarCents(m_amountEdit->text(), amount_cents);
}

bool DigiDollarSendWidget::validateBalance() const
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return true; // Empty is valid for enabling/disabling

    CAmount amount_cents{0};
    if (!ParseDigiDollarCents(amountText, amount_cents)) return false;
    // The exact Paymaster fee is not known until a quote is authenticated.
    // Validate the recipient amount here and the fee again before signing.
    const bool valid = amount_cents <=
        static_cast<CAmount>(std::llround(m_availableBalance * 100));

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

QMessageBox::StandardButton DigiDollarSendWidget::showDialog(
    QMessageBox::Icon icon, const QString& title, const QString& message,
    QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton default_button)
{
    if (m_dialogHandlerForTesting) {
        return m_dialogHandlerForTesting(
            icon, title, message, buttons, default_button);
    }

    QMessageBox msgBox(this);
    msgBox.setIcon(icon);
    msgBox.setWindowTitle(title);
    // Backend errors are presentation data, never trusted rich text.
    msgBox.setTextFormat(Qt::PlainText);
    msgBox.setText(message);
    msgBox.setStandardButtons(buttons);
    msgBox.setDefaultButton(default_button);
    return static_cast<QMessageBox::StandardButton>(msgBox.exec());
}

// PHASE 7.3: Error display helper
void DigiDollarSendWidget::showError(const QString& title, const QString& message)
{
    showDialog(QMessageBox::Critical, title, message);

    // Dialog text can contain wallet-local payment details or backend data.
    LogPrintf("DigiDollar GUI error displayed\n");
}

// PHASE 7.3: Warning display helper
void DigiDollarSendWidget::showWarning(const QString& title, const QString& message)
{
    showDialog(QMessageBox::Warning, title, message);

    // Dialog text can contain wallet-local payment details or backend data.
    LogPrintf("DigiDollar GUI warning displayed\n");
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
    if (m_privacy) {
        return tr("DigiDollar transfer broadcast. Transfer details are hidden while privacy mode is enabled.\n\nThe transaction is pending until miners include it in a block and the block confirms.");
    }
    return tr("DigiDollar transfer broadcast\n\nAmount sent: %1\nTransaction ID: %2\n\nThe transaction has been broadcast to the network. It is pending until miners include it in a block and the block confirms.")
        .arg(formatDDAmount(amount), txid);
}

void DigiDollarSendWidget::showSuccess(const QString& txid, double amount)
{
    showDialog(QMessageBox::Information, tr("Transfer Broadcast"),
               buildSuccessMessage(txid, amount));

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Transfer broadcast\n");
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
        if (PaymasterSendWidget::DescribeBackendError(reasonFailed, errorTitle, errorMessage)) break;
        if (reasonFailed.contains("Insufficient DGB", Qt::CaseInsensitive)) {
            errorTitle = tr("Not enough DGB for the network fee");
            errorMessage = tr(
                "This wallet cannot currently fund the DigiByte network fee with suitable "
                "confirmed DGB inputs. Add DGB or deliberately select a Paymaster option.\n\n"
                "Technical details: %1").arg(reasonFailed);
        } else if (reasonFailed.contains("locked", Qt::CaseInsensitive)) {
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
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
    QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";

    if (address.isEmpty()) {
        m_addressValidationLabel->setText(tr("Enter a valid DigiDollar address for this network"));
        DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::INFO);
        m_addressEdit->setStyleSheet("");
    } else if (validateAddress()) {
        m_addressValidationLabel->setText(tr("✓ Valid DigiDollar address"));
        DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::SUCCESS);
        m_addressEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
    } else {
        m_addressValidationLabel->setText(tr("✗ Invalid DigiDollar address for this network"));
        DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::ERR);
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
    QPalette palette = QApplication::palette();
    QString midColor = palette.color(QPalette::Mid).name();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
    QString warningColor = isDarkTheme ? "#ff9800" : "#ffc107";
    QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";

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
// Uses the DGB send confirmation countdown while allowing a context-specific
// action label for prepare-only Paymaster requests.
DDSendConfirmationDialog::DDSendConfirmationDialog(const QString& title, const QString& text,
                                                     const QString& informative_text,
                                                     int secDelay,
                                                     const QString& confirm_button_text,
                                                     QWidget* parent)
    : QMessageBox(parent), secDelay(secDelay)
{
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setTextFormat(Qt::RichText);
    setText(text);
    setInformativeText(informative_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    confirmButtonText = confirm_button_text.isEmpty()
        ? yesButton->text() : confirm_button_text;
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
        m_paymaster->updateFeeDisplay();
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
        m_paymaster->updateFeeDisplay();
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

    m_paymaster->updateFeeDisplay();
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

void DigiDollarSendWidget::setAvailableDigiDollarBalanceForTesting(CAmount balance_cents)
{
    m_availableBalance = static_cast<double>(balance_cents) / 100.0;
    m_availableBalanceValue->setText(formatDDAmount(m_availableBalance));
    m_useAvailableBalanceButton->setEnabled(balance_cents > 0);
    updateAmountValidation();
    m_paymaster->updateFeeDisplay();
    updateSendButton();
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


void DigiDollarSendWidget::setDialogHandlerForTesting(
    DialogHandlerForTesting handler)
{
    m_dialogHandlerForTesting = std::move(handler);
}


void DigiDollarSendWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    updateBalance();
    updateUSDEquivalent();
    m_paymaster->setPrivacy(privacy);
    if (m_privacy) m_usdEquivalentValue->setText(maskValue(formatUSDAmount(0)));
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

void DigiDollarSendWidget::setWalletModel(WalletModel* model)
{
    if (m_walletModel != model) {
        if (m_walletModel) disconnect(m_walletModel, nullptr, this, nullptr);
        ++m_oraclePriceRequestGeneration;
    }
    m_walletModel = model;
    m_paymaster->setWalletModel(model);
}

DigiDollarSendWidget::PaymentInput DigiDollarSendWidget::paymentInput() const
{
    PaymentInput input;
    input.amount_text = m_amountEdit->text();
    input.comment = m_noteEdit ? m_noteEdit->text().trimmed() : QString{};
    input.amount_valid = ParseDigiDollarCents(input.amount_text, input.amount_cents);
    input.available_balance = m_availableBalance;
    if (m_coinControl && m_coinControl->HasSelected()) {
        input.selected_inputs = m_coinControl->ListSelected();
    }
    return input;
}

void DigiDollarSendWidget::setPaymasterFormFocus(bool focus)
{
    m_addressEdit->setReadOnly(focus);
    m_amountEdit->setReadOnly(focus);
    if (m_noteEdit) m_noteEdit->setReadOnly(focus);
    for (QLineEdit* field : {m_addressEdit, m_amountEdit, m_noteEdit}) {
        if (!field) continue;
        field->setProperty("paymasterSessionLocked", focus);
        field->style()->unpolish(field);
        field->style()->polish(field);
    }
    m_pasteAddressButton->setEnabled(!focus);
    m_addressBookButton->setEnabled(!focus);
    m_useAvailableBalanceButton->setEnabled(!focus);
    m_coinControlButton->setEnabled(!focus);
    if (m_clearButton) m_clearButton->setVisible(!focus);
    if (m_sendButton) m_sendButton->setVisible(!focus);
    if (m_buttonFrame) m_buttonFrame->setVisible(!focus);

}

void DigiDollarSendWidget::updatePaymasterFormMode(bool paymaster_enabled)
{
    if (m_useAvailableBalanceButton) {
        m_useAvailableBalanceButton->setText(
            paymaster_enabled ? tr("Empty wallet with Paymaster")
                              : tr("Use available balance"));
        m_useAvailableBalanceButton->setToolTip(paymaster_enabled
            ? tr("Use every confirmed, ordinary spendable $DD input. If a Paymaster is needed, "
                 "its exact fee is deducted so no spendable $DD remains.")
            : tr("Use the full available DigiDollar balance; the network fee is paid separately in DGB."));
    }
}

void DigiDollarSendWidget::setPaymasterFormPrivacy(bool privacy)
{
    for (QLineEdit* edit : {m_addressEdit, m_amountEdit, m_noteEdit}) {
        if (edit) {
            edit->setEchoMode(privacy ? QLineEdit::Password
                                        : QLineEdit::Normal);
        }
    }

}

void DigiDollarSendWidget::clearInputFields()
{
    m_addressEdit->clear();
    m_amountEdit->clear();
    if (m_noteEdit) m_noteEdit->clear();
}
