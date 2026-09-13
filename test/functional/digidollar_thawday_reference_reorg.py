#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check signed price references, activation, restart, and an ancestor reorg."""

import json
from pathlib import Path

from test_framework.script import CScript
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarThawDayReferenceReorgTest(DigiByteTestFramework):
    THAW_HEIGHT = 500
    FORK_HEIGHT = 245
    OLD_REFERENCE = 30000
    NEW_REFERENCE = 40000
    CANDIDATE_PRICE = 36000
    PRINCIPAL = 100000

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common = [
            "-digidollaractivationheight=1",
            f"-ddthawdayheight={self.THAW_HEIGHT}",
            "-dandelion=0",
            "-txindex=1",
            "-digidollarstatsindex=0",
            "-persistmempool=0",
        ]
        self.extra_args = [list(common), list(common)]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def clock(self, seconds=60):
        self.t += seconds
        for node in self.nodes:
            node.setmocktime(self.t)

    def quote(self, node, price):
        self.clock()
        node.setmockoracleprice(price)

    def mine_to(self, node, height, address):
        count = height - node.getblockcount()
        assert count >= 0
        if count:
            self.clock(max(60, count * 15))
            self.generatetoaddress(node, count, address, sync_fun=self.no_op)

    def remember(self, label, value):
        self.observations[label] = value
        self.log.info("%s: %s", label, json.dumps(value, sort_keys=True, default=str))
        Path(self.options.tmpdir, "observations.json").write_text(
            json.dumps(self.observations, indent=2, sort_keys=True, default=str) + "\n",
            encoding="utf-8",
        )

    def reference(self, node, price, expected_reference, restricted, label):
        self.quote(node, price)
        status = node.getprotectionstatus()["volatility"]
        self.remember(label, status)
        assert_equal(status["candidate_height"], node.getblockcount() + 1)
        assert_equal(status["activation_height"], self.THAW_HEIGHT)
        assert_equal(status["next_block_active"], True)
        assert_equal(status["rule_version"], 1)
        assert_equal(status["ready"], True)
        assert_equal(status["quote_available"], True)
        assert_equal(status["candidate_price_micro_usd"], price)
        assert_equal(status["reference_price_micro_usd"], expected_reference)
        assert_equal(status["sample_count"], 15)
        assert_equal(status["window_start_height"], max(0, node.getblockcount() + 1 - 1440))
        assert_equal(status["window_end_height"], node.getblockcount() + 1 - 240)
        assert_equal(status["minting_restricted"], restricted)
        assert_equal(status["all_operations_restricted"], False)
        assert_equal(status["rejection_reason"], "volatility_pause" if restricted else "none")
        assert_equal(status["data_error"], "")
        return status

    def signed_sample(self, node, height, expected_price):
        block_hash = node.getblockhash(height)
        coinbase = node.getblock(block_hash, 2)["tx"][0]
        scripts = [bytes.fromhex(vout["scriptPubKey"]["hex"]) for vout in coinbase["vout"]]
        oracle_scripts = [script for script in scripts if script.startswith(b"\x6a\xbf")]
        assert_equal(len(oracle_scripts), 1)
        fields = list(CScript(oracle_scripts[0]))
        assert_equal(len(fields), 4)
        assert_equal(fields[2], b"\x03")
        payload = fields[3]
        bitmap_size = payload[0]
        assert bitmap_size > 0
        assert_equal(len(payload), 1 + bitmap_size + 4 + 8 + 8 + 64)
        price_offset = 1 + bitmap_size + 4
        price = int.from_bytes(payload[price_offset:price_offset + 8], "little")
        assert_equal(price, expected_price)
        return {"height": height, "hash": block_hash, "price": price}

    def build_samples(self, node, price, address, label):
        assert_equal(node.getblockcount(), self.FORK_HEIGHT)
        samples = []
        # Candidate 500 samples 260..246. Height 261 also enters after a reorg
        # to tip 500, so write and inspect it explicitly as well.
        for height in range(246, 262):
            self.quote(node, price)
            self.mine_to(node, height, address)
            samples.append(self.signed_sample(node, height, price))
        self.remember(label, samples)

    def reject_mint_bytes(self, node, raw, label):
        result = node.testmempoolaccept([raw])[0]
        self.remember(label, result)
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], "minting-volatility-pause")

    def run_test(self):
        a, b = self.nodes
        self.t = 1700000000
        self.observations = {}
        self.clock()
        owner_address = a.getnewaddress()
        # B's branch rewards go to A. B can fund its mint only from the payment
        # confirmed on the common chain, so the same inputs exist on A's fork.
        sink_address = a.getnewaddress()
        self.mine_to(a, 175, owner_address)
        self.sync_all()
        self.quote(a, self.OLD_REFERENCE)
        self.quote(b, self.OLD_REFERENCE)
        funding_txid = a.sendtoaddress(b.getnewaddress(), 1000000)
        legacy_mint = a.mintdigidollar(self.PRINCIPAL, 0)
        self.mine_to(a, 176, owner_address)
        self.sync_all()
        common_block = a.getblock(a.getbestblockhash())
        assert funding_txid in common_block["tx"]
        assert legacy_mint["txid"] in common_block["tx"]
        self.mine_to(a, self.FORK_HEIGHT, owner_address)
        self.sync_all()
        common_hash = a.getbestblockhash()
        self.remember("common_chain", {
            "height": self.FORK_HEIGHT, "hash": common_hash,
            "legacy_mint": legacy_mint["txid"], "funding": funding_txid,
        })
        self.disconnect_nodes(0, 1)

        self.log.info("Build fork A with real signed samples at 30000")
        self.build_samples(a, self.OLD_REFERENCE, owner_address, "branch_a_samples")
        self.mine_to(a, self.THAW_HEIGHT - 1, owner_address)
        old_tip = a.getbestblockhash()
        old_branch = [a.getblockhash(height) for height in range(self.FORK_HEIGHT + 1, 500)]
        for price, restricted, label in (
            (35999, False, "upward_just_inside"),
            (36000, True, "upward_equality"),
            (24001, False, "downward_just_inside"),
            (24000, True, "downward_equality"),
        ):
            self.reference(a, price, self.OLD_REFERENCE, restricted, label)
            if restricted:
                assert_raises_rpc_error(-1, "Minting volatility pause", a.mintdigidollar, self.PRINCIPAL, 0)

        self.log.info("Build fork B with different signed sample ancestors at 40000")
        self.build_samples(b, self.NEW_REFERENCE, sink_address, "branch_b_samples")
        self.mine_to(b, self.THAW_HEIGHT - 2, sink_address)
        self.quote(b, self.CANDIDATE_PRICE)
        legacy = b.getprotectionstatus()["volatility"]
        self.remember("legacy_candidate_499", legacy)
        assert_equal(legacy["candidate_height"], 499)
        assert_equal(legacy["next_block_active"], False)
        assert_equal(legacy["minting_restricted"], True)
        assert_equal(legacy["rejection_reason"], "legacy_volatility_freeze")
        self.mine_to(b, self.THAW_HEIGHT - 1, sink_address)
        self.reference(b, self.CANDIDATE_PRICE, self.NEW_REFERENCE, False, "branch_b_candidate_500")
        mint = b.mintdigidollar(self.PRINCIPAL, 0)
        txid = mint["txid"]
        assert txid in b.getrawmempool()
        raw = b.getrawtransaction(txid)
        Path(self.options.tmpdir, "fixed_mint.hex").write_text(raw + "\n", encoding="ascii")
        for vin in b.decoderawtransaction(raw)["vin"]:
            coin = a.gettxout(vin["txid"], vin["vout"])
            assert coin is not None, "mint input is absent on fork A"
            assert coin["confirmations"] >= 499 - self.FORK_HEIGHT + 1
        self.remember("fixed_mint", {"txid": txid, "created_for_candidate": 500})

        self.reference(a, self.CANDIDATE_PRICE, self.OLD_REFERENCE, True, "branch_a_before_restart")
        self.reject_mint_bytes(a, raw, "same_mint_rejected_on_a")
        assert_raises_rpc_error(-25, "minting-volatility-pause", a.rpc.generateblock, owner_address, [raw], False)
        self.reference(a, 24000, self.OLD_REFERENCE, True, "same_mint_downward_equality")
        self.reject_mint_bytes(a, raw, "same_mint_rejected_downward")

        self.log.info("Restart fork A and repeat the same candidate and transaction")
        self.restart_node(0, self.extra_args[0])
        assert_equal(a.getbestblockhash(), old_tip)
        before = self.observations["branch_a_before_restart"]
        after = self.reference(a, self.CANDIDATE_PRICE, self.OLD_REFERENCE, True, "branch_a_after_restart")
        assert_equal(after, before)
        self.reject_mint_bytes(a, raw, "same_mint_rejected_after_restart")

        self.log.info("Replace sampled ancestors by accepting the longer valid branch B")
        self.quote(b, self.CANDIDATE_PRICE)
        # An explicit empty block leaves the fixed mint unconfirmed for an
        # independent testmempoolaccept after A changes branches.
        self.clock()
        empty_tip = self.generateblock(b, sink_address, [], sync_fun=self.no_op)
        assert_equal(b.getblockcount(), 500)
        assert txid not in b.getblock(empty_tip["hash"])["tx"]
        assert int(b.getblockchaininfo()["chainwork"], 16) > int(a.getblockchaininfo()["chainwork"], 16)
        # Submit real competing blocks through normal block processing while
        # disconnected. This prevents transaction relay from hiding admission.
        submissions = []
        try:
            for height in range(self.FORK_HEIGHT + 1, 501):
                block_hash = b.getblockhash(height)
                block_hex = b.getblock(block_hash, 0)
                result = a.submitblock(block_hex)
                submissions.append({
                    "height": height, "hash": block_hash, "result": result,
                    "active_tip_after_submission": a.getbestblockhash(),
                })
                # A stored side-chain block has no BlockChecked callback until
                # it connects, so submitblock can return "inconclusive" while
                # this branch still has less work. Reject every other string.
                if height < 500:
                    assert result in (None, "inconclusive"), (height, result)
                else:
                    # This final block gives B strictly more work. It must
                    # connect and produce a conclusive valid result.
                    assert_equal(result, None)
                assert_equal(a.getblock(block_hash, 0), block_hex)
        finally:
            self.remember("competing_block_submissions", submissions)
        assert_equal(a.getbestblockhash(), b.getbestblockhash())
        assert_equal(a.getblockhash(self.FORK_HEIGHT), common_hash)
        assert a.getblockhash(260) != self.observations["branch_a_samples"][14]["hash"]
        assert_equal(a.getblockhash(260), self.observations["branch_b_samples"][14]["hash"])
        assert_equal(a.getrawmempool(), [])
        self.reference(a, self.CANDIDATE_PRICE, self.NEW_REFERENCE, False, "branch_a_after_ancestor_reorg")
        result = a.testmempoolaccept([raw])[0]
        self.remember("same_mint_accepted_after_reorg", result)
        assert_equal(result["allowed"], True)
        assert_equal(a.sendrawtransaction(raw), txid)
        self.mine_to(a, 501, owner_address)
        assert txid in a.getblock(a.getbestblockhash())["tx"]
        assert_equal(b.submitblock(a.getblock(a.getbestblockhash(), 0)), None)
        assert_equal(a.getbestblockhash(), b.getbestblockhash())

        self.log.info("Restart the activated replacement branch and retain its reference")
        self.restart_node(0, self.extra_args[0])
        assert_equal(a.getbestblockhash(), b.getbestblockhash())
        self.reference(a, self.CANDIDATE_PRICE, self.NEW_REFERENCE, False, "activated_replacement_after_restart")
        assert_equal(a.verifychain(4, 8), True)

        self.log.info("Remove a pending mint when a reorg changes its price reference")
        # Fund this mint only from the common chain. It must become invalid
        # because of the new price reference, not because its inputs disappear.
        recent_coins = [{"txid": coin["txid"], "vout": coin["vout"]}
                        for coin in a.listunspent()
                        if coin["confirmations"] < a.getblockcount() - self.FORK_HEIGHT + 1]
        if recent_coins:
            assert_equal(a.lockunspent(False, recent_coins), True)
        pending = a.mintdigidollar(self.PRINCIPAL, 0)
        pending_id = pending["txid"]
        assert pending_id in a.getrawmempool()
        pending_raw = a.getrawtransaction(pending_id)
        inputs = a.decoderawtransaction(pending_raw)["vin"]
        for txin in inputs:
            coin = a.gettxout(txin["txid"], txin["vout"], False)
            assert coin is not None
            assert coin["confirmations"] >= a.getblockcount() - self.FORK_HEIGHT + 1

        # Give B the original A branch, then extend it beyond both current tips.
        # B is disconnected, so A keeps its pending mint until the real reorg.
        replaced_branch_start = b.getblockhash(self.FORK_HEIGHT + 1)
        for block_hash in old_branch:
            assert b.submitblock(a.getblock(block_hash, 0)) in (None, "inconclusive")
        b.invalidateblock(replaced_branch_start)
        assert_equal(b.getbestblockhash(), old_tip)
        self.quote(b, self.CANDIDATE_PRICE)
        new_blocks = []
        for _ in range(3):
            self.clock()
            new_blocks.append(self.generateblock(b, sink_address, [], sync_fun=self.no_op)["hash"])
        for block_hash in new_blocks:
            assert a.submitblock(b.getblock(block_hash, 0)) in (None, "inconclusive")
        assert_equal(a.getbestblockhash(), b.getbestblockhash())
        assert_equal(a.getblockcount(), 502)
        self.reference(a, self.CANDIDATE_PRICE, self.OLD_REFERENCE, True, "pending_mint_reorg_reference")
        assert pending_id not in a.getrawmempool()
        for txin in inputs:
            assert a.gettxout(txin["txid"], txin["vout"], False) is not None
        self.reject_mint_bytes(a, pending_raw, "pending_mint_rejected_after_reorg")
        self.remember("result", {"status": "PASS", "confirmed_mint_before_reorg": txid,
                                 "evicted_mint": pending_id, "height": 502})


if __name__ == "__main__":
    DigiDollarThawDayReferenceReorgTest().main()
