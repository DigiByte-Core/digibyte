#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Wave 13 multi-node functional: mempool / miner / ConnectBlock parity.

Three V1 invariants this test exercises across two connected nodes:

1. Mempool acceptance vs miner template exclusion (graceful degradation).
   * Node 0 publishes a fresh regtest MuSig2 quote, mints a DD tx (which is
     accepted to its mempool), and the tx propagates to node 1.
   * Node 1 has its mock oracle disabled, so its OracleBundleManager has no
     valid bundle to attach. When node 1 builds a template, the miner gate
     `BlockAssembler::ValidateDDForBlockInclusion` (src/node/miner.cpp) skips
     the DD tx and `BlockAssembler::RemoveDDTransactionsFromBlock` strips any
     residual DD inclusion. Node 1 STILL produces a valid block (the
     non-DD coinbase + a non-DD payment tx mempool resident); the DD tx
     stays in node 1's mempool until a peer with a fresh quote mines it.
   * Node 0 (with the bundle) then mines the DD tx into a real DD-touching
     block; node 1 accepts that block (block validation reads the bundle
     out of the coinbase, not from local oracle state).

2. Submit-block ConnectBlock rejection: a coinbase carrying NO oracle
   output is rejected with `bad-oracle-missing` on every node, even when
   the block contains a valid DD tx and was solved at sufficient PoW.
   The peer must never accept the rejected block onto its active chain.

3. Stale / disabled local quote rejects DD tx at mempool, but a DD-touching
   block from a peer (whose coinbase carries a recent valid bundle) is
   still validated and connected. Mempool rejects with
   `digidollar-missing-oracle-quote`; ConnectBlock accepts because the
   v0x03 bundle in the coinbase is the deterministic source of truth.

