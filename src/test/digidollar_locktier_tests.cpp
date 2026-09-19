// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <consensus/volatility.h>
#include <digidollar/scripts.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <key.h>
#include <kernel/chainparams.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <test/util/source_root.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

struct LockTierCase {
    int tier;
    int days;
    int ratio;
};

static const std::vector<LockTierCase>& CanonicalLockTiers()
{
    static const std::vector<LockTierCase> tiers{
        {0, 0, 1000},
        {1, 30, 500},
        {2, 90, 400},
        {3, 180, 350},
        {4, 365, 300},
        {5, 730, 275},
        {6, 1095, 250},
        {7, 1825, 225},
        {8, 2555, 212},
        {9, 3650, 200},
    };
    return tiers;
}

struct DigiDollarLockTierTestSetup : public TestingSetup {
    DigiDollarLockTierTestSetup()
        : TestingSetup(ChainType::REGTEST)
    {
        owner_key.MakeNewKey(true);
        owner_xonly = XOnlyPubKey(owner_key.GetPubKey());
        ResetVolatilityState();
    }

    // DD-FA-TEST-045: mirror DD-FA-TEST-002/005 — clear shared
    // VolatilityMonitor history/freeze on teardown so downstream suites
    // never inherit a frozen-or-populated price history from this fixture.
    ~DigiDollarLockTierTestSetup()
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearFreeze();
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    }

    static constexpr int CURRENT_HEIGHT = 1000;
    static constexpr CAmount ORACLE_PRICE_MICRO_USD = 1000000;
    static constexpr int SYSTEM_COLLATERAL = 150;
    static constexpr CAmount DD_AMOUNT = 10000;

    DigiDollar::ValidationContext MakeContext() const
    {
        return MakeContextAtHeight(CURRENT_HEIGHT);
    }

    DigiDollar::ValidationContext MakeContextAtHeight(int height) const
    {
        return DigiDollar::ValidationContext(height,
                                             ORACLE_PRICE_MICRO_USD,
                                             SYSTEM_COLLATERAL,
                                             Params());
    }

    void ResetVolatilityState() const
    {
        DigiDollar::Volatility::VolatilityMonitor::ReconstructFromBlockData({}, CURRENT_HEIGHT);
        DigiDollar::Volatility::VolatilityMonitor::ClearFreeze();
    }

    CAmount RequiredCollateralAtRatio(int ratio) const
    {
        const util::int128_t numerator = static_cast<util::int128_t>(DD_AMOUNT) *
                                   static_cast<util::int128_t>(COIN) *
                                   static_cast<util::int128_t>(ratio) *
                                   static_cast<util::int128_t>(100);
        const util::int128_t denominator = ORACLE_PRICE_MICRO_USD;
        return static_cast<CAmount>((numerator + denominator - 1) / denominator);
    }

    CTransaction CreateMintTx(int64_t lock_blocks, int64_t lock_tier, CAmount collateral_amount) const
    {
        const int64_t lock_height = CURRENT_HEIGHT + lock_blocks;

        DigiDollar::MintParams params;
        params.ddAmount = DD_AMOUNT;
        params.lockHeight = lock_height;
        params.ownerKey = owner_xonly;
        params.internalKey = DigiDollar::GetCollateralNUMSKey();
        params.oracleKeys = DigiDollar::GetOracleKeys(15);

        const CScript collateral_script = DigiDollar::CreateCollateralP2TR(params);
        const CScript dd_script = DigiDollar::CreateDigiDollarP2TR(owner_xonly, DD_AMOUNT);
        const CScript op_return = CScript() << OP_RETURN
                                            << std::vector<unsigned char>{'D', 'D'}
                                            << CScriptNum(1)
                                            << CScriptNum(DD_AMOUNT)
                                            << CScriptNum(lock_height)
                                            << CScriptNum(lock_tier)
                                            << std::vector<unsigned char>(owner_xonly.begin(), owner_xonly.end());

        CMutableTransaction mtx;
        mtx.nVersion = 0x01000770;
        mtx.vin.push_back(CTxIn(COutPoint(uint256S("0202020202020202020202020202020202020202020202020202020202020202"), 0)));
        mtx.vout.push_back(CTxOut(collateral_amount, collateral_script));
        mtx.vout.push_back(CTxOut(0, dd_script));
        mtx.vout.push_back(CTxOut(0, op_return));
        return CTransaction(mtx);
    }

    // Variant that lets the caller inject an arbitrary tier-byte payload into
    // the OP_RETURN. The validator parses the lock tier from the OP_RETURN
    // bytes via CScriptNum, so this lets us exercise tier values outside the
    // single-byte signed-integer range that CScriptNum normally produces.
    CTransaction CreateMintTxWithRawTier(int64_t lock_blocks,
                                         const std::vector<unsigned char>& tier_payload,
                                         CAmount collateral_amount) const
    {
        const int64_t lock_height = CURRENT_HEIGHT + lock_blocks;

        DigiDollar::MintParams params;
        params.ddAmount = DD_AMOUNT;
        params.lockHeight = lock_height;
        params.ownerKey = owner_xonly;
        params.internalKey = DigiDollar::GetCollateralNUMSKey();
        params.oracleKeys = DigiDollar::GetOracleKeys(15);

        const CScript collateral_script = DigiDollar::CreateCollateralP2TR(params);
        const CScript dd_script = DigiDollar::CreateDigiDollarP2TR(owner_xonly, DD_AMOUNT);
        const CScript op_return = CScript() << OP_RETURN
                                            << std::vector<unsigned char>{'D', 'D'}
                                            << CScriptNum(1)
                                            << CScriptNum(DD_AMOUNT)
                                            << CScriptNum(lock_height)
                                            << tier_payload
                                            << std::vector<unsigned char>(owner_xonly.begin(), owner_xonly.end());

        CMutableTransaction mtx;
        mtx.nVersion = 0x01000770;
        mtx.vin.push_back(CTxIn(COutPoint(uint256S("0202020202020202020202020202020202020202020202020202020202020202"), 0)));
        mtx.vout.push_back(CTxOut(collateral_amount, collateral_script));
        mtx.vout.push_back(CTxOut(0, dd_script));
        mtx.vout.push_back(CTxOut(0, op_return));
        return CTransaction(mtx);
    }

    CKey owner_key;
    XOnlyPubKey owner_xonly;
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_locktier_tests, DigiDollarLockTierTestSetup)

