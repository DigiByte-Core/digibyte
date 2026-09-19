#!/usr/bin/env python3
# Copyright (c) 2025 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test DigiDollar position reconstruction during blockchain rescan.

This test verifies that DigiDollar (DD) positions can be correctly reconstructed
from blockchain data during a wallet rescan. This is critical for:
- Wallet recovery scenarios
- Importing wallets from backup
- Syncing wallets after being offline

Test coverage:
1. Full rescan reconstructs all DD positions correctly
2. Partial rescan (from specific height) finds positions in scanned range
3. Rescan after position data loss reconstructs from blockchain
4. DD balance accuracy is maintained after rescan
5. Descriptor restore preserves per-output DD receive history
6. Rescan progress is properly reported
"""

from decimal import Decimal
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)
import time

ORACLE_PRICE_MICRO_USD = 500000


class DigiDollarRescanTest(DigiByteTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Enable DigiDollar, disable Dandelion for testing
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-debug=digidollar", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-debug=digidollar", "-dandelion=0"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def setup_digidollar_environment(self):
        """Setup test environment with mature coins and oracle price."""
        self.log.info("Setting up DigiDollar test environment...")

        # Mine blocks past coinbase maturity (DigiByte uses COINBASE_MATURITY = 8)
        # Mine extra blocks to ensure DD activation and have funds
        self.generate(self.nodes[0], 110)
        self.sync_all()

        # Set mock oracle price for DD minting
        # Oracle price is in micro-USD: 500,000 = $0.50 per DGB
        self.refresh_oracle_quotes()

        # Verify we have funds
        balance = self.nodes[0].getbalance()
        assert_greater_than(balance, 100000)
        self.log.info(f"Initial DGB balance: {balance}")

    def refresh_oracle_quotes(self, price=ORACLE_PRICE_MICRO_USD):
        for node in self.nodes:
            result = node.setmockoracleprice(price)
            assert_equal(result["price_micro_usd"], price)

    def run_test(self):
        self.log.info("=== Starting DigiDollar Rescan Tests ===")

        # Setup environment
        self.setup_digidollar_environment()

        # Run test scenarios
        self.test_rescan_reconstructs_positions()
        self.test_rescan_partial_range()
        self.test_rescan_after_position_removal()
        self.test_rescan_balance_accuracy()
        self.test_descriptor_restore_preserves_receive_history()
        self.test_bounded_rescan_skips_full_dd_scan()
        self.test_rescan_progress_reporting()

        self.log.info("=== All DigiDollar Rescan Tests Passed! ===")

    def test_rescan_reconstructs_positions(self):
        """Test that blockchain rescan reconstructs all DD positions correctly."""
        self.log.info("Test 1: Testing rescan reconstructs positions...")

        # Create multiple DD positions at different block heights
        positions_created = []

        # Position 1: 10000 cents ($100), tier 4 (365 days)
        self.log.info("  Creating position 1 at current height...")
        try:
            self.refresh_oracle_quotes()
            mint1 = self.nodes[0].mintdigidollar(10000, 4)
            height1 = self.nodes[0].getblockcount()
            self.generate(self.nodes[0], 10)
            self.sync_all()
            positions_created.append({
                'txid': mint1['txid'],
                'amount': 10000,
                'tier': 4,
                'height': height1,
            })
            self.log.info(f"  Position 1 created: txid={mint1['txid'][:16]}...")
        except Exception as e:
            self.log.error(f"  Failed to create position 1: {e}")
            self.log.info("  Skipping test - DigiDollar minting not available")
            return

        # Position 2: 5000 cents ($50), tier 2 (90 days)
        self.log.info("  Creating position 2...")
        self.refresh_oracle_quotes()
        mint2 = self.nodes[0].mintdigidollar(5000, 2)
        height2 = self.nodes[0].getblockcount()
        self.generate(self.nodes[0], 10)
        self.sync_all()
        positions_created.append({
            'txid': mint2['txid'],
            'amount': 5000,
            'tier': 2,
            'height': height2,
        })
        self.log.info(f"  Position 2 created: txid={mint2['txid'][:16]}...")

        # Position 3: 20000 cents ($200), tier 3 (180 days)
        self.log.info("  Creating position 3...")
        self.refresh_oracle_quotes()
        mint3 = self.nodes[0].mintdigidollar(20000, 3)
        height3 = self.nodes[0].getblockcount()
        self.generate(self.nodes[0], 5)
        self.sync_all()
        positions_created.append({
            'txid': mint3['txid'],
            'amount': 20000,
            'tier': 3,
            'height': height3,
        })
        self.log.info(f"  Position 3 created: txid={mint3['txid'][:16]}...")

        # Record positions before rescan
        positions_before = self.nodes[0].listdigidollarpositions()
        balance_before = self.nodes[0].getdigidollarbalance()

        self.log.info(f"  Positions before rescan: {len(positions_before)}")
        self.log.info(f"  Balance before rescan: {balance_before}")

        # Perform full blockchain rescan
        self.log.info("  Performing full blockchain rescan...")
        rescan_result = self.nodes[0].rescanblockchain()
        self.log.info(f"  Rescan completed: {rescan_result}")

        # Verify positions after rescan
        positions_after = self.nodes[0].listdigidollarpositions()
        balance_after = self.nodes[0].getdigidollarbalance()

        self.log.info(f"  Positions after rescan: {len(positions_after)}")
        self.log.info(f"  Balance after rescan: {balance_after}")

        assert len(positions_after) == len(positions_before), \
            f"Position count mismatch: {len(positions_after)} vs {len(positions_before)}"

        # Verify balance matches
        balance_before_total = balance_before['total'] if isinstance(balance_before, dict) else balance_before
        balance_after_total = balance_after['total'] if isinstance(balance_after, dict) else balance_after
        assert_equal(balance_after_total, balance_before_total)

        # Verify each created position exists after rescan
        for pos_data in positions_created:
            found = False
            for pos in positions_after:
                pos_amount = pos.get('dd_minted', pos.get('amount', 0))
                if pos_amount == pos_data['amount']:
                    found = True
                    break
            assert found, f"Position with amount {pos_data['amount']} not found after rescan"

        self.log.info("  SUCCESS: Full rescan reconstructed all positions correctly")

    def test_rescan_partial_range(self):
        """Test partial blockchain rescan from specific start_height."""
        self.log.info("Test 2: Testing partial rescan from specific height...")

        # Record current height before creating new positions
        initial_height = self.nodes[0].getblockcount()
        self.log.info(f"  Initial height: {initial_height}")

        # Get positions before creating new ones
        positions_initial = self.nodes[0].listdigidollarpositions()
        self.log.info(f"  Positions at start: {len(positions_initial)}")

        # Create new positions after initial_height
        self.log.info("  Creating new positions after initial height...")

        self.refresh_oracle_quotes()
        mint1 = self.nodes[0].mintdigidollar(8000, 1)  # tier 1 (30 days)
        self.generate(self.nodes[0], 5)

        self.refresh_oracle_quotes()
        mint2 = self.nodes[0].mintdigidollar(12000, 2)  # tier 2 (90 days)
        self.generate(self.nodes[0], 5)
        self.sync_all()

        # Record positions after minting
        positions_after_mint = self.nodes[0].listdigidollarpositions()
        self.log.info(f"  Positions after minting: {len(positions_after_mint)}")

        new_position_count = len(positions_after_mint) - len(positions_initial)
        self.log.info(f"  New positions created: {new_position_count}")

        # Perform partial rescan from initial_height
        self.log.info(f"  Performing partial rescan from height {initial_height}...")
        rescan_result = self.nodes[0].rescanblockchain(initial_height)
        self.log.info(f"  Partial rescan result: {rescan_result}")

        # Verify start and stop heights in result
        if 'start_height' in rescan_result:
            assert_greater_than_or_equal(rescan_result['start_height'], 0)
        if 'stop_height' in rescan_result:
            current_height = self.nodes[0].getblockcount()
            assert_equal(rescan_result['stop_height'], current_height)

        # Verify positions still exist
        positions_after_rescan = self.nodes[0].listdigidollarpositions()
        self.log.info(f"  Positions after partial rescan: {len(positions_after_rescan)}")

        assert len(positions_after_rescan) == len(positions_after_mint), "Partial rescan lost positions"

        self.log.info("  SUCCESS: Partial rescan correctly found positions in scanned range")

    def test_rescan_after_position_removal(self):
        """Test rescan reconstructs positions after simulated data loss."""
        self.log.info("Test 3: Testing rescan after position removal...")

        # Create a position
        self.log.info("  Creating position for recovery test...")
        mint_amount = 15000  # $150
        dca_tier = 3  # 180 days

        self.refresh_oracle_quotes()
        mint_result = self.nodes[0].mintdigidollar(mint_amount, dca_tier)
        mint_txid = mint_result['txid']
        self.generate(self.nodes[0], 3)
        self.sync_all()

        self.log.info(f"  Created position: txid={mint_txid[:16]}...")

        # Record state before "data loss"
        positions_before = self.nodes[0].listdigidollarpositions()
        balance_before = self.nodes[0].getdigidollarbalance()

        self.log.info(f"  Positions before data loss: {len(positions_before)}")

        # Export wallet descriptors for reimport
        descriptors = self.nodes[0].listdescriptors(True)  # Include private keys
        self.log.info(f"  Exported {len(descriptors['descriptors'])} descriptors")

        # Create a new wallet to simulate data loss and recovery
        self.log.info("  Simulating data loss by creating new wallet...")
        self.nodes[0].createwallet(
            wallet_name="recovered_dd",
            disable_private_keys=False,
            blank=True,
            descriptors=True
        )

        recovered_wallet = self.nodes[0].get_wallet_rpc("recovered_dd")

        # Import descriptors into new wallet
        self.log.info("  Importing descriptors to new wallet...")
        import_descs = []
        for desc in descriptors['descriptors']:
            import_descs.append({
                'desc': desc['desc'],
                'timestamp': 0,  # Scan from genesis
                'active': desc.get('active', False),
                'internal': desc.get('internal', False),
            })

        import_result = recovered_wallet.importdescriptors(import_descs)
        success_count = sum(1 for r in import_result if r.get('success', False))
        self.log.info(f"  Imported {success_count}/{len(import_result)} descriptors")

        # The import should trigger a rescan, but let's be explicit
        self.log.info("  Performing explicit rescan on recovered wallet...")
        rescan_result = recovered_wallet.rescanblockchain()
        self.log.info(f"  Rescan completed: {rescan_result}")

        # Verify positions were reconstructed
        positions_recovered = recovered_wallet.listdigidollarpositions()
        balance_recovered = recovered_wallet.getdigidollarbalance()

        self.log.info(f"  Positions recovered: {len(positions_recovered)}")
        self.log.info(f"  Balance recovered: {balance_recovered}")

        assert len(positions_recovered) > 0, "No positions recovered after rescan"

        balance_recovered_total = balance_recovered['total'] if isinstance(balance_recovered, dict) else balance_recovered
        assert balance_recovered_total > 0, "No DD balance recovered after rescan"

        self.log.info("  SUCCESS: Positions reconstructed from blockchain after data loss")

    def test_rescan_balance_accuracy(self):
        """Test that DD balance remains accurate after rescan with transfers."""
        self.log.info("Test 4: Testing rescan balance accuracy with transfers...")

        self.nodes[0].unloadwallet("recovered_dd")

        # Get initial balance
        initial_balance = self.nodes[0].getdigidollarbalance()
        initial_total = initial_balance['total'] if isinstance(initial_balance, dict) else initial_balance
        self.log.info(f"  Initial DD balance: {initial_total}")

        # Create a position
        mint_amount = 25000  # $250
        self.refresh_oracle_quotes()
        mint_result = self.nodes[0].mintdigidollar(mint_amount, 2)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        # Transfer some DD to node 1
        transfer_amount = 10000  # $100
        receiver_addr = self.nodes[1].getdigidollaraddress()
        self.refresh_oracle_quotes()
        send_result = self.nodes[0].senddigidollar(receiver_addr, transfer_amount)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        self.log.info(f"  Transferred {transfer_amount} cents to node 1")

        # Record expected balances
        balance_node0_before = self.nodes[0].getdigidollarbalance()
        balance_node1_before = self.nodes[1].getdigidollarbalance()

        expected_node0 = balance_node0_before['total'] if isinstance(balance_node0_before, dict) else balance_node0_before
        expected_node1 = balance_node1_before['total'] if isinstance(balance_node1_before, dict) else balance_node1_before

        self.log.info(f"  Node 0 balance before rescan: {expected_node0}")
        self.log.info(f"  Node 1 balance before rescan: {expected_node1}")

        # Perform rescan on both nodes
        self.log.info("  Performing rescan on both nodes...")
        rescan0 = self.nodes[0].rescanblockchain()
        rescan1 = self.nodes[1].rescanblockchain()

        # Verify balances after rescan
        balance_node0_after = self.nodes[0].getdigidollarbalance()
        balance_node1_after = self.nodes[1].getdigidollarbalance()

        actual_node0 = balance_node0_after['total'] if isinstance(balance_node0_after, dict) else balance_node0_after
        actual_node1 = balance_node1_after['total'] if isinstance(balance_node1_after, dict) else balance_node1_after

        self.log.info(f"  Node 0 balance after rescan: {actual_node0}")
        self.log.info(f"  Node 1 balance after rescan: {actual_node1}")

        assert actual_node0 == expected_node0, f"Node 0 balance mismatch: {actual_node0} vs {expected_node0}"
        assert actual_node1 == expected_node1, f"Node 1 balance mismatch: {actual_node1} vs {expected_node1}"

        total_dd = actual_node0 + actual_node1
        expected_total = initial_total + mint_amount
        assert total_dd == expected_total, f"Total DD incorrect: {total_dd} vs {expected_total}"

        self.log.info("  SUCCESS: DD balance accurate after rescan with transfers")

    def test_descriptor_restore_preserves_receive_history(self):
        """Test descriptor restore preserves per-output DD receive history."""
        self.log.info("Test 5: Testing descriptor restore preserves DD receive history...")

        self.log.info("  Creating isolated source wallet for outgoing-change history")
        self.nodes[1].createwallet(
            wallet_name="dd_change_history_source",
            disable_private_keys=False,
            blank=False,
            descriptors=True,
        )
        change_source = self.nodes[1].get_wallet_rpc("dd_change_history_source")

        change_fund_addr = change_source.getnewaddress("", "bech32")
        self.nodes[0].sendtoaddress(change_fund_addr, 10000)
        for _ in range(25):
            self.nodes[0].sendtoaddress(change_source.getnewaddress("", "bech32"), 1)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        self.refresh_oracle_quotes()
        change_mint = change_source.mintdigidollar(10000, 2)
        assert "txid" in change_mint
        self.generate(self.nodes[1], 2)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: change_source.gettransaction(change_mint["txid"])["confirmations"] > 0)
        assert_equal(Decimal(change_source.getdigidollarbalance()["total"]), Decimal(10000))

        external_addr = self.nodes[0].getdigidollaraddress("restore-change-destination")
        outgoing = change_source.senddigidollar(external_addr, 1000)
        outgoing_txid = outgoing["txid"]
        self.generate(self.nodes[1], 2)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: change_source.gettransaction(outgoing_txid)["confirmations"] > 0)
        assert_equal(Decimal(change_source.getdigidollarbalance()["total"]), Decimal(9000))

        original_change_receives = [
            tx for tx in change_source.listdigidollartxs(100, 0, "", "receive")
            if tx["txid"] == outgoing_txid
        ]
        assert_equal(len(original_change_receives), 0)

        change_descriptors = change_source.listdescriptors(True)["descriptors"]
        self.nodes[1].createwallet(
            wallet_name="dd_change_history_restored",
            disable_private_keys=False,
            blank=True,
            descriptors=True,
        )
        restored_change = self.nodes[1].get_wallet_rpc("dd_change_history_restored")

        change_imports = []
        for desc in change_descriptors:
            req = {
                "desc": desc["desc"],
                "timestamp": 0,
                "active": desc.get("active", False),
                "internal": desc.get("internal", False),
            }
            if "range" in desc:
                req["range"] = desc["range"]
            change_imports.append(req)

        change_import_result = restored_change.importdescriptors(change_imports)
        assert_equal(
            sum(1 for item in change_import_result if item.get("success", False)),
            len(change_imports),
        )
        restored_change.rescanblockchain()
        assert_equal(Decimal(restored_change.getdigidollarbalance()["total"]), Decimal(9000))

        restored_change_receives = [
            tx for tx in restored_change.listdigidollartxs(100, 0, "", "receive")
            if tx["txid"] == outgoing_txid
        ]
        assert_equal(len(restored_change_receives), len(original_change_receives))

        try:
            self.nodes[1].unloadwallet("dd_change_history_restored")
            self.nodes[1].unloadwallet("dd_change_history_source")
        except Exception:
            pass

        self.log.info("  Outgoing DD change is not restored as receive history")

        self.log.info("  Creating isolated source wallet for redeem-change restore history")
        self.nodes[1].createwallet(
            wallet_name="dd_redeem_change_history_source",
            disable_private_keys=False,
            blank=False,
            descriptors=True,
        )
        redeem_change_source = self.nodes[1].get_wallet_rpc("dd_redeem_change_history_source")

        redeem_change_fund_addr = redeem_change_source.getnewaddress("", "bech32")
        self.nodes[0].sendtoaddress(redeem_change_fund_addr, 50000)
        for _ in range(35):
            self.nodes[0].sendtoaddress(redeem_change_source.getnewaddress("", "bech32"), 1)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        self.refresh_oracle_quotes()
        small_mint = redeem_change_source.mintdigidollar(10000, 0)
        large_mint = redeem_change_source.mintdigidollar(15000, 0)
        small_position_id = small_mint["position_id"]
        self.generate(self.nodes[1], 350)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.refresh_oracle_quotes()
        assert_equal(Decimal(redeem_change_source.getdigidollarbalance()["total"]), Decimal(25000))

        away_tx = redeem_change_source.senddigidollar(self.nodes[0].getdigidollaraddress("redeem-change-away"), 10000)
        self.generate(self.nodes[1], 1)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: redeem_change_source.gettransaction(away_tx["txid"])["confirmations"] > 0)
        assert_equal(Decimal(redeem_change_source.getdigidollarbalance()["total"]), Decimal(15000))

        redeem_change = redeem_change_source.redeemdigidollar(small_position_id, 10000)
        redeem_change_txid = redeem_change["txid"]
        self.generate(self.nodes[1], 1)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: redeem_change_source.gettransaction(redeem_change_txid)["confirmations"] > 0)
        assert_equal(Decimal(redeem_change_source.getdigidollarbalance()["total"]), Decimal(5000))

        original_redeem_change_rows = [
            tx for tx in redeem_change_source.listdigidollartxs(200, 0)
            if tx["txid"] == redeem_change_txid
        ]
        original_redeems = [tx for tx in original_redeem_change_rows if tx["category"] == "redeem"]
        original_redeem_changes = [tx for tx in original_redeem_change_rows if tx["category"] == "redeem_change"]
        original_redeem_receives = [tx for tx in original_redeem_change_rows if tx["category"] == "receive"]
        assert_equal(len(original_redeems), 1)
        assert_equal(original_redeems[0]["amount"], Decimal(-10000))
        assert_equal(len(original_redeem_changes), 1)
        assert_equal(original_redeem_changes[0]["amount"], Decimal(5000))
        assert_equal(len(original_redeem_receives), 0)

        redeem_change_fragment_addresses = [
            redeem_change_source.getdigidollaraddress(f"redeem-change-fragment-{i}") for i in range(4)
        ]
        redeem_change_fragment = redeem_change_source.sendmanydigidollar(
            "",
            {addr: 500 for addr in redeem_change_fragment_addresses},
            "redeem-change self-fragment restore-history regression",
        )
        redeem_change_fragment_txid = redeem_change_fragment["txid"]
        assert_equal(redeem_change_fragment["total_amount"], 2000)
        self.generate(self.nodes[1], 1)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: redeem_change_source.gettransaction(redeem_change_fragment_txid)["confirmations"] > 0)
        assert_equal(Decimal(redeem_change_source.getdigidollarbalance()["total"]), Decimal(5000))

        original_redeem_change_fragment_receives = [
            tx for tx in redeem_change_source.listdigidollartxs(200, 0, "", "receive")
            if tx["txid"] == redeem_change_fragment_txid
        ]
        assert_equal(len(original_redeem_change_fragment_receives), len(redeem_change_fragment_addresses))
        assert_equal(
            sorted((tx["address"], tx["amount"]) for tx in original_redeem_change_fragment_receives),
            sorted((addr, Decimal(500)) for addr in redeem_change_fragment_addresses),
        )

        original_redeem_change_fragment_sends = [
            tx for tx in redeem_change_source.listdigidollartxs(200, 0, "", "send")
            if tx["txid"] == redeem_change_fragment_txid
        ]
        assert_equal(len(original_redeem_change_fragment_sends), 1)
        assert_equal(abs(original_redeem_change_fragment_sends[0]["amount"]), Decimal(2000))

        redeem_change_descriptors = redeem_change_source.listdescriptors(True)["descriptors"]
        self.nodes[1].createwallet(
            wallet_name="dd_redeem_change_history_restored",
            disable_private_keys=False,
            blank=True,
            descriptors=True,
        )
        restored_redeem_change = self.nodes[1].get_wallet_rpc("dd_redeem_change_history_restored")

        redeem_change_imports = []
        for desc in redeem_change_descriptors:
            req = {
                "desc": desc["desc"],
                "timestamp": 0,
                "active": desc.get("active", False),
                "internal": desc.get("internal", False),
            }
            if "range" in desc:
                req["range"] = desc["range"]
            redeem_change_imports.append(req)

        redeem_change_import_result = restored_redeem_change.importdescriptors(redeem_change_imports)
        assert_equal(
            sum(1 for item in redeem_change_import_result if item.get("success", False)),
            len(redeem_change_imports),
        )
        restored_redeem_change.rescanblockchain()
        assert_equal(Decimal(restored_redeem_change.getdigidollarbalance()["total"]), Decimal(5000))

        restored_redeem_change_rows = [
            tx for tx in restored_redeem_change.listdigidollartxs(200, 0)
            if tx["txid"] == redeem_change_txid
        ]
        restored_redeems = [tx for tx in restored_redeem_change_rows if tx["category"] == "redeem"]
        restored_redeem_changes = [tx for tx in restored_redeem_change_rows if tx["category"] == "redeem_change"]
        restored_redeem_receives = [tx for tx in restored_redeem_change_rows if tx["category"] == "receive"]
        assert_equal(len(restored_redeems), 1)
        assert_equal(restored_redeems[0]["amount"], Decimal(-10000))
        assert_equal(len(restored_redeem_changes), 1)
        assert_equal(restored_redeem_changes[0]["amount"], Decimal(5000))
        assert_equal(len(restored_redeem_receives), 0)

        restored_redeem_change_fragment_receives = [
            tx for tx in restored_redeem_change.listdigidollartxs(200, 0, "", "receive")
            if tx["txid"] == redeem_change_fragment_txid
        ]
        assert_equal(
            len(restored_redeem_change_fragment_receives),
            len(original_redeem_change_fragment_receives),
        )
        assert_equal(
            sorted((tx["address"], tx["amount"]) for tx in restored_redeem_change_fragment_receives),
            sorted((tx["address"], tx["amount"]) for tx in original_redeem_change_fragment_receives),
        )

        restored_redeem_change_fragment_sends = [
            tx for tx in restored_redeem_change.listdigidollartxs(200, 0, "", "send")
            if tx["txid"] == redeem_change_fragment_txid
        ]
        assert_equal(len(restored_redeem_change_fragment_sends), 1)
        assert_equal(
            abs(restored_redeem_change_fragment_sends[0]["amount"]),
            abs(original_redeem_change_fragment_sends[0]["amount"]),
        )

        try:
            self.nodes[1].unloadwallet("dd_redeem_change_history_restored")
            self.nodes[1].unloadwallet("dd_redeem_change_history_source")
        except Exception:
            pass

        self.log.info("  Redeem DD change UTXO and history restored")

        self.log.info("  Creating isolated source wallet for spent self-fragment history")
        self.nodes[1].createwallet(
            wallet_name="dd_receive_history_source",
            disable_private_keys=False,
            blank=False,
            descriptors=True,
        )
        source = self.nodes[1].get_wallet_rpc("dd_receive_history_source")

        source_fund_addr = source.getnewaddress("", "bech32")
        self.nodes[0].sendtoaddress(source_fund_addr, 10000)
        for _ in range(25):
            self.nodes[0].sendtoaddress(source.getnewaddress("", "bech32"), 1)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        self.refresh_oracle_quotes()
        mint = source.mintdigidollar(10000, 2)
        assert "txid" in mint
        self.generate(self.nodes[1], 2)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: source.gettransaction(mint["txid"])["confirmations"] > 0)
        assert_equal(Decimal(source.getdigidollarbalance()["total"]), Decimal(10000))

        fragment_addresses = [
            source.getdigidollaraddress(f"spent-fragment-{i}") for i in range(20)
        ]
        fragment_amounts = {addr: 500 for addr in fragment_addresses}
        fragment = source.sendmanydigidollar(
            "",
            fragment_amounts,
            "spent self-fragment restore-history regression",
        )
        fragment_txid = fragment["txid"]
        assert_equal(fragment["total_amount"], 10000)
        self.generate(self.nodes[1], 1)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: source.gettransaction(fragment_txid)["confirmations"] > 0)
        assert_equal(Decimal(source.getdigidollarbalance()["total"]), Decimal(10000))

        spend_to = self.nodes[0].getdigidollaraddress("spent-fragment-destination")
        spend_txids = []
        for _ in range(20):
            spend = source.senddigidollar(spend_to, 500)
            spend_txids.append(spend["txid"])
        self.generate(self.nodes[1], 2)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        for txid in spend_txids:
            self.wait_until(lambda txid=txid: source.gettransaction(txid)["confirmations"] > 0)
        assert_equal(Decimal(source.getdigidollarbalance()["total"]), Decimal(0))

        original_fragment_rows = [
            tx for tx in source.listdigidollartxs(200, 0, "", "receive")
            if tx["txid"] == fragment_txid
        ]
        assert_equal(len(original_fragment_rows), len(fragment_addresses))
        assert_equal(
            sorted((tx["address"], tx["amount"]) for tx in original_fragment_rows),
            sorted((addr, Decimal(500)) for addr in fragment_addresses),
        )

        descriptors = source.listdescriptors(True)["descriptors"]
        self.nodes[1].createwallet(
            wallet_name="dd_spent_receive_history_restored",
            disable_private_keys=False,
            blank=True,
            descriptors=True,
        )
        restored_spent = self.nodes[1].get_wallet_rpc("dd_spent_receive_history_restored")

        imports = []
        for desc in descriptors:
            req = {
                "desc": desc["desc"],
                "timestamp": 0,
                "active": desc.get("active", False),
                "internal": desc.get("internal", False),
            }
            if "range" in desc:
                req["range"] = desc["range"]
            imports.append(req)

        import_result = restored_spent.importdescriptors(imports)
        assert_equal(
            sum(1 for item in import_result if item.get("success", False)),
            len(imports),
        )
        restored_spent.rescanblockchain()

        restored_fragment_rows = [
            tx for tx in restored_spent.listdigidollartxs(200, 0, "", "receive")
            if tx["txid"] == fragment_txid
        ]
        assert_equal(len(restored_fragment_rows), len(original_fragment_rows))
        assert_equal(
            sorted((tx["address"], tx["amount"]) for tx in restored_fragment_rows),
            sorted((tx["address"], tx["amount"]) for tx in original_fragment_rows),
        )

        try:
            self.nodes[1].unloadwallet("dd_spent_receive_history_restored")
            self.nodes[1].unloadwallet("dd_receive_history_source")
        except Exception:
            pass

        self.log.info("  Spent self-fragment receive history restored")

        self.log.info("  Creating isolated source wallet for self-fragment-with-change history")
        self.nodes[1].createwallet(
            wallet_name="dd_self_change_history_source",
            disable_private_keys=False,
            blank=False,
            descriptors=True,
        )
        self_change_source = self.nodes[1].get_wallet_rpc("dd_self_change_history_source")

        self_change_fund_addr = self_change_source.getnewaddress("", "bech32")
        self.nodes[0].sendtoaddress(self_change_fund_addr, 10000)
        for _ in range(25):
            self.nodes[0].sendtoaddress(self_change_source.getnewaddress("", "bech32"), 1)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        self.refresh_oracle_quotes()
        self_change_mint = self_change_source.mintdigidollar(18000, 2)
        assert "txid" in self_change_mint
        self.generate(self.nodes[1], 2)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: self_change_source.gettransaction(self_change_mint["txid"])["confirmations"] > 0)
        assert_equal(Decimal(self_change_source.getdigidollarbalance()["total"]), Decimal(18000))

        self_change_addresses = [
            self_change_source.getdigidollaraddress(f"self-change-fragment-{i}") for i in range(20)
        ]
        self_change_amounts = {addr: 500 for addr in self_change_addresses}
        self_change_fragment = self_change_source.sendmanydigidollar(
            "",
            self_change_amounts,
            "self-fragment restore-history change regression",
        )
        self_change_txid = self_change_fragment["txid"]
        assert_equal(self_change_fragment["total_amount"], 10000)
        self.generate(self.nodes[1], 1)
        self.sync_all()
        self.nodes[1].syncwithvalidationinterfacequeue()
        self.wait_until(lambda: self_change_source.gettransaction(self_change_txid)["confirmations"] > 0)
        assert_equal(Decimal(self_change_source.getdigidollarbalance()["total"]), Decimal(18000))

        original_self_change_receives = [
            tx for tx in self_change_source.listdigidollartxs(200, 0, "", "receive")
            if tx["txid"] == self_change_txid
        ]
        assert_equal(len(original_self_change_receives), len(self_change_addresses))
        assert_equal(
            sorted((tx["address"], tx["amount"]) for tx in original_self_change_receives),
            sorted((addr, Decimal(500)) for addr in self_change_addresses),
        )

        original_self_change_sends = [
            tx for tx in self_change_source.listdigidollartxs(200, 0, "", "send")
            if tx["txid"] == self_change_txid
        ]
        assert_equal(len(original_self_change_sends), 1)
        assert_equal(abs(original_self_change_sends[0]["amount"]), Decimal(10000))

        self_change_descriptors = self_change_source.listdescriptors(True)["descriptors"]
        self.nodes[1].createwallet(
            wallet_name="dd_self_change_history_restored",
            disable_private_keys=False,
            blank=True,
            descriptors=True,
        )
        restored_self_change = self.nodes[1].get_wallet_rpc("dd_self_change_history_restored")

        self_change_imports = []
        for desc in self_change_descriptors:
            req = {
                "desc": desc["desc"],
                "timestamp": 0,
                "active": desc.get("active", False),
                "internal": desc.get("internal", False),
            }
            if "range" in desc:
                req["range"] = desc["range"]
            self_change_imports.append(req)

        self_change_import_result = restored_self_change.importdescriptors(self_change_imports)
        assert_equal(
            sum(1 for item in self_change_import_result if item.get("success", False)),
            len(self_change_imports),
        )
        restored_self_change.rescanblockchain()
        assert_equal(Decimal(restored_self_change.getdigidollarbalance()["total"]), Decimal(18000))

        restored_self_change_receives = [
            tx for tx in restored_self_change.listdigidollartxs(200, 0, "", "receive")
            if tx["txid"] == self_change_txid
        ]
        assert_equal(len(restored_self_change_receives), len(original_self_change_receives))
        assert_equal(
            sorted((tx["address"], tx["amount"]) for tx in restored_self_change_receives),
            sorted((tx["address"], tx["amount"]) for tx in original_self_change_receives),
        )

        restored_self_change_sends = [
            tx for tx in restored_self_change.listdigidollartxs(200, 0, "", "send")
            if tx["txid"] == self_change_txid
        ]
        assert_equal(len(restored_self_change_sends), 1)
        assert_equal(
            abs(restored_self_change_sends[0]["amount"]),
            abs(original_self_change_sends[0]["amount"]),
        )

        try:
            self.nodes[1].unloadwallet("dd_self_change_history_restored")
            self.nodes[1].unloadwallet("dd_self_change_history_source")
        except Exception:
            pass

        self.log.info("  Self-fragment DD change is not restored as receive history")

        receiver_addresses = [self.nodes[1].getdigidollaraddress() for _ in range(5)]
        amounts = {addr: 200 for addr in receiver_addresses}

        sender_before = Decimal(self.nodes[0].getdigidollarbalance()["total"])
        receiver_before = Decimal(self.nodes[1].getdigidollarbalance()["total"])

        self.refresh_oracle_quotes()
        result = self.nodes[0].sendmanydigidollar(
            "",
            amounts,
            "descriptor restore receive history regression",
        )
        txid = result["txid"]
        assert_equal(result["total_amount"], 1000)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        assert_equal(
            Decimal(self.nodes[0].getdigidollarbalance()["total"]),
            sender_before - Decimal(1000),
        )
        assert_equal(
            Decimal(self.nodes[1].getdigidollarbalance()["total"]),
            receiver_before + Decimal(1000),
        )

        original_rows = [
            tx for tx in self.nodes[1].listdigidollartxs(100, 0, "", "receive")
            if tx["txid"] == txid
        ]
        assert_equal(len(original_rows), len(receiver_addresses))
        assert_equal(sum(tx["amount"] for tx in original_rows), Decimal(1000))

        descriptors = self.nodes[1].listdescriptors(True)["descriptors"]
        self.nodes[1].createwallet(
            wallet_name="dd_receive_history_restored",
            disable_private_keys=False,
            blank=True,
            descriptors=True,
        )
        restored = self.nodes[1].get_wallet_rpc("dd_receive_history_restored")

        imports = []
        for desc in descriptors:
            req = {
                "desc": desc["desc"],
                "timestamp": 0,
                "active": desc.get("active", False),
                "internal": desc.get("internal", False),
            }
            if "range" in desc:
                req["range"] = desc["range"]
            imports.append(req)

        import_result = restored.importdescriptors(imports)
        assert_equal(
            sum(1 for item in import_result if item.get("success", False)),
            len(imports),
        )

        rescan_result = restored.rescanblockchain()
        self.log.info(f"  Restored wallet rescan completed: {rescan_result}")

        restored_rows = [
            tx for tx in restored.listdigidollartxs(100, 0, "", "receive")
            if tx["txid"] == txid
        ]
        assert_equal(len(restored_rows), len(original_rows))
        assert_equal(
            sorted((tx["address"], tx["amount"]) for tx in restored_rows),
            sorted((tx["address"], tx["amount"]) for tx in original_rows),
        )

        # A rescan has no wallet record to read a timestamp from, so it takes
        # the timestamp of the block the transaction is in. The rescan is not
        # allowed to ask the chain for that block while it holds the wallet
        # lock, so the block time is handed to it instead. Check the figure that
        # arrives is the right one.
        block_hash = self.nodes[1].getrawtransaction(txid, True)["blockhash"]
        block_time = self.nodes[1].getblock(block_hash)["time"]
        for tx in restored_rows:
            assert_equal(tx["time"], block_time)

        try:
            self.nodes[1].unloadwallet("dd_receive_history_restored")
        except Exception:
            pass

        self.log.info("  SUCCESS: Descriptor restore preserved DD receive history")

    def test_bounded_rescan_skips_full_dd_scan(self):
        """Test that a height-bounded rescan does not run a full DD UTXO rebuild."""
        self.log.info("Test 6: Testing bounded rescan avoids full DigiDollar scan...")

        self.nodes[0].createwallet(
            wallet_name="bounded_rescan",
            descriptors=True,
            blank=True
        )
        bounded_wallet = self.nodes[0].get_wallet_rpc("bounded_rescan")
        tip = self.nodes[0].getblockcount()

        # A rescan bounded to one block only records that the positions still
        # need checking against the chain. A rescan of the whole chain rebuilds
        # the wallet's DigiDollar state, which is the expensive part.
        with self.nodes[0].assert_debug_log(
            expected_msgs=["DigiDollar: bounded rescan finished"],
            unexpected_msgs=["DigiDollar: rebuilt DigiDollar state after rescan"],
            timeout=1):
            rescan_result = bounded_wallet.rescanblockchain(tip, tip)

        assert_equal(rescan_result["start_height"], tip)
        assert_equal(rescan_result["stop_height"], tip)

        with self.nodes[0].assert_debug_log(
            expected_msgs=["DigiDollar: rebuilt DigiDollar state after rescan"],
            unexpected_msgs=[],
            timeout=5):
            bounded_wallet.rescanblockchain()

        self.log.info("  SUCCESS: Bounded rescan skipped full DigiDollar scan")

    def test_rescan_progress_reporting(self):
        """Test rescan progress reporting via getwalletinfo."""
        self.log.info("Test 7: Testing rescan progress reporting...")

        # Mine some blocks to ensure rescan takes measurable time
        self.generate(self.nodes[0], 50)
        self.sync_all()

        # Create a new wallet for testing rescan progress
        self.log.info("  Creating test wallet for progress monitoring...")
        self.nodes[0].createwallet(
            wallet_name="progress_test",
            descriptors=True,
            blank=True
        )

        progress_wallet = self.nodes[0].get_wallet_rpc("progress_test")

        # Check wallet info for scanning status
        wallet_info = progress_wallet.getwalletinfo()
        self.log.info(f"  Wallet info: {wallet_info}")

        # The 'scanning' field indicates if wallet is currently rescanning
        if 'scanning' in wallet_info:
            scanning_status = wallet_info['scanning']
            self.log.info(f"  Scanning status: {scanning_status}")

            if isinstance(scanning_status, dict):
                # Wallet is currently scanning
                if 'progress' in scanning_status:
                    progress = scanning_status['progress']
                    self.log.info(f"  Scan progress: {progress * 100:.1f}%")
                    assert progress >= 0 and progress <= 1, "Invalid progress value"
            elif scanning_status == False:
                # Wallet finished scanning (expected if rescan completed quickly)
                self.log.info("  Rescan completed (no active scan)")

        # Perform an explicit rescan and verify completion
        self.log.info("  Performing explicit rescan...")
        rescan_result = progress_wallet.rescanblockchain()

        # Verify rescan result contains expected fields
        assert 'start_height' in rescan_result, "Missing start_height in rescan result"
        assert 'stop_height' in rescan_result, "Missing stop_height in rescan result"

        self.log.info(f"  Rescan range: {rescan_result['start_height']} to {rescan_result['stop_height']}")

        # After rescan, scanning should be false
        wallet_info_after = progress_wallet.getwalletinfo()
        if 'scanning' in wallet_info_after:
            assert wallet_info_after['scanning'] == False, "Wallet still scanning after rescanblockchain returned"

        self.log.info("  SUCCESS: Rescan progress reporting works correctly")


if __name__ == '__main__':
    DigiDollarRescanTest().main()
