// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarpositionswidget.h>

#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <interfaces/node.h>
#include <qt/digibyteunits.h>
#include <interfaces/wallet.h>
#include <wallet/digidollarwallet.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <oracle/mock_oracle.h>
#include <oracle/bundle_manager.h>
#include <chainparams.h>
#include <shutdown.h>
#include <algorithm>
#include <cmath>

#include <QTableWidget>
#include <QTableWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QProgressBar>
#include <QFont>
#include <QMessageBox>
#include <QTimer>
#include <QDateTime>
#include <QFrame>
#include <QApplication>
#include <QPalette>
#include <QMenu>
#include <QAction>

DigiDollarPositionsWidget::DigiDollarPositionsWidget(QWidget *parent) :
    QWidget(parent),
    m_mainLayout(nullptr),
    m_headerLayout(nullptr),
    m_titleLabel(nullptr),
    m_positionsTable(nullptr),
    m_statusLabel(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr)
{
    setupUI();
    connectSignals();
    // applyTheme(); // REMOVED: Now handled by CSS files
}

DigiDollarPositionsWidget::~DigiDollarPositionsWidget()
{
    stopRefresh();
}

void DigiDollarPositionsWidget::stopRefresh()
{
    if (m_autoRefreshTimer) {
        m_autoRefreshTimer->stop();
    }
}

void DigiDollarPositionsWidget::setupUI()
{
    // Create main layout - compact like DGB tabs
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Header section
    m_headerLayout = new QHBoxLayout();

    m_titleLabel = new QLabel(tr("DigiDollar Time Lock DGB Vault"), this);
    QFont titleFont = m_titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    m_titleLabel->setFont(titleFont);

    m_headerLayout->addWidget(m_titleLabel);
    m_headerLayout->addStretch();

    m_mainLayout->addLayout(m_headerLayout);

    // Positions table
    m_positionsTable = new QTableWidget(this);
    m_positionsTable->setObjectName("positionsTable");
    m_positionsTable->setColumnCount(NUM_COLUMNS);
    m_positionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_positionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_positionsTable->setAlternatingRowColors(true);
    m_positionsTable->setSortingEnabled(true);
    m_positionsTable->verticalHeader()->setVisible(false);

    // Setup table header
    setupTableHeader();

    m_positionsTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_mainLayout->addWidget(m_positionsTable, 1);  // stretch factor 1 to fill space

    // Status label
    m_statusLabel = new QLabel(tr("Loading vaults..."), this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    // Theme styling will be applied in applyTheme()
    m_mainLayout->addWidget(m_statusLabel);

    setLayout(m_mainLayout);
}

void DigiDollarPositionsWidget::setupTableHeader()
{
    QStringList headers;
    headers << tr("Vault ID")
            << tr("$DD Minted")
            << tr("DGB Collateral")
            << tr("Lock Date")
            << tr("Lock Tier")
            << tr("Time Remaining")
            << tr("Health")
            << tr("Actions");

    m_positionsTable->setHorizontalHeaderLabels(headers);

    // Enhanced header configuration
    QHeaderView* header = m_positionsTable->horizontalHeader();
    header->setStretchLastSection(false);
    header->setDefaultAlignment(Qt::AlignCenter);
    header->setHighlightSections(true);
    header->setMinimumSectionSize(50);

    // Set column resize modes - VAULT ID stretches to fill remaining space
    header->setSectionResizeMode(COL_POSITION_ID, QHeaderView::Stretch);   // Stretch to fill width
    header->setSectionResizeMode(COL_DD_MINTED, QHeaderView::Interactive);
    header->setSectionResizeMode(COL_DGB_COLLATERAL, QHeaderView::Interactive);
    header->setSectionResizeMode(COL_LOCK_DATE, QHeaderView::Interactive);
    header->setSectionResizeMode(COL_LOCK_TIER, QHeaderView::Interactive);
    header->setSectionResizeMode(COL_TIME_REMAINING, QHeaderView::Interactive);
    header->setSectionResizeMode(COL_HEALTH, QHeaderView::Fixed);
    header->setSectionResizeMode(COL_ACTIONS, QHeaderView::Fixed);

    // Set balanced column widths - VAULT ID stretches, others fixed
    // Total fixed: 115 + 145 + 100 + 85 + 130 + 105 + 100 = 780px, leaves ~170px for VAULT ID
    m_positionsTable->setColumnWidth(COL_DD_MINTED, 115);        // DD Minted
    m_positionsTable->setColumnWidth(COL_DGB_COLLATERAL, 145);   // DGB Collateral
    m_positionsTable->setColumnWidth(COL_LOCK_DATE, 100);        // Lock Date
    m_positionsTable->setColumnWidth(COL_LOCK_TIER, 150);        // Lock Tier — must fit "10 years" / "3 months" without truncation
    m_positionsTable->setColumnWidth(COL_TIME_REMAINING, 130);   // Time Remaining (wider to fit header)
    m_positionsTable->setColumnWidth(COL_HEALTH, 105);           // Health
    m_positionsTable->setColumnWidth(COL_ACTIONS, 100);          // Actions

    // Add sort indicators to sortable columns
    m_positionsTable->setSortingEnabled(true);
    header->setSortIndicatorShown(true);

    // Set sorting behavior - enable section clicking
    header->setSectionsClickable(true);

    // Add tooltips to headers for better UX
    QTableWidgetItem* idHeaderItem = m_positionsTable->horizontalHeaderItem(COL_POSITION_ID);
    if (idHeaderItem) idHeaderItem->setToolTip(tr("Unique identifier for this DigiDollar vault"));

    QTableWidgetItem* ddHeaderItem = m_positionsTable->horizontalHeaderItem(COL_DD_MINTED);
    if (ddHeaderItem) ddHeaderItem->setToolTip(tr("Amount of DigiDollar tokens minted for this position"));

    QTableWidgetItem* dgbHeaderItem = m_positionsTable->horizontalHeaderItem(COL_DGB_COLLATERAL);
    if (dgbHeaderItem) dgbHeaderItem->setToolTip(tr("DigiByte collateral locked in this position"));

    QTableWidgetItem* lockDateHeaderItem = m_positionsTable->horizontalHeaderItem(COL_LOCK_DATE);
    if (lockDateHeaderItem) lockDateHeaderItem->setToolTip(tr("Date when this vault was created"));

    QTableWidgetItem* tierHeaderItem = m_positionsTable->horizontalHeaderItem(COL_LOCK_TIER);
    if (tierHeaderItem) tierHeaderItem->setToolTip(tr("Lock tier (time lock duration) for this vault"));

    QTableWidgetItem* timeHeaderItem = m_positionsTable->horizontalHeaderItem(COL_TIME_REMAINING);
    if (timeHeaderItem) timeHeaderItem->setToolTip(tr("Time remaining before position can be redeemed"));

    QTableWidgetItem* healthHeaderItem = m_positionsTable->horizontalHeaderItem(COL_HEALTH);
    if (healthHeaderItem) healthHeaderItem->setToolTip(tr("Position health based on collateralization ratio"));

    QTableWidgetItem* actionsHeaderItem = m_positionsTable->horizontalHeaderItem(COL_ACTIONS);
    if (actionsHeaderItem) actionsHeaderItem->setToolTip(tr("Available actions for this position"));
}

void DigiDollarPositionsWidget::connectSignals()
{
    // Connect table clicks
    connect(m_positionsTable, &QTableWidget::cellClicked,
            this, &DigiDollarPositionsWidget::onPositionClicked);

    // Connect context menu
    connect(m_positionsTable, &QTableWidget::customContextMenuRequested,
            this, &DigiDollarPositionsWidget::showContextMenu);

    // Auto-refresh timer (every 60 seconds)
    m_autoRefreshTimer = new QTimer(this);
    connect(m_autoRefreshTimer, &QTimer::timeout,
            this, &DigiDollarPositionsWidget::updatePositions);
    m_autoRefreshTimer->start(60000); // 60 seconds
}

void DigiDollarPositionsWidget::connectWalletSignals()
{
    if (!m_walletModel) {
        return;
    }

    // Connect to wallet balance changes (indicates new transactions)
    connect(m_walletModel, &WalletModel::balanceChanged,
            this, &DigiDollarPositionsWidget::updatePositions);
    connect(m_walletModel, &WalletModel::encryptionStatusChanged, this, [this]() {
        m_lastUpdateTime = 0;
        updatePositions();
    });
}

void DigiDollarPositionsWidget::connectClientSignals()
{
    if (!m_clientModel) {
        return;
    }

    // NOTE: We do NOT connect to numBlocksChanged for DD updates!
    // Positions update when balance changes (via balanceChanged signal).
    // This prevents constant updates during block sync.
    // Timelock countdown and health are updated via the 60-second auto-refresh timer.
}

void DigiDollarPositionsWidget::setWalletModel(WalletModel* model)
{
    m_walletModel = model;

    if (m_walletModel) {
        // Connect wallet model signals for real-time updates
        connectWalletSignals();
        updatePositions();
        // applyTheme(); // REMOVED: Now handled by CSS files
    } else {
        // Wallet being torn down — stop refresh to prevent deadlock (Bug #23)
        stopRefresh();
    }
}

void DigiDollarPositionsWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    if (m_clientModel) {
        // Connect client model signals for block updates
        connectClientSignals();
        updatePositions();
        // applyTheme(); // REMOVED: Now handled by CSS files
    }
}

