#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The DigiDollar balance must wait for the wallet to catch up with the chain.

When a block is connected, the node hands it to the wallet on a different
thread. Until the wallet has worked through that block it still treats the
coins in it as unconfirmed. getdigidollarbalance reports confirmed
DigiDollars, so a call that arrives before the wallet has caught up reports
too little, or zero, for DigiDollars that are already in a block.

Every other DigiDollar wallet command that reports confirmed state waits for
the wallet first. This test mines a block and asks for the balance straight
away, with nothing in between that would let the wallet catch up, and requires
the answer to be right every time.

Two shapes, because both happen in real use:

A. The node that mined the block asks its own wallet. Anyone who mines and
   then reads their balance does this.
B. A second node receives the block over the network and its wallet is asked
   as soon as the block is in its chain. An exchange watching for a deposit
   does this.

C. Every other DigiDollar wallet command is asked the same way. Seven of them
   read wallet state that block processing changes on another thread, and the
   answer must already include the newest block. Three of them pick coins to
   spend, so reading too early is not a display problem: it loses money that is
   already confirmed, or reaches for a coin the block just spent.

Each round puts a pile of ordinary wallet transactions in the same block, so
the wallet has real work to do on the block and the gap this test is about is
wide enough to hit.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal

# Regtest DigiDollar activates at this height, so block 432 is the first block
# under DigiDollar rules.
ACTIVATION_HEIGHT = 432

ORACLE_PRICE_MICRO_USD = 500_000   # $0.50 per DGB
MINT_AMOUNT_CENTS = 100_000        # $1,000.00
SEND_AMOUNT_CENTS = 5_000          # $50.00
MINT_TIER = 0                      # tier 0 locks collateral for one hour

MINT_ROUNDS = 6
SEND_ROUNDS = 4
READ_ROUNDS = 4
# DGB paid into a fresh wallet so it can back a mint of MINT_AMOUNT_CENTS.
COLLATERAL_DGB = 300_000
# Ordinary wallet transactions added to each block to give the wallet work.
FILLER_TXS = 25


