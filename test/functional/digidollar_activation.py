#!/usr/bin/env python3
# Copyright (c) 2025-2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test DigiDollar buried-deployment (BIP90) activation lifecycle.

DigiDollar activated via BIP9 bit-23 signaling and is now a buried
deployment: its activation height is hardcoded per network, so there is no
DEFINED -> STARTED -> LOCKED_IN -> ACTIVE state machine (and no
timeout/FAILED path) left to exercise. On regtest,
-digidollaractivationheight=N retargets the buried height together with the
static DD/oracle/MuSig2 gates, so DigiDollar activates at exactly height N:
the first DD-active block is block N, and the RPCs report enabled=True once
the tip is at N-1 (the *next* block is DD-active).

This test pins the buried-boundary lifecycle:
  - below the boundary: getdeploymentinfo reports the digidollar entry as
    {type: "buried", active: False, height: N} with no "bip9" sub-object;
    getdigidollardeploymentinfo reports enabled=False / status "defined";
    DD RPCs error with "not yet active"; raw DD-marker transactions are
    rejected from the mempool with "digidollar-not-active"; block templates
    neither signal bit 23 nor advertise the "digidollar" rule
  - at/after the boundary: enabled=True / status "active", the template
    advertises "digidollar" in rules (still without signaling bit 23), and
    the full DD mint -> send -> redeem flow works
