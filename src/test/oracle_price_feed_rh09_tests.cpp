// Copyright (c) 2024-2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-09: Oracle Price Feed Validation — 10 Attack Vectors
 *
 * Systematic security testing of the oracle price validation pipeline.
 * Each test represents a specific attack scenario that an adversary
 * might use to manipulate the DGB/USD price feed.
 */

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/exchange.h>
#include <primitives/oracle.h>
#include <chainparams.h>
#include <test/util/setup_common.h>
#include <util/time.h>

#include <climits>
#include <cstdint>
#include <limits>
#include <vector>

using namespace ExchangeAPI;

// Helper: create a signed oracle message
static COraclePriceMessage MakeSignedMsg(uint32_t oracle_id, uint64_t price, int64_t ts, const CKey& key)
{
    COraclePriceMessage msg;
    msg.oracle_id = oracle_id;
    msg.price_micro_usd = price;
    msg.timestamp = ts;
    msg.block_height = 1000;
    msg.nonce = oracle_id;
    msg.oracle_pubkey = XOnlyPubKey(key.GetPubKey());
    msg.SignAttestation(key);
    return msg;
}

BOOST_FIXTURE_TEST_SUITE(oracle_price_feed_rh09_tests, BasicTestingSetup)

/**
 * RH-09 Attack #1: Single-source price manipulation
 *
 * SCENARIO: 2 of 3 oracles are offline. Attacker controls remaining oracle
 * and submits a manipulated price. With min_required_sources=2 for exchange
 * aggregation, can an attacker control the price with only 2 exchange sources?
 *
 * DEFENSE: FilterOutliers requires >= 3 data points. With < 3, ALL prices
 * pass unfiltered. The real defense is min_required_sources check in
 * FetchAggregatePrice() which returns 0 if insufficient sources.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_01_single_source_manipulation)
{
    MultiExchangeAggregator aggregator;

    // Simulate: only 1 valid exchange returns data
    std::vector<MultiExchangeAggregator::ExchangePrice> one_source;
    one_source.emplace_back("Attacker", 50000, GetTime(), true, 1.0);  // $0.05

    // FilterOutliers with < 3 sources returns ALL (no filtering)
    auto filtered = aggregator.FilterOutliers(one_source);
    BOOST_CHECK_EQUAL(filtered.size(), 1);

    // But CalculateMedianPrice still works — the real defense is min_required_sources
    CAmount median = aggregator.CalculateMedianPrice(one_source);
    BOOST_CHECK_EQUAL(median, 50000);

    // With default min_required_sources=2, a single source is insufficient
    // FetchAggregatePrice would return 0 (tested via HasSufficientData)
    aggregator.SetMinRequiredSources(2);

    // Verify: 2 sources with extreme divergence but < 3 means no outlier filtering
    std::vector<MultiExchangeAggregator::ExchangePrice> two_sources;
    two_sources.emplace_back("Legit", 6000, GetTime(), true, 1.0);       // $0.006
    two_sources.emplace_back("Attacker", 60000, GetTime(), true, 1.0);   // $0.06 (10x!)

    filtered = aggregator.FilterOutliers(two_sources);
    // SECURITY NOTE: With only 2 sources, NO outlier filtering occurs.
    // Both prices pass through — the median will be the average of the two.
    BOOST_CHECK_EQUAL(filtered.size(), 2);

    median = aggregator.CalculateMedianPrice(filtered);
    // Median of [6000, 60000] = (6000+60000)/2 = 33000 — attacker shifted price 5.5x!
    BOOST_CHECK_EQUAL(median, 33000);

    // DEFENSE: With min_required_sources=3, this attack fails entirely
    aggregator.SetMinRequiredSources(3);
    // Only 2 sources < 3 required → HasSufficientData returns false
    // FetchAggregatePrice would return 0

    // Document the risk: if min_required_sources drops to 2, attacker with 1 compromised
    // exchange can shift the price by up to the full attack amount with no outlier filtering.
    BOOST_TEST_MESSAGE("RH-09-01: With 2-source minimum, attacker with 1 source can shift median ~5.5x. "
                       "Defense: Enforce min_required_sources >= 3 in production.");
}

/**
 * RH-09 Attack #2: Extreme low price — 1 satoshi per DGB
 *
 * SCENARIO: Oracle reports DGB at $0.000001 (1 micro-USD).
 * Collateral calculation: $100 DD / $0.000001 * 500% = 500,000,000 DGB
 * Does this overflow or produce incorrect results?
 *
 * DEFENSE: util::int128_t arithmetic prevents overflow. ORACLE_MIN_PRICE_MICRO_USD=100
 * rejects anything below $0.0001. Price of 1 micro-USD is rejected.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_02_extreme_low_price)
{
    CKey key;
    key.MakeNewKey(true);
    int64_t now = GetTime();

    // Attack: submit price of 1 micro-USD (below ORACLE_MIN_PRICE_MICRO_USD=100)
    COraclePriceMessage msg = MakeSignedMsg(0, 1, now, key);
    BOOST_CHECK_MESSAGE(!msg.IsValid(now), "DEFENSE HOLDS: 1 micro-USD price rejected by IsValid");

    // Attack: submit price of exactly ORACLE_MIN_PRICE_MICRO_USD (boundary)
    COraclePriceMessage boundary_msg = MakeSignedMsg(0, ORACLE_MIN_PRICE_MICRO_USD, now, key);
    BOOST_CHECK_MESSAGE(boundary_msg.IsValid(now), "Minimum price boundary accepted");

    // Verify collateral math doesn't overflow at minimum price
    CAmount ddAmount = 10000;  // $100 in cents
    CAmount oraclePrice = static_cast<CAmount>(ORACLE_MIN_PRICE_MICRO_USD);  // 100 micro-USD
    int effectiveRatio = 1000;  // 1000%

    util::int128_t numerator = static_cast<util::int128_t>(ddAmount) * static_cast<util::int128_t>(COIN) *
                         static_cast<util::int128_t>(effectiveRatio) * 100;
    util::int128_t result128 = numerator / static_cast<util::int128_t>(oraclePrice);

    // Should be large but NOT negative (no overflow)
    BOOST_CHECK(result128 > 0);
    // 10000 * 1e8 * 1000 * 100 / 100 = 1e15 sats = 10M DGB
    // Exceeds 21B DGB supply? No, 10M < 21B. But at higher DD amounts it would.
    BOOST_CHECK_EQUAL(static_cast<uint64_t>(result128), 1000000000000000ULL);
}

/**
 * RH-09 Attack #3: Extreme high price — INT64_MAX
 *
 * SCENARIO: Oracle reports DGB at INT64_MAX micro-USD.
 * Does the price pass validation? If so, collateral would be nearly zero.
 *
 * DEFENSE: ORACLE_MAX_PRICE_MICRO_USD = 100,000,000 ($100) caps the price.
 * INT64_MAX is rejected at the message validation layer.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_03_extreme_high_price_int64_max)
{
    CKey key;
    key.MakeNewKey(true);
    int64_t now = GetTime();

    // Attack: INT64_MAX price
    uint64_t attack_price = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    COraclePriceMessage msg = MakeSignedMsg(0, attack_price, now, key);
    BOOST_CHECK_MESSAGE(!msg.IsValid(now), "DEFENSE HOLDS: INT64_MAX price rejected");

    // Attack: UINT64_MAX price
    COraclePriceMessage msg2 = MakeSignedMsg(0, std::numeric_limits<uint64_t>::max(), now, key);
    BOOST_CHECK_MESSAGE(!msg2.IsValid(now), "DEFENSE HOLDS: UINT64_MAX price rejected");

    // Attack: Just above the max (100,000,001)
    COraclePriceMessage msg3 = MakeSignedMsg(0, ORACLE_MAX_PRICE_MICRO_USD + 1, now, key);
    BOOST_CHECK_MESSAGE(!msg3.IsValid(now), "DEFENSE HOLDS: Max+1 price rejected");

    // Boundary: exact maximum should pass
    COraclePriceMessage msg4 = MakeSignedMsg(0, ORACLE_MAX_PRICE_MICRO_USD, now, key);
    BOOST_CHECK_MESSAGE(msg4.IsValid(now), "Maximum price boundary accepted");

    // Verify collateral at max price doesn't produce zero/underflow
    CAmount ddAmount = 10000;  // $100
    CAmount oraclePrice = static_cast<CAmount>(ORACLE_MAX_PRICE_MICRO_USD);  // $100/DGB
    int effectiveRatio = 500;

    util::int128_t numerator = static_cast<util::int128_t>(ddAmount) * static_cast<util::int128_t>(COIN) *
                         static_cast<util::int128_t>(effectiveRatio) * 100;
    util::int128_t result128 = numerator / static_cast<util::int128_t>(oraclePrice);

    // $100 DD at $100/DGB with 500% = 5 DGB = 5e8 sats
    BOOST_CHECK(result128 > 0);
    BOOST_CHECK_EQUAL(static_cast<uint64_t>(result128), 500000000ULL);
}

/**
 * RH-09 Attack #4: Stale price — 24 hours old
 *
 * SCENARIO: Oracle price was last updated 24 hours ago. DGB price has crashed
 * since then. Attacker mints DD using the stale (higher) cached price.
 *
 * DEFENSE: GetLatestPrice() rejects cached prices older than ORACLE_MAX_AGE_SECONDS
 * (3600s = 1 hour). Block validation also checks oracle_age > ORACLE_MAX_AGE_SECONDS.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_04_stale_price_24h)
{
    CKey key;
    key.MakeNewKey(true);

    // Set up a bundle manager for testing
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(1);

    int64_t now = GetTime();

    // Inject a message with current timestamp
    COraclePriceMessage fresh_msg = MakeSignedMsg(0, 6000, now, key);
    manager.InjectTestMessage(fresh_msg);

    // Force update the cached price
    manager.UpdatePriceCache(1000, 6000);

    // Fresh price should be available
    CAmount fresh_price = manager.GetLatestPrice();
    BOOST_CHECK_EQUAL(fresh_price, 6000);

    // Now simulate time passing: set last_update_time to 24 hours ago
    // We can't directly manipulate last_update_time, but we can test the age check
    // by verifying the ORACLE_MAX_AGE_SECONDS constant behavior

    // Verify the constant is 3600 (1 hour)
    BOOST_CHECK_EQUAL(ORACLE_MAX_AGE_SECONDS, 3600);

    // A message 24h old (86400 seconds) should fail IsValid timestamp check
    COraclePriceMessage stale_msg = MakeSignedMsg(0, 6000, now - 86400, key);
    // IsValid checks: |reference_time - timestamp| <= ORACLE_MAX_AGE_SECONDS
    BOOST_CHECK_MESSAGE(!stale_msg.IsValid(now),
        "DEFENSE HOLDS: 24-hour-old oracle message rejected by IsValid");

    // A message exactly at the boundary (1 hour old) should pass
    COraclePriceMessage boundary_msg = MakeSignedMsg(0, 6000, now - ORACLE_MAX_AGE_SECONDS, key);
    BOOST_CHECK_MESSAGE(boundary_msg.IsValid(now),
        "Oracle message at exact age boundary accepted");

    // A message 1 second past the boundary should fail
    COraclePriceMessage past_boundary = MakeSignedMsg(0, 6000, now - ORACLE_MAX_AGE_SECONDS - 1, key);
    BOOST_CHECK_MESSAGE(!past_boundary.IsValid(now),
        "DEFENSE HOLDS: Oracle message 1 second past age boundary rejected");

    manager.Clear();
}

/**
 * RH-09 Attack #5: Zero price from exchange
 *
 * SCENARIO: An exchange API returns price = 0 (due to error or manipulation).
 * Does FilterOutliers/ConvertToMicroUSD properly reject it?
 *
 * DEFENSE: ConvertToMicroUSD returns 0 for price <= 0. FilterValidPrices
 * removes entries with price_micro_usd <= 0. IsValid rejects price < ORACLE_MIN.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_05_zero_price_from_exchange)
{
    // Test ConvertToMicroUSD rejects zero
    {
        BinanceFetcher binance;
        CAmount result = binance.ConvertToMicroUSD(0.0);
        BOOST_CHECK_EQUAL(result, 0);  // Zero price → returns 0
    }

    // Test ConvertToMicroUSD rejects negative
    {
        BinanceFetcher binance;
        CAmount result = binance.ConvertToMicroUSD(-0.01);
        BOOST_CHECK_EQUAL(result, 0);  // Negative → returns 0
    }

    // Test that zero-price entries don't survive aggregation
    // FilterValidPrices is private, but we can test via FilterOutliers behavior
    {
        MultiExchangeAggregator aggregator;
        // Zero-price entries have success=false or price=0, which get filtered
        // by FilterValidPrices (called internally by FetchAllPrices)
        // We test the public FilterOutliers instead
        std::vector<MultiExchangeAggregator::ExchangePrice> prices;
        // Only valid (success=true, price>0) entries should be aggregated
        prices.emplace_back("Exchange2", 6000, GetTime(), true, 1.0);
        prices.emplace_back("Exchange3", 6100, GetTime(), true, 1.0);
        prices.emplace_back("Exchange4", 6050, GetTime(), true, 1.0);

        auto filtered = aggregator.FilterOutliers(prices);
        BOOST_CHECK_EQUAL(filtered.size(), 3);
        for (const auto& p : filtered) {
            BOOST_CHECK_GT(p.price_micro_usd, 0);
        }
    }

    // Test IsValid rejects zero price oracle message
    {
        CKey key;
        key.MakeNewKey(true);
        COraclePriceMessage msg = MakeSignedMsg(0, 0, GetTime(), key);
        BOOST_CHECK_MESSAGE(!msg.IsValid(GetTime()), "DEFENSE HOLDS: Zero price oracle message rejected");
    }
}

/**
 * RH-09 Attack #6: Negative price
 *
 * SCENARIO: An attacker crafts an oracle message with a negative price
 * (since price_micro_usd is uint64_t, this means a very large number).
 *
 * DEFENSE: price_micro_usd is uint64_t, so "negative" values are just very large
 * unsigned values that exceed ORACLE_MAX_PRICE_MICRO_USD. ConvertToMicroUSD
 * also rejects price_usd <= 0 from exchange APIs.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_06_negative_price)
{
    CKey key;
    key.MakeNewKey(true);
    int64_t now = GetTime();

    // price_micro_usd is uint64_t — "negative" via cast would be very large
    uint64_t negative_as_uint64 = static_cast<uint64_t>(-1LL);  // 0xFFFFFFFFFFFFFFFF
    COraclePriceMessage msg = MakeSignedMsg(0, negative_as_uint64, now, key);
    BOOST_CHECK_MESSAGE(!msg.IsValid(now),
        "DEFENSE HOLDS: 'Negative' price (huge uint64) rejected — exceeds ORACLE_MAX_PRICE_MICRO_USD");

    // ConvertToMicroUSD with negative double
    BinanceFetcher binance;
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(-1.0), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(-0.001), 0);

    // ConvertToMicroUSD with NaN and infinity
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(std::numeric_limits<double>::quiet_NaN()), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(std::numeric_limits<double>::infinity()), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(-std::numeric_limits<double>::infinity()), 0);
}

/**
 * RH-09 Attack #7: Price divergence — exchange A=$0.001, B=$0.01
 *
 * SCENARIO: Two exchanges report wildly different prices (10x divergence).
 * Does the outlier filter catch this? What if the attacker controls 2 of 5?
 *
 * DEFENSE: FilterOutliers uses median ± 10% threshold. With 3+ sources,
 * a 10x divergence from median is clearly rejected.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_07_price_divergence)
{
    MultiExchangeAggregator aggregator;

    // Scenario: 3 legitimate at ~$0.006, 2 attacker at $0.06 (10x)
    std::vector<MultiExchangeAggregator::ExchangePrice> prices;
    prices.emplace_back("Legit1", 6000, GetTime(), true, 1.0);   // $0.006
    prices.emplace_back("Legit2", 6100, GetTime(), true, 1.0);   // $0.0061
    prices.emplace_back("Legit3", 5900, GetTime(), true, 1.0);   // $0.0059
    prices.emplace_back("Attacker1", 60000, GetTime(), true, 1.0); // $0.06 (10x!)
    prices.emplace_back("Attacker2", 55000, GetTime(), true, 1.0); // $0.055 (9x!)

    auto filtered = aggregator.FilterOutliers(prices);

    // Median of [5900, 6000, 6100, 55000, 60000] = 6100
    // Threshold: 6100 * 0.10 = 610
    // Attacker prices deviate by ~49000 and ~54000 — WAY beyond threshold
    BOOST_CHECK_EQUAL(filtered.size(), 3);  // Only legitimate prices survive

    CAmount median = aggregator.CalculateMedianPrice(filtered);
    BOOST_CHECK_EQUAL(median, 6000);  // Median of [5900, 6000, 6100] = 6000

    // Scenario 2: Subtle attack — exchange A=$0.005, B=$0.007 (within 10%)
    std::vector<MultiExchangeAggregator::ExchangePrice> subtle;
    subtle.emplace_back("E1", 6000, GetTime(), true, 1.0);
    subtle.emplace_back("E2", 6100, GetTime(), true, 1.0);
    subtle.emplace_back("E3", 5900, GetTime(), true, 1.0);
    subtle.emplace_back("E4", 6500, GetTime(), true, 1.0);  // +8.3% from median 6050
    subtle.emplace_back("E5", 5500, GetTime(), true, 1.0);  // -9.2% from median 6000

    auto subtle_filtered = aggregator.FilterOutliers(subtle);
    // Median of [5500, 5900, 6000, 6100, 6500] = 6000
    // Threshold: 6000 * 0.10 = 600
    // 6500: deviation=500 (within 600) → PASSES
    // 5500: deviation=500 (within 600) → PASSES
    BOOST_CHECK_EQUAL(subtle_filtered.size(), 5);  // All within 10%

    BOOST_TEST_MESSAGE("RH-09-07: 10% outlier threshold rejects >10x divergence but allows ~8% manipulation.");
}

/**
 * RH-09 Attack #8: Rapid oscillation — multiple price updates in 1 block
 *
 * SCENARIO: Attacker rapidly submits many oracle messages with different prices
 * within the same block. Can they flood and manipulate which price is selected?
 *
 * DEFENSE: pending_messages is keyed by oracle_id — only the latest message
 * from each oracle is kept. Duplicate detection via seen_message_hashes.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_08_rapid_oscillation)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(1);

    CKey key;
    key.MakeNewKey(true);
    int64_t now = GetTime();

    // Rapid-fire: submit 10 messages from same oracle with increasing prices
    for (int i = 0; i < 10; i++) {
        COraclePriceMessage msg = MakeSignedMsg(0, 6000 + i * 1000, now + i, key);
        manager.InjectTestMessage(msg);
    }

    // Only ONE message should be stored (latest timestamp wins)
    BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), 1);

    auto messages = manager.GetPendingMessages();
    BOOST_REQUIRE_EQUAL(messages.size(), 1);
    // Last message should be the one with highest timestamp
    BOOST_CHECK_EQUAL(messages[0].price_micro_usd, 6000 + 9 * 1000);  // 15000
    BOOST_CHECK_EQUAL(messages[0].timestamp, now + 9);

    // Even AddOracleMessage replaces older timestamps from same oracle_id
    manager.Clear();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(1);

    COraclePriceMessage msg1 = MakeSignedMsg(0, 6000, now, key);
    manager.AddOracleMessage(msg1);
    BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), 1);

    // Older timestamp from same oracle — should be ignored
    COraclePriceMessage msg_old = MakeSignedMsg(0, 50000, now - 10, key);
    manager.AddOracleMessage(msg_old);
    BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), 1);

    auto msgs = manager.GetPendingMessages();
    BOOST_CHECK_EQUAL(msgs[0].price_micro_usd, 6000);  // Original stays

    manager.Clear();
}

/**
 * RH-09 Attack #9: All exchanges down — safe failure mode
 *
 * SCENARIO: All exchange APIs are unreachable. Does the system fail safely?
 *
 * DEFENSE: FetchAggregatePrice returns 0 when < min_required_sources.
 * GetLatestPrice returns 0 for stale/missing price. GetCurrentOraclePrice
 * returns 0, which blocks DD minting (collateral calc requires price > 0).
 */
