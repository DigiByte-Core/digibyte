// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <primitives/transaction.h>
#include <digidollar/digidollar.h>
#include <consensus/amount.h>
#include <uint256.h>
#include <pubkey.h>
#include <streams.h>
#include <util/strencodings.h>
#include <clientversion.h>

#include <boost/test/unit_test.hpp>
#include <limits>
#include <set>

BOOST_FIXTURE_TEST_SUITE(digidollar_structures_tests, BasicTestingSetup)

// =====================================
// DigiDollarTxType Enum Tests
// =====================================

BOOST_AUTO_TEST_CASE(digidollar_tx_type_values)
{
    // Test that all enum values are distinct and have expected values
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(DD_TX_NONE), 0);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(DD_TX_MINT), 1);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(DD_TX_TRANSFER), 2);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(DD_TX_REDEEM), 3);

    // Ensure all values are distinct
    std::set<uint8_t> values = {
        static_cast<uint8_t>(DD_TX_NONE),
        static_cast<uint8_t>(DD_TX_MINT),
        static_cast<uint8_t>(DD_TX_TRANSFER),
        static_cast<uint8_t>(DD_TX_REDEEM)
    };
    BOOST_CHECK_EQUAL(values.size(), 4);
}

BOOST_AUTO_TEST_CASE(digidollar_tx_version_marker)
{
    // Test the DigiDollar transaction version marker
    BOOST_CHECK_EQUAL(DD_TX_VERSION, 0x0D1D0770); // "DigiDollar" marker
}

// =====================================
// CDigiDollarOutput Tests
// =====================================

BOOST_AUTO_TEST_CASE(digidollar_output_construction)
{
    CDigiDollarOutput output;

    // Test default construction
    BOOST_CHECK_EQUAL(output.nDDAmount, 0);
    BOOST_CHECK(output.collateralId.IsNull());
    BOOST_CHECK_EQUAL(output.nLockTime, 0);
}

