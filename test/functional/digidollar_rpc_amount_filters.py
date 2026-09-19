#!/usr/bin/env python3
# Copyright (c) 2025-2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""DigiDollar RPC amount, filter, and confirmation boundary regressions."""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarRPCAmountFiltersTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node0, node1, node2 = self.nodes

        self.log.info("Preparing active DigiDollar chain")
        self.generate(node0, 110)
        self.sync_all()
        for node in self.nodes:
            node.setmockoracleprice(500000)

        self.log.info("Minting two positions with different tiers and amounts")
        tier0_mint = node0.mintdigidollar(20000, 0)
        tier1_mint = node0.mintdigidollar(5000, 1)
        self.generate(node0, 1)
        self.sync_all()

        self.log.info("DD-RH-025: minconf must filter one-confirmation DD balances")
        assert_equal(node0.getdigidollarbalance("", 1)["total"], 25000)
        assert_equal(node0.getdigidollarbalance("", 2)["total"], 0)
        self.generate(node0, 1)
        self.sync_all()
        assert_equal(node0.getdigidollarbalance("", 2)["total"], 25000)

        self.log.info("listdigidollarunspent reports DD amounts in cents and respects confirmation filters")
        unspent = node0.listdigidollarunspent()
        assert_equal(sum(entry["amount"] for entry in unspent), 25000)
        assert_equal({entry["amount"] for entry in unspent}, {5000, 20000})
        for entry in unspent:
            assert "txid" in entry
            assert "vout" in entry
            assert "address" in entry
            assert "confirmations" in entry
            assert "spendable" in entry
            assert entry["spendable"]
            assert entry["confirmations"] >= 2

        assert_equal(node0.listdigidollarunspent(3), [])

        self.log.info("DD-RH-023: position tier 0 and min_amount filters use DD units")
        tier0_positions = node0.listdigidollarpositions(False, 0)
        assert_equal(len(tier0_positions), 1)
        assert_equal(tier0_positions[0]["position_id"], tier0_mint["position_id"])
        assert_equal(tier0_positions[0]["lock_tier"], 0)

        large_positions = node0.listdigidollarpositions(False, None, 7500)
        assert_equal(len(large_positions), 1)
        assert_equal(large_positions[0]["position_id"], tier0_mint["position_id"])
        assert_equal(large_positions[0]["dd_minted"], 20000)

        self.log.info("DD-RH-078: named optional DigiDollar RPC arguments must preserve documented defaults")
        named_tier0_positions = node0.listdigidollarpositions(tier_filter=0)
        assert_equal(len(named_tier0_positions), 1)
        assert_equal(named_tier0_positions[0]["position_id"], tier0_mint["position_id"])

        named_large_positions = node0.listdigidollarpositions(min_amount=7500)
        assert_equal(len(named_large_positions), 1)
        assert_equal(named_large_positions[0]["position_id"], tier0_mint["position_id"])

        assert_equal(node0.getdigidollarbalance(include_watchonly=True)["total"], 25000)
        assert isinstance(node0.listdigidollaraddresses(min_balance=0), list)
        assert isinstance(node0.listdigidollartxs(category="mint"), list)

        self.log.info("DD-RH-072: redemption info must not advertise unsupported partial redemption")
        full_info = node0.getredemptioninfo(tier0_mint["position_id"])
        assert_equal(full_info["redeemable_dd"], 20000)
        assert_raises_rpc_error(
            -8,
            "Exact-amount redemption required",
            node0.getredemptioninfo,
            tier0_mint["position_id"],
            5000,
        )
        # A decimal amount is only read as dollars when the caller says so;
        # without amount_unit it is refused before the position is consulted.
        assert_raises_rpc_error(
            -8,
            "ambiguous amount: pass amount_unit=cents or amount_unit=dollars",
            node0.getredemptioninfo,
            tier0_mint["position_id"],
            "50.00",
        )
        assert_raises_rpc_error(
            -8,
            "Exact-amount redemption required",
            node0.getredemptioninfo,
            tier0_mint["position_id"],
            "50.00",
            "dollars",
        )

        self.log.info("DD-RH-022: decimal strings are dollars only with amount_unit=dollars, never guessed")
        node1_addr = node1.getdigidollaraddress()
        before_node1 = Decimal(node1.getdigidollarbalance()["total"])
        assert_raises_rpc_error(
            -8,
            "ambiguous amount: pass amount_unit=cents or amount_unit=dollars",
            node0.senddigidollar,
            node1_addr,
            "50.00",
        )
        send_result = node0.senddigidollar(address=node1_addr, amount="50.00", amount_unit="dollars")
        assert "txid" in send_result
        self.generate(node0, 1)
        self.sync_all()
        assert_equal(Decimal(node1.getdigidollarbalance()["total"]), before_node1 + Decimal(5000))

        self.log.info("sendmanydigidollar rejects over-capacity recipient metadata before balance/coin selection")
        too_many = {node2.getdigidollaraddress(): 10000000 for _ in range(40)}
        assert_equal(len(too_many), 40)
        assert_raises_rpc_error(
            -8,
            "Too many DigiDollar recipients",
            node0.sendmanydigidollar,
            "",
            too_many,
        )

        node2_addr = node2.getdigidollaraddress()
        before_node2 = Decimal(node2.getdigidollarbalance()["total"])
        assert_raises_rpc_error(
            -8,
            "ambiguous amount: pass amount_unit=cents or amount_unit=dollars",
            node0.sendmanydigidollar,
            "",
            {node2_addr: "25.00"},
        )
        sendmany_result = node0.sendmanydigidollar(
            dummy="", amounts={node2_addr: "25.00"}, comment="decimal string regression", amount_unit="dollars"
        )
        assert_equal(sendmany_result["total_amount"], 2500)
        assert_equal(sendmany_result["amounts"][node2_addr], 2500)
        self.generate(node0, 1)
        self.sync_all()
        assert_equal(Decimal(node2.getdigidollarbalance()["total"]), before_node2 + Decimal(2500))


if __name__ == "__main__":
    DigiDollarRPCAmountFiltersTest().main()