BOOST_AUTO_TEST_CASE(canonical_lock_tiers_accept_exact_collateral_ratios)
{
    const auto& params = Params().GetDigiDollarParams();

    for (const LockTierCase& tier : CanonicalLockTiers()) {
        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(tier.days);
        BOOST_CHECK_EQUAL(DigiDollar::GetCollateralRatioForLockTime(lock_blocks, params), tier.ratio);

        const CAmount collateral = RequiredCollateralAtRatio(tier.ratio);
        const CTransaction tx = CreateMintTx(lock_blocks, tier.tier, collateral);
        DigiDollar::ValidationContext ctx = MakeContext();
        TxValidationState state;

        ResetVolatilityState();
        BOOST_CHECK_MESSAGE(DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state),
            "Canonical tier " + std::to_string(tier.tier) +
            " should accept lock_blocks=" + std::to_string(lock_blocks) +
            " at ratio=" + std::to_string(tier.ratio) +
            ". Reject reason: " + state.GetRejectReason());
    }
}

BOOST_AUTO_TEST_CASE(tier_confirmation_buffer_window_accepts_and_bounds_duration)
{
    for (const LockTierCase& tier : CanonicalLockTiers()) {
        const int64_t canonical_blocks = DigiDollar::LockDaysToBlocks(tier.days);
        const CAmount collateral = RequiredCollateralAtRatio(tier.ratio);

        // Wallets may add up to MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS so a mint
        // that misses the immediately predicted block remains mineable while
        // never becoming under-locked for its claimed tier.
        for (const int64_t lock_blocks : {canonical_blocks,
                                          canonical_blocks + 1,
                                          canonical_blocks + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS}) {
            const CTransaction tx = CreateMintTx(lock_blocks, tier.tier, collateral);
            DigiDollar::ValidationContext ctx = MakeContext();
            TxValidationState state;

            ResetVolatilityState();
            BOOST_CHECK_MESSAGE(DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state),
                "Tier " + std::to_string(tier.tier) +
                " lock_blocks=" + std::to_string(lock_blocks) +
                " should accept within confirmation buffer. Reject reason: " + state.GetRejectReason());
        }

        for (const int64_t lock_blocks : {canonical_blocks - 1,
                                          canonical_blocks + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS + 1}) {
            BOOST_REQUIRE(lock_blocks > 0);
            const CTransaction tx = CreateMintTx(lock_blocks, tier.tier, collateral);
            DigiDollar::ValidationContext ctx = MakeContext();
            TxValidationState state;

            ResetVolatilityState();
            const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
            BOOST_CHECK_MESSAGE(!valid,
                "Out-of-window tier " + std::to_string(tier.tier) +
                " duration lock_blocks=" + std::to_string(lock_blocks) +
                " should reject; accepted with reject reason: " + state.GetRejectReason());
            BOOST_CHECK_MESSAGE(state.GetRejectReason().find("bad-mint-lock-tier-duration") != std::string::npos,
                "Tier " + std::to_string(tier.tier) +
                " out-of-window duration must reject with bad-mint-lock-tier-duration, got: " +
                state.GetRejectReason());
        }
    }
}

