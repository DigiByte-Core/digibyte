// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Fuzz targets for the v9.26.4 DigiDollar-compatible pruning surface.
//
// Targets:
//   dd_extract_amount_blockdb - DigiDollar::ExtractDDAmountFromBlockDb(), the
//       txindex-free DD amount resolution path that reads the creating
//       transaction out of the retained block via a TxLookupFn callback
//       (src/digidollar/validation.cpp; wired up in production by
//       MakeCachedBlockTxLookup in src/validation.cpp).
//   dd_prune_activation_floor - property fuzz of the shared dd_floor helper
//       DigiDollar::EarliestActivationFloor (src/consensus/digidollar.cpp),
//       the single source of truth used by the "digidollar" prune lock in
//       src/node/chainstate.cpp, the startup guards in
//       src/digidollar/health.cpp and src/oracle/bundle_manager.cpp, and
//       validation's EarliestDigiDollarActivationHeight — plus the inline
//       prune-lock clamp arithmetic used by Chainstate::FlushStateToDisk in
//       src/validation.cpp (still replicated below; update if it changes).
//       (unit coverage: src/test/digidollar_txindex_tests.cpp and
//       test/functional/feature_digidollar_pruning.py).
//   dd_prune_coin_gating - SpendsDigiDollarCollateralVault() /
//       RequiresDigiDollarValidation() on MAINNET params with coins created
//       around the DigiDollar activation floor and a fuzzed block-db lookup.
//       This drives the production EarliestDigiDollarActivationHeight
//       pre-floor coin skip and the LookupPreviousTransaction txLookup
//       fallback, i.e. the exact reason pruning below the floor is safe.
//

#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

//! Mirrors MEMPOOL_HEIGHT from src/txmempool.h without pulling in the mempool.
constexpr uint32_t FUZZ_MEMPOOL_HEIGHT{0x7FFFFFFF};

//! Mirrors PRUNE_LOCK_BUFFER from src/validation.cpp.
constexpr int FUZZ_PRUNE_LOCK_BUFFER{10};

constexpr int64_t NEVER = Consensus::BIP9Deployment::NEVER_ACTIVE;
constexpr int64_t ALWAYS = Consensus::BIP9Deployment::ALWAYS_ACTIVE;

/** Build a DD-versioned mutable transaction with the given tx type byte. */
CMutableTransaction MakeDDTx(uint8_t txTypeByte)
{
    CMutableTransaction mtx;
    // DD version format: txType(8) | flags(8) | 0x0770 (lower 16)
    mtx.nVersion = (static_cast<int32_t>(txTypeByte) << 24) | 0x0770;
    return mtx;
}

/** Build a P2TR-shaped script (OP_1 + 32 fuzzed bytes). */
CScript MakeP2TR(FuzzedDataProvider& fdp)
{
    std::vector<uint8_t> payload = fdp.ConsumeBytes<uint8_t>(32);
    if (payload.size() < 32) payload.resize(32, 0xAB);
    CScript s;
    s << OP_1 << payload;
    return s;
}

/** Build a DD-shaped "creating transaction" that a block-db lookup would return.
 *
 * shape 1: mint-shaped     - OP_RETURN <DD> <1> <amount> <lockHeight> <lockTier>,
 *                            one value>0 P2TR collateral output, one zero-value
 *                            P2TR DD token output.
 * shape 2: transfer-shaped - OP_RETURN <DD> <2> <amount>..., N zero-value P2TR outputs.
 * shape 3: corrupted       - fuzzed type byte / duplicate DD OP_RETURNs /
 *                            optionally coinbase-shaped (T5-02 rejection path).
 */
