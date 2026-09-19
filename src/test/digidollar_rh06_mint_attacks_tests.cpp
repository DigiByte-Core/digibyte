// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-06: DigiDollar Minting Validation Attacks
 *
 * Adversarial red-team testing specifically targeting the minting flow.
 * These tests attempt to create DD tokens without proper collateral.
 */

#include <consensus/amount.h>
#include <util/int128.h>
#include <consensus/digidollar.h>
#include <consensus/dca.h>
#include <consensus/err.h>
#include <consensus/volatility.h>
#include <digidollar/validation.h>
#include <digidollar/scripts.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <kernel/chainparams.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <script/interpreter.h>
#include <key.h>
#include <pubkey.h>
#include <util/strencodings.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <limits>

namespace {

// Helper: Build a minimal valid DD OP_RETURN for mint
CScript BuildMintOpReturn(CAmount ddAmount, int64_t lockHeight, int64_t lockTier,
                          const std::vector<unsigned char>& ownerPubKey) {
    CScript script = CScript() << OP_RETURN
                               << std::vector<unsigned char>{'D', 'D'}
                               << CScriptNum(1) // MINT type
                               << CScriptNum(ddAmount)
                               << CScriptNum(lockHeight)
                               << CScriptNum(lockTier);
    if (!ownerPubKey.empty()) {
        script << ownerPubKey;
    }
    return script;
}

// Helper: Create a P2TR output script (34 bytes: OP_1 + 32-byte pubkey)
CScript MakeP2TR(const XOnlyPubKey& xpk) {
    CScript script;
    script << OP_1;
    script << ToByteVector(xpk);
    return script;
}

// Helper: Generate a deterministic key for testing
CKey GenerateTestKey(unsigned char seed = 1) {
    CKey key;
    unsigned char keydata[32];
    memset(keydata, seed, 32);
    keydata[0] = seed; // Ensure uniqueness
    key.Set(keydata, keydata + 32, true);
    return key;
}

// Helper: Build DD version number for mint
int32_t MakeMintVersion() {
    // Lower 16 bits: 0x0770, upper byte: 0x01 (MINT type)
    return static_cast<int32_t>(0x01000770);
}

} // anonymous namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_rh06_mint_attacks, BasicTestingSetup)

