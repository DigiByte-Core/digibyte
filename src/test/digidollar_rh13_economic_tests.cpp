// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-13: Economic & Game Theory Attack Tests (Second Pass Red Team)
 *
 * Tests for economic exploits and game-theoretic attacks on the DigiDollar
 * stablecoin mechanism. Each test attempts to exploit a specific attack vector.
 */

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <consensus/err.h>
#include <consensus/volatility.h>
#include <consensus/amount.h>
#include <digidollar/health.h>
#include <digidollar/validation.h>

#include <limits>
#include <cmath>

using namespace DigiDollar;
using namespace DigiDollar::DCA;
using namespace DigiDollar::ERR;

BOOST_AUTO_TEST_SUITE(digidollar_rh13_economic_tests)

// ============================================================================
// ATTACK VECTOR 1: DCA Death Spiral
// Can an attacker intentionally drive DCA to extreme levels to make the system
// unusable? If DCA multiplier makes collateral requirements too high, nobody
// can mint, and the system dies.
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_01_dca_death_spiral_multiplier_cap)
{
    // Attack: Drive system health to 0, check if DCA multiplier is capped
    // If multiplier is uncapped, required collateral becomes infinite
    double multiplier_at_0 = DynamicCollateralAdjustment::GetDCAMultiplier(0);
    double multiplier_at_negative = DynamicCollateralAdjustment::GetDCAMultiplier(-1000);
    double multiplier_at_1 = DynamicCollateralAdjustment::GetDCAMultiplier(1);

    // FINDING: Max multiplier is 2.0x (emergency tier)
    // This is reasonable - at worst you need 2x the base ratio
    // 10-year lock: 200% * 2.0 = 400%, 1-hour lock: 1000% * 2.0 = 2000%
    BOOST_CHECK_EQUAL(multiplier_at_0, 2.0);
    BOOST_CHECK_EQUAL(multiplier_at_negative, 2.0);
    BOOST_CHECK_EQUAL(multiplier_at_1, 2.0);

    // Verify that the cap prevents death spiral:
    // Even at emergency, someone can still mint if they have enough DGB
    int baseRatio_10yr = 200;  // Lowest base ratio (10-year lock)
    int adjusted = DynamicCollateralAdjustment::ApplyDCA(baseRatio_10yr, 0);
    BOOST_CHECK_EQUAL(adjusted, 400);  // 200% * 2.0 = 400% — still feasible

    int baseRatio_1hr = 1000;  // Highest base ratio (1-hour lock)
    int adjusted_1hr = DynamicCollateralAdjustment::ApplyDCA(baseRatio_1hr, 0);
    BOOST_CHECK_EQUAL(adjusted_1hr, 2000);  // 1000% * 2.0 = 2000% — steep but possible
}

BOOST_AUTO_TEST_CASE(rh13_01_dca_death_spiral_recovery_path)
{
    // Attack scenario: System is at 50% health (emergency).
    // Can any new mints help recovery, or does DCA prevent all minting?
    //
    // At 50% health, DCA = 2.0x. New mints require double collateral.
    // This actually HELPS recovery because new positions are over-collateralized.
    // The question is: can the system recover once it hits emergency?

    // Test recovery path: 50% -> 80% -> 110% -> 160%
    std::vector<int> recovery = {50, 80, 110, 160};
    std::vector<double> expected_multipliers = {2.0, 2.0, 1.5, 1.0};

    for (size_t i = 0; i < recovery.size(); ++i) {
        double m = DynamicCollateralAdjustment::GetDCAMultiplier(recovery[i]);
        BOOST_CHECK_EQUAL(m, expected_multipliers[i]);
    }

    // FINDING: No hysteresis exists. The transition from emergency to healthy
    // is immediate — there's no "sticky" zone. This means the system can oscillate
    // rapidly between tiers. See rh13_01_dca_tier_oscillation below.
}

