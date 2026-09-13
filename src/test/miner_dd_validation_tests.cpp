// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <coins.h>
#include <consensus/digidollar.h>
#include <consensus/merkle.h>
#include <consensus/volatility.h>
#include <crypto/sha256.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/digidollarstatsindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <node/miner.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <oracle/musig2_aggregator.h>
#include <policy/feerate.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <random.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/standard.h>
#include <shutdown.h>
#include <test/util/setup_common.h>
#include <test/util/logging.h>
#include <test/util/index.h>
#include <test/util/txmempool.h>
#include <timedata.h>
#include <validation.h>
#include <validationinterface.h>
#include <warnings.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using node::BlockAssembler;
using node::CBlockTemplate;

namespace {

std::array<unsigned char, 32> RegtestOracleSecret(uint8_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    std::array<unsigned char, 32> secret{};
    std::memcpy(secret.data(), hash.begin(), secret.size());
    return secret;
}

bool SignRegtestV03Bundle(COracleBundle& bundle, const std::vector<uint8_t>& oracle_ids)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    const size_t n_signers = oracle_ids.size();
    std::vector<std::array<unsigned char, 32>> seckeys(n_signers);
    std::vector<secp256k1_keypair> keypairs(n_signers);
    std::vector<secp256k1_pubkey> pubkeys(n_signers);

    for (size_t i = 0; i < n_signers; ++i) {
        seckeys[i] = RegtestOracleSecret(oracle_ids[i]);
        if (!secp256k1_keypair_create(ctx, &keypairs[i], seckeys[i].data()) ||
            !secp256k1_keypair_pub(ctx, &pubkeys[i], &keypairs[i])) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_pubkey*> pubkey_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        pubkey_ptrs[i] = &pubkeys[i];
    }

    secp256k1_xonly_pubkey agg_pk{};
    secp256k1_musig_keyagg_cache cache{};
    if (!secp256k1_musig_pubkey_agg(ctx, &agg_pk, &cache, pubkey_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_secnonce> secnonces(n_signers);
    std::vector<secp256k1_musig_pubnonce> pubnonces(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        unsigned char session_rand[32];
        GetStrongRandBytes(Span{session_rand, 32});
        if (!secp256k1_musig_nonce_gen(ctx, &secnonces[i], &pubnonces[i],
                                       session_rand, seckeys[i].data(), &pubkeys[i],
                                       nullptr, &cache, nullptr)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_musig_pubnonce*> nonce_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        nonce_ptrs[i] = &pubnonces[i];
    }

    secp256k1_musig_aggnonce aggnonce{};
    if (!secp256k1_musig_nonce_agg(ctx, &aggnonce, nonce_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    const uint256 msg_hash = ComputeOracleBundleHash(bundle);
    unsigned char msg32[32];
    std::memcpy(msg32, msg_hash.begin(), sizeof(msg32));

    secp256k1_musig_session session{};
    if (!secp256k1_musig_nonce_process(ctx, &session, &aggnonce, msg32, &cache)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_partial_sig> partial_sigs(n_signers);
    std::vector<const secp256k1_musig_partial_sig*> partial_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        if (!secp256k1_musig_partial_sign(ctx, &partial_sigs[i], &secnonces[i],
                                          &keypairs[i], &cache, &session)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        partial_ptrs[i] = &partial_sigs[i];
    }

    bundle.participation_bitmap = MuSig2OracleAggregator::EncodeBitmap(
        oracle_ids, static_cast<uint16_t>(Params().GetConsensus().nOracleTotalOracles));
    bundle.aggregate_sig.assign(64, 0);
    if (!secp256k1_musig_partial_sig_agg(ctx, bundle.aggregate_sig.data(),
                                         &session, partial_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    const bool verifies = secp256k1_schnorrsig_verify(ctx, bundle.aggregate_sig.data(), msg32, 32, &agg_pk);
    secp256k1_context_destroy(ctx);
    return verifies;
}

bool BlockHasTx(const CBlock& block, const uint256& txid)
{
    for (const auto& tx : block.vtx) {
        if (tx->GetHash() == txid) {
            return true;
        }
    }
    return false;
}

struct MinerDDValidationSetup : public TestChain100Setup {
    size_t m_coinbase_spend_index{0};

    explicit MinerDDValidationSetup(const std::vector<const char*>& extra_args = {})
        : TestChain100Setup(ChainType::REGTEST, extra_args)
    {
        MockOracleManager::GetInstance().Reset();
        MockOracleManager::GetInstance().SetEnabled(false);
        OracleBundleManager::GetInstance().Clear();
        OracleBundleManager::GetInstance().SetEnabled(true);
        EnsureDigiDollarActive();
    }

    ~MinerDDValidationSetup()
    {
        MockOracleManager::GetInstance().Reset();
        OracleBundleManager::GetInstance().Clear();
        OracleBundleManager::GetInstance().SetEnabled(true);
    }

    void EnsureDigiDollarActive()
    {
        for (int i = 0; i < 2500; ++i) {
            const bool active = WITH_LOCK(cs_main, return DigiDollar::IsDigiDollarEnabled(m_node.chainman->ActiveChain().Tip(), *m_node.chainman));
            const int next_height = NextBlockHeight();
            const bool musig2_ready = Params().GetConsensus().IsMuSig2OracleActive(next_height);
            if (active && musig2_ready) {
                return;
            }
            mineBlocks(1);
        }
        BOOST_FAIL("DigiDollar/MuSig2 activation did not activate in time");
    }

    int NextBlockHeight() const
    {
        return WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height() + 1);
    }

    void InstallMuSig2OraclePrice(CAmount price_micro_usd, int block_height)
    {
        OracleBundleManager& manager = OracleBundleManager::GetInstance();
        manager.SetEnabled(true);

        COracleBundle bundle(GetCurrentEpoch(block_height));
        bundle.version = 3;
        bundle.median_price_micro_usd = static_cast<uint64_t>(price_micro_usd);
        bundle.timestamp = GetTime();

        std::vector<uint8_t> oracle_ids;
        for (int id = 0; id < Params().GetConsensus().nOracleConsensusRequired; ++id) {
            oracle_ids.push_back(static_cast<uint8_t>(id));
        }

        BOOST_REQUIRE(SignRegtestV03Bundle(bundle, oracle_ids));
        std::string error;
        BOOST_REQUIRE_MESSAGE(
            OracleBundleManager::ValidateMuSig2Bundle(bundle, block_height, Params().GetConsensus(), error),
            error);
        BOOST_REQUIRE(manager.UpdateBundle(bundle));
    }

    COutPoint ConfirmOpTrueFunding(CAmount output_value)
    {
        BOOST_REQUIRE(m_coinbase_spend_index < m_coinbase_txns.size());
        const int input_height = static_cast<int>(m_coinbase_spend_index + 1);
        CMutableTransaction funding = CreateValidMempoolTransaction(
            m_coinbase_txns[m_coinbase_spend_index], 0, input_height, coinbaseKey,
            CScript() << OP_TRUE, output_value, /*submit=*/false);
        ++m_coinbase_spend_index;

        const CPubKey coinbase_pubkey = coinbaseKey.GetPubKey();
        const CScript coinbase_script = CScript()
                                        << std::vector<unsigned char>(coinbase_pubkey.begin(), coinbase_pubkey.end())
                                        << OP_CHECKSIG;
        CBlock block = CreateAndProcessBlock({funding}, coinbase_script);
        BOOST_REQUIRE_GE(block.vtx.size(), 2U);
        return COutPoint(block.vtx[1]->GetHash(), 0);
    }

    CAmount RequiredCollateralAt(CAmount dd_amount, int lock_days, int next_height, CAmount oracle_price_micro_usd) const
    {
        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(lock_days);
        DigiDollar::ValidationContext ctx(next_height, oracle_price_micro_usd, 300, Params());
        return DigiDollar::CalculateRequiredCollateral(dd_amount, lock_blocks, ctx);
    }

    CTransactionRef BuildDDMint(const COutPoint& prevout, CAmount input_value, CAmount collateral_value,
                                CAmount dd_amount, int next_height, CAmount fee, int lock_days = 30,
                                const CKey* supplied_owner = nullptr)
    {
        BOOST_REQUIRE(input_value >= collateral_value + fee);

        CKey owner_key;
        if (supplied_owner) owner_key = *supplied_owner;
        else owner_key.MakeNewKey(true);
        XOnlyPubKey owner_xonly(owner_key.GetPubKey());

        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(lock_days);
        const int64_t lock_height = next_height + lock_blocks;

        DigiDollar::MintParams params;
        params.ddAmount = dd_amount;
        params.lockHeight = lock_height;
        params.ownerKey = owner_xonly;
        params.internalKey = DigiDollar::GetCollateralNUMSKey();
        params.oracleKeys = DigiDollar::GetOracleKeys(15);

        CMutableTransaction mint;
        mint.SetDigiDollarType(DD_TX_MINT);
        mint.vin.emplace_back(prevout);
        mint.vout.emplace_back(collateral_value, DigiDollar::CreateCollateralP2TR(params));
        mint.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(owner_xonly, dd_amount));

        CScript op_return = CScript() << OP_RETURN
                                      << std::vector<unsigned char>{'D', 'D'}
                                      << CScriptNum(1)
                                      << CScriptNum(dd_amount)
                                      << CScriptNum(lock_height)
                                      << CScriptNum(lock_days == 30 ? 1 : 0)
                                      << std::vector<unsigned char>(owner_xonly.begin(), owner_xonly.end());
        mint.vout.emplace_back(0, op_return);
        return MakeTransactionRef(mint);
    }

    CTransactionRef BuildOpTrueSpend(const COutPoint& prevout, CAmount input_value, CAmount fee)
    {
        BOOST_REQUIRE(input_value > fee);
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout.emplace_back(input_value - fee, CScript() << OP_TRUE);
        return MakeTransactionRef(tx);
    }

    CTransactionRef BuildOpTrueSpendWithDataCarrier(const COutPoint& prevout, CAmount input_value, CAmount fee,
                                                    const std::vector<unsigned char>& data)
    {
        BOOST_REQUIRE(input_value > fee);
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout.emplace_back(input_value - fee, CScript() << OP_TRUE);
        tx.vout.emplace_back(0, CScript() << OP_RETURN << data);
        return MakeTransactionRef(tx);
    }

    CTransactionRef BuildDDTransfer(const COutPoint& prevout, const XOnlyPubKey& recipient, CAmount dd_amount)
    {
        CMutableTransaction tx;
        tx.SetDigiDollarType(DD_TX_TRANSFER);
        tx.vin.emplace_back(prevout);
        tx.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(recipient, dd_amount));

        CScript op_return = CScript() << OP_RETURN
                                      << std::vector<unsigned char>{'D', 'D'}
                                      << CScriptNum(2)
                                      << CScriptNum(dd_amount);
        tx.vout.emplace_back(0, op_return);
        return MakeTransactionRef(tx);
    }

    bool ValidateMintAtPrice(const CTransaction& tx, int next_height, CAmount oracle_price_micro_usd, std::string* reject_reason = nullptr) const
    {
        DigiDollar::ValidationContext ctx(next_height, oracle_price_micro_usd, 300, Params());
        TxValidationState state;
        const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        if (reject_reason) {
            *reject_reason = state.GetRejectReason();
        }
        return valid;
    }

    void AddToMempool(const CTransactionRef& tx, CAmount fee)
    {
        LOCK2(cs_main, m_node.mempool->cs);
        TestMemPoolEntryHelper entry;
        m_node.mempool->addUnchecked(entry.Fee(fee).FromTx(tx));
    }

    std::unique_ptr<CBlockTemplate> BuildTemplate(const BlockAssembler::Options& options)
    {
        return BlockAssembler(m_node.chainman->ActiveChainstate(), m_node.mempool.get(), options)
            .CreateNewBlock(CScript() << OP_TRUE, ALGO_SHA256D);
    }
};

struct ThawMinerValidationSetup : MinerDDValidationSetup {
    static constexpr int THAW_HEIGHT{400};
    static constexpr CAmount REFERENCE_PRICE{50000};
    static constexpr CAmount MINT_FEE{1000};
    static constexpr CAmount CHILD_FEE{2000};
    static constexpr CAmount PLAIN_FEE{3000};

    struct CandidateTransactions {
        CTransactionRef mint;
        CTransactionRef child;
        CTransactionRef plain;
    };

    CKey redemption_owner;
    DigiDollar::MintParams redemption_vault;
    CTransactionRef redeemable_mint;
    CTransactionRef other_mint;

    explicit ThawMinerValidationSetup(bool seed_redemption = false, CAmount principal = 100)
        : MinerDDValidationSetup({"-digidollaractivationheight=100", "-ddthawdayheight=400"})
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        if (seed_redemption) {
            DigiDollar::SystemHealthMonitor::ResetMetrics();
            redemption_owner.MakeNewKey(true);
            const CAmount strong_collateral = 30 * COIN * principal / 100;
            const CAmount weak_collateral = 15 * COIN * principal / 100;
            const auto strong_funding = ConfirmOpTrueFunding(strong_collateral + MINT_FEE);
            const auto weak_funding = ConfirmOpTrueFunding(weak_collateral + MINT_FEE);
            const int height = NextBlockHeight();
            InstallMuSig2OraclePrice(1000000, height);
            redeemable_mint = BuildDDMint(strong_funding, strong_collateral + MINT_FEE,
                strong_collateral, principal, height, MINT_FEE, 0, &redemption_owner);
            other_mint = BuildDDMint(weak_funding, weak_collateral + MINT_FEE,
                weak_collateral, principal, height, MINT_FEE, 0, &redemption_owner);
            redemption_vault.ddAmount = principal;
            redemption_vault.lockHeight = height + DigiDollar::LockDaysToBlocks(0);
            redemption_vault.ownerKey = XOnlyPubKey(redemption_owner.GetPubKey());
            redemption_vault.internalKey = DigiDollar::GetCollateralNUMSKey();
            const auto seeded = CreateAndProcessBlock(
                {CMutableTransaction{*redeemable_mint}, CMutableTransaction{*other_mint}}, CScript() << OP_TRUE);
            BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()),
                                seeded.GetHash());
            SetMockTime(GetTime() + 1);
            OracleBundleManager::GetInstance().Clear();
        }
        InstallMuSig2OraclePrice(REFERENCE_PRICE, NextBlockHeight());
        BlockAssembler::Options options;
        auto reference_template = BuildTemplate(options);
        BOOST_REQUIRE(reference_template);
        COracleBundle reference_bundle;
        BOOST_REQUIRE(OracleBundleManager::GetInstance().ExtractOracleBundle(
            *reference_template->block.vtx[0], reference_bundle));
        CBlock reference_block{reference_template->block};
        while (!CheckProofOfWork(GetPoWAlgoHash(reference_block), reference_block.nBits,
                                Params().GetConsensus())) {
            ++reference_block.nNonce;
        }
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(
            std::make_shared<const CBlock>(reference_block), true, true, nullptr));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()),
                            reference_block.GetHash());
        SetMockTime(GetTime() + 1);
        OracleBundleManager::GetInstance().Clear();
        BOOST_REQUIRE_LT(NextBlockHeight(), THAW_HEIGHT);
        mineBlocks(THAW_HEIGHT - NextBlockHeight());
        BOOST_REQUIRE_EQUAL(NextBlockHeight(), THAW_HEIGHT);
    }

    ~ThawMinerValidationSetup()
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    }

    CandidateTransactions PrepareCandidate(CAmount candidate_price)
    {
        constexpr CAmount collateral{200 * COIN};
        constexpr CAmount dd_amount{100};
        const COutPoint mint_funding = ConfirmOpTrueFunding(collateral + COIN + MINT_FEE);
        const COutPoint plain_funding = ConfirmOpTrueFunding(2 * COIN);
        const int height = NextBlockHeight();
        InstallMuSig2OraclePrice(candidate_price, height);

        CMutableTransaction mint{*BuildDDMint(mint_funding, collateral + COIN + MINT_FEE,
                                             collateral, dd_amount, height, MINT_FEE)};
        mint.vout.emplace_back(COIN, CScript() << OP_TRUE);
        CandidateTransactions txs;
        txs.mint = MakeTransactionRef(mint);
        txs.child = BuildOpTrueSpend(COutPoint(txs.mint->GetHash(), 3), COIN, CHILD_FEE);
        txs.plain = BuildOpTrueSpend(plain_funding, 2 * COIN, PLAIN_FEE);
        AddToMempool(txs.mint, MINT_FEE);
        AddToMempool(txs.child, CHILD_FEE);
        AddToMempool(txs.plain, PLAIN_FEE);

        const auto reference = WITH_LOCK(cs_main, return DigiDollar::GetMintVolatilityReference(
            m_node.chainman->ActiveChain().Tip(), Params().GetConsensus(), m_node.chainman->m_blockman));
        BOOST_REQUIRE_MESSAGE(reference.ready, reference.error);
        BOOST_REQUIRE_GE(reference.sample_count, 1U);
        BOOST_REQUIRE_EQUAL(reference.price_micro_usd, REFERENCE_PRICE);
        return txs;
    }

    CBlockIndex* FirstRequiredAncestor() const
    {
        return WITH_LOCK(cs_main, return m_node.chainman->ActiveChain()[
            NextBlockHeight() - DigiDollar::Volatility::MINT_REFERENCE_MIN_DEPTH]);
    }

    void ReplaceCommittedPrice(CBlock& block, CAmount price)
    {
        auto& manager = OracleBundleManager::GetInstance();
        COracleBundle previous;
        BOOST_REQUIRE(manager.ExtractOracleBundle(*block.vtx[0], previous));
        const CScript old_script = manager.CreateOracleScript(previous);
        InstallMuSig2OraclePrice(price, NextBlockHeight());
        const CScript new_script = manager.CreateOracleScript(manager.GetCurrentBundle(GetCurrentEpoch(NextBlockHeight())));
        BOOST_REQUIRE(!new_script.empty());
        CMutableTransaction coinbase{*block.vtx[0]};
        size_t replaced{0};
        for (auto& output : coinbase.vout) {
            if (output.scriptPubKey == old_script) {
                output.scriptPubKey = new_script;
                ++replaced;
            }
        }
        BOOST_REQUIRE_EQUAL(replaced, 1U);
        block.vtx[0] = MakeTransactionRef(std::move(coinbase));
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }
};

