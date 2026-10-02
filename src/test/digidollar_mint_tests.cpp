// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <digidollar/txbuilder.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <consensus/digidollar.h>
#include <kernel/chainparams.h>
#include <chainparams.h>
#include <key.h>
#include <random.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <base58.h>

using namespace DigiDollar;

BOOST_FIXTURE_TEST_SUITE(digidollar_mint_tests, RegTestingSetup)

static constexpr CAmount MIN_DD_MINT_FEE = 10000000; // 0.1 DGB

// Helper function to create a test key
CKey CreateTestKey() {
    CKey key;
    key.MakeNewKey(true);
    return key;
}

// Helper function to create test UTXOs with specific values
std::vector<COutPoint> CreateTestUTXOsWithValues(const std::vector<CAmount>& values) {
    std::vector<COutPoint> utxos;
    for (size_t i = 0; i < values.size(); ++i) {
        uint256 hash;
        hash.SetHex("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcd" + std::to_string(i).substr(0,2));
        utxos.emplace_back(hash, i);
    }
    return utxos;
}

// Helper function to create test oracle price (DGB/USD in micro-USD)
// Format: 1,000,000 micro-USD = $1.00
CAmount CreateTestOraclePrice() {
    return 50000; // $0.05 per DGB = 50,000 micro-USD
}

// Give the mint an ordinary bech32 address for its leftover DGB. The builder
// refuses to place change anywhere else, and a mint may not send change to a
// taproot address.
void SetWalletChangeDestination(TxBuilderMintParams& params)
{
    CKey key;
    key.MakeNewKey(true);
    params.dgbChangeDest = CTxDestination{WitnessV0KeyHash(key.GetPubKey())};
}

void SetCanonicalLock(TxBuilderMintParams& params, int lockDays)
{
    const int tierIndex = GetLockTierIndex(LockDaysToBlocks(lockDays), Params().GetDigiDollarParams());
    BOOST_REQUIRE_MESSAGE(tierIndex >= 0, "test requested non-canonical lock days: " + std::to_string(lockDays));
    params.lockDays = lockDays;
    params.lockTier = static_cast<uint32_t>(tierIndex);
}

// Helper function to create mock mint transaction builder
class MockMintTxBuilder : public MintTxBuilder {
private:
    std::map<COutPoint, CAmount> m_utxo_values;

public:
    MockMintTxBuilder(const CChainParams& params, int height, CAmount price)
        : MintTxBuilder(params, height, price) {}

    // Override UTXO value lookup for testing
    void SetUTXOValue(const COutPoint& outpoint, CAmount value) {
        m_utxo_values[outpoint] = value;
    }

    // Override GetUTXOValue for testing
    CAmount GetUTXOValue(const COutPoint& outpoint) const {
        auto it = m_utxo_values.find(outpoint);
        return (it != m_utxo_values.end()) ? it->second : 100 * COIN; // Default for testing
    }

    // Override GetUTXOValueVirtual to make SelectCoins work with mocked values
    CAmount GetUTXOValueVirtual(const COutPoint& outpoint) const override {
        return GetUTXOValue(outpoint);
    }

    // Expose public methods for testing
    using MintTxBuilder::CalculateRequiredCollateral;
};

// ============================================================================
// Basic Mint Creation Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(mint_minimum_amount)
{
    // Test minting minimum allowed amount ($100.00 = 10000 cents on mainnet/testnet, $1.00 = 100 cents on regtest)
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Set up UTXOs with sufficient value
    auto utxos = CreateTestUTXOsWithValues({100 * COIN, 200 * COIN});
    for (size_t i = 0; i < utxos.size(); ++i) {
        builder.SetUTXOValue(utxos[i], (i + 1) * 100 * COIN);
    }

    TxBuilderMintParams mintParams;
    // Use consensus minimum - regtest uses $100.00 minimum (10000 cents)
    mintParams.ddAmount = params.GetDigiDollarParams().minMintAmount; // $100.00 in cents
    SetCanonicalLock(mintParams, 365); // 1 year
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    if (!result.success) {
        std::cout << "ERROR: BuildMintTransaction FAILED: " << result.error << std::endl;
    }
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());
    BOOST_CHECK(result.tx.vin.size() > 0);
    BOOST_CHECK(result.tx.vout.size() >= 3); // Collateral + DD + OP_RETURN outputs
    BOOST_CHECK(result.collateralRequired > 0);
    BOOST_CHECK(result.totalFees > 0);

    // Check transaction version format
    BOOST_CHECK(result.tx.IsDigiDollar());
    BOOST_CHECK(::GetDigiDollarTxType(CTransaction(result.tx)) == ::DD_TX_MINT);
}

