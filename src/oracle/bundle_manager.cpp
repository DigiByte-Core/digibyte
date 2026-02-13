// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <oracle/bundle_manager.h>

#include <algorithm>
#include <cassert>

#include <chainparams.h>
#include <consensus/consensus.h>
#include <digidollar/digidollar.h>
#include <kernel/chainparams.h>
#include <logging.h>
#include <net.h>
#include <netmessagemaker.h>
#include <oracle/mock_oracle.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/standard.h>
#include <util/time.h>
#include <validation.h>

// Forward declaration for missing functions
extern int32_t GetBestHeight();

// Get current chain height - used by oracle bundle manager
int32_t GetBestHeight() {
    // TODO: Wire up to ChainstateManager properly
    // For now, return 0 if we can't determine height
    return 0;
}

/**
 * Verify an oracle price message against the chain-configured oracle pubkey.
 *
 * SECURITY (DGB-SEC-002): Oracle messages arrive via P2P with an embedded pubkey
 * that is attacker-controlled. We must never verify the signature against that
 * embedded key — instead, look up the trusted pubkey from chainparams by oracle_id,
 * rebind it onto the message, and then verify the Phase 2 Schnorr signature.
 *
 * @param message  The oracle price message to verify
 * @return true if the message's signature is valid against the chainparams pubkey
 */
static bool VerifyOracleMessageWithChainparamsKey(const COraclePriceMessage& message)
{
    const CChainParams& params = Params();
    const OracleNodeInfo* oracle_config = params.GetOracleNode(message.oracle_id);
    if (!oracle_config) {
        return false;
    }
    COraclePriceMessage bound_msg = message;
    bound_msg.oracle_pubkey = XOnlyPubKey(oracle_config->pubkey);
    return bound_msg.VerifyPhase2();
}

//! Global oracle bundle manager instance
std::unique_ptr<OracleBundleManager> g_oracle_bundle_manager;

/**
 * OracleBundleManager Implementation
 */

OracleBundleManager::OracleBundleManager()
{
    LogPrintf("Oracle: Initializing Oracle Bundle Manager\n");
}

OracleBundleManager::~OracleBundleManager()
{
    LogPrintf("Oracle: Shutting down Oracle Bundle Manager\n");
}

bool OracleBundleManager::AddOracleMessage(const COraclePriceMessage& message)
{
    LogPrintf("Oracle: AddOracleMessage called for oracle_id=%d, price=%llu, timestamp=%d, enabled=%d\n",
             message.oracle_id, message.price_micro_usd, message.timestamp, enabled);

    if (!enabled) {
        LogPrintf("Oracle: Manager not enabled, rejecting message\n");
        return false;
    }

    if (!IsValidOracleMessage(message)) {
        LogPrintf("Oracle: Invalid oracle message from oracle %d\n", message.oracle_id);
        return false;
    }

    LogPrintf("Oracle: Message passed IsValidOracleMessage check\n");

    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    // Purge stale messages from pending_messages.
    // Messages older than ORACLE_MAX_AGE_SECONDS are from oracles that may no
    // longer be active. Without this cleanup, oracle nodes accumulate entries
    // from briefly-running oracles, inflating the consensus count (e.g. showing
    // "7-of-5" instead of "5-of-5").
    {
        int64_t now = GetTime();
        auto stale_it = pending_messages.begin();
        while (stale_it != pending_messages.end()) {
            if (now - stale_it->second.timestamp > ORACLE_MAX_AGE_SECONDS) {
                LogPrint(BCLog::DIGIDOLLAR, "Oracle: Purging stale message from oracle %d (age %lld seconds)\n",
                         stale_it->first, now - stale_it->second.timestamp);
                stale_it = pending_messages.erase(stale_it);
            } else {
                ++stale_it;
            }
        }
    }

    // Calculate message hash for duplicate detection
    uint256 msg_hash = message.GetSignatureHash();

    // Check if we've already seen this exact message
    if (seen_message_hashes.count(msg_hash) > 0) {
        LogPrintf("Oracle: Ignoring duplicate message from oracle %d\n", message.oracle_id);
        return false;
    }

    // Add to seen set
    seen_message_hashes.insert(msg_hash);

    // Cap seen_message_hashes to prevent unbounded growth.
    // Each oracle re-broadcasts every ~60 seconds with a new timestamp, adding a
    // new hash each time.  With 8 oracles broadcasting for hours, the set can
    // grow very large.  Keep only the most recent hashes; old hashes are no
    // longer needed because the corresponding messages have already expired
    // from pending_messages (purged above) and from the P2P relay window.
    static constexpr size_t MAX_SEEN_HASHES = 2048;
    if (seen_message_hashes.size() > MAX_SEEN_HASHES) {
        // std::set iteration is ordered; erasing from begin() removes the
        // "smallest" hashes which, being random SHA-256 digests, are
        // effectively arbitrary.  This is acceptable because the set is only
        // a best-effort duplicate filter — true dedup is enforced by the
        // pending_messages oracle_id key.
        auto erase_end = seen_message_hashes.begin();
        std::advance(erase_end, seen_message_hashes.size() - MAX_SEEN_HASHES);
        seen_message_hashes.erase(seen_message_hashes.begin(), erase_end);
    }

    // Check if we already have a message from this oracle for current epoch
    auto it = pending_messages.find(message.oracle_id);
    if (it != pending_messages.end()) {
        // Replace if new message is more recent
        if (message.timestamp > it->second.timestamp) {
            it->second = message;
            LogPrintf("Oracle: Updated message from oracle %d with newer timestamp\n", message.oracle_id);
        } else {
            LogPrintf("Oracle: Ignoring older message from oracle %d\n", message.oracle_id);
            return false;
        }
    } else {
        // Add new message
        pending_messages[message.oracle_id] = message;
        LogPrintf("Oracle: Added new message from oracle %d: price=%llu micro-USD, timestamp=%d\n",
                 message.oracle_id, message.price_micro_usd, message.timestamp);
    }

    // Note: Bundle creation happens in AddOracleBundleToBlock() or when explicitly requested
    // Don't auto-create here to avoid epoch mismatch issues

    // Update cached price only when consensus is met
    // Phase One (1-of-1): any single message is consensus
    // Phase Two (4-of-7): need min_oracle_count agreeing messages
    //
    // Only count fresh messages (within ORACLE_MAX_AGE_SECONDS) for consensus.
    // Stale entries were purged above, so pending_messages.size() is the
    // fresh message count.
    {
        std::lock_guard<std::recursive_mutex> pending_lock(mtx_messages);
        int fresh_count = static_cast<int>(pending_messages.size());
        if (fresh_count >= min_oracle_count) {
            // Calculate median of fresh pending messages for consensus price
            std::vector<uint64_t> prices;
            prices.reserve(pending_messages.size());
            for (const auto& pair : pending_messages) {
                prices.push_back(pair.second.price_micro_usd);
            }
            std::sort(prices.begin(), prices.end());
            uint64_t median_price = prices[prices.size() / 2];

            std::lock_guard<std::mutex> price_lock(mtx_bundles);
            cached_price = static_cast<CAmount>(median_price);
            last_update_time = GetTime();
            LogPrintf("Oracle: Updated cached price with %d-of-%d consensus: %llu micro-USD ($%.6f)\n",
                     fresh_count, min_oracle_count,
                     median_price, median_price / 1000000.0);
        } else {
            LogPrintf("Oracle: %d fresh messages, need %d for consensus - cached price unchanged\n",
                     fresh_count, min_oracle_count);
        }
    }

    return true;
}

bool OracleBundleManager::RemoveOracleMessage(uint32_t oracle_id)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    auto it = pending_messages.find(oracle_id);
    if (it != pending_messages.end()) {
        pending_messages.erase(it);
        LogPrintf("Oracle: Removed message from oracle %d\n", oracle_id);
        return true;
    }

    return false;
}

std::vector<COraclePriceMessage> OracleBundleManager::GetPendingMessages() const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    LogPrintf("Oracle: GetPendingMessages called, pending_messages.size()=%zu\n", pending_messages.size());

    std::vector<COraclePriceMessage> messages;
    messages.reserve(pending_messages.size());

    for (const auto& [oracle_id, message] : pending_messages) {
        LogPrintf("Oracle: GetPendingMessages - oracle_id=%d, price=%llu, timestamp=%d\n",
                 oracle_id, message.price_micro_usd, message.timestamp);
        messages.push_back(message);
    }

    LogPrintf("Oracle: GetPendingMessages returning %zu messages\n", messages.size());
    return messages;
}

