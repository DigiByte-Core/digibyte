// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <oracle/node.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#include <chainparams.h>
#include <clientversion.h>
#include <kernel/chainparams.h>
#include <logging.h>
#include <net.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <oracle/bundle_manager.h>
#include <oracle/exchange.h>
#include <oracle/musig2_messages.h>
#include <random.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

//! Global oracle manager instance
std::unique_ptr<OracleManager> g_oracle_manager;

// CURL callback function for writing response data
[[maybe_unused]] static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response)
{
    size_t total_size = size * nmemb;
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}

/**
 * OracleNode Implementation
 */

OracleNode::OracleNode()
    : oracle_id(0), running(false), enabled(false)
{
    // Fetch exchange prices every 60 seconds (exchanges don't update faster)
    price_update_interval = 60;
    // Broadcast every 60 seconds: 1 msg/min gives 12x redundancy per testnet
    // epoch (50 blocks ~12.5 min) and 25x per mainnet epoch (100 blocks ~25 min).
    // Previous 15s interval caused 7200 novel P2P msgs/hr with 30 mainnet oracles,
    // overwhelming the rate limiter and causing cascading peer disconnections.
    broadcast_interval = 60;
    heartbeat_interval = 300;
}

OracleNode::OracleNode(uint32_t oracle_id_in, const CKey& private_key_in)
    : oracle_id(oracle_id_in), private_key(private_key_in), running(false), enabled(false)
{
    public_key = private_key.GetPubKey();
    // 60-second intervals — see default constructor comment for rationale
    price_update_interval = 60;
    broadcast_interval = 60;
    heartbeat_interval = 300;
}

OracleNode::~OracleNode()
{
    Stop();
}

bool OracleNode::Initialize(uint32_t oracle_id_in, const std::string& private_key_hex)
{
    oracle_id = oracle_id_in;

    // Parse private key
    if (!IsHex(private_key_hex)) {
        LogPrintf("Oracle: Invalid hex format for oracle %d\n", oracle_id);
        return false;
    }
    auto key_data_opt = TryParseHex<unsigned char>(private_key_hex);
    if (!key_data_opt) {
        LogPrintf("Oracle: Invalid private key format for oracle %d\n", oracle_id);
        return false;
    }

    std::vector<unsigned char> key_data = *key_data_opt;
    if (key_data.size() != 32) {
        LogPrintf("Oracle: Invalid private key length for oracle %d, got %d\n", oracle_id, key_data.size());
        return false;
    }

    private_key.Set(key_data.begin(), key_data.end(), true);
    if (!private_key.IsValid()) {
        LogPrintf("Oracle: Invalid private key for oracle %d\n", oracle_id);
        return false;
    }

    public_key = private_key.GetPubKey();

    // Validate against chainparams (skip in REGTEST for unit testing)
    if (Params().GetChainType() == ChainType::REGTEST) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Skipping chainparams validation in REGTEST mode\n");
    } else if (!ValidateOracleId()) {
        LogPrintf("Oracle: Oracle ID %d not found in chainparams\n", oracle_id);
        return false;
    }

    // Set default exchange endpoints
    exchange_endpoints = {
        "https://api.binance.com/api/v3/ticker/price?symbol=DGBUSDT",
        "https://api.coinbase.com/v2/exchange-rates?currency=DGB",
        "https://api.kraken.com/0/public/Ticker?pair=DGBUSD",
        "https://bittrex.com/api/v1.1/public/getticker?market=USD-DGB",
        "https://poloniex.com/public?command=returnTicker"
    };

    enabled.store(true);
    LogPrintf("Oracle: Initialized oracle %d with pubkey %s\n", oracle_id, HexStr(public_key));
    return true;
}

void OracleNode::Initialize(uint32_t oracle_id_in, const CKey& key, const CPubKey& pubkey)
{
    oracle_id = oracle_id_in;
    private_key = key;
    public_key = pubkey;
    enabled.store(true);
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Test-initialized oracle %d\n", oracle_id);
}

void OracleNode::SetExchangeEndpoints(const std::vector<std::string>& endpoints)
{
    exchange_endpoints = endpoints;
    LogPrintf("Oracle: Set %d exchange endpoints for oracle %d\n", endpoints.size(), oracle_id);
}

