// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarreceivewidget.h>

#include <base58.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/qrimagewidget.h>
#include <qt/sendcoinsrecipient.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/digidollarreceiverequest.h>
#include <consensus/amount.h>
#include <logging.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <wallet/types.h>

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QToolButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QClipboard>
#include <QApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QDateTime>
#include <QLocale>
#include <QPalette>
#include <QUrl>
#include <QStandardPaths>
#include <QMenu>
#include <QAction>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>

namespace {
constexpr CAmount MAX_DD_REQUEST_AMOUNT_CENTS = 100000000LL * 100;

bool ParseDigiDollarRequestAmount(const QString& amount_text, CAmount& amount_out)
{
    amount_out = 0;
    if (amount_text.isEmpty()) {
        return true;
    }

    const QString trimmed = amount_text.trimmed();
    if (trimmed != amount_text || trimmed.isEmpty()) {
        return false;
    }

    int64_t parsed = 0;
    if (!ParseFixedPoint(trimmed.toStdString(), 2, &parsed)) {
        return false;
    }
    if (parsed < 0 || parsed > MAX_DD_REQUEST_AMOUNT_CENTS) {
        return false;
    }

    amount_out = static_cast<CAmount>(parsed);
    return true;
}

QString FormatDigiDollarRequestAmount(CAmount amount)
{
    return QString::number(static_cast<double>(amount) / 100.0, 'f', 2);
}

bool IsCurrentNetworkDigiDollarAddress(const QString& address)
{
    return CDigiDollarAddress::IsValidDigiDollarAddressForCurrentNetwork(address.toStdString());
}
} // namespace

DigiDollarReceiveWidget::DigiDollarReceiveWidget(QWidget *parent) :
    QWidget(parent),
    m_contextMenu(nullptr),
    m_mainLayout(nullptr),
    m_generateFrame(nullptr),
    m_generateLayout(nullptr),
    m_labelLabel(nullptr),
    m_labelEdit(nullptr),
    m_amountLabel(nullptr),
    m_amountEdit(nullptr),
    m_messageLabel(nullptr),
    m_messageEdit(nullptr),
    m_generateButton(nullptr),
    m_clearButton(nullptr),
    m_qrFrame(nullptr),
    m_qrLayout(nullptr),
    m_emptyStateLabel(nullptr),
    m_qrTitle(nullptr),
    m_qrImage(nullptr),
    m_addressLabel(nullptr),
    m_addressEdit(nullptr),
    m_qrButtonLayout(nullptr),
    m_copyAddressButton(nullptr),
    m_copyQRButton(nullptr),
    m_saveQRButton(nullptr),
    m_requestsFrame(nullptr),
    m_requestsLayout(nullptr),
    m_requestsTitle(nullptr),
    m_requestsTable(nullptr),
    m_requestsButtonLayout(nullptr),
    m_showRequestButton(nullptr),
    m_editRequestButton(nullptr),
    m_removeRequestButton(nullptr),
    m_noRequestsLabel(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr)
{
    setupUI();
    connectSignals();
    // applyTheme(); // REMOVED: Now handled by CSS files
}

DigiDollarReceiveWidget::~DigiDollarReceiveWidget()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarReceiveWidget::setupUI()
{
    // Create main layout - compact like DGB tabs
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Setup sections
    setupGenerateSection();
    setupQRSection();
    setupRecentRequestsSection();

    // Give the requests section stretch priority to expand and fill available space
    // No addStretch() at end - let the table section grow instead

    setLayout(m_mainLayout);
}

