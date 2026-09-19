// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <test/util/random.h>

#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <digidollar/digidollar.h>
#include <consensus/digidollar.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <key.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <map>

using namespace DigiDollar;

// External mock UTXO stores (shared with transfer tests)
extern std::map<COutPoint, CAmount> g_mockDDUTXOs;
extern std::map<COutPoint, CAmount> g_mockDGBUTXOs;

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

BOOST_FIXTURE_TEST_SUITE(digidollar_change_tests, TestingSetup)

/**
 * Test fixture for DigiDollar change output tests
 */
struct DDChangeTestFixture : public TestingSetup {
    const CChainParams& chainParams;
    CKey senderKey;
    CKey recipientKey;
    int currentHeight;
    CAmount oraclePrice;

    DDChangeTestFixture() : chainParams(m_node.chainman->GetParams()) {
        senderKey.MakeNewKey(true);
        recipientKey.MakeNewKey(true);
        currentHeight = 100000;
        oraclePrice = 2500; // $25.00 per DGB

        // Clear mock UTXO stores
        g_mockDDUTXOs.clear();
        g_mockDGBUTXOs.clear();
    }

    COutPoint CreateMockDDUTXO(CAmount ddAmount) {
        COutPoint outpoint(InsecureRand256(), 0);
        g_mockDDUTXOs[outpoint] = ddAmount;
        return outpoint;
    }

    COutPoint CreateMockDGBUTXO(CAmount dgbAmount) {
        COutPoint outpoint(InsecureRand256(), 1);
        g_mockDGBUTXOs[outpoint] = dgbAmount;
        return outpoint;
    }

    // An ordinary bech32 address, the kind a wallet hands out for leftover DGB.
    // The builder refuses to place change anywhere else.
    CTxDestination CreateChangeDestination() {
        CKey key;
        key.MakeNewKey(true);
        return CTxDestination{WitnessV0KeyHash(key.GetPubKey())};
    }

    std::string CreateDDAddress(const CPubKey& pubkey) {
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
};

// =============================================================================
// Phase 2.5 - Change Output Creation Tests
// =============================================================================

BOOST_FIXTURE_TEST_CASE(test_dd_change_output, DDChangeTestFixture)
{
    // Arrange: Select 1000 DD, send 600 DD, expect 400 DD change
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 60000}}; // Send 600 DD ($600.00)
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    params.dgbChangeDest = CreateChangeDestination();
    params.ddUtxos.push_back(CreateMockDDUTXO(100000)); // 1000 DD ($1000.00)
    params.feeUtxos.push_back(CreateMockDGBUTXO(20000000)); // 0.2 DGB for 0.1 DGB min fee + change

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert
    BOOST_REQUIRE_EQUAL(result.success, true);
    BOOST_CHECK_MESSAGE(result.error.empty(), "Unexpected error: " << result.error);

    // Find OP_RETURN output and extract DD amounts
    std::vector<CAmount> ddAmounts;
    bool found_opreturn = false;
    for (const auto& output : result.tx.vout) {
        if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
            found_opreturn = ExtractDDAmountsFromOpReturn(output.scriptPubKey, ddAmounts);
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE(found_opreturn, "No OP_RETURN found in transaction");

    // Debug: Count P2TR DD outputs
    int dd_p2tr_count = 0;
    for (const auto& output : result.tx.vout) {
        if (output.nValue == 0 && output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == 0x51) {
            dd_p2tr_count++;
        }
    }
    if (ddAmounts.size() != dd_p2tr_count) {
        BOOST_TEST_MESSAGE("WARNING: OP_RETURN amounts (" << ddAmounts.size() << ") != P2TR outputs (" << dd_p2tr_count << ")");
    }

    // Should have 2 DD amounts in OP_RETURN: recipient + change
    BOOST_REQUIRE_EQUAL(ddAmounts.size(), 2);

    // Verify total DD is conserved (1000 DD)
    CAmount totalDD = 0;
    for (CAmount amount : ddAmounts) {
        totalDD += amount;
    }
    BOOST_CHECK_EQUAL(totalDD, 100000); // 1000 DD

    // Verify we have the recipient amount and change amount
    std::sort(ddAmounts.begin(), ddAmounts.end());
    BOOST_CHECK_EQUAL(ddAmounts[0], 40000); // 400 DD change
    BOOST_CHECK_EQUAL(ddAmounts[1], 60000); // 600 DD recipient
}

