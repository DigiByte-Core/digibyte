// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <digidollar/digidollar.h>
#include <digidollar/scripts.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <key.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

void initialize_dd_txbuilder()
{
    ECC_Start();
    SelectParams(ChainType::REGTEST);
}

/** Generate a valid CKey from fuzz data. Returns invalid key if not enough bytes. */
CKey MakeFuzzKey(FuzzedDataProvider& fdp)
{
    CKey key;
    auto bytes = fdp.ConsumeBytes<uint8_t>(32);
    if (bytes.size() == 32) {
        key.Set(bytes.begin(), bytes.end(), true);
    }
    return key;
}

/** Where a redemption sends the collateral it unlocks. The build refuses when
 *  there is no address for it, so pick one most of the time and leave it out
 *  now and then to cover the refusal too. */
std::optional<CTxDestination> MakeFuzzCollateralDest(FuzzedDataProvider& fdp)
{
    const CKey key = MakeFuzzKey(fdp);
    const int choice = fdp.ConsumeIntegralInRange<int>(0, 3);
    if (!key.IsValid() || choice == 0) return std::nullopt;
    if (choice == 1) return CTxDestination{WitnessV1Taproot(XOnlyPubKey(key.GetPubKey()))};
    if (choice == 2) return CTxDestination{WitnessV0KeyHash(key.GetPubKey())};
    return CTxDestination{PKHash(key.GetPubKey())};
}

/** Build a fake outpoint from fuzz data */
COutPoint MakeFuzzOutpoint(FuzzedDataProvider& fdp)
{
    uint256 hash;
    auto hash_bytes = fdp.ConsumeBytes<uint8_t>(32);
    if (hash_bytes.size() == 32) {
        memcpy(hash.begin(), hash_bytes.data(), 32);
    }
    uint32_t n = fdp.ConsumeIntegral<uint32_t>();
    return COutPoint(hash, n);
}

} // namespace

FUZZ_TARGET(dd_txbuilder_mint, .init = initialize_dd_txbuilder)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const auto& chainParams = Params();

    // Fuzz builder constructor parameters
    int height = fdp.ConsumeIntegralInRange<int>(0, 10'000'000);
    CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000); // micro-USD

    DigiDollar::MintTxBuilder builder(chainParams, height, oraclePrice);

    // Fuzz mint parameters
    DigiDollar::TxBuilderMintParams params;
    params.ddAmount = fdp.ConsumeIntegral<CAmount>();
    params.lockDays = fdp.ConsumeIntegralInRange<int>(0, 20'000);
    params.lockTier = fdp.ConsumeIntegralInRange<uint32_t>(0, 15);
    params.feeRate = fdp.ConsumeIntegral<CAmount>();
    params.ownerKey = MakeFuzzKey(fdp);

    // Add 1-5 fuzzed UTXOs
    int numUtxos = fdp.ConsumeIntegralInRange<int>(0, 5);
    for (int i = 0; i < numUtxos; ++i) {
        params.utxos.push_back(MakeFuzzOutpoint(fdp));
    }

    // Exercise BuildMintTransaction — must not crash
    DigiDollar::TxBuilderResult result = builder.BuildMintTransaction(params);

    // If it succeeded, do basic sanity checks
    if (result.success) {
        assert(result.error.empty());
        assert(result.tx.vout.size() >= 2); // At least collateral + DD output
        assert(result.collateralRequired > 0);
    }

    // Also exercise the standalone helpers — must not crash
    (void)builder.CalculateRequiredCollateral(params.ddAmount, params.lockDays);
    (void)builder.LockDaysToBlocks(params.lockDays);
    (void)builder.ValidateAmount(params.ddAmount);
    (void)builder.ValidateFeeRate(params.feeRate);
}