void OracleNode::Start()
{
    if (running.load()) {
        LogPrintf("Oracle: Oracle %d is already running\n", oracle_id);
        return;
    }

    if (!enabled.load()) {
        LogPrintf("Oracle: Oracle %d is disabled, cannot start\n", oracle_id);
        return;
    }

    // Validate oracle key matches chainparams
    if (!ValidateOracleKey()) {
        LogPrintf("Oracle: ERROR - Oracle key validation failed for oracle %d, cannot start\n", oracle_id);
        return;
    }

    if (!ValidatePrivateKey()) {
        LogPrintf("Oracle: Oracle %d has invalid configuration, cannot start\n", oracle_id);
        return;
    }

    running.store(true);
    start_time = GetTime();
    price_thread = std::thread(&OracleNode::PriceThreadFunc, this);
    LogPrintf("Oracle: Started oracle %d for MuSig2 bundle participation\n", oracle_id);
}

void OracleNode::Stop()
{
    const bool was_running = running.exchange(false);
    cv_stop.notify_all();
    if (price_thread.joinable()) {
        price_thread.join();
    }
    if (was_running) {
        LogPrintf("Oracle: Stopped oracle %d\n", oracle_id);
    }
}

CAmount OracleNode::GetCurrentPrice() const
{
    std::lock_guard<std::mutex> lock(mtx_price);
    // Prefer fresh exchange price, fall back to last broadcast price
    if (current_price > 0 && (GetTime() - last_update_time) < ORACLE_MAX_AGE_SECONDS) {
        return current_price;
    }
    // We know what we last broadcast — return that
    return last_broadcast_price > 0 ? last_broadcast_price : current_price;
}

int64_t OracleNode::GetLastUpdateTime() const
{
    std::lock_guard<std::mutex> lock(mtx_price);
    // Match the price source: if using broadcast price, use broadcast timestamp
    if (current_price > 0 && (GetTime() - last_update_time) < ORACLE_MAX_AGE_SECONDS) {
        return last_update_time;
    }
    return last_broadcast_timestamp > 0 ? last_broadcast_timestamp : last_update_time;
}

bool OracleNode::HasValidPrice() const
{
    std::lock_guard<std::mutex> lock(mtx_price);
    // Fresh from exchange fetch
    if (current_price > 0 && (GetTime() - last_update_time) < ORACLE_MAX_AGE_SECONDS) {
        return true;
    }
    // FIX Bug #3: last_broadcast_price must also have a staleness check.
    // Previously, this returned true indefinitely as long as last_broadcast_price > 0,
    // even if the broadcast was hours old. This caused oracles to keep broadcasting
    // stale prices when all exchanges were unreachable, leading to persistent
    // "Invalid Price" messages on the network.
    if (last_broadcast_price > 0 && last_broadcast_timestamp > 0 &&
        (GetTime() - last_broadcast_timestamp) < ORACLE_MAX_AGE_SECONDS) {
        return true;
    }
    return false;
}

bool OracleNode::HasFreshExchangePrice() const
{
    std::lock_guard<std::mutex> lock(mtx_price);
    return current_price > 0 && (GetTime() - last_update_time) < ORACLE_MAX_AGE_SECONDS;
}

CAmount OracleNode::GetFreshExchangePrice() const
{
    std::lock_guard<std::mutex> lock(mtx_price);
    if (current_price > 0 && (GetTime() - last_update_time) < ORACLE_MAX_AGE_SECONDS) {
        return current_price;
    }
    return 0;
}

COraclePriceMessage OracleNode::CreatePriceMessage(CAmount price, int64_t timestamp)
{
    COraclePriceMessage message(oracle_id, price, timestamp);

    // Set oracle public key (XOnlyPubKey)
    message.oracle_pubkey = XOnlyPubKey(public_key);

    // Set nonce for uniqueness
    message.nonce = GetRand<uint64_t>(std::numeric_limits<uint64_t>::max());

    // Sign the compact attestation hash used as off-chain MuSig2 input.
    if (!message.SignAttestation(private_key)) {
        LogPrintf("Oracle: Failed to create compact oracle attestation for oracle %d\n", oracle_id);
        return COraclePriceMessage(); // Return empty message on failure
    }

    return message;
}

