// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarreceiverequest.h>

#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/qrimagewidget.h>
#include <qt/walletmodel.h>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QUrl>
#include <QFont>
#include <QPalette>
#include <QStyle>

namespace {

bool UseDarkDigiDollarReceiveRequestTheme(const WalletModel* model, const QWidget* widget)
{
    if (model && model->getOptionsModel()) {
        const QString theme = model->getOptionsModel()->data(
            model->getOptionsModel()->index(OptionsModel::Theme), Qt::EditRole).toString();
        return theme.isEmpty() || theme == QLatin1String("dark");
    }
    const QPalette palette = widget ? widget->palette() : qApp->palette();
    return palette.color(QPalette::Window).lightness() < 128;
}

QString DigiDollarReceiveRequestDialogStyleSheet(bool dark_theme)
{
    if (dark_theme) {
        return QStringLiteral(
            "QDialog#DigiDollarReceiveRequestDialog {"
            "  background-color: #0b2419;"
            "  color: #ffffff;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QWidget {"
            "  background-color: transparent;"
            "  color: #ffffff;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QLabel {"
            "  color: #ffffff;"
            "  background-color: transparent;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QRImageWidget {"
            "  background-color: #ffffff;"
            "  border: 2px solid #42d884;"
            "  border-radius: 4px;"
            "  padding: 10px;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QTextEdit,"
            "QDialog#DigiDollarReceiveRequestDialog QLineEdit {"
            "  background-color: #113a29;"
            "  color: #ffffff;"
            "  border: 2px solid #42d884;"
            "  border-radius: 4px;"
            "  padding: 5px;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QPushButton {"
            "  background-color: #16804f;"
            "  color: #ffffff;"
            "  border: 2px solid #16804f;"
            "  border-radius: 4px;"
            "  padding: 8px 16px;"
            "  font-weight: bold;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QPushButton:hover {"
            "  background-color: #21a866;"
            "  border-color: #21a866;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QPushButton:pressed {"
            "  background-color: #0f633c;"
            "  border-color: #0f633c;"
            "}"
            "QDialog#DigiDollarReceiveRequestDialog QDialogButtonBox {"
            "  background-color: transparent;"
            "}");
    }

    return QStringLiteral(
        "QDialog#DigiDollarReceiveRequestDialog {"
        "  background-color: #eef9f2;"
        "  color: #123f2b;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QWidget {"
        "  background-color: transparent;"
        "  color: #123f2b;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QLabel {"
        "  color: #123f2b;"
        "  background-color: transparent;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QRImageWidget {"
        "  background-color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 10px;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QTextEdit,"
        "QDialog#DigiDollarReceiveRequestDialog QLineEdit {"
        "  background-color: #ffffff;"
        "  color: #123f2b;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 5px;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QPushButton {"
        "  background-color: #1f9d57;"
        "  color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 8px 16px;"
        "  font-weight: bold;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QPushButton:hover {"
        "  background-color: #26b96a;"
        "  border-color: #26b96a;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QPushButton:pressed {"
        "  background-color: #147a42;"
        "  border-color: #147a42;"
        "}"
        "QDialog#DigiDollarReceiveRequestDialog QDialogButtonBox {"
        "  background-color: transparent;"
        "}");
}

} // namespace

DigiDollarReceiveRequestDialog::DigiDollarReceiveRequestDialog(QWidget *parent)
    : QDialog(parent, GUIUtil::dialog_flags),
      m_model(nullptr)
{
    // Set object name FIRST for CSS styling to work
    setObjectName("DigiDollarReceiveRequestDialog");

    setupUI();
    applyTheme();
    GUIUtil::handleCloseWindowShortcut(this);

    // Force style refresh after object name is set
    style()->unpolish(this);
    style()->polish(this);
}

