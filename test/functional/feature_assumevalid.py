#!/usr/bin/env python3
# Copyright (c) 2014-2022 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test logic for skipping signature validation on old blocks.

Test logic for skipping signature validation on blocks which we've assumed
valid (https://github.com/digibyte/digibyte/pull/9484)

We build a chain that includes an invalid signature for one of the
transactions:

    0:        genesis block
    1:        block 1 with coinbase transaction output.
    2-101:    bury that block with 100 blocks so the coinbase transaction
              output can be spent
    102:      a block containing a transaction spending the coinbase
              transaction output. The transaction has an invalid signature.
    103-134:  add 32 Scrypt blocks during the early multi-algorithm work era
    135:      add a SHA256d tip, making the bad block's equivalent-work burial
              greater than two weeks

Start three nodes:

    - node0 has no -assumevalid parameter. Try to sync to block 135. It will
      reject block 102 and only sync as far as block 101
    - node1 has -assumevalid set to the hash of block 102. Try to sync to
      block 135. node1 will sync all the way to block 135.
    - node2 has -assumevalid set to the hash of block 102. Try to sync to
      block 103. node2 will reject block 102 since it's assumed valid, but it
      isn't buried by at least two weeks' work.
"""

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.blockversion import VERSIONBITS_LAST_OLD_BLOCK_VERSION
from test_framework.messages import (
    BLOCK_VERSION_SHA256D,
    CBlockHeader,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    msg_block,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.script import (
    CScript,
    OP_TRUE,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal
from test_framework.wallet_util import generate_keypair

REGTEST_TARGET_SPACING = 15
TWO_WEEKS = 14 * 24 * 60 * 60


class BaseNode(P2PInterface):
    def send_header_for_blocks(self, new_blocks):
        headers_message = msg_headers()
        headers_message.headers = [CBlockHeader(b) for b in new_blocks]
        self.send_message(headers_message)


class AssumeValidTest(DigiByteTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.rpc_timeout = 120

    def setup_network(self):
        self.add_nodes(3)
        # Start node0. We don't start the other nodes yet since
        # we need to pre-mine a block with an invalid transaction
        # signature so we can pass in the block hash as assumevalid.
        self.start_node(0)

    def send_blocks_until_disconnected(self, p2p_conn, blocks):
        """Keep sending blocks to the node until we're disconnected."""
        for block in blocks:
            if not p2p_conn.is_connected:
                break
            try:
                p2p_conn.send_message(msg_block(block))
            except IOError:
                assert not p2p_conn.is_connected
                break
        p2p_conn.wait_for_disconnect()

    def equivalent_work_seconds(self, node, tip, buried_block):
        """Measure equivalent burial from the node's accepted header work."""
        tip_header = node.getblockheader(tip.hash)
        parent_header = node.getblockheader(tip_header["previousblockhash"])
        buried_header = node.getblockheader(buried_block.hash)
        tip_work = int(tip_header["chainwork"], 16)
        last_block_work = tip_work - int(parent_header["chainwork"], 16)
        assert last_block_work > 0
        return (tip_work - int(buried_header["chainwork"], 16)) * REGTEST_TARGET_SPACING // last_block_work

    def run_test(self):
        # Build the blockchain
        self.tip = int(self.nodes[0].getbestblockhash(), 16)
        self.block_time = self.nodes[0].getblock(self.nodes[0].getbestblockhash())['time'] + 1

        self.blocks = []

        # Get a pubkey for the coinbase TXO
        _, coinbase_pubkey = generate_keypair()

        # Create the first block with a coinbase output to our key
        height = 1
        block = create_block(self.tip, create_coinbase(height, coinbase_pubkey), self.block_time)
        self.blocks.append(block)
        self.block_time += 1
        block.solve()
        # Save the coinbase for later
        self.block1 = block
        self.tip = block.sha256
        height += 1

        # Mature the coinbase and enter the regtest multi-algorithm work era.
        for _ in range(100):
            block = create_block(self.tip, create_coinbase(height), self.block_time)
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        # Create a transaction spending the coinbase output with an invalid (null) signature
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(self.block1.vtx[0].sha256, 0), scriptSig=b""))
        tx.vout.append(CTxOut(49 * 100000000, CScript([OP_TRUE])))
        tx.calc_sha256()

        bad_height = height
        bad_block = create_block(self.tip, create_coinbase(height), self.block_time, txlist=[tx])
        bad_block.solve()
        self.blocks.append(bad_block)
        self.tip = bad_block.sha256
        self.block_time += 1
        height += 1

        # Before height 400, equal-target Scrypt contributes 4096 times the
        # work of SHA256d. With a SHA256d tip, these 32 Scrypt successors give
        # (32 * 4096 + 1) * 15 = 1,966,095 equivalent seconds: over 22 days.
        # Check the actual header work below so this margin cannot disappear.
        for _ in range(32):
            block = create_block(self.tip, create_coinbase(height), self.block_time)
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        block = create_block(self.tip, create_coinbase(height), self.block_time,
                             version=VERSIONBITS_LAST_OLD_BLOCK_VERSION | BLOCK_VERSION_SHA256D)
        block.solve()
        self.blocks.append(block)
        final_height = height
        shallow_height = bad_height + 1

        # Both nodes assume the same block valid; burial decides whether scripts are skipped.
        self.start_node(1, extra_args=["-assumevalid=" + bad_block.hash])
        self.start_node(2, extra_args=["-assumevalid=" + bad_block.hash])

        p2p0 = self.nodes[0].add_p2p_connection(BaseNode())
        p2p0.send_header_for_blocks(self.blocks)
        p2p0.sync_with_ping()

        # Send blocks to node0. Block 102 will be rejected.
        self.log.info("Default verification must reject the bad signature at height %d", bad_height)
        self.send_blocks_until_disconnected(p2p0, self.blocks)
        assert_equal(self.nodes[0].getblockcount(), bad_height - 1)
        assert_equal(self.nodes[0].getbestblockhash(), self.blocks[bad_height - 2].hash)

        p2p1 = self.nodes[1].add_p2p_connection(BaseNode())
        p2p1.send_header_for_blocks(self.blocks)
        p2p1.sync_with_ping()
        assert_equal(self.nodes[1].getblockchaininfo()["headers"], final_height)
        deep_burial = self.equivalent_work_seconds(self.nodes[1], self.blocks[-1], bad_block)
        assert deep_burial > TWO_WEEKS, f"Insufficient deep-chain burial: {deep_burial} seconds"
        self.log.info("Assumed-valid chain has %d equivalent seconds of burial", deep_burial)

        # Send all blocks to node1. All blocks will be accepted.
        for block in self.blocks:
            p2p1.send_message(msg_block(block))
        p2p1.sync_with_ping()
        assert_equal(self.nodes[1].getblockcount(), final_height)
        assert_equal(self.nodes[1].getbestblockhash(), self.blocks[-1].hash)

        p2p2 = self.nodes[2].add_p2p_connection(BaseNode())
        shallow_blocks = self.blocks[:shallow_height]
        p2p2.send_header_for_blocks(shallow_blocks)
        p2p2.sync_with_ping()
        assert_equal(self.nodes[2].getblockchaininfo()["headers"], shallow_height)
        shallow_burial = self.equivalent_work_seconds(self.nodes[2], shallow_blocks[-1], bad_block)
        assert shallow_burial <= TWO_WEEKS, f"Unexpected deep-chain burial: {shallow_burial} seconds"
        self.log.info("Insufficient-work chain has only %d equivalent seconds of burial", shallow_burial)

        # Send blocks to node2. Block 102 will be rejected.
        self.send_blocks_until_disconnected(p2p2, shallow_blocks)
        assert_equal(self.nodes[2].getblockcount(), bad_height - 1)
        assert_equal(self.nodes[2].getbestblockhash(), self.blocks[bad_height - 2].hash)


if __name__ == '__main__':
    AssumeValidTest().main()
