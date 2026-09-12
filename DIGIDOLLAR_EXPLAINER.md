# DigiDollar - Decentralized USD Stablecoin on DigiByte
*Updated: 2026-06-25*
*DigiByte Core v9.26.2 — DigiDollar V1 launch-parameter alignment*

## Overview

DigiDollar is a decentralized USD-denominated token design native to DigiByte's UTXO model, enabling stable-value transactions without custodial bank reserves or a smart-contract VM.

### Key Points
- **DGB becomes the strategic reserve asset** (21B max supply, ~1.94 per person on Earth at 8.1B population)
- **Everything happens inside DigiByte Core wallet** — you never give up control of your private keys
- **Status (V1, `feature/digidollar-v1`)**: Testnet26 activates at height 600 after BIP9 signaling; mainnet activation gate is configured for height 23,627,520 (start time 2026-06-01) — see `DIGIDOLLAR_ARCHITECTURE.md` for details

---

## What is DigiDollar?

### DGB as a Limited, Finite Strategic Reserve Asset

With a maximum supply of 21 billion DGB, there are only **1.94 DGB per person** on Earth (based on 8.1 billion world population). Combined with DigiByte's **15-second block speed** (40x faster than Bitcoin), this extreme scarcity and fast settlement makes DGB ideal as collateral for DigiDollar - a truly finite backing for instant, stable currency transactions.

### Simple Explanation

DigiDollar is designed to track $1 USD by locking up DigiByte (DGB) as over-collateralized backing. DGB becomes a strategic reserve asset - with only 21 billion max supply (just 1.94 DGB per person on Earth), it's a finite asset backing the stability mechanism.

Unlike traditional stablecoins backed by bank accounts, DigiDollar is designed to operate as a decentralized UTXO-native system. No company or bank controls user collateral.

**Most importantly**: Everything happens directly in your DigiByte Core wallet - you never give up control of your private keys or trust a third party.

**Transaction Limits**: Minimum mint $100, maximum $100,000 per transaction (mainnet and testnet26; regtest is capped at $1,000 for testing). Minimum output $1.

### Key Benefits

- ✅ UTXO-native stablecoin design with no custodial bank reserve
- ✅ Designed to track $1 USD through over-collateralization, oracle pricing, DCA, ERR, and volatility controls
- ✅ You keep full control of private keys in Core wallet
- ✅ DGB becomes strategic reserve asset
- ✅ 15-second blocks (40x faster than BTC); fees depend on current fee policy and network conditions

---

## How It Works

### Core Idea: The Silver Safe Analogy

**Imagine DGB is silver** stored in your basement safe. You have $1,000 worth of silver but need cash today. Instead of selling your silver (and losing future gains), you lock it in a special time-locked safe.

The safe gives you $500 cash to spend today. **The silver NEVER leaves your possession** - it stays in YOUR basement, in YOUR safe. You just can't access it until the timelock expires.

10 years later, your silver is worth $10,000 (10x gain)! To unlock: Simply return the $500 to the safe → get your $10,000 silver back. You kept ALL the appreciation.

#### That's EXACTLY how DigiDollar works:

- ✅ Lock DGB in YOUR wallet (never leaves your control)
- ✅ You ALWAYS keep control of your private keys
- ✅ Get DigiDollars to spend today
- ✅ When timelock expires, burn DD → get your DGB back
- ✅ Keep ALL the DGB price appreciation

### The Tax Advantage: Liquidity Without Selling

In some jurisdictions, borrowing against assets may be treated differently from selling them. Tax treatment depends on local law and individual facts.

**Traditional Crypto Sale:**
- ❌ Sell DGB → Pay 20-40% capital gains tax
- ❌ Lose future appreciation
- ❌ Taxable event recorded

**DigiDollar Method:**
- ✅ Lock DGB → Get DigiDollars
- ✅ May avoid a sale event in some jurisdictions
- ✅ Keep ALL future DGB gains
- ✅ Theoretically never need to sell DGB

_* This document is not tax advice. Tax laws vary by jurisdiction; consult a tax professional for your specific situation._

