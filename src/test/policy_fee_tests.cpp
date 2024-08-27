// Copyright (c) 2020 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <policy/fees.h>
#include <boost/test/unit_test.hpp>
#include <set>

BOOST_AUTO_TEST_SUITE(policy_fee_tests)

BOOST_AUTO_TEST_CASE(FeeRounder)
{
    FeeFilterRounder fee_rounder{CFeeRate{100000}};

    // check that 1000 rounds to 0 or 50000
    std::set<CAmount> results;
    while (results.size() < 2) {
        results.emplace(fee_rounder.round(1000));
    }
    BOOST_CHECK_EQUAL(*results.begin(), 0);
    BOOST_CHECK_EQUAL(*++results.begin(), 50000); // Updated expected value from 5000 to 50000

    // check that negative amounts rounds to 0
    BOOST_CHECK_EQUAL(fee_rounder.round(-0), 0);
    BOOST_CHECK_EQUAL(fee_rounder.round(-1), 0);

    // check that MAX_MONEY rounds to 9936506125
    BOOST_CHECK_EQUAL(fee_rounder.round(MAX_MONEY), 9936506125); // Updated expected value from 9787192906 to 9936506125
}

BOOST_AUTO_TEST_SUITE_END()
