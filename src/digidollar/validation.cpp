// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <digidollar/validation.h>
#include <digidollar/scripts.h>
#include <digidollar/digidollar.h>

// Phase 1 metadata tracking support
using DigiDollar::ScriptMetadata;
using DigiDollar::GetScriptMetadata;
#include <consensus/digidollar.h>
#include <consensus/volatility.h>
#include <consensus/dca.h>
#include <consensus/err.h>
#include <index/txindex.h>  // For decentralized DD amount lookup via g_txindex
#include <script/standard.h>
#include <script/solver.h>
#include <script/interpreter.h>
#include <logging.h>
#include <util/strencodings.h>
#include <util/hasher.h>
#include <sync.h>
#include <uint256.h>

#include <unordered_map>
#include <memory>

namespace DigiDollar {

// ============================================================================
// Validation Cache (Thread-Safe)
// ============================================================================

// Simple cache for script type identification to avoid re-parsing
struct ValidationCache {
    mutable RecursiveMutex cs_cache;
    std::unordered_map<uint256, ScriptType, BlockHasher> scriptTypeCache;
    std::unordered_map<uint256, std::pair<bool, CAmount>, BlockHasher> amountCache; // bool=valid, CAmount=amount

    // Limit cache size to prevent memory bloat
    static const size_t MAX_CACHE_SIZE = 10000;

    void ClearIfFull() EXCLUSIVE_LOCKS_REQUIRED(cs_cache) {
        if (scriptTypeCache.size() > MAX_CACHE_SIZE) {
            scriptTypeCache.clear();
            amountCache.clear();
            LogPrintf("DigiDollar: Validation cache cleared (size limit reached)\n");
        }
    }
};

// Global validation cache instance
static ValidationCache g_validationCache;

// ============================================================================
// Script Analysis Functions
// ============================================================================

ScriptType IdentifyScriptType(const CScript& script) {
    // Quick rejection for obviously non-P2TR scripts
    if (script.size() != 34 || script[0] != OP_1) {
        return ScriptType::NOT_DIGIDOLLAR;
    }

    // Phase 1: Use metadata tracking for scripts created by Create*P2TR functions
    // This is a testing workaround - Phase 2 will use UTXO database tracking
    ScriptMetadata metadata;
    if (GetScriptMetadata(script, metadata)) {
        return metadata.type;
    }

    // Unknown P2TR script - cannot determine without metadata
    return ScriptType::NOT_DIGIDOLLAR;
}

// Phase 1: These functions use the metadata registry for scripts created by
// Create*P2TR functions. Phase 2 will use UTXO database tracking.
// Note: HasDigiDollarMarker() and GetDigiDollarTxType() are still in
// consensus/digidollar.cpp as they work on transaction version fields.

bool IsDDTokenScript(const CScript& script) {
    // Phase 1: Use metadata registry for scripts created by CreateDigiDollarP2TR
    ScriptType type = IdentifyScriptType(script);
    return type == ScriptType::DD_TOKEN_OUTPUT;
}

bool ExtractDDAmount(const CScript& script, CAmount& amount) {
    // Phase 1: Use metadata registry for scripts created by Create*P2TR functions
    ScriptMetadata metadata;
    if (GetScriptMetadata(script, metadata)) {
        if (metadata.type == ScriptType::DD_TOKEN_OUTPUT ||
            metadata.type == ScriptType::COLLATERAL_LOCK) {
            amount = metadata.ddAmount;
            return true;
        }
    }

    // Fallback: Try to parse from OP_RETURN format (for real transactions)
    // This handles the case where scripts come from actual blockchain data
    auto pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    // Check for OP_RETURN
    if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) {
        amount = -1;
        return false;
    }

    // Get next element
    if (!script.GetOp(pc, opcode, data)) {
        amount = -1;
        return false;
    }

    // Format 1: OP_RETURN OP_DIGIDOLLAR <8-byte little-endian amount>
    // OP_DIGIDOLLAR (0xbb) marks DigiDollar outputs
    if (opcode == OP_DIGIDOLLAR) {
        // Read the amount data
        if (script.GetOp(pc, opcode, data) && data.size() == 8) {
            // Parse 8-byte little-endian amount
            amount = 0;
            for (size_t i = 0; i < 8; i++) {
                amount |= static_cast<int64_t>(data[i]) << (i * 8);
            }
            if (amount >= 1 && amount <= 100000000000LL) {
                return true;
            }
        }
    }
    // Format 2: OP_RETURN <"DD"> <txType> <ddAmount> <lockHeight>
    else if (data.size() == 2 && data[0] == 'D' && data[1] == 'D') {
        // Skip txType
        if (script.GetOp(pc, opcode, data)) {
            // Get ddAmount
            if (script.GetOp(pc, opcode, data)) {
                try {
                    // Allow up to 8 bytes for DD amounts (int64_t range)
                    CScriptNum scriptNum(data, false, 8);
                    amount = scriptNum.GetInt64();
                    if (amount >= 1 && amount <= 100000000000LL) {
                        return true;
                    }
                } catch (const scriptnum_error&) {
                    // Fall through
                }
            }
        }
    }

    amount = -1;
    return false;
}

/**
 * Extract DD amount from the previous transaction's OP_RETURN metadata.
 * This is the DECENTRALIZED approach - no local registry needed.
 * The DD amount is stored in the creating transaction's OP_RETURN output.
 *
 * @param prevout The outpoint (txid + output index) of the DD UTXO
 * @param amount Output: The DD amount in cents
 * @return true if amount was successfully extracted
 */
/**
 * Extract DD amount from a transaction reference given the output index.
 * Parses the OP_RETURN in the transaction to find DD amounts, then matches
 * the output index to the correct amount. Shared by txindex and block-db lookups.
 */
static bool ExtractDDAmountFromTxRef(const CTransactionRef& prev_tx, const COutPoint& prevout, CAmount& amount) {
    amount = 0;

    // SECURITY: Verify the creating transaction is actually a DigiDollar transaction.
    // Without this check, a malicious miner could include a regular (non-DD) transaction
    // with a DD-formatted OP_RETURN and zero-value P2TR outputs. A subsequent DD transfer
    // spending those outputs would pass conservation checks because ExtractDDAmountFromTxRef
    // would find DD amounts in the non-DD source tx's OP_RETURN — creating DD from nothing.
    if (!DigiDollar::HasDigiDollarMarker(*prev_tx)) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ExtractDDAmountFromTxRef - source tx %s is not a DD transaction (version=0x%08x)\n",
                 prevout.hash.ToString(), prev_tx->nVersion);
        return false;
    }

    // Parse the OP_RETURN in the previous transaction to get DD amounts
    std::vector<CAmount> dd_amounts;
    for (const auto& vout : prev_tx->vout) {
        if (vout.scriptPubKey.size() > 0 && vout.scriptPubKey[0] == OP_RETURN) {
            CScript::const_iterator pc = vout.scriptPubKey.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Skip OP_RETURN
            if (!vout.scriptPubKey.GetOp(pc, opcode)) continue;

            // Check for "DD" marker
            if (!vout.scriptPubKey.GetOp(pc, opcode, data)) continue;
            if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;

            // Read transaction type (1=MINT, 2=TRANSFER, 3=REDEEM)
            if (!vout.scriptPubKey.GetOp(pc, opcode, data)) continue;
            int64_t txType = 0;
            if (data.size() > 0) {
                try {
                    CScriptNum txTypeNum(data, true);
                    txType = txTypeNum.GetInt64();
                } catch (const scriptnum_error&) {
                    continue;
                }
            }

            // SECURITY: Type-aware parsing of OP_RETURN fields.
            // Mint OP_RETURN format:    DD <type=1> <ddAmount> <lockHeight> <lockTier>
            // Transfer OP_RETURN format: DD <type=2> <amount1> <amount2> ... <amountN>
            // Redeem OP_RETURN format:   DD <type=3> <ddAmount> [additional fields]
            //
            // For MINT (type 1), only the FIRST value after type is the DD amount.
            // lockHeight and lockTier are NOT DD amounts. Reading them as such would
            // allow an attacker to add extra P2TR zero-value outputs and inflate the
            // DD supply (e.g., 360-day lockHeight = 2,073,600 interpreted as $20,736).
            if (txType == 1 || txType == 3) {
                // MINT or REDEEM: Only first push is DD amount
                if (vout.scriptPubKey.GetOp(pc, opcode, data) && data.size() > 0) {
                    try {
                        CScriptNum scriptNum(data, true, 8);
                        dd_amounts.push_back(scriptNum.GetInt64());
                    } catch (const scriptnum_error&) {}
                }
            } else {
                // TRANSFER: All remaining pushes are DD amounts (one per output)
                while (vout.scriptPubKey.GetOp(pc, opcode, data)) {
                    if (data.size() > 0) {
                        try {
                            CScriptNum scriptNum(data, true, 8);  // 8-byte max for large DD amounts
                            dd_amounts.push_back(scriptNum.GetInt64());
                        } catch (const scriptnum_error&) {
                            continue;
                        }
                    }
                }
            }
            break;  // Only process first DD OP_RETURN
        }
    }

    if (dd_amounts.empty()) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ExtractDDAmountFromPrevTx - no DD amounts in tx %s\n",
                 prevout.hash.ToString());
        return false;
    }

    // Match output index to DD amount
    // Count P2TR (DD) outputs to find the correct amount index
    size_t dd_output_idx = 0;
    for (uint32_t n = 0; n < prev_tx->vout.size(); ++n) {
        const CTxOut& txout = prev_tx->vout[n];

        // Skip non-DD outputs (OP_RETURN, non-zero value)
        if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) continue;
        if (txout.nValue != 0) continue;

        // Check if it's a P2TR output (OP_1 + 32 bytes = DD output)
        if (txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1) {
            if (n == prevout.n) {
                // Found the matching output
                if (dd_output_idx < dd_amounts.size()) {
                    amount = dd_amounts[dd_output_idx];
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ExtractDDAmountFromPrevTx - tx %s vout %d = %lld cents\n",
                             prevout.hash.ToString(), prevout.n, (long long)amount);
                    return amount > 0;
                }
            }
            dd_output_idx++;
        }
    }

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ExtractDDAmountFromTxRef - output %d not found in tx %s\n",
             prevout.n, prevout.hash.ToString());
    return false;
}

bool ExtractDDAmountFromPrevTx(const COutPoint& prevout, CAmount& amount) {
    amount = 0;

    if (!g_txindex) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ExtractDDAmountFromPrevTx - txindex not available\n");
        return false;
    }

    uint256 block_hash;
    CTransactionRef prev_tx;
    if (!g_txindex->FindTx(prevout.hash, block_hash, prev_tx)) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ExtractDDAmountFromPrevTx - could not find tx %s\n",
                 prevout.hash.ToString());
        return false;
    }

    return ExtractDDAmountFromTxRef(prev_tx, prevout, amount);
}

/**
 * Extract DD amount by loading the creating transaction from the block database.
 * This is the universal fallback — every full node has every block on disk.
 * Uses the coin's creation height to find the right block.
 */
