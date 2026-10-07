// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <digidollar/txbuilder.h>
#include <digidollar/scripts.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/validation.h>
#include <consensus/dca.h>
#include <consensus/digidollar.h>
#include <consensus/err.h>
#include <script/standard.h>
#include <validation.h>
#include <base58.h>
#include <random.h>
#include <policy/policy.h>
#include <logging.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <map>

// Forward declare to avoid namespace conflicts

namespace DigiDollar {

// Constants for mint transaction building
static const CAmount DUST_THRESHOLD = 1000;        // Minimum change output (1000 sats)
static const size_t ESTIMATED_TX_VSIZE = 500;      // Estimated transaction size in vB (increased for multiple inputs)
static const int DEFAULT_SYSTEM_COLLATERAL = 150;   // Default system health (150%)
static const double MAX_FEE_RATIO = 0.5;           // Maximum fee as ratio of total input
static const size_t MAX_TX_INPUTS = 400;           // Maximum inputs per transaction to stay under MAX_STANDARD_TX_WEIGHT
static const CAmount MIN_DD_TX_FEE = 10000000;     // 0.1 DGB minimum DD transaction fee

// A taproot output is OP_1 followed by a 32-byte key.
static bool IsTaprootOutputScript(const CScript& script)
{
    int witnessVersion = -1;
    std::vector<unsigned char> witnessProgram;
    return script.IsWitnessProgram(witnessVersion, witnessProgram) &&
           witnessVersion == 1 &&
           witnessProgram.size() == WITNESS_V1_TAPROOT_SIZE;
}

// Work out where the leftover DGB in a DigiDollar transaction should go.
//
// The caller names the address. The builder must never make one up. A key
// invented here belongs to no wallet, so DGB paid to it can never be spent
// again. When there is no address to use, the build stops and the caller
// reports the error instead of sending the money somewhere unrecoverable.
//
// Mints have one extra rule. In a mint the only taproot output allowed to hold
// DGB is the locked collateral. A taproot change output would be a second one
// and every node would reject the mint, so mints pass allowTaproot = false and
// send their change to an ordinary bech32 address.
static bool ResolveDGBChangeScript(const std::optional<CTxDestination>& changeDest,
                                   bool allowTaproot,
                                   CScript& scriptOut,
                                   std::string& errorOut)
{
    if (!changeDest.has_value() || !IsValidDestination(*changeDest)) {
        errorOut = "No wallet change address is available for the leftover DGB. "
                   "Unlock the wallet, or check that it can still hand out addresses, then try again.";
        return false;
    }

    const CScript script = GetScriptForDestination(*changeDest);
    if (script.empty()) {
        errorOut = "The wallet change address for the leftover DGB could not be used.";
        return false;
    }

    if (!allowTaproot && IsTaprootOutputScript(script)) {
        errorOut = "A mint cannot send its leftover DGB to a taproot address. "
                   "A mint may hold DGB in only one taproot output, the locked collateral, "
                   "so the change must go to an ordinary bech32 address.";
        return false;
    }

    scriptOut = script;
    return true;
}

// Work out where the collateral a redemption unlocks should go.
//
// This is the whole vault, so it matters more than anything else in the
// transaction. The caller names the address. The builder must never make one
// up. An address worked out here from the owner key is not tweaked the way a
// wallet's taproot addresses are, so no wallet watches it and no wallet can
// spend from it. When there is no address to use, the build stops and the
// caller reports the error instead of sending the vault somewhere it can never
// be spent from again.
static bool ResolveCollateralReturnScript(const std::optional<CTxDestination>& collateralDest,
                                          CScript& scriptOut,
                                          std::string& errorOut)
{
    if (!collateralDest.has_value() || !IsValidDestination(*collateralDest)) {
        errorOut = "No address is available for the collateral this redemption unlocks. "
                   "Unlock the wallet, or check that it can still hand out addresses, then try again.";
        return false;
    }

    const CScript script = GetScriptForDestination(*collateralDest);
    if (script.empty()) {
        errorOut = "The address for the collateral this redemption unlocks could not be used.";
        return false;
    }

    scriptOut = script;
    return true;
}

CAmount ApplyCollateralSafetyMargin(CAmount requiredCollateral)
{
    if (requiredCollateral <= 0) {
        return 0;
    }

    __int128 padded128 = (static_cast<__int128>(requiredCollateral) * 101) / 100;
    if (padded128 > static_cast<__int128>(MAX_MONEY)) {
        return 0;
    }
    return static_cast<CAmount>(padded128);
}

int CalculatePositionCollateralRatio(CAmount dgbLocked, CAmount ddMinted, CAmount price)
{
    if (dgbLocked <= 0 || ddMinted <= 0 || price <= 0) {
        return 0;
    }

    const __int128 dgbValueCents = (static_cast<__int128>(dgbLocked) * price) / COIN;
    const __int128 ratio = (dgbValueCents * 100) / ddMinted;
    if (ratio > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ratio);
}

// ============================================================================
// Base TxBuilder implementation
// ============================================================================

TxBuilder::TxBuilder(const CChainParams& params, int height, CAmount price)
    : chainParams(params), currentHeight(height), oraclePrice(price) {}

CAmount TxBuilder::CalculateFee(const CMutableTransaction& tx, CAmount feeRate) const {
    // Estimate transaction virtual size
    size_t vsize = EstimateTransactionVSize(tx);
    return (vsize * feeRate) / 1000;  // Convert sat/vB to total fee
}

bool TxBuilder::SelectCoins(const std::vector<COutPoint>& utxos, CAmount target,
                           std::vector<CTxIn>& inputs, CAmount& total) const {
    total = 0;
    inputs.clear();

    // Sort UTXOs by value (largest first) for optimal selection
    // This minimizes the number of inputs needed
    std::vector<std::pair<CAmount, COutPoint>> sortedUtxos;
    for (const auto& utxo : utxos) {
        CAmount value = GetUTXOValueVirtual(utxo);
        if (value > 0) {
            sortedUtxos.emplace_back(value, utxo);
        }
    }
    std::sort(sortedUtxos.begin(), sortedUtxos.end(), std::greater<>());

    // Greedy selection with input limit to prevent tx-size errors
    // MAX_TX_INPUTS prevents transactions from exceeding MAX_STANDARD_TX_WEIGHT (400,000)
    // Each input is ~41 bytes base + ~65 bytes witness = ~106 bytes = ~68 vB
    // 400 inputs * 68 vB = ~27,200 vB which is well under 100,000 vB limit
    for (const auto& [value, utxo] : sortedUtxos) {
        inputs.push_back(CTxIn(utxo));
        total += value;

        // Stop if we have enough
        if (total >= target) {
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: SelectCoins succeeded with %d inputs totaling %lld sats (target: %lld)\n",
                      inputs.size(), total, target);
            return true;
        }

        // CRITICAL: Stop if we've reached the input limit to prevent tx-size errors
        if (inputs.size() >= MAX_TX_INPUTS) {
            LogPrintf("DigiDollar: SelectCoins reached MAX_TX_INPUTS (%d) with total %lld sats (target: %lld)\n",
                      MAX_TX_INPUTS, total, target);
            if (total >= target) {
                return true;
            }
            // Not enough funds within input limit - user needs to consolidate UTXOs
            LogPrintf("DigiDollar: SelectCoins FAILED - insufficient funds within %d input limit. "
                      "Need %lld sats, have %lld. Please consolidate UTXOs first.\n",
                      MAX_TX_INPUTS, target, total);
            return false;
        }
    }

    LogPrintf("DigiDollar: SelectCoins FAILED - exhausted all UTXOs. Need %lld sats, have %lld\n",
              target, total);
    return false;
}

CAmount TxBuilder::GetUTXOValue(const COutPoint& outpoint) const {
    // This is a simplified implementation
    // In production, this would query the UTXO set
    // For now, return a reasonable value for testing
    return 100 * COIN; // 100 DGB placeholder
}

CAmount TransferTxBuilder::GetDGBFromUTXO(const COutPoint& outpoint) const {
    // This is a simplified implementation
    // In production, this would query the UTXO set
    // For now, return a reasonable value for testing
    return 100 * COIN; // 100 DGB placeholder
}

bool TxBuilder::ValidateAmount(CAmount amount) const {
    return amount > 0 && amount <= MAX_MONEY;
}

bool TxBuilder::ValidateFeeRate(CAmount feeRate) const {
    // Fee rate is in sat/kB (used in formula: (vsize * feeRate) / 1000)
    // DigiByte minimum relay fee: 100,000 sat/kB (0.001 DGB/kB)
    // DigiDollar transactions MUST pay at least 0.1 DGB fee to miners
    // For 0.1 DGB min fee on ~300 vB tx: need ~33,333,333 sat/kB (we use 35M)
    // Allow up to 100M sat/kB for flexibility (1 DGB/kB max)
    return feeRate >= 100000 && feeRate <= 100000000; // 100k to 100M sat/kB
}