BOOST_AUTO_TEST_CASE(rh13_01_dca_tier_oscillation_attack)
{
    // VULNERABILITY: No hysteresis in DCA tiers.
    // Attack: Manipulate system health to oscillate around tier boundaries.
    // At health=149, multiplier=1.25x. At health=150, multiplier=1.0x.
    // Rapid oscillation could create arbitrage opportunities.

    double m_149 = DynamicCollateralAdjustment::GetDCAMultiplier(149);
    double m_150 = DynamicCollateralAdjustment::GetDCAMultiplier(150);

    BOOST_CHECK_EQUAL(m_149, 1.25); // Warning tier
    BOOST_CHECK_EQUAL(m_150, 1.0);  // Healthy tier

    // An attacker could:
    // 1. Wait until health is at 150% (1.0x)
    // 2. Mint a large position, dropping health to 149% (now 1.25x for everyone else)
    // 3. Other users must pay 25% more collateral
    // 4. Attacker's position was locked in at the 1.0x rate
    //
    // SEVERITY: LOW-MEDIUM — The attacker's own mint pushes health down,
    // but the DCA only applies to NEW mints, not existing positions.
    // The attacker doesn't directly profit, but griefs other users.

    // Test boundary sensitivity
    double m_119 = DynamicCollateralAdjustment::GetDCAMultiplier(119);
    double m_120 = DynamicCollateralAdjustment::GetDCAMultiplier(120);
    BOOST_CHECK_EQUAL(m_119, 1.5);  // Critical
    BOOST_CHECK_EQUAL(m_120, 1.25); // Warning

    double m_99 = DynamicCollateralAdjustment::GetDCAMultiplier(99);
    double m_100 = DynamicCollateralAdjustment::GetDCAMultiplier(100);
    BOOST_CHECK_EQUAL(m_99, 2.0);   // Emergency
    BOOST_CHECK_EQUAL(m_100, 2.0);  // Emergency floor
}

// ============================================================================
// ATTACK VECTOR 2: ERR Manipulation
// Can ERR be gamed to trigger/prevent minting freezes?
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_02_err_manipulation_trigger_freeze)
{
    // Attack: Can an attacker intentionally trigger ERR to freeze minting?
    // ERR activates at health < 100%. ERR blocks new mints.

    BOOST_CHECK(EmergencyRedemptionRatio::ShouldActivateERR(99));
    BOOST_CHECK(!EmergencyRedemptionRatio::ShouldActivateERR(100));

    // Attack scenario: Attacker has a large position. They mass-redeem to drop
    // system health below 100%, triggering ERR and freezing all new mints.
    //
    // FINDING: This is actually a design feature — ERR *should* block mints
    // when system is under-collateralized. But an attacker could weaponize it:
    // 1. Accumulate large DD positions
    // 2. Wait for DGB price drop
    // 3. Mass-redeem, dropping health below 100%
    // 4. ERR freezes new mints — system becomes illiquid
    // 5. Attacker profits from DD price instability (if DD is traded externally)
    //
    // SEVERITY: MEDIUM — Requires significant capital and a price drop catalyst.
}

BOOST_AUTO_TEST_CASE(rh13_02_err_dd_burn_requirements)
{
    // Test ERR DD burn requirements for potential gaming
    CAmount original = 10000;  // $100

    // At 95% health: must burn 105.3% (10527 for $100 due to ceiling)
    CAmount burn_95 = EmergencyRedemptionRatio::GetRequiredDDBurn(original, 95);
    BOOST_CHECK(burn_95 > original);
    BOOST_CHECK(burn_95 <= 10527);  // ceil(10000/0.95) = 10527

    // At 80% health: must burn 125%
    CAmount burn_80 = EmergencyRedemptionRatio::GetRequiredDDBurn(original, 80);
    BOOST_CHECK_EQUAL(burn_80, 12500);  // ceil(10000/0.80)

    // VULNERABILITY: ERR burn at <85% health caps at 0.80 ratio (125% burn).
    // At 50% health and 10% health, the burn multiplier is the same.
    // This means at extreme undercollateralization, the penalty doesn't increase.
    CAmount burn_50 = EmergencyRedemptionRatio::GetRequiredDDBurn(original, 50);
    CAmount burn_10 = EmergencyRedemptionRatio::GetRequiredDDBurn(original, 10);
    BOOST_CHECK_EQUAL(burn_50, burn_80);  // Same! Cap at 125%
    BOOST_CHECK_EQUAL(burn_10, burn_80);

    // FINDING: 125% max burn may be too lenient for extreme scenarios.
    // At 50% health, the system has $50 backing per $100 DD. A 125% burn
    // means the redeemer needs 125 DD to get 100 DD's worth of collateral.
    // The extra 25 DD is effectively burned, helping reduce supply.
    // But is 25% penalty enough to deter bank runs at 50% collateralization?
    // SEVERITY: LOW — Design decision, not a bug.
}

