// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <primitives/block.h>
#include <primitives/oracle.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <util/time.h>

#include <chrono>
#include <thread>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(oracle_bundle_timing_tests, RegTestingSetup)

namespace {
CBlock MakeBlockWithCoinbase()
{
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 0;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    return block;
}

bool HasOracleOutput(const CBlock& block)
{
    if (block.vtx.empty()) return false;
    const CTransaction& coinbase = *block.vtx[0];
    return coinbase.vout.size() > 1 && coinbase.vout.back().scriptPubKey.IsUnspendable();
}

void InjectSignedMessage(OracleBundleManager& manager, const CKey& key, uint32_t oracle_id, uint64_t price_micro_usd, int64_t timestamp)
{
    COraclePriceMessage msg(oracle_id, price_micro_usd, timestamp);
    msg.oracle_pubkey = XOnlyPubKey(key.GetPubKey());
    BOOST_REQUIRE(msg.SignAttestation(key));
    BOOST_REQUIRE(msg.VerifyAttestation());
    manager.InjectTestMessage(msg);
}

OracleBundleManager& ResetTimingManager(int min_oracle_count)
{
    MockOracleManager::GetInstance().Reset();
    MockOracleManager::GetInstance().SetEnabled(false);

    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(min_oracle_count);
    return manager;
}
} // namespace

BOOST_AUTO_TEST_CASE(bundle_immediate_when_quorum_met)
{
    OracleBundleManager& manager = ResetTimingManager(5);

    const uint64_t price = 7000;
    const int64_t ts = GetTime();

    std::vector<CKey> keys(5);
    for (CKey& key : keys) key.MakeNewKey(true);

    for (uint32_t i = 0; i < 5; ++i) {
        InjectSignedMessage(manager, keys[i], i, price, ts);
    }

    CBlock block = MakeBlockWithCoinbase();

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK(manager.AddOracleBundleToBlock(block, 1000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    // Legacy signed-price quorum is no longer a mining fallback. V1 only
    // serializes a completed MuSig2 v0x03 session.
    BOOST_CHECK(!HasOracleOutput(block));
    BOOST_CHECK_LT(elapsed.count(), 500);
}

BOOST_AUTO_TEST_CASE(bundle_does_not_wait_for_near_quorum_legacy_messages)
{
    OracleBundleManager& manager = ResetTimingManager(5);

    const uint64_t price = 7000;
    const int64_t ts = GetTime();

    std::vector<CKey> keys(5);
    for (CKey& key : keys) key.MakeNewKey(true);

    for (uint32_t i = 0; i < 4; ++i) {
        InjectSignedMessage(manager, keys[i], i, price, ts);
    }

    bool late_message_valid{false};
    std::thread late_oracle([&manager, &keys, price, ts, &late_message_valid]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        COraclePriceMessage message(4, price, ts);
        message.oracle_pubkey = XOnlyPubKey(keys[4].GetPubKey());
        late_message_valid = message.SignAttestation(keys[4]) && message.VerifyAttestation();
        if (late_message_valid) manager.InjectTestMessage(message);
    });

    CBlock block = MakeBlockWithCoinbase();

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK(manager.AddOracleBundleToBlock(block, 1000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    late_oracle.join();
    // Boost's assertion state is shared, so check the worker's result after joining.
    BOOST_REQUIRE(late_message_valid);

    // AddOracleBundleToBlock no longer waits for a near-quorum Phase 2
    // message to arrive. The async MuSig2 orchestrator owns session
    // completion; without a completed v0x03 session, the template omits
    // oracle data immediately.
    BOOST_CHECK(!HasOracleOutput(block));
    BOOST_CHECK_LT(elapsed.count(), 500);
}

BOOST_AUTO_TEST_CASE(bundle_no_timeout_wait_for_near_quorum_legacy_messages)
{
    OracleBundleManager& manager = ResetTimingManager(5);

    const uint64_t price = 7000;
    const int64_t ts = GetTime();

    std::vector<CKey> keys(4);
    for (CKey& key : keys) key.MakeNewKey(true);

    // Near quorum (4/5) in legacy messages is not enough for V1 mining and
    // should not trigger a synchronous wait.
    for (uint32_t i = 0; i < 4; ++i) {
        InjectSignedMessage(manager, keys[i], i, price, ts);
    }

    CBlock block = MakeBlockWithCoinbase();

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK(manager.AddOracleBundleToBlock(block, 1000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    BOOST_CHECK(!HasOracleOutput(block));
    BOOST_CHECK_LT(elapsed.count(), 700);
}

BOOST_AUTO_TEST_CASE(bundle_no_wait_when_far_from_quorum)
{
    OracleBundleManager& manager = ResetTimingManager(5);

    const uint64_t price = 7000;
    const int64_t ts = GetTime();

    std::vector<CKey> keys(2);
    for (CKey& key : keys) key.MakeNewKey(true);

    for (uint32_t i = 0; i < 2; ++i) {
        InjectSignedMessage(manager, keys[i], i, price, ts);
    }

    CBlock block = MakeBlockWithCoinbase();

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK(manager.AddOracleBundleToBlock(block, 1000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    BOOST_CHECK(!HasOracleOutput(block));
    BOOST_CHECK_LT(elapsed.count(), 700);
}

BOOST_AUTO_TEST_CASE(bundle_wait_does_not_block_too_long)
{
    OracleBundleManager& manager = ResetTimingManager(5);

    const uint64_t price = 7000;
    const int64_t ts = GetTime();

    std::vector<CKey> keys(4);
    for (CKey& key : keys) key.MakeNewKey(true);

    for (uint32_t i = 0; i < 4; ++i) {
        InjectSignedMessage(manager, keys[i], i, price, ts);
    }

    CBlock block = MakeBlockWithCoinbase();

    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK(manager.AddOracleBundleToBlock(block, 1000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    // Even near quorum, legacy Phase 2 messages must not block mining while
    // V1 waits for completed MuSig2 v0x03 sessions.
    BOOST_CHECK(!HasOracleOutput(block));
    BOOST_CHECK_LT(elapsed.count(), 700);
}

BOOST_AUTO_TEST_SUITE_END()
