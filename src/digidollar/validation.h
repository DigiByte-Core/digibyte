// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_DIGIDOLLAR_VALIDATION_H
#define DIGIBYTE_DIGIDOLLAR_VALIDATION_H

#include <script/script.h>
#include <script/interpreter.h>
#include <script/script_error.h>
#include <consensus/validation.h>
#include <consensus/amount.h>
#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <primitives/transaction.h>
#include <chainparams.h>
#include <coins.h>

class CTxMemPool;

#include <cstdint>
#include <functional>
#include <vector>

namespace DigiDollar {

// DigiDollarTxType enum is now defined in consensus/digidollar.h

/**
 * DigiDollar transaction version marker
 * Format: 0x4444XXYY where XX is transaction type, YY is sub-version
 */
static const uint32_t DD_TX_VERSION = 0x44440000;

/**
 * Script type identification for DigiDollar operations
 */
enum class ScriptType {
    NOT_DIGIDOLLAR,     // Regular Bitcoin/DigiByte script
    COLLATERAL_LOCK,    // P2TR script locking DGB collateral
    DD_TOKEN_OUTPUT     // P2TR script for DigiDollar token output
};

/**
 * Collateral redemption paths available in P2TR MAST
 * NOTE: Only 2 paths exist. NO partial redemption, NO emergency oracle override.
 */
enum class RedemptionPath {
    NORMAL = 0,      // Standard timelock expiry redemption (health >= 100%)
    ERR = 1          // Emergency Redemption Ratio (health < 100%, burn more DD)
};

/**
 * Validation context for DigiDollar operations
 * Contains current blockchain state needed for validation
 */
/**
 * Look up a transaction from the block database by txid and coin creation height.
 * Used to extract DD amounts from the creating transaction's OP_RETURN when
 * txindex is unavailable. Every full node has every block on disk, so this
 * provides a universal fallback for DD amount extraction.
 */
using TxLookupFn = std::function<bool(const uint256& txid, uint32_t coinHeight, CTransactionRef& tx_out)>;

struct ValidationContext {
    int nHeight;                     // Current block height
    CAmount oraclePriceMicroUSD;     // Current DGB price in micro-USD (e.g., 6310 = $0.00631)
    int systemCollateral;            // System-wide collateral ratio percentage
    const CChainParams& params;      // Chain parameters including DD consensus params
    const CCoinsViewCache* coins;    // Coins view for UTXO lookups (nullptr if not available)
    bool skipOracleValidation;       // Skip oracle-dependent validation (for historical blocks)
    TxLookupFn txLookup;             // Look up tx from block database (for DD amount extraction)
    const CTxMemPool* mempool;       // Mempool context; DD amount resolution remains confirmed-only
    int64_t nBlockTime;              // Candidate block timestamp for deterministic volatility recording
    CAmount volatilityAnchorPriceMicroUSD; // Chain-derived lagged-median mint volatility anchor
                                     // (0 = no in-window bundle samples => gate passes);
                                     // consulted only from nDDVolatilityFixHeight
    bool policyContext;              // True for mempool/reorg policy callers: the anchor freeze
                                     // is reported as TX_MEMPOOL_POLICY instead of TX_CONSENSUS