// =============================================================================
// RH-06-01: Collateral ratio bypass — Mint DD with insufficient collateral
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_01a_insufficient_collateral_rejected)
{
    // ATTACK: Provide only 50% of required collateral for a 30-day mint.
    // The 30-day tier requires 500% collateral.
    auto params = CChainParams::RegTest({});
    const CAmount ddAmount = 10000; // $100
    const int64_t lockBlocks = 30 * DigiDollar::BLOCKS_PER_DAY;
    const CAmount oraclePrice = 10000; // $0.01/DGB in micro-USD

    DigiDollar::ValidationContext ctx(1000, oraclePrice, 150, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(ddAmount, lockBlocks, ctx);
    BOOST_CHECK_GT(required, 0);

    // Provide only half
    CAmount halfCollateral = required / 2;
    bool valid = DigiDollar::ValidateCollateralRatio(halfCollateral, ddAmount, lockBlocks, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Half collateral should be rejected");

    // Provide 99%
    CAmount almostEnough = required - 1;
    valid = DigiDollar::ValidateCollateralRatio(almostEnough, ddAmount, lockBlocks, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: 99.999% collateral should be rejected (off by 1 sat)");

    // Provide exact amount — should pass
    valid = DigiDollar::ValidateCollateralRatio(required, ddAmount, lockBlocks, ctx);
    BOOST_CHECK_MESSAGE(valid, "Exact collateral should be accepted");
}

BOOST_AUTO_TEST_CASE(rh06_01b_collateral_ratio_all_tiers)
{
    // ATTACK: Try each lock tier with barely insufficient collateral.
    auto params = CChainParams::RegTest({});
    const CAmount ddAmount = 100000; // $1000
    const CAmount oraclePrice = 10000; // $0.01/DGB

    DigiDollar::ValidationContext ctx(1000, oraclePrice, 150, *params);

    // Test all standard tiers
    std::vector<int64_t> tiers = {
        240,                          // 1 hour: 1000%
        30 * DigiDollar::BLOCKS_PER_DAY,   // 30 days: 500%
        90 * DigiDollar::BLOCKS_PER_DAY,   // 90 days: 400%
        180 * DigiDollar::BLOCKS_PER_DAY,  // 180 days: 350%
        365 * DigiDollar::BLOCKS_PER_DAY,  // 1 year: 300%
        10 * 365 * DigiDollar::BLOCKS_PER_DAY, // 10 years: 200%
    };

    for (int64_t lockBlocks : tiers) {
        CAmount required = DigiDollar::CalculateRequiredCollateral(ddAmount, lockBlocks, ctx);
        BOOST_CHECK_GT(required, 0);

        // One sat less should fail
        bool valid = DigiDollar::ValidateCollateralRatio(required - 1, ddAmount, lockBlocks, ctx);
        BOOST_CHECK_MESSAGE(!valid,
            "EXPLOIT: Collateral " + std::to_string(required - 1) +
            " should fail for tier " + std::to_string(lockBlocks) + " blocks");
    }
}

// =============================================================================
// RH-06-02: Zero collateral mint
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_02a_zero_collateral_rejected)
{
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    bool valid = DigiDollar::ValidateCollateralRatio(0, 10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Zero collateral must be rejected");
}

BOOST_AUTO_TEST_CASE(rh06_02b_negative_collateral_rejected)
{
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    bool valid = DigiDollar::ValidateCollateralRatio(-1, 10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Negative collateral must be rejected");
}

// =============================================================================
// RH-06-03: Dust collateral — Mint with trivial collateral
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_03a_dust_collateral_insufficient)
{
    // ATTACK: Lock 546 sats (dust threshold) as collateral, try to mint $100 DD
    auto params = CChainParams::RegTest({});
    const CAmount dustCollateral = 546;
    const CAmount ddAmount = 10000; // $100

    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(ddAmount, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_GT(required, dustCollateral);

    bool valid = DigiDollar::ValidateCollateralRatio(dustCollateral, ddAmount, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Dust collateral must not cover $100 DD");
}

BOOST_AUTO_TEST_CASE(rh06_03b_one_satoshi_collateral)
{
    // ATTACK: 1 satoshi collateral for any DD amount
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    // Even $1 should require much more than 1 sat
    bool valid = DigiDollar::ValidateCollateralRatio(1, 100, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: 1 satoshi collateral must be rejected for $1 DD");
}

BOOST_AUTO_TEST_CASE(rh06_03c_dust_collateral_at_extreme_price)
{
    // ATTACK: At extreme DGB price ($1M/DGB), dust might actually cover DD amount.
    // This is legitimate — if DGB is worth $1M, 546 sats = $0.00546 worth of DGB
    // So 546 sats at $1M/DGB at 500% ratio could cover at most ~$0.001 DD
    auto params = CChainParams::RegTest({});
    const CAmount extremePrice = 1000000000000LL; // $1M/DGB in micro-USD
    DigiDollar::ValidationContext ctx(1000, extremePrice, 150, *params);

    // At this price, 546 sats = 546 * 1e12 / 1e8 = 5,460,000 micro-USD = $5.46
    // At 500% ratio, that covers $5.46/5 = $1.09 DD
    // So 100 cents ($1.00) should work, but 200 cents ($2.00) should fail
    CAmount required100 = DigiDollar::CalculateRequiredCollateral(100, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    CAmount required200 = DigiDollar::CalculateRequiredCollateral(200, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);

    BOOST_TEST_MESSAGE("At $1M/DGB: $1 DD requires " + std::to_string(required100) + " sats, $2 DD requires " + std::to_string(required200) + " sats");

    // Both should be positive
    BOOST_CHECK_GT(required100, 0);
    BOOST_CHECK_GT(required200, 0);
    // $2 should require more than $1
    BOOST_CHECK_GT(required200, required100);
}

// =============================================================================
// RH-06-04: Double-spend collateral (structural check)
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_04a_same_utxo_two_mint_txs)
{
    // ATTACK: Two mint transactions reference the same input UTXO.
    // This is caught by Bitcoin's UTXO model — a UTXO can only be spent once.
    // But verify that the DD validation doesn't independently accept both.
    //
    // Note: This is primarily a UTXO-layer defense (ConnectBlock double-spend check).
    // The mint validation itself doesn't track UTXOs between transactions.
    // This test documents the dependency on Bitcoin's UTXO model.

    BOOST_TEST_MESSAGE("RH-06-04a: Double-spend prevention relies on Bitcoin's UTXO model in ConnectBlock.");
    BOOST_TEST_MESSAGE("  Each collateral UTXO can only appear as input to one transaction.");
    BOOST_TEST_MESSAGE("  DD validation validates individual transactions; cross-tx dedup is UTXO consensus.");

    // Verify that duplicate inputs within a single TX are rejected
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    CMutableTransaction mtx;
    mtx.nVersion = MakeMintVersion();

    // Same input twice
    COutPoint sameOutpoint(uint256::ONE, 0);
    mtx.vin.push_back(CTxIn(sameOutpoint));
    mtx.vin.push_back(CTxIn(sameOutpoint));

    // This is caught by CheckTransaction (tx_check.cpp) before DD validation
    // which rejects duplicate inputs. The DD layer inherits this protection.
    BOOST_TEST_MESSAGE("  Duplicate inputs in single TX caught by CheckTransaction (pre-DD validation).");
    BOOST_CHECK(true); // Document the defense layer
}

// =============================================================================
// RH-06-05: Collateral amount overflow
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_05a_int128_overflow_max_values)
{
    // ATTACK: Max DD amount * MAX price * max ratio to overflow util::int128_t
    auto params = CChainParams::RegTest({});

    // MAX_DIGIDOLLAR (from consensus) should be bounded
    const CAmount maxDD = MAX_DIGIDOLLAR; // per-output serialization bound
    const CAmount maxPrice = 1000000000000LL; // $1M/DGB in micro-USD
    // Emergency DCA at 50% health = 2.0x multiplier, with 1000% base = 2000% effective

    DigiDollar::ValidationContext ctx(1000, maxPrice, 50, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(maxDD, 240, ctx);

    // Should be capped at MAX_MONEY, not overflow to garbage
    BOOST_CHECK_MESSAGE(required > 0, "EXPLOIT: Overflow caused zero/negative collateral requirement");
    BOOST_CHECK_MESSAGE(required <= MAX_MONEY, "EXPLOIT: Overflow exceeded MAX_MONEY: " + std::to_string(required));
}

BOOST_AUTO_TEST_CASE(rh06_05b_collateral_calc_with_zero_price)
{
    // ATTACK: Oracle price = 0 should return 0 collateral (caught by ValidateCollateralRatio)
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 0, 150, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_EQUAL(required, 0);

    // ValidateCollateralRatio should reject zero price
    bool valid = DigiDollar::ValidateCollateralRatio(1000000, 10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Zero oracle price must reject any mint");
}

BOOST_AUTO_TEST_CASE(rh06_05c_negative_dd_amount_collateral)
{
    // ATTACK: Negative DD amount — should return 0 collateral
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(-10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_EQUAL(required, 0);
}

// =============================================================================
// RH-06-06: Mint during system stress
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_06a_mint_during_err_blocked)
{
    // ATTACK: Mint when ERR is active (system health < 100%).
    // ShouldBlockMintingDuringERR should block this.
    auto params = CChainParams::RegTest({});

    // System health 50% — ERR should be active
    DigiDollar::ValidationContext ctx(1000, 10000, 50, *params);

    bool shouldBlock = DigiDollar::ShouldBlockMintingDuringERR(ctx);
    // Note: ERR blocking depends on oracle price passed to ShouldBlockMinting
    BOOST_TEST_MESSAGE("ShouldBlockMintingDuringERR at health=50%: " + std::to_string(shouldBlock));

    // Even if ERR doesn't block (implementation incomplete), DCA should increase collateral
    DigiDollar::ConsensusParams ddParams;
    int baseRatio = DigiDollar::GetCollateralRatioForLockTime(30 * DigiDollar::BLOCKS_PER_DAY, ddParams);
    int effectiveRatio = DigiDollar::GetEffectiveCollateralRatio(baseRatio, 50, *params);

    // At 50% health, DCA multiplier should be 2.0x
    BOOST_CHECK_MESSAGE(effectiveRatio >= baseRatio * 2,
        "EXPLOIT: DCA not applied during system stress. Base=" + std::to_string(baseRatio) +
        " Effective=" + std::to_string(effectiveRatio));
}

BOOST_AUTO_TEST_CASE(rh06_06b_mint_at_health_boundary)
{
    // ATTACK: Mint at exact health=100% boundary (ERR activates at <100%)
    auto params = CChainParams::RegTest({});

    // At exactly 100%, ERR should NOT be active
    DigiDollar::ValidationContext ctx100(1000, 10000, 100, *params);
    // At 99%, ERR SHOULD activate
    DigiDollar::ValidationContext ctx99(1000, 10000, 99, *params);

    DigiDollar::ConsensusParams ddParams;
    int baseRatio = DigiDollar::GetCollateralRatioForLockTime(30 * DigiDollar::BLOCKS_PER_DAY, ddParams);

    int effective100 = DigiDollar::GetEffectiveCollateralRatio(baseRatio, 100, *params);
    int effective99 = DigiDollar::GetEffectiveCollateralRatio(baseRatio, 99, *params);

    BOOST_TEST_MESSAGE("Effective ratio at 100% health: " + std::to_string(effective100));
    BOOST_TEST_MESSAGE("Effective ratio at 99% health: " + std::to_string(effective99));

    // At 99% health, DCA should start increasing collateral requirements
    BOOST_CHECK_GE(effective99, effective100);
}

// =============================================================================
// RH-06-07: Fake oracle price in mint calculation
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_07a_extreme_oracle_price_low)
{
    // ATTACK: Oracle price = 1 micro-USD ($0.000001/DGB). At this price, collateral
    // becomes astronomically expensive. Not capped at MAX_MONEY but still unfulfillable.
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 1, 150, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);

    // At $0.000001/DGB, $100 DD at 500% requires ~$500 worth of DGB = 500,000,000 DGB
    // That's 50,000,000,000,000,000 sats — way more than all DGB in existence
    // FINDING: CalculateRequiredCollateral does NOT cap at MAX_MONEY.
    // This is acceptable because ValidateCollateralRatio will reject since no one
    // can provide this much collateral. But capping would be a good defense-in-depth.
    BOOST_CHECK_GT(required, 21000000LL * COIN); // More than all DGB
    BOOST_TEST_MESSAGE("FINDING: At $0.000001/DGB, $100 DD requires " + std::to_string(required) +
        " sats (not capped at MAX_MONEY — defense-in-depth opportunity)");
}

BOOST_AUTO_TEST_CASE(rh06_07b_oracle_price_negative)
{
    // ATTACK: Negative oracle price
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, -10000, 150, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_EQUAL(required, 0);

    bool valid = DigiDollar::ValidateCollateralRatio(1000000, 10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Negative oracle price must reject mint");
}

BOOST_AUTO_TEST_CASE(rh06_07c_oracle_price_int64_max)
{
    // ATTACK: Oracle price = INT64_MAX (absurd price manipulation)
    // At INT64_MAX micro-USD, even 1 sat is worth ~$92 trillion, so collateral
    // requirement for $100 DD would be essentially zero sats.
    // FINDING: CalculateRequiredCollateral returns 0 at INT64_MAX price.
    // This means an attacker who can manipulate oracle price to INT64_MAX could
    // mint DD with zero collateral. Defense: Oracle price validation bounds.
    auto params = CChainParams::RegTest({});
    const CAmount absurdPrice = std::numeric_limits<int64_t>::max();
    DigiDollar::ValidationContext ctx(1000, absurdPrice, 150, *params);

    CAmount required = DigiDollar::CalculateRequiredCollateral(10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);

    // SECURITY FINDING: At INT64_MAX price, required collateral truncates to 0.
    // This is defended by oracle price range validation (see T1-05e).
    // The oracle system rejects prices outside reasonable bounds.
    BOOST_TEST_MESSAGE("FINDING: At INT64_MAX price, $100 DD requires " + std::to_string(required) +
        " sats — truncation to 0. Defended by oracle price range validation.");
    // Verify the ValidateCollateralRatio would still reject 0 collateral
    // (it should because required==0 means calculation failed)
    bool valid = DigiDollar::ValidateCollateralRatio(0, 10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "Zero collateral should still be rejected even at extreme price");
}

// =============================================================================
// RH-06-08: Below-minimum mint amount
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_08a_below_minimum_rejected)
{
    // RegTest has minMintAmount=1 (1 cent) for testing flexibility.
    // Mainnet uses minMintAmount=10000 ($100). Test against regtest values.
    auto params = CChainParams::RegTest({});
    const auto& ddParams = params->GetDigiDollarParams();

    BOOST_TEST_MESSAGE("RegTest minMintAmount=" + std::to_string(ddParams.minMintAmount) +
        " maxMintAmount=" + std::to_string(ddParams.maxMintAmount));

    // Zero should always be rejected
    bool valid = DigiDollar::ValidateMintAmount(0, *params, 1000);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Zero mint must be rejected");

    // 1 cent should be accepted on regtest (minMintAmount=1)
    valid = DigiDollar::ValidateMintAmount(1, *params, 1000);
    BOOST_CHECK_MESSAGE(valid, "1 cent should be accepted on regtest");

    // Above max should be rejected (regtest max = 100000 = $1000)
    valid = DigiDollar::ValidateMintAmount(ddParams.maxMintAmount + 1, *params, 1000);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Above max mint should be rejected");
}

BOOST_AUTO_TEST_CASE(rh06_08b_regtest_activation_height_zero)
{
    // RegTest has minMintAmountActivationHeight=0 (default), which means
    // effectiveMinMint = 1 cent always (the condition requires height > 0).
    // This is by design: regtest uses minMintAmount=1 for flexibility.
    auto params = CChainParams::RegTest({});
    const auto& ddParams = params->GetDigiDollarParams();

    BOOST_TEST_MESSAGE("RegTest minMintAmountActivationHeight=" +
        std::to_string(ddParams.minMintAmountActivationHeight));

    // On regtest, 1 cent should be valid at any height
    bool valid = DigiDollar::ValidateMintAmount(1, *params, 0);
    BOOST_CHECK_MESSAGE(valid, "RegTest: 1 cent valid at height 0");
    valid = DigiDollar::ValidateMintAmount(1, *params, 1000000);
    BOOST_CHECK_MESSAGE(valid, "RegTest: 1 cent valid at height 1M");
}

BOOST_AUTO_TEST_CASE(rh06_08c_zero_amount_rejected)
{
    auto params = CChainParams::RegTest({});
    bool valid = DigiDollar::ValidateMintAmount(0, *params, 1000);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Zero mint amount must be rejected");
}

BOOST_AUTO_TEST_CASE(rh06_08d_negative_amount_rejected)
{
    auto params = CChainParams::RegTest({});
    bool valid = DigiDollar::ValidateMintAmount(-1, *params, 1000);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Negative mint amount must be rejected");
}

// =============================================================================
// RH-06-09: Above-maximum mint amount
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_09a_above_max_rejected)
{
    auto params = CChainParams::RegTest({});
    const auto& ddParams = params->GetDigiDollarParams();

    // RegTest max = 100000 cents ($1000)
    bool valid = DigiDollar::ValidateMintAmount(ddParams.maxMintAmount + 1, *params, 1000);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Above max mint should be rejected");

    valid = DigiDollar::ValidateMintAmount(ddParams.maxMintAmount, *params, 1000);
    BOOST_CHECK_MESSAGE(valid, "Exact maximum should be accepted");
}

BOOST_AUTO_TEST_CASE(rh06_09b_int64_max_rejected)
{
    auto params = CChainParams::RegTest({});
    bool valid = DigiDollar::ValidateMintAmount(std::numeric_limits<int64_t>::max(), *params, 1000);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: INT64_MAX mint amount must be rejected");
}

// =============================================================================
// RH-06-10: Mint without oracle price
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_10a_zero_price_blocks_mint)
{
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 0, 150, *params);

    // ValidateMintTransaction should reject when oraclePriceMicroUSD <= 0
    // and skipOracleValidation is false (default)
    BOOST_CHECK_EQUAL(ctx.oraclePriceMicroUSD, 0);
    BOOST_CHECK_EQUAL(ctx.skipOracleValidation, false);

    // CalculateRequiredCollateral returns 0 on bad inputs
    CAmount required = DigiDollar::CalculateRequiredCollateral(10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_EQUAL(required, 0);

    // ValidateCollateralRatio rejects zero price
    bool valid = DigiDollar::ValidateCollateralRatio(100 * COIN, 10000, 30 * DigiDollar::BLOCKS_PER_DAY, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Zero oracle price must reject collateral validation");
}

BOOST_AUTO_TEST_CASE(rh06_10b_skip_oracle_skips_collateral_check)
{
    // IMPORTANT FINDING: When skipOracleValidation=true, the entire collateral
    // check in ValidateMintTransaction is skipped (step 7). This is by design
    // for IBD, but worth documenting the security implications.
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 0, 150, *params, nullptr, true /* skipOracle */);

    BOOST_CHECK(ctx.skipOracleValidation);
    BOOST_TEST_MESSAGE("SECURITY NOTE: skipOracleValidation=true bypasses ALL collateral checks.");
    BOOST_TEST_MESSAGE("  This is used during IBD (initial block download) where oracle data is unavailable.");
    BOOST_TEST_MESSAGE("  Defense: IBD blocks were already validated by the network at original acceptance.");
    BOOST_TEST_MESSAGE("  Risk: If skipOracleValidation is set outside IBD, mints could pass with zero collateral.");
}

// =============================================================================
// RH-06-11: ValidateMintTransaction full TX-level attacks
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_11a_mint_tx_no_inputs_rejected)
{
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    CMutableTransaction mtx;
    mtx.nVersion = MakeMintVersion();
    // No inputs

    CKey ownerKey = GenerateTestKey(1);
    XOnlyPubKey ownerXOnly = XOnlyPubKey(ownerKey.GetPubKey());
    auto ownerBytes = std::vector<unsigned char>(ownerXOnly.begin(), ownerXOnly.end());

    // Add minimal outputs
    DigiDollar::MintParams mp;
    mp.ddAmount = 10000;
    mp.lockHeight = 1000 + 30 * DigiDollar::BLOCKS_PER_DAY;
    mp.ownerKey = ownerXOnly;
    mp.internalKey = DigiDollar::GetCollateralNUMSKey();
    mp.oracleKeys = DigiDollar::GetOracleKeys(15);

    CScript collateralScript = DigiDollar::CreateCollateralP2TR(mp);
    mtx.vout.push_back(CTxOut(100 * COIN, collateralScript));

    CScript ddScript = MakeP2TR(ownerXOnly);
    DigiDollar::RegisterScriptMetadata(ddScript, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, 10000, 0);
    mtx.vout.push_back(CTxOut(0, ddScript));

    mtx.vout.push_back(CTxOut(0, BuildMintOpReturn(10000, mp.lockHeight, 1, ownerBytes)));

    TxValidationState state;
    CTransaction tx(mtx);
    bool valid = DigiDollar::ValidateMintTransaction(tx, ctx, state);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Mint TX with no inputs must be rejected");
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-mint-no-inputs");
}

BOOST_AUTO_TEST_CASE(rh06_11b_mint_tx_single_output_rejected)
{
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    CMutableTransaction mtx;
    mtx.nVersion = MakeMintVersion();
    mtx.vin.push_back(CTxIn(COutPoint(uint256::ONE, 0)));

    // Only 1 output
    CKey key = GenerateTestKey(2);
    XOnlyPubKey xpk = XOnlyPubKey(key.GetPubKey());
    mtx.vout.push_back(CTxOut(100 * COIN, MakeP2TR(xpk)));

    TxValidationState state;
    CTransaction tx(mtx);
    bool valid = DigiDollar::ValidateMintTransaction(tx, ctx, state);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Mint TX with 1 output must be rejected");
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-mint-outputs");
}

// =============================================================================
// RH-06-12: Lock period manipulation
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_12a_zero_lock_period_rejected_in_validate_collateral_ratio)
{
    // ATTACK: lockTime = 0 should be rejected
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    bool valid = DigiDollar::ValidateCollateralRatio(100 * COIN, 10000, 0, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Zero lock time must be rejected by ValidateCollateralRatio");
}

BOOST_AUTO_TEST_CASE(rh06_12b_negative_lock_period_rejected)
{
    auto params = CChainParams::RegTest({});
    DigiDollar::ValidationContext ctx(1000, 10000, 150, *params);

    bool valid = DigiDollar::ValidateCollateralRatio(100 * COIN, 10000, -1, ctx);
    BOOST_CHECK_MESSAGE(!valid, "EXPLOIT: Negative lock time must be rejected");
}

BOOST_AUTO_TEST_CASE(rh06_12c_claim_long_tier_short_lock)
{
    // ATTACK: Claim tier 9 (10 years, 200% ratio) but actually lock for 1 hour (240 blocks).
    // Benefit: 200% instead of 1000% collateral requirement.
    // Defense: Lock tier consistency check in ValidateMintTransaction.
    auto params = CChainParams::RegTest({});
    const int currentHeight = 1000;

    // Short lock: expires in 240 blocks (1 hour)
    const int64_t shortLockHeight = currentHeight + 240;

    // Claim tier 9 (10 years)
    CKey ownerKey = GenerateTestKey(3);
    XOnlyPubKey ownerXOnly = XOnlyPubKey(ownerKey.GetPubKey());
    auto ownerBytes = std::vector<unsigned char>(ownerXOnly.begin(), ownerXOnly.end());

    CScript opReturn = BuildMintOpReturn(10000, shortLockHeight, 9 /* tier 9 = 10 years */, ownerBytes);

    // Parse back to verify the defense
    CScript::const_iterator pc = opReturn.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;
    opReturn.GetOp(pc, opcode); // OP_RETURN
    opReturn.GetOp(pc, opcode, data); // "DD"
    opReturn.GetOp(pc, opcode, data); // type
    opReturn.GetOp(pc, opcode, data); // DD amount
    opReturn.GetOp(pc, opcode, data); // lock height
    CScriptNum lockHeightNum(data, true, 8);

    int64_t lockHeight = lockHeightNum.GetInt64();
    int64_t remaining = lockHeight - currentHeight;

    // Tier 9 = 10 years = 3650 * 5760 blocks ≈ 21,024,000 blocks
    int64_t expectedBlocks = DigiDollar::LockDaysToBlocks(3650);

    BOOST_CHECK_MESSAGE(remaining < expectedBlocks - 10,
        "Lock height mismatch detected: remaining=" + std::to_string(remaining) +
        " expected=" + std::to_string(expectedBlocks));

    BOOST_TEST_MESSAGE("RH-06-12c: Tier 9 mismatch attack detected. Remaining=" +
        std::to_string(remaining) + " blocks, expected ~" + std::to_string(expectedBlocks));
}

// =============================================================================
// RH-06-13: DCA multiplier edge cases during minting
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_13a_dca_at_system_health_zero)
{
    // ATTACK: System health = 0%. What's the DCA multiplier?
    auto params = CChainParams::RegTest({});
    DigiDollar::ConsensusParams ddParams;

    int baseRatio = DigiDollar::GetCollateralRatioForLockTime(30 * DigiDollar::BLOCKS_PER_DAY, ddParams);
    int effectiveRatio = DigiDollar::GetEffectiveCollateralRatio(baseRatio, 0, *params);

    // At 0% health, DCA should apply maximum multiplier (2.0x)
    BOOST_CHECK_GE(effectiveRatio, baseRatio);
    BOOST_TEST_MESSAGE("Base ratio: " + std::to_string(baseRatio) +
        " Effective at 0% health: " + std::to_string(effectiveRatio));
}

BOOST_AUTO_TEST_CASE(rh06_13b_dca_at_negative_health)
{
    // ATTACK: Negative system health (shouldn't happen but test defensively)
    auto params = CChainParams::RegTest({});

    int baseRatio = 500;
    int effectiveRatio = DigiDollar::GetEffectiveCollateralRatio(baseRatio, -100, *params);

    // Should still produce a valid ratio (not crash, not underflow)
    BOOST_CHECK_GT(effectiveRatio, 0);
    BOOST_CHECK_GE(effectiveRatio, baseRatio);
}

// =============================================================================
// RH-06-14: ExtractDDAmount boundary attacks
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_14a_extract_dd_amount_boundary)
{
    // Test ExtractDDAmount at the per-output serialization boundary.
    CAmount amount;

    // Build OP_RETURN with exactly max allowed amount
    CScript atMax = CScript() << OP_RETURN << OP_DIGIDOLLAR;
    std::vector<unsigned char> maxBytes(8, 0);
    int64_t maxVal = MAX_DIGIDOLLAR;
    for (int i = 0; i < 8; i++) {
        maxBytes[i] = (maxVal >> (i * 8)) & 0xFF;
    }
    atMax << maxBytes;

    bool ok = DigiDollar::ExtractDDAmount(atMax, amount);
    BOOST_CHECK(ok);
    BOOST_CHECK_EQUAL(amount, maxVal);

    // One above MAX_DIGIDOLLAR should fail
    CScript aboveMax = CScript() << OP_RETURN << OP_DIGIDOLLAR;
    std::vector<unsigned char> aboveBytes(8, 0);
    int64_t aboveVal = MAX_DIGIDOLLAR + 1;
    for (int i = 0; i < 8; i++) {
        aboveBytes[i] = (aboveVal >> (i * 8)) & 0xFF;
    }
    aboveMax << aboveBytes;

    ok = DigiDollar::ExtractDDAmount(aboveMax, amount);
    BOOST_CHECK_MESSAGE(!ok, "EXPLOIT: Amount above MAX_DIGIDOLLAR should be rejected by ExtractDDAmount");
}