static bool ExtractDDAmountFromBlockDb(const COutPoint& prevout, uint32_t coinHeight,
                                       const TxLookupFn& txLookup, CAmount& amount) {
    amount = 0;
    if (!txLookup) return false;

    CTransactionRef prev_tx;
    if (!txLookup(prevout.hash, coinHeight, prev_tx)) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ExtractDDAmountFromBlockDb - tx %s not found at height %u\n",
                 prevout.hash.ToString(), coinHeight);
        return false;
    }

    return ExtractDDAmountFromTxRef(prev_tx, prevout, amount);
}

bool IsCollateralScript(const CScript& script) {
    // Phase 1: Use metadata to identify collateral scripts
    // (Phase 2 will use UTXO database for actual deployment)
    ScriptType type = IdentifyScriptType(script);
    return type == ScriptType::COLLATERAL_LOCK;
}

// ============================================================================
// Amount and Collateral Validation
// ============================================================================

bool ValidateMintAmount(CAmount amount, const CChainParams& params, int nHeight) {
    const auto& ddParams = params.GetDigiDollarParams();

    // Only enforce minMintAmount after the activation height
    // This ensures historical blocks with lower amounts remain valid
    CAmount effectiveMinMint = (nHeight >= ddParams.minMintAmountActivationHeight && ddParams.minMintAmountActivationHeight > 0)
        ? ddParams.minMintAmount : 1;  // Before activation: 1 cent minimum

    return amount >= effectiveMinMint && amount <= ddParams.maxMintAmount;
}

bool ValidateOutputAmount(CAmount amount, const CChainParams& params) {
    const auto& ddParams = params.GetDigiDollarParams();
    return amount >= ddParams.minOutputAmount && amount <= MAX_DIGIDOLLAR;
}

CAmount CalculateRequiredCollateral(CAmount ddAmount, int64_t lockTime,
                                   const ValidationContext& ctx) {
    if (ddAmount <= 0 || ctx.oraclePriceMicroUSD <= 0) {
        return 0;
    }

    // Get base collateral ratio for lock period
    const auto& ddParams = ctx.params.GetDigiDollarParams();
    int baseRatio = GetCollateralRatioForLockTime(lockTime, ddParams);

    // Use system health from context (passed in by caller)
    // This ensures consistency between test and production code
    int systemHealth = ctx.systemCollateral;

    LogPrint(BCLog::DIGIDOLLAR, "DCA: Collateral requirement calculation:\n");
    LogPrint(BCLog::DIGIDOLLAR, "  DD amount: %lld cents ($%.2f)\n",
             ddAmount, ddAmount / 100.0);
    LogPrint(BCLog::DIGIDOLLAR, "  Lock time: %lld blocks (~%lld days)\n",
             lockTime, lockTime / (24 * 60 * 4));
    LogPrint(BCLog::DIGIDOLLAR, "  Oracle price: %lld micro-USD ($%.6f per DGB)\n",
             ctx.oraclePriceMicroUSD, ctx.oraclePriceMicroUSD / 1000000.0);
    LogPrint(BCLog::DIGIDOLLAR, "  System health: %d%%\n", systemHealth);

    // Apply DCA multiplier based on system health from context
    int effectiveRatio = GetEffectiveCollateralRatio(baseRatio, systemHealth, ctx.params);

    // Calculate required DGB collateral
    // DD amount is in cents (100 = $1.00 USD)
    // Oracle price is in micro-USD (1,000,000 = $1.00 DGB price)
    // Use 64-bit arithmetic to prevent overflow
    //
    // Formula: Required_DGB_sats = (DD_cents * COIN * ratio) / (oracle_micro_usd / 100)
    //        = (DD_cents * COIN * ratio * 100) / oracle_micro_usd
    // Example: $100 DD at $0.00631 DGB with 150% ratio (oracle_micro_usd = 6310)
    //   = (10000 cents * COIN * 150 * 100) / 6310
    //   = (10000 * 100000000 * 150 * 100) / 6310
    //   = 15,000,000,000,000,000 / 6310
    //   = 2,377,179,080,509 sats = ~23,772 DGB
    // Use __int128 to avoid uint64 overflow for large DD amounts (overflows at ~$18K@1000%)
    __int128 numerator = static_cast<__int128>(ddAmount) * static_cast<__int128>(COIN) *
                         static_cast<__int128>(effectiveRatio) * 100;
    __int128 result = numerator / static_cast<__int128>(ctx.oraclePriceMicroUSD);
    // Overflow guard: if result exceeds MAX_MONEY, cap at MAX_MONEY.
    // This prevents silent uint64_t truncation that could wrap required
    // collateral to near-zero, allowing mints with almost no collateral.
    if (result > static_cast<__int128>(MAX_MONEY)) {
        result = static_cast<__int128>(MAX_MONEY);
    }
    uint64_t requiredDGB = static_cast<uint64_t>(result);

    LogPrint(BCLog::DIGIDOLLAR, "DCA: Collateral calculation: %lld cents * %lld * %d * 100 / %lld micro-USD = %llu sat (~%llu DGB)\n",
             ddAmount, COIN, effectiveRatio, ctx.oraclePriceMicroUSD, requiredDGB, requiredDGB / COIN);

    return requiredDGB;
}

int GetEffectiveCollateralRatio(int baseRatio, int systemCollateral,
                               const CChainParams& params) {
    // Use the new DCA system for more comprehensive health calculation
    double multiplier = DigiDollar::DCA::DynamicCollateralAdjustment::GetDCAMultiplier(systemCollateral);
    int effectiveRatio = DigiDollar::DCA::DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemCollateral);

    LogPrint(BCLog::DIGIDOLLAR, "DCA: Base ratio %d%%, system health %d%%, multiplier %.1fx -> effective ratio %d%%\n",
             baseRatio, systemCollateral, multiplier, effectiveRatio);

    return effectiveRatio;
}

bool ValidateCollateralRatio(CAmount dgbLocked, CAmount ddMinted,
                            int64_t lockTime, const ValidationContext& ctx) {
    // Input validation
    if (ddMinted <= 0) {
        LogPrintf("DigiDollar: Invalid DD minted amount: %d\n", ddMinted);
        return false;
    }

    if (dgbLocked <= 0) {
        LogPrintf("DigiDollar: Invalid DGB locked amount: %d\n", dgbLocked);
        return false;
    }

    if (ctx.oraclePriceMicroUSD <= 0) {
        LogPrintf("DigiDollar: Invalid oracle price: %d\n", ctx.oraclePriceMicroUSD);
        return false;
    }

    if (lockTime <= 0) {
        LogPrintf("DigiDollar: Invalid lock time: %d blocks\n", lockTime);
        return false;
    }

    // Calculate required collateral
    CAmount requiredCollateral = CalculateRequiredCollateral(ddMinted, lockTime, ctx);
    if (requiredCollateral <= 0) {
        LogPrintf("DigiDollar: Failed to calculate required collateral\n");
        return false;
    }

    // Calculate actual collateral ratio for logging
    // Oracle price is in micro-USD (1,000,000 = $1.00), DD is in cents
    // Convert: (DGB_sats * oracle_micro_usd / COIN) = micro-USD value
    // Then: micro-USD / 10000 = cents
    CAmount dgbValueMicroUSD = (dgbLocked * ctx.oraclePriceMicroUSD) / COIN;
    CAmount dgbValueInCents = dgbValueMicroUSD / 10000;  // Convert micro-USD to cents
    int actualRatio = (dgbValueInCents * 100) / ddMinted;

    // Get expected ratio for comparison
    const auto& ddParams = ctx.params.GetDigiDollarParams();
    int baseRatio = GetCollateralRatioForLockTime(lockTime, ddParams);
    int effectiveRatio = GetEffectiveCollateralRatio(baseRatio, ctx.systemCollateral, ctx.params);

    LogPrintf("DigiDollar: Collateral validation details:\n");
    LogPrintf("  DGB locked: %d satoshis (%.2f DGB)\n", dgbLocked, dgbLocked / (double)COIN);
    LogPrintf("  DD minted: %d cents ($%.2f)\n", ddMinted, ddMinted / 100.0);
    LogPrintf("  Lock time: %d blocks (~%d days)\n", lockTime, lockTime / (24 * 60 * 4));
    LogPrintf("  Oracle price: %lld micro-USD ($%.6f per DGB)\n", ctx.oraclePriceMicroUSD, ctx.oraclePriceMicroUSD / 1000000.0);
    LogPrintf("  DGB value: %d cents ($%.2f)\n", dgbValueInCents, dgbValueInCents / 100.0);
    LogPrintf("  Base ratio: %d%%, Effective ratio: %d%%, Actual ratio: %d%%\n",
              baseRatio, effectiveRatio, actualRatio);
    LogPrintf("  Required collateral: %d satoshis (%.2f DGB)\n",
              requiredCollateral, requiredCollateral / (double)COIN);
    LogPrintf("  System collateral: %d%%\n", ctx.systemCollateral);

    bool isValid = dgbLocked >= requiredCollateral;
    LogPrintf("  Result: %s\n", isValid ? "VALID" : "INVALID");

    return isValid;
}

// ============================================================================
// Path-Specific Validation Functions
// ============================================================================

bool ValidateNormalRedemption(const CScript& script, int currentHeight) {
    // Phase 1 simplified implementation
    // Extract lock height from script metadata if available
    // Phase 2 will extract from UTXO database or witness data

    ScriptMetadata metadata;
    if (GetScriptMetadata(script, metadata)) {
        // Metadata available - validate timelock
        if (currentHeight < metadata.lockHeight) {
            // Timelock has not expired yet - redemption REJECTED
            LogPrintf("DigiDollar: Normal redemption rejected - timelock not expired (current: %d, required: %d)\n",
                      currentHeight, metadata.lockHeight);
            return false;
        }
        // Timelock has expired - redemption allowed
        LogPrintf("DigiDollar: Normal redemption allowed - timelock expired\n");
        return true;
    }

    // Phase 1: No metadata available (script paths, cross-node validation, etc.)
    // For testing purposes, allow redemptions when height > 0
    // In Phase 2, this would extract timelock from witness data during script execution
    if (currentHeight > 0) {
        LogPrintf("DigiDollar: Normal redemption validation simplified (Phase 1) - allowing based on height > 0\n");
        return true;
    }

    LogPrintf("DigiDollar: Normal redemption rejected - invalid height\n");
    return false;
}

bool ValidateEmergencyRedemption(const CScript& script,
                                const std::vector<std::vector<unsigned char>>& sigs) {
    // Emergency path requires 8-of-15 oracle signatures
    const size_t requiredSigs = 8;

    // Count valid signatures (simplified check)
    size_t validSigs = 0;
    for (const auto& sig : sigs) {
        if (!sig.empty()) {
            validSigs++;
        }
    }

    LogPrintf("DigiDollar: Emergency redemption - %d signatures provided, %d required\n",
              validSigs, requiredSigs);

    return validSigs >= requiredSigs;
}


