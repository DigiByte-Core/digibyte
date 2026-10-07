#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check that leftover DGB from DigiDollar transactions comes back to the wallet.

A mint, a send and a redemption all spend ordinary DGB and hand back what is
left over as a change output. If that change goes to an address the wallet does
not own, the money is gone and the balance looks as if it shrank.

This test mints, sends and redeems on a regtest node with a normal wallet, and
for each transaction it checks that:

  - every output holding DGB, apart from the locked collateral of a mint,
    belongs to the wallet (getaddressinfo says ismine),
  - the change output is listed by listunspent and is spendable, and
  - the wallet balance is the total of those spendable coins, so the change is
    counted.

The change of a mint must also be an ordinary bech32 output. A mint may hold
DGB in only one taproot output, the locked collateral, so taproot change would
make every node reject the mint.

Node 0 mines its own blocks, so a DigiDollar transaction is always in the
mempool of the node that mines it.
"""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

# One cent per DGB, written in millionths of a dollar.
ORACLE_PRICE_MICRO_USD = 500000  # $0.50 per DGB

MINT_CENTS = 100000  # $1,000.00
LOCK_TIER_ONE_HOUR = 0
SEND_CENTS = 1000  # $10.00


class DigiDollarMintChangeDestinationTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-dandelion=0", "-mocktime=0"],
            ["-digidollar=1", "-txindex=1", "-dandelion=0", "-mocktime=0"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_oracle_price(self):
        """Give every node a price, so the next block can carry a price quote."""
        for node in self.nodes:
            node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

    def mine(self, blocks=1):
        """Mine blocks, each carrying a fresh price quote for the mints."""
        self.set_oracle_price()
        self.generate(self.nodes[0], blocks)

    def confirm(self, node, txid, label):
        """Mine until the transaction is in a block, and say so."""
        self.mine(2)
        confirmations = node.gettransaction(txid)["confirmations"]
        assert_greater_than(confirmations, 0)
        self.log.info(f"{label}: {txid} has {confirmations} confirmations")

    def decode(self, node, txid):
        return node.getrawtransaction(txid, True)

    def dgb_outputs(self, tx):
        """Every output of the transaction that holds DGB."""
        return [out for out in tx["vout"] if out["value"] > 0]

    def collateral_script_of_mint(self, node, txid):
        """The locked collateral of a mint: its one taproot output holding DGB."""
        tx = self.decode(node, txid)
        scripts = [
            out["scriptPubKey"]["hex"]
            for out in self.dgb_outputs(tx)
            if out["scriptPubKey"]["type"] == "witness_v1_taproot"
        ]
        assert_equal(len(scripts), 1)
        return scripts[0]

    def address_of(self, out):
        return out["scriptPubKey"].get("address")

    def check_change_comes_back(self, node, txid, label, expect_bech32_change,
                               collateral_script=None):
        """Check where the leftover DGB of one transaction went.

        collateral_script is the locked collateral of a mint. Nobody's wallet
        owns it directly, so it is the one output allowed not to be ours.
        Returns the change outputs that were found.
        """
        tx = self.decode(node, txid)
        change_outputs = []

        for out in self.dgb_outputs(tx):
            script_hex = out["scriptPubKey"]["hex"]
            if collateral_script is not None and script_hex == collateral_script:
                self.log.info(
                    f"{label}: output {out['n']} is the locked collateral ({out['value']} DGB)")
                continue

            address = self.address_of(out)
            assert address is not None, f"{label}: output {out['n']} has no address"
            info = node.getaddressinfo(address)
            assert_equal(info["ismine"], True)
            self.log.info(
                f"{label}: output {out['n']} pays {out['value']} DGB to our own "
                f"{out['scriptPubKey']['type']} address {address}")
            change_outputs.append(out)

        assert_greater_than(len(change_outputs), 0)

        if expect_bech32_change:
            # A mint may hold DGB in only one taproot output, the collateral.
            for out in change_outputs:
                assert_equal(out["scriptPubKey"]["type"], "witness_v0_keyhash")

        return change_outputs

    def check_spendable(self, node, txid, outputs, label):
        """Check the wallet can spend these outputs and counts them in the balance."""
        unspent = {(u["txid"], u["vout"]): u for u in node.listunspent()}
        for out in outputs:
            key = (txid, out["n"])
            assert key in unspent, f"{label}: output {out['n']} is missing from listunspent"
            coin = unspent[key]
            assert_equal(coin["spendable"], True)
            assert_equal(coin["solvable"], True)
            assert_equal(coin["amount"], out["value"])
            self.log.info(f"{label}: output {out['n']} is spendable, {coin['amount']} DGB")

        # listunspent is what the wallet can spend, and getbalance is its total.
        # If the change were missing from one it would be missing from both.
        total = sum(coin["amount"] for coin in unspent.values())
        assert_equal(node.getbalance(), total)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mining past the DigiDollar activation height")
        self.set_oracle_price()
        self.generate(node, 680)
        self.set_oracle_price()

        self.log.info("Minting, and checking where the leftover DGB went")
        first_mint = node.mintdigidollar(MINT_CENTS, LOCK_TIER_ONE_HOUR)
        mint_txid = first_mint["txid"]
        position_id = first_mint["position_id"]
        unlock_height = first_mint["unlock_height"]
        self.confirm(node, mint_txid, "mint")

        # The collateral is locked by a script, not by a key the wallet holds,
        # so it is the one DGB output of a mint that is not ours to spend.
        mint_change = self.check_change_comes_back(
            node, mint_txid, "mint", expect_bech32_change=True,
            collateral_script=self.collateral_script_of_mint(node, mint_txid))
        self.check_spendable(node, mint_txid, mint_change, "mint")

        self.log.info("Minting a second position, so one can be redeemed in full")
        second_mint = node.mintdigidollar(MINT_CENTS, LOCK_TIER_ONE_HOUR)
        second_txid = second_mint["txid"]
        self.confirm(node, second_txid, "second mint")
        second_change = self.check_change_comes_back(
            node, second_txid, "second mint", expect_bech32_change=True,
            collateral_script=self.collateral_script_of_mint(node, second_txid))
        self.check_spendable(node, second_txid, second_change, "second mint")

        self.log.info("Sending DigiDollars, and checking where the leftover DGB went")
        recipient = self.nodes[1].getdigidollaraddress()
        send_result = node.senddigidollar(recipient, SEND_CENTS)
        send_txid = send_result["txid"]
        self.confirm(node, send_txid, "send")

        send_change = self.check_change_comes_back(
            node, send_txid, "send", expect_bech32_change=True)
        self.check_spendable(node, send_txid, send_change, "send")

        self.log.info("Waiting for the one hour lock to run out")
        remaining = unlock_height - node.getblockcount() + 2
        if remaining > 0:
            self.mine(remaining)
        assert_greater_than(node.getblockcount(), unlock_height)

        self.log.info("Redeeming, and checking where the returned DGB went")
        redeem_result = node.redeemdigidollar(position_id, MINT_CENTS)
        redeem_txid = redeem_result["txid"]
        self.confirm(node, redeem_txid, "redeem")

        # A redemption has no locked collateral of its own: the returned
        # collateral and the leftover fee money both belong to this wallet.
        redeem_outputs = self.check_change_comes_back(
            node, redeem_txid, "redeem", expect_bech32_change=False)
        self.check_spendable(node, redeem_txid, redeem_outputs, "redeem")

        self.log.info("Spending the mint change, to prove the money is really usable")
        spend_target = self.nodes[1].getnewaddress()
        change_coin = mint_change[0]
        spend_txid = node.sendtoaddress(
            spend_target, Decimal(change_coin["value"]) / 2, "", "", False, True)
        self.confirm(node, spend_txid, "spending the mint change")


if __name__ == "__main__":
    DigiDollarMintChangeDestinationTest().main()
