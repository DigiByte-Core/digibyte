#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Collateral estimates must report volatility restrictions separately from health."""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class DigiDollarEstimateMintRestrictionsTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-digidollaractivationheight=1", "-ddthawdayheight=500",
            "-dandelion=0", "-txindex=1", "-rpcdoccheck=1",
        ]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def check_estimate(self, reason, rule_version):
        node = self.nodes[0]
        stats = node.getdigidollarstats()
        assert_equal(stats["minting_restricted_reason"], reason)
        estimate = node.estimatecollateral(10000, 0)
        self.log.info("Estimate under rule %s: %s", rule_version, estimate)
        # This object deliberately reports only oracle and health restrictions.
        assert_equal(estimate["next_block_health"]["minting_restricted"], False)
        assert_equal(estimate["next_block_health"]["rule_version"], rule_version)
        assert_equal(estimate["minting_restricted"], reason != "none")
        assert_equal(estimate["minting_restricted_reason"], reason)
        assert_equal(estimate["mint_volatility"]["rejection_reason"], reason)
        assert_equal(estimate["mint_volatility"]["rule_version"], rule_version)
        assert estimate["required_dgb"] > 0

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 175)
        node.setmockoracleprice(500000)
        node.mintdigidollar(10000, 0)
        self.generate(node, 498 - node.getblockcount())

        self.log.info("Healthy collateral must not hide a legacy volatility freeze")
        node.setmockoracleprice(600000)
        self.check_estimate("legacy_volatility_freeze", 0)

        self.log.info("The same estimate must report the activated mint-only rule")
        self.generate(node, 1)
        node.setmockoracleprice(600000)
        self.check_estimate("volatility_pause", 1)

        self.log.info("Returning to the chain reference clears the activated restriction")
        node.setmockoracleprice(500000)
        self.check_estimate("none", 1)


if __name__ == "__main__":
    DigiDollarEstimateMintRestrictionsTest().main()
