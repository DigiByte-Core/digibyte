// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/merkle.h>
#include <consensus/volatility.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <primitives/block.h>
#include <script/interpreter.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <memory>
#include <tuple>

namespace {
using CoinsSnapshot = std::map<COutPoint, std::tuple<CAmount, CScript, uint32_t, bool>>;

CoinsSnapshot Snapshot(const CCoinsView& view)
{
    CoinsSnapshot result;
    auto cursor = view.Cursor();
    BOOST_REQUIRE(cursor);
    for (; cursor->Valid(); cursor->Next()) {
        COutPoint out;
        Coin coin;
        BOOST_REQUIRE(cursor->GetKey(out));
        BOOST_REQUIRE(cursor->GetValue(coin));
        result.emplace(out, std::make_tuple(coin.out.nValue, coin.out.scriptPubKey, uint32_t{coin.nHeight}, coin.IsCoinBase()));
    }
    cursor->CheckStatus();
    return result;
}

struct Candidate {
    CBlock block;
    uint256 hash;
    CBlockIndex index;
    Candidate(CBlock value, CBlockIndex* parent)
        : block(std::move(value)), hash(block.GetHash()), index(block)
    {
        index.pprev = parent;
        index.nHeight = parent->nHeight + 1;
        index.phashBlock = &hash;
    }
};

class ScopedTokenMetadata {
    CScript m_script;
    DigiDollar::ScriptMetadata m_previous;

public:
    ScopedTokenMetadata(const CScript& script, DigiDollar::ScriptType type, CAmount amount)
        : m_script{script}
    {
        BOOST_REQUIRE(DigiDollar::GetScriptMetadata(script, m_previous));
        DigiDollar::RegisterScriptMetadata(script, type, amount, 0);
    }
    ~ScopedTokenMetadata()
    {
        DigiDollar::RegisterScriptMetadata(m_script, m_previous.type, m_previous.ddAmount, m_previous.lockHeight);
    }
};

struct ChainstateValidationSetup : TestChain100Setup {
    static constexpr CAmount PRINCIPAL{10000};
    static constexpr CAmount FUNDING{6000 * COIN};
    static constexpr CAmount PRICE{220000};
    CKey owner;
    std::vector<COutPoint> funding;

