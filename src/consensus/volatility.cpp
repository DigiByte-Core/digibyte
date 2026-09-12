// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/volatility.h>
#include <chain.h>
#include <consensus/params.h>
#include <consensus/digidollar.h>
#include <primitives/oracle.h>
#include <logging.h>
#include <util/time.h>
#include <util/moneystr.h>
#include <limits>
#include <hash.h>
#include <pubkey.h>
#include <serialize.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <set>

namespace DigiDollar {
namespace Volatility {

namespace {

int64_t SaturatingInt64(__int128 value)
{
    if (value > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if (value < static_cast<__int128>(std::numeric_limits<int64_t>::min())) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(value);
}

uint64_t IntegerSqrtFloor(unsigned __int128 value)
{
    uint64_t low = 0;
    uint64_t high = std::numeric_limits<uint64_t>::max();
    while (low < high) {
        const uint64_t mid = low + (high - low) / 2 + 1;
        const unsigned __int128 square = static_cast<unsigned __int128>(mid) * mid;
        if (square <= value) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return low;
}

int64_t AbsBps(int64_t value)
{
    if (value == std::numeric_limits<int64_t>::min()) {
        return std::numeric_limits<int64_t>::max();
    }
    return value < 0 ? -value : value;
}

double BpsToPercent(int64_t bps)
{
    return static_cast<double>(bps) / 100.0;
}

double CalculateStandardDeviationDouble(const std::vector<double>& values)
{
    if (values.size() < 2) {
        return 0.0;
    }

    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    double mean = sum / values.size();

    double squaredDiffSum = 0.0;
    for (double value : values) {
        double diff = value - mean;
        squaredDiffSum += diff * diff;
    }

    double variance = squaredDiffSum / (values.size() - 1);
    return std::sqrt(variance);
}

} // namespace

MintReference BuildMintReference(int candidate_height, const CBlockIndex* parent,
                                const Consensus::Params& params, const AncestorPriceReader& read_price)
{
    MintReference result;
    result.candidate_height = candidate_height;
    result.genesis_hash = params.hashGenesisBlock;
    if (candidate_height < 0 || (parent ? parent->nHeight != candidate_height - 1 : candidate_height != 0)) {
        result.error = "DigiDollar volatility state not ready: candidate parent is unavailable or mismatched";
        return result;
    }
    if (parent) result.parent_hash = parent->GetBlockHash();
    result.window_start_height = std::max(0, candidate_height - MINT_REFERENCE_MAX_DEPTH);
    result.window_end_height = candidate_height - MINT_REFERENCE_MIN_DEPTH;
    const int first_dd_height = params.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);
    const int oldest = std::max(result.window_start_height, first_dd_height);
    std::vector<CAmount> prices;
    prices.reserve(MINT_REFERENCE_SAMPLES);
    if (result.window_end_height >= oldest) {
        const CBlockIndex* ancestor = parent;
        while (ancestor && ancestor->nHeight > result.window_end_height) ancestor = ancestor->pprev;
        for (int height = result.window_end_height; height >= oldest && prices.size() < MINT_REFERENCE_SAMPLES; --height) {
            if (!ancestor || ancestor->nHeight != height || !read_price) {
                result.error = "DigiDollar volatility state not ready: required ancestor data is unavailable";
                return result;
            }
            CAmount price{0};
            const auto status = read_price(*ancestor, price, result.error);
            if (status == AncestorPriceResult::UNAVAILABLE || (status == AncestorPriceResult::PRICE && price <= 0)) {
                if (result.error.empty()) result.error = "DigiDollar volatility state not ready: required ancestor price is unavailable";
                return result;
            }
            if (status == AncestorPriceResult::PRICE) prices.push_back(price);
            ancestor = ancestor->pprev;
        }
    }
    result.sample_count = prices.size();
    if (!prices.empty()) {
        std::sort(prices.begin(), prices.end());
        result.price_micro_usd = prices[(prices.size() - 1) / 2];
    }
    result.ready = true;
    return result;
}

MintPriceStatus EvaluateMintPrice(CAmount candidate_price_micro_usd, const MintReference& reference)
{
    MintPriceStatus result;
    result.quote_available = candidate_price_micro_usd > 0;
    result.ready = reference.ready;
    if (!result.quote_available) {
        result.reason = "oracle_unavailable";
    } else if (!reference.ready || reference.rules_version != MINT_REFERENCE_RULE_VERSION) {
        result.ready = false;
        result.reason = "volatility_state_not_ready";
    } else if (reference.sample_count == 0) {
        result.reason = "none";
    } else if (reference.price_micro_usd <= 0 || reference.sample_count > MINT_REFERENCE_SAMPLES) {
        result.ready = false;
        result.reason = "volatility_state_not_ready";
    } else {
        // Positive int64 operands and these fixed multipliers fit in signed 128 bits.
        // Widen before subtraction or multiplication; equality at 20% rejects.
        const __int128 reference_price = reference.price_micro_usd;
        const __int128 candidate_price = candidate_price_micro_usd;
        const __int128 difference = candidate_price >= reference_price ? candidate_price - reference_price : reference_price - candidate_price;
        const __int128 numerator = difference * 10000;
        result.restricted = numerator >= reference_price * MINT_MAX_DEVIATION_BPS;
        result.deviation_bps = SaturatingInt64(numerator / reference_price);
        result.reason = result.restricted ? "volatility_pause" : "none";
    }
    return result;
}

// ============================================================================
// Static Member Definitions
// ============================================================================

RecursiveMutex VolatilityMonitor::cs_volatility;
std::deque<PricePoint> VolatilityMonitor::priceHistory;
VolatilityState VolatilityMonitor::currentState;
uint32_t VolatilityMonitor::lastUpdateHeight = 0;

// ============================================================================
// VolatilityMonitor Implementation
// ============================================================================

void VolatilityMonitor::RecordPrice(CAmount price, int64_t timestamp, uint32_t height)
{
    LOCK(cs_volatility);

    if (price < static_cast<CAmount>(ORACLE_MIN_PRICE_MICRO_USD) ||
        price > static_cast<CAmount>(ORACLE_MAX_PRICE_MICRO_USD)) {
        LogPrintf("VolatilityMonitor: Rejecting invalid candidate price %lld\n",
                  static_cast<long long>(price));
        return;
    }

    // Use current time if timestamp is 0
    if (timestamp == 0) {
        timestamp = GetTime();
    }

    // Use provided height or 0 if not available
    // In full implementation, this would query the chain state

    // Check if we should record this price (avoid spam)
    if (!priceHistory.empty()) {
        const auto& lastPrice = priceHistory.back();

        // Don't record if too recent (< 1 hour interval)
        if (timestamp - lastPrice.timestamp < MIN_PRICE_INTERVAL) {
            LogPrint(BCLog::DIGIDOLLAR, "VolatilityMonitor: Skipping price update (too recent: %d seconds)\n",
                     timestamp - lastPrice.timestamp);
            return;
        }

    }

    // Add new price point
    priceHistory.emplace_back(price, timestamp, height);

    LogPrint(BCLog::DIGIDOLLAR, "VolatilityMonitor: Recorded price %s at height %d (timestamp %d)\n",
             FormatMoney(price), height, timestamp);

    // Clean old history to maintain size limits
    CleanOldHistory();

    // Update volatility state
    UpdateVolatilityState();
}

std::vector<PricePoint> VolatilityMonitor::GetPriceHistory()
{
    LOCK(cs_volatility);
    return std::vector<PricePoint>(priceHistory.begin(), priceHistory.end());
}

bool VolatilityMonitor::WouldCandidateFreezeMinting(CAmount price)
{
    LOCK(cs_volatility);

    if (price < static_cast<CAmount>(ORACLE_MIN_PRICE_MICRO_USD) ||
        price > static_cast<CAmount>(ORACLE_MAX_PRICE_MICRO_USD) ||
        priceHistory.empty()) {
        return false;
    }

    const int64_t candidateChangeBps = AbsBps(CalculatePercentageChangeBps(priceHistory.back().price, price));
    return candidateChangeBps >= VolatilityThresholds::FREEZE_MINT_1H_BPS;
}

void VolatilityMonitor::ClearHistory()
{
    LOCK(cs_volatility);
    priceHistory.clear();
    currentState = VolatilityState();
    lastUpdateHeight = 0;

    LogPrint(BCLog::DIGIDOLLAR, "VolatilityMonitor: History cleared\n");
}

double VolatilityMonitor::CalculateVolatility(int64_t timeWindow)
{
    LOCK(cs_volatility);

    if (priceHistory.size() < 2) {
        return 0.0;
    }

    auto windowPrices = GetPricesInWindow(timeWindow);
    if (windowPrices.size() < 2) {
        return 0.0;
    }

    CAmount startPrice = windowPrices.front().price;
    double maxAbsChange = 0.0;

    for (size_t i = 1; i < windowPrices.size(); ++i) {
        double change = std::abs(CalculatePercentageChange(startPrice, windowPrices[i].price));
        maxAbsChange = std::max(maxAbsChange, change);
    }

    std::vector<double> percentChanges;
    percentChanges.reserve(windowPrices.size() - 1);

    for (size_t i = 1; i < windowPrices.size(); ++i) {
        double change = CalculatePercentageChange(windowPrices[i-1].price, windowPrices[i].price);
        percentChanges.push_back(std::abs(change));
    }

    double stdDevVolatility = CalculateStandardDeviationDouble(percentChanges);
    return std::max(maxAbsChange, stdDevVolatility * 2.0);
}

int64_t VolatilityMonitor::CalculateVolatilityBps(int64_t timeWindow)
{
    LOCK(cs_volatility);

    if (priceHistory.size() < 2) {
        return 0;
    }

    // Get prices in the specified time window
    auto windowPrices = GetPricesInWindow(timeWindow);

    if (windowPrices.size() < 2) {
        return 0;
    }

    // Calculate volatility as the maximum absolute percentage change from the starting price
    // This better reflects the actual price movement for freeze thresholds
    CAmount startPrice = windowPrices.front().price;
    int64_t maxAbsChangeBps = 0;

    for (size_t i = 1; i < windowPrices.size(); ++i) {
        int64_t changeBps = AbsBps(CalculatePercentageChangeBps(startPrice, windowPrices[i].price));
        maxAbsChangeBps = std::max(maxAbsChangeBps, changeBps);
    }

    // Also calculate standard deviation for additional volatility metric
    std::vector<int64_t> percentChangeBps;
    percentChangeBps.reserve(windowPrices.size() - 1);

    for (size_t i = 1; i < windowPrices.size(); ++i) {
        int64_t changeBps = AbsBps(CalculatePercentageChangeBps(windowPrices[i-1].price, windowPrices[i].price));
        percentChangeBps.push_back(changeBps);
    }

    const int64_t stdDevVolatilityBps = CalculateStandardDeviationBps(percentChangeBps);
    const int64_t doubledStdDevBps = stdDevVolatilityBps > std::numeric_limits<int64_t>::max() / 2
        ? std::numeric_limits<int64_t>::max()
        : stdDevVolatilityBps * 2;

    // Return the maximum of: max absolute change and 2x standard deviation
    // This captures both large single moves and sustained volatility
    return std::max(maxAbsChangeBps, doubledStdDevBps);
}

VolatilityState VolatilityMonitor::GetCurrentState()
{
    LOCK(cs_volatility);
    return currentState;
}

void VolatilityMonitor::UpdateState(uint32_t currentHeight)
{
    LOCK(cs_volatility);

    if (priceHistory.empty()) {
        return;
    }

    lastUpdateHeight = currentHeight;

    // Calculate volatility for different time windows
    currentState.hourlyVolatilityBps = CalculateVolatilityBps(3600);      // 1 hour
    currentState.dailyVolatilityBps = CalculateVolatilityBps(24 * 3600);  // 24 hours
    currentState.weeklyVolatilityBps = CalculateVolatilityBps(7 * 24 * 3600); // 7 days
    currentState.hourlyVolatility = BpsToPercent(currentState.hourlyVolatilityBps);
    currentState.dailyVolatility = BpsToPercent(currentState.dailyVolatilityBps);
    currentState.weeklyVolatility = BpsToPercent(currentState.weeklyVolatilityBps);

    // Update freeze state based on thresholds
    bool shouldFreezeMint = (currentState.hourlyVolatilityBps >= VolatilityThresholds::FREEZE_MINT_1H_BPS);
    bool shouldFreezeAll = (currentState.dailyVolatilityBps >= VolatilityThresholds::FREEZE_ALL_24H_BPS) ||
                          (currentState.weeklyVolatilityBps >= VolatilityThresholds::EMERGENCY_7D_BPS);

    // Log warnings
    if (currentState.hourlyVolatilityBps >= VolatilityThresholds::WARNING_1H_BPS) {
        LogPrintf("VolatilityMonitor: WARNING - High volatility detected: 1h=%.2f%%, 24h=%.2f%%, 7d=%.2f%%\n",
                  currentState.hourlyVolatility, currentState.dailyVolatility, currentState.weeklyVolatility);
    }

    // Handle freeze state changes
    if (shouldFreezeAll && !currentState.allOperationsFrozen) {
        // Trigger all operations freeze
        currentState.allOperationsFrozen = true;
        currentState.mintingFrozen = true;
        currentState.freezeHeight = currentHeight;
        currentState.cooldownEndHeight = (currentHeight > std::numeric_limits<uint32_t>::max() - VolatilityThresholds::COOLDOWN_BLOCKS)
            ? std::numeric_limits<uint32_t>::max()
            : currentHeight + VolatilityThresholds::COOLDOWN_BLOCKS;

        LogPrintf("VolatilityMonitor: FREEZE - All DigiDollar operations frozen due to high volatility (%.2f%% daily, %.2f%% weekly)\n",
                  currentState.dailyVolatility, currentState.weeklyVolatility);

    } else if (shouldFreezeMint && !currentState.mintingFrozen) {
        // Trigger minting freeze only
        currentState.mintingFrozen = true;
        currentState.freezeHeight = currentHeight;
        currentState.cooldownEndHeight = (currentHeight > std::numeric_limits<uint32_t>::max() - VolatilityThresholds::COOLDOWN_BLOCKS)
            ? std::numeric_limits<uint32_t>::max()
            : currentHeight + VolatilityThresholds::COOLDOWN_BLOCKS;

        LogPrintf("VolatilityMonitor: FREEZE - DigiDollar minting frozen due to high volatility (%.2f%% hourly)\n",
                  currentState.hourlyVolatility);
    }

    // Check if we should unfreeze (only if not in cooldown and volatility is low)
    if ((currentState.mintingFrozen || currentState.allOperationsFrozen) &&
        currentHeight > currentState.cooldownEndHeight) {

        bool canUnfreeze = (currentState.hourlyVolatilityBps < VolatilityThresholds::WARNING_1H_BPS) &&
                          (currentState.dailyVolatilityBps < VolatilityThresholds::FREEZE_MINT_1H_BPS) &&
                          (currentState.weeklyVolatilityBps < VolatilityThresholds::FREEZE_ALL_24H_BPS);

        if (canUnfreeze) {
            LogPrintf("VolatilityMonitor: UNFREEZE - Volatility stabilized, unfreezing operations\n");
            currentState.mintingFrozen = false;
            currentState.allOperationsFrozen = false;
            currentState.freezeHeight = 0;
            currentState.cooldownEndHeight = 0;
        }
    }
}

bool VolatilityMonitor::ShouldFreezeMinting()
{
    LOCK(cs_volatility);
    return currentState.mintingFrozen;
}

bool VolatilityMonitor::ShouldFreezeAll()
{
    LOCK(cs_volatility);
    return currentState.allOperationsFrozen;
}

bool VolatilityMonitor::InCooldownPeriod()
{
    LOCK(cs_volatility);

    if (currentState.cooldownEndHeight == 0) {
        return false;
    }

    // In full implementation, this would check against current chain height
    // For now, return based on last known height
    return lastUpdateHeight <= currentState.cooldownEndHeight;
}

uint32_t VolatilityMonitor::GetCooldownEndHeight()
{
    LOCK(cs_volatility);
    return currentState.cooldownEndHeight;
}


void VolatilityMonitor::TriggerFreeze(bool freezeAll, uint32_t height)
{
    LOCK(cs_volatility);

    if (height == 0) {
        height = lastUpdateHeight; // Use last known height
    }

    currentState.mintingFrozen = true;
    currentState.allOperationsFrozen = freezeAll;
    currentState.freezeHeight = height;
    currentState.cooldownEndHeight = (height > std::numeric_limits<uint32_t>::max() - VolatilityThresholds::COOLDOWN_BLOCKS)
        ? std::numeric_limits<uint32_t>::max()
        : height + VolatilityThresholds::COOLDOWN_BLOCKS;

    LogPrintf("VolatilityMonitor: MANUAL FREEZE - %s frozen at height %d\n",
              freezeAll ? "All operations" : "Minting only", height);
}

void VolatilityMonitor::ClearFreeze()
{
    LOCK(cs_volatility);

    currentState.mintingFrozen = false;
    currentState.allOperationsFrozen = false;
    currentState.freezeHeight = 0;
    currentState.cooldownEndHeight = 0;

    LogPrintf("VolatilityMonitor: MANUAL UNFREEZE - All freeze states cleared\n");
}

void VolatilityMonitor::ReconstructFromBlockData(const std::vector<PricePoint>& blockPrices, uint32_t currentHeight)
{
    LOCK(cs_volatility);

    // Clear existing state
    priceHistory.clear();
    currentState = VolatilityState();
    lastUpdateHeight = 0;

    LogPrintf("VolatilityMonitor: Reconstructing state from %d block price points at height %d\n",
              blockPrices.size(), currentHeight);

    // Re-feed all price points (they're already sorted chronologically from blocks)
    for (const auto& pp : blockPrices) {
        priceHistory.emplace_back(pp);
    }

    // Update last known height
    lastUpdateHeight = currentHeight;

    // Clean old history to maintain size limits
    CleanOldHistory();

    // Recalculate volatility state from reconstructed history
    if (!priceHistory.empty()) {
        UpdateVolatilityState();
    }

    LogPrintf("VolatilityMonitor: Reconstruction complete - frozen=%d, mintFrozen=%d, pricePoints=%d\n",
              currentState.allOperationsFrozen, currentState.mintingFrozen, priceHistory.size());
}

void VolatilityMonitor::RemovePriceForHeight(uint32_t height)
{
    LOCK(cs_volatility);

    const auto old_size = priceHistory.size();
    priceHistory.erase(std::remove_if(priceHistory.begin(), priceHistory.end(),
                                      [height](const PricePoint& pp) { return pp.height == height; }),
                       priceHistory.end());
    if (priceHistory.size() == old_size) {
        return;
    }

    currentState = VolatilityState();
    lastUpdateHeight = priceHistory.empty() ? 0 : priceHistory.back().height;
    CleanOldHistory();
    if (!priceHistory.empty()) {
        UpdateVolatilityState();
    }

    LogPrint(BCLog::DIGIDOLLAR, "VolatilityMonitor: Removed price data for height %u, remaining=%d\n",
              height, priceHistory.size());
}

std::string VolatilityMonitor::GetDiagnosticInfo()
{
    LOCK(cs_volatility);

    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);

