// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Wave 6 (Final Audit): targeted boundary, fail-closed, and non-mutation
// coverage for DigiDollar canonical health, DCA, and volatility paths.
// Existing suites cover happy paths; this file fills documented gaps:
//   - Health math edge values across both code paths and confirms each
//     path's documented return value at the (ddSupply == 0) corner.
//   - DCA tier off-by-one, ceil-up rounding, and __int128 overflow guards.
//   - WouldCandidateFreezeMinting non-mutation invariants.
//   - Cooldown boundaries at H, H+cooldown-1, H+cooldown.
//   - Stale runtime cache rejection in OracleBundleManager::GetLatestPrice.
//   - ReconstructFromBlockData reorg semantics matching new block data.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <consensus/volatility.h>
#include <digidollar/health.h>
#include <oracle/bundle_manager.h>
#include <primitives/oracle.h>
#include <test/util/setup_common.h>
#include <test/util/source_root.h>
#include <util/time.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using DigiDollar::AlertThresholds;
using DigiDollar::SystemHealthMonitor;
using DigiDollar::SystemMetrics;
using DigiDollar::DCA::DynamicCollateralAdjustment;
using DigiDollar::HealthUtils::CalculateHealthRatio;
using DigiDollar::Volatility::PricePoint;
using DigiDollar::Volatility::VolatilityMonitor;
using DigiDollar::Volatility::VolatilityState;
using DigiDollar::Volatility::VolatilityThresholds;

BOOST_AUTO_TEST_SUITE(digidollar_wave6_health_dca_volatility_tests)

namespace {

struct Wave6Setup : public TestingSetup {
    Wave6Setup() : TestingSetup(ChainType::REGTEST) {
        VolatilityMonitor::ClearHistory();
        VolatilityMonitor::ClearFreeze();
        SystemHealthMonitor::ResetMetrics();
    }
    ~Wave6Setup() {
        VolatilityMonitor::ClearHistory();
        VolatilityMonitor::ClearFreeze();
        SystemHealthMonitor::ResetMetrics();
    }
};

// HealthUtils::CalculateHealthRatio uses cents-per-DGB price input.
// Returns 300 when ddAmount == 0 (perfect health, no liabilities).
// Returns 0 when price/amount invalid; clamps to [0, 300].
//
// DCA::DynamicCollateralAdjustment::CalculateSystemHealth uses milli-cents
// (100,000 = $1.00 DGB) and clamps to [0, 30000] (i.e. 300.00% scaled by 100).
// Returns 30000 when totalDD == 0 and clamps everything else to 30000.
//
// The two ranges differ by a 100x scale and should not be compared directly.

constexpr CAmount kCents = 1;  // helper for readability

} // namespace

// =============================================================================
// 1. Canonical health-math boundaries (per documented contract).
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_healthutils_zero_supply_returns_300, Wave6Setup)
{
    // CalculateHealthRatio returns 300 when ddAmount == 0 regardless of
    // collateral/price (zero supply -> perfect health).
    BOOST_CHECK_EQUAL(CalculateHealthRatio(0, 0, 0), 300);
    BOOST_CHECK_EQUAL(CalculateHealthRatio(0, 1, 1), 300);
    BOOST_CHECK_EQUAL(CalculateHealthRatio(0, MAX_MONEY, 100), 300);
}

BOOST_FIXTURE_TEST_CASE(wave6_healthutils_invalid_price_or_collateral_returns_0, Wave6Setup)
{
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 100 * COIN, 0), 0);
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 0, 50), 0);
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 100 * COIN, -1), 0);
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, -1, 50), 0);
}

BOOST_FIXTURE_TEST_CASE(wave6_healthutils_clamps_to_300_at_extreme_collateral, Wave6Setup)
{
    // 200 DGB at $0.50/DGB backs $100 DD with 100% ratio.
    // Below uses 600 DGB at $0.50/DGB to get 300% (the cap).
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 600 * COIN, 50), 300);
    // Even more extreme collateral must still clamp at 300.
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 6000 * COIN, 50), 300);
    BOOST_CHECK_EQUAL(CalculateHealthRatio(1, MAX_MONEY, 100), 300);
}