void DigiDollarPositionsWidget::updateView()
{
    if (!isVisible()) return;
    updatePositions();
}

void DigiDollarPositionsWidget::updatePositions()
{
    if (!isVisible()) {
        return;
    }

    // Bail out during shutdown to prevent deadlock on cs_dd_wallet (Bug #23)
    if (ShutdownRequested() || !m_walletModel || !m_clientModel) {
        return;
    }

    // Skip updates during Initial Block Download - DD data only matters when synced
    if (m_clientModel && m_clientModel->node().isInitialBlockDownload()) {
        return;
    }

    // Throttle updates - skip if less than 5 seconds since last update
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastUpdateTime < UPDATE_THROTTLE_MS) {
        return; // Skip this update, too soon
    }
    m_lastUpdateTime = now;

    loadPositionsFromWallet();
    populatePositionsTable();
}

void DigiDollarPositionsWidget::onPositionClicked(int row, int column)
{
    if (row < 0 || row >= m_positionsTable->rowCount()) {
        return;
    }

    // Get position ID from the first column
    QTableWidgetItem* idItem = m_positionsTable->item(row, COL_POSITION_ID);
    if (!idItem) return;

    QString positionId = idItem->text();

    // Handle different column clicks
    switch (column) {
    case COL_POSITION_ID:
        // Copy vault ID to clipboard
        GUIUtil::setClipboard(positionId);
        Q_EMIT message(tr("Vault ID Copied"),
                    tr("Vault ID copied to clipboard: %1").arg(positionId),
                    QMessageBox::Information);
        break;
    default:
        // Other columns - could add more functionality here
        break;
    }
}

void DigiDollarPositionsWidget::onRedeemPositionClicked()
{
    QPushButton* button = qobject_cast<QPushButton*>(sender());
    if (!button) return;

    QString positionId = button->property("positionId").toString();

    // Find the position in our list
    DigiDollarPosition position;
    bool found = false;
    for (const auto& pos : m_positions) {
        if (pos.positionId == positionId) {
            position = pos;
            found = true;
            break;
        }
    }

    if (!found) {
        Q_EMIT message(tr("Error"), tr("Position not found: %1").arg(positionId), QMessageBox::Warning);
        return;
    }

    // Emit signal to request redemption (will be handled by parent tab)
    Q_EMIT redeemRequested(positionId);
}

