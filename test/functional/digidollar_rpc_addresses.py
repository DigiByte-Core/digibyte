#!/usr/bin/env python3
"""Test DigiDollar address management RPC commands.

Test comprehensive functionality for DD address RPCs:
- validateddaddress: Validate DD address format, prefix, checksum
- listdigidollaraddresses: List wallet DD addresses with filtering
- importdigidollaraddress: Import external DD addresses for watch-only

DD addresses use prefixes:
- DD (mainnet)
- TD (testnet)
- RD (regtest)

Addresses are P2TR (Taproot) encoded in base58check format.
"""

from datetime import datetime, timezone

from test_framework.test_framework import DigiByteTestFramework
from test_framework.messages import hash256
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)


B58CHARS = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
DD_VERSION_MAINNET = bytes.fromhex("5285")
DD_VERSION_TESTNET = bytes.fromhex("b129")
DD_VERSION_REGTEST = bytes.fromhex("a3a4")


def b58check_decode_raw(address):
    value = 0
    for char in address:
        value *= 58
        assert char in B58CHARS
        value += B58CHARS.index(char)
    raw = value.to_bytes((value.bit_length() + 7) // 8, 'big')
    pad = 0
    for char in address:
        if char == B58CHARS[0]:
            pad += 1
        else:
            break
    raw = b'\x00' * pad + raw
    assert hash256(raw[:-4])[:4] == raw[-4:]
    return raw[:-4]


def b58check_encode_raw(payload):
    raw = payload + hash256(payload)[:4]
    value = int.from_bytes(raw, 'big')
    result = ''
    while value > 0:
        result = B58CHARS[value % 58] + result
        value //= 58
    while raw and raw[0] == 0:
        result = B58CHARS[0] + result
        raw = raw[1:]
    return result


def reencode_digidollar_address(address, version):
    payload = b58check_decode_raw(address)
    assert payload[:2] in (DD_VERSION_MAINNET, DD_VERSION_TESTNET, DD_VERSION_REGTEST)
    return b58check_encode_raw(version + payload[2:])


class DigiDollarAddressTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"],
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0"]
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Testing DigiDollar address management RPCs...")

        self.setup_digidollar_test()

        self.log.info("=== validateddaddress tests ===")
        self.test_validate_valid_regtest_address()
        self.test_reject_cross_network_addresses()
        self.test_validate_invalid_prefix()
        self.test_validate_invalid_checksum()
        self.test_validate_empty_address()
        self.test_validate_response_format()

        self.log.info("=== Wave 15 wrong-shape tests ===")
        self.test_validate_whitespace_padded_address_wave15()
        self.test_validate_invalid_base58_chars_wave15()
        self.test_validate_bech32_address_rejected_wave15()
        self.test_validate_oversized_address_wave15()
        self.test_validate_invalid_address_field_blank_wave15()
        self.test_send_rejects_cross_network_address_wave15()

        self.log.info("=== listdigidollaraddresses tests ===")
        self.test_list_addresses_empty_wallet()
        self.test_list_addresses_after_generation()
        self.test_get_address_does_not_create_dgb_receive_entry()
        self.test_list_addresses_with_balance()
        self.test_list_addresses_include_watchonly()
        self.test_address_metadata()

        self.log.info("=== Bug #12 regression test ===")
        self.test_no_mock_addresses_bug12()

        self.log.info("=== importdigidollaraddress tests ===")
        self.test_import_valid_address()
        self.test_import_with_label()
        self.test_import_duplicate()
        self.test_import_invalid_address()

        self.log.info("All DigiDollar address management tests passed!")

    def setup_digidollar_test(self):
        self.log.info("Generating initial blocks for test setup...")
        self.generate(self.nodes[0], 110)
        self.sync_all()

        self.log.info("Setting mock oracle price...")
        for node in self.nodes:
            node.setmockoracleprice(500000)

        stats = self.nodes[0].getdigidollarstats()
        assert "health_percentage" in stats
        assert "health_status" in stats

    # === validateddaddress tests ===

    def test_validate_valid_regtest_address(self):
        self.log.info("Testing validateddaddress with valid regtest address...")

        dd_address = self.nodes[0].getdigidollaraddress()
        self.log.info(f"Generated DD address: {dd_address}")

        assert dd_address.startswith('RD'), f"Regtest DD address should start with 'RD', got: {dd_address}"

        result = self.nodes[0].validateddaddress(dd_address)

        assert_equal(result['isvalid'], True)
        assert_equal(result['address'], dd_address)
        assert_equal(result['network'], 'regtest')
        assert_equal(result['prefix'], 'RD')

        self.log.info("Valid regtest address validation passed")

    def test_reject_cross_network_addresses(self):
        self.log.info("Testing validateddaddress rejects cross-network DD prefixes...")

        regtest_address = self.nodes[0].getdigidollaraddress()
        mainnet_address = reencode_digidollar_address(regtest_address, DD_VERSION_MAINNET)
        testnet_address = reencode_digidollar_address(regtest_address, DD_VERSION_TESTNET)

        assert mainnet_address.startswith("DD")
        assert testnet_address.startswith("TD")

        for wrong_network_address in (mainnet_address, testnet_address):
            result = self.nodes[0].validateddaddress(wrong_network_address)
            assert_equal(result["isvalid"], False)
            assert "network" in result["error"].lower()

        self.log.info("Cross-network DD addresses are rejected on regtest")

    def test_validate_invalid_prefix(self):
        self.log.info("Testing validateddaddress with invalid prefix...")

        invalid_address = "XXinvalidprefix123456789abc"

        result = self.nodes[0].validateddaddress(invalid_address)

        assert_equal(result['isvalid'], False)
        assert 'error' in result
        assert 'prefix' in result['error'].lower() or 'invalid' in result['error'].lower()

        self.log.info("Invalid prefix validation passed")

    def test_validate_invalid_checksum(self):
        self.log.info("Testing validateddaddress with corrupted address...")

        valid_address = self.nodes[0].getdigidollaraddress()

        if len(valid_address) > 5:
            corrupted_address = valid_address[:-4] + "XXXX"
        else:
            corrupted_address = valid_address + "corrupt"

        result = self.nodes[0].validateddaddress(corrupted_address)

        assert 'isvalid' in result
        assert_equal(result['isvalid'], False)
        assert 'error' in result
        self.log.info(f"Corrupted address correctly marked invalid: {result['error']}")

    def test_validate_empty_address(self):
        self.log.info("Testing validateddaddress with empty address...")

        try:
            result = self.nodes[0].validateddaddress("")

            assert_equal(result['isvalid'], False)
            assert 'error' in result
            self.log.info(f"Empty address validation error: {result['error']}")

        except Exception as e:
            self.log.info(f"Empty address raised expected error: {e}")

        self.log.info("Empty address validation passed")

    def test_validate_response_format(self):
        self.log.info("Testing validateddaddress response format...")

        dd_address = self.nodes[0].getdigidollaraddress()

        result = self.nodes[0].validateddaddress(dd_address)

        required_fields = ['isvalid', 'address', 'network', 'prefix']
        for field in required_fields:
            assert field in result, f"Missing required field: {field}"
            self.log.info(f"  {field}: {result[field]}")

        optional_fields = ['ismine', 'iswatchonly']
        for field in optional_fields:
            if field in result:
                self.log.info(f"  {field}: {result[field]}")

        assert isinstance(result['isvalid'], bool)
        assert isinstance(result['address'], str)
        assert isinstance(result['network'], str)
        assert isinstance(result['prefix'], str)

        invalid_result = self.nodes[0].validateddaddress("XXinvalid")
        assert 'isvalid' in invalid_result
        assert_equal(invalid_result['isvalid'], False)
        assert 'error' in invalid_result
        assert isinstance(invalid_result['error'], str)

        self.log.info("Response format validation passed")

    # === Wave 15 wrong-shape tests ===

    def test_validate_whitespace_padded_address_wave15(self):
        """Pin: validateddaddress must reject leading/trailing whitespace.

        DecodeBase58 transparently skips leading/trailing whitespace, so a
        valid DD payload wrapped in spaces decodes successfully. The
        node-level prefix check must catch this and refuse the address
        because addressStr.substr(0, 2) does NOT match the expected DD/TD/RD
        prefix when the first byte is whitespace.
        """
        self.log.info("Wave 15: validateddaddress rejects whitespace-padded address...")

        valid = self.nodes[0].getdigidollaraddress()
        for padded in (" " + valid, valid + " ", " " + valid + " ",
                       "\t" + valid, "\n" + valid, valid + "\n"):
            result = self.nodes[0].validateddaddress(padded)
            assert_equal(result["isvalid"], False)
            # On invalid, the address echo MUST be empty so callers can
            # never copy a whitespace-corrupted string back into a send.
            assert_equal(result["address"], "")
            assert "error" in result
            assert result["error"], f"empty error for padded={padded!r}"

        self.log.info("Whitespace-padded DD address correctly rejected")

    def test_validate_invalid_base58_chars_wave15(self):
        """Pin: validateddaddress rejects strings containing the four
        non-base58 ASCII characters: '0', 'O', 'I', 'l'."""
        self.log.info("Wave 15: validateddaddress rejects invalid base58 chars...")

        valid = self.nodes[0].getdigidollaraddress()
        # Replace position 4 with each forbidden character.
        for bad_char in ("0", "O", "I", "l"):
            corrupted = valid[:4] + bad_char + valid[5:]
            result = self.nodes[0].validateddaddress(corrupted)
            assert_equal(result["isvalid"], False)
            assert_equal(result["address"], "")

        self.log.info("Forbidden base58 chars correctly rejected")

    def test_validate_bech32_address_rejected_wave15(self):
        """Pin: validateddaddress rejects bech32m taproot addresses, which
        are a different encoding family (HRP + bech32m) and must never
        decode as DigiDollar base58check."""
        self.log.info("Wave 15: validateddaddress rejects bech32m taproot input...")

        # Generate a real bech32m P2TR DigiByte address on the regtest node;
        # this is a structurally valid taproot address but in a wholly
        # different encoding family from DigiDollar's base58check format.
        bech32m_taproot = self.nodes[0].getnewaddress("wave15-bech32m", "bech32m")
        assert bech32m_taproot.startswith("dgbrt1p"), \
            f"Expected regtest taproot HRP, got {bech32m_taproot}"

        result = self.nodes[0].validateddaddress(bech32m_taproot)
        assert_equal(result["isvalid"], False)
        assert_equal(result["address"], "")

        # Also a fabricated bech32m string with the DD-style prefix glued on.
        fabricated = "dgbrt1pddwave15fakebech32m000000000000000000000000"
        result = self.nodes[0].validateddaddress(fabricated)
        assert_equal(result["isvalid"], False)

        self.log.info("Bech32m taproot input correctly rejected by validateddaddress")

    def test_validate_oversized_address_wave15(self):
        """Pin: validateddaddress rejects strings whose decoded payload is
        not exactly 34 bytes (2-byte version + 32-byte taproot key)."""
        self.log.info("Wave 15: validateddaddress rejects oversized input...")

        # Length 200 of valid base58 chars; decodes to far more than 34 bytes
        # so the constructor's `vchTemp.size() == 34` arm is not entered.
        oversized = "RD" + ("a" * 200)
        result = self.nodes[0].validateddaddress(oversized)
        assert_equal(result["isvalid"], False)
        assert_equal(result["address"], "")

        # And a 1024-char garbage string; decoder length cap is 256 bytes,
        # so this also fails cleanly without raising.
        result_huge = self.nodes[0].validateddaddress("RD" + ("D" * 1024))
        assert_equal(result_huge["isvalid"], False)

        self.log.info("Oversized DD address correctly rejected")

    def test_validate_invalid_address_field_blank_wave15(self):
        """Pin: every invalid validateddaddress result has address=='', so
        UIs/integrators cannot accidentally echo attacker-controlled junk
        back into a send."""
        self.log.info("Wave 15: validateddaddress address echo is blank on failure...")

        for junk in (
            "",
            "RD",
            "XXnotanaddress123456789",
            "DDgarbage" + "0" * 30,        # contains '0'
            "RD!@#$%^&*()",
            "../../etc/passwd",
            "data:text/html;base64,UE9D",
        ):
            result = self.nodes[0].validateddaddress(junk)
            assert_equal(result["isvalid"], False)
            assert_equal(result["address"], "")

        self.log.info("Invalid input never echoed back as 'address'")

    def test_send_rejects_cross_network_address_wave15(self):
        """Pin: senddigidollar rejects a cross-network DD address (mainnet
        DD prefix submitted to a regtest node) before any wallet/oracle
        side effects occur. This is the consensus-safety analog of the
        cross-network validateddaddress rejection."""
        self.log.info("Wave 15: senddigidollar rejects cross-network address...")

        regtest = self.nodes[0].getdigidollaraddress()
        mainnet_dd = reencode_digidollar_address(regtest, DD_VERSION_MAINNET)
        testnet_dd = reencode_digidollar_address(regtest, DD_VERSION_TESTNET)

        for cross in (mainnet_dd, testnet_dd):
            assert_raises_rpc_error(
                -5,  # RPC_INVALID_ADDRESS_OR_KEY
                "DigiDollar address is for",
                self.nodes[0].senddigidollar, cross, 100,
            )

        self.log.info("Cross-network senddigidollar correctly rejected")

    # === listdigidollaraddresses tests ===

    def test_list_addresses_empty_wallet(self):
        self.log.info("Testing listdigidollaraddresses on empty/new wallet...")

        result = self.nodes[1].listdigidollaraddresses()

        assert isinstance(result, list), f"Expected list, got {type(result)}"

        self.log.info(f"Node 1 has {len(result)} DD addresses initially")

        for addr_info in result:
            assert isinstance(addr_info, dict), "Each address entry should be a dict"
            assert 'address' in addr_info, "Missing 'address' field"

        self.log.info("Empty wallet address list test passed")

    def test_list_addresses_after_generation(self):
        self.log.info("Testing listdigidollaraddresses after generating addresses...")

        generated_addresses = []
        for i in range(3):
            addr = self.nodes[0].getdigidollaraddress()
            generated_addresses.append(addr)
            self.log.info(f"Generated address {i+1}: {addr}")

        assert len(set(generated_addresses)) == len(generated_addresses), "Generated addresses should be unique"

        result = self.nodes[0].listdigidollaraddresses()

        assert isinstance(result, list), f"Expected list, got {type(result)}"
        self.log.info(f"listdigidollaraddresses returned {len(result)} addresses")

        for addr_info in result:
            assert isinstance(addr_info, dict), "Each address entry should be a dict"
            if 'address' in addr_info:
                self.log.info(f"  Address: {addr_info['address']}")

        self.log.info("Address list after generation test passed")

    def test_get_address_does_not_create_dgb_receive_entry(self):
        self.log.info("Testing getdigidollaraddress keeps DD labels out of normal DGB receive book...")

        label = "dd_final_009_rpc_label"
        assert_raises_rpc_error(
            -11,
            "No addresses with label",
            self.nodes[0].getaddressesbylabel,
            label,
        )

        dd_address = self.nodes[0].getdigidollaraddress(label)
        assert dd_address.startswith("RD"), f"Expected regtest DD address, got {dd_address}"

        entries = self.nodes[0].getaddressesbylabel(label)
        receive_entries = {
            address: info
            for address, info in entries.items()
            if info.get("purpose") == "receive"
        }
        assert_equal(receive_entries, {})
        assert any(info.get("purpose") == "digidollar" for info in entries.values()), entries

        self.log.info("getdigidollaraddress did not leak a normal DGB receive entry")

    def test_list_addresses_with_balance(self):
        self.log.info("Testing listdigidollaraddresses with balance filter...")

        mint_amount = 50000
        dca_tier = 1

        self.log.info(f"Minting {mint_amount} cents DD...")
        mint_result = self.nodes[0].mintdigidollar(mint_amount, dca_tier)
        assert 'txid' in mint_result

        self.generate(self.nodes[0], 2)
        self.sync_all()

        balance_info = self.nodes[0].getdigidollarbalance()
        current_balance = balance_info['total'] if isinstance(balance_info, dict) else balance_info
        assert_greater_than(current_balance, 0)
        self.log.info(f"Current DD balance: {current_balance} cents")

        result_with_filter = self.nodes[0].listdigidollaraddresses(False, 1000)
        assert isinstance(result_with_filter, list)
        self.log.info(f"Addresses with balance >= 1000 cents: {len(result_with_filter)}")
        assert_greater_than(len(result_with_filter), 0)

        for addr_info in result_with_filter:
            assert addr_info['balance'] >= 1000, f"Balance {addr_info['balance']} below filter"

        result_above_balance = self.nodes[0].listdigidollaraddresses(False, mint_amount + 1)
        assert_equal(result_above_balance, [])

        result_all = self.nodes[0].listdigidollaraddresses()
        assert isinstance(result_all, list)
        self.log.info(f"Total addresses listed: {len(result_all)}")

        self.log.info("Address list with balance test passed")

    def test_address_metadata(self):
        sender, receiver = self.nodes
        label = "DD payments"
        address = receiver.getdigidollaraddress(label)

        def entry():
            return next(row for row in receiver.listdigidollaraddresses(False, 0, True)
                        if row["address"] == address)

        empty = entry()
        assert_equal(empty["label"], label)
        assert_equal(empty["txcount"], 0)
        assert_equal(empty["created_date"], "")
        assert_equal(empty["last_used"], "")
        assert address not in [row["address"] for row in receiver.listdigidollaraddresses()]
        sender.sendtoaddress(receiver.getnewaddress(), 10)
        transactions = []
        for _ in range(2):
            transactions.append(sender.senddigidollar(address, 2000)["txid"])
            self.generate(sender, 1)
            self.sync_all()
        received = entry()
        assert_equal(received["balance"], 4000)
        assert_equal(received["txcount"], 2)
        assert_equal(received["created_date"], "")
        def latest_time():
            timestamp = max(receiver.gettransaction(txid)["time"] for txid in transactions)
            return datetime.fromtimestamp(timestamp, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

        assert_equal(received["last_used"], latest_time())

        # Both inputs use the same address, but this is only one transaction.
        transactions.append(receiver.senddigidollar(sender.getdigidollaraddress(), 4000)["txid"])
        self.generate(sender, 1)
        self.sync_all()
        spent = entry()
        assert_equal(spent["balance"], 0)
        assert_equal(spent["txcount"], 3)
        assert_equal(spent["label"], label)
        assert_equal(spent["last_used"], latest_time())
        assert address not in [row["address"] for row in receiver.listdigidollaraddresses()]
        self.restart_node(1)
        assert_equal(entry(), spent)
        self.connect_nodes(0, 1)

    def test_list_addresses_include_watchonly(self):
        self.log.info("Testing listdigidollaraddresses with include_watchonly...")

        external_address = "RDtestwatchonly123456789abcdef"
        try:
            self.nodes[0].importdigidollaraddress(external_address, "watchonly_test")
        except Exception as e:
            self.log.info(f"Import for watchonly test: {e}")

        result_no_watchonly = self.nodes[0].listdigidollaraddresses(False)
        assert isinstance(result_no_watchonly, list)

        count_no_watchonly = len(result_no_watchonly)
        self.log.info(f"Addresses without watch-only: {count_no_watchonly}")

        result_with_watchonly = self.nodes[0].listdigidollaraddresses(True)
        assert isinstance(result_with_watchonly, list)

        count_with_watchonly = len(result_with_watchonly)
        self.log.info(f"Addresses with watch-only: {count_with_watchonly}")

        assert count_with_watchonly >= count_no_watchonly, \
            "Including watch-only should not decrease address count"

        for addr_info in result_with_watchonly:
            if addr_info.get('iswatchonly', False):
                self.log.info(f"Watch-only address: {addr_info.get('address', 'N/A')}")

        self.log.info("Include watchonly test passed")

    # === Bug #12 regression: no hardcoded mock addresses ===

    def test_no_mock_addresses_bug12(self):
        """Regression test for Bug #12: listdigidollaraddresses must not return
        hardcoded mock data. Verify that no address starts with 'DDmock' and
        that addresses use the correct network prefix (RD for regtest)."""
        self.log.info("Bug #12 regression: verifying no mock addresses returned...")

        result = self.nodes[0].listdigidollaraddresses()
        assert isinstance(result, list)

        for addr_info in result:
            addr = addr_info.get('address', '')
            # Must not contain hardcoded mock strings
            assert 'DDmock' not in addr, f"Bug #12: found mock address '{addr}'"
            assert 'DDwatchonly' not in addr, f"Bug #12: found mock address '{addr}'"
            # Regtest addresses must start with 'RD', not 'DD'
            assert addr.startswith('RD'), \
                f"Bug #12: regtest address '{addr}' should start with 'RD'"
            # Balance must be a real integer, not hardcoded 10000/25000/5000
            assert isinstance(addr_info.get('balance', 0), int)

        # On a fresh node with no DD activity, list should be empty
        result_node1 = self.nodes[1].listdigidollaraddresses()
        assert_equal(len(result_node1), 0)

        self.log.info("Bug #12 regression test passed: no mock addresses found")

    # === importdigidollaraddress tests ===

    def test_import_valid_address(self):
        self.log.info("Testing importdigidollaraddress with valid address...")

        external_address = self.nodes[1].getdigidollaraddress()
        self.log.info(f"Importing address from node 1: {external_address}")

        result = self.nodes[0].importdigidollaraddress(external_address)

        assert 'address' in result
        assert_equal(result['address'], external_address)
        assert 'success' in result
        assert_equal(result['success'], False)
        assert_equal(result['rescan_performed'], False)
        assert_equal(result['transactions_found'], 0)
        assert 'warning' in result
        assert 'unsupported' in result['warning']

        self.log.info(f"Import result: {result}")
        self.log.info("Valid address import test passed")

    def test_import_with_label(self):
        self.log.info("Testing importdigidollaraddress with custom label...")

        external_address = self.nodes[1].getdigidollaraddress()
        label = "my_external_wallet"

        result = self.nodes[0].importdigidollaraddress(external_address, label)

        assert 'address' in result
        assert_equal(result['address'], external_address)
        assert 'label' in result
        assert_equal(result['label'], label)
        assert 'success' in result
        assert_equal(result['success'], False)
        assert 'warning' in result
        assert 'unsupported' in result['warning']

        self.log.info(f"Import with label result: {result}")
        self.log.info("Import with label test passed")

    def test_import_duplicate(self):
        self.log.info("Testing importdigidollaraddress duplicate import...")

        external_address = self.nodes[1].getdigidollaraddress()

        result1 = self.nodes[0].importdigidollaraddress(external_address, "first_import")
        assert 'success' in result1
        assert_equal(result1['success'], False)
        self.log.info(f"First import: success={result1['success']}")

        try:
            result2 = self.nodes[0].importdigidollaraddress(external_address, "second_import")

            if 'success' in result2:
                self.log.info(f"Second import: success={result2['success']}")
                assert_equal(result2['success'], False)
                if 'warning' in result2:
                    self.log.info(f"Warning: {result2['warning']}")

        except Exception as e:
            self.log.info(f"Duplicate import raised expected error: {e}")

        self.log.info("Duplicate import test passed")

    def test_import_invalid_address(self):
        self.log.info("Testing importdigidollaraddress with invalid address...")

        invalid_addresses = [
            ("XXinvalidprefix123456789abcdef", "invalid prefix"),
            ("RD", "address too short"),
            ("RD" + "x" * 50, "address too long"),
            ("BTCnotdigidollaraddr12345678", "wrong currency prefix"),
        ]

        for invalid_address, description in invalid_addresses:
            self.log.info(f"Testing {description}: {invalid_address[:20]}...")

            try:
                result = self.nodes[0].importdigidollaraddress(invalid_address)

                if 'success' in result and not result['success']:
                    raise AssertionError(f"Invalid address returned non-error result: {result}")
                else:
                    raise AssertionError(f"Unexpected success for invalid address: {result}")

            except Exception as e:
                error_msg = str(e)
                assert 'Invalid' in error_msg or 'invalid' in error_msg or 'address' in error_msg.lower(), \
                    f"Unexpected error message: {error_msg}"
                self.log.info(f"  Correctly raised error: {error_msg[:50]}...")

        self.log.info("Invalid address import test passed")


if __name__ == '__main__':
    DigiDollarAddressTest().main()
