// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollartransactionswidget.h>
#include <qt/digidollarstatus.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/transactiontablemodel.h>
#include <qt/guiutil.h>
#include <wallet/digidollarwallet.h>
#include <logging.h>
#include <univalue.h>

#include <algorithm>

#include <QHeaderView>
#include <QDateTime>
#include <QClipboard>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPointer>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QFile>
#include <QFileDialog>
#include <QTextStream>
#include <QTimer>

namespace {

QString DigiDollarTransactionDetailsDialogStyleSheet(bool dark_theme)
{
    if (dark_theme) {
        return QStringLiteral(
            "QDialog#DDTransactionDescDialog {"
            "  background-color: #0b2419;"
            "  color: #ffffff;"
            "}"
            "QDialog#DDTransactionDescDialog QTextEdit#detailText {"
            "  background-color: #113a29;"
            "  color: #ffffff;"
            "  border: 2px solid #42d884;"
            "  border-radius: 4px;"
            "  padding: 8px;"
            "  font-size: 11pt;"
            "  selection-background-color: #16804f;"
            "  selection-color: #ffffff;"
            "}"
            "QDialog#DDTransactionDescDialog QLabel {"
            "  color: #ffffff;"
            "}"
            "QDialog#DDTransactionDescDialog QDialogButtonBox {"
            "  background-color: transparent;"
            "}"
            "QDialog#DDTransactionDescDialog QPushButton {"
            "  background-color: #16804f;"
            "  color: #ffffff;"
            "  border: 2px solid #16804f;"
            "  border-radius: 4px;"
            "  padding: 8px 16px;"
            "  font-weight: bold;"
            "}"
            "QDialog#DDTransactionDescDialog QPushButton:hover {"
            "  background-color: #21a866;"
            "  border-color: #21a866;"
            "  color: #ffffff;"
            "}"
            "QDialog#DDTransactionDescDialog QPushButton:pressed {"
            "  background-color: #0f633c;"
            "  border-color: #0f633c;"
            "}"
            "QDialog#DDTransactionDescDialog QScrollBar:vertical,"
            "QDialog#DDTransactionDescDialog QScrollBar:horizontal {"
            "  background-color: #0b2419;"
            "  border: 1px solid #42d884;"
            "}"
            "QDialog#DDTransactionDescDialog QScrollBar::handle:vertical,"
            "QDialog#DDTransactionDescDialog QScrollBar::handle:horizontal {"
            "  background-color: #16804f;"
            "  border-radius: 3px;"
            "}");
    }

    return QStringLiteral(
        "QDialog#DDTransactionDescDialog {"
        "  background-color: #eef9f2;"
        "  color: #123f2b;"
        "}"
        "QDialog#DDTransactionDescDialog QTextEdit#detailText {"
        "  background-color: #ffffff;"
        "  color: #123f2b;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 8px;"
        "  font-size: 11pt;"
        "  selection-background-color: #1f9d57;"
        "  selection-color: #ffffff;"
        "}"
        "QDialog#DDTransactionDescDialog QLabel {"
        "  color: #123f2b;"
        "  font-weight: bold;"
        "}"
        "QDialog#DDTransactionDescDialog QDialogButtonBox {"
        "  background-color: transparent;"
        "}"
        "QDialog#DDTransactionDescDialog QPushButton {"
        "  background-color: #1f9d57;"
        "  color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 8px 16px;"
        "  font-weight: bold;"
        "}"
        "QDialog#DDTransactionDescDialog QPushButton:hover {"
        "  background-color: #26b96a;"
        "  border-color: #26b96a;"
        "  color: #ffffff;"
        "}"
        "QDialog#DDTransactionDescDialog QPushButton:pressed {"
        "  background-color: #147a42;"
        "  border-color: #147a42;"
        "}"
        "QDialog#DDTransactionDescDialog QScrollBar:vertical,"
        "QDialog#DDTransactionDescDialog QScrollBar:horizontal {"
        "  background-color: #eef9f2;"
        "  border: 1px solid #1f9d57;"
        "}"
        "QDialog#DDTransactionDescDialog QScrollBar::handle:vertical,"
        "QDialog#DDTransactionDescDialog QScrollBar::handle:horizontal {"
        "  background-color: #1f9d57;"
        "  border-radius: 3px;"
        "}");
}

QString DigiDollarTransactionDetailsDocumentStyleSheet(bool dark_theme)
{
    return dark_theme
        ? QStringLiteral("body { background-color: transparent; color: #ffffff; } b { color: #ffffff; }")
        : QStringLiteral("body { background-color: transparent; color: #123f2b; } b { color: #123f2b; }");
}

class DigiDollarTransactionDetailsDialog final : public QDialog
{
public:
    DigiDollarTransactionDetailsDialog(const QString& txid, const QString& details_html, bool dark_theme, QWidget* parent)
        : QDialog(parent, GUIUtil::dialog_flags)
    {
        setObjectName(QStringLiteral("DDTransactionDescDialog"));
        setWindowTitle(QObject::tr("Details for %1").arg(txid));
        resize(620, 250);
        setStyleSheet(DigiDollarTransactionDetailsDialogStyleSheet(dark_theme));

        QVBoxLayout* layout = new QVBoxLayout(this);
        QTextEdit* detailText = new QTextEdit(this);
        detailText->setObjectName(QStringLiteral("detailText"));
        detailText->setToolTip(QObject::tr("This pane shows a detailed description of the transaction"));
        detailText->setReadOnly(true);
        detailText->document()->setDefaultStyleSheet(DigiDollarTransactionDetailsDocumentStyleSheet(dark_theme));
        detailText->setHtml(details_html);

        QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);

