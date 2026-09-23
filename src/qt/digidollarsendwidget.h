// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARSENDWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLARSENDWIDGET_H

#include <QWidget>
#include <QValidator>
#include <QMessageBox>
#include <QTimer>

#include <qt/walletmodel.h>
#include <primitives/transaction.h>

#include <vector>

class ClientModel;
class DigiDollarAddressValidator;
class AmountValidator;
class DDAddressBookPage;
class PlatformStyle;

namespace wallet {
class DDCoinControl;
} // namespace wallet

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QGridLayout;
class QFrame;
class QToolButton;
class QScrollArea;
class QSpacerItem;
class QAbstractButton;
QT_END_NAMESPACE

// 3-second confirmation delay constant (matches DGB send behavior)
#define DD_SEND_CONFIRM_DELAY 3

/**
 * DigiDollar send widget for sending DD to other addresses.
 * This widget provides functionality to send DigiDollar with proper
 * address validation and fee calculation.
 */
class DigiDollarSendWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DigiDollarSendWidget(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~DigiDollarSendWidget();

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);
    void updateView();

    /** Test hook for exercising coin-control summary/preflight state without opening the modal dialog. */
    void setSelectedDigiDollarInputsForTesting(const std::vector<COutPoint>& inputs);
    /** Test hook for verifying the widget forwards selected DD inputs to WalletModel without opening modal UI. */
    WalletModel::DigiDollarSendResult sendDigiDollarForTesting(const QString& address, CAmount amount, const QString& comment = "");
    /** Test hook for verifying success copy without opening a modal dialog. */
    QString successMessageForTesting(const QString& txid, double amount) const;

Q_SIGNALS:
    /** Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);

public Q_SLOTS:
    /** Update balance display */
    void updateBalance();
    /** Update oracle price for USD equivalent calculation */
    void updateOraclePrice();
    /** Set privacy mode — masks balance displays */
    void setPrivacy(bool privacy);

private Q_SLOTS:
    /** Address field changed */
    void onAddressChanged();
    /** Amount field changed */
    void onAmountChanged();
    /** Send button clicked */
    void onSendClicked();
    /** Clear all fields */
    void onClearClicked();
    /** Use available balance */
    void onUseAvailableBalanceClicked();
    void onPasteAddressClicked();
    void onAddressBookClicked();
    void onCoinControlButtonClicked();
    /** Update coin control labels */
    void updateCoinControlLabels();

