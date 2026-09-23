#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regression test for the Dandelion embargo lock-order deadlock.

Two threads used to take the same two locks in opposite order:

  * message thread, DANDELIONTX handler (net_processing.cpp): cs_main first,
    then m_dandelion_embargo_mutex inside CConnman::insertDandelionEmbargo();
  * scheduler thread, CheckDandelionEmbargoes(): m_dandelion_embargo_mutex
    first, then cs_main for AcceptToMemoryPool() when an embargo expired.

When both ran at once each thread waited for the other's lock forever: the
node froze, logging stopped and every RPC timed out.

A build configured with --enable-debug defines DEBUG_LOCKORDER. Its checker
aborts the process the moment it has seen both orders in one process, whether
or not they overlapped in time, which makes this test deterministic on such a
build:

  1. An inbound P2P peer (the node treats every inbound peer as a Dandelion
     inbound peer while -dandelion is on) sends several valid dandeliontx
     messages, so the node runs the cs_main -> embargo order once per tx.
  2. Mock time is advanced past the longest possible embargo, so the next
     one-second CheckDandelionEmbargoes tick runs the expiry path.
  3. The node must still answer RPCs, every stem transaction must reach the
     mempool through that path, and the node must announce them to the peer.

On an unfixed DEBUG_LOCKORDER build step 2 aborts digibyted and debug.log
contains "POTENTIAL DEADLOCK DETECTED". On a release build the test still
exercises the whole embargo-expiry path end to end.
"""

import time

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import msg_dandeliontx
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.wallet import MiniWallet

# Number of independent stem transactions to send. Each one is "fluffed"
# (accepted straight into the mempool, skipping the expiry path) with
# probability DANDELION_FLUFF / 100 = 10%, so with 8 the chance that none of
# them exercises the expiry path is 1e-8.
NUM_STEM_TXS = 8

# Mock-time jump that guarantees every embargo has expired. An embargo lasts
# DANDELION_EMBARGO_MINIMUM (10 s) plus PoissonNextSend(DANDELION_EMBARGO_AVG_ADD
# = 20 s). PoissonNextSend draws -log1p(-r) with r < 1 - 2^-48, so its output is
# bounded by ln(2^48) * 20 s ~= 665 s; 900 s clears that and stays below the
# 20-minute P2P inactivity timeout, so the test peer is not disconnected.
EMBARGO_SKIP_SECONDS = 900

# Second jump so the node's inbound inventory trickle timer (Poisson, 5 s
# average, same bounded tail: ~166 s) fires while mock time is frozen.
INV_SKIP_SECONDS = 200


class InvCollector(P2PInterface):
    """Records every inventory hash the node announces to us."""

    def __init__(self):
        super().__init__()
        self.announced = set()

    def on_inv(self, message):
        # Only record the announcement; do not request the items.
        for inv in message.inv:
            self.announced.add(inv.hash)


class DandelionLockOrderTest(DigiByteTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-dandelion=1", "-debug=dandelion", "-debug=mempool"]]

    def node_died_message(self, node):
        """Return a plain-English failure message if the node process has exited, else None."""
        return_code = node.process.poll() if node.process else None
        if return_code is None:
            return None
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
            log = f.read()
        detail = ""
        if "POTENTIAL DEADLOCK DETECTED" in log:
            detail = (" and debug.log contains POTENTIAL DEADLOCK DETECTED: the embargo "
                      "lock-order inversion (embargo mutex held while taking cs_main)")
        return "digibyted exited with code %s while the expired embargo was being processed%s" % (return_code, detail)

    def wait_until_or_report_crash(self, node, predicate, timeout):
        try:
            self.wait_until(predicate, timeout=timeout)
        except Exception:
            died = self.node_died_message(node)
            if died:
                raise AssertionError(died)
            raise

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)

        self.log.info("Mine coins for %d independent stem transactions" % NUM_STEM_TXS)
        # Regtest switches to the 100-block coinbase maturity at height 100
        # (multiAlgoDiffChangeTarget), so keep the chain short and spend the
        # earliest coinbases, which are mature under the 8-block rule.
        self.generate(wallet, NUM_STEM_TXS + COINBASE_MATURITY + 1)
        utxos = sorted(wallet.get_utxos(mark_as_spent=True), key=lambda u: u["height"])[:NUM_STEM_TXS]
        assert_equal(len(utxos), NUM_STEM_TXS)

        self.log.info("Connect an inbound peer; with -dandelion on the node treats it as a Dandelion inbound peer")
        peer = node.add_p2p_connection(InvCollector())

        # Freeze the node's clock at a known base so the embargo expiry times
        # are computed from it and can be jumped past deterministically.
        mock_start = int(time.time())
        node.setmocktime(mock_start)

        self.log.info("Send %d dandeliontx messages (each runs cs_main -> embargo mutex on the message thread)" % NUM_STEM_TXS)
        txs = [wallet.create_self_transfer(utxo_to_spend=utxo) for utxo in utxos]
        txids = [tx["txid"] for tx in txs]
        embargo_logs = ["dandeliontx %s embargoed for" % txid for txid in txids]
        with node.assert_debug_log(expected_msgs=embargo_logs, timeout=30):
            for tx in txs:
                peer.send_and_ping(msg_dandeliontx(tx["tx"]))

        fluffed = [txid for txid in txids if txid in node.getrawmempool()]
        stem_txids = [txid for txid in txids if txid not in fluffed]
        self.log.info("%d fluffed straight into the mempool, %d embargoed in the stempool" % (len(fluffed), len(stem_txids)))
        assert_greater_than(len(stem_txids), 0)

        self.log.info("Jump mock time past every embargo so the scheduler runs the expiry path")
        # Refresh the peer's last-received time first so the jump cannot trip
        # the inactivity timeout.
        peer.sync_with_ping()
        moved_logs = ["CheckDandelionEmbargoes: Successfully moved tx %s to mempool" % txid for txid in stem_txids]
        with node.assert_debug_log(expected_msgs=moved_logs, timeout=60):
            node.setmocktime(mock_start + EMBARGO_SKIP_SECONDS)
            self.log.info("Node must stay alive and move every stem transaction into the mempool")
            self.wait_until_or_report_crash(node, lambda: set(txids) <= set(node.getrawmempool()), timeout=60)

        self.log.info("Node must announce the released transactions to the peer (fluff phase)")
        peer.sync_with_ping()
        node.setmocktime(mock_start + EMBARGO_SKIP_SECONDS + INV_SKIP_SECONDS)
        wanted = [(int(tx["txid"], 16), tx["tx"].calc_sha256(True)) for tx in txs]

        def all_announced():
            with p2p_lock:
                return all(txid in peer.announced or wtxid in peer.announced for txid, wtxid in wanted)

        self.wait_until_or_report_crash(node, all_announced, timeout=60)

        self.log.info("Embargo bookkeeping is clear: nothing left to release, node still responsive")
        assert_equal(node.getmempoolinfo()["size"], NUM_STEM_TXS)
        peer.sync_with_ping()


if __name__ == "__main__":
    DandelionLockOrderTest().main()
