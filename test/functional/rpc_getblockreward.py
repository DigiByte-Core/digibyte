#!/usr/bin/env python3
# Copyright (c) 2021 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal

class GetBlockRewardTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]

        # Mine some blocks
        num_blocks = 101
        node.generatetoaddress(num_blocks, node.get_deterministic_priv_key().address, invalid_call=False)

        # Test getblockreward RPC
        block_reward = node.getblockreward()
        self.log.info("Current block reward: {}".format(block_reward["blockreward"]))

        # Replace 12.5 with the expected block reward value for your test
        expected_block_reward = 72000
        assert_equal(block_reward["blockreward"], expected_block_reward)

if __name__ == '__main__':
    GetBlockRewardTest().main()
