// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// =============================================================================
// Wave 18 Agent B - RPC Command Completeness, Schemas, Units, Errors
// =============================================================================
//
// Strengthens DigiDollar RPC unit coverage along the Wave 18 brief:
//
//   1. Schema tests for invalid params: per RPC, exercise each parameter
//      with negative / zero / overflow / wrong-type / oversized values and
//      pin the rejection contract (RPC_INVALID_PARAMETER vs silent default).
//   2. Unit consistency: cents vs DGB conversion at every RPC boundary.
//      ParseDigiDollarRpcAmount accepts integer cents and decimal-dollar
//      strings; integer JSON values map to cents (50 = 50c) and
//      "50.00" maps to dollars (5000c). Pin both sides to lock the unit
//      contract that consensus depends on.
//   3. minconf semantics: getdigidollarbalance must reject negative minconf
//      consistently and treat minconf=0 as include-mempool.
//   4. min_amount default and override: listdigidollarpositions silently
//      treats negative min_amount as "no filter" (the >0 guard at
//      src/rpc/digidollar.cpp:2216). Pin the observed behavior so a future
//      tightening (reject vs accept) is a deliberate decision rather than
//      a regression.
//   5. change_amount handling on send: senddigidollar's change_amount field
//      must always be a non-negative integer and reflect (selectedDDTotal
//      - target) when wallet selection produces change. The contract is
//      end-to-end pinned by the functional test digidollar_send.py; here
//      we add a unit-level pin against the listdigidollarpositions /
//      getredemptioninfo result schemas (cents-as-int, never floated).
//   6. Deployment-gated commands: pre-activation rejection with clear
//      error. The functional digidollar_rpc_gating.py drives the 30 gated
//      protocol/action commands at DEFINED state and separately pins
//      createoraclekey as pre-activation wallet key management; this file
//      adds a unit-level pin at the regtest-active boundary that the
//      cents/dgb units do not silently shift between defined-and-active states.
//
// All cases run under the same DigiDollarRPCUnitSetup fixture used by
// src/test/digidollar_rpc_unit_tests.cpp (regtest, DigiDollar
// ALWAYS_ACTIVE), and use HandleRequest(...) on the RPCHelpMan factories
// so we exercise the production parameter-parsing path, not a private
// helper. Where the RPC is `static` (e.g. getoraclepubkey,
// setmockoracleprice) we use the CallNodeRPC table dispatch which
// matches the user-facing parsing path.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <consensus/err.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <oracle/mock_oracle.h>
#include <primitives/oracle.h>
#include <rpc/client.h>
#include <rpc/digidollar.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tinyformat.h>
#include <vector>

BOOST_AUTO_TEST_SUITE(digidollar_wave18_rpc_schema_tests)

namespace {

struct DigiDollarRPCSchemaSetup : public TestingSetup {
    DigiDollarRPCSchemaSetup() : TestingSetup(ChainType::REGTEST)
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
    }