BOOST_AUTO_TEST_CASE(rh13_02_err_overflow_on_large_burn)
{
    // Attack: Can we overflow the DD burn calculation with extreme values?
    CAmount maxDD = std::numeric_limits<CAmount>::max();

    // This should not overflow — GetRequiredDDBurn uses double with overflow guard
    CAmount burn = EmergencyRedemptionRatio::GetRequiredDDBurn(maxDD, 80);
    // Should be capped at CAmount max, not wrap around
    BOOST_CHECK(burn > 0);
    BOOST_CHECK(burn >= maxDD);  // At 80% ratio, result >= input
}

// ============================================================================
// ATTACK VECTOR 3: Collateral Tier Gaming
// Different lock periods have different collateral ratios.
// Can you exploit the tier boundaries?
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_03_tier_boundary_gaming)
{
    ConsensusParams params;

    // Tier boundaries:
    // 240 blocks (1hr): 1000%
    // 172800 blocks (30d): 500%
    // 518400 blocks (90d): 400%

    // Attack: Lock for 241 blocks instead of 240. V1 must reject it instead
    // of giving it the cheaper 30-day collateral ratio.
    int ratio_240 = GetCollateralRatioForLockTime(240, params);
    int ratio_241 = GetCollateralRatioForLockTime(241, params);

    BOOST_CHECK_EQUAL(ratio_240, 1000);  // Exact match: 1000%
    BOOST_CHECK_EQUAL(ratio_241, 0);
    BOOST_CHECK(!IsCanonicalLockTier(241, params));

    // Check what ratio a 1-block lock gets (below minimum)
    int ratio_1 = GetCollateralRatioForLockTime(1, params);
    BOOST_CHECK_EQUAL(ratio_1, 0);
    BOOST_CHECK(!IsCanonicalLockTier(1, params));
}

BOOST_AUTO_TEST_CASE(rh13_03_exact_boundary_values)
{
    ConsensusParams params;

    // Test every exact boundary
    int ratio_exact_1hr = GetCollateralRatioForLockTime(240, params);
    int ratio_exact_30d = GetCollateralRatioForLockTime(30 * 5760, params);
    int ratio_exact_90d = GetCollateralRatioForLockTime(90 * 5760, params);
    int ratio_exact_180d = GetCollateralRatioForLockTime(180 * 5760, params);
    int ratio_exact_1yr = GetCollateralRatioForLockTime(365 * 5760, params);
    int ratio_exact_10yr = GetCollateralRatioForLockTime(3650 * 5760, params);

    BOOST_CHECK_EQUAL(ratio_exact_1hr, 1000);
    BOOST_CHECK_EQUAL(ratio_exact_30d, 500);
    BOOST_CHECK_EQUAL(ratio_exact_90d, 400);
    BOOST_CHECK_EQUAL(ratio_exact_180d, 350);
    BOOST_CHECK_EQUAL(ratio_exact_1yr, 300);
    BOOST_CHECK_EQUAL(ratio_exact_10yr, 200);

    // Test lock time beyond max tier.
    int ratio_20yr = GetCollateralRatioForLockTime(7300 * 5760, params);
    BOOST_CHECK_EQUAL(ratio_20yr, 0);
    BOOST_CHECK(!IsCanonicalLockTier(7300 * 5760, params));
}

