// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// =============================================================================
// Wave 17 Agent B - Wallet Spendability, Watch-Only, Accounting, Privacy
// =============================================================================
//
// Strengthens DD wallet test coverage for the Wave 17 brief (#3-#6):
//   3. DD balance accounting positive/negative paths (mint -> send -> redeem
//      conservation, double-counted UTXOs, stale unconfirmed inputs).
//   4. Minconf semantics — confirmed-only enforcement: GetTotalDDBalance
//      excludes wallet-known unconfirmed DD UTXOs (depth < 1) and the
//      include_unconfirmed=false default of GetDDUTXOs() suppresses them
//      from coin selection.
//   5. DD change unit invariant: SelectDDCoins enforces
//      change == 0 || change >= minOutputAmount and refuses to return
//      "successful" selections that would build a sub-minimum DD change
//      output, so the consensus rule
//      transfer-dd-amount-below-minimum cannot be tripped from the wallet
//      side after the fact.
//   6. Zero-balance generated DD addresses must NOT be advertised through
//      address->balance accounting maps; they may surface in the read-only
//      "known DD addresses" list, but they cannot inflate dd_balances or
//      contaminate the spendable balance.
//
// Watch-only (Wave 17 #1) and locked-wallet (Wave 17 #2) matrix items are
// already pinned by Wave 16 Agent B (DD-FA-TEST-022 / DD-FA-TEST-023 /
// DD-FA-TEST-024) and the existing rh08_locked_wallet_blocks_dd_key_access
// test in digidollar_wallet_security_tests.cpp; this file only adds
// non-overlapping defence-in-depth pins for the change/balance/minconf
// surface that Wave 16 deferred to "Wave 17 wallet spendability".
//
// All cases run under the existing WalletTestingSetup fixture, registered
// alongside the other wallet/test/ DigiDollar suites in
// src/Makefile.test.include.

#include <boost/test/unit_test.hpp>

#include <wallet/wallet.h>
#include <wallet/digidollarwallet.h>
#include <wallet/walletdb.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/walletutil.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/digidollar.h>
#include <digidollar/digidollar.h>
#include <key.h>
#include <key_io.h>
#include <node/context.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <validation.h>

namespace wallet {

BOOST_FIXTURE_TEST_SUITE(digidollar_wave17_spendability_tests, WalletTestingSetup)

// -----------------------------------------------------------------------------
// Helper: random 256-bit hash (small wrapper around GetRandBytes for use in
// COutPoint construction at call sites).
// -----------------------------------------------------------------------------
static uint256 RandHash()
{
    uint256 h;
    GetRandBytes(h);
    return h;
}

// -----------------------------------------------------------------------------
// Helper: build a minimal CTransactionRef with vout[1] = zero-value P2TR DD
// token (good enough for AddDDUTXO bookkeeping; the wallet does not run
// consensus on these inputs in this fixture).
// -----------------------------------------------------------------------------
static CTransactionRef MakeMintLikeTx()
{
    CKey dd_key;
    dd_key.MakeNewKey(true);
    XOnlyPubKey dd_xonly(dd_key.GetPubKey());
    auto tweaked = dd_xonly.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(RandHash(), 0);
    mtx.vout.resize(2);
    mtx.vout[0].nValue = 1 * COIN;          // collateral placeholder
    mtx.vout[1].nValue = 0;                 // DD token output
    mtx.vout[1].scriptPubKey << OP_1 << ToByteVector(tweaked->first);
    return MakeTransactionRef(std::move(mtx));
}

static CDigiDollarAddress MakeValidDDAddress()
{
    CKey key;
    key.MakeNewKey(true);
    XOnlyPubKey xonly(key.GetPubKey());
    return CDigiDollarAddress(EncodeDigiDollarAddress(CTxDestination{WitnessV1Taproot(xonly)}));
}

static void AddConfirmedWalletTx(WalletTestingSetup& setup, CWallet& wallet, const CTransactionRef& tx)
{
    LOCK(wallet.cs_wallet);
    const CBlockIndex* tip = WITH_LOCK(::cs_main, return setup.m_node.chainman->ActiveChain().Tip());
    BOOST_REQUIRE(tip != nullptr);
    wallet.SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    wallet.AddToWallet(tx, TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/0});
}

