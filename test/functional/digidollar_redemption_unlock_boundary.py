#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Redemption quotes and submission must agree at the CLTV unlock height."""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarRedemptionUnlockBoundaryTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common = ["-digidollaractivationheight=1", "-dandelion=0", "-txindex=1"]
        self.extra_args = [common, common + ["-ddthawdayheight=200"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        for node, rules in zip(self.nodes, ("legacy", "Thaw Day")):
            self.log.info("Check unlock boundary with %s health rules", rules)
            self.generate(node, 175, sync_fun=self.no_op)
            node.setmockoracleprice(500000)
            mint = node.mintdigidollar(10000, 0)
            self.generate(node, 1, sync_fun=self.no_op)
            unlock_height = node.getredemptioninfo(mint["position_id"])["unlock_height"]
            self.generate(node, unlock_height - 2 - node.getblockcount(), sync_fun=self.no_op)

            for remaining in (2, 1):
                node.setmockoracleprice(500000)
                info = node.getredemptioninfo(mint["position_id"])
                assert_equal(info["timelock_remaining"], remaining)
                assert_equal(info["can_redeem"], False)
                assert_equal(info["status"], "active")
                assert_raises_rpc_error(
                    -8, "Position locked until block", node.redeemdigidollar,
                    mint["position_id"], 10000,
                )
                self.generate(node, 1, sync_fun=self.no_op)

            assert_equal(node.getblockcount(), unlock_height)
            node.setmockoracleprice(500000)
            info = node.getredemptioninfo(mint["position_id"])
            assert_equal(info["timelock_remaining"], 0)
            assert_equal(info["can_redeem"], True)
            assert_equal(info["status"], "unlocked")
            redemption = node.redeemdigidollar(mint["position_id"], 10000)
            assert redemption["txid"] in node.getrawmempool()
            block_hash = self.generate(node, 1, sync_fun=self.no_op)[0]
            assert redemption["txid"] in node.getblock(block_hash)["tx"]


if __name__ == "__main__":
    DigiDollarRedemptionUnlockBoundaryTest().main()