// ============================================================================
// ATTACK VECTOR 4: Liquidation Cascade
// If price drops sharply, can mass redemptions cause a cascading failure?
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_04_liquidation_cascade_health_calculation)
{
    // Simulate a price crash and track system health

    CAmount totalCollateral = 1000 * COIN;  // 1000 DGB locked
    CAmount totalDD = 1000000;  // $10,000 DD (1M cents)

    // Health at $0.05/DGB: (1000 * 50000 millicents / COIN) / totalDD * 100
    // = (1000 * 100000000 * 50000 / 100000000) / 1000000 * 100
    // = 50000000 / 100000000 ... let me use the actual function

    // Price at $50 (healthy): 50 * 100000 = 5000000 millicents
    int health_50 = DynamicCollateralAdjustment::CalculateSystemHealth(
        totalCollateral, totalDD, 5000000);

    // Price at $5 (crash): 5 * 100000 = 500000 millicents
    int health_5 = DynamicCollateralAdjustment::CalculateSystemHealth(
        totalCollateral, totalDD, 500000);

    // Price at $0.50 (90% crash): 0.50 * 100000 = 50000 millicents
    int health_050 = DynamicCollateralAdjustment::CalculateSystemHealth(
        totalCollateral, totalDD, 50000);

    BOOST_CHECK(health_50 > 150);   // Should be healthy at $50
    BOOST_CHECK(health_5 < health_50); // Price drop reduces health
    BOOST_CHECK(health_050 < 100);  // Should trigger ERR at $0.50

    // FINDING: Health is directly proportional to price.
    // A 90% price drop causes a ~90% health drop.
    // There's no buffer or dampening mechanism beyond volatility freezes.
    // The cascade would be:
    // 1. Price drops 50% → health drops from 300% to 150%
    // 2. DCA kicks in (warning level at 120-150%)
    // 3. Price drops another 20% → health drops to ~120%
    // 4. DCA at 1.5x (critical)
    // 5. Minters flee → redemptions reduce supply but also collateral
    // 6. ERR at <100% → minting frozen, burn penalty applies

    // The volatile freeze mechanism should help prevent panic:
    // 20% in 1 hour → freeze mints, 30% in 24 hours → freeze all
    // But: freeze duration is only 144 blocks (~36 hours)
    // A prolonged decline over days could still cause cascading failure.
}

// ============================================================================
// ATTACK VECTOR 5: Supply Cap Boundary
// What happens at exactly ALERT_DD_SUPPLY? Off-by-one errors?
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_05_supply_cap_boundary)
{
    // ALERT_DD_SUPPLY = 10000000000 (100M DD = $100M in cents)

    // FINDING: ALERT_DD_SUPPLY is only used in AlertThresholds for monitoring.
    // It is NOT enforced as a consensus supply cap!
    //
    // CheckSupplyAlert returns true when supply > ALERT_DD_SUPPLY, but this
    // is an alert, not a rejection. There's no ValidateSupplyCap() or
    // similar function that rejects transactions.
    //
    // ValidateMintAmount checks individual mint limits ($100-$100K),
    // but NOT cumulative supply.
    //
    // VULNERABILITY: There is NO consensus-enforced supply cap.
    // The system can theoretically mint infinite DD as long as:
    // 1. Individual mints are $100-$100K
    // 2. Sufficient collateral is provided
    // 3. Oracle price is available
    //
    // SEVERITY: MEDIUM — This is a design choice (supply is naturally limited
    // by available DGB collateral), but worth explicit documentation.
    // If DGB market cap is $1B and average collateral ratio is 300%,
    // max DD supply is ~$333M.

    CAmount maxSupply = DigiDollar::AlertThresholds::ALERT_DD_SUPPLY;
    BOOST_CHECK_EQUAL(maxSupply, 10000000000);  // 100M DD

    // Verify alert triggers at boundary
    DigiDollar::SystemMetrics metrics;
    metrics.totalDDSupply = maxSupply;
    // CheckSupplyAlert is private, but we can test via ShouldAlert
    // For now, just document the finding.
}