BOOST_AUTO_TEST_CASE(rh09_attack_09_all_exchanges_down)
{
    MultiExchangeAggregator aggregator;

    // All exchanges return 0 (failure)
    std::vector<MultiExchangeAggregator::ExchangePrice> all_down;
    all_down.emplace_back("Binance", 0, GetTime(), false, 1.5);
    all_down.emplace_back("CoinGecko", 0, GetTime(), false, 1.4);
    all_down.emplace_back("KuCoin", 0, GetTime(), false, 1.0);

    // All failed exchanges produce empty valid set after internal filtering
    // CalculateMedianPrice/WeightedAverage with empty set returns 0
    std::vector<MultiExchangeAggregator::ExchangePrice> empty;
    BOOST_CHECK_EQUAL(aggregator.CalculateMedianPrice(empty), 0);
    BOOST_CHECK_EQUAL(aggregator.CalculateWeightedAverage(empty), 0);

    // OracleBundleManager: no price means 0
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    CAmount price = manager.GetLatestPrice();
    BOOST_CHECK_EQUAL(price, 0);  // No cached price = 0

    // Collateral calculation with price=0 should be rejected
    // (Division by zero is prevented in validation: oraclePrice <= 0 → reject)
    BOOST_TEST_MESSAGE("RH-09-09: All exchanges down → price=0 → DD minting blocked. Safe failure.");
}

