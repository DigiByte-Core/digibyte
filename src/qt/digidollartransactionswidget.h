// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARTRANSACTIONSWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLARTRANSACTIONSWIDGET_H

#include <QWidget>
#include <QList>
#include <consensus/amount.h>
#include <univalue.h>

class WalletModel;
class ClientModel;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QVBoxLayout;
class QHBoxLayout;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QMenu;
class QDialog;
QT_END_NAMESPACE

/**
 * DigiDollar transactions widget showing complete transaction history.
 * This widget displays all DigiDollar transactions (mints, sends, receives, redemptions)
 * with filtering and search capabilities.
 */
class DigiDollarTransactionsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DigiDollarTransactionsWidget(QWidget* parent = nullptr);
    ~DigiDollarTransactionsWidget();

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);
    void updateView();

    /** Test hook for reading what the Confirmations column shows, without a wallet. */
    QString confirmationsTextForTesting(int confirmations, bool isAbandoned = false, bool isLocal = false) const;

public Q_SLOTS:
    /** Set privacy mode — hides transactions table */
    void setPrivacy(bool privacy);
    /** Select and scroll to the transaction row with this txid. */
    void focusTransaction(const QString& txid);

Q_SIGNALS:
    void message(const QString& title, const QString& message, unsigned int style);

private Q_SLOTS:
    void updateTransactions();
    void onTypeFilterChanged(int index);
    void onSearchTextChanged();
    void showContextMenu(const QPoint& pos);
    void copyTxId();
    void copyAmount();
    void copyNote();
    void showDetails();
    void exportClicked();

private:
    void setupUI();
    void setupFilterBar();
    void setupTable();
    void connectSignals();
    void populateTable();
    bool selectTransaction(const QString& txid);
    bool isDarkTheme() const;
    QColor getAmountColor(bool isPositive) const;
    QString formatDDAmount(CAmount amount) const;
    QString formatTimestamp(uint64_t timestamp) const;
    QString formatConfirmations(int confirmations, bool isAbandoned = false, bool isLocal = false) const;
    QString formatLockPeriod(int lockTier) const;
    QString formatLockPeriodShort(int lockTier) const;

    // UI Components
    QVBoxLayout* m_mainLayout;
    QHBoxLayout* m_filterLayout;
    QComboBox* m_typeFilter;
    QLineEdit* m_searchEdit;
    QPushButton* m_exportButton;
    QTableWidget* m_table;
    QLabel* m_statusLabel;
    QMenu* m_contextMenu;
    QList<QDialog*> m_openedDialogs;

    // Privacy
    bool m_privacy{false};
    bool m_hasAppliedDefaultSort{false};
    bool m_initialSnapshotShown{false};
    bool m_refreshInFlight{false};
    bool m_refreshPending{false};
    UniValue m_cachedTransactions{UniValue::VARR};
    QString m_lastTransactionsFingerprint;
    QString m_pendingFocusTxid;

    // Models
    WalletModel* m_walletModel;
    ClientModel* m_clientModel;

    enum Column {
        Date = 0,
        Type,
        Amount,
        LockPeriod,
        Note,
        TxId,
        Confirmations,
        ColumnCount
    };
};

#endif // DIGIBYTE_QT_DIGIDOLLARTRANSACTIONSWIDGET_H
