#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check the node always finishes shutting down when it stops itself at a height.

A node started with -stopatheight stops itself from inside block connection, as
soon as the chain reaches that height. That shutdown therefore starts while the
block that triggered it is still being handed to the rest of the node, so the
shutdown thread races the other threads that are still working on that block.

The node used to lose that race sometimes and never exit. The race is a matter
of microseconds, so one shutdown proves nothing; this test does many shutdowns
and fails if any one of them leaves a node that will not exit.

Pruning is on because the node is measurably more likely to lose the race that
way, which is also how rpc_blockchain.py runs its own stop-at-height check.
"""
import http.client

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

# Blocks mined before the first stop-at-height shutdown.
STARTING_HEIGHT = 206

# How many stop-at-height shutdowns to perform. On the broken code roughly one
# shutdown in three hung, so this many rounds fail it nearly every time.
ROUNDS = 20

# How long to allow one shutdown. A healthy shutdown here takes a fifth of a
# second, so anything approaching this means the node is stuck.
SHUTDOWN_TIMEOUT = 60


class ShutdownAtHeightTest(DigiByteTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [['-easypow', '-prune=1']]

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        self.generate(wallet, STARTING_HEIGHT, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), STARTING_HEIGHT)

        # The node is stopped for most of each round, so keep count here rather
        # than asking it.
        height = STARTING_HEIGHT
        for round_number in range(1, ROUNDS + 1):
            stop_height = height + 1

            self.stop_node(0)
            self.start_node(0, extra_args=[
                '-easypow',
                '-prune=1',
                '-stopatheight={}'.format(stop_height),
            ])

            try:
                self.generatetoaddress(node, 1, wallet.get_address(), sync_fun=self.no_op)
            except (ConnectionError, http.client.BadStatusLine, http.client.CannotSendRequest):
                pass  # the node can shut down before it answers the mining call

            # This is the check. If the node is stuck this never becomes true.
            node.wait_until_stopped(timeout=SHUTDOWN_TIMEOUT)
            height = stop_height
            self.log.info("round %d of %d: node stopped itself at height %d",
                          round_number, ROUNDS, stop_height)

        self.start_node(0, extra_args=['-easypow', '-prune=1'])
        assert_equal(node.getblockcount(), STARTING_HEIGHT + ROUNDS)


if __name__ == '__main__':
    ShutdownAtHeightTest().main()
