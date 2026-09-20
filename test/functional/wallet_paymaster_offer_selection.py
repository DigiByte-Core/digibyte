#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test offer ordering and cheapest-provider selection across real nodes.

Two independent Paymaster daemons advertise eligible USER_PAID offers with
different fees. A third daemon hosts a DGB-less client and proves that public
discovery is sorted by exact total cost, only the cheapest provider receives
the private payment protocol, and the resulting transfer is accounted once.
"""

from test_framework.paymaster import (
    confirm_pool_preparation,
    default_liquidity_policy,
    paymaster_node_args,
    provider_safety_policy,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, p2p_port


class PaymasterOfferSelectionTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(provider_node=0),
            paymaster_node_args(provider_node=1),
            paymaster_node_args(),
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    @staticmethod
    def provider_policy(fee_rate_bps):
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
        policy = self.provider_policy(fee_rate_bps)
        persisted = cli.setpaymasterpolicy(policy)
        assert_equal(persisted["fee_rate_bps"], fee_rate_bps)
        cli.setpaymastersafetypolicy(provider_safety_policy(["user_paid"]))
        assert_equal(cli.setpaymasterenabled(True)["enabled"], True)

        runtime = wallet.setpaymasterruntimesettings({
            "operation_mode": "manual",
            "autostart": False,
        })
        assert_equal(runtime["operation_mode"], "manual")
        assert_equal(runtime["autostart"], False)

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
        prepared = confirm_pool_preparation(self, self.nodes[0], cli, pool_targets, prepared)
        assert_equal(len(prepared["dgb_txid"]), 64)
        assert_equal(len(prepared["dd_txid"]), 64)

        liquidity = wallet.setpaymasterliquiditypolicy(
            default_liquidity_policy(carriers=True))
        assert_equal(liquidity["target_operational_dgb"], 1)
        assert_equal(liquidity["target_operational_carriers"], 1)
        return identity

    @staticmethod
    def reserved_operational_entries(wallet):
        return [
            entry for entry in wallet.getpaymasterpoolinfo()["pool"]
            if entry["purpose"] == "operational" and
            entry["state"] == "reserved"
        ]

    def run_test(self):
        cheap_node, expensive_node, client_node = self.nodes

        self.log.info("Create two provider wallets and an isolated client")
        cheap_node.createwallet(
            wallet_name="cheap-provider", descriptors=True,
            load_on_startup=True)
        expensive_node.createwallet(
            wallet_name="expensive-provider", descriptors=True,
            load_on_startup=True)
        client_node.createwallet(
            wallet_name="selection-client", descriptors=True,
            load_on_startup=True)
        client_node.createwallet(
            wallet_name="selection-recipient", descriptors=True,
            load_on_startup=True)

        cheap = cheap_node.get_wallet_rpc("cheap-provider")
        expensive = expensive_node.get_wallet_rpc("expensive-provider")
        client = client_node.get_wallet_rpc("selection-client")
        recipient = client_node.get_wallet_rpc("selection-recipient")
        cheap_cli = cheap_node.cli("-rpcwallet=cheap-provider")
        expensive_cli = expensive_node.cli("-rpcwallet=expensive-provider")

        self.log.info("Fund both providers while leaving the client without DGB")
        self.generatetoaddress(
            cheap_node, 110, cheap.getnewaddress(address_type="bech32m"))
        self.sync_blocks()
        for node in self.nodes:
            node.setmockoracleprice(500_000)

        dgb_funding = cheap.sendtoaddress(
            expensive.getnewaddress(address_type="bech32m"), 3)
        assert_equal(len(dgb_funding), 64)
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()

        minted = cheap.mintdigidollar(5_000, 0)
        assert_equal(minted["dd_minted"], 5_000)
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()

        expensive_funding = cheap.senddigidollar(
            expensive.getdigidollaraddress(), 1_000)
        assert_equal(len(expensive_funding["txid"]), 64)
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()

        client_funding = cheap.senddigidollar(
            client.getdigidollaraddress(), 1_500)
        assert_equal(len(client_funding["txid"]), 64)
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()
        assert_equal(client.getbalance(), 0)
        assert_equal(client.getdigidollarbalance()["total"], 1_500)

        client_safety = client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10_000,
        })
        assert_equal(
            client_safety["maximum_service_fee_per_transaction_cents"], 100)

        self.log.info("Advertise independent 0.50% and 2.00% USER_PAID offers")
        cheap_identity = self.configure_provider(
            cheap, cheap_cli, "Cheap Independent Provider", 50)
        expensive_identity = self.configure_provider(
            expensive, expensive_cli, "Expensive Independent Provider", 200)
        self.sync_mempools()
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()

        cheap_start = cheap.startpaymaster()
        expensive_start = expensive.startpaymaster()
        assert_equal(cheap_start["running"], True)
        assert_equal(cheap_start["ready"], True)
        assert_equal(cheap_start["endpoint"], f"127.0.0.1:{p2p_port(0)}")
        assert_equal(expensive_start["running"], True)
        assert_equal(expensive_start["ready"], True)
        assert_equal(
            expensive_start["endpoint"], f"127.0.0.1:{p2p_port(1)}")

        provider_ids = {
            cheap_identity["provider_id"],
            expensive_identity["provider_id"],
        }
        self.wait_until(lambda: {
            entry["provider_id"] for entry in client_node.listpaymasters()
        }.issuperset(provider_ids))

        self.log.info("Client displays both offers sorted by exact total cost")
        offers = [
            offer for offer in client.getpaymasteroffers(1_000)
            if offer["provider_id"] in provider_ids and
            offer["funding_model"] == "user_paid"
        ]
        assert_equal(len(offers), 2)
        assert_equal(
            [offer["provider_id"] for offer in offers],
            [cheap_identity["provider_id"], expensive_identity["provider_id"]])
        assert_equal(
            [offer["display_name"] for offer in offers],
            ["Cheap Independent Provider", "Expensive Independent Provider"])
        assert_equal(
            [offer["endpoint"] for offer in offers],
            [f"127.0.0.1:{p2p_port(0)}", f"127.0.0.1:{p2p_port(1)}"])
        assert_equal(
            [offer["fee_rate_bps"] for offer in offers], [50, 200])
        assert_equal(
            [offer["payment_cents"] for offer in offers], [1_000, 1_000])
        assert_equal(
            [offer["service_fee_cents"] for offer in offers], [5, 20])
        assert_equal(
            [offer["user_total_cents"] for offer in offers], [1_005, 1_020])

        self.log.info("Replace one offer monotonically and restore its price")
        initial_cheap_sequence = next(
            entry["sequence"] for entry in client_node.listpaymasters()
            if entry["provider_id"] == cheap_identity["provider_id"])
        cheap.stoppaymaster()
        repriced_policy = cheap_cli.setpaymasterpolicy(
            self.provider_policy(300))
        assert_equal(repriced_policy["fee_rate_bps"], 300)
        repriced_start = cheap.startpaymaster()
        assert repriced_start["announcement_sequence"] > initial_cheap_sequence

        def repriced_offers_ready():
            candidates = [
                offer for offer in client.getpaymasteroffers(1_000)
                if offer["provider_id"] in provider_ids and
                offer["funding_model"] == "user_paid"
            ]
            return (
                len(candidates) == 2 and
                [offer["provider_id"] for offer in candidates] == [
                    expensive_identity["provider_id"],
                    cheap_identity["provider_id"],
                ] and
                [offer["fee_rate_bps"] for offer in candidates] == [200, 300]
            )

        self.wait_until(repriced_offers_ready)
        cheap.stoppaymaster()
        restored_policy = cheap_cli.setpaymasterpolicy(
            self.provider_policy(50))
        assert_equal(restored_policy["fee_rate_bps"], 50)
        restored_start = cheap.startpaymaster()
        assert (restored_start["announcement_sequence"] >
                repriced_start["announcement_sequence"])

        def restored_offers_ready():
            candidates = [
                offer for offer in client.getpaymasteroffers(1_000)
                if offer["provider_id"] in provider_ids and
                offer["funding_model"] == "user_paid"
            ]
            return (
                len(candidates) == 2 and
                [offer["provider_id"] for offer in candidates] == [
                    cheap_identity["provider_id"],
                    expensive_identity["provider_id"],
                ] and
                [offer["fee_rate_bps"] for offer in candidates] == [50, 200]
            )

        self.wait_until(restored_offers_ready)

        assert_equal(self.reserved_operational_entries(cheap), [])
        assert_equal(self.reserved_operational_entries(expensive), [])

        expensive_pool_before = expensive.getpaymasterpoolinfo()["pool"]
        expensive_safety_before = (
            expensive.getpaymastersafetystatus()["user_paid"])
        expensive_finance_before = expensive.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10,
        })

        self.log.info("Automatically select only the cheapest provider")
        request_id = "550e8400-e29b-41d4-a716-446655442001"
        options = {
            "fee_mode": "paymaster",
            "request_id": request_id,
            "maximum_paymaster_fee_cents": 20,
            "privacy": "standard",
            "selection": "lowest_total_cost",
            "maximum_provider_attempts": 2,
        }
        recipient_address = recipient.getdigidollaraddress()

        def send():
            result = client.senddigidollar(
                recipient_address, 1_000, "", 0, None, "cents", options)
            assert_equal(result["provider_id"], cheap_identity["provider_id"])
            return result

        initial = send()
        assert_equal(initial["request_id"], request_id)
        assert_equal(initial["status"], "pending")

        capacity_seen = False
        provider_quote = {}

        def process_quote():
            nonlocal capacity_seen, provider_quote
            send()
            not_selected = expensive.processpaymasterrequests()
            assert_equal(not_selected["processed"], False)
            assert_equal(not_selected["queued"], False)
            response = cheap.processpaymasterrequests()
            if not response["processed"]:
                return False
            assert_equal(response["queued"], True)
            assert_equal(
                response["provider_id"], cheap_identity["provider_id"])
            if response["message_type"] == "capacity":
                capacity_seen = True
                return False
            assert_equal(response["message_type"], "quote")
            assert_equal(response["request_id"], request_id)
            provider_quote = response
            return True

        self.wait_until(process_quote)
        assert_equal(capacity_seen, True)
        assert_equal(provider_quote["provider_id"], cheap_identity["provider_id"])
        cheap_reserved = self.reserved_operational_entries(cheap)
        assert_equal(len(cheap_reserved), 2)
        assert_equal(
            {entry["asset"] for entry in cheap_reserved},
            {"dgb", "dd_carrier"})
        assert_equal(self.reserved_operational_entries(expensive), [])

        self.log.info("Authorize the exact cheap offer and complete its transfer")
        authorization = {}

        def authorization_ready():
            nonlocal authorization
            authorization = send()
            if authorization.get("authorization_required", False):
                return True
            replay = cheap.processpaymasterrequests()
            if replay["processed"]:
                assert_equal(replay["queued"], True)
                assert_equal(
                    replay["provider_id"], cheap_identity["provider_id"])
            return False

        self.wait_until(authorization_ready)
        assert_equal(authorization["authorization_accepted"], False)
        assert_equal(authorization["funding_model"], "user_paid")
        assert_equal(authorization["payment_cents"], 1_000)
        assert_equal(authorization["service_fee_cents"], 5)
        assert_equal(authorization["user_total_cents"], 1_005)
        options["authorization_commitment"] = authorization[
            "authorization_commitment"]

        authorized = send()
        assert_equal(authorized["authorization_accepted"], True)
        assert_equal(authorized["session_state"], "PENDING_PROVIDER")
        if "psbt" not in authorized:
            authorized = send()
        assert "psbt" in authorized

        expensive_submit = expensive.processpaymastersubmits()
        assert_equal(expensive_submit["processed"], False)
        assert_equal(expensive_submit["queued"], False)

        provider_commit = {}

        def submit_ready():
            nonlocal provider_commit
            provider_commit = cheap.processpaymastersubmits()
            if provider_commit["processed"]:
                return True
            send()
            return False

        self.wait_until(submit_ready)
        assert_equal(provider_commit["queued"], True)
        assert_equal(provider_commit["request_id"], request_id)
        assert_equal(
            provider_commit["provider_id"], cheap_identity["provider_id"])
        assert_equal(provider_commit["result_status"], "broadcast_attempted")
        txid = provider_commit["txid"]
        assert_equal(len(txid), 64)
        assert txid in cheap_node.getrawmempool()

        client_result = {}

        def result_ready():
            nonlocal client_result
            client_result = client.processpaymasterresult(request_id)
            return client_result["processed"]

        self.wait_until(result_ready)
        assert_equal(client_result["txid"], txid)
        assert_equal(
            client_result["provider_id"], cheap_identity["provider_id"])
        assert_equal(client_result["payment_cents"], 1_000)
        assert_equal(client_result["service_fee_cents"], 5)
        assert_equal(client_result["user_total_cents"], 1_005)
        assert_equal(client_result["result_status"], "broadcast_attempted")

        self.wait_until(lambda: all(
            txid in node.getrawmempool() for node in self.nodes))
        self.generatetoaddress(cheap_node, 1, cheap.getnewaddress())
        self.sync_blocks()
        self.wait_until(
            lambda: recipient.getdigidollarbalance()["total"] == 1_000)
        assert_equal(client.getdigidollarbalance()["total"], 495)
        assert_equal(client.getbalance(), 0)
        client_safety_after = client.getpaymasterclientsafetystatus()
        assert_equal(client_safety_after["active_reservations"], 0)
        assert_equal(client_safety_after["reserved_service_fee_cents"], 0)
        assert_equal(
            client_safety_after["spent_service_fee_last_day_cents"], 5)

        self.log.info("Only the selected provider records cost and income")
        self.wait_until(lambda: cheap.getpaymasterfinancestatus({
            "period": "all",
        })["successful_transfers"] == 1)
        cheap_finance = cheap.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10,
        })
        expensive_finance = expensive.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10,
        })
        assert_equal(cheap_finance["successful_transfers"], 1)
        assert_equal(cheap_finance["user_paid_transfers"], 1)
        assert_equal(cheap_finance["service_fee_income_cents"], 5)
        cheap_transfer_events = [
            event for event in cheap_finance["events"]
            if event["kind"] == "transfer"
        ]
        assert_equal(len(cheap_transfer_events), 1)
        assert_equal(cheap_transfer_events[0]["state"], "confirmed")
        assert_equal(cheap_transfer_events[0]["funding_model"], "user_paid")
        assert_equal(cheap_transfer_events[0]["dd_income_cents"], 5)
        assert_equal(cheap_transfer_events[0]["transaction_id"], txid)
        for field in (
                "successful_transfers", "user_paid_transfers",
                "service_fee_income_cents"):
            assert_equal(
                expensive_finance[field], expensive_finance_before[field])
        assert_equal(
            [event["event_id"] for event in expensive_finance["events"]],
            [event["event_id"]
             for event in expensive_finance_before["events"]])

        late_request = expensive.processpaymasterrequests()
        assert_equal(late_request["processed"], False)
        assert_equal(late_request["queued"], False)
        late_submit = expensive.processpaymastersubmits()
        assert_equal(late_submit["processed"], False)
        assert_equal(late_submit["queued"], False)

        assert_equal(
            expensive.getpaymasterpoolinfo()["pool"], expensive_pool_before)
        expensive_safety_after = (
            expensive.getpaymastersafetystatus()["user_paid"])
        assert_equal(
            expensive_safety_after["reserved_network_fee_satoshis"],
            expensive_safety_before["reserved_network_fee_satoshis"])
        assert_equal(
            expensive_safety_after["spent_network_fee_last_day_satoshis"],
            expensive_safety_before["spent_network_fee_last_day_satoshis"])
        assert_equal(
            expensive_safety_after["completed_last_day"],
            expensive_safety_before["completed_last_day"])
        assert_equal(self.reserved_operational_entries(expensive), [])

        self.log.info("Expire stale announcements and accept a fresh replacement")
        active_announcements = [
            entry for entry in client_node.listpaymasters()
            if entry["provider_id"] in provider_ids
        ]
        assert_equal(len(active_announcements), 2)
        expires_after = max(
            entry["expires_at"] for entry in active_announcements) + 1
        cheap.stoppaymaster()
        expensive.stoppaymaster()
        for node in self.nodes:
            node.setmocktime(expires_after)
        self.wait_until(lambda: not (
            {entry["provider_id"] for entry in client_node.listpaymasters()} &
            provider_ids))

        refreshed_expensive = expensive.startpaymaster()
        assert_equal(refreshed_expensive["running"], True)
        self.wait_until(lambda: {
            entry["provider_id"] for entry in client_node.listpaymasters()
        } & provider_ids == {expensive_identity["provider_id"]})


if __name__ == "__main__":
    PaymasterOfferSelectionTest().main()
