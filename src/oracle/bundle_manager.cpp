// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <oracle/bundle_manager.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>

#include <chainparams.h>
#include <common/args.h>
#include <consensus/consensus.h>
#include <consensus/digidollar.h>
#include <consensus/volatility.h>
#include <digidollar/digidollar.h>
#include <kernel/chainparams.h>
#include <logging.h>
#include <net.h>
#include <netmessagemaker.h>
#include <oracle/mock_oracle.h>
#include <oracle/musig2_aggregator.h>
#include <oracle/musig2_orchestrator.h>
#include <oracle/musig2_messages.h>
#include <oracle/signing_orchestrator.h>
#include <oracle/musig2_session.h>
#include <oracle/node.h>
#include <primitives/block.h>
#include <primitives/oracle.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/script.h>
#include <script/standard.h>
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <util/time.h>
#include <validation.h>

// Forward declaration for missing functions
extern int32_t GetBestHeight();

// MuSig2 global signing session store (defined in musig2_session.cpp)
extern std::map<int32_t, MuSig2SigningSession> g_oracle_signing_sessions;
extern Mutex g_oracle_signing_sessions_mutex;

// Get current chain height - used by oracle bundle manager
int32_t GetBestHeight() {
    // TODO: Wire up to ChainstateManager properly
    // For now, return 0 if we can't determine height
    return 0;
}

namespace {
bool IsFreshLiveOracleTimestamp(int64_t timestamp, int64_t now)
{
    return timestamp > 0 &&
           timestamp <= now + 60 &&
           now - timestamp <= ORACLE_MAX_AGE_SECONDS;
}

bool IsConsensusMuSig2OracleId(uint32_t oracle_id, const Consensus::Params& params)
{
    return params.nOraclePubkeyCount > 0 &&
           oracle_id < static_cast<uint32_t>(params.nOraclePubkeyCount);
}

bool IsActiveConsensusOracle(uint32_t oracle_id)
{
    const CChainParams& params = Params();
    if (!IsConsensusMuSig2OracleId(oracle_id, params.GetConsensus())) return false;

    const OracleNodeInfo* oracle_config = params.GetOracleNode(oracle_id);
    return oracle_config && oracle_config->is_active;
}

bool IsCompleteMuSig2Bundle(const COracleBundle& bundle)
{
    return bundle.IsMuSig2() &&
           bundle.aggregate_sig.size() == 64 &&
           !bundle.participation_bitmap.empty() &&
           bundle.median_price_micro_usd > 0 &&
           bundle.timestamp > 0;
}

bool HasMuSig2Quorum(const COracleBundle& bundle, const Consensus::Params& params)
{
    if (!IsCompleteMuSig2Bundle(bundle)) return false;

    const std::vector<uint8_t> oracle_ids = MuSig2OracleAggregator::DecodeBitmap(
        bundle.participation_bitmap, static_cast<uint16_t>(params.nOracleTotalOracles));
    if (static_cast<int>(oracle_ids.size()) < params.nOracleConsensusRequired) return false;

    for (uint8_t oracle_id : oracle_ids) {
        if (static_cast<int>(oracle_id) >= params.nOraclePubkeyCount) return false;
    }

    return true;
}

bool BlockTouchesDigiDollar(const CBlock& block)
{
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        if (DigiDollar::HasDigiDollarMarker(*block.vtx[i])) {
            return true;
        }
    }
    return false;
}

bool BlockNeedsOraclePrice(const CBlock& block)
{
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const CTransaction& tx = *block.vtx[i];
        if (!DigiDollar::HasDigiDollarMarker(tx)) continue;
        const DigiDollar::DigiDollarTxType tx_type = DigiDollar::GetDigiDollarTxType(tx);
        if (tx_type == DigiDollar::DD_TX_MINT || tx_type == DigiDollar::DD_TX_REDEEM) {
            return true;
        }
    }
    return false;
}

bool BlockHasDigiDollarMint(const CBlock& block)
{
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const CTransaction& tx = *block.vtx[i];
        if (DigiDollar::HasDigiDollarMarker(tx) &&
            DigiDollar::GetDigiDollarTxType(tx) == DigiDollar::DD_TX_MINT) {
            return true;
        }
    }
    return false;
}

bool ComputeAggregatePubkeyFromConsensusParams(const std::vector<uint8_t>& oracle_ids,
                                               const Consensus::Params& params,
                                               secp256k1_xonly_pubkey& agg_pk,
                                               secp256k1_musig_keyagg_cache& cache)
{
    if (oracle_ids.empty()) return false;
    if (params.nOraclePubkeyCount < 0) return false;
    if (params.vOraclePublicKeys.size() < static_cast<size_t>(params.nOraclePubkeyCount)) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    std::vector<secp256k1_pubkey> pubkeys;
    pubkeys.reserve(oracle_ids.size());
    for (uint8_t id : oracle_ids) {
        if (id >= static_cast<uint8_t>(params.nOraclePubkeyCount)) {
            secp256k1_context_destroy(ctx);
            return false;
        }

        const std::vector<unsigned char> xonly = ParseHex(params.vOraclePublicKeys[id]);
        if (xonly.size() != 32) {
            secp256k1_context_destroy(ctx);
            return false;
        }

        std::vector<unsigned char> compressed;
        compressed.reserve(CPubKey::COMPRESSED_SIZE);
        compressed.push_back(0x02);
        compressed.insert(compressed.end(), xonly.begin(), xonly.end());

        secp256k1_pubkey pk;
        if (!secp256k1_ec_pubkey_parse(ctx, &pk, compressed.data(), compressed.size())) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        pubkeys.push_back(pk);
    }

    std::vector<const secp256k1_pubkey*> pubkey_ptrs(pubkeys.size());
    for (size_t i = 0; i < pubkeys.size(); ++i) {
        pubkey_ptrs[i] = &pubkeys[i];
    }

    const bool ok = secp256k1_musig_pubkey_agg(ctx, &agg_pk, &cache,
                                               pubkey_ptrs.data(), pubkey_ptrs.size()) == 1;
    secp256k1_context_destroy(ctx);
    return ok;
}
} // namespace

//! Global oracle bundle manager instance
std::unique_ptr<OracleBundleManager> g_oracle_bundle_manager;

/**
 * OracleBundleManager Implementation
 */

OracleBundleManager::OracleBundleManager()
{
    const int64_t configured_wait_ms = std::max<int64_t>(0, gArgs.GetIntArg("-oraclebundlewaitms", 2000));
    const int64_t configured_poll_ms = std::max<int64_t>(1, gArgs.GetIntArg("-oraclebundlewaitpollms", 200));
    near_quorum_wait_timeout = std::chrono::milliseconds(configured_wait_ms);
    near_quorum_wait_poll_interval = std::chrono::milliseconds(configured_poll_ms);
    if (near_quorum_wait_timeout.count() > 0 && near_quorum_wait_poll_interval > near_quorum_wait_timeout) {
        near_quorum_wait_poll_interval = near_quorum_wait_timeout;
    }

    LogPrintf("Oracle: Initializing Oracle Bundle Manager\n");
    LogPrintf("Oracle: Near-quorum wait config: timeout=%lldms poll=%lldms\n",
             static_cast<long long>(near_quorum_wait_timeout.count()),
             static_cast<long long>(near_quorum_wait_poll_interval.count()));
}

OracleBundleManager::~OracleBundleManager()
{
    LogPrintf("Oracle: Shutting down Oracle Bundle Manager\n");
}