// ============================================================================
// ATTACK VECTOR 6: Economic Griefing (UTXO Bloat)
// Mint minimum DD amounts to bloat the UTXO set
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_06_utxo_bloat_griefing)
{
    ConsensusParams params;

    // Minimum mint: $100 (10000 cents)
    // Minimum output: $1 (100 cents)
    BOOST_CHECK_EQUAL(params.minMintAmount, 10000);
    BOOST_CHECK_EQUAL(params.minOutputAmount, 100);

    // Attack: Mint $100, then split via transfers into 100 x $1 outputs
    // Each mint creates: 1 collateral UTXO + 1 DD UTXO + 1 OP_RETURN = 3 outputs
    // Each transfer creates: N DD UTXOs + 1 OP_RETURN

    // Cost analysis at $0.05/DGB:
    // Mint $100 DD at 1000% (1hr lock): need $1000 worth of DGB = 20,000 DGB
    // At 500% (30d lock): need $500 worth = 10,000 DGB
    // Plus transaction fees

    // Then transfer to 100 x $1 outputs = 100 UTXOs from one mint
    // After lock expires, redeem to get DGB back

    // FINDING: The $100 minimum mint limits direct UTXO creation,
    // but $1 minimum output allows splitting into small UTXOs via transfers.
    // An attacker with $10K could create 10,000 UTXOs.
    //
    // MITIGATION: Each transfer requires a fee, so bloating has a cost.
    // Also, each DD UTXO is small (P2TR = 34 bytes + overhead).
    //
    // SEVERITY: LOW — Standard UTXO bloat attack with standard mitigations.
    // The $1 minimum output is the key control. Consider raising if bloat
    // becomes an issue.

    // Verify transfer creates valid outputs at minimum
    // minOutputAmount of 100 cents = $1
    BOOST_CHECK(params.minOutputAmount >= 100);
}

// ============================================================================
// ATTACK VECTOR 7: Oracle Quorum Manipulation
// With the launch chainparams (7 signatures from active keys in a 35-slot roster),
// can colluding oracles manipulate the system?
// This test exercises the DEFAULT ConsensusParams struct values
// (src/consensus/digidollar.h): 35 active slots, 7 threshold.
// Consensus::Params tracks the same 7-signature threshold per chain.
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_07_oracle_quorum_manipulation)
{
    ConsensusParams params;

    // Oracle struct defaults in src/consensus/digidollar.h.
    BOOST_CHECK_EQUAL(params.oracleCount, 35);
    BOOST_CHECK_EQUAL(params.activeOracles, 35);
    BOOST_CHECK_EQUAL(params.oracleThreshold, 7);

    // Attack 1: Can N colluding oracles set arbitrary price?
    // YES — the threshold is the bar. Colluders meeting it control the price.
    //
    // But: Oracle activation is gated by hardcoded public keys and inactive slots
    // cannot sign until a coordinated release adds their x-only keys.
    //
    // FINDING: ValidateConsensusParams ensures the threshold is non-zero and does
    // not exceed the active key roster. Chainparams set the launch
    // threshold to 7 signatures from the active keys.

    // Verify the configured launch threshold.
    BOOST_CHECK_GT(params.oracleThreshold, 0U);
    BOOST_CHECK_LE(params.oracleThreshold, params.activeOracles);
    BOOST_CHECK_EQUAL(params.oracleThreshold, 7);
    BOOST_CHECK_EQUAL(params.activeOracles, 35);
}

