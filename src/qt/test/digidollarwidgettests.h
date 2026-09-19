// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_TEST_DIGIDOLLARWIDGETTESTS_H
#define DIGIBYTE_QT_TEST_DIGIDOLLARWIDGETTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
}

class DigiDollarWidgetTests : public QObject
{
public:
    explicit DigiDollarWidgetTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void overviewWidgetTests();
    void overviewPaymasterReservationUsesConditionalBalanceBreakdown();
    void watchOnlyDigiDollarBalanceHiddenInWalletModel();
    void privateKeyDisabledWalletCannotGenerateDigiDollarAddress();
    void mintWidgetTests();
    void mintWidgetUsesChainParamMintLimits();
    void mintConfirmationCopyExplainsConfirmationBuffer();
    void mintWidgetCollateralMatchesBuilderSafetyMargin();
    void qtMintStoresDescriptorRecoverableOwnerKey();
    void staleMintUnlockHeightCacheRepairsFromOpReturn();
    void qtFailedMintAbandonsRejectedDraft();
    void sendWidgetTests();
    void sendSuccessDialogDoesNotPromiseNextBlockConfirmation();
    void sendWidgetCoinControlLabelsMirrorDgb();
    void sendWidgetCoinControlDialogSelectionFeedsSend();
    void sendWidgetNoteFieldTests();
    void receiveWidgetTests();
    void redeemWidgetTests();
    void positionsWidgetTests();
    void positionsWidgetHiddenDoesNotPollWallet();
    void addressBookTests();
    void transactionsWidgetTests();
    void transactionsWidgetExportTests();
    void privacyTabSetPrivacySlotTests();
    void privacyOverviewMaskTests();
    void overviewUsdValueShowsUsdSuffixWhenPrivacyOff();
    void digiDollarAmountLabelsUseCurrencyPrefix();
    void digiDollarPersistentStatusesUseSharedSemantics();
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
    void overviewPrivacyMaskHidesAmountUnits();
    void overviewLayoutStretchFavorsBlockchainTotals();
    void overviewBlockchainTotalsFitLaunchScaleValues();
    void overviewHealthUsesCollateralizedLanguage();
    void overviewSystemHealthRpcPollingIsThrottled();
    void overviewPendingBalanceHasThemeRules();
    void digiDollarSectionUsesGreenThemeRules();
    void digiDollarModalDialogsUseGreenThemeRules();
    void digiDollarModalDialogsOverrideDgbBlueFallback();
    void digiDollarModalDialogsVisualQaDarkAndLight();
    void overviewRecentTransactionAmountIsRightAligned();
    void overviewRecentTransactionDoubleClickOpensTransactionsTab();
    void transactionsWidgetDoubleClickShowsDetailsDialog();
    void transactionsWidgetDetailsDialogOverridesDgbBlueDialogFallback();
    void transactionsWidgetDetailsDialogVisualQaDarkAndLight();
    void transactionsWidgetDetailsDialogHasDigiDollarThemeRules();
    void privacySendMaskTests();
    void privacyMintMaskTests();
    void privacyRedeemMaskTests();
    void privacyPositionsMaskTests();
    void privacyTransactionsMaskTests();
    void privacySignalPropagationTests();
    void mintValidationUpdatesOnBalanceChange();
    void ddTabRefreshesBalancesOnWalletSignal();
    void ddTabRefreshDoesNotWaitForBusyWallet();
    void ddTabLoadsSelectedPageOnFirstShow();
    void transactionsWidgetShowsStoredHistoryWhileWalletBusy();
    void transactionsWidgetRefreshesOnDigiDollarSignal();
    void walletViewRefreshesDigiDollarPageOnOpen();
    void ddReceivePanelFollowsSelectedRow();
    void ddReceiveDoubleClickShowsRequestDialog();
    void ddReceiveHidesCrossNetworkRequests();
    void ddReceiveEditPersistsAndKeepsDgbSeparated();
    void ddReceiveEditCancelLeavesRequestUnchanged();
    void ddReceiveRemovePersistsAndKeepsDgbSeparated();
    void ddReceiveRequestDialogFormatsURIAndAmount();
    void ddReceiveRejectsMalformedRequestAmount();
    void ddReceiveRequestDialogReportsQRSaveFailure();
    void darkThemePeerDetailWidgetHasExplicitRule();
    void darkThemeDigiDollarSendTotalLabelHasReadableContrast();
    void darkThemeShutdownWindowHasReadableSurface();
    void globalTooltipFilterHandlesNativeTooltipEvents();
    void globalTooltipVisualContrastRendersReadablePixels();
    void customTooltipRenderersNormalizeQtRichTextEnvelope();
    void overviewRecentTransactionsSendShowsNegativeSign();
    void transactionsWidgetShowsRpcHistorySignsAndFields();
    void positionsWidgetLockTierColumnFitsLongestLabel();
    void positionsWidgetSortingKeepsHealthAndActionsOnSameRow();
    void positionsWidgetInitialLoadNotThrottled();
    void positionsWidgetHealthUsesMicroUsdOraclePrice();
    void positionsWidgetDisablesRedeemForPrivateKeyDisabledWallet();
    void positionsWidgetEnablesRedeemForLockedEncryptedWallet();
    void redeemWidgetButtonStateNoSelection();
    void redeemWidgetButtonStateTimelockActive();
    void redeemWidgetButtonStateInvalidAmount();
    void redeemWidgetButtonStateInsufficientDDBalance();
    void redeemWidgetButtonStatePrivateKeyDisabledWallet();
    void redeemWidgetButtonStateLockedWallet();
    void redeemWidgetLockedWalletClickRequestsUnlock();
    void redeemWidgetCancelledUnlockLeavesFormUnchanged();
    void redeemWidgetRefreshesWhenWalletUnlocks();
    void redeemWidgetButtonStateReady();
    void redeemWidgetCanonicalHealthDoesNotRequireCirculatingSupply();
    void positionsWidgetLockedTooltipShowsRemainingBlocksAndTime();
    void positionsWidgetPendingMintButtonNotRedeemed();
    void positionsWidgetPendingRedeemButtonNotRedeemed();
    void redeemWidgetKeepsTimelockedPositionDisabled();
    void mintDigiDollarRejectsPrivateKeyDisabledWallet();
    void mintWidgetRejectsUnsupportedWalletBeforeConfirmation_data();
    void mintWidgetRejectsUnsupportedWalletBeforeConfirmation();
    void mintWidgetPreservesEncryptedWalletUnlockFlow();
    void redeemResultAlwaysReachesTheUser();
    void transactionsConfirmationsColumnIsAlwaysACount();
    void sendWidgetSaysWhyAnAmountIsRefused();
};

#endif // DIGIBYTE_QT_TEST_DIGIDOLLARWIDGETTESTS_H