size_t OracleBundleManager::GetPendingMessageCount() const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    return pending_messages.size();
}

void OracleBundleManager::ClearPendingMessages()
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    pending_messages.clear();
    LogPrintf("Oracle: Manually cleared all pending messages\n");
}

void OracleBundleManager::InjectTestMessage(const COraclePriceMessage& message)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    pending_messages[message.oracle_id] = message;
    LogPrintf("Oracle: Injected test message for oracle %d, price=%llu\n",
             message.oracle_id, message.price_micro_usd);
}

COracleBundle OracleBundleManager::GetCurrentBundle(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(mtx_bundles);

    auto it = epoch_bundles.find(epoch);
    if (it != epoch_bundles.end()) {
        return it->second;
    }

    // Return empty bundle if not found
    return COracleBundle(epoch);
}

bool OracleBundleManager::UpdateBundle(const COracleBundle& bundle)
{
    if (!enabled) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mtx_bundles);

    epoch_bundles[bundle.epoch] = bundle;
    LogPrintf("Oracle: Updated bundle for epoch %d with %d messages\n",
             bundle.epoch, bundle.messages.size());

    // Update cached price if this is the latest bundle
    if (bundle.epoch >= cached_epoch && bundle.HasConsensus(min_oracle_count)) {
        cached_price = bundle.GetConsensusPrice(min_oracle_count);
        cached_epoch = bundle.epoch;
        last_update_time = GetTime();
    }

    return true;
}

bool OracleBundleManager::HasValidBundle(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(mtx_bundles);

    auto it = epoch_bundles.find(epoch);
    return it != epoch_bundles.end() && it->second.HasConsensus(min_oracle_count);
}

void OracleBundleManager::CleanupOldBundles(int32_t current_epoch)
{
    std::lock_guard<std::mutex> lock(mtx_bundles);

    // Keep bundles for current and previous epoch only
    auto it = epoch_bundles.begin();
    while (it != epoch_bundles.end()) {
        if (it->first < current_epoch - 1) {
            LogPrintf("Oracle: Cleaning up old bundle for epoch %d\n", it->first);
            it = epoch_bundles.erase(it);
        } else {
            ++it;
        }
    }
}

bool OracleBundleManager::AddOracleBundleToBlock(CBlock& block, int32_t block_height)
{
    LogPrintf("Oracle: AddOracleBundleToBlock called for height %d, enabled=%d, min_oracle_count=%d\n",
             block_height, enabled, min_oracle_count);

    if (!enabled) {
        LogPrintf("Oracle: Oracles disabled, skipping bundle addition\n");
        return true; // Don't fail block creation if oracles are disabled
    }

    int32_t epoch = GetCurrentEpoch(block_height);
    LogPrintf("Oracle: Current epoch=%d for height %d\n", epoch, block_height);

    COracleBundle bundle = GetCurrentBundle(epoch);
    LogPrintf("Oracle: GetCurrentBundle(epoch=%d) returned bundle with %zu messages, HasConsensus=%d\n",
             epoch, bundle.messages.size(), bundle.HasConsensus(min_oracle_count));

    // If no consensus yet, try previous epoch
    if (!bundle.HasConsensus(min_oracle_count)) {
        bundle = GetCurrentBundle(epoch - 1);
        LogPrintf("Oracle: Tried previous epoch, bundle now has %zu messages, HasConsensus=%d\n",
                 bundle.messages.size(), bundle.HasConsensus(min_oracle_count));
    }

    // Phase One: If still no consensus and min_oracle_count == 1, use pending messages directly
    // This allows unit tests to work without full epoch consensus flow
    if (!bundle.HasConsensus(min_oracle_count) && min_oracle_count == 1) {
        LogPrintf("Oracle: Phase One mode - checking pending messages\n");
        std::lock_guard<std::recursive_mutex> lock(mtx_messages);
        std::vector<COraclePriceMessage> pending;
        pending.reserve(pending_messages.size());
        for (const auto& pair : pending_messages) {
            pending.push_back(pair.second);
        }
        LogPrintf("Oracle: Phase One - %zu pending messages\n", pending.size());

        if (!pending.empty()) {
            bundle = COracleBundle(epoch);
            bundle.messages = pending;
            bundle.median_price_micro_usd = pending[0].price_micro_usd;
            LogPrintf("Oracle: Phase One - Using %zu pending message(s) for block %d\n",
                     pending.size(), block_height);
            // Clear pending messages after consuming them into a bundle
            pending_messages.clear();
            LogPrintf("Oracle: Phase One - Cleared pending messages after bundle creation\n");
        } else {
            LogPrintf("Oracle: Phase One - No pending messages available!\n");
        }
    }

    // Phase Two: Create bundle from pending messages when enough are available
    if (!bundle.HasConsensus(min_oracle_count) && min_oracle_count > 1) {
        LogPrintf("Oracle: Phase Two mode - checking pending messages for %d-of-N consensus\n", min_oracle_count);
        std::lock_guard<std::recursive_mutex> lock(mtx_messages);
        std::vector<COraclePriceMessage> pending;
        pending.reserve(pending_messages.size());
        for (const auto& pair : pending_messages) {
            pending.push_back(pair.second);
        }
        LogPrintf("Oracle: Phase Two - %zu pending messages (need %d)\n", pending.size(), min_oracle_count);
        
        if (static_cast<int>(pending.size()) >= min_oracle_count) {
            bundle = COracleBundle(epoch);
            bundle.messages = pending;
            // Calculate consensus price using the Phase Two algorithm
            const Consensus::Params& cparams = Params().GetConsensus();
            bundle.median_price_micro_usd = static_cast<uint64_t>(CalculateConsensusPrice(bundle, cparams));
            bundle.timestamp = pending[0].timestamp;
            LogPrintf("Oracle: Phase Two - Created bundle with %zu messages, consensus price=%llu\n",
                     pending.size(), bundle.median_price_micro_usd);
            // Clear pending messages after consuming them into a bundle
            // This ensures each block only uses fresh oracle submissions
            pending_messages.clear();
            LogPrintf("Oracle: Phase Two - Cleared pending messages after bundle creation\n");
        }
    }

    // If still no consensus, create empty bundle (graceful degradation)
    if (!bundle.HasConsensus(min_oracle_count)) {
        LogPrintf("Oracle: No consensus bundle available for block %d, creating empty oracle data\n", block_height);
        bundle = COracleBundle(epoch);
    }

    LogPrintf("Oracle: Final bundle has %zu messages before CreateOracleScript\n", bundle.messages.size());

    // Create oracle script and add to coinbase
    CScript oracle_script = CreateOracleScript(bundle);
    LogPrintf("Oracle: CreateOracleScript returned script of size %zu\n", oracle_script.size());

    if (oracle_script.empty()) {
        LogPrintf("Oracle: Script is empty, returning true without adding bundle\n");
        return true; // Empty script is OK for transition period
    }

    // Add oracle data as OP_RETURN output to coinbase
    CMutableTransaction coinbase_tx(*block.vtx[0]);
    CTxOut oracle_output;
    oracle_output.nValue = 0;
    oracle_output.scriptPubKey = oracle_script;
    coinbase_tx.vout.push_back(oracle_output);

    // Update block with modified coinbase
    block.vtx[0] = MakeTransactionRef(std::move(coinbase_tx));

    LogPrintf("Oracle: Added oracle bundle to block %d with %d oracle messages\n",
             block_height, bundle.messages.size());
    return true;
}