// =============================================================================
// W17-01: SelectDDCoins enforces DD change >= minOutputAmount
// =============================================================================
//
// DD outputs have a consensus minimum (Params().GetDigiDollarParams()
// .minOutputAmount = 100 cents on regtest/test). Wallet coin selection
// must refuse to return "selected_total = target + 1cent" because that
// would force a 1-cent DD change output that consensus would later reject
// with transfer-dd-amount-below-minimum.
//
// This pin guards SelectDDCoins() at src/wallet/digidollarwallet.cpp:5215
// (loop break) and 5229 (final success check). A regression that drops
// the (current_change == 0 || current_change >= min_change) test would
// silently let the wallet build invalid transactions.

BOOST_AUTO_TEST_CASE(w17_01_select_ddcoins_rejects_below_min_change)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const CAmount min_change = Params().GetDigiDollarParams().minOutputAmount;
    BOOST_REQUIRE_GT(min_change, 0);

    // Two confirmed-equivalent UTXOs (no m_wallet -> all considered spendable).
    // Deliberately pick a UTXO so that target = full - 1 cent would force
    // sub-minimum change. The selector must NOT report success in that case.
    const CAmount utxo_amount = 50000; // $500.00
    COutPoint a(RandHash(), 1);
    dd_wallet.AddDDUTXO(a, utxo_amount);

    std::vector<COutPoint> selected;
    CAmount selected_total = 0;
    const CAmount sub_min_target = utxo_amount - (min_change - 1); // change = min_change - 1

    BOOST_CHECK_MESSAGE(
        !dd_wallet.SelectDDCoins(sub_min_target, selected, selected_total),
        "DD-FA-TEST-025: SelectDDCoins returned success for a target that "
        "would force a sub-minOutputAmount DD change output; consensus "
        "would later reject the resulting transfer with "
        "transfer-dd-amount-below-minimum");
    BOOST_CHECK(selected.empty());
    BOOST_CHECK_EQUAL(selected_total, 0);

    // Sanity: an exact-spend (change == 0) is still allowed.
    selected.clear();
    selected_total = 0;
    BOOST_CHECK(dd_wallet.SelectDDCoins(utxo_amount, selected, selected_total));
    BOOST_CHECK_EQUAL(selected_total, utxo_amount);
    BOOST_REQUIRE_EQUAL(selected.size(), 1u);
    BOOST_CHECK(selected[0] == a);

    // And a target that leaves change == min_change is allowed.
    selected.clear();
    selected_total = 0;
    BOOST_CHECK(dd_wallet.SelectDDCoins(utxo_amount - min_change, selected,
                                        selected_total));
    BOOST_CHECK_EQUAL(selected_total, utxo_amount);
}

BOOST_AUTO_TEST_CASE(w17_01b_select_ddcoins_picks_extra_utxo_to_avoid_dust_change)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const CAmount min_change = Params().GetDigiDollarParams().minOutputAmount;
    BOOST_REQUIRE_GT(min_change, 0);

    // Two unequal UTXOs. Asking for an amount that would leave dust change
    // from the smaller UTXO alone forces the selector to add the larger one.
    // Greedy sorts smallest-first, so the first hit is the small UTXO with
    // dust change; the loop must keep going until the change invariant holds.
    const CAmount small = 10000; // $100
    const CAmount large = 100000; // $1000
    COutPoint a(RandHash(), 1);
    COutPoint b(RandHash(), 1);
    dd_wallet.AddDDUTXO(a, small);
    dd_wallet.AddDDUTXO(b, large);

    std::vector<COutPoint> selected;
    CAmount selected_total = 0;
    const CAmount target = small - (min_change - 1); // would be dust from {a} alone
    BOOST_REQUIRE_GT(target, 0);

    BOOST_CHECK(dd_wallet.SelectDDCoins(target, selected, selected_total));
    BOOST_CHECK_EQUAL(selected_total, small + large);
    BOOST_CHECK_GE(selected_total - target, min_change);
    BOOST_CHECK_EQUAL(selected.size(), 2u);
}

