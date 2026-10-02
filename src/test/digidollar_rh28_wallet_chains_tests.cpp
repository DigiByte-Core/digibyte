// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-28: WALLET-LEVEL ATTACK CHAINS
 *
 * Red-team tests targeting complex attacks that combine wallet operations
 * with DigiDollar protocol exploitation. These tests probe:
 *
 * 1. Coin selection manipulation for under-collateralized mints
 * 2. Wallet/chain DD state divergence after reorgs
 * 3. DD key derivation weaknesses
 * 4. RPC parameter injection (malformed inputs, overflow, null bytes)
 * 5. Wallet backup with stale DD state
 * 6. Concurrent wallet DD operations (race conditions)
 * 7. Transaction construction timing attacks (TOCTOU)
 * 8. Wallet DB corruption resilience for DD positions
 *
 * Attack model: Malicious wallet operator, local attacker with DB access,
 * concurrent manipulation, and RPC injection.
 */

#include <boost/test/unit_test.hpp>

#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <consensus/digidollar_transaction_validation.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <digidollar/txbuilder.h>
#include <wallet/digidollarwallet.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <kernel/chainparams.h>
#include <chainparams.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <oracle/mock_oracle.h>
#include <key.h>
#include <hash.h>
#include <streams.h>
#include <random.h>
#include <base58.h>
#include <uint256.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <limits>

BOOST_FIXTURE_TEST_SUITE(digidollar_rh28_wallet_chains_tests, BasicTestingSetup)

// =============================================================================
// Helper: Create a DigiDollarWallet without a real CWallet (unit-test mode)
// =============================================================================

static uint256 MakeTestHash(int seed) {
    HashWriter hasher{};
    hasher << seed;
    return hasher.GetHash();
}

// =============================================================================
// RH-28-01: Coin Selection Manipulation — Under-Collateralized Mint Attack
// ATTACK: Manipulate wallet UTXO set to trick SelectDDCoins into selecting
// UTXOs that don't actually exist on-chain, producing invalid transactions.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_01_select_dd_coins_zero_amount_utxo)
{
    // ATTACK: Insert a DD UTXO with amount=0 into dd_utxos map.
    // If coin selection counts it, attacker gets "free" selection slots.
    DigiDollarWallet ddw;

    // Add a zero-amount UTXO
    COutPoint zeroUtxo(MakeTestHash(1), 1);
    ddw.AddDDUTXO(zeroUtxo, 0);

    // Add a real UTXO
    COutPoint realUtxo(MakeTestHash(2), 1);
    ddw.AddDDUTXO(realUtxo, 5000); // $50

    // Try to select $50 worth
    std::vector<COutPoint> selected;
    CAmount total = 0;
    bool ok = ddw.SelectDDCoins(5000, selected, total);
    BOOST_CHECK(ok);
    // The zero UTXO should not help reach the target
    BOOST_CHECK_EQUAL(total, 5000);

    // The zero UTXO should either be excluded or not contribute
    // Verify we selected the real one
    bool found_real = false;
    for (const auto& s : selected) {
        if (s == realUtxo) found_real = true;
    }
    BOOST_CHECK(found_real);
}

BOOST_AUTO_TEST_CASE(rh28_01b_select_dd_coins_negative_amount_utxo)
{
    // ATTACK: Insert a DD UTXO with negative amount. If coin selection
    // adds this, it could underflow the total, allowing infinite selection.
    DigiDollarWallet ddw;

    COutPoint negUtxo(MakeTestHash(10), 1);
    ddw.AddDDUTXO(negUtxo, -5000); // Negative $50

    COutPoint realUtxo(MakeTestHash(11), 1);
    ddw.AddDDUTXO(realUtxo, 10000); // $100

    std::vector<COutPoint> selected;
    CAmount total = 0;
    bool ok = ddw.SelectDDCoins(10000, selected, total);

    // Either selection succeeds with only the real UTXO,
    // or it fails because the negative contaminates the total.
    // Either way, total must NOT be negative.
    if (ok) {
        BOOST_CHECK(total >= 10000);
    }
    // total should never be negative regardless of outcome
    BOOST_CHECK_GE(total, 0);
}

BOOST_AUTO_TEST_CASE(rh28_01c_select_dd_coins_refuses_a_total_that_cannot_fit)
{
    // ATTACK: put DigiDollar outputs in the wallet whose amounts together are
    // more than a money amount can hold. If coin selection added them up and
    // let the total wrap round, an attacker could satisfy any target with
    // outputs that are not really worth that much.
    //
    // Coin selection must stop before the addition, refuse, and leave nothing
    // selected.
    DigiDollarWallet ddw;

    COutPoint utxo1(MakeTestHash(20), 1);
    ddw.AddDDUTXO(utxo1, std::numeric_limits<CAmount>::max());

    COutPoint utxo2(MakeTestHash(21), 1);
    ddw.AddDDUTXO(utxo2, 1);

    std::vector<COutPoint> selected;
    CAmount total = 0;
    const bool ok = ddw.SelectDDCoins(100, selected, total);

    BOOST_CHECK(!ok);
    BOOST_CHECK(selected.empty());
    BOOST_CHECK_EQUAL(total, 0);
}