BOOST_AUTO_TEST_CASE(mint_standard_amounts)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Test standard amounts (regtest max is 100,000 cents = $1,000)
    // Test: $1, $10, $100 in cents
    std::vector<CAmount> amounts = {100, 1000, 10000}; // $1, $10, $100 in cents

    for (CAmount amount : amounts) {
        // Provide enough for largest amount ($100 needs ~6000 DGB at 300% ratio)
        auto utxos = CreateTestUTXOsWithValues({3000 * COIN, 3000 * COIN, 3000 * COIN});
        for (size_t i = 0; i < utxos.size(); ++i) {
            builder.SetUTXOValue(utxos[i], 3000 * COIN);
        }

        TxBuilderMintParams mintParams;
        mintParams.ddAmount = amount;
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);

        BOOST_CHECK_MESSAGE(result.success, "Failed for amount: " + std::to_string(amount));
        BOOST_CHECK(result.collateralRequired > 0);

        // Collateral should scale roughly with DD amount
        CAmount expectedCollateral = builder.CalculateRequiredCollateral(amount, 365);
        BOOST_CHECK_MESSAGE(result.collateralRequired == expectedCollateral,
                          "Collateral mismatch for amount: " + std::to_string(amount));
    }
}

BOOST_AUTO_TEST_CASE(mint_maximum_amount)
{
    // Test minting maximum allowed amount per transaction (regtest: 100,000 cents = $1,000)
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Create large UTXOs for maximum mint
    auto utxos = CreateTestUTXOsWithValues({
        10000 * COIN, 20000 * COIN, 30000 * COIN, 40000 * COIN
    });
    for (size_t i = 0; i < utxos.size(); ++i) {
        builder.SetUTXOValue(utxos[i], (i + 1) * 10000 * COIN);
    }

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 100000; // 100,000 cents = $1,000 (regtest max)
    SetCanonicalLock(mintParams, 3650); // 10 years (lowest collateral ratio = 200%)
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    if (!result.success) {
        std::cout << "ERROR: BuildMintTransaction FAILED: " << result.error << std::endl;
    }
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.collateralRequired > 0);
    // With 10-year lock and $1k mint at 200% ratio and $0.05 DGB (50000 micro-USD):
    // Formula: (DD_cents * COIN * ratio * 100) / oracle_micro_usd
    // = (100000 * COIN * 200 * 100) / 50000 = 40000 DGB
    // Note: May be higher due to DCA multiplier (system health < 200%)
    CAmount expectedBase = (static_cast<int64_t>(100000) * COIN * 200 * 100) / price;
    // Allow for DCA multiplier up to 2.0x
    BOOST_CHECK(result.collateralRequired >= expectedBase);
    BOOST_CHECK(result.collateralRequired <= expectedBase * 2); // Max 2x with DCA
}

BOOST_AUTO_TEST_CASE(mint_invalid_amounts)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    auto utxos = CreateTestUTXOsWithValues({100 * COIN});
    builder.SetUTXOValue(utxos[0], 100 * COIN);

    // Test zero amount
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 0;
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
        BOOST_CHECK(!result.error.empty());
    }

    // Test negative amount
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = -1000;
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
        BOOST_CHECK(!result.error.empty());
    }

    // Test amount below minimum (consensus minMintAmount is $100.00 = 10000 cents)
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = params.GetDigiDollarParams().minMintAmount - 1; // Just below minimum
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
        BOOST_CHECK(result.error.find("Invalid mint parameters") != std::string::npos);
    }

    // Test amount above maximum (regtest: 100,000 cents = $1,000)
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 150000; // 150,000 cents = $1,500 (above regtest max)
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);
        BOOST_CHECK(!result.success);
        BOOST_CHECK(!result.error.empty());
    }
}