BOOST_FIXTURE_TEST_CASE(wave6_healthutils_boundary_values, Wave6Setup)
{
    // 200 DGB * $0.50 / DGB = $100 collateral value, $100 DD = 100% health.
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 200 * COIN, 50), 100);
    // 220 DGB * $0.50 = $110 backing $100 DD = 110%.
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 220 * COIN, 50), 110);
    // 240 DGB -> 120%.
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 240 * COIN, 50), 120);
    // 300 DGB -> 150%.
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 300 * COIN, 50), 150);
    // 398 DGB -> 199%.
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 398 * COIN, 50), 199);
    // 400 DGB -> 200%.
    BOOST_CHECK_EQUAL(CalculateHealthRatio(10000, 400 * COIN, 50), 200);
}

BOOST_FIXTURE_TEST_CASE(wave6_dca_health_int128_no_overflow, Wave6Setup)
{
    // Documents the DCA::CalculateSystemHealth contract:
    //   - totalCollateral in satoshis
    //   - totalDD in cents
    //   - oraclePrice in milli-cents per DGB (100,000 = $1.00 DGB)
    // Result is a health ratio in basis-of-percent units (100 = 100%, 30000 cap).
    // numerator   = collateral * oraclePrice * 100
    // denominator = COIN * 1000 * totalDD
    // health      = numerator / denominator (clamped at 30000).

    // Sanity at 100% collateralization: 200 DGB * $0.50 = $100 backing $100 DD.
    // $0.50/DGB is 50,000 milli-cents per DGB.
    int health = DynamicCollateralAdjustment::CalculateSystemHealth(
        200 * COIN, 10000, 50000);
    BOOST_CHECK_EQUAL(health, 100);

    // Negative totalDD must fail closed (returns 0).
    int negDD = DynamicCollateralAdjustment::CalculateSystemHealth(
        100 * COIN, -1, 50000);
    BOOST_CHECK_EQUAL(negDD, 0);

    // Negative collateral fails closed.
    int negColl = DynamicCollateralAdjustment::CalculateSystemHealth(
        -1, 10000, 50000);
    BOOST_CHECK_EQUAL(negColl, 0);

    // Negative price (any non-positive) fails closed.
    int negPrice = DynamicCollateralAdjustment::CalculateSystemHealth(
        100 * COIN, 10000, -1);
    BOOST_CHECK_EQUAL(negPrice, 0);

    // INT64_MIN price: hits oraclePrice <= 0 guard, returns 0.
    int minPrice = DynamicCollateralAdjustment::CalculateSystemHealth(
        100 * COIN, 10000, std::numeric_limits<CAmount>::min());
    BOOST_CHECK_EQUAL(minPrice, 0);

    // Maximum supported price + maximum collateral must clamp at 30000.
    int extremeHealth = DynamicCollateralAdjustment::CalculateSystemHealth(
        MAX_MONEY, 10000,
        static_cast<CAmount>(ORACLE_MAX_PRICE_MICRO_USD));
    BOOST_CHECK_EQUAL(extremeHealth, 30000);

    // Zero DD, any price -> 30000 (max health).
    int zeroDD = DynamicCollateralAdjustment::CalculateSystemHealth(
        1 * COIN, 0, 50000);
    BOOST_CHECK_EQUAL(zeroDD, 30000);
}

BOOST_FIXTURE_TEST_CASE(wave6_dca_health_clamp_avoids_int_overflow, Wave6Setup)
{
    // RH-fix from src/consensus/dca.cpp:107-110: clamp the __int128 health
    // BEFORE casting to int. Hammer the path with combinations that would
    // overflow a naive int truncation.
    constexpr CAmount tinyDD = 1;            // 1 cent of DD
    constexpr CAmount maxColl = MAX_MONEY;
    int huge = DynamicCollateralAdjustment::CalculateSystemHealth(
        maxColl, tinyDD, ORACLE_MAX_PRICE_MICRO_USD);
    BOOST_CHECK_EQUAL(huge, 30000);

    // GetDCAMultiplier on the clamped 30000 must return 1.0x (healthy).
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(huge), 1.0);
}