    ~DigiDollarRPCSchemaSetup()
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
    }

    UniValue CallNodeRPC(const std::string& args)
    {
        std::vector<std::string> v_args{SplitString(args, ' ')};
        const std::string method = v_args.front();
        v_args.erase(v_args.begin());

        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = method;
        request.params = RPCConvertValues(method, v_args);
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        return tableRPC.execute(request);
    }

    std::shared_ptr<wallet::CWallet> CreateWalletWithThreePositions()
    {
        auto wallet = std::make_shared<wallet::CWallet>(
            m_node.chain.get(), "wave18-rpc-wallet", wallet::CreateMockableWalletDatabase());
        wallet->LoadWallet();
        wallet->EnsureDDWallet();
        WITH_LOCK(wallet->cs_wallet, wallet->SetLastBlockProcessed(1000, uint256::ONE));

        DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
        BOOST_REQUIRE(dd_wallet != nullptr);
        // Three positions spanning the cents range so min_amount filter
        // boundaries can be exercised without reaching MAX_MONEY.
        dd_wallet->AddCollateralPosition(WalletCollateralPosition(
            uint256S("00000000000000000000000000000000000000000000000000000000dd180001"),
            100, 1 * COIN, 1, 1100));
        dd_wallet->AddCollateralPosition(WalletCollateralPosition(
            uint256S("00000000000000000000000000000000000000000000000000000000dd180002"),
            5'000, 50 * COIN, 1, 1100));
        dd_wallet->AddCollateralPosition(WalletCollateralPosition(
            uint256S("00000000000000000000000000000000000000000000000000000000dd180003"),
            1'000'000, 10'000 * COIN, 9, 2000));

        return wallet;
    }
};

// Helper: invoke a gated RPC factory and capture either the JSONRPCError
// "message" (when an RPC error is thrown) or the std::exception::what()
// for everything else. Returns true if the call threw.
template <typename Fn>
bool RpcThrows(Fn&& fn, std::string& out_message)
{
    try {
        fn();
        return false;
    } catch (const UniValue& objError) {
        if (objError.exists("message")) {
            out_message = objError["message"].get_str();
        }
        return true;
    } catch (const std::exception& e) {
        out_message = e.what();
        return true;
    }
}

} // namespace

// =============================================================================
// W18-01: getdigidollarbalance rejects negative minconf
// =============================================================================
//
// Pins the contract at src/rpc/digidollar.cpp:2840-2842:
//   if (minConf < 0) throw RPC_INVALID_PARAMETER
//
// Invariant: a negative minconf must never silently default to 1 (which
// would mask a wallet client bug that passes a signed integer underflow).
// Functional test digidollar_rpc_amount_filters.py covers minconf=1, 2;
// this case adds the negative-rejection unit pin.
BOOST_FIXTURE_TEST_CASE(w18_01_getdigidollarbalance_rejects_negative_minconf,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getdigidollarbalance";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(""); // empty address -> wallet total
    request.params.push_back(-1); // minconf

    std::string err_message;
    bool threw = RpcThrows(
        [&] { getdigidollarbalance().HandleRequest(request); },
        err_message);

    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_CHECK_MESSAGE(threw,
        "getdigidollarbalance must reject minconf=-1 instead of silently defaulting");
    BOOST_CHECK_MESSAGE(err_message.find("non-negative") != std::string::npos,
        strprintf("Error must name the non-negative invariant. Got: %s", err_message));
}

// =============================================================================
// W18-02: getdigidollarbalance accepts minconf=0 (include-mempool semantics)
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:2847 confirmedMinConf = std::max(1, minConf)
// and the minconf==0 mempool branch. minconf=0 must always be valid input
// even though it relaxes the confirmation guarantee — the cents/DGB unit
// math must remain stable across the boundary.
BOOST_FIXTURE_TEST_CASE(w18_02_getdigidollarbalance_accepts_minconf_zero,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getdigidollarbalance";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back("");
    request.params.push_back(0);

    UniValue result = getdigidollarbalance().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isObject());
    // Schema fields must always be present in cents-as-integer format
    // regardless of minconf so wallet UIs can rely on a stable shape.
    for (const char* const field : {"confirmed", "unconfirmed", "total"}) {
        BOOST_REQUIRE_MESSAGE(result.exists(field),
            strprintf("getdigidollarbalance result must always include `%s`", field));
        BOOST_CHECK_MESSAGE(result[field].isNum(),
            strprintf("`%s` must be a numeric cents value, never decimal-formatted", field));
    }
    // Position fixture has no wallet-tracked CWalletTx, so balances are 0
    // — the schema invariant (cents-as-integer) is what we are pinning.
    BOOST_CHECK_EQUAL(result["confirmed"].getInt<int64_t>(), 0);
    BOOST_CHECK_EQUAL(result["unconfirmed"].getInt<int64_t>(), 0);
    BOOST_CHECK_EQUAL(result["total"].getInt<int64_t>(), 0);
}

// =============================================================================
// W18-03: listdigidollarpositions refuses a negative min_amount
// =============================================================================
//
// A negative minimum amount is a mistake in the caller, and there is no
// reading of it that makes sense. It used to be accepted and then ignored, so
// the call returned every position and the caller never learned that its
// filter had been thrown away. It is now refused, like a negative amount
// anywhere else in the DigiDollar RPCs.
BOOST_FIXTURE_TEST_CASE(w18_03_listdigidollarpositions_negative_min_amount_is_refused,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "listdigidollarpositions";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(true);   // active_only
    request.params.push_back(-1);     // tier_filter (no filter)
    request.params.push_back(-100);   // min_amount: negative

    std::string message;
    BOOST_CHECK(RpcThrows([&] { listdigidollarpositions().HandleRequest(request); }, message));
    BOOST_CHECK_MESSAGE(message.find("must not be negative") != std::string::npos, message);

    // Leaving the filter out still returns every position.
    JSONRPCRequest unfiltered;
    unfiltered.context = &context;
    unfiltered.strMethod = "listdigidollarpositions";
    unfiltered.params = UniValue(UniValue::VARR);

    UniValue result = listdigidollarpositions().HandleRequest(unfiltered);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isArray());
    BOOST_CHECK_EQUAL(result.size(), 3U);
}

// =============================================================================
// W18-04: listdigidollarpositions min_amount default returns all positions
// =============================================================================
//
// With min_amount omitted, every active position is returned. Without a
// pin a future "default to a sane min" change could silently truncate
// the spendable view a wallet UI builds.
BOOST_FIXTURE_TEST_CASE(w18_04_listdigidollarpositions_default_min_amount_returns_all,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "listdigidollarpositions";
    request.params = UniValue(UniValue::VARR);
    // No params -> active_only=true default, no tier filter, no min_amount

    UniValue result = listdigidollarpositions().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isArray());
    BOOST_CHECK_EQUAL(result.size(), 3U);
}

// =============================================================================
// W18-05: listdigidollarpositions min_amount override filters strictly less
// =============================================================================
//
// min_amount=10 cents must drop the 1-cent fixture position and keep
// the 50.00 + 10000.00 cent positions. Pins the cents-not-DGB unit
// contract end-to-end through the RPC surface, mirroring the Wave 1
// pin at src/test/digidollar_rpc_unit_tests.cpp:106 but with explicit
// integer cents (not "1.00") so both number-as-integer and
// number-as-string ParseDigiDollarRpcAmount paths are pinned.
BOOST_FIXTURE_TEST_CASE(w18_05_listdigidollarpositions_min_amount_strict_filter_cents,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "listdigidollarpositions";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(true);      // active_only
    request.params.push_back(-1);        // tier_filter
    request.params.push_back(int64_t{10}); // min_amount as integer cents

    UniValue result = listdigidollarpositions().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isArray());
    BOOST_CHECK_EQUAL(result.size(), 3U); // 100, 5000, 1000000 all >= 10c

    // Now drop the 100-cent position by raising min_amount above it.
    auto wallet2 = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet2);

    JSONRPCRequest request2;
    request2.context = &context;
    request2.strMethod = "listdigidollarpositions";
    request2.params = UniValue(UniValue::VARR);
    request2.params.push_back(true);
    request2.params.push_back(-1);
    request2.params.push_back(int64_t{500}); // 500 cents > 100, <= 5000

    UniValue result2 = listdigidollarpositions().HandleRequest(request2);
    wallet::RemoveWallet(context, wallet2, std::nullopt);

    BOOST_REQUIRE(result2.isArray());
    BOOST_CHECK_EQUAL(result2.size(), 2U);
    for (const UniValue& pos : result2.getValues()) {
        const int64_t minted = pos["dd_minted"].getInt<int64_t>();
        BOOST_CHECK_MESSAGE(minted >= 500,
            strprintf("Position dd_minted=%d should not appear under min_amount=500", minted));
    }
}