// ============================================================================
// ATTACK VECTOR 8: Epoch Boundary Arbitrage
// Mint at end of epoch when price is stale, redeem at start of next when fresh
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_08_epoch_boundary_arbitrage)
{
    ConsensusParams params;

    // Price valid for 20 blocks (5 minutes at 15s blocks)
    BOOST_CHECK_EQUAL(params.priceValidBlocks, 20);

    // Stale oracle threshold: 100 blocks
    BOOST_CHECK_EQUAL(DigiDollar::AlertThresholds::STALE_ORACLE_BLOCKS, 100);

    // Attack scenario:
    // 1. Oracle epoch is 1440 blocks (24 hours)
    // 2. Oracle update interval is 4 blocks (1 minute)
    // 3. Near end of epoch, price may be up to 4 blocks stale
    // 4. If DGB price has moved significantly in those 4 blocks...
    //
    // Actually: prices are valid for 20 blocks (5 min).
    // An attacker could:
    // 1. See DGB price dropping on external exchanges
    // 2. Mint DD using the stale (higher) oracle price → get more DD per DGB
    // 3. Wait for oracle to update with lower price
    // 4. The position is now undercollateralized relative to reality
    //
    // FINDING: The 20-block price validity window creates a ~5 minute
    // stale price window. In high volatility, 5 minutes can be significant.
    // However, the volatility freeze mechanism (20% in 1hr freezes mints)
    // should catch extreme moves.
    //
    // SEVERITY: LOW-MEDIUM — Natural limitation of on-chain price feeds.
    // The 5-minute window is reasonable for a 15-second block time chain.
}

// ============================================================================
// ATTACK VECTOR 9: Health Metric Overflow
// Can system health calculations overflow with extreme inputs?
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_09_health_calculation_overflow)
{
    // Test with maximum possible values
    CAmount maxMoney = MAX_MONEY;  // ~21B DGB in satoshis
    CAmount maxPrice = std::numeric_limits<CAmount>::max();

    // Absurd prices outside valid oracle bounds must fail closed before
    // multiplication can overflow.
    int health = DynamicCollateralAdjustment::CalculateSystemHealth(
        maxMoney, 1, maxPrice);  // Massive collateral, tiny DD

    BOOST_CHECK_EQUAL(health, 0);
}

BOOST_AUTO_TEST_CASE(rh13_09_health_calculation_tiny_dd)
{
    // Edge case: 1 cent of DD with massive collateral
    int health = DynamicCollateralAdjustment::CalculateSystemHealth(
        1000 * COIN, 1, 5000000);  // 1000 DGB, 1 cent DD, $50/DGB

    // health = (1000 * COIN * 5000000 / COIN) / 1000 * 100 / 1
    // = 5000000 * 100 = 500000000 ... should be capped at 30000
    BOOST_CHECK_EQUAL(health, 30000);
}

BOOST_AUTO_TEST_CASE(rh13_09_health_calculation_zero_dd_division)
{
    // The DGB-SEC-003 fix addresses totalDD between 1-999 with overflow path
    // When collateralValueCents > maxSafeDividend AND totalDD/1000 == 0

    int health = DynamicCollateralAdjustment::CalculateSystemHealth(
        MAX_MONEY, 500, MAX_MONEY);  // Extreme values

    // Should not crash and should return capped value
    BOOST_CHECK(health >= 0);
    BOOST_CHECK(health <= 30000);
}

BOOST_AUTO_TEST_CASE(rh13_09_health_utils_overflow)
{
    // Test HealthUtils::CalculateHealthRatio with extreme values
    // Uses util::int128_t, should handle large inputs

    int ratio = DigiDollar::HealthUtils::CalculateHealthRatio(
        1, MAX_MONEY, std::numeric_limits<CAmount>::max());

    // Should be capped at 300
    BOOST_CHECK(ratio >= 0);
    BOOST_CHECK(ratio <= 300);

    // Test with zero DD (division by zero guard)
    int ratio_zero = DigiDollar::HealthUtils::CalculateHealthRatio(0, 1000, 50);
    BOOST_CHECK_EQUAL(ratio_zero, 300);  // Perfect health if no DD

    // Test with zero price
    int ratio_no_price = DigiDollar::HealthUtils::CalculateHealthRatio(1000, 1000, 0);
    BOOST_CHECK_EQUAL(ratio_no_price, 0);  // Can't calculate without price
}