### Economic Incentives: Why This Benefits Everyone

#### 🔒 DGB Becomes More Scarce

**1.94 DGB per person on Earth** (21B max supply ÷ 8.1B population)

With only 21 billion DGB ever to exist, locking DGB for DigiDollars makes an already scarce asset even more scarce. This creates natural price support.

- **Reduced selling pressure**: Locked DGB can't be panic sold during market volatility
- **Supply shock potential**: Significant locking could create supply squeeze
- **Benefits all DGB holders**: Even unlocked DGB benefits from reduced circulating supply

#### 💰 Personal Financial Benefits

DigiDollar can provide additional financial flexibility for DGB holders by separating liquidity access from an immediate DGB sale.

- **Tax-efficient liquidity**: Access funds without triggering capital gains
- **Keep upside potential**: Maintain full exposure to DGB price appreciation
- **Strategic flexibility**: Lock portions based on liquidity needs

**The Network Effect**: The more people use DigiDollar, the stronger the DGB ecosystem becomes. Locked DGB creates scarcity → drives price → attracts more users → creates more demand for both DGB and DigiDollar. It's a positive feedback loop that benefits all participants.

### The Technical Process

#### 1. Lock DGB Collateral
Users lock DigiByte as collateral in a P2TR (Pay-to-Taproot) time-locked vault. The amount depends on the lock period (200%-1000% of DigiDollar value, with shorter locks requiring more collateral).

#### 2. Mint DigiDollars
DigiDollars are automatically minted based on the locked DGB value and current USD exchange rate from decentralized oracles.

#### 3. Use & Redeem
Use DigiDollars for stable-value transactions. After the lock period expires, redemption requires burning the required DigiDollars to unlock the full DGB collateral; during ERR the required DD burn increases.

---

## Collateral Requirements

DigiDollar uses a sliding collateral scale to prevent attacks while rewarding long-term participants:

| Lock Period | Collateral Ratio | Undercollateralized After | DGB for $100 | Notes |
|------------|------------------|---------------------------|--------------|-------|
| 1 hour     | 1000%           | 90% drop                  | 1000 DGB     | Shortest/onboarding tier (canonical on all networks) |
| 30 days    | 500%            | 80% drop                  | 500 DGB      | |
| 3 months   | 400%            | 75% drop                  | 400 DGB      | |
| 6 months   | 350%            | 71.4% drop                | 350 DGB      | |
| 1 year     | 300%            | 66.7% drop                | 300 DGB      | |
| 2 years    | 275%            | 63.6% drop                | 275 DGB      | |
| 3 years    | 250%            | 60% drop                  | 250 DGB      | |
| 5 years    | 225%            | 55.6% drop                | 225 DGB      | |
| 7 years    | 212%            | 52.8% drop                | 212 DGB      | |
| 10 years   | 200%            | 50% drop                  | 200 DGB      | |

**Note**: The updated collateral schedule (1000% → 200%) provides enhanced stability. The 1-hour tier is canonical on all networks and locks real collateral until expiry. The "Undercollateralized After" column shows how much DGB price can drop before position becomes undercollateralized.

Mint validation uses the canonical tier declared in the mint OP_RETURN. The lock height must leave at least the tier's canonical block count and no more than that count plus the 100-block confirmation buffer; under-locked or custom durations are rejected.

---

## Potential Use Cases

These examples describe directions the protocol could support. V1 ships the core mint, transfer, redeem, collateral, oracle, DCA, ERR, and volatility machinery; it does not ship vertical-specific applications.

### Corporate Bonds
Faster settlement workflows for USD-denominated instruments

### Real Estate
Collateralized liquidity and settlement workflows around property-related assets

### Autonomous Vehicles
Machine-to-machine payments where a UTXO-native dollar unit is useful

### Global Remittances
Cross-border payments where users want non-custodial wallet control

### Healthcare Payments
Transparent payment and settlement records

### Additional Applications
Supply chain, gaming, and other wallet-native payment flows can be explored on top of the same protocol primitives.

---

## Technical Implementation

### Protocol Architecture

