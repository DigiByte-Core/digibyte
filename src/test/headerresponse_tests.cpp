// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <blockencodings.h>
#include <chainparams.h>
#include <common/args.h>
#include <core_io.h>
#include <dbwrapper.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <rest.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <streams.h>
#include <test/util/logging.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <timedata.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
class ScopedMessageCapture {
    decltype(CaptureMessage) m_original{CaptureMessage};
    std::optional<common::SettingsValue> m_original_setting;
public:
    explicit ScopedMessageCapture(decltype(CaptureMessage) capture)
    {
        CaptureMessage = std::move(capture);
        gArgs.LockSettings([&](auto& settings) {
            const auto it = settings.forced_settings.find("capturemessages");
            if (it != settings.forced_settings.end()) m_original_setting = it->second;
            settings.forced_settings["capturemessages"] = std::string{"1"};
        });
    }
    ~ScopedMessageCapture()
    {
        CaptureMessage = std::move(m_original);
        gArgs.LockSettings([&](auto& settings) {
            if (m_original_setting) {
                settings.forced_settings["capturemessages"] = std::move(*m_original_setting);
            } else {
                settings.forced_settings.erase("capturemessages");
            }
        });
    }
};

class HiddenBlocksDirectory {
    const fs::path m_original;
    const fs::path m_hidden;
    node::KernelNotifications& m_notifications;
    const bool m_shutdown_on_error;
public:
    HiddenBlocksDirectory(const fs::path& path, node::KernelNotifications& notifications)
        : m_original{path}, m_hidden{path.parent_path() / "blocks-temporarily-hidden"},
          m_notifications{notifications}, m_shutdown_on_error{notifications.m_shutdown_on_fatal_error}
    {
        fs::rename(m_original, m_hidden);
        m_notifications.m_shutdown_on_fatal_error = false;
    }
    ~HiddenBlocksDirectory()
    {
        m_notifications.m_shutdown_on_fatal_error = m_shutdown_on_error;
        std::error_code error;
        fs::rename(m_hidden, m_original, error);
        BOOST_CHECK_MESSAGE(!error, "Could not restore the fixture blocks directory: " << error.message());
    }
};

class ResponseHeaderSource final : public BlockHeaderSource {
public:
    std::map<uint256, CBlockHeader> headers;
    std::optional<uint256> fail_hash;
    bool other_error{false};
    mutable std::function<void()> before_read;
    mutable size_t reads{0};

    BlockHeaderData Read(const uint256& hash) const override
    {
        ++reads;
        if (auto action = std::exchange(before_read, {})) action();
        if (other_error) throw std::logic_error("Unexpected header error");
        if (fail_hash == hash) throw dbwrapper_error("Injected header storage failure");
        const auto& header = headers.at(hash);
        return {header.hashMerkleRoot, header.nNonce};
    }
};

struct HeaderResponseSetup : RegTestingSetup {
    ResponseHeaderSource source;
    std::vector<CBlockIndex*> indices;
    CBlockIndex* original_tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    CBlockIndex* original_best{m_node.chainman->m_best_header};
    uint32_t next_nonce{1};
    std::atomic<bool> interrupt{false};
    CNode peer{990, nullptr, CAddress{CService{}, NODE_NETWORK}, 0, 0, CAddress{}, "",
               ConnectionType::OUTBOUND_FULL_RELAY, false};
    std::unique_ptr<ScopedMessageCapture> capture;
    std::vector<std::vector<uint256>> sent_headers;