void DigiDollarPositionsWidget::showContextMenu(const QPoint& point)
{
    QTableWidgetItem* item = m_positionsTable->itemAt(point);
    if (!item) return;

    int row = item->row();
    if (row < 0 || row >= m_positions.size()) return;

    const DigiDollarPosition& position = m_positions[row];

    QMenu contextMenu(this);

    // Copy Vault ID action
    QAction* copyIdAction = contextMenu.addAction(tr("📋 Copy Vault ID"));
    connect(copyIdAction, &QAction::triggered, [this, position]() {
        GUIUtil::setClipboard(position.positionId);
        Q_EMIT message(tr("Vault ID Copied"),
                    tr("Vault ID copied to clipboard: %1").arg(position.positionId),
                    QMessageBox::Information);
    });

    contextMenu.addSeparator();

    // Show Details action
    QAction* detailsAction = contextMenu.addAction(tr("📊 Show Details"));
    connect(detailsAction, &QAction::triggered, [this, position]() {
        // Get lock period name. Tier 0 (1 hour) MUST have its
        // own case — falling through to the default would mislabel a 240-block
        // position as "1 year". Order and labels MUST match the
        // canonical 10 tiers in consensus/digidollar.h and the corresponding
        // Lock Tier table column above.
        QString lockPeriodName;
        switch(position.lockTier) {
            case 0: lockPeriodName = tr("1 hour + 100 block buffer"); break;
            case 1: lockPeriodName = tr("30 days"); break;
            case 2: lockPeriodName = tr("3 months"); break;
            case 3: lockPeriodName = tr("6 months"); break;
            case 4: lockPeriodName = tr("1 year"); break;
            case 5: lockPeriodName = tr("2 years"); break;
            case 6: lockPeriodName = tr("3 years"); break;
            case 7: lockPeriodName = tr("5 years"); break;
            case 8: lockPeriodName = tr("7 years"); break;
            case 9: lockPeriodName = tr("10 years"); break;
            default: lockPeriodName = tr("Unknown"); break;
        }

        const QString healthText = formatHealthStatus(position.health);
        QString details = tr("Vault Details\n\n"
                           "Vault ID: %1\n"
                           "$DD Minted: %2\n"
                           "DGB Collateral: %3\n"
                           "Lock Period: %4\n"
                           "Blocks Remaining: %5\n"
                           "Health: %6\n"
                           "Can Redeem: %7")
                           .arg(position.positionId)
                           .arg(formatDDAmount(position.ddMinted))
                           .arg(formatDGBAmount(position.dgbCollateral))
                           .arg(lockPeriodName)
                           .arg(position.blocksRemaining)
                           .arg(healthText)
                           .arg(position.canRedeem ? tr("Yes") : tr("No"));

        QMessageBox::information(this, tr("Vault Details"), details);
    });

    contextMenu.addSeparator();

    // Redeem action (if applicable)
    if (position.canRedeem) {
        QAction* redeemAction = contextMenu.addAction(tr("💰 Redeem Vault"));
        connect(redeemAction, &QAction::triggered, [this, position]() {
            // Emit signal to request redemption
            Q_EMIT redeemRequested(position.positionId);
        });
    }

    // Style the context menu to match the theme
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString menuStyle = QString(
        "QMenu { "
        "  background-color: %1; "
        "  color: %2; "
        "  border: 1px solid %3; "
        "  border-radius: 4px; "
        "  padding: 4px; "
        "} "
        "QMenu::item { "
        "  padding: 8px 16px; "
        "  border-radius: 3px; "
        "} "
        "QMenu::item:selected { "
        "  background-color: %4; "
        "  color: white; "
        "}")
        .arg(isDarkTheme ? "#3a3a3a" : "#ffffff")
        .arg(isDarkTheme ? "#e0e0e0" : "#333333")
        .arg(isDarkTheme ? "#555555" : "#cccccc")
        .arg("#0078d4");

    contextMenu.setStyleSheet(menuStyle);
    contextMenu.exec(m_positionsTable->mapToGlobal(point));
}

void DigiDollarPositionsWidget::loadPositionsFromWallet()
{
    m_positions.clear();

    if (!m_walletModel || !m_clientModel) {
        return;
    }

    // Get current blockchain height for timelock calculations
    int currentHeight = m_clientModel->getNumBlocks();

    // Get current oracle price in micro-USD; regtest can use mock helpers,
    // while testnet/mainnet use the live oracle price.
    CAmount oraclePrice = GetMockOraclePrice();
    // Only a watch-only wallet can never sign a redemption. A locked but
    // encrypted wallet still holds its keys and the redeem flow prompts for the
    // passphrase, so its vaults stay redeemable.
    const bool walletCanSign = !m_walletModel->wallet().privateKeysDisabled();

    // Get positions from wallet backend
    std::vector<WalletCollateralPosition> walletPositions = GetWalletPositions();
    const std::set<uint256> pendingRedeemPositions = GetPendingRedeemPositionIds();

    for (const auto& wp : walletPositions) {
        // Skip positions with 0 collateral - these are RECEIVED DD, not minted vaults
        // Only minted vaults (where user locked collateral) should appear in the Vault tab
        if (wp.dgb_collateral == 0) {
            continue;
        }

        // NOTE: We do NOT filter by IsDDTokenUnspent here anymore!
        //
        // When the user spends DD from a vault (e.g., sends $30 to Alice), the original mint
        // output at txid:1 gets spent and user receives DD change at a NEW UTXO. But the
        // collateral is STILL LOCKED in the vault! The vault should still be shown.
        //
        // Filtering by dgb_collateral > 0 (done above) is sufficient:
        // - Minted vaults: dgb_collateral > 0, shown in Vault tab
        // - Received DD: dgb_collateral = 0, skipped (correctly filtered above)
        //
        // Show ALL minted positions (active or redeemed) for the user who locked the collateral
        DigiDollarPosition pos;

        // Position ID (use txid as string)
        pos.positionId = QString::fromStdString(wp.dd_timelock_id.ToString());

        // DD minted (convert from cents to DD with 8 decimals)
        pos.ddMinted = wp.dd_minted / 100.0; // cents to dollars

        // DGB collateral (convert from satoshis to DGB)
        pos.dgbCollateral = wp.dgb_collateral / 100000000.0;

        // Lock tier
        pos.lockTier = wp.lock_tier;

        // Store unlock height for lock date calculation
        pos.unlockHeight = wp.unlock_height;

        // Calculate blocks remaining until unlock
        pos.blocksRemaining = std::max<int64_t>(0, wp.unlock_height - currentHeight);

        // Calculate health ratio using actual oracle price
        pos.health = CalculatePositionHealth(wp.dd_minted, wp.dgb_collateral, oraclePrice);

        // Can redeem if timelock expired, position is active, and the wallet can sign.
        // Private-key-disabled/watch-only wallets may observe vaults but can never redeem them.
        pos.canRedeem = walletCanSign && (pos.blocksRemaining == 0) && wp.is_active;

        // A freshly-created mint can be known to the wallet before the collateral
        // outpoint is confirmed in the UTXO set. During that window the wallet
        // reconciliation layer may mark the position inactive because the coin
        // lookup returns spent/missing, but that is NOT a redeemed vault. Surface
        // it as confirming until the mint has at least one confirmation.
        pos.isPendingMint = false;
        try {
            interfaces::WalletTxStatus status;
            interfaces::WalletOrderForm orderForm;
            bool inMempool = false;
            int numBlocks = 0;
            interfaces::WalletTx details = m_walletModel->wallet().getWalletTxDetails(
                wp.dd_timelock_id, status, orderForm, inMempool, numBlocks);
            if (details.tx && !status.is_abandoned && !status.is_in_main_chain && status.depth_in_main_chain == 0) {
                // Wallet-local mints may not be in the mempool yet (for example
                // when relay is disabled with -blocksonly), but they are still
                // unconfirmed vaults, not redeemed vaults.
                pos.isPendingMint = true;
            }
        } catch (...) {
            pos.isPendingMint = false;
        }

        // A wallet marks the position inactive as soon as a redemption spend is
        // created. Keep unconfirmed spends visually pending until they confirm.
        pos.isPendingRedeem = !pos.isPendingMint && !wp.is_active && pendingRedeemPositions.count(wp.dd_timelock_id) > 0;
        pos.isRedeemed = !pos.isPendingMint && !wp.is_active && !pos.isPendingRedeem;

        // Get the mint transaction timestamp from the wallet
        pos.mintTime = 0;  // Default to 0 (will show current time if not found)
        try {
            interfaces::WalletTx wtx = m_walletModel->wallet().getWalletTx(wp.dd_timelock_id);
            if (wtx.tx) {
                pos.mintTime = wtx.time;
            }
        } catch (...) {
            // If transaction lookup fails, leave mintTime at 0
        }

        m_positions.append(pos);
    }

    // Sort positions by mintTime descending (most recent mint first)
    std::sort(m_positions.begin(), m_positions.end(),
              [](const DigiDollarPosition& a, const DigiDollarPosition& b) {
                  return a.mintTime > b.mintTime;
              });
}