    ss << "=== DigiDollar Volatility Monitor Status ===\n";
    ss << "Price History Points: " << priceHistory.size() << "\n";

    if (!priceHistory.empty()) {
        ss << "Latest Price: " << FormatMoney(priceHistory.back().price)
           << " (height " << priceHistory.back().height << ")\n";
        ss << "Data Age: " << GetDataAge() << " seconds\n";
    }

    ss << "\nVolatility Metrics:\n";
    ss << "  1-hour:  " << FormatVolatility(currentState.hourlyVolatility) << "\n";
    ss << "  24-hour: " << FormatVolatility(currentState.dailyVolatility) << "\n";
    ss << "  7-day:   " << FormatVolatility(currentState.weeklyVolatility) << "\n";

    ss << "\nFreeze Status:\n";
    ss << "  Minting Frozen: " << (currentState.mintingFrozen ? "YES" : "NO") << "\n";
    ss << "  All Operations Frozen: " << (currentState.allOperationsFrozen ? "YES" : "NO") << "\n";

    if (currentState.freezeHeight > 0) {
        ss << "  Freeze Height: " << currentState.freezeHeight << "\n";
        ss << "  Cooldown End Height: " << currentState.cooldownEndHeight << "\n";
        ss << "  In Cooldown: " << (InCooldownPeriod() ? "YES" : "NO") << "\n";
    }