CTransactionRef BuildDDShapedPrevTx(FuzzedDataProvider& fdp, uint8_t shape)
{
    const std::vector<unsigned char> dd_marker = {'D', 'D'};

    if (shape == 1) {
        CMutableTransaction mtx = MakeDDTx(DigiDollar::DD_TX_MINT);
        mtx.vin.emplace_back(COutPoint(uint256::ONE, 0));
        // Collateral output (value > 0 P2TR)
        mtx.vout.emplace_back(fdp.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY), MakeP2TR(fdp));
        // DD token output (zero-value P2TR)
        mtx.vout.emplace_back(0, MakeP2TR(fdp));
        // Mint OP_RETURN: DD <type=1> <ddAmount> <lockHeight> <lockTier>
        CScript opret;
        opret << OP_RETURN << dd_marker << CScriptNum(DigiDollar::DD_TX_MINT);
        opret << CScriptNum(fdp.ConsumeIntegral<int64_t>());                        // ddAmount (any value)
        opret << CScriptNum(fdp.ConsumeIntegralInRange<int64_t>(0, 30'000'000));    // lockHeight
        opret << CScriptNum(fdp.ConsumeIntegralInRange<int64_t>(0, 9));             // lockTier
        mtx.vout.emplace_back(0, opret);
        return MakeTransactionRef(std::move(mtx));
    }

    if (shape == 2) {
        CMutableTransaction mtx = MakeDDTx(DigiDollar::DD_TX_TRANSFER);
        mtx.vin.emplace_back(COutPoint(uint256::ONE, 0));
        const int splits = fdp.ConsumeIntegralInRange<int>(1, 4);
        for (int i = 0; i < splits; i++) {
            mtx.vout.emplace_back(0, MakeP2TR(fdp));
        }
        // Transfer OP_RETURN: DD <type=2> <amount1> ... <amountN>
        CScript opret;
        opret << OP_RETURN << dd_marker << CScriptNum(DigiDollar::DD_TX_TRANSFER);
        for (int i = 0; i < splits; i++) {
            opret << CScriptNum(fdp.ConsumeIntegral<int64_t>());
        }
        mtx.vout.emplace_back(0, opret);
        return MakeTransactionRef(std::move(mtx));
    }

    // shape 3: corrupted / adversarial
    CMutableTransaction mtx = MakeDDTx(fdp.ConsumeIntegral<uint8_t>());
    if (fdp.ConsumeBool()) {
        // Coinbase-shaped source tx (must be rejected as a DD source, T5-02)
        mtx.vin.emplace_back(COutPoint(uint256(), std::numeric_limits<uint32_t>::max()));
    } else {
        mtx.vin.emplace_back(COutPoint(uint256::ONE, 0));
    }
    const int n_out = fdp.ConsumeIntegralInRange<int>(0, 5);
    for (int i = 0; i < n_out; i++) {
        const uint8_t kind = fdp.ConsumeIntegralInRange<uint8_t>(0, 2);
        if (kind == 0) {
            mtx.vout.emplace_back(fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY), MakeP2TR(fdp));
        } else if (kind == 1) {
            // DD OP_RETURN with a fuzzed type byte and fuzzed pushes (may
            // mismatch the version type, may appear more than once).
            CScript opret;
            opret << OP_RETURN << dd_marker << CScriptNum(fdp.ConsumeIntegral<uint8_t>());
            const int pushes = fdp.ConsumeIntegralInRange<int>(0, 3);
            for (int p = 0; p < pushes; p++) {
                opret << CScriptNum(fdp.ConsumeIntegral<int64_t>());
            }
            mtx.vout.emplace_back(0, opret);
        } else {
            const auto raw = ConsumeRandomLengthByteVector(fdp, 48);
            mtx.vout.emplace_back(fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY),
                                  CScript(raw.begin(), raw.end()));
        }
    }
    return MakeTransactionRef(std::move(mtx));
}