private:
    void setupUI();
    void setupCoinControlSection();
    void setupAddressSection();
    void setupAmountSection();
    void setupNoteSection();
    void setupFeeSection();
    void setupButtonSection();
    void setupStyleSheets();
    void connectSignals();
    void applyTheme();
    void updateAddressValidation();
    void updateAmountValidation();
    void updateSendButton();
    void updateUSDEquivalent();
    void updateFeeDisplay();
    CAmount selectedDigiDollarAmount() const;

    bool validateAddress() const;
    bool validateAmount() const;
    bool validateBalance() const;
    /**
     * What is wrong with the amount in the box, in words the user can act on.
     * An empty string means the amount can be sent.
     */
    QString amountProblem() const;

    QString formatDDAmount(double amount) const;
    QString formatUSDAmount(double amount) const;
    /** Mask a formatted string by replacing digits with '#' */
    QString maskValue(const QString& value) const;

    // Phase 7.2-7.3: Helper methods for improved UX
    void showError(const QString& title, const QString& message);
    void showWarning(const QString& title, const QString& message);
    bool checkWalletState();
    bool showConfirmationDialog(const QString& address, double amount);
    QString buildSuccessMessage(const QString& txid, double amount) const;
    void executeTransfer(const QString& address, double amount);
    void showSuccess(const QString& txid, double amount);
    void showBackendError(int status, const QString& reasonFailed);

    // UI components
    QVBoxLayout* m_mainLayout;

    // Address section
    QFrame* m_addressFrame;
    QGridLayout* m_addressLayout;
    QLabel* m_addressLabel;
    QLineEdit* m_addressEdit;
    QToolButton* m_pasteAddressButton;
    QToolButton* m_addressBookButton;
    QLabel* m_addressValidationLabel;

    // Amount section
    QFrame* m_amountFrame;
    QGridLayout* m_amountLayout;
    QLabel* m_amountLabel;
    QLineEdit* m_amountEdit;
    QLabel* m_amountSuffix;
    QPushButton* m_useAvailableBalanceButton;
    QLabel* m_amountValidationLabel;
    QLabel* m_usdEquivalentLabel;
    QLabel* m_usdEquivalentValue;
    QLabel* m_availableBalanceLabel;
    QLabel* m_availableBalanceValue;

    // Note section
    QFrame* m_noteFrame;
    QGridLayout* m_noteLayout;
    QLabel* m_noteLabel;
    QLineEdit* m_noteEdit;

    // Fee section
    QFrame* m_feeFrame;
    QGridLayout* m_feeLayout;
    QLabel* m_feeLabel;
    QLabel* m_feeValue;
    QLabel* m_totalLabel;
    QLabel* m_totalValue;

    // Button section
    QFrame* m_buttonFrame;
    QHBoxLayout* m_buttonLayout;
    QPushButton* m_sendButton;
    QPushButton* m_clearButton;

    // Coin control section
    QFrame* m_coinControlFrame;
    QHBoxLayout* m_coinControlLayout;
    QPushButton* m_coinControlButton;
    QLabel* m_coinControlQuantityLabel;
    QLabel* m_coinControlAmountLabel;

    // Validators
    DigiDollarAddressValidator* m_addressValidator;
    AmountValidator* m_amountValidator;

    // Models
    WalletModel* m_walletModel;
    ClientModel* m_clientModel;
    const PlatformStyle* m_platformStyle;

    // Data
    double m_availableBalance;
    double m_oraclePrice;
    double m_estimatedFee;

    // Privacy
    bool m_privacy{false};

    // Coin control
    std::unique_ptr<wallet::DDCoinControl> m_coinControl;
};

/**
 * Validator for DigiDollar addresses on the active network.
 * Uses CDigiDollarAddress::IsValidDigiDollarAddressForCurrentNetwork().
 */
class DigiDollarAddressValidator : public QValidator
{
    Q_OBJECT

public:
    explicit DigiDollarAddressValidator(QObject* parent = nullptr);

    QValidator::State validate(QString& input, int& pos) const override;

private:
    bool isValidDDAddress(const QString& address) const;
};

/**
 * Validator for DigiDollar amounts
 */
class AmountValidator : public QValidator
{
    Q_OBJECT

public:
    explicit AmountValidator(double min = 0.00000001, double max = 999999999.99999999, int maxDecimals = 8, QObject* parent = nullptr);

    QValidator::State validate(QString& input, int& pos) const override;

private:
    double m_min;
    double m_max;
    int m_maxDecimals;
};

/**
 * Confirmation dialog with 3-second countdown timer for DigiDollar sends.
 * This matches the DGB send confirmation behavior exactly, requiring users to wait
 * 3 seconds before the Send button becomes enabled, allowing them to review
 * the transaction details before confirming.
 */
class DDSendConfirmationDialog : public QMessageBox
{
    Q_OBJECT

public:
    DDSendConfirmationDialog(const QString& title, const QString& text,
                             const QString& informative_text = "",
                             int secDelay = DD_SEND_CONFIRM_DELAY,
                             QWidget* parent = nullptr);

    /* Returns QMessageBox::Yes when "Send" is clicked, QMessageBox::Cancel otherwise */
    int exec() override;

private Q_SLOTS:
    void countDown();
    void updateButtons();

private:
    QAbstractButton* yesButton;
    QTimer countDownTimer;
    int secDelay;
    QString confirmButtonText{tr("Send")};  // Matches DGB default
};

#endif // DIGIBYTE_QT_DIGIDOLLARSENDWIDGET_H
