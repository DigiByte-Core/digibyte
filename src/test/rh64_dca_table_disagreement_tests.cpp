// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-64: DCA table agreement regression tests.
 *
 * The wallet/builder path reads DCA levels from chainparams through
 * DigiDollar::GetDCAMultiplier().  The validator path uses
 * DynamicCollateralAdjustment::GetDCAMultiplier()/ApplyDCA().
 *
 * If those tables diverge, a hand-crafted mint can satisfy consensus with
 * less collateral than honest wallet/RPC builders require. These tests bind
 * the two paths together at the known vulnerable bands and across the full
 * boundary sweep used by the original RH64 proof.
 */

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <chainparams.h>
#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <consensus/amount.h>
#include <digidollar/validation.h>
#include <test/util/setup_common.h>

#include <cmath>
#include <vector>

using namespace DigiDollar;
using namespace DigiDollar::DCA;

namespace {

double ChainparamsMultiplier(int health, const ConsensusParams& dd_params)
{
    return DigiDollar::GetDCAMultiplier(health, dd_params);
}

double ValidatorMultiplier(int health)
{
    return DynamicCollateralAdjustment::GetDCAMultiplier(health);
}

void CheckMultiplierMatch(int health, const ConsensusParams& dd_params)
{
    const double expected = ChainparamsMultiplier(health, dd_params);
    const double actual = ValidatorMultiplier(health);

    BOOST_CHECK_MESSAGE(std::fabs(expected - actual) < 1e-9,
        "DCA multiplier mismatch at health=" << health
        << ": chainparams/builder=" << expected
        << ", validator=" << actual);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(rh64_dca_table_disagreement_tests, RegTestingSetup)

BOOST_AUTO_TEST_CASE(rh64_01_dca_tables_match_at_boundaries)
{
    const auto& dd_params = Params().GetDigiDollarParams();

    const std::vector<int> boundary_health = {
        0, 50, 99,
        100, 105, 109,
        110, 115, 119,
        120, 125, 149,
        150, 200, 30000,
    };

    for (const int health : boundary_health) {
        CheckMultiplierMatch(health, dd_params);
    }

    BOOST_CHECK_EQUAL(ValidatorMultiplier(99), 2.0);
    BOOST_CHECK_EQUAL(ValidatorMultiplier(100), 2.0);
    BOOST_CHECK_EQUAL(ValidatorMultiplier(109), 2.0);
    BOOST_CHECK_EQUAL(ValidatorMultiplier(110), 1.5);
    BOOST_CHECK_EQUAL(ValidatorMultiplier(119), 1.5);
    BOOST_CHECK_EQUAL(ValidatorMultiplier(120), 1.25);
    BOOST_CHECK_EQUAL(ValidatorMultiplier(149), 1.25);
    BOOST_CHECK_EQUAL(ValidatorMultiplier(150), 1.0);
}

BOOST_AUTO_TEST_CASE(rh64_02_validator_requires_builder_collateral_at_health_105)
{
    const CChainParams& chain_params = Params();
    const auto& dd_params = chain_params.GetDigiDollarParams();

    const CAmount dd_amount = 10000;       // $100 in cents
    const int64_t lock_blocks = 30 * DigiDollar::BLOCKS_PER_DAY;
    const CAmount oracle_price_micro_usd = 10000; // $0.01/DGB
    const int system_health = 105;

    const int base_ratio = DigiDollar::GetCollateralRatioForLockTime(lock_blocks, dd_params);
    BOOST_REQUIRE_EQUAL(base_ratio, 500);

    const int expected_ratio = static_cast<int>(std::ceil(
        base_ratio * ChainparamsMultiplier(system_health, dd_params)));
    BOOST_REQUIRE_EQUAL(expected_ratio, 1000);

    const util::int128_t expected_num = static_cast<util::int128_t>(dd_amount) *
                                  static_cast<util::int128_t>(COIN) *
                                  static_cast<util::int128_t>(expected_ratio) * 100;
    const CAmount expected_required = static_cast<CAmount>(
        (expected_num + oracle_price_micro_usd - 1) / oracle_price_micro_usd);

    DigiDollar::ValidationContext ctx(/*height=*/1000,
                                      /*price_micro_usd=*/oracle_price_micro_usd,
                                      /*collateral=*/system_health,
                                      chain_params);

    const int validator_ratio = DigiDollar::GetEffectiveCollateralRatio(
        base_ratio, system_health, chain_params);
    const CAmount validator_required = DigiDollar::CalculateRequiredCollateral(
        dd_amount, lock_blocks, ctx);

    BOOST_CHECK_EQUAL(validator_ratio, expected_ratio);
    BOOST_CHECK_EQUAL(validator_required, expected_required);
}

BOOST_AUTO_TEST_CASE(rh64_03_full_boundary_sweep_has_no_table_disagreement)
{
    const auto& dd_params = Params().GetDigiDollarParams();

    for (int health = 0; health <= 30000; ++health) {
        CheckMultiplierMatch(health, dd_params);
    }
}

BOOST_AUTO_TEST_SUITE_END()
