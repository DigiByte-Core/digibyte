// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Pins the Thaw Day activation predicate and the height each caller must
// feed it, before any consensus rule depends on it.
//
// Thaw Day is the single block height (Consensus::Params::nDDThawDayHeight)
// at which every consensus change of the v9.26.6 release takes effect. The
// predicate DigiDollar::IsThawDayActive(params, candidate_height) is a pure
// function of the parameters and one height: true only when Thaw Day is
// scheduled on the network, DigiDollar is active at the candidate height,
// and the candidate height is at or above Thaw Day.
//
// The table below enumerates every combination of {not scheduled, scheduled
// at H} x {DigiDollar active before H, at H, after H, never} x heights
// {0, H-1, H, H+1, INT_MAX-1} with hand-written expectations, so a mistake
// in the predicate cannot be mirrored by a mistake in the expectation. The
// last suite expresses which height each caller passes: a block's own height
// for validation and replay, parent + 1 for mining, active tip + 1 for
// mempool admission and wallet construction.

#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <string>
#include <vector>

namespace {

constexpr int MAX_INT = std::numeric_limits<int>::max();
// The largest int value is the "not scheduled on this network" sentinel.
constexpr int NOT_SCHEDULED = MAX_INT;
// Thaw Day height used by the table.
constexpr int H = 1000;
// DigiDollar activation heights relative to H.
constexpr int DD_BEFORE = H - 100;
constexpr int DD_AT = H;
constexpr int DD_AFTER = H + 100;
constexpr int DD_NEVER = MAX_INT;

Consensus::Params MakeParams(int thaw_day_height, int digidollar_height)
{
    Consensus::Params params{};
    params.nDDThawDayHeight = thaw_day_height;
    params.DigiDollarHeight = digidollar_height;
    return params;
}

std::string Describe(int thaw_day_height, int digidollar_height, int candidate_height)
{
    return "thaw=" + std::to_string(thaw_day_height) +
           " digidollar=" + std::to_string(digidollar_height) +
           " candidate=" + std::to_string(candidate_height);
}

struct Row {
    int thaw_day_height;
    int digidollar_height;
    int candidate_height;
    bool expected;
};

// Every expectation is written out by hand.
const std::vector<Row> TABLE{
    // Not scheduled: never active, whatever DigiDollar does.
    {NOT_SCHEDULED, DD_BEFORE, 0, false},
    {NOT_SCHEDULED, DD_BEFORE, H - 1, false},
    {NOT_SCHEDULED, DD_BEFORE, H, false},
    {NOT_SCHEDULED, DD_BEFORE, H + 1, false},
    {NOT_SCHEDULED, DD_BEFORE, MAX_INT - 1, false},
    {NOT_SCHEDULED, DD_AT, 0, false},
    {NOT_SCHEDULED, DD_AT, H - 1, false},
    {NOT_SCHEDULED, DD_AT, H, false},
    {NOT_SCHEDULED, DD_AT, H + 1, false},
    {NOT_SCHEDULED, DD_AT, MAX_INT - 1, false},
    {NOT_SCHEDULED, DD_AFTER, 0, false},
    {NOT_SCHEDULED, DD_AFTER, H - 1, false},
    {NOT_SCHEDULED, DD_AFTER, H, false},
    {NOT_SCHEDULED, DD_AFTER, H + 1, false},
    {NOT_SCHEDULED, DD_AFTER, MAX_INT - 1, false},
    {NOT_SCHEDULED, DD_NEVER, 0, false},
    {NOT_SCHEDULED, DD_NEVER, H - 1, false},
    {NOT_SCHEDULED, DD_NEVER, H, false},
    {NOT_SCHEDULED, DD_NEVER, H + 1, false},
    {NOT_SCHEDULED, DD_NEVER, MAX_INT - 1, false},

    // Scheduled at H, DigiDollar active before H: on from H exactly.
    {H, DD_BEFORE, 0, false},
    {H, DD_BEFORE, H - 1, false},
    {H, DD_BEFORE, H, true},
    {H, DD_BEFORE, H + 1, true},
    {H, DD_BEFORE, MAX_INT - 1, true},

    // Scheduled at H, DigiDollar activates at H as well: on from H exactly.
    {H, DD_AT, 0, false},
    {H, DD_AT, H - 1, false},
    {H, DD_AT, H, true},
    {H, DD_AT, H + 1, true},
    {H, DD_AT, MAX_INT - 1, true},

    // Scheduled at H, DigiDollar activates after H: Thaw Day waits for
    // DigiDollar, so H and H+1 are still off.
    {H, DD_AFTER, 0, false},
    {H, DD_AFTER, H - 1, false},
    {H, DD_AFTER, H, false},
    {H, DD_AFTER, H + 1, false},
    {H, DD_AFTER, MAX_INT - 1, true},

    // Scheduled at H, DigiDollar never active: never on.
    {H, DD_NEVER, 0, false},
    {H, DD_NEVER, H - 1, false},
    {H, DD_NEVER, H, false},
    {H, DD_NEVER, H + 1, false},
    {H, DD_NEVER, MAX_INT - 1, false},
};

// Which height each caller hands to the predicate. These are the contract
// the consensus, mining, and mempool code must follow when they start
// consulting Thaw Day.

// Validating or replaying a block judges that block by its own height. The
// current tip is deliberately not an input: a block below Thaw Day keeps the
// old rules even when the node's tip is far above it.
int CandidateHeightForBlock(int block_height)
{
    return block_height;
}

// Mining builds a candidate on top of a parent, so the candidate's height
// is the parent's height plus one.
int CandidateHeightForMining(int parent_height)
{
    return parent_height + 1;
}

// Mempool admission, revalidation, and wallet construction target the next
// block on the node's active chain: the tip height plus one.
int CandidateHeightForNextBlock(int active_tip_height)
{
    return active_tip_height + 1;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_thawday_selection_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(scheduled_flag_follows_the_sentinel_only)
{
    // Not scheduled, whatever DigiDollar does.
    for (int dd : {DD_BEFORE, DD_AT, DD_AFTER, DD_NEVER, 0}) {
        BOOST_CHECK_MESSAGE(!DigiDollar::IsThawDayScheduled(MakeParams(NOT_SCHEDULED, dd)),
                            "not scheduled with digidollar=" << dd);
    }
    // Any other value, including 0 and the largest non-sentinel value, is a
    // schedule.
    for (int thaw : {0, 1, H, MAX_INT - 1}) {
        BOOST_CHECK_MESSAGE(DigiDollar::IsThawDayScheduled(MakeParams(thaw, DD_BEFORE)),
                            "scheduled at " << thaw);
        BOOST_CHECK_MESSAGE(DigiDollar::IsThawDayScheduled(MakeParams(thaw, DD_NEVER)),
                            "scheduled at " << thaw << " even though DigiDollar never activates");
    }
}

BOOST_AUTO_TEST_CASE(activation_table)
{
    BOOST_REQUIRE_EQUAL(TABLE.size(), 40U);
    for (const Row& row : TABLE) {
        const Consensus::Params params = MakeParams(row.thaw_day_height, row.digidollar_height);
        const bool active = DigiDollar::IsThawDayActive(params, row.candidate_height);
        BOOST_CHECK_MESSAGE(active == row.expected,
                            Describe(row.thaw_day_height, row.digidollar_height, row.candidate_height)
                                << " expected " << row.expected << " got " << active);
    }
}

BOOST_AUTO_TEST_CASE(negative_candidate_height_is_never_active)
{
    for (int thaw : {0, H}) {
        for (int dd : {0, DD_BEFORE}) {
            const Consensus::Params params = MakeParams(thaw, dd);
            for (int candidate : {-1, -H, std::numeric_limits<int>::min()}) {
                BOOST_CHECK_MESSAGE(!DigiDollar::IsThawDayActive(params, candidate),
                                    Describe(thaw, dd, candidate) << " must be inactive");
            }
            // The schedule itself does not depend on any candidate.
            BOOST_CHECK(DigiDollar::IsThawDayScheduled(params));
        }
    }
}

BOOST_AUTO_TEST_CASE(scheduled_at_genesis)
{
    // Thaw Day at height 0 with DigiDollar active from genesis: on from the
    // genesis block.
    const Consensus::Params from_genesis = MakeParams(0, 0);
    BOOST_CHECK(DigiDollar::IsThawDayActive(from_genesis, 0));
    BOOST_CHECK(DigiDollar::IsThawDayActive(from_genesis, 1));

    // Thaw Day at height 0 but DigiDollar only from height 5: DigiDollar's
    // own activation is the boundary.
    const Consensus::Params dd_later = MakeParams(0, 5);
    BOOST_CHECK(!DigiDollar::IsThawDayActive(dd_later, 0));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(dd_later, 4));
    BOOST_CHECK(DigiDollar::IsThawDayActive(dd_later, 5));
    BOOST_CHECK(DigiDollar::IsThawDayActive(dd_later, 6));
}

BOOST_AUTO_TEST_CASE(no_arithmetic_on_the_scheduled_height)
{
    // The largest non-sentinel schedule must work without adding anything to
    // it: a "+1" would overflow and never match.
    const Consensus::Params extreme = MakeParams(MAX_INT - 1, 0);
    BOOST_CHECK(DigiDollar::IsThawDayScheduled(extreme));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(extreme, MAX_INT - 2));
    BOOST_CHECK(DigiDollar::IsThawDayActive(extreme, MAX_INT - 1));