void DigiDollarReceiveWidget::setupGenerateSection()
{
    // Create generate frame
    m_generateFrame = new QFrame(this);
    m_generateFrame->setFrameStyle(QFrame::StyledPanel);
    m_generateFrame->setFrameShadow(QFrame::Sunken);
    m_generateFrame->setObjectName("generateFrame");

    m_generateLayout = new QGridLayout(m_generateFrame);
    m_generateLayout->setSpacing(4);
    m_generateLayout->setContentsMargins(6, 6, 6, 6);

    // Label field (optional)
    m_labelLabel = new QLabel(tr("&Label:"), this);
    m_labelLabel->setObjectName("labelLabel");
    m_labelLabel->setToolTip(tr("Optional label to identify this request"));

    m_labelEdit = new QLineEdit(this);
    m_labelEdit->setObjectName("labelEdit");
    m_labelEdit->setPlaceholderText(tr("e.g., Invoice #123"));
    m_labelEdit->setToolTip(tr("An optional label to identify this payment request"));
    m_labelLabel->setBuddy(m_labelEdit);

    // Amount field (optional)
    m_amountLabel = new QLabel(tr("&Amount:"), this);
    m_amountLabel->setObjectName("amountLabel");
    m_amountLabel->setToolTip(tr("Optional amount in DigiDollar"));

    m_amountEdit = new QLineEdit(this);
    m_amountEdit->setObjectName("amountEdit");
    m_amountEdit->setPlaceholderText(tr("0.00"));
    m_amountEdit->setToolTip(tr("An optional amount to request (in $DD)"));
    m_amountLabel->setBuddy(m_amountEdit);

    // Message field (optional)
    m_messageLabel = new QLabel(tr("&Message:"), this);
    m_messageLabel->setObjectName("messageLabel");
    m_messageLabel->setToolTip(tr("Optional message for the payment"));

    m_messageEdit = new QLineEdit(this);
    m_messageEdit->setObjectName("messageEdit");
    m_messageEdit->setPlaceholderText(tr("Payment for goods or services"));
    m_messageEdit->setToolTip(tr("An optional message to attach to the payment request"));
    m_messageLabel->setBuddy(m_messageEdit);

    // Buttons
    m_generateButton = new QPushButton(tr("&Generate New Address"), this);
    m_generateButton->setObjectName("generateButton");
    m_generateButton->setProperty("ddState", "primaryEnabled");
    m_generateButton->setToolTip(tr("Generate a new DigiDollar receiving address"));
    m_generateButton->setMinimumWidth(175);
    m_generateButton->setStyleSheet(
        "QPushButton#generateButton:enabled {"
        "  background-color: #16804f;"
        "  color: #ffffff;"
        "  border: 1px solid #16804f;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "  font-weight: 600;"
        "}"
        "QPushButton#generateButton:enabled:hover {"
        "  background-color: #21a866;"
        "}"
        "QPushButton#generateButton:enabled:pressed {"
        "  background-color: #0b3f2a;"
        "}"
        "QPushButton#generateButton:disabled {"
        "  background-color: #d0d0d0;"
        "  color: #777777;"
        "  border: 1px solid #b8b8b8;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "}");

    m_clearButton = new QPushButton(tr("C&lear"), this);
    m_clearButton->setObjectName("clearButton");
    m_clearButton->setToolTip(tr("Clear all input fields"));

    // Add to layout
    m_generateLayout->addWidget(m_labelLabel, 0, 0);
    m_generateLayout->addWidget(m_labelEdit, 0, 1, 1, 2);

    m_generateLayout->addWidget(m_amountLabel, 1, 0);
    m_generateLayout->addWidget(m_amountEdit, 1, 1, 1, 2);

    m_generateLayout->addWidget(m_messageLabel, 2, 0);
    m_generateLayout->addWidget(m_messageEdit, 2, 1, 1, 2);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_clearButton);
    buttonLayout->addWidget(m_generateButton);

    m_generateLayout->addLayout(buttonLayout, 3, 0, 1, 3);

    m_mainLayout->addWidget(m_generateFrame);
}

void DigiDollarReceiveWidget::setupQRSection()
{
    // Create QR frame - compact design
    m_qrFrame = new QFrame(this);
    m_qrFrame->setFrameStyle(QFrame::StyledPanel);
    m_qrFrame->setFrameShadow(QFrame::Sunken);
    m_qrFrame->setObjectName("qrFrame");
    m_qrFrame->setVisible(false); // Hidden until address is generated

    m_qrLayout = new QVBoxLayout(m_qrFrame);
    m_qrLayout->setSpacing(4);
    m_qrLayout->setContentsMargins(8, 6, 8, 6);

    // Title - smaller font
    m_qrTitle = new QLabel(tr("Your DigiDollar Address"), this);
    m_qrTitle->setObjectName("qrTitle");
    QFont titleFont = m_qrTitle->font();
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleFont.setBold(true);
    m_qrTitle->setFont(titleFont);
    m_qrTitle->setAlignment(Qt::AlignCenter);

    // QR code image (hidden to save space - kept for future use)
    m_qrImage = new QRImageWidget(this);
    m_qrImage->setObjectName("qrImage");
    m_qrImage->setMinimumSize(200, 200);
    m_qrImage->setMaximumSize(200, 200);
    m_qrImage->setVisible(false);

    // Address label (hidden - title is sufficient)
    m_addressLabel = new QLabel(tr("Address:"), this);
    m_addressLabel->setObjectName("addressLabel");
    m_addressLabel->setVisible(false);

    // Address edit - compact
    m_addressEdit = new QLineEdit(this);
    m_addressEdit->setObjectName("addressEdit");
    m_addressEdit->setReadOnly(true);
    m_addressEdit->setFont(GUIUtil::fixedPitchFont());
    m_addressEdit->setAlignment(Qt::AlignCenter);
    m_addressEdit->setMinimumHeight(28);

    // Copy button inline with address
    m_qrButtonLayout = new QHBoxLayout();
    m_qrButtonLayout->setSpacing(8);
    m_qrButtonLayout->setContentsMargins(0, 0, 0, 0);

    m_copyAddressButton = new QPushButton(tr("Copy Address"), this);
    m_copyAddressButton->setObjectName("copyAddressButton");
    m_copyAddressButton->setToolTip(tr("Copy the DigiDollar address to the clipboard"));
    m_copyAddressButton->setFixedWidth(120);

    m_copyQRButton = new QPushButton(tr("Copy &QR Code"), this);
    m_copyQRButton->setObjectName("copyQRButton");
    m_copyQRButton->setToolTip(tr("Copy the QR code image to the clipboard"));
    m_copyQRButton->setVisible(false);

    m_saveQRButton = new QPushButton(tr("&Save QR Code"), this);
    m_saveQRButton->setObjectName("saveQRButton");
    m_saveQRButton->setToolTip(tr("Save the QR code as an image file"));
    m_saveQRButton->setVisible(false);

    m_qrButtonLayout->addStretch();
    m_qrButtonLayout->addWidget(m_copyAddressButton);
    m_qrButtonLayout->addWidget(m_copyQRButton);
    m_qrButtonLayout->addWidget(m_saveQRButton);
    m_qrButtonLayout->addStretch();

    // Add to layout - compact vertical arrangement
    m_qrLayout->addWidget(m_qrTitle);
    m_qrLayout->addWidget(m_addressEdit);
    m_qrLayout->addLayout(m_qrButtonLayout);

    m_mainLayout->addWidget(m_qrFrame);

    m_emptyStateLabel = new QLabel(tr("Generate a new DigiDollar address to receive $DD."), this);
    m_emptyStateLabel->setObjectName("emptyStateLabel");
    m_emptyStateLabel->setAlignment(Qt::AlignCenter);
    m_emptyStateLabel->setWordWrap(true);
    m_emptyStateLabel->setMinimumHeight(42);
    m_emptyStateLabel->setToolTip(tr("No receive address is shown until you generate or select a DigiDollar payment request."));
    m_mainLayout->addWidget(m_emptyStateLabel);

    // Frame starts hidden, shown when address is generated or selected.
}