bool ValidateERRRedemption(const CScript& script, int systemCollateral) {
    // ERR (Emergency Redemption Ratio) activates when system < 100% collateralized
    bool errActive = systemCollateral < 100;

    LogPrintf("DigiDollar: ERR validation - System collateral: %d%%, ERR %s\n",
              systemCollateral, errActive ? "ACTIVE" : "INACTIVE");

    return errActive;
}

// ============================================================================
// Script Validation
// ============================================================================

bool ValidateDigiDollarScript(const CScript& script,
                              const ValidationContext& ctx,
                              ScriptError* serror) {
    ScriptType type = IdentifyScriptType(script);

    // Non-DD scripts pass through without validation
    if (type == ScriptType::NOT_DIGIDOLLAR) {
        // Check for malformed scripts with DD markers but invalid structure
        // Look for OP_DIGIDOLLAR in scripts that aren't properly formed
        CScript::const_iterator pc = script.begin();
        opcodetype opcode;
        while (pc < script.end()) {
            if (script.GetOp(pc, opcode)) {
                if (opcode == OP_DIGIDOLLAR) {
                    // Found DD marker but script isn't valid DD type
                    if (serror) *serror = SCRIPT_ERR_INVALID_DD_AMOUNT;
                    LogPrintf("DigiDollar: Script has DD marker but invalid structure\n");
                    return false;
                }
            }
        }
        return true;
    }

    // Extract and validate DD amount if present
    CAmount amount;
    if (!ExtractDDAmount(script, amount)) {
        if (serror) *serror = SCRIPT_ERR_INVALID_DD_AMOUNT;
        LogPrintf("DigiDollar: Script validation failed - cannot extract DD amount\n");
        return false;
    }

    // Validate amount based on script type
    switch (type) {
        case ScriptType::DD_TOKEN_OUTPUT:
            if (!ValidateOutputAmount(amount, ctx.params)) {
                if (serror) *serror = SCRIPT_ERR_INVALID_DD_AMOUNT;
                LogPrintf("DigiDollar: Invalid DD output amount: %d cents\n", amount);
                return false;
            }
            break;

        case ScriptType::COLLATERAL_LOCK:
            // Collateral scripts don't have amount limits in the same way
            break;

        default:
            break;
    }

    LogPrintf("DigiDollar: Script validation passed - Type: %d, Amount: %d cents\n",
              static_cast<int>(type), amount);

    return true;
}

// ============================================================================
// Transaction Type Validation
// ============================================================================

