// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-32: Deep red-team of Collateral + DCA interaction attack surface
 *
 * FINDINGS SUMMARY:
 * [RH-32-F1] DUAL DCA SYSTEMS — ConsensusParams::dcaLevels is dead code; actual
 *            validation uses DCA::HEALTH_TIERS. Ranges and multipliers differ.
 *            Not exploitable (correct system is used), but confusing and fragile.
 * [RH-32-F2] TRUNCATION IN ApplyDCA — uses static_cast<int>(baseRatio * multiplier)
 *            which truncates. E.g., 225 * 1.25 = 281.25, but floating-point
 *            could yield 269.999... → 269. Attacker saves 1% collateral on some tiers.
 * [RH-32-F3] COLLATERAL RETURN 0 FOR OVERFLOW — CalculateRequiredCollateral (txbuilder)
 *            returns 0 when result > MAX_MONEY. This is correctly caught by caller
 *            (checks <= 0), but the validation.cpp version caps at MAX_MONEY instead.
 *            Inconsistent behavior between TxBuilder (wallet) and consensus validation.
 * [RH-32-F4] ERR + DCA DO NOT COMPOUND — ERR blocks minting entirely when active.
 *            DCA only applies to minting. So they never stack. This is CORRECT behavior.
 * [RH-32-F5] HEALTH CALCULATION PRECISION LOSS — CalculateSystemHealth scales down
 *            both numerator and denominator by /1000 when large. For totalDD between
 *            1000-1999, this loses ~50% precision in the health calculation.
 * [RH-32-F6] DCA TIER BOUNDARY GAMING — An attacker can observe system health at 150%
 *            (1.0x) vs 149% (1.25x) and time mints to just after health dips below 150%
 *            then immediately back. No hysteresis exists. This is by design but worth noting.
 */

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <consensus/err.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/validation.h>

#include <cmath>
#include <limits>

using namespace DigiDollar;
using namespace DigiDollar::DCA;
using namespace DigiDollar::ERR;

BOOST_AUTO_TEST_SUITE(digidollar_rh32_collateral_dca_tests)

// ============================================================================
// Attack Vector 1: DCA tier manipulation via specific DD amounts
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_tier_boundary_manipulation)
{
    // An attacker crafts DD amounts to exploit tier boundaries for lower collateral.
    // At health=150 (healthy, 1.0x) vs health=149 (warning, 1.25x), the jump is 25%.
    // Verify the boundary is sharp and cannot be gamed with fractional health values.

    // Exact boundary: 150 = healthy (1.0x)
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(150), 1.0);
    // Just below: 149 = warning (1.25x)
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(149), 1.25);

    // Exact boundary: 120 = warning (1.25x)
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(120), 1.25);
    // Just below: 119 = critical (1.5x)
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(119), 1.5);

    // Exact boundary: 110 = critical (1.5x)
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(110), 1.5);
    // Just below: 109 = emergency floor (2.0x)
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(109), 2.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(99), 2.0);

    // [RH-32-F6] No hysteresis — verify immediate transitions both directions
    // Attacker manipulates health: 151 → 149 → 151
    double m1 = DynamicCollateralAdjustment::GetDCAMultiplier(151);
    double m2 = DynamicCollateralAdjustment::GetDCAMultiplier(149);
    double m3 = DynamicCollateralAdjustment::GetDCAMultiplier(151);
    BOOST_CHECK_EQUAL(m1, 1.0);
    BOOST_CHECK_EQUAL(m2, 1.25);
    BOOST_CHECK_EQUAL(m3, 1.0); // Immediately back to 1.0x — no hysteresis

    // Verify ApplyDCA at boundaries with all base ratios
    std::vector<int> baseRatios = {1000, 500, 400, 350, 300, 275, 250, 225, 212, 200};
    for (int base : baseRatios) {
        int at150 = DynamicCollateralAdjustment::ApplyDCA(base, 150);
        int at149 = DynamicCollateralAdjustment::ApplyDCA(base, 149);
        // The jump should be exactly 25% more at 149 vs 150
        // [RH-32-F2] Fractional DCA multipliers must round up so the final ratio
        // never undercuts the intended collateral requirement.
        double expected149 = base * 1.25;
        BOOST_CHECK_EQUAL(at150, base); // 1.0x = no change
        BOOST_CHECK_EQUAL(at149, static_cast<int>(std::ceil(expected149)));
    }
}

