// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/params.h>
#include <consensus/volatility.h>
#include <chainparams.h>
#include <digidollar/validation.h>
#include <test/util/setup_common.h>
#include <qt/digidollar_qt_translate.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <vector>

using namespace DigiDollar::Volatility;

BOOST_AUTO_TEST_SUITE(digidollar_thaw_volatility_tests)

struct ReferenceChain {
    std::vector<CBlockIndex> blocks{1601};
    std::vector<uint256> hashes{1601};
    Consensus::Params params{};
    std::map<int, CAmount> prices;
    std::vector<int> reads;
    int missing{-1};

    ReferenceChain()
    {
        params.DigiDollarHeight = 100;
        params.nDDActivationHeight = 650;
        for (size_t i = 0; i < blocks.size(); ++i) {
            hashes[i] = uint256S(std::to_string(i + 1));
            blocks[i].phashBlock = &hashes[i];
            blocks[i].nHeight = i;
            blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
            blocks[i].BuildSkip();
        }
    }

    MintReference Reference(int height = 1601)
    {
        reads.clear();
        return BuildMintReference(height, &blocks[height - 1], params,
            [&](const CBlockIndex& block, CAmount& price, std::string& error) {
                reads.push_back(block.nHeight);
                if (block.nHeight == missing) {
                    error = "unreadable test ancestor";
                    return AncestorPriceResult::UNAVAILABLE;
                }
                const auto it = prices.find(block.nHeight);
                if (it == prices.end()) return AncestorPriceResult::NO_BUNDLE;
                price = it->second;
                return AncestorPriceResult::PRICE;
            });
    }
};

BOOST_AUTO_TEST_CASE(exact_deviation_and_wide_products)
{
    MintReference reference;
    reference.ready = true;
    reference.sample_count = 1;
    reference.price_micro_usd = 10000;
    BOOST_CHECK(!EvaluateMintPrice(11999, reference).restricted);
    BOOST_CHECK(EvaluateMintPrice(12000, reference).restricted);
    BOOST_CHECK(!EvaluateMintPrice(8001, reference).restricted);
    BOOST_CHECK(EvaluateMintPrice(8000, reference).restricted);
    reference.price_micro_usd = std::numeric_limits<CAmount>::max();
    BOOST_CHECK(!EvaluateMintPrice(std::numeric_limits<CAmount>::max(), reference).restricted);
    BOOST_CHECK(EvaluateMintPrice(1, reference).restricted);
    reference.price_micro_usd = 1;
    BOOST_CHECK(EvaluateMintPrice(std::numeric_limits<CAmount>::max(), reference).restricted);
    BOOST_CHECK(!EvaluateMintPrice(0, reference).quote_available);
    reference.ready = false;
    BOOST_CHECK(!EvaluateMintPrice(10000, reference).ready);
}

BOOST_AUTO_TEST_CASE(window_endpoints_lower_median_and_repeats)
{
    ReferenceChain chain;
    chain.prices = {{1362, 99999}, {1361, 10000}, {1360, 20000}, {161, 30000}, {160, 99999}};
    auto reference = chain.Reference();
    BOOST_REQUIRE(reference.ready);
    BOOST_CHECK_EQUAL(reference.sample_count, 3);
    BOOST_CHECK_EQUAL(reference.price_micro_usd, 20000);
    BOOST_CHECK_EQUAL(chain.reads.front(), 1361);
    BOOST_CHECK_EQUAL(chain.reads.back(), 161);
    chain.prices.erase(161);
    reference = chain.Reference();
    BOOST_CHECK_EQUAL(reference.sample_count, 2);
    BOOST_CHECK_EQUAL(reference.price_micro_usd, 10000);
    chain.prices[1359] = 10000;
    chain.prices[1358] = 10000;
    reference = chain.Reference();
    BOOST_CHECK_EQUAL(reference.sample_count, 4);
    BOOST_CHECK_EQUAL(reference.price_micro_usd, 10000);
}

