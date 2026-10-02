#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test DigiDollar wallet state across -reindex.

A confirmed, unredeemed mint must remain an active position after the wallet
reindexes the chain. Reindex and rescan code must not interpret the mint collateral
as spent unless a real redeem/transfer path spends it on the active chain.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


def position_is_active(position):
    return position.get("is_active", position.get("status") in ("active", "unlocked"))


class WalletDigiDollarReindexTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txindex=1"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mining spendable funds")
        node.generate(200)
        node.setmockoracleprice(500000)

        self.log.info("Minting and confirming DigiDollar")
        mint = node.mintdigidollar(100000, 0)
        position_id = mint["position_id"]
        node.generate(1)
        node.syncwithvalidationinterfacequeue()

        balance = node.getdigidollarbalance()
        assert_equal(balance["confirmed"], 100000)
        assert_equal(balance["total"], 100000)

        positions = node.listdigidollarpositions(False)
        position = next(p for p in positions if p["position_id"] == position_id)
        assert_equal(position_is_active(position), True)

        self.log.info("Restarting with -reindex")
        chain_height = node.getblockcount()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex=1"])
        node = self.nodes[0]
        node.syncwithvalidationinterfacequeue()
        assert_equal(node.getblockcount(), chain_height)

        balance = node.getdigidollarbalance()
        assert_equal(balance["confirmed"], 100000)
        assert_equal(balance["unconfirmed"], 0)
        assert_equal(balance["total"], 100000)

        positions = node.listdigidollarpositions(False)
        position = next(p for p in positions if p["position_id"] == position_id)
        assert_equal(position_is_active(position), True)


if __name__ == "__main__":
    WalletDigiDollarReindexTest().main()
