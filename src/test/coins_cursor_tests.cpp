// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for walking every unspent coin of a CCoinsViewCache.
//
// A coins cache holds only the changes made since the last write to the coins
// database. Walking the cache therefore has to combine two sources: the coins
// still sitting in the database, and the changed entries the cache is holding
// in memory. DigiDollar vault accounting walks this set and the answer decides
// whether a block is accepted, so the walk has to visit every unspent coin
// exactly once and nothing else.

#include <coins.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <uint256.h>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(coins_cursor_tests, BasicTestingSetup)

namespace {

bool SameCoin(const Coin& a, const Coin& b)
{
    if (a.IsSpent() && b.IsSpent()) return true;
    return a.fCoinBase == b.fCoinBase && a.nHeight == b.nHeight && a.out == b.out;
}

//! A distinct hash value made from a number, so the test data is easy to follow.
//! The number goes into the first bytes, which are the ones compared first, so
//! bigger numbers also sort later.
uint256 HashFromNumber(uint32_t n)
{
    uint256 out;
    unsigned char* bytes = out.begin();
    bytes[0] = static_cast<unsigned char>(n);
    bytes[1] = static_cast<unsigned char>(n >> 8);
    bytes[2] = static_cast<unsigned char>(n >> 16);
    bytes[3] = static_cast<unsigned char>(n >> 24);
    return out;
}

//! A spendable script. AddCoin silently drops outputs that can never be spent,
//! so an OP_RETURN script would never reach the cache at all.
CScript SpendableScript(uint32_t tag)
{
    std::vector<unsigned char> key_hash(20, 0);
    key_hash[0] = static_cast<unsigned char>(tag);
    key_hash[1] = static_cast<unsigned char>(tag >> 8);
    CScript script;
    script << OP_DUP << OP_HASH160 << key_hash << OP_EQUALVERIFY << OP_CHECKSIG;
    return script;
}

Coin MakeCoin(CAmount value, uint32_t height, uint32_t tag)
{
    return Coin{CTxOut{value, SpendableScript(tag)}, static_cast<int>(height), false};
}

COutPoint MakeOutPoint(FastRandomContext& rng, uint32_t n)
{
    return COutPoint{rng.rand256(), n};
}

//! What one full walk of a cursor produced.
struct Walk {
    std::map<COutPoint, Coin> coins;   //!< every coin handed out, keyed by outpoint
    std::vector<COutPoint> order;      //!< the outpoints in the order they arrived
    size_t handed_out{0};              //!< how many coins were handed out in total
    bool unreadable{false};            //!< the cursor said it had a coin but would not give it
};

Walk WalkCursor(const CCoinsView& view)
{
    Walk walk;
    std::unique_ptr<CCoinsViewCursor> cursor = view.Cursor();
    BOOST_REQUIRE(cursor);
    for (; cursor->Valid(); cursor->Next()) {
        COutPoint key;
        Coin coin;
        // Valid() promising a coin that GetKey() will not produce is the one
        // failure the callers have to notice, so check the two agree.
        if (!cursor->GetKey(key) || !cursor->GetValue(coin)) {
            walk.unreadable = true;
            break;
        }
        ++walk.handed_out;
        walk.order.push_back(key);
        walk.coins.emplace(key, coin);
    }
    cursor->CheckStatus();
    return walk;
}

//! Walk the cursor and check it against the view's own answers for a list of
//! outpoints that covers everything the test has touched.
//! Returns the walk so a test can make further checks on it.
Walk CheckWalkMatchesView(const CCoinsViewCache& view, const std::vector<COutPoint>& universe)
{
    const Walk walk = WalkCursor(view);
    BOOST_CHECK(!walk.unreadable);
    // A repeated outpoint would collapse in the map but still be counted.
    BOOST_CHECK_EQUAL(walk.handed_out, walk.coins.size());

    size_t expected_unspent = 0;
    for (const COutPoint& outpoint : universe) {
        Coin from_view;
        const bool unspent = view.GetCoin(outpoint, from_view);
        const auto found = walk.coins.find(outpoint);
        if (unspent) {
            ++expected_unspent;
            BOOST_CHECK_MESSAGE(found != walk.coins.end(),
                                "cursor missed unspent coin " << outpoint.ToString());
            if (found != walk.coins.end()) {
                BOOST_CHECK_MESSAGE(SameCoin(found->second, from_view),
                                    "cursor gave the wrong coin for " << outpoint.ToString());
            }
        } else {
            BOOST_CHECK_MESSAGE(found == walk.coins.end(),
                                "cursor handed out a coin that is not unspent: " << outpoint.ToString());
        }
    }
    // Nothing outside the list may be handed out either.
    BOOST_CHECK_EQUAL(walk.coins.size(), expected_unspent);
    return walk;
}

//! An in-memory coins database, the same one the other coins tests use.
struct MemoryDb {
    CCoinsViewDB db{{.path = "test", .cache_bytes = 1 << 23, .memory_only = true}, {}};
};

//! Write a cache down to its backing view under the given block hash.
void FlushAt(CCoinsViewCache& cache, const uint256& block)
{
    cache.SetBestBlock(block);
    BOOST_REQUIRE(cache.Flush());
}

} // namespace