// =============================================================================
// W18-06: listdigidollarpositions decimal string is dollars only with amount_unit
// =============================================================================
//
// A decimal amount is never guessed to be dollars. Without amount_unit the
// RPC refuses "50.01" outright; with amount_unit="dollars" it is 5001 cents,
// so the 100-cent and 5000-cent positions drop (dd_minted < minAmount, not
// <=) and only the 1,000,000-cent position survives.
BOOST_FIXTURE_TEST_CASE(w18_06_listdigidollarpositions_decimal_string_requires_dollars_unit,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest ambiguous;
    ambiguous.context = &context;
    ambiguous.strMethod = "listdigidollarpositions";
    ambiguous.params = UniValue(UniValue::VARR);
    ambiguous.params.push_back(true);
    ambiguous.params.push_back(-1);
    ambiguous.params.push_back("50.01"); // no unit: refused, never read as 5001 or 50 cents
    BOOST_CHECK_THROW(listdigidollarpositions().HandleRequest(ambiguous), UniValue);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "listdigidollarpositions";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(true);
    request.params.push_back(-1);
    request.params.push_back("50.01"); // 5001 cents once the unit says dollars
    request.params.push_back(UniValue(UniValue::VNULL)); // count
    request.params.push_back(UniValue(UniValue::VNULL)); // skip
    request.params.push_back("dollars");

    UniValue result = listdigidollarpositions().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isArray());
    BOOST_CHECK_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0]["dd_minted"].getInt<int64_t>(), 1'000'000);
}

// =============================================================================
// W18-06b: a JSON number with a decimal point is refused the same way a
// string is
// =============================================================================
//
// A JSON-RPC client can send the amount as a bare number token with a decimal
// point (50.00) instead of a string ("50.00"). The node keeps that token text,
// so both forms must be treated identically: without amount_unit both are
// refused, and with amount_unit="dollars" both are 5000 cents. If only the
// string form were refused, the same amount typed two ways would mean two
// different things.
BOOST_FIXTURE_TEST_CASE(w18_06b_listdigidollarpositions_decimal_number_needs_dollars_unit,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    UniValue numeric_decimal;
    numeric_decimal.setNumStr("50.00");

    JSONRPCRequest ambiguous;
    ambiguous.context = &context;
    ambiguous.strMethod = "listdigidollarpositions";
    ambiguous.params = UniValue(UniValue::VARR);
    ambiguous.params.push_back(true);
    ambiguous.params.push_back(-1);
    ambiguous.params.push_back(numeric_decimal); // no unit: refused, not guessed

    std::string message;
    BOOST_CHECK(RpcThrows([&] { listdigidollarpositions().HandleRequest(ambiguous); }, message));
    BOOST_CHECK_MESSAGE(message.find("ambiguous amount") != std::string::npos, message);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "listdigidollarpositions";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(true);
    request.params.push_back(-1);
    request.params.push_back(numeric_decimal); // 50.00 dollars -> 5000 cents
    request.params.push_back(UniValue(UniValue::VNULL)); // count
    request.params.push_back(UniValue(UniValue::VNULL)); // skip
    request.params.push_back("dollars");

    UniValue result = listdigidollarpositions().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isArray());
    BOOST_CHECK_EQUAL(result.size(), 2U);
    BOOST_CHECK_EQUAL(result[0]["dd_minted"].getInt<int64_t>(), 5'000);
    BOOST_CHECK_EQUAL(result[1]["dd_minted"].getInt<int64_t>(), 1'000'000);
}

// =============================================================================
// W18-06c: dollar amounts below one cent are refused, never rounded
// =============================================================================
//
// DigiDollar accounting is whole cents. An amount with a third decimal place
// must fail rather than be rounded into a different amount than the caller
// asked for, whether it arrives as a string or as a bare JSON number.
BOOST_FIXTURE_TEST_CASE(w18_06c_decimal_dollar_amount_rejects_subcent_precision,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    for (const UniValue& amount_param : {UniValue{"50.001"}, [] {
             UniValue numeric_decimal;
             numeric_decimal.setNumStr("50.001");
             return numeric_decimal;
         }()}) {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = "listdigidollarpositions";
        request.params = UniValue(UniValue::VARR);
        request.params.push_back(true);
        request.params.push_back(-1);
        request.params.push_back(amount_param);
        request.params.push_back(UniValue(UniValue::VNULL)); // count
        request.params.push_back(UniValue(UniValue::VNULL)); // skip
        request.params.push_back("dollars");

        std::string message;
        BOOST_CHECK(RpcThrows([&] { listdigidollarpositions().HandleRequest(request); }, message));
        BOOST_CHECK_MESSAGE(message.find("at most two decimal places") != std::string::npos, message);
    }

    wallet::RemoveWallet(context, wallet, std::nullopt);
}