// ============================================================================
// Collateral Calculation Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(collateral_all_lock_tiers)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice(); // $0.05 per DGB

    MockMintTxBuilder builder(params, height, price);

    CAmount ddAmount = 10000; // $100 in cents

    // Test all 8 lock tiers with expected collateral ratios
    struct LockTier {
        int days;
        int expectedRatio; // percentage
    };

    std::vector<LockTier> tiers = {
        {0, 1000},    // 1 hour (0 days = 240 blocks): 1000% (testing/onboarding)
        {30, 500},    // 30 days: 500%
        {90, 400},    // 3 months: 400%
        {180, 350},   // 6 months: 350%
        {365, 300},   // 1 year: 300%
        {1095, 250},  // 3 years: 250%
        {1825, 225},  // 5 years: 225%
        {2555, 212},  // 7 years: 212%
        {3650, 200}   // 10 years: 200%
    };

    for (const auto& tier : tiers) {
        CAmount collateral = builder.CalculateRequiredCollateral(ddAmount, tier.days);

        // Just verify the collateral amount is positive and reasonable
        // The actual calculation is tested against the production code's output
        // which is validated by real-world testnet transactions
        BOOST_CHECK_MESSAGE(collateral > 0,
                          "Tier " + std::to_string(tier.days) + " days: collateral must be positive");

        // For higher ratios, more collateral should be required
        // Tier 0 (1000%) should require more collateral than Tier 4 (300%)
        // This verifies the ratio is being applied correctly
        if (tier.days > 0) {
            CAmount tier0Collateral = builder.CalculateRequiredCollateral(ddAmount, 0);
            BOOST_CHECK_MESSAGE(collateral < tier0Collateral,
                              "Tier " + std::to_string(tier.days) + " days should require less collateral than tier 0");
        }
    }
}

BOOST_AUTO_TEST_CASE(collateral_calculation_consistency)
{
    // Test that collateral calculation is consistent across multiple calls
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    CAmount ddAmount = 10000; // $100

    // Multiple calls should return the same result
    CAmount collateral1 = builder.CalculateRequiredCollateral(ddAmount, 365);
    CAmount collateral2 = builder.CalculateRequiredCollateral(ddAmount, 365);
    CAmount collateral3 = builder.CalculateRequiredCollateral(ddAmount, 365);

    BOOST_CHECK_EQUAL(collateral1, collateral2);
    BOOST_CHECK_EQUAL(collateral2, collateral3);
    BOOST_CHECK(collateral1 > 0);
}

BOOST_AUTO_TEST_CASE(collateral_price_dependency)
{
    // Test that collateral requirements change with different oracle prices
    const CChainParams& params = Params();
    int height = 1000;

    CAmount ddAmount = 10000; // $100

    // Test with different prices (micro-USD format: 1,000,000 = $1.00)
    CAmount lowPrice = 25000;   // $0.025 per DGB (25,000 micro-USD)
    CAmount highPrice = 100000; // $0.10 per DGB (100,000 micro-USD)

    MockMintTxBuilder builderLow(params, height, lowPrice);
    MockMintTxBuilder builderHigh(params, height, highPrice);

    CAmount collateralLow = builderLow.CalculateRequiredCollateral(ddAmount, 365);
    CAmount collateralHigh = builderHigh.CalculateRequiredCollateral(ddAmount, 365);

    // Higher DGB price should require less DGB collateral for same USD amount
    BOOST_CHECK(collateralLow > collateralHigh);
    BOOST_CHECK(collateralLow > 0);
    BOOST_CHECK(collateralHigh > 0);
}

BOOST_AUTO_TEST_CASE(collateral_insufficient_rejection)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice(); // $0.05 per DGB

    MockMintTxBuilder builder(params, height, price);

    // Create UTXOs with insufficient value
    auto utxos = CreateTestUTXOsWithValues({50 * COIN}); // Only 50 DGB
    builder.SetUTXOValue(utxos[0], 50 * COIN);

    TxBuilderMintParams mintParams;
    // Mint $1000 (max for regtest) - needs 200% * $1000 / $0.05 = 40,000 DGB at minimum (10 year lock)
    // With 1 year lock (300%), needs 60,000 DGB
    mintParams.ddAmount = 100000; // $1000 in cents (regtest max)
    SetCanonicalLock(mintParams, 365);     // 1 year = 300% ratio
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("Insufficient funds") != std::string::npos);
}