BOOST_AUTO_TEST_CASE(transfer_planner_reports_automatic_change_and_insufficient_funds)
{
    DigiDollarWallet dd_wallet(nullptr);
    const COutPoint first(RandHash(), 0);
    dd_wallet.AddDDUTXO(first, 10000);
    DDTransferPlan plan;
    std::string error;
    const auto address = MakeValidDDAddress();

    BOOST_REQUIRE(dd_wallet.PlanDigiDollarTransfer({{address, 10000}}, plan, error));
    BOOST_CHECK_EQUAL(plan.dd_change, 0);
    BOOST_REQUIRE(dd_wallet.PlanDigiDollarTransfer({{address, 9900}}, plan, error));
    BOOST_CHECK_EQUAL(plan.dd_change, 100);
    for (CAmount change : {1, 50, 99}) {
        BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{address, 10000 - change}}, plan, error));
        BOOST_CHECK_MESSAGE(error.find("change is below minimum") != std::string::npos, error);
    }
    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{address, 10100}}, plan, error));
    BOOST_CHECK_MESSAGE(error.find("Insufficient confirmed DD balance") != std::string::npos, error);

    dd_wallet.AddDDUTXO(COutPoint(RandHash(), 0), 20000);
    BOOST_REQUIRE(dd_wallet.PlanDigiDollarTransfer({{address, 9950}}, plan, error));
    BOOST_CHECK_EQUAL(plan.dd_change, 20050);
    const std::vector<COutPoint> selected{first};
    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{address, 9950}}, plan, error, &selected));
    BOOST_CHECK_MESSAGE(error.find("change is below minimum") != std::string::npos, error);
}

BOOST_AUTO_TEST_CASE(transfer_planner_distinguishes_unconfirmed_inputs)
{
    DigiDollarWallet dd_wallet(&m_wallet);
    auto tx = MakeMintLikeTx();
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.AddToWallet(tx, TxStateInactive{});
    }
    const COutPoint outpoint(tx->GetHash(), 1);
    dd_wallet.AddDDUTXO(outpoint, 10000);
    DDTransferPlan plan;
    std::string error;
    const auto address = MakeValidDDAddress();
    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{address, 9950}}, plan, error));
    BOOST_CHECK_MESSAGE(error.find("No spendable confirmed DD") != std::string::npos, error);
    const std::vector<COutPoint> selected{outpoint};
    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{address, 9950}}, plan, error, &selected));
    BOOST_CHECK_MESSAGE(error.find("unconfirmed") != std::string::npos, error);
}

BOOST_AUTO_TEST_CASE(w17_07_selected_dd_input_planner_accepts_owned_input_and_reports_change)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const COutPoint selected_input(RandHash(), 1);
    dd_wallet.AddDDUTXO(selected_input, 50000);

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input};

    BOOST_REQUIRE(dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 30000}}, plan, error, &preset));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(plan.total_amount, 30000);
    BOOST_CHECK_EQUAL(plan.selected_dd_total, 50000);
    BOOST_CHECK_EQUAL(plan.dd_change, 20000);
    BOOST_REQUIRE_EQUAL(plan.dd_utxos.size(), 1u);
    BOOST_CHECK(plan.dd_utxos[0] == selected_input);
    BOOST_REQUIRE_EQUAL(plan.dd_amounts.size(), 1u);
    BOOST_CHECK_EQUAL(plan.dd_amounts[0], 50000);
    BOOST_CHECK_GT(plan.projected_vsize, 0u);
    BOOST_CHECK_GE(plan.estimated_fee, 10000000);
}

BOOST_AUTO_TEST_CASE(w17_08_selected_dd_input_planner_rejects_insufficient_selection)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const COutPoint selected_input(RandHash(), 1);
    dd_wallet.AddDDUTXO(selected_input, 25000);

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input};

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 30000}}, plan, error, &preset));
    BOOST_CHECK(error.find("Insufficient selected DD input amount") != std::string::npos);
    BOOST_CHECK(plan.dd_utxos.empty());
}