// ============================================================================
// ATTACK VECTOR 10: Lock Period Exploitation
// Mint with longest lock (lowest ratio), immediately sell DD, let collateral sit
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_10_lock_period_exploitation)
{
    ConsensusParams params;

    // Attack: Lock for 10 years at 200% ratio to maximize DD minted per DGB.
    // Then immediately sell the DD on the market.
    // The DGB is locked for 10 years — you've effectively leveraged your DGB.

    int ratio_10yr = GetCollateralRatioForLockTime(3650 * 5760, params);
    int ratio_1hr = GetCollateralRatioForLockTime(240, params);

    BOOST_CHECK_EQUAL(ratio_10yr, 200);   // 2x collateral for 10yr lock
    BOOST_CHECK_EQUAL(ratio_1hr, 1000);   // 10x collateral for 1hr lock

    // With $1000 worth of DGB at 200%, you mint $500 DD
    // With $1000 worth of DGB at 1000%, you mint $100 DD
    // The 10-year lock gives 5x more DD per DGB!

    // Is this an exploit? No — it's the intended economic model.
    // Longer lock = more trust in the system = lower collateral needed.
    // The attacker bears the risk of DGB price dropping over 10 years
    // while their collateral is locked.

    // BUT: What if DGB goes to zero?
    // The DD holder is left with worthless DD (no collateral backing).
    // This is the fundamental risk of ANY collateralized stablecoin.

    // FINDING: The tier model is working as intended, but there's an
    // asymmetric risk: DD holders bear the risk of DGB declining,
    // while DGB lockers benefit if DGB appreciates (they get their
    // DGB back after lock period, plus they had DD to use).
    //
    // This creates rational incentive to mint only when you're bullish on DGB.
    // If everyone is bearish, nobody mints, and the system contracts naturally.

    // More interesting attack: Mint at 10yr/200%, price goes up 5x,
    // now your 200% collateral is worth 1000%. Your DD is backed 10x.
    // You can't redeem early (locked). But: you got $500 DD for $1000 DGB.
    // If DGB 5x'd, your $1000 DGB is now $5000 — locked for 10 years.
    // Opportunity cost is enormous.
    //
    // SEVERITY: NOT A VULNERABILITY — Intended economic design.
}

// ============================================================================
// ADDITIONAL FINDINGS: DCA + ERR Interaction
// ============================================================================

BOOST_AUTO_TEST_CASE(rh13_extra_dca_err_interaction)
{
    // FINDING: DCA and ERR use DIFFERENT health tier definitions!
    //
    // DCA tiers (from dca.cpp HEALTH_TIERS):
    //   Emergency floor: 0-109, Critical: 110-119, Warning: 120-149, Healthy: 150+
    //
    // ERR tiers (from err.cpp ERR_TIERS):
    //   Activates at < 100%
    //   0.95 ratio: 95-100, 0.90 ratio: 90-95, 0.85 ratio: 85-90, 0.80 ratio: <85
    //
    // ConsensusParams dcaLevels:
    //   >150%: 100 mult, 120-150%: 125 mult, 110-120%: 150 mult, <110%: 200 mult
    //
    // DCA is now aligned with ConsensusParams, while ERR still uses its own
    // redemption burn schedule.
    //
    // GetDCAMultiplier (DCA class) and ConsensusParams should agree.
    //
    // Which one is used in validation? GetEffectiveCollateralRatio in validation.cpp
    // calls DCA::DynamicCollateralAdjustment::GetDCAMultiplier — the class version.
    // This test protects the previously confusing boundary behavior.

    // Verify the class DCA is what's actually used
    double class_multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(115);
    // Class: 110-119 = critical = 1.5x
    BOOST_CHECK_EQUAL(class_multiplier, 1.5);

    // ConsensusParams would give: 110-120 = 150/100 = 1.5x
    // Same result here:
    double class_at_110 = DynamicCollateralAdjustment::GetDCAMultiplier(110);
    BOOST_CHECK_EQUAL(class_at_110, 1.5);
    double class_at_109 = DynamicCollateralAdjustment::GetDCAMultiplier(109);
    BOOST_CHECK_EQUAL(class_at_109, 2.0);
}