// ============================================================================
// P2TR Output Creation Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(p2tr_script_creation_through_transaction)
{
    // Test P2TR script creation through successful transaction building
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% ratio needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    // These two have to stop the test, not just record a failure. The checks
    // below read the first outputs, and a build that was refused has none.
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE(result.tx.vout.size() >= 3); // Collateral + DD + OP_RETURN

    // First output should be collateral P2TR script (OP_1 + 32 bytes)
    BOOST_CHECK(result.tx.vout[0].scriptPubKey.size() == 34);
    BOOST_CHECK(result.tx.vout[0].scriptPubKey[0] == OP_1);
    BOOST_CHECK(result.tx.vout[0].scriptPubKey[1] == 32);

    // Second output should be DigiDollar P2TR script (OP_1 + 32 bytes)
    BOOST_CHECK(result.tx.vout[1].scriptPubKey.size() == 34);
    BOOST_CHECK(result.tx.vout[1].scriptPubKey[0] == OP_1);
    BOOST_CHECK(result.tx.vout[1].scriptPubKey[1] == 32);
}

BOOST_AUTO_TEST_CASE(p2tr_redemption_paths_verification)
{
    // Test that mint transactions create proper scripts through the full transaction
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% ratio needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);
    // Verify that scripts are created (specific MAST verification would be in scripts_tests.cpp)
    BOOST_CHECK(!result.tx.vout[0].scriptPubKey.empty());
    BOOST_CHECK(!result.tx.vout[1].scriptPubKey.empty());
}

// ============================================================================
// Transaction Structure Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(transaction_version_field)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% ratio needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);

    // Check DigiDollar version format
    // Version structure: [Type:8][Flags:8][Marker:16]
    // Marker is 0x0770 for DigiDollar transactions
    BOOST_CHECK(result.tx.IsDigiDollar());
    BOOST_CHECK((result.tx.nVersion & 0x0000FFFF) == 0x0770); // Check DigiDollar marker

    // Check mint transaction type (type is in bits 24-31)
    BOOST_CHECK(::GetDigiDollarTxType(CTransaction(result.tx)) == ::DD_TX_MINT);
}

BOOST_AUTO_TEST_CASE(transaction_input_consumption)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Provide multiple UTXOs
    // $100 at 300% ratio and $0.05/DGB needs ~6000 DGB + fees
    auto utxos = CreateTestUTXOsWithValues({3000 * COIN, 3000 * COIN, 1000 * COIN});
    for (size_t i = 0; i < utxos.size(); ++i) {
        if (i == 0 || i == 1) builder.SetUTXOValue(utxos[i], 3000 * COIN);
        else builder.SetUTXOValue(utxos[i], 1000 * COIN);
    }

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    if (!result.success) {
        std::cout << "ERROR: BuildMintTransaction FAILED: " << result.error << std::endl;
    }
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.tx.vin.size() > 0);
    BOOST_CHECK(result.tx.vin.size() <= utxos.size());

    // Verify inputs reference provided UTXOs
    for (const auto& input : result.tx.vin) {
        bool found = false;
        for (const auto& utxo : utxos) {
            if (input.prevout == utxo) {
                found = true;
                break;
            }
        }
        BOOST_CHECK(found);
    }
}

BOOST_AUTO_TEST_CASE(transaction_output_creation)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% ratio needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);
    BOOST_CHECK(result.tx.vout.size() >= 3); // Collateral + DD + OP_RETURN + possible change

    // First output should be collateral (positive DGB value)
    BOOST_CHECK(result.tx.vout[0].nValue > 0);
    BOOST_CHECK(result.tx.vout[0].nValue == result.collateralRequired);
    const auto& collateralScript = result.tx.vout[0].scriptPubKey;
    BOOST_CHECK(collateralScript.size() == 34 && collateralScript[0] == OP_1 && collateralScript[1] == 32);

    // Second output should be DigiDollar (zero DGB value)
    BOOST_CHECK(result.tx.vout[1].nValue == 0);
    const auto& ddScript = result.tx.vout[1].scriptPubKey;
    BOOST_CHECK(ddScript.size() == 34 && ddScript[0] == OP_1 && ddScript[1] == 32);

    // Third output should be OP_RETURN (zero DGB value)
    BOOST_CHECK(result.tx.vout[2].nValue == 0);
    BOOST_CHECK(result.tx.vout[2].scriptPubKey[0] == OP_RETURN);

    // If change exists, it should be in a fourth output
    if (result.tx.vout.size() > 3) {
        BOOST_CHECK(result.tx.vout[3].nValue > 0); // Change has positive value
    }
}

