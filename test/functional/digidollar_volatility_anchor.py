#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Miner, restart, and observability coverage for the DD mint volatility anchor.

Companion to digidollar_volatility_fix_activation.py (which pins the anchor
rule itself). This test pins the integration surface:

1. Miner graceful degradation: a mint admitted to the mempool before a >=20%
   committed price move must not break block assembly. The unpatched miner
   validates mints with no anchor (permissive), includes the paused mint, and
   CreateNewBlock then throws when TestBlockValidity rejects the block with
   minting-frozen-volatility-candidate (not in the retryable DD failure list)
   — halting mining on the node. Post-fix the template pre-filters the paused
   mint and a price-independent DD transfer still confirms. (getblocktemplate
   itself needs peers on regtest; generate/generateblock exercise the same
   CreateNewBlock/TestBlockValidity path.)
2. Observability: getprotectionstatus reports the tip-derived anchor
   (anchor_price_micro_usd, anchor_deviation_bps, minting_paused,
   volatility_fix_height) from the same helper consensus uses.
3. Restart determinism: the freeze decision is identical before and after a
   restart (anchor rebuilt from disk, not process state), still frozen just
   below the lag boundary, and self-expiry works after the restart.
4. Incident replay: a long bundle drought (empty anchor window) followed by a
   huge drift mints immediately — the legacy permafreeze is gone.
5. Classification: testmempoolaccept reports the paused mint as
   minting-frozen-volatility-candidate (policy), a block containing it is
   rejected in the block-consensus direction (generateblock), and the same
   raw tx is accepted and mined once the pause self-expires — the policy
   rejection does not poison the tx.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# Regtest anchor parameters (consensus/params.h overrides in CRegTestParams)
ANCHOR_LAG = 24
ANCHOR_WINDOW = 144
ANCHOR_MAX_SAMPLES = 15
ORACLE_EPOCH_BLOCKS = 40
# Enough blocks that the newest ANCHOR_MAX_SAMPLES bundles at lag depth all
# carry the latest price: lag + samples + margin.
STABILIZE_BLOCKS = ANCHOR_LAG + ANCHOR_MAX_SAMPLES + 6
# Bare generate() only stamps the cached mock quote until the next oracle
# epoch boundary (quotes are epoch-bound), so a gap this long guarantees the
# whole anchor window [H-window, H-lag] is bundle-less.
DROUGHT_BLOCKS = ANCHOR_WINDOW + ANCHOR_LAG + ORACLE_EPOCH_BLOCKS + 12

MINT_CENTS = 1000  # $10.00
MINT_TIER = 4      # 1 year lock
TRANSFER_CENTS = 100

FREEZE_REJECT = "minting-frozen-volatility-candidate"
FREEZE_THRESHOLD_BPS = 2000

PRICE_P0 = 500000    # $0.50/DGB
PRICE_P1 = 650000    # +30.0% vs P0 (3000 bps)
PRICE_P3 = 2000000   # huge drift vs P1 (~+207%) for the incident replay
PRICE_P4 = 2600000   # +30.0% vs P3


