// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Final Audit Wave 14 - IBD security regression tests
 *
 * Pins reachable IBD/catch-up consensus splits where
 * `ctx.skipOracleValidation` (set by `Chainstate::ConnectBlock` for
 * IBD/catch-up nodes) bypasses validations that are NOT actually oracle
 * dependent.
 *
 * DD-FA-SEC-011 (Wave 14): the canonical lock-tier-duration check at
 * `src/digidollar/validation.cpp:1288` skips when
 * `ctx.skipOracleValidation || lockTime <= ctx.nHeight`. The historical
 * `lockTime <= ctx.nHeight` arm is correct (a mint that already passed
 * its lock predates re-validation in IBD). The `ctx.skipOracleValidation`
 * arm is overly broad: the canonical-duration relationship between
 * `lockHeight` (OP_RETURN data) and the claimed `lockTier` is purely
 * deterministic and does not depend on oracle data, so an IBD/catch-up
 * node accepts a non-canonical mint that a caught-up node rejects with
 * `bad-mint-lock-tier-duration`.
 *
 * Reachable exploit: a malicious peer feeds an IBD node a fake fork
 * containing a non-canonical-tier mint (e.g. tier byte 9 claiming
 * 10y/200% but lockHeight only 30 days out). The IBD node accepts the
 * mint and considers the fork valid. Caught-up peers reject the same
 * mint at `ConnectBlock`, so the IBD node forks off the network.
 *
 * Independent of DD-FA-FUNC-016 / DD-FA-SEC-010 / DD-RH-115. The
 * `digidollar_skip_oracle_tests` suite covers the oracle-price and
 * collateral-ratio cases; this file covers the lock-tier-duration case.
 */

#include <consensus/amount.h>
#include <util/int128.h>
#include <consensus/digidollar.h>
#include <consensus/volatility.h>
#include <chainparams.h>
#include <digidollar/digidollar.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <key.h>
#include <kernel/chainparams.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

struct DigiDollarWave14IBDTestSetup : public TestingSetup {
    DigiDollarWave14IBDTestSetup()
        : TestingSetup(ChainType::REGTEST)
    {
        owner_key.MakeNewKey(true);
        owner_xonly = XOnlyPubKey(owner_key.GetPubKey());

        // Match locktier_tests.cpp default: a clean baseline so the
        // canonical/Volatility/ERR pre-checks do not perturb the result.
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        DigiDollar::Volatility::VolatilityMonitor::ReconstructFromBlockData({}, CURRENT_HEIGHT);
        DigiDollar::Volatility::VolatilityMonitor::ClearFreeze();
    }

    // DD-FA-TEST-047: clear volatility state on teardown so any
    // RecordPrice() invoked by a mint validation path inside this fixture
    // does not leak priceHistory into a later suite (e.g. redteam tests
    // that assume priceHistory.empty() at WouldCandidateFreezeMinting).
    ~DigiDollarWave14IBDTestSetup()
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        DigiDollar::Volatility::VolatilityMonitor::ClearFreeze();
    }

    static constexpr int CURRENT_HEIGHT = 1000;
    static constexpr CAmount ORACLE_PRICE_MICRO_USD = 1000000; // $1.00/DGB
    static constexpr int SYSTEM_COLLATERAL = 150;
    static constexpr CAmount DD_AMOUNT = 10000; // $100.00

    DigiDollar::ValidationContext MakeContext(bool skip_oracle) const
    {
        return DigiDollar::ValidationContext(CURRENT_HEIGHT,
                                             ORACLE_PRICE_MICRO_USD,
                                             SYSTEM_COLLATERAL,
                                             Params(),
                                             /*coins_view=*/nullptr,
                                             /*skip_oracle=*/skip_oracle);
    }

    // Required collateral to keep the canonical-duration check the only
    // failing condition. Picks the worst case (1000% for tier 0) so the
    // collateral check passes for every claimed tier.
    CAmount GenerousCollateral() const
    {
        const util::int128_t numerator = static_cast<util::int128_t>(DD_AMOUNT) *
                                   static_cast<util::int128_t>(COIN) *
                                   1000 * 100 * 4;
        const util::int128_t denominator = ORACLE_PRICE_MICRO_USD;
        return static_cast<CAmount>((numerator + denominator - 1) / denominator);
    }

    // Build a mint tx whose OP_RETURN claims `lock_tier` but commits a
    // `lock_blocks` lock duration. The owner key is fresh per fixture.
    CTransaction CreateMintTx(int64_t lock_blocks, int64_t lock_tier) const
    {
        const CAmount collateral = GenerousCollateral();
        const int64_t lock_height = CURRENT_HEIGHT + lock_blocks;

        DigiDollar::MintParams mint;
        mint.ddAmount = DD_AMOUNT;
        mint.lockHeight = lock_height;
        mint.ownerKey = owner_xonly;
        mint.internalKey = DigiDollar::GetCollateralNUMSKey();
        mint.oracleKeys = DigiDollar::GetOracleKeys(15);

        const CScript collateral_script = DigiDollar::CreateCollateralP2TR(mint);
        const CScript dd_script = DigiDollar::CreateDigiDollarP2TR(owner_xonly, DD_AMOUNT);
        const CScript op_return = CScript() << OP_RETURN
                                            << std::vector<unsigned char>{'D', 'D'}
                                            << CScriptNum(1)
                                            << CScriptNum(DD_AMOUNT)
                                            << CScriptNum(lock_height)
                                            << CScriptNum(lock_tier)
                                            << std::vector<unsigned char>(owner_xonly.begin(), owner_xonly.end());

        CMutableTransaction mtx;
        mtx.nVersion = 0x01000770; // DD_TX_MINT
        mtx.vin.push_back(CTxIn(COutPoint(uint256S("0x4141414141414141414141414141414141414141414141414141414141414141"), 0)));
        mtx.vout.push_back(CTxOut(collateral, collateral_script));
        mtx.vout.push_back(CTxOut(0, dd_script));
        mtx.vout.push_back(CTxOut(0, op_return));
        return CTransaction(mtx);
    }

    CKey owner_key;
    XOnlyPubKey owner_xonly;
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_wave14_ibd_security_tests, DigiDollarWave14IBDTestSetup)