DigiDollar is built natively on a UTXO (Unspent Transaction Output) blockchain. All operations occur directly in DigiByte Core wallet — users maintain complete control of their private keys throughout the entire process.

**Implementation status:** This is v9.26.6 development documentation.
DigiDollar's existing mainnet deployment is buried at height 23,869,440.
The separate Thaw Day transition remains disabled on public networks in this
source. Completed code does not establish release readiness or a successful
network activation. See the [architecture guide](DIGIDOLLAR_ARCHITECTURE.md)
and [activation guide](DIGIDOLLAR_ACTIVATION_EXPLAINER.md).

### Core Technologies

#### Taproot Integration
Enhanced privacy using P2TR outputs and Schnorr signatures

#### Decentralized Oracles
Mainnet/testnet: 35 active oracle slots (0-34). The active keyset uses MuSig2
BIP-327 threshold consensus and requires 7 BIP-340 Schnorr signatures. Slot 28
uses the DigiHash Mining Pool key, slot 31 uses the Peer2Peer / DigiRoos key,
and all 35 configured slots contain valid compressed secp256k1 oracle keys.
Regtest remains 4-of-7 for local testing. Oracle prices are reported in
micro-USD format
(1,000,000 = $1.00). `primitives/oracle.h` now declares header defaults
`ORACLE_TOTAL_COUNT=35`, `ORACLE_ACTIVE_COUNT=35`, and
`ORACLE_CONSENSUS_REQUIRED=7`; per-network chainparams values such as
`nOraclePubkeyCount` and `nOracleConsensusRequired` remain authoritative for
validation.

#### MAST Implementation
Efficient script execution with Merkleized Alternative Script Trees. The collateral vault uses **2 redemption paths**:
1. **Normal Path**: CLTV timelock expiry + owner signature (system health ≥ 100%)
2. **ERR Path**: CLTV timelock expiry + OP_CHECKCOLLATERAL + owner signature (system health < 100%)

Both paths **require the timelock to expire first** - there is no early redemption, no forced liquidation, and no exceptions.

**Implementation Note**: Partial redemption is rejected at consensus. `ValidateCollateralReleaseAmount` (`src/digidollar/validation.cpp:2299+`) requires the redeemer to burn at least `requiredDDBurn` (= `originalDDMinted` for healthy systems, or the ERR-adjusted amount when health < 100%) AND release the full locked collateral; otherwise the transaction is rejected with `bad-collateral-release-partial-burn`. Non-DD transactions cannot spend a registered collateral vault at all (`bad-collateral-spend-missing-dd-burn`).

### Key Features

- ✅ No forced liquidations during market volatility
- ✅ All transactions appear identical on-chain (privacy)
- ✅ Batch signature verification for efficiency
- ✅ Native blockchain integration (no side chains)

---

## Technical Implementation Details

DigiDollar leverages advanced Bitcoin Script opcodes and DigiByte's unique capabilities to create a trustless, decentralized stablecoin system:

### Time Lock Mechanism

#### OP_CHECKLOCKTIMEVERIFY (CLTV)
Enforces canonical time-based collateral lock periods (1 hour to 10 years)

#### OP_CHECKSEQUENCEVERIFY (CSV)
⚠️ Not yet implemented: Listed as a capability but not currently used in DigiDollar scripts. Only CLTV (absolute timelocks) is used.

#### nLockTime
Prevents transactions from being mined until specified block height

### Core Script Functions

#### Multi-Sig Oracle Validation
Mainnet/testnet require 7 BIP-340 Schnorr signatures from the configured 35-slot active oracle keyset; regtest uses 4-of-7. `OP_CHECKPRICE` is reserved and deterministically disabled; it consumes its operand and pushes false rather than reading node-local oracle state.

#### Taproot Script Paths
Multiple redemption conditions in a single P2TR output

#### MAST Trees
Merkleized scripts for privacy and efficiency

### How It Works - Simple Technical Flow

#### 1. Minting Process
User creates a P2TR output with DGB collateral, embedding time lock (CLTV) and oracle price data. Script validates collateral ratio and mints corresponding DigiDollars.

