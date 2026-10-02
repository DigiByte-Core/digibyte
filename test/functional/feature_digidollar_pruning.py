#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""DigiDollar-compatible pruning (v9.26.4).

Before v9.26.4 a DigiByte node that wanted DigiDollar had to run with
``-txindex=1`` (``IsDigiDollarTxIndexRequired`` refused to start otherwise),
and ``-txindex`` is incompatible with ``-prune``. That made it impossible to
run a pruned node once DigiDollar was configured/active.

v9.26.4 makes pruning DigiDollar-compatible:
  1. On a pruned node ``-txindex`` is left off automatically and
     ``IsDigiDollarTxIndexRequired`` returns false, so the node boots. DigiDollar
     validation resolves a spent DD output's amount/lock by reading the creating
     transaction out of the retained block (``node::GetTransaction``'s block-db
     path) instead of the txindex.
  2. A prune lock at the DigiDollar activation floor keeps the DD-era block
     window from being deleted, so historical DD blocks stay available for
     redemption/validation. Pre-activation blocks (which can never contain DD
     data) are still prunable.
  3. ``-digidollarstatsindex`` is left off automatically under prune;
     ``getdigidollarstats`` still works via its live UTXO-set fallback.

This test proves a pruned node (node 1) can do everything a full node (node 0)
can with DigiDollar:

  * F1  - the pruned + DigiDollar-configured node boots with no txindex.
  * F0  - DigiDollar activates at the buried deployment height on both nodes
          simultaneously.
  * F4  - the PRUNED node mints a DigiDollar position, mines the block itself,
          and the full node accepts it (tips + consensus DD state agree).
  * a full user lifecycle (mint -> send -> redeem) runs on the pruned node.
  * F2  - ``pruneblockchain`` on the pruned node deletes early pre-activation
          block files while the activation-floor block and DD-era blocks stay
          available, and the node stays consensus-consistent with the full node.
  * F7  - the "digidollar" prune lock is the BINDING constraint: with the tip far
          enough past the floor that the last-288-blocks window no longer covers
          it, ``pruneblockchain(tip)`` is clamped below the activation floor and
          the floor/mint blocks survive.
  * F6  - after a restart the pruned node reconstructs identical DigiDollar
          stats to the full node.
  * F8  - a THIRD node, pruned from genesis, cold-syncs the whole DD-era chain
          from the full node over P2P (network-driven ConnectBlock resolves DD
          amounts from retained blocks, no txindex, no wallet knowledge) and
          reaches identical DigiDollar state.
  * F11 - reorg across DigiDollar blocks ON the pruned node
          (invalidateblock/reconsiderblock): DisconnectBlock undoes DD supply
          via block-db reads, reconnect restores parity.
  * F10 - the pool migration path: a FULL node with txindex (node 3, following
          the whole test) restarts in place with ``-prune`` (automatic prune
          mode), prunes pre-activation history clamped by the DD lock, stays in
          DD parity, and can even explicitly re-enable the stats index.
  * F9  - a pruned datadir that IS missing DD-era blocks (created by pruning
          with DigiDollar activation and its retention floor temporarily beyond
          the bounded regtest fixture) refuses to start under the original
          activation schedule with retained-history repair guidance;
          ``-prune`` + explicit ``-txindex=1`` is still rejected; and explicit
          ``-reindex`` recovery rebuilds the DD-era window from the network and
          restores full parity.

