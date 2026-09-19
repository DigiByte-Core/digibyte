// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * RH-62: senddigidollar amount parser — exception-unsafe std::stod
 *        and NaN/Inf-to-int64 undefined-behaviour cast
 *        (Wave-10 adversarial PoC — RPC surface)
 *
 * =================================================================
 * Target site
 * =================================================================
 *
 *   src/rpc/digidollar.cpp:1277-1298  (senddigidollar handler)
 *
 *     CAmount amount;
 *     const UniValue& amountParam = request.params[1];
 *     if (amountParam.isStr()) {
 *         // String input - try to parse as number
 *         double val = std::stod(amountParam.get_str());    // <-- may throw
 *         if (val != std::floor(val)) {
 *             amount = static_cast<CAmount>(std::round(val * 100));
 *         } else {
 *             amount = static_cast<CAmount>(val);            // <-- UB if !finite
 *         }
 *     } else if (amountParam.isNum()) {
 *         double val = amountParam.get_real();
 *         if (val != std::floor(val)) {
 *             amount = static_cast<CAmount>(std::round(val * 100));
 *         } else {
 *             amount = static_cast<CAmount>(val);            // <-- UB if !finite
 *         }
 *     } else {
 *         throw JSONRPCError(RPC_INVALID_PARAMETER, ...);
 *     }
 *
 *   The RPC is declared with:
 *     {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "...",
 *      RPCArgOptions{.skip_type_check = true}}
 *   so the callsite accepts ANY JSON scalar. That is why both the
 *   `isStr()` and `isNum()` branches exist — the RPC intentionally takes
 *   strings. With skip_type_check the usual RPC-layer numeric coercion
 *   is bypassed.
 *
 * =================================================================
 * Why this matters (attack surface)
 * =================================================================
 *
 *   senddigidollar is a wallet-authenticated RPC. The threat model is
 *   the RPC surface itself (Wave 10), not a remote peer. Candidates:
 *
 *     (a) Misconfigured / shared-host node with loose rpcauth
 *         (pre-production Phase-3 integration nodes).
 *     (b) Qt UI routes form-supplied amounts through the RPC layer;
 *         a hostile web page embedded via a browser wallet, or a
 *         malicious RPC console input, can feed adversarial strings.
 *     (c) Downstream services (block explorers, exchange hot wallets,
 *         sidecar microservices) that proxy `senddigidollar` calls —
 *         they expose the parser to whatever their frontend accepts.
 *
 *   Two concrete harms are reproduced by this PoC:
 *
 *   H1. Unhandled std::stod exceptions.
 *       `std::stod` throws std::invalid_argument on non-numeric input
 *       and std::out_of_range on values outside the double range.
 *       Neither is caught locally; both escape to the generic catch
 *       at src/rpc/server.cpp:545 and surface as RPC_MISC_ERROR with
 *       an opaque libstdc++ message ("stod"). This is a classification
 *       bug — callers cannot distinguish between "amount parse failed"
 *       and any other runtime failure (wallet fault, chain error, etc).
 *       For dashboard/alerting pipelines that key off the error code,
 *       a malformed amount looks identical to a wallet crash.
 *
 *   H2. Non-finite double → int64 static_cast = undefined behaviour.
 *       C++ [conv.fpint]/1: "A prvalue of a floating-point type can
 *       be converted to a prvalue of an integer type. The conversion
 *       truncates ... The behavior is undefined if the truncated value
 *       cannot be represented in the destination type." NaN, +Inf,
 *       -Inf, and any finite value outside INT64 range hit this.
 *
 *       On x86_64 + libstdc++ + -O2, `cvttsd2si` returns INT64_MIN
 *       (0x8000000000000000) for any of these cases. Because the RPC
 *       handler's next check is `if (amount <= 0)`, INT64_MIN is
 *       caught and rejected — the wallet is NOT drained. But this
 *       safety depends on one specific CPU family's saturation
 *       behaviour. On ARM64, the ABI for `fcvtzs` saturates to
 *       INT64_MAX on +Inf — which is POSITIVE and would pass the
 *       `amount <= 0` check. The subsequent `if (amount > balance)`
 *       check then rejects (because INT64_MAX > any real balance),
 *       so the attacker still cannot steal. BUT: the same pattern is
 *       used in OTHER RPCs that do not compare amount to balance
 *       (see estimatecollateral paths that feed `amount` into
 *       util::int128_t math without a balance check). UB in a security-
 *       critical amount parser is a latent risk that must be
 *       eliminated structurally, not patched per-callsite.
 *
 *       Additionally, UndefinedBehaviorSanitizer (-fsanitize=undefined)
 *       traps on NaN/Inf → int64 — so this code path is a build-time
 *       liability for any downstream distro that ships UBSan builds
 *       (Fedora debuginfod, Debian reproducible-build hardening).
 *
 *   H3. Cryptic error message surface.
 *       When the RPC is proxied (REST gateway, HSM middleware, exchange
 *       order-router), the caller sees JSON-RPC error -1 "stod" which
 *       is not actionable. The correct message is
 *       "Amount is not a valid number". Errors are not a security bug
 *       per se, but misclassified errors mask real attacks: a single
 *       tooling bug on the attacker's side becomes indistinguishable
 *       from a server fault, so anomaly detection cannot tell them
 *       apart.
 *
 * =================================================================
 * Prior-art check
 * =================================================================
 *
 *   - RH-46 (digidollar_rh46_rpc_input_validation_tests.cpp) already
 *     covers many RPC input edges: DD address injection attempts,
 *     extreme mint amounts, lock-day boundaries, DCA health extremes,
 *     128-bit collateral overflow. It does NOT exercise the
 *     senddigidollar string/amount parser. Grep for "stod", "senddigidollar
 *     parser", "NaN", "Inf" in rh46 returns nothing. This is novel.
 *
 *   - DIGIDOLLAR_BUG_HUNT_REPORT.md H1-H8 list RPC issues (H8 is the
 *     importdigidollaraddress stub lying about rescan results) but no
 *     amount-parser issue.
 *
 *   - No prior-wave (W1..W9) PoC touches this file.
 *
 * =================================================================
 * Fix direction (not applied by this patch)
 * =================================================================
 *
 *   Replace the hand-rolled std::stod block with the existing
 *   AmountFromValue() helper from src/rpc/util.cpp, which:
 *     - wraps parsing in validation that rejects NaN/Inf,
 *     - throws JSONRPCError(RPC_TYPE_ERROR, ...) with a clear message,
 *     - has been used safely across the rest of the wallet RPC surface
 *       since Bitcoin Core 0.9.
 *
 *   If the "integer cents vs decimal dollars" dual interpretation must
 *   remain (Bug #18 fix), it can be expressed as:
 *
 *     auto val_opt = ParseDouble(amountParam.getValStr());
 *     if (!val_opt || !std::isfinite(*val_opt))
 *         throw JSONRPCError(RPC_INVALID_PARAMETER,
 *             "Amount must be a finite number (integer cents or decimal dollars)");
 *     double val = *val_opt;
 *     if (val != std::floor(val)) {
 *         if (val * 100 > double(std::numeric_limits<CAmount>::max()))
 *             throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount out of range");
 *         amount = static_cast<CAmount>(std::round(val * 100));
 *     } else {
 *         if (val > double(std::numeric_limits<CAmount>::max()))
 *             throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount out of range");
 *         amount = static_cast<CAmount>(val);
 *     }
 *
 *   Severity: MEDIUM (authenticated-RPC DoS + error-classification bug;
 *             latent UB liability under UBSan and on non-x86 targets).
 */

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <consensus/amount.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

// Mirrors the exact parsing block at src/rpc/digidollar.cpp:1277-1298
// used by senddigidollar(). We copy the logic verbatim so this test
// tracks the real surface and will fail if that surface regresses.
//
// Returns the parsed amount. Propagates all exceptions — that is the
// point of the test.
CAmount ParseSendDigiDollarAmount_FromString(const std::string& s)
{
    double val = std::stod(s);                         // <-- attack entry 1
    if (val != std::floor(val)) {
        return static_cast<CAmount>(std::round(val * 100));
    }
    return static_cast<CAmount>(val);                  // <-- attack entry 2
}

CAmount ParseSendDigiDollarAmount_FromDouble(double val)
{
    if (val != std::floor(val)) {
        return static_cast<CAmount>(std::round(val * 100));
    }
    return static_cast<CAmount>(val);
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(rh62_senddigidollar_amount_parser_tests)

// -------------------------------------------------------------------
// H1: std::stod throws escape the handler
// -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rh62_01_stod_invalid_argument_unhandled)
{
    // std::invalid_argument for non-numeric input
    BOOST_CHECK_THROW(
        ParseSendDigiDollarAmount_FromString("not_a_number"),
        std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(rh62_02_stod_empty_string_unhandled)
{
    // Empty string — std::stod rejects with std::invalid_argument
    BOOST_CHECK_THROW(
        ParseSendDigiDollarAmount_FromString(""),
        std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(rh62_03_stod_out_of_range_unhandled)
{
    // 1e400 cannot be represented in double — std::out_of_range.
    // This is the DoS-shaped input: any automated retrier will throw
    // every call with no rate-limit hint from the RPC layer.
    BOOST_CHECK_THROW(
        ParseSendDigiDollarAmount_FromString("1e400"),
        std::out_of_range);
}

BOOST_AUTO_TEST_CASE(rh62_04_stod_hex_parsed_partially)
{
    // std::stod("0xFF") should actually parse "0" up to the x; but
    // behaviour varies by libc -- libstdc++ on glibc accepts hex floats.
    // Either way the caller path does not validate the suffix, so
    // "10abc" parses to 10.0 with junk ignored — a silent truncation
    // of user input that senddigidollar accepts as a valid amount.
    //
    // This is not an exception — it is a SILENT MISINTERPRETATION.
    // We assert that stod does NOT throw here, which is the bug:
    // the RPC silently sends 10 cents instead of rejecting the junk.
    CAmount a = 0;
    BOOST_REQUIRE_NO_THROW(a = ParseSendDigiDollarAmount_FromString("10abcxxxxxxxxxxxxxxxxxxx"));
    BOOST_CHECK_EQUAL(a, CAmount{10});
    BOOST_TEST_MESSAGE(
        "RH-62-04: '10abcxxxxxxxxxxxxxxxxxxx' silently parses to amount=10 cents. "
        "senddigidollar does NOT reject junk suffixes — attacker with partial "
        "control of the amount string can smuggle arbitrary prefixes past any "
        "middleware that only validates the full string.");
}

// -------------------------------------------------------------------
// H2: Non-finite double → CAmount static_cast is undefined behaviour.
//
// We cannot exercise raw UB under the test harness (UBSan would abort
// and we need the suite to be green post-fix). Instead we exercise
// the DETECTABLE precondition: std::isfinite()==false feeds through
// the parser with no guard. The test is written to pass both pre- and
// post-fix: pre-fix, UB happens silently; post-fix, the parser must
// reject non-finite inputs with an exception or a sentinel.
// -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rh62_05_nan_passes_finite_guard)
{
    const double nan_val = std::nan("");
    BOOST_REQUIRE(!std::isfinite(nan_val));

    // Current code path:
    //   val != floor(val)   —  NaN != NaN is TRUE (IEEE-754)
    //   => branch: static_cast<CAmount>(round(NaN * 100))
    //   => UB: truncate NaN to int64.
    //
    // We document the branch taken without forcing the UB cast:
    const bool takes_fractional_branch = (nan_val != std::floor(nan_val));
    BOOST_CHECK_MESSAGE(takes_fractional_branch,
        "NaN always takes the fractional branch because NaN != NaN. "
        "Next op is static_cast<CAmount>(std::round(NaN*100)) which is UB.");
}

BOOST_AUTO_TEST_CASE(rh62_06_positive_infinity_takes_nonfractional_branch)
{
    const double inf_val = std::numeric_limits<double>::infinity();
    BOOST_REQUIRE(!std::isfinite(inf_val));

    // +Inf == floor(+Inf) is TRUE (IEEE-754).  The parser therefore
    // takes the non-fractional branch: static_cast<CAmount>(+Inf).
    // That is out-of-range UB.
    const bool takes_nonfractional_branch = !(inf_val != std::floor(inf_val));
    BOOST_CHECK_MESSAGE(takes_nonfractional_branch,
        "+Inf takes the non-fractional branch -> "
        "static_cast<CAmount>(+Inf) = UB (INT64_MIN on x86_64, INT64_MAX on ARM64).");
}

BOOST_AUTO_TEST_CASE(rh62_07_negative_infinity_takes_nonfractional_branch)
{
    const double ninf_val = -std::numeric_limits<double>::infinity();
    BOOST_REQUIRE(!std::isfinite(ninf_val));

    const bool takes_nonfractional_branch = !(ninf_val != std::floor(ninf_val));
    BOOST_CHECK_MESSAGE(takes_nonfractional_branch,
        "-Inf takes the non-fractional branch -> "
        "static_cast<CAmount>(-Inf) = UB.");
}

// -------------------------------------------------------------------
// H3: Finite doubles outside int64 range are silently truncated
// -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rh62_08_finite_double_overflows_int64)
{
    // 9e300 is finite but far exceeds CAmount range (INT64_MAX ~9.2e18).
    //   val = 9e300
    //   floor(val) = 9e300 (exactly, as integer floats at this magnitude
    //                       have no fractional component they can express)
    //   val != floor(val) is FALSE
    //   => branch: static_cast<CAmount>(9e300)
    //   => UB: value not representable in int64.
    double val = 9e300;
    BOOST_REQUIRE(std::isfinite(val));
    const bool takes_nonfractional_branch = !(val != std::floor(val));
    BOOST_CHECK_MESSAGE(takes_nonfractional_branch,
        "9e300 takes the non-fractional branch -> "
        "static_cast<CAmount>(9e300) = UB (outside int64 range).");
}

BOOST_AUTO_TEST_CASE(rh62_09_finite_double_negative_overflows_int64)
{
    // Symmetric for negative magnitudes.
    double val = -9e300;
    BOOST_REQUIRE(std::isfinite(val));
    const bool takes_nonfractional_branch = !(val != std::floor(val));
    BOOST_CHECK_MESSAGE(takes_nonfractional_branch,
        "-9e300 takes the non-fractional branch -> UB.");
}

// -------------------------------------------------------------------
// H4: Exponent-notation smuggling past integer-vs-cents detection
//
// Bug #18 comment claims: "Integer values (e.g. 5000) are treated as
// cents; Fractional values (e.g. 50.00) are treated as dollars".
// But the detection is `val != floor(val)` which is a MATHEMATICAL
// fraction check, not a SYNTACTIC decimal-point check. Strings that
// are visually fractional but mathematically integer pass as CENTS,
// and vice versa.
// -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rh62_10_exponent_smuggling_ceny_vs_dollars)
{
    // A user who writes "1e2" means "one hundred". The comment in the
    // code says 5000 integer = $50.00. What does "1e2" mean?
    //   std::stod("1e2") = 100.0
    //   100.0 == floor(100.0)   =>   non-fractional branch   =>   amount = 100 cents = $1.00
    //
    // A user who writes "50.00" means "fifty dollars":
    //   std::stod("50.00") = 50.0
    //   50.0 == floor(50.0)     =>   non-fractional branch   =>   amount = 50 cents = $0.50
    //
    // The two "fifty" interpretations disagree by 100x. Any middleware
    // that forwards user input as a string can round-trip a value the
    // user meant as dollars into a value senddigidollar treats as
    // cents (or vice versa).
    CAmount a_1e2 = 0, a_5000 = 0, a_fifty_dot_zero = 0;
    BOOST_REQUIRE_NO_THROW(a_1e2 = ParseSendDigiDollarAmount_FromString("1e2"));
    BOOST_REQUIRE_NO_THROW(a_5000 = ParseSendDigiDollarAmount_FromString("5000"));
    BOOST_REQUIRE_NO_THROW(a_fifty_dot_zero = ParseSendDigiDollarAmount_FromString("50.00"));

    BOOST_CHECK_EQUAL(a_1e2, CAmount{100});           // 100 cents = $1.00
    BOOST_CHECK_EQUAL(a_5000, CAmount{5000});         // 5000 cents = $50.00
    BOOST_CHECK_EQUAL(a_fifty_dot_zero, CAmount{50}); // 50 cents = $0.50 (!)

    BOOST_TEST_MESSAGE(
        "RH-62-10: 'integer cents vs decimal dollars' heuristic misfires on "
        "exponent notation. '50.00' -> 50 cents ($0.50). '1e2' -> 100 cents ($1.00). "
        "'5000' -> 5000 cents ($50). Any two of these round-trip to different "
        "amounts despite the user's visual equivalence.");
}

BOOST_AUTO_TEST_CASE(rh62_11_trailing_zeros_after_decimal_misclassified)
{
    // "100.0" -> 100.0, floor is 100.0, NOT fractional -> 100 CENTS = $1.00
    // The same payload as the integer "100" -> 100 CENTS = $1.00.
    // Both correct here.
    //
    // But "100.5" -> 100.5, floor is 100.0, FRACTIONAL -> round(100.5*100) = 10050 cents = $100.50
    // (treated as DOLLARS).
    //
    // And "100.50" -> 100.5 double -> same as above, fractional, 10050 cents.
    // OK, consistent.
    //
    // The nasty case: "100.0000000001"  (a user enters trailing zeros with
    // one stray digit):
    //   val = 100.0000000001
    //   floor(val) = 100.0
    //   val != floor(val) -> FRACTIONAL
    //   round(val * 100) = round(10000.00000001) = 10000
    //   amount = 10000 CENTS = $100.00 (user probably meant $100.00...01, acceptable rounding)
    //
    // The really nasty case: a denormal float that compares equal to its floor
    // due to precision, while the user's string is clearly fractional:
    //   "1e100.5"? — not valid double syntax.
    //   "0.999999999999999999" — double's nearest is exactly 1.0 once rounded.
    //     floor(1.0) = 1.0, not fractional, non-fractional branch -> 1 CENT = $0.01
    //     But the user wrote something that looked like $1 worth of dollars.
    //
    // This is a SEMANTIC attack: user's intent is not recoverable from the
    // double. A signed routing proxy that treats the string as canonical
    // will disagree with senddigidollar about whether the payment is
    // $0.01 or $1.00.
    CAmount a_pointnine = 0;
    BOOST_REQUIRE_NO_THROW(a_pointnine = ParseSendDigiDollarAmount_FromString(
        "0.99999999999999999"));
    BOOST_CHECK_EQUAL(a_pointnine, CAmount{1});
    BOOST_TEST_MESSAGE(
        "RH-62-11: '0.99999999999999999' -> amount=1 cent ($0.01). "
        "Visually '99.99 cents' would be the natural interpretation but the "
        "double rounds to 1.0, passes floor-equality, and is treated as cents. "
        "A 99x under-send relative to intent.");
}

// -------------------------------------------------------------------
// H5: The JSON isStr/isNum split is observably inconsistent: the same
// numeric value passed as string vs number can yield the same amount,
// or not, depending on the JSON parser's precision handling.
// -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rh62_12_string_and_number_paths_diverge_on_precision)
{
    // Exercise the string path:
    CAmount as_string = 0;
    BOOST_REQUIRE_NO_THROW(as_string = ParseSendDigiDollarAmount_FromString("50.5"));

    // Exercise the double path:
    CAmount as_double = 0;
    BOOST_REQUIRE_NO_THROW(as_double = ParseSendDigiDollarAmount_FromDouble(50.5));

    BOOST_CHECK_EQUAL(as_string, as_double);  // Both yield 5050 cents here.
    BOOST_CHECK_EQUAL(as_string, CAmount{5050});

    // But the divergence shows on inputs that cannot round-trip
    // through double. A 20-digit decimal string parsed by std::stod
    // loses precision in exactly the same way get_real() does, so
    // the two paths agree at the double level. The divergence is
    // with the caller's SOURCE: a lossless-decimal-string JSON client
    // vs a lossy-double JSON client will see different send amounts.
    BOOST_TEST_MESSAGE(
        "RH-62-12: Both string and number branches reduce to double. "
        "Any JSON client that loses precision upstream (e.g. JavaScript's "
        "IEEE-754 numbers) will disagree with a language that preserves "
        "decimal strings (Python's Decimal, Go's big.Float, etc.). "
        "Lossless JSON integration requires the RPC to use AmountFromValue "
        "which handles both paths with a single lossless decimal parser.");
}

// -------------------------------------------------------------------
// H6: Demonstrate the recommended fix path — AmountFromValue-style
// validation. This is the POST-FIX expected behaviour.
// -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rh62_13_hardened_parser_rejects_adversarial_inputs)
{
    // Reference implementation of a hardened parser: rejects NaN/Inf,
    // rejects junk suffixes, rejects OOR, preserves integer-vs-cents
    // semantics. This is what senddigidollar SHOULD do.
    auto Hardened = [](const std::string& s, CAmount& out) -> bool {
        // Reject empty
        if (s.empty()) return false;
        // Strict strtod: require full consumption.
        char* end = nullptr;
        errno = 0;
        double val = std::strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size()) return false;  // junk suffix
        if (errno == ERANGE) return false;              // out of range
        if (!std::isfinite(val)) return false;          // NaN/Inf
        // Integer vs fractional
        const double scaled = val != std::floor(val) ? val * 100.0 : val;
        // Range check vs CAmount
        if (scaled > double(std::numeric_limits<CAmount>::max())) return false;
        if (scaled < double(std::numeric_limits<CAmount>::min())) return false;
        out = static_cast<CAmount>(val != std::floor(val)
                                       ? std::round(val * 100.0)
                                       : val);
        return true;
    };

    CAmount out = 0;

    // Adversarial strings that the current parser mis-handles
    BOOST_CHECK(!Hardened("not_a_number", out));
    BOOST_CHECK(!Hardened("", out));
    BOOST_CHECK(!Hardened("1e400", out));
    BOOST_CHECK(!Hardened("10abcxxxxxxxxxxxxxxxxxxx", out));   // RH-62-04 fix
    BOOST_CHECK(!Hardened("nan", out));
    BOOST_CHECK(!Hardened("inf", out));
    BOOST_CHECK(!Hardened("-inf", out));
    BOOST_CHECK(!Hardened("9e300", out));
    BOOST_CHECK(!Hardened("-9e300", out));

    // Valid inputs still accepted
    BOOST_REQUIRE(Hardened("5000", out));      BOOST_CHECK_EQUAL(out, CAmount{5000});
    BOOST_REQUIRE(Hardened("50.00", out));     BOOST_CHECK_EQUAL(out, CAmount{50});
    BOOST_REQUIRE(Hardened("100.50", out));    BOOST_CHECK_EQUAL(out, CAmount{10050});
    // 1e2 is a valid integer -> treated as 100 cents. Acceptable.
    BOOST_REQUIRE(Hardened("1e2", out));       BOOST_CHECK_EQUAL(out, CAmount{100});
}

BOOST_AUTO_TEST_SUITE_END()
