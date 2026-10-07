#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""From the Thaw Day height on, a block gets the same answer whatever the node's sync state.

A node decides for itself whether it is still downloading the chain or has
caught up. That decision used to reach the DigiDollar checks: a node that
thought it was still downloading skipped the oracle-dependent checks, and a
node that knew it was up to date ran them. Two honest nodes could then reach
different answers about the same block, which splits the chain.

This test builds a chain above the Thaw Day height that contains the awkward
case: a redemption made while system health is below 100 per cent, so the
emergency rule decides how much must be burned. The same chain is then handed
to two nodes. One of them permanently believes it is still downloading: its
clock is fixed two hours ahead of the chain and -maxtipage=0 leaves it no
tolerance, so its tip always looks too old to have caught up. The other knows
it is up to date. Both must end on the same block with the same DigiDollar
accounting.

This test passes on the code before the repair as well. The two checks the
sync flag used to switch off cannot currently be reached, so nothing observable
changed for them; see the unit tests in
src/test/digidollar_skip_oracle_tests.cpp for the case that does change. What
this test pins is the property itself, so that a later change to the redemption
or mint rules cannot quietly make the answer depend on sync state again.
"""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class DigiDollarThawDaySyncStateTest(DigiByteTestFramework):
    THAW_HEIGHT = 100
    PRINCIPAL = 100000
    MINT_PRICE = 500000
    REDEMPTION_PRICE = 35000

    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.wallet_names = [self.default_wallet_name]
        common = [
            "-digidollaractivationheight=1",
            f"-ddthawdayheight={self.THAW_HEIGHT}",
            "-dandelion=0",
        ]
        self.common_args = common
        self.extra_args = [
            common,
            # This node gets a fixed clock ahead of the chain in run_test, which
            # together with no tolerance for tip age means it never decides it
            # has caught up.
            common + ["-maxtipage=0"],
            common,
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()

    def mine(self, count):
        return self.generate(self.nodes[0], count, sync_fun=self.no_op)

    def downloading_args(self):
        # A clock two hours ahead of the chain, so every block this node has is
        # older than the age at which it would decide it had caught up.
        tip = self.nodes[0].getbestblockhash()
        tip_time = self.nodes[0].getblockheader(tip)["time"]
        return self.common_args + ["-maxtipage=0", f"-mocktime={tip_time + 7200}"]

    def accounting(self, node):
        stats = node.getdigidollarstats()
        state = stats["canonical_health"]
        assert_equal(state["ready"], True)
        assert_equal(state["history_checked"], True)
        assert_equal(state["activation_height"], self.THAW_HEIGHT)
        return (
            node.getbestblockhash(),
            state["open_vault_principal"],
            state["collateral"],
            state["active_vaults"],
            stats["total_dd_supply"],
        )

    def run_test(self):
        producer, downloading, caught_up = self.nodes

        self.log.info("Build a chain above the Thaw Day height with an emergency redemption in it")
        self.mine(175)
        assert producer.getblockcount() > self.THAW_HEIGHT
        positions = []
        for _ in range(3):
            producer.setmockoracleprice(self.MINT_PRICE)
            position = producer.mintdigidollar(self.PRINCIPAL, 0)
            block_hash = self.mine(1)[0]
            assert position["txid"] in producer.getblock(block_hash)["tx"]
            positions.append(position)

        unlock_height = max(position["unlock_height"] for position in positions) + 1
        blocks_needed = unlock_height - producer.getblockcount()
        if blocks_needed > 0:
            self.mine(blocks_needed)

        # The lower price puts system health under 100 per cent, so this
        # redemption has to burn more than the amount originally minted. This
        # is the case whose rule used to be switched off by the sync flag.
        producer.setmockoracleprice(self.REDEMPTION_PRICE)
        redemption = producer.redeemdigidollar(positions[0]["position_id"], self.PRINCIPAL)
        assert_equal(redemption["err_active"], True)
        redemption_block = self.mine(1)[0]
        assert redemption["txid"] in producer.getblock(redemption_block)["tx"]
        expected = self.accounting(producer)
        assert_equal(expected[0], redemption_block)

        self.log.info("One node believes it is still downloading, the other knows it is up to date")
        for observer in (downloading, caught_up):
            assert_equal(observer.getblockcount(), 0)
        downloading_args = self.downloading_args()
        self.restart_node(1, downloading_args)
        assert_equal(downloading.getblockchaininfo()["initialblockdownload"], True)
        self.connect_nodes(1, 0)
        self.connect_nodes(2, 0)
        self.sync_blocks()
        assert_equal(downloading.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(caught_up.getblockchaininfo()["initialblockdownload"], False)

        self.log.info("Both reach the same block and the same DigiDollar accounting")
        assert_equal(self.accounting(downloading), expected)
        assert_equal(self.accounting(caught_up), expected)

        self.log.info("A restart does not change either node's answer")
        self.restart_node(1, downloading_args)
        self.restart_node(2, self.extra_args[2])
        assert_equal(downloading.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(self.accounting(downloading), expected)
        assert_equal(self.accounting(caught_up), expected)

        self.log.info("The node that thinks it is downloading keeps agreeing as the chain grows")
        producer.setmockoracleprice(self.MINT_PRICE)
        added = producer.mintdigidollar(self.PRINCIPAL, 0)
        added_block = self.mine(1)[0]
        assert added["txid"] in producer.getblock(added_block)["tx"]
        grown = self.accounting(producer)
        assert_equal(grown[0], added_block)
        self.connect_nodes(1, 0)
        self.connect_nodes(2, 0)
        self.sync_blocks()
        assert_equal(downloading.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(self.accounting(downloading), grown)
        assert_equal(self.accounting(caught_up), grown)
        assert_equal(
            int(Decimal(added["dgb_collateral"]) * 100000000),
            grown[2] - expected[2],
        )


if __name__ == "__main__":
    DigiDollarThawDaySyncStateTest().main()