bool OracleBundleManager::AddOracleMessage(const COraclePriceMessage& message)
{
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: AddOracleMessage called for oracle_id=%d, price=%llu, timestamp=%d, enabled=%d\n",
             message.oracle_id, message.price_micro_usd, message.timestamp, enabled);

    if (!enabled) {
        LogPrintf("Oracle: Manager not enabled, rejecting message\n");
        return false;
    }

    if (!IsValidOracleMessage(message)) {
        LogPrintf("Oracle: Invalid oracle message from oracle %d\n", message.oracle_id);
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Message passed IsValidOracleMessage check\n");

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
            if (!IsFreshLiveOracleTimestamp(stale_it->second.timestamp, now)) {
                LogPrint(BCLog::DIGIDOLLAR, "Oracle: Purging stale/future message from oracle %d (age %lld seconds)\n",
                         stale_it->first, now - stale_it->second.timestamp);
                stale_it = pending_messages.erase(stale_it);
            } else {
                ++stale_it;
            }
        }
    }

    // Periodic cleanup of seen message hashes to prevent deadlock.
    // When the oracle consensus round stalls (e.g., due to rapid block production
    // or network partition), messages with the same attestation hash keep getting
    // rejected as duplicates, preventing recovery. Clearing the set periodically
    // allows fresh consensus rounds to form. The 300-second interval is shorter
    // than the 40-block (~600s) DigiDollar oracle epoch, ensuring at least one
    // cleanup per epoch. The pending_messages map (keyed by oracle_id)
    // provides the authoritative dedup; seen_message_hashes is best-effort P2P
    // optimization only.
    {
        static int64_t last_seen_cleanup = 0;
        int64_t now_cleanup = GetTime();
        if (now_cleanup - last_seen_cleanup > 300) {
            size_t old_size = seen_message_hashes.size();
            seen_message_hashes.clear();
            last_seen_cleanup = now_cleanup;
            if (old_size > 0) {
                LogPrint(BCLog::DIGIDOLLAR, "Oracle: Periodic cleanup cleared %zu seen message hashes\n", old_size);
            }
        }
    }

    // Calculate message hash for duplicate detection.
    // Use the attestation hash (oracle_id + price + timestamp) for compact
    // off-chain oracle messages. GetSignatureHash() includes block_height+nonce,
    // which are not covered by attestations; letting peers mutate those fields
    // would bypass deduplication.
    uint256 msg_hash = (!message.schnorr_sig.empty())
        ? message.GetAttestationSignatureHash()
        : message.GetSignatureHash();

    // Check if we've already seen this exact message
    if (seen_message_hashes.count(msg_hash) > 0) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Ignoring duplicate message from oracle %d\n", message.oracle_id);
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
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Ignoring older message from oracle %d\n", message.oracle_id);
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

    // Individual P2P messages may coordinate MuSig2 signing, but they do not
    // update the canonical price cache. V1 cache updates come only from a
    // validated on-chain MuSig2 bundle after block validation succeeds.
    {
        std::lock_guard<std::recursive_mutex> pending_lock(mtx_messages);
        const int64_t now = GetTime();
        COracleBundle temp;
        temp.messages.reserve(pending_messages.size());
        for (const auto& [id, m] : pending_messages) {
            if (IsActiveConsensusOracle(id) &&
                IsFreshLiveOracleTimestamp(m.timestamp, now)) {
                temp.messages.push_back(m);
            }
        }

        int fresh_count = static_cast<int>(temp.messages.size());
        if (fresh_count >= min_oracle_count) {
            const Consensus::Params& cparams = Params().GetConsensus();
            const CAmount consensus_price = CalculateConsensusPrice(temp, cparams);
            if (consensus_price <= 0) {
                LogPrint(BCLog::DIGIDOLLAR, "Oracle: %d fresh messages reached threshold but no valid consensus price was calculated\n",
                         fresh_count);
                m_messages_updated_cv.notify_all();
                return true;
            }

            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: %d/%d individual messages reached off-chain consensus (min %d required); waiting for MuSig2 on-chain bundle before updating price cache\n",
                     fresh_count, (int)pending_messages.size(), min_oracle_count);

            // T5-03: When consensus is reached from individual messages, try to generate
            // consensus attestations from local oracle nodes. Each oracle signs
            // H(oracle_id, consensus_price, consensus_timestamp) so the signature verifies
            // when stored on-chain with the consensus values.
            if (min_oracle_count > 1) {
                uint64_t att_consensus_price = static_cast<uint64_t>(consensus_price);
                int64_t att_consensus_timestamp = 0;
                // Compute consensus values (median price + median timestamp)
                {
                    std::vector<int64_t> ts;
                    for (const auto& m : temp.messages) ts.push_back(m.timestamp);
                    std::sort(ts.begin(), ts.end());
                    size_t tmid = ts.size() / 2;
                    att_consensus_timestamp = (ts.size() % 2 == 0) ?
                        (ts[tmid - 1] + ts[tmid]) / 2 : ts[tmid];

                    // Proactive broadcast: send consensus proposal when quorum is reached
                    // This ensures remote oracles get the proposal BEFORE any block template is needed
                    // Use cached_epoch, or fallback to a conservative estimate if not set
                    int32_t epoch_for_broadcast = (cached_epoch >= 0) ? cached_epoch :
                                                   static_cast<int32_t>(GetTime() / (1440 * 15));  // 1440 blocks * 15 seconds/block
                    BroadcastConsensusProposal(epoch_for_broadcast, att_consensus_price, att_consensus_timestamp);

                    // Ask local oracle nodes to sign consensus values
                    OracleManager& om = OracleManager::GetInstance();
                    for (const auto& [oid, omsg] : pending_messages) {
                        if (!IsActiveConsensusOracle(oid)) continue;
                        if (pending_attestations.count(oid)) continue; // Already have attestation
                        OracleNode* node = om.GetOracleNode(oid);
                        if (node) {
                            COraclePriceMessage att = node->CreateConsensusAttestation(
                                att_consensus_price, att_consensus_timestamp);
                            if (!att.schnorr_sig.empty() && att.VerifyAttestation()) {
                                pending_attestations[oid] = att;
                                LogPrint(BCLog::DIGIDOLLAR,
                                    "Oracle: Auto-generated consensus attestation for local oracle %d\n", oid);
                            }
                        }
                    }
                }
            }
        } else {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: %d fresh messages, need %d for consensus - cached price unchanged\n",
                     fresh_count, min_oracle_count);
        }
    }

    m_messages_updated_cv.notify_all();
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

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: GetPendingMessages called, pending_messages.size()=%zu\n", pending_messages.size());

    std::vector<COraclePriceMessage> messages;
    messages.reserve(pending_messages.size());

    for (const auto& [oracle_id, message] : pending_messages) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: GetPendingMessages - oracle_id=%d, price=%llu, timestamp=%d\n",
                 oracle_id, message.price_micro_usd, message.timestamp);
        messages.push_back(message);
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: GetPendingMessages returning %zu messages\n", messages.size());
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
    pending_attestations.clear();
    seen_message_hashes.clear();
    seen_attestation_hashes.clear();
    LogPrintf("Oracle: Manually cleared all pending messages and attestations\n");
    m_messages_updated_cv.notify_all();
}

void OracleBundleManager::InjectTestMessage(const COraclePriceMessage& message)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    pending_messages[message.oracle_id] = message;
    LogPrintf("Oracle: Injected test message for oracle %d, price=%llu\n",
             message.oracle_id, message.price_micro_usd);
    m_messages_updated_cv.notify_all();
}

bool OracleBundleManager::AddConsensusAttestation(const COraclePriceMessage& attestation)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    // Consensus attestations are off-chain inputs to the MuSig2 signing round.
    // They must be signed, but they are never accepted as on-chain oracle bundles.
    if (attestation.schnorr_sig.empty() || attestation.schnorr_sig.size() != 64) {
        LogPrintf("Oracle: Rejecting consensus attestation from oracle %d: missing/invalid signature\n",
                 attestation.oracle_id);
        return false;
    }

    if (!IsActiveConsensusOracle(attestation.oracle_id)) {
        LogPrintf("Oracle: Rejecting consensus attestation from oracle %d: outside active MuSig2 consensus keyset\n",
                 attestation.oracle_id);
        return false;
    }

    // Price range check
    if (attestation.price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD ||
        attestation.price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) {
        LogPrintf("Oracle: Rejecting consensus attestation from oracle %d: price out of range\n",
                 attestation.oracle_id);
        return false;
    }

    const int64_t now = GetTime();
    if (!IsFreshLiveOracleTimestamp(attestation.timestamp, now)) {
        LogPrintf("Oracle: Rejecting consensus attestation from oracle %d: stale/future timestamp=%lld now=%lld\n",
                 attestation.oracle_id, attestation.timestamp, now);
        return false;
    }

    // Verify compact attestation signature over the consensus values.
    if (!attestation.VerifyAttestation()) {
        // Also try with chainparams pubkey binding
        const CChainParams& params = Params();
        const OracleNodeInfo* oracle_config = params.GetOracleNode(attestation.oracle_id);
        if (oracle_config) {
            COraclePriceMessage bound = attestation;
            bound.oracle_pubkey = XOnlyPubKey(oracle_config->pubkey);
            if (!bound.VerifyAttestation()) {
                LogPrintf("Oracle: Rejecting consensus attestation from oracle %d: invalid signature\n",
                         attestation.oracle_id);
                return false;
            }
            // Store with chainparams-bound pubkey
            pending_attestations[attestation.oracle_id] = bound;
        } else {
            // In regtest, chainparams may not have this oracle — accept with original pubkey
            if (Params().GetChainType() == ChainType::REGTEST) {
                pending_attestations[attestation.oracle_id] = attestation;
            } else {
                LogPrintf("Oracle: Rejecting consensus attestation from oracle %d: unknown oracle\n",
                         attestation.oracle_id);
                return false;
            }
        }
    } else {
        pending_attestations[attestation.oracle_id] = attestation;
    }

    LogPrintf("Oracle: Added consensus attestation from oracle %d: price=%llu, timestamp=%lld\n",
             attestation.oracle_id, attestation.price_micro_usd, attestation.timestamp);
    m_messages_updated_cv.notify_all();
    return true;
}

std::vector<COraclePriceMessage> OracleBundleManager::GetPendingAttestations() const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    std::vector<COraclePriceMessage> attestations;
    attestations.reserve(pending_attestations.size());
    for (const auto& [id, msg] : pending_attestations) {
        attestations.push_back(msg);
    }
    return attestations;
}

size_t OracleBundleManager::GetPendingAttestationCount() const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    return pending_attestations.size();
}

void OracleBundleManager::ClearPendingAttestations()
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    pending_attestations.clear();
    m_messages_updated_cv.notify_all();
}

bool OracleBundleManager::ComputeConsensusValues(uint64_t& consensus_price, int64_t& consensus_timestamp) const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    const int64_t now = GetTime();
    COracleBundle temp;
    for (const auto& [id, msg] : pending_messages) {
        if (IsActiveConsensusOracle(id) &&
            IsFreshLiveOracleTimestamp(msg.timestamp, now)) {
            temp.messages.push_back(msg);
        }
    }

    if (static_cast<int>(temp.messages.size()) < min_oracle_count) {
        return false;
    }

    const Consensus::Params& cparams = Params().GetConsensus();
    CAmount price = CalculateConsensusPrice(temp, cparams);
    if (price <= 0) return false;

    consensus_price = static_cast<uint64_t>(price);

    // Compute consensus timestamp as the median of individual message timestamps
    std::vector<int64_t> timestamps;
    for (const auto& msg : temp.messages) {
        timestamps.push_back(msg.timestamp);
    }
    std::sort(timestamps.begin(), timestamps.end());
    size_t mid = timestamps.size() / 2;
    if (timestamps.size() % 2 == 0) {
        consensus_timestamp = (timestamps[mid - 1] + timestamps[mid]) / 2;
    } else {
        consensus_timestamp = timestamps[mid];
    }

    return true;
}

bool OracleBundleManager::ComputeConsensusValuesForOracles(const std::vector<uint8_t>& oracle_ids,
                                                           uint64_t& consensus_price,
                                                           int64_t& consensus_timestamp) const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    if (static_cast<int>(oracle_ids.size()) < min_oracle_count) return false;

    const int64_t now = GetTime();
    COracleBundle temp;
    temp.messages.reserve(oracle_ids.size());

    std::set<uint8_t> seen;
    for (uint8_t id : oracle_ids) {
        if (!seen.insert(id).second) return false;
        if (!IsActiveConsensusOracle(id)) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Signing context cannot compute consensus: oracle %u outside active keyset\n",
                     id);
            return false;
        }
        auto it = pending_messages.find(id);
        if (it == pending_messages.end()) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Signing context waiting for price message from oracle %u\n",
                     id);
            return false;
        }
        if (!IsFreshLiveOracleTimestamp(it->second.timestamp, now)) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Signing context price from oracle %u is stale/future timestamp=%lld now=%lld\n",
                     id, static_cast<long long>(it->second.timestamp), static_cast<long long>(now));
            return false;
        }
        temp.messages.push_back(it->second);
    }

    if (static_cast<int>(temp.messages.size()) < min_oracle_count) return false;

    const Consensus::Params& cparams = Params().GetConsensus();
    const CAmount price = CalculateConsensusPrice(temp, cparams);
    if (price <= 0) return false;
    consensus_price = static_cast<uint64_t>(price);

    std::vector<int64_t> timestamps;
    timestamps.reserve(temp.messages.size());
    for (const auto& msg : temp.messages) {
        timestamps.push_back(msg.timestamp);
    }
    std::sort(timestamps.begin(), timestamps.end());
    const size_t mid = timestamps.size() / 2;
    consensus_timestamp = (timestamps.size() % 2 == 0) ?
        (timestamps[mid - 1] + timestamps[mid]) / 2 : timestamps[mid];

    return true;
}