// =============================================================================
// 2. DCA tier off-by-one and ceil-up rounding.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_dca_apply_rounds_up_to_favor_safety, Wave6Setup)
{
    // 1.25x on baseRatio=121 -> 121 * 12500 / 10000 = 151.25, ceil -> 152.
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(121, 130), 152);

    // 1.5x on baseRatio=101 -> 101 * 15000 / 10000 = 151.5, ceil -> 152.
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(101, 110), 152);

    // 2.0x on baseRatio=151 -> 151 * 20000 / 10000 = 302.0 (exact, not ceil).
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(151, 50), 302);

    // 1.25x on baseRatio=200 -> exactly 250 (no ceil bump).
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(200, 130), 250);
}

BOOST_FIXTURE_TEST_CASE(wave6_dca_tier_boundaries_off_by_one, Wave6Setup)
{
    // Healthy lower boundary at exactly 150 (1.0x).
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(150), 1.0);
    // 149 falls into warning tier (1.25x).
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(149), 1.25);
    // 120 still warning, 119 critical.
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(120), 1.25);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(119), 1.5);
    // 110 critical, 109 emergency.
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(110), 1.5);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(109), 2.0);

    // Spot-check 100, 130, 200, 300, 400 health values.
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(100), 2.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(130), 1.25);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(200), 1.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(300), 1.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(400), 1.0);
}

BOOST_FIXTURE_TEST_CASE(wave6_dca_apply_int128_overflow_caps_int_max, Wave6Setup)
{
    // ApplyDCA must guard against int truncation by capping at INT_MAX
    // when the __int128 product would otherwise overflow.
    int hugeBase = std::numeric_limits<int>::max();
    int multiplied = DynamicCollateralAdjustment::ApplyDCA(hugeBase, 50); // 2.0x emergency
    BOOST_CHECK_EQUAL(multiplied, std::numeric_limits<int>::max());
}

BOOST_FIXTURE_TEST_CASE(wave6_dca_apply_with_zero_or_negative_base_ratio, Wave6Setup)
{
    // Negative or zero base ratio should not become a positive number.
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(0, 110), 0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(-1, 110), 0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::ApplyDCA(-1000, 50), 0);
}

// =============================================================================
// 3. Volatility — non-mutation, cooldown boundaries, reorg semantics.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_volatility_would_freeze_no_mutation, Wave6Setup)
{
    // RecordPrice adds one fixed point.
    const CAmount basePrice = 500000;  // $0.50 in micro-USD
    const int64_t t0 = GetTime();
    VolatilityMonitor::RecordPrice(basePrice, t0, 1000);

    auto historyBefore = VolatilityMonitor::GetPriceHistory();
    auto stateBefore = VolatilityMonitor::GetCurrentState();
    BOOST_REQUIRE_EQUAL(historyBefore.size(), 1U);

    // Threshold-crossing candidate (+25%) must not mutate any reachable state.
    const CAmount candidate = basePrice * 125 / 100;
    BOOST_CHECK(VolatilityMonitor::WouldCandidateFreezeMinting(candidate));

    auto historyAfter = VolatilityMonitor::GetPriceHistory();
    auto stateAfter = VolatilityMonitor::GetCurrentState();
    BOOST_CHECK_EQUAL(historyAfter.size(), historyBefore.size());
    BOOST_CHECK_EQUAL(historyAfter.back().price, historyBefore.back().price);
    BOOST_CHECK_EQUAL(historyAfter.back().timestamp, historyBefore.back().timestamp);
    BOOST_CHECK_EQUAL(historyAfter.back().height, historyBefore.back().height);
    BOOST_CHECK(!stateAfter.mintingFrozen);
    BOOST_CHECK(!stateAfter.allOperationsFrozen);
    BOOST_CHECK_EQUAL(stateAfter.freezeHeight, stateBefore.freezeHeight);
    BOOST_CHECK_EQUAL(stateAfter.cooldownEndHeight, stateBefore.cooldownEndHeight);
    BOOST_CHECK_EQUAL(VolatilityMonitor::GetCooldownEndHeight(), 0u);

    // A safe candidate (small move) returns false and also does not mutate.
    BOOST_CHECK(!VolatilityMonitor::WouldCandidateFreezeMinting(basePrice + 1));
    auto historyFinal = VolatilityMonitor::GetPriceHistory();
    BOOST_CHECK_EQUAL(historyFinal.size(), 1U);
    BOOST_CHECK_EQUAL(historyFinal.back().price, basePrice);
}

