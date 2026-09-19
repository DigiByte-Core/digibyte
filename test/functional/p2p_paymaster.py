#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise the Paymaster P2P negotiation and transport firewall."""

from test_framework.messages import (
    msg_pmcapreq,
    msg_pmcapresp,
    msg_pmquotereq,
    msg_pmquoteresp,
    msg_pmresult,
    msg_pmsubmit,
    msg_sendpmasters,
)
from test_framework.p2p import P2PInterface
from test_framework.paymaster import paymaster_node_args
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    p2p_port,
)


PAYMASTER_PROTOCOL_VERSION = 5


class PaymasterP2PTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(),
            ["-dandelion=0", "-digidollar=1", "-paymaster=0",
             "-prune=0", "-txindex=1", "-v2transport=1"],
            paymaster_node_args() + ["-digidollaractivationheight=100"],
        ]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("A node advertises support only when Paymaster is enabled")
        compatible = node.add_p2p_connection(P2PInterface())
        compatible.wait_until(
            lambda: compatible.message_count["sendpmasters"] == 1)
        assert_equal(
            compatible.last_message["sendpmasters"].version,
            PAYMASTER_PROTOCOL_VERSION)

        incompatible = self.nodes[1].add_p2p_connection(P2PInterface())
        incompatible.sync_with_ping()
        assert_equal(incompatible.message_count["sendpmasters"], 0)

        self.log.info("Negotiation remains inactive before DigiDollar activation")
        inactive = self.nodes[2].add_p2p_connection(P2PInterface())
        inactive.sync_with_ping()
        assert_equal(inactive.message_count["sendpmasters"], 0)
        inactive.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION, capabilities=0))
        assert_equal(inactive.message_count["getpmasters"], 0)

        self.log.info("Invalid negotiation versions and capabilities fail closed")
        wrong_version = node.add_p2p_connection(P2PInterface())
        wrong_version.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION - 1, capabilities=0))
        assert_equal(wrong_version.message_count["getpmasters"], 0)

        unknown_capability = node.add_p2p_connection(P2PInterface())
        unknown_capability.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION, capabilities=1 << 31))
        assert_equal(unknown_capability.message_count["getpmasters"], 0)

        negotiated = node.add_p2p_connection(P2PInterface())
        negotiated.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION, capabilities=0))
        negotiated.wait_until(
            lambda: negotiated.message_count["getpmasters"] == 1)

        self.log.info("Payment-bearing messages always reject plaintext V1")
        # Every command gets an independent connection so the first expected
        # disconnect cannot mask a less strict handler for a later command.
        direct_messages = (
            msg_pmcapreq(b"\x00"),
            msg_pmcapresp(b"\x00"),
            msg_pmquotereq(b"\x00"),
            msg_pmquoteresp(b"\x00"),
            msg_pmsubmit(b"\x00"),
            msg_pmresult(b"\x00"),
        )
        for message in direct_messages:
            peer = node.add_p2p_connection(P2PInterface())
            peer.send_message(message)
            peer.wait_for_disconnect()

        self.log.info("Negotiation never upgrades an ordinary V1 connection")
        plaintext = node.add_p2p_connection(P2PInterface())
        plaintext.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION, capabilities=0))
        plaintext.send_message(msg_pmcapreq(b"\x01"))
        plaintext.wait_for_disconnect()

        self.log.info("Oversized direct payloads are rejected before decoding")
        oversized = node.add_p2p_connection(P2PInterface())
        oversized.send_message(msg_pmsubmit(b"x" * (4 * 1024 * 1024 + 1)))
        oversized.wait_for_disconnect()

        self.log.info("Malformed announcements across V2 reconnects do not stop block relay")
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(1, 2)
        # Reuse real daemon V2 transport and the existing regtest injection RPC.
        # This is bounded integration coverage, not a sustained CPU/RSS benchmark.
        for _ in range(3):
            for index in (1, 2):
                self.connect_nodes(index, 0, peer_advertises_v2=True)
                sender = self.nodes[index]
                peer = next(peer for peer in sender.getpeerinfo()
                            if not peer["inbound"] and
                            peer["addr"].endswith(f":{p2p_port(0)}"))
                assert_equal(peer["transport_protocol_type"], "v2")
                sender.sendmsgtopeer(peer["id"], "sendpmasters", msg_sendpmasters(
                    version=PAYMASTER_PROTOCOL_VERSION, capabilities=0).serialize().hex())
                for _ in range(32):
                    sender.sendmsgtopeer(peer["id"], "pmannounce", "00")
                # A pong queued after this burst proves that the receiver
                # consumed all preceding messages on that same TCP stream.
                pong_bytes = next(p["bytesrecv_per_msg"].get("pong", 0)
                                  for p in sender.getpeerinfo() if p["id"] == peer["id"])
                sender.ping()
                self.wait_until(lambda: any(
                    p["id"] == peer["id"] and
                    p["bytesrecv_per_msg"].get("pong", 0) > pong_bytes
                    for p in sender.getpeerinfo()))
            negotiated.sync_with_ping()
            assert_equal(node.listpaymasters(), [])
            self.generatetoaddress(node, 1, node.get_deterministic_priv_key().address)
            self.sync_blocks()
            assert_equal(self.nodes[1].getbestblockhash(), node.getbestblockhash())
            assert_equal(self.nodes[2].getbestblockhash(), node.getbestblockhash())
            for index in (1, 2):
                self.disconnect_nodes(index, 0)


if __name__ == "__main__":
    PaymasterP2PTest().main()
