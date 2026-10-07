// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <index/digidollarstatsindex.h>
#include <kernel/context.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {

struct RpcTotalsSetup : TestChain100Setup {
    static constexpr CAmount PRINCIPAL{10000};
    static constexpr CAmount COLLATERAL{500 * COIN};
    std::vector<COutPoint> funding;
    CKey owner;
    CTransactionRef first_mint;

    RpcTotalsSetup()
        : TestChain100Setup(ChainType::REGTEST,
            {"-digidollaractivationheight=100", "-ddthawdayheight=100", "-digidollarstatsindex=0"})
    {
        BOOST_REQUIRE(!g_digidollar_stats_index);
        OracleBundleManager::GetInstance().Clear();
        MockOracleManager::GetInstance().Reset();
        std::vector<CMutableTransaction> transactions;
        for (int i = 0; i < 3; ++i) {
            transactions.push_back(CreateValidMempoolTransaction(m_coinbase_txns[i], 0, i + 1,
                coinbaseKey, CScript() << OP_TRUE, COLLATERAL + COIN, false));
        }
        const CBlock block = CreateAndProcessBlock(transactions, CScript() << OP_TRUE);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()), block.GetHash());
        for (size_t i = 1; i < block.vtx.size(); ++i) funding.emplace_back(block.vtx[i]->GetHash(), 0);
        BOOST_REQUIRE_EQUAL(funding.size(), 3U);
        owner.MakeNewKey(true);
        first_mint = Mint(0).vtx.at(1);
    }

    ~RpcTotalsSetup()
    {
        m_node.kernel->interrupt.reset();
        OracleBundleManager::GetInstance().Clear();
        MockOracleManager::GetInstance().Reset();
    }

    CBlock Mint(size_t input)
    {
        const int height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height() + 1);
        auto& mock = MockOracleManager::GetInstance();
        mock.SetMockPrice(1000000);
        auto bundle = mock.CreateMockMuSig2Bundle(height);
        auto& manager = OracleBundleManager::GetInstance();
        manager.SetEnabled(true);
        std::string error;
        BOOST_REQUIRE_MESSAGE(OracleBundleManager::ValidateMuSig2Bundle(bundle, height, Params().GetConsensus(), error), error);
        BOOST_REQUIRE(manager.UpdateBundle(bundle));

        const XOnlyPubKey key{owner.GetPubKey()};
        DigiDollar::MintParams params;
        params.ddAmount = PRINCIPAL;
        params.lockHeight = height + DigiDollar::LockDaysToBlocks(30);
        params.ownerKey = key;
        params.internalKey = DigiDollar::GetCollateralNUMSKey();
        CMutableTransaction tx;
        tx.SetDigiDollarType(DD_TX_MINT);
        tx.vin.emplace_back(funding.at(input));
        tx.vout.emplace_back(COLLATERAL, DigiDollar::CreateCollateralP2TR(params));
        tx.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(key, PRINCIPAL));
        tx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
            << CScriptNum(1) << CScriptNum(PRINCIPAL) << CScriptNum(params.lockHeight) << CScriptNum(1)
            << std::vector<unsigned char>(key.begin(), key.end()));
        const CBlock block = CreateAndProcessBlock({tx}, CScript() << OP_TRUE);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()), block.GetHash());
        return block;
    }

    UniValue Call(const std::string& method, const std::string& argument = {})
    {
        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = method;
        request.params = UniValue(UniValue::VARR);
        if (!argument.empty()) request.params.push_back(argument);
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        return tableRPC.execute(request);
    }

    void CheckStats(int vaults, CAmount supply = -1)
    {
        const auto before = WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState());
        BOOST_REQUIRE(before);
        const auto stats = Call("getdigidollarstats");
        BOOST_CHECK_EQUAL(stats["total_dd_supply"].getInt<int64_t>(), supply < 0 ? vaults * PRINCIPAL : supply);
        BOOST_CHECK_EQUAL(stats["active_positions"].getInt<int>(), vaults);
        BOOST_CHECK_EQUAL(stats["total_collateral_dgb"].get_real(), vaults * 500);
        const auto& canonical = stats["canonical_health"];
        BOOST_CHECK(canonical["ready"].get_bool());
        BOOST_CHECK(canonical["history_checked"].get_bool());
        BOOST_CHECK_EQUAL(canonical["block_hash"].get_str(), before->best_block.GetHex());
        BOOST_CHECK_EQUAL(canonical["open_vault_principal"].getInt<int64_t>(), vaults * PRINCIPAL);
        BOOST_CHECK_EQUAL(canonical["collateral"].getInt<int64_t>(), vaults * COLLATERAL);
        BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState()) == before);
    }
};