BOOST_AUTO_TEST_CASE(rh32_applydca_truncation_attack)
{
    // [RH-32-F2] Test floating-point truncation in ApplyDCA
    // ApplyDCA does: static_cast<int>(baseRatio * multiplier)
    // This truncates rather than rounds. For most values this is fine,
    // but edge cases could lose a percentage point.

    // 225 * 1.25 = 281.25 -> must round up to 282
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(225, 149), 282);

    // 212 * 1.25 = 265.0
    int result = DynamicCollateralAdjustment::ApplyDCA(212, 149);
    BOOST_CHECK_EQUAL(result, 265);

    // 275 * 1.5 = 412.5 → must round up to 413
    result = DynamicCollateralAdjustment::ApplyDCA(275, 119);
    BOOST_CHECK_EQUAL(result, 413);

    // 212 * 1.5 = 318.0 (exact)
    result = DynamicCollateralAdjustment::ApplyDCA(212, 119);
    BOOST_CHECK_EQUAL(result, 318);

    // 225 * 2.0 = 450.0 (exact)
    result = DynamicCollateralAdjustment::ApplyDCA(225, 50);
    BOOST_CHECK_EQUAL(result, 450);

    // Worst case examples now use the 1.25x warning multiplier and must round up.
    // Not exploitable for significant value extraction, but worth documenting.
}

// ============================================================================
// Attack Vector 2: util::int128_t edge cases in collateral calculation
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_int128_max_money_collateral)
{
    // Test CalculateSystemHealth with extreme values that previously caused overflow
    // MAX_MONEY = 21B * COIN = 2,100,000,000 * 100,000,000 = 2.1e17

    CAmount maxCollateral = MAX_MONEY; // ~2.1e17 sats
    CAmount smallDD = 100;             // $1.00
    CAmount highPrice = 10000000;      // High bounded price, still safe to multiply

    // Should not crash or return garbage; bounded util::int128_t math caps health.
    int health = DynamicCollateralAdjustment::CalculateSystemHealth(maxCollateral, smallDD, highPrice);
    // With massive collateral and tiny DD, health should be capped at 30000
    BOOST_CHECK_EQUAL(health, 30000);

    // Reverse: tiny collateral, massive DD supply
    CAmount tinyCollateral = 1;                    // 1 satoshi
    CAmount massiveDD = 2100000000000LL;           // MAX_DIGIDOLLAR = 21B * 100
    CAmount normalPrice = 5000;                    // $0.05 = 5000 millicents

    health = DynamicCollateralAdjustment::CalculateSystemHealth(tinyCollateral, massiveDD, normalPrice);
    BOOST_CHECK_EQUAL(health, 0); // Essentially zero collateral
}

BOOST_AUTO_TEST_CASE(rh32_int128_validation_collateral_calc)
{
    // Test the consensus validation CalculateRequiredCollateral with extreme values
    SelectParams(ChainType::REGTEST);
    const CChainParams& chainparams = Params();

    ValidationContext ctx(1000, 6310, 150, chainparams);

    // Max DD amount ($100k = 10,000,000 cents) with 1-hour tier (1000%)
    CAmount maxDD = 10000000;
    int64_t oneHourBlocks = 240;

    CAmount required = CalculateRequiredCollateral(maxDD, oneHourBlocks, ctx);
    BOOST_CHECK_GT(required, 0);
    BOOST_CHECK_LE(required, MAX_MONEY);

    // Extreme: MAX_DIGIDOLLAR with minimum price — should cap at MAX_MONEY
    ValidationContext ctx2(1000, 1, 150, chainparams);
    CAmount extremeDD = 2100000000000LL; // 21B DD
    required = CalculateRequiredCollateral(extremeDD, oneHourBlocks, ctx2);
    // Should be capped at MAX_MONEY, not overflow to garbage
    BOOST_CHECK_LE(required, MAX_MONEY);
    BOOST_CHECK_GE(required, 0);
}