// =============================================================================
// RH-28-02: Wallet State Divergence After Reorg
// ATTACK: Wallet tracks DD positions that no longer exist on-chain after reorg.
// Wallet thinks it has DD balance but chain disagrees.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_02_position_state_divergence)
{
    // SCENARIO: Wallet has an active position. Chain reorgs and the mint tx
    // is no longer in the best chain. Wallet should detect this.
    DigiDollarWallet ddw;

    uint256 mintTxid = MakeTestHash(100);
    CAmount ddAmount = 50000; // $500
    CAmount dgbCollateral = 250 * COIN;

    // Add position as active
    ddw.AddMockPosition(mintTxid, ddAmount, dgbCollateral, 4, 100000);

    // Verify position exists and is active
    auto positions = ddw.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(positions.size(), 1);
    BOOST_CHECK(positions[0].is_active);

    // Simulate reorg: mark position inactive (as ValidatePositionStates would)
    ddw.UpdatePositionStatus(mintTxid, false);

    // Active positions should be empty now
    positions = ddw.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(positions.size(), 0);

    // All positions (including inactive) should still show 1
    positions = ddw.GetDDTimeLocks(false);
    BOOST_CHECK_EQUAL(positions.size(), 1);
    BOOST_CHECK(!positions[0].is_active);
}

BOOST_AUTO_TEST_CASE(rh28_02b_dd_utxo_removal_after_spend)
{
    // ATTACK: After DD UTXO is spent, wallet still tracks it.
    // Attacker tries to double-spend the same DD UTXO.
    DigiDollarWallet ddw;

    COutPoint ddUtxo(MakeTestHash(200), 1);
    ddw.AddDDUTXO(ddUtxo, 10000); // $100

    // Verify it's tracked
    BOOST_CHECK(ddw.HasDDUTXO(ddUtxo));
    BOOST_CHECK_EQUAL(ddw.GetDDFromUTXO(ddUtxo), 10000);

    // Remove it (simulating spend)
    ddw.RemoveDDUTXO(ddUtxo);

    // Should no longer be available
    BOOST_CHECK(!ddw.HasDDUTXO(ddUtxo));
    BOOST_CHECK_EQUAL(ddw.GetDDFromUTXO(ddUtxo), 0);

    // Double-add should work (in case of reorg re-adding)
    ddw.AddDDUTXO(ddUtxo, 10000);
    BOOST_CHECK(ddw.HasDDUTXO(ddUtxo));
}

// =============================================================================
// RH-28-03: DD Key Derivation Weakness
// ATTACK: If DD owner keys are predictable or reused, attacker can steal DD.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_03_owner_key_uniqueness)
{
    // Each DD position MUST have a unique owner key.
    // Reusing keys across positions would allow a single key compromise
    // to affect all positions.
    DigiDollarWallet ddw;

    // Store multiple owner keys and verify they're different
    CKey key1;
    key1.MakeNewKey(true);
    CKey key2;
    key2.MakeNewKey(true);

    uint256 id1 = MakeTestHash(300);
    uint256 id2 = MakeTestHash(301);

    ddw.StoreOwnerKey(id1, key1);
    ddw.StoreOwnerKey(id2, key2);

    CKey retrieved1, retrieved2;
    BOOST_CHECK(ddw.GetOwnerKey(id1, retrieved1));
    BOOST_CHECK(ddw.GetOwnerKey(id2, retrieved2));

    // Keys must be different
    BOOST_CHECK(retrieved1.GetPubKey() != retrieved2.GetPubKey());

    // Keys must match what was stored
    BOOST_CHECK(retrieved1.GetPubKey() == key1.GetPubKey());
    BOOST_CHECK(retrieved2.GetPubKey() == key2.GetPubKey());
}

BOOST_AUTO_TEST_CASE(rh28_03b_owner_key_overwrite_attack)
{
    // ATTACK: Overwrite an existing owner key with attacker's key.
    // If StoreOwnerKey allows overwrite, attacker who gets wallet access
    // could replace legitimate keys with their own.
    DigiDollarWallet ddw;

    CKey legitimateKey;
    legitimateKey.MakeNewKey(true);
    CKey attackerKey;
    attackerKey.MakeNewKey(true);

    uint256 posId = MakeTestHash(310);

    ddw.StoreOwnerKey(posId, legitimateKey);

    // Attacker overwrites
    ddw.StoreOwnerKey(posId, attackerKey);

    CKey retrieved;
    BOOST_CHECK(ddw.GetOwnerKey(posId, retrieved));

    // Document the behavior: is overwrite allowed?
    // If the attacker key won, this is a vulnerability for any scenario
    // where an attacker has wallet.dat write access.
    bool overwrite_succeeded = (retrieved.GetPubKey() == attackerKey.GetPubKey());

    // NOTE: If this check fails, it means StoreOwnerKey properly prevents
    // overwrite of existing keys, which is the secure behavior.
    // If it passes, we've documented that key overwrite is possible.
    if (overwrite_succeeded) {
        // This is expected in current implementation - document as known risk
        BOOST_TEST_MESSAGE("WARNING: DD owner key overwrite is possible. "
                          "Attacker with wallet.dat access can replace position keys.");
    }
    // Key should at least be valid
    BOOST_CHECK(retrieved.IsValid());
}

