// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/params.h>
#include <kernel/chainparams.h>
#include <oracle/musig2_aggregator.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <vector>

namespace {

std::array<unsigned char, 32> SerializeXOnly(const secp256k1_xonly_pubkey& key)
{
    std::array<unsigned char, 32> bytes{};
    BOOST_REQUIRE(secp256k1_xonly_pubkey_serialize(secp256k1_context_static, bytes.data(), &key));
    return bytes;
}

// Independently retain the production key selection used before consolidation.
// Passing the same ordered full points to libsecp256k1 must give the same key.
std::array<unsigned char, 32> AggregateRoster(const CChainParams& chain,
                                            const std::vector<uint8_t>& ids)
{
    std::vector<secp256k1_pubkey> keys(ids.size());
    std::vector<const secp256k1_pubkey*> pointers(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto& key = chain.GetOracleNodes().at(ids[i]).pubkey;
        BOOST_REQUIRE(secp256k1_ec_pubkey_parse(secp256k1_context_static,
                                               &keys[i], key.data(), key.size()));
        pointers[i] = &keys[i];
    }
    secp256k1_xonly_pubkey aggregate{};
    secp256k1_musig_keyagg_cache cache{};
    BOOST_REQUIRE(secp256k1_musig_pubkey_agg(secp256k1_context_static, &aggregate,
                                           &cache, pointers.data(), pointers.size()));
    return SerializeXOnly(aggregate);
}

std::vector<unsigned char> Bitmap(const std::vector<uint8_t>& ids, int total)
{
    std::vector<unsigned char> bitmap((total + 7) / 8, 0);
    for (uint8_t id : ids) bitmap.at(id / 8) |= 1U << (id % 8);
    return bitmap;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(oracle_params_aggregation_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(configured_rosters_preserve_production_aggregation)
{
    std::vector<std::unique_ptr<const CChainParams>> chains;
    chains.push_back(CChainParams::Main());
    chains.push_back(CChainParams::TestNet());
    chains.push_back(CChainParams::TestNet(CChainParams::TestNetOptions{true}));
    chains.push_back(CChainParams::RegTest(CChainParams::RegTestOptions{}));

    // Reuse one cache across networks to also cover identical participation sets.
    MuSig2OracleAggregator aggregator;
    for (const auto& chain : chains) {
        const Consensus::Params copy = chain->GetConsensus();
        const auto& nodes = chain->GetOracleNodes();
        BOOST_REQUIRE_EQUAL(copy.vOracleCompressedPublicKeys.size(), nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            BOOST_CHECK(copy.vOracleCompressedPublicKeys[i] ==
                        std::vector<unsigned char>(nodes[i].pubkey.begin(), nodes[i].pubkey.end()));
        }
        BOOST_REQUIRE(chain->ValidateOracleNodeAlignment());

        std::vector<std::vector<uint8_t>> sets;
        std::vector<uint8_t> all(copy.nOraclePubkeyCount);
        std::iota(all.begin(), all.end(), 0);
        sets.push_back(all);
        for (int start = 0; start < copy.nOraclePubkeyCount; ++start) {
            std::vector<uint8_t> ids;
            for (int i = 0; i < copy.nOracleConsensusRequired; ++i) {
                ids.push_back((start + i) % copy.nOraclePubkeyCount);
            }
            std::sort(ids.begin(), ids.end());
            sets.push_back(ids);
        }
        for (unsigned char prefix : {0x02, 0x03}) {
            std::vector<uint8_t> ids;
            for (int i = 0; i < copy.nOraclePubkeyCount; ++i) {
                if (nodes[i].pubkey[0] == prefix) ids.push_back(i);
            }
            if (static_cast<int>(ids.size()) >= copy.nOracleConsensusRequired) sets.push_back(ids);
        }

        for (const auto& ids : sets) {
            const auto expected = AggregateRoster(*chain, ids);
            secp256k1_xonly_pubkey key{};
            secp256k1_musig_keyagg_cache cache{};
            BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, copy, key, cache));
            BOOST_CHECK(SerializeXOnly(key) == expected);
            BOOST_REQUIRE(aggregator.ComputeAggregatePubkeyFromBitmap(
                Bitmap(ids, copy.nOracleTotalOracles), copy, key, cache));
            BOOST_CHECK(SerializeXOnly(key) == expected);

            auto reordered = ids;
            std::reverse(reordered.begin(), reordered.end());
            // Raw ID callers historically deduplicate; duplicates never add votes.
            reordered.push_back(reordered.front());
            aggregator.ClearCache();
            BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(reordered, copy, key, cache));
            BOOST_CHECK(SerializeXOnly(key) == expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(active_overloads_match_explicit_copies)
{
    const auto& params = Params().GetConsensus();
    const Consensus::Params copy = params;
    std::vector<uint8_t> ids(params.nOracleConsensusRequired);
    std::iota(ids.begin(), ids.end(), 0);
    const auto bitmap = Bitmap(ids, params.nOracleTotalOracles);
    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey active{}, copied{};
    secp256k1_musig_keyagg_cache cache{};
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, active, cache));
    aggregator.ClearCache();
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, copy, copied, cache));
    BOOST_CHECK(SerializeXOnly(active) == SerializeXOnly(copied));
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkeyFromBitmap(
        bitmap, params.nOracleTotalOracles, active, cache));
    BOOST_CHECK(SerializeXOnly(active) == SerializeXOnly(copied));
}

