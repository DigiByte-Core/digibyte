// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// RH-27: ERR (Emergency Reserve Ratio) Path Attack Tests
// Red-team tests targeting ERR calculation, volatility freeze bypass,
// TOCTOU races, and edge cases in the protection system.

#include <consensus/digidollar.h>
#include <util/int128.h>
#include <consensus/dca.h>
#include <consensus/err.h>
#include <consensus/volatility.h>
#include <digidollar/health.h>
#include <digidollar/validation.h>
#include <digidollar/digidollar.h>
#include <primitives/oracle.h>
#include <key.h>
#include <pubkey.h>
#include <script/script.h>
#include <primitives/transaction.h>
#include <consensus/validation.h>
#include <test/util/setup_common.h>
#include <chainparams.h>
#include <cmath>
#include <limits>

#include <boost/test/unit_test.hpp>

using namespace DigiDollar;
using namespace DigiDollar::ERR;
using namespace DigiDollar::DCA;
using namespace DigiDollar::Volatility;

BOOST_AUTO_TEST_SUITE(digidollar_err_attack_tests)

// ============================================================================
// Test fixture with health state manipulation
// ============================================================================

struct ERRAttackTestSetup : public TestingSetup {
    ERRAttackTestSetup() : TestingSetup(ChainType::REGTEST) {
        // Reset all global state
        SystemHealthMonitor::ResetMetrics();
        VolatilityMonitor::ClearHistory();
        VolatilityMonitor::ClearFreeze();

        // Reset ERR state by reconstructing as healthy
        EmergencyRedemptionRatio::ReconstructERRState(200, 10000);
    }

    ~ERRAttackTestSetup() {
        SystemHealthMonitor::ResetMetrics();
        VolatilityMonitor::ClearHistory();
        VolatilityMonitor::ClearFreeze();
        EmergencyRedemptionRatio::ReconstructERRState(200, 10000);
    }

    // Helper: Set system health by directly manipulating cached metrics
    void SetSystemHealth(CAmount ddSupply, CAmount collateral) {
        SystemHealthMonitor::ResetMetrics();
        if (ddSupply > 0) {
            SystemHealthMonitor::OnMintConnected(ddSupply, collateral);
        }
    }
};

// ============================================================================
// Attack Vector 1: ERR calculation with extreme health values
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_err_negative_health_adjustment, ERRAttackTestSetup)
{
    // ATTACK: Feed negative system health to CalculateERRAdjustment
    // Expected: Should return minimum ratio (0.80), NOT crash or return > 1.0
    double ratio = EmergencyRedemptionRatio::CalculateERRAdjustment(-1);
    BOOST_CHECK_EQUAL(ratio, 0.80);

    ratio = EmergencyRedemptionRatio::CalculateERRAdjustment(-100);
    BOOST_CHECK_EQUAL(ratio, 0.80);

    ratio = EmergencyRedemptionRatio::CalculateERRAdjustment(std::numeric_limits<int>::min());
    BOOST_CHECK_EQUAL(ratio, 0.80);
}

BOOST_FIXTURE_TEST_CASE(rh27_err_extreme_dd_burn_overflow, ERRAttackTestSetup)
{
    // ATTACK: GetRequiredDDBurn with MAX CAmount — can the division by 0.80
    // produce a value exceeding int64_t?
    CAmount maxDD = std::numeric_limits<CAmount>::max();
    CAmount burn = EmergencyRedemptionRatio::GetRequiredDDBurn(maxDD, 50); // worst case 0.80 ratio

    // Must not overflow — should be clamped to max
    BOOST_CHECK_MESSAGE(burn > 0, "Burn amount must be positive");
    BOOST_CHECK_MESSAGE(burn >= maxDD || burn == std::numeric_limits<CAmount>::max(),
        "Burn amount with 0.80 ratio should be >= original or clamped to max");
}

BOOST_FIXTURE_TEST_CASE(rh27_err_zero_dd_burn, ERRAttackTestSetup)
{
    // ATTACK: GetRequiredDDBurn with 0 amount
    CAmount burn = EmergencyRedemptionRatio::GetRequiredDDBurn(0, 50);
    BOOST_CHECK_EQUAL(burn, 0);

    // Negative DD amount
    burn = EmergencyRedemptionRatio::GetRequiredDDBurn(-100, 50);
    BOOST_CHECK_EQUAL(burn, 0);
}

