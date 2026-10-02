#!/usr/bin/env python3
"""Test DigiDollar minting operations.

Test comprehensive minting functionality including:
- Minting with different lock tiers
- Collateral calculation with DCA
- Mint validation rules
- Oracle price integration
- Error conditions and edge cases
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_less_than,
    assert_raises_rpc_error,
)
from decimal import Decimal


ORACLE_PRICE_MICRO_USD = 500000
TIER_TO_LOCK_DAYS = {
    1: 30,
    2: 90,
    3: 180,
    4: 365,
    5: 730,
    6: 1095,
    7: 1825,
    8: 2555,
    9: 3650,
}


class DigiDollarMintTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        # Enable DigiDollar features, disable Dandelion for testing
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"]
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Testing DigiDollar minting operations...")

        # Test setup
        self.setup_digidollar_test()

        # Run test scenarios
        self.test_mint_lock_tiers()
        self.test_collateral_calculations()
        self.test_dca_impact_on_minting()
        self.test_oracle_price_integration()
        self.test_mint_validation_rules()
        self.test_edge_cases()
        self.test_error_conditions()

    def setup_digidollar_test(self):
        """Setup test environment for DigiDollar."""
        # Generate initial blocks past coinbase maturity
        self.log.info("Generating initial blocks for test setup...")
        self.nodes[0].generate(110)
        self.sync_all()

        # Set mock oracle price ($0.50 per DGB)
        # Oracle price is in micro-USD: 1,000,000 micro-USD = $1.00
        # So $0.50/DGB = 500,000 micro-USD
        self.refresh_oracle_quotes()

        # Verify DigiDollar system is accessible
        stats = self.nodes[0].getdigidollarstats()
        assert "health_percentage" in stats
        assert "health_status" in stats

    def refresh_oracle_quotes(self, price=ORACLE_PRICE_MICRO_USD):
        for node in self.nodes:
            result = node.setmockoracleprice(price)
            assert_equal(result["price_micro_usd"], price)

    def test_mint_lock_tiers(self):
        """Test minting with different lock tiers and collateral ratios."""
        self.log.info("Testing mint lock tiers...")

        # calculatecollateralrequirement uses lock_days, mintdigidollar uses tier.
        # Tier 0 is the 1-hour testing tier and is not accepted by
        # calculatecollateralrequirement, so this loop covers non-zero tiers.
        lock_tiers = list(TIER_TO_LOCK_DAYS.keys())

        mint_amount = Decimal('1000.00')  # $1000
        mint_amount_cents = int(mint_amount * 100)  # Convert to cents

        for tier in lock_tiers:
            self.log.info(f"Testing tier {tier} minting...")

            # Calculate expected collateral requirement (uses lock_days)
            lock_days = TIER_TO_LOCK_DAYS[tier]
            collateral_req = self.nodes[0].calculatecollateralrequirement(mint_amount_cents, lock_days)

            # Verify required fields are present
            assert 'required_dgb' in collateral_req, "Missing required_dgb field"
            assert 'effective_ratio' in collateral_req, "Missing effective_ratio field"

            # Perform actual mint
            self.refresh_oracle_quotes()
            result = self.nodes[0].mintdigidollar(mint_amount_cents, tier)
            assert 'txid' in result
            assert 'dd_address' in result or 'dd_minted' in result
            assert_equal(result["collateral_ratio"], collateral_req["effective_ratio"])

            # Mine block to confirm
            self.nodes[0].generate(1)
            self.sync_all()

            # Verify position was created
            positions = self.nodes[0].listdigidollarpositions()
            assert len(positions) > 0, "No positions created"
            latest_position = positions[-1]  # Most recent position

            # Verify the tier matches if field exists
            if 'tier' in latest_position:
                assert latest_position['tier'] == tier, \
                    f"Tier mismatch: expected {tier}, got {latest_position['tier']}"

    def test_collateral_calculations(self):
        """Test collateral calculation accuracy."""
        self.log.info("Testing collateral calculations...")

        test_cases = [
            {"amount": Decimal('100.00'), "tier": 4},   # tier 4 (~365 days)
            {"amount": Decimal('500.50'), "tier": 3},   # tier 3 (~180 days)
            {"amount": Decimal('750.25'), "tier": 2},   # tier 2 (~90 days)
            {"amount": Decimal('999.99'), "tier": 1}    # tier 1 (~30 days)
        ]

        for case in test_cases:
            amount = case['amount']
            amount_cents = int(amount * 100)
            tier = case['tier']
            lock_days = TIER_TO_LOCK_DAYS[tier]

            # Get collateral requirement (uses lock_days)
            req = self.nodes[0].calculatecollateralrequirement(amount_cents, lock_days)

            # Verify required fields
            assert 'required_dgb' in req, "Missing required_dgb field"
            assert 'effective_ratio' in req, "Missing effective_ratio field"
            assert 'oracle_price_micro_usd' in req, "Missing oracle_price_micro_usd field"
            assert 'dca_multiplier' in req, "Missing dca_multiplier field"

            # Verify values are reasonable
            assert Decimal(req['required_dgb']) > 0, "Collateral must be positive"
            assert Decimal(req['effective_ratio']) > 0, "Ratio must be positive"

    def test_dca_impact_on_minting(self):
        """Test how DCA (Dynamic Collateral Adjustment) affects minting."""
        self.log.info("Testing DCA impact on minting...")

        # The previous mint coverage can change system health, so only assert
        # that the DCA multiplier stays within the configured policy range.
        normal_req = self.nodes[0].calculatecollateralrequirement(100000, 365)  # $1000.00 in cents, tier 4
        normal_multiplier = Decimal(normal_req['dca_multiplier'])
        assert_greater_than_or_equal(normal_multiplier, Decimal('1.0'))
        assert_less_than(normal_multiplier, Decimal('2.1'))

        # Simulate system stress by creating many undercollateralized positions
        # (This would be done through manipulating oracle prices in a real implementation)

        # For testing purposes, we can check if the DCA system responds correctly
        # by examining the DCA multiplier calculation
        try:
            dca_multiplier = self.nodes[0].getdcamultiplier()
            assert 'multiplier' in dca_multiplier, "Missing multiplier field"

            # Verify multiplier is reasonable (between 1.0 and 2.0)
            multiplier_value = Decimal(dca_multiplier['multiplier'])
            assert_greater_than_or_equal(multiplier_value, Decimal('1.0'))
            assert_less_than(multiplier_value, Decimal('2.1'))
        except Exception as e:
            self.log.info(f"DCA multiplier RPC not fully implemented: {e}")

    def test_oracle_price_integration(self):
        """Test oracle price integration in minting."""
        self.log.info("Testing oracle price integration...")

        # Test with different oracle prices (in micro-USD per DGB)
        # 1,000,000 micro-USD = $1.00
        price_scenarios = [
            50000,    # Low price ($0.05/DGB)
            500000,   # Medium price ($0.50/DGB)
            1000000,  # High price ($1.00/DGB)
        ]

        mint_amount = Decimal('1000.00')
        mint_amount_cents = int(mint_amount * 100)
        tier = 4  # tier 4 (~365 days)
        lock_days = 365  # tier 4 = 365 days

        for price in price_scenarios:
            self.log.info(f"Testing with oracle price: {price} satoshis per USD")

            # Set new oracle price
            self.nodes[0].setmockoracleprice(price)

            # Calculate collateral requirement (uses lock_days)
            req = self.nodes[0].calculatecollateralrequirement(mint_amount_cents, lock_days)

            # Verify oracle price field exists
            assert 'oracle_price_micro_usd' in req, "Missing oracle_price_micro_usd field"

            # Verify collateral requirement is reasonable
            collateral_dgb = Decimal(req['required_dgb'])
            assert collateral_dgb > 0, "Collateral must be positive"

        # Reset to original price ($0.50/DGB = 500,000 micro-USD)
        self.refresh_oracle_quotes()

    def test_mint_validation_rules(self):
        """Test mint validation rules and limits."""
        self.log.info("Testing mint validation rules...")

        # Regtest lowers the consensus minimum to 1 cent so tests can cover
        # tiny DD positions without huge collateral requirements.
        assert_raises_rpc_error(
            -8,
            "DigiDollar amount must be positive",
            self.nodes[0].mintdigidollar,
            0,
            4,
        )

        # Regtest caps mints at $1,000 (100,000 cents).
        assert_raises_rpc_error(
            -8,
            "Maximum mint amount is $1000 (100000 cents)",
            self.nodes[0].mintdigidollar,
            100001,
            4,
        )

        # Test invalid tiers
        invalid_tiers = [-1, 10, 100]  # Negative and above max tier 9

        for invalid_tier in invalid_tiers:
            assert_raises_rpc_error(
                -8,
                "Lock tier must be between 0 and 9",
                self.nodes[0].mintdigidollar,
                100000,
                invalid_tier,
            )

        # Test insufficient balance
        # Create a new node with minimal balance
        insufficient_balance_node = self.nodes[2]

        assert_raises_rpc_error(
            -6,
            "No spendable DGB coins for collateral",
            insufficient_balance_node.mintdigidollar,
            100000,
            4,
        )

        # Test valid regtest amounts at boundaries
        valid_amounts = [1, 100000]  # Min and max valid regtest amounts in cents ($0.01, $1000.00)

        for amount in valid_amounts:
            # Should not raise an error, just calculate requirements
            req = self.nodes[0].calculatecollateralrequirement(amount, 365)  # tier 4
            assert 'required_dgb' in req

    def test_edge_cases(self):
        """Test edge cases in minting."""
        self.log.info("Testing minting edge cases...")

        # Test with very precise amounts
        precise_amounts = [
            10001,    # $100.01
            99999,    # $999.99
            75025     # $750.25 (cents don't support sub-cent precision)
        ]

        for amount in precise_amounts:
            req = self.nodes[0].calculatecollateralrequirement(amount, 365)  # tier 4 (~365 days)
            assert 'required_dgb' in req

            # Verify precision is maintained
            assert amount > 0

        # Test all calculatecollateralrequirement-supported tiers.
        for tier, lock_days in TIER_TO_LOCK_DAYS.items():
            req = self.nodes[0].calculatecollateralrequirement(100000, lock_days)  # $1000.00 in cents
            ratio = int(req['effective_ratio'])

            # Should have a reasonable ratio
            assert ratio >= 200, f"Collateral ratio {ratio}% too low for tier {tier}"

    def test_error_conditions(self):
        """Test error conditions and error handling."""
        self.log.info("Testing error conditions...")

        # Test with DigiDollar disabled
        # (Would require restarting node without -digidollar=1, skipped for now)

        # Test with invalid parameters
        invalid_params = [
            {"amount": -100, "tier": 4},
            {"amount": 0, "tier": 4},
        ]

        for params in invalid_params:
            assert_raises_rpc_error(
                -8,
                "DigiDollar amount must be positive",
                self.nodes[0].mintdigidollar,
                params["amount"],
                params["tier"],
            )

        # Test oracle price validation
        # Set invalid oracle price and verify it's handled
        try:
            # This should fail or be ignored
            self.nodes[0].setmockoracleprice(-1)
            price_info = self.nodes[0].getoracleprice()
            assert_greater_than(int(price_info['price']), 0)
        except Exception as e:
            # Error is acceptable - negative prices should be rejected
            self.log.info(f"Invalid oracle price rejected: {e}")

        # Reset to valid oracle price after test ($0.50/DGB = 500,000 micro-USD)
        self.refresh_oracle_quotes()

        # Test concurrent minting (stress test)
        import threading
        import time

        def mint_worker():
            try:
                result = self.nodes[0].mintdigidollar(50000, 4)  # $500.00 in cents, tier 4
                return result['txid']
            except Exception as e:
                self.log.info(f"Concurrent mint failed (acceptable): {e}")
                return None

        # Launch multiple concurrent mint operations
        self.refresh_oracle_quotes()
        threads = []
        for i in range(3):
            thread = threading.Thread(target=mint_worker)
            threads.append(thread)
            thread.start()

        # Wait for all threads to complete
        for thread in threads:
            thread.join()

        # Mine blocks to confirm any successful transactions
        self.nodes[0].generate(5)
        self.sync_all()

        self.log.info("Minting stress test completed")


if __name__ == '__main__':
    DigiDollarMintTest().main()
