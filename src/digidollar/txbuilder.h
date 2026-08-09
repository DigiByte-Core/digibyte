// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_DIGIDOLLAR_TXBUILDER_H
#define DIGIBYTE_DIGIDOLLAR_TXBUILDER_H

#include <primitives/transaction.h>
#include <consensus/amount.h>
#include <key.h>
#include <script/script.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <consensus/digidollar.h>
#include <kernel/chainparams.h>
#include <base58.h>

#include <vector>
#include <string>
#include <utility>

namespace DigiDollar {

/**
 * Minimum fee rate for DigiDollar transactions, in satoshis per kvB
 * (0.35 DGB/kvB, i.e. 35,000 sat/vB). DigiByte expresses fee rates per
 * kilo-vbyte, not per vbyte.
 */
static constexpr CAmount MIN_DD_FEE_RATE{35000000};

/** Absolute fee floor for any DigiDollar transaction (0.1 DGB). */
static constexpr CAmount MIN_DD_TX_FEE{10000000};

// Apply the wallet mint collateral safety margin used by MintTxBuilder.
// The input and output are DGB satoshis.
CAmount ApplyCollateralSafetyMargin(CAmount requiredCollateral);

// Result of transaction building
struct TxBuilderResult {
    bool success;
    CMutableTransaction tx;
    std::string error;
    CAmount totalFees;
    CAmount collateralRequired;
    CAmount ddChange;  // DD change returned (for redemption transactions)

    TxBuilderResult() : success(false), totalFees(0), collateralRequired(0), ddChange(0) {}
};

// Parameters for minting DigiDollars (txbuilder version)
struct TxBuilderMintParams {
    CAmount ddAmount;           // Amount of DD to mint (in cents)
    int lockDays;               // Lock period in days
    uint32_t lockTier;          // Lock tier (0-9) - stored in OP_RETURN for exact reconstruction
    CKey ownerKey;              // Owner's private key
    CAmount feeRate;            // Fee rate in sat/vB
    std::vector<COutPoint> utxos; // Available UTXOs for collateral

    // Optional: Destination for DGB change output. Production wallet/RPC/Qt
    // paths must set this to a wallet-controlled address.
    std::optional<CTxDestination> dgbChangeDest;

    TxBuilderMintParams() : ddAmount(0), lockDays(0), lockTier(0), feeRate(1000) {} // Default 1000 sat/vB
};

// Parameters for transferring DigiDollars (V2 - enhanced for tests)
struct TxBuilderTransferParams {
    std::vector<std::pair<std::string, CAmount>> recipients; // DD address, amount
    CAmount feeRate;            // Fee rate in sat/vB
    std::vector<COutPoint> ddUtxos;    // DD UTXOs to spend
    std::vector<CAmount> ddAmounts;    // DD amounts for each UTXO (parallel to ddUtxos)
    std::vector<COutPoint> feeUtxos;   // DGB UTXOs for fees
    std::vector<CAmount> feeAmounts;   // DGB amounts for each fee UTXO (parallel to feeUtxos)
    CKey spenderKey;            // Key for signing DD inputs

    // Optional: Destination for DGB change output (if not set, uses spenderKey pubkey)
    // CRITICAL: Must be set to a wallet-controlled address to avoid losing DGB!
    std::optional<CTxDestination> dgbChangeDest;

    TxBuilderTransferParams() : feeRate(1000) {} // Default 1000 sat/vB
};

// Legacy alias for compatibility
using TransferParams = TxBuilderTransferParams;

// Parameters for redeeming DigiDollars (enhanced for TDD)
struct TxBuilderRedeemParams {
    COutPoint collateralOutpoint;  // Collateral to unlock
    CAmount ddToRedeem;             // Amount of DD to burn
    RedemptionPath path;            // Which redemption path to use
    CKey ownerKey;                  // Owner's private key (for signing collateral input)
    CAmount feeRate;                // Fee rate in sat/vB
    std::vector<COutPoint> ddUtxos; // DD UTXOs to burn
    std::vector<CAmount> ddAmounts;  // DD amounts for each UTXO (parallel to ddUtxos)
    std::vector<COutPoint> feeUtxos; // DGB UTXOs for fees
    std::vector<CAmount> feeAmounts; // Amounts of fee UTXOs

    // Optional: Destination for returned collateral (if not set, uses ownerKey pubkey)
    std::optional<CTxDestination> collateralDest;

    // Optional: Destination for DGB change output (if not set, uses collateralDest or ownerKey)
    // CRITICAL: Must be set to a wallet-controlled address to avoid losing DGB!
    std::optional<CTxDestination> dgbChangeDest;

    // Optional pre-queried position data (caller can provide to avoid UTXO lookups)
    CAmount collateralAmount = 0;   // Actual DGB collateral locked (0 = not provided)
    CAmount ddMinted = 0;            // DD amount minted (0 = not provided)
    uint32_t unlockHeight = 0;       // Unlock height (0 = not provided)