// ============================================================================
// MintTxBuilder implementation
// ============================================================================

CAmount MintTxBuilder::CalculateRequiredCollateral(CAmount ddAmount, int lockDays) const {
    // Validate inputs
    if (ddAmount <= 0) {
        return 0;
    }

    if (oraclePrice <= 0) {
        return 0; // Cannot calculate without valid oracle price
    }

    // Convert days to blocks
    int64_t lockBlocks = LockDaysToBlocks(lockDays);

    // Get base collateral ratio
    const auto& ddParams = chainParams.GetDigiDollarParams();
    int baseRatio = GetCollateralRatioForLockTime(lockBlocks, ddParams);

    // Apply DCA using the canonical cached health source shared by validation.
    int systemCollateral = GetCurrentSystemCollateral();
    if (systemCollateral < 0) {
        LogPrintf("DigiDollar TxBuilder: Cannot calculate collateral without canonical system health\n");
        return 0;
    }
    int effectiveRatio = IsThawDayActive(chainParams.GetConsensus(), currentHeight) ?
        DCA::DynamicCollateralAdjustment::ApplyDCAForHealth(baseRatio, systemCollateral) :
        DCA::DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemCollateral);
    if (effectiveRatio <= 0 || effectiveRatio == std::numeric_limits<int>::max()) {
        LogPrintf("DigiDollar TxBuilder: DCA calculation failed (baseRatio=%d, health=%d)\n",
                  baseRatio, systemCollateral);
        return 0;
    }
    double dcaMultiplier = DCA::DynamicCollateralAdjustment::GetDCAMultiplier(systemCollateral);

    // Calculate required DGB
    // DD amount is in cents (100 = $1.00), oracle price is in micro-USD (1,000,000 = $1.00)
    CAmount usdValue = ddAmount; // DD amount = USD value in cents

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar TxBuilder: CalculateRequiredCollateral - DD: %d cents, Price: %lld micro-USD ($%.6f), BaseRatio: %d%%, DCA: %.2f, EffectiveRatio: %d%%\n",
              ddAmount, oraclePrice, oraclePrice / 1000000.0, baseRatio, dcaMultiplier, effectiveRatio);

    // Use 128-bit arithmetic to prevent overflow
    // Oracle price format: micro-USD (1,000,000 = $1.00 DGB price)
    // DD amount is in cents (100 = $1.00 USD)
    // Formula: DGB_sats = (DD_cents * COIN * ratio * 100) / oracle_micro_usd
    // Example: $100 DD at $0.00631 DGB with 150% ratio (oracle_micro_usd = 6310)
    //   = (10000 cents * 100000000 * 150 * 100) / 6310
    //   = 15,000,000,000,000,000 / 6310
    //   = 2,377,179,080,509 sats = ~23,772 DGB
    //
    // NOTE: Using __int128 to avoid uint64 overflow. Without this, minting >$18K DD
    // at 1000% ratio (or >$36K at 500%) causes silent overflow, producing a tiny
    // collateral requirement and allowing massively under-collateralized positions.
    __int128 numerator = static_cast<__int128>(usdValue) * static_cast<__int128>(COIN) *
                         static_cast<__int128>(effectiveRatio) * 100;
    __int128 denominator = static_cast<__int128>(oraclePrice);
    __int128 result128 = (numerator + denominator - 1) / denominator;

    // Overflow guard: cap at MAX_MONEY before casting to uint64_t.
    // Without this, extreme values could silently truncate to near-zero.
    if (result128 > static_cast<__int128>(MAX_MONEY)) {
        return 0; // Amount too large
    }
    uint64_t requiredCollateral = static_cast<uint64_t>(result128);

    // Add a fixed 1% safety margin to reduce knife-edge failures from
    // small oracle price movements between mempool admission and block template checks.
    const CAmount requiredWithSafetyMargin = ApplyCollateralSafetyMargin(requiredCollateral);
    if (requiredWithSafetyMargin <= 0) {
        return 0;
    }

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar TxBuilder: - Required collateral base: %llu sats (%.8f DGB), with 1%% safety margin: %llu sats (%.8f DGB)\n",
              requiredCollateral, requiredCollateral / 100000000.0,
              requiredWithSafetyMargin, requiredWithSafetyMargin / 100000000.0);

    return static_cast<CAmount>(requiredWithSafetyMargin);
}

int64_t MintTxBuilder::LockDaysToBlocks(int days) const {
    return DigiDollar::LockDaysToBlocks(days);
}

CScript MintTxBuilder::CreateCollateralScript(const TxBuilderMintParams& params) const {
    // Create P2TR collateral locking script using the scripts.h MintParams
    // Use global namespace to access the correct MintParams from scripts.h
    ::DigiDollar::MintParams scriptParams;
    scriptParams.ddAmount = params.ddAmount;
    scriptParams.lockHeight = currentHeight + LockDaysToBlocks(params.lockDays) + MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;

    CPubKey pubkey = params.ownerKey.GetPubKey();
    scriptParams.ownerKey = XOnlyPubKey(pubkey);
    scriptParams.internalKey = DigiDollar::GetCollateralNUMSKey();  // NUMS point — key-path spend impossible
    scriptParams.oracleKeys = GetOracleKeys(15); // Mock oracle keys

    return CreateCollateralP2TR(scriptParams);
}

CScript MintTxBuilder::CreateDDOutputScript(const CKey& owner, CAmount amount) const {
    CPubKey pubkey = owner.GetPubKey();
    return CreateDigiDollarP2TR(XOnlyPubKey(pubkey), amount);
}

bool MintTxBuilder::ValidateMintParams(const TxBuilderMintParams& params) const {
    const auto& ddParams = chainParams.GetDigiDollarParams();

    // Validate amount range (check for zero, negative, and excessive amounts)
    if (params.ddAmount <= 0) {
        LogPrintf("ValidateMintParams FAILED: ddAmount <= 0 (%d)\n", params.ddAmount);
        return false;
    }

    // Validate against consensus parameters
    // params.ddAmount is in cents (100 cents = $1.00)
    // Consensus params (minMintAmount/maxMintAmount) are also in cents
    if (!IsValidMintAmount(params.ddAmount, ddParams)) {
        LogPrintf("ValidateMintParams FAILED: IsValidMintAmount returned false for %d cents (min=%d, max=%d)\n",
                 params.ddAmount, ddParams.minMintAmount, ddParams.maxMintAmount);
        return false;
    }

    const int64_t lockBlocks = LockDaysToBlocks(params.lockDays);
    const int tierIndex = GetLockTierIndex(lockBlocks, ddParams);
    if (tierIndex < 0 || static_cast<uint32_t>(tierIndex) != params.lockTier) {
        LogPrintf("ValidateMintParams FAILED: non-canonical lock tier (lockDays=%d, lockBlocks=%lld, lockTier=%u, expectedTier=%d)\n",
                 params.lockDays, static_cast<long long>(lockBlocks), params.lockTier, tierIndex);
        return false;
    }

    // Validate key
    if (!params.ownerKey.IsValid()) {
        LogPrintf("ValidateMintParams FAILED: ownerKey is invalid\n");
        return false;
    }

    // Validate fee rate (must be reasonable)
    if (!ValidateFeeRate(params.feeRate)) {
        LogPrintf("ValidateMintParams FAILED: fee rate invalid (%d)\n", params.feeRate);
        return false;
    }

    // Validate that UTXOs are provided
    if (params.utxos.empty()) {
        LogPrintf("ValidateMintParams FAILED: no UTXOs provided\n");
        return false;
    }

    // Additional sanity checks
    if (params.ddAmount > MAX_DIGIDOLLAR) {
        LogPrintf("ValidateMintParams FAILED: ddAmount > MAX_DIGIDOLLAR (%d > %d)\n", params.ddAmount, MAX_DIGIDOLLAR);
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "ValidateMintParams PASSED\n");
    return true;
}

int TxBuilder::GetCurrentSystemCollateral() const {
    if (IsThawDayActive(chainParams.GetConsensus(), currentHeight)) {
        return candidateHealth && *candidateHealth >= 0 && *candidateHealth <= 30000 ? *candidateHealth : -1;
    }
    int systemHealth = DCA::DynamicCollateralAdjustment::GetCurrentSystemHealth();
    if (systemHealth >= 0) {
        return systemHealth;
    }

    const SystemMetrics metrics = SystemHealthMonitor::GetCachedMetrics();
    if (metrics.totalDDSupply == 0) {
        return 30000;
    }
    if (metrics.totalCollateral > 0 && metrics.totalDDSupply > 0 && oraclePrice > 0) {
        return DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
            metrics.totalCollateral,
            metrics.totalDDSupply,
            oraclePrice / 10);
    }

    return -1;
}