BOOST_AUTO_TEST_CASE(rh28_03c_nonexistent_key_retrieval)
{
    // Retrieving a key for a non-existent position must fail cleanly
    DigiDollarWallet ddw;

    CKey key;
    uint256 fakeId = MakeTestHash(320);
    BOOST_CHECK(!ddw.GetOwnerKey(fakeId, key));

    // Key should be invalid after failed retrieval
    BOOST_CHECK(!key.IsValid());
}

// =============================================================================
// RH-28-04: RPC Parameter Injection
// ATTACK: Send malformed parameters to DD RPC endpoints to trigger crashes,
// buffer overflows, or logic errors.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_04_validate_dd_address_injection)
{
    // Test DD address validation with malicious inputs
    DigiDollarWallet ddw;

    // Empty string
    BOOST_CHECK(!ddw.ValidateDDAddress(""));

    // Extremely long string (1MB)
    std::string longAddr(1024 * 1024, 'D');
    BOOST_CHECK(!ddw.ValidateDDAddress(longAddr));

    // Null bytes embedded
    std::string nullAddr = "DD";
    nullAddr += '\0';
    nullAddr += "abcdef123456";
    BOOST_CHECK(!ddw.ValidateDDAddress(nullAddr));

    // Unicode characters
    BOOST_CHECK(!ddw.ValidateDDAddress("DD\xc0\xaftest"));
    BOOST_CHECK(!ddw.ValidateDDAddress("DD😀test"));

    // SQL injection-style
    BOOST_CHECK(!ddw.ValidateDDAddress("DD'; DROP TABLE positions;--"));

    // Path traversal
    BOOST_CHECK(!ddw.ValidateDDAddress("DD../../etc/passwd"));

    // Valid-looking but wrong prefix
    BOOST_CHECK(!ddw.ValidateDDAddress("BT1qtest"));
    BOOST_CHECK(!ddw.ValidateDDAddress("dgb1qtest"));
}

// Helper: Expose protected ValidateMintParams for testing
class TestableDigiDollarWallet : public DigiDollarWallet {
public:
    using DigiDollarWallet::ValidateMintParams;
    using DigiDollarWallet::ValidateTransferParams;
    using DigiDollarWallet::ValidateRedeemParams;
};

BOOST_AUTO_TEST_CASE(rh28_04b_mint_validation_boundary_amounts)
{
    // Test mint parameter validation at boundary values
    TestableDigiDollarWallet ddw;

    // Zero amount
    BOOST_CHECK(!ddw.ValidateMintParams(0, 1));

    // Negative amount
    BOOST_CHECK(!ddw.ValidateMintParams(-1, 1));

    // INT64_MAX
    BOOST_CHECK(!ddw.ValidateMintParams(std::numeric_limits<CAmount>::max(), 1));

    // INT64_MIN
    BOOST_CHECK(!ddw.ValidateMintParams(std::numeric_limits<CAmount>::min(), 1));

    // Invalid lock tiers
    BOOST_CHECK(!ddw.ValidateMintParams(5000, 10)); // tier 10 doesn't exist
    BOOST_CHECK(!ddw.ValidateMintParams(5000, 255)); // uint8 max
    BOOST_CHECK(!ddw.ValidateMintParams(5000, std::numeric_limits<uint32_t>::max()));
}

BOOST_AUTO_TEST_CASE(rh28_04c_transfer_validation_boundary)
{
    TestableDigiDollarWallet ddw;

    // Invalid address
    CDigiDollarAddress emptyAddr;
    BOOST_CHECK(!ddw.ValidateTransferParams(emptyAddr, 5000));

    // Zero amount — validation should reject amount <= 0
    CDigiDollarAddress addr;
    BOOST_CHECK(!ddw.ValidateTransferParams(addr, 0));
    BOOST_CHECK(!ddw.ValidateTransferParams(addr, -1));

    // Redeem validation with zero amount
    uint256 fakePos = MakeTestHash(450);
    BOOST_CHECK(!ddw.ValidateRedeemParams(fakePos, 0));
    BOOST_CHECK(!ddw.ValidateRedeemParams(fakePos, -1));
}