#### 2. Oracle Verification
Mainnet/testnet expose 35 active oracle slots in `consensus.vOraclePublicKeys` and `vOracleNodes`. Each DD-touching block carries a MuSig2 oracle bundle in the coinbase whose aggregate Schnorr signature represents 7 configured active oracles signing the same price (BIP-327 MuSig2 over BIP-340 Schnorr). Pre-V1 (legacy) oracle bundle versions are rejected once DigiDollar is active.

#### 3. Redemption Process
After time lock expires (verified by CLTV), user can redeem DigiDollars to unlock DGB. Script burns DigiDollars and releases collateral to user's address.

**Key Design Difference**: DigiDollar uses native UTXO script capabilities instead of an account-style smart-contract VM. The protocol avoids custodial collateral and contract-admin controls; actual transaction cost and user experience depend on wallet, fee policy, and network conditions.

---

## Five-Layer Protection System

### The Time-Lock Challenge

**CRITICAL RULE**: DGB locked as collateral **CAN NEVER BE UNLOCKED** until the timelock expires. No exceptions. No early redemption. Ever.

Since collateral is cryptographically time-locked, there are **NO forced liquidations, NO margin calls, and NO early exit**. Positions must ride out the full term regardless of market conditions. This is intentional - it prevents manipulation and panic selling. This requires a unique protection approach.

### 1️⃣ Higher Collateral Requirements (First Defense)

The 1000%→200% sliding scale provides massive buffer against price drops. The one-hour tier requires 10x collateral, protecting against short-term volatility.

**Example**: With 1000% collateral, DGB can drop 90% before undercollateralization; with 500% collateral, DGB can drop 80%.

### 2️⃣ Dynamic Collateral Adjustment (Second Defense)

As system health changes, collateral requirements automatically adjust (`src/consensus/dca.cpp` HEALTH_TIERS, basis points):

- **≥150% healthy**: 1.00× (no adjustment, 10000 bps)
- **120–149% warning**: 1.25× (+25% collateral, 12500 bps)
- **110–119% critical**: 1.50× (+50% collateral, 15000 bps)
- **<110% emergency**: 2.00× (+100% collateral, 20000 bps)

DCA math runs entirely in `__int128` ceiling arithmetic and `ApplyDCA` fails closed (returns INT_MAX) when supplied with health that's stale relative to the canonical cached value, so an attacker cannot trick collateral calculations by feeding a frozen old health number.

### 3️⃣ Emergency Redemption Ratio (Third Defense)

**CRITICAL: ERR increases DD burn requirement, NOT reduces collateral return!**

If system drops below 100% collateralized, users must burn MORE DigiDollars to redeem their FULL collateral:

| System Health | ERR Ratio | DD Burn Required | Collateral Return |
|--------------|-----------|------------------|-------------------|
| 95-100% | 0.95 | 105.3% (1/0.95) | 100% (FULL) |
| 90-95% | 0.90 | 111.1% (1/0.90) | 100% (FULL) |
| 85-90% | 0.85 | 117.6% (1/0.85) | 100% (FULL) |
| <85% | 0.80 | 125% (1/0.80) | 100% (FULL) |

**Example**: At 80% system health with a $100 DD position:
- You must burn: $100 / 0.80 = **$125 DD**
- You receive back: **100% of your locked collateral** (not reduced!)

**Why this design?** ERR creates buying pressure on DD during crises - people need more DD to redeem, which helps stabilize the system. Reducing collateral would harm innocent DD holders.

ERR activates automatically when system health drops below 100%. New minting is BLOCKED during ERR until health recovers above 100%.

### 4️⃣ Volatility Protection (Fourth Defense)

The candidate block height selects the volatility rules. Before Thaw Day,
the legacy freezes remain in effect. At and above Thaw Day, new mints use a
price reference from ancestor blocks. Transfers and redemptions no longer use
the old volatility checks there, but still need to satisfy their other rules.
A large valid price move can still pause minting. There is no exact wall-clock
thaw time or oracle override.

