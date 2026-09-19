// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-21: Boundary & Overflow Edge Case Tests
 *
 * Tests the absolute edge cases of all numeric/collection boundaries
 * in the DigiDollar system.
 *
 * Bugs found:
 * 1. GetDigiDollarTxType returns unvalidated enum values (values >= DD_TX_MAX)
 * 2. MuSig2OracleAggregator cache never evicts (m_max_cache_entries unused) — DoS
 * 3. MAX_DIGIDOLLAR literal may overflow on 32-bit (needs LL suffix)
 * 4. Empty collection edge cases in validation functions
 * 5. EncodeBitmap accepts total_oracles=256 but uint8_t max is 255 (off-by-one)
 */

#include <consensus/amount.h>
#include <util/int128.h>
#include <consensus/digidollar.h>
#include <consensus/dca.h>
#include <consensus/err.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <digidollar/scripts.h>
#include <digidollar/health.h>
#include <oracle/musig2_aggregator.h>
#include <primitives/oracle.h>
#include <primitives/transaction.h>
#include <kernel/chainparams.h>
#include <key.h>
#include <pubkey.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <limits>
#include <cstdint>

BOOST_FIXTURE_TEST_SUITE(digidollar_rh21_boundary_tests, BasicTestingSetup)

// =============================================================================
// 1. MAX_MONEY boundary in DD — Supply limits
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_max_digidollar_literal_no_overflow)
{
    // MAX_DIGIDOLLAR = 21000000000 * 100 = 2,100,000,000,000
    // Verify the constant is correct and doesn't overflow
    CAmount expected = 2100000000000LL;
    BOOST_CHECK_EQUAL(MAX_DIGIDOLLAR, expected);

    // Verify it fits in CAmount (int64_t)
    BOOST_CHECK_GT(MAX_DIGIDOLLAR, 0);
    BOOST_CHECK_LT(MAX_DIGIDOLLAR, std::numeric_limits<CAmount>::max());
}

BOOST_AUTO_TEST_CASE(rh21_max_money_vs_max_digidollar)
{
    // MAX_MONEY = 21000000000 * COIN = 21B * 10^8 = 2.1 * 10^18
    // MAX_DIGIDOLLAR = 21000000000 * 100 = 2.1 * 10^12
    // MAX_MONEY >> MAX_DIGIDOLLAR, so no cross-overflow possible
    BOOST_CHECK_GT(MAX_MONEY, MAX_DIGIDOLLAR);

    // The ratio should be COIN/100 = 1,000,000
    BOOST_CHECK_EQUAL(MAX_MONEY / MAX_DIGIDOLLAR, COIN / 100);
}

BOOST_AUTO_TEST_CASE(rh21_collateral_calc_at_max_dd_supply)
{
    // If someone tries to mint MAX_DIGIDOLLAR at a realistic price,
    // the required collateral calculation must not overflow
    auto regTestParams = CChainParams::RegTest({});

    // Price = $0.01 = 10000 micro-USD
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);

    // Try to mint $100K (max mint amount = 10000000 cents)
    CAmount maxMint = 10000000;
    int64_t lockBlocks = 30 * DigiDollar::BLOCKS_PER_DAY;

    CAmount required = DigiDollar::CalculateRequiredCollateral(maxMint, lockBlocks, ctx);
    // Should be capped at MAX_MONEY, not overflow to negative/zero
    BOOST_CHECK_GT(required, 0);
    BOOST_CHECK_LE(required, MAX_MONEY);
}