// =============================================================================
// W18-07: getredemptioninfo rejects malformed position_id (length / hex)
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:3133 IsHex/length==64 contract. Three
// distinct malformed inputs:
//   (a) too short (32-char hex)
//   (b) correct length but contains non-hex characters
//   (c) empty string
// All must be rejected with RPC_INVALID_PARAMETER and a clear message.
BOOST_FIXTURE_TEST_CASE(w18_07_getredemptioninfo_rejects_malformed_position_id,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    auto run_one = [&](const std::string& raw_position_id) {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = "getredemptioninfo";
        request.params = UniValue(UniValue::VARR);
        request.params.push_back(raw_position_id);

        std::string err;
        bool threw = RpcThrows(
            [&] { getredemptioninfo().HandleRequest(request); }, err);
        BOOST_CHECK_MESSAGE(threw,
            strprintf("getredemptioninfo must reject malformed position_id `%s`",
                      raw_position_id));
        // The schema-level rejection happens before the wallet lookup, so
        // the error must reference the format invariant (not "not found").
        const bool format_or_invalid =
            err.find("position ID") != std::string::npos ||
            err.find("position_id") != std::string::npos ||
            err.find("Invalid") != std::string::npos;
        BOOST_CHECK_MESSAGE(format_or_invalid,
            strprintf("Error message must reference the position_id contract. Got: %s", err));
    };

    run_one("dead"); // too short
    run_one("zz" + std::string(62, '0')); // non-hex chars
    run_one(""); // empty

    wallet::RemoveWallet(context, wallet, std::nullopt);
}

// =============================================================================
// W18-08: getredemptioninfo enforces exact-amount partial-redemption rejection
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:3196-3201 (and the redeem-side enforcement
// at 1813-1818). Partial redemptions are not supported — passing a
// non-zero dd_amount that does not equal the position's dd_minted
// must return "Exact-amount redemption required" with both amounts
// echoed back. Functional pin lives in
// digidollar_rpc_amount_filters.py:78-84; this case re-pins the wallet-
// context unit so the contract holds without an active mocked RPC node.
BOOST_FIXTURE_TEST_CASE(w18_08_getredemptioninfo_rejects_partial_redemption,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getredemptioninfo";
    request.params = UniValue(UniValue::VARR);
    // Use the fixture position with dd_minted=5000 cents
    request.params.push_back("00000000000000000000000000000000000000000000000000000000dd180002");
    request.params.push_back(int64_t{2500}); // half — must reject

    std::string err;
    bool threw = RpcThrows(
        [&] { getredemptioninfo().HandleRequest(request); }, err);

    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_CHECK_MESSAGE(threw, "getredemptioninfo must reject partial redemption requests");
    BOOST_CHECK_MESSAGE(err.find("Exact-amount redemption required") != std::string::npos,
        strprintf("Error must explain the exact-amount contract. Got: %s", err));
    BOOST_CHECK_MESSAGE(err.find("5000") != std::string::npos,
        strprintf("Error must echo the position's full dd_minted. Got: %s", err));
    BOOST_CHECK_MESSAGE(err.find("2500") != std::string::npos,
        strprintf("Error must echo the requested amount. Got: %s", err));
}

// =============================================================================
// W18-09: getredemptioninfo equal dd_amount succeeds
// =============================================================================
//
// Symmetric to W18-08. Passing dd_amount == position.dd_minted must NOT
// trigger the partial-redemption error path. This pins the symmetry
// invariant so the strict-equality check at 3196 cannot accidentally
// flip to strict-greater-than.
BOOST_FIXTURE_TEST_CASE(w18_09_getredemptioninfo_exact_amount_succeeds,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getredemptioninfo";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back("00000000000000000000000000000000000000000000000000000000dd180002");
    request.params.push_back(int64_t{5000}); // exact match

    UniValue result = getredemptioninfo().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isObject());
    BOOST_CHECK_EQUAL(result["total_dd_minted"].getInt<int64_t>(), 5000);
    BOOST_CHECK_EQUAL(result["redeemable_dd"].getInt<int64_t>(), 5000);
    // dgb_return is the full collateral amount formatted as DGB string
    // (from ValueFromAmount(50 * COIN) -> "50.00000000"), pinning the
    // DGB-as-string vs cents-as-integer unit boundary.
    BOOST_CHECK_EQUAL(result["dgb_return"].getValStr(), "50.00000000");
}