BOOST_AUTO_TEST_CASE(cache_separates_genesis_full_key_parity_and_threshold)
{
    Consensus::Params params = CChainParams::RegTest(CChainParams::RegTestOptions{})->GetConsensus();
    const std::vector<uint8_t> ids{0, 1, 2, 3};
    const auto bitmap = Bitmap(ids, params.nOracleTotalOracles);
    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey original{}, changed{};
    secp256k1_musig_keyagg_cache cache{};
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, params, original, cache));
    BOOST_REQUIRE(aggregator.GetCachedAggregatePubkey(bitmap, params, changed));

    auto other_chain = params;
    other_chain.hashGenesisBlock = uint256::ONE;
    BOOST_REQUIRE(other_chain.hashGenesisBlock != params.hashGenesisBlock);
    BOOST_CHECK(!aggregator.GetCachedAggregatePubkey(bitmap, other_chain, changed));
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, other_chain, changed, cache));
    BOOST_CHECK(SerializeXOnly(changed) == SerializeXOnly(original));

    auto other_roster = params;
    other_roster.vOracleCompressedPublicKeys[0][0] ^= 1;
    BOOST_CHECK(!aggregator.GetCachedAggregatePubkey(bitmap, other_roster, changed));
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, other_roster, changed, cache));
    BOOST_CHECK(SerializeXOnly(changed) != SerializeXOnly(original));
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, params, changed, cache));
    BOOST_CHECK(SerializeXOnly(changed) == SerializeXOnly(original));

    auto higher_threshold = params;
    higher_threshold.nOracleConsensusRequired = 5;
    BOOST_CHECK(!aggregator.GetCachedAggregatePubkey(bitmap, higher_threshold, changed));
    BOOST_CHECK(!aggregator.ComputeAggregatePubkey(ids, higher_threshold, changed, cache));
    BOOST_CHECK(!aggregator.ComputeAggregatePubkey({0, 1, 2, 2, 2}, params, changed, cache));
}

BOOST_AUTO_TEST_CASE(supplied_roster_requires_full_matching_keys_and_valid_bounds)
{
    const Consensus::Params base = CChainParams::RegTest(CChainParams::RegTestOptions{})->GetConsensus();
    const std::vector<uint8_t> ids{0, 1, 2, 3};
    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey key{};
    secp256k1_musig_keyagg_cache cache{};
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, base, key, cache));

    auto params = base;
    params.vOracleCompressedPublicKeys.clear();
    BOOST_CHECK(!aggregator.ComputeAggregatePubkey(ids, params, key, cache));
    params = base;
    params.vOracleCompressedPublicKeys[0].erase(params.vOracleCompressedPublicKeys[0].begin());
    BOOST_CHECK(!aggregator.ComputeAggregatePubkey(ids, params, key, cache));
    params = base;
    params.vOracleCompressedPublicKeys[0][0] = 0x04;
    BOOST_CHECK(!aggregator.ComputeAggregatePubkey(ids, params, key, cache));
    params = base;
    params.vOraclePublicKeys[0] = params.vOraclePublicKeys[1];
    BOOST_CHECK(!aggregator.ComputeAggregatePubkey(ids, params, key, cache));
    BOOST_CHECK(!aggregator.ComputeAggregatePubkey({0, 1, 2, 7}, base, key, cache));
    for (int invalid : {-1, 0, 257, 65543, std::numeric_limits<int>::max()}) {
        params = base;
        params.nOracleTotalOracles = invalid;
        BOOST_CHECK(!aggregator.ComputeAggregatePubkeyFromBitmap({0x0f}, params, key, cache));
    }
    BOOST_CHECK(!aggregator.ComputeAggregatePubkeyFromBitmap({0x8f}, base, key, cache));
    BOOST_CHECK(!aggregator.ComputeAggregatePubkeyFromBitmap({0x0f, 0x00}, base, key, cache));
}

BOOST_AUTO_TEST_CASE(full_byte_oracle_id_is_not_narrowed)
{
    auto params = CChainParams::RegTest(CChainParams::RegTestOptions{})->GetConsensus();
    // Aggregate only unique existing keys, with the final key moved to slot 255.
    const auto last_key = params.vOracleCompressedPublicKeys.back();
    const auto last_xonly = params.vOraclePublicKeys.back();
    params.nOracleTotalOracles = 256;
    params.nOraclePubkeyCount = 256;
    params.vOracleCompressedPublicKeys.resize(256);
    params.vOraclePublicKeys.resize(256);
    params.vOracleCompressedPublicKeys[255] = last_key;
    params.vOraclePublicKeys[255] = last_xonly;
    const std::vector<uint8_t> ids{0, 1, 2, 255};
    MuSig2OracleAggregator aggregator;
    secp256k1_xonly_pubkey direct{}, bitmap_key{};
    secp256k1_musig_keyagg_cache cache{};
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkey(ids, params, direct, cache));
    BOOST_REQUIRE(aggregator.ComputeAggregatePubkeyFromBitmap(Bitmap(ids, 256), params, bitmap_key, cache));
    BOOST_CHECK(SerializeXOnly(direct) == SerializeXOnly(bitmap_key));
}

BOOST_AUTO_TEST_SUITE_END()