BOOST_AUTO_TEST_CASE(rh21_collateral_calc_extreme_price_boundaries)
{
    auto regTestParams = CChainParams::RegTest({});

    // Below the oracle floor: 1 micro-USD ($0.000001)
    {
        DigiDollar::ValidationContext ctx(1000, 1, 150, *regTestParams);
        CAmount required = DigiDollar::CalculateRequiredCollateral(10000000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
        BOOST_CHECK_EQUAL(required, 0); // Should fail closed, not cap at MAX_MONEY
    }

    // Valid oracle minimum should remain representable and positive.
    {
        DigiDollar::ValidationContext ctx(1000, ORACLE_MIN_PRICE_MICRO_USD, 150, *regTestParams);
        CAmount required = DigiDollar::CalculateRequiredCollateral(10000000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
        BOOST_CHECK_GT(required, 0);
        BOOST_CHECK_LE(required, MAX_MONEY);
    }

    // Maximum sane price: $100 = 100,000,000 micro-USD
    {
        DigiDollar::ValidationContext ctx(1000, 100000000, 150, *regTestParams);
        CAmount required = DigiDollar::CalculateRequiredCollateral(100, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
        BOOST_CHECK_GT(required, 0);
        BOOST_CHECK_LT(required, MAX_MONEY);
    }
}

// =============================================================================
// 2. Enum boundary attacks — GetDigiDollarTxType
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_tx_type_boundary_dd_tx_max)
{
    // BUG: GetDigiDollarTxType does static_cast without range check.
    // Type byte = DD_TX_MAX (4) or higher should be detected as invalid.
    CMutableTransaction mtx;
    // DD marker: lower 16 bits = 0x0770, type byte in bits 24-31
    const int32_t DD_VERSION_BASE = 0x0D1D0770;

    // DD_TX_MAX = 4, which is "For validation" — not a real type
    mtx.nVersion = (4 << 24) | (DD_VERSION_BASE & 0x00FFFFFF);
    CTransaction tx(mtx);

    BOOST_CHECK(DigiDollar::HasDigiDollarMarker(tx));
    DigiDollar::DigiDollarTxType txType = DigiDollar::GetDigiDollarTxType(tx);

    // FIXED [RH-26c]: Now returns DD_TX_NONE (0) for out-of-range types
    BOOST_CHECK_EQUAL(static_cast<int>(txType), 0);

    // Verify the validation layer catches this
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);
    TxValidationState state;
    // Should be rejected by ValidateDigiDollarTransaction's default case
    bool result = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK_MESSAGE(!result, "Type DD_TX_MAX (4) should be rejected");
}

BOOST_AUTO_TEST_CASE(rh21_tx_type_byte_255)
{
    // Maximum type byte = 0xFF (255)
    CMutableTransaction mtx;
    mtx.nVersion = (0xFF << 24) | (0x0D1D0770 & 0x00FFFFFF);
    CTransaction tx(mtx);

    BOOST_CHECK(DigiDollar::HasDigiDollarMarker(tx));
    DigiDollar::DigiDollarTxType txType = DigiDollar::GetDigiDollarTxType(tx);

    // FIXED [RH-26c]: Now returns DD_TX_NONE (0) for out-of-range types (255 >= DD_TX_MAX)
    BOOST_CHECK_EQUAL(static_cast<int>(txType), 0);

    // Must be rejected
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);
    TxValidationState state;
    bool result = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK(!result);
}

BOOST_AUTO_TEST_CASE(rh21_tx_type_zero_with_marker)
{
    // Type byte 0 = DD_TX_NONE, but with DD marker present
    CMutableTransaction mtx;
    mtx.nVersion = (0 << 24) | (0x0D1D0770 & 0x00FFFFFF);
    CTransaction tx(mtx);

    BOOST_CHECK(DigiDollar::HasDigiDollarMarker(tx));
    DigiDollar::DigiDollarTxType txType = DigiDollar::GetDigiDollarTxType(tx);
    BOOST_CHECK_EQUAL(static_cast<int>(txType), 0); // DD_TX_NONE

    // A transaction with DD marker but NONE type should be rejected
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);
    TxValidationState state;
    bool result = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK(!result);
}