struct ThawBoundaryTemplateSetup : MinerDDValidationSetup {
    static constexpr int THAW_HEIGHT{140};

    // Leaves the tip two blocks below the Thaw Day height, so the next block a
    // miner builds is the block immediately before the height. That is the block
    // whose accounting record the first Thaw Day block is checked against.
    ThawBoundaryTemplateSetup()
        : MinerDDValidationSetup({"-digidollaractivationheight=100", "-ddthawdayheight=140"})
    {
        BOOST_REQUIRE_LT(NextBlockHeight(), THAW_HEIGHT - 1);
        mineBlocks(THAW_HEIGHT - 1 - NextBlockHeight());
        BOOST_REQUIRE_EQUAL(NextBlockHeight(), THAW_HEIGHT - 1);
    }
};

struct ThawMinerERRValidationSetup : ThawMinerValidationSetup {
    explicit ThawMinerERRValidationSetup(CAmount principal = 100) : ThawMinerValidationSetup(true, principal) {}

    ~ThawMinerERRValidationSetup()
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
    }

    CTransactionRef BuildMatureRedemption(const COutPoint& fee_input, CAmount fee_value, CAmount fee,
                                         CAmount dd_change = 0, bool serialized_change = true,
                                         bool duplicate_positive_metadata = false)
    {
        BOOST_REQUIRE(redeemable_mint);
        BOOST_REQUIRE_LT(redemption_vault.lockHeight, NextBlockHeight());
        const CScript normal = DigiDollar::CreateNormalRedemptionPath(redemption_vault);
        TaprootBuilder tree;
        tree.Add(1, normal, 0xc0);
        tree.Add(1, DigiDollar::CreateERRPath(redemption_vault), 0xc0);
        tree.Finalize(redemption_vault.internalKey);
        BOOST_REQUIRE(tree.IsComplete());
        const auto spend_data = tree.GetSpendData();
        const auto paths = spend_data.scripts.find({normal, 0xc0});
        BOOST_REQUIRE(paths != spend_data.scripts.end());
        BOOST_REQUIRE(!paths->second.empty());

        CMutableTransaction tx;
        tx.SetDigiDollarType(DD_TX_REDEEM);
        tx.nLockTime = redemption_vault.lockHeight;
        tx.vin.emplace_back(COutPoint{redeemable_mint->GetHash(), 0}, CScript{}, 0xfffffffe);
        tx.vin.emplace_back(COutPoint{redeemable_mint->GetHash(), 1});
        std::vector<CTxOut> spent{redeemable_mint->vout[0], redeemable_mint->vout[1]};
        if (dd_change > 0) {
            BOOST_REQUIRE(other_mint);
            tx.vin.emplace_back(COutPoint{other_mint->GetHash(), 1});
            spent.push_back(other_mint->vout[1]);
        }
        tx.vin.emplace_back(fee_input);
        spent.emplace_back(fee_value, CScript() << OP_TRUE);
        tx.vout.emplace_back(redeemable_mint->vout[0].nValue, CScript() << OP_TRUE);
        tx.vout.emplace_back(fee_value - fee, CScript() << OP_TRUE);
        if (dd_change > 0) {
            tx.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(XOnlyPubKey(redemption_owner.GetPubKey()), dd_change));
            const CScript metadata = CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                               << CScriptNum(3) << CScriptNum(dd_change);
            if (serialized_change) tx.vout.emplace_back(0, metadata);
            if (duplicate_positive_metadata) tx.vout.emplace_back(1, metadata);
        }
        PrecomputedTransactionData data;
        data.Init(tx, std::move(spent), true);
        ScriptExecutionData execution;
        execution.m_annex_init = true;
        execution.m_annex_present = false;
        execution.m_tapleaf_hash_init = true;
        execution.m_tapleaf_hash = ComputeTapleafHash(0xc0, normal);
        execution.m_codeseparator_pos_init = true;
        execution.m_codeseparator_pos = 0xffffffff;
        uint256 hash;
        BOOST_REQUIRE(SignatureHashSchnorr(hash, execution, tx, 0, SIGHASH_DEFAULT, SigVersion::TAPSCRIPT,
                                          data, MissingDataBehavior::ASSERT_FAIL));
        std::vector<unsigned char> signature(64);
        BOOST_REQUIRE(redemption_owner.SignSchnorr(hash, signature, nullptr, uint256{}));
        tx.vin[0].scriptWitness.stack = {signature, std::vector<unsigned char>(normal.begin(), normal.end()),
                                       *paths->second.begin()};
        const uint256 no_script_tree;
        for (size_t i = 1; i + 1 < tx.vin.size(); ++i) {
            BOOST_REQUIRE(SignatureHashSchnorr(hash, execution, tx, i, SIGHASH_DEFAULT, SigVersion::TAPROOT,
                                              data, MissingDataBehavior::ASSERT_FAIL));
            BOOST_REQUIRE(redemption_owner.SignSchnorr(hash, signature, &no_script_tree, uint256{}));
            tx.vin[i].scriptWitness.stack = {signature};
        }
        return MakeTransactionRef(tx);
    }
};

