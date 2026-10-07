#!/usr/bin/env python3
# Copyright (c) 2025-2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from decimal import Decimal
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class DigiDollarRPCRedemptionTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Testing DigiDollar redemption info RPC...")
        node = self.nodes[0]
        
        self.log.info("Generating initial blocks for test setup...")
        self.generate(node, 340)

        node.setmockoracleprice(500000)
        mint_result = node.mintdigidollar(10000, 0)
        self.position_id = mint_result["position_id"]
        self.position_amount = mint_result["dd_minted"]
        self.generate(node, 1)
        
        self.test_redemption_info_basic()
        self.test_redemption_info_fields()
        self.test_redemption_info_with_amount()
        self.test_redemption_info_invalid_params()
        self.test_redemption_info_real_position()
        self.test_redeem_uses_requested_unlock_address()
        
        self.log.info("All redemption info tests passed!")

    def test_redemption_info_basic(self):
        self.log.info("Testing basic redemption info response...")
        node = self.nodes[0]
        
        test_position_id = self.position_id
        result = node.getredemptioninfo(test_position_id)
        
        assert 'position_id' in result, "Missing 'position_id' field"
        assert 'can_redeem' in result, "Missing 'can_redeem' field"
        assert 'redemption_path' in result, "Missing 'redemption_path' field"
        assert 'total_dd_minted' in result, "Missing 'total_dd_minted' field"
        assert 'dgb_return' in result, "Missing 'dgb_return' field"
        
        assert_equal(result['position_id'], test_position_id)
        self.log.info(f"Position ID: {result['position_id'][:16]}...")
        self.log.info(f"Can redeem: {result['can_redeem']}")
        self.log.info(f"Redemption path: {result['redemption_path']}")

    def test_redemption_info_fields(self):
        self.log.info("Testing redemption info field types...")
        node = self.nodes[0]
        
        test_position_id = self.position_id
        result = node.getredemptioninfo(test_position_id)
        
        assert isinstance(result['position_id'], str), "position_id should be string"
        assert isinstance(result['can_redeem'], bool), "can_redeem should be boolean"
        assert isinstance(result['redemption_path'], str), "redemption_path should be string"
        assert isinstance(result['total_dd_minted'], int), "total_dd_minted should be int"
        assert isinstance(result['unlock_height'], int), "unlock_height should be int"
        assert isinstance(result['timelock_remaining'], int), "timelock_remaining should be int"
        assert isinstance(result['status'], str), "status should be string"
        
        valid_paths = ['normal', 'emergency']
        assert result['redemption_path'] in valid_paths, f"Invalid redemption_path: {result['redemption_path']}"
        
        self.log.info("All field types verified correctly")

    def test_redemption_info_with_amount(self):
        self.log.info("Testing redemption info with specific DD amount...")
        node = self.nodes[0]
        
        test_position_id = self.position_id

        result_full = node.getredemptioninfo(test_position_id)
        assert_equal(result_full['redeemable_dd'], result_full['total_dd_minted'])
        assert_raises_rpc_error(
            -8,
            "Exact-amount redemption required",
            node.getredemptioninfo,
            test_position_id,
            5000,
        )
        result_exact = node.getredemptioninfo(test_position_id, self.position_amount)
        assert_equal(result_exact['redeemable_dd'], self.position_amount)

    def test_redemption_info_invalid_params(self):
        self.log.info("Testing redemption info with invalid parameters...")
        node = self.nodes[0]
        
        self.log.info("  Testing invalid position ID format...")
        assert_raises_rpc_error(-8, None, node.getredemptioninfo, "invalid_not_hex")
        
        self.log.info("  Testing short position ID...")
        assert_raises_rpc_error(-8, None, node.getredemptioninfo, "abcd1234")
        
        self.log.info("  Testing too long position ID...")
        long_id = "a" * 128
        assert_raises_rpc_error(-8, None, node.getredemptioninfo, long_id)

        self.log.info("  Testing unknown valid-looking position ID...")
        unknown_id = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890"
        assert_raises_rpc_error(-8, "not found in wallet", node.getredemptioninfo, unknown_id)
        
        self.log.info("Invalid parameter tests passed")

    def test_redemption_info_real_position(self):
        self.log.info("Testing redemption info with real minted position...")
        node = self.nodes[0]
        
        result = node.getredemptioninfo(self.position_id)

        assert_equal(result['position_id'], self.position_id)
        assert 'can_redeem' in result
        assert 'dgb_return' in result

        self.log.info(f"Real position redemption info retrieved successfully")
        self.log.info(f"  Position ID: {self.position_id[:16]}...")
        self.log.info(f"  Can redeem: {result['can_redeem']}")
        self.log.info(f"  DGB return: {result['dgb_return']}")

    def test_redeem_uses_requested_unlock_address(self):
        self.log.info("Testing redeemdigidollar pays the requested unlock address...")
        node = self.nodes[0]

        position = next(p for p in node.listdigidollarpositions(False) if p["position_id"] == self.position_id)
        blocks_needed = max(0, position["unlock_height"] - node.getblockcount())
        if blocks_needed:
            self.generate(node, blocks_needed)

        unlock_address = node.getnewaddress("dd-rh-076-requested-unlock", "bech32")
        unlock_script = node.getaddressinfo(unlock_address)["scriptPubKey"]

        redeem = node.redeemdigidollar(position_id=self.position_id, dd_amount="100.00", redemption_address=unlock_address, amount_unit="dollars")
        assert_equal(redeem["unlock_address"], unlock_address)

        decoded = node.getrawtransaction(redeem["txid"], True)
        output_scripts = [vout["scriptPubKey"]["hex"] for vout in decoded["vout"]]
        assert unlock_script in output_scripts, "redemption transaction did not pay the requested unlock address"


if __name__ == '__main__':
    DigiDollarRPCRedemptionTest().main()
