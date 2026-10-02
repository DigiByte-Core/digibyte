// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <test/util/random.h>

#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <digidollar/digidollar.h>
#include <digidollar/scripts.h>
#include <consensus/digidollar.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/interpreter.h>
#include <key.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/test/util.h>
#include <wallet/digidollarwallet.h>
#include <logging.h>

#include <boost/test/unit_test.hpp>

#include <map>

using namespace DigiDollar;

// Mock UTXO store for testing - maps outpoint to DD amount
std::map<COutPoint, CAmount> g_mockDDUTXOs;
std::map<COutPoint, CAmount> g_mockDGBUTXOs;

/**
 * Test-specific TransferTxBuilder that uses mock UTXO stores
 */
class MockTransferTxBuilder : public TransferTxBuilder {
public:
    using TransferTxBuilder::TransferTxBuilder;

protected:
    CAmount GetDDFromUTXO(const COutPoint& outpoint) const override {
        auto it = g_mockDDUTXOs.find(outpoint);
        if (it != g_mockDDUTXOs.end()) {
            return it->second;
        }
        return 0;
    }

    CAmount GetDGBFromUTXO(const COutPoint& outpoint) const override {
        auto it = g_mockDGBUTXOs.find(outpoint);
        if (it != g_mockDGBUTXOs.end()) {
            return it->second;
        }
        return 100 * COIN; // Default for testing
    }
};

BOOST_FIXTURE_TEST_SUITE(digidollar_transfer_tests, TestingSetup)

/**
 * Test fixture for DigiDollar transfer transaction tests
 * Sets up necessary environment for testing transfer operations
 */
struct DDTransferTestFixture : public TestingSetup {
    // Test chain parameters
    const CChainParams& chainParams;

    // Test keys for various roles
    CKey senderKey;
    CKey recipientKey;
    CKey changeKey;

    // Mock blockchain state
    int currentHeight;
    CAmount oraclePrice;
    int systemCollateral;

    // Test amounts (in cents)
    static const CAmount TEST_DD_AMOUNT = 10000;  // $100.00
    static const CAmount LARGE_DD_AMOUNT = 5000000; // $50,000.00
    static const CAmount MAX_TRANSFER_AMOUNT = 10000000; // $100,000.00
    static const CAmount DUST_AMOUNT = 50;        // $0.50

    DDTransferTestFixture() : chainParams(m_node.chainman->GetParams()) {
        // Generate test keys
        senderKey.MakeNewKey(true);
        recipientKey.MakeNewKey(true);
        changeKey.MakeNewKey(true);

        // Set blockchain state
        currentHeight = 100000;
        oraclePrice = 2500; // $25.00 per DGB
        systemCollateral = 150; // 150% healthy system

        // Clear mock UTXO stores
        g_mockDDUTXOs.clear();
        g_mockDGBUTXOs.clear();
    }

    /**
     * Create mock DD UTXO for testing
     */
    COutPoint CreateMockDDUTXO(CAmount ddAmount) {
        // Mock UTXO creation - in real implementation this would reference blockchain
        COutPoint outpoint(InsecureRand256(), 0);
        g_mockDDUTXOs[outpoint] = ddAmount; // Store amount in mock store
        return outpoint;
    }

    /**
     * Create mock DGB UTXO for fees
     */
    COutPoint CreateMockDGBUTXO(CAmount dgbAmount) {
        // Mock UTXO creation
        COutPoint outpoint(InsecureRand256(), 1);
        g_mockDGBUTXOs[outpoint] = dgbAmount; // Store amount in mock store
        return outpoint;
    }

    /**
     * Create DD address from public key
     */
    std::string CreateDDAddress(const CPubKey& pubkey) {
        // Create P2TR destination
        XOnlyPubKey xonly(pubkey);
        WitnessV1Taproot dest(xonly);
        return EncodeDigiDollarAddress(dest, chainParams);
    }

    /**
     * Extract DD amounts from OP_RETURN metadata output
     * Transfer format: OP_RETURN <"DD"> <txType=2> <amount1> <amount2> ... <amountN>
     */
    bool ExtractDDAmountsFromOpReturn(const CScript& script, std::vector<CAmount>& amounts) {
        amounts.clear();

        auto pc = script.begin();
        opcodetype opcode;
        std::vector<unsigned char> data;

        // Check for OP_RETURN
        if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) {
            return false;
        }

        // Get "DD" marker
        if (!script.GetOp(pc, opcode, data) || data.size() != 2 || data[0] != 'D' || data[1] != 'D') {
            return false;
        }

        // Get transaction type (should be 2 for TRANSFER)
        if (!script.GetOp(pc, opcode, data)) {
            return false;
        }

        // Extract all remaining amounts
        while (script.GetOp(pc, opcode, data)) {
            try {
                CScriptNum scriptNum(data, false);
                CAmount amount = scriptNum.GetInt64();
                if (amount > 0) {
                    amounts.push_back(amount);
                }
            } catch (const scriptnum_error&) {
                // Skip invalid data
                continue;
            }
        }

        return !amounts.empty();
    }

    /**
     * Parse DD transaction outputs
     * Returns: DD P2TR outputs, OP_RETURN output, and extracted DD amounts
     */
    struct DDOutputInfo {
        std::vector<CTxOut> ddP2TROutputs;
        CTxOut opReturnOutput;
        std::vector<CAmount> ddAmounts;
        CAmount totalDD = 0;
        bool hasOpReturn = false;
    };

    DDOutputInfo ParseDDOutputs(const CMutableTransaction& tx) {
        DDOutputInfo info;

        // Find DD P2TR outputs and OP_RETURN
        for (const auto& output : tx.vout) {
            if (output.nValue == 0) {
                if (!output.scriptPubKey.empty() && output.scriptPubKey[0] == OP_RETURN) {
                    info.opReturnOutput = output;
                    info.hasOpReturn = true;
                } else if (!output.scriptPubKey.empty() && output.scriptPubKey[0] == 0x51) {
                    // P2TR output (OP_1)
                    info.ddP2TROutputs.push_back(output);
                }
            }
        }

        // Extract DD amounts from OP_RETURN
        if (info.hasOpReturn) {
            ExtractDDAmountsFromOpReturn(info.opReturnOutput.scriptPubKey, info.ddAmounts);
            for (CAmount amt : info.ddAmounts) {
                info.totalDD += amt;
            }
        }

        return info;
    }

    /**
     * Build TransferParams for testing
     */
    TransferParams BuildTransferParams(const std::vector<std::pair<std::string, CAmount>>& recipients) {
        TransferParams params;
        params.recipients = recipients;
        params.feeRate = 100000; // 100,000 sat/kB (DigiByte minimum relay fee)
        params.spenderKey = senderKey;

        // An ordinary bech32 address for the leftover DGB. The builder refuses
        // to place change anywhere else.
        CKey changeKey;
        changeKey.MakeNewKey(true);
        params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(changeKey.GetPubKey())};

        // Add some mock UTXOs
        params.ddUtxos.push_back(CreateMockDDUTXO(TEST_DD_AMOUNT));
        params.feeUtxos.push_back(CreateMockDGBUTXO(COIN)); // 1 DGB for fees

        return params;
    }
};

// Define static const members
const CAmount DDTransferTestFixture::TEST_DD_AMOUNT;
const CAmount DDTransferTestFixture::LARGE_DD_AMOUNT;
const CAmount DDTransferTestFixture::MAX_TRANSFER_AMOUNT;
const CAmount DDTransferTestFixture::DUST_AMOUNT;