// ============================================================================
// Attack Vector 3: Zero-amount DD mint
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_zero_amount_mint)
{
    // Attempt to mint 0 DD — should be rejected at every layer

    // Layer 1: CalculateRequiredCollateral returns 0 for ddAmount <= 0
    SelectParams(ChainType::REGTEST);
    const CChainParams& chainparams = Params();

    ValidationContext ctx(1000, 6310, 150, chainparams);

    CAmount required = CalculateRequiredCollateral(0, 240, ctx);
    BOOST_CHECK_EQUAL(required, 0);

    // Negative amount
    required = CalculateRequiredCollateral(-100, 240, ctx);
    BOOST_CHECK_EQUAL(required, 0);

    // Layer 2: ValidateMintAmount should reject 0
    BOOST_CHECK(!ValidateMintAmount(0, chainparams, 1000));
    BOOST_CHECK(!ValidateMintAmount(-1, chainparams, 1000));

    // Layer 3: IsValidMintAmount (consensus) should reject 0
    const auto& ddParams = chainparams.GetDigiDollarParams();
    BOOST_CHECK(!IsValidMintAmount(0, ddParams));
    BOOST_CHECK(!IsValidMintAmount(-1, ddParams));

    // Layer 4: Below minimum (0 cents) should be rejected
    // Regtest minMintAmount = 1 cent, so 0 is below minimum
    BOOST_CHECK(!IsValidMintAmount(0, ddParams));
    BOOST_CHECK(!ValidateMintAmount(0, chainparams, 1000));
}

// ============================================================================
// Attack Vector 4: Fractional satoshi rounding
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_fractional_satoshi_rounding)
{
    // Test collateral calculation for small DD amounts near minimum
    // Looking for cases where rounding gives less collateral than expected
    SelectParams(ChainType::REGTEST);
    const CChainParams& chainparams = Params();

    ValidationContext ctx(1000, 0, 150, chainparams);

    // At very high DGB price, small DD amounts could require < 1 satoshi
    // $100 DD at $1000/DGB with 200% ratio = $200 worth of DGB = 0.2 DGB = 20M sats
    CAmount minDD = 10000; // $100 minimum
    int64_t tenYearBlocks = 10 * 365 * 24 * 60 * 4;

    // At $1000/DGB
    ValidationContext ctx1(1000, 1000000000, 150, chainparams);
    CAmount required = CalculateRequiredCollateral(minDD, tenYearBlocks, ctx1);
    BOOST_CHECK_GT(required, 0);

    // At $10,000/DGB with minimum DD
    ValidationContext ctx2(1000, 10000000000LL, 150, chainparams);
    required = CalculateRequiredCollateral(minDD, tenYearBlocks, ctx2);
    BOOST_CHECK_GT(required, 0);

    // At $100,000/DGB (extreme)
    ValidationContext ctx3(1000, 100000000000LL, 150, chainparams);
    required = CalculateRequiredCollateral(minDD, tenYearBlocks, ctx3);
    BOOST_CHECK_GT(required, 0);
}

BOOST_AUTO_TEST_CASE(rh32_required_collateral_rounds_up)
{
    // The consensus collateral requirement must round fractional satoshis up.
    // Otherwise a mint can be accepted one satoshi below the intended ratio.
    SelectParams(ChainType::REGTEST);
    const CChainParams& chainparams = Params();

    const CAmount ddAmount = 10000; // $100
    const int64_t oneHourBlocks = 240;
    ValidationContext ctx(1000, 6310, 150, chainparams);

    const int effectiveRatio = GetEffectiveCollateralRatio(1000, 150, chainparams);
    util::int128_t numerator = static_cast<util::int128_t>(ddAmount) * COIN * effectiveRatio * 100;
    util::int128_t expectedCeil128 = (numerator + ctx.oraclePriceMicroUSD - 1) / ctx.oraclePriceMicroUSD;
    CAmount expectedCeil = static_cast<CAmount>(expectedCeil128);

    CAmount required = CalculateRequiredCollateral(ddAmount, oneHourBlocks, ctx);
    BOOST_CHECK_EQUAL(required, expectedCeil);
    BOOST_CHECK_MESSAGE(!ValidateCollateralRatio(expectedCeil - 1, ddAmount, oneHourBlocks, ctx),
                        "one satoshi below the rounded-up requirement must reject");
    BOOST_CHECK(ValidateCollateralRatio(expectedCeil, ddAmount, oneHourBlocks, ctx));
}