    explicit ChainstateValidationSetup(const char* thaw_height_arg = "-ddthawdayheight=120")
        : TestChain100Setup(ChainType::REGTEST, {"-digidollaractivationheight=100", thaw_height_arg})
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        owner.MakeNewKey(true);
        auto& manager = OracleBundleManager::GetInstance();
        manager.Clear();
        manager.SetEnabled(false);
        std::vector<CMutableTransaction> outputs;
        for (int i = 0; i < 4; ++i) {
            outputs.push_back(CreateValidMempoolTransaction(m_coinbase_txns[i], 0, i + 1,
                coinbaseKey, CScript{} << OP_TRUE, FUNDING, false));
        }
        const CBlock funded = CreateAndProcessBlock(outputs, CScript{} << OP_TRUE);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()), funded.GetHash());
        for (size_t i = 1; i < funded.vtx.size(); ++i) funding.emplace_back(funded.vtx[i]->GetHash(), 0);
        BOOST_REQUIRE_EQUAL(funding.size(), 4U);

        InstallQuote(1000000);
        const CBlock seeded = CreateAndProcessBlock({Mint(0, 500 * COIN, 102)}, CScript{} << OP_TRUE);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()), seeded.GetHash());
        manager.Clear();
        manager.SetEnabled(false);
        mineBlocks(119 - WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()));
        InstallQuote(PRICE);
        LOCK(cs_main);
        auto& chain = m_node.chainman->ActiveChainstate();
        if (DigiDollar::IsThawDayActive(Params().GetConsensus(), chain.m_chain.Height() + 1)) {
            std::string error;
            BOOST_REQUIRE_MESSAGE(chain.InitializeDigiDollarState({}, error), error);
            const auto state = chain.CoinsTip().GetDigiDollarState();
            BOOST_REQUIRE(state);
            BOOST_REQUIRE_EQUAL(state->open_vault_principal, PRINCIPAL);
            BOOST_REQUIRE_EQUAL(state->collateral, 500 * COIN);
            BOOST_REQUIRE_EQUAL(*DigiDollar::CalculateChainstateHealth(*state, PRICE), 110);
        }
        BOOST_REQUIRE(chain.CoinsTip().Flush());
    }

    ~ChainstateValidationSetup()
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        OracleBundleManager::GetInstance().Clear();
        MockOracleManager::GetInstance().Reset();
    }

    void InstallQuote(CAmount price)
    {
        auto& mock = MockOracleManager::GetInstance();
        mock.SetMockPrice(price);
        const int height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height() + 1);
        auto bundle = mock.CreateMockMuSig2Bundle(height);
        std::string error;
        BOOST_REQUIRE_MESSAGE(OracleBundleManager::ValidateMuSig2Bundle(bundle, height, Params().GetConsensus(), error), error);
        auto& manager = OracleBundleManager::GetInstance();
        manager.SetEnabled(true);
        BOOST_REQUIRE(manager.UpdateBundle(bundle));
    }

    CMutableTransaction Mint(size_t input, CAmount collateral, int height) const
    {
        const XOnlyPubKey key{owner.GetPubKey()};
        DigiDollar::MintParams params;
        params.ddAmount = PRINCIPAL;
        params.lockHeight = height + DigiDollar::LockDaysToBlocks(30);
        params.ownerKey = key;
        params.internalKey = DigiDollar::GetCollateralNUMSKey();
        params.oracleKeys = DigiDollar::GetOracleKeys(15);
        CMutableTransaction tx;
        tx.SetDigiDollarType(DD_TX_MINT);
        tx.vin.emplace_back(funding.at(input));
        tx.vout.emplace_back(collateral, DigiDollar::CreateCollateralP2TR(params));
        tx.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(key, PRINCIPAL));
        tx.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
            << CScriptNum(1) << CScriptNum(PRINCIPAL) << CScriptNum(params.lockHeight) << CScriptNum(1)
            << std::vector<unsigned char>(key.begin(), key.end()));
        return tx;
    }

    CMutableTransaction TransferMintToken(const CMutableTransaction& mint, CAmount amount) const
    {
        CKey recipient;
        recipient.MakeNewKey(true);
        CMutableTransaction transfer;
        transfer.SetDigiDollarType(DD_TX_TRANSFER);
        transfer.vin.emplace_back(COutPoint{mint.GetHash(), 1});
        transfer.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(XOnlyPubKey{recipient.GetPubKey()}, amount));
        transfer.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                              << CScriptNum(2) << CScriptNum(amount));
        PrecomputedTransactionData data;
        data.Init(transfer, std::vector<CTxOut>{mint.vout[1]}, true);
        ScriptExecutionData execution;
        execution.m_annex_init = true;
        execution.m_annex_present = false;
        uint256 hash;
        BOOST_REQUIRE(SignatureHashSchnorr(hash, execution, transfer, 0, SIGHASH_DEFAULT, SigVersion::TAPROOT,
                                          data, MissingDataBehavior::ASSERT_FAIL));
        std::vector<unsigned char> signature(64);
        const uint256 no_script_tree;
        BOOST_REQUIRE(owner.SignSchnorr(hash, signature, &no_script_tree, uint256{}));
        transfer.vin[0].scriptWitness.stack = {signature};
        return transfer;
    }

    std::unique_ptr<Candidate> Block(const std::vector<CMutableTransaction>& transactions, CBlockIndex* parent = nullptr)
    {
        auto& chain = m_node.chainman->ActiveChainstate();
        CBlock block = CreateBlock(transactions, CScript{} << OP_TRUE, chain);
        if (!parent) parent = WITH_LOCK(cs_main, return chain.m_chain.Tip());
        if (block.hashPrevBlock != parent->GetBlockHash()) {
            block.hashPrevBlock = parent->GetBlockHash();
            CMutableTransaction coinbase{*block.vtx[0]};
            coinbase.vin[0].scriptSig = CScript{} << (parent->nHeight + 1) << OP_0;
            block.vtx[0] = MakeTransactionRef(coinbase);
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        return std::make_unique<Candidate>(std::move(block), parent);
    }

    void CloneCoins(Chainstate& destination) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        destination.InitCoinsDB(1 << 20, true, false);
        destination.InitCoinsCache(1 << 20);
        auto& source = m_node.chainman->ActiveChainstate();
        auto cursor = source.CoinsTip().Cursor();
        BOOST_REQUIRE(cursor);
        for (; cursor->Valid(); cursor->Next()) {
            COutPoint out;
            Coin coin;
            BOOST_REQUIRE(cursor->GetKey(out));
            BOOST_REQUIRE(cursor->GetValue(coin));
            destination.CoinsTip().AddCoin(out, std::move(coin), false);
        }
        cursor->CheckStatus();
        destination.CoinsTip().SetBestBlock(source.CoinsTip().GetBestBlock());
        destination.CoinsTip().SetDigiDollarState(source.CoinsTip().GetDigiDollarState());
        destination.m_chain.SetTip(*source.m_chain.Tip());
        BOOST_REQUIRE(destination.CoinsTip().Flush());
    }

    void CheckBackgroundPublication(bool canonical)
    {
        auto candidate = Block({Mint(1, (canonical ? 3410 : 500) * COIN, 120)});
        LOCK(cs_main);
        auto& manager = *m_node.chainman;
        auto& active = manager.ActiveChainstate();
        const auto live_coins = Snapshot(active.CoinsTip());
        const auto live_health = active.CoinsTip().GetDigiDollarState();
        const auto legacy = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
        Chainstate background{nullptr, nullptr, manager.m_blockman, manager};
        CloneCoins(background);

        BlockValidationState accepted;
        CBlockIndex* index{nullptr};
        BOOST_REQUIRE_MESSAGE(manager.AcceptBlock(std::make_shared<CBlock>(candidate->block), accepted,
            &index, true, nullptr, nullptr, true), accepted.ToString());
        BOOST_REQUIRE(index);
        auto& oracle = OracleBundleManager::GetInstance();
        constexpr CAmount previous_price{1500000};
        oracle.UpdatePriceCache(index->nHeight - 1, previous_price, candidate->block.nTime);
        MockOracleManager::GetInstance().SetMockPrice(previous_price);
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();

        BlockValidationState connected;
        BOOST_REQUIRE_MESSAGE(background.ConnectBlock(candidate->block, connected, index, background.CoinsTip(), false), connected.ToString());
        const auto connected_metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
        BOOST_CHECK_EQUAL(connected_metrics.totalDDSupply, legacy.totalDDSupply + (canonical ? 0 : PRINCIPAL));
        BOOST_CHECK_EQUAL(connected_metrics.totalCollateral, legacy.totalCollateral + (canonical ? 0 : 500 * COIN));
        BOOST_CHECK_EQUAL(oracle.GetLatestPrice(), canonical ? previous_price : 1000000);
        BOOST_CHECK_EQUAL(MockOracleManager::GetInstance().GetCurrentPrice(), canonical ? previous_price : 1000000);
        BOOST_CHECK_EQUAL(DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().size(), canonical ? 0U : 1U);
        BOOST_REQUIRE(background.CoinsTip().Flush());
        if (canonical) {
            const auto saved = background.CoinsDB().GetDigiDollarState();
            BOOST_REQUIRE(saved);
            BOOST_CHECK_EQUAL(saved->open_vault_principal, 2 * PRINCIPAL);
        }

        BOOST_REQUIRE_EQUAL(background.DisconnectBlock(candidate->block, index, background.CoinsTip(), false), DISCONNECT_OK);
        BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalDDSupply, legacy.totalDDSupply);
        BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalCollateral, legacy.totalCollateral);
        BOOST_CHECK_EQUAL(oracle.GetLatestPrice(), previous_price);
        BOOST_CHECK_EQUAL(MockOracleManager::GetInstance().GetCurrentPrice(), previous_price);
        BOOST_CHECK(DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().empty());
        BOOST_CHECK(Snapshot(active.CoinsTip()) == live_coins);
        BOOST_CHECK(active.CoinsTip().GetDigiDollarState() == live_health);
    }
};