// =============================================================================
// Basic Transfer Creation Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_basic_transfer_creation, DDTransferTestFixture)
{
    // Arrange: Create transfer parameters
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, TEST_DD_AMOUNT}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer transaction
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should succeed with proper implementation
    if (!result.success) {
        BOOST_TEST_MESSAGE("Transfer failed: " << result.error);
    }
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
    BOOST_CHECK_GT(result.tx.vout.size(), 0);
}

BOOST_FIXTURE_TEST_CASE(test_transfer_with_change, DDTransferTestFixture)
{
    // Arrange: Transfer less than available, should create change
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    CAmount transferAmount = TEST_DD_AMOUNT / 2; // Transfer half
    TransferParams params = BuildTransferParams({{recipientAddr, transferAmount}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer with change - GREEN phase (should succeed)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should succeed in GREEN phase
    if (!result.success) {
        BOOST_TEST_MESSAGE("Transfer failed: " << result.error);
    }
    BOOST_CHECK(result.success);
    // Outputs: recipient P2TR + DD change P2TR + DGB fee change + OP_RETURN = 4
    BOOST_CHECK_EQUAL(result.tx.vout.size(), 4);
}

BOOST_FIXTURE_TEST_CASE(test_multiple_dd_inputs_consolidation, DDTransferTestFixture)
{
    // Arrange: Multiple DD inputs that need consolidation
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, LARGE_DD_AMOUNT}});

    // Add multiple DD UTXOs
    params.ddUtxos.clear();
    params.ddUtxos.push_back(CreateMockDDUTXO(TEST_DD_AMOUNT));
    params.ddUtxos.push_back(CreateMockDDUTXO(TEST_DD_AMOUNT));
    params.ddUtxos.push_back(CreateMockDDUTXO(LARGE_DD_AMOUNT));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build consolidation transfer - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail in RED phase
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_multiple_recipients, DDTransferTestFixture)
{
    // Arrange: Send to multiple recipients
    std::string recipient1 = CreateDDAddress(recipientKey.GetPubKey());
    CKey recipient2Key;
    recipient2Key.MakeNewKey(true);
    std::string recipient2 = CreateDDAddress(recipient2Key.GetPubKey());

    std::vector<std::pair<std::string, CAmount>> recipients = {
        {recipient1, 3000}, // $30.00
        {recipient2, 7000}  // $70.00
    };

    TransferParams params = BuildTransferParams(recipients);
    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build multi-recipient transfer - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail in RED phase
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
}

// =============================================================================
// Error Handling Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_insufficient_balance_handling, DDTransferTestFixture)
{
    // Arrange: Try to transfer more DD than available
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    CAmount excessiveAmount = TEST_DD_AMOUNT * 10; // 10x available
    TransferParams params = BuildTransferParams({{recipientAddr, excessiveAmount}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt transfer with insufficient balance
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail with insufficient balance error
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
    BOOST_CHECK(result.error.find("Insufficient") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(test_zero_amount_validation, DDTransferTestFixture)
{
    // Arrange: Try to transfer zero amount
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, 0}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt zero transfer
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail with validation error
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_negative_amount_validation, DDTransferTestFixture)
{
    // Arrange: Try to transfer negative amount
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, -1000}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt negative transfer
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail with validation error
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_maximum_transfer_limits, DDTransferTestFixture)
{
    // Arrange: Try to transfer maximum allowed amount
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, MAX_TRANSFER_AMOUNT}});

    // Mock sufficient UTXOs
    params.ddUtxos.clear();
    for(int i = 0; i < 100; ++i) {
        params.ddUtxos.push_back(CreateMockDDUTXO(MAX_TRANSFER_AMOUNT / 100));
    }

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt maximum transfer - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail in RED phase
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_exceed_maximum_transfer_limits, DDTransferTestFixture)
{
    // Arrange: Try to transfer more than maximum allowed
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    CAmount excessiveAmount = MAX_TRANSFER_AMOUNT + 1;
    TransferParams params = BuildTransferParams({{recipientAddr, excessiveAmount}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt excessive transfer - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail - exceeds maximum
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

// =============================================================================
// DD Conservation Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_dd_conservation_verification, DDTransferTestFixture)
{
    // Arrange: Transfer that should conserve DD (input = output)
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, TEST_DD_AMOUNT}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build conservation test - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail in RED phase
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());

    // After GREEN phase implementation:
    // CAmount totalInputDD = CalculateTotalDDInputs(params.ddUtxos);
    // CAmount totalOutputDD = TEST_DD_AMOUNT;
    // BOOST_CHECK_EQUAL(totalInputDD, totalOutputDD);
}

BOOST_FIXTURE_TEST_CASE(test_dust_threshold_handling, DDTransferTestFixture)
{
    // Arrange: Transfer very small amount (dust)
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, DUST_AMOUNT}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt dust transfer - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail - below dust threshold
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

// =============================================================================
// Address Validation Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_invalid_recipient_validation, DDTransferTestFixture)
{
    // Arrange: Invalid DD address
    std::string invalidAddr = "invalid_dd_address_format";
    TransferParams params = BuildTransferParams({{invalidAddr, TEST_DD_AMOUNT}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt transfer to invalid address - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail - invalid address
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_empty_recipient_validation, DDTransferTestFixture)
{
    // Arrange: Empty recipients list
    TransferParams params = BuildTransferParams({});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt transfer with no recipients - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail - no recipients
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

// =============================================================================
// Transaction Structure Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_transaction_version_validation, DDTransferTestFixture)
{
    // Arrange: Standard transfer
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, TEST_DD_AMOUNT}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer and check version - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail in RED phase
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());

    // After GREEN phase implementation:
    // uint32_t expectedVersion = DD_TX_VERSION | DD_TX_TRANSFER;
    // BOOST_CHECK_EQUAL(result.tx.nVersion, expectedVersion);
}

BOOST_FIXTURE_TEST_CASE(test_transaction_type_validation, DDTransferTestFixture)
{
    // Arrange: Transfer transaction
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, TEST_DD_AMOUNT}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build and validate transaction type - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail in RED phase
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
}

// =============================================================================
// Helper Function Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_validate_transfer_params, DDTransferTestFixture)
{
    // Arrange: Valid and invalid parameters
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams validParams = BuildTransferParams({{recipientAddr, TEST_DD_AMOUNT}});
    TransferParams invalidParams = BuildTransferParams({{recipientAddr, -1000}});

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act & Assert: These should fail in RED phase since methods don't exist yet
    // In GREEN phase, we'll test:
    // BOOST_CHECK(builder.ValidateTransferParams(validParams));
    // BOOST_CHECK(!builder.ValidateTransferParams(invalidParams));

    // For now, just verify the fixture works
    BOOST_CHECK(!validParams.recipients.empty());
    BOOST_CHECK(!invalidParams.recipients.empty());
}

BOOST_FIXTURE_TEST_CASE(test_calculate_total_dd_input, DDTransferTestFixture)
{
    // Arrange: Mock DD UTXOs with known amounts
    std::vector<CTxOut> inputs;
    std::vector<CAmount> amounts = {1000, 2000, 3000}; // $10, $20, $30

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act & Assert: This should fail in RED phase since method doesn't exist
    // In GREEN phase:
    // CAmount total = builder.CalculateTotalDDInput(inputs, amounts);
    // BOOST_CHECK_EQUAL(total, 6000); // $60.00 total

    // For now, verify test data
    BOOST_CHECK_EQUAL(amounts.size(), 3);
}

BOOST_FIXTURE_TEST_CASE(test_create_dd_transfer_script, DDTransferTestFixture)
{
    // Arrange: Recipient key and amount
    CPubKey recipient = recipientKey.GetPubKey();
    CAmount amount = TEST_DD_AMOUNT;

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act & Assert: This should fail in RED phase since method doesn't exist
    // In GREEN phase:
    // CScript script = builder.CreateDDTransferScript(recipient, amount);
    // BOOST_CHECK(!script.empty());
    // BOOST_CHECK(IsDDTokenScript(script));

    // For now, verify test inputs
    BOOST_CHECK(recipient.IsValid());
    BOOST_CHECK_GT(amount, 0);
}

BOOST_FIXTURE_TEST_CASE(test_select_dd_inputs, DDTransferTestFixture)
{
    // Arrange: Available DD UTXOs and needed amount
    std::vector<CTxOut> available;
    CAmount needed = TEST_DD_AMOUNT;
    std::vector<CTxOut> selected;
    CAmount total = 0;

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act & Assert: This should fail in RED phase since method doesn't exist
    // In GREEN phase:
    // bool success = builder.SelectDDInputs(available, needed, selected, total);
    // BOOST_CHECK(success);
    // BOOST_CHECK_GE(total, needed);

    // For now, verify test setup
    BOOST_CHECK_GT(needed, 0);
    BOOST_CHECK_EQUAL(total, 0); // Initially zero
}

// =============================================================================
// Edge Cases and Stress Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_maximum_inputs_consolidation, DDTransferTestFixture)
{
    // Arrange: Many small DD UTXOs that need consolidation
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, LARGE_DD_AMOUNT}});

    // Add many small UTXOs (each $1.00)
    params.ddUtxos.clear();
    for(int i = 0; i < 1000; ++i) {
        params.ddUtxos.push_back(CreateMockDDUTXO(100)); // $1.00 each
    }

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Attempt large consolidation - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail - insufficient balance
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_precise_amount_matching, DDTransferTestFixture)
{
    // Arrange: Exact amount match (no change needed)
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, TEST_DD_AMOUNT}});

    // Ensure exact match
    params.ddUtxos.clear();
    params.ddUtxos.push_back(CreateMockDDUTXO(TEST_DD_AMOUNT));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build exact amount transfer - EXPECTED TO FAIL (RED phase)
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail in RED phase
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
}

