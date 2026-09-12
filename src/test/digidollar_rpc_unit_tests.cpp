// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <consensus/err.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <oracle/mock_oracle.h>
#include <rpc/client.h>
#include <rpc/digidollar.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <tinyformat.h>
#include <vector>

BOOST_AUTO_TEST_SUITE(digidollar_rpc_unit_tests)

namespace {

struct DigiDollarRPCUnitSetup : public TestingSetup {
    DigiDollarRPCUnitSetup() : TestingSetup(ChainType::REGTEST)
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
    }

    ~DigiDollarRPCUnitSetup()
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
    }

    UniValue CallNodeRPC(const std::string& args)
    {
        std::vector<std::string> v_args{SplitString(args, ' ')};
        const std::string method = v_args.front();
        v_args.erase(v_args.begin());

        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = method;
        request.params = RPCConvertValues(method, v_args);
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        return tableRPC.execute(request);
    }

    std::shared_ptr<wallet::CWallet> CreateWalletWithPositions()
    {
        auto wallet = std::make_shared<wallet::CWallet>(
            m_node.chain.get(), "wave1-rpc-wallet", wallet::CreateMockableWalletDatabase());
        wallet->LoadWallet();
        wallet->EnsureDDWallet();
        WITH_LOCK(wallet->cs_wallet, wallet->SetLastBlockProcessed(1000, uint256::ONE));

        DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
        BOOST_REQUIRE(dd_wallet != nullptr);
        dd_wallet->AddCollateralPosition(WalletCollateralPosition(
            uint256S("00000000000000000000000000000000000000000000000000000000dd100001"),
            50, COIN, 1, 1100));
        dd_wallet->AddCollateralPosition(WalletCollateralPosition(
            uint256S("00000000000000000000000000000000000000000000000000000000dd100002"),
            150, 2 * COIN, 1, 1100));
        dd_wallet->AddCollateralPosition(WalletCollateralPosition(
            uint256S("00000000000000000000000000000000000000000000000000000000dd100003"),
            10000, 300 * COIN, 9, 2000));

        return wallet;
    }
};

int LockDaysForTier(int tier)
{
    switch (tier) {
    case 5: return 730;
    case 6: return 1095;
    case 7: return 1825;
    case 8: return 2555;
    default: return 3650;
    }
}

int ConsensusRatioForTier(int tier)
{
    return DigiDollar::GetCollateralRatioForLockTime(
        DigiDollar::LockDaysToBlocks(LockDaysForTier(tier)), Params().GetDigiDollarParams());
}

} // namespace

BOOST_FIXTURE_TEST_CASE(wave1_list_positions_min_amount_filters_dd_cents_not_dgb_sats, DigiDollarRPCUnitSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    std::shared_ptr<wallet::CWallet> wallet = CreateWalletWithPositions();
    wallet::AddWallet(context, wallet);

    // "1.00" means one dollar, which is 100 DigiDollar cents. It must never
    // be read as a DGB amount in satoshis. The unit has to be said out loud:
    // an amount with a decimal point and no amount_unit is refused.
    JSONRPCRequest ambiguous;
    ambiguous.context = &context;
    ambiguous.strMethod = "listdigidollarpositions";
    ambiguous.params = UniValue(UniValue::VARR);
    ambiguous.params.push_back(true);
    ambiguous.params.push_back(-1);
    ambiguous.params.push_back("1.00");
    BOOST_CHECK_THROW(listdigidollarpositions().HandleRequest(ambiguous), UniValue);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "listdigidollarpositions";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(true);
    request.params.push_back(-1);
    request.params.push_back("1.00");
    request.params.push_back(UniValue(UniValue::VNULL)); // count
    request.params.push_back(UniValue(UniValue::VNULL)); // skip
    request.params.push_back("dollars");

    UniValue result = listdigidollarpositions().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isArray());
    BOOST_CHECK_EQUAL(result.size(), 2U);

    std::vector<CAmount> amounts;
    for (const UniValue& position : result.getValues()) {
        amounts.push_back(position["dd_minted"].getInt<int64_t>());
    }
    BOOST_CHECK(std::find(amounts.begin(), amounts.end(), 150) != amounts.end());
    BOOST_CHECK(std::find(amounts.begin(), amounts.end(), 10000) != amounts.end());
    BOOST_CHECK(std::find(amounts.begin(), amounts.end(), 50) == amounts.end());
}

