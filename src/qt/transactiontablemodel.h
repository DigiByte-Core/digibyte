// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_QT_TRANSACTIONTABLEMODEL_H
#define DIGIBYTE_QT_TRANSACTIONTABLEMODEL_H

#include <qt/digibyteunits.h>

#include <QAbstractTableModel>
#include <QStringList>

#include <memory>

namespace interfaces {
class Handler;
}

class PlatformStyle;
class TransactionRecord;
class TransactionTablePriv;
class WalletModel;

/** Wording the wallet uses for DigiDollar rows. The main transaction history
    and the DigiDollar tab both call these so they say the same thing. */
namespace DigiDollarLabels {
/** Name for DigiDollars handed back by a redemption. */
QString ChangeReturned();
/** Why those DigiDollars came back. */
QString ChangeReturnedExplanation();
} // namespace DigiDollarLabels


/** UI model for the transaction table of a wallet.
 */
class TransactionTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit TransactionTableModel(const PlatformStyle *platformStyle, WalletModel *parent = nullptr);
    ~TransactionTableModel();

    enum ColumnIndex {
        Status = 0,
        Watchonly = 1,
        Date = 2,
        Type = 3,
        ToAddress = 4,
        //! DigiByte amount of the row.
        Amount = 5,
        //! DigiDollar amount of the row, in dollars and cents. A row carries a
        //! DigiByte amount or a DigiDollar amount, so only one of these two
        //! columns holds a number on any given row.
        AmountDD = 6
    };

    /** Roles to get specific information from a transaction row.
        These are independent of column.
    */
    enum RoleIndex {
        /** Type of transaction */
        TypeRole = Qt::UserRole,
        /** Date and time this transaction was created */
        DateRole,
        /** Watch-only boolean */
        WatchonlyRole,
        /** Watch-only icon */
        WatchonlyDecorationRole,
        /** Long description (HTML format) */
        LongDescriptionRole,
        /** Address of transaction */
        AddressRole,
        /** Label of address related to transaction */
        LabelRole,
        /** Net DigiByte amount of transaction */
        AmountRole,
        /** DigiDollar amount of transaction, in cents */
        AmountDDRole,
        /** Transaction hash */
        TxHashRole,
        /** Transaction data, hex-encoded */
        TxHexRole,
        /** Whole transaction as plain text */
        TxPlainTextRole,
        /** Is transaction confirmed? */
        ConfirmedRole,
        /** Formatted DigiByte amount, without brackets when unconfirmed */
        FormattedAmountRole,
        /** Formatted DigiDollar amount, without brackets when unconfirmed */
        FormattedAmountDDRole,
        /** Transaction status (TransactionRecord::Status) */
        StatusRole,
        /** Unprocessed icon */
        RawDecorationRole,
    };

    /** Text for the type column. */
    static QString formatTxType(const TransactionRecord *wtx);
    /** Icon resource for the type of a row. */
    static QString txTypeIconPath(const TransactionRecord *wtx);
    /** Text for the DigiByte amount column. Empty on a row that carries a
        DigiDollar amount instead, so the two columns never repeat each other. */
    static QString formatAmountDGB(const TransactionRecord *wtx, DigiByteUnit unit, bool showUnconfirmed=true, DigiByteUnits::SeparatorStyle separators=DigiByteUnits::SeparatorStyle::STANDARD);
    /** Text for the DigiDollar amount column. Empty on a row that carries no
        DigiDollars. */
    static QString formatAmountDD(const TransactionRecord *wtx, bool showUnconfirmed=true);

    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex & parent = QModelIndex()) const override;
    bool processingQueuedTransactions() const { return fProcessingQueuedTransactions; }

private:
    WalletModel *walletModel;
    std::unique_ptr<interfaces::Handler> m_handler_transaction_changed;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    QStringList columns;
    TransactionTablePriv *priv;
    bool fProcessingQueuedTransactions{false};
    const PlatformStyle *platformStyle;

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

    QString lookupAddress(const std::string &address, bool tooltip) const;
    QVariant addressColor(const TransactionRecord *wtx) const;
    QString formatTxStatus(const TransactionRecord *wtx) const;
    QString formatTxDate(const TransactionRecord *wtx) const;
    QString formatTxToAddress(const TransactionRecord *wtx, bool tooltip) const;
    QString formatTooltip(const TransactionRecord *rec) const;
    QVariant txStatusDecoration(const TransactionRecord *wtx) const;
    QVariant txWatchonlyDecoration(const TransactionRecord *wtx) const;
    QVariant txAddressDecoration(const TransactionRecord *wtx) const;

public Q_SLOTS:
    /* New transaction, or transaction changed status */
    void updateTransaction(const QString &hash, int status, bool showTransaction);
    void updateConfirmations();
    void updateDisplayUnit();
    /** Updates the DigiByte amount column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
    void updateAmountColumnTitle();
    /* Needed to update fProcessingQueuedTransactions through a QueuedConnection */
    void setProcessingQueuedTransactions(bool value) { fProcessingQueuedTransactions = value; }

    friend class TransactionTablePriv;
};

#endif // DIGIBYTE_QT_TRANSACTIONTABLEMODEL_H