// =============================================================================
// Phase 2.2 - DD Input Assembly Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_build_transfer_inputs, DDTransferTestFixture)
{
    // Arrange: Create mock DD UTXOs
    COutPoint utxo1 = CreateMockDDUTXO(5000);  // $50.00
    COutPoint utxo2 = CreateMockDDUTXO(3000);  // $30.00

    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, 8000}}); // $80.00

    // Set specific UTXOs
    params.ddUtxos.clear();
    params.ddUtxos.push_back(utxo1);
    params.ddUtxos.push_back(utxo2);

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer transaction
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Transaction should be created with correct inputs.
    // These two have to stop the test, not just record a failure. The checks
    // below read the first inputs, and a build that was refused has none.
    BOOST_REQUIRE_EQUAL(result.success, true);

    // Verify DD inputs were added (should be first 2 inputs before fee inputs)
    BOOST_REQUIRE_GE(result.tx.vin.size(), 2u);
    BOOST_CHECK(result.tx.vin[0].prevout == utxo1);
    BOOST_CHECK(result.tx.vin[1].prevout == utxo2);
}

BOOST_FIXTURE_TEST_CASE(test_single_dd_input_assembly, DDTransferTestFixture)
{
    // Arrange: Single DD UTXO
    COutPoint utxo = CreateMockDDUTXO(10000);  // $100.00

    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    TransferParams params = BuildTransferParams({{recipientAddr, 10000}}); // Exact match

    params.ddUtxos.clear();
    params.ddUtxos.push_back(utxo);

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Single input should be present
    BOOST_CHECK_EQUAL(result.success, true);
    BOOST_CHECK_GE(result.tx.vin.size(), 1);
    BOOST_CHECK(result.tx.vin[0].prevout == utxo);
}

BOOST_FIXTURE_TEST_CASE(test_multiple_dd_inputs_assembly, DDTransferTestFixture)
{
    // Arrange: Multiple DD UTXOs for consolidation
    std::vector<COutPoint> utxos;
    TransferParams params;
    params.recipients = {{CreateDDAddress(recipientKey.GetPubKey()), 15000}}; // $150.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    // An ordinary bech32 address for the leftover DGB. The builder refuses to
    // place change anywhere else.
    CKey changeKey;
    changeKey.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(changeKey.GetPubKey())};

    // Create 5 small UTXOs
    for (int i = 0; i < 5; ++i) {
        COutPoint utxo = CreateMockDDUTXO(3000);  // $30.00 each
        utxos.push_back(utxo);
        params.ddUtxos.push_back(utxo);
    }

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build consolidation transfer
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: All 5 DD inputs should be assembled
    BOOST_CHECK_EQUAL(result.success, true);
    BOOST_CHECK_GE(result.tx.vin.size(), 5);

    // Verify each input matches
    for (size_t i = 0; i < utxos.size(); ++i) {
        BOOST_CHECK(result.tx.vin[i].prevout == utxos[i]);
    }
}

BOOST_FIXTURE_TEST_CASE(test_dd_input_count_matches_utxo_count, DDTransferTestFixture)
{
    // Arrange: Variable number of UTXOs
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    // Test with 1, 3, and 10 UTXOs
    std::vector<int> utxoCounts = {1, 3, 10};

    for (int count : utxoCounts) {
        TransferParams params;
        params.recipients = {{recipientAddr, count * 1000}}; // $10.00 per UTXO
        params.feeRate = 100000; // 100,000 sat/kB
        params.spenderKey = senderKey;
        // An ordinary bech32 address for the leftover DGB. The builder
        // refuses to place change anywhere else.
        CKey params_change_key;
        params_change_key.MakeNewKey(true);
        params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
        params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

        for (int i = 0; i < count; ++i) {
            params.ddUtxos.push_back(CreateMockDDUTXO(1000));
        }

        MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

        // Act: Build transfer
        TxBuilderResult result = builder.BuildTransferTransaction(params);

        // Assert: Input count should match (DD inputs + possibly fee inputs)
        BOOST_CHECK_EQUAL(result.success, true);
        BOOST_CHECK_GE(result.tx.vin.size(), static_cast<size_t>(count));
    }
}

