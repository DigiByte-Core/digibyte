#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test sequential Paymaster fallback and one-slot client contention.

Two independent providers advertise differently priced USER_PAID offers. One
client reserves the cheap provider's only operational slot without signing.
A second client initially selects that provider, explicitly abandons only its
unsigned attempt, and completes through the standby provider with the exact
same DD input set. The test also pins the no-fallback boundary after a user
PSBT exists and proves that only one payment is committed and accounted.
"""

from test_framework.paymaster import (
    default_liquidity_policy,
    paymaster_node_args,
    provider_safety_policy,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class PaymasterFailoverTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(provider_node=0),
            paymaster_node_args(provider_node=1),
            paymaster_node_args(),
            paymaster_node_args(),
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    @staticmethod
    def policy(fee_rate_bps):
        return {
            "funding_models": ["user_paid"],
            "sponsorship_scope": "public",
            "fee_rate_bps": fee_rate_bps,
            "min_amount_cents": 100,
            "max_amount_cents": 100_000,
            "quote_ttl": 60,
            "maximum_network_fee_dgb_satoshis": 20_000_000,
        }

    def configure_provider(self, wallet, cli, display_name, fee_rate_bps):
        identity = wallet.createpaymasteridentity(display_name)
        persisted = cli.setpaymasterpolicy(self.policy(fee_rate_bps))
        assert_equal(persisted["fee_rate_bps"], fee_rate_bps)
        cli.setpaymastersafetypolicy(provider_safety_policy(["user_paid"]))
        assert_equal(cli.setpaymasterenabled(True)["enabled"], True)
        wallet.setpaymasterruntimesettings({
            "operation_mode": "manual",
            "autostart": False,
        })
        pool_targets = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 1,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 1,
        }
        pool_preview = cli.preparepaymasterpool(pool_targets)
        pool_targets["execute"] = True
        pool_targets["plan_id"] = pool_preview["plan_id"]
        prepared = cli.preparepaymasterpool(pool_targets)
        assert_equal(prepared["executed"], True)
        wallet.setpaymasterliquiditypolicy(
            default_liquidity_policy(carriers=True))
        return identity

    @staticmethod
    def reserved_operational_entries(wallet):
        return [
            entry for entry in wallet.getpaymasterpoolinfo()["pool"]
            if entry["purpose"] == "operational" and
            entry["state"] == "reserved"
        ]

    def wait_for_quote(self, client, provider, recipient, request_id,
                       expected_provider_id, maximum_provider_attempts=2):
        options = {
            "fee_mode": "paymaster",
            "request_id": request_id,
            "maximum_paymaster_fee_cents": 100,
            "privacy": "standard",
            "selection": "lowest_total_cost",
            "maximum_provider_attempts": maximum_provider_attempts,
        }

        def send():
            return client.senddigidollar(
                recipient, 1_000, "", 0, None, "cents", options)

        initial = send()
        assert_equal(initial["provider_id"], expected_provider_id)
        capacity_seen = False
        quote = {}

        def quote_ready():
            nonlocal capacity_seen, quote
            candidate = send()
            assert_equal(candidate["provider_id"], expected_provider_id)
            response = provider.processpaymasterrequests()
            if not response["processed"]:
                return False
            assert_equal(response["queued"], True)
            assert_equal(response["provider_id"], expected_provider_id)
            if response["message_type"] == "capacity":
                capacity_seen = True
                return False
            assert_equal(response["message_type"], "quote")
            assert_equal(response["request_id"], request_id)
            quote = response
            return True

        self.wait_until(quote_ready)
        assert_equal(capacity_seen, True)
        return options, quote, send

    def authorize(self, provider, options, send):
        prepared = {}

        def authorization_ready():
            nonlocal prepared
            prepared = send()
            if prepared.get("authorization_required", False):
                return True
            replay = provider.processpaymasterrequests()
            if replay["processed"]:
                assert_equal(replay["queued"], True)
            return False

        self.wait_until(authorization_ready)
        options["authorization_commitment"] = prepared[
            "authorization_commitment"]
        authorized = send()
        assert_equal(authorized["authorization_accepted"], True)
        if "psbt" not in authorized:
            authorized = send()
        assert "psbt" in authorized
        return authorized

    def run_test(self):
        cheap_node, standby_node, _, client_node = self.nodes

        self.log.info("Create two providers, two contending clients and a recipient")
        cheap_node.createwallet(
            wallet_name="cheap-provider", descriptors=True,
            load_on_startup=True)
        standby_node.createwallet(
            wallet_name="standby-provider", descriptors=True,
            load_on_startup=True)
        for name in ("blocking-client", "fallback-client", "recipient"):
            client_node.createwallet(
                wallet_name=name, descriptors=True, load_on_startup=True)

        cheap = cheap_node.get_wallet_rpc("cheap-provider")
        standby = standby_node.get_wallet_rpc("standby-provider")
        blocking = client_node.get_wallet_rpc("blocking-client")
        fallback_client = client_node.get_wallet_rpc("fallback-client")
        recipient = client_node.get_wallet_rpc("recipient")
        cheap_cli = cheap_node.cli("-rpcwallet=cheap-provider")
        standby_cli = standby_node.cli("-rpcwallet=standby-provider")

        self.log.info("Fund all wallets while both clients remain DGB-less")
        self.generatetoaddress(
            cheap_node, 110, cheap.getnewaddress(address_type="bech32m"))
        self.sync_blocks()
        for node in self.nodes:
            node.setmockoracleprice(500_000)

        cheap.sendtoaddress(
            standby.getnewaddress(address_type="bech32m"), 3)
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()

        minted = cheap.mintdigidollar(8_000, 0)
        assert_equal(minted["dd_minted"], 8_000)
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()
        for wallet, amount in (
                (standby, 2_000), (blocking, 1_500),
                (fallback_client, 1_500)):
            transfer = cheap.senddigidollar(
                wallet.getdigidollaraddress(), amount)
            assert_equal(len(transfer["txid"]), 64)
            self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
            self.sync_blocks()
        assert_equal(blocking.getbalance(), 0)
        assert_equal(fallback_client.getbalance(), 0)

        for client in (blocking, fallback_client):
            client.setpaymasterclientsafetypolicy({
                "maximum_service_fee_per_transaction_cents": 100,
                "maximum_service_fee_per_day_cents": 10_000,
            })

        cheap_identity = self.configure_provider(
            cheap, cheap_cli, "Capacity-limited cheap provider", 50)
        standby_identity = self.configure_provider(
            standby, standby_cli, "Standby provider", 200)
        self.sync_mempools()
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()

        assert_equal(cheap.startpaymaster()["ready"], True)
        assert_equal(standby.startpaymaster()["ready"], True)
        provider_ids = {
            cheap_identity["provider_id"],
            standby_identity["provider_id"],
        }
        self.wait_until(lambda: {
            entry["provider_id"] for entry in client_node.listpaymasters()
        }.issuperset(provider_ids))

        recipient_address = recipient.getdigidollaraddress()
        self.log.info("First client reserves the cheap provider's only slot")
        blocking_request = "550e8400-e29b-41d4-a716-446655442101"
        _, blocking_quote, _ = self.wait_for_quote(
            blocking, cheap, recipient_address, blocking_request,
            cheap_identity["provider_id"])
        blocking_inputs = blocking.resolvepaymastersession(
            {"request_id": blocking_request}, "refresh"
        )["session"]["reserved_user_inputs"]
        assert blocking_inputs
        cheap_reserved = self.reserved_operational_entries(cheap)
        assert_equal(len(cheap_reserved), 2)
        assert_equal(
            {entry["asset"] for entry in cheap_reserved},
            {"dgb", "dd_carrier"})
        assert_equal(self.reserved_operational_entries(standby), [])

        self.log.info("Second client falls back without changing reserved DD inputs")
        fallback_request = "550e8400-e29b-41d4-a716-446655442102"
        fallback_options = {
            "fee_mode": "paymaster",
            "request_id": fallback_request,
            "maximum_paymaster_fee_cents": 100,
            "privacy": "standard",
            "selection": "lowest_total_cost",
            "maximum_provider_attempts": 2,
        }

        def fallback_send():
            return fallback_client.senddigidollar(
                recipient_address, 1_000, "", 0, None, "cents", fallback_options)

        first_attempt = fallback_send()
        assert_equal(
            first_attempt["provider_id"], cheap_identity["provider_id"])
        lookup = {"request_id": fallback_request}
        before_fallback = fallback_client.resolvepaymastersession(
            lookup, "refresh")
        original_inputs = before_fallback["session"]["reserved_user_inputs"]
        assert original_inputs
        assert_equal(before_fallback["session"]["provider_attempts"], 1)

        abandoned_attempt = fallback_client.resolvepaymastersession(
            lookup, "fallback")
        assert_equal(abandoned_attempt["attempt"]["attempt_state"], "REJECTED")
        assert_equal(abandoned_attempt["artifact"], "none")
        assert "refresh" in abandoned_attempt["allowed_actions"]
        assert "fallback" not in abandoned_attempt["allowed_actions"]
        assert_equal(abandoned_attempt["recovery"], None)
        assert_equal(
            abandoned_attempt["session"]["session_state"],
            "INPUTS_RESERVED")
        assert_equal(
            abandoned_attempt["session"]["reserved_user_inputs"],
            original_inputs)

        # The node intentionally keeps one short-lived authenticated direct
        # channel at a time. Close the failed provider's channel so the next
        # sequential attempt can authenticate the standby endpoint.
        direct_peers = [
            peer for peer in client_node.getpeerinfo()
            if peer["connection_type"] == "paymaster"
        ]
        assert_equal(len(direct_peers), 1)
        client_node.disconnectnode(nodeid=direct_peers[0]["id"])
        self.wait_until(lambda: not any(
            peer["connection_type"] == "paymaster"
            for peer in client_node.getpeerinfo()
        ))

        second_attempt = fallback_send()
        assert_equal(
            second_attempt["provider_id"], standby_identity["provider_id"])
        capacity_seen = False
        standby_quote = {}

        def standby_quote_ready():
            nonlocal capacity_seen, standby_quote
            candidate = fallback_send()
            assert_equal(
                candidate["provider_id"], standby_identity["provider_id"])
            response = standby.processpaymasterrequests()
            if not response["processed"]:
                return False
            assert_equal(response["queued"], True)
            if response["message_type"] == "capacity":
                capacity_seen = True
                return False
            assert_equal(response["message_type"], "quote")
            assert_equal(response["request_id"], fallback_request)
            standby_quote = response
            return True

        self.wait_until(standby_quote_ready)
        assert_equal(capacity_seen, True)
        assert_equal(
            standby_quote["provider_id"], standby_identity["provider_id"])
        after_fallback = fallback_client.resolvepaymastersession(
            lookup, "refresh")
        assert_equal(after_fallback["session"]["provider_attempts"], 2)
        assert_equal(
            after_fallback["session"]["reserved_user_inputs"],
            original_inputs)
        assert_equal(len(self.reserved_operational_entries(cheap)), 2)
        assert_equal(len(self.reserved_operational_entries(standby)), 2)

        self.log.info("Signing closes the ordinary provider-fallback boundary")
        authorized = self.authorize(
            standby, fallback_options, fallback_send)
        assert_equal(
            authorized["provider_id"], standby_identity["provider_id"])
        assert_raises_rpc_error(
            -4, "PAYMASTER_FALLBACK_AUTHORIZATION_MAY_EXIST",
            fallback_client.resolvepaymastersession, lookup, "fallback")

        commit = {}

        def submit_ready():
            nonlocal commit
            commit = standby.processpaymastersubmits()
            if commit["processed"]:
                return True
            fallback_send()
            return False

        self.wait_until(submit_ready)
        assert_equal(commit["queued"], True)
        assert_equal(commit["request_id"], fallback_request)
        assert_equal(commit["provider_id"], standby_identity["provider_id"])
        txid = commit["txid"]
        assert_equal(set(standby_node.getrawmempool()), {txid})

        result = {}

        def result_ready():
            nonlocal result
            result = fallback_client.processpaymasterresult(fallback_request)
            return result["processed"]

        self.wait_until(result_ready)
        assert_equal(result["txid"], txid)
        assert_equal(result["provider_id"], standby_identity["provider_id"])
        assert_equal(result["service_fee_cents"], 20)
        self.wait_until(lambda: all(
            txid in node.getrawmempool() for node in self.nodes))
        self.generatetoaddress(standby_node, 1, standby.getnewaddress())
        self.sync_blocks()
        self.wait_until(
            lambda: recipient.getdigidollarbalance()["total"] == 1_000)
        assert_equal(fallback_client.getdigidollarbalance()["total"], 480)
        assert_equal(fallback_client.getbalance(), 0)

        self.log.info("Release the unsigned contender without financial effects")
        canceled = cheap.cancelpaymasterquote(blocking_quote["attempt_id"])
        assert_equal(canceled["attempt_state"], "REJECTED")
        abandoned_session = blocking.resolvepaymastersession(
            {"request_id": blocking_request}, "abandon_unsigned")
        assert_equal(abandoned_session["session"]["session_state"], "FAILED")
        assert_equal(abandoned_session["session"]["final"], True)
        assert_equal(abandoned_session["artifact"], "none")
        assert_equal(abandoned_session["allowed_actions"], ["refresh"])
        assert_equal(
            abandoned_session["session"]["reserved_user_inputs"],
            blocking_inputs)
        assert_equal(
            blocking.getpaymasterclientsafetystatus()["active_reservations"],
            0)
        assert_equal(blocking.getdigidollarbalance()["total"], 1_500)
        assert_equal(self.reserved_operational_entries(cheap), [])

        cheap_finance = cheap.getpaymasterfinancestatus({"period": "all"})
        assert_equal(cheap_finance["successful_transfers"], 0)
        assert_equal(cheap_finance["service_fee_income_cents"], 0)
        self.wait_until(lambda: standby.getpaymasterfinancestatus({
            "period": "all",
        })["successful_transfers"] == 1)
        standby_finance = standby.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10,
        })
        assert_equal(standby_finance["successful_transfers"], 1)
        assert_equal(standby_finance["service_fee_income_cents"], 20)
        transfer_events = [
            event for event in standby_finance["events"]
            if event["kind"] == "transfer"
        ]
        assert_equal(len(transfer_events), 1)
        assert_equal(transfer_events[0]["transaction_id"], txid)
        assert_equal(transfer_events[0]["state"], "confirmed")


if __name__ == "__main__":
    PaymasterFailoverTest().main()
