// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARSENDWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLARSENDWIDGET_H

#include <QWidget>
#include <QValidator>
#include <QMessageBox>
#include <QTimer>

#include <qt/paymasterconfirmation.h>
#include <qt/walletmodel.h>
#include <primitives/transaction.h>

#include <functional>
#include <cstdint>
#include <string>
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
class QComboBox;
class QCheckBox;
class QRadioButton;
class QSpinBox;
class QTableWidget;
class QResizeEvent;
QT_END_NAMESPACE

// 3-second confirmation delay constant (matches DGB send behavior)
#define DD_SEND_CONFIRM_DELAY 3

/**
 * DigiDollar send widget for sending DD to other addresses.
 * This widget provides functionality to send DigiDollar with proper
 * address validation and fee calculation. Paymaster sends use a two-stage
 * workflow: preparation may discover an offer, but only a second prompt for
 * the exact Core-provided commitment can authorize wallet signing.
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
    /** Test hook for exercising the explicit wallet-empty workflow without a live wallet backend. */
    void setAvailableDigiDollarBalanceForTesting(CAmount balance_cents);
    /** Test hook for verifying the widget forwards selected DD inputs to WalletModel without opening modal UI. */
    WalletModel::DigiDollarSendResult sendDigiDollarForTesting(const QString& address, CAmount amount, const QString& comment = "");
    /** Test hook for verifying success copy without opening a modal dialog. */
    QString successMessageForTesting(const QString& txid, double amount) const;
    /** Test hook for exercising the non-mutating Paymaster focus-state presentation. */
    void setPaymasterSessionForTesting(const QString& state, const QString& artifact,
                                       bool persisted, const QString& address, double amount,
                                       const QString& attempt_state = QString{},
                                       const QString& pending_phase = QString{},
                                       const QStringList& allowed_actions = {},
                                       bool allowed_actions_known = false);
    /**
     * Install a deterministic wallet RPC transport for Paymaster widget tests.
     *
     * Production leaves this unset and continues through WalletModel's
     * asynchronous worker. The synchronous test transport makes each client
     * state transition observable without a live Paymaster network.
     */
    using PaymasterRpcExecutorForTesting =
        std::function<UniValue(const std::string&, const UniValue&)>;
    void setPaymasterRpcExecutorForTesting(PaymasterRpcExecutorForTesting executor);
    /** Delayed transport hook for lifecycle and callback-ordering tests. */
    using PaymasterAsyncRpcExecutorForTesting = std::function<void(
        const std::string&, const UniValue&, WalletModel::RpcCallback)>;
    void setPaymasterAsyncRpcExecutorForTesting(
        PaymasterAsyncRpcExecutorForTesting executor);
    using DialogHandlerForTesting = std::function<QMessageBox::StandardButton(
        QMessageBox::Icon, const QString&, const QString&,
        QMessageBox::StandardButtons, QMessageBox::StandardButton)>;
    void setDialogHandlerForTesting(DialogHandlerForTesting handler);

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

