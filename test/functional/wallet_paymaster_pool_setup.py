#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Finite pool setup survives stem-only change, restart, and missing funding.

Dandelion stays enabled deliberately: disabling it would hide the reported
fee-precheck failure. All wallets, peers and blocks belong to isolated regtest.
"""

from decimal import Decimal
import time

from test_framework.messages import CInv, msg_getdata
from test_framework.p2p import P2PInterface
from test_framework.paymaster import (
    default_liquidity_policy,
    default_provider_policy,
    paymaster_node_args,
    provider_safety_policy,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PaymasterPoolSetupTest(DigiByteTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [paymaster_node_args(provider_node=0)]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def configure(self, name, *, user_paid=True):
        node = self.nodes[0]
        node.createwallet(wallet_name=name, descriptors=True, load_on_startup=True)
        wallet = node.get_wallet_rpc(name)
        wallet.createpaymasteridentity(name)
        policy = default_provider_policy(["user_paid"] if user_paid else ["sponsored"])
        wallet.setpaymasterpolicy(policy)
        wallet.setpaymastersafetypolicy(provider_safety_policy(policy["funding_models"]))
        wallet.setpaymasterenabled(True)
        wallet.setpaymasterruntimesettings({"operation_mode": "manual", "autostart": False})
        return wallet

    @staticmethod
    def targets(user_paid=True):
        return {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 1,
            "admission_carrier_slots": 3 if user_paid else 0,
            "operational_carrier_slots": 1 if user_paid else 0,
        }

    def stem_peer(self):
        node = self.nodes[0]
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=0)
        # DigiByte marks a peer as stem-capable when it requests the discovery
        # inventory. The inert peer never fluffs transactions back to the node.
        peer.send_and_ping(msg_getdata([CInv(5, (1 << 256) - 1)]))
        return peer

    def tick(self):
        self.nodes[0].mockscheduler(31)

    def confirm(self, wallet, txid):
        self.generateblock(self.nodes[0], self.mining_address,
                           [wallet.gettransaction(txid)["hex"]], sync_fun=self.no_op)
        self.wait_until(lambda: self.nodes[0].getindexinfo()["txindex"]["synced"])

    def setup_steps(self, wallet):
        return wallet.getpaymasterpoolinfo()["preparation"]

    def wait_for(self, wallet, condition):
        def check():
            self.tick()
            return condition(self.setup_steps(wallet))
        self.wait_until(check, timeout=90)

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="miner", descriptors=True, load_on_startup=True)
        miner = node.get_wallet_rpc("miner")
        self.mining_address = miner.getnewaddress()
        self.generatetoaddress(node, 110, self.mining_address, sync_fun=self.no_op)
        node.setmockoracleprice(500_000)
        miner.mintdigidollar(4000, 0)
        self.generatetoaddress(node, 1, self.mining_address, sync_fun=self.no_op)
        provider = self.configure("setup")
        legacy = self.configure("legacy", user_paid=False)
        waiting = self.configure("waiting", user_paid=False)
        limited = self.configure("limited", user_paid=False)
        for wallet in (provider, legacy):
            miner.senddigidollar(wallet.getdigidollaraddress(), 1000)
            miner.sendtoaddress(wallet.getnewaddress(), 2)
            self.generatetoaddress(node, 1, self.mining_address, sync_fun=self.no_op)
        miner.sendtoaddress(limited.getnewaddress(), 2)
        self.generatetoaddress(node, 1, self.mining_address, sync_fun=self.no_op)

        args = [a for a in self.extra_args[0] if a != "-dandelion=0"] + ["-dandelion=1"]
        self.restart_node(0, extra_args=args)
        node.setmockoracleprice(500_000)
        node.setmocktime(max(int(time.time()), node.getblockheader(node.getbestblockhash())["time"] + 1))
        self.stem_peer()
        provider = node.get_wallet_rpc("setup")
        legacy = node.get_wallet_rpc("legacy")
        waiting = node.get_wallet_rpc("waiting")
        limited = node.get_wallet_rpc("limited")
        miner = node.get_wallet_rpc("miner")

        self.log.info("One confirmed DGB input: carrier succeeds, DGB setup waits without signing a child")
        targets = self.targets()
        default_preview = provider.preparepaymasterpool(targets)
        assert_equal(default_preview["maximum_fee_satoshis"], 20_000_000)
        assert_equal(default_preview["maximum_total_fee_satoshis"], 40_000_000)
        # The multi-output DD planner includes change, metadata and a 35%
        # margin. Give this stem test an explicit sufficient ceiling; the
        # separate low-cap case below verifies refusal without signing.
        targets["maximum_fee_satoshis"] = 30_000_000
        preview = provider.preparepaymasterpool(targets)
        assert_equal(preview["accepted"], False)
        assert_equal(preview["maximum_total_fee_satoshis"], 60_000_000)
        assert_equal(self.setup_steps(provider), [])
        execute = dict(targets, execute=True, plan_id=preview["plan_id"])
        prepared = provider.preparepaymasterpool(execute)
        assert_equal(prepared["accepted"], True)
        carrier = prepared["dd_txid"]
        assert "dgb_txid" not in prepared
        assert carrier not in node.getrawmempool()
        assert_equal(len(provider.getpaymasterpoolinfo()["pool"]), 4)
        ids = [step["operation_id"] for step in self.setup_steps(provider)]
        assert_raises_rpc_error(-8, "PAYMASTER_POOL_PLAN_CHANGED", provider.preparepaymasterpool,
                                dict(execute, operational_dgb_slots=2))
        provider.preparepaymasterpool(execute)
        assert_equal([s["operation_id"] for s in self.setup_steps(provider)], ids)
        self.confirm(provider, carrier)

        self.log.info("Restart resumes the same finite setup with autostart disabled")
        self.restart_node(0, extra_args=args)
        node.setmockoracleprice(500_000)
        node.setmocktime(max(int(time.time()), node.getblockheader(node.getbestblockhash())["time"] + 1))
        self.stem_peer()
        provider = node.get_wallet_rpc("setup")
        legacy = node.get_wallet_rpc("legacy")
        waiting = node.get_wallet_rpc("waiting")
        limited = node.get_wallet_rpc("limited")
        miner = node.get_wallet_rpc("miner")
        self.wait_for(provider, lambda steps: all("txid" in step for step in steps))
        steps = self.setup_steps(provider)
        assert_equal([s["operation_id"] for s in steps], ids)
        dgb = next(s["txid"] for s in steps if s["asset"] == "dgb")
        tx = node.decoderawtransaction(provider.gettransaction(dgb)["hex"])
        assert_equal({vin["txid"] for vin in tx["vin"]}, {carrier})
        self.confirm(provider, dgb)
        self.wait_for(provider, lambda steps: all(s["state"] == "complete" for s in steps))
        assert_equal(len(provider.getpaymasterpoolinfo()["pool"]), 8)
        assert_equal(provider.getpaymasterinfo()["running"], False)
        before = provider.getwalletinfo()["txcount"]
        replayed = provider.preparepaymasterpool(execute)
        assert_equal(replayed["maximum_total_fee_satoshis"], 60_000_000)
        self.tick()
        assert_equal(provider.getwalletinfo()["txcount"], before)

        self.log.info("Recurring carrier and DGB refill must also wait for a stem-only parent")
        provider.setpaymasterliquiditypolicy(default_liquidity_policy())
        provider.setpaymasterruntimesettings({"operation_mode": "automatic", "autostart": False})
        assert_equal(provider.startpaymaster()["running"], True)
        old_pool = provider.getpaymasterpoolinfo()["pool"]
        old_txids = {entry["txid"] for entry in old_pool}
        refill_policy = default_liquidity_policy()
        refill_policy.update(target_operational_dgb=2, target_operational_carriers=2)
        provider.setpaymasterliquiditypolicy(refill_policy)

        def refill_entries(asset):
            self.tick()
            return [entry for entry in provider.getpaymasterpoolinfo()["pool"]
                    if entry["txid"] not in old_txids and entry["asset"] == asset]

        self.wait_until(lambda: len(refill_entries("dd_carrier")) == 1)
        refill_carrier = refill_entries("dd_carrier")[0]["txid"]
        assert refill_carrier not in node.getrawmempool()
        before_refill = provider.getwalletinfo()["txcount"]
        for _ in range(2):
            self.tick()
            assert_equal(refill_entries("dgb"), [])
            assert_equal(provider.getwalletinfo()["txcount"], before_refill)
        self.confirm(provider, refill_carrier)
        self.wait_until(lambda: len(refill_entries("dgb")) == 1)
        refill_dgb = refill_entries("dgb")[0]["txid"]
        refill_raw = provider.gettransaction(refill_dgb)["hex"]
        assert_equal({vin["txid"] for vin in node.decoderawtransaction(refill_raw)["vin"]}, {refill_carrier})
        self.confirm(provider, refill_dgb)
        self.tick()
        assert_equal(provider.getwalletinfo()["txcount"], before_refill + 1)
        provider.stoppaymaster()
        provider.setpaymasterruntimesettings({"operation_mode": "manual", "autostart": False})

        self.log.info("Insufficient funding is durable and additional confirmed DGB resumes it")
        targets = self.targets(False)
        waiting.setpaymasterenabled(False)
        preview = waiting.preparepaymasterpool(targets)
        before_waiting = waiting.getwalletinfo()["txcount"]
        pending = waiting.preparepaymasterpool(dict(targets, execute=True, plan_id=preview["plan_id"]))
        assert_equal(pending["accepted"], True)
        assert_equal(pending["executed"], False)
        assert_equal(self.setup_steps(waiting)[0]["error"], "PAYMASTER_PROVIDER_DISABLED")
        self.tick()
        assert_equal(waiting.getwalletinfo()["txcount"], before_waiting)
        assert_equal(waiting.getpaymasterinfo()["enabled"], False)
        # Treat the first execute response as lost: a fresh preview and retry
        # recover the same authorization rather than another output plan.
        pending_ids = [step["operation_id"] for step in self.setup_steps(waiting)]
        retry_preview = waiting.preparepaymasterpool(targets)
        assert_equal(retry_preview["plan_id"], preview["plan_id"])
        assert_equal(retry_preview["maximum_total_fee_satoshis"], preview["maximum_total_fee_satoshis"])
        waiting.preparepaymasterpool(dict(targets, execute=True, plan_id=retry_preview["plan_id"]))
        assert_equal([step["operation_id"] for step in self.setup_steps(waiting)], pending_ids)
        assert_equal(waiting.getwalletinfo()["txcount"], before_waiting)
        assert self.setup_steps(waiting)[0]["funding_shortfall_upper_bound_satoshis"] > 0
        funding_address = waiting.getnewaddress()
        waiting.encryptwallet("pool-setup-test")
        waiting.setpaymasterenabled(False)
        funding = miner.sendtoaddress(funding_address, 2)
        self.confirm(miner, funding)
        self.tick()
        assert "txid" not in self.setup_steps(waiting)[0]
        waiting.setpaymasterenabled(True)
        self.wait_for(waiting, lambda steps: steps[0]["error"] == "PAYMASTER_WALLET_LOCKED")
        waiting.walletpassphrase("pool-setup-test", 3600)
        self.wait_for(waiting, lambda steps: "txid" in steps[0])
        self.confirm(waiting, self.setup_steps(waiting)[0]["txid"])

        self.log.info("Fee ceiling is bound to the plan and stops construction")
        low = dict(targets, maximum_fee_satoshis=1)
        preview = limited.preparepaymasterpool(low)
        assert_raises_rpc_error(-8, "PAYMASTER_POOL_PLAN_CHANGED", limited.preparepaymasterpool,
                                dict(low, maximum_fee_satoshis=20_000_000, execute=True, plan_id=preview["plan_id"]))
        accepted = limited.preparepaymasterpool(dict(low, execute=True, plan_id=preview["plan_id"]))
        assert_equal(accepted["accepted"], True)
        assert_equal(accepted["executed"], False)
        assert_equal(self.setup_steps(limited)[0]["error"], "PAYMASTER_POOL_FEE_LIMIT")
        self.tick()
        assert "txid" not in self.setup_steps(limited)[0]
        cancelled = limited.preparepaymasterpool(dict(low, execute=True, cancel=True, plan_id=preview["plan_id"]))
        assert_equal(cancelled["cancelled"], True)
        assert_raises_rpc_error(-8, "PAYMASTER_POOL_PLAN_CANCELLED", limited.preparepaymasterpool,
                                dict(low, execute=True, plan_id=preview["plan_id"]))
        approved = limited.preparepaymasterpool(targets)
        finished = limited.preparepaymasterpool(dict(targets, execute=True, plan_id=approved["plan_id"]))
        assert_equal(finished["accepted"], True)
        self.confirm(limited, finished["dgb_txid"])

        self.log.info("A real conflicting reorg must not make a saved setup permanently terminal")
        setup_txid = finished["dgb_txid"]
        setup_raw = limited.gettransaction(setup_txid)["hex"]
        setup_block = limited.gettransaction(setup_txid)["blockhash"]
        setup_inputs = node.decoderawtransaction(setup_raw)["vin"]
        assert_equal(len(setup_inputs), 1)
        parent_raw = limited.gettransaction(setup_inputs[0]["txid"])["hex"]
        parent_output = node.decoderawtransaction(parent_raw)["vout"][setup_inputs[0]["vout"]]
        parent_value = parent_output["value"]
        conflicting_raw = limited.createrawtransaction(
            [{"txid": setup_inputs[0]["txid"], "vout": setup_inputs[0]["vout"]}],
            {limited.getnewaddress(): parent_value - Decimal("0.1")})
        # The original confirmed setup already spent this coin. Supply its
        # previous output explicitly when signing the competing transaction.
        signed_conflict = limited.signrawtransactionwithwallet(conflicting_raw, [{
            "txid": setup_inputs[0]["txid"],
            "vout": setup_inputs[0]["vout"],
            "scriptPubKey": parent_output["scriptPubKey"]["hex"],
            "amount": parent_value,
        }])
        assert_equal(signed_conflict["complete"], True)
        node.invalidateblock(setup_block)
        conflict_block = self.generateblock(node, self.mining_address, [signed_conflict["hex"]],
                                           sync_fun=self.no_op)["hash"]
        self.wait_until(lambda: limited.gettransaction(setup_txid)["confirmations"] < 0)
        self.wait_for(limited, lambda steps: any(step["plan_id"] == approved["plan_id"] and
                                               step["state"] == "conflict" for step in steps))
        node.invalidateblock(conflict_block)
        # Re-enable the original block instead of regenerating its identical,
        # explicitly invalidated header at the same mocked timestamp.
        node.reconsiderblock(setup_block)
        self.wait_until(lambda: limited.gettransaction(setup_txid)["confirmations"] > 0)
        self.wait_for(limited, lambda steps: any(step["plan_id"] == approved["plan_id"] and
                                               step["state"] == "complete" for step in steps))
        assert_equal(limited.gettransaction(setup_txid)["hex"], setup_raw)
        restored_pool = [entry for entry in limited.getpaymasterpoolinfo()["pool"]
                         if entry["txid"] == setup_txid]
        assert_equal(len(restored_pool), 4)
        assert all(entry["state"] == "available" for entry in restored_pool)

        self.log.info("Explicitly adopt an old wallet-saved child rejected by the stem-parent fee precheck")
        dd_targets = {legacy.getdigidollaraddress(): 100 for _ in range(4)}
        parent = legacy.sendmanydigidollar("", dd_targets)["txid"]
        assert parent not in node.getrawmempool()
        outputs = {legacy.getnewaddress(address_type="bech32m"): Decimal("0.1") for _ in range(3)}
        operational = legacy.getnewaddress(address_type="bech32m")
        outputs[operational] = Decimal("0.2")
        # Ordinary sendmany retains the wallet transaction even though its
        # broadcast failed; its RPC currently returns that transaction's ID.
        with node.assert_debug_log(["Fee check FAILED - bad-txns-inputs-missingorspent"]):
            child = legacy.sendmany("", outputs)
        assert child not in node.getrawmempool()
        raw = legacy.gettransaction(child)["hex"]
        mapping = [{"vout": output["n"], "purpose": "operational" if output["scriptPubKey"].get("address") == operational else "admission"}
                   for output in node.decoderawtransaction(raw)["vout"]
                   if output["scriptPubKey"].get("address") in outputs]
        self.confirm(legacy, parent)
        recovery = dict(targets, recover_dgb_txid=child, recover_dgb_outputs=mapping)
        preview = legacy.preparepaymasterpool(recovery)
        before = legacy.getwalletinfo()["txcount"]
        legacy.preparepaymasterpool(dict(recovery, execute=True, plan_id=preview["plan_id"]))
        assert_equal(self.setup_steps(legacy)[0]["txid"], child)
        assert_equal(legacy.gettransaction(child)["hex"], raw)
        assert_equal(legacy.getwalletinfo()["txcount"], before)
        self.confirm(legacy, child)
        self.wait_for(legacy, lambda steps: steps[0]["state"] == "complete")
        assert_equal(len(legacy.getpaymasterpoolinfo()["pool"]), 4)
        assert_equal(legacy.getpaymasterinfo()["running"], False)


if __name__ == '__main__':
    PaymasterPoolSetupTest().main()
