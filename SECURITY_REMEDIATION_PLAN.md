# DigiByte Core v8.26 DigiDollar Security Remediation Plan

**Document Version:** 1.0  
**Date:** 2026-02-03  
**Prepared by:** Security Audit Team  
**Status:** Ready for Implementation

---

## Executive Summary

This document provides detailed remediation plans for the 7 security vulnerabilities identified during the comprehensive security assessment of DigiByte Core v8.26, specifically focusing on the DigiDollar stablecoin implementation, Oracle price feed system, and consensus mechanisms.

### Priority Matrix

| Priority | Vulnerabilities | Timeline |
|----------|-----------------|----------|
| **P0 - Critical** | DGB-SEC-001, DGB-SEC-002 | Immediate (0-7 days) |
| **P1 - High** | DGB-SEC-003, DGB-SEC-004, DGB-SEC-007 | Short-term (1-4 weeks) |
| **P2 - Medium** | DGB-SEC-005, DGB-SEC-006 | Long-term (1-3 months) |

---

## DGB-SEC-001: Integer Overflow in Collateral Ratio Validation

### Vulnerability Summary
- **Severity:** HIGH
- **CVSS Score:** 7.5
- **Location:** `src/consensus/digidollar_transaction_validation.cpp:36-43`
- **Function:** `ValidateCollateralRatio()`

### Root Cause Analysis
The multiplication `collateralAmount * oraclePrice` can overflow `int64_t` when:
- `collateralAmount` approaches `MAX_MONEY` (21 billion DGB × 10^8 satoshis = 2.1×10^18)
- `oraclePrice` is in the expected range (up to 10^6 for price in cents)
- Product: 2.1×10^24 which exceeds `INT64_MAX` (9.2×10^18)

### Detailed Remediation Plan

#### Step 1: Implement Safe Multiplication Pattern
Replace the vulnerable code with the safe pattern already used in `src/consensus/dca.cpp`:

```cpp
// BEFORE (Vulnerable)
bool ValidateCollateralRatio(CAmount ddAmount, CAmount collateralAmount, CAmount oraclePrice, int requiredRatio) {
    if (oraclePrice <= 0) return false;
    if (ddAmount <= 0) return false;
    if (collateralAmount <= 0) return false;

    CAmount requiredUSDCents = (ddAmount * requiredRatio) / 100;
    CAmount collateralValueCents = (collateralAmount * oraclePrice) / COIN;

    return collateralValueCents >= requiredUSDCents;
}

// AFTER (Safe)
bool ValidateCollateralRatio(CAmount ddAmount, CAmount collateralAmount, CAmount oraclePrice, int requiredRatio) {
    if (oraclePrice <= 0) return false;
    if (ddAmount <= 0) return false;
    if (collateralAmount <= 0) return false;
    if (requiredRatio <= 0 || requiredRatio > 10000) return false; // Sanity check ratio

    // Safe calculation for requiredUSDCents: avoid overflow by reordering
    // requiredUSDCents = ddAmount * requiredRatio / 100
    // Split: (ddAmount / 100) * requiredRatio + (ddAmount % 100) * requiredRatio / 100
    CAmount ddQuotient = ddAmount / 100;
    CAmount ddRemainder = ddAmount % 100;
    
    // Check for potential overflow before multiplication
    if (ddQuotient > std::numeric_limits<CAmount>::max() / requiredRatio) {
        return false; // Would overflow - reject as invalid
    }
    CAmount requiredUSDCents = ddQuotient * requiredRatio + (ddRemainder * requiredRatio) / 100;

    // Safe calculation for collateralValueCents: divide by COIN first
    // collateralValueCents = collateralAmount * oraclePrice / COIN
    // Split: (collateralAmount / COIN) * oraclePrice + (collateralAmount % COIN) * oraclePrice / COIN
    CAmount collateralDGB = collateralAmount / COIN;
    CAmount collateralRemainder = collateralAmount % COIN;
    
    // Check for potential overflow before multiplication
    if (collateralDGB > std::numeric_limits<CAmount>::max() / oraclePrice) {
        // Extremely large collateral - likely invalid or attack
        return false;
    }
    CAmount collateralValueCents = collateralDGB * oraclePrice + 
                                   (collateralRemainder * oraclePrice) / COIN;

    return collateralValueCents >= requiredUSDCents;
}
```