BOOST_AUTO_TEST_CASE(w17_09_selected_dd_input_rejects_unknown_and_wrong_wallet_inputs)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    DDTransferPlan plan;
    std::string error;
    const COutPoint unknown(RandHash(), 1);
    std::vector<COutPoint> preset{unknown};

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 10000}}, plan, error, &preset));
    BOOST_CHECK(error.find("unknown or not owned") != std::string::npos);

    const COutPoint wrong_wallet(RandHash(), 1);
    dd_wallet.AddDDUTXO(wrong_wallet, 10000);
    error.clear();
    preset = {wrong_wallet};
    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 10000}}, plan, error, &preset));
    BOOST_CHECK(error.find("not owned by this wallet") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(w17_10_selected_dd_input_rejects_unconfirmed_and_spent_inputs)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CTransactionRef unconfirmed_tx = MakeMintLikeTx();
    const COutPoint unconfirmed_outpoint(unconfirmed_tx->GetHash(), 1);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.AddToWallet(unconfirmed_tx, TxStateInMempool{});
    }
    dd_wallet.AddDDUTXO(unconfirmed_outpoint, 10000);

    DDTransferPlan plan;
    std::string error;
    std::vector<COutPoint> preset{unconfirmed_outpoint};
    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 10000}}, plan, error, &preset));
    BOOST_CHECK(error.find("unconfirmed") != std::string::npos);

    CTransactionRef confirmed_tx = MakeMintLikeTx();
    const COutPoint spent_outpoint(confirmed_tx->GetHash(), 1);
    AddConfirmedWalletTx(*this, m_wallet, confirmed_tx);
    dd_wallet.AddDDUTXO(spent_outpoint, 10000);

    CMutableTransaction spend_mtx;
    spend_mtx.vin.push_back(CTxIn(spent_outpoint));
    spend_mtx.vout.push_back(CTxOut(1, CScript() << OP_TRUE));
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.AddToWallet(MakeTransactionRef(std::move(spend_mtx)), TxStateInMempool{});
    }

    error.clear();
    preset = {spent_outpoint};
    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 10000}}, plan, error, &preset));
    BOOST_CHECK(error.find("already spent") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(w17_11_selected_dd_input_rejects_non_dd_wallet_outputs)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(RandHash(), 0);
    mtx.vout.resize(2);
    mtx.vout[0].nValue = 1 * COIN;
    mtx.vout[1].nValue = 1 * COIN;
    mtx.vout[1].scriptPubKey << OP_TRUE;
    CTransactionRef tx = MakeTransactionRef(std::move(mtx));

    const COutPoint selected_input(tx->GetHash(), 1);
    AddConfirmedWalletTx(*this, m_wallet, tx);
    dd_wallet.AddDDUTXO(selected_input, 10000);

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input};

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 10000}}, plan, error, &preset));
    BOOST_CHECK(error.find("not a standard DigiDollar token output") != std::string::npos);
    BOOST_CHECK(plan.dd_utxos.empty());
}

BOOST_AUTO_TEST_CASE(w17_12_selected_dd_input_rejects_duplicates)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const COutPoint selected_input(RandHash(), 1);
    dd_wallet.AddDDUTXO(selected_input, 50000);

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input, selected_input};

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 60000}}, plan, error, &preset));
    BOOST_CHECK(error.find("Duplicate selected DD input") != std::string::npos);
    BOOST_CHECK(plan.dd_utxos.empty());
}

BOOST_AUTO_TEST_CASE(w17_13_selected_dd_input_rejects_below_min_change)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const CAmount min_change = Params().GetDigiDollarParams().minOutputAmount;
    BOOST_REQUIRE_GT(min_change, 1);

    const COutPoint selected_input(RandHash(), 1);
    dd_wallet.AddDDUTXO(selected_input, 50000);

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input};
    const CAmount target = 50000 - (min_change - 1);

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), target}}, plan, error, &preset));
    BOOST_CHECK(error.find("Selected DD input change") != std::string::npos);
    BOOST_CHECK(plan.dd_utxos.empty());
}