FUZZ_TARGET(dd_txbuilder_redeem, .init = initialize_dd_txbuilder)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const auto& chainParams = Params();

    int height = fdp.ConsumeIntegralInRange<int>(0, 10'000'000);
    CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000);

    DigiDollar::RedeemTxBuilder builder(chainParams, height, oraclePrice);

    // Fuzz redeem parameters
    DigiDollar::TxBuilderRedeemParams params;
    params.collateralOutpoint = MakeFuzzOutpoint(fdp);
    params.ddToRedeem = fdp.ConsumeIntegral<CAmount>();
    params.path = fdp.ConsumeBool() ? DigiDollar::RedemptionPath::NORMAL
                                    : DigiDollar::RedemptionPath::ERR;
    params.ownerKey = MakeFuzzKey(fdp);
    params.collateralDest = MakeFuzzCollateralDest(fdp);
    params.feeRate = fdp.ConsumeIntegral<CAmount>();

    // Pre-queried position data (avoids UTXO lookups)
    params.collateralAmount = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
    params.ddMinted = fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY / 2);
    params.unlockHeight = fdp.ConsumeIntegralInRange<uint32_t>(0, 10'000'000);

    // DD UTXOs to burn
    int numDDUtxos = fdp.ConsumeIntegralInRange<int>(0, 5);
    for (int i = 0; i < numDDUtxos; ++i) {
        params.ddUtxos.push_back(MakeFuzzOutpoint(fdp));
        params.ddAmounts.push_back(fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY / 10));
    }

    // Fee UTXOs
    int numFeeUtxos = fdp.ConsumeIntegralInRange<int>(0, 3);
    for (int i = 0; i < numFeeUtxos; ++i) {
        params.feeUtxos.push_back(MakeFuzzOutpoint(fdp));
        params.feeAmounts.push_back(fdp.ConsumeIntegralInRange<CAmount>(0, 100 * COIN));
    }

    // Exercise BuildRedemptionTransaction — must not crash
    DigiDollar::TxBuilderResult result = builder.BuildRedemptionTransaction(params);

    if (result.success) {
        assert(result.error.empty());
        assert(!result.tx.vin.empty());
        assert(!result.tx.vout.empty());
    }

    // Exercise standalone helpers
    (void)builder.DetermineRedemptionPath(params);
    (void)builder.CalculateCollateralReturn(
        params.ddToRedeem,
        params.collateralAmount,
        oraclePrice);

    // Exercise CreateRedemptionScript if we have a valid key
    if (params.ownerKey.IsValid()) {
        (void)builder.CreateRedemptionScript(DigiDollar::RedemptionPath::NORMAL, params.ownerKey);
        (void)builder.CreateRedemptionScript(DigiDollar::RedemptionPath::ERR, params.ownerKey);
    }
}

FUZZ_TARGET(dd_txbuilder_transfer, .init = initialize_dd_txbuilder)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const auto& chainParams = Params();

    int height = fdp.ConsumeIntegralInRange<int>(0, 10'000'000);
    CAmount oraclePrice = fdp.ConsumeIntegralInRange<CAmount>(1, 100'000'000);

    DigiDollar::TransferTxBuilder builder(chainParams, height, oraclePrice);

    // Fuzz transfer parameters
    DigiDollar::TxBuilderTransferParams params;
    params.feeRate = fdp.ConsumeIntegral<CAmount>();
    params.spenderKey = MakeFuzzKey(fdp);

    // Add 1-3 recipients with fuzzed DD addresses and amounts
    int numRecipients = fdp.ConsumeIntegralInRange<int>(0, 3);
    for (int i = 0; i < numRecipients; ++i) {
        // Generate a plausible DD address from a random key
        CKey recipientKey = MakeFuzzKey(fdp);
        std::string address;
        if (recipientKey.IsValid()) {
            CPubKey pub = recipientKey.GetPubKey();
            CTxDestination dest{WitnessV1Taproot(XOnlyPubKey(pub))};
            address = DigiDollar::EncodeDigiDollarAddress(dest, chainParams);
        } else {
            // Use a random string to test invalid address handling
            address = fdp.ConsumeRandomLengthString(64);
        }
        CAmount amount = fdp.ConsumeIntegral<CAmount>();
        params.recipients.push_back({address, amount});
    }

    // DD UTXOs
    int numDDUtxos = fdp.ConsumeIntegralInRange<int>(0, 5);
    for (int i = 0; i < numDDUtxos; ++i) {
        params.ddUtxos.push_back(MakeFuzzOutpoint(fdp));
        params.ddAmounts.push_back(fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY / 10));
    }

    // Fee UTXOs
    int numFeeUtxos = fdp.ConsumeIntegralInRange<int>(0, 3);
    for (int i = 0; i < numFeeUtxos; ++i) {
        params.feeUtxos.push_back(MakeFuzzOutpoint(fdp));
        params.feeAmounts.push_back(fdp.ConsumeIntegralInRange<CAmount>(0, 100 * COIN));
    }

    // Exercise BuildTransferTransaction — must not crash
    DigiDollar::TxBuilderResult result = builder.BuildTransferTransaction(params);

    if (result.success) {
        assert(result.error.empty());
        assert(!result.tx.vin.empty());
        assert(!result.tx.vout.empty());
    }

    // Exercise validation helper
    (void)builder.ValidateTransferParams(params);
}