struct LegacyChainstateValidationSetup : ChainstateValidationSetup {
    LegacyChainstateValidationSetup() : ChainstateValidationSetup("-ddthawdayheight=121")
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        InstallQuote(1000000);
    }
};
struct LegacyLookupCacheSetup : ChainstateValidationSetup {
    LegacyLookupCacheSetup() : ChainstateValidationSetup("-ddthawdayheight=122")
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        InstallQuote(1000000);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_chainstate_validation_tests, ChainstateValidationSetup)

BOOST_AUTO_TEST_CASE(private_connection_advances_health_without_publishing)
{
    auto block = Block({Mint(1, 3410 * COIN, 120)});
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto before = chain.CoinsTip().GetDigiDollarState();
    const auto coins = Snapshot(chain.CoinsTip());
    const auto legacy = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    const auto oracle = OracleBundleManager::GetInstance().GetLatestPrice();
    CCoinsViewCache candidate{&chain.CoinsTip()};
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chain.ConnectBlock(block->block, state, &block->index, candidate, true), state.ToString());
    BOOST_REQUIRE(candidate.GetDigiDollarState());
    BOOST_CHECK_EQUAL(candidate.GetDigiDollarState()->open_vault_principal, 2 * PRINCIPAL);
    BOOST_CHECK_EQUAL(candidate.GetDigiDollarState()->collateral, 3910 * COIN);
    BOOST_CHECK(candidate.GetDigiDollarState()->history_checked);
    BOOST_CHECK(candidate.GetBestBlock() == block->hash);
    BOOST_CHECK(!candidate.HaveCoin(funding[1]));
    BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == before);
    BOOST_CHECK(chain.CoinsDB().GetDigiDollarState() == before);
    BOOST_CHECK(Snapshot(chain.CoinsTip()) == coins);
    BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalDDSupply, legacy.totalDDSupply);
    BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalCollateral, legacy.totalCollateral);
    BOOST_CHECK_EQUAL(OracleBundleManager::GetInstance().GetLatestPrice(), oracle);
}

