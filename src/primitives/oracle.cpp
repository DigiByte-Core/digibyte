// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/oracle.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <set>

#include <chainparams.h>
#include <hash.h>
#include <logging.h>
#include <protocol.h>
#include <util/time.h>

/**
 * COraclePriceMessage Implementation
 */

COraclePriceMessage::COraclePriceMessage(uint32_t oracle_id_in, uint64_t price_in, int64_t timestamp_in)
    : oracle_id(oracle_id_in), price_micro_usd(price_in), timestamp(timestamp_in)
{
}

bool COraclePriceMessage::IsValid(int64_t reference_time) const
{
    // Check price is positive and in reasonable range
    // price_micro_usd format: 1,000,000 micro-USD = $1.00
    // Uses shared constants from oracle.h: ORACLE_MIN/MAX_PRICE_MICRO_USD
    if (price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD) return false;
    if (price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) return false;

    // Use provided reference time (block time during validation) or current time
    int64_t current_time = (reference_time > 0) ? reference_time : GetTime();

    // Check timestamp is not in the future (with 1 minute tolerance for clock skew)
    if (timestamp > current_time + 60) return false;

    // Check timestamp is not too old (1 hour max)
    if (timestamp < current_time - ORACLE_MAX_AGE_SECONDS) return false;

    // V1 does not accept unsigned compact oracle messages.
    return VerifyAttestation();
}

bool COraclePriceMessage::Sign(const CKey& key, const uint256* merkle_root, const uint256& aux)
{
    // Get message hash
    uint256 hash = GetSignatureHash();

    // Create Schnorr signature (64 bytes)
    schnorr_sig.resize(64);
    if (!key.SignSchnorr(hash, schnorr_sig, merkle_root, aux)) {
        schnorr_sig.clear();
        return false;
    }

    // Set oracle pubkey from private key
    oracle_pubkey = XOnlyPubKey(key.GetPubKey());

    return true;
}

bool COraclePriceMessage::Verify() const
{
    // Check signature size
    if (schnorr_sig.size() != 64) {
        return false;
    }

    // Check pubkey is valid
    if (!oracle_pubkey.IsFullyValid()) {
        return false;
    }

    // Verify Schnorr signature
    uint256 hash = GetSignatureHash();
    return oracle_pubkey.VerifySchnorr(hash, schnorr_sig);
}

bool COraclePriceMessage::CheckForConflictingMessages(const std::vector<COraclePriceMessage>& messages)
{
    if (messages.empty()) return true; // Empty list has no conflicts

    std::map<uint32_t, const COraclePriceMessage*> oracle_messages;

    for (const auto& msg : messages) {
        // Validate message first
        if (!msg.IsValid()) {
            LogPrint(BCLog::DIGIDOLLAR, "CheckForConflictingMessages: Invalid message from oracle %u\n", msg.oracle_id);
            continue; // Skip invalid messages
        }

        auto it = oracle_messages.find(msg.oracle_id);
        if (it != oracle_messages.end()) {
            // Found conflict - same oracle has multiple messages
            const COraclePriceMessage* existing = it->second;

            // Check if they're actually different (not just duplicate)
            if (existing->price_micro_usd != msg.price_micro_usd ||
                existing->timestamp != msg.timestamp) {
                LogPrint(BCLog::DIGIDOLLAR, "CheckForConflictingMessages: Conflict detected for oracle %u: "
                         "existing price %llu micro-USD (time %d) vs new price %llu micro-USD (time %d)\n",
                         msg.oracle_id, existing->price_micro_usd, existing->timestamp,
                         msg.price_micro_usd, msg.timestamp);
                return false; // Conflicting messages found
            }
            // If messages are identical, it's just a duplicate - continue
        } else {
            oracle_messages[msg.oracle_id] = &msg;
        }
    }

    return true; // No conflicts found
}

uint256 COraclePriceMessage::GetSignatureHash() const
{
    // Create deterministic hash for signing (BIP-340 compatible)
    // Hash all message fields EXCEPT signature and pubkey
    CHashWriter ss(0);
    ss << oracle_id;
    ss << price_micro_usd;
    ss << timestamp;
    ss << block_height;
    ss << nonce;

    return ss.GetHash();
}

uint256 COraclePriceMessage::GetAttestationSignatureHash() const
{
    // Compact attestation hash for off-chain MuSig2 inputs. The final on-chain
    // oracle data is the v0x03 aggregate signature, not this individual message.
    CHashWriter ss(0);
    ss << oracle_id;
    ss << price_micro_usd;
    ss << timestamp;
    return ss.GetHash();
}

