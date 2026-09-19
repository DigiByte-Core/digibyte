#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Direct contract tests for Paymaster wallet RPCs.

The broad provider scenario proves the complete product flow.  This file keeps
the public RPC boundaries independently testable, especially preview binding,
idempotency, redacted reservation output, and atomic release semantics.
"""

import base64
from decimal import Decimal
import time

from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    assert_snapshot_equal,
    default_liquidity_policy,
    paymaster_node_args,
    value_snapshot,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    try_rpc,
)


class PaymasterRPCContractsTest(DigiByteTestFramework):
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
        info = harness.fund_and_configure(
            operation_mode="manual", carrier_cents=4_000,
            fee_rate_bps=1_900)
        assert_equal(info["operation_mode"], "manual")
        # Keep one independent operational carrier available after the submit
        # contract spends another one.  This lets release_slot be tested
        # without depending on successor promotion timing.
        expanded_targets = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 2,
        }
        expanded_preview = harness.provider_cli.preparepaymasterpool(
            expanded_targets)
        expanded_targets["execute"] = True
        expanded_targets["plan_id"] = expanded_preview["plan_id"]
        expanded_pool = harness.provider_cli.preparepaymasterpool(
            expanded_targets)
        assert_equal(expanded_pool["executed"], True)
        expanded_liquidity = default_liquidity_policy(carriers=True)
        expanded_liquidity.update({
            "target_operational_dgb": 2,
            "target_operational_carriers": 2,
            "maximum_maintenance_fee_per_transaction_satoshis": 200_000_000,
            "maximum_maintenance_fee_per_hour_satoshis": 500_000_000,
            "maximum_maintenance_fee_per_day_satoshis": 1_000_000_000,
        })
        provider.setpaymasterliquiditypolicy(expanded_liquidity)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        harness.fund_client_dd(1_500)

        # Use a third wallet as an independent recipient so every value-moving
        # assertion distinguishes client change from recipient value.
        self.nodes[0].createwallet(
            wallet_name="recipient", descriptors=True, load_on_startup=True)
        recipient_wallet = self.nodes[0].get_wallet_rpc("recipient")
        recipient = recipient_wallet.getdigidollaraddress()

        started = provider.startpaymaster()
        assert_equal(started["running"], True)
        self.wait_until(lambda: len(client.getpaymasteroffers(500)) > 0)

        self.log.info("Client session inbox is bounded and read-only")
        empty_inbox = client.listdigidollarsendsessions()
        assert_equal(empty_inbox["active_only"], True)
        assert_equal(empty_inbox["count"], 0)
        assert_equal(empty_inbox["sessions"], [])
        assert "next_cursor" not in empty_inbox
        assert_raises_rpc_error(
            -8, "limit must be between 1 and 100",
            client.listdigidollarsendsessions, {"limit": 0})
        assert_raises_rpc_error(
            -8, "cursor must be a canonical lowercase request UUID",
            client.listdigidollarsendsessions, {"cursor": "not-a-cursor"})

        self.log.info("Reject malformed client PSBTs at the direct RPC boundary")
        assert_raises_rpc_error(
            -22, "Paymaster PSBT decode failed",
            client.walletprocesspaymasterpsbt, "not-a-psbt")

        self.log.info("Reject malformed inputs at the remaining public RPC boundaries")
        assert_raises_rpc_error(
            -8, "", client.requestpaymasterquote, "00", {})
        assert_raises_rpc_error(
            -8, "", client.resolvepaymastersession,
            {"request_id": "not-a-canonical-request-id"}, "refresh")
        assert_raises_rpc_error(
            -8, "", provider.createrestrictedpaymasterdescriptor,
            "00", "invalid sponsor", 300)
        assert_raises_rpc_error(
            -8, "request_id must be a canonical lowercase UUID",
            client.processpaymasterresult, "not-a-canonical-request-id")

        # Rebalancing defaults to a side-effect-free preview.  Calling the
        # boundary directly with the current targets also proves that a no-op
        # retirement cannot mutate the live pool.
        before_rebalance = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        rebalance = provider.rebalancepaymasterpool({
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 2,
        })
        assert_equal(rebalance["executed"], False)
        assert_snapshot_equal(
            before_rebalance,
            value_snapshot(self.nodes[0], provider, client, recipient_wallet))

        self.log.info("Unsigned quote cancellation releases pool and budget atomically")
        first_options, quote, _ = harness.wait_for_quote(
            "550e8400-e29b-41d4-a716-446655441001", recipient, 500)
        assert "attempt_id" in quote

        first_lookup = {
            "request_id": "550e8400-e29b-41d4-a716-446655441001"}
        first_snapshot = client.resolvepaymastersession(
            first_lookup, "refresh")
        assert_equal(first_snapshot["action"], "refresh")
        assert_equal(first_snapshot["artifact"], "none")
        assert first_snapshot["attempt"] is not None
        assert first_snapshot["recovery"] is None
        assert_equal(first_snapshot["session"]["to_address"], recipient)
        assert "refresh" in first_snapshot["allowed_actions"]
        assert "resume" in first_snapshot["allowed_actions"]
        assert "abandon_unsigned" in first_snapshot["allowed_actions"]
        assert "retry_same" not in first_snapshot["allowed_actions"]
        assert_equal(first_snapshot["requires_attention"], False)
        assert_raises_rpc_error(
            -4, "PAYMASTER_SESSION_ACTION_NOT_ALLOWED",
            client.resolvepaymastersession, first_lookup, "retry_same")

        second_request_id = "550e8400-e29b-41d4-a716-446655441000"
        second_options = dict(first_options)
        second_options["request_id"] = second_request_id
        second_pending = client.senddigidollar(
            recipient, 200, "", 0, None, "cents", second_options)
        assert_equal(second_pending["request_id"], second_request_id)
        # The upstream amount_unit argument stays before Paymaster options.
        # Equivalent cents/dollars retries must resume the same durable request.
        dollars_retry = client.senddigidollar(
            recipient, Decimal("2.00"), "", 0, None, "dollars", second_options)
        assert_equal(dollars_retry["request_id"], second_request_id)
        assert_equal(dollars_retry["requested_amount_cents"], 200)
        assert_equal(dollars_retry["session_id"], second_pending["session_id"])


        first_page = client.listdigidollarsendsessions({"limit": 1})
        assert_equal(first_page["count"], 1)
        assert "next_cursor" in first_page
        second_page = client.listdigidollarsendsessions({
            "limit": 1,
            "cursor": first_page["next_cursor"],
        })
        assert_equal(second_page["count"], 1)
        assert_equal(
            {first_page["sessions"][0]["request_id"],
             second_page["sessions"][0]["request_id"]},
            {first_lookup["request_id"], second_request_id})
        assert "next_cursor" not in second_page
        for entry in first_page["sessions"] + second_page["sessions"]:
            assert "raw_transaction" not in entry
            assert "psbt" not in entry
            assert "authorization_commitment" not in entry
            assert_equal(entry["artifact"], "none")
            assert "allowed_actions" in entry

        self.log.info(
            "Active unsigned sessions survive wallet reload and node restart")
        persisted_request_ids = {
            first_lookup["request_id"], second_request_id}

        def unsigned_session_snapshots(wallet_rpc):
            inbox = wallet_rpc.listdigidollarsendsessions({"limit": 100})
            summaries = {
                entry["request_id"]: entry for entry in inbox["sessions"]
                if entry["request_id"] in persisted_request_ids
            }
            assert_equal(set(summaries), persisted_request_ids)
            snapshots = {}
            for request_id in sorted(persisted_request_ids):
                snapshot = wallet_rpc.resolvepaymastersession(
                    {"request_id": request_id}, "refresh")
                assert_equal(snapshot["artifact"], "none")
                assert snapshot["attempt"] is not None
                assert_equal(snapshot["recovery"], None)
                assert_equal(snapshot["result_status"], None)
                assert_equal(snapshot["result_sequence"], None)
                assert "txid" not in snapshot["session"]
                assert "recovery_txid" not in snapshot["session"]
                snapshots[request_id] = {
                    "summary": summaries[request_id],
                    "session": snapshot["session"],
                    "attempt": snapshot["attempt"],
                    "artifact": snapshot["artifact"],
                    "recovery": snapshot["recovery"],
                    "result_status": snapshot["result_status"],
                    "result_sequence": snapshot["result_sequence"],
                    "requires_attention": snapshot["requires_attention"],
                    "allowed_actions": snapshot["allowed_actions"],
                }
            return snapshots

        before_reload = unsigned_session_snapshots(client)
        mempool_before_restart = set(self.nodes[0].getrawmempool())
        client_balance_before_restart = client.getdigidollarbalance()
        client_safety_before_restart = (
            client.getpaymasterclientsafetystatus())

        self.nodes[1].unloadwallet("client")
        assert "client" not in self.nodes[1].listwallets()
        self.nodes[1].loadwallet("client")
        client = self.nodes[1].get_wallet_rpc("client")
        harness.client = client
        assert_equal(unsigned_session_snapshots(client), before_reload)
        assert_equal(set(self.nodes[0].getrawmempool()),
                     mempool_before_restart)
        assert_equal(client.getdigidollarbalance(),
                     client_balance_before_restart)
        assert_equal(client.getpaymasterclientsafetystatus(),
                     client_safety_before_restart)

        self.restart_node(1)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert "client" in self.nodes[1].listwallets()
        client = self.nodes[1].get_wallet_rpc("client")
        harness.client = client
        assert_equal(unsigned_session_snapshots(client), before_reload)
        assert_equal(set(self.nodes[0].getrawmempool()),
                     mempool_before_restart)
        assert_equal(client.getdigidollarbalance(),
                     client_balance_before_restart)
        assert_equal(client.getpaymasterclientsafetystatus(),
                     client_safety_before_restart)
        self.nodes[1].setmockoracleprice(500_000)

        abandoned_second = client.resolvepaymastersession(
            {"request_id": second_request_id}, "abandon_unsigned")
        assert_equal(abandoned_second["session"]["session_state"], "FAILED")
        assert_equal(abandoned_second["artifact"], "none")
        assert_equal(abandoned_second["recovery"], None)
        assert_equal(abandoned_second["result_status"], None)
        assert_equal(abandoned_second["result_sequence"], None)
        assert_equal(abandoned_second["allowed_actions"], ["refresh"])
        for disallowed_action in (
                "retry_same", "fallback", "cancel_to_self"):
            assert_raises_rpc_error(
                -4, "PAYMASTER_SESSION_ACTION_NOT_ALLOWED",
                client.resolvepaymastersession,
                {"request_id": second_request_id}, disallowed_action)
        active_ids = {
            entry["request_id"]
            for entry in client.listdigidollarsendsessions()["sessions"]
        }
        assert second_request_id not in active_ids
        all_ids = {
            entry["request_id"]
            for entry in client.listdigidollarsendsessions({
                "active_only": False})["sessions"]
        }
        assert second_request_id in all_ids

        reservations = provider.listpaymasterreservations()
        assert_equal(len(reservations) >= 1, True)
        required_fields = {
            "txid", "vout", "purpose", "asset", "state",
            "dgb_satoshis", "dd_cents", "confirmation_height", "updated_at",
        }
        for reservation in reservations:
            assert required_fields.issubset(reservation)
            assert reservation["state"] != "available"
            # This diagnostic RPC must not expose payment or network metadata.
            assert "recipient" not in reservation
            assert "amount" not in reservation
            assert "peer_id" not in reservation
            assert "netgroup" not in reservation

        canceled = provider.cancelpaymasterquote(quote["attempt_id"])
        assert_equal(canceled["attempt_state"], "REJECTED")
        assert_equal(provider.listpaymasterreservations(), [])
        safety = provider.getpaymastersafetystatus()["user_paid"]
        assert_equal(safety["reserved_network_fee_satoshis"], 0)
        # Exact cancellation retries are intentionally idempotent.
        assert_equal(provider.cancelpaymasterquote(quote["attempt_id"]), canceled)

        self.restart_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        self.sync_blocks()
        provider = self.nodes[0].get_wallet_rpc("provider")
        recipient_wallet = self.nodes[0].get_wallet_rpc("recipient")
        harness.provider_node = self.nodes[0]
        harness.provider = provider
        harness.provider_cli = self.nodes[0].cli("-rpcwallet=provider")
        assert_equal(provider.listpaymasterreservations(), [])
        assert_equal(provider.cancelpaymasterquote(quote["attempt_id"])[
            "attempt_state"], "REJECTED")

        # Keep the cancellation tombstone isolated from the independent submit
        # contract.  A fresh wallet must not inherit capacity evidence or
        # reservations from the first client.
        self.nodes[1].createwallet(
            wallet_name="submit_client", descriptors=True,
            load_on_startup=True)
        client = self.nodes[1].get_wallet_rpc("submit_client")
        client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10_000,
        })
        harness.client = client
        provider.senddigidollar(client.getdigidollaraddress(), 1_500)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        provider.startpaymaster()

        self.log.info("Manual submit rejects mutation and commits exact retries once")
        submit_request_id = "550e8400-e29b-41d4-a716-446655441002"
        options, _, send = harness.wait_for_quote(
            submit_request_id, recipient, 500)
        authorization = harness.authorize_quote(options, send)
        psbt = authorization["psbt"]

        authorized_snapshot = client.resolvepaymastersession(
            {"request_id": submit_request_id}, "refresh")
        assert_equal(authorized_snapshot["artifact"], "user_psbt")
        assert authorized_snapshot["attempt"] is not None
        assert authorized_snapshot["recovery"] is None
        assert_equal(
            authorized_snapshot["attempt"]["privacy_profile"], "standard")
        assert_equal(
            authorized_snapshot["session"]["to_address"], recipient)
        assert "retry_same" in authorized_snapshot["allowed_actions"]
        assert "cancel_to_self" in authorized_snapshot["allowed_actions"]
        assert "fallback" not in authorized_snapshot["allowed_actions"]
        assert "abandon_unsigned" not in authorized_snapshot["allowed_actions"]

        tampered = bytearray(base64.b64decode(psbt))
        tampered[-1] ^= 1
        assert_raises_rpc_error(
            -22, "Paymaster PSBT decode failed",
            provider.submitpaymasterdigidollar,
            base64.b64encode(tampered).decode())

        before_commit = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        assert_equal(
            before_commit.provider_budget["user_paid"]
            ["reserved_network_fee_satoshis"] > 0,
            True)
        assert_raises_rpc_error(
            -4, "", provider.cancelpaymasterquote,
            authorization["attempt_id"])
        commit = provider.submitpaymasterdigidollar(psbt)
        assert_equal(len(commit["txid"]), 64)
        assert_equal(commit["broadcast"], True)
        after_commit = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        replay = provider.submitpaymasterdigidollar(psbt)
        assert_equal(replay["txid"], commit["txid"])
        assert_equal(len(self.nodes[0].getrawmempool()), 1)
        after_replay = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        assert_equal(after_replay.mempool, frozenset({commit["txid"]}))
        # Replaying an exact durable commit cannot reserve another budget or
        # create another transaction.
        assert_equal(
            after_replay.provider_budget["user_paid"]
            ["reserved_network_fee_satoshis"],
            after_commit.provider_budget["user_paid"]
            ["reserved_network_fee_satoshis"])

        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(
            lambda: recipient_wallet.getdigidollarbalance()["total"] == 500)
        assert_equal(client.getdigidollarbalance()["total"], 905)

        # A single sub-DD fee is intentionally below the minimum ordinary DD
        # output.  Recycle the successor once more so all_excess can prove its
        # intended batching contract rather than manufacture dust.
        second_options, _, second_send = harness.wait_for_quote(
            "550e8400-e29b-41d4-a716-446655441003", recipient, 500)
        second_authorization = harness.authorize_quote(
            second_options, second_send)
        second_commit = provider.submitpaymasterdigidollar(
            second_authorization["psbt"])
        assert_equal(len(second_commit["txid"]), 64)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(
            lambda: recipient_wallet.getdigidollarbalance()["total"] == 1_000)
        assert_equal(client.getdigidollarbalance()["total"], 310)

        self.log.info("Carrier release preview is inert and plan-bound")
        self.restart_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        self.sync_blocks()
        provider = self.nodes[0].get_wallet_rpc("provider")
        recipient_wallet = self.nodes[0].get_wallet_rpc("recipient")
        harness.provider = provider
        harness.provider_cli = self.nodes[0].cli("-rpcwallet=provider")

        self.log.info("Automatic service excludes manual processing RPCs")
        provider.setpaymasterruntimesettings({
            "operation_mode": "automatic",
            "autostart": False,
        })
        provider.startpaymaster()
        assert_raises_rpc_error(
            -4, "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE",
            provider.processpaymasterrequests)
        assert_raises_rpc_error(
            -4, "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE",
            provider.processpaymastersubmits)
        provider.stoppaymaster()
        provider.setpaymasterruntimesettings({
            "operation_mode": "manual",
            "autostart": False,
        })

        self.log.info("Carrier excess withdrawal preserves every 1 DD base")
        liquidity_before_excess = provider.getpaymasterliquiditystatus()
        self.log.info(
            "Carrier accounting before withdrawal: %s",
            {
                "base": liquidity_before_excess["carrier_base_cents"],
                "excess": liquidity_before_excess[
                    "carrier_withdrawable_excess_cents"],
                "pool": [
                    (entry["purpose"], entry["state"], entry["dd_cents"])
                    for entry in provider.getpaymasterpoolinfo()["pool"]
                    if entry["asset"] == "dd_carrier"
                ],
            })
        assert_equal(
            liquidity_before_excess["carrier_withdrawable_excess_cents"] > 0,
            True)
        before_excess_preview = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        available_before_excess = provider.getdigidollarbalance()["total"]
        excess_preview = provider.withdrawpaymastercarrier({
            "mode": "all_excess",
        })
        assert_equal(excess_preview["executed"], False)
        assert_equal(excess_preview["withdrawable_excess_cents"] > 0, True)
        assert_equal(excess_preview["source_carriers"] > 0, True)
        assert_equal(
            excess_preview["retained_carrier_cents"],
            excess_preview["source_carriers"] * 100)
        assert_snapshot_equal(
            before_excess_preview,
            value_snapshot(self.nodes[0], provider, client, recipient_wallet))
        execution_preview = provider.withdrawpaymastercarrier({
            "mode": "all_excess",
        })
        excess_result = provider.withdrawpaymastercarrier({
            "mode": "all_excess",
            "execute": True,
            "plan_id": execution_preview["plan_id"],
        })
        assert_equal(excess_result["executed"], True)
        assert_equal(len(excess_result["txid"]), 64)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(lambda: provider.getpaymasterliquiditystatus()[
            "carrier_withdrawable_excess_cents"] == 0)
        assert_equal(
            provider.getdigidollarbalance()["total"],
            available_before_excess +
            excess_preview["withdrawable_excess_cents"])

        carrier = next(entry for entry in provider.getpaymasterpoolinfo()["pool"]
                       if entry["purpose"] == "operational" and
                       entry["asset"] == "dd_carrier" and
                       entry["state"] == "available")

        before_preview = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        preview = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "txid": carrier["txid"],
            "vout": carrier["vout"],
        })
        assert_equal(preview["executed"], False)
        after_preview = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        assert_snapshot_equal(before_preview, after_preview)

        release_policy = default_liquidity_policy(True)
        release_policy["target_operational_carriers"] = 2
        provider.setpaymasterliquiditypolicy(release_policy)
        assert_raises_rpc_error(
            -4, "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED",
            provider.withdrawpaymastercarrier,
            {"mode": "release_slot", "execute": True,
             "plan_id": preview["plan_id"]})

        release_preview = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "txid": carrier["txid"],
            "vout": carrier["vout"],
        })
        released = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "execute": True,
            "plan_id": release_preview["plan_id"],
        })
        assert_equal(released["executed"], True)
        assert_equal(released["operational_carrier_target"], 1)

        # Earlier transaction contract cases may also consume the operational
        # DGB slot. Restore that independent asset while one valid carrier is
        # still present, so the automatic scenario below has exactly one
        # missing carrier and cannot legitimately choose DGB maintenance.
        restored_targets = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 1,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 1,
        }
        restored_preview = harness.provider_cli.preparepaymasterpool(
            restored_targets)
        restored_targets["execute"] = True
        restored_targets["plan_id"] = restored_preview["plan_id"]
        restored_dgb = harness.provider_cli.preparepaymasterpool(
            restored_targets)
        if restored_dgb["executed"]:
            self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
            self.sync_blocks()

        # Empty the operational-carrier target deliberately.  The wallet has
        # two independently prepared carriers at this point; leaving either
        # one available would make a later target of one already satisfied and
        # would exercise DGB replenishment instead of carrier replenishment.
        remaining_carrier = next(
            entry for entry in provider.getpaymasterpoolinfo()["pool"]
            if entry["purpose"] == "operational" and
            entry["asset"] == "dd_carrier" and
            entry["state"] == "available")
        final_release_preview = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "txid": remaining_carrier["txid"],
            "vout": remaining_carrier["vout"],
        })
        released = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "execute": True,
            "plan_id": final_release_preview["plan_id"],
        })
        assert_equal(released["executed"], True)
        assert_equal(released["operational_carrier_target"], 0)

        self.log.info(
            "A released final carrier is a deliberate stopped target, not an automatic repair")
        provider.setpaymasterruntimesettings({
            "operation_mode": "automatic",
            "autostart": False,
        })
        released_liquidity = provider.getpaymasterliquiditystatus()
        assert_equal(
            released_liquidity["targets_satisfy_provider_policy"], False)
        assert_equal(
            released_liquidity["maintenance_state"],
            "waiting_for_target_configuration")
        released_info = provider.getpaymasterinfo()
        assert_equal(released_info["ready"], False)
        assert (
            "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE" in
            released_info["readiness_errors"])
        refused_start = provider.startpaymaster()
        assert_equal(refused_start["running"], False)
        assert_equal(refused_start["ready"], False)
        assert (
            "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE" in
            refused_start["readiness_errors"])

        def carrier_policy(*, automatic=True, approved=True,
                           per_transaction=200_000_000,
                           per_hour=500_000_000,
                           per_day=1_000_000_000):
            """Return one explicit operational-carrier recovery policy."""
            policy = default_liquidity_policy(carriers=True)
            policy.update({
                "automatic_replenishment": automatic,
                "paid_maintenance_approved": approved,
                "maximum_maintenance_fee_per_transaction_satoshis":
                    per_transaction,
                "maximum_maintenance_fee_per_hour_satoshis": per_hour,
                "maximum_maintenance_fee_per_day_satoshis": per_day,
            })
            return policy

        def set_liquidity_policy_when_idle(policy):
            """Wait for bounded in-flight work after stopping the provider."""
            self.wait_until(lambda: not try_rpc(
                -4, "PAYMASTER_PROVIDER_BUSY",
                provider.setpaymasterliquiditypolicy, policy))

        def release_operational_carrier():
            """Release exactly one confirmed operational carrier and target."""
            entry = next(
                candidate
                for candidate in provider.getpaymasterpoolinfo()["pool"]
                if candidate["purpose"] == "operational" and
                candidate["asset"] == "dd_carrier" and
                candidate["state"] == "available")
            release = provider.withdrawpaymastercarrier({
                "mode": "release_slot",
                "txid": entry["txid"],
                "vout": entry["vout"],
            })
            result = provider.withdrawpaymastercarrier({
                "mode": "release_slot",
                "execute": True,
                "plan_id": release["plan_id"],
            })
            assert_equal(result["executed"], True)
            assert_equal(result["operational_carrier_target"], 0)
            return entry

        self.log.info(
            "Automatic carrier maintenance respects disabled and approval gates")
        mempool_before_maintenance = set(self.nodes[0].getrawmempool())
        provider.setpaymasterliquiditypolicy(
            carrier_policy(automatic=False, approved=True))
        disabled_start = provider.startpaymaster()
        assert_equal(disabled_start["running"], False)
        assert_equal(disabled_start["ready"], False)
        assert (
            "PAYMASTER_OPERATIONAL_SLOT_MISSING" in
            disabled_start["readiness_errors"])
        assert_equal(set(self.nodes[0].getrawmempool()),
                     mempool_before_maintenance)

        provider.setpaymasterliquiditypolicy(
            carrier_policy(automatic=True, approved=False))
        unapproved_start = provider.startpaymaster()
        assert_equal(unapproved_start["running"], True)
        assert_equal(unapproved_start["ready"], False)
        assert_equal(unapproved_start["service_state"],
                     "waiting_for_maintenance_approval")
        self.wait_until(
            lambda: provider.getpaymasterinfo()["service_state"] ==
            "waiting_for_maintenance_approval")
        assert_equal(
            provider.getpaymasterliquiditystatus()["maintenance_state"],
            "waiting_for_maintenance_approval")
        assert_equal(set(self.nodes[0].getrawmempool()),
                     mempool_before_maintenance)
        assert_equal(provider.stoppaymaster()["running"], False)

        self.log.info(
            "Automatic carrier maintenance creates one durable replacement")
        set_liquidity_policy_when_idle(carrier_policy())
        maintenance_start = provider.startpaymaster()
        assert_equal(maintenance_start["running"], True)
        assert_equal(maintenance_start["ready"], False)

        maintenance_txids = set()

        def one_carrier_maintenance_transaction():
            nonlocal maintenance_txids
            maintenance_txids = (
                set(self.nodes[0].getrawmempool()) -
                mempool_before_maintenance)
            return len(maintenance_txids) == 1

        self.wait_until(one_carrier_maintenance_transaction)
        maintenance_txid = next(iter(maintenance_txids))
        maintenance_entries = [
            entry for entry in provider.getpaymasterpoolinfo()["pool"]
            if entry["txid"] == maintenance_txid
        ]
        assert_equal(
            [(entry["purpose"], entry["asset"], entry["state"],
              entry["dd_cents"]) for entry in maintenance_entries],
            [("operational", "dd_carrier", "pending_successor", 100)])
        pending_liquidity = provider.getpaymasterliquiditystatus()
        assert_equal(pending_liquidity["operational_carriers"]["ready"], 0)
        assert_equal(pending_liquidity["operational_carriers"]["pending"], 1)
        assert_equal(pending_liquidity["operational_carriers"]["missing"], 0)
        assert_equal(pending_liquidity["maintenance_state"],
                     "waiting_for_liquidity_confirmation")

        # Pending successors count toward the target. Multiple scheduler ticks
        # must neither spend another ordinary DD input nor reserve another
        # maintenance budget while the first transaction awaits confirmation.
        duplicate_check_at = time.monotonic() + 2
        self.wait_until(lambda: time.monotonic() >= duplicate_check_at)
        assert_equal(
            set(self.nodes[0].getrawmempool()) - mempool_before_maintenance,
            maintenance_txids)

        self.log.info(
            "Pending carrier maintenance survives restart without duplication")
        self.restart_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        self.sync_blocks()
        provider = self.nodes[0].get_wallet_rpc("provider")
        harness.provider_node = self.nodes[0]
        harness.provider = provider
        harness.provider_cli = self.nodes[0].cli("-rpcwallet=provider")
        restarted_start = provider.startpaymaster()
        assert_equal(restarted_start["running"], True)
        self.wait_until(
            lambda: provider.getpaymasterinfo()["service_state"] ==
            "waiting_for_liquidity_confirmation")
        assert_equal(
            set(self.nodes[0].getrawmempool()) - mempool_before_maintenance,
            maintenance_txids)

        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()

        def carrier_maintenance_confirmed():
            status = provider.getpaymasterliquiditystatus()
            return (
                status["operational_carriers"]["ready"] == 1 and
                status["operational_carriers"]["pending"] == 0 and
                status["operational_carriers"]["missing"] == 0 and
                provider.getpaymasterinfo()["service_state"] == "active")

        self.wait_until(carrier_maintenance_confirmed)
        confirmed_carriers = [
            entry for entry in provider.getpaymasterpoolinfo()["pool"]
            if entry["txid"] in maintenance_txids and
            entry["purpose"] == "operational" and
            entry["asset"] == "dd_carrier" and
            entry["state"] == "available" and
            entry["dd_cents"] == 100
        ]
        assert_equal(len(confirmed_carriers), 1)

        self.log.info(
            "Rolling maintenance budget blocks a second paid replacement")
        assert_equal(provider.stoppaymaster()["running"], False)
        # Stop rejects new work but deliberately lets a bounded scheduler step
        # release its exclusive guard.  Use an idempotent policy write as a
        # barrier before the multi-RPC release sequence.
        set_liquidity_policy_when_idle(carrier_policy())
        release_operational_carrier()
        set_liquidity_policy_when_idle(carrier_policy(
            per_transaction=200_000_000,
            per_hour=200_000_000,
            per_day=200_000_000))
        budget_mempool = set(self.nodes[0].getrawmempool())
        budget_start = provider.startpaymaster()
        assert_equal(budget_start["running"], True)

        def maintenance_budget_exhausted():
            info = provider.getpaymasterinfo()
            return (
                info["service_state"] ==
                "waiting_for_maintenance_approval")

        self.wait_until(maintenance_budget_exhausted)
        assert_equal(set(self.nodes[0].getrawmempool()), budget_mempool)
        exhausted_liquidity = provider.getpaymasterliquiditystatus()
        assert_equal(exhausted_liquidity["operational_carriers"]["ready"], 0)
        assert_equal(exhausted_liquidity["operational_carriers"]["pending"], 0)
        assert_equal(exhausted_liquidity["operational_carriers"]["missing"], 1)
        assert_equal(
            exhausted_liquidity["maintenance_fee_spent_last_hour_satoshis"] >
            0,
            True)
        assert_equal(provider.stoppaymaster()["running"], False)

        self.log.info("Reputation listing and clearing have deterministic contracts")
        records = client.getpaymasterreputation()
        assert_equal(records, sorted(records, key=lambda item: item["provider_id"]))
        if records:
            provider_id = records[0]["provider_id"]
            assert_equal(len(client.getpaymasterreputation(provider_id)), 1)
            assert_equal(client.clearpaymasterreputation(provider_id)["cleared"], 1)
            assert_equal(client.getpaymasterreputation(provider_id), [])
        remaining_records = len(client.getpaymasterreputation())
        assert_equal(client.clearpaymasterreputation()["cleared"],
                     remaining_records)
        assert_equal(client.getpaymasterreputation(), [])


if __name__ == "__main__":
    PaymasterRPCContractsTest().main()