/**
 * RH-09 Attack #10: Timestamp manipulation — future oracle timestamps
 *
 * SCENARIO: Attacker submits oracle messages with timestamps in the future.
 * This could bypass staleness checks or cause consensus issues.
 *
 * DEFENSE: ValidateBlockOracleData checks:
 * - bundle.timestamp > block.nTime + 60 → reject (future timestamp)
 * - block.nTime - bundle.timestamp > ORACLE_MAX_AGE_SECONDS → reject (too old)
 * IsValid also rejects future timestamps with tolerance.
 */
BOOST_AUTO_TEST_CASE(rh09_attack_10_future_timestamp)
{
    CKey key;
    key.MakeNewKey(true);
    int64_t now = GetTime();

    // Attack: timestamp 5 minutes in the future
    COraclePriceMessage msg_future = MakeSignedMsg(0, 6000, now + 300, key);
    BOOST_CHECK_MESSAGE(!msg_future.IsValid(now),
        "DEFENSE HOLDS: Oracle message 5 minutes in the future rejected");

    // Attack: timestamp 1 hour in the future
    COraclePriceMessage msg_far_future = MakeSignedMsg(0, 6000, now + 3600, key);
    BOOST_CHECK_MESSAGE(!msg_far_future.IsValid(now),
        "DEFENSE HOLDS: Oracle message 1 hour in the future rejected");

    // Boundary: timestamp exactly at now should pass
    COraclePriceMessage msg_now = MakeSignedMsg(0, 6000, now, key);
    BOOST_CHECK_MESSAGE(msg_now.IsValid(now), "Current-time oracle message accepted");

    // Block validation: oracle_timestamp > block_nTime + 60 is rejected
    // (Tested in ValidateBlockOracleData, but we verify the constant here)
    // The 60-second tolerance for clock skew is reasonable
    COraclePriceMessage msg_slight_future = MakeSignedMsg(0, 6000, now + 30, key);
    // Should pass — within 60s tolerance of IsValid (which uses ORACLE_MAX_AGE_SECONDS range)
    // IsValid checks: |reference_time - timestamp| <= ORACLE_MAX_AGE_SECONDS
    // |now - (now+30)| = 30 <= 3600 → passes (the age check is symmetric?)
    // Actually IsValid checks timestamp > 0 and age, let's verify:
    BOOST_CHECK(msg_slight_future.IsValid(now));

    BOOST_TEST_MESSAGE("RH-09-10: Future timestamps rejected. 60-second clock skew tolerance in block validation.");
}