// ============================================================================
// Attack Vector 5: DCA tier lookup with duplicate timestamps
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_duplicate_timestamp_mints)
{
    // Multiple mints in the same block hit CalculateSystemHealth identically.
    // The DCA system is stateless per-call — it just reads system health.
    // Verify that the same health value always produces the same multiplier
    // regardless of how many times it's called.

    for (int health = 0; health <= 300; ++health) {
        double m1 = DynamicCollateralAdjustment::GetDCAMultiplier(health);
        double m2 = DynamicCollateralAdjustment::GetDCAMultiplier(health);
        BOOST_CHECK_EQUAL(m1, m2);
    }

    // Verify CalculateSystemHealth is deterministic with same inputs
    CAmount collateral = 50 * COIN;
    CAmount dd = 5000; // $50
    CAmount price = 5000; // $0.05

    int h1 = DynamicCollateralAdjustment::CalculateSystemHealth(collateral, dd, price);
    int h2 = DynamicCollateralAdjustment::CalculateSystemHealth(collateral, dd, price);
    BOOST_CHECK_EQUAL(h1, h2);
}

// ============================================================================
// Attack Vector 6: Collateral unlock timing edge cases
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_collateral_unlock_boundary)
{
    // Test LockDaysToBlocks for all tier boundaries
    // Verify no off-by-one in lock period calculation

    // Special case: 0 days = 1 hour = 240 blocks
    BOOST_CHECK_EQUAL(LockDaysToBlocks(0), 240);

    // Standard tiers
    BOOST_CHECK_EQUAL(LockDaysToBlocks(30), 30 * 5760);
    BOOST_CHECK_EQUAL(LockDaysToBlocks(90), 90 * 5760);
    BOOST_CHECK_EQUAL(LockDaysToBlocks(365), 365 * 5760);
    BOOST_CHECK_EQUAL(LockDaysToBlocks(3650), 3650 * 5760);

    // BlocksToLockDays should round-trip (except for 0-day special case)
    BOOST_CHECK_EQUAL(BlocksToLockDays(LockDaysToBlocks(30)), 30);
    BOOST_CHECK_EQUAL(BlocksToLockDays(LockDaysToBlocks(365)), 365);

    // Edge: 1 block before unlock should still be locked
    // This is enforced by CLTV in the script, not by this code,
    // but verify the lock height calculation is correct
    int64_t lockBlocks = LockDaysToBlocks(30);
    BOOST_CHECK_EQUAL(lockBlocks, 172800); // 30 * 5760
}

// ============================================================================
// Attack Vector 7: Cross-tier atomic exploitation
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_cross_tier_ratio_lookup)
{
    // V1 rejects durations between defined tiers. This prevents attackers from
    // picking custom lock periods that get rounded into a more favorable tier.

    SelectParams(ChainType::REGTEST);
    const auto& ddParams = Params().GetDigiDollarParams();

    // Between 1-hour (240 blocks) and 30-day (172800 blocks)
    int64_t fifteenDays = 15 * 5760; // 86400 blocks
    int ratio = GetCollateralRatioForLockTime(fifteenDays, ddParams);
    BOOST_CHECK_EQUAL(ratio, 0);

    // Exactly at 30-day boundary
    ratio = GetCollateralRatioForLockTime(172800, ddParams);
    BOOST_CHECK_EQUAL(ratio, 500);

    // 1 block more than 30 days is non-canonical.
    ratio = GetCollateralRatioForLockTime(172801, ddParams);
    BOOST_CHECK_EQUAL(ratio, 0);

    // Longer than 10-year tier is non-canonical.
    int64_t elevenYears = 11 * 365 * 5760;
    ratio = GetCollateralRatioForLockTime(elevenYears, ddParams);
    BOOST_CHECK_EQUAL(ratio, 0);

    // 1 block is shorter than the canonical 1-hour tier.
    ratio = GetCollateralRatioForLockTime(1, ddParams);
    BOOST_CHECK_EQUAL(ratio, 0);
}