BOOST_FIXTURE_TEST_CASE(rh27_dca_health_zero_supply_zero_collateral, ERRAttackTestSetup)
{
    // ATTACK: CalculateSystemHealth with all zeros
    int health = DynamicCollateralAdjustment::CalculateSystemHealth(0, 0, 50000);
    BOOST_CHECK_EQUAL(health, 30000); // No DD = max health

    // Zero collateral with DD supply
    health = DynamicCollateralAdjustment::CalculateSystemHealth(0, 10000, 50000);
    BOOST_CHECK_EQUAL(health, 0); // No collateral = zero health

    // Zero oracle price
    health = DynamicCollateralAdjustment::CalculateSystemHealth(100 * COIN, 10000, 0);
    BOOST_CHECK_EQUAL(health, 0); // No price = zero health
}

BOOST_FIXTURE_TEST_CASE(rh27_dca_health_int64_max_values, ERRAttackTestSetup)
{
    // ATTACK: MAX_MONEY collateral and MAX_MONEY DD supply
    // This tests the util::int128_t overflow protection
    CAmount maxMoney = MAX_MONEY;
    int health = DynamicCollateralAdjustment::CalculateSystemHealth(maxMoney, maxMoney, maxMoney);

    // Should not crash, should return a clamped value
    BOOST_CHECK_MESSAGE(health >= 0, "Health must be non-negative");
    BOOST_CHECK_MESSAGE(health <= 30000, "Health must be capped at 30000");
}

BOOST_FIXTURE_TEST_CASE(rh27_dca_health_tiny_supply_huge_collateral, ERRAttackTestSetup)
{
    // ATTACK: 1 cent DD supply with massive collateral
    // Tests division-by-small-number edge case
    CAmount hugeCollateral = 1000000 * COIN; // 1M DGB
    CAmount tinyDD = 1; // 1 cent
    CAmount price = 50000; // $0.50

    int health = DynamicCollateralAdjustment::CalculateSystemHealth(hugeCollateral, tinyDD, price);
    BOOST_CHECK_MESSAGE(health == 30000, "Massive over-collateralization should cap at 30000");
}

// ============================================================================
// Attack Vector 2: Volatility freeze bypass via block timing
// BUG FOUND: COOLDOWN_BLOCKS = 144 is ~36 MINUTES at 15s, NOT 36 hours!
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_cooldown_duration_is_36_hours_not_36_minutes, ERRAttackTestSetup)
{
    // BUG [RH-27-001]: COOLDOWN_BLOCKS = 144, but at 15s/block that's only
    // 144 * 15 = 2160 seconds = 36 MINUTES.
    // For 36 hours: 36 * 3600 / 15 = 8640 blocks needed.
    //
    // This means a miner only needs to wait 36 minutes after a volatility
    // freeze before they can mint again — 100x shorter than intended.

    uint32_t cooldownBlocks = VolatilityThresholds::COOLDOWN_BLOCKS;
    uint32_t dgb_block_time = 15; // DigiByte 15-second blocks
    uint32_t cooldown_seconds = cooldownBlocks * dgb_block_time;
    uint32_t cooldown_hours = cooldown_seconds / 3600;

    // This test FAILS with current code (144 blocks = 36 min)
    // It should be 8640 blocks for 36 hours
    BOOST_CHECK_MESSAGE(cooldown_hours >= 36,
        "COOLDOWN_BLOCKS should represent ~36 hours at 15s blocks. "
        "Currently " + std::to_string(cooldownBlocks) + " blocks = " +
        std::to_string(cooldown_seconds) + " seconds = " +
        std::to_string(cooldown_seconds / 60) + " minutes. "
        "Expected 8640 blocks for 36 hours.");
}