CScript OracleBundleManager::CreateOracleScript(const COracleBundle& bundle) const
{
    if (bundle.messages.empty()) {
        return CScript(); // Empty script for no oracle data
    }

    // Phase One: Compact format using oracle pubkey hash from chainparams
    // Format: OP_RETURN OP_ORACLE <version=0x01> <oracle_id> <price_micro_usd> <timestamp>
    // Total: ~20 bytes (well within 83 byte MAX_OP_RETURN_RELAY limit)

    if (bundle.messages.size() > 1) {
        // Check if Phase Two is active — only create multi-oracle scripts when Phase Two is enabled
        const Consensus::Params& cparams = Params().GetConsensus();
        int current_height = 0; // Best effort — CreateOracleScript doesn't have height context
        // If Phase Two is not activated (INT_MAX), reject multi-message bundles
        if (cparams.nDigiDollarPhase2Height == std::numeric_limits<int>::max()) {
            LogPrintf("Oracle: Phase Two not active, rejecting multi-message bundle (%zu messages)\n",
                     bundle.messages.size());
            return CScript(); // Phase One: reject multi-message bundles
        }

        // Phase Two: Full signature format — all signatures stored on-chain for verification
        // Format: OP_RETURN OP_ORACLE <version=0x02> <data>
        // Data layout:
        //   num_messages (1 byte)
        //   consensus_price (8 bytes, uint64 LE)
        //   timestamp (8 bytes, int64 LE)
        //   For each message:
        //     oracle_id (1 byte)
        //     schnorr_sig (64 bytes)
        // Total for 4 oracles: 1+8+8 + 4*(1+64) = 277 bytes
        // Exceeds 83-byte MAX_OP_RETURN_RELAY but coinbase is not subject to relay policy

        CScript script;
        script << OP_RETURN << OP_ORACLE;

        script << std::vector<unsigned char>{0x02};

        std::vector<unsigned char> p2_data;
        size_t num_msgs = bundle.messages.size();
        p2_data.reserve(17 + num_msgs * 65);

        // Number of oracle messages
        p2_data.push_back(static_cast<unsigned char>(num_msgs & 0xFF));

        // Consensus price in micro-USD (uint64, little-endian)
        uint64_t p2_price = bundle.median_price_micro_usd;
        for (int i = 0; i < 8; ++i) {
            p2_data.push_back(static_cast<unsigned char>((p2_price >> (i * 8)) & 0xFF));
        }

        // Consensus timestamp (int64, little-endian)
        int64_t p2_timestamp = bundle.timestamp;
        for (int i = 0; i < 8; ++i) {
            p2_data.push_back(static_cast<unsigned char>((p2_timestamp >> (i * 8)) & 0xFF));
        }

        // Per-oracle entries: oracle_id + schnorr_sig
        for (const auto& msg : bundle.messages) {
            p2_data.push_back(static_cast<unsigned char>(msg.oracle_id & 0xFF));
            if (msg.schnorr_sig.size() == 64) {
                p2_data.insert(p2_data.end(), msg.schnorr_sig.begin(), msg.schnorr_sig.end());
            } else {
                // Pad with zeros if signature missing (will fail validation)
                p2_data.insert(p2_data.end(), 64, 0x00);
                LogPrintf("Oracle: WARNING - Phase Two message for oracle %d missing signature\n", msg.oracle_id);
            }
        }

        // For data > 75 bytes, CScript << vector uses OP_PUSHDATA1/2 automatically
        script << p2_data;

        LogPrintf("Oracle: Created Phase Two script with %zu oracle messages, %zu bytes, price=%llu\n",
                 num_msgs, p2_data.size(), p2_price);
        return script;
    }

    // Phase One: Must have exactly 1 message (1-of-1 consensus)
    CScript script;
    script << OP_RETURN << OP_ORACLE;

    // Version byte (0x01 = Phase One compact format)
    script << std::vector<unsigned char>{0x01};

    // Phase One: Single oracle message (chainparams verification)
    const COraclePriceMessage& msg = bundle.messages[0];

    // Compact data: oracle_id (1 byte) + price (8 bytes) + timestamp (8 bytes)
    std::vector<unsigned char> compact_data;
    compact_data.reserve(17);

    // Oracle ID (uint8 for Phase One, expandable to uint32 for Phase Two)
    compact_data.push_back(static_cast<unsigned char>(msg.oracle_id & 0xFF));

    // Price in micro-USD (uint64, little-endian)
    uint64_t price = msg.price_micro_usd;
    for (int i = 0; i < 8; ++i) {
        compact_data.push_back(static_cast<unsigned char>((price >> (i * 8)) & 0xFF));
    }

    // Timestamp (int64, little-endian)
    int64_t timestamp = msg.timestamp;
    for (int i = 0; i < 8; ++i) {
        compact_data.push_back(static_cast<unsigned char>((timestamp >> (i * 8)) & 0xFF));
    }

    script << compact_data;

    return script;
}

bool OracleBundleManager::ExtractOracleBundle(const CTransaction& coinbase_tx, COracleBundle& bundle) const
{
    // Look for OP_RETURN output with OP_ORACLE marker
    for (const auto& output : coinbase_tx.vout) {
        if (output.scriptPubKey.size() > 2 && output.scriptPubKey[0] == OP_RETURN) {
            // Check for OP_ORACLE opcode at byte 1
            if (output.scriptPubKey.size() >= 4 &&
                output.scriptPubKey[1] == OP_ORACLE) {

                try {
                    // Extract data chunks
                    std::vector<unsigned char> data;
                    auto script_it = output.scriptPubKey.begin() + 2; // Skip OP_RETURN + OP_ORACLE

                    while (script_it < output.scriptPubKey.end()) {
                        if (*script_it <= 75) { // Direct push (1-75 bytes)
                            unsigned char chunk_size = *script_it;
                            ++script_it;
                            if (script_it + chunk_size <= output.scriptPubKey.end()) {
                                data.insert(data.end(), script_it, script_it + chunk_size);
                                script_it += chunk_size;
                            } else {
                                break;
                            }
                        } else if (*script_it == 0x4c) { // OP_PUSHDATA1: next byte is length
                            ++script_it;
                            if (script_it >= output.scriptPubKey.end()) break;
                            unsigned int chunk_size = *script_it;
                            ++script_it;
                            if (script_it + chunk_size <= output.scriptPubKey.end()) {
                                data.insert(data.end(), script_it, script_it + chunk_size);
                                script_it += chunk_size;
                            } else {
                                break;
                            }
                        } else if (*script_it == 0x4d) { // OP_PUSHDATA2: next 2 bytes are length (LE)
                            ++script_it;
                            if (script_it + 2 > output.scriptPubKey.end()) break;
                            unsigned int chunk_size = *script_it | (*(script_it + 1) << 8);
                            script_it += 2;
                            if (script_it + chunk_size <= output.scriptPubKey.end()) {
                                data.insert(data.end(), script_it, script_it + chunk_size);
                                script_it += chunk_size;
                            } else {
                                break;
                            }
                        } else {
                            break;
                        }
                    }

                    if (data.empty()) {
                        return false;
                    }

                    // Check version byte
                    if (data[0] == 0x01) {
                        // Phase One compact format: oracle_id (1) + price (8) + timestamp (8) = 17 bytes
                        if (data.size() < 18) { // 1 (version) + 17 (data)
                            LogPrintf("Oracle: Invalid Phase One bundle size: %d\n", data.size());
                            return false;
                        }

                        COraclePriceMessage msg;

                        // Parse oracle_id (uint8)
                        msg.oracle_id = data[1];

                        // Parse price (uint64, little-endian)
                        uint64_t price = 0;
                        for (int i = 0; i < 8; ++i) {
                            price |= (static_cast<uint64_t>(data[2 + i]) << (i * 8));
                        }
                        msg.price_micro_usd = price;

                        // Parse timestamp (int64, little-endian)
                        int64_t timestamp = 0;
                        for (int i = 0; i < 8; ++i) {
                            timestamp |= (static_cast<int64_t>(data[10 + i]) << (i * 8));
                        }
                        msg.timestamp = timestamp;

                        // Set remaining fields (not in compact format)
                        msg.block_height = 0; // Not needed for Phase One
                        msg.nonce = 0;

                        // Phase One: Get oracle pubkey from chainparams for verification
                        // Compact format doesn't include signature/pubkey (would exceed OP_RETURN size limit)
                        const CChainParams& chainparams = Params();
                        const OracleNodeInfo* oracle_info = chainparams.GetOracleNode(msg.oracle_id);
                        if (oracle_info) {
                            msg.oracle_pubkey = XOnlyPubKey(oracle_info->pubkey);
                            // Schnorr signature is NOT in compact format (verified at bundle creation time)
                            // For Phase One, trust is based on chainparams oracle pubkey
                        }

                        // Create bundle with single message
                        bundle.messages.clear();
                        bundle.messages.push_back(msg);
                        bundle.median_price_micro_usd = price;
                        bundle.timestamp = timestamp;
                        bundle.epoch = GetCurrentEpoch(msg.block_height);

                        return true;
                    }
                    else if (data[0] == 0x02) {
                        // Phase Two: Full signature format
                        // Layout: version(1) + num_msgs(1) + price(8) + timestamp(8) + N*(oracle_id(1)+sig(64))
                        if (data.size() < 18) { // minimum: version + num_msgs + price + timestamp
                            return false;
                        }
                        
                        uint8_t num_messages = data[1];
                        
                        // Validate we have enough data for all messages
                        size_t expected_size = 1 + 1 + 8 + 8 + num_messages * 65; // version + header + per-msg
                        if (data.size() < expected_size) {
                            LogPrintf("Oracle: Phase Two data too short: %zu < %zu (for %d messages)\n",
                                     data.size(), expected_size, num_messages);
                            return false;
                        }
                        
                        // Parse consensus price (uint64, little-endian)
                        uint64_t price = 0;
                        for (int i = 0; i < 8; ++i) {
                            price |= (static_cast<uint64_t>(data[2 + i]) << (i * 8));
                        }
                        
                        // Parse consensus timestamp (int64, little-endian)
                        int64_t timestamp = 0;
                        for (int i = 0; i < 8; ++i) {
                            timestamp |= (static_cast<int64_t>(data[10 + i]) << (i * 8));
                        }
                        
                        bundle.messages.clear();
                        const CChainParams& chainparams = Params();
                        
                        // Parse each oracle message (oracle_id + schnorr_sig)
                        size_t offset = 18; // past version + num_msgs + price + timestamp
                        for (uint8_t m = 0; m < num_messages; m++) {
                            COraclePriceMessage msg;
                            msg.oracle_id = data[offset];
                            offset += 1;
                            
                            msg.schnorr_sig.assign(data.begin() + offset, data.begin() + offset + 64);
                            offset += 64;
                            
                            // All oracles in the bundle attested to the same consensus price
                            msg.price_micro_usd = price;
                            msg.timestamp = timestamp;
                            msg.block_height = 0;
                            msg.nonce = 0;
                            
                            // Get oracle pubkey from chainparams for verification
                            const OracleNodeInfo* oracle_info = chainparams.GetOracleNode(msg.oracle_id);
                            if (oracle_info) {
                                msg.oracle_pubkey = XOnlyPubKey(oracle_info->pubkey);
                            }
                            
                            bundle.messages.push_back(msg);
                        }
                        
                        bundle.median_price_micro_usd = price;
                        bundle.timestamp = timestamp;
                        bundle.epoch = 0;
                        
                        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Extracted Phase Two bundle: %d oracles with signatures, price=%llu micro-USD\n",
                                 num_messages, price);
                        
                        return true;
                    }

                    return false;
                }
                catch (const std::exception& e) {
                    LogPrintf("Oracle: Failed to extract oracle bundle: %s\n", e.what());
                }
            }
        }
    }

    return false;
}

