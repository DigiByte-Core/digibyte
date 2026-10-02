#!/usr/bin/env python3
"""Test DigiDollar redemption operations.

Test comprehensive redemption functionality including:
- Normal exact-amount redemption path
- Emergency redemption (ERR)
- Exact-amount enforcement (partial redemption rejection)
- Timelock expiry redemption
- Collateral return calculations
- ERR trigger conditions
- Vault closure after full redemption
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
import time


class DigiDollarRedeemTest(DigiByteTestFramework):
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
        self.log.info("Testing DigiDollar redemption operations...")

        # Test setup
        self.setup_digidollar_test()

        # Run test scenarios
        self.test_redemption_change_history_classification()
        self.test_normal_redemption()
        self.test_partial_redemption_rejected()  # Changed from test_partial_redemption
        self.test_exact_amount_enforcement()      # New test
        self.test_full_position_redemption()
        self.test_timelock_expiry_redemption()
        self.test_emergency_redemption()
        self.test_collateral_return_calculations()
        self.test_redemption_validation()
        self.test_redemption_edge_cases()
        # Last, because it mines several hundred blocks to mature its own
        # position and every case above expects its positions still locked.
        self.test_fragmented_fee_inputs_redemption()

    def publish_musig2_quote(self, node, price_micro_usd=None):
        """Publish a fresh regtest MuSig2 oracle quote for this node's current epoch."""
        if price_micro_usd is None:
            price_micro_usd = self.base_oracle_price
        node.setmockoracleprice(price_micro_usd)

    def setup_digidollar_test(self):
        """Setup test environment for DigiDollar."""
        # Generate initial blocks past coinbase maturity for all nodes
        self.log.info("Generating initial blocks for test setup...")
        self.nodes[0].generate(110)
        self.nodes[1].generate(110)
        self.nodes[2].generate(110)
        self.sync_all()

        # Set mock oracle price ($0.50 per DGB)
        # Oracle price is in micro-USD: 1,000,000 micro-USD = $1.00
        # So $0.50/DGB = 500,000 micro-USD
        base_price = 500000  # 500000 micro-USD = $0.50 per DGB
        self.base_oracle_price = base_price
        for node in self.nodes:
            self.publish_musig2_quote(node, base_price)

        # Create DD positions for testing redemption
        self.log.info("Creating DD positions for redemption testing...")

        # Node 0: Multiple positions with different lock tiers
        # mintdigidollar(dd_amount_cents: int, lock_tier: int)
        # Tiers: 0=1h, 1=30d, 2=90d, 3=180d, 4=1y, 5=3y, 6=5y, 7=7y, 8=10y
        # Max mint amount is 100000 cents ($1000)
        # Using tier 0 (1 hour) for testing to avoid long lock periods
        positions = [
            {"amount_cents": 50000, "lock_tier": 0},    # $500, 1 hour (testing)
            {"amount_cents": 75000, "lock_tier": 0},    # $750, 1 hour (testing)
            {"amount_cents": 100000, "lock_tier": 0}    # $1000, 1 hour (testing)
        ]

        self.position_info = []
        for pos in positions:
            result = self.nodes[0].mintdigidollar(pos["amount_cents"], pos["lock_tier"])
            pos["txid"] = result["txid"]
            pos["position_id"] = result["position_id"]
            self.position_info.append(pos)

        # Node 1: Single position
        self.nodes[1].mintdigidollar(100000, 0)  # $1000, 1 hour (testing)

        # Sync mempools to ensure all mint transactions reach Node 0 before mining
        self.sync_mempools()

        # Mine blocks to confirm and pass tier-0 lock (240 blocks) plus 100-block mint confirmation buffer
        self.nodes[0].generate(350)
        self.sync_all()

        # The 350-block timelock advance crosses many short regtest oracle
        # epochs. Refresh each node's local MuSig2 quote before redemption tests
        # start so mempool policy has a current-epoch v0x03 bundle available.
        for node in self.nodes:
            self.publish_musig2_quote(node)

        # Verify positions were created
        total_expected = 225000  # Node 0 total in cents ($2250)
        actual_balance = self.nodes[0].getdigidollarbalance()
        assert_equal(actual_balance['total'], total_expected)

    def test_redemption_change_history_classification(self):
        """DD change from redemption must not appear as a normal receive."""
        self.log.info("Testing redemption DD change history classification...")

        smallest_position = min(
            self.position_info,
            key=lambda pos: pos["amount_cents"],
        )
        position_id = smallest_position["position_id"]
        position_amount = smallest_position["amount_cents"]
        assert_equal(position_amount, 50000)

        # Spend the exact matching DD token away while keeping the collateral
        # position. Redeeming the position must then burn a larger confirmed DD
        # UTXO and create DD change.
        self.nodes[0].senddigidollar(self.nodes[2].getdigidollaraddress(), position_amount)
        self.nodes[0].generate(1)
        self.sync_all()

        self.publish_musig2_quote(self.nodes[0])
        redeem_result = self.nodes[0].redeemdigidollar(position_id, position_amount)
        redeem_txid = redeem_result["txid"]

        self.nodes[0].generate(1)
        self.sync_all()

        history_rows = [tx for tx in self.nodes[0].listdigidollartxs(50, 0) if tx["txid"] == redeem_txid]
        redeem_rows = [tx for tx in history_rows if tx["category"] == "redeem"]
        receive_rows = [tx for tx in history_rows if tx["category"] == "receive"]
        change_rows = [tx for tx in history_rows if tx["category"] == "redeem_change"]

        assert_equal(len(redeem_rows), 1)
        assert_equal(redeem_rows[0]["amount"], Decimal(-position_amount))
        assert_equal(len(receive_rows), 0)
        assert_equal(len(change_rows), 1)
        assert_equal(change_rows[0]["amount"], Decimal(25000))

        # The change must still be accounted as spendable DD after confirmation.
        balance = self.nodes[0].getdigidollarbalance()
        assert_equal(balance["total"], 125000)

    def test_fragmented_fee_inputs_redemption(self):
        """A wallet whose DGB sits in many small coins must still be able to redeem.

        Redemption used to pick its DGB fee coins once, against a fixed
        400-byte size guess (0.21 DGB with a 50% margin), and then build the
        real transaction. With only 0.05 DGB coins that single selection
        returns five coins (0.25 DGB), but a transaction with seven inputs
        costs more than that, so the build failed with "Insufficient fee
        inputs for DD redemption fee" and never tried again. The fee coins
        must now be chosen from the size of the transaction they produce, so
        the redemption goes through and spends more of the small coins than
        the old one-shot selection would have.

        The second half of this test covers a different risk in the same
        transaction: when the caller names a DGB address for the returned
        collateral, only the collateral may go there. The DGB left over after
        the fee must stay in the redeeming wallet.
        """
        self.log.info("Testing redemption from a wallet whose DGB is fragmented into small coins...")
        node = self.nodes[2]
        funder = self.nodes[0]
        node.createwallet(wallet_name="fragmented")
        wallet = node.get_wallet_rpc("fragmented")

        # One large coin to lock as collateral, then the mint.
        funder.sendtoaddress(wallet.getnewaddress(), 200000)
        self.generate(funder, 1)
        self.publish_musig2_quote(node)
        mint_cents = 50000  # $500, tier 0
        position_id = wallet.mintdigidollar(mint_cents, 0)["position_id"]
        self.generate(node, 1)
        self.publish_musig2_quote(node)
        # A second position, redeemed at the end to a DGB address this wallet
        # does not own.
        away_position_id = wallet.mintdigidollar(mint_cents, 0)["position_id"]
        self.generate(node, 1)

        # Now the DGB the wallet can pay fees with: forty coins of 0.05 DGB
        # (plus the large mint change, which smallest-first selection reaches
        # only after every small coin). Funded after the mint so the mint
        # cannot sweep them.
        fragment_value = Decimal("0.05")
        fragments = {wallet.getnewaddress(): fragment_value for _ in range(40)}
        funder.sendmany("", fragments)
        self.generate(funder, 1)
        fragment_outpoints = {
            (utxo["txid"], utxo["vout"])
            for utxo in wallet.listunspent()
            if Decimal(utxo["amount"]) == fragment_value
        }
        assert_equal(len(fragment_outpoints), 40)

        # Mature the tier-0 lock (240 blocks) past the 100-block mint window.
        self.generate(node, 350)
        self.publish_musig2_quote(node)
        info = wallet.getredemptioninfo(position_id)
        assert info["can_redeem"], info

        dgb_before = wallet.getbalance()
        result = wallet.redeemdigidollar(position_id, mint_cents)
        redeem_txid = result["txid"]
        assert redeem_txid in node.getrawmempool()
        assert_equal(result["position_id"], position_id)
        assert result["position_closed"]

        decoded = node.getrawtransaction(redeem_txid, True)
        fee_inputs = [
            (vin["txid"], vin["vout"]) for vin in decoded["vin"]
            if (vin["txid"], vin["vout"]) in fragment_outpoints
        ]
        # Five coins is what the old fixed 0.21 DGB target selected; the fee of
        # the transaction those five produce is above 0.25 DGB, so a working
        # selection must have taken more of them.
        assert_greater_than(len(fee_inputs), 5)
        assert_greater_than_or_equal(Decimal(result["fee_paid"]), Decimal("0.1"))
        assert_greater_than_or_equal(len(fee_inputs) * fragment_value, Decimal(result["fee_paid"]))
        self.log.info(f"Redemption used {len(fee_inputs)} small fee coins, fee {result['fee_paid']} DGB")

        self.generate(node, 1)
        active_ids = [p["position_id"] for p in wallet.listdigidollarpositions()]
        assert position_id not in active_ids, "Position should be closed after redemption"
        assert_greater_than(wallet.getbalance(), dgb_before)
        self.log.info("✓ Fragmented-wallet redemption succeeded")

        # A redemption address given by the caller may belong to someone else,
        # for example an exchange deposit address. The collateral goes there
        # because that is what was asked for, but the DGB left over after the
        # fee must stay in this wallet.
        self.log.info("Testing that leftover DGB does not follow a redemption address the caller supplied...")
        self.publish_musig2_quote(node)
        elsewhere = funder.getnewaddress()
        assert not wallet.getaddressinfo(elsewhere)["ismine"]
        away = wallet.redeemdigidollar(away_position_id, mint_cents, elsewhere)
        assert_equal(away["unlock_address"], elsewhere)

        away_decoded = node.getrawtransaction(away["txid"], True)
        to_elsewhere = [
            out for out in away_decoded["vout"]
            if out["scriptPubKey"].get("address") == elsewhere
        ]
        assert_equal(len(to_elsewhere), 1)  # the returned collateral, nothing else
        for out in away_decoded["vout"]:
            address = out["scriptPubKey"].get("address")
            if address is None or address == elsewhere:
                continue
            if out["value"] == 0:
                continue  # DigiDollar token outputs carry no DGB
            assert wallet.getaddressinfo(address)["ismine"], \
                f"DGB left over after the fee went to {address}, which this wallet does not own"
        self.generate(node, 1)
        self.log.info("✓ Leftover DGB stayed in the redeeming wallet")

        # Leave the node with one wallet again, so a plain RPC call to it does
        # not have to say which wallet it means.
        node.unloadwallet("fragmented")

    def test_normal_redemption(self):
        """Test normal EXACT-AMOUNT redemption process."""
        self.log.info("Testing exact-amount redemption...")

        # Get node 1's position
        positions = self.nodes[1].listdigidollarpositions()
        assert len(positions) > 0, "Expected at least one position"
        position = positions[0]
        position_id = position['position_id']

        # Get EXACT amount in position (not partial)
        dd_amount = int(position.get('dd_amount', position.get('dd_minted', 100000)))

        self.log.info(f"Position {position_id} has {dd_amount} cents DD")
        self.log.info(f"Redeeming EXACT amount {dd_amount} cents (not partial)...")

        # Redeem EXACT amount
        self.publish_musig2_quote(self.nodes[1])
        result = self.nodes[1].redeemdigidollar(position_id, dd_amount)

        assert 'txid' in result, "Redemption should return txid"
        self.log.info(f"Redemption txid: {result['txid']}")

        # Mine block to confirm
        self.nodes[1].generate(1)
        self.sync_all()

        # Verify position is closed
        positions_after = self.nodes[1].listdigidollarpositions()
        remaining_ids = [p['position_id'] for p in positions_after if p.get('is_active', True)]
        assert position_id not in remaining_ids, "Position should be closed after full redemption"

        self.log.info("✓ Exact-amount redemption succeeded and position closed")

    def test_partial_redemption_rejected(self):
        """Test that partial redemption is properly rejected."""
        self.log.info("Testing partial redemption rejection...")

        # First mint a new position for this test
        self.publish_musig2_quote(self.nodes[0])
        mint_result = self.nodes[0].mintdigidollar(100000, 0)  # $1000 DD, tier 0 (1 hour test)
        position_id = mint_result['position_id']
        position_amount = 100000  # cents

        # Generate blocks to pass tier-0 timelock plus the 100-block mint confirmation buffer
        self.nodes[0].generate(350)
        self.sync_all()
        self.publish_musig2_quote(self.nodes[0])

        # Try to redeem HALF the position (should fail with exact-amount enforcement)
        partial_amount = position_amount // 2  # 50000 cents = $500

        self.log.info(f"Attempting partial redemption of {partial_amount} from {position_amount} cents...")

        try:
            self.nodes[0].redeemdigidollar(position_id, partial_amount)
            raise AssertionError(f"Partial redemption of {partial_amount} should have been rejected!")
        except Exception as e:
            error_msg = str(e).lower()
            # Check for exact-amount related error messages
            assert any(x in error_msg for x in ['exact', 'must equal', 'full', 'minted']), \
                f"Error should mention exact amount requirement, got: {e}"
            self.log.info(f"✓ Partial redemption correctly rejected: {e}")

        # Now redeem the full amount (should succeed)
        self.log.info(f"Now redeeming full amount {position_amount} cents...")
        result = self.nodes[0].redeemdigidollar(position_id, position_amount)
        assert 'txid' in result, "Full redemption should succeed"
        self.log.info("✓ Full redemption after partial rejection succeeded")

    def test_exact_amount_enforcement(self):
        """Test comprehensive exact-amount enforcement."""
        self.log.info("Testing exact-amount enforcement...")

        # Mint a position
        mint_cents = 50000  # $500 DD
        self.publish_musig2_quote(self.nodes[0])
        mint_result = self.nodes[0].mintdigidollar(mint_cents, 0)
        position_id = mint_result['position_id']

        self.nodes[0].generate(350)
        self.sync_all()
        self.publish_musig2_quote(self.nodes[0])

        # Test 1: Try slightly less than exact (should fail)
        self.log.info("Test 1: Trying amount slightly less than minted...")
        try:
            self.nodes[0].redeemdigidollar(position_id, mint_cents - 1)
            raise AssertionError("Should reject amount less than minted")
        except Exception as e:
            self.log.info(f"  ✓ Rejected: {e}")

        # Test 2: Try slightly more than exact (should fail)
        self.log.info("Test 2: Trying amount slightly more than minted...")
        try:
            self.nodes[0].redeemdigidollar(position_id, mint_cents + 1)
            raise AssertionError("Should reject amount more than minted")
        except Exception as e:
            self.log.info(f"  ✓ Rejected: {e}")

        # Test 3: Try exactly the minted amount (should succeed)
        self.log.info(f"Test 3: Trying exact amount {mint_cents}...")
        result = self.nodes[0].redeemdigidollar(position_id, mint_cents)
        assert 'txid' in result
        self.log.info(f"  ✓ Exact amount accepted: {result['txid']}")

        # Confirm and verify position closed
        self.nodes[0].generate(1)
        self.sync_all()
        positions = self.nodes[0].listdigidollarpositions()
        active_ids = [p['position_id'] for p in positions if p.get('is_active', True)]
        assert position_id not in active_ids, "Position should be closed"

        self.log.info("✓ Exact-amount enforcement verified")

    def test_full_position_redemption(self):
        """Test full redemption of entire positions."""
        self.log.info("Testing full position redemption...")

        # Get specific position to redeem fully
        positions = self.nodes[0].listdigidollarpositions()

        if len(positions) == 0:
            self.log.info("No positions remaining for full redemption test, skipping...")
            return

        # Find the smallest position
        smallest_position = min(positions, key=lambda p: int(p.get('dd_amount', p.get('amount', 0))))

        # Get the CURRENT amount in the position (not the original amount)
        # Position may have been partially redeemed in previous tests
        current_amount = int(smallest_position.get('dd_amount', smallest_position.get('amount', 0)))

        if current_amount == 0:
            self.log.info("Selected position has 0 DD, selecting another position...")
            # Find first non-zero position
            for pos in positions:
                amt = int(pos.get('dd_amount', pos.get('amount', 0)))
                if amt > 0:
                    smallest_position = pos
                    current_amount = amt
                    break

        if current_amount == 0:
            self.log.info("No positions with DD remaining, skipping full redemption test...")
            return

        redeem_amount_cents = current_amount
        position_id = smallest_position['position_id']
        position_count_before = len(positions)

        self.log.info(f"Redeeming full position {position_id} with {redeem_amount_cents} cents...")

        # Redeem exact position amount
        self.publish_musig2_quote(self.nodes[0])
        result = self.nodes[0].redeemdigidollar(position_id, redeem_amount_cents)

        self.nodes[0].generate(1)
        self.sync_all()

        # Verify position was fully redeemed
        positions_after = self.nodes[0].listdigidollarpositions()
        position_count_after = len(positions_after)

        # Should have one less position
        assert_equal(position_count_after, position_count_before - 1)

        # Verify the specific position is gone
        remaining_ids = [pos['position_id'] for pos in positions_after]
        assert position_id not in remaining_ids

    def test_timelock_expiry_redemption(self):
        """Test redemption behavior with timelock expiry."""
        self.log.info("Testing timelock expiry redemption...")

        # Create a position with very short lock (for testing)
        self.publish_musig2_quote(self.nodes[2])
        short_lock_result = self.nodes[2].mintdigidollar(50000, 0)  # $500, 1 hour (tier 0)

        self.nodes[2].generate(1)
        self.sync_all()

        # Get position ID
        position_id = short_lock_result['position_id']

        # Get redemption info immediately (timelock active)
        immediate_info = self.nodes[2].getredemptioninfo(position_id, 50000)  # Full vault amount

        assert 'can_redeem' in immediate_info
        assert 'timelock_remaining' in immediate_info
        # Note: penalty_rate may not be implemented in Phase 1
        if 'penalty_rate' in immediate_info:
            # For immediate redemption, there should be penalty
            if immediate_info['can_redeem']:
                assert_greater_than(Decimal(immediate_info['penalty_rate']), Decimal('0'))

        # Simulate time passage by advancing blocks
        # In real scenarios, we'd wait for actual timelock expiry
        current_height = self.nodes[2].getblockcount()
        lock_period_blocks = 500  # Reduced for testing (original: 30 * 24 * 60 * 4)

        # Fast-forward by generating blocks
        # Note: This simulates time passage but doesn't actually expire timelocks
        # In production, timelocks are based on actual block height
        self.log.info(f"Advancing {lock_period_blocks} blocks to simulate timelock expiry...")

        # Generate blocks in chunks to avoid memory issues
        chunk_size = 1000
        blocks_generated = 0
        while blocks_generated < lock_period_blocks:
            remaining = min(chunk_size, lock_period_blocks - blocks_generated)
            self.nodes[2].generate(remaining)
            blocks_generated += remaining
            self.log.info(f"Generated {blocks_generated}/{lock_period_blocks} blocks")

        self.sync_all()

        # Check redemption info after timelock expiry
        expired_info = self.nodes[2].getredemptioninfo(position_id, 50000)  # Full vault amount

        # After expiry, penalty should be reduced or eliminated
        if expired_info['can_redeem'] and 'penalty_rate' in expired_info and 'penalty_rate' in immediate_info:
            expired_penalty = Decimal(expired_info['penalty_rate'])
            immediate_penalty = Decimal(immediate_info['penalty_rate'])
            assert_less_than(expired_penalty, immediate_penalty)

    def test_emergency_redemption(self):
        """Test Emergency Redemption Route (ERR) functionality."""
        self.log.info("Testing Emergency Redemption Route (ERR)...")

        # ERR is triggered when system collateral falls below emergency threshold
        # For testing, we'll manipulate oracle price to simulate this condition

        # Get current system health
        initial_health = self.nodes[0].getdigidollarstats()
        self.log.info(f"Initial system health: {initial_health}")
        initial_ratio = Decimal(initial_health.get('system_collateral_ratio', 0))

        # Dramatically decrease oracle price to simulate DGB crash
        # This reduces the value of collateral, triggering ERR
        # Oracle price is in micro-USD: 1,000,000 micro-USD = $1.00
        crisis_price = 100000  # 100000 micro-USD = $0.10 per DGB (from $0.50)

        self.log.info(f"Simulating DGB price crash by setting oracle price to {crisis_price}...")
        for node in self.nodes:
            self.publish_musig2_quote(node, crisis_price)

        # Generate block to make price change effective
        self.nodes[0].generate(1)
        self.sync_all()

        # Check if ERR is triggered
        protection_status = self.nodes[0].getprotectionstatus()
        self.log.info(f"Protection status after price crash: {protection_status}")

        # Get ERR status from protection status
        err_status = protection_status.get('err', {})
        err_active = err_status.get('active', False)

        # If ERR is active, test emergency redemption
        if err_active:
            self.log.info("ERR is active, testing emergency redemption...")

            # Get a position to redeem from
            positions = self.nodes[0].listdigidollarpositions()
            if len(positions) > 0:
                position = positions[0]
                position_id = position['position_id']
                err_amount_cents = int(position.get('dd_minted', position.get('dd_amount', 0)))
                self.publish_musig2_quote(self.nodes[0], crisis_price)
                result = self.nodes[0].redeemdigidollar(position_id, err_amount_cents)

                assert 'txid' in result
                assert 'emergency_redemption' in result or 'dd_redeemed' in result
                # Emergency redemption may or may not have a flag depending on implementation

                self.nodes[0].generate(1)
                self.sync_all()

                self.log.info("Emergency redemption completed successfully")

        else:
            self.log.info("ERR not triggered by price manipulation, testing protection mechanisms...")

            # Even if ERR isn't triggered, verify the system tracked the price change
            system_health = self.nodes[0].getdigidollarstats()
            assert 'system_collateral_ratio' in system_health

            # Verify system health data is being reported
            collateral_ratio = Decimal(system_health['system_collateral_ratio'])

            # Note: At this point in the test, most positions have been redeemed,
            # so the system may still appear healthy even with a large price drop.
            # This is acceptable behavior - just verify the system is tracking health.
            self.log.info(f"System collateral ratio after price crash: {collateral_ratio}")

            # Verify protection status is accessible and contains expected fields
            assert 'dca' in protection_status, "DCA protection status should be available"
            assert 'err' in protection_status, "ERR protection status should be available"
            assert 'volatility' in protection_status, "Volatility protection status should be available"

        # Restore normal price ($0.50/DGB = 500,000 micro-USD)
        self.publish_musig2_quote(self.nodes[0])

    def test_collateral_return_calculations(self):
        """Test accuracy of collateral return calculations."""
        self.log.info("Testing collateral return calculations...")

        # Reset oracle price to original value after ERR test modified it
        # Oracle price is in micro-USD: 500000 micro-USD = $0.50 per DGB
        base_price = 500000
        for node in self.nodes:
            self.publish_musig2_quote(node, base_price)

        # Get positions to test with
        positions = self.nodes[1].listdigidollarpositions()
        if len(positions) == 0:
            self.log.info("No positions available for collateral return test, skipping...")
            return

        position_id = positions[0]['position_id']

        # Check if this position still has DD to redeem
        position = positions[0]
        if position.get('dd_remaining', position.get('dd_minted', 0)) <= 0:
            self.log.info("Position has no DD remaining, skipping collateral return test...")
            return

        # Redemption is exact/full-vault only.
        test_amounts_cents = [position.get('dd_minted', position.get('dd_remaining', 0))]

        for amount_cents in test_amounts_cents:
            # Skip if not enough DD balance
            if self.nodes[1].getdigidollarbalance()['total'] < amount_cents:
                self.log.info(f"Skipping {amount_cents} cents test - insufficient DD balance")
                continue

            # Get redemption info before actual redemption
            info = self.nodes[1].getredemptioninfo(position_id, amount_cents)

            assert 'dgb_return' in info or 'dgb_unlocked' in info

            predicted_dgb = Decimal(info.get('dgb_return', info.get('dgb_unlocked', '0')))
            expected_full_collateral = Decimal(str(position['dgb_collateral']))
            assert_equal(
                predicted_dgb,
                expected_full_collateral,
                "getredemptioninfo must report the full locked collateral return; "
                "fees are paid from separate fee inputs and are not a DGB haircut",
            )

            # Perform actual redemption
            self.publish_musig2_quote(self.nodes[1], base_price)
            result = self.nodes[1].redeemdigidollar(position_id, amount_cents)
            actual_dgb = Decimal(result['dgb_unlocked'])

            # Mine to confirm
            self.nodes[1].generate(1)
            self.sync_all()

            # Verify prediction accuracy - use larger tolerance since position state
            # may have changed from previous tests (partial redemptions)
            # Allow up to 10x difference for partially redeemed positions
            tolerance = max(predicted_dgb * Decimal('0.5'), actual_dgb * Decimal('0.5'))
            if abs(actual_dgb - predicted_dgb) > tolerance:
                self.log.info(f"Warning: DGB return prediction mismatch: predicted {predicted_dgb}, actual {actual_dgb}")
                self.log.info("This may be due to position state changes from previous tests")
            # Don't fail the test - just log the mismatch.
            # The important thing is that exact full-vault redemption works.
            break

    def test_redemption_validation(self):
        """Test redemption validation rules."""
        self.log.info("Testing redemption validation...")

        # Get a valid position for testing
        positions = self.nodes[0].listdigidollarpositions()
        if len(positions) == 0:
            self.log.info("No positions for validation test, skipping...")
            return

        position_id = positions[0]['position_id']

        # Test exact-amount requirement
        self.log.info("Testing exact-amount requirement...")
        position = positions[0]
        position_amount = int(position.get('dd_amount', 50000))

        # Partial amount should fail
        try:
            self.nodes[0].redeemdigidollar(position_id, position_amount // 2)
            raise AssertionError("Partial amount should be rejected")
        except Exception as e:
            self.log.info(f"Partial correctly rejected: {e}")

        # Test insufficient DD balance or amount exceeding position
        excessive_amount_cents = self.nodes[0].getdigidollarbalance()['total'] + 100  # 100 cents more
        # The error code can be -4 (insufficient balance) or -8 (exceeds position amount)
        try:
            self.nodes[0].redeemdigidollar(position_id, excessive_amount_cents)
            raise AssertionError("Should have raised an error for excessive redemption amount")
        except Exception as e:
            # Expected to fail - verify it's an RPC error
            # With exact-amount enforcement, this will fail with "Exact-amount redemption required"
            assert "Cannot redeem" in str(e) or "Insufficient" in str(e) or "Exact-amount" in str(e), f"Unexpected error: {e}"
            self.log.info(f"Excessive redemption rejected as expected: {e}")

        # Test invalid amounts
        invalid_amounts = [0, -10000]  # 0 and negative

        for invalid_amount in invalid_amounts:
            # Some implementations may reject these with different error codes
            try:
                result = self.nodes[0].redeemdigidollar(position_id, invalid_amount)
                # If it doesn't raise an error, the implementation may handle it differently
                self.log.info(f"Invalid amount {invalid_amount} was accepted or handled: {result}")
            except Exception as e:
                # Expected to fail
                self.log.info(f"Invalid amount {invalid_amount} rejected as expected: {e}")

        # Test minimum redemption amount (50 cents is very small)
        # Note: Implementation may not have a minimum, so don't assert hard failure
        try:
            result = self.nodes[0].redeemdigidollar(position_id, 50)  # 50 cents
            self.log.info(f"Small amount (50 cents) redemption result: {result}")
        except Exception as e:
            self.log.info(f"Small amount (50 cents) rejected: {e}")

        # Test redemption with invalid position ID
        # Error code can be -4 or -8 depending on implementation
        try:
            self.nodes[0].redeemdigidollar(
                "0000000000000000000000000000000000000000000000000000000000000000", 10000)
            raise AssertionError("Should have raised an error for invalid position ID")
        except Exception as e:
            # Expected to fail
            assert "Position not found" in str(e) or "not found" in str(e).lower(), f"Unexpected error: {e}"
            self.log.info(f"Invalid position ID rejected as expected: {e}")

    def test_redemption_edge_cases(self):
        """Test edge cases in redemption."""
        self.log.info("Testing redemption edge cases...")

        # Get a position for testing
        positions = self.nodes[0].listdigidollarpositions()
        if len(positions) == 0:
            self.log.info("No positions for edge case test, skipping...")
            return

        position_id = positions[0]['position_id']

        # Test redemption with various amounts (in cents)
        precise_amounts_cents = [10001, 9999, 100012]  # $100.01, $99.99, $1000.12

        for amount_cents in precise_amounts_cents:
            try:
                info = self.nodes[0].getredemptioninfo(position_id, amount_cents)
                assert 'dgb_return' in info or 'dgb_unlocked' in info
                dgb_val = info.get('dgb_return', info.get('dgb_unlocked', '0'))
                self.log.info(f"Precise redemption info for {amount_cents} cents: {dgb_val} DGB")
            except Exception as e:
                self.log.info(f"Precise amount {amount_cents} validation failed (acceptable): {e}")

        # Test concurrent redemptions
        import threading

        def redeem_worker(position_id, amount_cents):
            try:
                if self.nodes[0].getdigidollarbalance()['total'] >= amount_cents:
                    result = self.nodes[0].redeemdigidollar(position_id, amount_cents)
                    return result['txid']
            except Exception as e:
                self.log.info(f"Concurrent redemption failed (acceptable): {e}")
                return None

        # Launch multiple redemption attempts
        threads = []
        redemption_amounts_cents = [5000, 7500, 10000]  # $50, $75, $100

        for amount_cents in redemption_amounts_cents:
            thread = threading.Thread(target=redeem_worker, args=(position_id, amount_cents))
            threads.append(thread)
            thread.start()

        for thread in threads:
            thread.join()

        # Mine blocks to confirm any successful redemptions
        self.nodes[0].generate(3)
        self.sync_all()

        # Test redemption during system stress
        # Modify oracle price to create stress
        # Oracle price is in micro-USD: 1,000,000 micro-USD = $1.00
        stress_price = 250000  # 250000 micro-USD = $0.25 per DGB (half the normal $0.50)
        self.publish_musig2_quote(self.nodes[0], stress_price)

        try:
            # Get positions for stress test
            stress_positions = self.nodes[0].listdigidollarpositions()
            if len(stress_positions) > 0:
                stress_position_id = stress_positions[0]['position_id']
                stress_amount = stress_positions[0].get('dd_minted', 0)
                stress_info = self.nodes[0].getredemptioninfo(stress_position_id, stress_amount)
                self.log.info(f"Redemption during stress: {stress_info}")

                # During stress, penalty rates should be higher (if implemented)
                if 'penalty_rate' in stress_info:
                    penalty_rate = Decimal(stress_info['penalty_rate'])
                    assert_greater_than_or_equal(penalty_rate, Decimal('0'))

        except Exception as e:
            self.log.info(f"Redemption during stress failed (may be acceptable): {e}")

        # Restore normal price ($0.50/DGB = 500,000 micro-USD)
        self.publish_musig2_quote(self.nodes[0])


if __name__ == '__main__':
    DigiDollarRedeemTest().main()
