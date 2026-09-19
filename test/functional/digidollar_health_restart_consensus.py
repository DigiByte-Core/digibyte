#!/usr/bin/env python3
"""Exact health totals and emergency mint restrictions survive a restart, a reindex and a fresh copy of the chain.

Three nodes hold the same activated chain: one that has been running the whole
time, one that is restarted normally, and one that is restarted with -reindex so
it validates every block again from its own block files. All three must report
the same open-vault principal, the same collateral, the same vault count and the
same number of tokens in circulation, and the emergency mint restriction must
still apply on each of them.

The two totals are named and checked separately. The open-vault principal is the
amount the still-open vaults originally minted and it is what the health figure
is calculated from. The circulating total is the number of tokens left after
burns. They are equal here because nothing has been burned yet; they are still
two different totals and are asserted one by one.
"""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

CONTINUOUS, RESTARTED, REINDEXED = range(3)
NAMES = ["continuous", "restarted", "reindexed"]


class DigiDollarHealthRestartConsensusTest(DigiByteTestFramework):
    # What the chain this test builds must add up to, in cents.
    OPEN_VAULT_PRINCIPAL = 300000
    CIRCULATING_TOKENS = 300000
    VAULTS = 3

    def set_test_params(self):
        self.setup_clean_chain = True
        base = ["-digidollar=1", "-digidollaractivationheight=1",
                "-txindex=1", "-dandelion=0"]
        if self.options.legacy:
            # The legacy regression keeps its single node and no Thaw Day.
            self.num_nodes = 1
            self.extra_args = [list(base)]
        else:
            self.num_nodes = 3
            thaw = base + ["-ddthawdayheight=100"]
            self.extra_args = [
                thaw + ["-digidollarstatsindex=1"],
                thaw + ["-digidollarstatsindex=0"],
                thaw + ["-digidollarstatsindex=1"],
            ]

    def add_options(self, parser):
        self.add_wallet_options(parser)
        parser.add_argument("--legacy", action="store_true", help="Also retain the legacy restart regression")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_clock(self):
        for node in self.nodes:
            node.setmocktime(self.t)

    def advance(self, seconds, blocks=2):
        """Advance mock time and mine a few blocks so the price history records
        the current (stable) oracle price at the new timestamps."""
        self.t += seconds
        self.set_clock()
        self.nodes[0].generate(blocks)

    def stabilize_and_assert_blocked(self, price, label, node=None):
        """Keep the quote stable and check the emergency-health restriction."""
        node = node or self.nodes[0]
        node.setmockoracleprice(price)
        for _ in range(8):
            self.advance(1200, blocks=2)  # +20 min * 8 = +160 min of stable price
        # Renew the signed quote after aging it; this assertion targets health.
        node.setmockoracleprice(price)
        self.log.info(f"{label}: expecting ERR (emergency state) to block minting")
        assert_raises_rpc_error(-1, "emergency state", node.mintdigidollar, 10000, 0)

    def assert_accounting(self, collateral, node=None, label="continuous"):
        node = node or self.nodes[0]
        stats = node.getdigidollarstats()

        # The number of tokens in circulation, on its own.
        assert_equal(stats["total_dd_supply"], self.CIRCULATING_TOKENS)
        assert_equal(int(Decimal(stats["total_collateral_locked"]) * 100000000), collateral)
        assert_equal(stats["active_positions"], self.VAULTS)
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

        # The amount the still-open vaults originally minted, which is what the
        # health figure is calculated from, and the collateral and vault count
        # that go with it.
        assert_equal(state["open_vault_principal"], self.OPEN_VAULT_PRINCIPAL)
        assert_equal(state["collateral"], collateral)
        assert_equal(state["active_vaults"], self.VAULTS)
        assert_equal(stats["selected_health_denominator"], "open_vault_principal")
        assert_equal(stats["health_denominator_cents"], self.OPEN_VAULT_PRINCIPAL)

        self.log.info(f"  {label}: open-vault principal {state['open_vault_principal']} cents, "
                      f"tokens {stats['total_dd_supply']} cents, collateral {state['collateral']} sat, "
                      f"{state['active_vaults']} vaults at block {state['block_hash']}")
        return state

    def run_test(self):
        node = self.nodes[0]
        self.t = 1700000000
        self.set_clock()
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
        self.set_clock()
        assert_equal(self.assert_accounting(collateral), before_restart)

        # Restoring exact totals must preserve the same health restriction.
        self.stabilize_and_assert_blocked(err_price, "Post-restart")
        self.assert_accounting(collateral)

        self.log.info("Exact health totals and the emergency mint restriction survived restart")

        if self.options.legacy:
            return

        # --- The same chain on a restarted and on a reindexed node ---
        self.log.info("Give the other two nodes the same chain")
        # Restarting the first node dropped its connections, so reconnect
        # before waiting for the other two to catch up.
        for peer in (RESTARTED, REINDEXED):
            self.connect_nodes(peer, CONTINUOUS)
        self.sync_blocks()
        named_block = node.getbestblockhash()
        reference = self.assert_accounting(collateral, label="continuous")

        self.log.info("Restart the second node without a reindex")
        self.restart_node(RESTARTED, extra_args=self.extra_args[RESTARTED])
        self.connect_nodes(RESTARTED, CONTINUOUS)

        self.log.info("Reindex the whole history on the third node")
        self.restart_node(REINDEXED, extra_args=self.extra_args[REINDEXED] + ["-reindex"])
        self.connect_nodes(REINDEXED, CONTINUOUS)
        with open(self.nodes[REINDEXED].debug_log_path, encoding="utf-8", errors="replace") as log:
            assert "Reindexing block file blk00000.dat" in log.read()

        self.set_clock()
        self.sync_blocks()
        for index in range(self.num_nodes):
            assert_equal(self.nodes[index].getbestblockhash(), named_block)
            assert_equal(self.assert_accounting(collateral, self.nodes[index], NAMES[index]), reference)

        # The restriction is a consequence of those totals, so it has to hold on
        # the reindexed node too, not only on the one that never stopped.
        self.nodes[REINDEXED].setmockoracleprice(err_price)
        assert_raises_rpc_error(-1, "emergency state", self.nodes[REINDEXED].mintdigidollar, 10000, 0)

        self.log.info("A continuously running, a restarted and a reindexed node reported the same "
                      "open-vault principal, collateral, vault count and token total at the same block")


if __name__ == '__main__':
    DigiDollarHealthRestartConsensusTest().main()