bool COraclePriceMessage::SignAttestation(const CKey& key)
{
    uint256 hash = GetAttestationSignatureHash();
    schnorr_sig.resize(64);
    if (!key.SignSchnorr(hash, schnorr_sig, nullptr, uint256())) {
        schnorr_sig.clear();
        return false;
    }
    oracle_pubkey = XOnlyPubKey(key.GetPubKey());
    return true;
}

bool COraclePriceMessage::VerifyAttestation() const
{
    if (schnorr_sig.size() != 64) return false;
    if (!oracle_pubkey.IsFullyValid()) return false;
    uint256 hash = GetAttestationSignatureHash();
    return oracle_pubkey.VerifySchnorr(hash, schnorr_sig);
}

bool operator==(const COraclePriceMessage& a, const COraclePriceMessage& b)
{
    return a.oracle_id == b.oracle_id &&
           a.price_micro_usd == b.price_micro_usd &&
           a.timestamp == b.timestamp &&
           a.block_height == b.block_height &&
           a.nonce == b.nonce &&
           a.oracle_pubkey == b.oracle_pubkey &&
           a.schnorr_sig == b.schnorr_sig;
}

bool operator!=(const COraclePriceMessage& a, const COraclePriceMessage& b)
{
    return !(a == b);
}

/**
 * COracleBundle Implementation
 */

COracleBundle::COracleBundle(int32_t epoch_in) : epoch(epoch_in)
{
}

size_t COracleBundle::GetV03PayloadSize() const
{
    // v0x03 on-chain format (RC30):
    //   bitmap_len(1) + bitmap(variable) + epoch(4) + price(8) + timestamp(8) + aggregate_sig(64)
    return 1 + participation_bitmap.size() + 4 + 8 + 8 + 64;
}

std::vector<unsigned char> COracleBundle::SerializeV03Data() const
{
    // Validate: aggregate_sig must be exactly 64 bytes
    if (aggregate_sig.size() != 64) return {};

    // Validate: bitmap must not be empty
    if (participation_bitmap.empty()) return {};

    std::vector<unsigned char> data;
    data.reserve(GetV03PayloadSize());

    // bitmap_len (1 byte)
    data.push_back(static_cast<unsigned char>(participation_bitmap.size()));

    // bitmap (variable)
    data.insert(data.end(), participation_bitmap.begin(), participation_bitmap.end());

    // epoch (4 bytes, little-endian, int32) — RC30: required for signature verification.
    // Signer computes H(epoch, price, timestamp); validator must see the same epoch.
    uint32_t ep = static_cast<uint32_t>(epoch);
    for (int i = 0; i < 4; ++i) {
        data.push_back(static_cast<unsigned char>(ep & 0xFF));
        ep >>= 8;
    }

    // price (8 bytes, little-endian)
    uint64_t price = median_price_micro_usd;
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<unsigned char>(price & 0xFF));
        price >>= 8;
    }

    // timestamp (8 bytes, little-endian)
    uint64_t ts = static_cast<uint64_t>(timestamp);
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<unsigned char>(ts & 0xFF));
        ts >>= 8;
    }

    // aggregate_sig (64 bytes)
    data.insert(data.end(), aggregate_sig.begin(), aggregate_sig.end());

    return data;
}

bool COracleBundle::DeserializeV03Data(const std::vector<unsigned char>& data, COracleBundle& bundle)
{
    // RC30 minimum: bitmap_len(1) + bitmap(>=1) + epoch(4) + price(8) + timestamp(8) + sig(64) = 86
    if (data.size() < 86) return false;

    // Ensure bundle is marked as v0x03 when decoding this payload type.
    bundle.version = 3;

    size_t pos = 0;

    // bitmap_len (1 byte)
    uint8_t bitmap_len = data[pos++];
    if (bitmap_len == 0) return false;

    // Enforce exact payload size to avoid trailing-byte ambiguity/malleability.
    // Need exactly: bitmap_len + 4 (epoch) + 8 (price) + 8 (timestamp) + 64 (sig) after bitmap_len byte
    const size_t expected_size = 1 + bitmap_len + 4 + 8 + 8 + 64;
    if (data.size() != expected_size) return false;

    // bitmap (variable)
    bundle.participation_bitmap.assign(data.begin() + pos, data.begin() + pos + bitmap_len);
    pos += bitmap_len;

    // epoch (4 bytes, little-endian, int32) — RC30
    uint32_t ep = 0;
    for (int i = 0; i < 4; ++i) {
        ep |= static_cast<uint32_t>(data[pos++]) << (i * 8);
    }
    bundle.epoch = static_cast<int32_t>(ep);

    // price (8 bytes, little-endian)
    uint64_t price = 0;
    for (int i = 0; i < 8; ++i) {
        price |= static_cast<uint64_t>(data[pos++]) << (i * 8);
    }
    bundle.median_price_micro_usd = price;

    // timestamp (8 bytes, little-endian)
    uint64_t ts = 0;
    for (int i = 0; i < 8; ++i) {
        ts |= static_cast<uint64_t>(data[pos++]) << (i * 8);
    }
    bundle.timestamp = static_cast<int64_t>(ts);

    // aggregate_sig (64 bytes)
    bundle.aggregate_sig.assign(data.begin() + pos, data.begin() + pos + 64);
    pos += 64;

    return true;
}