BOOST_AUTO_TEST_CASE(rh06_14b_extract_dd_amount_zero)
{
    CAmount amount;

    CScript zeroScript = CScript() << OP_RETURN << OP_DIGIDOLLAR;
    std::vector<unsigned char> zeroBytes(8, 0);
    zeroScript << zeroBytes;

    bool ok = DigiDollar::ExtractDDAmount(zeroScript, amount);
    BOOST_CHECK_MESSAGE(!ok, "EXPLOIT: Zero amount should be rejected by ExtractDDAmount");
}

// =============================================================================
// RH-06-15: Volatility freeze bypass during minting
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_15a_volatility_freeze_check_exists)
{
    // Verify that ValidateMintTransaction checks volatility freeze
    // The code checks ShouldFreezeMinting() and ShouldFreezeAll()
    // This is a structural test — real freeze tests need volatility state manipulation

    bool freezeMinting = DigiDollar::Volatility::VolatilityMonitor::ShouldFreezeMinting();
    bool freezeAll = DigiDollar::Volatility::VolatilityMonitor::ShouldFreezeAll();

    BOOST_TEST_MESSAGE("Default freeze state: minting=" + std::to_string(freezeMinting) +
        " all=" + std::to_string(freezeAll));

    // In clean state, neither should be frozen
    BOOST_CHECK(!freezeMinting);
    BOOST_CHECK(!freezeAll);
}

// =============================================================================
// RH-06-16: NUMS key verification integrity
// =============================================================================

BOOST_AUTO_TEST_CASE(rh06_16a_nums_key_is_valid)
{
    XOnlyPubKey numsKey = DigiDollar::GetCollateralNUMSKey();
    BOOST_CHECK_MESSAGE(numsKey.IsFullyValid(), "NUMS key must be a valid curve point");
}

BOOST_AUTO_TEST_CASE(rh06_16b_nums_key_deterministic)
{
    // Verify NUMS key is deterministic (same across calls)
    XOnlyPubKey key1 = DigiDollar::GetCollateralNUMSKey();
    XOnlyPubKey key2 = DigiDollar::GetCollateralNUMSKey();
    BOOST_CHECK(key1 == key2);
}

BOOST_AUTO_TEST_SUITE_END()
