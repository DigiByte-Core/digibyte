// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <random.h>
#include <util/compact_work.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

void CheckWork(const CompactWork& stored, const arith_uint256& expected)
{
    BOOST_CHECK_EQUAL(stored.Get().GetHex(), expected.GetHex());
}

} // namespace

BOOST_AUTO_TEST_SUITE(compact_work_tests)

BOOST_AUTO_TEST_CASE(storage_budget)
{
    BOOST_CHECK_LE(sizeof(CompactWork), 16U);
    BOOST_CHECK(std::is_nothrow_move_constructible_v<CompactWork>);
    BOOST_CHECK(std::is_nothrow_move_assignable_v<CompactWork>);
}

BOOST_AUTO_TEST_CASE(all_bit_boundaries)
{
    CompactWork stored;
    CheckWork(stored, arith_uint256{0});
    for (int bit = 0; bit < 256; ++bit) {
        const arith_uint256 value = arith_uint256{1} << bit;
        for (const arith_uint256& boundary : std::array<arith_uint256, 3>{value - 1, value, value + 1}) {
            stored.Set(boundary);
            CheckWork(stored, boundary);
            CheckWork(CompactWork{boundary}, boundary);
        }
    }
    const arith_uint256 maximum = ~arith_uint256{0};
    stored.Set(maximum);
    CheckWork(stored, maximum);
    stored.Set(arith_uint256{0});
    CheckWork(stored, arith_uint256{0});
}

BOOST_AUTO_TEST_CASE(inline_boundary_and_random_values)
{
    const arith_uint256 first_large = arith_uint256{1} << 112;
    const arith_uint256 largest_small = first_large - 1;
    CompactWork stored{largest_small};
    for (int pass = 0; pass < 32; ++pass) {
        stored.Set(first_large);
        CheckWork(stored, first_large);
        stored.Set(largest_small);
        CheckWork(stored, largest_small);
    }

    FastRandomContext random{true};
    for (int pass = 0; pass < 1024; ++pass) {
        const arith_uint256 value = UintToArith256(random.rand256());
        stored.Set(value);
        CheckWork(stored, value);
        stored.Set(value & largest_small);
        CheckWork(stored, value & largest_small);
    }
}

BOOST_AUTO_TEST_CASE(copies_own_their_values)
{
    const arith_uint256 small{7};
    const arith_uint256 large = (arith_uint256{1} << 200) + 9;
    const arith_uint256 other_large = ~arith_uint256{0};
    CompactWork original{large};
    CompactWork copied{original};
    original.Set(small);
    CheckWork(copied, large);
    CheckWork(original, small);

    CompactWork assigned{other_large};
    assigned = original;
    original.Set(other_large);
    CheckWork(assigned, small);
    assigned = copied;
    copied.Set(other_large);
    CheckWork(assigned, large);
    assigned = original;
    CheckWork(assigned, other_large);

    for (const arith_uint256& value : {small, large}) {
        assigned.Set(value);
        const CompactWork* same = &assigned;
        assigned = *same;
        CheckWork(assigned, value);
    }
}

BOOST_AUTO_TEST_CASE(moves_leave_values_usable)
{
    const arith_uint256 small{3};
    const arith_uint256 large = (arith_uint256{1} << 255) + 5;
    for (const arith_uint256& source_value : {small, large}) {
        CompactWork source{source_value};
        CompactWork moved{std::move(source)};
        CheckWork(moved, source_value);
        source.Set(large);
        CheckWork(source, large);

        for (const arith_uint256& destination_value : {small, large}) {
            CompactWork destination{destination_value};
            source.Set(source_value);
            destination = std::move(source);
            CheckWork(destination, source_value);
            source.Set(small);
            CheckWork(source, small);

            CompactWork* same = &destination;
            destination = std::move(*same);
            CheckWork(destination, source_value);
        }
    }
}

BOOST_AUTO_TEST_CASE(vector_growth_and_erasure)
{
    std::vector<CompactWork> stored;
    std::vector<arith_uint256> expected;
    for (unsigned int i = 0; i < 1024; ++i) {
        const arith_uint256 value = (arith_uint256{i + 1} << (i % 2 ? 200 : 64)) + i;
        stored.emplace_back(value);
        expected.push_back(value);
    }
    stored.reserve(stored.capacity() * 2);
    stored.erase(stored.begin() + 17, stored.begin() + 301);
    expected.erase(expected.begin() + 17, expected.begin() + 301);
    for (size_t i = 0; i < expected.size(); ++i) CheckWork(stored[i], expected[i]);

    auto copied = stored;
    stored.clear();
    for (size_t i = 0; i < expected.size(); ++i) CheckWork(copied[i], expected[i]);
}

BOOST_AUTO_TEST_SUITE_END()