#### Step 2: Add Unit Tests

Create test file `src/test/digidollar_overflow_tests.cpp`:

```cpp
#include <boost/test/unit_test.hpp>
#include <consensus/digidollar_transaction_validation.h>
#include <consensus/amount.h>
#include <limits>

BOOST_AUTO_TEST_SUITE(digidollar_overflow_tests)

BOOST_AUTO_TEST_CASE(collateral_ratio_overflow_protection)
{
    // Test case 1: Normal operation
    BOOST_CHECK(ValidateCollateralRatio(1000, 100 * COIN, 5000, 200)); // $10 DD, 100 DGB @ $50
    
    // Test case 2: Large but valid collateral
    BOOST_CHECK(ValidateCollateralRatio(100000000, 1000000 * COIN, 100000, 200));
    
    // Test case 3: Potential overflow case - should not crash
    CAmount maxCollateral = MAX_MONEY;
    CAmount highPrice = 1000000; // $10 per DGB
    // This would overflow without protection: maxCollateral * highPrice > INT64_MAX
    BOOST_CHECK_NO_THROW(ValidateCollateralRatio(1000, maxCollateral, highPrice, 200));
    
    // Test case 4: Edge case - exactly at overflow boundary
    CAmount overflowBoundary = std::numeric_limits<CAmount>::max() / 1000000;
    BOOST_CHECK_NO_THROW(ValidateCollateralRatio(1000, overflowBoundary * COIN, 1000000, 200));
    
    // Test case 5: Invalid inputs should return false, not crash
    BOOST_CHECK(!ValidateCollateralRatio(-1, 100 * COIN, 5000, 200));
    BOOST_CHECK(!ValidateCollateralRatio(1000, -1, 5000, 200));
    BOOST_CHECK(!ValidateCollateralRatio(1000, 100 * COIN, -1, 200));
    BOOST_CHECK(!ValidateCollateralRatio(1000, 100 * COIN, 5000, -1));
}

BOOST_AUTO_TEST_SUITE_END()
```

#### Step 3: Add Overflow-Safe Utility Functions (Optional Enhancement)

Create reusable safe arithmetic in `src/util/overflow.h`:

```cpp
#ifndef DIGIBYTE_UTIL_OVERFLOW_H
#define DIGIBYTE_UTIL_OVERFLOW_H

#include <consensus/amount.h>
#include <limits>
#include <optional>

namespace util {

/**
 * Safely multiply two CAmount values, returning nullopt on overflow
 */
inline std::optional<CAmount> SafeMultiply(CAmount a, CAmount b) {
    if (a == 0 || b == 0) return 0;
    if (a > 0 && b > 0 && a > std::numeric_limits<CAmount>::max() / b) {
        return std::nullopt;
    }
    if (a < 0 && b < 0 && a < std::numeric_limits<CAmount>::max() / b) {
        return std::nullopt;
    }
    if ((a > 0 && b < 0 && b < std::numeric_limits<CAmount>::min() / a) ||
        (a < 0 && b > 0 && a < std::numeric_limits<CAmount>::min() / b)) {
        return std::nullopt;
    }
    return a * b;
}

/**
 * Safely add two CAmount values, returning nullopt on overflow
 */
inline std::optional<CAmount> SafeAdd(CAmount a, CAmount b) {
    if (b > 0 && a > std::numeric_limits<CAmount>::max() - b) {
        return std::nullopt;
    }
    if (b < 0 && a < std::numeric_limits<CAmount>::min() - b) {
        return std::nullopt;
    }
    return a + b;
}

} // namespace util

#endif // DIGIBYTE_UTIL_OVERFLOW_H
```

#### Verification Steps
1. Run unit tests: `./src/test/test_digibyte --run_test=digidollar_overflow_tests`
2. Run functional tests: `./test/functional/test_runner.py digidollar_*`
3. Manual testing with edge case transactions on regtest

#### Rollback Plan
If issues arise, revert to original code with explicit documentation that large collateral amounts are rejected at the RPC layer.

