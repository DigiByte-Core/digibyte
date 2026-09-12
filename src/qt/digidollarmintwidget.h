// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARMINTWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLARMINTWIDGET_H

#include <QWidget>

class WalletModel;
class ClientModel;
class AmountValidator;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QVBoxLayout;
class QHBoxLayout;
class QGridLayout;
class QFrame;
class QProgressBar;
QT_END_NAMESPACE

/**
 * DigiDollar mint widget for creating new DigiDollar positions.
 * This widget provides functionality to mint DigiDollar by locking
 * DGB collateral with proper collateral ratio calculation.
 */
class DigiDollarMintWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DigiDollarMintWidget(QWidget *parent = nullptr);
    ~DigiDollarMintWidget();

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);
    void updateView();

Q_SIGNALS:
    /** Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);

public Q_SLOTS:
    /** Update balance display */
    void updateBalance();
    /** Update oracle price for collateral calculation */
    void updateOraclePrice();
    /** Set privacy mode — masks balance and collateral displays */
    void setPrivacy(bool privacy);

private Q_SLOTS:
    /** Amount field changed */
    void onAmountChanged();
    /** Lock tier selection changed */
    void onLockTierChanged();
    /** Mint button clicked */
    void onMintClicked();
    /** Clear all fields */
    void onClearClicked();

private:
    void setupUI();
    void setupMintAmountSection();
    void setupLockTierSection();
    void setupCollateralSection();
    void setupButtonSection();
    void connectSignals();
    void updateMintButton();
    void updateCollateralCalculation();
    void updateAmountValidation();
    void updateUSDEquivalent();
    void applyTheme();
    void calculateRequiredCollateral();

    bool validateAmount() const;
    bool validateCollateral() const;

    QString formatDDAmount(double amount) const;
    QString formatDGBAmount(double amount) const;
    QString formatUSDAmount(double amount) const;
    QString formatDigiDollarUSDEquivalent(double amount) const;
    QString formatRatio(double ratio) const;
    /** Mask a formatted string by replacing digits with '#' */
    QString maskValue(const QString& value) const;

    double getCollateralRatioForTier(int tier) const;
    QString getLockTierDisplayName(int tier) const;
    int getLockTierBlocks(int tier) const;

    // UI components
    QVBoxLayout* m_mainLayout;

    // Mint amount section
    QFrame* m_amountFrame;
    QGridLayout* m_amountLayout;
    QLabel* m_amountLabel;
    QLineEdit* m_amountEdit;
    QLabel* m_amountSuffix;
    QLabel* m_usdValueLabel;
    QLabel* m_usdValueValue;
    QLabel* m_amountWarningLabel;

    // Lock tier section
    QFrame* m_lockTierFrame;
    QGridLayout* m_lockTierLayout;
    QLabel* m_lockTierLabel;
    QComboBox* m_lockTierCombo;
    QLabel* m_lockTierInfoLabel;
    QLabel* m_lockTierInfoValue;

    // Collateral section
    QFrame* m_collateralFrame;
    QGridLayout* m_collateralLayout;
    QLabel* m_oraclePriceLabel;
    QLabel* m_oraclePriceValue;
    QLabel* m_mintStatusLabel{nullptr};
    bool m_mintVolatilityAllowed{true};
    QLabel* m_collateralLabel;
    QLabel* m_collateralValue;
    QLabel* m_ratioLabel;
    QLabel* m_ratioValue;
    QProgressBar* m_ratioBar;
    QLabel* m_availableDGBLabel;
    QLabel* m_availableDGBValue;

    // Button section
    QFrame* m_buttonFrame;
    QHBoxLayout* m_buttonLayout;
    QPushButton* m_mintButton;
    QPushButton* m_clearButton;

    // Validators
    AmountValidator* m_amountValidator;

    // Models
    WalletModel* m_walletModel;
    ClientModel* m_clientModel;

    // Data
    double m_availableDGBBalance;
    double m_oraclePrice;
    double m_mintAmount;
    int m_selectedTier;
    double m_requiredCollateral;
    double m_collateralRatio;

    // Privacy
    bool m_privacy{false};

    // Tracks collateral shown to user so we can detect oracle price drift
    double m_lastDisplayedCollateral;
    double m_lastDisplayedOraclePrice;
};

#endif // DIGIBYTE_QT_DIGIDOLLARMINTWIDGET_H
