#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fresh nodes and interrupted database writes preserve DigiDollar accounting."""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class DigiDollarThawDayFreshSyncTest(DigiByteTestFramework):
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
        self.extra_args = [
            common + ["-digidollarstatsindex=1", "-txindex=1"],
            common + ["-digidollarstatsindex=0", "-txindex=1"],
            common + ["-digidollarstatsindex=0", "-txindex=0", "-prune=550"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()

    def assert_fresh_observers(self, genesis):
        for observer in self.nodes[1:]:
            assert_equal(observer.getblockcount(), 0)
            assert_equal(observer.getbestblockhash(), genesis)
            assert_equal(observer.getpeerinfo(), [])

    def mine(self, count):
        return self.generate(self.nodes[0], count, sync_fun=self.no_op)

    def canonical(self, node, principal, circulating, collateral, vaults):
        stats = node.getdigidollarstats()
        state = stats["canonical_health"]
        assert_equal(state["ready"], True)
        assert_equal(state["history_checked"], True)
        assert_equal(state["format_version"], 1)
        assert_equal(state["rules_version"], 1)
        assert_equal(state["activation_height"], self.THAW_HEIGHT)
        assert_equal(state["digidollar_height"], 1)
        assert_equal(state["genesis_hash"], node.getblockhash(0))
        assert_equal(state["block_hash"], node.getbestblockhash())
        assert_equal(state["open_vault_principal"], principal)
        assert_equal(state["collateral"], collateral)
        assert_equal(state["active_vaults"], vaults)
        assert_equal(stats["selected_health_denominator"], "open_vault_principal")
        assert_equal(stats["health_denominator_cents"], principal)
        assert_equal(stats["total_dd_supply"], circulating)
        return state

    def run_test(self):
        producer, full, pruned = self.nodes
        genesis = producer.getblockhash(0)
        assert_equal(producer.getblockcount(), 0)
        assert_equal(producer.getpeerinfo(), [])
        self.assert_fresh_observers(genesis)
        assert_equal(full.getblockchaininfo()["pruned"], False)
        assert_equal(pruned.getblockchaininfo()["pruned"], True)

        self.log.info("Create three activated vaults while both observers remain at genesis")
        self.mine(175)
        positions = []
        for _ in range(3):
            producer.setmockoracleprice(self.MINT_PRICE)
            position = producer.mintdigidollar(self.PRINCIPAL, 0)
            block_hash = self.mine(1)[0]
            assert position["txid"] in producer.getblock(block_hash)["tx"]
            positions.append(position)
        collateral = sum(
            int(Decimal(position["dgb_collateral"]) * 100000000)
            for position in positions
        )
        self.canonical(producer, 300000, 300000, collateral, 3)

        unlock_height = max(position["unlock_height"] for position in positions) + 1
        blocks_needed = unlock_height - producer.getblockcount()
        if blocks_needed > 0:
            self.mine(blocks_needed)
        self.log.info("Recover a real partial UTXO write that spends a vault and burns extra tokens")
        self.restart_node(0, self.extra_args[0] + ["-dbbatchsize=1", "-dbcrashratio=1"])
        producer.setmockoracleprice(self.REDEMPTION_PRICE)
        redemption = producer.redeemdigidollar(positions[0]["position_id"], self.PRINCIPAL)
        assert_equal(redemption["err_active"], True)
        assert_equal(redemption["required_dd_burn"], 125000)
        redemption_block = self.mine(1)[0]
        assert redemption["txid"] in producer.getblock(redemption_block)["tx"]
        remaining_collateral = collateral - int(Decimal(positions[0]["dgb_collateral"]) * 100000000)
        expected = self.canonical(producer, 200000, 175000, remaining_collateral, 2)
        with producer.assert_debug_log(["Writing partial batch", "Simulating a crash. Goodbye."]):
            self.stop_node(0)
        self.start_node(0, self.extra_args[0])
        assert_equal(producer.getpeerinfo(), [])
        assert_equal(producer.getbestblockhash(), redemption_block)
        assert_equal(self.canonical(producer, 200000, 175000, remaining_collateral, 2), expected)

        self.log.info("Fresh observers validate the entire retained history from genesis")
        self.assert_fresh_observers(genesis)
        self.connect_nodes(1, 0)
        self.connect_nodes(2, 0)
        self.sync_all()
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), redemption_block)
            assert_equal(self.canonical(node, 200000, 175000, remaining_collateral, 2), expected)

        self.log.info("The pruning-configured observer restores the same state before reconnecting")
        self.disconnect_nodes(2, 0)
        self.restart_node(2, self.extra_args[2])
        assert_equal(pruned.getpeerinfo(), [])
        assert_equal(pruned.getblockchaininfo()["pruned"], True)
        assert_equal(pruned.getbestblockhash(), redemption_block)
        assert_equal(self.canonical(pruned, 200000, 175000, remaining_collateral, 2), expected)
        self.connect_nodes(2, 0)
        self.sync_all()
        assert_equal(self.canonical(pruned, 200000, 175000, remaining_collateral, 2), expected)

        self.log.info("Recover a real partial UTXO write containing a new vault")
        self.disconnect_nodes(1, 0)
        self.disconnect_nodes(2, 0)
        self.restart_node(0, self.extra_args[0] + ["-dbbatchsize=1", "-dbcrashratio=1"])
        producer.setmockoracleprice(self.MINT_PRICE)
        added = producer.mintdigidollar(self.PRINCIPAL, 0)
        added_block = self.mine(1)[0]
        assert added["txid"] in producer.getblock(added_block)["tx"]
        final_collateral = remaining_collateral + int(Decimal(added["dgb_collateral"]) * 100000000)
        final_state = self.canonical(producer, 300000, 275000, final_collateral, 3)
        # The existing database test option exits immediately after the first
        # partial batch, before committing the UTXO tip and health together.
        with producer.assert_debug_log(["Writing partial batch", "Simulating a crash. Goodbye."]):
            self.stop_node(0)

        self.start_node(0, self.extra_args[0])
        assert_equal(producer.getpeerinfo(), [])
        assert_equal(producer.getbestblockhash(), added_block)
        assert_equal(self.canonical(producer, 300000, 275000, final_collateral, 3), final_state)
        self.connect_nodes(1, 0)
        self.connect_nodes(2, 0)
        self.sync_all()
        for observer in (full, pruned):
            assert_equal(self.canonical(observer, 300000, 275000, final_collateral, 3), final_state)
            assert_equal(observer.gettxoutsetinfo("muhash")["muhash"],
                         producer.gettxoutsetinfo("muhash")["muhash"])


if __name__ == "__main__":
    DigiDollarThawDayFreshSyncTest().main()