BOOST_FIXTURE_TEST_CASE(wave6_volatility_consensus_freeze_path_uses_integer_bps, Wave6Setup)
{
    const std::string source = ReadRepositoryFile("src/consensus/volatility.cpp");

    const size_t would_pos = source.find("bool VolatilityMonitor::WouldCandidateFreezeMinting");
    BOOST_REQUIRE_NE(would_pos, std::string::npos);
    const size_t would_end = source.find("\n}", would_pos);
    BOOST_REQUIRE_NE(would_end, std::string::npos);
    const std::string would_source = source.substr(would_pos, would_end - would_pos);

    BOOST_CHECK_NE(source.find("CalculatePercentageChangeBps"), std::string::npos);
    BOOST_CHECK_EQUAL(would_source.find("double"), std::string::npos);
    BOOST_CHECK_EQUAL(would_source.find("CalculatePercentageChange("), std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(wave6_volatility_cooldown_exact_boundaries, Wave6Setup)
{
    // Seed exactly one calm price so UpdateState can advance lastUpdateHeight.
    // Without a seed, UpdateState early-returns and lastUpdateHeight stays
    // pinned at 0, leaving InCooldownPeriod permanently true.
    const CAmount basePrice = 500000;       // $0.50 in micro-USD
    const int64_t t0 = 1'700'000'000;
    VolatilityMonitor::RecordPrice(basePrice, t0, 0);

    // Trigger a manual freeze at H. Cooldown end is H + COOLDOWN_BLOCKS.
    const uint32_t H = 5'000;
    VolatilityMonitor::TriggerFreeze(false, H);
    BOOST_CHECK(VolatilityMonitor::ShouldFreezeMinting());
    BOOST_CHECK_EQUAL(VolatilityMonitor::GetCooldownEndHeight(),
                      H + VolatilityThresholds::COOLDOWN_BLOCKS);

    // At H itself: definitely in cooldown.
    VolatilityMonitor::UpdateState(H);
    BOOST_CHECK(VolatilityMonitor::InCooldownPeriod());

    // At H + cooldown - 1: still in cooldown.
    VolatilityMonitor::UpdateState(H + VolatilityThresholds::COOLDOWN_BLOCKS - 1);
    BOOST_CHECK(VolatilityMonitor::InCooldownPeriod());

    // At H + cooldown: still considered in cooldown (boundary inclusive).
    // InCooldownPeriod() returns lastUpdateHeight <= cooldownEndHeight.
    VolatilityMonitor::UpdateState(H + VolatilityThresholds::COOLDOWN_BLOCKS);
    BOOST_CHECK(VolatilityMonitor::InCooldownPeriod());

    // One block past the cooldown end: cooldown has expired (and the
    // calm-volatility path inside UpdateState may also clear cooldownEndHeight).
    VolatilityMonitor::UpdateState(H + VolatilityThresholds::COOLDOWN_BLOCKS + 1);
    BOOST_CHECK(!VolatilityMonitor::InCooldownPeriod());
}

BOOST_FIXTURE_TEST_CASE(wave6_volatility_reconstruct_replaces_state, Wave6Setup)
{
    // Build a freezing scenario, save the price history, clear all state,
    // then reconstruct from a DIFFERENT (non-freezing) history. Final state
    // must reflect the new history, not the old.
    const CAmount basePrice = 500000;
    const int64_t t0 = 1'700'000'000;
    VolatilityMonitor::RecordPrice(basePrice, t0, 1000);
    VolatilityMonitor::RecordPrice(basePrice * 130 / 100, t0 + 3600, 1240);
    BOOST_REQUIRE(VolatilityMonitor::ShouldFreezeMinting());

    // Reconstruct from a stable history with no freeze trigger.
    std::vector<PricePoint> stable;
    for (int i = 0; i < 5; ++i) {
        stable.emplace_back(basePrice + i, t0 + i * 3600, 2000 + i);
    }
    VolatilityMonitor::ReconstructFromBlockData(stable, 2010);

    auto state = VolatilityMonitor::GetCurrentState();
    BOOST_CHECK(!state.mintingFrozen);
    BOOST_CHECK(!state.allOperationsFrozen);
    auto history = VolatilityMonitor::GetPriceHistory();
    BOOST_CHECK_EQUAL(history.size(), stable.size());
    BOOST_CHECK_EQUAL(history.back().height, 2004u);

    // And the inverse — reconstruct a freeze-triggering history; the state
    // after reorg must show the freeze even though the working tree was
    // previously clean.
    std::vector<PricePoint> volatile_history;
    volatile_history.emplace_back(basePrice, t0, 1000);
    volatile_history.emplace_back(basePrice * 130 / 100, t0 + 3600, 1240);
    VolatilityMonitor::ReconstructFromBlockData(volatile_history, 1240);
    BOOST_CHECK(VolatilityMonitor::ShouldFreezeMinting());
}

BOOST_FIXTURE_TEST_CASE(wave6_volatility_outlier_invalid_price_does_not_mutate, Wave6Setup)
{
    const CAmount basePrice = 500000;
    const int64_t t0 = GetTime();
    VolatilityMonitor::RecordPrice(basePrice, t0, 100);

    auto before = VolatilityMonitor::GetPriceHistory();
    BOOST_REQUIRE_EQUAL(before.size(), 1U);

    // Out-of-range prices must be silently rejected.
    VolatilityMonitor::RecordPrice(0, t0 + 3600, 101);
    VolatilityMonitor::RecordPrice(-basePrice, t0 + 7200, 102);
    VolatilityMonitor::RecordPrice(
        static_cast<CAmount>(ORACLE_MAX_PRICE_MICRO_USD) + 1, t0 + 10800, 103);
    VolatilityMonitor::RecordPrice(
        static_cast<CAmount>(ORACLE_MIN_PRICE_MICRO_USD) - 1, t0 + 14400, 104);

    auto after = VolatilityMonitor::GetPriceHistory();
    BOOST_CHECK_EQUAL(after.size(), 1U);
    BOOST_CHECK_EQUAL(after.back().price, basePrice);

    auto state = VolatilityMonitor::GetCurrentState();
    BOOST_CHECK(!state.mintingFrozen);
    BOOST_CHECK(!state.allOperationsFrozen);
}

// =============================================================================
// 4. Stale-cache rejection in OracleBundleManager::GetLatestPrice.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_oracle_get_latest_price_stale_after_runtime, Wave6Setup)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    const int64_t fresh_time = 1'800'000'000;
    SetMockTime(fresh_time);

    // Inject fresh cache entry.
    manager.UpdatePriceCache(1000, 500000, fresh_time);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 500000);

    // Wall-clock advances past ORACLE_MAX_AGE_SECONDS without a refresh.
    SetMockTime(fresh_time + ORACLE_MAX_AGE_SECONDS + 1);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 0);

    SetMockTime(0);
    manager.Clear();
}

