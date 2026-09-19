#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Crash/backup recovery, idempotency, and lock-state tests for Paymaster work."""

from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    default_liquidity_policy,
    paymaster_node_args,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class PaymasterLifecycleTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(provider_node=0),
            paymaster_node_args(),
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        harness = PaymasterFunctionalHarness(self)
        provider, client = harness.create_wallets()
        harness.fund_and_configure(
            operation_mode="manual", carrier_cents=1_000)

        self.log.info("An authorized submit survives an abrupt provider process exit")
        harness.fund_client_dd(500)
        assert_equal(provider.startpaymaster()["running"], True)
        recipient = provider.getdigidollaraddress()
        request_id = "72f58f36-babf-4e84-a830-f3e055f5bf54"
        options, _, resume_send = harness.wait_for_quote(
            request_id, recipient, 100)
        authorized = harness.authorize_quote(options, resume_send)
        assert_equal(authorized["session_state"], "PENDING_PROVIDER")
        mempool_before = set(self.nodes[0].getrawmempool())

        # Preserve an older real SQLite image containing the authorized PSBT,
        # but no provider result. It will be restored after the payment confirms.
        authorized_backup = self.nodes[1].datadir_path / "authorized-client.bak"
        client.backupwallet(authorized_backup)
        signed_session = client.getdigidollarsendsession({"request_id": request_id})

        # Kill only this test's child process, without RPC shutdown/DB flushing.
        # Wallet transactions already committed by the quote must survive.
        provider_node = self.nodes[0]
        provider_node.process.kill()
        crash_exit = provider_node.process.wait(timeout=60 * self.options.timeout_factor)
        assert crash_exit != 0
        assert provider_node.is_node_stopped(expected_ret_code=crash_exit)
        self.start_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        provider = self.nodes[0].get_wallet_rpc("provider")
        provider.setpaymasterruntimesettings({
            "operation_mode": "automatic",
            "autostart": False,
        })
        assert_equal(provider.startpaymaster()["running"], True)

        completed = {}

        def automatic_submit_completed():
            nonlocal completed
            completed = resume_send()
            return (completed.get("processed", False) and
                    completed.get("session_state") == "MEMPOOL")

        self.wait_until(automatic_submit_completed)
        # The session RPC keeps nullable fields present. Once a signed result
        # exists, its status and sequence must have their documented types.
        resolved = client.resolvepaymastersession(
            {"request_id": request_id}, "refresh")
        assert isinstance(resolved["attempt"], dict)
        assert resolved["recovery"] is None
        assert isinstance(resolved["result_status"], str)
        assert type(resolved["result_sequence"]) is int
        payment_txid = completed["txid"]
        assert payment_txid in self.nodes[0].getrawmempool()
        assert_equal(
            set(self.nodes[0].getrawmempool()) - mempool_before,
            {payment_txid})

        # Exact retries are idempotent both before and after confirmation.
        replayed = resume_send()
        assert_equal(replayed["txid"], payment_txid)
        assert_equal(len(self.nodes[0].getrawmempool()), 1)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(lambda: client.getdigidollarsendsession({
            "request_id": request_id,
        })["session_state"] == "CONFIRMED")
        confirmed = resume_send()
        assert_equal(confirmed["txid"], payment_txid)
        # The provider's 50 bps policy rounds this one-dollar payment to one
        # cent, charged in addition to the recipient amount.
        assert_equal(client.getdigidollarbalance()["total"], 399)

        self.log.info("An older authorized client backup cannot create a second payment")
        provider.stoppaymaster()
        finance_before_restore = provider.getpaymasterfinancestatus({"limit": 100})
        # Valuation time is generated by each RPC, not a durable ledger field.
        finance_before_restore.pop("valuation_time")
        self.sync_mempools()
        mempools_before_restore = [set(node.getrawmempool()) for node in self.nodes]
        self.nodes[1].unloadwallet("client")
        self.nodes[1].restorewallet("authorized_restore", authorized_backup)
        restored_client = self.nodes[1].get_wallet_rpc("authorized_restore")
        try:
            # Never run two loaded copies of the same wallet during this check.
            harness.client = restored_client
            restored_session = restored_client.getdigidollarsendsession({
                "request_id": request_id})
            assert_equal(restored_session["session_id"], signed_session["session_id"])
            assert_equal(restored_session["reserved_user_inputs"],
                         signed_session["reserved_user_inputs"])
            restored_budget = restored_client.getpaymasterclientsafetystatus()
            restored_reservations = restored_client.listpaymasterreservations()
            # This image has no signed provider result. It cannot reconstruct
            # the off-chain completion merely from a rescan, and must not reuse
            # the already spent Capacity inputs or silently sign a new payment.
            assert_raises_rpc_error(
                -4, "PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE", resume_send)
            assert_equal(restored_client.getdigidollarsendsession({
                "request_id": request_id}), restored_session)
            assert_equal(restored_client.getpaymasterclientsafetystatus(), restored_budget)
            assert_equal(restored_client.listpaymasterreservations(), restored_reservations)
            assert_equal(restored_client.gettransaction(payment_txid)["confirmations"], 1)
            assert_equal(restored_client.getdigidollarbalance()["total"], 399)
            assert_equal([set(node.getrawmempool()) for node in self.nodes],
                         mempools_before_restore)
            finance_after_restore = provider.getpaymasterfinancestatus({"limit": 100})
            finance_after_restore.pop("valuation_time")
            assert_equal(finance_after_restore, finance_before_restore)
        finally:
            self.nodes[1].unloadwallet("authorized_restore")
            self.nodes[1].loadwallet("client")
            client = self.nodes[1].get_wallet_rpc("client")
            harness.client = client

        self.log.info("Pending replenishment counts toward targets across restart")
        expanded = default_liquidity_policy(carriers=True)
        expanded.update({
            "target_admission_dgb": 4,
            "target_operational_dgb": 2,
            "target_admission_carriers": 4,
            "target_operational_carriers": 2,
        })
        provider.setpaymasterliquiditypolicy(expanded)
        assert_equal(provider.startpaymaster()["running"], True)

        self.wait_until(lambda: all(
            provider.getpaymasterliquiditystatus()[key]["missing"] == 0
            for key in ("admission_dgb", "operational_dgb",
                        "admission_carriers", "operational_carriers")))
        first_mempool = set(self.nodes[0].getrawmempool())
        assert len(first_mempool) > 0
        first_status = provider.getpaymasterliquiditystatus()
        pending_before = sum(first_status[key]["pending"] for key in (
            "admission_dgb", "operational_dgb", "admission_carriers",
            "operational_carriers"))
        assert pending_before > 0
        for key in ("admission_dgb", "operational_dgb",
                    "admission_carriers", "operational_carriers"):
            counts = first_status[key]
            assert counts["ready"] + counts["pending"] <= counts["target"]

        self.restart_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        provider = self.nodes[0].get_wallet_rpc("provider")
        assert_equal(provider.getpaymasterinfo()["running"], False)
        assert_equal(set(self.nodes[0].getrawmempool()), first_mempool)
        assert_equal(provider.startpaymaster()["running"], True)

        # Several scheduler passes after a restart must continue the stored
        # operation rather than create another transaction for the same gap.
        self.wait_until(lambda: provider.getpaymasterliquiditystatus()[
            "maintenance_fee_reserved_satoshis"] > 0)
        assert_equal(set(self.nodes[0].getrawmempool()), first_mempool)

        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(lambda: all(
            provider.getpaymasterliquiditystatus()[key]["missing"] == 0
            for key in ("admission_dgb", "operational_dgb",
                        "admission_carriers", "operational_carriers")))
        final_status = provider.getpaymasterliquiditystatus()
        for key in ("admission_dgb", "operational_dgb",
                    "admission_carriers", "operational_carriers"):
            counts = final_status[key]
            assert_equal(counts["ready"], counts["target"])
            assert_equal(counts["pending"], 0)

        self.log.info("Locked autostart pauses a real replenishment gap")
        provider.stoppaymaster()
        locked_targets = dict(expanded)
        locked_targets.update({
            "target_admission_dgb": 5,
            "target_operational_dgb": 3,
        })
        provider.setpaymasterliquiditypolicy(locked_targets)
        reservations_before_lock = provider.listpaymasterreservations()
        provider.encryptwallet("paymaster lifecycle passphrase")
        settings = provider.setpaymasterruntimesettings({
            "operation_mode": "automatic",
            "autostart": True,
        })
        assert_equal(settings["autostart"], True)
        self.restart_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        provider = self.nodes[0].get_wallet_rpc("provider")
        locked = provider.getpaymasterinfo()
        assert_equal(locked["running"], False)
        assert_equal(locked["service_state"], "waiting_for_unlock")
        assert_equal(provider.listpaymasterreservations(),
                     reservations_before_lock)
        locked_status = provider.getpaymasterliquiditystatus()
        assert any(locked_status[key]["missing"] > 0 for key in (
            "admission_dgb", "operational_dgb",
            "admission_carriers", "operational_carriers"))
        assert_equal(self.nodes[0].getrawmempool(), [])

        provider.walletpassphrase("paymaster lifecycle passphrase", 60)
        self.wait_until(lambda: (
            provider.getpaymasterinfo()["running"] and
            provider.getpaymasterinfo()["service_state"] == "active"))
        self.wait_until(lambda: len(self.nodes[0].getrawmempool()) == 1)
        maintenance_txid = self.nodes[0].getrawmempool()[0]
        unlocked_status = provider.getpaymasterliquiditystatus()
        assert all(unlocked_status[key]["missing"] == 0 for key in (
            "admission_dgb", "operational_dgb",
            "admission_carriers", "operational_carriers"))

        # Repeated scheduler passes while the maintenance transaction is
        # pending must keep exactly the same transaction and slot counts.
        self.wait_until(lambda: provider.getpaymasterliquiditystatus()[
            "maintenance_fee_reserved_satoshis"] > 0)
        assert_equal(self.nodes[0].getrawmempool(), [maintenance_txid])
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(lambda: all(
            provider.getpaymasterliquiditystatus()[key]["ready"] ==
            provider.getpaymasterliquiditystatus()[key]["target"]
            for key in ("admission_dgb", "operational_dgb",
                        "admission_carriers", "operational_carriers")))
        assert not any(entry["state"] in (
            "reserved", "committed", "pending_successor")
            for entry in provider.listpaymasterreservations())


if __name__ == "__main__":
    PaymasterLifecycleTest().main()
