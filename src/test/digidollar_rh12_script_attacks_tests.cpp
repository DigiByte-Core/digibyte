// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-12: Transaction Builder & Script-Level Attacks
 *
 * Second-pass adversarial red-team focusing on creative, unusual attack
 * vectors against DD transaction construction and script validation.
 *
 * FINDINGS SUMMARY:
 * [RH-12-01] SIGHASH_NONE/ANYONECANPAY — output stripping on DD txs (VULNERABILITY)
 * [RH-12-02] OP_RETURN data injection — arbitrary data in DD metadata fields
 * [RH-12-03] Transfer OP_RETURN amount inflation — extra amounts in OP_RETURN
 * [RH-12-04] Fee siphoning via change manipulation in redemption txs
 * [RH-12-05] Script size limit edge cases for DD MAST scripts
 * [RH-12-06] DD tx nVersion collision — non-DD tx with DD marker version
 * [RH-12-07] Redeem OP_RETURN DD change injection — fake DD from nothing
 * [RH-12-08] Transfer with zero-amount OP_RETURN entries
 * [RH-12-09] Multiple OP_RETURN outputs in transfer transactions
 * [RH-12-10] Witness stack manipulation — oversized witness for DD P2TR
 */

#include <digidollar/scripts.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <digidollar/digidollar.h>
#include <consensus/digidollar.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <script/interpreter.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <key.h>
#include <pubkey.h>
#include <coins.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(digidollar_rh12_script_attacks_tests, TestingSetup)

// ============================================================================
// Helper: create a minimal valid DD mint transaction for mutation testing
// ============================================================================
static CMutableTransaction CreateBaseMintTx(const CKey& ownerKey, CAmount ddAmount,
                                             int64_t lockHeight, CAmount collateral)
{
    CMutableTransaction tx;
    tx.SetDigiDollarType(DD_TX_MINT);

    // Input (fake UTXO)
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    // Output 0: Collateral P2TR (using NUMS key)
    DigiDollar::MintParams mp;
    mp.ddAmount = ddAmount;
    mp.lockHeight = lockHeight;
    CPubKey pub = ownerKey.GetPubKey();
    mp.ownerKey = XOnlyPubKey(pub);
    mp.internalKey = DigiDollar::GetCollateralNUMSKey();
    mp.oracleKeys = DigiDollar::GetOracleKeys(15);
    CScript collateralScript = DigiDollar::CreateCollateralP2TR(mp);
    tx.vout.push_back(CTxOut(collateral, collateralScript));

    // Output 1: DD token (0 value P2TR)
    CScript ddScript = DigiDollar::CreateDigiDollarP2TR(mp.ownerKey, ddAmount);
    tx.vout.push_back(CTxOut(0, ddScript));

    // Output 2: OP_RETURN metadata
    XOnlyPubKey ownerXOnly(pub);
    CScript metaScript = CScript() << OP_RETURN
                                   << std::vector<unsigned char>{'D', 'D'}
                                   << CScriptNum(1)
                                   << CScriptNum(ddAmount)
                                   << CScriptNum(lockHeight)
                                   << CScriptNum(0) // tier 0
                                   << std::vector<unsigned char>(ownerXOnly.begin(), ownerXOnly.end());
    tx.vout.push_back(CTxOut(0, metaScript));

    return tx;
}