struct OpenRedemptionStatsIndex {
    DigiDollarStatsIndex index;
    explicit OpenRedemptionStatsIndex(node::NodeContext& node)
        : index{interfaces::MakeChain(node), 1 << 20, false, false} {}
    ~OpenRedemptionStatsIndex()
    {
        SyncWithValidationInterfaceQueue();
        index.Interrupt();
        index.Stop();
    }
};

// Encode the current derived row to model a logically wrong saved total without
// changing its format, block hash, collateral, vault count or physical checksum.
struct SavedSupplyHeightKey {
    int height;
    template <typename Stream>
    void Serialize(Stream& stream) const
    {
        ser_writedata8(stream, uint8_t{'h'});
        ser_writedata32be(stream, 3);
        ser_writedata32be(stream, height);
    }
};

struct SavedSupplyValue {
    CAmount supply;
    CAmount collateral;
    uint64_t vaults;
    bool known;
    SERIALIZE_METHODS(SavedSupplyValue, obj) { READWRITE(obj.supply, obj.collateral, obj.vaults, obj.known); }
};

struct PreviousSupplyHeightKey {
    int height;
    template <typename Stream>
    void Serialize(Stream& stream) const
    {
        ser_writedata8(stream, uint8_t{'h'});
        ser_writedata32be(stream, height);
    }
};

struct PreviousSupplyValue {
    CAmount supply;
    CAmount collateral;
    uint64_t vaults;
    SERIALIZE_METHODS(PreviousSupplyValue, obj) { READWRITE(obj.supply, obj.collateral, obj.vaults); }
};

struct SupplyVerificationSetup : ThawMinerERRValidationSetup {
    SupplyVerificationSetup() : ThawMinerERRValidationSetup(1000) {}

    fs::path IndexPath() const { return m_args.GetDataDirNet() / "indexes" / "digidollarstats" / "db"; }
    CBlockIndex* Tip() { return WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()); }

    void SaveIndex()
    {
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_REQUIRE(opened.index.Init());
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        opened.index.Stop();
    }

    void ChangeSavedSupply(const CBlockIndex& block, CAmount supply)
    {
        CDBWrapper db{{.path = IndexPath(), .cache_bytes = 1 << 20, .memory_only = false}};
        std::pair<uint256, SavedSupplyValue> row;
        BOOST_REQUIRE(db.Read(SavedSupplyHeightKey{block.nHeight}, row));
        BOOST_REQUIRE(row.first == block.GetBlockHash());
        row.second.supply = supply;
        BOOST_REQUIRE(db.Write(SavedSupplyHeightKey{block.nHeight}, row));
    }

    void CheckSupply(const DigiDollarStatsIndex& index, const CBlockIndex& block,
                     CAmount supply, CAmount collateral, uint64_t vaults)
    {
        const auto stats = index.LookUpStats(block);
        BOOST_REQUIRE(stats);
        BOOST_CHECK_EQUAL(stats->total_dd_supply, supply);
        BOOST_CHECK_EQUAL(stats->total_collateral, collateral);
        BOOST_CHECK_EQUAL(stats->vault_count, vaults);
    }

    CBlockIndex* AcceptExtraBurn(const COutPoint& funding)
    {
        InstallMuSig2OraclePrice(REFERENCE_PRICE, NextBlockHeight());
        const auto redemption = BuildMatureRedemption(funding, 2 * COIN, COIN, 750);
        const auto accepted = CreateAndProcessBlock({CMutableTransaction{*redemption}}, CScript() << OP_TRUE);
        BOOST_REQUIRE(Tip()->GetBlockHash() == accepted.GetHash());
        BOOST_REQUIRE(BlockHasTx(accepted, redemption->GetHash()));
        const auto canonical = WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState());
        BOOST_REQUIRE(canonical);
        BOOST_REQUIRE_EQUAL(canonical->open_vault_principal, 1000);
        return Tip();
    }
};

struct OpenGlobalRedemptionStatsIndex {
    explicit OpenGlobalRedemptionStatsIndex(node::NodeContext& node)
    {
        BOOST_REQUIRE(!g_digidollar_stats_index);
        g_digidollar_stats_index = std::make_unique<DigiDollarStatsIndex>(interfaces::MakeChain(node), 1 << 20, false, false);
    }
    ~OpenGlobalRedemptionStatsIndex()
    {
        SyncWithValidationInterfaceQueue();
        g_digidollar_stats_index->Interrupt();
        g_digidollar_stats_index->Stop();
        g_digidollar_stats_index.reset();
    }
};

void CheckLegacyUncountableSupply(ThawMinerERRValidationSetup& fixture, bool duplicate_metadata,
                                 bool cross_thaw = false, bool cross_while_open = false)
{
    struct RestoreHeight {
        int saved;
        RestoreHeight()
        {
            LOCK(cs_main);
            auto& height = const_cast<Consensus::Params&>(Params().GetConsensus()).nDDThawDayHeight;
            saved = height;
            height += 100;
        }
        ~RestoreHeight()
        {
            LOCK(cs_main);
            const_cast<Consensus::Params&>(Params().GetConsensus()).nDDThawDayHeight = saved;
        }
    } restore;
    constexpr CAmount fee_value{2 * COIN};
    constexpr CAmount fee{COIN};
    const auto funding = fixture.ConfirmOpTrueFunding(fee_value);
    fixture.InstallMuSig2OraclePrice(fixture.REFERENCE_PRICE, fixture.NextBlockHeight());
    const auto redemption = fixture.BuildMatureRedemption(funding, fee_value, fee, 100,
                                                         duplicate_metadata, duplicate_metadata);
    const auto replacement = fixture.BuildMatureRedemption(funding, fee_value, fee, 100);
    auto& chain = fixture.m_node.chainman->ActiveChainstate();
    const auto* parent = WITH_LOCK(cs_main, return chain.m_chain.Tip());
    BOOST_REQUIRE(parent);
    BOOST_REQUIRE_LT(fixture.NextBlockHeight(), Params().GetConsensus().nDDThawDayHeight);

    const auto call = [&](const std::string& method, const std::string& argument = {}) {
        JSONRPCRequest request;
        request.context = &fixture.m_node;
        request.strMethod = method;
        request.params = UniValue(UniValue::VARR);
        if (!argument.empty()) request.params.push_back(argument);
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        return tableRPC.execute(request);
    };
    const auto unavailable = [&](const UniValue& error) {
        const bool thaw = WITH_LOCK(cs_main, return DigiDollar::IsThawDayActive(Params().GetConsensus(), chain.m_chain.Height()));
        const std::string expected = thaw ? "DigiDollar supply not ready: invalid creating token metadata" :
                                           "DigiDollar circulating supply is unavailable from retained metadata";
        return error["message"].get_str() == expected;
    };
    const DigiDollar::CanonicalTxLookup lookup = [&](const uint256& txid, uint32_t height, CTransactionRef& tx) {
        LOCK(cs_main);
        const auto* tip = chain.m_chain.Tip();
        if (height > static_cast<uint32_t>(tip->nHeight)) return false;
        const auto* source = tip->GetAncestor(height);
        CBlock block;
        if (!source || !chain.m_blockman.ReadBlockFromDisk(block, *source)) return false;
        for (const auto& candidate : block.vtx) {
            if (candidate->GetHash() == txid) { tx = candidate; return true; }
        }
        return false;
    };
    const auto check_unknown = [&] {
        auto& index = *g_digidollar_stats_index;
        BOOST_REQUIRE(index.BlockUntilSyncedToCurrentChain());
        const auto* tip = WITH_LOCK(cs_main, return chain.m_chain.Tip());
        BOOST_CHECK(index.GetSummary().synced);
        BOOST_CHECK(index.GetSummary().best_block_hash == tip->GetBlockHash());
        BOOST_CHECK(!index.LookUpStats(*tip));
        const auto canonical = WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState());
        BOOST_CHECK_EXCEPTION(call("getdigidollarstats"), UniValue, unavailable);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
        if (DigiDollar::IsThawDayActive(Params().GetConsensus(), tip->nHeight)) {
            BOOST_REQUIRE(canonical);
            BOOST_CHECK_EQUAL(canonical->open_vault_principal, 100);
            BOOST_CHECK(canonical->history_checked);
        }
        const auto earlier = index.LookUpStats(*parent);
        BOOST_REQUIRE(earlier);
        BOOST_CHECK_EQUAL(earlier->total_dd_supply, 200);
    };
    const CBlockIndex* unknown_block{nullptr};
    const CBlockIndex* unknown_descendant{nullptr};
    {
        OpenGlobalRedemptionStatsIndex opened{fixture.m_node};
        auto& index = *g_digidollar_stats_index;
        BOOST_REQUIRE(index.Init());
        BOOST_REQUIRE(index.StartBackgroundSync());
        IndexWaitSynced(index);
        BOOST_CHECK_EQUAL(call("getdigidollarstats")["total_dd_supply"].getInt<int64_t>(), 200);
        const auto accepted = fixture.CreateAndProcessBlock({CMutableTransaction{*redemption}}, CScript() << OP_TRUE);
        unknown_block = WITH_LOCK(cs_main, return chain.m_chain.Tip());
        BOOST_REQUIRE(unknown_block->GetBlockHash() == accepted.GetHash());
        check_unknown();
        {
            LOCK(cs_main);
            // Canonical vault health does not require the uncountable token amount.
            DigiDollar::ChainstateHealth health;
            std::string error;
            BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(chain.CoinsTip(), Params().GetConsensus(),
                                                                         lookup, health, error), error);
            BOOST_CHECK_EQUAL(health.open_vault_principal, 100);
            BOOST_CHECK_EQUAL(health.collateral, 15 * COIN);
            BOOST_CHECK_EQUAL(health.active_vaults, 1U);
            const auto saved_health = health;
            CAmount supply{12345};
            BOOST_CHECK(!DigiDollar::ReconstructChainstateHealth(chain.CoinsTip(), Params().GetConsensus(),
                                                               lookup, health, error, {}, &supply));
            BOOST_CHECK_EQUAL(supply, 12345);
            BOOST_CHECK(health == saved_health);
            bool supply_known{true};
            BOOST_REQUIRE(DigiDollar::ReconstructChainstateHealth(chain.CoinsTip(), Params().GetConsensus(),
                lookup, health, error, {}, &supply, {}, &supply_known));
            BOOST_CHECK(!supply_known);
            BOOST_CHECK_EQUAL(supply, 12345);
            BOOST_CHECK(health == saved_health);

            // Even after unknown metadata, a later unreadable source remains an error.
            CMutableTransaction inspect;
            inspect.vin.emplace_back(COutPoint{redemption->GetHash(), 2});
            std::vector<Coin> inputs{Coin{redemption->vout[2], unknown_block->nHeight, false}};
            CAmount delta{54321};
            BOOST_CHECK(DigiDollar::GetCirculatingSupplyChange(CTransaction{inspect}, inputs, unknown_block->nHeight + 1,
                Params().GetConsensus(), lookup, delta, error) == DigiDollar::SupplyChangeResult::UNKNOWN_METADATA);
            BOOST_CHECK_EQUAL(delta, 54321);
            BOOST_CHECK(!DigiDollar::CalculateCirculatingSupplyChange(CTransaction{inspect}, inputs, unknown_block->nHeight + 1,
                Params().GetConsensus(), lookup, delta, error));
            BOOST_CHECK_EQUAL(delta, 54321);
            inspect.vin.emplace_back(COutPoint{fixture.other_mint->GetHash(), 1});
            inputs.emplace_back(fixture.other_mint->vout[1], fixture.redemption_vault.lockHeight - DigiDollar::LockDaysToBlocks(0), false);
            const DigiDollar::CanonicalTxLookup missing_source = [&](const uint256& hash, uint32_t height, CTransactionRef& tx) {
                return hash != fixture.other_mint->GetHash() && lookup(hash, height, tx);
            };
            BOOST_CHECK(DigiDollar::GetCirculatingSupplyChange(CTransaction{inspect}, inputs, unknown_block->nHeight + 1,
                Params().GetConsensus(), missing_source, delta, error) == DigiDollar::SupplyChangeResult::FAILURE);
            BOOST_CHECK_EQUAL(delta, 54321);
            BOOST_CHECK(error.find("restore the creating block") != std::string::npos);
        }
        const auto descendant = fixture.CreateAndProcessBlock({}, CScript() << OP_TRUE);
        unknown_descendant = WITH_LOCK(cs_main, return chain.m_chain.Tip());
        BOOST_REQUIRE(unknown_descendant->GetBlockHash() == descendant.GetHash());
        check_unknown();
        chain.ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        index.Stop();
    }
    if (cross_thaw && !cross_while_open) {
        fixture.mineBlocks(Params().GetConsensus().nDDThawDayHeight - fixture.NextBlockHeight() + 1);
        BOOST_REQUIRE_EQUAL(fixture.NextBlockHeight(), Params().GetConsensus().nDDThawDayHeight + 1);
    }
    {
        OpenGlobalRedemptionStatsIndex reopened{fixture.m_node};
        auto& index = *g_digidollar_stats_index;
        BOOST_REQUIRE(index.Init());
        BOOST_CHECK(index.GetSummary().best_block_hash == unknown_descendant->GetBlockHash());
        BOOST_REQUIRE(index.StartBackgroundSync());
        IndexWaitSynced(index);
        if (cross_while_open) {
            fixture.mineBlocks(Params().GetConsensus().nDDThawDayHeight - fixture.NextBlockHeight() + 1);
            BOOST_REQUIRE_EQUAL(fixture.NextBlockHeight(), Params().GetConsensus().nDDThawDayHeight + 1);
        }
        check_unknown();
        call("invalidateblock", unknown_block->GetBlockHash().GetHex());
        BOOST_REQUIRE(WITH_LOCK(cs_main, return chain.m_chain.Tip()) == parent);
        fixture.InstallMuSig2OraclePrice(fixture.REFERENCE_PRICE, fixture.NextBlockHeight());
        const auto known = fixture.CreateAndProcessBlock({CMutableTransaction{*replacement}}, CScript() << OP_TRUE);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chain.m_chain.Tip()->GetBlockHash()), known.GetHash());
        BOOST_REQUIRE(index.BlockUntilSyncedToCurrentChain());
        const auto stats = index.LookUpStats(*WITH_LOCK(cs_main, return chain.m_chain.Tip()));
        BOOST_REQUIRE(stats);
        BOOST_CHECK_EQUAL(stats->total_dd_supply, 100);
        BOOST_CHECK_EQUAL(stats->total_collateral, 15 * COIN);
        BOOST_CHECK_EQUAL(stats->vault_count, 1U);
        BOOST_CHECK_EQUAL(call("getdigidollarstats")["total_dd_supply"].getInt<int64_t>(), 100);
        BOOST_CHECK(!index.LookUpStats(*unknown_block));
        fixture.CreateAndProcessBlock({}, CScript() << OP_TRUE);
        BOOST_REQUIRE(index.BlockUntilSyncedToCurrentChain());
        BOOST_CHECK(!index.LookUpStats(*unknown_descendant));
        chain.ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        index.Stop();
    }
    {
        OpenGlobalRedemptionStatsIndex reopened{fixture.m_node};
        auto& index = *g_digidollar_stats_index;
        BOOST_REQUIRE(index.Init());
        BOOST_REQUIRE(index.StartBackgroundSync());
        IndexWaitSynced(index);
        BOOST_CHECK_EQUAL(call("getdigidollarstats")["total_dd_supply"].getInt<int64_t>(), 100);
        BOOST_CHECK(!index.LookUpStats(*unknown_block));
        BOOST_CHECK(!index.LookUpStats(*unknown_descendant));
    }
}

