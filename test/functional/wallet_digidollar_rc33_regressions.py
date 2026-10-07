#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Regression coverage for DigiDollar wallet bugs reported during RC33.

The scenarios here target the wallet state problems that isolated txbuilder
tests do not prove: fragmented collateral mints, rapid consecutive mints, and
rapid consecutive redemptions across mining and restart.
"""

from decimal import Decimal
from pathlib import Path
import threading
import urllib.parse

from test_framework.test_framework import DigiByteTestFramework
from test_framework.messages import tx_from_hex
from test_framework.util import assert_equal, get_rpc_proxy


ORACLE_PRICE_MICRO_USD = 100_000_000  # $100/DGB keeps the test small.


def position_active(position):
    return position.get("is_active", position.get("status") in ("active", "unlocked"))


class WalletDigiDollarRC33RegressionsTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0", "-rpcthreads=8"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 160)
        result = node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        assert_equal(result["price_micro_usd"], ORACLE_PRICE_MICRO_USD)
        self.default_wallet_name = node.listwallets()[0]

        self.test_redeem_rpc_uses_wallet_owned_relay()
        self.test_mint_dgb_change_confirms_and_spends()
        self.test_fragmented_large_mint_consolidates_and_confirms()
        self.test_repeated_mints_spend_unconfirmed_change()
        self.test_fragmented_multi_pass_mint_consolidates_and_confirms()
        self.test_rapid_mints_confirm_after_restart()
        self.test_concurrent_rapid_mints_have_unique_inputs()
        self.test_rapid_dd_sends_confirm_without_redeeming_vaults()
        self.test_rapid_redeems_confirm_after_restart()

    def create_descriptor_wallet(self, name):
        self.nodes[0].createwallet(wallet_name=name, descriptors=True)
        return self.nodes[0].get_wallet_rpc(name)

    def get_loaded_wallet(self, name):
        node = self.nodes[0]
        if name not in node.listwallets():
            node.loadwallet(name)
        return node.get_wallet_rpc(name)

    def funder_wallet(self):
        return self.get_loaded_wallet(self.default_wallet_name)

    def assert_confirmed_clean(self, wallet, txid):
        tx = wallet.gettransaction(txid)
        assert tx["confirmations"] > 0
        assert_equal(tx.get("abandoned", False), False)
        assert_equal(tx.get("walletconflicts", []), [])
        return tx

    def test_redeem_rpc_uses_wallet_owned_relay(self):
        self.log.info("Testing redeem RPC uses wallet-owned relay path")
        source_path = Path(__file__).resolve().parents[2] / "src" / "rpc" / "digidollar.cpp"
        source = source_path.read_text(encoding="utf8")
        redeem_start = source.index("RPCHelpMan redeemdigidollar()")
        redeem_end = source.index("RPCHelpMan listdigidollarpositions()", redeem_start)
        redeem_body = source[redeem_start:redeem_end]

        assert "broadcastTransaction" not in redeem_body
        assert "CommitTransaction(redeemTx, {}, {}, &commit_error)" in redeem_body

    def mint_after_merges_confirm(self, wallet):
        node = self.nodes[0]
        pending = wallet.mintdigidollar(100000, 0)
        assert_equal(pending["status"], "consolidation_pending")
        assert "txid" not in pending
        assert "position_id" not in pending
        assert_equal(wallet.listdigidollarpositions(False), [])
        merge_txids = pending["consolidation_txids"]
        assert merge_txids
        for txid in merge_txids:
            assert txid in node.getrawmempool()

        self.generate(node, 1)
        for txid in merge_txids:
            self.assert_confirmed_clean(wallet, txid)
        mint = wallet.mintdigidollar(100000, 0)
        assert mint["txid"] in node.getrawmempool()
        return mint, merge_txids

    def test_fragmented_large_mint_consolidates_and_confirms(self):
        self.log.info("Testing fragmented large mint auto-consolidation")
        node = self.nodes[0]
        fragmented = self.create_descriptor_wallet("rc33_fragmented")

        outputs = {
            fragmented.getnewaddress(): Decimal("0.25")
            for _ in range(450)
        }
        funding_txid = self.funder_wallet().sendmany("", outputs)
        self.generate(node, 1)
        assert_equal(self.funder_wallet().gettransaction(funding_txid)["confirmations"], 1)

        assert_equal(len(fragmented.listunspent(1)), 450)

        mint, merge_txids = self.mint_after_merges_confirm(fragmented)

        self.generate(node, 1)
        self.restart_node(0, extra_args=["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"])
        node = self.nodes[0]
        fragmented = self.get_loaded_wallet("rc33_fragmented")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        assert_got_mint = fragmented.gettransaction(mint["txid"])
        assert_got_mint["confirmations"] > 0
        for txid in merge_txids:
            self.assert_confirmed_clean(fragmented, txid)

        positions = fragmented.listdigidollarpositions(False)
        matching = [p for p in positions if p["position_id"] == mint["position_id"]]
        assert_equal(len(matching), 1)
        assert_equal(matching[0]["confirmations"] > 0, True)
        assert_equal(position_active(matching[0]), True)

    def test_fragmented_multi_pass_mint_consolidates_and_confirms(self):
        self.log.info("Testing multi-pass fragmented mint auto-consolidation")
        node = self.nodes[0]
        fragmented = self.create_descriptor_wallet("rc41_fragmented_multi_pass")

        outputs = {
            fragmented.getnewaddress(): Decimal("0.08")
            for _ in range(2000)
        }
        funding_txid = self.funder_wallet().sendmany("", outputs)
        self.generate(node, 1)
        assert_equal(self.funder_wallet().gettransaction(funding_txid)["confirmations"], 1)

        assert_equal(len(fragmented.listunspent(1)), 2000)

        mint, merge_txids = self.mint_after_merges_confirm(fragmented)

        self.generate(node, 1)
        self.restart_node(0, extra_args=["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"])
        node = self.nodes[0]
        fragmented = self.get_loaded_wallet("rc41_fragmented_multi_pass")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        assert_got_mint = fragmented.gettransaction(mint["txid"])
        assert_got_mint["confirmations"] > 0
        for txid in merge_txids:
            self.assert_confirmed_clean(fragmented, txid)

        positions = fragmented.listdigidollarpositions(False)
        matching = [p for p in positions if p["position_id"] == mint["position_id"]]
        assert_equal(len(matching), 1)
        assert_equal(matching[0]["confirmations"] > 0, True)
        assert_equal(position_active(matching[0]), True)

    def test_mint_dgb_change_confirms_and_spends(self):
        self.log.info("Testing mint DGB change remains visible and spendable")
        node = self.nodes[0]
        change_wallet = self.create_descriptor_wallet("rc41_mint_change")

        funding_txid = self.funder_wallet().sendtoaddress(change_wallet.getnewaddress(), Decimal("25.00"))
        self.generate(node, 1)
        assert_equal(self.funder_wallet().gettransaction(funding_txid)["confirmations"], 1)
        assert_equal(len(change_wallet.listunspent(1)), 1)

        mint = change_wallet.mintdigidollar(10000, 0)
        self.generate(node, 1)

        change_outputs = [
            utxo for utxo in change_wallet.listunspent(1)
            if utxo["txid"] == mint["txid"] and utxo["vout"] >= 3 and utxo["amount"] > 0
        ]
        assert_equal(len(change_outputs), 1)

        spend_txid = change_wallet.sendtoaddress(self.funder_wallet().getnewaddress(), Decimal("1.00"))
        assert spend_txid in node.getrawmempool()
        self.generate(node, 1)
        assert change_wallet.gettransaction(spend_txid)["confirmations"] > 0

    def test_repeated_mints_spend_unconfirmed_change(self):
        self.log.info("Testing repeated mints from one large UTXO do not corrupt wallet mempool state")
        node = self.nodes[0]
        rapid = self.create_descriptor_wallet("rc41_repeated_mint")

        funding_txid = self.funder_wallet().sendtoaddress(rapid.getnewaddress(), Decimal("80.00"))
        self.generate(node, 1)
        assert_equal(self.funder_wallet().gettransaction(funding_txid)["confirmations"], 1)
        assert_equal(len(rapid.listunspent(1)), 1)

        mints = []
        for _ in range(5):
            mint = rapid.mintdigidollar(10000, 0)
            mints.append(mint)
            assert mint["txid"] in node.getrawmempool()

        self.generate(node, 1)
        self.restart_node(0, extra_args=["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"])
        node = self.nodes[0]
        rapid = self.get_loaded_wallet("rc41_repeated_mint")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        positions = rapid.listdigidollarpositions(False)
        position_ids = {p["position_id"]: p for p in positions}
        for mint in mints:
            tx = rapid.gettransaction(mint["txid"])
            assert tx["confirmations"] > 0
            assert mint["position_id"] in position_ids
            assert position_ids[mint["position_id"]]["confirmations"] > 0
            assert_equal(position_active(position_ids[mint["position_id"]]), True)

    def test_rapid_mints_confirm_after_restart(self):
        self.log.info("Testing 22 rapid mints do not remain permanently pending")
        node = self.nodes[0]
        rapid = self.create_descriptor_wallet("rc33_rapid_mint")

        outputs = {
            rapid.getnewaddress(): Decimal("12.00")
            for _ in range(30)
        }
        self.funder_wallet().sendmany("", outputs)
        self.generate(node, 1)

        mints = []
        for _ in range(22):
            mint = rapid.mintdigidollar(10000, 0)
            mints.append(mint)
            assert mint["txid"] in node.getrawmempool()

        self.generate(node, 1)
        self.restart_node(0, extra_args=["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"])
        node = self.nodes[0]
        rapid = self.get_loaded_wallet("rc33_rapid_mint")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        positions = rapid.listdigidollarpositions(False)
        position_ids = {p["position_id"]: p for p in positions}
        for mint in mints:
            self.assert_confirmed_clean(rapid, mint["txid"])
            assert mint["position_id"] in position_ids
            assert position_ids[mint["position_id"]]["confirmations"] > 0
            assert_equal(position_active(position_ids[mint["position_id"]]), True)

    def test_concurrent_rapid_mints_have_unique_inputs(self):
        self.log.info("Testing 20 concurrent 100 DD mints do not create wallet conflicts")
        node = self.nodes[0]
        rapid = self.create_descriptor_wallet("rc42_concurrent_mint")

        outputs = {
            rapid.getnewaddress(): Decimal("12.00")
            for _ in range(24)
        }
        self.funder_wallet().sendmany("", outputs)
        self.generate(node, 1)
        assert_equal(len(rapid.listunspent(1)), 24)

        results = []
        errors = []
        lock = threading.Lock()
        rapid_url = f"{node.url}/wallet/{urllib.parse.quote('rc42_concurrent_mint')}"

        def mint_worker():
            try:
                wallet_rpc = get_rpc_proxy(rapid_url, node.index, timeout=60, coveragedir=self.options.coveragedir)
                result = wallet_rpc.mintdigidollar(10000, 0)
                with lock:
                    results.append(result)
            except Exception as e:
                with lock:
                    errors.append(str(e))

        threads = [threading.Thread(target=mint_worker) for _ in range(20)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        assert_equal(errors, [])
        assert_equal(len(results), 20)
        txids = [mint["txid"] for mint in results]
        assert_equal(len(set(txids)), 20)

        spent_inputs = set()
        for txid in txids:
            raw = tx_from_hex(node.getrawtransaction(txid))
            for vin in raw.vin:
                prevout = (f"{vin.prevout.hash:064x}", vin.prevout.n)
                assert prevout not in spent_inputs
                spent_inputs.add(prevout)
            assert txid in node.getrawmempool()

        self.generate(node, 1)
        self.restart_node(0, extra_args=["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0", "-rpcthreads=8"])
        node = self.nodes[0]
        rapid = self.get_loaded_wallet("rc42_concurrent_mint")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        positions = rapid.listdigidollarpositions(False)
        position_ids = {p["position_id"]: p for p in positions}
        for mint in results:
            self.assert_confirmed_clean(rapid, mint["txid"])
            assert mint["position_id"] in position_ids
            assert_equal(position_active(position_ids[mint["position_id"]]), True)

    def test_rapid_dd_sends_confirm_without_redeeming_vaults(self):
        self.log.info("Testing 20 rapid 5 DD sends confirm and leave vaults active")
        node = self.nodes[0]
        sender = self.create_descriptor_wallet("rc42_rapid_send_sender")
        receiver = self.create_descriptor_wallet("rc42_rapid_send_receiver")

        outputs = {
            sender.getnewaddress(): Decimal("12.00")
            for _ in range(24)
        }
        self.funder_wallet().sendmany("", outputs)
        self.generate(node, 1)

        mints = []
        for _ in range(20):
            mint = sender.mintdigidollar(10000, 0)
            mints.append(mint)
            assert mint["txid"] in node.getrawmempool()
        self.generate(node, 1)

        sends = []
        for _ in range(20):
            send = sender.senddigidollar(receiver.getdigidollaraddress(), 500)
            sends.append(send)
            assert send["txid"] in node.getrawmempool()

        positions_before_confirm = sender.listdigidollarpositions(False)
        minted_ids = {mint["position_id"] for mint in mints}
        for position in positions_before_confirm:
            if position["position_id"] in minted_ids:
                assert position["status"] != "redeemed"

        self.generate(node, 1)
        self.restart_node(0, extra_args=["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0", "-rpcthreads=8"])
        node = self.nodes[0]
        sender = self.get_loaded_wallet("rc42_rapid_send_sender")
        receiver = self.get_loaded_wallet("rc42_rapid_send_receiver")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        for send in sends:
            self.assert_confirmed_clean(sender, send["txid"])

        positions = sender.listdigidollarpositions(False)
        position_ids = {p["position_id"]: p for p in positions}
        for mint in mints:
            assert mint["position_id"] in position_ids
            assert position_ids[mint["position_id"]]["status"] != "redeemed"

        assert_equal(receiver.getdigidollarbalance()["total"], 10000)

    def test_rapid_redeems_confirm_after_restart(self):
        self.log.info("Testing 20 rapid redemptions do not report success then disappear")
        node = self.nodes[0]
        redeem_wallet = self.create_descriptor_wallet("rc33_rapid_redeem")

        outputs = {
            redeem_wallet.getnewaddress(): Decimal("12.00")
            for _ in range(24)
        }
        self.funder_wallet().sendmany("", outputs)
        self.generate(node, 1)

        mints = []
        for _ in range(20):
            mints.append(redeem_wallet.mintdigidollar(10000, 0))
        self.generate(node, 1)

        positions = redeem_wallet.listdigidollarpositions(False)
        unlock_height = max(p["unlock_height"] for p in positions if p["position_id"] in {m["position_id"] for m in mints})
        blocks_needed = max(0, unlock_height - node.getblockcount())
        if blocks_needed:
            self.generate(node, blocks_needed)
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        redeems = []
        for mint in mints:
            redeem = redeem_wallet.redeemdigidollar(mint["position_id"], 10000)
            redeems.append(redeem)
            assert redeem["txid"] in node.getrawmempool()

        self.generate(node, 1)
        self.restart_node(0, extra_args=["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"])
        node = self.nodes[0]
        redeem_wallet = self.get_loaded_wallet("rc33_rapid_redeem")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        for redeem in redeems:
            self.assert_confirmed_clean(redeem_wallet, redeem["txid"])

        positions = redeem_wallet.listdigidollarpositions(False)
        remaining = [p for p in positions if p["position_id"] in {m["position_id"] for m in mints} and position_active(p)]
        assert_equal(remaining, [])


if __name__ == "__main__":
    WalletDigiDollarRC33RegressionsTest().main()