        layout->addWidget(detailText);
        layout->addWidget(buttonBox);
        setLayout(layout);
        GUIUtil::handleCloseWindowShortcut(this);
    }
};

class DigiDollarTimestampTableItem final : public QTableWidgetItem
{
public:
    DigiDollarTimestampTableItem(const QString& display_text, uint64_t timestamp)
        : QTableWidgetItem(display_text)
    {
        setData(Qt::UserRole, QVariant::fromValue(timestamp));
    }

    bool operator<(const QTableWidgetItem& other) const override
    {
        // The visible date is localized for the operator (for example,
        // "Aug 02, 2026 23:30") and therefore cannot be sorted as text.
        // Compare the underlying Unix timestamps so both sort directions stay
        // chronological across hours, days, months, locales, and year changes.
        return data(Qt::UserRole).toULongLong() <
               other.data(Qt::UserRole).toULongLong();
    }
};

QString DetailRow(const QString& label, const QString& value)
{
    if (value.isEmpty()) return QString();
    return QStringLiteral("<b>%1:</b> %2<br>")
        .arg(GUIUtil::HtmlEscape(label), GUIUtil::HtmlEscape(value));
}

} // namespace

DigiDollarTransactionsWidget::DigiDollarTransactionsWidget(QWidget* parent)
    : QWidget(parent)
    , m_mainLayout(nullptr)
    , m_filterLayout(nullptr)
    , m_typeFilter(nullptr)
    , m_searchEdit(nullptr)
    , m_exportButton(nullptr)
    , m_table(nullptr)
    , m_statusLabel(nullptr)
    , m_contextMenu(nullptr)
    , m_walletModel(nullptr)
    , m_clientModel(nullptr)
{
    setupUI();
    connectSignals();
}

DigiDollarTransactionsWidget::~DigiDollarTransactionsWidget()
{
}

void DigiDollarTransactionsWidget::setupUI()
{
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    setupFilterBar();
    setupTable();

    // Status label
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("transactionsStatusLabel"));
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::INFO);
    m_mainLayout->addWidget(m_statusLabel);

    setLayout(m_mainLayout);
}