void DigiDollarReceiveWidget::setupRecentRequestsSection()
{
    // Create requests frame
    m_requestsFrame = new QFrame(this);
    m_requestsFrame->setFrameStyle(QFrame::StyledPanel);
    m_requestsFrame->setFrameShadow(QFrame::Sunken);
    m_requestsFrame->setObjectName("requestsFrame");

    m_requestsLayout = new QVBoxLayout(m_requestsFrame);
    m_requestsLayout->setSpacing(4);
    m_requestsLayout->setContentsMargins(6, 6, 6, 6);

    // Title
    m_requestsTitle = new QLabel(tr("Recent Payment Requests"), this);
    m_requestsTitle->setObjectName("requestsTitle");
    QFont titleFont = m_requestsTitle->font();
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleFont.setBold(true);
    m_requestsTitle->setFont(titleFont);

    // Table with improved column configuration
    m_requestsTable = new QTableWidget(this);
    m_requestsTable->setObjectName("requestsTable");
    m_requestsTable->setColumnCount(4);
    m_requestsTable->setHorizontalHeaderLabels({tr("Date"), tr("Label"), tr("Amount"), tr("Address")});
    m_requestsTable->verticalHeader()->setVisible(false);
    m_requestsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_requestsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_requestsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_requestsTable->setAlternatingRowColors(true);
    m_requestsTable->setShowGrid(false);
    // Removed setMinimumHeight - allow table to shrink with window
    m_requestsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_requestsTable->setSortingEnabled(true);

    // Let the date column fit the computer's regional format.
    m_requestsTable->setColumnWidth(1, 120);  // Label
    m_requestsTable->setColumnWidth(2, 80);   // Amount
    m_requestsTable->horizontalHeader()->setStretchLastSection(true);  // Address fills remaining
    m_requestsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_requestsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_requestsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_requestsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    // No requests label (shown when table is empty)
    m_noRequestsLabel = new QLabel(tr("No recent payment requests"), this);
    m_noRequestsLabel->setObjectName("noRequestsLabel");
    m_noRequestsLabel->setAlignment(Qt::AlignCenter);
    // Removed setMinimumHeight - allow to shrink with window

    // Buttons
    m_requestsButtonLayout = new QHBoxLayout();

    m_showRequestButton = new QPushButton(tr("&Show"), this);
    m_showRequestButton->setObjectName("showRequestButton");
    m_showRequestButton->setToolTip(tr("Show the selected request"));
    m_showRequestButton->setEnabled(false);

    m_editRequestButton = new QPushButton(tr("&Edit"), this);
    m_editRequestButton->setObjectName("editRequestButton");
    m_editRequestButton->setToolTip(tr("Edit the selected request"));
    m_editRequestButton->setEnabled(false);

    m_removeRequestButton = new QPushButton(tr("&Remove"), this);
    m_removeRequestButton->setObjectName("removeRequestButton");
    m_removeRequestButton->setToolTip(tr("Remove the selected request from history"));
    m_removeRequestButton->setEnabled(false);

    m_requestsButtonLayout->addWidget(m_showRequestButton);
    m_requestsButtonLayout->addWidget(m_editRequestButton);
    m_requestsButtonLayout->addWidget(m_removeRequestButton);
    m_requestsButtonLayout->addStretch();

    // Add to layout
    m_requestsLayout->addWidget(m_requestsTitle);
    m_requestsLayout->addWidget(m_requestsTable, 1);  // Stretch factor 1 - table expands
    m_requestsLayout->addWidget(m_noRequestsLabel);
    m_requestsLayout->addLayout(m_requestsButtonLayout);

    // Initially show "no requests" label
    m_requestsTable->setVisible(false);
    m_noRequestsLabel->setVisible(true);

    // Add with stretch factor so this section expands to fill available space
    m_mainLayout->addWidget(m_requestsFrame, 1);
}