// Simulate unavailable local block data without changing any block or data file.
struct UnavailableAncestorData {
    CBlockIndex& index;
    unsigned int saved_position;

    explicit UnavailableAncestorData(CBlockIndex& ancestor) : index(ancestor)
    {
        LOCK(cs_main);
        saved_position = index.nDataPos;
        index.nDataPos = std::numeric_limits<unsigned int>::max();
    }

    ~UnavailableAncestorData()
    {
        LOCK(cs_main);
        index.nDataPos = saved_position;
    }
};

bool IsReferenceReadinessError(const std::runtime_error& error)
{
    return std::string(error.what()).find("DigiDollar volatility state not ready: restore or download ancestor") != std::string::npos;
}

} // namespace

BOOST_AUTO_TEST_SUITE(miner_dd_validation_tests)

BOOST_FIXTURE_TEST_CASE(block_includes_non_dd_op_return_dd_payload_without_oracle, MinerDDValidationSetup)
{
    constexpr CAmount kInput = 2 * COIN;
    constexpr CAmount kFee = 1000;

    const COutPoint funding = ConfirmOpTrueFunding(kInput);
    const CTransactionRef tx = BuildOpTrueSpendWithDataCarrier(
        funding,
        kInput,
        kFee,
        std::vector<unsigned char>{'D', 'D'});

    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*tx));
    OracleBundleManager::GetInstance().Clear();

    AddToMempool(tx, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_REQUIRE_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK_MESSAGE(BlockHasTx(block_template->block, tx->GetHash()),
        "ordinary DGB transaction with OP_RETURN \"DD\" payload must not require a DigiDollar oracle bundle");
}