BOOST_FIXTURE_TEST_CASE(rh27_volatility_freeze_bypass_fast_blocks, ERRAttackTestSetup)
{
    // ATTACK: Trigger freeze, then advance just past COOLDOWN_BLOCKS
    // With the bug, attacker waits only 36 min instead of 36 hours

    uint32_t freezeHeight = 10000;
    VolatilityMonitor::TriggerFreeze(true, freezeHeight);

    // Should be frozen
    BOOST_CHECK(VolatilityMonitor::ShouldFreezeAll());

    // Advance past cooldown — with bug, only 145 blocks needed
    // Record a stable price so volatility drops
    int64_t baseTime = 1700000000;
    CAmount stablePrice = 50000;
    for (uint32_t h = freezeHeight; h <= freezeHeight + VolatilityThresholds::COOLDOWN_BLOCKS + 10; h++) {
        VolatilityMonitor::RecordPrice(stablePrice, baseTime + (h - freezeHeight) * 15, h);
    }
    VolatilityMonitor::UpdateState(freezeHeight + VolatilityThresholds::COOLDOWN_BLOCKS + 1);

    // After cooldown + stable prices, freeze should lift
    // The problem is this happens after only ~36 minutes
    bool stillFrozen = VolatilityMonitor::ShouldFreezeAll();

    // This documents the bug: operations unfrozen way too quickly
    // After fix, COOLDOWN_BLOCKS should be 8640 and this won't unfreeze at block +145
    if (!stillFrozen) {
        BOOST_TEST_MESSAGE("WARNING: Freeze lifted after only " +
            std::to_string(VolatilityThresholds::COOLDOWN_BLOCKS) +
            " blocks (~" + std::to_string(VolatilityThresholds::COOLDOWN_BLOCKS * 15 / 60) +
            " minutes). Should be 36 hours.");
    }
}

// ============================================================================
// Attack Vector 3: GetCurrentState() side-effect mutation
// BUG FOUND: Querying ERR state can auto-deactivate it
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_get_current_state_mutates_err, ERRAttackTestSetup)
{
    // BUG [RH-27-002]: GetCurrentState() calls DeactivateERR() if health >= 100%.
    // This means a monitoring/RPC query can change consensus state.
    //
    // Attack scenario:
    // 1. ERR is active (health < 100%)
    // 2. A single large mint increases collateral enough that health >= 100%
    //    (via OnMintConnected updating s_currentMetrics)
    // 3. Any call to GetCurrentState() (monitoring, RPC, etc.) auto-deactivates ERR
    // 4. This is NOT deterministic across nodes (depends on when they query)

    // Setup: Force ERR active with unhealthy system
    SetSystemHealth(100000, 50 * COIN); // Under-collateralized
    EmergencyRedemptionRatio::ReconstructERRState(80, 10000);

    ERRState state1 = EmergencyRedemptionRatio::GetCurrentState();
    // State depends on GetCurrentSystemHealth() which reads cached metrics

    // Now simulate health recovery by adding collateral
    SystemHealthMonitor::OnMintConnected(0, 10000 * COIN); // Add massive collateral

    // This query SHOULD be read-only but actually mutates state!
    ERRState state2 = EmergencyRedemptionRatio::GetCurrentState();

    // Document the bug: GetCurrentState should not have side effects
    BOOST_TEST_MESSAGE("GetCurrentState side-effect: isActive went from " +
        std::string(state1.isActive ? "true" : "false") + " to " +
        std::string(state2.isActive ? "true" : "false") +
        " just by querying state");

    // A proper implementation would separate state queries from state transitions
    // ERR deactivation should only happen during block validation, not on queries
}

// ============================================================================
// Attack Vector 4: ERR + DCA interaction producing unexpected requirements
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_dca_multiplier_at_exact_boundaries, ERRAttackTestSetup)
{
    // ATTACK: DCA tier boundaries — check for off-by-one
    // Tier boundaries: 0-109 (2.0x), 110-119 (1.5x), 120-149 (1.25x), 150+ (1.0x)

    // At exact boundary: 110 should be critical (1.5x); 109 is emergency floor (2.0x)
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(110), 1.5);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(109), 2.0);

    // At 120 boundary
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(120), 1.25);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(119), 1.5);

    // At 150 boundary
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(150), 1.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(149), 1.25);
}