bool COracleBundle::IsValid(int min_required, int64_t reference_time) const
{
    (void)min_required;

    // DigiDollar V1 consensus-visible oracle data is MuSig2 v0x03 only.
    // Message-bundle validation is intentionally not a fallback path.
    if (!IsMuSig2()) return false;
    if (aggregate_sig.size() != 64) return false;
    if (participation_bitmap.empty()) return false;
    if (median_price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD ||
        median_price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) {
        return false;
    }

    // Use provided reference time (block time during validation) or current time
    int64_t current_time = (reference_time > 0) ? reference_time : GetTime();

    // Verify timestamp is reasonable (within 1 hour of reference time)
    if (timestamp > current_time + 3600 || timestamp < current_time - 3600) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Bundle timestamp out of range: %d (current: %d)\n", timestamp, current_time);
        return false;
    }

    return true;
}

bool COracleBundle::AddMessage(const COraclePriceMessage& message)
{
    // Don't allow more messages than the reserved oracle slot capacity.
    if (messages.size() >= ORACLE_ACTIVE_COUNT) return false;

    // Check if oracle already submitted a message
    for (const auto& existing : messages) {
        if (existing.oracle_id == message.oracle_id) return false;
    }

    // Note: Timestamp/price validation should be done at P2P layer via ValidateIncomingMessage()
    // AddMessage() only checks structural constraints (no duplicates, size limits)

    messages.push_back(message);
    return true;
}

bool COracleBundle::HasConsensus(int min_required) const
{
    std::set<uint32_t> unique_oracle_ids;
    for (const auto& msg : messages) {
        if (!unique_oracle_ids.insert(msg.oracle_id).second) {
            return false;
        }
    }

    return unique_oracle_ids.size() >= static_cast<size_t>(min_required);
}

uint64_t COracleBundle::GetConsensusPrice(int min_required) const
{
    if (!HasConsensus(min_required)) return 0;

    // CONSENSUS-CRITICAL: This MUST use the exact same IQR algorithm as
    // OracleBundleManager::CalculateConsensusPrice() to avoid consensus forks.
    // See T9-01: Unified oracle median/outlier formula.

    // Step 1: Price-range filter only — deterministic, time-independent
    std::vector<int64_t> prices;
    for (const auto& msg : messages) {
        if (msg.price_micro_usd >= ORACLE_MIN_PRICE_MICRO_USD &&
            msg.price_micro_usd <= ORACLE_MAX_PRICE_MICRO_USD) {
            prices.push_back(static_cast<int64_t>(msg.price_micro_usd));
        }
    }

    if (prices.empty()) return 0;

    // Step 2: Sort for IQR calculation
    std::sort(prices.begin(), prices.end());

    // Step 3: If less than 4 prices, just return median without outlier filtering
    if (prices.size() < 4) {
        size_t mid = prices.size() / 2;
        if (prices.size() % 2 == 0) {
            return static_cast<uint64_t>((prices[mid - 1] + prices[mid]) / 2);
        }
        return static_cast<uint64_t>(prices[mid]);
    }

    // Step 4: Apply IQR outlier filtering (1.5 * IQR rule)
    size_t q1_idx = prices.size() / 4;
    size_t q3_idx = (prices.size() * 3) / 4;
    int64_t q1 = prices[q1_idx];
    int64_t q3 = prices[q3_idx];
    int64_t iqr = q3 - q1;
    int64_t lower_bound = q1 - (iqr * 3 / 2);  // 1.5 * IQR below Q1
    int64_t upper_bound = q3 + (iqr * 3 / 2);  // 1.5 * IQR above Q3

    // Step 5: Filter outliers
    std::vector<int64_t> filtered;
    for (int64_t price : prices) {
        if (price >= lower_bound && price <= upper_bound) {
            filtered.push_back(price);
        }
    }

    // Step 6: If filtering removed all prices, fall back to unfiltered median
    if (filtered.empty()) {
        size_t mid = prices.size() / 2;
        if (prices.size() % 2 == 0) {
            return static_cast<uint64_t>((prices[mid - 1] + prices[mid]) / 2);
        }
        return static_cast<uint64_t>(prices[mid]);
    }

    // Step 7: Calculate median of filtered prices
    std::sort(filtered.begin(), filtered.end());
    size_t mid = filtered.size() / 2;
    if (filtered.size() % 2 == 0) {
        return static_cast<uint64_t>((filtered[mid - 1] + filtered[mid]) / 2);
    }
    return static_cast<uint64_t>(filtered[mid]);
}