    ss << "\nThresholds:\n";
    ss << "  Warning (1h): " << VolatilityThresholds::WARNING_1H << "%\n";
    ss << "  Freeze Mint (1h): " << VolatilityThresholds::FREEZE_MINT_1H << "%\n";
    ss << "  Freeze All (24h): " << VolatilityThresholds::FREEZE_ALL_24H << "%\n";
    ss << "  Emergency (7d): " << VolatilityThresholds::EMERGENCY_7D << "%\n";

    return ss.str();
}

bool VolatilityMonitor::IsInitialized()
{
    LOCK(cs_volatility);
    return !priceHistory.empty();
}

int64_t VolatilityMonitor::GetDataAge()
{
    LOCK(cs_volatility);

    if (priceHistory.empty()) {
        return 0;
    }

    return GetTime() - priceHistory.back().timestamp;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void VolatilityMonitor::UpdateVolatilityState()
{
    // This is called from RecordPrice, which already holds the lock
    AssertLockHeld(cs_volatility);

    if (priceHistory.empty()) {
        return;
    }

    // Calculate volatility for different time windows
    currentState.hourlyVolatilityBps = CalculateVolatilityBps(3600);      // 1 hour
    currentState.dailyVolatilityBps = CalculateVolatilityBps(24 * 3600);  // 24 hours
    currentState.weeklyVolatilityBps = CalculateVolatilityBps(7 * 24 * 3600); // 7 days
    currentState.hourlyVolatility = BpsToPercent(currentState.hourlyVolatilityBps);
    currentState.dailyVolatility = BpsToPercent(currentState.dailyVolatilityBps);
    currentState.weeklyVolatility = BpsToPercent(currentState.weeklyVolatilityBps);

    // Get the most recent height from price history (if available)
    uint32_t currentHeight = priceHistory.empty() ? lastUpdateHeight : priceHistory.back().height;

    // Update freeze state based on thresholds
    bool shouldFreezeMint = (currentState.hourlyVolatilityBps >= VolatilityThresholds::FREEZE_MINT_1H_BPS);
    bool shouldFreezeAll = (currentState.dailyVolatilityBps >= VolatilityThresholds::FREEZE_ALL_24H_BPS) ||
                          (currentState.weeklyVolatilityBps >= VolatilityThresholds::EMERGENCY_7D_BPS);

    // Log warnings
    if (currentState.hourlyVolatilityBps >= VolatilityThresholds::WARNING_1H_BPS) {
        LogPrintf("VolatilityMonitor: WARNING - High volatility detected: 1h=%.2f%%, 24h=%.2f%%, 7d=%.2f%%\n",
                  currentState.hourlyVolatility, currentState.dailyVolatility, currentState.weeklyVolatility);
    }

    // Handle freeze state changes
    if (shouldFreezeAll && !currentState.allOperationsFrozen) {
        // Trigger all operations freeze
        currentState.allOperationsFrozen = true;
        currentState.mintingFrozen = true;
        currentState.freezeHeight = currentHeight;
        currentState.cooldownEndHeight = (currentHeight > std::numeric_limits<uint32_t>::max() - VolatilityThresholds::COOLDOWN_BLOCKS)
            ? std::numeric_limits<uint32_t>::max()
            : currentHeight + VolatilityThresholds::COOLDOWN_BLOCKS;

        LogPrintf("VolatilityMonitor: FREEZE - All DigiDollar operations frozen due to high volatility (%.2f%% daily, %.2f%% weekly)\n",
                  currentState.dailyVolatility, currentState.weeklyVolatility);

    } else if (shouldFreezeMint && !currentState.mintingFrozen) {
        // Trigger minting freeze only
        currentState.mintingFrozen = true;
        currentState.freezeHeight = currentHeight;
        currentState.cooldownEndHeight = (currentHeight > std::numeric_limits<uint32_t>::max() - VolatilityThresholds::COOLDOWN_BLOCKS)
            ? std::numeric_limits<uint32_t>::max()
            : currentHeight + VolatilityThresholds::COOLDOWN_BLOCKS;

        LogPrintf("VolatilityMonitor: FREEZE - DigiDollar minting frozen due to high volatility (%.2f%% hourly)\n",
                  currentState.hourlyVolatility);
    }

    // Check if we should unfreeze (only if not in cooldown and volatility is low)
    if ((currentState.mintingFrozen || currentState.allOperationsFrozen) &&
        currentHeight > currentState.cooldownEndHeight) {

        bool canUnfreeze = (currentState.hourlyVolatilityBps < VolatilityThresholds::WARNING_1H_BPS) &&
                          (currentState.dailyVolatilityBps < VolatilityThresholds::FREEZE_MINT_1H_BPS) &&
                          (currentState.weeklyVolatilityBps < VolatilityThresholds::FREEZE_ALL_24H_BPS);

        if (canUnfreeze) {
            LogPrintf("VolatilityMonitor: UNFREEZE - Volatility stabilized, unfreezing operations\n");
            currentState.mintingFrozen = false;
            currentState.allOperationsFrozen = false;
            currentState.freezeHeight = 0;
            currentState.cooldownEndHeight = 0;
        }
    }
}

void VolatilityMonitor::CleanOldHistory()
{
    AssertLockHeld(cs_volatility);

    if (priceHistory.empty()) {
        return;
    }

    // Use the most recent timestamp as reference, not current time
    // This ensures consistent behavior in tests and when replaying history
    const int64_t cutoffTime = priceHistory.back().timestamp - (MAX_HISTORY_DAYS * 24 * 3600);

    // Remove old entries
    while (!priceHistory.empty() && priceHistory.front().timestamp < cutoffTime) {
        priceHistory.pop_front();
    }

    // Also enforce maximum point limit
    while (priceHistory.size() > MAX_HISTORY_POINTS) {
        priceHistory.pop_front();
    }
}

std::vector<PricePoint> VolatilityMonitor::GetPricesInWindow(int64_t timeWindow)
{
    AssertLockHeld(cs_volatility);

    std::vector<PricePoint> windowPrices;

    if (priceHistory.empty()) {
        return windowPrices;
    }

    const int64_t cutoffTime = priceHistory.back().timestamp - timeWindow;

    // Find all prices within the time window
    for (auto it = priceHistory.rbegin(); it != priceHistory.rend(); ++it) {
        if (it->timestamp >= cutoffTime) {
            windowPrices.push_back(*it);
        } else {
            break; // Older than window
        }
    }

    // Reverse to get chronological order
    std::reverse(windowPrices.begin(), windowPrices.end());

    return windowPrices;
}

int64_t VolatilityMonitor::CalculateStandardDeviationBps(const std::vector<int64_t>& values)
{
    if (values.size() < 2) {
        return 0;
    }

    __int128 sum = 0;
    __int128 sumSquares = 0;
    for (int64_t value : values) {
        sum += value;
        sumSquares += static_cast<__int128>(value) * value;
    }

    const __int128 count = values.size();
    __int128 numerator = count * sumSquares - sum * sum;
    if (numerator <= 0) {
        return 0;
    }
    const __int128 denominator = count * (count - 1); // Sample variance
    const unsigned __int128 variance = static_cast<unsigned __int128>(numerator / denominator);
    const uint64_t sqrt = IntegerSqrtFloor(variance);
    if (sqrt > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(sqrt);
}


// ============================================================================
// Utility Functions
// ============================================================================

std::string FormatVolatility(double volatility)
{
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << volatility << "%";
    return ss.str();
}

double CalculatePercentageChange(CAmount oldPrice, CAmount newPrice)
{
    if (oldPrice == 0) {
        return 0.0;
    }

    return (static_cast<double>(newPrice - oldPrice) / static_cast<double>(oldPrice)) * 100.0;
}

int64_t CalculatePercentageChangeBps(CAmount oldPrice, CAmount newPrice)
{
    if (oldPrice <= 0) {
        return 0;
    }

    const __int128 delta = static_cast<__int128>(newPrice) - oldPrice;
    const __int128 scaled = delta * 10000;
    return SaturatingInt64(scaled / oldPrice);
}

bool ExceedsThreshold(CAmount oldPrice, CAmount newPrice, double threshold)
{
    if (oldPrice == 0) {
        return false;
    }

    double change = std::abs(CalculatePercentageChange(oldPrice, newPrice));
    return change >= threshold;
}

bool ExceedsThresholdBps(CAmount oldPrice, CAmount newPrice, int64_t thresholdBps)
{
    int64_t changeBps = AbsBps(CalculatePercentageChangeBps(oldPrice, newPrice));
    return changeBps >= thresholdBps;
}

} // namespace Volatility
} // namespace DigiDollar