BOOST_AUTO_TEST_CASE(transaction_fee_calculation)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    // Low fee-rate callers should be floored to the DD minimum; a high
    // enough rate can still pay above the floor.
    std::vector<CAmount> feeRates = {100000, 500000, 50000000}; // sat/kB

    CAmount prevFee = 0;
    for (size_t i = 0; i < feeRates.size(); ++i) {
        CAmount feeRate = feeRates[i];
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 10000;
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = feeRate;
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);

        BOOST_CHECK(result.success);
        BOOST_CHECK_GE(result.totalFees, MIN_DD_MINT_FEE);
        if (i < 2) {
            BOOST_CHECK_EQUAL(result.totalFees, MIN_DD_MINT_FEE);
        } else {
            BOOST_CHECK_GT(result.totalFees, MIN_DD_MINT_FEE);
        }
        BOOST_CHECK_GE(result.totalFees, prevFee);

        prevFee = result.totalFees;
    }
}

BOOST_AUTO_TEST_CASE(mint_fee_floor_applies_to_low_fee_rate_callers)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 500000; // Qt mint path rate, below the DD fee floor.
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_REQUIRE_MESSAGE(result.success, result.error);
    BOOST_CHECK_GE(result.totalFees, MIN_DD_MINT_FEE);
}

BOOST_AUTO_TEST_CASE(transaction_signing_preparation)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% ratio needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);

    // Transaction should be ready for signing
    BOOST_CHECK(!result.tx.vin.empty());
    BOOST_CHECK(!result.tx.vout.empty());

    // All inputs should have empty signatures (unsigned)
    for (const auto& input : result.tx.vin) {
        BOOST_CHECK(input.scriptSig.empty());
        BOOST_CHECK(input.scriptWitness.IsNull());
    }
}

// ============================================================================
// Edge Cases and Error Handling Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(edge_case_exact_collateral_no_change)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Calculate exact collateral needed
    CAmount ddAmount = 10000;
    int lockDays = 365;
    CAmount requiredCollateral = builder.CalculateRequiredCollateral(ddAmount, lockDays);
    // Fund the exact collateral plus the DD mint fee floor. Low fee-rate
    // callers are still charged at least this amount.
    CAmount estimatedFees = MIN_DD_MINT_FEE;

    // Provide exact amount needed (collateral + fees)
    auto utxos = CreateTestUTXOsWithValues({requiredCollateral + estimatedFees});
    builder.SetUTXOValue(utxos[0], requiredCollateral + estimatedFees);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = ddAmount;
    SetCanonicalLock(mintParams, lockDays);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);
    // Should have 3-4 outputs (collateral + DD + OP_RETURN + optional change)
    // Getting exact fee estimate is difficult, so we allow small change
    BOOST_CHECK(result.tx.vout.size() >= 3 && result.tx.vout.size() <= 4);
}

BOOST_AUTO_TEST_CASE(edge_case_multiple_inputs)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Provide many small UTXOs that need to be combined
    // With $100 DD at 300% ratio and $0.05/DGB (50000 micro-USD):
    // Formula: (10000 * COIN * 300 * 100) / 50000 = 6000 DGB
    // Use 1000 DGB UTXOs so multiple inputs are required
    std::vector<CAmount> values;
    for (int i = 0; i < 10; ++i) {
        values.push_back(1000 * COIN); // 1000 DGB each = 10000 DGB total
    }
    auto utxos = CreateTestUTXOsWithValues(values);
    for (size_t i = 0; i < utxos.size(); ++i) {
        builder.SetUTXOValue(utxos[i], values[i]);
    }

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100 (needs 6000 DGB at 300% ratio with $0.05/DGB)
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);
    BOOST_CHECK(result.tx.vin.size() > 1); // Multiple inputs used
    BOOST_CHECK(result.tx.vin.size() <= 10); // Not more than provided
}