BOOST_AUTO_TEST_CASE(rh13_extra_no_supply_cap_enforcement)
{
    // Verify there's no consensus-level supply cap check
    // Search through validation flow:
    //
    // ValidateMintTransaction checks:
    //   - ValidateMintAmount (per-tx limits: $100-$100K) ✓
    //   - Collateral sufficiency ✓
    //   - Oracle price ✓
    //   - Volatility freeze ✓
    //   - NUMS verification ✓
    //
    // NOT checked:
    //   - Total system DD supply < ALERT_DD_SUPPLY ✗
    //
    // ALERT_DD_SUPPLY is only in AlertThresholds, used for monitoring alerts.
    //
    // FINDING CONFIRMED: No consensus supply cap exists.
    // This should be documented as an intentional design decision or
    // a supply cap should be added to ValidateMintTransaction.
}

BOOST_AUTO_TEST_CASE(rh13_extra_incremental_tracking_desync)
{
    // The health system uses incremental tracking:
    // OnMintConnected: totalDDSupply += ddAmount
    // OnRedeemConnected: totalDDSupply = max(0, totalDDSupply - ddAmount)
    //
    // Attack: Can reorg cause desync between tracked supply and actual UTXO set?
    //
    // OnMintDisconnected reverses a mint: supply -= ddAmount (clamped to 0)
    // OnRedeemDisconnected reverses a redeem: supply += ddAmount
    //
    // If a reorg happens: disconnect block N+1, reconnect block N+1'
    // The disconnect should reverse, then reconnect should re-apply.
    //
    // POTENTIAL ISSUE: If ddAmount differs between disconnect and reconnect
    // (shouldn't happen unless implementation bug), supply could desync.
    //
    // Also: the clamping to 0 (std::max<CAmount>(0, ...)) means if supply
    // ever goes negative due to a bug, it silently becomes 0 instead of
    // crashing. This could hide errors.

    DigiDollar::SystemHealthMonitor::ResetMetrics();

    // Simulate: mint 1000, mint 2000, redeem 1000
    DigiDollar::SystemHealthMonitor::OnMintConnected(1000, 10 * COIN);
    DigiDollar::SystemHealthMonitor::OnMintConnected(2000, 20 * COIN);
    DigiDollar::SystemHealthMonitor::OnRedeemConnected(1000, 10 * COIN);

    auto metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(metrics.totalDDSupply, 2000);
    BOOST_CHECK_EQUAL(metrics.totalCollateral, 20 * COIN);

    // Now disconnect the redeem (reorg)
    DigiDollar::SystemHealthMonitor::OnRedeemDisconnected(1000, 10 * COIN);
    metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(metrics.totalDDSupply, 3000);  // Restored
    BOOST_CHECK_EQUAL(metrics.totalCollateral, 30 * COIN);

    // Disconnect both mints (deeper reorg)
    DigiDollar::SystemHealthMonitor::OnMintDisconnected(2000, 20 * COIN);
    DigiDollar::SystemHealthMonitor::OnMintDisconnected(1000, 10 * COIN);
    metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(metrics.totalDDSupply, 0);
    BOOST_CHECK_EQUAL(metrics.totalCollateral, 0);

    // Test the clamping: disconnect more than exists
    DigiDollar::SystemHealthMonitor::OnMintDisconnected(5000, 50 * COIN);
    metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(metrics.totalDDSupply, 0);  // Clamped, not negative
    BOOST_CHECK_EQUAL(metrics.totalCollateral, 0);

    // FINDING: Clamping to 0 silently absorbs over-disconnects.
    // In normal operation this shouldn't happen. But if it does,
    // subsequent operations will have incorrect supply tracking.
    // Consider adding an assertion or log warning when clamping triggers.

    DigiDollar::SystemHealthMonitor::ResetMetrics();
}

BOOST_AUTO_TEST_SUITE_END()
