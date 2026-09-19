// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_PAYMASTERWIDGET_H
#define DIGIBYTE_QT_PAYMASTERWIDGET_H

#include <QWidget>
#include <univalue.h>

#include <functional>
#include <string>

class QScrollArea;
class WalletModel;

/** Small embedding interface for the wallet-scoped Paymaster operator panel.
 * The implementation owns presentation and asynchronous RPC state; Core owns
 * authorization, coin reservations and persistence. Keeping the panel separate
 * avoids coupling the upstream DigiDollar tab to provider workflows.
 */
class DigiDollarPaymasterWidget : public QWidget
{
public:
    using RpcExecutor = std::function<UniValue(const std::string&, const UniValue&)>;
    using QWidget::QWidget;

    virtual void setWalletModel(WalletModel* model) = 0;
    virtual void refreshStatus() = 0;
    virtual void setPrivacy(bool privacy) = 0;
    virtual void setProviderBackupRequestHandler(std::function<void()> handler) = 0;

    // Deterministic presentation tests inject snapshots or a local RPC executor.
    virtual void setFinanceExportFilenameForTesting(QString filename) = 0;
    virtual void setLiquidityPoolForTesting(const UniValue& pool_info) = 0;
    virtual void setLiquidityStatusForTesting(const UniValue& status) = 0;
    virtual void setMutationSnapshotsAvailableForTesting(bool provider_info, bool provider_safety, bool liquidity) = 0;
    virtual void setReadinessStatusForTesting(const UniValue& status) = 0;
    virtual void setRpcExecutorForTesting(RpcExecutor executor) = 0;
    virtual void setStartResultForTesting(const UniValue& result) = 0;
};

/** The returned panel is owned by its Qt parent. */
DigiDollarPaymasterWidget* CreatePaymasterWidget(QWidget* parent);
void ConfigurePaymasterScrollArea(QScrollArea* scroll, QWidget* contents);

#endif // DIGIBYTE_QT_PAYMASTERWIDGET_H
