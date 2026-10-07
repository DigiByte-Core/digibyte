#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Regression test for DD-RH-121: if a reorg disconnects both a DD mint and a
descendant DD transfer, the transfer must not resurrect through stale txindex
data while its DD input is only available from the mempool parent. Once the
same mint confirms again, normal wallet rebroadcast must recover the transfer.
"""

import time

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class WalletDigiDollarTransferAncestorReorgTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txindex=1", "-dandelion=0", "-persistmempool=1"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mining spendable funds")
        node.generate(200)
        # Match the regtest fallback/P2P oracle price used during disconnect
        # mempool re-add, so the disconnected mint itself remains policy-valid.
        node.setmockoracleprice(6500)

        self.log.info("Minting and confirming DigiDollar")
        mint = node.mintdigidollar(100, 0)
        mint_txid = mint["txid"]
        mint_hex = node.getrawtransaction(mint_txid)
        mint_block = node.generate(1)[0]
        node.syncwithvalidationinterfacequeue()

        self.log.info("Confirming a transfer descendant of the minted DD output")
        transfer = node.senddigidollar(node.getdigidollaraddress(), 100)
        transfer_txid = transfer["txid"]
        transfer_hex = node.getrawtransaction(transfer_txid)
        transfer_block = node.generate(1)[0]
        assert transfer_txid in node.getblock(transfer_block)["tx"]
        node.syncwithvalidationinterfacequeue()

        mocktime = int(time.time())
        for restart in (False, True):
            self.log.info("Invalidating the mint ancestor block (restart=%s)", restart)
            node.invalidateblock(mint_block)
            node.syncwithvalidationinterfacequeue()

            assert transfer_txid not in node.getrawmempool(), (
                "DD transfer descendant must not return to mempool while its DD input "
                "comes from an unconfirmed mint"
            )
            assert node.getdigidollarbalance()["total"] == 0, (
                "Wallet must not keep phantom DD balance from a temporarily re-added "
                "descendant whose confirmed ancestor was disconnected"
            )

            if restart:
                self.log.info("Restarting with mempool persistence must not reload the descendant")
                self.restart_node(0, extra_args=self.extra_args[0])
                node = self.nodes[0]
                node.syncwithvalidationinterfacequeue()
                assert transfer_txid not in node.getrawmempool(), "Rejected DD descendant must not survive restart"
                assert node.getdigidollarbalance()["total"] == 0

            self.log.info("Confirming the same mint again on the replacement branch")
            # Wallet rebroadcast only considers transactions received at least
            # five minutes before its latest block notification.
            mocktime += 6 * 60
            node.setmocktime(mocktime)
            node.setmockoracleprice(6500)
            # Restart clears the mock oracle, so startup can reject the mint
            # from its saved mempool. Mine the original bytes to model another
            # miner confirming it, independently of startup quote availability.
            mint_block = self.generateblock(node, node.getnewaddress(), [mint_hex])["hash"]
            assert mint_txid in node.getblock(mint_block)["tx"]
            node.syncwithvalidationinterfacequeue()
            assert transfer_txid not in node.getrawmempool()
            assert_equal(node.testmempoolaccept([transfer_hex])[0]["allowed"], True)
            assert_equal(node.getdigidollarbalance("", 0)["total"], 0)

            self.log.info("Normal wallet rebroadcast recovers the transfer after its mint confirms")
            # Drive the existing 12-36 hour wallet rebroadcast schedule. Do not
            # submit the transfer manually or permit unconfirmed DD inputs.
            mocktime += 36 * 60 * 60
            node.setmocktime(mocktime)
            node.mockscheduler(60)
            self.wait_until(lambda: transfer_txid in node.getrawmempool())
            node.syncwithvalidationinterfacequeue()
            assert_equal(node.getdigidollarbalance("", 0)["unconfirmed"], 100)
            assert_equal(node.getdigidollarbalance()["confirmed"], 0)

            recovered_block = node.generate(1)[0]
            assert transfer_txid in node.getblock(recovered_block)["tx"]
            node.syncwithvalidationinterfacequeue()
            assert_equal(node.getdigidollarbalance()["confirmed"], 100)
            assert_equal(node.getdigidollarbalance()["unconfirmed"], 0)


if __name__ == "__main__":
    WalletDigiDollarTransferAncestorReorgTest().main()