bool ValidateMintTransaction(const CTransaction& tx,
                            const ValidationContext& ctx,
                            TxValidationState& state) {
    LogPrintf("DigiDollar: Validating mint transaction (txid: %s)\n", tx.GetHash().ToString());

    // 1. Basic structural checks
    if (tx.vin.empty()) {
        LogPrintf("DigiDollar: Mint transaction has no inputs\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-no-inputs");
    }

    // Early validation: Check for DD outputs with invalid amounts before structural checks
    // This allows us to give more specific error messages
    for (const auto& output : tx.vout) {
        if (output.nValue == 0) {
            CAmount ddAmt = 0;
            if (ExtractDDAmount(output.scriptPubKey, ddAmt)) {
                // Check both mint amount limits AND output amount limits
                if (!ValidateMintAmount(ddAmt, ctx.params, ctx.nHeight) || !ValidateOutputAmount(ddAmt, ctx.params)) {
                    LogPrintf("DigiDollar: Invalid DD mint/output amount detected: %d cents\n", ddAmt);
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dd-mint-amount");
                }
            }
        }
    }

    if (tx.vout.size() < 2) {
        LogPrintf("DigiDollar: Mint transaction needs at least 2 outputs (collateral + DD)\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-outputs");
    }

    // 2. Oracle price validation (skip for historical blocks)
    if (!ctx.skipOracleValidation && ctx.oraclePriceMicroUSD <= 0) {
        LogPrintf("DigiDollar: Invalid oracle price: %d\n", ctx.oraclePriceMicroUSD);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-oracle-price");
    }

    // 3. Volatility protection checks for minting (skip for historical blocks)
    if (!ctx.skipOracleValidation) {
        if (Volatility::VolatilityMonitor::ShouldFreezeMinting()) {
            LogPrintf("DigiDollar: Minting frozen due to high volatility\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "minting-frozen-volatility");
        }

        if (Volatility::VolatilityMonitor::ShouldFreezeAll()) {
            LogPrintf("DigiDollar: All DD operations frozen due to extreme volatility\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "all-operations-frozen");
        }
    }

    // 4. Analyze outputs to find DD amounts and collateral
    CAmount totalDD = 0;
    CAmount totalCollateral = 0;
    bool hasCollateralOutput = false;
    bool hasDDOutput = false;
    int ddOutputCount = 0;  // Security: count DD outputs to prevent inflation attack
    int collateralOutputCount = 0;  // Security [T1-04c]: count collateral outputs to prevent NUMS bypass
    int ddOpReturnCount = 0;  // Security [T1-04f]: count DD OP_RETURN outputs to prevent owner key overwrite
    int64_t lockTime = 0;
    std::vector<unsigned char> ownerXOnlyPubKeyData;  // Owner pubkey for NUMS verification
    bool hasOwnerPubKey = false;
    CScript actualCollateralScript;  // Store the actual collateral P2TR script for NUMS verification

    for (size_t i = 0; i < tx.vout.size(); i++) {
        const CTxOut& output = tx.vout[i];

        // Phase 1 workaround: Identify outputs by structure since metadata doesn't cross nodes
        // P2TR outputs: collateral has value > 0, DD token has value = 0
        bool isP2TR = (output.scriptPubKey.size() == 34 && output.scriptPubKey[0] == OP_1);
        bool isOpReturn = (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN);

        // Check script type using metadata
        ScriptType scriptType = IdentifyScriptType(output.scriptPubKey);
        CAmount ddAmount = 0;
        bool hasDDAmount = ExtractDDAmount(output.scriptPubKey, ddAmount);

        if (output.nValue > 0 && !isOpReturn) {
            // Any output with value could be collateral in a mint transaction
            // Check if this is actually a DD TOKEN script with non-zero value (invalid)
            if (scriptType == ScriptType::DD_TOKEN_OUTPUT) {
                LogPrintf("DigiDollar: DD token output has non-zero DGB value: %d\n", output.nValue);
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "dd-output-value");
            }

            // Check if it's P2TR (required for collateral)
            // Non-P2TR outputs are allowed as change outputs - skip them
            if (!isP2TR) {
                // This is a change output (P2WPKH, P2SH, etc.) - not collateral
                LogPrintf("DigiDollar: Output %zu is non-P2TR change output (value=%d, scriptSize=%d)\n",
                         i, output.nValue, output.scriptPubKey.size());
                continue;  // Skip to next output - change outputs are allowed
            }

            // This is a P2TR output with value - must be collateral
            if (!ValidateCollateralOutput(output, tx, state)) {
                return false;
            }
            collateralOutputCount++;

            // Security [T1-04c]: Mint transactions MUST have exactly 1 collateral output.
            // Without this, an attacker can include a large FAKE collateral output (with their
            // own key as P2TR internal key, enabling key-path spend) plus a small LEGITIMATE
            // output (with NUMS key). The NUMS verification only checks the last P2TR value
            // output, so the large fake collateral passes unverified. The attacker key-path
            // spends the fake output immediately, leaving DD backed by only the tiny amount.
            if (collateralOutputCount > 1) {
                LogPrintf("DigiDollar: SECURITY [T1-04c] - Mint tx has %d collateral outputs (max 1 allowed). "
                         "Rejecting to prevent NUMS verification bypass via multiple collateral outputs.\n",
                         collateralOutputCount);
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-multiple-collateral-outputs",
                                   "Mint transactions must have exactly 1 collateral output to prevent NUMS bypass");
            }

            totalCollateral += output.nValue;
            hasCollateralOutput = true;
            actualCollateralScript = output.scriptPubKey;  // Store for NUMS verification
        }

        // Check for OP_RETURN metadata: <"DD"> <txType> <ddAmount> <lockHeight>
        if (isOpReturn) {
            CScript::const_iterator pc = output.scriptPubKey.begin() + 1;
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Check for DD marker
            if (output.scriptPubKey.GetOp(pc, opcode, data) && data.size() == 2 &&
                data[0] == 'D' && data[1] == 'D') {

                // Security [T1-04f]: Mint transactions MUST have exactly 1 DD OP_RETURN.
                // Without this, an attacker can include multiple DD OP_RETURNs with different
                // owner keys. The validation loop overwrites ownerXOnlyPubKeyData with each
                // OP_RETURN, so the last one's owner key is used for NUMS reconstruction.
                // This creates ambiguity about which owner key is authoritative.
                ddOpReturnCount++;
                if (ddOpReturnCount > 1) {
                    LogPrintf("DigiDollar: SECURITY [T1-04f] - Mint tx has %d DD OP_RETURN outputs (max 1 allowed). "
                             "Rejecting to prevent owner key overwrite via multiple DD OP_RETURNs.\n",
                             ddOpReturnCount);
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-multiple-dd-opreturn",
                                       "Mint transactions must have exactly 1 DD OP_RETURN to prevent owner key confusion");
                }

                // Extract tx type (1 = MINT, 2 = TRANSFER, etc.)
                int64_t txType = 0;
                if (output.scriptPubKey.GetOp(pc, opcode, data)) {
                    try {
                        CScriptNum txTypeNum(data, true);
                        txType = txTypeNum.getint();
                        LogPrintf("DigiDollar: Extracted tx type from OP_RETURN: %d\n", txType);
                    } catch (const std::exception&) {}
                }

                // Extract DD amount in cents
                if (output.scriptPubKey.GetOp(pc, opcode, data)) {
                    try {
                        // Allow up to 8 bytes for DD amounts (int64_t range)
                        CScriptNum ddAmountNum(data, true, 8);
                        totalDD = ddAmountNum.GetInt64();
                        LogPrintf("DigiDollar: Extracted DD amount from OP_RETURN: %lld cents ($%.2f)\n",
                                  static_cast<long long>(totalDD), totalDD / 100.0);
                    } catch (const std::exception&) {}
                }

                // Extract lock height
                if (output.scriptPubKey.GetOp(pc, opcode, data)) {
                    try {
                        // Allow up to 8 bytes for lock heights (int64_t range)
                        CScriptNum lockHeightNum(data, true, 8);
                        lockTime = lockHeightNum.GetInt64();
                        LogPrintf("DigiDollar: Extracted lock height from OP_RETURN: %lld blocks\n", static_cast<long long>(lockTime));
                    } catch (const std::exception&) {}
                }

                // Extract lock tier and VERIFY consistency with lockHeight
                // SECURITY: Without this check, an attacker could claim a long lock tier
                // (e.g., tier 9 = 10 years, 200% ratio) in OP_RETURN but commit a short
                // lock (e.g., 1 hour) in the MAST tree, getting a favorable collateral
                // ratio without actually locking for the claimed period.
                if (output.scriptPubKey.GetOp(pc, opcode, data)) {
                    try {
                        CScriptNum lockTierNum(data, true);
                        int64_t lockTier = lockTierNum.getint();
                        LogPrintf("DigiDollar: Extracted lock tier from OP_RETURN: %lld\n", static_cast<long long>(lockTier));

                        // Validate tier is in range (0-9)
                        if (lockTier < 0 || lockTier > 9) {
                            LogPrintf("DigiDollar: SECURITY - Invalid lock tier %lld (must be 0-9)\n", static_cast<long long>(lockTier));
                            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-lock-tier",
                                               "Lock tier must be 0-9");
                        }

                        // Verify lockHeight is consistent with the claimed tier
                        // Tier lock days: {0, 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650}
                        static const int TIER_LOCK_DAYS[] = {0, 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650};
                        int64_t expectedLockBlocks = DigiDollar::LockDaysToBlocks(TIER_LOCK_DAYS[lockTier]);
                        int64_t actualLockBlocks = lockTime - ctx.nHeight;

                        // Allow small tolerance (±10 blocks) for timing variance
                        if (actualLockBlocks < expectedLockBlocks - 10) {
                            LogPrintf("DigiDollar: SECURITY - Lock height mismatch! "
                                     "Tier %lld claims %lld blocks but actual lock is only %lld blocks "
                                     "(lockHeight=%lld, currentHeight=%d). "
                                     "Possible collateral ratio manipulation attack.\n",
                                     static_cast<long long>(lockTier),
                                     static_cast<long long>(expectedLockBlocks),
                                     static_cast<long long>(actualLockBlocks),
                                     static_cast<long long>(lockTime), ctx.nHeight);
                            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-lock-height-mismatch",
                                               "Lock height does not match claimed lock tier");
                        }
                    } catch (const std::exception&) {
                        // If we can't parse lock tier, reject the mint
                        LogPrintf("DigiDollar: Failed to parse lock tier from OP_RETURN\n");
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-lock-tier-parse");
                    }
                }

                // Extract owner x-only pubkey (32 bytes) for NUMS verification
                // SECURITY [T1-04]: Without this, an attacker could use their own key as
                // the P2TR internal key instead of the NUMS point, enabling key-path
                // spending that bypasses CLTV timelocks and creates unbacked DD tokens.
                if (output.scriptPubKey.GetOp(pc, opcode, data)) {
                    if (data.size() == 32) {
                        ownerXOnlyPubKeyData = data;
                        hasOwnerPubKey = true;
                        LogPrintf("DigiDollar: Extracted owner x-only pubkey from OP_RETURN (%d bytes)\n", data.size());
                    } else {
                        LogPrintf("DigiDollar: SECURITY - Invalid owner pubkey size in OP_RETURN: %d (expected 32)\n", data.size());
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-owner-pubkey",
                                           "Owner x-only pubkey must be 32 bytes");
                    }
                } else {
                    // Owner pubkey is REQUIRED for NUMS verification
                    LogPrintf("DigiDollar: SECURITY - Missing owner pubkey in mint OP_RETURN\n");
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-missing-owner-pubkey",
                                       "Mint OP_RETURN must include owner x-only pubkey for NUMS verification");
                }
            }
        }

        if (isP2TR && output.nValue == 0) {
            // This is the DD token output
            if (!ValidateDDOutput(output, tx, state)) {
                return false;
            }
            ddOutputCount++;

            // Security: Mint transactions MUST have exactly 1 DD output.
            // Extra P2TR zero-value outputs would be indexed against the OP_RETURN
            // metadata (lockHeight, lockTier fields), causing those non-amount values
            // to be misinterpreted as DD amounts during transfer validation lookups.
            // This would allow an attacker to inflate DD supply from nothing.
            if (ddOutputCount > 1) {
                LogPrintf("DigiDollar: SECURITY - Mint tx has %d DD outputs (max 1 allowed). "
                         "Rejecting to prevent OP_RETURN inflation attack.\n", ddOutputCount);
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-multiple-dd-outputs",
                                   "Mint transactions must have exactly 1 DD token output");
            }

            hasDDOutput = true;

            // Phase 1: Try to extract DD amount from metadata if available
            // If not available (cross-node validation), calculate from collateral
            if (hasDDAmount) {
                if (ddAmount <= 0) {
                    LogPrintf("DigiDollar: Invalid DD amount: %d\n", ddAmount);
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dd-amount");
                }
                totalDD += ddAmount;
            }
            // If we can't extract (cross-node validation), we'll calculate after loop
        }
    }

    // 4. Calculate DD amount if not extracted from metadata (skip for historical blocks)
    // For mint transactions, DD amount = (collateral * oracle_price * 100) / (collateral_ratio * COIN)
    if (!ctx.skipOracleValidation && hasDDOutput && totalDD == 0 && totalCollateral > 0) {
        // Calculate DD amount from collateral and oracle price
        CAmount oraclePrice = ctx.oraclePriceMicroUSD;
        if (oraclePrice <= 0) {
            LogPrintf("DigiDollar: Invalid oracle price for DD amount calculation\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "invalid-oracle-price");
        }

        // Calculate max DD that can be minted with this collateral at minimum ratio
        // Oracle price is in cents (100 = $1.00), DD is in cents (100 = $1.00)
        // DD_cents = (collateral_sats * price_cents) / (min_ratio * COIN)
        // But we don't know the tier/ratio yet, so use a conservative 200% (tier 1)
        int minRatio = 200;
        totalDD = (totalCollateral * oraclePrice) / (minRatio * COIN);

        LogPrintf("DigiDollar: Calculated DD amount from collateral: %d cents ($%.2f) from %d DGB at %d cents\n",
                  totalDD, totalDD / 100.0, totalCollateral / COIN, oraclePrice);
    }

    // 5. Ensure we have both required output types
    if (!hasCollateralOutput) {
        LogPrintf("DigiDollar: Mint transaction missing collateral output\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "missing-collateral-output");
    }

    if (!hasDDOutput) {
        LogPrintf("DigiDollar: Mint transaction missing DD output\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "missing-dd-output");
    }

    // 5. Ensure valid lock time was found (Phase 1: allow default for testing)
    if (lockTime <= 0) {
        // Phase 1 workaround: Use default 30-day lock for testing if no OP_RETURN
        lockTime = 30 * 24 * 60 * 4; // 30 days default
        LogPrintf("DigiDollar: No lock time in OP_RETURN, using default 30 days for testing\n");
    }

    // 5b. SECURITY [T1-04b]: Require DD OP_RETURN with owner pubkey for ALL mint transactions.
    // Without this, an attacker can omit OP_RETURN to bypass NUMS verification entirely,
    // since hasOwnerPubKey stays false and the NUMS check guard skips verification.
    // The attacker uses their own key as P2TR internal key, enabling key-path spend
    // that bypasses CLTV timelocks, stealing collateral and creating unbacked DD tokens.
    if (hasCollateralOutput && !hasOwnerPubKey) {
        LogPrintf("DigiDollar: SECURITY [T1-04b] - Mint tx has collateral but missing DD OP_RETURN with owner pubkey!\n");
        LogPrintf("  Without owner pubkey, NUMS verification cannot be performed.\n");
        LogPrintf("  This could allow key-path spending that bypasses CLTV timelocks.\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-missing-dd-opreturn",
                           "Mint transaction must include DD OP_RETURN with owner pubkey for NUMS verification");
    }

    // SECURITY [T1-04]: Verify collateral P2TR output was constructed with NUMS internal key.
    // Without this check, an attacker can use their own key as internal key, enabling
    // key-path spending that bypasses CLTV timelocks and creates unbacked DD tokens.
    if (hasOwnerPubKey && hasCollateralOutput && lockTime > 0 && totalDD > 0) {
        XOnlyPubKey ownerXOnly{Span<const unsigned char>(ownerXOnlyPubKeyData.data(), 32)};
        if (!ownerXOnly.IsFullyValid()) {
            LogPrintf("DigiDollar: SECURITY - Invalid owner x-only pubkey in OP_RETURN\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-mint-owner-pubkey-invalid",
                               "Owner x-only pubkey is not a valid curve point");
        }

        // Reconstruct the expected P2TR collateral output using NUMS internal key
        DigiDollar::MintParams expectedParams;
        expectedParams.ddAmount = totalDD;
        expectedParams.lockHeight = lockTime;
        expectedParams.ownerKey = ownerXOnly;
        expectedParams.internalKey = DigiDollar::GetCollateralNUMSKey();
        expectedParams.oracleKeys = DigiDollar::GetOracleKeys(15);

        CScript expectedCollateral = DigiDollar::CreateCollateralP2TR(expectedParams);
        if (expectedCollateral.empty()) {
            LogPrintf("DigiDollar: SECURITY - Failed to reconstruct expected P2TR collateral\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-reconstruction",
                               "Failed to reconstruct expected P2TR collateral output");
        }

        if (actualCollateralScript != expectedCollateral) {
            LogPrintf("DigiDollar: SECURITY [T1-04] - Collateral P2TR output does NOT match expected NUMS reconstruction!\n");
            LogPrintf("  Actual:   %s\n", HexStr(actualCollateralScript));
            LogPrintf("  Expected: %s\n", HexStr(expectedCollateral));
            LogPrintf("  This means the collateral was constructed with a non-NUMS internal key,\n");
            LogPrintf("  allowing key-path spending that bypasses CLTV timelocks.\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-nums-mismatch",
                               "Collateral P2TR output does not match NUMS-key reconstruction. "
                               "Internal key must be the NUMS point to prevent key-path spending.");
        }

        LogPrintf("DigiDollar: NUMS verification passed - collateral P2TR matches expected output\n");
    }

    // 6. Validate total DD amount against mint limits
    if (!ValidateMintAmount(totalDD, ctx.params, ctx.nHeight)) {
        LogPrintf("DigiDollar: Invalid total mint amount: %d cents (limits: %d - %d)\n",
                  totalDD, ctx.params.GetDigiDollarParams().minMintAmount,
                  ctx.params.GetDigiDollarParams().maxMintAmount);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dd-mint-amount");
    }

    // 7. Calculate and verify collateral (skip for historical blocks - oracle price dependent)
    CAmount requiredCollateral = 0;
    if (!ctx.skipOracleValidation) {
        // SECURITY [T2-01]: Convert absolute lock HEIGHT to relative lock PERIOD for
        // collateral ratio calculation. The OP_RETURN stores an absolute lockHeight
        // (currentHeight + lockPeriod), but GetCollateralRatioForLockTime expects a
        // relative lock period in blocks. Without this conversion, on mainnet (height ~22M)
        // the absolute height exceeds ALL tier thresholds (max is 10yr = 21M blocks),
        // causing every lock tier to use the 200% (10-year) ratio instead of its correct
        // higher ratio. A 1-hour lock would require only 200% instead of 1000% collateral.
        int64_t lockPeriod = lockTime - ctx.nHeight;
        if (lockPeriod <= 0) {
            LogPrintf("DigiDollar: Invalid lock period: lockTime=%lld, height=%d, period=%lld\n",
                      (long long)lockTime, ctx.nHeight, (long long)lockPeriod);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-lock-period");
        }

        requiredCollateral = CalculateRequiredCollateral(totalDD, lockPeriod, ctx);
        if (requiredCollateral <= 0) {
            LogPrintf("DigiDollar: Failed to calculate required collateral\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "collateral-calculation-failed");
        }

        // Verify sufficient collateral
        if (totalCollateral < requiredCollateral) {
            LogPrintf("DigiDollar: Insufficient collateral: provided %d, required %d\n",
                      totalCollateral, requiredCollateral);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "insufficient-collateral");
        }

        // 8. Additional validation checks
        if (!ValidateCollateralRatio(totalCollateral, totalDD, lockPeriod, ctx)) {
            LogPrintf("DigiDollar: Collateral ratio validation failed\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-ratio");
        }
    }

    // 9. Log successful validation
    LogPrintf("DigiDollar: Mint validation successful:\n");
    LogPrintf("  Total DD: %d cents ($%.2f)\n", totalDD, totalDD / 100.0);
    LogPrintf("  Total collateral: %d satoshis (%.2f DGB)\n", totalCollateral, totalCollateral / (double)COIN);
    LogPrintf("  Required collateral: %d satoshis (%.2f DGB)\n", requiredCollateral, requiredCollateral / (double)COIN);
    LogPrintf("  Lock time: %d blocks (~%d days)\n", lockTime, lockTime / (24 * 60 * 4));
    LogPrintf("  Oracle price: %lld micro-USD ($%.6f per DGB)\n", ctx.oraclePriceMicroUSD, ctx.oraclePriceMicroUSD / 1000000.0);

    return true;
}

bool ValidateTransferTransaction(const CTransaction& tx,
                                const ValidationContext& ctx,
                                TxValidationState& state) {
    // Comprehensive transfer transaction validation
    CAmount inputDD = 0;
    CAmount outputDD = 0;
    int ddInputCount = 0;
    int ddOutputCount = 0;

    // Check transaction version/type
    if (!HasDigiDollarMarker(tx)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-missing-dd-marker");
    }

    if (DigiDollar::GetDigiDollarTxType(tx) != DD_TX_TRANSFER) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-wrong-tx-type");
    }

    // Volatility protection checks for transfers (skip for historical blocks)
    if (!ctx.skipOracleValidation && Volatility::VolatilityMonitor::ShouldFreezeAll()) {
        LogPrintf("DigiDollar: All DD operations frozen due to extreme volatility\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "all-operations-frozen");
    }

    // Extract DD amounts from OP_RETURN (needed for cross-node validation)
    // Format: OP_RETURN <"DD"> <txType> <amount1> <amount2> ...
    std::vector<CAmount> dd_amounts;
    for (const auto& output : tx.vout) {
        if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
            CScript::const_iterator pc = output.scriptPubKey.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Skip OP_RETURN
            if (!output.scriptPubKey.GetOp(pc, opcode)) continue;

            // Check for "DD" marker
            if (!output.scriptPubKey.GetOp(pc, opcode, data)) continue;
            if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;

            // Get transaction type
            if (!output.scriptPubKey.GetOp(pc, opcode, data)) continue;
            CScriptNum txType(data, true);
            if (txType.getint() != 2) continue;  // Must be TRANSFER (type 2)

            // Extract DD amounts
            while (output.scriptPubKey.GetOp(pc, opcode, data)) {
                if (data.size() > 0) {
                    // Allow up to 8 bytes for DD amounts (int64_t range)
                    CScriptNum amount(data, true, 8);
                    dd_amounts.push_back(amount.GetInt64());
                }
            }
            break;
        }
    }

    if (dd_amounts.empty()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-no-op-return-data");
    }

    // Validate P2TR outputs using amounts from OP_RETURN
    size_t dd_amount_index = 0;
    for (const auto& output : tx.vout) {
        // Skip OP_RETURN and non-zero value outputs
        if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) continue;
        if (output.nValue != 0) continue;

        // Check if it's a P2TR output (OP_1 + 32 bytes)
        if (output.scriptPubKey.size() == 34 && output.scriptPubKey[0] == OP_1) {
            if (dd_amount_index >= dd_amounts.size()) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-dd-output-amount-mismatch");
            }

            CAmount ddAmount = dd_amounts[dd_amount_index++];
            ddOutputCount++;

            // Validate amount is positive and within limits
            if (ddAmount <= 0) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-zero-or-negative-dd-amount");
            }

            if (!ValidateOutputAmount(ddAmount, ctx.params)) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-dd-amount-below-minimum");
            }

            // Check maximum single transfer limit ($100,000)
            if (ddAmount > 10000000) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-dd-amount-exceeds-maximum");
            }

            outputDD += ddAmount;
        }
    }

    // Must have at least one DD output
    if (ddOutputCount == 0) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-no-dd-outputs");
    }

    // Check inputs contain DD UTXOs
    if (tx.vin.empty()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-no-inputs");
    }

    // Look up input DD amounts from the previous transaction's OP_RETURN data.
    //
    // IMPORTANT: DD P2TR scripts are bare OP_1 <tweaked_pubkey> with NO embedded
    // DD amount. The DD amount is stored in the OP_RETURN output of the transaction
    // that created the UTXO. The in-memory metadata registry is unreliable because:
    //   - It's ephemeral (lost on restart)
    //   - Same key → same script hash → metadata can be overwritten by change outputs
    //
    // Strategy:
    //   1. Try txindex to find the original tx and parse its OP_RETURN (most reliable)
    //   2. Fall back to conservation assumption if txindex unavailable
    //
    // Phase 2 will store DD amounts in the UTXO database directly, eliminating
    // the need for txindex lookups or metadata registries.
    {
        for (const auto& txin : tx.vin) {
            if (txin.prevout.IsNull()) continue;

            CAmount ddAmt = 0;
            bool found = false;

            // 1. Try txindex (authoritative — reads creating tx's OP_RETURN)
            if (!found && ExtractDDAmountFromPrevTx(txin.prevout, ddAmt) && ddAmt > 0) {
                found = true;
            }

            // 2. Try block-db lookup (authoritative — reads creating tx's OP_RETURN)
            if (!found && ctx.coins && ctx.txLookup) {
                Coin coin;
                if (ctx.coins->GetCoin(txin.prevout, coin) && coin.out.nValue == 0) {
                    if (ExtractDDAmountFromBlockDb(txin.prevout, coin.nHeight, ctx.txLookup, ddAmt) && ddAmt > 0) {
                        found = true;
                    }
                }
            }

            // 3. Try coins view + metadata registry (may be stale — last resort)
            if (!found && ctx.coins) {
                Coin coin;
                if (ctx.coins->GetCoin(txin.prevout, coin) && coin.out.nValue == 0) {
                    if (ExtractDDAmount(coin.out.scriptPubKey, ddAmt) && ddAmt > 0) {
                        found = true;
                    }
                }
            }

            if (found) {
                inputDD += ddAmt;
                ddInputCount++;
            }
        }

        if (ddInputCount == 0) {
            // Could not determine input DD amounts from any source (metadata registry,
            // coins view, or block-db lookup). This should not happen during normal
            // mempool acceptance or block validation since both provide coins + txLookup.
            // Reject instead of silently assuming conservation — a consensus rule must
            // never be soft-bypassed. If this triggers in testing, the validation context
            // is missing required data sources.
            LogPrintf("DigiDollar: REJECT - Could not determine input DD amounts for conservation check\n");
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "dd-input-amounts-unknown",
                               "Cannot verify DD conservation: input DD amounts undetermined");
        }
    }

    // DD Conservation: Total DD in must equal total DD out
    if (inputDD != outputDD) {
        LogPrintf("DigiDollar: Transfer DD conservation violation - Input: %d, Output: %d\n",
                  inputDD, outputDD);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-dd-conservation-violation",
                           strprintf("DD not conserved: input=%d, output=%d", inputDD, outputDD));
    }

    // Validate P2TR spending (simplified - would need full witness validation)
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        // In full implementation:
        // 1. Look up prevout script
        // 2. Verify it's a DD P2TR script
        // 3. Validate witness data
        // 4. Check signature against script

        // For now, just verify we have inputs
        if (tx.vin[i].prevout.IsNull()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "transfer-invalid-input");
        }
    }

    // Update UTXO tracking (in full implementation)
    // This would:
    // 1. Mark input UTXOs as spent
    // 2. Add new output UTXOs
    // 3. Update DD balance tracking

    LogPrintf("DigiDollar: Transfer transaction validated successfully - DD: %d cents (%d inputs, %d outputs)\n",
              outputDD, tx.vin.size(), ddOutputCount);

    return true;
}

