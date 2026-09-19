# DigiByte Blockchain Architecture
**DigiByte Core v9.26.5 (Based on Bitcoin Core v26.2)**
*Comprehensive Technical Documentation*
*Last Updated: 2026-07-28*
*Validation Status: Spot-validated against live `feature/digidollar-v1` code surfaces listed in Appendix D*

---

## Paymaster architecture in this branch

The v9.26.6rc2 integration keeps the provider panel in
`src/qt/paymasterwidget.cpp`, behind a small embedding interface. The concrete
`PaymasterSendWidget` owns fee selection and client-session presentation while
the original send form retains its editable inputs and ordinary transfer path.
Paymaster record codecs remain `WalletBatch` methods in `wallet/paymasterdb.cpp`;
`PaymasterStore` still owns atomic state transitions. Free functions in
`wallet/rpc/paymaster_send.cpp` isolate the existing send RPC integration without
adding RPC endpoints, persisted state, or another service layer. See the
[integration notes](doc/digidollar-paymaster-v9.26.6rc2-integration.md) for scope
and pending verification.

For the Paymaster implementation at `bd270044c1`, see the
[developer starting point](PAYMASTER.md) and
[implementation reference](doc/digidollar-paymaster-implementation.md).
It adds collaborative DD transfer construction, wallet authorization and
persistence, provider discovery, and direct P2P sessions. The resulting
transaction is an ordinary `DD_TX_TRANSFER`. The broader branch also contains
arithmetic portability changes in consensus source, which require separate
equivalence review. Verification is tracked in the
[Paymaster release gate](doc/digidollar-paymaster-release-gate.md).

## Executive Summary

DigiByte is a decentralized, open-source blockchain focused on speed, security, and decentralization. This document provides a complete architectural overview of the DigiByte Core implementation, validated against the actual codebase.

### Key Differentiators from Bitcoin