/** Fuzzed coin creation height, biased towards activation/prune boundaries. */
uint32_t ConsumeCoinHeight(FuzzedDataProvider& fdp)
{
    const uint32_t boundaries[] = {
        0, 1,
        599, 600, 601,          // testnet DD activation floor
        649, 650, 651,          // regtest DD/oracle height gates
        23'627'519, 23'627'520, 23'627'521, // mainnet DD activation floor
        FUZZ_MEMPOOL_HEIGHT - 1, FUZZ_MEMPOOL_HEIGHT,
        std::numeric_limits<uint32_t>::max(),
    };
    if (fdp.ConsumeBool()) return fdp.PickValueInArray(boundaries);
    return fdp.ConsumeIntegral<uint32_t>();
}

/** Build one of the TxLookupFn flavors used by both lookup-driven targets.
 *
 * The production callback (MakeCachedBlockTxLookup in src/validation.cpp) only
 * ever returns true with a non-null transaction found by txid in the retained
 * block at coinHeight. Mode 3 mimics that contract exactly; mode 2 stresses a
 * hash-mismatched return; modes 0/1 are the fail-closed paths (no callback /
 * block or tx not found).
 */
DigiDollar::TxLookupFn MakeFuzzedLookup(uint8_t mode, const CTransactionRef& prev_tx)
{
    switch (mode) {
    case 0:
        return nullptr;
    case 1:
        return [](const uint256&, uint32_t, CTransactionRef&) { return false; };
    case 2:
        return [prev_tx](const uint256&, uint32_t, CTransactionRef& tx_out) {
            tx_out = prev_tx;
            return true;
        };
    default:
        return [prev_tx](const uint256& txid, uint32_t, CTransactionRef& tx_out) {
            if (txid != prev_tx->GetHash()) return false;
            tx_out = prev_tx;
            return true;
        };
    }
}

/** Build a Consensus::Params carrying only the fields EarliestActivationFloor reads. */
Consensus::Params MakeFloorParams(int64_t nStartTime, int64_t nTimeout, int min_activation_height, int nDDActivationHeight)
{
    Consensus::Params cp{};
    auto& dep = cp.vDeployments[Consensus::DEPLOYMENT_DIGIDOLLAR];
    dep.nStartTime = nStartTime;
    dep.nTimeout = nTimeout;
    dep.min_activation_height = min_activation_height;
    cp.nDDActivationHeight = nDDActivationHeight;
    return cp;
}

} // namespace

void initialize_dd_prune_blockdb()
{
    SelectParams(ChainType::REGTEST);
}

void initialize_dd_prune_gating_main()
{
    // Mainnet params so the DigiDollar activation floor (23,627,520) is a real,
    // positive boundary and the pre-floor coin skip is live.
    SelectParams(ChainType::MAIN);
}

// ============================================================================
// Target 1: dd_extract_amount_blockdb
//
// Feed arbitrary/DD-shaped transactions through the block-db DD amount
// extraction used by pruned nodes (no txindex).
// ============================================================================