bool OracleBundleManager::ValidateConsensusProposal(uint64_t consensus_price, int64_t consensus_timestamp) const
{
    if (consensus_price < ORACLE_MIN_PRICE_MICRO_USD ||
        consensus_price > ORACLE_MAX_PRICE_MICRO_USD) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Rejecting consensus proposal with out-of-range price=%llu\n",
                 consensus_price);
        return false;
    }

    uint64_t local_price = 0;
    int64_t local_timestamp = 0;
    if (!ComputeConsensusValues(local_price, local_timestamp)) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Rejecting consensus proposal price=%llu timestamp=%lld: no local quorum\n",
                 consensus_price, consensus_timestamp);
        return false;
    }

    if (local_price != consensus_price || local_timestamp != consensus_timestamp) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Rejecting consensus proposal price=%llu timestamp=%lld: local price=%llu timestamp=%lld\n",
                 consensus_price, consensus_timestamp, local_price, local_timestamp);
        return false;
    }

    return true;
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
    const int32_t newest_epoch = std::max_element(
        epoch_bundles.begin(), epoch_bundles.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; })->first;
    for (auto it = epoch_bundles.begin(); it != epoch_bundles.end(); ) {
        if (it->first < newest_epoch - 1) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Pruning stale bundle for epoch %d after update\n", it->first);
            it = epoch_bundles.erase(it);
        } else {
            ++it;
        }
    }

    LogPrintf("Oracle: Updated bundle for epoch %d with %d messages\n",
             bundle.epoch, bundle.messages.size());

    // Only complete MuSig2 bundles may update the latest price cache. Off-chain
    // individual-message consensus can coordinate signing but is not canonical.
    if (HasMuSig2Quorum(bundle, Params().GetConsensus()) && bundle.epoch >= cached_epoch) {
        cached_price = static_cast<CAmount>(bundle.median_price_micro_usd);
        cached_epoch = bundle.epoch;
        last_update_time = GetTime();
    }

    return true;
}