protected:
    void resizeEvent(QResizeEvent* event) override;

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
    void onFeeModeChanged();
    void showPaymasterExplanation();
    void configureClientSafetyPolicy();
    void refreshClientSafetyStatus();
    void refreshPaymasterOffers();
    void loadSelectedPersistedPaymasterSession();
    void refreshPaymasterSessionState();
    void pollPaymasterSession();
    void retryPaymasterSession();
    void fallbackPaymasterSession();
    void recoverPaymasterSessionToSelf();
    void abandonUnsignedPaymasterSession();
    void cancelPaymasterQuote();
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
    void updateClientSafetyDisplay();
    void updateFeeChoiceLayout();
    void updatePaymasterFocusMode();
    void applyPaymasterPrivacy();
    void invalidatePaymasterOfferPreview();
    void discoverPersistedPaymasterSessions();
    void activatePersistedPaymasterSession(const QString& request_id);
    void refreshPaymasterSessionForAction(
        const QString& required_action,
        std::function<void()> continuation);
    void schedulePaymasterPoll(bool state_changed);
    void stopPaymasterPolling();
    QString friendlyPaymasterSessionStatus() const;
    void onPaymasterPrimaryAction();
    QString feeMode() const;
    QString formatCents(qint64 cents) const;
    QString friendlyFundingModel(const QString& model) const;
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
    QMessageBox::StandardButton showDialog(
        QMessageBox::Icon icon, const QString& title, const QString& message,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton default_button = QMessageBox::Ok);
    bool checkWalletState();
    bool showConfirmationDialog(const QString& address, double amount);
    QString buildSuccessMessage(const QString& txid, double amount) const;
    void executeTransfer(const QString& address, double amount);
    void executePaymasterRpcAsync(std::string command, UniValue params,
                                  WalletModel::RpcCallback callback);
    void executePaymasterTransfer(const QString& address, CAmount amount_cents,
                                  bool allow_unlock);
    UniValue buildPaymasterSendParams(const QString& address, CAmount amount_cents) const;
    void handlePaymasterResult(const UniValue& result, const QString& error,
                               const QString& address, CAmount amount_cents);
    bool updatePaymasterSessionView(const UniValue& result,
                                    QString* decode_error = nullptr);
    bool handleAuthoritativePaymasterCompletion(const UniValue& result);
    PaymasterConfirmationSelection paymasterConfirmationSelection(
        const UniValue& result, const QString& address) const;
    bool confirmPaymasterSelectionBeforeSigning(
        const UniValue& result, const QString& address);
    void executeAlternativePaymasterRecovery(bool allow_unlock);
    UniValue buildAlternativePaymasterRecoveryParams() const;
    void handleAlternativePaymasterRecoveryResult(
        const UniValue& result, const QString& error);
    PaymasterRecoveryConfirmationSelection paymasterRecoveryConfirmationSelection(
        const UniValue& recovery) const;
    bool confirmPaymasterRecoveryBeforeSigning(
        const PaymasterRecoveryConfirmationSelection& selection);
    void blockAlternativePaymasterRecovery(const QString& reason,
                                           const QString& detail);
    bool paymasterModeSelected() const;
    void setPaymasterBusy(bool busy);
    void reportPaymasterOperationNotStarted(const QString& reason);
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
    QLabel* m_feeHeading;
    QFrame* m_feeChoicesFrame;
    QGridLayout* m_feeChoicesLayout;
    QFrame* m_dgbFeeCard;
    QFrame* m_autoFeeCard;
    QFrame* m_paymasterFeeCard;
    QLabel* m_feeLabel;
    QLabel* m_feeValue;
    QLabel* m_totalLabel;
    QLabel* m_totalValue;
    QLabel* m_feeIntroduction;
    QRadioButton* m_dgbFeeRadio;
    QRadioButton* m_autoFeeRadio;
    QRadioButton* m_paymasterFeeRadio;
    QLabel* m_feeModeExplanation;
    QLabel* m_feeSummary;
    QCheckBox* m_subtractPaymasterFeeCheck;
    QPushButton* m_paymasterExplanationButton;
    QPushButton* m_advancedPaymasterButton;
    QComboBox* m_feeModeCombo;
    QFrame* m_advancedPaymasterFrame;
    QComboBox* m_privacyCombo;
    QComboBox* m_selectionCombo;
    QSpinBox* m_feeCapSpin;
    QSpinBox* m_maxAttemptsSpin;
    QPushButton* m_refreshOffersButton;
    QLabel* m_offersStatus;
    QTableWidget* m_offersTable;
    QFrame* m_persistedPaymasterSessionsFrame;
    QComboBox* m_persistedPaymasterSessions;
    QPushButton* m_loadPersistedPaymasterSessionButton;
    QFrame* m_clientSafetyFrame;
    QLabel* m_clientSafetyStatus;
    QLabel* m_clientSafetyDetails;
    QPushButton* m_configureClientSafetyButton;
    QFrame* m_paymasterSessionFrame;
    QLabel* m_paymasterStateValue;
    QLabel* m_paymasterTransferValue;
    QLabel* m_paymasterIdentityValue;
    QLabel* m_paymasterCostValue;
    QLabel* m_paymasterExpiryValue;
    QPushButton* m_retrySessionButton;
    QPushButton* m_fallbackSessionButton;
    QPushButton* m_recoverSessionButton;
    QPushButton* m_abandonSessionButton;
    QPushButton* m_cancelQuoteButton;
    QLabel* m_paymasterNextStepValue;
    QPushButton* m_paymasterPrimaryButton;
    QPushButton* m_paymasterMoreButton;
    QPushButton* m_paymasterTechnicalButton;
    QFrame* m_paymasterSecondaryActions;
    QFrame* m_paymasterTechnicalDetails;

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
    double m_paymasterInitialAvailableBalance{0.0};
    double m_oraclePrice;
    double m_estimatedFee;
    QString m_paymasterRequestId;
    QString m_paymasterSessionId;
    QString m_paymasterSessionState;
    QString m_paymasterAttemptState;
    QString m_paymasterArtifact;
    QString m_paymasterPendingPhase;
    QString m_paymasterBroadcastState;
    QString m_paymasterConfirmationState;
    QString m_paymasterTransactionId;
    QString m_paymasterRecoveryTransactionId;
    QString m_paymasterResultStatus;
    qint64 m_paymasterResultSequence{-1};
    qint64 m_paymasterRecoveryExpiresAt{-1};
    QStringList m_paymasterAllowedActions;
    QString m_paymasterAddress;
    QString m_paymasterAuthorizationCommitment;
    QString m_paymasterSessionPrivacy;
    QString m_paymasterRecoveryAuthorizationCommitment;
    double m_paymasterAmount{0.0};
    CAmount m_paymasterAmountCents{0};
    qint64 m_paymasterPreviewRecipientCents{-1};
    qint64 m_paymasterPreviewServiceFeeCents{-1};
    qint64 m_paymasterPreviewTotalCents{-1};
    uint64_t m_paymasterOfferPreviewGeneration{0};
    uint64_t m_paymasterWalletGeneration{0};
    uint64_t m_clientSafetyRefreshGeneration{0};
    uint64_t m_oraclePriceRequestGeneration{0};
    bool m_sendAllSpendableDD{false};
    bool m_settingSweepAmount{false};
    qint64 m_paymasterRecoveryMaximumServiceFeeCents{0};
    bool m_paymasterBusy{false};
    bool m_paymasterSessionPersisted{false};
    bool m_paymasterRecoveryActive{false};
    enum class PaymasterPrimaryAction {
        REFRESH,
        RESUME,
        REVIEW_OFFER,
        FALLBACK,
        RECOVER,
        NEW_TRANSFER,
    };
    PaymasterPrimaryAction m_paymasterPrimaryAction{PaymasterPrimaryAction::REFRESH};
    bool m_clientSafetyStatusKnown{false};
    bool m_clientSafetyConfigured{false};
    qint64 m_clientSafetyMaximumPerTransaction{100};
    qint64 m_clientSafetyMaximumPerDay{1000};
    qint64 m_clientSafetyActiveReservations{0};
    qint64 m_clientSafetyReservedCents{0};
    qint64 m_clientSafetySpentTodayCents{0};
    qint64 m_clientSafetyAvailableTodayCents{0};
    QString m_clientSafetyError;
    QTimer* m_paymasterPollTimer;
    int m_paymasterPollIntervalMs{1500};
    static constexpr int MAX_PAYMASTER_POLL_INTERVAL_MS{15000};
    bool m_paymasterAllowedActionsKnown{false};
    bool m_paymasterTerminalNoticeShown{false};
    PaymasterRpcExecutorForTesting m_paymasterRpcExecutorForTesting;
    PaymasterAsyncRpcExecutorForTesting m_paymasterAsyncRpcExecutorForTesting;
    DialogHandlerForTesting m_dialogHandlerForTesting;
    PaymasterConfirmationGuard m_paymasterConfirmationGuard;
    PaymasterRecoveryConfirmationGuard m_paymasterRecoveryConfirmationGuard;

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
                             const QString& confirm_button_text = "",
                             QWidget* parent = nullptr);

    /* Returns QMessageBox::Yes when the contextual confirmation action is
     * clicked, QMessageBox::Cancel otherwise. */
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