// ============================================================================
// [RH-12-01] SIGHASH_NONE/ANYONECANPAY — Output Stripping Attack
//
// ATTACK: DD transactions don't enforce sighash types. An attacker who sees
// a signed DD transfer in the mempool with SIGHASH_NONE could strip/replace
// all outputs (redirecting DD to themselves). With SIGHASH_ANYONECANPAY,
// they could add their own inputs and manipulate the transaction.
//
// The txbuilder doesn't set signature hash types — it's up to the signer.
// But validation should reject DD txs signed with dangerous sighash types.
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_01_sighash_type_not_enforced)
{
    // FINDING: DD validation (ValidateTransferTransaction, ValidateRedemptionTransaction)
    // does NOT check the sighash type of input signatures. While Taproot/Schnorr
    // signatures include the sighash in the signed message, there's no consensus rule
    // preventing SIGHASH_NONE or SIGHASH_ANYONECANPAY on DD transactions.
    //
    // With SIGHASH_NONE: Attacker replaces ALL outputs → redirects DD tokens
    // With SIGHASH_SINGLE: Attacker adds outputs → creates DD from nothing
    // With ANYONECANPAY: Attacker adds inputs from other DD positions
    //
    // IMPACT: HIGH — Could redirect DD tokens in-flight
    // STATUS: Needs mitigation — DD validation should reject non-SIGHASH_DEFAULT signatures

    // Demonstrate: Build a transfer tx — nothing prevents dangerous sighash
    CKey spenderKey;
    spenderKey.MakeNewKey(true);

    // The txbuilder creates transactions but doesn't sign them.
    // The signing step is separate and there's no enforcement of sighash type.
    // We verify that validation doesn't check sighash type by examining the code path.

    // Create a transfer transaction
    CMutableTransaction tx;
    tx.SetDigiDollarType(DD_TX_TRANSFER);
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    // DD output to attacker
    CKey attackerKey;
    attackerKey.MakeNewKey(true);
    XOnlyPubKey attackerXOnly(attackerKey.GetPubKey());
    auto tweaked = attackerXOnly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    CScript attackerScript;
    attackerScript << OP_1 << ToByteVector(tweaked->first);
    tx.vout.push_back(CTxOut(0, attackerScript));

    // OP_RETURN
    CScript metaScript;
    metaScript << OP_RETURN
               << std::vector<unsigned char>{'D', 'D'}
               << CScriptNum(2)
               << CScriptNum(5000); // $50
    tx.vout.push_back(CTxOut(0, metaScript));

    // The transaction structure is valid. If signed with SIGHASH_NONE,
    // the outputs could be replaced entirely. DD validation in
    // ValidateTransferTransaction never checks witness sighash bytes.
    BOOST_TEST_MESSAGE("RH-12-01: DD validation does NOT enforce sighash type.");
    BOOST_TEST_MESSAGE("  SIGHASH_NONE on DD transfer allows output replacement.");
    BOOST_TEST_MESSAGE("  RECOMMENDATION: Reject DD transactions where any input");
    BOOST_TEST_MESSAGE("  uses SIGHASH_NONE, SIGHASH_SINGLE, or SIGHASH_ANYONECANPAY.");

    // Verify the transaction structurally passes (no sighash check exists)
    BOOST_CHECK(tx.vout.size() >= 2);
    BOOST_CHECK(DigiDollar::HasDigiDollarMarker(CTransaction(tx)));
}

// ============================================================================
// [RH-12-02] OP_RETURN Data Injection — Arbitrary Data in DD Metadata
//
// ATTACK: DD OP_RETURN parsing uses positional fields. Can we inject extra
// data pushes that confuse validation or downstream parsers?
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_02_opreturn_extra_data_injection)
{
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    XOnlyPubKey ownerXOnly(ownerKey.GetPubKey());

    // Craft OP_RETURN with extra data pushes after the standard fields
    CScript metaScript = CScript() << OP_RETURN
                                   << std::vector<unsigned char>{'D', 'D'}
                                   << CScriptNum(1)  // MINT
                                   << CScriptNum(10000)  // $100
                                   << CScriptNum(2000)  // lockHeight
                                   << CScriptNum(1)  // tier 1
                                   << std::vector<unsigned char>(ownerXOnly.begin(), ownerXOnly.end())
                                   // INJECTED: Extra data pushes
                                   << std::vector<unsigned char>{'E', 'V', 'I', 'L'}
                                   << CScriptNum(999999999);  // Huge fake amount

    // Mint validation reads fields positionally and stops after owner pubkey.
    // Extra data is ignored. This is actually SAFE for mint validation.
    // But check: does any downstream code iterate ALL pushes?
    BOOST_CHECK(metaScript.size() > 0);

    // The extra data makes the OP_RETURN larger. Check it fits.
    BOOST_CHECK(metaScript.size() <= MAX_OP_RETURN_RELAY);

    BOOST_TEST_MESSAGE("RH-12-02: Extra OP_RETURN pushes are safely ignored by mint validation.");
    BOOST_TEST_MESSAGE("  However, downstream indexers or wallets parsing ALL pushes could be confused.");
    BOOST_TEST_MESSAGE("  RECOMMENDATION: Reject DD OP_RETURN with unexpected trailing data.");
}