void DigiDollarReceiveWidget::connectSignals()
{
    // Generate section
    connect(m_generateButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onGenerateAddressClicked);
    connect(m_clearButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onClearClicked);
    connect(m_labelEdit, &QLineEdit::textChanged,
            this, &DigiDollarReceiveWidget::onLabelChanged);
    connect(m_amountEdit, &QLineEdit::textChanged,
            this, &DigiDollarReceiveWidget::onAmountChanged);
    connect(m_messageEdit, &QLineEdit::textChanged,
            this, &DigiDollarReceiveWidget::onMessageChanged);

    // QR section
    connect(m_copyAddressButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onCopyAddressClicked);
    connect(m_copyQRButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onCopyQRClicked);
    connect(m_saveQRButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onSaveQRClicked);

    // Recent requests section
    connect(m_requestsTable, &QTableWidget::itemSelectionChanged,
            this, &DigiDollarReceiveWidget::onRecentRequestSelected);
    connect(m_showRequestButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onShowRequestClicked);
    connect(m_editRequestButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onEditRequestClicked);
    connect(m_removeRequestButton, &QPushButton::clicked,
            this, &DigiDollarReceiveWidget::onRemoveRequestClicked);
    connect(m_requestsTable, &QTableWidget::cellDoubleClicked,
            this, &DigiDollarReceiveWidget::onRecentRequestDoubleClicked);
    connect(m_requestsTable, &QWidget::customContextMenuRequested,
            this, &DigiDollarReceiveWidget::showContextMenu);

    // Create context menu
    m_contextMenu = new QMenu(this);
    m_contextMenu->addAction(tr("&Edit request"), this, &DigiDollarReceiveWidget::editRequest);
    m_contextMenu->addSeparator();
    m_contextMenu->addAction(tr("Copy &URI"), this, &DigiDollarReceiveWidget::copyURI);
    m_contextMenu->addAction(tr("&Copy address"), this, &DigiDollarReceiveWidget::copyAddress);
    m_contextMenu->addAction(tr("Copy &label"), this, &DigiDollarReceiveWidget::copyLabel);
    m_contextMenu->addAction(tr("Copy &message"), this, &DigiDollarReceiveWidget::copyMessage);
    m_contextMenu->addAction(tr("Copy &amount"), this, &DigiDollarReceiveWidget::copyAmount);
}

void DigiDollarReceiveWidget::setWalletModel(WalletModel* model)
{
    m_walletModel = model;

    if (m_walletModel) {
        updateRecentRequests();
        // applyTheme(); // REMOVED: Now handled by CSS files
    }
}

void DigiDollarReceiveWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    if (m_clientModel) {
        // applyTheme(); // REMOVED: Now handled by CSS files
    }
}

void DigiDollarReceiveWidget::updateView()
{
    updateRecentRequests();
}

void DigiDollarReceiveWidget::updateRecentRequests()
{
    // TODO: Implement recent requests from wallet model
    // For now, keep the table empty
    populateRecentRequests();
}

// REMOVED: applyTheme() - All styling now handled by CSS files (light.css/dark.css)
// This method was overriding the CSS theme with programmatic styling
void DigiDollarReceiveWidget::applyTheme()
{
    // Method disabled - CSS handles all theming now
}

void DigiDollarReceiveWidget::setMonospacedFont(bool use_embedded_font)
{
    if (use_embedded_font) {
        m_addressEdit->setFont(GUIUtil::fixedPitchFont());
    }
}

void DigiDollarReceiveWidget::onGenerateAddressClicked()
{
    if (!m_walletModel) {
        Q_EMIT message(tr("Error"), tr("No wallet model available"), QMessageBox::Critical);
        return;
    }

    generateNewAddress();
}

void DigiDollarReceiveWidget::generateNewAddress()
{
    if (!m_walletModel) {
        return;
    }

    CAmount requestedAmount = 0;
    if (!ParseDigiDollarRequestAmount(m_amountEdit->text(), requestedAmount)) {
        Q_EMIT message(tr("Invalid Amount"),
                       tr("Enter a DigiDollar request amount with no more than two decimal places, or leave the amount blank."),
                       QMessageBox::Warning);
        m_amountEdit->setFocus();
        return;
    }

    // Get label from input field
    QString label = m_labelEdit->text();
    if (label.isEmpty()) {
        label = tr("Payment request");
    }

    // Generate new DD address using wallet model
    QString newAddress = m_walletModel->getNewDigiDollarAddress(label);

    if (newAddress.isEmpty()) {
        Q_EMIT message(tr("Error"), tr("Failed to generate new DigiDollar address"), QMessageBox::Critical);
        return;
    }

    m_currentAddress = newAddress;
    m_currentLabel = m_labelEdit->text();
    m_currentAmount = requestedAmount > 0 ? FormatDigiDollarRequestAmount(requestedAmount) : QString();
    m_currentMessage = m_messageEdit->text();

    // Update address display
    m_addressEdit->setText(m_currentAddress);

    // Update QR code
    updateQRCode();

    // Show address section (QR image hidden to save space)
    m_qrFrame->setVisible(true);
    m_emptyStateLabel->setVisible(false);

    // Create payment request and save to wallet
    SendCoinsRecipient recipient;
    recipient.address = m_currentAddress;
    recipient.label = m_currentLabel;

    recipient.amount = requestedAmount;

    recipient.message = m_currentMessage;

    // Add to recent requests table model (this persists to wallet.dat)
    if (m_walletModel && m_walletModel->getRecentRequestsTableModel()) {
        m_walletModel->getRecentRequestsTableModel()->addNewRequest(recipient);
    }

    // Add to UI table for immediate display
    const QDateTime date = QDateTime::currentDateTime();
    QString amountStr = requestedAmount > 0 ? formatDDAmount(requestedAmount / 100.0) : tr("Any");
    addRequestToTable(date, m_currentLabel, amountStr, m_currentAddress);

    // Show the QR code popup dialog (same as DGB receive behavior)
    DigiDollarReceiveRequestDialog* dialog = new DigiDollarReceiveRequestDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModel(m_walletModel);
    dialog->setInfo(recipient);
    dialog->show();
}