TxBuilderResult MintTxBuilder::BuildMintTransaction(const TxBuilderMintParams& params) {
    TxBuilderResult result;

    // Validate parameters
    if (!ValidateMintParams(params)) {
        result.error = "Invalid mint parameters";
        return result;
    }

    // Check oracle price availability
    if (oraclePrice <= 0) {
        result.error = "Oracle price unavailable or invalid";
        return result;
    }

    if (IsThawDayActive(chainParams.GetConsensus(), currentHeight)) {
        const int health = GetCurrentSystemCollateral();
        if (health < 0) { result.error = "DigiDollar candidate health state not ready"; return result; }
        if (health < 100) { result.error = "Minting blocked by candidate emergency health"; return result; }
    }

    // Create transaction
    CMutableTransaction tx;
    tx.SetDigiDollarType(::DD_TX_MINT);

    // Calculate required collateral
    result.collateralRequired = CalculateRequiredCollateral(params.ddAmount, params.lockDays);

    // Check if collateral calculation failed
    if (result.collateralRequired <= 0) {
        result.error = "Failed to calculate required collateral";
        return result;
    }

    // Estimate fees (rough estimate before final inputs are selected)
    CAmount estimatedFees = std::max<CAmount>(ESTIMATED_TX_VSIZE * params.feeRate / 1000, MIN_DD_TX_FEE);

    // Sanity check on total required amount
    if (result.collateralRequired > MAX_MONEY - estimatedFees) {
        result.error = "Required collateral amount too large";
        return result;
    }

    // Select inputs for collateral + fees
    std::vector<CTxIn> inputs;
    CAmount totalIn = 0;
    if (!SelectCoins(params.utxos, result.collateralRequired + estimatedFees,
                     inputs, totalIn)) {
        // Provide helpful error message distinguishing between truly insufficient funds
        // and having too many small UTXOs (which would exceed tx size limits)
        if (params.utxos.size() > MAX_TX_INPUTS) {
            result.error = "Too many small UTXOs - please consolidate your wallet first. "
                          "A regular DGB transaction can combine UTXOs before minting.";
        } else {
            result.error = "Insufficient funds for collateral and fees";
        }
        return result;
    }

    tx.vin = inputs;

    // Create collateral output (P2TR)
    CScript collateralScript = CreateCollateralScript(params);
    tx.vout.push_back(CTxOut(result.collateralRequired, collateralScript));

    // DD token output: Simple P2TR (key-path only, no MAST, no CLTV)
    // DD tokens must be freely transferable, unlike collateral which has timelock
    // Use CreateDDOutputScript for simple P2TR with key-path spending only
    CScript ddScript = CreateDDOutputScript(params.ownerKey, params.ddAmount);
    tx.vout.push_back(CTxOut(0, ddScript));

    // Add OP_RETURN output with metadata for validation
    // Format: OP_RETURN <"DD"> <txType> <ddAmount> <lockHeight> <lockTier> <ownerXOnlyPubKey>
    // NOTE: lockTier is stored explicitly to avoid deriving it from block heights
    // during wallet restore, which has timing variance issues.
    // SECURITY: Owner's x-only pubkey is included so validators can reconstruct the
    // expected P2TR collateral output (with NUMS internal key) and verify the output
    // matches. This prevents attackers from using their own key as internal key,
    // which would allow key-path spending that bypasses CLTV timelocks.
    int64_t lockHeight = currentHeight + LockDaysToBlocks(params.lockDays) + MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;
    CPubKey ownerPubKey = params.ownerKey.GetPubKey();
    XOnlyPubKey ownerXOnly(ownerPubKey);
    CScript metadataScript = CScript() << OP_RETURN
                                       << std::vector<unsigned char>{'D', 'D'}
                                       << CScriptNum(1)  // 1 = MINT transaction
                                       << CScriptNum(params.ddAmount)  // DD amount in cents
                                       << CScriptNum(lockHeight)  // Lock height in blocks
                                       << CScriptNum(params.lockTier)  // Lock tier (0-9)
                                       << std::vector<unsigned char>(ownerXOnly.begin(), ownerXOnly.end());  // Owner x-only pubkey (32 bytes)
    tx.vout.push_back(CTxOut(0, metadataScript));

    // Iterative fee calculation to account for change output
    // We need to calculate fee, then add change output, then recalculate fee
    CAmount change = 0;
    result.totalFees = 0;

    // First iteration: calculate fee without change output
    CAmount feeWithoutChange = std::max<CAmount>(CalculateFee(tx, params.feeRate), MIN_DD_TX_FEE);
    change = totalIn - result.collateralRequired - feeWithoutChange;

    if (change < 0) {
        result.error = "Insufficient funds after fee calculation";
        return result;
    }

    // If we have significant change, add change output and recalculate
    if (change >= DUST_THRESHOLD) {
        // The change only goes to the address the caller gave us. If there is
        // none, stop here: the money is still in the inputs and nothing has
        // been sent.
        CScript changeScript;
        if (!ResolveDGBChangeScript(params.dgbChangeDest, /*allowTaproot=*/false, changeScript, result.error)) {
            LogPrintf("DigiDollar: MINT stopped - %s\n", result.error);
            return result;
        }
        tx.vout.push_back(CTxOut(change, changeScript));

        // Recalculate fee with change output included
        result.totalFees = std::max<CAmount>(CalculateFee(tx, params.feeRate), MIN_DD_TX_FEE);

        // Ensure fees are reasonable
        if (result.totalFees > static_cast<CAmount>(totalIn * MAX_FEE_RATIO)) {
            result.error = "Transaction fees too high";
            return result;
        }

        // Adjust change amount based on actual fee
        change = totalIn - result.collateralRequired - result.totalFees;

        if (change < 0) {
            result.error = "Insufficient funds after final fee calculation";
            return result;
        }

        if (change < DUST_THRESHOLD) {
            // Change became dust after fee adjustment, remove change output
            tx.vout.pop_back();
            result.totalFees += change;  // Add dust to fee
        } else {
            // Update change output with correct amount
            tx.vout.back().nValue = change;
        }
    } else {
        // No change output, small change goes to fee
        result.totalFees = feeWithoutChange + change;
    }

    result.tx = tx;
    result.success = true;
    return result;
}

// ============================================================================
// TransferTxBuilder implementation
// ============================================================================

bool TransferTxBuilder::ValidateDDAddress(const std::string& address) const {
    return CDigiDollarAddress::IsValidDigiDollarAddressForCurrentNetwork(address);
}

CAmount TransferTxBuilder::GetDDFromUTXO(const COutPoint& outpoint) const {
    // This would query the UTXO set to get the DD amount
    // In production, this would look up the output in the blockchain
    // and extract the DD amount from the script
    // For testing, return a reasonable default value (can be overridden in test subclass)
    return 5000; // Default: $50.00 in cents for testing
}

