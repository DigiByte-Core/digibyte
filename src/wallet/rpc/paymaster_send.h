// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_RPC_PAYMASTER_SEND_H
#define DIGIBYTE_WALLET_RPC_PAYMASTER_SEND_H

#include <consensus/amount.h>
#include <paymaster/types.h>
#include <primitives/transaction.h>
#include <univalue.h>

#include <optional>
#include <string>
#include <vector>

class CDigiDollarAddress;
class DigiDollarWallet;
class JSONRPCRequest;

namespace wallet {
class CWallet;

/** Parsed seventh argument of senddigidollar; not persisted session state. */
struct PaymasterSendOptions {
    DigiDollar::Paymaster::FeeMode fee_mode{DigiDollar::Paymaster::FeeMode::DGB};
    bool subtract_paymaster_fee_from_amount{false};
    bool send_all_spendable_dd{false};
    UniValue send_options{UniValue::VOBJ};
};

PaymasterSendOptions ParsePaymasterSendOptions(const JSONRPCRequest& request);

/** Resume a durable session before selecting inputs, then enforce the existing
 * balance and fee-funding checks. A result completes this RPC through Paymaster;
 * nullopt continues the ordinary DGB-funded transfer. The selected-input buffer
 * belongs to the caller and remains alive through that transfer.
 * Call after wallet synchronization and ordinary address/amount validation.
 */
std::optional<UniValue> PrepareDigiDollarFeeFunding(
    const JSONRPCRequest& request, CWallet& wallet, DigiDollarWallet& dd_wallet,
    const std::string& addressStr, const CDigiDollarAddress& dd_address, CAmount amount,
    const PaymasterSendOptions& options, std::vector<COutPoint>& selected_inputs,
    const std::vector<COutPoint>*& preset_dd_inputs);
} // namespace wallet

#endif // DIGIBYTE_WALLET_RPC_PAYMASTER_SEND_H