BOOST_AUTO_TEST_CASE(later_failure_discards_an_earlier_staged_mint)
{
    auto mint = Mint(1, 3410 * COIN, 120);
    CMutableTransaction later;
    later.vin.emplace_back(funding[3]);
    later.vout.emplace_back(FUNDING + 1, CScript{} << OP_TRUE);
    auto block = Block({mint, later});
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto before = chain.CoinsTip().GetDigiDollarState();
    const auto coins = Snapshot(chain.CoinsTip());
    const auto oracle = OracleBundleManager::GetInstance().GetLatestPrice();
    {
        CCoinsViewCache candidate{&chain.CoinsTip()};
        BlockValidationState state;
        BOOST_CHECK(!chain.ConnectBlock(block->block, state, &block->index, candidate, false));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-in-belowout");
        BOOST_REQUIRE(candidate.GetDigiDollarState());
        BOOST_CHECK_EQUAL(candidate.GetDigiDollarState()->open_vault_principal, 2 * PRINCIPAL);
        BOOST_CHECK(!candidate.HaveCoin(funding[1]));
        BOOST_CHECK(candidate.HaveCoin(COutPoint{mint.GetHash(), 0}));
    }
    BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == before);
    BOOST_CHECK(chain.CoinsDB().GetDigiDollarState() == before);
    BOOST_CHECK(Snapshot(chain.CoinsTip()) == coins);
    BOOST_CHECK_EQUAL(OracleBundleManager::GetInstance().GetLatestPrice(), oracle);
}

BOOST_AUTO_TEST_CASE(later_mint_uses_updated_candidate_health)
{
    auto alone = Block({Mint(2, 2273 * COIN, 120)});
    auto ordered = Block({Mint(1, 3410 * COIN, 120), Mint(2, 2273 * COIN, 120)});
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto before = chain.CoinsTip().GetDigiDollarState();
    CCoinsViewCache isolated{&chain.CoinsTip()};
    BlockValidationState failed;
    BOOST_CHECK(!chain.ConnectBlock(alone->block, failed, &alone->index, isolated, true));
    BOOST_CHECK_EQUAL(failed.GetRejectReason(), "insufficient-collateral");
    CCoinsViewCache candidate{&chain.CoinsTip()};
    BlockValidationState accepted;
    BOOST_REQUIRE_MESSAGE(chain.ConnectBlock(ordered->block, accepted, &ordered->index, candidate, true), accepted.ToString());
    BOOST_REQUIRE(candidate.GetDigiDollarState());
    BOOST_CHECK_EQUAL(candidate.GetDigiDollarState()->open_vault_principal, 3 * PRINCIPAL);
    BOOST_CHECK_EQUAL(candidate.GetDigiDollarState()->collateral, 6183 * COIN);
    BOOST_CHECK_EQUAL(candidate.GetDigiDollarState()->active_vaults, 3U);
    BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == before);
}

BOOST_AUTO_TEST_CASE(same_block_token_source_is_resolved_from_candidate_memory)
{
    auto mint = Mint(1, 3410 * COIN, 120);
    CMutableTransaction transfer;
    transfer.SetDigiDollarType(DD_TX_TRANSFER);
    transfer.vin.emplace_back(COutPoint{mint.GetHash(), 1});
    transfer.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(XOnlyPubKey{owner.GetPubKey()}, PRINCIPAL));
    transfer.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                          << CScriptNum(2) << CScriptNum(PRINCIPAL));
    PrecomputedTransactionData data;
    data.Init(transfer, std::vector<CTxOut>{mint.vout[1]}, true);
    ScriptExecutionData execution;
    execution.m_annex_init = true;
    execution.m_annex_present = false;
    uint256 hash;
    BOOST_REQUIRE(SignatureHashSchnorr(hash, execution, transfer, 0, SIGHASH_DEFAULT, SigVersion::TAPROOT,
                                      data, MissingDataBehavior::ASSERT_FAIL));
    std::vector<unsigned char> signature(64);
    const uint256 no_script_tree;
    BOOST_REQUIRE(owner.SignSchnorr(hash, signature, &no_script_tree, uint256{}));
    transfer.vin[0].scriptWitness.stack = {signature};
    auto block = Block({mint, transfer});
    ScopedTokenMetadata missing_registry{mint.vout[1].scriptPubKey, DigiDollar::ScriptType::NOT_DIGIDOLLAR, 0};
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    BOOST_CHECK(!chain.m_blockman.LookupBlockIndex(block->hash));
    CCoinsViewCache candidate{&chain.CoinsTip()};
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chain.ConnectBlock(block->block, state, &block->index, candidate, true), state.ToString());
    BOOST_CHECK(!candidate.HaveCoin(COutPoint{mint.GetHash(), 1}));
    BOOST_CHECK(candidate.HaveCoin(COutPoint{transfer.GetHash(), 0}));
    BOOST_REQUIRE(candidate.GetDigiDollarState());
    BOOST_CHECK_EQUAL(candidate.GetDigiDollarState()->open_vault_principal, 2 * PRINCIPAL);
    BOOST_CHECK_EQUAL(chain.CoinsTip().GetDigiDollarState()->open_vault_principal, PRINCIPAL);
}

