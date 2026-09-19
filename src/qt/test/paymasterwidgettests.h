// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_TEST_PAYMASTERWIDGETTESTS_H
#define DIGIBYTE_QT_TEST_PAYMASTERWIDGETTESTS_H

#include <QObject>

namespace interfaces { class Node; }

class PaymasterWidgetTests : public QObject
{
    Q_OBJECT
public:
    explicit PaymasterWidgetTests(interfaces::Node& node) : m_node(node) {}

private Q_SLOTS:
    void paymasterLiquidityPolicyDefaultsAndApprovalGuard();
    void paymasterLiquidityPolicySavePersistsVisibleValues();
    void paymasterLiquidityMaintenanceStatesAreReadable();
    void paymasterExternalReadinessIsSeparatedFromConfiguration();
    void paymasterPendingStartUsesLiveStatusInsteadOfStaleModal();
    void paymasterCarrierWithdrawalActionsFailClosed();
    void paymasterCarrierWithdrawalPreviewsArePlanBound();
    void paymasterSafetyControlsDefaultFailClosed();
    void paymasterSafetyPolicyDisplaysFiniteDisabledSemantics();
    void paymasterInjectedRpcCoversConfigurationWorkflows();
    void paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows();
    void paymasterManualActivityUsesExpectedRpcAndReadableStates();
    void paymasterFinancesAndBackupWorkflow();
    void paymasterConfirmationGuardDetectsMaterialChanges();
    void paymasterRecoveryConfirmationGuardDetectsMaterialChanges();
    void paymasterClientOfferPreviewUsesExactRpcAndPlainText();
    void paymasterClientOfferPreviewInvalidatesAndHandlesFailures();
    void paymasterClientAuthorizationIsTwoStageAndFailClosed();
    void paymasterClientSessionActionMatrix_data();
    void paymasterClientSessionActionMatrix();
    void paymasterClientSessionRpcActionsAreBound();
    void paymasterClientRestartCreatedSessionWithoutRecipientIsReadOnly();
    void paymasterClientMultipleRestartSessionsRequireSelection();
    void paymasterClientRecoveryExpiryIsFailClosed();
    void paymasterClientDelayedCallbackIgnoresWalletClose();
    void paymasterClientUnlockLeaseSurvivesWalletModelClose();
    void paymasterClientDatabaseReadErrorIsActionable();
    void paymasterClientControlsAreAccessible();
    void paymasterOfferTableRendersGreenTheme();
    void paymasterGuidedSetupRendersConsistentTheme();
    void paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep();

private:
    interfaces::Node& m_node;
};

#endif // DIGIBYTE_QT_TEST_PAYMASTERWIDGETTESTS_H