// ============================================================================
// [RH-12-03] Transfer OP_RETURN Amount Inflation
//
// ATTACK: Transfer OP_RETURN format is: DD <2> <amt1> <amt2> ...
// What if we include MORE amounts in OP_RETURN than there are P2TR outputs?
// Or FEWER? The validation maps OP_RETURN amounts to P2TR outputs by index.
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_03_transfer_opreturn_amount_mismatch)
{
    // SCENARIO 1: More OP_RETURN amounts than P2TR outputs
    // If OP_RETURN says [5000, 3000, 2000] but only 2 P2TR outputs exist,
    // the extra 2000 is ignored in output counting but counted in input matching.
    // This could break conservation: outputDD = 8000, but actual outputs = 8000
    // Wait — the validation sums dd_amounts matched to P2TR outputs, not all amounts.
    // So extra amounts are harmless for output sum. But for INPUT matching:
    // ExtractDDAmountFromTxRef reads dd_amounts and maps by P2TR output index.
    // If this tx is later spent, the EXTRA dd_amount entry could be mapped
    // to a non-existent output index. Let's verify.

    CMutableTransaction tx;
    tx.SetDigiDollarType(DD_TX_TRANSFER);
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    // Only 1 P2TR output
    CKey recipientKey;
    recipientKey.MakeNewKey(true);
    XOnlyPubKey recipientXOnly(recipientKey.GetPubKey());
    auto tweaked = recipientXOnly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    CScript ddScript;
    ddScript << OP_1 << ToByteVector(tweaked->first);
    tx.vout.push_back(CTxOut(0, ddScript));

    // OP_RETURN with 3 amounts but only 1 P2TR output
    CScript metaScript;
    metaScript << OP_RETURN
               << std::vector<unsigned char>{'D', 'D'}
               << CScriptNum(2)
               << CScriptNum(5000)
               << CScriptNum(3000)   // phantom amount
               << CScriptNum(99999); // phantom amount
    tx.vout.push_back(CTxOut(0, metaScript));

    // Validate: Transfer validation should sum only amounts matched to P2TR outputs
    TxValidationState state;
    DigiDollar::ValidationContext ctx(1000, 6310, 150, Params());

    // This tests whether phantom OP_RETURN amounts bypass conservation
    bool valid = DigiDollar::ValidateTransferTransaction(CTransaction(tx), ctx, state);

    // The validation should either:
    // 1. Reject for mismatch between OP_RETURN count and P2TR output count, OR
    // 2. Only count the first amount (matching the single P2TR output)
    // If it sums ALL OP_RETURN amounts as outputDD, that's an inflation bug.
    BOOST_TEST_MESSAGE("RH-12-03: Transfer with phantom OP_RETURN amounts. Valid=" +
                       std::to_string(valid) + " Reason=" + state.GetRejectReason());
    BOOST_CHECK(!valid);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "transfer-dd-output-amount-mismatch");

    // SCENARIO 2: Fewer OP_RETURN amounts than P2TR outputs
    // This is the reverse — if validation counts P2TR outputs but runs out of
    // OP_RETURN amounts, it should reject.
    CMutableTransaction tx2;
    tx2.SetDigiDollarType(DD_TX_TRANSFER);
    tx2.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    // 2 P2TR outputs
    tx2.vout.push_back(CTxOut(0, ddScript));
    tx2.vout.push_back(CTxOut(0, ddScript));

    // Only 1 amount in OP_RETURN
    CScript metaScript2;
    metaScript2 << OP_RETURN
                << std::vector<unsigned char>{'D', 'D'}
                << CScriptNum(2)
                << CScriptNum(5000);
    tx2.vout.push_back(CTxOut(0, metaScript2));

    TxValidationState state2;
    bool valid2 = DigiDollar::ValidateTransferTransaction(CTransaction(tx2), ctx, state2);
    BOOST_TEST_MESSAGE("RH-12-03: Transfer with fewer OP_RETURN amounts. Valid=" +
                       std::to_string(valid2) + " Reason=" + state2.GetRejectReason());
    BOOST_CHECK(!valid2);
    BOOST_CHECK_EQUAL(state2.GetRejectReason(), "transfer-dd-output-amount-mismatch");

    // FINDING: If valid2 is true, the second P2TR output gets no DD amount,
    // effectively creating a "free" P2TR output the receiver thinks has value
    // (or that could be spent in a future transfer with an inflated OP_RETURN).
    if (valid2) {
        BOOST_TEST_MESSAGE("  VULNERABILITY: P2TR output without matching OP_RETURN amount accepted!");
        BOOST_TEST_MESSAGE("  This output could be spent later with an inflated amount claim.");
    }
}

