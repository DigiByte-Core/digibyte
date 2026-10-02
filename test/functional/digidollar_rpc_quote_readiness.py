#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A previous-epoch quote must fail closed with an actionable RPC error."""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarRPCQuoteReadinessTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-digidollaractivationheight=1", "-ddthawdayheight=200",
            "-dandelion=0", "-txindex=1", "-rpcdoccheck=1",
        ]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def check_waiting(self):
        node = self.nodes[0]
        health = node.getdigidollarstats()["next_block_health"]
        self.log.info("Next-block health while waiting for the new epoch: %s", health)
        assert_equal(health["ready"], False)
        assert "epoch mismatch" in health["data_error"]
        for method, args in ((node.estimatecollateral, (10000, 0)), (node.getdcamultiplier, ())):
            assert_raises_rpc_error(
                -1, "Waiting for a valid signed oracle quote for the next block", method, *args,
            )

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 238)
        node.setmockoracleprice(500000)
        self.generate(node, 1)
        assert_equal(node.getblockcount(), 239)
        node.enablemockoracle(False)
        self.check_waiting()

        self.log.info("Restart must preserve the epoch check and explain the wait")
        self.restart_node(0)
        self.check_waiting()

        self.log.info("A valid quote for the new epoch restores both RPCs")
        node.setmockoracleprice(500000)
        assert_equal(node.getdigidollarstats()["next_block_health"]["ready"], True)
        assert node.estimatecollateral(10000, 0)["required_dgb"] > 0
        assert_equal(node.getdcamultiplier()["next_block_health"]["ready"], True)


if __name__ == "__main__":
    DigiDollarRPCQuoteReadinessTest().main()
