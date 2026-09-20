#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a v9.26.5-only bridge between current Paymaster peers.

The initial topology is current provider <-> official v9.26.5 <-> current
client, with no direct current-current connection. Ordinary DGB and DigiDollar
transactions must relay through and be mined by the old node, while Paymaster
gossip must stop there and the client must fail closed. Adding a direct link
between current nodes must restore discovery and deterministic provider
selection without changing the old node.
"""

from io import BytesIO
import os
from pathlib import Path

from test_framework.paymaster import (
    confirm_pool_preparation,
    default_liquidity_policy,
    paymaster_node_args,
    provider_safety_policy,
)
from test_framework.test_framework import DigiByteTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


V9_26_5_CLIENT_VERSION = 92605
ORACLE_PRICE_MICRO_USD = 500_000
OLD_NODE_ARGS = [
    "-capturemessages=1",
    "-digidollar=1",
    "-txindex=1",
    "-dandelion=0",
    "-fallbackfee=0.1",
    "-v2transport=1",
]


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
                commands.append(
                    encoded.read(12).rstrip(b"\x00").decode("ascii"))
                payload_size = int.from_bytes(encoded.read(4), "little")
                assert len(stream.read(payload_size)) == payload_size
    return commands


class PaymasterV9BridgeTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(provider_node=0),
            OLD_NODE_ARGS.copy(),
            paymaster_node_args(),
        ]
        self.wallet_names = []

        self.v9_binary_env = None
        self.v9_binary_override = None
        for variable in ("V9_26_5_DIGIBYTED", "PRE_PAYMASTER_DIGIBYTED"):
            if value := os.getenv(variable):
                self.v9_binary_env = variable
                self.v9_binary_override = value
                break
        self.v9_binary = None

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def _locate_v9_binary(self):
        if self.v9_binary_override:
            return Path(self.v9_binary_override).expanduser()
        exeext = self.config["environment"]["EXEEXT"]
        return Path(
            self.options.previous_releases_path,
            "v9.26.5",
            "bin",
            f"digibyted{exeext}",
        )

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()
        binary = self._locate_v9_binary()
        if not binary.is_file():
            if self.v9_binary_env:
                raise AssertionError(
                    f"{self.v9_binary_env} does not name a file: {binary}")
            raise SkipTest(
                "DigiByte Core v9.26.5 is required at "
                f"{binary}. Set V9_26_5_DIGIBYTED or install the previous "
                "release under the functional-test releases directory."
            )
        self.v9_binary = str(binary)

    def setup_nodes(self):
        assert self.v9_binary is not None
        self.add_nodes(
            self.num_nodes,
            extra_args=self.extra_args,
            binary=[
                self.options.digibyted,
                self.v9_binary,
                self.options.digibyted,
            ],
            binary_cli=[self.options.digibytecli] * self.num_nodes,
            versions=[None, 260000, None],
        )
        self.start_nodes()

    @staticmethod
    def provider_policy():
        return {
            "funding_models": ["user_paid"],
            "sponsorship_scope": "public",
            "fee_rate_bps": 50,
            "min_amount_cents": 100,
            "max_amount_cents": 100_000,
            "quote_ttl": 60,
            "maximum_network_fee_dgb_satoshis": 20_000_000,
        }

    def run_test(self):
        provider_node, old_node, client_node = self.nodes
        assert_equal(
            old_node.getnetworkinfo()["version"], V9_26_5_CLIENT_VERSION)
        assert_raises_rpc_error(
            -32601, "Method not found", old_node.getpaymasterinfo)

        self.log.info("Create wallets on the current endpoints and old bridge")
        provider_node.createwallet(
            wallet_name="bridge-provider", descriptors=True,
            load_on_startup=True)
        old_node.createwallet(
            wallet_name="bridge-v9-miner", descriptors=True,
            load_on_startup=True)
        client_node.createwallet(
            wallet_name="bridge-relay-wallet", descriptors=True,
            load_on_startup=True)
        client_node.createwallet(
            wallet_name="bridge-paymaster-client", descriptors=True,
            load_on_startup=True)
        client_node.createwallet(
            wallet_name="bridge-recipient", descriptors=True,
            load_on_startup=True)

        provider = provider_node.get_wallet_rpc("bridge-provider")
        old_miner = old_node.get_wallet_rpc("bridge-v9-miner")
        relay_wallet = client_node.get_wallet_rpc("bridge-relay-wallet")
        client = client_node.get_wallet_rpc("bridge-paymaster-client")
        recipient = client_node.get_wallet_rpc("bridge-recipient")
        provider_cli = provider_node.cli("-rpcwallet=bridge-provider")

        self.log.info("Relay and confirm ordinary DGB through v9.26.5")
        self.generatetoaddress(
            provider_node, 110,
            provider.getnewaddress(address_type="bech32m"))
        dgb_txid = provider.sendtoaddress(relay_wallet.getnewaddress(), 5)
        self.sync_mempools()
        assert dgb_txid in old_node.getrawmempool()
        assert_equal(
            old_node.getrawtransaction(dgb_txid),
            provider_node.getrawtransaction(dgb_txid))
        dgb_block = self.generatetoaddress(
            old_node, 1, old_miner.getnewaddress())[0]
        assert dgb_txid in old_node.getblock(dgb_block)["tx"]
        assert_equal(relay_wallet.getbalance(), 5)

        self.log.info("Relay minting and an ordinary DD transfer through v9.26.5")
        for node in self.nodes:
            node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        mint = provider.mintdigidollar(5_000, 0)
        self.sync_mempools()
        assert mint["txid"] in old_node.getrawmempool()
        mint_block = self.generatetoaddress(
            old_node, 1, old_miner.getnewaddress())[0]
        assert mint["txid"] in old_node.getblock(mint_block)["tx"]

        dd_transfer = provider.senddigidollar(
            client.getdigidollaraddress(), 1_500)
        dd_txid = dd_transfer["txid"]
        self.sync_mempools()
        assert dd_txid in old_node.getrawmempool()
        assert_equal(
            old_node.getrawtransaction(dd_txid),
            client_node.getrawtransaction(dd_txid))
        dd_block = self.generatetoaddress(
            old_node, 1, old_miner.getnewaddress())[0]
        assert dd_txid in old_node.getblock(dd_block)["tx"]
        assert_equal(client.getdigidollarbalance()["total"], 1_500)
        assert_equal(client.getbalance(), 0)

        self.log.info("Configure and announce the current provider")
        identity = provider.createpaymasteridentity("Provider behind v9 bridge")
        provider_cli.setpaymasterpolicy(self.provider_policy())
        provider_cli.setpaymastersafetypolicy(
            provider_safety_policy(["user_paid"]))
        provider_cli.setpaymasterenabled(True)
        provider.setpaymasterruntimesettings({
            "operation_mode": "manual",
            "autostart": False,
        })
        pool_targets = {
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 1,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 1,
        }
        pool_preview = provider_cli.preparepaymasterpool(pool_targets)
        pool_targets["execute"] = True
        pool_targets["plan_id"] = pool_preview["plan_id"]
        prepared = provider_cli.preparepaymasterpool(pool_targets)
        assert_equal(prepared["executed"], True)
        confirm_pool_preparation(self, provider_node, provider_cli, pool_targets, prepared)
        provider.setpaymasterliquiditypolicy(
            default_liquidity_policy(carriers=True))
        client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10_000,
        })
        started = provider.startpaymaster()
        assert_equal(started["ready"], True)
        provider_id = identity["provider_id"]
        assert provider_id in {
            entry["provider_id"] for entry in provider_node.listpaymasters()
        }

        self.log.info("The old bridge receives but does not relay Paymaster gossip")
        # v9.26.5 does not expose unknown message commands through
        # getpeerinfo.bytesrecv_per_msg. Its raw message capture still proves
        # that the provider sent PMANNOUNCE to the old bridge, while the sent
        # capture proves that the bridge never forwarded it.
        self.wait_until(lambda: "pmannounce" in captured_message_types(
            old_node.chain_path, "recv"))
        assert "pmannounce" not in captured_message_types(
            old_node.chain_path, "sent")
        assert provider_id not in {
            entry["provider_id"] for entry in client_node.listpaymasters()
        }
        assert_equal(client.getpaymasteroffers(1_000), [])

        isolated_options = {
            "fee_mode": "paymaster",
            "request_id": "550e8400-e29b-41d4-a716-446655442301",
            "maximum_paymaster_fee_cents": 100,
            "privacy": "standard",
            "selection": "lowest_total_cost",
            "maximum_provider_attempts": 1,
        }
        assert_raises_rpc_error(
            -4, "PAYMASTER_NO_ELIGIBLE_OFFER",
            client.senddigidollar,
            recipient.getdigidollaraddress(), 1_000, "", 0, None,
            "cents", isolated_options)

        self.log.info("A direct current-current link restores discovery")
        self.connect_nodes(2, 0)
        self.wait_until(lambda: provider_id in {
            entry["provider_id"] for entry in client_node.listpaymasters()
        })
        offers = [
            offer for offer in client.getpaymasteroffers(1_000)
            if offer["provider_id"] == provider_id
        ]
        assert_equal(len(offers), 1)
        assert_equal(offers[0]["fee_rate_bps"], 50)

        direct_request_id = "550e8400-e29b-41d4-a716-446655442302"
        direct_options = dict(isolated_options)
        direct_options["request_id"] = direct_request_id
        pending = client.senddigidollar(
            recipient.getdigidollaraddress(), 1_000, "", 0, None,
            "cents", direct_options)
        assert_equal(pending["provider_id"], provider_id)
        assert_equal(pending["status"], "pending")
        direct_inputs = pending["reserved_user_inputs"]
        assert direct_inputs
        abandoned = client.resolvepaymastersession(
            {"request_id": direct_request_id}, "abandon_unsigned")
        assert_equal(abandoned["session"]["session_state"], "FAILED")
        assert_equal(
            abandoned["session"]["reserved_user_inputs"], direct_inputs)
        assert_equal(
            client.getpaymasterclientsafetystatus()["active_reservations"],
            0)


if __name__ == "__main__":
    PaymasterV9BridgeTest().main()