    TxBuilderRedeemParams() : ddToRedeem(0), path(RedemptionPath::NORMAL), feeRate(1000) {}
};

// Legacy alias for compatibility
using RedeemParams = TxBuilderRedeemParams;

// Redeemable position information for wallet functions
struct RedeemablePosition {
    COutPoint collateralOutpoint;    // The collateral UTXO
    CAmount ddAmount;                // DD amount that can be redeemed
    CAmount dgbLocked;               // DGB collateral locked
    int64_t unlockHeight;            // Height when normal redemption is available
    std::vector<RedemptionPath> availablePaths; // Available redemption paths
    bool canRedeemNow;               // Whether any path is currently available
    CAmount estimatedReturn;         // Estimated DGB return amount

    RedeemablePosition() : ddAmount(0), dgbLocked(0), unlockHeight(0), canRedeemNow(false), estimatedReturn(0) {}
};

// Base transaction builder class
class TxBuilder {
protected:
    const CChainParams& chainParams;
    int currentHeight;
    CAmount oraclePrice;

    // Helper functions
    CAmount CalculateFee(const CMutableTransaction& tx, CAmount feeRate) const;
    bool SelectCoins(const std::vector<COutPoint>& utxos, CAmount target,
                     std::vector<CTxIn>& inputs, CAmount& total) const;
    CAmount GetUTXOValue(const COutPoint& outpoint) const;
    int GetCurrentSystemCollateral() const;

    // Virtual method for UTXO value lookup (can be overridden by child classes)
    // This is used by SelectCoins to determine UTXO values when building transactions
    virtual CAmount GetDGBFromUTXO(const COutPoint& outpoint) const { return 100 * COIN; }
    virtual CAmount GetUTXOValueVirtual(const COutPoint& outpoint) const { return GetDGBFromUTXO(outpoint); }

public:
    TxBuilder(const CChainParams& params, int height, CAmount price);
    virtual ~TxBuilder() = default;

    // Validation helpers
    bool ValidateAmount(CAmount amount) const;
    bool ValidateFeeRate(CAmount feeRate) const;
};

/**
 * Mint transaction builder for DigiDollar
 *
 * Handles the creation of mint transactions that lock DGB collateral
 * and create new DigiDollar outputs. Features include:
 * - 8 lock tier support (30 days to 10 years)
 * - Dynamic Collateral Adjustment (DCA) based on system health
 * - P2TR outputs with MAST redemption paths
 * - Comprehensive validation and error handling
 */
class MintTxBuilder : public TxBuilder {
public:
    using TxBuilder::TxBuilder;

    /**
     * Build a complete mint transaction
     * @param params Mint parameters including DD amount, lock period, keys, etc.
     * @return Transaction builder result with success/error and transaction data
     */
    TxBuilderResult BuildMintTransaction(const TxBuilderMintParams& params);

    /**
     * Calculate required DGB collateral for given DD amount and lock period
     * @param ddAmount DigiDollar amount to mint (in cents)
     * @param lockDays Lock period in days
     * @return Required DGB collateral amount in satoshis
     */
    CAmount CalculateRequiredCollateral(CAmount ddAmount, int lockDays) const;

    /**
     * Convert lock days to blocks using DigiByte's 15-second block time
     * @param days Lock period in days
     * @return Equivalent number of blocks
     */
    int64_t LockDaysToBlocks(int days) const;

    /**
     * Validate mint parameters before building a transaction. Rejects
     * non-canonical lock durations, mismatched tier bytes, invalid amounts,
     * invalid keys, and missing UTXOs. Mirrors the public TransferTxBuilder
     * input-validation API and lets callers reject doomed mints early.
     * @param params Mint parameters to validate
     * @return true if every parameter is acceptable to the mint builder
     */
    bool ValidateMintParams(const TxBuilderMintParams& params) const;

protected:

private:
    CScript CreateCollateralScript(const TxBuilderMintParams& params) const;
    CScript CreateDDOutputScript(const CKey& owner, CAmount amount) const;
    CKey GenerateChangeKey() const;
};

// Transfer transaction builder
class TransferTxBuilder : public TxBuilder {
public:
    using TxBuilder::TxBuilder;
    TxBuilderResult BuildTransferTransaction(const TxBuilderTransferParams& params);
    bool ValidateTransferParams(const TxBuilderTransferParams& params) const;
    CAmount CalculateTotalDDInput(const std::vector<CTxOut>& inputs,
                                  const std::vector<CAmount>& amounts) const;
    CScript CreateDDTransferScript(const CPubKey& recipient, CAmount amount) const;
    bool SelectDDInputs(const std::vector<CTxOut>& available, CAmount needed,
                       std::vector<CTxOut>& selected, CAmount& total);

protected:
    // Virtual for testing - can be overridden
    virtual CAmount GetDDFromUTXO(const COutPoint& outpoint) const;
    CAmount GetDGBFromUTXO(const COutPoint& outpoint) const override;

private:
    bool ValidateDDAddress(const std::string& address) const;
    CAmount CalculateTotalDDInputs(const std::vector<COutPoint>& ddUtxos) const;
    CAmount CalculateTotalDDOutputs(const std::vector<std::pair<std::string, CAmount>>& recipients) const;
};

/**
 * Redeem transaction builder for DigiDollar
 *
 * Handles the creation of redemption transactions that unlock DGB collateral
 * and burn DigiDollar tokens. Features include:
 * - 2 redemption paths (Normal, ERR)
 * - Taproot script path spending with MAST
 * - Timelock validation and emergency conditions
 * - Collateral release calculation with oracle price integration
 *
 * Transaction Output Structure:
 * - Output 0: Collateral return (100% of locked DGB) to collateralDest
 * - Output 1+: DD change (if any) back to owner
 * - Output N: DGB change from fee inputs to dgbChangeDest (SEPARATE from collateral!)
 *
 * CRITICAL: collateralDest and dgbChangeDest MUST be different to prevent merging
 * collateral return with DGB change in a single output.
 */
class RedeemTxBuilder : public TxBuilder {
public:
    using TxBuilder::TxBuilder;

