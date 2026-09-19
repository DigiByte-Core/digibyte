// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_PAYMASTERSENDWIDGET_H
#define DIGIBYTE_QT_PAYMASTERSENDWIDGET_H

#include <qt/paymasterconfirmation.h>
#include <qt/walletmodel.h>

#include <QWidget>
#include <QStringList>
#include <cstdint>
#include <functional>

class DigiDollarSendWidget;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QRadioButton;
class QResizeEvent;
class QSpinBox;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

/** Owns fee selection and the Paymaster client presentation state. Core remains
 * authoritative for authorization, reservations and durable payment sessions.
 * The parent owns editable payment fields; this component reads snapshots and
 * uses its presentation hooks without retaining a second editable form.
 */
class PaymasterSendWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit PaymasterSendWidget(DigiDollarSendWidget& form);
    void amountChanged(bool setting_sweep);
    void beginSweep();
    void setPrivacy(bool privacy);
    bool isBusy() const { return m_paymasterBusy; }
    bool isReady() const { return m_clientSafetyStatusKnown && m_clientSafetyConfigured; }
    bool safetyStatusKnown() const { return m_clientSafetyStatusKnown; }
    bool subtractFee() const;
    static bool DescribeBackendError(const QString& reasonFailed, QString& title, QString& message);
    bool paymasterModeSelected() const;
    void invalidatePaymasterOfferPreview();
    void updateFeeDisplay();
    bool showConfirmationDialog(const QString& address, double amount);
    void setWalletModel(WalletModel* model);
    void send(const QString& address, CAmount amount_cents);
    void resetEntry();
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

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateFeeChoiceLayout();
    void setupFeeSection();
    Q_SLOT void onFeeModeChanged();
    QString friendlyPaymasterSessionStatus() const;
    void updatePaymasterFocusMode();
    void onPaymasterPrimaryAction();
    Q_SLOT void showPaymasterExplanation();
    Q_SLOT void configureClientSafetyPolicy();
    Q_SLOT void refreshClientSafetyStatus();
    void updateClientSafetyDisplay();
    void reportPaymasterOperationNotStarted(
        const QString& reason);
    void discoverPersistedPaymasterSessions();
    Q_SLOT void loadSelectedPersistedPaymasterSession();
    void activatePersistedPaymasterSession(
        const QString& request_id);
    Q_SLOT void refreshPaymasterOffers();
    UniValue buildPaymasterSendParams(const QString& address, CAmount amount_cents) const;
    void executePaymasterRpcAsync(
        std::string command, UniValue params, WalletModel::RpcCallback callback);
    void executePaymasterTransfer(
        const QString& address, CAmount amount_cents, bool allow_unlock);
    void handlePaymasterResult(const UniValue& result, const QString& error,
                               const QString& address, CAmount amount_cents);
    bool updatePaymasterSessionView(
        const UniValue& result, QString* decode_error = nullptr);
    bool handleAuthoritativePaymasterCompletion(
        const UniValue& result);
    PaymasterConfirmationSelection paymasterConfirmationSelection(
        const UniValue& result, const QString& address) const;
    bool confirmPaymasterSelectionBeforeSigning(
        const UniValue& result, const QString& address);
    Q_SLOT void pollPaymasterSession();
    Q_SLOT void refreshPaymasterSessionState();
    void stopPaymasterPolling();
    void schedulePaymasterPoll(bool state_changed);
    void refreshPaymasterSessionForAction(
        const QString& required_action, std::function<void()> continuation);
    Q_SLOT void retryPaymasterSession();
    Q_SLOT void fallbackPaymasterSession();
    Q_SLOT void recoverPaymasterSessionToSelf();
    Q_SLOT void abandonUnsignedPaymasterSession();
    UniValue buildAlternativePaymasterRecoveryParams() const;
    void executeAlternativePaymasterRecovery(bool allow_unlock);
    PaymasterRecoveryConfirmationSelection paymasterRecoveryConfirmationSelection(
        const UniValue& recovery) const;
    bool confirmPaymasterRecoveryBeforeSigning(
        const PaymasterRecoveryConfirmationSelection& selection);
    void blockAlternativePaymasterRecovery(
        const QString& reason, const QString& detail);
    void handleAlternativePaymasterRecoveryResult(
        const UniValue& result, const QString& error);
    Q_SLOT void cancelPaymasterQuote();
    QString formatCents(qint64 cents) const;
    QString friendlyFundingModel(const QString& model) const;
    void applyPaymasterPrivacy();