bool OracleBundleManager::HasValidBundle(int32_t epoch) const
{
    std::lock_guard<std::mutex> lock(mtx_bundles);

    auto it = epoch_bundles.find(epoch);
    return it != epoch_bundles.end() && HasMuSig2Quorum(it->second, Params().GetConsensus());
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
    const bool block_touches_dd = BlockTouchesDigiDollar(block);
    const bool block_needs_oracle_price = BlockNeedsOraclePrice(block);

    LogPrintf("Oracle: AddOracleBundleToBlock called for height %d, enabled=%d, min_oracle_count=%d\n",
             block_height, enabled, min_oracle_count);

    if (!enabled) {
        if (block_needs_oracle_price) {
            LogPrintf("Oracle: price-dependent DD block at height %d requires a MuSig2 bundle, but oracles are disabled\n",
                      block_height);
        } else {
            LogPrintf("Oracle: Oracles disabled, no bundle available for block %d\n", block_height);
        }
        return false;
    }

    int32_t epoch = GetCurrentEpoch(block_height);
    LogPrintf("Oracle: Current epoch=%d for height %d\n", epoch, block_height);
    // Cleanup stale MuSig2 sessions at epoch boundary (keep current epoch only).
    // This prevents unbounded growth of the global session map when epoch advances.
    {
        LOCK(g_oracle_signing_sessions_mutex);
        auto it = g_oracle_signing_sessions.begin();
        while (it != g_oracle_signing_sessions.end()) {
            if (it->first < epoch) {
                LogPrintf("Oracle: Pruning stale MuSig2 session for epoch %d (current epoch=%d)\n",
                          it->first, epoch);
                it = g_oracle_signing_sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto bundle_is_fresh = [&](const COracleBundle& bundle) -> bool {
        // Mine only bundles that will still be fresh against the candidate block
        // timestamp. Functional tests and real miners can advance mock/adjusted
        // time beyond wall-clock GetTime(); using wall time here allowed stale
        // cached bundles into templates that TestBlockValidity then rejected.
        const int64_t now = std::max<int64_t>(GetTime(), block.nTime);
        if (bundle.timestamp <= 0 || bundle.timestamp > now + 60 ||
            now - bundle.timestamp > ORACLE_MAX_AGE_SECONDS) {
            LogPrintf("Oracle: Refusing stale/future MuSig2 bundle for block %d: timestamp=%lld now=%lld age=%lld max=%d\n",
                      block_height,
                      static_cast<long long>(bundle.timestamp),
                      static_cast<long long>(now),
                      static_cast<long long>(now - bundle.timestamp),
                      ORACLE_MAX_AGE_SECONDS);
            return false;
        }
        return true;
    };

    auto add_bundle_to_coinbase = [&](const COracleBundle& bundle) -> bool {
        CScript oracle_script = CreateOracleScript(bundle);
        if (oracle_script.empty()) {
            return false;
        }

        CMutableTransaction coinbase_tx(*block.vtx[0]);
        CTxOut oracle_output;
        oracle_output.nValue = 0;
        oracle_output.scriptPubKey = oracle_script;
        coinbase_tx.vout.push_back(oracle_output);
        block.vtx[0] = MakeTransactionRef(std::move(coinbase_tx));
        return true;
    };

    // V1 is MuSig2-only. The OracleSigningOrchestrator drives the
    // MuSig2 protocol asynchronously via BlockConnected callbacks.
    // AddOracleBundleToBlock only *queries* for a completed session.
    if (g_signing_orchestrator) {
        COracleBundle bundle(epoch);
        bundle.version = 3;

        uint64_t signed_price = 0;
        int64_t signed_timestamp = 0;
        bool session_ready = g_signing_orchestrator->GetCompletedSession(
            epoch, bundle.aggregate_sig, bundle.participation_bitmap,
            signed_price, signed_timestamp);

        if (session_ready) {
            LogPrintf("Oracle: MuSig2 session for epoch %d is COMPLETE, sig=%zu bytes, bitmap=%zu bytes\n",
                     epoch, bundle.aggregate_sig.size(), bundle.participation_bitmap.size());

            // Use the EXACT values signed by the MuSig2 ceremony
            bundle.median_price_micro_usd = signed_price;
            bundle.timestamp = signed_timestamp;

            std::string error;
            if (ValidateMuSig2Bundle(bundle, block_height, Params().GetConsensus(), error) &&
                bundle_is_fresh(bundle) &&
                add_bundle_to_coinbase(bundle)) {
                LogPrintf("Oracle: Added MuSig2 v0x03 bundle to block %d (epoch %d)\n",
                         block_height, epoch);
                return true;
            }
            LogPrintf("Oracle: Completed MuSig2 session for epoch %d failed bundle validation: %s\n",
                      epoch, error);
        }
    }

    COracleBundle cached_bundle = GetCurrentBundle(epoch);
    std::string error;
    if (ValidateMuSig2Bundle(cached_bundle, block_height, Params().GetConsensus(), error) &&
        bundle_is_fresh(cached_bundle) &&
        add_bundle_to_coinbase(cached_bundle)) {
        LogPrintf("Oracle: Added cached MuSig2 v0x03 bundle to block %d (epoch %d)\n",
                  block_height, epoch);
        return true;
    }

    if (block_needs_oracle_price) {
        LogPrintf("Oracle: No valid MuSig2 bundle ready for price-dependent DD block %d (epoch %d): %s\n",
                  block_height, epoch, error);
        return false;
    }

    if (block_touches_dd) {
        LogPrintf("Oracle: MuSig2 not ready/fresh for transfer-only DD block %d (epoch %d); no oracle data added\n",
                  block_height, epoch);
    } else {
        LogPrintf("Oracle: MuSig2 not ready/fresh for epoch %d; no oracle data added to block template\n", epoch);
    }
    return true;
}

CScript OracleBundleManager::CreateOracleScript(const COracleBundle& bundle) const
{
    // MuSig2 v0x03: aggregate signature + participation bitmap
    // Format: OP_RETURN OP_ORACLE <version=0x03> <v03_data>
    // v03_data: bitmap_len(1) + bitmap(var) + price(8) + timestamp(8) + aggregate_sig(64)
    if (bundle.version == 3) {
        if (bundle.aggregate_sig.size() != 64) {
            LogPrintf("Oracle: CreateOracleScript v0x03 error: aggregate_sig size %zu != 64\n",
                     bundle.aggregate_sig.size());
            return CScript();
        }

        if (bundle.participation_bitmap.empty()) {
            LogPrintf("Oracle: CreateOracleScript v0x03 error: empty participation_bitmap\n");
            return CScript();
        }

        const Consensus::Params& cparams = Params().GetConsensus();
        const uint16_t total_oracles = static_cast<uint16_t>(std::max(1, cparams.nOracleTotalOracles));
        const std::vector<uint8_t> participants = MuSig2OracleAggregator::DecodeBitmap(
            bundle.participation_bitmap, total_oracles);
        if (participants.empty()) {
            LogPrintf("Oracle: CreateOracleScript v0x03 error: malformed participation_bitmap size=%zu for total_oracles=%u\n",
                     bundle.participation_bitmap.size(), total_oracles);
            return CScript();
        }
        const int required = std::max(1, cparams.nOracleConsensusRequired);
        if (static_cast<int>(participants.size()) < required) {
            LogPrintf("Oracle: CreateOracleScript v0x03 error: participation count %zu below threshold %d\n",
                     participants.size(), required);
            return CScript();
        }

        std::vector<unsigned char> v03_data = bundle.SerializeV03Data();
        if (v03_data.empty()) {
            LogPrintf("Oracle: CreateOracleScript v0x03 error: SerializeV03Data failed\n");
            return CScript();
        }

        CScript script;
        script << OP_RETURN << OP_ORACLE;
        script << std::vector<unsigned char>{0x03};
        script << v03_data;

        LogPrintf("Oracle: Created MuSig2 v0x03 script with bitmap_bytes=%zu, payload=%zu, price=%llu\n",
                 bundle.participation_bitmap.size(),
                 v03_data.size(),
                 static_cast<unsigned long long>(bundle.median_price_micro_usd));
        return script;
    }

    return CScript();
}

bool OracleBundleManager::ExtractOracleBundle(const CTransaction& coinbase_tx, COracleBundle& bundle) const
{
    int oracle_output_count = 0;
    for (const auto& output : coinbase_tx.vout) {
        if (output.scriptPubKey.size() >= 2 &&
            output.scriptPubKey[0] == OP_RETURN &&
            output.scriptPubKey[1] == OP_ORACLE) {
            ++oracle_output_count;
        }
    }
    if (oracle_output_count > 1) {
        LogPrintf("Oracle: Rejecting transaction with %d oracle outputs (expected at most 1)\n",
                  oracle_output_count);
        return false;
    }

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
                                return false;
                            }
                        } else if (*script_it == 0x4c) { // OP_PUSHDATA1: next byte is length
                            ++script_it;
                            if (script_it >= output.scriptPubKey.end()) return false;
                            unsigned int chunk_size = *script_it;
                            ++script_it;
                            if (script_it + chunk_size <= output.scriptPubKey.end()) {
                                data.insert(data.end(), script_it, script_it + chunk_size);
                                script_it += chunk_size;
                            } else {
                                return false;
                            }
                        } else if (*script_it == 0x4d) { // OP_PUSHDATA2: next 2 bytes are length (LE)
                            ++script_it;
                            if (script_it + 2 > output.scriptPubKey.end()) return false;
                            unsigned int chunk_size = *script_it | (*(script_it + 1) << 8);
                            script_it += 2;
                            if (script_it + chunk_size <= output.scriptPubKey.end()) {
                                data.insert(data.end(), script_it, script_it + chunk_size);
                                script_it += chunk_size;
                            } else {
                                return false;
                            }
                        } else {
                            return false;
                        }
                    }

                    if (data.empty()) {
                        return false;
                    }

                    // Check version byte
                    if (data[0] == 0x03) {
                        // v0x03 MuSig2 format: aggregate sig + participation bitmap (RC30)
                        // Data layout (after version byte):
                        //   bitmap_len(1) + bitmap(variable) + epoch(4) + price(8) + timestamp(8) + aggregate_sig(64)
                        std::vector<unsigned char> v03_data(data.begin() + 1, data.end());

                        if (!COracleBundle::DeserializeV03Data(v03_data, bundle)) {
                            LogPrintf("Oracle: Failed to deserialize v0x03 MuSig2 bundle data (%zu bytes)\n",
                                     v03_data.size());
                            return false;
                        }

                        bundle.version = 3;
                        // RC30: bundle.epoch is now parsed from the on-chain payload by
                        // DeserializeV03Data — do NOT zero it here. The validator binds
                        // the epoch to the current block height in ValidateMuSig2Bundle.

                        // Wave 3: decode participation bitmap into synthetic oracle messages.
                        // v0x03 stores one aggregate signature, so per-oracle schnorr_sig is empty.
                        bundle.messages.clear();
                        const Consensus::Params& params = Params().GetConsensus();
                        const uint16_t total_oracles = static_cast<uint16_t>(std::max(1, params.nOracleTotalOracles));
                        std::vector<uint8_t> oracle_ids = MuSig2OracleAggregator::DecodeBitmap(
                            bundle.participation_bitmap, total_oracles);
                        if (oracle_ids.empty()) {
                            LogPrintf("Oracle: Rejecting v0x03 MuSig2 bundle with malformed participation bitmap (bytes=%zu, total_oracles=%u)\n",
                                      bundle.participation_bitmap.size(), total_oracles);
                            return false;
                        }
                        const CChainParams& chainparams = Params();
                        for (uint8_t oracle_id : oracle_ids) {
                            COraclePriceMessage msg;
                            msg.oracle_id = oracle_id;
                            msg.price_micro_usd = bundle.median_price_micro_usd;
                            msg.timestamp = bundle.timestamp;
                            msg.block_height = 0;
                            msg.nonce = 0;
                            const OracleNodeInfo* oracle_info = chainparams.GetOracleNode(msg.oracle_id);
                            if (oracle_info) {
                                msg.oracle_pubkey = XOnlyPubKey(oracle_info->pubkey);
                            }
                            bundle.messages.push_back(std::move(msg));
                        }

                        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Extracted v0x03 MuSig2 bundle: "
                                 "bitmap=%zu bytes, participants=%zu, price=%llu micro-USD, sig=%zu bytes\n",
                                 bundle.participation_bitmap.size(),
                                 bundle.messages.size(),
                                 static_cast<unsigned long long>(bundle.median_price_micro_usd),
                                 bundle.aggregate_sig.size());
                        return true;
                    }
                    if (data[0] == 0x01 || data[0] == 0x02) {
                        LogPrintf("Oracle: Rejecting legacy oracle bundle version v0x%02x; DigiDollar V1 requires MuSig2 v0x03\n",
                                  data[0]);
                        return false;
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

bool OracleBundleManager::ValidateV03BundleFormat(const CScript& script, uint8_t& version)
{
    version = 0;

    // Minimum script: OP_RETURN(1) + OP_ORACLE(1) + push(1) + version(1) = 4 bytes
    if (script.size() < 4) return false;

    // Check OP_RETURN + OP_ORACLE marker
    if (script[0] != OP_RETURN || script[1] != OP_ORACLE) return false;

    // Extract data chunks (same logic as ExtractOracleBundle)
    std::vector<unsigned char> data;
    auto it = script.begin() + 2;
    while (it < script.end()) {
        if (*it <= 75) {
            unsigned char chunk_size = *it;
            ++it;
            if (it + chunk_size <= script.end()) {
                data.insert(data.end(), it, it + chunk_size);
                it += chunk_size;
            } else {
                return false;
            }
        } else if (*it == 0x4c) { // OP_PUSHDATA1
            ++it;
            if (it >= script.end()) return false;
            unsigned int chunk_size = *it;
            ++it;
            if (it + chunk_size <= script.end()) {
                data.insert(data.end(), it, it + chunk_size);
                it += chunk_size;
            } else {
                return false;
            }
        } else if (*it == 0x4d) { // OP_PUSHDATA2
            ++it;
            if (it + 2 > script.end()) return false;
            unsigned int chunk_size = *it | (*(it + 1) << 8);
            it += 2;
            if (it + chunk_size <= script.end()) {
                data.insert(data.end(), it, it + chunk_size);
                it += chunk_size;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }

    if (data.empty()) return false;

    version = data[0];

    if (version != 0x03) return false;

    // v0x03: version(1) + bitmap_len(1) + bitmap(>=1) + epoch(4) + price(8) + timestamp(8) + sig(64)
    if (data.size() < 87) return false;
    uint8_t bitmap_len = data[1];
    if (bitmap_len == 0) return false;
    size_t expected = 1 + 1 + bitmap_len + 4 + 8 + 8 + 64;
    return data.size() == expected;
}

CAmount OracleBundleManager::GetConsensusPrice(int32_t epoch) const
{
    COracleBundle bundle = GetCurrentBundle(epoch);
    if (!HasMuSig2Quorum(bundle, Params().GetConsensus())) return 0;
    return static_cast<CAmount>(bundle.median_price_micro_usd);
}

CAmount OracleBundleManager::GetLatestPrice() const
{
    std::lock_guard<std::mutex> lock(mtx_bundles);

    // SECURITY: Reject stale cached prices.
    // If the last oracle update was more than ORACLE_MAX_AGE_SECONDS ago,
    // the price is stale and must not be used for collateral calculations.
    // Without this check, an attacker can DDoS oracles and mint DD using
    // the last known (higher) price while the real DGB price has crashed.
    if (cached_price > 0 && last_update_time > 0) {
        int64_t age = GetTime() - last_update_time;
        if (age > ORACLE_MAX_AGE_SECONDS) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Rejecting stale cached price %lld micro-USD (age: %lld seconds, max: %d)\n",
                     cached_price, age, ORACLE_MAX_AGE_SECONDS);
            return 0;
        }
    }

    return cached_price;
}

bool OracleBundleManager::UpdateCachedPrice(int32_t epoch)
{
    COracleBundle bundle = GetCurrentBundle(epoch);
    if (HasMuSig2Quorum(bundle, Params().GetConsensus())) {
        CAmount price = static_cast<CAmount>(bundle.median_price_micro_usd);
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
    return OracleDataValidator::ValidateOracleBundle(bundle, block_height, params);
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

void OracleBundleManager::RegisterSeenHash(const uint256& hash)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    seen_message_hashes.insert(hash);

    // RH-03 Fix: Cap seen_message_hashes from P2P RegisterSeenHash path too.
    // Without this, an attacker flooding unique MuSig2 messages can grow the
    // set unboundedly since RegisterSeenHash bypasses the MAX_SEEN_HASHES
    // cap in AddOracleMessage.
    static constexpr size_t MAX_SEEN_HASHES = 2048;
    if (seen_message_hashes.size() > MAX_SEEN_HASHES) {
        auto erase_end = seen_message_hashes.begin();
        std::advance(erase_end, seen_message_hashes.size() - MAX_SEEN_HASHES);
        seen_message_hashes.erase(seen_message_hashes.begin(), erase_end);
    }
}

bool OracleBundleManager::AddVersionHeartbeat(const OracleVersionHeartbeatMsg& heartbeat)
{
    if (!heartbeat.IsValid()) return false;
    if (!IsAuthorizedMuSig2OracleIdForRelay(Params(), heartbeat.oracle_id)) return false;

    const OracleNodeInfo* oracle_config = Params().GetOracleNode(heartbeat.oracle_id);
    if (!oracle_config || !heartbeat.VerifySignature(XOnlyPubKey(oracle_config->pubkey))) {
        return false;
    }

    const int64_t now = GetTime();
    if (heartbeat.timestamp > now + 10 * 60) return false;
    if (heartbeat.timestamp < now - 7 * 24 * 60 * 60) return false;

    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    auto it = version_heartbeats.find(heartbeat.oracle_id);
    if (it != version_heartbeats.end() && it->second.timestamp > heartbeat.timestamp) {
        return false;
    }
    version_heartbeats[heartbeat.oracle_id] = heartbeat;
    return true;
}

bool OracleBundleManager::GetVersionHeartbeat(uint32_t oracle_id,
                                              OracleVersionHeartbeatMsg& heartbeat_out) const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    const auto it = version_heartbeats.find(oracle_id);
    if (it == version_heartbeats.end()) return false;
    heartbeat_out = it->second;
    return true;
}

std::vector<OracleVersionHeartbeatMsg> OracleBundleManager::GetVersionHeartbeats() const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    std::vector<OracleVersionHeartbeatMsg> heartbeats;
    heartbeats.reserve(version_heartbeats.size());
    for (const auto& [oracle_id, heartbeat] : version_heartbeats) {
        heartbeats.push_back(heartbeat);
    }
    return heartbeats;
}

bool OracleBundleManager::BroadcastVersionHeartbeat(const OracleVersionHeartbeatMsg& heartbeat)
{
    if (!AddVersionHeartbeat(heartbeat)) return false;
    if (!m_connman) return true;

    m_connman->ForEachNode([this, &heartbeat](CNode* node) {
        m_connman->PushMessage(node,
            CNetMsgMaker(node->GetCommonVersion()).Make(NetMsgType::ORACLEHEARTBEAT, heartbeat));
    });
    return true;
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

bool OracleBundleManager::BroadcastConsensusProposal(int32_t epoch, uint64_t consensus_price, int64_t consensus_timestamp)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mtx_messages);

        // Rate limit: only broadcast once per 30 seconds for recovery
        static int64_t last_broadcast_time = 0;
        int64_t now = GetTime();
        if (now - last_broadcast_time < 30) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Consensus proposal rate limited, last broadcast %d seconds ago\n", 
                     (int)(now - last_broadcast_time));
            return false;
        }
        last_broadcast_time = now;

        // Still track epochs to avoid redundant broadcasts within same epoch
        if (broadcast_proposal_epochs.count(epoch)) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Consensus proposal already broadcast for epoch %d\n", epoch);
            return false;
        }
        broadcast_proposal_epochs.insert(epoch);

        // Cleanup old epochs (keep last 3)
        while (broadcast_proposal_epochs.size() > 3) {
            broadcast_proposal_epochs.erase(broadcast_proposal_epochs.begin());
        }
    }

    if (!m_connman) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Cannot broadcast consensus proposal - no P2P connection\n");
        return false;
    }

    OracleConsensusMsg proposal;
    proposal.epoch = epoch;
    proposal.consensus_price = consensus_price;
    proposal.consensus_timestamp = consensus_timestamp;

    m_connman->ForEachNode([this, &proposal](CNode* node) {
        m_connman->PushMessage(node,
            CNetMsgMaker(node->GetCommonVersion()).Make(
                NetMsgType::ORACLECONSENSUS, proposal));
    });

    LogPrintf("Oracle: Broadcast consensus proposal for epoch %d: price=%llu, timestamp=%lld\n",
             epoch, consensus_price, consensus_timestamp);
    return true;
}