void DigiDollarPositionsWidget::populatePositionsTable()
{
    // Clear existing rows
    m_positionsTable->setRowCount(0);

    if (m_positions.isEmpty()) {
        m_statusLabel->setText(tr("📋 No DigiDollar vaults found\n\nYou haven't created any DigiDollar vaults yet.\nUse the 'Mint' tab to create your first DigiDollar vault."));
        m_statusLabel->show();
        return;
    }

    // Hide status label when we have positions
    m_statusLabel->hide();

    // Populate with sorting disabled. QTableWidget can move rows while items
    // are inserted when sorting is active; if that happens before the Health
    // and Actions cell widgets are attached, widgets land on the wrong row or
    // disappear. Re-enable the user's previous sorting state after the rows are
    // complete.
    const bool sortingWasEnabled = m_positionsTable->isSortingEnabled();
    m_positionsTable->setSortingEnabled(false);
    m_positionsTable->setRowCount(m_positions.size());
    for (int i = 0; i < m_positions.size(); ++i) {
        addPositionToTable(m_positions[i], i);
    }
    m_positionsTable->setSortingEnabled(sortingWasEnabled);

    // Resize table to fit content
    m_positionsTable->resizeRowsToContents();
}

void DigiDollarPositionsWidget::addPositionToTable(const DigiDollarPosition& position, int row)
{
    QFont monospaceFont = GUIUtil::fixedPitchFont();

    // Determine theme for redeemed styling
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;
    QColor redeemedTextColor = isDarkTheme ? QColor("#888888") : QColor("#999999");
    QColor redeemedBgColor = isDarkTheme ? QColor("#2a2a2a") : QColor("#f5f5f5");

    // Vault ID
    QTableWidgetItem* idItem = new QTableWidgetItem(position.positionId);
    idItem->setFont(monospaceFont);
    idItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    idItem->setToolTip(tr("Vault ID: %1\nClick to copy to clipboard").arg(position.positionId));

    // Apply redeemed styling
    if (position.isRedeemed) {
        idItem->setForeground(QBrush(redeemedTextColor));
        idItem->setBackground(QBrush(redeemedBgColor));
    }

    m_positionsTable->setItem(row, COL_POSITION_ID, idItem);

    // DD Minted
    QTableWidgetItem* ddItem = new QTableWidgetItem(formatDDAmount(position.ddMinted));
    ddItem->setFont(monospaceFont);
    ddItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    ddItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ddItem->setToolTip(tr("DigiDollar Minted: %1\nThis is the amount of $DD tokens you received for this position")
                     .arg(formatDDAmount(position.ddMinted)));

    // Apply redeemed styling
    if (position.isRedeemed) {
        ddItem->setForeground(QBrush(redeemedTextColor));
        ddItem->setBackground(QBrush(redeemedBgColor));
    }

    m_positionsTable->setItem(row, COL_DD_MINTED, ddItem);

    // DGB Collateral
    QTableWidgetItem* dgbItem = new QTableWidgetItem(formatDGBAmount(position.dgbCollateral));
    dgbItem->setFont(monospaceFont);
    dgbItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    dgbItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    dgbItem->setToolTip(tr("DGB Collateral: %1\nThis is your locked DigiByte collateral that backs this position")
                       .arg(formatDGBAmount(position.dgbCollateral)));

    // Apply redeemed styling
    if (position.isRedeemed) {
        dgbItem->setForeground(QBrush(redeemedTextColor));
        dgbItem->setBackground(QBrush(redeemedBgColor));
    }

    m_positionsTable->setItem(row, COL_DGB_COLLATERAL, dgbItem);

    // Lock Date - Use the actual mint transaction timestamp
    QDateTime lockDate;
    if (position.mintTime > 0) {
        // Use the actual transaction timestamp
        lockDate = QDateTime::fromSecsSinceEpoch(position.mintTime);
    } else {
        // Fallback: estimate from block heights (less accurate)
        int lockTierBlocks = getLockTierBlocks(position.lockTier);
        int64_t elapsedBlocks = lockTierBlocks - position.blocksRemaining;
        elapsedBlocks = std::max<int64_t>(0, std::min<int64_t>(elapsedBlocks, lockTierBlocks));
        int64_t elapsedSeconds = elapsedBlocks * 15; // 15 seconds per block
        lockDate = QDateTime::currentDateTime().addSecs(-elapsedSeconds);
    }

        // Calculate estimated mint height for tooltip. unlockHeight includes
        // the canonical lock period plus the 100-block confirmation buffer.
        int lockTierBlocks = getLockTierBlocks(position.lockTier);
        const int bufferBlocks = DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;
        int64_t lockHeight = position.unlockHeight - lockTierBlocks - bufferBlocks;

    QString lockDateStr = lockDate.toString("yyyy-MM-dd");
    QTableWidgetItem* lockDateItem = new QTableWidgetItem(lockDateStr);
    lockDateItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    lockDateItem->setTextAlignment(Qt::AlignCenter);
    // Store mintTime for sorting (higher = more recent)
    lockDateItem->setData(Qt::UserRole, QVariant::fromValue(position.mintTime));
    lockDateItem->setToolTip(tr("Vault created: %1\nMint block height: %2\nUnlock block height: %3")
                            .arg(lockDate.toString("yyyy-MM-dd hh:mm"))
                            .arg(lockHeight)
                            .arg(position.unlockHeight));

    // Apply redeemed styling
    if (position.isRedeemed) {
        lockDateItem->setForeground(QBrush(redeemedTextColor));
        lockDateItem->setBackground(QBrush(redeemedBgColor));
    }

    m_positionsTable->setItem(row, COL_LOCK_DATE, lockDateItem);

    // Lock Tier (renamed from Lock Period)
    // IMPORTANT: These tiers MUST match getLockTierDisplayName() in digidollarmintwidget.cpp
    // and the consensus tier definitions in consensus/digidollar.h
    QString lockPeriodName;
    QString lockPeriodTooltip;
    switch(position.lockTier) {
        case 0:
            lockPeriodName = tr("1 hour + 100 block buffer");
            lockPeriodTooltip = tr("1 hour time lock plus 100-block confirmation buffer (1000% collateral)");
            break;
        case 1:
            lockPeriodName = tr("30 days");
            lockPeriodTooltip = tr("30 day time lock (500% collateral)");
            break;
        case 2:
            lockPeriodName = tr("3 months");
            lockPeriodTooltip = tr("3 month time lock (400% collateral)");
            break;
        case 3:
            lockPeriodName = tr("6 months");
            lockPeriodTooltip = tr("6 month time lock (350% collateral)");
            break;
        case 4:
            lockPeriodName = tr("1 year");
            lockPeriodTooltip = tr("1 year time lock (300% collateral)");
            break;
        case 5:
            lockPeriodName = tr("2 years");
            lockPeriodTooltip = tr("2 year time lock (275% collateral)");
            break;
        case 6:
            lockPeriodName = tr("3 years");
            lockPeriodTooltip = tr("3 year time lock (250% collateral)");
            break;
        case 7:
            lockPeriodName = tr("5 years");
            lockPeriodTooltip = tr("5 year time lock (225% collateral)");
            break;
        case 8:
            lockPeriodName = tr("7 years");
            lockPeriodTooltip = tr("7 year time lock (212% collateral)");
            break;
        case 9:
            lockPeriodName = tr("10 years");
            lockPeriodTooltip = tr("10 year time lock (200% collateral)");
            break;
        default:
            lockPeriodName = tr("Unknown");
            lockPeriodTooltip = tr("Unknown lock tier");
    }
    if (position.lockTier >= 0 && position.lockTier <= 9) {
        const int lock_days = DigiDollar::BlocksToLockDays(getLockTierBlocks(position.lockTier));
        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(lock_days);
        const int ratio = DigiDollar::GetCollateralRatioForLockTime(lock_blocks, Params().GetDigiDollarParams());
        if (ratio > 0) {
            if (position.lockTier == 0) {
                lockPeriodTooltip = tr("1 hour time lock plus %1-block confirmation buffer (%2% collateral)")
                                        .arg(DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS)
                                        .arg(ratio);
            } else {
                lockPeriodTooltip = tr("%1 time lock (%2% collateral)").arg(lockPeriodName).arg(ratio);
            }
        }
    }

    QTableWidgetItem* tierItem = new QTableWidgetItem(lockPeriodName);
    tierItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    tierItem->setTextAlignment(Qt::AlignCenter);
    tierItem->setToolTip(lockPeriodTooltip);

    // Apply redeemed styling
    if (position.isRedeemed) {
        tierItem->setForeground(QBrush(redeemedTextColor));
        tierItem->setBackground(QBrush(redeemedBgColor));
    }

    m_positionsTable->setItem(row, COL_LOCK_TIER, tierItem);

    // Time Remaining - always show actual time remaining
    QString timeText;
    if (position.isPendingMint) {
        timeText = tr("Confirming");
    } else if (position.isPendingRedeem) {
        timeText = tr("Pending");
    } else if (position.isRedeemed) {
        timeText = tr("Redeemed");
    } else if (position.blocksRemaining <= 0) {
        timeText = tr("Unlocked");  // Show "Unlocked" for expired but not redeemed
    } else {
        timeText = formatBlockTime(position.blocksRemaining);  // Show actual time remaining
    }

    QTableWidgetItem* timeItem = new QTableWidgetItem(timeText);
    timeItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    timeItem->setTextAlignment(Qt::AlignCenter);

    // Apply styling based on status
    if (position.isPendingMint) {
        QString pendingColor = isDarkTheme ? "#ffb74d" : "#856404";
        QString pendingBg = isDarkTheme ? "#4a3a1f" : "#fff3cd";
        timeItem->setForeground(QBrush(QColor(pendingColor)));
        timeItem->setBackground(QBrush(QColor(pendingBg)));
        timeItem->setFont(QFont(timeItem->font().family(), timeItem->font().pointSize(), QFont::Bold));
        timeItem->setToolTip(tr("Mint transaction is pending confirmation; this vault is not redeemed"));
    } else if (position.isPendingRedeem) {
        QString pendingColor = isDarkTheme ? "#ffb74d" : "#856404";
        QString pendingBg = isDarkTheme ? "#4a3a1f" : "#fff3cd";
        timeItem->setForeground(QBrush(QColor(pendingColor)));
        timeItem->setBackground(QBrush(QColor(pendingBg)));
        timeItem->setFont(QFont(timeItem->font().family(), timeItem->font().pointSize(), QFont::Bold));
        timeItem->setToolTip(tr("Redemption is pending confirmation"));
    } else if (position.isRedeemed) {
        // Redeemed status - grayed out
        timeItem->setForeground(QBrush(redeemedTextColor));
        timeItem->setBackground(QBrush(redeemedBgColor));
        timeItem->setFont(QFont(timeItem->font().family(), timeItem->font().pointSize(), QFont::Bold));
        timeItem->setToolTip(tr("This vault has been redeemed"));
    } else if (position.blocksRemaining <= 0) {
        // Unlocked - ready to redeem (green success styling)
        QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
        QString successBg = isDarkTheme ? "#1e3d1e" : "#d4edda";

        timeItem->setForeground(QBrush(QColor(successColor)));
        timeItem->setBackground(QBrush(QColor(successBg)));
        timeItem->setFont(QFont(timeItem->font().family(), timeItem->font().pointSize(), QFont::Bold));
        timeItem->setToolTip(tr("This vault is unlocked and ready to be redeemed"));
    } else {
        // Still locked - normal styling with tooltip
        timeItem->setToolTip(tr("Time remaining: %1\nBlocks remaining: %2")
                            .arg(formatBlockTime(position.blocksRemaining))
                            .arg(position.blocksRemaining));
    }
    m_positionsTable->setItem(row, COL_TIME_REMAINING, timeItem);

    // Health Status (using a custom widget with progress bar)
    QWidget* healthWidget = createHealthWidget(position.health);

    // Apply redeemed styling to health widget background
    if (position.isRedeemed) {
        healthWidget->setStyleSheet(QString("background-color: %1;").arg(redeemedBgColor.name()));
    }

    m_positionsTable->setCellWidget(row, COL_HEALTH, healthWidget);

    // Actions (Redeem button)
    const bool isWatchOnly =
        m_walletModel ? m_walletModel->wallet().privateKeysDisabled() : false;
    const bool isWalletLocked =
        m_walletModel ? m_walletModel->getEncryptionStatus() == WalletModel::Locked : false;
    QPushButton* redeemButton = createRedeemButton(
        position.positionId, position.isPendingMint, position.isPendingRedeem, position.isRedeemed, position.canRedeem, isWatchOnly, isWalletLocked, position.blocksRemaining);
    m_positionsTable->setCellWidget(row, COL_ACTIONS, redeemButton);
}