// A coin that sits in the database and that the cache has not touched is handed
// out once.
BOOST_AUTO_TEST_CASE(database_coin_is_visited_once)
{
    FastRandomContext rng{HashFromNumber(0x11)};
    MemoryDb backing;
    const COutPoint outpoint = MakeOutPoint(rng, 0);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(outpoint, MakeCoin(1000, 7, 1), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }

    CCoinsViewCache cache{&backing.db};
    const Walk walk = CheckWalkMatchesView(cache, {outpoint});
    BOOST_CHECK_EQUAL(walk.handed_out, 1U);
    BOOST_CHECK(SameCoin(walk.coins.at(outpoint), MakeCoin(1000, 7, 1)));
}

// A coin that sits in the database but has been spent in the cache is not
// handed out at all.
BOOST_AUTO_TEST_CASE(coin_spent_in_cache_is_not_visited)
{
    FastRandomContext rng{HashFromNumber(0x12)};
    MemoryDb backing;
    const COutPoint spent = MakeOutPoint(rng, 0);
    const COutPoint kept = MakeOutPoint(rng, 1);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(spent, MakeCoin(1000, 7, 1), false);
        writer.AddCoin(kept, MakeCoin(2000, 7, 2), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }

    CCoinsViewCache cache{&backing.db};
    BOOST_REQUIRE(cache.SpendCoin(spent));
    const Walk walk = CheckWalkMatchesView(cache, {spent, kept});
    BOOST_CHECK_EQUAL(walk.handed_out, 1U);
    BOOST_CHECK(walk.coins.count(kept) == 1);
    BOOST_CHECK(walk.coins.count(spent) == 0);
}

// Output numbers are stored as variable-length integers. The database visits
// 16512 before 256, so spending 256 must still hide its later database entry.
BOOST_AUTO_TEST_CASE(spent_coin_is_hidden_across_database_key_lengths)
{
    MemoryDb backing;
    const uint256 txid = HashFromNumber(0x24);
    const std::vector<COutPoint> outpoints{{txid, 128}, {txid, 16512}, {txid, 256}};
    CCoinsViewCache cache{&backing.db};
    for (const auto& outpoint : outpoints) {
        cache.AddCoin(outpoint, MakeCoin(1000, 7, outpoint.n), false);
    }
    FlushAt(cache, HashFromNumber(0xaa));
    BOOST_REQUIRE(WalkCursor(backing.db).order == outpoints);

    BOOST_REQUIRE(cache.SpendCoin(outpoints.back()));
    const Walk before = CheckWalkMatchesView(cache, outpoints);
    BOOST_CHECK_EQUAL(before.handed_out, 2U);
    FlushAt(cache, HashFromNumber(0xaa));
    const Walk after = CheckWalkMatchesView(cache, outpoints);
    BOOST_CHECK(before.order == after.order);
}

