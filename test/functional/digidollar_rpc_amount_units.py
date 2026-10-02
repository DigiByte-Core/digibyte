#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""One explicit amount_unit contract across the DigiDollar amount RPCs.

The DigiDollar RPCs used to guess the unit of an amount from its shape:
``10000`` was 10,000 cents ($100.00) while ``10000.00`` was $10,000.00, so a
habitual decimal point asked for one hundred times the intended amount.

The contract asserted here, for ``senddigidollar``, ``sendmanydigidollar``,
``redeemdigidollar`` and the read-only ``getredemptioninfo`` (plus the
``min_amount`` / ``min_balance`` filters that share the parser):

* No ``amount_unit`` and an integer: cents, exactly as before.
* No ``amount_unit`` and a decimal point: refused with an error that names
  the fix ("ambiguous amount: pass amount_unit=cents or amount_unit=dollars")
  and changes nothing in the wallet.
* ``amount_unit="cents"``: the amount must be an integer.
* ``amount_unit="dollars"``: at most two decimal places.
* Exponents, signs, whitespace and other non-plain text are refused.
* The $100,000 cap still applies at the RPC boundary, under either unit.
"""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

AMBIGUOUS = "ambiguous amount: pass amount_unit=cents or amount_unit=dollars"
CENTS_NOT_INTEGRAL = "amount_unit=cents requires an integer number of cents"
TOO_MANY_DECIMALS = "amount_unit=dollars allows at most two decimal places"
NOT_A_NUMBER = "not a plain decimal number"
NEGATIVE = "must not be negative"
BAD_UNIT = 'amount_unit must be "cents" or "dollars"'
SEND_CAP = "Amount exceeds maximum transfer limit ($100,000)"
REDEEM_CAP = "Amount exceeds maximum redemption limit ($100,000)"
REGTEST_MAX_MINT_CENTS = 100_000  # $1,000, the regtest maximum for one mint
ORACLE_PRICE_MICRO_USD = 500000  # $0.50 per DGB


class DigiDollarRPCAmountUnitsTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def dd_state(self, node):
        """Everything an unintended send or redeem would change."""
        return {
            "balance": Decimal(node.getdigidollarbalance()["total"]),
            "history": sorted(tx["txid"] for tx in node.listdigidollartxs(100, 0)),
            "unspent": sorted((u["txid"], u["vout"]) for u in node.listdigidollarunspent()),
            "mempool": sorted(node.getrawmempool()),
            "positions": sorted(p["position_id"] for p in node.listdigidollarpositions()),
        }

    def publish_quote(self):
        for node in self.nodes:
            node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

    def run_test(self):
        node0, node1 = self.nodes
        self.log.info("Preparing an active DigiDollar chain with four regtest-maximum positions")
        self.generate(node0, 110)
        self.publish_quote()
        # Three positions are redeemed at the end of the test. The fourth is
        # the wallet's spare DigiDollar: redeeming a vault burns its full
        # minted amount, and this test also sends DigiDollar away, so without
        # a spare the last redemption would run out of coins to burn.
        self.positions = [node0.mintdigidollar(REGTEST_MAX_MINT_CENTS, 0)["position_id"] for _ in range(4)]
        self.generate(node0, 2)
        assert_equal(Decimal(node0.getdigidollarbalance()["total"]), Decimal(4 * REGTEST_MAX_MINT_CENTS))
        self.recv_addr = node1.getdigidollaraddress()

        self.test_send_units()
        self.test_sendmany_units()
        self.test_filter_units()
        self.test_redeem_units()

    def expect_rejected(self, node, message, fun, *args, **kwargs):
        before = self.dd_state(node)
        assert_raises_rpc_error(-8, message, fun, *args, **kwargs)
        assert_equal(self.dd_state(node), before)

    def send_confirmed(self, fun, *args, **kwargs):
        """Send, then mine the block that confirms it.

        DigiDollar transfers may only spend confirmed coins, so a second send
        from the same wallet has to wait for the first one to be in a block.
        """
        result = fun(*args, **kwargs)
        self.generate(self.nodes[0], 1)
        self.publish_quote()
        return result

    def test_send_units(self):
        node0, node1 = self.nodes
        recv = self.recv_addr

        self.log.info("senddigidollar: a decimal amount with no unit is refused and changes nothing")
        self.expect_rejected(node0, AMBIGUOUS, node0.senddigidollar, recv, "50.00")
        self.expect_rejected(node0, AMBIGUOUS, node0.senddigidollar, recv, "10000.00")
        self.expect_rejected(node0, AMBIGUOUS, node0.senddigidollar, recv, 50.5)
        self.expect_rejected(node0, AMBIGUOUS, node0.senddigidollar, address=recv, amount="1.0")

        self.log.info("senddigidollar: an integer with no unit is still cents")
        before_node1 = Decimal(node1.getdigidollarbalance()["total"])
        result = self.send_confirmed(node0.senddigidollar, recv, 5000)
        assert_equal(result["amount"], 5000)
        result = self.send_confirmed(node0.senddigidollar, recv, "1500")
        assert_equal(result["amount"], 1500)
        assert_equal(Decimal(node1.getdigidollarbalance()["total"]), before_node1 + Decimal(6500))

        self.log.info("senddigidollar: amount_unit=dollars reads decimals as dollars, named and positional")
        before_node1 = Decimal(node1.getdigidollarbalance()["total"])
        result = self.send_confirmed(node0.senddigidollar, address=recv, amount="50.00", amount_unit="dollars")
        assert_equal(result["amount"], 5000)
        result = self.send_confirmed(node0.senddigidollar, address=recv, amount="25.5", amount_unit="dollars")
        assert_equal(result["amount"], 2550)
        result = self.send_confirmed(node0.senddigidollar, address=recv, amount=12, amount_unit="dollars")
        assert_equal(result["amount"], 1200)
        result = self.send_confirmed(node0.senddigidollar, recv, "10.25", "", None, None, "dollars")
        assert_equal(result["amount"], 1025)
        assert_equal(Decimal(node1.getdigidollarbalance()["total"]), before_node1 + Decimal(5000 + 2550 + 1200 + 1025))

        self.log.info("senddigidollar: amount_unit=cents requires an integer")
        before_node1 = Decimal(node1.getdigidollarbalance()["total"])
        result = self.send_confirmed(node0.senddigidollar, address=recv, amount=1000, amount_unit="cents")
        assert_equal(result["amount"], 1000)
        result = self.send_confirmed(node0.senddigidollar, address=recv, amount="1000", amount_unit="cents")
        assert_equal(result["amount"], 1000)
        assert_equal(Decimal(node1.getdigidollarbalance()["total"]), before_node1 + Decimal(2000))
        self.expect_rejected(node0, CENTS_NOT_INTEGRAL, node0.senddigidollar, address=recv, amount="10.00", amount_unit="cents")
        self.expect_rejected(node0, CENTS_NOT_INTEGRAL, node0.senddigidollar, address=recv, amount=10.5, amount_unit="cents")

        self.log.info("senddigidollar: malformed amounts and units are refused under every unit")
        self.expect_rejected(node0, TOO_MANY_DECIMALS, node0.senddigidollar, address=recv, amount="12.345", amount_unit="dollars")
        self.expect_rejected(node0, BAD_UNIT, node0.senddigidollar, address=recv, amount=1000, amount_unit="usd")
        self.expect_rejected(node0, BAD_UNIT, node0.senddigidollar, address=recv, amount=1000, amount_unit="Cents")
        self.expect_rejected(node0, BAD_UNIT, node0.senddigidollar, address=recv, amount=1000, amount_unit="")
        for unit in (None, "cents", "dollars"):
            kwargs = {} if unit is None else {"amount_unit": unit}
            self.expect_rejected(node0, NOT_A_NUMBER, node0.senddigidollar, address=recv, amount="1e3", **kwargs)
            self.expect_rejected(node0, NOT_A_NUMBER, node0.senddigidollar, address=recv, amount="+5", **kwargs)
            self.expect_rejected(node0, NOT_A_NUMBER, node0.senddigidollar, address=recv, amount=" 5", **kwargs)
            self.expect_rejected(node0, NOT_A_NUMBER, node0.senddigidollar, address=recv, amount="5,000", **kwargs)
            self.expect_rejected(node0, NOT_A_NUMBER, node0.senddigidollar, address=recv, amount="", **kwargs)
            self.expect_rejected(node0, NEGATIVE, node0.senddigidollar, address=recv, amount="-5", **kwargs)
            self.expect_rejected(node0, NEGATIVE, node0.senddigidollar, address=recv, amount=-5, **kwargs)
            self.expect_rejected(node0, "Amount must be positive", node0.senddigidollar, address=recv, amount=0, **kwargs)
        self.expect_rejected(node0, "Amount must be positive", node0.senddigidollar, address=recv, amount="0.00", amount_unit="dollars")
        self.expect_rejected(node0, "Amount must be a number", node0.senddigidollar, address=recv, amount=True)
        self.expect_rejected(node0, "Amount must be a number", node0.senddigidollar, address=recv, amount=[5000])

        self.log.info("senddigidollar: the $100,000 cap holds under either unit")
        self.expect_rejected(node0, SEND_CAP, node0.senddigidollar, recv, 10_000_001)
        self.expect_rejected(node0, SEND_CAP, node0.senddigidollar, address=recv, amount=10_000_001, amount_unit="cents")
        self.expect_rejected(node0, SEND_CAP, node0.senddigidollar, address=recv, amount="100000.01", amount_unit="dollars")
        self.expect_rejected(node0, SEND_CAP, node0.senddigidollar, address=recv, amount="10000000.00", amount_unit="dollars")
        # Exactly at the cap passes the cap and fails on balance instead (regtest cannot hold $100,000).
        assert_raises_rpc_error(-6, "Insufficient DD balance", node0.senddigidollar, recv, 10_000_000)
        assert_raises_rpc_error(-6, "Insufficient DD balance", node0.senddigidollar, address=recv, amount="100000.00", amount_unit="dollars")

    def test_sendmany_units(self):
        node0, node1 = self.nodes
        addr_a = node1.getdigidollaraddress()
        addr_b = node1.getdigidollaraddress()

        self.log.info("sendmanydigidollar: a decimal amount with no unit is refused and names the recipient")
        before = self.dd_state(node0)
        assert_raises_rpc_error(-8, AMBIGUOUS, node0.sendmanydigidollar, "", {addr_a: "25.00"})
        assert_raises_rpc_error(-8, addr_a, node0.sendmanydigidollar, "", {addr_a: "25.00"})
        assert_raises_rpc_error(-8, AMBIGUOUS, node0.sendmanydigidollar, "", {addr_a: 1000, addr_b: 12.5})
        assert_equal(self.dd_state(node0), before)

        self.log.info("sendmanydigidollar: integers with no unit are still cents")
        before_node1 = Decimal(node1.getdigidollarbalance()["total"])
        result = self.send_confirmed(node0.sendmanydigidollar, "", {addr_a: 1500, addr_b: "2500"})
        assert_equal(result["total_amount"], 4000)
        assert_equal(result["amounts"][addr_a], 1500)
        assert_equal(result["amounts"][addr_b], 2500)
        assert_equal(Decimal(node1.getdigidollarbalance()["total"]), before_node1 + Decimal(4000))

        self.log.info("sendmanydigidollar: one amount_unit applies to every recipient")
        before_node1 = Decimal(node1.getdigidollarbalance()["total"])
        result = self.send_confirmed(
            node0.sendmanydigidollar, dummy="", amounts={addr_a: "25.00", addr_b: "10.5"}, amount_unit="dollars")
        assert_equal(result["total_amount"], 3550)
        assert_equal(result["amounts"][addr_a], 2500)
        assert_equal(result["amounts"][addr_b], 1050)
        result = self.send_confirmed(node0.sendmanydigidollar, "", {addr_a: 1000, addr_b: "1000"}, "", None, "cents")
        assert_equal(result["total_amount"], 2000)
        assert_equal(Decimal(node1.getdigidollarbalance()["total"]), before_node1 + Decimal(3550 + 2000))

        self.log.info("sendmanydigidollar: bad amounts, bad units and the per-recipient cap are refused")
        before = self.dd_state(node0)
        assert_raises_rpc_error(-8, CENTS_NOT_INTEGRAL, node0.sendmanydigidollar, dummy="", amounts={addr_a: "10.00"}, amount_unit="cents")
        assert_raises_rpc_error(-8, TOO_MANY_DECIMALS, node0.sendmanydigidollar, dummy="", amounts={addr_a: "10.001"}, amount_unit="dollars")
        assert_raises_rpc_error(-8, NOT_A_NUMBER, node0.sendmanydigidollar, "", {addr_a: "1e3"})
        assert_raises_rpc_error(-8, NEGATIVE, node0.sendmanydigidollar, "", {addr_a: -1})
        assert_raises_rpc_error(-8, BAD_UNIT, node0.sendmanydigidollar, dummy="", amounts={addr_a: 1000}, amount_unit="dollar")
        assert_raises_rpc_error(-8, SEND_CAP, node0.sendmanydigidollar, "", {addr_a: 10_000_001})
        assert_raises_rpc_error(-8, SEND_CAP, node0.sendmanydigidollar, dummy="", amounts={addr_a: "100000.01"}, amount_unit="dollars")
        assert_raises_rpc_error(-8, "Amount must be positive", node0.sendmanydigidollar, "", {addr_a: 0})
        assert_equal(self.dd_state(node0), before)

    def test_filter_units(self):
        node0 = self.nodes[0]
        self.log.info("Amount filters share the contract: decimals need a unit")
        assert_raises_rpc_error(-8, AMBIGUOUS, node0.listdigidollarpositions, False, None, "500.00")
        assert_raises_rpc_error(-8, AMBIGUOUS, node0.listdigidollaraddresses, False, "1.50")
        assert_equal(len(node0.listdigidollarpositions(False, None, REGTEST_MAX_MINT_CENTS)), 4)
        assert_equal(len(node0.listdigidollarpositions(min_amount="1000.00", amount_unit="dollars")), 4)
        assert_equal(len(node0.listdigidollarpositions(min_amount="1000.01", amount_unit="dollars")), 0)
        assert_equal(len(node0.listdigidollarpositions(min_amount=REGTEST_MAX_MINT_CENTS + 1, amount_unit="cents")), 0)
        assert isinstance(node0.listdigidollaraddresses(min_balance="0.50", amount_unit="dollars"), list)
        assert_raises_rpc_error(-8, BAD_UNIT, node0.listdigidollarpositions, min_amount=1, amount_unit="pennies")

    def test_redeem_units(self):
        node0 = self.nodes[0]
        pos_dollars, pos_cents, pos_plain, pos_reserve = self.positions

        self.log.info("Maturing the tier-0 positions (240-block lock plus the 100-block confirmation buffer)")
        self.generate(node0, 350)
        self.publish_quote()

        self.log.info("getredemptioninfo: read-only, same contract")
        assert_raises_rpc_error(-8, AMBIGUOUS, node0.getredemptioninfo, pos_dollars, "1000.00")
        info = node0.getredemptioninfo(pos_dollars, "1000.00", "dollars")
        assert_equal(info["total_dd_minted"], REGTEST_MAX_MINT_CENTS)
        assert info["can_redeem"], info
        assert_equal(node0.getredemptioninfo(pos_dollars, REGTEST_MAX_MINT_CENTS)["can_redeem"], True)
        assert_equal(node0.getredemptioninfo(position_id=pos_dollars, dd_amount="100000", amount_unit="cents")["can_redeem"], True)
        assert_raises_rpc_error(
            -8,
            "Exact-amount redemption required: must redeem full vault amount of %d cents (requested: 99999 cents)" % REGTEST_MAX_MINT_CENTS,
            node0.getredemptioninfo, pos_dollars, "999.99", "dollars",
        )
        assert_raises_rpc_error(-8, TOO_MANY_DECIMALS, node0.getredemptioninfo, pos_dollars, "1000.001", "dollars")
        assert_raises_rpc_error(-8, CENTS_NOT_INTEGRAL, node0.getredemptioninfo, pos_dollars, "1000.00", "cents")
        assert_raises_rpc_error(-8, BAD_UNIT, node0.getredemptioninfo, pos_dollars, 100000, "usd")
        assert_raises_rpc_error(-8, REDEEM_CAP, node0.getredemptioninfo, pos_dollars, 10_000_001)
        assert_raises_rpc_error(-8, REDEEM_CAP, node0.getredemptioninfo, pos_dollars, "100000.01", "dollars")

        self.log.info("redeemdigidollar: a decimal amount with no unit is refused and changes nothing")
        self.expect_rejected(node0, AMBIGUOUS, node0.redeemdigidollar, pos_dollars, "1000.00")
        self.expect_rejected(node0, AMBIGUOUS, node0.redeemdigidollar, position_id=pos_dollars, dd_amount="1000.0")
        self.expect_rejected(node0, TOO_MANY_DECIMALS, node0.redeemdigidollar, position_id=pos_dollars, dd_amount="1000.000", amount_unit="dollars")
        self.expect_rejected(node0, CENTS_NOT_INTEGRAL, node0.redeemdigidollar, position_id=pos_dollars, dd_amount="100000.00", amount_unit="cents")
        self.expect_rejected(node0, NOT_A_NUMBER, node0.redeemdigidollar, pos_dollars, "1e5")
        self.expect_rejected(node0, NEGATIVE, node0.redeemdigidollar, pos_dollars, "-100000")
        self.expect_rejected(node0, BAD_UNIT, node0.redeemdigidollar, position_id=pos_dollars, dd_amount=100000, amount_unit="USD")
        self.expect_rejected(node0, REDEEM_CAP, node0.redeemdigidollar, pos_dollars, 10_000_001)
        self.expect_rejected(node0, REDEEM_CAP, node0.redeemdigidollar, position_id=pos_dollars, dd_amount="100000.01", amount_unit="dollars")
        self.expect_rejected(
            node0,
            "Exact-amount redemption required: must redeem full vault amount of %d cents (requested: 99999 cents)" % REGTEST_MAX_MINT_CENTS,
            node0.redeemdigidollar, position_id=pos_dollars, dd_amount="999.99", amount_unit="dollars",
        )

        self.log.info("redeemdigidollar: dollars, explicit cents and plain integer cents all redeem the full vault")
        result = node0.redeemdigidollar(position_id=pos_dollars, dd_amount="1000.00", amount_unit="dollars")
        assert result["txid"] in node0.getrawmempool()
        assert_equal(result["dd_redeemed"], REGTEST_MAX_MINT_CENTS)
        assert result["position_closed"]
        self.generate(node0, 1)
        self.publish_quote()

        result = node0.redeemdigidollar(pos_cents, "100000", None, None, "cents")
        assert result["txid"] in node0.getrawmempool()
        assert_equal(result["dd_redeemed"], REGTEST_MAX_MINT_CENTS)
        self.generate(node0, 1)
        self.publish_quote()

        result = node0.redeemdigidollar(pos_plain, REGTEST_MAX_MINT_CENTS)
        assert result["txid"] in node0.getrawmempool()
        assert_equal(result["dd_redeemed"], REGTEST_MAX_MINT_CENTS)
        self.generate(node0, 1)

        remaining = [p["position_id"] for p in node0.listdigidollarpositions()]
        assert_equal(remaining, [pos_reserve])
        for position_id in (pos_dollars, pos_cents, pos_plain):
            assert_equal(node0.getredemptioninfo(position_id)["status"], "redeemed")


if __name__ == "__main__":
    DigiDollarRPCAmountUnitsTest().main()
