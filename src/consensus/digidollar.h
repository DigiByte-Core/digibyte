// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_CONSENSUS_DIGIDOLLAR_H
#define DIGIBYTE_CONSENSUS_DIGIDOLLAR_H

#include <consensus/amount.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Forward declarations
class CBlockIndex;
class ChainstateManager;
class CTransaction;
class CScript;
namespace Consensus {
    struct Params;
}

namespace DigiDollar {

// DigiByte specific constants
static const int BLOCKS_PER_DAY = 24 * 60 * 4;  // 5760 blocks (15s blocks)
static const CAmount CENT = 1000000;  // DigiDollar cent in satoshis

// Mint transactions commit to an absolute collateral unlock height. Wallets
// build against the next block height, but real mempool inclusion can be
// delayed by oracle-bundle timing, block assembly, or fee/package ordering.
// Allowing a small consensus window above the claimed tier keeps delayed mints
// mineable without ever permitting a shorter-than-tier lock.
static constexpr int64_t MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS = 100;

// DigiDollar transaction types
// NOTE: Only 4 types exist. NO partial redemption, NO emergency oracle override.
// ERR (Emergency Redemption Ratio) uses DD_TX_REDEEM with health-based DD burn adjustment.
enum DigiDollarTxType : uint8_t {
    DD_TX_NONE = 0,
    DD_TX_MINT = 1,      // Lock DGB, create DigiDollars
    DD_TX_TRANSFER = 2,  // Transfer DigiDollars between addresses
    DD_TX_REDEEM = 3,    // Burn DigiDollars, unlock DGB (ERR handled via burn amount)
    DD_TX_MAX = 4        // For validation
};

/**
 * Earliest height at which a DigiDollar output can exist on this chain (the
 * "activation floor"). Single source of truth shared by consensus validation
 * (EarliestDigiDollarActivationHeight), the "digidollar" prune lock
 * (src/node/chainstate.cpp), and the startup fail-closed guards
 * (src/digidollar/health.cpp, src/oracle/bundle_manager.cpp) — these must
 * never disagree, or a pruned node could delete a block validation still
 * needs to read.
 *
 * Returns 0 when the deployment can never activate (NEVER_ACTIVE start or
 * timeout); callers treat 0 as "no floor" (no prune lock, no fail-closed
 * window, pre-floor gates disabled).
 */
int EarliestActivationFloor(const Consensus::Params& params);

/**
 * Core consensus parameters for the DigiDollar stablecoin system.
 * These parameters define the economic model, collateral requirements,
 * oracle configuration, and protection mechanisms.
 */
struct ConsensusParams {
    // Collateral ratios (higher for shorter periods - treasury model)
    // Map of lock time in blocks to collateral ratio percentage
    // Note: Sorted by lock time (shortest first)
    std::map<int64_t, int> collateralRatios = {
        {240, 1000},                       // 1 hour: 1000% (testing/onboarding tier)
        {30 * 24 * 60 * 4, 500},           // 30 days: 500%
        {90 * 24 * 60 * 4, 400},           // 3 months: 400%
        {180 * 24 * 60 * 4, 350},          // 6 months: 350%
        {365 * 24 * 60 * 4, 300},          // 1 year: 300%
        {2 * 365 * 24 * 60 * 4, 275},      // 2 years: 275%
        {3 * 365 * 24 * 60 * 4, 250},      // 3 years: 250%
        {5 * 365 * 24 * 60 * 4, 225},      // 5 years: 225%
        {7 * 365 * 24 * 60 * 4, 212},      // 7 years: 212%
        {10 * 365 * 24 * 60 * 4, 200}      // 10 years: 200%
    };

    // Transaction limits (amounts in cents: 100 cents = $1.00)
    CAmount minMintAmount = 10000;             // $100 minimum (10000 cents)
    CAmount maxMintAmount = 10000000;          // $100k maximum per tx (10000000 cents)
    CAmount minOutputAmount = 100;             // $1 minimum output (100 cents)

    // Activation heights for rule changes
    int minMintAmountActivationHeight = 0;     // Height at which minMintAmount rule activates