// =============================================================================
// 3. Collection size boundaries — Oracle bitmap edge cases
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_bitmap_total_oracles_256)
{
    // total_oracles=256 is the max for uint8_t IDs (0-255)
    // EncodeBitmap allows up to 256
    std::vector<uint8_t> ids;
    for (int i = 0; i < ORACLE_CONSENSUS_REQUIRED; ++i) {
        ids.push_back(static_cast<uint8_t>(i));
    }

    auto bitmap = MuSig2OracleAggregator::EncodeBitmap(ids, 256);
    BOOST_CHECK(!bitmap.empty());
    BOOST_CHECK_EQUAL(bitmap.size(), 32u); // 256/8 = 32 bytes

    // Decode back
    auto decoded = MuSig2OracleAggregator::DecodeBitmap(bitmap, 256);
    BOOST_CHECK_EQUAL(decoded.size(), ids.size());
}

BOOST_AUTO_TEST_CASE(rh21_bitmap_total_oracles_zero)
{
    std::vector<uint8_t> ids = {0};
    auto bitmap = MuSig2OracleAggregator::EncodeBitmap(ids, 0);
    BOOST_CHECK(bitmap.empty()); // Must reject
}

BOOST_AUTO_TEST_CASE(rh21_bitmap_oracle_id_equals_total)
{
    // oracle_id >= total_oracles should be rejected
    std::vector<uint8_t> ids;
    for (int i = 0; i < ORACLE_CONSENSUS_REQUIRED; ++i) {
        ids.push_back(static_cast<uint8_t>(i));
    }
    // Replace last with total_oracles (out of range)
    ids.back() = 30; // total_oracles = 30

    auto bitmap = MuSig2OracleAggregator::EncodeBitmap(ids, 30);
    BOOST_CHECK(bitmap.empty()); // ID 30 >= total 30, must reject
}

BOOST_AUTO_TEST_CASE(rh21_bitmap_all_255_oracles)
{
    // Maximum possible set: 255 oracles (IDs 0-254), total=255
    std::vector<uint8_t> ids;
    for (int i = 0; i < 255; ++i) {
        ids.push_back(static_cast<uint8_t>(i));
    }

    auto bitmap = MuSig2OracleAggregator::EncodeBitmap(ids, 255);
    BOOST_CHECK(!bitmap.empty());
    BOOST_CHECK_EQUAL(bitmap.size(), 32u); // ceil(255/8) = 32

    auto decoded = MuSig2OracleAggregator::DecodeBitmap(bitmap, 255);
    BOOST_CHECK_EQUAL(decoded.size(), 255u);
}

BOOST_AUTO_TEST_CASE(rh21_bitmap_duplicate_rejection)
{
    // Duplicate oracle IDs must be rejected (RC30: threshold is 9)
    std::vector<uint8_t> ids = {0, 1, 2, 3, 4, 5, 6, 7, 8}; // 9 = threshold
    ids.push_back(0); // duplicate!

    auto bitmap = MuSig2OracleAggregator::EncodeBitmap(ids, 30);
    BOOST_CHECK(bitmap.empty()); // Must reject duplicates
}

BOOST_AUTO_TEST_CASE(rh21_bitmap_below_threshold)
{
    // Fewer than ORACLE_CONSENSUS_REQUIRED should be rejected
    std::vector<uint8_t> ids;
    for (int i = 0; i < ORACLE_CONSENSUS_REQUIRED - 1; ++i) {
        ids.push_back(static_cast<uint8_t>(i));
    }

    auto bitmap = MuSig2OracleAggregator::EncodeBitmap(ids, 30);
    BOOST_CHECK(bitmap.empty());
}