BOOST_AUTO_TEST_CASE(digidollar_output_basic_initialization)
{
    CDigiDollarOutput output;

    // Set values
    output.nDDAmount = 10000; // $100.00 in cents
    output.collateralId = uint256S("0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    output.nLockTime = 144; // 144 blocks

    BOOST_CHECK_EQUAL(output.nDDAmount, 10000);
    BOOST_CHECK_EQUAL(output.collateralId.ToString(), "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    BOOST_CHECK_EQUAL(output.nLockTime, 144);
}

BOOST_AUTO_TEST_CASE(digidollar_output_validation)
{
    CDigiDollarOutput output;

    // Test invalid cases
    output.nDDAmount = -1;
    BOOST_CHECK(!output.IsValid()); // Negative amount should be invalid

    output.nDDAmount = 0;
    BOOST_CHECK(!output.IsValid()); // Zero amount should be invalid

    output.nDDAmount = MAX_MONEY + 1;
    BOOST_CHECK(!output.IsValid()); // Amount exceeding MAX_MONEY should be invalid

    // Test negative lock time
    output.nDDAmount = 100; // $1.00
    output.nLockTime = -1;
    BOOST_CHECK(!output.IsValid()); // Negative lock time should be invalid

    // Test valid case
    output.nDDAmount = 100; // $1.00
    output.nLockTime = 144;
    output.collateralId = uint256S("0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    BOOST_CHECK(output.IsValid()); // Should be valid now
}

BOOST_AUTO_TEST_CASE(digidollar_output_usd_value)
{
    CDigiDollarOutput output;

    output.nDDAmount = 100; // $1.00 in cents
    BOOST_CHECK_EQUAL(output.GetUSDValue(), 100);

    output.nDDAmount = 12345; // $123.45 in cents
    BOOST_CHECK_EQUAL(output.GetUSDValue(), 12345);

    output.nDDAmount = 0;
    BOOST_CHECK_EQUAL(output.GetUSDValue(), 0);
}

BOOST_AUTO_TEST_CASE(digidollar_output_serialization)
{
    CDigiDollarOutput original;
    original.nDDAmount = 54321; // $543.21
    original.collateralId = uint256S("0xabcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    original.nLockTime = 288; // 288 blocks

    // Serialize
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << original;

    // Deserialize
    CDigiDollarOutput deserialized;
    ss >> deserialized;

    // Verify round-trip
    BOOST_CHECK_EQUAL(original.nDDAmount, deserialized.nDDAmount);
    BOOST_CHECK_EQUAL(original.collateralId, deserialized.collateralId);
    BOOST_CHECK_EQUAL(original.nLockTime, deserialized.nLockTime);
}

BOOST_AUTO_TEST_CASE(digidollar_output_edge_cases)
{
    CDigiDollarOutput output;

    // Test maximum valid amount
    output.nDDAmount = MAX_DIGIDOLLAR;
    output.nLockTime = 0;
    output.collateralId = uint256S("0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    BOOST_CHECK(output.IsValid());

    // Test minimum valid amount
    output.nDDAmount = 1; // 1 cent
    BOOST_CHECK(output.IsValid());

    // Test large lock time
    output.nLockTime = INT64_MAX;
    BOOST_CHECK(output.IsValid());
}

// =====================================
// CCollateralPosition Tests
// =====================================

BOOST_AUTO_TEST_CASE(collateral_position_construction)
{
    CCollateralPosition position;

    // Test default construction
    BOOST_CHECK(position.outpoint.IsNull());
    BOOST_CHECK_EQUAL(position.dgbLocked, 0);
    BOOST_CHECK_EQUAL(position.ddMinted, 0);
    BOOST_CHECK_EQUAL(position.unlockHeight, 0);
    BOOST_CHECK_EQUAL(position.collateralRatio, 0);
    BOOST_CHECK(position.availablePaths.empty());
}

BOOST_AUTO_TEST_CASE(collateral_position_redemption_paths)
{
    // Test RedemptionPath enum values
    BOOST_CHECK_EQUAL(static_cast<int>(CCollateralPosition::PATH_NORMAL), 0);
    BOOST_CHECK_EQUAL(static_cast<int>(CCollateralPosition::PATH_ERR), 1);
}

BOOST_AUTO_TEST_CASE(collateral_position_basic_setup)
{
    CCollateralPosition position;

    // Set up a basic position
    position.outpoint = COutPoint(uint256S("0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"), 0);
    position.dgbLocked = 1000 * COIN; // 1000 DGB
    position.ddMinted = 50000; // $500.00 in cents
    position.unlockHeight = 100000;
    position.collateralRatio = 200; // 200% collateral ratio

    BOOST_CHECK(!position.outpoint.IsNull());
    BOOST_CHECK_EQUAL(position.dgbLocked, 1000 * COIN);
    BOOST_CHECK_EQUAL(position.ddMinted, 50000);
    BOOST_CHECK_EQUAL(position.unlockHeight, 100000);
    BOOST_CHECK_EQUAL(position.collateralRatio, 200);
}

BOOST_AUTO_TEST_CASE(collateral_position_ratio_calculation)
{
    CCollateralPosition position;
    position.dgbLocked = 1000 * COIN; // 1000 DGB
    position.ddMinted = 50000; // $500.00 in cents

    // Test with DGB price = $1.00 per DGB (100 cents)
    CAmount dgbPrice = 100; // 100 cents per DGB
    CAmount expectedRatio = (position.dgbLocked / COIN * dgbPrice * 100) / position.ddMinted;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(dgbPrice), expectedRatio);

    // Test with DGB price = $0.50 per DGB (50 cents)
    dgbPrice = 50;
    expectedRatio = (position.dgbLocked / COIN * dgbPrice * 100) / position.ddMinted;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(dgbPrice), expectedRatio);

    // Test with DGB price = $2.00 per DGB (200 cents)
    dgbPrice = 200;
    expectedRatio = (position.dgbLocked / COIN * dgbPrice * 100) / position.ddMinted;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(dgbPrice), expectedRatio);
}

BOOST_AUTO_TEST_CASE(collateral_position_health_check)
{
    CCollateralPosition position;
    position.dgbLocked = 1000 * COIN; // 1000 DGB
    position.ddMinted = 50000; // $500.00 in cents

    // Test healthy position (200% collateral ratio)
    CAmount dgbPrice = 100; // $1.00 per DGB
    BOOST_CHECK(position.IsHealthy(dgbPrice)); // 1000 DGB * $1.00 = $1000 vs $500 minted = 200%

    // Test exactly at 100% (borderline healthy)
    dgbPrice = 50; // $0.50 per DGB
    BOOST_CHECK(position.IsHealthy(dgbPrice)); // 1000 DGB * $0.50 = $500 vs $500 minted = 100%

    // Test unhealthy position (below 100%)
    dgbPrice = 25; // $0.25 per DGB
    BOOST_CHECK(!position.IsHealthy(dgbPrice)); // 1000 DGB * $0.25 = $250 vs $500 minted = 50%
}

BOOST_AUTO_TEST_CASE(collateral_position_redemption_amount_normal)
{
    CCollateralPosition position;
    position.ddMinted = 10000; // $100.00 in cents

    // Test normal system collateral (above 100%)
    int systemCollateral = 150; // 150%
    BOOST_CHECK_EQUAL(position.GetRequiredDDForRedemption(systemCollateral), position.ddMinted);

    systemCollateral = 100; // Exactly 100%
    BOOST_CHECK_EQUAL(position.GetRequiredDDForRedemption(systemCollateral), position.ddMinted);
}

BOOST_AUTO_TEST_CASE(collateral_position_redemption_amount_err)
{
    CCollateralPosition position;
    position.ddMinted = 10000; // $100.00 in cents

    // Test Emergency Redemption Ratio (ERR) when system below 100%
    int systemCollateral = 80; // 80% system collateral

    // ERR formula: required = ddMinted * (100 / systemCollateral)
    CAmount expected = (position.ddMinted * 100) / systemCollateral;
    BOOST_CHECK_EQUAL(position.GetRequiredDDForRedemption(systemCollateral), expected);

    // Test another ERR case
    systemCollateral = 90; // 90% system collateral
    expected = (position.ddMinted * 100) / systemCollateral;
    BOOST_CHECK_EQUAL(position.GetRequiredDDForRedemption(systemCollateral), expected);
}

BOOST_AUTO_TEST_CASE(collateral_position_available_paths)
{
    CCollateralPosition position;

    // Test adding redemption paths
    position.availablePaths.push_back(CCollateralPosition::PATH_NORMAL);
    position.availablePaths.push_back(CCollateralPosition::PATH_ERR);

    BOOST_CHECK_EQUAL(position.availablePaths.size(), 2);
    BOOST_CHECK_EQUAL(position.availablePaths[0], CCollateralPosition::PATH_NORMAL);
    BOOST_CHECK_EQUAL(position.availablePaths[1], CCollateralPosition::PATH_ERR);
}

BOOST_AUTO_TEST_CASE(collateral_position_edge_cases)
{
    CCollateralPosition position;

    // Test zero values
    position.dgbLocked = 0;
    position.ddMinted = 0;

    // Should handle division by zero gracefully
    CAmount ratio = position.GetCurrentCollateralRatio(100);
    BOOST_CHECK_EQUAL(ratio, 0); // Or handle as appropriate for zero ddMinted

    // Test with zero DD minted but DGB locked
    position.dgbLocked = 1000 * COIN;
    position.ddMinted = 0;

    // This should be handled gracefully (no division by zero)
    BOOST_CHECK_NO_THROW(position.GetCurrentCollateralRatio(100));

    // Test redemption amount with zero system collateral
    position.ddMinted = 10000;
    BOOST_CHECK_THROW(position.GetRequiredDDForRedemption(0), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(collateral_position_ratio_never_wraps_round)
{
    CCollateralPosition position;

    // Every DGB there will ever be, locked against one cent of DigiDollar, at
    // a very high price. Multiplying the locked satoshis by the price does not
    // fit in a 64-bit money amount, but the ratio itself does, so the exact
    // answer must come back.
    position.dgbLocked = MAX_MONEY;
    position.ddMinted = 1;
    const CAmount highPrice = 1000000; // $10,000 per DGB, in cents

    const CAmount expectedRatio = (MAX_MONEY / COIN) * highPrice * 100;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(highPrice), expectedRatio);

    // A thousand times that price puts the true ratio past what a money amount
    // can hold. The answer is then the largest amount, a ceiling, and never a
    // wrapped-round or negative number.
    const CAmount absurdPrice = 1000000000;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(absurdPrice),
                      std::numeric_limits<CAmount>::max());

    // Damaged data has no honest ratio, so nothing is reported for it.
    position.dgbLocked = -1;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(highPrice), 0);
    position.dgbLocked = MAX_MONEY;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(-1), 0);
    position.ddMinted = -1;
    BOOST_CHECK_EQUAL(position.GetCurrentCollateralRatio(highPrice), 0);

    // Redemption with a very large minted amount and 1% system collateral.
    position.ddMinted = MAX_MONEY / 100;
    BOOST_CHECK_NO_THROW(position.GetRequiredDDForRedemption(1));
}

BOOST_AUTO_TEST_SUITE_END()