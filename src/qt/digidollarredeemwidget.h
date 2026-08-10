// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARREDEEMWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLARREDEEMWIDGET_H

#include <QWidget>
#include <memory>

class WalletModel;
class ClientModel;
class AmountValidator;
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
class QProgressBar;
QT_END_NAMESPACE

/**
 * DigiDollar redeem widget for redeeming positions and recovering collateral.
 * This widget provides functionality to redeem DigiDollar positions
 * either partially or fully.
 */
class DigiDollarRedeemWidget : public QWidget
{
    Q_OBJECT
    friend class DigiDollarWidgetTests;

public:
    explicit DigiDollarRedeemWidget(QWidget *parent = nullptr);
    ~DigiDollarRedeemWidget();

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);
    void updateView();

    /** Set position to redeem (e.g., from Vault tab) */
    void setPosition(const QString& outpoint);

Q_SIGNALS:
    /** Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);

    /** Fired when redemption completes successfully */
    void redemptionCompleted();

public Q_SLOTS:
    /** Update balance display */
    void updateBalance();
    /** Update positions information */
    void updatePositions();
    /** Set privacy mode — masks position values */
    void setPrivacy(bool privacy);

private Q_SLOTS:
    /** Position ID field changed */
    void onPositionIdChanged();
    /** Amount field changed */
    void onAmountChanged();
    /** Redeem button clicked */
    void onRedeemClicked();
    /** Redeem all button clicked - REMOVED (exact-amount redemption only) */
    // void onRedeemAllClicked();
    /** Clear all fields */
    void onClearClicked();
    /** Coin control button clicked */
    void onCoinControlButtonClicked();
    /** Update coin control labels */
    void updateCoinControlLabels();

private:
    void setupUI();
    void setupCoinControlSection();
    void setupPositionSection();
    void setupAmountSection();
    void setupPositionInfoSection();
    void setupButtonSection();
    void connectSignals();
    void updateRedeemButtons();
    void updatePositionInfo();
    void loadPositionDetails();
    void applyTheme();
    void updateValidationLabels();
    void updateAmountValidation();

    bool validatePositionId() const;
    bool validateAmount() const;
    bool validateRedeemable() const;
    bool validateDDBalance() const;
    /** True unless the wallet can never sign a redemption (no wallet model, or
     *  private keys disabled). A locked but encrypted wallet still holds its
     *  keys, so it returns true; see walletNeedsUnlockToRedeem(). */
    bool canWalletSignRedemption() const;
    /** True when redeeming will first prompt for the wallet passphrase. */
    bool walletNeedsUnlockToRedeem() const;
    QString redeemDisabledReason() const;

    QString formatDDAmount(double amount) const;
    QString formatDGBAmount(double amount) const;
    QString formatBlockTime(int blocks) const;
    /** Mask a formatted string by replacing digits with '#' */
    QString maskValue(const QString& value) const;

    // UI components
    QVBoxLayout* m_mainLayout;

    // Coin control section
    QFrame* m_coinControlFrame;
    QHBoxLayout* m_coinControlLayout;
    QPushButton* m_coinControlButton;
    QLabel* m_coinControlQuantityLabel;
    QLabel* m_coinControlAmountLabel;

    // Position section
    QFrame* m_positionFrame;
    QGridLayout* m_positionLayout;
    QLabel* m_positionIdLabel;
    QLineEdit* m_positionIdEdit;
    QLabel* m_positionValidationLabel;

    // Amount section
    QFrame* m_amountFrame;
    QGridLayout* m_amountLayout;
    QLabel* m_amountLabel;
    QLineEdit* m_amountEdit;
    QLabel* m_amountSuffix;
    QLabel* m_redeemableLabel;
    QLabel* m_redeemableValue;

    // Position info section
    QFrame* m_positionInfoFrame;
    QGridLayout* m_positionInfoLayout;
    QLabel* m_positionInfoLabel;
    QLabel* m_ddMintedLabel;
    QLabel* m_ddMintedValue;
    QLabel* m_dgbCollateralLabel;
    QLabel* m_dgbCollateralValue;
    QLabel* m_lockTierLabel;
    QLabel* m_lockTierValue;
    QLabel* m_timeRemainingLabel;
    QLabel* m_timeRemainingValue;
    QLabel* m_healthStatusLabel;
    QLabel* m_healthStatusValue;
    QProgressBar* m_healthBar;

    // Button section
    QFrame* m_buttonFrame;
    QHBoxLayout* m_buttonLayout;
    QPushButton* m_redeemButton;
    // QPushButton* m_redeemAllButton;  // REMOVED - exact-amount redemption only
    QPushButton* m_clearButton;

    // Validators
    AmountValidator* m_amountValidator;

    // Models
    WalletModel* m_walletModel;
    ClientModel* m_clientModel;

    // Position data
    QString m_selectedPositionId;
    double m_positionDDMinted;
    double m_positionDGBCollateral;
    int m_positionLockTier;
    int m_positionBlocksRemaining;
    double m_positionHealth;
    double m_redeemableAmount;
    bool m_positionFound;

    // Privacy
    bool m_privacy{false};

    // Coin control
    std::unique_ptr<wallet::DDCoinControl> m_coinControl;
};

#endif // DIGIBYTE_QT_DIGIDOLLARREDEEMWIDGET_H