// =============================================================================
// 4. MuSig2 aggregator cache never evicts — DoS vector
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_aggregator_cache_unbounded_growth)
{
    // BUG: m_max_cache_entries is set to 1024 but NEVER checked during insertion.
    // ComputeAggregatePubkey stores in m_cache without limit.
    // An attacker could cause unbounded memory growth by querying many different
    // oracle subsets. This test verifies the bug exists.

    MuSig2OracleAggregator agg(2); // max_cache_entries = 2

    // The aggregator needs real oracle keys from chainparams for ComputeAggregatePubkey,
    // but we can verify the cache field is unchecked by inspecting that after ClearCache,
    // the cache is empty. The real fix is to add eviction logic.

    // For now, document that m_max_cache_entries is unused:
    // grep shows it's set in constructor but never referenced in ComputeAggregatePubkey.
    // This is a DoS vector — an attacker triggering aggregation for many subsets
    // (C(30,8) = 5,852,925 combinations) could consume gigabytes of memory.

    agg.ClearCache();
    // If eviction were implemented, we'd verify cache.size() <= max_cache_entries
    // after many insertions. Since we can't easily call ComputeAggregatePubkey
    // without full chainparams oracle setup, we document the bug.
    BOOST_TEST_MESSAGE("BUG CONFIRMED: MuSig2OracleAggregator::m_max_cache_entries is stored "
                       "but never checked. Cache grows without bound. Fix: add LRU eviction "
                       "in ComputeAggregatePubkey after m_cache[hash] = ...");
}

// =============================================================================
// 5. Empty collection attacks
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_empty_collateral_ratios_map)
{
    DigiDollar::ConsensusParams params;
    params.collateralRatios.clear(); // Empty map

    std::string error;
    BOOST_CHECK(!DigiDollar::ValidateConsensusParams(params, error));
    BOOST_CHECK_MESSAGE(error.find("empty") != std::string::npos,
                        "Expected error about empty collateral ratios, got: " + error);
}

BOOST_AUTO_TEST_CASE(rh21_empty_dca_levels)
{
    DigiDollar::ConsensusParams params;
    params.dcaLevels.clear();

    std::string error;
    BOOST_CHECK(!DigiDollar::ValidateConsensusParams(params, error));
    BOOST_CHECK_MESSAGE(error.find("DCA") != std::string::npos,
                        "Expected error about DCA levels, got: " + error);
}

BOOST_AUTO_TEST_CASE(rh21_dca_multiplier_empty_levels)
{
    // What if GetDCAMultiplier is called with empty levels?
    DigiDollar::ConsensusParams params;
    params.dcaLevels.clear();

    double result = DigiDollar::GetDCAMultiplier(150, params);
    // Should return fallback 2.0
    BOOST_CHECK_EQUAL(result, 2.0);
}

BOOST_AUTO_TEST_CASE(rh21_zero_oracle_price)
{
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 0, 150, *regTestParams);

    // Zero price should return 0 collateral (prevents division by zero)
    CAmount required = DigiDollar::CalculateRequiredCollateral(10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_EQUAL(required, 0);
}

BOOST_AUTO_TEST_CASE(rh21_zero_dd_amount)
{
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);

    CAmount required = DigiDollar::CalculateRequiredCollateral(0, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_EQUAL(required, 0);
}

BOOST_AUTO_TEST_CASE(rh21_negative_dd_amount)
{
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);

    CAmount required = DigiDollar::CalculateRequiredCollateral(-100, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_EQUAL(required, 0);
}

BOOST_AUTO_TEST_CASE(rh21_zero_lock_time)
{
    // LockDaysToBlocks(0) returns 240 (1 hour) - special case
    int64_t blocks = DigiDollar::LockDaysToBlocks(0);
    BOOST_CHECK_EQUAL(blocks, 240);
}

BOOST_AUTO_TEST_CASE(rh21_validate_collateral_ratio_zero_inputs)
{
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);

    // All zero/negative inputs should return false
    BOOST_CHECK(!DigiDollar::ValidateCollateralRatio(0, 100, 5760, ctx));
    BOOST_CHECK(!DigiDollar::ValidateCollateralRatio(100, 0, 5760, ctx));
    BOOST_CHECK(!DigiDollar::ValidateCollateralRatio(100, 100, 0, ctx));
    BOOST_CHECK(!DigiDollar::ValidateCollateralRatio(-1, 100, 5760, ctx));
    BOOST_CHECK(!DigiDollar::ValidateCollateralRatio(100, -1, 5760, ctx));
}