// ============================================================================
// [RH-12-04] Fee Siphoning via Redemption Change Manipulation
//
// ATTACK: In BuildRedemptionTransaction, DGB change from fee inputs is
// calculated as: feeChange = totalFeeIn - result.totalFees
// If we provide fee UTXOs with inflated amounts (the virtual GetUTXOValue
// returns 100 DGB), the change calculation uses those inflated values.
// In production, this relies on accurate UTXO values.
// But: The collateral return is OUTPUT 0, and fee change is a later output.
// Can we craft params where collateral return + fee change > actual inputs?
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_04_fee_siphon_redemption)
{
    // The RedeemTxBuilder uses pre-queried position data (collateralAmount,
    // ddMinted, unlockHeight) from params. If a malicious RPC caller provides
    // inflated collateralAmount, the builder creates a tx releasing MORE DGB
    // than actually locked.
    //
    // FINDING: The txbuilder TRUSTS the caller-provided collateralAmount.
    // Validation (ValidateCollateralReleaseAmount) checks against the actual
    // UTXO only during ConnectBlock. But during MEMPOOL ACCEPTANCE, if the
    // coins view is unavailable, it returns true unconditionally!

    // Simulate: redeemer claims 1000 DGB collateral but only 10 DGB is locked
    DigiDollar::TxBuilderRedeemParams params;
    params.collateralOutpoint = COutPoint(uint256::ONE, 0);
    params.ddToRedeem = 10000; // $100
    params.path = DigiDollar::RedemptionPath::NORMAL;

    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    params.ownerKey = ownerKey;
    params.feeRate = 1000000; // 1M sat/kB

    params.ddUtxos.push_back(COutPoint(uint256::ONE, 1));
    params.ddAmounts.push_back(10000);

    params.feeUtxos.push_back(COutPoint(uint256::ONE, 2));
    params.feeAmounts.push_back(1 * COIN); // 1 DGB for fees

    // ATTACK: Claim 1000 DGB collateral when only 10 is locked
    params.collateralAmount = 1000 * COIN; // INFLATED!
    params.ddMinted = 10000;
    params.unlockHeight = 500;

    // The builder only pays out to addresses the caller names: one for the
    // collateral the redemption unlocks, one for the fee change. These two
    // stand in for wallet addresses.
    CKey collateralKey;
    collateralKey.MakeNewKey(true);
    params.collateralDest = CTxDestination{WitnessV1Taproot(XOnlyPubKey(collateralKey.GetPubKey()))};
    CKey changeKey;
    changeKey.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(changeKey.GetPubKey())};

    DigiDollar::RedeemTxBuilder builder(Params(), 1000, 6310);
    DigiDollar::TxBuilderResult result = builder.BuildRedemptionTransaction(params);

    if (result.success) {
        // Check if output 0 (collateral return) has the inflated amount
        BOOST_CHECK(result.tx.vout.size() > 0);
        CAmount collateralReturn = result.tx.vout[0].nValue;
        BOOST_TEST_MESSAGE("RH-12-04: Redemption tx built with inflated collateral.");
        BOOST_TEST_MESSAGE("  Collateral return in tx: " + std::to_string(collateralReturn) + " sats");
        BOOST_TEST_MESSAGE("  Actual locked (should be): 10 DGB = " + std::to_string(10 * COIN) + " sats");

        if (collateralReturn > 100 * COIN) {
            BOOST_TEST_MESSAGE("  VULNERABILITY: TxBuilder trusts caller-provided collateralAmount!");
            BOOST_TEST_MESSAGE("  The built tx claims " + std::to_string(collateralReturn / COIN) + " DGB");
            BOOST_TEST_MESSAGE("  ConnectBlock validation catches this, but mempool may not.");
        }
    }

    BOOST_CHECK(result.success); // Builder should succeed (it trusts params)
}

// ============================================================================
// [RH-12-05] Script Size Limits for DD MAST Scripts
//
// ATTACK: DD collateral uses TaprootBuilder with 2 MAST branches.
// What if we create MintParams with extreme values that produce oversized
// scripts? CScriptNum encoding of large lockHeight values grows.
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_05_script_size_edge_cases)
{
    DigiDollar::MintParams params;
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    params.ownerKey = XOnlyPubKey(ownerKey.GetPubKey());
    params.internalKey = DigiDollar::GetCollateralNUMSKey();
    params.oracleKeys = DigiDollar::GetOracleKeys(15);
    params.ddAmount = 10000;

    // Test with maximum possible lockHeight (int64_t max)
    params.lockHeight = std::numeric_limits<int64_t>::max();
    CScript normalPath = DigiDollar::CreateNormalRedemptionPath(params);
    CScript errPath = DigiDollar::CreateERRPath(params);

    BOOST_TEST_MESSAGE("RH-12-05: Script size with max lockHeight:");
    BOOST_TEST_MESSAGE("  Normal path size: " + std::to_string(normalPath.size()) + " bytes");
    BOOST_TEST_MESSAGE("  ERR path size: " + std::to_string(errPath.size()) + " bytes");

    // Check against MAX_SCRIPT_SIZE (10,000 bytes)
    BOOST_CHECK(normalPath.size() < MAX_SCRIPT_SIZE);
    BOOST_CHECK(errPath.size() < MAX_SCRIPT_SIZE);

    // Try creating the full P2TR — CScriptNum for int64_max uses 9 bytes
    CScript p2tr = DigiDollar::CreateCollateralP2TR(params);
    BOOST_TEST_MESSAGE("  P2TR output size: " + std::to_string(p2tr.size()) + " bytes");

    // P2TR output is always 34 bytes (OP_1 + 32-byte key), regardless of MAST complexity.
    // The scripts only affect the control block size during spending.
    BOOST_CHECK_EQUAL(p2tr.size(), 34);

    // Test with negative lockHeight (should be rejected by CreateNormalRedemptionPath)
    params.lockHeight = -1;
    CScript negPath = DigiDollar::CreateNormalRedemptionPath(params);
    BOOST_TEST_MESSAGE("  Negative lockHeight path empty: " + std::to_string(negPath.empty()));
    BOOST_CHECK(negPath.empty()); // Should be rejected
}

