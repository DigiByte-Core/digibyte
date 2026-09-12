// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Network-configuration checks for the Thaw Day height
// (Consensus::Params::nDDThawDayHeight): the one block height at which every
// consensus change of the "Thaw Day" release takes effect.
//
// In this release step no network schedules Thaw Day. Mainnet, testnet26,
// signet and default regtest all keep the "not scheduled" value (the maximum
// int). The only way to schedule it is the regtest-only -ddthawdayheight
// option, which sets the field exactly and is refused on every public network.
// These tests pin that configuration matrix at the chainparams / option layer,
// the same layer digidollar_activation_tests.cpp uses for
// -digidollaractivationheight, so a stray placeholder height, an accidental
// default of zero, or a knob that leaks onto a public network fails here and
// not on a live node.

#include <chainparams.h>
#include <common/args.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <kernel/chainparams.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

// The "not scheduled on this network" value of nDDThawDayHeight.
constexpr int NOT_SCHEDULED = std::numeric_limits<int>::max();

// The height the regtest option schedules in these tests.
constexpr int KNOB_HEIGHT = 700;

// Build the chain params for a network the way startup does: through
// CreateChainParams with the given -ddthawdayheight value (empty = not given).
std::unique_ptr<const CChainParams> ParamsWithThawDayArg(ChainType chain, const std::string& thaw_day_arg)
{
    ArgsManager args;
    if (!thaw_day_arg.empty()) args.ForceSetArg("-ddthawdayheight", thaw_day_arg);
    return CreateChainParams(args, chain);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_thawday_chainparams_tests, BasicTestingSetup)

// Without the option, every network reports "not scheduled", and the
// predicate says the rules apply nowhere, including at the largest heights.
BOOST_AUTO_TEST_CASE(no_network_schedules_thaw_day_by_default)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET, ChainType::REGTEST}) {
        const auto chainparams = ParamsWithThawDayArg(chain, "");
        const Consensus::Params& params = chainparams->GetConsensus();

        BOOST_CHECK_EQUAL(params.nDDThawDayHeight, NOT_SCHEDULED);
        BOOST_CHECK(!DigiDollar::IsThawDayScheduled(params));

        // Not scheduled means never active, whatever the height, even where
        // DigiDollar itself is active. The last value is the sentinel itself,
        // so this also proves nothing adds one to it.
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 0));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, params.DigiDollarHeight));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, NOT_SCHEDULED - 1));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, NOT_SCHEDULED));
    }
}

// The per-network DigiDollar activation heights are untouched by adding the
// Thaw Day field: the two heights are independent.
BOOST_AUTO_TEST_CASE(digidollar_activation_heights_are_unchanged)
{
    BOOST_CHECK_EQUAL(CChainParams::Main()->GetConsensus().DigiDollarHeight, 23869440);
    BOOST_CHECK_EQUAL(CChainParams::TestNet()->GetConsensus().DigiDollarHeight, 600);
    BOOST_CHECK_EQUAL(CChainParams::SigNet(CChainParams::SigNetOptions{})->GetConsensus().DigiDollarHeight, 0);
    BOOST_CHECK_EQUAL(CChainParams::RegTest(CChainParams::RegTestOptions{})->GetConsensus().DigiDollarHeight, 0);
}

// On regtest the option sets the height exactly and touches nothing else.
BOOST_AUTO_TEST_CASE(regtest_option_sets_the_height_exactly)
{
    const auto chainparams = ParamsWithThawDayArg(ChainType::REGTEST, std::to_string(KNOB_HEIGHT));
    const Consensus::Params& params = chainparams->GetConsensus();

    BOOST_CHECK_EQUAL(params.nDDThawDayHeight, KNOB_HEIGHT);
    BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));

    // Default regtest has DigiDollar active from genesis, so the option alone
    // decides: the block before Thaw Day keeps the old rules, the block at
    // Thaw Day and every later block use the new ones.
    BOOST_CHECK_EQUAL(params.DigiDollarHeight, 0);
    BOOST_CHECK(!DigiDollar::IsThawDayActive(params, KNOB_HEIGHT - 1));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, KNOB_HEIGHT));
    BOOST_CHECK(DigiDollar::IsThawDayActive(params, KNOB_HEIGHT + 1));

    // The DigiDollar gates keep their regtest defaults.
    BOOST_CHECK_EQUAL(params.nDDActivationHeight, 650);
    BOOST_CHECK_EQUAL(params.nOracleActivationHeight, 650);
    BOOST_CHECK_EQUAL(params.nDigiDollarMuSig2Height, 0);
}