FUZZ_TARGET(dd_extract_amount_blockdb, .init = initialize_dd_prune_blockdb)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    // The "creating transaction" the block-db lookup hands back.
    CTransactionRef prev_tx;
    const uint8_t shape = fdp.ConsumeIntegralInRange<uint8_t>(0, 3);
    if (shape == 0) {
        // Arbitrary bytes deserialized as a transaction.
        const std::optional<CMutableTransaction> mtx = ConsumeDeserializable<CMutableTransaction>(fdp);
        if (!mtx) return;
        prev_tx = MakeTransactionRef(*mtx);
    } else {
        prev_tx = BuildDDShapedPrevTx(fdp, shape);
    }

    // Outpoint being resolved: usually the tx's own hash (contract-accurate),
    // sometimes unrelated; output index both in and out of range.
    COutPoint prevout;
    prevout.hash = fdp.ConsumeBool() ? prev_tx->GetHash() : ConsumeUInt256(fdp);
    prevout.n = fdp.ConsumeBool() ? fdp.ConsumeIntegralInRange<uint32_t>(0, 8)
                                  : fdp.ConsumeIntegral<uint32_t>();

    const uint32_t coin_height = ConsumeCoinHeight(fdp);

    const uint8_t mode = fdp.ConsumeIntegralInRange<uint8_t>(0, 3);
    const DigiDollar::TxLookupFn lookup = MakeFuzzedLookup(mode, prev_tx);

    // Seed the out-param with garbage: the callee must fully define it.
    CAmount amount{-99};
    const bool ok = DigiDollar::ExtractDDAmountFromBlockDb(prevout, coin_height, lookup, amount);

    // Fail-closed invariant: success iff a strictly positive DD amount was
    // recovered. Failure must never leave a positive amount behind for a
    // caller that (incorrectly) ignored the return value.
    assert(ok == (amount > 0));
    if (mode == 0 || mode == 1) {
        // No callback / lookup miss: hard failure with a zeroed amount.
        assert(!ok && amount == 0);
    }

    // Determinism: extraction is a pure function of (prevout, coinHeight,
    // looked-up tx) - a pruned node and an archival node resolving the same
    // coin must agree.
    CAmount amount2{-77};
    const bool ok2 = DigiDollar::ExtractDDAmountFromBlockDb(prevout, coin_height, lookup, amount2);
    assert(ok2 == ok && amount2 == amount);

    // Documented (not asserted): on success the amount is only guaranteed
    // positive here. MAX_DIGIDOLLAR bounding is enforced at the consumption
    // sites (AddDDAmount / conservation checks in src/digidollar/validation.cpp).
}

// ============================================================================
// Target 2: dd_prune_activation_floor
//
// Property fuzz of the shared activation-floor helper
// (DigiDollar::EarliestActivationFloor) and the prune-lock clamp arithmetic.
// ============================================================================