void DigiDollarTransactionsWidget::setupFilterBar()
{
    m_filterLayout = new QHBoxLayout();
    m_filterLayout->setSpacing(8);

    // Type filter
    QLabel* typeLabel = new QLabel(tr("Type:"), this);
    m_typeFilter = new QComboBox(this);
    m_typeFilter->addItem(tr("All Types"), "");
    m_typeFilter->addItem(tr("Mints"), "mint");
    m_typeFilter->addItem(tr("Sends"), "send");
    m_typeFilter->addItem(tr("Receives"), "receive");
    m_typeFilter->addItem(tr("Redemptions"), "redeem");
    m_typeFilter->addItem(DigiDollarLabels::ChangeReturned(), "redeem_change");
    m_typeFilter->setItemData(m_typeFilter->count() - 1,
                              DigiDollarLabels::ChangeReturnedExplanation(),
                              Qt::ToolTipRole);

    // Search box
    QLabel* searchLabel = new QLabel(tr("Search:"), this);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("TX ID or address..."));
    m_searchEdit->setMinimumWidth(200);

    m_exportButton = new QPushButton(tr("Export"), this);
    m_exportButton->setToolTip(tr("Export the data in the current tab to a file"));

    m_filterLayout->addWidget(typeLabel);
    m_filterLayout->addWidget(m_typeFilter);
    m_filterLayout->addSpacing(20);
    m_filterLayout->addWidget(searchLabel);
    m_filterLayout->addWidget(m_searchEdit);
    m_filterLayout->addStretch();
    m_filterLayout->addWidget(m_exportButton);

    m_mainLayout->addLayout(m_filterLayout);
}

void DigiDollarTransactionsWidget::setupTable()
{
    m_table = new QTableWidget(this);
    m_table->setColumnCount(Column::ColumnCount);
    m_table->setHorizontalHeaderLabels({
        tr("Date"),
        tr("Type"),
        tr("Amount ($DD)"),
        tr("Lock Period"),
        tr("Note"),
        tr("Transaction ID"),
        tr("Confirmations")
    });

    // Table settings - match TransactionView styling
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSortingEnabled(true);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionsClickable(true);

    // Scroll bar settings - show vertical scroll when needed
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Let the table inherit colors from the application palette/theme
    // Don't override with custom colors - this ensures proper dark/light mode support

    m_table->setColumnWidth(Column::Date, 130);
    // The type column has to fit the longest name a row can carry, which is
    // the name for DigiDollars a redemption hands back.
    m_table->setColumnWidth(Column::Type,
                            std::max(90, m_table->fontMetrics().horizontalAdvance(
                                             DigiDollarLabels::ChangeReturned()) + 24));
    m_table->setColumnWidth(Column::Amount, 110);
    m_table->setColumnWidth(Column::LockPeriod, 90);
    m_table->setColumnWidth(Column::Note, 150);
    m_table->setColumnWidth(Column::Confirmations, 100);

    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(Column::TxId, QHeaderView::Stretch);

    m_contextMenu = new QMenu(this);
    m_contextMenu->addAction(tr("Copy TX ID"), this, &DigiDollarTransactionsWidget::copyTxId);
    m_contextMenu->addAction(tr("Copy Amount"), this, &DigiDollarTransactionsWidget::copyAmount);
    m_contextMenu->addAction(tr("Copy Note"), this, &DigiDollarTransactionsWidget::copyNote);
    m_contextMenu->addSeparator();
    m_contextMenu->addAction(tr("Show Details"), this, &DigiDollarTransactionsWidget::showDetails);

    m_mainLayout->addWidget(m_table, 1);
}

void DigiDollarTransactionsWidget::connectSignals()
{
    connect(m_typeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DigiDollarTransactionsWidget::onTypeFilterChanged);
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &DigiDollarTransactionsWidget::onSearchTextChanged);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &DigiDollarTransactionsWidget::showContextMenu);
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        if (!item) return;
        m_table->setCurrentItem(item);
        showDetails();
    });
    connect(m_table, &QTableWidget::itemActivated, this, [this](QTableWidgetItem* item) {
        if (!item) return;
        m_table->setCurrentItem(item);
        showDetails();
    });
    connect(m_exportButton, &QPushButton::clicked,
            this, &DigiDollarTransactionsWidget::exportClicked);

    // Auto-refresh timer to update confirmation counts (matches DD Overview behavior)
    QTimer* refreshTimer = new QTimer(this);
    connect(refreshTimer, &QTimer::timeout, this, &DigiDollarTransactionsWidget::updateTransactions);
    refreshTimer->start(5000); // 5 seconds - same interval as DD Overview
}

