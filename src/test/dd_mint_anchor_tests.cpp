// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <consensus/dd_mint_anchor.h>
#include <consensus/volatility.h>
#include <primitives/oracle.h>
#include <test/util/setup_common.h>

#include <algorithm>
#include <vector>

#include <boost/test/unit_test.hpp>

using DigiDollar::MintAnchor::LowerMedian;
using DigiDollar::Volatility::ExceedsThresholdBps;
using DigiDollar::Volatility::VolatilityThresholds;

BOOST_FIXTURE_TEST_SUITE(dd_mint_anchor_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// LowerMedian: sorted ascending, element at index (n-1)/2; empty => 0.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(lower_median_empty_returns_zero)
{
    // Zero in-window samples => anchor 0 => the volatility gate passes
    // (bootstrap / oracle-drought self-expiry).
    BOOST_CHECK_EQUAL(LowerMedian({}), 0);
}

BOOST_AUTO_TEST_CASE(lower_median_single_element)
{
    BOOST_CHECK_EQUAL(LowerMedian({42}), 42);
    BOOST_CHECK_EQUAL(LowerMedian({500000}), 500000);
}

BOOST_AUTO_TEST_CASE(lower_median_odd_count)
{
    // n=3: sorted {10,20,30}, index (3-1)/2 = 1 => 20
    BOOST_CHECK_EQUAL(LowerMedian({30, 10, 20}), 20);
    // n=5: sorted {1,2,3,4,5}, index (5-1)/2 = 2 => 3
    BOOST_CHECK_EQUAL(LowerMedian({5, 1, 4, 2, 3}), 3);
}

BOOST_AUTO_TEST_CASE(lower_median_even_count_picks_lower)
{
    // Explicit pin of the LOWER-median convention for even n:
    // n=2: sorted {10,20}, index (2-1)/2 = 0 => 10 (NOT the mean 15)
    BOOST_CHECK_EQUAL(LowerMedian({20, 10}), 10);
    // n=4: sorted {10,20,30,40}, index (4-1)/2 = 1 => 20 (NOT 25)
    BOOST_CHECK_EQUAL(LowerMedian({40, 10, 30, 20}), 20);
    // n=6: sorted {1,2,3,4,5,6}, index (6-1)/2 = 2 => 3
    BOOST_CHECK_EQUAL(LowerMedian({6, 5, 4, 3, 2, 1}), 3);
}

BOOST_AUTO_TEST_CASE(lower_median_permutation_invariance)
{
    // The anchor must not depend on the order samples were collected
    // (newest-first ancestor walk): every permutation yields the same median.
    std::vector<CAmount> values{5, 1, 4, 2, 3};
    std::sort(values.begin(), values.end());
    do {
        BOOST_CHECK_EQUAL(LowerMedian(values), 3);
    } while (std::next_permutation(values.begin(), values.end()));
}

BOOST_AUTO_TEST_CASE(lower_median_duplicates)
{
    BOOST_CHECK_EQUAL(LowerMedian({7, 7, 7, 7}), 7);
    // sorted {1,2,2,3}, index 1 => 2
    BOOST_CHECK_EQUAL(LowerMedian({2, 3, 1, 2}), 2);
    // sorted {1,5,5}, index 1 => 5
    BOOST_CHECK_EQUAL(LowerMedian({5, 5, 1}), 5);
}

BOOST_AUTO_TEST_CASE(lower_median_large_values_no_overflow)
{
    // Values at/near ORACLE_MAX_PRICE_MICRO_USD ($100.00 = 100,000,000
    // micro-USD). LowerMedian selects (never sums), so no overflow is
    // possible; pin that with max-magnitude inputs anyway.
    const CAmount maxPrice = static_cast<CAmount>(ORACLE_MAX_PRICE_MICRO_USD);
    BOOST_CHECK_EQUAL(LowerMedian({maxPrice, maxPrice - 1, maxPrice}), maxPrice);
    BOOST_CHECK_EQUAL(LowerMedian({maxPrice, maxPrice, maxPrice}), maxPrice);
    // Full 15-sample window (nDDVolAnchorMaxSamples) of max-value prices.
    std::vector<CAmount> window(15, maxPrice);
    window[7] = maxPrice - 1;
    // sorted: maxPrice-1 first, then 14x maxPrice; index (15-1)/2 = 7 => maxPrice
    BOOST_CHECK_EQUAL(LowerMedian(window), maxPrice);
}

// ---------------------------------------------------------------------------
// Threshold boundary semantics via the existing consensus helper:
// ExceedsThresholdBps(anchor, candidate, FREEZE_MINT_1H_BPS) with
// FREEZE_MINT_1H_BPS = 2000 (20%, symmetric). Change is computed as
// delta * 10000 / anchor with integer division truncating toward zero,
// and the comparison is >= threshold.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(threshold_exactly_2000_bps_exceeds)
{
    static_assert(VolatilityThresholds::FREEZE_MINT_1H_BPS == 2000);
    // +20.00% exactly: 10000 -> 12000
    BOOST_CHECK(ExceedsThresholdBps(10000, 12000, VolatilityThresholds::FREEZE_MINT_1H_BPS));
    // -20.00% exactly: 10000 -> 8000 (symmetric)
    BOOST_CHECK(ExceedsThresholdBps(10000, 8000, VolatilityThresholds::FREEZE_MINT_1H_BPS));
}

BOOST_AUTO_TEST_CASE(threshold_1999_bps_does_not_exceed)
{
    // +19.99%: 10000 -> 11999 => 1999 bps < 2000 => passes
    BOOST_CHECK(!ExceedsThresholdBps(10000, 11999, VolatilityThresholds::FREEZE_MINT_1H_BPS));
    // -19.99%: 10000 -> 8001 => |-1999| bps < 2000 => passes
    BOOST_CHECK(!ExceedsThresholdBps(10000, 8001, VolatilityThresholds::FREEZE_MINT_1H_BPS));
}

BOOST_AUTO_TEST_CASE(threshold_truncation_around_odd_anchor)
{
    // Anchor 2545: 20% of 2545 is exactly 509, so delta 509 => 2000 bps.
    // Delta 508 => 5,080,000 / 2545 = 1996.07... which TRUNCATES to 1996 bps
    // (no delta at this anchor can produce 1997-1999 bps). Truncation is
    // toward zero in both directions, so the boundary is symmetric.
    BOOST_CHECK(ExceedsThresholdBps(2545, 2545 + 509, VolatilityThresholds::FREEZE_MINT_1H_BPS));  // +2000 bps
    BOOST_CHECK(!ExceedsThresholdBps(2545, 2545 + 508, VolatilityThresholds::FREEZE_MINT_1H_BPS)); // +1996 bps
    BOOST_CHECK(ExceedsThresholdBps(2545, 2545 - 509, VolatilityThresholds::FREEZE_MINT_1H_BPS));  // -2000 bps
    BOOST_CHECK(!ExceedsThresholdBps(2545, 2545 - 508, VolatilityThresholds::FREEZE_MINT_1H_BPS)); // -1996 bps
}

BOOST_AUTO_TEST_CASE(threshold_zero_anchor_never_exceeds)
{
    // Anchor 0 (no in-window samples) can never trip the threshold helper —
    // matches the normative rule's "R > 0 &&" short-circuit.
    BOOST_CHECK(!ExceedsThresholdBps(0, 500000, VolatilityThresholds::FREEZE_MINT_1H_BPS));
    BOOST_CHECK(!ExceedsThresholdBps(0, 1, VolatilityThresholds::FREEZE_MINT_1H_BPS));
}

BOOST_AUTO_TEST_SUITE_END()