BOOST_AUTO_TEST_CASE(activated_same_block_lookup_ignores_conflicting_registry_amount)
{
    auto mint = Mint(1, 3410 * COIN, 120);
    auto transfer = TransferMintToken(mint, 2 * PRINCIPAL);
    auto block = Block({mint, transfer});
    ScopedTokenMetadata registry{mint.vout[1].scriptPubKey, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, 2 * PRINCIPAL};
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto saved = chain.CoinsTip().GetDigiDollarState();
    const auto legacy = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    CCoinsViewCache candidate{&chain.CoinsTip()};
    BlockValidationState state;
    BOOST_CHECK(!chain.ConnectBlock(block->block, state, &block->index, candidate, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "transfer-dd-conservation-violation");
    BOOST_CHECK(candidate.HaveCoin(COutPoint{mint.GetHash(), 1}));
    BOOST_CHECK(!candidate.HaveCoin(COutPoint{transfer.GetHash(), 0}));
    BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == saved);
    BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalDDSupply, legacy.totalDDSupply);
    BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalCollateral, legacy.totalCollateral);
}

BOOST_AUTO_TEST_CASE(independent_chainstates_keep_distinct_validated_health)
{
    auto improved_block = Block({Mint(1, 3410 * COIN, 120)});
    auto unchanged_block = Block({});
    LOCK(cs_main);
    auto& manager = *m_node.chainman;
    const auto live = manager.ActiveChainstate().CoinsTip().GetDigiDollarState();
    Chainstate improved{nullptr, nullptr, manager.m_blockman, manager};
    Chainstate unchanged{nullptr, nullptr, manager.m_blockman, manager};
    CloneCoins(improved);
    CloneCoins(unchanged);
    BlockValidationState first, empty;
    BOOST_REQUIRE_MESSAGE(improved.ConnectBlock(improved_block->block, first, &improved_block->index, improved.CoinsTip(), true), first.ToString());
    BOOST_REQUIRE_MESSAGE(unchanged.ConnectBlock(unchanged_block->block, empty, &unchanged_block->index, unchanged.CoinsTip(), true), empty.ToString());
    improved.m_chain.SetTip(improved_block->index);
    unchanged.m_chain.SetTip(unchanged_block->index);
    BOOST_REQUIRE(improved.CoinsTip().Flush());
    BOOST_REQUIRE(unchanged.CoinsTip().Flush());
    auto next_improved = Block({Mint(2, 2273 * COIN, 121)}, &improved_block->index);
    auto next_unchanged = Block({Mint(2, 2273 * COIN, 121)}, &unchanged_block->index);
    CCoinsViewCache improved_candidate{&improved.CoinsTip()};
    CCoinsViewCache unchanged_candidate{&unchanged.CoinsTip()};
    BlockValidationState accepted, rejected;
    BOOST_REQUIRE_MESSAGE(improved.ConnectBlock(next_improved->block, accepted, &next_improved->index, improved_candidate, true), accepted.ToString());
    BOOST_CHECK(!unchanged.ConnectBlock(next_unchanged->block, rejected, &next_unchanged->index, unchanged_candidate, true));
    BOOST_CHECK_EQUAL(rejected.GetRejectReason(), "insufficient-collateral");
    BOOST_REQUIRE(improved_candidate.GetDigiDollarState());
    BOOST_CHECK_EQUAL(improved_candidate.GetDigiDollarState()->open_vault_principal, 3 * PRINCIPAL);
    const auto improved_saved = improved.CoinsDB().GetDigiDollarState();
    const auto unchanged_saved = unchanged.CoinsDB().GetDigiDollarState();
    BOOST_REQUIRE(improved_saved);
    BOOST_REQUIRE(unchanged_saved);
    BOOST_CHECK_EQUAL(improved_saved->open_vault_principal, 2 * PRINCIPAL);
    BOOST_CHECK_EQUAL(unchanged_saved->open_vault_principal, PRINCIPAL);
    BOOST_CHECK(improved_saved->best_block == improved_block->hash);
    BOOST_CHECK(unchanged_saved->best_block == unchanged_block->hash);
    BOOST_CHECK(manager.ActiveChainstate().CoinsTip().GetDigiDollarState() == live);
    BOOST_CHECK(manager.ActiveChainstate().CoinsDB().GetDigiDollarState() == live);
}