bool TransferTxBuilder::ValidateTransferParams(const TxBuilderTransferParams& params) const {
    // Must have recipients
    if (params.recipients.empty()) {
        LogPrintf("DigiDollar: ValidateTransferParams FAILED - No recipients\n");
        return false;
    }

    // Validate all recipient addresses and amounts
    const auto& ddParams = chainParams.GetDigiDollarParams();
    CAmount minOutput = DigiDollar::GetMinimumDDOutput(ddParams);
    CAmount totalOutput = 0;
    for (const auto& [address, amount] : params.recipients) {
        // Validate address format
        if (!ValidateDDAddress(address)) {
            LogPrintf("DigiDollar: ValidateTransferParams FAILED - Invalid address: %s\n", address);
            return false;
        }

        // Validate amount ranges
        if (amount <= 0) {
            LogPrintf("DigiDollar: ValidateTransferParams FAILED - Non-positive amount: %d\n", amount);
            return false; // No zero or negative amounts
        }

        if (amount < minOutput) {
            LogPrintf("DigiDollar: ValidateTransferParams FAILED - Below dust threshold: %d < %d\n", amount, minOutput);
            return false; // Below dust threshold
        }

        // Check maximum single transfer limit ($100,000)
        if (amount > 10000000) { // $100,000.00 in cents
            LogPrintf("DigiDollar: ValidateTransferParams FAILED - Exceeds max transfer: %d > 10000000\n", amount);
            return false;
        }

        totalOutput += amount;
    }

    // Must have DD inputs
    if (params.ddUtxos.empty()) {
        LogPrintf("DigiDollar: ValidateTransferParams FAILED - No DD UTXOs\n");
        return false;
    }

    // Validate key
    if (!params.spenderKey.IsValid()) {
        LogPrintf("DigiDollar: ValidateTransferParams FAILED - Invalid spender key\n");
        return false;
    }

    // Validate fee rate
    if (!ValidateFeeRate(params.feeRate)) {
        LogPrintf("DigiDollar: ValidateTransferParams FAILED - Invalid fee rate: %d\n", params.feeRate);
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ValidateTransferParams PASSED\n");
    return true;
}

CAmount TransferTxBuilder::CalculateTotalDDInputs(const std::vector<COutPoint>& ddUtxos) const {
    CAmount total = 0;
    for (const auto& utxo : ddUtxos) {
        total += GetDDFromUTXO(utxo);
    }
    return total;
}

CAmount TransferTxBuilder::CalculateTotalDDOutputs(const std::vector<std::pair<std::string, CAmount>>& recipients) const {
    CAmount total = 0;
    for (const auto& [address, amount] : recipients) {
        total += amount;
    }
    return total;
}

// New functions for enhanced transfer functionality

CAmount TransferTxBuilder::CalculateTotalDDInput(const std::vector<CTxOut>& inputs,
                                                const std::vector<CAmount>& amounts) const {
    CAmount total = 0;

    // If amounts are provided, use them (for testing/mocking)
    if (!amounts.empty()) {
        for (CAmount amount : amounts) {
            total += amount;
        }
        return total;
    }

    // Otherwise extract from actual outputs
    for (const auto& output : inputs) {
        CAmount ddAmount = 0;
        if (DigiDollar::ExtractDDAmount(output.scriptPubKey, ddAmount)) {
            total += ddAmount;
        }
    }

    return total;
}

CScript TransferTxBuilder::CreateDDTransferScript(const CPubKey& recipient, CAmount amount) const {
    // Create P2TR script for DD transfer
    XOnlyPubKey xonly(recipient);
    return CreateDigiDollarP2TR(xonly, amount);
}

bool TransferTxBuilder::SelectDDInputs(const std::vector<CTxOut>& available, CAmount needed,
                                      std::vector<CTxOut>& selected, CAmount& total) {
    selected.clear();
    total = 0;

    // Simple greedy selection
    for (const auto& output : available) {
        CAmount ddAmount = 0;
        if (DigiDollar::ExtractDDAmount(output.scriptPubKey, ddAmount) && ddAmount > 0) {
            selected.push_back(output);
            total += ddAmount;

            if (total >= needed) {
                return true; // Found sufficient inputs
            }
        }
    }

    return total >= needed;
}

TxBuilderResult TransferTxBuilder::BuildTransferTransaction(const TxBuilderTransferParams& params) {
    TxBuilderResult result;

    // Validate parameters
    if (!ValidateTransferParams(params)) {
        result.error = "Invalid transfer parameters";
        return result;
    }

    // Calculate totals and check DD conservation
    CAmount totalDDIn = 0;
    if (!params.ddAmounts.empty() && params.ddAmounts.size() == params.ddUtxos.size()) {
        // Use provided amounts
        for (CAmount amount : params.ddAmounts) {
            totalDDIn += amount;
        }
    } else {
        // Fallback to UTXO lookup
        totalDDIn = CalculateTotalDDInputs(params.ddUtxos);
    }
    CAmount totalDDOut = CalculateTotalDDOutputs(params.recipients);

    // Strict DD conservation check
    if (totalDDIn < totalDDOut) {
        result.error = "Insufficient DD balance for transfer";
        return result;
    }

    // Check for dust outputs
    const auto& ddParams = chainParams.GetDigiDollarParams();
    CAmount minOutput = DigiDollar::GetMinimumDDOutput(ddParams);
    for (const auto& [address, amount] : params.recipients) {
        if (amount < minOutput) {
            result.error = "Transfer amount below dust threshold";
            return result;
        }
    }

    // Create transaction with DD transfer marker
    CMutableTransaction tx;
    tx.SetDigiDollarType(::DD_TX_TRANSFER);

    // Add DD inputs
    for (const auto& utxo : params.ddUtxos) {
        tx.vin.push_back(CTxIn(utxo));
    }

    // Add DGB fee inputs (after DD inputs)
    // Note: Phase 2.1 already selected these UTXOs, so we just add them directly
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: TxBuilder - feeUtxos.size=%d, feeAmounts.size=%d\n",
              params.feeUtxos.size(), params.feeAmounts.size());

    CAmount totalFeeIn = 0;
    for (size_t i = 0; i < params.feeUtxos.size(); ++i) {
        const auto& utxo = params.feeUtxos[i];
        tx.vin.push_back(CTxIn(utxo));
        // Get actual fee UTXO amount from feeAmounts
        CAmount feeAmount = (i < params.feeAmounts.size()) ? params.feeAmounts[i] : GetDGBFromUTXO(utxo);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Fee input %d - using %s: %d sats\n",
                  i, (i < params.feeAmounts.size()) ? "feeAmounts[i]" : "GetDGBFromUTXO()", feeAmount);
        totalFeeIn += feeAmount;
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added fee input %s:%d (%d sats)\n",
                  utxo.hash.ToString(), utxo.n, feeAmount);
    }

    // Add DD outputs for recipients (all with 0 DGB value)
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: TxBuilder - recipients.size=%d\n", params.recipients.size());
    for (const auto& [address, amount] : params.recipients) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Creating DD output - address=%s, amount=%d cents\n", address, amount);
        CTxDestination dest = DecodeDigiDollarAddress(address);
        const auto* taproot = std::get_if<WitnessV1Taproot>(&dest);
        if (!taproot) {
            result.error = "Failed to decode DD address: " + address;
            return result;
        }

        // Create P2TR output directly from the decoded address
        // The address is already a fully-formed Taproot output key (already tweaked by wallet)
        // We don't want to tweak it again, so we create the scriptPubKey directly
        CScript ddScript;
        ddScript << OP_1 << ToByteVector(*taproot);
        tx.vout.push_back(CTxOut(0, ddScript)); // DD outputs always have 0 DGB value

        // Register recipient DD output in metadata registry so future redemptions
        // can extract the DD amount when spending this output
        RegisterScriptMetadata(ddScript, ScriptType::DD_TOKEN_OUTPUT, amount, 0);

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added DD output for %s: %d cents (registered)\n", address, amount);
    }

    // Add DD change output if needed — EVERY cent of DD must be accounted for.
    // Unlike DGB dust, DD has real dollar value. No DD may be silently dropped.
    CAmount ddChange = totalDDIn - totalDDOut;
    if (ddChange > 0) {
        // Apply Taproot tweak to change output (same as CreateDigiDollarP2TR)
        // This ensures SignDDInputs can verify and sign the change output correctly.
        // The signing code expects tweaked keys for key-path spending.
        CPubKey changePubkey = params.spenderKey.GetPubKey();
        XOnlyPubKey xonly(changePubkey);

        // Apply the standard Taproot tweak (nullptr = no merkle root, key-path only)
        auto tweaked = xonly.CreateTapTweak(nullptr);
        if (!tweaked) {
            result.error = "Failed to create Taproot tweak for DD change output";
            return result;
        }
        XOnlyPubKey tweaked_key = tweaked->first;

        // Create P2TR output with TWEAKED key (matches CreateDigiDollarP2TR)
        CScript changeScript;
        changeScript << OP_1 << ToByteVector(tweaked_key);
        tx.vout.push_back(CTxOut(0, changeScript));

        // Register change output in metadata registry so future transactions
        // can extract the DD amount. Without this, multi-input operations fail
        // because ExtractDDAmount can't find the amount.
        RegisterScriptMetadata(changeScript, ScriptType::DD_TOKEN_OUTPUT, ddChange, 0);

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added DD change output: %d cents (tweaked key, registered)\n", ddChange);
    } else if (ddChange < 0) {
        // This should never happen - SelectDDCoins should ensure enough DD
        result.error = strprintf("Insufficient DD: need %d cents, have %d cents", totalDDOut, totalDDIn);
        return result;
    }
    // If ddChange == 0, perfect match - no change output needed

    // Calculate actual fee based on transaction with all outputs
    CAmount calculatedFee = CalculateFee(tx, params.feeRate);
    CAmount actualFee = std::max(calculatedFee, MIN_DD_TX_FEE);

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Fee calculation - calculated: %d sats, minimum: %d sats, actual: %d sats\n",
              calculatedFee, MIN_DD_TX_FEE, actualFee);

    if (totalFeeIn <= 0) {
        result.error = "Insufficient DGB fee input: no fee inputs selected";
        return result;
    }

    if (totalFeeIn < actualFee) {
        result.error = strprintf("Insufficient DGB fee input: selected=%d sats, required=%d sats",
                                 totalFeeIn, actualFee);
        return result;
    }

    // Add DGB change output if needed (after we know actual fee)
    if (totalFeeIn > 0) {
        CAmount dgbChange = totalFeeIn - actualFee;
        if (dgbChange > 0 && dgbChange >= DUST_THRESHOLD) {
            // The change only goes to the address the caller gave us. A wallet
            // watches the addresses it handed out and nothing else, so change
            // paid anywhere else looks to the owner like money that left the
            // wallet and never came back.
            CScript dgbChangeScript;
            if (!ResolveDGBChangeScript(params.dgbChangeDest, /*allowTaproot=*/true, dgbChangeScript, result.error)) {
                LogPrintf("DigiDollar: Transfer stopped - %s\n", result.error);
                return result;
            }
            tx.vout.push_back(CTxOut(dgbChange, dgbChangeScript));
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added DGB change output: %d sats\n", dgbChange);
        }
    }

    // Add OP_RETURN with DD amounts for each output (needed for receiver to identify amounts)
    // Format: OP_RETURN <"DD"> <txType> <output_count> <amount1> <amount2> ... <amountN>
    std::vector<CAmount> ddOutputAmounts;
    for (const auto& [address, amount] : params.recipients) {
        ddOutputAmounts.push_back(amount);
    }
    if (ddChange > 0) {
        ddOutputAmounts.push_back(ddChange);
    }

    CScript metadataScript;
    metadataScript << OP_RETURN
                   << std::vector<unsigned char>{'D', 'D'}
                   << CScriptNum(2);  // 2 = TRANSFER transaction

    // Add each DD output amount
    for (CAmount amt : ddOutputAmounts) {
        metadataScript << CScriptNum(amt);
    }

    tx.vout.push_back(CTxOut(0, metadataScript));
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added OP_RETURN with %d DD output amounts\n", ddOutputAmounts.size());

    // Final validation - ensure DD conservation
    // DD amounts are now stored in OP_RETURN, so sum up ddOutputAmounts instead
    CAmount finalDDOut = 0;
    for (CAmount amt : ddOutputAmounts) {
        finalDDOut += amt;
    }

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Conservation check - Input: %d cents, Output: %d cents\n",
              totalDDIn, finalDDOut);

    // DD conservation is absolute — every cent in must equal every cent out.
    // DD change outputs are always created (no dust exception), so strict equality holds.
    if (totalDDIn != finalDDOut) {
        result.error = "DD conservation violation: input=" + std::to_string(totalDDIn) +
                      " output=" + std::to_string(finalDDOut) +
                      " diff=" + std::to_string(totalDDIn - finalDDOut);
        return result;
    }

    // Set actual fees (already calculated above)
    result.totalFees = actualFee;

    // ============================================================================
    // Phase 2.6: Transaction Finalization
    // ============================================================================

    // Set transaction version - use DigiDollar marker to bypass dust checks
    // Format: bits 0-15 = 0x0770 (marker), bits 24-31 = transaction type
    // DD_TX_TRANSFER = 2, so version = 0x02000770
    tx.nVersion = 0x02000770;

    // Set locktime (0 for immediate broadcast)
    tx.nLockTime = 0;

    // Validate transaction structure - must have inputs
    if (tx.vin.empty()) {
        result.error = "Transaction has no inputs";
        return result;
    }

    // Validate transaction structure - must have outputs
    if (tx.vout.empty()) {
        result.error = "Transaction has no outputs";
        return result;
    }

    // Verify DD amounts balance (conservation check)
    CAmount totalDDInCheck = 0;
    if (!params.ddAmounts.empty() && params.ddAmounts.size() == params.ddUtxos.size()) {
        // Use provided amounts (CRITICAL FIX #8: Use actual UTXO amounts, not GetDDFromUTXO default)
        for (const auto& amt : params.ddAmounts) {
            totalDDInCheck += amt;
        }
    } else {
        // Fallback to GetDDFromUTXO (which returns hardcoded 5000 for testing)
        for (const auto& utxo : params.ddUtxos) {
            totalDDInCheck += GetDDFromUTXO(utxo);
        }
    }

    CAmount totalDDOutCheck = 0;
    for (const auto& [addr, amt] : params.recipients) {
        totalDDOutCheck += amt;
    }
    // Add DD change if exists — always created for any positive remainder
    if (ddChange > 0) {
        totalDDOutCheck += ddChange;
    }

    // DD conservation is absolute — every cent must be accounted for
    if (totalDDInCheck != totalDDOutCheck) {
        result.error = strprintf("DD amount mismatch: in=%d, out=%d", totalDDInCheck, totalDDOutCheck);
        return result;
    }

    // Transaction is valid - finalize
    result.tx = tx;
    result.success = true;

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Transaction finalized - %d inputs, %d outputs, version=%d, locktime=%d\n",
              tx.vin.size(), tx.vout.size(), tx.nVersion, tx.nLockTime);

    return result;
}

