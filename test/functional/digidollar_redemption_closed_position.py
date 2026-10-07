#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A repeated redemption must explain the vault state before checking funds."""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarRedemptionClosedPositionTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-digidollaractivationheight=1", "-ddthawdayheight=200",
            "-dandelion=0", "-txindex=1",
        ]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 175)
        node.setmockoracleprice(500000)
        mint = node.mintdigidollar(10000, 0)
        self.generate(node, 1)
        unlock_height = node.getredemptioninfo(mint["position_id"])["unlock_height"]
        self.generate(node, unlock_height - node.getblockcount())
        node.setmockoracleprice(500000)
        redemption = node.redeemdigidollar(mint["position_id"], 10000)
        assert redemption["txid"] in node.getrawmempool()
        assert_equal(node.getdigidollarbalance()["total"], 0)

        self.log.info("A duplicate pending redemption must not request more DigiDollar")
        assert_raises_rpc_error(
            -8, "Position already has a pending redemption", node.redeemdigidollar,
            mint["position_id"], 10000,
        )
        self.generate(node, 1)
        node.setmockoracleprice(500000)
        assert_equal(node.getredemptioninfo(mint["position_id"])["status"], "redeemed")
        self.log.info("A closed vault must not request more DigiDollar")
        assert_raises_rpc_error(
            -8, "Position already redeemed", node.redeemdigidollar,
            mint["position_id"], 10000,
        )


if __name__ == "__main__":
    DigiDollarRedemptionClosedPositionTest().main()