CAmount OracleBundleManager::GetConsensusPrice(int32_t epoch) const
{
    COracleBundle bundle = GetCurrentBundle(epoch);
    return bundle.GetConsensusPrice(min_oracle_count);
}

CAmount OracleBundleManager::GetLatestPrice() const
{
    std::lock_guard<std::mutex> lock(mtx_bundles);
    return cached_price;
}

bool OracleBundleManager::UpdateCachedPrice(int32_t epoch)
{
    COracleBundle bundle = GetCurrentBundle(epoch);
    if (bundle.HasConsensus()) {
        CAmount price = bundle.GetConsensusPrice();
        if (price > 0) {
            std::lock_guard<std::mutex> lock(mtx_bundles);
            cached_price = price;
            cached_epoch = epoch;
            last_update_time = GetTime();
            return true;
        }
    }
    return false;
}

bool OracleBundleManager::ValidateOracleBundle(const COracleBundle& bundle, int32_t block_height, const Consensus::Params& params) const
{
    return OracleDataValidator::ValidateOracleBundle(bundle, GetCurrentEpoch(block_height), params);
}

bool OracleBundleManager::ValidateOracleDataInBlock(const CBlock& block, int32_t block_height, const Consensus::Params& params) const
{
    BlockValidationState state;
    return OracleDataValidator::ValidateBlockOracleData(block, nullptr, params, state);
}

bool OracleBundleManager::HasOracleMessage(const uint256& hash) const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    return seen_message_hashes.count(hash) > 0;
}

bool OracleBundleManager::BroadcastMessage(const COraclePriceMessage& message)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    // Validate message before broadcasting
    if (!message.IsValid()) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Cannot broadcast invalid oracle message\n");
        return false;
    }

    // Add message to our own collection first
    if (!AddOracleMessage(message)) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Failed to add message to local storage before broadcast\n");
        return false;
    }

    // Broadcast to P2P network
    if (!m_connman) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Cannot broadcast - no P2P connection available\n");
        return false;
    }

    // Push message to all connected peers using OraclePriceMsg wrapper
    OraclePriceMsg price_msg;
    price_msg.price_message = message;
    m_connman->ForEachNode([this, &price_msg](CNode* node) {
        m_connman->PushMessage(node,
            CNetMsgMaker(node->GetCommonVersion()).Make(
                NetMsgType::ORACLEPRICE, price_msg));
    });

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Broadcast oracle message to network: oracle_id=%d, price=%llu micro-USD\n",
             message.oracle_id, message.price_micro_usd);

    return true;
}

void OracleBundleManager::SetConnman(CConnman* connman)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    m_connman = connman;
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: P2P connection manager set for OracleBundleManager\n");
}

void OracleBundleManager::ProcessIncomingMessage(const COraclePriceMessage& message)
{
    AddOracleMessage(message);
}

OracleBundleManager::OracleStats OracleBundleManager::GetStats() const
{
    OracleStats stats;

    {
        std::lock_guard<std::recursive_mutex> lock(mtx_messages);
        stats.pending_messages = pending_messages.size();
    }

    {
        std::lock_guard<std::mutex> lock(mtx_bundles);
        stats.active_bundles = epoch_bundles.size();
        stats.latest_price = cached_price;
        stats.latest_epoch = cached_epoch;
        stats.last_update = last_update_time;
        stats.has_consensus = cached_price > 0;
    }

    return stats;
}

OracleBundleManager& OracleBundleManager::GetInstance()
{
    if (!g_oracle_bundle_manager) {
        g_oracle_bundle_manager = std::make_unique<OracleBundleManager>();
    }
    return *g_oracle_bundle_manager;
}

void OracleBundleManager::Initialize()
{
    OracleBundleManager& manager = GetInstance();
    const Consensus::Params& consensus = Params().GetConsensus();

    // Set consensus requirements from chain parameters
    manager.min_oracle_count = consensus.nOracleRequiredMessages;
    manager.total_oracle_count = consensus.nOracleTotalOracles;
    // Note: epoch_length is not a member variable in header, but nOracleEpochLength is in consensus

    LogPrintf("Oracle: Initialized with %d-of-%d consensus, epoch length %d blocks\n",
              manager.min_oracle_count, manager.total_oracle_count, consensus.nOracleEpochLength);

    // Verify oracle public keys are configured
    if (consensus.vOraclePublicKeys.empty()) {
        LogPrintf("Oracle: WARNING - No oracle public keys configured\n");
        manager.SetEnabled(false);
    } else {
        LogPrintf("Oracle: %d oracle public keys configured\n", consensus.vOraclePublicKeys.size());
        manager.SetEnabled(true);
    }

    // Log oracle configuration
    LogPrintf("Oracle: Testnet configured with %zu oracle keys, %d-of-%d consensus\n",
             consensus.vOraclePublicKeys.size(),
             consensus.nOracleRequiredMessages, consensus.nOracleTotalOracles);

    // Check epoch length is reasonable
    if (consensus.nOracleEpochLength < 144 || consensus.nOracleEpochLength > 10080) {
        LogPrintf("Oracle: WARNING - Unusual epoch length: %d blocks\n", consensus.nOracleEpochLength);
    }
}

