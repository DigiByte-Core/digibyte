#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Five kinds of node agree on DigiDollar accounting, and reorgs across Thaw Day return to the same figures.

One regtest chain is built with DigiDollar active from height 200 and Thaw Day
at height 300. It carries mints below and above Thaw Day, DigiDollar sends, one
ordinary redemption and one emergency redemption, so the number of tokens left
in circulation is deliberately smaller than the amount originally minted
against the vaults that are still open.

Five nodes are then compared at one named block hash:

  0  continuous  - mined the chain and was never restarted
  1  restarted   - plain restart, no reindex
  2  reindexed   - restarted with -reindex, so it validated the whole history again
  3  fresh sync  - started from an empty directory and downloaded everything
  4  pruned      - runs with -prune, which also turns off both optional indexes

The comparison reads the accounting twice on every node. Once from
getprotectionstatus, which takes the numbers from the chainstate and the
next block's quote and never looks at the statistics index, and once from
getdigidollarstats, which may read that index. Matching statistics totals on
their own would not be evidence, because the index is a separate copy of the
numbers. The number of tokens in circulation is checked on its own, because it
is a different total from the open-vault principal and does not have to equal
it.

The last part rewinds the chain three ways - to a tip above Thaw Day, to
exactly the Thaw Day block, and back below Thaw Day - restarts the node while
it is rewound, then puts the blocks back and checks every figure returns to
what it was.
"""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error


COIN = 100000000
CONTINUOUS, RESTARTED, REINDEXED, FRESH, PRUNED = range(5)
NAMES = ["continuous", "restarted", "reindexed", "fresh-sync", "pruned"]


def expected_health(collateral_sat, liability_cents, price_micro_usd):
    """The integer health figure the node must report for these three inputs.

    This repeats the consensus arithmetic in Python so the test proves the five
    nodes agree with the formula, not just with each other. Collateral is in
    satoshis, the liability is in cents, and the quote is converted from
    micro-USD to millicents by integer division by ten, exactly as validation
    does.
    """
    price_millicents = price_micro_usd // 10
    if price_millicents <= 0 or collateral_sat < 0 or liability_cents < 0:
        return 0
    if liability_cents == 0:
        return 30000
    value = (collateral_sat * price_millicents * 100) // (COIN * 1000 * liability_cents)
    if value <= 0:
        return 0
    return min(value, 30000)


class DigiDollarThawDayNodeMatrixTest(DigiByteTestFramework):
    # The DigiDollar height is set well above the first 64 kilobytes of block
    # file so the pruned node has whole pre-DigiDollar block files it is allowed
    # to delete, while the DigiDollar-era window it must keep stays intact.
    DD_HEIGHT = 400
    THAW_HEIGHT = 500
    PRINCIPAL = 100000      # $1000.00 per vault, in cents
    TRANSFER = 10000        # $100.00 per send, in cents
    HEALTHY_PRICE = 500000  # $0.50 per DGB, in micro-USD
    ERR_PRICE = 35000       # $0.035 per DGB, in micro-USD

    def set_test_params(self):
        self.num_nodes = 5
        self.setup_clean_chain = True
        common = [
            f"-digidollaractivationheight={self.DD_HEIGHT}",
            f"-ddthawdayheight={self.THAW_HEIGHT}",
            "-dandelion=0",
        ]
        self.extra_args = [
            common + ["-txindex=1", "-digidollarstatsindex=1"],
            common + ["-txindex=1", "-digidollarstatsindex=0"],
            common + ["-txindex=1", "-digidollarstatsindex=1"],
            common + ["-txindex=1", "-digidollarstatsindex=0"],
            # A pruned node turns the transaction index and the DigiDollar
            # statistics index off by itself, so this is the only supported way
            # to run DigiDollar without the transaction index.
            common + ["-prune=550", "-fastprune"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # No automatic connections: the fresh-sync node must stay at genesis
        # until the rest of the chain exists.
        self.setup_nodes()
        for peer in (RESTARTED, REINDEXED, PRUNED):
            self.connect_nodes(peer, CONTINUOUS)

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------
    def prime(self, price, nodes=None):
        """Give the chosen nodes the same clock and the same quote.

        The health figure a node reports is the one for the next block, so it
        depends on the quote that node holds. Setting the same mock clock and
        the same mock price everywhere is what makes the five nodes comparable:
        they are then all using the same quote.
        """
        self.price = price
        for node in (self.nodes if nodes is None else nodes):
            node.setmocktime(self.t)
            # The quote RPC only exists once DigiDollar is active on that
            # node's own chain, so a node still sitting at genesis is skipped.
            if node.getblockcount() >= self.DD_HEIGHT:
                node.setmockoracleprice(price)

    def tick(self, seconds=60, blocks=0):
        self.t += seconds
        for node in self.nodes:
            node.setmocktime(self.t)
        if blocks:
            self.mine(blocks)

    def mine(self, count):
        return self.generate(self.nodes[CONTINUOUS], count, sync_fun=self.no_op)

    def mine_to(self, height):
        count = height - self.nodes[CONTINUOUS].getblockcount()
        assert count >= 0, f"chain is already past height {height}"
        if count:
            self.mine(count)

    def chainstate_view(self, node):
        """The accounting as the chainstate and the next block's quote give it.

        getprotectionstatus builds this from the chainstate record and the quote
        for the next block. It never consults the DigiDollar statistics index,
        so two nodes agreeing here are agreeing about consensus state and not
        about a copy of it held in an index.
        """
        block = node.getprotectionstatus()["next_block_health"]
        record = block["canonical_health"]
        return {
            "candidate_height": block["candidate_height"],
            "rule_version": block["rule_version"],
            "ready": block["ready"],
            "denominator_name": block["selected_health_denominator"],
            "denominator_cents": block["health_denominator_cents"],
            "health": block["health_percentage"],
            "price": block["oracle_price_micro_usd"],
            "record_ready": record["ready"],
            "principal": record.get("open_vault_principal"),
            "collateral": record.get("collateral"),
            "vaults": record.get("active_vaults"),
            "record_block": record.get("block_hash"),
            "history_checked": record.get("history_checked"),
            "activation_height": record.get("activation_height"),
            "digidollar_height": record.get("digidollar_height"),
        }

    @staticmethod
    def accounting_only(view):
        """The same reading with the quote and the health figure removed.

        A node keeps the oracle sessions it has collected in memory, and it
        loses them when it stops. So a node that is restarted while sitting on
        a given block can legitimately pick a different, still valid, quote for
        the next block than the node that has been running all along. The
        amounts that come out of the chainstate must not move; the health
        figure follows the quote and is checked against it separately.
        """
        return {key: value for key, value in view.items() if key not in ("health", "price")}

    def stats_view(self, node):
        """The accounting as the statistics RPC gives it, plus the token total.

        Above Thaw Day the canonical_health block here comes from the chainstate
        too, but total_dd_supply - the number of tokens left in circulation -
        can come from the optional statistics index. That is why it is compared
        on its own and against nodes that do not run the index.
        """
        stats = node.getdigidollarstats()
        record = stats["canonical_health"]
        return {
            "principal": record.get("open_vault_principal"),
            "collateral": record.get("collateral"),
            "vaults": record.get("active_vaults"),
            "record_block": record.get("block_hash"),
            "denominator_name": stats["selected_health_denominator"],
            "denominator_cents": stats["health_denominator_cents"],
            "circulating": stats["total_dd_supply"],
            "positions": stats["active_positions"],
            "collateral_dgb": stats["total_collateral_locked"],
        }

    def activated_state(self, node, label, principal, circulating, collateral, vaults):
        """Check one activated node against explicit expected totals."""
        tip = node.getbestblockhash()
        chain = self.chainstate_view(node)
        stats = self.stats_view(node)

        assert_equal(chain["rule_version"], 1)
        assert_equal(chain["ready"], True)
        assert_equal(chain["record_ready"], True)
        assert_equal(chain["history_checked"], True)
        assert_equal(chain["activation_height"], self.THAW_HEIGHT)
        assert_equal(chain["digidollar_height"], self.DD_HEIGHT)
        assert_equal(chain["record_block"], tip)
        assert_equal(chain["denominator_name"], "open_vault_principal")

        # The amount the health figure is calculated from, and the three
        # canonical quantities it comes from.
        assert_equal(chain["principal"], principal)
        assert_equal(chain["denominator_cents"], principal)
        assert_equal(chain["collateral"], collateral)
        assert_equal(chain["vaults"], vaults)

        # The same numbers again through the statistics RPC, and the token
        # total on its own.
        assert_equal(stats["principal"], principal)
        assert_equal(stats["collateral"], collateral)
        assert_equal(stats["vaults"], vaults)
        assert_equal(stats["record_block"], tip)
        assert_equal(stats["denominator_name"], "open_vault_principal")
        assert_equal(stats["denominator_cents"], principal)
        assert_equal(stats["circulating"], circulating)
        assert_equal(stats["positions"], vaults)
        assert_equal(int(Decimal(stats["collateral_dgb"]) * COIN), collateral)

        # The health figure has to follow from the quote actually used.
        assert_equal(chain["health"], expected_health(collateral, principal, chain["price"]))

        self.log.info(
            f"  {label}: height {node.getblockcount()} principal {principal} cents, "
            f"tokens {circulating} cents, collateral {collateral} sat, {vaults} vaults, "
            f"health {chain['health']}% at quote {chain['price']}")
        return {"tip": tip, "chain": chain, "stats": stats}

    def legacy_state(self, node, label, circulating, collateral, positions):
        """Check one node whose tip is below Thaw Day, on the old rules."""
        tip_height = node.getblockcount()
        assert tip_height < self.THAW_HEIGHT
        stats = self.stats_view(node)
        assert_equal(stats["circulating"], circulating)
        assert_equal(stats["positions"], positions)
        assert_equal(int(Decimal(stats["collateral_dgb"]) * COIN), collateral)
        boundary = node.getdigidollardeploymentinfo()["thaw_day"]
        assert_equal(boundary["scheduled"], True)
        assert_equal(boundary["height"], self.THAW_HEIGHT)
        assert_equal(boundary["active_at_tip"], False)
        assert_equal(boundary["active_next_block"], tip_height + 1 >= self.THAW_HEIGHT)
        self.log.info(
            f"  {label}: height {tip_height} below Thaw Day, tokens {circulating} cents, "
            f"collateral {collateral} sat, {positions} positions")
        return {"tip": node.getbestblockhash(), "stats": stats, "boundary": boundary}

    # ------------------------------------------------------------------
    # the chain
    # ------------------------------------------------------------------
    def build_chain(self):
        producer = self.nodes[CONTINUOUS]
        self.t = 1700000000
        self.price = self.HEALTHY_PRICE
        for node in self.nodes:
            node.setmocktime(self.t)

        # The mock quote RPC is itself gated on DigiDollar being active, so the
        # pre-DigiDollar history is mined before any quote exists.
        self.log.info(f"Mine up to the DigiDollar height {self.DD_HEIGHT} and leave the fresh-sync node at genesis")
        self.mine_to(self.DD_HEIGHT)
        assert_equal(self.nodes[FRESH].getblockcount(), 0)
        self.sync_blocks([self.nodes[i] for i in (CONTINUOUS, RESTARTED, REINDEXED, PRUNED)])
        self.prime(self.HEALTHY_PRICE)

        self.log.info("Two vaults below Thaw Day, on the old rules")
        positions = []
        for _ in range(2):
            self.prime(self.HEALTHY_PRICE)
            position = producer.mintdigidollar(self.PRINCIPAL, 0)
            block = self.mine(1)[0]
            assert position["txid"] in producer.getblock(block)["tx"]
            positions.append(position)
            self.tick(60)
        assert producer.getblockcount() < self.THAW_HEIGHT

        self.log.info("Send DigiDollar below Thaw Day")
        self.prime(self.HEALTHY_PRICE)
        transfer = producer.senddigidollar(self.nodes[RESTARTED].getdigidollaraddress(), self.TRANSFER)
        block = self.mine(1)[0]
        assert transfer["txid"] in producer.getblock(block)["tx"]

        self.log.info(f"Stop one block short of Thaw Day at height {self.THAW_HEIGHT - 1}")
        self.mine_to(self.THAW_HEIGHT - 1)
        self.prime(self.HEALTHY_PRICE)
        boundary = producer.getdigidollardeploymentinfo()["thaw_day"]
        assert_equal(boundary["active_at_tip"], False)
        assert_equal(boundary["active_next_block"], True)

        # Keep the old-rules figures for this block. A later reorg comes back
        # across Thaw Day to this exact tip and must report them again.
        below = self.stats_view(producer)
        self.legacy_collateral = int(Decimal(below["collateral_dgb"]) * COIN)
        self.legacy_positions = below["positions"]
        self.legacy_circulating = below["circulating"]
        assert_equal(self.legacy_circulating, 200000)
        assert_equal(self.legacy_positions, 2)

        self.log.info(f"Mine the first Thaw Day block at height {self.THAW_HEIGHT}")
        self.activation_hash = self.mine(1)[0]
        assert_equal(producer.getblockcount(), self.THAW_HEIGHT)
        collateral = sum(int(Decimal(p["dgb_collateral"]) * COIN) for p in positions)
        self.activated_state(producer, "at Thaw Day", 200000, 200000, collateral, 2)

        self.log.info("A third vault and another send above Thaw Day")
        self.prime(self.HEALTHY_PRICE)
        position = producer.mintdigidollar(self.PRINCIPAL, 0)
        block = self.mine(1)[0]
        assert position["txid"] in producer.getblock(block)["tx"]
        positions.append(position)
        collateral = sum(int(Decimal(p["dgb_collateral"]) * COIN) for p in positions)
        self.tick(60)
        self.prime(self.HEALTHY_PRICE)
        transfer = producer.senddigidollar(self.nodes[RESTARTED].getdigidollaraddress(), self.TRANSFER)
        block = self.mine(1)[0]
        assert transfer["txid"] in producer.getblock(block)["tx"]
        self.activated_state(producer, "three vaults", 300000, 300000, collateral, 3)

        self.log.info("Wait out the locks on the first two vaults")
        self.mine_to(max(positions[0]["unlock_height"], positions[1]["unlock_height"]) + 1)
        self.tick(60)

        self.log.info("Ordinary redemption: burn exactly the amount the vault minted")
        self.prime(self.HEALTHY_PRICE)
        ordinary = producer.redeemdigidollar(positions[0]["position_id"], self.PRINCIPAL)
        assert_equal(ordinary["err_active"], False)
        assert_equal(ordinary["required_dd_burn"], self.PRINCIPAL)
        block = self.mine(1)[0]
        assert ordinary["txid"] in producer.getblock(block)["tx"]
        collateral -= int(Decimal(positions[0]["dgb_collateral"]) * COIN)
        self.activated_state(producer, "after ordinary redemption", 200000, 200000, collateral, 2)

        self.log.info("Emergency redemption: the burn is larger than the vault's own amount")
        self.prime(self.ERR_PRICE)
        self.tick(60, blocks=2)
        self.prime(self.ERR_PRICE)
        emergency = producer.redeemdigidollar(positions[1]["position_id"], self.PRINCIPAL)
        assert_equal(emergency["err_active"], True)
        assert_greater_than(emergency["required_dd_burn"], self.PRINCIPAL)
        self.emergency_burn = emergency["required_dd_burn"]
        block = self.mine(1)[0]
        assert emergency["txid"] in producer.getblock(block)["tx"]
        collateral -= int(Decimal(positions[1]["dgb_collateral"]) * COIN)

        # One vault is left. It still holds the full amount it minted, even
        # though the emergency redemption destroyed more tokens than the vault
        # it closed had minted. The extra tokens are gone from circulation and
        # are not recreated.
        self.expected_principal = self.PRINCIPAL
        self.expected_collateral = collateral
        self.expected_vaults = 1
        self.expected_circulating = 300000 - self.PRINCIPAL - self.emergency_burn
        assert_greater_than(self.expected_principal, self.expected_circulating)
        self.log.info(
            f"Emergency burn was {self.emergency_burn} cents, so {self.expected_circulating} cents "
            f"of tokens are left while the one open vault still counts {self.expected_principal} cents")

        self.matrix_hash = producer.getbestblockhash()
        self.matrix_height = producer.getblockcount()
        return self.activated_state(producer, "named block", self.expected_principal,
                                    self.expected_circulating, self.expected_collateral,
                                    self.expected_vaults)

    # ------------------------------------------------------------------
    # the five kinds of node
    # ------------------------------------------------------------------
    def compare_matrix(self, reference, stage):
        producer = self.nodes[CONTINUOUS]
        self.log.info(f"Node matrix ({stage}) at block {self.matrix_hash} height {self.matrix_height}")

        self.sync_blocks([self.nodes[i] for i in (CONTINUOUS, RESTARTED, REINDEXED, PRUNED)])

        self.log.info("Restart one node without a reindex")
        self.restart_node(RESTARTED, self.extra_args[RESTARTED])
        self.connect_nodes(RESTARTED, CONTINUOUS)

        self.log.info("Reindex the whole history on another node")
        self.restart_node(REINDEXED, self.extra_args[REINDEXED] + ["-reindex"])
        self.connect_nodes(REINDEXED, CONTINUOUS)
        # Proof it really reindexed rather than reopening its databases.
        with open(self.nodes[REINDEXED].debug_log_path, encoding="utf-8", errors="replace") as log:
            assert "Reindexing block file blk00000.dat" in log.read()

        self.log.info("Prune the node that runs with -prune, then restart it")
        pruned = self.nodes[PRUNED]
        assert_equal(pruned.getblockchaininfo()["pruned"], True)
        pruned.pruneblockchain(self.DD_HEIGHT - 1)
        self.prune_height = pruned.getblockchaininfo()["pruneheight"]
        # The DigiDollar-era window has to stay, because a redemption reads the
        # block that created the vault it is closing. Anything the node did
        # delete has to be below the DigiDollar height.
        assert self.prune_height <= self.DD_HEIGHT, f"pruned into the DigiDollar era at {self.prune_height}"
        assert_equal(pruned.getblock(pruned.getblockhash(self.DD_HEIGHT))["height"], self.DD_HEIGHT)
        if self.prune_height > 0:
            assert_raises_rpc_error(-1, "Block not available (pruned data)",
                                    pruned.getblock, pruned.getblockhash(self.prune_height - 1))
            self.log.info(f"  pruned node deleted the blocks below height {self.prune_height} "
                          f"and kept the DigiDollar era from {self.DD_HEIGHT}")
        else:
            self.log.info("  pruned node deleted nothing: no whole block file lies below the "
                          "DigiDollar-era window it has to keep")
        self.restart_node(PRUNED, self.extra_args[PRUNED])
        self.connect_nodes(PRUNED, CONTINUOUS)

        if self.nodes[FRESH].getblockcount() == 0:
            self.log.info("Let the empty node download and validate the whole chain")
            self.connect_nodes(FRESH, CONTINUOUS)
        self.sync_blocks()

        self.prime(self.price)
        views = {}
        for index, node in enumerate(self.nodes):
            assert_equal(node.getbestblockhash(), self.matrix_hash)
            views[index] = self.activated_state(
                node, NAMES[index], self.expected_principal, self.expected_circulating,
                self.expected_collateral, self.expected_vaults)

        # Every node has to produce the same figures, not merely figures that
        # each pass the same thresholds.
        for index in range(1, self.num_nodes):
            assert_equal(views[index]["chain"], views[CONTINUOUS]["chain"])
            assert_equal(views[index]["stats"], views[CONTINUOUS]["stats"])
        if reference is not None:
            assert_equal(self.accounting_only(views[CONTINUOUS]["chain"]),
                         self.accounting_only(reference["chain"]))
            assert_equal(views[CONTINUOUS]["stats"], reference["stats"])

        # The unspent output set is the same too, which is what the accounting
        # is derived from.
        muhash = producer.gettxoutsetinfo("muhash")["muhash"]
        for index, node in enumerate(self.nodes):
            assert_equal(node.gettxoutsetinfo("muhash")["muhash"], muhash)
        return views[CONTINUOUS]

    # ------------------------------------------------------------------
    # reorgs across the boundary
    # ------------------------------------------------------------------
    def reorg_checks(self, reference):
        producer = self.nodes[CONTINUOUS]
        self.log.info("Take the mining node off the network for the reorg checks")
        for peer in (RESTARTED, REINDEXED, FRESH, PRUNED):
            self.disconnect_nodes(peer, CONTINUOUS)

        # A reorg that stays wholly above Thaw Day.
        above = producer.getblockhash(self.matrix_height - 1)
        self.log.info(f"Rewind to height {self.matrix_height - 2}, wholly above Thaw Day")
        producer.invalidateblock(above)
        assert_equal(producer.getblockcount(), self.matrix_height - 2)
        assert producer.getblockcount() > self.THAW_HEIGHT
        self.prime(self.price, [producer])
        rewound_above = self.chainstate_view(producer)
        assert_equal(rewound_above["rule_version"], 1)
        assert_equal(rewound_above["record_block"], producer.getbestblockhash())
        assert_equal(rewound_above["history_checked"], True)
        self.log.info("Restart while rewound above Thaw Day")
        self.restart_node(CONTINUOUS, self.extra_args[CONTINUOUS])
        self.prime(self.price, [producer])
        after = self.chainstate_view(producer)
        assert_equal(self.accounting_only(after), self.accounting_only(rewound_above))
        assert_equal(after["health"], expected_health(after["collateral"], after["principal"], after["price"]))
        producer.reconsiderblock(above)
        assert_equal(producer.getbestblockhash(), self.matrix_hash)
        self.prime(self.price, [producer])
        self.activated_state(producer, "back from a reorg above Thaw Day", self.expected_principal,
                             self.expected_circulating, self.expected_collateral, self.expected_vaults)

        # A reorg whose new tip is exactly the Thaw Day block.
        first_after = producer.getblockhash(self.THAW_HEIGHT + 1)
        self.log.info(f"Rewind so the tip is exactly the Thaw Day block, height {self.THAW_HEIGHT}")
        producer.invalidateblock(first_after)
        assert_equal(producer.getblockcount(), self.THAW_HEIGHT)
        assert_equal(producer.getbestblockhash(), self.activation_hash)
        self.prime(self.price, [producer])
        at_boundary = self.chainstate_view(producer)
        assert_equal(at_boundary["rule_version"], 1)
        assert_equal(at_boundary["record_block"], self.activation_hash)
        assert_equal(at_boundary["history_checked"], True)
        assert_equal(at_boundary["principal"], 200000)
        assert_equal(at_boundary["vaults"], 2)
        self.log.info("Restart while the tip is the Thaw Day block")
        self.restart_node(CONTINUOUS, self.extra_args[CONTINUOUS])
        self.prime(self.price, [producer])
        after = self.chainstate_view(producer)
        assert_equal(self.accounting_only(after), self.accounting_only(at_boundary))
        assert_equal(after["health"], expected_health(after["collateral"], after["principal"], after["price"]))
        producer.reconsiderblock(first_after)
        assert_equal(producer.getbestblockhash(), self.matrix_hash)
        self.prime(self.price, [producer])
        self.activated_state(producer, "back from a reorg ending on Thaw Day", self.expected_principal,
                             self.expected_circulating, self.expected_collateral, self.expected_vaults)

        # A reorg that crosses the boundary: the new tip is below Thaw Day.
        self.log.info(f"Rewind across Thaw Day to height {self.THAW_HEIGHT - 1}")
        producer.invalidateblock(self.activation_hash)
        assert_equal(producer.getblockcount(), self.THAW_HEIGHT - 1)
        self.prime(self.price, [producer])
        below = self.legacy_state(producer, "rewound below Thaw Day", self.legacy_circulating,
                                  self.legacy_collateral, self.legacy_positions)
        self.log.info("Restart while the tip is below Thaw Day")
        self.restart_node(CONTINUOUS, self.extra_args[CONTINUOUS])
        self.prime(self.price, [producer])
        assert_equal(self.legacy_state(producer, "restarted below Thaw Day", self.legacy_circulating,
                                       self.legacy_collateral, self.legacy_positions), below)
        self.log.info("Put the activated blocks back")
        producer.reconsiderblock(self.activation_hash)
        assert_equal(producer.getbestblockhash(), self.matrix_hash)
        self.prime(self.price, [producer])
        assert_equal(
            self.accounting_only(
                self.activated_state(producer, "back from a reorg across Thaw Day", self.expected_principal,
                                     self.expected_circulating, self.expected_collateral,
                                     self.expected_vaults)["chain"]),
            self.accounting_only(reference["chain"]))

        self.log.info("The rewound and restarted node still validates its whole chain")
        assert_equal(producer.verifychain(4, 0), True)

    def run_test(self):
        reference = self.build_chain()
        first = self.compare_matrix(reference, "first pass")
        self.reorg_checks(first)
        self.compare_matrix(first, "after the reorg checks")

        self.log.info(
            "Five kinds of node agreed on the open-vault principal, collateral, vault count, "
            "health input and health figure at the same block, the token total was checked on "
            "its own, and reorgs above, on and across Thaw Day returned to the same figures")


if __name__ == "__main__":
    DigiDollarThawDayNodeMatrixTest().main()