DigiDollarReceiveRequestDialog::~DigiDollarReceiveRequestDialog()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarReceiveRequestDialog::setupUI()
{
    setWindowTitle(tr("DigiDollar Payment Request"));

    // Use QGridLayout exactly like DGB's receiverequestdialog.ui
    QGridLayout* gridLayout = new QGridLayout(this);
    gridLayout->setSizeConstraint(QLayout::SetFixedSize);
    gridLayout->setColumnStretch(0, 0);  // First column fixed width
    gridLayout->setColumnStretch(1, 1);  // Second column stretches

    int row = 0;

    // Row 0: QR Code - centered, spanning both columns (EXACTLY like DGB)
    m_qrWidget = new QRImageWidget(this);
    gridLayout->addWidget(m_qrWidget, row, 0, 1, 2, Qt::AlignHCenter);
    row++;

    // Row 1: "Payment information" header (matching DGB)
    m_titleLabel = new QLabel(tr("Payment information"), this);
    QFont boldFont = m_titleLabel->font();
    boldFont.setBold(true);
    m_titleLabel->setFont(boldFont);
    gridLayout->addWidget(m_titleLabel, row, 0, 1, 2);
    row++;

    // Row 2: URI
    m_uriTagLabel = new QLabel(tr("URI:"), this);
    m_uriTagLabel->setFont(boldFont);
    gridLayout->addWidget(m_uriTagLabel, row, 0, Qt::AlignRight | Qt::AlignTop);

    m_uriContent = new QLabel(this);
    m_uriContent->setTextFormat(Qt::RichText);
    m_uriContent->setWordWrap(true);
    m_uriContent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gridLayout->addWidget(m_uriContent, row, 1, Qt::AlignTop);
    row++;

    // Row 3: Address
    m_addressTagLabel = new QLabel(tr("Address:"), this);
    m_addressTagLabel->setFont(boldFont);
    gridLayout->addWidget(m_addressTagLabel, row, 0, Qt::AlignRight | Qt::AlignTop);

    m_addressContent = new QLabel(this);
    m_addressContent->setTextFormat(Qt::PlainText);
    m_addressContent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gridLayout->addWidget(m_addressContent, row, 1, Qt::AlignTop);
    row++;

    // Row 4: Amount (hidden if not specified)
    m_amountTagLabel = new QLabel(tr("Amount:"), this);
    m_amountTagLabel->setFont(boldFont);
    gridLayout->addWidget(m_amountTagLabel, row, 0, Qt::AlignRight | Qt::AlignTop);

    m_amountContent = new QLabel(this);
    m_amountContent->setTextFormat(Qt::PlainText);
    m_amountContent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gridLayout->addWidget(m_amountContent, row, 1, Qt::AlignTop);
    row++;

    // Row 5: Label (hidden if not specified)
    m_labelTagLabel = new QLabel(tr("Label:"), this);
    m_labelTagLabel->setFont(boldFont);
    gridLayout->addWidget(m_labelTagLabel, row, 0, Qt::AlignRight | Qt::AlignTop);

    m_labelContent = new QLabel(this);
    m_labelContent->setTextFormat(Qt::PlainText);
    m_labelContent->setWordWrap(true);
    m_labelContent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gridLayout->addWidget(m_labelContent, row, 1, Qt::AlignTop);
    row++;

    // Row 6: Message (hidden if not specified)
    m_messageTagLabel = new QLabel(tr("Message:"), this);
    m_messageTagLabel->setFont(boldFont);
    gridLayout->addWidget(m_messageTagLabel, row, 0, Qt::AlignRight | Qt::AlignTop);

    m_messageContent = new QLabel(this);
    m_messageContent->setTextFormat(Qt::PlainText);
    m_messageContent->setWordWrap(true);
    m_messageContent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gridLayout->addWidget(m_messageContent, row, 1, Qt::AlignTop);
    row++;

    // Row 7: Wallet
    m_walletTagLabel = new QLabel(tr("Wallet:"), this);
    m_walletTagLabel->setFont(boldFont);
    gridLayout->addWidget(m_walletTagLabel, row, 0, Qt::AlignRight | Qt::AlignTop);

    m_walletContent = new QLabel(this);
    m_walletContent->setTextFormat(Qt::PlainText);
    m_walletContent->setWordWrap(true);
    m_walletContent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gridLayout->addWidget(m_walletContent, row, 1, Qt::AlignTop);
    row++;

    // Row 8: Buttons (exactly like DGB)
    QHBoxLayout* buttonLayout = new QHBoxLayout();

    m_copyURIButton = new QPushButton(tr("Copy &URI"), this);
    m_copyURIButton->setAutoDefault(false);
    connect(m_copyURIButton, &QPushButton::clicked, this, &DigiDollarReceiveRequestDialog::onCopyURIClicked);

    m_copyAddressButton = new QPushButton(tr("Copy &Address"), this);
    m_copyAddressButton->setAutoDefault(false);
    connect(m_copyAddressButton, &QPushButton::clicked, this, &DigiDollarReceiveRequestDialog::onCopyAddressClicked);

    m_verifyButton = new QPushButton(tr("&Verify"), this);
    m_verifyButton->setToolTip(tr("Verify this address on e.g. a hardware wallet screen"));
    m_verifyButton->setAutoDefault(false);
    m_verifyButton->setVisible(false);

    m_saveQRButton = new QPushButton(tr("&Save Image..."), this);
    m_saveQRButton->setAutoDefault(false);
    connect(m_saveQRButton, &QPushButton::clicked, this, &DigiDollarReceiveRequestDialog::onSaveQRClicked);

    buttonLayout->addWidget(m_copyURIButton);
    buttonLayout->addWidget(m_copyAddressButton);
    buttonLayout->addWidget(m_verifyButton);
    buttonLayout->addWidget(m_saveQRButton);
    buttonLayout->addStretch();

    gridLayout->addLayout(buttonLayout, row, 0, 1, 2);
    row++;

    // Button box (exactly like DGB's buttonBox - gets green checkmark styling)
    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    gridLayout->addWidget(buttonBox, row, 0, 1, 2);

    setLayout(gridLayout);
}

