// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <digidollar/amount.h>

#include <tinyformat.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace DigiDollar {

namespace {

bool IsAsciiDigit(char c)
{
    return c >= '0' && c <= '9';
}

// value = value * 10 + digit, refused instead of overflowing or exceeding max.
bool AppendDigitChecked(CAmount& value, int digit, CAmount max)
{
    if (digit > max) return false; // keeps (max - digit) non-negative below
    if (value > (max - digit) / 10) return false;
    value = value * 10 + digit;
    return true;
}

DDAmountParseResult Refuse(DDAmountError error, std::string message)
{
    DDAmountParseResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

} // namespace

std::optional<DDAmountUnit> ParseDDAmountUnit(std::string_view name)
{
    if (name == "cents") return DDAmountUnit::CENTS;
    if (name == "dollars") return DDAmountUnit::DOLLARS;
    return std::nullopt;
}

std::string DDAmountUnitName(DDAmountUnit unit)
{
    switch (unit) {
    case DDAmountUnit::CENTS: return "cents";
    case DDAmountUnit::DOLLARS: return "dollars";
    case DDAmountUnit::UNSPECIFIED: break;
    }
    return "unspecified";
}

DDAmountParseResult ParseDDAmount(std::string_view text, DDAmountUnit unit, CAmount max_cents)
{
    if (max_cents < 0) max_cents = 0;

    if (text.empty()) {
        return Refuse(DDAmountError::NOT_A_NUMBER, "amount is not a plain decimal number (it is empty)");
    }
    if (text.front() == '-') {
        return Refuse(DDAmountError::NEGATIVE, "amount must not be negative");
    }

    // Syntax first: <digits>[.<digits>] and nothing else. Only after the
    // whole text is known to be a plain decimal number do the unit rules and
    // the magnitude get a say, so a caller always sees the most basic
    // problem first.
    std::string_view integer_part;
    std::string_view fraction_part;
    {
        size_t pos = 0;
        while (pos < text.size() && IsAsciiDigit(text[pos])) ++pos;
        integer_part = text.substr(0, pos);
        if (integer_part.empty()) {
            return Refuse(DDAmountError::NOT_A_NUMBER,
                          "amount is not a plain decimal number (digits, optionally followed by a decimal point and more digits)");
        }
        if (integer_part.size() > 1 && integer_part.front() == '0') {
            return Refuse(DDAmountError::NOT_A_NUMBER, "amount must not have leading zeros");
        }
        if (pos < text.size()) {
            if (text[pos] != '.') {
                return Refuse(DDAmountError::NOT_A_NUMBER,
                              "amount is not a plain decimal number (no sign, spaces, exponent, or separators are accepted)");
            }
            ++pos;
            const size_t fraction_start = pos;
            while (pos < text.size() && IsAsciiDigit(text[pos])) ++pos;
            fraction_part = text.substr(fraction_start, pos - fraction_start);
            if (fraction_part.empty()) {
                return Refuse(DDAmountError::NOT_A_NUMBER, "amount has a decimal point with no digits after it");
            }
            if (pos != text.size()) {
                return Refuse(DDAmountError::NOT_A_NUMBER,
                              "amount is not a plain decimal number (no sign, spaces, exponent, or separators are accepted)");
            }
        }
    }
    const bool has_point = !fraction_part.empty();

    // Unit rules. A decimal point is only ever read as dollars when the
    // caller said "dollars"; it is never inferred from the shape of the text.
    switch (unit) {
    case DDAmountUnit::UNSPECIFIED:
        if (has_point) return Refuse(DDAmountError::AMBIGUOUS_UNIT, DD_AMOUNT_AMBIGUOUS_MESSAGE);
        break;
    case DDAmountUnit::CENTS:
        if (has_point) {
            return Refuse(DDAmountError::CENTS_NOT_INTEGRAL,
                          "amount_unit=cents requires an integer number of cents (no decimal point)");
        }
        break;
    case DDAmountUnit::DOLLARS:
        if (fraction_part.size() > 2) {
            return Refuse(DDAmountError::TOO_MANY_DECIMALS,
                          "amount_unit=dollars allows at most two decimal places (whole cents)");
        }
        break;
    }

    // Magnitude, in cents, with every step checked against max_cents. For
    // dollars the integer digits are worth 100 cents each, so they are
    // accumulated against max_cents / 100 and then scaled once.
    const std::string too_large = strprintf("amount exceeds the maximum of %d cents ($%s)",
                                            max_cents, FormatDDAmountDollars(max_cents));
    CAmount cents = 0;
    if (unit == DDAmountUnit::DOLLARS) {
        const CAmount max_whole_dollars = max_cents / 100;
        CAmount whole = 0;
        for (const char c : integer_part) {
            if (!AppendDigitChecked(whole, c - '0', max_whole_dollars)) {
                return Refuse(DDAmountError::TOO_LARGE, too_large);
            }
        }
        CAmount fraction_cents = 0;
        if (fraction_part.size() >= 1) fraction_cents += (fraction_part[0] - '0') * 10;
        if (fraction_part.size() >= 2) fraction_cents += (fraction_part[1] - '0');
        // whole <= max_cents / 100, so whole * 100 <= max_cents and cannot overflow.
        cents = whole * 100;
        if (fraction_cents > max_cents - cents) {
            return Refuse(DDAmountError::TOO_LARGE, too_large);
        }
        cents += fraction_cents;
    } else {
        for (const char c : integer_part) {
            if (!AppendDigitChecked(cents, c - '0', max_cents)) {
                return Refuse(DDAmountError::TOO_LARGE, too_large);
            }
        }
    }

    DDAmountParseResult result;
    result.cents = cents;
    return result;
}

std::string FormatDDAmountDollars(CAmount cents)
{
    // Work on the magnitude as unsigned so the most negative value cannot
    // overflow when negated.
    const bool negative = cents < 0;
    const uint64_t magnitude = negative ? (uint64_t{0} - static_cast<uint64_t>(cents))
                                        : static_cast<uint64_t>(cents);
    return strprintf("%s%d.%02d", negative ? "-" : "", magnitude / 100, magnitude % 100);
}

std::string FormatDDAmountCents(CAmount cents)
{
    return strprintf("%d", cents);
}

} // namespace DigiDollar