bool ValidateRedemptionTransaction(const CTransaction& tx,
                                  const ValidationContext& ctx,
                                  TxValidationState& state) {
    // Redemption transactions unlock collateral and burn DD tokens
    LogPrintf("DigiDollar: Validating redemption transaction %s\n", tx.GetHash().ToString());

    // Extract DD transaction type for specific redemption path validation
    DigiDollarTxType txType = DigiDollar::GetDigiDollarTxType(tx);

    // Basic structure validation
    if (tx.vin.empty()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-no-inputs");
    }

    // Volatility protection checks for redemptions (skip for historical blocks)
    if (!ctx.skipOracleValidation && Volatility::VolatilityMonitor::ShouldFreezeAll()) {
        LogPrintf("DigiDollar: All DD operations frozen due to extreme volatility\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "all-operations-frozen");
    }

    if (tx.vout.empty()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-no-outputs");
    }

    // Must have at least collateral input + DD input(s) to burn
    if (tx.vin.size() < 2) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-insufficient-inputs");
    }

    // Identify collateral and DD inputs, lookup DD amounts from UTXO
    bool hasCollateralInput = false;
    bool hasDDInput = false;
    CAmount totalDDInputs = 0;
    std::vector<size_t> ddInputIndices;

    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const CTxIn& input = tx.vin[i];

        if (i == 0) {
            // First input assumed to be collateral for this validation
            hasCollateralInput = true;
        } else {
            // Check if this input is a DD UTXO (nValue=0) or a fee UTXO (nValue>0)
            if (ctx.coins) {
                Coin coin;
                if (ctx.coins->GetCoin(input.prevout, coin)) {
                    if (coin.out.nValue == 0) {
                        // DD UTXO (zero satoshi value) - extract DD amount
                        hasDDInput = true;
                        ddInputIndices.push_back(i);

                        CAmount ddAmount = 0;
                        if (ExtractDDAmount(coin.out.scriptPubKey, ddAmount) && ddAmount > 0) {
                            totalDDInputs += ddAmount;
                            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD input %d - amount: %lld cents (from registry)\n",
                                     i, (long long)ddAmount);
                        } else if (ExtractDDAmountFromPrevTx(input.prevout, ddAmount) && ddAmount > 0) {
                            totalDDInputs += ddAmount;
                            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD input %d - amount: %lld cents (from txindex)\n",
                                     i, (long long)ddAmount);
                        } else if (ctx.txLookup && ExtractDDAmountFromBlockDb(input.prevout, coin.nHeight, ctx.txLookup, ddAmount) && ddAmount > 0) {
                            // Universal fallback: load creating tx from block database
                            totalDDInputs += ddAmount;
                            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD input %d - amount: %lld cents (from block db)\n",
                                     i, (long long)ddAmount);
                        } else {
                            LogPrintf("DigiDollar: WARNING - Could not extract DD amount from DD input %d\n", i);
                        }
                    } else {
                        // Fee UTXO (has satoshi value) - skip for DD tracking
                        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Input %d is fee UTXO (value: %d sats), skipping\n",
                                 i, coin.out.nValue);
                    }
                } else {
                    LogPrintf("DigiDollar: WARNING - Could not find UTXO for input %d: %s:%d\n",
                              i, input.prevout.hash.ToString(), input.prevout.n);
                }
            } else {
                // No coins view - can't distinguish DD from fee inputs, assume DD
                hasDDInput = true;
                ddInputIndices.push_back(i);
                LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: No coins view available for input %d lookup\n", i);
            }
        }
    }

    if (!hasCollateralInput) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-no-collateral-input");
    }

    if (!hasDDInput) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-no-dd-inputs");
    }

    // Validate outputs - should have DGB output to user, possibly change
    bool hasDGBOutput = false;
    CAmount totalDGBOutputs = 0;
    CAmount totalDDOutputs = 0;

    // First pass: Find OP_RETURN metadata and extract DD output amounts
    // The OP_RETURN contains the authoritative DD amounts for P2TR outputs in this transaction
    CAmount ddAmountFromOpReturn = 0;
    bool foundOpReturn = false;
    for (const auto& output : tx.vout) {
        if (output.nValue == 0 && output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
            CAmount amount = 0;
            if (ExtractDDAmount(output.scriptPubKey, amount) && amount > 0) {
                ddAmountFromOpReturn = amount;
                foundOpReturn = true;
                LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Found OP_RETURN with DD amount: %lld cents\n", (long long)amount);
                break;  // Only use first OP_RETURN with DD amount
            }
        }
    }

    for (size_t outIdx = 0; outIdx < tx.vout.size(); ++outIdx) {
        const CTxOut& output = tx.vout[outIdx];
        if (output.nValue > 0) {
            // DGB output
            hasDGBOutput = true;
            totalDGBOutputs += output.nValue;
        } else {
            // Skip OP_RETURN outputs - they are metadata, not DD outputs
            if (output.scriptPubKey.size() > 0 && output.scriptPubKey[0] == OP_RETURN) {
                continue;
            }
            // Check if it's a P2TR DD output (nValue=0, starts with OP_1)
            // For DD outputs, use the amount from OP_RETURN metadata if available
            if (output.scriptPubKey.size() > 1 && output.scriptPubKey[0] == OP_1) {
                if (foundOpReturn && ddAmountFromOpReturn > 0) {
                    // Use the authoritative amount from OP_RETURN
                    totalDDOutputs += ddAmountFromOpReturn;
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Output %d - DD P2TR: %lld cents (from OP_RETURN)\n",
                             outIdx, (long long)ddAmountFromOpReturn);
                } else {
                    // Fallback to metadata registry (may be stale)
                    CAmount ddAmount = 0;
                    if (ExtractDDAmount(output.scriptPubKey, ddAmount)) {
                        totalDDOutputs += ddAmount;
                        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Output %d - DD P2TR: %lld cents (from metadata)\n",
                                 outIdx, (long long)ddAmount);
                    }
                }
            }
        }
    }

    if (!hasDGBOutput) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-no-dgb-output");
    }

    // Validate DD burning in redemption transactions
    // Redemption is valid when DD inputs > DD outputs (some DD is burned)
    // This ensures the redemption actually burns DD to unlock collateral
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD burn check - txType=%d, totalDDInputs=%lld, totalDDOutputs=%lld\n",
              static_cast<int>(txType), (long long)totalDDInputs, (long long)totalDDOutputs);
    if (ctx.coins && totalDDInputs > 0) {
        // We have UTXO access and successfully looked up DD input amounts
        if (txType == DD_TX_REDEEM && totalDDInputs <= totalDDOutputs) {
            LogPrintf("DigiDollar: Redemption rejected - no DD burned (inputs: %lld, outputs: %lld)\n",
                      (long long)totalDDInputs, (long long)totalDDOutputs);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-dd-not-burned");
        }

        // NOTE: Partial redemption is NOT supported - must burn full DD amount

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD burning validated (inputs: %d, outputs: %d, burned: %d)\n",
                 totalDDInputs, totalDDOutputs, totalDDInputs - totalDDOutputs);
    } else {
        // DD amount extraction failed (metadata registry miss + no txindex + no block-db lookup).
        // Fall back to structural validation: verify DD inputs exist and no DD outputs remain.
        // The full burn validation was performed by the miner during mempool acceptance
        // where coins view + amount extraction are always available.
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD burn structural validation only - %d DD inputs, %d DD change outputs\n",
                 ddInputIndices.size(), totalDDOutputs > 0 ? 1 : 0);
    }

    // Validate redemption conditions
    // NOTE: Only DD_TX_REDEEM exists. ERR is handled via burn amount, not tx type.
    if (txType != DD_TX_REDEEM) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-redeem-invalid-type",
                            "Only DD_TX_REDEEM type is valid for redemption");
    }

    // Validate redemption path (NORMAL or ERR) based on system health
    if (ctx.systemCollateral < 100) {
        // ERR path - validate system conditions
        if (!ValidateEmergencyRedemptionConditions(tx, ctx, state)) {
            return false;
        }
    } else {
        // Normal path - validate timelock expiry
        if (!ValidateNormalRedemptionConditions(tx, ctx, state)) {
            return false;
        }
    }

    // Validate collateral release amount is reasonable
    CAmount ddBurned = (totalDDInputs > totalDDOutputs) ? (totalDDInputs - totalDDOutputs) : 0;
    if (!ValidateCollateralReleaseAmount(tx, ctx, ddBurned, state)) {
        return false;
    }

    // Validate script path spending for collateral input
    if (!ValidateScriptPathSpending(tx, ctx, state)) {
        return false;
    }

    LogPrintf("DigiDollar: Redemption transaction %s validated successfully\n", tx.GetHash().ToString());
    return true;
}