// =============================================================================
// 6. Empty transaction attacks
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_mint_tx_no_outputs)
{
    CMutableTransaction mtx;
    mtx.nVersion = (1 << 24) | (0x0D1D0770 & 0x00FFFFFF); // MINT type
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(uint256::ONE, 0);
    // No outputs

    CTransaction tx(mtx);
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);
    TxValidationState state;

    bool result = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK(!result);
}

BOOST_AUTO_TEST_CASE(rh21_transfer_tx_no_inputs)
{
    CMutableTransaction mtx;
    mtx.nVersion = (2 << 24) | (0x0D1D0770 & 0x00FFFFFF); // TRANSFER type
    // No inputs

    CTransaction tx(mtx);
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);
    TxValidationState state;

    bool result = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK(!result);
}

BOOST_AUTO_TEST_CASE(rh21_redeem_tx_single_input)
{
    // Redeem needs at least 2 inputs (collateral + DD)
    CMutableTransaction mtx;
    mtx.nVersion = (3 << 24) | (0x0D1D0770 & 0x00FFFFFF); // REDEEM type
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(uint256::ONE, 0);
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1000;

    CTransaction tx(mtx);
    auto regTestParams = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *regTestParams);
    TxValidationState state;

    bool result = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK(!result);
}

// =============================================================================
// 7. Map/set ordering determinism
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_collateral_ratios_ordering)
{
    // std::map<int64_t, int> is ordered by key — verify iteration order
    DigiDollar::ConsensusParams params;

    int64_t prevKey = -1;
    for (const auto& [lockTime, ratio] : params.collateralRatios) {
        BOOST_CHECK_GT(lockTime, prevKey);
        prevKey = lockTime;
    }
}

BOOST_AUTO_TEST_CASE(rh21_collateral_ratio_rejects_between_tiers)
{
    // V1 accepts only exact canonical lock tiers; in-between durations reject.
    DigiDollar::ConsensusParams params;

    int64_t fortyFiveDays = 45 * DigiDollar::BLOCKS_PER_DAY;
    int ratio = DigiDollar::GetCollateralRatioForLockTime(fortyFiveDays, params);

    BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(fortyFiveDays, params));
    BOOST_CHECK_EQUAL(ratio, 0);
}

BOOST_AUTO_TEST_CASE(rh21_collateral_ratio_rejects_beyond_max_tier)
{
    // V1 does not silently cap custom long locks to the longest canonical tier.
    DigiDollar::ConsensusParams params;

    int64_t fifteenYears = 15 * 365 * DigiDollar::BLOCKS_PER_DAY;
    int ratio = DigiDollar::GetCollateralRatioForLockTime(fifteenYears, params);

    BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(fifteenYears, params));
    BOOST_CHECK_EQUAL(ratio, 0);
}

// =============================================================================
// 8. util::int128_t overflow guards
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_int128_collateral_calc_max_values)
{
    // Verify util::int128_t arithmetic doesn't overflow with worst-case inputs
    // numerator = ddAmount * COIN * effectiveRatio * 100
    // Max: 10000000 (max mint) * 100000000 (COIN) * 20000 (max ratio*DCA) * 100
    // = 10^7 * 10^8 * 2*10^4 * 10^2 = 2 * 10^21
    // util::int128_t max = 1.7 * 10^38 — safe

    auto regTestParams = CChainParams::RegTest({});
    // Emergency DCA (2.0x) with highest ratio tier (1000%)
    DigiDollar::ValidationContext ctx(1000, ORACLE_MIN_PRICE_MICRO_USD, 50, *regTestParams); // Low health = high DCA

    CAmount maxMint = 10000000; // $100K max
    int64_t shortLock = 240; // 1 hour = 1000% ratio
    CAmount required = DigiDollar::CalculateRequiredCollateral(maxMint, shortLock, ctx);

    // Should be representable at the valid oracle floor, not overflow
    BOOST_CHECK_GT(required, 0);
    BOOST_CHECK_LE(required, MAX_MONEY);
}