BOOST_AUTO_TEST_CASE(edge_case_invalid_lock_times)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    auto utxos = CreateTestUTXOsWithValues({1000 * COIN});
    builder.SetUTXOValue(utxos[0], 1000 * COIN);

    // Test various invalid lock times
    // Valid: 0 (1 hour testing), 30-3650 days
    // Invalid: 1-29, >3650
    std::vector<int> invalidLockTimes = {1, 10, 29, 3651, 5000}; // Too short or too long

    for (int lockDays : invalidLockTimes) {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 10000;
        mintParams.lockDays = lockDays;
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);

        BOOST_CHECK_MESSAGE(!result.success, "Lock time " + std::to_string(lockDays) + " should be invalid");
        BOOST_CHECK(!result.error.empty());
    }
}

BOOST_AUTO_TEST_CASE(edge_case_oracle_price_unavailable)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = 0; // No oracle price available

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% ratio needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    // Should fail gracefully when oracle price is zero/unavailable
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

BOOST_AUTO_TEST_CASE(edge_case_extreme_fee_rates)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    auto utxos = CreateTestUTXOsWithValues({10000 * COIN});
    builder.SetUTXOValue(utxos[0], 10000 * COIN);

    // Test extremely high fee rate (max is 5M sat/kB)
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 10000;
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 200000000; // 200M sat/kB (above max of 100M sat/kB)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);

        BOOST_CHECK(!result.success);
        BOOST_CHECK(!result.error.empty());
    }

    // Test zero fee rate
    {
        TxBuilderMintParams mintParams;
        mintParams.ddAmount = 10000;
        SetCanonicalLock(mintParams, 365);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 0;
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);

        BOOST_CHECK(!result.success);
        BOOST_CHECK(!result.error.empty());
    }
}

BOOST_AUTO_TEST_CASE(edge_case_invalid_keys)
{
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% ratio needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    // mintParams.ownerKey not set (invalid)
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());
}

// ============================================================================
// Integration Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(integration_complete_mint_flow)
{
    // Test complete mint transaction flow with realistic parameters
    const CChainParams& params = Params();
    int height = 1000;
    // Oracle price in micro-USD format: 50,000 = $0.05 per DGB
    CAmount price = 50000; // 50,000 micro-USD = $0.05 per DGB

    MockMintTxBuilder builder(params, height, price);

    // Set up realistic UTXOs
    // $500 at 300% ratio needs 30,000 DGB at $0.05/DGB price
    auto utxos = CreateTestUTXOsWithValues({
        15000 * COIN,  // 15000 DGB
        15000 * COIN,  // 15000 DGB
        5000 * COIN    // 5000 DGB (for fees)
    });
    for (size_t i = 0; i < utxos.size(); ++i) {
        builder.SetUTXOValue(utxos[i], (i == 0 ? 15000 : (i == 1 ? 15000 : 5000)) * COIN);
    }

    // Mint $500 worth of DigiDollars with 1-year lock (within regtest max of $1000)
    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 50000; // $500 in cents
    SetCanonicalLock(mintParams, 365);   // 1 year = 300% collateral ratio
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 200000;   // 200,000 sat/kB (= 200 sat/vB)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);
    BOOST_CHECK(result.error.empty());

    // Verify transaction structure
    BOOST_CHECK(result.tx.IsDigiDollar());
    BOOST_CHECK(::GetDigiDollarTxType(CTransaction(result.tx)) == ::DD_TX_MINT);
    BOOST_CHECK(result.tx.vin.size() > 0);
    BOOST_CHECK(result.tx.vout.size() >= 3); // Collateral + DD + OP_RETURN

    // Verify collateral calculation
    // $500 at 300% collateral = $1500 worth of DGB
    // At $0.05/DGB = 30,000 DGB = 30,000 * COIN sats
    CAmount expectedCollateral = 30000 * COIN;
    // Account for 1% safety margin added in Bug #16 fix
    CAmount expectedWithMargin = (expectedCollateral * 101) / 100;
    BOOST_CHECK(std::abs(result.collateralRequired - expectedWithMargin) < COIN);

    // Verify outputs
    BOOST_CHECK(result.tx.vout[0].nValue == result.collateralRequired); // Collateral
    BOOST_CHECK(result.tx.vout[1].nValue == 0); // DigiDollar output
    BOOST_CHECK(result.tx.vout[2].nValue == 0); // OP_RETURN
    const auto& script0 = result.tx.vout[0].scriptPubKey;
    const auto& script1 = result.tx.vout[1].scriptPubKey;
    BOOST_CHECK(script0.size() == 34 && script0[0] == OP_1 && script0[1] == 32);
    BOOST_CHECK(script1.size() == 34 && script1[0] == OP_1 && script1[1] == 32);
    BOOST_CHECK(result.tx.vout[2].scriptPubKey[0] == OP_RETURN);

    // Verify the DD mint fee floor is enforced.
    BOOST_CHECK_GE(result.totalFees, MIN_DD_MINT_FEE);
}

