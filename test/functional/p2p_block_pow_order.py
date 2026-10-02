#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check claimed PoW before screening full and compact blocks for chain work."""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, HeaderAndShortIDs, msg_block, msg_cmpctblock, msg_sendcmpct
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class BlockPoWOrderTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollaractivationheight=2147483646", "-dandelion=0"]]
        self.wallet_names = []

    def add_options(self, parser):
        parser.add_argument("--compact", action="store_true", help="Send compact blocks instead of full blocks")

    def message(self, block):
        if not self.options.compact:
            return msg_block(block)
        compact = HeaderAndShortIDs()
        compact.initialize_from_block(block, prefill_list=[0], use_witness=True)
        return msg_cmpctblock(compact.to_p2p())

    def next_block(self):
        node = self.nodes[0]
        tip = node.getblock(node.getbestblockhash())
        return create_block(int(tip["hash"], 16), create_coinbase(tip["height"] + 1), tip["time"] + 1)

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 800)
        peer = node.add_p2p_connection(P2PInterface())
        peer.send_and_ping(msg_sendcmpct(announce=False, version=2))

        self.log.info("A solved block still reaches normal block acceptance")
        valid = self.next_block()
        valid.solve()
        peer.send_and_ping(self.message(valid))
        self.wait_until(lambda: node.getbestblockhash() == valid.hash)

        self.log.info("Ignore an unsolicited low-work fork before checking its required target")
        fork_parent = node.getblock(node.getblockhash(400))
        low_work = create_block(int(fork_parent["hash"], 16), create_coinbase(401), fork_parent["time"] + 1)
        low_work.nBits = 0x207ffffe  # Valid claimed PoW, but not the required regtest target.
        low_work.solve()
        ignored_message = "Ignoring low-work compact block" if self.options.compact else "Ignoring low-work block"
        with node.assert_debug_log([ignored_message], unexpected_msgs=["bad-diffbits"]):
            peer.send_and_ping(self.message(low_work))
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, low_work.hash)

        if not self.options.compact:
            self.log.info("Keep known headers and requested low-work blocks on their existing paths")
            known = create_block(int(fork_parent["hash"], 16), create_coinbase(401), fork_parent["time"] + 1)
            known.solve()
            node.submitheader(CBlockHeader(known).serialize().hex())
            with node.assert_debug_log([], unexpected_msgs=["Ignoring low-work block"]):
                peer.send_and_ping(msg_block(known))
            assert_raises_rpc_error(-1, "Block not found on disk", node.getblock, known.hash)

            node.getblockfrompeer(known.hash, node.getpeerinfo()[0]["id"])
            peer.wait_for_getdata([known.sha256])
            peer.send_and_ping(msg_block(known))
            assert_equal(node.getblock(known.hash)["height"], 401)

        self.log.info("An impossible target must reach PoW rejection before chain-work screening")
        invalid = self.next_block()
        invalid.nBits = 0
        invalid.rehash()
        with node.assert_debug_log(
            ["header with invalid proof of work"],
            unexpected_msgs=["Ignoring low-work compact block"],
        ):
            peer.send_message(self.message(invalid))
            peer.wait_for_disconnect(timeout=5)
        assert_equal(node.getbestblockhash(), valid.hash)


if __name__ == "__main__":
    BlockPoWOrderTest().main()