BOOST_FIXTURE_TEST_CASE(test_dgb_change_output, DDChangeTestFixture)
{
    // Arrange: Use large DGB UTXO for fees, expect DGB change
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 10000}}; // Send 100 DD ($100.00)
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    params.dgbChangeDest = CreateChangeDestination();
    params.ddUtxos.push_back(CreateMockDDUTXO(10000)); // Exact DD amount
    params.feeUtxos.push_back(CreateMockDGBUTXO(20000000)); // 0.2 DGB (enough for 0.1 min fee + change)

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert
    BOOST_CHECK_EQUAL(result.success, true);

    // Count DGB outputs (those with nValue > 0)
    std::vector<CAmount> dgbAmounts;
    for (const auto& output : result.tx.vout) {
        if (output.nValue > 0) {
            dgbAmounts.push_back(output.nValue);
        }
    }

    // Should have at least 1 DGB change output
    BOOST_CHECK_GE(dgbAmounts.size(), 1);

    // The DGB change should be significantly less than the input (most went to fees or dust)
    CAmount totalDGBOut = 0;
    for (CAmount amount : dgbAmounts) {
        totalDGBOut += amount;
    }

    // Change should be less than input minus minimum fee
    BOOST_CHECK_LT(totalDGBOut, 20000000); // Less than the 0.2 DGB input
    BOOST_CHECK_GT(totalDGBOut, 0); // But greater than zero (we should have change)

    // Should be approximately input - min_fee (0.2 - 0.1 = 0.1 DGB)
    BOOST_CHECK_GT(totalDGBOut, 9000000); // At least 0.09 DGB change
    BOOST_CHECK_LT(totalDGBOut, 11000000); // At most 0.11 DGB change
}

BOOST_FIXTURE_TEST_CASE(test_no_dd_change_exact_amount, DDChangeTestFixture)
{
    // Arrange: Exact DD amount match, no change needed
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 50000}}; // Send 500 DD ($500.00)
    params.feeRate = 100000; // 100,000 sat/kB
    params.spenderKey = senderKey;
    params.dgbChangeDest = CreateChangeDestination();
    params.ddUtxos.push_back(CreateMockDDUTXO(50000)); // Exact amount
    params.feeUtxos.push_back(CreateMockDGBUTXO(20000000));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert
    BOOST_REQUIRE_EQUAL(result.success, true);
    BOOST_CHECK_MESSAGE(result.error.empty(), "Unexpected error: " << result.error);

    // Find OP_RETURN and extract DD amounts
    std::vector<CAmount> ddAmounts;
    bool found_opreturn = false;
    for (const auto& output : result.tx.vout) {
        if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
            found_opreturn = ExtractDDAmountsFromOpReturn(output.scriptPubKey, ddAmounts);
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE(found_opreturn, "No OP_RETURN found in transaction");

    // Should have exactly 1 DD amount in OP_RETURN (recipient only, no change)
    BOOST_REQUIRE_EQUAL(ddAmounts.size(), 1);
    BOOST_CHECK_EQUAL(ddAmounts[0], 50000);
}