BOOST_AUTO_TEST_CASE(w17_14_planner_rejects_below_min_recipient_output)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const CAmount min_output = Params().GetDigiDollarParams().minOutputAmount;
    BOOST_REQUIRE_GT(min_output, 1);

    const COutPoint selected_input(RandHash(), 1);
    dd_wallet.AddDDUTXO(selected_input, min_output);

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input};

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), min_output - 1}}, plan, error, &preset));
    BOOST_CHECK(error.find("below minimum DigiDollar output") != std::string::npos);
    BOOST_CHECK(plan.dd_utxos.empty());
}

BOOST_AUTO_TEST_CASE(w17_15_selected_dd_planner_is_non_mutating)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CTransactionRef tx = MakeMintLikeTx();
    const COutPoint selected_input(tx->GetHash(), 1);
    AddConfirmedWalletTx(*this, m_wallet, tx);
    dd_wallet.AddDDUTXO(selected_input, 50000);

    BOOST_REQUIRE(dd_wallet.HasDDUTXO(selected_input));
    BOOST_REQUIRE_EQUAL(dd_wallet.GetTotalDDBalance(), 50000);

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input};

    BOOST_REQUIRE(dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), 30000}}, plan, error, &preset));
    BOOST_CHECK(error.empty());

    BOOST_CHECK(dd_wallet.HasDDUTXO(selected_input));
    BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), 50000);
    auto utxos = dd_wallet.GetDDUTXOs();
    BOOST_REQUIRE_EQUAL(utxos.size(), 1u);
    BOOST_CHECK(utxos[0].outpoint == selected_input);
}

BOOST_AUTO_TEST_CASE(w17_16_planner_rejects_opreturn_capacity_overflow)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const CAmount min_output = Params().GetDigiDollarParams().minOutputAmount;
    std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
    recipients.reserve(50);
    for (int i = 0; i < 50; ++i) {
        recipients.push_back({MakeValidDDAddress(), min_output});
    }

    const COutPoint selected_input(RandHash(), 1);
    dd_wallet.AddDDUTXO(selected_input, min_output * recipients.size());

    DDTransferPlan plan;
    std::string error;
    const std::vector<COutPoint> preset{selected_input};

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer(recipients, plan, error, &preset));
    BOOST_CHECK(error.find("Too many DigiDollar outputs") != std::string::npos);
    BOOST_CHECK(plan.dd_utxos.size() == 1u || plan.dd_utxos.empty());
}

BOOST_AUTO_TEST_CASE(w17_17_planner_rejects_standard_weight_overflow)
{
    DigiDollarWallet dd_wallet(/*wallet=*/nullptr);

    const CAmount min_output = Params().GetDigiDollarParams().minOutputAmount;
    std::vector<COutPoint> preset;
    preset.reserve(1200);
    for (int i = 0; i < 1200; ++i) {
        const COutPoint selected_input(RandHash(), 1);
        dd_wallet.AddDDUTXO(selected_input, min_output);
        preset.push_back(selected_input);
    }

    DDTransferPlan plan;
    std::string error;

    BOOST_CHECK(!dd_wallet.PlanDigiDollarTransfer({{MakeValidDDAddress(), min_output}}, plan, error, &preset));
    BOOST_CHECK(error.find("too large") != std::string::npos);
}

// =============================================================================
// W17-02: GetTotalDDBalance returns zero when every DD UTXO is unconfirmed
// =============================================================================
//
// Existing test_mixed_confirmed_unconfirmed_dd_balance pins the mixed case
// (one confirmed + one unconfirmed -> only confirmed counted). It does NOT
// pin the corner where every wallet-known DD UTXO is still in the mempool —
// the resulting balance must be exactly zero, not "a fallback from the
// dd_utxos map". A regression that flipped the depth check from < 1 to
// >= 0, or that fell back to "no wallet -> count all", would silently let
// senddigidollar / redeemdigidollar consume unconfirmed inputs and then
// fail at consensus time with an unhelpful error.
//
// This case uses a real CWallet (m_wallet from the fixture) so the
// "m_wallet known + depth < 1" branch is exercised end-to-end.

