// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <common/args.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <kernel/chainparams.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <stdexcept>

/**
 * Thaw Day activation predicate tests.
 *
 * Thaw Day is the single block height (Consensus::Params::nDDThawDayHeight)
 * at which every consensus change of the DigiDollar "Thaw Day" release takes
 * effect. This suite pins the contract of the two pure helpers:
 *
 *   DigiDollar::IsThawDayScheduled(params)      -> the field does not hold
 *                                                  the "not scheduled" value
 *   DigiDollar::IsThawDayActive(params, height) -> scheduled, DigiDollar
 *                                                  active at height, and
 *                                                  height >= nDDThawDayHeight
 *
 * It also checks the regtest-only -ddthawdayheight knob: it sets the field
 * exactly on regtest and is a startup error on every other network.
 */

namespace {

constexpr int NOT_SCHEDULED = std::numeric_limits<int>::max();
constexpr int INT_MAX_VALUE = std::numeric_limits<int>::max();

// A params value with DigiDollar active from genesis and Thaw Day not scheduled.
Consensus::Params DigiDollarActiveParams()
{
    Consensus::Params params = CChainParams::RegTest(CChainParams::RegTestOptions{})->GetConsensus();
    params.DigiDollarHeight = 0;
    params.nDDThawDayHeight = NOT_SCHEDULED;
    return params;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_thawday_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(not_scheduled_is_false_at_every_height)
{
    const Consensus::Params params = DigiDollarActiveParams();
    BOOST_CHECK(!DigiDollar::IsThawDayScheduled(params));
    for (const int height : {0, 1, 649, 650, 651, 1000000, INT_MAX_VALUE - 1, INT_MAX_VALUE}) {
        BOOST_CHECK_MESSAGE(!DigiDollar::IsThawDayActive(params, height), "unexpectedly active at height " << height);
    }
    // The regtest defaults themselves (no knob given) must also be "not scheduled".
    const auto regtest = CChainParams::RegTest(CChainParams::RegTestOptions{});
    BOOST_CHECK(!DigiDollar::IsThawDayScheduled(regtest->GetConsensus()));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(regtest->GetConsensus(), INT_MAX_VALUE));
}

BOOST_AUTO_TEST_CASE(scheduled_with_digidollar_active_flips_exactly_at_height)
{
    constexpr int H = 1000;
    Consensus::Params params = DigiDollarActiveParams();
    params.nDDThawDayHeight = H;
    BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 0));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, H - 1));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, H));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, H + 1));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, INT_MAX_VALUE));
}

BOOST_AUTO_TEST_CASE(scheduled_before_digidollar_activation_waits_for_digidollar)
{
    // Thaw Day scheduled at 1000 but DigiDollar only activates at 2000: the
    // Thaw Day rules cannot apply while DigiDollar itself is inactive, so the
    // predicate stays false until the DigiDollar height and is true from there.
    Consensus::Params params = DigiDollarActiveParams();
    params.nDDThawDayHeight = 1000;
    params.DigiDollarHeight = 2000;
    BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 999));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 1000));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 1001));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 1999));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, 2000));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, 2001));

    // The same schedule on a network where the DigiDollar deployment is
    // disabled altogether ("disabled" is the same int max value for the
    // buried deployment): never active, at any height including int max,
    // because such a network can never have Thaw Day rules at all.
    params.DigiDollarHeight = NOT_SCHEDULED;
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 1000));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, INT_MAX_VALUE - 1));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, INT_MAX_VALUE));
}

BOOST_AUTO_TEST_CASE(negative_candidate_height_is_false)
{
    Consensus::Params params = DigiDollarActiveParams();
    params.nDDThawDayHeight = 0;
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, 0));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, -1));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, std::numeric_limits<int>::min()));
    params.nDDThawDayHeight = NOT_SCHEDULED;
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, -1));
}

BOOST_AUTO_TEST_CASE(largest_schedulable_height_needs_no_arithmetic)
{
    // The largest value the regtest knob accepts is int max - 1. A predicate
    // that added one to the configured height would overflow here; a correct
    // one compares directly.
    Consensus::Params params = DigiDollarActiveParams();
    params.nDDThawDayHeight = INT_MAX_VALUE - 1;
    BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, INT_MAX_VALUE - 2));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, INT_MAX_VALUE - 1));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, INT_MAX_VALUE));
}

BOOST_AUTO_TEST_CASE(regtest_knob_sets_height_and_public_networks_reject_it)
{
    // No knob: regtest is not scheduled.
    {
        const auto params = CreateChainParams(ArgsManager{}, ChainType::REGTEST);
        BOOST_CHECK_EQUAL(params->GetConsensus().nDDThawDayHeight, NOT_SCHEDULED);
        BOOST_CHECK(!DigiDollar::IsThawDayScheduled(params->GetConsensus()));
    }
    // Knob alone: sets the field exactly.
    {
        ArgsManager args;
        args.ForceSetArg("-ddthawdayheight", "1234");
        const auto params = CreateChainParams(args, ChainType::REGTEST);
        BOOST_CHECK_EQUAL(params->GetConsensus().nDDThawDayHeight, 1234);
        BOOST_CHECK(DigiDollar::IsThawDayScheduled(params->GetConsensus()));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params->GetConsensus(), 1233));
        BOOST_CHECK(DigiDollar::IsThawDayActive(params->GetConsensus(), 1234));
    }
    // Knob together with the DigiDollar activation knob: the two are
    // independent, and a Thaw Day scheduled before DigiDollar activation
    // only becomes active once DigiDollar is.
    {
        ArgsManager args;
        args.ForceSetArg("-digidollaractivationheight", "500");
        args.ForceSetArg("-ddthawdayheight", "400");
        const auto params = CreateChainParams(args, ChainType::REGTEST);
        BOOST_CHECK_EQUAL(params->GetConsensus().DigiDollarHeight, 500);
        BOOST_CHECK_EQUAL(params->GetConsensus().nDDThawDayHeight, 400);
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params->GetConsensus(), 499));
        BOOST_CHECK(DigiDollar::IsThawDayActive(params->GetConsensus(), 500));
    }
    // Out-of-range or malformed values are rejected on regtest.
    for (const char* bad : {"-1", "2147483647", "2147483648", "abc", ""}) {
        ArgsManager args;
        args.ForceSetArg("-ddthawdayheight", bad);
        BOOST_CHECK_THROW(CreateChainParams(args, ChainType::REGTEST), std::runtime_error);
    }
    // Any value on a public network is a startup error.
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET}) {
        ArgsManager args;
        args.ForceSetArg("-ddthawdayheight", "1234");
        BOOST_CHECK_THROW(CreateChainParams(args, chain), std::runtime_error);
    }
}

BOOST_AUTO_TEST_SUITE_END()