void DigiDollarReceiveWidget::updateQRCode()
{
    if (m_currentAddress.isEmpty()) {
        return;
    }

    // Create payment URI
    QString uri = formatDDURI(m_currentAddress, m_currentLabel, m_currentAmount, m_currentMessage);

    // Update QR code widget
    m_qrImage->setQR(uri, m_currentAddress);
}

void DigiDollarReceiveWidget::onCopyAddressClicked()
{
    QApplication::clipboard()->setText(m_currentAddress);
    Q_EMIT message(tr("Address Copied"), tr("DigiDollar address copied to clipboard"), QMessageBox::Information);
}

void DigiDollarReceiveWidget::onCopyQRClicked()
{
    if (m_qrImage) {
        m_qrImage->copyImage();
        Q_EMIT message(tr("QR Code Copied"), tr("QR code image copied to clipboard"), QMessageBox::Information);
    }
}

void DigiDollarReceiveWidget::onSaveQRClicked()
{
    if (!m_qrImage) {
        return;
    }

    QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString defaultFileName = defaultPath + "/digidollar-" + m_currentAddress.left(8) + ".png";

    QString fileName = QFileDialog::getSaveFileName(this, tr("Save QR Code"),
                                                    defaultFileName, tr("PNG Image (*.png)"));
    if (fileName.isEmpty()) {
        return;
    }

    QImage qrImage = m_qrImage->exportImage();
    if (qrImage.save(fileName, "PNG")) {
        Q_EMIT message(tr("QR Code Saved"),
                      tr("QR code saved successfully to:\n%1").arg(fileName),
                      QMessageBox::Information);
    } else {
        Q_EMIT message(tr("Error"), tr("Failed to save QR code"), QMessageBox::Critical);
    }
}

void DigiDollarReceiveWidget::onClearClicked()
{
    clearFields();
}

void DigiDollarReceiveWidget::clearFields()
{
    m_labelEdit->clear();
    m_amountEdit->clear();
    m_messageEdit->clear();
    m_addressEdit->clear();
    m_currentAddress.clear();
    m_currentLabel.clear();
    m_currentAmount.clear();
    m_currentMessage.clear();
    m_qrFrame->setVisible(false);
    m_emptyStateLabel->setVisible(true);
}

void DigiDollarReceiveWidget::onLabelChanged()
{
    m_currentLabel = m_labelEdit->text();
    if (!m_currentAddress.isEmpty()) {
        updateQRCode();
    }
}

void DigiDollarReceiveWidget::onAmountChanged()
{
    CAmount requestedAmount = 0;
    m_currentAmount = ParseDigiDollarRequestAmount(m_amountEdit->text(), requestedAmount) && requestedAmount > 0 ?
        FormatDigiDollarRequestAmount(requestedAmount) : QString();
    if (!m_currentAddress.isEmpty()) {
        updateQRCode();
    }
}

void DigiDollarReceiveWidget::onMessageChanged()
{
    m_currentMessage = m_messageEdit->text();
    if (!m_currentAddress.isEmpty()) {
        updateQRCode();
    }
}

void DigiDollarReceiveWidget::onRecentRequestSelected()
{
    bool hasSelection = !m_requestsTable->selectedItems().isEmpty();
    m_showRequestButton->setEnabled(hasSelection);
    m_editRequestButton->setEnabled(hasSelection);
    m_removeRequestButton->setEnabled(hasSelection);

    // Keep the "Your DigiDollar Address" panel in sync with the highlighted
    // request row, matching DGB receive-table UX. Without this, the panel
    // kept showing the last-generated address instead of the selected one.
    if (!hasSelection) {
        return;
    }
    int row = selectedRow();
    if (row < 0 || row >= m_requestsTable->rowCount()) {
        return;
    }
    QTableWidgetItem* addressItem = m_requestsTable->item(row, 3);
    if (!addressItem) {
        return;
    }
    QString address = addressItem->data(Qt::UserRole).toString();
    if (address.isEmpty()) {
        address = addressItem->text();
    }
    if (address.isEmpty()) {
        return;
    }
    m_currentAddress = address;
    if (m_addressEdit) {
        m_addressEdit->setText(address);
    }
    updateQRCode();
    m_qrFrame->setVisible(true);
    m_emptyStateLabel->setVisible(false);
}