// Zero is a valid schedule (new rules from genesis, where DigiDollar is
// already active on default regtest), and the largest schedulable height is
// accepted without any arithmetic on it.
BOOST_AUTO_TEST_CASE(regtest_option_accepts_the_whole_valid_range)
{
    {
        const auto chainparams = ParamsWithThawDayArg(ChainType::REGTEST, "0");
        const Consensus::Params& params = chainparams->GetConsensus();
        BOOST_CHECK_EQUAL(params.nDDThawDayHeight, 0);
        BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));
        BOOST_CHECK(DigiDollar::IsThawDayActive(params, 0));
    }
    {
        const auto chainparams = ParamsWithThawDayArg(ChainType::REGTEST, std::to_string(NOT_SCHEDULED - 1));
        const Consensus::Params& params = chainparams->GetConsensus();
        BOOST_CHECK_EQUAL(params.nDDThawDayHeight, NOT_SCHEDULED - 1);
        BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, NOT_SCHEDULED - 2));
        BOOST_CHECK(DigiDollar::IsThawDayActive(params, NOT_SCHEDULED - 1));
    }
}

// A negative height and the "not scheduled" sentinel itself are refused at
// startup instead of being silently clamped or treated as a schedule.
BOOST_AUTO_TEST_CASE(regtest_option_rejects_out_of_range_values)
{
    BOOST_CHECK_THROW(ParamsWithThawDayArg(ChainType::REGTEST, "-1"), std::runtime_error);
    BOOST_CHECK_THROW(ParamsWithThawDayArg(ChainType::REGTEST, std::to_string(NOT_SCHEDULED)), std::runtime_error);
    BOOST_CHECK_THROW(ParamsWithThawDayArg(ChainType::REGTEST, "2147483648"), std::runtime_error);
}

// Thaw Day and the DigiDollar activation height are independent options:
// each can be scheduled before or after the other, and the predicate waits
// for whichever comes later, because the new rules need DigiDollar itself.
BOOST_AUTO_TEST_CASE(regtest_option_is_independent_of_digidollar_activation)
{
    {
        // Thaw Day before DigiDollar activation: scheduled, but nothing
        // applies until DigiDollar is active.
        ArgsManager args;
        args.ForceSetArg("-ddthawdayheight", "700");
        args.ForceSetArg("-digidollaractivationheight", "900");
        const auto chainparams = CreateChainParams(args, ChainType::REGTEST);
        const Consensus::Params& params = chainparams->GetConsensus();
        BOOST_CHECK_EQUAL(params.nDDThawDayHeight, 700);
        BOOST_CHECK_EQUAL(params.DigiDollarHeight, 900);
        BOOST_CHECK_EQUAL(params.nDDActivationHeight, 900);
        BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 700));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 899));
        BOOST_CHECK(DigiDollar::IsThawDayActive(params, 900));
    }
    {
        // Thaw Day after DigiDollar activation: the Thaw Day height governs.
        ArgsManager args;
        args.ForceSetArg("-ddthawdayheight", "900");
        args.ForceSetArg("-digidollaractivationheight", "700");
        const auto chainparams = CreateChainParams(args, ChainType::REGTEST);
        const Consensus::Params& params = chainparams->GetConsensus();
        BOOST_CHECK_EQUAL(params.nDDThawDayHeight, 900);
        BOOST_CHECK_EQUAL(params.DigiDollarHeight, 700);
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 700));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 899));
        BOOST_CHECK(DigiDollar::IsThawDayActive(params, 900));
    }
    {
        // The DigiDollar option alone never schedules Thaw Day.
        ArgsManager args;
        args.ForceSetArg("-digidollaractivationheight", "700");
        const auto chainparams = CreateChainParams(args, ChainType::REGTEST);
        const Consensus::Params& params = chainparams->GetConsensus();
        BOOST_CHECK_EQUAL(params.nDDThawDayHeight, NOT_SCHEDULED);
        BOOST_CHECK(!DigiDollar::IsThawDayScheduled(params));
        BOOST_CHECK(!DigiDollar::IsThawDayActive(params, 700));
    }
}

// The option is a startup error on every public network, so a production
// node can never be talked into an unpublished Thaw Day height. Without the
// option those networks still build, with "not scheduled".
BOOST_AUTO_TEST_CASE(option_is_refused_on_every_public_network)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET}) {
        BOOST_CHECK_THROW(ParamsWithThawDayArg(chain, std::to_string(KNOB_HEIGHT)), std::runtime_error);
        BOOST_CHECK_THROW(ParamsWithThawDayArg(chain, "0"), std::runtime_error);
        BOOST_CHECK_EQUAL(ParamsWithThawDayArg(chain, "")->GetConsensus().nDDThawDayHeight, NOT_SCHEDULED);
    }
}

BOOST_AUTO_TEST_SUITE_END()
