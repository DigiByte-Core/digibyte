#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A mint that can no longer be mined must not keep DGB reserved for ever.

A mint names the exact height at which its collateral unlocks. A block can only
include that mint while the lock still left to run is at least the full length
of the tier the mint claims, which leaves about 100 blocks after the wallet
built it. Once the chain is past that, a mint that never confirmed and is in no
mempool can never be mined here.

The wallet used to keep such a mint as a local transaction for ever. Its DGB
inputs stayed reserved and its collateral and token outputs stayed locked. The
wallet now reports it as an expired mint and frees what it was holding. It keeps
the owner key and the record of the attempt, so a reorg or a late confirmation
brings the position straight back.

What this test checks:

A. A mint the wallet never sent passes its window: it is reported as
   "expired_mint", the DGB balance comes back, the coin locks are gone, a
   restart does not bring them back, and undoing the last block makes the
   wallet look at the state again.
B. Node 0 gives up on a mint, node 1 mines it in time on a longer chain, and
   after node 0 follows that chain the position is active again, its outputs
   are locked again, and the owner key the wallet kept still redeems it.
C. A mint still sitting in the mempool is not expired, even at the last height
   that could include it, and confirms there normally.
D. Undoing the block that held a confirmed mint puts it back in the mempool as
   pending, and reconsidering that block makes it active again.