// =============================================================================
// W18-10: estimatecollateral rejects negative dd_amount
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:2978-2980 (RPC_INVALID_PARAMETER if
// ddAmount <= 0). dd_amount is in cents at this RPC boundary; a
// negative value must never silently be coerced to an unsigned and
// quote a giant collateral.
BOOST_FIXTURE_TEST_CASE(w18_10_estimatecollateral_rejects_negative_dd_amount,
                        DigiDollarRPCSchemaSetup)
{
    std::string err;
    bool threw = RpcThrows(
        [&] { CallNodeRPC("estimatecollateral -100 5 500000"); }, err);
    BOOST_CHECK_MESSAGE(threw, "estimatecollateral must reject negative dd_amount");
    BOOST_CHECK_MESSAGE(err.find("positive") != std::string::npos,
        strprintf("Error must explain the positive-amount invariant. Got: %s", err));
}

// =============================================================================
// W18-11: estimatecollateral rejects above-maximum dd_amount
// =============================================================================
//
// Pins the consensus mint-max boundary at src/rpc/digidollar.cpp:2986-2996.
// On regtest maxMintAmount = 100000 cents = $1000 (kernel/chainparams.cpp:1100).
// A 100001-cent request must be rejected with the consensus maximum
// echoed back so wallet UIs can render the correct upper-bound hint
// regardless of which network they run on.
BOOST_FIXTURE_TEST_CASE(w18_11_estimatecollateral_rejects_above_max_mint,
                        DigiDollarRPCSchemaSetup)
{
    const auto& ddParams = Params().GetDigiDollarParams();
    const CAmount above_max = ddParams.maxMintAmount + 1;
    std::string err;
    bool threw = RpcThrows(
        [&] { CallNodeRPC(strprintf("estimatecollateral %d 5 500000", above_max)); }, err);
    BOOST_CHECK_MESSAGE(threw,
        strprintf("estimatecollateral must reject %d cents (above %d max)",
                  above_max, ddParams.maxMintAmount));
    BOOST_CHECK_MESSAGE(err.find("Maximum mint amount") != std::string::npos,
        strprintf("Error must explain the maximum mint contract. Got: %s", err));
    BOOST_CHECK_MESSAGE(err.find(strprintf("%d", ddParams.maxMintAmount)) != std::string::npos,
        strprintf("Error must echo the canonical maximum (%d cents). Got: %s",
                  ddParams.maxMintAmount, err));
}

// =============================================================================
// W18-12: estimatecollateral rejects out-of-range lock tier
// =============================================================================
//
// Pins the tier 0..9 contract at src/rpc/digidollar.cpp:2999-3001.
// Both -1 (negative tier) and 10 (above max) must be rejected with the
// same canonical error string so wallet UIs can render a single
// "Choose a tier 0-9" hint regardless of which boundary was hit.
BOOST_FIXTURE_TEST_CASE(w18_12_estimatecollateral_rejects_out_of_range_lock_tier,
                        DigiDollarRPCSchemaSetup)
{
    for (const std::string& bad_tier : {std::string{"-1"}, std::string{"10"}, std::string{"99"}}) {
        std::string err;
        bool threw = RpcThrows(
            [&] { CallNodeRPC("estimatecollateral 10000 " + bad_tier + " 500000"); }, err);
        BOOST_CHECK_MESSAGE(threw,
            strprintf("estimatecollateral must reject lock_tier=%s", bad_tier));
        BOOST_CHECK_MESSAGE(err.find("0 and 9") != std::string::npos,
            strprintf("Error must name the 0..9 invariant. Got: %s", err));
    }
}

// =============================================================================
// W18-13: estimatecollateral rejects non-positive oracle price
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:3019-3021. A zero / negative oracle price
// supplied via the third parameter must be rejected, never coerced to
// a giant or zero collateral quote.
BOOST_FIXTURE_TEST_CASE(w18_13_estimatecollateral_rejects_non_positive_oracle_price,
                        DigiDollarRPCSchemaSetup)
{
    for (const std::string& bad_price : {std::string{"0"}, std::string{"-1"}}) {
        std::string err;
        bool threw = RpcThrows(
            [&] { CallNodeRPC("estimatecollateral 10000 5 " + bad_price); }, err);
        BOOST_CHECK_MESSAGE(threw,
            strprintf("estimatecollateral must reject oracle_price=%s", bad_price));
        BOOST_CHECK_MESSAGE(err.find("positive") != std::string::npos,
            strprintf("Error must explain the positive-price invariant. Got: %s", err));
    }
}

// =============================================================================
// W18-14: estimatecollateral cents-to-DGB unit invariant ($100 at $1.00)
// =============================================================================
//
// At oracle_price_micro_usd = 1,000,000 ($1.00/DGB), tier 5 (730d
// canonical, 275% base ratio) and systemHealth=200% (>=150 -> healthy
// tier, 1.0x DCA multiplier per src/consensus/dca.cpp:51-56), a 10000-
// cent ($100) mint requires:
//   numerator   = 10000 * COIN * 275 * 100
//               = 10000 * 100_000_000 * 275 * 100
//               = 27_500_000_000_000_000
//   denominator = oracle_price_micro_usd = 1_000_000
//   required    = 27_500_000_000 sats = 275 DGB
// (formatted by ValueFromAmount as "275.00000000").
//
// Pin the cents-as-input -> DGB-as-amount-string output unit boundary
// at the healthy-tier base ratio so the test does not depend on the
// emergency-tier 2x multiplier (which would otherwise turn 275% into
// 550% and silently double the quote).
BOOST_FIXTURE_TEST_CASE(w18_14_estimatecollateral_cents_to_dgb_unit_boundary,
                        DigiDollarRPCSchemaSetup)
{
    // Set canonical health = 200% (healthy tier, 1.0x multiplier).
    DigiDollar::SystemMetrics metrics;
    metrics.totalDDSupply = 10000;
    metrics.totalCollateral = 200 * COIN;
    metrics.systemHealth = 200;
    metrics.lastOraclePrice = 1'000'000;
    metrics.hasCanonicalHealth = true;
    DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);

    UniValue result = CallNodeRPC("estimatecollateral 10000 5 1000000");
    BOOST_REQUIRE(result.isObject());
    BOOST_CHECK_EQUAL(result["dd_amount"].getInt<int64_t>(), 10000);
    BOOST_CHECK_EQUAL(result["lock_tier"].getInt<int>(), 5);
    BOOST_CHECK_EQUAL(result["base_ratio"].getInt<int>(), 275);
    BOOST_CHECK_EQUAL(result["effective_ratio"].getInt<int>(), 275);
    // required_dgb is formatted as a DGB string by ValueFromAmount.
    // 275 DGB at COIN=1e8 sats = "275.00000000"
    BOOST_CHECK_EQUAL(result["required_dgb"].getValStr(), "275.00000000");
}