void DigiDollarTransactionsWidget::setWalletModel(WalletModel* model)
{
    if (m_walletModel == model) {
        updateTransactions();
        return;
    }
    if (m_walletModel) disconnect(m_walletModel, nullptr, this, nullptr);

    m_walletModel = model;
    m_initialSnapshotShown = false;
    m_refreshInFlight = false;
    m_refreshPending = false;
    m_cachedTransactions = UniValue{UniValue::VARR};
    m_lastTransactionsFingerprint.clear();
    m_pendingFocusTxid.clear();
    m_table->setRowCount(0);

    if (m_walletModel) {
        connect(m_walletModel, &WalletModel::digiDollarChanged,
                this, &DigiDollarTransactionsWidget::updateTransactions);
        updateTransactions();
    } else {
        DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::INFO);
        m_statusLabel->setText(tr("ℹ No wallet is loaded. Select a wallet to view its DigiDollar transactions."));
        m_statusLabel->setVisible(true);
    }
}

void DigiDollarTransactionsWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;
}

void DigiDollarTransactionsWidget::updateView()
{
    updateTransactions();
}

void DigiDollarTransactionsWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    m_table->setVisible(!m_privacy);
    if (m_privacy) {
        DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::INFO);
        m_statusLabel->setText(tr("ℹ Values are hidden by privacy mode. Disable Settings → Mask values to show them."));
        m_statusLabel->setVisible(true);
    } else {
        updateTransactions();
    }
}

void DigiDollarTransactionsWidget::focusTransaction(const QString& txid)
{
    if (!m_table || txid.isEmpty() || m_privacy) {
        return;
    }

    m_pendingFocusTxid = txid;
    if (!selectTransaction(txid)) updateTransactions();
}

void DigiDollarTransactionsWidget::updateTransactions()
{
    if (!isVisible()) return;
    if (m_privacy) return;
    if (!m_walletModel) {
        DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::INFO);
        m_statusLabel->setText(tr("ℹ No wallet is loaded. Select a wallet to view its DigiDollar transactions."));
        m_statusLabel->setVisible(true);
        return;
    }

    // Paint the newest rows from WalletDB before starting the canonical
    // ownership/confirmation reconstruction. This snapshot never waits for
    // cs_wallet, so a user who opens the page immediately after startup sees
    // useful history instead of an empty table for several seconds.
    if (!m_initialSnapshotShown) {
        UniValue initial = m_walletModel->getCachedDigiDollarTransactionHistory(50, 0);
        if (initial.isArray() && !initial.empty()) {
            m_initialSnapshotShown = true;
            m_cachedTransactions = std::move(initial);
            m_lastTransactionsFingerprint =
                QString::fromStdString(m_cachedTransactions.write());
            populateTable();
        }
    }

    if (m_refreshInFlight) {
        m_refreshPending = true;
        return;
    }

    m_refreshInFlight = true;
    if (m_cachedTransactions.empty()) {
        DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::WAITING);
        m_statusLabel->setText(tr("… Loading DigiDollar transactions…"));
        m_statusLabel->setVisible(true);
    }

    WalletModel* const requested_model = m_walletModel;
    QPointer<DigiDollarTransactionsWidget> guard{this};
    m_walletModel->getDigiDollarTransactionHistoryAsync(
        1000, 0,
        [guard, requested_model](UniValue result, QString error) mutable {
            if (!guard || guard->m_walletModel != requested_model) return;
            guard->m_refreshInFlight = false;

            if (!error.isEmpty()) {
                // Never retry a failed asynchronous history request by reading
                // the wallet synchronously on Qt's event thread. Preserve the
                // last good table and let a later refresh retry the snapshot.
                LogPrintf("DigiDollar Transactions: asynchronous history error - %s\n",
                          error.toStdString());
                DigiDollarStatus::SetBanner(
                    guard->m_statusLabel,
                    guard->m_table->rowCount() > 0
                        ? DigiDollarStatus::Kind::WAITING
                        : DigiDollarStatus::Kind::ERR);
                guard->m_statusLabel->setText(
                    guard->m_table->rowCount() > 0
                        ? tr("… Saved transactions are shown. Checking for updates will retry automatically.")
                        : tr("✕ Transaction history could not be loaded. It will be retried automatically."));
                guard->m_statusLabel->setVisible(true);
            }

            if (result.isArray()) {
                const QString fingerprint = QString::fromStdString(result.write());
                const bool changed = fingerprint != guard->m_lastTransactionsFingerprint;
                guard->m_cachedTransactions = std::move(result);
                guard->m_lastTransactionsFingerprint = fingerprint;
                if (changed || guard->m_table->rowCount() == 0) guard->populateTable();
                if (!guard->m_pendingFocusTxid.isEmpty() &&
                    guard->selectTransaction(guard->m_pendingFocusTxid)) {
                    guard->m_pendingFocusTxid.clear();
                }
            }

            if (guard->m_refreshPending) {
                guard->m_refreshPending = false;
                QTimer::singleShot(0, guard, &DigiDollarTransactionsWidget::updateTransactions);
            }
        });
}

