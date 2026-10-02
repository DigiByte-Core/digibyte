#!/usr/bin/env python3
# Copyright (c) 2025-2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test DigiDollar deployment info RPC (getdigidollardeploymentinfo).

DigiDollar is a buried deployment (BIP90): the RPC reports the hardcoded
per-network activation height instead of BIP9 signaling state. On default
regtest the buried deployment height is 0, so the deployment is active from
genesis while the static DD/oracle height gates stay at 650.

The RPC also reports Thaw Day, the one block height at which every consensus
change of the Thaw Day release takes effect. No network schedules it by
default; on regtest the -ddthawdayheight option schedules it, and the status
booleans follow the active chain tip through the shared predicate.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal

# Thaw Day height scheduled with the regtest option in the tests below. It is
# above the default regtest DD/oracle gate (650) so the walk to it crosses
# nothing else.
THAW_DAY_HEIGHT = 700


class DigiDollarRPCDeploymentTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Testing DigiDollar deployment info RPC...")
        node = self.nodes[0]

        self.log.info("Generating initial blocks for test setup...")
        self.generate(node, 110)

        self.test_deployment_info_basic()
        self.test_deployment_info_fields()
        self.test_deployment_status_values()
        self.test_deployment_after_activation()
        self.test_deployment_info_oracle_activation_fields()
        self.test_thaw_day_not_scheduled_by_default()
        self.test_thaw_day_scheduled_with_regtest_option()

        self.log.info("All deployment info tests passed!")

    def test_deployment_info_basic(self):
        self.log.info("Testing basic deployment info response...")
        node = self.nodes[0]

        result = node.getdigidollardeploymentinfo()

        assert 'enabled' in result, "Missing 'enabled' field"
        assert 'type' in result, "Missing 'type' field"
        assert 'status' in result, "Missing 'status' field"
        # BIP9 signaling fields are gone with the burial.
        for removed in ('bit', 'start_time', 'timeout', 'min_activation_height',
                        'blocks_until_timeout', 'signaling_blocks', 'threshold',
                        'period_blocks', 'progress_percent'):
            assert removed not in result, f"Stale BIP9 field '{removed}' present"

        self.log.info(f"Deployment enabled: {result['enabled']}")
        self.log.info(f"Deployment status: {result['status']}")

    def test_deployment_info_fields(self):
        self.log.info("Testing deployment info field types...")
        node = self.nodes[0]

        result = node.getdigidollardeploymentinfo()

        assert isinstance(result['enabled'], bool), "enabled should be boolean"
        assert isinstance(result['type'], str), "type should be string"
        assert isinstance(result['status'], str), "status should be string"
        assert_equal(result['type'], 'buried')
        # The deployment is enabled on regtest, so the buried activation
        # height must be reported.
        assert 'activation_height' in result, "Missing 'activation_height' field"
        assert isinstance(result['activation_height'], int), "activation_height should be integer"
        assert_greater_than_or_equal(result['activation_height'], 0)

        self.log.info("All field types verified correctly")

    def test_deployment_status_values(self):
        self.log.info("Testing deployment status is valid...")
        node = self.nodes[0]

        result = node.getdigidollardeploymentinfo()

        # Buried deployments only ever report defined (below the activation
        # height) or active (at/after it); the BIP9 started/locked_in/failed
        # states no longer exist for DigiDollar.
        valid_statuses = ['defined', 'active']
        assert result['status'] in valid_statuses, f"Invalid status: {result['status']}"

        self.log.info(f"Status '{result['status']}' is valid")

        if result['status'] == 'active':
            assert result['enabled'] == True, "If active, enabled should be True"
        else:
            assert result['enabled'] == False, "If defined, enabled should be False"

        # Default regtest buries DigiDollar at height 0: active from genesis.
        assert_equal(result['status'], 'active')
        assert_equal(result['activation_height'], 0)
        self.log.info(f"Activation height: {result['activation_height']}")

    def test_deployment_info_oracle_activation_fields(self):
        # Wave 9 (Agent C): operators need a single RPC that exposes the
        # MuSig2 oracle activation height and the active roster shape so
        # they can correlate `nDigiDollarMuSig2Height`, `nOracleConsensusRequired`,
        # and `nOraclePubkeyCount` with the deployment status. Before
        # this fix the RPC only exposed deployment fields; operators had to
        # read chainparams source to discover the oracle quorum.
        self.log.info("Testing deployment info exposes oracle activation fields...")
        node = self.nodes[0]
        result = node.getdigidollardeploymentinfo()

        for field in (
                "oracle_activation_height",
                "musig2_format_activation_height",
                "oracle_pubkey_count",
                "oracle_consensus_required",
                "oracle_total_slots"):
            assert field in result, f"Missing '{field}' field — operator cannot see oracle roster shape"
            assert isinstance(result[field], int), f"'{field}' should be integer"

        # Default regtest keeps oracle/DD height gates at 650, but the buried
        # DigiDollar deployment height is 0. MuSig2 follows that effective
        # DigiDollar boundary so v0x03 quotes are valid whenever DD is active.
        assert_equal(result["oracle_activation_height"], 650)
        assert_equal(result["musig2_format_activation_height"], 0)
        # Regtest 4-of-7 quorum.
        assert_equal(result["oracle_pubkey_count"], 7)
        assert_equal(result["oracle_consensus_required"], 4)
        # Regtest configures 7 oracle slots.
        assert_equal(result["oracle_total_slots"], 7)

        self.log.info(
            "Oracle activation fields verified: oracle_height=%d, musig2_height=%d, consensus=%d-of-%d, slots=%d" % (
                result["oracle_activation_height"],
                result["musig2_format_activation_height"],
                result["oracle_consensus_required"],
                result["oracle_pubkey_count"],
                result["oracle_total_slots"]))

    def test_deployment_after_activation(self):
        self.log.info("Testing deployment info consistency...")
        node = self.nodes[0]

        result1 = node.getdigidollardeploymentinfo()

        self.generate(node, 10)

        result2 = node.getdigidollardeploymentinfo()

        # The buried deployment parameters are constants: they must not
        # change as the chain advances.
        assert_equal(result1['type'], result2['type'])
        assert_equal(result1['activation_height'], result2['activation_height'])

        if result1['status'] == 'active':
            assert_equal(result1['status'], result2['status'])
            assert_equal(result1['enabled'], result2['enabled'])

        self.log.info("Deployment info is consistent across blocks")

    def check_thaw_day(self, node, scheduled_height=None):
        """Check the thaw_day object against the node's actual tip.

        With scheduled_height None the object must say "not scheduled" and
        carry no height. Otherwise it must carry exactly that height and the
        two booleans must follow the tip height, because default regtest has
        DigiDollar active from genesis so only the Thaw Day height decides.
        Returns the whole RPC result.
        """
        result = node.getdigidollardeploymentinfo()
        assert 'thaw_day' in result, "Missing 'thaw_day' object"
        thaw = result['thaw_day']
        assert isinstance(thaw, dict), "thaw_day should be an object"
        # bool is a subclass of int in Python, so pin the exact JSON types.
        for field, expected_type in (('scheduled', bool),
                                     ('tip_height', int),
                                     ('next_block_height', int),
                                     ('active_at_tip', bool),
                                     ('active_next_block', bool)):
            assert field in thaw, f"Missing 'thaw_day.{field}'"
            assert type(thaw[field]) is expected_type, \
                f"'thaw_day.{field}' should be {expected_type.__name__}, got {type(thaw[field]).__name__}"

        tip = node.getblockcount()
        assert_equal(thaw['tip_height'], tip)
        assert_equal(thaw['next_block_height'], tip + 1)

        if scheduled_height is None:
            assert_equal(thaw['scheduled'], False)
            assert 'height' not in thaw, "'thaw_day.height' must be absent when not scheduled"
            assert_equal(thaw['active_at_tip'], False)
            assert_equal(thaw['active_next_block'], False)
        else:
            assert_equal(thaw['scheduled'], True)
            assert 'height' in thaw, "Missing 'thaw_day.height' although scheduled"
            assert type(thaw['height']) is int, "'thaw_day.height' should be int"
            assert_equal(thaw['height'], scheduled_height)
            assert_equal(thaw['active_at_tip'], tip >= scheduled_height)
            assert_equal(thaw['active_next_block'], tip + 1 >= scheduled_height)
        return result

    def test_thaw_day_not_scheduled_by_default(self):
        self.log.info("Testing thaw_day is reported as not scheduled without the regtest option...")
        node = self.nodes[0]

        self.check_thaw_day(node)
        # Still not scheduled, and still tracking the tip, after more blocks.
        self.generate(node, 1)
        self.check_thaw_day(node)

        self.log.info("thaw_day present, not scheduled, no height, both actives false")

    def test_thaw_day_scheduled_with_regtest_option(self):
        self.log.info("Testing thaw_day with -ddthawdayheight=%d..." % THAW_DAY_HEIGHT)
        node = self.nodes[0]

        before = node.getdigidollardeploymentinfo()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-ddthawdayheight=%d" % THAW_DAY_HEIGHT])
        after = self.check_thaw_day(node, THAW_DAY_HEIGHT)

        # Scheduling Thaw Day changes nothing else the RPC reports (the
        # MuSig2 session object is live operator state, not configuration).
        for key in before:
            if key in ('thaw_day', 'musig2_session'):
                continue
            assert_equal(before[key], after[key])

        # Walk the tip to the boundary: two below, one below, at, and past.
        tip = node.getblockcount()
        assert tip < THAW_DAY_HEIGHT - 2, "test setup mined past the Thaw Day boundary"
        self.generate(node, THAW_DAY_HEIGHT - 2 - tip)
        thaw = self.check_thaw_day(node, THAW_DAY_HEIGHT)['thaw_day']
        assert_equal(thaw['tip_height'], THAW_DAY_HEIGHT - 2)
        assert_equal(thaw['active_at_tip'], False)
        assert_equal(thaw['active_next_block'], False)

        # One below: the next block may use the new rules; the tip does not.
        self.generate(node, 1)
        thaw = self.check_thaw_day(node, THAW_DAY_HEIGHT)['thaw_day']
        assert_equal(thaw['tip_height'], THAW_DAY_HEIGHT - 1)
        assert_equal(thaw['active_at_tip'], False)
        assert_equal(thaw['active_next_block'], True)

        # At Thaw Day: both.
        self.generate(node, 1)
        thaw = self.check_thaw_day(node, THAW_DAY_HEIGHT)['thaw_day']
        assert_equal(thaw['tip_height'], THAW_DAY_HEIGHT)
        assert_equal(thaw['active_at_tip'], True)
        assert_equal(thaw['active_next_block'], True)

        # Past Thaw Day: stays active.
        self.generate(node, 5)
        thaw = self.check_thaw_day(node, THAW_DAY_HEIGHT)['thaw_day']
        assert_equal(thaw['tip_height'], THAW_DAY_HEIGHT + 5)
        assert_equal(thaw['active_at_tip'], True)
        assert_equal(thaw['active_next_block'], True)

        # The schedule comes from configuration, not from anything the node
        # saved: restarting without the option reports "not scheduled" again
        # even though the chain is past the height.
        self.restart_node(0, extra_args=self.extra_args[0])
        self.check_thaw_day(node)

        self.log.info("thaw_day follows the tip: false/false, false/true, true/true, and unscheduled after restart")


if __name__ == '__main__':
    DigiDollarRPCDeploymentTest().main()