BOOST_FIXTURE_TEST_CASE(block_skips_stale_dd_mint, MinerDDValidationSetup)
{
    constexpr CAmount kHighPrice = 50000; // $0.05
    constexpr CAmount kLowPrice = 45000;  // $0.045
    constexpr CAmount kDDAmount = 10000;  // $100.00
    constexpr CAmount kFee = 1000;

    const CAmount required_high = RequiredCollateralAt(kDDAmount, /*lock_days=*/30, NextBlockHeight(), kHighPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required_high + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef stale_mint = BuildDDMint(funding, required_high + kFee, required_high, kDDAmount, next_height, kFee);

    std::string reject_low;
    BOOST_REQUIRE(ValidateMintAtPrice(*stale_mint, next_height, kHighPrice));
    BOOST_REQUIRE(!ValidateMintAtPrice(*stale_mint, next_height, kLowPrice, &reject_low));
    BOOST_CHECK_EQUAL(reject_low, "insufficient-collateral");

    InstallMuSig2OraclePrice(kLowPrice, next_height);
    AddToMempool(stale_mint, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(!BlockHasTx(block_template->block, stale_mint->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(block_succeeds_without_dd_after_failure, MinerDDValidationSetup)
{
    constexpr CAmount kHighPrice = 50000;
    constexpr CAmount kLowPrice = 45000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;
    constexpr CAmount kStdInput = 2 * COIN;

    const CAmount required_high = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kHighPrice);
    const COutPoint dd_funding = ConfirmOpTrueFunding(required_high + kFee);
    const COutPoint std_funding = ConfirmOpTrueFunding(kStdInput);

    const int next_height = NextBlockHeight();
    const CTransactionRef dd_mint = BuildDDMint(dd_funding, required_high + kFee, required_high, kDDAmount, next_height, kFee);
    const CTransactionRef std_tx = BuildOpTrueSpend(std_funding, kStdInput, kFee);

    AddToMempool(dd_mint, kFee);
    AddToMempool(std_tx, kFee);
    InstallMuSig2OraclePrice(kLowPrice, next_height);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_REQUIRE_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, std_tx->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, dd_mint->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(mint_includes_safety_margin, RegTestingSetup)
{
    constexpr CAmount kOraclePrice = 50000; // $0.05
    constexpr CAmount kDDAmount = 10000;    // $100
    constexpr int kLockDays = 30;

    DigiDollar::MintTxBuilder builder(Params(), /*height=*/1000, kOraclePrice);
    const CAmount with_margin = builder.CalculateRequiredCollateral(kDDAmount, kLockDays);

    const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(kLockDays);
    const auto& dd_params = Params().GetDigiDollarParams();
    const int base_ratio = DigiDollar::GetCollateralRatioForLockTime(lock_blocks, dd_params);

    __int128 numerator = static_cast<__int128>(kDDAmount) *
                         static_cast<__int128>(COIN) *
                         static_cast<__int128>(base_ratio) * 100;
    const CAmount base_required = static_cast<CAmount>(numerator / static_cast<__int128>(kOraclePrice));
    const CAmount expected_with_margin = static_cast<CAmount>((static_cast<__int128>(base_required) * 101) / 100);

    BOOST_CHECK_EQUAL(with_margin, expected_with_margin);
    BOOST_CHECK(with_margin > base_required);
}

BOOST_FIXTURE_TEST_CASE(addPackageTxs_skips_invalid_dd, MinerDDValidationSetup)
{
    constexpr CAmount kPrice = 45000;   // $0.045
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    BOOST_REQUIRE(required > COIN);
    const CAmount insufficient_collateral = required - COIN;

    const COutPoint funding = ConfirmOpTrueFunding(insufficient_collateral + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef invalid_mint = BuildDDMint(funding, insufficient_collateral + kFee, insufficient_collateral, kDDAmount, next_height, kFee);
    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(invalid_mint, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(!BlockHasTx(block_template->block, invalid_mint->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(test_block_validity_uses_musig2_price, MinerDDValidationSetup)
{
    constexpr CAmount kHighPrice = 50000;
    constexpr CAmount kLowPrice = 45000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required_high = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kHighPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required_high + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef borderline_mint = BuildDDMint(funding, required_high + kFee, required_high, kDDAmount, next_height, kFee);

    std::string reject_low;
    BOOST_REQUIRE(ValidateMintAtPrice(*borderline_mint, next_height, kHighPrice));
    BOOST_REQUIRE(!ValidateMintAtPrice(*borderline_mint, next_height, kLowPrice, &reject_low));
    BOOST_CHECK_EQUAL(reject_low, "insufficient-collateral");

    AddToMempool(borderline_mint, kFee);
    InstallMuSig2OraclePrice(kLowPrice, next_height);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_REQUIRE_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(!BlockHasTx(block_template->block, borderline_mint->GetHash()));
    BOOST_CHECK_EQUAL(block_template->block.vtx.size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(block_skips_unconfirmed_dd_transfer_chains, MinerDDValidationSetup)
{
    // DD transfers are confirmed-only (RC32): transfers spending unconfirmed
    // mempool DD outputs cannot resolve their input DD amounts and are
    // rejected with dd-input-amounts-unknown. The mint has no DD inputs so
    // it still qualifies. Only the mint should land in the block.
    //
    // Regression: addPackageTxs must erase mapModifiedTx entries whose trigger
    // tx fails ValidateDDForBlockInclusion. After the mint is selected,
    // UpdatePackagesForAdded enqueues its unconfirmed DD descendants into
    // mapModifiedTx. Each descendant fails DD validation. Without the erase,
    // the same mapModifiedTx entry is reselected forever and CreateNewBlock
    // never returns (previously observed writing ~250GB of debug.log before
    // the disk filled).
    constexpr CAmount kPrice = 50000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef mint = BuildDDMint(funding, required + kFee, required, kDDAmount, next_height, kFee);

    std::string reject_reason;
    BOOST_REQUIRE(ValidateMintAtPrice(*mint, next_height, kPrice, &reject_reason));

    CKey transfer1_key;
    transfer1_key.MakeNewKey(true);
    const XOnlyPubKey transfer1_xonly(transfer1_key.GetPubKey());

    CKey transfer2_key;
    transfer2_key.MakeNewKey(true);
    const XOnlyPubKey transfer2_xonly(transfer2_key.GetPubKey());

    const COutPoint mint_dd_out(mint->GetHash(), 1);
    const CTransactionRef transfer1 = BuildDDTransfer(mint_dd_out, transfer1_xonly, kDDAmount);
    const COutPoint transfer1_dd_out(transfer1->GetHash(), 0);
    const CTransactionRef transfer2 = BuildDDTransfer(transfer1_dd_out, transfer2_xonly, kDDAmount);

    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(mint, kFee);
    AddToMempool(transfer1, kFee);
    AddToMempool(transfer2, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, mint->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, transfer1->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, transfer2->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(block_skips_non_dd_child_spending_unconfirmed_mint_collateral, MinerDDValidationSetup)
{
    constexpr CAmount kPrice = 50000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef mint = BuildDDMint(funding, required + kFee, required, kDDAmount, next_height, kFee);

    CMutableTransaction child;
    child.nVersion = 2;
    child.vin.emplace_back(COutPoint(mint->GetHash(), 0));
    child.vout.emplace_back(required - kFee, CScript() << OP_TRUE);
    const CTransactionRef non_dd_child = MakeTransactionRef(child);

    BOOST_REQUIRE(ValidateMintAtPrice(*mint, next_height, kPrice));
    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*non_dd_child));

    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(mint, kFee);
    AddToMempool(non_dd_child, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, mint->GetHash()));
    BOOST_CHECK_MESSAGE(!BlockHasTx(block_template->block, non_dd_child->GetHash()),
        "Miner must not include a non-DD child that spends an unconfirmed "
        "DigiDollar mint collateral output without the required redemption burn.");
}

BOOST_FIXTURE_TEST_CASE(block_skips_descendant_of_skipped_non_dd_collateral_child, MinerDDValidationSetup)
{
    constexpr CAmount kPrice = 50000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef mint = BuildDDMint(funding, required + kFee, required, kDDAmount, next_height, kFee);

    CMutableTransaction child;
    child.nVersion = 2;
    child.vin.emplace_back(COutPoint(mint->GetHash(), 0));
    child.vout.emplace_back(required - kFee, CScript() << OP_TRUE);
    const CTransactionRef non_dd_child = MakeTransactionRef(child);

    const CTransactionRef grandchild = BuildOpTrueSpend(
        COutPoint(non_dd_child->GetHash(), 0),
        required - kFee,
        kFee);

    BOOST_REQUIRE(ValidateMintAtPrice(*mint, next_height, kPrice));
    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*non_dd_child));
    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*grandchild));

    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(mint, kFee);
    AddToMempool(non_dd_child, kFee);
    AddToMempool(grandchild, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_CHECK_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, mint->GetHash()));
    BOOST_CHECK_MESSAGE(!BlockHasTx(block_template->block, non_dd_child->GetHash()),
        "Miner must skip the non-DD child that spends unconfirmed DigiDollar "
        "collateral without the required redemption burn.");
    BOOST_CHECK_MESSAGE(!BlockHasTx(block_template->block, grandchild->GetHash()),
        "Miner must also skip descendants of a skipped collateral-spend child; "
        "otherwise block template validity can fail with a missing in-block input.");
}

// Regression: the miner previously skipped AddOracleBundleToBlock() for blocks
// with no DigiDollar transactions. This created a bootstrapping deadlock:
//   - Minting requires an on-chain oracle price (OP_CHECKPRICE fails closed)
//   - Oracle bundles only appeared in DD-touching blocks
//   - No DD txs could form → no DD-touching blocks → no bundle → no price → loop
//
// Fix: call AddOracleBundleToBlock() for ALL DigiDollar-active blocks.
// When a completed MuSig2 session is available, the bundle stamps the price
// on-chain even in plain (non-DD) blocks, breaking the circular dependency.
BOOST_FIXTURE_TEST_CASE(oracle_bundle_stamped_in_non_dd_blocks, MinerDDValidationSetup)
{
    const int next_height = NextBlockHeight();
    InstallMuSig2OraclePrice(5000, next_height);

    // Mine a block with NO DigiDollar transactions in the mempool.
    BlockAssembler::Options opts;
    opts.test_block_validity = false;
    auto tmpl = BuildTemplate(opts);
    BOOST_REQUIRE(tmpl);
    const CTransaction& coinbase = *tmpl->block.vtx[0];

    bool found_oracle_output = false;
    for (const CTxOut& out : coinbase.vout) {
        const CScript& s = out.scriptPubKey;
        if (s.size() >= 2 && s[0] == OP_RETURN && s[1] == 0xbf) {
            found_oracle_output = true;
            break;
        }
    }

    BOOST_CHECK_MESSAGE(found_oracle_output,
        "Non-DD block must carry the oracle bundle when a completed MuSig2 "
        "session is available — required for price-cache bootstrapping");
}



BOOST_FIXTURE_TEST_CASE(next_block_quote_uses_supplied_epoch_parameters, MinerDDValidationSetup)
{
    const int height = NextBlockHeight();
    auto params = Params().GetConsensus();
    params.nDDOracleEpochBlocks = 1;
    COracleBundle bundle(height);
    bundle.version = 3;
    bundle.median_price_micro_usd = 50000;
    bundle.timestamp = GetTime();
    std::vector<uint8_t> oracle_ids;
    for (int id = 0; id < params.nOracleConsensusRequired; ++id) {
        oracle_ids.push_back(static_cast<uint8_t>(id));
    }
    BOOST_REQUIRE(SignRegtestV03Bundle(bundle, oracle_ids));
    std::string error;
    BOOST_REQUIRE_MESSAGE(OracleBundleManager::ValidateMuSig2Bundle(bundle, height, params, error), error);
    BOOST_REQUIRE(OracleBundleManager::GetInstance().UpdateBundle(bundle));
    CAmount quote{0};
    const bool ready = WITH_LOCK(cs_main, return DigiDollar::GetNextBlockOracleQuote(
        m_node.chainman->ActiveChain().Tip(), params, m_node.chainman->m_blockman, quote, error));
    BOOST_REQUIRE_MESSAGE(ready, error);
    BOOST_CHECK_EQUAL(quote, 50000);
}

BOOST_FIXTURE_TEST_CASE(thaw_miner_omits_boundary_mint_and_change_descendant, ThawMinerValidationSetup)
{
    const auto txs = PrepareCandidate(60000);
    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    const auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.mint->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.child->GetHash()));
    BOOST_CHECK(BlockHasTx(block_template->block, txs.plain->GetHash()));
    BOOST_CHECK_EQUAL(block_template->vTxFees[0], -PLAIN_FEE);
}

BOOST_FIXTURE_TEST_CASE(thaw_miner_keeps_eligible_mint_despite_legacy_freeze, ThawMinerValidationSetup)
{
    const auto txs = PrepareCandidate(59999);
    DigiDollar::Volatility::VolatilityMonitor::TriggerFreeze(true, NextBlockHeight() - 1);
    BOOST_REQUIRE(DigiDollar::Volatility::VolatilityMonitor::ShouldFreezeAll());
    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    const auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, txs.mint->GetHash()));
    BOOST_CHECK(BlockHasTx(block_template->block, txs.child->GetHash()));
    BOOST_CHECK(BlockHasTx(block_template->block, txs.plain->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(thaw_miner_retries_committed_quote_failure_from_parent, ThawMinerValidationSetup)
{
    const auto txs = PrepareCandidate(REFERENCE_PRICE);
    const uint256 parent_hash = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    int hook_calls{0};
    options.on_before_test_block_validity = [&](CBlock& block) {
        ++hook_calls;
        BOOST_REQUIRE(BlockHasTx(block, txs.mint->GetHash()));
        BOOST_REQUIRE(BlockHasTx(block, txs.child->GetHash()));
        BOOST_REQUIRE(BlockHasTx(block, txs.plain->GetHash()));
        ReplaceCommittedPrice(block, 60000);
    };
    const auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK_EQUAL(hook_calls, 1);
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.mint->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.child->GetHash()));
    BOOST_CHECK(BlockHasTx(block_template->block, txs.plain->GetHash()));
    BOOST_CHECK_EQUAL(block_template->block.vtx.size(), 2U);
    BOOST_CHECK_EQUAL(block_template->vTxFees.size(), block_template->block.vtx.size());
    BOOST_CHECK_EQUAL(block_template->vTxSigOpsCost.size(), block_template->block.vtx.size());
    BOOST_CHECK_EQUAL(block_template->vTxFees[0], -PLAIN_FEE);
    BOOST_CHECK_EQUAL(block_template->block.vtx[0]->vout[0].nValue,
                      GetBlockSubsidy(NextBlockHeight(), Params().GetConsensus()) + PLAIN_FEE);
    BOOST_CHECK_EQUAL(block_template->block.hashMerkleRoot, BlockMerkleRoot(block_template->block));
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()), parent_hash);
}

BOOST_FIXTURE_TEST_CASE(thaw_miner_retries_ordered_redemption_then_mint, ThawMinerERRValidationSetup)
{
    constexpr CAmount redemption_fee{COIN};
    const auto fee_input = ConfirmOpTrueFunding(2 * COIN);
    const auto txs = PrepareCandidate(REFERENCE_PRICE);
    const auto redemption = BuildMatureRedemption(fee_input, 2 * COIN, redemption_fee);
    AddToMempool(redemption, redemption_fee);
    const auto parent_state = WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState());
    BOOST_REQUIRE(parent_state);
    const auto health = DigiDollar::CalculateChainstateHealth(*parent_state, REFERENCE_PRICE);
    BOOST_REQUIRE(health);
    BOOST_REQUIRE_EQUAL(*health, 112);
    BOOST_REQUIRE_EQUAL(parent_state->open_vault_principal, 200);
    BOOST_REQUIRE_EQUAL(parent_state->collateral, 45 * COIN);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    int hook_calls{0};
    options.on_before_test_block_validity = [&](CBlock& block) {
        ++hook_calls;
        // Both transactions pass selection against the same parent. The high-fee
        // normal redemption runs first and leaves the remaining vault at 75%.
        BOOST_REQUIRE_EQUAL(block.vtx.size(), 5U);
        BOOST_REQUIRE_EQUAL(block.vtx[1]->GetHash(), redemption->GetHash());
        BOOST_REQUIRE(BlockHasTx(block, txs.mint->GetHash()));
        BOOST_REQUIRE(BlockHasTx(block, txs.child->GetHash()));
        BOOST_REQUIRE(BlockHasTx(block, txs.plain->GetHash()));
        BlockValidationState state;
        BOOST_REQUIRE(!TestBlockValidity(state, Params(), m_node.chainman->ActiveChainstate(), block,
            m_node.chainman->ActiveChain().Tip(), GetAdjustedTime, false, false));
        BOOST_REQUIRE_EQUAL(state.GetRejectReason(), "minting-blocked-during-err");
        BOOST_REQUIRE(!state.IsError());
    };
    const auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK_EQUAL(hook_calls, 1);
    BOOST_CHECK(BlockHasTx(block_template->block, redemption->GetHash()));
    BOOST_CHECK(BlockHasTx(block_template->block, txs.plain->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.mint->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.child->GetHash()));
    BOOST_CHECK_EQUAL(block_template->block.vtx.size(), 3U);
    BOOST_CHECK_EQUAL(block_template->vTxFees.size(), block_template->block.vtx.size());
    BOOST_CHECK_EQUAL(block_template->vTxSigOpsCost.size(), block_template->block.vtx.size());
    BOOST_CHECK_EQUAL(block_template->vTxFees[0], -(redemption_fee + PLAIN_FEE));
    BOOST_CHECK_EQUAL(block_template->block.vtx[0]->vout[0].nValue,
                      GetBlockSubsidy(NextBlockHeight(), Params().GetConsensus()) + redemption_fee + PLAIN_FEE);
    BOOST_CHECK_EQUAL(block_template->block.hashMerkleRoot, BlockMerkleRoot(block_template->block));
    LOCK(cs_main);
    auto& chain = m_node.chainman->ActiveChainstate();
    BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == parent_state);
    BOOST_CHECK_EQUAL(chain.m_chain.Tip()->GetBlockHash(), block_template->block.hashPrevBlock);
    BOOST_CHECK(chain.CoinsTip().HaveCoin(COutPoint{redeemable_mint->GetHash(), 0}));
    BlockValidationState remaining_state;
    BOOST_CHECK_MESSAGE(TestBlockValidity(remaining_state, Params(), chain, block_template->block,
        chain.m_chain.Tip(), GetAdjustedTime, false, false), remaining_state.ToString());
    BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == parent_state);
}