bool DigiDollarTransactionsWidget::selectTransaction(const QString& txid)
{
    if (m_table->selectionModel()) m_table->selectionModel()->clearSelection();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* item = m_table->item(row, Column::TxId);
        if (!item || item->data(Qt::UserRole).toString() != txid) continue;
        m_table->setCurrentCell(row, Column::TxId);
        m_table->selectRow(row);
        m_table->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        m_table->setFocus();
        return true;
    }
    return false;
}

void DigiDollarTransactionsWidget::populateTable()
{
    const bool restoreUserSort = m_hasAppliedDefaultSort;
    const int sortColumn = m_table->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = m_table->horizontalHeader()->sortIndicatorOrder();
    const auto applySort = [&] {
        m_table->setSortingEnabled(true);
        if (restoreUserSort && sortColumn >= 0 && sortColumn < Column::ColumnCount) {
            m_table->sortByColumn(sortColumn, sortOrder);
        } else {
            m_table->sortByColumn(Column::Date, Qt::DescendingOrder);
            m_hasAppliedDefaultSort = true;
        }
    };

    m_table->setRowCount(0);
    m_table->setSortingEnabled(false);

    try {
        const UniValue& result = m_cachedTransactions;

        if (!result.isArray()) {
            DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::INFO);
            m_statusLabel->setText(tr("ℹ No DigiDollar transactions were found in this wallet."));
            m_statusLabel->setVisible(true);
            applySort();
            return;
        }

        QString typeFilter = m_typeFilter->currentData().toString();
        QString searchText = m_searchEdit->text().toLower();

        int row = 0;
        for (size_t i = 0; i < result.size(); ++i) {
            const UniValue& tx = result[i];

            QString category = QString::fromStdString(tx.find_value("category").get_str());
            QString txid = QString::fromStdString(tx.find_value("txid").get_str());

            // Apply filters
            if (!typeFilter.isEmpty() && category != typeFilter) {
                continue;
            }
            if (!searchText.isEmpty() && !txid.toLower().contains(searchText)) {
                continue;
            }

            m_table->insertRow(row);

            // Date
            uint64_t timestamp = tx.find_value("time").getInt<uint64_t>();
            QTableWidgetItem* dateItem =
                new DigiDollarTimestampTableItem(formatTimestamp(timestamp), timestamp);
            m_table->setItem(row, Column::Date, dateItem);

            // Get lock tier early so we can use it for Type column
            int lockTier = -1;
            if (tx.exists("lock_tier")) {
                lockTier = tx.find_value("lock_tier").getInt<int>();
            }

            // Type with lock period for mints/redeems
            QString typeText = category.left(1).toUpper() + category.mid(1);
            QString typeTooltip;
            if (category == "redeem_change") {
                typeText = DigiDollarLabels::ChangeReturned();
                typeTooltip = DigiDollarLabels::ChangeReturnedExplanation();
            }
            if ((category == "mint" || category == "redeem") && lockTier >= 0) {
                QString lockPeriodShort = formatLockPeriodShort(lockTier);
                if (!lockPeriodShort.isEmpty()) {
                    typeText += " " + lockPeriodShort;
                }
            }
            QTableWidgetItem* typeItem = new QTableWidgetItem(typeText);
            if (!typeTooltip.isEmpty()) {
                typeItem->setToolTip(typeTooltip);
            }
            m_table->setItem(row, Column::Type, typeItem);

            // Amount
            CAmount amount = tx.find_value("amount").getInt<int64_t>();
            QTableWidgetItem* amountItem = new QTableWidgetItem(formatDDAmount(amount));
            amountItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            amountItem->setData(Qt::UserRole, QVariant::fromValue(amount));

            // Color code: green for receives/mints, red for sends/redeems
            // Use theme-appropriate colors that work in both light and dark mode
            bool isPositive = (category == "receive" || category == "mint" || category == "redeem_change");
            amountItem->setForeground(getAmountColor(isPositive));
            m_table->setItem(row, Column::Amount, amountItem);

            QTableWidgetItem* lockItem = new QTableWidgetItem(formatLockPeriod(lockTier));
            lockItem->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, Column::LockPeriod, lockItem);

            QString noteText;
            const UniValue& commentVal = tx.find_value("comment");
            if (commentVal.isStr()) {
                noteText = QString::fromStdString(commentVal.get_str());
            }
            QTableWidgetItem* noteItem = new QTableWidgetItem(noteText);
            noteItem->setToolTip(noteText.isEmpty() ? tr("No note") : noteText);
            m_table->setItem(row, Column::Note, noteItem);

            QString displayTxid = txid.left(16) + "..." + txid.right(8);
            QTableWidgetItem* txidItem = new QTableWidgetItem(displayTxid);
            txidItem->setData(Qt::UserRole, txid);  // Store full txid
            txidItem->setToolTip(txid);
            m_table->setItem(row, Column::TxId, txidItem);

            // Confirmations - also check if transaction is abandoned
            int confirmations = tx.find_value("confirmations").getInt<int>();
            bool isAbandoned = false;
            // Check abandoned flag directly from transaction (from listdigidollartxs RPC)
            const UniValue& abandonedVal = tx.find_value("abandoned");
            if (abandonedVal.isBool()) {
                isAbandoned = abandonedVal.get_bool();
            }
            bool isLocal = false;
            const UniValue& walletStateVal = tx.find_value("wallet_state");
            if (walletStateVal.isStr()) {
                isLocal = walletStateVal.get_str() == "local";
            }
            QTableWidgetItem* confItem = new QTableWidgetItem(formatConfirmations(confirmations, isAbandoned, isLocal));
            confItem->setTextAlignment(Qt::AlignCenter);
            if (isLocal) {
                confItem->setToolTip(tr("Created locally but not currently in mempool. It may need rebroadcast or may have been rejected."));
            }
            m_table->setItem(row, Column::Confirmations, confItem);

            ++row;
        }

        m_statusLabel->setVisible(row == 0);
        if (row == 0) {
            DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::INFO);
            m_statusLabel->setText(tr("ℹ No transactions match the current filters."));
        }

    } catch (const UniValue& e) {
        LogPrintf("DigiDollar Transactions: RPC error - %s\n", e.write());
        DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::WAITING);
        m_statusLabel->setText(tr("… Transaction history is temporarily unavailable; retrying automatically."));
        m_statusLabel->setVisible(true);
    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Transactions: Error loading transactions - %s\n", e.what());
        DigiDollarStatus::SetBanner(m_statusLabel, DigiDollarStatus::Kind::ERR);
        m_statusLabel->setText(tr("✕ Transaction history could not be loaded."));
        m_statusLabel->setVisible(true);
    }

    applySort();
}

