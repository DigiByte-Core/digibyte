// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_ORACLE_BUNDLE_MANAGER_H
#define DIGIBYTE_ORACLE_BUNDLE_MANAGER_H

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <protocol.h>
#include <set>
#include <unordered_map>

#include <consensus/amount.h>
#include <primitives/oracle.h>
#include <script/script.h>
#include <uint256.h>

class CBlock;
class CTransaction;
class CBlockIndex;
class BlockValidationState;
class CConnman;
class ChainstateManager;
class OracleMusigNonceMsg;
class OracleMusigPartialSigMsg;

namespace Consensus { struct Params; }

/**
 * Oracle Bundle Manager
 * Manages oracle message collection, validation, and block integration
 */
class OracleBundleManager
{
private:
    mutable std::mutex mtx_bundles;
    mutable std::recursive_mutex mtx_messages;
    mutable std::condition_variable_any m_messages_updated_cv;

    // Current oracle bundles by epoch
    std::unordered_map<int32_t, COracleBundle> epoch_bundles;

    // Individual oracle messages waiting to be bundled
    std::unordered_map<uint32_t, COraclePriceMessage> pending_messages;

    // Track seen messages for duplicate detection
    std::set<uint256> seen_message_hashes;

    // Cached price data
    CAmount cached_price{0};
    int32_t cached_epoch{-1};
    int64_t last_update_time{0};

    // Configuration
    bool enabled{true};
    int32_t min_oracle_count{ORACLE_CONSENSUS_REQUIRED};
    int32_t total_oracle_count{ORACLE_ACTIVE_COUNT};
    std::chrono::milliseconds near_quorum_wait_timeout{std::chrono::seconds(2)};
    std::chrono::milliseconds near_quorum_wait_poll_interval{std::chrono::milliseconds(200)};
    uint64_t near_quorum_wait_attempts{0};
    uint64_t near_quorum_wait_successes{0};
    uint64_t near_quorum_wait_timeouts{0};

public:
    OracleBundleManager();
    ~OracleBundleManager();

    //! Configuration
    void SetEnabled(bool enable) { enabled = enable; }
    bool IsEnabled() const { return enabled; }
    void SetMinOracleCount(int32_t min_count) { min_oracle_count = min_count; }
    int32_t GetMinOracleCount() const { return min_oracle_count; }

    //! Message management
    bool AddOracleMessage(const COraclePriceMessage& message);
    bool RemoveOracleMessage(uint32_t oracle_id);
    std::vector<COraclePriceMessage> GetPendingMessages() const;
    size_t GetPendingMessageCount() const;
    void ClearPendingMessages();

    //! For testing: directly inject a message into pending (bypasses validation)
    void InjectTestMessage(const COraclePriceMessage& message);

    //! Consensus attestation management for MuSig2 signing
    //! Consensus attestations are messages signed over the consensus price/timestamp
    //! (as opposed to individual oracle prices). They feed aggregation only and
    //! are never mined as on-chain bundles.
    bool AddConsensusAttestation(const COraclePriceMessage& attestation);
    std::vector<COraclePriceMessage> GetPendingAttestations() const;
    size_t GetPendingAttestationCount() const;
    void ClearPendingAttestations();

    //! Compute consensus values from pending individual messages
    //! @param[out] consensus_price Computed consensus price (IQR median)
    //! @param[out] consensus_timestamp Computed consensus timestamp (median of message timestamps)
    //! @return true if enough messages exist for consensus computation
    bool ComputeConsensusValues(uint64_t& consensus_price, int64_t& consensus_timestamp) const;
    //! Compute consensus values from the exact oracle IDs selected for a MuSig2 signing context.
    bool ComputeConsensusValuesForOracles(const std::vector<uint8_t>& oracle_ids,
                                          uint64_t& consensus_price,
                                          int64_t& consensus_timestamp) const;
    //! Return true only when proposed consensus values exactly match locally computed pending-message consensus.
    bool ValidateConsensusProposal(uint64_t consensus_price, int64_t consensus_timestamp) const;

    //! Bundle management
    COracleBundle GetCurrentBundle(int32_t epoch) const;
    bool UpdateBundle(const COracleBundle& bundle);
    bool HasValidBundle(int32_t epoch) const;
    void CleanupOldBundles(int32_t current_epoch);

    //! Block integration
    bool AddOracleBundleToBlock(CBlock& block, int32_t block_height);
    CScript CreateOracleScript(const COracleBundle& bundle) const;
    bool ExtractOracleBundle(const CTransaction& coinbase_tx, COracleBundle& bundle) const;
    bool TryCreateBundle(int32_t epoch);  // Explicitly create bundle for given epoch

    //! MuSig2 session lifecycle
    bool StartMuSig2Session(int32_t block_height);
    bool CompleteMuSig2Session(int32_t block_height);