BOOST_FIXTURE_TEST_CASE(test_dd_change_below_dust, DDChangeTestFixture)
{
    // Arrange: DD change would be below the $1 minimum send threshold.
    // Unlike DGB dust, DD has real dollar value — sub-$1 change MUST be
    // preserved as a change output, not silently dropped. (Bug #27 fix)
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    const auto& ddParams = chainParams.GetDigiDollarParams();
    CAmount minOutput = DigiDollar::GetMinimumDDOutput(ddParams); // 100 cents = $1

    // Send 100 DD minus 99 cents → leaves 99 cents change (sub-$1)
    CAmount totalDD = 10000; // 100 DD ($100.00)
    CAmount sendAmount = totalDD - (minOutput - 1); // 10000 - 99 = 9901 cents ($99.01)
    CAmount expectedChange = minOutput - 1; // 99 cents ($0.99)

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, sendAmount}};
    params.feeRate = 100000;
    params.spenderKey = senderKey;
    params.dgbChangeDest = CreateChangeDestination();
    params.ddUtxos.push_back(CreateMockDDUTXO(totalDD));
    params.feeUtxos.push_back(CreateMockDGBUTXO(20000000));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Transaction succeeds
    BOOST_REQUIRE_EQUAL(result.success, true);
    BOOST_CHECK_MESSAGE(result.error.empty(), "Unexpected error: " << result.error);

    // Extract DD amounts from OP_RETURN
    std::vector<CAmount> ddAmounts;
    bool found_opreturn = false;
    for (const auto& output : result.tx.vout) {
        if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
            found_opreturn = ExtractDDAmountsFromOpReturn(output.scriptPubKey, ddAmounts);
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE(found_opreturn, "No OP_RETURN found in transaction");

    // Must have 2 DD amounts: recipient + sub-$1 change (NOT dropped as dust)
    BOOST_REQUIRE_EQUAL(ddAmounts.size(), 2);

    // Total DD must be perfectly conserved
    CAmount totalDDOut = 0;
    for (CAmount amount : ddAmounts) {
        totalDDOut += amount;
    }
    BOOST_CHECK_EQUAL(totalDDOut, totalDD);

    // Verify exact amounts
    std::sort(ddAmounts.begin(), ddAmounts.end());
    BOOST_CHECK_EQUAL(ddAmounts[0], expectedChange); // 99 cents change
    BOOST_CHECK_EQUAL(ddAmounts[1], sendAmount); // 9901 cents recipient
}

/**
 * Bug #27 regression test: Many small DD-UTXOs where multi-input aggregation
 * produces sub-$1 change. This is the exact scenario reported by shenger.
 *
 * Previously, the builder silently dropped sub-$1 DD change and excluded it
 * from the OP_RETURN. The validator then saw inputDD != outputDD and rejected
 * the transaction with "transfer-dd-conservation-violation, DD not conserved."
 */