BOOST_FIXTURE_TEST_CASE(thaw_miner_second_retry_removes_redemptions_that_need_an_earlier_mint, ThawMinerERRValidationSetup)
{
    const auto strong_fee_input = ConfirmOpTrueFunding(2 * COIN);
    const auto weak_fee_input = ConfirmOpTrueFunding(2 * COIN);
    const auto txs = PrepareCandidate(REFERENCE_PRICE);
    const auto strong = BuildMatureRedemption(strong_fee_input, 2 * COIN, COIN);
    std::swap(redeemable_mint, other_mint);
    const auto weak = BuildMatureRedemption(weak_fee_input, 2 * COIN, COIN / 2);
    std::swap(redeemable_mint, other_mint);
    AddToMempool(strong, COIN);
    AddToMempool(weak, COIN / 2);
    {
        LOCK(m_node.mempool->cs);
        m_node.mempool->PrioritiseTransaction(txs.mint->GetHash(), 10 * COIN);
    }
    const auto parent = WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState());
    BOOST_REQUIRE(parent);
    BOOST_REQUIRE_EQUAL(parent->open_vault_principal, 200);
    BOOST_REQUIRE_EQUAL(parent->collateral, 45 * COIN);
    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    int hook_calls{0};
    options.on_before_test_block_validity = [&](CBlock& block) {
        ++hook_calls;
        BOOST_REQUIRE_EQUAL(block.vtx[1]->GetHash(), txs.mint->GetHash());
        BOOST_REQUIRE(BlockHasTx(block, strong->GetHash()));
        BOOST_REQUIRE(BlockHasTx(block, weak->GetHash()));
        BlockValidationState state;
        BOOST_REQUIRE_MESSAGE(TestBlockValidity(state, Params(), m_node.chainman->ActiveChainstate(), block,
            m_node.chainman->ActiveChain().Tip(), GetAdjustedTime, false, false), state.ToString());
        // At the committed quote the mint pauses. Removing it leaves the first
        // redemption healthy, then the second sees only the 90% weak vault.
        ReplaceCommittedPrice(block, 60000);
    };
    const auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK_EQUAL(hook_calls, 1);
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.mint->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, txs.child->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, strong->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, weak->GetHash()));
    BOOST_CHECK(BlockHasTx(block_template->block, txs.plain->GetHash()));
    BOOST_CHECK_EQUAL(block_template->block.vtx.size(), 2U);
    BOOST_CHECK_EQUAL(block_template->vTxFees.size(), block_template->block.vtx.size());
    BOOST_CHECK_EQUAL(block_template->vTxSigOpsCost.size(), block_template->block.vtx.size());
    BOOST_CHECK_EQUAL(block_template->vTxFees[0], -PLAIN_FEE);
    BOOST_CHECK_EQUAL(block_template->block.vtx[0]->vout[0].nValue,
                      GetBlockSubsidy(NextBlockHeight(), Params().GetConsensus()) + PLAIN_FEE);
    BOOST_CHECK_EQUAL(block_template->block.hashMerkleRoot, BlockMerkleRoot(block_template->block));
    BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState()) == parent);
}

BOOST_FIXTURE_TEST_CASE(thaw_redemption_change_is_countable_through_index_reopen, ThawMinerERRValidationSetup)
{
    constexpr CAmount fee_value{2 * COIN};
    constexpr CAmount fee{COIN};
    const auto fee_input = ConfirmOpTrueFunding(fee_value);
    InstallMuSig2OraclePrice(REFERENCE_PRICE, NextBlockHeight());
    const auto missing = BuildMatureRedemption(fee_input, fee_value, fee, 100, false);
    const auto ambiguous = BuildMatureRedemption(fee_input, fee_value, fee, 100, true, true);
    const auto valid = BuildMatureRedemption(fee_input, fee_value, fee, 100);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto parent_health = WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState());
    BOOST_REQUIRE(parent_health);
    const auto parent_hash = WITH_LOCK(cs_main, return chain.m_chain.Tip()->GetBlockHash());

    auto check_supply = [&](const DigiDollarStatsIndex& index, CAmount supply, CAmount collateral, uint64_t vaults) {
        LOCK(cs_main);
        const auto* tip = chain.m_chain.Tip();
        const auto stats = index.LookUpStats(*tip);
        BOOST_REQUIRE(stats);
        BOOST_CHECK_EQUAL(stats->total_dd_supply, supply);
        BOOST_CHECK_EQUAL(stats->total_collateral, collateral);
        BOOST_CHECK_EQUAL(stats->vault_count, vaults);
        const DigiDollar::CanonicalTxLookup lookup = [&](const uint256& hash, uint32_t height, CTransactionRef& tx) {
            if (height > static_cast<uint32_t>(tip->nHeight)) return false;
            const auto* source = tip->GetAncestor(height);
            CBlock block;
            if (!source || !chain.m_blockman.ReadBlockFromDisk(block, *source)) return false;
            for (const auto& candidate : block.vtx) {
                if (candidate->GetHash() == hash) { tx = candidate; return true; }
            }
            return false;
        };
        DigiDollar::ChainstateHealth reconstructed;
        CAmount reconstructed_supply{-1};
        std::string error;
        BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(chain.CoinsTip(), Params().GetConsensus(),
            lookup, reconstructed, error, {}, &reconstructed_supply), error);
        BOOST_CHECK_EQUAL(reconstructed_supply, supply);
        BOOST_CHECK_EQUAL(reconstructed.collateral, collateral);
        BOOST_CHECK_EQUAL(reconstructed.active_vaults, vaults);
    };

    {
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_REQUIRE(opened.index.Init());
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        check_supply(opened.index, 200, 45 * COIN, 2);
        for (const auto& redemption : {missing, ambiguous}) {
            const auto block = CreateBlock({CMutableTransaction{*redemption}}, CScript() << OP_TRUE, chain);
            LOCK(cs_main);
            BlockValidationState rejected;
            BOOST_REQUIRE(!TestBlockValidity(rejected, Params(), chain, block, chain.m_chain.Tip(),
                                             GetAdjustedTime, false, false));
            BOOST_CHECK_EQUAL(rejected.GetRejectReason(), "bad-redeem-dd-output-amount");
            BOOST_CHECK(!rejected.IsError());
            BOOST_CHECK(chain.CoinsTip().GetDigiDollarState() == parent_health);
            BOOST_CHECK(chain.m_chain.Tip()->GetBlockHash() == parent_hash);
            BOOST_CHECK(chain.CoinsTip().HaveCoin(COutPoint{redeemable_mint->GetHash(), 0}));
            BOOST_CHECK(chain.CoinsTip().HaveCoin(COutPoint{other_mint->GetHash(), 1}));

            struct RestoreHeight {
                int& value;
                const int saved;
                ~RestoreHeight() { value = saved; }
            } restore{const_cast<Consensus::Params&>(Params().GetConsensus()).nDDThawDayHeight, THAW_HEIGHT};
            restore.value = NextBlockHeight() + 1;
            const auto legacy_metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();
            BlockValidationState legacy;
            BOOST_REQUIRE_MESSAGE(TestBlockValidity(legacy, Params(), chain, block, chain.m_chain.Tip(),
                                                     GetAdjustedTime, false, false), legacy.ToString());
            BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalDDSupply, legacy_metrics.totalDDSupply);
            BOOST_CHECK_EQUAL(DigiDollar::SystemHealthMonitor::GetCachedMetrics().totalCollateral, legacy_metrics.totalCollateral);
        }
        BOOST_REQUIRE(opened.index.BlockUntilSyncedToCurrentChain());
        check_supply(opened.index, 200, 45 * COIN, 2);

        const auto accepted = CreateAndProcessBlock({CMutableTransaction{*valid}}, CScript() << OP_TRUE);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chain.m_chain.Tip()->GetBlockHash()), accepted.GetHash());
        BOOST_REQUIRE(opened.index.BlockUntilSyncedToCurrentChain());
        check_supply(opened.index, 100, 15 * COIN, 1);
        {
            LOCK(cs_main);
            const auto state = chain.CoinsTip().GetDigiDollarState();
            BOOST_REQUIRE(state);
            BOOST_CHECK_EQUAL(state->open_vault_principal, 100);
            BOOST_CHECK_EQUAL(state->active_vaults, 1U);
            BOOST_CHECK(chain.CoinsTip().HaveCoin(COutPoint{valid->GetHash(), 2}));
        }
        chain.ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        opened.index.Stop();
    }
    {
        OpenRedemptionStatsIndex reopened{m_node};
        BOOST_REQUIRE(reopened.index.Init());
        BOOST_REQUIRE(reopened.index.StartBackgroundSync());
        IndexWaitSynced(reopened.index);
        check_supply(reopened.index, 100, 15 * COIN, 1);
    }
}