private:
    QString feeMode() const;
    void connectSignals();
    void setPaymasterBusy(bool busy);
    DigiDollarSendWidget& m_form;
    WalletModel* m_walletModel{nullptr};
    bool m_privacy{false};
    // Fee section
    QFrame* m_feeFrame{nullptr};
    QGridLayout* m_feeLayout{nullptr};
    QLabel* m_feeHeading{nullptr};
    QFrame* m_feeChoicesFrame{nullptr};
    QGridLayout* m_feeChoicesLayout{nullptr};
    QFrame* m_dgbFeeCard{nullptr};
    QFrame* m_autoFeeCard{nullptr};
    QFrame* m_paymasterFeeCard{nullptr};
    QLabel* m_feeLabel{nullptr};
    QLabel* m_feeValue{nullptr};
    QLabel* m_totalLabel{nullptr};
    QLabel* m_totalValue{nullptr};
    QLabel* m_feeIntroduction{nullptr};
    QRadioButton* m_dgbFeeRadio{nullptr};
    QRadioButton* m_autoFeeRadio{nullptr};
    QRadioButton* m_paymasterFeeRadio{nullptr};
    QLabel* m_feeModeExplanation{nullptr};
    QLabel* m_feeSummary{nullptr};
    QCheckBox* m_subtractPaymasterFeeCheck{nullptr};
    QPushButton* m_paymasterExplanationButton{nullptr};
    QPushButton* m_advancedPaymasterButton{nullptr};
    QComboBox* m_feeModeCombo{nullptr};
    QFrame* m_advancedPaymasterFrame{nullptr};
    QComboBox* m_privacyCombo{nullptr};
    QComboBox* m_selectionCombo{nullptr};
    QSpinBox* m_feeCapSpin{nullptr};
    QSpinBox* m_maxAttemptsSpin{nullptr};
    QPushButton* m_refreshOffersButton{nullptr};
    QLabel* m_offersStatus{nullptr};
    QTableWidget* m_offersTable{nullptr};
    QFrame* m_persistedPaymasterSessionsFrame{nullptr};
    QComboBox* m_persistedPaymasterSessions{nullptr};
    QPushButton* m_loadPersistedPaymasterSessionButton{nullptr};
    QFrame* m_clientSafetyFrame{nullptr};
    QLabel* m_clientSafetyStatus{nullptr};
    QLabel* m_clientSafetyDetails{nullptr};
    QPushButton* m_configureClientSafetyButton{nullptr};
    QFrame* m_paymasterSessionFrame{nullptr};
    QLabel* m_paymasterStateValue{nullptr};
    QLabel* m_paymasterTransferValue{nullptr};
    QLabel* m_paymasterIdentityValue{nullptr};
    QLabel* m_paymasterCostValue{nullptr};
    QLabel* m_paymasterExpiryValue{nullptr};
    QPushButton* m_retrySessionButton{nullptr};
    QPushButton* m_fallbackSessionButton{nullptr};
    QPushButton* m_recoverSessionButton{nullptr};
    QPushButton* m_abandonSessionButton{nullptr};
    QPushButton* m_cancelQuoteButton{nullptr};
    QLabel* m_paymasterNextStepValue{nullptr};
    QPushButton* m_paymasterPrimaryButton{nullptr};
    QPushButton* m_paymasterMoreButton{nullptr};
    QPushButton* m_paymasterTechnicalButton{nullptr};
    QFrame* m_paymasterSecondaryActions{nullptr};
    QFrame* m_paymasterTechnicalDetails{nullptr};

    double m_paymasterInitialAvailableBalance{0.0};
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
    bool m_sendAllSpendableDD{false};
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
    PaymasterConfirmationGuard m_paymasterConfirmationGuard;
    PaymasterRecoveryConfirmationGuard m_paymasterRecoveryConfirmationGuard;

};

#endif // DIGIBYTE_QT_PAYMASTERSENDWIDGET_H