bool OracleBundleManager::HasBroadcastConsensusProposal(int32_t epoch) const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    return broadcast_proposal_epochs.count(epoch) > 0;
}

bool OracleBundleManager::StartMuSig2Session(int32_t block_height)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    if (!consensus.IsMuSig2OracleActive(block_height)) return false;

    OracleManager& om = OracleManager::GetInstance();
    const std::vector<uint32_t> active_ids = om.GetActiveOracleIds();
    if (active_ids.empty()) return false;

    const uint8_t local_oracle_id = static_cast<uint8_t>(active_ids.front());
    OracleNode* local_node = om.GetOracleNode(local_oracle_id);
    if (!local_node) return false;

    const int32_t epoch = GetCurrentEpoch(block_height);

    MuSig2SigningSession* session_ptr = nullptr;
    {
        LOCK(g_oracle_signing_sessions_mutex);
        if (g_oracle_signing_sessions.count(epoch)) return false;

        auto [it, ok] = g_oracle_signing_sessions.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(epoch),
            std::forward_as_tuple(epoch, static_cast<uint8_t>(consensus.nOracleConsensusRequired)));
        if (!ok) return false;
        session_ptr = &it->second;
    }

    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache keyagg_cache;
    // Convert uint32_t oracle IDs to uint8_t for aggregator
    std::vector<uint8_t> oracle_ids_u8;
    oracle_ids_u8.reserve(active_ids.size());
    for (uint32_t id : active_ids) oracle_ids_u8.push_back(static_cast<uint8_t>(id));
    if (!aggregator.ComputeAggregatePubkey(oracle_ids_u8, agg_pk, keyagg_cache)) return false;

    const CKey signing_key = local_node->GetOraclePrivateKey();
    if (!signing_key.IsValid()) return false;

    const CPubKey pubkey = local_node->GetPublicKey();
    if (!pubkey.IsFullyValid()) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    secp256k1_pubkey secp_pubkey;
    if (!secp256k1_ec_pubkey_parse(ctx, &secp_pubkey, pubkey.data(), pubkey.size())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_musig_pubnonce pubnonce;
    if (!session_ptr->GenerateNonce(local_oracle_id, signing_key, secp_pubkey, keyagg_cache, pubnonce)) {
        secp256k1_context_destroy(ctx);
        return false;
    }
    session_ptr->AddPubnonce(local_oracle_id, pubnonce);

    OracleMusigNonceMsg msg;
    msg.epoch = epoch;
    msg.oracle_id = local_oracle_id;
    msg.pubnonce.resize(66);
    if (!secp256k1_musig_pubnonce_serialize(ctx, msg.pubnonce.data(), &pubnonce)) {
        secp256k1_context_destroy(ctx);
        return false;
    }
    secp256k1_context_destroy(ctx);

    if (m_connman) {
        m_connman->ForEachNode([this, &msg](CNode* node) {
            m_connman->PushMessage(node,
                CNetMsgMaker(node->GetCommonVersion()).Make(NetMsgType::ORACLEMUSIGNONCE, msg));
        });
    }

    return true;
}

bool OracleBundleManager::CompleteMuSig2Session(int32_t block_height)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    if (!consensus.IsMuSig2OracleActive(block_height)) return false;

    OracleManager& om = OracleManager::GetInstance();
    const std::vector<uint32_t> active_ids = om.GetActiveOracleIds();
    if (active_ids.empty()) return false;

    const uint8_t local_oracle_id = static_cast<uint8_t>(active_ids.front());
    OracleNode* local_node = om.GetOracleNode(local_oracle_id);
    if (!local_node) return false;

    const int32_t epoch = GetCurrentEpoch(block_height);
    LOCK(g_oracle_signing_sessions_mutex);
    auto it = g_oracle_signing_sessions.find(epoch);
    if (it == g_oracle_signing_sessions.end()) return false;

    MuSig2SigningSession& session = it->second;
    if (!session.HasEnoughNonces()) return false;

    unsigned char msg32[32] = {0};
    std::memcpy(msg32, &epoch, std::min(sizeof(epoch), sizeof(msg32)));

    if (session.GetState() == MuSig2SessionState::NONCES_COMPLETE && !session.AggregateNonces(msg32)) {
        return false;
    }
    if (session.GetState() != MuSig2SessionState::SIGNING) return false;

    const CKey signing_key = local_node->GetOraclePrivateKey();
    if (!signing_key.IsValid()) return false;

    secp256k1_musig_partial_sig partial_sig;
    if (!session.CreatePartialSignature(local_oracle_id, signing_key, partial_sig)) return false;
    session.AddPartialSignature(local_oracle_id, partial_sig);

    OracleMusigPartialSigMsg msg;
    msg.epoch = epoch;
    msg.context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
    msg.session_context_id = session.GetSessionContextId();
    msg.oracle_id = local_oracle_id;
    msg.partial_sig.resize(32);
    if (msg.session_context_id.IsNull()) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;
    if (!secp256k1_musig_partial_sig_serialize(ctx, msg.partial_sig.data(), &partial_sig)) {
        secp256k1_context_destroy(ctx);
        return false;
    }
    secp256k1_context_destroy(ctx);

    if (m_connman) {
        m_connman->ForEachNode([this, &msg](CNode* node) {
            m_connman->PushMessage(node,
                CNetMsgMaker(node->GetCommonVersion()).Make(NetMsgType::ORACLEMUSIGPARTIALSIG, msg));
        });
    }

    return true;
}

// ============================================================================
// MuSig2 P2P ingestion — feed remote nonces/partial-sigs into
// g_oracle_signing_sessions so the local node can aggregate them.
// Called by net_processing on ORACLEMUSIGNONCE / ORACLEMUSIGPARTIALSIG.
// ============================================================================

bool OracleBundleManager::ProcessRemoteMusigNonce(const OracleMusigNonceMsg& msg)
{
    if (!msg.IsValid()) return false;

    // RH-24: Verify authentication signature before processing
    {
        const OracleNodeInfo* oracle_config = Params().GetOracleNode(msg.oracle_id);
        if (!oracle_config) return false;
        XOnlyPubKey oracle_pubkey(oracle_config->pubkey);
        if (!msg.VerifySignature(oracle_pubkey)) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: MuSig2 nonce signature verification failed for oracle %u\n",
                     msg.oracle_id);
            return false;
        }
    }

    LOCK(g_oracle_signing_sessions_mutex);
    auto it = g_oracle_signing_sessions.find(msg.epoch);
    if (it == g_oracle_signing_sessions.end()) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Received MuSig2 nonce for unknown epoch %d from oracle %u\n",
                 msg.epoch, msg.oracle_id);
        return false;
    }

    MuSig2SigningSession& session = it->second;

    // Deserialize the 66-byte pubnonce
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    secp256k1_musig_pubnonce pubnonce;
    bool parsed = secp256k1_musig_pubnonce_parse(ctx, &pubnonce, msg.pubnonce.data());
    secp256k1_context_destroy(ctx);

    if (!parsed) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Failed to parse MuSig2 pubnonce from oracle %u epoch %d\n",
                 msg.oracle_id, msg.epoch);
        return false;
    }

    if (!session.AddPubnonce(msg.oracle_id, pubnonce)) {
        // Duplicate or wrong state — not an error, just skip
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Ingested remote MuSig2 nonce: epoch=%d, oracle_id=%u, nonces_now=%zu\n",
             msg.epoch, msg.oracle_id, session.GetNonceCount());
    return true;
}

bool OracleBundleManager::ProcessRemoteMusigPartialSig(const OracleMusigPartialSigMsg& msg)
{
    if (!msg.IsValid()) return false;
    if (msg.context_version != ORACLE_MUSIG2_SESSION_CONTEXT_VERSION ||
        msg.session_context_id.IsNull()) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Rejecting MuSig2 partial sig from oracle %u epoch %d without valid session context\n",
                 msg.oracle_id, msg.epoch);
        return false;
    }

    // RH-24: Verify authentication signature before processing
    {
        const OracleNodeInfo* oracle_config = Params().GetOracleNode(msg.oracle_id);
        if (!oracle_config) return false;
        XOnlyPubKey oracle_pubkey(oracle_config->pubkey);
        if (!msg.VerifySignature(oracle_pubkey)) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: MuSig2 partial sig signature verification failed for oracle %u\n",
                     msg.oracle_id);
            return false;
        }
    }

    LOCK(g_oracle_signing_sessions_mutex);
    auto it = g_oracle_signing_sessions.find(msg.epoch);
    if (it == g_oracle_signing_sessions.end()) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Received MuSig2 partial sig for unknown epoch %d from oracle %u\n",
                 msg.epoch, msg.oracle_id);
        return false;
    }

    MuSig2SigningSession& session = it->second;
    const uint256 session_context = session.GetSessionContextId();
    if (session_context.IsNull() || session_context != msg.session_context_id) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Rejecting MuSig2 partial sig from oracle %u epoch %d due context mismatch msg=%s session=%s\n",
                 msg.oracle_id, msg.epoch, msg.session_context_id.ToString(),
                 session_context.ToString());
        return false;
    }

    // Deserialize the 32-byte partial sig
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    secp256k1_musig_partial_sig psig;
    bool parsed = secp256k1_musig_partial_sig_parse(ctx, &psig, msg.partial_sig.data());
    if (!parsed) {
        secp256k1_context_destroy(ctx);
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Failed to parse MuSig2 partial sig from oracle %u epoch %d\n",
                 msg.oracle_id, msg.epoch);
        return false;
    }

    // W3-H-01 fix: verify the partial sig under the signer's chainparams
    // pubkey + session's keyagg_cache before admitting it to the session.
    // Previously used unverified AddPartialSignature which allowed any
    // in-range scalar submitted by a single compromised oracle to poison
    // aggregation and force schnorrsig_verify failure on the produced
    // 64-byte aggregate — per-epoch DoS on oracle attestation.
    const OracleNodeInfo* oracle_cfg = Params().GetOracleNode(msg.oracle_id);
    if (!oracle_cfg) {
        secp256k1_context_destroy(ctx);
        return false;
    }
    secp256k1_pubkey signer_pk;
    if (!secp256k1_ec_pubkey_parse(ctx, &signer_pk,
                                   oracle_cfg->pubkey.data(),
                                   oracle_cfg->pubkey.size())) {
        secp256k1_context_destroy(ctx);
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Failed to parse chainparams pubkey for oracle %u\n",
                 msg.oracle_id);
        return false;
    }
    secp256k1_context_destroy(ctx);

    if (!session.AddPartialSignatureVerified(msg.oracle_id, psig, signer_pk)) {
        // Verification failed (garbage scalar, wrong session, or duplicate)
        // — not an error, just skip. Prevents a single malicious oracle from
        // bricking the epoch's aggregate signature.
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Ingested remote MuSig2 partial sig: epoch=%d, oracle_id=%u, sigs_now=%zu\n",
             msg.epoch, msg.oracle_id, session.GetPartialSigCount());

    // Eagerly try to aggregate when we have enough partial sigs.
    // This avoids waiting for the next AddOracleBundleToBlock() call.
    if (session.HasEnoughPartialSigs() &&
        session.GetState() == MuSig2SessionState::SIGNING) {
        std::vector<unsigned char> sig64;
        if (session.AggregateSignature(sig64)) {
            LogPrintf("Oracle: MuSig2 signature eagerly aggregated for epoch %d (%zu bytes)\n",
                     msg.epoch, sig64.size());
        }
    }

    return true;
}