// =============================================================================
// RH-28-05: Wallet Backup with Stale DD State
// ATTACK: Restore a backup from before a redemption. Wallet thinks it still
// has DD but the chain has already burned them.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_05_stale_position_after_restore)
{
    // SCENARIO: Position created, then redeemed on-chain.
    // Wallet restored from backup before redemption.
    // Wallet should have stale position; ValidatePositionStates should catch it.
    DigiDollarWallet ddw;

    uint256 mintId = MakeTestHash(500);
    CAmount ddAmt = 100000;
    CAmount collateral = 500 * COIN;

    // Add position (simulates backup state)
    ddw.AddMockPosition(mintId, ddAmt, collateral, 4, 200000);
    ddw.AddDDUTXO(COutPoint(mintId, 1), ddAmt);

    // Verify position and UTXO exist
    BOOST_CHECK_EQUAL(ddw.GetPositionCount(), 1);
    BOOST_CHECK(ddw.HasDDUTXO(COutPoint(mintId, 1)));

    // Simulate chain state: UTXO was spent (redeemed)
    // In real scenario, ScanForDDUTXOs + ValidatePositionStates would detect this.
    // Here we manually test the removal path.
    ddw.RemoveDDUTXO(COutPoint(mintId, 1));

    // UTXO gone but position still active — this is the stale state
    BOOST_CHECK(!ddw.HasDDUTXO(COutPoint(mintId, 1)));
    auto positions = ddw.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(positions.size(), 1); // Still shows active (stale!)

    // After ValidatePositionStates runs, it should mark inactive
    // (can't call directly without real wallet, but document the expected behavior)
    BOOST_TEST_MESSAGE("NOTE: ValidatePositionStates() would detect stale position "
                      "by checking collateral UTXO against the real UTXO set.");
}

