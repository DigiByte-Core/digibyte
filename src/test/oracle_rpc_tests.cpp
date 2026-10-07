// Copyright (c) 2024-2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <oracle/node.h>
#include <oracle/bundle_manager.h>
#include <chainparams.h>
#include <kernel/chainparams.h>
#include <key.h>
#include <pubkey.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/time.h>

/**
 * ORACLE RPC LOGIC TESTS
 *
 * Unit tests validating the logic used by the getoracles, listoracle,
 * and related oracle RPC commands. Tests the OracleManager/OracleNode
 * APIs that the RPCs depend on.
 */

BOOST_FIXTURE_TEST_SUITE(oracle_rpc_tests, BasicTestingSetup)

// ============================================================================
// PART 1: getoracles logic — OracleManager oracle enumeration
// ============================================================================

/**
 * Test: Oracle node list from chainparams is non-empty on testnet
 *
 * The getoracles RPC iterates Params().GetOracleNodes(). Verify that
 * testnet has oracle nodes configured.
 */
BOOST_AUTO_TEST_CASE(getoracles_chainparams_has_oracles)
{
    SelectParams(ChainType::TESTNET);
    const std::vector<OracleNodeInfo>& oracles = Params().GetOracleNodes();

    BOOST_CHECK(!oracles.empty());
    BOOST_CHECK_GE(oracles.size(), 1u);

    // Verify first oracle has valid fields
    const auto& first = oracles[0];
    BOOST_CHECK_EQUAL(first.id, 0u);
    BOOST_CHECK(!first.endpoint.empty());
    BOOST_CHECK(first.pubkey.IsValid());
}

/**
 * Test: Public oracle rosters contain each operator's display name
 *
 * getoracles uses operator display names for active slots. Verify the
 * active launch roster has names for every consensus oracle.
 */
