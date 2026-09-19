#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""senddigidollar and redeemdigidollar reject amounts above $100,000 at the RPC boundary.

The DigiDollar amount parser reads ``10000`` as 10,000 cents ($100.00) and,
with ``amount_unit="dollars"``, ``10000.00`` as $10,000.00 (a decimal without
a unit is refused, see digidollar_rpc_amount_units.py), so a habitual decimal
point can still ask for 100x the intended amount when the unit is given.
``sendmanydigidollar`` already refuses a per-recipient amount
above 10,000,000 cents ($100,000) before touching the wallet; ``senddigidollar``
and ``redeemdigidollar`` did not, so an over-cap request reached the wallet's
balance query, coin selection, or position lookup first and surfaced as a
misleading later error.

This test asserts, for both RPCs:

* 10,000,001 cents (integer form and ``"100000.01"`` with ``amount_unit="dollars"``) is
  rejected with the cap error, error code -8 (RPC_INVALID_PARAMETER), and no
  wallet state changes: DD balance, DD history, DD UTXOs and the mempool are
  all unchanged.
* 10,000,000 cents exactly is NOT rejected by the cap. Regtest limits a single
  mint to 100,000 cents ($1,000), so no single vault can reach $100,000 and the
  test wallet's balance stays far below it; the exactly-at-cap request
  therefore passes the cap and fails later with the ordinary balance error
  (send) or exact-amount / position-not-found error
  (redeem), which is asserted explicitly.
* An ordinary send and an ordinary full redemption still succeed with the cap
  in place.