BOOST_AUTO_TEST_CASE(w17_02_unconfirmed_only_balance_is_zero)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CTransactionRef tx = MakeMintLikeTx();
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.AddToWallet(tx, TxStateInMempool{});
    }

    const COutPoint dd_outpoint(tx->GetHash(), 1);
    const CAmount dd_amount = 25000; // $250.00
    dd_wallet.AddDDUTXO(dd_outpoint, dd_amount);

    BOOST_CHECK_MESSAGE(
        dd_wallet.GetTotalDDBalance() == 0,
        "DD-FA-TEST-025: wallet-known unconfirmed DD UTXO leaked into the "
        "spendable balance; senddigidollar / redeemdigidollar would later "
        "consume it and fail at consensus time on the confirmed-only rule");

    // GetDDUTXOs() with the default include_unconfirmed=false must NOT return
    // it either, so coin selection cannot pick it up.
    auto utxos_default = dd_wallet.GetDDUTXOs();
    BOOST_CHECK(utxos_default.empty());

    // Explicit include_unconfirmed=true is the read-only display path; it
    // should still surface the entry.
    auto utxos_pending = dd_wallet.GetDDUTXOs(/*include_unconfirmed=*/true);
    BOOST_CHECK_EQUAL(utxos_pending.size(), 1u);

    // GetPendingDDBalance must report the unconfirmed amount so RPC/Qt can
    // show "pending" without tricking spendable callers.
    BOOST_CHECK_EQUAL(dd_wallet.GetPendingDDBalance(), dd_amount);
}

BOOST_AUTO_TEST_CASE(w17_02b_wallet_local_dd_utxo_is_pending_not_spendable)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CTransactionRef tx = MakeMintLikeTx();
    const COutPoint dd_outpoint(tx->GetHash(), 1);
    const CAmount dd_amount = 12500; // $125.00

    // A wallet-local mint created while wallet broadcast is disabled is stored
    // as non-abandoned TxStateInactive, not TxStateInMempool. It is still a
    // pending local mint from the UI/RPC point of view, but must not be
    // selectable as spendable DD until it confirms.
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.AddToWallet(tx, TxStateInactive{/*abandoned=*/false});
    }
    dd_wallet.AddDDUTXO(dd_outpoint, dd_amount);

    BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), 0);
    BOOST_CHECK(dd_wallet.GetDDUTXOs().empty());
    BOOST_CHECK_EQUAL(dd_wallet.GetPendingDDBalance(), dd_amount);

    // An abandoned local transaction must not appear in pending display.
    CTransactionRef abandoned_tx = MakeMintLikeTx();
    const COutPoint abandoned_outpoint(abandoned_tx->GetHash(), 1);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.AddToWallet(abandoned_tx, TxStateInactive{/*abandoned=*/true});
    }
    dd_wallet.RemoveDDUTXO(dd_outpoint);
    dd_wallet.AddDDUTXO(abandoned_outpoint, dd_amount);
    BOOST_CHECK_EQUAL(dd_wallet.GetPendingDDBalance(), 0);
}

// =============================================================================
// W17-03: Confirming an unconfirmed DD UTXO promotes it to spendable balance
// =============================================================================
//
// Positive-path follow-up to W17-02. After AddToWallet flips from
// TxStateInMempool to TxStateConfirmed, the same DD UTXO must now appear in
// the spendable balance, and GetPendingDDBalance must drop back to 0.
// Pins the round-trip so a regression that left wtxs cached at "depth = 0"
// after confirmation would be caught immediately.

