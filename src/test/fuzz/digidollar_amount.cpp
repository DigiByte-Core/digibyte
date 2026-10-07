// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Fuzz harness for the DigiDollar RPC amount parser (digidollar/amount.h).
//
// Invariants asserted on every input:
//   1. ParseDDAmount never throws for any text, unit, or limit; the only way
//      it reports a problem is the documented DDAmountParseResult error.
//   2. A successful result is within [0, max_cents].
//   3. A dollars result is exactly the decimal text times 100, checked
//      against the node's independent fixed-point parser.
//   4. Round trip: formatting a result and parsing it again gives the same
//      cents, for both the dollars and the cents formatter.
//   5. Text without a decimal point parses to the same value with no unit
//      and with an explicit cents unit; the dollars reading of that text is
//      exactly 100 times the cents reading whenever it fits.
//   6. A decimal point is never accepted without a unit, and never accepted
//      under the cents unit.

#include <digidollar/amount.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <util/strencodings.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

using DigiDollar::DDAmountError;
using DigiDollar::DDAmountParseResult;
using DigiDollar::DDAmountUnit;
using DigiDollar::ParseDDAmount;

namespace {

DDAmountParseResult ParseNoThrow(const std::string& text, DDAmountUnit unit, CAmount max_cents)
{
    try {
        return ParseDDAmount(text, unit, max_cents);
    } catch (...) {
        std::abort(); // ParseDDAmount must never throw
    }
}

bool IsPlainDecimalText(const std::string& text, bool& has_point, size_t& fraction_digits)
{
    has_point = false;
    fraction_digits = 0;
    if (text.empty()) return false;
    size_t pos = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos == 0) return false;
    if (pos > 1 && text[0] == '0') return false;
    if (pos == text.size()) return true;
    if (text[pos] != '.') return false;
    has_point = true;
    ++pos;
    const size_t fraction_start = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    fraction_digits = pos - fraction_start;
    return fraction_digits > 0 && pos == text.size();
}

} // namespace

