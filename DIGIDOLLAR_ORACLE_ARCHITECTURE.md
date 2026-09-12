# DigiDollar Oracle System - Complete Architecture Documentation
**DigiByte v9.26.2 — DigiDollar V1 Oracle Network (MuSig2-only)**
*Updated: 2026-04-30*
*Document Version: 8.0 — Re-validated against `feature/digidollar-v1` branch*

> **V1 invariants (enforced by code on every network):**
> - Mainnet and testnet validate identically. The previous mainnet short-circuit in `OracleDataValidator::ValidateBlockOracleData` was removed (commit `f0d9a7b2c7`); both networks honor the same Phase 3 gates.
> - Only MuSig2 v0x03 oracle bundles are accepted on-chain (commits `bbb85cf363`, `fa29405adc`, `f2bb0a19a4`). Raw v0x01/v0x02 OP_RETURN payloads short-circuit inside `OracleBundleManager::ExtractOracleBundle`, so `OracleDataValidator::ValidateBlockOracleData` emits `bad-oracle-malformed`. The `bad-oracle-legacy` branch only fires when extraction succeeds with a non-MuSig2 version, which is structurally unreachable for current v0x03 wire payloads — it is kept as a defense-in-depth gate.
> - DD mint/redeem blocks must include exactly one valid v0x03 bundle in the coinbase, or they are rejected with `bad-oracle-missing` / `bad-oracle-malformed` / `bad-oracle-multiple-outputs`. Transfer-only and non-DD blocks may omit oracle data; if any block includes oracle data, it must still be valid v0x03.
> - `OP_CHECKPRICE` is reserved and deterministically disabled (`src/script/interpreter.cpp:708-735`). It consumes one operand and pushes false rather than reading node-local oracle state.
> - `nDigiDollarMuSig2Height` is aligned with the effective DigiDollar activation boundary. MuSig2 is the only on-chain bundle format from the moment DigiDollar/oracle consensus activates.

> **Sections that survived from earlier doc revisions (Phase One single-oracle, Phase Two roadmap, "MAINNET DISABLED" warnings, the Section 14 roadmap) describe a code path that no longer exists.** Treat the V1 invariants above as authoritative; flagged sections are kept only for historical context.

---

## Table of Contents