---

## DGB-SEC-002: Oracle Signature Verification Bypass (Phase 1)

### Vulnerability Summary
- **Severity:** HIGH
- **CVSS Score:** 7.8
- **Location:** `src/oracle/bundle_manager.cpp:273-295`
- **Function:** `AddOracleBundleToBlock()`

### Root Cause Analysis
In Phase 1 mode (1-of-1 oracle consensus), bundle creation uses pending messages without re-verifying signatures at the time of block inclusion, creating a TOCTOU (Time-Of-Check-To-Time-Of-Use) vulnerability.

### Detailed Remediation Plan

#### Step 1: Add Signature Re-verification at Bundle Creation

```cpp
// BEFORE (Vulnerable)
if (!pending.empty()) {
    bundle = COracleBundle(epoch);
    bundle.messages = pending;
    bundle.median_price_micro_usd = pending[0].price_micro_usd;
    LogPrintf("Oracle: Phase One - Using %zu pending message(s) for block %d\n",
             pending.size(), block_height);
    pending_messages.clear();
}

// AFTER (Safe)
if (!pending.empty()) {
    // CRITICAL: Re-verify all signatures before committing to block
    bool allValid = true;
    for (const auto& msg : pending) {
        // Verify signature using Phase 2 hash (canonical format)
        if (!msg.VerifyPhase2()) {
            LogPrintf("Oracle: Phase One - Message from oracle %d failed re-verification, rejecting\n", 
                     msg.oracle_id);
            allValid = false;
            break;
        }
        
        // Additionally verify oracle is authorized in chainparams
        const CChainParams& params = Params();
        const OracleNodeInfo* oracle_config = params.GetOracleNode(msg.oracle_id);
        if (!oracle_config) {
            LogPrintf("Oracle: Phase One - Oracle %d not found in chainparams, rejecting\n", 
                     msg.oracle_id);
            allValid = false;
            break;
        }
        
        // Verify pubkey matches chainparams
        XOnlyPubKey expected_xonly(oracle_config->pubkey);
        if (HexStr(msg.oracle_pubkey) != HexStr(expected_xonly)) {
            LogPrintf("Oracle: Phase One - Oracle %d pubkey mismatch, rejecting\n", 
                     msg.oracle_id);
            allValid = false;
            break;
        }
    }
    
    if (!allValid) {
        LogPrintf("Oracle: Phase One - Validation failed, not adding oracle data to block\n");
        return true; // Don't fail block creation, just skip oracle data
    }
    
    bundle = COracleBundle(epoch);
    bundle.messages = pending;
    bundle.median_price_micro_usd = pending[0].price_micro_usd;
    bundle.timestamp = GetTime();
    
    LogPrintf("Oracle: Phase One - Using %zu verified message(s) for block %d\n",
             pending.size(), block_height);
    pending_messages.clear();
}
```

#### Step 2: Add Verification Timestamp Validation

```cpp
// Add to IsValidOracleMessage() or create new function
bool IsMessageFresh(const COraclePriceMessage& message, int64_t currentTime) {
    const int64_t MAX_MESSAGE_AGE = 300; // 5 minutes
    const int64_t MAX_FUTURE_TIME = 60;  // 1 minute future tolerance
    
    if (message.timestamp > currentTime + MAX_FUTURE_TIME) {
        LogPrintf("Oracle: Message timestamp %d is too far in future (current: %d)\n",
                 message.timestamp, currentTime);
        return false;
    }
    
    if (message.timestamp < currentTime - MAX_MESSAGE_AGE) {
        LogPrintf("Oracle: Message timestamp %d is too old (current: %d)\n",
                 message.timestamp, currentTime);
        return false;
    }
    
    return true;
}
```

#### Step 3: Add Atomic Bundle Creation