// ============================================================================
// RedeemTxBuilder implementation
// ============================================================================

bool RedeemTxBuilder::ValidateRedemptionPath(const TxBuilderRedeemParams& params) const {
    // Validate that the chosen path is valid for current conditions
    // NOTE: Only 2 paths exist - NORMAL and ERR
    switch (params.path) {
        case RedemptionPath::NORMAL:
            // Check if timelock has expired (simplified)
            return true;
        case RedemptionPath::ERR:
            // Check if system collateral < 100% (simplified)
            return true;
        default:
            return false;
    }
}

CAmount RedeemTxBuilder::CalculateRedemptionAmount(const TxBuilderRedeemParams& params) const {
    // Get collateral position data
    CCollateralPosition position = GetCollateralPosition(params.collateralOutpoint);

    // Calculate DGB to release based on DD burned and current conditions

    if (IsThawDayActive(chainParams.GetConsensus(), currentHeight)) {
        const int health = GetCurrentSystemCollateral();
        if (health < 0 || position.ddMinted <= 0) return 0;
        const CAmount required = ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(position.ddMinted, health);
        return required > 0 && params.ddToRedeem >= required ? position.dgbLocked : 0;
    }

    if (params.path == RedemptionPath::ERR) {
        // ERR (Emergency Redemption Ratio) path:
        // System < 100% requires MORE DD to burn for full collateral
        // Formula: RequiredDD = OriginalDD × (100 / SystemHealth%)

        int systemHealth = GetCurrentSystemCollateral();

        // Calculate required DD amount with ERR multiplier
        // Use 64-bit to prevent overflow: (ddMinted * 100) / systemHealth
        uint64_t requiredDD = (static_cast<uint64_t>(position.ddMinted) * 100) / systemHealth;

        // Validate user is burning enough DD
        if (params.ddToRedeem < static_cast<CAmount>(requiredDD)) {
            LogPrintf("DigiDollar: ERR redemption - insufficient DD (provided: %d, required: %llu)\n",
                     params.ddToRedeem, requiredDD);
            return 0; // Insufficient DD burned
        }

        // If enough DD burned, return full proportional collateral
        // For full redemption: return all collateral
        // For partial: return proportional amount
        if (params.ddToRedeem >= position.ddMinted) {
            return position.dgbLocked; // Full redemption
        } else {
            // Partial ERR redemption: proportional collateral
            uint64_t proportional = (static_cast<uint64_t>(position.dgbLocked) *
                                    static_cast<uint64_t>(params.ddToRedeem)) /
                                    static_cast<uint64_t>(position.ddMinted);
            return static_cast<CAmount>(proportional);
        }
    } else {
        // Normal/Emergency/Partial path: standard proportional collateral recovery
        if (params.ddToRedeem >= position.ddMinted) {
            return position.dgbLocked; // Full redemption
        } else {
            // Partial redemption: proportional collateral
            uint64_t proportional = (static_cast<uint64_t>(position.dgbLocked) *
                                    static_cast<uint64_t>(params.ddToRedeem)) /
                                    static_cast<uint64_t>(position.ddMinted);
            return static_cast<CAmount>(proportional);
        }
    }
}

bool RedeemTxBuilder::ValidateRedeemParams(const TxBuilderRedeemParams& params) const {
    // Validate DD amount
    if (params.ddToRedeem <= 0) {
        return false;
    }

    // Validate redemption path
    if (!ValidateRedemptionPath(params)) {
        return false;
    }

    // Validate key
    if (!params.ownerKey.IsValid()) {
        return false;
    }

    // Validate fee rate
    if (!ValidateFeeRate(params.feeRate)) {
        return false;
    }

    // Validate collateral outpoint
    if (params.collateralOutpoint.IsNull()) {
        return false;
    }

    // Validate DD UTXOs
    if (params.ddUtxos.empty()) {
        return false;
    }

    return true;
}

CScript RedeemTxBuilder::CreateRedemptionScript(RedemptionPath path, const CKey& owner) const {
    // Create the appropriate redemption script based on path
    // NOTE: Only 2 paths exist - NORMAL and ERR
    CPubKey pubkey = owner.GetPubKey();
    XOnlyPubKey xonly(pubkey);

    // Convert XOnlyPubKey to vector for script insertion
    std::vector<unsigned char> xonly_bytes(xonly.begin(), xonly.end());

    switch (path) {
        case RedemptionPath::NORMAL:
            // Normal redemption: P2TR with timelock (CLTV)
            return CScript() << xonly_bytes << OP_CHECKSIG;

        case RedemptionPath::ERR:
            // ERR: Emergency redemption ratio when system < 100%
            return CScript() << xonly_bytes << OP_CHECKSIG;

        default:
            return CScript();
    }
}

