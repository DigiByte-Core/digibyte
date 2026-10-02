// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_CONSENSUS_VOLATILITY_H
#define DIGIBYTE_CONSENSUS_VOLATILITY_H

#include <consensus/amount.h>
#include <uint256.h>
#include <sync.h>

#include <vector>
#include <deque>
#include <cstdint>
#include <functional>
#include <string>

// Forward declarations
class COraclePriceMessage;
class CBlockIndex;
namespace Consensus { struct Params; }

namespace DigiDollar {
namespace Volatility {

/** The reference is bound to one candidate parent; it never stores an allow/reject answer. */
static constexpr int MINT_REFERENCE_MIN_DEPTH{240};
static constexpr int MINT_REFERENCE_MAX_DEPTH{1440};
static constexpr size_t MINT_REFERENCE_SAMPLES{15};
static constexpr int MINT_REFERENCE_RULE_VERSION{1};
static constexpr int64_t MINT_MAX_DEVIATION_BPS{2000};

enum class AncestorPriceResult { NO_BUNDLE, PRICE, UNAVAILABLE };
using AncestorPriceReader = std::function<AncestorPriceResult(const CBlockIndex&, CAmount&, std::string&)>;

struct MintReference {
    bool ready{false};
    int candidate_height{0};
    uint256 parent_hash;
    uint256 genesis_hash;
    int rules_version{MINT_REFERENCE_RULE_VERSION};
    CAmount price_micro_usd{0};
    size_t sample_count{0};
    int window_start_height{0};
    int window_end_height{-1};
    std::string error;
};

struct MintPriceStatus {
    bool ready{false};
    bool quote_available{false};
    bool restricted{false};
    int64_t deviation_bps{0}; //!< Display only; consensus compares the unrounded wide products.
    std::string reason;
};

MintReference BuildMintReference(int candidate_height, const CBlockIndex* parent,
                                const Consensus::Params& params, const AncestorPriceReader& read_price);
MintPriceStatus EvaluateMintPrice(CAmount candidate_price_micro_usd, const MintReference& reference);

// ============================================================================
// Data Structures
// ============================================================================

/** Price data point for volatility calculations */
struct PricePoint {
    CAmount price;         //!< Price in hundredths (e.g., 50000 = $500.00)
    int64_t timestamp;     //!< Unix timestamp
    uint32_t height;       //!< Block height when recorded

    PricePoint() : price(0), timestamp(0), height(0) {}
    PricePoint(CAmount p, int64_t t, uint32_t h) : price(p), timestamp(t), height(h) {}
};

/** Current volatility state */
struct VolatilityState {
    int64_t hourlyVolatilityBps;    //!< 1-hour volatility in basis points
    int64_t dailyVolatilityBps;     //!< 24-hour volatility in basis points
    int64_t weeklyVolatilityBps;    //!< 7-day volatility in basis points
    double hourlyVolatility;      //!< 1-hour volatility percentage
    double dailyVolatility;       //!< 24-hour volatility percentage
    double weeklyVolatility;      //!< 7-day volatility percentage
    bool mintingFrozen;           //!< True if new minting is frozen
    bool allOperationsFrozen;     //!< True if all DD operations are frozen
    uint32_t freezeHeight;        //!< Height when freeze was triggered
    uint32_t cooldownEndHeight;   //!< Height when cooldown period ends

    VolatilityState() :
        hourlyVolatilityBps(0),
        dailyVolatilityBps(0),
        weeklyVolatilityBps(0),
        hourlyVolatility(0.0),
        dailyVolatility(0.0),
        weeklyVolatility(0.0),
        mintingFrozen(false),
        allOperationsFrozen(false),
        freezeHeight(0),
        cooldownEndHeight(0) {}
};

/** Volatility thresholds for triggering freeze mechanisms */
struct VolatilityThresholds {
    static constexpr int64_t WARNING_1H_BPS = 1000;          //!< 10% in 1 hour: warning
    static constexpr int64_t FREEZE_MINT_1H_BPS = 2000;      //!< 20% in 1 hour: freeze new mints
    static constexpr int64_t FREEZE_ALL_24H_BPS = 3000;      //!< 30% in 24 hours: freeze all operations
    static constexpr int64_t EMERGENCY_7D_BPS = 5000;        //!< 50% in 7 days: emergency mode