bool COracleBundle::ValidateEpoch(int32_t current_epoch) const
{
    // Accept current epoch or previous epoch only
    return epoch == current_epoch || int64_t{epoch} == int64_t{current_epoch} - 1;
}

bool operator==(const COracleBundle& a, const COracleBundle& b)
{
    return a.epoch == b.epoch && a.messages == b.messages;
}

bool operator!=(const COracleBundle& a, const COracleBundle& b)
{
    return !(a == b);
}

/**
 * OracleNode Implementation
 */

OracleNodeInfo::OracleNodeInfo(uint32_t id_in, const CPubKey& pubkey_in, const std::string& endpoint_in, bool is_active_in, const std::string& display_name_in)
    : id(id_in), pubkey(pubkey_in), endpoint(endpoint_in), is_active(is_active_in), display_name(display_name_in)
{
}

bool OracleNodeInfo::IsValid() const
{
    // Check pubkey is valid
    if (!pubkey.IsValid()) return false;

    // Check endpoint is not empty and has reasonable format
    if (endpoint.empty()) return false;

    // Basic format check: should contain a colon for host:port
    size_t colon_pos = endpoint.find(':');
    if (colon_pos == std::string::npos) return false;

    // Check port is valid (1-65535)
    std::string port_str = endpoint.substr(colon_pos + 1);
    try {
        int port = std::stoi(port_str);
        if (port < 1 || port > 65535) return false;
    } catch (...) {
        return false;
    }

    return true;
}

bool operator==(const OracleNodeInfo& a, const OracleNodeInfo& b)
{
    return a.id == b.id &&
           a.pubkey == b.pubkey &&
           a.endpoint == b.endpoint &&
           a.is_active == b.is_active;
}

bool operator!=(const OracleNodeInfo& a, const OracleNodeInfo& b)
{
    return !(a == b);
}

/**
 * Oracle Selection Functions
 */

uint256 GetOracleEpochSelectionHash(int32_t epoch, uint32_t oracle_id)
{
    return GetOracleEpochSelectionHash(epoch, oracle_id, Params().GetConsensus().hashGenesisBlock);
}

uint256 GetOracleEpochSelectionHash(int32_t epoch, uint32_t oracle_id, const uint256& epoch_seed)
{
    HashWriter hasher{};
    hasher << std::string{"DigiDollar/OracleEpochSelection/v1"};
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch_seed;
    hasher << epoch;
    hasher << oracle_id;
    return hasher.GetHash();
}

std::vector<OracleNodeInfo> SelectOraclesForEpoch(const std::vector<OracleNodeInfo>& all_oracles, int32_t epoch)
{
    // Filter active oracles
    std::vector<OracleNodeInfo> active_oracles;
    for (const auto& oracle : all_oracles) {
        if (oracle.is_active) {
            active_oracles.push_back(oracle);
        }
    }

    // Use deterministic scoring based on epoch for every roster size. When
    // there are 17 or fewer active oracles we still return all of them, but in
    // epoch-scored order so callers that take a threshold subset do not fall
    // back to fixed chainparams order.
    std::vector<OracleNodeInfo> selected;
    selected.reserve(ORACLE_ACTIVE_COUNT);

    // Create indices for deterministic sorting
    std::vector<size_t> indices;
    for (size_t i = 0; i < active_oracles.size(); i++) {
        indices.push_back(i);
    }

    // Sort indices by oracle scores (deterministic based on epoch)
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        const uint256 score_a = GetOracleEpochSelectionHash(epoch, active_oracles[a].id);
        const uint256 score_b = GetOracleEpochSelectionHash(epoch, active_oracles[b].id);
        if (score_a == score_b) {
            return active_oracles[a].id < active_oracles[b].id;
        }
        return score_a < score_b;
    });

    // Select the first active-per-epoch entries after deterministic sorting.
    for (size_t i = 0; i < indices.size() && selected.size() < ORACLE_ACTIVE_COUNT; i++) {
        selected.push_back(active_oracles[indices[i]]);
    }

    // Ensure we have exactly ORACLE_ACTIVE_COUNT, or all available if fewer.
    selected.resize(std::min(selected.size(), size_t(ORACLE_ACTIVE_COUNT)));

    return selected;
}

