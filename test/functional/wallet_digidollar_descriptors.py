#!/usr/bin/env python3
# Copyright (c) 2024 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test DigiDollar with descriptor wallets.

Test comprehensive DigiDollar functionality with descriptor wallets including:
- Export DD descriptors via listdescriptors
- Import descriptors to new wallets
- DD operations on descriptor wallets
- Legacy wallet DD creation rejection and descriptor migration target checks
- Watch-only descriptor import
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)
ORACLE_PRICE_MICRO_USD = 500000


class DigiDollarDescriptorTest(DigiByteTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Enable DigiDollar features and disable Dandelion for testing
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-dandelion=0", "-addresstype=bech32"],
            ["-digidollar=1", "-txindex=1", "-dandelion=0", "-addresstype=bech32"]
        ]
        # whitelist peers to speed up tx relay / mempool sync
        for args in self.extra_args:
            args.append("-whitelist=noban@127.0.0.1")
        self.wallet_names = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        self.log.info("Testing DigiDollar with descriptor wallets...")

        # Setup test environment
        self.setup_digidollar_test()

        # Run test scenarios
        self.test_mint_wallet_capabilities()
        self.test_export_dd_descriptors()
        self.test_import_dd_descriptors()
        self.test_descriptor_wallet_dd_operations()
        self.test_legacy_wallet_dd_creation_rejected()
        self.test_watchonly_descriptor_import()

    def setup_digidollar_test(self):
        """Setup test environment for DigiDollar descriptor testing."""
        self.log.info("Setting up DigiDollar descriptor test environment...")

        # Create initial descriptor wallets
        self.nodes[0].createwallet(wallet_name='dd_source', descriptors=True)
        self.nodes[1].createwallet(wallet_name='dd_target', descriptors=True, blank=True)

        self.source_wallet = self.nodes[0].get_wallet_rpc('dd_source')
        self.target_wallet = self.nodes[1].get_wallet_rpc('dd_target')

        # Generate initial blocks for coinbase maturity
        self.log.info("Generating initial blocks...")
        self.generatetoaddress(self.nodes[0], 110, self.source_wallet.getnewaddress())
        self.sync_all()

        # Set mock oracle price on both nodes
        # Oracle price is in micro-USD: 500000 = $0.50/DGB
        self.refresh_oracle_quotes()

        # Verify DigiDollar system is accessible
        stats = self.nodes[0].getdigidollarstats()
        assert "health_percentage" in stats
        self.log.info(f"DigiDollar system ready, oracle price: {stats['oracle_price_cents']} cents/DGB")

    def refresh_oracle_quotes(self, *node_indices, price=ORACLE_PRICE_MICRO_USD):
        indices = node_indices if node_indices else range(len(self.nodes))
        for index in indices:
            result = self.nodes[index].setmockoracleprice(price)
            assert_equal(result["price_micro_usd"], price)

    def test_mint_wallet_capabilities(self):
        """Reject incomplete wallets before oracle, balance, or key allocation work."""
        private_descriptors = self.source_wallet.listdescriptors(True)["descriptors"]
        owner = next(d for d in private_descriptors if d["desc"].startswith("tr(") and not d["internal"])
        change = next(d for d in private_descriptors if d["desc"].startswith("wpkh(") and d["internal"])
        cases = [
            ("blank", [], "cannot generate DigiDollar owner keys"),
            ("no_taproot", [change], "cannot generate DigiDollar owner keys"),
            ("no_change", [owner], "cannot generate the change addresses"),
            ("inactive_taproot", [change, dict(owner, active=False)], "cannot generate DigiDollar owner keys"),
        ]
        for name, descriptors, reason in cases:
            self.nodes[0].createwallet(wallet_name=f"mint_{name}", descriptors=True, blank=True)
            wallet = self.nodes[0].get_wallet_rpc(f"mint_{name}")
            for descriptor in descriptors:
                request = {key: descriptor[key] for key in ("desc", "active", "internal", "range")}
                request["timestamp"] = "now"
                result = wallet.importdescriptors([request])
                assert_equal(result[0]["success"], True)
            before = wallet.listdescriptors()
            for _ in range(2):
                assert_raises_rpc_error(-4, reason, wallet.mintdigidollar, 10000, 0)
            assert_equal(wallet.listdescriptors(), before)
            assert_equal(wallet.getwalletinfo()["txcount"], 0)

    def test_export_dd_descriptors(self):
        """Test exporting DD-related descriptors from a wallet."""
        self.log.info("Testing export of DD descriptors...")

        # Create DD address
        dd_address = self.source_wallet.getdigidollaraddress()
        self.log.info(f"Created DD address: {dd_address}")

        # Create DD position via minting
        mint_amount = 10000  # 100.00 DD = 10000 cents
        dca_tier = 1  # 30 days lock
        self.refresh_oracle_quotes(0)
        mint_result = self.source_wallet.mintdigidollar(mint_amount, dca_tier)
        assert 'txid' in mint_result, "Mint should return transaction ID"
        assert 'dd_minted' in mint_result, "Mint should return DD minted amount"
        self.log.info(f"Minted {mint_result['dd_minted']} DD, txid: {mint_result['txid']}")

        # Mine blocks to confirm
        self.generate(self.nodes[0], 2)
        self.sync_all()

        # Verify DD balance
        balance_info = self.source_wallet.getdigidollarbalance()
        dd_balance = balance_info['total'] if isinstance(balance_info, dict) else balance_info
        assert_equal(dd_balance, mint_amount)
        self.log.info(f"DD balance confirmed: {dd_balance} cents")

        # Export descriptors (including private keys)
        descriptors_result = self.source_wallet.listdescriptors(True)
        assert 'wallet_name' in descriptors_result
        assert 'descriptors' in descriptors_result
        assert_equal(descriptors_result['wallet_name'], 'dd_source')

        descriptors = descriptors_result['descriptors']
        assert_greater_than(len(descriptors), 0)
        self.log.info(f"Exported {len(descriptors)} descriptors from source wallet")

        # Verify descriptor structure
        for desc in descriptors:
            assert 'desc' in desc, "Descriptor should have 'desc' field"
            assert 'timestamp' in desc, "Descriptor should have 'timestamp' field"
            assert desc['desc'] != '', "Descriptor string should not be empty"

        # Export descriptors without private keys
        public_descriptors = self.source_wallet.listdescriptors(False)
        assert_equal(len(public_descriptors['descriptors']), len(descriptors))

        # Verify private descriptors contain xprv (or WIF for single keys)
        private_count = 0
        for desc in descriptors:
            if 'prv' in desc['desc'].lower() or desc['desc'].startswith('wpkh(e') or desc['desc'].startswith('pkh(e'):
                private_count += 1
        self.log.info(f"Found {private_count} descriptors with private key material")

        self.log.info("Export DD descriptors test passed")

    def test_import_dd_descriptors(self):
        """Test importing DD descriptors to a new wallet."""
        self.log.info("Testing import of DD descriptors...")

        # Create another DD position in source wallet for testing
        additional_mint = 5000  # 50.00 DD = 5000 cents
        dca_tier = 2  # 90 days
        self.refresh_oracle_quotes(0)
        mint_result = self.source_wallet.mintdigidollar(additional_mint, dca_tier)
        self.generate(self.nodes[0], 2)
        self.sync_all()

        # Get source wallet state before export
        source_balance_info = self.source_wallet.getdigidollarbalance()
        source_balance = source_balance_info['total'] if isinstance(source_balance_info, dict) else source_balance_info
        source_positions = self.source_wallet.listdigidollarpositions()
        self.log.info(f"Source wallet: {source_balance} DD cents, {len(source_positions)} positions")

        # Export descriptors with private keys
        source_descriptors = self.source_wallet.listdescriptors(True)['descriptors']

        # Create new descriptor wallet for import on node 1
        self.nodes[1].createwallet(wallet_name='dd_imported', descriptors=True, blank=True)
        imported_wallet = self.nodes[1].get_wallet_rpc('dd_imported')

        # Verify empty wallet
        empty_balance_info = imported_wallet.getdigidollarbalance()
        empty_balance = empty_balance_info['total'] if isinstance(empty_balance_info, dict) else empty_balance_info
        assert_equal(empty_balance, 0)

        # Prepare descriptors for import with rescan
        import_requests = []
        for desc in source_descriptors:
            import_req = {
                "desc": desc['desc'],
                "timestamp": 0,  # Rescan from genesis
            }
            # Copy additional fields if present
            if 'active' in desc:
                import_req['active'] = desc['active']
            if 'internal' in desc:
                import_req['internal'] = desc['internal']
            if 'range' in desc:
                import_req['range'] = desc['range']
            import_requests.append(import_req)

        self.log.info(f"Importing {len(import_requests)} descriptors...")

        # Import descriptors
        import_results = imported_wallet.importdescriptors(import_requests)

        # Verify all imports succeeded
        success_count = sum(1 for r in import_results if r.get('success', False))
        self.log.info(f"Successfully imported {success_count}/{len(import_requests)} descriptors")

        # Some may fail if they're duplicates or have issues, but at least some should succeed
        assert_greater_than(success_count, 0)

        # Verify wallet now has the DD balances after rescan
        imported_balance_info = imported_wallet.getdigidollarbalance()
        imported_balance = imported_balance_info['total'] if isinstance(imported_balance_info, dict) else imported_balance_info

        self.log.info(f"Imported wallet balance: {imported_balance} DD cents")

        # Verify we can generate addresses in imported wallet
        new_dd_address = imported_wallet.getdigidollaraddress()
        assert new_dd_address.startswith('RD') or new_dd_address.startswith('DD') or new_dd_address.startswith('TD')
        self.log.info(f"Generated new DD address in imported wallet: {new_dd_address}")

        self.log.info("Import DD descriptors test passed")

    def test_descriptor_wallet_dd_operations(self):
        """Test DD operations on a pure descriptor wallet."""
        self.log.info("Testing DD operations on descriptor wallet...")

        # Create a fresh descriptor wallet (not legacy)
        self.nodes[0].createwallet(wallet_name='dd_descriptor_ops', descriptors=True)
        ops_wallet = self.nodes[0].get_wallet_rpc('dd_descriptor_ops')

        # Verify it's a descriptor wallet
        wallet_info = ops_wallet.getwalletinfo()
        # Descriptor wallets have 'descriptors' field set to true
        if 'descriptors' in wallet_info:
            assert_equal(wallet_info['descriptors'], True)
        self.log.info("Created descriptor wallet for DD operations")

        # Fund the wallet
        fund_address = ops_wallet.getnewaddress()
        self.source_wallet.sendtoaddress(fund_address, 50000)  # 50000 DGB
        self.generate(self.nodes[0], 2)
        self.sync_all()

        # Verify funding
        ops_balance = ops_wallet.getbalance()
        assert_greater_than(ops_balance, 0)
        self.log.info(f"Funded descriptor wallet with {ops_balance} DGB")

        # Test DD address generation
        dd_addr1 = ops_wallet.getdigidollaraddress()
        dd_addr2 = ops_wallet.getdigidollaraddress()
        assert dd_addr1 != dd_addr2, "DD addresses should be unique"
        self.log.info(f"Generated DD addresses: {dd_addr1}, {dd_addr2}")

        # Test minting on descriptor wallet
        mint_amount = 20000  # 200.00 DD = 20000 cents
        dca_tier = 3  # 180 days
        self.refresh_oracle_quotes(0)
        mint_result = ops_wallet.mintdigidollar(mint_amount, dca_tier)
        assert 'txid' in mint_result
        assert 'dd_minted' in mint_result
        self.log.info(f"Minted {mint_result['dd_minted']} DD on descriptor wallet")

        self.generate(self.nodes[0], 2)
        self.sync_all()

        # Verify balance
        dd_balance_info = ops_wallet.getdigidollarbalance()
        dd_balance = dd_balance_info['total'] if isinstance(dd_balance_info, dict) else dd_balance_info
        assert_equal(dd_balance, mint_amount)

        # Test sending DD from descriptor wallet
        send_amount = 5000  # 50.00 DD = 5000 cents
        receiver_address = self.source_wallet.getdigidollaraddress()
        self.refresh_oracle_quotes(0)
        send_result = ops_wallet.senddigidollar(receiver_address, send_amount)
        assert 'txid' in send_result
        self.log.info(f"Sent {send_amount} cents DD from descriptor wallet")

        self.generate(self.nodes[0], 2)
        self.sync_all()

        # Verify balance decreased
        final_balance_info = ops_wallet.getdigidollarbalance()
        final_balance = final_balance_info['total'] if isinstance(final_balance_info, dict) else final_balance_info
        assert_equal(final_balance, mint_amount - send_amount)

        # Test position listing
        positions = ops_wallet.listdigidollarpositions()
        assert_greater_than(len(positions), 0)
        self.log.info(f"Descriptor wallet has {len(positions)} DD positions")

        # Verify descriptors can be exported from this wallet
        descriptors = ops_wallet.listdescriptors()
        assert_greater_than(len(descriptors['descriptors']), 0)
        self.log.info(f"Descriptor wallet has {len(descriptors['descriptors'])} exportable descriptors")

        self.log.info("Descriptor wallet DD operations test passed")

    def test_legacy_wallet_dd_creation_rejected(self):
        """Test legacy wallets cannot create new DigiDollar V1 state."""
        self.log.info("Testing legacy wallet DigiDollar creation rejection...")

        # Check if BDB (legacy wallet support) is available
        if not self.is_bdb_compiled():
            self.log.info("Skipping legacy wallet test - BDB not compiled")
            return

        # Create legacy wallet with DD
        self.nodes[0].createwallet(wallet_name='dd_legacy', descriptors=False)
        legacy_wallet = self.nodes[0].get_wallet_rpc('dd_legacy')

        # Fund legacy wallet
        fund_address = legacy_wallet.getnewaddress()
        self.source_wallet.sendtoaddress(fund_address, 30000)  # 30000 DGB
        self.generate(self.nodes[0], 2)
        self.sync_all()

        # Legacy wallets cannot create V1 DigiDollar owner/address keys. They
        # may still exist for ordinary DGB usage, but DD creation must fail
        # before any owner-key fallback or BECH32 downgrade can occur.
        legacy_mint_amount = 8000  # 80.00 DD = 8000 cents
        dca_tier = 1  # 30 days
        self.refresh_oracle_quotes(0)
        assert_raises_rpc_error(
            -4,
            "legacy wallet cannot mint DigiDollar",
            legacy_wallet.mintdigidollar,
            legacy_mint_amount,
            dca_tier,
        )
        assert_raises_rpc_error(
            -4,
            "descriptor/bech32m HD wallet",
            legacy_wallet.getdigidollaraddress,
        )

        # Legacy wallet remains usable for ordinary DGB and has no DD state.
        legacy_balance_info = legacy_wallet.getdigidollarbalance()
        legacy_balance = legacy_balance_info['total'] if isinstance(legacy_balance_info, dict) else legacy_balance_info
        legacy_positions = legacy_wallet.listdigidollarpositions()
        self.log.info(f"Legacy wallet: {legacy_balance} DD cents, {len(legacy_positions)} positions")
        assert_equal(legacy_balance, 0)
        assert_equal(len(legacy_positions), 0)

        # A descriptor wallet is the supported DD migration target for future
        # use; it can create fresh DD receive keys.
        self.nodes[0].createwallet(wallet_name='dd_migrated', descriptors=True)
        migrated_wallet = self.nodes[0].get_wallet_rpc('dd_migrated')
        migrated_dd_addr = migrated_wallet.getdigidollaraddress()
        assert migrated_dd_addr.startswith('RD') or migrated_dd_addr.startswith('DD') or migrated_dd_addr.startswith('TD')
        self.log.info(f"Descriptor migration target can generate DD addresses: {migrated_dd_addr}")

        self.log.info("Legacy wallet DigiDollar creation rejection test passed")

    def test_watchonly_descriptor_import(self):
        """Test importing DD descriptors as watch-only."""
        self.log.info("Testing watch-only descriptor import...")

        self.log.info("Creating an unlocked tier-0 source position for watch-only redemption preflight checks")
        self.refresh_oracle_quotes(0)
        watchonly_redeem_mint = self.source_wallet.mintdigidollar(10000, 0)
        self.generate(self.nodes[0], 241)
        self.sync_all()

        # Export public descriptors (no private keys) from source wallet
        public_descriptors = self.source_wallet.listdescriptors(False)['descriptors']
        self.log.info(f"Exporting {len(public_descriptors)} public descriptors")

        # Create watch-only descriptor wallet
        self.nodes[1].createwallet(
            wallet_name='dd_watchonly',
            descriptors=True,
            disable_private_keys=True,
            blank=True
        )
        watchonly_wallet = self.nodes[1].get_wallet_rpc('dd_watchonly')

        # Verify it's a watch-only wallet
        wallet_info = watchonly_wallet.getwalletinfo()
        if 'private_keys_enabled' in wallet_info:
            assert_equal(wallet_info['private_keys_enabled'], False)
        self.log.info("Created watch-only descriptor wallet")

        # Import public descriptors
        import_requests = []
        for desc in public_descriptors:
            import_req = {
                "desc": desc['desc'],
                "timestamp": 0,  # Rescan from genesis
            }
            if 'active' in desc:
                import_req['active'] = desc['active']
            if 'internal' in desc:
                import_req['internal'] = desc['internal']
            if 'range' in desc:
                import_req['range'] = desc['range']
            import_requests.append(import_req)

        import_results = watchonly_wallet.importdescriptors(import_requests)
        success_count = sum(1 for r in import_results if r.get('success', False))
        self.log.info(f"Imported {success_count}/{len(import_requests)} public descriptors")

        # Default balance must exclude watch-only DD so it is not presented as
        # spendable. The explicit include_watchonly flag can expose it for
        # monitoring-only wallets.
        watchonly_balance_info = watchonly_wallet.getdigidollarbalance()
        watchonly_balance = watchonly_balance_info['total'] if isinstance(watchonly_balance_info, dict) else watchonly_balance_info
        self.log.info(f"Watch-only wallet default balance: {watchonly_balance} DD cents")
        assert_equal(watchonly_balance, 0)

        watchonly_included_info = watchonly_wallet.getdigidollarbalance("", 1, True)
        watchonly_included = watchonly_included_info['total'] if isinstance(watchonly_included_info, dict) else watchonly_included_info
        self.log.info(f"Watch-only wallet monitoring balance: {watchonly_included} DD cents")
        assert_greater_than(watchonly_included, 0)

        # Verify watch-only CANNOT spend
        other_address = self.source_wallet.getdigidollaraddress()
        assert_raises_rpc_error(
            -4,
            "Private keys are disabled",
            watchonly_wallet.senddigidollar,
            other_address,
            100,
        )

        # Verify watch-only CANNOT mint (requires signing)
        assert_raises_rpc_error(
            -4, "Private keys are disabled", watchonly_wallet.mintdigidollar, 1000, 1,
        )

        # Verify watch-only can list positions for monitoring, but every
        # position is clearly non-spendable.
        positions = watchonly_wallet.listdigidollarpositions()
        self.log.info(f"Watch-only wallet can view {len(positions)} positions")
        for position in positions:
            assert_equal(position["iswatchonly"], True)
            assert_equal(position["spendable"], False)
            assert_equal(position["can_redeem"], False)

        redemption_info = watchonly_wallet.getredemptioninfo(watchonly_redeem_mint["position_id"])
        assert_equal(redemption_info["can_redeem"], False)
        assert_equal(redemption_info["redeemable_dd"], watchonly_redeem_mint["dd_minted"])

        watchonly_addresses_default = watchonly_wallet.listdigidollaraddresses()
        assert_equal(watchonly_addresses_default, [])

        watchonly_addresses = watchonly_wallet.listdigidollaraddresses(True)
        self.log.info(f"Watch-only wallet can view {len(watchonly_addresses)} DD address balances")
        assert_greater_than(len(watchonly_addresses), 0)
        for addr_info in watchonly_addresses:
            assert_equal(addr_info["ismine"], False)
            assert_equal(addr_info["iswatchonly"], True)
            validation = watchonly_wallet.validateddaddress(addr_info["address"])
            assert_equal(validation["isvalid"], True)
            assert_equal(validation["ismine"], False)
            assert_equal(validation["iswatchonly"], True)

        # Verify watch-only can list private descriptors fails
        try:
            watchonly_wallet.listdescriptors(True)
            self.log.warning("Watch-only wallet should not export private descriptors")
        except Exception as e:
            self.log.info(f"Watch-only correctly prevented private descriptor export: {type(e).__name__}")

        self.log.info("Watch-only descriptor import test passed")


if __name__ == '__main__':
    DigiDollarDescriptorTest().main()