BOOST_FIXTURE_TEST_CASE(wave6_oracle_get_latest_price_fresh_within_window, Wave6Setup)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    const int64_t cache_time = 1'800'000'000;
    SetMockTime(cache_time);
    manager.UpdatePriceCache(1234, 700000, cache_time);

    // Just inside the staleness window — must still return the cached price.
    SetMockTime(cache_time + ORACLE_MAX_AGE_SECONDS - 1);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 700000);

    // Exactly at the boundary: still considered fresh (age == ORACLE_MAX_AGE).
    SetMockTime(cache_time + ORACLE_MAX_AGE_SECONDS);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 700000);

    SetMockTime(0);
    manager.Clear();
}

// =============================================================================
// 5. Reorg-style cache rebuild: RemovePriceCache + ReconstructFromBlockData.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_reorg_volatility_state_matches_new_blocks, Wave6Setup)
{
    const CAmount basePrice = 500000;
    const int64_t t0 = 1'700'000'000;

    // Original chain: freeze trigger at height 1240.
    VolatilityMonitor::RecordPrice(basePrice, t0, 1000);
    VolatilityMonitor::RecordPrice(basePrice * 130 / 100, t0 + 3600, 1240);
    BOOST_REQUIRE(VolatilityMonitor::ShouldFreezeMinting());

    // Simulate disconnect by clearing the in-memory state, then replaying
    // the surviving block data (just the calm price at 1000).
    std::vector<PricePoint> survivors{{basePrice, t0, 1000}};
    VolatilityMonitor::ReconstructFromBlockData(survivors, 1000);

    auto state = VolatilityMonitor::GetCurrentState();
    BOOST_CHECK(!state.mintingFrozen);
    BOOST_CHECK(!state.allOperationsFrozen);
    auto history = VolatilityMonitor::GetPriceHistory();
    BOOST_REQUIRE_EQUAL(history.size(), 1U);
    BOOST_CHECK_EQUAL(history[0].height, 1000u);
    BOOST_CHECK_EQUAL(history[0].price, basePrice);
}