"""

from test_framework.messages import tx_from_hex
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_greater_than

ORACLE_PRICE_MICRO_USD = 500000
MINT_AMOUNT_CENTS = 100000  # $1000.00
TIER0_LOCK_BLOCKS = 240     # tier 0 locks collateral for 1 hour, which is 240 blocks
CONFIRMATION_BUFFER = 100   # extra blocks a mint may wait before it must be mined
BASE_ARGS = ["-digidollar=1", "-txindex=1", "-dandelion=0"]


class DigiDollarExpiredMintTest(DigiByteTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Node 0 keeps its mints wallet-local so the test controls who sees them.
        self.extra_args = [BASE_ARGS + ["-walletbroadcast=0"], BASE_ARGS]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    # ------------------------------------------------------------------
    def position(self, node, position_id):
        matches = [p for p in node.listdigidollarpositions(False) if p["position_id"] == position_id]
        assert_equal(len(matches), 1)
        return matches[0]

    def history_row(self, node, txid):
        rows = [t for t in node.listdigidollartxs(50, 0, "", "mint") if t["txid"] == txid]
        assert_equal(len(rows), 1)
        return rows[0]

    def locked_outpoints(self, node):
        return {(entry["txid"], entry["vout"]) for entry in node.listlockunspent()}

    def mint_inputs(self, node, txid):
        # The inputs are read out of the raw bytes rather than through
        # decoderawtransaction, which saves a second round trip to the node.
        tx = tx_from_hex(node.gettransaction(txid)["hex"])
        return {("%064x" % vin.prevout.hash, vin.prevout.n) for vin in tx.vin}

    def unspent_outpoints(self, node):
        return {(entry["txid"], entry["vout"]) for entry in node.listunspent(0)}

    def assert_inputs_reserved(self, node, inputs):
        assert_equal(inputs & self.unspent_outpoints(node), set())

    def assert_inputs_released(self, node, inputs):
        assert_equal(inputs & self.unspent_outpoints(node), inputs)

    def restart_node0(self, extra):
        self.restart_node(0, extra_args=BASE_ARGS + extra)
        self.connect_nodes(0, 1)
        self.nodes[0].syncwithvalidationinterfacequeue()
        self.nodes[0].setmockoracleprice(ORACLE_PRICE_MICRO_USD)

    def last_valid_height(self, unlock_height):
        """The last height at which a block could still include this tier 0 mint."""
        return unlock_height - TIER0_LOCK_BLOCKS

    def fork_height(self, a, b):
        """Highest height at which both nodes hold the same block."""
        height = min(a.getblockcount(), b.getblockcount())
        while height > 0 and a.getblockhash(height) != b.getblockhash(height):
            height -= 1
        return height

    def hand_over_chain(self, src, dst):
        """Give dst every block of src's chain that dst does not have yet.

        The two chains here have forked more than a hundred blocks back. A node
        will not follow a fork that old from a peer it has just met, because
        that is how it protects itself from being fed junk headers. The blocks
        are handed over directly instead, which still puts them through full
        validation and still makes dst reorganise onto the longer chain.
        """
        start = self.fork_height(src, dst) + 1
        for height in range(start, src.getblockcount() + 1):
            dst.submitblock(src.getblock(src.getblockhash(height), 0))

    def run_test(self):
        node0, node1 = self.nodes
        self.generate(node0, 200)
        for node in self.nodes:
            node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        self.test_local_mint_expires()
        self.test_late_confirmation_through_reorg()
        self.test_mempool_mint_is_not_expired()
        self.test_disconnected_mint_block()

    def test_local_mint_expires(self):
        self.log.info("A: a wallet-local mint that misses its window is released and reported as expired")
        node0 = self.nodes[0]
        tip = node0.getblockcount()

        mint = node0.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        position_id = mint["position_id"]
        unlock_height = mint["unlock_height"]
        assert_equal(unlock_height, tip + 1 + TIER0_LOCK_BLOCKS + CONFIRMATION_BUFFER)
        assert position_id not in node0.getrawmempool()
        last_valid = self.last_valid_height(unlock_height)
        assert_equal(last_valid, tip + 1 + CONFIRMATION_BUFFER)
        inputs = self.mint_inputs(node0, position_id)
        assert_greater_than(len(inputs), 0)

        # While the attempt is live its DGB inputs are reserved.
        self.assert_inputs_reserved(node0, inputs)
        assert_equal(self.position(node0, position_id)["status"], "pending")

        # A restart while the mint can still be mined locks its collateral and
        # token outputs again.
        self.generate(node0, 50)
        self.restart_node0(["-walletbroadcast=0"])
        assert (position_id, 0) in self.locked_outpoints(node0)
        assert (position_id, 1) in self.locked_outpoints(node0)
        assert_equal(self.position(node0, position_id)["status"], "pending")

        # Up to the last height that could still include the mint it stays pending.
        self.generate(node0, (last_valid - 1) - node0.getblockcount())
        assert_equal(node0.getblockcount(), last_valid - 1)
        node0.syncwithvalidationinterfacequeue()
        assert_equal(self.position(node0, position_id)["status"], "pending")
        self.assert_inputs_reserved(node0, inputs)

        # One more block and the next block can no longer include it.
        self.generate(node0, 1)
        node0.syncwithvalidationinterfacequeue()
        expired = self.position(node0, position_id)
        assert_equal(expired["status"], "expired_mint")
        assert_equal(expired["can_redeem"], False)
        row = self.history_row(node0, position_id)
        assert_equal(row["wallet_state"], "expired_mint")
        assert_equal(row["abandoned"], True)
        self.assert_inputs_released(node0, inputs)
        assert_equal(self.locked_outpoints(node0), set())
        assert_equal(node0.getdigidollarbalance("", 0)["unconfirmed"], 0)
        # The default active-only view no longer lists it.
        assert_equal([p for p in node0.listdigidollarpositions() if p["position_id"] == position_id], [])

        self.log.info("A: a restart keeps it expired and does not re-lock or re-reserve anything")
        self.restart_node0(["-walletbroadcast=0"])
        assert_equal(self.position(node0, position_id)["status"], "expired_mint")
        self.assert_inputs_released(node0, inputs)
        assert_equal(self.locked_outpoints(node0), set())

        self.log.info("A: undoing the last block re-evaluates the attempt against the shorter chain")
        undone = node0.getbestblockhash()
        node0.invalidateblock(undone)
        node0.syncwithvalidationinterfacequeue()
        # The window is open again at this height, but the wallet does not resend a
        # mint it already gave up on: the attempt stays abandoned and inactive.
        assert_equal(self.position(node0, position_id)["status"], "abandoned_mint")
        self.assert_inputs_released(node0, inputs)
        node0.reconsiderblock(undone)
        node0.syncwithvalidationinterfacequeue()
        assert_equal(self.position(node0, position_id)["status"], "expired_mint")
        self.sync_blocks()

    def test_late_confirmation_through_reorg(self):
        self.log.info("B: a mint node 0 gave up on is mined by node 1 and comes back after the reorg")
        node0, node1 = self.nodes
        self.disconnect_nodes(0, 1)

        mint = node0.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        position_id = mint["position_id"]
        last_valid = self.last_valid_height(mint["unlock_height"])
        raw_mint = node0.gettransaction(position_id)["hex"]
        inputs = self.mint_inputs(node0, position_id)
        self.assert_inputs_reserved(node0, inputs)

        # Node 1 is handed the mint directly. Node 0 never sees it in a mempool.
        node1.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        node1.sendrawtransaction(raw_mint)
        assert position_id in node1.getrawmempool()
        assert position_id not in node0.getrawmempool()

        # Node 0 mines past the point where the mint could be included and gives up.
        self.generate(node0, last_valid - node0.getblockcount(), sync_fun=self.no_op)
        node0.syncwithvalidationinterfacequeue()
        assert_equal(self.position(node0, position_id)["status"], "expired_mint")
        self.assert_inputs_released(node0, inputs)
        assert_equal(self.locked_outpoints(node0), set())

        # Node 1 mines the mint straight away, in good time, and builds a longer chain.
        mint_block = self.generate(node1, 1, sync_fun=self.no_op)[0]
        assert position_id in node1.getblock(mint_block)["tx"]
        self.generate(node1, node0.getblockcount() - node1.getblockcount() + 1, sync_fun=self.no_op)
        assert_greater_than(node1.getblockcount(), node0.getblockcount())

        self.hand_over_chain(node1, node0)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        node0.syncwithvalidationinterfacequeue()

        recovered = self.position(node0, position_id)
        assert_equal(recovered["status"], "active")
        assert_greater_than(recovered["confirmations"], 0)
        row = self.history_row(node0, position_id)
        assert_equal(row["wallet_state"], "confirmed")
        assert_equal(row["abandoned"], False)
        assert_equal(node0.getdigidollarbalance()["confirmed"], MINT_AMOUNT_CENTS)
        assert (position_id, 0) in self.locked_outpoints(node0)
        assert (position_id, 1) in self.locked_outpoints(node0)
        self.assert_inputs_reserved(node0, inputs)

        self.log.info("B: the owner key the wallet kept still redeems the position")
        self.restart_node0([])
        self.generate(node0, mint["unlock_height"] - node0.getblockcount() + 1)
        node0.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        redeem = node0.redeemdigidollar(position_id, MINT_AMOUNT_CENTS)
        assert_equal(redeem["position_closed"], True)
        self.generate(node0, 1)
        assert_equal(self.position(node0, position_id)["status"], "redeemed")

    def test_mempool_mint_is_not_expired(self):
        self.log.info("C: a mint that is still in the mempool is not expired at the last valid height")
        node0 = self.nodes[0]
        node0.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        mint = node0.mintdigidollar(MINT_AMOUNT_CENTS, 0)
        position_id = mint["position_id"]
        last_valid = self.last_valid_height(mint["unlock_height"])
        assert position_id in node0.getrawmempool()

        # Mine empty blocks so the mint stays in the mempool while the chain moves
        # on. Only node 0 matters from here, so the other node is left alone.
        empty_block_address = node0.getnewaddress()
        while node0.getblockcount() < last_valid - 1:
            self.generateblock(node0, empty_block_address, [], sync_fun=self.no_op)
        node0.syncwithvalidationinterfacequeue()
        assert position_id in node0.getrawmempool()
        pending = self.position(node0, position_id)
        assert_equal(pending["status"], "pending")
        row = self.history_row(node0, position_id)
        assert_equal(row["in_mempool"], True)
        assert_equal(row["wallet_state"], "pending")
        assert_equal(node0.getdigidollarbalance("", 0)["unconfirmed"], MINT_AMOUNT_CENTS)

        # It confirms at the last valid height.
        node0.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        block = self.generate(node0, 1, sync_fun=self.no_op)[0]
        assert position_id in node0.getblock(block)["tx"]
        assert_equal(node0.getblockcount(), last_valid)
        assert_equal(self.position(node0, position_id)["status"], "active")
        self.mempool_mint = (position_id, block)

    def test_disconnected_mint_block(self):
        self.log.info("D: a disconnected mint block sends the mint back to the mempool as pending")
        node0 = self.nodes[0]
        position_id, block = self.mempool_mint
        node0.invalidateblock(block)
        node0.syncwithvalidationinterfacequeue()
        assert position_id in node0.getrawmempool()
        assert_equal(self.position(node0, position_id)["status"], "pending")
        assert_equal(self.history_row(node0, position_id)["wallet_state"], "pending")

        node0.reconsiderblock(block)
        node0.syncwithvalidationinterfacequeue()
        assert_equal(self.position(node0, position_id)["status"], "active")


if __name__ == "__main__":
    DigiDollarExpiredMintTest().main()
