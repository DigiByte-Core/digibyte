#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Retain DD history from height zero while ordinary DGB history can be pruned."""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error


class DigiDollarZeroFloorPruningTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common = ["-fastprune", "-digidollarstatsindex=0"]
        self.extra_args = [
            common + ["-prune=1"],  # Default regtest DigiDollar deployment starts at zero.
            common + ["-digidollaractivationheight=2147483646"],
        ]

    def setup_network(self):
        self.setup_nodes()
        # Initial block download needs a peer that serves the full chain.
        self.connect_nodes(0, 1)
        self.sync_all()

    def run_test(self):
        retained, control = self.nodes
        assert_equal(retained.getdeploymentinfo()["deployments"]["digidollar"]["height"], 0)
        assert_equal(control.getdeploymentinfo()["deployments"]["digidollar"]["active"], False)

        # Small block files and a tip beyond the recent-block window make the
        # same early files eligible for pruning on the control node.
        for count in [250, 250, 200]:
            self.generate(control, count)
        assert_equal(retained.getbestblockhash(), control.getbestblockhash())
        early_hash = retained.getblockhash(2)
        early_block = retained.getblock(early_hash, 0)
        for node in self.nodes:
            assert (node.blocks_path / "blk00000.dat").is_file()
            assert (node.blocks_path / "rev00000.dat").is_file()

        self.log.info("The control prunes early history while DD retains it")
        self.extra_args[1].append("-prune=1")
        self.restart_node(1, self.extra_args[1])
        self.connect_nodes(0, 1)
        self.sync_all()
        assert_equal(control.getblockchaininfo()["pruned"], True)
        assert_greater_than(control.pruneblockchain(350), 0)
        assert_raises_rpc_error(-1, "Block not available (pruned data)", control.getblock, early_hash)
        assert not (control.blocks_path / "rev00000.dat").exists()
        assert_equal(retained.pruneblockchain(350), -1)
        assert_equal(retained.getblock(early_hash, 0), early_block)
        assert (retained.blocks_path / "blk00000.dat").is_file()
        assert (retained.blocks_path / "rev00000.dat").is_file()

        self.log.info("Restart restores the zero-floor retention lock")
        tip_hash = retained.getbestblockhash()
        self.restart_node(0)
        assert_equal(retained.getbestblockhash(), tip_hash)
        self.connect_nodes(0, 1)
        self.generate(control, 50)
        assert_equal(retained.getbestblockhash(), control.getbestblockhash())
        assert_equal(retained.pruneblockchain(retained.getblockcount()), -1)
        assert_equal(retained.getblock(early_hash, 0), early_block)
        assert (retained.blocks_path / "blk00000.dat").is_file()
        assert (retained.blocks_path / "rev00000.dat").is_file()


if __name__ == '__main__':
    DigiDollarZeroFloorPruningTest().main()