void DigiDollarReceiveWidget::onShowRequestClicked()
{
    if (!m_walletModel) {
        return;
    }

    int row = selectedRow();
    if (row < 0 || row >= m_requestsTable->rowCount()) {
        LogPrint(BCLog::QT, "DigiDollarReceiveWidget: No valid row selected for Show button\n");
        return;
    }

    QString address = addressFromRow(row);

    // Verify this is a current-network DD address before showing dialog.
    if (!IsCurrentNetworkDigiDollarAddress(address)) {
        LogPrint(BCLog::QT, "DigiDollarReceiveWidget: Address is not a valid DD address: %s\n",
                address.toStdString());
        Q_EMIT message(tr("Error"),
                      tr("Selected address is not a valid DigiDollar address"),
                      QMessageBox::Critical);
        return;
    }

    SendCoinsRecipient recipient = recipientFromRow(row);

    // Show detailed request dialog
    DigiDollarReceiveRequestDialog* dialog = new DigiDollarReceiveRequestDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModel(m_walletModel);
    dialog->setInfo(recipient);
    dialog->show();
}

void DigiDollarReceiveWidget::onRemoveRequestClicked()
{
    int row = m_requestsTable->currentRow();
    if (row < 0) {
        return;
    }

    const QString address = addressFromRow(row);
    if (!address.isEmpty() && !removeDigiDollarRequest(address)) {
        Q_EMIT message(tr("Error"), tr("Failed to remove DigiDollar payment request"), QMessageBox::Critical);
        return;
    }

    m_requestsTable->removeRow(row);

    // Update visibility based on remaining rows
    if (m_requestsTable->rowCount() == 0) {
        m_requestsTable->setVisible(false);
        m_noRequestsLabel->setVisible(true);
    }
}

void DigiDollarReceiveWidget::onEditRequestClicked()
{
    const int row = selectedRow();
    if (row < 0 || row >= m_requestsTable->rowCount()) {
        return;
    }
    editDigiDollarRequest(row);
}

void DigiDollarReceiveWidget::populateRecentRequests()
{
    // Clear existing table entries first
    m_requestsTable->setRowCount(0);

    if (!m_walletModel) {
        m_requestsTable->setVisible(false);
        m_noRequestsLabel->setVisible(true);
        return;
    }

    // Load DD addresses directly from wallet storage (not from RecentRequestsTableModel,
    // which now filters out DD addresses to keep DGB and DD systems separate)
    std::vector<std::string> requests = m_walletModel->wallet().getAddressReceiveRequests();

    LogPrint(BCLog::QT, "DigiDollarReceiveWidget: Loading DD requests from %d total wallet requests\n", (int)requests.size());

    for (const std::string& requestStr : requests) {
        // Deserialize the entry
        std::vector<uint8_t> data(requestStr.begin(), requestStr.end());
        DataStream ss{data};

        RecentRequestEntry entry;
        try {
            ss >> entry;
        } catch (const std::exception&) {
            continue;  // Skip malformed entries
        }

        if (entry.id == 0) {
            continue;  // Should not happen
        }

        // Get address
        QString address = entry.recipient.address;

        // Filter: only show DigiDollar requests for the active chain.
        if (!IsCurrentNetworkDigiDollarAddress(address)) {
            continue;  // Skip non-DD addresses
        }

        // Get label
        QString label = entry.recipient.label;

        // Format amount
        QString amountStr;
        if (entry.recipient.amount > 0) {
            // Amount stored as cents for DD
            double ddAmount = entry.recipient.amount / 100.0;
            amountStr = formatDDAmount(ddAmount);
        } else {
            amountStr = tr("Any");
        }

        // Add to table
        addRequestToTable(entry.date, label, amountStr, address, entry.id);
    }

    // Update visibility based on row count
    if (m_requestsTable->rowCount() == 0) {
        m_requestsTable->setVisible(false);
        m_noRequestsLabel->setVisible(true);
    } else {
        m_requestsTable->setVisible(true);
        m_noRequestsLabel->setVisible(false);
    }
}

void DigiDollarReceiveWidget::addRequestToTable(const QDateTime& date, const QString& label,
                                                const QString& amount, const QString& address, qint64 id)
{
    // Keep the date and request details together while inserting a row.
    const bool sorting = m_requestsTable->isSortingEnabled();
    m_requestsTable->setSortingEnabled(false);
    m_requestsTable->insertRow(0);

    // Date column
    auto* dateItem = new GUIUtil::NumericTableWidgetItem(QLocale::system().toString(date.date(), QLocale::ShortFormat));
    dateItem->setData(Qt::UserRole, date.toSecsSinceEpoch());
    dateItem->setToolTip(date.isValid() ? GUIUtil::dateTimeStr(date) : tr("No date"));
    m_requestsTable->setItem(0, 0, dateItem);

    // Label column
    QTableWidgetItem* labelItem = new QTableWidgetItem(label.isEmpty() ? tr("-") : label);
    labelItem->setToolTip(label.isEmpty() ? tr("No label") : label);
    m_requestsTable->setItem(0, 1, labelItem);

    // Amount column
    QTableWidgetItem* amountItem = new QTableWidgetItem(amount);
    amountItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_requestsTable->setItem(0, 2, amountItem);

    // Address column - store full address in UserRole for retrieval, show with tooltip
    QTableWidgetItem* addressItem = new QTableWidgetItem(address);
    addressItem->setData(Qt::UserRole, address);  // Store full address
    addressItem->setData(Qt::UserRole + 1, id);
    addressItem->setToolTip(address.isEmpty() ? tr("No address available") : address);  // Show full address on hover
    addressItem->setFont(GUIUtil::fixedPitchFont());
    m_requestsTable->setItem(0, 3, addressItem);
    m_requestsTable->setSortingEnabled(sorting);

    // Show table, hide "no requests" label
    m_requestsTable->setVisible(true);
    m_noRequestsLabel->setVisible(false);
}