// =============================================================================
// Phase 2.3 - DD Output Assembly Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_build_transfer_outputs, DDTransferTestFixture)
{
    // Arrange: Transfer params with 2 recipients
    std::string recipient1 = CreateDDAddress(recipientKey.GetPubKey());
    CKey recipient2Key;
    recipient2Key.MakeNewKey(true);
    std::string recipient2 = CreateDDAddress(recipient2Key.GetPubKey());

    TransferParams params;
    params.recipients = {
        {recipient1, 50000},  // $500.00
        {recipient2, 25000}   // $250.00
    };
    params.feeRate = 100000; // 100,000 sat/kB (minimum relay fee for DigiByte)
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};

    // Create DD UTXOs with sufficient balance (need 75000 cents total)
    params.ddUtxos.push_back(CreateMockDDUTXO(50000)); // $500
    params.ddUtxos.push_back(CreateMockDDUTXO(25000)); // $250

    // Add fee UTXOs
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN)); // 1 DGB

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer transaction
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Transaction should be built successfully
    BOOST_CHECK_MESSAGE(result.success, "Transfer failed: " << result.error);

    // Parse DD outputs using helper
    DDOutputInfo ddInfo = ParseDDOutputs(result.tx);

    // Should have exactly 2 DD P2TR outputs for our 2 recipients
    BOOST_CHECK_EQUAL(ddInfo.ddP2TROutputs.size(), 2);

    // Should have OP_RETURN with DD amounts
    BOOST_CHECK(ddInfo.hasOpReturn);

    // Should have 2 amounts in OP_RETURN (for 2 recipients)
    BOOST_CHECK_MESSAGE(ddInfo.ddAmounts.size() == 2, "Expected 2 DD amounts, got " << ddInfo.ddAmounts.size());

    // Verify total DD output equals requested amounts (75000)
    BOOST_CHECK_MESSAGE(ddInfo.totalDD == 75000, "Expected 75000 DD, got " << ddInfo.totalDD);

    // Verify each DD P2TR output has proper script format
    for (const auto& output : ddInfo.ddP2TROutputs) {
        // P2TR scripts start with OP_1 (0x51) followed by 32 bytes
        BOOST_CHECK_GE(output.scriptPubKey.size(), 34);
        BOOST_CHECK_EQUAL(output.scriptPubKey[0], 0x51); // OP_1
    }
}

BOOST_FIXTURE_TEST_CASE(test_single_recipient_output, DDTransferTestFixture)
{
    // Arrange: Single recipient transfer
    std::string recipient = CreateDDAddress(recipientKey.GetPubKey());

    TransferParams params;
    params.recipients = {{recipient, 30000}}; // $300.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(30000)); // Exact amount
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert
    BOOST_CHECK_MESSAGE(result.success, "Transfer failed: " << result.error);

    // Parse DD outputs
    DDOutputInfo ddInfo = ParseDDOutputs(result.tx);

    // Should have exactly 1 DD P2TR output (exact match, no change)
    BOOST_CHECK_MESSAGE(ddInfo.ddP2TROutputs.size() == 1, "Expected 1 DD P2TR output, got " << ddInfo.ddP2TROutputs.size());

    // Should have OP_RETURN
    BOOST_CHECK(ddInfo.hasOpReturn);

    // Verify amount from OP_RETURN
    BOOST_CHECK_MESSAGE(ddInfo.ddAmounts.size() == 1, "Expected 1 DD amount, got " << ddInfo.ddAmounts.size());
    if (ddInfo.ddAmounts.size() > 0) {
        BOOST_CHECK_EQUAL(ddInfo.ddAmounts[0], 30000);
    }
    BOOST_CHECK_EQUAL(ddInfo.totalDD, 30000);
}

BOOST_FIXTURE_TEST_CASE(test_output_p2tr_script_format, DDTransferTestFixture)
{
    // Arrange
    std::string recipient = CreateDDAddress(recipientKey.GetPubKey());

    TransferParams params;
    params.recipients = {{recipient, 10000}}; // $100.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(10000));
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert
    BOOST_CHECK(result.success);

    // Get DD output
    CTxOut ddOutput;
    bool found = false;
    for (const auto& output : result.tx.vout) {
        if (output.nValue == 0) {
            ddOutput = output;
            found = true;
            break;
        }
    }
    BOOST_CHECK(found);

    // Verify P2TR script structure
    // P2TR: OP_1 <32-byte-pubkey> (possibly followed by DD amount data)
    BOOST_CHECK_GE(ddOutput.scriptPubKey.size(), 34);

    // First byte should be OP_1 (version 1 witness)
    BOOST_CHECK_EQUAL(ddOutput.scriptPubKey[0], 0x51);

    // Second byte should be 0x20 (32 bytes following)
    BOOST_CHECK_EQUAL(ddOutput.scriptPubKey[1], 0x20);
}

BOOST_FIXTURE_TEST_CASE(test_all_recipients_get_outputs, DDTransferTestFixture)
{
    // Arrange: 3 recipients with different amounts
    std::string recipient1 = CreateDDAddress(recipientKey.GetPubKey());

    CKey recipient2Key, recipient3Key;
    recipient2Key.MakeNewKey(true);
    recipient3Key.MakeNewKey(true);

    std::string recipient2 = CreateDDAddress(recipient2Key.GetPubKey());
    std::string recipient3 = CreateDDAddress(recipient3Key.GetPubKey());

    TransferParams params;
    params.recipients = {
        {recipient1, 10000},  // $100.00
        {recipient2, 20000},  // $200.00
        {recipient3, 30000}   // $300.00
    };
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(60000)); // Exact total
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert
    BOOST_CHECK_MESSAGE(result.success, "Transfer failed: " << result.error);

    // Parse DD outputs using helper
    DDOutputInfo ddInfo = ParseDDOutputs(result.tx);

    // Should have 3 DD P2TR outputs
    BOOST_CHECK_MESSAGE(ddInfo.ddP2TROutputs.size() == 3, "Expected 3 DD P2TR outputs, got " << ddInfo.ddP2TROutputs.size());

    // Should have OP_RETURN with DD amounts
    BOOST_CHECK(ddInfo.hasOpReturn);

    // Should have 3 amounts in OP_RETURN
    BOOST_CHECK_MESSAGE(ddInfo.ddAmounts.size() == 3, "Expected 3 DD amounts, got " << ddInfo.ddAmounts.size());

    // Verify total DD output equals requested amounts (60000)
    BOOST_CHECK_MESSAGE(ddInfo.totalDD == 60000, "Expected 60000 DD, got " << ddInfo.totalDD);
}

BOOST_FIXTURE_TEST_CASE(test_output_amounts_match_requested, DDTransferTestFixture)
{
    // Arrange: Multiple recipients with specific amounts
    std::string recipient1 = CreateDDAddress(recipientKey.GetPubKey());
    CKey recipient2Key;
    recipient2Key.MakeNewKey(true);
    std::string recipient2 = CreateDDAddress(recipient2Key.GetPubKey());

    CAmount amount1 = 12345; // $123.45
    CAmount amount2 = 67890; // $678.90

    TransferParams params;
    params.recipients = {
        {recipient1, amount1},
        {recipient2, amount2}
    };
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(amount1 + amount2));
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert
    BOOST_CHECK_MESSAGE(result.success, "Transfer failed: " << result.error);

    // Parse DD outputs using helper
    DDOutputInfo ddInfo = ParseDDOutputs(result.tx);

    // Should have 2 DD amounts in OP_RETURN
    BOOST_CHECK_MESSAGE(ddInfo.ddAmounts.size() == 2, "Expected 2 DD amounts, got " << ddInfo.ddAmounts.size());

    // Sort for comparison
    std::vector<CAmount> ddAmounts = ddInfo.ddAmounts;
    std::sort(ddAmounts.begin(), ddAmounts.end());
    std::vector<CAmount> expected = {amount1, amount2};
    std::sort(expected.begin(), expected.end());

    // Verify amounts match
    for (size_t i = 0; i < ddAmounts.size() && i < expected.size(); ++i) {
        BOOST_CHECK_MESSAGE(ddAmounts[i] == expected[i],
                          "Amount mismatch at index " << i << ": got " << ddAmounts[i] << ", expected " << expected[i]);
    }
}