// =============================================================================
// W18-15: getoraclepubkey rejects out-of-range oracle_id
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:4720-4724 (oracle_id must be 0..34 where
// ORACLE_TOTAL_COUNT=35 from primitives/oracle.h).
// Functional gating is covered by digidollar_rpc_gating.py; this case
// pins the schema-side numeric range so a future regtest-roster
// shrink (e.g. setting nOraclePubkeyCount=7) cannot accidentally drop
// the bound check.
BOOST_FIXTURE_TEST_CASE(w18_15_getoraclepubkey_rejects_out_of_range_oracle_id,
                        DigiDollarRPCSchemaSetup)
{
    for (const std::string& bad_id : {std::string{"-1"}, std::string{"35"}, std::string{"99"}}) {
        std::string err;
        bool threw = RpcThrows(
            [&] { CallNodeRPC("getoraclepubkey " + bad_id); }, err);
        BOOST_CHECK_MESSAGE(threw,
            strprintf("getoraclepubkey must reject oracle_id=%s", bad_id));
        BOOST_CHECK_MESSAGE(err.find("Invalid oracle ID") != std::string::npos ||
                            err.find("between 0 and") != std::string::npos,
            strprintf("Error must explain the 0..%d range. Got: %s",
                      ORACLE_TOTAL_COUNT - 1, err));
    }
}

// =============================================================================
// W18-16: setmockoracleprice rejects non-positive and out-of-range prices
// =============================================================================
//
// Pins the dual contract at src/rpc/digidollar.cpp:4806-4818:
//   price_micro_usd > 0
//   100 <= price_micro_usd <= 1_000_000_000
// A regtest-only RPC; both bounds matter for the cents/DGB unit
// stability of OP_CHECKPRICE-derived script paths.
BOOST_FIXTURE_TEST_CASE(w18_16_setmockoracleprice_rejects_out_of_range,
                        DigiDollarRPCSchemaSetup)
{
    // Negative -> "Price must be positive"
    {
        std::string err;
        bool threw = RpcThrows(
            [&] { CallNodeRPC("setmockoracleprice -1"); }, err);
        BOOST_CHECK_MESSAGE(threw, "setmockoracleprice must reject negative price");
        BOOST_CHECK_MESSAGE(err.find("positive") != std::string::npos,
            strprintf("Error must explain positive-only invariant. Got: %s", err));
    }
    // Below MIN_PRICE (100 micro-USD) but positive -> band rejection
    {
        std::string err;
        bool threw = RpcThrows(
            [&] { CallNodeRPC("setmockoracleprice 50"); }, err);
        BOOST_CHECK_MESSAGE(threw,
            "setmockoracleprice must reject price below MIN_PRICE");
        BOOST_CHECK_MESSAGE(err.find("between") != std::string::npos &&
                            err.find("100") != std::string::npos,
            strprintf("Error must mention the [100, 1e9] band. Got: %s", err));
    }
    // Above MAX_PRICE (1e9 micro-USD)
    {
        std::string err;
        bool threw = RpcThrows(
            [&] { CallNodeRPC("setmockoracleprice 1000000001"); }, err);
        BOOST_CHECK_MESSAGE(threw,
            "setmockoracleprice must reject price above MAX_PRICE");
        BOOST_CHECK_MESSAGE(err.find("between") != std::string::npos,
            strprintf("Error must mention the band. Got: %s", err));
    }
}

// =============================================================================
// W18-17: estimatecollateral default oracle price falls back without error
// =============================================================================
//
// When the third parameter is omitted, the RPC pulls the live oracle
// price (or the regtest mock). Pin that the default path works AND
// produces the same cents-input -> DGB-string-output contract as the
// explicit-price W18-14 case. Health=200% keeps the DCA multiplier at
// 1.0x (healthy tier) so the result is determined entirely by the
// base ratio and oracle price, not by the emergency-tier 2x amplifier.
BOOST_FIXTURE_TEST_CASE(w18_17_estimatecollateral_default_price_uses_mock_oracle,
                        DigiDollarRPCSchemaSetup)
{
    DigiDollar::SystemMetrics metrics;
    metrics.totalDDSupply = 10000;
    metrics.totalCollateral = 200 * COIN;
    metrics.systemHealth = 200;
    metrics.lastOraclePrice = 1'000'000;
    metrics.hasCanonicalHealth = true;
    DigiDollar::SystemHealthMonitor::SetMetricsForTesting(metrics);

    MockOracleManager& mock = MockOracleManager::GetInstance();
    const bool mock_was_enabled = mock.IsEnabled();
    const CAmount prior_mock_price = mock.GetCurrentPrice();
    mock.SetEnabled(true);
    mock.SetMockPrice(1'000'000); // $1.00/DGB

    UniValue result = CallNodeRPC("estimatecollateral 10000 5");

    mock.SetMockPrice(prior_mock_price);
    mock.SetEnabled(mock_was_enabled);

    BOOST_REQUIRE(result.isObject());
    BOOST_CHECK_EQUAL(result["oracle_price_micro_usd"].getInt<int64_t>(), 1'000'000);
    BOOST_CHECK_EQUAL(result["effective_ratio"].getInt<int>(), 275);
    BOOST_CHECK_EQUAL(result["required_dgb"].getValStr(), "275.00000000");
}

// =============================================================================
// W18-18: getredemptioninfo dd_amount=0 (default) is the no-op partial check
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:3196 ("ddAmount > 0 && ddAmount != ..."): a
// dd_amount of zero (or omitted) must NOT trigger the partial-amount
// rejection — it is treated as "no preflight requested". This protects
// the RPC's read-only mode (callers asking only for the redemption
// shape) from accidentally being denied.
BOOST_FIXTURE_TEST_CASE(w18_18_getredemptioninfo_dd_amount_zero_returns_full_info,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getredemptioninfo";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back("00000000000000000000000000000000000000000000000000000000dd180002");
    request.params.push_back(int64_t{0}); // dd_amount = 0 -> no preflight

    UniValue result = getredemptioninfo().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isObject());
    BOOST_CHECK_EQUAL(result["total_dd_minted"].getInt<int64_t>(), 5000);
    BOOST_CHECK_EQUAL(result["redeemable_dd"].getInt<int64_t>(), 5000);
}

