// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_TEST_DDTRANSACTIONTABLETESTS_H
#define DIGIBYTE_QT_TEST_DDTRANSACTIONTABLETESTS_H

#include <interfaces/node.h>

#include <QObject>
#include <QTest>

/** Tests for the way the transaction history shows DigiByte and DigiDollar
    amounts.

    The history has one column for DigiByte amounts and one for DigiDollar
    amounts. A row belongs in one column or the other, never both, and the
    exported CSV file has the same two columns. These tests also cover the
    wording the wallet uses for DigiDollars handed back by a redemption. */
class DDTransactionTableTests : public QObject
{
    Q_OBJECT

public:
    explicit DDTransactionTableTests(interfaces::Node& node) : m_node(node) {}

private Q_SLOTS:
    /** A mint, a transfer and a redemption each fill one amount column. */
    void amountColumnsAreSeparate();
    /** The exported CSV file carries both amount columns. */
    void csvExportHasSeparateAmountColumns();
    /** A send that spends the wallet's own DigiDollar input puts its dollars on
        one row and its DigiByte fee on a row of its own. */
    void sendFromOwnTokenShowsTheFeeOnItsOwnRow();
    /** The places with room for one figure show the figure the row has: the
        DigiByte amount, or the dollar amount on a DigiDollar row. */
    void theSingleAmountIsTheOneTheRowHas();
    /** The transaction list opens with the column widths the program chooses,
        and a width the user set is still there next time. */
    void transactionViewAppliesAndRemembersColumnWidths();
    /** The DigiDollar tab calls returned change what it is and explains it. */
    void changeReturnedLabelExplainsItself();
    /** Every DigiDollar row type has a name, an icon and one amount. */
    void digiDollarRowTypesAreLabelled();

private:
    interfaces::Node& m_node;
};

#endif // DIGIBYTE_QT_TEST_DDTRANSACTIONTABLETESTS_H
