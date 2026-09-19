// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_DIGIDOLLAR_AMOUNT_H
#define DIGIBYTE_DIGIDOLLAR_AMOUNT_H

#include <consensus/amount.h>

#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace DigiDollar {

// DigiDollar amounts are integer cents everywhere inside the node. RPC
// callers, however, type amounts as text, and the same text can mean two very
// different things: "10000" is 10,000 cents ($100.00) while "10000.00" written
// by habit means $10,000.00 if it is read as dollars. This unit is the single
// place that turns caller text into cents. It never goes through floating
// point, it never guesses the unit from the shape of the number, and every
// refusal carries a plain-English reason the caller can act on.

/** How the caller said an amount should be read. */
enum class DDAmountUnit {
    UNSPECIFIED, // No unit given: only a plain integer (cents) is accepted.
    CENTS,       // An integer number of cents: "5000" is $50.00.
    DOLLARS,     // Dollars with at most two decimals: "50" or "50.00" is $50.00.
};

/** Why an amount text was refused. */
enum class DDAmountError {
    NONE,
    NOT_A_NUMBER,       // Empty, sign, whitespace, exponent, letters, malformed decimal.
    AMBIGUOUS_UNIT,     // A decimal point with no unit: dollars, or a typo for cents?
    CENTS_NOT_INTEGRAL, // amount_unit=cents together with a decimal point.
    TOO_MANY_DECIMALS,  // amount_unit=dollars with more than two decimal places.
    NEGATIVE,           // A leading minus sign.
    TOO_LARGE,          // Above the maximum the caller allows.
};

struct DDAmountParseResult {
    DDAmountError error{DDAmountError::NONE};
    CAmount cents{0};    // Meaningful only when error == NONE.
    std::string message; // Plain-English reason when error != NONE.

    bool ok() const { return error == DDAmountError::NONE; }
};

/**
 * The largest DigiDollar amount a single senddigidollar request, a single
 * sendmanydigidollar recipient, or a redeemdigidollar principal may name:
 * 10,000,000 cents = $100,000.00. It equals the per-output transfer
 * consensus limit and the mainnet maximum mint, so no legitimate send or full
 * vault redemption is ever blocked by it. It exists to stop a typo from
 * moving far more than intended. It bounds only what the caller types: the
 * emergency-redemption burn a wallet computes from a vault is not a typed
 * amount and is never capped by it.
 */
inline constexpr CAmount MAX_DD_RPC_AMOUNT_CENTS{10'000'000};

/** Pass as max_cents to accept any amount that fits in a CAmount. */
inline constexpr CAmount DD_AMOUNT_NO_CAP{std::numeric_limits<CAmount>::max()};

/** The exact text of the refusal for a decimal amount given without a unit. */
inline constexpr const char* DD_AMOUNT_AMBIGUOUS_MESSAGE{
    "ambiguous amount: pass amount_unit=cents or amount_unit=dollars "
    "(an amount with a decimal point is not accepted without a unit)"};

/** Parse an amount_unit argument: exactly "cents" or "dollars". */
std::optional<DDAmountUnit> ParseDDAmountUnit(std::string_view name);

/** "unspecified", "cents" or "dollars". */
std::string DDAmountUnitName(DDAmountUnit unit);

/**
 * Turn caller text into cents under the given unit contract.
 *
 * Accepted syntax is a plain decimal number: ASCII digits, an optional single
 * '.' followed by at least one digit, no sign, no whitespace, no exponent, no
 * thousands separators, and no leading zeros other than a lone "0" before the
 * point. Anything else is NOT_A_NUMBER; a leading '-' is NEGATIVE.
 *
 *  - UNSPECIFIED: an integer is cents (the behaviour every existing caller
 *    relies on); a decimal point is AMBIGUOUS_UNIT and is never guessed.
 *  - CENTS: an integer is cents; a decimal point is CENTS_NOT_INTEGRAL.
 *  - DOLLARS: at most two decimals; "12.3" is 1230 cents, "12.345" is
 *    TOO_MANY_DECIMALS.
 *
 * The value is accumulated with checked integer arithmetic and refused as
 * TOO_LARGE as soon as it would exceed max_cents, so no intermediate can
 * overflow whatever the text length. A result is always in [0, max_cents].
 */
DDAmountParseResult ParseDDAmount(std::string_view text, DDAmountUnit unit, CAmount max_cents);

/** 5000 -> "50.00", 5 -> "0.05". Parsing the result as dollars gives cents back. */
std::string FormatDDAmountDollars(CAmount cents);

/** 5000 -> "5000". Parsing the result as cents (or with no unit) gives cents back. */
std::string FormatDDAmountCents(CAmount cents);

} // namespace DigiDollar

#endif // DIGIBYTE_DIGIDOLLAR_AMOUNT_H