COraclePriceMessage OracleNode::CreateConsensusAttestation(uint64_t consensus_price, int64_t consensus_timestamp)
{
    OracleBundleManager& bundleManager = OracleBundleManager::GetInstance();
    if (!bundleManager.ValidateConsensusProposal(consensus_price, consensus_timestamp)) {
        LogPrint(BCLog::DIGIDOLLAR,
                 "Oracle: Refusing consensus attestation for oracle %d: price=%llu timestamp=%lld is not local consensus\n",
                 oracle_id, consensus_price, consensus_timestamp);
        return COraclePriceMessage();
    }

    COraclePriceMessage message(oracle_id, consensus_price, consensus_timestamp);

    // Set oracle public key (XOnlyPubKey)
    message.oracle_pubkey = XOnlyPubKey(public_key);

    // Set nonce for uniqueness
    message.nonce = GetRand<uint64_t>(std::numeric_limits<uint64_t>::max());

    // Sign H(oracle_id, consensus_price, consensus_timestamp). These attestations
    // are off-chain inputs to MuSig2 aggregation; only v0x03 bundles are mined.
    if (!message.SignAttestation(private_key)) {
        LogPrintf("Oracle: Failed to create consensus attestation for oracle %d\n", oracle_id);
        return COraclePriceMessage();
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Created consensus attestation for oracle %d: price=%llu, timestamp=%lld\n",
             oracle_id, consensus_price, consensus_timestamp);

    return message;
}

bool OracleNode::BroadcastPriceMessage(const COraclePriceMessage& message)
{
    if (!message.IsValid()) {
        LogPrintf("Oracle: Invalid price message for oracle %d\n", oracle_id);
        return false;
    }

    // Validate Schnorr signature
    if (!message.VerifyAttestation()) {
        LogPrintf("Oracle: Invalid Schnorr signature on price message for oracle %d\n", oracle_id);
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Broadcasting price message - Oracle: %d, Price: %llu micro-USD, Time: %lld\n",
             message.oracle_id, message.price_micro_usd, message.timestamp);

    // Send to bundle manager for processing
    OracleBundleManager& bundleManager = OracleBundleManager::GetInstance();
    bundleManager.BroadcastMessage(message);

    // Update last broadcast time
    last_broadcast_time = GetTime();
    return true;
}

OracleVersionHeartbeatMsg OracleNode::CreateVersionHeartbeat()
{
    OracleVersionHeartbeatMsg message;
    message.heartbeat_version = 1;
    message.oracle_id = oracle_id;
    message.timestamp = GetTime();
    message.nonce = GetRand<uint64_t>(std::numeric_limits<uint64_t>::max());
    message.client_version = CLIENT_VERSION;
    message.p2p_protocol_version = PROTOCOL_VERSION;
    message.oracle_protocol_version = 1;
    message.musig2_context_version = ORACLE_MUSIG2_SESSION_CONTEXT_VERSION;
    message.software_version = FormatFullVersion();
    message.subversion = FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<std::string>{});

    if (!message.Sign(private_key)) {
        LogPrintf("Oracle: Failed to sign version heartbeat for oracle %d\n", oracle_id);
        return OracleVersionHeartbeatMsg{};
    }
    return message;
}

bool OracleNode::BroadcastVersionHeartbeat()
{
    if (!private_key.IsValid() || !public_key.IsValid()) {
        LogPrintf("Oracle: Cannot broadcast version heartbeat for oracle %d: invalid key state\n", oracle_id);
        return false;
    }

    OracleVersionHeartbeatMsg heartbeat = CreateVersionHeartbeat();
    if (!heartbeat.IsValid()) {
        LogPrintf("Oracle: Refusing invalid version heartbeat for oracle %d\n", oracle_id);
        return false;
    }

    OracleBundleManager& bundleManager = OracleBundleManager::GetInstance();
    if (!bundleManager.BroadcastVersionHeartbeat(heartbeat)) {
        LogPrintf("Oracle: Failed to broadcast version heartbeat for oracle %d\n", oracle_id);
        return false;
    }

    last_heartbeat_time = heartbeat.timestamp;
    LogPrint(BCLog::DIGIDOLLAR,
             "Oracle: Broadcast version heartbeat oracle=%u client=%d oracle_protocol=%u musig2_context=%u version=%s\n",
             oracle_id, heartbeat.client_version, heartbeat.oracle_protocol_version,
             heartbeat.musig2_context_version, heartbeat.software_version);
    return true;
}