```cpp
// Ensure bundle creation and pending message clear are atomic
bool OracleBundleManager::CreateAndCommitBundle(CBlock& block, int32_t epoch, int32_t block_height) {
    std::lock_guard<std::recursive_mutex> msg_lock(mtx_messages);
    std::lock_guard<std::mutex> bundle_lock(mtx_bundles);
    
    // All operations within single critical section
    std::vector<COraclePriceMessage> verified_messages;
    int64_t current_time = GetTime();
    
    for (const auto& [oracle_id, msg] : pending_messages) {
        if (msg.VerifyPhase2() && IsMessageFresh(msg, current_time)) {
            verified_messages.push_back(msg);
        }
    }
    
    if (verified_messages.empty()) {
        return false;
    }
    
    COracleBundle bundle(epoch);
    bundle.messages = verified_messages;
    bundle.median_price_micro_usd = verified_messages[0].price_micro_usd;
    bundle.timestamp = current_time;
    
    // Add to block
    CScript oracle_script = CreateOracleScript(bundle);
    if (!oracle_script.empty()) {
        CMutableTransaction coinbase_tx(*block.vtx[0]);
        CTxOut oracle_output;
        oracle_output.nValue = 0;
        oracle_output.scriptPubKey = oracle_script;
        coinbase_tx.vout.push_back(oracle_output);
        block.vtx[0] = MakeTransactionRef(std::move(coinbase_tx));
    }
    
    // Only clear after successful commit
    pending_messages.clear();
    epoch_bundles[epoch] = bundle;
    
    return true;
}
```

#### Verification Steps
1. Unit test: Create test that modifies pending message after validation but before commit
2. Functional test: `test/functional/feature_oracle_validation.py`
3. Manual test: Submit oracle message, verify it appears in block with correct signature

---

## DGB-SEC-003: Division by Zero in DCA System Health Calculation

### Vulnerability Summary
- **Severity:** MEDIUM
- **CVSS Score:** 5.3
- **Location:** `src/consensus/dca.cpp:88-91`
- **Function:** `CalculateSystemHealth()`

### Root Cause Analysis
When `totalDD < 1000`, the expression `totalDD / 1000` evaluates to 0, causing division by zero in the scaled calculation path.

### Detailed Remediation Plan

#### Step 1: Add Zero-Division Protection

```cpp
// BEFORE (Vulnerable)
if (collateralValueCents > maxSafeDividend) {
    // Scale down both numerator and denominator to avoid overflow
    healthCalculation = (collateralValueCents / 1000) * 100 / (totalDD / 1000);
}

// AFTER (Safe)
if (collateralValueCents > maxSafeDividend) {
    // Handle case where totalDD is very small (< 1000)
    CAmount scaledDD = totalDD / 1000;
    if (scaledDD == 0) {
        // For very small DD supply, don't scale - use different approach
        // Cap collateral value to avoid overflow in direct calculation
        CAmount cappedCollateral = std::min(collateralValueCents, maxSafeDividend);
        healthCalculation = (cappedCollateral * 100) / totalDD;
    } else {
        healthCalculation = (collateralValueCents / 1000) * 100 / scaledDD;
    }
} else {
    healthCalculation = (collateralValueCents * 100) / totalDD;
}

// Ensure health is non-negative before returning
if (healthCalculation < 0) {
    LogPrintf("DCA: Warning - negative health calculation detected, clamping to 0\n");
    healthCalculation = 0;
}

int systemHealth = std::max(0, std::min(static_cast<int>(healthCalculation), 30000));
```

#### Step 2: Add Input Validation at Function Entry

```cpp
int DynamicCollateralAdjustment::CalculateSystemHealth(CAmount totalCollateral,
                                                       CAmount totalDD,
                                                       CAmount oraclePrice)
{
    // Validate inputs early
    if (oraclePrice <= 0) {
        LogPrintf("DCA: Cannot calculate system health - invalid oracle price: %lld\n", oraclePrice);
        return 0;
    }

    if (totalCollateral < 0) {
        LogPrintf("DCA: Cannot calculate system health - negative collateral: %lld\n", totalCollateral);
        return 0;
    }

    if (totalDD < 0) {
        LogPrintf("DCA: Cannot calculate system health - negative DD supply: %lld\n", totalDD);
        return 0;
    }

    // Special case: no DigiDollars issued yet
    if (totalDD == 0) {
        LogPrint(BCLog::DIGIDOLLAR, "DCA: No DigiDollars in circulation, returning maximum health\n");
        return 30000;
    }

    // Special case: very small DD supply (< minimum tracking threshold)
    const CAmount MIN_DD_FOR_HEALTH_CALC = 100; // $1.00 minimum
    if (totalDD < MIN_DD_FOR_HEALTH_CALC) {
        LogPrint(BCLog::DIGIDOLLAR, "DCA: DD supply below tracking threshold (%lld < %lld), returning max health\n",
                 totalDD, MIN_DD_FOR_HEALTH_CALC);
        return 30000;
    }
    
    // ... rest of calculation
}
```

