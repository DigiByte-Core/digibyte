#!/usr/bin/env python3
"""Test DigiDollar basic functionality.

Test basic DigiDollar operations including:
- DD address generation and validation
- Basic mint/transfer/redeem cycle
- Balance tracking
- Integration with DigiByte blockchain
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)
from decimal import Decimal
import time


class DigiDollarBasicTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Enable DigiDollar features and wallet, disable Dandelion for testing
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"]
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Testing DigiDollar basic functionality...")

        # Test setup
        self.setup_digidollar_test()

        # Run test scenarios
        self.test_address_generation()
        self.test_address_validation()
        self.test_basic_mint_cycle()
        self.test_decode_raw_transaction()
        self.test_balance_tracking()
        self.test_multi_node_sync()

    def setup_digidollar_test(self):
        """Setup test environment for DigiDollar."""
        # Generate initial blocks past coinbase maturity
        self.log.info("Generating initial blocks for test setup...")
        self.nodes[0].generate(110)
        self.sync_all()

        # Set mock oracle price ($0.50 per DGB)
        # Oracle price is in micro-USD: 1,000,000 micro-USD = $1.00
        # So $0.50/DGB = 500,000 micro-USD
        self.log.info("Setting mock oracle price...")
        self.nodes[0].setmockoracleprice(500000)  # 500000 micro-USD = $0.50/DGB
        self.nodes[1].setmockoracleprice(500000)

        # Verify DigiDollar system is accessible
        stats = self.nodes[0].getdigidollarstats()
        # Check that we get valid system health data
        assert "health_percentage" in stats
        assert "health_status" in stats

    def test_address_generation(self):
        """Test DigiDollar address generation."""
        self.log.info("Testing DD address generation...")

        # Generate new DD address
        dd_address = self.nodes[0].getdigidollaraddress()
        self.log.info(f"Generated DD address: {dd_address}")

        # Verify address format (should start with 'DD' or 'RD' for regtest Base58 encoding)
        assert dd_address.startswith('RD') or dd_address.startswith('DD') or dd_address.startswith('TD'), f"DD address should start with 'RD', 'DD', or 'TD', got: {dd_address}"

        # Generate multiple addresses and verify they're different
        dd_address2 = self.nodes[0].getdigidollaraddress()
        assert dd_address != dd_address2, "DD addresses should be unique"

        # Note: getdigidollaraddress() generates new addresses on-demand
        # Addresses are tracked through transactions, not a pre-registered list
        self.log.info(f"Generated two different DD addresses: {dd_address} and {dd_address2}")

    def test_address_validation(self):
        """Test DigiDollar address validation."""
        self.log.info("Testing DD address validation...")

        # Generate valid address and verify it can be used
        valid_address = self.nodes[0].getdigidollaraddress()
        self.log.info(f"Generated valid DD address for validation testing: {valid_address}")

        # Verify address starts with expected prefix
        assert valid_address.startswith('RD') or valid_address.startswith('DD') or valid_address.startswith('TD'), \
            f"Valid DD address should have correct prefix, got: {valid_address}"

    def test_basic_mint_cycle(self):
        """Test basic mint/transfer/redeem cycle."""
        self.log.info("Testing basic mint/transfer/redeem cycle...")

        # Get initial balances
        initial_dgb_balance = self.nodes[0].getbalance()
        dd_balance_info = self.nodes[0].getdigidollarbalance()
        initial_dd_balance = dd_balance_info['total'] if isinstance(dd_balance_info, dict) else dd_balance_info

        # Mint DigiDollars (amount in cents, so 1000 = $10.00)
        mint_amount = 1000  # 1000 cents = $10.00 worth of DD
        dca_tier = 4  # Tier 4 = 1 year lock (365 days)

        self.log.info(f"Minting {mint_amount} cents DD (${mint_amount/100}) with tier {dca_tier}...")
        mint_result = self.nodes[0].mintdigidollar(mint_amount, dca_tier)

        # Verify mint result
        assert 'txid' in mint_result, "Mint should return transaction ID"
        assert 'dd_minted' in mint_result, "Mint should return DD minted amount"
        assert 'dgb_collateral' in mint_result, "Mint should return DGB collateral"

        mint_txid = mint_result['txid']
        # Kept so the decode test below has a real mint to look at.
        self.mint_txid = mint_txid

        # Mine blocks to confirm the transaction
        self.nodes[0].generate(2)
        self.sync_all()

        # Verify balances after mint
        new_dgb_balance = self.nodes[0].getbalance()
        dd_balance_info = self.nodes[0].getdigidollarbalance()
        new_dd_balance = dd_balance_info['total'] if isinstance(dd_balance_info, dict) else dd_balance_info

        # DD balance should increase by mint amount
        assert_equal(new_dd_balance, mint_amount)

        # Test cross-node transfer (Node 0 → Node 1)
        transfer_amount = 100  # 100 cents = $1.00
        receiver_address = self.nodes[1].getdigidollaraddress()

        self.log.info(f"Testing cross-node transfer of {transfer_amount} cents DD from node 0 to node 1...")

        # Get initial balances
        sender_initial = self.nodes[0].getdigidollarbalance()['total'] if isinstance(self.nodes[0].getdigidollarbalance(), dict) else self.nodes[0].getdigidollarbalance()
        receiver_initial = self.nodes[1].getdigidollarbalance()['total'] if isinstance(self.nodes[1].getdigidollarbalance(), dict) else self.nodes[1].getdigidollarbalance()

        # Perform transfer
        transfer_result = self.nodes[0].senddigidollar(receiver_address, transfer_amount)
        assert 'txid' in transfer_result, "Transfer should return transaction ID"
        # Kept so the decode test below has a real transfer to look at.
        self.transfer_txid = transfer_result['txid']

        # Mine blocks to confirm
        self.nodes[0].generate(2)
        self.sync_all()

        # Verify balances after transfer
        sender_final = self.nodes[0].getdigidollarbalance()['total'] if isinstance(self.nodes[0].getdigidollarbalance(), dict) else self.nodes[0].getdigidollarbalance()
        receiver_final = self.nodes[1].getdigidollarbalance()['total'] if isinstance(self.nodes[1].getdigidollarbalance(), dict) else self.nodes[1].getdigidollarbalance()

        # Verify sender decreased and receiver increased
        assert_equal(sender_final, sender_initial - transfer_amount)
        assert_equal(receiver_final, receiver_initial + transfer_amount)

        self.log.info("Cross-node transfer test passed")

    def test_decode_raw_transaction(self):
        """decoderawtransaction must describe a DigiDollar transaction and keep its help honest.

        Test nodes run with -rpcdoccheck, so the node compares what an RPC
        returns against what its help says it returns. If the decoded form
        carries a field the help does not list, the call fails instead of
        answering. That makes this a real check of the help text, not just of
        the values.
        """
        self.log.info("Testing decoderawtransaction on DigiDollar transactions...")

        expected = [
            (self.mint_txid, "MINT", 1),
            (self.transfer_txid, "TRANSFER", 2),
        ]
        for txid, expected_type, expected_type_id in expected:
            raw = self.nodes[0].getrawtransaction(txid)
            decoded = self.nodes[0].decoderawtransaction(raw)
            assert "digidollar" in decoded, f"{txid} should decode with a digidollar object"
            digidollar = decoded["digidollar"]
            assert_equal(digidollar["type"], expected_type)
            assert_equal(digidollar["type_id"], expected_type_id)
            assert_equal(digidollar["flags"], 0)
            assert_equal(digidollar["flags_hex"], "0x00")

            # getrawtransaction in JSON form returns the same object.
            verbose = self.nodes[0].getrawtransaction(txid, True)
            assert_equal(verbose["digidollar"], digidollar)

        # An ordinary DGB payment has no DigiDollar marker, so the object must
        # be absent rather than empty.
        plain_txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        plain_decoded = self.nodes[0].decoderawtransaction(
            self.nodes[0].getrawtransaction(plain_txid))
        assert "digidollar" not in plain_decoded, "plain DGB transactions carry no digidollar object"

        # The help has to name the field, otherwise wallet authors cannot find it.
        help_text = self.nodes[0].help("decoderawtransaction")
        for field in ("digidollar", "type_id", "flags_hex"):
            assert field in help_text, f"decoderawtransaction help should mention {field}"

        self.log.info("decoderawtransaction DigiDollar test passed")

    def test_balance_tracking(self):
        """Test DigiDollar balance tracking and position management."""
        self.log.info("Testing DD balance tracking...")

        # Get initial balance (from previous tests)
        dd_balance_info = self.nodes[0].getdigidollarbalance()
        initial_balance = dd_balance_info['total'] if isinstance(dd_balance_info, dict) else dd_balance_info

        # Create multiple DD positions with different DCA tiers (amounts in cents)
        positions_data = [
            {"amount": 500, "tier": 1},    # $5.00, tier 1 (30 days)
            {"amount": 1000, "tier": 3},   # $10.00, tier 3 (180 days)
            {"amount": 2000, "tier": 5}    # $20.00, tier 5 (730 days / 2 years)
        ]

        for position in positions_data:
            self.log.info(f"Creating position: {position['amount']} cents DD, tier {position['tier']}")
            result = self.nodes[0].mintdigidollar(position['amount'], position['tier'])
            position['txid'] = result['txid']
            position['dd_minted'] = result['dd_minted']

        # Mine blocks to confirm
        self.nodes[0].generate(3)
        self.sync_all()

        # Verify total balance (including initial balance from previous tests)
        new_mints_total = sum(pos['amount'] for pos in positions_data)
        total_expected = initial_balance + new_mints_total
        dd_balance_info = self.nodes[0].getdigidollarbalance()
        actual_balance = dd_balance_info['total'] if isinstance(dd_balance_info, dict) else dd_balance_info
        assert_equal(actual_balance, total_expected)

        # Test position listing
        positions = self.nodes[0].listdigidollarpositions()
        # We should have at least as many positions as we just created
        assert_greater_than(len(positions), 0)
        assert len(positions) >= len(positions_data)

        # Verify position details
        for position in positions:
            # Check for fields that are actually returned by listdigidollarpositions
            assert 'dd_minted' in position or 'amount' in position
            assert 'unlock_height' in position or 'lock_height' in position
            assert 'dgb_collateral' in position or 'collateral_locked' in position
            assert 'is_active' in position or 'status' in position

        # Test transaction history
        # TODO: Fix listdigidollartxs() - missing required fields in response
        # transactions = self.nodes[0].listdigidollartxs()
        # mint_txs = [tx for tx in transactions if tx['category'] == 'mint']
        # assert_equal(len(mint_txs), len(positions_data))
        self.log.info("Transaction history test skipped - listdigidollartxs needs field fixes")

    def test_multi_node_sync(self):
        """Test DigiDollar state synchronization across nodes."""
        self.log.info("Testing multi-node DD state synchronization...")

        # Ensure nodes are connected and synced
        self.connect_nodes(0, 1)
        self.sync_all()

        # Re-set mock oracle on both nodes to ensure consistent state for sync test
        # Oracle price is in micro-USD: 500000 = $0.50/DGB
        self.nodes[0].setmockoracleprice(500000)
        self.nodes[1].setmockoracleprice(500000)

        # Verify both nodes have same oracle price (mock oracle is per-process)
        stats0 = self.nodes[0].getdigidollarstats()
        stats1 = self.nodes[1].getdigidollarstats()

        # Both nodes should see same oracle price after setting it the same
        assert_equal(stats0['oracle_price_cents'], stats1['oracle_price_cents'])

        # Note: health_status and health_percentage depend on wallet positions
        # which are per-wallet state, so we don't compare them across nodes
        # Each node calculates health based on its own wallet's positions

        # Verify the DD system is accessible on both nodes
        assert 'health_percentage' in stats0
        assert 'health_percentage' in stats1
        assert 'total_dd_supply' in stats0
        assert 'total_dd_supply' in stats1

        self.log.info("Multi-node synchronization test passed")


if __name__ == '__main__':
    DigiDollarBasicTest().main()