    //! MuSig2 P2P message ingestion — called by net_processing when
    //! receiving ORACLEMUSIGNONCE / ORACLEMUSIGPARTIALSIG from peers.
    //! These feed remote nonces/partial-sigs into g_oracle_signing_sessions
    //! so the local miner can aggregate them into v0x03 bundles.
    bool ProcessRemoteMusigNonce(const OracleMusigNonceMsg& msg);
    bool ProcessRemoteMusigPartialSig(const OracleMusigPartialSigMsg& msg);

    //! Price functions
    CAmount GetConsensusPrice(int32_t epoch) const;
    CAmount GetLatestPrice() const;
    bool UpdateCachedPrice(int32_t epoch);

    //! Validation
    bool ValidateOracleBundle(const COracleBundle& bundle, int32_t block_height, const Consensus::Params& params) const;
    bool ValidateOracleDataInBlock(const CBlock& block, int32_t block_height, const Consensus::Params& params) const;

    //! V0x03 script format validation
    bool ValidateV03BundleFormat(const CScript& script, uint8_t& version);

    /** Validate against the supplied epoch settings, genesis, quorum, and full compressed oracle keys. */
    static bool ValidateMuSig2Bundle(const COracleBundle& bundle, int32_t block_height, const Consensus::Params& params, std::string& error);
    static bool ValidateBundle(const COracleBundle& bundle, int block_height, const Consensus::Params& params);
    static int GetRequiredConsensus(int block_height, const Consensus::Params& params);
    static CAmount CalculateConsensusPrice(const COracleBundle& bundle, const Consensus::Params& params);

    //! Network functions
    bool BroadcastMessage(const COraclePriceMessage& message);
    void ProcessIncomingMessage(const COraclePriceMessage& message);
    bool HasOracleMessage(const uint256& hash) const;
    bool AddVersionHeartbeat(const OracleVersionHeartbeatMsg& heartbeat);
    bool GetVersionHeartbeat(uint32_t oracle_id, OracleVersionHeartbeatMsg& heartbeat_out) const;
    std::vector<OracleVersionHeartbeatMsg> GetVersionHeartbeats() const;
    bool BroadcastVersionHeartbeat(const OracleVersionHeartbeatMsg& heartbeat);

    /**
     * Broadcast a consensus proposal (epoch, price, timestamp) to the P2P network.
     * Called by AddOracleBundleToBlock() when consensus is computed but not enough
     * attestations exist. Remote oracle nodes will sign and send back attestations.
     * @param epoch Current oracle epoch
     * @param consensus_price IQR-filtered median price
     * @param consensus_timestamp Median timestamp
     * @return true if broadcast was initiated
     */
    bool BroadcastConsensusProposal(int32_t epoch, uint64_t consensus_price, int64_t consensus_timestamp);

    /**
     * Check if a consensus proposal has already been broadcast for this epoch.
     * Prevents spamming the network with duplicate proposals.
     */
    bool HasBroadcastConsensusProposal(int32_t epoch) const;

    /**
     * Track seen attestation hashes for replay prevention.
     * @param hash P2P attestation message hash
     * @return true if this is a new (unseen) attestation
     */
    bool HasSeenAttestation(const uint256& hash) const;
    bool RegisterSeenAttestation(const uint256& hash);

    /**
     * Register a hash in the seen_message_hashes set (P2P dedup).
     * Called by net_processing after a successful AddOracleMessage() to ensure
     * the P2P wrapper hash (OraclePriceMsg::GetHash()) is also tracked.
     * This prevents duplicate processing when the same message arrives from
     * multiple peers.
     * @param hash The hash to register (typically OraclePriceMsg::GetHash())
     */
    void RegisterSeenHash(const uint256& hash);

    void SetConnman(CConnman* connman);

    //! Status and statistics
    struct OracleStats {
        size_t pending_messages;
        size_t active_bundles;
        CAmount latest_price;
        int32_t latest_epoch;
        int64_t last_update;
        bool has_consensus;
        uint64_t near_quorum_wait_attempts;
        uint64_t near_quorum_wait_successes;
        uint64_t near_quorum_wait_timeouts;
    };
    OracleStats GetStats() const;

    //! Singleton access
    static OracleBundleManager& GetInstance();
    static void Initialize();
    static void Shutdown();

    //! Load oracle prices from blockchain on startup
    //! Must be called after chainstate is fully loaded
    //! Returns false when a post-activation block that should be present could
    //! not be read (incomplete/damaged block data) — the caller must abort
    //! startup rather than reconstruct price/volatility state from partial data.
    static bool LoadPricesFromChain(ChainstateManager& chainman);
    //! Startup price-scan per-block gate = the BIP9 DigiDollar-activation predicate for
    //! block_index. Production uses the ChainstateManager overload (shared, memoized
    //! versionbits cache — O(1) amortized, the fix for the ~15-minute startup hang); the
    //! Consensus::Params overload is the non-memoized path retained for tests/edge callers.
    static bool ShouldLoadStartupOraclePriceForBlock(int height, const CBlockIndex* block_index, const Consensus::Params& params);
    static bool ShouldLoadStartupOraclePriceForBlock(int height, const CBlockIndex* block_index, const ChainstateManager& chainman);

    //! Clear all state (for testing)
    void Clear();

