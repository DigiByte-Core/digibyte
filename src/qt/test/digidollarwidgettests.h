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
    void positionsWidgetKeepsRedeemActionableForLockedEncryptedWallet();
    void redeemWidgetButtonStateNoSelection();
    void redeemWidgetButtonStateTimelockActive();
    void redeemWidgetButtonStateInvalidAmount();
    void redeemWidgetButtonStateInsufficientDDBalance();
    void redeemWidgetButtonStatePrivateKeyDisabledWallet();
    void redeemWidgetButtonStateLockedWalletOffersUnlock();
    void redeemWidgetLockedWalletRequestsUnlockOnRedeem();
    void widgetsDisableRedeemForEncryptedWatchOnlyWallet();
    void redeemWidgetRefreshesWhenWalletUnlocks();
    void redeemWidgetButtonStateReady();
    void positionsWidgetLockedTooltipShowsRemainingBlocksAndTime();
    void positionsWidgetPendingMintButtonNotRedeemed();
    void positionsWidgetPendingRedeemButtonNotRedeemed();
    void redeemWidgetKeepsTimelockedPositionDisabled();
    void mintDigiDollarRejectsPrivateKeyDisabledWallet();
};

#endif // DIGIBYTE_QT_TEST_DIGIDOLLARWIDGETTESTS_H
