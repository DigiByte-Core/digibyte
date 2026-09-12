#!/usr/bin/env python3
"""Exact health totals and emergency mint restrictions survive restart."""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarHealthRestartConsensusTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollar=1", "-digidollaractivationheight=1",
                            "-txindex=1", "-dandelion=0"]]
        if not self.options.legacy:
            self.extra_args[0].append("-ddthawdayheight=100")

    def add_options(self, parser):
        self.add_wallet_options(parser)
        parser.add_argument("--legacy", action="store_true", help="Also retain the legacy restart regression")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def advance(self, seconds, blocks=2):
        """Advance mock time and mine a few blocks so the price history records
        the current (stable) oracle price at the new timestamps."""
        self.t += seconds
        self.nodes[0].setmocktime(self.t)
        self.nodes[0].generate(blocks)

    def stabilize_and_assert_blocked(self, price, label):
        """Keep the quote stable and check the emergency-health restriction."""
        node = self.nodes[0]
        node.setmockoracleprice(price)
        for _ in range(8):
            self.advance(1200, blocks=2)  # +20 min * 8 = +160 min of stable price
        # Renew the signed quote after aging it; this assertion targets health.
        node.setmockoracleprice(price)
        self.log.info(f"{label}: expecting ERR (emergency state) to block minting")
        assert_raises_rpc_error(-1, "emergency state", node.mintdigidollar, 10000, 0)

    def assert_accounting(self, collateral):
        node = self.nodes[0]
        stats = node.getdigidollarstats()
        assert_equal(stats["total_dd_supply"], 300000)
        assert_equal(int(Decimal(stats["total_collateral_locked"]) * 100000000), collateral)
        assert_equal(stats["active_positions"], 3)
        if self.options.legacy:
            return {
                "block_hash": node.getbestblockhash(),
                "supply": stats["total_dd_supply"],
                "collateral": stats["total_collateral_locked"],
                "vaults": stats["active_positions"],
            }
        state = stats["canonical_health"]
        assert_equal(state["ready"], True)
        assert_equal(state["history_checked"], True)
        assert_equal(state["format_version"], 1)
        assert_equal(state["rules_version"], 1)
        assert_equal(state["activation_height"], 100)
        assert_equal(state["digidollar_height"], 1)
        assert_equal(state["block_hash"], node.getbestblockhash())
        assert_equal(state["genesis_hash"], node.getblockhash(0))
        assert_equal(state["open_vault_principal"], 300000)
        assert_equal(state["collateral"], collateral)
        assert_equal(state["active_vaults"], 3)
        assert_equal(stats["selected_health_denominator"], "open_vault_principal")
        assert_equal(stats["health_denominator_cents"], 300000)
        return state

    def run_test(self):
        node = self.nodes[0]
        self.t = 1700000000
        node.setmocktime(self.t)
        base_price = 50000  # $0.05 / DGB, in micro-USD

        # --- Build real on-chain DD supply at a healthy price ---
        node.generate(150)  # past coinbase maturity + DD activation height
        node.setmockoracleprice(base_price)
        self.advance(60, blocks=2)
        collateral = 0
        for _ in range(3):
            res = node.mintdigidollar(100000, 4)  # $1000 each, tier 4
            collateral += int(Decimal(res["dgb_collateral"]) * 100000000)
            node.generate(1)
            assert res["txid"] in node.getblock(node.getbestblockhash())["tx"]
            self.advance(60, blocks=1)

        # --- Stress below 100% health, settle volatility, confirm ERR blocks ---
        err_price = 9000  # $0.009 / DGB -> locked collateral devalued -> < 100%
        self.stabilize_and_assert_blocked(err_price, "Pre-restart")
        before_restart = self.assert_accounting(collateral)

        # --- Restart the node (no reindex) ---
        self.log.info("Restarting node (no reindex) ...")
        self.restart_node(0, extra_args=self.extra_args[0])
        node.setmocktime(self.t)
        assert_equal(self.assert_accounting(collateral), before_restart)

        # Restoring exact totals must preserve the same health restriction.
        self.stabilize_and_assert_blocked(err_price, "Post-restart")
        self.assert_accounting(collateral)

        self.log.info("Exact health totals and the emergency mint restriction survived restart")


if __name__ == '__main__':
    DigiDollarHealthRestartConsensusTest().main()