class DigiDollarBalanceSyncTest(DigiByteTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        args = [
            f"-digidollaractivationheight={ACTIVATION_HEIGHT}",
            "-dandelion=0",
            "-fallbackfee=0.0001",
        ]
        self.extra_args = [args, args]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ------------------------------------------------------------------
    def set_price(self, node):
        """Publish a fresh regtest oracle quote so a DigiDollar block can build."""
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

    def mine_without_waiting_for_wallets(self, node):
        """Mine one block and return as soon as the block is in the chain.

        The block is deliberately NOT followed by anything that waits for the
        wallets: no mempool sync, no syncwithvalidationinterfacequeue. The
        wallet of every node is still working through the block when this
        returns, which is the situation the balance command has to cope with.
        """
        self.generate(node, 1, sync_fun=lambda: self.sync_blocks(self.nodes, wait=0.01))

    def add_filler_transactions(self, node):
        """Put ordinary wallet transactions in the next block."""
        address = node.getnewaddress()
        for _ in range(FILLER_TXS):
            node.sendtoaddress(address, 1)

    def dd_total(self, node):
        return int(node.getdigidollarbalance()["total"])

    def check_balance_is_not_early(self, node, expected, label):
        """The balance read straight after the block must already be right."""
        immediate = self.dd_total(node)
        # Let the wallet finish, then read again. The two answers must agree:
        # the balance a caller is given must not depend on whether the wallet
        # happened to have caught up yet.
        node.syncwithvalidationinterfacequeue()
        settled = self.dd_total(node)
        assert_equal(settled, expected)
        assert_equal(immediate, expected)
        self.log.info("  %s: balance answered %d cents with no wait", label, immediate)

    # ------------------------------------------------------------------
    def run_test(self):
        node0, node1 = self.nodes
        self.generate(node0, ACTIVATION_HEIGHT + 30)
        for node in self.nodes:
            self.set_price(node)

        self.test_balance_on_the_mining_node()
        self.test_balance_on_the_receiving_node()
        self.test_reading_commands_include_the_newest_block()
        self.test_spending_commands_see_coins_from_the_newest_block()

    def test_balance_on_the_mining_node(self):
        self.log.info("A: the node that mined the block gets the right balance at once")
        node0 = self.nodes[0]
        expected = self.dd_total(node0)

        for round_number in range(1, MINT_ROUNDS + 1):
            self.set_price(node0)
            node0.mintdigidollar(MINT_AMOUNT_CENTS, MINT_TIER)
            self.add_filler_transactions(node0)
            self.set_price(node0)
            self.mine_without_waiting_for_wallets(node0)
            expected += MINT_AMOUNT_CENTS
            self.check_balance_is_not_early(node0, expected, f"mint round {round_number}")

    def test_balance_on_the_receiving_node(self):
        self.log.info("B: a node that received the block over the network gets it too")
        node0, node1 = self.nodes
        # Node 1 has no coins of its own here; it only receives DigiDollars.
        received = self.dd_total(node1)

        for round_number in range(1, SEND_ROUNDS + 1):
            dd_address = node1.getdigidollaraddress()
            self.set_price(node0)
            node0.senddigidollar(dd_address, SEND_AMOUNT_CENTS)
            self.add_filler_transactions(node0)
            self.set_price(node0)
            self.mine_without_waiting_for_wallets(node0)
            received += SEND_AMOUNT_CENTS
            self.check_balance_is_not_early(node1, received, f"receive round {round_number}")


    # ------------------------------------------------------------------
    # C. The rest of the DigiDollar wallet commands.
    # ------------------------------------------------------------------
    def test_reading_commands_include_the_newest_block(self):
        """Every DigiDollar command that reports wallet state is right at once.

        One command per block. Each of these commands now waits for the wallet
        before it reads, so asking two of them after the same block would let
        the first one do the waiting for the second and prove nothing.
        """
        self.log.info("C1: the reading commands include the newest block with no wait")
        node0, node1 = self.nodes
        balance = self.dd_total(node1)

        for command in ("getdigidollarbalance", "listdigidollartxs",
                        "listdigidollaraddresses", "validateddaddress",
                        "getdigidollaraddress"):
            for _ in range(READ_ROUNDS):
                dd_address = node1.getdigidollaraddress()
                self.set_price(node0)
                txid = node0.senddigidollar(dd_address, SEND_AMOUNT_CENTS)["txid"]
                self.add_filler_transactions(node0)
                self.set_price(node0)
                self.mine_without_waiting_for_wallets(node0)
                balance += SEND_AMOUNT_CENTS

                # Exactly one command is asked here, before anything else has
                # had a chance to wait for the wallet.
                if command == "getdigidollarbalance":
                    assert_equal(self.dd_total(node1), balance)
                elif command == "listdigidollartxs":
                    confirmed = [tx for tx in node1.listdigidollartxs(50)
                                 if tx["txid"] == txid and tx["confirmations"] >= 1]
                    assert_equal(len(confirmed), 1)
                elif command == "listdigidollaraddresses":
                    listed = [entry for entry in node1.listdigidollaraddresses()
                              if entry["address"] == dd_address]
                    assert_equal(len(listed), 1)
                    assert_equal(int(listed[0]["balance"]), SEND_AMOUNT_CENTS)
                elif command == "validateddaddress":
                    assert_equal(node1.validateddaddress(dd_address)["ismine"], True)
                else:
                    # getdigidollaraddress hands out a new address and records
                    # it. There is no figure here that the block could change,
                    # so this only checks the command works when the wallet is
                    # still busy with a block.
                    fresh = node1.getdigidollaraddress()
                    assert_equal(node1.validateddaddress(fresh)["isvalid"], True)

                # Settle before the next round so each round starts level.
                node1.syncwithvalidationinterfacequeue()
                assert_equal(self.dd_total(node1), balance)

            self.log.info("  %s: right every time with no wait", command)

    def test_spending_commands_see_coins_from_the_newest_block(self):
        """Minting and sending pick from coins the newest block confirmed."""
        self.log.info("C2: the spending commands see coins confirmed in the newest block")
        self.check_mint_uses_dgb_from_the_newest_block()
        self.check_send_uses_dd_from_the_newest_block("senddigidollar")
        self.check_send_uses_dd_from_the_newest_block("sendmanydigidollar")

    def new_wallet_holding_only_new_coins(self, name, dgb):
        """A wallet whose only coins arrive in one settled block."""
        node0, node1 = self.nodes
        node1.createwallet(wallet_name=name)
        wallet = node1.get_wallet_rpc(name)
        node0.sendtoaddress(wallet.getnewaddress(), dgb)
        self.generate(node0, 1)
        assert_equal(wallet.getbalances()["mine"]["trusted"] > 0, True)
        return wallet

    def check_mint_uses_dgb_from_the_newest_block(self):
        """A mint must spend DGB that the block just mined confirmed."""
        node0, node1 = self.nodes
        node1.createwallet(wallet_name="mint_race")
        wallet = node1.get_wallet_rpc("mint_race")

        node0.sendtoaddress(wallet.getnewaddress(), COLLATERAL_DGB)
        self.add_filler_transactions(node0)
        self.set_price(node0)
        self.set_price(node1)
        self.mine_without_waiting_for_wallets(node0)

        # This wallet owns nothing except what the block just confirmed. If
        # minting picks coins before the wallet has worked through the block it
        # finds an empty wallet.
        mint = wallet.mintdigidollar(MINT_AMOUNT_CENTS, MINT_TIER)
        assert mint["txid"]
        self.log.info("  mintdigidollar built a mint from DGB confirmed moments earlier")

        self.generate(node0, 1)
        node1.unloadwallet("mint_race")

    def check_send_uses_dd_from_the_newest_block(self, command):
        """A DigiDollar send must spend DigiDollars the block just confirmed."""
        node0, node1 = self.nodes
        name = f"{command}_race"
        wallet = self.new_wallet_holding_only_new_coins(name, COLLATERAL_DGB)

        self.set_price(node1)
        wallet.mintdigidollar(MINT_AMOUNT_CENTS, MINT_TIER)
        self.sync_mempools()
        self.add_filler_transactions(node0)
        self.set_price(node0)
        destination = node0.getdigidollaraddress()
        second = node0.getdigidollaraddress()
        self.mine_without_waiting_for_wallets(node0)

        # The DigiDollars this send needs exist only in the block just mined.
        # DigiDollar sends may only spend confirmed outputs, so reading too
        # early leaves the wallet with nothing to send.
        if command == "senddigidollar":
            result = wallet.senddigidollar(destination, SEND_AMOUNT_CENTS)
        else:
            result = wallet.sendmanydigidollar("", {destination: SEND_AMOUNT_CENTS,
                                                    second: SEND_AMOUNT_CENTS})
        assert result["txid"]
        self.log.info("  %s spent DigiDollars confirmed moments earlier", command)

        self.generate(node0, 1)
        node1.unloadwallet(name)


if __name__ == "__main__":
    DigiDollarBalanceSyncTest().main()