// ============================================================================
// Attack Vector 8: DCA window manipulation (whale attack)
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_whale_health_manipulation)
{
    // A whale floods mints to shift system health for everyone.
    // Verify health calculation is proportional and cannot be gamed with small amounts.

    CAmount baseCollateral = 1000000 * COIN; // 1M DGB
    CAmount baseDD = 10000000;               // $100k DD
    CAmount price = 5000;                    // $0.05 = 5000 millicents

    int baseHealth = DynamicCollateralAdjustment::CalculateSystemHealth(
        baseCollateral, baseDD, price);

    // Whale adds massive DD without proportional collateral
    // 1M DGB collateral but 10M DD ($100k worth) at $0.05/DGB
    // Collateral value = 1M * $0.05 = $50,000
    // DD value = $100,000
    // Health = ($50k / $100k) * 100 = 50%
    BOOST_CHECK_LT(baseHealth, 100); // Under-collateralized

    // Now with proper collateral: 10M DGB for same DD
    CAmount goodCollateral = 10000000 * COIN;
    int goodHealth = DynamicCollateralAdjustment::CalculateSystemHealth(
        goodCollateral, baseDD, price);
    // 10M * $0.05 = $500k / $100k = 500% → capped at 30000
    BOOST_CHECK_GE(goodHealth, 150); // Healthy

    // Key insight: health is a global metric. A whale adding under-collateralized
    // positions drags down health for EVERYONE, increasing DCA multiplier for
    // new mints. This is working as designed — it protects the system.
}

// ============================================================================
// Attack Vector 9: ERR + DCA compounding
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_err_dca_no_compounding)
{
    // [RH-32-F4] Verify ERR and DCA cannot compound.
    // When ERR is active (health < 100%), minting is blocked entirely.
    // So DCA multiplier is irrelevant during ERR — you can't mint at all.

    // At health=80%, ERR should be active
    BOOST_CHECK(EmergencyRedemptionRatio::ShouldActivateERR(80));

    // DCA at health=80% gives 2.0x multiplier (emergency tier)
    double dcaMultiplier = DynamicCollateralAdjustment::GetDCAMultiplier(80);
    BOOST_CHECK_EQUAL(dcaMultiplier, 2.0);

    // ERR at health=80% gives adjustment ratio of 0.85 (85-90% tier... wait, 80% < 85%)
    double errRatio = EmergencyRedemptionRatio::CalculateERRAdjustment(80);
    BOOST_CHECK_EQUAL(errRatio, 0.80); // <85% = 0.80

    // Activate ERR state so ShouldBlockMinting() returns true
    EmergencyRedemptionRatio::SetActiveForTesting(true);

    // But minting is BLOCKED during ERR, so DCA * ERR never compounds
    BOOST_CHECK(EmergencyRedemptionRatio::ShouldBlockMinting());

    // ERR only affects redemptions (DD burn amount), DCA only affects minting (collateral)
    // They operate on different transaction types — no compounding possible.

    // Verify ERR required burn at various health levels
    CAmount originalDD = 100000; // $1000
    CAmount burn80 = EmergencyRedemptionRatio::GetRequiredDDBurn(originalDD, 80);
    // 100000 / 0.80 = 125000
    BOOST_CHECK_EQUAL(burn80, 125000);

    CAmount burn90 = EmergencyRedemptionRatio::GetRequiredDDBurn(originalDD, 90);
    // 100000 / 0.90 = 111112 (ceil)
    BOOST_CHECK_EQUAL(burn90, 111112);

    CAmount burn95 = EmergencyRedemptionRatio::GetRequiredDDBurn(originalDD, 95);
    // 100000 / 0.95 = 105264 (ceil)
    BOOST_CHECK_EQUAL(burn95, 105264);

    // At health >= 100, no extra burn
    CAmount burn100 = EmergencyRedemptionRatio::GetRequiredDDBurn(originalDD, 100);
    BOOST_CHECK_EQUAL(burn100, originalDD);

    // Clean up ERR state
    EmergencyRedemptionRatio::ResetForTesting();
}

