#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The wallet's Dandelion fallback must hold the stempool lock.

A wallet send on a -dandelion=1 node first goes into the stempool. When the
node has no Dandelion destination (here: no peers at all) BroadcastTransaction
falls back to a normal broadcast: it removes the transaction from the stempool
and submits it to the mempool. Before the fix that removal ran without the
stempool lock, racing block connect and embargo expiry on other threads; on a
DEBUG_LOCKORDER build (--enable-debug) it aborted the node outright
("lock stempool->cs not held in node/transaction.cpp"). After the fix the send
completes and the transaction sits in the mempool.
"""

from test_framework.blocktools import COINBASE_MATURITY_2
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class WalletDandelionFallbackLockTest(DigiByteTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # Dandelion on, no peers: every send takes the fallback path.
        self.extra_args = [["-dandelion=1", "-checkmempool=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, COINBASE_MATURITY_2 + 1, sync_fun=self.no_op)

        self.log.info("Send with no Dandelion destination available (fallback to normal broadcast)")
        txids = [node.sendtoaddress(node.getnewaddress(), 1) for _ in range(3)]
        # The node is still up and every transaction went to the mempool.
        assert_equal(sorted(node.getrawmempool()), sorted(txids))

        self.log.info("Confirm them and send once more")
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.getrawmempool(), [])
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        assert_equal(node.getrawmempool(), [txid])


if __name__ == '__main__':
    WalletDandelionFallbackLockTest().main()