// Use the real database as the ordering reference, including every encoded
// length that a 32-bit output number can have. Check adds, spends and changes
// through two caches before and after each write to the backing view.
BOOST_AUTO_TEST_CASE(stacked_caches_match_database_at_output_number_boundaries)
{
    const std::vector<uint32_t> numbers{
        0, 1, 126, 127, 128, 129, 255, 256, 257,
        16510, 16511, 16512, 16513, 16639, 16640,
        2113662, 2113663, 2113664, 2113665,
        270549118, 270549119, 270549120, 270549121,
        std::numeric_limits<uint32_t>::max() - 1, std::numeric_limits<uint32_t>::max()};
    std::vector<COutPoint> outpoints;
    for (uint32_t hash : {1, 2, 256}) {
        for (uint32_t n : numbers) outpoints.emplace_back(HashFromNumber(hash), n);
    }
    MemoryDb backing;
    CCoinsViewCache middle{&backing.db};
    for (size_t i = 0; i < outpoints.size(); ++i) {
        middle.AddCoin(outpoints[i], MakeCoin(1000 + i, 7, i), false);
    }
    FlushAt(middle, HashFromNumber(0xaa));
    for (size_t i = 0; i < outpoints.size(); ++i) {
        if (i % 3 == 0) BOOST_REQUIRE(middle.SpendCoin(outpoints[i]));
        if (i % 3 == 1) middle.AddCoin(outpoints[i], MakeCoin(2000 + i, 8, i), true);
    }
    CCoinsViewCache top{&middle};
    for (size_t i = 0; i < outpoints.size(); ++i) {
        if (i % 5 == 0) top.SpendCoin(outpoints[i]);
        if (i % 5 == 1) top.AddCoin(outpoints[i], MakeCoin(3000 + i, 9, i), true);
    }
    const COutPoint added{HashFromNumber(2), 258};
    outpoints.push_back(added);
    top.AddCoin(added, MakeCoin(4000, 9, 1), false);

    const Walk before = CheckWalkMatchesView(top, outpoints);
    top.SetBestBlock(HashFromNumber(0xbb));
    BOOST_REQUIRE(top.Sync());
    BOOST_CHECK(CheckWalkMatchesView(top, outpoints).order == before.order);
    BOOST_REQUIRE(middle.Sync());
    BOOST_CHECK(CheckWalkMatchesView(top, outpoints).order == before.order);
    BOOST_CHECK(WalkCursor(backing.db).order == before.order);
    FlushAt(top, HashFromNumber(0xbb));
    FlushAt(middle, HashFromNumber(0xbb));
    BOOST_CHECK(CheckWalkMatchesView(top, outpoints).order == before.order);
    BOOST_CHECK(WalkCursor(backing.db).order == before.order);
}

// A coin created in the cache and not yet written to the database is handed out
// once, with the cache's copy of it.
BOOST_AUTO_TEST_CASE(coin_created_in_cache_is_visited_once)
{
    FastRandomContext rng{HashFromNumber(0x13)};
    MemoryDb backing;
    const COutPoint on_disk = MakeOutPoint(rng, 0);
    const COutPoint in_cache = MakeOutPoint(rng, 1);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(on_disk, MakeCoin(1000, 7, 1), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }

    CCoinsViewCache cache{&backing.db};
    cache.AddCoin(in_cache, MakeCoin(3000, 9, 3), false);
    const Walk walk = CheckWalkMatchesView(cache, {on_disk, in_cache});
    BOOST_CHECK_EQUAL(walk.handed_out, 2U);
    BOOST_CHECK(SameCoin(walk.coins.at(in_cache), MakeCoin(3000, 9, 3)));
}

// A coin created in the cache and then spent again in the same cache is never
// handed out. The cache drops such an entry entirely, so it exists nowhere.
BOOST_AUTO_TEST_CASE(coin_created_then_spent_in_cache_is_not_visited)
{
    FastRandomContext rng{HashFromNumber(0x14)};
    MemoryDb backing;
    const COutPoint on_disk = MakeOutPoint(rng, 0);
    const COutPoint short_lived = MakeOutPoint(rng, 1);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(on_disk, MakeCoin(1000, 7, 1), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }

    CCoinsViewCache cache{&backing.db};
    cache.AddCoin(short_lived, MakeCoin(3000, 9, 3), false);
    BOOST_REQUIRE(cache.SpendCoin(short_lived));
    const Walk walk = CheckWalkMatchesView(cache, {on_disk, short_lived});
    BOOST_CHECK_EQUAL(walk.handed_out, 1U);
    BOOST_CHECK(walk.coins.count(short_lived) == 0);
}