void DigiDollarReceiveRequestDialog::setModel(WalletModel *model)
{
    m_model = model;
    applyTheme();
    updateUriContent();
    updateDisplayUnit();
}

void DigiDollarReceiveRequestDialog::setInfo(const SendCoinsRecipient &info)
{
    m_info = info;

    // Set window title (like DGB: "Request payment to <address>...")
    QString title = tr("Request payment to %1").arg(info.address);
    setWindowTitle(title);

    // Generate URI
    QString uri = formatDDURI(info);

    // Set QR code
    if (m_qrWidget->setQR(uri, info.address)) {
        m_saveQRButton->setEnabled(true);
    } else {
        m_saveQRButton->setEnabled(false);
    }

    updateUriContent();

    // Set address (plain text, like DGB)
    m_addressContent->setText(info.address);

    // Set amount (or hide if not specified)
    if (info.amount > 0) {
        m_amountContent->setText(formatDDAmount(info.amount));
        m_amountTagLabel->setVisible(true);
        m_amountContent->setVisible(true);
    } else {
        m_amountTagLabel->setVisible(false);
        m_amountContent->setVisible(false);
    }

    // Set label (or hide if not specified)
    if (!info.label.isEmpty()) {
        m_labelContent->setText(info.label);
        m_labelTagLabel->setVisible(true);
        m_labelContent->setVisible(true);
    } else {
        m_labelTagLabel->setVisible(false);
        m_labelContent->setVisible(false);
    }

    // Set message (or hide if not specified)
    if (!info.message.isEmpty()) {
        m_messageContent->setText(info.message);
        m_messageTagLabel->setVisible(true);
        m_messageContent->setVisible(true);
    } else {
        m_messageTagLabel->setVisible(false);
        m_messageContent->setVisible(false);
    }

    // Set wallet name (or hide if not available)
    if (m_model && !m_model->getWalletName().isEmpty()) {
        m_walletContent->setText(m_model->getWalletName());
        m_walletTagLabel->setVisible(true);
        m_walletContent->setVisible(true);
    } else {
        m_walletTagLabel->setVisible(false);
        m_walletContent->setVisible(false);
    }

    // Show verify button if external signer available
    if (m_model) {
        m_verifyButton->setVisible(m_model->wallet().hasExternalSigner());
        if (m_verifyButton->isVisible()) {
            connect(m_verifyButton, &QPushButton::clicked, [this] {
                m_model->displayAddress(m_info.address.toStdString());
            });
        }
    }
}