QPushButton* DigiDollarPositionsWidget::createRedeemButton(const QString& positionId, bool isPendingMint, bool isPendingRedeem, bool isRedeemed, bool canRedeem, bool isWatchOnly, bool isWalletLocked, int blocksRemaining)
{
    // Set button text based on status (priority order):
    // - "Confirming" if the mint transaction is unconfirmed
    // - "Pending" if a redeem transaction is unconfirmed
    // - "Redeemed" if already redeemed (with strikethrough)
    // - "Watch-Only" if the wallet has private keys disabled and so cannot
    //   ever construct a redemption witness
    // - "Redeem" if can redeem now (clickable; a locked wallet is prompted for
    //   its passphrase by the redeem flow, so it stays clickable too)
    // - "Locked" if vault hasn't matured yet (grayed out)
    QString buttonText;
    if (isPendingMint) {
        buttonText = tr("Confirming");
    } else if (isPendingRedeem) {
        buttonText = tr("Pending");
    } else if (isRedeemed) {
        buttonText = tr("Redeemed");
    } else if (isWatchOnly) {
        buttonText = tr("Watch-Only");
    } else if (canRedeem) {
        buttonText = tr("Redeem");
    } else {
        buttonText = tr("Locked");
    }

    QPushButton* button = new QPushButton(buttonText, this);
    button->setProperty("positionId", positionId);
    button->setFixedSize(96, 28);

    // Apply enhanced theme-aware styling
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString buttonStyle;
    QString tooltip;

    if (isPendingMint) {
        QString pendingBg = isDarkTheme ? "#5a4520" : "#fff3cd";
        QString pendingText = isDarkTheme ? "#ffcf7a" : "#856404";
        buttonStyle = QString(
            "QPushButton { "
            "  background-color: %1; "
            "  color: %2; "
            "  border: 1px solid %2; "
            "  border-radius: 5px; "
            "  padding: 6px 8px; "
            "  font-weight: 600; "
            "  font-size: 10px; "
            "  min-width: 60px; "
            "}")
            .arg(pendingBg)
            .arg(pendingText);
        tooltip = tr("Mint transaction pending confirmation\nThis vault is not redeemed; it will become locked once the mint confirms.");
        button->setEnabled(false);
    } else if (isPendingRedeem) {
        QString pendingBg = isDarkTheme ? "#5a4520" : "#fff3cd";
        QString pendingText = isDarkTheme ? "#ffcf7a" : "#856404";
        buttonStyle = QString(
            "QPushButton { "
            "  background-color: %1; "
            "  color: %2; "
            "  border: 1px solid %2; "
            "  border-radius: 5px; "
            "  padding: 6px 8px; "
            "  font-weight: 600; "
            "  font-size: 10px; "
            "  min-width: 60px; "
            "}")
            .arg(pendingBg)
            .arg(pendingText);
        tooltip = tr("Redemption pending confirmation\nThis vault is not final until the redeem transaction confirms.");
        button->setEnabled(false);
    } else if (isRedeemed) {
        // Redeemed status - grayed out button with strikethrough
        QString redeemedBg = isDarkTheme ? "#555555" : "#cccccc";
        QString redeemedText = isDarkTheme ? "#999999" : "#888888";
        buttonStyle = QString(
            "QPushButton { "
            "  background-color: %1; "
            "  color: %2; "
            "  border: none; "
            "  border-radius: 5px; "
            "  padding: 6px 12px; "
            "  font-weight: 600; "
            "  font-size: 11px; "
            "  min-width: 60px; "
            "  text-decoration: line-through; "
            "}")
            .arg(redeemedBg)
            .arg(redeemedText);
        tooltip = tr("This vault has already been redeemed");
        button->setEnabled(false);
    } else if (isWatchOnly) {
        // Watch-Only - wallet has private keys disabled; redemption is
        // physically impossible from this wallet. Use a distinct grey-blue
        // styling so the badge is visually different from the timelock
        // "Locked" state above.
        QString woBg = isDarkTheme ? "#3a4a5a" : "#dde6ef";
        QString woText = isDarkTheme ? "#a0c0e0" : "#34495e";

        buttonStyle = QString(
            "QPushButton { "
            "  background-color: %1; "
            "  color: %2; "
            "  border: 1px solid %2; "
            "  border-radius: 5px; "
            "  padding: 6px 8px; "
            "  font-weight: 600; "
            "  font-size: 10px; "
            "  min-width: 60px; "
            "}")
            .arg(woBg)
            .arg(woText);
        tooltip = tr("Watch-only wallet\nThis wallet cannot sign DigiDollar redemptions because private keys are disabled.");
        button->setEnabled(false);
    } else if (canRedeem && isWalletLocked) {
        // Redeemable, but the wallet is locked: keep the action available and
        // use a distinct styling to flag the extra passphrase step.
        QString lockedWalletBg = isDarkTheme ? "#4a4655" : "#e4dfea";
        QString lockedWalletText = isDarkTheme ? "#d6c6e6" : "#4d3f5f";

        // Metrics match the plain redeemable branch below so that the same
        // "Redeem" label does not change size with the wallet lock state; only
        // the colour differs.
        buttonStyle = QString(
            "QPushButton { "
            "  background-color: %1; "
            "  color: %2; "
            "  border: 1px solid %2; "
            "  border-radius: 5px; "
            "  padding: 6px 12px; "
            "  font-weight: 600; "
            "  font-size: 11px; "
            "  min-width: 60px; "
            "}")
            .arg(lockedWalletBg)
            .arg(lockedWalletText);
        tooltip = tr("Wallet is locked\nYou will be asked for your passphrase before this DigiDollar vault is redeemed.");
        button->setEnabled(true);
    } else if (canRedeem) {
        // Can redeem - green button
        QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
        QString successHover = isDarkTheme ? "#5cbf60" : "#34ce57";
        QString successPressed = isDarkTheme ? "#449d48" : "#1e7e34";

        buttonStyle = QString(
            "QPushButton { "
            "  background-color: %1; "
            "  color: white; "
            "  border: none; "
            "  border-radius: 5px; "
            "  padding: 6px 12px; "
            "  font-weight: 600; "
            "  font-size: 11px; "
            "  min-width: 60px; "
            "} "
            "QPushButton:hover { "
            "  background-color: %2; "
            "  transform: translateY(-1px); "
            "} "
            "QPushButton:pressed { "
            "  background-color: %3; "
            "  transform: translateY(0px); "
            "}")
            .arg(successColor)
            .arg(successHover)
            .arg(successPressed);
        tooltip = tr("Click to redeem this DigiDollar position\nThis will return your DGB collateral and burn the $DD tokens");
        button->setEnabled(true);
    } else {
        // Locked - vault hasn't matured yet - grayed out button with dark text
        QString lockedBg = isDarkTheme ? "#555555" : "#cccccc";
        QString lockedText = isDarkTheme ? "#aaaaaa" : "#555555";  // Dark lettering

        buttonStyle = QString(
            "QPushButton { "
            "  background-color: %1; "
            "  color: %2; "
            "  border: none; "
            "  border-radius: 5px; "
            "  padding: 6px 12px; "
            "  font-weight: 600; "
            "  font-size: 11px; "
            "  min-width: 60px; "
            "}")
            .arg(lockedBg)
            .arg(lockedText);
        tooltip = tr("Vault is locked\nTime remaining: %1\nBlocks remaining: %2")
            .arg(formatBlockTime(blocksRemaining))
            .arg(blocksRemaining);
        button->setEnabled(false);
    }

    button->setStyleSheet(buttonStyle);
    button->setToolTip(tooltip);

    connect(button, &QPushButton::clicked,
            this, &DigiDollarPositionsWidget::onRedeemPositionClicked);

    return button;
}