    HeaderResponseSetup()
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        static_cast<TestChainstateManager&>(*m_node.chainman).JumpOutOfIbd();
        auto& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
        connman.Handshake(peer, true, ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                          ServiceFlags(NODE_NETWORK | NODE_WITNESS), PROTOCOL_VERSION, true);
        TestOnlyResetTimeData();
        RegisterValidationInterface(m_node.peerman.get());
        capture = std::make_unique<ScopedMessageCapture>([this](const CAddress&, const std::string& type, Span<const unsigned char> bytes, bool incoming) {
            if (incoming || type != NetMsgType::HEADERS) return;
            CDataStream stream{bytes, SER_NETWORK, PROTOCOL_VERSION};
            std::vector<CBlock> headers;
            stream >> headers;
            std::vector<uint256> hashes;
            for (const auto& header : headers) {
                BOOST_CHECK(header.vtx.empty());
                hashes.push_back(header.GetHash());
            }
            BOOST_CHECK(stream.empty());
            sent_headers.push_back(std::move(hashes));
        });
        Message(NetMsgType::SENDHEADERS);
    }

    ~HeaderResponseSetup()
    {
        UnregisterValidationInterface(m_node.peerman.get());
        SyncWithValidationInterfaceQueue();
        m_node.peerman->FinalizeNode(peer);
        capture.reset();
        LOCK(cs_main);
        m_node.chainman->ActiveChain().SetTip(*original_tip);
        m_node.chainman->m_best_header = original_best;
        // The source belongs to this fixture, so detach it before base teardown.
        for (auto* index : indices) {
            const auto& header = source.headers.at(index->GetBlockHash());
            index->SetHeaderData({header.hashMerkleRoot, header.nNonce});
        }
    }

    template <typename... Args>
    void Message(const std::string& type, const Args&... args)
    {
        const auto message = CNetMsgMaker{PROTOCOL_VERSION}.Make(type, args...);
        CDataStream stream{message.data, SER_NETWORK, PROTOCOL_VERSION};
        m_node.peerman->ProcessMessage(peer, type, stream, std::chrono::microseconds{0}, interrupt);
    }

    CBlockIndex* AddHeader(CBlockIndex* parent)
    {
        LOCK(cs_main);
        CBlockHeader header;
        header.nVersion = 2;
        header.hashPrevBlock = parent->GetBlockHash();
        header.nTime = parent->nTime + 1;
        header.nBits = parent->nBits;
        header.nNonce = next_nonce++;
        header.hashMerkleRoot = uint256{static_cast<uint8_t>(header.nNonce)};
        const auto hash = header.GetHash();
        source.headers.emplace(hash, header);
        auto* index = m_node.chainman->m_blockman.InsertBlockIndex(hash);
        index->pprev = parent;
        index->nHeight = parent->nHeight + 1;
        index->nVersion = header.nVersion;
        index->nTime = header.nTime;
        index->nTimeMax = header.nTime;
        index->nBits = header.nBits;
        index->SetChainWork(parent->GetChainWork() + arith_uint256{1});
        index->nStatus = BLOCK_VALID_SCRIPTS;
        index->BuildSkip();
        index->UseHeaderSource(source);
        indices.push_back(index);
        return index;
    }

    void SetTip(CBlockIndex* tip)
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChain().SetTip(*tip);
        m_node.chainman->m_best_header = tip;
    }

    void GetHeaders(const CBlockIndex* from, const uint256& stop = {})
    {
        Message(NetMsgType::GETHEADERS, from ? GetLocator(from) : CBlockLocator{}, stop);
    }

    void Announce(CBlockIndex* tip, CBlockIndex* fork)
    {
        GetMainSignals().UpdatedBlockTip(tip, fork, false);
        SyncWithValidationInterfaceQueue();
        m_node.peerman->SendMessages(&peer);
    }

    void CheckIncomingHeaderFlushFailure(bool compact)
    {
        auto& chainman = *m_node.chainman;
        auto& blockman = chainman.m_blockman;
        CBlockHeader header = Params().GenesisBlock();
        header.hashPrevBlock = original_tip->GetBlockHash();
        header.nVersion = 4 | BLOCK_VERSION_SCRYPT;
        header.nTime = original_tip->nTime + 1;
        header.nBits = GetNextWorkRequired(original_tip, &header, Params().GetConsensus(), ALGO_SCRYPT);
        BOOST_REQUIRE(IsAlgoActive(original_tip, Params().GetConsensus(), header.GetAlgo()));
        {
            LOCK(cs_main);
            chainman.ActiveChainstate().ForceFlushStateToDisk();
            BOOST_REQUIRE(!blockman.HeaderCacheNeedsFlush());
            // Seed the real owner without spending time mining unrelated headers.
            // Each entry is a separate child of the already validated genesis.
            for (header.nNonce = 0; header.nNonce < 65535; ++header.nNonce) {
                blockman.AddToBlockIndex(header, chainman.m_best_header);
            }
            BOOST_REQUIRE(!blockman.HeaderCacheNeedsFlush());
        }
        uint32_t attempts{0};
        while (!CheckProofOfWork(header.GetPoWAlgoHash(Params().GetConsensus()), header.nBits, Params().GetConsensus()) && attempts++ < 10000) {
            ++header.nNonce;
        }
        BOOST_REQUIRE(CheckProofOfWork(header.GetPoWAlgoHash(Params().GetConsensus()), header.nBits, Params().GetConsensus()));

        const auto blocks_dir = m_args.GetBlocksDirPath();
        // Never hide a directory supplied from outside this fixture.
        BOOST_REQUIRE(blocks_dir == m_path_root / "regtest" / "blocks");
        BOOST_REQUIRE(fs::exists(blocks_dir));
        {
            HiddenBlocksDirectory hidden{blocks_dir, *m_node.notifications};
            {
                ASSERT_DEBUG_LOG("System error while flushing:");
                if (compact) {
                    CBlock block{header};
                    block.vtx.push_back(Params().GenesisBlock().vtx.front());
                    BOOST_CHECK_NO_THROW(Message(NetMsgType::CMPCTBLOCK, CBlockHeaderAndShortTxIDs{block}));
                } else {
                    BOOST_CHECK_NO_THROW(Message(NetMsgType::HEADERS, std::vector<CBlock>{CBlock{header}}));
                }
            }
            BOOST_CHECK(!peer.fDisconnect);
            BOOST_CHECK(sent_headers.empty());
            BOOST_CHECK(blockman.HeaderCacheNeedsFlush());
            {
                LOCK(cs_main);
                const auto* accepted = blockman.LookupBlockIndex(header.GetHash());
                BOOST_REQUIRE(accepted);
                BOOST_CHECK_EQUAL(accepted->nStatus & BLOCK_FAILED_MASK, 0U);
            }

            // The same real storage failure reports an error, not invalidity,
            // and intentionally leaves the caller's output index unset.
            BlockValidationState state;
            const CBlockIndex* index{nullptr};
            {
                ASSERT_DEBUG_LOG("System error while flushing:");
                BOOST_CHECK(!chainman.ProcessNewBlockHeaders({header}, true, state, &index));
            }
            BOOST_CHECK(state.IsError());
            BOOST_CHECK(!state.IsInvalid());
            BOOST_CHECK(index == nullptr);
        }
        BOOST_REQUIRE(fs::exists(blocks_dir));
        LOCK(cs_main);
        BlockValidationState recovered;
        BOOST_CHECK(chainman.ActiveChainstate().FlushStateToDisk(recovered, FlushStateMode::NONE));
        BOOST_CHECK(!blockman.HeaderCacheNeedsFlush());
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(headerresponse_tests, HeaderResponseSetup)

BOOST_AUTO_TEST_CASE(rest_headers_preserve_each_format)
{
    auto* a = AddHeader(original_tip);
    auto* b = AddHeader(a);
    const std::array<const CBlockIndex*, 2> headers{a, b};
    DataStream bytes;
    bytes << source.headers.at(a->GetBlockHash()) << source.headers.at(b->GetBlockHash());
    const auto binary = BuildRESTHeadersResponse(headers, b, RESTResponseFormat::BINARY);
    BOOST_CHECK_EQUAL(binary.status, HTTP_OK);
    BOOST_CHECK_EQUAL(binary.content_type, "application/octet-stream");
    BOOST_CHECK_EQUAL(binary.body, bytes.str());
    const auto hex = BuildRESTHeadersResponse(headers, b, RESTResponseFormat::HEX);
    BOOST_CHECK_EQUAL(hex.status, HTTP_OK);
    BOOST_CHECK_EQUAL(hex.body, HexStr(bytes) + "\n");
    UniValue expected{UniValue::VARR};
    expected.push_back(blockheaderToJSON(b, a));
    expected.push_back(blockheaderToJSON(b, b));
    const auto json = BuildRESTHeadersResponse(headers, b, RESTResponseFormat::JSON);
    BOOST_CHECK_EQUAL(json.status, HTTP_OK);
    BOOST_CHECK_EQUAL(json.body, expected.write() + "\n");
}

BOOST_AUTO_TEST_CASE(rest_storage_errors_replace_the_whole_response)
{
    auto* a = AddHeader(original_tip);
    auto* b = AddHeader(a);
    const std::array<const CBlockIndex*, 2> headers{a, b};
    source.fail_hash = b->GetBlockHash();
    for (auto format : {RESTResponseFormat::BINARY, RESTResponseFormat::HEX, RESTResponseFormat::JSON}) {
        const auto response = BuildRESTHeadersResponse(headers, b, format);
        BOOST_CHECK_EQUAL(response.status, HTTP_INTERNAL_SERVER_ERROR);
        BOOST_CHECK_EQUAL(response.content_type, "text/plain");
        BOOST_CHECK_EQUAL(response.body, "Error reading block header from local storage\r\n");
    }
    const CBlock block{source.headers.at(b->GetBlockHash())};
    const auto response = BuildRESTBlockResponse(m_node.chainman->m_blockman, block, b, b,
                                                 RESTResponseFormat::JSON, TxVerbosity::SHOW_TXID);
    BOOST_CHECK_EQUAL(response.status, HTTP_INTERNAL_SERVER_ERROR);
    BOOST_CHECK_EQUAL(response.body, "Error reading block header from local storage\r\n");
    // Binary and hex blocks already own their header and need no index read.
    for (auto format : {RESTResponseFormat::BINARY, RESTResponseFormat::HEX}) {
        const auto complete = BuildRESTBlockResponse(m_node.chainman->m_blockman, block, b, b, format, TxVerbosity::SHOW_TXID);
        BOOST_CHECK_EQUAL(complete.status, HTTP_OK);
        BOOST_CHECK(!complete.body.empty());
    }
}

BOOST_AUTO_TEST_CASE(rest_block_preserves_each_format)
{
    auto* a = AddHeader(original_tip);
    const CBlock block{source.headers.at(a->GetBlockHash())};
    CDataStream bytes{SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags()};
    bytes << block;
    const auto binary = BuildRESTBlockResponse(m_node.chainman->m_blockman, block, a, a,
                                               RESTResponseFormat::BINARY, TxVerbosity::SHOW_TXID);
    BOOST_CHECK_EQUAL(binary.status, HTTP_OK);
    BOOST_CHECK_EQUAL(binary.body, bytes.str());
    const auto hex = BuildRESTBlockResponse(m_node.chainman->m_blockman, block, a, a,
                                            RESTResponseFormat::HEX, TxVerbosity::SHOW_TXID);
    BOOST_CHECK_EQUAL(hex.status, HTTP_OK);
    BOOST_CHECK_EQUAL(hex.body, HexStr(bytes) + "\n");
    const auto json = BuildRESTBlockResponse(m_node.chainman->m_blockman, block, a, a,
                                             RESTResponseFormat::JSON, TxVerbosity::SHOW_TXID);
    BOOST_CHECK_EQUAL(json.status, HTTP_OK);
    BOOST_CHECK_EQUAL(json.body, blockToJSON(m_node.chainman->m_blockman, block, a, a, TxVerbosity::SHOW_TXID).write() + "\n");
}

BOOST_AUTO_TEST_CASE(rest_only_catches_storage_errors)
{
    auto* a = AddHeader(original_tip);
    const std::array<const CBlockIndex*, 1> headers{a};
    source.other_error = true;
    BOOST_CHECK_THROW(BuildRESTHeadersResponse(headers, a, RESTResponseFormat::BINARY), std::logic_error);
}

BOOST_AUTO_TEST_CASE(getheaders_preserves_stop_empty_and_side_chain_responses)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto* a = AddHeader(original_tip);
    auto* b = AddHeader(a);
    auto* side = AddHeader(a);
    SetTip(b);
    GetHeaders(original_tip);
    GetHeaders(original_tip, a->GetBlockHash());
    GetHeaders(b);
    GetHeaders(nullptr, side->GetBlockHash());
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 4U);
    BOOST_CHECK(sent_headers[0] == (std::vector<uint256>{a->GetBlockHash(), b->GetBlockHash()}));
    BOOST_CHECK(sent_headers[1] == (std::vector<uint256>{a->GetBlockHash()}));
    BOOST_CHECK(sent_headers[2].empty());
    BOOST_CHECK(sent_headers[3] == (std::vector<uint256>{side->GetBlockHash()}));
}

