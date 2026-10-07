// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <pow.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/signalinterrupt.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>

namespace {
// The full-header calculation is the reference for changing header storage.
arith_uint256 FullHeaderWork(const CBlockIndex& index, std::optional<int> algo = std::nullopt)
{
    const auto header = index.GetBlockHeader();
    const auto& params = Params().GetConsensus();
    if (index.nHeight < params.workComputationChangeTarget) {
        arith_uint256 target;
        bool negative, overflow;
        target.SetCompact(index.nBits, &negative, &overflow);
        if (negative || overflow || target == 0) return 0;
        uint32_t factor{1};
        if (index.nHeight >= params.multiAlgoDiffChangeTarget) {
            switch (header.GetAlgo()) {
            case ALGO_SCRYPT: factor = 4096; break;
            case ALGO_GROESTL: factor = 512; break;
            case ALGO_SKEIN: factor = 24; break;
            case ALGO_QUBIT: factor = 1024; break;
            default: break;
            }
        }
        return ((~target / (target + 1)) + 1) * factor;
    }
    arith_uint256 product{1};
    for (int i = 0; i < NUM_ALGOS_IMPL; ++i) {
        if (algo && *algo != i) continue;
        if (!IsAlgoActive(index.pprev, params, i)) {
            if (algo) return 0;
            continue;
        }
        arith_uint256 target;
        bool negative, overflow;
        target.SetCompact(GetNextWorkRequired(index.pprev, &header, params, i), &negative, &overflow);
        if (negative || overflow || target == 0) return 0;
        if (algo) return (~target / (target + 1)) + 1;
        product *= target.ApproxNthRoot(NUM_ALGOS);
    }
    return ((~product / (product + 1)) + 1) << 7;
}

class TestHeaderSource final : public BlockHeaderSource {
public:
    CBlockHeader header;
    mutable unsigned reads{0};
    bool fail{false};
    BlockHeaderData Read(const uint256& hash) const override
    {
        ++reads;
        if (fail || hash != header.GetHash()) throw std::runtime_error("Header storage read failed");
        return {header.hashMerkleRoot, header.nNonce};
    }
};

class ScopedWorkParams {
    const ChainType m_original{Params().GetChainType()};
public:
    explicit ScopedWorkParams(ChainType chain) { SelectParams(chain); }
    ~ScopedWorkParams() { SelectParams(m_original); }
};

struct WorkHistoryEntry {
    const uint256 hash;
    CBlockIndex reference;
    CBlockIndex stored;

    WorkHistoryEntry(const CBlockHeader& header, int height, WorkHistoryEntry* previous,
                     const BlockHeaderSource& source)
        : hash{header.GetHash()}, reference{header}, stored{header}
    {
        reference.phashBlock = stored.phashBlock = &hash;
        reference.nHeight = stored.nHeight = height;
        reference.pprev = previous ? &previous->reference : nullptr;
        stored.pprev = previous ? &previous->stored : nullptr;
        stored.UseHeaderSource(source);
    }
};

class LinkedWorkHistory {
public:
    TestHeaderSource source;
    std::vector<std::unique_ptr<WorkHistoryEntry>> entries;

    explicit LinkedWorkHistory(int next_height)
    {
        source.fail = true;
        const auto& params = Params().GetConsensus();
        // Keep enough ancestors for the full averaging window and its oldest median.
        const int count = static_cast<int>(NUM_ALGOS * params.nAveragingInterval + CBlockIndex::nMedianTimeSpan + 5);
        const std::array<int, 5> algorithms{ALGO_SCRYPT, ALGO_SHA256D, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT};
        for (int i = 0; i < count; ++i) {
            WorkHistoryEntry* previous = entries.empty() ? nullptr : entries.back().get();
            const int height = next_height - count + i;
            int algo = algorithms[i % algorithms.size()];
            if (algo == ALGO_GROESTL && height > params.algoSwapChangeTarget && height >= params.OdoHeight) {
                algo = ALGO_ODO;
            }
            CBlockHeader header;
            header.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
            header.hashPrevBlock = previous ? previous->hash : uint256{};
            header.hashMerkleRoot = uint256S("123456");
            header.nTime = static_cast<uint32_t>(1500000000 + i * params.nPowTargetSpacing + i % 7);
            header.nBits = 0x1c123456;
            header.nNonce = static_cast<uint32_t>(i);
            entries.push_back(std::make_unique<WorkHistoryEntry>(header, height, previous, source));
        }
    }