QString DigiDollarReceiveWidget::formatDDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $DD";
}

QString DigiDollarReceiveWidget::formatDDURI(const QString& address, const QString& label,
                                             const QString& amount, const QString& message) const
{
    QString uri = "digidollar:" + address;
    QStringList params;

    if (!label.isEmpty()) {
        params << "label=" + QString(QUrl::toPercentEncoding(label));
    }
    CAmount parsedAmount = 0;
    if (ParseDigiDollarRequestAmount(amount, parsedAmount) && parsedAmount > 0) {
        params << "amount=" + FormatDigiDollarRequestAmount(parsedAmount);
    }
    if (!message.isEmpty()) {
        params << "message=" + QString(QUrl::toPercentEncoding(message));
    }

    if (!params.isEmpty()) {
        uri += "?" + params.join("&");
    }

    return uri;
}

void DigiDollarReceiveWidget::onRecentRequestDoubleClicked(int row, int column)
{
    Q_UNUSED(column);

    if (!m_walletModel) {
        return;
    }

    if (row < 0 || row >= m_requestsTable->rowCount()) {
        LogPrint(BCLog::QT, "DigiDollarReceiveWidget: Invalid row %d for double-click\n", row);
        return;
    }

    m_requestsTable->setCurrentCell(row, 0);
    onShowRequestClicked();
}

int DigiDollarReceiveWidget::selectedRow() const
{
    if (!m_requestsTable) {
        return -1;
    }
    return m_requestsTable->currentRow();
}

QString DigiDollarReceiveWidget::addressFromRow(int row) const
{
    if (row < 0 || row >= m_requestsTable->rowCount()) {
        return {};
    }

    QTableWidgetItem* addressItem = m_requestsTable->item(row, 3); // Address column
    if (!addressItem) {
        return {};
    }
    QString address = addressItem->data(Qt::UserRole).toString();
    if (address.isEmpty()) {
        address = addressItem->text();
    }
    return address;
}

SendCoinsRecipient DigiDollarReceiveWidget::recipientFromRow(int row) const
{
    RecentRequestEntry entry;
    if (findDigiDollarRequest(addressFromRow(row), entry)) {
        return entry.recipient;
    }

    SendCoinsRecipient recipient;
    recipient.address = addressFromRow(row);
    QTableWidgetItem* labelItem = m_requestsTable->item(row, 1);
    QTableWidgetItem* amountItem = m_requestsTable->item(row, 2);
    recipient.label = labelItem ? labelItem->text() : QString();
    if (recipient.label == tr("-")) recipient.label.clear();
    if (amountItem) {
        QString amountStr = amountItem->text();
        if (amountStr != tr("Any") && !amountStr.isEmpty()) {
            amountStr.remove(" $DD");
            amountStr.remove(" DD");
            bool ok = false;
            const double ddAmount = amountStr.toDouble(&ok);
            if (ok && ddAmount > 0) {
                recipient.amount = static_cast<CAmount>(ddAmount * 100);
            }
        }
    }
    return recipient;
}

bool DigiDollarReceiveWidget::getSelectedRequest(RecentRequestEntry& entry) const
{
    return findDigiDollarRequest(addressFromRow(selectedRow()), entry);
}

bool DigiDollarReceiveWidget::findDigiDollarRequest(const QString& address, RecentRequestEntry& entry) const
{
    if (!m_walletModel || address.isEmpty() || !IsCurrentNetworkDigiDollarAddress(address)) {
        return false;
    }

    for (const std::string& requestStr : m_walletModel->wallet().getAddressReceiveRequests()) {
        std::vector<uint8_t> data(requestStr.begin(), requestStr.end());
        DataStream ss{data};
        RecentRequestEntry candidate;
        try {
            ss >> candidate;
        } catch (const std::exception&) {
            continue;
        }
        if (candidate.recipient.address == address &&
            IsCurrentNetworkDigiDollarAddress(candidate.recipient.address)) {
            entry = candidate;
            return true;
        }
    }
    return false;
}

bool DigiDollarReceiveWidget::updateDigiDollarRequest(const RecentRequestEntry& entry)
{
    if (!m_walletModel || entry.id == 0 ||
        !IsCurrentNetworkDigiDollarAddress(entry.recipient.address)) {
        return false;
    }

    DataStream ss{};
    ss << entry;
    const CTxDestination request_dest = DecodeDigiDollarAddress(entry.recipient.address.toStdString());
    if (!m_walletModel->wallet().setAddressReceiveRequest(request_dest, ToString(entry.id), ss.str())) {
        return false;
    }
    const CTxDestination address_book_dest = DecodeDigiDollarAddress(entry.recipient.address.toStdString());
    return m_walletModel->wallet().setAddressBook(address_book_dest, entry.recipient.label.toStdString(),
                                                  wallet::AddressPurpose::DIGIDOLLAR);
}

bool DigiDollarReceiveWidget::removeDigiDollarRequest(const QString& address)
{
    RecentRequestEntry entry;
    if (!findDigiDollarRequest(address, entry)) {
        return false;
    }
    const CTxDestination dest = DecodeDigiDollarAddress(entry.recipient.address.toStdString());
    return m_walletModel->wallet().setAddressReceiveRequest(dest, ToString(entry.id), "");
}