BOOST_FIXTURE_TEST_CASE(wave1_estimatecollateral_matches_consensus_tiers_5_to_8, DigiDollarRPCUnitSetup)
{
    for (int tier = 5; tier <= 8; ++tier) {
        UniValue result = CallNodeRPC(strprintf("estimatecollateral 10000 %d 500000", tier));
        BOOST_REQUIRE(result.isObject());

        const int expected_ratio = ConsensusRatioForTier(tier);
        BOOST_CHECK_EQUAL(result["lock_tier"].getInt<int>(), tier);
        BOOST_CHECK_EQUAL(result["base_ratio"].getInt<int>(), expected_ratio);
        BOOST_CHECK_EQUAL(result["effective_ratio"].getInt<int>(), expected_ratio);
    }
}

BOOST_FIXTURE_TEST_CASE(wave3_estimatecollateral_rounds_fractional_dca_up, DigiDollarRPCUnitSetup)
{
    DigiDollar::SystemMetrics metrics;
    metrics.totalDDSupply = 10000;
    metrics.totalCollateral = 149 * COIN;
    metrics.systemHealth = 149;
    metrics.hasCanonicalHealth = true;
    DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);

    UniValue result = CallNodeRPC("estimatecollateral 10000 5 1000000");
    BOOST_REQUIRE(result.isObject());

    BOOST_CHECK_EQUAL(result["system_health"].getInt<int>(), 149);
    BOOST_CHECK_EQUAL(result["base_ratio"].getInt<int>(), 275);
    BOOST_CHECK_EQUAL(result["effective_ratio"].getInt<int>(), 344);
}

BOOST_FIXTURE_TEST_CASE(wave6_rpc_quotes_wallet_collateral_safety_margin, DigiDollarRPCUnitSetup)
{
    DigiDollar::SystemMetrics metrics;
    metrics.totalDDSupply = 10000;
    metrics.totalCollateral = 200 * COIN;
    metrics.systemHealth = 200;
    metrics.lastOraclePrice = 1'000'000;
    metrics.hasCanonicalHealth = true;
    DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);

    UniValue estimate = CallNodeRPC("estimatecollateral 10000 5 1000000");
    BOOST_REQUIRE(estimate.isObject());
    BOOST_REQUIRE(estimate.exists("minimum_required_dgb"));
    BOOST_REQUIRE(estimate.exists("wallet_collateral_dgb"));
    BOOST_REQUIRE(estimate.exists("collateral_safety_margin_dgb"));
    BOOST_CHECK_EQUAL(estimate["required_dgb"].getValStr(), "275.00000000");
    BOOST_CHECK_EQUAL(estimate["minimum_required_dgb"].getValStr(), "275.00000000");
    BOOST_CHECK_EQUAL(estimate["wallet_collateral_dgb"].getValStr(), "277.75000000");
    BOOST_CHECK_EQUAL(estimate["collateral_safety_margin_dgb"].getValStr(), "2.75000000");

    UniValue requirement = CallNodeRPC("calculatecollateralrequirement 10000 730 1000000");
    BOOST_REQUIRE(requirement.isObject());
    BOOST_REQUIRE(requirement.exists("minimum_required_dgb"));
    BOOST_REQUIRE(requirement.exists("wallet_collateral_dgb"));
    BOOST_REQUIRE(requirement.exists("collateral_safety_margin_dgb"));
    BOOST_CHECK_EQUAL(requirement["required_dgb"].getValStr(), "275.00000000");
    BOOST_CHECK_EQUAL(requirement["minimum_required_dgb"].getValStr(), "275.00000000");
    BOOST_CHECK_EQUAL(requirement["wallet_collateral_dgb"].getValStr(), "277.75000000");
    BOOST_CHECK_EQUAL(requirement["collateral_safety_margin_dgb"].getValStr(), "2.75000000");
}