void OracleNode::InjectTestPriceState(CAmount current_price_in, int64_t last_update_time_in,
                                      CAmount last_broadcast_price_in, int64_t last_broadcast_timestamp_in)
{
    std::lock_guard<std::mutex> lock(mtx_price);
    current_price = current_price_in;
    last_update_time = last_update_time_in;
    last_broadcast_price = last_broadcast_price_in;
    last_broadcast_timestamp = last_broadcast_timestamp_in;
}

void OracleNode::PriceThreadFunc()
{
    LogPrintf("Oracle: Price thread started for oracle %d\n", oracle_id);

    while (running.load()) {
        try {
            if (enabled.load()) {
                // Heartbeats are independent of price health. They let operators
                // see who is online and what oracle protocol version they run,
                // even while exchange fetches or price consensus are stalled.
                if ((GetTime() - last_heartbeat_time) >= heartbeat_interval) {
                    BroadcastVersionHeartbeat();
                }

                // Fetch and update price
                FetchAndUpdatePrice();

                // Broadcast if needed
                if (ShouldBroadcast()) {
                    BroadcastCurrentPrice();
                }
            }

            std::unique_lock<std::mutex> lock(mtx_stop);
            cv_stop.wait_for(lock, std::chrono::seconds(price_update_interval), [this] {
                return !running.load();
            });
        }
        catch (const std::exception& e) {
            LogPrintf("Oracle: Exception in price thread for oracle %d: %s\n", oracle_id, e.what());
            std::unique_lock<std::mutex> lock(mtx_stop);
            cv_stop.wait_for(lock, std::chrono::seconds(60), [this] {
                return !running.load();
            });
        }
    }

    LogPrintf("Oracle: Price thread stopped for oracle %d\n", oracle_id);
}

void OracleNode::FetchAndUpdatePrice()
{
    CAmount median_price = FetchMedianPrice();

    if (median_price > 0) {
        std::lock_guard<std::mutex> lock(mtx_price);
        current_price = median_price;
        last_update_time = GetTime();
        consecutive_fetch_failures = 0;

        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Updated price for oracle %d: %d micro-USD\n", oracle_id, median_price);
    } else {
        std::lock_guard<std::mutex> lock(mtx_price);
        consecutive_fetch_failures++;

        // Log escalating warnings
        if (consecutive_fetch_failures == 5) {
            LogPrintf("Oracle: WARNING - 5 consecutive price fetch failures for oracle %d. "
                      "Check exchange API connectivity.\n", oracle_id);
        } else if (consecutive_fetch_failures == 15) {
            LogPrintf("Oracle: ALERT - 15 consecutive price fetch failures for oracle %d. "
                      "Oracle may be broadcasting stale prices.\n", oracle_id);
        } else if (consecutive_fetch_failures % 30 == 0) {
            LogPrintf("Oracle: CRITICAL - %d consecutive price fetch failures for oracle %d. "
                      "Exchange connectivity appears permanently broken.\n",
                      consecutive_fetch_failures, oracle_id);
        } else {
            LogPrintf("Oracle: Failed to fetch valid price for oracle %d (failure #%d)\n",
                      oracle_id, consecutive_fetch_failures);
        }
    }
}

CAmount OracleNode::FetchMedianPrice()
{
    // Use the real MultiExchangeAggregator from exchange.cpp
    // This fetches from the active exchange set and returns median with outlier filtering.
    ExchangeAPI::MultiExchangeAggregator aggregator;
    aggregator.SetMinRequiredSources(3);  // Need at least 3 exchanges
    aggregator.SetOutlierThreshold(0.10); // 10% deviation threshold
    aggregator.SetInterruptCallback([this] {
        return !running.load();
    });

    CAmount price = aggregator.FetchAggregatePrice();

    if (price > 0) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Fetched aggregate price from exchanges: %lld micro-USD ($%.6f)\n",
                 price, static_cast<double>(price) / 1000000.0);
    } else {
        LogPrintf("Oracle: Failed to fetch aggregate price from exchanges\n");
    }

    return price;
}