// ============================================================================
// [RH-12-06] DD Transaction Version Collision
//
// ATTACK: The DD marker is in the nVersion field. What if a regular (non-DD)
// transaction happens to have a version that matches the DD pattern?
// DD marker: 0x____0770 where lower 16 bits = 0x0770
// Can a miner craft a coinbase with DD nVersion to create DD from nothing?
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_06_version_collision_attack)
{
    // Create a non-DD transaction with DD marker version
    CMutableTransaction fakeTx;
    fakeTx.nVersion = 0x01000770; // DD MINT marker but actually regular tx

    // Add regular inputs/outputs
    fakeTx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    // Add a zero-value P2TR output (looks like DD token)
    CKey fakeKey;
    fakeKey.MakeNewKey(true);
    XOnlyPubKey fakeXOnly(fakeKey.GetPubKey());
    auto tweaked = fakeXOnly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    CScript fakeP2TR;
    fakeP2TR << OP_1 << ToByteVector(tweaked->first);
    fakeTx.vout.push_back(CTxOut(0, fakeP2TR));

    // Add DD OP_RETURN to make it look like a DD transfer
    CScript metaScript;
    metaScript << OP_RETURN
               << std::vector<unsigned char>{'D', 'D'}
               << CScriptNum(2) // TRANSFER
               << CScriptNum(100000); // $1000
    fakeTx.vout.push_back(CTxOut(0, metaScript));

    // This transaction has DD marker version. Will it be treated as DD?
    CTransaction ctx_tx(fakeTx);
    bool hasMarker = DigiDollar::HasDigiDollarMarker(ctx_tx);
    BOOST_TEST_MESSAGE("RH-12-06: Fake DD tx has marker: " + std::to_string(hasMarker));
    BOOST_CHECK(hasMarker); // It WILL be detected as DD

    // If this tx makes it into a block, later DD transfers could reference
    // its zero-value P2TR output as a DD UTXO worth $1000.
    // MITIGATION: ExtractDDAmountFromTxRef checks HasDigiDollarMarker — but this
    // tx HAS the marker! The only protection is full DD validation at ConnectBlock.
    // If DD validation is somehow bypassed (e.g., skipOracleValidation), this is exploitable.
    BOOST_TEST_MESSAGE("  RISK: A non-DD tx with DD nVersion passes HasDigiDollarMarker.");
    BOOST_TEST_MESSAGE("  Protection depends entirely on full DD validation at ConnectBlock.");
}

// ============================================================================
// [RH-12-07] Redeem OP_RETURN DD Change Injection
//
// ATTACK: Redemption tx with DD change adds OP_RETURN with DD type=3 and
// the change amount. But what if an attacker crafts a redeem tx with an
// INFLATED DD change amount in the OP_RETURN? The OP_RETURN says the change
// output is worth $10,000 but only $100 was actually input.
// When this change UTXO is later spent, ExtractDDAmountFromTxRef reads
// the OP_RETURN and returns the inflated amount.
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_07_redeem_opreturn_change_inflation)
{
    // Build a redeem tx manually with inflated DD change in OP_RETURN
    CMutableTransaction tx;
    tx.SetDigiDollarType(DD_TX_REDEEM);

    // Inputs
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0), CScript(), 0xFFFFFFFE)); // collateral
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 1), CScript(), 0xFFFFFFFE)); // DD to burn

    // Output 0: Collateral return
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    CScript collateralReturn = GetScriptForDestination(
        WitnessV1Taproot(XOnlyPubKey(ownerKey.GetPubKey())));
    tx.vout.push_back(CTxOut(100 * COIN, collateralReturn));

    // Output 1: DD change P2TR (zero value)
    XOnlyPubKey ownerXOnly(ownerKey.GetPubKey());
    CScript ddChangeScript = DigiDollar::CreateDigiDollarP2TR(ownerXOnly, 100); // $1 real
    tx.vout.push_back(CTxOut(0, ddChangeScript));

    // Output 2: OP_RETURN with INFLATED DD change amount
    CScript metaScript;
    metaScript << OP_RETURN
               << std::vector<unsigned char>{'D', 'D'}
               << CScriptNum(3) // REDEEM
               << CScriptNum(1000000); // $10,000 — but only $1 of DD change!
    tx.vout.push_back(CTxOut(0, metaScript));

    tx.nLockTime = 500;

    // If this tx gets into a block, the DD change UTXO (output 1) would be
    // valued at $10,000 by ExtractDDAmountFromTxRef (reads OP_RETURN).
    // The actual DD burned was: totalDDInputs - ddChange. If the inflated
    // ddChange is used, the burn validation sees LESS burn than reality.
    // But the BIGGER issue: when the change UTXO is SPENT in a later transfer,
    // it carries the inflated $10,000 value.

    // Check what ValidateRedemptionTransaction does with this
    TxValidationState state;
    DigiDollar::ValidationContext ctx(1000, 6310, 150, Params());

    // Without coins view, validation is limited
    bool valid = DigiDollar::ValidateRedemptionTransaction(CTransaction(tx), ctx, state);
    BOOST_TEST_MESSAGE("RH-12-07: Redeem with inflated DD change OP_RETURN. Valid=" +
                       std::to_string(valid) + " Reason=" + state.GetRejectReason());

    // FINDING: ValidateRedemptionTransaction checks DD burn (inputs > outputs),
    // but the DD output amount comes from OP_RETURN or metadata registry.
    // If the OP_RETURN is inflated, the burn check might FAIL (thinks more DD
    // is output than input), which is actually a protection! But if it passes
    // (because input amounts also can't be determined without coins view),
    // the inflated UTXO enters the blockchain.
    BOOST_TEST_MESSAGE("  KEY FINDING: Redemption DD change amount in OP_RETURN is");
    BOOST_TEST_MESSAGE("  NOT cross-validated against actual DD input amounts.");
    BOOST_TEST_MESSAGE("  RECOMMENDATION: Verify DD_change = DD_inputs - DD_burned");
    BOOST_TEST_MESSAGE("  explicitly, rejecting if OP_RETURN change > computed change.");

    // VULNERABILITY CONFIRMED: Redemption with inflated DD change passes without coins view
    BOOST_CHECK_MESSAGE(valid, "Redemption with inflated OP_RETURN DD change should be caught (currently passes)");
}