BOOST_AUTO_TEST_CASE(buffered_mint_remains_valid_after_confirmation_delay)
{
    for (const LockTierCase& tier : CanonicalLockTiers()) {
        const int64_t canonical_blocks = DigiDollar::LockDaysToBlocks(tier.days);
        const CAmount collateral = RequiredCollateralAtRatio(tier.ratio);
        const CTransaction tx = CreateMintTx(canonical_blocks + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS,
                                             tier.tier,
                                             collateral);

        // Simulate a mint created at CURRENT_HEIGHT but mined after the full
        // buffer. Remaining lock is exactly canonical and must still validate.
        DigiDollar::ValidationContext delayed_ctx = MakeContextAtHeight(CURRENT_HEIGHT + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS);
        TxValidationState delayed_state;
        ResetVolatilityState();
        BOOST_CHECK_MESSAGE(DigiDollar::ValidateDigiDollarTransaction(tx, delayed_ctx, delayed_state),
            "Buffered tier " + std::to_string(tier.tier) +
            " mint should remain valid after confirmation delay. Reject reason: " + delayed_state.GetRejectReason());

        // One more block would under-lock the claimed tier and must reject.
        DigiDollar::ValidationContext expired_ctx = MakeContextAtHeight(CURRENT_HEIGHT + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS + 1);
        TxValidationState expired_state;
        ResetVolatilityState();
        BOOST_CHECK_MESSAGE(!DigiDollar::ValidateDigiDollarTransaction(tx, expired_ctx, expired_state),
            "Buffered tier " + std::to_string(tier.tier) +
            " mint delayed beyond buffer should reject as under-locked");
        BOOST_CHECK_MESSAGE(expired_state.GetRejectReason().find("bad-mint-lock-tier-duration") != std::string::npos,
            "Expired buffered mint must reject with bad-mint-lock-tier-duration, got: " + expired_state.GetRejectReason());
    }
}