void OracleBundleManager::Shutdown()
{
    if (g_oracle_bundle_manager) {
        g_oracle_bundle_manager.reset();
        LogPrintf("Oracle: Oracle Bundle Manager shut down\n");
    }
}

void OracleBundleManager::LoadPricesFromChain(ChainstateManager& chainman)
{
    OracleBundleManager& manager = GetInstance();
    const Consensus::Params& consensus = Params().GetConsensus();

    LOCK(cs_main);

    // Get the active chain tip
    CBlockIndex* pindex = chainman.ActiveChain().Tip();
    if (!pindex) {
        LogPrintf("Oracle: No active chain tip, skipping price loading\n");
        return;
    }

    int tip_height = pindex->nHeight;

    // Only scan if DigiDollar is active via BIP9 deployment
    if (!DigiDollar::IsDigiDollarEnabled(pindex, chainman)) {
        LogPrintf("Oracle: DigiDollar not yet active (BIP9) at height %d, skipping price loading\n",
                 tip_height);
        return;
    }

    // Scan back 20 blocks to find recent oracle prices (default validity window)
    static constexpr int ORACLE_VALIDITY_BLOCKS = 20;
    int scan_depth = std::min(ORACLE_VALIDITY_BLOCKS, tip_height);
    int prices_found = 0;

    LogPrintf("Oracle: Scanning last %d blocks for oracle prices (height %d to %d)...\n",
             scan_depth, tip_height - scan_depth + 1, tip_height);

    for (int height = tip_height; height >= tip_height - scan_depth + 1 && height >= 0; --height) {
        CBlockIndex* block_index = chainman.ActiveChain()[height];
        if (!block_index) continue;

        CBlock block;
        if (!chainman.m_blockman.ReadBlockFromDisk(block, *block_index)) {
            LogPrintf("Oracle: Failed to read block at height %d\n", height);
            continue;
        }

        // Extract oracle bundle from coinbase
        if (block.vtx.empty()) continue;
        const CTransaction& coinbase = *block.vtx[0];

        COracleBundle bundle;
        if (manager.ExtractOracleBundle(coinbase, bundle)) {
            if (bundle.median_price_micro_usd > 0) {
                manager.UpdatePriceCache(height, bundle.median_price_micro_usd);
                prices_found++;
                LogPrintf("Oracle: Found price %llu micro-USD at height %d\n",
                         bundle.median_price_micro_usd, height);
            }
        }
    }

    if (prices_found > 0) {
        LogPrintf("Oracle: Loaded %d oracle prices from blockchain, latest price: %llu micro-USD\n",
                 prices_found, manager.GetLatestPrice());
    } else {
        LogPrintf("Oracle: No oracle prices found in recent blocks\n");
    }
}

void OracleBundleManager::Clear()
{
    // Clear all state for test isolation
    {
        std::lock_guard<std::mutex> lock(mtx_bundles);
        epoch_bundles.clear();
        cached_price = 0;
        cached_epoch = -1;
        last_update_time = 0;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mtx_messages);
        pending_messages.clear();
        seen_message_hashes.clear();
    }

    {
        std::lock_guard<std::mutex> lock(mtx_price_cache);
        height_to_price.clear();
    }

    LogPrintf("Oracle: Cleared all bundle manager state\n");
}

bool OracleBundleManager::ValidateConfiguration() const
{
    const Consensus::Params& consensus = Params().GetConsensus();

    // Validate oracle configuration
    if (consensus.vOraclePublicKeys.empty()) {
        LogPrintf("Oracle: ERROR - No oracle public keys configured\n");
        return false;
    }

    if (consensus.nOracleRequiredMessages > consensus.nOracleTotalOracles) {
        LogPrintf("Oracle: ERROR - Required messages (%d) exceeds total oracles (%d)\n",
                 consensus.nOracleRequiredMessages, consensus.nOracleTotalOracles);
        return false;
    }

    LogPrintf("Oracle: Configuration valid - %zu keys, %d-of-%d consensus\n",
             consensus.vOraclePublicKeys.size(),
             consensus.nOracleRequiredMessages, consensus.nOracleTotalOracles);

    // Check epoch length is reasonable
    if (consensus.nOracleEpochLength < 144 || consensus.nOracleEpochLength > 10080) {
        LogPrintf("Oracle: WARNING - Unusual epoch length: %d blocks\n", consensus.nOracleEpochLength);
    }

    // Check min_oracle_count matches consensus
    if (min_oracle_count != consensus.nOracleRequiredMessages) {
        LogPrintf("Oracle: ERROR - min_oracle_count (%d) does not match consensus.nOracleRequiredMessages (%d)\n",
                 min_oracle_count, consensus.nOracleRequiredMessages);
        return false;
    }

    // Check total_oracle_count matches consensus
    if (total_oracle_count != consensus.nOracleTotalOracles) {
        LogPrintf("Oracle: ERROR - total_oracle_count (%d) does not match consensus.nOracleTotalOracles (%d)\n",
                 total_oracle_count, consensus.nOracleTotalOracles);
        return false;
    }

    return true;
}

bool OracleBundleManager::TryCreateBundle(int32_t epoch)
{
    std::lock_guard<std::recursive_mutex> messages_lock(mtx_messages);

    // Check if we have enough messages for consensus
    if (pending_messages.size() < static_cast<size_t>(min_oracle_count)) {
        return false;
    }

    // Create bundle for current epoch
    COracleBundle bundle(epoch);

    // Add messages to bundle
    for (const auto& [oracle_id, message] : pending_messages) {
        if (!bundle.AddMessage(message)) {
            LogPrintf("Oracle: Failed to add message from oracle %d to bundle\n", oracle_id);
        }
    }

    // Check if bundle has consensus
    if (bundle.HasConsensus(min_oracle_count)) {
        // Calculate and set median price
        bundle.median_price_micro_usd = bundle.GetConsensusPrice(min_oracle_count);

        // Set bundle timestamp to the latest message timestamp
        bundle.timestamp = GetTime();
        for (const auto& msg : bundle.messages) {
            if (msg.timestamp > bundle.timestamp) {
                bundle.timestamp = msg.timestamp;
            }
        }

        UpdateBundle(bundle);
        LogPrintf("Oracle: Created consensus bundle for epoch %d with %d messages (min required: %d), median price=%llu\n",
                 epoch, bundle.messages.size(), min_oracle_count, bundle.median_price_micro_usd);
        return true;
    }

    return false;
}

void OracleBundleManager::UpdateEpochBundle(int32_t epoch)
{
    TryCreateBundle(epoch);
}

bool OracleBundleManager::IsValidOracleMessage(const COraclePriceMessage& message) const
{
    // Common price bounds validation (applies to both phases)
    if (message.price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD) return false;
    if (message.price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) return false;

    // Phase One: min_oracle_count == 1 (regtest/testing mode)
    if (min_oracle_count == 1) {
        // Field validation (timestamp freshness, price range) via IsValid()
        if (!message.IsValid()) return false;

        // SECURITY FIX (DGB-SEC-002): Verify against chainparams pubkey
        return VerifyOracleMessageWithChainparamsKey(message);
    }

    // Phase Two (min_oracle_count > 1): Verify against chainparams pubkey
    return VerifyOracleMessageWithChainparamsKey(message);
}

std::vector<uint32_t> OracleBundleManager::GetActiveOraclesForEpoch(int32_t epoch) const
{
    const CChainParams& params = Params();
    const std::vector<OracleNodeInfo>& all_oracles = params.GetOracleNodes();

    std::vector<OracleNodeInfo> selected_oracles = SelectOraclesForEpoch(all_oracles, epoch);
    std::vector<uint32_t> oracle_ids;

    for (const auto& oracle : selected_oracles) {
        oracle_ids.push_back(oracle.id);
    }

    return oracle_ids;
}