FUZZ_TARGET(dd_prune_activation_floor, .init = initialize_dd_prune_blockdb)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const int64_t time_values[] = {
        NEVER, ALWAYS, 0, 1,
        1780156800, // testnet26 BIP9 genesis
        1780272000, // mainnet DD start epoch
        std::numeric_limits<int64_t>::max(),
    };
    const int height_values[] = {
        std::numeric_limits<int>::min(), -1, 0, 1,
        600, 650, 23'627'520,
        std::numeric_limits<int>::max() - 1, std::numeric_limits<int>::max(),
    };

    const int64_t start = fdp.ConsumeBool() ? fdp.PickValueInArray(time_values) : fdp.ConsumeIntegral<int64_t>();
    const int64_t timeout = fdp.ConsumeBool() ? fdp.PickValueInArray(time_values) : fdp.ConsumeIntegral<int64_t>();
    const int min_act = fdp.ConsumeBool() ? fdp.PickValueInArray(height_values) : fdp.ConsumeIntegral<int>();
    const int dd_act = fdp.ConsumeBool() ? fdp.PickValueInArray(height_values) : fdp.ConsumeIntegral<int>();

    const Consensus::Params cp = MakeFloorParams(start, timeout, min_act, dd_act);
    const int dd_floor = DigiDollar::EarliestActivationFloor(cp);

    // Deterministic.
    assert(dd_floor == DigiDollar::EarliestActivationFloor(cp));

    if (start == NEVER || timeout == NEVER) {
        // DD can never activate: no block retention needed, no prune lock.
        assert(dd_floor == 0);
    } else {
        // Spec check: the floor is the earliest DD-creating height, so the
        // retained window [dd_floor, tip] covers every block a DD spend can
        // ever need to read. A higher floor would let pruning delete a
        // needed block.
        assert(dd_floor == ((start == ALWAYS) ? min_act : std::min(dd_act, min_act)));

        // The floor never exceeds the BIP9 minimum activation height.
        assert(dd_floor <= min_act);

        // Sane params (non-negative heights) yield a non-negative floor, and
        // strictly positive mainnet/testnet-style params always register the
        // lock on a pruned node (dd_floor > 0 gate in chainstate.cpp).
        if (min_act >= 0 && dd_act >= 0) assert(dd_floor >= 0);
        if (min_act > 0 && dd_act > 0) assert(dd_floor > 0);
    }

    // Live-params sanity for the selected chain (regtest here): the shared
    // helper is what production consumes, so it must be deterministic and
    // non-negative on real chainparams too.
    {
        const Consensus::Params& cp_live = Params().GetConsensus();
        const int live_floor = DigiDollar::EarliestActivationFloor(cp_live);
        assert(live_floor == DigiDollar::EarliestActivationFloor(cp_live));
        assert(live_floor >= 0);
    }

    // Prune-lock registration predicate from chainstate.cpp: lock registered
    // iff pruning is on and the floor is positive.
    const bool prune_enabled = fdp.ConsumeBool();
    const bool lock_registered = prune_enabled && dd_floor > 0;
    if (lock_registered) {
        // Clamp arithmetic from Chainstate::FlushStateToDisk
        // (src/validation.cpp):
        //   lock_height = height_first - PRUNE_LOCK_BUFFER - 1
        //   last_prune  = max(1, min(last_prune, lock_height))
        // height_first == INT_MAX is the "no lock" sentinel and is skipped in
        // production before this expression runs.
        const int height_first = dd_floor;
        if (height_first != std::numeric_limits<int>::max()) {
            const int lock_height = height_first - FUZZ_PRUNE_LOCK_BUFFER - 1;
            // No signed overflow: the lock only exists for height_first >= 1.
            assert(static_cast<int64_t>(height_first) - FUZZ_PRUNE_LOCK_BUFFER - 1 == lock_height);

            const int last_prune_in = fdp.ConsumeIntegralInRange<int>(-1, std::numeric_limits<int>::max());
            const int clamped = std::max(1, std::min(last_prune_in, lock_height));
            assert(clamped >= 1);
            if (height_first >= FUZZ_PRUNE_LOCK_BUFFER + 2) {
                // Binding property: nothing at or above (floor - buffer) can be
                // pruned, no matter how high the requested prune height is.
                assert(clamped <= height_first - FUZZ_PRUNE_LOCK_BUFFER - 1);
            }
            // For height_first in [1, 11] the max(1, ...) floor dominates; that
            // is inherited upstream Bitcoin Core behavior (block file 0 is
            // practically never prunable), documented rather than asserted.
        }
    }
}

// ============================================================================
// Target 3: dd_prune_coin_gating
//
// Drive the production pre-floor coin skip (EarliestDigiDollarActivationHeight)
// and the LookupPreviousTransaction block-db fallback through the public
// SpendsDigiDollarCollateralVault / RequiresDigiDollarValidation entry points,
// on mainnet params where the activation floor is a real positive boundary.
// ============================================================================