void OracleNode::BroadcastCurrentPrice()
{
    CAmount price = GetFreshExchangePrice();
    int64_t timestamp = GetTime();

    if (price > 0) {
        // Check if consensus has formed from pending P2P messages.
        // If so, broadcast a consensus attestation over those values so
        // the MuSig2 signing round can aggregate the final v0x03 bundle.
        OracleBundleManager& bm = OracleBundleManager::GetInstance();
        uint64_t consensus_price = 0;
        int64_t consensus_timestamp = 0;

        if (bm.GetMinOracleCount() > 1 && bm.ComputeConsensusValues(consensus_price, consensus_timestamp)) {
            // DEADLOCK PREVENTION: If the consensus timestamp is stale (>5 minutes
            // old), the consensus round is frozen. All oracles are creating
            // attestations with the same (price, timestamp) tuple, producing
            // identical hashes that get rejected by the duplicate filter.
            // Fall through to individual price broadcast with a fresh timestamp
            // to break the cycle and allow a new consensus round to form.
            int64_t consensus_age = timestamp - consensus_timestamp;
            if (consensus_age > 300) {
                LogPrintf("Oracle: Consensus timestamp is %lld seconds stale, broadcasting individual price to break deadlock\n", consensus_age);
                // Clear stale state so fresh messages can form a new consensus
                bm.ClearPendingMessages();
            } else {
                // Consensus exists and is fresh — create attestation over consensus values
                COraclePriceMessage attestation = CreateConsensusAttestation(consensus_price, consensus_timestamp);
                if (!attestation.schnorr_sig.empty()) {
                    // Submit as consensus attestation (for block construction)
                    bm.AddConsensusAttestation(attestation);

                    // Also broadcast via P2P so other nodes receive our attestation
                    if (BroadcastPriceMessage(attestation)) {
                        std::lock_guard<std::mutex> lock(mtx_price);
                        last_broadcast_price = price;
                        last_broadcast_timestamp = timestamp;
                    }
                    return;
                }
            }
        }

        // No fresh consensus yet — broadcast individual price input for the next round.
        COraclePriceMessage message = CreatePriceMessage(price, timestamp);
        if (BroadcastPriceMessage(message)) {
            std::lock_guard<std::mutex> lock(mtx_price);
            last_broadcast_price = price;
            last_broadcast_timestamp = timestamp;
        }
    }
}

bool OracleNode::ShouldBroadcast() const
{
    int64_t now = GetTime();
    return (now - last_broadcast_time) >= broadcast_interval && HasFreshExchangePrice();
}

bool OracleNode::ValidateOracleId() const
{
    // Skip chainparams validation in REGTEST for unit testing
    if (Params().GetChainType() == ChainType::REGTEST) {
        return true;
    }

    const CChainParams& params = Params();
    const OracleNodeInfo* oracle_config = params.GetOracleNode(oracle_id);

    if (!oracle_config) {
        return false;
    }

    // Verify public key matches chainparams
    return oracle_config->pubkey == public_key;
}

bool OracleNode::ValidatePrivateKey() const
{
    return private_key.IsValid() && public_key.IsValid() && ValidateOracleId();
}

CKey OracleNode::GetOraclePrivateKey()
{
    // Return the private key set during Initialize() or via AddOracleNode()
    if (private_key.IsValid()) {
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Using oracle private key from initialization\n");
        return private_key;
    }

    LogPrintf("Oracle: ERROR - No valid oracle private key available\n");
    return CKey();
}

XOnlyPubKey OracleNode::GetOraclePublicKey()
{
    CKey privkey = GetOraclePrivateKey();

    if (!privkey.IsValid()) {
        LogPrintf("Oracle: ERROR - Cannot derive public key from invalid private key\n");
        return XOnlyPubKey();
    }

    // Get compressed public key
    CPubKey pubkey = privkey.GetPubKey();

    // Convert to XOnlyPubKey for Schnorr signatures
    XOnlyPubKey xonly(pubkey);

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Public key: %s\n", HexStr(xonly));

    return xonly;
}