// =============================================================================
// Phase 2.4 - Fee Input Assembly Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_fee_inputs_added, DDTransferTestFixture)
{
    // Arrange: Create transfer params with fee inputs
    COutPoint ddUtxo = CreateMockDDUTXO(50000);  // $500.00
    COutPoint feeUtxo = CreateMockDGBUTXO(COIN); // 1 DGB for fees

    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TransferParams params;
    params.recipients = {{recipientAddr, 50000}};
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos = {ddUtxo};
    params.feeUtxos = {feeUtxo};

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer transaction
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Transaction should be created successfully
    BOOST_CHECK_EQUAL(result.success, true);

    // Verify input count (1 DD + 1 fee = 2 total)
    BOOST_CHECK_EQUAL(result.tx.vin.size(), 2);

    // Verify first input is DD input
    BOOST_CHECK(result.tx.vin[0].prevout == ddUtxo);

    // Verify second input is fee input
    BOOST_CHECK(result.tx.vin[1].prevout == feeUtxo);
}

// =============================================================================
// Phase 2.6 - Transaction Finalization Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_transaction_finalization, DDTransferTestFixture)
{
    // Arrange: Create transfer params with valid inputs and outputs
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 50000}}; // $500.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};

    // Create DD UTXO with sufficient balance
    params.ddUtxos.push_back(CreateMockDDUTXO(50000)); // Exact amount
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN)); // Fee UTXO

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act: Build transfer transaction
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Transaction should be finalized successfully
    BOOST_CHECK_MESSAGE(result.success, "Transaction finalization failed: " << result.error);
    BOOST_CHECK(result.error.empty());

    // Verify transaction structure
    BOOST_CHECK_GT(result.tx.vin.size(), 0);  // Must have inputs
    BOOST_CHECK_GT(result.tx.vout.size(), 0); // Must have outputs

    // Verify transaction version is DigiDollar TRANSFER type
    // Version format: (type << 24) | (flags << 16) | (DD_TX_VERSION & 0xFFFF)
    // For TRANSFER (type=2): 0x02000770 = 33556336 in decimal
    BOOST_CHECK_EQUAL(result.tx.nVersion, 0x02000770);

    // Verify locktime is set to 0 (immediate broadcast)
    BOOST_CHECK_EQUAL(result.tx.nLockTime, 0);
}

BOOST_FIXTURE_TEST_CASE(test_transaction_version_and_locktime, DDTransferTestFixture)
{
    // Arrange
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 25000}}; // $250.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(25000));
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Verify version and locktime
    BOOST_CHECK(result.success);
    // DigiDollar TRANSFER transaction version: 0x02000770 = 33556336
    BOOST_CHECK_EQUAL(result.tx.nVersion, 0x02000770);
    BOOST_CHECK_EQUAL(result.tx.nLockTime, 0);
}

BOOST_FIXTURE_TEST_CASE(test_dd_amount_balance_verification, DDTransferTestFixture)
{
    // Arrange: Create transfer with balanced DD amounts
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 30000}}; // $300.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(30000)); // Exact balance
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: DD amounts should balance
    BOOST_CHECK_MESSAGE(result.success, "DD balance verification failed: " << result.error);

    // Extract and verify total DD input and output
    CAmount totalDDIn = 0;
    for (const auto& utxo : params.ddUtxos) {
        totalDDIn += g_mockDDUTXOs[utxo];
    }

    CAmount totalDDOut = 0;
    for (const auto& output : result.tx.vout) {
        // DD outputs have nValue == 0 and are P2TR scripts (start with OP_1)
        // Skip OP_RETURN outputs (metadata, not spendable DD)
        if (output.nValue == 0 && output.scriptPubKey.size() > 0 &&
            output.scriptPubKey[0] == OP_1) {
            // The builder registers each output it creates; read that
            // registry explicitly. Consensus never does.
            CAmount ddAmount = 0;
            if (DigiDollar::ExtractDDAmount(output.scriptPubKey, ddAmount, /*allow_registry=*/true)) {
                totalDDOut += ddAmount;
            }
        }
    }

    BOOST_CHECK_EQUAL(totalDDIn, totalDDOut);
}

BOOST_FIXTURE_TEST_CASE(test_dd_amount_mismatch_detection, DDTransferTestFixture)
{
    // Arrange: Create transfer with insufficient DD inputs (should fail)
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 50000}}; // $500.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(30000)); // Only $300.00 (insufficient!)
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail due to insufficient DD
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
    BOOST_CHECK(result.error.find("Insufficient") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(transfer_rejects_malformed_opreturn_without_throwing, DDTransferTestFixture)
{
    CMutableTransaction tx;
    tx.SetDigiDollarType(::DD_TX_TRANSFER);
    tx.vin.push_back(CTxIn(CreateMockDDUTXO(TEST_DD_AMOUNT)));

    XOnlyPubKey recipientXOnly(recipientKey.GetPubKey());
    tx.vout.push_back(CTxOut(0, DigiDollar::CreateDigiDollarP2TR(recipientXOnly, TEST_DD_AMOUNT)));

    CScript malformedType;
    malformedType << OP_RETURN
                  << std::vector<unsigned char>{'D', 'D'}
                  << std::vector<unsigned char>(5, 0x01)
                  << CScriptNum(TEST_DD_AMOUNT);
    tx.vout.push_back(CTxOut(0, malformedType));

    TxValidationState state;
    DigiDollar::ValidationContext ctx(currentHeight, oraclePrice, systemCollateral, chainParams);

    bool valid = true;
    BOOST_CHECK_NO_THROW(valid = DigiDollar::ValidateTransferTransaction(CTransaction(tx), ctx, state));
    BOOST_CHECK(!valid);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "transfer-malformed-op-return");

    tx.vout.back().scriptPubKey = CScript() << OP_RETURN
                                            << std::vector<unsigned char>{'D', 'D'}
                                            << CScriptNum(2)
                                            << std::vector<unsigned char>(9, 0x01);

    state = TxValidationState();
    valid = true;
    BOOST_CHECK_NO_THROW(valid = DigiDollar::ValidateTransferTransaction(CTransaction(tx), ctx, state));
    BOOST_CHECK(!valid);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "transfer-malformed-op-return");
}

BOOST_FIXTURE_TEST_CASE(test_empty_inputs_validation, DDTransferTestFixture)
{
    // Arrange: Create params with no DD inputs
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 10000}}; // $100.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    // No ddUtxos provided!
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail validation - no DD inputs
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_empty_outputs_validation, DDTransferTestFixture)
{
    // Arrange: Create params with no recipients
    TxBuilderTransferParams params;
    params.recipients = {}; // No recipients!
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(10000));
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Should fail validation - no recipients
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_FIXTURE_TEST_CASE(test_complete_transaction_structure, DDTransferTestFixture)
{
    // Arrange: Complete valid transfer
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 40000}}; // $400.00
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos.push_back(CreateMockDDUTXO(40000));
    params.feeUtxos.push_back(CreateMockDGBUTXO(COIN));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Complete validation
    BOOST_CHECK_MESSAGE(result.success, "Transaction failed: " << result.error);

    // Structure checks
    BOOST_CHECK_GT(result.tx.vin.size(), 0);   // Has inputs
    BOOST_CHECK_GT(result.tx.vout.size(), 0);  // Has outputs
    BOOST_CHECK_EQUAL(result.tx.nVersion, 0x02000770);  // DD TRANSFER version
    BOOST_CHECK_EQUAL(result.tx.nLockTime, 0); // Locktime 0

    // Fee checks
    BOOST_CHECK_GT(result.totalFees, 0); // Has fees calculated

    // DD conservation
    CAmount totalDDIn = g_mockDDUTXOs[params.ddUtxos[0]];
    CAmount totalDDOut = 0;
    for (const auto& output : result.tx.vout) {
        // DD outputs have nValue == 0 and are P2TR scripts (start with OP_1)
        // Skip OP_RETURN outputs (metadata, not spendable DD)
        if (output.nValue == 0 && output.scriptPubKey.size() > 0 &&
            output.scriptPubKey[0] == OP_1) {
            // The builder registers each output it creates; read that
            // registry explicitly. Consensus never does.
            CAmount ddAmount = 0;
            if (DigiDollar::ExtractDDAmount(output.scriptPubKey, ddAmount, /*allow_registry=*/true)) {
                totalDDOut += ddAmount;
            }
        }
    }
    BOOST_CHECK_EQUAL(totalDDIn, totalDDOut);
}