    CBlockHeader NextHeader(int version, uint32_t time) const
    {
        CBlockHeader header = entries.back()->reference.GetBlockHeader();
        header.hashPrevBlock = entries.back()->hash;
        header.nVersion = BLOCK_VERSION_DEFAULT | version;
        header.nTime = time;
        ++header.nNonce;
        return header;
    }

    arith_uint256 CheckWork(const CBlockHeader& header)
    {
        auto* previous = entries.back().get();
        BOOST_REQUIRE(header.hashPrevBlock == previous->hash);
        WorkHistoryEntry candidate{header, previous->reference.nHeight + 1, previous, source};
        BOOST_CHECK(candidate.reference.GetBlockHeader().GetHash() == candidate.hash);
        const auto expected = FullHeaderWork(candidate.reference);
        BOOST_CHECK(expected == GetBlockProof(candidate.stored));
        for (int algo = 0; algo < NUM_ALGOS_IMPL; ++algo) {
            BOOST_CHECK(FullHeaderWork(candidate.reference, algo) == GetBlockProof(candidate.stored, algo));
        }
        BOOST_CHECK_EQUAL(source.reads, 0U);
        return expected;
    }
};

std::vector<std::byte> DiskBytes(const CBlockIndex& index)
{
    DataStream stream;
    stream << CDiskBlockIndex{&index};
    return {stream.begin(), stream.end()};
}

bool LoadTestBlockIndex(node::BlockTreeDB& db, std::map<uint256, CBlockIndex>& loaded)
{
    const auto insert = [&](const uint256& hash) -> CBlockIndex* {
        if (hash.IsNull()) return nullptr;
        auto [it, added] = loaded.try_emplace(hash);
        it->second.phashBlock = &it->first;
        return &it->second;
    };
    util::SignalInterrupt interrupt;
    LOCK(cs_main);
    return db.LoadBlockIndexGuts(Params().GetConsensus(), insert, interrupt);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(blockindexstorage_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(header_storage_reduces_resident_index)
{
    BOOST_CHECK_LE(sizeof(CBlockIndex), 112U);
}

BOOST_AUTO_TEST_CASE(corrupt_database_key_is_rejected_before_publishing)
{
    node::BlockTreeDB db{{.path = m_args.GetDataDirBase() / "header-key-test", .cache_bytes = 1 << 20, .memory_only = true}};
    const auto header = Params().GenesisBlock();
    const auto hash = header.GetHash();
    CBlockIndex genesis{header};
    genesis.phashBlock = &hash;
    const uint256 wrong_key{1};
    LOCK(cs_main);
    BOOST_REQUIRE(db.Write(std::make_pair(uint8_t{'b'}, wrong_key), CDiskBlockIndex{&genesis}));
    BOOST_CHECK_THROW(db.ReadBlockHeader(wrong_key), dbwrapper_error);
    BOOST_CHECK_THROW(db.ReadBlockHeader(hash), dbwrapper_error);
    std::map<uint256, CBlockIndex> loaded;
    const auto insert = [&](const uint256& block_hash) -> CBlockIndex* {
        if (block_hash.IsNull()) return nullptr;
        auto [it, added] = loaded.try_emplace(block_hash);
        it->second.phashBlock = &it->first;
        return &it->second;
    };
    util::SignalInterrupt interrupt;
    BOOST_CHECK(!db.LoadBlockIndexGuts(Params().GetConsensus(), insert, interrupt));
    BOOST_CHECK(loaded.empty());
}

BOOST_AUTO_TEST_CASE(block_index_scan_stops_before_file_metadata)
{
    node::BlockTreeDB db{{.path = m_args.GetDataDirBase() / "header-metadata-test", .cache_bytes = 1 << 20, .memory_only = true}};
    const auto header = Params().GenesisBlock();
    const auto hash = header.GetHash();
    CBlockIndex genesis{header};
    genesis.phashBlock = &hash;
    CBlockFileInfo file_info;
    file_info.AddBlock(0, header.nTime);
    file_info.nSize = 128;
    LOCK(cs_main);
    BOOST_REQUIRE(db.WriteBatchSync({{0, &file_info}}, 0, {&genesis}));
    std::map<uint256, CBlockIndex> loaded;
    BOOST_REQUIRE(LoadTestBlockIndex(db, loaded));
    BOOST_REQUIRE_EQUAL(loaded.size(), 1U);
    BOOST_CHECK(loaded.at(hash).GetBlockHeader().GetHash() == hash);
    CBlockFileInfo read_info;
    int last_file{-1};
    BOOST_REQUIRE(db.ReadBlockFileInfo(0, read_info));
    BOOST_REQUIRE(db.ReadLastBlockFile(last_file));
    BOOST_CHECK_EQUAL(read_info.nSize, file_info.nSize);
    BOOST_CHECK_EQUAL(last_file, 0);
}

BOOST_AUTO_TEST_CASE(block_index_scan_rejects_inexact_key_lengths)
{
    const auto header = Params().GenesisBlock();
    const auto hash = header.GetHash();
    CBlockIndex genesis{header};
    genesis.phashBlock = &hash;
    const auto check_key = [&](const auto& key) {
        node::BlockTreeDB db{{.path = m_args.GetDataDirBase() / "header-length-test", .cache_bytes = 1 << 20, .memory_only = true}};
        LOCK(cs_main);
        BOOST_REQUIRE(db.Write(key, CDiskBlockIndex{&genesis}));
        std::map<uint256, CBlockIndex> loaded;
        BOOST_CHECK(!LoadTestBlockIndex(db, loaded));
        BOOST_CHECK(loaded.empty());
    };
    check_key(std::array<uint8_t, 1>{'b'});
    check_key(std::array<uint8_t, 2>{'b', 1});
    std::array<uint8_t, 34> trailing_key{'b'};
    std::copy(hash.begin(), hash.end(), trailing_key.begin() + 1);
    check_key(trailing_key);
}

BOOST_FIXTURE_TEST_CASE(unwritten_headers_trigger_safe_flush, TestChain100Setup)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();
    auto& blockman = chainman.m_blockman;
    LOCK(cs_main);
    chainstate.ForceFlushStateToDisk();
    BOOST_CHECK(!blockman.HeaderCacheNeedsFlush());
    CBlockHeader header = chainstate.m_chain.Tip()->GetBlockHeader();
    header.hashPrevBlock = chainstate.m_chain.Tip()->GetBlockHash();
    ++header.nTime;
    CBlockIndex* best{nullptr};
    for (uint32_t nonce = 0; nonce < 65536; ++nonce) {
        header.nNonce = nonce;
        blockman.AddToBlockIndex(header, best);
    }
    const auto hash = header.GetHash();
    BOOST_CHECK(blockman.HeaderCacheNeedsFlush());
    BOOST_CHECK(blockman.LookupBlockIndex(hash)->GetBlockHeader().GetHash() == hash);
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::NONE));
    BOOST_CHECK(!blockman.HeaderCacheNeedsFlush());
    BOOST_CHECK(blockman.m_block_tree_db->ReadBlockHeader(hash).GetHash() == hash);
    BOOST_CHECK(blockman.LookupBlockIndex(hash)->GetBlockHeader().GetHash() == hash);
    BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), 100);
}

