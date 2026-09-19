// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_ORACLE_NODE_H
#define DIGIBYTE_ORACLE_NODE_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <consensus/amount.h>
#include <key.h>
#include <primitives/oracle.h>
#include <protocol.h>
#include <pubkey.h>

/**
 * Oracle Node Daemon
 * Handles price fetching, signing, and broadcasting for oracle nodes
 */
class OracleNode
{
private:
    uint32_t oracle_id;
    CKey private_key;
    CPubKey public_key;
    std::vector<std::string> exchange_endpoints;
    std::thread price_thread;
    std::atomic<bool> running;
    std::atomic<bool> enabled;
    mutable std::mutex mtx_price;
    std::mutex mtx_stop;
    std::condition_variable cv_stop;

    // Current price data (from exchange fetch)
    CAmount current_price{0};
    int64_t last_update_time{0};
    int64_t last_broadcast_time{0};
    int64_t last_heartbeat_time{0};

    // Last successfully broadcast price (never expires — this is what we reported)
    CAmount last_broadcast_price{0};
    int64_t last_broadcast_timestamp{0};
    int consecutive_fetch_failures{0};  //!< Tracks consecutive exchange fetch failures for alerting
    int64_t start_time{0};

    //! Test-only: when set, the price thread runs but fetches no prices.
    std::atomic<bool> skip_exchange_fetch{false};

    // Configuration
    int price_update_interval{30};  // seconds
    int broadcast_interval{60};     // 60 seconds: 1 broadcast/min gives 12x-25x redundancy per epoch
    int heartbeat_interval{300};    // seconds: version/status telemetry, independent of price health

public:
    //! Constructor
    OracleNode();
    OracleNode(uint32_t oracle_id_in, const CKey& private_key_in);

    //! Destructor
    ~OracleNode();

    //! Configuration
    bool Initialize(uint32_t oracle_id_in, const std::string& private_key_hex);
    /** Test-only: Initialize with CKey + CPubKey directly (no hex parsing) */
    void Initialize(uint32_t oracle_id_in, const CKey& key, const CPubKey& pubkey);
    void SetExchangeEndpoints(const std::vector<std::string>& endpoints);
    void SetUpdateInterval(int seconds) { price_update_interval = seconds; }
    void SetBroadcastInterval(int seconds) { broadcast_interval = seconds; }
    int GetBroadcastInterval() const { return broadcast_interval; }
    void SetHeartbeatInterval(int seconds) { heartbeat_interval = seconds; }
    int GetHeartbeatInterval() const { return heartbeat_interval; }

    //! Control functions
    void Start();
    void Stop();
    bool IsRunning() const { return running.load(); }
    bool IsEnabled() const { return enabled.load(); }
    void SetEnabled(bool enable) { enabled.store(enable); }

    //! Price functions
    CAmount GetCurrentPrice() const;
    int64_t GetLastUpdateTime() const;
    bool HasValidPrice() const;

    //! Oracle functions
    uint32_t GetOracleId() const { return oracle_id; }
    CPubKey GetPublicKey() const { return public_key; }
    int64_t GetLastBroadcastTime() const { return last_broadcast_time; }
    int64_t GetLastHeartbeatTime() const { return last_heartbeat_time; }
    int64_t GetStartTime() const { return start_time; }

    //! Key management
    /**
     * Get oracle private key for MuSig2 participation.
     * @return CKey private key for signing
     */
    CKey GetOraclePrivateKey();

    /**
     * Get oracle public key (derived from private key)
     * @return XOnlyPubKey public key for Schnorr signatures
     */
    XOnlyPubKey GetOraclePublicKey();

    /**
     * Validate oracle key matches consensus parameters
     * Verifies that the oracle's public key is authorized in chainparams
     * @return true if key is authorized for oracle operation
     */
    bool ValidateOracleKey();

    //! Message creation and broadcasting
    COraclePriceMessage CreatePriceMessage(CAmount price, int64_t timestamp);

    /**
     * Create a signed consensus-value message used while coordinating MuSig2.
     * @param consensus_price The consensus price all oracles agreed upon
     * @param consensus_timestamp The consensus timestamp (median of individual timestamps)
     * @return Signed message with consensus values, or empty message on failure
     */
    COraclePriceMessage CreateConsensusAttestation(uint64_t consensus_price, int64_t consensus_timestamp);

    bool BroadcastPriceMessage(const COraclePriceMessage& message);
    OracleVersionHeartbeatMsg CreateVersionHeartbeat();
    bool BroadcastVersionHeartbeat();

    /** Test-only: inject price freshness state without running the price thread. */
    void InjectTestPriceState(CAmount current_price_in, int64_t last_update_time_in,
                              CAmount last_broadcast_price_in, int64_t last_broadcast_timestamp_in);
    /**
     * Test-only: run the price thread without asking any exchange for a price.
     *
     * The thread is real: it starts, sends heartbeats and stops the same way.
     * It just skips the part that would reach out over the network, which a
     * test must never do. Call this before Start().
     */
    void SetSkipExchangeFetchForTesting(bool skip) { skip_exchange_fetch.store(skip); }
    /** Test-only: expose broadcast-gating decision for staleness regressions. */
    bool ShouldBroadcastForTesting() const { return ShouldBroadcast(); }

private:
    //! Main thread function
    void PriceThreadFunc();