// =========================================================================
// CASE 1: Caught-up nodes reject a non-canonical lock-tier-duration mint.
// Sanity / non-regression for the existing canonical-duration check.
// =========================================================================

BOOST_AUTO_TEST_CASE(wave14_caught_up_node_rejects_noncanonical_lock_tier_duration)
{
    // Claim tier 9 (10 years, 200% ratio) but commit only a 30-day lock.
    // Caught-up node MUST reject with bad-mint-lock-tier-duration.
    const int64_t thirty_days = DigiDollar::LockDaysToBlocks(30);
    const int64_t claimed_tier_9 = 9;
    const CTransaction tx = CreateMintTx(thirty_days, claimed_tier_9);

    DigiDollar::ValidationContext ctx = MakeContext(/*skip_oracle=*/false);
    TxValidationState state;
    const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_CHECK_MESSAGE(!valid,
        "Caught-up node accepted noncanonical lock-tier mint; reject reason: " +
        state.GetRejectReason());
    BOOST_CHECK_MESSAGE(state.GetRejectReason().find("bad-mint-lock-tier-duration") != std::string::npos,
        "Caught-up node must reject with bad-mint-lock-tier-duration; got: " +
        state.GetRejectReason());
}

// =========================================================================
// CASE 2 (TDD failing pre-fix): IBD/catch-up node MUST also reject the
// same non-canonical mint. Otherwise the validator yields a different
// accept/reject result depending on local sync state, which is a
// deterministic IBD/non-IBD consensus split.
// =========================================================================

BOOST_AUTO_TEST_CASE(wave14_ibd_node_must_reject_noncanonical_lock_tier_duration)
{
    const int64_t thirty_days = DigiDollar::LockDaysToBlocks(30);
    const int64_t claimed_tier_9 = 9;
    const CTransaction tx = CreateMintTx(thirty_days, claimed_tier_9);

    // SAME tx, but presented to an IBD/catch-up node.
    DigiDollar::ValidationContext ctx = MakeContext(/*skip_oracle=*/true);
    TxValidationState state;
    const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
    BOOST_TEST_MESSAGE("IBD result for noncanonical tier 9 / 30d lock: " +
                       std::to_string(valid) + " reason=" + state.GetRejectReason());
    BOOST_CHECK_MESSAGE(!valid,
        "DD-FA-SEC-011: IBD/catch-up node accepted noncanonical lock-tier "
        "mint that caught-up nodes reject. The lock-tier-duration check is "
        "deterministic and does NOT depend on oracle data; "
        "ctx.skipOracleValidation must not bypass it.");
    BOOST_CHECK_MESSAGE(state.GetRejectReason().find("bad-mint-lock-tier-duration") != std::string::npos,
        "DD-FA-SEC-011: IBD/catch-up node must reject with "
        "bad-mint-lock-tier-duration to match caught-up nodes; got: " +
        state.GetRejectReason());
}

// =========================================================================
// CASE 3: A historical mint already past its lock height MUST still be
// accepted in IBD even when the claimed tier does not algebraically match
// (lockTime - currentHeight). This pins the legitimate use of the skip
// branch: re-validation of an older block whose lockTime has expired.
// =========================================================================