bool OracleNode::ValidateOracleKey()
{
    // Get oracle public key
    XOnlyPubKey oracle_xonly_pubkey = GetOraclePublicKey();

    if (!oracle_xonly_pubkey.IsFullyValid()) {
        LogPrintf("Oracle: ERROR - Oracle public key is not valid\n");
        return false;
    }

    // Get expected public key from consensus parameters
    const CChainParams& params = Params();
    const OracleNodeInfo* oracle_config = params.GetOracleNode(oracle_id);

    if (!oracle_config) {
        LogPrintf("Oracle: ERROR - No oracle configuration found for oracle %d\n", oracle_id);
        return false;
    }

    // Get the expected public key from chainparams
    CPubKey expected_pubkey = oracle_config->pubkey;

    if (!expected_pubkey.IsValid()) {
        LogPrintf("Oracle: ERROR - Expected public key in chainparams is not valid\n");
        return false;
    }

    // Convert expected pubkey to XOnlyPubKey for comparison
    XOnlyPubKey expected_xonly(expected_pubkey);

    // Convert both to hex for comparison
    std::string our_pubkey_hex = HexStr(oracle_xonly_pubkey);
    std::string expected_pubkey_hex = HexStr(expected_xonly);

    // Check if our public key matches the expected one
    if (our_pubkey_hex != expected_pubkey_hex) {
        LogPrintf("Oracle: ERROR - Oracle public key mismatch\n");
        LogPrintf("Oracle: Our key:      %s\n", our_pubkey_hex);
        LogPrintf("Oracle: Expected key: %s\n", expected_pubkey_hex);
        return false;
    }

    LogPrintf("Oracle: Key validation successful - authorized for oracle operation (oracle_id=%d)\n", oracle_id);
    return true;
}

/**
 * ExchangePriceFetcher Implementation
 */

ExchangePriceFetcher::ExchangePriceFetcher()
{
    // Initialize CURL globally if not already done
    static bool curl_initialized = false;
    if (!curl_initialized) {
#ifdef HAVE_CURL
        curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
        curl_initialized = true;
    }
}

ExchangePriceFetcher::ExchangePriceFetcher(const std::vector<std::string>& endpoints)
    : exchange_endpoints(endpoints)
{
    // Initialize CURL globally if not already done
    static bool curl_initialized = false;
    if (!curl_initialized) {
#ifdef HAVE_CURL
        curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
        curl_initialized = true;
    }
}

void ExchangePriceFetcher::SetExchangeEndpoints(const std::vector<std::string>& endpoints)
{
    exchange_endpoints = endpoints;
}

std::vector<ExchangePriceFetcher::ExchangePrice> ExchangePriceFetcher::FetchAllPrices()
{
    std::vector<ExchangePrice> prices;

    // Legacy test fetcher path; the live oracle daemon uses
    // MultiExchangeAggregator::FetchAggregatePrice().
    int64_t timestamp = GetTime();

    // Mock exchange prices with slight variation
    prices.emplace_back("Binance", 5000 + GetRand(200) - 100, timestamp);    // $0.05 ± $0.001
    prices.emplace_back("Coinbase", 4950 + GetRand(200) - 100, timestamp);   // $0.0495 ± $0.001
    prices.emplace_back("Kraken", 5050 + GetRand(200) - 100, timestamp);     // $0.0505 ± $0.001
    prices.emplace_back("Bittrex", 5025 + GetRand(200) - 100, timestamp);    // $0.05025 ± $0.001
    prices.emplace_back("Poloniex", 4975 + GetRand(200) - 100, timestamp);   // $0.04975 ± $0.001

    return FilterValidPrices(prices);
}

CAmount ExchangePriceFetcher::GetMedianPrice()
{
    std::vector<ExchangePrice> prices = FetchAllPrices();
    return GetMedianPrice(prices);
}