BOOST_FIXTURE_TEST_CASE(ignored_full_block_flushes_its_accepted_header, TestChain100Setup)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();
    auto& blockman = chainman.m_blockman;
    LOCK(cs_main);
    chainstate.ForceFlushStateToDisk();
    BOOST_REQUIRE(!blockman.HeaderCacheNeedsFlush());

    CBlock block;
    BOOST_REQUIRE(blockman.ReadBlockFromDisk(block, *chainstate.m_chain[1]));
    do {
        ++block.nNonce;
    } while (!CheckProofOfWork(GetPoWAlgoHash(block), block.nBits, Params().GetConsensus()));
    const auto hash = block.GetHash();
    BOOST_REQUIRE(blockman.LookupBlockIndex(hash) == nullptr);

    CBlockHeader filler = chainstate.m_chain.Tip()->GetBlockHeader();
    filler.hashPrevBlock = chainstate.m_chain.Tip()->GetBlockHash();
    ++filler.nTime;
    for (uint32_t nonce = 0; nonce < 65535; ++nonce) {
        filler.nNonce = nonce;
        blockman.AddToBlockIndex(filler, chainman.m_best_header);
    }
    BOOST_REQUIRE(!blockman.HeaderCacheNeedsFlush());

    BlockValidationState state;
    CBlockIndex* index{nullptr};
    bool new_block{true};
    BOOST_REQUIRE(chainman.AcceptBlock(std::make_shared<const CBlock>(block), state, &index,
                                      /*fRequested=*/false, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
    BOOST_REQUIRE(index);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(!new_block);
    BOOST_CHECK_EQUAL(index->nHeight, 1);
    BOOST_CHECK(!(index->nStatus & (BLOCK_HAVE_DATA | BLOCK_FAILED_MASK)));
    BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), 100);
    BOOST_REQUIRE(!blockman.HeaderCacheNeedsFlush());
    BOOST_CHECK(blockman.m_block_tree_db->ReadBlockHeader(hash).GetHash() == hash);
}

