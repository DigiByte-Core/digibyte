#!/usr/bin/env python3
# Copyright (c) 2020-2022 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test indices in conjunction with prune."""
import os
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)


# Regtest turns DigiDollar on at the genesis block. A node that has DigiDollar
# keeps every block from the DigiDollar activation height upwards, so that it
# can always read the block that created a DigiDollar coin. On regtest that
# would mean keeping the whole chain, and then this test has no history it is
# allowed to delete. So set DigiDollar activation to a height this test never
# reaches. Every block it mines is then ordinary history that a pruned node may
# delete, which is what this test is here to check.
DIGIDOLLAR_NEVER_ACTIVATES = 2147483646
DIGIDOLLAR_OFF = f"-digidollaractivationheight={DIGIDOLLAR_NEVER_ACTIVATES}"


class FeatureIndexPruneTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.extra_args = [
            [DIGIDOLLAR_OFF, "-fastprune", "-prune=1", "-blockfilterindex=1", "-digidollarstatsindex=0"],
            [DIGIDOLLAR_OFF, "-fastprune", "-prune=1", "-coinstatsindex=1", "-digidollarstatsindex=0"],
            [DIGIDOLLAR_OFF, "-fastprune", "-prune=1", "-blockfilterindex=1", "-coinstatsindex=1", "-digidollarstatsindex=0"],
            [DIGIDOLLAR_OFF]
        ]

    def sync_index(self, height):
        expected_filter = {
            'basic block filter index': {'synced': True, 'best_block_height': height},
        }
        self.wait_until(
            lambda: self.nodes[0].getindexinfo() == expected_filter,
            timeout=300,
        )

        expected_stats = {
            'coinstatsindex': {'synced': True, 'best_block_height': height}
        }
        self.wait_until(
            lambda: self.nodes[1].getindexinfo() == expected_stats,
            timeout=300,
        )

        expected = {**expected_filter, **expected_stats}
        self.wait_until(
            lambda: self.nodes[2].getindexinfo() == expected,
            timeout=300,
        )

    def reconnect_nodes(self):
        self.connect_nodes(0,1)
        self.connect_nodes(0,2)
        self.connect_nodes(0,3)

    def mine_batches(self, blocks):
        n = blocks // 250
        for _ in range(n):
            self.generate(self.nodes[0], 250)
        self.generate(self.nodes[0], blocks % 250)
        self.sync_blocks()

    def restart_without_indices(self):
        for i in range(3):
            self.restart_node(i, extra_args=[DIGIDOLLAR_OFF, "-fastprune", "-prune=1", "-digidollarstatsindex=0"])
        self.reconnect_nodes()

    def run_test(self):
        filter_nodes = [self.nodes[0], self.nodes[2]]
        stats_nodes = [self.nodes[1], self.nodes[2]]

        self.log.info("check if we can access blockfilters and coinstats when pruning is enabled but no blocks are actually pruned")
        self.sync_index(height=200)
        tip = self.nodes[0].getbestblockhash()
        for node in filter_nodes:
            assert_greater_than(len(node.getblockfilter(tip)['filter']), 0)
        for node in stats_nodes:
            assert node.gettxoutsetinfo(hash_type="muhash", hash_or_height=tip)['muhash']

        self.mine_batches(500)
        self.sync_index(height=700)

        self.log.info("prune some blocks")
        for node in self.nodes[:2]:
            with node.assert_debug_log(['limited pruning to height 689']):
                pruneheight_new = node.pruneblockchain(400)
                # the prune heights used here and below are magic numbers that are determined by the
                # thresholds at which block files wrap, so they depend on disk serialization and default block file size.
                assert_equal(pruneheight_new, 249)

        self.log.info("check if we can access the tips blockfilter and coinstats when we have pruned some blocks")
        tip = self.nodes[0].getbestblockhash()
        for node in filter_nodes:
            assert_greater_than(len(node.getblockfilter(tip)['filter']), 0)
        for node in stats_nodes:
            assert node.gettxoutsetinfo(hash_type="muhash", hash_or_height=tip)['muhash']

        self.log.info("check if we can access the blockfilter and coinstats of a pruned block")
        height_hash = self.nodes[0].getblockhash(2)
        for node in filter_nodes:
            assert_greater_than(len(node.getblockfilter(height_hash)['filter']), 0)
        for node in stats_nodes:
            assert node.gettxoutsetinfo(hash_type="muhash", hash_or_height=height_hash)['muhash']

        # Mine and sync the index one block beyond the next prune height.
        # DigiByte block headers include the mining algorithm field, so the
        # fast-prune block-file boundaries differ from upstream Bitcoin's.
        self.generate(self.nodes[0], 52)
        self.sync_index(height=752)

        self.restart_without_indices()

        self.log.info("make sure trying to access the indices throws errors")
        for node in filter_nodes:
            msg = "Index is not enabled for filtertype basic"
            assert_raises_rpc_error(-1, msg, node.getblockfilter, height_hash)
        for node in stats_nodes:
            msg = "Querying specific block heights requires coinstatsindex"
            assert_raises_rpc_error(-8, msg, node.gettxoutsetinfo, "muhash", height_hash)

        self.mine_batches(581)

        self.log.info("prune exactly up to the indices best blocks while the indices are disabled")
        for i in range(3):
            pruneheight_2 = self.nodes[i].pruneblockchain(1000)
            assert_equal(pruneheight_2, 751)
            # Restart the nodes again with the indices activated
            self.restart_node(i, extra_args=self.extra_args[i])

        self.log.info("make sure that we can continue with the partially synced indices after having pruned up to the index height")
        self.sync_index(height=1333)

        self.log.info("prune further than the indices best blocks while the indices are disabled")
        self.restart_without_indices()
        self.mine_batches(1000)

        for i in range(3):
            pruneheight_3 = self.nodes[i].pruneblockchain(2000)
            assert_greater_than(pruneheight_3, pruneheight_2)
            self.stop_node(i)

        self.log.info("make sure we get an init error when starting the nodes again with the indices")
        filter_msg = "Error: basic block filter index best block of the index goes beyond pruned data. Please disable the index or reindex (which will download the whole blockchain again)"
        stats_msg = "Error: coinstatsindex best block of the index goes beyond pruned data. Please disable the index or reindex (which will download the whole blockchain again)"
        end_msg = f"{os.linesep}Error: Failed to start indexes, shutting down.."
        for i, msg in enumerate([filter_msg, stats_msg, filter_msg]):
            self.nodes[i].assert_start_raises_init_error(extra_args=self.extra_args[i], expected_msg=msg+end_msg)

        self.log.info("make sure the nodes start again with the indices and an additional -reindex arg")
        for i in range(3):
            restart_args = self.extra_args[i]+["-reindex"]
            self.restart_node(i, extra_args=restart_args)
            # The nodes need to be reconnected to the non-pruning node upon restart, otherwise they will be stuck
            self.connect_nodes(i, 3)

        # Reindexing the pruned nodes rebuilds the enabled indexes while the
        # blocks are downloaded again.  On Windows this can take longer than
        # the ordinary block-sync timeout even though all nodes keep making
        # progress on the same chain.
        self.sync_blocks(timeout=600)
        self.sync_index(height=2333)

        for node in self.nodes[:2]:
            with node.assert_debug_log(['limited pruning to height 2322']):
                pruneheight_new = node.pruneblockchain(2333)
                assert_equal(pruneheight_new, 2006)

        self.log.info("ensure that prune locks don't prevent indices from failing in a reorg scenario")
        with self.nodes[0].assert_debug_log(['basic block filter index prune lock moved back to 2313']):
            self.nodes[3].invalidateblock(self.nodes[0].getblockhash(2313))
            self.generate(self.nodes[3], 30)
            self.sync_blocks()


if __name__ == '__main__':
    FeatureIndexPruneTest().main()