// ============================================================================
// [RH-32-F5] Health calculation precision loss
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_health_calculation_precision)
{
    // Test the scaling path in CalculateSystemHealth for large collateral values

    // Normal path: collateralValueCents <= maxSafeDividend
    CAmount normalCollateral = 100 * COIN;
    CAmount normalDD = 10000; // $100
    CAmount normalPrice = 5000; // 5000 millicents = $0.05

    int health = DynamicCollateralAdjustment::CalculateSystemHealth(
        normalCollateral, normalDD, normalPrice);
    // 100 DGB * $0.05 = $5.00 = 500 cents
    // health = (500 * 100) / 10000 = 5%
    BOOST_CHECK_EQUAL(health, 5);

    // [RH-32-F5] Scaling path: when collateralValueCents > maxSafeDividend
    // This triggers the /1000 scaling on both numerator and denominator.
    // For totalDD between 1000-1999, totalDD/1000 = 1, losing precision.
    // The DGB-SEC-003 fix handles totalDD < 1000 (returns 30000).
    // But for totalDD = 1000: scaledDD = 1, still works but with less precision.

    // totalDD = 999 (< 1000): should return 30000 per DGB-SEC-003 fix
    // This is the case where collateral is massive and DD is tiny
    CAmount hugeCollateral = MAX_MONEY;
    CAmount tinyDD = 999;
    CAmount hugePrice = 100000; // $1.00 = 100000 millicents

    health = DynamicCollateralAdjustment::CalculateSystemHealth(
        hugeCollateral, tinyDD, hugePrice);
    // Massive collateral, tiny DD → should be capped at 30000
    BOOST_CHECK_EQUAL(health, 30000);

    // totalDD = 1000: scaledDD = 1
    CAmount dd1000 = 1000;
    health = DynamicCollateralAdjustment::CalculateSystemHealth(
        hugeCollateral, dd1000, hugePrice);
    // Should still cap at 30000
    BOOST_CHECK_EQUAL(health, 30000);

    // Edge case: oraclePrice = 0 should return 0, not crash
    health = DynamicCollateralAdjustment::CalculateSystemHealth(
        normalCollateral, normalDD, 0);
    BOOST_CHECK_EQUAL(health, 0);

    // Edge case: totalDD = 0 should return 30000 (no liabilities)
    health = DynamicCollateralAdjustment::CalculateSystemHealth(
        normalCollateral, 0, normalPrice);
    BOOST_CHECK_EQUAL(health, 30000);

    // Edge case: negative values
    health = DynamicCollateralAdjustment::CalculateSystemHealth(-1, normalDD, normalPrice);
    BOOST_CHECK_EQUAL(health, 0);
    health = DynamicCollateralAdjustment::CalculateSystemHealth(normalCollateral, -1, normalPrice);
    BOOST_CHECK_EQUAL(health, 0);
}

// ============================================================================
// [RH-32-F1] Dual DCA system consistency check
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_dual_dca_system_divergence)
{
    // ConsensusParams::dcaLevels and DCA::HEALTH_TIERS define different tiers.
    // Verify that actual validation uses DCA::HEALTH_TIERS (the correct one).

    SelectParams(ChainType::REGTEST);
    const CChainParams& chainparams = Params();

    // GetEffectiveCollateralRatio calls DCA::DynamicCollateralAdjustment::GetDCAMultiplier
    // which uses HEALTH_TIERS, NOT ConsensusParams::dcaLevels.

    // At health=130%: DCA::HEALTH_TIERS says "warning" (1.25x)
    // ConsensusParams::dcaLevels says >120% = 1.25x
    int base = 300;
    int effective = GetEffectiveCollateralRatio(base, 130, chainparams);
    BOOST_CHECK_EQUAL(effective, 375);

    // At health=115%: DCA::HEALTH_TIERS says "critical" (1.5x)
    // ConsensusParams::dcaLevels says 110-120% = 1.5x (same here by coincidence)
    effective = GetEffectiveCollateralRatio(base, 115, chainparams);
    BOOST_CHECK_EQUAL(effective, 450);

    // At health=105%: both validator and chainparams use the emergency floor.
    effective = GetEffectiveCollateralRatio(base, 105, chainparams);
    BOOST_CHECK_EQUAL(effective, 600);
}