bool DigiDollarReceiveWidget::editDigiDollarRequest(int row)
{
    RecentRequestEntry entry;
    if (!findDigiDollarRequest(addressFromRow(row), entry)) {
        Q_EMIT message(tr("Error"), tr("Selected DigiDollar request was not found"), QMessageBox::Critical);
        return false;
    }

    QDialog dialog(this, GUIUtil::dialog_flags);
    dialog.setWindowTitle(tr("Edit DigiDollar Payment Request"));

    QGridLayout* layout = new QGridLayout(&dialog);
    QLabel* labelLabel = new QLabel(tr("&Label:"), &dialog);
    QLineEdit* labelEdit = new QLineEdit(entry.recipient.label, &dialog);
    labelEdit->setObjectName("ddRequestLabelEdit");
    labelLabel->setBuddy(labelEdit);

    QLabel* amountLabel = new QLabel(tr("&Amount:"), &dialog);
    QDoubleSpinBox* amountEdit = new QDoubleSpinBox(&dialog);
    amountEdit->setObjectName("ddRequestAmountEdit");
    amountEdit->setDecimals(2);
    amountEdit->setRange(0.00, 100000000.00);
    amountEdit->setSuffix(QStringLiteral(" $DD"));
    amountEdit->setValue(entry.recipient.amount > 0 ? entry.recipient.amount / 100.0 : 0.0);
    amountLabel->setBuddy(amountEdit);

    QLabel* messageLabel = new QLabel(tr("&Message:"), &dialog);
    QLineEdit* messageEdit = new QLineEdit(entry.recipient.message, &dialog);
    messageEdit->setObjectName("ddRequestMessageEdit");
    messageLabel->setBuddy(messageEdit);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    layout->addWidget(labelLabel, 0, 0);
    layout->addWidget(labelEdit, 0, 1);
    layout->addWidget(amountLabel, 1, 0);
    layout->addWidget(amountEdit, 1, 1);
    layout->addWidget(messageLabel, 2, 0);
    layout->addWidget(messageEdit, 2, 1);
    layout->addWidget(buttons, 3, 0, 1, 2);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    entry.recipient.label = labelEdit->text();
    entry.recipient.message = messageEdit->text();
    entry.recipient.amount = amountEdit->value() > 0.0 ? static_cast<CAmount>(amountEdit->value() * 100) : 0;

    if (!updateDigiDollarRequest(entry)) {
        Q_EMIT message(tr("Error"), tr("Failed to update DigiDollar payment request"), QMessageBox::Critical);
        return false;
    }

    updateRecentRequests();
    for (int i = 0; i < m_requestsTable->rowCount(); ++i) {
        if (addressFromRow(i) == entry.recipient.address) {
            m_requestsTable->selectRow(i);
            break;
        }
    }
    return true;
}

void DigiDollarReceiveWidget::showContextMenu(const QPoint &point)
{
    if (selectedRow() < 0) {
        return;
    }

    RecentRequestEntry entry;
    if (!getSelectedRequest(entry)) {
        return;
    }

    // Enable/disable menu items based on data availability
    QList<QAction*> actions = m_contextMenu->actions();
    if (actions.size() >= 7) {
        // Copy label - disable if empty
        actions[4]->setEnabled(!entry.recipient.label.isEmpty());
        // Copy message - disable if empty
        actions[5]->setEnabled(!entry.recipient.message.isEmpty());
        // Copy amount - disable if zero
        actions[6]->setEnabled(entry.recipient.amount > 0);
    }

    m_contextMenu->exec(QCursor::pos());
}

void DigiDollarReceiveWidget::copyURI()
{
    RecentRequestEntry entry;
    if (!getSelectedRequest(entry)) {
        return;
    }

    QString uri = formatDDURI(entry.recipient.address,
                              entry.recipient.label,
                              entry.recipient.amount > 0 ? QString::number(entry.recipient.amount / 100.0, 'f', 8) : QString(),
                              entry.recipient.message);
    GUIUtil::setClipboard(uri);
}

void DigiDollarReceiveWidget::copyAddress()
{
    RecentRequestEntry entry;
    if (!getSelectedRequest(entry)) {
        return;
    }

    GUIUtil::setClipboard(entry.recipient.address);
}

void DigiDollarReceiveWidget::copyLabel()
{
    RecentRequestEntry entry;
    if (!getSelectedRequest(entry)) {
        return;
    }

    GUIUtil::setClipboard(entry.recipient.label);
}

void DigiDollarReceiveWidget::copyMessage()
{
    RecentRequestEntry entry;
    if (!getSelectedRequest(entry)) {
        return;
    }

    GUIUtil::setClipboard(entry.recipient.message);
}

void DigiDollarReceiveWidget::copyAmount()
{
    RecentRequestEntry entry;
    if (!getSelectedRequest(entry)) {
        return;
    }

    if (entry.recipient.amount > 0) {
        double ddAmount = entry.recipient.amount / 100.0;
        GUIUtil::setClipboard(QString::number(ddAmount, 'f', 8));
    }
}

void DigiDollarReceiveWidget::editRequest()
{
    onEditRequestClicked();
}