BOOST_AUTO_TEST_CASE(activated_background_connection_keeps_process_observations_private)
{
    CheckBackgroundPublication(true);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(digidollar_legacy_chainstate_validation_tests, LegacyChainstateValidationSetup)

BOOST_AUTO_TEST_CASE(abandoned_mint_preserves_legacy_clamped_inverse)
{
    auto checked = Block({Mint(1, 500 * COIN, 120)});
    CMutableTransaction later;
    later.vin.emplace_back(funding[3]);
    later.vout.emplace_back(FUNDING + 1, CScript{} << OP_TRUE);
    auto rejected = Block({Mint(1, 500 * COIN, 120), later});
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto live_coins = Snapshot(chain.CoinsTip());
    for (bool just_check : {true, false}) {
        DigiDollar::SystemMetrics metrics;
        metrics.totalCollateral = std::numeric_limits<CAmount>::max();
        DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);
        auto& block = just_check ? checked : rejected;
        CCoinsViewCache candidate{&chain.CoinsTip()};
        BlockValidationState state;
        BOOST_CHECK_EQUAL(chain.ConnectBlock(block->block, state, &block->index, candidate, just_check), just_check);
        if (!just_check) BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-in-belowout");
        BOOST_CHECK(candidate.HaveCoin(COutPoint{block->block.vtx[1]->GetHash(), 0}));
        const auto after = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
        BOOST_CHECK_EQUAL(after.totalDDSupply, 0);
        BOOST_CHECK_EQUAL(after.totalCollateral, metrics.totalCollateral - 500 * COIN);
        BOOST_CHECK(Snapshot(chain.CoinsTip()) == live_coins);
    }
}

BOOST_AUTO_TEST_CASE(unsaved_legacy_candidate_preserves_registry_acceptance_and_updates)
{
    auto mint = Mint(1, 500 * COIN, 120);
    auto transfer = TransferMintToken(mint, 2 * PRINCIPAL);
    auto later_mint = Mint(2, 500 * COIN, 120);
    auto block = Block({mint, transfer, later_mint});
    ScopedTokenMetadata registry{mint.vout[1].scriptPubKey, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, 2 * PRINCIPAL};
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto live_coins = Snapshot(chain.CoinsTip());
    BOOST_REQUIRE(!DigiDollar::IsThawDayActive(Params().GetConsensus(), block->index.nHeight));
    BOOST_REQUIRE(!(block->index.nStatus & BLOCK_HAVE_DATA));
    BOOST_REQUIRE(!chain.m_blockman.LookupBlockIndex(block->hash));
    DigiDollar::SystemMetrics metrics;
    metrics.totalCollateral = std::numeric_limits<CAmount>::max();
    DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);
    const auto history_size = DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().size();
    CCoinsViewCache candidate{&chain.CoinsTip()};
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chain.ConnectBlock(block->block, state, &block->index, candidate, true), state.ToString());
    BOOST_CHECK(!candidate.HaveCoin(COutPoint{mint.GetHash(), 1}));
    BOOST_CHECK(candidate.HaveCoin(COutPoint{transfer.GetHash(), 0}));
    BOOST_CHECK(candidate.HaveCoin(COutPoint{later_mint.GetHash(), 0}));
    // Both legacy mint updates ran; abandoning the check retains their clamped inverse behavior.
    const auto after = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(after.totalDDSupply, 0);
    BOOST_CHECK_EQUAL(after.totalCollateral, metrics.totalCollateral - 1000 * COIN);
    DigiDollar::ScriptMetadata metadata;
    BOOST_REQUIRE(DigiDollar::GetScriptMetadata(mint.vout[1].scriptPubKey, metadata));
    BOOST_CHECK_EQUAL(metadata.ddAmount, 2 * PRINCIPAL);
    BOOST_CHECK_EQUAL(DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().size(), history_size);
    BOOST_CHECK(Snapshot(chain.CoinsTip()) == live_coins);
}

