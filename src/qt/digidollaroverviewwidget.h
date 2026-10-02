// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLAROVERVIEWWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLAROVERVIEWWIDGET_H

#include <QWidget>

class WalletModel;
class ClientModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QVBoxLayout;
class QHBoxLayout;
class QGridLayout;
class QFrame;
class QProgressBar;
class QListWidget;
class QListWidgetItem;
class QSpacerItem;
QT_END_NAMESPACE

/**
 * DigiDollar overview widget showing balances, oracle price, and system health.
 * This widget provides a summary view of the user's DigiDollar holdings and
 * system status information.
 */
class DigiDollarOverviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DigiDollarOverviewWidget(QWidget *parent = nullptr);
    ~DigiDollarOverviewWidget();

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);
    void updateView();

    /** Show incoming DigiDollar transaction notification */
    void incomingDDTransaction(const QString& date, const QString& amount,
                               const QString& type, const QString& address);

Q_SIGNALS:
    /** Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);
    /** Fired when a recent transaction row should open in the DD Transactions tab. */
    void recentTransactionActivated(const QString& txid);

public Q_SLOTS:
    /** Update balance displays */
    void updateBalance();
    /** Update oracle price display */
    void updateOraclePrice();
    /** Update system health status */
    void updateSystemHealth();
    /** Set monospaced font for balance labels */
    void setMonospacedFont(bool use_embedded_font);
    /** Set privacy mode — masks all values and hides transaction list */
    void setPrivacy(bool privacy);
    // REMOVED: updateTheme() and applyTheme() - CSS handles all theming now

private Q_SLOTS:
    /** Refresh immediately after wallet DigiDollar state changes. */
    void refreshDigiDollarState();
    /** Update recent transactions display */
    void updateRecentTransactions();
    /** Activate a recent transaction row. */
    void activateRecentTransaction(QListWidgetItem* item);

private:
    void setupUI();
    void setupBalanceSection();
    void setupSystemHealthSection();
    void setupRecentTransactionsSection();
    void connectSignals();
    void addDemoTransactions(); // For demo purposes only
    void updateSystemHealthIfDue(bool force);

    QString formatDDAmount(double amount) const;
    QString formatDGBAmount(double amount) const;
    QString formatUSDAmount(double amount) const;
    /** Mask a formatted string by replacing digits with '#' */
    QString maskValue(const QString& value) const;
    /** Keep blockchain total labels wide enough for launch-scale values. */
    void updateBlockchainTotalsMinimumWidth();

    // UI components
    QVBoxLayout* m_mainLayout;

    // Balance section
    QFrame* m_balanceFrame;
    QGridLayout* m_balanceLayout;
    QLabel* m_ddBalanceLabel;
    QLabel* m_ddBalanceValue;
    QLabel* m_ddPendingLabel;
    QLabel* m_ddPendingValue;
    QLabel* m_dgbCollateralLabel;
    QLabel* m_dgbCollateralValue;
    QLabel* m_usdValueLabel;
    QLabel* m_usdValueValue;

    // System health section
    QFrame* m_systemHealthFrame;
    QGridLayout* m_systemHealthLayout;
    QLabel* m_oraclePriceLabel;
    QLabel* m_oraclePriceValue;
    QLabel* m_networkTotalDDLabel;
    QLabel* m_networkTotalDDValue;
    QLabel* m_networkTotalCollateralLabel;
    QLabel* m_networkTotalCollateralValue;
    QLabel* m_systemHealthLabel;
    QLabel* m_systemHealthValue;
    QLabel* m_dcaLevelLabel;
    QLabel* m_dcaLevelValue;
    QLabel* m_errLevelLabel;
    QLabel* m_errLevelValue;
    QProgressBar* m_systemHealthBar;
    QLabel* m_mintStatusLabel;

    // Recent transactions section
    QFrame* m_transactionsFrame;
    QVBoxLayout* m_transactionsLayout;
    QLabel* m_transactionsTitle;
    QListWidget* m_transactionsList;
    QLabel* m_recentTransactionsInfo;

    // Models
    WalletModel* m_walletModel;
    ClientModel* m_clientModel;

    // Data
    double m_ddBalance;
    double m_dgbCollateral;
    double m_oraclePrice;
    QString m_systemHealthStatus;
    int m_dcaLevel;
    int m_errLevel;

    // Privacy
    bool m_privacy{false};

    // Throttling - minimum 5 seconds between updates during sync
    qint64 m_lastBalanceUpdateTime{0};
    qint64 m_lastTxUpdateTime{0};
    qint64 m_lastSystemHealthUpdateTime{0};
    static constexpr int UPDATE_THROTTLE_MS = 5000;
    static constexpr int SYSTEM_HEALTH_UPDATE_INTERVAL_MS = 30000;
};

#endif // DIGIBYTE_QT_DIGIDOLLAROVERVIEWWIDGET_H
