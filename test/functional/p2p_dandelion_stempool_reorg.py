#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Dandelion stempool entries must survive a reorg intact.

node0 holds stem-phase transactions in its stempool whose parents are
confirmed in the tip block. Invalidating that block puts the parents back into
node0's mempool and forces node0 to recompute the children's lock points
inside Chainstate::MaybeUpdateMempoolForReorg (the children use BIP68-enabled
inputs, nSequence 0, so their lock points are pinned to the parents' block).

Before the fix that recomputation wrote to the wrong container (the mempool
instead of the stempool): boost::multi_index re-linked the children's index
nodes into the mempool (they leak into getrawmempool) and cross-wired both
pools' red-black trees; on a --enable-debug build the node aborts right there
(-ftrapv catches the garbage arithmetic), and on a release build the next block
connect crashes in the tree rebalance (the same signature as the crash reported
from live nodes, whose own report had no trace). After the fix the parents are the
only new mempool entries, the stem children stay in the stempool, and node0
survives reconnecting the block and confirming the children afterwards.

node0's clock is frozen with mock time so that no stem embargo expires during
the test: embargo expiry is the embargo lock-order path in
CheckDandelionEmbargoes and is exercised by p2p_dandelion_stempool_race.py.
"""

import time

from test_framework.blocktools import COINBASE_MATURITY_2
from test_framework.messages import msg_dandeliontx
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class DandelionStempoolReorgTest(DigiByteTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [
            [
                # node0: Dandelion on (every inbound peer is a Dandelion-inbound
                # peer, so the test peer's dandeliontx messages fill the stempool)
                # and pool consistency checks after every block and transaction.
                "-dandelion=1",
                "-checkmempool=1",
                "-whitelist=noban@127.0.0.1",
            ],
            [
                # node1: plain miner; it receives the same transactions directly.
                "-dandelion=0",
                "-whitelist=noban@127.0.0.1",
            ],
        ]

    def setup_network(self):
        self.setup_nodes()
        # node0 -> node1: node0 receives node1's blocks. node1 runs without
        # Dandelion, so it never asks node0 for stem transactions and entries
        # stay in node0's stempool until their embargo expires.
        self.connect_nodes(0, 1)

    def send_stem_tx(self, tx):
        """Deliver tx to node0's stempool through the Dandelion-inbound peer and
        wait until node0 has accepted it (AcceptToStemPool log line)."""
        txid = tx["txid"]
        with self.node0.wait_for_debug_log([f"accepted {txid} (poolsz".encode()], timeout=60):
            self.peer.send_message(msg_dandeliontx(tx["tx"]))
        # The message handler finishes with this tx (including the fluff
        # decision) before it answers the ping.
        self.peer.sync_with_ping()

    def run_test(self):
        self.node0, self.node1 = self.nodes
        node0, node1 = self.node0, self.node1
        self.peer = node0.add_p2p_connection(P2PInterface())
        self.wallet = MiniWallet(node1)

        # DigiByte regtest applies the 100-block coinbase maturity to every
        # coinbase while MiniWallet only filters with the 8-block constant, so
        # mine the wallet's coins first and then bury them under 100 more blocks.
        self.generate(self.wallet, 20)
        self.generate(node1, COINBASE_MATURITY_2)

        # Freeze node0's clock: embargoes expire only when the test says so.
        node0.setmocktime(int(time.time()))

        # Parents: confirmed in one block on both nodes.
        parents = [self.wallet.create_self_transfer() for _ in range(6)]
        for parent in parents:
            self.wallet.sendrawtransaction(from_node=node1, tx_hex=parent["hex"])
        height_before = node0.getblockcount()
        parents_block = self.generate(node1, 1)[0]
        assert_equal(node0.getbestblockhash(), parents_block)
        assert_equal(node0.getrawmempool(), [])

        # Children: one stem-phase spend of each parent, delivered to node0's
        # stempool only (node1 never sees them).
        children = [self.wallet.create_self_transfer(utxo_to_spend=parent["new_utxo"]) for parent in parents]
        for child in children:
            self.send_stem_tx(child)
        parent_ids = {parent["txid"] for parent in parents}
        child_ids = {child["txid"] for child in children}

        # Dandelion fluffs about 10% of inbound stem transactions straight into
        # the mempool (they also stay in the stempool). Those are legitimately
        # in the mempool already; the others are stempool-only, which is the
        # state this test is about.
        fluffed = set(node0.getrawmempool())
        assert fluffed <= child_ids, f"unexpected mempool entries: {fluffed - child_ids}"
        stem_only = child_ids - fluffed
        assert stem_only, "every stem transaction was fluffed (probability 1e-6); rerun"
        self.log.info(f"{len(stem_only)} stempool-only children, {len(fluffed)} fluffed")

        # Disconnect the parents' block. The parents return to node0's mempool
        # and node0 recomputes the lock points of every stempool child.
        self.log.info("Invalidate the parents' block on node0")
        node0.invalidateblock(parents_block)
        assert_equal(node0.getblockcount(), height_before)
        # The stempool-only children must not have leaked into the mempool.
        assert_equal(set(node0.getrawmempool()), parent_ids | fluffed)

        # Reconnect the block: node0 runs removeForBlock on both pools.
        self.log.info("Reconsider the parents' block on node0")
        node0.reconsiderblock(parents_block)
        assert_equal(node0.getbestblockhash(), parents_block)
        assert_equal(set(node0.getrawmempool()), fluffed)

        # Confirm the children through node1. node0's block connect removes
        # them from the stempool (and the fluffed ones from the mempool) with
        # removeForBlock, the descendant walk where the node used to crash, now running
        # on a stempool that has been through a reorg. node0's clock stays
        # frozen so no embargo expires: expiry takes cs_main after the embargo
        # lock, the embargo lock-order inversion, which is not the subject of
        # this test.
        self.log.info("Confirm the stem children through node1")
        for child in children:
            self.wallet.sendrawtransaction(from_node=node1, tx_hex=child["hex"])
        self.generate(node1, 1)
        assert_equal(node0.getrawmempool(), [])
        self.peer.sync_with_ping()

        # One more block and one more spend of a freshly confirmed child, to
        # show both pools are still usable afterwards.
        grandchild = self.wallet.create_self_transfer(utxo_to_spend=children[0]["new_utxo"])
        self.send_stem_tx(grandchild)
        self.wallet.sendrawtransaction(from_node=node1, tx_hex=grandchild["hex"])
        self.generate(node1, 1)
        assert_equal(node0.getrawmempool(), [])
        self.peer.sync_with_ping()
        self.log.info("Stempool entries stayed in place across the reorg")


if __name__ == '__main__':
    DandelionStempoolReorgTest().main()