#### Step 3: Add Comprehensive Unit Tests

```cpp
BOOST_AUTO_TEST_CASE(dca_system_health_edge_cases)
{
    using namespace DigiDollar::DCA;
    
    // Test zero DD supply
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::CalculateSystemHealth(1000000, 0, 5000), 30000);
    
    // Test small DD supply (< 1000)
    BOOST_CHECK_NO_THROW(DynamicCollateralAdjustment::CalculateSystemHealth(1000000, 500, 5000));
    
    // Test exactly 1000 DD
    BOOST_CHECK_NO_THROW(DynamicCollateralAdjustment::CalculateSystemHealth(1000000, 1000, 5000));
    
    // Test negative inputs
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::CalculateSystemHealth(-1, 1000, 5000), 0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::CalculateSystemHealth(1000, -1, 5000), 0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::CalculateSystemHealth(1000, 1000, -1), 0);
    
    // Test very large values (overflow boundary)
    CAmount largeCollateral = MAX_MONEY;
    CAmount largeDD = 100000000000LL; // $1 billion
    BOOST_CHECK_NO_THROW(DynamicCollateralAdjustment::CalculateSystemHealth(largeCollateral, largeDD, 100000));
}
```

---

## DGB-SEC-004: Oracle ID Truncation in Phase 2 Script Format

### Vulnerability Summary
- **Severity:** MEDIUM
- **CVSS Score:** 5.9
- **Location:** `src/oracle/bundle_manager.cpp:413-420`
- **Function:** `CreateOracleScript()`

### Root Cause Analysis
Oracle ID is stored as a single byte (`msg.oracle_id & 0xFF`), causing IDs 0, 256, 512, etc. to be indistinguishable on-chain.

### Detailed Remediation Plan

#### Step 1: Add Runtime Validation

```cpp
// Add before encoding oracle_id
for (const auto& msg : bundle.messages) {
    // Validate oracle_id fits in single byte for Phase 2 format
    if (msg.oracle_id > 255) {
        LogPrintf("Oracle: ERROR - oracle_id %d exceeds Phase 2 single-byte limit (max 255)\n", 
                 msg.oracle_id);
        return CScript(); // Return empty script on validation failure
    }
    
    p2_data.push_back(static_cast<unsigned char>(msg.oracle_id));
    // ... rest of encoding
}
```

#### Step 2: Define Version 3 Format for Future Expansion

```cpp
// Version byte definitions
static constexpr uint8_t ORACLE_SCRIPT_VERSION_1 = 0x01; // Phase 1: single oracle, compact
static constexpr uint8_t ORACLE_SCRIPT_VERSION_2 = 0x02; // Phase 2: multi-oracle, 1-byte ID
static constexpr uint8_t ORACLE_SCRIPT_VERSION_3 = 0x03; // Future: multi-oracle, 2-byte ID

CScript CreateOracleScriptV3(const COracleBundle& bundle) const
{
    // Format: OP_RETURN OP_ORACLE <version=0x03> <data>
    // Data layout:
    //   num_messages (1 byte)
    //   consensus_price (8 bytes, uint64 LE)
    //   timestamp (8 bytes, int64 LE)
    //   For each message:
    //     oracle_id (2 bytes, uint16 LE)  // Changed from 1 byte
    //     schnorr_sig (64 bytes)
    
    CScript script;
    script << OP_RETURN << OP_ORACLE;
    script << std::vector<unsigned char>{ORACLE_SCRIPT_VERSION_3};
    
    std::vector<unsigned char> data;
    // ... encode with 2-byte oracle_id
    
    return script;
}
```