bool IsReadinessError(const UniValue& error)
{
    return error["message"].get_str().find("canonical health state not ready") != std::string::npos;
}

bool IsCancellationError(const UniValue& error)
{
    return error["message"].get_str().find("reconstruction cancelled") != std::string::npos;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_rpc_totals_cache_tests, RpcTotalsSetup)

BOOST_AUTO_TEST_CASE(repeated_stats_follow_new_tips_and_reorgs)
{
    int reconstructions{0};
    DebugLogHelper log{"DigiDollar stats: reconstructed circulating supply at ", [&](const std::string* line) {
        if (line) ++reconstructions;
        return false;
    }};
    for (int i = 0; i < 3; ++i) CheckStats(1);
    BOOST_CHECK_EQUAL(reconstructions, 1);

    const CBlock second = Mint(1);
    for (int i = 0; i < 3; ++i) CheckStats(2);
    BOOST_CHECK_EQUAL(reconstructions, 2);

    Call("invalidateblock", second.GetHash().GetHex());
    for (int i = 0; i < 3; ++i) CheckStats(1);
    BOOST_CHECK_EQUAL(reconstructions, 3);

    Call("reconsiderblock", second.GetHash().GetHex());
    for (int i = 0; i < 3; ++i) CheckStats(2);
    BOOST_CHECK_EQUAL(reconstructions, 4);

    // A normal token burn changes circulation without closing either vault.
    CMutableTransaction burn;
    burn.vin.emplace_back(COutPoint{first_mint->GetHash(), 1});
    burn.vin.emplace_back(funding.at(2));
    burn.vout.emplace_back(COLLATERAL, CScript() << OP_TRUE);
    PrecomputedTransactionData data;
    data.Init(burn, std::vector<CTxOut>{first_mint->vout[1], CTxOut{COLLATERAL + COIN, CScript() << OP_TRUE}}, true);
    ScriptExecutionData execution;
    execution.m_annex_init = true;
    execution.m_annex_present = false;
    uint256 hash;
    BOOST_REQUIRE(SignatureHashSchnorr(hash, execution, burn, 0, SIGHASH_DEFAULT, SigVersion::TAPROOT,
                                      data, MissingDataBehavior::ASSERT_FAIL));
    std::vector<unsigned char> signature(64);
    const uint256 no_script_tree;
    BOOST_REQUIRE(owner.SignSchnorr(hash, signature, &no_script_tree, uint256{}));
    burn.vin[0].scriptWitness.stack = {signature};
    const CBlock burned = CreateAndProcessBlock({burn}, CScript() << OP_TRUE);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()), burned.GetHash());
    CheckStats(2, PRINCIPAL);
    CheckStats(2, PRINCIPAL);
    BOOST_CHECK_EQUAL(reconstructions, 5);
    Call("invalidateblock", burned.GetHash().GetHex());
    CheckStats(2);
    BOOST_CHECK_EQUAL(reconstructions, 6);
}

BOOST_AUTO_TEST_CASE(cached_totals_require_current_canonical_readiness)
{
    int reconstructions{0};
    DebugLogHelper log{"DigiDollar stats: reconstructed circulating supply at ", [&](const std::string* line) {
        if (line) ++reconstructions;
        return false;
    }};
    CheckStats(1);
    BOOST_REQUIRE_EQUAL(reconstructions, 1);
    {
        LOCK(cs_main);
        auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
        const auto saved = coins.GetDigiDollarState();
        BOOST_REQUIRE(saved);
        auto unchecked = *saved;
        unchecked.history_checked = false;
        coins.SetDigiDollarState(unchecked);
        BOOST_CHECK_EXCEPTION(Call("getdigidollarstats"), UniValue, IsReadinessError);
        coins.SetDigiDollarState(saved);
    }
    BOOST_CHECK_EQUAL(reconstructions, 1);
    CheckStats(1);
    CheckStats(1);
    BOOST_CHECK_EQUAL(reconstructions, 2);
}

BOOST_AUTO_TEST_CASE(cancelled_reconstruction_can_be_retried)
{
    int reconstructions{0};
    DebugLogHelper log{"DigiDollar stats: reconstructed circulating supply at ", [&](const std::string* line) {
        if (line) ++reconstructions;
        return false;
    }};
    m_node.kernel->interrupt();
    BOOST_CHECK_EXCEPTION(Call("getdigidollarstats"), UniValue, IsCancellationError);
    BOOST_CHECK_EQUAL(reconstructions, 0);
    m_node.kernel->interrupt.reset();
    CheckStats(1);
    CheckStats(1);
    BOOST_CHECK_EQUAL(reconstructions, 1);
}

BOOST_AUTO_TEST_SUITE_END()