CCollateralPosition RedeemTxBuilder::GetCollateralPosition(const COutPoint& outpoint) const {
    // Query the actual UTXO using DigiDollar's UTXO lookup function
    CCollateralPosition position;
    position.outpoint = outpoint;

    // Fail-safe fallback when the caller did not provide wallet-cached
    // collateral metadata. Production RPC/wallet callers pass the original
    // amount, minted DD, and unlock height explicitly before building.
    Coin coin;
    if (false) {  // Disabled UTXO lookup - will use metadata from script instead
        LogPrintf("DigiDollar: GetCollateralPosition - UTXO not found or already spent: %s:%d\n",
                 outpoint.hash.ToString(), outpoint.n);
        // Return empty position if UTXO doesn't exist
        position.dgbLocked = 0;
        position.ddMinted = 0;
        position.unlockHeight = 0;
        position.collateralRatio = 0;
        return position;
    }

    // Extract actual collateral amount from the UTXO
    position.dgbLocked = coin.out.nValue;

    // Extract DD amount from script metadata
    CAmount ddAmount = 0;
    if (DigiDollar::ExtractDDAmount(coin.out.scriptPubKey, ddAmount)) {
        position.ddMinted = ddAmount;
    } else {
        LogPrintf("DigiDollar: GetCollateralPosition - WARNING: Could not extract DD amount from script\n");
        position.ddMinted = 0;
    }

    // Extract unlock height from script metadata
    int64_t lockTime = DigiDollar::ExtractLockTime(coin.out.scriptPubKey);
    if (lockTime > 0) {
        position.unlockHeight = static_cast<uint32_t>(lockTime);
    } else {
        LogPrintf("DigiDollar: GetCollateralPosition - WARNING: Could not extract lock time from script\n");
        position.unlockHeight = 0;
    }

    // Calculate collateral ratio
    if (position.ddMinted > 0) {
        position.collateralRatio = CalculatePositionCollateralRatio(position.dgbLocked, position.ddMinted, oraclePrice);
    } else {
        position.collateralRatio = 0;
    }

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: GetCollateralPosition - Found UTXO %s:%d with dgbLocked=%d, ddMinted=%d, unlockHeight=%d\n",
             outpoint.hash.ToString(), outpoint.n, position.dgbLocked, position.ddMinted, position.unlockHeight);

    return position;
}

TxBuilderResult RedeemTxBuilder::BuildRedemptionTransaction(const TxBuilderRedeemParams& params) {
    TxBuilderResult result;

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: BuildRedemptionTransaction - Starting (path: %d, ddToRedeem: %d)\n",
             static_cast<int>(params.path), params.ddToRedeem);

    // Step 1: Validate parameters
    if (!ValidateRedeemParams(params)) {
        result.error = "Invalid redemption parameters";
        LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
        return result;
    }

    // Step 2: Get collateral position (use pre-queried data if provided)
    CCollateralPosition position;
    if (params.collateralAmount > 0) {
        // Use pre-queried position data from caller (RPC provided wallet's cached data)
        if (params.ddMinted <= 0) {
            result.error = "Original DD minted amount unavailable for redemption";
            LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
            return result;
        }
        position.outpoint = params.collateralOutpoint;
        position.dgbLocked = params.collateralAmount;
        position.ddMinted = params.ddMinted;
        position.unlockHeight = params.unlockHeight;
        position.collateralRatio = CalculatePositionCollateralRatio(position.dgbLocked, position.ddMinted, oraclePrice);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Using pre-queried collateral position - dgbLocked: %d, ddMinted: %d, unlockHeight: %d\n",
                 position.dgbLocked, position.ddMinted, position.unlockHeight);
    } else {
        // Fallback is fail-safe unless a caller has supplied a chainstate-backed
        // position lookup in a derived builder.
        position = GetCollateralPosition(params.collateralOutpoint);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Queried collateral position from UTXO - dgbLocked: %d, ddMinted: %d, unlockHeight: %d\n",
                 position.dgbLocked, position.ddMinted, position.unlockHeight);
    }

    if (IsThawDayActive(chainParams.GetConsensus(), currentHeight)) {
        const int health = GetCurrentSystemCollateral();
        if (health < 0) { result.error = "DigiDollar candidate health state not ready"; return result; }
        const auto expected = health < 100 ? RedemptionPath::ERR : RedemptionPath::NORMAL;
        if (params.path != expected) {
            result.error = "Candidate emergency health changed; rebuild the redemption with the current required burn";
            return result;
        }
    }

    CAmount ddToBurn = params.ddToRedeem;
    if (params.path == RedemptionPath::ERR) {
        const int systemHealth = GetCurrentSystemCollateral();
        if (systemHealth < 0) {
            result.error = "ERR system health unavailable";
            LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
            return result;
        }
        if (position.ddMinted <= 0) {
            result.error = "Original DD minted amount unavailable for ERR redemption";
            LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
            return result;
        }
        ddToBurn = ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(position.ddMinted, systemHealth);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ERR redemption burn requirement - original: %lld, health: %d%%, required burn: %lld\n",
                  (long long)position.ddMinted, systemHealth, (long long)ddToBurn);
    }

    // Step 3: Verify redemption conditions are met (pass position to avoid re-querying)
    if (!VerifyRedemptionConditions(params, params.path, position)) {
        result.error = "Redemption conditions not met for path " + std::to_string(static_cast<int>(params.path));
        LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
        return result;
    }

    // Step 4: Calculate collateral return
    CAmount dgbToRelease = CalculateCollateralReturn(position.ddMinted > 0 ? position.ddMinted : params.ddToRedeem,
                                                     position.dgbLocked, oraclePrice);
    if (dgbToRelease <= 0) {
        result.error = "Failed to calculate collateral return";
        LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
        return result;
    }

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Collateral to release: %d sats (%.8f DGB)\n",
             dgbToRelease, dgbToRelease / 100000000.0);

    // Step 5: Build transaction
    CMutableTransaction tx;

    // Set type - always DD_TX_REDEEM (ERR is handled via burn amount, not tx type)
    // NOTE: Only 2 paths exist - NORMAL and ERR. Both use DD_TX_REDEEM.
    tx.SetDigiDollarType(::DD_TX_REDEEM);
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Using DD_TX_REDEEM type (path=%s)\n",
              params.path == RedemptionPath::ERR ? "ERR" : "NORMAL");

    // Input 0: Collateral UTXO (P2TR)
    // CRITICAL: nSequence must be < 0xFFFFFFFF to enable OP_CHECKLOCKTIMEVERIFY
    // Using 0xFFFFFFFE to signal opt-in Replace-By-Fee (BIP125) and enable CLTV
    tx.vin.push_back(CTxIn(params.collateralOutpoint, CScript(), 0xFFFFFFFE));
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added collateral input: %s:%d (nSequence=0xFFFFFFFE for CLTV)\n",
             params.collateralOutpoint.hash.ToString(), params.collateralOutpoint.n);

    // Inputs 1+: DD UTXOs to burn
    // CRITICAL: DD UTXOs also have CLTV in their script (same MAST tree as collateral)
    // So they MUST use nSequence < 0xFFFFFFFF to enable locktime checking
    for (const auto& utxo : params.ddUtxos) {
        tx.vin.push_back(CTxIn(utxo, CScript(), 0xFFFFFFFE));
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added DD input to burn: %s:%d (nSequence=0xFFFFFFFE for CLTV)\n", utxo.hash.ToString(), utxo.n);
    }

    // Inputs N+: Fee UTXOs (DGB)
    // CRITICAL FIX: Use the pre-selected fee UTXOs directly, don't re-select!
    // The caller (wallet or RPC) has already selected the appropriate fee UTXOs
    CAmount totalFeeIn = 0;
    if (!params.feeUtxos.empty()) {
        // Add all pre-selected fee UTXOs as inputs
        for (const auto& feeUtxo : params.feeUtxos) {
            tx.vin.push_back(CTxIn(feeUtxo));
        }

        // Calculate total fee input from provided amounts
        if (!params.feeAmounts.empty() && params.feeAmounts.size() == params.feeUtxos.size()) {
            for (CAmount amount : params.feeAmounts) {
                totalFeeIn += amount;
            }
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added %d fee inputs (total: %d sats) from pre-selected UTXOs\n",
                      params.feeUtxos.size(), totalFeeIn);
        } else {
            // Fallback: Try to get amounts from UTXO set (shouldn't normally happen)
            LogPrintf("DigiDollar: WARNING - feeAmounts not provided or size mismatch, using GetUTXOValue\n");
            for (const auto& feeUtxo : params.feeUtxos) {
                CAmount amount = GetUTXOValue(feeUtxo);
                totalFeeIn += amount;
            }
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added %d fee inputs (total: %d sats) via GetUTXOValue fallback\n",
                      params.feeUtxos.size(), totalFeeIn);
        }
    }

    // Output 0: the collateral this redemption unlocks, all of it. It is always
    // its own output, never mixed with the DGB left over after the fee. The
    // address comes from the caller; the builder never works one out for
    // itself, because the money would then be unspendable.
    CScript collateralReturnScript;
    if (!ResolveCollateralReturnScript(params.collateralDest, collateralReturnScript, result.error)) {
        LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
        return result;
    }
    tx.vout.push_back(CTxOut(dgbToRelease, collateralReturnScript));
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added collateral return output: %d sats (100%% of locked collateral)\n", dgbToRelease);

    // Calculate DD change - if we selected more DD UTXOs than needed, return the change
    CAmount totalDDInput = 0;
    for (const auto& amount : params.ddAmounts) {
        totalDDInput += amount;
    }
    CAmount ddChange = totalDDInput - ddToBurn;
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD input total: %d cents, to burn: %d cents, change: %d cents\n",
              totalDDInput, ddToBurn, ddChange);

    // Add DD change output if wallet selected more DD UTXOs than needed
    // NOTE: DD is fungible - exact-amount enforcement is at the VAULT level (ddToRedeem == position.ddMinted)
    // not at the DD input level. The wallet may select excess DD UTXOs and receive change.
    if (ddChange > 0) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: DD change: %d cents will be returned to owner\n", ddChange);
        // Create DD change output back to the owner using P2TR
        CPubKey ownerPubKey = params.ownerKey.GetPubKey();
        CScript ddChangeScript = CreateDigiDollarP2TR(XOnlyPubKey(ownerPubKey), ddChange);
        tx.vout.push_back(CTxOut(0, ddChangeScript));

        // CRITICAL FIX: Add OP_RETURN with DD change amount so wallet can recognize it
        // Format: OP_RETURN <"DD"> <txType=3 for REDEEM> <ddChangeAmount>
        // Without this, the wallet cannot determine the DD value of the change output
        // when scanning the blockchain (the P2TR output alone has 0 DGB value)
        CScript metadataScript;
        metadataScript << OP_RETURN
                       << std::vector<unsigned char>{'D', 'D'}
                       << CScriptNum(3)  // 3 = REDEEM transaction type
                       << CScriptNum(ddChange);  // DD change amount in cents
        tx.vout.push_back(CTxOut(0, metadataScript));
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added OP_RETURN for DD change: %d cents\n", ddChange);

        // CRITICAL FIX: Store ddChange in result so wallet can track the change UTXO
        // Without this, result.ddChange stays 0 and wallet never tracks the change!
        result.ddChange = ddChange;
    } else if (ddChange < 0) {
        // This should never happen - SelectDDCoins should ensure enough DD
        result.error = "Insufficient DD selected (input: " +
                       std::to_string(totalDDInput) + ", need: " +
                       std::to_string(params.ddToRedeem) + ")";
        LogPrintf("DigiDollar: REJECTED - insufficient DD inputs\n");
        return result;
    }
    // If ddChange == 0, no change output needed - burning exact amount

    // Set transaction locktime to unlockHeight (critical for CLTV validation)
    tx.nLockTime = position.unlockHeight;
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Set tx.nLockTime = %d (unlockHeight)\n", position.unlockHeight);

    // Calculate actual fees. DigiDollar redemptions must pay the absolute DD
    // fee floor, independent of transaction size, to preserve the anti-spam
    // policy used by mint and transfer builders.
    CAmount calculatedFee = CalculateFee(tx, params.feeRate);
    result.totalFees = std::max<CAmount>(calculatedFee, MIN_DD_TX_FEE);
    if (IsThawDayActive(chainParams.GetConsensus(), currentHeight)) {
        if (!MoneyRange(params.minimumFee)) { result.error = "Invalid redemption fee requirement"; return result; }
        result.totalFees = std::max(result.totalFees, params.minimumFee);
    }
    if (result.totalFees > calculatedFee) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Calculated redemption fee (%d sats) below DD minimum fee, using %d sats\n",
                  calculatedFee, result.totalFees);
    }

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Calculated fees: %d sats (fee inputs: %d sats)\n", result.totalFees, totalFeeIn);

    if (totalFeeIn <= 0) {
        result.error = "Insufficient fee inputs for DD redemption fee";
        LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
        return result;
    }

    CAmount feeChange = totalFeeIn - result.totalFees;
    if (feeChange < 0) {
        result.error = "Insufficient fee inputs for DD redemption fee";
        LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
        return result;
    }

    // Add fee change output if needed
    if (totalFeeIn > 0) {

        if (feeChange >= DUST_THRESHOLD) {
            // Prefer the caller's own change address, so the returned
            // collateral and the leftover fee money stay in separate outputs.
            // If there is no change address, the address chosen for the
            // returned collateral is used instead: that one also belongs to
            // whoever asked for the redemption, so nothing is lost.
            //
            // Those are the only two addresses used. Anything the builder could
            // work out for itself from the owner key would be an address no
            // wallet watches and no wallet can spend, so with neither of them
            // the build stops.
            std::optional<CTxDestination> changeDest = params.dgbChangeDest;
            if (!changeDest.has_value()) {
                changeDest = params.collateralDest;
                LogPrintf("DigiDollar: No separate change address given, sending fee change to the collateral address\n");
            }

            CScript changeScript;
            if (!ResolveDGBChangeScript(changeDest, /*allowTaproot=*/true, changeScript, result.error)) {
                LogPrintf("DigiDollar: BuildRedemptionTransaction FAILED - %s\n", result.error);
                return result;
            }
            tx.vout.push_back(CTxOut(feeChange, changeScript));
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Added fee change output: %d sats\n", feeChange);
        } else {
            // Dust goes to miner as fee
            result.totalFees += feeChange;
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Fee change (%d sats) below dust, added to fee\n", feeChange);
        }
    }

    // Success
    result.tx = tx;
    result.success = true;
    result.collateralRequired = 0; // No collateral required for redemption

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: BuildRedemptionTransaction SUCCESS - %d inputs, %d outputs, fee: %d sats\n",
             tx.vin.size(), tx.vout.size(), result.totalFees);

    return result;
}

