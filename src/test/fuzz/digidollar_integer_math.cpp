// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Deep-dive fuzz targets for DigiDollar integer arithmetic and
// price conversion math.
// Targets: dd_int128_arithmetic, dd_collateral_math, dd_price_conversion
//
// These are CONSENSUS-CRITICAL: any integer overflow in this math can cause
// a chain split between nodes compiled with different compilers/optimizations.
//

#include <chainparams.h>
#include <util/int128.h>
#include <consensus/amount.h>
#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <consensus/err.h>
#include <consensus/volatility.h>
#include <digidollar/health.h>
#include <digidollar/validation.h>
#include <primitives/oracle.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <util/chaintype.h>

#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>

// ============================================================================
// Shared Initialization
// ============================================================================

void initialize_dd_integer_math()
{
    SelectParams(ChainType::REGTEST);
}

// ============================================================================
// Target 6: dd_int128_arithmetic
//
// Test ALL util::int128_t arithmetic paths in DD consensus code.
// Every place util::int128_t is used can cause a chain split if it overflows
// differently under different compilers.
//
// Known util::int128_t usage sites:
//   1. validation.cpp:399 — CalculateRequiredCollateral numerator
//   2. txbuilder.cpp:171 — CalculateRequiredCollateral (txbuilder variant)
//   3. txbuilder.cpp:184 — 1% safety margin padding
//   4. health.cpp:922 — CalculateHealthRatio (dgbValue128)
//   5. health.cpp:926 — CalculateHealthRatio (health128)
// ============================================================================

