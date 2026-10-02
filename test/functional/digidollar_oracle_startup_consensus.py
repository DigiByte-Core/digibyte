#!/usr/bin/env python3
"""Consensus-neutrality regression for the DigiDollar startup oracle price scan.

The startup price/volatility reconstruction (OracleBundleManager::LoadPricesFromChain)
was changed to evaluate the per-block BIP9 DigiDollar-activation gate through the
SHARED, memoized versionbits cache (chainman) instead of allocating a throwaway
VersionBitsCache on every one of the up-to ~172,800 iterations. That is the fix for
the ~15-minute "Verifying blocks..." startup hang. It is a PURE performance change:
it computes the identical activation boolean, so the reconstructed oracle price +
volatility-freeze state MUST be byte-identical to a full rescan / -reindex, and MUST
match the pre-restart state.

This test proves exactly that invariant end to end. It builds real on-chain oracle
bundles + DD mints + a volatility move, then asserts the reconstructed oracle state
is IDENTICAL:
  (before)              the live state before any restart
  (A) restart no reindex   -> windowed rescan rebuild
  (B) restart -reindex     -> full reindex rebuild (ground truth)

It also asserts the node does NOT write a durable oracle snapshot: the durable
persist/reindex optimization was intentionally NOT shipped here (it must be made
byte-identical to a full rescan first, before it can be consensus-safe), so a
stray oracle/oracleprices.dat would be a regression.
"""

import os

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class DigiDollarOracleStartupConsensusTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollar=1", "-txindex=1", "-dandelion=0"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def advance(self, seconds, blocks=2):
        """Advance mock time and mine oracle-bundle blocks so the price history
        records the current oracle price at the new timestamps."""
        self.t += seconds
        self.nodes[0].setmocktime(self.t)
        self.nodes[0].generate(blocks)

    def reconstructed_state(self):
        """The reconstruction fingerprint LoadPricesFromChain rebuilds from the
        chain. getoracleprice()['price_micro_usd'] reflects the LIVE regtest
        mock-oracle shim (resets on restart) so it is excluded; last_update_height
        / last_update_time / volatility are the on-chain reconstruction that a
        restart and a -reindex must reproduce identically."""
        p = self.nodes[0].getoracleprice()
        return {
            "last_update_height": p.get("last_update_height"),
            "last_update_time": p.get("last_update_time"),
            "volatility": p.get("volatility"),
        }

    def run_test(self):
        node = self.nodes[0]
        self.t = 1700000000
        node.setmocktime(self.t)

        # --- Build on-chain DD supply + oracle price history + volatility event ---
        node.generate(150)  # past coinbase maturity + DD activation
        node.setmockoracleprice(50000)  # ~$0.50 / DGB
        self.advance(60, blocks=2)
        for _ in range(3):
            res = node.mintdigidollar(100000, 4)  # $1000 each, tier 4
            node.generate(1)
            assert res["txid"] in node.getblock(node.getbestblockhash())["tx"]
            self.advance(60, blocks=1)
        node.setmockoracleprice(9000)  # sharp move engages the volatility state
        for _ in range(4):
            self.advance(300, blocks=2)

        before = self.reconstructed_state()
        self.log.info(f"Pre-restart reconstructed state: {before}")
        assert before["last_update_height"] and before["last_update_height"] > 100

        snapshot = os.path.join(node.chain_path, "oracle", "oracleprices.dat")

        # --- (A) Restart WITHOUT reindex -> windowed rescan rebuild ---
        self.log.info("Restart A: no reindex (windowed rescan reconstruction) ...")
        self.restart_node(0, extra_args=self.extra_args[0])
        node.setmocktime(self.t)
        with open(node.debug_log_path, "r", encoding="utf-8") as fh:
            log_after_a = fh.read()
        # The L1 fix keeps the windowed rescan; it must NOT persist/reload a durable
        # oracle snapshot (that optimization is deferred until it is proven
        # byte-identical to a rescan).
        assert not os.path.exists(snapshot), \
            "unexpected durable oracle snapshot written (L3 must stay reverted)"
        assert "loaded price snapshot" not in log_after_a, \
            "startup took a durable-snapshot fast path (L3 must stay reverted)"
        assert "Scanning last" in log_after_a, \
            "startup did not run the windowed oracle price rescan"
        after_restart = self.reconstructed_state()
        self.log.info(f"Post-restart (rescan) state: {after_restart}")
        assert_equal(before, after_restart)

        # --- (B) Restart WITH -reindex -> full reindex = ground truth ---
        self.log.info("Restart B: -reindex (authoritative full reindex) ...")
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        node.setmocktime(self.t)
        after_reindex = self.reconstructed_state()
        self.log.info(f"Post-reindex state: {after_reindex}")

        # Restart (windowed rescan) must reconstruct EXACTLY what the authoritative
        # a full reindex does, or a restarted node could diverge in consensus.
        assert_equal(after_restart, after_reindex)
        assert_equal(before, after_reindex)
        self.log.info("PASS: restart (rescan) == full-reindex == pre-restart state; "
                      "L1 cache swap is consensus-neutral, no durable snapshot written.")


if __name__ == '__main__':
    DigiDollarOracleStartupConsensusTest().main()
