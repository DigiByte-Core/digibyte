// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <coins.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <script/script.h>
#include <key.h>
#include <logging.h>
#include <util/strencodings.h>
#include <util/hasher.h>
#include <sync.h>

#include <algorithm>
#include <map>

namespace DigiDollar {

XOnlyPubKey GetCollateralNUMSKey()
{
    XOnlyPubKey nums_key{Span<const unsigned char>(COLLATERAL_NUMS_POINT_BYTES.data(), 32)};
    return nums_key;
}

std::vector<XOnlyPubKey> GetOracleKeys(size_t count)
{
    std::vector<XOnlyPubKey> keys;
    keys.reserve(count);

    // Generate deterministic keys for testing (Phase 1)
    // In Phase 2, this will connect to real oracle infrastructure
    for (size_t i = 0; i < count; i++) {
        CKey key;
        // Use deterministic seed based on index for consistent testing
        // Initialize with a valid base seed and modify it to make it unique per index
        std::vector<unsigned char> seed(32);
        // Start with a valid seed base (0x01 repeated) to ensure validity
        for (size_t j = 0; j < 32; j++) {
            seed[j] = static_cast<unsigned char>((i + 1 + j) % 256);
        }
        // Ensure the key is non-zero and within the valid secp256k1 range
        seed[31] = static_cast<unsigned char>((i + 1) % 255 + 1);

        key.Set(seed.begin(), seed.end(), true);

        // Verify the key was initialized successfully
        if (!key.IsValid()) {
            // Fallback: use MakeNewKey with deterministic seed
            key.MakeNewKey(true);
        }

        keys.emplace_back(XOnlyPubKey(key.GetPubKey()));
    }

    // Don't log during test setup to avoid logging initialization issues
    // // LogPrintf("DigiDollar: Generated %d oracle keys for testing\n", count);
    return keys;
}

CScript CreateNormalRedemptionPath(const MintParams& params)
{
    if (params.ddAmount <= 0 || params.lockHeight < 0) {
        // // LogPrintf("DigiDollar: Invalid parameters for normal redemption path\n");
        return CScript();
    }

    CScript script;

    // Normal redemption after timelock
    script << params.lockHeight << OP_CHECKLOCKTIMEVERIFY << OP_DROP;

    // DD marker/amount ABI followed by owner signature verification.
    script << OP_DIGIDOLLAR << CScriptNum(params.ddAmount) << OP_DDVERIFY;
    script << ToByteVector(params.ownerKey) << OP_CHECKSIG;

    // // LogPrintf("DigiDollar: Created normal redemption path for %d DD at height %d\n",
    //           params.ddAmount, params.lockHeight);

    return script;
}

// NOTE: CreateEmergencyPath was removed - NO emergency oracle override exists
// Only 2 redemption paths: Normal and ERR (both require timelock expiry)

CScript CreateERRPath(const MintParams& params)
{
    if (params.ddAmount <= 0 || params.lockHeight < 0) {
        // LogPrintf("DigiDollar: Invalid parameters for ERR path\n");
        return CScript();
    }

    CScript script;

    // ERR path also requires timelock expiry (same as Normal path)
    // This ensures collateral is NEVER unlocked until timelock expires
    script << params.lockHeight << OP_CHECKLOCKTIMEVERIFY << OP_DROP;

    // Check if system collateral ratio < 100%.
    // The witness stack provides <signature> <collateralRatio>; OP_CHECKCOLLATERAL
    // verifies ratio >= threshold, so OP_NOT flips it into ratio < 100.
    script << CScriptNum(100) << OP_CHECKCOLLATERAL << OP_NOT << OP_VERIFY;

    // DigiDollar verification
    script << OP_DIGIDOLLAR << CScriptNum(params.ddAmount) << OP_DDVERIFY;

    // Owner signature
    script << ToByteVector(params.ownerKey) << OP_CHECKSIG;

    // LogPrintf("DigiDollar: Created ERR path for %d DD at height %d\n", params.ddAmount, params.lockHeight);

    return script;
}

CScript CreateCollateralP2TR(const MintParams& params, bool register_metadata)
{
    if (params.ddAmount <= 0 || params.lockHeight < 0 || !params.internalKey.IsFullyValid()) {
        // LogPrintf("DigiDollar: Invalid parameters for P2TR collateral script\n");
        return CScript();
    }

    try {
        // Use TaprootBuilder to create MAST
        TaprootBuilder builder;

        // DigiDollar uses exactly 2 MAST redemption paths:
        // 1. Normal path: CLTV + DD amount verification + owner signature
        // 2. ERR path: CLTV + collateral ratio check + DD amount verification + owner signature
        //
        // CRITICAL: Both paths REQUIRE the timelock (CLTV) to expire first.
        // There is NO early redemption, NO forced liquidation, NO exceptions.
        //
        // NOTE: Partial redemption and Emergency oracle override are NOT supported.
        // Users must redeem the full minted amount in a single transaction.

        // Normal path (most common) - depth 1
        CScript normalPath = CreateNormalRedemptionPath(params);
        if (!normalPath.empty()) {
            builder.Add(1, normalPath, 0xC0);  // Leaf version 0xC0 for Tapscript
        }

        // ERR path (rare - only when system < 100% collateralized) - depth 1
        CScript errPath = CreateERRPath(params);
        if (!errPath.empty()) {
            builder.Add(1, errPath, 0xC0);
        }

        // Finalize with internal key
        builder.Finalize(params.internalKey);

        if (!builder.IsValid() || !builder.IsComplete()) {
            // LogPrintf("DigiDollar: TaprootBuilder failed to create valid tree\n");
            return CScript();
        }

        // Create P2TR output script
        CScript scriptPubKey;
        WitnessV1Taproot output = builder.GetOutput();

        // P2TR format: OP_1 + 32-byte taproot output
        scriptPubKey << OP_1 << ToByteVector(output);

        // LogPrintf("DigiDollar: Created P2TR collateral script for %d DD (size: %d bytes)\n",
        //           params.ddAmount, scriptPubKey.size());

        // Phase 1: Register metadata for testing
        if (register_metadata) RegisterScriptMetadata(scriptPubKey, DigiDollar::ScriptType::COLLATERAL_LOCK, params.ddAmount, params.lockHeight);

        return scriptPubKey;

    } catch (const std::exception& e) {
        // LogPrintf("DigiDollar: Exception creating P2TR script: %s\n", e.what());
        return CScript();
    }
}

CScript CreateDigiDollarP2TR(const XOnlyPubKey& owner, CAmount ddAmount)
{
    if (ddAmount <= 0 || !owner.IsFullyValid()) {
        // LogPrintf("DigiDollar: Invalid parameters for DD P2TR script\n");
        return CScript();
    }

    try {
        // Standard Taproot P2TR output with tweaked key
        // The owner's x-only pubkey is tweaked with nullptr merkle root (key-path only)
        // This is standard BIP-341 behavior for simple P2TR outputs.
        //
        // When signing, the wallet must apply the same tweak to the private key.
        // For minted DD: owner key is stored, tweak is applied during signing
        // For received DD: wallet already knows the tweaked key from the output
        auto tweaked = owner.CreateTapTweak(nullptr);  // nullptr = no merkle root, key-path only
        if (!tweaked) {
            return CScript();
        }
        XOnlyPubKey output_key = tweaked->first;

        // Create P2TR output with the TWEAKED key (standard Taproot)
        CScript scriptPubKey;
        scriptPubKey << OP_1 << ToByteVector(output_key);

        // LogPrintf("DigiDollar: Created DD P2TR script for %d DD (size: %d bytes)\n",
        //           ddAmount, scriptPubKey.size());

        // Phase 1: Register metadata for testing
        RegisterScriptMetadata(scriptPubKey, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, ddAmount, 0);

        return scriptPubKey;

    } catch (const std::exception& e) {
        // LogPrintf("DigiDollar: Exception creating DD P2TR script: %s\n", e.what());
        return CScript();
    }
}

// ============================================================================
// Phase 1 Script Metadata Tracking
// ============================================================================
// IMPORTANT: This is a Phase 1 testing workaround. In production (Phase 2),
// DD amounts and script types should be tracked in the UTXO database.
// This global map allows tests to identify scripts created by Create*P2TR functions.

static std::map<uint256, ScriptMetadata> g_scriptMetadataMap;
static RecursiveMutex g_scriptMetadataMutex;

// BUG #9 FIX: Limit map size to prevent unbounded memory growth
static constexpr size_t MAX_SCRIPT_METADATA_ENTRIES = 10000;

void RegisterScriptMetadata(const CScript& script, DigiDollar::ScriptType type, CAmount ddAmount, int64_t lockHeight) {
    uint256 scriptHash = Hash(script);
    LOCK(g_scriptMetadataMutex);

    // Evict oldest entries if map is too large (simple FIFO via erase from begin)
    while (g_scriptMetadataMap.size() >= MAX_SCRIPT_METADATA_ENTRIES) {
        g_scriptMetadataMap.erase(g_scriptMetadataMap.begin());
    }

    g_scriptMetadataMap[scriptHash] = {type, ddAmount, lockHeight};
}

bool GetScriptMetadata(const CScript& script, ScriptMetadata& metadata) {
    uint256 scriptHash = Hash(script);
    LOCK(g_scriptMetadataMutex);
    auto it = g_scriptMetadataMap.find(scriptHash);
    if (it != g_scriptMetadataMap.end()) {
        metadata = it->second;
        return true;
    }
    return false;
}

bool IsRegisteredCollateralVaultScript(const CScript& script)
{
    ScriptMetadata metadata;
    return GetScriptMetadata(script, metadata) &&
           metadata.type == DigiDollar::ScriptType::COLLATERAL_LOCK;
}

bool SpendsRegisteredCollateralVault(const CTransaction& tx, const CCoinsViewCache& coins)
{
    for (const CTxIn& txin : tx.vin) {
        Coin coin;
        if (!coins.GetCoin(txin.prevout, coin)) {
            continue;
        }
        if (IsRegisteredCollateralVaultScript(coin.out.scriptPubKey)) {
            return true;
        }
    }
    return false;
}

} // namespace DigiDollar
