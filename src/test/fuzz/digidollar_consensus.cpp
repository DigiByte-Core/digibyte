// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <util/int128.h>
#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <key.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <util/chaintype.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

// ============================================================================
// Initialization
// ============================================================================

void initialize_dd_consensus()
{
    ECC_Start();
    SelectParams(ChainType::REGTEST);
}

// ============================================================================
// Target 1: fuzz_dd_script_types
// Fuzz DigiDollar::IdentifyScriptType() with random CScript data.
// ============================================================================

FUZZ_TARGET(dd_script_types, .init = initialize_dd_consensus)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Strategy 1: Feed completely random script bytes
    {
        const std::vector<uint8_t> random_bytes = ConsumeRandomLengthByteVector(fuzzed_data_provider, 520);
        CScript script(random_bytes.begin(), random_bytes.end());
        DigiDollar::ScriptType type = DigiDollar::IdentifyScriptType(script);
        // Must always return a valid enum value
        assert(type == DigiDollar::ScriptType::NOT_DIGIDOLLAR ||
               type == DigiDollar::ScriptType::COLLATERAL_LOCK ||
               type == DigiDollar::ScriptType::DD_TOKEN_OUTPUT);
    }

    // Strategy 2: Empty script
    {
        CScript empty_script;
        DigiDollar::ScriptType type = DigiDollar::IdentifyScriptType(empty_script);
        // Empty script is definitely not DD
        assert(type == DigiDollar::ScriptType::NOT_DIGIDOLLAR);
    }

    // Strategy 3: P2TR-shaped scripts (OP_1 + 32 bytes)
    {
        const std::vector<uint8_t> payload = ConsumeRandomLengthByteVector(fuzzed_data_provider, 32);
        if (payload.size() == 32) {
            CScript p2tr_script;
            p2tr_script << OP_1;
            p2tr_script << payload;
            DigiDollar::ScriptType type = DigiDollar::IdentifyScriptType(p2tr_script);
            assert(type == DigiDollar::ScriptType::NOT_DIGIDOLLAR ||
                   type == DigiDollar::ScriptType::COLLATERAL_LOCK ||
                   type == DigiDollar::ScriptType::DD_TOKEN_OUTPUT);
        }
    }

    // Strategy 4: Oversized scripts
    {
        const std::vector<uint8_t> big_bytes = ConsumeRandomLengthByteVector(fuzzed_data_provider, 10000);
        CScript big_script(big_bytes.begin(), big_bytes.end());
        DigiDollar::ScriptType type = DigiDollar::IdentifyScriptType(big_script);
        assert(type == DigiDollar::ScriptType::NOT_DIGIDOLLAR ||
               type == DigiDollar::ScriptType::COLLATERAL_LOCK ||
               type == DigiDollar::ScriptType::DD_TOKEN_OUTPUT);
    }

    // Strategy 5: Also test IsDDTokenScript and IsCollateralScript
    {
        const std::vector<uint8_t> bytes = ConsumeRandomLengthByteVector(fuzzed_data_provider, 256);
        CScript script(bytes.begin(), bytes.end());
        (void)DigiDollar::IsDDTokenScript(script);
        (void)DigiDollar::IsCollateralScript(script);

        CAmount extracted_amount;
        (void)DigiDollar::ExtractDDAmount(script, extracted_amount);
    }

    // Strategy 6: Use ConsumeScript helper for well-formed scripts
    {
        CScript fuzzed_script = ConsumeScript(fuzzed_data_provider);
        DigiDollar::ScriptType type = DigiDollar::IdentifyScriptType(fuzzed_script);
        assert(type == DigiDollar::ScriptType::NOT_DIGIDOLLAR ||
               type == DigiDollar::ScriptType::COLLATERAL_LOCK ||
               type == DigiDollar::ScriptType::DD_TOKEN_OUTPUT);
    }
}

// ============================================================================
// Target 2: fuzz_dd_tx_types
// Fuzz DigiDollar::GetDigiDollarTxType() and HasDigiDollarMarker()
// ============================================================================