BOOST_AUTO_TEST_CASE(shared_header_reads_preserve_header_and_disk_bytes)
{
    TestHeaderSource source;
    source.header = Params().GenesisBlock();
    const auto hash = source.header.GetHash();
    CBlockIndex index{source.header};
    index.phashBlock = &hash;
    LOCK(cs_main);
    index.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO;
    index.nFile = 12;
    index.nDataPos = 300;
    index.nUndoPos = 900;
    index.nTx = 4;
    const auto original_bytes = DiskBytes(index);
    index.UseHeaderSource(source);
    BOOST_CHECK(index.GetBlockHeader().GetHash() == hash);
    BOOST_CHECK_EQUAL(source.reads, 1U);
    BOOST_CHECK(DiskBytes(index) == original_bytes);
    BOOST_CHECK_EQUAL(source.reads, 2U);
}

BOOST_AUTO_TEST_CASE(header_read_failure_does_not_return_default_fields)
{
    TestHeaderSource source;
    source.header = Params().GenesisBlock();
    const auto hash = source.header.GetHash();
    CBlockIndex index{source.header};
    index.phashBlock = &hash;
    index.UseHeaderSource(source);
    source.fail = true;
    BOOST_CHECK_THROW(index.GetBlockHeader(), std::runtime_error);
    BOOST_CHECK_THROW(DiskBytes(index), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(disk_copy_owns_its_header)
{
    const auto header = Params().GenesisBlock();
    const auto hash = header.GetHash();
    std::unique_ptr<CDiskBlockIndex> copy;
    {
        TestHeaderSource source;
        source.header = header;
        CBlockIndex index{header};
        index.phashBlock = &hash;
        index.UseHeaderSource(source);
        copy = std::make_unique<CDiskBlockIndex>(&index);
        source.fail = true;
        BOOST_CHECK(copy->ConstructBlockHash() == hash);
    }
    BOOST_CHECK(copy->ConstructBlockHash() == hash);
    DataStream stream;
    stream << *copy;
    CDiskBlockIndex loaded;
    stream >> loaded;
    BOOST_CHECK(loaded.ConstructBlockHash() == hash);
}

BOOST_AUTO_TEST_CASE(block_index_copy_owns_large_chainwork)
{
    struct CopiedBlockIndex : CBlockIndex {
        explicit CopiedBlockIndex(const CBlockIndex& original) : CBlockIndex{original} {}
    };
    const auto header = Params().GenesisBlock();
    const auto hash = header.GetHash();
    const arith_uint256 work = (arith_uint256{1} << 255) | (arith_uint256{1} << 112) | arith_uint256{0x12345678};
    std::unique_ptr<CopiedBlockIndex> copy;
    std::unique_ptr<CDiskBlockIndex> disk_copy;
    {
        TestHeaderSource source;
        source.header = header;
        CBlockIndex index{header};
        index.phashBlock = &hash;
        index.UseHeaderSource(source);
        index.SetChainWork(work);
        copy = std::make_unique<CopiedBlockIndex>(index);
        disk_copy = std::make_unique<CDiskBlockIndex>(&index);
        index.SetChainWork(arith_uint256{1});
        source.fail = true;
    }
    BOOST_CHECK(copy->GetChainWork() == work);
    BOOST_CHECK(disk_copy->GetChainWork() == work);
    BOOST_CHECK(copy->GetBlockHeader().GetHash() == hash);
    BOOST_CHECK(disk_copy->ConstructBlockHash() == hash);
    copy->SetChainWork(arith_uint256{2});
    BOOST_CHECK(disk_copy->GetChainWork() == work);
}

BOOST_AUTO_TEST_CASE(block_work_does_not_read_cold_header_fields)
{
    const auto header = Params().GenesisBlock();
    const auto hash = header.GetHash();
    TestHeaderSource source;
    source.header = header;
    CBlockIndex reference{header};
    CBlockIndex stored{header};
    stored.phashBlock = &hash;
    stored.UseHeaderSource(source);
    source.fail = true;
    const auto& params = Params().GetConsensus();
    for (int64_t height : std::array<int64_t, 6>{0, params.multiAlgoDiffChangeTarget - 1, params.multiAlgoDiffChangeTarget,
                       params.workComputationChangeTarget - 1, params.workComputationChangeTarget,
                       params.workComputationChangeTarget + 1}) {
        reference.nHeight = stored.nHeight = height;
        for (int version : {2, 0x00000200, 0x00000400, 0x00000600, 0x00000800, 0x00000a00, 0x00000e00}) {
            reference.nVersion = stored.nVersion = version;
            for (uint32_t bits : {0U, 0x1d00ffffU, 0x207fffffU, 0x1d80ffffU, 0x23000001U}) {
                reference.nBits = stored.nBits = bits;
                BOOST_CHECK(FullHeaderWork(reference) == GetBlockProof(stored));
                for (int algo = 0; algo < NUM_ALGOS_IMPL; ++algo) {
                    BOOST_CHECK(FullHeaderWork(reference, algo) == GetBlockProof(stored, algo));
                }
            }
        }
    }
    BOOST_CHECK_EQUAL(source.reads, 0U);
}

BOOST_AUTO_TEST_CASE(linked_history_preserves_work_across_geometric_transition)
{
    for (ChainType chain : {ChainType::MAIN, ChainType::TESTNET}) {
        const ScopedWorkParams selected{chain};
        const auto& params = Params().GetConsensus();
        for (int offset : {-1, 0, 1}) {
            LinkedWorkHistory history{static_cast<int>(params.workComputationChangeTarget + offset)};
            const auto& previous = history.entries.back()->reference;
            BOOST_REQUIRE(previous.pprev != nullptr);
            for (int version : std::array<int, 7>{BLOCK_VERSION_SCRYPT, BLOCK_VERSION_SHA256D, BLOCK_VERSION_GROESTL,
                                                 BLOCK_VERSION_SKEIN, BLOCK_VERSION_QUBIT, BLOCK_VERSION_ODO, 0x00000a00}) {
                const auto header = history.NextHeader(version, static_cast<uint32_t>(previous.nTime + params.nPowTargetSpacing));
                BOOST_CHECK(history.CheckWork(header) > 0);
                for (int algo = 0; algo < NUM_ALGOS_IMPL; ++algo) {
                    if (!IsAlgoActive(&previous, params, algo)) continue;
                    // These vectors must reach the ancestor calculation, not its missing-history fallback.
                    BOOST_CHECK(GetNextWorkRequired(&previous, &header, params, algo) != InitialDifficulty(params, algo));
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(linked_history_preserves_minimum_difficulty_time_boundary)
{
    // Signet uses the historical testnet time exception. Public testnet26 disables it.
    for (ChainType chain : {ChainType::SIGNET, ChainType::TESTNET}) {
        const ScopedWorkParams selected{chain};
        const auto& params = Params().GetConsensus();
        BOOST_REQUIRE(!params.fEasyPow);
        BOOST_REQUIRE_EQUAL(params.fPowAllowMinDifficultyBlocks, chain == ChainType::SIGNET);
        LinkedWorkHistory history{static_cast<int>(params.workComputationChangeTarget + 1)};
        const auto& previous = history.entries.back()->reference;
        const uint32_t boundary = static_cast<uint32_t>(previous.nTime + 2 * params.nTargetSpacing);
        const auto minimum_bits = UintToArith256(params.powLimit).GetCompact();
        for (int version : std::array<int, 3>{BLOCK_VERSION_SCRYPT, BLOCK_VERSION_GROESTL, 0x00000a00}) {
            std::array<arith_uint256, 3> work;
            for (int offset : {-1, 0, 1}) {
                const auto header = history.NextHeader(version, boundary + offset);
                work[offset + 1] = history.CheckWork(header);
                const auto bits = GetNextWorkRequired(&previous, &header, params, ALGO_SCRYPT);
                if (params.fPowAllowMinDifficultyBlocks && offset > 0) {
                    BOOST_CHECK_EQUAL(bits, minimum_bits);
                } else {
                    BOOST_CHECK(bits != minimum_bits);
                }
            }
            BOOST_CHECK(work[0] == work[1]);
            if (params.fPowAllowMinDifficultyBlocks) {
                BOOST_CHECK(work[2] < work[1]);
            } else {
                BOOST_CHECK(work[2] == work[1]);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(linked_history_preserves_work_across_odocrypt_transition)
{
    for (ChainType chain : {ChainType::MAIN, ChainType::TESTNET}) {
        const ScopedWorkParams selected{chain};
        const auto& params = Params().GetConsensus();
        // The swap checks the parent height; the buried deployment checks the new height.
        const int first_odo_height = std::max(static_cast<int>(params.algoSwapChangeTarget + 1), params.OdoHeight);
        const int mature_odo_height = first_odo_height + 100;
        for (int height : std::array<int, 7>{static_cast<int>(params.algoSwapChangeTarget - 1),
                                             static_cast<int>(params.algoSwapChangeTarget),
                                             static_cast<int>(params.algoSwapChangeTarget + 1),
                                             first_odo_height - 1, first_odo_height, first_odo_height + 1,
                                             mature_odo_height}) {
            LinkedWorkHistory history{height};
            auto* previous = history.entries.back().get();
            const bool odo_active = height >= first_odo_height;
            BOOST_CHECK_EQUAL(IsAlgoActive(&previous->reference, params, ALGO_ODO), odo_active);
            BOOST_CHECK_EQUAL(IsAlgoActive(&previous->reference, params, ALGO_GROESTL), !odo_active);
            if (height == first_odo_height) {
                BOOST_CHECK(GetLastBlockIndexForAlgo(&previous->reference, params, ALGO_ODO) == nullptr);
            } else if (height == mature_odo_height) {
                BOOST_REQUIRE(GetLastBlockIndexForAlgo(&previous->reference, params, ALGO_ODO) != nullptr);
                BOOST_CHECK(GetLastBlockIndexForAlgo(&previous->reference, params, ALGO_GROESTL) == nullptr);
            }
            for (int version : std::array<int, 7>{BLOCK_VERSION_SCRYPT, BLOCK_VERSION_SHA256D, BLOCK_VERSION_GROESTL,
                                                 BLOCK_VERSION_SKEIN, BLOCK_VERSION_QUBIT, BLOCK_VERSION_ODO, 0x00000a00}) {
                const auto header = history.NextHeader(version, static_cast<uint32_t>(previous->reference.nTime + params.nPowTargetSpacing));
                WorkHistoryEntry candidate{header, height, previous, history.source};
                BOOST_CHECK(candidate.reference.GetBlockHeader().GetHash() == candidate.hash);
                const auto expected = FullHeaderWork(candidate.reference);
                BOOST_CHECK(expected > 0);
                BOOST_CHECK(expected == GetBlockProof(candidate.stored));
                for (int algo = 0; algo < NUM_ALGOS_IMPL; ++algo) {
                    const bool active = algo == ALGO_SHA256D || algo == ALGO_SCRYPT || algo == ALGO_SKEIN || algo == ALGO_QUBIT ||
                                        (algo == ALGO_GROESTL && !odo_active) || (algo == ALGO_ODO && odo_active);
                    const auto work = GetBlockProof(candidate.stored, algo);
                    BOOST_CHECK(work == FullHeaderWork(candidate.reference, algo));
                    BOOST_CHECK_EQUAL(work != 0, active);
                    if (active && height == mature_odo_height) {
                        BOOST_CHECK(GetNextWorkRequired(&previous->reference, &header, params, algo) != InitialDifficulty(params, algo));
                    }
                }
            }
            BOOST_CHECK_EQUAL(history.source.reads, 0U);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
