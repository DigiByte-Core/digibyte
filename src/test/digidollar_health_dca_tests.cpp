// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <consensus/err.h>
#include <consensus/validation.h>
#include <consensus/volatility.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <key.h>
#include <rpc/client.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <utility>
#include <vector>

BOOST_AUTO_TEST_SUITE(digidollar_health_dca_tests)

namespace {

static constexpr int WAVE1_HEIGHT{1000};
static constexpr CAmount WAVE1_ORACLE_PRICE_MICRO_USD{500000};
static constexpr CAmount WAVE1_DD_AMOUNT{10000};
static constexpr int WAVE1_TEN_YEAR_DAYS{3650};
static constexpr uint32_t WAVE1_TEN_YEAR_TIER{9};

int ConsensusRatioForLockDays(int lock_days)
{
    return DigiDollar::GetCollateralRatioForLockTime(
        DigiDollar::LockDaysToBlocks(lock_days), Params().GetDigiDollarParams());
}

struct DigiDollarHealthDCASetup : public TestingSetup {
    DigiDollarHealthDCASetup() : TestingSetup(ChainType::REGTEST)
    {
        DigiDollar::SystemHealthMonitor::Initialize();
        ResetSharedState();
    }

    ~DigiDollarHealthDCASetup()
    {
        ResetSharedState();
        DigiDollar::SystemHealthMonitor::Shutdown();
    }

    void ResetSharedState()
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        DigiDollar::ERR::EmergencyRedemptionRatio::ResetForTesting();
    }

    DigiDollar::SystemMetrics SeedCachedHealth120()
    {
        ResetSharedState();
        DigiDollar::Volatility::VolatilityMonitor::RecordPrice(
            WAVE1_ORACLE_PRICE_MICRO_USD, 1'700'000'000, WAVE1_HEIGHT);
        DigiDollar::SystemHealthMonitor::OnMintConnected(WAVE1_DD_AMOUNT, 240 * COIN);
        DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();
        BOOST_REQUIRE_EQUAL(metrics.systemHealth, 120);
        return metrics;
    }

    DigiDollar::TxBuilderResult BuildBaseCollateralMint()
    {
        CKey owner_key;
        owner_key.MakeNewKey(true);
        const XOnlyPubKey owner_xonly(owner_key.GetPubKey());

        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(WAVE1_TEN_YEAR_DAYS);
        const int64_t lock_height = WAVE1_HEIGHT + lock_blocks;
        const int base_ratio = ConsensusRatioForLockDays(WAVE1_TEN_YEAR_DAYS);
        const util::int128_t numerator = static_cast<util::int128_t>(WAVE1_DD_AMOUNT) * COIN *
                                   base_ratio * 100;
        const CAmount base_collateral = static_cast<CAmount>(
            (numerator + WAVE1_ORACLE_PRICE_MICRO_USD - 1) / WAVE1_ORACLE_PRICE_MICRO_USD);

        DigiDollar::MintParams mint_params;
        mint_params.ddAmount = WAVE1_DD_AMOUNT;
        mint_params.lockHeight = lock_height;
        mint_params.ownerKey = owner_xonly;
        mint_params.internalKey = DigiDollar::GetCollateralNUMSKey();
        mint_params.oracleKeys = DigiDollar::GetOracleKeys(15);

        CMutableTransaction tx;
        tx.nVersion = 0x01000770;
        tx.vin.emplace_back(COutPoint(uint256::ONE, 0));
        tx.vout.emplace_back(0, CScript() << OP_RETURN
                                          << std::vector<unsigned char>{'D', 'D'}
                                          << CScriptNum(1)
                                          << CScriptNum(WAVE1_DD_AMOUNT)
                                          << CScriptNum(lock_height)
                                          << CScriptNum(WAVE1_TEN_YEAR_TIER)
                                          << std::vector<unsigned char>(owner_xonly.begin(), owner_xonly.end()));
        tx.vout.emplace_back(base_collateral, DigiDollar::CreateCollateralP2TR(mint_params));
        tx.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(owner_xonly, WAVE1_DD_AMOUNT));

        DigiDollar::TxBuilderResult result;
        result.success = true;
        result.tx = std::move(tx);
        result.collateralRequired = base_collateral;
        return result;
    }