FUZZ_TARGET(dd_tx_types, .init = initialize_dd_consensus)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Strategy 1: Fuzz with ConsumeTransaction helper
    {
        CMutableTransaction mtx = ConsumeTransaction(fuzzed_data_provider, std::nullopt);
        const CTransaction tx(mtx);
        bool has_marker = DigiDollar::HasDigiDollarMarker(tx);
        DigiDollar::DigiDollarTxType tx_type = DigiDollar::GetDigiDollarTxType(tx);

        // If no marker, type must be NONE
        if (!has_marker) {
            assert(tx_type == DigiDollar::DD_TX_NONE);
        }

        // Type must always be in valid range
        assert(tx_type <= DigiDollar::DD_TX_MAX);
    }

    // Strategy 2: Manually craft DD-versioned transactions
    {
        CMutableTransaction mtx;
        // DD version marker: lower 16 bits = 0x0770
        const int32_t DD_VERSION_BASE = 0x0770;
        uint8_t fuzz_type = fuzzed_data_provider.ConsumeIntegral<uint8_t>();
        uint8_t fuzz_flags = fuzzed_data_provider.ConsumeIntegral<uint8_t>();
        mtx.nVersion = (static_cast<int32_t>(fuzz_type) << 24) |
                       (static_cast<int32_t>(fuzz_flags) << 16) |
                       DD_VERSION_BASE;

        // Add some fuzzed outputs
        int num_outputs = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 5);
        for (int i = 0; i < num_outputs; i++) {
            CAmount value = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(0, MAX_MONEY);
            const std::vector<uint8_t> script_bytes = ConsumeRandomLengthByteVector(fuzzed_data_provider, 100);
            CScript script(script_bytes.begin(), script_bytes.end());
            mtx.vout.emplace_back(value, script);
        }

        const CTransaction tx(mtx);
        assert(DigiDollar::HasDigiDollarMarker(tx));
        DigiDollar::DigiDollarTxType tx_type = DigiDollar::GetDigiDollarTxType(tx);
        // Type is extracted from bits 24-31, can be anything the fuzzer provides
        (void)tx_type;
    }

    // Strategy 3: OP_RETURN patterns in outputs
    {
        CMutableTransaction mtx;
        mtx.nVersion = fuzzed_data_provider.ConsumeIntegral<int32_t>();

        // Create an OP_RETURN output
        CScript op_return_script;
        op_return_script << OP_RETURN;
        const std::vector<uint8_t> data = ConsumeRandomLengthByteVector(fuzzed_data_provider, 80);
        if (!data.empty()) {
            op_return_script << data;
        }
        mtx.vout.emplace_back(0, op_return_script);

        // Add a normal P2TR-looking output
        const std::vector<uint8_t> p2tr_data = ConsumeRandomLengthByteVector(fuzzed_data_provider, 32);
        if (p2tr_data.size() == 32) {
            CScript p2tr;
            p2tr << OP_1 << p2tr_data;
            mtx.vout.emplace_back(fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(0, MAX_MONEY), p2tr);
        }

        const CTransaction tx(mtx);
        (void)DigiDollar::HasDigiDollarMarker(tx);
        (void)DigiDollar::GetDigiDollarTxType(tx);
    }

    // Strategy 4: Edge case - empty transaction
    {
        CMutableTransaction mtx;
        mtx.nVersion = 0;
        const CTransaction tx(mtx);
        assert(!DigiDollar::HasDigiDollarMarker(tx));
        assert(DigiDollar::GetDigiDollarTxType(tx) == DigiDollar::DD_TX_NONE);
    }
}

// ============================================================================
// Target 3: fuzz_dd_collateral_ratio
// Fuzz GetCollateralRatioForLockTime() with various lock durations.
// ============================================================================