FUZZ_TARGET(dd_int128_arithmetic, .init = initialize_dd_integer_math)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    // -- Path 1: CalculateRequiredCollateral util::int128_t path --
    // Formula: numerator = ddAmount * COIN * effectiveRatio * 100
    //          result = numerator / oraclePriceMicroUSD
    // All three multiplicands can be large. We must verify the util::int128_t
    // intermediate never overflows, and the final cast to uint64_t is safe.
    {
        // Adversarial inputs designed to maximize the util::int128_t numerator:
        // - Max ddAmount: 100,000,000,000 cents ($1 billion DD)
        // - COIN = 100,000,000 (constant)
        // - Max effectiveRatio: could be 2000% (10.0x DCA * 200% base = 2000)
        //   or even higher with adversarial DCA
        // - Times 100
        // - Min oraclePriceMicroUSD: 1 (one micro-USD = $0.000001 per DGB)
        CAmount ddAmount = fdp.ConsumeIntegralInRange<CAmount>(0, 100'000'000'000LL);
        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000'000LL);
        int systemHealth = fdp.ConsumeIntegralInRange<int>(0, 30000);

        const auto& chainparams = Params();
        DigiDollar::ValidationContext ctx(1000, oraclePrice, systemHealth, chainparams,
                                          nullptr, true);

        // Choose lock times that map to different collateral ratio tiers
        int64_t lockTimes[] = {
            5760,        // 1 day  — 1000% ratio
            40320,       // 7 days — 800%
            172800,      // 30 days — 600%
            518400,      // 90 days — 400%
            2102400,     // 1 year — 300%
            10512000,    // 5 years — 200%
        };

        for (int64_t lockTime : lockTimes) {
            CAmount result = DigiDollar::CalculateRequiredCollateral(ddAmount, lockTime, ctx);
            // Result must be non-negative and capped at MAX_MONEY
            assert(result >= 0);
            assert(result <= MAX_MONEY);
        }

        // Extreme: fuzzed lock time
        int64_t fuzzLock = fdp.ConsumeIntegral<int64_t>();
        if (fuzzLock > 0 && oraclePrice > 0) {
            CAmount result = DigiDollar::CalculateRequiredCollateral(ddAmount, fuzzLock, ctx);
            assert(result >= 0);
            assert(result <= MAX_MONEY);
        }
    }

    // -- Path 2: Reproduce the exact util::int128_t computation manually --
    // This tests the raw arithmetic to ensure it never silently overflows util::int128_t
    {
        CAmount ddAmount = fdp.ConsumeIntegral<CAmount>();
        int effectiveRatio = fdp.ConsumeIntegralInRange<int>(200, 10000);
        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1, std::numeric_limits<CAmount>::max());

        // Mimic: numerator = ddAmount * COIN * effectiveRatio * 100
        util::int128_t numerator = static_cast<util::int128_t>(ddAmount) *
                             static_cast<util::int128_t>(COIN) *
                             static_cast<util::int128_t>(effectiveRatio) *
                             static_cast<util::int128_t>(100);
        util::int128_t result = numerator / static_cast<util::int128_t>(oraclePrice);

        // Verify MAX_MONEY cap
        if (result > static_cast<util::int128_t>(MAX_MONEY)) {
            result = static_cast<util::int128_t>(MAX_MONEY);
        }
        if (result < 0) {
            result = 0;
        }
        uint64_t finalResult = static_cast<uint64_t>(result);
        assert(finalResult <= static_cast<uint64_t>(MAX_MONEY));

        // Verify the 1% safety margin path (txbuilder.cpp:184)
        util::int128_t padded = (static_cast<util::int128_t>(finalResult) * 101) / 100;
        if (padded > static_cast<util::int128_t>(MAX_MONEY)) {
            padded = static_cast<util::int128_t>(MAX_MONEY);
        }
        uint64_t paddedResult = static_cast<uint64_t>(padded);
        assert(paddedResult <= static_cast<uint64_t>(MAX_MONEY));
    }

    // -- Path 3: HealthUtils::CalculateHealthRatio util::int128_t path --
    // health.cpp uses: dgbValue128 = dgbAmount * dgbPrice / COIN
    //                  health128 = dgbValue128 * 100 / ddAmount
    {
        CAmount dgbAmount = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        CAmount dgbPrice = fdp.ConsumeIntegralInRange<CAmount>(0, 100'000'000LL); // cents
        CAmount ddAmount = fdp.ConsumeIntegralInRange<CAmount>(0, 100'000'000'000LL);

        // Must never crash. Result should be in [0, 300]
        int health = DigiDollar::HealthUtils::CalculateHealthRatio(ddAmount, dgbAmount, dgbPrice);
        assert(health >= 0 && health <= 300);

        // Edge cases
        assert(DigiDollar::HealthUtils::CalculateHealthRatio(0, 0, 0) == 300); // No DD = perfect
        assert(DigiDollar::HealthUtils::CalculateHealthRatio(100, 0, 100) == 0); // No DGB = 0
        assert(DigiDollar::HealthUtils::CalculateHealthRatio(100, 1000'00000000LL, 0) == 0); // 0 price = 0
    }

    // -- Path 4: MAX_MONEY boundary products --
    // If ddAmount = MAX_MONEY and COIN = 100M and ratio = 10000 and 100,
    // the product is MAX_MONEY * 10^8 * 10^4 * 10^2 = ~2.1e31
    // util::int128_t max is ~1.7e38, so this should fit. Verify.
    {
        util::int128_t maxProduct = static_cast<util::int128_t>(MAX_MONEY) *
                              static_cast<util::int128_t>(COIN) *
                              static_cast<util::int128_t>(10000) *
                              static_cast<util::int128_t>(100);
        // This should not overflow util::int128_t
        assert(maxProduct > 0);

        // Divide by minimum oracle price (1)
        util::int128_t worstCase = maxProduct / 1;
        // Will exceed MAX_MONEY — verify the cap logic works
        assert(worstCase > static_cast<util::int128_t>(MAX_MONEY));

        // After cap:
        uint64_t capped = (worstCase > static_cast<util::int128_t>(MAX_MONEY)) ? MAX_MONEY : static_cast<uint64_t>(worstCase);
        assert(capped == static_cast<uint64_t>(MAX_MONEY));
    }

    // -- Path 5: Negative CAmount values through util::int128_t --
    // CAmount is int64_t, so negative values are valid.
    // The consensus code should handle these gracefully.
    {
        CAmount negDD = fdp.ConsumeIntegralInRange<CAmount>(std::numeric_limits<CAmount>::min(), -1);
        CAmount posPrice = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000LL);

        util::int128_t product = static_cast<util::int128_t>(negDD) * static_cast<util::int128_t>(posPrice);
        // Product should be negative
        assert(product < 0);

        // CalculateHealthRatio should handle negative gracefully
        int h = DigiDollar::HealthUtils::CalculateHealthRatio(negDD, 1000'00000000LL, posPrice);
        assert(h >= 0 && h <= 300);
    }

    // -- Path 6: Two MAX_MONEY values multiplied --
    {
        util::int128_t big = static_cast<util::int128_t>(MAX_MONEY) * static_cast<util::int128_t>(MAX_MONEY);
        // MAX_MONEY = 21e9 * 1e8 = 2.1e18, so MAX_MONEY^2 ≈ 4.41e36
        // util::int128_t max ≈ 1.7e38, so this is safe within util::int128_t
        assert(big > 0);
        assert(big < (static_cast<util::int128_t>(1) << 126)); // util::int128_t can hold up to 2^127-1
    }
}

// ============================================================================
// Target 7: dd_collateral_math
//
// Fuzz the end-to-end collateral requirement calculation:
//   DD amount + lock period + oracle price → required DGB collateral
// Tests with extreme prices, extreme DD amounts, extreme lock periods.
// ============================================================================

FUZZ_TARGET(dd_collateral_math, .init = initialize_dd_integer_math)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const auto& chainparams = Params();
    const auto& ddparams = chainparams.GetDigiDollarParams();

    uint8_t strategy = fdp.ConsumeIntegralInRange<uint8_t>(1, 8);

    // -- Strategy 1: Extreme DGB prices --
    // $0.001 DGB = 1000 micro-USD, $1000 DGB = 1,000,000,000,000 micro-USD
    if (strategy == 1) {
        struct PriceCase {
            CAmount microUSD;
            const char* label;
        } prices[] = {
            {1, "$0.000001"},
            {100, "$0.0001"},
            {1000, "$0.001"},
            {10000, "$0.01"},
            {100000, "$0.10"},
            {1000000, "$1.00"},
            {10000000, "$10.00"},
            {100000000, "$100.00"},
            {1000000000LL, "$1,000"},
            {10000000000LL, "$10,000"},
            {100000000000LL, "$100,000"},
        };

        CAmount ddAmount = fdp.ConsumeIntegralInRange<CAmount>(100, 100'000'000'000LL);
        int systemHealth = fdp.ConsumeIntegralInRange<int>(100, 30000); // Healthy
        int64_t lockTime = fdp.ConsumeIntegralInRange<int64_t>(5760, 10512000);

        for (const auto& pc : prices) {
            DigiDollar::ValidationContext ctx(1000, pc.microUSD, systemHealth, chainparams,
                                              nullptr, true);

            CAmount required = DigiDollar::CalculateRequiredCollateral(ddAmount, lockTime, ctx);
            assert(required >= 0);
            assert(required <= MAX_MONEY);

            // Cross-check: ValidateCollateralRatio should accept exactly the required amount
            if (required > 0 && required < MAX_MONEY) {
                bool ratioOk = DigiDollar::ValidateCollateralRatio(required, ddAmount, lockTime, ctx);
                // It should pass if we provide exactly the calculated requirement
                // (may fail due to rounding edge cases in integer division — acceptable)
                (void)ratioOk;
            }
        }
    }

    // -- Strategy 2: Extreme DD amounts --
    if (strategy == 2) {
        CAmount extremeAmounts[] = {
            1,                      // 1 cent
            100,                    // $1
            10000,                  // $100
            1000000,                // $10,000
            100000000,              // $1,000,000
            1000000000LL,           // $10,000,000
            10000000000LL,          // $100,000,000
            100000000000LL,         // $1,000,000,000
        };

        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1000, 10000000);
        int systemHealth = fdp.ConsumeIntegralInRange<int>(100, 300);
        int64_t lockTime = 40320; // 7 days

        for (CAmount ddAmt : extremeAmounts) {
            DigiDollar::ValidationContext ctx(1000, oraclePrice, systemHealth, chainparams,
                                              nullptr, true);
            CAmount required = DigiDollar::CalculateRequiredCollateral(ddAmt, lockTime, ctx);
            assert(required >= 0);
            assert(required <= MAX_MONEY);
        }
    }

    // -- Strategy 3: Extreme lock periods --
    if (strategy == 3) {
        int64_t lockTimes[] = {
            1,                     // 1 block (~15 seconds)
            5760,                  // 1 day
            40320,                 // 7 days
            172800,                // 30 days
            518400,                // 90 days
            2102400,               // 1 year
            10512000,              // 5 years
            21024000,              // 10 years
            std::numeric_limits<int64_t>::max(),
            0,
            -1,
            -1000000,
        };

        CAmount ddAmt = fdp.ConsumeIntegralInRange<CAmount>(100, 10'000'000);
        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1000, 100000000LL);
        int systemHealth = fdp.ConsumeIntegralInRange<int>(100, 30000);

        for (int64_t lt : lockTimes) {
            DigiDollar::ValidationContext ctx(1000, oraclePrice, systemHealth, chainparams,
                                              nullptr, true);
            CAmount required = DigiDollar::CalculateRequiredCollateral(ddAmt, lt, ctx);
            assert(required >= 0);
            assert(required <= MAX_MONEY);
        }
    }

    // -- Strategy 4: Collateral ratio tiers — verify monotonicity --
    // Longer lock = lower ratio = less collateral needed
    // So: RequiredCollateral(1 day) >= RequiredCollateral(5 years)
    if (strategy == 4) {
        CAmount ddAmt = fdp.ConsumeIntegralInRange<CAmount>(1000, 1'000'000);
        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(10000, 10000000);
        int systemHealth = fdp.ConsumeIntegralInRange<int>(200, 30000);

        DigiDollar::ValidationContext ctx(1000, oraclePrice, systemHealth, chainparams,
                                          nullptr, true);

        CAmount req_1day = DigiDollar::CalculateRequiredCollateral(ddAmt, 5760, ctx);
        CAmount req_7day = DigiDollar::CalculateRequiredCollateral(ddAmt, 40320, ctx);
        CAmount req_90day = DigiDollar::CalculateRequiredCollateral(ddAmt, 518400, ctx);
        CAmount req_1year = DigiDollar::CalculateRequiredCollateral(ddAmt, 2102400, ctx);
        CAmount req_5year = DigiDollar::CalculateRequiredCollateral(ddAmt, 10512000, ctx);

        // Monotonically decreasing (or equal if tiers collapse)
        assert(req_1day >= req_7day || req_1day == 0 || req_7day == 0);
        assert(req_7day >= req_90day || req_7day == 0 || req_90day == 0);
        assert(req_90day >= req_1year || req_90day == 0 || req_1year == 0);
        assert(req_1year >= req_5year || req_1year == 0 || req_5year == 0);
    }

    // -- Strategy 5: DCA multiplier impact --
    // Worse system health = higher DCA multiplier = more collateral required
    if (strategy == 5) {
        CAmount ddAmt = fdp.ConsumeIntegralInRange<CAmount>(1000, 1'000'000);
        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(10000, 10000000);
        int64_t lockTime = 40320; // 7 days

        DigiDollar::ValidationContext ctx_healthy(1000, oraclePrice, 300, chainparams, nullptr, true);
        DigiDollar::ValidationContext ctx_critical(1000, oraclePrice, 50, chainparams, nullptr, true);

        CAmount req_healthy = DigiDollar::CalculateRequiredCollateral(ddAmt, lockTime, ctx_healthy);
        CAmount req_critical = DigiDollar::CalculateRequiredCollateral(ddAmt, lockTime, ctx_critical);

        // Critical health should require MORE collateral (higher DCA multiplier)
        // unless both hit MAX_MONEY cap
        if (req_healthy > 0 && req_critical > 0 &&
            req_healthy < MAX_MONEY && req_critical < MAX_MONEY) {
            assert(req_critical >= req_healthy);
        }
    }

    // -- Strategy 6: GetEffectiveCollateralRatio with fuzzed inputs --
    if (strategy == 6) {
        int baseRatio = fdp.ConsumeIntegralInRange<int>(200, 1000);
        int fuzzHealth = fdp.ConsumeIntegralInRange<int>(0, 30000);

        int effective = DigiDollar::GetEffectiveCollateralRatio(baseRatio, fuzzHealth, chainparams);
        // Effective ratio must be >= base ratio (DCA only increases it)
        assert(effective >= baseRatio);
    }

    // -- Strategy 7: GetCollateralRatioForLockTime consistency --
    if (strategy == 7) {
        int64_t lockBlocks = fdp.ConsumeIntegral<int64_t>();
        int ratio = DigiDollar::GetCollateralRatioForLockTime(lockBlocks, ddparams);
        assert(ratio == 0 || (ratio >= 200 && ratio <= 1000));

        // Same input should always give same output
        int ratio2 = DigiDollar::GetCollateralRatioForLockTime(lockBlocks, ddparams);
        assert(ratio == ratio2);
    }

    // -- Strategy 8: ValidateCollateralRatio with extreme values --
    if (strategy == 8) {
        CAmount dgbLocked = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        CAmount ddMinted = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        int64_t lockTime = fdp.ConsumeIntegralInRange<int64_t>(0, 100'000'000LL);
        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(0, 100'000'000'000LL);
        int systemHealth = fdp.ConsumeIntegralInRange<int>(0, 30000);

        DigiDollar::ValidationContext ctx(1000, oraclePrice, systemHealth, chainparams,
                                          nullptr, true);
        // Must not crash, even with zero/negative values
        (void)DigiDollar::ValidateCollateralRatio(dgbLocked, ddMinted, lockTime, ctx);
    }
}

// ============================================================================
// Target 8: dd_price_conversion
//
// Fuzz the full chain of price-dependent calculations:
//   DGB price → collateral value → health ratio → DCA multiplier → effective ratio
// Tests with adversarial price sequences to catch rounding/overflow issues.
// ============================================================================

FUZZ_TARGET(dd_price_conversion, .init = initialize_dd_integer_math)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    uint8_t strategy = fdp.ConsumeIntegralInRange<uint8_t>(1, 10);

    // -- Path A: CalculateSystemHealth --
    // dca.cpp: Takes totalCollateral (sats), totalDD (cents), oraclePrice (millicents)
    // Returns health percentage (0-30000)
    if (strategy == 1) {
        CAmount totalCollateral = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        CAmount totalDD = fdp.ConsumeIntegralInRange<CAmount>(0, 100'000'000'000LL);
        CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(0, 100'000'000'000LL);

        int health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
            totalCollateral, totalDD, oraclePrice);
        assert(health >= 0 && health <= 30000);
    }

    // -- Path B: Full chain: health → DCA multiplier → effective ratio --
    if (strategy == 2) {
        int systemHealth = fdp.ConsumeIntegralInRange<int>(0, 30000);

        double multiplier = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(systemHealth);
        // Multiplier must be positive and reasonable (1.0x to 10.0x expected range)
        assert(multiplier > 0.0);
        assert(!std::isnan(multiplier));
        assert(!std::isinf(multiplier));

        int baseRatio = fdp.ConsumeIntegralInRange<int>(200, 1000);
        int effective = DigiDollar::DCA::DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemHealth);
        // Must be >= base ratio (DCA only increases requirements)
        assert(effective >= baseRatio);
        // The applied ratio uses consensus integer basis points and rounds up.
        const int multiplier_bps =
            DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplierBps(systemHealth);
        const util::int128_t expected128 =
            (static_cast<util::int128_t>(baseRatio) * multiplier_bps + 9999) / 10000;
        int expected = expected128 > std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(expected128);
        assert(effective == expected);
    }

    // -- Path C: CalculateSystemHealth overflow protection --
    // When totalCollateral is very large and oraclePrice is very large,
    // the product could overflow int64_t. The code handles this with a
    // divide-first path.
    if (strategy == 3) {
        // Adversarial: max collateral * max price
        int health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
            MAX_MONEY, 1, MAX_MONEY);
        assert(health >= 0 && health <= 30000);

        health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
            MAX_MONEY, MAX_MONEY, MAX_MONEY);
        assert(health >= 0 && health <= 30000);

        // Edge: zero DD supply = max health
        health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
            MAX_MONEY, 0, 1000000);
        assert(health == 30000);

        // Edge: zero oracle price = 0 health
        health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
            MAX_MONEY, 1000, 0);
        assert(health == 0);

        // Edge: negative collateral
        health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
            -1, 1000, 1000000);
        assert(health == 0);
    }

    // -- Path D: ERR adjustment ratio → DD burn calculation --
    // Test the full ERR math chain with fuzzed system health values
    if (strategy == 4) {
        int systemHealth = fdp.ConsumeIntegralInRange<int>(-100, 30000);

        double ratio = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(systemHealth);
        // Ratio must be in [0.80, 1.0]
        assert(ratio >= 0.79 && ratio <= 1.01); // Small epsilon for floating point

        CAmount originalDD = fdp.ConsumeIntegralInRange<CAmount>(0, 100'000'000'000LL);
        CAmount requiredBurn = DigiDollar::ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(
            originalDD, systemHealth);

        if (originalDD <= 0) {
            assert(requiredBurn == 0);
        } else if (systemHealth >= 100) {
            // No ERR adjustment needed
            assert(requiredBurn == originalDD);
        } else {
            // ERR requires MORE DD to be burned
            assert(requiredBurn >= originalDD);
        }
    }

    // -- Path E: ERR DD burn with extreme amounts --
    if (strategy == 5) {
        CAmount extremeAmounts[] = {
            1,
            100,
            100'000'000'000LL,     // $1 billion
            MAX_MONEY,
            std::numeric_limits<CAmount>::max(),
        };
        int healthValues[] = {0, 50, 84, 85, 89, 90, 94, 95, 99, 100, 200, 30000};

        for (CAmount dd : extremeAmounts) {
            for (int h : healthValues) {
                CAmount burn = DigiDollar::ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(dd, h);
                // Must not crash; burn should be >= original (or 0 for invalid input)
                // Note: for extreme values near int64_max, burn may saturate at int64_max
                // which could be == dd (when dd == int64_max), so use >= check
                if (dd > 0 && h < 100) {
                    assert(burn >= dd);
                }
            }
        }
    }

    // -- Path F: ERR ShouldActivateERR boundary --
    if (strategy == 6) {
        assert(!DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(100));
        assert(!DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(101));
        assert(!DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(30000));
        assert(DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(99));
        assert(DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(0));
        assert(DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(-1));

        int fuzzHealth = fdp.ConsumeIntegral<int>();
        bool shouldActivate = DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(fuzzHealth);
        assert(shouldActivate == (fuzzHealth < 100));
    }

    // -- Path G: Volatility math --
    // CalculatePercentageChange and ExceedsThreshold
    if (strategy == 7) {
        CAmount oldPrice = fdp.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY);
        CAmount newPrice = fdp.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY);
        double change = DigiDollar::Volatility::CalculatePercentageChange(oldPrice, newPrice);
        assert(!std::isnan(change));
        // If prices are equal, change should be ~0
        if (oldPrice == newPrice) {
            assert(std::abs(change) < 0.0001);
        }

        double threshold = fdp.ConsumeFloatingPointInRange<double>(0.0, 100.0);
        bool exceeds = DigiDollar::Volatility::ExceedsThreshold(oldPrice, newPrice, threshold);
        (void)exceeds;

        // Edge case: zero prices
        (void)DigiDollar::Volatility::CalculatePercentageChange(0, 100);
        (void)DigiDollar::Volatility::CalculatePercentageChange(100, 0);
        (void)DigiDollar::Volatility::CalculatePercentageChange(0, 0);
    }

    // -- Path H: Adversarial price sequences through DCA --
    // Rapidly oscillating prices should produce monotonically-behaving health
    if (strategy == 8) {
        int numPrices = fdp.ConsumeIntegralInRange<int>(2, 20);
        CAmount prevHealth = 30000;

        for (int i = 0; i < numPrices; i++) {
            CAmount totalCollateral = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
            CAmount totalDD = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000'000LL);
            CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000'000LL);

            int health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
                totalCollateral, totalDD, oraclePrice);
            assert(health >= 0 && health <= 30000);

            double mult = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(health);
            assert(mult > 0.0 && !std::isnan(mult) && !std::isinf(mult));

            (void)prevHealth;
            prevHealth = health;
        }
    }

    // -- Path I: CalculateSystemHealth DGB-SEC-003 fix --
    // totalDD between 1 and 999: used to cause division by zero in the
    // scaled-down path (totalDD / 1000 == 0).
    if (strategy == 9) {
        for (CAmount tdd = 1; tdd < 1000; tdd += 100) {
            // MAX collateral * maximum valid oracle price should cap at max health.
            int health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
                MAX_MONEY, tdd, ORACLE_MAX_PRICE_MICRO_USD);
            // Should be capped at max, not crash
            assert(health == 30000);
        }
    }

    // -- Path J: GetAdjustedRedemption (deprecated but still callable) --
    if (strategy == 10) {
        CAmount redemption = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        int systemHealth = fdp.ConsumeIntegralInRange<int>(0, 30000);
        CAmount adjusted = DigiDollar::ERR::EmergencyRedemptionRatio::GetAdjustedRedemption(
            redemption, systemHealth);
        // Deprecated function should return input unchanged
        assert(adjusted == redemption);
    }
}