void DigiDollarReceiveRequestDialog::updateDisplayUnit()
{
    if (m_model && m_info.amount > 0) {
        m_amountContent->setText(formatDDAmount(m_info.amount));
    }
}

void DigiDollarReceiveRequestDialog::applyTheme()
{
    setStyleSheet(DigiDollarReceiveRequestDialogStyleSheet(
        UseDarkDigiDollarReceiveRequestTheme(m_model, this)));
}

QString DigiDollarReceiveRequestDialog::linkColor() const
{
    return UseDarkDigiDollarReceiveRequestTheme(m_model, this)
        ? QStringLiteral("#ffffff")
        : QStringLiteral("#123f2b");
}

void DigiDollarReceiveRequestDialog::updateUriContent()
{
    if (m_info.address.isEmpty()) return;

    const QString uri = formatDDURI(m_info);
    m_uriContent->setText(QStringLiteral("<a style=\"color: %1;\" href=\"%2\">%3</a>")
                              .arg(linkColor(), GUIUtil::HtmlEscape(uri), GUIUtil::HtmlEscape(uri)));
}

void DigiDollarReceiveRequestDialog::onCopyURIClicked()
{
    QString uri = formatDDURI(m_info);
    GUIUtil::setClipboard(uri);
}

void DigiDollarReceiveRequestDialog::onCopyAddressClicked()
{
    GUIUtil::setClipboard(m_info.address);
}

void DigiDollarReceiveRequestDialog::onSaveQRClicked()
{
    QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString defaultFileName = defaultPath + "/digidollar-request-" + m_info.address.left(8) + ".png";

    QString fileName = QFileDialog::getSaveFileName(this, tr("Save QR Code"),
                                                    defaultFileName, tr("PNG Image (*.png)"));
    if (fileName.isEmpty()) {
        return;
    }

    QString error;
    if (!saveQRImageToFile(fileName, error)) {
        QMessageBox::critical(this, tr("QR Code Save Failed"), error);
    }
}

bool DigiDollarReceiveRequestDialog::saveQRImageToFile(const QString& fileName, QString& error) const
{
    error.clear();
    QImage qrImage = m_qrWidget->exportImage();
    if (!qrImage.save(fileName, "PNG")) {
        error = tr("Failed to save QR code to:\n%1").arg(fileName);
        return false;
    }
    return true;
}

QString DigiDollarReceiveRequestDialog::saveQRImageForTesting(const QString& fileName) const
{
    QString error;
    saveQRImageToFile(fileName, error);
    return error;
}

QString DigiDollarReceiveRequestDialog::formatDDAmount(CAmount amount) const
{
    // Amount is stored in cents for DD (100 cents = 1 DD)
    double ddAmount = amount / 100.0;
    return QString::number(ddAmount, 'f', 2) + " $DD";
}

QString DigiDollarReceiveRequestDialog::formatDDURI(const SendCoinsRecipient &info) const
{
    QString uri = "digidollar:" + info.address;
    QStringList params;

    if (!info.label.isEmpty()) {
        params << "label=" + QString(QUrl::toPercentEncoding(info.label));
    }
    if (info.amount > 0) {
        // Amount in DD (cents / 100)
        double ddAmount = info.amount / 100.0;
        params << "amount=" + QString::number(ddAmount, 'f', 8);
    }
    if (!info.message.isEmpty()) {
        params << "message=" + QString(QUrl::toPercentEncoding(info.message));
    }

    if (!params.isEmpty()) {
        uri += "?" + params.join("&");
    }

    return uri;
}
