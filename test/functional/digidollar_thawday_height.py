#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Pin how a node reports the Thaw Day height (regtest -ddthawdayheight).

Thaw Day is the one block height at which every consensus change of the
v9.26.6 release takes effect. This test checks only how the node reports
that height through the "thaw_day" object of getdigidollardeploymentinfo;
the rules that will consult it arrive in a later release step, so block
validity is identical with and without the knob.

Expectations, with H the scheduled height:

- tip H-2: neither the tip nor the next block is under the new rules.
- tip H-1: the tip is not, the next block (height H) is. That is the
  expected "next block may use the new rules" case, not early activation.
- tip H: the tip is under the new rules.
- invalidateblock / reconsiderblock and a competing branch: the status
  follows the active chain's height only.
- restart: the status is unchanged (it comes from chainparams, not from
  saved state); starting with a different knob value changes it.
- the knob rejects a negative value, the "not scheduled" sentinel value,
  and any non-regtest chain.
- a node started without the knob reports "not scheduled" and both actives
  false at every height.
- Thaw Day scheduled below the DigiDollar activation height stays inactive
  until DigiDollar itself is active.
"""

import re

from test_framework.test_framework import DigiByteTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal

# Thaw Day height configured on the nodes that carry the knob. It sits above
# the default regtest DigiDollar/oracle gates (650) so the boundary is
# exercised on a chain where DigiDollar is already active.
THAW_HEIGHT = 700
# DigiDollar activation height for the node whose Thaw Day is scheduled
# before DigiDollar itself activates.
LATE_DIGIDOLLAR_HEIGHT = 710
# The largest int value means "not scheduled" inside the node and must be
# rejected as a knob value.
INT_MAX = 2**31 - 1

BASE_ARGS = ["-txindex=1", "-dandelion=0"]
KNOB_ARGS = BASE_ARGS + [f"-ddthawdayheight={THAW_HEIGHT}"]



# The exact startup errors the node prints for a rejected knob, as written to
# stderr with the "Error: " prefix. An unknown option would instead print
# "Error parsing command line arguments: Invalid parameter ...", which is
# deliberately not accepted because it would mean the knob does not exist.
def rejected_value_error(value):
    return ("Error: Invalid height value (%s) for -ddthawdayheight: expected a whole "
            "number from 0 up to, but not including, %d." % (value, INT_MAX))


def rejected_chain_error(chain):
    return ("Error: -ddthawdayheight is only accepted on regtest; the Thaw Day height "
            "of the %s network is fixed in the release." % chain)


def exact_line(message):
    """A regex that matches the message as one whole line of stderr."""
    return "^" + re.escape(message) + "$"


class DigiDollarThawDayHeightTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [
            # node0: Thaw Day scheduled, DigiDollar active from genesis.
            KNOB_ARGS,
            # node1: same settings; mines the competing branch.
            KNOB_ARGS,
            # node2: no knob; must report "not scheduled" everywhere.
            BASE_ARGS,
            # node3: Thaw Day scheduled before DigiDollar activates.
            KNOB_ARGS + [f"-digidollaractivationheight={LATE_DIGIDOLLAR_HEIGHT}"],
        ]

    def setup_network(self):
        self.setup_nodes()
        # Star topology around node0 so node1 can be cut off on its own
        # while node2 and node3 keep following node0.
        for i in (1, 2, 3):
            self.connect_nodes(0, i)
        self.sync_all()

    def assert_thaw_day(self, node, *, scheduled, tip, at_tip, next_block, height=None):
        """Check every field of the thaw_day object and return it."""
        info = node.getdigidollardeploymentinfo()
        assert "thaw_day" in info, "getdigidollardeploymentinfo has no thaw_day object"
        thaw = info["thaw_day"]
        for field in ("scheduled", "active_at_tip", "active_next_block"):
            assert field in thaw, f"thaw_day is missing '{field}'"
            assert isinstance(thaw[field], bool), f"thaw_day.{field} should be a boolean"
        for field in ("tip_height", "next_block_height"):
            assert field in thaw, f"thaw_day is missing '{field}'"
            assert isinstance(thaw[field], int) and not isinstance(thaw[field], bool), \
                f"thaw_day.{field} should be a number"

        assert_equal(thaw["scheduled"], scheduled)
        if scheduled:
            assert "height" in thaw, "thaw_day.height must be present when scheduled"
            assert isinstance(thaw["height"], int) and not isinstance(thaw["height"], bool)
            assert_equal(thaw["height"], height)
        else:
            assert "height" not in thaw, "thaw_day.height must be absent when not scheduled"

        # Both heights come from the active chain, never from saved state.
        assert_equal(thaw["tip_height"], tip)
        assert_equal(thaw["tip_height"], node.getblockcount())
        assert_equal(thaw["next_block_height"], tip + 1)
        assert_equal(thaw["active_at_tip"], at_tip)
        assert_equal(thaw["active_next_block"], next_block)
        return thaw

    def assert_not_scheduled(self, node, *, tip):
        return self.assert_thaw_day(node, scheduled=False, tip=tip, at_tip=False, next_block=False)

    def assert_scheduled(self, node, *, tip, at_tip, next_block, height=THAW_HEIGHT):
        return self.assert_thaw_day(node, scheduled=True, height=height, tip=tip,
                                    at_tip=at_tip, next_block=next_block)

    def run_test(self):
        self.test_boundary_heights()
        self.test_reorg_and_competing_branch()
        self.test_thaw_day_before_digidollar_activation()
        self.test_restart_keeps_status()
        self.test_knob_rejection()
        self.test_final_state()

    def test_boundary_heights(self):
        node0, node1, node2, node3 = self.nodes
        h = THAW_HEIGHT

        self.log.info(f"Mine to tip {h - 2} (two below Thaw Day): nothing is active yet")
        self.generate(node0, h - 2)
        self.assert_scheduled(node0, tip=h - 2, at_tip=False, next_block=False)
        self.assert_scheduled(node1, tip=h - 2, at_tip=False, next_block=False)
        self.assert_not_scheduled(node2, tip=h - 2)
        self.assert_scheduled(node3, tip=h - 2, at_tip=False, next_block=False)

        self.log.info(f"Mine to tip {h - 1}: the tip is not active, the next block (height {h}) is")
        self.generate(node0, 1)
        self.assert_scheduled(node0, tip=h - 1, at_tip=False, next_block=True)
        self.assert_scheduled(node1, tip=h - 1, at_tip=False, next_block=True)
        self.assert_not_scheduled(node2, tip=h - 1)
        # node3's DigiDollar deployment is not active at height h, so its
        # next block is not under the Thaw Day rules even though h is its
        # scheduled height.
        self.assert_scheduled(node3, tip=h - 1, at_tip=False, next_block=False)

    def test_reorg_and_competing_branch(self):
        node0, node1, node2, node3 = self.nodes
        h = THAW_HEIGHT

        self.log.info("Cut node1 off at H-1 so it can mine a competing branch later")
        self.disconnect_nodes(0, 1)
        assert_equal(node1.getblockcount(), h - 1)

        self.log.info(f"Mine block {h} on node0: the tip is now active")
        block_h = self.generate(node0, 1, sync_fun=lambda: self.sync_blocks([node0, node2, node3]))[0]
        self.assert_scheduled(node0, tip=h, at_tip=True, next_block=True)
        self.assert_not_scheduled(node2, tip=h)
        self.assert_scheduled(node3, tip=h, at_tip=False, next_block=False)

        self.log.info(f"invalidateblock {h}: the tip drops to {h - 1} and only the next block is active again")
        node0.invalidateblock(block_h)
        assert_equal(node0.getblockcount(), h - 1)
        self.assert_scheduled(node0, tip=h - 1, at_tip=False, next_block=True)

        self.log.info(f"reconsiderblock {h}: the tip is active again")
        node0.reconsiderblock(block_h)
        assert_equal(node0.getbestblockhash(), block_h)
        self.assert_scheduled(node0, tip=h, at_tip=True, next_block=True)

        self.log.info(f"node1 mines a competing branch {h}' and {h + 1}' while disconnected")
        self.assert_scheduled(node1, tip=h - 1, at_tip=False, next_block=True)
        branch = self.generate(node1, 2, sync_fun=self.no_op)
        assert branch[0] != block_h
        self.assert_scheduled(node1, tip=h + 1, at_tip=True, next_block=True)

        self.log.info("Reconnect: node0 switches to the longer branch and reports that chain's height")
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(node0.getbestblockhash(), branch[1])
        assert_equal(node0.getblockhash(h), branch[0])
        self.assert_scheduled(node0, tip=h + 1, at_tip=True, next_block=True)
        self.assert_scheduled(node1, tip=h + 1, at_tip=True, next_block=True)
        self.assert_not_scheduled(node2, tip=h + 1)
        self.assert_scheduled(node3, tip=h + 1, at_tip=False, next_block=False)

        self.log.info("Invalidate the competing branch on node0 only: it returns to its own block at H")
        node0.invalidateblock(branch[0])
        assert_equal(node0.getbestblockhash(), block_h)
        self.assert_scheduled(node0, tip=h, at_tip=True, next_block=True)
        # The other nodes did not invalidate anything and keep the longer branch.
        self.assert_scheduled(node1, tip=h + 1, at_tip=True, next_block=True)
        self.assert_not_scheduled(node2, tip=h + 1)

        self.log.info("Reconsider the competing branch: node0 follows it again")
        node0.reconsiderblock(branch[0])
        assert_equal(node0.getbestblockhash(), branch[1])
        self.assert_scheduled(node0, tip=h + 1, at_tip=True, next_block=True)
        self.sync_blocks()

    def test_thaw_day_before_digidollar_activation(self):
        node0, node1, node2, node3 = self.nodes
        dd = LATE_DIGIDOLLAR_HEIGHT

        self.log.info(f"Mine to tip {dd - 1}: node3's next block activates DigiDollar, so Thaw Day starts there")
        self.generate(node0, dd - 1 - node0.getblockcount())
        assert_equal(node3.getblockcount(), dd - 1)
        self.assert_scheduled(node3, tip=dd - 1, at_tip=False, next_block=True)
        self.assert_scheduled(node0, tip=dd - 1, at_tip=True, next_block=True)
        self.assert_not_scheduled(node2, tip=dd - 1)

        self.log.info(f"Mine block {dd}: node3 is now under the Thaw Day rules")
        self.generate(node0, 1)
        self.assert_scheduled(node3, tip=dd, at_tip=True, next_block=True)
        self.assert_scheduled(node0, tip=dd, at_tip=True, next_block=True)
        self.assert_scheduled(node1, tip=dd, at_tip=True, next_block=True)
        self.assert_not_scheduled(node2, tip=dd)

    def test_restart_keeps_status(self):
        node0, node1, node2, node3 = self.nodes
        tip = node0.getblockcount()

        self.log.info("Restart nodes with the same settings: the status is unchanged")
        for node in (node0, node2, node3):
            before = node.getdigidollardeploymentinfo()["thaw_day"]
            self.restart_node(node.index)
            after = node.getdigidollardeploymentinfo()["thaw_day"]
            assert_equal(before, after)
        for i in (1, 2, 3):
            self.connect_nodes(0, i)
        self.assert_scheduled(node0, tip=tip, at_tip=True, next_block=True)
        self.assert_not_scheduled(node2, tip=tip)
        self.assert_scheduled(node3, tip=tip, at_tip=True, next_block=True)

        self.log.info("Restart the knob-less node with -ddthawdayheight=0: the status follows the new setting")
        self.restart_node(2, extra_args=BASE_ARGS + ["-ddthawdayheight=0"])
        self.connect_nodes(0, 2)
        self.assert_scheduled(node2, tip=tip, at_tip=True, next_block=True, height=0)

        self.log.info("Restart it without the knob again: nothing was saved, it is not scheduled")
        self.restart_node(2)
        self.connect_nodes(0, 2)
        self.assert_not_scheduled(node2, tip=tip)

    def test_knob_rejection(self):
        node0 = self.nodes[0]
        before = node0.getdigidollardeploymentinfo()["thaw_day"]
        self.stop_node(0)

        self.log.info("A negative height is a startup error")
        node0.assert_start_raises_init_error(
            BASE_ARGS + ["-ddthawdayheight=-1"],
            expected_msg=exact_line(rejected_value_error("-1")), match=ErrorMatch.PARTIAL_REGEX)

        self.log.info("The largest int value is the not-scheduled sentinel and is a startup error")
        node0.assert_start_raises_init_error(
            BASE_ARGS + [f"-ddthawdayheight={INT_MAX}"],
            expected_msg=exact_line(rejected_value_error(INT_MAX)), match=ErrorMatch.PARTIAL_REGEX)

        # -regtest=0 on the command line overrides the regtest=1 line the
        # framework writes into the config file, so -chain selects the other
        # network. Every kind of network activity is switched off on the
        # command line as well, so that even a wrongly accepted knob could
        # never make this process talk to anyone; the [regtest] config
        # section does not apply to the other chains.
        no_network = ["-connect=0", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
                      "-discover=0", "-listenonion=0"]
        for chain in ("signet", "test"):
            self.log.info(f"The knob is a startup error on the {chain} chain")
            node0.assert_start_raises_init_error(
                BASE_ARGS + ["-regtest=0", f"-chain={chain}", f"-ddthawdayheight={THAW_HEIGHT}"] + no_network,
                expected_msg=exact_line(rejected_chain_error(chain)), match=ErrorMatch.PARTIAL_REGEX)

        self.log.info("The regular knob still starts on regtest and the status is unchanged")
        self.start_node(0)
        for i in (1, 2, 3):
            self.connect_nodes(0, i)
        assert_equal(node0.getdigidollardeploymentinfo()["thaw_day"], before)

    def test_final_state(self):
        node0, node1, node2, node3 = self.nodes
        self.sync_blocks()
        tip = node0.getblockcount()
        assert tip >= LATE_DIGIDOLLAR_HEIGHT
        self.assert_scheduled(node0, tip=tip, at_tip=True, next_block=True)
        self.assert_scheduled(node1, tip=tip, at_tip=True, next_block=True)
        self.assert_not_scheduled(node2, tip=tip)
        self.assert_scheduled(node3, tip=tip, at_tip=True, next_block=True)


if __name__ == '__main__':
    DigiDollarThawDayHeightTest().main()