    static constexpr double WARNING_1H = WARNING_1H_BPS / 100.0;
    static constexpr double FREEZE_MINT_1H = FREEZE_MINT_1H_BPS / 100.0;
    static constexpr double FREEZE_ALL_24H = FREEZE_ALL_24H_BPS / 100.0;
    static constexpr double EMERGENCY_7D = EMERGENCY_7D_BPS / 100.0;

    static constexpr uint32_t COOLDOWN_BLOCKS = 8640;   //!< Cooldown period in blocks (8640 × 15s = 36 hours)
};

// ============================================================================
// Volatility Monitor Class
// ============================================================================

/**
 * Monitors price volatility and manages freeze mechanisms for DigiDollar system
 *
 * This class tracks price history, calculates volatility metrics, and determines
 * when operations should be frozen due to excessive price volatility.
 */
class VolatilityMonitor {
private:
    static RecursiveMutex cs_volatility;  //!< Protects all static data

    static std::deque<PricePoint> priceHistory;   //!< Price history (max 30 days)
    static VolatilityState currentState;          //!< Current volatility state
    static uint32_t lastUpdateHeight;             //!< Last height state was updated

    // Constants
    static constexpr size_t MAX_HISTORY_DAYS = 30;           //!< Maximum days of history to keep
    static constexpr size_t MAX_HISTORY_POINTS = 30 * 24;    //!< Maximum price points (hourly for 30 days)
    static constexpr int64_t MIN_PRICE_INTERVAL = 3600;      //!< Minimum seconds between price updates

    // Internal helper methods
    static void UpdateVolatilityState() EXCLUSIVE_LOCKS_REQUIRED(cs_volatility);
    static void CleanOldHistory() EXCLUSIVE_LOCKS_REQUIRED(cs_volatility);
    static std::vector<PricePoint> GetPricesInWindow(int64_t timeWindow) EXCLUSIVE_LOCKS_REQUIRED(cs_volatility);
    static int64_t CalculateStandardDeviationBps(const std::vector<int64_t>& values);

public:
    // ========================================================================
    // Price History Management
    // ========================================================================

    /**
     * Record a new price point in the volatility monitoring system
     * @param price Price in hundredths (e.g., 50000 = $500.00)
     * @param timestamp Unix timestamp of the price
     * @param height Block height (optional, uses current chain tip if 0)
     */
    static void RecordPrice(CAmount price, int64_t timestamp, uint32_t height = 0);

    /**
     * Get complete price history
     * @return Vector of all recorded price points
     */
    static std::vector<PricePoint> GetPriceHistory();

    /**
     * Check whether a candidate price would cross the mint freeze threshold
     * without mutating volatility history or freeze state.
     */
    static bool WouldCandidateFreezeMinting(CAmount price);

    /**
     * Clear all price history (primarily for testing)
     */
    static void ClearHistory();

    // ========================================================================
    // Volatility Calculations
    // ========================================================================

    /**
     * Calculate volatility for a specific time window
     * @param timeWindow Time window in seconds (e.g., 3600 for 1 hour)
     * @return Volatility as a percentage (e.g., 15.5 for 15.5%)
     */
    static double CalculateVolatility(int64_t timeWindow);

    /**
     * Calculate volatility for a specific time window using deterministic
     * integer basis points. Consensus-visible freeze decisions must use this
     * path rather than floating point percentages.
     * @param timeWindow Time window in seconds
     * @return Volatility in basis points (100 bps = 1%)
     */
    static int64_t CalculateVolatilityBps(int64_t timeWindow);