BOOST_FIXTURE_TEST_CASE(supply_index_repairs_saved_total_without_replacing_open_principal, SupplyVerificationSetup)
{
    const auto funding = ConfirmOpTrueFunding(2 * COIN);
    InstallMuSig2OraclePrice(REFERENCE_PRICE, NextBlockHeight());
    const auto redemption = BuildMatureRedemption(funding, 2 * COIN, COIN, 750);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto accepted = CreateAndProcessBlock({CMutableTransaction{*redemption}}, CScript() << OP_TRUE);
    const auto* tip = WITH_LOCK(cs_main, return chain.m_chain.Tip());
    BOOST_REQUIRE(tip->GetBlockHash() == accepted.GetHash());
    BOOST_REQUIRE(BlockHasTx(accepted, redemption->GetHash()));
    const auto canonical = WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState());
    BOOST_REQUIRE(canonical);
    BOOST_REQUIRE_EQUAL(canonical->open_vault_principal, 1000);
    BOOST_REQUIRE_EQUAL(canonical->collateral, 150 * COIN);
    {
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_REQUIRE(opened.index.Init());
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        const auto stats = opened.index.LookUpStats(*tip);
        BOOST_REQUIRE(stats);
        BOOST_REQUIRE_EQUAL(stats->total_dd_supply, 750);
        chain.ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        opened.index.Stop();
    }
    const auto path = m_args.GetDataDirNet() / "indexes" / "digidollarstats" / "db";
    {
        CDBWrapper db{{.path = path, .cache_bytes = 1 << 20, .memory_only = false}};
        uint32_t format{0};
        BOOST_REQUIRE(db.Read(uint8_t{'S'}, format));
        BOOST_REQUIRE_EQUAL(format, 3U);
        std::pair<uint256, SavedSupplyValue> row;
        BOOST_REQUIRE(db.Read(SavedSupplyHeightKey{tip->nHeight}, row));
        BOOST_REQUIRE(row.first == tip->GetBlockHash());
        BOOST_REQUIRE_EQUAL(row.second.supply, 750);
        BOOST_REQUIRE(row.second.known);
        row.second.supply = 873;
        BOOST_REQUIRE(db.Write(SavedSupplyHeightKey{tip->nHeight}, row));
    }
    for (int reopen = 0; reopen < 2; ++reopen) {
        OpenRedemptionStatsIndex opened{m_node};
        const auto scans_before = DigiDollar::ChainstateHealthRebuildCount();
        BOOST_REQUIRE(opened.index.Init());
        BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), scans_before + 1);
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        const auto stats = opened.index.LookUpStats(*tip);
        BOOST_REQUIRE(stats);
        BOOST_CHECK_EQUAL(stats->total_dd_supply, 750);
        BOOST_CHECK_EQUAL(stats->total_collateral, 150 * COIN);
        BOOST_CHECK_EQUAL(stats->vault_count, 1U);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.m_chain.Tip()) == tip);
    }
}

BOOST_FIXTURE_TEST_CASE(supply_index_repairs_behind_and_disconnected_saved_rows, SupplyVerificationSetup)
{
    const auto funding = ConfirmOpTrueFunding(2 * COIN);
    auto* prefix = Tip();
    SaveIndex();
    ChangeSavedSupply(*prefix, 873);
    auto* redeemed = AcceptExtraBurn(funding);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto canonical = WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState());
    {
        OpenRedemptionStatsIndex opened{m_node};
        const auto scans_before = DigiDollar::ChainstateHealthRebuildCount();
        BOOST_REQUIRE(opened.index.Init());
        CheckSupply(opened.index, *prefix, 2000, 450 * COIN, 2);
        BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), scans_before + 1);
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        CheckSupply(opened.index, *redeemed, 750, 150 * COIN, 1);
        BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), scans_before + 1);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
        chain.ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        opened.index.Stop();
    }
    // Both the saved fork tip and the row to which sync will rewind are wrong.
    ChangeSavedSupply(*redeemed, 873);
    ChangeSavedSupply(*prefix, 873);
    BlockValidationState disconnected;
    BOOST_REQUIRE(chain.InvalidateBlock(disconnected, redeemed));
    BOOST_REQUIRE(Tip() == prefix);
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    auto* replacement = Tip();
    BOOST_REQUIRE(replacement != redeemed);
    {
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_REQUIRE(opened.index.Init());
        CheckSupply(opened.index, *redeemed, 750, 150 * COIN, 1);
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        CheckSupply(opened.index, *replacement, 2000, 450 * COIN, 2);
        CheckSupply(opened.index, *prefix, 2000, 450 * COIN, 2);
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()->open_vault_principal), 2000);
        // Keep the synced index open: the live callback has a different lock
        // context from the background rewind exercised above.
        const auto scans_before_live_reorg = DigiDollar::ChainstateHealthRebuildCount();
        BlockValidationState live_disconnect;
        BOOST_REQUIRE(chain.InvalidateBlock(live_disconnect, replacement));
        WITH_LOCK(cs_main, chain.ResetBlockFailureFlags(redeemed));
        BlockValidationState live_reconnect;
        BOOST_REQUIRE(chain.ActivateBestChain(live_reconnect));
        BOOST_REQUIRE(Tip() == redeemed);
        BOOST_REQUIRE(opened.index.BlockUntilSyncedToCurrentChain());
        CheckSupply(opened.index, *redeemed, 750, 150 * COIN, 1);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
        BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), scans_before_live_reorg);
        for (int i = 0; i < 3; ++i) {
            CreateAndProcessBlock({}, CScript() << OP_TRUE);
            BOOST_REQUIRE(opened.index.BlockUntilSyncedToCurrentChain());
            CheckSupply(opened.index, *Tip(), 750, 150 * COIN, 1);
        }
        BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), scans_before_live_reorg);
    }
}

BOOST_FIXTURE_TEST_CASE(supply_index_cancellation_does_not_publish_a_repair, SupplyVerificationSetup)
{
    auto* tip = AcceptExtraBurn(ConfirmOpTrueFunding(2 * COIN));
    SaveIndex();
    ChangeSavedSupply(*tip, 873);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto canonical = WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState());
    {
        auto& interrupt = const_cast<util::SignalInterrupt&>(m_node.chainman->m_interrupt);
        struct ResetInterrupt {
            util::SignalInterrupt& value;
            ~ResetInterrupt() { value.reset(); }
        } reset{interrupt};
        DebugLogHelper cancelled{"outputs checked", [&](const std::string* line) {
            if (!line) return true;
            if (line->find("DigiDollar supply verification: ") == std::string::npos ||
                line->find("verification: 0 outputs checked") != std::string::npos) return false;
            interrupt();
            return true;
        }};
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_CHECK(!opened.index.Init());
    }
    {
        CDBWrapper db{{.path = IndexPath(), .cache_bytes = 1 << 20, .memory_only = false}};
        std::pair<uint256, SavedSupplyValue> row;
        BOOST_REQUIRE(db.Read(SavedSupplyHeightKey{tip->nHeight}, row));
        BOOST_CHECK_EQUAL(row.second.supply, 873);
    }
    BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
    OpenRedemptionStatsIndex reopened{m_node};
    BOOST_REQUIRE(reopened.index.Init());
    CheckSupply(reopened.index, *tip, 750, 150 * COIN, 1);
}

BOOST_FIXTURE_TEST_CASE(supply_index_verifies_before_first_activated_burn, SupplyVerificationSetup)
{
    auto& chain = m_node.chainman->ActiveChainstate();
    BlockValidationState disconnected;
    BOOST_REQUIRE(chain.InvalidateBlock(disconnected, Tip()));
    const auto funding = ConfirmOpTrueFunding(2 * COIN);
    auto* prefix = Tip();
    BOOST_REQUIRE_EQUAL(prefix->nHeight, THAW_HEIGHT - 1);
    SaveIndex();
    ChangeSavedSupply(*prefix, 1);
    OpenRedemptionStatsIndex opened{m_node};
    BOOST_REQUIRE(opened.index.Init());
    // The legacy path retains its saved value until H.
    CheckSupply(opened.index, *prefix, 1, 450 * COIN, 2);
    BOOST_REQUIRE(opened.index.StartBackgroundSync());
    IndexWaitSynced(opened.index);
    auto* tip = AcceptExtraBurn(funding);
    BOOST_REQUIRE_EQUAL(tip->nHeight, THAW_HEIGHT);
    BOOST_REQUIRE(opened.index.BlockUntilSyncedToCurrentChain());
    CheckSupply(opened.index, *tip, 750, 150 * COIN, 1);
}

BOOST_FIXTURE_TEST_CASE(supply_index_missing_undo_keeps_the_saved_total_unmodified, SupplyVerificationSetup)
{
    const auto funding = ConfirmOpTrueFunding(2 * COIN);
    auto* prefix = Tip();
    SaveIndex();
    ChangeSavedSupply(*prefix, 873);
    auto* redeemed = AcceptExtraBurn(funding);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto canonical = WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState());
    {
        struct RestoreUndo {
            CBlockIndex& block;
            unsigned int position;
            ~RestoreUndo() { LOCK(cs_main); block.nUndoPos = position; }
        } restore{*redeemed, redeemed->nUndoPos};
        WITH_LOCK(cs_main, redeemed->nUndoPos = 0);
        ASSERT_DEBUG_LOG("restore undo data at height " + std::to_string(redeemed->nHeight));
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_CHECK(!opened.index.Init());
    }
    {
        CDBWrapper db{{.path = IndexPath(), .cache_bytes = 1 << 20, .memory_only = false}};
        std::pair<uint256, SavedSupplyValue> row;
        BOOST_REQUIRE(db.Read(SavedSupplyHeightKey{prefix->nHeight}, row));
        BOOST_CHECK_EQUAL(row.second.supply, 873);
    }
    BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
    OpenRedemptionStatsIndex reopened{m_node};
    BOOST_REQUIRE(reopened.index.Init());
    BOOST_REQUIRE(reopened.index.StartBackgroundSync());
    IndexWaitSynced(reopened.index);
    CheckSupply(reopened.index, *redeemed, 750, 150 * COIN, 1);
}

BOOST_FIXTURE_TEST_CASE(supply_index_old_format_rebuild_preserves_extra_burn, SupplyVerificationSetup)
{
    auto* tip = AcceptExtraBurn(ConfirmOpTrueFunding(2 * COIN));
    SaveIndex();
    ChangeSavedSupply(*tip, 1000);
    {
        CDBWrapper db{{.path = IndexPath(), .cache_bytes = 1 << 20, .memory_only = false}};
        BOOST_REQUIRE(db.Write(uint8_t{'S'}, uint32_t{2}));
        // Deployed format two did not version its keys or store availability.
        BOOST_REQUIRE(db.Write(PreviousSupplyHeightKey{tip->nHeight}, std::make_pair(tip->GetBlockHash(),
            PreviousSupplyValue{1000, 150 * COIN, 1})));
    }
    for (int reopen = 0; reopen < 2; ++reopen) {
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_REQUIRE(opened.index.Init());
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        CheckSupply(opened.index, *tip, 750, 150 * COIN, 1);
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState()->open_vault_principal), 1000);
    }
}

