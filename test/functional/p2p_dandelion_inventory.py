#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bound exact Dandelion inventory history received through the P2P interface."""

from test_framework.messages import CInv, MSG_DANDELION, msg_getdata, msg_inv
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


INVENTORY_LIMIT = 50_000


class DandelionInventoryTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.wallet_names = []
        self.extra_args = [["-dandelion=0", "-debug=dandelion"]]

    def check_known(self, tx_hash, known):
        node = self.nodes[0]
        marker = f"Dandelion transaction {tx_hash:064x} not found for peer=0 (have_tx=false, is_known={'true' if known else 'false'})"
        with node.assert_debug_log([marker]):
            self.peer.send_and_ping(msg_getdata([CInv(MSG_DANDELION, tx_hash)]))
        assert_equal(self.peer.last_message["notfound"].vec[0].hash, tx_hash)

    def run_test(self):
        node = self.nodes[0]
        assert node.getblockchaininfo()["initialblockdownload"]
        self.peer = node.add_p2p_connection(P2PInterface())

        self.log.info("Track incoming inventory even during IBD with Dandelion disabled")
        self.peer.send_and_ping(msg_inv([CInv(MSG_DANDELION, 1)]))
        self.check_known(1, True)

        self.log.info("Retain only the most recent distinct hashes across INV messages")
        hashes = range(2, INVENTORY_LIMIT + 2)
        self.peer.send_and_ping(msg_inv([CInv(MSG_DANDELION, value) for value in hashes]))
        self.check_known(1, False)
        self.check_known(2, True)
        self.check_known(INVENTORY_LIMIT + 1, True)
        self.check_known(INVENTORY_LIMIT + 2, False)

        self.log.info("Repeated announcements must not evict distinct entries")
        self.peer.send_and_ping(msg_inv([CInv(MSG_DANDELION, INVENTORY_LIMIT + 1)] * INVENTORY_LIMIT))
        self.check_known(2, True)

        self.log.info("An evicted hash can be learned again without authorizing its neighbors")
        self.peer.send_and_ping(msg_inv([CInv(MSG_DANDELION, 1)]))
        self.check_known(1, True)
        self.check_known(2, False)
        self.check_known(3, True)
        self.check_known(INVENTORY_LIMIT + 2, False)


if __name__ == "__main__":
    DandelionInventoryTest().main()