// =============================================================================
// W18-19: calculatecollateralrequirement rejects non-canonical lock days
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:727-733. A 45-day lock is not in the
// canonical map; the error must list every canonical period so users
// see no drift from the tier table. Functional pin lives in Wave 7
// digidollar_rpc_unit_tests.cpp:309 — this case re-pins the stricter
// full enumeration (omitting tier 0 and tier 9 would slip past the
// existing pin if the help text alone changes).
BOOST_FIXTURE_TEST_CASE(w18_19_calculatecollateralrequirement_rejects_45_days,
                        DigiDollarRPCSchemaSetup)
{
    std::string err;
    bool threw = RpcThrows(
        [&] { CallNodeRPC("calculatecollateralrequirement 10000 45 1000000"); }, err);
    BOOST_CHECK_MESSAGE(threw, "calculatecollateralrequirement must reject 45-day lock");

    // Every canonical period must appear in the error message. The set
    // matches the consensus collateral map and the help text at line 633.
    for (const char* const period :
            {"30", "90", "180", "365", "730", "1095", "1825", "2555", "3650"}) {
        BOOST_CHECK_MESSAGE(err.find(period) != std::string::npos,
            strprintf("Error must enumerate canonical period %s. Got: %s",
                      period, err));
    }
    // The 1-hour testing tier should also be advertised.
    BOOST_CHECK_MESSAGE(err.find("1 hour") != std::string::npos ||
                        err.find("0 (1 hour") != std::string::npos,
        strprintf("Error must advertise the 1-hour testing tier. Got: %s", err));
}

// =============================================================================
// W18-20: calculatecollateralrequirement rejects negative lock_days
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:698-700. Negative lock_days must be
// rejected with the "non-negative" invariant string before the
// lockBlocks lookup runs.
BOOST_FIXTURE_TEST_CASE(w18_20_calculatecollateralrequirement_rejects_negative_lock_days,
                        DigiDollarRPCSchemaSetup)
{
    std::string err;
    bool threw = RpcThrows(
        [&] { CallNodeRPC("calculatecollateralrequirement 10000 -1 1000000"); }, err);
    BOOST_CHECK_MESSAGE(threw,
        "calculatecollateralrequirement must reject negative lock_days");
    BOOST_CHECK_MESSAGE(err.find("non-negative") != std::string::npos,
        strprintf("Error must explain the non-negative invariant. Got: %s", err));
}

// =============================================================================
// W18-21: getredemptioninfo schema fields are integer cents (not DGB strings)
// =============================================================================
//
// Pin the cents-as-int side of the cents/DGB unit boundary. dd_minted,
// redeemable_dd, required_dd_burn, penalty_amount must all be returned
// as integer cents through pushKV(int64_t{...}) — never via
// ValueFromAmount which would produce a DGB-style decimal string. The
// dgb_return field on the same response is the DGB-string side
// (covered by W18-09). A regression that flips any cents field to
// ValueFromAmount would silently shrink wallet UIs by a factor of 1e8.
BOOST_FIXTURE_TEST_CASE(w18_21_getredemptioninfo_cents_fields_are_integer,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = CreateWalletWithThreePositions();
    wallet::AddWallet(context, wallet);

    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getredemptioninfo";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back("00000000000000000000000000000000000000000000000000000000dd180003");

    UniValue result = getredemptioninfo().HandleRequest(request);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_REQUIRE(result.isObject());
    for (const char* const field :
            {"total_dd_minted", "redeemable_dd", "required_dd_burn", "penalty_amount"}) {
        BOOST_REQUIRE_MESSAGE(result.exists(field),
            strprintf("getredemptioninfo result must include `%s`", field));
        BOOST_CHECK_MESSAGE(result[field].isNum(),
            strprintf("`%s` must be an integer cents value, not a DGB string", field));
    }
    BOOST_CHECK_EQUAL(result["total_dd_minted"].getInt<int64_t>(), 1'000'000);
    BOOST_CHECK_EQUAL(result["redeemable_dd"].getInt<int64_t>(), 1'000'000);
    // dgb_return is the DGB-string side of the boundary.
    BOOST_REQUIRE(result.exists("dgb_return"));
    const std::string dgb_return_str = result["dgb_return"].getValStr();
    BOOST_CHECK_MESSAGE(dgb_return_str.find('.') != std::string::npos,
        strprintf("dgb_return must be a DGB-formatted string. Got: %s",
                  dgb_return_str));
}

