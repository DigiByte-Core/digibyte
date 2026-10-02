// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_PRIMITIVES_ORACLE_H
#define DIGIBYTE_PRIMITIVES_ORACLE_H

#include <consensus/amount.h>
#include <key.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

//! Oracle consensus constants (defined before classes for use in default parameters)
static constexpr int ORACLE_CONSENSUS_REQUIRED = 7;     // 7 oracle signatures required
static constexpr int ORACLE_ACTIVE_COUNT = 35;          // Maximum active oracle roster capacity
static constexpr int ORACLE_TOTAL_COUNT = 35;           // 35 total reserved oracle slots
static constexpr int ORACLE_MAX_AGE_SECONDS = 3600;    // 1 hour max age for prices
static constexpr uint64_t ORACLE_MIN_PRICE_MICRO_USD = 100;          // $0.0001 minimum
static constexpr uint64_t ORACLE_MAX_PRICE_MICRO_USD = 100000000;    // $100.00 maximum

/**
 * Oracle Price Message
 * Individual price report from a single oracle node
 * Uses BIP-340 Schnorr signatures for compact, efficient verification
 */
class COraclePriceMessage
{
public:
    uint32_t oracle_id{0};
    uint64_t price_micro_usd{0};         // Price in DigiDollar cents (100 = $1.00) - field name kept for compatibility
    int64_t timestamp{0};                // Unix timestamp
    int32_t block_height{0};             // Block height when created
    uint64_t nonce{0};                   // Random nonce for uniqueness
    XOnlyPubKey oracle_pubkey;           // Schnorr public key (32 bytes)
    std::vector<unsigned char> schnorr_sig;  // Schnorr signature (64 bytes)

    //! Constructors
    COraclePriceMessage() = default;
    COraclePriceMessage(uint32_t oracle_id_in, uint64_t price_in, int64_t timestamp_in);

    //! Serialization
    SERIALIZE_METHODS(COraclePriceMessage, obj)
    {
        READWRITE(obj.oracle_id);
        READWRITE(obj.price_micro_usd);
        READWRITE(obj.timestamp);
        READWRITE(obj.block_height);
        READWRITE(obj.nonce);
        READWRITE(obj.oracle_pubkey);
        READWRITE(obj.schnorr_sig);
    }

    //! Validation
    bool IsValid(int64_t reference_time = 0) const;

    /**
     * Sign the message with a private key using Schnorr signature
     * @param key Private key to sign with
     * @param merkle_root Optional Taproot merkle root (nullptr for oracle signatures)
     * @param aux Optional auxiliary random data (default: zero hash)
     * @return true if signature was created successfully
     */
    bool Sign(const CKey& key, const uint256* merkle_root = nullptr, const uint256& aux = uint256());

    /**
     * Verify Schnorr signature
     * @return true if signature is valid
     */
    bool Verify() const;

    //! Get hash for signature verification
    uint256 GetSignatureHash() const;

    /**
     * Get compact attestation signature hash (oracle_id + price + timestamp).
     * These signatures are off-chain inputs to MuSig2 aggregation; they are not
     * accepted as V1 on-chain oracle bundles.
     */
    uint256 GetAttestationSignatureHash() const;

    /**
     * Sign using the compact attestation hash.
     */
    bool SignAttestation(const CKey& key);

    /**
     * Verify using the compact attestation hash.
     */
    bool VerifyAttestation() const;

    /**
     * Check for conflicting messages from the same oracle.
     * Detects if an oracle has submitted multiple different price messages.
     * @param messages Vector of oracle messages to check
     * @return true if no conflicts found, false if conflicts detected
     */
    static bool CheckForConflictingMessages(const std::vector<COraclePriceMessage>& messages);

    //! Equality operators
    friend bool operator==(const COraclePriceMessage& a, const COraclePriceMessage& b);
    friend bool operator!=(const COraclePriceMessage& a, const COraclePriceMessage& b);
};

/**
 * Oracle Bundle
 * Collection of oracle messages for consensus calculation
 */
class COracleBundle
{
public:
    std::vector<COraclePriceMessage> messages;
    int32_t epoch{0};
    uint64_t median_price_micro_usd{0};      // Median price in DigiDollar cents (100 = $1.00) - field name kept for compatibility
    int64_t timestamp{0};                     // Unix timestamp of bundle creation

    //! MuSig2 v0x03 fields
    uint8_t version{3};
    std::vector<unsigned char> aggregate_sig;             // 64 bytes BIP-340 Schnorr for v0x03
    std::vector<unsigned char> participation_bitmap;      // variable-length bitmap for v0x03

    //! Constructors
    COracleBundle() = default;
    explicit COracleBundle(int32_t epoch_in);

    //! Serialization
    SERIALIZE_METHODS(COracleBundle, obj)
    {
        READWRITE(obj.version);
        READWRITE(obj.messages, obj.epoch, obj.median_price_micro_usd, obj.timestamp);
        if (obj.version >= 3) {
            READWRITE(obj.aggregate_sig, obj.participation_bitmap);
        }
    }