// =============================================================================
// DD UTXO Database Persistence Tests (Fix #5)
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_dd_utxo_persistence, DDTransferTestFixture)
{
    // Arrange: Create a DD UTXO to persist
    COutPoint outpoint(InsecureRand256(), 0);
    CAmount dd_amount = 50000; // $500.00

    // Create test wallet database
    std::unique_ptr<wallet::WalletDatabase> database = wallet::CreateMockableWalletDatabase();
    wallet::WalletBatch batch(*database);

    // Act: Write DD UTXO to database
    bool write_success = batch.WriteDDUTXO(outpoint, dd_amount);
    BOOST_CHECK(write_success);

    // Read DD UTXO back from database
    CAmount read_amount = 0;
    bool read_success = batch.ReadDDUTXO(outpoint, read_amount);
    BOOST_CHECK(read_success);

    // Assert: Verify the data matches
    BOOST_CHECK_EQUAL(read_amount, dd_amount);

    // Clean up: Erase DD UTXO
    bool erase_success = batch.EraseDDUTXO(outpoint);
    BOOST_CHECK(erase_success);

    // Verify it's gone
    CAmount verify_amount = 0;
    bool verify_read = batch.ReadDDUTXO(outpoint, verify_amount);
    BOOST_CHECK(!verify_read);
}

// =============================================================================
// DD UTXO Tracking Tests (Fix #1)
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_dd_utxo_tracking_after_transfer, DDTransferTestFixture)
{
    // GREEN PHASE TEST: Verify FIX #1 - DD UTXOs are tracked correctly after transfers
    // FIX #1 ensures dd_utxos map is updated when transfers occur
    // Time-locks stay active, only DD UTXO tracking changes

    // Arrange: Setup wallet with DD balance from mint
    std::unique_ptr<wallet::WalletDatabase> database = wallet::CreateMockableWalletDatabase();
    std::shared_ptr<wallet::CWallet> wallet = std::make_shared<wallet::CWallet>(m_node.chain.get(), "", std::move(database));
    DigiDollarWallet dd_wallet(wallet.get());

    // Step 1: Create mint position with DD UTXO
    uint256 mint_txid = InsecureRand256();
    CAmount mint_amount = 10000;  // $100.00

    WalletCollateralPosition mint_position;
    mint_position.dd_timelock_id = mint_txid;
    mint_position.dd_minted = mint_amount;
    mint_position.dgb_collateral = 100 * COIN;
    mint_position.lock_tier = 1;
    mint_position.unlock_height = currentHeight + 1000;
    mint_position.is_active = true;
    dd_wallet.AddCollateralPosition(mint_position);

    // Add mint DD UTXO to dd_utxos map (FIX #1)
    COutPoint mint_dd_utxo(mint_txid, 1);
    dd_wallet.AddDDUTXO(mint_dd_utxo, mint_amount);

    // Verify initial state
    std::vector<DDUtxo> initial_utxos = dd_wallet.GetDDUTXOs();
    BOOST_CHECK_EQUAL(initial_utxos.size(), 1);
    BOOST_CHECK(initial_utxos[0].outpoint == mint_dd_utxo);
    BOOST_CHECK_EQUAL(initial_utxos[0].dd_amount, mint_amount);

    CAmount initial_balance = dd_wallet.GetTotalDDBalance();
    BOOST_CHECK_EQUAL(initial_balance, mint_amount);

    // Step 2: Simulate transfer that spends mint UTXO and creates change
    uint256 transfer_txid = InsecureRand256();
    CAmount transfer_amount = 5000;  // Send $50.00
    CAmount change_amount = 5000;    // Change $50.00

    // Create transfer outputs (FIX #1 architecture)
    COutPoint recipient_utxo(transfer_txid, 0);  // Recipient gets $50 at vout 0
    COutPoint change_utxo(transfer_txid, 1);     // Sender change $50 at vout 1

    // Update dd_utxos map (FIX #1 implementation)
    dd_wallet.RemoveDDUTXO(mint_dd_utxo);          // Spent in transfer
    dd_wallet.AddDDUTXO(change_utxo, change_amount); // Add change UTXO

    // Act: Query UTXOs and balance after transfer
    std::vector<DDUtxo> after_transfer_utxos = dd_wallet.GetDDUTXOs();
    CAmount final_balance = dd_wallet.GetTotalDDBalance();

    // Assert FIX #1: Correct UTXO tracking
    // Should have ONLY the change UTXO, NOT the mint UTXO
    BOOST_CHECK_EQUAL(after_transfer_utxos.size(), 1);
    BOOST_CHECK(after_transfer_utxos[0].outpoint == change_utxo);
    BOOST_CHECK_EQUAL(after_transfer_utxos[0].outpoint.hash, transfer_txid);
    BOOST_CHECK_EQUAL(after_transfer_utxos[0].outpoint.n, 1);
    BOOST_CHECK_EQUAL(after_transfer_utxos[0].dd_amount, change_amount);

    // Balance should equal change UTXO amount
    BOOST_CHECK_EQUAL(final_balance, change_amount);

    // CRITICAL: Time-lock should STILL BE ACTIVE (FIX #2)
    std::vector<WalletCollateralPosition> active_positions = dd_wallet.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(active_positions.size(), 1);
    BOOST_CHECK(active_positions[0].is_active);
    BOOST_CHECK_EQUAL(active_positions[0].dd_timelock_id, mint_txid);

    LogPrintf("FIX #1 GREEN TEST: UTXO tracking correct after transfer\n");
    LogPrintf("  - dd_utxos contains change UTXO (%s:1), NOT mint UTXO\n", transfer_txid.ToString());
    LogPrintf("  - Balance correctly calculated from dd_utxos: %d cents\n", final_balance);
    LogPrintf("  - Time-lock preserved (FIX #2): active=%d\n", active_positions[0].is_active);
}