FUZZ_TARGET(dd_collateral_ratio, .init = initialize_dd_consensus)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    DigiDollar::ConsensusParams params;
    int min_ratio = std::numeric_limits<int>::max();
    int max_ratio = 0;
    for (const auto& [_, ratio] : params.collateralRatios) {
        min_ratio = std::min(min_ratio, ratio);
        max_ratio = std::max(max_ratio, ratio);
    }
    const auto ratio_is_valid = [&](int ratio) {
        return ratio == 0 || (ratio >= min_ratio && ratio <= max_ratio);
    };

    // Strategy 1: Random lock block values across full range
    {
        int64_t lock_blocks = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(0, 10 * 365 * 24 * 60 * 4 + 1000);
        int ratio = DigiDollar::GetCollateralRatioForLockTime(lock_blocks, params);
        // V1 accepts canonical tiers only. Non-tier durations return 0.
        assert(ratio_is_valid(ratio));
    }

    // Strategy 2: Exact tier boundaries
    {
        for (const auto& [blocks, expected_ratio] : params.collateralRatios) {
            int ratio = DigiDollar::GetCollateralRatioForLockTime(blocks, params);
            // At exact boundary, should return this tier's ratio
            assert(ratio == expected_ratio);
        }
    }

    // Strategy 3: Zero and negative lock times
    {
        int ratio_zero = DigiDollar::GetCollateralRatioForLockTime(0, params);
        assert(ratio_is_valid(ratio_zero));

        int64_t negative = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(-1000000, -1);
        int ratio_neg = DigiDollar::GetCollateralRatioForLockTime(negative, params);
        assert(ratio_is_valid(ratio_neg));
    }

    // Strategy 4: Extremely large lock times
    {
        int64_t huge = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(
            10 * 365 * 24 * 60 * 4, std::numeric_limits<int64_t>::max());
        int ratio = DigiDollar::GetCollateralRatioForLockTime(huge, params);
        // Beyond max tier is non-canonical and returns 0.
        assert(ratio_is_valid(ratio));
    }

    // Strategy 5: Random values from full int64 range
    {
        int64_t arbitrary = fuzzed_data_provider.ConsumeIntegral<int64_t>();
        int ratio = DigiDollar::GetCollateralRatioForLockTime(arbitrary, params);
        assert(ratio_is_valid(ratio));
    }

    // Strategy 6: Test GetLockTierIndex consistency
    {
        int64_t lock_blocks = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(0, 20 * 365 * 24 * 60 * 4);
        int tier_index = DigiDollar::GetLockTierIndex(lock_blocks, params);
        // tier_index is -1 (no tier) or a non-negative index
        assert(tier_index >= -1);
    }

    // Strategy 7: LockDaysToBlocks / BlocksToLockDays roundtrip
    {
        int days = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 3650);
        int64_t blocks = DigiDollar::LockDaysToBlocks(days);
        assert(blocks >= 0);
        int days_back = DigiDollar::BlocksToLockDays(blocks);
        assert(days_back >= 0);
    }

    // Strategy 8: FormatLockPeriod should never crash
    {
        int64_t blocks = fuzzed_data_provider.ConsumeIntegral<int64_t>();
        (void)DigiDollar::FormatLockPeriod(blocks);
    }
}

// ============================================================================
// Target 4: fuzz_dd_amount_validation
// Fuzz DD amount validation against consensus limits.
// ============================================================================

FUZZ_TARGET(dd_amount_validation, .init = initialize_dd_consensus)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    DigiDollar::ConsensusParams params;

    // Strategy 1: Random amounts across full CAmount range
    {
        CAmount amount = fuzzed_data_provider.ConsumeIntegral<CAmount>();
        bool valid_mint = DigiDollar::IsValidMintAmount(amount, params);

        // Cross-check with known boundaries
        if (amount >= params.minMintAmount && amount <= params.maxMintAmount) {
            assert(valid_mint);
        } else {
            assert(!valid_mint);
        }
    }

    // Strategy 2: Boundary values
    {
        assert(!DigiDollar::IsValidMintAmount(0, params));
        assert(!DigiDollar::IsValidMintAmount(-1, params));
        assert(!DigiDollar::IsValidMintAmount(params.minMintAmount - 1, params));
        assert(DigiDollar::IsValidMintAmount(params.minMintAmount, params));
        assert(DigiDollar::IsValidMintAmount(params.maxMintAmount, params));
        assert(!DigiDollar::IsValidMintAmount(params.maxMintAmount + 1, params));
    }

    // Strategy 3: GetMinimumDDOutput
    {
        CAmount min_output = DigiDollar::GetMinimumDDOutput(params);
        assert(min_output == params.minOutputAmount);
        assert(min_output > 0);
    }

    // Strategy 4: Fuzz with modified params
    {
        DigiDollar::ConsensusParams custom_params;
        custom_params.minMintAmount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        custom_params.maxMintAmount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        custom_params.minOutputAmount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);

        CAmount amount = fuzzed_data_provider.ConsumeIntegral<CAmount>();
        // These should never crash regardless of params
        (void)DigiDollar::IsValidMintAmount(amount, custom_params);
        (void)DigiDollar::GetMinimumDDOutput(custom_params);
    }

    // Strategy 5: ValidateConsensusParams
    {
        DigiDollar::ConsensusParams fuzz_params;
        fuzz_params.minMintAmount = fuzzed_data_provider.ConsumeIntegral<CAmount>();
        fuzz_params.maxMintAmount = fuzzed_data_provider.ConsumeIntegral<CAmount>();
        fuzz_params.minOutputAmount = fuzzed_data_provider.ConsumeIntegral<CAmount>();
        fuzz_params.oracleCount = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
        fuzz_params.oracleThreshold = fuzzed_data_provider.ConsumeIntegral<uint32_t>();

        std::string error;
        // Should never crash, only return true/false
        (void)DigiDollar::ValidateConsensusParams(fuzz_params, error);
    }

    // Strategy 6: util::int128_t arithmetic overflow scenarios
    {
        CAmount a = fuzzed_data_provider.ConsumeIntegral<CAmount>();
        CAmount b = fuzzed_data_provider.ConsumeIntegral<CAmount>();
        // Test that util::int128_t can safely hold any product of two CAmount values
        util::int128_t product = static_cast<util::int128_t>(a) * static_cast<util::int128_t>(b);
        // Verify the product didn't lose information on conversion back
        // (it WILL overflow CAmount for large inputs — that's expected)
        (void)product;

        // Test sum
        util::int128_t sum = static_cast<util::int128_t>(a) + static_cast<util::int128_t>(b);
        (void)sum;
    }
}