FUZZ_TARGET(digidollar_amount)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    // Mix free-form text with well-formed numbers so the accept paths are
    // reached often, not only the refusal paths.
    std::string text;
    if (fdp.ConsumeBool()) {
        text = fdp.ConsumeRandomLengthString(64);
    } else {
        text = std::to_string(fdp.ConsumeIntegral<uint64_t>());
        if (fdp.ConsumeBool()) {
            text += '.';
            const int digits = fdp.ConsumeIntegralInRange<int>(0, 4);
            for (int i = 0; i < digits; ++i) text += static_cast<char>('0' + fdp.ConsumeIntegralInRange<int>(0, 9));
        }
    }

    const CAmount max_cents = fdp.ConsumeBool()
                                  ? DigiDollar::MAX_DD_RPC_AMOUNT_CENTS
                                  : fdp.ConsumeIntegralInRange<CAmount>(-1, std::numeric_limits<CAmount>::max());
    const CAmount effective_max = max_cents < 0 ? 0 : max_cents;

    const DDAmountParseResult unspecified = ParseNoThrow(text, DDAmountUnit::UNSPECIFIED, max_cents);
    const DDAmountParseResult cents = ParseNoThrow(text, DDAmountUnit::CENTS, max_cents);
    const DDAmountParseResult dollars = ParseNoThrow(text, DDAmountUnit::DOLLARS, max_cents);

    for (const DDAmountParseResult* result : {&unspecified, &cents, &dollars}) {
        if (result->ok()) {
            assert(result->error == DDAmountError::NONE);
            assert(result->message.empty());
            assert(result->cents >= 0);
            assert(result->cents <= effective_max);
        } else {
            assert(!result->message.empty());
            assert(result->cents == 0);
        }
    }

    bool has_point = false;
    size_t fraction_digits = 0;
    const bool plain = IsPlainDecimalText(text, has_point, fraction_digits);

    if (!plain) {
        // Nothing that is not a plain decimal number may ever be accepted.
        assert(!unspecified.ok() && !cents.ok() && !dollars.ok());
        if (!text.empty() && text[0] == '-') {
            assert(unspecified.error == DDAmountError::NEGATIVE);
            assert(cents.error == DDAmountError::NEGATIVE);
            assert(dollars.error == DDAmountError::NEGATIVE);
        } else {
            assert(unspecified.error == DDAmountError::NOT_A_NUMBER);
            assert(cents.error == DDAmountError::NOT_A_NUMBER);
            assert(dollars.error == DDAmountError::NOT_A_NUMBER);
        }
        return;
    }

    if (has_point) {
        // A decimal point is never guessed and never integral cents.
        assert(!unspecified.ok() && unspecified.error == DDAmountError::AMBIGUOUS_UNIT);
        assert(unspecified.message == DigiDollar::DD_AMOUNT_AMBIGUOUS_MESSAGE);
        assert(!cents.ok() && cents.error == DDAmountError::CENTS_NOT_INTEGRAL);
        if (fraction_digits > 2) {
            assert(!dollars.ok() && dollars.error == DDAmountError::TOO_MANY_DECIMALS);
            return;
        }
    } else {
        // No point: the unspecified and cents readings must agree exactly.
        assert(unspecified.ok() == cents.ok());
        assert(unspecified.error == cents.error);
        assert(unspecified.cents == cents.cents);
        if (!cents.ok()) assert(cents.error == DDAmountError::TOO_LARGE);
        if (dollars.ok()) {
            // Dollars is 100x cents; if dollars fits, cents certainly fits.
            assert(cents.ok());
            assert(dollars.cents == cents.cents * 100);
        } else {
            assert(dollars.error == DDAmountError::TOO_LARGE);
        }
    }

    // The dollars value must equal the exact decimal times 100. ParseFixedPoint
    // is an independent implementation with a fixed range of (-10^18, 10^18);
    // outside that range it refuses, so only compare where it can answer.
    constexpr CAmount FIXED_POINT_LIMIT{1'000'000'000'000'000'000LL}; // ParseFixedPoint refuses >= 10^18
    int64_t fixed_point = 0;
    const bool fixed_ok = ParseFixedPoint(text, 2, &fixed_point);
    if (dollars.ok()) {
        if (dollars.cents < FIXED_POINT_LIMIT) {
            assert(fixed_ok);
            assert(fixed_point == dollars.cents);
        }
    } else {
        assert(dollars.error == DDAmountError::TOO_LARGE);
        // The refusal is genuine: the exact value really is above the limit.
        assert(!fixed_ok || fixed_point > effective_max);
    }
    if (cents.ok() && cents.cents < FIXED_POINT_LIMIT) {
        int64_t fixed_cents = 0;
        assert(ParseFixedPoint(text, 0, &fixed_cents));
        assert(fixed_cents == cents.cents);
    }

    // Round trips through both formatters.
    if (dollars.ok()) {
        const std::string formatted = DigiDollar::FormatDDAmountDollars(dollars.cents);
        const DDAmountParseResult again = ParseNoThrow(formatted, DDAmountUnit::DOLLARS, max_cents);
        assert(again.ok());
        assert(again.cents == dollars.cents);
        assert(formatted.find('.') == formatted.size() - 3);
    }
    if (cents.ok()) {
        const std::string formatted = DigiDollar::FormatDDAmountCents(cents.cents);
        const DDAmountParseResult again = ParseNoThrow(formatted, DDAmountUnit::CENTS, max_cents);
        assert(again.ok());
        assert(again.cents == cents.cents);
        const DDAmountParseResult again_unspecified = ParseNoThrow(formatted, DDAmountUnit::UNSPECIFIED, max_cents);
        assert(again_unspecified.ok());
        assert(again_unspecified.cents == cents.cents);
    }
}