    /**
     * Get current volatility state
     * @return Current VolatilityState with all metrics
     */
    static VolatilityState GetCurrentState();

    /**
     * Update volatility calculations based on current price history
     * Should be called periodically (e.g., each block)
     */
    static void UpdateState(uint32_t currentHeight);

    // ========================================================================
    // Freeze Mechanism Controls
    // ========================================================================

    /**
     * Check if new minting should be frozen
     * @return True if minting should be frozen due to volatility
     */
    static bool ShouldFreezeMinting();

    /**
     * Check if all DigiDollar operations should be frozen
     * @return True if all operations should be frozen due to volatility
     */
    static bool ShouldFreezeAll();

    /**
     * Check if currently in cooldown period after a freeze
     * @return True if in cooldown period
     */
    static bool InCooldownPeriod();

    /**
     * Get the block height when cooldown period ends
     * @return Block height when cooldown expires (0 if not in cooldown)
     */
    static uint32_t GetCooldownEndHeight();

    // ========================================================================
    // Override Mechanism
    // ========================================================================


    /**
     * Manually trigger freeze (for testing or emergency)
     * @param freezeAll True to freeze all operations, false for minting only
     * @param height Block height when freeze is triggered
     */
    static void TriggerFreeze(bool freezeAll, uint32_t height);

    /**
     * Manually clear freeze state (for testing or after manual intervention)
     */
    static void ClearFreeze();

    /**
     * Reconstruct volatility state from block price data after restart
     * Re-feeds saved price history and recalculates freeze state
     * @param blockPrices Price points extracted from recent blocks
     * @param currentHeight Current chain height
     */
    static void ReconstructFromBlockData(const std::vector<PricePoint>& blockPrices, uint32_t currentHeight);

    /**
     * Remove volatility price points for a disconnected block.
     * @param height Disconnected block height
     */
    static void RemovePriceForHeight(uint32_t height);

    // ========================================================================
    // Diagnostic Functions
    // ========================================================================

    /**
     * Get detailed volatility metrics for diagnostics
     * @return String with detailed volatility information
     */
    static std::string GetDiagnosticInfo();

    /**
     * Check if volatility monitoring is properly initialized
     * @return True if system is ready for volatility monitoring
     */
    static bool IsInitialized();

    /**
     * Get the age of the most recent price data
     * @return Seconds since last price update (0 if no data)
     */
    static int64_t GetDataAge();
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Convert volatility percentage to human-readable string
 * @param volatility Volatility as percentage
 * @return Formatted string (e.g., "15.2%")
 */
std::string FormatVolatility(double volatility);

/**
 * Calculate percentage change between two prices
 * @param oldPrice Previous price
 * @param newPrice Current price
 * @return Percentage change (positive or negative)
 */
double CalculatePercentageChange(CAmount oldPrice, CAmount newPrice);

/**
 * Calculate percentage change between two prices in basis points.
 * @param oldPrice Previous price
 * @param newPrice Current price
 * @return Percentage change in basis points (100 bps = 1%)
 */
int64_t CalculatePercentageChangeBps(CAmount oldPrice, CAmount newPrice);

/**
 * Check if a price change exceeds a threshold
 * @param oldPrice Previous price
 * @param newPrice Current price
 * @param threshold Threshold percentage
 * @return True if change exceeds threshold
 */
bool ExceedsThreshold(CAmount oldPrice, CAmount newPrice, double threshold);

/**
 * Check if a price change exceeds an integer basis-point threshold.
 * @param oldPrice Previous price
 * @param newPrice Current price
 * @param thresholdBps Threshold in basis points
 * @return True if change exceeds threshold
 */
bool ExceedsThresholdBps(CAmount oldPrice, CAmount newPrice, int64_t thresholdBps);

} // namespace Volatility
} // namespace DigiDollar

#endif // DIGIBYTE_CONSENSUS_VOLATILITY_H