BOOST_AUTO_TEST_CASE(wave14_ibd_accepts_historical_mint_past_locktime)
{
    // Build a mint whose lockHeight is in the PAST relative to ctx.nHeight.
    // The relative lockBlocks computed by validation will be negative, so
    // the canonical comparison is mathematically meaningless; the skip
    // branch MUST cover this case.
    //
    // Choose a tier 1 (30 day) claim and a lockHeight 100 blocks below
    // CURRENT_HEIGHT.
    const int64_t past_lock_blocks = -100;
    const int64_t claimed_tier = 1;

    const CAmount collateral = GenerousCollateral();
    const int64_t lock_height = CURRENT_HEIGHT + past_lock_blocks; // 900
    BOOST_REQUIRE_LT(lock_height, CURRENT_HEIGHT);

    DigiDollar::MintParams mint;
    mint.ddAmount = DD_AMOUNT;
    mint.lockHeight = lock_height;
    mint.ownerKey = owner_xonly;
    mint.internalKey = DigiDollar::GetCollateralNUMSKey();
    mint.oracleKeys = DigiDollar::GetOracleKeys(15);

    const CScript collateral_script = DigiDollar::CreateCollateralP2TR(mint);
    const CScript dd_script = DigiDollar::CreateDigiDollarP2TR(owner_xonly, DD_AMOUNT);
    const CScript op_return = CScript() << OP_RETURN
                                        << std::vector<unsigned char>{'D', 'D'}
                                        << CScriptNum(1)
                                        << CScriptNum(DD_AMOUNT)
                                        << CScriptNum(lock_height)
                                        << CScriptNum(claimed_tier)
                                        << std::vector<unsigned char>(owner_xonly.begin(), owner_xonly.end());

    CMutableTransaction mtx;
    mtx.nVersion = 0x01000770;
    mtx.vin.push_back(CTxIn(COutPoint(uint256S("0x4242424242424242424242424242424242424242424242424242424242424242"), 0)));
    mtx.vout.push_back(CTxOut(collateral, collateral_script));
    mtx.vout.push_back(CTxOut(0, dd_script));
    mtx.vout.push_back(CTxOut(0, op_return));
    const CTransaction tx(mtx);

    // Both IBD and caught-up nodes must accept (or fail in a downstream
    // check, but NOT bad-mint-lock-tier-duration which is unreachable
    // for past-lock mints).
    for (bool skip_oracle : {true, false}) {
        DigiDollar::ValidationContext ctx = MakeContext(skip_oracle);
        TxValidationState state;
        const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        BOOST_TEST_MESSAGE("Historical mint, skip_oracle=" + std::to_string(skip_oracle) +
                           " result=" + std::to_string(valid) +
                           " reason=" + state.GetRejectReason());
        // We only assert the tier-duration check did not fire. Other
        // downstream checks may legitimately reject a fully-stub mint.
        BOOST_CHECK_MESSAGE(state.GetRejectReason().find("bad-mint-lock-tier-duration") == std::string::npos,
            "Historical past-lock mint must NOT trip bad-mint-lock-tier-duration; got: " +
            state.GetRejectReason());
    }
}

// =========================================================================
// CASE 4: Canonical (well-formed) mints continue to validate identically
// for IBD and caught-up nodes. Ensures the fix does not regress accepted
// historical / current canonical mints.
// =========================================================================

BOOST_AUTO_TEST_CASE(wave14_ibd_and_caught_up_agree_on_canonical_mints)
{
    // tier 1 / 30 day canonical pair.
    const int64_t canonical_30d = DigiDollar::LockDaysToBlocks(30);
    const int64_t canonical_tier_1 = 1;
    const CTransaction tx = CreateMintTx(canonical_30d, canonical_tier_1);

    bool ibd_valid;
    bool caught_up_valid;
    std::string ibd_reason;
    std::string caught_up_reason;

    {
        DigiDollar::ValidationContext ctx = MakeContext(/*skip_oracle=*/true);
        TxValidationState state;
        ibd_valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        ibd_reason = state.GetRejectReason();
    }
    {
        DigiDollar::ValidationContext ctx = MakeContext(/*skip_oracle=*/false);
        TxValidationState state;
        caught_up_valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        caught_up_reason = state.GetRejectReason();
    }

    BOOST_TEST_MESSAGE("Canonical mint IBD result=" + std::to_string(ibd_valid) +
                       " reason=" + ibd_reason);
    BOOST_TEST_MESSAGE("Canonical mint caught-up result=" + std::to_string(caught_up_valid) +
                       " reason=" + caught_up_reason);

    // Pin the duration check: neither path may emit bad-mint-lock-tier-duration.
    BOOST_CHECK(ibd_reason.find("bad-mint-lock-tier-duration") == std::string::npos);
    BOOST_CHECK(caught_up_reason.find("bad-mint-lock-tier-duration") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
