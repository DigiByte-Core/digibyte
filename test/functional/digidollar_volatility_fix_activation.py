#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the DD mint volatility gate fix (chain-derived lagged-median anchor).

The legacy "20% in 1 hour" mint circuit breaker compared the candidate oracle
price to the price at the LAST ACCEPTED MINT (a process-local deque), so any
>=20% price drift with no mint in between froze minting permanently. From
nDDVolatilityFixHeight the gate instead compares the candidate price to the
lower median of committed v0x03 bundle prices in ancestor blocks at heights
[H - nDDVolAnchorWindow, H - nDDVolAnchorLag] (regtest: window 144, lag 24),
so the pause self-expires by height arithmetic once the new price level has
been on-chain deeper than the lag.

Scenarios (regtest fix height defaults to 0 = anchor rule active):
1. Bootstrap: empty anchor window => mint passes.
2. Stale-baseline unfreeze (the live testnet26 incident): >=20% drift with no
   intermediate in-window bundles => mint MUST SUCCEED. Fails on the legacy
   rule (permanent deque freeze) — this is the RED discriminator.
3. Genuine spike still pauses: with a dense on-chain price history, a +30%
   jump is rejected with minting-frozen-volatility-candidate; after ~lag
   blocks of bundles at the new price the pause self-expires and minting
   resumes.
4. Legacy preserved below the gate: with -ddvolatilityfixheight far above the
   tip, the old deque rule still governs — the freeze fires after a drift and
   does NOT self-expire.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_raises_rpc_error

# Regtest anchor parameters (consensus/params.h overrides in CRegTestParams)
ANCHOR_LAG = 24
ANCHOR_WINDOW = 144
ANCHOR_MAX_SAMPLES = 15
# Enough blocks that the newest ANCHOR_MAX_SAMPLES bundles at lag depth all
# carry the latest price: lag + samples + margin.
STABILIZE_BLOCKS = ANCHOR_LAG + ANCHOR_MAX_SAMPLES + 6

MINT_CENTS = 1000  # $10.00
MINT_TIER = 4      # 1 year lock

FREEZE_REJECT = "minting-frozen-volatility-candidate"

PRICE_P0 = 500000    # $0.50/DGB
PRICE_P1 = 690000    # +38.0% vs P0
PRICE_P2 = 897000    # +30.0% vs P1


class DigiDollarVolatilityFixActivationTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.base_args = ["-digidollar=1", "-txindex=1", "-dandelion=0"]
        self.extra_args = [self.base_args]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine_with_bundles(self, price, count):
        """Mine blocks that each commit a fresh mock bundle at price.

        setmockoracleprice publishes a new MuSig2 quote for tip+1, which the
        miner stamps into the next block — mirroring a live oracle-fed chain
        where every block carries a bundle. A bare generate() would reuse the
        cached quote for only a few blocks and then mine bundle-less blocks.
        """
        node = self.nodes[0]
        for _ in range(count):
            node.setmockoracleprice(price)
            node.generate(1)

    def mint_ok(self, msg):
        """Mint and mine the tx into the next block, asserting acceptance."""
        node = self.nodes[0]
        self.log.info(f"Expect mint ACCEPTED: {msg}")
        result = node.mintdigidollar(MINT_CENTS, MINT_TIER)
        assert "txid" in result
        block_hash = node.generate(1)[0]
        block = node.getblock(block_hash)
        assert result["txid"] in block["tx"], (
            f"mint tx {result['txid']} not mined into block {block_hash}")
        return result["txid"]

    def mint_frozen(self, msg):
        """Assert the mint is rejected by the volatility freeze."""
        node = self.nodes[0]
        self.log.info(f"Expect mint FROZEN: {msg}")
        assert_raises_rpc_error(-26, FREEZE_REJECT,
                                node.mintdigidollar, MINT_CENTS, MINT_TIER)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Setup: mine past coinbase maturity (no oracle bundles yet)")
        node.generate(110)

        node.setmockoracleprice(PRICE_P0)
        self.mint_ok("bootstrap — empty anchor window (no in-window bundles)")

        self.log.info(
            "Scenario: stale-baseline unfreeze (testnet26 incident reproduction)")
        # +38% drift, but the only bundle-bearing blocks are shallower than the
        # anchor lag => empty window => anchor 0 => the gate must pass. The
        # legacy deque rule froze here permanently.
        node.setmockoracleprice(PRICE_P1)
        self.mint_ok(">=20% drift with no in-window bundle history")

        self.log.info("Scenario: genuine spike still pauses, then self-expires")
        # Build a dense stable on-chain price history at P1 deeper than the lag.
        self.mine_with_bundles(PRICE_P1, STABILIZE_BLOCKS)
        node.setmockoracleprice(PRICE_P2)
        self.mint_frozen("+30% jump against a stable in-window anchor")
        # Self-expiry: once the new price level has been committed on-chain
        # deeper than the lag, the anchor follows it and minting resumes.
        self.mine_with_bundles(PRICE_P2, STABILIZE_BLOCKS)
        self.mint_ok("pause self-expired after ~lag blocks at the new price")

        self.log.info("Scenario: legacy rule preserved below the fix height")
        self.restart_node(0, extra_args=self.base_args +
                          ["-ddvolatilityfixheight=99999"])
        # The startup scan replays mint blocks oldest-first with the legacy
        # 1-hour dedupe, so the reconstructed deque reference is P0 (the first
        # mint's price) — the same stale baseline the original process had.
        node.setmockoracleprice(PRICE_P0)  # mock oracle is per-process
        # Discriminator that the legacy rule governs: under the anchor rule
        # this mint would be frozen (the in-window history is dense P2 bundles,
        # -44% away), but the deque reference is P0 so it passes.
        self.mint_ok("legacy gate — candidate matches the stale deque baseline")
        node.setmockoracleprice(PRICE_P1)
        self.mint_frozen("legacy gate — +38% drift vs the deque baseline")
        # The legacy freeze must NOT self-expire: the deque reference only
        # moves when a mint is accepted, so the freeze is permanent below the
        # gate no matter how many blocks carry the new price.
        self.mine_with_bundles(PRICE_P1, STABILIZE_BLOCKS)
        self.mint_frozen("legacy gate — freeze persists after %d blocks"
                         % STABILIZE_BLOCKS)


if __name__ == '__main__':
    DigiDollarVolatilityFixActivationTest().main()