BOOST_AUTO_TEST_CASE(buffered_mint_still_requires_claimed_tier_collateral)
{
    for (const LockTierCase& tier : CanonicalLockTiers()) {
        const int64_t canonical_blocks = DigiDollar::LockDaysToBlocks(tier.days);
        const CAmount required_collateral = RequiredCollateralAtRatio(tier.ratio);
        BOOST_REQUIRE(required_collateral > 0);

        const CTransaction tx = CreateMintTx(canonical_blocks + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS,
                                             tier.tier,
                                             required_collateral - 1);
        DigiDollar::ValidationContext ctx = MakeContextAtHeight(CURRENT_HEIGHT + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS);
        TxValidationState state;

        ResetVolatilityState();
        BOOST_CHECK_MESSAGE(!DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state),
            "Buffered tier " + std::to_string(tier.tier) +
            " mint with one sat below required claimed-tier collateral should reject");
        BOOST_CHECK_MESSAGE(state.GetRejectReason().find("insufficient-collateral") != std::string::npos ||
                            state.GetRejectReason().find("bad-collateral-ratio") != std::string::npos,
            "Buffered undercollateralized mint must reject for collateral, got: " + state.GetRejectReason());
    }
}

BOOST_AUTO_TEST_CASE(delayed_buffered_mint_prices_collateral_from_claimed_tier)
{
    // Tier 1 requires 500% collateral. After the 100-block confirmation buffer
    // is fully consumed, the remaining lock is exactly canonical tier 1. The
    // validator must still price collateral from the claimed tier and reject
    // any lower amount; delay must not create a collateral discount.
    const LockTierCase tier{1, 30, 500};
    const int64_t canonical_blocks = DigiDollar::LockDaysToBlocks(tier.days);
    const CAmount tier_required_collateral = RequiredCollateralAtRatio(tier.ratio);
    const CAmount lower_collateral = tier_required_collateral - 1;

    const CTransaction tx = CreateMintTx(canonical_blocks + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS,
                                         tier.tier,
                                         lower_collateral);
    DigiDollar::ValidationContext delayed_ctx = MakeContextAtHeight(CURRENT_HEIGHT + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS);
    TxValidationState delayed_state;

    ResetVolatilityState();
    BOOST_CHECK_MESSAGE(!DigiDollar::ValidateDigiDollarTransaction(tx, delayed_ctx, delayed_state),
        "Delayed buffered tier 1 mint must still require full claimed-tier collateral");
    BOOST_CHECK_MESSAGE(delayed_state.GetRejectReason().find("insufficient-collateral") != std::string::npos ||
                        delayed_state.GetRejectReason().find("bad-collateral-ratio") != std::string::npos,
        "Delayed buffered undercollateralized mint must reject for collateral, got: " + delayed_state.GetRejectReason());
}

BOOST_AUTO_TEST_CASE(arbitrary_custom_lock_duration_rejects)
{
    const int64_t forty_five_days = DigiDollar::LockDaysToBlocks(45);
    const int claimed_tier = 1;
    const CAmount tier_one_collateral = RequiredCollateralAtRatio(/*ratio=*/500);
    const CTransaction tx = CreateMintTx(forty_five_days, claimed_tier, tier_one_collateral);
    DigiDollar::ValidationContext ctx = MakeContext();
    TxValidationState state;

    ResetVolatilityState();
    const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK_MESSAGE(!valid,
        "Arbitrary custom lock duration should reject; accepted with reject reason: " + state.GetRejectReason());
    BOOST_CHECK_MESSAGE(state.GetRejectReason().find("bad-mint-lock-tier-duration") != std::string::npos,
        "45-day lock with claimed tier 1 must reject with bad-mint-lock-tier-duration, got: " +
        state.GetRejectReason());
}

// =========================================================================
// Wave 7: tier-byte boundary coverage.
// The validator must reject any tier byte outside 0..9 with bad-mint-lock-tier
// (validation.cpp:1264-1268). Inside 0..9, lockBlocks must match the canonical
// duration for the claimed tier or the validator returns
// bad-mint-lock-tier-duration (validation.cpp:1290-1292).
// =========================================================================

