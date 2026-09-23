#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Reorg and reindex with a DigiDollar wallet whose mint address was used again.

The wallet fills an in-memory script registry that is keyed by output script
alone. When a mint's owner address later holds a different amount, the registry
keeps only the last amount. Block validation must not read that table: a node
holding such a wallet must still accept the original mint block when it
reconnects it during a reorg or a reindex.

Regtest allows one-cent mints, so this test makes the mint address later hold
more than the regtest maximum mint. Before the fix, reconnecting the mint block
on a node whose wallet held that address failed with bad-dd-mint-amount.
"""

from test_framework.address import byte_to_base58
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal

# Two-byte base58 version that gives regtest DigiDollar addresses their "RD" prefix.
DD_REGTEST_VERSION = (0xa3, 0xa4)
REGTEST_MAX_MINT_CENTS = 100000


def dd_address_for_token_script(script_hex):
    """Encode the taproot output key of a DigiDollar token output as a DigiDollar address."""
    assert script_hex.startswith("5120") and len(script_hex) == 68, script_hex
    output_key = bytes.fromhex(script_hex[4:])
    return byte_to_base58(bytes([DD_REGTEST_VERSION[1]]) + output_key, DD_REGTEST_VERSION[0])


class WalletDigiDollarReindexReusedAddressTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txindex=1"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="funder", descriptors=self.options.descriptors, load_on_startup=True)
        minter = node.get_wallet_rpc(self.default_wallet_name)
        funder = node.get_wallet_rpc("funder")

        self.log.info("Mining spendable funds for both wallets")
        self.generatetoaddress(node, 150, minter.getnewaddress())
        self.generatetoaddress(node, 150, funder.getnewaddress())
        node.setmockoracleprice(500000)

        self.log.info("Minting with the first wallet")
        mint = minter.mintdigidollar(10000, 0)
        self.generate(node, 1)
        mint_height = node.getblockcount()
        mint_block = node.getblockhash(mint_height)
        raw_mint = node.getrawtransaction(mint["txid"], True)
        token_outputs = [out for out in raw_mint["vout"]
                         if out["value"] == 0 and out["scriptPubKey"]["hex"].startswith("5120")]
        assert_equal(len(token_outputs), 1)
        mint_address = dd_address_for_token_script(token_outputs[0]["scriptPubKey"]["hex"])
        node.syncwithvalidationinterfacequeue()
        assert_equal(minter.getdigidollarbalance()["confirmed"], 10000)

        self.log.info("The mint address later receives more than one mint may create")
        for _ in range(2):
            funder.mintdigidollar(REGTEST_MAX_MINT_CENTS, 0)
            self.generate(node, 1)
        funder.senddigidollar(mint_address, REGTEST_MAX_MINT_CENTS + 50000)
        self.generate(node, 1)

        self.log.info("The first wallet spends its original token, so only the larger amount stays at the address")
        node.syncwithvalidationinterfacequeue()
        minter.senddigidollar(funder.getdigidollaraddress(), 10000)
        self.generate(node, 1)
        node.syncwithvalidationinterfacequeue()
        assert_equal(minter.getdigidollarbalance()["confirmed"], REGTEST_MAX_MINT_CENTS + 50000)
        assert_equal(funder.getdigidollarbalance()["confirmed"], 60000)

        tip = node.getbestblockhash()
        height = node.getblockcount()

        self.log.info("Reconnecting the mint block during a reorg with the wallets loaded")
        node.invalidateblock(mint_block)
        assert_equal(node.getblockcount(), mint_height - 1)
        node.reconsiderblock(mint_block)
        assert_equal(node.getbestblockhash(), tip)

        self.log.info("Reindexing with the wallets loaded")
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex=1"])
        node = self.nodes[0]
        self.wait_until(lambda: node.getblockcount() == height, timeout=180)
        assert_equal(node.getbestblockhash(), tip)
        node.syncwithvalidationinterfacequeue()
        minter = node.get_wallet_rpc(self.default_wallet_name)
        funder = node.get_wallet_rpc("funder")
        assert_equal(minter.getdigidollarbalance()["confirmed"], REGTEST_MAX_MINT_CENTS + 50000)
        assert_equal(funder.getdigidollarbalance()["confirmed"], 60000)


if __name__ == "__main__":
    WalletDigiDollarReindexReusedAddressTest().main()