BOOST_AUTO_TEST_CASE(first_fifteen_closest_and_historical_activation)
{
    ReferenceChain chain;
    for (int i = 0; i < 16; ++i) chain.prices[1361 - i] = 10000 + i;
    auto reference = chain.Reference();
    BOOST_REQUIRE(reference.ready);
    BOOST_CHECK_EQUAL(reference.sample_count, 15);
    BOOST_CHECK_EQUAL(reference.price_micro_usd, 10007);
    BOOST_CHECK_EQUAL(chain.reads.size(), 15);
    chain.missing = 1346;
    BOOST_CHECK(chain.Reference().ready);
    chain.prices = {{200, 10000}, {100, 20000}, {99, 90000}};
    reference = chain.Reference(500);
    BOOST_REQUIRE(reference.ready);
    BOOST_CHECK_EQUAL(reference.sample_count, 2);
    BOOST_CHECK_EQUAL(reference.price_micro_usd, 10000);
    BOOST_CHECK_EQUAL(chain.reads.back(), 100);
}

BOOST_AUTO_TEST_CASE(empty_short_and_missing_are_distinct)
{
    ReferenceChain chain;
    auto reference = chain.Reference();
    BOOST_REQUIRE(reference.ready);
    BOOST_CHECK_EQUAL(reference.sample_count, 0);
    BOOST_CHECK(!EvaluateMintPrice(10000, reference).restricted);
    chain.missing = 1000;
    reference = chain.Reference();
    BOOST_CHECK(!reference.ready);
    BOOST_CHECK(!reference.error.empty());
    chain.missing = -1;
    reference = chain.Reference(200);
    BOOST_CHECK(reference.ready);
    BOOST_CHECK(chain.reads.empty());
    reference = BuildMintReference(1600, &chain.blocks.back(), chain.params, {});
    BOOST_CHECK(!reference.ready);
}

BOOST_AUTO_TEST_CASE(branch_and_candidate_price_are_separate_inputs)
{
    ReferenceChain chain;
    chain.prices[1361] = 10000;
    const auto original = chain.Reference();
    BOOST_CHECK(!EvaluateMintPrice(10000, original).restricted);
    BOOST_CHECK(EvaluateMintPrice(12000, original).restricted);
    chain.prices[1361] = 20000;
    chain.hashes.back() = uint256S("99999");
    const auto replacement = chain.Reference();
    BOOST_CHECK(original.parent_hash != replacement.parent_hash);
    BOOST_CHECK_EQUAL(replacement.price_micro_usd, 20000);
    BOOST_CHECK(EvaluateMintPrice(10000, replacement).restricted);
    BOOST_CHECK_EQUAL(chain.Reference().price_micro_usd, replacement.price_micro_usd);
}

BOOST_AUTO_TEST_CASE(reference_recovers_as_new_bundle_blocks_enter_the_window)
{
    ReferenceChain chain;
    for (int height = 200; height < 215; ++height) chain.prices[height] = 10000;
    for (int height = 261; height < 276; ++height) chain.prices[height] = 12000;
    const auto initial = chain.Reference(500);
    BOOST_REQUIRE(initial.ready);
    BOOST_REQUIRE_EQUAL(initial.sample_count, 15U);
    BOOST_CHECK(EvaluateMintPrice(12000, initial).restricted);

    // Only the candidate height changes. No accepted mint or local price record is needed.
    const auto seven_new_samples = chain.Reference(507);
    BOOST_REQUIRE(seven_new_samples.ready);
    BOOST_CHECK_EQUAL(seven_new_samples.price_micro_usd, 10000);
    BOOST_CHECK(EvaluateMintPrice(12000, seven_new_samples).restricted);
    const auto eight_new_samples = chain.Reference(508);
    BOOST_REQUIRE(eight_new_samples.ready);
    BOOST_CHECK_EQUAL(eight_new_samples.sample_count, 15U);
    BOOST_CHECK_EQUAL(eight_new_samples.price_micro_usd, 12000);
    BOOST_CHECK(!EvaluateMintPrice(12000, eight_new_samples).restricted);
    const auto stable_window = chain.Reference(515);
    BOOST_REQUIRE(stable_window.ready);
    BOOST_CHECK_EQUAL(stable_window.sample_count, 15U);
    BOOST_CHECK_EQUAL(stable_window.price_micro_usd, 12000);
    BOOST_CHECK(!EvaluateMintPrice(12000, stable_window).restricted);
}

struct ThawValidationSetup : BasicTestingSetup {
    CBlockIndex parent;
    uint256 parent_hash{uint256S("abcdef")};

    ThawValidationSetup() : BasicTestingSetup(ChainType::REGTEST, {"-ddthawdayheight=1000"})
    {
        parent.phashBlock = &parent_hash;
        VolatilityMonitor::ClearHistory();
        VolatilityMonitor::TriggerFreeze(true, 999);
    }
    ~ThawValidationSetup() { VolatilityMonitor::ClearHistory(); }