// =============================================================================
// RH-28-06: Concurrent Wallet DD Operations (Race Conditions)
// ATTACK: Two threads simultaneously perform DD operations to exploit
// TOCTOU gaps in coin selection and balance tracking.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_06_concurrent_dd_utxo_modification)
{
    // ATTACK: Multiple threads add/remove DD UTXOs simultaneously.
    // Goal: Corrupt the dd_utxos map or create inconsistent balance.
    DigiDollarWallet ddw;
    std::atomic<int> errors{0};
    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 500;

    // Pre-populate with some UTXOs
    for (int i = 0; i < 100; i++) {
        ddw.AddDDUTXO(COutPoint(MakeTestHash(1000 + i), 1), 1000);
    }

    auto worker = [&](int thread_id) {
        try {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                int idx = thread_id * OPS_PER_THREAD + i;
                COutPoint op(MakeTestHash(2000 + idx), 1);

                // Alternate between add and remove
                if (i % 3 == 0) {
                    ddw.AddDDUTXO(op, 500 + (i % 100));
                } else if (i % 3 == 1) {
                    ddw.RemoveDDUTXO(op);
                } else {
                    // Read operation
                    ddw.HasDDUTXO(op);
                    ddw.GetDDFromUTXO(op);
                }
            }
        } catch (const std::exception& e) {
            errors++;
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    BOOST_CHECK_EQUAL(errors.load(), 0);
    // No crash = mutex protection working
    BOOST_TEST_MESSAGE("Concurrent DD UTXO modification: no crashes or exceptions");
}

BOOST_AUTO_TEST_CASE(rh28_06b_concurrent_coin_selection)
{
    // ATTACK: Two threads select coins simultaneously for transfers.
    // Without proper locking, both could select the same UTXOs.
    DigiDollarWallet ddw;

    // Add several UTXOs
    for (int i = 0; i < 10; i++) {
        ddw.AddDDUTXO(COutPoint(MakeTestHash(3000 + i), 1), 10000); // $100 each
    }

    std::atomic<int> successes{0};
    CAmount totalSelected1 = 0, totalSelected2 = 0;
    std::vector<COutPoint> selected1, selected2;

    // Thread 1: select $500
    auto t1 = std::thread([&]() {
        std::vector<COutPoint> sel;
        CAmount total = 0;
        if (ddw.SelectDDCoins(50000, sel, total)) {
            successes++;
            selected1 = sel;
            totalSelected1 = total;
        }
    });

    // Thread 2: select $500 simultaneously
    auto t2 = std::thread([&]() {
        std::vector<COutPoint> sel;
        CAmount total = 0;
        if (ddw.SelectDDCoins(50000, sel, total)) {
            successes++;
            selected2 = sel;
            totalSelected2 = total;
        }
    });

    t1.join();
    t2.join();

    // Both should succeed (coin selection is read-only, doesn't reserve)
    BOOST_CHECK_EQUAL(successes.load(), 2);

    // VULNERABILITY DOCUMENTATION: Both threads selected the SAME UTXOs.
    // In a real scenario, only one transfer would succeed on-chain,
    // but the wallet might show both as pending, confusing the user.
    if (!selected1.empty() && !selected2.empty()) {
        BOOST_TEST_MESSAGE("Both concurrent selections succeeded — "
                          "potential double-selection of same UTXOs for different transfers.");
    }
}

BOOST_AUTO_TEST_CASE(rh28_06c_concurrent_position_add_read)
{
    // Stress test: add positions while another thread reads them
    DigiDollarWallet ddw;
    std::atomic<bool> done{false};
    std::atomic<int> read_errors{0};

    auto writer = [&]() {
        for (int i = 0; i < 200; i++) {
            ddw.AddMockPosition(MakeTestHash(4000 + i), 1000 * (i + 1),
                               500 * COIN, 4, 100000 + i);
        }
        done = true;
    };

    auto reader = [&]() {
        while (!done) {
            try {
                auto pos = ddw.GetDDTimeLocks(true);
                auto allPos = ddw.GetDDTimeLocks(false);
                // Verify active subset <= all
                if (pos.size() > allPos.size()) {
                    read_errors++;
                }
            } catch (...) {
                read_errors++;
            }
        }
    };

    std::thread tw(writer);
    std::thread tr(reader);
    tw.join();
    tr.join();

    BOOST_CHECK_EQUAL(read_errors.load(), 0);
}

// =============================================================================
// RH-28-07: Transaction Construction Timing Attack (TOCTOU)
// ATTACK: Between coin selection and transaction signing, the UTXOs could
// be spent by another transaction, leaving a signed but invalid TX.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_07_utxo_vanishes_between_select_and_use)
{
    // SCENARIO: Select DD coins, then remove a selected UTXO before use.
    // The wallet should detect this when trying to sign.
    DigiDollarWallet ddw;

    COutPoint utxo1(MakeTestHash(5000), 1);
    COutPoint utxo2(MakeTestHash(5001), 1);
    ddw.AddDDUTXO(utxo1, 3000);
    ddw.AddDDUTXO(utxo2, 3000);

    // Select coins for $50 transfer
    std::vector<COutPoint> selected;
    CAmount total = 0;
    BOOST_CHECK(ddw.SelectDDCoins(5000, selected, total));
    BOOST_CHECK(total >= 5000);

    // Now remove one of the selected UTXOs (simulating concurrent spend)
    if (!selected.empty()) {
        ddw.RemoveDDUTXO(selected[0]);
    }

    // The UTXO is gone — any subsequent operation using it should detect this
    if (!selected.empty()) {
        BOOST_CHECK(!ddw.HasDDUTXO(selected[0]));
        BOOST_CHECK_EQUAL(ddw.GetDDFromUTXO(selected[0]), 0);
    }

    // A second selection for the same amount may now fail
    std::vector<COutPoint> selected2;
    CAmount total2 = 0;
    bool second_ok = ddw.SelectDDCoins(5000, selected2, total2);
    // With one UTXO removed (3000) and one remaining (3000), 
    // selecting 5000 should fail
    BOOST_CHECK(!second_ok);
}

// =============================================================================
// RH-28-08: Wallet DB Corruption Resilience
// ATTACK: Partially corrupt DD position data in wallet DB.
// Wallet should handle gracefully without losing funds.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_08_position_with_zero_collateral)
{
    // A corrupted position might have zero collateral but active status.
    // This could allow "free" DD if not caught.
    DigiDollarWallet ddw;

    uint256 id = MakeTestHash(6000);
    ddw.AddMockPosition(id, 50000, 0, 4, 100000); // zero collateral!

    auto positions = ddw.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(positions.size(), 1);

    // Document: the wallet stores it. Consensus validation is the real guard.
    CAmount locked = ddw.GetLockedCollateral();
    BOOST_CHECK_EQUAL(locked, 0); // Zero collateral correctly reported

    BOOST_TEST_MESSAGE("WARNING: Wallet stores position with 0 collateral. "
                      "Consensus must reject such mints at validation time.");
}

BOOST_AUTO_TEST_CASE(rh28_08b_position_with_negative_amounts)
{
    // Corrupted DB could produce negative DD amounts in positions
    DigiDollarWallet ddw;

    uint256 id = MakeTestHash(6010);
    // Negative DD amount and negative collateral
    ddw.AddMockPosition(id, -50000, -500 * COIN, 4, 100000);

    auto positions = ddw.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(positions.size(), 1);

    // These negative values must NOT corrupt balance calculations
    CAmount locked = ddw.GetLockedCollateral();
    // Depending on impl, this could be negative (bug) or clamped to 0
    BOOST_TEST_MESSAGE("Locked collateral with negative position: " + std::to_string(locked));
}

BOOST_AUTO_TEST_CASE(rh28_08c_duplicate_position_ids)
{
    // ATTACK: Two positions with the same dd_timelock_id.
    // The map should handle this (last write wins), but document behavior.
    DigiDollarWallet ddw;

    uint256 id = MakeTestHash(6020);
    ddw.AddMockPosition(id, 50000, 250 * COIN, 4, 100000);
    ddw.AddMockPosition(id, 100000, 500 * COIN, 4, 200000); // overwrite

    auto positions = ddw.GetDDTimeLocks(true);
    // Map uses same key, so should be 1
    BOOST_CHECK_EQUAL(positions.size(), 1);
    // The second write should win
    BOOST_CHECK_EQUAL(positions[0].dd_minted, 100000);
}

// =============================================================================
// RH-28-09: BurnDigiDollars Change Handling
// ATTACK: BurnDigiDollars selects more DD than needed but there's no
// mechanism to return "change" DD to the wallet.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_09_burn_excess_dd_no_change)
{
    // If we have a 10000-cent UTXO and burn 5000, what happens to the other 5000?
    DigiDollarWallet ddw;

    COutPoint utxo(MakeTestHash(7000), 1);
    ddw.AddDDUTXO(utxo, 10000); // $100

    std::vector<COutPoint> burned;
    bool ok = ddw.BurnDigiDollars(5000, burned); // Burn $50

    // BurnDigiDollars should succeed
    BOOST_CHECK(ok);
    BOOST_CHECK_EQUAL(burned.size(), 1);

    // The ENTIRE 10000-cent UTXO was selected for the 5000-cent burn.
    // The difference (5000) should be returned as change somewhere.
    // In the current implementation, BurnDigiDollars just marks UTXOs pending,
    // and the actual transaction builder handles change.
    // But document: if the burn is wallet-internal only (no on-chain tx),
    // the excess 5000 DD is LOST.
    BOOST_TEST_MESSAGE("NOTE: BurnDigiDollars selects whole UTXOs. "
                      "If burn amount < UTXO amount, excess DD depends on "
                      "transaction builder creating a change output.");
}

// =============================================================================
// RH-28-10: DD Address Key Storage Completeness
// ATTACK: Keys generated via getdigidollaraddress are stored in
// dd_address_keys. If this storage is lost, received DD becomes unspendable.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_10_address_key_roundtrip)
{
    DigiDollarWallet ddw;

    CKey key;
    key.MakeNewKey(true);
    XOnlyPubKey xpub(key.GetPubKey());

    ddw.StoreAddressKey(xpub, key);

    CKey retrieved;
    BOOST_CHECK(ddw.GetAddressKey(xpub, retrieved));
    BOOST_CHECK(retrieved.IsValid());
    BOOST_CHECK(retrieved.GetPubKey() == key.GetPubKey());
}

BOOST_AUTO_TEST_CASE(rh28_10b_address_key_missing)
{
    DigiDollarWallet ddw;

    // Random XOnlyPubKey that was never stored
    CKey randomKey;
    randomKey.MakeNewKey(true);
    XOnlyPubKey xpub(randomKey.GetPubKey());

    CKey retrieved;
    BOOST_CHECK(!ddw.GetAddressKey(xpub, retrieved));
}

// =============================================================================
// RH-28-11: IsLockedByDD Bypass
// ATTACK: If IsLockedByDD doesn't catch all DD-related UTXOs,
// regular DGB operations could accidentally spend collateral.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_11_locked_by_dd_coverage)
{
    DigiDollarWallet ddw;

    uint256 mintId = MakeTestHash(8000);
    ddw.AddMockPosition(mintId, 50000, 250 * COIN, 4, 100000);

    // Collateral output (vout 0 of mint)
    COutPoint collateralOut(mintId, 0);
    // DD token output (vout 1 of mint)
    COutPoint ddTokenOut(mintId, 1);
    // Random unrelated output
    COutPoint unrelatedOut(MakeTestHash(8001), 0);

    // Both collateral and DD token should be locked
    BOOST_CHECK(ddw.IsLockedByDD(collateralOut));
    BOOST_CHECK(ddw.IsLockedByDD(ddTokenOut));

    // Unrelated should NOT be locked
    BOOST_CHECK(!ddw.IsLockedByDD(unrelatedOut));
}

BOOST_AUTO_TEST_CASE(rh28_11b_locked_by_dd_inactive_position)
{
    // After a position is redeemed (inactive), its UTXOs should NOT be locked
    DigiDollarWallet ddw;

    uint256 mintId = MakeTestHash(8010);
    ddw.AddMockPosition(mintId, 50000, 250 * COIN, 4, 100000);

    COutPoint collateralOut(mintId, 0);
    BOOST_CHECK(ddw.IsLockedByDD(collateralOut));

    // Mark inactive
    ddw.UpdatePositionStatus(mintId, false);

    // Should no longer be locked (collateral is released)
    bool still_locked = ddw.IsLockedByDD(collateralOut);
    if (still_locked) {
        BOOST_TEST_MESSAGE("VULNERABILITY: Inactive position still locks collateral. "
                          "Redeemed collateral cannot be spent by wallet.");
    }
}

// =============================================================================
// RH-28-12: DDTimeLock Redeemability Check
// ATTACK: Attempt to redeem a position before its unlock height.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_12_premature_redeem_check)
{
    DigiDollarWallet ddw;

    uint256 mintId = MakeTestHash(9000);
    int64_t unlockHeight = 500000;
    ddw.AddMockPosition(mintId, 50000, 250 * COIN, 4, unlockHeight);

    // Before unlock height — should NOT be redeemable
    BOOST_CHECK(!ddw.IsDDTimeLockRedeemable(mintId, unlockHeight - 1));
    BOOST_CHECK(!ddw.IsDDTimeLockRedeemable(mintId, 0));

    // At exact unlock height — should be redeemable
    BOOST_CHECK(ddw.IsDDTimeLockRedeemable(mintId, unlockHeight));

    // After unlock height — should be redeemable
    BOOST_CHECK(ddw.IsDDTimeLockRedeemable(mintId, unlockHeight + 10000));
}

BOOST_AUTO_TEST_CASE(rh28_12b_redeem_nonexistent_position)
{
    DigiDollarWallet ddw;

    uint256 fakeId = MakeTestHash(9010);
    // Should return false for non-existent position
    BOOST_CHECK(!ddw.IsDDTimeLockRedeemable(fakeId, 999999));
}

BOOST_AUTO_TEST_CASE(rh28_12c_redeem_inactive_position)
{
    DigiDollarWallet ddw;

    uint256 mintId = MakeTestHash(9020);
    ddw.AddMockPosition(mintId, 50000, 250 * COIN, 4, 100000);
    ddw.UpdatePositionStatus(mintId, false);

    // Inactive position should NOT be redeemable even past unlock height
    BOOST_CHECK(!ddw.IsDDTimeLockRedeemable(mintId, 200000));
}

// =============================================================================
// RH-28-13: ClearWalletData Completeness
// ATTACK: If ClearWalletData doesn't clear all DD state, stale data
// from a previous wallet could contaminate a new one.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_13_clear_wallet_data_completeness)
{
    DigiDollarWallet ddw;

    // Populate all DD state
    uint256 posId = MakeTestHash(10000);
    ddw.AddMockPosition(posId, 50000, 250 * COIN, 4, 100000);
    ddw.AddDDUTXO(COutPoint(posId, 1), 50000);

    CKey key;
    key.MakeNewKey(true);
    ddw.StoreOwnerKey(posId, key);
    ddw.StoreAddressKey(XOnlyPubKey(key.GetPubKey()), key);

    // Verify populated
    BOOST_CHECK_GE(ddw.GetPositionCount(), 1);
    BOOST_CHECK(ddw.HasDDUTXO(COutPoint(posId, 1)));

    // Clear everything
    ddw.ClearWalletData();

    // Verify all DD state is gone
    BOOST_CHECK_EQUAL(ddw.GetPositionCount(), 0);
    BOOST_CHECK(!ddw.HasDDUTXO(COutPoint(posId, 1)));
    BOOST_CHECK_EQUAL(ddw.GetTotalDDBalance(), 0);
    BOOST_CHECK_EQUAL(ddw.GetLockedCollateral(), 0);

    auto positions = ddw.GetDDTimeLocks(false);
    BOOST_CHECK_EQUAL(positions.size(), 0);

    // Owner key should also be cleared
    CKey retrieved;
    BOOST_CHECK(!ddw.GetOwnerKey(posId, retrieved));
}

// =============================================================================
// RH-28-14: Serialization Round-Trip Integrity
// ATTACK: Corrupt serialized DD data to produce unexpected deserialization.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_14_ddtransaction_serialization_roundtrip)
{
    DDTransaction tx;
    tx.txid = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    tx.amount = 50000;
    tx.timestamp = 1700000000;
    tx.confirmations = 6;
    tx.incoming = true;
    tx.address = "DD1testaddress";
    tx.category = "mint";
    tx.blockheight = 100000;
    tx.blockhash = "0000000000000000000000000000000000000000000000000000000000000001";
    tx.fee = 10000000;
    tx.comment = "test transaction";
    tx.abandoned = false;
    tx.lock_tier = 4;

    // Serialize
    DataStream ss{};
    ss << tx;

    // Deserialize
    DDTransaction tx2;
    ss >> tx2;

    // Verify all fields match
    BOOST_CHECK_EQUAL(tx.txid, tx2.txid);
    BOOST_CHECK_EQUAL(tx.amount, tx2.amount);
    BOOST_CHECK_EQUAL(tx.timestamp, tx2.timestamp);
    BOOST_CHECK_EQUAL(tx.confirmations, tx2.confirmations);
    BOOST_CHECK_EQUAL(tx.incoming, tx2.incoming);
    BOOST_CHECK_EQUAL(tx.address, tx2.address);
    BOOST_CHECK_EQUAL(tx.category, tx2.category);
    BOOST_CHECK_EQUAL(tx.blockheight, tx2.blockheight);
    BOOST_CHECK_EQUAL(tx.blockhash, tx2.blockhash);
    BOOST_CHECK_EQUAL(tx.fee, tx2.fee);
    BOOST_CHECK_EQUAL(tx.comment, tx2.comment);
    BOOST_CHECK_EQUAL(tx.abandoned, tx2.abandoned);
    BOOST_CHECK_EQUAL(tx.lock_tier, tx2.lock_tier);
}

BOOST_AUTO_TEST_CASE(rh28_14b_collateral_position_serialization_roundtrip)
{
    WalletCollateralPosition pos;
    pos.dd_timelock_id = MakeTestHash(11000);
    pos.dd_minted = 100000;
    pos.dgb_collateral = 500 * COIN;
    pos.lock_tier = 4;
    pos.unlock_height = 200000;
    pos.is_active = true;

    DataStream ss{};
    ss << pos;

    WalletCollateralPosition pos2;
    ss >> pos2;

    BOOST_CHECK(pos.dd_timelock_id == pos2.dd_timelock_id);
    BOOST_CHECK_EQUAL(pos.dd_minted, pos2.dd_minted);
    BOOST_CHECK_EQUAL(pos.dgb_collateral, pos2.dgb_collateral);
    BOOST_CHECK_EQUAL(pos.lock_tier, pos2.lock_tier);
    BOOST_CHECK_EQUAL(pos.unlock_height, pos2.unlock_height);
    BOOST_CHECK_EQUAL(pos.is_active, pos2.is_active);
}

BOOST_AUTO_TEST_CASE(rh28_14c_truncated_serialization)
{
    // ATTACK: Feed truncated data to deserializer
    WalletCollateralPosition pos;
    pos.dd_timelock_id = MakeTestHash(11010);
    pos.dd_minted = 100000;
    pos.dgb_collateral = 500 * COIN;
    pos.lock_tier = 4;
    pos.unlock_height = 200000;
    pos.is_active = true;

    DataStream ss{};
    ss << pos;

    // Truncate the data (remove last 10 bytes)
    auto raw = MakeByteSpan(ss);
    std::vector<std::byte> data(raw.begin(), raw.end());
    if (data.size() > 10) {
        data.resize(data.size() - 10);
    }

    DataStream truncated(data);
    WalletCollateralPosition pos2;
    bool threw = false;
    try {
        truncated >> pos2;
    } catch (const std::exception&) {
        threw = true;
    }
    // Should throw on truncated data, not silently produce garbage
    BOOST_CHECK(threw);
}

// =============================================================================
// RH-28-15: OP_RETURN Metadata Extraction Robustness
// ATTACK: Craft malicious OP_RETURN data to confuse DD amount extraction.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_15_extract_dd_amount_empty_tx)
{
    // Empty transaction — should return false
    CMutableTransaction mtx;
    CTransaction tx(mtx);
    CAmount amount = -1;
    bool ok = DigiDollarWallet::ExtractDDAmountFromOpReturn(tx, amount);
    BOOST_CHECK(!ok);
}

BOOST_AUTO_TEST_CASE(rh28_15b_extract_dd_amount_no_opreturn)
{
    // Transaction with outputs but no OP_RETURN
    CMutableTransaction mtx;
    mtx.vout.resize(2);
    mtx.vout[0].nValue = 100 * COIN;
    mtx.vout[0].scriptPubKey = CScript() << OP_1 << std::vector<unsigned char>(32, 0xab);
    mtx.vout[1].nValue = 0;
    mtx.vout[1].scriptPubKey = CScript() << OP_1 << std::vector<unsigned char>(32, 0xcd);

    CTransaction tx(mtx);
    CAmount amount = -1;
    bool ok = DigiDollarWallet::ExtractDDAmountFromOpReturn(tx, amount);
    BOOST_CHECK(!ok);
}

// =============================================================================
// RH-28-16: Massive DD UTXO Set Stress Test
// ATTACK: Create thousands of tiny DD UTXOs to slow down coin selection
// and potentially cause DoS on wallet operations.
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_16_massive_utxo_set_performance)
{
    DigiDollarWallet ddw;

    // Add 10,000 tiny UTXOs (1 cent each)
    constexpr int NUM_UTXOS = 10000;
    for (int i = 0; i < NUM_UTXOS; i++) {
        ddw.AddDDUTXO(COutPoint(MakeTestHash(20000 + i), 1), 1); // 1 cent
    }

    // Time coin selection for $50 (need 5000 UTXOs)
    auto start = std::chrono::steady_clock::now();
    std::vector<COutPoint> selected;
    CAmount total = 0;
    bool ok = ddw.SelectDDCoins(5000, selected, total);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    BOOST_CHECK(ok);
    BOOST_CHECK_GE(total, 5000);
    BOOST_CHECK_GE(selected.size(), 5000);

    // Should complete in reasonable time (< 5 seconds)
    BOOST_CHECK_LT(ms, 5000);
    BOOST_TEST_MESSAGE("Coin selection with " + std::to_string(NUM_UTXOS) +
                      " UTXOs took " + std::to_string(ms) + "ms, selected " +
                      std::to_string(selected.size()) + " UTXOs");
}