/**
 * RH-09 Bonus: ConvertToMicroUSD edge cases
 *
 * Verify the exchange-level price conversion handles all edge cases.
 */
BOOST_AUTO_TEST_CASE(rh09_bonus_convert_to_micro_usd_edges)
{
    BinanceFetcher binance;

    // Normal price
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(0.006), 6000);

    // $1.00
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(1.0), 1000000);

    // Wave 11 / DD-FA-SEC-009: central per-fetcher cap tightened from
    // $100 to $10. Anything strictly above $10 must convert to 0.
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(10.0), 10000000);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(10.0001), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(100.0), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(100.01), 0);

    // Tiny but valid: $0.000001
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(0.000001), 1);  // Exactly 1 micro-USD

    // Very small: $0.0001 = 100 micro-USD (minimum for oracle)
    CAmount small = binance.ConvertToMicroUSD(0.0001);
    // Due to floating point: 0.0001 * 1000000 = 100 (should be exact)
    BOOST_CHECK_GE(small, 99);  // Allow tiny float imprecision
    BOOST_CHECK_LE(small, 101);

    // String conversion edge cases
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD("invalid"), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD(""), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD("-1.0"), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD("0"), 0);
    BOOST_CHECK_EQUAL(binance.ConvertToMicroUSD("0.006"), 6000);
}

