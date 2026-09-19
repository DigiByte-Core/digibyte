// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The DigiDollar RPC amount parser is the one place caller text becomes cents.
// These cases pin every branch of its contract: integer cents with no unit,
// explicit cents, explicit dollars with at most two decimals, the refusal of
// a decimal point without a unit, and the syntax and magnitude limits.

#include <digidollar/amount.h>

#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <string>

using DigiDollar::DDAmountError;
using DigiDollar::DDAmountUnit;
using DigiDollar::DD_AMOUNT_NO_CAP;
using DigiDollar::MAX_DD_RPC_AMOUNT_CENTS;
using DigiDollar::ParseDDAmount;

BOOST_FIXTURE_TEST_SUITE(digidollar_amount_tests, BasicTestingSetup)

namespace {

CAmount ParseOk(const std::string& text, DDAmountUnit unit, CAmount max_cents = MAX_DD_RPC_AMOUNT_CENTS)
{
    const auto result = ParseDDAmount(text, unit, max_cents);
    BOOST_CHECK_MESSAGE(result.ok(), "expected \"" << text << "\" (" << DigiDollar::DDAmountUnitName(unit)
                                                   << ") to parse, got: " << result.message);
    BOOST_CHECK(result.message.empty());
    return result.cents;
}

void ParseFails(const std::string& text, DDAmountUnit unit, DDAmountError expected,
                CAmount max_cents = MAX_DD_RPC_AMOUNT_CENTS)
{
    const auto result = ParseDDAmount(text, unit, max_cents);
    BOOST_CHECK_MESSAGE(!result.ok(), "expected \"" << text << "\" (" << DigiDollar::DDAmountUnitName(unit)
                                                    << ") to be refused, got " << result.cents << " cents");
    BOOST_CHECK_MESSAGE(result.error == expected, "\"" << text << "\": wrong refusal reason: " << result.message);
    BOOST_CHECK(!result.message.empty());
    BOOST_CHECK_EQUAL(result.cents, 0);
}

} // namespace

BOOST_AUTO_TEST_CASE(integer_without_unit_is_cents)
{
    // The behaviour every existing integer-cents caller relies on.
    BOOST_CHECK_EQUAL(ParseOk("10000", DDAmountUnit::UNSPECIFIED), 10000);
    BOOST_CHECK_EQUAL(ParseOk("1", DDAmountUnit::UNSPECIFIED), 1);
    BOOST_CHECK_EQUAL(ParseOk("0", DDAmountUnit::UNSPECIFIED), 0);
    BOOST_CHECK_EQUAL(ParseOk("10000000", DDAmountUnit::UNSPECIFIED), MAX_DD_RPC_AMOUNT_CENTS);
}

