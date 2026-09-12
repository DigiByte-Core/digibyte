// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for the order in which a mint started from the wallet window saves
// its records and sends its transaction. They live in their own translation
// unit so that work on the mint path does not collide with the larger
// digidollarwidgettests.cpp.

#ifndef DIGIBYTE_QT_TEST_DIGIDOLLARMINTRECORDTESTS_H
#define DIGIBYTE_QT_TEST_DIGIDOLLARMINTRECORDTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
}

class DigiDollarMintRecordTests : public QObject
{
public:
    explicit DigiDollarMintRecordTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void mintSavesItsRecordBeforeSendingTheTransaction();
    void mintDoesNotSendWhenTheWalletCannotSaveIt();
};

#endif // DIGIBYTE_QT_TEST_DIGIDOLLARMINTRECORDTESTS_H