#### Step 3: Add Consensus Constant for Maximum Oracle ID

```cpp
// In src/primitives/oracle.h
static constexpr uint32_t ORACLE_MAX_ID_PHASE2 = 255;     // Single-byte limit
static constexpr uint32_t ORACLE_MAX_ID_PHASE3 = 65535;   // Two-byte limit

// Validation function
bool IsValidOracleId(uint32_t oracle_id, int script_version) {
    switch (script_version) {
        case 1:
        case 2:
            return oracle_id <= ORACLE_MAX_ID_PHASE2;
        case 3:
            return oracle_id <= ORACLE_MAX_ID_PHASE3;
        default:
            return false;
    }
}
```

---

## DGB-SEC-005: Mock Oracle Price Randomness Without Production Guards

### Vulnerability Summary
- **Severity:** LOW
- **CVSS Score:** 3.7
- **Location:** `src/oracle/node.cpp:470-475`
- **Function:** `ExchangePriceFetcher::FetchAllPrices()`

### Detailed Remediation Plan

#### Step 1: Add Compile-Time Guard

```cpp
std::vector<ExchangePriceFetcher::ExchangePrice> ExchangePriceFetcher::FetchAllPrices()
{
#ifdef ENABLE_MOCK_ORACLE
    std::vector<ExchangePrice> prices;
    
    // Mock exchange prices with slight variation
    int64_t timestamp = GetTime();
    prices.emplace_back("Binance", 5000 + GetRand(200) - 100, timestamp);
    prices.emplace_back("Coinbase", 4950 + GetRand(200) - 100, timestamp);
    prices.emplace_back("Kraken", 5050 + GetRand(200) - 100, timestamp);
    prices.emplace_back("Bittrex", 5025 + GetRand(200) - 100, timestamp);
    prices.emplace_back("Poloniex", 4975 + GetRand(200) - 100, timestamp);
    
    LogPrintf("Oracle: WARNING - Using MOCK prices (ENABLE_MOCK_ORACLE defined)\n");
    return FilterValidPrices(prices);
#else
    // Production implementation uses real exchange APIs
    return FetchRealExchangePrices();
#endif
}
```

#### Step 2: Add Runtime Network Check

```cpp
std::vector<ExchangePriceFetcher::ExchangePrice> ExchangePriceFetcher::FetchAllPrices()
{
    // Only allow mock prices on regtest
    if (Params().GetChainType() != ChainType::REGTEST) {
#ifdef ENABLE_MOCK_ORACLE
        LogPrintf("Oracle: CRITICAL - Mock oracle code running on non-regtest network!\n");
        assert(false); // Crash rather than use mock prices in production
#endif
        return FetchRealExchangePrices();
    }
    
    // Regtest mock implementation
    // ...
}
```

#### Step 3: Add CI Check

Add to `.github/workflows/build.yml`:

```yaml
- name: Check for mock oracle in release build
  run: |
    if grep -r "ENABLE_MOCK_ORACLE" src/oracle/*.cpp; then
      if [ "${{ github.ref }}" == "refs/heads/master" ] || [ "${{ github.ref }}" == "refs/heads/release/*" ]; then
        echo "ERROR: Mock oracle code found in release branch"
        exit 1
      fi
    fi
```

---

## DGB-SEC-006: Deterministic IV Derivation in Wallet Encryption

### Vulnerability Summary
- **Severity:** LOW
- **CVSS Score:** 3.1
- **Location:** `src/wallet/crypter.cpp:108-126`
- **Function:** `EncryptSecret()` / `DecryptSecret()`

### Detailed Remediation Plan

This is an inherited design from Bitcoin Core. Full remediation would require a wallet format change, which is a significant undertaking. We recommend a phased approach:

#### Phase 1: Documentation (Immediate)

Add security documentation explaining the design decision:

```cpp
/**
 * EncryptSecret - Encrypt a secret using the master key
 * 
 * SECURITY NOTE: The IV is derived deterministically from the public key hash.
 * This is intentional and inherited from Bitcoin Core design. It allows:
 * 1. Deterministic encryption - same key always produces same ciphertext
 * 2. No need to store IV separately
 * 
 * Known limitations:
 * - Same key encrypted twice produces identical ciphertext (enables comparison)
 * - CBC mode with predictable IV has theoretical chosen-plaintext weaknesses
 * 
 * These are acceptable trade-offs because:
 * - Private keys are high-entropy random data
 * - Wallet file access is required for any attack
 * - Changing would break backward compatibility
 * 
 * Future versions may migrate to randomized IVs stored alongside ciphertext.
 */
bool EncryptSecret(...) { ... }
```

#### Phase 2: New Wallet Format (Long-term)

Design a new encryption scheme for wallet format v2:

```cpp
// Proposed new format with random IV
struct EncryptedKeyV2 {
    uint8_t version;           // = 2
    uint8_t iv[16];            // Random IV
    std::vector<uint8_t> ciphertext;
    uint8_t mac[32];           // HMAC for authenticated encryption
};

bool EncryptSecretV2(const CKeyingMaterial& vMasterKey, 
                     const CKeyingMaterial& vchPlaintext,
                     EncryptedKeyV2& out)
{
    // Generate random IV
    GetStrongRandBytes(out.iv, 16);
    
    // Encrypt with random IV
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(out.iv, out.iv + 16);
    if (!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    
    if (!cKeyCrypter.Encrypt(vchPlaintext, out.ciphertext))
        return false;
    
    // Add HMAC for authenticated encryption
    CHMAC_SHA256 hmac(vMasterKey.data(), vMasterKey.size());
    hmac.Write(out.iv, 16);
    hmac.Write(out.ciphertext.data(), out.ciphertext.size());
    hmac.Finalize(out.mac);
    
    out.version = 2;
    return true;
}
```

---

## DGB-SEC-007: Race Condition in Oracle Bundle Manager

### Vulnerability Summary
- **Severity:** MEDIUM
- **CVSS Score:** 4.8
- **Location:** `src/oracle/bundle_manager.cpp:107-130`
- **Function:** `AddOracleMessage()`

### Root Cause Analysis
Nested locking with different mutexes (`mtx_messages` and `mtx_bundles`) without consistent ordering, plus reads of shared state without locks.

### Detailed Remediation Plan

#### Step 1: Establish Lock Hierarchy

```cpp
// Document lock ordering in header file
/**
 * OracleBundleManager Lock Hierarchy
 * 
 * Always acquire locks in this order to prevent deadlock:
 * 1. mtx_messages (protects pending_messages, seen_message_hashes)
 * 2. mtx_bundles (protects epoch_bundles, cached_price, cached_epoch, last_update_time)
 * 
 * NEVER acquire mtx_messages while holding mtx_bundles.
 */
class OracleBundleManager {
private:
    mutable std::recursive_mutex mtx_messages;  // Lock order: 1
    mutable std::mutex mtx_bundles;              // Lock order: 2
    
    // ... rest of class
};
```

#### Step 2: Refactor Locking in AddOracleMessage

```cpp
bool OracleBundleManager::AddOracleMessage(const COraclePriceMessage& message)
{
    if (!enabled) {
        return false;
    }

    if (!IsValidOracleMessage(message)) {
        LogPrintf("Oracle: Invalid oracle message from oracle %d\n", message.oracle_id);
        return false;
    }

    // Phase 1: Acquire mtx_messages, update pending messages
    bool shouldUpdatePrice = false;
    uint64_t medianPrice = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mtx_messages);
        
        uint256 msg_hash = message.GetSignatureHash();
        if (seen_message_hashes.count(msg_hash) > 0) {
            return false; // Duplicate
        }
        seen_message_hashes.insert(msg_hash);
        
        auto it = pending_messages.find(message.oracle_id);
        if (it != pending_messages.end()) {
            if (message.timestamp > it->second.timestamp) {
                it->second = message;
            } else {
                return false; // Older message
            }
        } else {
            pending_messages[message.oracle_id] = message;
        }
        
        // Calculate consensus while holding mtx_messages
        if (static_cast<int>(pending_messages.size()) >= min_oracle_count) {
            std::vector<uint64_t> prices;
            prices.reserve(pending_messages.size());
            for (const auto& pair : pending_messages) {
                prices.push_back(pair.second.price_micro_usd);
            }
            std::sort(prices.begin(), prices.end());
            medianPrice = prices[prices.size() / 2];
            shouldUpdatePrice = true;
        }
    } // Release mtx_messages before acquiring mtx_bundles
    
    // Phase 2: Acquire mtx_bundles, update cached price
    if (shouldUpdatePrice) {
        std::lock_guard<std::mutex> lock(mtx_bundles);
        cached_price = static_cast<CAmount>(medianPrice);
        last_update_time = GetTime();
    }

    return true;
}
```