The fixture uses ``-digidollaractivationheight=432``: DigiDollar is a buried
deployment (BIP90), so the knob retargets the hardcoded activation height and
the static DD/oracle gates to 432 — block 432 is the first block under DD
rules. ``setmockoracleprice`` supplies the regtest oracle quote that lets
DD mint/redeem blocks be built. The pruned node mines every block so its wallet
stays funded (regtest coinbase maturity is 100 blocks for coins at height >= 100,
so seeding funds by mining from genesis is the simplest robust approach).
"""

import hashlib
import os

from test_framework.blocktools import MIN_BLOCKS_TO_KEEP
from test_framework.test_framework import DigiByteTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)

# Buried deployment (BIP90): -digidollaractivationheight sets the hardcoded
# activation height, so block 432 is the first block under DD rules (tip 431 is
# the last pre-activation tip). Mining a few extra blocks past that lands us
# safely inside the active range with mature coinbases to spend.
ACTIVATION_HEIGHT = 432
FIRST_ACTIVE_TIP = 431

ORACLE_PRICE_MICRO_USD = 500_000  # $0.50 / DGB
MINT_AMOUNT_CENTS = 100_000       # $1,000.00
SEND_AMOUNT_CENTS = 5_000         # $50.00
MINT_TIER = 0                     # tier 0 = 1h lock (shortest, quickest to redeem)

# Prune below the DigiDollar activation floor: pre-activation blocks can never
# contain DD data and are safe to delete, while the floor block and every DD-era
# block above it must stay available. A margin keeps the floor block itself out
# of any fully-pruned block file.
PRUNE_TARGET = ACTIVATION_HEIGHT - 50


class DigiDollarPruningTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        common = [
            f"-digidollaractivationheight={ACTIVATION_HEIGHT}",
            "-dandelion=0",
            "-fallbackfee=0.0001",
        ]
        # node 0: full node with txindex, never prunes.
        # node 1: pruned node. -prune=1 selects manual prune mode; the init
        #   parameter interaction leaves -txindex and -digidollarstatsindex off.
        #   -fastprune shrinks block files (64 KiB) and lowers PruneAfterHeight
        #   to 100 so pruning is exercisable on a short regtest chain.
        # node 2: pruned node kept DISCONNECTED at genesis until F8, when it
        #   cold-syncs the entire DD-era chain from node 0 (IBD-side DigiDollar
        #   validation without txindex or wallet knowledge).
        # node 3: FULL node with txindex that passively follows the whole test,
        #   then migrates in place to -prune in F10 (the pool upgrade path).
        #   It runs -fastprune from birth so its block files are small enough
        #   that the migration prune actually deletes pre-activation files.
        self.extra_args = [
            common + ["-txindex=1"],
            common + ["-prune=1", "-fastprune=1"],
            common + ["-prune=1", "-fastprune=1"],
            common + ["-txindex=1", "-fastprune=1"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 3)
        # node 2 stays unconnected until F8.

    # ------------------------------------------------------------------ helpers
    def set_price(self, node):
        """Publish a fresh regtest MuSig2 oracle quote so a DD block can build."""
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

    def gen(self, node, n):
        """Generate n blocks on `node` and sync BLOCKS only to the other node.

        We deliberately avoid sync_all()/sync_mempools(): a DD tx sitting in the
        miner's mempool is not relayable into a peer that has no recent local
        oracle quote, so the mempools legitimately differ until the block is
        mined. Only block agreement matters here.
        """
        return self.generate(
            node, n, sync_fun=lambda: self.sync_blocks([self.nodes[0], self.nodes[1]])
        )

    def blk_file(self, node, index):
        return os.path.join(node.blocks_path, f"blk{index:05}.dat")

    def has_blk(self, node, index):
        return os.path.isfile(self.blk_file(node, index))

    def assert_dd_parity(self, label, pruned_node=None):
        """The pruned node must agree with the full node on tip, deployment
        state and the consensus-derived DigiDollar totals.

        The totals come from the stats index on node 0 and from the equivalent
        UTXO-set scan on the pruned node, and must match — including
        `active_positions` (index `vault_count` on node 0, live vault count
        from the scan on the pruned node).
        """
        n0 = self.nodes[0]
        n1 = pruned_node if pruned_node is not None else self.nodes[1]
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())
        assert_equal(n0.getblockcount(), n1.getblockcount())

        d0 = n0.getdigidollardeploymentinfo()
        d1 = n1.getdigidollardeploymentinfo()
        for key in ("status", "enabled", "activation_height"):
            assert_equal(d0[key], d1[key])

        s0 = n0.getdigidollarstats()
        s1 = n1.getdigidollarstats()
        for key in ("total_dd_supply", "total_collateral_dgb",
                    "total_collateral_locked", "active_positions"):
            assert_equal(s0[key], s1[key])

        self.log.info(
            "  parity OK (%s): height=%d supply=%d",
            label, n0.getblockcount(), s0["total_dd_supply"],
        )

    # --------------------------------------------------------------- run_test
    def run_test(self):
        self.test_f1_pruned_node_booted()
        self.test_f0_activation()
        mint_block_hash = self.test_f4_mint_on_pruned_node()
        self.test_user_lifecycle()
        self.test_f2_prune_retains_dd_window(mint_block_hash)
        self.test_f7_prune_lock_is_binding(mint_block_hash)
        self.test_f6_restart_parity()
        self.test_f11_reorg_across_dd_blocks_on_pruned_node()
        self.test_f15_oracle_runs_on_pruned_node()
        self.test_f12_offline_miner_catches_up()
        self.test_f8_cold_ibd_pruned_node()
        self.test_f10_full_node_migrates_to_pruned()
        self.test_f9_incomplete_dd_window_guard()
        self.test_f13_unprune_requires_reindex()
        self.test_f14_truncated_dd_block_file_fails_closed()

        self.log.info("DigiDollar-compatible pruning tests PASSED")

    # ================================================================== F1
    def test_f1_pruned_node_booted(self):
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F1: pruned DigiDollar-configured node booted without txindex")

        # The pruned node came up at all (pre-v9.26.4 it would refuse to start
        # with "DigiDollar requires -txindex=1").
        info1 = node1.getblockchaininfo()
        assert_equal(info1["pruned"], True)

        # Prune auto-disables both txindex and the DD stats index on node 1.
        # (getindexinfo does not enumerate the DD stats index, so its on-disk
        # database directory is the observable for the parameter interaction.)
        assert "txindex" not in node1.getindexinfo()
        assert "txindex" in node0.getindexinfo()
        statsindex_db = os.path.join("indexes", "digidollarstats", "db")
        assert os.path.isdir(os.path.join(node0.chain_path, statsindex_db))
        assert not os.path.exists(os.path.join(node1.chain_path, statsindex_db))

        # DigiDollar deployment is still fully queryable on the pruned node.
        for node in (node0, node1):
            dep = node.getdigidollardeploymentinfo()
            assert_equal(dep["status"], "defined")   # height 0, pre-activation
            assert_equal(dep["enabled"], False)

    # ================================================================== F0
    def test_f0_activation(self):
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F0: crossing the buried DigiDollar activation height (pruned node follows)")

        # Build the activation chain on the FULL node: a pruned node advertises
        # NODE_NETWORK_LIMITED and does not serve deep history, so it cannot bootstrap a
        # fresh peer via initial block download. The pruned node validates and follows.
        self.gen(node0, FIRST_ACTIVE_TIP + 5)

        # Fund the pruned node's wallet from the full node so it can mint on its own in
        # F4. The full node holds the mature mined coinbases; DigiByte coinbase maturity
        # is 100 confirmations past the early-chain threshold, so coinbases freshly mined
        # on the pruned node would still be immature. A normal payment is spendable after
        # one confirmation, giving the pruned node DGB to lock as collateral.
        addr1 = node1.getnewaddress()
        node0.sendtoaddress(addr1, node0.getbalance() / 3)
        self.gen(node0, 3)

        dep0 = node0.getdigidollardeploymentinfo()
        dep1 = node1.getdigidollardeploymentinfo()
        for dep in (dep0, dep1):
            assert_equal(dep["status"], "active")
            assert_equal(dep["enabled"], True)
            # Buried deployment: the reported activation height is exactly
            # the -digidollaractivationheight knob value.
            assert_equal(dep["activation_height"], ACTIVATION_HEIGHT)
        # Both nodes must agree on where DigiDollar activated.
        assert_equal(dep0["activation_height"], dep1["activation_height"])
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("  DigiDollar ACTIVE on both nodes at height %d",
                      dep0["activation_height"])

    # ================================================================== F4
    def test_f4_mint_on_pruned_node(self):
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F4: minting a DigiDollar position ON THE PRUNED node")

        self.set_price(node1)
        mint = node1.mintdigidollar(MINT_AMOUNT_CENTS, MINT_TIER)
        self.mint_position_id = mint["position_id"]
        self.mint_txid = mint["txid"]

        self.set_price(node1)
        mint_block_hash = self.gen(node1, 1)[0]

        # The full node accepted the pruned node's DD mint block.
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        assert mint["txid"] in node0.getblock(mint_block_hash)["tx"]

        assert_greater_than_or_equal(
            node1.getdigidollarstats()["total_dd_supply"], MINT_AMOUNT_CENTS)
        self.assert_dd_parity("after mint on pruned node")
        return mint_block_hash

    # ============================================================= lifecycle
    def test_user_lifecycle(self):
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("Full user lifecycle (mint -> send -> redeem) on the pruned node")

        # --- transfer some DD (to self) ---
        dd_addr = node1.getdigidollaraddress()
        self.set_price(node1)
        node1.senddigidollar(dd_addr, SEND_AMOUNT_CENTS)
        self.set_price(node1)
        self.gen(node1, 1)
        self.assert_dd_parity("after DD transfer on pruned node")

        # --- redeem the position after it matures ---
        positions = node1.listdigidollarpositions(False)
        position = next(
            p for p in positions if p["position_id"] == self.mint_position_id)
        unlock_height = position["unlock_height"]
        to_unlock = unlock_height - node1.getblockcount()
        if to_unlock > 0:
            self.log.info("  mining %d blocks to reach the tier-%d unlock height",
                          to_unlock, MINT_TIER)
            self.gen(node1, to_unlock)

        self.set_price(node1)
        redeem = node1.redeemdigidollar(self.mint_position_id, MINT_AMOUNT_CENTS)
        assert "txid" in redeem
        self.set_price(node1)
        self.gen(node1, 1)
        assert_equal(node1.getdigidollarstats()["total_dd_supply"], 0)
        self.assert_dd_parity("after redeem on pruned node")

        # --- mint a fresh position and leave it open so the later parity checks
        #     compare a non-zero DD supply/collateral state ---
        self.set_price(node1)
        node1.mintdigidollar(MINT_AMOUNT_CENTS, MINT_TIER)
        self.set_price(node1)
        self.gen(node1, 1)
        assert_equal(node1.getdigidollarstats()["total_dd_supply"], MINT_AMOUNT_CENTS)
        self.assert_dd_parity("after second mint (left open)")

        # --- transfer DD cross-node: pruned wallet -> full node's wallet ---
        # (uses the confirmed second mint; DD transfers are confirmed-only)
        recv_before = int(node0.getdigidollarbalance()["total"])
        dd_addr0 = node0.getdigidollaraddress()
        self.set_price(node1)
        node1.senddigidollar(dd_addr0, SEND_AMOUNT_CENTS)
        self.set_price(node1)
        self.gen(node1, 1)
        assert_equal(int(node0.getdigidollarbalance()["total"]),
                     recv_before + SEND_AMOUNT_CENTS)
        self.assert_dd_parity("after cross-node DD transfer")

    # ================================================================== F2
    def test_f2_prune_retains_dd_window(self, mint_block_hash):
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F2: prune pre-activation blocks; DD-era window is retained")

        # Extend the chain so the prune target sits comfortably below the
        # last-MIN_BLOCKS_TO_KEEP window and pruning is not clamped by it.
        target_height = PRUNE_TARGET + MIN_BLOCKS_TO_KEEP + 20
        extra = target_height - node1.getblockcount()
        if extra > 0:
            self.gen(node1, extra)

        # Capture reference hashes before pruning (getblockhash keeps working on
        # pruned data; only getblock, which needs block *data*, fails).
        early_hash = node1.getblockhash(1)                  # pre-activation block
        early_txid = node1.getblock(early_hash)["tx"][0]    # its coinbase
        floor_hash = node1.getblockhash(ACTIVATION_HEIGHT)  # DD activation floor

        assert self.has_blk(node1, 0), "blk00000.dat should exist before pruning"

        # Manual prune below the activation floor.
        node1.pruneblockchain(PRUNE_TARGET)

        # The first (pre-activation) block file is deleted.
        self.wait_until(lambda: not self.has_blk(node1, 0), timeout=30)

        # An early pre-floor block's data is gone.
        assert_raises_rpc_error(-1, "Block not available (pruned data)",
                                node1.getblock, early_hash)

        # The activation-floor block and a DD-era block are still available.
        node1.getblock(floor_hash)
        node1.getblock(mint_block_hash)

        # getrawtransaction on the no-txindex pruned node: a DD-era tx is
        # retrievable with an explicit blockhash (the block is retained)...
        raw = node1.getrawtransaction(self.mint_txid, True, mint_block_hash)
        assert_equal(raw["txid"], self.mint_txid)
        # ...without a blockhash it needs the (absent) txindex...
        assert_raises_rpc_error(-5, "Use -txindex",
                                node1.getrawtransaction, self.mint_txid)
        # ...and a tx in a pruned pre-activation block errors cleanly.
        assert_raises_rpc_error(-1, "Block not available",
                                node1.getrawtransaction, early_txid, True, early_hash)

        # getblockchaininfo reflects the prune and the node is still fully
        # consistent with the full node's DigiDollar view.
        assert_greater_than_or_equal(node1.getblockchaininfo()["pruneheight"], 1)
        self.assert_dd_parity("after pruning on pruned node")

    # ================================================================== F7
    def test_f7_prune_lock_is_binding(self, mint_block_hash):
        """Prove the "digidollar" prune lock — not the generic keep-the-last-288
        window — is what protects the DD-era blocks.

        We push the tip far enough past the activation floor that the last-288
        window no longer covers the floor. Then ``pruneblockchain(tip)`` asks to
        prune everything: without the lock it would delete the floor and mint
        blocks; with the lock it must clamp below ``floor - PRUNE_LOCK_BUFFER``.
        """
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F7: the DigiDollar prune lock is the binding constraint")

        PRUNE_LOCK_BUFFER = 10  # src/validation.cpp prune-lock clamp

        # Extend so that tip - MIN_BLOCKS_TO_KEEP is comfortably ABOVE the floor
        # (past whole fastprune block files), making the lock the only protection.
        target_tip = ACTIVATION_HEIGHT + MIN_BLOCKS_TO_KEEP + 60
        extra = target_tip - node1.getblockcount()
        if extra > 0:
            self.gen(node1, extra)
        tip = node1.getblockcount()
        assert_greater_than(tip - MIN_BLOCKS_TO_KEEP,
                            ACTIVATION_HEIGHT - PRUNE_LOCK_BUFFER)

        floor_hash = node1.getblockhash(ACTIVATION_HEIGHT)
        pruned_to = node1.pruneblockchain(tip)

        # Clamped by the lock: nothing at or above floor - buffer was pruned.
        assert_greater_than(ACTIVATION_HEIGHT - PRUNE_LOCK_BUFFER, pruned_to)
        # The activation-floor block and the DD mint block are still readable.
        node1.getblock(floor_hash)
        node1.getblock(mint_block_hash)
        self.assert_dd_parity("after prune-to-tip clamped by the DD lock")
        self.log.info("  pruneblockchain(%d) clamped to %d (floor %d)",
                      tip, pruned_to, ACTIVATION_HEIGHT)

    # ================================================================== F6
    def test_f6_restart_parity(self):
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F6: restart the pruned node; DigiDollar stats survive and match")

        self.restart_node(1, extra_args=self.extra_args[1])
        self.connect_nodes(0, 1)
        self.sync_blocks([node0, node1])

        # After restart the pruned node has no stats index and reconstructs its
        # DigiDollar totals from the UTXO set; they must still match the full node.
        self.assert_dd_parity("after restart of pruned node")

        # Service bits: the pruned node advertises NODE_NETWORK_LIMITED, not
        # NODE_NETWORK; the full node advertises NODE_NETWORK.
        assert "NETWORK_LIMITED" in node1.getnetworkinfo()["localservicesnames"]
        assert "NETWORK" not in node1.getnetworkinfo()["localservicesnames"]
        assert "NETWORK" in node0.getnetworkinfo()["localservicesnames"]

        # The "digidollar" prune lock is in-memory and must be re-registered on
        # every startup: a fresh prune-to-tip is still clamped below the floor.
        pruned_to = node1.pruneblockchain(node1.getblockcount())
        assert_greater_than(ACTIVATION_HEIGHT - 10, pruned_to)
        node1.getblock(node1.getblockhash(ACTIVATION_HEIGHT))

        # Wallet rescans on the pruned node: within the retained window they
        # work (DD positions/balance intact), beyond it they error cleanly.
        balance_before = int(node1.getdigidollarbalance()["total"])
        node1.rescanblockchain(ACTIVATION_HEIGHT)
        assert_equal(int(node1.getdigidollarbalance()["total"]), balance_before)
        assert_raises_rpc_error(-1, "Can't rescan beyond pruned data",
                                node1.rescanblockchain, 1)

    # ================================================================== F11
    def test_f11_reorg_across_dd_blocks_on_pruned_node(self):
        """Reorg DigiDollar blocks on the pruned node.

        invalidateblock forces DisconnectBlock to undo the DD transfer and the
        second mint — resolving their input amounts from retained blocks (no
        txindex) — and reconsiderblock reconnects them. Supply must track
        exactly and end back in full parity with the full node.
        """
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F11: reorg across DD blocks on the pruned node")

        tip_height = node1.getblockcount()
        # Tip is the cross-node transfer block; tip-1 is the second mint block.
        mint2_hash = node1.getblockhash(tip_height - 1)

        node1.invalidateblock(mint2_hash)  # disconnects transfer + second mint
        assert_equal(node1.getblockcount(), tip_height - 2)
        assert_equal(node1.getdigidollarstats()["total_dd_supply"], 0)

        node1.reconsiderblock(mint2_hash)  # reconnects both DD blocks
        self.sync_blocks([node0, node1])
        assert_equal(node1.getblockcount(), tip_height)
        self.assert_dd_parity("after reorg across DD blocks on pruned node")

    # ================================================================== F15
    def test_f15_oracle_runs_on_pruned_node(self):
        """A pruned node can operate a DigiDollar ORACLE.

        The oracle daemon needs a wallet-held oracle key, live price input and
        P2P — never the transaction index or pre-activation blocks. node 1's
        pre-DD history is already pruned away at this point; importing the
        deterministic regtest oracle key and starting the oracle must work
        exactly as on a full node.
        """
        node1 = self.nodes[1]
        self.log.info("F15: running an oracle on the pruned node")

        ORACLE_ID = 3
        privkey_hex = hashlib.sha256(
            f"digibyte_regtest_oracle_{ORACLE_ID}".encode()).hexdigest()
        node1.importoracleprivkey(ORACLE_ID, privkey_hex)

        start = node1.startoracle(ORACLE_ID)
        assert_equal(start["success"], True)
        assert_equal(start["status"], "running")

        assert_equal(node1.getoraclepubkey(ORACLE_ID)["is_running"], True)
        lst = node1.listoracle()
        assert_equal(lst["running"], True)
        assert_equal(lst["oracle_id"], ORACLE_ID)

        # Stop it again so later phases (restarts, damage scenarios) run on the
        # same node state as before.
        node1.stoporacle(ORACLE_ID)
        assert_equal(node1.listoracle()["running"], False)
        self.log.info("  oracle started, reported running, and stopped cleanly"
                      " on the pruned node")

    # ================================================================== F12
    def test_f12_offline_miner_catches_up(self):
        """The offline-miner scenario: a pruned mining node drops offline, mines
        a short stale chain of its own (including a DigiDollar mint!), while the
        network advances by MORE blocks than the generic 288-block window. On
        reconnect it must abandon its stale chain, adopt the network chain, and
        end in exact DigiDollar parity — never stuck, never on the wrong chain.
        """
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F12: offline pruned miner reorgs onto the network chain")

        parity_supply = node1.getdigidollarstats()["total_dd_supply"]
        self.disconnect_nodes(0, 1)

        # Offline: node1 mines its own short chain WITH a DD mint on it.
        self.set_price(node1)
        node1.mintdigidollar(MINT_AMOUNT_CENTS, MINT_TIER)
        self.set_price(node1)
        self.generate(node1, 2, sync_fun=self.no_op)
        assert_equal(node1.getdigidollarstats()["total_dd_supply"],
                     parity_supply + MINT_AMOUNT_CENTS)

        # Meanwhile the network advances well past MIN_BLOCKS_TO_KEEP.
        self.generate(node0, MIN_BLOCKS_TO_KEEP + 32, sync_fun=self.no_op)

        # Back online: the pruned node must reorg off its stale chain (the
        # stale mint disappears from consensus totals) and catch up.
        self.connect_nodes(0, 1)
        self.sync_blocks([node0, node1], timeout=120)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        assert_equal(node1.getdigidollarstats()["total_dd_supply"], parity_supply)
        self.assert_dd_parity("after offline miner rejoined the network")

    # ================================================================== F8
    def test_f8_cold_ibd_pruned_node(self):
        """A fresh pruned node cold-syncs the entire DD-era chain over P2P.

        node 2 has been idle at genesis the whole test. Connecting it to the
        full node makes it validate every DigiDollar mint/transfer/redeem block
        via network-driven ConnectBlock — resolving DD input amounts from the
        retained blocks on its own disk, with no txindex and no wallet
        knowledge of the transactions.
        """
        node0, node2 = self.nodes[0], self.nodes[2]
        self.log.info("F8: fresh pruned node cold-syncs the DD-era chain (IBD)")

        assert_equal(node2.getblockcount(), 0)
        self.connect_nodes(0, 2)
        self.sync_blocks([node0, node2], timeout=120)
        self.assert_dd_parity("cold IBD onto a pruned node", pruned_node=node2)

    # ================================================================== F10
    def test_f10_full_node_migrates_to_pruned(self):
        """The pool upgrade path: an existing FULL node (txindex, complete
        history) restarts in place with -prune.

        node 3 has followed the entire test as a full node. After the restart,
        txindex and the stats index are auto-disabled, `pruneblockchain`
        deletes the pre-activation history (clamped by the DD lock — this run
        also covers automatic prune mode, -prune=550, unlike node 1's manual
        mode), DigiDollar state stays in parity, and an explicit
        -digidollarstatsindex=1 opt-in still works on the pruned datadir.
        """
        node0, node3 = self.nodes[0], self.nodes[3]
        self.log.info("F10: full node migrates in place to pruned (pool path)")

        assert "txindex" in node3.getindexinfo()
        self.assert_dd_parity("full node 3 before migration", pruned_node=node3)

        self.stop_node(3)
        migrated_args = [a for a in self.extra_args[3] if a != "-txindex=1"] \
            + ["-prune=550", "-fastprune=1"]
        self.start_node(3, extra_args=migrated_args)
        self.connect_nodes(0, 3)

        # Prune auto-disabled txindex (the stale txindex db on disk is ignored)
        # and the node is in prune mode.
        assert "txindex" not in node3.getindexinfo()
        assert_equal(node3.getblockchaininfo()["pruned"], True)

        # In-place prune of the pre-activation history: something is actually
        # deleted, and the deletion is clamped by the DD lock.
        pruned_to = node3.pruneblockchain(node3.getblockcount())
        assert_greater_than_or_equal(pruned_to, 1)
        assert_greater_than(ACTIVATION_HEIGHT - 10, pruned_to)
        assert_raises_rpc_error(-1, "Block not available (pruned data)",
                                node3.getblock, node3.getblockhash(1))
        node3.getblock(node3.getblockhash(ACTIVATION_HEIGHT))
        self.assert_dd_parity("after full->pruned in-place migration",
                              pruned_node=node3)

        # Explicit stats-index opt-in on the (already pruned) migrated node:
        # the index was synced during the node's full-node life and continues
        # incrementally from retained blocks.
        self.restart_node(3, extra_args=migrated_args + ["-digidollarstatsindex=1"])
        self.connect_nodes(0, 3)
        self.sync_blocks([node0, node3])
        self.assert_dd_parity("migrated pruned node with explicit stats index",
                              pruned_node=node3)
        self.log.info("  migration OK (pruned to %d, floor %d)",
                      pruned_to, ACTIVATION_HEIGHT)

    # ================================================================== F9
    def test_f9_incomplete_dd_window_guard(self):
        """A pruned datadir missing DD-era blocks refuses to start.

        Temporarily delay regtest DigiDollar activation and its retention floor
        beyond this bounded fixture. The registered lock then permits pruning
        blocks required by the original schedule. Restore that schedule and
        require the missing-history startup error. Also pins the
        -prune/-txindex=1 conflict.
        """
        node2 = self.nodes[2]
        self.log.info("F9: pruned datadir missing DD-era blocks refuses to start")

        floor_hash = node2.getblockhash(ACTIVATION_HEIGHT)
        delayed_activation_height = 1_000_000
        prune_batches = 8
        blocks_per_batch = 200
        # Keep the entire mining budget, with a pruning margin, below the
        # temporary activation floor so its lock cannot constrain this fixture.
        assert_greater_than(
            delayed_activation_height,
            node2.getblockcount() + prune_batches * blocks_per_batch + MIN_BLOCKS_TO_KEEP,
        )
        delayed_activation_args = [
            f"-digidollaractivationheight={delayed_activation_height}",
            "-prune=1", "-fastprune=1", "-dandelion=0",
        ]

        # Move both the buried activation height and the static retention floor.
        # Default regtest retains all history through its lock at height zero.
        # A 64 KiB fastprune block file holds many small regtest blocks, and a
        # file is only deleted once EVERY block in it is prunable, so we mine
        # past the original activation height and prune until its file is gone.
        self.stop_node(2)
        self.start_node(2, extra_args=delayed_activation_args)
        deployment = node2.getdigidollardeploymentinfo()
        assert_equal(deployment["activation_height"], delayed_activation_height)
        assert_equal(deployment["enabled"], False)

        def floor_block_pruned():
            try:
                node2.getblock(floor_hash)
                return False
            except Exception:
                return True

        for _ in range(prune_batches):
            if floor_block_pruned():
                break
            self.generate(node2, blocks_per_batch, sync_fun=self.no_op)
            node2.pruneblockchain(node2.getblockcount())
        assert_raises_rpc_error(-1, "Block not available (pruned data)",
                                node2.getblock, floor_hash)
        self.log.info("  DD-era blocks pruned with activation temporarily delayed "
                      "(tip %d)", node2.getblockcount())

        # Under the real activation schedule this datadir is now unusable for
        # DigiDollar validation: startup must refuse incomplete retained data
        # and identify the required history without offering an automatic rebuild.
        self.stop_node(2)
        node2.assert_start_raises_init_error(
            extra_args=self.extra_args[2],
            expected_msg="DigiDollar state not ready: retained block history is incomplete\\. "
                         "Restore the required block and undo files",
            match=ErrorMatch.PARTIAL_REGEX,
        )
        self.log.info("  startup correctly refused with retained-history repair guidance")

        # -prune plus an explicit -txindex=1 is still rejected outright.
        node2.assert_start_raises_init_error(
            extra_args=self.extra_args[2] + ["-txindex=1"],
            expected_msg="Error: Prune mode is incompatible with -txindex.",
        )

        # The error requires restoring history. Exercise explicit reindex
        # recovery from remaining files and the full node, then require parity.
        self.log.info("  recovering the damaged node with -reindex")
        self.start_node(2, extra_args=self.extra_args[2] + ["-reindex"])
        self.connect_nodes(0, 2)
        self.sync_blocks([self.nodes[0], node2], timeout=240)
        self.assert_dd_parity("after -reindex recovery of the damaged node",
                              pruned_node=node2)

    # ================================================================== F13
    def test_f13_unprune_requires_reindex(self):
        """A pruned datadir cannot silently become a full node again: starting
        without -prune must refuse with the standard go-back-to-unpruned error
        (a pruned DigiDollar node can't sneak around its guards by dropping the
        prune flag)."""
        node1 = self.nodes[1]
        self.log.info("F13: dropping -prune on a pruned datadir requires -reindex")

        no_prune_args = [a for a in self.extra_args[1]
                         if a not in ("-prune=1", "-fastprune=1")]
        self.stop_node(1)
        node1.assert_start_raises_init_error(
            extra_args=no_prune_args,
            expected_msg="You need to rebuild the database using -reindex to go "
                         "back to unpruned mode",
            match=ErrorMatch.PARTIAL_REGEX,
        )
        # Restart normally (still pruned) so teardown is clean.
        self.start_node(1, extra_args=self.extra_args[1])

    # ================================================================== F14
    def test_f14_truncated_dd_block_file_fails_closed(self):
        """DD-era block data lost WITHOUT the block index knowing must fail
        CLOSED at startup.

        Whole-file deletion is already caught by the generic block-db check at
        index load (every flagged blk file is opened). The sneaky case is a
        TRUNCATED / partially-restored file: it opens fine, the startup guard's
        index-flag walk passes, but reads of DigiDollar-era blocks fail. The
        startup reconstruction paths (oracle price cache + health metrics that
        feed consensus DCA/ERR) must then refuse to start rather than silently
        rebuild consensus-relevant state from partial data.
        """
        node0, node1 = self.nodes[0], self.nodes[1]
        self.log.info("F14: truncated DD-era block file refuses to start")

        self.stop_node(1)
        blk_files = sorted(
            f for f in os.listdir(node1.blocks_path)
            if f.startswith("blk") and f.endswith(".dat"))
        assert len(blk_files) > 3
        # Truncate everything except the two newest files (keeps the tip region
        # readable for the shallow startup block verification).
        for name in blk_files[:-2]:
            with open(os.path.join(node1.blocks_path, name), "r+b") as f:
                f.truncate(8)

        node1.assert_start_raises_init_error(
            extra_args=self.extra_args[1],
            expected_msg="Oracle price reconstruction could not read block",
            match=ErrorMatch.PARTIAL_REGEX,
        )
        self.log.info("  startup correctly refused on truncated block data")

        # Recovery: -reindex rebuilds from the network, back to full parity.
        self.start_node(1, extra_args=self.extra_args[1] + ["-reindex"])
        self.connect_nodes(0, 1)
        self.sync_blocks([node0, node1], timeout=240)
        self.assert_dd_parity("after -reindex recovery from truncated files")


if __name__ == "__main__":
    DigiDollarPruningTest().main()
