// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockheadercache.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <new>
#include <stdexcept>

namespace {

CBlockHeader MakeHeader(uint32_t nonce)
{
    CBlockHeader header;
    header.nVersion = 4 | BLOCK_VERSION_SHA256D;
    header.hashPrevBlock = uint256S("123456");
    header.hashMerkleRoot = uint256S("abcdef");
    header.nTime = 1780000000;
    header.nBits = 0x1d00ffff;
    header.nNonce = nonce;
    return header;
}

void CheckFound(const node::BlockHeaderReadResult& result, const CBlockHeader& expected)
{
    BOOST_REQUIRE(result.status == node::BlockHeaderReadStatus::FOUND);
    BOOST_REQUIRE(result.header.has_value());
    BOOST_CHECK(result.error.empty());
    BOOST_CHECK_EQUAL(result.header->nVersion, expected.nVersion);
    BOOST_CHECK(result.header->hashPrevBlock == expected.hashPrevBlock);
    BOOST_CHECK(result.header->hashMerkleRoot == expected.hashMerkleRoot);
    BOOST_CHECK_EQUAL(result.header->nTime, expected.nTime);
    BOOST_CHECK_EQUAL(result.header->nBits, expected.nBits);
    BOOST_CHECK_EQUAL(result.header->nNonce, expected.nNonce);
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockheadercache_tests)

BOOST_AUTO_TEST_CASE(cache_returns_owned_headers)
{
    const auto header = MakeHeader(1);
    size_t reads{0};
    node::BlockHeaderCache cache{[&](const uint256& hash) {
        BOOST_CHECK(hash == header.GetHash());
        ++reads;
        return node::BlockHeaderReadResult::Found(header);
    }, 1};

    auto first = cache.Read(header.GetHash());
    CheckFound(first, header);
    BOOST_REQUIRE(first.header.has_value());
    first.header->nNonce++;
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(reads, 1U);
    BOOST_CHECK_EQUAL(cache.Size(), 1U);
    cache.Clear();
    BOOST_CHECK_EQUAL(first.header->nNonce, header.nNonce + 1);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(reads, 2U);
}

BOOST_AUTO_TEST_CASE(zero_capacity_reads_through)
{
    const auto header = MakeHeader(1);
    size_t reads{0};
    node::BlockHeaderCache cache{[&](const uint256&) {
        ++reads;
        return node::BlockHeaderReadResult::Found(header);
    }, 0};

    CheckFound(cache.Read(header.GetHash()), header);
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(cache.Capacity(), 0U);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    BOOST_CHECK_EQUAL(reads, 2U);
}

BOOST_AUTO_TEST_CASE(eviction_preserves_recently_used_headers)
{
    const auto first = MakeHeader(1);
    const auto second = MakeHeader(2);
    const auto third = MakeHeader(3);
    const std::map<uint256, CBlockHeader> headers{
        {first.GetHash(), first}, {second.GetHash(), second}, {third.GetHash(), third}};
    std::map<uint256, size_t> reads;
    node::BlockHeaderCache cache{[&](const uint256& hash) {
        ++reads[hash];
        return node::BlockHeaderReadResult::Found(headers.at(hash));
    }, 2};

    CheckFound(cache.Read(first.GetHash()), first);
    CheckFound(cache.Read(second.GetHash()), second);
    CheckFound(cache.Read(first.GetHash()), first);
    CheckFound(cache.Read(third.GetHash()), third);
    CheckFound(cache.Read(first.GetHash()), first);
    CheckFound(cache.Read(second.GetHash()), second);
    BOOST_CHECK_EQUAL(reads[first.GetHash()], 1U);
    BOOST_CHECK_EQUAL(reads[second.GetHash()], 2U);
    BOOST_CHECK_EQUAL(reads[third.GetHash()], 1U);
    BOOST_CHECK_EQUAL(cache.Size(), 2U);
}

BOOST_AUTO_TEST_CASE(capacity_stays_bounded_under_repeated_misses)
{
    CBlockHeader current;
    node::BlockHeaderCache cache{[&](const uint256&) {
        return node::BlockHeaderReadResult::Found(current);
    }, 3};

    for (uint32_t nonce = 0; nonce < 128; ++nonce) {
        current = MakeHeader(nonce);
        CheckFound(cache.Read(current.GetHash()), current);
        BOOST_CHECK_LE(cache.Size(), cache.Capacity());
    }
    BOOST_CHECK_EQUAL(cache.Size(), 3U);
}

BOOST_AUTO_TEST_CASE(missing_and_failed_reads_are_retried)
{
    const auto header = MakeHeader(1);
    size_t reads{0};
    node::BlockHeaderCache cache{[&](const uint256&) {
        ++reads;
        if (reads == 1) return node::BlockHeaderReadResult::NotFound();
        if (reads == 2) return node::BlockHeaderReadResult::Error("Database read failed");
        return node::BlockHeaderReadResult::Found(header);
    }, 1};

    const auto missing = cache.Read(header.GetHash());
    BOOST_CHECK(missing.status == node::BlockHeaderReadStatus::NOT_FOUND);
    BOOST_CHECK(!missing.header.has_value());
    BOOST_CHECK(missing.error.empty());
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    const auto failed = cache.Read(header.GetHash());
    BOOST_CHECK(failed.status == node::BlockHeaderReadStatus::ERROR);
    BOOST_CHECK(!failed.header.has_value());
    BOOST_CHECK_EQUAL(failed.error, "Database read failed");
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    CheckFound(cache.Read(header.GetHash()), header);
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(reads, 3U);
}

BOOST_AUTO_TEST_CASE(wrong_hash_is_an_error_and_is_not_cached)
{
    const auto header = MakeHeader(1);
    size_t reads{0};
    node::BlockHeaderCache cache{[&](const uint256&) {
        return node::BlockHeaderReadResult::Found(++reads == 1 ? MakeHeader(2) : header);
    }, 1};

    const auto wrong = cache.Read(header.GetHash());
    BOOST_CHECK(wrong.status == node::BlockHeaderReadStatus::ERROR);
    BOOST_CHECK(!wrong.header.has_value());
    BOOST_CHECK(!wrong.error.empty());
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(reads, 2U);
}

BOOST_AUTO_TEST_CASE(success_without_a_header_is_an_error)
{
    node::BlockHeaderCache cache{[](const uint256&) {
        node::BlockHeaderReadResult result;
        result.status = node::BlockHeaderReadStatus::FOUND;
        return result;
    }, 1};

    const auto result = cache.Read(MakeHeader(1).GetHash());
    BOOST_CHECK(result.status == node::BlockHeaderReadStatus::ERROR);
    BOOST_CHECK(!result.header.has_value());
    BOOST_CHECK(!result.error.empty());
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(reader_exception_is_an_error_and_can_be_retried)
{
    const auto header = MakeHeader(1);
    size_t reads{0};
    node::BlockHeaderCache cache{[&](const uint256&) {
        if (++reads == 1) throw std::runtime_error("Database read failed");
        return node::BlockHeaderReadResult::Found(header);
    }, 1};

    const auto result = cache.Read(header.GetHash());
    BOOST_CHECK(result.status == node::BlockHeaderReadStatus::ERROR);
    BOOST_CHECK(!result.header.has_value());
    BOOST_CHECK(result.error.find("Database read failed") != std::string::npos);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(reads, 2U);
}

BOOST_AUTO_TEST_CASE(allocation_failure_is_not_a_missing_record)
{
    node::BlockHeaderCache cache{[](const uint256&) -> node::BlockHeaderReadResult {
        throw std::bad_alloc{};
    }, 1};

    BOOST_CHECK_THROW(cache.Read(MakeHeader(1).GetHash()), std::bad_alloc);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(clear_during_read_does_not_repopulate_cache)
{
    const auto header = MakeHeader(1);
    std::atomic<size_t> reads{0};
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    node::BlockHeaderCache cache{[&](const uint256&) {
        if (++reads == 1) {
            entered.set_value();
            release_future.wait();
        }
        return node::BlockHeaderReadResult::Found(header);
    }, 1};

    auto pending = std::async(std::launch::async, [&] { return cache.Read(header.GetHash()); });
    const auto entered_status = entered_future.wait_for(std::chrono::seconds{5});
    auto cleared = std::async(std::launch::async, [&] { cache.Clear(); });
    const auto cleared_status = cleared.wait_for(std::chrono::seconds{5});
    release.set_value();
    const auto result = pending.get();
    cleared.get();

    BOOST_REQUIRE(entered_status == std::future_status::ready);
    BOOST_CHECK(cleared_status == std::future_status::ready);
    CheckFound(result, header);
    BOOST_CHECK_EQUAL(cache.Size(), 0U);
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(reads.load(), 2U);
}

BOOST_AUTO_TEST_CASE(concurrent_misses_share_one_cache_entry)
{
    const auto header = MakeHeader(1);
    std::atomic<size_t> reads{0};
    std::promise<void> both_entered;
    auto entered_future = both_entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    node::BlockHeaderCache cache{[&](const uint256&) {
        if (++reads == 2) both_entered.set_value();
        release_future.wait();
        return node::BlockHeaderReadResult::Found(header);
    }, 1};

    auto first = std::async(std::launch::async, [&] { return cache.Read(header.GetHash()); });
    auto second = std::async(std::launch::async, [&] { return cache.Read(header.GetHash()); });
    const auto entered_status = entered_future.wait_for(std::chrono::seconds{5});
    release.set_value();
    const auto first_result = first.get();
    const auto second_result = second.get();

    BOOST_REQUIRE(entered_status == std::future_status::ready);
    CheckFound(first_result, header);
    CheckFound(second_result, header);
    BOOST_CHECK_EQUAL(cache.Size(), 1U);
    CheckFound(cache.Read(header.GetHash()), header);
    BOOST_CHECK_EQUAL(reads.load(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()