BOOST_AUTO_TEST_CASE(wave7_tier_byte_out_of_range_rejected)
{
    // Lock blocks set to a canonical 30d duration so the lockHeight itself
    // is benign; the only thing the test perturbs is the tier byte. This
    // confirms the tier-range gate fires before the duration cross-check.
    const int64_t canonical_blocks = DigiDollar::LockDaysToBlocks(30);
    const CAmount collateral = RequiredCollateralAtRatio(/*ratio=*/500);

    // Cover representative invalid tier values: 10 (just above max),
    // 11, 127 (max single-byte CScriptNum), -1 (signed wrap path), -2.
    // Tiers >127 require the multi-byte CScriptNum encoding which we
    // construct manually below.
    const std::vector<int64_t> invalid_tiers = {10, 11, 127, -1, -2};

    for (const int64_t bad_tier : invalid_tiers) {
        const CTransaction tx = CreateMintTx(canonical_blocks, bad_tier, collateral);
        DigiDollar::ValidationContext ctx = MakeContext();
        TxValidationState state;

        ResetVolatilityState();
        const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        BOOST_CHECK_MESSAGE(!valid,
            "Tier byte " + std::to_string(bad_tier) + " should reject; accepted.");
        BOOST_CHECK_MESSAGE(state.GetRejectReason().find("bad-mint-lock-tier") != std::string::npos,
            "Tier byte " + std::to_string(bad_tier) +
            " must reject with bad-mint-lock-tier (or duration variant), got: " +
            state.GetRejectReason());
    }
}

BOOST_AUTO_TEST_CASE(wave7_tier_byte_multi_byte_rejected)
{
    // Tier values >127 cannot be expressed as a single-byte CScriptNum, so
    // craft the OP_RETURN bytes directly. 255 requires a 2-byte little-endian
    // unsigned representation: 0xff 0x00 (positive, sign byte appended).
    // INT32_MAX requires the full 5-byte encoding (4 magnitude + sign byte).
    const int64_t canonical_blocks = DigiDollar::LockDaysToBlocks(90);
    const CAmount collateral = RequiredCollateralAtRatio(/*ratio=*/400);

    struct TierPayloadCase {
        std::string label;
        std::vector<unsigned char> payload;
    };

    const std::vector<TierPayloadCase> cases = {
        // 255 little-endian with explicit positive sign byte.
        {"u8_max_255", {0xff, 0x00}},
        // 256: 0x00 0x01 0x00 (LE bytes + sign byte)
        {"u16_low_256", {0x00, 0x01, 0x00}},
        // 65535
        {"u16_max_65535", {0xff, 0xff, 0x00}},
        // INT32_MAX = 0x7fffffff plus positive sign byte
        {"i32_max", {0xff, 0xff, 0xff, 0x7f, 0x00}},
    };

    for (const TierPayloadCase& tc : cases) {
        const CTransaction tx = CreateMintTxWithRawTier(canonical_blocks, tc.payload, collateral);
        DigiDollar::ValidationContext ctx = MakeContext();
        TxValidationState state;

        ResetVolatilityState();
        const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        BOOST_CHECK_MESSAGE(!valid,
            "Tier payload " + tc.label + " should reject; accepted.");
        // The validator may surface either bad-mint-lock-tier (range gate),
        // bad-mint-lock-tier-parse (>4-byte CScriptNum throws), or
        // bad-mint-lock-tier-duration. Any of these confirms tier-byte rejection.
        const std::string& reason = state.GetRejectReason();
        const bool reason_ok =
            reason.find("bad-mint-lock-tier") != std::string::npos;
        BOOST_CHECK_MESSAGE(reason_ok,
            "Tier payload " + tc.label +
            " must reject with a lock-tier reason, got: " + reason);
    }
}