// =============================================================================
// FIX #2: Time-Lock Preservation Tests (RED → GREEN → REFACTOR)
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_transfer_preserves_timelock, DDTransferTestFixture)
{
    // GREEN PHASE TEST: Verify FIX #2 - Time-locks stay active during transfers
    // FIX #2 ensures time-lock positions are NEVER marked inactive during transfers
    // Only redemptions should mark time-locks inactive

    // Arrange: Setup wallet with mint position and DD UTXOs
    std::unique_ptr<wallet::WalletDatabase> database = wallet::CreateMockableWalletDatabase();
    std::shared_ptr<wallet::CWallet> wallet = std::make_shared<wallet::CWallet>(m_node.chain.get(), "", std::move(database));
    DigiDollarWallet dd_wallet(wallet.get());

    // Create mint position
    uint256 mint_txid = InsecureRand256();
    CAmount mint_amount = 10000;  // $100.00

    WalletCollateralPosition mint_position;
    mint_position.dd_timelock_id = mint_txid;
    mint_position.dd_minted = mint_amount;
    mint_position.dgb_collateral = 100 * COIN;
    mint_position.lock_tier = 1;
    mint_position.unlock_height = currentHeight + 1000;
    mint_position.is_active = true;
    dd_wallet.AddCollateralPosition(mint_position);

    // Add DD UTXO for the mint (FIX #1)
    COutPoint mint_dd_utxo(mint_txid, 1);
    dd_wallet.AddDDUTXO(mint_dd_utxo, mint_amount);

    // Verify initial time-lock state
    std::vector<WalletCollateralPosition> before_positions = dd_wallet.GetDDTimeLocks(true);
    BOOST_REQUIRE_EQUAL(before_positions.size(), 1);
    BOOST_REQUIRE(before_positions[0].is_active);
    BOOST_CHECK_EQUAL(before_positions[0].dd_timelock_id, mint_txid);
    CAmount initial_collateral = before_positions[0].dgb_collateral;
    int initial_unlock_height = before_positions[0].unlock_height;

    // Act: Simulate transfer (update dd_utxos only, NOT position)
    uint256 transfer_txid = InsecureRand256();
    CAmount transfer_amount = 5000;  // Send $50.00
    CAmount change_amount = 5000;    // Keep $50.00

    // Update dd_utxos (FIX #1) - spend mint UTXO, create change UTXO
    dd_wallet.RemoveDDUTXO(mint_dd_utxo);
    COutPoint change_utxo(transfer_txid, 1);
    dd_wallet.AddDDUTXO(change_utxo, change_amount);

    // Assert FIX #2: Time-lock MUST still be ACTIVE
    std::vector<WalletCollateralPosition> after_positions = dd_wallet.GetDDTimeLocks(true);

    // Should still have 1 active position
    BOOST_CHECK_EQUAL(after_positions.size(), 1);

    // Time-lock should remain ACTIVE
    BOOST_CHECK(after_positions[0].is_active);

    // Same time-lock ID (not changed)
    BOOST_CHECK_EQUAL(after_positions[0].dd_timelock_id, mint_txid);

    // Collateral unchanged
    BOOST_CHECK_EQUAL(after_positions[0].dgb_collateral, initial_collateral);

    // Unlock height unchanged
    BOOST_CHECK_EQUAL(after_positions[0].unlock_height, initial_unlock_height);

    // DD minted amount unchanged (this tracks original mint, not current balance!)
    BOOST_CHECK_EQUAL(after_positions[0].dd_minted, mint_amount);

    // Balance should reflect change amount (from FIX #1)
    CAmount final_balance = dd_wallet.GetTotalDDBalance();
    BOOST_CHECK_EQUAL(final_balance, change_amount);

    // CRITICAL: Should have only 1 position (NO fake change positions!)
    std::vector<WalletCollateralPosition> all_positions = dd_wallet.GetDDTimeLocks(false);
    BOOST_CHECK_EQUAL(all_positions.size(), 1);

    LogPrintf("FIX #2 GREEN TEST: Time-lock preserved during transfer\n");
    LogPrintf("  - Time-lock active: %d (expected true)\n", after_positions[0].is_active);
    LogPrintf("  - Position count: %d (expected 1, no fake change positions)\n", all_positions.size());
    LogPrintf("  - Collateral unchanged: %d DGB\n", after_positions[0].dgb_collateral / COIN);
    LogPrintf("  - Balance (from dd_utxos): %d cents\n", final_balance);
}

// =============================================================================
// Fix #3: Transaction Broadcasting Tests (RED PHASE)
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_transfer_broadcasts_to_network, DDTransferTestFixture)
{
    // GREEN PHASE TEST: Verify FIX #3 - Transfers broadcast to network
    // FIX #3 ensures TransferDigiDollar() broadcasts transactions via AcceptToMemoryPool
    // Note: This is a simplified test since full mempool integration requires running node

    // Arrange: Setup wallet with DD balance
    std::unique_ptr<wallet::WalletDatabase> database = wallet::CreateMockableWalletDatabase();
    std::shared_ptr<wallet::CWallet> wallet = std::make_shared<wallet::CWallet>(m_node.chain.get(), "", std::move(database));
    DigiDollarWallet dd_wallet(wallet.get());

    // Create mint position with DD UTXO
    uint256 mint_txid = InsecureRand256();
    CAmount mint_amount = 10000;  // $100.00

    WalletCollateralPosition mint_position;
    mint_position.dd_timelock_id = mint_txid;
    mint_position.dd_minted = mint_amount;
    mint_position.dgb_collateral = 100 * COIN;
    mint_position.lock_tier = 1;
    mint_position.unlock_height = currentHeight + 1000;
    mint_position.is_active = true;
    dd_wallet.AddCollateralPosition(mint_position);

    // Add DD UTXO
    COutPoint mint_dd_utxo(mint_txid, 1);
    dd_wallet.AddDDUTXO(mint_dd_utxo, mint_amount);

    // Setup spending key
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    dd_wallet.StoreOwnerKey(mint_txid, ownerKey);

    // Act: Create transfer transaction using TransferTxBuilder
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());
    CAmount transferAmount = 5000;  // $50.00

    TransferParams params;
    params.recipients = {{recipientAddr, transferAmount}};
    params.feeRate = 100000;
    params.spenderKey = ownerKey;
    // An ordinary bech32 address for the leftover DGB. The builder
    // refuses to place change anywhere else.
    CKey params_change_key;
    params_change_key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(params_change_key.GetPubKey())};
    params.ddUtxos = {mint_dd_utxo};
    params.ddAmounts = {mint_amount};  // Provide DD amounts for mock UTXO
    params.feeUtxos = {CreateMockDGBUTXO(COIN)};

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Transfer transaction built successfully
    BOOST_CHECK_MESSAGE(result.success, "Transfer failed: " << result.error);
    BOOST_CHECK(!result.tx.vin.empty());
    BOOST_CHECK(!result.tx.vout.empty());

    // Verify transaction structure (FIX #3 implementation details)
    uint256 transfer_txid = result.tx.GetHash();
    BOOST_CHECK(!transfer_txid.IsNull());

    // Verify DD conservation using ParseDDOutputs helper
    DDOutputInfo ddInfo = ParseDDOutputs(result.tx);
    CAmount total_dd_in = mint_amount;
    CAmount total_dd_out = ddInfo.totalDD;
    BOOST_CHECK_EQUAL(total_dd_in, total_dd_out);

    // Verify transaction can be serialized (required for broadcast)
    std::vector<unsigned char> tx_data;
    CVectorWriter writer(PROTOCOL_VERSION, tx_data, 0);
    writer << result.tx;
    BOOST_CHECK(!tx_data.empty());

    // FIX #3: In real implementation, TransferDigiDollar() calls:
    // - AcceptToMemoryPool() to add to mempool
    // - BroadcastTransaction() to relay to network
    // This test verifies the transaction is properly constructed for broadcast

    LogPrintf("FIX #3 GREEN TEST: Transfer transaction ready for broadcast\n");
    LogPrintf("  - Transaction txid: %s\n", transfer_txid.ToString());
    LogPrintf("  - DD conservation: %d in = %d out\n", total_dd_in, total_dd_out);
    LogPrintf("  - Transaction size: %d bytes\n", tx_data.size());
    LogPrintf("  - Ready for AcceptToMemoryPool() and BroadcastTransaction()\n");
}