BOOST_FIXTURE_TEST_CASE(rh27_dca_err_combined_effect, ERRAttackTestSetup)
{
    // ATTACK: When both DCA and ERR are active, what are total requirements?
    // At health = 80%:
    //   DCA multiplier = 2.0x (emergency)
    //   ERR ratio = 0.80 (must burn 125% DD)
    // Combined: Need 2.0x collateral AND 1.25x DD burn

    int health = 80;

    // DCA effect on collateral
    int baseRatio = 300; // 300% base collateral
    int dcaAdjusted = DynamicCollateralAdjustment::ApplyDCA(baseRatio, health);
    BOOST_CHECK_EQUAL(dcaAdjusted, 600); // 300% * 2.0x = 600%

    // ERR effect on DD burn
    CAmount originalDD = 10000; // $100
    CAmount requiredBurn = EmergencyRedemptionRatio::GetRequiredDDBurn(originalDD, health);
    BOOST_CHECK_MESSAGE(requiredBurn == 12500, // 10000 / 0.80 = 12500
        "ERR at 80% health should require 125% DD burn, got " +
        std::to_string(requiredBurn));
}

BOOST_FIXTURE_TEST_CASE(rh27_dca_negative_health_multiplier, ERRAttackTestSetup)
{
    // ATTACK: Negative health value to DCA
    // With negative health, no tier matches (0-99 requires >= 0)
    // Falls through to fallback return 2.0
    double mult = DynamicCollateralAdjustment::GetDCAMultiplier(-1);
    BOOST_CHECK_EQUAL(mult, 2.0);

    mult = DynamicCollateralAdjustment::GetDCAMultiplier(-1000);
    BOOST_CHECK_EQUAL(mult, 2.0);

    // But the tier lookup for negative values — does GetCurrentTier crash?
    auto tier = DynamicCollateralAdjustment::GetCurrentTier(-1);
    BOOST_CHECK_EQUAL(tier.status, "emergency");
}

// ============================================================================
// Attack Vector 5: Freeze duration manipulation
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_cooldown_end_height_overflow, ERRAttackTestSetup)
{
    // ATTACK: Trigger freeze at height near UINT32_MAX
    // cooldownEndHeight = height + COOLDOWN_BLOCKS could overflow
    uint32_t nearMax = std::numeric_limits<uint32_t>::max() - 10;

    VolatilityMonitor::TriggerFreeze(false, nearMax);
    uint32_t cooldownEnd = VolatilityMonitor::GetCooldownEndHeight();

    // With saturation fix, cooldownEndHeight should be capped at UINT32_MAX
    BOOST_CHECK_EQUAL(cooldownEnd, std::numeric_limits<uint32_t>::max());
    BOOST_CHECK_MESSAGE(cooldownEnd >= nearMax,
        "cooldownEndHeight must not overflow. cooldownEnd=" +
        std::to_string(cooldownEnd) + " nearMax=" + std::to_string(nearMax));

    VolatilityMonitor::ClearFreeze();
}

// ============================================================================
// Attack Vector 6: System health recovery path edge cases
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_err_deactivation_with_pending_queue, ERRAttackTestSetup)
{
    // ATTACK: Queue ERR redemptions, then health recovers before they're processed
    // What happens to queued redemptions?

    // Activate ERR
    EmergencyRedemptionRatio::ReconstructERRState(80, 10000);

    // Queue some redemptions
    COutPoint op1(uint256::ONE, 0);
    COutPoint op2(uint256::ONE, 1);
    bool queued1 = EmergencyRedemptionRatio::QueueERRRedemption(op1, 10000, 10001);
    bool queued2 = EmergencyRedemptionRatio::QueueERRRedemption(op2, 20000, 10002);

    BOOST_CHECK(queued1);
    BOOST_CHECK(queued2);

    // Queue should have 2 items
    auto queue = EmergencyRedemptionRatio::GetERRQueue();
    BOOST_CHECK_EQUAL(queue.size(), 2);

    // Now health recovers — DeactivateERR processes remaining queue
    bool deactivated = EmergencyRedemptionRatio::DeactivateERR(150);
    BOOST_CHECK(deactivated);

    // Queue should be cleared
    queue = EmergencyRedemptionRatio::GetERRQueue();
    BOOST_CHECK_EQUAL(queue.size(), 0);
}