BOOST_AUTO_TEST_CASE(getheaders_keeps_one_branch_when_a_read_releases_cs_main)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto* a = AddHeader(original_tip);
    auto* b = AddHeader(a);
    auto* side = AddHeader(a);
    SetTip(b);
    bool read_unlocked{false};
    source.before_read = [&] {
        std::thread other{[&] {
            TRY_LOCK(cs_main, lock);
            read_unlocked = bool(lock);
            if (lock) SetTip(side);
        }};
        other.join();
    };
    GetHeaders(original_tip);
    BOOST_CHECK(read_unlocked);
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 1U);
    BOOST_CHECK(sent_headers[0] == (std::vector<uint256>{a->GetBlockHash(), b->GetBlockHash()}));
    // The peer was sent b, even though the active tip changed during its read.
    SetTip(b);
    sent_headers.clear();
    Announce(b, a);
    BOOST_CHECK(sent_headers.empty());
}

BOOST_AUTO_TEST_CASE(getheaders_failure_preserves_peer_bookkeeping)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto* a = AddHeader(original_tip);
    auto* b = AddHeader(a);
    SetTip(b);
    GetHeaders(original_tip, a->GetBlockHash());
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 1U);
    BOOST_REQUIRE(sent_headers[0] == (std::vector<uint256>{a->GetBlockHash()}));
    sent_headers.clear();
    source.fail_hash = b->GetBlockHash();
    bool logged{false};
    {
        DebugLogHelper log{"Cannot serve getheaders: local block header storage error", [&](const std::string* line) {
            logged |= line != nullptr;
            return line != nullptr;
        }};
        BOOST_CHECK_NO_THROW(GetHeaders(original_tip));
    }
    BOOST_CHECK(logged);
    BOOST_CHECK(sent_headers.empty());
    BOOST_CHECK(!peer.fDisconnect);
    BOOST_CHECK_EQUAL(b->nStatus & BLOCK_FAILED_MASK, 0U);
    source.fail_hash.reset();
    Announce(b, a);
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 1U);
    BOOST_CHECK(sent_headers[0] == (std::vector<uint256>{b->GetBlockHash()}));
}

