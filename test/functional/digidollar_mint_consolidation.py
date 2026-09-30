#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fragmented mint funds must confirm before a mint spends the merged coins."""

from test_framework.messages import CTransaction, CTxOut, COIN
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarMintConsolidationTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollar=1", "-dandelion=0", "-txindex=1", "-rpcdoccheck=1"]]

    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 200)
        node.setmockoracleprice(500000)
        miner = node.get_wallet_rpc(self.default_wallet_name)
        mining_address = miner.getnewaddress()
        node.createwallet("fragmented", load_on_startup=True)
        wallet = node.get_wallet_rpc("fragmented")
        address = wallet.getnewaddress("", "bech32")
        # More than 1,400 inputs are needed for $100 at the one-hour tier.
        # Their combined size exceeds the unchanged 101 kB ancestor limit.
        funding = CTransaction()
        script = bytes.fromhex(wallet.getaddressinfo(address)["scriptPubKey"])
        funding.vout = [CTxOut(12 * COIN // 10, script) for _ in range(2000)]
        for _ in range(2):
            funded = miner.fundrawtransaction(funding.serialize().hex())
            signed = miner.signrawtransactionwithwallet(funded["hex"])
            node.sendrawtransaction(signed["hex"])
            self.generatetoaddress(node, 1, mining_address)
        assert_equal(len(wallet.listunspent()), 4000)

        self.log.info("Failures before any merge preserve existing RPC error codes")
        self.restart_node(0, extra_args=self.extra_args[0] + ["-walletbroadcast=0"])
        node = self.nodes[0]
        miner = node.get_wallet_rpc(self.default_wallet_name)
        wallet = node.get_wallet_rpc("fragmented")
        node.setmockoracleprice(500000)
        assert_raises_rpc_error(-4, "broadcast", wallet.mintdigidollar, 10000, 0)
        assert_equal(node.getrawmempool(), [])
        self.restart_node(0)
        node = self.nodes[0]
        miner = node.get_wallet_rpc(self.default_wallet_name)
        wallet = node.get_wallet_rpc("fragmented")
        node.setmockoracleprice(500000)
        coins = wallet.listunspent(1)
        wallet.lockunspent(False, [{"txid": coin["txid"], "vout": coin["vout"]} for coin in coins[450:]])
        assert_raises_rpc_error(-6, "Insufficient confirmed funds", wallet.mintdigidollar, 10000, 0)
        assert_equal(node.getrawmempool(), [])
        wallet.lockunspent(True)

        pending = wallet.mintdigidollar(10000, 0)
        assert_equal(pending["status"], "consolidation_pending")
        assert "Wait for confirmation" in pending["message"]
        assert "txid" not in pending
        assert "position_id" not in pending
        assert "dd_minted" not in pending
        txids = pending["consolidation_txids"]
        assert_equal(len(txids), 2)
        assert_equal(set(node.getrawmempool()), set(txids))
        for txid in txids:
            transaction = wallet.gettransaction(txid)
            assert_equal(transaction["confirmations"], 0)
            assert "digidollar_mint_consolidation" not in transaction
        for transaction in wallet.listtransactions(count=10):
            assert "digidollar_mint_consolidation" not in transaction
        assert_equal(wallet.listdigidollarpositions(False), [])
        assert_equal(wallet.listdigidollartxs(), [])
        merged = [node.getrawtransaction(txid, True) for txid in txids]
        assert sum(tx["vsize"] for tx in merged) > 101000
        assert sum(len(tx["vin"]) for tx in merged) < 2200
        assert all("digidollar" not in tx for tx in merged)

        # Enough confirmed coins remain for another mint. Repeated requests
        # must still show the pending merges, without paying more fees.
        assert sum(coin["amount"] for coin in wallet.listunspent(1)) > 2000
        retry = wallet.mintdigidollar(10000, 0)
        assert_equal(retry["status"], "consolidation_pending")
        assert_equal(set(retry["consolidation_txids"]), set(txids))
        assert_equal(set(node.getrawmempool()), set(txids))
        assert_equal(wallet.listdigidollarpositions(False), [])
        self.restart_node(0)
        node = self.nodes[0]
        miner = node.get_wallet_rpc(self.default_wallet_name)
        wallet = node.get_wallet_rpc("fragmented")
        node.setmockoracleprice(500000)
        retry = wallet.mintdigidollar(10000, 0)
        assert_equal(retry["status"], "consolidation_pending")
        assert_equal(set(retry["consolidation_txids"]), set(txids))
        assert_equal(set(node.getrawmempool()), set(txids))

        self.generatetoaddress(node, 1, mining_address)
        for txid in txids:
            assert_equal(wallet.gettransaction(txid)["confirmations"], 1)
        minted = wallet.mintdigidollar(10000, 0)
        assert_equal(minted["dd_minted"], 10000)
        assert_equal(set(node.getrawmempool()), {minted["txid"]})
        self.generatetoaddress(node, 1, mining_address)
        assert_equal(wallet.gettransaction(minted["txid"])["confirmations"], 1)
        assert_equal(len(wallet.listdigidollarpositions()), 1)

        self.log.info("Ordinary trusted unconfirmed change remains valid RPC mint funding")
        node.createwallet("ordinary")
        ordinary = node.get_wallet_rpc("ordinary")
        miner.sendtoaddress(ordinary.getnewaddress(), 3000)
        self.generatetoaddress(node, 1, mining_address)
        parent = ordinary.sendtoaddress(mining_address, 1)
        normal = ordinary.mintdigidollar(10000, 0)
        assert "status" not in normal
        mint_tx = node.getrawtransaction(normal["txid"], True)
        assert any(txin["txid"] == parent for txin in mint_tx["vin"])
        self.generatetoaddress(node, 1, mining_address)

        self.log.info("A later failed merge still reports every earlier transaction")
        node.createwallet("partial")
        partial = node.get_wallet_rpc("partial")
        witness = bytes.fromhex(partial.getaddressinfo(partial.getnewaddress("", "bech32"))["scriptPubKey"])
        legacy = bytes.fromhex(partial.getaddressinfo(partial.getnewaddress("", "legacy"))["scriptPubKey"])
        funding = CTransaction()
        # The first batch fits. The larger legacy inputs make the second
        # batch exceed transaction weight without changing any policy limit.
        funding.vout = [CTxOut(13 * COIN // 10, witness) for _ in range(1400)]
        # 3,070 DGB also covers the mint builder's 1% collateral margin.
        funding.vout += [CTxOut(5 * COIN // 4, legacy) for _ in range(1000)]
        funded = miner.fundrawtransaction(funding.serialize().hex())
        node.sendrawtransaction(miner.signrawtransactionwithwallet(funded["hex"])["hex"])
        self.generatetoaddress(node, 1, mining_address)
        pending = partial.mintdigidollar(15000, 0)
        assert_equal(pending["status"], "consolidation_pending")
        assert_equal(len(pending["consolidation_txids"]), 1)
        assert "error" in pending
        assert_equal(set(node.getrawmempool()), set(pending["consolidation_txids"]))
        assert_equal(partial.listdigidollarpositions(False), [])
        assert_equal(partial.listdigidollartxs(), [])


if __name__ == '__main__':
    DigiDollarMintConsolidationTest().main()