| Feature | Bitcoin | DigiByte |
|---------|---------|----------|
| Block Time | 10 minutes | **15 seconds** |
| Mining Algorithms | SHA256D only | **5 algorithms** (multi-algo) |
| Difficulty Adjustment | 2016 blocks | **Real-time per block** |
| Max Supply | 21 million | **21 billion** |
| Coinbase Maturity | 100 blocks | **8 blocks** (early), 100 (later) |
| Privacy | Standard relay | **Dandelion++** |
| Stablecoin | None | **DigiDollar (native)** |

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Directory Structure](#2-directory-structure)
3. [Consensus Layer](#3-consensus-layer)
4. [Multi-Algorithm Mining](#4-multi-algorithm-mining)
5. [Difficulty Adjustment](#5-difficulty-adjustment)
6. [Block & Chain Management](#6-block--chain-management)
7. [Transaction Processing](#7-transaction-processing)
8. [Script & Signature Verification](#8-script--signature-verification)
9. [P2P Networking](#9-p2p-networking)
10. [Wallet System](#10-wallet-system)
11. [RPC Interface](#11-rpc-interface)
12. [DigiDollar Stablecoin](#12-digidollar-stablecoin)
13. [Oracle System](#13-oracle-system)

---

## 1. System Overview

### 1.1 Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          DigiByte Core Node                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │   RPC API    │  │   REST API   │  │  ZMQ Pub     │  │    GUI       │     │
│  │  (JSON-RPC)  │  │   (HTTP)     │  │  (Events)    │  │   (Qt)       │     │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘     │
│         │                 │                 │                 │             │
│  ┌──────┴─────────────────┴─────────────────┴─────────────────┴──────┐      │
│  │                     Node Interface Layer                          │      │
│  └────────────────────────────────┬──────────────────────────────────┘      │
│                                   │                                         │
│  ┌────────────────────────────────┴──────────────────────────────────┐      │
│  │                      ChainstateManager                            │      │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐    │      │
│  │  │   Chainstate    │  │   BlockManager  │  │    Mempool      │    │      │
│  │  │   (UTXO Set)    │  │  (Block Index)  │  │ (+ Stempool)    │    │      │
│  │  └─────────────────┘  └─────────────────┘  └─────────────────┘    │      │
│  └────────────────────────────────┬──────────────────────────────────┘      │
│                                   │                                         │
│  ┌────────────────────────────────┴──────────────────────────────────┐      │
│  │                      Consensus Layer                              │      │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────┐  │      │
│  │  │ Validation  │  │  PoW Check  │  │  Difficulty │  │ DigiDollar│ │      │
│  │  │   Rules     │  │  (5 Algos)  │  │  Adjustment │  │ Consensus │ │      │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └──────────┘  │      │
│  └────────────────────────────────┬──────────────────────────────────┘      │
│                                   │                                         │
│  ┌────────────────────────────────┴──────────────────────────────────┐      │
│  │                       P2P Network Layer                           │      │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────┐  │      │
│  │  │   CConnman  │  │ V1/V2       │  │ Dandelion++ │  │  Oracle  │  │      │
│  │  │   (Peers)   │  │ Transport   │  │  (Privacy)  │  │  P2P     │  │      │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └──────────┘  │      │
│  └───────────────────────────────────────────────────────────────────┘      │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────┐      │
│  │                        Storage Layer                              │      │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────┐  │      │
│  │  │  LevelDB    │  │  Block      │  │   Wallet    │  │  Oracle  │  │      │
│  │  │  (UTXO/Idx) │  │  Files      │  │   DB        │  │  Cache   │  │      │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └──────────┘  │      │
│  └───────────────────────────────────────────────────────────────────┘      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 Core Components

| Component | Purpose | Key Files |
|-----------|---------|-----------|
| **Consensus** | Block/tx validation rules | `src/validation.cpp`, `src/consensus/` |
| **Mining** | Multi-algorithm PoW | `src/pow.cpp`, `src/crypto/` |
| **Chain** | Block index & UTXO | `src/chain.cpp`, `src/coins.cpp` |
| **Network** | P2P communication | `src/net.cpp`, `src/net_processing.cpp` |
| **Wallet** | Key & transaction mgmt | `src/wallet/` |
| **RPC** | External API | `src/rpc/` |
| **DigiDollar** | Stablecoin system | `src/digidollar/`, `src/oracle/` |

---

## 2. Directory Structure

```
digibyte/
├── src/
│   ├── consensus/           # Consensus rules
│   │   ├── params.h         # Chain parameters (difficulty, timing)
│   │   ├── validation.h     # Validation state enums
│   │   ├── tx_check.cpp     # Transaction validation
│   │   ├── dca.cpp          # Dynamic Collateral Adjustment
│   │   ├── err.cpp          # Emergency Redemption Ratio
│   │   └── volatility.cpp   # Volatility protection
│   │
│   ├── primitives/          # Core data structures
│   │   ├── block.h          # CBlock, CBlockHeader (multi-algo)
│   │   ├── transaction.h    # CTransaction, CTxIn, CTxOut
│   │   └── oracle.h         # Oracle message structures
│   │
│   ├── crypto/              # Cryptographic algorithms
│   │   ├── sha256.cpp       # SHA256D (algo 0)
│   │   ├── scrypt.cpp       # Scrypt (algo 1)
│   │   ├── hashgroestl.h    # Groestl (algo 2)
│   │   ├── hashskein.h      # Skein (algo 3)
│   │   ├── hashqubit.h      # Qubit (algo 4)
│   │   ├── odocrypt.cpp     # Odocrypt (algo 7)
│   │   └── hashodo.h        # Odo hash wrapper
│   │
│   ├── script/              # Script system
│   │   ├── interpreter.cpp  # Script execution (2308 lines)
│   │   ├── script.h         # Opcodes, CScript class
│   │   └── sigcache.cpp     # Signature caching
│   │
│   ├── wallet/              # Wallet implementation
│   │   ├── wallet.h         # CWallet class
│   │   ├── scriptpubkeyman.h # Key management
│   │   ├── spend.cpp        # Transaction creation
│   │   └── digidollarwallet.cpp # DD wallet integration
│   │
│   ├── net.cpp              # P2P networking (~3900 lines, V1+V2 BIP324 transport)
│   ├── net_processing.cpp   # Message handling (~7500 lines, includes oracle msg handlers)
│   ├── dandelion.cpp        # Dandelion++ privacy (~500 lines)
│   ├── validation.cpp       # Block validation (~7050 lines; DD-aware ConnectBlock)
│   ├── pow.cpp              # Difficulty adjustment
│   │
│   ├── digidollar/          # DigiDollar stablecoin
│   │   ├── digidollar.h     # Core DD structures
│   │   ├── txbuilder.cpp    # Mint/Transfer/Redeem builders
│   │   ├── validation.cpp   # DD consensus rules
│   │   ├── health.cpp       # System health monitoring
│   │   └── scripts.cpp      # DD script handling
│   │
│   ├── oracle/              # Oracle system
│   │   ├── bundle_manager.cpp # Oracle bundle management
│   │   ├── exchange.cpp     # Exchange API integration
│   │   ├── node.cpp         # Oracle node operations
│   │   └── mock_oracle.cpp  # Testing mock oracle
│   │
│   ├── paymaster/           # Non-consensus DD fee-funding protocol
│   │   ├── txbuilder.cpp    # Deterministic collaborative transfer builder
│   │   ├── protocol.cpp     # Intent, quote, submit, and result validation
│   │   ├── manager.cpp      # Discovery and direct-message coordination
│   │   └── provider.cpp     # Provider policy, fees, and pool readiness
│   │
│   ├── rpc/                 # RPC commands
│   │   ├── blockchain.cpp   # Chain queries
│   │   ├── mining.cpp       # Mining commands
│   │   ├── net.cpp          # Network commands
│   │   ├── rawtransaction.cpp # Raw tx handling
│   │   └── digidollar.cpp   # DD RPC commands
│   │
│   ├── kernel/              # Chain parameters
│   │   └── chainparams.cpp  # Network configurations
│   │
│   └── qt/                  # GUI components
│       ├── digidollartab.cpp # DD main tab
│       └── digidollar*.cpp  # DD widgets
│
├── src/test/                # C++ unit tests and fuzz targets
├── src/wallet/test/         # Wallet C++ tests (linked into test_digibyte)
├── src/qt/test/             # Qt C++ tests
├── test/
│   └── functional/          # Python integration tests
│
└── doc/                     # Documentation
```

---

## 3. Consensus Layer

### 3.1 Block Validation Flow

```
AcceptBlockHeader()
    ↓
CheckBlockHeader() [PoW verification - algorithm-specific]
    ↓
ContextualCheckBlockHeader() [Difficulty target validation]
    ↓
AcceptBlock() / ProcessNewBlock()
    ↓
CheckBlock() [Context-independent: merkle root, size, transactions]
    ↓
ContextualCheckBlock() [Context-dependent: finality, witness, oracle]
    ↓
ActivateBestChain() → ActivateBestChainStep()
    ↓
ConnectTip() → ConnectBlock() [Full UTXO validation, script execution]
    ↓
UpdateTip() [Chain state update, notifications]
```

**Key Files:**
- `src/validation.cpp` - Main validation logic
- `src/consensus/tx_check.cpp` - Transaction validation
- `src/pow.cpp` - PoW and difficulty

### 3.2 Consensus Parameters

**File:** `src/consensus/params.h` (struct Params is roughly lines 83-238)

```cpp
struct Params {
    // Block timing
    int64_t nPowTargetSpacing = 15;      // 15-second blocks

    // Multi-algorithm
    int64_t nAveragingInterval = 10;      // 10 blocks per algo
    int64_t multiAlgoTargetSpacing = 150; // 30s × 5 algos (V3 MultiShield)
    int64_t multiAlgoTargetSpacingV4 = 75; // 15s × 5 algos (V4 DigiSpeed)

    // Hard fork heights
    int64_t multiAlgoDiffChangeTarget = 145000;    // MultiAlgo V2
    int64_t alwaysUpdateDiffChangeTarget = 400000; // MultiShield V3
    int64_t workComputationChangeTarget = 1430000; // DigiSpeed V4
    int OdoHeight = 9112320;                       // Odocrypt activation (BuriedDeployment)

    // DigiDollar / Oracle
    int nDDActivationHeight{0};                                            // Static DD floor gate (historical BIP9 floor)
    int nOracleActivationHeight{std::numeric_limits<int>::max()};          // Live-feed activation height
    int nOracleEpochLength{1440};                                          // Blocks per oracle epoch (default: 1440 = 24 hours)
    int nOracleRequiredMessages{1};                                        // Off-chain quorum threshold
    int nOracleTotalOracles{1};                                            // Total reserved oracle slots
    std::vector<std::string> vOraclePublicKeys;                            // Hardcoded XOnlyPubKeys (hex)
    int nDigiDollarMuSig2Height{std::numeric_limits<int>::max()};          // MuSig2 v0x03 bundle activation
    int nOraclePubkeyCount{0};                                             // MuSig2 pubkey count
    int nOracleConsensusRequired{0};                                       // MuSig2 quorum threshold
};
```

(v9.26.5 burial) `Consensus::DEPLOYMENT_DIGIDOLLAR` is now a **buried deployment** (BIP90; historically BIP9 bit 23) that gates `SCRIPT_VERIFY_DIGIDOLLAR` at `Consensus::Params::DigiDollarHeight` (mainnet 23869440, testnet26 600, signet/regtest 0 — the verified BIP9 `since` heights; `TaprootHeight`/`AlgoLockHeight` are buried alongside). The static gates `nDDActivationHeight`, `nOracleActivationHeight`, and `nDigiDollarMuSig2Height` remain aligned at the historical floor in `src/kernel/chainparams.cpp` (mainnet 23627520, testnet26 600). Testnet26 uses default P2P port 12033, data directory `testnet26`, and reset genesis timestamp 1780156800. Default regtest buries DigiDollar at height 0 with DD/oracle height gates at 650; `nDigiDollarMuSig2Height = min(nDDActivationHeight, DigiDollarHeight) = 0` so v0x03 quotes are valid whenever DigiDollar is active. The direct `-digidollaractivationheight=N` knob retargets the buried height and the DD/oracle/MuSig2 gates together (DD activates at exactly N); `-testactivationheight=digidollar@H` moves only the buried height, and `-vbparams=digidollar:...` is a startup error. Startup oracle-price cache reconstruction follows the same buried predicate used by block connection so regtest DD-active oracle bundles below 650 are not skipped on restart/reindex. MuSig2 v0x03 oracle bundles are required as soon as DigiDollar is active, not before the DD/oracle activation height.

### 3.3 Block Validation Results

**File:** `src/consensus/validation.h` (lines 64-84)

```cpp
enum class BlockValidationResult {
    BLOCK_RESULT_UNSET = 0,
    BLOCK_CONSENSUS,              // Consensus rule violation
    BLOCK_RECENT_CONSENSUS_CHANGE, // Consensus change (soft-fork grace period)
    BLOCK_CACHED_INVALID,         // Previously invalid
    BLOCK_INVALID_HEADER,         // PoW/timestamp invalid
    BLOCK_MUTATED,                // Data corruption
    BLOCK_MISSING_PREV,           // Missing parent
    BLOCK_INVALID_PREV,           // Invalid parent
    BLOCK_TIME_FUTURE,            // Too far in future
    BLOCK_CHECKPOINT,             // Checkpoint violation
    BLOCK_INVALID_ALGO,           // Invalid algorithm (DigiByte-specific)
    BLOCK_HEADER_LOW_WORK         // Insufficient work
};
```

### 3.4 Key Constants

**File:** `src/consensus/consensus.h`

| Constant | Value | Purpose |
|----------|-------|---------|
| `COINBASE_MATURITY` | 8 | Blocks before coinbase spendable |
| `COINBASE_MATURITY_2` | 100 | Later period maturity |
| `MAX_BLOCK_WEIGHT` | 4,000,000 | Max block weight (BIP141) |
| `MAX_BLOCK_SIGOPS_COST` | 80,000 | Max signature operations |
| `WITNESS_SCALE_FACTOR` | 4 | Witness discount factor |

---

## 4. Multi-Algorithm Mining

### 4.1 Supported Algorithms

**File:** `src/primitives/block.h` (lines 16-44)

```cpp
enum {
    ALGO_SHA256D  = 0,    // Bitcoin-compatible
    ALGO_SCRYPT   = 1,    // Memory-hard (Litecoin-style)
    ALGO_GROESTL  = 2,    // Groestl-512 → SHA256
    ALGO_SKEIN    = 3,    // Skein-512 → SHA256
    ALGO_QUBIT    = 4,    // 5-hash chain
    ALGO_ODO      = 7,    // Odocrypt (replaces Groestl post-9.1M)
    NUM_ALGOS     = 5     // Active algorithms at any time
};

// Version encoding (bits 8-11)
BLOCK_VERSION_SHA256D = (2 << 8)   // 0x0200
BLOCK_VERSION_SCRYPT  = (0 << 8)   // 0x0000
BLOCK_VERSION_GROESTL = (4 << 8)   // 0x0400
BLOCK_VERSION_SKEIN   = (6 << 8)   // 0x0600
BLOCK_VERSION_QUBIT   = (8 << 8)   // 0x0800
BLOCK_VERSION_ODO     = (14 << 8)  // 0x0E00
```

### 4.2 Algorithm Timeline

| Era | Block Range | Active Algorithms |
|-----|-------------|-------------------|
| 1 | 0 - 144,999 | Scrypt only |
| 2 | 145,000 - 9,099,999 | SHA256D, Scrypt, Groestl, Skein, Qubit |
| 3 | 9,100,000+ | SHA256D, Scrypt, Skein, Qubit, **Odocrypt** |

### 4.3 PoW Hash Computation

**File:** `src/primitives/block.cpp` (lines 56-106)

```cpp
uint256 CBlockHeader::GetPoWAlgoHash(const Consensus::Params& params) const
{
    switch (GetAlgo()) {
        case ALGO_SHA256D: return GetHash();           // Double SHA256
        case ALGO_SCRYPT:  return scrypt_1024_1_1_256(...);
        case ALGO_GROESTL: return HashGroestl(...);    // Groestl-512 → SHA256
        case ALGO_SKEIN:   return HashSkein(...);      // Skein-512 → SHA256
        case ALGO_QUBIT:   return HashQubit(...);      // Luffa→Cubehash→Shavite→SIMD→Echo
        case ALGO_ODO:     return HashOdo(..., OdoKey(params, nTime));
    }
}
```

### 4.4 Odocrypt Key Generation

**File:** `src/crypto/odocrypt.cpp`

Odocrypt uses a time-based key that changes every 10 days:

```cpp
uint32_t OdoKey(const Consensus::Params& params, uint32_t nTime) {
    uint32_t nShapechangeInterval = 864000;  // 10 days in seconds
    return nTime - nTime % nShapechangeInterval;
}
```

The cipher uses:
- **84 rounds** of encryption
- **S-boxes**: 6-bit (10 boxes) + 10-bit (10 boxes)
- **P-boxes**: 2 permutation boxes with 6 subrounds
- **Keccak-p[800]** finalization

### 4.5 Finding the last block mined with a given algorithm

**File:** `src/pow.cpp`, `GetLastBlockIndexForAlgo()`

The difficulty rules need the previous block that used the same mining algorithm. There is one way to find it:
walk back through the chain from the current block until a block with that algorithm turns up.

Every block used to carry an array of pointers, one per algorithm, so the answer could be read without walking.
That array was removed. It cost 64 bytes for every block header the node holds in memory, which is about 1.5 GB
on a fully synced mainnet node, and the walk it saved is short. The difficulty rule that uses this already steps
back fifty blocks on every call before it does anything else, so the walk adds roughly a tenth more work to a
path that was already doing a lot of it.

Removing it changed no difficulty result. The two lookups were run side by side over every algorithm, every
difficulty rule and the special case for minimum-difficulty blocks, and their answers were recorded as plain
numbers that the tests still check.

---

## 5. Difficulty Adjustment

### 5.1 Evolution of Algorithms

| Version | Block Range | Name | Key Feature |
|---------|-------------|------|-------------|
| V1 | 0 - 144,999 | DigiShield | 144-block interval |
| V2 | 145,000 - 399,999 | MultiAlgo | Per-algorithm targeting |
| V3 | 400,000 - 1,429,999 | MultiShield | Median time + global adjustment |
| V4 | 1,430,000+ | DigiSpeed | Optimized 15s targeting |

### 5.2 Difficulty Selection Logic

**File:** `src/pow.cpp` (lines 256-285)

```cpp
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast,
                                 const CBlockHeader *pblock,
                                 const Consensus::Params& params,
                                 int algo)
{
    if (pindexLast->nHeight < params.multiAlgoDiffChangeTarget)
        return GetNextWorkRequiredV1(pindexLast, params, algo);
    else if (pindexLast->nHeight < params.alwaysUpdateDiffChangeTarget)
        return GetNextWorkRequiredV2(pindexLast, params, algo);
    else if (pindexLast->nHeight < params.workComputationChangeTarget)
        return GetNextWorkRequiredV3(pindexLast, params, algo);
    else
        return GetNextWorkRequiredV4(pindexLast, params, algo);
}
```

### 5.3 V4 DigiSpeed Algorithm

**File:** `src/pow.cpp` (lines 192-254)

```cpp
// Parameters
nAveragingTargetTimespanV4 = 10 * 75;  // 750 seconds
nMinActualTimespanV4 = 690;             // 92% of target
nMaxActualTimespanV4 = 870;             // 116% of target
nLocalTargetAdjustment = 4;             // 4% per-algo adjustment

// Algorithm
1. Look back 50 blocks (10 per algorithm × 5 algorithms)
2. Use GetMedianTimePast() to prevent time-warp attacks
3. Calculate actual timespan with damping: target + (actual - target) / 4
4. Apply limits (690-870 seconds)
5. Global retarget: bnNew *= nActualTimespan / 750
6. Per-algo adjustment: ±4% based on algorithm's block share
```

### 5.4 Difficulty Adjustment Parameters

**File:** `src/kernel/chainparams.cpp` (lines 142-163)

| Parameter | V1-V2 | V3 | V4 |
|-----------|-------|-------|-------|
| Target Spacing | 150s (30s×5) | 150s | 75s (15s×5) |
| Averaging Window | 10 blocks | 50 blocks | 50 blocks |
| Max Adjust Down | 40% | 16% | 16% |
| Max Adjust Up | 20% | 8% | 8% |
| Local Adjustment | N/A | 4% | 4% |

---

## 6. Block & Chain Management

### 6.1 Block Structure

**File:** `src/primitives/block.h`

```cpp
class CBlockHeader {
    int32_t nVersion;         // Algorithm encoded in bits 8-11
    uint256 hashPrevBlock;    // Previous block hash
    uint256 hashMerkleRoot;   // Merkle root of transactions
    uint32_t nTime;           // Block timestamp
    uint32_t nBits;           // Difficulty target (compact)
    uint32_t nNonce;          // PoW counter
};

class CBlock : public CBlockHeader {
    std::vector<CTransactionRef> vtx;  // Block transactions
    mutable bool fChecked;              // Validation cache
};
```

### 6.2 Block Index Structure

**File:** `src/chain.h` (lines 146-380)

```cpp
class CBlockIndex {
    // Identity
    const uint256* phashBlock;    // Block hash pointer
    CBlockIndex* pprev;           // Previous block
    CBlockIndex* pskip;           // Skip pointer for fast traversal

    // Chain position
    int nHeight;                  // Block height
    int nFile;                    // Block file index
    unsigned int nDataPos;        // Position in block file
    unsigned int nUndoPos;        // Position in undo file

    // Validation
    uint32_t nStatus;             // BlockStatus flags
    arith_uint256 nChainWork;     // Cumulative work

    // Transactions
    unsigned int nTx;             // Transactions in block
    unsigned int nChainTx;        // Cumulative transactions

};
```

### 6.3 Chain State Management

**File:** `src/validation.h` (`Chainstate` at 497, `ChainstateManager` at 884-1308)

```
ChainstateManager
├── m_ibd_chainstate        (Normal IBD chainstate)
├── m_snapshot_chainstate   (Optional assumed-valid snapshot)
├── m_active_chainstate     (Points to active one)
└── m_blockman              (Shared BlockManager)
    └── m_block_index       (All blocks ever seen)

Chainstate
├── m_chain                 (CChain - active chain by height)
├── m_coins_views           (UTXO cache hierarchy)
│   ├── m_dbview            (LevelDB on disk)
│   ├── m_catcherview       (Error handler)
│   └── m_cacheview         (Memory cache)
├── m_mempool               (Transaction memory pool)
└── m_stempool              (Dandelion++ stem pool)
```

### 6.4 UTXO Management

**File:** `src/coins.h`

```cpp
struct Coin {
    CTxOut out;              // The unspent output
    unsigned int fCoinBase : 1;
    uint32_t nHeight : 31;   // Inclusion height
};

// Three-level cache hierarchy
CCoinsViewDB      → LevelDB on disk
CCoinsViewBacked  → Composable wrapper
CCoinsViewCache   → In-memory cache with DIRTY/FRESH flags
```

### 6.5 Block Storage

**Files:** `blocks/blk?????.dat` (128 MiB max each)

```
Block File Format:
[4-byte magic][4-byte size][block data][4-byte magic][4-byte size][block data]...

Undo File Format (rev?????.dat):
[CBlockUndo data for reverting transactions]

Index Database (blocks/index/):
- 'b' + hash → CDiskBlockIndex
- 'f' + num  → CBlockFileInfo
- 'l'        → Last file number
```

---

## 7. Transaction Processing

### 7.1 Transaction Structure

**File:** `src/primitives/transaction.h`

```cpp
class CTxIn {
    COutPoint prevout;           // Previous output reference
    CScript scriptSig;           // Signature script
    uint32_t nSequence;          // Sequence number (BIP68)
    CScriptWitness scriptWitness; // SegWit witness
};

class CTxOut {
    CAmount nValue;              // Value in satoshis
    CScript scriptPubKey;        // Output script
};

class CTransaction {
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    int32_t nVersion;            // 2 for standard, DD marker for DigiDollar
    uint32_t nLockTime;
    const uint256 hash;          // Cached txid
    const uint256 m_witness_hash; // Cached wtxid
};
```

### 7.2 DigiDollar Transaction Encoding

**File:** `src/primitives/transaction.h` (lines 34-58)

```cpp
static constexpr int32_t DD_TX_VERSION = 0x0D1D0770;  // "DigiDollar" marker

enum DigiDollarTxType : uint8_t {
    DD_TX_NONE = 0,      // Regular transaction
    DD_TX_MINT = 1,      // Lock DGB, create DigiDollars
    DD_TX_TRANSFER = 2,  // Transfer DigiDollars
    DD_TX_REDEEM = 3     // Burn DigiDollars, unlock DGB
};

// Version encoding: (type << 24) | (flags << 16) | (DD_TX_VERSION & 0xFFFF)
```

### 7.3 Transaction Validation Flow

```
CheckTransaction() [Context-free]
    ↓
    ├── Non-empty inputs/outputs
    ├── Size within limits
    ├── Output values valid (0 ≤ v ≤ MAX_MONEY)
    ├── No duplicate inputs (CVE-2018-17144)
    ├── Coinbase scriptSig length (2-100 bytes)
    └── DigiDollar type validation

MemPoolAccept::PreChecks() [Mempool validation]
    ↓
    ├── DigiDollar validation context
    ├── Standard transaction checks
    ├── Minimum size (65 bytes)
    ├── Locktime finality
    ├── Duplicate/conflict detection
    ├── UTXO availability
    └── BIP68 sequence lock validation

ConnectBlock() [Consensus validation]
    ↓
    ├── CheckTxInputs() - UTXO availability
    ├── SequenceLocks() - Relative timelocks
    ├── CheckInputScripts() - Script execution
    └── UpdateCoins() - UTXO set update
```

### 7.4 Fee Policy

**File:** `src/policy/policy.h`

| Constant | Value | Purpose |
|----------|-------|---------|
| `DEFAULT_MIN_RELAY_TX_FEE` | 100,000 sat/kvB | Minimum relay fee |
| `DUST_RELAY_TX_FEE` | 30,000 sat/kvB | Dust threshold |
| `DEFAULT_INCREMENTAL_RELAY_FEE` | 10,000 sat/kvB | RBF increment |
| `MAX_STANDARD_TX_WEIGHT` | 400,000 | Max relay weight |
| `DEFAULT_ANCESTOR_LIMIT` | 25 | Max mempool ancestors |
| `DEFAULT_DESCENDANT_LIMIT` | 25 | Max mempool descendants |

---

## 8. Script & Signature Verification

### 8.1 Script Interpreter

**File:** `src/script/interpreter.cpp` (~2300 lines)

```cpp
bool EvalScript(
    std::vector<std::vector<unsigned char>>& stack,
    const CScript& script,
    unsigned int flags,
    const BaseSignatureChecker& checker,
    SigVersion sigversion,
    ScriptExecutionData& execdata,
    ScriptError* serror
);
```

### 8.2 Signature Versions

**File:** `src/script/interpreter.h` (lines 210-216)

```cpp
enum class SigVersion {
    BASE = 0,        // Legacy (P2PKH, P2SH)
    WITNESS_V0 = 1,  // SegWit v0 (P2WPKH, P2WSH)
    TAPROOT = 2,     // Taproot key path
    TAPSCRIPT = 3    // Taproot script path
};
```

### 8.3 Verification Flags

**File:** `src/script/interpreter.h` (script verify flags around lines 43-150)

| Flag | Purpose |
|------|---------|
| `SCRIPT_VERIFY_P2SH` | BIP16 P2SH support |
| `SCRIPT_VERIFY_DERSIG` | Strict DER encoding |
| `SCRIPT_VERIFY_LOW_S` | Low S signatures |
| `SCRIPT_VERIFY_NULLDUMMY` | Zero-length CHECKMULTISIG dummy |
| `SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY` | BIP65 CLTV |
| `SCRIPT_VERIFY_CHECKSEQUENCEVERIFY` | BIP112 CSV |
| `SCRIPT_VERIFY_WITNESS` | BIP141 SegWit |
| `SCRIPT_VERIFY_TAPROOT` | BIP341/342 Taproot |
| `SCRIPT_VERIFY_DIGIDOLLAR` | DigiDollar opcodes (`OP_DIGIDOLLAR`/`OP_DDVERIFY`/`OP_CHECKPRICE`/`OP_CHECKCOLLATERAL`/`OP_ORACLE`); set in `GetBlockScriptFlags()` (`src/validation.cpp:3030, 3071-3072`) only when the buried `DEPLOYMENT_DIGIDOLLAR` deployment is active (BIP90 since v9.26.5). |

### 8.4 DigiDollar Opcodes

**File:** `src/script/script.h:210-214`

```cpp
OP_DIGIDOLLAR = 0xbb,       // DD output / payload marker (Tapscript OP_SUCCESSx slot)
OP_DDVERIFY = 0xbc,         // Verify DD conditions     (Tapscript OP_SUCCESSx slot)
OP_CHECKPRICE = 0xbd,       // Reserved/disabled price check (Tapscript OP_SUCCESSx slot)
OP_CHECKCOLLATERAL = 0xbe,  // Verify collateral ratio   (Tapscript OP_SUCCESSx slot)
OP_ORACLE = 0xbf            // Coinbase oracle bundle marker (Tapscript OP_SUCCESSx slot)
```

`MAX_OPCODE` is now `OP_ORACLE` (`script.h:220`). These opcodes consume BIP-342 OP_SUCCESSx slots (script.h's `OP_NOP10` is `0xb9`, not `0xb8`); they behave as OP_SUCCESSx until `SCRIPT_VERIFY_DIGIDOLLAR` is set, at which point the interpreter dispatches them via `IsDigiDollarOpcode` / `IsOpSuccessForFlags` (`src/script/interpreter.cpp:439-453`) and the per-opcode handlers (around lines 660-754). `OP_CHECKPRICE` is reserved and deterministically disabled; when reached under DigiDollar script flags it consumes its operand and pushes false instead of consulting node-local oracle state.

### 8.5 Taproot Support

**Files:** `src/script/interpreter.cpp` (`ExecuteWitnessScript` at 1963, `VerifyWitnessProgram` at 2048-2129)

- **Key path spending**: Single Schnorr signature (64 bytes)
- **Script path spending**: Control block + script + witness
- **Leaf versions**: 0xc0 for Tapscript (BIP342)
- **CHECKSIGADD** (0xba): Multi-sig accumulator for Tapscript

```cpp
// Taproot constants
TAPROOT_LEAF_MASK = 0xfe
TAPROOT_LEAF_TAPSCRIPT = 0xc0
TAPROOT_CONTROL_BASE_SIZE = 33
TAPROOT_CONTROL_NODE_SIZE = 32
TAPROOT_CONTROL_MAX_NODE_COUNT = 128
```

---

## 9. P2P Networking

### 9.1 Network Architecture

**Files:** `src/net.cpp` (~3900 lines), `src/net_processing.cpp` (~7300 lines)

```
CConnman (Connection Manager)
├── m_nodes[]              Active peer connections
├── ThreadSocketHandler()  I/O polling
├── ThreadOpenConnections() Outbound connections
├── ThreadMessageHandler() Message processing
└── ThreadDandelionShuffle() Privacy route reshuffling

CNode (Peer Connection)
├── m_transport            V1 or V2 (BIP324) transport
├── vSendMsg              Outgoing message queue
├── vRecvMsg              Received message queue
└── Connection metadata   (type, services, latency)

PeerManagerImpl (Message Processing)
├── ProcessMessages()     Route incoming messages
├── SendMessages()        Queue outgoing messages
└── RelayTransaction()    Broadcast transactions
```

### 9.2 Connection Types

| Type | Purpose | Count |
|------|---------|-------|
| OUTBOUND_FULL_RELAY | Full tx/block/addr relay | 16 |
| BLOCK_RELAY | Blocks only (privacy) | 2 |
| MANUAL | -addnode connections | 8 |
| INBOUND | Peer-initiated | Up to 125 total |
| FEELER | Quality testing | 1 |
| PAYMASTER | Short-lived isolated Paymaster session | At most 1 in high-privacy mode |

### 9.3 Dandelion++ Privacy

**File:** `src/dandelion.cpp` (~500 lines)

```
Transaction Privacy Flow:
1. Local tx enters stempool (private)
2. Embargo set: random(10-30 seconds)
3. Send to single Dandelion destination (stem phase)
4. Embargo expires → move to mempool (fluff phase)
5. Broadcast to all peers (normal relay)

Route Shuffling:
- Every 10 minutes: reshuffle destination mapping
- 2 maximum destinations per node
- Discovery via special INV message (DANDELION_DISCOVERYHASH)
```

**Key Constants:**
```cpp
DANDELION_MAX_DESTINATIONS = 2
DANDELION_SHUFFLE_INTERVAL = 10 minutes
DANDELION_EMBARGO_MINIMUM = 10 seconds
DANDELION_EMBARGO_AVG_ADD = 20 seconds
DANDELION_FLUFF = 10  // 10% immediate fluff probability
```

### 9.4 V2 Transport (BIP324)

**Files:** `src/bip324.cpp`, `src/net.cpp`/`src/net.h` (`V2Transport` declared around `net.h:469`)

- **Key exchange**: Elliptic Curve Diffie-Hellman (ElligatorSwift, BIP324)
- **Encryption**: ChaCha20-Poly1305 AEAD with forward-secure rekeying
- **Garbage padding**: Up to 4095 bytes
- **Fallback**: Automatic V1 detection on protocol-magic mismatch
- **Enable flag**: `-v2transport=1` (off by default; outbound use is gated by per-connection `use_v2transport` in `CConnman::OpenNetworkConnection`)

### 9.5 Message Types

**File:** `src/protocol.h`

| Message | Purpose |
|---------|---------|
| VERSION/VERACK | Handshake |
| INV/GETDATA/TX/BLOCK | Data exchange |
| HEADERS/GETHEADERS | Headers-first sync |
| PING/PONG | Keepalive |
| ADDR/ADDRV2 | Address gossip |
| CMPCTBLOCK | Compact blocks (BIP152) |
| **DANDELIONTX** | Stem phase transaction |
| **ORACLEPRICE** | Individual oracle price message |
| **ORACLEBUNDLE** | Legacy oracle bundle wrapper; V1 block data uses MuSig2 coinbase bundles |
| **GETORACLES** | Oracle data request |
| **ORACLECONSENSUS** / **ORACLEATTESTATION** | MuSig2 oracle consensus/attestation coordination |
| **ORACLEMUSIGNONCE** / **ORACLEMUSIGCONTEXT** / **ORACLEMUSIGPARTIALSIG** | MuSig2 nonce/context/partial-signature exchange |
| **ORACLEHEARTBEAT** | Oracle liveness heartbeat |
| **SENDPMASTERS** / **PMANNOUNCE** / **GETPMASTERS** | Paymaster discovery and bounded announcement exchange |
| **PMCAPREQ** / **PMCAPRESP** | Selected-provider operational capacity proof |
| **PMQUOTEREQ** / **PMQUOTERESP** | Bound payment intent and provider quote |
| **PMSUBMIT** / **PMRESULT** | User-signed PSBT submission and monotonic provider result |

All Paymaster message handling is gated by DigiDollar activation. On an
isolated connection, `PMCAPREQ`, `PMQUOTEREQ`, `PMSUBMIT`, `PMRECOVERYREQ`,
and `PMRECOVERYSUBMIT` flow client-to-provider; their response counterparts
flow provider-to-client. `GETPMASTERS` returns at most 16 entries from a
rotating bounded directory view.

---

## 10. Wallet System

### 10.1 Wallet Architecture

**File:** `src/wallet/wallet.h`

```cpp
class CWallet {
    // Transaction management
    std::map<uint256, CWalletTx> mapWallet;

    // Key management
    std::map<uint256, std::unique_ptr<ScriptPubKeyMan>> m_spk_managers;

    // Address book
    std::map<CTxDestination, CAddressBookData> m_address_book;

    // DigiDollar integration
    std::unique_ptr<DigiDollarWallet> m_dd_wallet;

    // Locking
    mutable RecursiveMutex cs_wallet;
};
```

### 10.2 Key Management

**File:** `src/wallet/scriptpubkeyman.h`

```
ScriptPubKeyMan (Abstract Base)
├── LegacyScriptPubKeyMan
│   ├── HD derivation (external/internal chains)
│   ├── Keypool management (1000 keys default)
│   └── P2PKH, P2SH-P2WPKH, P2WPKH support
│
└── DescriptorScriptPubKeyMan
    ├── BIP32/44/49/84/86 descriptors
    ├── Taproot (tr()) support
    └── DigiDollar key derivation
```

### 10.3 Transaction Creation

**File:** `src/wallet/spend.cpp`

```
CreateTransaction() Flow:
1. SelectCoins() - Choose UTXOs
   ├── BNB (Branch and Bound) - Optimal
   ├── Knapsack - Fallback
   └── SRD (Single Random Draw) - Simple
2. Build outputs with fees
3. Add change output
4. SignTransaction() - Sign inputs
   ├── ECDSA for legacy/SegWit
   └── Schnorr for Taproot
```

### 10.4 DigiDollar Wallet

**File:** `src/wallet/digidollarwallet.h`

```cpp
class DigiDollarWallet {
    // Key management
    std::map<uint256, CKey> dd_owner_keys;    // For vault redemption
    std::map<XOnlyPubKey, CKey> dd_address_keys; // For received DD

    // UTXO tracking
    std::map<COutPoint, CAmount> dd_utxos;    // DD amount per UTXO

    // Position tracking
    std::map<uint256, WalletCollateralPosition> collateral_positions;

    // Operations
    bool MintDigiDollar(...);
    bool TransferDigiDollar(...);
    bool RedeemDigiDollar(...);
};
```

Paymaster state is wallet scoped. `PaymasterStore` persists sessions, exact
provider attempts, input reservations, final provider commits, self-recovery
transactions, local reputation, sponsorship authorizations, and permanent
idempotency tombstones in the wallet database. The provider store additionally
persists pool-successor provenance, wallet-local liquidity targets, restartable
maintenance records, finite maintenance-fee accounting, and the latest reviewed
carrier-withdrawal plan. Paymaster reservations also use the ordinary wallet
lock set, so regular DD and DGB coin selection cannot spend active pool or
reserved inputs. Immediately before a new provider signature, all committed
template inputs are rechecked against chainstate and mempool. If client
recovery has already spent a user input, the provider atomically persists a
signed `REJECTED` result and releases only the still-unused pool entries bound
to that commit key; no provider signature or final commit is created.

---

## 11. RPC Interface

### 11.1 RPC Architecture

**Files:** `src/rpc/server.cpp`, `src/httprpc.cpp`

```
HTTP POST → Authorization → JSON Parse → CRPCTable::execute() → Response

Authentication Methods:
1. Cookie-based (default, secure)
2. Username/password (deprecated)
3. RPC auth with salt/hash (multi-user)
```

### 11.2 Command Categories

| Category | Commands | File |
|----------|----------|------|
| **blockchain** | getblock, getblockcount, getdifficulty... | blockchain.cpp |
| **mining** | getmininginfo, getblocktemplate, submitblock... | mining.cpp |
| **network** | getpeerinfo, addnode, getnetworkinfo... | net.cpp |
| **rawtransaction** | createrawtransaction, signrawtransaction... | rawtransaction.cpp |
| **wallet** | getnewaddress, sendtoaddress, listunspent... | wallet/rpc/*.cpp |
| **digidollar** | mintdigidollar, redeemdigidollar, getoracleprice... | digidollar.cpp |
| **paymaster** | listpaymasters, getpaymasteroffers, session and provider operations... | rpc/digidollar.cpp, wallet/rpc/paymaster*.cpp |

### 11.3 DigiByte-Specific RPC

**Files:** `src/rpc/blockchain.cpp` (DigiByte-specific helpers), `src/rpc/digidollar.cpp` (DigiDollar / oracle), wallet-context DD RPCs in `src/wallet/rpc/wallet.cpp`.

| Command | File | Purpose |
|---------|------|---------|
| `getblockreward` | `rpc/blockchain.cpp` | Current block subsidy with multi-algo awareness |
| `getdigidollarstats` | `rpc/digidollar.cpp` | Network-wide DD statistics |
| `getdcamultiplier` | `rpc/digidollar.cpp` | DCA collateral multiplier |
| `getoracleprice` / `getalloracleprices` | `rpc/digidollar.cpp` | Live oracle consensus / per-oracle prices |
| `getoracles` / `listoracle` / `getoraclepubkey` | `rpc/digidollar.cpp` | Oracle roster introspection |
| `validateddaddress` (wallet) | `wallet/rpc/wallet.cpp` | Validate DD address |
| `mintdigidollar` / `senddigidollar` / `redeemdigidollar` (wallet) | `wallet/rpc/wallet.cpp` | DigiDollar mint/transfer/redeem |
| `setmockoracleprice` / `getmockoracleprice` / `simulatepricevolatility` / `enablemockoracle` | `rpc/digidollar.cpp` | Regtest-only oracle helpers |
| `listpaymasters` | `rpc/digidollar.cpp` | List locally verified, unexpired announcements |
| `getpaymasteroffers` / `requestpaymasterquote` | `wallet/rpc/paymaster_discovery.cpp` | Select offers and advance a persistent client session |
| `getdigidollarsendsession` / `resolvepaymastersession` | `wallet/rpc/paymaster_client.cpp` | Inspect, retry, fall back, or recover a session safely |
| `createpaymasteridentity` / `setpaymasterpolicy` / `preparepaymasterpool` | `wallet/rpc/paymaster_provider.cpp` | Configure a wallet-scoped provider and its isolated pools |
| `setpaymasterliquiditypolicy` / `getpaymasterliquiditystatus` | `wallet/rpc/paymaster_provider.cpp` | Configure finite automatic pool-maintenance targets/budgets and inspect confirmed, pending, or missing capacity |
| `withdrawpaymastercarrier` | `wallet/rpc/paymaster_provider.cpp` | Preview and execute wallet-owned carrier-excess consolidation or release one stopped-provider carrier slot |
| `createrestrictedpaymasterdescriptor` | `wallet/rpc/paymaster_provider.cpp` | Create a non-gossiped provider-signed restricted sponsorship descriptor |
| `startpaymaster` / `stoppaymaster` / `getpaymasterinfo` | `wallet/rpc/paymaster_runtime.cpp`, `wallet/rpc/paymaster_provider.cpp` | Operate and inspect an explicitly enabled provider |

Both `sendoracleprice` and `submitoracleprice` are absent from the source tree — `sendoracleprice` was removed as a fake-price-injection vulnerability and `submitoracleprice` never existed. Oracle prices come exclusively from live exchange aggregation. See `REPO_MAP_DIGIDOLLAR.md` for the complete RPC inventory.

### 11.4 Error Codes

**File:** `src/rpc/protocol.h`

| Code | Meaning |
|------|---------|
| -1 | Misc error |
| -5 | Invalid address/key |
| -6 | Insufficient funds |
| -8 | Invalid parameter |
| -25 | Verification error |
| -32600 | Invalid JSON-RPC request |
| -32601 | Method not found |
| -32602 | Invalid params |

---

## 12. DigiDollar Stablecoin

### 12.1 System Overview

DigiDollar is a native, decentralized stablecoin built on DigiByte using over-collateralized DGB positions.

```
User Action Flow:

MINT: Lock DGB → Create DigiDollars
┌─────────────────────────────────────────────────────┐
│ 1. Select lock period (1 hour - 10 years)            │
│ 2. Get oracle price                                  │
│ 3. Calculate collateral (200-1000% based on tier)   │
│ 4. Apply DCA multiplier if system health < 150%     │
│ 5. Create P2TR vault (collateral locked)            │
│ 6. Create P2TR DD tokens (transferable)             │
│ 7. Record position in OP_RETURN metadata            │
└─────────────────────────────────────────────────────┘

TRANSFER: Send DigiDollars
┌─────────────────────────────────────────────────────┐
│ 1. Validate DD address (DD/TD/RD prefix)            │
│ 2. Select DD UTXOs                                   │
│ 3. Create DD outputs to recipient                   │
│ 4. Sign with Schnorr (P2TR key-path)               │
│ 5. Include OP_RETURN with DD amounts               │
└─────────────────────────────────────────────────────┘

REDEEM: Burn DigiDollars → Unlock DGB
┌─────────────────────────────────────────────────────┐
│ 1. Check timelock expired (REQUIRED)                │
│ 2. Determine path (NORMAL or ERR)                   │
│    NORMAL: health ≥ 100% → burn original DD        │
│    ERR: health < 100% → burn MORE DD (105-125%)    │
│ 3. Burn DD tokens (remove from UTXO)               │
│ 4. Return 100% collateral (ALWAYS full amount)     │
└─────────────────────────────────────────────────────┘
```

### 12.2 Collateral Tiers

**File:** `src/consensus/digidollar.cpp`

| Lock Period | Collateral Ratio | Use Case |
|-------------|------------------|----------|
| 1 hour | 1000% | Testing/onboarding tier |
| 30 days | 500% | Short-term |
| 3 months | 400% | Quarterly |
| 6 months | 350% | Semi-annual |
| 1 year | 300% | Annual |
| 2 years | 275% | Bi-annual |
| 3 years | 250% | Medium-term |
| 5 years | 225% | Long-term |
| 7 years | 212% | Extended |
| 10 years | 200% | Maximum lock |

### 12.3 Protection Systems

#### DCA (Dynamic Collateral Adjustment)

**File:** `src/consensus/dca.cpp`

```
System Health Tiers:
≥150%: Healthy    → 1.0x multiplier (no change)
120-149%: Warning → 1.25x multiplier (+25%)
110-119%: Critical → 1.5x multiplier (+50%)
<110%: Emergency  → 2.0x multiplier (+100%)
```

#### ERR (Emergency Redemption Ratio)

**File:** `src/consensus/err.cpp`

```
ERR increases DD burn requirement (NOT reduces collateral):

System Health → ERR Ratio → DD Burn Required
95-100%       → 0.95      → 105.3% (burn more DD)
90-95%        → 0.90      → 111.1%
85-90%        → 0.85      → 117.6%
<85%          → 0.80      → 125.0% (maximum)

Collateral return: ALWAYS 100% of locked DGB
New minting: BLOCKED during ERR
```

#### Volatility Protection

**File:** `src/consensus/volatility.cpp`

- Monitors hourly/daily/weekly price changes
- Freezes minting during high volatility (>20%)
- Prevents destabilizing transactions

### 12.4 Address Formats

**File:** `src/base58.cpp`

```cpp
// 2-byte Base58Check version prefixes
DD_P2TR_MAINNET = {0x52, 0x85}  // "DD" prefix
DD_P2TR_TESTNET = {0xb1, 0x29}  // "TD" prefix
DD_P2TR_REGTEST = {0xa3, 0xa4}  // "RD" prefix

// Examples:
Mainnet: DD1q2w3e4r5t6y7u8i9o0p...
Testnet: TD1q2w3e4r5t6y7u8i9o0p...
Regtest: RD1q2w3e4r5t6y7u8i9o0p...
```

### 12.5 Transaction Structure

```
MINT Transaction:
├── Inputs: DGB UTXOs
├── Outputs:
│   ├── [0] P2TR collateral vault (locked DGB)
│   ├── [1] P2TR DD token (0 DGB value)
│   ├── [2] OP_RETURN: "DD" 1 <amount> <lockHeight> <tier>
│   └── [3+] DGB change
└── Version: 0x0D1D0770 | (1 << 24)

TRANSFER Transaction:
├── Inputs: DD UTXOs + fee UTXOs
├── Outputs:
│   ├── [0-N] P2TR DD outputs to recipients
│   ├── [N+1] OP_RETURN: "DD" 2 <amount1> <amount2>...
│   └── [N+2] DGB change for fees
└── Version: 0x0D1D0770 | (2 << 24)

REDEEM Transaction:
├── Inputs:
│   ├── [0] Collateral vault (with CLTV)
│   ├── [1+] DD UTXOs to burn
│   └── [N+] Fee UTXOs
├── Outputs:
│   ├── [0] DGB returned to owner (100% collateral)
│   ├── [1] DD change (if partial burn)
│   ├── [2] OP_RETURN: "DD" 3 <changeAmount>
│   └── [3] DGB fee change
└── Version: 0x0D1D0770 | (3 << 24)
```

### 12.6 Network-Wide Tracking

**File:** `src/digidollar/health.cpp`

#### Baseline: UTXO Scanning

The system uses UTXO scanning for initial decentralized statistics:

```cpp
void SystemHealthMonitor::ScanUTXOSet(CCoinsView* view, ...)
{
    // Iterate ALL UTXOs in blockchain
    std::unique_ptr<CCoinsViewCursor> pcursor(view->Cursor());

    while (pcursor->Valid()) {
        // Find DD vaults: P2TR output 0 with value > 0
        if (key.n == 0 && coin.out.scriptPubKey[0] == OP_1 && coin.out.nValue > 0) {
            CTransactionRef tx = GetTransaction(...);
            if (DigiDollar::ExtractDDAmount(tx->vout[2].scriptPubKey, ddAmount)) {
                s_currentMetrics.totalDDSupply += ddAmount;
                s_currentMetrics.totalCollateral += dgbCollateral;
            }
        }
        pcursor->Next();
    }
}
```

#### Incremental Tracking (T5-06)

After the initial UTXO scan, DD supply and collateral are tracked incrementally via `ConnectBlock()`/`DisconnectBlock()` callbacks:

```cpp
// Called from ConnectBlock() under cs_main when a DD mint is connected:
static void OnMintConnected(CAmount ddAmount, CAmount dgbCollateral);

// Called from ConnectBlock() under cs_main when a DD redeem is connected:
static void OnRedeemConnected(CAmount ddAmount, CAmount dgbCollateral);

// Reverse operations for DisconnectBlock() (reorgs):
static void OnMintDisconnected(CAmount ddAmount, CAmount dgbCollateral);
static void OnRedeemDisconnected(CAmount ddAmount, CAmount dgbCollateral);
```

This avoids rescanning the entire UTXO set for every health check. The ERR system uses these live metrics to determine whether emergency redemption is active.

**Result:** All nodes see identical network statistics, updated incrementally per block.

### 12.7 Paymaster Network (non-consensus)

**Specification:** [`DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md`](DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md)
**Key files:** `src/paymaster/`, `src/wallet/paymaster*.{cpp,h}`,
`src/wallet/rpc/paymaster*.{cpp,h}`

The Paymaster Network lets a wallet with confirmed DD but no spendable DGB
construct a normal `DD_TX_TRANSFER` with a provider supplying the DGB miner-fee
inputs. It introduces no transaction type, script rule, service bit, chain
parameter, or privileged validator role.

```
bounded announcement gossip
        ↓
local admission-proof validation and offer selection
        ↓
isolated BIP324-v2 PAYMASTER connection to one selected provider
        ↓
authenticated Capacity V5 proof for one exact provider resource set
        ↓
payment intent → resource-bound quote → local client manifest/firewall
        ↓
provider manifest/budget firewall → role-limited signatures
        ↓
full witness/script/mempool validation → atomic exact raw-tx commit
        ↓
ordinary DD_TX_TRANSFER broadcast and wallet confirmation/reorg tracking
        ↓
wallet-owned carrier/DGB successors → confirmation-gated pool reuse
        ↓
finite-budget maintenance only when configured targets remain missing
```

- `-paymaster=1` enables discovery, relay, and client support. Provider
  operation additionally requires an eligible descriptor wallet, a persisted
  BIP86 identity and policy, prepared admission/operational pools, explicit
  enablement, and `startpaymaster`.
- `USER_PAID` and public or restricted `SPONSORED` offers use the same
  transaction protocol. Restricted capabilities are payment-bound and their
  durable records retain hashes rather than plaintext authorization material.
- Standard privacy requires v2 transport with no v1 fallback. High privacy is
  onion-only, uses Tor stream isolation, permits one provider attempt, and has
  no clearnet fallback. This reduces metadata; it does not provide anonymity.
- Persistent `request_id` sessions prevent duplicate payment. Once user
  signatures make a session ambiguous, timeout does not release its inputs;
  recovery uses exact-provider retry or a same-input `cancel_to_self` spend
  funded by a distinct Capacity-validated recovery provider when the client
  has no DGB.
- Paymaster is current-only: wire requests and responses must use protocol V5,
  and every intent, quote, manifest, recovery object, session, attempt, safety
  ledger, pool entry, and maintenance record must use its exact current
  persisted version. There is no automatic migration or executable legacy
  fallback. An unsupported Paymaster record stops only the affected Paymaster
  operation and is never erased or rewritten implicitly.
- The signed Capacity proof is verified against identity, BIP86 control proofs
  and live chainstate before the client discloses its intent, DD outpoints, or
  restricted capability. The quote must consume the exact resource snapshot.
- Client and provider signatures are gated by immutable wallet-local
  authorization manifests. Finite provider and client safety policies reserve
  worst-case fees atomically, persist rolling limits across restart, and make
  public sponsorship bounded rather than unlimited.
- Client requests normally treat the RPC amount as the recipient amount. An
  explicit wallet-local exact-outflow mode instead derives a recipient amount
  whose value plus the existing rounded service fee equals the requested gross
  amount. The gross amount, fee treatment, exact recipient/fee split, and input
  snapshot are persisted and bound by the client authorization manifest. A
  wallet sweep selects only confirmed ordinary spendable DD and fails closed if
  that snapshot changes; it does not alter the Paymaster wire protocol.
- A final result is accepted only after reconstructing the trusted PSBT and
  verifying the non-witness transaction, txid, wtxid, all witnesses and scripts
  with the real prevouts, followed by mempool preflight when it is not already
  known. Final transitions and their economic reservations are monotonic and
  atomic.
- Wallet lifecycle maintenance runs the same persistent reconciliation every
  30 seconds, in addition to startup and chain-tip notifications. Expired
  unsigned quotes and Capacity reservations are therefore released through the
  atomic store paths even when no further Paymaster RPC arrives and no block is
  connected.
- A successful provider commit registers the exact wallet-owned carrier return
  and sufficiently large DGB change as provenance-bound pending successors.
  Confirmation makes them available; reorg or conflict returns them to a safe
  pending/invalid state. Startup reconciliation can recover still-unspent
  historical successors without guessing by amount or output position.
- A wallet-local `ProviderLiquidityPolicy` defines admission/operational DGB and
  carrier targets plus finite maintenance-fee ceilings per transaction, hour,
  and day. A started automatic provider reuses free successors first, resumes
  durable maintenance idempotently, creates only missing slots, and accepts no
  new work until the target is confirmed. Paid maintenance requires explicit
  operator approval; zero limits never mean unlimited.
- Carrier fees can accumulate inside operational carrier outputs. The
  preview-bound `withdrawpaymastercarrier` RPC can retain exactly 1.00 DD per
  carrier and combine the excess, or release one carrier locally while reducing
  the target. Reserved or authorized carriers remain protected.
- Client/provider readiness requires an unpruned node, a fully synchronized
  `txindex=1`, complete DD transaction data, and an unlocked wallet for new
  signatures. Client quote creation fails before intent processing when these
  archival requirements are unavailable. Ordinary relay and validation do not
  require client or provider readiness.

---

## 13. Oracle System

### 13.1 Architecture Overview

**Files:** `src/oracle/`, `src/primitives/oracle.h`

```
DigiDollar V1 ships with MuSig2 v0x03 oracle bundles only — legacy single-oracle
and multi-oracle bundle paths have been removed (commits f2bb0a19a4, bbb85cf363).

Data Flow:
Exchange APIs → Per-oracle Schnorr quote → MuSig2 nonce/partial-sig session →
Aggregate Schnorr signature + participation bitmap → Coinbase OP_RETURN
                                         ↓
                    Block validation extracts COracleBundle (v0x03)
                                         ↓
                    IQR outlier filtering → median consensus price (deterministic)
                                         ↓
                    Block-extracted oracle price → DigiDollar tx validation
```

#### Activation

| Network | DD activation (`nDDActivationHeight`) | Oracle activation (`nOracleActivationHeight`) | MuSig2 (`nDigiDollarMuSig2Height`) | On-chain quorum |
|---------|--------------------------------------|----------------------------------------------|-----------------------------------|-----------------|
| Mainnet | 23,627,520 | 23,627,520 (= DD) | 23,627,520 (= DD) | 7 signatures from 35 configured active keys |
| Testnet26 | 600 | 600 (= DD) | 600 (= DD) | 7 signatures from 35 configured active keys |
| Regtest | 650 | 650 (= DD) | 0 (= buried `DigiDollarHeight`) | 4-of-7 |

`nDigiDollarMuSig2Height` now collapses to the effective DigiDollar activation boundary: `nDDActivationHeight` on mainnet/testnet, and the buried `DigiDollarHeight = 0` on default regtest (v9.26.5 burial; formerly the BIP9 `ALWAYS_ACTIVE` boundary). The legacy `nDigiDollarPhase2Height` / `nDigiDollarPhase3Height` fields no longer exist.

### 13.2 Price Message Structure

**File:** `src/primitives/oracle.h`

```cpp
class COraclePriceMessage {
    uint32_t oracle_id;              // 4 bytes
    uint64_t price_micro_usd;        // 8 bytes (1,000,000 = $1.00)
    int64_t timestamp;               // 8 bytes
    int32_t block_height;            // 4 bytes
    uint64_t nonce;                  // 8 bytes
    XOnlyPubKey oracle_pubkey;       // 32 bytes
    std::vector<unsigned char> schnorr_sig; // 64 bytes
    // Total: 128 bytes
};
```

### 13.3 Exchange Integration

**Files:** `src/oracle/exchange.h` (281 lines), `src/oracle/exchange.cpp` (~1,333 lines)

Eleven fetcher classes derive from `BaseExchangeFetcher` (`src/oracle/exchange.h:25-215`):
Binance, Coinbase, Kraken, CoinGecko, Bittrex, Poloniex, Messari, KuCoin, Crypto.com, Gate.io and HTX. Which of them run is not a setting: `MultiExchangeAggregator::InitializeFetchers` creates exactly six of them in the source (`src/oracle/exchange.cpp:1092-1097`) — Binance, CoinGecko, KuCoin, Gate.io, HTX and Crypto.com. The other five compile but are never created. See `DIGIDOLLAR_ORACLE_ARCHITECTURE.md`.

```cpp
MultiExchangeAggregator:
1. Fetch prices in parallel (libcurl)
2. Filter failures and stale quotes
3. Remove outliers
4. Calculate median (per-source weights honoured)
5. Convert to micro-USD before signing
```

### 13.4 V1 Oracle Coinbase Format

**File:** `src/oracle/bundle_manager.cpp`

```
Coinbase OP_RETURN:
┌──────────────────────────────────────────────────────────────┐
│ OP_RETURN OP_ORACLE <0x03> <bitmap_len + bitmap + epoch +    │
│ price_micro_usd + timestamp + 64-byte MuSig2 aggregate sig>  │
└──────────────────────────────────────────────────────────────┘
```

Legacy v0x01/v0x02 oracle payloads are not accepted on-chain in V1.

### 13.5 Consensus Price Calculation (IQR)

**File:** `src/oracle/bundle_manager.cpp` — `CalculateConsensusPrice()`

The consensus price is computed deterministically from bundle messages using IQR (Interquartile Range) outlier filtering:

```
1. Filter messages by price range only (NOT timestamp — avoids wall-clock dependency)
2. Sort valid prices
3. If < 4 prices: return simple median (no outlier filtering possible)
4. Compute Q1 (25th percentile) and Q3 (75th percentile)
5. IQR = Q3 - Q1
6. Lower bound = Q1 - 1.5 × IQR
7. Upper bound = Q3 + 1.5 × IQR
8. Filter outliers outside [lower, upper]
9. Return median of remaining prices
```

**SECURITY:** `CalculateConsensusPrice()` deliberately avoids `GetTime()` for timestamp checks. Timestamp validation uses `block.nTime` in `ValidateBlockOracleData()` to ensure deterministic consensus independent of wall-clock time (prevents chain splits during IBD or delayed relay).

### 13.6 Block-Extracted Oracle Price

**File:** `src/validation.cpp`

During `ConnectBlock()`, the oracle price is extracted directly from the block's coinbase OP_RETURN rather than queried from memory. This ensures DD transaction validation uses the exact same price the miner used:

```cpp
// In ConnectBlock():
COracleBundle extractedBundle;
if (oracleManager.ExtractOracleBundle(*block.vtx[0], extractedBundle) &&
    extractedBundle.median_price_micro_usd > 0) {
    blockOraclePrice = extractedBundle.median_price_micro_usd;
}
// This price is passed to DD validation — deterministic, block-local
```

### 13.7 Bundle Validation (V1: MuSig2 only)

**File:** `src/oracle/bundle_manager.cpp` (also `src/validation.cpp`)

```cpp
// On-chain validation path:
ValidateBundle(bundle, height, params)
  → bundle.version must be 3 (v0x03 MuSig2);
    legacy v0x01/v0x02 bundles are rejected once DEPLOYMENT_DIGIDOLLAR is active
    (src/oracle/bundle_manager.cpp commit f2bb0a19a4 / bbb85cf363).
  → Validate aggregate Schnorr signature against the MuSig2 aggregate of
    `consensus.vOraclePublicKeys` and the participation bitmap.
  → Apply IQR (1.5×) outlier filtering on the input price quotes used to
    compute the bundle's median price (deterministic; uses block.nTime, not
    GetTime()).
```

### 13.8 Mock Oracle (Regtest only)

**File:** `src/oracle/mock_oracle.cpp`

Mock prices are only available on regtest under the `OracleManagerInterface` shim. Mock-related RPCs (`setmockoracleprice`, `getmockoracleprice`, `simulatepricevolatility`, `enablemockoracle`) are gated to regtest in `src/rpc/digidollar.cpp`. `OP_CHECKPRICE` is reserved and deterministically disabled, so it does not read either mock prices or live node-local oracle state.

### 13.9 Mining Graceful Degradation (DD txs)

**File:** `src/node/miner.cpp`

`BlockAssembler::CreateNewBlock` strips DigiDollar transactions that fail validation in `mapModifiedTx` rather than aborting block assembly (commit `6b5ff516c3`). When no MuSig2 oracle bundle is ready for the candidate height, `addPackageTxs` skips DD-marker transactions and the resulting template falls back to a non-DD block; if a DD tx slips through and `TestBlockValidity` reports a retryable DD-only failure, the block is reassembled without DD txs (`miner.cpp` around lines 165-461).

---

## Appendix A: Key Line Number References

### Consensus & Validation
| File | Function/Class | Approx. Lines |
|------|---------------|---------------|
| validation.cpp | CheckBlock() | 4624+ |
| validation.cpp | ContextualCheckBlock() | 4842+ |
| validation.cpp | ContextualCheckBlockHeader() | 4784+ |
| validation.cpp | Chainstate::ConnectBlock() | 2808+ |
| validation.cpp | GetBlockSubsidy() | 2119-2194 |
| validation.cpp | IsAlgoActive() | 2196+ |
| validation.cpp | GetOraclePriceForTransaction() | 2073+ |
| pow.cpp | GetNextWorkRequiredV4() | 192-254 |
| pow.cpp | GetNextWorkRequired() (dispatcher) | 256-285 |
| pow.cpp | CheckProofOfWork() | 376+ |

### Block & Chain
| File | Function/Class | Lines |
|------|---------------|-------|
| chain.h | CBlockIndex | 146-384 |
| chain.h | BlockStatus enum | 85-139 |
| chain.cpp | GetAlgoForBlockIndex() | 152-174 |
| primitives/block.h | CBlockHeader | 85-139 |
| primitives/block.cpp | GetPoWAlgoHash() | 56-106 |

### Script & Crypto
| File | Function/Class | Lines |
|------|---------------|-------|
| interpreter.cpp | EvalScript() | 439-1380 |
| interpreter.cpp | CheckSchnorrSignature() | 1819-1847 |
| interpreter.cpp | Taproot verification | 2055-2098 |
| interpreter.cpp | DigiDollar opcodes | 651-754 |
| script.h | Opcodes enum | 70-217 |

### Networking
| File | Function/Class | Lines |
|------|---------------|-------|
| net.h | CNode | 696-1029 |
| net.h | CConnman | 1074-1698 |
| net.cpp | SocketHandler() | 2052-2186 |
| dandelion.cpp | DandelionShuffle() | 329-383 |
| net_processing.cpp | ProcessMessages() | 5560+ |

### DigiDollar
| File | Function/Class | Lines |
|------|---------------|-------|
| digidollar/digidollar.h | CCollateralPosition | 65-112 |
| digidollar/txbuilder.cpp | BuildMintTransaction() | 278-422 |
| digidollar/txbuilder.cpp | BuildRedemptionTransaction() | 1041-1265 |
| digidollar/health.cpp | ScanUTXOSet() | 291-502 |
| consensus/dca.cpp | GetDCAMultiplier() | 85+ |
| consensus/err.cpp | GetRequiredDDBurn() | 112+ |

---

## Appendix B: Configuration Files

### Main Configuration
- `~/.digibyte/digibyte.conf` - Node configuration
- `~/.digibyte/wallet.dat` - Legacy wallet
- `~/.digibyte/wallets/` - Descriptor wallets

### Data Directories
- `~/.digibyte/blocks/` - Block data files
- `~/.digibyte/chainstate/` - UTXO database
- `~/.digibyte/indexes/` - Optional indexes

### Key Parameters
```ini
# Network
testnet=1
regtest=1

# Mining
algo=sha256d|scrypt|groestl|skein|qubit|odocrypt

# DigiDollar (regtest helpers shown; live oracle aggregation has no static price config)
digidollar=1
digidollarstatsindex=1   # Maintain DigiDollar stats index
# mockoracle=1            # regtest only — see init.cpp -enablemockoracle args
# oracleprice=6500        # regtest mock seeding (RPC setmockoracleprice preferred)

# P2P
v2transport=1            # Opt in to BIP324 V2 transport (off by default)

# Debug
debug=digidollar
debug=net
debug=validation
```

---

## Appendix C: Test Suites

### Unit Tests
- `src/test/` - C++ unit tests (Boost.Test); see `REPO_MAP_DIGIDOLLAR.md` for the granular DigiDollar/Oracle/MuSig2/Red-Hornet test inventory (~150 unit suites at last count).
- `src/test/paymaster_*` - Paymaster builder, protocol, provider, sponsorship, discovery, wire, selection, reputation, and state-machine tests.
- `src/wallet/test/` - DigiDollar wallet suites plus Paymaster identity, role-limited PSBT, persistence, reservation, recovery, reorg, and idempotency tests.
- `src/qt/test/` - DigiDollar Qt widget tests (`digidollarwidgettests.cpp`, `digidollarwave19widgettests.cpp`); generated `moc_*.cpp` files are build products.

### Functional Tests
- `test/functional/` - Python integration tests; DD/oracle coverage is registered through `digidollar_*`, `wallet_digidollar_*`, and `feature_oracle_p2p.py`; `wallet_paymaster_provider.py` covers the descriptor-provider lifecycle, high-privacy selection rejection, user-paid/public/restricted transfers, and recovery.

### Running Tests
```bash
# Unit tests
make check

# Specific test suite
./src/test/test_digibyte --run_test=digidollar_validation_tests
./src/test/test_digibyte --list_content | grep -Ei 'digidollar|oracle|musig|^rh'

# Functional tests
./test/functional/test_runner.py

# Single functional test
./test/functional/digidollar_basic.py --nocleanup
```

---

## Appendix D: Codebase Validation Report

### Validation Date: 2026-05-20

This architecture document has been spot-validated against the active DigiByte Core v9.26.2 codebase on `feature/digidollar-v1` across the major subsystems. Generated/build trees (`.deps`, `.libs`, `*.o`, `*.lo`, Qt `moc_*.cpp`, Qt `forms/ui_*.h`) were ignored.

### Validation Summary

| Subsystem | Status | Key Files Verified |
|-----------|--------|-------------------|
| Consensus Layer | Verified | `validation.cpp`, `consensus/params.h`, `consensus/consensus.h` |
| Multi-Algorithm Mining | Verified | `primitives/block.h`, `primitives/block.cpp`, `crypto/*` |
| Difficulty Adjustment | Verified | `pow.cpp`, `kernel/chainparams.cpp` |
| Block/Chain Management | Verified | `chain.h`, `coins.h`, `txdb.h` |
| Transaction Processing | Verified | `primitives/transaction.h`, `validation.cpp` |
| Script & Signatures | Verified | `script/interpreter.cpp`, `script/script.h` |
| P2P Networking | Verified | `net.cpp`, `net_processing.cpp`, `dandelion.cpp`, `bip324.cpp` |
| Wallet System | Verified | `wallet/wallet.h`, `wallet/scriptpubkeyman.h`, `wallet/digidollarwallet.h` |
| RPC Interface | Verified | `rpc/server.cpp`, `rpc/digidollar.cpp`, `httprpc.cpp`, `wallet/rpc/wallet.cpp` |
| DigiDollar Stablecoin | Verified | `digidollar/*`, `consensus/dca.cpp`, `consensus/err.cpp` |
| Oracle System | Verified | `oracle/*`, `primitives/oracle.h` |
| Paymaster Network | Implemented; release-gate testing in progress | `paymaster/*`, `wallet/paymaster*`, `wallet/rpc/paymaster*`, Qt DD send/provider surfaces |

### Key Verified Constants

| Constant | Value | File:Line |
|----------|-------|-----------|
| COINBASE_MATURITY | 8 | consensus/consensus.h:20 |
| COINBASE_MATURITY_2 | 100 | consensus/consensus.h:21 |
| MAX_BLOCK_WEIGHT | 4,000,000 | consensus/consensus.h:15 |
| MAX_BLOCK_SERIALIZED_SIZE | 4,000,000 | consensus/consensus.h:13 |
| MAX_BLOCK_SIGOPS_COST | 80,000 | consensus/consensus.h:18 |
| WITNESS_SCALE_FACTOR | 4 | consensus/consensus.h:25 |
| nPowTargetSpacing | 15 seconds | kernel/chainparams.cpp:107 |
| multiAlgoDiffChangeTarget | 145,000 | kernel/chainparams.cpp:122 |
| alwaysUpdateDiffChangeTarget | 400,000 | kernel/chainparams.cpp:123 |
| workComputationChangeTarget | 1,430,000 | kernel/chainparams.cpp:124 |
| OdoHeight | 9,112,320 | kernel/chainparams.cpp:126 |
| DD_TX_VERSION | 0x0D1D0770 | primitives/transaction.h:47 |
| OP_DIGIDOLLAR..OP_ORACLE | 0xbb..0xbf | script/script.h:210-214 |
| DigiDollarHeight (buried deployment, v9.26.5; historically BIP9 bit 23) | mainnet 23,869,440; testnet26 600; signet/regtest 0 | kernel/chainparams.cpp |
| nDigiDollarMuSig2Height | equals effective DigiDollar activation boundary (mainnet 23,627,520; testnet26 600; default regtest 0) | kernel/chainparams.cpp |

### Verified Algorithm Implementations

| Algorithm | Enum Value | Hash Function |
|-----------|------------|---------------|
| SHA256D | 0 | GetHash() |
| Scrypt | 1 | scrypt_1024_1_1_256() |
| Groestl | 2 | HashGroestl() |
| Skein | 3 | HashSkein() |
| Qubit | 4 | HashQubit() |
| Odocrypt | 7 | HashOdo() |

### DigiDollar Verification

| Feature | Implementation |
|---------|---------------|
| 10 Collateral Tiers | consensus/digidollar.h |
| DCA Multipliers (1.0x-2.0x) | consensus/dca.cpp |
| ERR Ratios (0.80-0.95) | consensus/err.cpp |
| DD Address Prefixes | base58.cpp |
| Transaction Types (0-3) | primitives/transaction.h:38-44 |
| Confirmed-only DD transfers | validation.cpp / wallet/digidollarwallet.cpp (commit 0b4959f563) |

### Oracle System Verification

| Feature | Implementation |
|---------|---------------|
| COraclePriceMessage | primitives/oracle.h:31+ |
| COracleBundle v0x03 (MuSig2 only on V1) | primitives/oracle.h, oracle/bundle_manager.cpp (commits f2bb0a19a4, bbb85cf363) |
| MuSig2 Aggregator/Session/Orchestrator | oracle/musig2_*.{cpp,h} |
| IQR Outlier Filtering | oracle/bundle_manager.cpp:CalculateConsensusPrice |
| Block-Extracted Oracle Price | validation.cpp:ConnectBlock (around lines 3084-3098) |
| Incremental DD Supply Tracking | digidollar/health.cpp:OnMintConnected/OnRedeemConnected |
| Mining graceful degradation for DD txs | node/miner.cpp (commit 6b5ff516c3) |

### Cross-Reference Documents

This document is consistent with:
- **DIGIDOLLAR_ARCHITECTURE.md** - Complete stablecoin implementation details
- **DIGIDOLLAR_ORACLE_ARCHITECTURE.md** - Complete oracle system specification
- **REPO_MAP.md** - Granular core-DigiByte file index
- **REPO_MAP_DIGIDOLLAR.md** - Granular DigiDollar/oracle file index
- **DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md** - Approved Paymaster V1 protocol and acceptance criteria
- **doc/digidollar-paymaster.md** - Client and provider operation guide

---

*Document Version: 2.2*
*Generated from DigiByte Core v9.26.5 codebase analysis (`feature/digidollar-paymaster-v1`)*
*Updated: 2026-07-28*