CAmount ExchangePriceFetcher::GetMedianPrice(const std::vector<ExchangePrice>& prices)
{
    if (prices.empty()) {
        return 0;
    }

    // Extract price values and sort
    std::vector<CAmount> price_values;
    for (const auto& price : prices) {
        if (price.valid && IsValidPrice(price.price)) {
            price_values.push_back(price.price);
        }
    }

    if (price_values.empty()) {
        return 0;
    }

    std::sort(price_values.begin(), price_values.end());

    // Calculate median
    size_t size = price_values.size();
    if (size % 2 == 0) {
        // Even number - average of middle two
        return (price_values[size/2 - 1] + price_values[size/2]) / 2;
    } else {
        // Odd number - middle element
        return price_values[size/2];
    }
}

// Individual exchange fetchers - Mock implementations for now
ExchangePriceFetcher::ExchangePrice ExchangePriceFetcher::FetchFromBinance()
{
    return ExchangePrice("Binance", 5000, GetTime());
}

ExchangePriceFetcher::ExchangePrice ExchangePriceFetcher::FetchFromCoinbase()
{
    return ExchangePrice("Coinbase", 4950, GetTime());
}

ExchangePriceFetcher::ExchangePrice ExchangePriceFetcher::FetchFromKraken()
{
    return ExchangePrice("Kraken", 5050, GetTime());
}

ExchangePriceFetcher::ExchangePrice ExchangePriceFetcher::FetchFromBittrex()
{
    return ExchangePrice("Bittrex", 5025, GetTime());
}

ExchangePriceFetcher::ExchangePrice ExchangePriceFetcher::FetchFromPoloniex()
{
    return ExchangePrice("Poloniex", 4975, GetTime());
}

std::string ExchangePriceFetcher::HttpRequest(const std::string& url)
{
    // Mock HTTP request - in real implementation would use CURL
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Mock HTTP request to %s\n", url);
    return "{\"price\":\"0.05\"}"; // Mock JSON response
}

CAmount ExchangePriceFetcher::ParseBinancePrice(const std::string& response)
{
    // Mock JSON parsing - return 50,000 micro-USD = $0.05
    return 50000;
}

CAmount ExchangePriceFetcher::ParseCoinbasePrice(const std::string& response)
{
    // Mock JSON parsing - return 49,500 micro-USD = $0.0495
    return 49500;
}

CAmount ExchangePriceFetcher::ParseKrakenPrice(const std::string& response)
{
    // Mock JSON parsing - return 50,500 micro-USD = $0.0505
    return 50500;
}

CAmount ExchangePriceFetcher::ParseBittrexPrice(const std::string& response)
{
    // Mock JSON parsing - return 50,250 micro-USD = $0.05025
    return 50250;
}

CAmount ExchangePriceFetcher::ParsePoloniexPrice(const std::string& response)
{
    // Mock JSON parsing - return 49,750 micro-USD = $0.04975
    return 49750;
}

bool ExchangePriceFetcher::IsValidPrice(CAmount price) const
{
    // Price should be positive and reasonable (between $0.0001 and $10 per DGB)
    // micro-USD: 1,000,000 = $1.00
    return price > 100 && price < 10000000; // 100 micro-USD ($0.0001) to 10,000,000 micro-USD ($10)
}

std::vector<ExchangePriceFetcher::ExchangePrice> ExchangePriceFetcher::FilterValidPrices(
    const std::vector<ExchangePrice>& prices) const
{
    std::vector<ExchangePrice> filtered;
    for (const auto& price : prices) {
        if (price.valid && IsValidPrice(price.price)) {
            filtered.push_back(price);
        }
    }
    return filtered;
}

/**
 * OracleManager Implementation
 */

OracleManager::OracleManager()
{
}

OracleManager::~OracleManager()
{
    Shutdown();
}

bool OracleManager::Initialize()
{
    if (initialized) {
        return true;
    }

    LogPrintf("Oracle: Initializing Oracle Manager\n");
    initialized = true;
    return true;
}

void OracleManager::Shutdown()
{
    if (!initialized) {
        return;
    }

    LogPrintf("Oracle: Shutting down Oracle Manager\n");
    StopAll();

    std::lock_guard<std::mutex> lock(mtx_manager);
    oracle_nodes.clear();
    initialized = false;
}