bool OracleBundleManager::HasRequiredSignatures(const COracleBundle& bundle, int32_t block_height) const
{
    // Check that bundle has minimum required signatures from active oracles
    int32_t epoch = GetCurrentEpoch(block_height);
    std::vector<uint32_t> active_oracles = GetActiveOraclesForEpoch(epoch);

    size_t valid_signatures = 0;
    for (const auto& message : bundle.messages) {
        // Check if oracle is active for this epoch
        auto it = std::find(active_oracles.begin(), active_oracles.end(), message.oracle_id);
        if (it != active_oracles.end()) {
            valid_signatures++;
        }
    }

    return valid_signatures >= static_cast<size_t>(min_oracle_count);
}

void OracleBundleManager::UpdatePriceCache(int height, uint64_t price_micro_usd)
{
    // Update the height-to-price map
    {
        std::lock_guard<std::mutex> lock(mtx_price_cache);
        height_to_price[height] = price_micro_usd;

        // Keep cache size limited (last 1000 blocks)
        if (height_to_price.size() > 1000) {
            height_to_price.erase(height_to_price.begin());
        }
    }

    // CRITICAL: Also update cached_price so GetLatestPrice() returns the correct value
    // This is the price that GetCurrentOraclePrice() uses for the DigiDollar system
    // The price comes from oracle data embedded in blocks - this is the consensus price
    {
        std::lock_guard<std::mutex> lock(mtx_bundles);
        cached_price = static_cast<CAmount>(price_micro_usd);
        last_update_time = GetTime();
    }

    LogPrintf("Oracle: Price cache updated for height %d: %llu micro-USD ($%.6f) - cached_price updated\n",
             height, price_micro_usd, price_micro_usd / 1000000.0);
}

uint64_t OracleBundleManager::GetOraclePriceForHeight(int height) const
{
    std::lock_guard<std::mutex> lock(mtx_price_cache);
    auto it = height_to_price.find(height);
    if (it != height_to_price.end()) {
        return it->second;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: No price available in cache for height %d\n", height);
    return 0; // No price available
}

void OracleBundleManager::RemovePriceCache(int height)
{
    std::lock_guard<std::mutex> lock(mtx_price_cache);
    auto it = height_to_price.find(height);
    if (it != height_to_price.end()) {
        height_to_price.erase(it);
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Removed price cache for height %d\n", height);
    }
}

/**
 * OracleDataValidator Implementation
 */

bool OracleDataValidator::ValidateBlockOracleData(const CBlock& block, const CBlockIndex* pindex_prev, const Consensus::Params& params, BlockValidationState& state)
{
    // Phase One: Oracle validation on testnet and regtest (for unit tests)
    if (Params().GetChainType() != ChainType::TESTNET && Params().GetChainType() != ChainType::REGTEST) {
        return true; // Oracle validation disabled on mainnet
    }

    // Extract oracle bundle from coinbase OP_RETURN (output index 1)
    if (block.vtx.empty()) {
        return true; // No transactions, nothing to validate
    }

    const CTransaction& coinbase = *block.vtx[0];

    // Determine block height
    int32_t block_height = 0;
    if (pindex_prev) {
        block_height = pindex_prev->nHeight + 1;
    } else {
        // Extract height from coinbase scriptSig (BIP34)
        if (!coinbase.vin.empty() && coinbase.vin[0].scriptSig.size() >= 1) {
            CScript::const_iterator pc = coinbase.vin[0].scriptSig.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;
            if (coinbase.vin[0].scriptSig.GetOp(pc, opcode, data) && !data.empty()) {
                block_height = CScriptNum(data, true).getint();
            }
        }
    }

    // Check if DigiDollar is active via BIP9 deployment
    if (pindex_prev) {
        if (!DigiDollar::IsDigiDollarEnabled(pindex_prev, params)) {
            return true; // Oracle validation not required before BIP9 activation
        }
    } else {
        // Fallback when no block index available — use height-based check
        if (block_height < params.nDDActivationHeight) {
            return true;
        }
    }
    if (coinbase.vout.size() < 2) {
        // Allow blocks without oracle data during transition
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: No oracle bundle in block %d (transition period)\n", block_height);
        return true;
    }

    // Check for oracle bundle in output 1 (OP_RETURN)
    const CTxOut& oracle_output = coinbase.vout[1];
    if (!oracle_output.scriptPubKey.IsUnspendable()) {
        // Not an OP_RETURN, allow during transition
        return true;
    }

    // Extract and validate oracle bundle
    if (oracle_output.scriptPubKey.size() <= 2) {
        // Empty OP_RETURN, allow during transition
        return true;
    }

    // Check for OP_ORACLE opcode at byte 1
    if (oracle_output.scriptPubKey.size() >= 2 && oracle_output.scriptPubKey[1] != OP_ORACLE) {
        // Not an oracle output, allow during transition
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Block %d output is OP_RETURN but not OP_ORACLE (transition period)\n", block_height);
        return true;
    }

    // Extract oracle bundle using OracleBundleManager's parser (handles compact Phase One format)
    COracleBundle bundle;
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    if (!manager.ExtractOracleBundle(coinbase, bundle)) {
        // During transition period, allow blocks without valid oracle bundles
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Block %d could not extract oracle bundle (transition period)\n", block_height);
        return true;
    }

    // IMPORTANT: Once we successfully extract a bundle, we MUST validate it fully
    // No transition period leniency for bundles that are present but invalid

    // STEP 7: PHASE-AWARE CONSENSUS VALIDATION
    const Consensus::Params& consensusParams_ref = Params().GetConsensus();

    // Phase 1: Use generic bundle.IsValid() which checks Phase 1 signatures
    // Phase 2: Skip generic IsValid() — ValidatePhaseTwoBundle does Phase 2-specific validation
    if (block_height < consensusParams_ref.nDigiDollarPhase2Height) {
        if (!bundle.IsValid(block.nTime)) {
            LogPrintf("Oracle: Invalid oracle bundle in block %d\n", block_height);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-bundle", "invalid oracle bundle structure");
        }
    } else {
        // Phase 2: Basic structural checks only (signatures checked by ValidatePhaseTwoBundle)
        if (bundle.messages.empty()) {
            LogPrintf("Oracle: Empty oracle bundle in block %d\n", block_height);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-bundle", "empty oracle bundle");
        }
    }

    if (block_height >= consensusParams_ref.nDigiDollarPhase2Height) {
        // Phase Two: Full multi-oracle validation with on-chain signature verification
        if (!OracleBundleManager::ValidatePhaseTwoBundle(bundle, consensusParams_ref)) {
            LogPrintf("Oracle: Phase Two bundle validation failed at block %d\n", block_height);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                                "bad-oracle-phase2",
                                "Phase Two oracle bundle validation failed");
        }
    } else {
        // Phase One: Exactly 1 message required
        if (bundle.messages.size() != 1) {
            LogPrintf("Oracle: Phase One requires exactly 1 oracle message, got %zu\n",
                     bundle.messages.size());
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                                "bad-oracle-consensus",
                                strprintf("Phase One requires exactly 1 oracle message, got %zu",
                                         bundle.messages.size()));
        }
        
        // Phase One: Median price must equal single message price
        const COraclePriceMessage& msg_p1 = bundle.messages[0];
        if (bundle.median_price_micro_usd != msg_p1.price_micro_usd) {
            LogPrintf("Oracle: Median price mismatch: bundle=%llu, message=%llu\n",
                     bundle.median_price_micro_usd, msg_p1.price_micro_usd);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                                "bad-oracle-median",
                                strprintf("Median price mismatch: bundle=%llu, message=%llu",
                                         bundle.median_price_micro_usd, msg_p1.price_micro_usd));
        }
    }

    // Use first message for remaining validation checks
    const COraclePriceMessage& msg = bundle.messages[0];

    // Phase 1: Verify Schnorr signature (Phase 2 signatures verified in ValidatePhaseTwoBundle)
    if (block_height < consensusParams_ref.nDigiDollarPhase2Height) {
        if (!msg.schnorr_sig.empty()) {
            if (!msg.VerifyPhase2()) {
                LogPrintf("Oracle: Invalid Schnorr signature in oracle message (oracle_id=%d, block=%d)\n",
                         msg.oracle_id, block_height);
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-signature",
                    "Invalid oracle Schnorr signature");
            }
        }
    }

    // Verify oracle timestamp is not too old (max 1 hour = 3600 seconds)
    int64_t oracle_age = block.nTime - bundle.timestamp;
    if (oracle_age > ORACLE_MAX_AGE_SECONDS) {
        LogPrintf("Oracle: Oracle message too old: age=%lld seconds (max=%d) in block %d\n",
                 (long long)oracle_age, ORACLE_MAX_AGE_SECONDS, block_height);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-timestamp",
            strprintf("Oracle timestamp too old: age=%lld seconds (max=%d)", (long long)oracle_age, ORACLE_MAX_AGE_SECONDS));
    }

    // Verify oracle timestamp is not in the future (with 60 second tolerance for clock skew)
    if (bundle.timestamp > block.nTime + 60) {
        LogPrintf("Oracle: Oracle timestamp in future: oracle=%lld, block=%u\n",
                 (long long)bundle.timestamp, block.nTime);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-timestamp",
            "Oracle timestamp is in the future");
    }

    // Verify oracle is authorized (skip in REGTEST for unit testing)
    if (Params().GetChainType() != ChainType::REGTEST) {
        const CChainParams& chainparams = Params();
        const OracleNodeInfo* oracle_config = chainparams.GetOracleNode(msg.oracle_id);
        if (!oracle_config || !oracle_config->is_active) {
            LogPrintf("Oracle: Unauthorized oracle ID %d in block %d\n", msg.oracle_id, block_height);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-unauthorized",
                strprintf("Unauthorized oracle ID %d", msg.oracle_id));
        }
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Block %d oracle bundle validated: price=%llu micro-USD\n",
             block_height, bundle.median_price_micro_usd);

    return true;
}

