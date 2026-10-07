// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLAR_QT_TRANSLATE_H
#define DIGIBYTE_QT_DIGIDOLLAR_QT_TRANSLATE_H

#include <string>

// DD-FA-FUNC-032 (Wave 19 Agent C): Translate consensus reject reasons into
// user-facing explanations for the Qt mint flow.
//
// The Qt mint widget broadcasts the mint transaction directly through
// node().broadcastTransaction (not through the mintdigidollar RPC), so the
// Wave 6 RPC pre-check `25639153d7` does not run on this code path.
// Mempool/ConnectBlock still reject the transaction with the canonical
// consensus tag (e.g. "minting-blocked-during-err") but the user only sees
// the bare tag. This translator maps known DigiDollar/oracle reject tokens
// to plain-English explanations with remediation hints. Unknown reasons
// pass through unchanged so operators retain full forensic detail.
//
// Implemented as an inline header so a Boost unit test can pin the
// translation contract without pulling in the full Qt build (which is
// disabled in the audit configuration via ENABLE_QT_TRUE='#').
inline std::string TranslateMintRejectReasonForUser(const std::string& reason)
{
    if (reason.empty()) return reason;

    auto contains = [&reason](const char* needle) {
        return reason.find(needle) != std::string::npos;
    };

    // ERR (Emergency Redemption Ratio) is active and minting is paused
    // (consensus rule, see src/digidollar/validation.cpp:2595-2597 and
    // src/consensus/err.cpp ShouldBlockMinting()).
    if (contains("minting-blocked-during-err")) {
        return "DigiDollar minting is paused because the system is in "
               "Emergency Redemption Ratio (ERR) recovery mode. "
               "Please wait until system health recovers above the ERR "
               "threshold and try again.";
    }

    // No recent v0x03 MuSig2 oracle quote in the mempool; mint cannot
    // be relayed until the next valid bundle arrives
    // (src/validation.cpp:154-217 HasRecentValidMuSig2OracleQuote).
    if (contains("no-musig2-quote") || contains("missing-oracle-quote")) {
        return "No recent oracle quote is available. "
               "Please wait for the next block (about 15 seconds) and try again.";
    }

    if (contains("minting-volatility-pause")) {
        return "DigiDollar minting is paused because the candidate oracle quote "
               "differs by at least 20% from the ancestor reference. "
               "Transfers and redemptions are not paused by this volatility rule.";
    }
    if (contains("volatility state not ready") || contains("volatility_state_not_ready")) {
        return "Required ancestor price data is unavailable. Restore or download "
               "the missing blocks before minting. This is a local data-readiness error.";
    }

    // Volatility freeze is active; the oracle price would cross the
    // freeze threshold (src/digidollar/validation.cpp:2600-2604).
    if (contains("volatility-freeze") || contains("volatility-protection") ||
        contains("minting-frozen-volatility") || contains("all-operations-frozen")) {
        return "DigiDollar minting is temporarily frozen due to oracle "
               "price volatility. Please try again later when prices stabilise.";
    }

    // Lock tier is canonical-only; the txbuilder somehow produced a
    // non-canonical duration (src/digidollar/validation.cpp).
    if (contains("bad-mint-lock-tier-duration") || contains("bad-mint-lock-period") ||
        contains("bad-mint-lock-tier")) {
        return "The selected lock tier produced a non-canonical lock "
               "duration. Please reopen the mint dialog and select a "
               "lock tier from the dropdown.";
    }

    // Oracle price unavailable / zero (fail-closed during IBD or
    // before activation, src/script/interpreter.cpp:725).
    if (contains("bad-oracle-price")) {
        return "Oracle price data is not yet available. "
               "Please wait for the node to finish syncing and for the "
               "next oracle bundle to arrive, then try again.";
    }

    // Insufficient collateral / DCA multiplier mismatch surfaced by
    // the consensus collateral check.
    if (contains("bad-mint-collateral") || contains("insufficient-collateral")) {
        return "The provided DGB collateral does not satisfy the current "
               "Dynamic Collateral Adjustment requirement. The oracle "
               "price may have moved; please reopen the mint dialog "
               "and review the updated collateral estimate.";
    }

    // Unknown reason: pass through verbatim. We deliberately do NOT
    // wrap unknown reasons so debug output remains usable.
    return reason;
}

#endif // DIGIBYTE_QT_DIGIDOLLAR_QT_TRANSLATE_H