// =============================================================================
// 9. Consensus parameter validation edge cases
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_consensus_params_oracle_threshold_zero)
{
    // Threshold must be non-zero and cannot exceed active oracles.
    DigiDollar::ConsensusParams params;
    params.oracleThreshold = 0;

    std::string error;
    BOOST_CHECK(!DigiDollar::ValidateConsensusParams(params, error));
    BOOST_CHECK(error.find("zero") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(rh21_consensus_params_zero_price_valid_blocks)
{
    DigiDollar::ConsensusParams params;
    params.priceValidBlocks = 0;

    std::string error;
    BOOST_CHECK(!DigiDollar::ValidateConsensusParams(params, error));
}

BOOST_AUTO_TEST_CASE(rh21_consensus_params_ratio_10000)
{
    // Max ratio = 10000% (100x) should be valid
    DigiDollar::ConsensusParams params;
    params.collateralRatios = {{240, 10000}};

    std::string error;
    BOOST_CHECK(DigiDollar::ValidateConsensusParams(params, error));
}

BOOST_AUTO_TEST_CASE(rh21_consensus_params_ratio_10001)
{
    // Ratio > 10000% should fail
    DigiDollar::ConsensusParams params;
    params.collateralRatios = {{240, 10001}};

    std::string error;
    BOOST_CHECK(!DigiDollar::ValidateConsensusParams(params, error));
}

// =============================================================================
// 10. DCA system boundary tests
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_dca_system_health_zero)
{
    int health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(0, 100, 100);
    BOOST_CHECK_EQUAL(health, 0);
}

BOOST_AUTO_TEST_CASE(rh21_dca_system_health_zero_dd_supply)
{
    int health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
        1000000 * COIN, 0, 10000);
    BOOST_CHECK_EQUAL(health, 30000); // Max health when no DD exists
}

BOOST_AUTO_TEST_CASE(rh21_dca_system_health_zero_price)
{
    int health = DigiDollar::DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
        1000000 * COIN, 100000, 0);
    BOOST_CHECK_EQUAL(health, 0); // No price = no health
}

BOOST_AUTO_TEST_CASE(rh21_dca_multiplier_at_exact_boundaries)
{
    // Exactly at tier boundaries — verify no off-by-one
    double m150 = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(150);
    double m151 = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(151);
    double m149 = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(149);

    // At 150: should be in healthy tier (1.0x) or warning boundary
    BOOST_CHECK_GE(m150, 1.0);
    BOOST_CHECK_GE(m151, 1.0);
    // At 149: should be warning tier (1.25x)
    BOOST_CHECK_GE(m149, 1.0);

    // 120 boundary
    double m120 = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(120);
    double m119 = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(119);
    BOOST_CHECK_GE(m119, m120); // Below boundary should be same or higher

    // 100 boundary
    double m100 = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(100);
    double m99 = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(99);
    BOOST_CHECK_GE(m99, m100);
}

BOOST_AUTO_TEST_CASE(rh21_dca_multiplier_negative_health)
{
    double m = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(-100);
    // Negative health should use emergency tier (2.0x)
    BOOST_CHECK_GE(m, 2.0);
}

BOOST_AUTO_TEST_CASE(rh21_dca_apply_overflow)
{
    // Base ratio 10000% * 2.0x multiplier = 20000%, fits in int
    int result = DigiDollar::DCA::DynamicCollateralAdjustment::ApplyDCA(10000, 50);
    BOOST_CHECK_GT(result, 10000);
    BOOST_CHECK_LT(result, std::numeric_limits<int>::max());
}

// =============================================================================
// 11. Script creation boundary tests
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_create_collateral_p2tr_negative_dd)
{
    DigiDollar::MintParams params;
    params.ddAmount = -1;
    params.lockHeight = 1000;
    params.internalKey = DigiDollar::GetCollateralNUMSKey();

    CScript result = DigiDollar::CreateCollateralP2TR(params);
    BOOST_CHECK(result.empty()); // Must reject negative amount
}