Thaw Day also changes health to use the original DD amounts attached to open
vaults. Circulating tokens are counted separately. If closing a 100-DD vault
burns 125 DD, circulating supply falls by 125 and open-vault principal falls
by 100. With the same remaining collateral, the larger principal denominator
can keep emergency restrictions active longer or require more DD to redeem.
It does not create tokens or change balances at activation.

Mainnet, testnet26 and signet Thaw Day heights remain disabled in this
development source. See the [activation guide](DIGIDOLLAR_ACTIVATION_EXPLAINER.md)
and [accounting explanation](DIGIDOLLAR_ARCHITECTURE.md#layer-2-dynamic-collateral-adjustment-dca).

### 5️⃣ Supply & Demand Dynamics (Natural Defense)

Locked DGB reduces circulating supply, creating natural price support. With only 21B DGB max, locking creates scarcity.

**Effect**: More locking → Less supply → Higher DGB price → Better collateralization

### Real-Time System Monitoring

The system continuously tracks critical health metrics to ensure stability:

- Total DGB locked per tier
- Total DigiDollars minted
- Per-tier collateral ratios
- Aggregate system health

Accessible via RPC command: `getdigidollarstats`

**Key Insight**: These five layers work together without forced liquidations. Prevention (higher collateral), adaptation (dynamic adjustment), volatility freezes (circuit breakers), crisis management (emergency ratios), and market forces (scarcity) create a self-balancing, resilient system.

---

## Wallet Backup & Restore

### How DigiDollar Keys Work

DigiDollar uses HD (Hierarchical Deterministic) key derivation from your wallet's seed. This means:

- ✅ **Keys are derived from your wallet seed** - Not generated randomly
- ✅ **Positions can be restored from descriptors** - Using standard wallet backup
- ✅ **All operations use HD keys** - Minting, redeeming, receiving

### Backup Methods

#### Method 1: Standard Wallet Backup
```bash
# Creates a complete backup including all DigiDollar data
digibyte-cli backupwallet /path/to/backup.dat
```

#### Method 2: Descriptor Export (Recommended)
```bash
# Export descriptors with private keys
digibyte-cli listdescriptors true > descriptors.json
```

### Restore Process

```bash
# 1. Create a new wallet
digibyte-cli createwallet "restored" false false "" false true

# 2. Import descriptors
digibyte-cli -rpcwallet=restored importdescriptors '[...]'

# 3. Rescan blockchain to reconstruct DD positions
digibyte-cli -rpcwallet=restored rescanblockchain
```

**What gets restored during rescan:**
- ✅ All DGB balances and UTXOs
- ✅ DigiDollar positions (from OP_RETURN metadata)
- ✅ DD token balances (from blockchain scan)
- ✅ Position status (active/redeemed)

### Important Notes

- **Legacy wallets**: Unsupported for DigiDollar V1 mint/address creation; migrate to a descriptor/bech32m HD wallet before using DD
- **Descriptor wallets** (default since v8.23): Required for DD V1 and provide full restore capability via descriptors
- **Position data**: Reconstructed from blockchain during rescan, not stored in descriptors

---

## Learn More

- **White Paper**: https://github.com/orgs/DigiByte-Core/discussions/319
- **Tech Specs**: https://github.com/orgs/DigiByte-Core/discussions/324
- **50 Use Cases**: https://github.com/orgs/DigiByte-Core/discussions/325
- **Join Discussion**: https://github.com/orgs/DigiByte-Core/discussions

---

## Implementation Status & Code Alignment

**Last Verified**: 2026-06-25

| Feature | Document Spec | Code Reference |
|---------|---------------|----------------|
| 2 MAST Paths | Normal + ERR only | `src/digidollar/scripts.cpp:117-177` |
| Emergency oracle override | Removed | Comment at `src/digidollar/scripts.cpp:85-86` records removal |
| Partial redemption | Rejected at consensus | `src/digidollar/validation.cpp:2534` (`bad-collateral-release-partial-burn`) |
| Non-DD spend of collateral vault | Rejected at consensus | `src/digidollar/validation.cpp:2653,2671` (`bad-collateral-spend-missing-dd-burn`) |
| ERR semantics | 100% collateral, MORE DD burned | `src/consensus/err.cpp:100-149` (`__int128` ceiling math) |
| Minting blocked during ERR | Yes; also blocked when oracle absent | `src/consensus/err.cpp:417-469` |
| Both MAST paths require CLTV | Both leaves prefix-match `<lockHeight> OP_CLTV OP_DROP` | `src/digidollar/scripts.cpp:73-99` |
| Lock tiers | 10 tiers (1h, 30d, 90d, 180d, 1y, 2y, 3y, 5y, 7y, 10y) | `src/consensus/digidollar.h:57-68` |
| Custom durations rejected | Mint validation enforces canonical tier windows: `[tier_blocks, tier_blocks + 100]` | `src/digidollar/validation.cpp:1354-1384` (`bad-mint-lock-tier-duration`) |
| DCA tiers | 1.00 / 1.25 / 1.50 / 2.00 (≥150 / 120-149 / 110-119 / <110) | `src/consensus/dca.cpp:53-59` (HEALTH_TIERS) and `src/consensus/digidollar.h:94-99` (dcaLevels) |
| ERR ratios | 0.95 / 0.90 / 0.85 / 0.80 | `src/consensus/err.cpp:53-58` (ERR_TIERS) |
| Oracle config | 35 active slots, 7 signatures required (mainnet/testnet); 4-of-7 regtest | `src/kernel/chainparams.cpp` (`nOracleTotalOracles`, `nOracleRequiredMessages`, `nOracleConsensusRequired`) |
| Cooldown period | 8640 blocks (~36h) | `src/consensus/volatility.h:74` (`COOLDOWN_BLOCKS`) |
| DD amount unit | Cents (100 = $1.00) | `src/consensus/digidollar.h:70-73`, `src/digidollar/digidollar.h` |
| Oracle price unit | Micro-USD (1,000,000 = $1.00) | `src/oracle/bundle_manager.*`, `src/script/interpreter.cpp` |
| DD supply alert | Monitoring only — no hard cap | `src/digidollar/health.h:83` (`ALERT_DD_SUPPLY`) |

The full code-to-spec verification table lives in `DIGIDOLLAR_ARCHITECTURE.md` Section 18.

---

## V1 State at a Glance

The V1 branch closes the consensus and policy gaps that the previous draft of this document called out. The status is now:

| Subsystem | Source | Status |
|-----------|--------|--------|
| OP_CHECKPRICE reserved opcode | `src/script/interpreter.cpp:436-746` | `OP_CHECKPRICE` is reserved and deterministically disabled; it does not read `g_get_oracle_consensus_price` |
| MuSig2-only oracle bundles | `src/validation.cpp:185-283` | Pre-V1 (legacy) bundles rejected; mempool requires recent valid MuSig2 quote |
| Mainnet/testnet validator parity | `src/validation.cpp` | Mainnet short-circuit removed (commit `f0d9a7b2c7`) |
| DCA/ERR integer math | `src/consensus/dca.cpp`, `src/consensus/err.cpp` | `__int128` ceiling arithmetic; `ApplyDCA` fails closed on stale health |
| Confirmed-only DD chaining | `src/digidollar/validation.cpp:1810, 1954` | `MEMPOOL_HEIGHT` DD inputs rejected (commit `0b4959f563`) |
| Mining graceful degradation | `src/node/miner.cpp:857-859` | Failing DD txs (`ValidateDDForBlockInclusion`) are added to `failedTx` and skipped; assembler continues |
| DD supply alert (not a cap) | `src/digidollar/health.h:83` | Monitoring threshold only |

**Where this leaves operators**:
- **Regtest / testnet**: Fully exercisable today; testnet26 is configured with `min_activation_height = 600`.
- **Mainnet**: Configuration is in place (BIP9 bit 23, start time 2026-06-01, timeout 2027-06-01, `min_activation_height = 23627520`). Outstanding work is operational — mainnet oracle operator deployment and continued testnet validation.

---

_DigiDollar is a decentralized stablecoin on a UTXO blockchain where DGB serves as the strategic reserve asset and users retain custody of their keys throughout mint, transfer, and redeem operations._