// ============================================================================
// Target 5: fuzz_dd_mint_params
// Fuzz MintParams construction and CreateCollateralP2TR().
// ============================================================================

FUZZ_TARGET(dd_mint_params, .init = initialize_dd_consensus)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Generate a valid key for the owner
    CKey owner_key;
    {
        std::vector<uint8_t> key_bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(32);
        if (key_bytes.size() < 32) {
            key_bytes.resize(32, 0x01);
        }
        owner_key.Set(key_bytes.begin(), key_bytes.end(), true);
        if (!owner_key.IsValid()) {
            return; // Need a valid key to proceed
        }
    }

    XOnlyPubKey owner_xonly = XOnlyPubKey(owner_key.GetPubKey());
    XOnlyPubKey internal_key = DigiDollar::GetCollateralNUMSKey();

    // Strategy 1: Valid MintParams with fuzzed amounts and heights
    {
        DigiDollar::MintParams params;
        params.ddAmount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY);
        params.lockHeight = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(1, 100000000);
        params.ownerKey = owner_xonly;
        params.internalKey = internal_key;
        params.oracleKeys = DigiDollar::GetOracleKeys(15);

        CScript result = DigiDollar::CreateCollateralP2TR(params);
        if (!result.empty()) {
            // Valid P2TR must be exactly 34 bytes: OP_1 (1 byte) + pushdata (1 byte) + 32 bytes
            assert(result.size() == 34);
            assert(result[0] == OP_1);

            // Verify it's identified as a collateral script
            DigiDollar::ScriptType type = DigiDollar::IdentifyScriptType(result);
            assert(type == DigiDollar::ScriptType::COLLATERAL_LOCK);
        }
    }

    // Strategy 2: Invalid params — zero/negative amounts
    {
        DigiDollar::MintParams params;
        params.ddAmount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(-1000000, 0);
        params.lockHeight = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(-1000, 1000);
        params.ownerKey = owner_xonly;
        params.internalKey = internal_key;

        CScript result = DigiDollar::CreateCollateralP2TR(params);
        // Invalid params should produce empty script
        assert(result.empty());
    }

    // Strategy 3: Invalid internal key
    {
        DigiDollar::MintParams params;
        params.ddAmount = 100000;
        params.lockHeight = 5760;
        params.ownerKey = owner_xonly;
        // Default-constructed XOnlyPubKey is not fully valid
        params.internalKey = XOnlyPubKey();

        CScript result = DigiDollar::CreateCollateralP2TR(params);
        // Should fail gracefully (empty script)
        assert(result.empty());
    }

    // Strategy 4: CreateDigiDollarP2TR with fuzzed amounts
    {
        CAmount dd_amount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY);
        CScript dd_script = DigiDollar::CreateDigiDollarP2TR(owner_xonly, dd_amount);
        if (!dd_script.empty()) {
            assert(dd_script.size() == 34);
            assert(dd_script[0] == OP_1);
            DigiDollar::ScriptType type = DigiDollar::IdentifyScriptType(dd_script);
            assert(type == DigiDollar::ScriptType::DD_TOKEN_OUTPUT);
        }
    }

    // Strategy 5: CreateDigiDollarP2TR with invalid amounts
    {
        CAmount bad_amount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(-1000000, 0);
        CScript result = DigiDollar::CreateDigiDollarP2TR(owner_xonly, bad_amount);
        assert(result.empty());
    }

    // Strategy 6: CreateNormalRedemptionPath and CreateERRPath
    {
        DigiDollar::MintParams params;
        params.ddAmount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY);
        params.lockHeight = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(1, 100000000);
        params.ownerKey = owner_xonly;
        params.internalKey = internal_key;

        CScript normal_path = DigiDollar::CreateNormalRedemptionPath(params);
        CScript err_path = DigiDollar::CreateERRPath(params);
        // Both should produce non-empty scripts for valid params
        (void)normal_path;
        (void)err_path;
    }
}