#### Step 3: Add Thread Safety Annotations

```cpp
// Use Clang thread safety annotations (also works with GCC)
#include <threadsafety.h>

class OracleBundleManager {
private:
    mutable std::recursive_mutex mtx_messages ACQUIRED_BEFORE(mtx_bundles);
    mutable std::mutex mtx_bundles;
    
    std::map<uint32_t, COraclePriceMessage> pending_messages GUARDED_BY(mtx_messages);
    std::set<uint256> seen_message_hashes GUARDED_BY(mtx_messages);
    
    std::map<int32_t, COracleBundle> epoch_bundles GUARDED_BY(mtx_bundles);
    CAmount cached_price GUARDED_BY(mtx_bundles);
    int32_t cached_epoch GUARDED_BY(mtx_bundles);
    int64_t last_update_time GUARDED_BY(mtx_bundles);
    
    // ... methods
};
```

#### Step 4: Add Deadlock Detection in Debug Builds

```cpp
#ifdef DEBUG_LOCKORDER
// Enable lock order checking
static std::map<std::thread::id, std::vector<void*>> g_lock_stacks;
static std::mutex g_lock_debug_mutex;

template<typename Mutex>
class DebugLockGuard {
    Mutex& m;
    void* id;
public:
    DebugLockGuard(Mutex& mutex, void* lock_id) : m(mutex), id(lock_id) {
        std::lock_guard<std::mutex> debug_lock(g_lock_debug_mutex);
        auto& stack = g_lock_stacks[std::this_thread::get_id()];
        // Check lock ordering
        for (void* held : stack) {
            if (held > id) {
                LogPrintf("LOCK ORDER VIOLATION: acquiring %p while holding %p\n", id, held);
                assert(false);
            }
        }
        stack.push_back(id);
        m.lock();
    }
    ~DebugLockGuard() {
        m.unlock();
        std::lock_guard<std::mutex> debug_lock(g_lock_debug_mutex);
        auto& stack = g_lock_stacks[std::this_thread::get_id()];
        stack.pop_back();
    }
};
#endif
```

---

## Implementation Timeline

### Week 1 (Days 1-7): Critical Fixes
- [ ] DGB-SEC-001: Integer overflow fix + unit tests
- [ ] DGB-SEC-002: Oracle signature re-verification

### Week 2-3 (Days 8-21): High Priority
- [ ] DGB-SEC-003: Division by zero protection
- [ ] DGB-SEC-004: Oracle ID validation
- [ ] DGB-SEC-007: Lock hierarchy refactoring

### Week 4-8 (Days 22-56): Medium Priority
- [ ] DGB-SEC-005: Mock oracle guards
- [ ] DGB-SEC-006: Documentation + design for wallet v2

### Ongoing
- [ ] Add fuzz testing for all fixed components
- [ ] Security regression tests in CI
- [ ] Periodic security audit reviews

---

## Testing Requirements

### Unit Tests
- All fixes must have corresponding unit tests
- Tests must cover normal operation, edge cases, and failure modes
- Minimum 80% code coverage for new/modified code

### Functional Tests
- Integration tests for oracle bundle flow
- DigiDollar transaction lifecycle tests
- Stress tests for concurrent oracle message processing

### Manual Testing
- Regtest deployment with test scenarios
- Testnet validation before mainnet deployment

---

## Approval and Sign-off

| Role | Name | Date | Signature |
|------|------|------|-----------|
| Security Lead | | | |
| Lead Developer | | | |
| QA Lead | | | |
| Release Manager | | | |

---

## Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-03 | Security Audit Team | Initial remediation plan |