void DigiDollarTransactionsWidget::onTypeFilterChanged(int /*index*/)
{
    populateTable();
}

void DigiDollarTransactionsWidget::onSearchTextChanged()
{
    populateTable();
}

void DigiDollarTransactionsWidget::showContextMenu(const QPoint& pos)
{
    if (m_table->currentRow() >= 0) {
        m_contextMenu->popup(m_table->viewport()->mapToGlobal(pos));
    }
}

void DigiDollarTransactionsWidget::copyTxId()
{
    int row = m_table->currentRow();
    if (row >= 0) {
        QTableWidgetItem* item = m_table->item(row, Column::TxId);
        if (item) {
            QString txid = item->data(Qt::UserRole).toString();
            QApplication::clipboard()->setText(txid);
        }
    }
}

void DigiDollarTransactionsWidget::copyAmount()
{
    int row = m_table->currentRow();
    if (row >= 0) {
        QTableWidgetItem* item = m_table->item(row, Column::Amount);
        if (item) {
            QApplication::clipboard()->setText(item->text());
        }
    }
}

void DigiDollarTransactionsWidget::copyNote()
{
    int row = m_table->currentRow();
    if (row >= 0) {
        QTableWidgetItem* item = m_table->item(row, Column::Note);
        if (item) {
            QApplication::clipboard()->setText(item->text());
        }
    }
}

