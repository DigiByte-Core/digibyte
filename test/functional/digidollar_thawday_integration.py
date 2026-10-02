#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check ordinary DigiDollar activity, accounting and upgrade across Thaw Day."""

from decimal import Decimal

from test_framework.messages import tx_from_hex
from test_framework.script import CScript, CScriptNum, OP_RETURN
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class DigiDollarThawDayIntegrationTest(DigiByteTestFramework):
    THAW_HEIGHT = 200
    PRICE = 500000
    PRINCIPAL = 100000

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common = ["-digidollaractivationheight=1", "-dandelion=0", "-txindex=1"]
        self.extra_args = [
            common + [f"-ddthawdayheight={self.THAW_HEIGHT}", "-digidollarstatsindex=1"],
            common + ["-digidollarstatsindex=0"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)
        parser.add_argument("--previous-binary", help="Optional previous daemon for the observer before upgrading it")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_nodes(self):
        self.add_nodes(self.num_nodes, self.extra_args, binary=[
            self.options.digibyted, self.options.previous_binary or self.options.digibyted,
        ])
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()

    def quote(self, price):
        for node in self.nodes:
            node.setmockoracleprice(price)

    def mine_to(self, height):
        count = height - self.nodes[0].getblockcount()
        assert count >= 0
        if count:
            self.generate(self.nodes[0], count)

    def canonical(self, node, principal, circulating, collateral, vaults):
        stats = node.getdigidollarstats()
        state = stats["canonical_health"]
        assert_equal(state["ready"], True)
        assert_equal(state["history_checked"], True)
        assert_equal(state["format_version"], 1)
        assert_equal(state["rules_version"], 1)
        assert_equal(state["activation_height"], self.THAW_HEIGHT)
        assert_equal(state["digidollar_height"], 1)
        assert_equal(state["block_hash"], node.getbestblockhash())
        assert_equal(state["genesis_hash"], node.getblockhash(0))
        assert_equal(state["open_vault_principal"], principal)
        assert_equal(state["collateral"], collateral)
        assert_equal(state["active_vaults"], vaults)
        assert_equal(stats["selected_health_denominator"], "open_vault_principal")
        assert_equal(stats["health_denominator_cents"], principal)
        assert_equal(stats["total_dd_supply"], circulating)
        return state

    def redemption_change(self, node, txid):
        tx = tx_from_hex(node.getrawtransaction(txid))
        token_outputs = [
            i for i, output in enumerate(tx.vout)
            if output.nValue == 0 and len(output.scriptPubKey) == 34
            and output.scriptPubKey[:2] == b"\x51\x20"
        ]
        assert_equal(len(token_outputs), 1)
        metadata = []
        for output in tx.vout:
            if output.scriptPubKey[:1] != bytes([OP_RETURN]):
                continue
            fields = list(CScript(output.scriptPubKey))
            if len(fields) >= 2 and fields[1] == b"DD":
                metadata.append(fields)
        assert_equal(len(metadata), 1)
        fields = metadata[0]
        assert len(fields) >= 4

        def script_number(value):
            return value if isinstance(value, int) else CScriptNum.decode(bytes([len(value)]) + value)

        assert_equal(script_number(fields[2]), 3)
        amount = script_number(fields[3])
        assert amount > 0
        assert node.gettxout(txid, token_outputs[0]) is not None
        return token_outputs[0], amount

    def run_test(self):
        owner, observer = self.nodes
        self.generate(owner, 175)
        self.quote(self.PRICE)

        self.log.info("Create open vaults before activation with both nodes on the legacy rules")
        positions = []
        for _ in range(3):
            self.quote(self.PRICE)
            position = owner.mintdigidollar(self.PRINCIPAL, 0)
            positions.append(position)
            self.generate(owner, 1)
            assert position["txid"] in owner.getblock(owner.getbestblockhash())["tx"]
        collateral = sum(
            int(Decimal(position["dgb_collateral"]) * 100000000)
            for position in positions
        )

        self.mine_to(self.THAW_HEIGHT - 1)
        self.quote(self.PRICE)
        status = owner.getprotectionstatus()["volatility"]
        assert_equal(status["tip_height"], self.THAW_HEIGHT - 1)
        assert_equal(status["candidate_height"], self.THAW_HEIGHT)
        assert_equal(status["tip_active"], False)
        assert_equal(status["next_block_active"], True)
        assert_equal(status["rule_version"], 1)
        assert_equal(status["all_operations_restricted"], False)
        observer_deployment = observer.getdigidollardeploymentinfo()
        if self.options.previous_binary:
            assert "thaw_day" not in observer_deployment
        else:
            assert_equal(observer_deployment["thaw_day"]["scheduled"], False)

        self.log.info("The first activated block derives accounting from its actual parent")
        self.generate(owner, 1)
        at_activation = self.canonical(owner, 300000, 300000, collateral, 3)
        self.mine_to(self.THAW_HEIGHT + 5)

        self.log.info("Install the height on a node that already holds a valid post-activation segment")
        self.extra_args[1].append(f"-ddthawdayheight={self.THAW_HEIGHT}")
        if self.options.previous_binary:
            executable = self.nodes[1].args.index(self.options.previous_binary)
            self.nodes[1].args[executable] = self.options.digibyted
        self.restart_node(1, self.extra_args[1])
        self.connect_nodes(0, 1)
        self.sync_all()
        assert_equal(owner.getbestblockhash(), observer.getbestblockhash())
        assert_equal(
            self.canonical(owner, 300000, 300000, collateral, 3),
            self.canonical(observer, 300000, 300000, collateral, 3),
        )

        self.log.info("Verification and a short reorg leave the canonical state tied to its block")
        for node in self.nodes:
            assert_equal(node.verifychain(4, 35), True)
        self.disconnect_nodes(0, 1)
        old_tip = owner.getbestblockhash()
        owner.invalidateblock(old_tip)
        self.canonical(owner, 300000, 300000, collateral, 3)
        owner.reconsiderblock(old_tip)
        assert_equal(owner.getbestblockhash(), old_tip)

        self.log.info("A cold activated node can return below the activation boundary")
        self.restart_node(0, self.extra_args[0])
        self.canonical(owner, 300000, 300000, collateral, 3)
        activation_hash = owner.getblockhash(self.THAW_HEIGHT)
        owner.invalidateblock(activation_hash)
        assert_equal(owner.getblockcount(), self.THAW_HEIGHT - 1)
        self.restart_node(0, self.extra_args[0])
        boundary = owner.getdigidollardeploymentinfo()["thaw_day"]
        assert_equal(boundary["active_at_tip"], False)
        assert_equal(boundary["active_next_block"], True)
        owner.reconsiderblock(activation_hash)
        assert_equal(owner.getbestblockhash(), old_tip)
        self.canonical(owner, 300000, 300000, collateral, 3)
        self.connect_nodes(0, 1)
        self.sync_all()
        assert_equal(at_activation["block_hash"], owner.getblockhash(self.THAW_HEIGHT))

        self.quote(self.PRICE)
        transfer = owner.senddigidollar(observer.getdigidollaraddress(), 10000)
        self.generate(owner, 1)
        assert transfer["txid"] in owner.getblock(owner.getbestblockhash())["tx"]
        assert_equal(observer.getdigidollarbalance()["confirmed"], 10000)

        self.mine_to(max(position["unlock_height"] for position in positions) + 1)
        self.log.info("Wallet emergency redemption uses current canonical health without a preceding stats RPC")
        self.quote(35000)
        redemption = owner.redeemdigidollar(positions[0]["position_id"], self.PRINCIPAL)
        assert_equal(redemption["required_dd_burn"], 125000)
        assert_equal(redemption["err_active"], True)
        self.generate(owner, 1)
        assert redemption["txid"] in owner.getblock(owner.getbestblockhash())["tx"]
        serialized_change = self.redemption_change(owner, redemption["txid"])
        remaining_collateral = collateral - int(Decimal(positions[0]["dgb_collateral"]) * 100000000)
        before_restart = self.canonical(owner, 200000, 175000, remaining_collateral, 2)
        assert_equal(
            before_restart,
            self.canonical(observer, 200000, 175000, remaining_collateral, 2),
        )

        self.log.info("Undo restores the closed vault principal and all tokens burned by its redemption")
        self.disconnect_nodes(0, 1)
        redemption_block = owner.getbestblockhash()
        owner.invalidateblock(redemption_block)
        self.canonical(owner, 300000, 300000, collateral, 3)
        owner.reconsiderblock(redemption_block)
        assert_equal(owner.getbestblockhash(), redemption_block)
        assert_equal(self.canonical(owner, 200000, 175000, remaining_collateral, 2), before_restart)
        self.connect_nodes(0, 1)
        self.sync_all()

        self.log.info("Both supply definitions survive restart, with and without the optional stats index")
        self.restart_node(0, self.extra_args[0])
        self.restart_node(1, self.extra_args[1])
        self.connect_nodes(0, 1)
        self.sync_all()
        for node in self.nodes:
            assert_equal(self.redemption_change(node, redemption["txid"]), serialized_change)
            assert_equal(self.canonical(node, 200000, 175000, remaining_collateral, 2), before_restart)
            assert_equal(node.verifychain(4, 6), True)
            assert_equal(self.canonical(node, 200000, 175000, remaining_collateral, 2), before_restart)


if __name__ == "__main__":
    DigiDollarThawDayIntegrationTest().main()