"""

from test_framework.address import address_to_scriptpubkey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal

# Buried activation height used by this test (-digidollaractivationheight).
ACTIVATION_HEIGHT = 200
DD_TX_TRANSFER_VERSION = (2 << 24) | 0x0770


class DigiDollarActivationTest(DigiByteTestFramework):
    """Test suite for the DigiDollar buried-deployment activation boundary."""

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-digidollaractivationheight={}".format(ACTIVATION_HEIGHT),
            "-dandelion=0",
            "-txindex=1",
        ]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_buried_entry(self, node, *, active):
        """Pin the getdeploymentinfo shape of the buried digidollar entry."""
        dd_info = node.getdeploymentinfo()["deployments"]["digidollar"]
        assert_equal(dd_info["type"], "buried")
        assert_equal(dd_info["active"], active)
        assert_equal(dd_info["height"], ACTIVATION_HEIGHT)
        assert "bip9" not in dd_info, \
            "buried deployment must not carry a bip9 sub-object"

    def make_dd_marker_tx(self, node):
        """Build a final raw tx carrying the DD transfer marker bits."""
        tx = CTransaction()
        tx.nVersion = DD_TX_TRANSFER_VERSION
        tx.vin = [CTxIn(COutPoint(1, 0), b"", 0xFFFFFFFF)]
        tx.vout = [CTxOut(100000, address_to_scriptpubkey(node.getnewaddress()))]
        return tx

    def confirm_transaction(self, node, txid):
        block_hash = node.generate(1)[0]
        assert txid in node.getblock(block_hash)["tx"]
        # Wallet confirmation updates are delivered asynchronously after mining.
        node.syncwithvalidationinterfacequeue()

    def run_test(self):
        self.log.info("Starting DigiDollar buried-deployment activation tests")

        node = self.nodes[0]

        # ── Pre-activation: genesis ──
        self.log.info("Checking buried deployment reporting at genesis...")
        self.assert_buried_entry(node, active=False)

        dep = node.getdigidollardeploymentinfo()
        assert_equal(dep["enabled"], False)
        assert_equal(dep["type"], "buried")
        assert_equal(dep["status"], "defined")
        assert_equal(dep["activation_height"], ACTIVATION_HEIGHT)

        # ── Pre-activation: last inactive tip (N-2) ──
        # enabled flips at tip N-1 because the RPCs report whether the *next*
        # block is DD-active, so N-2 is the last tip with DigiDollar disabled.
        self.log.info("Mining to height %d (last pre-activation tip)..." %
                      (ACTIVATION_HEIGHT - 2))
        node.generate(ACTIVATION_HEIGHT - 2)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 2)

        self.assert_buried_entry(node, active=False)
        dep = node.getdigidollardeploymentinfo()
        assert_equal(dep["enabled"], False)
        assert_equal(dep["status"], "defined")
        assert_equal(dep["activation_height"], ACTIVATION_HEIGHT)

        # DD RPCs must refuse pre-activation.
        self.log.info("Verifying DD RPCs rejected pre-activation...")
        try:
            node.mintdigidollar(100000, 4)
            assert False, "mintdigidollar should reject pre-activation"
        except Exception as e:
            error_msg = str(e)
            self.log.info(f"  Correctly rejected: {error_msg}")
            assert "not yet active" in error_msg.lower(), \
                f"Expected 'not yet active' error, got: {error_msg}"

        # Raw DD-marker transactions must be rejected from the mempool.
        self.log.info("Verifying raw DD-marker tx rejected pre-activation...")
        dd_tx = self.make_dd_marker_tx(node)
        result = node.testmempoolaccept([dd_tx.serialize().hex()], maxfeerate=0)[0]
        assert not result["allowed"], result
        assert_equal(result["reject-reason"], "digidollar-not-active")
        self.log.info("  Rejected with digidollar-not-active")

        # Block templates: no bit-23 signaling, no "digidollar" rule yet.
        self.log.info("Verifying block template pre-activation...")
        template = node.getblocktemplate({"rules": ["segwit"]})
        version = template["version"]
        assert (version & (1 << 23)) == 0, \
            "Bit 23 must not be signaled for the buried deployment"
        assert "digidollar" not in template["rules"], \
            "digidollar rule must not be advertised pre-activation"
        assert "digidollar" not in template["vbavailable"], \
            "digidollar must not appear in vbavailable post-burial"
        # Taproot/AlgoLock are buried at height 0 on regtest and always active.
        assert "taproot" in template["rules"]
        assert "algolock" in template["rules"]
        self.log.info(f"  Template version 0x{version:08x}, rules={template['rules']}")

        # ── Activation boundary: tip N-1 (next block is the first DD block) ──
        self.log.info("Mining the activation boundary...")
        node.generate(1)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 1)

        self.assert_buried_entry(node, active=True)
        dep = node.getdigidollardeploymentinfo()
        assert_equal(dep["enabled"], True)
        assert_equal(dep["status"], "active")
        assert_equal(dep["activation_height"], ACTIVATION_HEIGHT)

        # The template for block N advertises the digidollar rule without
        # signaling bit 23 (buried deployments never set version bits).
        template = node.getblocktemplate({"rules": ["segwit"]})
        assert (template["version"] & (1 << 23)) == 0
        assert "digidollar" in template["rules"], \
            "digidollar rule must be advertised once active"
        assert "digidollar" not in template["vbavailable"]

        # Mine the first DD-active block.
        node.generate(1)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT)
        self.assert_buried_entry(node, active=True)

        # ── Post-activation: full mint → send → redeem flow ──
        self.log.info("Testing DD mint/send/redeem post-activation...")

        # Mature coinbases for collateral, then publish a MuSig2 oracle quote.
        node.generate(110)
        node.setmockoracleprice(500000)  # $0.50/DGB

        mint_result = node.mintdigidollar(15000, 0)  # $150, tier 0 (1h lock)
        assert_equal(mint_result["dd_minted"], 15000)
        unlock_height = mint_result["unlock_height"]
        position_id = mint_result["position_id"]
        self.confirm_transaction(node, mint_result["txid"])
        self.log.info(f"  Mint confirmed: txid={mint_result['txid']}, "
                      f"unlock_height={unlock_height}")

        balance = node.getdigidollarbalance()
        total = balance["total"] if isinstance(balance, dict) else balance
        assert_equal(total, 15000)

        # Send DD to a fresh local DD address (confirmed-only transfers).
        node.setmockoracleprice(500000)
        recv_addr = node.getdigidollaraddress()
        send_result = node.senddigidollar(recv_addr, 1000)  # $10
        assert "txid" in send_result
        self.confirm_transaction(node, send_result["txid"])
        balance = node.getdigidollarbalance()
        total = balance["total"] if isinstance(balance, dict) else balance
        assert_equal(total, 15000)  # self-send conserves DD balance
        self.log.info(f"  Send confirmed: txid={send_result['txid']}")

        # Pass the tier-0 timelock, refresh the oracle quote, and redeem.
        current_height = node.getblockcount()
        if current_height <= unlock_height:
            node.generate(unlock_height - current_height + 5)
        node.setmockoracleprice(500000)
        redeem_result = node.redeemdigidollar(position_id, 15000)
        assert "txid" in redeem_result
        self.confirm_transaction(node, redeem_result["txid"])
        self.log.info(f"  Redeem confirmed: txid={redeem_result['txid']}")

        active_ids = [p["position_id"] for p in node.listdigidollarpositions()
                      if p.get("is_active", p.get("status", "active") == "active")]
        assert position_id not in active_ids, "Position should be closed after redeem"

        # Final: deployment reporting is still active/buried.
        self.assert_buried_entry(node, active=True)
        dep = node.getdigidollardeploymentinfo()
        assert_equal(dep["enabled"], True)
        assert_equal(dep["status"], "active")

        self.log.info("All DigiDollar buried-deployment activation tests PASSED ✓")


if __name__ == '__main__':
    DigiDollarActivationTest().main()
