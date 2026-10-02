#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check that compact-block header messages respect network debug logging."""

from test_framework.address import ADDRESS_BCRT1_UNSPENDABLE
from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, create_block
from test_framework.messages import HeaderAndShortIDs, msg_cmpctblock, msg_sendcmpct
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class CompactBlockLoggingTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-debug=net", "-easypow"]]

    def run_test(self):
        node = self.nodes[0]
        self.generatetoaddress(node, 1, ADDRESS_BCRT1_UNSPENDABLE)

        for debug_enabled in (True, False):
            if not debug_enabled:
                self.restart_node(0, extra_args=["-debug=0", "-easypow"])
            self.log.info("Delivering a valid compact block with net logging=%s", debug_enabled)
            assert_equal(node.logging()["net"], debug_enabled)
            peer = node.add_p2p_connection(P2PInterface())
            peer.send_and_ping(msg_sendcmpct(announce=True, version=2))
            block = create_block(tmpl=node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS))
            block.solve()
            compact = HeaderAndShortIDs()
            compact.initialize_from_block(block, use_witness=True)
            message = f"Saw new cmpctblock header hash={block.hash}"

            with node.assert_debug_log(
                expected_msgs=[message] if debug_enabled else [],
                unexpected_msgs=[] if debug_enabled else [message],
            ):
                peer.send_and_ping(msg_cmpctblock(compact.to_p2p()))
                self.wait_until(lambda: node.getbestblockhash() == block.hash)
            node.disconnect_p2ps()


if __name__ == "__main__":
    CompactBlockLoggingTest().main()
