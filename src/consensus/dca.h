// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_CONSENSUS_DCA_H
#define DIGIBYTE_CONSENSUS_DCA_H

#include <consensus/amount.h>

#include <cstdint>
#include <string>
#include <vector>

namespace DigiDollar {
namespace DCA {

/**
 * DCA health tier definition.
 * Each tier defines a range of system collateralization percentages
 * and the corresponding collateral multiplier for new mints.
 */
struct HealthTier {
    int minCollateral;     // Minimum system collateral % for this tier
    int maxCollateral;     // Maximum system collateral % for this tier
    int multiplierBps;     // DCA multiplier in basis points (10000 = 1.0x)
    double multiplier;     // DCA multiplier for this tier (1.0 = no adjustment)
    std::string status;    // Human-readable status: "healthy", "warning", "critical", "emergency"

    HealthTier(int min, int max, int multBps, const std::string& stat)
        : minCollateral(min), maxCollateral(max), multiplierBps(multBps),
          multiplier(multBps / 10000.0), status(stat) {}
};

/**
 * Dynamic Collateral Adjustment (DCA) System
 *
 * The DCA system automatically adjusts collateral requirements for new DigiDollar
 * mints based on the overall system health. When the system becomes under-collateralized,
 * the DCA increases collateral requirements to protect the stability of the stablecoin.
 *
 * System Health Calculation:
 * health = (total_collateral_value_usd / total_dd_supply_usd) * 100
 *
 * Health Tiers:
 * - Healthy (>150%): 1.0x multiplier (no additional collateral)
 * - Warning (120-150%): 1.2x multiplier (+20% collateral required)
 * - Critical (100-120%): 1.5x multiplier (+50% collateral required)
 * - Emergency (<100%): 2.0x multiplier (double collateral required)
 */
class DynamicCollateralAdjustment {
private:
    // DCA health tiers (sorted from highest to lowest health)
    static const std::vector<HealthTier> HEALTH_TIERS;

public:
    /**
     * Calculate current system health as a percentage.
     *
     * @param totalCollateral Total DGB locked as collateral across all mints
     * @param totalDD Total DigiDollar supply in circulation (in cents)
     * @param oraclePrice Current DGB/USD price from oracle (in cents per DGB)
     * @return System health percentage (0-30000, where 100 = 100% collateralized)
     *
     * Formula: health = (totalCollateral * oraclePrice) / totalDD * 100
     *
     * Special cases:
     * - If totalDD is 0: returns 30000 (maximum health, no liabilities)
     * - If oraclePrice is 0: returns 0 (cannot function without price feed)
     * - If totalCollateral is 0: returns 0 (no collateral backing)
     */
    static int CalculateSystemHealth(CAmount totalCollateral,
                                    CAmount totalDD,
                                    CAmount oraclePrice);

    /**
     * Get DCA multiplier based on system health.
     *
     * @param systemHealth Current system health percentage
     * @return Collateral multiplier (1.0 = no adjustment, 2.0 = double collateral)
     *
     * Health ranges and multipliers:
     * - >150%: 1.0x (healthy system)
     * - 120-150%: 1.2x (warning level)
     * - 100-120%: 1.5x (critical level)
     * - <100%: 2.0x (emergency level)
     */
    static double GetDCAMultiplier(int systemHealth);

    /**
     * Get DCA multiplier in basis points for consensus-safe integer math.
     *
     * @param systemHealth Current system health percentage
     * @return Collateral multiplier in basis points (10000 = 1.0x)
     */
    static int GetDCAMultiplierBps(int systemHealth);

    /**
     * Apply DCA adjustment to base collateral ratio.
     *
     * @param baseRatio Base collateral ratio percentage (e.g., 300 for 300%)
     * @param systemHealth Current system health percentage
     * @return Adjusted collateral ratio with DCA applied
     *
     * Example: 300% base ratio with 1.5x DCA multiplier = 450% final ratio
     */
    static int ApplyDCA(int baseRatio, int systemHealth);

    /** Apply the same integer formula to an already verified candidate health. */
    static int ApplyDCAForHealth(int baseRatio, int systemHealth);

    /**
     * Get current health tier information.
     *
     * @param systemHealth Current system health percentage
     * @return HealthTier structure with details about current tier
     */
    static HealthTier GetCurrentTier(int systemHealth);

    /**
     * Check if system is in emergency state.
     * Emergency state triggers additional protections like ERR.
     *
     * @param systemHealth Current system health percentage
     * @return True if system health is below emergency threshold (<100%)
     */
    static bool IsSystemEmergency(int systemHealth);

    /**
     * Get total system collateral from UTXO set.
     * Scans the UTXO set for all DigiDollar collateral outputs.
     *
     * @return Total DGB amount locked as collateral across all active mints
     *
     * Note: This is an expensive operation that scans the UTXO set.
     * Results should be cached for performance.
     */
    static CAmount GetTotalSystemCollateral();