QWidget* DigiDollarPositionsWidget::createHealthWidget(double health) const
{
    QWidget* widget = new QWidget();
    QHBoxLayout* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(0);

    const bool healthUnknown = health < 0.0;

    QProgressBar* healthBar = new QProgressBar(widget);
    healthBar->setRange(0, 500);  // Allow up to 500% collateralization
    healthBar->setValue(healthUnknown ? 0 : static_cast<int>(health));
    healthBar->setFormat(healthUnknown ? tr("N/A") : QString("%1%").arg(QString::number(health, 'f', 1)));
    healthBar->setFixedHeight(20);
    healthBar->setMinimumWidth(100);

    // Use system palette for theme-aware styling
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString borderColor = palette.color(QPalette::Mid).name();
    QString textColor = palette.color(QPalette::WindowText).name();
    QString healthBg = palette.color(QPalette::Base).name();

    // Determine health color and gradient (only custom colors for status indicators)
    // 100%+ = green (healthy, over-collateralized)
    // 80-99% = yellow (warning, under-collateralized)
    // <80% = red (at risk, severely under-collateralized)
    QString healthColor, gradientEnd;
    if (healthUnknown) {
        healthColor = borderColor;
        gradientEnd = borderColor;
    } else if (health >= 100) {
        healthColor = isDarkTheme ? "#4caf50" : "#28a745";
        gradientEnd = isDarkTheme ? "#66bb6a" : "#4caf50";
    } else if (health >= 80) {
        healthColor = isDarkTheme ? "#ff9800" : "#ffc107";
        gradientEnd = isDarkTheme ? "#ffb74d" : "#ff9800";
    } else {
        healthColor = isDarkTheme ? "#f44336" : "#dc3545";
        gradientEnd = isDarkTheme ? "#ef5350" : "#f44336";
    }

    // Apply professional health bar styling with gradient
    QString healthBarStyle = QString(
        "QProgressBar { "
        "  background-color: %1; "
        "  border: 1px solid %2; "
        "  border-radius: 5px; "
        "  text-align: center; "
        "  color: %3; "
        "  font-weight: bold; "
        "  font-size: 10px; "
        "  padding: 1px; "
        "} "
        "QProgressBar::chunk { "
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "    stop:0 %4, stop:0.5 %5, stop:1 %4); "
        "  border-radius: 4px; "
        "  margin: 1px; "
        "}")
        .arg(healthBg)
        .arg(borderColor)
        .arg(textColor)
        .arg(healthColor)
        .arg(gradientEnd);

    healthBar->setStyleSheet(healthBarStyle);

    // Add tooltip with health information
    QString healthStatus;
    if (healthUnknown) {
        healthStatus = tr("Oracle price unavailable");
    } else if (health >= 120) {
        healthStatus = tr("Healthy - Over-Collateralized");
    } else if (health >= 100) {
        healthStatus = tr("Adequate - At Required Ratio");
    } else if (health >= 80) {
        healthStatus = tr("Warning - Below Required Ratio");
    } else {
        healthStatus = tr("At Risk - Under-Collateralized");
    }

    if (healthUnknown) {
        healthBar->setToolTip(tr("Vault Health: N/A (Oracle price unavailable)\nHealth will update when a fresh oracle price is available."));
    } else {
        healthBar->setToolTip(tr("Vault Health: %1% (%2)\n100% = Required collateral ratio\nAbove 100% = Over-collateralized (safer)\nBelow 100% = Under-collateralized (at risk)")
                             .arg(QString::number(health, 'f', 1))
                             .arg(healthStatus));
    }

    layout->addWidget(healthBar);
    widget->setLayout(layout);

    return widget;
}