// A coin the cache has only read is held as an unchanged entry. The walk takes
// unchanged coins from the database, and it must be the very same coin.
BOOST_AUTO_TEST_CASE(unchanged_cache_entry_matches_the_database_coin)
{
    FastRandomContext rng{HashFromNumber(0x15)};
    MemoryDb backing;
    const COutPoint outpoint = MakeOutPoint(rng, 0);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(outpoint, MakeCoin(1234, 11, 5), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }

    CCoinsViewCache cache{&backing.db};
    // Reading the coin loads it into the cache as an unchanged entry.
    const Coin& read_back = cache.AccessCoin(outpoint);
    BOOST_REQUIRE(!read_back.IsSpent());
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 1U);

    const Walk walk = CheckWalkMatchesView(cache, {outpoint});
    BOOST_CHECK_EQUAL(walk.handed_out, 1U);
    // The coin from the database is the coin the cache itself reports.
    BOOST_CHECK(SameCoin(walk.coins.at(outpoint), cache.AccessCoin(outpoint)));

    // Writing the cache down without changing anything, which clears the
    // changed marks but keeps the entries, must not change the answer either.
    BOOST_REQUIRE(cache.Sync());
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 1U);
    const Walk after_sync = CheckWalkMatchesView(cache, {outpoint});
    BOOST_CHECK_EQUAL(after_sync.handed_out, 1U);
}

// The cursor reports the cache's block, not the database's. The database can be
// many blocks behind; the cache's changed entries make up the difference, so
// the coins handed out are the coins at the cache's block.
BOOST_AUTO_TEST_CASE(cursor_reports_the_cache_block_and_the_cache_coins)
{
    FastRandomContext rng{HashFromNumber(0x16)};
    MemoryDb backing;
    const uint256 old_block{0xaa};
    const uint256 new_block{0xbb};
    const COutPoint written = MakeOutPoint(rng, 0);
    const COutPoint spent_later = MakeOutPoint(rng, 1);
    const COutPoint added_later = MakeOutPoint(rng, 2);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(written, MakeCoin(1000, 7, 1), false);
        writer.AddCoin(spent_later, MakeCoin(2000, 7, 2), false);
        FlushAt(writer, old_block);
    }
    BOOST_CHECK(backing.db.GetBestBlock() == old_block);

    CCoinsViewCache cache{&backing.db};
    BOOST_REQUIRE(cache.SpendCoin(spent_later));
    cache.AddCoin(added_later, MakeCoin(3000, 9, 3), false);
    cache.SetBestBlock(new_block);

    std::unique_ptr<CCoinsViewCursor> cursor = cache.Cursor();
    BOOST_REQUIRE(cursor);
    BOOST_CHECK(cursor->GetBestBlock() == new_block);
    BOOST_CHECK(backing.db.GetBestBlock() == old_block);

    const Walk walk = CheckWalkMatchesView(cache, {written, spent_later, added_later});
    BOOST_CHECK_EQUAL(walk.handed_out, 2U);
    BOOST_CHECK(walk.coins.count(written) == 1);
    BOOST_CHECK(walk.coins.count(added_later) == 1);
    BOOST_CHECK(walk.coins.count(spent_later) == 0);
}

// A cache stacked on another cache works the same way: every unspent coin once,
// taking the topmost copy of anything changed more than once.
BOOST_AUTO_TEST_CASE(stacked_caches_hand_out_every_unspent_coin_once)
{
    FastRandomContext rng{HashFromNumber(0x17)};
    MemoryDb backing;
    const COutPoint on_disk = MakeOutPoint(rng, 0);
    const COutPoint spent_in_middle = MakeOutPoint(rng, 1);
    const COutPoint added_in_middle = MakeOutPoint(rng, 2);
    const COutPoint spent_on_top = MakeOutPoint(rng, 3);
    const COutPoint added_on_top = MakeOutPoint(rng, 4);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(on_disk, MakeCoin(1000, 7, 1), false);
        writer.AddCoin(spent_in_middle, MakeCoin(2000, 7, 2), false);
        writer.AddCoin(spent_on_top, MakeCoin(4000, 7, 4), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }

    CCoinsViewCache middle{&backing.db};
    BOOST_REQUIRE(middle.SpendCoin(spent_in_middle));
    middle.AddCoin(added_in_middle, MakeCoin(3000, 9, 3), false);

    CCoinsViewCache top{&middle};
    BOOST_REQUIRE(top.SpendCoin(spent_on_top));
    top.AddCoin(added_on_top, MakeCoin(5000, 9, 5), false);

    const std::vector<COutPoint> universe{on_disk, spent_in_middle, added_in_middle, spent_on_top, added_on_top};
    const Walk walk = CheckWalkMatchesView(top, universe);
    BOOST_CHECK_EQUAL(walk.handed_out, 3U);
    BOOST_CHECK(walk.coins.count(on_disk) == 1);
    BOOST_CHECK(walk.coins.count(added_in_middle) == 1);
    BOOST_CHECK(walk.coins.count(added_on_top) == 1);
}