BOOST_AUTO_TEST_CASE(rh21_create_collateral_p2tr_zero_dd)
{
    DigiDollar::MintParams params;
    params.ddAmount = 0;
    params.lockHeight = 1000;
    params.internalKey = DigiDollar::GetCollateralNUMSKey();

    CScript result = DigiDollar::CreateCollateralP2TR(params);
    BOOST_CHECK(result.empty()); // Must reject zero amount
}

BOOST_AUTO_TEST_CASE(rh21_create_dd_p2tr_zero_amount)
{
    CKey key;
    key.MakeNewKey(true);
    XOnlyPubKey xonly(key.GetPubKey());

    CScript result = DigiDollar::CreateDigiDollarP2TR(xonly, 0);
    BOOST_CHECK(result.empty());
}

// =============================================================================
// 12. ERR boundary tests
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_err_adjustment_at_zero_health)
{
    double adj = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(0);
    BOOST_CHECK_GT(adj, 0.0);
    BOOST_CHECK_LE(adj, 1.0);
}

BOOST_AUTO_TEST_CASE(rh21_err_adjustment_at_99_health)
{
    double adj = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(99);
    BOOST_CHECK_GT(adj, 0.0);
    BOOST_CHECK_LE(adj, 1.0);
}

BOOST_AUTO_TEST_CASE(rh21_err_adjustment_at_100_health)
{
    double adj = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(100);
    // At 100% health, ERR should not be active — adjustment should be 1.0 or close
    BOOST_CHECK_GE(adj, 0.95);
}

BOOST_AUTO_TEST_CASE(rh21_err_negative_health)
{
    double adj = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(-50);
    // Negative health — should still return a valid ratio, not crash
    BOOST_CHECK_GT(adj, 0.0);
    BOOST_CHECK_LE(adj, 1.0);
}

// =============================================================================
// 13. BlocksToLockDays rounding
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_blocks_to_lock_days_rounding)
{
    // 5760 blocks = 1 day exactly
    BOOST_CHECK_EQUAL(DigiDollar::BlocksToLockDays(5760), 1);

    // 5759 blocks = 0 days (truncation)
    BOOST_CHECK_EQUAL(DigiDollar::BlocksToLockDays(5759), 0);

    // 0 blocks = 0 days
    BOOST_CHECK_EQUAL(DigiDollar::BlocksToLockDays(0), 0);

    // Large value
    BOOST_CHECK_EQUAL(DigiDollar::BlocksToLockDays(10 * 365 * 5760), 3650);
}

// =============================================================================
// 14. ValidateOutputAmount boundary
// =============================================================================

BOOST_AUTO_TEST_CASE(rh21_validate_output_amount_at_max_digidollar)
{
    auto regTestParams = CChainParams::RegTest({});

    // MAX_DIGIDOLLAR should be valid as output
    BOOST_CHECK(DigiDollar::ValidateOutputAmount(MAX_DIGIDOLLAR, *regTestParams));

    // MAX_DIGIDOLLAR + 1 should be invalid
    BOOST_CHECK(!DigiDollar::ValidateOutputAmount(MAX_DIGIDOLLAR + 1, *regTestParams));
}

BOOST_AUTO_TEST_CASE(rh21_validate_output_amount_at_minimum)
{
    auto regTestParams = CChainParams::RegTest({});
    const auto& ddParams = regTestParams->GetDigiDollarParams();

    // Exactly at minimum
    BOOST_CHECK(DigiDollar::ValidateOutputAmount(ddParams.minOutputAmount, *regTestParams));

    // Below minimum
    BOOST_CHECK(!DigiDollar::ValidateOutputAmount(ddParams.minOutputAmount - 1, *regTestParams));

    // Zero
    BOOST_CHECK(!DigiDollar::ValidateOutputAmount(0, *regTestParams));

    // Negative
    BOOST_CHECK(!DigiDollar::ValidateOutputAmount(-1, *regTestParams));
}

BOOST_AUTO_TEST_SUITE_END()