BOOST_AUTO_TEST_CASE(w17_03_confirmation_moves_dd_balance_pending_to_spendable)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CTransactionRef tx = MakeMintLikeTx();
    const COutPoint dd_outpoint(tx->GetHash(), 1);
    const CAmount dd_amount = 12345; // $123.45

    // Step 1: insert unconfirmed.
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.AddToWallet(tx, TxStateInMempool{});
    }
    dd_wallet.AddDDUTXO(dd_outpoint, dd_amount);
    BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), 0);
    BOOST_CHECK_EQUAL(dd_wallet.GetPendingDDBalance(), dd_amount);

    // Step 2: confirm via the wallet's chain tip.
    {
        LOCK(m_wallet.cs_wallet);
        const CBlockIndex* tip = WITH_LOCK(::cs_main,
            return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE(tip != nullptr);
        m_wallet.SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        m_wallet.AddToWallet(tx,
            TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/0});
    }

    BOOST_CHECK_MESSAGE(
        dd_wallet.GetTotalDDBalance() == dd_amount,
        "DD-FA-TEST-025: post-confirmation DD UTXO did not promote into the "
        "spendable balance; the wallet would refuse to spend a freshly "
        "confirmed mint output");
    BOOST_CHECK_EQUAL(dd_wallet.GetPendingDDBalance(), 0);

    // The spendable UTXO must now be selectable by SelectDDCoins for an
    // exact-spend target.
    std::vector<COutPoint> selected;
    CAmount selected_total = 0;
    BOOST_CHECK(dd_wallet.SelectDDCoins(dd_amount, selected, selected_total));
    BOOST_CHECK_EQUAL(selected_total, dd_amount);
    BOOST_REQUIRE_EQUAL(selected.size(), 1u);
    BOOST_CHECK(selected[0] == dd_outpoint);
}

// =============================================================================
// W17-04: Removing a spent DD UTXO drops the spendable balance to zero
// =============================================================================
//
// Negative path covering the post-send / post-redeem accounting:
// RemoveDDUTXO is what ProcessTransactionForDD calls when a confirmed spend
// is observed. After removal, GetTotalDDBalance must return zero and the
// outpoint must be invisible to coin selection. Pins the contract that
// erasure happens at the dd_utxos map level rather than a parallel
// "available" cache.

BOOST_AUTO_TEST_CASE(w17_04_remove_dd_utxo_drops_balance_and_visibility)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CTransactionRef tx = MakeMintLikeTx();
    const COutPoint dd_outpoint(tx->GetHash(), 1);
    const CAmount dd_amount = 60000; // $600.00

    {
        LOCK(m_wallet.cs_wallet);
        const CBlockIndex* tip = WITH_LOCK(::cs_main,
            return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE(tip != nullptr);
        m_wallet.SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        m_wallet.AddToWallet(tx,
            TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/0});
    }

    dd_wallet.AddDDUTXO(dd_outpoint, dd_amount);
    BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), dd_amount);
    BOOST_CHECK(dd_wallet.HasDDUTXO(dd_outpoint));

    dd_wallet.RemoveDDUTXO(dd_outpoint);

    BOOST_CHECK_MESSAGE(
        dd_wallet.GetTotalDDBalance() == 0,
        "DD-FA-TEST-025: spendable DD balance survived RemoveDDUTXO; "
        "spent DD UTXOs would be double-counted across send/redeem flows");
    BOOST_CHECK(!dd_wallet.HasDDUTXO(dd_outpoint));
    BOOST_CHECK(dd_wallet.GetDDUTXOs().empty());

    std::vector<COutPoint> selected;
    CAmount selected_total = 0;
    BOOST_CHECK(!dd_wallet.SelectDDCoins(dd_amount, selected, selected_total));
}

// =============================================================================
// W17-05: Zero-balance generated DD addresses do not pollute dd_balances
// =============================================================================
//
// Wave 17 brief #6: zero-balance generated addresses should not appear in
// the wallet's address->balance accounting map. Storing an address-key only
// (the path used by `getdigidollaraddress` to pre-register an HD-derived
// receive address) must surface the address through GetKnownDDAddresses() so
// the listdigidollaraddresses RPC can show known-but-empty DD addresses,
// but it must NOT inflate dd_balances or the spendable totals.
//
// This pins the privacy / accounting boundary: a fresh generated DD address
// stays at "balance = 0" until a real DD UTXO actually pays to it; a
// regression that auto-credited "balance = 0" entries into dd_balances
// would be caught by GetBalanceCount() growing to 1 here.