BOOST_AUTO_TEST_CASE(getheaders_interruption_sends_no_partial_response)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto* a = AddHeader(original_tip);
    auto* b = AddHeader(a);
    SetTip(b);
    GetHeaders(original_tip);
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 1U);
    BOOST_REQUIRE_EQUAL(sent_headers[0].size(), 2U);
    sent_headers.clear();
    source.before_read = [&] { interrupt = true; };
    GetHeaders(original_tip);
    BOOST_CHECK(sent_headers.empty());
    BOOST_CHECK(!peer.fDisconnect);
}

BOOST_AUTO_TEST_CASE(getheaders_retains_the_existing_protocol_limit)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto* tip = original_tip;
    for (size_t i = 0; i < 20001; ++i) tip = AddHeader(tip);
    SetTip(tip);
    GetHeaders(original_tip);
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 1U);
    BOOST_REQUIRE_EQUAL(sent_headers[0].size(), 20000U);
    BOOST_CHECK(sent_headers[0].front() == indices.front()->GetBlockHash());
    BOOST_CHECK(sent_headers[0].back() == indices[19999]->GetBlockHash());
    BOOST_CHECK_EQUAL(source.reads, 20000U);
}

BOOST_AUTO_TEST_CASE(announcement_storage_failure_does_not_escape_or_advance_peer)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto* a = AddHeader(original_tip);
    auto* b = AddHeader(a);
    SetTip(b);
    GetHeaders(original_tip, a->GetBlockHash());
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 1U);
    BOOST_REQUIRE(sent_headers[0] == (std::vector<uint256>{a->GetBlockHash()}));
    sent_headers.clear();
    source.fail_hash = b->GetBlockHash();
    bool logged{false};
    {
        DebugLogHelper log{"Cannot announce headers: local block header storage error", [&](const std::string* line) {
            logged |= line != nullptr;
            return line != nullptr;
        }};
        BOOST_CHECK_NO_THROW(Announce(b, a));
    }
    BOOST_CHECK(logged);
    BOOST_CHECK(sent_headers.empty());
    BOOST_CHECK(!peer.fDisconnect);
    BOOST_CHECK_EQUAL(b->nStatus & BLOCK_FAILED_MASK, 0U);
    source.fail_hash.reset();
    m_node.peerman->SendMessages(&peer);
    BOOST_REQUIRE_EQUAL(sent_headers.size(), 1U);
    BOOST_CHECK(sent_headers[0] == (std::vector<uint256>{b->GetBlockHash()}));
}

BOOST_AUTO_TEST_CASE(headers_local_flush_failure_stops_processing)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CheckIncomingHeaderFlushFailure(false);
}

BOOST_AUTO_TEST_CASE(compact_block_local_flush_failure_stops_processing)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CheckIncomingHeaderFlushFailure(true);
}

BOOST_AUTO_TEST_SUITE_END()