BOOST_AUTO_TEST_CASE(wave7_tier_byte_lockblocks_cross_mismatch)
{
    // Cross-mismatch matrix: tier byte claims tier N but lockBlocks matches
    // the canonical duration of tier M (M != N). Validator must reject every
    // off-diagonal cell with bad-mint-lock-tier-duration.
    const auto& canonical = CanonicalLockTiers();

    for (const LockTierCase& claimed : canonical) {
        for (const LockTierCase& actual : canonical) {
            if (claimed.tier == actual.tier) continue;

            const int64_t actual_blocks = DigiDollar::LockDaysToBlocks(actual.days);
            // Provide collateral sized for whichever is more demanding so the
            // duration check, not insufficient-collateral, is the failure path.
            const CAmount collateral = std::max(RequiredCollateralAtRatio(claimed.ratio),
                                                RequiredCollateralAtRatio(actual.ratio));
            const CTransaction tx = CreateMintTx(actual_blocks, claimed.tier, collateral);
            DigiDollar::ValidationContext ctx = MakeContext();
            TxValidationState state;

            ResetVolatilityState();
            const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
            BOOST_CHECK_MESSAGE(!valid,
                "Tier byte " + std::to_string(claimed.tier) +
                " with lock_blocks for tier " + std::to_string(actual.tier) +
                " should reject; accepted.");
            BOOST_CHECK_MESSAGE(state.GetRejectReason().find("bad-mint-lock-tier-duration") != std::string::npos,
                "Tier byte " + std::to_string(claimed.tier) +
                " with lock_blocks for tier " + std::to_string(actual.tier) +
                " must reject with bad-mint-lock-tier-duration, got: " + state.GetRejectReason());
        }
    }
}

BOOST_AUTO_TEST_CASE(wave7_random_non_canonical_durations_rejected)
{
    // Sample a spread of arbitrary non-canonical durations across the supported
    // range. Tier byte is fixed to a valid tier so the duration cross-check is
    // the failure path. Each chosen value must NOT be canonical.
    const auto& params = Params().GetDigiDollarParams();
    const std::vector<int64_t> non_canonical_blocks = {
        2,                                               // single block
        239,                                             // tier 0 - 1
        341,                                             // tier 0 + buffer + 1
        720,                                             // 3 hours
        DigiDollar::LockDaysToBlocks(7),                 // 1 week
        DigiDollar::LockDaysToBlocks(14),                // 2 weeks
        DigiDollar::LockDaysToBlocks(45),                // 45 days
        DigiDollar::LockDaysToBlocks(60),                // 60 days
        DigiDollar::LockDaysToBlocks(120),               // 120 days
        DigiDollar::LockDaysToBlocks(200),               // ~6.5 months
        DigiDollar::LockDaysToBlocks(400),               // ~13 months
        DigiDollar::LockDaysToBlocks(500),               // ~16 months
        DigiDollar::LockDaysToBlocks(1000),              // ~2.7 years
        DigiDollar::LockDaysToBlocks(2000),              // ~5.5 years
        DigiDollar::LockDaysToBlocks(4000),              // ~11 years (above max canonical)
        DigiDollar::LockDaysToBlocks(7300),              // 20 years
    };

    const CAmount collateral = RequiredCollateralAtRatio(/*ratio=*/1000);
    for (const int64_t lock_blocks : non_canonical_blocks) {
        BOOST_REQUIRE_MESSAGE(!DigiDollar::IsCanonicalLockTier(lock_blocks, params),
            "Test bug: " + std::to_string(lock_blocks) + " is unexpectedly canonical");

        // Use tier byte 0 so the bad-mint-lock-tier-duration check is the gate.
        const CTransaction tx = CreateMintTx(lock_blocks, /*lock_tier=*/0, collateral);
        DigiDollar::ValidationContext ctx = MakeContext();
        TxValidationState state;

        ResetVolatilityState();
        const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        BOOST_CHECK_MESSAGE(!valid,
            "Non-canonical lock_blocks=" + std::to_string(lock_blocks) +
            " should reject; accepted.");
        const std::string& reason = state.GetRejectReason();
        const bool reason_ok =
            reason.find("bad-mint-lock-tier") != std::string::npos ||
            reason.find("bad-mint-lock-period") != std::string::npos;
        BOOST_CHECK_MESSAGE(reason_ok,
            "Non-canonical lock_blocks=" + std::to_string(lock_blocks) +
            " must reject with a lock-tier or lock-period reason, got: " + reason);
    }
}