void DigiDollarTransactionsWidget::showDetails()
{
    int row = m_table->currentRow();
    if (row >= 0) {
        QTableWidgetItem* txidItem = m_table->item(row, Column::TxId);
        QTableWidgetItem* typeItem = m_table->item(row, Column::Type);
        QTableWidgetItem* amountItem = m_table->item(row, Column::Amount);
        QTableWidgetItem* dateItem = m_table->item(row, Column::Date);
        QTableWidgetItem* confItem = m_table->item(row, Column::Confirmations);
        QTableWidgetItem* noteItem = m_table->item(row, Column::Note);
        QTableWidgetItem* lockItem = m_table->item(row, Column::LockPeriod);

        const QString txid = txidItem ? txidItem->data(Qt::UserRole).toString() : QString();
        if (txid.isEmpty()) {
            return;
        }

        for (QDialog* dialog : m_openedDialogs) {
            if (!dialog || dialog->property("ddTxid").toString() != txid) continue;
            if (dialog->isMinimized()) {
                dialog->showNormal();
            }
            dialog->raise();
            dialog->activateWindow();
            return;
        }

        const QString noteText = noteItem ? noteItem->text() : QString();
        const QString lockText = lockItem ? lockItem->text() : QString();
        QString details;
        details += QStringLiteral("<html><body>");
        details += DetailRow(tr("Confirmations"), confItem ? confItem->text() : QString());
        details += DetailRow(tr("Date"), dateItem ? dateItem->text() : QString());
        details += DetailRow(tr("Type"), typeItem ? typeItem->text() : QString());
        details += DetailRow(tr("Amount"), amountItem ? amountItem->text() : QString());
        if (!lockText.isEmpty() && lockText != QStringLiteral("-")) {
            details += DetailRow(tr("Lock period"), lockText);
        }
        if (!noteText.isEmpty()) {
            details += DetailRow(tr("Note"), noteText);
        }
        details += DetailRow(tr("Transaction ID"), txid);
        details += QStringLiteral("</body></html>");

        DigiDollarTransactionDetailsDialog* dlg = new DigiDollarTransactionDetailsDialog(txid, details, isDarkTheme(), this);
        dlg->setProperty("ddTxid", txid);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        m_openedDialogs.append(dlg);
        connect(dlg, &QObject::destroyed, [this, dlg] {
            m_openedDialogs.removeOne(dlg);
        });
        dlg->show();
    }
}

bool DigiDollarTransactionsWidget::isDarkTheme() const
{
    if (!m_walletModel || !m_walletModel->getOptionsModel()) {
        // Fallback: detect from window palette
        int lightness = window()->palette().window().color().lightness();
        return lightness < 128;  // Dark if lightness < 128
    }

    QString currentTheme = m_walletModel->getOptionsModel()->data(
        m_walletModel->getOptionsModel()->index(OptionsModel::Theme),
        Qt::EditRole).toString();
    return (currentTheme == "dark");
}

QColor DigiDollarTransactionsWidget::getAmountColor(bool isPositive) const
{
    // Use colors consistent with TransactionTableModel
    // These colors are designed to be readable in both light and dark themes
    if (isDarkTheme()) {
        // Dark theme: brighter colors for visibility
        return isPositive ? QColor(100, 255, 100) : QColor(255, 70, 70);
    } else {
        // Light theme: darker colors
        return isPositive ? QColor(0, 150, 0) : QColor(200, 0, 0);
    }
}

QString DigiDollarTransactionsWidget::formatDDAmount(CAmount amount) const
{
    // Amount is in cents, convert to DD with 2 decimal places
    const CAmount absAmount = amount < 0 ? -amount : amount;
    const QString prefix = amount > 0 ? "+" : (amount < 0 ? "-" : "");
    return prefix + QString::number(absAmount / 100.0, 'f', 2) + " $DD";
}