    /**
     * Build a complete redemption transaction
     * @param params Redemption parameters including DD amount, path, keys, etc.
     * @return Transaction builder result with success/error and transaction data
     */
    TxBuilderResult BuildRedemptionTransaction(const TxBuilderRedeemParams& params);

    /**
     * Determine the appropriate redemption path based on current conditions
     * @param params Redemption parameters
     * @return The optimal redemption path for current system state
     */
    RedemptionPath DetermineRedemptionPath(const TxBuilderRedeemParams& params) const;

    /**
     * Calculate collateral return amount based on current conditions
     * @param ddAmount DigiDollar amount being redeemed
     * @param originalCollateral Original collateral locked
     * @param currentPrice Current oracle price
     * @return Amount of DGB collateral to release
     */
    CAmount CalculateCollateralReturn(CAmount ddAmount, CAmount originalCollateral,
                                     CAmount currentPrice) const;

    /**
     * Verify redemption conditions are met for the specified path
     * @param params Redemption parameters
     * @param path Redemption path to verify
     * @param position Collateral position (already queried, avoid duplicate lookups)
     * @return true if conditions are met for this path
     */
    bool VerifyRedemptionConditions(const TxBuilderRedeemParams& params,
                                   RedemptionPath path,
                                   const CCollateralPosition& position) const;

    /**
     * Create redemption script for the specified path
     * @param path Redemption path
     * @param owner Owner key for script creation
     * @return Redemption script for the path
     */
    CScript CreateRedemptionScript(RedemptionPath path, const CKey& owner) const;

private:
    bool ValidateRedemptionPath(const TxBuilderRedeemParams& params) const;
    CAmount CalculateRedemptionAmount(const TxBuilderRedeemParams& params) const;
    bool ValidateRedeemParams(const TxBuilderRedeemParams& params) const;
    CCollateralPosition GetCollateralPosition(const COutPoint& outpoint) const;
};

// Utility functions for working with DigiDollar transactions

// Note: LockDaysToBlocks, GetCollateralRatioForLockTime, and GetDCAMultiplier
// are defined in consensus/digidollar.h and implemented there

/**
 * Encode DigiDollar address from destination
 * @param dest Destination (should be WitnessV1Taproot)
 * @param chainParams Chain parameters for network type
 * @return DigiDollar address string
 */
std::string EncodeDigiDollarAddress(const CTxDestination& dest, const CChainParams& chainParams);

/**
 * Estimate transaction virtual size
 * @param tx Transaction to estimate
 * @return Estimated virtual size in vBytes
 */
size_t EstimateTransactionVSize(const CMutableTransaction& tx);

/**
 * Approximate fee that spending one additional input costs at the given fee rate.
 *
 * A fee UTXO worth less than this has negative effective value: adding it to a
 * transaction reduces, rather than increases, the amount available to pay the
 * fee. Coin selection for DigiDollar fee inputs prices inputs with this.
 *
 * The marginal input size is measured against EstimateTransactionVSize() at zero
 * inputs and comes out at 92 vB (41 base bytes plus the flat 110-byte witness
 * allowance, i.e. 68.5 vB, carrying the estimator's 35% margin). It is an
 * approximation, not an exact per-input cost: EstimateTransactionVSize()
 * truncates twice, so the true marginal alternates between 92 and 93 vB
 * depending on the size of the rest of the transaction. Callers that must not
 * come up short re-project the transaction and re-select (see
 * DigiDollarWallet::SelectRedemptionFeeCoins), which absorbs the difference.
 *
 * @param feeRate Fee rate in satoshis per kvB
 * @return Cost in satoshis of spending one extra input, 0 for a non-positive
 *         fee rate, MAX_MONEY if the fee rate is large enough to overflow
 */
CAmount EstimateInputSpendCost(CAmount feeRate);

} // namespace DigiDollar

#endif // DIGIBYTE_DIGIDOLLAR_TXBUILDER_H
