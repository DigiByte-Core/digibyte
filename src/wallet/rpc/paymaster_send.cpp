// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/rpc/paymaster_send.h>

#include <key_io.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <sync.h>
#include <tinyformat.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/wallet.h>

#include <utility>

namespace wallet {
PaymasterSendOptions ParsePaymasterSendOptions(const JSONRPCRequest& request)
{
    // Preserve upstream amount_unit at index 5; Paymaster options follow it.
    const bool has_send_options = request.params.size() > 6 && !request.params[6].isNull();
    PaymasterSendOptions options;
    auto& [fee_mode, subtract_paymaster_fee_from_amount, send_all_spendable_dd, send_options] = options;
    if (has_send_options) {
        send_options = request.params[6].get_obj();
        const bool fee_cap_supplied = !send_options.find_value("maximum_paymaster_fee_cents").isNull();
        const bool attempts_supplied = !send_options.find_value("maximum_provider_attempts").isNull();
        const bool privacy_supplied = !send_options.find_value("privacy").isNull();
        const bool selection_supplied = !send_options.find_value("selection").isNull();
        const bool restricted_key_supplied = !send_options.find_value("provider_identity_key").isNull();
        const bool restricted_descriptor_supplied =
            !send_options.find_value("restricted_service_descriptor").isNull();
        const bool restricted_capability_supplied =
            !send_options.find_value("sponsorship_capability").isNull();
        const bool prepare_only_supplied = !send_options.find_value("prepare_only").isNull();
        const bool authorization_commitment_supplied =
            !send_options.find_value("authorization_commitment").isNull();
        const bool subtract_fee_supplied =
            !send_options.find_value("subtract_paymaster_fee_from_amount").isNull();
        const bool send_all_supplied =
            !send_options.find_value("send_all_spendable_dd").isNull();
        const bool restricted_context_supplied = restricted_key_supplied ||
            restricted_descriptor_supplied || restricted_capability_supplied;
        RPCTypeCheckObj(send_options,
                        {{"fee_mode", UniValueType(UniValue::VSTR)},
                         {"request_id", UniValueType(UniValue::VSTR)},
                         {"maximum_paymaster_fee_cents", UniValueType(UniValue::VNUM)},
                         {"maximum_provider_attempts", UniValueType(UniValue::VNUM)},
                          {"privacy", UniValueType(UniValue::VSTR)},
                          {"selection", UniValueType(UniValue::VSTR)},
                          {"provider_identity_key", UniValueType(UniValue::VSTR)},
                          {"restricted_service_descriptor", UniValueType(UniValue::VSTR)},
                          {"sponsorship_capability", UniValueType(UniValue::VSTR)},
                          {"prepare_only", UniValueType(UniValue::VBOOL)},
                          {"authorization_commitment", UniValueType(UniValue::VSTR)},
                          {"subtract_paymaster_fee_from_amount", UniValueType(UniValue::VBOOL)},
                          {"send_all_spendable_dd", UniValueType(UniValue::VBOOL)}},
                        /*fAllowNull=*/true, /*fStrict=*/true);
        const std::string mode = send_options.find_value("fee_mode").isNull()
            ? "auto" : send_options.find_value("fee_mode").get_str();
        if (mode == "dgb") fee_mode = DigiDollar::Paymaster::FeeMode::DGB;
        else if (mode == "paymaster") fee_mode = DigiDollar::Paymaster::FeeMode::PAYMASTER;
        else if (mode == "auto") fee_mode = DigiDollar::Paymaster::FeeMode::AUTO;
        else throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_mode must be dgb, paymaster, or auto");
        if (fee_mode == DigiDollar::Paymaster::FeeMode::DGB &&
            (fee_cap_supplied || attempts_supplied || privacy_supplied || selection_supplied ||
             restricted_key_supplied || restricted_descriptor_supplied ||
             restricted_capability_supplied || prepare_only_supplied ||
             authorization_commitment_supplied || subtract_fee_supplied ||
             send_all_supplied)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "fee_mode dgb does not accept Paymaster authorization or selection fields");
        }
        if (send_options.find_value("fee_mode").isNull()) send_options.pushKV("fee_mode", mode);

        const std::string privacy = send_options.find_value("privacy").isNull()
            ? "standard" : send_options.find_value("privacy").get_str();
        if (privacy != "standard" && privacy != "high") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "privacy must be standard or high");
        }
        if (send_options.find_value("privacy").isNull()) send_options.pushKV("privacy", privacy);
        const int maximum_attempts = send_options.find_value("maximum_provider_attempts").isNull()
            ? ((privacy == "high" || restricted_context_supplied) ? 1 : 3)
            : send_options.find_value("maximum_provider_attempts").getInt<int>();
        if (maximum_attempts < 1 || maximum_attempts > 16 ||
            (privacy == "high" && maximum_attempts != 1)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy");
        }
        if (send_options.find_value("maximum_provider_attempts").isNull()) {
            send_options.pushKV("maximum_provider_attempts", maximum_attempts);
        }
        const std::string selection = send_options.find_value("selection").isNull()
            ? "lowest_total_cost" : send_options.find_value("selection").get_str();
        if (selection != "lowest_total_cost" && selection != "privacy_weighted") {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "selection must be lowest_total_cost or privacy_weighted");
        }
        if (send_options.find_value("selection").isNull()) send_options.pushKV("selection", selection);
        subtract_paymaster_fee_from_amount = subtract_fee_supplied &&
            send_options.find_value("subtract_paymaster_fee_from_amount").get_bool();
        send_all_spendable_dd = send_all_supplied &&
            send_options.find_value("send_all_spendable_dd").get_bool();
        if (send_all_spendable_dd &&
            !subtract_paymaster_fee_from_amount) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "send_all_spendable_dd requires subtract_paymaster_fee_from_amount");
        }
        if (send_options.find_value("subtract_paymaster_fee_from_amount").isNull()) {
            send_options.pushKV("subtract_paymaster_fee_from_amount", false);
        }
        if (send_options.find_value("send_all_spendable_dd").isNull()) {
            send_options.pushKV("send_all_spendable_dd", false);
        }

        const bool restricted = restricted_context_supplied;
        if (restricted && !(restricted_key_supplied && restricted_descriptor_supplied &&
                            restricted_capability_supplied)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "provider_identity_key, restricted_service_descriptor, and sponsorship_capability "
                "must be supplied together");
        }
        if (prepare_only_supplied && send_options.find_value("prepare_only").get_bool() &&
            authorization_commitment_supplied) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "prepare_only and authorization_commitment are mutually exclusive");
        }

        if (fee_mode != DigiDollar::Paymaster::FeeMode::DGB) {
            const UniValue& request_id = send_options.find_value("request_id");
            const UniValue& fee_cap = send_options.find_value("maximum_paymaster_fee_cents");
            if (request_id.isNull() || fee_cap.isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "request_id and maximum_paymaster_fee_cents are required for paymaster or auto");
            }
            if (!DigiDollar::Paymaster::IsCanonicalRequestId(request_id.get_str())) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "request_id must be a canonical lowercase UUID");
            }
            const int64_t maximum_fee = fee_cap.getInt<int64_t>();
            if (maximum_fee < 0 || maximum_fee > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "maximum_paymaster_fee_cents is out of range");
            }
        }
    }
    return options;
}

