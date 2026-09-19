// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Wave 19 Agent B (Functional/test auditor) — Qt unit/signal-slot pins
// covering the Wave 19 release-critical UX surface (mint tier dropdown,
// redeem widget tier display, transactions widget filter, coin-control
// DD-locked filter). Lives in its own translation unit to stay clear of
// other Wave 19 agents touching `digidollarwidgettests.{h,cpp}`.

#ifndef DIGIBYTE_QT_TEST_DIGIDOLLARWAVE19WIDGETTESTS_H
#define DIGIBYTE_QT_TEST_DIGIDOLLARWAVE19WIDGETTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
}

class DigiDollarWave19WidgetTests : public QObject
{
public:
    explicit DigiDollarWave19WidgetTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    // DD-FA-FUNC-030: redeem-widget tier label parity
    void redeemWidgetLockTierShowsHumanReadableLabel();
    // DD-FA-TEST-027: tier dropdown coverage
    void mintWidgetTierComboShowsAllCanonicalTiers();
    void mintWidgetTierChangeRefreshesCollateralRatio();
    void positionsWidgetTierZeroTooltipIsCanonical();
    void overviewWidgetUsesProtocolAcronymTooltips();
    void walletModelAndSendValidatorRejectCrossNetworkDDAddresses();
    void sendAddressValidatorTakesAnAddressTypedOneCharacterAtATime();
    void sendAmountValidatorUsesCentsPrecisionAndBounds();
    void mintWidgetUsdEquivalentUsesCentsPrecision();
    void positionsWidgetMissingOracleHealthIsUnavailable();
    // DD-FA-TEST-028: transactions widget filter coverage
    void transactionsWidgetTypeFilterFiltersRows();
    void transactionsWidgetSearchFilterMatchesByTxid();
    void transactionsWidgetPreservesUserSortAcrossRefresh();
    // DD-FA-TEST-029: coin control DD-locked filter
    void coinControlDialogShowsOnlySpendableDDUtxos();
};

#endif // DIGIBYTE_QT_TEST_DIGIDOLLARWAVE19WIDGETTESTS_H