    ValidationContext(int height, CAmount price_micro_usd, int collateral, const CChainParams& chainParams,
                      const CCoinsViewCache* coins_view = nullptr, bool skip_oracle = false,
                      TxLookupFn tx_lookup = nullptr, const CTxMemPool* pool = nullptr,
                      int64_t block_time = 0, CAmount volatility_anchor_price_micro_usd = 0,
                      bool policy_context = false)
        : nHeight(height), oraclePriceMicroUSD(price_micro_usd), systemCollateral(collateral),
          params(chainParams), coins(coins_view), skipOracleValidation(skip_oracle),
          txLookup(std::move(tx_lookup)), mempool(pool), nBlockTime(block_time),
          volatilityAnchorPriceMicroUSD(volatility_anchor_price_micro_usd),
          policyContext(policy_context) {}
};

// ============================================================================
// Core Validation Functions
// ============================================================================

/**
 * Validate DigiDollar script according to consensus rules
 *
 * @param script Script to validate
 * @param ctx Validation context with current blockchain state
 * @param serror Optional pointer to receive script error code
 * @return true if script is valid, false otherwise
 */
bool ValidateDigiDollarScript(const CScript& script,
                              const ValidationContext& ctx,
                              ScriptError* serror = nullptr);

/**
 * Validate complete DigiDollar transaction
 *
 * Performs comprehensive validation including:
 * - Transaction type verification
 * - Amount validation (mint limits, output minimums)
 * - Collateral ratio requirements
 * - Input/output consistency
 *
 * @param tx Transaction to validate
 * @param ctx Validation context
 * @param state Transaction validation state for error reporting
 * @return true if transaction is valid, false otherwise
 */
bool ValidateDigiDollarTransaction(const CTransaction& tx,
                                   const ValidationContext& ctx,
                                   TxValidationState& state);

/**
 * Record deterministic volatility input after a mint block has fully connected.
 *
 * Validation itself must remain side-effect free because mempool admission,
 * miner template probing, and failed block validation can all call the same
 * transaction validator.
 */
void RecordAcceptedMintVolatility(const ValidationContext& ctx);

/**
 * Check whether a transaction spends a confirmed DigiDollar collateral vault,
 * including non-DD-looking spends that must still be rejected unless they are
 * proper DD redemptions.
 */
bool SpendsDigiDollarCollateralVault(const CTransaction& tx,
                                     const ValidationContext& ctx);

/**
 * Check whether a transaction must enter DigiDollar validation. This includes
 * canonical DD transactions and non-DD transactions that spend DD collateral.
 */
bool RequiresDigiDollarValidation(const CTransaction& tx,
                                  const ValidationContext& ctx);

// ============================================================================
// Script Analysis Functions
// ============================================================================

/**
 * Identify the type of script (DD collateral, DD token, or regular)
 *
 * @param script Script to analyze
 * @return ScriptType indicating the script's purpose
 */
ScriptType IdentifyScriptType(const CScript& script);

/**
 * Extract DigiDollar amount from script
 *
 * @param script Script containing DD amount
 * @param amount Output parameter to receive extracted amount
 * @return true if amount successfully extracted, false otherwise
 */
bool ExtractDDAmount(const CScript& script, CAmount& amount);

/**
 * Find the index of the DD OP_RETURN output in a transaction.
 * Searches all vouts for an OP_RETURN with the 'DD' marker bytes.
 *
 * @param tx Transaction to search
 * @return Index of the DD OP_RETURN vout, or -1 if not found
 */
int FindDDOpReturn(const CTransaction& tx);

/**
 * Extract DD amount from the previous transaction's OP_RETURN metadata.
 * This is the DECENTRALIZED approach - no local registry needed.
 * The DD amount is stored in the creating transaction's OP_RETURN output.
 *
 * @param prevout The outpoint (txid + output index) of the DD UTXO
 * @param amount Output: The DD amount in cents
 * @return true if amount was successfully extracted
 */
bool ExtractDDAmountFromPrevTx(const COutPoint& prevout, CAmount& amount);

/**
 * Extract DD amount from the creating transaction fetched via block database
 * lookup callback (used when txindex is unavailable).
 *
 * @param prevout The outpoint (txid + output index) of the DD UTXO
 * @param coinHeight Height at which the coin was created
 * @param txLookup Callback that loads the creating transaction
 * @param amount Output: The DD amount in cents
 * @return true if amount was successfully extracted
 */
bool ExtractDDAmountFromBlockDb(const COutPoint& prevout, uint32_t coinHeight,
                                const TxLookupFn& txLookup, CAmount& amount);

/**
 * Extract the DD minted amount and locked collateral from a mint transaction.
 * This is used by block connect/disconnect accounting, where output order must
 * not be assumed beyond the mint validator's consensus rules.
 *
 * @param tx Mint transaction to inspect
 * @param ddAmount Output: DD amount minted, in cents
 * @param collateralAmount Output: DGB collateral locked, in satoshis
 * @return true if both values were recovered from the transaction
 */
bool ExtractMintAccountingAmounts(const CTransaction& tx,
                                  CAmount& ddAmount,
                                  CAmount& collateralAmount);

/**
 * Extract actual DD burned and collateral released by a redemption transaction.
 * The spent coins vector must align one-for-one with tx.vin and contain the
 * pre-spend coins from the coins view or block undo data.
 *
 * @param tx Redemption transaction to inspect
 * @param spentCoins Pre-spend coins corresponding to tx.vin
 * @param txLookup Callback that loads creating transactions for DD inputs
 * @param ddBurned Output: DD destroyed by the redemption, in cents
 * @param collateralAmount Output: DGB collateral input value, in satoshis
 * @return true if the redemption accounting amounts were recovered
 */
bool ExtractRedemptionAccountingAmounts(const CTransaction& tx,
                                        const std::vector<Coin>& spentCoins,
                                        const TxLookupFn& txLookup,
                                        CAmount& ddBurned,
                                        CAmount& collateralAmount);

/**
 * Check if script is a DigiDollar collateral locking script
 *
 * @param script Script to check
 * @return true if script locks DGB collateral for DD minting
 */
bool IsCollateralScript(const CScript& script);

/**
 * Check if script is a DigiDollar token output script
 *
 * @param script Script to check
 * @return true if script represents DD token ownership
 */
bool IsDDTokenScript(const CScript& script);

// ============================================================================
// Path-Specific Validation Functions
// ============================================================================

/**
 * Validate normal redemption path conditions
 *
 * Checks that timelock has expired and other normal conditions are met.
 *
 * @param script Redemption script
 * @param currentHeight Current block height
 * @return true if normal redemption is valid
 */
bool ValidateNormalRedemption(const CScript& script, int currentHeight);

/**
 * Validate emergency redemption path conditions
 *
 * Verifies the configured oracle signature threshold is met.
 *
 * @param script Emergency redemption script
 * @param sigs Oracle signatures provided
 * @return true if emergency override is valid
 */
bool ValidateEmergencyRedemption(const CScript& script,
                                const std::vector<std::vector<unsigned char>>& sigs);


/**
 * Validate ERR (Emergency Redemption Ratio) path conditions
 *
 * Checks that system is under-collateralized (< 100%) enabling ERR redemptions.
 *
 * @param script ERR redemption script
 * @param systemCollateral System-wide collateral ratio percentage
 * @return true if ERR redemption is valid
 */
bool ValidateERRRedemption(const CScript& script, int systemCollateral);

// ============================================================================
// Amount and Collateral Validation
// ============================================================================

/**
 * Validate DigiDollar mint amount against consensus limits
 *
 * @param amount Amount to validate (in cents)
 * @param params Chain parameters
 * @param nHeight Current block height (for activation height checks)
 * @return true if amount is within valid range for minting
 */
bool ValidateMintAmount(CAmount amount, const CChainParams& params, int nHeight = 0);

/**
 * Validate DigiDollar output amount against minimum requirements
 *
 * @param amount Amount to validate (in cents)
 * @param params Chain parameters
 * @return true if amount meets minimum output requirements
 */
bool ValidateOutputAmount(CAmount amount, const CChainParams& params);

/**
 * Validate collateral ratio for mint operation
 *
 * Checks that DGB locked provides sufficient collateral for DD minted,
 * considering lock time requirements and DCA adjustments.
 *
 * @param dgbLocked Amount of DGB locked as collateral
 * @param ddMinted Amount of DD being minted
 * @param lockTime Lock period in blocks
 * @param ctx Validation context with current price and system state
 * @return true if collateral ratio is sufficient
 */
bool ValidateCollateralRatio(CAmount dgbLocked, CAmount ddMinted,
                            int64_t lockTime, const ValidationContext& ctx);

/**
 * Calculate required DGB collateral for DD mint
 *
 * @param ddAmount DD amount to mint (in cents)
 * @param lockTime Lock period in blocks
 * @param ctx Validation context
 * @return Required DGB amount in satoshis
 */
CAmount CalculateRequiredCollateral(CAmount ddAmount, int64_t lockTime,
                                   const ValidationContext& ctx);

/**
 * Get effective collateral ratio after DCA adjustments
 *
 * @param baseRatio Base collateral ratio for lock period
 * @param systemCollateral Current system collateral percentage
 * @param params Chain parameters
 * @return Effective ratio after DCA multiplier
 */
int GetEffectiveCollateralRatio(int baseRatio, int systemCollateral,
                               const CChainParams& params);

// ============================================================================
// Transaction Type Validation
// ============================================================================

/**
 * Validate mint transaction structure and amounts
 *
 * @param tx Mint transaction
 * @param ctx Validation context
 * @param state Transaction validation state
 * @return true if mint transaction is valid
 */
bool ValidateMintTransaction(const CTransaction& tx,
                            const ValidationContext& ctx,
                            TxValidationState& state);

/**
 * Validate transfer transaction (DD conservation)
 *
 * @param tx Transfer transaction
 * @param ctx Validation context
 * @param state Transaction validation state
 * @return true if transfer transaction is valid
 */
bool ValidateTransferTransaction(const CTransaction& tx,
                                const ValidationContext& ctx,
                                TxValidationState& state);

/**
 * Validate redemption transaction
 *
 * @param tx Redemption transaction
 * @param ctx Validation context
 * @param state Transaction validation state
 * @return true if redemption transaction is valid
 */
bool ValidateRedemptionTransaction(const CTransaction& tx,
                                  const ValidationContext& ctx,
                                  TxValidationState& state);

// ============================================================================
// Helper Functions for Output Validation
// ============================================================================

/**
 * Validate collateral output in mint transaction
 *
 * Checks that collateral output:
 * - Uses P2TR script format
 * - Has minimum value above dust threshold
 * - Contains valid redemption paths
 *
 * @param output Collateral output to validate
 * @param tx Transaction containing the output
 * @param state Transaction validation state for error reporting
 * @return true if collateral output is valid
 */
bool ValidateCollateralOutput(const CTxOut& output, const CTransaction& tx,
                             TxValidationState& state);

/**
 * Validate DigiDollar token output in transaction
 *
 * Checks that DD output:
 * - Has 0 DGB value
 * - Uses P2TR script format
 * - Contains valid DD amount encoding
 *
 * @param output DD token output to validate
 * @param tx Transaction containing the output
 * @param state Transaction validation state for error reporting
 * @return true if DD output is valid
 */
bool ValidateDDOutput(const CTxOut& output, const CTransaction& tx,
                     TxValidationState& state);

/**
 * Extract lock time from collateral script
 *
 * Parses the collateral script to extract the timelock period.
 * Returns the lock time in blocks.
 *
 * @param script Collateral script to parse
 * @return Lock time in blocks, or default value if extraction fails
 */
int64_t ExtractLockTime(const CScript& script);

/**
 * Get current system-wide collateral ratio
 *
 * Calculates the overall health of the DigiDollar system by comparing
 * total locked collateral value to total minted DD value.
 *
 * @return System collateral ratio as percentage (e.g., 150 for 150%)
 */
CAmount GetSystemCollateralRatio();

// ============================================================================
// Redemption Transaction Helper Functions
// ============================================================================

/**
 * Validate normal redemption conditions (timelock expiry)
 *
 * @param tx Redemption transaction
 * @param ctx Validation context
 * @param state Transaction validation state
 * @return true if normal redemption conditions are met
 */
bool ValidateNormalRedemptionConditions(const CTransaction& tx,
                                       const ValidationContext& ctx,
                                       TxValidationState& state);

/**
 * Validate emergency redemption conditions (ERR or oracle approval)
 *
 * @param tx Redemption transaction
 * @param ctx Validation context
 * @param state Transaction validation state
 * @return true if emergency redemption conditions are met
 */
bool ValidateEmergencyRedemptionConditions(const CTransaction& tx,
                                         const ValidationContext& ctx,
                                         TxValidationState& state);


/**
 * Validate collateral release amount is reasonable
 *
 * @param tx Redemption transaction
 * @param ctx Validation context
 * @param ddBurned Amount of DD being burned
 * @param state Transaction validation state
 * @return true if collateral release amount is valid
 */
bool ValidateCollateralReleaseAmount(const CTransaction& tx,
                                   const ValidationContext& ctx,
                                   CAmount ddBurned,
                                   TxValidationState& state);

/**
 * Validate script path spending for collateral input
 *
 * @param tx Redemption transaction
 * @param ctx Validation context
 * @param state Transaction validation state
 * @return true if script path spending is valid
 */
bool ValidateScriptPathSpending(const CTransaction& tx,
                               const ValidationContext& ctx,
                               TxValidationState& state);

// ============================================================================
// ERR (Emergency Redemption Ratio) Validation Functions
// ============================================================================

/**
 * Validate ERR redemption transaction
 *
 * Verifies that ERR redemption meets all requirements:
 * - ERR is currently active (system < 100% collateralized)
 * - Correct ERR adjustment ratio applied
 * - Oracle consensus verified for ERR activation
 * - DD properly burned in transaction
 * - Collateral release matches ERR calculation
 *
 * @param tx ERR redemption transaction
 * @param ctx Validation context
 * @param state Transaction validation state
 * @return true if ERR redemption is valid
 */
bool ValidateERRRedemption(const CTransaction& tx,
                          const ValidationContext& ctx,
                          TxValidationState& state);

/**
 * Check if minting should be blocked due to ERR
 *
 * During ERR activation, new mints are blocked to:
 * - Prevent further system destabilization
 * - Focus on processing redemptions
 * - Allow system health to recover
 *
 * @param ctx Validation context
 * @return true if minting should be blocked
 */
bool ShouldBlockMintingDuringERR(const ValidationContext& ctx);

/**
 * Check if normal redemptions should be blocked due to ERR
 *
 * During ERR activation, normal redemptions are queued and
 * processed through the ERR system for fair distribution.
 *
 * @param ctx Validation context
 * @return true if normal redemptions should be blocked
 */
bool ShouldBlockNormalRedemptionsDuringERR(const ValidationContext& ctx);

/**
 * Validate ERR adjustment amount
 *
 * Verifies that the collateral return amount matches the
 * expected ERR adjustment for current system health.
 *
 * @param originalCollateral Original collateral amount
 * @param adjustedCollateral ERR-adjusted collateral amount
 * @param systemHealth Current system health percentage
 * @return true if ERR adjustment is correct
 */
bool ValidateERRAdjustmentAmount(CAmount originalCollateral,
                                CAmount adjustedCollateral,
                                int systemHealth);

/**
 * Legacy fail-closed transaction-level oracle consensus helper
 *
 * V1 ERR activation uses the block's validated MuSig2 v0x03 oracle bundle
 * and deterministic system health in ValidationContext. This helper remains
 * for older tests/external callers and returns false if called directly.
 *
 * @param tx Transaction containing oracle consensus data
 * @param ctx Validation context
 * @return true if oracle consensus is valid
 */
bool ValidateERROracleConsensus(const CTransaction& tx,
                               const ValidationContext& ctx);

/**
 * Calculate expected ERR adjustment for system health
 *
 * Returns the expected collateral return percentage based on
 * current system health and ERR tier thresholds.
 *
 * @param systemHealth Current system health percentage (0-30000)
 * @return ERR adjustment ratio (0.80-0.95)
 */
double CalculateExpectedERRAdjustment(int systemHealth);

} // namespace DigiDollar

#endif // DIGIBYTE_DIGIDOLLAR_VALIDATION_H
