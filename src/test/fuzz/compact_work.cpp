// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <util/compact_work.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

arith_uint256 ConsumeWork(FuzzedDataProvider& provider)
{
    const arith_uint256 first_large = arith_uint256{1} << 112;
    switch (provider.ConsumeIntegralInRange<unsigned>(0, 6)) {
    case 0:
        return 0;
    case 1:
        return ~arith_uint256{0};
    case 2:
        return ConsumeArithUInt256(provider);
    case 3:
        return ConsumeArithUInt256(provider) & (first_large - 1);
    case 4:
        return arith_uint256{1} << provider.ConsumeIntegralInRange<unsigned>(0, 255);
    case 5:
        return (arith_uint256{1} << provider.ConsumeIntegralInRange<unsigned>(0, 255)) - 1;
    default:
        return provider.PickValueInArray(std::array<arith_uint256, 3>{first_large - 1, first_large, first_large + 1});
    }
}

void CheckValues(const std::vector<CompactWork>& stored, const std::vector<arith_uint256>& expected)
{
    assert(stored.size() == expected.size());
    for (size_t i = 0; i < stored.size(); ++i) assert(stored[i].Get() == expected[i]);
}

} // namespace

FUZZ_TARGET(compact_work)
{
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    const arith_uint256 first_large = arith_uint256{1} << 112;
    std::vector<arith_uint256> expected{arith_uint256{0}, first_large - 1, first_large, ~arith_uint256{0}};
    std::vector<CompactWork> stored;
    for (const auto& value : expected) stored.emplace_back(value);
    CheckValues(stored, expected);

    LIMITED_WHILE(provider.remaining_bytes() > 0, 256) {
        const size_t source = provider.ConsumeIntegralInRange<size_t>(0, stored.size() - 1);
        const size_t destination = provider.ConsumeIntegralInRange<size_t>(0, stored.size() - 1);
        CallOneOf(
            provider,
            [&] {
                const auto value = ConsumeWork(provider);
                stored[destination].Set(value);
                expected[destination] = value;
            },
            [&] {
                stored[destination] = stored[source];
                expected[destination] = expected[source];
            },
            [&] {
                stored[destination] = std::move(stored[source]);
                expected[destination] = expected[source];
                assert(stored[destination].Get() == expected[destination]);
                if (source != destination) {
                    // A moved-from value must remain usable; its contents need not be retained.
                    expected[source] = ConsumeWork(provider);
                    stored[source].Set(expected[source]);
                }
            },
            [&] {
                CompactWork copy{stored[source]};
                assert(copy.Get() == expected[source]);
                const auto value = ConsumeWork(provider);
                copy.Set(value);
                assert(stored[source].Get() == expected[source]);
                stored[destination] = copy;
                expected[destination] = value;
            },
            [&] {
                const auto value = expected[source];
                CompactWork moved{std::move(stored[source])};
                expected[source] = ConsumeWork(provider);
                stored[source].Set(expected[source]);
                assert(moved.Get() == value);
                stored[destination] = std::move(moved);
                expected[destination] = value;
                moved.Set(ConsumeWork(provider));
            },
            [&] {
                if (stored.size() == 32) return;
                stored.push_back(stored[source]);
                expected.push_back(expected[source]);
            },
            [&] {
                if (stored.size() == 1) return;
                stored.erase(stored.begin() + destination);
                expected.erase(expected.begin() + destination);
            },
            [&] {
                const size_t size = provider.ConsumeIntegralInRange<size_t>(1, 32);
                stored.resize(size);
                expected.resize(size);
            },
            [&] {
                stored.reserve(provider.ConsumeIntegralInRange<size_t>(1, 64));
                stored.shrink_to_fit();
            },
            [&] {
                auto copy = stored;
                stored.clear();
                CheckValues(copy, expected);
                stored = std::move(copy);
            },
            [&] {
                std::swap(stored[source], stored[destination]);
                std::swap(expected[source], expected[destination]);
            },
            [&] {
                const auto value = ConsumeWork(provider);
                stored[destination].Set(stored[destination].Get() + value);
                expected[destination] += value;
            });
        CheckValues(stored, expected);
    }
}