    //! Configuration validation
    bool ValidateConfiguration() const;

    //! Price cache management
    /**
     * Update oracle price cache for a specific height
     * @param height Block height
     * @param price_micro_usd Price in micro-USD
     * @param source_time Oracle bundle timestamp, or 0 to use current node time
     */
    void UpdatePriceCache(int height, uint64_t price_micro_usd, int64_t source_time = 0);

    /**
     * Get oracle price for a specific height
     * @param height Block height
     * @return Price in micro-USD, or 0 if not available
     */
    uint64_t GetOraclePriceForHeight(int height) const;

    /**
     * Remove oracle price cache for a specific height (used during block disconnect)
     * @param height Block height to remove
     */
    void RemovePriceCache(int height);

private:
    //! Internal helpers
    void UpdateEpochBundle(int32_t epoch);
    bool IsValidOracleMessage(const COraclePriceMessage& message) const;
    std::vector<uint32_t> GetActiveOraclesForEpoch(int32_t epoch) const;
    bool HasRequiredSignatures(const COracleBundle& bundle, int32_t block_height) const;

    //! Consensus attestations signed over MuSig2 consensus values
    //! Separate from pending_messages (which contain individual prices)
    std::unordered_map<uint32_t, COraclePriceMessage> pending_attestations;

    //! Track which epochs have had consensus proposals broadcast (prevent spam)
    std::set<int32_t> broadcast_proposal_epochs;

    //! Track seen attestation hashes (replay prevention)
    std::set<uint256> seen_attestation_hashes;

    //! Latest verified oracle software/protocol heartbeat per oracle ID.
    std::unordered_map<uint32_t, OracleVersionHeartbeatMsg> version_heartbeats;

    //! Price cache (block height -> price in micro-USD)
    std::map<int, uint64_t> height_to_price;
    std::map<int, int64_t> height_to_price_time;
    mutable std::mutex mtx_price_cache;

    //! P2P connection manager for broadcasting
    CConnman* m_connman{nullptr};
};

/**
 * Oracle Data Validator
 * Validates oracle data in blocks and transactions
 */
class OracleDataValidator
{
public:
    //! Block validation
    static bool ValidateBlockOracleData(const CBlock& block, const CBlockIndex* pindex_prev, const Consensus::Params& params, BlockValidationState& state);

    //! Transaction validation for DigiDollar operations
    static bool ValidateOraclePriceForTx(const CTransaction& tx, CAmount oracle_price, int32_t block_height);

    //! Oracle message validation
    static bool ValidateOracleMessage(const COraclePriceMessage& message, const Consensus::Params& params);

    //! Bundle validation
    static bool ValidateOracleBundle(const COracleBundle& bundle, int32_t block_height, const Consensus::Params& params);

private:
    //! Internal validation helpers
    static bool CheckOracleSignatures(const COracleBundle& bundle, const Consensus::Params& params);
    static bool CheckOracleEpoch(const COracleBundle& bundle, int32_t current_epoch);
    static bool CheckOracleConsensus(const COracleBundle& bundle, const Consensus::Params& params);
};

//! Global oracle bundle manager instance
extern std::unique_ptr<OracleBundleManager> g_oracle_bundle_manager;

//! Compute deterministic v0x03 MuSig2 message hash bound to a chain identity.
//! Used as the message for MuSig2 aggregate signature verification.
//! DD-FA-SEC-008 — the chain's hashGenesisBlock prevents cross-chain replay.
uint256 ComputeOracleBundleHash(const COracleBundle& bundle, const uint256& chain_hash);

//! Compatibility overload: hash a bundle bound to the active chain's genesis.
uint256 ComputeOracleBundleHash(const COracleBundle& bundle);

//! Utility functions for integration with existing code
namespace OracleIntegration {

    //! Get current oracle price for DigiDollar operations (LEGACY - returns cents)
    //! WARNING: For sub-cent prices (< $0.01/DGB), this rounds to 1 cent minimum.
    //! For precision with sub-cent prices, use GetCurrentOraclePriceMicroUSD() instead.
    CAmount GetCurrentOraclePrice();

    //! Get current oracle price in micro-USD (1,000,000 = $1.00)
    //! This provides full precision for sub-cent DGB prices.
    //! @return Price in micro-USD, or 0 if not available
    CAmount GetCurrentOraclePriceMicroUSD();

    //! Get oracle price for a specific block height
    //! @param nHeight Block height to query
    //! @return Price in micro-USD (1,000,000 = $1.00), or 0 if not available
    CAmount GetOraclePriceForHeight(int nHeight);

    //! Check if oracle system is ready
    bool IsOracleSystemReady();

    //! Get oracle data for specific epoch/height
    COracleBundle GetOracleBundleForHeight(int32_t block_height);

    //! Validate oracle requirements for DigiDollar transactions
    bool ValidateOracleRequirements(const CTransaction& tx, int32_t block_height);

} // namespace OracleIntegration

#endif // DIGIBYTE_ORACLE_BUNDLE_MANAGER_H