QString DigiDollarPositionsWidget::formatDDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $DD";
}

QString DigiDollarPositionsWidget::formatDGBAmount(double amount) const
{
    return QString::number(amount, 'f', 8) + " DGB";
}

QString DigiDollarPositionsWidget::formatBlockTime(int blocks) const
{
    if (blocks <= 0) return tr("Expired");

    // Estimate time remaining (15 seconds per block). Round up to avoid showing
    // "1h 0m" when the actual remaining block count is slightly above one hour
    // (for example the 100-block mint confirmation buffer).
    int totalSeconds = blocks * 15;
    int days = totalSeconds / (24 * 3600);
    int hours = (totalSeconds % (24 * 3600)) / 3600;
    int minutes = (totalSeconds % 3600) / 60;

    if (days > 0) {
        if (hours > 0) {
            return QString("%1d %2h").arg(days).arg(hours);
        }
        return QString("%1d").arg(days);
    } else if (hours > 0) {
        if (minutes > 0) {
            return QString("%1h %2m").arg(hours).arg(minutes);
        }
        return QString("%1h").arg(hours);
    } else {
        const int roundedMinutes = std::max(1, static_cast<int>(std::ceil(totalSeconds / 60.0)));
        return QString("%1m").arg(roundedMinutes);
    }
}