bool OracleManager::AddOracleNode(uint32_t oracle_id, const std::string& private_key_hex)
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    // Check if oracle already exists
    for (const auto& node : oracle_nodes) {
        if (node->GetOracleId() == oracle_id) {
            LogPrintf("Oracle: Oracle %d already exists\n", oracle_id);
            return false;
        }
    }

    // Create new oracle node
    auto oracle_node = std::make_unique<OracleNode>();
    if (!oracle_node->Initialize(oracle_id, private_key_hex)) {
        LogPrintf("Oracle: Failed to initialize oracle %d\n", oracle_id);
        return false;
    }

    oracle_nodes.push_back(std::move(oracle_node));
    LogPrintf("Oracle: Added oracle %d to manager\n", oracle_id);
    return true;
}

bool OracleManager::RemoveOracleNode(uint32_t oracle_id)
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    auto it = std::find_if(oracle_nodes.begin(), oracle_nodes.end(),
        [oracle_id](const std::unique_ptr<OracleNode>& node) {
            return node->GetOracleId() == oracle_id;
        });

    if (it != oracle_nodes.end()) {
        (*it)->Stop();
        oracle_nodes.erase(it);
        LogPrintf("Oracle: Removed oracle %d from manager\n", oracle_id);
        return true;
    }

    return false;
}

OracleNode* OracleManager::GetOracleNode(uint32_t oracle_id)
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    auto it = std::find_if(oracle_nodes.begin(), oracle_nodes.end(),
        [oracle_id](const std::unique_ptr<OracleNode>& node) {
            return node->GetOracleId() == oracle_id;
        });

    return (it != oracle_nodes.end()) ? it->get() : nullptr;
}

void OracleManager::StartAll()
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    for (auto& node : oracle_nodes) {
        node->Start();
    }
    LogPrintf("Oracle: Started all oracle nodes (%d total)\n", oracle_nodes.size());
}

void OracleManager::StopAll()
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    for (auto& node : oracle_nodes) {
        node->Stop();
    }
    LogPrintf("Oracle: Stopped all oracle nodes\n");
}

void OracleManager::EnableOracle(uint32_t oracle_id, bool enable)
{
    OracleNode* node = GetOracleNode(oracle_id);
    if (node) {
        node->SetEnabled(enable);
        LogPrintf("Oracle: %s oracle %d\n", enable ? "Enabled" : "Disabled", oracle_id);
    }
}

size_t OracleManager::GetActiveOracleCount() const
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    size_t count = 0;
    for (const auto& node : oracle_nodes) {
        if (node->IsRunning() && node->IsEnabled()) {
            count++;
        }
    }
    return count;
}

std::vector<uint32_t> OracleManager::GetActiveOracleIds() const
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    std::vector<uint32_t> active_ids;
    for (const auto& node : oracle_nodes) {
        if (node->IsRunning() && node->IsEnabled()) {
            active_ids.push_back(node->GetOracleId());
        }
    }
    return active_ids;
}

bool OracleManager::IsOracleRunning(uint32_t oracle_id) const
{
    std::lock_guard<std::mutex> lock(mtx_manager);

    auto it = std::find_if(oracle_nodes.begin(), oracle_nodes.end(),
        [oracle_id](const std::unique_ptr<OracleNode>& node) {
            return node->GetOracleId() == oracle_id;
        });

    return (it != oracle_nodes.end()) && (*it)->IsRunning();
}

OracleManager& OracleManager::GetInstance()
{
    if (!g_oracle_manager) {
        g_oracle_manager = std::make_unique<OracleManager>();
    }
    return *g_oracle_manager;
}

void OracleManager::StartOracleService()
{
    OracleManager& manager = GetInstance();
    manager.Initialize();

    // Do NOT auto-start with hardcoded keys.
    // Oracle operators must use 'startoracle <id> <privkey>' or 'createoraclekey' + 'startoracle'.
    // Each operator participates in MuSig2 with its own configured key.
    LogPrintf("Oracle: Oracle service initialized. Use 'startoracle <id> <privkey>' to start an oracle.\n");
}

void OracleManager::StopOracleService()
{
    if (g_oracle_manager) {
        g_oracle_manager->Shutdown();
        g_oracle_manager.reset();
        LogPrintf("Oracle: Oracle service stopped\n");
    }
}