class DigiDollarVolatilityAnchorTest(DigiByteTestFramework):
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
        """Mine blocks that each commit a fresh mock bundle at price."""
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

    def assert_anchor_status(self, anchor_price, deviation_bps, paused):
        vol = self.nodes[0].getprotectionstatus()["volatility"]
        assert_equal(vol["volatility_fix_height"], 0)
        assert_equal(vol["anchor_price_micro_usd"], anchor_price)
        assert_equal(vol["anchor_deviation_bps"], deviation_bps)
        assert_equal(vol["minting_paused"], paused)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Setup: mature coinbases, confirmed DD balance, dense P0 anchor")
        node.generate(110)
        node.setmockoracleprice(PRICE_P0)
        self.mint_ok("setup mint (DD balance for the transfer)")
        self.mine_with_bundles(PRICE_P0, STABILIZE_BLOCKS)

        self.log.info("Scenario 1: paused mint in the mempool must not break block assembly")
        paused_mint = node.mintdigidollar(MINT_CENTS, MINT_TIER)["txid"]
        transfer = node.senddigidollar(node.getdigidollaraddress(), TRANSFER_CENTS)["txid"]
        mempool = node.getrawmempool()
        assert paused_mint in mempool and transfer in mempool
        # +30% committed by the next block's bundle; the lingering mint is now
        # frozen against the lagged P0 anchor. On the unpatched miner this
        # generate() throws (CreateNewBlock: TestBlockValidity failed:
        # minting-frozen-volatility-candidate) and mining halts — the RED
        # discriminator for the miner wiring.
        node.setmockoracleprice(PRICE_P1)
        block_hash = node.generate(1)[0]
        block_txs = node.getblock(block_hash)["tx"]
        assert transfer in block_txs, "price-independent DD transfer must still confirm"
        assert paused_mint not in block_txs, "paused mint must be excluded from the template"
        assert paused_mint in node.getrawmempool(), "paused mint is skipped, not evicted"
        self.mint_frozen("direct mint attempt while paused")

        self.log.info("Scenario 2: getprotectionstatus reports the tip-derived anchor")
        self.assert_anchor_status(PRICE_P0, 3000, True)

        self.log.info("Scenario 3: identical freeze decision across a restart")
        self.restart_node(0, extra_args=self.base_args)
        node.setmockoracleprice(PRICE_P1)  # mock oracle is per-process
        self.mint_frozen("same rejection after restart — anchor rebuilt from disk")
        self.assert_anchor_status(PRICE_P0, 3000, True)
        self.mine_with_bundles(PRICE_P1, ANCHOR_LAG - 2)
        self.mint_frozen("still paused below the lag boundary after restart")
        self.mine_with_bundles(PRICE_P1, STABILIZE_BLOCKS)
        self.mint_ok("pause self-expired after restart")
        self.assert_anchor_status(PRICE_P1, 0, False)

        self.log.info("Scenario 4: bundle drought then huge drift — mint passes immediately")
        node.generate(DROUGHT_BLOCKS)  # bare blocks: quotes age out at the epoch boundary
        self.assert_anchor_status(0, 0, False)  # empty window => anchor 0
        node.setmockoracleprice(PRICE_P3)
        self.mint_ok("incident replay: huge drift with an empty anchor window")

        self.log.info("Scenario 5: policy classification vs block-consensus direction")
        self.mine_with_bundles(PRICE_P3, STABILIZE_BLOCKS)
        frozen_mint = node.mintdigidollar(MINT_CENTS, MINT_TIER)["txid"]  # accepted at P3
        frozen_hex = node.gettransaction(frozen_mint)["hex"]
        self.restart_node(0, extra_args=self.base_args + ["-persistmempool=0"])
        assert_equal(node.getrawmempool(), [])
        node.setmockoracleprice(PRICE_P4)  # +30% vs the P3 anchor
        res = node.testmempoolaccept([frozen_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], FREEZE_REJECT)
        # Block-consensus direction: a block containing the paused mint is
        # rejected by TestBlockValidity/ConnectBlock with the same reason.
        assert_raises_rpc_error(-25, FREEZE_REJECT, self.generateblock,
                                node, node.getnewaddress(), [frozen_hex])
        # The policy rejection did not poison the tx: after self-expiry the
        # same raw tx is accepted and mined (block-context acceptance).
        self.mine_with_bundles(PRICE_P4, STABILIZE_BLOCKS)
        assert_equal(node.sendrawtransaction(frozen_hex), frozen_mint)
        block_hash = node.generate(1)[0]
        assert frozen_mint in node.getblock(block_hash)["tx"]


if __name__ == '__main__':
    DigiDollarVolatilityAnchorTest().main()
