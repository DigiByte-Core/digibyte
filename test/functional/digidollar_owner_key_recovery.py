#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Redeeming must find a missing owner key again among the wallet's own keys.

The owner key of a mint comes from the wallet's own chain of keys, so it is not
really lost while the wallet still has its private keys. The wallet also writes
that key down against the position ("ddownerkey", or "ddcownerkey" when the
wallet is encrypted) so redeeming can find it straight away. When that row is
missing, because a write failed or a restore skipped it, redeeming used to fail
with "Owner key not found for position" and the collateral looked lost.

This test deletes that row straight out of the wallet file and checks that:

1. redeemdigidollar finds the matching key again, writes it down and redeems;
2. a wallet with no private keys is refused, and says so;
3. a locked wallet is refused, and asks for the passphrase;
4. an encrypted wallet finds the key again while unlocked and stores it encrypted;
5. a wallet that has the position but none of its keys gets its own message
   telling the user to restore the right wallet and rescan.
"""

try:
    import sqlite3
except ImportError:
    pass

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

ORACLE_PRICE_MICRO_USD = 500000
MINT_AMOUNT_CENTS = 100000  # $1000.00
PASSPHRASE = "owner-key-recovery"


def record_key(tag, txid):
    """The key this row has in the wallet database file."""
    return bytes([len(tag)]) + tag.encode() + bytes.fromhex(txid)[::-1]


class DigiDollarOwnerKeyRecoveryTest(DigiByteTestFramework):
    def add_options(self, parser):
        # Minting needs a descriptor wallet with an HD chain.
        self.add_wallet_options(parser, descriptors=True, legacy=False)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollar=1", "-txindex=1", "-dandelion=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()
        self.skip_if_no_py_sqlite3()

    # ------------------------------------------------------------------
    # Reading and editing the wallet file. The wallet must be unloaded first.
    # ------------------------------------------------------------------
    def wallet_db(self, name):
        return self.nodes[0].wallets_path / name / self.wallet_data_filename

    def db_rows(self, name, key):
        conn = sqlite3.connect(self.wallet_db(name))
        try:
            return conn.execute("SELECT key, value FROM main WHERE key = ?", (key,)).fetchall()
        finally:
            conn.close()

    def delete_row(self, name, key):
        conn = sqlite3.connect(self.wallet_db(name))
        try:
            with conn:
                deleted = conn.execute("DELETE FROM main WHERE key = ?", (key,)).rowcount
        finally:
            conn.close()
        return deleted

    def copy_row(self, src, dst, key):
        rows = self.db_rows(src, key)
        assert_equal(len(rows), 1)
        conn = sqlite3.connect(self.wallet_db(dst))
        try:
            with conn:
                conn.execute("INSERT OR REPLACE INTO main VALUES (?, ?)", rows[0])
        finally:
            conn.close()

    def with_node_stopped(self, edit):
        """Stop the node cleanly, edit wallet files, start it again.

        A clean stop flushes the chain state, so the wallets record the tip as
        their best block and no rescan runs on the next start. That matters:
        a rescan can rebuild the owner key itself, and this test wants the
        redeem RPC to do it.
        """
        self.stop_node(0)
        edit()
        self.start_node(0, extra_args=self.extra_args[0])
        self.nodes[0].syncwithvalidationinterfacequeue()
        self.nodes[0].setmockoracleprice(ORACLE_PRICE_MICRO_USD)

    # ------------------------------------------------------------------
    def position(self, wallet, position_id):
        matches = [p for p in wallet.listdigidollarpositions(False) if p["position_id"] == position_id]
        assert_equal(len(matches), 1)
        return matches[0]

    def advance_past(self, unlock_height):
        node = self.nodes[0]
        if node.getblockcount() <= unlock_height:
            self.generate(node, unlock_height - node.getblockcount() + 1)

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 200)
        assert_equal(node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)["price_micro_usd"], ORACLE_PRICE_MICRO_USD)

        funding = node.get_wallet_rpc(self.default_wallet_name)
        node.createwallet(wallet_name="minter", load_on_startup=True)
        minter = node.get_wallet_rpc("minter")
        funding.sendtoaddress(minter.getnewaddress(), 100000)
        self.generate(node, 1)

        self.log.info("Minting two tier-0 positions")
        mint_a = minter.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        self.generate(node, 1)
        mint_b = minter.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        self.generate(node, 1)
        pos_a = mint_a["position_id"]
        pos_b = mint_b["position_id"]
        self.advance_past(max(mint_a["unlock_height"], mint_b["unlock_height"]))
        assert_equal(self.position(minter, pos_a)["can_redeem"], True)

        self.log.info("Removing the written-down owner key of position A")
        self.with_node_stopped(lambda: assert_equal(self.delete_row("minter", record_key("ddownerkey", pos_a)), 1))
        minter = node.get_wallet_rpc("minter")
        # The position record itself is still there.
        assert_equal(self.position(minter, pos_a)["status"], "unlocked")

        self.log.info("Redeeming position A recovers the matching owner key")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        redeem_a = minter.redeemdigidollar(pos_a, MINT_AMOUNT_CENTS)
        assert_equal(redeem_a["position_closed"], True)
        self.generate(node, 1)
        assert_equal(self.position(minter, pos_a)["status"], "redeemed")

        self.log.info("The key that was found again is written back to the wallet")
        self.with_node_stopped(lambda: assert_equal(len(self.db_rows("minter", record_key("ddownerkey", pos_a))), 1))
        minter = node.get_wallet_rpc("minter")

        self.log.info("A copy of the wallet without private keys is refused, and says so")
        node.createwallet(wallet_name="watch", disable_private_keys=True, blank=True, load_on_startup=True)
        watch = node.get_wallet_rpc("watch")
        public_descriptors = minter.listdescriptors()["descriptors"]
        imports = [{"desc": d["desc"], "timestamp": 0, "active": d.get("active", False),
                    "internal": d.get("internal", False)} | ({"range": d["range"]} if "range" in d else {})
                   for d in public_descriptors]
        watch.importdescriptors(imports)
        assert_raises_rpc_error(-4, "Private keys are disabled for this wallet",
                                watch.redeemdigidollar, pos_b, MINT_AMOUNT_CENTS)

        self.log.info("A locked wallet is refused, and asks for the passphrase")
        minter.encryptwallet(PASSPHRASE)
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase with walletpassphrase first",
                                minter.redeemdigidollar, pos_b, MINT_AMOUNT_CENTS)

        self.log.info("An encrypted wallet recovers a missing key while unlocked and stores it encrypted")

        def drop_encrypted_key():
            assert_equal(self.delete_row("minter", record_key("ddcownerkey", pos_b)), 1)
            assert_equal(len(self.db_rows("minter", record_key("ddownerkey", pos_b))), 0)

        self.with_node_stopped(drop_encrypted_key)
        minter = node.get_wallet_rpc("minter")
        minter.walletpassphrase(PASSPHRASE, 3600)
        redeem_b = minter.redeemdigidollar(pos_b, MINT_AMOUNT_CENTS)
        assert_equal(redeem_b["position_closed"], True)
        self.generate(node, 1)
        assert_equal(self.position(minter, pos_b)["status"], "redeemed")

        def check_encrypted_key():
            assert_equal(len(self.db_rows("minter", record_key("ddcownerkey", pos_b))), 1)
            assert_equal(len(self.db_rows("minter", record_key("ddownerkey", pos_b))), 0)

        self.with_node_stopped(check_encrypted_key)
        minter = node.get_wallet_rpc("minter")
        minter.walletpassphrase(PASSPHRASE, 3600)

        self.log.info("A wallet without the matching key gets a distinct error with restore advice")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        mint_c = minter.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        pos_c = mint_c["position_id"]
        self.generate(node, 1)
        self.advance_past(mint_c["unlock_height"])
        node.createwallet(wallet_name="stranger", load_on_startup=True)
        # Give the stranger wallet the position record and the mint transaction,
        # but none of the keys: this is what a wallet restored from the wrong
        # seed looks like.
        def give_stranger_the_position():
            self.copy_row("minter", "stranger", record_key("ddposition", pos_c))
            self.copy_row("minter", "stranger", record_key("tx", pos_c))

        self.with_node_stopped(give_stranger_the_position)
        stranger = node.get_wallet_rpc("stranger")
        assert_equal(self.position(stranger, pos_c)["status"], "unlocked")
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        assert_raises_rpc_error(-4, "No key in this wallet matches the owner of this DigiDollar vault",
                                stranger.redeemdigidollar, pos_c, MINT_AMOUNT_CENTS)
        # The message tells the user what to do, it is not just a bare failure.
        assert_raises_rpc_error(-4, "rescanblockchain",
                                stranger.redeemdigidollar, pos_c, MINT_AMOUNT_CENTS)

        self.log.info("The original wallet still redeems position C normally")
        minter = node.get_wallet_rpc("minter")
        minter.walletpassphrase(PASSPHRASE, 3600)
        redeem_c = minter.redeemdigidollar(pos_c, MINT_AMOUNT_CENTS)
        assert_equal(redeem_c["position_closed"], True)
        self.generate(node, 1)
        assert_equal(self.position(minter, pos_c)["status"], "redeemed")
        assert_greater_than(minter.getbalance(), 0)


if __name__ == "__main__":
    DigiDollarOwnerKeyRecoveryTest().main()