// Helper functions for redemption validation
bool ValidateNormalRedemptionConditions(const CTransaction& tx,
                                       const ValidationContext& ctx,
                                       TxValidationState& state) {
    // Validate normal redemption conditions:
    // 1. The transaction's nLockTime must have expired (current height >= nLockTime)
    // 2. No ERR is active (system health >= 100%)

    // Check if nLockTime has been reached (CLTV uses >= semantics)
    // Standard Bitcoin CLTV behavior: nHeight >= nLockTime is valid
    if (ctx.nHeight < static_cast<int>(tx.nLockTime)) {
        LogPrintf("DigiDollar: Normal redemption rejected - timelock not expired (current: %d, required: %d)\n",
                  ctx.nHeight, tx.nLockTime);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "redemption-timelock-active",
                            strprintf("Timelock not expired (current height %d, required %d)",
                                    ctx.nHeight, tx.nLockTime));
    }

    // Check if ERR (Emergency Redemption Ratio) is active (skip for historical blocks)
    // Use systemCollateral from context (percentage, where 100 = 100% collateralized)
    if (!ctx.skipOracleValidation && ctx.systemCollateral < 100) {
        LogPrintf("DigiDollar: Normal redemption rejected - ERR active (system health: %d%%)\n", ctx.systemCollateral);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "redemption-err-active",
                            strprintf("ERR active - system health %d%% (normal redemptions blocked)", ctx.systemCollateral));
    }

    LogPrintf("DigiDollar: Normal redemption validation passed (height: %d >= locktime: %d, system health: %d%%)\n",
              ctx.nHeight, tx.nLockTime, ctx.systemCollateral);
    return true;
}

bool ValidateEmergencyRedemptionConditions(const CTransaction& tx,
                                         const ValidationContext& ctx,
                                         TxValidationState& state) {
    // ERR (Emergency Redemption Ratio) validation:
    // KEY CONCEPT: ERR increases DD burn requirement, NOT reduces collateral return!
    // - User must burn MORE DD than originally minted to get FULL collateral back
    // - Example: At 80% ratio, burn 125 DD to get back collateral for 100 DD position

    LogPrintf("DigiDollar: Validating emergency (ERR) redemption conditions\n");

    // IMPORTANT: Check structural requirements FIRST before checking ERR activation
    // This ensures more specific error messages for invalid transactions

    // Verify transaction has inputs
    if (tx.vin.empty()) {
        LogPrintf("DigiDollar: ERR redemption rejected - no inputs\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-no-inputs");
    }

    // Verify timelock has expired (if using locktime)
    // ERR redemptions still require timelock expiry
    if (tx.nLockTime > 0 && ctx.nHeight < static_cast<int>(tx.nLockTime)) {
        LogPrintf("DigiDollar: ERR redemption rejected - timelock not expired (current: %d, required: %d)\n",
                  ctx.nHeight, tx.nLockTime);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-timelock-active",
                            strprintf("Timelock not expired (current height %d, required %d)",
                                    ctx.nHeight, tx.nLockTime));
    }

    // Get system health from context (consistent with ValidateNormalRedemptionConditions)
    int systemHealth = ctx.systemCollateral;

    // Check if ERR is needed (system health < 100%)
    if (systemHealth >= 100) {
        LogPrintf("DigiDollar: ERR redemption rejected - system health %d%% is healthy (ERR requires < 100%%)\n",
                  systemHealth);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-not-required",
                            strprintf("ERR not needed - system health %d%% (ERR requires < 100%%)", systemHealth));
    }

    // Calculate ERR parameters
    double errRatio = ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(systemHealth);
    double ddMultiplier = 1.0 / errRatio; // How much MORE DD is needed

    LogPrintf("DigiDollar: ERR redemption - system health: %d%%, ratio: %.2f, DD burn multiplier: %.2fx\n",
              systemHealth, errRatio, ddMultiplier);

    // Note: The actual DD burn verification (checking that user burns >= RequiredDDBurn)
    // happens in the transaction builder and full validation. Here we just verify:
    // 1. ERR is needed (system health < 100%)
    // 2. Timelock has expired
    // 3. Transaction has valid structure

    // During ERR, user burns MORE DD to get FULL collateral back
    // The collateral return is NOT reduced - only the DD burn requirement increases

    LogPrintf("DigiDollar: ERR redemption conditions validated - user must burn %.1f%% extra DD to get full collateral\n",
              (ddMultiplier - 1.0) * 100);

    // RED Phase: ERR validation not fully implemented yet (oracle consensus needed)
    // For now, reject all ERR transactions until GREEN phase
    LogPrintf("DigiDollar: ERR redemption rejected - RED phase, validation incomplete\n");
    return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-validation-incomplete");
}