BOOST_AUTO_TEST_CASE(decimal_without_unit_is_refused_and_names_the_fix)
{
    // "10000.00" used to be read as $10,000.00 while "10000" was $100.00.
    // Neither reading may be guessed any more.
    for (const std::string text : {"10000.00", "100.00", "0.50", "1.0", "5.5"}) {
        ParseFails(text, DDAmountUnit::UNSPECIFIED, DDAmountError::AMBIGUOUS_UNIT);
        const auto result = ParseDDAmount(text, DDAmountUnit::UNSPECIFIED, MAX_DD_RPC_AMOUNT_CENTS);
        BOOST_CHECK_EQUAL(result.message, DigiDollar::DD_AMOUNT_AMBIGUOUS_MESSAGE);
        BOOST_CHECK(result.message.find("pass amount_unit=cents or amount_unit=dollars") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(explicit_cents_must_be_an_integer)
{
    BOOST_CHECK_EQUAL(ParseOk("10000", DDAmountUnit::CENTS), 10000);
    BOOST_CHECK_EQUAL(ParseOk("0", DDAmountUnit::CENTS), 0);
    // Even a whole-number decimal is refused: the unit says integer cents.
    ParseFails("10000.00", DDAmountUnit::CENTS, DDAmountError::CENTS_NOT_INTEGRAL);
    ParseFails("12.34", DDAmountUnit::CENTS, DDAmountError::CENTS_NOT_INTEGRAL);
    ParseFails("0.5", DDAmountUnit::CENTS, DDAmountError::CENTS_NOT_INTEGRAL);
}

BOOST_AUTO_TEST_CASE(explicit_dollars_allow_at_most_two_decimals)
{
    BOOST_CHECK_EQUAL(ParseOk("12.34", DDAmountUnit::DOLLARS), 1234);
    BOOST_CHECK_EQUAL(ParseOk("12.3", DDAmountUnit::DOLLARS), 1230);
    BOOST_CHECK_EQUAL(ParseOk("12", DDAmountUnit::DOLLARS), 1200);
    BOOST_CHECK_EQUAL(ParseOk("12.00", DDAmountUnit::DOLLARS), 1200);
    BOOST_CHECK_EQUAL(ParseOk("0.01", DDAmountUnit::DOLLARS), 1);
    BOOST_CHECK_EQUAL(ParseOk("0.10", DDAmountUnit::DOLLARS), 10);
    BOOST_CHECK_EQUAL(ParseOk("0", DDAmountUnit::DOLLARS), 0);
    BOOST_CHECK_EQUAL(ParseOk("0.00", DDAmountUnit::DOLLARS), 0);
    BOOST_CHECK_EQUAL(ParseOk("100000.00", DDAmountUnit::DOLLARS), MAX_DD_RPC_AMOUNT_CENTS);
    BOOST_CHECK_EQUAL(ParseOk("100000", DDAmountUnit::DOLLARS), MAX_DD_RPC_AMOUNT_CENTS);

    // A third decimal would be a fraction of a cent.
    ParseFails("12.345", DDAmountUnit::DOLLARS, DDAmountError::TOO_MANY_DECIMALS);
    ParseFails("12.340", DDAmountUnit::DOLLARS, DDAmountError::TOO_MANY_DECIMALS);
    ParseFails("0.001", DDAmountUnit::DOLLARS, DDAmountError::TOO_MANY_DECIMALS);
}

BOOST_AUTO_TEST_CASE(exponents_are_refused)
{
    // "1e3" must never be read as 1000 of anything: a caller cannot tell at a
    // glance whether it meant cents or dollars, and neither can we.
    for (const auto unit : {DDAmountUnit::UNSPECIFIED, DDAmountUnit::CENTS, DDAmountUnit::DOLLARS}) {
        ParseFails("1e3", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1E3", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1e-2", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1.5e2", unit, DDAmountError::NOT_A_NUMBER);
    }
}

BOOST_AUTO_TEST_CASE(negative_amounts_are_refused)
{
    for (const auto unit : {DDAmountUnit::UNSPECIFIED, DDAmountUnit::CENTS, DDAmountUnit::DOLLARS}) {
        ParseFails("-1", unit, DDAmountError::NEGATIVE);
        ParseFails("-0", unit, DDAmountError::NEGATIVE);
        ParseFails("-12.34", unit, DDAmountError::NEGATIVE);
        ParseFails("-", unit, DDAmountError::NEGATIVE);
    }
}

BOOST_AUTO_TEST_CASE(leading_plus_is_refused)
{
    for (const auto unit : {DDAmountUnit::UNSPECIFIED, DDAmountUnit::CENTS, DDAmountUnit::DOLLARS}) {
        ParseFails("+1", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("+12.34", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("+", unit, DDAmountError::NOT_A_NUMBER);
    }
}

BOOST_AUTO_TEST_CASE(whitespace_is_refused)
{
    for (const auto unit : {DDAmountUnit::UNSPECIFIED, DDAmountUnit::CENTS, DDAmountUnit::DOLLARS}) {
        ParseFails(" 1", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1 ", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1 000", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("\t12.34", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("12.34\n", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails(" ", unit, DDAmountError::NOT_A_NUMBER);
    }
}

BOOST_AUTO_TEST_CASE(malformed_text_is_refused)
{
    for (const auto unit : {DDAmountUnit::UNSPECIFIED, DDAmountUnit::CENTS, DDAmountUnit::DOLLARS}) {
        ParseFails("", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails(".", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails(".5", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("5.", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1.2.3", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1,000", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("$100", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("100$", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("0x10", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("abc", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("NaN", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("inf", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("Infinity", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("1_000", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("\xd9\xa3", unit, DDAmountError::NOT_A_NUMBER); // Arabic-Indic digit three
        // Leading zeros hide the magnitude; a lone "0" before the point is fine.
        ParseFails("00", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("007", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("0100", unit, DDAmountError::NOT_A_NUMBER);
        ParseFails("00.5", unit, DDAmountError::NOT_A_NUMBER);
    }
}

BOOST_AUTO_TEST_CASE(syntax_is_checked_before_unit_and_magnitude)
{
    // Broken syntax wins over the unit rule, and the sign wins over both,
    // so the caller sees the most basic problem first.
    ParseFails("-10000.00", DDAmountUnit::UNSPECIFIED, DDAmountError::NEGATIVE);
    ParseFails("10000.00x", DDAmountUnit::UNSPECIFIED, DDAmountError::NOT_A_NUMBER);
    ParseFails("1e3.5", DDAmountUnit::DOLLARS, DDAmountError::NOT_A_NUMBER);
    ParseFails("99999999999999999999999.999", DDAmountUnit::DOLLARS, DDAmountError::TOO_MANY_DECIMALS);
}

BOOST_AUTO_TEST_CASE(cap_boundary_is_exact)
{
    // 10,000,000 cents = $100,000.00 is allowed; one cent more is not.
    BOOST_CHECK_EQUAL(ParseOk("10000000", DDAmountUnit::UNSPECIFIED), MAX_DD_RPC_AMOUNT_CENTS);
    BOOST_CHECK_EQUAL(ParseOk("10000000", DDAmountUnit::CENTS), MAX_DD_RPC_AMOUNT_CENTS);
    BOOST_CHECK_EQUAL(ParseOk("100000.00", DDAmountUnit::DOLLARS), MAX_DD_RPC_AMOUNT_CENTS);
    BOOST_CHECK_EQUAL(ParseOk("100000.0", DDAmountUnit::DOLLARS), MAX_DD_RPC_AMOUNT_CENTS);
    BOOST_CHECK_EQUAL(ParseOk("100000", DDAmountUnit::DOLLARS), MAX_DD_RPC_AMOUNT_CENTS);

    ParseFails("10000001", DDAmountUnit::UNSPECIFIED, DDAmountError::TOO_LARGE);
    ParseFails("10000001", DDAmountUnit::CENTS, DDAmountError::TOO_LARGE);
    ParseFails("100000.01", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE);
    ParseFails("100001", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE);
    ParseFails("100000.1", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE);
    ParseFails("1000000.00", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE);

    const auto over = ParseDDAmount("10000001", DDAmountUnit::CENTS, MAX_DD_RPC_AMOUNT_CENTS);
    BOOST_CHECK(over.message.find("10000000 cents") != std::string::npos);
    BOOST_CHECK(over.message.find("$100000.00") != std::string::npos);

    // A caller-chosen limit is honoured exactly, including tiny ones.
    BOOST_CHECK_EQUAL(ParseOk("5", DDAmountUnit::CENTS, 5), 5);
    ParseFails("6", DDAmountUnit::CENTS, DDAmountError::TOO_LARGE, 5);
    BOOST_CHECK_EQUAL(ParseOk("0", DDAmountUnit::CENTS, 0), 0);
    ParseFails("1", DDAmountUnit::CENTS, DDAmountError::TOO_LARGE, 0);
    ParseFails("0.01", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE, 0);
    BOOST_CHECK_EQUAL(ParseOk("0.05", DDAmountUnit::DOLLARS, 5), 5);
    ParseFails("0.06", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE, 5);
    ParseFails("1", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE, 99);
    BOOST_CHECK_EQUAL(ParseOk("1", DDAmountUnit::DOLLARS, 100), 100);
    // A negative limit behaves like zero rather than accepting anything.
    ParseFails("1", DDAmountUnit::CENTS, DDAmountError::TOO_LARGE, -1);
    BOOST_CHECK_EQUAL(ParseOk("0", DDAmountUnit::CENTS, -1), 0);
}

BOOST_AUTO_TEST_CASE(huge_values_never_overflow)
{
    const CAmount max = std::numeric_limits<CAmount>::max(); // 9223372036854775807
    BOOST_CHECK_EQUAL(ParseOk("9223372036854775807", DDAmountUnit::CENTS, DD_AMOUNT_NO_CAP), max);
    ParseFails("9223372036854775808", DDAmountUnit::CENTS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);
    ParseFails("18446744073709551616", DDAmountUnit::CENTS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);
    BOOST_CHECK_EQUAL(ParseOk("92233720368547758.07", DDAmountUnit::DOLLARS, DD_AMOUNT_NO_CAP), max);
    ParseFails("92233720368547758.08", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);
    ParseFails("92233720368547759", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);

    // Absurdly long digit strings are refused early instead of wrapping.
    const std::string long_digits(400, '9');
    ParseFails(long_digits, DDAmountUnit::CENTS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);
    ParseFails(long_digits, DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);
    ParseFails(long_digits, DDAmountUnit::UNSPECIFIED, DDAmountError::TOO_LARGE);
    ParseFails("1" + std::string(400, '0'), DDAmountUnit::CENTS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);
    ParseFails("1" + std::string(400, '0') + ".00", DDAmountUnit::DOLLARS, DDAmountError::TOO_LARGE, DD_AMOUNT_NO_CAP);
}

BOOST_AUTO_TEST_CASE(unit_names_are_exact)
{
    BOOST_CHECK(DigiDollar::ParseDDAmountUnit("cents") == DDAmountUnit::CENTS);
    BOOST_CHECK(DigiDollar::ParseDDAmountUnit("dollars") == DDAmountUnit::DOLLARS);
    for (const std::string bad : {"", "Cents", "DOLLARS", "cent", "dollar", "usd", "$", " cents", "cents ", "unspecified"}) {
        BOOST_CHECK_MESSAGE(!DigiDollar::ParseDDAmountUnit(bad).has_value(), "unit name \"" << bad << "\" must be refused");
    }
    BOOST_CHECK_EQUAL(DigiDollar::DDAmountUnitName(DDAmountUnit::CENTS), "cents");
    BOOST_CHECK_EQUAL(DigiDollar::DDAmountUnitName(DDAmountUnit::DOLLARS), "dollars");
    BOOST_CHECK_EQUAL(DigiDollar::DDAmountUnitName(DDAmountUnit::UNSPECIFIED), "unspecified");
}

BOOST_AUTO_TEST_CASE(formatting_round_trips)
{
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountDollars(0), "0.00");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountDollars(5), "0.05");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountDollars(50), "0.50");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountDollars(1234), "12.34");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountDollars(MAX_DD_RPC_AMOUNT_CENTS), "100000.00");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountDollars(-1234), "-12.34");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountDollars(std::numeric_limits<CAmount>::min()), "-92233720368547758.08");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountCents(1234), "1234");
    BOOST_CHECK_EQUAL(DigiDollar::FormatDDAmountCents(0), "0");

    for (const CAmount cents : {CAmount{0}, CAmount{1}, CAmount{99}, CAmount{100}, CAmount{1234}, CAmount{5000},
                                MAX_DD_RPC_AMOUNT_CENTS - 1, MAX_DD_RPC_AMOUNT_CENTS}) {
        BOOST_CHECK_EQUAL(ParseOk(DigiDollar::FormatDDAmountDollars(cents), DDAmountUnit::DOLLARS), cents);
        BOOST_CHECK_EQUAL(ParseOk(DigiDollar::FormatDDAmountCents(cents), DDAmountUnit::CENTS), cents);
        BOOST_CHECK_EQUAL(ParseOk(DigiDollar::FormatDDAmountCents(cents), DDAmountUnit::UNSPECIFIED), cents);
        // Cross-check the dollar arithmetic against the node's own fixed-point parser.
        int64_t fixed_point = 0;
        BOOST_CHECK(ParseFixedPoint(DigiDollar::FormatDDAmountDollars(cents), 2, &fixed_point));
        BOOST_CHECK_EQUAL(fixed_point, cents);
    }
}

BOOST_AUTO_TEST_CASE(dollars_and_cents_agree_on_integers)
{
    // An integer read as dollars is exactly 100 times the same integer read as cents.
    for (const std::string text : {"1", "7", "50", "999", "100000"}) {
        const CAmount as_cents = ParseOk(text, DDAmountUnit::CENTS, DD_AMOUNT_NO_CAP);
        const CAmount as_dollars = ParseOk(text, DDAmountUnit::DOLLARS, DD_AMOUNT_NO_CAP);
        BOOST_CHECK_EQUAL(as_dollars, as_cents * 100);
    }
}

BOOST_AUTO_TEST_SUITE_END()