    // The sentinel stays inactive at the largest heights too.
    const Consensus::Params unscheduled = MakeParams(NOT_SCHEDULED, 0);
    BOOST_CHECK(!DigiDollar::IsThawDayActive(unscheduled, MAX_INT - 1));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(unscheduled, MAX_INT));
}

BOOST_AUTO_TEST_CASE(which_height_each_caller_uses)
{
    const Consensus::Params params = MakeParams(H, DD_BEFORE);
    const auto active = [&params](int candidate) { return DigiDollar::IsThawDayActive(params, candidate); };

    // Validation and replay: the block's own height. Height alone decides;
    // the node's tip at the time of validation plays no part.
    BOOST_CHECK(!active(CandidateHeightForBlock(0)));
    BOOST_CHECK(!active(CandidateHeightForBlock(H - 1)));
    BOOST_CHECK(active(CandidateHeightForBlock(H)));
    BOOST_CHECK(active(CandidateHeightForBlock(H + 1)));

    // Mining: parent height plus one. Building on the parent at H-1 yields
    // the candidate at H, which may use the new rules. That is the expected
    // "next block" case, not early activation.
    BOOST_CHECK(!active(CandidateHeightForMining(H - 2)));
    BOOST_CHECK(active(CandidateHeightForMining(H - 1)));
    BOOST_CHECK(active(CandidateHeightForMining(H)));

    // Mempool admission and wallet construction: active tip plus one.
    BOOST_CHECK(!active(CandidateHeightForNextBlock(H - 2)));
    BOOST_CHECK(active(CandidateHeightForNextBlock(H - 1)));
    BOOST_CHECK(active(CandidateHeightForNextBlock(H)));

    // The three callers agree on the same next block: mining on a parent at
    // height h, admitting into the mempool with the tip at h, and validating
    // the block that ends up at h+1 must all pick the same rule set.
    for (int h = H - 3; h <= H + 3; ++h) {
        BOOST_CHECK_EQUAL(CandidateHeightForMining(h), CandidateHeightForNextBlock(h));
        BOOST_CHECK_EQUAL(CandidateHeightForMining(h), CandidateHeightForBlock(h + 1));
        BOOST_CHECK_EQUAL(active(CandidateHeightForMining(h)), active(CandidateHeightForBlock(h + 1)));
        BOOST_CHECK_EQUAL(active(CandidateHeightForNextBlock(h)), active(CandidateHeightForBlock(h + 1)));
    }

    // Reorg across Thaw Day: disconnecting the block at H leaves the tip at
    // H-1. The disconnected block was and stays a block at H (new rules); the
    // replacement candidate on the other branch is also at H (new rules); a
    // replacement branch that only reaches H-1 is still under the old rules.
    BOOST_CHECK(active(CandidateHeightForBlock(H)));
    BOOST_CHECK(active(CandidateHeightForNextBlock(H - 1)));
    BOOST_CHECK(!active(CandidateHeightForBlock(H - 1)));
    BOOST_CHECK(!active(CandidateHeightForNextBlock(H - 2)));

    // With DigiDollar activating after Thaw Day, every caller waits for
    // DigiDollar: the candidate at H is off, the candidate at DigiDollar's
    // own height is on.
    const Consensus::Params dd_after = MakeParams(H, DD_AFTER);
    BOOST_CHECK(!DigiDollar::IsThawDayActive(dd_after, CandidateHeightForMining(H - 1)));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(dd_after, CandidateHeightForNextBlock(H - 1)));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(dd_after, CandidateHeightForBlock(H)));
    BOOST_CHECK(!DigiDollar::IsThawDayActive(dd_after, CandidateHeightForBlock(DD_AFTER - 1)));
    BOOST_CHECK(DigiDollar::IsThawDayActive(dd_after, CandidateHeightForMining(DD_AFTER - 1)));
    BOOST_CHECK(DigiDollar::IsThawDayActive(dd_after, CandidateHeightForNextBlock(DD_AFTER - 1)));
    BOOST_CHECK(DigiDollar::IsThawDayActive(dd_after, CandidateHeightForBlock(DD_AFTER)));
}

BOOST_AUTO_TEST_SUITE_END()
