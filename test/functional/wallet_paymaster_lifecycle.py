#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Restart, idempotency, and lock-state tests for automatic Paymaster work."""

from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    default_liquidity_policy,
    paymaster_node_args,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


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

        self.log.info("An authorized submit survives a provider restart")
        harness.fund_client_dd(500)
        assert_equal(provider.startpaymaster()["running"], True)
        recipient = provider.getdigidollaraddress()
        request_id = "72f58f36-babf-4e84-a830-f3e055f5bf54"
        options, _, resume_send = harness.wait_for_quote(
            request_id, recipient, 100)
        authorized = harness.authorize_quote(options, resume_send)
        assert_equal(authorized["session_state"], "PENDING_PROVIDER")
        mempool_before = set(self.nodes[0].getrawmempool())

        # Restart after the client has durably authorized its exact template,
        # but before the provider consumes the submit.  The automatic service
        # must resume that work without creating a second payment or fee.
        self.restart_node(0, paymaster_node_args(provider_node=0))
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