BOOST_FIXTURE_TEST_CASE(supply_index_rejects_readable_current_block_body_corruption, ThawMinerERRValidationSetup)
{
    constexpr CAmount fee_value{2 * COIN};
    constexpr CAmount fee{COIN};
    const auto funding = ConfirmOpTrueFunding(fee_value);
    InstallMuSig2OraclePrice(REFERENCE_PRICE, NextBlockHeight());
    const auto redemption = BuildMatureRedemption(funding, fee_value, fee, 100);
    auto& chain = m_node.chainman->ActiveChainstate();
    const auto* prefix = WITH_LOCK(cs_main, return chain.m_chain.Tip());
    BOOST_REQUIRE(prefix);
    {
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_REQUIRE(opened.index.Init());
        BOOST_REQUIRE(opened.index.StartBackgroundSync());
        IndexWaitSynced(opened.index);
        const auto stats = opened.index.LookUpStats(*prefix);
        BOOST_REQUIRE(stats);
        BOOST_CHECK_EQUAL(stats->total_dd_supply, 200);
        chain.ForceFlushStateToDisk();
        SyncWithValidationInterfaceQueue();
        opened.index.Stop();
    }
    const auto accepted = CreateAndProcessBlock({CMutableTransaction{*redemption}}, CScript() << OP_TRUE);
    auto* current = WITH_LOCK(cs_main, return chain.m_chain.Tip());
    BOOST_REQUIRE(current->GetBlockHash() == accepted.GetHash());
    BOOST_REQUIRE(current->pprev == prefix);
    const auto canonical = WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState());
    BOOST_REQUIRE(canonical);
    BOOST_CHECK_EQUAL(canonical->open_vault_principal, 100);

    CBlock damaged = accepted;
    BOOST_REQUIRE_EQUAL(damaged.vtx.size(), 2U);
    CMutableTransaction altered{*damaged.vtx[1]};
    BOOST_REQUIRE_EQUAL(altered.vout.size(), 4U);
    altered.vout[3].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                            << CScriptNum(3) << CScriptNum(101);
    damaged.vtx[1] = MakeTransactionRef(altered);
    BOOST_REQUIRE(damaged.GetHash() == accepted.GetHash());
    BOOST_REQUIRE(BlockMerkleRoot(damaged) != damaged.hashMerkleRoot);
    {
        struct RestorePhysicalFailure {
            std::atomic<int>& exit_status;
            const int saved_status;
            ~RestorePhysicalFailure()
            {
                AbortShutdown();
                exit_status.store(saved_status);
                SetMiscWarning({});
            }
        } failure{m_node.exit_status, m_node.exit_status.load()};
        BOOST_REQUIRE(!ShutdownRequested());
        struct RestorePosition {
            CBlockIndex& block;
            const int file;
            const unsigned int position;
            ~RestorePosition()
            {
                LOCK(cs_main);
                block.nFile = file;
                block.nDataPos = position;
            }
        } restore{*current, current->nFile, current->nDataPos};
        OpenRedemptionStatsIndex opened{m_node};
        BOOST_REQUIRE(opened.index.Init());
        BOOST_CHECK(opened.index.GetSummary().best_block_hash == prefix->GetBlockHash());
        {
            LOCK(cs_main);
            // Append only to fixture-owned files; retain the original accepted bytes.
            const auto position = chain.m_blockman.SaveBlockToDisk(damaged, current->nHeight, nullptr);
            BOOST_REQUIRE(!position.IsNull());
            current->nFile = position.nFile;
            current->nDataPos = position.nPos;
            CBlock readable;
            BOOST_REQUIRE(chain.m_blockman.ReadBlockFromDisk(readable, *current));
            BOOST_CHECK(readable.GetHash() == current->GetBlockHash());
            BOOST_CHECK(BlockMerkleRoot(readable) != readable.hashMerkleRoot);
        }
        {
            ASSERT_DEBUG_LOG("DigiDollar supply index current block failed integrity verification");
            BOOST_REQUIRE(opened.index.StartBackgroundSync());
            // Join the actual disk reconstruction worker, including its failure path.
            opened.index.Stop();
        }
        BOOST_CHECK(ShutdownRequested());
        BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
        BOOST_CHECK(!opened.index.GetSummary().synced);
        BOOST_CHECK(opened.index.GetSummary().best_block_hash == prefix->GetBlockHash());
        BOOST_CHECK(!opened.index.LookUpStats(*current));
        const auto retained = opened.index.LookUpStats(*prefix);
        BOOST_REQUIRE(retained);
        BOOST_CHECK_EQUAL(retained->total_dd_supply, 200);
        BOOST_CHECK_EQUAL(retained->total_collateral, 45 * COIN);
        BOOST_CHECK_EQUAL(retained->vault_count, 2U);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.m_chain.Tip()) == current);
    }
    {
        OpenRedemptionStatsIndex reopened{m_node};
        BOOST_REQUIRE(reopened.index.Init());
        BOOST_CHECK(reopened.index.GetSummary().best_block_hash == prefix->GetBlockHash());
        BOOST_CHECK(!reopened.index.LookUpStats(*current));
        BOOST_REQUIRE(reopened.index.StartBackgroundSync());
        IndexWaitSynced(reopened.index);
        const auto stats = reopened.index.LookUpStats(*current);
        BOOST_REQUIRE(stats);
        BOOST_CHECK_EQUAL(stats->total_dd_supply, 100);
        BOOST_CHECK_EQUAL(stats->total_collateral, 15 * COIN);
        BOOST_CHECK_EQUAL(stats->vault_count, 1U);
        BOOST_CHECK(WITH_LOCK(cs_main, return chain.CoinsTip().GetDigiDollarState()) == canonical);
    }
}

BOOST_FIXTURE_TEST_CASE(legacy_missing_token_metadata_keeps_supply_unavailable, ThawMinerERRValidationSetup)
{
    CheckLegacyUncountableSupply(*this, false);
}

BOOST_FIXTURE_TEST_CASE(legacy_ambiguous_token_metadata_keeps_supply_unavailable, ThawMinerERRValidationSetup)
{
    CheckLegacyUncountableSupply(*this, true);
}

BOOST_FIXTURE_TEST_CASE(legacy_unknown_token_supply_reopens_across_thaw, ThawMinerERRValidationSetup)
{
    CheckLegacyUncountableSupply(*this, false, true);
}

BOOST_FIXTURE_TEST_CASE(legacy_ambiguous_token_supply_reopens_across_thaw, ThawMinerERRValidationSetup)
{
    CheckLegacyUncountableSupply(*this, true, true);
}

BOOST_FIXTURE_TEST_CASE(legacy_unknown_token_supply_crosses_thaw_with_open_index, ThawMinerERRValidationSetup)
{
    CheckLegacyUncountableSupply(*this, false, true, true);
}

BOOST_FIXTURE_TEST_CASE(thaw_miner_reports_missing_reference_before_selection, ThawMinerValidationSetup)
{
    PrepareCandidate(REFERENCE_PRICE);
    CBlockIndex* ancestor = FirstRequiredAncestor();
    BOOST_REQUIRE(ancestor);
    const auto previous_status = WITH_LOCK(cs_main, return ancestor->nStatus);
    {
        UnavailableAncestorData unavailable{*ancestor};
        BlockAssembler::Options options;
        options.blockMinFeeRate = CFeeRate(0);
        BOOST_CHECK_EXCEPTION(BuildTemplate(options), std::runtime_error, IsReferenceReadinessError);
    }
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return ancestor->nStatus), previous_status);
}

BOOST_FIXTURE_TEST_CASE(thaw_miner_does_not_retry_missing_reference_in_final_validation, ThawMinerValidationSetup)
{
    const auto txs = PrepareCandidate(REFERENCE_PRICE);
    CBlockIndex* ancestor = FirstRequiredAncestor();
    BOOST_REQUIRE(ancestor);
    const auto previous_status = WITH_LOCK(cs_main, return ancestor->nStatus);
    std::unique_ptr<UnavailableAncestorData> unavailable;
    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    int hook_calls{0};
    options.on_before_test_block_validity = [&](CBlock& block) {
        ++hook_calls;
        BOOST_REQUIRE(BlockHasTx(block, txs.mint->GetHash()));
        unavailable = std::make_unique<UnavailableAncestorData>(*ancestor);
    };
    BOOST_CHECK_EXCEPTION(BuildTemplate(options), std::runtime_error, IsReferenceReadinessError);
    BOOST_CHECK_EQUAL(hook_calls, 1);
    unavailable.reset();
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return ancestor->nStatus), previous_status);
}


// Building a block template must not walk the whole coin database. The walk reads
// a block from disk for every unspent DigiDollar output, and miners ask for a new
// template constantly, so doing it per template would stall the node for as long
// as the tip sits one block below the Thaw Day height.
BOOST_FIXTURE_TEST_CASE(boundary_template_does_not_walk_the_coin_set, ThawBoundaryTemplateSetup)
{
    const auto blockman_lookup = [&](const uint256& txid, uint32_t height, CTransactionRef& tx) {
        AssertLockHeld(cs_main);
        const auto* tip = m_node.chainman->ActiveChain().Tip();
        if (!tip || height > static_cast<uint32_t>(tip->nHeight)) return false;
        const auto* source = tip->GetAncestor(height);
        CBlock block;
        if (!source || !m_node.chainman->m_blockman.ReadBlockFromDisk(block, *source)) return false;
        for (const auto& candidate : block.vtx) {
            if (candidate->GetHash() == txid) { tx = candidate; return true; }
        }
        return false;
    };

    BlockAssembler::Options options;
    const auto before_templates = DigiDollar::ChainstateHealthRebuildCount();
    for (int attempt = 0; attempt < 3; ++attempt) {
        BOOST_REQUIRE(BuildTemplate(options));
    }
    BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), before_templates);

    // Actually connecting the block before the height builds the record, once.
    mineBlocks(1);
    BOOST_REQUIRE_EQUAL(NextBlockHeight(), THAW_HEIGHT);
    BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), before_templates + 1);

    // The record the node keeps is the same record a full walk produces.
    {
        LOCK(cs_main);
        auto& chainstate = m_node.chainman->ActiveChainstate();
        const auto kept = chainstate.CoinsTip().GetDigiDollarState();
        BOOST_REQUIRE(kept);
        BOOST_CHECK(kept->Matches(Params().GetConsensus().hashGenesisBlock,
                                  chainstate.m_chain.Tip()->GetBlockHash(),
                                  THAW_HEIGHT, Params().GetConsensus().DigiDollarHeight));
        BOOST_CHECK(!kept->history_checked);
        DigiDollar::ChainstateHealth fresh;
        std::string error;
        BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(
            chainstate.CoinsTip(), Params().GetConsensus(), blockman_lookup, fresh, error), error);
        BOOST_CHECK(*kept == fresh);
    }

    // Templates for the first Thaw Day block reuse that record instead of walking again.
    const auto after_connect = DigiDollar::ChainstateHealthRebuildCount();
    for (int attempt = 0; attempt < 3; ++attempt) {
        BOOST_REQUIRE(BuildTemplate(options));
    }
    BOOST_CHECK_EQUAL(DigiDollar::ChainstateHealthRebuildCount(), after_connect);

    // Connect the first Thaw Day block and a few after it, then re-check the whole
    // stretch the way "verifychain 4" does. That check reconnects blocks into a
    // private coins view using the same code path a mining template uses, so it
    // has to still get the record it needs when it crosses the height.
    mineBlocks(4);
    BOOST_REQUIRE_EQUAL(NextBlockHeight(), THAW_HEIGHT + 4);
    m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    {
        LOCK(cs_main);
        auto& chainstate = m_node.chainman->ActiveChainstate();
        BOOST_CHECK(CVerifyDB(m_node.chainman->GetNotifications()).VerifyDB(
            chainstate, Params().GetConsensus(), chainstate.CoinsDB(),
            /*nCheckLevel=*/4, /*nCheckDepth=*/10) == VerifyDBResult::SUCCESS);
    }
}

BOOST_AUTO_TEST_SUITE_END()