RedeemTxBuilder::FeeEstimate RedeemTxBuilder::EstimateRedemptionFee(const TxBuilderRedeemParams& params) const
{
    FeeEstimate estimate;

    // Whether the transaction carries DD change decides two of its outputs,
    // so resolve the burn amount the same way BuildRedemptionTransaction
    // does: the caller's amount on the normal path, the health-derived
    // amount on the emergency path.
    CAmount ddMinted = params.ddMinted;
    if (params.collateralAmount <= 0) {
        ddMinted = GetCollateralPosition(params.collateralOutpoint).ddMinted;
    }
    CAmount ddToBurn = params.ddToRedeem;
    if (params.path == RedemptionPath::ERR) {
        const int systemHealth = GetCurrentSystemCollateral();
        if (systemHealth < 0) {
            estimate.error = "ERR system health unavailable";
            return estimate;
        }
        if (ddMinted <= 0) {
            estimate.error = "Original DD minted amount unavailable for ERR redemption";
            return estimate;
        }
        ddToBurn = ERR::EmergencyRedemptionRatio::GetRequiredDDBurn(ddMinted, systemHealth);
    }
    CAmount totalDDInput = 0;
    for (const CAmount amount : params.ddAmounts) totalDDInput += amount;
    const CAmount ddChange = totalDDInput - ddToBurn;
    if (ddChange < 0) {
        estimate.error = strprintf("Insufficient DD selected (input: %d, need: %d)", totalDDInput, ddToBurn);
        return estimate;
    }

    // Only sizes matter here, not values or keys. Every input and output
    // below has the same serialized length as the one the build creates.
    CMutableTransaction skeleton;
    skeleton.SetDigiDollarType(::DD_TX_REDEEM);
    skeleton.vin.push_back(CTxIn(params.collateralOutpoint, CScript(), 0xFFFFFFFE));
    for (const auto& utxo : params.ddUtxos) {
        skeleton.vin.push_back(CTxIn(utxo, CScript(), 0xFFFFFFFE));
    }
    for (const auto& utxo : params.feeUtxos) {
        skeleton.vin.push_back(CTxIn(utxo));
    }

    // The collateral return goes to the caller's destination, and a legacy
    // or SegWit v0 address has a different script length than Taproot. With no
    // destination at all the real build stops, so a taproot-sized placeholder
    // is enough to finish the measurement.
    CScript collateralReturnScript;
    if (params.collateralDest.has_value()) {
        collateralReturnScript = GetScriptForDestination(params.collateralDest.value());
    } else {
        collateralReturnScript = CScript() << OP_1 << std::vector<unsigned char>(32, 0);
    }
    skeleton.vout.push_back(CTxOut(1, collateralReturnScript));

    if (ddChange > 0) {
        // A Taproot token output of the same length as the real DD change
        // output, without registering anything in the script registry.
        skeleton.vout.push_back(CTxOut(0, CScript() << OP_1 << std::vector<unsigned char>(32, 0)));
        CScript metadataScript;
        metadataScript << OP_RETURN
                       << std::vector<unsigned char>{'D', 'D'}
                       << CScriptNum(3)
                       << CScriptNum(ddChange);
        skeleton.vout.push_back(CTxOut(0, metadataScript));
    }

    estimate.vsize = EstimateTransactionVSize(skeleton);
    const int64_t weight = static_cast<int64_t>(estimate.vsize) * WITNESS_SCALE_FACTOR;
    if (weight > MAX_STANDARD_TX_WEIGHT) {
        estimate.error = strprintf("Projected redemption transaction is too large: %d weight units (%u vB), standard limit is %d WU. Consolidate DGB fee coins first.",
                                   weight, static_cast<unsigned>(estimate.vsize), MAX_STANDARD_TX_WEIGHT);
        return estimate;
    }
    estimate.fee = std::max<CAmount>(CalculateFee(skeleton, params.feeRate), MIN_DD_TX_FEE);
    estimate.ok = true;
    return estimate;
}

