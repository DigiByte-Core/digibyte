// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Wallet Paymaster RPC registration and automatic-service entry points. */

#ifndef DIGIBYTE_WALLET_RPC_PAYMASTER_H
#define DIGIBYTE_WALLET_RPC_PAYMASTER_H

#include <consensus/amount.h>
#include <paymaster/types.h>
#include <primitives/transaction.h>

#include <cstddef>
#include <string>
#include <vector>

class JSONRPCRequest;
class RPCHelpMan;
class UniValue;

namespace wallet {
class CWallet;
class PaymasterStore;
struct WalletContext;

/** Fail closed unless the provider-block record is definitively absent. A
 * malformed or unreadable record is a wallet database error, not evidence
 * that the provider is eligible. */
bool EnsureProviderNotEquivocationBlocked(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    std::string& error);

/** Persist every verifiable signed conflict currently waiting for this
 * wallet. This is invoked periodically so evidence does not depend on a user
 * opening an RPC or Qt flow inside the bounded network-inbox TTL. */
bool DrainPaymasterEquivocationInbox(WalletContext& context,
                                     CWallet& wallet,
                                     std::string& error);

/** Reconcile restartable paid maintenance and carrier-withdrawal records with
 * wallet transactions. Safe to invoke repeatedly at startup and at each
 * periodic wallet-maintenance tick. */
bool ReconcilePaymasterProviderMaintenance(CWallet& wallet,
                                           size_t& recovered,
                                           std::string& error);

/** Rebuild provider income and cost state from durable commits, maintenance
 * records, and current wallet transaction state. Safe across replay, reorg,
 * and restart. */
bool ReconcilePaymasterProviderFinances(CWallet& wallet,
                                        size_t& changed_events,
                                        std::string& error);

/** Restore client-facing DD send rows for successful Paymaster sessions made
 * by older wallet versions. This changes display history only; authoritative
 * session, transaction, reservation, and accounting records are untouched. */
bool ReconcilePaymasterClientHistory(CWallet& wallet, std::string& error);

/** Continue explicitly authorized setup even while service is stopped. */
void RunPaymasterPoolPreparation(WalletContext& context, CWallet& wallet);

/** Run one bounded provider-service cycle. This is the single scheduler entry
 * point for optional autostart plus automatic request/submit processing. */
void RunPaymasterProviderServiceCycle(WalletContext& context, CWallet& wallet);

UniValue RequestAutomaticPaymasterQuote(const JSONRPCRequest& request,
                                        const std::string& address,
                                        CAmount amount,
                                        const UniValue& options,
                                        const std::vector<COutPoint>* preset_inputs);
RPCHelpMan getdigidollarsendsession();
RPCHelpMan listdigidollarsendsessions();
RPCHelpMan getpaymasterreputation();
RPCHelpMan clearpaymasterreputation();
RPCHelpMan getpaymasteroffers();
RPCHelpMan requestpaymasterquote();
RPCHelpMan resolvepaymastersession();
RPCHelpMan walletprocesspaymasterpsbt();
RPCHelpMan submitpaymasterdigidollar();
RPCHelpMan createpaymasteridentity();
RPCHelpMan createrestrictedpaymasterdescriptor();
RPCHelpMan setpaymasterpolicy();
RPCHelpMan setpaymastersafetypolicy();
RPCHelpMan getpaymastersafetystatus();
RPCHelpMan setpaymasterclientsafetypolicy();
RPCHelpMan getpaymasterclientsafetystatus();
RPCHelpMan setpaymasterenabled();
RPCHelpMan setpaymasterruntimesettings();
RPCHelpMan setpaymasterliquiditypolicy();
RPCHelpMan getpaymasterliquiditystatus();
RPCHelpMan getpaymasterfinancestatus();
RPCHelpMan acknowledgepaymasterproviderbackup();
RPCHelpMan withdrawpaymastercarrier();
RPCHelpMan preparepaymasterpool();
RPCHelpMan rebalancepaymasterpool();
RPCHelpMan getpaymasterinfo();
RPCHelpMan getpaymasterpoolinfo();
RPCHelpMan listpaymasterreservations();
RPCHelpMan cancelpaymasterquote();
RPCHelpMan processpaymasterrequests();
RPCHelpMan processpaymastersubmits();
RPCHelpMan processpaymasterresult();
RPCHelpMan startpaymaster();
RPCHelpMan stoppaymaster();
} // namespace wallet

#endif // DIGIBYTE_WALLET_RPC_PAYMASTER_H