QString DigiDollarPositionsWidget::formatHealthStatus(double health) const
{
    if (health < 0.0) {
        return tr("N/A");
    }
    return QString("%1%").arg(QString::number(health, 'f', 1));
}

void DigiDollarPositionsWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    m_positionsTable->setVisible(!m_privacy);
    if (m_privacy) {
        m_statusLabel->setText(tr("Privacy mode activated for the $DD Vault tab. To unmask the values, uncheck Settings->Mask values."));
        m_statusLabel->show();
    } else {
        updatePositions();
    }
}

// REMOVED: applyTheme() - All styling now handled by CSS files (light.css/dark.css)
// This method was overriding the CSS theme with programmatic styling
void DigiDollarPositionsWidget::applyTheme()
{
    // Method disabled - CSS handles all theming now
}

// =============================================================================
// Backend Integration Helper Functions
// =============================================================================

CAmount DigiDollarPositionsWidget::GetMockOraclePrice() const
{
    // RegTest only: use MockOracleManager for testing
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        return MockOracleManager::GetInstance().GetCurrentPrice();
    }

    // Testnet/Mainnet: use real oracle price from bundle manager
    CAmount realPrice = OracleBundleManager::GetInstance().GetLatestPrice();
    if (realPrice > 0) return realPrice;

    // No oracle price available
    return 0;
}

std::vector<WalletCollateralPosition> DigiDollarPositionsWidget::GetWalletPositions() const
{
    std::vector<WalletCollateralPosition> positions;

    if (!m_walletModel) {
        return positions;
    }

    // Access DigiDollarWallet directly from wallet model
    DigiDollarWallet* ddWallet = m_walletModel->wallet().getDigiDollarWallet();
    if (ddWallet) {
        // ReconcilePositionStates() only reads the wallet's transactions and the
        // chain UTXO set and rewrites the is_active flag; it never touches key
        // material, so a locked wallet must still refresh. Skipping it there
        // would leave the row action enabled off a stale is_active flag.
        const bool walletIsWatchOnly = m_walletModel->wallet().privateKeysDisabled();
        if (!walletIsWatchOnly) {
            ddWallet->ReconcilePositionStates();
        }
        positions = ddWallet->GetDDTimeLocks(false); // Get ALL time locks including redeemed ones
    }

    return positions;
}

std::set<uint256> DigiDollarPositionsWidget::GetPendingRedeemPositionIds() const
{
    std::set<uint256> pendingPositions;

    if (!m_walletModel) {
        return pendingPositions;
    }

    try {
        for (const interfaces::WalletTx& wtx : m_walletModel->wallet().getWalletTxs()) {
            if (!wtx.tx || DigiDollar::GetDigiDollarTxType(*wtx.tx) != DigiDollar::DD_TX_REDEEM) {
                continue;
            }

            interfaces::WalletTxStatus status;
            interfaces::WalletOrderForm orderForm;
            bool inMempool = false;
            int numBlocks = 0;
            interfaces::WalletTx details = m_walletModel->wallet().getWalletTxDetails(
                wtx.tx->GetHash(), status, orderForm, inMempool, numBlocks);
            if (!details.tx) {
                continue;
            }
            if (!inMempool || status.is_abandoned || status.depth_in_main_chain != 0 || status.is_in_main_chain) {
                continue;
            }

            for (const CTxIn& txin : wtx.tx->vin) {
                if (txin.prevout.n == 0) {
                    pendingPositions.insert(txin.prevout.hash);
                }
            }
        }
    } catch (...) {
        pendingPositions.clear();
    }

    return pendingPositions;
}

double DigiDollarPositionsWidget::CalculatePositionHealth(CAmount ddAmount, CAmount dgbCollateral, CAmount oraclePrice) const
{
    if (ddAmount == 0) {
        return 0.0; // No DD = no health to display
    }

    if (oraclePrice <= 0 || dgbCollateral <= 0) {
        return -1.0;
    }

    // Calculate DGB collateral value in USD cents.
    // dgbCollateral is in satoshis and oraclePrice is micro-USD per DGB.

    // Step 1: Calculate DGB amount in whole coins
    double dgbAmount = dgbCollateral / 100000000.0;

    // Step 2: Convert micro-USD to cents per DGB.
    double centsPerDGB = static_cast<double>(oraclePrice) / 10000.0;

    // Step 3: Calculate value in cents
    CAmount collateralValueCents = static_cast<CAmount>(dgbAmount * centsPerDGB);

    // Health ratio = (Collateral Value / DD Value) * 100
    // ddAmount is already in cents
    double healthRatio = (static_cast<double>(collateralValueCents) * 100.0) / static_cast<double>(ddAmount);

    // Cap at 500% for display purposes (max collateral ratio)
    return std::min(healthRatio, 500.0);
}

int DigiDollarPositionsWidget::getLockTierBlocks(int tier) const
{
    // Lock periods in blocks (15 second blocks)
    // 1 hour = 240 blocks, 1 day = 5760 blocks, 1 month = 172800 blocks, 1 year = 2102400 blocks
    switch (tier) {
    case 0: return 240;         // 1 hour
    case 1: return 172800;      // 30 days
    case 2: return 518400;      // 3 months (90 days)
    case 3: return 1036800;     // 6 months (180 days)
    case 4: return 2102400;     // 1 year (365 days)
    case 5: return 4204800;     // 2 years (730 days)
    case 6: return 6307200;     // 3 years
    case 7: return 10512000;    // 5 years
    case 8: return 14716800;    // 7 years
    case 9: return 21024000;    // 10 years
    default: return 2102400;    // 1 year
    }
}