bool RedeemTxBuilder::SelectRedemptionFeeInputs(TxBuilderRedeemParams& params,
                                                const FeeCoinSelector& select_coins,
                                                std::string& error,
                                                int max_attempts) const
{
    error.clear();
    if (!select_coins) {
        error = "No DGB fee coin selector provided";
        return false;
    }
    if (max_attempts < 1) max_attempts = 1;

    const auto give_up = [&params](std::string& out, std::string message) {
        params.feeUtxos.clear();
        params.feeAmounts.clear();
        out = std::move(message);
        return false;
    };

    // The first target is the fee of the smallest redemption that can
    // exist: the collateral input, the DD inputs and exactly one fee coin.
    CAmount target = 0;
    {
        TxBuilderRedeemParams smallest = params;
        smallest.feeUtxos.assign(1, COutPoint());
        smallest.feeAmounts.clear();
        const FeeEstimate first = EstimateRedemptionFee(smallest);
        if (!first.ok) return give_up(error, first.error);
        target = first.fee;
    }

    CAmount lastTotal = -1;
    size_t lastCount = 0;
    FeeEstimate estimate;
    CAmount total = 0;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        std::vector<COutPoint> utxos;
        std::vector<CAmount> amounts;
        total = 0;
        if (!select_coins(target, utxos, amounts, total)) {
            return give_up(error, strprintf("Insufficient DGB balance for the redemption fee: need at least %d sats (%.8f DGB) in spendable DGB coins other than the collateral and DD token outputs",
                                            target, target / 100000000.0));
        }
        if (utxos.empty() || utxos.size() != amounts.size()) {
            return give_up(error, "DGB fee coin selection returned no usable coins");
        }
        params.feeUtxos = utxos;
        params.feeAmounts = amounts;

        estimate = EstimateRedemptionFee(params);
        if (!estimate.ok) return give_up(error, estimate.error);

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: redemption fee selection attempt %d - %zu fee inputs worth %d sats, projected %u vB, fee %d sats\n",
                 attempt, utxos.size(), total, static_cast<unsigned>(estimate.vsize), estimate.fee);

        if (total >= estimate.fee) return true;

        if (total <= lastTotal && utxos.size() <= lastCount) {
            // The selector returned nothing more than last time: the wallet
            // has no further coin to add, so more attempts cannot help.
            break;
        }
        lastTotal = total;
        lastCount = utxos.size();

        // Cover the fee of the transaction this attempt would produce, and
        // ask for strictly more than was just selected so the next
        // selection cannot be the same set of coins.
        target = std::max<CAmount>(estimate.fee, total + 1);
    }

    return give_up(error, strprintf("Insufficient DGB fee inputs: %zu DGB coins worth %d sats cannot pay the %d sat fee of the %u vB redemption they would produce, because each additional small coin adds more fee than value. Consolidate small DGB coins into one larger coin and retry.",
                                    lastCount, lastTotal, estimate.fee, static_cast<unsigned>(estimate.vsize)));
}

RedemptionPath RedeemTxBuilder::DetermineRedemptionPath(const TxBuilderRedeemParams& params) const {
    // Determine redemption path based on system health
    // NOTE: Only 2 paths exist - NORMAL and ERR
    // Both require timelock expiry. Partial redemption is NOT supported.

    // Check if ERR conditions are met (system under-collateralized)
    if (GetCurrentSystemCollateral() < 100) {
        return RedemptionPath::ERR;
    }

    // Default: Normal redemption (system healthy)
    return RedemptionPath::NORMAL;
}

CAmount RedeemTxBuilder::CalculateCollateralReturn(CAmount ddAmount, CAmount originalCollateral,
                                                  CAmount currentPrice) const {
    // Calculate collateral return based on DD amount and current price

    // Validation
    if (ddAmount <= 0 || originalCollateral <= 0 || currentPrice <= 0) {
        LogPrintf("DigiDollar: CalculateCollateralReturn FAILED - invalid parameters (dd: %d, collateral: %d, price: %d)\n",
                 ddAmount, originalCollateral, currentPrice);
        return 0;
    }

    // For Phase 1: Return full proportional collateral
    // Formula: (ddAmount / totalDDMinted) * originalCollateral
    // Since we're redeeming the full position in most cases, we return the full collateral
    // For partial redemptions, this would be adjusted

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Calculating collateral return - DD amount: %d, Original collateral: %d, Current price: %d\n",
             ddAmount, originalCollateral, currentPrice);

    // V1 supports only full-position redemption. Normal redemption and ERR both
    // return full collateral; ERR safety is enforced by requiring an increased
    // DD burn in ValidateCollateralReleaseAmount()/BuildRedemptionTransaction.

    CAmount returnAmount = originalCollateral;

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Collateral return calculated: %d sats (%.8f DGB)\n",
             returnAmount, returnAmount / 100000000.0);

    return returnAmount;
}

bool RedeemTxBuilder::VerifyRedemptionConditions(const TxBuilderRedeemParams& params,
                                                RedemptionPath path,
                                                const CCollateralPosition& position) const {
    // Verify conditions are met for the specified redemption path
    // NOTE: Only 2 paths exist - NORMAL and ERR. Both require timelock expiry.

    // Both paths require timelock to be expired
    if (currentHeight < position.unlockHeight) {
        LogPrintf("DigiDollar: Redemption FAILED - timelock not expired (current: %d, unlock: %d)\n",
                 currentHeight, position.unlockHeight);
        return false;
    }

    switch (path) {
        case RedemptionPath::NORMAL:
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Normal redemption conditions met (timelock expired)\n");
            return true;

        case RedemptionPath::ERR:
            // ERR (Emergency Redemption Ratio) - Check system health < 100%
            {
                int systemHealth = GetCurrentSystemCollateral();
                if (systemHealth >= 100) {
                    LogPrintf("DigiDollar: ERR redemption FAILED - system healthy (health: %d%%, need < 100%%)\n", systemHealth);
                    return false;
                }
                // Verify oracle price is valid for ERR calculation
                if (oraclePrice <= 0) {
                    LogPrintf("DigiDollar: ERR redemption FAILED - invalid oracle price (%d)\n", oraclePrice);
                    return false;
                }
                LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: ERR redemption conditions met (system health: %d%%)\n", systemHealth);
                return true;
            }

        default:
            LogPrintf("DigiDollar: Unknown redemption path: %d\n", static_cast<int>(path));
            return false;
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

// LockDaysToBlocks, GetCollateralRatioForLockTime, and GetDCAMultiplier
// are implemented in consensus/digidollar.cpp

std::string EncodeDigiDollarAddress(const CTxDestination& dest, const CChainParams& chainParams) {
    const auto* taproot = std::get_if<WitnessV1Taproot>(&dest);
    if (!taproot) {
        return "";
    }

    // Determine network type from chain params
    int networkType;
    std::string chainType = chainParams.GetChainTypeString();
    if (chainType == "regtest") {
        networkType = CChainParams::DIGIDOLLAR_ADDRESS_REGTEST;
    } else if (chainType == "test") {
        networkType = CChainParams::DIGIDOLLAR_ADDRESS_TESTNET;
    } else {
        networkType = CChainParams::DIGIDOLLAR_ADDRESS;
    }

    // Use the CDigiDollarAddress class to encode
    CDigiDollarAddress addr;
    if (addr.SetDigiDollar(dest, networkType)) {
        return addr.ToString();
    }

    return "";
}

size_t EstimateTransactionVSize(const CMutableTransaction& tx) {
    // Simplified estimation - actual implementation would be more sophisticated
    size_t baseSize = ::GetSerializeSize(tx, PROTOCOL_VERSION);

    // Add witness overhead estimation
    size_t witnessSize = 0;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        // Estimate P2TR witness size (signature + control block)
        // P2TR key path spend: 1 (stack items) + 1 (sig length) + 64 (signature) = 66 bytes
        // But we use script path with control block:
        // 1 (items) + 1 (sig len) + 64 (sig) + 1 (script len) + script + 1 (control len) + 33 (control)
        // Conservatively estimate 110 bytes per input to account for script path
        witnessSize += 110;
    }

    // Virtual size calculation: (base_size * 3 + total_size) / 4
    size_t totalSize = baseSize + witnessSize;
    size_t vsize = (baseSize * 3 + totalSize) / 4;

    // Add 35% safety margin to account for estimation errors
    // Taproot transactions with script-path spending can be larger than key-path estimates
    return vsize + (vsize * 35 / 100);
}

} // namespace DigiDollar