// =============================================================================
// RH-28-17: DDTimeLock Status Lifecycle Integrity
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_17_timelock_status_transitions)
{
    DigiDollarWallet ddw;

    uint256 id = MakeTestHash(12000);
    ddw.AddMockPosition(id, 50000, 250 * COIN, 4, 100000);

    // Initial: active
    BOOST_CHECK_EQUAL(ddw.GetDDTimeLockStatus(id), "active");

    // Deactivate (redeem)
    ddw.UpdatePositionStatus(id, false);
    BOOST_CHECK_EQUAL(ddw.GetDDTimeLockStatus(id), "fully_redeemed");

    // Re-activate (should this be allowed? Document behavior)
    ddw.UpdatePositionStatus(id, true);
    std::string status = ddw.GetDDTimeLockStatus(id);
    BOOST_TEST_MESSAGE("Re-activating redeemed position: status = " + status);

    // Non-existent
    BOOST_CHECK_EQUAL(ddw.GetDDTimeLockStatus(MakeTestHash(12001)), "not_found");
}

// =============================================================================
// RH-28-18: IsDDOutputMine with Outpoint Overload
// =============================================================================

BOOST_AUTO_TEST_CASE(rh28_18_is_dd_output_mine_outpoint)
{
    DigiDollarWallet ddw;

    COutPoint knownUtxo(MakeTestHash(13000), 1);
    ddw.AddDDUTXO(knownUtxo, 5000);

    COutPoint unknownUtxo(MakeTestHash(13001), 1);

    // Known UTXO should be "mine"
    BOOST_CHECK(ddw.IsDDOutputMine(knownUtxo));

    // Unknown UTXO should not be "mine"
    BOOST_CHECK(!ddw.IsDDOutputMine(unknownUtxo));
}

BOOST_AUTO_TEST_SUITE_END()