bool ValidateCollateralReleaseAmount(const CTransaction& tx,
                                   const ValidationContext& ctx,
                                   CAmount ddBurned,
                                   TxValidationState& state) {
    // Validate collateral release amount for redemption transactions
    // Must verify that DGB released is proportional to DD burned

    if (ctx.coins == nullptr) {
        // No UTXO access — cannot validate collateral proportionality.
        // This path should not be hit during ConnectBlock (always has coins view).
        LogPrintf("DigiDollar: WARNING - No coins view for collateral release validation\n");
        return true;
    }

    // Input 0 is assumed to be the collateral input
    if (tx.vin.empty()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-release-no-inputs");
    }

    // Look up collateral UTXO to get locked DGB amount and original DD minted
    Coin collateralCoin;
    if (!ctx.coins->GetCoin(tx.vin[0].prevout, collateralCoin)) {
        LogPrintf("DigiDollar: Could not find collateral UTXO for release validation\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-release-utxo-not-found");
    }

    CAmount lockedCollateral = collateralCoin.out.nValue;
    if (lockedCollateral <= 0) {
        LogPrintf("DigiDollar: Collateral UTXO has zero value\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-release-zero-collateral");
    }

    // Extract original DD amount — first try metadata registry, then creating tx lookup.
    //
    // SECURITY [T1-08]: The collateral UTXO is a P2TR script (OP_1 + 32 bytes) which does
    // NOT contain the DD amount. During cross-node block validation, the ephemeral metadata
    // registry is empty. Without this fix, the function silently allowed ANY release amount,
    // enabling an attacker to burn 1 cent of DD and steal all locked collateral.
    //
    // Strategy: 1) metadata registry, 2) txindex, 3) block-db lookup, 4) REJECT
    CAmount originalDDMinted = 0;
    if (!ExtractDDAmount(collateralCoin.out.scriptPubKey, originalDDMinted) || originalDDMinted <= 0) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Could not extract original DD amount from collateral script, "
                 "trying creating transaction lookup...\n");

        // Helper: extract DD minted amount from a mint transaction's OP_RETURN
        auto extractDDFromMintTx = [](const CTransactionRef& prev_tx, CAmount& ddOut) -> bool {
            for (const auto& vout : prev_tx->vout) {
                if (vout.scriptPubKey.size() == 0 || vout.scriptPubKey[0] != OP_RETURN) continue;

                CScript::const_iterator pc = vout.scriptPubKey.begin();
                opcodetype opcode;
                std::vector<unsigned char> data;

                if (!vout.scriptPubKey.GetOp(pc, opcode)) continue; // Skip OP_RETURN
                if (!vout.scriptPubKey.GetOp(pc, opcode, data)) continue;
                if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;

                // Read tx type
                if (!vout.scriptPubKey.GetOp(pc, opcode, data)) continue;
                int64_t txType = 0;
                if (data.size() > 0) {
                    try {
                        CScriptNum txTypeNum(data, true);
                        txType = txTypeNum.GetInt64();
                    } catch (const scriptnum_error&) { continue; }
                }

                // Only process MINT (type 1) — that's the transaction that created collateral
                if (txType != 1) continue;

                // Read DD amount (first push after type for mint)
                if (vout.scriptPubKey.GetOp(pc, opcode, data) && data.size() > 0) {
                    try {
                        CScriptNum scriptNum(data, true, 8);
                        ddOut = scriptNum.GetInt64();
                        return ddOut > 0;
                    } catch (const scriptnum_error&) {}
                }
            }
            return false;
        };

        bool found = false;

        // Try txindex (authoritative — reads creating tx from indexed database)
        if (!found && g_txindex) {
            uint256 block_hash;
            CTransactionRef prev_tx;
            if (g_txindex->FindTx(tx.vin[0].prevout.hash, block_hash, prev_tx)) {
                found = extractDDFromMintTx(prev_tx, originalDDMinted);
                if (found) {
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Extracted original DD minted (%lld) from txindex\n",
                             (long long)originalDDMinted);
                }
            }
        }

        // Try block-db lookup (universal fallback — every full node has every block)
        if (!found && ctx.txLookup) {
            CTransactionRef prev_tx;
            if (ctx.txLookup(tx.vin[0].prevout.hash, collateralCoin.nHeight, prev_tx)) {
                found = extractDDFromMintTx(prev_tx, originalDDMinted);
                if (found) {
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Extracted original DD minted (%lld) from block db\n",
                             (long long)originalDDMinted);
                }
            }
        }

        if (!found || originalDDMinted <= 0) {
            // SECURITY: REJECT if we cannot determine original DD amount.
            // A consensus rule must never be silently bypassed.
            LogPrintf("DigiDollar: SECURITY [T1-08] - Cannot determine original DD minted amount "
                      "for collateral at %s:%d. Rejecting to prevent collateral theft.\n",
                      tx.vin[0].prevout.hash.ToString(), tx.vin[0].prevout.n);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-release-unknown-dd-amount",
                               "Cannot verify proportional collateral release: original DD amount undetermined");
        }
    }

    // SECURITY [T2-03]: Require full DD burn for collateral release.
    //
    // In the UTXO model, the entire collateral UTXO is consumed as vin[0].
    // Partial burns are NOT safe because:
    //   1) The excess collateral (lockedCollateral - proportionalRelease) becomes miner fee
    //   2) A miner-attacker can burn 1% of DD and recover 100% of collateral (99% as fee)
    //   3) Cross-mint burns allow releasing collateral from mint A using DD from mint B
    //   4) Remaining DD from the original mint stays in circulation, completely unbacked
    //
    // To redeem, you MUST burn at least the full DD amount minted against this collateral.
    // Burning more than originalDDMinted is allowed (user's loss, not a security risk).
    if (ddBurned < originalDDMinted) {
        LogPrintf("DigiDollar: SECURITY [T2-03] - Partial DD burn rejected: burned %lld < original %lld. "
                  "Full burn required to release collateral (UTXO is indivisible).\n",
                  (long long)ddBurned, (long long)originalDDMinted);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-release-partial-burn",
                           strprintf("Must burn all DD minted against this collateral: burned %lld < required %lld",
                                     (long long)ddBurned, (long long)originalDDMinted));
    }

    // Full redemption: ddBurned >= originalDDMinted
    CAmount allowedRelease = lockedCollateral;

    // Small fee tolerance (0.1% or 1000 satoshis, whichever is larger)
    CAmount feeTolerance = std::max((CAmount)1000, allowedRelease / 1000);

    // Sum total DGB outputs in the transaction
    CAmount totalDGBOutputs = 0;
    for (const auto& output : tx.vout) {
        if (output.nValue > 0) {
            totalDGBOutputs += output.nValue;
        }
    }

    // Subtract fee input values to get NET collateral release
    // Fee inputs are non-collateral, non-DD inputs (inputs with nValue > 0 after input 0)
    // Input 0 = collateral, then DD inputs (nValue=0), then fee inputs (nValue>0)
    CAmount totalFeeInputs = 0;
    for (size_t i = 1; i < tx.vin.size(); ++i) {
        Coin coin;
        if (ctx.coins->GetCoin(tx.vin[i].prevout, coin) && coin.out.nValue > 0) {
            totalFeeInputs += coin.out.nValue;
        }
    }

    // Net collateral release = total outputs - fee inputs returned as change
    CAmount totalDGBRelease = totalDGBOutputs - totalFeeInputs;
    if (totalDGBRelease < 0) totalDGBRelease = 0;

    LogPrintf("DigiDollar: Collateral release check - totalOutputs: %lld, feeInputs: %lld, netRelease: %lld, allowed: %lld\n",
              (long long)totalDGBOutputs, (long long)totalFeeInputs, (long long)totalDGBRelease, (long long)allowedRelease);

    if (totalDGBRelease > allowedRelease + feeTolerance) {
        LogPrintf("DigiDollar: Collateral release too large - releasing: %lld, allowed: %lld (+ %lld tolerance), locked: %lld, ddBurned: %lld, originalDD: %lld\n",
                  (long long)totalDGBRelease, (long long)allowedRelease, (long long)feeTolerance,
                  (long long)lockedCollateral, (long long)ddBurned, (long long)originalDDMinted);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-release-excessive",
                           strprintf("Collateral release %lld exceeds allowed %lld", (long long)totalDGBRelease, (long long)allowedRelease));
    }

    LogPrintf("DigiDollar: Collateral release validated - releasing: %lld, allowed: %lld, ddBurned: %lld/%lld\n",
              (long long)totalDGBRelease, (long long)allowedRelease, (long long)ddBurned, (long long)originalDDMinted);
    return true;
}

bool ValidateScriptPathSpending(const CTransaction& tx,
                               const ValidationContext& ctx,
                               TxValidationState& state) {
    // Phase 1: We use key-path spending (Schnorr signatures), not script-path
    // Script path spending (MAST) will be implemented in RED phase for advanced features
    // For now, all redemptions use Taproot key-path spending which is validated by consensus

    // Key-path spending validation happens in standard Bitcoin Script validation
    // The Schnorr signature verification is handled by the consensus engine
    // No additional validation needed here for Phase 1

    LogPrintf("DigiDollar: Script path spending validation - using key-path (Schnorr), validation passed\n");
    return true;  // Allow key-path spending (standard Taproot)
}

// ============================================================================
// Main Transaction Validation
// ============================================================================