FUZZ_TARGET(dd_prune_coin_gating, .init = initialize_dd_prune_gating_main)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const CChainParams& chainparams = Params();
    const Consensus::Params& cp = chainparams.GetConsensus();

    // The production pre-floor skip boundary. On mainnet v9.26.4 this is the
    // DigiDollar activation floor used for the prune lock.
    const int dd_floor = DigiDollar::EarliestActivationFloor(cp);
    assert(dd_floor == 23'627'520);

    // "Creating transaction" served by the fuzzed block-db lookup.
    const CTransactionRef prev_tx = BuildDDShapedPrevTx(fdp, fdp.ConsumeIntegralInRange<uint8_t>(1, 3));

    // Spending transaction: sometimes DD-marked, sometimes arbitrary version.
    CMutableTransaction spend;
    spend.nVersion = fdp.ConsumeBool()
                         ? ((static_cast<int32_t>(fdp.ConsumeIntegralInRange<uint8_t>(0, 4)) << 24) | 0x0770)
                         : fdp.ConsumeIntegral<int32_t>();

    CCoinsView backend;
    CCoinsViewCache coins{&backend};

    bool all_coins_below_floor = true;
    const int n_in = fdp.ConsumeIntegralInRange<int>(0, 4);
    for (int i = 0; i < n_in; i++) {
        COutPoint op;
        op.hash = fdp.ConsumeBool() ? prev_tx->GetHash() : ConsumeUInt256(fdp);
        op.n = fdp.ConsumeIntegralInRange<uint32_t>(0, 8);
        spend.vin.emplace_back(op);

        // Sometimes the coin is simply absent from the view.
        if (!fdp.ConsumeBool()) continue;

        // Coin height around the activation floor (Coin::nHeight is 31 bits).
        const uint32_t heights[] = {
            0u, 1u,
            static_cast<uint32_t>(dd_floor - 1),
            static_cast<uint32_t>(dd_floor),
            static_cast<uint32_t>(dd_floor + 1),
            FUZZ_MEMPOOL_HEIGHT,
        };
        const uint32_t h = fdp.ConsumeBool() ? fdp.PickValueInArray(heights)
                                             : fdp.ConsumeIntegralInRange<uint32_t>(0, FUZZ_MEMPOOL_HEIGHT);
        if (h >= static_cast<uint32_t>(dd_floor)) all_coins_below_floor = false;

        CTxOut out;
        out.nValue = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        const uint8_t script_kind = fdp.ConsumeIntegralInRange<uint8_t>(0, 2);
        if (script_kind == 0) {
            out.scriptPubKey = MakeP2TR(fdp);
        } else if (script_kind == 1) {
            const auto raw = ConsumeRandomLengthByteVector(fdp, 40);
            out.scriptPubKey = CScript(raw.begin(), raw.end());
        } else {
            // Registered collateral vault script (process-local metadata map),
            // exercising the IsRegisteredCollateralVaultScript branch.
            out.scriptPubKey = MakeP2TR(fdp);
            DigiDollar::RegisterScriptMetadata(out.scriptPubKey, DigiDollar::ScriptType::COLLATERAL_LOCK,
                                               /*ddAmount=*/10000, /*lockHeight=*/1000);
        }
        coins.AddCoin(op, Coin(out, static_cast<int>(h), /*fCoinBaseIn=*/fdp.ConsumeBool()),
                      /*possible_overwrite=*/true);
    }

    const CTransaction tx(spend);
    const DigiDollar::TxLookupFn lookup = MakeFuzzedLookup(fdp.ConsumeIntegralInRange<uint8_t>(0, 3), prev_tx);

    const int tip_height = fdp.ConsumeIntegralInRange<int>(0, 40'000'000);
    const CAmount price = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000'000LL);
    const DigiDollar::ValidationContext ctx(tip_height, price, /*collateral=*/15000, chainparams,
                                            &coins, /*skip_oracle=*/true, lookup,
                                            /*pool=*/nullptr, /*block_time=*/0);

    const bool vault1 = DigiDollar::SpendsDigiDollarCollateralVault(tx, ctx);
    const bool vault2 = DigiDollar::SpendsDigiDollarCollateralVault(tx, ctx);
    assert(vault1 == vault2); // deterministic

    // Pre-activation coins are plain DGB history: if every resolvable input
    // coin was created below the activation floor, the tx must never classify
    // as a DD collateral vault spend. This is exactly the property that makes
    // pruning blocks below the floor safe.
    if (all_coins_below_floor) assert(!vault1);

    const bool requires_dd = DigiDollar::RequiresDigiDollarValidation(tx, ctx);
    assert(requires_dd == (DigiDollar::HasDigiDollarMarker(tx) || vault1));

    // Without a coins view the vault check fails closed to "not a vault spend".
    const DigiDollar::ValidationContext ctx_nocoins(tip_height, price, 15000, chainparams,
                                                    nullptr, true, lookup, nullptr, 0);
    assert(!DigiDollar::SpendsDigiDollarCollateralVault(tx, ctx_nocoins));
}