bool OracleBundleManager::HasSeenAttestation(const uint256& hash) const
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);
    return seen_attestation_hashes.count(hash) > 0;
}

bool OracleBundleManager::RegisterSeenAttestation(const uint256& hash)
{
    std::lock_guard<std::recursive_mutex> lock(mtx_messages);

    if (seen_attestation_hashes.count(hash)) {
        return false; // Already seen — replay
    }

    seen_attestation_hashes.insert(hash);

    // Limit set size to prevent memory exhaustion
    if (seen_attestation_hashes.size() > 10000) {
        // Remove oldest entries (set is ordered, so begin() is smallest hash)
        auto it = seen_attestation_hashes.begin();
        for (size_t i = 0; i < 1000 && it != seen_attestation_hashes.end(); ++i) {
            it = seen_attestation_hashes.erase(it);
        }
    }

    return true; // New attestation
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
        stats.near_quorum_wait_attempts = near_quorum_wait_attempts;
        stats.near_quorum_wait_successes = near_quorum_wait_successes;
        stats.near_quorum_wait_timeouts = near_quorum_wait_timeouts;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_bundles);
        stats.active_bundles = epoch_bundles.size();
        const bool fresh_price = cached_price > 0 && last_update_time > 0 &&
            (GetTime() - last_update_time) <= ORACLE_MAX_AGE_SECONDS;
        stats.latest_price = fresh_price ? cached_price : 0;
        stats.latest_epoch = cached_epoch;
        stats.last_update = last_update_time;
        stats.has_consensus = stats.latest_price > 0;
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
    manager.min_oracle_count = consensus.nOracleConsensusRequired;
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
    LogPrintf("Oracle: configured with %zu MuSig2 oracle keys, %d-of-%d quorum\n",
             consensus.vOraclePublicKeys.size(),
             consensus.nOracleConsensusRequired, consensus.nOracleTotalOracles);

    // Check epoch length is reasonable
    if (consensus.nOracleEpochLength < 40 || consensus.nOracleEpochLength > 10080) {
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

bool OracleBundleManager::LoadPricesFromChain(ChainstateManager& chainman)
{
    OracleBundleManager& manager = GetInstance();
    const Consensus::Params& consensus = Params().GetConsensus();

    LOCK(cs_main);

    // Get the active chain tip
    CBlockIndex* pindex = chainman.ActiveChain().Tip();
    if (!pindex) {
        LogPrintf("Oracle: No active chain tip, skipping price loading\n");
        return true;
    }

    int tip_height = pindex->nHeight;

    // Only scan if DigiDollar is active via BIP9 deployment
    if (!DigiDollar::IsDigiDollarEnabled(pindex, chainman)) {
        LogPrintf("Oracle: DigiDollar not yet active (BIP9) at height %d, skipping price loading\n",
                 tip_height);
        return true;
    }

    // DigiDollar activation floor: blocks at/above it are guaranteed retained on a
    // pruned node (the "digidollar" prune lock). An unreadable block THERE means the
    // data is damaged and we must fail closed. Below it — or when the floor is 0
    // (default regtest ALWAYS_ACTIVE, where no retention is guaranteed and no lock is
    // registered) — a missing block is a legitimate prune/assumeutxo state, not damage.
    const int dd_floor = DigiDollar::EarliestActivationFloor(consensus);

    // Scan recent blocks for the live oracle cache and enough history to
    // deterministically rebuild volatility state after restart/reindex.
    static constexpr int ORACLE_VALIDITY_BLOCKS = 20;
    static constexpr int VOLATILITY_HISTORY_BLOCKS = 30 * 24 * 60 * 4;
    int scan_depth = std::min(std::max(ORACLE_VALIDITY_BLOCKS, VOLATILITY_HISTORY_BLOCKS), tip_height);
    const int price_cache_start_height = std::max(0, tip_height - ORACLE_VALIDITY_BLOCKS + 1);
    int prices_found = 0;
    std::vector<DigiDollar::Volatility::PricePoint> volatility_prices;

    LogPrintf("Oracle: Scanning last %d blocks for oracle prices (height %d to %d)...\n",
             scan_depth, tip_height - scan_depth + 1, tip_height);

    const int start_height = std::max(0, tip_height - scan_depth + 1);
    for (int height = start_height; height <= tip_height; ++height) {
        CBlockIndex* block_index = chainman.ActiveChain()[height];
        if (!block_index) continue;

        if (!ShouldLoadStartupOraclePriceForBlock(height, block_index, consensus)) {
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: Skipping pre-activation price cache load at height %d\n",
                     height);
            continue;
        }

        CBlock block;
        if (!chainman.m_blockman.ReadBlockFromDisk(block, *block_index)) {
            if (dd_floor > 0 && height >= dd_floor) {
                // A block inside the guaranteed-retained DigiDollar window is
                // unreadable (e.g. a truncated/partially-restored blk file that the
                // index-flag startup guard cannot see). Fail CLOSED: the volatility
                // freeze state reconstructed here is enforced as a consensus rule
                // post-activation, so refusing to start beats rebuilding it from
                // partial price history and diverging from the network.
                LogPrintf("ERROR: Oracle: failed to read block at height %d during startup "
                          "price reconstruction (>= DigiDollar floor %d). Block data is "
                          "incomplete; restart with -reindex.\n", height, dd_floor);
                return false;
            }
            // Below the floor (or floor 0, e.g. default regtest / assumeutxo gaps)
            // a missing block is a legitimate prune state, not damage.
            LogPrint(BCLog::DIGIDOLLAR,
                     "Oracle: skipping unreadable pre-floor block at height %d during "
                     "startup price reconstruction\n", height);
            continue;
        }

        // Extract oracle bundle from coinbase
        if (block.vtx.empty()) continue;
        const CTransaction& coinbase = *block.vtx[0];

        COracleBundle bundle;
        if (manager.ExtractOracleBundle(coinbase, bundle)) {
            if (bundle.median_price_micro_usd > 0) {
                BlockValidationState state;
                if (!OracleDataValidator::ValidateBlockOracleData(block, block_index->pprev, consensus, state)) {
                    LogPrintf("Oracle: Skipping invalid startup oracle bundle at height %d: %s\n",
                             height, state.ToString());
                    continue;
                }
                if (height >= price_cache_start_height) {
                    manager.UpdatePriceCache(height, bundle.median_price_micro_usd, bundle.timestamp);
                    prices_found++;
                }
                if (BlockHasDigiDollarMint(block)) {
                    const int64_t block_time = block.GetBlockTime();
                    if (volatility_prices.empty() ||
                        block_time - volatility_prices.back().timestamp >= 3600) {
                        volatility_prices.emplace_back(static_cast<CAmount>(bundle.median_price_micro_usd),
                                                       block_time,
                                                       static_cast<uint32_t>(height));
                    }
                }
                LogPrint(BCLog::DIGIDOLLAR, "Oracle: Found price %llu micro-USD at height %d\n",
                         bundle.median_price_micro_usd, height);
            }
        }
    }

    DigiDollar::Volatility::VolatilityMonitor::ReconstructFromBlockData(
        volatility_prices, static_cast<uint32_t>(tip_height));

    if (prices_found > 0) {
        LogPrintf("Oracle: Loaded %d oracle prices from blockchain, latest price: %llu micro-USD\n",
                 prices_found, manager.GetLatestPrice());
    } else {
        LogPrintf("Oracle: No oracle prices found in recent blocks\n");
    }
    return true;
}

bool OracleBundleManager::ShouldLoadStartupOraclePriceForBlock(int height, const CBlockIndex* block_index, const Consensus::Params& params)
{
    if (block_index && block_index->pprev) {
        return DigiDollar::IsDigiDollarEnabled(block_index->pprev, params);
    }

    const auto& deployment = params.vDeployments[Consensus::DEPLOYMENT_DIGIDOLLAR];
    const int activation_height = deployment.nStartTime == Consensus::BIP9Deployment::ALWAYS_ACTIVE
        ? deployment.min_activation_height
        : std::min(params.nDDActivationHeight, deployment.min_activation_height);

    if (activation_height <= 0) {
        return true;
    }

    return height >= activation_height;
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
        pending_attestations.clear();
        seen_message_hashes.clear();
        version_heartbeats.clear();
        broadcast_proposal_epochs.clear();
        seen_attestation_hashes.clear();
        near_quorum_wait_attempts = 0;
        near_quorum_wait_successes = 0;
        near_quorum_wait_timeouts = 0;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_price_cache);
        height_to_price.clear();
        height_to_price_time.clear();
    }

    m_messages_updated_cv.notify_all();
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

    if (consensus.nOracleConsensusRequired > consensus.nOracleTotalOracles) {
        LogPrintf("Oracle: ERROR - MuSig2 quorum (%d) exceeds total oracles (%d)\n",
                 consensus.nOracleConsensusRequired, consensus.nOracleTotalOracles);
        return false;
    }

    LogPrintf("Oracle: Configuration valid - %zu keys, %d-of-%d consensus\n",
             consensus.vOraclePublicKeys.size(),
             consensus.nOracleConsensusRequired, consensus.nOracleTotalOracles);

    // Check epoch length is reasonable
    if (consensus.nOracleEpochLength < 40 || consensus.nOracleEpochLength > 10080) {
        LogPrintf("Oracle: WARNING - Unusual epoch length: %d blocks\n", consensus.nOracleEpochLength);
    }

    // Check min_oracle_count matches consensus
    if (min_oracle_count != consensus.nOracleConsensusRequired) {
        LogPrintf("Oracle: ERROR - min_oracle_count (%d) does not match consensus.nOracleConsensusRequired (%d)\n",
                 min_oracle_count, consensus.nOracleConsensusRequired);
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
    (void)epoch;
    LogPrint(BCLog::DIGIDOLLAR,
             "Oracle: legacy message-bundle creation disabled; V1 uses completed MuSig2 bundles only\n");
    return false;
}