    /**
     * Get total DigiDollar supply in circulation.
     * Calculates the total amount of DigiDollars that have been minted
     * and not yet redeemed.
     *
     * @return Total DigiDollar supply in cents (1 DD = 100 cents)
     *
     * Note: This is an expensive operation that may scan the UTXO set.
     * Results should be cached for performance.
     */
    static CAmount GetTotalDDSupply();

    /**
     * Get real-time system health using current chain state.
     * Convenience function that fetches current collateral, supply, and price.
     *
     * @return Current system health percentage, or -1 if oracle unavailable
     *
     * IMPORTANT: Returns -1 when oracle price is not available.
     * Callers MUST check for -1 and block operations (mint/redeem) when oracle is down.
     * There are NO fallback prices - the oracle is required for all DigiDollar operations.
     */
    static int GetCurrentSystemHealth();

    /**
     * Check if the oracle system is available and has a valid price.
     *
     * @return True if oracle price is available, false otherwise
     *
     * When this returns false, all DigiDollar operations (mint, redeem) must be blocked.
     */
    static bool IsOracleAvailable();

    /**
     * Get real-time DCA multiplier using current chain state.
     * Convenience function for getting current DCA adjustment.
     *
     * @return Current DCA multiplier
     */
    static double GetCurrentDCAMultiplier();

    /**
     * Validate DCA configuration parameters.
     * Ensures health tiers are properly configured and non-overlapping.
     *
     * @param error Output parameter for error message if validation fails
     * @return True if DCA configuration is valid
     */
    static bool ValidateDCAConfig(std::string& error);

    /**
     * Format system health for display.
     * Converts health percentage to human-readable format.
     *
     * @param systemHealth Health percentage
     * @return Formatted string (e.g., "150.5%")
     */
    static std::string FormatSystemHealth(int systemHealth);

    /**
     * Format DCA multiplier for display.
     * Converts multiplier to human-readable format.
     *
     * @param multiplier DCA multiplier
     * @return Formatted string (e.g., "1.2x", "2.0x")
     */
    static std::string FormatDCAMultiplier(double multiplier);

    // ============================================================================
    // Extreme Scenario Testing Functions (for RED phase tests)
    // ============================================================================

    /**
     * Handle rapid transition between health tiers.
     * Provides stability during volatile conditions.
     *
     * @param health1 Initial health
     * @param health2 Intermediate health
     * @param health3 Final health
     * @return True if transitions handled properly
     */
    static bool HandleRapidTransition(int health1, int health2, int health3);

    /**
     * Validate extreme health values are handled correctly.
     *
     * @param zeroHealth Zero or negative health value
     * @param extremeHealth Very high health value
     * @return True if extreme values handled properly
     */
    static bool ValidateExtremeValues(int zeroHealth, int extremeHealth);

    /**
     * Validate multiplier precision at boundaries.
     *
     * @return True if precision is maintained
     */
    static bool ValidateMultiplierPrecision();

    /**
     * Prevent integer overflow in calculations.
     *
     * @param baseRatio Base ratio value
     * @param multiplier Multiplier to apply
     * @return True if overflow prevented
     */
    static bool PreventIntegerOverflow(int baseRatio, double multiplier);

    /**
     * Handle concurrent updates to health calculations.
     *
     * @param healthChanges Vector of rapid health changes
     * @return True if concurrent updates handled safely
     */
    static bool HandleConcurrentUpdates(const std::vector<int>& healthChanges);

    /**
     * Verify memory stability under load.
     *
     * @return True if memory usage is stable
     */
    static bool VerifyMemoryStability();

    /**
     * Validate error handling for edge cases.
     *
     * @param negativeHealth Negative health value to test
     * @return True if errors handled properly
     */
    static bool ValidateErrorHandling(int negativeHealth);

    /**
     * Track state transitions between tiers.
     *
     * @param fromStatus Initial tier status
     * @param toStatus Final tier status
     * @return True if transition is tracked
     */
    static bool IsStateTransitionTracked(const std::string& fromStatus, const std::string& toStatus);

    /**
     * Check for hysteresis in multiplier calculations.
     *
     * @param multipliers Vector of multiplier values
     * @return True if hysteresis is implemented
     */
    static bool HasHysteresis(const std::vector<double>& multipliers);

    /**
     * Track system recovery metrics.
     *
     * @param recoveryPath Vector of health values during recovery
     * @return True if recovery is tracked
     */
    static bool TrackSystemRecovery(const std::vector<int>& recoveryPath);

    /**
     * Validate concurrent calculation safety.
     *
     * @param adjustedRatios Results from concurrent calculations
     * @return True if calculations are thread-safe
     */
    static bool ValidateConcurrentCalculations(const std::vector<int>& adjustedRatios);

    /**
     * Simulate resource exhaustion conditions.
     *
     * @return True if resource exhaustion is simulated
     */
    static bool SimulateResourceExhaustion();
};

} // namespace DCA
} // namespace DigiDollar

#endif // DIGIBYTE_CONSENSUS_DCA_H