BOOST_FIXTURE_TEST_CASE(wave5_getredemptioninfo_reports_full_collateral_return, DigiDollarRPCUnitSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    std::shared_ptr<wallet::CWallet> wallet = CreateWalletWithPositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getredemptioninfo";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back("00000000000000000000000000000000000000000000000000000000dd100001");

    UniValue result = getredemptioninfo().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isObject());
    BOOST_CHECK_EQUAL(result["dgb_return"].getValStr(), "1.00000000");
}

// Wave 6: mintdigidollar's ERR pre-check must use the same canonical
// network-wide health source the consensus path uses. The earlier
// implementation summed only the local wallet's positions, so a wallet with
// zero DD positions silently skipped the ERR gate even when the network was
// already in emergency state — the user only learned that their mint was
// invalid after broadcast, when mempool rejected it with
// "minting-blocked-during-err" using canonical metrics.
BOOST_FIXTURE_TEST_CASE(wave6_mintdigidollar_blocks_when_network_err_active, DigiDollarRPCUnitSetup)
{
    // Simulate canonical metrics indicating the network is in ERR
    // (system health = 80% < 100% with active DD supply). This is what
    // ConnectBlock would observe via SystemHealthMonitor::GetCachedMetrics()
    // and what ShouldBlockMintingDuringERR() will use during mempool
    // acceptance and block validation.
    DigiDollar::SystemMetrics metrics;
    metrics.totalDDSupply = 1'000'000;          // 10,000.00 DD outstanding
    metrics.totalCollateral = 100 * COIN;        // small collateral relative to DD
    metrics.systemHealth = 80;                   // 80% — below 100% triggers ERR
    metrics.lastOraclePrice = 800'000;           // micro-USD per DGB; consistent with above
    metrics.hasCanonicalHealth = true;
    DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);

    // Provide a fresh oracle price so the RPC's "no oracle price" guard
    // does not fire before it reaches the ERR check.
    MockOracleManager& mock = MockOracleManager::GetInstance();
    const bool mock_was_enabled = mock.IsEnabled();
    const CAmount prior_mock_price = mock.GetCurrentPrice();
    mock.SetEnabled(true);
    mock.SetMockPrice(800'000); // 800,000 micro-USD = $0.80/DGB

    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    // Empty wallet: zero DD positions. Under the buggy wallet-only check
    // this would pass through the ERR gate and try to broadcast.
    auto wallet = std::make_shared<wallet::CWallet>(
        m_node.chain.get(), "wave6-rpc-mint-wallet", wallet::CreateMockableWalletDatabase());
    wallet->LoadWallet();
    wallet->EnsureDDWallet();
    WITH_LOCK(wallet->cs_wallet, wallet->SetLastBlockProcessed(1000, uint256::ONE));
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "mintdigidollar";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(static_cast<int64_t>(10000)); // 100.00 DD
    request.params.push_back(0);                            // 1-hour test tier

    bool threw_err = false;
    std::string error_message;
    try {
        mintdigidollar().HandleRequest(request);
    } catch (const UniValue& objError) {
        if (objError.exists("message")) {
            error_message = objError["message"].get_str();
        }
        threw_err = error_message.find("emergency state") != std::string::npos ||
                    error_message.find("ERR") != std::string::npos ||
                    error_message.find("minting-blocked-during-err") != std::string::npos;
    } catch (const std::exception& e) {
        error_message = e.what();
    }

    wallet::RemoveWallet(context, wallet, std::nullopt);
    mock.SetMockPrice(prior_mock_price);
    mock.SetEnabled(mock_was_enabled);

    BOOST_CHECK_MESSAGE(threw_err,
        strprintf("mintdigidollar must reject ERR-state mints based on canonical network "
                  "metrics, even when the local wallet has no DD positions. Got: %s",
                  error_message));
}

