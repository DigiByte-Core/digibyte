// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARPOSITIONSWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLARPOSITIONSWIDGET_H

#include <QWidget>
#include <consensus/amount.h>
#include <wallet/digidollarwallet.h>

#include <set>

class WalletModel;
class ClientModel;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QTableWidgetItem;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QLabel;
class QHeaderView;
class QProgressBar;
QT_END_NAMESPACE

struct DigiDollarPosition {
    QString positionId;
    double ddMinted;
    double dgbCollateral;
    int lockTier;
    int64_t unlockHeight;  // Block height when vault unlocks
    int blocksRemaining;
    double health;
    bool canRedeem;
    bool isPendingMint;
    bool isPendingRedeem;
    bool isRedeemed;
    QString failedMintStatus;
    int64_t mintTime;      // Unix timestamp of mint transaction
};

/**
 * DigiDollar positions widget showing all active positions in a table.
 * This widget provides a comprehensive view of all DigiDollar positions
 * with ability to manage individual positions.
 */
class DigiDollarPositionsWidget : public QWidget
{
    Q_OBJECT
    friend class DigiDollarWidgetTests;

public:
    explicit DigiDollarPositionsWidget(QWidget *parent = nullptr);
    ~DigiDollarPositionsWidget();

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);
    void updateView();

Q_SIGNALS:
    /** Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);
    /** Fired when redeem is requested for a specific position */
    void redeemRequested(const QString &positionId);

public Q_SLOTS:
    /** Update positions table */
    void updatePositions();
    /** Set privacy mode — hides positions table */
    void setPrivacy(bool privacy);

private Q_SLOTS:
    /** Position table cell clicked */
    void onPositionClicked(int row, int column);
    /** Redeem button clicked for a specific position */
    void onRedeemPositionClicked();
    /** Show context menu for table */
    void showContextMenu(const QPoint& point);

private:
    void setupUI();
    void setupTableHeader();
    void connectSignals();
    void connectWalletSignals();
    void connectClientSignals();
    void populatePositionsTable();
    void loadPositionsFromWallet();
    void applyTheme();
    void addPositionToTable(const DigiDollarPosition& position, int row);
    // Wallet-state badges keep matured vaults from looking redeemable when
    // this wallet cannot currently sign.
    QPushButton* createRedeemButton(const QString& positionId, bool isPendingMint, bool isPendingRedeem, bool isRedeemed, bool canRedeem, bool isWatchOnly, bool isWalletLocked, int blocksRemaining);

    QString formatDDAmount(double amount) const;
    QString formatDGBAmount(double amount) const;
    QString formatBlockTime(int blocks) const;
    QString formatHealthStatus(double health) const;
    QWidget* createHealthWidget(double health) const;

    // Backend integration helpers
    CAmount GetMockOraclePrice() const;
    std::vector<WalletCollateralPosition> GetWalletPositions() const;
    std::set<uint256> GetPendingRedeemPositionIds() const;
    double CalculatePositionHealth(CAmount ddAmount, CAmount dgbCollateral, CAmount oraclePrice) const;
    int getLockTierBlocks(int tier) const;

    // UI components
    QVBoxLayout* m_mainLayout;
    QHBoxLayout* m_headerLayout;
    QLabel* m_titleLabel;
    QTableWidget* m_positionsTable;
    QLabel* m_statusLabel;

    // Models
    WalletModel* m_walletModel;
    ClientModel* m_clientModel;

    // Data
    QList<DigiDollarPosition> m_positions;

    // Privacy
    bool m_privacy{false};

public:
    // Table columns — public so widget tests can address columns by name
    // when asserting layout invariants (e.g. minimum column widths).
    enum PositionColumn {
        COL_POSITION_ID = 0,
        COL_DD_MINTED = 1,
        COL_DGB_COLLATERAL = 2,
        COL_LOCK_DATE = 3,      // New: when vault was created
        COL_LOCK_TIER = 4,      // Renamed from Lock Period
        COL_TIME_REMAINING = 5,
        COL_HEALTH = 6,
        COL_ACTIONS = 7,
        NUM_COLUMNS = 8
    };

private:

    // Auto-refresh timer — must be stopped during shutdown (Bug #23)
    QTimer* m_autoRefreshTimer{nullptr};
    void stopRefresh();

    // Throttling - minimum 5 seconds between updates during sync
    qint64 m_lastUpdateTime{0};
    static constexpr int UPDATE_THROTTLE_MS = 5000;
};

#endif // DIGIBYTE_QT_DIGIDOLLARPOSITIONSWIDGET_H
