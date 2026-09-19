#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the persistent Paymaster provider RPC lifecycle without Qt.

The scenarios deliberately cross RPC, P2P, wallet persistence, automatic
provider processing, liquidity recycling, safety accounting, and recovery.
Balance assertions are end-to-end security oracles: successful retries must not
duplicate a transfer or fee, and failed paths must not silently release value
that may already have an authorization artifact.
"""

from io import BytesIO
import os
from pathlib import Path
import struct
import time

from test_framework.address import base58_to_byte
from test_framework.key import TaggedHash, compute_xonly_pubkey, sign_schnorr
from test_framework.messages import ser_string, ser_uint256
from test_framework.paymaster import paymaster_node_args, provider_safety_policy
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    p2p_port,
)


def digidollar_scriptpubkey(address):
    """Decode a regtest two-byte-version DD address into its P2TR script."""
    payload, first_version = base58_to_byte(address)
    assert first_version == 0xa3
    assert len(payload) == 33 and payload[0] == 0xa4
    return b"\x51\x20" + payload[1:]


def sponsorship_capability(secret, descriptor, recipient_script, amount_cents,
                           payment_request_nonce, expires_at, genesis_hash):
    """Create the canonical V1 capability consumed by restricted sponsorship."""
    unsigned = (
        struct.pack("<H", 1) +
        ser_uint256(int(genesis_hash, 16)) +
        ser_uint256(int(descriptor["provider_id"], 16)) +
        ser_uint256(int(descriptor["offer_id"], 16)) +
        ser_uint256(int(descriptor["policy_hash"], 16)) +
        ser_string(recipient_script) +
        struct.pack("<q", amount_cents) +
        ser_uint256(int(payment_request_nonce, 16)) +
        struct.pack("<q", expires_at)
    )
    signature_hash = TaggedHash(
        "DigiByte Paymaster Sponsorship Capability v1", unsigned)
    signature = sign_schnorr(secret, signature_hash)
    assert signature is not None and len(signature) == 64
    return (unsigned + ser_string(signature)).hex()


def captured_message_types(chain_path, direction):
    """Return message command names captured across every peer connection."""
    commands = []
    for capture_file in Path(chain_path, "message_capture").glob(
            f"*/msgs_{direction}.dat"):
        with capture_file.open("rb") as stream:
            while header := stream.read(24):
                assert len(header) == 24
                encoded = BytesIO(header)
                encoded.read(8)  # timestamp
                commands.append(encoded.read(12).rstrip(b"\x00").decode("ascii"))
                payload_size = int.from_bytes(encoded.read(4), "little")
                assert len(stream.read(payload_size)) == payload_size
    return commands


class PaymasterProviderRPCTest(DigiByteTestFramework):
    def set_test_params(self):
        self.pre_paymaster_digibyted = os.getenv("PRE_PAYMASTER_DIGIBYTED")
        self.num_nodes = 3 if self.pre_paymaster_digibyted else 2
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(provider_node=0),
            paymaster_node_args(),
        ]
        if self.pre_paymaster_digibyted:
            # A pre-Paymaster daemon must remain an ordinary DD validator. It
            # deliberately receives no Paymaster option, while message capture
            # proves that it never participates in announcement gossip.
            self.extra_args.append([
                "-capturemessages=1",
                "-dandelion=0",
                "-debug=net",
                "-digidollar=1",
                "-prune=0",
                "-txindex=1",
                "-v2transport=1",
            ])

    def setup_nodes(self):
        if not self.pre_paymaster_digibyted:
            return super().setup_nodes()

        if not Path(self.pre_paymaster_digibyted).is_file():
            raise AssertionError(
                f"PRE_PAYMASTER_DIGIBYTED does not exist: {self.pre_paymaster_digibyted}")
        self.add_nodes(
            self.num_nodes,
            self.extra_args,
            binary=[
                self.options.digibyted,
                self.options.digibyted,
                self.pre_paymaster_digibyted,
            ],
            # The current CLI speaks the unchanged JSON-RPC transport needed
            # for validation observations on the frozen daemon.
            binary_cli=[self.options.digibytecli] * self.num_nodes,
        )
        self.start_nodes()
        if self._requires_wallet:
            self.import_deterministic_coinbase_privkeys()

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(
            wallet_name="paymaster",
            descriptors=True,
            load_on_startup=True,
        )
        wallet = node.get_wallet_rpc("paymaster")
        cli = node.cli("-rpcwallet=paymaster")

        self.log.info("Check the initial provider readiness boundary")
        initial = wallet.getpaymasterinfo()
        assert_equal(initial["settings_present"], False)
        assert_equal(initial["enabled"], False)
        assert_equal(initial["running"], False)
        assert_equal(initial["ready"], False)
        assert_equal(initial["wallet_eligible"], True)
        assert "PAYMASTER_PROVIDER_NOT_ENABLED" in initial["readiness_errors"]
        assert "PAYMASTER_IDENTITY_NOT_FOUND" in initial["readiness_errors"]
        assert "PAYMASTER_POLICY_NOT_FOUND" in initial["readiness_errors"]
        assert "PAYMASTER_POOLS_NOT_PREPARED" in initial["readiness_errors"]

        self.log.info("Create the persistent BIP86 identity and provider policy")
        identity = wallet.createpaymasteridentity("Functional Test Provider")
        assert_equal(len(identity["provider_id"]), 64)
        assert_equal(len(identity["identity_key"]), 64)
        assert_equal(identity["display_name"], "Functional Test Provider")

        self.log.info("Fund the dedicated provider wallet")
        self.generatetoaddress(node, 110, wallet.getnewaddress(address_type="bech32m"))

        policy = {
            "funding_models": ["sponsored"],
            "sponsorship_scope": "public",
            "fee_rate_bps": 0,
            "min_amount_cents": 100,
            "max_amount_cents": 100000,
            "quote_ttl": 60,
            "maximum_network_fee_dgb_satoshis": 20000000,
        }
        persisted_policy = cli.setpaymasterpolicy(policy)
        for key, value in policy.items():
            assert_equal(persisted_policy[key], value)
        assert_equal(len(persisted_policy["policy_hash"]), 64)

        assert_raises_rpc_error(
            -4,
            "PAYMASTER_SAFETY_POLICY_NOT_FOUND",
            cli.setpaymasterenabled,
            True,
        )

        staged = wallet.getpaymasterinfo()
        assert_equal(staged["settings_present"], True)
        assert_equal(staged["enabled"], False)
        assert_equal(staged["provider_id"], identity["provider_id"])
        assert_equal(staged["policy"], persisted_policy)
        assert_equal(staged["ready"], False)
        assert "PAYMASTER_POOLS_NOT_PREPARED" in staged["readiness_errors"]
        assert "PAYMASTER_SAFETY_POLICY_NOT_FOUND" in staged["readiness_errors"]

        start = wallet.startpaymaster()
        assert_equal(start["running"], False)
        assert_equal(start["ready"], False)
        assert_equal(start["recovery_candidates"], 0)
        assert_equal(start["recovered_broadcasts"], 0)
        assert "PAYMASTER_POOLS_NOT_PREPARED" in start["readiness_errors"]
        assert "PAYMASTER_SAFETY_POLICY_NOT_FOUND" in start["readiness_errors"]

        persisted_safety = cli.setpaymastersafetypolicy(
            provider_safety_policy(policy["funding_models"],
                                   policy["sponsorship_scope"]))
        assert_equal(persisted_safety["public_sponsored"]
                     ["maximum_network_fee_per_day_satoshis"], 4000000000)

        enabled = cli.setpaymasterenabled(True)
        assert_equal(enabled["enabled"], True)
        assert_equal(enabled["running"], False)
        assert_equal(len(enabled["policy_hash"]), 64)

        # Automatic queue service is the safe default, but provider runtime
        # autostart remains an explicit operator opt-in.
        runtime = wallet.getpaymasterinfo()
        assert_equal(runtime["operation_mode"], "automatic")
        assert_equal(runtime["autostart"], False)
        assert_equal(runtime["service_state"], "stopped")

        self.log.info("Preview and explicitly execute sponsored DGB pool preparation")
        pool_target = {
            "admission_dgb_slots": 4,
            "operational_dgb_slots": 2,
        }
        preview = cli.preparepaymasterpool(pool_target)
        assert_equal(preview["executed"], False)
        assert_equal(preview["admission_dgb_slots"], 4)
        assert_equal(preview["operational_dgb_slots"], 2)
        assert_equal(preview["total_output_satoshis"], 80000000)
        assert_equal(len(preview["plan_id"]), 64)

        pool_target["execute"] = True
        pool_target["plan_id"] = "00" * 32
        assert_raises_rpc_error(
            -8,
            "PAYMASTER_POOL_PLAN_CHANGED",
            cli.preparepaymasterpool,
            pool_target,
        )
        pool_target["plan_id"] = preview["plan_id"]
        prepared = cli.preparepaymasterpool(pool_target)
        assert_equal(prepared["executed"], True)
        assert_equal(len(prepared["txid"]), 64)
        assert_equal(len(prepared["pool"]), 6)
        assert_equal(prepared["network_fee_satoshis"] > 0, True)

        unconfirmed_pool = wallet.getpaymasterpoolinfo()
        assert_equal(unconfirmed_pool["prepared"], True)
        assert_equal(unconfirmed_pool["ready"], False)
        assert_equal(unconfirmed_pool["entries"], 6)

        self.generatetoaddress(node, 1, wallet.getnewaddress())
        ready = wallet.getpaymasterinfo()
        assert_equal(ready["ready"], True)
        assert_equal(ready["pool_ready"], True)
        assert_equal(ready["pool"]["admission_dgb"], 4)
        assert_equal(ready["pool"]["complete_operational_slots"], 2)

        started = wallet.startpaymaster()
        assert_equal(started["ready"], True)
        assert_equal(started["running"], True)
        assert_equal(started["endpoint"], f"127.0.0.1:{p2p_port(0)}")
        assert_equal(started["announcement_sequence"], 1)
        assert_equal(wallet.getpaymasterinfo()["announcement_sequence"], 1)
        changed_while_running = policy.copy()
        changed_while_running["max_amount_cents"] += 1
        assert_raises_rpc_error(
            -4,
            "PAYMASTER_PROVIDER_RUNNING",
            cli.setpaymasterpolicy,
            changed_while_running,
        )
        disabled = cli.setpaymasterenabled(False)
        assert_equal(disabled["enabled"], False)
        assert_equal(disabled["running"], False)
        disabled_status = wallet.getpaymasterinfo()
        assert_equal(disabled_status["enabled"], False)
        assert_equal(disabled_status["running"], False)
        reenabled = cli.setpaymasterenabled(True)
        assert_equal(reenabled["enabled"], True)
        assert_equal(reenabled["running"], False)

        self.log.info("Do not count operational DGB slots below a changed fee ceiling")
        raised_fee_policy = policy.copy()
        raised_fee_policy["maximum_network_fee_dgb_satoshis"] = 30000000
        cli.setpaymasterpolicy(raised_fee_policy)
        raised_preview = cli.preparepaymasterpool({
            "admission_dgb_slots": 4,
            "operational_dgb_slots": 2,
        })
        assert_equal(raised_preview["missing_admission_dgb_slots"], 0)
        assert_equal(raised_preview["missing_operational_dgb_slots"], 2)
        assert_equal(raised_preview["operational_dgb_satoshis_each"], 30000000)
        assert_equal(raised_preview["total_output_satoshis"], 60000000)
        raised_execute = {
            "admission_dgb_slots": 4,
            "operational_dgb_slots": 2,
            "execute": True,
            "plan_id": raised_preview["plan_id"],
        }
        raised_pool = cli.preparepaymasterpool(raised_execute)
        assert_equal(raised_pool["executed"], True)
        self.generatetoaddress(node, 1, wallet.getnewaddress())

        policy_bound_preview = cli.preparepaymasterpool({
            "admission_dgb_slots": 4,
            "operational_dgb_slots": 5,
        })
        # Only the two 0.30 DGB slots meet the raised fee ceiling; the two
        # older 0.20 DGB slots must not count toward the five-slot target.
        assert_equal(policy_bound_preview["missing_operational_dgb_slots"], 3)
        assert_equal(policy_bound_preview["total_output_satoshis"], 90000000)
        cli.setpaymasterpolicy(policy)
        assert_raises_rpc_error(
            -8,
            "PAYMASTER_POOL_PLAN_CHANGED",
            cli.preparepaymasterpool,
            {
                "admission_dgb_slots": 4,
                "operational_dgb_slots": 5,
                "execute": True,
                "plan_id": policy_bound_preview["plan_id"],
            },
        )
        value_rebalance_target = {
            "admission_dgb_slots": 4,
            "operational_dgb_slots": 2,
        }
        value_rebalance_preview = cli.rebalancepaymasterpool(
            value_rebalance_target)
        assert_equal(value_rebalance_preview[
            "retired_operational_dgb_slots"], 2)
        # Keep the two 0.30 DGB outputs and retire the older undersized 0.20
        # DGB outputs, not the other way around.
        assert_equal(value_rebalance_preview["retired_dgb_satoshis"], 40000000)
        value_rebalance_target["execute"] = True
        value_rebalance_target["plan_id"] = value_rebalance_preview["plan_id"]
        value_rebalanced = cli.rebalancepaymasterpool(value_rebalance_target)
        assert_equal(value_rebalanced["executed"], True)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        retained_operational_values = sorted(
            entry["dgb_satoshis"]
            for entry in wallet.getpaymasterpoolinfo()["pool"]
            if entry["purpose"] == "operational" and
            entry["asset"] == "dgb" and entry["state"] == "available"
        )
        assert_equal(retained_operational_values, [30000000, 30000000])

        self.log.info("Add the USER_PAID carrier pool without replacing existing DGB slots")
        node.setmockoracleprice(500000)
        # Keep later recovery/restricted-sponsorship fixtures disjoint from
        # the 600-cent carrier pool and from each other.
        minted = wallet.mintdigidollar(2000, 0)
        assert_equal(minted["dd_minted"], 2000)
        self.generatetoaddress(node, 1, wallet.getnewaddress())

        policy["funding_models"] = ["sponsored", "user_paid"]
        policy["fee_rate_bps"] = 50
        # Widen the local spending firewall before advertising USER_PAID.
        cli.setpaymastersafetypolicy(
            provider_safety_policy(policy["funding_models"],
                                   policy["sponsorship_scope"]))
        persisted_policy = cli.setpaymasterpolicy(policy)
        carrier_target = {
            "admission_dgb_slots": 4,
            "operational_dgb_slots": 2,
            "admission_carrier_slots": 4,
            "operational_carrier_slots": 2,
        }
        carrier_preview = cli.preparepaymasterpool(carrier_target)
        assert_equal(carrier_preview["executed"], False)
        assert_equal(carrier_preview["missing_admission_dgb_slots"], 0)
        assert_equal(carrier_preview["missing_operational_dgb_slots"], 0)
        assert_equal(carrier_preview["missing_admission_carrier_slots"], 4)
        assert_equal(carrier_preview["missing_operational_carrier_slots"], 2)
        assert_equal(carrier_preview["total_carrier_cents"], 600)

        carrier_target["execute"] = True
        carrier_target["plan_id"] = carrier_preview["plan_id"]
        carriers = cli.preparepaymasterpool(carrier_target)
        assert_equal(carriers["executed"], True)
        assert_equal(len(carriers["dd_txid"]), 64)
        assert "dgb_txid" not in carriers
        # Rebalancing retains the two spent undersized DGB entries as history.
        assert_equal(len(carriers["pool"]), 14)
        self.generatetoaddress(node, 1, wallet.getnewaddress())

        user_paid_ready = wallet.getpaymasterpoolinfo()
        assert_equal(user_paid_ready["ready"], True)
        assert_equal(user_paid_ready["entries"], 14)
        assert_equal(sum(entry["state"] == "available"
                         for entry in user_paid_ready["pool"]), 12)
        assert_equal(sum(entry["state"] == "spent"
                         for entry in user_paid_ready["pool"]), 2)
        provider_ready = wallet.getpaymasterinfo()
        assert_equal(provider_ready["pool"]["admission_carriers"], 4)
        assert_equal(provider_ready["pool"]["operational_carriers"], 2)
        assert_equal(provider_ready["pool"]["complete_operational_slots"], 2)

        carrier_target.pop("execute")
        carrier_target.pop("plan_id")
        retry = cli.preparepaymasterpool(carrier_target)
        assert_equal(retry["executed"], False)
        assert_equal(retry["missing_admission_carrier_slots"], 0)
        assert_equal(retry["missing_operational_carrier_slots"], 0)
        assert_equal(wallet.getpaymasterpoolinfo(), user_paid_ready)

        self.log.info("Preview and execute retirement of excess pool liquidity")
        minimum_target = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 1,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 1,
        }
        rebalance_preview = cli.rebalancepaymasterpool(minimum_target)
        assert_equal(rebalance_preview["executed"], False)
        assert_equal(rebalance_preview["retired_admission_dgb_slots"], 1)
        assert_equal(rebalance_preview["retired_operational_dgb_slots"], 1)
        assert_equal(rebalance_preview["retired_admission_carrier_slots"], 1)
        assert_equal(rebalance_preview["retired_operational_carrier_slots"], 1)
        assert_equal(rebalance_preview["retired_dgb_satoshis"], 40000000)
        assert_equal(rebalance_preview["retired_carrier_cents"], 200)

        minimum_target["execute"] = True
        minimum_target["plan_id"] = rebalance_preview["plan_id"]
        rebalanced = cli.rebalancepaymasterpool(minimum_target)
        assert_equal(rebalanced["executed"], True)
        assert_equal(len(rebalanced["dd_txid"]), 64)
        assert_equal(len(rebalanced["dgb_txid"]), 64)
        assert_equal(rebalanced["network_fee_satoshis"] > 0, True)
        self.generatetoaddress(node, 1, wallet.getnewaddress())

        minimum_ready = wallet.getpaymasterinfo()
        assert_equal(minimum_ready["ready"], True)
        assert_equal(minimum_ready["pool"]["admission_dgb"], 3)
        assert_equal(minimum_ready["pool"]["admission_carriers"], 3)
        assert_equal(minimum_ready["pool"]["operational_dgb"], 1)
        assert_equal(minimum_ready["pool"]["operational_carriers"], 1)
        assert_equal(minimum_ready["pool"]["complete_operational_slots"], 1)

        minimum_target.pop("execute")
        minimum_target.pop("plan_id")
        rebalance_retry = cli.rebalancepaymasterpool(minimum_target)
        assert_equal(rebalance_retry["executed"], False)
        assert_equal(rebalance_retry["retired_dgb_satoshis"], 0)
        assert_equal(rebalance_retry["retired_carrier_cents"], 0)

        self.log.info("Complete a discovered USER_PAID transfer from a client without DGB")
        policy["funding_models"] = ["user_paid"]
        # Stop advertising sponsorship before removing its local budget.
        persisted_policy = cli.setpaymasterpolicy(policy)
        client_node = self.nodes[1]
        client_node.createwallet(
            wallet_name="client",
            descriptors=True,
            load_on_startup=True,
        )
        client = client_node.get_wallet_rpc("client")
        client_node.setmockoracleprice(500000)
        client_safety = client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10000,
        })
        assert_equal(client_safety[
            "maximum_service_fee_per_transaction_cents"], 100)
        cli.setpaymastersafetypolicy(
            provider_safety_policy(policy["funding_models"],
                                   policy["sponsorship_scope"]))
        liquidity_policy = cli.setpaymasterliquiditypolicy({
            "automatic_replenishment": True,
            "paid_maintenance_approved": True,
            "target_admission_dgb": 3,
            "target_operational_dgb": 1,
            "target_admission_carriers": 3,
            "target_operational_carriers": 1,
            "maximum_maintenance_fee_per_transaction_satoshis": 20000000,
            "maximum_maintenance_fee_per_hour_satoshis": 100000000,
            "maximum_maintenance_fee_per_day_satoshis": 400000000,
        })
        assert_equal(liquidity_policy["automatic_replenishment"], True)
        assert_equal(liquidity_policy["paid_maintenance_approved"], True)
        assert_equal(liquidity_policy["target_operational_dgb"], 1)
        assert_equal(liquidity_policy["target_operational_carriers"], 1)
        initial_client_safety = client.getpaymasterclientsafetystatus()
        assert_equal(initial_client_safety["configured"], True)
        assert_equal(initial_client_safety["ledger_present"], True)
        assert_equal(initial_client_safety["active_reservations"], 0)
        assert_equal(initial_client_safety["reserved_service_fee_cents"], 0)
        assert_equal(initial_client_safety["spent_service_fee_last_day_cents"], 0)
        assert_equal(initial_client_safety[
            "available_service_fee_today_cents"], 10000)
        initial_provider_safety = cli.getpaymastersafetystatus()
        assert_equal(initial_provider_safety["configured"], True)
        assert_equal(initial_provider_safety["ledger_present"], True)
        assert_equal(initial_provider_safety["user_paid"]
                     ["reserved_network_fee_satoshis"], 0)
        assert_equal(initial_provider_safety["user_paid"]
                     ["spent_network_fee_last_day_satoshis"], 0)
        assert_equal(initial_provider_safety["user_paid"]
                     ["completed_last_day"], 0)

        # Give the client confirmed DD through the ordinary backward-compatible
        # path. Node 1 has never mined or received DGB.
        client_address = client.getdigidollaraddress()
        funded = wallet.senddigidollar(client_address, 1106)
        assert_equal(len(funded["txid"]), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        assert_equal(client.getbalance(), 0)
        assert_equal(client.getdigidollarbalance()["total"], 1106)

        network_started = wallet.startpaymaster()
        assert_equal(network_started["running"], True)
        assert_equal(network_started["operation_mode"], "automatic")
        assert_equal(network_started["autostart"], False)
        assert_equal(network_started["service_state"], "active")
        assert_equal(network_started["announcement_sequence"], 2)
        assert_raises_rpc_error(
            -4,
            "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE",
            wallet.processpaymasterrequests,
        )
        self.wait_until(
            lambda: any(entry["provider_id"] == identity["provider_id"] and
                        entry["sequence"] == 2
                        for entry in client_node.listpaymasters())
        )
        if self.pre_paymaster_digibyted:
            old_node = self.nodes[2]
            self.wait_until(
                lambda: "sendpmasters" in captured_message_types(
                    old_node.chain_path, "recv"))
            assert "pmannounce" not in captured_message_types(
                old_node.chain_path, "recv")
            assert "sendpmasters" not in captured_message_types(
                old_node.chain_path, "sent")
            assert "pmannounce" not in captured_message_types(
                old_node.chain_path, "sent")
        offers = client.getpaymasteroffers(100)
        assert_equal(len(offers) > 0, True)
        assert_equal(offers[0]["provider_id"], identity["provider_id"])
        assert_equal(offers[0]["service_fee_cents"], 1)

        recipient = wallet.getdigidollaraddress()
        self.log.info("Reject unsafe high-privacy selection before session creation")
        high_privacy_request_id = "550e8400-e29b-41d4-a716-446655440106"
        high_privacy_options = {
            "fee_mode": "paymaster",
            "request_id": high_privacy_request_id,
            "maximum_paymaster_fee_cents": 1,
            "privacy": "high",
            "selection": "lowest_total_cost",
            "maximum_provider_attempts": 1,
        }
        assert_raises_rpc_error(
            -4,
            "PAYMASTER_NO_ELIGIBLE_OFFER",
            client.senddigidollar,
            recipient, 100, "", 0, None, "cents", high_privacy_options,
        )
        assert_raises_rpc_error(
            -4,
            "Paymaster session not found",
            client.getdigidollarsendsession,
            {"request_id": high_privacy_request_id},
        )

        multiple_attempt_request_id = "550e8400-e29b-41d4-a716-446655440107"
        high_privacy_options["request_id"] = multiple_attempt_request_id
        high_privacy_options["maximum_provider_attempts"] = 2
        assert_raises_rpc_error(
            -8,
            "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy",
            client.senddigidollar,
            recipient, 100, "", 0, None, "cents", high_privacy_options,
        )
        assert_raises_rpc_error(
            -4,
            "Paymaster session not found",
            client.getdigidollarsendsession,
            {"request_id": multiple_attempt_request_id},
        )

        observed_paymaster_psbts = []
        provider_wallets = {identity["provider_id"]: wallet}

        def complete_paymaster_transfer(request_id, amount_cents, fee_cap_cents,
                                        extra_options=None,
                                        stop_after_authorization=False,
                                        provider_wallet=None,
                                        unselected_provider_wallet=None,
                                        after_authorization=None,
                                        automatic_provider=False,
                                        client_wallet=None,
                                        payment_recipient=None):
            transfer_client = client if client_wallet is None else client_wallet
            transfer_recipient = (recipient if payment_recipient is None else
                                  payment_recipient)
            options = {
                "fee_mode": "paymaster",
                "request_id": request_id,
                "maximum_paymaster_fee_cents": fee_cap_cents,
                "privacy": "standard",
                "selection": "lowest_total_cost",
            }
            if extra_options is not None:
                options.update(extra_options)

            def resume_client_send():
                return transfer_client.senddigidollar(
                    transfer_recipient, amount_cents, "", 0, None, "cents", options)

            initial_request = resume_client_send()
            assert_equal(initial_request["request_id"], request_id)
            assert_equal(initial_request["status"], "pending")
            if provider_wallet is None:
                provider_wallet = provider_wallets[initial_request["provider_id"]]

            provider_quote = {}
            capacity_proof_processed = False
            automatic_poll_at = time.monotonic()
            automatic_poll_result = initial_request

            def poll_automatic_client():
                """Retry protocol progress without tripping transport flood limits."""
                nonlocal automatic_poll_at, automatic_poll_result
                now = time.monotonic()
                if now - automatic_poll_at < 0.5:
                    return automatic_poll_result
                automatic_poll_at = now
                automatic_poll_result = resume_client_send()
                return automatic_poll_result

            def process_quote():
                nonlocal provider_quote, capacity_proof_processed
                # A mandatory Capacity-v5 exchange precedes every quote. Once
                # the provider has produced that proof, exact client retries
                # consume it and enqueue the still-undisclosed payment intent.
                # Keeping this retry in the polling loop also covers the short
                # interval while the proof is in flight on the v2 connection.
                if capacity_proof_processed:
                    resume_client_send()
                if unselected_provider_wallet is not None:
                    # Poll the other simultaneously running provider first.
                    # Once the addressed message is present, its provider-ID
                    # filter must leave it untouched for the selected wallet.
                    not_selected = unselected_provider_wallet.processpaymasterrequests()
                    assert_equal(not_selected["processed"], False)
                    assert_equal(not_selected["queued"], False)
                response = provider_wallet.processpaymasterrequests()
                if not response["processed"]:
                    return False
                assert_equal(response["queued"], True)
                if response["message_type"] == "capacity":
                    assert_equal(response["provider_id"],
                                 provider_wallet.getpaymasterinfo()["provider_id"])
                    capacity_proof_processed = True
                    return False
                assert_equal(response["message_type"], "quote")
                provider_quote = response
                return True

            # The first retry queues the request after the dedicated v2
            # connection has completed its handshake. In automatic mode Core
            # performs the bounded provider work; client retries only collect
            # each response and advance the privacy-preserving handshake.
            def request_queued_or_ready():
                state = (poll_automatic_client() if automatic_provider else
                         resume_client_send())
                return (state.get("queued", False) or
                        state.get("authorization_required", False))

            self.wait_until(request_queued_or_ready)
            if automatic_provider:
                self.wait_until(
                    lambda: poll_automatic_client().get(
                        "authorization_required", False))
            else:
                self.wait_until(process_quote)
                assert_equal(provider_quote["queued"], True)
                assert_equal(provider_quote["request_id"], request_id)
                assert_equal(provider_quote["provider_id"],
                             provider_wallet.getpaymasterinfo()["provider_id"])
            if unselected_provider_wallet is not None:
                selected_reserved = [
                    entry for entry in provider_wallet.getpaymasterpoolinfo()["pool"]
                    if entry["purpose"] == "operational" and
                    entry["state"] == "reserved"
                ]
                unselected_reserved = [
                    entry for entry in unselected_provider_wallet.getpaymasterpoolinfo()["pool"]
                    if entry["purpose"] == "operational" and
                    entry["state"] == "reserved"
                ]
                assert_equal(len(selected_reserved), 1)
                assert_equal(len(unselected_reserved), 0)

            authorization = {}
            approved_exact_authorization = {}
            approval_bypass_checked = False

            def authorize_and_submit():
                nonlocal authorization, approved_exact_authorization
                nonlocal approval_bypass_checked
                authorization = resume_client_send()
                if authorization.get("authorization_required", False):
                    commitment = authorization["authorization_commitment"]
                    approved_exact_authorization = {
                        field: authorization[field]
                        for field in (
                            "provider_id", "offer_id", "policy_hash",
                            "funding_model", "payment_cents",
                            "service_fee_cents", "user_total_cents",
                            "authorization_commitment")
                    }
                    assert_equal(authorization["authorization_accepted"], False)
                    if not approval_bypass_checked:
                        # The low-level signer is the final boundary too: a
                        # caller cannot bypass the two-stage high-level RPC by
                        # submitting the prepared PSBT directly.
                        assert_raises_rpc_error(
                            -4, "PAYMASTER_AUTHORIZATION_COMMITMENT_REQUIRED",
                            transfer_client.walletprocesspaymasterpsbt,
                            authorization["psbt"])
                        assert_raises_rpc_error(
                            -4, "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH",
                            transfer_client.walletprocesspaymasterpsbt,
                            authorization["psbt"], "00" * 32)
                        approval_bypass_checked = True
                    options["authorization_commitment"] = commitment
                    return False
                if (authorization.get("queued", False) and
                        authorization.get("session_state") == "PENDING_PROVIDER"):
                    assert_equal(authorization["authorization_accepted"], True)
                    return True
                # A direct Paymaster connection is deliberately short-lived.
                # Keep the manual provider service moving so an exact retry can
                # receive the already durable signed quote on its new peer.
                if not automatic_provider:
                    replay = provider_wallet.processpaymasterrequests()
                    if replay["processed"]:
                        assert_equal(replay["queued"], True)
                        assert_equal(replay["request_id"], request_id)
                return False

            self.wait_until(authorize_and_submit)
            assert_equal(approval_bypass_checked, True)
            assert_equal(authorization["session_state"], "PENDING_PROVIDER")
            if "psbt" in authorization:
                observed_paymaster_psbts.append(authorization["psbt"])
            if stop_after_authorization:
                return authorization
            if after_authorization is not None:
                after_authorization()
                exact_resume = resume_client_send()
                assert_equal(exact_resume["provider_id"],
                             authorization["provider_id"])
                assert_equal(exact_resume["authorization_accepted"], True)
                assert_equal(exact_resume.get("authorization_required", False),
                             False)

            provider_commit = {}

            def process_submit():
                nonlocal provider_commit
                provider_commit = provider_wallet.processpaymastersubmits()
                if not provider_commit["processed"]:
                    return False
                if provider_commit["request_id"] != request_id:
                    # A client can recover after retrying its exact signed
                    # artifact while the corresponding submit is still in
                    # flight. The provider must reject that stale template,
                    # release only its unused pool slot, and continue serving
                    # the next independently addressed submit.
                    assert_equal(provider_commit["result_status"], "rejected")
                    assert_equal(provider_commit["attempt_state"], "REJECTED")
                    assert_equal(provider_commit["rejection_code"],
                                 "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE")
                    return False
                return True

            final_txid = None
            if not automatic_provider:
                self.wait_until(process_submit)
                assert_equal(provider_commit["queued"], True)
                commit = provider_commit["commit"]
                # Broadcast can synchronously invoke the wallet callback. The
                # provider RPC must treat that callback's exact MEMPOOL update
                # as an idempotent advancement, not fail a stale
                # FINAL_COMMITTED -> BROADCAST write.
                assert_equal(commit["broadcast"], True)
                assert "broadcast_error" not in commit
                assert_equal(commit["session_state"], "MEMPOOL")
                assert_equal(provider_commit["attempt_state"], "MEMPOOL")
                assert_equal(commit["attempt_state"], "MEMPOOL")
                assert_equal(commit["result_status"], "broadcast_attempted")
                assert_equal(provider_commit["result_status"],
                             commit["result_status"])
                final_txid = commit["txid"]
                assert_equal(provider_commit["txid"], final_txid)
                assert_equal(len(final_txid), 64)

            client_result = {}

            def process_result():
                nonlocal client_result
                # Automatic providers are consumed through the same durable
                # high-level retry used by Qt.  This specifically protects the
                # race where the provider has already spent its advertised
                # Capacity outpoint with the exact final transaction before
                # the client collects the signed result.  Revalidating the
                # already-authorized submit first would falsely report
                # PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE.
                client_result = (resume_client_send() if automatic_provider else
                                 transfer_client.processpaymasterresult(request_id))
                return client_result.get("processed", False)

            self.wait_until(process_result)
            if automatic_provider:
                final_txid = client_result["txid"]
                assert_equal(len(final_txid), 64)
            assert_equal(client_result["txid"], final_txid)
            assert_equal(client_result["result_status"], "broadcast_attempted")
            assert_equal(client_result["session_state"], "MEMPOOL")
            assert_equal(client_result["attempt_state"], "MEMPOOL")
            # The terminal response must echo the exact manifest approved by
            # the client. Qt must never have to infer a fee or funding model
            # from a stale offer preview after the transaction has succeeded.
            for field, expected in approved_exact_authorization.items():
                assert_equal(client_result[field], expected)
            assert final_txid in node.getrawmempool()
            return final_txid

        def assert_automatic_liquidity_recovery(
                final_txid, expected_carrier_cents, mempool_before):
            """Assert free carrier recycling and one bounded DGB replacement."""
            successor_entries = []

            def successor_registered():
                nonlocal successor_entries
                successor_entries = [
                    entry for entry in wallet.getpaymasterpoolinfo()["pool"]
                    if entry["txid"] == final_txid and
                    entry["purpose"] == "operational"
                ]
                carriers = [
                    entry for entry in successor_entries
                    if entry["asset"] == "dd_carrier"
                ]
                return (len(carriers) == 1 and
                        carriers[0]["dd_cents"] == expected_carrier_cents)

            self.wait_until(successor_registered)
            carrier_successors = [
                entry for entry in successor_entries
                if entry["asset"] == "dd_carrier"
            ]
            assert_equal(len(carrier_successors), 1)
            assert_equal(carrier_successors[0]["state"],
                         "pending_successor")
            assert_equal(carrier_successors[0]["dd_cents"],
                         expected_carrier_cents)

            direct_dgb_successors = [
                entry for entry in successor_entries
                if entry["asset"] == "dgb"
            ]
            assert len(direct_dgb_successors) <= 1
            maintenance_txids = set()
            if direct_dgb_successors:
                assert_equal(direct_dgb_successors[0]["state"],
                             "pending_successor")
            else:
                def one_maintenance_transaction_created():
                    nonlocal maintenance_txids
                    maintenance_txids = (
                        set(node.getrawmempool()) - mempool_before -
                        {final_txid}
                    )
                    return len(maintenance_txids) == 1

                self.wait_until(one_maintenance_transaction_created)
                maintenance_entries = [
                    entry for entry in wallet.getpaymasterpoolinfo()["pool"]
                    if entry["txid"] in maintenance_txids and
                    entry["purpose"] == "operational" and
                    entry["asset"] == "dgb"
                ]
                assert_equal(len(maintenance_entries), 1)
                assert_equal(maintenance_entries[0]["state"],
                             "pending_successor")

            liquidity = cli.getpaymasterliquiditystatus()
            assert_equal(liquidity["operational_carriers"][
                "counted_toward_target"], 1)
            assert_equal(liquidity["operational_carriers"]["pending"], 1)
            assert_equal(liquidity["operational_carriers"]["missing"], 0)
            assert_equal(liquidity["operational_dgb"][
                "counted_toward_target"], 1)
            assert_equal(liquidity["operational_dgb"]["pending"], 1)
            assert_equal(liquidity["operational_dgb"]["missing"], 0)
            assert_equal(liquidity["maintenance_state"],
                         "waiting_for_liquidity_confirmation")

            # Let several scheduler ticks observe the pending outputs. They
            # count against the target and therefore must not trigger a
            # duplicate maintenance transaction.
            expected_new_txids = {final_txid} | maintenance_txids
            check_after = time.monotonic() + 2
            self.wait_until(lambda: time.monotonic() >= check_after)
            assert_equal(
                set(node.getrawmempool()) - mempool_before,
                expected_new_txids,
            )
            return maintenance_txids

        def wait_for_recycled_liquidity(final_txid,
                                        expected_carrier_cents):
            def successors_ready():
                liquidity = cli.getpaymasterliquiditystatus()
                active_carriers = [
                    entry for entry in wallet.getpaymasterpoolinfo()["pool"]
                    if entry["purpose"] == "operational" and
                    entry["asset"] == "dd_carrier" and
                    entry["state"] == "available"
                ]
                return (
                    liquidity["operational_carriers"]["ready"] == 1 and
                    liquidity["operational_carriers"]["pending"] == 0 and
                    liquidity["operational_carriers"]["missing"] == 0 and
                    liquidity["operational_dgb"]["ready"] == 1 and
                    liquidity["operational_dgb"]["pending"] == 0 and
                    liquidity["operational_dgb"]["missing"] == 0 and
                    len(active_carriers) == 1 and
                    active_carriers[0]["txid"] == final_txid and
                    active_carriers[0]["dd_cents"] ==
                    expected_carrier_cents
                )

            self.wait_until(successors_ready)
            liquidity = cli.getpaymasterliquiditystatus()
            assert_equal(liquidity["carrier_withdrawable_excess_cents"],
                         expected_carrier_cents - 100)

        def tighten_after_durable_authorization():
            # Both wallets have already durably accepted the exact artifacts.
            # Changing current policies must not strand this transfer, while
            # subsequent new quotes remain subject to the tightened limits.
            tightened_provider = provider_safety_policy(
                policy["funding_models"], policy["sponsorship_scope"])
            tightened_provider["maximum_active_quotes_total"] = 63
            cli.setpaymastersafetypolicy(tightened_provider)
            client.setpaymasterclientsafetypolicy({
                "maximum_service_fee_per_transaction_cents": 0,
                "maximum_service_fee_per_day_cents": 0,
            })

        user_paid_request_id = "550e8400-e29b-41d4-a716-446655440101"
        user_paid_mempool_before = set(node.getrawmempool())
        final_txid = complete_paymaster_transfer(
            user_paid_request_id, 500, 3,
            after_authorization=tighten_after_durable_authorization,
            automatic_provider=True)
        first_maintenance_txids = assert_automatic_liquidity_recovery(
            final_txid, 103, user_paid_mempool_before)
        client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10000,
        })
        user_paid_provider_safety = cli.getpaymastersafetystatus()["user_paid"]
        assert_equal(user_paid_provider_safety[
            "reserved_network_fee_satoshis"], 0)
        assert_equal(user_paid_provider_safety[
            "spent_network_fee_last_day_satoshis"], 10000000)
        assert_equal(user_paid_provider_safety["completed_last_day"], 1)
        user_paid_client_safety = client.getpaymasterclientsafetystatus()
        assert_equal(user_paid_client_safety["active_reservations"], 0)
        assert_equal(user_paid_client_safety["reserved_service_fee_cents"], 0)
        assert_equal(user_paid_client_safety[
            "spent_service_fee_last_day_cents"], 3)
        assert_equal(user_paid_client_safety[
            "available_service_fee_today_cents"], 9997)

        if self.pre_paymaster_digibyted:
            old_node = self.nodes[2]
            self.wait_until(lambda: final_txid in old_node.getrawmempool())
            assert_equal(old_node.getrawtransaction(final_txid),
                         node.getrawtransaction(final_txid))

        confirmation_block = self.generatetoaddress(
            node, 1, wallet.getnewaddress())[0]
        self.sync_blocks()
        wait_for_recycled_liquidity(final_txid, 103)
        first_liquidity = cli.getpaymasterliquiditystatus()
        if first_maintenance_txids:
            assert_equal(first_liquidity[
                "maintenance_fee_spent_last_day_satoshis"] > 0, True)

        self.log.info("Verify provider finance accounting and backup metadata")
        finance = wallet.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 1,
        })
        assert_equal(finance["provider_id"], identity["provider_id"])
        assert_equal(finance["service_fee_income_cents"], 3)
        assert_equal(finance["successful_transfers"], 1)
        assert_equal(finance["user_paid_transfers"], 1)
        assert_equal(finance["public_sponsored_transfers"], 0)
        assert_equal(finance["restricted_sponsored_transfers"], 0)
        assert_equal(finance["model_breakdown"]["user_paid"]
                     ["successful_transfers"], 1)
        assert_equal(finance["model_breakdown"]["user_paid"]
                     ["service_fee_income_cents"], 3)
        assert_equal(finance["model_breakdown"]["user_paid"]
                     ["dgb_operating_cost_satoshis"] > 0, True)
        assert_equal(finance["dgb_operating_cost_satoshis"] >= 10000000,
                     True)
        assert_equal(finance["period_summaries"]["all"]
                     ["service_fee_income_cents"], 3)
        assert_equal(finance["period_summaries"]["all"]
                     ["successful_transfers"], 1)
        assert_equal(finance["pool_capital"]["carrier_base_cents"], 400)
        assert_equal(finance["pool_capital"]["carrier_earned_cents"], 3)
        assert_equal(finance["pool_capital"]
                     ["carrier_withdrawable_cents"], 3)
        assert_equal(finance["backup_required"], True)
        assert_equal(len(finance["events"]), 1)
        assert "next_cursor" in finance
        assert_equal(len(finance["daily_totals"]) >= 1, True)
        assert_equal(finance["daily_totals"][0]
                     ["service_fee_income_cents"] >= 3, True)

        next_finance_page = wallet.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 1,
            "cursor": finance["next_cursor"],
        })
        assert_equal(len(next_finance_page["events"]), 1)
        assert (next_finance_page["events"][0]["event_id"] !=
                finance["events"][0]["event_id"])
        assert_raises_rpc_error(
            -8, "PAYMASTER_INVALID_FINANCE_QUERY",
            wallet.getpaymasterfinancestatus,
            {"period": "year", "include_events": False},
        )
        assert_raises_rpc_error(
            -8, "PAYMASTER_INVALID_FINANCE_CURSOR",
            wallet.getpaymasterfinancestatus,
            {"period": "all", "include_events": True,
             "cursor": "00" * 32},
        )
        assert_raises_rpc_error(
            -8, "PAYMASTER_EXTERNAL_BACKUP_NOT_CONFIRMED",
            wallet.acknowledgepaymasterproviderbackup,
            {"external_backup": False},
        )

        self.log.info(
            "Restore a full provider-wallet backup with identity, pool and finance history")
        backup_file = node.datadir_path / "paymaster_provider_finance.bak"
        wallet.backupwallet(backup_file)
        assert_equal(wallet.getpaymasterinfo()["backup_status"]["required"],
                     False)
        restored_wallet_name = "paymaster_finance_restored"
        node.restorewallet(restored_wallet_name, backup_file)
        try:
            restored = node.get_wallet_rpc(restored_wallet_name)
            restored_info = restored.getpaymasterinfo()
            restored_finance = restored.getpaymasterfinancestatus({
                "period": "all",
                "include_events": True,
                "limit": 10000,
            })
            restored_pool = restored.getpaymasterpoolinfo()["pool"]
            assert_equal(restored_info["provider_id"],
                         identity["provider_id"])
            assert_equal(restored_info["enabled"], True)
            assert_equal(restored_finance["provider_id"],
                         identity["provider_id"])
            assert_equal(restored_finance["service_fee_income_cents"], 3)
            assert_equal(restored_finance["successful_transfers"], 1)
            assert_equal(len(restored_pool),
                         len(wallet.getpaymasterpoolinfo()["pool"]))
            # The backup image is created before the source wallet records
            # successful completion. A restored machine therefore receives a
            # fresh, non-blocking reminder to back up its local provider copy.
            assert_equal(restored_finance["backup_required"], True)
        finally:
            node.unloadwallet(restored_wallet_name)

        acknowledged_backup = wallet.acknowledgepaymasterproviderbackup({
            "external_backup": True,
        })
        assert_equal(acknowledged_backup["acknowledged"], True)
        assert_equal(acknowledged_backup["backup_required"], False)
        assert_equal(wallet.getpaymasterinfo()["backup_status"]["required"],
                     False)
        if self.pre_paymaster_digibyted:
            old_node = self.nodes[2]
            assert final_txid in old_node.getblock(confirmation_block)["tx"]
            assert_equal(old_node.getbestblockhash(), node.getbestblockhash())

        self.log.info(
            "Replay a finalized USER_PAID request through requestpaymasterquote")
        user_paid_lookup = {"request_id": user_paid_request_id}
        self.wait_until(lambda: client.getdigidollarsendsession(
            user_paid_lookup)["session_state"] == "CONFIRMED")
        finalized_user_paid = client.getdigidollarsendsession(user_paid_lookup)
        assert_equal(finalized_user_paid["final"], True)
        assert_equal(finalized_user_paid["txid"], final_txid)
        assert_equal(len(finalized_user_paid["reserved_user_inputs"]), 1)

        tombstone_retry = client.requestpaymasterquote(
            identity["provider_id"], {
                "request_id": user_paid_request_id,
                "address": recipient,
                "amount_cents": 500,
                "maximum_paymaster_fee_cents": 3,
                "fee_mode": "paymaster",
                "privacy": "standard",
                "selection": "lowest_total_cost",
                "maximum_provider_attempts": 3,
                "selected_inputs": finalized_user_paid[
                    "reserved_user_inputs"],
            })
        assert_equal(tombstone_retry, finalized_user_paid)
        assert_equal(tombstone_retry["provider_id"], identity["provider_id"])
        assert_equal(tombstone_retry["funding_model"], "user_paid")
        assert_equal(tombstone_retry["payment_cents"], 500)
        assert_equal(tombstone_retry["service_fee_cents"], 3)
        assert_equal(tombstone_retry["user_total_cents"], 503)
        assert_equal(tombstone_retry["requested_amount_cents"], 500)
        assert_equal(
            tombstone_retry["subtract_paymaster_fee_from_amount"], False)
        assert_equal(tombstone_retry["send_all_spendable_dd"], False)
        for quote_field in (
                "expires_at", "queued", "connection_pending",
                "capacity_pending", "capacity_snapshot_id", "quote_id",
                "unsigned_txid", "template_commitment",
                "authorization_commitment", "authorization_accepted",
                "authorization_accepted_at", "psbt"):
            assert quote_field not in tombstone_retry
        assert_equal(client.getdigidollarsendsession(user_paid_lookup),
                     finalized_user_paid)

        self.log.info(
            "Recycle the USER_PAID carrier again without duplicate maintenance")
        self.wait_until(
            lambda: wallet.getpaymasterinfo()["service_state"] == "active")
        second_user_paid_request_id = (
            "550e8400-e29b-41d4-a716-446655440108")
        second_mempool_before = set(node.getrawmempool())
        second_user_paid_txid = complete_paymaster_transfer(
            second_user_paid_request_id, 500, 3,
            automatic_provider=True)
        second_maintenance_txids = assert_automatic_liquidity_recovery(
            second_user_paid_txid, 106, second_mempool_before)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        wait_for_recycled_liquidity(second_user_paid_txid, 106)

        second_provider_safety = cli.getpaymastersafetystatus()["user_paid"]
        assert_equal(second_provider_safety[
            "reserved_network_fee_satoshis"], 0)
        assert_equal(second_provider_safety[
            "spent_network_fee_last_day_satoshis"], 20000000)
        assert_equal(second_provider_safety["completed_last_day"], 2)
        second_client_safety = client.getpaymasterclientsafetystatus()
        assert_equal(second_client_safety["active_reservations"], 0)
        assert_equal(second_client_safety["reserved_service_fee_cents"], 0)
        assert_equal(second_client_safety[
            "spent_service_fee_last_day_cents"], 6)
        assert_equal(second_client_safety[
            "available_service_fee_today_cents"], 9994)
        second_liquidity = cli.getpaymasterliquiditystatus()
        if second_maintenance_txids:
            assert_equal(second_liquidity[
                "maintenance_fee_spent_last_day_satoshis"] >
                first_liquidity[
                    "maintenance_fee_spent_last_day_satoshis"], True)

        self.log.info(
            "Empty a 50 DD wallet by deducting the exact Paymaster fee")
        client_node.createwallet(
            wallet_name="sweep-client",
            descriptors=True,
            load_on_startup=True,
        )
        client_node.createwallet(
            wallet_name="sweep-recipient",
            descriptors=True,
            load_on_startup=True,
        )
        sweep_client = client_node.get_wallet_rpc("sweep-client")
        sweep_recipient = client_node.get_wallet_rpc("sweep-recipient")
        sweep_client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10000,
        })
        sweep_recipient_address = sweep_recipient.getdigidollaraddress()

        sweep_mint = wallet.mintdigidollar(5000, 0)
        assert_equal(sweep_mint["dd_minted"], 5000)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        sweep_funding = wallet.senddigidollar(
            sweep_client.getdigidollaraddress(), 5000)
        assert_equal(len(sweep_funding["txid"]), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        assert_equal(sweep_client.getbalance(), 0)
        assert_equal(sweep_recipient.getbalance(), 0)
        assert_equal(sweep_client.getdigidollarbalance()["total"], 5000)
        assert_equal(sweep_recipient.getdigidollarbalance()["total"], 0)

        exact_offers = sweep_client.getpaymasteroffers(
            5000, {"subtract_paymaster_fee_from_amount": True})
        exact_offer = next(
            entry for entry in exact_offers
            if entry["provider_id"] == identity["provider_id"] and
            entry["funding_model"] == "user_paid")
        assert_equal(exact_offer["payment_cents"], 4975)
        assert_equal(exact_offer["service_fee_cents"], 25)
        assert_equal(exact_offer["user_total_cents"], 5000)
        assert_equal(
            exact_offer["subtract_paymaster_fee_from_amount"], True)

        exact_gap_request_id = (
            "550e8400-e29b-41d4-a716-446655440109")
        assert_raises_rpc_error(
            -4,
            "PAYMASTER_NO_EXACT_GROSS_OFFER",
            sweep_client.senddigidollar,
            sweep_recipient_address,
            202,
            "",
            0,
            None,
            "cents", {
                "fee_mode": "paymaster",
                "request_id": exact_gap_request_id,
                "maximum_paymaster_fee_cents": 100,
                "privacy": "standard",
                "selection": "lowest_total_cost",
                "subtract_paymaster_fee_from_amount": True,
            },
        )
        assert_raises_rpc_error(
            -4,
            "Paymaster session not found",
            sweep_client.getdigidollarsendsession,
            {"request_id": exact_gap_request_id},
        )
        assert_equal(sweep_client.getdigidollarbalance()["total"], 5000)

        sweep_request_id = "550e8400-e29b-41d4-a716-446655440110"
        sweep_mempool_before = set(node.getrawmempool())
        sweep_txid = complete_paymaster_transfer(
            sweep_request_id,
            5000,
            100,
            extra_options={
                "subtract_paymaster_fee_from_amount": True,
                "send_all_spendable_dd": True,
            },
            automatic_provider=True,
            client_wallet=sweep_client,
            payment_recipient=sweep_recipient_address,
        )
        sweep_session = sweep_client.getdigidollarsendsession(
            {"request_id": sweep_request_id})
        assert_equal(sweep_session["requested_amount_cents"], 5000)
        assert_equal(
            sweep_session["subtract_paymaster_fee_from_amount"], True)
        assert_equal(sweep_session["send_all_spendable_dd"], True)
        assert_equal(sweep_session["payment_cents"], 4975)
        assert_equal(sweep_session["service_fee_cents"], 25)
        assert_equal(sweep_session["user_total_cents"], 5000)
        assert_automatic_liquidity_recovery(
            sweep_txid, 131, sweep_mempool_before)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        wait_for_recycled_liquidity(sweep_txid, 131)
        assert_equal(sweep_client.getdigidollarbalance()["total"], 0)
        assert_equal(sweep_recipient.getdigidollarbalance()["total"], 4975)
        assert_equal(sweep_client.getbalance(), 0)
        assert_equal(sweep_recipient.getbalance(), 0)

        # A collaborative Paymaster transaction is not created by the normal
        # direct-send wallet path. The final result must nevertheless replace
        # any provisional wallet-owned change row with one durable outgoing
        # history row for the client's exact gross DD outflow.
        sweep_history = [
            entry for entry in sweep_client.listdigidollartxs(1000, 0)
            if entry["txid"] == sweep_txid
        ]
        assert_equal(len(sweep_history), 1)
        assert_equal(sweep_history[0]["category"], "send")
        assert_equal(sweep_history[0]["amount"], -5000)
        assert_equal(sweep_history[0]["address"], sweep_recipient_address)

        self.wait_until(
            lambda: sweep_client.getdigidollarsendsession({
                "request_id": sweep_request_id,
            })["session_state"] == "CONFIRMED")
        sweep_retry = sweep_client.senddigidollar(
            sweep_recipient_address,
            5000,
            "",
            0,
            None,
            "cents", {
                "fee_mode": "paymaster",
                "request_id": sweep_request_id,
                "maximum_paymaster_fee_cents": 100,
                "privacy": "standard",
                "selection": "lowest_total_cost",
                "subtract_paymaster_fee_from_amount": True,
                "send_all_spendable_dd": True,
            },
        )
        assert_equal(sweep_retry["txid"], sweep_txid)
        assert_equal(sweep_retry["amount"], 4975)
        assert_equal(sweep_retry["payment_cents"], 4975)
        assert_equal(sweep_retry["service_fee_cents"], 25)
        assert_equal(sweep_retry["user_total_cents"], 5000)
        assert_equal(sweep_retry["final"], True)

        # The exact-sweep regression deliberately exercises enough automatic
        # liquidity refreshes to fill the client's one-minute announcement
        # validation bucket. Restart the client node so the following manual
        # policy-switch scenario starts with an independent discovery window.
        # All client wallets are load-on-startup, so this also proves that the
        # finalized exact-gross session remains durable across a node restart.
        self.restart_node(1)
        self.connect_nodes(1, 0, peer_advertises_v2=True)
        if self.pre_paymaster_digibyted:
            self.connect_nodes(1, 2, peer_advertises_v2=True)
        self.sync_blocks()
        client = client_node.get_wallet_rpc("client")

        assert_equal(client.getbalance(), 0)
        assert_equal(client.getdigidollarbalance()["total"], 100)
        automatic_sequence = wallet.getpaymasterinfo()[
            "announcement_sequence"]
        self.wait_until(
            lambda: any(
                entry["provider_id"] == identity["provider_id"] and
                entry["sequence"] >= automatic_sequence
                for entry in client_node.listpaymasters()))

        self.log.info(
            "Reject fee-bearing Paymaster use by a 1 DD wallet without DGB")
        boundary_recipient = wallet.getdigidollaraddress()
        boundary_balance = client.getdigidollarbalance()
        boundary_client_safety = client.getpaymasterclientsafetystatus()
        boundary_provider_safety = wallet.getpaymastersafetystatus()[
            "user_paid"]
        boundary_offers = [
            offer for offer in client.getpaymasteroffers(100)
            if offer["provider_id"] == identity["provider_id"] and
            offer["funding_model"] == "user_paid"
        ]
        assert_equal(len(boundary_offers), 1)
        assert_equal(boundary_offers[0]["payment_cents"], 100)
        assert_equal(boundary_offers[0]["service_fee_cents"], 1)
        assert_equal(boundary_offers[0]["user_total_cents"], 101)

        additive_boundary_request = (
            "550e8400-e29b-41d4-a716-446655440111")
        boundary_options = {
            "fee_mode": "paymaster",
            "request_id": additive_boundary_request,
            "maximum_paymaster_fee_cents": 100,
            "privacy": "standard",
            "selection": "lowest_total_cost",
        }
        assert_raises_rpc_error(
            -6,
            "Insufficient confirmed DigiDollar inputs",
            client.senddigidollar,
            boundary_recipient,
            100,
            "",
            0,
            None,
            "cents", boundary_options,
        )
        assert_raises_rpc_error(
            -4,
            "Paymaster session not found",
            client.getdigidollarsendsession,
            {"request_id": additive_boundary_request},
        )

        self.log.info(
            "Reject deducting a Paymaster fee below the 1 DD payment floor")
        gross_boundary_request = "550e8400-e29b-41d4-a716-446655440112"
        boundary_options["request_id"] = gross_boundary_request
        boundary_options["subtract_paymaster_fee_from_amount"] = True
        assert_raises_rpc_error(
            -4,
            "PAYMASTER_NO_EXACT_GROSS_OFFER",
            client.senddigidollar,
            boundary_recipient,
            100,
            "",
            0,
            None,
            "cents", boundary_options,
        )
        assert_raises_rpc_error(
            -4,
            "Paymaster session not found",
            client.getdigidollarsendsession,
            {"request_id": gross_boundary_request},
        )

        # Both failures occur before input reservation or provider work. The
        # client's exact 1.00 DD remains ordinary spendable balance, and both
        # client and provider safety ledgers remain unchanged.
        assert_equal(client.getdigidollarbalance(), boundary_balance)
        assert_equal(client.getpaymasterclientsafetystatus(),
                     boundary_client_safety)
        assert_equal(wallet.getpaymastersafetystatus()["user_paid"],
                     boundary_provider_safety)
        assert_equal(wallet.stoppaymaster()["running"], False)

        # The remainder of this regression file deliberately exercises the
        # expert one-message RPCs. Switching mode is allowed only now that the
        # automatic runtime is stopped.
        manual_runtime = wallet.setpaymasterruntimesettings({
            "operation_mode": "manual",
            "autostart": False,
        })
        assert_equal(manual_runtime["operation_mode"], "manual")
        assert_equal(manual_runtime["autostart"], False)
        assert_equal(manual_runtime["running"], False)

        self.log.info(
            "Spend the same 1 DD wallet through a zero-fee public sponsor")
        policy["funding_models"] = ["sponsored"]
        policy["sponsorship_scope"] = "public"
        policy["fee_rate_bps"] = 0
        # A disjoint funding-model switch needs a temporary safety superset.
        cli.setpaymastersafetypolicy(
            provider_safety_policy(["sponsored", "user_paid"],
                                   policy["sponsorship_scope"]))
        persisted_policy = cli.setpaymasterpolicy(policy)
        cli.setpaymastersafetypolicy(
            provider_safety_policy(policy["funding_models"],
                                   policy["sponsorship_scope"]))

        sponsored_target = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
        }
        sponsored_preview = cli.preparepaymasterpool(sponsored_target)
        assert_equal(sponsored_preview["executed"], False)
        assert_equal(sponsored_preview["missing_admission_dgb_slots"], 0)
        assert_equal(sponsored_preview["missing_operational_dgb_slots"], 1)

        sponsored_target["execute"] = True
        sponsored_target["plan_id"] = sponsored_preview["plan_id"]
        sponsored_pool = cli.preparepaymasterpool(sponsored_target)
        assert_equal(sponsored_pool["executed"], True)
        assert_equal(len(sponsored_pool["dgb_txid"]), 64)
        assert "dd_txid" not in sponsored_pool
        self.generatetoaddress(node, 1, wallet.getnewaddress())

        sponsored_started = wallet.startpaymaster()
        assert_equal(sponsored_started["running"], True)
        assert_equal(sponsored_started["announcement_sequence"],
                     automatic_sequence + 1)
        # Automatic policy/liquidity refreshes may publish a newer monotonic
        # announcement before the client observes the explicit start.
        self.wait_until(
            lambda: any(entry["provider_id"] == identity["provider_id"] and
                        entry["sequence"] >= sponsored_started[
                            "announcement_sequence"]
                        for entry in client_node.listpaymasters())
        )

        self.log.info("Prove payment-intent isolation with two eligible providers")
        node.createwallet(
            wallet_name="paymaster-shadow",
            descriptors=True,
            load_on_startup=True,
        )
        shadow = node.get_wallet_rpc("paymaster-shadow")
        shadow_cli = node.cli("-rpcwallet=paymaster-shadow")
        shadow_identity = shadow.createpaymasteridentity("Functional Shadow Provider")
        provider_wallets[shadow_identity["provider_id"]] = shadow
        # Keep enough confirmed DGB outside the first sponsored pool for the
        # later model switch to build carrier outputs and replenish a slot if
        # this provider was selected for the sponsored transfer.
        shadow_funding = wallet.sendtoaddress(
            shadow.getnewaddress(address_type="bech32m"), 2)
        assert_equal(len(shadow_funding), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())

        shadow_policy = shadow_cli.setpaymasterpolicy(policy)
        assert_equal(shadow_policy["funding_models"], ["sponsored"])
        assert_equal(shadow_policy["fee_rate_bps"], 0)
        shadow_cli.setpaymastersafetypolicy(
            provider_safety_policy(policy["funding_models"],
                                   policy["sponsorship_scope"]))
        assert_equal(shadow_cli.setpaymasterenabled(True)["enabled"], True)
        shadow_runtime = shadow.setpaymasterruntimesettings({
            "operation_mode": "manual",
            "autostart": False,
        })
        assert_equal(shadow_runtime["operation_mode"], "manual")
        assert_equal(shadow_runtime["autostart"], False)
        shadow_pool_target = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
        }
        shadow_pool_preview = shadow_cli.preparepaymasterpool(
            shadow_pool_target)
        shadow_pool_target["execute"] = True
        shadow_pool_target["plan_id"] = shadow_pool_preview["plan_id"]
        shadow_pool = shadow_cli.preparepaymasterpool(shadow_pool_target)
        assert_equal(shadow_pool["executed"], True)
        assert_equal(len(shadow_pool["dgb_txid"]), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())

        shadow_started = shadow.startpaymaster()
        assert_equal(shadow_started["running"], True)
        assert_equal(shadow_started["ready"], True)
        assert_equal(shadow_started["announcement_sequence"], 1)
        self.wait_until(
            lambda: {
                entry["provider_id"] for entry in client_node.listpaymasters()
            }.issuperset({identity["provider_id"], shadow_identity["provider_id"]})
        )

        sponsored_offers = client.getpaymasteroffers(100)
        sponsored_offers = [
            offer for offer in sponsored_offers
            if offer["funding_model"] == "sponsored" and
            offer["provider_id"] in
            (identity["provider_id"], shadow_identity["provider_id"])
        ]
        assert_equal(len(sponsored_offers), 2)
        assert_equal(sponsored_offers[0]["funding_model"], "sponsored")
        assert_equal(sponsored_offers[0]["service_fee_cents"], 0)
        assert_equal(sponsored_offers[0]["user_total_cents"], 100)
        assert_equal(sponsored_offers[1]["service_fee_cents"], 0)
        assert_equal(sponsored_offers[1]["user_total_cents"], 100)
        selected_provider = (
            wallet if sponsored_offers[0]["provider_id"] == identity["provider_id"]
            else shadow
        )
        unselected_provider = shadow if selected_provider is wallet else wallet

        sponsored_txid = complete_paymaster_transfer(
            "550e8400-e29b-41d4-a716-446655440102", 100, 0,
            provider_wallet=selected_provider,
            unselected_provider_wallet=unselected_provider)
        assert sponsored_txid != final_txid
        assert sponsored_txid != second_user_paid_txid
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        assert_equal(client.getbalance(), 0)
        assert_equal(client.getdigidollarbalance()["total"], 0)
        sponsored_finance = selected_provider.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10000,
        })
        sponsored_events = [
            event for event in sponsored_finance["events"]
            if event.get("transaction_id") == sponsored_txid
        ]
        assert_equal(len(sponsored_events), 1)
        assert_equal(sponsored_events[0]["kind"], "transfer")
        assert_equal(sponsored_events[0]["state"], "confirmed")
        assert_equal(sponsored_events[0]["funding_model"], "sponsored")
        assert_equal(sponsored_events[0]["sponsorship_scope"], "public")
        assert_equal(sponsored_events[0]["dd_income_cents"], 0)
        assert_equal(sponsored_events[0]["dgb_cost_satoshis"], 10000000)

        self.log.info(
            "Recover without client DGB through a distinct Capacity-v5 provider")
        assert_equal(client.getbalance(), 0)

        # Convert the shadow provider into a finite-budget USER_PAID recovery
        # provider. The original provider remains the sole public sponsor, so
        # the ambiguous original authorization and its recovery are rooted in
        # different identities.
        assert_equal(shadow.stoppaymaster()["running"], False)
        recovery_provider_policy = dict(policy)
        recovery_provider_policy["funding_models"] = ["user_paid"]
        recovery_provider_policy["sponsorship_scope"] = "public"
        recovery_provider_policy["fee_rate_bps"] = 50
        # Keep the currently advertised model safe throughout the switch.
        shadow_cli.setpaymastersafetypolicy(provider_safety_policy(
            ["sponsored", "user_paid"],
            recovery_provider_policy["sponsorship_scope"]))
        shadow_cli.setpaymasterpolicy(recovery_provider_policy)
        shadow_cli.setpaymastersafetypolicy(provider_safety_policy(
            recovery_provider_policy["funding_models"],
            recovery_provider_policy["sponsorship_scope"]))

        shadow_dd_funding = wallet.senddigidollar(
            shadow.getdigidollaraddress(), 600)
        assert_equal(len(shadow_dd_funding["txid"]), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        recovery_pool_target = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 2,
        }
        recovery_pool_preview = shadow_cli.preparepaymasterpool(
            recovery_pool_target)
        if any(recovery_pool_preview[key] > 0 for key in (
                "missing_admission_dgb_slots",
                "missing_operational_dgb_slots",
                "missing_admission_carrier_slots",
                "missing_operational_carrier_slots")):
            recovery_pool_target["execute"] = True
            recovery_pool_target["plan_id"] = recovery_pool_preview["plan_id"]
            recovery_pool = shadow_cli.preparepaymasterpool(
                recovery_pool_target)
            assert_equal(recovery_pool["executed"], True)
            assert_equal(len(recovery_pool["dd_txid"]), 64)
            self.generatetoaddress(node, 1, wallet.getnewaddress())
        shadow_recovery_started = shadow.startpaymaster()
        assert_equal(shadow_recovery_started["running"], True)
        assert_equal(shadow_recovery_started["ready"], True)
        self.wait_until(lambda: any(
            offer["provider_id"] == shadow_identity["provider_id"] and
            offer["funding_model"] == "user_paid"
            for offer in client.getpaymasteroffers(200)))

        # Fund DD only. A 200-cent input leaves an exact 199-cent wallet return
        # after the recovery provider's one-cent service fee; the client still
        # owns no DGB at any point.
        recovery_funding = wallet.senddigidollar(client_address, 200)
        assert_equal(len(recovery_funding["txid"]), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        assert_equal(client.getdigidollarbalance()["total"], 200)
        assert_equal(client.getbalance(), 0)

        recovery_request_id = "550e8400-e29b-41d4-a716-446655440105"
        interrupted = complete_paymaster_transfer(
            recovery_request_id, 100, 0,
            stop_after_authorization=True, provider_wallet=wallet)
        assert_equal(interrupted["session_state"], "PENDING_PROVIDER")
        assert_equal(interrupted["provider_id"], identity["provider_id"])
        interrupted_session_id = interrupted["session_id"]

        recovery_lookup = {"request_id": recovery_request_id}
        prepare_recovery_options = {
            "recovery_provider_id": shadow_identity["provider_id"],
            "maximum_recovery_service_fee_cents": 1,
            "prepare_only": True,
        }

        initial_recovery = client.resolvepaymastersession(
            recovery_lookup, "cancel_to_self", prepare_recovery_options)
        assert_equal(initial_recovery["artifact"], "alternative_recovery")
        assert "refresh" in initial_recovery["allowed_actions"]
        assert "cancel_to_self" in initial_recovery["allowed_actions"]
        assert_equal(initial_recovery["result_status"], None)
        assert_equal(initial_recovery["result_sequence"], None)
        assert_equal(initial_recovery["recovery"]["phase"],
                     "capacity_pending")
        assert_equal(initial_recovery["recovery"]["user_dd_inputs"], [])
        assert_equal(initial_recovery["recovery"]["wallet_returns"], [])
        assert_equal(initial_recovery["recovery"]["recovery_provider_id"],
                     shadow_identity["provider_id"])

        # Until a signed Capacity-v5 proof is received, exact retries expose no
        # user outpoint or return script to the recovery provider.
        self.wait_until(lambda: client.resolvepaymastersession(
            recovery_lookup, "cancel_to_self",
            prepare_recovery_options).get("queued", False))
        capacity_reply = {}

        def process_recovery_capacity():
            nonlocal capacity_reply
            # Re-drive the exact client artifact while polling. A busy direct
            # transport may deliberately close the short-lived Paymaster
            # connection after applying its per-peer window; the production
            # client retries the same idempotent Capacity request on the
            # replacement connection as well.
            client.resolvepaymastersession(
                recovery_lookup, "cancel_to_self", prepare_recovery_options)
            capacity_reply = shadow.processpaymasterrequests()
            return capacity_reply.get("processed", False)

        self.wait_until(process_recovery_capacity)
        assert_equal(capacity_reply["message_type"], "capacity")
        assert_equal(capacity_reply["provider_id"],
                     shadow_identity["provider_id"])

        disclosed_request = {}

        def queue_recovery_request():
            nonlocal disclosed_request
            disclosed_request = client.resolvepaymastersession(
                recovery_lookup, "cancel_to_self", prepare_recovery_options)
            return (disclosed_request["recovery"]["phase"] ==
                    "request_ready" and disclosed_request.get("queued", False))

        self.wait_until(queue_recovery_request)
        assert_equal(len(disclosed_request["recovery"]["user_dd_inputs"]), 1)
        assert_equal(len(disclosed_request["recovery"]["wallet_returns"]), 1)

        recovery_quote = {}

        def process_recovery_request():
            nonlocal recovery_quote
            client.resolvepaymastersession(
                recovery_lookup, "cancel_to_self", prepare_recovery_options)
            recovery_quote = shadow.processpaymasterrequests()
            if not recovery_quote.get("processed", False):
                return False
            assert_equal(recovery_quote["queued"], True)
            if recovery_quote["message_type"] == "capacity":
                return False
            assert_equal(recovery_quote["message_type"], "recovery_request")
            return True

        self.wait_until(process_recovery_request)
        assert_equal(recovery_quote["message_type"], "recovery_request")
        assert_equal(recovery_quote["provider_id"],
                     shadow_identity["provider_id"])

        prepared_recovery = {}

        def receive_recovery_quote():
            nonlocal prepared_recovery
            prepared_recovery = client.resolvepaymastersession(
                recovery_lookup, "cancel_to_self", prepare_recovery_options)
            return (prepared_recovery["recovery"]["phase"] ==
                    "response_validated")

        self.wait_until(receive_recovery_quote)
        recovery_commitment = prepared_recovery["recovery"][
            "authorization_commitment"]
        assert_equal(prepared_recovery["recovery"]["authorization_accepted"],
                     False)
        assert_equal(prepared_recovery["recovery"]["service_fee_cents"], 1)
        assert_equal(prepared_recovery["recovery"]["wallet_returns"][0]
                     ["amount_cents"], 199)

        # A one-bit commitment mutation is rejected before any recovery user
        # signature exists.
        wrong_commitment = (
            ("00" if recovery_commitment[:2] != "00" else "01") +
            recovery_commitment[2:])
        mutated_options = dict(prepare_recovery_options)
        mutated_options.pop("prepare_only")
        mutated_options["recovery_authorization_commitment"] = wrong_commitment
        assert_raises_rpc_error(
            -8, "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_MISMATCH",
            client.resolvepaymastersession, recovery_lookup,
            "cancel_to_self", mutated_options)

        # Restart the client between preparation and authorization. The exact
        # manifest, fee reservation boundary, capacity binding and provider
        # route must survive, while the original DD input remains reserved.
        # A provider-side quote alone intentionally does not survive a later
        # operator policy change as fresh signing authority; that fail-closed
        # boundary is covered by the wallet security regression tests.
        self.restart_node(1)
        self.connect_nodes(0, 1)
        if self.pre_paymaster_digibyted:
            self.connect_nodes(1, 2, peer_advertises_v2=True)
        client_node = self.nodes[1]
        client = client_node.get_wallet_rpc("client")
        restarted_inbox = client.listdigidollarsendsessions()
        restarted_summary = next(
            entry for entry in restarted_inbox["sessions"]
            if entry["request_id"] == recovery_request_id)
        assert_equal(restarted_summary["artifact"], "alternative_recovery")
        assert_equal(restarted_summary["requires_attention"], True)
        assert "cancel_to_self" in restarted_summary["allowed_actions"]
        restarted_prepared = client.resolvepaymastersession(
            recovery_lookup, "cancel_to_self", prepare_recovery_options)
        assert_equal(restarted_prepared["recovery"]["authorization_commitment"],
                     recovery_commitment)
        assert_equal(restarted_prepared["recovery"]["authorization_accepted"],
                     False)
        assert_equal(client.getbalance(), 0)

        authorize_options = dict(prepare_recovery_options)
        authorize_options.pop("prepare_only")
        authorize_options["recovery_authorization_commitment"] = (
            recovery_commitment)
        authorized_recovery = {}

        def authorize_recovery():
            nonlocal authorized_recovery
            authorized_recovery = client.resolvepaymastersession(
                recovery_lookup, "cancel_to_self", authorize_options)
            return (authorized_recovery["recovery"]["phase"] ==
                    "user_signed" and authorized_recovery.get("queued", False))

        self.wait_until(authorize_recovery)
        assert_equal(authorized_recovery["recovery"]["authorization_accepted"],
                     True)

        client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 0,
            "maximum_service_fee_per_day_cents": 0,
        })
        resumed_authorized_recovery = client.resolvepaymastersession(
            recovery_lookup, "cancel_to_self", authorize_options)
        assert_equal(resumed_authorized_recovery["recovery"][
            "authorization_commitment"], recovery_commitment)
        assert_equal(resumed_authorized_recovery["recovery"][
            "authorization_accepted"], True)
        assert resumed_authorized_recovery["recovery"]["phase"] in (
            "user_signed", "final_committed")

        recovery_commit = {}

        def process_recovery_submit():
            nonlocal recovery_commit
            client.resolvepaymastersession(
                recovery_lookup, "cancel_to_self", authorize_options)
            recovery_commit = shadow.processpaymastersubmits()
            return (recovery_commit.get("processed", False) and
                    recovery_commit.get("message_type") == "recovery_submit")

        self.wait_until(process_recovery_submit)
        assert_equal(recovery_commit["queued"], True)
        recovery_txid = recovery_commit["txid"]

        completed_recovery = {}

        def receive_recovery_result():
            nonlocal completed_recovery
            completed_recovery = client.resolvepaymastersession(
                recovery_lookup, "cancel_to_self", authorize_options)
            return (completed_recovery["recovery"]["phase"] ==
                    "final_committed")

        self.wait_until(receive_recovery_result)
        assert_equal(completed_recovery["recovery"]["txid"], recovery_txid)
        assert_equal(completed_recovery["broadcast"], True)
        recovery_hex = completed_recovery["recovery"]["raw_transaction"]
        pending_recovery = client.getdigidollarsendsession(recovery_lookup)
        assert_equal(pending_recovery["final"], False)
        assert_equal(len(pending_recovery["reserved_user_inputs"]), 1)

        self.wait_until(lambda: recovery_txid in node.getrawmempool())
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()

        confirmed_recovery = {}

        def recovery_confirmed():
            nonlocal confirmed_recovery
            confirmed_recovery = client.getdigidollarsendsession(
                recovery_lookup)
            return confirmed_recovery["session_state"] == "CANCELED_SAFE"

        self.wait_until(recovery_confirmed)
        assert_equal(confirmed_recovery["final"], True)
        assert_equal(confirmed_recovery["confirmation_state"],
                     "recovery_confirmed")
        assert_equal(confirmed_recovery["recovery_txid"], recovery_txid)
        assert_equal(client.getdigidollarbalance()["total"], 199)
        assert_equal(client.getbalance(), 0)
        client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10000,
        })
        recovery_provider_safety = shadow.getpaymastersafetystatus()[
            "user_paid"]
        assert_equal(recovery_provider_safety[
            "reserved_network_fee_satoshis"], 0)
        assert_equal(recovery_provider_safety[
            "spent_network_fee_last_day_satoshis"], 10000000)
        assert_equal(recovery_provider_safety["completed_last_day"], 1)
        recovery_client_safety = client.getpaymasterclientsafetystatus()
        assert_equal(recovery_client_safety["active_reservations"], 0)
        assert_equal(recovery_client_safety["reserved_service_fee_cents"], 0)
        assert_equal(recovery_client_safety[
            "spent_service_fee_last_day_cents"], 7)
        assert_equal(recovery_client_safety[
            "available_service_fee_today_cents"], 9993)
        assert_equal(wallet.stoppaymaster()["running"], False)
        assert_equal(shadow.stoppaymaster()["running"], False)

        self.log.info("Complete a non-gossiped restricted SPONSORED transfer")
        policy["sponsorship_scope"] = "restricted"
        # Public and restricted sponsorship use independent safety budgets.
        sponsorship_transition_safety = provider_safety_policy(
            policy["funding_models"], "public")
        sponsorship_transition_safety["restricted_sponsored"] = (
            sponsorship_transition_safety["public_sponsored"].copy())
        cli.setpaymastersafetypolicy(sponsorship_transition_safety)
        persisted_policy = cli.setpaymasterpolicy(policy)
        cli.setpaymastersafetypolicy(
            provider_safety_policy(policy["funding_models"],
                                   policy["sponsorship_scope"]))

        # Restore sufficient operational capacity after the prior transfers.
        # Capability reuse itself is rejected earlier by the persistent
        # Capacity-v5 nonce firewall and never consumes this pool.
        restricted_target = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
        }
        restricted_preview = cli.preparepaymasterpool(restricted_target)
        assert_equal(restricted_preview["executed"], False)
        if (restricted_preview["missing_admission_dgb_slots"] > 0 or
                restricted_preview["missing_operational_dgb_slots"] > 0):
            restricted_target["execute"] = True
            restricted_target["plan_id"] = restricted_preview["plan_id"]
            restricted_pool = cli.preparepaymasterpool(restricted_target)
            assert_equal(restricted_pool["executed"], True)
            assert_equal(len(restricted_pool["dgb_txid"]), 64)
            self.generatetoaddress(node, 1, wallet.getnewaddress())

        restricted_started = wallet.startpaymaster()
        assert_equal(restricted_started["running"], True)
        assert_equal(restricted_started["ready"], True)
        assert "announcement_sequence" not in restricted_started

        sponsor_secret = (42).to_bytes(32, "big")
        sponsor_key, _ = compute_xonly_pubkey(sponsor_secret)
        assert sponsor_key is not None
        descriptor = wallet.createrestrictedpaymasterdescriptor(
            sponsor_key.hex(), "Functional Test Sponsor", 300)
        assert_equal(descriptor["provider_id"], identity["provider_id"])
        assert_equal(descriptor["policy_hash"], persisted_policy["policy_hash"])

        # Refill only the client DD side. Restricted sponsorship remains free
        # to the user and is never exposed through public discovery.
        restricted_funding = wallet.senddigidollar(client_address, 100)
        assert_equal(len(restricted_funding["txid"]), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        assert_equal(client.getdigidollarbalance()["total"], 299)

        restricted_amount = 100
        payment_nonce = "7f" + "00" * 30 + "01"
        capability_expiry = min(
            descriptor["expires_at"], int(time.time()) + 240)
        capability = sponsorship_capability(
            sponsor_secret,
            descriptor,
            digidollar_scriptpubkey(recipient),
            restricted_amount,
            payment_nonce,
            capability_expiry,
            node.getblockhash(0),
        )
        restricted_options = {
            "maximum_provider_attempts": 1,
            "provider_identity_key": descriptor["provider_identity_key"],
            "restricted_service_descriptor": descriptor["descriptor"],
            "sponsorship_capability": capability,
        }
        restricted_txid = complete_paymaster_transfer(
            "550e8400-e29b-41d4-a716-446655440103",
            restricted_amount,
            0,
            restricted_options,
        )
        assert restricted_txid not in (final_txid, sponsored_txid)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        assert_equal(client.getdigidollarbalance()["total"], 199)
        restricted_provider_safety = wallet.getpaymastersafetystatus()[
            "restricted_sponsored"]
        assert_equal(restricted_provider_safety[
            "reserved_network_fee_satoshis"], 0)
        assert_equal(restricted_provider_safety[
            "spent_network_fee_last_day_satoshis"], 10000000)
        assert_equal(restricted_provider_safety["completed_last_day"], 1)
        restricted_finance = wallet.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10000,
        })
        restricted_events = [
            event for event in restricted_finance["events"]
            if event.get("transaction_id") == restricted_txid
        ]
        assert_equal(len(restricted_events), 1)
        assert_equal(restricted_events[0]["kind"], "transfer")
        assert_equal(restricted_events[0]["state"], "confirmed")
        assert_equal(restricted_events[0]["funding_model"], "sponsored")
        assert_equal(restricted_events[0]["sponsorship_scope"], "restricted")
        assert_equal(restricted_events[0]["dd_income_cents"], 0)
        assert_equal(restricted_events[0]["dgb_cost_satoshis"], 10000000)

        self.log.info("Reject reuse of the consumed restricted capability")
        # The recovered 199-cent output cannot fund an exact 100-cent payment:
        # its 99-cent change would be below the consensus minimum. Give the
        # replay attempt a fresh exact input so this test reaches the durable
        # Capacity-v5 nonce firewall instead of failing in local coin selection.
        # Restricted capabilities intentionally use their payment nonce as the
        # pre-intent client nonce; reuse is therefore rejected before the
        # capability or payment intent is disclosed again.
        replay_funding = wallet.senddigidollar(client_address, 100)
        assert_equal(len(replay_funding["txid"]), 64)
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        self.sync_blocks()
        assert_equal(client.getdigidollarbalance()["total"], 299)
        replay_options = {
            "fee_mode": "paymaster",
            "request_id": "550e8400-e29b-41d4-a716-446655440104",
            "maximum_paymaster_fee_cents": 0,
            "privacy": "standard",
            "selection": "lowest_total_cost",
        }
        replay_options.update(restricted_options)

        def replay_client_send():
            return client.senddigidollar(
                recipient, restricted_amount, "", 0, None, "cents", replay_options)

        replay_request = replay_client_send()
        assert_equal(replay_request["status"], "pending")
        self.wait_until(lambda: replay_client_send()["queued"])
        assert_raises_rpc_error(
            -4,
            "PAYMASTER_CAPACITY_REQUEST_CONFLICT",
            wallet.processpaymasterrequests,
        )
        assert_equal(wallet.stoppaymaster()["running"], False)
        persisted_sequence = wallet.getpaymasterinfo()["announcement_sequence"]
        # Policy and liquidity refreshes may each consume another valid
        # monotonic sequence after the public Sponsored restart. The security
        # invariant is that the durable value never moves backwards; the
        # restart assertion below then proves this exact value is recovered.
        assert persisted_sequence >= sponsored_started["announcement_sequence"]

        self.log.info("A locked wallet stays enabled but cannot become ready")
        wallet.encryptwallet("paymaster test passphrase")
        locked = wallet.getpaymasterinfo()
        assert_equal(locked["enabled"], True)
        assert_equal(locked["wallet_locked"], True)
        assert "PAYMASTER_WALLET_LOCKED" in locked["readiness_errors"]
        assert_raises_rpc_error(
            -13,
            "PAYMASTER_WALLET_LOCKED",
            wallet.createpaymasteridentity,
            "Ignored Replacement",
        )
        autostart_settings = wallet.setpaymasterruntimesettings({
            "operation_mode": "automatic",
            "autostart": True,
        })
        assert_equal(autostart_settings["operation_mode"], "automatic")
        assert_equal(autostart_settings["autostart"], True)
        assert_equal(autostart_settings["running"], False)

        self.log.info(
            "Provider configuration and locked-wallet autostart survive restart")
        self.restart_node(0)
        self.connect_nodes(0, 1)
        wallet = self.nodes[0].get_wallet_rpc("paymaster")
        recovered = wallet.getpaymasterinfo()
        assert_equal(recovered["enabled"], True)
        assert_equal(recovered["running"], False)
        assert_equal(recovered["provider_id"], identity["provider_id"])
        assert_equal(recovered["policy"], persisted_policy)
        assert_equal(recovered["wallet_locked"], True)
        assert_equal(recovered["operation_mode"], "automatic")
        assert_equal(recovered["autostart"], True)
        assert_equal(recovered["service_state"], "waiting_for_unlock")
        assert_equal(recovered["announcement_sequence"], persisted_sequence)
        recovered_safety = wallet.getpaymastersafetystatus()
        assert_equal(recovered_safety["configured"], True)
        assert_equal(recovered_safety["restricted_sponsored"]
                     ["reserved_network_fee_satoshis"], 0)
        assert_equal(recovered_safety["restricted_sponsored"]
                     ["spent_network_fee_last_day_satoshis"], 10000000)
        assert_equal(recovered_safety["restricted_sponsored"]
                     ["completed_last_day"], 1)

        self.log.info(
            "Unlock resumes autostart without storing a passphrase")
        wallet.walletpassphrase("paymaster test passphrase", 60)
        def automatic_service_active():
            status = wallet.getpaymasterinfo()
            return (status["running"] and
                    status["service_state"] == "active")

        self.wait_until(automatic_service_active)
        resumed = wallet.getpaymasterinfo()
        assert_equal(resumed["running"], True)
        assert_equal(resumed["operation_mode"], "automatic")

        # Keep the unload/reload assertion deterministic: disabling autostart
        # while running does not stop the current ephemeral runtime, but the
        # subsequently reloaded locked wallet must remain offline.
        no_autostart = wallet.setpaymasterruntimesettings({
            "autostart": False,
        })
        assert_equal(no_autostart["operation_mode"], "automatic")
        assert_equal(no_autostart["autostart"], False)
        assert_equal(no_autostart["running"], True)

        self.log.info("Unloading a running provider stops only its runtime state")
        self.nodes[0].unloadwallet("paymaster", False)
        assert_raises_rpc_error(
            -18,
            "Requested wallet does not exist or is not loaded",
            self.nodes[0].get_wallet_rpc("paymaster").getpaymasterinfo,
        )

        self.nodes[0].loadwallet("paymaster")
        wallet = self.nodes[0].get_wallet_rpc("paymaster")
        reloaded = wallet.getpaymasterinfo()
        assert_equal(reloaded["enabled"], True)
        assert_equal(reloaded["running"], False)
        assert_equal(reloaded["provider_id"], identity["provider_id"])
        assert_equal(reloaded["policy"], persisted_policy)
        assert_equal(reloaded["wallet_locked"], True)
        assert_equal(reloaded["autostart"], False)
        assert_equal(reloaded["service_state"], "stopped")

        self.log.info("Verify production logs omit Paymaster payment artifacts")
        debug_logs = "\n".join(
            test_node.debug_log_path.read_text(
                encoding="utf-8", errors="replace")
            for test_node in self.nodes
        )
        sensitive_artifacts = {
            "client DigiDollar address": client_address,
            "recipient DigiDollar address": recipient,
            "high-privacy request ID": high_privacy_request_id,
            "multi-attempt request ID": multiple_attempt_request_id,
            "user-paid request ID": "550e8400-e29b-41d4-a716-446655440101",
            "second user-paid request ID": second_user_paid_request_id,
            "public sponsorship request ID": "550e8400-e29b-41d4-a716-446655440102",
            "restricted sponsorship request ID": "550e8400-e29b-41d4-a716-446655440103",
            "capability replay request ID": "550e8400-e29b-41d4-a716-446655440104",
            "recovery request ID": recovery_request_id,
            "recovery session ID": interrupted_session_id,
            "restricted service descriptor": descriptor["descriptor"],
            "restricted sponsorship capability": capability,
            "restricted payment nonce": payment_nonce,
            "recovery transaction": recovery_hex,
            "sponsor secret": sponsor_secret.hex(),
            "wallet passphrase": "paymaster test passphrase",
        }
        for index, psbt in enumerate(set(observed_paymaster_psbts)):
            sensitive_artifacts[f"Paymaster PSBT {index}"] = psbt
        for artifact_name, artifact in sensitive_artifacts.items():
            assert artifact not in debug_logs, \
                f"{artifact_name} leaked into a node debug log"

        if self.pre_paymaster_digibyted:
            old_node = self.nodes[2]
            assert "pmannounce" not in captured_message_types(
                old_node.chain_path, "recv")
            assert "sendpmasters" not in captured_message_types(
                old_node.chain_path, "sent")
            assert "pmannounce" not in captured_message_types(
                old_node.chain_path, "sent")


if __name__ == "__main__":
    PaymasterProviderRPCTest().main()
