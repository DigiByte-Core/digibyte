// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARRECEIVEWIDGET_H
#define DIGIBYTE_QT_DIGIDOLLARRECEIVEWIDGET_H

#include <qt/sendcoinsrecipient.h>

#include <QWidget>

class WalletModel;
class ClientModel;
class QRImageWidget;
class RecentRequestEntry;

QT_BEGIN_NAMESPACE
class QDateTime;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QGridLayout;
class QFrame;
class QToolButton;
class QTableWidget;
class QTableWidgetItem;
class QMenu;
QT_END_NAMESPACE

/**
 * DigiDollar receive widget for generating DD addresses and payment requests.
 * This widget provides functionality to receive DigiDollar with QR code generation,
 * address management, and payment request tracking.
 */
class DigiDollarReceiveWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DigiDollarReceiveWidget(QWidget *parent = nullptr);
    ~DigiDollarReceiveWidget();

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);
    void updateView();

Q_SIGNALS:
    /** Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);

public Q_SLOTS:
    /** Update recent requests display */
    void updateRecentRequests();
    /** Apply current theme to all UI elements */
    void applyTheme();
    /** Set monospaced font for address labels */
    void setMonospacedFont(bool use_embedded_font);

private Q_SLOTS:
    /** Generate new DD address */
    void onGenerateAddressClicked();
    /** Copy address to clipboard */
    void onCopyAddressClicked();
    /** Copy QR code image to clipboard */
    void onCopyQRClicked();
    /** Save QR code as image */
    void onSaveQRClicked();
    /** Clear all input fields */
    void onClearClicked();
    /** Label field changed */
    void onLabelChanged();
    /** Amount field changed */
    void onAmountChanged();
    /** Message field changed */
    void onMessageChanged();
    /** Recent request row selected */
    void onRecentRequestSelected();
    /** Remove selected request */
    void onRemoveRequestClicked();
    /** Edit selected request */
    void onEditRequestClicked();
    /** Show selected request */
    void onShowRequestClicked();
    /** Recent request row double-clicked */
    void onRecentRequestDoubleClicked(int row, int column);
    /** Show context menu for requests table */
    void showContextMenu(const QPoint &point);
    /** Context menu action: copy URI */
    void copyURI();
    /** Context menu action: copy address */
    void copyAddress();
    /** Context menu action: copy label */
    void copyLabel();
    /** Context menu action: copy message */
    void copyMessage();
    /** Context menu action: copy amount */
    void copyAmount();
    /** Context menu action: edit request */
    void editRequest();

private:
    void setupUI();
    void setupGenerateSection();
    void setupQRSection();
    void setupRecentRequestsSection();
    void connectSignals();
    void updateQRCode();
    void clearFields();
    void generateNewAddress();
    void populateRecentRequests();
    void addRequestToTable(const QDateTime& date, const QString& label,
                          const QString& amount, const QString& address, qint64 id = 0);

    QString formatDDAmount(double amount) const;
    QString formatDDURI(const QString& address, const QString& label = QString(),
                       const QString& amount = QString(), const QString& message = QString()) const;
    int selectedRow() const;
    bool getSelectedRequest(RecentRequestEntry& entry) const;
    bool findDigiDollarRequest(const QString& address, RecentRequestEntry& entry) const;
    bool updateDigiDollarRequest(const RecentRequestEntry& entry);
    bool removeDigiDollarRequest(const QString& address);
    bool editDigiDollarRequest(int row);
    SendCoinsRecipient recipientFromRow(int row) const;
    QString addressFromRow(int row) const;

    // UI components
    QMenu* m_contextMenu;
    QVBoxLayout* m_mainLayout;

    // Generate section
    QFrame* m_generateFrame;
    QGridLayout* m_generateLayout;
    QLabel* m_labelLabel;
    QLineEdit* m_labelEdit;
    QLabel* m_amountLabel;
    QLineEdit* m_amountEdit;
    QLabel* m_messageLabel;
    QLineEdit* m_messageEdit;
    QPushButton* m_generateButton;
    QPushButton* m_clearButton;

    // QR code section
    QFrame* m_qrFrame;
    QVBoxLayout* m_qrLayout;
    QLabel* m_emptyStateLabel;
    QLabel* m_qrTitle;
    QRImageWidget* m_qrImage;
    QLabel* m_addressLabel;
    QLineEdit* m_addressEdit;
    QHBoxLayout* m_qrButtonLayout;
    QPushButton* m_copyAddressButton;
    QPushButton* m_copyQRButton;
    QPushButton* m_saveQRButton;

    // Recent requests section
    QFrame* m_requestsFrame;
    QVBoxLayout* m_requestsLayout;
    QLabel* m_requestsTitle;
    QTableWidget* m_requestsTable;
    QHBoxLayout* m_requestsButtonLayout;
    QPushButton* m_showRequestButton;
    QPushButton* m_editRequestButton;
    QPushButton* m_removeRequestButton;
    QLabel* m_noRequestsLabel;

    // Models
    WalletModel* m_walletModel;
    ClientModel* m_clientModel;

    // Current request data
    QString m_currentAddress;
    QString m_currentLabel;
    QString m_currentAmount;
    QString m_currentMessage;
};

#endif // DIGIBYTE_QT_DIGIDOLLARRECEIVEWIDGET_H
