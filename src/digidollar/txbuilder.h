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

#include <functional>
#include <vector>
#include <string>
#include <utility>

namespace DigiDollar {

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

    // Where leftover DGB goes. Set this to an address the wallet owns. It must
    // be an ordinary bech32 address, never taproot: a mint may hold DGB in only
    // one taproot output, the locked collateral. If a mint would leave change
    // behind and this is not set, the build fails and no transaction is made.
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

    // Where leftover DGB goes. Set this to an address the wallet owns. If a
    // transfer would leave change behind and this is not set, the build fails
    // and no transaction is made.
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
    CAmount feeRate;                // Fee rate in sat/kB
    CAmount minimumFee{0};          // Fee measured after signing a prior construction attempt
    std::vector<COutPoint> ddUtxos; // DD UTXOs to burn
    std::vector<CAmount> ddAmounts;  // DD amounts for each UTXO (parallel to ddUtxos)
    std::vector<COutPoint> feeUtxos; // DGB UTXOs for fees
    std::vector<CAmount> feeAmounts; // Amounts of fee UTXOs

    // Where the collateral this redemption unlocks is paid. That is the whole
    // vault, so it always gets its own output. Set this to an address the
    // wallet owns, or to the address the user asked for. If it is not set the
    // build fails and no transaction is made.
    std::optional<CTxDestination> collateralDest;

    // Where leftover fee money goes. Set this to an address the wallet owns so
    // it stays in its own output, separate from the returned collateral. If it
    // is not set, the collateral address above is used instead.
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
    std::optional<int> candidateHealth;

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

    /** Supply health verified for this builder's candidate height and quote. */
    void SetCandidateHealth(int health) { candidateHealth = health; }

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
     * The size and fee BuildRedemptionTransaction arrives at for exactly
     * these parameters. It lays out the same inputs (collateral, every DD
     * token, every fee coin) and the same outputs that exist at the moment
     * the build fixes its fee (the collateral return, plus the DD change
     * token and its OP_RETURN record when the DD inputs exceed the burn),
     * runs them through the same size estimator and applies the same
     * absolute DD minimum fee. The DGB change output is added after the fee
     * is fixed and therefore does not count, in the build or here.
     *
     * Callers use it to choose fee coins that the build will accept, instead
     * of guessing a size in advance. A unit test pins it to the build.
     */
    struct FeeEstimate {
        bool ok{false};
        size_t vsize{0};   // Projected virtual size, safety margin included.
        CAmount fee{0};    // The fee the build will charge, in satoshis.
        std::string error; // Why no estimate could be made, when !ok.
    };
    FeeEstimate EstimateRedemptionFee(const TxBuilderRedeemParams& params) const;

    /**
     * Selects DGB coins worth at least `target` satoshis. On success it
     * fills the coins and their amounts (same order, same length) and their
     * total; it returns false when the wallet cannot reach the target. The
     * wallet's own coin selection is passed in this shape so the builder
     * never needs to know about wallets.
     */
    using FeeCoinSelector = std::function<bool(CAmount target,
                                               std::vector<COutPoint>& utxos,
                                               std::vector<CAmount>& amounts,
                                               CAmount& total)>;

    static constexpr int DEFAULT_FEE_SELECTION_ATTEMPTS{12};

    /**
     * Choose fee coins for a redemption so that the coins actually cover the
     * fee of the transaction they produce. Every fee coin makes the
     * transaction bigger and so raises the fee, which is why a single
     * selection against a guessed size can come up short when a wallet
     * holds only small DGB coins. Each attempt asks the selector for at
     * least the fee the previous attempt's real size needs, and for
     * strictly more than the previous attempt selected, so every attempt
     * changes the input set until the coins cover the fee or the attempt
     * budget is spent. On success params.feeUtxos and params.feeAmounts
     * hold the chosen coins; on failure they are cleared and `error` says
     * what the caller can do about it.
     */
    bool SelectRedemptionFeeInputs(TxBuilderRedeemParams& params,
                                   const FeeCoinSelector& select_coins,
                                   std::string& error,
                                   int max_attempts = DEFAULT_FEE_SELECTION_ATTEMPTS) const;

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

} // namespace DigiDollar

#endif // DIGIBYTE_DIGIDOLLAR_TXBUILDER_H