    //! v0x03 MuSig2 helpers
    bool IsMuSig2() const { return version == 3; }

    //! Get v0x03 on-chain payload size: bitmap_len(1) + bitmap + price(8) + timestamp(8) + sig(64)
    size_t GetV03PayloadSize() const;

    //! Serialize v0x03 on-chain data (standalone, no bundle_manager dependency)
    std::vector<unsigned char> SerializeV03Data() const;

    //! Deserialize v0x03 on-chain data into a COracleBundle
    static bool DeserializeV03Data(const std::vector<unsigned char>& data, COracleBundle& bundle);

    //! Validation
    //! @param min_required  Number of oracle messages required for off-chain consensus
    //! @param reference_time  Block time for timestamp validation (0 = use current time)
    bool IsValid(int min_required, int64_t reference_time = 0) const;  // Validate bundle structure and signatures

    //! Message management
    bool AddMessage(const COraclePriceMessage& message);

    //! Off-chain consensus validation for MuSig2 inputs.
    bool HasConsensus(int min_required) const;
    uint64_t GetConsensusPrice(int min_required) const;
    bool ValidateEpoch(int32_t current_epoch) const;

    //! Equality operators
    friend bool operator==(const COracleBundle& a, const COracleBundle& b);
    friend bool operator!=(const COracleBundle& a, const COracleBundle& b);
};

/**
 * Oracle Node Definition
 * Represents a single oracle node in the network
 */
struct OracleNodeInfo
{
    uint32_t id{0};
    CPubKey pubkey;
    std::string endpoint;
    bool is_active{false};
    //! Local roster metadata; excluded from serialization and node identity.
    std::string display_name;

    //! Constructors
    OracleNodeInfo() = default;
    OracleNodeInfo(uint32_t id_in, const CPubKey& pubkey_in, const std::string& endpoint_in, bool is_active_in, const std::string& display_name_in = {});

    //! Serialization
    SERIALIZE_METHODS(OracleNodeInfo, obj)
    {
        READWRITE(obj.id);
        READWRITE(obj.pubkey);
        READWRITE(obj.endpoint);
        READWRITE(obj.is_active);
    }

    //! Validation
    bool IsValid() const;

    //! Equality operators
    friend bool operator==(const OracleNodeInfo& a, const OracleNodeInfo& b);
    friend bool operator!=(const OracleNodeInfo& a, const OracleNodeInfo& b);
};

/**
 * Oracle Selection Functions
 * Deterministic oracle selection for each epoch
 */

//! Deterministic per-epoch score for an oracle ID. Lower score wins.
uint256 GetOracleEpochSelectionHash(int32_t epoch, uint32_t oracle_id);
uint256 GetOracleEpochSelectionHash(int32_t epoch, uint32_t oracle_id, const uint256& epoch_seed);

//! Select active oracles for given epoch (deterministic, RC30)
std::vector<OracleNodeInfo> SelectOraclesForEpoch(const std::vector<OracleNodeInfo>& all_oracles, int32_t epoch);

//! Get current epoch based on block height
int32_t GetCurrentEpoch(int32_t block_height);

//! Forward declarations for P2P
class GetOracleDataMsg;

/**
 * Oracle P2P Validation Namespace
 * Handles validation of oracle messages over the P2P network with DOS protection
 */
namespace OracleP2P {

    //! Message validation
    /**
     * Validate incoming oracle price message from P2P network.
     * Performs comprehensive validation including basic checks, rate limiting, and size limits.
     * @param message The oracle price message to validate
     * @return true if message passes all validation checks
     */
    bool ValidateIncomingMessage(const COraclePriceMessage& message);

    /**
     * Validate oracle bundle message from P2P network.
     * Checks bundle size limits, message validity, and duplicate prevention.
     * @param bundle The oracle bundle to validate
     * @return true if bundle is valid for P2P transmission
     */
    bool ValidateBundleMessage(const COracleBundle& bundle);

    /**
     * Validate oracle data request message.
     * Ensures request parameters are within acceptable bounds.
     * @param request The oracle data request to validate
     * @return true if request is valid
     */
    bool ValidateGetOracleRequest(const GetOracleDataMsg& request);

    //! Rate limiting and DOS protection
    /**
     * Check if oracle ID is within rate limits.
     * Implements sliding window rate limiting to prevent spam.
     * @param oracle_id The oracle ID to check
     * @return true if within rate limits, false if rate limited
     */
    bool CheckRateLimit(uint32_t oracle_id);

    /**
     * Validate message size to prevent oversized messages.
     * @param message The message to check
     * @return true if message size is acceptable
     */
    bool CheckMessageSize(const COraclePriceMessage& message);

    /**
     * Update and cleanup rate limiting state.
     * Should be called periodically to clean up old entries.
     */
    void UpdateRateLimits();

    //! Internal state management
    /**
     * Clear all rate limiting state.
     * Used for testing and node restart scenarios.
     */
    void ClearRateLimitState();

} // namespace OracleP2P

#endif // DIGIBYTE_PRIMITIVES_ORACLE_H