QString DigiDollarTransactionsWidget::formatTimestamp(uint64_t timestamp) const
{
    QDateTime dt = QDateTime::fromSecsSinceEpoch(timestamp);
    return dt.toString("MMM dd, yyyy hh:mm");
}

QString DigiDollarTransactionsWidget::formatConfirmations(int confirmations, bool isAbandoned, bool isLocal) const
{
    if (isAbandoned) {
        return tr("Abandoned");
    }
    if (confirmations < 0) {
        return tr("Conflicted");
    }
    if (confirmations == 0) {
        if (isLocal) {
            return tr("Local");
        }
        return tr("Pending");
    }
    // Once a transaction is in a block this column shows how many blocks deep
    // it is and nothing else, so every row in the column reads the same way.
    // A word is used only where there is no count to show.
    return QString::number(confirmations);
}

QString DigiDollarTransactionsWidget::confirmationsTextForTesting(int confirmations, bool isAbandoned, bool isLocal) const
{
    return formatConfirmations(confirmations, isAbandoned, isLocal);
}

QString DigiDollarTransactionsWidget::formatLockPeriod(int lockTier) const
{
    // Tier mappings: 0-9 (10 tiers total) - matches consensus/digidollar.h
    // 0=1h, 1=30d, 2=3mo, 3=6mo, 4=1y, 5=2y, 6=3y, 7=5y, 8=7y, 9=10y
    switch (lockTier) {
        case 0:  return tr("1 hour");
        case 1:  return tr("30 days");
        case 2:  return tr("3 months");
        case 3:  return tr("6 months");
        case 4:  return tr("1 year");
        case 5:  return tr("2 years");
        case 6:  return tr("3 years");
        case 7:  return tr("5 years");
        case 8:  return tr("7 years");
        case 9:  return tr("10 years");
        default: return QString("-");  // Non-mint or unknown
    }
}

QString DigiDollarTransactionsWidget::formatLockPeriodShort(int lockTier) const
{
    switch (lockTier) {
        case 0:  return tr("1-hr");
        case 1:  return tr("30-day");
        case 2:  return tr("3-mo");
        case 3:  return tr("6-mo");
        case 4:  return tr("1-yr");
        case 5:  return tr("2-yr");
        case 6:  return tr("3-yr");
        case 7:  return tr("5-yr");
        case 8:  return tr("7-yr");
        case 9:  return tr("10-yr");
        default: return QString("");
    }
}

void DigiDollarTransactionsWidget::exportClicked()
{
    QString filename = QFileDialog::getSaveFileName(this,
        tr("Export DigiDollar Transaction History"),
        QString(),
        tr("Comma separated file") + QLatin1String(" (*.csv)"));

    if (filename.isEmpty()) return;

    if (!filename.endsWith(".csv", Qt::CaseInsensitive)) {
        filename += ".csv";
    }

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Export Failed"),
            tr("Could not open file %1 for writing.").arg(filename));
        return;
    }

    QTextStream out(&file);

    QStringList headers;
    headers << "Date" << "Type" << "Amount" << "Lock Period" << "Note" << "Transaction ID" << "Confirmations";
    out << "\"" << headers.join("\",\"") << "\"\n";

    for (int row = 0; row < m_table->rowCount(); ++row) {
        QStringList rowData;
        for (int col = 0; col < Column::ColumnCount; ++col) {
            QTableWidgetItem* item = m_table->item(row, col);
            QString value;
            if (item) {
                if (col == Column::TxId) {
                    value = item->data(Qt::UserRole).toString();
                } else {
                    value = item->text();
                }
            }
            value.replace("\"", "\"\"");
            rowData << value;
        }
        out << "\"" << rowData.join("\",\"") << "\"\n";
    }

    file.close();

    if (file.error() == QFile::NoError) {
        QMessageBox::information(this, tr("Export Successful"),
            tr("Transaction history was successfully saved to %1.").arg(filename));
    } else {
        QMessageBox::critical(this, tr("Export Failed"),
            tr("Error writing to file %1.").arg(filename));
    }
}