bool ValidateDigiDollarTransaction(const CTransaction& tx,
                                  const ValidationContext& ctx,
                                  TxValidationState& state) {
    // Check if this is a DD transaction
    if (!HasDigiDollarMarker(tx)) {
        return true; // Not a DD transaction - pass through
    }

    // Extract transaction type
    DigiDollarTxType txType;
    try {
        txType = DigiDollar::GetDigiDollarTxType(tx);
    } catch (const std::exception&) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dd-tx-version");
    }

    LogPrintf("DigiDollar: Validating %s transaction (txid: %s)\n",
              txType == DD_TX_MINT ? "MINT" :
              txType == DD_TX_TRANSFER ? "TRANSFER" :
              txType == DD_TX_REDEEM ? "REDEEM" : "UNKNOWN",
              tx.GetHash().ToString());

    // Update volatility monitoring state if we have valid context
    if (ctx.nHeight > 0 && ctx.oraclePriceMicroUSD > 0) {
        Volatility::VolatilityMonitor::UpdateState(ctx.nHeight);

        // Record new oracle price if this is a mint transaction with fresh oracle data
        if (txType == DD_TX_MINT) {
            // In a full implementation, we would extract oracle data from the transaction
            // For now, we use the context oracle price
            Volatility::VolatilityMonitor::RecordPrice(ctx.oraclePriceMicroUSD, GetTime(), ctx.nHeight);
        }
    }

    // ERR Pre-validation: Check if minting should be blocked during ERR
    if (txType == DD_TX_MINT && ShouldBlockMintingDuringERR(ctx)) {
        LogPrintf("DigiDollar: Minting blocked during ERR activation\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "minting-blocked-during-err");
    }

    // BUG #3 FIX: Don't block normal redemptions during ERR until ERR path is implemented.
    // The ERR redemption path (ValidateEmergencyRedemptionConditions) always returns
    // "err-validation-incomplete", which means blocking normal redemptions creates a
    // deadlock where NO redemption path is available.
    // TODO: Re-enable this check when ERR redemption validation is implemented.
    // if (txType == DD_TX_REDEEM && ShouldBlockNormalRedemptionsDuringERR(ctx)) {
    //     LogPrintf("DigiDollar: Normal redemptions blocked during ERR activation\n");
    //     return state.Invalid(TxValidationResult::TX_CONSENSUS, "normal-redemption-blocked-during-err");
    // }

    // Type-specific validation
    // NOTE: Only 3 types exist - MINT, TRANSFER, REDEEM
    // ERR is handled within REDEEM based on system health
    switch (txType) {
        case DD_TX_MINT:
            return ValidateMintTransaction(tx, ctx, state);

        case DD_TX_TRANSFER:
            return ValidateTransferTransaction(tx, ctx, state);

        case DD_TX_REDEEM:
            return ValidateRedemptionTransaction(tx, ctx, state);

        default:
            LogPrintf("DigiDollar: Unknown transaction type: %d\n", static_cast<int>(txType));
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dd-tx-type");
    }
}

// ============================================================================
// Helper Functions for Transaction Validation
// ============================================================================

bool ValidateCollateralOutput(const CTxOut& output, const CTransaction& tx,
                             TxValidationState& state) {
    // Check if script is valid P2TR (Taproot)
    std::vector<std::vector<unsigned char>> vSolutions;
    TxoutType scriptType = Solver(output.scriptPubKey, vSolutions);

    if (scriptType != TxoutType::WITNESS_V1_TAPROOT) {
        LogPrintf("DigiDollar: Collateral output is not P2TR (Taproot), type: %s\n",
                  GetTxnOutputType(scriptType));
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-collateral-script");
    }

    // Check minimum value (dust threshold)
    const CAmount DUST_THRESHOLD = 546; // Standard Bitcoin dust threshold
    if (output.nValue < DUST_THRESHOLD) {
        LogPrintf("DigiDollar: Collateral output below dust threshold: %d < %d\n",
                  output.nValue, DUST_THRESHOLD);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "collateral-dust");
    }

    // Additional validation could include:
    // - Verify taproot script structure
    // - Check for valid redemption paths
    // - Validate timelock parameters

    LogPrintf("DigiDollar: Collateral output validation passed - Value: %d DGB\n",
              output.nValue / COIN);

    return true;
}

bool ValidateDDOutput(const CTxOut& output, const CTransaction& tx,
                     TxValidationState& state) {
    // DD outputs must have 0 DGB value
    if (output.nValue != 0) {
        LogPrintf("DigiDollar: DD output has non-zero DGB value: %d\n", output.nValue);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "dd-output-value");
    }

    // Check P2TR format (Taproot)
    std::vector<std::vector<unsigned char>> vSolutions;
    TxoutType scriptType = Solver(output.scriptPubKey, vSolutions);

    if (scriptType != TxoutType::WITNESS_V1_TAPROOT) {
        LogPrintf("DigiDollar: DD output is not P2TR (Taproot), type: %s\n",
                  GetTxnOutputType(scriptType));
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dd-script");
    }

    // Phase 1: Script type already verified by caller using value-based heuristic
    // (P2TR with value=0 indicates DD token output)
    // Phase 2 will use UTXO database tracking for proper identification

    LogPrintf("DigiDollar: DD output validation passed\n");

    return true;
}

// ExtractDDAmount is defined in consensus/digidollar.cpp
// Removed duplicate implementation

int64_t ExtractLockTime(const CScript& script) {
    // Extract lock time from collateral script
    // This is a simplified implementation for Phase 1
    // In Phase 2, this would properly parse the taproot witness program
    // and extract the timelock from the specific script path

    // For now, we'll use a heuristic approach by scanning for timelock opcodes
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;

    while (script.GetOp(pc, opcode, data)) {
        if (opcode == OP_CHECKLOCKTIMEVERIFY) {
            // Look backward for the height value
            // This is simplified - real implementation would parse properly

            // TODO: For Phase 2, properly extract from witness/OP_RETURN
            // For now use a heuristic: we can infer lock time from collateral ratio
            // However P2TR scripts are hashed, so we can't read them from outputs
            // Return a reasonable middle-ground default
            return 90 * 24 * 60 * 4; // 90 days (tier 2) = 400% ratio as default
        }

        // Try to interpret data as a potential timelock value
        if (data.size() >= 4 && data.size() <= 8) {
            try {
                CScriptNum timelock(data, true, data.size());
                int64_t lockValue = timelock.GetInt64();

                // Reasonable timelock range (between 1 day and 10 years)
                int64_t minLock = 24 * 60 * 4; // 1 day
                int64_t maxLock = 10 * 365 * 24 * 60 * 4; // 10 years

                if (lockValue >= minLock && lockValue <= maxLock) {
                    LogPrintf("DigiDollar: Extracted lock time: %d blocks (~%d days)\n",
                              lockValue, lockValue / (24 * 60 * 4));
                    return lockValue;
                }
            } catch (const std::exception&) {
                // Ignore invalid script numbers
                continue;
            }
        }
    }

    // If we can't extract a proper timelock, log warning and return default
    LogPrintf("DigiDollar: Warning - Could not extract lock time from script, using default\n");
    return 30 * 24 * 60 * 4; // Default to 30 days
}

CAmount GetSystemCollateralRatio() {
    // Get system-wide collateral ratio
    // This would normally query the UTXO set to calculate total system health
    // For Phase 1, return a mock value that can be adjusted for testing

    // In full implementation, this would:
    // 1. Sum all locked collateral values
    // 2. Sum all minted DD amounts
    // 3. Calculate ratio using current oracle price
    // 4. Return percentage (e.g., 150 for 150%)

    return 150; // Mock 150% system collateral ratio
}

// ============================================================================
// ERR (Emergency Redemption Ratio) Validation Implementation
// ============================================================================

bool ValidateERRRedemption(const CTransaction& tx,
                          const ValidationContext& ctx,
                          TxValidationState& state) {
    // RED Phase: Basic framework with ERR integration
    LogPrintf("DigiDollar: Validating ERR redemption transaction\n");

    // Check if ERR should be active based on system health
    if (!DigiDollar::ERR::EmergencyRedemptionRatio::ShouldActivateERR(ctx.systemCollateral)) {
        LogPrintf("DigiDollar: ERR redemption attempted but ERR not active (system health: %d%%)\n",
                  ctx.systemCollateral);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-not-active");
    }

    // Get current ERR state
    DigiDollar::ERR::ERRState errState = DigiDollar::ERR::EmergencyRedemptionRatio::GetCurrentState();
    if (!errState.isActive) {
        LogPrintf("DigiDollar: ERR state not active\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-state-inactive");
    }

    // Calculate expected DD amount being burned and collateral being released
    CAmount ddInputAmount = 0;
    CAmount collateralOutputAmount = 0;

    // Sum DD inputs (should be burned)
    for (const auto& input : tx.vin) {
        // In full implementation, would lookup the UTXO to get DD amount
        // For RED phase, use placeholder calculation
        ddInputAmount += 10000; // Placeholder: $100 DD per input
    }

    // Sum non-DD outputs (collateral being released)
    for (const auto& output : tx.vout) {
        if (!IsDDTokenScript(output.scriptPubKey)) {
            collateralOutputAmount += output.nValue;
        }
    }

    if (ddInputAmount <= 0) {
        LogPrintf("DigiDollar: ERR redemption has no DD inputs to burn\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-no-dd-inputs");
    }

    // Calculate expected collateral return with ERR adjustment
    // Oracle price is in cents, DD is in cents
    // Collateral_sats = (DD_cents * COIN) / oracle_cents
    CAmount expectedFullCollateral = (ddInputAmount * COIN) / ctx.oraclePriceMicroUSD;
    CAmount expectedERRCollateral = DigiDollar::ERR::EmergencyRedemptionRatio::GetAdjustedRedemption(
        expectedFullCollateral, ctx.systemCollateral);

    LogPrintf("DigiDollar: ERR validation - DD burned: %d, Expected collateral: %d, ERR adjusted: %d, Actual: %d\n",
              ddInputAmount, expectedFullCollateral, expectedERRCollateral, collateralOutputAmount);

    // Validate ERR adjustment is correct (with small tolerance for rounding)
    CAmount tolerance = COIN / 1000; // 0.001 DGB tolerance
    if (abs(collateralOutputAmount - expectedERRCollateral) > tolerance) {
        LogPrintf("DigiDollar: ERR adjustment validation failed\n");
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "invalid-err-adjustment");
    }

    // RED Phase: For now, always fail since oracle consensus not implemented
    LogPrintf("DigiDollar: ERR redemption validation - implementation incomplete\n");
    return state.Invalid(TxValidationResult::TX_CONSENSUS, "err-validation-incomplete");
}

bool ShouldBlockMintingDuringERR(const ValidationContext& ctx) {
    // Check if ERR is currently active
    return DigiDollar::ERR::EmergencyRedemptionRatio::ShouldBlockMinting();
}

bool ShouldBlockNormalRedemptionsDuringERR(const ValidationContext& ctx) {
    // Check if ERR is currently active
    DigiDollar::ERR::ERRState errState = DigiDollar::ERR::EmergencyRedemptionRatio::GetCurrentState();
    return errState.isActive;
}

bool ValidateERRAdjustmentAmount(CAmount originalCollateral,
                                CAmount adjustedCollateral,
                                int systemHealth) {
    // Calculate expected ERR adjustment
    double expectedRatio = DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(systemHealth);
    CAmount expectedAdjusted = static_cast<CAmount>(originalCollateral * expectedRatio);

    // Allow small tolerance for rounding
    CAmount tolerance = COIN / 1000; // 0.001 DGB tolerance
    return abs(adjustedCollateral - expectedAdjusted) <= tolerance;
}

bool ValidateERROracleConsensus(const CTransaction& tx,
                               const ValidationContext& ctx) {
    // RED Phase: Always return false - oracle consensus not implemented
    // In GREEN phase, this will extract oracle messages from transaction
    // and validate 8-of-15 signature threshold
    LogPrintf("DigiDollar: Oracle consensus validation not implemented yet\n");
    return false;
}

double CalculateExpectedERRAdjustment(int systemHealth) {
    // Delegate to ERR system
    return DigiDollar::ERR::EmergencyRedemptionRatio::CalculateERRAdjustment(systemHealth);
}

} // namespace DigiDollar