    UniValue CallRPC(const std::string& args)
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
};

} // namespace

BOOST_FIXTURE_TEST_CASE(wave1_stale_cached_health_cannot_allow_base_collateral_mint, DigiDollarHealthDCASetup)
{
    const DigiDollar::SystemMetrics current_metrics = SeedCachedHealth120();

    DigiDollar::TxBuilderResult build = BuildBaseCollateralMint();
    BOOST_REQUIRE_MESSAGE(build.success, build.error);

    DigiDollar::ValidationContext stale_healthy_ctx(
        WAVE1_HEIGHT, WAVE1_ORACLE_PRICE_MICRO_USD, 30000, Params());
    TxValidationState state;
    const bool accepted = DigiDollar::ValidateMintTransaction(CTransaction(build.tx), stale_healthy_ctx, state);

    BOOST_CHECK_MESSAGE(!accepted,
        "base-collateral mint accepted using stale healthy context while current cached health is "
        << current_metrics.systemHealth << "%; reject_reason=" << state.GetRejectReason());
    if (!accepted) {
        const std::string reason = state.GetRejectReason();
        BOOST_CHECK_MESSAGE(reason == "insufficient-collateral" || reason == "bad-collateral-ratio",
            "expected health-source collateral rejection, observed reason=" << reason);
    }
}

BOOST_FIXTURE_TEST_CASE(wave1_missing_post_activation_health_rejects_not_default_healthy, DigiDollarHealthDCASetup)
{
    ResetSharedState();
    DigiDollar::SystemHealthMonitor::OnMintConnected(WAVE1_DD_AMOUNT, 240 * COIN);

    const int health = DigiDollar::DCA::DynamicCollateralAdjustment::GetCurrentSystemHealth();
    BOOST_CHECK_MESSAGE(health < 0,
        "missing oracle/health data should fail closed after DD supply exists, observed health="
        << health);
}

BOOST_FIXTURE_TEST_CASE(wave1_dca_err_rpc_quote_and_txbuilder_share_health_source, DigiDollarHealthDCASetup)
{
    const DigiDollar::SystemMetrics metrics = SeedCachedHealth120();
    const int dca_health = DigiDollar::DCA::DynamicCollateralAdjustment::GetCurrentSystemHealth();
    BOOST_REQUIRE_EQUAL(dca_health, metrics.systemHealth);

    const int base_ratio = ConsensusRatioForLockDays(WAVE1_TEN_YEAR_DAYS);
    const int expected_ratio = DigiDollar::DCA::DynamicCollateralAdjustment::ApplyDCA(base_ratio, dca_health);

    BOOST_CHECK(!DigiDollar::ERR::EmergencyRedemptionRatio::ShouldBlockMinting(WAVE1_ORACLE_PRICE_MICRO_USD));

    UniValue rpc_quote = CallRPC("calculatecollateralrequirement 10000 3650 500000");
    BOOST_REQUIRE(rpc_quote.isObject());
    BOOST_CHECK_EQUAL(rpc_quote["system_health"].getInt<int>(), dca_health);
    BOOST_CHECK_EQUAL(rpc_quote["effective_ratio"].getInt<int>(), expected_ratio);

    DigiDollar::ValidationContext current_ctx(WAVE1_HEIGHT, WAVE1_ORACLE_PRICE_MICRO_USD, dca_health, Params());
    const CAmount consensus_required = DigiDollar::CalculateRequiredCollateral(
        WAVE1_DD_AMOUNT, DigiDollar::LockDaysToBlocks(WAVE1_TEN_YEAR_DAYS), current_ctx);

    DigiDollar::MintTxBuilder builder(Params(), WAVE1_HEIGHT, WAVE1_ORACLE_PRICE_MICRO_USD);
    const CAmount txbuilder_required = builder.CalculateRequiredCollateral(WAVE1_DD_AMOUNT, WAVE1_TEN_YEAR_DAYS);

    BOOST_CHECK_MESSAGE(txbuilder_required >= consensus_required,
        "txbuilder collateral quote used a different health source: consensus_required="
        << consensus_required << " txbuilder_required=" << txbuilder_required
        << " health=" << dca_health);
}

BOOST_AUTO_TEST_SUITE_END()