- [Executive Summary](#1-executive-summary)
- [Quick Start Guide](#2-quick-start-guide)
- [System Architecture Overview](#3-system-architecture-overview)
- [Data Structures & Serialization](#4-data-structures--serialization)
- [Block Validation & Consensus Rules](#5-block-validation--consensus-rules)
- [Current Oracle Configuration](#14-current-oracle-configuration)

For deployment and diagnostics, see the [oracle setup guide](DIGIDOLLAR_ORACLE_SETUP.md)
and [node operations](doc/digidollar-operations.md).

---

# Part I: Executive Summary & Overview

## 1. Executive Summary

### 1.1 What is the Oracle System?

The Oracle System provides **decentralized price feeds** for the DigiByte blockchain, enabling DigiDollar's collateralized stablecoin functionality. Operator nodes aggregate DGB/USD prices from six exchanges, attest to a median, and run a MuSig2 round to produce a single 64-byte BIP-340 Schnorr signature. The miner embeds that aggregate signature plus a participation bitmap into the coinbase as a v0x03 OP_ORACLE bundle. Every full node validates the bundle on `CheckBlock`, and the median price flows into a height-keyed cache that DigiDollar minting/redemption logic consults.

**Real-World Analogy:** A multisignature appraiser cooperative — no single appraiser can move the price, and the chain only accepts an appraisal that at least 7 configured active members signed.

### 1.2 V1 Design Philosophy

**Core principle: cryptographic threshold consensus, on-chain compact, off-chain signed.**

V1 ships with:
- **Threshold MuSig2 quorum**: 7 signatures from the configured 35-active mainnet/testnet keyset, 4-of-7 regtest. The aggregate signature is verified by every full node against the on-chain participation bitmap.
- **One on-chain format**: v0x03 (`bitmap_len + bitmap + epoch + price + timestamp + 64-byte aggregate sig`). Legacy v0x01 (single-message compact) and v0x02 (multi-message with per-oracle sigs) are explicitly rejected at extraction and validation time.
- **Six working exchange fetchers** (Binance, KuCoin, Gate.io, HTX, Crypto.com, CoinGecko — see `src/oracle/exchange.cpp:1092-1097`). The classes for Coinbase, Kraken, Messari, Bittrex, Poloniex still compile but are **not** initialized into `MultiExchangeAggregator::fetchers`. CoinMarketCap was removed entirely. The current fetch loop is sequential, then filtered by a 10% median-deviation outlier rule.
- **Block-cadence validation** aligned with DigiByte's 15-second block target;
  operator exchange fetch/broadcast loops run on the code's 60-second timer.

**Trade-offs (still relevant in V1):**
- ✅ Compact on-chain footprint (86-byte minimum v0x03 data; 90 bytes on the 35-slot mainnet/testnet bitmap)
- ✅ Constant-size signature whether 7 or all 35 active oracles participate
- ✅ Same validator code on mainnet and testnet
- ⚠️ MuSig2 requires two interactive rounds (nonce + partial sig) per epoch
- ⚠️ Quorum failure means the next price-dependent DD mint/redeem block must wait — mining graceful degradation strips those txs and continues; DD transfer-only and non-DD blocks are unaffected (commits `6b5ff516c3`, `1e08bd811f`).

### 1.3 Implementation Status

**V1 surface — code as shipped on `feature/digidollar-v1`:**

- ✅ OP_ORACLE opcode (0xbf) wired through script flag `SCRIPT_VERIFY_DIGIDOLLAR`
- ✅ MuSig2 v0x03 on-chain format only — `OracleBundleManager::CreateOracleScript` produces v0x03 (`src/oracle/bundle_manager.cpp:889-925`); `ExtractOracleBundle` rejects v0x01/v0x02 (`src/oracle/bundle_manager.cpp:1065-1068`)
- ✅ 35 active slots (35 active oracle slots) with a 7-signature mainnet/testnet quorum
- ✅ Single validator path on mainnet and testnet (`OracleDataValidator::ValidateBlockOracleData`, `src/oracle/bundle_manager.cpp:2151`) — the prior mainnet short-circuit is gone
- ✅ P2P message surface: `oracleprice`, `oraclebundle` (received-and-dropped), `oracleconsns`, `oracleattest`, `oramusnonce`, `oramusigctx`, `oramusigpsig`, `oraclehb`, `getoracles` (`src/protocol.cpp:53-62`, handlers in `src/net_processing.cpp` 5440–6340). All oracle P2P handlers, including `oraclehb`, share the `IsOracleP2PActive` gate.
- ✅ Six initialized exchange fetchers (`src/oracle/exchange.cpp:1092-1097`)
- ✅ Block-validated price cache, gated by the buried `DEPLOYMENT_DIGIDOLLAR` deployment (BIP90 since v9.26.5) (`src/validation.cpp:3064-3094, 3365-3372`)
- ✅ BIP-340 Schnorr verification at every relay hop, with bound-from-chainparams pubkey replacement before verification (`src/net_processing.cpp:5462-5491`) so an attacker cannot ship their own pubkey alongside a forged signature

**Removed / never-shipped:**
- `sendoracleprice` RPC — removed for fake-price-injection (commit history); replaced by signed P2P attestations sourced from the local exchange aggregator
- v0x01 (Phase One single-oracle compact) on-chain format — accepted nowhere
- v0x02 (Phase Two multi-message-with-per-oracle-sigs) on-chain format — accepted nowhere
- Legacy `oraclebundle` payload — handler still exists for backward wire compatibility but unconditionally drops the message

**Test coverage** (counts approximate — see `REPO_MAP_DIGIDOLLAR.md` for the up-to-date file lists):
- C++ unit tests: ~150 across `src/test/digidollar_*`, `src/test/oracle_*`, `src/test/musig2_*`, plus the `rh*` Red Hornet suites
- Fuzz harnesses: `src/test/fuzz/oracle_*`, `src/test/fuzz/oracle_musig2_*`, and the MuSig2 P2P targets named `musig2_*`
- Functional tests: current DD/oracle coverage is registered in
  `test/functional/test_runner.py` under `digidollar_*` and
  `wallet_digidollar_*`; `feature_oracle_p2p.py` is a legacy/superseded
  scaffold and the live P2P proof is `digidollar_wave20_oracle_p2p.py`.

---

## 2. Quick Start Guide

### 2.1 For Users: What Does This Mean?

**If you're minting DigiDollars:**
- The oracle tells the blockchain how much your DGB collateral is worth
- You need 200% collateral (e.g., $200 of DGB to mint $100 DigiDollar)
- Oracle operators fetch and broadcast fresh exchange prices every 60 seconds; chainparams separately control how often block-level oracle prices are accepted per network

**If you're running a node:**
- Your node validates the MuSig2 aggregate signature in every DD mint/redeem block once `DEPLOYMENT_DIGIDOLLAR` is active (buried height since v9.26.5) and `nHeight >= nOracleActivationHeight`; DD transfer-only and ordinary DGB blocks can omit a coinbase oracle bundle
- No setup needed — validation happens automatically; the trust anchor is `consensus.vOraclePublicKeys` in chainparams
- Enable `-debug=digidollar` temporarily for routine oracle diagnostics. Useful errors and startup/recovery progress remain visible without it. See [node operations](doc/digidollar-operations.md) for log retention and compact filter service.

### 2.2 For Developers: Integration Points

```cpp
// 1. Bundle manager price cache (height-keyed)
//    Used by DD mint/redeem validation after an authenticated coinbase
//    oracle bundle has been accepted for the block.
OracleBundleManager& manager = OracleBundleManager::GetInstance();
CAmount price_micro_usd  = manager.GetLatestPrice();
CAmount historical_price = manager.GetOraclePriceForHeight(block_height);

// 2. Quorum / activation queries
bool quorum  = OracleBundleManager::HasMuSig2Quorum(bundle, params);
bool enabled = Consensus::IsOracleActive(params, height);
```

### 2.3 Key Files Quick Reference

```
Core implementation:
├── src/script/script.h                    OP_ORACLE definition (0xbf), DigiDollar opcode family
├── src/primitives/oracle.{h,cpp}          COraclePriceMessage, COracleBundle (with v0x03 fields:
│                                          aggregate_sig, participation_bitmap), OracleNodeInfo,
│                                          ORACLE_TOTAL_COUNT/ACTIVE_COUNT/CONSENSUS_REQUIRED constants
├── src/oracle/bundle_manager.{h,cpp}      Bundle assembly, ExtractOracleBundle (rejects v0x01/v0x02),
│                                          ValidateBlockOracleData, ValidateMuSig2Bundle, price cache
├── src/oracle/exchange.{h,cpp}            6 active fetchers (Binance, KuCoin, Gate.io, HTX,
│                                          Crypto.com, CoinGecko); MultiExchangeAggregator
├── src/oracle/mock_oracle.{h,cpp}         Regtest helper; `OP_CHECKPRICE` is reserved and
│                                          deterministically disabled, so it never reads mock state
├── src/oracle/musig2_aggregator.{h,cpp}   secp256k1 MuSig2 key aggregation + bitmap encode/decode
├── src/oracle/musig2_session.{h,cpp}      Per-epoch session (state machine, nonce + partial-sig)
├── src/oracle/musig2_session_manager.{h,cpp}  Per-epoch session lifecycle (create/lookup/prune)
├── src/oracle/musig2_orchestrator.{h,cpp}     Inter-session coordination
├── src/oracle/musig2_oracle_participation.{h,cpp}  Per-oracle participation tracking
├── src/oracle/musig2_messages.h           Wire formats for nonce / context / partial-sig messages
├── src/oracle/musig2_session_mining.h     Helpers for miner template path
├── src/oracle/signing_orchestrator.{h,cpp}    CValidationInterface; drives MuSig2 round 1/2
├── src/oracle/node.{h,cpp}                Oracle daemon entry, lifecycle
├── src/script/interpreter.{h,cpp}         Reserved/disabled OP_CHECKPRICE behavior
├── src/validation.cpp                     ConnectBlock → ValidateBlockOracleData (right after
│                                          CheckBlock); ConnectBlock price cache; UpdatePriceCache
│                                          gated on buried DEPLOYMENT_DIGIDOLLAR (v9.26.5)
├── src/net_processing.cpp                 P2P handlers (~5440–6340), including heartbeat, are gated
│                                          by IsOracleP2PActive, rate-limited, roster-limited, and
│                                          verified with chainparams pubkey replacement before
│                                          signature verification
├── src/rpc/digidollar.cpp                 18 node-context RPCs; sendoracleprice REMOVED
├── src/wallet/rpc/wallet.cpp              17 wallet-context DD/oracle RPCs (createoraclekey,
│                                          exportoracleprivkey, importoracleprivkey, startoracle,
│                                          mintdigidollar, etc.)
└── src/kernel/chainparams.cpp             vOracleNodes (35 mainnet/testnet active slots,
                                           7 regtest), vOraclePublicKeys
                                           (35 active mainnet/testnet, 7 regtest), nDDActivationHeight,
                                           nOracleActivationHeight, nDigiDollarMuSig2Height, buried
                                           DigiDollarHeight (v9.26.5)

Tests (current; counts in REPO_MAP_DIGIDOLLAR.md):
├── src/test/digidollar_*_tests.cpp        DD validation, mint, redeem, transfer, P2P, persistence,…
├── src/test/oracle_*_tests.cpp            Oracle-specific (block validation, bundle, exchange, RPC,…)
├── src/test/musig2_*_tests.cpp            MuSig2 aggregator/session/orchestrator/participation
├── src/test/rh*_tests.cpp                 Red Hornet (regression suite)
├── src/test/fuzz/oracle_*.cpp             Fuzz harnesses for oracle wire formats
├── src/test/fuzz/oracle_musig2_*.cpp      Fuzz harnesses for MuSig2 message handling
└── test/functional/digidollar_*.py + wallet_digidollar_*.py + feature_oracle_p2p.py
```

---

## 3. System Architecture Overview

### 3.1 The Complete Data Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    ORACLE SYSTEM: COMPLETE DATA FLOW                     │
└─────────────────────────────────────────────────────────────────────────┘

PHASE 1: PRICE DISCOVERY (Every 60 seconds)
═══════════════════════════════════════════

Exchange APIs (6 initialized exchanges, sequential fetch loop - 5 defined but not initialized):
┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│  Binance    │  │   KuCoin    │  │  Gate.io    │  │    HTX      │
│ DGB/USDT    │  │  DGB/USDT   │  │ DGB_USDT    │  │ dgbusdt     │
│ $0.05023    │  │  $0.05017   │  │  $0.05021   │  │  $0.05019   │
└──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘
       └─────────────────┴────────────┬────────────────────┘
                                      │
      ┌─────────────┐  ┌─────────────┐
      │ Crypto.com  │  │  CoinGecko  │
      │  DGB/USD    │  │ (aggregator)│
      │ $0.05020    │  │  $0.05022   │
      └──────┬──────┘  └──────┬──────┘
             └────────────────┘
                                      │
                                      ▼
                    MultiExchangeAggregator
                    ┌─────────────────────────────┐
                    │ 1. Filter failures          │
                    │ 2. Remove outliers          │
                    │ 3. Calculate median         │
                    │ 4. Convert to micro-USD     │
                    └──────────────┬──────────────┘
                                   │
                            Median: $0.05020
                              (50,200 micro-USD)

PHASE 2: MESSAGE CREATION & SIGNING
═══════════════════════════════════

Oracle Node (Authorized, oracle_id=0)
├─► Create COraclePriceMessage:
│     oracle_id:        0
│     price_micro_usd:  50200
│     timestamp:        1732204800
│     block_height:     700
│     nonce:            0x123456789ABCDEF0
│     oracle_pubkey:    XOnlyPubKey (32 bytes)
│
├─► Sign with Schnorr (BIP-340):
│     msg_hash = SHA256d(oracle_id || price || timestamp || ...)
│     schnorr_sig = Sign(msg_hash, oracle_privkey)  [64 bytes]
│
└─► Full Format Message: 128 bytes
      (Used for P2P transmission, NOT stored on-chain)


PHASE 3: MUSIG2 SESSION LIFECYCLE
══════════════════════════════════

Off-chain relay
├─► oracleprice: signed price attestations enter pending_messages
├─► oracleconsns / oracleattest: consensus price/timestamp agreement
├─► oramusnonce: round-1 public nonces
├─► oramusigctx: context proposal freezes participant IDs, nonce set,
│                quote set, consensus price/timestamp, and session ID
├─► oramusigpsig: round-2 partial signatures bound to that context
└─► completed session:
      aggregate_sig: 64-byte BIP-340 signature
      participation_bitmap: variable-length bitmap over active signer IDs
      epoch: GetCurrentEpoch(block_height)


PHASE 4: V0X03 FORMAT ENCODING
═══════════════════════════════

CreateOracleScript(bundle) -> OP_RETURN OP_ORACLE <0x03> <v03_data>

┌────────────────────────────────────────────────────────────┐
│  version:             0x03                                │
│  bitmap_len:          1 byte                              │
│  participation_bitmap bitmap_len bytes                    │
│  epoch:               4 bytes little-endian               │
│  price:               8 bytes little-endian, micro-USD    │
│  timestamp:           8 bytes little-endian               │
│  aggregate_sig:       64 bytes                            │
└────────────────────────────────────────────────────────────┘

Total v03 data: 86-byte minimum, 90 bytes on the 35-slot
mainnet/testnet roster. Raw v0x01/v0x02 payloads are rejected at
extraction and do not update the price cache.


PHASE 5: BLOCK INCLUSION
═════════════════════════

Miner (BlockAssembler::CreateNewBlock)
├─► If the template contains DD mint/redeem:
│     - query completed MuSig2 sessions first
│     - fall back to cached current-epoch bundle
│     - CreateOracleScript(bundle) -> v0x03 OP_ORACLE
│     - add one coinbase oracle output
├─► If no valid bundle is available:
│     - remove/skip price-dependent DD mint/redeem txs
│     - keep ordinary DGB and valid DD transfer-only txs flowing
└─► Transfer-only and non-DD templates may omit OP_ORACLE.


PHASE 6: BLOCK VALIDATION
══════════════════════════

CheckBlock/ConnectBlock
├─► OracleDataValidator::ValidateBlockOracleData()
│     │
│     ├─► Pre-activation historical state: return true
│     ├─► Count all coinbase OP_RETURN OP_ORACLE markers
│     ├─► Reject more than one marker: bad-oracle-multiple-outputs
│     ├─► If no marker:
│     │     - DD mint/redeem -> bad-oracle-missing
│     │     - DD transfer-only / non-DD -> accepted
│     ├─► Extract v0x03 only; raw v0x01/v0x02 -> bad-oracle-malformed
│     ├─► Validate bitmap, signer IDs, quorum, epoch, price range, and
│     │   aggregate BIP-340 signature over the chain-bound bundle hash
│     └─► Enforce timestamp window: <=1 hour old and <=60 seconds future
│
└─► Valid connected block updates the height-keyed price cache.


PHASE 7: PRICE CACHE UPDATE
════════════════════════════

ConnectBlock(block, state, pindex)  [validation.cpp:~2805]
├─► ExtractOracleBundle(coinbase_tx, bundle)
│     - Parse compact format from OP_RETURN
│     - Reconstruct COracleBundle
│
├─► UpdatePriceCache(height, median_price)
│     - height_to_price[700] = 50200
│     - Keep last 1000 blocks in cache
│     - Thread-safe (mutex-protected)
│
└─► DigiDollar Access:
      OracleIntegration::GetCurrentOraclePrice()
      → Returns: 50200 micro-USD ($0.05020/DGB)


PHASE 8: P2P BROADCASTING (Parallel to Block Inclusion)
═══════════════════════════════════════════════════════

OracleBundleManager::BroadcastMessage(message)  [Full 128-byte format]
├─► Validate message.IsValid() ✓
├─► AddOracleMessage() to local storage ✓
├─► CConnman::ForEachNode([&](CNode* node) {
│       m_connman->PushMessage(node,
│         CNetMsgMaker(version).Make(NetMsgType::ORACLEPRICE, message));
│     });
│
└─► Message propagates to all peers (~2-5 seconds for 95th percentile)

Receiving Node:
├─► ProcessMessage(NetMsgType::ORACLEPRICE)
│     - Rate limit: 3600 msg/hour per peer ✓
│     - Deserialize message ✓
│     - Validate structure, timestamp, signature ✓
│     - Check duplicate (hash-based) ✓
│     - AddOracleMessage() to local storage ✓
│     - Relay to other peers (except sender) ✓
│
└─► Message stored, ready for bundle creation
```

### 3.2 Component Interaction Map

```
┌──────────────────────────────────────────────────────────────────┐
│                    COMPONENT RELATIONSHIPS                        │
└──────────────────────────────────────────────────────────────────┘

External World:
  ┌─────────────────────────────────────────────────────────┐
  │    Exchange APIs (6 WORKING + 5 BROKEN/REMOVED)          │
  │  ✅ Binance • KuCoin • Gate.io • HTX • Crypto.com       │
  │  ✅ CoinGecko                                            │
  │  ❌ Coinbase • Kraken • Messari • Bittrex/Poloniex      │
  │  ❌ CoinMarketCap (removed - paid API)                  │
  └──────────────────┬──────────────────────────────────────┘
                     │ HTTP/HTTPS (libcurl)
                     ▼
Core Oracle Layer:
  ┌──────────────────────────────────────────┐
  │   MultiExchangeAggregator                │
  │   - FetchAllPrices() [sequential]        │
  │   - FilterOutliers() [10% threshold]     │
  │   - CalculateMedianPrice()               │
  └──────────────────┬───────────────────────┘
                     │ Median price (micro-USD)
                     ▼
  ┌──────────────────────────────────────────┐
  │   OracleNode (if running oracle)         │
  │   - FetchMedianPrice()                   │
  │   - CreatePriceMessage()                 │
  │   - Sign(oracle_privkey) [Schnorr]      │
  └──────────────────┬───────────────────────┘
                     │ COraclePriceMessage (128B)
                     ▼
Bundle Management:
  ┌──────────────────────────────────────────┐
  │   OracleBundleManager (Singleton)        │
  │   ├─ AddOracleMessage()                  │
  │   ├─ completed MuSig2 session lookup     │
  │   ├─ CreateOracleScript() [v0x03]        │
  │   ├─ ExtractOracleBundle()               │
  │   ├─ BroadcastMessage() ◄──┐             │
  │   └─ UpdatePriceCache()     │             │
  └──────────────┬────────────┬─┘             │
                 │            │               │
        ┌────────┘            └───────┐       │
        │                             │       │
        ▼                             ▼       │
P2P Layer:                   Block Layer:     │
┌─────────────────┐         ┌──────────────────────┐
│   CConnman      │         │  BlockAssembler      │
│   - ForEachNode │         │  - CreateNewBlock()  │
│   - PushMessage │         │  - AddOracleBundle   │
└────────┬────────┘         └──────────┬───────────┘
         │                              │
         │ oracleprice/oraclehb/MuSig2 │ Coinbase OP_ORACLE
         │                              │
         ▼                              ▼
┌──────────────────┐         ┌──────────────────────┐
│  P2P Network     │         │   CBlock             │
│  (All peers)     │         │   - vtx[0] coinbase  │
└──────────────────┘         │   - merkle root      │
                             └──────────┬───────────┘
                                        │
                                        ▼
Validation Layer:              CheckBlock(block, state)
┌──────────────────────────────────────────────────────┐
│  OracleDataValidator::ValidateBlockOracleData()      │
│  ├─ ExtractOracleBundle() from coinbase             │
│  ├─ Validate structure, consensus, timestamp         │
│  ├─ Check oracle authorization (chainparams)         │
│  └─ Accept/Reject block                              │
└──────────────────┬───────────────────────────────────┘
                   │ Valid block
                   ▼
         ConnectBlock(block, pindex)
┌──────────────────────────────────────────────────────┐
│  UpdatePriceCache(height, price)                     │
│  ├─ height_to_price[700] = 50200                     │
│  ├─ Live OracleBundleManager cache                  │
│  └─ Log: "Oracle: Updated price cache at height 700" │
└──────────────────┬───────────────────────────────────┘
                   │
                   ▼
DigiDollar Integration:
┌──────────────────────────────────────────────────────┐
│  OracleIntegration::GetCurrentOraclePrice()          │
│  └─ Returns cached price for collateral calculation  │
│                                                       │
│  DigiDollarManager::MintDigiDollar()                 │
│  ├─ Get DGB/USD from oracle                          │
│  ├─ Calculate required collateral (200%)             │
│  └─ Create mint transaction                          │
└──────────────────────────────────────────────────────┘
```

---

> **Reading note for Part II.** The deep-dive that follows is preserved from earlier revisions and frequently uses Phase One / Phase Two phrasing, describes a 1-of-1 single-oracle bundle, and shows v0x01 / v0x02 byte layouts. Those formats are still mentioned in the data-structure types but they are **not** accepted on-chain in V1 — `OracleBundleManager::ExtractOracleBundle` rejects them by returning false. The validator (`OracleDataValidator::ValidateBlockOracleData`) then emits `bad-oracle-malformed` for raw v0x01/v0x02 wire scripts. The `bad-oracle-legacy` branch is kept as defense-in-depth but is structurally unreachable from on-wire OP_RETURN payloads in current code. Where the deep-dive describes the 22-byte compact OP_ORACLE layout or "Phase One consensus exactly 1 message", read it as background on the wire-format primitives, not on the actual V1 acceptance rules. Authoritative V1 behavior is summarized in Section 1, Section 5.2 (V1 flow), and Section 14 below; everything else in Part II is historical context.

# Part II: Core Components (Deep Dive)

## 4. Data Structures & Serialization

### 4.1 COraclePriceMessage - Complete Specification

**Location**: `/home/jared/Code/digibyte/src/primitives/oracle.h` (lines 31-107)

#### 4.1.1 Field-by-Field Breakdown

```cpp
class COraclePriceMessage
{
public:
    uint32_t oracle_id{0};                      // 4 bytes  - Oracle identifier
    uint64_t price_micro_usd{0};                // 8 bytes  - Price in micro-USD
    int64_t timestamp{0};                       // 8 bytes  - Unix timestamp
    int32_t block_height{0};                    // 4 bytes  - Block height at creation
    uint64_t nonce{0};                          // 8 bytes  - Random nonce
    XOnlyPubKey oracle_pubkey;                  // 32 bytes - BIP-340 Schnorr pubkey
    std::vector<unsigned char> schnorr_sig;     // 64 bytes - BIP-340 Schnorr signature

    // Total: 128 bytes (full format)
};
```

**Field Details:**

| Field | Size | Type | Range/Constraint | Purpose |
|-------|------|------|------------------|---------|
| **oracle_id** | 4 bytes | uint32_t | 0-34 (`< ORACLE_TOTAL_COUNT` = 35) | Identifies oracle node |
| **price_micro_usd** | 8 bytes | uint64_t | 100 - 100,000,000 ($0.0001-$100.00) | DGB price in micro-USD (1,000,000 = $1.00) |
| **timestamp** | 8 bytes | int64_t | Unix timestamp, ≤1 hour old | Message creation time |
| **block_height** | 4 bytes | int32_t | Current chain height | Context for message |
| **nonce** | 8 bytes | uint64_t | Random value | Ensures hash uniqueness |
| **oracle_pubkey** | 32 bytes | XOnlyPubKey | Valid secp256k1 x-coordinate | BIP-340 pubkey |
| **schnorr_sig** | 64 bytes | vector<uchar> | Valid BIP-340 signature | Message authentication |

#### 4.1.2 Micro-USD Price Format (Detailed)

**CRITICAL: Actual Price Format in Code**
- **Field name**: `price_micro_usd`
- **Actual format**: **Micro-USD** where `1,000,000 micro-USD = $1.00 USD`
- **NOT cents**: The code does NOT use 100 = $1.00 format for oracle prices

**Definition**: `1,000,000 micro-USD = $1.00 USD`

**Why Micro-USD?**
1. **Precision**: 6 decimal places (sufficient for extremely small DGB prices)
2. **Integer Arithmetic**: No floating-point rounding errors
3. **Future-Proof**: Handles prices from $0.000001 to $100+ per DGB
4. **Standard**: Aligns with common financial data precision

**Conversion Examples:**
```
Price (USD/DGB)  →  Micro-USD        →  Hex (LE)
$0.0001          →  100              →  0x6400000000000000
$0.001           →  1,000            →  0xE803000000000000
$0.01            →  10,000           →  0x1027000000000000
$0.0065          →  6,500            →  0x6419000000000000  (realistic DGB price)
$0.05            →  50,000           →  0x50C3000000000000
$1.00            →  1,000,000        →  0x40420F0000000000
$10.00           →  10,000,000       →  0x8096980000000000
$100.00          →  100,000,000      →  0x00E1F50500000000
```

**Validation Constraints** (`IsValid()` implementation at oracle.cpp:35-36):
```cpp
static constexpr uint64_t MIN_PRICE_MICRO_USD = 100;        // $0.0001 per DGB (minimum)
static constexpr uint64_t MAX_PRICE_MICRO_USD = 100000000;  // $100.00 per DGB (maximum)

if (price_micro_usd < MIN_PRICE_MICRO_USD) return false;
if (price_micro_usd > MAX_PRICE_MICRO_USD) return false;
```

**Rationale**:
- **Lower bound ($0.0001)**: Prevents oracle spam with near-zero prices
- **Upper bound ($100.00)**: Reasonable max for DGB; prevents data corruption bugs

**Mock Oracle Default Price**:
- Default: `6500 micro-USD = $0.0065/DGB` (realistic DGB price)

#### 4.1.3 XOnlyPubKey (BIP-340 Schnorr)

**Implementation**: `/home/jared/Code/digibyte/src/pubkey.h` (lines 230-300)

```cpp
class XOnlyPubKey
{
private:
    uint256 m_keydata;  // 32 bytes - x-coordinate only

public:
    // Construct from CPubKey (extracts x-coordinate)
    explicit XOnlyPubKey(const CPubKey& pubkey);

    // Construct from 32-byte span
    explicit XOnlyPubKey(Span<const unsigned char> bytes);

    // BIP-340 Schnorr signature verification
    bool VerifySchnorr(const uint256& msg, Span<const unsigned char> sigbytes) const;

    // Serialization (no length prefix, fixed 32 bytes)
    SERIALIZE_METHODS(XOnlyPubKey, obj) { READWRITE(obj.m_keydata); }
};
```

**Key Properties**:
- **Size**: 32 bytes (vs 33 for compressed CPubKey)
- **Format**: x-coordinate only (y-coordinate parity implicit)
- **Validity**: Only ~50% of 32-byte arrays are valid secp256k1 points
- **BIP-340**: Deterministic, non-malleable Schnorr signatures

**Example**:
```
CPubKey (33 bytes):     03 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798
                        ↑  └──────────────────────────────────────────┬────────────────┘
                     prefix                                       x-coordinate
                                                                       ↓
XOnlyPubKey (32 bytes):    79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798
```

#### 4.1.4 Schnorr Signature (BIP-340)

**Format**: 64 bytes (no DER encoding, fixed length)
- **Bytes 0-31**: r component (x-coordinate of R point)
- **Bytes 32-63**: s component (scalar)

**Signature Creation** (`COraclePriceMessage::Sign()`):

```cpp
bool COraclePriceMessage::Sign(const CKey& key,
                                const uint256* merkle_root,
                                const uint256& aux)
{
    // 1. Get message hash (excludes signature and pubkey)
    uint256 hash = GetSignatureHash();

    // 2. Create 64-byte Schnorr signature
    schnorr_sig.resize(64);
    if (!key.SignSchnorr(hash, schnorr_sig, merkle_root, aux)) {
        schnorr_sig.clear();
        return false;
    }

    // 3. Set oracle pubkey from private key
    oracle_pubkey = XOnlyPubKey(key.GetPubKey());

    return true;
}
```

**Signature Hash Computation**:
```cpp
uint256 COraclePriceMessage::GetSignatureHash() const
{
    // Hash all fields EXCEPT signature and pubkey
    CHashWriter ss(0);
    ss << oracle_id;           // 4 bytes
    ss << price_micro_usd;     // 8 bytes
    ss << timestamp;           // 8 bytes
    ss << block_height;        // 4 bytes
    ss << nonce;               // 8 bytes

    return ss.GetHash();  // SHA256d(data)
}
```

**Why exclude signature/pubkey from hash?**
- Prevents circular dependency (can't sign a hash that includes the signature)
- Nonce ensures uniqueness even with identical price/timestamp

#### 4.1.5 Serialization (Full Format)

**SERIALIZE_METHODS Implementation**:
```cpp
SERIALIZE_METHODS(COraclePriceMessage, obj) {
    READWRITE(obj.oracle_id);        // CompactSize + 4 bytes
    READWRITE(obj.price_micro_usd);  // CompactSize + 8 bytes
    READWRITE(obj.timestamp);        // CompactSize + 8 bytes
    READWRITE(obj.block_height);     // CompactSize + 4 bytes
    READWRITE(obj.nonce);            // CompactSize + 8 bytes
    READWRITE(obj.oracle_pubkey);    // 32 bytes (no prefix)
    READWRITE(obj.schnorr_sig);      // CompactSize + 64 bytes
}
```

**On-Wire Size**: ~133 bytes (128 data + ~5 CompactSize prefixes)

**P2P Message Structure**:
```
┌──────────────────────────────────────────────────────┐
│ Bitcoin P2P Header (24 bytes)                        │
├──────────────────────────────────────────────────────┤
│ Magic:        0xDAB5BFFA (DigiByte mainnet)         │
│ Command:      "oracleprice\0\0\0" (12 bytes)        │
│ Payload Size: 133 (4 bytes)                         │
│ Checksum:     <4 bytes>                             │
├──────────────────────────────────────────────────────┤
│ COraclePriceMessage Payload (133 bytes)             │
├──────────────────────────────────────────────────────┤
│ TOTAL: 157 bytes                                     │
└──────────────────────────────────────────────────────┘
```

#### 4.1.6 Validation Rules (`IsValid()`)

**Complete Validation Logic** (src/primitives/oracle.cpp:30-49):

```cpp
bool COraclePriceMessage::IsValid(int64_t reference_time) const
{
    // 1. PRICE RANGE VALIDATION
    // Uses shared constants from oracle.h: ORACLE_MIN/MAX_PRICE_MICRO_USD
    if (price_micro_usd < ORACLE_MIN_PRICE_MICRO_USD) return false;  // 100 ($0.0001)
    if (price_micro_usd > ORACLE_MAX_PRICE_MICRO_USD) return false;  // 100000000 ($100.00)

    // 2. TIMESTAMP VALIDATION
    // Use provided reference time (block time during validation) or current time
    int64_t current_time = (reference_time > 0) ? reference_time : GetTime();

    // Not in future (1 minute tolerance for clock skew)
    if (timestamp > current_time + 60) return false;

    // Not too old (1 hour maximum age)
    if (timestamp < current_time - ORACLE_MAX_AGE_SECONDS) return false;

    // 3. SIGNATURE VERIFICATION (mandatory — fail closed)
    // V1 does not accept unsigned compact oracle messages: an empty or invalid
    // schnorr_sig makes VerifyAttestation() return false, so IsValid() returns false.
    return VerifyAttestation();
}
```

**Validation Summary**:

| Check | Constraint | Rejection Behavior |
|-------|-----------|-------------------|
| Price minimum | ≥ 100 micro-USD ($0.0001) | `return false` |
| Price maximum | ≤ 100,000,000 micro-USD ($100.00) | `return false` (uses ORACLE_MAX_PRICE_MICRO_USD) |
| Future timestamp | ≤ now + 60s | `return false` |
| Old timestamp | ≥ now - 3600s | `return false` |
| Signature | Valid BIP-340 attestation, always required | `return false` if `VerifyAttestation()` fails (empty sig fails closed) |

> **Note**: V1 fails closed on signatures — `IsValid()` ends in `return VerifyAttestation();`,
> so an empty or invalid `schnorr_sig` is rejected. The earlier empty-signature bypass is gone.

### 4.2 COracleBundle - Complete Specification

**Location**: `/home/jared/Code/digibyte/src/primitives/oracle.h` (lines 113-168)

#### 4.2.1 Bundle Structure

```cpp
class COracleBundle
{
public:
    std::vector<COraclePriceMessage> messages;  // bounded by reserved slot capacity
    int32_t epoch{0};                           // Epoch identifier
    uint64_t median_price_micro_usd{0};         // Consensus price
    int64_t timestamp{0};                       // Bundle creation time

    // Phase One: messages.size() == 1 (1-of-1 consensus)
    // MuSig2: messages.size() >= nOracleRequiredMessages
};
```

#### 4.2.2 Median Price Calculation

**Algorithm** (src/primitives/oracle.cpp):

```cpp
uint64_t COracleBundle::GetConsensusPrice(int min_required) const
{
    if (!HasConsensus(min_required)) return 0;

    // Step 1: Price-range filter only (deterministic, time-independent)
    std::vector<int64_t> prices;
    for (const auto& msg : messages) {
        if (msg.price_micro_usd >= ORACLE_MIN_PRICE_MICRO_USD &&
            msg.price_micro_usd <= ORACLE_MAX_PRICE_MICRO_USD) {
            prices.push_back(static_cast<int64_t>(msg.price_micro_usd));
        }
    }
    if (prices.empty()) return 0;

    // Step 2: Sort for IQR calculation
    std::sort(prices.begin(), prices.end());

    // Step 3: If less than 4 prices, return simple median (no IQR filtering)
    if (prices.size() < 4) {
        size_t mid = prices.size() / 2;
        if (prices.size() % 2 == 0) {
            return static_cast<uint64_t>((prices[mid - 1] + prices[mid]) / 2);
        }
        return static_cast<uint64_t>(prices[mid]);
    }

    // Step 4: Apply IQR outlier filtering (1.5 * IQR rule)
    size_t q1_idx = prices.size() / 4;
    size_t q3_idx = (prices.size() * 3) / 4;
    int64_t q1 = prices[q1_idx];
    int64_t q3 = prices[q3_idx];
    int64_t iqr = q3 - q1;
    int64_t lower_bound = q1 - (iqr * 3 / 2);
    int64_t upper_bound = q3 + (iqr * 3 / 2);

    // Step 5: Filter outliers and return median of filtered set
    std::vector<int64_t> filtered;
    for (int64_t price : prices) {
        if (price >= lower_bound && price <= upper_bound) {
            filtered.push_back(price);
        }
    }

    // Step 6: Fall back to unfiltered median if all filtered
    if (filtered.empty()) filtered = prices;

    std::sort(filtered.begin(), filtered.end());
    size_t mid = filtered.size() / 2;
    if (filtered.size() % 2 == 0) {
        return static_cast<uint64_t>((filtered[mid - 1] + filtered[mid]) / 2);
    }
    return static_cast<uint64_t>(filtered[mid]);
}
```

**Examples**:

```
Odd Count (7 prices):
Input:  [990000, 1000000, 1010000, 1020000, 1030000]
Sorted: [990000, 1000000, 1010000, 1020000, 1030000]
                              ↑
                        Middle (index 2)
Median: 1,010,000 micro-USD = $1.01

Even Count (8 prices):
Input:  [995000, 1000000, 1005000, 1010000]
Sorted: [995000, 1000000, 1005000, 1010000]
                      ↑        ↑
                Middle two (indices 1,2)
Average: (1000000 + 1005000) / 2 = 1,002,500 micro-USD = $1.0025

Phase One (single price):
Input:  [1234567]
Median: 1,234,567 micro-USD = $1.234567
```

#### 4.2.3 Epoch System

**Epoch Calculation**:
```cpp
int32_t GetCurrentEpoch(int32_t block_height) {
    const Consensus::Params& params = Params().GetConsensus();
    int32_t epoch_length = params.nDDOracleEpochBlocks;

    return block_height / epoch_length;
}
```

**Network-Specific Lengths**:
```
Mainnet:   40 blocks (~10 minutes at 15s/block)
Testnet:   40 blocks (~10 minutes)
Regtest:   40 blocks (~10 minutes)
```

**Epoch Timeline Example (Testnet)**:
```
Epoch  0: Blocks    0 -   39
Epoch  1: Blocks   40 -   79
Epoch  2: Blocks   80 -  119
...
Epoch 15: Blocks  600 -  639 (testnet DD/oracle activation height)
```

### 4.3 Historical Compact Format Encoding (Not V1 Acceptance)

**Location**: `/home/jared/Code/digibyte/src/oracle/bundle_manager.cpp` (lines 896-1048)

This subsection is retained as historical context for the old v0x01 compact
layout. Current V1 block acceptance is MuSig2-only v0x03:
`OP_RETURN OP_ORACLE <0x03> <bitmap_len> <bitmap> <epoch> <price> <timestamp> <64-byte aggregate_sig>`.
Raw v0x01/v0x02 payloads return false from `ExtractOracleBundle()` and
surface as `bad-oracle-malformed`.

#### 4.3.0 OP_ORACLE: Complete System Flow

```
┌────────────────────────────────────────────────────────────────────┐
│  OP_ORACLE ARCHITECTURE: Technical Implementation Flow            │
└────────────────────────────────────────────────────────────────────┘

PHASE 1: MESSAGE CREATION (External Oracle Daemon)
════════════════════════════════════════════════════════════════════
┌───────────────────────────────────────────────────────────────┐
│ Oracle Daemon (oracle.digibyte.io)                           │
├───────────────────────────────────────────────────────────────┤
│ 1. Fetch prices from 6 working exchanges (5 broken/removed)       │
│ 2. Calculate median with percentage-threshold outlier filtering│
│ 3. Create COraclePriceMessage structure:                     │
│    ┌─────────────────────────────────────────────────────┐  │
│    │ struct COraclePriceMessage {                        │  │
│    │   uint32_t oracle_id;        // 0 (Phase One)      │  │
│    │   uint64_t price_micro_usd;  // Micro-USD (1M = $1) │  │
│    │   int64_t  timestamp;        // Unix time          │  │
│    │   uint32_t block_height;     // Current height     │  │
│    │   uint64_t nonce;            // Random nonce       │  │
│    │   CPubKey  oracle_pubkey;    // 32-byte pubkey     │  │
│    │   std::vector<uint8_t> schnorr_sig; // 64 bytes   │  │
│    │ };                                                  │  │
│    │ Total: 128 bytes                                    │  │
│    └─────────────────────────────────────────────────────┘  │
│ 4. Sign message with BIP-340 Schnorr signature               │
│ 5. Broadcast via P2P: NetMsgType::ORACLEPRICE                │
└───────────────────────────────────────────────────────────────┘
                              │
                              ▼
                     P2P Network Relay
                              │
                              ▼
PHASE 2: P2P VALIDATION (All Nodes - net_processing.cpp)
════════════════════════════════════════════════════════════════════
┌───────────────────────────────────────────────────────────────┐
│ ProcessMessage(NetMsgType::ORACLEPRICE)                      │
├───────────────────────────────────────────────────────────────┤
│ Validation Steps:                                             │
│ ✓ Deserialize 128-byte COraclePriceMessage                   │
│ ✓ msg.IsValid() → Structural validation                      │
│ ✓ msg.Verify() → BIP-340 Schnorr signature verification      │
│ ✓ oracle_id < 35? (ORACLE_TOTAL_COUNT range check)           │
│ ✓ Timestamp fresh? (age < 1 hour, not > 1 min future)        │
│ ✓ Rate limit: max 3600 messages/hour from this peer           │
│ ✓ Duplicate check: msg.GetHash() not in seen_messages        │
│                                                               │
│ If VALID:                                                     │
│   → OracleBundleManager::AddOracleMessage(msg)               │
│   → RelayOracleMessage(msg, exclude_peer_id)                 │
│                                                               │
│ If INVALID:                                                   │
│   → Misbehavior(peer, 2-20 points depending on violation)    │
│   → Drop message (rate limit: silently dropped, no penalty)  │
└───────────────────────────────────────────────────────────────┘
                              │
                              ▼
PHASE 3: BLOCK INCLUSION (Miner - bundle_manager.cpp:896-1048)
════════════════════════════════════════════════════════════════════
┌───────────────────────────────────────────────────────────────┐
│ Miner calls CreateOracleScript()                              │
├───────────────────────────────────────────────────────────────┤
│ Input:  COracleBundle with 1 message (from P2P)              │
│ Output: CScript with compact 22-byte OP_ORACLE data          │
│                                                               │
│ Encoding Process:                                             │
│ 1. script << OP_RETURN << OP_ORACLE;  // 2 bytes             │
│ 2. script << std::vector<uchar>{0x01}; // Version byte       │
│ 3. Create compact data (17 bytes):                           │
│    ┌────────────────────────────────────────────┐            │
│    │ oracle_id (1) + price (8) + timestamp (8) │            │
│    │          = 17 bytes total                  │            │
│    └────────────────────────────────────────────┘            │
│ 4. script << compact_data; // Push 17 bytes                  │
│                                                               │
│ Coinbase Transaction Structure:                              │
│   vout[0]: 72,000 DGB → Miner reward                         │
│   vout[1]: 0 DGB → OP_RETURN OP_ORACLE <22-byte data>  ◄──┐  │
│   vout[2]: 0 DGB → Witness commitment                    │  │
│                                                          │  │
│ Final scriptPubKey (22 bytes):                          │  │
│   6a bf 01 01 11 00 [price 8B] [timestamp 8B]          │  │
│   │  │  │  │  │  │                                      │  │
│   │  │  │  │  │  └─ Oracle ID                          │  │
│   │  │  │  │  └──── PUSH 17 bytes                      │  │
│   │  │  │  └─────── Version                            │  │
│   │  │  └────────── PUSH 1 byte                        │  │
│   │  └───────────── OP_ORACLE (0xbf) ◄─────────────────┘  │
│   └──────────────── OP_RETURN (0x6a)                       │
└───────────────────────────────────────────────────────────────┘
                              │
                              ▼
PHASE 4: BLOCK VALIDATION (All Nodes - V1 validator path)
════════════════════════════════════════════════════════════════════
┌───────────────────────────────────────────────────────────────┐
│ CheckBlock() calls ValidateBlockOracleData()                  │
├───────────────────────────────────────────────────────────────┤
│ 1. Network Parity:                                            │
│    mainnet, testnet, and regtest share the V1 validator;       │
│    there is no mainnet oracle-validation bypass.              │
│                                                               │
│ 2. Activation Height:                                         │
│    if (block_height < nDDActivationHeight)                     │
│        skip oracle validation; // Not active yet              │
│                                                               │
│ 3. Find OP_ORACLE in coinbase:                                │
│    ┌───────────────────────────────────────────────┐         │
│    │ for vout in coinbase.vout:                    │         │
│    │   if vout.scriptPubKey[0] == 0x6a:  // OP_RETURN        │
│    │     if vout.scriptPubKey[1] == 0xbf:  // OP_ORACLE      │
│    │       found = true;                            │         │
│    └───────────────────────────────────────────────┘         │
│                                                               │
│ 4. Extract Compact Data (bundle_manager.cpp:1050-1200):      │
│    ┌───────────────────────────────────────────────┐         │
│    │ Parse 22-byte scriptPubKey:                   │         │
│    │ - Byte 3: version (must be 0x01)             │         │
│    │ - Byte 5: oracle_id (must be 0x00)           │         │
│    │ - Bytes 6-13: price (LE uint64)              │         │
│    │ - Bytes 14-21: timestamp (LE int64)          │         │
│    └───────────────────────────────────────────────┘         │
│                                                               │
│ 5. Validate Bundle:                                           │
│    ✓ version == 0x01                                         │
│    ✓ oracle_id == 0                                          │
│    ✓ price in range [100, 100M] micro-USD                    │
│    ✓ timestamp < block.nTime + 60                            │
│    ✓ timestamp > block.nTime - 3600                          │
│    ✓ messages.size() == 1 (Phase One)                        │
│    ✓ GetOracleNode(oracle_id)->is_active == true             │
│                                                               │
│ 6. Result:                                                    │
│    if (all_checks_pass)                                       │
│        ACCEPT block;                                          │
│    else                                                       │
│        REJECT block; // Invalid oracle data                  │
└───────────────────────────────────────────────────────────────┘
                              │
                              ▼
PHASE 5: PRICE CACHE (ConnectBlock - validation.cpp:~2805)
════════════════════════════════════════════════════════════════════
┌───────────────────────────────────────────────────────────────┐
│ OracleBundleManager::UpdatePriceCache()                       │
├───────────────────────────────────────────────────────────────┤
│ Extract oracle price from connected block:                    │
│   COracleBundle bundle;                                       │
│   ExtractOracleBundle(coinbase_tx, bundle);                   │
│                                                               │
│ Update in-memory cache:                                       │
│   ┌─────────────────────────────────────────┐                │
│   │ std::map<int, uint64_t> height_to_price│                │
│   ├─────────────────────────────────────────┤                │
│   │ [695] → 6400 micro-USD ($0.0064)       │                │
│   │ [696] → 6450 micro-USD ($0.00645)      │                │
│   │ [697] → 6500 micro-USD ($0.0065)       │                │
│   │ [698] → 6550 micro-USD ($0.00655)      │                │
│   │ [699] → 6500 micro-USD ($0.0065)       │                │
│   │ [700] → 6500 micro-USD ($0.0065) ◄ NEW │                │
│   └─────────────────────────────────────────┘                │
│                                                               │
│ Cache management:                                             │
│   - Keep last 1,000 blocks                                    │
│   - Thread-safe (std::mutex)                                  │
│   - Auto-evict oldest entries                                 │
└───────────────────────────────────────────────────────────────┘
                              │
                              ▼
PHASE 6: DIGIDOLLAR USAGE (Minting/Redemption)
════════════════════════════════════════════════════════════════════
┌───────────────────────────────────────────────────────────────┐
│ DigiDollar Minting Process                                    │
├───────────────────────────────────────────────────────────────┤
│ User calls: mintdigidollar(amount, lock_tier)                 │
│                                                               │
│ Query oracle price:                                           │
│   uint64_t price = OracleBundleManager::GetInstance()         │
│                      .GetLatestPrice();                       │
│   // Returns: 6500 micro-USD = $0.0065 per DGB                │
│                                                               │
│ Calculate collateral:                                         │
│   ┌─────────────────────────────────────────────────┐        │
│   │ Mint 10000 DD cents ($100)                     │        │
│   │ Collateral ratio: 300%                         │        │
│   │ Oracle price: 6500 micro-USD ($0.0065/DGB)     │        │
│   │                                                 │        │
│   │ Required DGB:                                   │        │
│   │   ($100 × 3) ÷ $0.0065/DGB = 46,153 DGB        │        │
│   │                                                 │        │
│   │ Formula:                                        │        │
│   │   collateral_sats = (dd_cents × COIN × ratio × │        │
│   │                     100) / oracle_micro_usd     │        │
│   │                   = (10000 × 100000000 × 300 ×  │        │
│   │                     100) / 6500                 │        │
│   │                   ≈ 4,615,384,615,385 sats      │        │
│   └─────────────────────────────────────────────────┘        │
└───────────────────────────────────────────────────────────────┘
```

#### 4.3.1 Byte-by-Byte Format Specification

```
┌────────────────────────────────────────────────────────────────────┐
│  OP_ORACLE (0xbf): Custom Opcode for Oracle Data                  │
└────────────────────────────────────────────────────────────────────┘

OPCODE DEFINITION
═══════════════════════════════════════════════════════════════════
File: src/script/script.h:214

OP_ORACLE = 0xbf  // BIP-342 OP_SUCCESSx slot reused for oracle price data

Context in DigiDollar Opcode Family (Tapscript OP_SUCCESSx slots, gated by SCRIPT_VERIFY_DIGIDOLLAR):
┌──────────────────────────────────────────────────────────────────────┐
│ OP_DIGIDOLLAR      = 0xbb  (OP_SUCCESSx) - DD output marker         │
│ OP_DDVERIFY        = 0xbc  (OP_SUCCESSx) - DD verification          │
│ OP_CHECKPRICE      = 0xbd  (OP_SUCCESSx) - Price checking           │
│ OP_CHECKCOLLATERAL = 0xbe  (OP_SUCCESSx) - Collateral check         │
│ OP_ORACLE          = 0xbf  (OP_SUCCESSx) - Oracle data ◄───┐        │
└──────────────────────────────────────────────────────────────────────┘


WHY A CUSTOM OPCODE?
═══════════════════════════════════════════════════════════════════
✓ Fast Detection: Nodes instantly recognize oracle data
✓ Efficient Parsing: Skip non-oracle OP_RETURNs without parsing
✓ Type Safety: Compiler enforces correct opcode usage
✓ Extensibility: Future versions (0x02, 0x03) possible
✓ Self-Documenting: Code intent is crystal clear


DETECTION ALGORITHM
═══════════════════════════════════════════════════════════════════
Pseudocode (validation.cpp):

for (const auto& tx : block.vtx) {
    if (!tx.IsCoinBase()) continue;

    for (const auto& out : tx.vout) {
        const CScript& script = out.scriptPubKey;

        // Check for OP_RETURN
        if (script.size() < 2) continue;
        if (script[0] != OP_RETURN) continue;  // 0x6a

        // Check for OP_ORACLE ◄─── KEY CHECK
        if (script[1] == OP_ORACLE) {  // 0xbf
            // Found oracle data! Parse it.
            ExtractOracleBundle(tx, bundle);
            break;
        }
    }
}


COMPACT FORMAT STRUCTURE (22 bytes total)
═══════════════════════════════════════════════════════════════════

Phase One Compact Format:

┌─────┬─────┬─────┬──────────────┬──────────────┬──────────────┐
│ Pos │ Len │ Type│ Name         │ Value        │ Description  │
├─────┼─────┼─────┼──────────────┼──────────────┼──────────────┤
│ 0   │ 1   │ OP  │ OP_RETURN    │ 0x6a         │ Unspendable  │
│ 1   │ 1   │ OP  │ OP_ORACLE    │ 0xbf         │ Oracle marker│
│ 2   │ 1   │ OP  │ PUSHDATA     │ 0x01         │ Push 1 byte  │
│ 3   │ 1   │ u8  │ Version      │ 0x01         │ Phase One    │
│ 4   │ 1   │ OP  │ PUSHDATA     │ 0x11 (17)    │ Push 17 bytes│
│ 5   │ 1   │ u8  │ Oracle ID    │ 0x00         │ Oracle 0     │
│ 6-13│ 8   │ u64 │ Price        │ LE uint64    │ Micro-USD    │
│14-21│ 8   │ i64 │ Timestamp    │ LE int64     │ Unix time    │
└─────┴─────┴─────┴──────────────┴──────────────┴──────────────┘

Total: 22 bytes (within 83-byte MAX_OP_RETURN_RELAY limit) ✅


EXAMPLE: Real OP_ORACLE Output
═══════════════════════════════════════════════════════════════════
Hex Dump (22 bytes):
6a bf 01 01 11 00 64 19 00 00 00 00 00 00 00 2f 50 65 00 00 00 00

Parsed:
┌──────┬──────┬──────────────────────────────────────────────┐
│ Pos  │ Hex  │ Meaning                                      │
├──────┼──────┼──────────────────────────────────────────────┤
│ 0    │ 6a   │ OP_RETURN: Output is unspendable            │
│ 1    │ bf   │ OP_ORACLE: This is oracle data ✓            │
│ 2    │ 01   │ PUSH 1: Next 1 byte is data                 │
│ 3    │ 01   │ Version 1 (Phase One format)                │
│ 4    │ 11   │ PUSH 17: Next 17 bytes are data             │
│ 5    │ 00   │ Oracle ID = 0                               │
│ 6-13 │ 6419 │ Price = 0x0000000000001964 (LE)             │
│      │ 0000 │       = 6500 micro-USD = $0.0065/DGB        │
│      │ 0000 │                                              │
│      │ 0000 │                                              │
│14-21 │ 002f │ Timestamp = 0x0000000065502f00 (LE)         │
│      │ 5065 │           = 1,700,000,000                   │
│      │ 0000 │           = Nov 14, 2023 22:13:20 UTC       │
│      │ 0000 │                                              │
└──────┴──────┴──────────────────────────────────────────────┘


SPACE EFFICIENCY ANALYSIS
═══════════════════════════════════════════════════════════════════
Full P2P Format:        128 bytes (includes signature)
Compact Block Format:    22 bytes (no signature needed)
Savings per block:      106 bytes (82.8% reduction)

Annual savings (5,760 blocks/day × 365 days):
  Full format:    269 MB/year
  Compact format:  46 MB/year
  Savings:        223 MB/year ✓

10-year savings: 2.23 GB ✓
```

#### 4.3.2 Encoding Implementation

```cpp
CScript OracleBundleManager::CreateOracleScript(const COracleBundle& bundle) const
{
    // Phase Three (v0x03): MuSig2 aggregate signature + participation bitmap
    if (bundle.version == 3) {
        // ... MuSig2 handling (omitted for brevity) ...
    }

    if (bundle.messages.empty()) {
        return CScript(); // Empty script for no oracle data
    }

    // Phase Two: multi-message bundles (version 0x02) when Phase Two is active
    if (bundle.messages.size() > 1) {
        // Only create multi-oracle scripts when Phase Two is enabled
        // If Phase Two not activated, reject multi-message bundles
        // Phase Two format: OP_RETURN OP_ORACLE <0x02> <data>
        // Data: num_messages(1) + price(8) + timestamp(8) + per-oracle: id(1) + sig(64)
        // ... (see bundle_manager.cpp for full implementation)
    }

    // Phase One: Must have exactly 1 message (1-of-1 consensus)
    CScript script;
    script << OP_RETURN << OP_ORACLE;

    // Version byte (0x01 = Phase One compact format)
    script << std::vector<unsigned char>{0x01};

    // Compact data: oracle_id (1) + price (8) + timestamp (8) = 17 bytes
    const COraclePriceMessage& msg = bundle.messages[0];
    std::vector<unsigned char> compact_data;
    compact_data.reserve(17);

    // Oracle ID (uint8 for Phase One)
    // SECURITY (DGB-SEC-004): Defense-in-depth — reject oracle_id > 255
    compact_data.push_back(static_cast<unsigned char>(msg.oracle_id & 0xFF));

    // Price in micro-USD (uint64, little-endian)
    uint64_t price = msg.price_micro_usd;
    for (int i = 0; i < 8; ++i) {
        compact_data.push_back(static_cast<unsigned char>((price >> (i * 8)) & 0xFF));
    }

    // Timestamp (int64, little-endian)
    int64_t timestamp = msg.timestamp;
    for (int i = 0; i < 8; ++i) {
        compact_data.push_back(static_cast<unsigned char>((timestamp >> (i * 8)) & 0xFF));
    }

    script << compact_data;
    return script;
}
```

#### 4.3.3 Concrete Encoding Example

**Input Message**:
```
oracle_id:        0
price_micro_usd:  6500 micro-USD (= $0.0065/DGB)
timestamp:        1700000000 (0x65502F00 in hex)
```

**Output Script (hex)**:
```
6a          - OP_RETURN
bf          - OP_ORACLE
01          - PUSH 1 byte (version follows)
01          - Version (0x01 = Phase One)
11          - PUSH 17 bytes (compact data follows)
00          - Oracle ID (0)
64 19 00 00 00 00 00 00  - Price (6500 micro-USD = $0.0065/DGB in little-endian)
00 2f 50 65 00 00 00 00  - Timestamp (1,700,000,000 in little-endian)

Total: 22 bytes (2 opcodes + 2 push headers + 18 data bytes)
```

**Verification (Little-Endian)**:
```
Price bytes: 64 19 00 00 00 00 00 00
  Value: 6500 micro-USD = $0.0065 per DGB ✓

Timestamp bytes: 00 2f 50 65 00 00 00 00
  Step 1: 0x00                          = 0
  Step 2: 0x2f << 8                     = 12,032
  Step 3: 0x50 << 16                    = 5,242,880
  Step 4: 0x65 << 24                    = 1,694,498,816
  Total:  0 + 12,032 + 5,242,880 + 1,694,498,816 = 1,699,753,728 (example value)
```

#### 4.3.4 Decoding Implementation

```cpp
bool OracleBundleManager::ExtractOracleBundle(
    const CTransaction& coinbase_tx,
    COracleBundle& bundle) const
{
    // Look for OP_RETURN output with OP_ORACLE marker
    for (const auto& output : coinbase_tx.vout) {
        if (output.scriptPubKey.size() > 2 &&
            output.scriptPubKey[0] == OP_RETURN &&
            output.scriptPubKey[1] == OP_ORACLE) {

            // Extract data chunks
            std::vector<unsigned char> data;
            auto script_it = output.scriptPubKey.begin() + 2; // Skip OP_RETURN + OP_ORACLE

            while (script_it < output.scriptPubKey.end()) {
                if (*script_it <= 75) { // OP_PUSHDATA1 range
                    unsigned char chunk_size = *script_it;
                    ++script_it;

                    if (script_it + chunk_size <= output.scriptPubKey.end()) {
                        data.insert(data.end(), script_it, script_it + chunk_size);
                        script_it += chunk_size;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }

            // Check version byte
            if (data[0] == 0x01) {
                // Phase One compact format
                if (data.size() < 18) {
                    return false;
                }

                COraclePriceMessage msg;

                // Parse oracle_id (uint8)
                msg.oracle_id = data[1];

                // Parse price (uint64, little-endian)
                uint64_t price = 0;
                for (int i = 0; i < 8; ++i) {
                    price |= (static_cast<uint64_t>(data[2 + i]) << (i * 8));
                }
                msg.price_micro_usd = price;

                // Parse timestamp (int64, little-endian)
                int64_t timestamp = 0;
                for (int i = 0; i < 8; ++i) {
                    timestamp |= (static_cast<int64_t>(data[10 + i]) << (i * 8));
                }
                msg.timestamp = timestamp;

                // Set remaining fields (not in compact format)
                msg.block_height = 0; // Not needed for Phase One
                msg.nonce = 0;

                // Phase One: Get oracle pubkey from chainparams
                const CChainParams& chainparams = Params();
                const OracleNodeInfo* oracle_info = chainparams.GetOracleNode(msg.oracle_id);
                if (oracle_info) {
                    msg.oracle_pubkey = XOnlyPubKey(oracle_info->pubkey);
                }

                // Create bundle with single message
                bundle.messages.clear();
                bundle.messages.push_back(msg);
                bundle.median_price_micro_usd = price;
                bundle.timestamp = timestamp;
                bundle.epoch = GetCurrentEpoch(msg.block_height);

                return true;
            }

            return false;
        }
    }

    return false;
}
```

#### 4.3.5 Size Optimization Analysis

**Comparison: Full Format vs Compact Format**

```
FULL FORMAT (P2P Transmission):
┌──────────────────────┬──────────┐
│ Field                │ Size     │
├──────────────────────┼──────────┤
│ oracle_id            │  4 bytes │
│ price_micro_usd      │  8 bytes │  (1,000,000 = $1.00)
│ timestamp            │  8 bytes │
│ block_height         │  4 bytes │
│ nonce                │  8 bytes │
│ oracle_pubkey        │ 32 bytes │
│ schnorr_sig          │ 64 bytes │
├──────────────────────┼──────────┤
│ TOTAL                │ 128 bytes│
└──────────────────────┴──────────┘

COMPACT FORMAT (Blockchain Storage):
┌──────────────────────┬──────────┐
│ Field                │ Size     │
├──────────────────────┼──────────┤
│ OP_RETURN            │  1 byte  │
│ OP_ORACLE            │  1 byte  │
│ OP_PUSHDATA          │  1 byte  │
│ Version              │  1 byte  │
│ oracle_id            │  1 byte  │
│ price_micro_usd      │  8 bytes │  (1,000,000 = $1.00)
│ timestamp            │  8 bytes │
├──────────────────────┼──────────┤
│ TOTAL                │ 22 bytes │
└──────────────────────┴──────────┘

SAVINGS: 106 bytes per message (82.8% reduction)

Phase Two / Phase 3 MuSig2 (35 reserved slots, RC41):
- Full format:    17 × 128 = 2,176 bytes
- Compact format: ~170 bytes (version/opcodes shared, 17 × price+timestamp)
- MuSig2 v0x03:   90 bytes on the 35-slot mainnet/testnet roster (one aggregate signature)
- Savings: up to ~2,088 bytes (>95% reduction with MuSig2)
```

---

## 5. Block Validation & Consensus Rules

### 5.1 ConnectBlock() Oracle Validation

**Location**: `/home/jared/Code/digibyte/src/validation.cpp` (ConnectBlock calls ValidateBlockOracleData at line 2852, immediately after the CheckBlock call at line 2842)

**Integration Point (line 2852)**:
```cpp
// Validate oracle data (if present and after activation)
if (!OracleDataValidator::ValidateBlockOracleData(block, pindex->pprev, params.GetConsensus(), state)) {
    return error("%s: OracleDataValidator::ValidateBlockOracleData: %s", __func__, state.ToString());
}
```

**Execution Position**:

```
Chainstate::ConnectBlock() Validation Sequence:
├─► CheckBlock(block, state, ...)            [line 2842]
│     (context-free PoW / merkle / size / coinbase / sigop checks)
│
└─► ★ ORACLE VALIDATION ★                    [line 2852]
      OracleDataValidator::ValidateBlockOracleData(block, pindex->pprev, ...)
      followed by CheckMuSig2OracleBundleVersion(block, pindex->pprev, ...) [line 2856]
```

### 5.2 ValidateBlockOracleData() — V1 Flow

**Location:** `src/oracle/bundle_manager.cpp:2151` (`OracleDataValidator::ValidateBlockOracleData`).

The validator runs identically on mainnet, testnet, and regtest. The pseudocode below is updated to match V1 — the prior "Phase One" / "Phase Two" multi-branch description and the mainnet short-circuit have been removed from the code (commits `f0d9a7b2c7`, `bbb85cf363`, `fa29405adc`, `f2bb0a19a4`).

```cpp
bool OracleDataValidator::ValidateBlockOracleData(
    const CBlock& block,
    const CBlockIndex* pindex_prev,
    const Consensus::Params& params,
    BlockValidationState& state)
{
    if (block.vtx.empty()) return true;
    const CTransaction& coinbase = *block.vtx[0];

    // 1. Determine height (pindex_prev->nHeight+1, or BIP34 from coinbase scriptSig).
    int32_t block_height = /* ... */;

    // 2. Activation / height gate (buried deployment since v9.26.5). Pre-activation: skip oracle validation entirely.
    if (pindex_prev) {
        if (!DigiDollar::IsDigiDollarEnabled(pindex_prev, params)) return true;
    } else if (block_height < params.nDDActivationHeight) {
        return true;
    }

    // 3. Scan ALL coinbase outputs for OP_RETURN OP_ORACLE markers.
    int oracle_output_count = /* count */;
    if (oracle_output_count > 1)
        return state.Invalid(..., "bad-oracle-multiple-outputs", ...);

    // 4. DD mint/redeem blocks must include a bundle; transfer-only and
    //    non-DD blocks may omit it.
    if (oracle_output_count == 0) {
        if (BlockNeedsOraclePrice(block))
            return state.Invalid(..., "bad-oracle-missing", ...);
        return true;
    }

    // 5. Extract. ExtractOracleBundle returns false for raw v0x01/v0x02
    //    OP_RETURN payloads and for
    //    every other malformed shape, so this branch emits bad-oracle-malformed
    //    in both cases. The bad-oracle-legacy branch below is defense-in-depth
    //    for a hypothetical future shape that parses successfully but is not
    //    MuSig2; with v0x03 returning true immediately after parsing, current
    //    wire payloads cannot reach it.
    COracleBundle bundle;
    if (!OracleBundleManager::GetInstance().ExtractOracleBundle(coinbase, bundle))
        return state.Invalid(..., "bad-oracle-malformed", ...);
    if (!bundle.IsMuSig2())
        return state.Invalid(..., "bad-oracle-legacy", ...);

    // 6. Validate v0x03: bitmap parses, ≥ nOracleConsensusRequired participants,
    //    bitmap members ∈ [0, nOraclePubkeyCount), aggregate pubkey computed via
    //    MuSig2OracleAggregator, BIP-340 Schnorr verify of aggregate_sig over
    //    ComputeOracleBundleHash(bundle).
    std::string err;
    if (!OracleBundleManager::ValidateMuSig2Bundle(bundle, block_height, params, err))
        return state.Invalid(..., "bad-oracle-musig2", err);

    // 7. Timestamp window: not older than ORACLE_MAX_AGE_SECONDS, not more than 60 s
    //    in the future relative to block.nTime.
    if ((block.nTime - bundle.timestamp) > ORACLE_MAX_AGE_SECONDS)
        return state.Invalid(..., "bad-oracle-timestamp", ...);
    if (bundle.timestamp > block.nTime + 60)
        return state.Invalid(..., "bad-oracle-timestamp", ...);

    return true;
}
```

> The remainder of this section (a longer expanded pseudocode block) is kept below for reference, but it describes the **older** Phase One single-message validator and the now-deleted mainnet short-circuit. Treat the snippet above as authoritative.

```cpp
bool OracleDataValidator::ValidateBlockOracleData(
    const CBlock& block,
    const CBlockIndex* pindex_prev,
    const Consensus::Params& params,
    BlockValidationState& state)
{
    //═══════════════════════════════════════════════════════════════════
    // STEP 1: NETWORK PARITY - V1 validator is shared
    //═══════════════════════════════════════════════════════════════════
    // Mainnet, testnet, and regtest run the same V1 MuSig2 bundle checks.
    // Regtest only differs by activation knobs and mock-price helpers that
    // are not production fallbacks.

    //═══════════════════════════════════════════════════════════════════
    // STEP 2: BLOCK HEIGHT DETERMINATION (lines 817-830)
    //═══════════════════════════════════════════════════════════════════
    int32_t block_height = 0;
    if (pindex_prev) {
        // ContextualCheckBlock has access to prev block index
        block_height = pindex_prev->nHeight + 1;
    } else {
        // CheckBlock doesn't have chain context - extract from coinbase (BIP34)
        const CTransaction& coinbase = *block.vtx[0];
        if (!coinbase.vin.empty() && coinbase.vin[0].scriptSig.size() >= 1) {
            CScript::const_iterator pc = coinbase.vin[0].scriptSig.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;
            if (coinbase.vin[0].scriptSig.GetOp(pc, opcode, data) && !data.empty()) {
                block_height = CScriptNum(data, true).getint();
            }
        }
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 3: ACTIVATION CHECK (buried deployment since v9.26.5)
    //═══════════════════════════════════════════════════════════════════
    // Primary: buried deployment check (when pindex_prev available)
    // Fallback: Height-based check (when no chain context)
    if (pindex_prev) {
        if (!DigiDollar::IsDigiDollarEnabled(pindex_prev, params)) {
            return true; // Oracle validation not required before activation
        }
    } else {
        if (block_height < params.nDDActivationHeight) {
            return true;
        }
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 4: SCAN ALL COINBASE OUTPUTS FOR OP_ORACLE (lines 1601-1624)
    //═══════════════════════════════════════════════════════════════════
    const CTransaction& coinbase = *block.vtx[0];

    // Scan ALL coinbase outputs for OP_ORACLE marker (oracle output position
    // varies: may be vout[1] without witness commitment, or vout[2] with it)
    int oracle_output_count = 0;
    for (const auto& output : coinbase.vout) {
        if (output.scriptPubKey.size() >= 2 &&
            output.scriptPubKey[0] == OP_RETURN &&
            output.scriptPubKey[1] == OP_ORACLE) {
            oracle_output_count++;
        }
    }

    // SECURITY: Reject blocks with multiple oracle outputs (prevents confusion attacks)
    if (oracle_output_count > 1) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
            "bad-oracle-multiple-outputs",
            strprintf("Block contains %d oracle outputs, expected at most 1",
                       oracle_output_count));
    }

    if (oracle_output_count == 0) {
        // Allow blocks without oracle data during transition
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: No oracle bundle in block %d (transition period)\n",
                 block_height);
        return true;
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 5: ORACLE BUNDLE EXTRACTION (lines 862-869)
    //═══════════════════════════════════════════════════════════════════
    COracleBundle bundle;
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    if (!manager.ExtractOracleBundle(coinbase, bundle)) {
        // During transition, allow blocks without valid oracle bundles
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Block %d could not extract bundle (transition)\n",
                 block_height);
        return true;
    }

    // CRITICAL: Once extraction succeeds, full validation is MANDATORY

    //═══════════════════════════════════════════════════════════════════
    // STEP 6: BUNDLE STRUCTURE VALIDATION (lines 874-878)
    //═══════════════════════════════════════════════════════════════════
    if (!bundle.IsValid()) {
        LogPrintf("Oracle: Invalid oracle bundle in block %d\n", block_height);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                            "bad-oracle-bundle",
                            "invalid oracle bundle structure");
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 7: PHASE ONE CONSENSUS RULE (1-of-1) (lines 880-885)
    //═══════════════════════════════════════════════════════════════════
    if (bundle.messages.size() != 1) {
        LogPrintf("Oracle: Phase One requires exactly 1 oracle message, got %d\n",
                 bundle.messages.size());
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                            "bad-oracle-consensus",
                            strprintf("Phase One requires exactly 1 oracle message, got %d",
                                     bundle.messages.size()));
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 8: MEDIAN PRICE VERIFICATION (lines 887-894)
    //═══════════════════════════════════════════════════════════════════
    const COraclePriceMessage& msg = bundle.messages[0];
    if (bundle.median_price_micro_usd != msg.price_micro_usd) {
        LogPrintf("Oracle: Median price mismatch: bundle=%llu, message=%llu\n",
                 bundle.median_price_micro_usd, msg.price_micro_usd);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                            "bad-oracle-median",
                            strprintf("Median price mismatch: bundle=%llu, message=%llu",
                                     bundle.median_price_micro_usd, msg.price_micro_usd));
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 9: SCHNORR SIGNATURE VERIFICATION (lines 896-908)
    // ⚠️ SECURITY ISSUE: Empty signature BYPASSES verification
    //═══════════════════════════════════════════════════════════════════
    // Phase One compact format: Trust based on chainparams (no embedded sig)
    if (!msg.schnorr_sig.empty()) {
        // Full format with embedded signature - verify it
        if (!msg.Verify()) {
            LogPrintf("Oracle: Invalid Schnorr signature (oracle_id=%d, block=%d)\n",
                     msg.oracle_id, block_height);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                                "bad-oracle-signature",
                                "Invalid oracle Schnorr signature");
        }
    }
    // ⚠️ WARNING: If schnorr_sig is empty, NO verification occurs!
    // This allows any message without a signature to pass validation.

    //═══════════════════════════════════════════════════════════════════
    // STEP 10: TIMESTAMP AGE VALIDATION (lines 909-916)
    //═══════════════════════════════════════════════════════════════════
    // Verify oracle timestamp is not too old (max 1 hour = 3600 seconds)
    int64_t oracle_age = block.nTime - msg.timestamp;
    if (oracle_age > ORACLE_MAX_AGE_SECONDS) {
        LogPrintf("Oracle: Timestamp too old: age=%d seconds (max=%d) block %d\n",
                 oracle_age, ORACLE_MAX_AGE_SECONDS, block_height);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                            "bad-oracle-timestamp",
                            strprintf("Oracle timestamp too old: age=%d seconds (max=%d)",
                                     oracle_age, ORACLE_MAX_AGE_SECONDS));
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 11: FUTURE TIMESTAMP REJECTION (lines 918-924)
    //═══════════════════════════════════════════════════════════════════
    // Verify oracle timestamp is not in the future (60 second tolerance)
    if (msg.timestamp > block.nTime + 60) {
        LogPrintf("Oracle: Timestamp in future: oracle=%d, block=%d\n",
                 msg.timestamp, block.nTime);
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                            "bad-oracle-timestamp",
                            "Oracle timestamp is in the future");
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 12: ORACLE AUTHORIZATION VERIFICATION (lines 926-935)
    //═══════════════════════════════════════════════════════════════════
    // Verify oracle is authorized (skip in REGTEST for unit testing)
    if (Params().GetChainType() != ChainType::REGTEST) {
        const CChainParams& chainparams = Params();
        const OracleNodeInfo* oracle_config = chainparams.GetOracleNode(msg.oracle_id);
        if (!oracle_config || !oracle_config->is_active) {
            LogPrintf("Oracle: Unauthorized oracle ID %d in block %d\n",
                     msg.oracle_id, block_height);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                                "bad-oracle-unauthorized",
                                strprintf("Unauthorized oracle ID %d", msg.oracle_id));
        }
    }

    //═══════════════════════════════════════════════════════════════════
    // STEP 13: VALIDATION SUCCESS (lines 937-939)
    //═══════════════════════════════════════════════════════════════════
    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Block %d oracle bundle validated: price=%llu micro-USD\n",
             block_height, bundle.median_price_micro_usd);

    return true;
}
```

### 5.3 Consensus Rules Summary Table

| Rule | Specification | Enforcement Point | Rejection Reason | Penalty |
|------|---------------|-------------------|------------------|---------|
| **Activation Gate** | Oracle V1 rules apply only after the deployment predicate says DigiDollar is active | `src/validation.cpp` / `deploymentstatus` | N/A before activation | None before activation |
| **Bundle Count** | DD mint/redeem active blocks require exactly one oracle bundle; DD transfer-only and non-DD blocks do not | `src/validation.cpp` | `bad-oracle-*` | Block rejected |
| **Format** | MuSig2 v0x03 bundle only for V1 | `src/oracle/bundle_manager.cpp` | `bad-oracle-version` / `bad-oracle-format` | Block rejected |
| **MuSig2 Aggregate Signature** | 64-byte aggregate signature over the V1 message domain | `src/oracle/musig2/*` | `bad-oracle-signature` | Block rejected |
| **Roster/Quorum** | Consensus-active signer set, floor 7 signatures | `src/oracle/bundle_manager.cpp`, chainparams | `bad-oracle-quorum` / signer rejection | Block rejected |
| **Timestamp/Epoch** | Bundle epoch and timestamp must be fresh for the block being validated | `src/oracle/bundle_manager.cpp` | `bad-oracle-timestamp` / stale epoch | Block rejected |
| **Non-DD Blocks** | No oracle bundle required for ordinary DGB blocks | `src/validation.cpp` | N/A | Ordinary blocks remain valid |

### 5.4 ConnectBlock() Price-Cache Update

**Location**: `/home/jared/Code/digibyte/src/validation.cpp:~2805` (oracle price extraction in ConnectBlock)

```cpp
bool Chainstate::ConnectBlock(const CBlock& block, BlockValidationState& state,
                               CBlockIndex* pindex, CCoinsViewCache& view, bool fJustCheck)
{
    // ... existing block connection logic ...

    // Extract and cache oracle price (ALL networks)
    if (!fJustCheck && !block.vtx.empty()) {
        if (!block.vtx.empty()) {
            // Use OracleBundleManager's ExtractOracleBundle for proper parsing
            OracleBundleManager& manager = OracleBundleManager::GetInstance();
            COracleBundle bundle;

            if (manager.ExtractOracleBundle(*block.vtx[0], bundle)) {
                // Update oracle price cache for this height
                manager.UpdatePriceCache(pindex->nHeight, bundle.median_price_micro_usd);

                // In RegTest mode, also update MockOracleManager
                if (chain_type == ChainType::REGTEST) {
                    MockOracleManager::GetInstance().SetMockPrice(bundle.median_price_micro_usd);
                }

                LogPrint(BCLog::DIGIDOLLAR,
                        "Oracle: Updated price cache at height %d: %llu micro-USD ($%.6f)\n",
                        pindex->nHeight,
                        bundle.median_price_micro_usd,
                        bundle.median_price_micro_usd / 1000000.0);
            }
        }
    }

    return true;
}
```

**Price Cache Implementation**:
```cpp
// src/oracle/bundle_manager.cpp:2067-2095
void OracleBundleManager::UpdatePriceCache(int height, uint64_t price_micro_usd)
{
    std::lock_guard<std::mutex> lock(mtx_price_cache);
    height_to_price[height] = price_micro_usd;

    // Keep cache size limited (last 1000 blocks)
    if (height_to_price.size() > 1000) {
        height_to_price.erase(height_to_price.begin());
    }

    LogPrint(BCLog::DIGIDOLLAR, "Oracle: Price cache updated for height %d: %llu micro-USD\n",
             height, price_micro_usd);
}
```

**Cache Properties**:
- **Thread-safe**: Protected by `mtx_price_cache` mutex
- **Maximum size**: 1000 entries (last 1000 blocks)
- **Eviction**: FIFO (oldest entries removed first)
- **Data structure**: `std::map<int, uint64_t> height_to_price`

### 5.5 DisconnectBlock() Integration

**Location**: `/home/jared/Code/digibyte/src/validation.cpp:~2460-2485`

```cpp
bool Chainstate::DisconnectBlock(const CBlock& block, const CBlockIndex* pindex,
                                  CCoinsViewCache& view)
{
    // ... existing disconnect logic ...

    // T8-03: Revert oracle price cache for ALL networks (not just testnet/regtest)
    // Mainnet needs deterministic oracle pricing too.
    if (!block.vtx.empty() && block.vtx[0]->vout.size() >= 2) {
        const CTxOut& oracle_output = block.vtx[0]->vout[1];
        if (oracle_output.scriptPubKey.IsUnspendable() &&
            oracle_output.scriptPubKey.size() > 2) {
            // This block had oracle data, need to revert the cache
            OracleBundleManager& manager = OracleBundleManager::GetInstance();
            manager.RemovePriceCache(pindex->nHeight);

            // In RegTest mode, also revert MockOracleManager
            auto chain_type = Params().GetChainType();
            if (chain_type == ChainType::REGTEST && pindex->pprev) {
                // Reset to previous height's price or default
                uint64_t prevPrice = manager.GetOraclePriceForHeight(pindex->pprev->nHeight);
                if (prevPrice > 0) {
                    MockOracleManager::GetInstance().SetMockPrice(prevPrice);
                } else {
                    // Reset to default if no previous price
                    MockOracleManager::GetInstance().Reset();
                }
            }

                LogPrint(BCLog::DIGIDOLLAR,
                        "Oracle: Reverted price cache at height %d during block disconnect\n",
                        pindex->nHeight);
            }
        }
    }

    return true;
}
```

**Cache Removal Implementation**:
```cpp
// src/oracle/bundle_manager.cpp:2110-2133
void OracleBundleManager::RemovePriceCache(int height)
{
    std::lock_guard<std::mutex> lock(mtx_price_cache);
    auto it = height_to_price.find(height);
    if (it != height_to_price.end()) {
        height_to_price.erase(it);
        LogPrint(BCLog::DIGIDOLLAR, "Oracle: Removed price cache for height %d\n", height);
    }
}
```

---

## 14. Current Oracle Configuration

### 14.1 V1 Activation & Quorum Reality (replaces the old Phase Two roadmap)

The "Phase Two roadmap" section that previously occupied this slot is obsolete. Phase 3 / MuSig2 is the only on-chain oracle format in V1, available everywhere from the moment DigiDollar is active (buried deployment since v9.26.5; historically BIP9). The roadmap below has been replaced with the actual configuration the validator uses today.

**Code-validated configuration** (`src/kernel/chainparams.cpp`, `src/consensus/params.h`):

```cpp
// Default in src/consensus/params.h:
int nDDActivationHeight{0};
int nOracleActivationHeight{std::numeric_limits<int>::max()};
int nDigiDollarMuSig2Height{std::numeric_limits<int>::max()};

// Mainnet override (src/kernel/chainparams.cpp):
consensus.nDDActivationHeight        = 23627520;                       // historical BIP9 floor; buried DigiDollarHeight = 23869440 (v9.26.5)
consensus.nOracleActivationHeight    = consensus.nDDActivationHeight;  // 23627520
consensus.nDigiDollarMuSig2Height    = consensus.nDDActivationHeight;
consensus.nOracleRequiredMessages    = 7;     // off-chain quorum input to MuSig2
consensus.nOracleTotalOracles        = 35;
consensus.nOraclePubkeyCount         = 35;
consensus.nOracleConsensusRequired   = 7;     // on-chain MuSig2 threshold

// Testnet override:
consensus.nDDActivationHeight        = 600;
consensus.nOracleActivationHeight    = 600;
consensus.nDigiDollarMuSig2Height    = consensus.nDDActivationHeight;
consensus.nOracleTotalOracles        = 35;
consensus.nOraclePubkeyCount         = 35;
consensus.nOracleConsensusRequired   = 7;

// Regtest override (chainparams.cpp:1112-1119):
consensus.nDDActivationHeight        = 650;
consensus.nOracleActivationHeight    = 650;
consensus.nDigiDollarMuSig2Height    = 0; // min(650, buried DigiDollarHeight=0) — v9.26.5 burial
consensus.nOraclePubkeyCount         = 7;
consensus.nOracleConsensusRequired   = 4;
```

### 14.2 Network-Specific Configuration

| Network | On-chain quorum | Oracles configured | `nDDActivationHeight` | `nOracleActivationHeight` | `nDigiDollarMuSig2Height` |
|---------|-----------------|--------------------|-----------------------|---------------------------|---------------------------|
| Mainnet | 7 signatures from active keyset | 35 in `vOracleNodes` and `vOraclePublicKeys` (slots 0-34 active) | 23 627 520 | 23 627 520 | 23 627 520 |
| Testnet26 | 7 signatures from active keyset | 35 in `vOracleNodes` and `vOraclePublicKeys` (slots 0-34 active) | 600 | 600 | 600 |
| Regtest | 4-of-7 MuSig2 | 7 in `vOracleNodes` (all in `vOraclePublicKeys`) | 650 | 650 | 0 |

There is no longer a "mainnet validation bypass" — mainnet runs the same validator and the same MuSig2 verification path as testnet and regtest.

### 14.3 Network-specific oracle roster

The roster is in [src/kernel/chainparams.cpp](src/kernel/chainparams.cpp).
Mainnet and testnet each configure 35 active slots, IDs 0–34, but their names
and keys are network-specific. `getoracles` uses the selected roster's
`OracleNodeInfo.display_name`.

| Slot | Mainnet display name | Testnet display name |
|------|----------------------|----------------------|
| 0 | DigiByte.Io Oracle | Jared |
| 11 | Crypto Corner Shop | hallvardo |

Slot 28 uses the DigiHash Mining Pool key and slot 31 uses the Peer2Peer /
DigiRoos key. Slot 31 is not an empty placeholder in this source. All configured
mainnet/testnet slots have compressed public keys; use the selected network's
actual roster when checking an operator.

Display names are local metadata. They are excluded from `OracleNodeInfo`
serialization and do not change IDs, keys, slot order, quorum or signatures.
See [src/primitives/oracle.h](src/primitives/oracle.h) and the name lookup in
[src/rpc/digidollar.cpp](src/rpc/digidollar.cpp).

### 14.4 V1 Validator Helpers (`src/oracle/bundle_manager.cpp`)

```cpp
// Live MuSig2 validator (single path for all networks).
// src/oracle/bundle_manager.cpp:ValidateMuSig2Bundle (definition at line 2430)
bool OracleBundleManager::ValidateMuSig2Bundle(
    const COracleBundle& bundle, int32_t block_height,
    const Consensus::Params& params, std::string& error);

// Off-chain consensus price (deterministic, time-independent).
// src/oracle/bundle_manager.cpp:CalculateConsensusPrice (definition at line 2553)
CAmount OracleBundleManager::CalculateConsensusPrice(
    const COracleBundle& bundle, const Consensus::Params& params);

// Required quorum for a v0x03 bundle (always nOracleConsensusRequired).
// src/oracle/bundle_manager.cpp:GetRequiredConsensus (definition at line 2371)
int OracleBundleManager::GetRequiredConsensus(
    int block_height, const Consensus::Params& params);
```

Earlier "Phase One" / "Phase Two" branch helpers (`ValidatePhaseOneBundle`, `ValidatePhaseTwoBundle`) and the `nDigiDollarPhase2Height` parameter are gone in V1. The corresponding gate is `nDigiDollarMuSig2Height` (aligned with the effective DigiDollar activation boundary), and the only on-chain bundle format is v0x03 once DigiDollar/oracle consensus is active.

**IQR Outlier Filtering** (used by `CalculateConsensusPrice` over off-chain attestations):
```
Q1 = 25th percentile
Q3 = 75th percentile
IQR = Q3 - Q1
Lower bound = Q1 - (1.5 * IQR)
Upper bound = Q3 + (1.5 * IQR)
Reject prices outside [lower_bound, upper_bound]
```

### 14.5 (Removed)

There is no longer a separate "activate Phase Two on testnet" step — testnet/regtest already activate oracle consensus and MuSig2 together with DigiDollar at their `nDDActivationHeight`, so the v0x03 format applies from the first DD-active block.

---

## Document Status

**Version:** 8.0 — Re-validated against `feature/digidollar-v1`
**Last Updated:** 2026-04-30

**Verified against code:**
- Price format: micro-USD (`1,000,000 = $1.00 USD`), constants `ORACLE_MIN_PRICE_MICRO_USD=100`, `ORACLE_MAX_PRICE_MICRO_USD=100000000` (`src/primitives/oracle.h:23-24`)
- v0x03 on-chain payload: `version + bitmap_len + bitmap + epoch + price + timestamp + 64-byte aggregate sig` (`COracleBundle::SerializeV03Data`, `OracleBundleManager::CreateOracleScript`)
- Validator: single code path for mainnet/testnet/regtest in `OracleDataValidator::ValidateBlockOracleData` in [src/oracle/bundle_manager.cpp](src/oracle/bundle_manager.cpp)
- P2P handlers: 9 message types in `src/protocol.cpp:53-62`; price/consensus/MuSig2/getoracles handlers, including `oraclehb`, share the `IsOracleP2PActive` gate in `src/net_processing.cpp` ~5440–6340, and `oraclebundle` is accepted-and-dropped.
- Historical BIP9 (deployment buried in v9.26.5 — mainnet activated at 23,869,440, testnet26 at 600, regtest buried height 0): bit 23, mainnet start `2026-06-01`, mainnet timeout `2027-06-01`, mainnet `min_activation_height=23627520`, mainnet window 40320 / threshold 28224 (70%); testnet26 start at genesis, `min_activation_height=600`, window 200, threshold 140 (70%); regtest `ALWAYS_ACTIVE`
- `OP_CHECKPRICE` is reserved and deterministically disabled (`src/script/interpreter.cpp:708-735`); it consumes one operand and pushes false without reading oracle state
- 6 active exchange fetchers initialized in `MultiExchangeAggregator::InitializeFetchers` (`src/oracle/exchange.cpp:1092-1097`); 5 fetcher classes still compile but are NOT initialized (Coinbase, Kraken, Messari, Bittrex, Poloniex); CoinMarketCap removed entirely. `FetchAllPrices()` iterates the initialized fetchers sequentially.
- `min_required_sources = 2` is the `MultiExchangeAggregator` header default (`src/oracle/exchange.h:235`); the production caller `OracleNode::FetchMedianPrice` raises the floor to 3 via `SetMinRequiredSources(3)` (`src/oracle/node.cpp:450`), so the live oracle daemon publishes only when >=3 of the 6 fetchers respond. Outlier filtering removes prices more than 10% from the median and aggregation requires the source floor before and after filtering.

**Key technical details:**
```
Oracle price format:   Micro-USD (1,000,000 = $1.00 USD)
Validation range:      100 - 100,000,000 micro-USD ($0.0001 - $100.00)
On-chain bundle size:  86-byte minimum v0x03 data; 90 bytes on the 35-slot mainnet/testnet bitmap
Off-chain attestation: 128-byte COraclePriceMessage (32-byte XOnly pubkey + 64-byte Schnorr)
Quorum:                7 signatures from the configured active mainnet/testnet keyset, 4-of-7 regtest
Activation heights:    Buried DigiDollarHeight mainnet 23869440 (static DD/oracle gates at the 23627520 floor), testnet26 600, regtest buried 0 with DD/oracle height gates 650 and MuSig2 0 by default (v9.26.5 burial)
```

Regtest note (v9.26.5 burial): the default buried `DigiDollarHeight` is `0`,
while the DD/oracle P2P height gates default to 650.
`nDigiDollarMuSig2Height = min(nDDActivationHeight, DigiDollarHeight) = 0` so
v0x03 quotes are valid whenever DigiDollar is active. The direct
`-digidollaractivationheight=N` knob retargets the buried height and the
DD/oracle/MuSig2 gates together (DD activates at exactly N);
`-testactivationheight=digidollar@H` moves only the buried deployment height,
and `-vbparams=digidollar:...` is now a startup error. Startup oracle-price
cache reconstruction follows the same buried predicate used by block
connection, so default-regtest DD-active oracle bundles below 650 are not
skipped on restart/reindex.

## Historical Issue Tracker (resolved in V1)

These items appeared in earlier revisions of this document. They are recorded here to spare future readers from chasing line numbers that no longer exist.

| Earlier claim | V1 reality |
|---------------|-----------|
| Mainnet validation returns true (bundle_manager.cpp:2229) | Removed (commit `f0d9a7b2c7`); validator runs identically on mainnet and testnet |
| v0x01 / v0x02 oracle bundle accepted on-chain | Rejected at extraction, surfaced by the validator as `bad-oracle-malformed`. The `bad-oracle-legacy` branch only fires when extraction returns true with a non-MuSig2 version and remains as defense-in-depth; commits `bbb85cf363`, `fa29405adc`, `f2bb0a19a4` |
| Empty `schnorr_sig` bypasses verification in P2P | Bound to chainparams pubkey then verified in `src/net_processing.cpp:5462-5491`; v0x03 on-chain bundle uses an aggregate signature that is always required |
| Phase One single oracle on testnet/regtest | Replaced by 7-signature mainnet/testnet MuSig2 / 4-of-7 regtest MuSig2 |
| `sendoracleprice` RPC | Removed |
| Mock prices reachable from `OP_CHECKPRICE` | Removed; `OP_CHECKPRICE` is reserved and deterministically disabled, so neither mock nor live node-local prices are read |