The cap applies only to the amount the caller supplies. The emergency (ERR)
burn that a redemption may compute from the vault principal is not capped and
is not exercised here.
"""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

CAP_CENTS = 10_000_000  # $100,000.00
SEND_CAP_MESSAGE = "Amount exceeds maximum transfer limit ($100,000)"
REDEEM_CAP_MESSAGE = "Amount exceeds maximum redemption limit ($100,000)"
REGTEST_MAX_MINT_CENTS = 100_000  # regtest maxMintAmount ($1,000)
UNKNOWN_POSITION_ID = "00" * 32
ORACLE_PRICE_MICRO_USD = 500000  # $0.50 per DGB


class DigiDollarRPCAmountCapTest(DigiByteTestFramework):
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
        """Snapshot of everything an accidental send or redeem would change."""
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

        self.log.info("Preparing an active DigiDollar chain with two regtest-maximum positions")
        self.generate(node0, 110)
        self.publish_quote()
        position_a = node0.mintdigidollar(REGTEST_MAX_MINT_CENTS, 0)
        position_b = node0.mintdigidollar(REGTEST_MAX_MINT_CENTS, 0)
        self.generate(node0, 2)
        assert_equal(Decimal(node0.getdigidollarbalance()["total"]), Decimal(2 * REGTEST_MAX_MINT_CENTS))
        recv_addr = node1.getdigidollaraddress()

        self.test_send_cap(node0, node1, recv_addr)
        self.test_redeem_cap(node0, position_a["position_id"], position_b["position_id"])

    def test_send_cap(self, node0, node1, recv_addr):
        self.log.info("senddigidollar: 10,000,001 cents is rejected at the boundary with no wallet side effects")
        before = self.dd_state(node0)
        assert_raises_rpc_error(-8, SEND_CAP_MESSAGE, node0.senddigidollar, recv_addr, CAP_CENTS + 1)
        assert_raises_rpc_error(-8, SEND_CAP_MESSAGE, node0.senddigidollar, address=recv_addr, amount="100000.01", amount_unit="dollars")
        # A wildly over-cap amount (the 100x typo the issue describes) is caught the same way.
        assert_raises_rpc_error(-8, SEND_CAP_MESSAGE, node0.senddigidollar, address=recv_addr, amount="10000000.00", amount_unit="dollars")
        assert_equal(self.dd_state(node0), before)

        self.log.info("senddigidollar: exactly 10,000,000 cents passes the cap and fails on balance instead")
        # Regtest cannot hold $100,000 of DD, so the request must get past the cap
        # and reach the ordinary balance check (-6, RPC_WALLET_INSUFFICIENT_FUNDS).
        assert_raises_rpc_error(-6, "Insufficient DD balance", node0.senddigidollar, recv_addr, CAP_CENTS)
        assert_raises_rpc_error(-6, "Insufficient DD balance", node0.senddigidollar, address=recv_addr, amount="100000.00", amount_unit="dollars")
        assert_equal(self.dd_state(node0), before)

        self.log.info("senddigidollar: an ordinary send still succeeds with the cap in place")
        result = node0.senddigidollar(recv_addr, 5000)
        assert_equal(result["amount"], 5000)
        assert result["txid"] in node0.getrawmempool()
        self.generate(node0, 1)
        assert_equal(Decimal(node1.getdigidollarbalance()["total"]), Decimal(5000))
        assert_equal(Decimal(node0.getdigidollarbalance()["total"]), Decimal(2 * REGTEST_MAX_MINT_CENTS - 5000))

    def test_redeem_cap(self, node0, position_a, position_b):
        self.log.info("redeemdigidollar: 10,000,001 cents is rejected before any position lookup")
        before = self.dd_state(node0)
        # The cap must fire even for a position this wallet does not have, i.e.
        # before the wallet is consulted at all.
        assert_raises_rpc_error(-8, REDEEM_CAP_MESSAGE, node0.redeemdigidollar, UNKNOWN_POSITION_ID, CAP_CENTS + 1)
        assert_raises_rpc_error(-8, REDEEM_CAP_MESSAGE, node0.redeemdigidollar, position_a, CAP_CENTS + 1)
        assert_raises_rpc_error(-8, REDEEM_CAP_MESSAGE, node0.redeemdigidollar, position_id=position_a, dd_amount="100000.01", amount_unit="dollars")
        assert_raises_rpc_error(-8, REDEEM_CAP_MESSAGE, node0.redeemdigidollar, position_id=position_a, dd_amount="1000000.00", amount_unit="dollars")
        assert_equal(self.dd_state(node0), before)

        self.log.info("redeemdigidollar: exactly 10,000,000 cents passes the cap and reaches the position logic")
        assert_raises_rpc_error(-8, "Position not found", node0.redeemdigidollar, UNKNOWN_POSITION_ID, CAP_CENTS)
        assert_equal(self.dd_state(node0), before)

        self.log.info("Maturing the tier-0 positions (240-block lock plus the 100-block confirmation buffer)")
        self.generate(node0, 350)
        self.publish_quote()
        before = self.dd_state(node0)

        # With the position unlocked, an exactly-at-cap request gets all the way
        # to the exact-amount rule, proving the cap did not intercept it.
        assert_raises_rpc_error(
            -8,
            "Exact-amount redemption required: must redeem full vault amount of %d cents (requested: %d cents)"
            % (REGTEST_MAX_MINT_CENTS, CAP_CENTS),
            node0.redeemdigidollar,
            position_a,
            CAP_CENTS,
        )
        assert_raises_rpc_error(-8, REDEEM_CAP_MESSAGE, node0.redeemdigidollar, position_a, CAP_CENTS + 1)
        assert_equal(self.dd_state(node0), before)

        self.log.info("redeemdigidollar: an ordinary full redemption still succeeds with the cap in place")
        result = node0.redeemdigidollar(position_a, REGTEST_MAX_MINT_CENTS)
        assert result["txid"] in node0.getrawmempool()
        assert_equal(result["position_id"], position_a)
        assert result["position_closed"]
        self.generate(node0, 1)
        # listdigidollarpositions defaults to active_only=true.
        active_positions = [p["position_id"] for p in node0.listdigidollarpositions()]
        assert position_a not in active_positions
        assert position_b in active_positions
        assert_equal(
            Decimal(node0.getdigidollarbalance()["total"]),
            Decimal(2 * REGTEST_MAX_MINT_CENTS - 5000 - REGTEST_MAX_MINT_CENTS),
        )


if __name__ == "__main__":
    DigiDollarRPCAmountCapTest().main()