int32_t GetCurrentEpoch(int32_t block_height)
{
    // Get DigiDollar oracle signing epoch length from consensus parameters.
    // V1 uses 40-block (~10 minute) epochs on mainnet, testnet, and regtest.
    const Consensus::Params& params = Params().GetConsensus();
    int32_t epoch_length = params.nDDOracleEpochBlocks;

    if (epoch_length <= 0) {
        // Fallback to default if not set (shouldn't happen)
        epoch_length = 1440;
    }

    return block_height / epoch_length;
}

/**
 * Oracle P2P Validation Implementation
 */

namespace OracleP2P {

// Rate limiting state
static std::map<uint32_t, std::pair<int64_t, int>> rate_limit_state; // oracle_id -> (last_time, count)
static std::mutex rate_limit_mutex;

bool ValidateIncomingMessage(const COraclePriceMessage& message)
{
    // Basic validation first
    if (!message.IsValid()) return false;

    // Check oracle ID is within valid range
    if (message.oracle_id >= ORACLE_TOTAL_COUNT) return false;

    // Check message size
    if (!CheckMessageSize(message)) return false;

    // Check rate limiting
    if (!CheckRateLimit(message.oracle_id)) return false;

    return true;
}

bool ValidateBundleMessage(const COracleBundle& bundle)
{
    (void)bundle;
    return false;
}

bool ValidateGetOracleRequest(const GetOracleDataMsg& request)
{
    // Epoch must be non-negative
    if (request.epoch < 0) return false;

    // Oracle ID must be valid (or 0xFFFFFFFF for all)
    if (request.oracle_id != 0xFFFFFFFF && request.oracle_id >= ORACLE_TOTAL_COUNT) {
        return false;
    }

    return true;
}

bool CheckRateLimit(uint32_t oracle_id)
{
    std::lock_guard<std::mutex> lock(rate_limit_mutex);

    int64_t current_time = GetTime();
    const int max_messages_per_minute = 3;
    const int64_t time_window = 60; // seconds

    auto it = rate_limit_state.find(oracle_id);
    if (it == rate_limit_state.end()) {
        // First message from this oracle
        rate_limit_state[oracle_id] = {current_time, 1};
        return true;
    }

    int64_t last_time = it->second.first;
    int count = it->second.second;

    if (current_time - last_time > time_window) {
        // Time window expired, reset counter
        rate_limit_state[oracle_id] = {current_time, 1};
        return true;
    } else {
        // Within time window, check count
        if (count >= max_messages_per_minute) {
            return false; // Rate limited
        } else {
            rate_limit_state[oracle_id] = {last_time, count + 1};
            return true;
        }
    }
}

bool CheckMessageSize(const COraclePriceMessage& message)
{
    const size_t expected_schnorr_sig_size = 64; // BIP-340 Schnorr signature size

    // Check signature size - must be exactly 64 bytes for Schnorr
    if (message.schnorr_sig.size() != expected_schnorr_sig_size) return false;

    // Message structure is now fixed size with Schnorr signatures
    return true;
}

void UpdateRateLimits()
{
    std::lock_guard<std::mutex> lock(rate_limit_mutex);

    int64_t current_time = GetTime();
    const int64_t cleanup_age = 3600; // Clean up entries older than 1 hour

    auto it = rate_limit_state.begin();
    while (it != rate_limit_state.end()) {
        if (current_time - it->second.first > cleanup_age) {
            it = rate_limit_state.erase(it);
        } else {
            ++it;
        }
    }
}

void ClearRateLimitState()
{
    std::lock_guard<std::mutex> lock(rate_limit_mutex);
    rate_limit_state.clear();
}

} // namespace OracleP2P