    DigiDollar::ValidationContext Context(int height)
    {
        parent.nHeight = height - 1;
        DigiDollar::ValidationContext ctx(height, 10000, 30000, Params(), nullptr, false, nullptr, nullptr, 0, &parent);
        ctx.mintReference.ready = true;
        ctx.mintReference.candidate_height = height;
        ctx.mintReference.parent_hash = parent_hash;
        ctx.mintReference.genesis_hash = Params().GetConsensus().hashGenesisBlock;
        ctx.mintReference.sample_count = 1;
        ctx.mintReference.price_micro_usd = 10000;
        return ctx;
    }
};

BOOST_FIXTURE_TEST_CASE(candidate_height_removes_legacy_freezes_but_preserves_other_checks, ThawValidationSetup)
{
    CMutableTransaction mint;
    mint.nVersion = 0x01000770;
    mint.vin.emplace_back(COutPoint(uint256S("12345"), 0));
    mint.vout.emplace_back(0, CScript());
    mint.vout.emplace_back(0, CScript());
    auto before = Context(999);
    TxValidationState before_state;
    BOOST_CHECK(!DigiDollar::ValidateMintTransaction(CTransaction(mint), before, before_state));
    BOOST_CHECK_EQUAL(before_state.GetRejectReason(), "minting-frozen-volatility");
    for (int height : {1000, 1001}) {
        auto ctx = Context(height);
        TxValidationState state;
        BOOST_CHECK(!DigiDollar::ValidateMintTransaction(CTransaction(mint), ctx, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "missing-collateral-output");
        ctx.oraclePriceMicroUSD = 12000;
        TxValidationState paused;
        BOOST_CHECK(!DigiDollar::ValidateMintTransaction(CTransaction(mint), ctx, paused));
        BOOST_CHECK_EQUAL(paused.GetRejectReason(), "minting-volatility-pause");
        ctx.mintReference.ready = false;
        TxValidationState unavailable;
        BOOST_CHECK(!DigiDollar::ValidateMintTransaction(CTransaction(mint), ctx, unavailable));
        BOOST_CHECK(unavailable.IsError());
        BOOST_CHECK(!unavailable.IsInvalid());
    }
    auto restored_context = Context(999);
    TxValidationState restored;
    BOOST_CHECK(!DigiDollar::ValidateMintTransaction(CTransaction(mint), restored_context, restored));
    BOOST_CHECK_EQUAL(restored.GetRejectReason(), "minting-frozen-volatility");
}

BOOST_FIXTURE_TEST_CASE(transfers_and_redemptions_leave_legacy_freeze_at_height, ThawValidationSetup)
{
    for (const int type : {2, 3}) {
        CMutableTransaction tx;
        tx.nVersion = (type << 24) | 0x0770;
        tx.vin.emplace_back(COutPoint(uint256S("12345"), 0));
        const CTransaction transaction(tx);
        auto validate = [&](const DigiDollar::ValidationContext& ctx, TxValidationState& state) {
            return type == 2 ? DigiDollar::ValidateTransferTransaction(transaction, ctx, state) :
                               DigiDollar::ValidateRedemptionTransaction(transaction, ctx, state);
        };
        auto before = Context(999);
        TxValidationState frozen;
        BOOST_CHECK(!validate(before, frozen));
        BOOST_CHECK_EQUAL(frozen.GetRejectReason(), "all-operations-frozen");
        auto after = Context(1000);
        TxValidationState ordinary;
        BOOST_CHECK(!validate(after, ordinary));
        BOOST_CHECK(ordinary.IsInvalid());
        BOOST_CHECK(ordinary.GetRejectReason() != "all-operations-frozen");
    }
}

BOOST_AUTO_TEST_CASE(user_reasons_distinguish_volatility_from_local_readiness)
{
    BOOST_CHECK(TranslateMintRejectReasonForUser("minting-volatility-pause").find("20%") != std::string::npos);
    BOOST_CHECK(TranslateMintRejectReasonForUser("DigiDollar volatility state not ready").find("missing blocks") != std::string::npos);
    BOOST_CHECK_EQUAL(TranslateMintRejectReasonForUser("ordinary fee error"), "ordinary fee error");
}

BOOST_AUTO_TEST_SUITE_END()
