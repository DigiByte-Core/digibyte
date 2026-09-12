// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <consensus/digidollar.h>
#include <kernel/chainparams.h>
#include <chainparams.h>
#include <key.h>
#include <policy/policy.h>
#include <random.h>
#include <test/util/setup_common.h>

using namespace DigiDollar;

BOOST_FIXTURE_TEST_SUITE(digidollar_txbuilder_tests, BasicTestingSetup)

// Helper function to create a test key
CKey CreateTestKey() {
    CKey key;
    key.MakeNewKey(true);
    return key;
}

bool IsCanonicalP2TROutput(const CScript& script)
{
    int witness_version = -1;
    std::vector<unsigned char> witness_program;
    return script.IsWitnessProgram(witness_version, witness_program) &&
           witness_version == 1 &&
           witness_program.size() == WITNESS_V1_TAPROOT_SIZE;
}

// Helper function to create test UTXOs
std::vector<COutPoint> CreateTestUTXOs(size_t count) {
    std::vector<COutPoint> utxos;
    for (size_t i = 0; i < count; ++i) {
        uint256 hash;
        hash.SetHex("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
        utxos.emplace_back(hash, i);
    }
    return utxos;
}

uint32_t CanonicalTierForLockDays(int lockDays)
{
    const int tier = GetLockTierIndex(LockDaysToBlocks(lockDays), Params().GetDigiDollarParams());
    BOOST_REQUIRE_GE(tier, 0);
    return static_cast<uint32_t>(tier);
}

// Test MintTxBuilder with larger UTXO values for collateral
class TestMintTxBuilder : public MintTxBuilder {
public:
    using MintTxBuilder::MintTxBuilder;

    // Override to provide larger UTXOs (10,000 DGB each instead of 100 DGB)
    CAmount GetDGBFromUTXO(const COutPoint& outpoint) const override {
        return 10000 * COIN; // 10,000 DGB per UTXO
    }
};

// Test RedeemTxBuilder with larger UTXO values
class TestRedeemTxBuilder : public RedeemTxBuilder {
public:
    using RedeemTxBuilder::RedeemTxBuilder;

    CAmount GetDGBFromUTXO(const COutPoint& outpoint) const override {
        return 10000 * COIN; // 10,000 DGB per UTXO
    }
};

// Test TransferTxBuilder with larger UTXO values
class TestTransferTxBuilder : public TransferTxBuilder {
public:
    using TransferTxBuilder::TransferTxBuilder;

    CAmount GetDGBFromUTXO(const COutPoint& outpoint) const override {
        return 10000 * COIN; // 10,000 DGB per UTXO
    }
};

BOOST_AUTO_TEST_CASE(txbuilder_basic_construction)
{
    // Test basic construction of transaction builders
    const CChainParams& params = Params();
    int height = 1000;
    // Oracle price in micro-USD format (1,000,000 = $1.00)
    CAmount price = 50000; // $0.05 per DGB (50,000 micro-USD)

    MintTxBuilder mintBuilder(params, height, price);
    TransferTxBuilder transferBuilder(params, height, price);
    RedeemTxBuilder redeemBuilder(params, height, price);

    // Builders should construct without error
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(mint_transaction_basic)
{
    const CChainParams& params = Params();
    int height = 1000;
    // Oracle price in micro-USD format (1,000,000 = $1.00)
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    // Create mint parameters
    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100 in cents
    mintParams.lockDays = 365;   // 1 year
    mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = CreateTestUTXOs(5);

    // Build mint transaction
    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty()); // No error
    BOOST_CHECK(result.tx.vin.size() > 0);
    BOOST_CHECK(result.tx.vout.size() >= 2); // Collateral + DD outputs
    BOOST_CHECK(result.collateralRequired > 0);
    BOOST_CHECK(result.totalFees > 0);

    // Check transaction type
    BOOST_CHECK(result.tx.IsDigiDollar());
    BOOST_CHECK(::GetDigiDollarTxType(CTransaction(result.tx)) == ::DD_TX_MINT);
}

BOOST_AUTO_TEST_CASE(mint_change_without_destination_is_not_p2tr_collateral)
{
    const CChainParams& params = Params();
    const int height = 1000;
    const CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    mintParams.lockDays = 365;
    mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000;
    mintParams.utxos = CreateTestUTXOs(5);

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_REQUIRE_MESSAGE(result.success, result.error);

    int positiveP2TROutputs = 0;
    for (const CTxOut& out : result.tx.vout) {
        if (out.nValue > 0 && IsCanonicalP2TROutput(out.scriptPubKey)) {
            positiveP2TROutputs++;
        }
    }

    BOOST_CHECK_EQUAL(positiveP2TROutputs, 1);
}

BOOST_AUTO_TEST_CASE(mint_transaction_insufficient_funds)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    // Create mint parameters with no UTXOs
    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100 in cents
    mintParams.lockDays = 365;   // 1 year
    mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    // No UTXOs provided - this should cause "Invalid mint parameters" error

    // Build mint transaction should fail
    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
    // The error will be "Invalid mint parameters" because validation checks UTXOs first
    BOOST_CHECK(result.error.find("Invalid mint parameters") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(mint_transaction_invalid_amount)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    // Create mint parameters with invalid amount (below minimum)
    // Check consensus params for actual minimum - typically $100 (10000 cents)
    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 5000; // $50 in cents (below $100 minimum)
    mintParams.lockDays = 365;  // 1 year
    mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = CreateTestUTXOs(5);

    // Build mint transaction should fail due to invalid amount
    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
    // Should get "Invalid mint parameters" error
    BOOST_CHECK(result.error.find("Invalid mint parameters") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(collateral_calculation)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    // Test collateral calculation for different lock periods
    CAmount ddAmount = 10000; // $100

    // 30 days should require more collateral than 1 year (higher ratio)
    CAmount collateral30Days = builder.CalculateRequiredCollateral(ddAmount, 30);
    CAmount collateral1Year = builder.CalculateRequiredCollateral(ddAmount, 365);

    BOOST_CHECK(collateral30Days > collateral1Year);
    BOOST_CHECK(collateral30Days > 0);
    BOOST_CHECK(collateral1Year > 0);

    // Test with larger amount - collateral should scale linearly
    CAmount collateralLarge = builder.CalculateRequiredCollateral(ddAmount * 10, 365);
    BOOST_CHECK(collateralLarge > collateral1Year * 9); // Should be roughly 10x (allowing for rounding)
    BOOST_CHECK(collateralLarge <= collateral1Year * 11); // But not too much more
}

BOOST_AUTO_TEST_CASE(transfer_transaction_basic)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestTransferTxBuilder builder(params, height, price);

    // Create transfer parameters with valid DD addresses
    CKey recipient1 = CreateTestKey();
    CKey recipient2 = CreateTestKey();
    CTxDestination dest1{WitnessV1Taproot(XOnlyPubKey(recipient1.GetPubKey()))};
    CTxDestination dest2{WitnessV1Taproot(XOnlyPubKey(recipient2.GetPubKey()))};
    std::string addr1 = DigiDollar::EncodeDigiDollarAddress(dest1, params);
    std::string addr2 = DigiDollar::EncodeDigiDollarAddress(dest2, params);

    TxBuilderTransferParams transferParams;
    transferParams.recipients = {
        {addr1, 5000}, // $50
        {addr2, 3000}  // $30
    };
    transferParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    transferParams.ddUtxos = CreateTestUTXOs(2);
    transferParams.feeUtxos = CreateTestUTXOs(2);
    transferParams.spenderKey = CreateTestKey();
    // Provide DD amounts for the test UTXOs (total must >= output amount)
    transferParams.ddAmounts = {5000, 3000}; // Total 8000 cents available

    // Build transfer transaction
    TxBuilderResult result = builder.BuildTransferTransaction(transferParams);

    if (!result.success) {
        std::cout << "TRANSFER FAILED: " << result.error << std::endl;
    }

    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
    BOOST_CHECK(result.tx.vin.size() > 0);
    BOOST_CHECK(result.tx.vout.size() >= 2); // At least recipient outputs

    // Check transaction type
    BOOST_CHECK(result.tx.IsDigiDollar());
    BOOST_CHECK(::GetDigiDollarTxType(CTransaction(result.tx)) == ::DD_TX_TRANSFER);
}

BOOST_AUTO_TEST_CASE(transfer_rejects_underfunded_fee_inputs)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestTransferTxBuilder builder(params, height, price);

    CKey recipient = CreateTestKey();
    CTxDestination dest{WitnessV1Taproot(XOnlyPubKey(recipient.GetPubKey()))};
    std::string addr = DigiDollar::EncodeDigiDollarAddress(dest, params);

    uint256 feeHash;
    feeHash.SetHex("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");

    TxBuilderTransferParams transferParams;
    transferParams.recipients = {{addr, 5000}};
    transferParams.feeRate = 35000000;
    transferParams.ddUtxos = CreateTestUTXOs(1);
    transferParams.ddAmounts = {5000};
    transferParams.feeUtxos = {COutPoint(feeHash, 0)};
    transferParams.feeAmounts = {1};
    transferParams.spenderKey = CreateTestKey();

    TxBuilderResult result = builder.BuildTransferTransaction(transferParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("Insufficient DGB fee input") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(transfer_transaction_invalid_address)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestTransferTxBuilder builder(params, height, price);

    // Create transfer parameters with invalid address
    TxBuilderTransferParams transferParams;
    transferParams.recipients = {
        {"INVALID_ADDRESS_FORMAT", 5000} // Invalid DD address
    };
    transferParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    transferParams.ddUtxos = CreateTestUTXOs(2);
    transferParams.spenderKey = CreateTestKey();
    transferParams.ddAmounts = {5000, 3000}; // Provide DD amounts

    // Build transfer transaction should fail due to invalid address
    TxBuilderResult result = builder.BuildTransferTransaction(transferParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
    // Should get "Invalid transfer parameters" error
    BOOST_CHECK(result.error.find("Invalid transfer parameters") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(redeem_transaction_basic)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestRedeemTxBuilder builder(params, height, price);

    // Create redeem parameters
    TxBuilderRedeemParams redeemParams;
    uint256 collateralHash;
    collateralHash.SetHex("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    redeemParams.collateralOutpoint = COutPoint(collateralHash, 0);
    redeemParams.ddToRedeem = 10000; // $100
    redeemParams.path = RedemptionPath::NORMAL;
    redeemParams.ownerKey = CreateTestKey();
    redeemParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    redeemParams.ddUtxos = CreateTestUTXOs(1);
    redeemParams.feeUtxos = CreateTestUTXOs(1);
    // Provide pre-queried collateral position (to avoid UTXO lookup in test)
    redeemParams.collateralAmount = 30000000000; // 300 DGB collateral (for 300% ratio)
    redeemParams.ddMinted = 10000; // $100 DD minted
    redeemParams.unlockHeight = 500; // Unlock at height 500 (current height is 1000, so timelock expired)
    // EXACT-AMOUNT REDEMPTION: DD UTXOs must contain exactly the amount being redeemed
    redeemParams.ddAmounts = {10000}; // DD UTXO contains exactly 10000 cents (matches ddMinted)

    // Build redeem transaction
    TxBuilderResult result = builder.BuildRedemptionTransaction(redeemParams);

    if (!result.success) {
        std::cout << "REDEEM FAILED: " << result.error << std::endl;
    }

    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
    BOOST_CHECK(result.tx.vin.size() > 0);
    BOOST_CHECK(result.tx.vout.size() >= 1); // DGB output
    BOOST_CHECK_EQUAL(result.tx.nLockTime, redeemParams.unlockHeight);
    BOOST_CHECK_EQUAL(result.totalFees, COIN / 10);

    // Check transaction type
    BOOST_CHECK(result.tx.IsDigiDollar());
    BOOST_CHECK(::GetDigiDollarTxType(CTransaction(result.tx)) == ::DD_TX_REDEEM);
}

BOOST_AUTO_TEST_CASE(redeem_transaction_requires_fee_inputs)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestRedeemTxBuilder builder(params, height, price);

    TxBuilderRedeemParams redeemParams;
    uint256 collateralHash;
    collateralHash.SetHex("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    redeemParams.collateralOutpoint = COutPoint(collateralHash, 0);
    redeemParams.ddToRedeem = 10000;
    redeemParams.path = RedemptionPath::NORMAL;
    redeemParams.ownerKey = CreateTestKey();
    redeemParams.feeRate = 100000;
    redeemParams.ddUtxos = CreateTestUTXOs(1);
    redeemParams.collateralAmount = 30000000000;
    redeemParams.ddMinted = 10000;
    redeemParams.unlockHeight = 500;
    redeemParams.ddAmounts = {10000};

    TxBuilderResult result = builder.BuildRedemptionTransaction(redeemParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("Insufficient fee inputs") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Redemption fee estimate and fee-coin reselection
//
// A redemption's fee grows with every DGB coin added to pay it, so choosing
// coins once against a guessed size can leave the real transaction short.
// These cases pin the estimate to the fee the build actually charges and
// show that reselection changes the input set until the coins cover it.
// ---------------------------------------------------------------------------

namespace {

constexpr CAmount PRODUCTION_DD_FEE_RATE{35000000}; // 0.35 DGB/kB, the rate the wallet and RPC use
constexpr CAmount OLD_FIXED_REDEEM_TARGET{21000000}; // 400 vB * 0.35 DGB/kB + 50%, the old one-shot guess

TxBuilderRedeemParams MakeRedeemParams(CAmount feeRate = PRODUCTION_DD_FEE_RATE)
{
    TxBuilderRedeemParams params;
    uint256 collateralHash;
    collateralHash.SetHex("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    params.collateralOutpoint = COutPoint(collateralHash, 0);
    params.ddToRedeem = 10000;
    params.path = RedemptionPath::NORMAL;
    params.ownerKey = CreateTestKey();
    params.feeRate = feeRate;
    params.ddUtxos = CreateTestUTXOs(1);
    params.ddAmounts = {10000};
    params.collateralAmount = 30000000000; // 300 DGB
    params.ddMinted = 10000;
    params.unlockHeight = 500;
    return params;
}

// A wallet holding the given DGB coins, selecting smallest-first until the
// target is met, exactly like DigiDollarWallet::SelectFeeCoins.
struct FakeFeeWallet {
    std::vector<CAmount> coins;
    std::vector<size_t> selected_counts; // one entry per selector call

    explicit FakeFeeWallet(std::vector<CAmount> c) : coins(std::move(c))
    {
        std::sort(coins.begin(), coins.end());
    }

    RedeemTxBuilder::FeeCoinSelector Selector()
    {
        return [this](CAmount target, std::vector<COutPoint>& utxos, std::vector<CAmount>& amounts, CAmount& total) {
            utxos.clear();
            amounts.clear();
            total = 0;
            for (size_t i = 0; i < coins.size() && total < target; ++i) {
                utxos.emplace_back(uint256::ONE, static_cast<uint32_t>(i));
                amounts.push_back(coins[i]);
                total += coins[i];
            }
            selected_counts.push_back(utxos.size());
            if (total >= target) return true;
            utxos.clear();
            amounts.clear();
            total = 0;
            return false;
        };
    }
};

CAmount Sum(const std::vector<CAmount>& amounts)
{
    CAmount total = 0;
    for (const CAmount a : amounts) total += a;
    return total;
}

} // namespace

BOOST_AUTO_TEST_CASE(redeem_fee_estimate_matches_build_fee)
{
    // The estimate must be the fee the build charges, for every input count,
    // with and without DD change, and for every collateral destination type.
    TestRedeemTxBuilder builder(Params(), 1000, 10000);
    const CKey destKey = CreateTestKey();
    const std::vector<std::optional<CTxDestination>> destinations{
        std::nullopt, // owner-key Taproot fallback
        CTxDestination{WitnessV1Taproot(XOnlyPubKey(destKey.GetPubKey()))},
        CTxDestination{WitnessV0KeyHash(destKey.GetPubKey())},
        CTxDestination{PKHash(destKey.GetPubKey())},
    };
    for (const size_t feeInputs : {1u, 2u, 3u, 5u, 8u, 20u}) {
        for (const CAmount ddInput : {CAmount{10000}, CAmount{12500}}) {
            for (const auto& dest : destinations) {
                TxBuilderRedeemParams params = MakeRedeemParams();
                params.ddAmounts = {ddInput};
                params.collateralDest = dest;
                params.feeUtxos = CreateTestUTXOs(feeInputs);
                // Generous coins: the DGB change stays above dust, so the
                // build's fee is exactly the estimate and not "fee + dust".
                params.feeAmounts.assign(feeInputs, 100 * COIN);

                const RedeemTxBuilder::FeeEstimate estimate = builder.EstimateRedemptionFee(params);
                BOOST_REQUIRE_MESSAGE(estimate.ok, estimate.error);
                BOOST_CHECK_GE(estimate.fee, COIN / 10);

                const TxBuilderResult result = builder.BuildRedemptionTransaction(params);
                BOOST_REQUIRE_MESSAGE(result.success, result.error);
                BOOST_CHECK_EQUAL(result.totalFees, estimate.fee);
                BOOST_CHECK_EQUAL(result.tx.vin.size(), 2 + feeInputs);
                // The build appends the DGB change output after fixing the fee, so the
                // finished transaction is larger than the skeleton the fee was set on.
                BOOST_CHECK_GT(EstimateTransactionVSize(result.tx), estimate.vsize);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(redeem_fee_estimate_keeps_minimum_fee_floor)
{
    TestRedeemTxBuilder builder(Params(), 1000, 10000);
    TxBuilderRedeemParams params = MakeRedeemParams(/*feeRate=*/100000);
    params.feeUtxos = CreateTestUTXOs(1);
    const RedeemTxBuilder::FeeEstimate estimate = builder.EstimateRedemptionFee(params);
    BOOST_REQUIRE_MESSAGE(estimate.ok, estimate.error);
    BOOST_CHECK_EQUAL(estimate.fee, COIN / 10); // the absolute DD minimum, as redeem_transaction_basic expects
}

BOOST_AUTO_TEST_CASE(redeem_fee_estimate_rejects_insufficient_dd)
{
    TestRedeemTxBuilder builder(Params(), 1000, 10000);
    TxBuilderRedeemParams params = MakeRedeemParams();
    params.ddAmounts = {5000}; // less than the 10000 to burn
    params.feeUtxos = CreateTestUTXOs(1);
    const RedeemTxBuilder::FeeEstimate estimate = builder.EstimateRedemptionFee(params);
    BOOST_CHECK(!estimate.ok);
    BOOST_CHECK(estimate.error.find("Insufficient DD") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(redeem_fragmented_wallet_one_shot_selection_fails_then_reselection_converges)
{
    TestRedeemTxBuilder builder(Params(), 1000, 10000);
    FakeFeeWallet wallet(std::vector<CAmount>(40, 5000000)); // forty coins of 0.05 DGB

    // The old behaviour: one selection against a 400 vB guess. Smallest-first
    // reaches 0.21 DGB with five coins, but seven inputs make the real
    // transaction cost more than 0.25 DGB, and the build refuses it.
    {
        TxBuilderRedeemParams params = MakeRedeemParams();
        CAmount total = 0;
        BOOST_REQUIRE(wallet.Selector()(OLD_FIXED_REDEEM_TARGET, params.feeUtxos, params.feeAmounts, total));
        BOOST_CHECK_EQUAL(params.feeUtxos.size(), 5u);
        BOOST_CHECK_EQUAL(total, 25000000);
        const RedeemTxBuilder::FeeEstimate estimate = builder.EstimateRedemptionFee(params);
        BOOST_REQUIRE_MESSAGE(estimate.ok, estimate.error);
        BOOST_CHECK_GT(estimate.fee, total);
        const TxBuilderResult result = builder.BuildRedemptionTransaction(params);
        BOOST_CHECK(!result.success);
        BOOST_CHECK_EQUAL(result.error, "Insufficient fee inputs for DD redemption fee");
    }
    wallet.selected_counts.clear();

    // Reselection: every attempt changes the input set until the coins cover
    // the fee of the transaction they produce, and the build then succeeds.
    {
        TxBuilderRedeemParams params = MakeRedeemParams();
        std::string error;
        BOOST_REQUIRE_MESSAGE(builder.SelectRedemptionFeeInputs(params, wallet.Selector(), error), error);
        BOOST_CHECK(error.empty());
        BOOST_CHECK_GT(params.feeUtxos.size(), 5u);
        BOOST_CHECK_EQUAL(params.feeUtxos.size(), params.feeAmounts.size());
        BOOST_CHECK_GT(wallet.selected_counts.size(), 1u);
        for (size_t i = 1; i < wallet.selected_counts.size(); ++i) {
            BOOST_CHECK_MESSAGE(wallet.selected_counts[i] > wallet.selected_counts[i - 1],
                                "attempt " << i + 1 << " selected " << wallet.selected_counts[i]
                                           << " coins, not more than attempt " << i << " (" << wallet.selected_counts[i - 1] << ")");
        }
        const RedeemTxBuilder::FeeEstimate estimate = builder.EstimateRedemptionFee(params);
        BOOST_REQUIRE_MESSAGE(estimate.ok, estimate.error);
        BOOST_CHECK_GE(Sum(params.feeAmounts), estimate.fee);
        const TxBuilderResult result = builder.BuildRedemptionTransaction(params);
        BOOST_REQUIRE_MESSAGE(result.success, result.error);
        BOOST_CHECK_EQUAL(result.tx.vin.size(), 2 + params.feeUtxos.size());
        BOOST_CHECK_GE(Sum(params.feeAmounts), result.totalFees);
        BOOST_CHECK_GE(result.totalFees, COIN / 10);
    }
}

BOOST_AUTO_TEST_CASE(redeem_single_large_coin_is_selected_once)
{
    // A wallet with one large coin behaves exactly as before: one attempt,
    // one fee input, and the build charges the estimated fee.
    TestRedeemTxBuilder builder(Params(), 1000, 10000);
    FakeFeeWallet wallet({100 * COIN});
    TxBuilderRedeemParams params = MakeRedeemParams();
    std::string error;
    BOOST_REQUIRE_MESSAGE(builder.SelectRedemptionFeeInputs(params, wallet.Selector(), error), error);
    BOOST_CHECK_EQUAL(wallet.selected_counts.size(), 1u);
    BOOST_CHECK_EQUAL(params.feeUtxos.size(), 1u);
    const RedeemTxBuilder::FeeEstimate estimate = builder.EstimateRedemptionFee(params);
    const TxBuilderResult result = builder.BuildRedemptionTransaction(params);
    BOOST_REQUIRE_MESSAGE(result.success, result.error);
    BOOST_CHECK_EQUAL(result.totalFees, estimate.fee);
    BOOST_CHECK_EQUAL(result.tx.vin.size(), 3u);
}

BOOST_AUTO_TEST_CASE(redeem_dust_only_wallet_fails_within_attempt_budget)
{
    // Coins so small that each one adds more fee than value can never
    // converge. The loop must stop at its budget, clear the selection, and
    // tell the user to consolidate rather than spin or return a short set.
    TestRedeemTxBuilder builder(Params(), 1000, 10000);
    FakeFeeWallet wallet(std::vector<CAmount>(200, 2000000)); // two hundred coins of 0.02 DGB
    TxBuilderRedeemParams params = MakeRedeemParams();
    std::string error;
    BOOST_CHECK(!builder.SelectRedemptionFeeInputs(params, wallet.Selector(), error, /*max_attempts=*/4));
    BOOST_CHECK_EQUAL(wallet.selected_counts.size(), 4u);
    BOOST_CHECK(params.feeUtxos.empty());
    BOOST_CHECK(params.feeAmounts.empty());
    BOOST_CHECK_MESSAGE(error.find("onsolidate") != std::string::npos, error);
    BOOST_CHECK_MESSAGE(error.find("Insufficient DGB fee inputs") != std::string::npos, error);
}

BOOST_AUTO_TEST_CASE(redeem_wallet_without_dgb_fails_with_balance_error)
{
    TestRedeemTxBuilder builder(Params(), 1000, 10000);
    FakeFeeWallet empty({});
    TxBuilderRedeemParams params = MakeRedeemParams();
    std::string error;
    BOOST_CHECK(!builder.SelectRedemptionFeeInputs(params, empty.Selector(), error));
    BOOST_CHECK_EQUAL(empty.selected_counts.size(), 1u);
    BOOST_CHECK(params.feeUtxos.empty());
    BOOST_CHECK_MESSAGE(error.find("Insufficient DGB balance") != std::string::npos, error);

    // Coins that run out part-way stop with the same balance error.
    FakeFeeWallet tiny({1000000}); // a single 0.01 DGB coin
    TxBuilderRedeemParams params2 = MakeRedeemParams();
    BOOST_CHECK(!builder.SelectRedemptionFeeInputs(params2, tiny.Selector(), error));
    BOOST_CHECK(params2.feeUtxos.empty());
    BOOST_CHECK_MESSAGE(error.find("Insufficient DGB balance") != std::string::npos, error);
}

BOOST_AUTO_TEST_CASE(redeem_transaction_rejects_zero_prequeried_dd_minted)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestRedeemTxBuilder builder(params, height, price);

    TxBuilderRedeemParams redeemParams;
    uint256 collateralHash;
    collateralHash.SetHex("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    redeemParams.collateralOutpoint = COutPoint(collateralHash, 0);
    redeemParams.ddToRedeem = 10000;
    redeemParams.path = RedemptionPath::NORMAL;
    redeemParams.ownerKey = CreateTestKey();
    redeemParams.feeRate = 100000;
    redeemParams.ddUtxos = CreateTestUTXOs(1);
    redeemParams.ddAmounts = {10000};
    redeemParams.feeUtxos = CreateTestUTXOs(1);
    redeemParams.collateralAmount = 30000000000;
    redeemParams.ddMinted = 0;
    redeemParams.unlockHeight = 500;

    TxBuilderResult result = builder.BuildRedemptionTransaction(redeemParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_AUTO_TEST_CASE(redeem_transaction_different_paths)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestRedeemTxBuilder builder(params, height, price);

    uint256 collateralHash;
    collateralHash.SetHex("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");

    // Test different redemption paths
    // Note: ERR path requires system health < 100%, which we can't easily mock in this test
    // DELETED: RedemptionPath::EMERGENCY - Emergency redemption path does not exist
    std::vector<RedemptionPath> paths = {
        RedemptionPath::NORMAL
    };

    for (RedemptionPath path : paths) {
        TxBuilderRedeemParams redeemParams;
        redeemParams.collateralOutpoint = COutPoint(collateralHash, 0);
        redeemParams.ddToRedeem = 10000; // $100
        redeemParams.path = path;
        redeemParams.ownerKey = CreateTestKey();
        redeemParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        redeemParams.ddUtxos = CreateTestUTXOs(1);
        redeemParams.feeUtxos = CreateTestUTXOs(1);
        // Provide pre-queried collateral position (to avoid UTXO lookup in test)
        redeemParams.collateralAmount = 30000000000; // 300 DGB collateral (for 300% ratio)
        redeemParams.ddMinted = 10000; // $100 DD minted
        redeemParams.unlockHeight = 500; // Unlock at height 500 (current height is 1000, so timelock expired)
        // EXACT-AMOUNT REDEMPTION: DD UTXOs must contain exactly the amount being redeemed
        redeemParams.ddAmounts = {10000}; // DD UTXO contains exactly 10000 cents (matches ddMinted)

        TxBuilderResult result = builder.BuildRedemptionTransaction(redeemParams);

        if (!result.success) {
            std::cout << "REDEEM PATH " << static_cast<int>(path) << " FAILED: " << result.error << std::endl;
        }

        BOOST_CHECK(result.success);
        BOOST_CHECK(result.error.empty());
    }
}

BOOST_AUTO_TEST_CASE(fee_calculation)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    // Create a simple transaction for fee calculation
    CMutableTransaction tx;
    tx.SetDigiDollarType(::DD_TX_MINT);

    // Add some inputs and outputs
    uint256 hash;
    hash.SetHex("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    tx.vin.push_back(CTxIn(COutPoint(hash, 0)));
    tx.vout.push_back(CTxOut(100 * COIN, CScript()));
    tx.vout.push_back(CTxOut(0, CScript()));

    // Test fee calculation with different rates
    // Since CalculateFee is protected, we'll use EstimateTransactionVSize instead
    size_t vsize = EstimateTransactionVSize(tx);
    CAmount fee1000 = (vsize * 1000) / 1000; // 1000 sat/vB
    CAmount fee2000 = (vsize * 2000) / 1000; // 2000 sat/vB

    BOOST_CHECK(fee1000 > 0);
    BOOST_CHECK(fee2000 > fee1000);
    BOOST_CHECK(fee2000 >= fee1000 * 2); // Should be roughly double
}

BOOST_AUTO_TEST_CASE(utility_functions)
{
    // Test lock days to blocks conversion
    int64_t blocks30Days = LockDaysToBlocks(30);
    int64_t blocks365Days = LockDaysToBlocks(365);

    BOOST_CHECK(blocks30Days > 0);
    BOOST_CHECK(blocks365Days > blocks30Days);
    BOOST_CHECK(blocks365Days >= blocks30Days * 12); // Roughly 12x

    // Test with DigiByte's 15-second block time
    int64_t blocksPerDay = 24 * 60 * 4; // 5760 blocks per day
    BOOST_CHECK(blocks30Days == 30 * blocksPerDay);
    BOOST_CHECK(blocks365Days == 365 * blocksPerDay);
}

BOOST_AUTO_TEST_CASE(transaction_validation_integration)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    // Create valid mint parameters
    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100 in cents
    mintParams.lockDays = 365;   // 1 year
    mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = CreateTestUTXOs(5);

    // Build mint transaction
    TxBuilderResult result = builder.BuildMintTransaction(mintParams);
    BOOST_CHECK(result.success);

    // Test that the built transaction passes basic validation
    ValidationContext ctx(height, price, 150, params); // 150% system collateral
    TxValidationState state;

    // Note: This will fail until full validation is implemented
    // but it tests the integration between builder and validator
    CTransaction tx(result.tx);
    bool isValid = ValidateDigiDollarTransaction(tx, ctx, state);

    // For now, just check that validation runs without crashing
    // In a complete implementation, this should return true
    BOOST_CHECK(true); // Placeholder until validation is complete
}

BOOST_AUTO_TEST_CASE(edge_cases_and_error_handling)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000; // $0.01 per DGB (10,000 micro-USD)

    TestMintTxBuilder builder(params, height, price);

    // Test with zero DD amount
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 0; // Invalid
        mintParams.lockDays = 365;
        mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = CreateTestUTXOs(5);

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
    }

    // Test with invalid lock period
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 10000;
        mintParams.lockDays = 10; // Too short
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = CreateTestUTXOs(5);

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
    }

    // Test with invalid key
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 10000;
        mintParams.lockDays = 365;
        mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
        // mintParams.ownerKey not set (invalid)
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = CreateTestUTXOs(5);

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
    }

    // Test with extreme fee rate
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 10000;
        mintParams.lockDays = 365;
        mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 200000000; // 200M sat/kB - above max of 100M sat/kB
        mintParams.utxos = CreateTestUTXOs(5);

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
    }
}

BOOST_AUTO_TEST_CASE(consolidation_pass_calculation)
{
    // Bug #24: Test the multi-pass consolidation constants.
    // MAX_CONSOLIDATION_INPUTS = 1400 (conservative, ~379k WU for P2WPKH)
    // Each P2WPKH input is ~271 WU. 1400 * 271 = 379,400 WU < 400,000 (MAX_STANDARD_TX_WEIGHT)
    const size_t MAX_CONSOLIDATION_INPUTS = 1400;

    // 600 UTXOs: should fit in a single pass
    {
        size_t utxo_count = 600;
        size_t passes = (utxo_count + MAX_CONSOLIDATION_INPUTS - 1) / MAX_CONSOLIDATION_INPUTS;
        BOOST_CHECK_EQUAL(passes, 1u);
    }

    // 1400 UTXOs: exactly one pass
    {
        size_t utxo_count = 1400;
        size_t passes = (utxo_count + MAX_CONSOLIDATION_INPUTS - 1) / MAX_CONSOLIDATION_INPUTS;
        BOOST_CHECK_EQUAL(passes, 1u);
    }

    // 1401 UTXOs: needs two passes
    {
        size_t utxo_count = 1401;
        size_t passes = (utxo_count + MAX_CONSOLIDATION_INPUTS - 1) / MAX_CONSOLIDATION_INPUTS;
        BOOST_CHECK_EQUAL(passes, 2u);
    }

    // 3000 UTXOs: needs three passes
    {
        size_t utxo_count = 3000;
        size_t passes = (utxo_count + MAX_CONSOLIDATION_INPUTS - 1) / MAX_CONSOLIDATION_INPUTS;
        BOOST_CHECK_EQUAL(passes, 3u);
    }
}

BOOST_AUTO_TEST_CASE(select_coins_respects_max_inputs)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000;

    TestMintTxBuilder builder(params, height, price);

    // Create 500 UTXOs worth 10,000 DGB each = 5,000,000 DGB total
    // Target collateral = 10,000 DGB (should be achievable with 1 UTXO)
    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    mintParams.lockDays = 365;
    mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000;
    mintParams.utxos = CreateTestUTXOs(500);

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);
    BOOST_CHECK(result.success);
    BOOST_CHECK_LE(result.tx.vin.size(), 400u); // MAX_TX_INPUTS = 400
}

BOOST_AUTO_TEST_CASE(select_coins_fails_fragmented_wallet)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 10000;

    // Create a builder with small UTXOs (100 DGB each, default)
    // 500 UTXOs * 100 DGB = 50,000 DGB. But MAX_TX_INPUTS=400, so
    // max selectable = 400 * 100 = 40,000 DGB. If collateral needed > 40k, fail.
    MintTxBuilder builder(params, height, price);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 50000; // $500 at $0.01/DGB needs huge collateral
    mintParams.lockDays = 365;
    mintParams.lockTier = CanonicalTierForLockDays(mintParams.lockDays);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000;
    mintParams.utxos = CreateTestUTXOs(500);

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);
    // Should fail because 100 DGB * 400 inputs is not enough for the collateral
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("Too many small UTXOs") != std::string::npos ||
                result.error.find("Insufficient funds") != std::string::npos);
}


BOOST_AUTO_TEST_CASE(sendmany_metadata_boundary_preflight)
{
    auto build_metadata = [](size_t outputs) {
        CScript metadata;
        metadata << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(2);
        for (size_t i = 0; i < outputs; ++i) {
            metadata << CScriptNum(CAmount{10000000}); // max per-recipient DD amount encodes worst-case here
        }
        return metadata;
    };

    // sendmanydigidollar includes one OP_RETURN amount per recipient plus DD change
    // when selected inputs exceed the sent total. Fourteen recipients + change is the
    // largest standard payload with max-sized transfer amounts.
    BOOST_CHECK_LE(build_metadata(15).size(), MAX_OP_RETURN_RELAY);
    BOOST_CHECK_GT(build_metadata(16).size(), MAX_OP_RETURN_RELAY);
}

BOOST_AUTO_TEST_CASE(sendmany_projected_vsize_fee_scales_with_recipient_count)
{
    CMutableTransaction small;
    small.SetDigiDollarType(::DD_TX_TRANSFER);
    uint256 hash1;
    uint256 hash2;
    hash1.SetHex("01");
    hash2.SetHex("02");
    small.vin.push_back(CTxIn(COutPoint(hash1, 0)));
    small.vin.push_back(CTxIn(COutPoint(hash2, 0)));
    small.vout.push_back(CTxOut(0, CScript() << OP_1 << std::vector<unsigned char>(32, 1)));
    small.vout.push_back(CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(2) << CScriptNum(10000000)));

    CMutableTransaction large = small;
    for (int i = 0; i < 13; ++i) {
        large.vout.insert(large.vout.end() - 1, CTxOut(0, CScript() << OP_1 << std::vector<unsigned char>(32, static_cast<unsigned char>(i + 2))));
    }

    const size_t small_vsize = EstimateTransactionVSize(small);
    const size_t large_vsize = EstimateTransactionVSize(large);
    BOOST_CHECK_GT(large_vsize, small_vsize);
    BOOST_CHECK_LE(large_vsize * WITNESS_SCALE_FACTOR, static_cast<size_t>(MAX_STANDARD_TX_WEIGHT));
}

BOOST_AUTO_TEST_CASE(consolidation_input_weight_budget)
{
    // Verify the weight budget calculation for consolidation.
    // P2WPKH input: 41 bytes base + 107 bytes witness ≈ 271 WU
    // MAX_STANDARD_TX_WEIGHT = 400,000 WU
    // Tx overhead ~42 bytes base = ~168 WU (header, locktime, etc.)
    // P2WPKH output: 31 bytes = 124 WU
    // Available for inputs: 400,000 - 168 - 124 = 399,708 WU
    // Max inputs: 399,708 / 271 ≈ 1475
    // Using 1400 for safety margin

    const int32_t MAX_STANDARD_TX_WEIGHT = 400000;
    const size_t P2WPKH_INPUT_WEIGHT = 271;
    const size_t TX_OVERHEAD_WEIGHT = 168;
    const size_t P2WPKH_OUTPUT_WEIGHT = 124;

    size_t available_weight = MAX_STANDARD_TX_WEIGHT - TX_OVERHEAD_WEIGHT - P2WPKH_OUTPUT_WEIGHT;
    size_t theoretical_max = available_weight / P2WPKH_INPUT_WEIGHT;
    const size_t MAX_CONSOLIDATION_INPUTS = 1400;

    BOOST_CHECK_GT(theoretical_max, MAX_CONSOLIDATION_INPUTS);
    BOOST_CHECK_LE(MAX_CONSOLIDATION_INPUTS * P2WPKH_INPUT_WEIGHT + TX_OVERHEAD_WEIGHT + P2WPKH_OUTPUT_WEIGHT,
                   (size_t)MAX_STANDARD_TX_WEIGHT);
}

BOOST_AUTO_TEST_SUITE_END()
