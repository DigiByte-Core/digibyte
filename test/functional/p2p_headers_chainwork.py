#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Adopt a stronger fork using only peer messages, including header presync."""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_greater_than


class HeadersChainworkTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-digidollaractivationheight=2147483646"]] * 2
        self.wallet_names = []

    def replace_chain(self, fork_height, count):
        miner, peer = self.nodes
        old_work = int(peer.getblockchaininfo()["chainwork"], 16)
        fork_time = miner.getblock(miner.getbestblockhash())["time"] + 1000
        self.disconnect_nodes(0, 1)
        miner.invalidateblock(miner.getblockhash(fork_height + 1))
        for node in self.nodes:
            node.setmocktime(fork_time)
        fork = self.generate(miner, count, sync_fun=self.no_op)
        assert_greater_than(int(miner.getblockchaininfo()["chainwork"], 16), old_work)
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=30)
        assert_equal(peer.getbestblockhash(), fork[-1])

    def run_test(self):
        self.generate(self.nodes[0], 1000)
        self.log.info("Accept a stronger fork in one headers message")
        self.replace_chain(750, 301)

        self.generate(self.nodes[0], 2249)
        self.log.info("Accept a stronger fork through both header download passes")
        self.replace_chain(750, 2600)


if __name__ == "__main__":
    HeadersChainworkTest().main()