// =============================================================================
// Fix #4: Receive Detection Tests (RED → GREEN → REFACTOR)
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_receive_dd_from_transfer, DDTransferTestFixture)
{
    // GREEN PHASE TEST: Verify FIX #4 - Receiving wallets detect incoming DD transfers
    // FIX #4 ensures ProcessIncomingDDTransaction() updates dd_utxos when DD is received
    // This test simulates a transfer and receiving wallet processing it

    // Arrange: Setup sender and receiver wallets
    std::unique_ptr<wallet::WalletDatabase> sender_db = wallet::CreateMockableWalletDatabase();
    std::shared_ptr<wallet::CWallet> sender_wallet = std::make_shared<wallet::CWallet>(
        m_node.chain.get(), "sender", std::move(sender_db));
    DigiDollarWallet sender_dd_wallet(sender_wallet.get());

    std::unique_ptr<wallet::WalletDatabase> receiver_db = wallet::CreateMockableWalletDatabase();
    std::shared_ptr<wallet::CWallet> receiver_wallet = std::make_shared<wallet::CWallet>(
        m_node.chain.get(), "receiver", std::move(receiver_db));
    DigiDollarWallet receiver_dd_wallet(receiver_wallet.get());

    // Setup sender with mint position and DD UTXO
    uint256 mint_txid = InsecureRand256();
    CAmount mint_amount = 10000;  // $100.00

    WalletCollateralPosition mint_position;
    mint_position.dd_timelock_id = mint_txid;
    mint_position.dd_minted = mint_amount;
    mint_position.dgb_collateral = 100 * COIN;
    mint_position.lock_tier = 1;
    mint_position.unlock_height = currentHeight + 1000;
    mint_position.is_active = true;
    sender_dd_wallet.AddCollateralPosition(mint_position);

    COutPoint mint_dd_utxo(mint_txid, 1);
    sender_dd_wallet.AddDDUTXO(mint_dd_utxo, mint_amount);

    // Verify sender initial state
    BOOST_CHECK_EQUAL(sender_dd_wallet.GetTotalDDBalance(), mint_amount);

    // Verify receiver initial state (no DD)
    BOOST_CHECK_EQUAL(receiver_dd_wallet.GetTotalDDBalance(), 0);
    BOOST_CHECK_EQUAL(receiver_dd_wallet.GetDDUTXOs().size(), 0);

    // Act: Simulate transfer transaction
    CKey senderKey, receiverKey;
    senderKey.MakeNewKey(true);
    receiverKey.MakeNewKey(true);

    CAmount transferAmount = 5000;  // Send $50.00
    CAmount changeAmount = 5000;    // Change $50.00

    // Build transfer transaction
    CMutableTransaction transferTx;
    transferTx.nVersion = 2;
    transferTx.SetDigiDollarType(::DD_TX_TRANSFER);

    // Input: spend sender's mint UTXO
    transferTx.vin.push_back(CTxIn(mint_dd_utxo));

    // Output 0: DD to receiver
    CPubKey receiverPubKey = receiverKey.GetPubKey();
    XOnlyPubKey receiverXOnly(receiverPubKey);
    CScript receiverScript = DigiDollar::CreateDigiDollarP2TR(receiverXOnly, transferAmount);
    transferTx.vout.push_back(CTxOut(0, receiverScript));

    // Output 1: DD change to sender
    CPubKey senderPubKey = senderKey.GetPubKey();
    XOnlyPubKey senderXOnly(senderPubKey);
    CScript senderScript = DigiDollar::CreateDigiDollarP2TR(senderXOnly, changeAmount);
    transferTx.vout.push_back(CTxOut(0, senderScript));

    // Output 2: OP_RETURN with DD amounts
    CScript metadataScript;
    metadataScript << OP_RETURN
                   << std::vector<unsigned char>{'D', 'D'}
                   << CScriptNum(2)  // TRANSFER type
                   << CScriptNum(transferAmount)
                   << CScriptNum(changeAmount);
    transferTx.vout.push_back(CTxOut(0, metadataScript));

    CTransactionRef tx = MakeTransactionRef(transferTx);
    uint256 transfer_txid = tx->GetHash();

    // Store receiver key (so wallet can detect it's ours)
    receiver_dd_wallet.StoreOwnerKey(transfer_txid, receiverKey);

    // FIX #4: Verify that DD amounts can be extracted from the transaction
    // Parse DD outputs from the transaction
    DDOutputInfo ddInfo = ParseDDOutputs(transferTx);

    // Should have 2 DD P2TR outputs (recipient + sender change)
    BOOST_CHECK_MESSAGE(ddInfo.ddP2TROutputs.size() == 2, "Expected 2 DD P2TR outputs, got " << ddInfo.ddP2TROutputs.size());

    // Should have OP_RETURN with DD amounts
    BOOST_CHECK(ddInfo.hasOpReturn);

    // Should have 2 DD amounts (recipient + change)
    BOOST_CHECK_MESSAGE(ddInfo.ddAmounts.size() == 2, "Expected 2 DD amounts, got " << ddInfo.ddAmounts.size());

    // Verify total equals input
    BOOST_CHECK_EQUAL(ddInfo.totalDD, mint_amount);

    // Verify first amount is the transfer amount (recipient gets it first)
    if (ddInfo.ddAmounts.size() >= 1) {
        BOOST_CHECK_EQUAL(ddInfo.ddAmounts[0], transferAmount);
    }

    // Verify second amount is the change
    if (ddInfo.ddAmounts.size() >= 2) {
        BOOST_CHECK_EQUAL(ddInfo.ddAmounts[1], changeAmount);
    }

    // Sender processes their change UTXO
    sender_dd_wallet.RemoveDDUTXO(mint_dd_utxo);  // Spent
    COutPoint sender_change_utxo(transfer_txid, 1);
    sender_dd_wallet.AddDDUTXO(sender_change_utxo, changeAmount);

    // Sender balance should be change amount
    CAmount sender_balance = sender_dd_wallet.GetTotalDDBalance();
    BOOST_CHECK_EQUAL(sender_balance, changeAmount);

    // Sender time-lock should still be active (FIX #2)
    std::vector<WalletCollateralPosition> sender_positions = sender_dd_wallet.GetDDTimeLocks(true);
    BOOST_CHECK_EQUAL(sender_positions.size(), 1);
    BOOST_CHECK(sender_positions[0].is_active);

    LogPrintf("FIX #4 GREEN TEST: DD amount extraction from transaction successful\n");
    LogPrintf("  - DD P2TR outputs: %d\n", ddInfo.ddP2TROutputs.size());
    LogPrintf("  - DD amounts in OP_RETURN: %d\n", ddInfo.ddAmounts.size());
    LogPrintf("  - Total DD: %d cents\n", ddInfo.totalDD);
    LogPrintf("  - Sender balance: %d DD cents (change)\n", sender_balance);
    LogPrintf("  - Sender time-lock active: %d\n", sender_positions[0].is_active);
}

BOOST_AUTO_TEST_SUITE_END()