// =============================================================================
// 6. Documenting the SystemHealthMonitor cached-metrics contract.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_systemhealthmonitor_set_metrics_for_testing, Wave6Setup)
{
    // SetMetricsForTesting + GetCachedMetrics is the only public path that
    // unit tests can use to seed a canonical health snapshot. Confirm that
    // the snapshot round-trips and that hasCanonicalHealth is preserved.
    SystemMetrics seed;
    seed.totalDDSupply = 50'000;          // $500.00 in cents
    seed.totalCollateral = 200 * COIN;    // 200 DGB
    seed.systemHealth = 120;
    seed.hasCanonicalHealth = true;
    seed.lastOraclePrice = 500'000;       // $0.50 per DGB in micro-USD
    seed.activeOracles = 17;
    SystemHealthMonitor::SetMetricsForTesting(seed);

    auto cached = SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(cached.totalDDSupply, 50'000);
    BOOST_CHECK_EQUAL(cached.totalCollateral, 200 * COIN);
    BOOST_CHECK_EQUAL(cached.systemHealth, 120);
    BOOST_CHECK(cached.hasCanonicalHealth);

    // GetCurrentSystemHealth must return the cached canonical value when
    // hasCanonicalHealth is true and supply > 0.
    int dca_health = DynamicCollateralAdjustment::GetCurrentSystemHealth();
    BOOST_CHECK_EQUAL(dca_health, 120);

    // Mint connect invalidates the canonical bit (hasCanonicalHealth=false)
    // so a follow-up GetCurrentSystemHealth recomputes from cents -> millicents.
    SystemHealthMonitor::OnMintConnected(0, 0);
    auto after = SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK(!after.hasCanonicalHealth);
    BOOST_CHECK_EQUAL(after.systemHealth, 0);
}

BOOST_FIXTURE_TEST_CASE(wave6_systemhealthmonitor_volatility_price_units_are_cents, Wave6Setup)
{
    // Volatility history records oracle prices in micro-USD because accepted
    // mint volatility is fed from ValidationContext::oraclePriceMicroUSD.
    // SystemHealthMonitor::lastOraclePrice is documented as micro-USD, and
    // UpdateTierMetrics converts to millicents before health calculation.
    // It must not treat a micro-USD volatility point as cents.
    SystemHealthMonitor::Initialize();

    SystemMetrics seed;
    seed.totalDDSupply = 10'000;       // $100.00 DD
    seed.totalCollateral = 100 * COIN; // 100 DGB
    seed.systemHealth = 0;
    seed.hasCanonicalHealth = false;
    SystemHealthMonitor::SetMetricsForTesting(seed);

    VolatilityMonitor::RecordPrice(500'000, 1'700'000'000, 1'000); // $0.50 in micro-USD

    SystemMetrics metrics = SystemHealthMonitor::GetSystemMetrics();
    BOOST_CHECK(metrics.hasCanonicalHealth);
    BOOST_CHECK_EQUAL(metrics.lastOraclePrice, 500'000);
    BOOST_CHECK_EQUAL(metrics.systemHealth, 50);
}

// =============================================================================
// 7. Additional pure-check / non-mutation invariants for WouldCandidateFreezeMinting.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_would_candidate_does_not_mutate_when_already_frozen, Wave6Setup)
{
    // Setup: trigger a freeze manually so currentState.mintingFrozen == true and
    // cooldownEndHeight is set to a future block. WouldCandidateFreezeMinting()
    // is documented as a pure check; calling it during a live freeze must not
    // touch priceHistory, freezeHeight, cooldownEndHeight, or
    // mintingFrozen/allOperationsFrozen.
    const CAmount basePrice = 500000;
    const int64_t t0 = 1'700'000'000;
    VolatilityMonitor::RecordPrice(basePrice, t0, 1000);

    const uint32_t H = 9'000;
    VolatilityMonitor::TriggerFreeze(false, H);
    BOOST_REQUIRE(VolatilityMonitor::ShouldFreezeMinting());

    auto historyBefore = VolatilityMonitor::GetPriceHistory();
    auto stateBefore = VolatilityMonitor::GetCurrentState();
    const uint32_t cooldownBefore = VolatilityMonitor::GetCooldownEndHeight();

    // Threshold-crossing candidate: must still report would-freeze, but must
    // not extend or re-arm the existing freeze/cooldown.
    const CAmount big_candidate = basePrice * 200 / 100;
    BOOST_CHECK(VolatilityMonitor::WouldCandidateFreezeMinting(big_candidate));

    // A safe candidate during freeze must also not mutate.
    BOOST_CHECK(!VolatilityMonitor::WouldCandidateFreezeMinting(basePrice + 1));

    auto historyAfter = VolatilityMonitor::GetPriceHistory();
    auto stateAfter = VolatilityMonitor::GetCurrentState();
    const uint32_t cooldownAfter = VolatilityMonitor::GetCooldownEndHeight();

    BOOST_CHECK_EQUAL(historyAfter.size(), historyBefore.size());
    BOOST_CHECK_EQUAL(historyAfter.back().price, historyBefore.back().price);
    BOOST_CHECK_EQUAL(historyAfter.back().timestamp, historyBefore.back().timestamp);
    BOOST_CHECK_EQUAL(historyAfter.back().height, historyBefore.back().height);
    BOOST_CHECK_EQUAL(stateAfter.mintingFrozen, stateBefore.mintingFrozen);
    BOOST_CHECK_EQUAL(stateAfter.allOperationsFrozen, stateBefore.allOperationsFrozen);
    BOOST_CHECK_EQUAL(stateAfter.freezeHeight, stateBefore.freezeHeight);
    BOOST_CHECK_EQUAL(cooldownAfter, cooldownBefore);
}

BOOST_FIXTURE_TEST_CASE(wave6_would_candidate_pure_when_priceHistory_empty, Wave6Setup)
{
    // priceHistory empty -> WouldCandidateFreezeMinting always false (per
    // volatility.cpp:96), and there must be no side effects.
    BOOST_REQUIRE(VolatilityMonitor::GetPriceHistory().empty());
    BOOST_CHECK(!VolatilityMonitor::WouldCandidateFreezeMinting(500000));
    BOOST_CHECK(!VolatilityMonitor::WouldCandidateFreezeMinting(0));
    BOOST_CHECK(!VolatilityMonitor::WouldCandidateFreezeMinting(1));
    BOOST_CHECK(VolatilityMonitor::GetPriceHistory().empty());

    auto state = VolatilityMonitor::GetCurrentState();
    BOOST_CHECK(!state.mintingFrozen);
    BOOST_CHECK(!state.allOperationsFrozen);
    BOOST_CHECK_EQUAL(state.freezeHeight, 0u);
    BOOST_CHECK_EQUAL(state.cooldownEndHeight, 0u);
}

// =============================================================================
// 8. Cooldown re-arm semantics: re-trigger after expiry must extend cooldownEndHeight.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_volatility_cooldown_rearms_after_expiry, Wave6Setup)
{
    const CAmount basePrice = 500000;
    const int64_t t0 = 1'700'000'000;
    VolatilityMonitor::RecordPrice(basePrice, t0, 0);

    const uint32_t H1 = 1'000;
    VolatilityMonitor::TriggerFreeze(false, H1);
    BOOST_REQUIRE(VolatilityMonitor::ShouldFreezeMinting());
    const uint32_t cooldown1 = VolatilityMonitor::GetCooldownEndHeight();
    BOOST_CHECK_EQUAL(cooldown1, H1 + VolatilityThresholds::COOLDOWN_BLOCKS);

    // Advance past cooldown1 so the unfreeze path inside UpdateState clears
    // the freeze (calm volatility, single price, no excursion).
    VolatilityMonitor::UpdateState(cooldown1 + 1);
    BOOST_CHECK(!VolatilityMonitor::InCooldownPeriod());
    BOOST_CHECK(!VolatilityMonitor::ShouldFreezeMinting());
    BOOST_CHECK_EQUAL(VolatilityMonitor::GetCooldownEndHeight(), 0u);

    // Now re-trigger at H2. cooldownEndHeight must be H2 + COOLDOWN_BLOCKS,
    // not H1 + COOLDOWN_BLOCKS — the second freeze fully re-arms the window.
    const uint32_t H2 = cooldown1 + 100;
    VolatilityMonitor::TriggerFreeze(false, H2);
    const uint32_t cooldown2 = VolatilityMonitor::GetCooldownEndHeight();
    BOOST_CHECK_EQUAL(cooldown2, H2 + VolatilityThresholds::COOLDOWN_BLOCKS);
    BOOST_CHECK_GT(cooldown2, cooldown1);
    BOOST_CHECK(VolatilityMonitor::ShouldFreezeMinting());
}

// =============================================================================
// 9. RemovePriceCache reverts cached_price to highest remaining height
//    (proves the rh61-fix invariant is maintained on disconnect).
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_remove_price_cache_reverts_to_prior_height, Wave6Setup)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    const int64_t t0 = 1'800'000'000;
    SetMockTime(t0);

    manager.UpdatePriceCache(1000, 100000, t0);
    manager.UpdatePriceCache(1001, 200000, t0);
    manager.UpdatePriceCache(1002, 300000, t0);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 300000);
    BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(1001), 200000);

    // Disconnect height 1002: cached_price must revert to 1001's price.
    manager.RemovePriceCache(1002);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 200000);
    BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(1002), 0u);

    // Disconnect 1001: revert to 1000's price.
    manager.RemovePriceCache(1001);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 100000);

    // Disconnect 1000: cache empty, cached_price must be 0 (fail-closed).
    manager.RemovePriceCache(1000);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 0);

    SetMockTime(0);
    manager.Clear();
}