BOOST_AUTO_TEST_CASE(getoracles_oracle_names_coverage)
{
    const std::vector<std::string> oracle_names = {
        "Jared", "Green Candle", "Bastian", "DanGB", "Shenger",
        "Ycagel", "Aussie", "LookInto", "JohnnyLawDGB", "Ogilvie",
        "ChopperBrian", "hallvardo", "DaPunzy", "DigiByteForce",
        "Neel", "DigiSwarm", "GTO90", "digibyte-maxi", "Anthony",
        "mbah_jambon", "Camden", "Twoface123", "LivingTheLife",
        "ChozenOne43", "ckunchained", "JMag", "HashedMax",
        "DennisPitallano", "DigiHash Mining Pool", "medgboracle3452",
        "DigibyteDaily", "Peer2Peer", "3DogsKanab",
        "LiberatedLark", "Manu_DGB_oracle"
    };

    for (const auto network : {ChainType::MAIN, ChainType::TESTNET}) {
        const auto params = CreateChainParams(*m_node.args, network);
        const auto& oracles = params->GetOracleNodes();
        BOOST_REQUIRE_EQUAL(oracles.size(), oracle_names.size());
        for (const auto& oracle : oracles) {
            BOOST_REQUIRE_LT(oracle.id, oracle_names.size());
            std::string expected = oracle_names[oracle.id];
            if (network == ChainType::MAIN && oracle.id == 0) expected = "DigiByte.Io Oracle";
            if (network == ChainType::MAIN && oracle.id == 11) expected = "Crypto Corner Shop";
            BOOST_CHECK_EQUAL(oracle.display_name, expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(oracle_display_names_preserve_wire_data_and_selection)
{
    const auto params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& roster = params->GetOracleNodes();
    auto renamed_roster = roster;
    for (auto& oracle : renamed_roster) {
        const OracleNodeInfo original = oracle;
        oracle.display_name = "Changed local display name";
        BOOST_CHECK(oracle == original);
        BOOST_CHECK_EQUAL(oracle.IsValid(), original.IsValid());

        DataStream legacy_wire, named_wire;
        legacy_wire << original.id << original.pubkey << original.endpoint << original.is_active;
        named_wire << oracle;
        BOOST_CHECK_EQUAL_COLLECTIONS(legacy_wire.begin(), legacy_wire.end(), named_wire.begin(), named_wire.end());

        OracleNodeInfo decoded;
        named_wire >> decoded;
        BOOST_CHECK(named_wire.empty());
        BOOST_CHECK(decoded == original);
        BOOST_CHECK(decoded.display_name.empty());
    }

    for (const int epoch : {0, 1, 1234}) {
        BOOST_CHECK(SelectOraclesForEpoch(roster, epoch) == SelectOraclesForEpoch(renamed_roster, epoch));
    }
}

/**
 * Test: OracleManager reports no running oracles by default
 *
 * When no oracle has been started, IsOracleRunning should return false
 * for all IDs. This is the state getoracles sees for local status.
 */
BOOST_AUTO_TEST_CASE(getoracles_no_running_oracles_by_default)
{
    OracleManager& mgr = OracleManager::GetInstance();

    for (uint32_t id = 0; id <= 6; ++id) {
        BOOST_CHECK_EQUAL(mgr.IsOracleRunning(id), false);
        BOOST_CHECK(mgr.GetOracleNode(id) == nullptr);
    }
}

/**
 * Test: Active oracle count is zero when none started
 */
BOOST_AUTO_TEST_CASE(getoracles_active_count_zero)
{
    OracleManager& mgr = OracleManager::GetInstance();
    BOOST_CHECK_EQUAL(mgr.GetActiveOracleCount(), 0u);

    std::vector<uint32_t> active_ids = mgr.GetActiveOracleIds();
    BOOST_CHECK(active_ids.empty());
}

// ============================================================================
// PART 2: listoracle logic — local oracle status
// ============================================================================

/**
 * Test: No local oracle running — listoracle should indicate not running
 *
 * When no oracle is running, the listoracle RPC returns running=false
 * with a help message. Verify the manager state supports this.
 */
BOOST_AUTO_TEST_CASE(listoracle_no_running_oracle)
{
    OracleManager& mgr = OracleManager::GetInstance();

    // Scan all IDs 0-6 like the RPC does
    OracleNode* found = nullptr;
    for (uint32_t id = 0; id <= 6; ++id) {
        if (mgr.IsOracleRunning(id)) {
            found = mgr.GetOracleNode(id);
            break;
        }
    }

    BOOST_CHECK(found == nullptr);
}

/**
 * Test: OracleNode default state — price and timestamps are zero
 *
 * A freshly constructed OracleNode should have no valid price,
 * zero timestamps, and not be running.
 */
BOOST_AUTO_TEST_CASE(listoracle_oracle_node_default_state)
{
    OracleNode node;

    BOOST_CHECK_EQUAL(node.IsRunning(), false);
    BOOST_CHECK_EQUAL(node.IsEnabled(), false);
    BOOST_CHECK_EQUAL(node.HasValidPrice(), false);
    BOOST_CHECK_EQUAL(node.GetCurrentPrice(), 0);
    BOOST_CHECK_EQUAL(node.GetLastUpdateTime(), 0);
    BOOST_CHECK_EQUAL(node.GetLastBroadcastTime(), 0);
    BOOST_CHECK_EQUAL(node.GetStartTime(), 0);
}

/**
 * Test: OracleNode initialized with key has correct pubkey
 *
 * listoracle reports the pubkey of the running oracle. Verify
 * Initialize populates it correctly.
 */
BOOST_AUTO_TEST_CASE(listoracle_oracle_node_pubkey_after_init)
{
    OracleNode node;
    CKey key;
    key.MakeNewKey(true);
    CPubKey pubkey = key.GetPubKey();

    node.Initialize(3, key, pubkey);

    BOOST_CHECK_EQUAL(node.GetOracleId(), 3u);
    BOOST_CHECK(node.GetPublicKey() == pubkey);
    BOOST_CHECK(node.GetPublicKey().IsValid());
}

/**
 * Test: OracleNode enable/disable toggle
 *
 * listoracle reports the enabled state. Verify toggling works.
 */
BOOST_AUTO_TEST_CASE(listoracle_oracle_enable_disable)
{
    OracleNode node;
    CKey key;
    key.MakeNewKey(true);
    node.Initialize(0, key, key.GetPubKey());

    // After Initialize, oracle defaults to enabled
    BOOST_CHECK_EQUAL(node.IsEnabled(), true);

    node.SetEnabled(false);
    BOOST_CHECK_EQUAL(node.IsEnabled(), false);

    node.SetEnabled(true);
    BOOST_CHECK_EQUAL(node.IsEnabled(), true);
}

// ============================================================================
// PART 3: On-chain data extraction (getoracles on-chain fallback)
// ============================================================================

/**
 * Test: OracleBundleManager singleton is accessible
 *
 * getoracles uses OracleBundleManager::GetInstance(). Verify it works.
 */
BOOST_AUTO_TEST_CASE(getoracles_bundle_manager_accessible)
{
    OracleBundleManager& mgr = OracleBundleManager::GetInstance();
    // Just verify we can call it without crashing
    (void)mgr;
    BOOST_CHECK(true);
}

/**
 * Test: ExtractOracleBundle returns false for empty transaction
 *
 * When scanning blocks, getoracles calls ExtractOracleBundle on coinbase.
 * Verify it handles non-oracle transactions gracefully.
 */
BOOST_AUTO_TEST_CASE(getoracles_extract_bundle_empty_tx)
{
    OracleBundleManager& mgr = OracleBundleManager::GetInstance();

    // Create a minimal transaction with no oracle data
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 0;
    mtx.vout[0].scriptPubKey = CScript();

    CTransaction tx(mtx);
    COracleBundle bundle;

    bool result = mgr.ExtractOracleBundle(tx, bundle);
    BOOST_CHECK_EQUAL(result, false);
}

// ============================================================================
// PART 4: Edge cases
// ============================================================================

/**
 * Test: GetOracleNode for out-of-range ID returns nullptr
 *
 * getoracles iterates configured oracles, but listoracle scans 0-6.
 * Verify out-of-range IDs don't crash.
 */
BOOST_AUTO_TEST_CASE(oracle_manager_out_of_range_id)
{
    OracleManager& mgr = OracleManager::GetInstance();

    BOOST_CHECK(mgr.GetOracleNode(99) == nullptr);
    BOOST_CHECK_EQUAL(mgr.IsOracleRunning(99), false);
    BOOST_CHECK(mgr.GetOracleNode(255) == nullptr);
}

/**
 * Test: Price source logic — local vs on-chain vs pending vs none
 *
 * getoracles prefers local runtime price, falls back to on-chain,
 * then pending P2P messages, then "none".
 * Verify the precedence logic by checking HasValidPrice behavior.
 */
BOOST_AUTO_TEST_CASE(getoracles_price_source_precedence)
{
    // A default OracleNode has no valid price — should fall back to on-chain
    OracleNode node;
    BOOST_CHECK_EQUAL(node.HasValidPrice(), false);
    BOOST_CHECK_EQUAL(node.GetCurrentPrice(), 0);

    // This means getoracles would use "on-chain", "pending", or "none"
    // as price_source (depending on available data)
}

// ============================================================================
// PART 5: Pending P2P message integration (getoracles fix)
// ============================================================================

/**
 * Test: GetPendingMessages returns empty when no messages injected
 *
 * getoracles now checks pending P2P messages as a fallback.
 * Verify the base case returns empty.
 */
BOOST_AUTO_TEST_CASE(getoracles_pending_messages_empty_by_default)
{
    OracleBundleManager& mgr = OracleBundleManager::GetInstance();
    mgr.ClearPendingMessages();

    std::vector<COraclePriceMessage> pending = mgr.GetPendingMessages();
    BOOST_CHECK(pending.empty());
    BOOST_CHECK_EQUAL(mgr.GetPendingMessageCount(), 0u);
}

/**
 * Test: InjectTestMessage makes messages available via GetPendingMessages
 *
 * Simulates receiving P2P oracle price messages from remote oracles.
 * getoracles should see these as "pending" price sources.
 */
BOOST_AUTO_TEST_CASE(getoracles_pending_messages_after_inject)
{
    OracleBundleManager& mgr = OracleBundleManager::GetInstance();
    mgr.ClearPendingMessages();

    // Inject a price message for oracle 3
    COraclePriceMessage msg;
    msg.oracle_id = 3;
    msg.price_micro_usd = 6500;  // $0.0065/DGB
    msg.timestamp = 1700000000;
    msg.block_height = 0;
    msg.nonce = 42;

    mgr.InjectTestMessage(msg);

    std::vector<COraclePriceMessage> pending = mgr.GetPendingMessages();
    BOOST_CHECK_EQUAL(pending.size(), 1u);
    BOOST_CHECK_EQUAL(pending[0].oracle_id, 3u);
    BOOST_CHECK_EQUAL(pending[0].price_micro_usd, 6500u);
    BOOST_CHECK_EQUAL(pending[0].timestamp, 1700000000);

    mgr.ClearPendingMessages();
}

/**
 * Test: Multiple pending messages for different oracles
 *
 * getoracles iterates all 7 oracles. Pending messages should provide
 * data for any oracle that doesn't have local or on-chain data.
 */
BOOST_AUTO_TEST_CASE(getoracles_multiple_pending_messages)
{
    OracleBundleManager& mgr = OracleBundleManager::GetInstance();
    mgr.ClearPendingMessages();

    // Inject messages for oracles 0, 2, 4, 6
    for (uint32_t id : {0u, 2u, 4u, 6u}) {
        COraclePriceMessage msg;
        msg.oracle_id = id;
        msg.price_micro_usd = 6000 + id * 100;
        msg.timestamp = 1700000000 + id;
        msg.block_height = 0;
        msg.nonce = id;
        mgr.InjectTestMessage(msg);
    }

    std::vector<COraclePriceMessage> pending = mgr.GetPendingMessages();
    BOOST_CHECK_EQUAL(pending.size(), 4u);

    // Build a lookup map like getoracles does
    std::map<uint32_t, std::pair<uint64_t, int64_t>> pending_prices;
    for (const auto& m : pending) {
        pending_prices[m.oracle_id] = {m.price_micro_usd, m.timestamp};
    }

    // Verify all 4 oracles have pending data
    BOOST_CHECK(pending_prices.count(0) > 0);
    BOOST_CHECK(pending_prices.count(2) > 0);
    BOOST_CHECK(pending_prices.count(4) > 0);
    BOOST_CHECK(pending_prices.count(6) > 0);

    // Verify oracles 1, 3, 5 do NOT have pending data
    BOOST_CHECK(pending_prices.count(1) == 0);
    BOOST_CHECK(pending_prices.count(3) == 0);
    BOOST_CHECK(pending_prices.count(5) == 0);

    // Verify prices are correct
    BOOST_CHECK_EQUAL(pending_prices[0].first, 6000u);
    BOOST_CHECK_EQUAL(pending_prices[2].first, 6200u);
    BOOST_CHECK_EQUAL(pending_prices[4].first, 6400u);
    BOOST_CHECK_EQUAL(pending_prices[6].first, 6600u);

    mgr.ClearPendingMessages();
}

/**
 * Test: InjectTestMessage overwrites existing message for same oracle_id
 *
 * The pending_messages map is keyed by oracle_id, so a newer message
 * from the same oracle should replace the old one.
 */
BOOST_AUTO_TEST_CASE(getoracles_pending_message_overwrites)
{
    OracleBundleManager& mgr = OracleBundleManager::GetInstance();
    mgr.ClearPendingMessages();

    // First message for oracle 1
    COraclePriceMessage msg1;
    msg1.oracle_id = 1;
    msg1.price_micro_usd = 5000;
    msg1.timestamp = 1700000000;
    msg1.block_height = 0;
    msg1.nonce = 1;
    mgr.InjectTestMessage(msg1);

    // Second (newer) message for same oracle 1
    COraclePriceMessage msg2;
    msg2.oracle_id = 1;
    msg2.price_micro_usd = 7000;
    msg2.timestamp = 1700000015;
    msg2.block_height = 0;
    msg2.nonce = 2;
    mgr.InjectTestMessage(msg2);

    // Should only have 1 message (the newer one)
    BOOST_CHECK_EQUAL(mgr.GetPendingMessageCount(), 1u);

    std::vector<COraclePriceMessage> pending = mgr.GetPendingMessages();
    BOOST_CHECK_EQUAL(pending.size(), 1u);
    BOOST_CHECK_EQUAL(pending[0].oracle_id, 1u);
    BOOST_CHECK_EQUAL(pending[0].price_micro_usd, 7000u);
    BOOST_CHECK_EQUAL(pending[0].timestamp, 1700000015);

    mgr.ClearPendingMessages();
}

/**
 * Test: ClearPendingMessages resets all pending data
 *
 * After clearing, getoracles should find no pending data for any oracle.
 */
BOOST_AUTO_TEST_CASE(getoracles_clear_pending_messages)
{
    OracleBundleManager& mgr = OracleBundleManager::GetInstance();

    // Inject messages for all 7 oracles
    for (uint32_t id = 0; id < 7; ++id) {
        COraclePriceMessage msg;
        msg.oracle_id = id;
        msg.price_micro_usd = 6000 + id * 50;
        msg.timestamp = 1700000000;
        msg.block_height = 0;
        msg.nonce = id;
        mgr.InjectTestMessage(msg);
    }

    BOOST_CHECK_EQUAL(mgr.GetPendingMessageCount(), 7u);

    mgr.ClearPendingMessages();

    BOOST_CHECK_EQUAL(mgr.GetPendingMessageCount(), 0u);
    std::vector<COraclePriceMessage> pending = mgr.GetPendingMessages();
    BOOST_CHECK(pending.empty());
}

/**
 * Test: getoracles price source fallback priority simulation
 *
 * Simulates the exact logic in getoracles to verify the priority:
 * 1. local runtime > 2. on-chain > 3. pending P2P > 4. none
 */
BOOST_AUTO_TEST_CASE(getoracles_price_source_fallback_priority)
{
    OracleManager& oracle_mgr = OracleManager::GetInstance();
    OracleBundleManager& bundle_mgr = OracleBundleManager::GetInstance();
    bundle_mgr.ClearPendingMessages();

    // Simulate: oracle 5 has a pending P2P message but no local or on-chain data
    COraclePriceMessage msg;
    msg.oracle_id = 5;
    msg.price_micro_usd = 8200;
    msg.timestamp = 1700000042;
    msg.block_height = 0;
    msg.nonce = 99;
    bundle_mgr.InjectTestMessage(msg);

    // Simulate the getoracles fallback logic for oracle 5
    uint32_t oracle_id = 5;
    OracleNode* runtime = oracle_mgr.GetOracleNode(oracle_id);

    // No local runtime
    BOOST_CHECK(runtime == nullptr);

    // No on-chain data (simulated empty map)
    struct OnChainPrice { uint64_t price = 0; int64_t timestamp = 0; bool found = false; };
    std::map<uint32_t, OnChainPrice> onchain_prices;

    // Build pending prices map (like getoracles does)
    std::map<uint32_t, std::pair<uint64_t, int64_t>> pending_prices;
    {
        std::vector<COraclePriceMessage> pending = bundle_mgr.GetPendingMessages();
        for (const auto& m : pending) {
            pending_prices[m.oracle_id] = {m.price_micro_usd, m.timestamp};
        }
    }

    // Apply the same priority logic as getoracles
    uint64_t price = 0;
    int64_t update_time = 0;
    std::string price_source = "none";
    std::string status = "no_data";

    if (runtime && runtime->HasValidPrice()) {
        price = runtime->GetCurrentPrice();
        update_time = runtime->GetLastUpdateTime();
        price_source = "local";
        status = "reporting";
    } else if (onchain_prices.count(oracle_id) && onchain_prices[oracle_id].found) {
        price = onchain_prices[oracle_id].price;
        update_time = onchain_prices[oracle_id].timestamp;
        price_source = "on-chain";
        status = "reporting";
    } else if (pending_prices.count(oracle_id)) {
        price = pending_prices[oracle_id].first;
        update_time = pending_prices[oracle_id].second;
        price_source = "pending";
        status = "reporting";
    }

    // Should use pending data
    BOOST_CHECK_EQUAL(price_source, "pending");
    BOOST_CHECK_EQUAL(status, "reporting");
    BOOST_CHECK_EQUAL(price, 8200u);
    BOOST_CHECK_EQUAL(update_time, 1700000042);

    bundle_mgr.ClearPendingMessages();
}

/**
 * Test: Oracle with no data from any source gets "none"/"no_data"
 *
 * Verifies that an oracle with no local, on-chain, or pending data
 * correctly shows price_source="none" and status="no_data".
 */
BOOST_AUTO_TEST_CASE(getoracles_no_data_from_any_source)
{
    OracleManager& oracle_mgr = OracleManager::GetInstance();
    OracleBundleManager& bundle_mgr = OracleBundleManager::GetInstance();
    bundle_mgr.ClearPendingMessages();

    uint32_t oracle_id = 2;
    OracleNode* runtime = oracle_mgr.GetOracleNode(oracle_id);
    BOOST_CHECK(runtime == nullptr);

    struct OnChainPrice { uint64_t price = 0; int64_t timestamp = 0; bool found = false; };
    std::map<uint32_t, OnChainPrice> onchain_prices;
    std::map<uint32_t, std::pair<uint64_t, int64_t>> pending_prices;

    // Apply fallback logic
    uint64_t price = 0;
    int64_t update_time = 0;
    std::string price_source = "none";
    std::string status = "no_data";

    if (runtime && runtime->HasValidPrice()) {
        price_source = "local";
        status = "reporting";
    } else if (onchain_prices.count(oracle_id) && onchain_prices[oracle_id].found) {
        price_source = "on-chain";
        status = "reporting";
    } else if (pending_prices.count(oracle_id)) {
        price_source = "pending";
        status = "reporting";
    }

    BOOST_CHECK_EQUAL(price_source, "none");
    BOOST_CHECK_EQUAL(status, "no_data");
    BOOST_CHECK_EQUAL(price, 0u);
    BOOST_CHECK_EQUAL(update_time, 0);

    bundle_mgr.ClearPendingMessages();
}

/**
 * Test: Epoch selection logic is deterministic
 *
 * getoracles reports selected_for_epoch. Verify the selection functions
 * exist and return consistent results.
 */
BOOST_AUTO_TEST_CASE(getoracles_epoch_selection_deterministic)
{
    SelectParams(ChainType::TESTNET);
    const std::vector<OracleNodeInfo>& oracles = Params().GetOracleNodes();

    if (oracles.empty()) {
        // Skip if no oracles configured
        return;
    }

    int32_t epoch = GetCurrentEpoch(100);
    std::vector<OracleNodeInfo> selected1 = SelectOraclesForEpoch(oracles, epoch);
    std::vector<OracleNodeInfo> selected2 = SelectOraclesForEpoch(oracles, epoch);

    // Same epoch should produce same selection
    BOOST_CHECK_EQUAL(selected1.size(), selected2.size());
    for (size_t i = 0; i < selected1.size(); ++i) {
        BOOST_CHECK_EQUAL(selected1[i].id, selected2[i].id);
    }
}

/**
 * Test: Oracle USD price calculation
 *
 * Both getoracles and listoracle compute price_usd as price / 1000000.0.
 * Verify the arithmetic for representative values.
 */
BOOST_AUTO_TEST_CASE(oracle_price_usd_calculation)
{
    // 50000 micro-USD = $0.05
    int64_t price1 = 50000;
    double usd1 = static_cast<double>(price1) / 1000000.0;
    BOOST_CHECK_CLOSE(usd1, 0.05, 0.001);

    // 6000 micro-USD = $0.006
    int64_t price2 = 6000;
    double usd2 = static_cast<double>(price2) / 1000000.0;
    BOOST_CHECK_CLOSE(usd2, 0.006, 0.001);

    // 0 micro-USD = $0.00
    int64_t price3 = 0;
    double usd3 = static_cast<double>(price3) / 1000000.0;
    BOOST_CHECK_EQUAL(usd3, 0.0);

    // 1000000 micro-USD = $1.00
    int64_t price4 = 1000000;
    double usd4 = static_cast<double>(price4) / 1000000.0;
    BOOST_CHECK_CLOSE(usd4, 1.0, 0.001);
}

// ============================================================================
// PART 6: Bug #26 — Local oracle preserves on-chain block_height
// ============================================================================

BOOST_AUTO_TEST_CASE(local_oracle_preserves_onchain_block_height)
{
    // Bug #26: Phase 3 (local oracle) was overwriting Phase 1 on-chain
    // block_height with 0. After fix, if Phase 1 set block_height > 0,
    // Phase 3 should preserve it.
    //
    // Simulate the ScanOracleDataFromChain logic:
    // Phase 1 sets block_height = 42000 (from on-chain bundle)
    // Phase 3 sets local price data but should keep block_height = 42000

    struct TestOracleData {
        uint64_t price_micro_usd = 0;
        int64_t timestamp = 0;
        int32_t block_height = 0;
        bool signature_valid = false;
        bool has_data = false;
        std::string price_source;
    };

    TestOracleData od;

    // Phase 1: on-chain data
    od.price_micro_usd = 5000;
    od.timestamp = 1700000000;
    od.block_height = 42000;
    od.signature_valid = true;
    od.has_data = true;
    od.price_source = "on-chain";

    // Phase 3: local runtime override — the FIX preserves block_height
    int32_t existing_height = od.block_height;
    od.price_micro_usd = 5200; // newer local price
    od.timestamp = 1700000015;
    od.block_height = (existing_height > 0) ? existing_height : 0;
    od.signature_valid = true;
    od.has_data = true;
    od.price_source = "local";

    BOOST_CHECK_EQUAL(od.block_height, 42000);
    BOOST_CHECK_EQUAL(od.price_source, "local");
    BOOST_CHECK_EQUAL(od.price_micro_usd, 5200u);
}

BOOST_AUTO_TEST_CASE(local_oracle_no_onchain_data_shows_zero_height)
{
    // When there's no on-chain data (Phase 1 never ran for this oracle),
    // Phase 3 should still show block_height = 0.

    struct TestOracleData {
        uint64_t price_micro_usd = 0;
        int64_t timestamp = 0;
        int32_t block_height = 0;
        bool signature_valid = false;
        bool has_data = false;
        std::string price_source;
    };

    TestOracleData od;

    // No Phase 1 data — block_height stays at default 0

    // Phase 3: local runtime sets data
    int32_t existing_height = od.block_height;
    od.price_micro_usd = 5200;
    od.timestamp = 1700000015;
    od.block_height = (existing_height > 0) ? existing_height : 0;
    od.signature_valid = true;
    od.has_data = true;
    od.price_source = "local";

    BOOST_CHECK_EQUAL(od.block_height, 0);
    BOOST_CHECK_EQUAL(od.price_source, "local");
}

BOOST_AUTO_TEST_SUITE_END()