std::optional<UniValue> PrepareDigiDollarFeeFunding(
    const JSONRPCRequest& request, CWallet& wallet, DigiDollarWallet& dd_wallet,
    const std::string& addressStr, const CDigiDollarAddress& dd_address, CAmount amount,
    const PaymasterSendOptions& options, std::vector<COutPoint>& selected_inputs,
    const std::vector<COutPoint>*& preset_dd_inputs)
{
    const auto& [fee_mode, subtract_paymaster_fee_from_amount, send_all_spendable_dd, send_options] = options;
    const auto finalize_paymaster_result =
        [&](UniValue result) -> UniValue {
            result.pushKV("to_address", addressStr);
            const UniValue& payment =
                result.find_value("payment_cents");
            result.pushKV("amount", payment.isNull()
                                        ? amount
                                        : payment.getInt<int64_t>());
            result.pushKV("requested_amount_cents", amount);
            result.pushKV("subtract_paymaster_fee_from_amount",
                          subtract_paymaster_fee_from_amount);
            result.pushKV("send_all_spendable_dd",
                          send_all_spendable_dd);
            result.pushKV("status",
                          result.find_value("final").isTrue()
                              ? "success"
                              : "pending");
            return result;
        };

    // Once an AUTO or PAYMASTER request owns a persistent client
    // session it must resume that exact flow before ordinary balance
    // checks or a fresh direct-DGB preflight. Its inputs may already
    // be reserved or spent by the final transaction, so both the
    // spendable and total DD balances can legitimately be lower than
    // the original request. RequestAutomaticPaymasterQuote performs
    // the durable order/input checks and returns the idempotent state.
    if (fee_mode != DigiDollar::Paymaster::FeeMode::DGB) {
        const std::string request_id =
            send_options.find_value("request_id").get_str();
        DigiDollar::Paymaster::PaymentSession persisted_session;
        wallet::PaymasterStore store{wallet};
        const auto status = store.GetSessionByRequestIdWithStatus(request_id, persisted_session);
        if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                status == DatabaseReadStatus::UNSUPPORTED_VERSION
                    ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION"
                    : "PAYMASTER_SESSION_READ_FAILED");
        }
        if (status == DatabaseReadStatus::FOUND && !persisted_session.provider_side) {
            return finalize_paymaster_result(
                wallet::RequestAutomaticPaymasterQuote(
                    request, addressStr, amount, send_options,
                    preset_dd_inputs));
        }
    }

    // Bind send-all to the exact ordinary spendable set observed by
    // this call. RequestAutomaticPaymasterQuote verifies the same
    // balance again immediately before durable reservation.
    std::vector<CAmount> send_all_amounts;
    CAmount send_all_total{0};
    std::string send_all_error;
    bool send_all_snapshot_available{!send_all_spendable_dd};
    if (send_all_spendable_dd) {
        send_all_snapshot_available =
            dd_wallet.SelectAllSpendableDDCoins(
                amount, selected_inputs, send_all_total,
                &send_all_amounts, send_all_error);
        if (send_all_snapshot_available) {
            preset_dd_inputs = &selected_inputs;
        } else {
            // A durable Paymaster session already hides its exact
            // inputs from ordinary spendable coin selection. Leave
            // the preset empty so RequestAutomaticPaymasterQuote can
            // distinguish that safe retry from a genuine pre-reserve
            // balance change and reuse only the persisted binding.
            selected_inputs.clear();
            send_all_amounts.clear();
            preset_dd_inputs = nullptr;
        }
    }

    // Direct DGB-funded sends may only use ordinary spendable inputs.
    // Paymaster and AUTO retries deliberately use the total confirmed
    // balance here because the exact retry may already own a durable
    // reservation; RequestAutomaticPaymasterQuote validates that
    // reservation against the persisted session before signing.
    const CAmount balance = fee_mode == DigiDollar::Paymaster::FeeMode::DGB
        ? dd_wallet.GetSpendableDDBalance()
        : dd_wallet.GetTotalDDBalance();
    if (amount > balance) {
        const CAmount pending_balance = dd_wallet.GetPendingDDBalance();
        if (amount <= balance + pending_balance) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                "Insufficient confirmed DD balance; please wait for prior DigiDollar transfer confirmation and try again.");
        }
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
            strprintf("Insufficient DD balance (have %d cents, need %d cents)",
                     balance, amount));
    }

    if (fee_mode == DigiDollar::Paymaster::FeeMode::PAYMASTER) {
        return finalize_paymaster_result(
            wallet::RequestAutomaticPaymasterQuote(
                request, addressStr, amount, send_options,
                preset_dd_inputs));
    }

    if (fee_mode == DigiDollar::Paymaster::FeeMode::AUTO) {
        DDTransferPlan plan;
        std::string preflight_error;
        const bool can_fund_direct = send_all_snapshot_available &&
            dd_wallet.PlanDigiDollarTransfer(
                {{dd_address, amount}}, plan, preflight_error,
                preset_dd_inputs);
        if (!can_fund_direct) {
            if (send_all_spendable_dd &&
                !send_all_snapshot_available) {
                // Either resume the already persisted exact session,
                // or fail closed with PAYMASTER_SWEEP_BALANCE_CHANGED
                // before a new reservation is created.
                return finalize_paymaster_result(
                    wallet::RequestAutomaticPaymasterQuote(
                        request, addressStr, amount, send_options,
                        /*preset_inputs=*/nullptr));
            }
            if (plan.error_code !=
                DDTransferPlanError::INSUFFICIENT_DGB_FEE_INPUTS) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   strprintf("Transfer preflight failed: %s", preflight_error));
            }
            return finalize_paymaster_result(
                wallet::RequestAutomaticPaymasterQuote(
                    request, addressStr, amount, send_options,
                    preset_dd_inputs));
        }
        if (wallet.IsLocked()) {
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                "DigiDollar send requires the wallet to be unlocked. "
                "Error: Please enter the wallet passphrase with walletpassphrase first.");
        }
    }
    return std::nullopt;
}
} // namespace wallet