// Wave 7: calculatecollateralrequirement must accept every canonical lock tier
// the consensus collateral-ratio map exposes — including the 1-hour testing
// tier (lockDays = 0, which LockDaysToBlocks(0) maps to 240 blocks) and the
// 2-year tier (lockDays = 730). The earlier RPC rejected lockDays <= 0
// outright, so the user could never query the 1-hour tier; it also listed
// only 8 of the 10 canonical lock periods in its error message, omitting the
// 2-year tier and the 1-hour testing tier.
BOOST_FIXTURE_TEST_CASE(wave7_calculatecollateralrequirement_accepts_all_canonical_tiers, DigiDollarRPCUnitSetup)
{
    struct CanonicalTierExpectation {
        int lockDays;
        int expectedRatio;
    };
    const std::vector<CanonicalTierExpectation> tiers{
        {0,    1000}, // 1 hour testing tier — lockDays = 0 maps to 240 blocks
        {30,    500},
        {90,    400},
        {180,   350},
        {365,   300},
        {730,   275}, // 2-year tier
        {1095,  250},
        {1825,  225},
        {2555,  212},
        {3650,  200},
    };

    for (const auto& tier : tiers) {
        UniValue result = CallNodeRPC(strprintf(
            "calculatecollateralrequirement 10000 %d 1000000", tier.lockDays));
        BOOST_REQUIRE_MESSAGE(result.isObject(),
            strprintf("calculatecollateralrequirement should return an object for lockDays=%d", tier.lockDays));
        BOOST_CHECK_MESSAGE(result["lock_days"].getInt<int>() == tier.lockDays,
            strprintf("lock_days mismatch: expected %d, got %d", tier.lockDays, result["lock_days"].getInt<int>()));
        BOOST_CHECK_MESSAGE(result["base_ratio"].getInt<int>() == tier.expectedRatio,
            strprintf("base_ratio mismatch for lockDays=%d: expected %d, got %d",
                      tier.lockDays, tier.expectedRatio, result["base_ratio"].getInt<int>()));
    }
}

// Wave 7: calculatecollateralrequirement must reject non-canonical lock days
// with an error message that names every canonical lock period the consensus
// rule set actually accepts. A drift between the error message and the
// consensus map causes user-visible confusion (e.g., the 2-year tier was
// missing from the listed valid periods even though consensus accepts it).
BOOST_FIXTURE_TEST_CASE(wave7_calculatecollateralrequirement_rejects_noncanonical_lock_days, DigiDollarRPCUnitSetup)
{
    bool threw = false;
    std::string error_message;
    try {
        CallNodeRPC("calculatecollateralrequirement 10000 45 1000000");
    } catch (const UniValue& objError) {
        threw = true;
        if (objError.exists("message")) {
            error_message = objError["message"].get_str();
        }
    } catch (const std::exception& e) {
        threw = true;
        error_message = e.what();
    }
    BOOST_CHECK_MESSAGE(threw, "calculatecollateralrequirement must reject non-canonical lockDays=45");

    // The error message must list every canonical lock-days value the
    // consensus collateral map exposes, so RPC users do not see drift.
    const std::vector<std::string> required_periods{
        "30", "90", "180", "365", "730", "1095", "1825", "2555", "3650"};
    for (const std::string& period : required_periods) {
        BOOST_CHECK_MESSAGE(error_message.find(period) != std::string::npos,
            strprintf("Error message must mention canonical period %s, got: %s",
                      period, error_message));
    }
}

BOOST_AUTO_TEST_SUITE_END()