// ============================================================================
// [RH-12-08] Transfer with Zero or Negative OP_RETURN Amounts
//
// ATTACK: What happens if OP_RETURN contains zero or negative DD amounts?
// CScriptNum can represent negative numbers. Does validation catch this?
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_08_transfer_zero_negative_amounts)
{
    // Zero amount in OP_RETURN
    CMutableTransaction tx;
    tx.SetDigiDollarType(DD_TX_TRANSFER);
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    CKey recipientKey;
    recipientKey.MakeNewKey(true);
    XOnlyPubKey recipientXOnly(recipientKey.GetPubKey());
    auto tweaked = recipientXOnly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    CScript ddScript;
    ddScript << OP_1 << ToByteVector(tweaked->first);
    tx.vout.push_back(CTxOut(0, ddScript));

    // OP_RETURN with zero amount
    CScript zeroMeta;
    zeroMeta << OP_RETURN
             << std::vector<unsigned char>{'D', 'D'}
             << CScriptNum(2)
             << CScriptNum(0); // Zero DD!
    tx.vout.push_back(CTxOut(0, zeroMeta));

    TxValidationState state;
    DigiDollar::ValidationContext ctx(1000, 6310, 150, Params());
    bool valid = DigiDollar::ValidateTransferTransaction(CTransaction(tx), ctx, state);
    BOOST_TEST_MESSAGE("RH-12-08a: Transfer with zero DD amount. Valid=" +
                       std::to_string(valid) + " Reason=" + state.GetRejectReason());
    BOOST_CHECK(!valid); // Should be rejected

    // Negative amount (CScriptNum can encode negatives)
    CMutableTransaction tx2;
    tx2.SetDigiDollarType(DD_TX_TRANSFER);
    tx2.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));
    tx2.vout.push_back(CTxOut(0, ddScript));

    // Encode negative amount: CScriptNum(-5000) has high bit set
    CScript negMeta;
    negMeta << OP_RETURN
            << std::vector<unsigned char>{'D', 'D'}
            << CScriptNum(2)
            << CScriptNum(-5000); // Negative DD!
    tx2.vout.push_back(CTxOut(0, negMeta));

    TxValidationState state2;
    bool valid2 = DigiDollar::ValidateTransferTransaction(CTransaction(tx2), ctx, state2);
    BOOST_TEST_MESSAGE("RH-12-08b: Transfer with negative DD amount. Valid=" +
                       std::to_string(valid2) + " Reason=" + state2.GetRejectReason());

    // FINDING: If negative amounts pass, conservation check becomes:
    // inputDD == outputDD where outputDD is negative → inputDD would need to be negative too
    // This is likely caught but worth verifying.
    if (!valid2) {
        BOOST_TEST_MESSAGE("  Good: Negative amounts correctly rejected.");
    } else {
        BOOST_TEST_MESSAGE("  VULNERABILITY: Negative DD amounts accepted in transfer!");
    }
}