    // Oracle configuration
    uint32_t oracleCount = 35;                 // Total configured oracle slots
    uint32_t activeOracles = 35;               // Active oracle keys in the current roster
    uint32_t oracleThreshold = 7;              // 7 signatures required
    uint32_t priceValidBlocks = 20;            // 5 minutes at 15s blocks

    // Protection mechanisms
    uint32_t volatilityThreshold = 20;         // 20% triggers DCA
    uint32_t emergencyThreshold = 100;         // 100% collateral triggers ERR

    // System health thresholds for DCA (Dynamic Collateral Adjustment)
    struct DCALevel {
        int systemCollateral;   // System-wide collateral %
        int multiplier;         // Collateral requirement multiplier
    };

    std::vector<DCALevel> dcaLevels = {
        {150, 100},  // >150%: Normal (100% of base requirement)
        {120, 125},  // 120-150%: +25% collateral required
        {110, 150},  // 110-120%: +50% collateral required
        {100, 200},  // <110%: +100% collateral required
    };
};

// Helper functions for consensus parameter operations

/**
 * Get collateral ratio for a given lock time in blocks.
 * Returns the appropriate collateral ratio percentage based on lock period.
 * Returns 0 if lockBlocks is not one of the canonical lock tiers.
 */
int GetCollateralRatioForLockTime(int64_t lockBlocks, const ConsensusParams& params);

/**
 * Check if a lock period is one of the exact consensus lock tiers.
 */
bool IsCanonicalLockTier(int64_t lockBlocks, const ConsensusParams& params);

/**
 * Get DCA multiplier based on system health.
 * Returns the collateral requirement multiplier (100 = 100% = no change).
 * Lower system health requires higher collateral.
 */
double GetDCAMultiplier(int systemCollateral, const ConsensusParams& params);

/**
 * Check if amount is valid for minting operations.
 * Validates against minimum and maximum mint amounts.
 */
bool IsValidMintAmount(CAmount amount, const ConsensusParams& params);

/**
 * Get minimum DD output amount.
 * Returns the minimum amount for DigiDollar outputs.
 */
CAmount GetMinimumDDOutput(const ConsensusParams& params);

/**
 * Convert lock days to blocks using DigiByte's 15-second block time.
 * Utility function for converting human-readable lock periods.
 */
int64_t LockDaysToBlocks(int days);

/**
 * Convert blocks to approximate days.
 * Utility function for displaying lock periods to users.
 */
int BlocksToLockDays(int64_t blocks);

/**
 * Validate consensus parameters for sanity checks.
 * Ensures all parameters are within reasonable ranges and consistent.
 */
bool ValidateConsensusParams(const ConsensusParams& params, std::string& strError);

/**
 * Check if DigiDollar is active at given height.
 * Determines if DigiDollar functionality should be available.
 * @deprecated Use IsDigiDollarEnabled instead for BIP9 deployment checking
 */
bool IsDigiDollarActive(int nHeight, const Consensus::Params& consensusParams);

// Note: IsDigiDollarEnabled() functions are in digidollar/digidollar.h
// (not here) as they require deployment checking which is not part of consensus library

/**
 * Get the tier index for a lock period.
 * Returns the index in the collateral ratios map for the given lock period.
 * Returns -1 if lockBlocks is not an exact canonical tier.
 */
int GetLockTierIndex(int64_t lockBlocks, const ConsensusParams& params);

/**
 * Format lock period for display.
 * Converts block count to human-readable format (e.g., "30 days", "1 year").
 */
std::string FormatLockPeriod(int64_t lockBlocks);

/**
 * Check if transaction has DigiDollar marker in version field
 */
bool HasDigiDollarMarker(const CTransaction& tx);

/**
 * Extract DigiDollar transaction type from version field
 */
DigiDollarTxType GetDigiDollarTxType(const CTransaction& tx);

// Note: IsDDTokenScript() and ExtractDDAmount() are declared in
// src/digidollar/validation.h as they require CScript methods not
// available in the consensus library

} // namespace DigiDollar

#endif // DIGIBYTE_CONSENSUS_DIGIDOLLAR_H