BOOST_AUTO_TEST_CASE(wave7_iscanonical_locktier_table_drives_acceptance)
{
    // Pure table-driven proof: IsCanonicalLockTier accepts only the 10 canonical
    // values and rejects anything else. This isolates the consensus helper from
    // surrounding mint-validation noise.
    const auto& params = Params().GetDigiDollarParams();

    for (const LockTierCase& tier : CanonicalLockTiers()) {
        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(tier.days);
        BOOST_CHECK(DigiDollar::IsCanonicalLockTier(lock_blocks, params));
        BOOST_CHECK_EQUAL(DigiDollar::GetLockTierIndex(lock_blocks, params), tier.tier);
        BOOST_CHECK_EQUAL(DigiDollar::GetCollateralRatioForLockTime(lock_blocks, params), tier.ratio);
        // Adjacent values must NOT be canonical.
        BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(lock_blocks - 1, params));
        BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(lock_blocks + 1, params));
        BOOST_CHECK_EQUAL(DigiDollar::GetCollateralRatioForLockTime(lock_blocks - 1, params), 0);
        BOOST_CHECK_EQUAL(DigiDollar::GetCollateralRatioForLockTime(lock_blocks + 1, params), 0);
    }

    // Additional sanity: extreme/negative values must be rejected. The helper
    // returns false for any non-canonical key — exercising 0, -1, INT64_MAX,
    // INT64_MIN documents the closed-set invariant.
    BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(0, params));
    BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(-1, params));
    BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(std::numeric_limits<int64_t>::max(), params));
    BOOST_CHECK(!DigiDollar::IsCanonicalLockTier(std::numeric_limits<int64_t>::min(), params));
}

BOOST_AUTO_TEST_CASE(wave7_locktier_duration_reject_log_includes_txid_and_window)
{
    const std::string source = ReadRepositoryFile("src/digidollar/validation.cpp");

    BOOST_CHECK_MESSAGE(source.find("Non-canonical lock duration for mint txid=%s") != std::string::npos,
        "bad-mint-lock-tier-duration logs must identify the exact mint txid");
    BOOST_CHECK_MESSAGE(source.find("expected_range=[%lld,%lld]") != std::string::npos,
        "bad-mint-lock-tier-duration logs must retain the accepted canonical window");
    BOOST_CHECK_MESSAGE(source.find("tx.GetHash().ToString()") != std::string::npos,
        "bad-mint-lock-tier-duration logs must pass the transaction hash to LogPrintf");
}