BOOST_FIXTURE_TEST_CASE(test_bug27_multi_input_dd_conservation, DDChangeTestFixture)
{
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    // Simulate shenger's wallet: 20 small DD-UTXOs of $5 each = $100 total
    const int numUtxos = 20;
    const CAmount perUtxo = 500; // 500 cents = $5.00 each
    CAmount totalDD = numUtxos * perUtxo; // 10000 cents = $100.00

    TxBuilderTransferParams params;
    // Send $99.50 — leaves $0.50 change (below old $1 dust threshold)
    params.recipients = {{recipientAddr, 9950}}; // $99.50
    params.feeRate = 100000;
    params.spenderKey = senderKey;
    params.dgbChangeDest = CreateChangeDestination();

    for (int i = 0; i < numUtxos; i++) {
        params.ddUtxos.push_back(CreateMockDDUTXO(perUtxo));
    }
    params.feeUtxos.push_back(CreateMockDGBUTXO(20000000));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    // Act
    TxBuilderResult result = builder.BuildTransferTransaction(params);

    // Assert: Must succeed — this was the exact failure scenario
    BOOST_REQUIRE_EQUAL(result.success, true);
    BOOST_CHECK_MESSAGE(result.error.empty(), "Bug #27 regression: " << result.error);

    // Extract DD amounts from OP_RETURN
    std::vector<CAmount> ddAmounts;
    bool found_opreturn = false;
    for (const auto& output : result.tx.vout) {
        if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
            found_opreturn = ExtractDDAmountsFromOpReturn(output.scriptPubKey, ddAmounts);
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE(found_opreturn, "No OP_RETURN found in transaction");

    // Must have 2 amounts: $99.50 recipient + $0.50 change
    BOOST_REQUIRE_EQUAL(ddAmounts.size(), 2);

    // CRITICAL: Total DD must be perfectly conserved — not one cent lost
    CAmount totalDDOut = 0;
    for (CAmount amount : ddAmounts) {
        totalDDOut += amount;
    }
    BOOST_CHECK_EQUAL(totalDDOut, totalDD); // 10000 == 10000

    // Verify exact amounts
    std::sort(ddAmounts.begin(), ddAmounts.end());
    BOOST_CHECK_EQUAL(ddAmounts[0], 50); // 50 cents ($0.50) change — was silently dropped before
    BOOST_CHECK_EQUAL(ddAmounts[1], 9950); // 9950 cents ($99.50) recipient

    // Count P2TR DD outputs (nValue==0, starts with OP_1)
    int dd_outputs = 0;
    for (const auto& output : result.tx.vout) {
        if (output.nValue == 0 && output.scriptPubKey.size() > 1 && output.scriptPubKey[0] == 0x51) {
            dd_outputs++;
        }
    }
    // Must have 2 DD P2TR outputs: recipient + change (change was missing before)
    BOOST_CHECK_EQUAL(dd_outputs, 2);
}

/**
 * Verify that single-cent DD change is preserved.
 * Edge case: send $99.99 from $100.00 — $0.01 change must survive.
 */
BOOST_FIXTURE_TEST_CASE(test_single_cent_dd_change_preserved, DDChangeTestFixture)
{
    std::string recipientAddr = CreateDDAddress(recipientKey.GetPubKey());

    TxBuilderTransferParams params;
    params.recipients = {{recipientAddr, 9999}}; // $99.99
    params.feeRate = 100000;
    params.spenderKey = senderKey;
    params.dgbChangeDest = CreateChangeDestination();
    params.ddUtxos.push_back(CreateMockDDUTXO(10000)); // $100.00
    params.feeUtxos.push_back(CreateMockDGBUTXO(20000000));

    MockTransferTxBuilder builder(chainParams, currentHeight, oraclePrice);

    TxBuilderResult result = builder.BuildTransferTransaction(params);

    BOOST_REQUIRE_EQUAL(result.success, true);
    BOOST_CHECK_MESSAGE(result.error.empty(), "Unexpected error: " << result.error);

    // Extract DD amounts from OP_RETURN
    std::vector<CAmount> ddAmounts;
    bool found_opreturn = false;
    for (const auto& output : result.tx.vout) {
        if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
            found_opreturn = ExtractDDAmountsFromOpReturn(output.scriptPubKey, ddAmounts);
            break;
        }
    }
    BOOST_REQUIRE_MESSAGE(found_opreturn, "No OP_RETURN found in transaction");

    // Must have 2 amounts: $99.99 + $0.01 change
    BOOST_REQUIRE_EQUAL(ddAmounts.size(), 2);

    CAmount totalDDOut = 0;
    for (CAmount amount : ddAmounts) {
        totalDDOut += amount;
    }
    BOOST_CHECK_EQUAL(totalDDOut, 10000); // Perfect conservation

    std::sort(ddAmounts.begin(), ddAmounts.end());
    BOOST_CHECK_EQUAL(ddAmounts[0], 1); // 1 cent change — the absolute minimum
    BOOST_CHECK_EQUAL(ddAmounts[1], 9999); // $99.99 recipient
}

BOOST_AUTO_TEST_SUITE_END()