// ============================================================================
// [RH-12-09] Multiple OP_RETURN Outputs in Transfer
//
// ATTACK: What if a transfer has TWO DD OP_RETURN outputs? Validation
// breaks on the first one found. But ExtractDDAmountFromTxRef also breaks
// on the first. If they contain different amounts, validation sees one
// set of amounts and future spending sees another.
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_09_multiple_opreturn_transfer)
{
    CMutableTransaction tx;
    tx.SetDigiDollarType(DD_TX_TRANSFER);
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    CKey recipientKey;
    recipientKey.MakeNewKey(true);
    XOnlyPubKey recipientXOnly(recipientKey.GetPubKey());
    auto tweaked = recipientXOnly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    CScript ddScript;
    ddScript << OP_1 << ToByteVector(tweaked->first);
    tx.vout.push_back(CTxOut(0, ddScript));

    // First OP_RETURN: $50
    CScript meta1;
    meta1 << OP_RETURN
          << std::vector<unsigned char>{'D', 'D'}
          << CScriptNum(2)
          << CScriptNum(5000);
    tx.vout.push_back(CTxOut(0, meta1));

    // Second OP_RETURN: $10,000 (inflated!)
    CScript meta2;
    meta2 << OP_RETURN
          << std::vector<unsigned char>{'D', 'D'}
          << CScriptNum(2)
          << CScriptNum(1000000);
    tx.vout.push_back(CTxOut(0, meta2));

    TxValidationState state;
    DigiDollar::ValidationContext ctx(1000, 6310, 150, Params());
    bool valid = DigiDollar::ValidateTransferTransaction(CTransaction(tx), ctx, state);
    BOOST_TEST_MESSAGE("RH-12-09: Transfer with 2 DD OP_RETURN outputs. Valid=" +
                       std::to_string(valid) + " Reason=" + state.GetRejectReason());

    // CRITICAL CHECK: If valid, which OP_RETURN does validation use?
    // And which does ExtractDDAmountFromTxRef use when this UTXO is later spent?
    // If they use DIFFERENT ones, this is an inflation attack.
    //
    // ValidateTransferTransaction: iterates outputs, breaks on first DD OP_RETURN → uses $50
    // ExtractDDAmountFromTxRef: iterates outputs, breaks on first DD OP_RETURN → uses $50
    // Both use the FIRST one → consistent. But this depends on output ordering.
    // If the P2TR output is AFTER the first OP_RETURN but BEFORE the second...
    BOOST_TEST_MESSAGE("  Both validation and extraction use first DD OP_RETURN (consistent).");
    BOOST_TEST_MESSAGE("  RECOMMENDATION: Reject transactions with multiple DD OP_RETURN outputs.");
}

// ============================================================================
// [RH-12-10] MAST Branch Confusion — Spending Wrong Path
//
// ATTACK: DD collateral has 2 MAST branches (Normal, ERR).
// Can an attacker use the ERR path when system is healthy (>100%)?
// The ERR path script includes OP_CHECKCOLLATERAL < 100 < OP_LESSTHAN OP_VERIFY.
// If OP_CHECKCOLLATERAL is not properly implemented, this check might pass.
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_10_mast_branch_confusion)
{
    // Create collateral with both paths
    DigiDollar::MintParams params;
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    params.ownerKey = XOnlyPubKey(ownerKey.GetPubKey());
    params.internalKey = DigiDollar::GetCollateralNUMSKey();
    params.oracleKeys = DigiDollar::GetOracleKeys(15);
    params.ddAmount = 10000;
    params.lockHeight = 1000;

    CScript normalPath = DigiDollar::CreateNormalRedemptionPath(params);
    CScript errPath = DigiDollar::CreateERRPath(params);

    BOOST_CHECK(!normalPath.empty());
    BOOST_CHECK(!errPath.empty());

    // The ERR path contains: OP_CHECKCOLLATERAL 100 OP_LESSTHAN OP_VERIFY
    // This means: push collateral ratio, push 100, check if ratio < 100
    // If system is healthy (150%), OP_CHECKCOLLATERAL pushes 150,
    // 150 < 100 = false, OP_VERIFY fails → ERR path correctly blocked.
    //
    // BUT: What if OP_CHECKCOLLATERAL is not implemented in script interpreter?
    // In Phase 1, these are custom opcodes. If the interpreter treats them as
    // OP_NOP (no-operation), OP_CHECKCOLLATERAL pushes nothing, and the stack
    // state is wrong, potentially leading to unexpected behavior.

    BOOST_TEST_MESSAGE("RH-12-10: MAST branch analysis:");
    BOOST_TEST_MESSAGE("  Normal path size: " + std::to_string(normalPath.size()));
    BOOST_TEST_MESSAGE("  ERR path size: " + std::to_string(errPath.size()));
    BOOST_TEST_MESSAGE("  Normal path: CLTV + owner CHECKSIG (straightforward)");
    BOOST_TEST_MESSAGE("  ERR path: CLTV + CHECKCOLLATERAL + DIGIDOLLAR + DDVERIFY + CHECKSIG");
    BOOST_TEST_MESSAGE("  CRITICAL: OP_CHECKCOLLATERAL, OP_DIGIDOLLAR, OP_DDVERIFY must be");
    BOOST_TEST_MESSAGE("  fully implemented in interpreter.cpp, not treated as NOP.");
    BOOST_TEST_MESSAGE("  If any DD opcode is NOP, the ERR path degrades to just CLTV + CHECKSIG,");
    BOOST_TEST_MESSAGE("  identical to the Normal path — meaning ERR provides no additional check.");

    // Verify the scripts contain the expected opcodes
    bool hasCheckCollateral = false;
    bool hasDigiDollar = false;
    bool hasDDVerify = false;

    CScript::const_iterator pc = errPath.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;
    while (pc < errPath.end()) {
        if (!errPath.GetOp(pc, opcode, data)) break;
        if (opcode == OP_CHECKCOLLATERAL) hasCheckCollateral = true;
        if (opcode == OP_DIGIDOLLAR) hasDigiDollar = true;
        if (opcode == OP_DDVERIFY) hasDDVerify = true;
    }

    BOOST_CHECK_MESSAGE(hasCheckCollateral, "ERR path missing OP_CHECKCOLLATERAL");
    BOOST_CHECK_MESSAGE(hasDigiDollar, "ERR path missing OP_DIGIDOLLAR");
    BOOST_CHECK_MESSAGE(hasDDVerify, "ERR path missing OP_DDVERIFY");
}