// A coin that is spent in the middle cache and created again on top is handed
// out once, with the new copy.
BOOST_AUTO_TEST_CASE(coin_respent_higher_up_is_handed_out_once)
{
    FastRandomContext rng{HashFromNumber(0x18)};
    MemoryDb backing;
    const COutPoint outpoint = MakeOutPoint(rng, 0);

    {
        CCoinsViewCache writer{&backing.db};
        writer.AddCoin(outpoint, MakeCoin(1000, 7, 1), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }

    CCoinsViewCache middle{&backing.db};
    BOOST_REQUIRE(middle.SpendCoin(outpoint));

    CCoinsViewCache top{&middle};
    top.AddCoin(outpoint, MakeCoin(6000, 12, 6), false);

    const Walk walk = CheckWalkMatchesView(top, {outpoint});
    BOOST_CHECK_EQUAL(walk.handed_out, 1U);
    BOOST_CHECK(SameCoin(walk.coins.at(outpoint), MakeCoin(6000, 12, 6)));
}

// Nothing in the database and nothing in the cache means nothing to hand out.
BOOST_AUTO_TEST_CASE(empty_view_hands_out_nothing)
{
    MemoryDb backing;
    CCoinsViewCache cache{&backing.db};
    std::unique_ptr<CCoinsViewCursor> cursor = cache.Cursor();
    BOOST_REQUIRE(cursor);
    BOOST_CHECK(!cursor->Valid());
    COutPoint key;
    BOOST_CHECK(!cursor->GetKey(key));
    cursor->CheckStatus();
}

// Every coin lives in the cache and none in the database.
BOOST_AUTO_TEST_CASE(cache_only_coins_are_all_handed_out)
{
    FastRandomContext rng{HashFromNumber(0x19)};
    MemoryDb backing;
    std::vector<COutPoint> universe;
    CCoinsViewCache cache{&backing.db};
    for (uint32_t i = 0; i < 32; ++i) {
        const COutPoint outpoint = MakeOutPoint(rng, i);
        universe.push_back(outpoint);
        cache.AddCoin(outpoint, MakeCoin(100 + i, 5, i), false);
        // Spend every third one again so the cache also holds spent entries.
        if (i % 3 == 2) BOOST_REQUIRE(cache.SpendCoin(outpoint));
    }
    const Walk walk = CheckWalkMatchesView(cache, universe);
    BOOST_CHECK_EQUAL(walk.handed_out, 32U - 32U / 3U);
}

// A view whose backing store cannot be walked at all gives no cursor. Both
// callers in the node check for this before using the result.
BOOST_AUTO_TEST_CASE(no_cursor_when_the_backing_view_has_none)
{
    CCoinsView no_cursor_base;
    CCoinsViewCache cache{&no_cursor_base};
    BOOST_CHECK(cache.Cursor() == nullptr);
}

// Valid() and GetKey() must agree. The walk relies on it: a cursor that says it
// has a coin but will not name it would otherwise be read as a short database.
BOOST_AUTO_TEST_CASE(valid_and_getkey_always_agree)
{
    FastRandomContext rng{HashFromNumber(0x1a)};
    MemoryDb backing;
    {
        CCoinsViewCache writer{&backing.db};
        for (uint32_t i = 0; i < 20; ++i) writer.AddCoin(MakeOutPoint(rng, i), MakeCoin(100 + i, 5, i), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }
    CCoinsViewCache cache{&backing.db};
    for (uint32_t i = 0; i < 20; ++i) cache.AddCoin(MakeOutPoint(rng, i), MakeCoin(200 + i, 6, i), false);

    std::unique_ptr<CCoinsViewCursor> cursor = cache.Cursor();
    BOOST_REQUIRE(cursor);
    size_t seen = 0;
    while (true) {
        COutPoint key;
        const bool valid = cursor->Valid();
        BOOST_CHECK_EQUAL(valid, cursor->GetKey(key));
        if (!valid) break;
        ++seen;
        cursor->Next();
        BOOST_REQUIRE(seen <= 100);  // stop a runaway walk rather than hang
    }
    BOOST_CHECK_EQUAL(seen, 40U);
}

// The set of coins handed out does not depend on how the cache happens to be
// laid out in memory. Two caches reaching the same state by different routes
// hand out the same coins.
BOOST_AUTO_TEST_CASE(same_state_reached_two_ways_hands_out_the_same_coins)
{
    FastRandomContext rng{HashFromNumber(0x1b)};
    std::vector<COutPoint> outpoints;
    for (uint32_t i = 0; i < 40; ++i) outpoints.push_back(MakeOutPoint(rng, i));

    auto build = [&](bool forwards) {
        std::map<COutPoint, Coin> handed_out;
        MemoryDb backing;
        {
            CCoinsViewCache writer{&backing.db};
            for (size_t i = 0; i < outpoints.size(); ++i) {
                const size_t idx = forwards ? i : outpoints.size() - 1 - i;
                if (idx % 2 == 0) writer.AddCoin(outpoints[idx], MakeCoin(100 + idx, 5, idx), false);
            }
            FlushAt(writer, HashFromNumber(0xaa));
        }
        CCoinsViewCache cache{&backing.db};
        for (size_t i = 0; i < outpoints.size(); ++i) {
            const size_t idx = forwards ? i : outpoints.size() - 1 - i;
            if (idx % 2 == 1) cache.AddCoin(outpoints[idx], MakeCoin(100 + idx, 5, idx), false);
            if (idx % 5 == 0) cache.SpendCoin(outpoints[idx]);
        }
        return WalkCursor(cache).coins;
    };

    const std::map<COutPoint, Coin> forwards = build(true);
    const std::map<COutPoint, Coin> backwards = build(false);
    BOOST_CHECK_EQUAL(forwards.size(), backwards.size());
    for (const auto& [outpoint, coin] : forwards) {
        const auto found = backwards.find(outpoint);
        BOOST_REQUIRE(found != backwards.end());
        BOOST_CHECK(SameCoin(coin, found->second));
    }
}

// A long run of random creates, spends and writes to disk, checked against a
// plain map of what should be there. This is the test that would catch a coin
// handed out twice or a coin quietly left out.
BOOST_AUTO_TEST_CASE(random_activity_matches_a_plain_model)
{
    FastRandomContext rng{HashFromNumber(0x20)};
    MemoryDb backing;
    auto cache = std::make_unique<CCoinsViewCache>(&backing.db);
    cache->SetBestBlock(HashFromNumber(1));

    std::vector<COutPoint> universe;
    std::map<COutPoint, Coin> model;   //!< what should be unspent right now
    uint32_t block = 1;

    for (int step = 0; step < 3000; ++step) {
        const uint32_t action = rng.randrange(10);
        if (action < 5 || universe.empty()) {
            // Create a coin. Half the time reuse an outpoint we already know
            // about, so the overwrite path gets exercised as well.
            const bool reuse = !universe.empty() && rng.randbool();
            const COutPoint outpoint = reuse ? universe[rng.randrange(universe.size())]
                                             : MakeOutPoint(rng, rng.randrange(4));
            if (!reuse) universe.push_back(outpoint);
            Coin coin = MakeCoin(1 + rng.randrange(100000), 1 + rng.randrange(1000), rng.randrange(64));
            model[outpoint] = coin;
            cache->AddCoin(outpoint, std::move(coin), /*possible_overwrite=*/true);
        } else if (action < 8) {
            // Spend a coin we know about, whether or not it is still unspent.
            const COutPoint& outpoint = universe[rng.randrange(universe.size())];
            cache->SpendCoin(outpoint);
            model.erase(outpoint);
        } else if (action == 8) {
            // Write everything down to the database, as a flush would.
            cache->SetBestBlock(HashFromNumber(++block));
            BOOST_REQUIRE(cache->Flush());
        } else {
            // Write everything down but keep the entries, as a sync would.
            cache->SetBestBlock(HashFromNumber(++block));
            BOOST_REQUIRE(cache->Sync());
        }

        // Check often enough to pin down which step broke it, but not so often
        // that the test becomes slow.
        if (step % 97 == 0) {
            const Walk walk = WalkCursor(*cache);
            BOOST_REQUIRE(!walk.unreadable);
            BOOST_REQUIRE_EQUAL(walk.handed_out, walk.coins.size());
            BOOST_REQUIRE_EQUAL(walk.coins.size(), model.size());
            for (const auto& [outpoint, coin] : model) {
                const auto found = walk.coins.find(outpoint);
                BOOST_REQUIRE_MESSAGE(found != walk.coins.end(),
                                      "step " << step << ": missed " << outpoint.ToString());
                BOOST_REQUIRE_MESSAGE(SameCoin(coin, found->second),
                                      "step " << step << ": wrong coin for " << outpoint.ToString());
            }
        }
    }

    // Finish with a full check after one more write to disk.
    cache->SetBestBlock(HashFromNumber(++block));
    BOOST_REQUIRE(cache->Flush());
    const Walk walk = WalkCursor(*cache);
    BOOST_CHECK_EQUAL(walk.coins.size(), model.size());
    BOOST_CHECK_EQUAL(walk.handed_out, model.size());
}

// The order coins are handed out in must not depend on how much of the cache
// has already been written to the database. Two nodes at the same block can
// have flushed at different times; if the order differed, anything that reads
// the walk in order, such as the serialized UTXO hash, would disagree between
// them. The order is the backing database's own outpoint order.
BOOST_AUTO_TEST_CASE(order_does_not_depend_on_what_has_been_written_to_disk)
{
    FastRandomContext rng{HashFromNumber(0x21)};
    std::vector<COutPoint> outpoints;
    for (uint32_t i = 0; i < 200; ++i) outpoints.push_back(MakeOutPoint(rng, i % 3));

    // Build the same coin set twice. The first run writes everything to the
    // database and leaves the cache empty. The second leaves half of it sitting
    // in the cache as unwritten changes.
    auto build = [&](bool flush_everything) -> std::vector<COutPoint> {
        MemoryDb backing;
        auto cache = std::make_unique<CCoinsViewCache>(&backing.db);
        for (size_t i = 0; i < outpoints.size(); ++i) {
            cache->AddCoin(outpoints[i], MakeCoin(100 + i, 5, i), false);
            if (!flush_everything && i == outpoints.size() / 2) {
                cache->SetBestBlock(HashFromNumber(0xaa));
                BOOST_REQUIRE(cache->Flush());
            }
        }
        if (flush_everything) {
            cache->SetBestBlock(HashFromNumber(0xaa));
            BOOST_REQUIRE(cache->Flush());
        }
        cache->SetBestBlock(HashFromNumber(0xbb));
        return WalkCursor(*cache).order;
    };

    const std::vector<COutPoint> all_on_disk = build(true);
    const std::vector<COutPoint> half_in_cache = build(false);

    BOOST_CHECK_EQUAL(all_on_disk.size(), outpoints.size());
    BOOST_CHECK_EQUAL(half_in_cache.size(), outpoints.size());
    BOOST_CHECK(all_on_disk == half_in_cache);

    // And that shared order is the database's own outpoint order.
    BOOST_CHECK(std::is_sorted(all_on_disk.begin(), all_on_disk.end()));
    BOOST_CHECK(std::is_sorted(half_in_cache.begin(), half_in_cache.end()));
}

// The same order guarantee under a stack of caches.
BOOST_AUTO_TEST_CASE(stacked_caches_keep_the_database_order)
{
    FastRandomContext rng{HashFromNumber(0x22)};
    MemoryDb backing;
    std::vector<COutPoint> outpoints;
    for (uint32_t i = 0; i < 150; ++i) outpoints.push_back(MakeOutPoint(rng, i % 4));

    {
        CCoinsViewCache writer{&backing.db};
        for (size_t i = 0; i < outpoints.size(); i += 3) writer.AddCoin(outpoints[i], MakeCoin(100 + i, 5, i), false);
        FlushAt(writer, HashFromNumber(0xaa));
    }
    CCoinsViewCache middle{&backing.db};
    for (size_t i = 1; i < outpoints.size(); i += 3) middle.AddCoin(outpoints[i], MakeCoin(100 + i, 5, i), false);
    CCoinsViewCache top{&middle};
    for (size_t i = 2; i < outpoints.size(); i += 3) top.AddCoin(outpoints[i], MakeCoin(100 + i, 5, i), false);

    const Walk walk = WalkCursor(top);
    BOOST_CHECK_EQUAL(walk.order.size(), outpoints.size());
    BOOST_CHECK(std::is_sorted(walk.order.begin(), walk.order.end()));
}

BOOST_AUTO_TEST_SUITE_END()