bool OracleDataValidator::ValidateOraclePriceForTx(const CTransaction& tx, CAmount oracle_price, int32_t block_height)
{
    // Basic validation for DigiDollar transactions
    if (oracle_price <= 0) {
        LogPrintf("Oracle: Invalid oracle price %d for transaction validation\n", oracle_price);
        return false;
    }

    // Check price is within reasonable bounds
    if (oracle_price < 100 || oracle_price > 1000000) { // $0.001 to $10 per DGB
        LogPrintf("Oracle: Oracle price %d is outside reasonable bounds\n", oracle_price);
        return false;
    }

    return true;
}

bool OracleDataValidator::ValidateOracleMessage(const COraclePriceMessage& message, const Consensus::Params& params)
{
    // Basic message validation
    if (!message.IsValid()) {
        return false;
    }

    // Verify oracle is authorized and active
    const CChainParams& chainparams = Params();
    const OracleNodeInfo* oracle_config = chainparams.GetOracleNode(message.oracle_id);
    if (!oracle_config || !oracle_config->is_active) {
        return false;
    }

    // SECURITY (DGB-SEC-002): Verify against chainparams pubkey
    return VerifyOracleMessageWithChainparamsKey(message);
}

bool OracleDataValidator::ValidateOracleBundle(const COracleBundle& bundle, int32_t epoch, const Consensus::Params& params)
{
    // Validate epoch
    if (!bundle.ValidateEpoch(epoch)) {
        LogPrintf("Oracle: Invalid epoch in bundle: %d (current: %d)\n", bundle.epoch, epoch);
        return false;
    }

    // Check consensus requirement
    if (!bundle.HasConsensus()) {
        LogPrintf("Oracle: Bundle does not have required consensus (%d messages)\n", bundle.messages.size());
        return false;
    }

    // Validate individual messages
    for (const auto& message : bundle.messages) {
        if (!ValidateOracleMessage(message, params)) {
            LogPrintf("Oracle: Invalid message from oracle %d in bundle\n", message.oracle_id);
            return false;
        }
    }

    return true;
}

bool OracleDataValidator::CheckOracleSignatures(const COracleBundle& bundle, const Consensus::Params& params)
{
    for (const auto& message : bundle.messages) {
        if (!ValidateOracleMessage(message, params)) {
            return false;
        }
    }
    return true;
}

bool OracleDataValidator::CheckOracleEpoch(const COracleBundle& bundle, int32_t current_epoch)
{
    return bundle.ValidateEpoch(current_epoch);
}

bool OracleDataValidator::CheckOracleConsensus(const COracleBundle& bundle)
{
    return bundle.HasConsensus();
}

/**
 * Phase Two Bundle Validation Implementation
 */

bool OracleBundleManager::ValidateBundle(const COracleBundle& bundle, int block_height, const Consensus::Params& params)
{
    if (block_height >= params.nDigiDollarPhase2Height) {
        return ValidatePhaseTwoBundle(bundle, params);
    } else {
        return ValidatePhaseOneBundle(bundle, params);
    }
}

int OracleBundleManager::GetRequiredConsensus(int block_height, const Consensus::Params& params)
{
    if (block_height >= params.nDigiDollarPhase2Height) {
        return params.nOracleRequiredMessages;  // 3 for testnet, 8 for mainnet
    }
    return 1;  // Phase One: 1-of-1
}

bool OracleBundleManager::ValidatePhaseOneBundle(const COracleBundle& bundle, const Consensus::Params& params)
{
    // Phase One: Must have exactly 1 message (1-of-1 consensus)
    if (bundle.messages.size() != 1) {
        LogPrintf("Oracle: Phase One requires exactly 1 oracle message, got %zu\n", bundle.messages.size());
        return false;
    }

    const COraclePriceMessage& msg = bundle.messages[0];

    // Verify message is valid
    if (!msg.IsValid()) {
        LogPrintf("Oracle: Phase One message validation failed\n");
        return false;
    }

    // Verify median price matches single message price
    if (bundle.median_price_micro_usd != msg.price_micro_usd) {
        LogPrintf("Oracle: Phase One median price mismatch: bundle=%llu, message=%llu\n",
                 bundle.median_price_micro_usd, msg.price_micro_usd);
        return false;
    }

    // Phase One: Schnorr signature verification (if signature is present)
    if (!msg.schnorr_sig.empty()) {
        // SECURITY (DGB-SEC-002): Verify against chainparams pubkey
        if (!VerifyOracleMessageWithChainparamsKey(msg)) {
            LogPrintf("Oracle: Phase One signature verification failed for oracle %d\n", msg.oracle_id);
            return false;
        }
    }

    return true;
}

bool OracleBundleManager::ValidatePhaseTwoBundle(const COracleBundle& bundle, const Consensus::Params& params)
{
    // Check minimum message count
    int min_required = params.nOracleRequiredMessages;
    if (bundle.messages.size() < static_cast<size_t>(min_required)) {
        LogPrintf("Oracle: Phase Two requires at least %d messages, got %zu\n",
                 min_required, bundle.messages.size());
        return false;
    }

    // Check for duplicate oracle IDs
    std::set<uint32_t> seen_oracles;
    for (const auto& msg : bundle.messages) {
        if (seen_oracles.count(msg.oracle_id) > 0) {
            LogPrintf("Oracle: Phase Two detected duplicate oracle ID %d\n", msg.oracle_id);
            return false;
        }
        seen_oracles.insert(msg.oracle_id);
    }

    // Get active oracle set for current epoch
    std::vector<uint32_t> active_oracles = OracleBundleManager::GetInstance().GetActiveOraclesForEpoch(bundle.epoch);

    // Verify each message is from an active oracle and has valid signature
    int valid_count = 0;
    for (const auto& msg : bundle.messages) {
        // Check if oracle is in active set
        auto it = std::find(active_oracles.begin(), active_oracles.end(), msg.oracle_id);
        if (it == active_oracles.end()) {
            LogPrintf("Oracle: Phase Two oracle %d not in active set for epoch %d\n",
                     msg.oracle_id, bundle.epoch);
            continue;  // Skip invalid oracle, don't fail entire bundle
        }

        // Verify basic message fields (price range)
        if (msg.price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD ||
            msg.price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) {
            LogPrintf("Oracle: Phase Two message price out of range for oracle %d\n", msg.oracle_id);
            continue;
        }

        // Verify Schnorr signature using Phase 2 hash (oracle_id + price + timestamp only)
        if (!msg.schnorr_sig.empty()) {
            // SECURITY (DGB-SEC-002): Verify against chainparams pubkey
            if (!VerifyOracleMessageWithChainparamsKey(msg)) {
                LogPrintf("Oracle: Phase Two signature verification failed for oracle %d\n", msg.oracle_id);
                continue;  // Skip message with invalid signature
            }
        } else {
            LogPrintf("Oracle: Phase Two message missing signature for oracle %d\n", msg.oracle_id);
            continue;  // Skip message without signature
        }

        valid_count++;
    }

    // Check if we have enough valid signatures
    if (valid_count < min_required) {
        LogPrintf("Oracle: Phase Two requires %d valid signatures, got %d\n",
                 min_required, valid_count);
        return false;
    }

    // Verify consensus price calculation
    CAmount calculated_price = CalculateConsensusPrice(bundle, params);
    if (calculated_price != static_cast<CAmount>(bundle.median_price_micro_usd)) {
        LogPrintf("Oracle: Phase Two consensus price mismatch: calculated=%lld, bundle=%llu\n",
                 calculated_price, bundle.median_price_micro_usd);
        return false;
    }

    LogPrintf("Oracle: Phase Two bundle validated successfully: %d valid signatures (min: %d), price=%llu micro-USD\n",
             valid_count, min_required, bundle.median_price_micro_usd);

    return true;
}