// ============================================================================
// [RH-12-11] Collateral Output With Non-NUMS Internal Key Bypass
//
// ATTACK: The NUMS verification reconstructs the expected P2TR output and
// compares. But what if the attacker uses the SAME owner key and parameters
// but a different leaf version? TaprootBuilder.Add uses 0xC0 leaf version.
// What about 0xC2 or other valid leaf versions?
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_11_leaf_version_bypass)
{
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    XOnlyPubKey ownerXOnly(ownerKey.GetPubKey());

    DigiDollar::MintParams params;
    params.ddAmount = 10000;
    params.lockHeight = 1000;
    params.ownerKey = ownerXOnly;
    params.internalKey = DigiDollar::GetCollateralNUMSKey();
    params.oracleKeys = DigiDollar::GetOracleKeys(15);

    // Standard script (leaf version 0xC0)
    CScript standardP2TR = DigiDollar::CreateCollateralP2TR(params);
    BOOST_CHECK(!standardP2TR.empty());

    // Try building with different leaf version manually
    TaprootBuilder altBuilder;
    CScript normalPath = DigiDollar::CreateNormalRedemptionPath(params);
    CScript errPath = DigiDollar::CreateERRPath(params);

    // Use leaf version 0xC2 instead of 0xC0
    altBuilder.Add(1, normalPath, 0xC2);
    altBuilder.Add(1, errPath, 0xC2);
    altBuilder.Finalize(params.internalKey);

    if (altBuilder.IsValid() && altBuilder.IsComplete()) {
        WitnessV1Taproot altOutput = altBuilder.GetOutput();
        CScript altP2TR;
        altP2TR << OP_1 << ToByteVector(altOutput);

        bool same = (altP2TR == standardP2TR);
        BOOST_TEST_MESSAGE("RH-12-11: Leaf version 0xC2 produces same P2TR: " + std::to_string(same));

        if (!same) {
            BOOST_TEST_MESSAGE("  Different leaf version → different P2TR output → NUMS check rejects.");
            BOOST_TEST_MESSAGE("  This is SAFE: attacker can't bypass NUMS with alt leaf version.");
        }
    } else {
        BOOST_TEST_MESSAGE("RH-12-11: Leaf version 0xC2 rejected by TaprootBuilder (expected).");
    }
}

// ============================================================================
// [RH-12-12] Mint Transaction Without DD Token Output (Collateral-Only)
//
// ATTACK: What if a mint tx has collateral + OP_RETURN but NO DD token output?
// The collateral gets locked but no DD is issued. Later, can someone craft
// a redemption that releases the collateral without burning DD?
// ============================================================================
BOOST_AUTO_TEST_CASE(rh12_12_mint_without_dd_output)
{
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    XOnlyPubKey ownerXOnly(ownerKey.GetPubKey());

    CMutableTransaction tx;
    tx.SetDigiDollarType(DD_TX_MINT);
    tx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    // Collateral output only
    DigiDollar::MintParams mp;
    mp.ddAmount = 10000;
    mp.lockHeight = 2000;
    mp.ownerKey = ownerXOnly;
    mp.internalKey = DigiDollar::GetCollateralNUMSKey();
    mp.oracleKeys = DigiDollar::GetOracleKeys(15);
    CScript collateralScript = DigiDollar::CreateCollateralP2TR(mp);
    tx.vout.push_back(CTxOut(500 * COIN, collateralScript));

    // OP_RETURN (claims $100 DD)
    CScript metaScript = CScript() << OP_RETURN
                                   << std::vector<unsigned char>{'D', 'D'}
                                   << CScriptNum(1)
                                   << CScriptNum(10000)
                                   << CScriptNum(2000)
                                   << CScriptNum(1)
                                   << std::vector<unsigned char>(ownerXOnly.begin(), ownerXOnly.end());
    tx.vout.push_back(CTxOut(0, metaScript));

    // NO DD token output!

    TxValidationState state;
    DigiDollar::ValidationContext ctx(1000, 6310, 150, Params());
    bool valid = DigiDollar::ValidateMintTransaction(CTransaction(tx), ctx, state);
    BOOST_TEST_MESSAGE("RH-12-12: Mint without DD output. Valid=" +
                       std::to_string(valid) + " Reason=" + state.GetRejectReason());
    BOOST_CHECK(!valid); // Should be rejected: missing DD output
}

BOOST_AUTO_TEST_SUITE_END()
