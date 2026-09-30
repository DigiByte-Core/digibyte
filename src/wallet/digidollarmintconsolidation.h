// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_DIGIDOLLARMINTCONSOLIDATION_H
#define DIGIBYTE_WALLET_DIGIDOLLARMINTCONSOLIDATION_H

#include <util/result.h>
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <exception>
#include <map>
#include <string>
#include <vector>

namespace wallet {

enum class MintConsolidationFailure { WALLET, INSUFFICIENT_FUNDS, REJECTED };

struct MintConsolidationResult {
    std::vector<uint256> txids;
    std::string error;
    MintConsolidationFailure failure{MintConsolidationFailure::WALLET};
};

//! Release only a merge whose commit is known to have failed. A merge that
//! was accepted and later evicted must still be allowed to confirm.
inline bool ReleaseFailedDigiDollarMintConsolidation(CWallet& wallet, const uint256& txid)
{
    LOCK(wallet.cs_wallet);
    const auto it = wallet.mapWallet.find(txid);
    if (it == wallet.mapWallet.end() || it->second.isAbandoned()) return true;
    if (it->second.InMempool() || wallet.GetTxDepthInMainChain(it->second) != 0) return false;
    it->second.mapValue["digidollar_mint_consolidation"] = "failed";
    try {
        if (wallet.AbandonTransaction(txid)) return true;
        // Best effort: retain the reason for retrying cleanup after the wallet
        // file becomes writable. The caller reports failure either way.
        WalletBatch(wallet.GetDatabase()).WriteTx(it->second);
    } catch (const std::exception&) {
        // Cleanup must not hide the transaction IDs of earlier accepted merges.
    }
    return false;
}

//! Wallet transaction metadata keeps repeated mint requests from paying for
//! another merge while the previous one is still waiting, including after restart.
inline MintConsolidationResult GetPendingDigiDollarMintConsolidations(CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    MintConsolidationResult pending;
    for (const auto& [txid, tx] : wallet.mapWallet) {
        const auto marker = tx.mapValue.find("digidollar_mint_consolidation");
        if (!tx.isUnconfirmed() || marker == tx.mapValue.end()) continue;
        if (marker->second == "failed" && !tx.InMempool()) {
            if (!ReleaseFailedDigiDollarMintConsolidation(wallet, txid)) {
                pending.error = "Could not release failed coin merge " + txid.GetHex() +
                    ". Check that the wallet file is writable, then retry.";
            }
            continue;
        }
        pending.txids.push_back(txid);
    }
    return pending;
}

//! Combine only enough confirmed coins for this mint. Callers must stop here
//! and wait for confirmation, including when a later batch fails.
inline MintConsolidationResult ConsolidateDigiDollarMintCoins(
    CWallet& wallet, std::vector<COutPoint> coins,
    const std::map<COutPoint, CAmount>& values, CAmount target)
{
    LOCK(wallet.cs_wallet);
    MintConsolidationResult result;
    if (!wallet.GetBroadcastTransactions()) {
        result.error = "Coin merging requires wallet transaction broadcast to be enabled.";
        return result;
    }
    // Ordinary mint funding may use trusted change, but a merge must not add
    // unconfirmed ancestors to these large batches.
    coins.erase(std::remove_if(coins.begin(), coins.end(), [&](const auto& coin) {
        const auto* tx = wallet.GetWalletTx(coin.hash);
        return !tx || wallet.GetTxDepthInMainChain(*tx) <= 0;
    }), coins.end());
    CAmount available{0};
    for (const auto& coin : coins) available += values.at(coin);
    if (available < target) {
        result.failure = MintConsolidationFailure::INSUFFICIENT_FUNDS;
        result.error = "Insufficient confirmed funds for collateral and fees. If a coin merge is pending, wait for confirmation and retry.";
        return result;
    }
    auto destination = wallet.GetNewChangeDestination(OutputType::BECH32);
    if (!destination) {
        result.error = "Failed to get a coin merge address.";
        return result;
    }
    std::sort(coins.begin(), coins.end(), [&](const auto& a, const auto& b) {
        return values.at(a) != values.at(b) ? values.at(a) > values.at(b) : a < b;
    });

    // Keep the existing batch limits. CreateTransaction checks transaction
    // weight, fees and ancestry before any batch is committed.
    constexpr size_t MAX_INPUTS{1400};
    constexpr size_t MAX_BATCHES{10};
    CAmount merged{0};
    size_t offset{0};
    while (offset < coins.size() && merged < target && result.txids.size() < MAX_BATCHES) {
        CCoinControl control;
        control.m_allow_other_inputs = false;
        control.m_min_depth = 1;
        CAmount batch_total{0};
        CAmount fee{0};
        size_t count{0};
        while (true) {
            while (offset < coins.size() && count < MAX_INPUTS && batch_total < target - merged + fee) {
                const auto& coin = coins[offset++];
                control.Select(coin);
                batch_total += values.at(coin);
                ++count;
            }
            CTransactionRef attempted;
            auto release_failed_attempt = [&] {
                if (attempted && !ReleaseFailedDigiDollarMintConsolidation(wallet, attempted->GetHash())) {
                    result.error += " Could not release failed coin merge " + attempted->GetHash().GetHex() +
                        ". Check that the wallet file is writable, then retry.";
                }
            };
            try {
                const std::vector<CRecipient> recipients{{*destination, batch_total, /*subtract_fee=*/true}};
                auto created = CreateTransaction(wallet, recipients, /*change_pos=*/-1, control, /*sign=*/true);
                if (!created) {
                    result.error = "Coin merge failed: " + util::ErrorString(created).original;
                    return result;
                }
                fee = created->fee;
                // Include the batch fee without sweeping unrelated remaining coins.
                if (batch_total - fee < target - merged && count < MAX_INPUTS && offset < coins.size()) continue;
                std::string error;
                attempted = created->tx;
                if (!wallet.CommitTransaction(created->tx, {{"digidollar_mint_consolidation", "1"}}, {}, &error)) {
                    result.failure = MintConsolidationFailure::REJECTED;
                    result.error = "Coin merge transaction rejected: " + error;
                    release_failed_attempt();
                    return result;
                }
                result.txids.push_back(created->tx->GetHash());
                merged += batch_total - fee;
                break;
            } catch (const std::exception& e) {
                result.error = std::string("Coin merge failed: ") + e.what();
                release_failed_attempt();
                return result;
            }
        }
    }
    if (merged < target) {
        result.error = "More confirmed funds or another coin merge may be needed for collateral and fees.";
    }
    return result;
}

} // namespace wallet

#endif // DIGIBYTE_WALLET_DIGIDOLLARMINTCONSOLIDATION_H
