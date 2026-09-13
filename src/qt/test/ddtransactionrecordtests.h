// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for the rows the wallet's transaction list builds from a DigiDollar
// transaction, and for the text of the details window that opens when a row
// is double-clicked.

#ifndef DIGIBYTE_QT_TEST_DDTRANSACTIONRECORDTESTS_H
#define DIGIBYTE_QT_TEST_DDTRANSACTIONRECORDTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
}

class DDTransactionRecordTests : public QObject
{
public:
    explicit DDTransactionRecordTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    // A mint must show the DigiDollars it created, the DGB it locked, and the
    // fee, each once.
    void mintRowsShowMintedAmountCollateralAndFeeOnce();
    // The details window for a mint must name the DigiDollar amount, the
    // locked collateral, the lock period, the unlock height and the vault.
    void mintDetailsShowDigiDollarFacts();
    void mintDetailsFindCollateralInAnyOutput_data();
    void mintDetailsFindCollateralInAnyOutput();
    // A transfer must show the DigiDollars on one row and the DigiByte fee on
    // a row of its own, whichever of the two ways the wallet breaks it up.
    void transferRowsShowDollarsAndFeeSeparately();
    // A transfer's fee row must not be given the number of a real row in the
    // same transaction.
    void transferFeeRowNumberCannotClashWithARealRow();
    // The details window for a transfer must name the DigiDollar amounts.
    void transferDetailsShowDigiDollarAmounts();
    // A redemption keeps its two DGB rows and gains one row for DigiDollars
    // handed back as change.
    void redeemRowsKeepDgbRowsAndAddReturnedChange();
    // The details window for a redemption must name the returned collateral,
    // the DigiDollar change and the vault that was closed.
    void redeemDetailsShowDigiDollarFacts();
};

#endif // DIGIBYTE_QT_TEST_DDTRANSACTIONRECORDTESTS_H