// ============================================================================
// [RH-32-F3] Collateral overflow handling consistency
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_collateral_overflow_consistency)
{
    // TxBuilder::CalculateRequiredCollateral returns 0 on overflow (> MAX_MONEY)
    // validation.cpp::CalculateRequiredCollateral caps at MAX_MONEY
    // Verify the consensus validation path handles overflow correctly

    SelectParams(ChainType::REGTEST);
    const CChainParams& chainparams = Params();

    ValidationContext ctx(1000, 1, 50, chainparams);

    CAmount bigDD = 10000000; // $100k
    int64_t oneHourBlocks = 240; // 1000% base ratio * 2.0x DCA = 2000%

    CAmount required = CalculateRequiredCollateral(bigDD, oneHourBlocks, ctx);
    // With $0.000001 price and 2000% ratio, required DGB is astronomical
    // Should cap at MAX_MONEY, not wrap to garbage
    BOOST_CHECK_LE(required, MAX_MONEY);
    BOOST_CHECK_GE(required, 0);
}

// ============================================================================
// ERR burn calculation edge cases
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_err_burn_overflow)
{
    // Test GetRequiredDDBurn with extreme values
    // MAX_DIGIDOLLAR / 0.80 could overflow int64_t

    CAmount maxDD = 2100000000000LL; // MAX_DIGIDOLLAR
    CAmount burn = EmergencyRedemptionRatio::GetRequiredDDBurn(maxDD, 80);
    // 2.1T / 0.80 = 2.625T — fits in int64_t (max ~9.2e18)
    BOOST_CHECK_GT(burn, maxDD);
    BOOST_CHECK_LE(burn, std::numeric_limits<CAmount>::max());

    // Verify exact: ceil(2100000000000 / 0.80) = 2625000000000
    BOOST_CHECK_EQUAL(burn, 2625000000000LL);

    // Zero DD
    burn = EmergencyRedemptionRatio::GetRequiredDDBurn(0, 80);
    BOOST_CHECK_EQUAL(burn, 0);

    // Negative DD
    burn = EmergencyRedemptionRatio::GetRequiredDDBurn(-100, 80);
    BOOST_CHECK_EQUAL(burn, 0);

    // Healthy system — should return same amount
    burn = EmergencyRedemptionRatio::GetRequiredDDBurn(100000, 150);
    BOOST_CHECK_EQUAL(burn, 100000);
}

// ============================================================================
// DCA config validation
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_dca_config_validity)
{
    // Verify HEALTH_TIERS have no gaps or overlaps
    std::string error;
    bool valid = DynamicCollateralAdjustment::ValidateDCAConfig(error);
    BOOST_CHECK_MESSAGE(valid, "DCA config invalid: " + error);

    // Verify ERR config
    valid = EmergencyRedemptionRatio::ValidateERRConfig(error);
    BOOST_CHECK_MESSAGE(valid, "ERR config invalid: " + error);
}

// ============================================================================
// Negative health / extreme DCA behavior
// ============================================================================

BOOST_AUTO_TEST_CASE(rh32_negative_health_dca)
{
    // What happens if somehow system health goes negative?
    // DCA::HEALTH_TIERS emergency tier covers 0-99.
    // Negative values should fall through to emergency (2.0x).

    double m = DynamicCollateralAdjustment::GetDCAMultiplier(-1);
    BOOST_CHECK_EQUAL(m, 2.0); // Falls through all tiers, gets fallback

    m = DynamicCollateralAdjustment::GetDCAMultiplier(-1000);
    BOOST_CHECK_EQUAL(m, 2.0);

    m = DynamicCollateralAdjustment::GetDCAMultiplier(std::numeric_limits<int>::min());
    BOOST_CHECK_EQUAL(m, 2.0);

    // Very high health
    m = DynamicCollateralAdjustment::GetDCAMultiplier(30000);
    BOOST_CHECK_EQUAL(m, 1.0);

    m = DynamicCollateralAdjustment::GetDCAMultiplier(std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(m, 1.0);
}

BOOST_AUTO_TEST_SUITE_END()