void OracleBundleManager::UpdateEpochBundle(int32_t epoch)
{
    TryCreateBundle(epoch);
}

bool OracleBundleManager::IsValidOracleMessage(const COraclePriceMessage& message) const
{
    // SECURITY (DGB-SEC-004): Reject oracle_id > 255. The on-chain script
    // format stores oracle_id as a single byte. Accepting larger IDs would
    // silently truncate, causing ID collisions and signature mismatches.
    if (message.oracle_id > 255) {
        LogPrintf("Oracle: Rejecting message with oracle_id %d > 255 (exceeds 1-byte on-chain format)\n",
                 message.oracle_id);
        return false;
    }

    // MuSig2 v0x03 validation only recognizes slots in the consensus pubkey
    // roster. Reserve metadata entries must not satisfy pending-message quorum.
    if (!IsActiveConsensusOracle(message.oracle_id)) {
        LogPrintf("Oracle: Rejecting message from oracle %d: outside active MuSig2 consensus keyset\n",
                 message.oracle_id);
        return false;
    }

    // Single-signer regtest mode still uses signed compact attestations.
    if (min_oracle_count == 1) {
        if (!message.IsValid()) return false;
        return message.VerifyAttestation();
    }

    // Basic field validation without trusting caller-supplied pubkeys.
    if (message.price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD) return false;
    if (message.price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) return false;

    const int64_t now = GetTime();
    if (message.timestamp > now + 60) {
        LogPrintf("Oracle: Rejecting future oracle message from oracle %d (timestamp=%lld, now=%lld)\n",
                 message.oracle_id, message.timestamp, now);
        return false;
    }
    if (message.timestamp < now - ORACLE_MAX_AGE_SECONDS) {
        LogPrintf("Oracle: Rejecting stale oracle message from oracle %d (timestamp=%lld, now=%lld)\n",
                 message.oracle_id, message.timestamp, now);
        return false;
    }

    // Verify oracle ID is in valid range and matches chainparams
    const CChainParams& params = Params();
    const OracleNodeInfo* oracle_config = params.GetOracleNode(message.oracle_id);
    if (!oracle_config || !oracle_config->is_active) {
        return false;
    }

    // SECURITY: Bind pubkey from chainparams before verification.
    // The message may contain an attacker-supplied pubkey — we must verify
    // against the authorized key, not whatever was deserialized from P2P.
    COraclePriceMessage bound_msg = message;
    bound_msg.oracle_pubkey = XOnlyPubKey(oracle_config->pubkey);

    // Verify compact attestation signature against chainparams pubkey.
    return bound_msg.VerifyAttestation();
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

void OracleBundleManager::UpdatePriceCache(int height, uint64_t price_micro_usd, int64_t source_time)
{
    const int64_t effective_update_time = source_time > 0 ? source_time : GetTime();

    // Update the height-to-price map
    {
        std::lock_guard<std::mutex> lock(mtx_price_cache);
        height_to_price[height] = price_micro_usd;
        height_to_price_time[height] = effective_update_time;

        // Keep cache size limited (last 1000 blocks)
        if (height_to_price.size() > 1000) {
            const int erase_height = height_to_price.begin()->first;
            height_to_price.erase(height_to_price.begin());
            height_to_price_time.erase(erase_height);
        }
    }

    // CRITICAL: Also update cached_price so GetLatestPrice() returns the correct value
    // This is the price that GetCurrentOraclePrice() uses for the DigiDollar system
    // The price comes from oracle data embedded in blocks - this is the consensus price
    {
        std::lock_guard<std::mutex> lock(mtx_bundles);
        cached_price = static_cast<CAmount>(price_micro_usd);
        last_update_time = effective_update_time;
    }

    LogPrintf("Oracle: Price cache updated for height %d: %llu micro-USD ($%.6f), source_time=%lld - cached_price updated\n",
             height, price_micro_usd, price_micro_usd / 1000000.0, (long long)effective_update_time);
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
    // RH-44: Must hold both mtx_price_cache (for height_to_price) and
    // mtx_bundles (for cached_price) to avoid data race with GetLatestPrice().
    // Lock order: mtx_bundles before mtx_price_cache to prevent deadlocks.
    std::lock_guard<std::mutex> bundles_lock(mtx_bundles);
    std::lock_guard<std::mutex> price_lock(mtx_price_cache);
    auto it = height_to_price.find(height);
    if (it != height_to_price.end()) {
        height_to_price.erase(it);
        height_to_price_time.erase(height);
        // Revert cached_price to highest remaining height's price
        if (!height_to_price.empty()) {
            const int restored_height = height_to_price.rbegin()->first;
            cached_price = height_to_price.rbegin()->second;
            auto time_it = height_to_price_time.find(restored_height);
            last_update_time = time_it != height_to_price_time.end() ? time_it->second : 0;
        } else {
            cached_price = 0;
            last_update_time = 0;
        }
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Removed price cache for height %d, cached_price reverted to %d\n", height, cached_price);
    }
}

/**
 * OracleDataValidator Implementation
 */

bool OracleDataValidator::ValidateBlockOracleData(const CBlock& block, const CBlockIndex* pindex_prev, const Consensus::Params& params, BlockValidationState& state)
{
    // Oracle validation runs identically on testnet and mainnet. The
    // previous chain-type short-circuit (return true on any chain other
    // than testnet/regtest) meant mainnet would never run the phase-
    // aware validator once DigiDollar activated via BIP9 — miners could
    // publish arbitrary oracle data with no consensus check. Removing the
    // short-circuit lets the BIP9/activation-height gate below decide
    // whether oracle validation applies, in parallel with testnet.
    //
    // Pre-activation behavior is preserved by the BIP9/height gate at
    // the top of the function: any chain where DigiDollar is not yet
    // BIP9-active returns true without running the validator. Blocks
    // without an OP_ORACLE output (or with a malformed one) remain
    // accepted via the intentional transition-period escape hatches
    // further down — the chain must keep producing blocks when the
    // oracle network is temporarily unavailable.

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
                try {
                    block_height = CScriptNum(data, true).getint();
                } catch (const scriptnum_error&) {
                    return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                                         "bad-cb-bip34-height-encoding",
                                         "coinbase BIP34 height push is non-minimal or overflows");
                }
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
    // Scan all coinbase outputs for OP_ORACLE marker (oracle output position varies:
    // may be vout[1] without witness commitment, or vout[2] with witness commitment)
    int oracle_output_count = 0;
    for (const auto& output : coinbase.vout) {
        if (output.scriptPubKey.size() >= 2 &&
            output.scriptPubKey[0] == OP_RETURN &&
            output.scriptPubKey[1] == OP_ORACLE) {
            oracle_output_count++;
        }
    }

    // SECURITY: Reject blocks with multiple oracle outputs (prevents confusion attacks)
    if (oracle_output_count > 1) {
        LogPrintf("Oracle: Block %d has %d oracle outputs (expected at most 1)\n",
                 block_height, oracle_output_count);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-multiple-outputs",
            strprintf("Block contains %d oracle outputs, expected at most 1", oracle_output_count));
    }

    if (oracle_output_count == 0) {
        if (BlockNeedsOraclePrice(block)) {
            LogPrintf("Oracle: price-dependent DD block %d has no oracle bundle\n", block_height);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                                 "bad-oracle-missing",
                                 "DigiDollar mint/redeem blocks must include a valid MuSig2 oracle bundle");
        }
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: block %d has no oracle bundle and no price-dependent DD operation\n", block_height);
        return true;
    }

    // Extract oracle bundle using OracleBundleManager's parser (scans all outputs for OP_ORACLE)
    COracleBundle bundle;
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    if (!manager.ExtractOracleBundle(coinbase, bundle)) {
        LogPrintf("Oracle: Block %d has malformed or legacy oracle data\n", block_height);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                             "bad-oracle-malformed",
                             "Oracle output must be a valid MuSig2 bundle");
    }

    // IMPORTANT: Once we successfully extract a bundle, we MUST validate it fully
    // No transition period leniency for bundles that are present but invalid

    if (!bundle.IsMuSig2()) {
        LogPrintf("Oracle: Block %d used legacy oracle bundle version %d\n",
                  block_height, bundle.version);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                             "bad-oracle-legacy",
                             "DigiDollar V1 accepts only MuSig2 oracle bundles");
    }

    std::string musig_error;
    if (!OracleBundleManager::ValidateMuSig2Bundle(bundle, block_height, params, musig_error)) {
        LogPrintf("Oracle: MuSig2 bundle validation failed at block %d: %s\n",
                 block_height, musig_error);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                            "bad-oracle-musig2",
                            strprintf("MuSig2 oracle bundle validation failed: %s", musig_error));
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
    if (bundle.timestamp > static_cast<int64_t>(block.nTime) + 60) {
        LogPrintf("Oracle: Oracle timestamp in future: oracle=%lld, block=%u\n",
                 (long long)bundle.timestamp, block.nTime);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-oracle-timestamp",
            "Oracle timestamp is in the future");
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

    if (!IsConsensusMuSig2OracleId(message.oracle_id, params)) {
        return false;
    }

    // Verify oracle is authorized
    const CChainParams& chainparams = Params();
    const OracleNodeInfo* oracle_config = chainparams.GetOracleNode(message.oracle_id);
    if (!oracle_config || !oracle_config->is_active) {
        return false;
    }

    // Verify signature
    return message.VerifyAttestation();
}

bool OracleDataValidator::ValidateOracleBundle(const COracleBundle& bundle, int32_t block_height, const Consensus::Params& params)
{
    if (!bundle.IsMuSig2()) {
        LogPrintf("Oracle: Rejecting legacy oracle bundle version %u; DigiDollar V1 requires MuSig2 v0x03\n",
                  bundle.version);
        return false;
    }

    std::string error;
    if (!OracleBundleManager::ValidateMuSig2Bundle(bundle, block_height, params, error)) {
        LogPrintf("Oracle: MuSig2 bundle validation failed at height %d: %s\n",
                  block_height, error);
        return false;
    }

    return true;
}

bool OracleDataValidator::CheckOracleSignatures(const COracleBundle& bundle, const Consensus::Params& params)
{
    (void)params;
    return HasMuSig2Quorum(bundle, params);
}

bool OracleDataValidator::CheckOracleEpoch(const COracleBundle& bundle, int32_t current_epoch)
{
    return bundle.ValidateEpoch(current_epoch);
}

bool OracleDataValidator::CheckOracleConsensus(const COracleBundle& bundle, const Consensus::Params& params)
{
    return HasMuSig2Quorum(bundle, params);
}