BOOST_AUTO_TEST_CASE(unsaved_legacy_candidate_preserves_missing_amount_rejection_and_updates)
{
    auto mint = Mint(1, 500 * COIN, 120);
    auto transfer = TransferMintToken(mint, PRINCIPAL);
    auto later_mint = Mint(2, 500 * COIN, 120);
    auto block = Block({mint, transfer, later_mint});
    ScopedTokenMetadata missing_registry{mint.vout[1].scriptPubKey, DigiDollar::ScriptType::NOT_DIGIDOLLAR, 0};
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto live_coins = Snapshot(chain.CoinsTip());
    BOOST_REQUIRE(!DigiDollar::IsThawDayActive(Params().GetConsensus(), block->index.nHeight));
    BOOST_REQUIRE(!(block->index.nStatus & BLOCK_HAVE_DATA));
    BOOST_REQUIRE(!chain.m_blockman.LookupBlockIndex(block->hash));
    DigiDollar::SystemMetrics metrics;
    metrics.totalCollateral = std::numeric_limits<CAmount>::max();
    DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);
    const auto history_size = DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().size();
    CCoinsViewCache candidate{&chain.CoinsTip()};
    BlockValidationState state;
    BOOST_CHECK(!chain.ConnectBlock(block->block, state, &block->index, candidate, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "dd-input-amounts-unknown");
    BOOST_CHECK(candidate.HaveCoin(COutPoint{mint.GetHash(), 1}));
    BOOST_CHECK(!candidate.HaveCoin(COutPoint{transfer.GetHash(), 0}));
    BOOST_CHECK(!candidate.HaveCoin(COutPoint{later_mint.GetHash(), 0}));
    // The failed transfer prevents the second mint's legacy accounting update.
    const auto after = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(after.totalDDSupply, 0);
    BOOST_CHECK_EQUAL(after.totalCollateral, metrics.totalCollateral - 500 * COIN);
    DigiDollar::ScriptMetadata metadata;
    BOOST_REQUIRE(DigiDollar::GetScriptMetadata(mint.vout[1].scriptPubKey, metadata));
    BOOST_CHECK(metadata.type == DigiDollar::ScriptType::NOT_DIGIDOLLAR);
    BOOST_CHECK_EQUAL(metadata.ddAmount, 0);
    BOOST_CHECK_EQUAL(DigiDollar::Volatility::VolatilityMonitor::GetPriceHistory().size(), history_size);
    BOOST_CHECK(Snapshot(chain.CoinsTip()) == live_coins);
}

BOOST_FIXTURE_TEST_CASE(legacy_disk_cache_requires_verification_before_canonical_reuse, LegacyLookupCacheSetup)
{
    CBlock source;
    {
        LOCK(cs_main);
        auto& chain = m_node.chainman->ActiveChainstate();
        BOOST_REQUIRE(chain.m_blockman.ReadBlockFromDisk(source, *chain.m_chain[102]));
    }
    BOOST_REQUIRE_EQUAL(source.vtx.size(), 2U);
    const CMutableTransaction mint{*source.vtx[1]};
    auto transfer = TransferMintToken(mint, PRINCIPAL);
    auto block = Block({transfer});
    ScopedTokenMetadata missing_registry{mint.vout[1].scriptPubKey, DigiDollar::ScriptType::NOT_DIGIDOLLAR, 0};
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    auto& blockman = chain.m_blockman;
    const auto saved_state = chain.CoinsTip().GetDigiDollarState();
    const auto live_coins = Snapshot(chain.CoinsTip());
    auto check_legacy = [&] {
        CCoinsViewCache candidate{&chain.CoinsTip()};
        BlockValidationState state;
        BOOST_REQUIRE_MESSAGE(chain.ConnectBlock(block->block, state, &block->index, candidate, true), state.ToString());
        BOOST_CHECK(candidate.HaveCoin(COutPoint{transfer.GetHash(), 0}));
    };
    // Populate the same legacy disk cache used before the activation gate.
    check_legacy();

    // Append a body that disagrees with its unchanged header in the temporary
    // block files. Restore only the index position; retain the original bytes.
    CBlock damaged = source;
    CMutableTransaction coinbase{*damaged.vtx[0]};
    coinbase.nLockTime ^= 1;
    damaged.vtx[0] = MakeTransactionRef(coinbase);
    BOOST_REQUIRE(damaged.GetHash() == source.GetHash());
    BOOST_REQUIRE(BlockMerkleRoot(damaged) != damaged.hashMerkleRoot);
    const auto damaged_pos = blockman.SaveBlockToDisk(damaged, 102, nullptr);
    BOOST_REQUIRE(!damaged_pos.IsNull());
    auto* index = chain.m_chain[102];
    struct RestorePosition {
        CBlockIndex& index;
        const int file;
        const unsigned int position;
        ~RestorePosition() { index.nFile = file; index.nDataPos = position; }
    } restore{*index, index->nFile, index->nDataPos};
    index->nFile = damaged_pos.nFile;
    index->nDataPos = damaged_pos.nPos;

    auto params = Params().GetConsensus();
    params.nDDThawDayHeight = 120;
    DigiDollar::ChainstateHealth canonical;
    canonical.best_block = uint256::ONE;
    const auto untouched = canonical;
    int health{-1};
    std::string error;
    BOOST_CHECK(!DigiDollar::GetChainstateHealthForNextBlock(chain.m_chain.Tip(), params, blockman,
        chain.CoinsTip(), PRICE, health, canonical, error));
    BOOST_CHECK_MESSAGE(error.find("DigiDollar state not ready:") != std::string::npos, error);
    BOOST_CHECK(canonical == untouched);
    BOOST_CHECK_EQUAL(health, -1);
    // A failed canonical verification must not change legacy cached lookup behavior.
    check_legacy();

    index->nFile = restore.file;
    index->nDataPos = restore.position;
    BOOST_REQUIRE_MESSAGE(DigiDollar::GetChainstateHealthForNextBlock(chain.m_chain.Tip(), params, blockman,
        chain.CoinsTip(), PRICE, health, canonical, error), error);
    BOOST_CHECK_EQUAL(health, 110);
    BOOST_CHECK_EQUAL(canonical.open_vault_principal, PRINCIPAL);
    BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == saved_state);
    BOOST_CHECK(Snapshot(chain.CoinsTip()) == live_coins);
}