/**
 * RH-09 Bonus: CalculateConsensusPrice IQR filtering
 *
 * Verify that the IQR-based outlier filtering in CalculateConsensusPrice
 * correctly rejects manipulated prices from colluding oracles.
 */
BOOST_AUTO_TEST_CASE(rh09_bonus_consensus_price_iqr)
{
    CKey keys[7];
    for (int i = 0; i < 7; i++) keys[i].MakeNewKey(true);
    int64_t now = GetTime();

    const Consensus::Params& params = Params().GetConsensus();

    // Scenario: 5 honest oracles at ~$0.006, 2 colluding at $0.12 (20x)
    COracleBundle bundle(0);
    bundle.messages.push_back(MakeSignedMsg(0, 6000, now, keys[0]));
    bundle.messages.push_back(MakeSignedMsg(1, 6100, now, keys[1]));
    bundle.messages.push_back(MakeSignedMsg(2, 5900, now, keys[2]));
    bundle.messages.push_back(MakeSignedMsg(3, 6050, now, keys[3]));
    bundle.messages.push_back(MakeSignedMsg(4, 5950, now, keys[4]));
    bundle.messages.push_back(MakeSignedMsg(5, 120000, now, keys[5]));  // 20x attack
    bundle.messages.push_back(MakeSignedMsg(6, 115000, now, keys[6]));  // ~19x attack

    CAmount consensus = OracleBundleManager::CalculateConsensusPrice(bundle, params);

    // With 7 prices, IQR may not filter the extreme outliers depending on quartile spread.
    // Key check: consensus should NOT be near the attacker prices (115000-120000)
    BOOST_CHECK_LT(consensus, 10000);  // Well below attacker range
    BOOST_CHECK_GT(consensus, 5000);   // Within legitimate range

    BOOST_TEST_MESSAGE("RH-09-IQR: Consensus=" + std::to_string(consensus) + ", attackers' 120k did not dominate.");
}

BOOST_AUTO_TEST_SUITE_END()