bool OracleBundleManager::ValidateBundle(const COracleBundle& bundle, int block_height, const Consensus::Params& params)
{
    (void)block_height;
    return HasMuSig2Quorum(bundle, params);
}

int OracleBundleManager::GetRequiredConsensus(int block_height, const Consensus::Params& params)
{
    (void)block_height;
    return params.nOracleConsensusRequired;
}

/**
 * Compute deterministic hash of oracle bundle consensus data, bound to a
 * specific DigiByte chain identity.
 *
 * For v0x03 (MuSig2) bundles, the aggregate signature covers the consensus
 * price, timestamp, epoch, and the chain's hashGenesisBlock — the fields
 * all participating oracles agreed upon for THIS chain. This hash is used
 * as the message for secp256k1_schnorrsig_verify.
 *
 * DD-FA-SEC-008 — binding the chain identity prevents a bundle signed for
 * one DigiByte chain from being replayed on another chain that shares the
 * oracle roster (mainnet and testnet share oracle keys).
 */
uint256 ComputeOracleBundleHash(const COracleBundle& bundle, const uint256& chain_hash)
{
    // Must match OracleSigningOrchestrator::ComputeOracleMessageHash
    // which hashes (tag, chain_hash, epoch, price, timestamp).
    CHashWriter ss(0);
    ss << std::string{"DigiDollar/OracleBundle"};
    ss << chain_hash;
    ss << bundle.epoch;
    ss << bundle.median_price_micro_usd;
    ss << bundle.timestamp;
    return ss.GetHash();
}

/**
 * Compatibility overload: hash a bundle bound to the active chain's
 * genesis. Production validator paths now pass the consensus params'
 * hashGenesisBlock explicitly so cross-chain replay is rejected even when
 * the validator is invoked with non-active params (e.g., test fixtures).
 */
uint256 ComputeOracleBundleHash(const COracleBundle& bundle)
{
    return ComputeOracleBundleHash(bundle, Params().GetConsensus().hashGenesisBlock);
}

/**
 * MuSig2 Bundle Validation
 *
 * Validates a v0x03 oracle bundle containing:
 * - participation_bitmap: which oracles contributed to the aggregate signature
 * - aggregate_sig: 64-byte BIP-340 Schnorr signature from the MuSig2 ceremony
 * - median_price_micro_usd + timestamp: consensus data signed by the aggregate key
 *
 * Verification steps:
 * 1. Check activation height
 * 2. Validate aggregate_sig size (must be 64 bytes)
 * 3. Validate bitmap is non-empty and decodes correctly
 * 4. Check participating oracle count >= threshold
 * 5. Compute aggregate pubkey from participating oracle subset (via chainparams)
 * 6. Verify aggregate signature against bundle hash using schnorrsig_verify
 */
bool OracleBundleManager::ValidateMuSig2Bundle(const COracleBundle& bundle,
                                                    int32_t block_height,
                                                    const Consensus::Params& params,
                                                    std::string& error)
{
    // Pre-condition: MuSig2 roster must be active
    // Pre-condition: must be a v0x03 bundle
    if (bundle.version != 3) {
        error = "ValidateMuSig2Bundle called with non-v0x03 bundle (version=" + std::to_string(bundle.version) + ")";
        return false;
    }

    if (!params.IsMuSig2OracleActive(block_height)) {
        error = "v0x03 roster activation height not reached";
        return false;
    }

    if (bundle.median_price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD ||
        bundle.median_price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) {
        error = "v0x03 consensus price out of range (" + std::to_string(bundle.median_price_micro_usd) + ")";
        return false;
    }

    // Check aggregate signature size (BIP-340 Schnorr: exactly 64 bytes)
    if (bundle.aggregate_sig.size() != 64) {
        error = "Invalid v0x03 aggregate signature size (" + std::to_string(bundle.aggregate_sig.size()) + " bytes, expected 64)";
        return false;
    }

    // Check bitmap is present
    if (bundle.participation_bitmap.empty()) {
        error = "v0x03 bitmap cannot be empty";
        return false;
    }

    // RC30: Bind the v0x03 payload epoch to the current block's epoch.
    // The signer hashes H(epoch, price, timestamp); a bundle whose payload epoch
    // doesn't match the current epoch cannot verify (and could otherwise enable
    // cross-epoch replay of a previously-valid aggregate signature).
    const int32_t expected_epoch = GetCurrentEpoch(block_height);
    if (bundle.epoch != expected_epoch) {
        error = "v0x03 bundle epoch mismatch (payload=" + std::to_string(bundle.epoch) +
                ", expected=" + std::to_string(expected_epoch) + ")";
        return false;
    }

    // Decode bitmap to get participating oracle IDs
    std::vector<uint8_t> oracle_ids = MuSig2OracleAggregator::DecodeBitmap(
        bundle.participation_bitmap, static_cast<uint16_t>(params.nOracleTotalOracles));

    if (oracle_ids.empty()) {
        error = "v0x03 bitmap decoding failed (malformed bitmap or size mismatch)";
        return false;
    }

    // Check minimum oracle threshold
    if (static_cast<int>(oracle_ids.size()) < params.nOracleConsensusRequired) {
        error = "v0x03 bundle below minimum oracle threshold (" +
                std::to_string(oracle_ids.size()) + " signers, need " +
                std::to_string(params.nOracleConsensusRequired) + ")";
        return false;
    }

    for (uint8_t oracle_id : oracle_ids) {
        if (oracle_id >= static_cast<uint8_t>(params.nOraclePubkeyCount)) {
            error = "v0x03 signer outside active oracle roster (id=" + std::to_string(oracle_id) + ")";
            return false;
        }
    }

    // Compute aggregate pubkey for participating oracles
    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey agg_pk;
    secp256k1_musig_keyagg_cache cache;

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: ValidateMuSig2Bundle: bitmap=%s, total_oracles=%d, oracle_ids=[%s]\n",
             HexStr(bundle.participation_bitmap),
             params.nOracleTotalOracles,
             [&]() -> std::string {
                 std::string s;
                 for (size_t i = 0; i < oracle_ids.size(); ++i) {
                     if (i > 0) s += ",";
                     s += std::to_string(oracle_ids[i]);
                 }
                 return s;
             }());

    const bool using_active_chainparams = &params == &Params().GetConsensus();
    const bool aggregate_ok = using_active_chainparams ?
        aggregator.ComputeAggregatePubkeyFromBitmap(bundle.participation_bitmap,
                                                    static_cast<uint16_t>(params.nOracleTotalOracles),
                                                    agg_pk, cache) :
        ComputeAggregatePubkeyFromConsensusParams(oracle_ids, params, agg_pk, cache);
    if (!aggregate_ok) {
        error = "Failed to compute aggregate pubkey from bitmap";
        return false;
    }

    // Compute message hash (tag, chain_hash, epoch, price, timestamp).
    // DD-FA-SEC-008 — bind validation to params.hashGenesisBlock so a v0x03
    // bundle signed for one DigiByte chain cannot be replayed against
    // another chain's validator that shares the oracle roster.
    uint256 msg_hash = ComputeOracleBundleHash(bundle, params.hashGenesisBlock);

    // Verify aggregate signature using BIP-340 Schnorr verification
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    int result = secp256k1_schnorrsig_verify(ctx,
                                              bundle.aggregate_sig.data(),
                                              msg_hash.begin(), 32,
                                              &agg_pk);
    secp256k1_context_destroy(ctx);

    if (!result) {
        error = "v0x03 aggregate signature verification failed";
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: MuSig2 bundle validated: %zu oracles, price=%llu micro-USD\n",
             oracle_ids.size(), bundle.median_price_micro_usd);

    return true;
}

CAmount OracleBundleManager::CalculateConsensusPrice(const COracleBundle& bundle, const Consensus::Params& params)
{
    // SECURITY: Use price-range checks ONLY — NOT msg.IsValid().
    // IsValid() calls GetTime() for timestamp checks, making the consensus
    // price depend on wall-clock time. During IBD or delayed block relay,
    // oracle timestamps become "stale" relative to GetTime(), causing
    // messages to be excluded. This produces a DIFFERENT consensus price
    // than the miner calculated, rejecting valid blocks and causing chain
    // splits between timely and delayed nodes.
    //
    // Timestamp validation belongs in ValidateBlockOracleData (which uses
    // block.nTime, not GetTime()). Here we only filter by price range.
    std::vector<CAmount> prices;
    for (const auto& msg : bundle.messages) {
        // Price-range filter only — deterministic, time-independent
        if (msg.price_micro_usd >= ORACLE_MIN_PRICE_MICRO_USD &&
            msg.price_micro_usd <= ORACLE_MAX_PRICE_MICRO_USD) {
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
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        CAmount mockPrice = MockOracleManager::GetInstance().GetCurrentPrice();
        if (mockPrice > 0) {
            return mockPrice;
        }
    }

    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    CAmount price_micro_usd = manager.GetLatestPrice();

    // The canonical oracle cache is stored in micro-USD format.
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
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
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
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: No oracle price available in GetCurrentOraclePriceMicroUSD, returning 0\n");
    return 0;
}

CAmount GetOraclePriceForHeight(int nHeight)
{
    // Get oracle bundle manager instance
    OracleBundleManager& manager = OracleBundleManager::GetInstance();

    // Try the block-connected MuSig2 price cache first. This takes priority
    // over MockOracleManager for accurate integration testing.
    uint64_t cached_price = manager.GetOraclePriceForHeight(nHeight);
    if (cached_price > 0) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using cached price for height %d: %llu micro-USD ($%.6f)\n",
                 nHeight, cached_price, cached_price / 1000000.0);
        return static_cast<CAmount>(cached_price);
    }

    // In RegTest mode, fall back to MockOracleManager for simple tests
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        CAmount mockPrice = MockOracleManager::GetInstance().GetCurrentPrice();
        if (mockPrice > 0) {
            LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using mock price for height %d: %lld (MockOracleManager regtest)\n",
                     nHeight, mockPrice);
            return mockPrice;
        }
    }

    // For mempool transactions, try the current in-memory MuSig2 bundle.
    int32_t epoch = GetCurrentEpoch(nHeight);
    COracleBundle bundle = manager.GetCurrentBundle(epoch);

    if (HasMuSig2Quorum(bundle, Params().GetConsensus())) {
        CAmount price = static_cast<CAmount>(bundle.median_price_micro_usd);
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using current epoch price for height %d (epoch %d): %lld micro-USD\n",
                 nHeight, epoch, price);
        return price;
    }

    // Then try the previous epoch for mempool liveness.
    if (epoch > 0) {
        COracleBundle prev_bundle = manager.GetCurrentBundle(epoch - 1);
        if (HasMuSig2Quorum(prev_bundle, Params().GetConsensus())) {
            CAmount price = static_cast<CAmount>(prev_bundle.median_price_micro_usd);
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