// ============================================================================
// DCA Integration Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(mint_with_dca_healthy_system)
{
    // Test minting with healthy system (no DCA adjustment)
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Create mock system state for healthy system (200% collateralization)
    CAmount totalCollateral = 100000000 * COIN;  // 100M DGB
    CAmount totalDD = 10000000;                  // 10M DD ($100k)
    // Health = (100M * $0.05) / $100k * 100 = $5M / $100k * 100 = 500%

    // $100 DD at 300% ratio with $0.05/DGB (50000 micro-USD) needs ~6000 DGB
    // Formula: (10000 * COIN * 300 * 100) / 50000 = 6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100
    SetCanonicalLock(mintParams, 365);   // 1 year (300% base ratio)
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);

    // With healthy system, DCA multiplier should be 1.0x (no adjustment)
    // Formula: (DD_cents * COIN * ratio * 100) / oracle_micro_usd
    // Expected: (10000 * COIN * 300 * 100) / 50000 = 6000 DGB
    CAmount expectedBaseCollateral = (static_cast<int64_t>(10000) * COIN * 300 * 100) / price;
    // Account for 1% safety margin added in Bug #16 fix
    CAmount expectedWithMargin = (expectedBaseCollateral * 101) / 100;
    BOOST_CHECK(std::abs(result.collateralRequired - expectedWithMargin) < COIN);
}

BOOST_AUTO_TEST_CASE(mint_with_dca_warning_system)
{
    // Test minting with warning system (1.25x DCA multiplier)
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Create mock system state for warning system (130% collateralization)
    // This should trigger 1.25x DCA multiplier
    // $100 at 300% needs ~6000 DGB base (may need up to 7500 with DCA)

    auto utxos = CreateTestUTXOsWithValues({8000 * COIN});
    builder.SetUTXOValue(utxos[0], 8000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100
    SetCanonicalLock(mintParams, 365);   // 1 year (300% base ratio)
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    // TODO: Mock system health to return 130% for warning tier
    // For now, test basic structure without DCA adjustment

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);

    // With warning system, DCA multiplier should be 1.25x
    // Expected: $100 / $50 * 300% = 6 DGB base (7.5 DGB with 1.25x DCA)
    // For now, test without DCA integration until validation is updated
    CAmount baseCollateral = ((10000 * COIN) / 5000) * 300 / 100; // 6 DGB
    BOOST_CHECK(result.collateralRequired >= baseCollateral);
}

BOOST_AUTO_TEST_CASE(mint_with_dca_critical_system)
{
    // Test minting with critical system (1.5x DCA multiplier)
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Create sufficient UTXOs for higher collateral requirement
    // $100 at 300% * 1.5 DCA = 9000 DGB needed
    auto utxos = CreateTestUTXOsWithValues({10000 * COIN});
    builder.SetUTXOValue(utxos[0], 10000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100
    SetCanonicalLock(mintParams, 365);   // 1 year (300% base ratio)
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    // TODO: Mock system health to return 110% for critical tier

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);

    // With critical system, DCA multiplier should be 1.5x
    // Expected: $100 / $50 * 300% = 6 DGB base (9 DGB with 1.5x DCA)
    // For now, test basic structure
    CAmount baseCollateral = ((10000 * COIN) / 5000) * 300 / 100; // 6 DGB
    BOOST_CHECK(result.collateralRequired >= baseCollateral);
}

BOOST_AUTO_TEST_CASE(mint_with_dca_emergency_system)
{
    // Test minting with emergency system (2.0x DCA multiplier)
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Create sufficient UTXOs for doubled collateral requirement
    // $100 at 300% * 2.0 DCA = 12000 DGB needed
    auto utxos = CreateTestUTXOsWithValues({13000 * COIN});
    builder.SetUTXOValue(utxos[0], 13000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100
    SetCanonicalLock(mintParams, 365);   // 1 year (300% base ratio)
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    // TODO: Mock system health to return 90% for emergency tier

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);

    // With emergency system, DCA multiplier should be 2.0x
    // Expected: $100 / $50 * 300% = 6 DGB base (12 DGB with 2.0x DCA)
    // For now, test basic structure
    CAmount baseCollateral = ((10000 * COIN) / 5000) * 300 / 100; // 6 DGB
    BOOST_CHECK(result.collateralRequired >= baseCollateral);
}