BOOST_AUTO_TEST_CASE(wave7_txbuilder_rejects_non_canonical_lock_days)
{
    // MintTxBuilder::ValidateMintParams must refuse non-canonical lockDays
    // before the transaction is broadcast. Tier byte is harmless because the
    // tier index check inside the builder enforces lockBlocks ↔ tier match.
    const CChainParams& chain = Params();
    DigiDollar::MintTxBuilder builder(chain, CURRENT_HEIGHT, ORACLE_PRICE_MICRO_USD);

    // First sanity-check that the canonical days do pass tier indexing.
    for (const LockTierCase& tier : CanonicalLockTiers()) {
        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(tier.days);
        BOOST_CHECK_MESSAGE(
            DigiDollar::GetLockTierIndex(lock_blocks, chain.GetDigiDollarParams()) ==
                static_cast<int>(tier.tier),
            "Canonical tier " + std::to_string(tier.tier) +
            " must map to its own index in the chainparams table");
    }

    // Custom non-canonical durations (in days) must fail ValidateMintParams.
    const std::vector<int> bad_lock_days = {1, 7, 14, 31, 45, 60, 91, 200, 366, 731, 4000};
    for (const int days : bad_lock_days) {
        DigiDollar::TxBuilderMintParams params;
        params.ddAmount = 10000;
        params.lockDays = days;
        // Use tier 1 so we are certain validation fails for a tier reason
        // (the index gate sees lockBlocks ≠ tier 1's canonical 30d duration).
        params.lockTier = 1;
        params.ownerKey = owner_key;
        params.feeRate = 100000;
        params.utxos.push_back(COutPoint(uint256S("0303030303030303030303030303030303030303030303030303030303030303"), 0));

        BOOST_CHECK_MESSAGE(!builder.ValidateMintParams(params),
            "TxBuilder must reject non-canonical lockDays=" + std::to_string(days));
    }

    // And tier byte mismatched with a valid lockDays must also reject.
    for (const LockTierCase& tier : CanonicalLockTiers()) {
        DigiDollar::TxBuilderMintParams params;
        params.ddAmount = 10000;
        params.lockDays = tier.days;
        // Wrong tier byte: pick the next tier, wrapping around the 0..9 range.
        params.lockTier = static_cast<uint32_t>((tier.tier + 1) % 10);
        params.ownerKey = owner_key;
        params.feeRate = 100000;
        params.utxos.push_back(COutPoint(uint256S("0404040404040404040404040404040404040404040404040404040404040404"), 0));

        BOOST_CHECK_MESSAGE(!builder.ValidateMintParams(params),
            "TxBuilder must reject lockDays=" + std::to_string(tier.days) +
            " with mismatched tier byte=" + std::to_string(params.lockTier));
    }
}

BOOST_AUTO_TEST_CASE(wave7_per_network_canonical_tiers_match_consensus_default)
{
    // The canonical lock tiers are part of the DigiDollar consensus default
    // (consensus/digidollar.h) and chainparams does not override the
    // collateralRatios map on any network. Verify that mainnet, testnet, and
    // regtest all expose the same 10-tier table so a regtest-only short tier
    // (240 blocks = 1 hour) is also accepted by mainnet/testnet.
    const std::vector<ChainType> networks = {
        ChainType::MAIN, ChainType::TESTNET, ChainType::REGTEST};

    const auto& canonical = CanonicalLockTiers();
    for (ChainType chain_type : networks) {
        std::unique_ptr<const CChainParams> params;
        if (chain_type == ChainType::REGTEST) {
            params = CChainParams::RegTest({});
        } else if (chain_type == ChainType::TESTNET) {
            params = CChainParams::TestNet();
        } else {
            params = CChainParams::Main();
        }
        const auto& dd = params->GetDigiDollarParams();
        BOOST_CHECK_EQUAL(dd.collateralRatios.size(), canonical.size());
        for (const LockTierCase& tier : canonical) {
            const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(tier.days);
            BOOST_CHECK_MESSAGE(DigiDollar::IsCanonicalLockTier(lock_blocks, dd),
                "Network " + std::string(ChainTypeToString(chain_type)) +
                " must accept canonical tier " + std::to_string(tier.tier));
            BOOST_CHECK_EQUAL(DigiDollar::GetCollateralRatioForLockTime(lock_blocks, dd), tier.ratio);
        }
        // Tier 0 (240 blocks = 1 hour) is also enabled on mainnet/testnet — it
        // is the testing/onboarding tier with a 1000% ratio, NOT a regtest-only
        // tier. Confirm that here so a future chainparams override would surface.
        BOOST_CHECK(DigiDollar::IsCanonicalLockTier(240, dd));
        BOOST_CHECK_EQUAL(DigiDollar::GetCollateralRatioForLockTime(240, dd), 1000);
    }
}

BOOST_AUTO_TEST_SUITE_END()
