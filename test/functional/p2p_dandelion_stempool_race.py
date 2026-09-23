#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Concurrent writers of the Dandelion stempool.

The stempool writers that run on different threads in production are driven
at the same time: block connect on the message-handler thread (removeForBlock,
where the node used to crash), embargo expiry on the scheduler thread
(removeRecursive and promotion to the mempool), and inbound dandeliontx
acceptance. node0 runs with -checkmempool=1, so the mempool's and the
stempool's consistency checks run after every block and transaction; under a
thread-sanitizer build any access without the pool lock is reported.

Note: embargo expiry used to take cs_main while holding the Dandelion embargo
lock, the opposite order from the message handler. On a tree without that
repair a --enable-debug build aborts here with "detected inconsistent lock
order for 'cs_main' in net_processing.cpp" as soon as the first embargo
expires, and a release build can deadlock. This test therefore expects the
embargo lock-order repair.

The deterministic reorg regression is p2p_dandelion_stempool_reorg.py.
"""

import threading
import time

from test_framework.authproxy import JSONRPCException
from test_framework.blocktools import COINBASE_MATURITY_2
from test_framework.messages import msg_dandeliontx
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    get_rpc_proxy,
)
from test_framework.wallet import MiniWallet

# An embargo lasts 10 s plus a Poisson-distributed extra with a 20 s mean; the
# random draw's tail is capped by the RNG's resolution at roughly 665 s, so
# advancing the clock by this much guarantees that every embargo has expired.
EMBARGO_EXPIRY_SECONDS = 900


class DandelionStempoolRaceTest(DigiByteTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [
            [
                "-dandelion=1",
                "-checkmempool=1",
                "-whitelist=noban@127.0.0.1",
            ],
            [
                "-dandelion=0",
                "-whitelist=noban@127.0.0.1",
            ],
        ]

    def setup_network(self):
        self.setup_nodes()
        # node0 -> node1: node0 receives node1's blocks; node1 never asks for
        # stem transactions, so they stay in node0's stempool until expiry.
        self.connect_nodes(0, 1)

    def send_stem_tx(self, tx, wait_for_accept=True):
        """Deliver tx to node0's stempool through the Dandelion-inbound peer.

        With wait_for_accept, block until node0 has accepted it into the
        stempool (AcceptToStemPool log line). Without it the transaction may
        legitimately be rejected, e.g. a stem child whose parent has just been
        promoted to the mempool; the point is only that node0 processed it."""
        txid = tx["txid"]
        if wait_for_accept:
            with self.node0.wait_for_debug_log([f"accepted {txid} (poolsz".encode()], timeout=60):
                self.peer.send_message(msg_dandeliontx(tx["tx"]))
        else:
            self.peer.send_message(msg_dandeliontx(tx["tx"]))
        self.peer.sync_with_ping()

    def run_test(self):
        self.node0, self.node1 = self.nodes
        node0, node1 = self.node0, self.node1
        self.peer = node0.add_p2p_connection(P2PInterface())
        self.wallet = MiniWallet(node1)

        # Mature coins for MiniWallet under DigiByte's 100-block maturity.
        self.generate(self.wallet, 40)
        self.generate(node1, COINBASE_MATURITY_2)
        start_height = node1.getblockcount()

        # Freeze node0's clock: embargoes expire only when the test says so.
        node0.setmocktime(int(time.time()))

        self.log.info("Block connect, embargo expiry and stem acceptance write the stempool at the same time")

        # Chains of three: parent -> child -> grandchild. Every transaction of
        # every chain goes into node0's stempool. For the even chains node1 also
        # gets the parent and child and mines them, so node0's block connect
        # prunes stem entries that still have stem descendants (the descendant
        # walk in UpdateForRemoveFromMempool where the node used to crash).
        chains = [self.wallet.create_self_transfer_chain(chain_length=3) for _ in range(12)]
        to_mine = [chain[:2] for chain in chains[::2]]
        # Extra stem transactions fed in while blocks and expiries are happening.
        extra = [tx for chain in (self.wallet.create_self_transfer_chain(chain_length=2) for _ in range(6)) for tx in chain]

        for chain in chains:
            for tx in chain:
                self.send_stem_tx(tx)

        miner_done = threading.Event()
        miner_errors = []
        miner_rpc = get_rpc_proxy(node1.url, 1, timeout=600, coveragedir=node1.coverage_dir)
        mining_address = node1.get_deterministic_priv_key().address

        def count_promotions():
            # Only a completed embargo expiry (scheduler thread, after mempool
            # acceptance) writes this line; the fluff path never does.
            with open(node0.debug_log_path, encoding="utf-8") as f:
                return sum(1 for line in f if "CheckDandelionEmbargoes: Successfully moved tx" in line)

        def confirmed_on_node1(txid):
            for height in range(start_height + 1, miner_rpc.getblockcount() + 1):
                if txid in miner_rpc.getblock(miner_rpc.getblockhash(height), 1)["tx"]:
                    return True
            return False

        def wait_for_a_promotion(seen, timeout=10):
            # Wait (bounded) until node0's expiry path has completed at least one
            # more promotion, so that the next block is connected after an expiry
            # and the expiries that follow run after a block: the two writers
            # really interleave instead of running one after the other.
            deadline = time.time() + timeout
            while time.time() < deadline:
                now = count_promotions()
                if now > seen:
                    return now
                time.sleep(0.1)
            return seen

        def miner():
            try:
                promotions_seen = count_promotions()
                for index, txs in enumerate(to_mine):
                    for tx in txs:
                        try:
                            miner_rpc.sendrawtransaction(tx["hex"], 0)
                        except JSONRPCException as e:
                            # node0 may already have promoted and relayed this
                            # transaction, and node1 may already have mined it.
                            # If its outputs were then spent by its own mined
                            # child, resubmitting it reports the inputs as
                            # spent; that is fine as long as it really is in
                            # node1's chain.
                            msg = e.error["message"]
                            if "already in block chain" in msg:
                                continue
                            if "bad-txns-inputs-missingorspent" in msg and confirmed_on_node1(tx["txid"]):
                                continue
                            raise
                    if index > 0:
                        promotions_seen = wait_for_a_promotion(promotions_seen)
                    miner_rpc.generatetoaddress(1, mining_address)
                    time.sleep(0.2)
            except Exception as e:  # noqa: BLE001 - surfaced to the main thread below
                miner_errors.append(e)
            finally:
                miner_done.set()

        # The concurrency this test is about: embargo expiries must complete on
        # node0 while node1's blocks are being connected. So, while the miner
        # thread is still alive, record node0's height each time the promotion
        # count grows; afterwards require that promotions happened at more than
        # one point of the block stream: one before node0's final height (a
        # block was connected after it) and one after the starting height (it
        # ran after a block was connected).
        promotions_before = count_promotions()
        heights_at_promotion = []
        height_at_start = node0.getblockcount()
        miner_thread = threading.Thread(target=miner)
        miner_thread.start()
        try:
            # Main thread: expire embargoes (scheduler-thread work on node0) and
            # keep feeding new stem transactions (message-handler-thread work)
            # while node1's blocks arrive (block connect on node0).
            while not miner_done.is_set():
                node0.bumpmocktime(30)
                if extra:
                    self.send_stem_tx(extra.pop(0), wait_for_accept=False)
                self.peer.sync_with_ping()
                promotions_now = count_promotions()
                if promotions_now > promotions_before and not miner_done.is_set():
                    heights_at_promotion.append(node0.getblockcount())
                    promotions_before = promotions_now
                time.sleep(0.25)
        finally:
            miner_thread.join()
        assert_equal(miner_errors, [])
        height_at_end = node0.getblockcount()
        self.log.info("node0 went from height %d to %d; promotions observed at heights %s",
                      height_at_start, height_at_end, heights_at_promotion)
        assert height_at_end > height_at_start, "node0 connected no blocks during the workload"
        assert heights_at_promotion, "no embargo expiry completed while the miner was running"
        assert min(heights_at_promotion) < height_at_end, "no block was connected after an expiry completed"
        assert max(heights_at_promotion) > height_at_start, "no expiry completed after a block was connected"
        for tx in extra:
            self.send_stem_tx(tx, wait_for_accept=False)

        # Let every remaining embargo expire. The odd chains never went to
        # node1 directly, so the only way one of their parents leaves node0's
        # stempool is the expiry path: promotion into node0's mempool, after
        # which node0 relays it to node1, which may mine it straight away on a
        # fast build. So "promoted" means: in node0's mempool, or in node1's
        # mempool, or already in a block. At least one odd parent must get
        # there (the only other way out is a ~10% fluff). Then connect one
        # more block from node1 and make sure node0 is still alive and
        # answering; the pool consistency checks abort node0 otherwise.
        odd_parent_ids = {chain[0]["txid"] for chain in chains[1::2]}
        node0.bumpmocktime(EMBARGO_EXPIRY_SECONDS)

        def promoted_by_expiry(txid):
            # True only when node0's expiry path moved this transaction out of
            # the stempool: it logs "Successfully moved tx <txid>". A fluffed
            # transaction takes a different path and never logs this line, so
            # ordinary Dandelion fluff cannot satisfy the check.
            with open(node0.debug_log_path, encoding="utf-8") as f:
                return any(f"CheckDandelionEmbargoes: Successfully moved tx {txid}" in line for line in f)

        self.wait_until(lambda: any(promoted_by_expiry(txid) for txid in odd_parent_ids), timeout=60)
        # And every promoted parent is where a promoted transaction belongs:
        # node0's mempool, node1's mempool (relayed), or already in a block.
        def left_the_stempool(txid):
            if txid in node0.getrawmempool() or txid in node1.getrawmempool():
                return True
            for height in range(start_height + 1, node1.getblockcount() + 1):
                if txid in node1.getblock(node1.getblockhash(height), 1)["tx"]:
                    return True
            return False
        for txid in odd_parent_ids:
            if promoted_by_expiry(txid):
                assert left_the_stempool(txid), f"{txid} was promoted by expiry but is nowhere"
        self.sync_blocks()
        # Mempools are not expected to match here: node0 still holds stem-only
        # transactions that node1 never received, so do not wait for a mempool
        # sync. Only the block needs to reach node0.
        self.generate(node1, 1, sync_fun=self.sync_blocks)
        self.peer.sync_with_ping()
        node0.getmempoolinfo()
        self.log.info("node0 survived concurrent stempool writers")


if __name__ == '__main__':
    DandelionStempoolRaceTest().main()