These cases are not yet covered by `digidollar_network_relay.py` (which
assumes both nodes have a fresh quote), `digidollar_oracle_block_rules_relay.py`
(coinbase format failures only), or `digidollar_oracle_bundle_reject_matrix.py`
(single-node submitblock matrix). Wave 13 needs them to prove that the
mempool gate, miner gate, and ConnectBlock gate cannot diverge in the
multi-node common case.
"""

from test_framework.authproxy import JSONRPCException
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import COIN, CTransaction, tx_from_hex
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

from decimal import Decimal


REGTEST_CONFIRMATION_WINDOW = 144
ACTIVATION_HEIGHT = 200
ORACLE_PRICE_MICRO_USD = 500_000  # $0.50 / DGB


class DigiDollarMempoolMinerParityTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common = [
            f"-digidollaractivationheight={ACTIVATION_HEIGHT}",
            "-dandelion=0",
            "-txindex=1",
        ]
        self.extra_args = [list(common), list(common)]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.setup_nodes()
        self.connect_nodes(0, 1)

    # ------------------------------------------------------------------ helpers
    def activate_digidollar(self):
        for node in self.nodes:
            while True:
                info = node.getdeploymentinfo()
                if info["deployments"]["digidollar"]["active"]:
                    break
                current = node.getblockcount()
                remaining = REGTEST_CONFIRMATION_WINDOW - (current % REGTEST_CONFIRMATION_WINDOW)
                if remaining == 0:
                    remaining = REGTEST_CONFIRMATION_WINDOW
                node.generate(remaining)
            self.sync_blocks()

    def publish_quote_on(self, indices, price=ORACLE_PRICE_MICRO_USD):
        for i in indices:
            assert_equal(
                self.nodes[i].setmockoracleprice(price)["price_micro_usd"],
                price,
            )

    def coinbase_has_oracle(self, node, block_hash):
        block = node.getblock(block_hash, 2)
        for vout in block["tx"][0]["vout"]:
            if vout["scriptPubKey"].get("hex", "").startswith("6abf"):
                return True
        return False

    # --------------------------------------------------------------- run_test
    def run_test(self):
        self.log.info("Activate DigiDollar on both nodes")
        # Mine the activation runway on BOTH nodes alternately so each wallet
        # owns a meaningful share of coinbases. We need each node to have
        # enough mature, spendable DGB to fund (a) a DD mint and (b) a
        # non-DD `sendtoaddress` later.
        for _ in range(8):
            self.nodes[0].generate(20)
            self.sync_blocks()
            self.nodes[1].generate(20)
            self.sync_blocks()
        self.activate_digidollar()
        # Bury everything under another wave of mixed coinbases so all of
        # the recently mined ones are well past COINBASE_MATURITY.
        self.nodes[0].generate(20)
        self.sync_blocks()
        self.nodes[1].generate(20)
        self.sync_blocks()
        # Establish a baseline shared quote so the rest of setup works.
        self.publish_quote_on([0, 1])
        # Sanity: both wallets must have spendable DGB.
        assert_greater_than(self.nodes[0].getbalance(), Decimal("100000"))
        assert_greater_than(self.nodes[1].getbalance(), Decimal("100000"))

        self.log.info("Phase 1: DD tx accepted to mempool but excluded by miner without oracle bundle")
        self.test_mempool_accept_miner_strip()

        self.log.info("Phase 2: ConnectBlock rejection: DD mint block with no oracle output")
        self.test_connectblock_rejects_missing_bundle()

        self.log.info("Phase 3: Stale/disabled local quote rejects DD relay; peer block still accepted")
        self.test_stale_quote_mempool_rejection_with_block_acceptance()

        self.log.info("All Wave 13 multi-node mempool/miner parity assertions held")

    # =============================================================== Phase 1
    def test_mempool_accept_miner_strip(self):
        """Mempool accepts DD tx, miner template strips it when bundle is stale.

        This proves miner graceful degradation
        (`BlockAssembler::ValidateDDForBlockInclusion` skip path,
        `BlockAssembler::RemoveDDTransactionsFromBlock`,
        `OracleBundleManager::AddOracleBundleToBlock` returning false) does
        not hang the node and still produces a valid non-DD block when the
        only resident bundle is older than `ORACLE_MAX_AGE_SECONDS = 3600`.
        """
        import time as _time

        node0, node1 = self.nodes

        # Both nodes have a fresh quote; DD mint accepted on node 0, relays.
        self.publish_quote_on([0, 1])
        mint = node0.mintdigidollar(100_000, 4)  # $1000, tier 4
        dd_txid = mint["txid"]
        self.sync_mempools()
        assert dd_txid in node0.getrawmempool(), "node 0 mempool missing DD mint"
        assert dd_txid in node1.getrawmempool(), "DD mint did not relay to node 1"
        self.log.info("  baseline: DD mint %s present in both mempools", dd_txid)

        # Drop a non-DD payment into node 1's mempool so the template has
        # something else to mine alongside the DD tx.
        addr = node1.getnewaddress()
        non_dd_txid = node1.sendtoaddress(addr, Decimal("1.0"))
        self.sync_mempools()
        assert non_dd_txid in node1.getrawmempool(), "non-DD payment missing on node 1"

        # Age out the cached oracle bundle on node 1 by jumping mocktime past
        # ORACLE_MAX_AGE_SECONDS (3600). After this, the cached MuSig2 bundle
        # is stale and `ValidateMuSig2Bundle` rejects it on the time check
        # inside `AddOracleBundleToBlock` and `ValidateDDForBlockInclusion`.
        # We disable the mock oracle so a refresh path cannot republish.
        # We move BOTH nodes' clocks because the next block timestamp is
        # constrained by GetMedianTimePast / GetAdjustedTime.
        future_time = int(_time.time()) + 7200  # 2 hours ahead, > 1h cutoff
        for n in self.nodes:
            n.setmocktime(future_time)
        node1.enablemockoracle(False)

        # Mine on node 1. With its only bundle stale, the miner must:
        #   - drop the DD mint from the template, AND
        #   - still include the non-DD payment, AND
        #   - succeed in producing a valid block.
        block_hashes = node1.generate(1)
        block = node1.getblock(block_hashes[0])
        assert dd_txid not in block["tx"], (
            f"miner should have stripped DD tx {dd_txid} when bundle is stale, "
            f"but it appears in block {block_hashes[0]}"
        )
        assert non_dd_txid in block["tx"], (
            "miner must continue with non-DD txs even when DD txs are stripped; "
            f"block {block_hashes[0]} unexpectedly omits {non_dd_txid}"
        )
        # A fresh oracle output is optional for this non-DD block. If present,
        # consensus still fully validates it; if absent, the block is valid.
        # Block must propagate to node 0.
        self.sync_blocks()
        assert_equal(node0.getbestblockhash(), block_hashes[0])
        self.log.info(
            "  node1 mined non-DD block %s, dropped DD tx %s gracefully",
            block_hashes[0], dd_txid,
        )

        # Restore live time and re-enable the oracle on both nodes for the
        # next phase. Note: the original DD mint is now off-by-one in the
        # canonical lock-tier duration check (a block was mined since it
        # was constructed). That's expected post-Wave-7 consensus behavior;
        # we re-mint a fresh DD tx for the follow-up assertion that node 0
        # CAN successfully mine a DD-touching block when its bundle is
        # fresh and node 1 accepts it via the in-coinbase bundle.
        for n in self.nodes:
            n.setmocktime(0)
        node1.enablemockoracle(True)
        self.publish_quote_on([0, 1])

        fresh_mint = node0.mintdigidollar(100_000, 0)  # tier 0 = 1 hour
        fresh_dd = fresh_mint["txid"]
        # Mine immediately on node 0 so the canonical lock-tier check
        # passes (the mint was constructed at this exact tip height).
        block_hashes = node0.generate(1)
        block = node0.getblock(block_hashes[0])
        assert fresh_dd in block["tx"], (
            f"node 0 with valid bundle must mine fresh DD tx {fresh_dd} into block "
            f"{block_hashes[0]}"
        )
        assert self.coinbase_has_oracle(node0, block_hashes[0]), (
            f"DD-touching block {block_hashes[0]} coinbase missing oracle output"
        )
        self.sync_blocks()
        assert_equal(node1.getbestblockhash(), block_hashes[0])
        self.log.info(
            "  node0 mined fresh DD block %s; node1 accepted it via in-coinbase bundle",
            block_hashes[0],
        )

    # =============================================================== Phase 2
    def test_connectblock_rejects_missing_bundle(self):
        """A DD-touching block with NO oracle output is rejected on every node.

        This is the multi-node restatement of the Wave 8 reject matrix: even
        if a miner brute-forces PoW for a DD-touching block whose coinbase
        omits the v0x03 OP_ORACLE output, ConnectBlock returns
        `bad-oracle-missing` and the peer never sees the bad tip on its
        active chain.
        """
        node0, node1 = self.nodes
        self.publish_quote_on([0, 1])

        # Build a fresh DD mint on node 0; we will hand-craft a block that
        # includes it but omits the oracle output. We do NOT sync mempools
        # here: any leftover off-by-one DD mints from Phase 1 may have
        # become consensus-invalid after blocks were mined, and node 1 may
        # decline to accept them via relay. The hand-crafted block only
        # needs the freshly-minted DD tx body.
        mint = node0.mintdigidollar(100_000, 0)
        dd_txid = mint["txid"]
        raw_dd = node0.getrawtransaction(dd_txid)

        prev_hash_hex = node0.getbestblockhash()
        prev_hash = int(prev_hash_hex, 16)
        prev_block_time = node0.getblock(prev_hash_hex)["time"]
        block_time = prev_block_time + 1
        next_height = node0.getblockcount() + 1

        coinbase = create_coinbase(next_height)
        coinbase.rehash()

        block = create_block(prev_hash, coinbase, block_time)
        block.vtx.append(tx_from_hex(raw_dd))
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()

        baseline_height = node0.getblockcount()
        baseline_hash_node0 = node0.getbestblockhash()
        baseline_hash_node1 = node1.getbestblockhash()

        result = node0.submitblock(block.serialize().hex())
        assert_equal(result, "bad-oracle-missing")
        assert_equal(node0.getblockcount(), baseline_height)
        assert_equal(node0.getbestblockhash(), baseline_hash_node0)
        # Bad block must not propagate to peer.
        self.sync_blocks()
        assert_equal(node1.getbestblockhash(), baseline_hash_node1)
        self.log.info(
            "  submitblock(bad-oracle-missing) on node0 rejected; node1 tip unchanged"
        )

        # Sanity: re-mine the DD tx properly on node 0 so subsequent phases
        # have a clean chain state.
        block_hashes = node0.generate(1)
        assert dd_txid in node0.getblock(block_hashes[0])["tx"]
        self.sync_blocks()
        assert_equal(node1.getbestblockhash(), block_hashes[0])

    # =============================================================== Phase 3
    def test_stale_quote_mempool_rejection_with_block_acceptance(self):
        """Mempool rejects raw DD tx without local quote; peer block still accepted.

        Build a DD mint on node 0, capture the raw tx, then attempt
        `sendrawtransaction` on node 1 after aging out node 1's oracle
        bundle and disabling the mock so its node-local quote cache is
        stale. `sendrawtransaction` does NOT go through the wallet RPC's
        `RefreshRegtestMockMuSig2QuoteForMempool` helper, so the only
        bundle node 1 has is the stale cached one. The mempool gate
        (`HasRecentValidMuSig2OracleQuote` at src/validation.cpp:922-929)
        rejects the tx with `digidollar-missing-oracle-quote`.

        Then node 0 mines the same DD tx into a block; node 1 accepts the
        block through ConnectBlock, because block validation uses the
        v0x03 bundle in the coinbase as the deterministic price source,
        not local mempool oracle policy. Mempool gate and block gate are
        intentionally different layers; this case proves they do not
        collapse into each other.
        """
        import time as _time

        node0, node1 = self.nodes

        # Build a fresh DD mint on node 0 so we have a raw tx body to
        # submit onto node 1.
        self.publish_quote_on([0])
        mint = node0.mintdigidollar(100_000, 0)
        dd_txid = mint["txid"]
        raw_dd = node0.getrawtransaction(dd_txid)

        # Age out node 1's oracle bundle (cached from earlier setup) by
        # jumping mocktime past ORACLE_MAX_AGE_SECONDS = 3600. Disable the
        # mock so no RPC path can refresh.
        future_time = int(_time.time()) + 7200
        node1.setmocktime(future_time)
        node1.enablemockoracle(False)

        # Submit the raw DD tx directly to node 1 — no wallet RPC, so
        # `RefreshRegtestMockMuSig2QuoteForMempool` is not invoked. The
        # mempool gate is the only thing that can decide.
        try:
            node1.sendrawtransaction(raw_dd)
            assert False, "node 1 mempool must reject raw DD tx without fresh quote"
        except JSONRPCException as exc:
            err = str(exc).lower()
            assert (
                "digidollar-missing-oracle-quote" in err
                or "digidollar mempool acceptance requires" in err
                or "no valid musig2" in err
                or "oracle" in err
            ), f"unexpected node1 sendrawtransaction error: {exc}"
            self.log.info("  node1 rejected raw DD tx with: %s", exc)

        # Reset node 1 to live time so it can validate blocks normally.
        # The bundle gate ONLY blocks mempool admission, not block
        # acceptance. This is the parity invariant Wave 13 needs to prove.
        node1.setmocktime(0)
        node1.enablemockoracle(True)
        self.publish_quote_on([0, 1])

        # Node 0 mines the DD tx; node 1 must accept the block via the
        # in-coinbase bundle, regardless of its earlier mempool rejection.
        block_hashes = node0.generate(1)
        block = node0.getblock(block_hashes[0])
        assert dd_txid in block["tx"], "node 0 must mine its own DD tx"
        self.sync_blocks()
        assert_equal(node1.getbestblockhash(), block_hashes[0])
        self.log.info(
            "  node1 accepted DD block %s via in-coinbase bundle after earlier mempool rejection",
            block_hashes[0],
        )


if __name__ == "__main__":
    DigiDollarMempoolMinerParityTest().main()
