#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A mint must be written to the wallet before it is sent.

mintdigidollar used to send the transaction first and write the owner key and
the position afterwards. If the node stopped in between, a mint that could still
be mined was left with nothing in the wallet to redeem it. The order is now the
other way round: write the owner key and the position, then send. Every write to
the wallet database reports whether it worked. A mint the network refuses has
what it reserved released again, and its owner key stays on disk.

What this test checks:

1. When the mempool refuses the mint, the position and the owner key had already
   been written (they show afterwards as a given-up attempt) and the DGB inputs
   are free again.
2. A restart between minting and confirmation keeps the position and the key.
   The mint then confirms and redeems normally.
3. A mint the wallet does not send (-walletbroadcast=0) keeps its owner key,
   position and token output across unloading and loading the wallet, and
   confirms and redeems after it is sent by hand.
"""

try:
    import sqlite3
except ImportError:
    pass

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

ORACLE_PRICE_MICRO_USD = 500000
MINT_AMOUNT_CENTS = 100000  # $1000.00
BASE_ARGS = ["-digidollar=1", "-txindex=1", "-dandelion=0"]


def record_key(tag, txid):
    """The key this row has in the wallet database file."""
    return bytes([len(tag)]) + tag.encode() + bytes.fromhex(txid)[::-1]


class DigiDollarMintPersistenceTest(DigiByteTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [BASE_ARGS]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()
        self.skip_if_no_py_sqlite3()

    def wallet_db(self):
        return self.nodes[0].wallets_path / self.default_wallet_name / self.wallet_data_filename

    def count_rows(self, key):
        conn = sqlite3.connect(self.wallet_db())
        try:
            return len(conn.execute("SELECT key FROM main WHERE key = ?", (key,)).fetchall())
        finally:
            conn.close()

    def count_prefix(self, prefix):
        conn = sqlite3.connect(self.wallet_db())
        try:
            rows = conn.execute("SELECT key FROM main").fetchall()
        finally:
            conn.close()
        return sum(1 for (key,) in rows if key.startswith(prefix))

    def wallet(self):
        return self.nodes[0].get_wallet_rpc(self.default_wallet_name)

    def position(self, position_id, active_only=False):
        matches = [p for p in self.wallet().listdigidollarpositions(active_only) if p["position_id"] == position_id]
        assert_equal(len(matches), 1)
        return matches[0]

    def history_row(self, txid):
        rows = [t for t in self.wallet().listdigidollartxs(50, 0, "", "mint") if t["txid"] == txid]
        assert_equal(len(rows), 1)
        return rows[0]

    def rows_on_disk(self, txid):
        """Count the wallet database rows for a mint while the wallet is unloaded."""
        node = self.nodes[0]
        node.unloadwallet(self.default_wallet_name)
        try:
            owner_keys = self.count_rows(record_key("ddownerkey", txid))
            positions = self.count_rows(record_key("ddposition", txid))
            dd_outputs = self.count_prefix(bytes([len("ddutxo")]) + b"ddutxo" + bytes.fromhex(txid)[::-1])
        finally:
            node.loadwallet(self.default_wallet_name)
        return owner_keys, positions, dd_outputs

    def restart(self, extra):
        self.restart_node(0, extra_args=BASE_ARGS + extra)
        self.nodes[0].syncwithvalidationinterfacequeue()
        self.nodes[0].setmockoracleprice(ORACLE_PRICE_MICRO_USD)

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 200)
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        self.test_rejected_broadcast_keeps_record_and_releases_inputs()
        self.test_restart_between_mint_and_confirmation()
        self.test_wallet_local_mint_survives_reload()

    def test_rejected_broadcast_keeps_record_and_releases_inputs(self):
        self.log.info("A mint the mempool refuses was written first and frees its inputs")
        # The DigiDollar fee policy is fixed at 0.35 DGB/kB; a 1 DGB/kB relay floor
        # makes the mempool refuse the mint after the wallet has signed it.
        self.restart(["-minrelaytxfee=1", "-incrementalrelayfee=1"])
        wallet = self.wallet()
        balance_before = wallet.getbalance()
        assert_equal(wallet.listdigidollarpositions(False), [])

        assert_raises_rpc_error(-26, "Mint transaction rejected by mempool",
                                wallet.mintdigidollar, MINT_AMOUNT_CENTS, 0)

        # The attempt is on record, written before it was sent, but no longer active.
        positions = wallet.listdigidollarpositions(False)
        assert_equal(len(positions), 1)
        attempt = positions[0]
        assert_equal(attempt["status"], "abandoned_mint")
        assert_equal(attempt["confirmations"], 0)
        assert_equal(attempt["can_redeem"], False)
        assert_equal(wallet.listdigidollarpositions(), [])
        row = self.history_row(attempt["position_id"])
        assert_equal(row["abandoned"], True)
        assert_equal(row["wallet_state"], "abandoned")

        # Its DGB inputs are free again and nothing stays locked.
        assert_equal(wallet.getbalance(), balance_before)
        assert_equal(wallet.listlockunspent(), [])
        assert_equal(wallet.getdigidollarbalance("", 0)["unconfirmed"], 0)

        # The owner key and the position record are on disk, ready if it is needed.
        owner_keys, positions_on_disk, _ = self.rows_on_disk(attempt["position_id"])
        assert_equal(owner_keys, 1)
        assert_equal(positions_on_disk, 1)

    def test_restart_between_mint_and_confirmation(self):
        self.log.info("A restart between mint and confirmation keeps the position and the owner key")
        self.restart([])
        node = self.nodes[0]
        wallet = self.wallet()

        mint = wallet.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        position_id = mint["position_id"]
        assert position_id in node.getrawmempool()
        assert_equal(self.position(position_id)["status"], "pending")

        owner_keys, positions_on_disk, dd_outputs = self.rows_on_disk(position_id)
        assert_equal((owner_keys, positions_on_disk, dd_outputs), (1, 1, 1))

        self.restart([])
        wallet = self.wallet()
        pending = self.position(position_id)
        assert_equal(pending["status"], "pending")
        assert_equal(pending["confirmations"], 0)

        # The node drops a DigiDollar transaction from its mempool while it
        # restarts, because it only accepts one when it has a recent oracle
        # price and the test price is set after the node is up. Send the mint
        # again, the way a wallet rebroadcast would, so a block can include it.
        node.sendrawtransaction(wallet.gettransaction(position_id)["hex"])
        assert position_id in node.getrawmempool()

        self.generate(node, 1)
        active = self.position(position_id)
        assert_equal(active["status"], "active")
        assert_equal(active["confirmations"], 1)

        self.generate(node, mint["unlock_height"] - node.getblockcount() + 1)
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        redeem = wallet.redeemdigidollar(position_id, MINT_AMOUNT_CENTS)
        assert_equal(redeem["position_closed"], True)
        self.generate(node, 1)
        assert_equal(self.position(position_id)["status"], "redeemed")

    def test_wallet_local_mint_survives_reload(self):
        self.log.info("A mint the wallet did not send keeps its records and confirms later")
        self.restart(["-walletbroadcast=0"])
        node = self.nodes[0]
        wallet = self.wallet()

        mint = wallet.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        position_id = mint["position_id"]
        assert position_id not in node.getrawmempool()
        assert_equal(self.position(position_id)["status"], "pending")

        owner_keys, positions_on_disk, dd_outputs = self.rows_on_disk(position_id)
        assert_equal((owner_keys, positions_on_disk, dd_outputs), (1, 1, 1))
        wallet = self.wallet()
        assert_equal(self.position(position_id)["status"], "pending")
        assert_equal(self.history_row(position_id)["wallet_state"], "local")

        raw_mint = wallet.gettransaction(position_id)["hex"]
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        node.sendrawtransaction(raw_mint)
        self.generate(node, 1)
        assert_equal(self.position(position_id)["status"], "active")

        self.restart([])
        node = self.nodes[0]
        wallet = self.wallet()
        self.generate(node, mint["unlock_height"] - node.getblockcount() + 1)
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        redeem = wallet.redeemdigidollar(position_id, MINT_AMOUNT_CENTS)
        assert_equal(redeem["position_closed"], True)
        self.generate(node, 1)
        assert_equal(self.position(position_id)["status"], "redeemed")


if __name__ == "__main__":
    DigiDollarMintPersistenceTest().main()
