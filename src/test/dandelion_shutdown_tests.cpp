// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <net.h>
#include <net_processing.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

namespace {
// Observe the real finalizer so clearing routes after deletion cannot pass.
class ShutdownObserver final : public NetEventsInterface {
    ConnmanTestMsg& m_connman;
    PeerManager& m_peerman;

public:
    bool routes_cleared{true};
    std::vector<NodeId> finalized;

    ShutdownObserver(ConnmanTestMsg& connman, PeerManager& peerman)
        : m_connman{connman}, m_peerman{peerman}
    {
        CConnman::Options options;
        options.m_msgproc = this;
        m_connman.Init(options);
    }

    ~ShutdownObserver()
    {
        CConnman::Options options;
        options.m_msgproc = &m_peerman;
        m_connman.Init(options);
    }

    void InitializeNode(CNode& node, ServiceFlags services) override { m_peerman.InitializeNode(node, services); }
    bool ProcessMessages(CNode* node, std::atomic<bool>& interrupt) override EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex)
    {
        return m_peerman.ProcessMessages(node, interrupt);
    }
    bool SendMessages(CNode* node) override EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex) { return m_peerman.SendMessages(node); }
    bool PushDandelionInventory(CNode* node, const CInv& inv) override { return m_peerman.PushDandelionInventory(node, inv); }
    void FinalizeNode(const CNode& node) override
    {
        routes_cleared &= m_connman.DandelionRoutingEmpty();
        finalized.push_back(node.GetId());
        m_peerman.FinalizeNode(node);
    }
};

struct DandelionShutdownSetup : TestingSetup {
    void CheckShutdown(bool source_disconnected)
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        auto& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
        auto& peerman = *m_node.peerman;
        connman.SetPeerConnectTimeout(99999s);

        auto add_peer = [&](NodeId id, ConnectionType type) {
            in_addr address{};
            address.s_addr = 0xa0b0c001 + id;
            auto* node = new CNode(id, nullptr,
                                  CAddress(CService(CNetAddr(address), Params().GetDefaultPort()), NODE_NONE),
                                  0, 0, CAddress(), std::string{}, type, false);
            connman.AddTestNode(*node);
            connman.Handshake(*node, true, ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                             ServiceFlags(NODE_NETWORK | NODE_WITNESS), PROTOCOL_VERSION, true);
            return node;
        };

        auto* destination = add_peer(0, ConnectionType::OUTBOUND_FULL_RELAY);
        auto* source = add_peer(1, ConnectionType::INBOUND);
        connman.AddDandelionDestination(destination);
        connman.AddDandelionInboundTest(source);
        BOOST_REQUIRE(connman.getLocalDandelionDestination() == destination);
        BOOST_REQUIRE(connman.getDandelionDestination(source) == destination);
        BOOST_REQUIRE(peerman.PushDandelionInventory(source, CInv(MSG_DANDELION_TX, uint256S("01"))));
        BOOST_REQUIRE(!connman.DandelionRoutingEmpty());
        if (source_disconnected) connman.MoveTestNodeToDisconnected(*source);

        // The destination is deleted first. Finalizing the source must not use it.
        ShutdownObserver observer{connman, peerman};
        connman.StopNodes();
        BOOST_CHECK(observer.routes_cleared);
        BOOST_CHECK(connman.DandelionRoutingEmpty());
        const std::vector<NodeId> expected{0, 1};
        BOOST_CHECK_EQUAL_COLLECTIONS(observer.finalized.begin(), observer.finalized.end(), expected.begin(), expected.end());

        // A repeated shutdown must be harmless and must not finalize peers again.
        connman.StopNodes();
        BOOST_CHECK_EQUAL(observer.finalized.size(), 2U);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dandelion_shutdown_tests, DandelionShutdownSetup)

BOOST_AUTO_TEST_CASE(clear_routes_before_active_peer_deletion)
{
    CheckShutdown(false);
}

BOOST_AUTO_TEST_CASE(clear_routes_before_disconnected_peer_deletion)
{
    CheckShutdown(true);
}

BOOST_AUTO_TEST_SUITE_END()