BOOST_FIXTURE_TEST_CASE(wave6_remove_price_cache_unknown_height_is_noop, Wave6Setup)
{
    // Disconnecting a height that was never cached must be a no-op and must
    // not clobber the legitimate cached_price for the highest known height.
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    const int64_t t0 = 1'800'000'000;
    SetMockTime(t0);

    manager.UpdatePriceCache(2000, 555000, t0);
    BOOST_REQUIRE_EQUAL(manager.GetLatestPrice(), 555000);

    manager.RemovePriceCache(9999);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 555000);
    BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(2000), 555000);

    SetMockTime(0);
    manager.Clear();
}

// =============================================================================
// 10. UpdatePriceCache at an existing height overwrites both height map and
//     the latest cached_price (post-rh61 invariant).
// =============================================================================

BOOST_FIXTURE_TEST_CASE(wave6_update_price_cache_overwrites_existing_height, Wave6Setup)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    const int64_t t0 = 1'800'000'000;
    SetMockTime(t0);

    manager.UpdatePriceCache(3000, 100000, t0);
    BOOST_REQUIRE_EQUAL(manager.GetLatestPrice(), 100000);

    // Same-height re-application replaces the entry (e.g., reorg replay).
    manager.UpdatePriceCache(3000, 222222, t0);
    BOOST_CHECK_EQUAL(manager.GetOraclePriceForHeight(3000), 222222);
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(), 222222);

    SetMockTime(0);
    manager.Clear();
}

BOOST_AUTO_TEST_SUITE_END()
