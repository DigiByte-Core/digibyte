// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <consensus/validation.h>
#include <index/txindex.h>
#include <net.h>
#include <net_processing.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <txmempool.h>
#include <validation.h>
#include <validationinterface.h>
#include <node/transaction.h>
#include <random.h>
#include <logging.h>
#include <common/args.h>
#include <primitives/transaction.h>

#include <future>

namespace node {
static TransactionError HandleATMPError(const TxValidationState& state, std::string& err_string_out)
{
    err_string_out = state.ToString();
    if (state.IsInvalid()) {
        if (state.GetResult() == TxValidationResult::TX_MISSING_INPUTS) {
            return TransactionError::MISSING_INPUTS;
        }
        return TransactionError::MEMPOOL_REJECTED;
    } else {
        return TransactionError::MEMPOOL_ERROR;
    }
}

TransactionError BroadcastTransaction(NodeContext& node, const CTransactionRef tx, std::string& err_string, const CAmount& max_tx_fee, bool relay, bool wait_callback)
{
    // BroadcastTransaction can be called by either sendrawtransaction RPC or the wallet.
    // chainman, mempool and peerman are initialized before the RPC server and wallet are started
    // and reset after the RPC sever and wallet are stopped.
    assert(node.chainman);
    assert(node.mempool);
    assert(node.peerman);

    std::promise<void> promise;
    uint256 txid = tx->GetHash();
    uint256 wtxid = tx->GetWitnessHash();
    bool callback_set = false;
    bool already_in_mempool = false;  // Track if we already added to regular mempool

    LogPrintf("BroadcastTransaction: Starting broadcast for tx %s (relay=%d, max_fee=%d)\n",
              txid.ToString(), relay, max_tx_fee);

    {
        LOCK(cs_main);

        // If the transaction is already confirmed in the chain, don't do anything
        // and return early.
        CCoinsViewCache &view = node.chainman->ActiveChainstate().CoinsTip();
        for (size_t o = 0; o < tx->vout.size(); o++) {
            const Coin& existingCoin = view.AccessCoin(COutPoint(txid, o));
            // IsSpent doesn't mean the coin is spent, it means the output doesn't exist.
            // So if the output does exist, then this transaction exists in the chain.
            if (!existingCoin.IsSpent()) {
                LogPrintf("BroadcastTransaction: Transaction %s already in chain, returning ALREADY_IN_CHAIN\n", txid.ToString());
                return TransactionError::ALREADY_IN_CHAIN;
            }
        }
        LogPrintf("BroadcastTransaction: Transaction %s NOT in chain, checking mempool/stempool\n", txid.ToString());

        if (auto mempool_tx = node.mempool->get(txid); mempool_tx) {
            LogPrintf("BroadcastTransaction: Transaction %s found in mempool, reannouncing\n", txid.ToString());
            // There's already a transaction in the mempool with this txid. Don't
            // try to submit this transaction to the mempool (since it'll be
            // rejected as a TX_CONFLICT), but do attempt to reannounce the mempool
            // transaction if relay=true.
            //
            // The mempool transaction may have the same or different witness (and
            // wtxid) as this transaction. Use the mempool's wtxid for reannouncement.
            wtxid = mempool_tx->GetWitnessHash();
        } else if (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION) && node.stempool && node.stempool->exists(GenTxid::Txid(txid))) {
            // Transaction is already in stempool for Dandelion routing
            LogPrintf("BroadcastTransaction: Transaction %s already in stempool, will proceed with relay\n", txid.ToString());
            // Don't try to re-add to stempool, but DO continue with relay logic below
        } else {
            LogPrintf("BroadcastTransaction: Transaction %s NOT in mempool or stempool, submitting\n", txid.ToString());
            // Transaction is not already in the mempool.

            // Check if this is a DigiDollar transaction
            bool isDigiDollar = IsDigiDollarTransaction(*tx);

            // Skip fee check for DigiDollar transactions - they can have large inputs (block rewards)
            // that make the calculated "fee" appear huge, when it's actually just change.
            if (max_tx_fee > 0 && !isDigiDollar) {
                LogPrintf("BroadcastTransaction: Checking fee limit (max_fee=%d)\n", max_tx_fee);
                // First, call ATMP with test_accept and check the fee. If ATMP
                // fails here, return error immediately.
                const MempoolAcceptResult result = AcceptToMemoryPool(node.chainman->ActiveChainstate(), *node.mempool, tx, /*bypass_limits=*/false, /*test_accept=*/true);
                if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                    LogPrintf("BroadcastTransaction: Fee check FAILED - %s\n", result.m_state.ToString());
                    return HandleATMPError(result.m_state, err_string);
                } else if (result.m_base_fees.value() > max_tx_fee) {
                    LogPrintf("BroadcastTransaction: Fee exceeds max (%d > %d)\n", result.m_base_fees.value(), max_tx_fee);
                    return TransactionError::MAX_FEE_EXCEEDED;
                } else {
                    LogPrintf("BroadcastTransaction: Fee check PASSED (fee=%d)\n", result.m_base_fees.value());
                }
            } else if (isDigiDollar) {
                LogPrintf("BroadcastTransaction: Skipping fee check for DigiDollar transaction\n");
            }

            // Test acceptance to stempool for consistency with Dandelion routing
            if (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION)) {
                LogPrintf("BroadcastTransaction: Testing stempool acceptance\n");
                AcceptToMemoryPoolForStempool(node.chainman->ActiveChainstate(), *node.stempool, *node.mempool, tx, /*bypass_limits=*/false, /*test_accept=*/true);
            }
            // Try to submit the transaction to the stempool only (if dandelion is enabled);
            LogPrintf("BroadcastTransaction: Dandelion=%d, checking submission path\n", gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION));
            if (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION)) {
                // Only submit if not already in stempool
                if (!node.stempool->exists(GenTxid::Txid(txid))) {
                    // Submit to stempool for Dandelion routing
                    LogPrintf("BroadcastTransaction: Dandelion enabled, submitting transaction %s to stempool\n", txid.ToString());
                    const MempoolAcceptResult result = AcceptToMemoryPoolForStempool(node.chainman->ActiveChainstate(), *node.stempool, *node.mempool, tx, /*bypass_limits=*/false);
                    if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                    LogPrintf("BroadcastTransaction: Failed to accept transaction %s to stempool: %s\n", 
                             txid.ToString(), result.m_state.ToString());
                    
                    // If it failed due to missing inputs, it might be because the inputs are in mempool
                    // In this case, fall back to regular broadcast
                    if (result.m_state.GetResult() == TxValidationResult::TX_MISSING_INPUTS) {
                        LogPrintf("BroadcastTransaction: Transaction %s has inputs in mempool, falling back to regular broadcast\n", 
                                 txid.ToString());
                        // Fall back to regular mempool submission
                        const MempoolAcceptResult mempool_result = node.chainman->ProcessTransaction(tx, /*test_accept=*/ false);
                        if (mempool_result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                            return HandleATMPError(mempool_result.m_state, err_string);
                        }
                        // Add to unbroadcast for immediate relay
                        if (relay) {
                            node.mempool->AddUnbroadcastTx(txid);
                        }
                        already_in_mempool = true;  // Mark that we've already added to mempool
                    } else {
                        return HandleATMPError(result.m_state, err_string);
                    }
                } else {
                    LogPrintf("BroadcastTransaction: Successfully accepted transaction %s to stempool (poolsz %u txn, %u kB)\n",
                             txid.ToString(), node.stempool->size(), node.stempool->DynamicMemoryUsage() / 1000);
                    // Verify the transaction is actually in the stempool
                    if (node.stempool->exists(txid)) {
                        LogPrintf("BroadcastTransaction: Verified transaction %s is in stempool\n", txid.ToString());
                    } else {
                        LogPrintf("BroadcastTransaction: ERROR - Transaction %s not found in stempool after acceptance!\n", txid.ToString());
                    }
                    // Don't notify wallet here - stempool transactions should remain separate until fluffed
                    }
                }
            } else {
                LogPrintf("BroadcastTransaction: Dandelion DISABLED, submitting to regular mempool\n");
                const MempoolAcceptResult result = node.chainman->ProcessTransaction(tx, /*test_accept=*/ false);
                if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                    LogPrintf("BroadcastTransaction: Mempool submission FAILED - %s\n", result.m_state.ToString());
                    return HandleATMPError(result.m_state, err_string);
                }
                LogPrintf("BroadcastTransaction: Successfully added to mempool\n");
            }

            // Transaction was accepted to the mempool.

            if (relay && !gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION)) {
                // the mempool tracks locally submitted transactions to make a
                // best-effort of initial broadcast
                // Only add to unbroadcast if not using Dandelion
                node.mempool->AddUnbroadcastTx(txid);
            }

            if (wait_callback) {
                // For transactions broadcast from outside the wallet, make sure
                // that the wallet has been notified of the transaction before
                // continuing.
                //
                // This prevents a race where a user might call sendrawtransaction
                // with a transaction to/from their wallet, immediately call some
                // wallet RPC, and get a stale result because callbacks have not
                // yet been processed.
                CallFunctionInValidationInterfaceQueue([&promise] {
                    promise.set_value();
                });
                callback_set = true;
            }
        }
    } // cs_main

    LogPrintf("BroadcastTransaction: Exited cs_main lock, callback_set=%d\n", callback_set);

    if (callback_set) {
        // Wait until Validation Interface clients have been notified of the
        // transaction entering the mempool.
        promise.get_future().wait();
    }

    LogPrintf("BroadcastTransaction: Entering relay section (relay=%d)\n", relay);
    if (relay) {
        if (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION)) {
            LogPrintf("BroadcastTransaction: Processing Dandelion relay for %s\n", txid.ToString());
            auto current_time = GetTime<std::chrono::milliseconds>();
            std::chrono::microseconds nEmbargo = DANDELION_EMBARGO_MINIMUM + PoissonNextSend(current_time, DANDELION_EMBARGO_AVG_ADD);
            node.connman->insertDandelionEmbargo(txid, nEmbargo);
            auto embargo_timeout = std::chrono::duration_cast<std::chrono::seconds>(nEmbargo - current_time).count();
            LogPrint(BCLog::DANDELION, "dandeliontx %s embargoed for %d seconds\n", txid.ToString(), embargo_timeout);
            CInv embargoTx(MSG_DANDELION_TX, txid);
            
            bool pushed = node.connman->localDandelionDestinationPushInventory(embargoTx);
            if (!pushed) {
                // No Dandelion destination available - fallback to regular broadcast
                LogPrintf("BroadcastTransaction: DANDELION FALLBACK - No viable Dandelion destinations for transaction %s\n", txid.ToString());
                
                // Only move from stempool to mempool if we haven't already done so
                if (!already_in_mempool) {
                    LogPrintf("BroadcastTransaction: Moving transaction %s from stempool to mempool for regular broadcast\n", txid.ToString());
                    // Remove from stempool and add to mempool for regular broadcast.
                    // This fallback used to run both steps with no lock at all.
                    // removeRecursive needs the stempool lock (block connect and
                    // embargo expiry change the stempool from other threads) and
                    // ProcessTransaction needs cs_main. cs_main is taken first and
                    // the stempool lock inside it, the same order every other
                    // stempool writer uses (cs_main, then mempool, then stempool).
                    // Both are released at the end of this block, before the relay.
                    LOCK(cs_main);
                    WITH_LOCK(node.stempool->cs, node.stempool->removeRecursive(*tx, MemPoolRemovalReason::REORG));
                    const MempoolAcceptResult result = node.chainman->ProcessTransaction(tx, /*test_accept=*/ false);
                    if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                        return HandleATMPError(result.m_state, err_string);
                    }
                    // Add to unbroadcast for regular relay
                    node.mempool->AddUnbroadcastTx(txid);
                }
                node.peerman->RelayTransaction(txid, wtxid);
                LogPrintf("BroadcastTransaction: Transaction %s relayed via regular broadcast\n", txid.ToString());
            } else {
                // Push the transaction immediately to the Dandelion destination
                // This ensures it actually propagates during the stem phase
                LogPrintf("BroadcastTransaction: DANDELION SUCCESS - Transaction %s queued for stem routing\n", txid.ToString());
                node.peerman->PushDandelionTransaction(txid);
            }
            
            return TransactionError::OK;
        }
        node.peerman->RelayTransaction(txid, wtxid);
    } else if (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION)) {
        // For Dandelion, we need to set up the embargo and routing even when relay=false
        // This happens during wallet startup when resubmitting transactions
        auto current_time = GetTime<std::chrono::milliseconds>();
        std::chrono::microseconds nEmbargo = DANDELION_EMBARGO_MINIMUM + PoissonNextSend(current_time, DANDELION_EMBARGO_AVG_ADD);
        node.connman->insertDandelionEmbargo(txid, nEmbargo);
        auto embargo_timeout = std::chrono::duration_cast<std::chrono::seconds>(nEmbargo - current_time).count();
        LogPrint(BCLog::DANDELION, "dandeliontx %s embargoed for %d seconds (relay=false)\n", txid.ToString(), embargo_timeout);
        CInv embargoTx(MSG_DANDELION_TX, txid);
        
        bool pushed = node.connman->localDandelionDestinationPushInventory(embargoTx);
        if (!pushed) {
            // No Dandelion destination available for wallet rebroadcast
            LogPrintf("BroadcastTransaction: DANDELION DEFERRED - No viable destinations for %s (relay=false)\n", txid.ToString());
            LogPrintf("BroadcastTransaction: Transaction %s remains in stempool awaiting Dandelion peers\n", txid.ToString());
            // Transaction stays in stempool and will be handled when Dandelion peers become available
        } else {
            LogPrintf("BroadcastTransaction: DANDELION QUEUED - Transaction %s scheduled for stem routing (relay=false)\n", txid.ToString());
            node.peerman->PushDandelionTransaction(txid);
        }
    }

    return TransactionError::OK;
}

CTransactionRef GetTransaction(const CBlockIndex* const block_index, const CTxMemPool* const mempool, const uint256& hash, uint256& hashBlock, const BlockManager& blockman)
{
    if (mempool && !block_index) {
        CTransactionRef ptx = mempool->get(hash);
        if (ptx) return ptx;
    }
    if (g_txindex) {
        CTransactionRef tx;
        uint256 block_hash;
        if (g_txindex->FindTx(hash, block_hash, tx)) {
            if (!block_index || block_index->GetBlockHash() == block_hash) {
                // Don't return the transaction if the provided block hash doesn't match.
                // The case where a transaction appears in multiple blocks (e.g. reorgs or
                // BIP30) is handled by the block lookup below.
                hashBlock = block_hash;
                return tx;
            }
        }
    }
    if (block_index) {
        CBlock block;
        if (blockman.ReadBlockFromDisk(block, *block_index)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    hashBlock = block_index->GetBlockHash();
                    return tx;
                }
            }
        }
    }
    return nullptr;
}
} // namespace node