BOOST_FIXTURE_TEST_CASE(rh27_err_double_queue_same_outpoint, ERRAttackTestSetup)
{
    // ATTACK: Try to queue the same outpoint twice to get double redemption
    EmergencyRedemptionRatio::ReconstructERRState(80, 10000);

    COutPoint op(uint256::ONE, 0);
    bool queued1 = EmergencyRedemptionRatio::QueueERRRedemption(op, 10000, 10001);
    bool queued2 = EmergencyRedemptionRatio::QueueERRRedemption(op, 10000, 10001);

    BOOST_CHECK(queued1);
    BOOST_CHECK_MESSAGE(!queued2, "Should not allow double-queuing same outpoint");

    auto queue = EmergencyRedemptionRatio::GetERRQueue();
    BOOST_CHECK_EQUAL(queue.size(), 1);
}

// ============================================================================
// Attack Vector 7: ERR with concurrent mint+redeem in same block
// BUG FOUND: Health changes mid-block affecting ERR decisions
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_mint_redeem_same_block_order_dependency, ERRAttackTestSetup)
{
    // BUG [RH-27-004]: When a block contains both mint and redeem txs,
    // OnMintConnected/OnRedeemConnected update s_currentMetrics incrementally.
    // This means the system health changes between processing tx1 and tx2.
    //
    // Attack: Craft a block where:
    // - Tx1 is a large mint that pushes health above 100% (disabling ERR)
    // - Tx2 is a large redeem that exploits the now-disabled ERR
    // The order of tx processing determines whether ERR protections apply.

    // Start with unhealthy system: 10000 DD, 50 DGB collateral, $0.50 price
    // Health = (50 * 50000 / 1e8) * 100000 / (10000 * 1000) ≈ low
    SetSystemHealth(10000, 50 * COIN);

    // Check initial ShouldBlockMinting with a known oracle price
    // oraclePriceMicroUSD: 500000 = $0.50
    bool blocked1 = EmergencyRedemptionRatio::ShouldBlockMinting(500000);

    // Now simulate mint connected (adds massive collateral)
    SystemHealthMonitor::OnMintConnected(5000, 10000 * COIN);

    // Re-check — health has changed mid-block!
    bool blocked2 = EmergencyRedemptionRatio::ShouldBlockMinting(500000);

    // Document the inconsistency
    BOOST_TEST_MESSAGE("ShouldBlockMinting before mint: " +
        std::string(blocked1 ? "blocked" : "allowed") +
        ", after mint in same block: " +
        std::string(blocked2 ? "blocked" : "allowed"));

    // In a correct implementation, ERR state should be snapshotted at block start
    // and used consistently for all txs in the block
}

// ============================================================================
// Attack Vector 8: ERR tier boundary manipulation
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_err_tier_boundary_precision, ERRAttackTestSetup)
{
    // Test exact ERR tier boundaries
    // Tiers: {95, 0.95}, {90, 0.90}, {85, 0.85}, {0, 0.80}

    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(100), 1.0);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(99), 0.95);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(95), 0.95);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(94), 0.90);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(90), 0.90);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(89), 0.85);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(85), 0.85);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(84), 0.80);
    BOOST_CHECK_EQUAL(EmergencyRedemptionRatio::CalculateERRAdjustment(0), 0.80);
}

BOOST_FIXTURE_TEST_CASE(rh27_err_burn_calculation_precision, ERRAttackTestSetup)
{
    // Test that burn calculations use ceiling (not floor/round)
    // 1 DD / 0.95 = 1.05263... → should ceil to 2
    CAmount burn = EmergencyRedemptionRatio::GetRequiredDDBurn(1, 99);
    BOOST_CHECK_MESSAGE(burn >= 2,
        "1 cent at 0.95 ratio should ceil to 2 cents, got " + std::to_string(burn));

    // 100 DD / 0.80 = 125 exactly
    burn = EmergencyRedemptionRatio::GetRequiredDDBurn(100, 50);
    BOOST_CHECK_EQUAL(burn, 125);

    // 7 DD / 0.85 = 8.235... → should ceil to 9
    burn = EmergencyRedemptionRatio::GetRequiredDDBurn(7, 89);
    BOOST_CHECK_MESSAGE(burn >= 9,
        "7 cents at 0.85 ratio should ceil to 9, got " + std::to_string(burn));
}