BOOST_AUTO_TEST_CASE(w17_05_known_dd_address_does_not_inflate_balance_map)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    CKey k;
    k.MakeNewKey(/*fCompressedIn=*/true);
    XOnlyPubKey output_key(k.GetPubKey());
    BOOST_REQUIRE(output_key.IsFullyValid());

    dd_wallet.StoreAddressKey(output_key, k);

    // The address must show up in the read-only "known addresses" list so
    // listdigidollaraddresses can render it (even with balance 0).
    auto known = dd_wallet.GetKnownDDAddresses();
    BOOST_CHECK_MESSAGE(!known.empty(),
        "DD-FA-FUNC-026: stored DD address key did not surface through "
        "GetKnownDDAddresses, so listdigidollaraddresses would lose track "
        "of HD-generated receive addresses");

    // It must NOT inflate the dd_balances accounting map (no zero-row
    // pollution) and must NOT contribute to GetTotalDDBalance.
    BOOST_CHECK_MESSAGE(
        dd_wallet.GetBalanceCount() == 0,
        "DD-FA-FUNC-026: storing a DD address key auto-credited a "
        "zero-balance row in dd_balances; getdigidollarbalance and "
        "listdigidollaraddresses would over-report address_count");
    BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), 0);
    BOOST_CHECK(dd_wallet.GetDDUTXOs().empty());
}

// =============================================================================
// W17-06: Conservation across mint -> spend simulated UTXO transitions
// =============================================================================
//
// Conservation pin for Wave 17 brief #3 (DD balance accounting positive
// and negative paths). Walks the wallet through a simulated lifecycle:
//   mint produces 100000 cents -> spendable balance = 100000
//   spend 30000 cents (consume mint UTXO, add 70000 cent change UTXO) ->
//       spendable balance = 70000
//   redeem the 70000 change to zero -> spendable balance = 0
// At every step the address-balance map and the spendable total agree, and
// no transient state leaves a phantom DD credit behind.

BOOST_AUTO_TEST_CASE(w17_06_lifecycle_conserves_dd_balance)
{
    DigiDollarWallet dd_wallet(&m_wallet);

    // Stage 1: confirmed mint UTXO worth 100000 cents.
    CTransactionRef mint_tx = MakeMintLikeTx();
    const COutPoint mint_utxo(mint_tx->GetHash(), 1);
    const CAmount minted = 100000; // $1000

    {
        LOCK(m_wallet.cs_wallet);
        const CBlockIndex* tip = WITH_LOCK(::cs_main,
            return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE(tip != nullptr);
        m_wallet.SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        m_wallet.AddToWallet(mint_tx,
            TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, 0});
    }
    dd_wallet.AddDDUTXO(mint_utxo, minted);
    BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), minted);

    // Stage 2: simulate a 30000-cent spend by removing the mint UTXO and
    // adding a confirmed 70000-cent change UTXO.
    const CAmount spend = 30000; // $300
    const CAmount change_amount = minted - spend;
    dd_wallet.RemoveDDUTXO(mint_utxo);

    CTransactionRef spend_tx = MakeMintLikeTx();
    const COutPoint change_utxo(spend_tx->GetHash(), 1);
    {
        LOCK(m_wallet.cs_wallet);
        const CBlockIndex* tip = WITH_LOCK(::cs_main,
            return m_node.chainman->ActiveChain().Tip());
        m_wallet.AddToWallet(spend_tx,
            TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, 0});
    }
    dd_wallet.AddDDUTXO(change_utxo, change_amount);

    BOOST_CHECK_MESSAGE(
        dd_wallet.GetTotalDDBalance() == change_amount,
        "DD-FA-TEST-025: post-spend balance did not converge on the change "
        "amount; the consumed mint UTXO is still credited, breaking "
        "conservation between dd_utxos and senddigidollar");

    // Stage 3: simulate a redeem-everything that erases the change UTXO.
    dd_wallet.RemoveDDUTXO(change_utxo);
    BOOST_CHECK_MESSAGE(
        dd_wallet.GetTotalDDBalance() == 0,
        "DD-FA-TEST-025: full-burn redeem did not zero the spendable "
        "balance; downstream RPC/Qt would still show a non-zero DD total");
    BOOST_CHECK(dd_wallet.GetDDUTXOs().empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