CAmount OracleBundleManager::CalculateConsensusPrice(const COracleBundle& bundle, const Consensus::Params& params)
{
    std::vector<CAmount> prices;
    for (const auto& msg : bundle.messages) {
        if (msg.IsValid()) {
            prices.push_back(static_cast<CAmount>(msg.price_micro_usd));
        }
    }

    if (prices.empty()) {
        return 0;  // No valid prices
    }

    // Sort prices for IQR calculation
    std::sort(prices.begin(), prices.end());

    // If less than 4 prices, just return median without outlier filtering
    if (prices.size() < 4) {
        size_t mid = prices.size() / 2;
        if (prices.size() % 2 == 0) {
            return (prices[mid - 1] + prices[mid]) / 2;
        }
        return prices[mid];
    }

    // Apply IQR outlier filtering (1.5 * IQR rule)
    size_t q1_idx = prices.size() / 4;
    size_t q3_idx = (prices.size() * 3) / 4;
    CAmount q1 = prices[q1_idx];
    CAmount q3 = prices[q3_idx];
    CAmount iqr = q3 - q1;
    CAmount lower_bound = q1 - (iqr * 3 / 2);  // 1.5 * IQR below Q1
    CAmount upper_bound = q3 + (iqr * 3 / 2);  // 1.5 * IQR above Q3

    // Filter outliers
    std::vector<CAmount> filtered;
    for (CAmount price : prices) {
        if (price >= lower_bound && price <= upper_bound) {
            filtered.push_back(price);
        }
    }

    // If filtering removed all prices, fall back to unfiltered median
    if (filtered.empty()) {
        size_t mid = prices.size() / 2;
        if (prices.size() % 2 == 0) {
            return (prices[mid - 1] + prices[mid]) / 2;
        }
        return prices[mid];
    }

    // Calculate median of filtered prices
    std::sort(filtered.begin(), filtered.end());
    size_t mid = filtered.size() / 2;
    if (filtered.size() % 2 == 0) {
        return (filtered[mid - 1] + filtered[mid]) / 2;
    }
    return filtered[mid];
}

/**
 * OracleIntegration Utility Functions
 */

namespace OracleIntegration {

CAmount GetCurrentOraclePrice()
{
    // In RegTest mode, use MockOracleManager for testing
    if (Params().GetChainType() == ChainType::REGTEST) {
        CAmount mockPrice = MockOracleManager::GetInstance().GetCurrentPrice();
        if (mockPrice > 0) {
            return mockPrice;
        }
    }

    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    CAmount price_micro_usd = manager.GetLatestPrice();

    // Phase One (testnet): cached_price is stored in micro-USD format
    // Convert micro-USD to cents: cents = micro-USD / 10,000
    // e.g. 50,000 micro-USD = $0.05 = 5 cents
    // e.g. 6,310 micro-USD = $0.00631 = 0.631 cents (rounds to 1 cent)
    if (price_micro_usd > 0) {
        CAmount price_cents = (price_micro_usd + 5000) / 10000; // Round to nearest cent
        if (price_cents == 0) {
            price_cents = 1; // Minimum 1 cent for any non-zero price
        }
        // Use LogPrint instead of LogPrintf to avoid log spam during sync
        LogPrint(BCLog::NET, "Oracle: GetCurrentOraclePrice returning %lld micro-USD = %lld cents ($%.4f)\n",
                 price_micro_usd, price_cents, price_cents / 100.0);
        return price_cents;
    }

    // No oracle price available - return 0 to indicate no data
    // Caller must handle this case appropriately
    LogPrintf("Oracle: No oracle price available, returning 0\n");
    return 0;
}

CAmount GetCurrentOraclePriceMicroUSD()
{
    // In RegTest mode, use MockOracleManager for testing
    // MockOracleManager stores and returns micro-USD directly (set via setmockoracleprice RPC)
    if (Params().GetChainType() == ChainType::REGTEST) {
        CAmount mockPriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
        if (mockPriceMicroUSD > 0) {
            LogPrintf("Oracle: GetCurrentOraclePriceMicroUSD returning %lld micro-USD ($%.6f) from MockOracleManager\n",
                     mockPriceMicroUSD, mockPriceMicroUSD / 1000000.0);
            return mockPriceMicroUSD;
        }
    }

    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    CAmount price_micro_usd = manager.GetLatestPrice();

    if (price_micro_usd > 0) {
        LogPrintf("Oracle: GetCurrentOraclePriceMicroUSD returning %lld micro-USD ($%.6f)\n",
                 price_micro_usd, price_micro_usd / 1000000.0);
        return price_micro_usd;
    }

    // No oracle price available
    LogPrintf("Oracle: No oracle price available in GetCurrentOraclePriceMicroUSD, returning 0\n");
    return 0;
}

CAmount GetOraclePriceForHeight(int nHeight)
{
    // Get oracle bundle manager instance
    OracleBundleManager& manager = OracleBundleManager::GetInstance();

    // Phase One: Try to get price from cache first (populated by ConnectBlock)
    // This takes priority over MockOracleManager for accurate integration testing
    uint64_t cached_price = manager.GetOraclePriceForHeight(nHeight);
    if (cached_price > 0) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using cached price for height %d: %llu micro-USD ($%.6f)\n",
                 nHeight, cached_price, cached_price / 1000000.0);
        return static_cast<CAmount>(cached_price);
    }

    // In RegTest mode, fall back to MockOracleManager for simple tests
    if (Params().GetChainType() == ChainType::REGTEST) {
        CAmount mockPrice = MockOracleManager::GetInstance().GetCurrentPrice();
        if (mockPrice > 0) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using mock price for height %d: %lld (MockOracleManager fallback)\n",
                     nHeight, mockPrice);
            return mockPrice;
        }
    }

    // Fallback: Try to get from current epoch bundle (for mempool transactions)
    int32_t epoch = GetCurrentEpoch(nHeight);
    COracleBundle bundle = manager.GetCurrentBundle(epoch);

    if (bundle.HasConsensus()) {
        CAmount price = bundle.GetConsensusPrice();
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using current epoch price for height %d (epoch %d): %lld micro-USD\n",
                 nHeight, epoch, price);
        return price;
    }

    // Try previous epoch as fallback
    if (epoch > 0) {
        COracleBundle prev_bundle = manager.GetCurrentBundle(epoch - 1);
        if (prev_bundle.HasConsensus()) {
            CAmount price = prev_bundle.GetConsensusPrice();
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using previous epoch price for height %d: %lld micro-USD\n",
                     nHeight, price);
            return price;
        }
    }

    // No oracle price available
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: No price available for height %d\n", nHeight);
    return 0;
}

bool IsOracleSystemReady()
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    return manager.IsEnabled() && manager.GetLatestPrice() > 0;
}

COracleBundle GetOracleBundleForHeight(int32_t block_height)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    int32_t epoch = GetCurrentEpoch(block_height);
    return manager.GetCurrentBundle(epoch);
}

bool ValidateOracleRequirements(const CTransaction& tx, int32_t block_height)
{
    CAmount oracle_price = GetCurrentOraclePrice();
    return OracleDataValidator::ValidateOraclePriceForTx(tx, oracle_price, block_height);
}

} // namespace OracleIntegration