// ============================================================================
// Attack Vector: ShouldBlockMinting fail-closed behavior
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_should_block_minting_no_oracle, ERRAttackTestSetup)
{
    // When oracle price is 0, ShouldBlockMinting should fail-closed (block minting)
    SetSystemHealth(10000, 100 * COIN); // Some DD in circulation

    bool blocked = EmergencyRedemptionRatio::ShouldBlockMinting(0);

    // With oraclePriceOverride=0, it falls through to query global oracle
    // In test env, mock oracle may return a price
    // But if no price available, should block (fail-closed)
    BOOST_TEST_MESSAGE("ShouldBlockMinting with zero override: " +
        std::string(blocked ? "blocked (correct)" : "allowed"));
}

BOOST_FIXTURE_TEST_CASE(rh27_should_block_minting_no_dd_supply, ERRAttackTestSetup)
{
    // When no DD in circulation, minting should ALWAYS be allowed
    SystemHealthMonitor::ResetMetrics(); // Zero supply

    bool blocked = EmergencyRedemptionRatio::ShouldBlockMinting(500000);
    BOOST_CHECK_MESSAGE(!blocked,
        "Minting should be allowed when no DD is in circulation");
}

// ============================================================================
// Attack Vector: Health calculation with tiny DD supply (division precision)
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_health_calculation_small_dd_supply, ERRAttackTestSetup)
{
    // ATTACK: Very small DD supply (1-999 cents) triggers the scaledDD=0 path
    // in CalculateSystemHealth's overflow branch

    // Massive collateral, tiny DD supply
    CAmount hugeCollateral = MAX_MONEY;
    CAmount tinyDD = 500; // $5
    CAmount highPrice = MAX_MONEY; // Extreme price

    int health = DynamicCollateralAdjustment::CalculateSystemHealth(hugeCollateral, tinyDD, highPrice);

    // Should not crash, should return capped max
    BOOST_CHECK_MESSAGE(health >= 0 && health <= 30000,
        "Health with extreme inputs should be in valid range, got " + std::to_string(health));
}

BOOST_FIXTURE_TEST_CASE(rh27_health_calculation_dd_between_1_and_999, ERRAttackTestSetup)
{
    // Test the DGB-SEC-003 fix: when collateralValueCents is huge and
    // totalDD is between 1-999, scaledDD = totalDD/1000 = 0
    // This previously caused division by zero

    CAmount collateral = 1000000 * COIN; // 1M DGB
    CAmount dd = 500; // $5 (500 cents)
    CAmount price = 100000; // $1 per DGB

    int health = DynamicCollateralAdjustment::CalculateSystemHealth(collateral, dd, price);

    // The fix returns 30000 when scaledDD==0 (tiny DD, massive collateral)
    BOOST_CHECK_MESSAGE(health == 30000,
        "Tiny DD with huge collateral should return max health, got " + std::to_string(health));
}

// ============================================================================
// Attack Vector: Volatility state persistence across UpdateState calls
// ============================================================================

BOOST_FIXTURE_TEST_CASE(rh27_volatility_freeze_without_update_state, ERRAttackTestSetup)
{
    // ATTACK: Can a miner avoid volatility freeze by not calling UpdateState?
    // In validation, UpdateState is called with ctx.nHeight > 0 && ctx.oraclePriceMicroUSD > 0
    // If a miner crafts a block with oraclePriceMicroUSD = 0, UpdateState is skipped

    // Record volatile prices
    int64_t baseTime = 1700000000;
    uint32_t baseHeight = 10000;
    VolatilityMonitor::RecordPrice(50000, baseTime, baseHeight);
    VolatilityMonitor::RecordPrice(10000, baseTime + 1800, baseHeight + 1); // 80% drop in 30 min

    // Update state to trigger freeze detection
    VolatilityMonitor::UpdateState(baseHeight + 1);

    bool frozen = VolatilityMonitor::ShouldFreezeMinting();

    // The freeze check in ValidateMintTransaction is independent of UpdateState:
    // It calls ShouldFreezeMinting() which returns the cached state.
    // But if UpdateState was never called after the price drop, the state is stale.
    BOOST_TEST_MESSAGE("After volatile price, freeze status: " +
        std::string(frozen ? "frozen" : "not frozen"));
}

BOOST_AUTO_TEST_SUITE_END()