    //! Price fetching
    void FetchAndUpdatePrice();
    CAmount FetchMedianPrice();
    bool HasFreshExchangePrice() const;
    CAmount GetFreshExchangePrice() const;

    //! Price broadcasting
    void BroadcastCurrentPrice();
    bool ShouldBroadcast() const;

    //! Validation
    bool ValidateOracleId() const;
    bool ValidatePrivateKey() const;
};

/**
 * Exchange Price Fetcher
 * Fetches prices from multiple exchanges and calculates median
 */
class ExchangePriceFetcher
{
public:
    struct ExchangePrice {
        std::string exchange;
        CAmount price;
        int64_t timestamp;
        bool valid;

        ExchangePrice() : price(0), timestamp(0), valid(false) {}
        ExchangePrice(const std::string& exchange_in, CAmount price_in, int64_t timestamp_in)
            : exchange(exchange_in), price(price_in), timestamp(timestamp_in), valid(true) {}
    };

private:
    std::vector<std::string> exchange_endpoints;
    int timeout_seconds{10};

public:
    //! Constructor
    ExchangePriceFetcher();
    explicit ExchangePriceFetcher(const std::vector<std::string>& endpoints);

    //! Configuration
    void SetExchangeEndpoints(const std::vector<std::string>& endpoints);
    void SetTimeout(int seconds) { timeout_seconds = seconds; }

    //! Price fetching
    std::vector<ExchangePrice> FetchAllPrices();
    CAmount GetMedianPrice();
    CAmount GetMedianPrice(const std::vector<ExchangePrice>& prices);

    //! Individual exchange fetchers
    ExchangePrice FetchFromBinance();
    ExchangePrice FetchFromCoinbase();
    ExchangePrice FetchFromKraken();
    ExchangePrice FetchFromBittrex();
    ExchangePrice FetchFromPoloniex();

private:
    //! HTTP request helper
    std::string HttpRequest(const std::string& url);

    //! JSON parsing helpers
    CAmount ParseBinancePrice(const std::string& response);
    CAmount ParseCoinbasePrice(const std::string& response);
    CAmount ParseKrakenPrice(const std::string& response);
    CAmount ParseBittrexPrice(const std::string& response);
    CAmount ParsePoloniexPrice(const std::string& response);

    //! Utility functions
    bool IsValidPrice(CAmount price) const;
    std::vector<ExchangePrice> FilterValidPrices(const std::vector<ExchangePrice>& prices) const;
};

/**
 * Oracle Manager
 * Manages multiple oracle nodes and provides central control
 */
class OracleManager
{
private:
    std::vector<std::unique_ptr<OracleNode>> oracle_nodes;
    mutable std::mutex mtx_manager;
    bool initialized{false};

public:
    //! Constructor/Destructor
    OracleManager();
    ~OracleManager();

    //! Initialization
    bool Initialize();
    void Shutdown();

    //! Oracle management
    bool AddOracleNode(uint32_t oracle_id, const std::string& private_key_hex);
    bool RemoveOracleNode(uint32_t oracle_id);
    OracleNode* GetOracleNode(uint32_t oracle_id);

    //! Control functions
    void StartAll();
    void StopAll();
    void EnableOracle(uint32_t oracle_id, bool enable);

    //! Status functions
    size_t GetActiveOracleCount() const;
    std::vector<uint32_t> GetActiveOracleIds() const;
    bool IsOracleRunning(uint32_t oracle_id) const;

    //! Global functions
    static OracleManager& GetInstance();

    /**
     * Whether the one oracle manager has been built yet.
     *
     * Callers that only want to know whether an oracle service exists must not
     * ask GetInstance(), because that builds one on demand. This reads the
     * pointer under the same lock that builds and clears it.
     */
    static bool IsBuilt();
    static void StartOracleService();

    /**
     * Stop the price thread of every oracle this node runs, and leave the
     * oracle objects in place.
     *
     * Shutdown calls this early. The price threads fetch prices from exchanges
     * over the network and hand messages to the connection manager, so they
     * have to be gone before networking is torn down. The oracle objects have
     * to live longer than that, because block notifications, the remote
     * procedure call server and the peer-to-peer message handler all look
     * oracles up through this manager, and they stop later.
     *
     * This also refuses any further oracle start. The command server is still
     * answering requests at this point in shutdown, so a start can be part way
     * through; it is refused rather than left to create a thread that would
     * outlive the connection manager.
     */
    static void StopOraclePriceThreads();

    /**
     * Stop everything and destroy the oracle objects, including this manager.
     *
     * Only call this once nothing can look an oracle up any more: the remote
     * procedure call server, the peer-to-peer message handler and the
     * scheduler thread that delivers block notifications must all have
     * stopped, and every queued block notification must have run.
     */
    static void StopOracleService();
};

//! Global oracle manager instance
extern std::unique_ptr<OracleManager> g_oracle_manager;

#endif // DIGIBYTE_ORACLE_NODE_H