BOOST_AUTO_TEST_CASE(background_connection_preserves_legacy_process_publication)
{
    CheckBackgroundPublication(false);
}

BOOST_AUTO_TEST_CASE(late_upgrade_rechecks_previously_valid_digidollar_history)
{
    InstallQuote(PRICE);
    auto& manager = *m_node.chainman;
    auto& chain = manager.ActiveChainstate();
    const auto parent_hash = WITH_LOCK(cs_main, return chain.m_chain.Tip()->GetBlockHash());
    const auto parent_coins = WITH_LOCK(cs_main, return Snapshot(chain.CoinsTip()));

    // Model a legacy process with its bootstrap metrics. The installed rules
    // still accept this mint; canonical accounting must independently check it.
    DigiDollar::SystemHealthMonitor::ResetMetrics();
    const auto historical = CreateAndProcessBlock({Mint(1, 2273 * COIN, 120)}, CScript{} << OP_TRUE);
    LOCK(cs_main);
    BOOST_REQUIRE(chain.m_chain.Tip()->GetBlockHash() == historical.GetHash());
    CBlockIndex* old_tip = chain.m_chain.Tip();
    BOOST_REQUIRE_EQUAL(old_tip->nHeight, 120);
    BOOST_REQUIRE(old_tip->IsValid(BLOCK_VALID_SCRIPTS));
    BOOST_REQUIRE(old_tip->nStatus & BLOCK_HAVE_UNDO);
    // Reaching the old H-1 may prepare totals, but cannot prove that block
    // was validated under an earlier activation boundary installed later.
    const auto prepared = chain.CoinsTip().GetDigiDollarState();
    BOOST_REQUIRE(prepared);
    BOOST_REQUIRE_EQUAL(prepared->activation_height, 121);
    BOOST_REQUIRE(!prepared->history_checked);

    auto& params = const_cast<Consensus::Params&>(manager.GetConsensus());
    struct RestoreActivation {
        int& height;
        const int saved;
        ~RestoreActivation() { height = saved; }
    } restore{params.nDDThawDayHeight, params.nDDThawDayHeight};
    params.nDDThawDayHeight = 120;
    std::string error;
    BOOST_CHECK(!chain.InitializeDigiDollarState({}, error));
    BOOST_CHECK_MESSAGE(error.find("insufficient-collateral") != std::string::npos, error);
    BOOST_CHECK_MESSAGE(error.find("height 120") != std::string::npos, error);
    BOOST_CHECK(old_tip->nStatus & BLOCK_FAILED_VALID);
    BOOST_CHECK(chain.m_chain.Tip()->GetBlockHash() == parent_hash);
    BOOST_CHECK(chain.CoinsTip().GetBestBlock() == parent_hash);
    BOOST_CHECK(chain.CoinsDB().GetBestBlock() == parent_hash);
    BOOST_CHECK(Snapshot(chain.CoinsTip()) == parent_coins);

    // A second initialization can use the accepted prefix without repeatedly
    // retrying the rejected history or treating its old validity flag as proof.
    error.clear();
    BOOST_REQUIRE_MESSAGE(chain.InitializeDigiDollarState({}, error), error);
    const auto saved = chain.CoinsTip().GetDigiDollarState();
    BOOST_REQUIRE(saved);
    BOOST_CHECK(saved->best_block == parent_hash);
    BOOST_CHECK_EQUAL(saved->open_vault_principal, PRINCIPAL);
    BOOST_CHECK_EQUAL(saved->collateral, 500 * COIN);
    BOOST_CHECK_EQUAL(saved->active_vaults, 1U);
    BOOST_CHECK(!saved->history_checked);
    BOOST_CHECK(chain.CoinsDB().GetDigiDollarState() == saved);
}

BOOST_AUTO_TEST_SUITE_END()