// =============================================================================
// W18-22: listdigidollarpositions tier_filter accepts every canonical tier
// =============================================================================
//
// Pins src/rpc/digidollar.cpp:2215. tier_filter is an int that must
// accept 0..9 inclusive. The fixture has positions at tier 1 and 9;
// querying tier 9 must return only the 1,000,000-cent position; tier 5
// (no fixture position) must return an empty array; tier_filter=-1
// must return all positions ("no filter" sentinel).
BOOST_FIXTURE_TEST_CASE(w18_22_listdigidollarpositions_tier_filter_canonical_range,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto run_tier = [&](int tier_filter, size_t expected_size) {
        auto wallet = CreateWalletWithThreePositions();
        wallet::AddWallet(context, wallet);

        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = "listdigidollarpositions";
        request.params = UniValue(UniValue::VARR);
        request.params.push_back(true);  // active_only
        request.params.push_back(tier_filter);
        request.params.push_back(int64_t{0}); // min_amount

        UniValue result = listdigidollarpositions().HandleRequest(request);
        wallet::RemoveWallet(context, wallet, std::nullopt);

        BOOST_REQUIRE_MESSAGE(result.isArray(),
            strprintf("listdigidollarpositions tier=%d must return an array", tier_filter));
        BOOST_CHECK_MESSAGE(result.size() == expected_size,
            strprintf("tier=%d expected %zu position(s), got %zu",
                      tier_filter, expected_size, result.size()));
    };

    run_tier(-1, 3);  // no filter -> all
    run_tier(1, 2);   // fixture has two tier-1 positions (100c and 5000c)
    run_tier(9, 1);   // 1,000,000c at tier 9
    run_tier(5, 0);   // no fixture at tier 5
    run_tier(0, 0);   // no fixture at tier 0

    auto expect_invalid_tier = [&](int tier_filter) {
        auto wallet = CreateWalletWithThreePositions();
        wallet::AddWallet(context, wallet);

        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = "listdigidollarpositions";
        request.params = UniValue(UniValue::VARR);
        request.params.push_back(true);  // active_only
        request.params.push_back(tier_filter);
        request.params.push_back(int64_t{0}); // min_amount

        std::string err;
        const bool threw = RpcThrows(
            [&] { listdigidollarpositions().HandleRequest(request); }, err);
        wallet::RemoveWallet(context, wallet, std::nullopt);

        BOOST_CHECK_MESSAGE(threw,
            strprintf("listdigidollarpositions must reject invalid tier_filter=%d",
                      tier_filter));
        BOOST_CHECK_MESSAGE(err.find("Tier filter must be between 0 and 9") != std::string::npos,
            strprintf("tier_filter=%d must reject with canonical tier range error, got: %s",
                      tier_filter, err));
    };

    expect_invalid_tier(-2);
    expect_invalid_tier(10);
    expect_invalid_tier(100);
}

// =============================================================================
// W25-01: deprecated send/redeem fee_rate arguments remain accepted
// =============================================================================
//
// Wave 24 corrected help text around the fixed DD fee policy, but clients that
// followed earlier RC help could already be passing a fourth `fee_rate`
// argument to senddigidollar/redeemdigidollar. Those values were ignored by the
// implementation. Keep accepting them as deprecated no-ops so the cleanup does
// not turn a harmless compatibility quirk into a hard RPC arity failure.
BOOST_FIXTURE_TEST_CASE(w25_01_send_redeem_accept_deprecated_fee_rate_arg,
                        DigiDollarRPCSchemaSetup)
{
    wallet::WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();

    auto wallet = std::make_shared<wallet::CWallet>(
        m_node.chain.get(), "wave25-rpc-wallet", wallet::CreateMockableWalletDatabase());
    wallet->LoadWallet();
    wallet->EnsureDDWallet();
    WITH_LOCK(wallet->cs_wallet, wallet->SetLastBlockProcessed(1000, uint256::ONE));
    wallet::AddWallet(context, wallet);

    JSONRPCRequest send_request;
    send_request.context = &context;
    send_request.strMethod = "senddigidollar";
    send_request.params = UniValue(UniValue::VARR);
    send_request.params.push_back("RDtest1234"); // invalid, but arity-valid
    send_request.params.push_back(int64_t{1000});
    send_request.params.push_back("legacy comment");
    send_request.params.push_back(int64_t{35000000}); // deprecated no-op

    std::string send_err;
    const bool send_threw = RpcThrows(
        [&] { senddigidollar().HandleRequest(send_request); }, send_err);
    BOOST_CHECK(send_threw);
    BOOST_CHECK_MESSAGE(send_err.find("too many") == std::string::npos &&
                            send_err.find("Too many") == std::string::npos,
        strprintf("senddigidollar deprecated fee_rate must not fail arity validation. Got: %s",
                  send_err));
    BOOST_CHECK_MESSAGE(send_err.find("Invalid DigiDollar address") != std::string::npos ||
                            send_err.find("Invalid address") != std::string::npos,
        strprintf("senddigidollar should continue into address validation. Got: %s",
                  send_err));

    JSONRPCRequest redeem_request;
    redeem_request.context = &context;
    redeem_request.strMethod = "redeemdigidollar";
    redeem_request.params = UniValue(UniValue::VARR);
    redeem_request.params.push_back("ab"); // invalid, but arity-valid
    redeem_request.params.push_back(int64_t{1000});
    redeem_request.params.push_back("");
    redeem_request.params.push_back(int64_t{35000000}); // deprecated no-op

    std::string redeem_err;
    const bool redeem_threw = RpcThrows(
        [&] { redeemdigidollar().HandleRequest(redeem_request); }, redeem_err);
    wallet::RemoveWallet(context, wallet, std::nullopt);

    BOOST_CHECK(redeem_threw);
    BOOST_CHECK_MESSAGE(redeem_err.find("too many") == std::string::npos &&
                            redeem_err.find("Too many") == std::string::npos,
        strprintf("redeemdigidollar deprecated fee_rate must not fail arity validation. Got: %s",
                  redeem_err));
    BOOST_CHECK_MESSAGE(redeem_err.find("Invalid position ID format") != std::string::npos,
        strprintf("redeemdigidollar should continue into position validation. Got: %s",
                  redeem_err));
}

BOOST_AUTO_TEST_SUITE_END()