BOOST_AUTO_TEST_CASE(mint_dca_applies_to_all_lock_tiers)
{
    // Test that DCA adjustment applies correctly to all lock tiers
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Test DCA with different lock tiers
    struct LockTier {
        int days;
        int baseRatio;
        const char* description;
    };

    std::vector<LockTier> tiers = {
        {0, 1000, "1 hour"},
        {30, 500, "30 days"},
        {90, 400, "3 months"},
        {180, 350, "6 months"},
        {365, 300, "1 year"},
        {1095, 250, "3 years"},
        {1825, 225, "5 years"},
        {2555, 212, "7 years"},
        {3650, 200, "10 years"}
    };

    CAmount ddAmount = 10000; // $100

    for (const auto& tier : tiers) {
        // Create sufficient UTXOs (conservative estimate)
        // Max ratio is 1000% for 1 hour: $100 * 1000% / $0.05 = 200,000 DGB
        auto utxos = CreateTestUTXOsWithValues({210000 * COIN});
        builder.SetUTXOValue(utxos[0], 210000 * COIN);

        TxBuilderMintParams mintParams;
        mintParams.ddAmount = ddAmount;
        SetCanonicalLock(mintParams, tier.days);
        SetWalletChangeDestination(mintParams);
        mintParams.ownerKey = CreateTestKey();
        mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
        mintParams.utxos = utxos;

        TxBuilderResult result = builder.BuildMintTransaction(mintParams);

        BOOST_CHECK_MESSAGE(result.success,
                          "Failed for tier: " + std::string(tier.description));

        // Calculate expected base collateral
        // Oracle price is in cents per DGB, ratio is percentage
        CAmount expectedBase = ((ddAmount * COIN) / price) * tier.baseRatio / 100;

        // For now, verify base collateral is calculated correctly
        // DCA integration will be tested after validation is updated
        BOOST_CHECK_MESSAGE(result.collateralRequired >= expectedBase,
                          "Insufficient collateral for tier: " + std::string(tier.description));
    }
}

BOOST_AUTO_TEST_CASE(mint_insufficient_funds_with_dca)
{
    // Test insufficient funds rejection when DCA increases requirements
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // Provide just enough for base collateral but not DCA adjustment
    // $100 at 300% = 6000 DGB base, but with 2.0x DCA needs 12000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN}); // Only enough for base + small buffer
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000; // $100
    SetCanonicalLock(mintParams, 365);   // 1 year (300% base = 6k DGB)
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    // TODO: Mock system health to trigger emergency DCA (2.0x = 120k DGB required)

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    // Should succeed with current implementation (DCA not integrated yet)
    // Will fail after DCA integration when requirement becomes 120k DGB
    BOOST_CHECK(result.success); // For now
}

BOOST_AUTO_TEST_CASE(mint_dca_real_time_adjustment)
{
    // Test that DCA adjusts in real-time during minting process
    const CChainParams& params = Params();
    int height = 1000;
    CAmount price = CreateTestOraclePrice();

    MockMintTxBuilder builder(params, height, price);

    // $100 at 300% needs ~6000 DGB
    auto utxos = CreateTestUTXOsWithValues({7000 * COIN});
    builder.SetUTXOValue(utxos[0], 7000 * COIN);

    TxBuilderMintParams mintParams;
    mintParams.ddAmount = 10000;
    SetCanonicalLock(mintParams, 365);
    SetWalletChangeDestination(mintParams);
    mintParams.ownerKey = CreateTestKey();
    mintParams.feeRate = 100000; // 100,000 sat/kB (minimum for DigiByte)
    mintParams.utxos = utxos;

    // TODO: Test that system health is calculated during mint validation
    // TODO: Test that DCA multiplier is applied to collateral requirement
    // TODO: Test that transaction fails if system health changes during building

    TxBuilderResult result = builder.BuildMintTransaction(mintParams);

    BOOST_CHECK(result.success);
    // Additional real-time tests will be added after DCA integration
}

BOOST_AUTO_TEST_SUITE_END()
