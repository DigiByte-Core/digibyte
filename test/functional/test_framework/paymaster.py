#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reusable fixtures and value oracles for Paymaster functional tests.

This module deliberately contains no test cases.  It keeps provider setup and
the security-sensitive accounting assertions identical across the focused RPC,
lifecycle, and end-to-end tests without hiding protocol progress from them.
"""

from dataclasses import dataclass
import time

from test_framework.util import assert_equal, p2p_port


def paymaster_node_args(provider_node=None):
    """Return the common fail-closed node configuration used by tests.

    ``provider_node`` is the node index whose reachable endpoint is announced.
    Omitting it creates a client-only node with Paymaster support enabled.
    """
    args = [
        "-dandelion=0",
        "-debug=net",
        "-debug=rpc",
        "-digidollar=1",
        "-paymaster=1",
        "-prune=0",
        "-txindex=1",
        "-v2transport=1",
        "-onion=127.0.0.1:9050",
        "-proxyrandomize=1",
    ]
    if provider_node is not None:
        args.append(f"-paymasterendpoint=127.0.0.1:{p2p_port(provider_node)}")
    return args


def provider_safety_policy(funding_models, sponsorship_scope="public"):
    """Return explicit finite budgets for every enabled funding class."""
    disabled = {
        "maximum_network_fee_per_transaction_satoshis": 0,
        "maximum_reserved_network_fee_satoshis": 0,
        "maximum_network_fee_per_hour_satoshis": 0,
        "maximum_network_fee_per_day_satoshis": 0,
        "maximum_completed_per_hour": 0,
        "maximum_completed_per_day": 0,
    }
    enabled = {
        "maximum_network_fee_per_transaction_satoshis": 20_000_000,
        "maximum_reserved_network_fee_satoshis": 400_000_000,
        "maximum_network_fee_per_hour_satoshis": 800_000_000,
        "maximum_network_fee_per_day_satoshis": 4_000_000_000,
        "maximum_completed_per_hour": 100,
        "maximum_completed_per_day": 1000,
    }
    return {
        "user_paid": (enabled.copy() if "user_paid" in funding_models
                      else disabled.copy()),
        "public_sponsored": (
            enabled.copy() if "sponsored" in funding_models and
            sponsorship_scope == "public" else disabled.copy()),
        "restricted_sponsored": (
            enabled.copy() if "sponsored" in funding_models and
            sponsorship_scope == "restricted" else disabled.copy()),
        "maximum_active_quotes_total": 64,
        "maximum_active_quotes_per_netgroup": 16,
        "maximum_active_quotes_per_recipient": 16,
        "maximum_quote_requests_per_netgroup_per_minute": 120,
    }


def default_provider_policy(funding_models=None):
    """Return a deterministic policy suitable for contract-level tests."""
    models = ["user_paid"] if funding_models is None else funding_models
    return {
        "funding_models": models,
        "sponsorship_scope": "public",
        "fee_rate_bps": 50 if "user_paid" in models else 0,
        "min_amount_cents": 100,
        "max_amount_cents": 100_000,
        "quote_ttl": 60,
        "maximum_network_fee_dgb_satoshis": 20_000_000,
    }


def default_liquidity_policy(carriers=True):
    """Return finite maintenance limits and the minimum production pool shape."""
    return {
        "automatic_replenishment": True,
        "paid_maintenance_approved": True,
        "target_admission_dgb": 3,
        "target_operational_dgb": 1,
        "target_admission_carriers": 3 if carriers else 0,
        "target_operational_carriers": 1 if carriers else 0,
        "maximum_maintenance_fee_per_transaction_satoshis": 20_000_000,
        "maximum_maintenance_fee_per_hour_satoshis": 100_000_000,
        "maximum_maintenance_fee_per_day_satoshis": 400_000_000,
    }


@dataclass(frozen=True)
class PaymasterValueSnapshot:
    """Balances and durable ledgers surrounding a value-moving operation."""
    client_dd: int
    provider_dd: int
    recipient_dd: int
    provider_dgb: float
    pool: dict
    provider_budget: dict
    client_budget: dict
    mempool: frozenset


def value_snapshot(provider_node, provider_wallet, client_wallet,
                   recipient_wallet):
    """Capture all value domains that a Paymaster operation may mutate."""
    return PaymasterValueSnapshot(
        client_dd=client_wallet.getdigidollarbalance()["total"],
        provider_dd=provider_wallet.getdigidollarbalance()["total"],
        recipient_dd=recipient_wallet.getdigidollarbalance()["total"],
        provider_dgb=provider_wallet.getbalance(),
        pool=provider_wallet.getpaymasterpoolinfo(),
        provider_budget=provider_wallet.getpaymastersafetystatus(),
        client_budget=client_wallet.getpaymasterclientsafetystatus(),
        mempool=frozenset(provider_node.getrawmempool()),
    )


def assert_snapshot_equal(before, after):
    """Assert that a preview or rejected operation moved no value or budget."""
    assert_equal(after.client_dd, before.client_dd)
    assert_equal(after.provider_dd, before.provider_dd)
    assert_equal(after.recipient_dd, before.recipient_dd)
    assert_equal(after.provider_dgb, before.provider_dgb)
    assert_equal(after.pool, before.pool)
    assert_equal(after.provider_budget, before.provider_budget)
    assert_equal(after.client_budget, before.client_budget)
    assert_equal(after.mempool, before.mempool)


class PaymasterFunctionalHarness:
    """Small shared setup/protocol driver for focused functional tests."""

    def __init__(self, test, provider_wallet="provider", client_wallet="client"):
        self.test = test
        self.provider_node = test.nodes[0]
        self.client_node = test.nodes[1]
        self.provider_wallet_name = provider_wallet
        self.client_wallet_name = client_wallet
        self.provider = None
        self.provider_cli = None
        self.client = None
        self.identity = None
        self.policy = None

    def create_wallets(self):
        """Create isolated descriptor wallets and return their RPC handles."""
        self.provider_node.createwallet(
            wallet_name=self.provider_wallet_name,
            descriptors=True,
            load_on_startup=True,
        )
        self.client_node.createwallet(
            wallet_name=self.client_wallet_name,
            descriptors=True,
            load_on_startup=True,
        )
        self.provider = self.provider_node.get_wallet_rpc(self.provider_wallet_name)
        self.provider_cli = self.provider_node.cli(
            f"-rpcwallet={self.provider_wallet_name}")
        self.client = self.client_node.get_wallet_rpc(self.client_wallet_name)
        return self.provider, self.client

    def fund_and_configure(self, *, funding_models=None, carrier_cents=800,
                           operation_mode="manual", fee_rate_bps=None):
        """Create identity, finite policies, confirmed pool and client limits."""
        if self.provider is None:
            self.create_wallets()
        self.test.generatetoaddress(
            self.provider_node, 110,
            self.provider.getnewaddress(address_type="bech32m"))
        self.provider_node.setmockoracleprice(500_000)
        self.client_node.setmockoracleprice(500_000)

        self.identity = self.provider.createpaymasteridentity(
            "Focused RPC Test Provider")
        self.policy = default_provider_policy(funding_models)
        if fee_rate_bps is not None:
            self.policy["fee_rate_bps"] = fee_rate_bps
        self.provider_cli.setpaymasterpolicy(self.policy)
        self.provider_cli.setpaymastersafetypolicy(provider_safety_policy(
            self.policy["funding_models"], self.policy["sponsorship_scope"]))
        self.provider_cli.setpaymasterenabled(True)

        carriers = "user_paid" in self.policy["funding_models"]
        if carriers:
            minted = self.provider.mintdigidollar(carrier_cents, 0)
            assert_equal(minted["dd_minted"], carrier_cents)
            self.test.generatetoaddress(
                self.provider_node, 1, self.provider.getnewaddress())

        targets = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 1,
            "admission_carrier_slots": 3 if carriers else 0,
            "operational_carrier_slots": 1 if carriers else 0,
        }
        preview = self.provider_cli.preparepaymasterpool(targets)
        targets["execute"] = True
        targets["plan_id"] = preview["plan_id"]
        prepared = self.provider_cli.preparepaymasterpool(targets)
        assert_equal(prepared["executed"], True)
        self.test.generatetoaddress(
            self.provider_node, 1, self.provider.getnewaddress())
        self.provider_cli.setpaymasterliquiditypolicy(
            default_liquidity_policy(carriers))
        self.client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10_000,
        })
        self.provider.setpaymasterruntimesettings({
            "operation_mode": operation_mode,
            "autostart": False,
        })
        return self.provider.getpaymasterinfo()

    def fund_client_dd(self, amount_cents):
        """Fund client DD through the ordinary path and confirm it."""
        address = self.client.getdigidollaraddress()
        result = self.provider.senddigidollar(address, amount_cents)
        self.test.generatetoaddress(
            self.provider_node, 1, self.provider.getnewaddress())
        self.test.sync_blocks()
        assert_equal(self.client.getdigidollarbalance()["total"], amount_cents)
        return result["txid"]

    def wait_for_quote(self, request_id, recipient, amount_cents,
                       fee_cap_cents=100):
        """Drive Capacity then Intent until the manual provider creates a quote."""
        options = {
            "fee_mode": "paymaster",
            "request_id": request_id,
            "maximum_paymaster_fee_cents": fee_cap_cents,
            "privacy": "standard",
            "selection": "lowest_total_cost",
            "maximum_provider_attempts": 1,
        }

        def send():
            return self.client.senddigidollar(
                recipient, amount_cents, "", 0, None, "cents", options)

        initial = send()
        assert_equal(initial["request_id"], request_id)
        capacity_seen = False
        quote = {}
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            send()
            response = self.provider.processpaymasterrequests()
            if response.get("processed", False):
                if response.get("message_type") == "capacity":
                    capacity_seen = True
                elif response.get("message_type") == "quote":
                    quote = response
                    break
            time.sleep(0.25)
        if not quote:
            raise AssertionError("manual Paymaster quote was not produced")
        assert_equal(capacity_seen, True)
        return options, quote, send

    def authorize_quote(self, options, send):
        """Perform the mandatory second client authorization and return its PSBT."""
        deadline = time.monotonic() + 60
        prepared = None
        while time.monotonic() < deadline:
            candidate = send()
            if candidate.get("authorization_required", False):
                prepared = candidate
                break
            # Exact quote responses may be replayed over a replacement direct
            # connection before the client observes them.
            replay = self.provider.processpaymasterrequests()
            if replay.get("processed", False):
                assert_equal(replay["queued"], True)
            time.sleep(0.25)
        if prepared is None:
            raise AssertionError("client authorization was not requested")
        options["authorization_commitment"] = prepared[
            "authorization_commitment"]
        authorized = send()
        assert_equal(authorized["authorization_accepted"], True)
        assert_equal(authorized["session_state"], "PENDING_PROVIDER")
        if "psbt" not in authorized:
            # The exact retry returns the durable signed artifact when the
            # initial response was queued before its RPC result was observed.
            authorized = send()
        if "psbt" not in authorized:
            raise AssertionError("authorized Paymaster PSBT is unavailable")
        return authorized
