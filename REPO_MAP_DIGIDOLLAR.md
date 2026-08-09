# REPO_MAP_DIGIDOLLAR.md — DigiDollar + Oracle Subsystem v9.26.2

*Last updated: 2026-05-20 (v9.26.2 / `feature/digidollar-v1`)*

This is the granular file index for all DigiDollar and Oracle source code. Read `DIGIDOLLAR_ARCHITECTURE.md` and `DIGIDOLLAR_ORACLE_ARCHITECTURE.md` first for system design context.

---

## Table of Contents

1. [DigiDollar Core (`src/digidollar/`)](#digidollar-core)
2. [Consensus Rules (`src/consensus/`)](#consensus-rules)
3. [Oracle System (`src/oracle/`)](#oracle-system)
4. [Oracle Primitives (`src/primitives/`)](#oracle-primitives)
5. [RPC Interface (`src/rpc/`)](#rpc-interface)
6. [Wallet Integration (`src/wallet/`)](#wallet-integration)
7. [Index (`src/index/`)](#index)
8. [Scattered References](#scattered-references)
9. [Qt GUI](#qt-gui)
10. [Tests](#tests)

---

## DigiDollar Core

### src/digidollar/digidollar.h
- `MAX_DIGIDOLLAR` → `21000000000LL * 100` (21B dollars in cents): per-output serialization bound only. DigiDollar has no global supply cap; total circulating DD is constrained by available DGB collateral, per-block minting rate, and the alerting threshold `AlertThresholds::ALERT_DD_SUPPLY` (= 10000000000 cents = 100M DD) which fires monitoring alerts but does NOT block minting (commit `99b1f79480`).
- `CDigiDollarOutput` (class) → represents a DigiDollar UTXO with Taproot-based redemption paths
  - `CDigiDollarOutput()` → default constructor, zeroes amount/locktime, nulls collateral ID
  - `CDigiDollarOutput(nDDAmountIn, collateralIdIn, nLockTimeIn)` → parameterized constructor linking DD to specific collateral
  - `IsValid()` → validates amount is positive, within MAX_DIGIDOLLAR, and locktime is non-negative
  - `GetUSDValue()` → returns nDDAmount directly (stored in cents, 100 = $1.00)
- `CCollateralPosition` (class) → represents a locked DGB position backing DigiDollar issuance
  - `RedemptionPath` (enum) → PATH_NORMAL (timelock expiry) and PATH_ERR (emergency under-collateralization)
  - `CCollateralPosition()` → default constructor
  - `CCollateralPosition(outpoint, dgbLocked, ddMinted, unlockHeight, collateralRatio)` → full constructor
  - `GetCurrentCollateralRatio(currentPrice)` → calculates (dgbLocked × currentPrice × 100) / ddMinted with overflow protection
  - `IsHealthy(currentPrice)` → returns true if collateral ratio ≥ 100%
  - `GetRequiredDDForRedemption(systemCollateral)` → normal: returns ddMinted; ERR: returns (ddMinted × 100) / systemCollateral
  - `AddRedemptionPath(path)` → adds path to availablePaths if not already present
  - `HasRedemptionPath(path)` → checks if specific redemption path is available
- `DigiDollar::IsDigiDollarEnabled(pindexPrev, chainman)` → checks buried DEPLOYMENT_DIGIDOLLAR activation via DeploymentActiveAfter (BIP90 since v9.26.5)
- `DigiDollar::IsDigiDollarEnabled(pindexPrev, params)` → overload using consensus params — pure height compare against `DeploymentHeight(DEPLOYMENT_DIGIDOLLAR)` (no VersionBitsCache since the v9.26.5 burial)

### src/digidollar/digidollar.cpp
- Implementation of all `CDigiDollarOutput` and `CCollateralPosition` methods
- `DigiDollar::IsDigiDollarEnabled()` → both overloads reduce to the buried-height predicate (v9.26.5 burial)

### src/digidollar/health.h
- `DigiDollar::SystemMetrics` (struct) → aggregates all system-wide DD health data: supply, collateral, per-tier breakdown, DCA/ERR/volatility status, oracle status
  - `TierMetrics` (nested struct) → per-tier stats: lockDays, ddMinted, dgbLocked, positions count, healthRatio
- `DigiDollar::AlertThresholds` (struct) → static constexpr monitoring alert thresholds: ALERT_DD_SUPPLY (monitoring trigger at 100M, not a supply cap), MIN_HEALTH_RATIO (120%), CRITICAL (110%), MIN_ORACLES (5), MAX_VOLATILITY (30%), STALE_ORACLE_BLOCKS (100)
- `DigiDollar::SystemHealthMonitor` (class) → real-time system health tracking and alerting
  - `GetSystemMetrics()` → returns current SystemMetrics after updating tiers/protection/oracle status
  - `GetTierBreakdown()` → returns per-tier metrics vector with health ratios per lock period
  - `ShouldAlert(metric)` → dispatches to specific alert checkers (health, supply, collateral, oracle, volatility, positions)
  - `GetHealthHistory(blocks)` → retrieves recent health percentages from internal history map
  - `UpdateMetrics(block)` → called during block processing; updates tiers, protection, oracle status; records health history
  - `GetHealthReport()` → returns comprehensive UniValue JSON with supply, collateral, health, DCA, ERR, tiers, oracle status, alerts, recommendations
  - `Initialize()` → sets up data structures, initializes 10 tier slots (1h to 10y), preserves pre-populated data from ScanUTXOSet
  - `Shutdown()` → clears all metrics and history, marks uninitialized
  - `AggregateWalletStats(wallets, totalDDSupply, totalCollateral)` → sums DD supply and collateral across all loaded wallets via GetDDTimeLocks
  - `ScanUTXOSet(view, validation_view, blockman, mempool)` → iterates entire UTXO set via cursor, identifies DD vaults by structure (P2TR output 0 with value + P2TR output 1 with 0 value + OP_RETURN), extracts DD amounts from OP_RETURN, validates txType is MINT, skips spent coins via validation_view
  - `GetCachedMetrics()` → lightweight accessor returning const ref to s_currentMetrics without triggering updates
- `DigiDollar::HealthUtils` (namespace)
  - `GetTierIndex(lockDays)` → maps lock days to 0-based tier index
  - `GetTierLockDays(tierIndex)` → reverse lookup: tier index to lock days
  - `CalculateHealthRatio(ddAmount, dgbAmount, dgbPrice)` → (dgbAmount × dgbPrice / COIN × 100) / ddAmount, capped at 300%
  - `FormatHealthStatus(health)` → "Healthy" (≥120%), "Warning" (≥110%), "Critical" (<110%)
  - `GetRecommendedAction(health)` → "Monitor", "Add Collateral", or "Emergency Action Required"

### src/digidollar/health.cpp
- Implementation of all SystemHealthMonitor static members and HealthUtils functions
- 10-tier system: {0, 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650} days
- CalculateSystemHealth uses (collateral × price / COIN × 100) / ddSupply, capped at 300%
- Integrates with Volatility, DCA, and ERR subsystems for protection status
- **Incremental DD metrics tracking (T5-06)** — called from ConnectBlock/DisconnectBlock under cs_main:
  - `OnMintConnected(ddAmount, dgbCollateral)` → increments totalDDSupply and totalCollateral when a DD mint is connected
  - `OnRedeemConnected(ddAmount, dgbCollateral)` → decrements supply/collateral when a DD redeem is connected (clamped to 0)
  - `OnMintDisconnected(ddAmount, dgbCollateral)` → reverses mint during reorg (decrements, clamped to 0)
  - `OnRedeemDisconnected(ddAmount, dgbCollateral)` → reverses redeem during reorg (re-increments supply/collateral)

### src/digidollar/scripts.h
- `DigiDollar::COLLATERAL_NUMS_POINT_BYTES` → BIP-341 NUMS (Nothing Up My Sleeve) point bytes: provably unspendable key for Taproot internal key, prevents key-path spending that bypasses CLTV
- `DigiDollar::GetCollateralNUMSKey()` → returns XOnlyPubKey from NUMS bytes
- `DigiDollar::MintParams` (struct) → parameters for minting: ddAmount, lockHeight, ownerKey, internalKey, oracleKeys vector
- `DigiDollar::CreateCollateralP2TR(params)` → creates Taproot output with 2-leaf MAST: normal redemption (CLTV + owner sig) and ERR path (CLTV + OP_CHECKCOLLATERAL + OP_DIGIDOLLAR + owner sig); uses TaprootBuilder with leaf version 0xC0
- `DigiDollar::CreateDigiDollarP2TR(owner, ddAmount)` → creates simple key-path-only P2TR for freely transferable DD tokens, applies standard Taproot tweak
- `DigiDollar::GetOracleKeys(count)` → deterministic helper-key generator retained for tests/helpers; production oracle quorum comes from chainparams and MuSig2, not this helper
- `DigiDollar::CreateNormalRedemptionPath(params)` → script: `<lockHeight> OP_CLTV OP_DROP <ownerKey> OP_CHECKSIG`
- `DigiDollar::CreateERRPath(params)` → script: `<lockHeight> OP_CHECKLOCKTIMEVERIFY OP_DROP <100> OP_CHECKCOLLATERAL OP_NOT OP_VERIFY OP_DIGIDOLLAR <ddAmount> OP_DDVERIFY <ownerKey> OP_CHECKSIG`. Witness stack is `<signature> <collateralRatio>`; `OP_CHECKCOLLATERAL` consumes `<ratio> <100>` and pushes `ratio>=100`, then `OP_NOT` flips it to `ratio<100`, `OP_VERIFY` enforces ERR-only path (system under-collateralized) — see `src/digidollar/scripts.cpp:88-115`. The CLTV prefix means ERR is not an early-redemption path.
- `DigiDollar::RegisterScriptMetadata(script, type, ddAmount, lockHeight)` → process-local helper/test metadata registry for scripts (max 10k entries, FIFO eviction); production validation derives authoritative metadata from transactions/UTXO lookup when available
- `DigiDollar::GetScriptMetadata(script, metadata)` → retrieves metadata by script hash from global registry

### src/digidollar/scripts.cpp
- Implementation of all script creation functions
- `CreateCollateralP2TR` finalizes TaprootBuilder with 2 leaves at depth 1, registers metadata
- `CreateDigiDollarP2TR` applies standard BIP-341 tweak (nullptr merkle root) for key-path only spending
- Global `g_scriptMetadataMap` protected by `RecursiveMutex`, capped at 10,000 entries

### src/digidollar/txbuilder.h
- `DigiDollar::MIN_DD_FEE_RATE` = 35,000,000 sat/kvB (0.35 DGB/kvB) → the DigiDollar fee rate; shared by mint/transfer/redeem RPCs and wallet coin selection
- `DigiDollar::MIN_DD_TX_FEE` = 10,000,000 sat (0.1 DGB) → absolute fee floor for any DigiDollar transaction
- `DigiDollar::TxBuilderResult` (struct) → result of tx building: success, CMutableTransaction, error string, totalFees, collateralRequired, ddChange
- `DigiDollar::TxBuilderMintParams` (struct) → ddAmount, lockDays, lockTier (0-9), ownerKey, feeRate, utxos, optional dgbChangeDest
- `DigiDollar::TxBuilderTransferParams` (struct) → recipients vector (address, amount), feeRate, ddUtxos, ddAmounts, feeUtxos, feeAmounts, spenderKey, optional dgbChangeDest
- `DigiDollar::TxBuilderRedeemParams` (struct) → collateralOutpoint, ddToRedeem, path, ownerKey, feeRate, ddUtxos, ddAmounts, feeUtxos, feeAmounts, optional collateralDest/dgbChangeDest, pre-queried position data
- `DigiDollar::RedeemablePosition` (struct) → wallet-facing position info: collateralOutpoint, ddAmount, dgbLocked, unlockHeight, availablePaths, canRedeemNow, estimatedReturn
- `DigiDollar::TxBuilder` (base class) → holds chainParams, currentHeight, oraclePrice
  - `CalculateFee(tx, feeRate)` → estimates vsize × feeRate / 1000
  - `SelectCoins(utxos, target, inputs, total)` → greedy largest-first selection with MAX_TX_INPUTS (400) cap
  - `ValidateAmount(amount)` → checks 0 < amount ≤ MAX_MONEY
  - `ValidateFeeRate(feeRate)` → enforces 100k–100M sat/kB range
- `DigiDollar::MintTxBuilder` (class extends TxBuilder)
  - `BuildMintTransaction(params)` → creates complete mint tx: selects coins, creates P2TR collateral + DD token + OP_RETURN metadata (DD marker, txType, amount, lockHeight, lockTier, ownerXOnlyPubKey), handles change with iterative fee calculation
  - `CalculateRequiredCollateral(ddAmount, lockDays)` → uses __int128 arithmetic to prevent overflow: (ddAmount × COIN × adjustedRatio × 100) / oraclePrice
  - `LockDaysToBlocks(days)` → delegates to consensus LockDaysToBlocks
- `DigiDollar::TransferTxBuilder` (class extends TxBuilder)
  - `BuildTransferTransaction(params)` → creates DD transfer: adds DD inputs + fee inputs, creates P2TR outputs for recipients, DD change with Taproot tweak, DGB change, OP_RETURN with all DD amounts; sets version 0x02000770; enforces DD conservation with dust tolerance
  - `ValidateTransferParams(params)` → validates recipients (addresses, amounts ≤ $100k, ≥ dust), DD UTXOs, key, fee rate
  - `CalculateTotalDDInput(inputs, amounts)` → sums DD amounts from provided vector or extracts from outputs
  - `CreateDDTransferScript(recipient, amount)` → creates P2TR for transfer via CreateDigiDollarP2TR
  - `SelectDDInputs(available, needed, selected, total)` → greedy DD coin selection
- `DigiDollar::RedeemTxBuilder` (class extends TxBuilder)
  - `BuildRedemptionTransaction(params)` → creates redemption tx: collateral input (nSequence=0xFFFFFFFE for CLTV), DD inputs to burn, fee inputs; output 0 = full collateral return, DD change if excess, DGB fee change; sets nLockTime = unlockHeight; supports pre-queried position data
  - `DetermineRedemptionPath(params)` → returns ERR if systemCollateral < 100%, else NORMAL
  - `CalculateCollateralReturn(ddAmount, originalCollateral, currentPrice)` → returns full proportional collateral
  - `VerifyRedemptionConditions(params, path, position)` → checks timelock expired + path-specific conditions
  - `CreateRedemptionScript(path, owner)` → creates Schnorr-signed redemption script
- `DigiDollar::EncodeDigiDollarAddress(dest, chainParams)` → converts CTxDestination to DD address string via CDigiDollarAddress
- `DigiDollar::EstimateTransactionVSize(tx)` → estimates vsize with 110-byte witness per input + 35% safety margin
- `DigiDollar::EstimateInputSpendCost(feeRate)` → approximate fee cost of one extra input (92 vB marginal measured against EstimateTransactionVSize at zero inputs → 3,220,000 sat at MIN_DD_FEE_RATE); a fee UTXO worth less has negative effective value. Approximation only: the estimator's double truncation makes the true marginal alternate 92/93 vB, absorbed by the redemption re-projection loop. Returns 0 for a non-positive rate, MAX_MONEY on overflow

### src/digidollar/txbuilder.cpp
- Full implementation of all TxBuilder classes (~1,425 lines)
- Mint tx: OP_RETURN format = `OP_RETURN <"DD"> <1> <ddAmount> <lockHeight> <lockTier> <ownerXOnlyPubKey>`
- Transfer tx: OP_RETURN format = `OP_RETURN <"DD"> <2> <amount1> <amount2> ... <amountN>`
- Redeem tx: OP_RETURN format = `OP_RETURN <"DD"> <3> <ddChangeAmount>`
- Uses `__int128` for collateral calculations to prevent overflow at large amounts
- MAX_TX_INPUTS = 400 prevents exceeding MAX_STANDARD_TX_WEIGHT

### src/digidollar/validation.h
- `DigiDollar::DD_TX_VERSION` → 0x44440000 (defined but unused; actual marker is 0x0D1D0770 with lower 16-bit mask 0x0770 in `consensus/digidollar.cpp`)
- `DigiDollar::ScriptType` (enum) → NOT_DIGIDOLLAR, COLLATERAL_LOCK, DD_TOKEN_OUTPUT
- `DigiDollar::RedemptionPath` (enum) → NORMAL (health ≥ 100%), ERR (health < 100%)
- `DigiDollar::TxLookupFn` (typedef) → function type for looking up transactions from block database by txid + coin height
- `DigiDollar::ValidationContext` (struct) → nHeight, oraclePriceMicroUSD, systemCollateral, chainParams, coins view, skipOracleValidation flag, txLookup callback
- **Core Validation:**
  - `ValidateDigiDollarScript(script, ctx, serror)` → validates DD script structure, checks for OP_DIGIDOLLAR markers in non-DD scripts, verifies amounts
  - `ValidateDigiDollarTransaction(tx, ctx, state)` → main entry: dispatches to mint/transfer/redeem validators, updates volatility state, checks ERR mint blocking
- **Script Analysis:**
  - `IdentifyScriptType(script)` → returns ScriptType using Phase 1 metadata registry (Phase 2 uses UTXO db)
  - `ExtractDDAmount(script, amount)` → tries metadata registry first, then parses OP_RETURN formats: `OP_DIGIDOLLAR <8-byte LE>` or `"DD" <type> <amount>`
  - `ExtractDDAmountFromPrevTx(prevout, amount)` → decentralized approach: looks up creating tx via g_txindex, parses its OP_RETURN
  - `IsCollateralScript(script)` → checks metadata for COLLATERAL_LOCK type
  - `IsDDTokenScript(script)` → checks metadata for DD_TOKEN_OUTPUT type
  - `HasDigiDollarMarker(tx)` → checks version field for DD marker (delegates to consensus)
  - `GetDigiDollarTxType(tx)` → extracts type from version field (delegates to consensus)
- **Path Validation:**
  - `ValidateNormalRedemption(script, currentHeight)` → checks timelock expiry via metadata
  - `ValidateEmergencyRedemption(script, sigs)` → validates the configured oracle signature threshold
  - `ValidateERRRedemption(script, systemCollateral)` → checks system < 100% collateralized
- **Amount/Collateral Validation:**
  - `ValidateMintAmount(amount, params, nHeight)` → validates against min/max with activation height awareness
  - `ValidateOutputAmount(amount, params)` → validates ≥ minOutputAmount and ≤ MAX_DIGIDOLLAR
  - `ValidateCollateralRatio(dgbLocked, ddMinted, lockTime, ctx)` → calculates required collateral, compares with actual, logs detailed breakdown
  - `CalculateRequiredCollateral(ddAmount, lockTime, ctx)` → uses __int128: (ddAmount × COIN × effectiveRatio × 100) / oraclePriceMicroUSD
  - `GetEffectiveCollateralRatio(baseRatio, systemCollateral, params)` → applies DCA multiplier to base ratio
- **Transaction Type Validation:**
  - `ValidateMintTransaction(tx, ctx, state)` → comprehensive: structural checks, volatility freeze, output analysis, OP_RETURN parsing (type/amount/lockHeight/lockTier/ownerPubKey), SECURITY: lock tier↔height consistency using `[canonical_blocks, canonical_blocks + 100]`, NUMS key reconstruction + verification [T1-04], single collateral output [T1-04c], single DD output (inflation attack prevention), single OP_RETURN [T1-04f], collateral ratio validation using the claimed canonical tier [T2-01]
  - `ValidateTransferTransaction(tx, ctx, state)` → DD conservation: extracts amounts from OP_RETURN, validates P2TR outputs, looks up confirmed input DD amounts via txindex → block-db → metadata registry → coins view, rejects `MEMPOOL_HEIGHT` DD inputs, enforces strict inputDD == outputDD
  - `ValidateRedemptionTransaction(tx, ctx, state)` → validates collateral + confirmed DD inputs, burns check (totalDDInputs > totalDDOutputs), path routing by system health, delegates to normal/ERR conditions
- **Redemption Helpers:**
  - `ValidateNormalRedemptionConditions(tx, ctx, state)` → checks nHeight ≥ nLockTime and system health ≥ 100%
  - `ValidateEmergencyRedemptionConditions(tx, ctx, state)` → validates ERR: timelock expired, system < 100%, and collateral release burns the consensus ERR-adjusted DD amount while returning full locked collateral
  - `ValidateCollateralReleaseAmount(tx, ctx, ddBurned, state)` → SECURITY [T1-08]: extract original DD and lock height from creating MINT tx via txindex → block-db → ephemeral metadata registry, REJECTS as `bad-collateral-release-utxo-not-found` / `bad-collateral-release-zero-collateral` / `bad-collateral-release-unknown-lock-height` if unknown; SECURITY [T2-03]: requires `ddBurned >= requiredDDBurn` where `requiredDDBurn = ERR::GetRequiredDDBurn(originalDDMinted, systemHealth)` when health < 100 else `originalDDMinted` — partial burn rejected as `bad-collateral-release-partial-burn`; SECURITY [T2-06b]: detects collateral-as-fee-input attack and rejects as `bad-redeem-collateral-as-fee-input`; net DGB release must equal full locked amount within `feeTolerance = max(1000, allowedRelease/1000)` — over-release rejected as `bad-collateral-release-excessive`, under-release as `bad-collateral-release-incomplete`; input 0 must be the canonical collateral vault output of the creating mint and `tx.nLockTime` must be at least the original mint lock height.
  - `ValidateScriptPathSpending(tx, ctx, state)` → supplemental DD witness hook currently logs/returns true; standard Taproot script validation plus NUMS output reconstruction enforce collateral script-path spending. Do not treat this helper as the consensus witness validator.
  - `ValidateCollateralOutput(output, tx, state)` → checks P2TR format via Solver, value ≥ 546 sats dust
  - `ValidateDDOutput(output, tx, state)` → checks nValue == 0, P2TR format via Solver
  - `ExtractLockTime(script)` → parses script for OP_CLTV, extracts lock period, defaults to 30 days
  - `GetSystemCollateralRatio()` → reads cached `SystemHealthMonitor` metrics; returns cached health when present, computes deterministic health from cached collateral/DD supply/oracle price otherwise, returns 300% when no DD exists, and fails closed to 0% when active DD supply lacks price/collateral data.
- **ERR Validation:**
  - `ValidateERRRedemption(tx, ctx, state)` → routes through `ValidateEmergencyRedemptionConditions()` and `ValidateCollateralReleaseAmount()` so ERR requires expired timelock, system health < 100%, full collateral return, and ERR-adjusted DD burn
  - `ShouldBlockMintingDuringERR(ctx)` → delegates to ERR::EmergencyRedemptionRatio::ShouldBlockMinting()
  - `ShouldBlockNormalRedemptionsDuringERR(ctx)` → checks ERRState.isActive
  - `ValidateERRAdjustmentAmount(original, adjusted, systemHealth)` → verifies ERR ratio within tolerance
  - `ValidateERROracleConsensus(tx, ctx)` → legacy helper retained for tests/external callers from earlier ERR designs; intentionally fails closed because V1 ERR consensus comes from the block's validated v0x03 MuSig2 oracle bundle and deterministic `ValidationContext` health, not per-transaction oracle signatures.
  - `CalculateExpectedERRAdjustment(systemHealth)` → delegates to ERR system

### src/digidollar/validation.cpp
- Full implementation (~3,026 lines)
- `ExtractDDAmountFromTxRef()` (static) → shared helper: parses OP_RETURN with type-aware field extraction (MINT: only first value is DD amount; TRANSFER: all values are DD amounts); SECURITY: verifies source tx has DD marker to prevent DD-from-nothing attacks
- `ExtractDDAmountFromBlockDb()` (static) → universal fallback: loads creating tx from block database using coin height
- `ValidationCache` (struct) → thread-safe cache for script types and amounts (max 10k entries)

---

## Consensus Rules

### src/consensus/digidollar.h
- `DigiDollar::BLOCKS_PER_DAY` → 5760 blocks (15-second block time)
- `DigiDollarTxType` (enum) → DD_TX_NONE(0), DD_TX_MINT(1), DD_TX_TRANSFER(2), DD_TX_REDEEM(3), DD_TX_MAX(4)
- `DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS` → 100-block consensus buffer used by mint validation; remaining lock blocks must be in `[canonical_blocks, canonical_blocks + 100]` for the claimed tier.
- `DigiDollar::ConsensusParams` (struct) → collateral ratios map (1h:1000%, 30d:500%, 90d:400%, 180d:350%, 1y:300%, 2y:275%, 3y:250%, 5y:225%, 7y:212%, 10y:200%); mint limits (`minMintAmount=10000`, `maxMintAmount=10000000` in cents = $100-$100k); `minOutputAmount=100` ($1); oracle config defaults `oracleCount=35`, `activeOracles=35`, `oracleThreshold=7` (35-slot active roster); DCA levels `dcaLevels = [{150,100},{120,125},{110,150},{100,200}]` (system collateral % → multiplier %, e.g. 110-119% triggers 150%) - these match `src/consensus/dca.cpp:53-58` HEALTH_TIERS (1.00/1.25/1.50/2.00x).
- `GetCollateralRatioForLockTime(lockBlocks, params)` → returns collateral ratio % only for exact canonical lock periods; returns 0 for custom/in-between periods. Mint validation applies the 100-block buffer separately against the declared tier.
- `GetDCAMultiplier(systemCollateral, params)` → returns collateral requirement multiplier from DCA levels
- `IsValidMintAmount(amount, params)` → validates against min/max mint amounts
- `GetMinimumDDOutput(params)` → returns minOutputAmount from consensus params
- `LockDaysToBlocks(days)` → converts days to blocks (special case: 0 days = 240 blocks = 1 hour)
- `BlocksToLockDays(blocks)` → reverse: blocks to approximate days
- `ValidateConsensusParams(params, strError)` → sanity checks all DD consensus parameters
- `IsDigiDollarActive(nHeight, params)` → checks activation height (deprecated: use IsDigiDollarEnabled)
- `GetLockTierIndex(lockBlocks, params)` → returns tier index in collateral ratios map
- `FormatLockPeriod(lockBlocks)` → human-readable lock period string
- `HasDigiDollarMarker(tx)` → checks bits 0-15 of nVersion for DD marker 0x0770
- `GetDigiDollarTxType(tx)` → extracts DD tx type from bits 24-31 of nVersion

### src/consensus/digidollar.cpp
- Implementation of all consensus parameter functions
- OP_RETURN DD marker detection logic

### src/consensus/dca.h
- `DigiDollar::DCA::HealthTier` (struct) → minCollateral %, maxCollateral %, multiplier, status string
- `DigiDollar::DCA::DynamicCollateralAdjustment` (class) → adjusts collateral requirements based on system health
  - `CalculateSystemHealth(totalCollateral, totalDD, oraclePrice)` → returns health % (0–30000); 30000 if no DD, 0 if no price/collateral
  - `GetDCAMultiplier(systemHealth)` → >=150%: 1.0x, 120-149%: 1.25x, 110-119%: 1.5x, <110%: 2.0x
  - `ApplyDCA(baseRatio, systemHealth)` → baseRatio × multiplier
  - `GetCurrentTier(systemHealth)` → returns HealthTier for current health level
  - `IsSystemEmergency(systemHealth)` → true if health < 100%
  - `GetTotalSystemCollateral()` → scans UTXO set for total locked DGB (expensive)
  - `GetTotalDDSupply()` → calculates total DD in circulation (expensive)
  - `GetCurrentSystemHealth()` → convenience: returns -1 if oracle unavailable
  - `IsOracleAvailable()` → checks if oracle price is available
  - `GetCurrentDCAMultiplier()` → convenience: current multiplier from live data
  - `ValidateDCAConfig(error)` → validates tier configuration
  - `FormatSystemHealth(systemHealth)` → formatted percentage string
  - `FormatDCAMultiplier(multiplier)` → formatted multiplier string (e.g. "1.25x")
  - `HandleRapidTransition()`, `ValidateExtremeValues()`, `ValidateMultiplierPrecision()`, `PreventIntegerOverflow()`, `HandleConcurrentUpdates()`, `VerifyMemoryStability()`, `ValidateErrorHandling()`, `IsStateTransitionTracked()`, `HasHysteresis()`, `TrackSystemRecovery()`, `ValidateConcurrentCalculations()`, `SimulateResourceExhaustion()` → extreme scenario testing functions

### src/consensus/dca.cpp
- Implementation of DCA system with 4 health tiers
- Thread-safe health calculations

### src/consensus/err.h
- `DigiDollar::ERR::ERRState` (struct) → isActive, systemHealth, adjustmentRatio (0.8–1.0), activationHeight, oracleConsensusHash, activationTimestamp
- `DigiDollar::ERR::EmergencyRedemptionRatio` (class) → emergency protection when system < 100% collateralized
  - `ShouldActivateERR(systemHealth)` → true if health < 100%
  - `CalculateERRAdjustment(systemHealth)` → 95–100%: 0.95, 90–95%: 0.90, 85–90%: 0.85, <85%: 0.80
  - `GetRequiredDDBurn(originalDDMinted, systemHealth)` → returns `ceil(originalDDMinted * 10000 / ratioBps)` using `__int128` (no double-precision): e.g. at 80% health (ratioBps=8000), 100 DD requires 125 DD burn. Saturates at `numeric_limits<CAmount>::max()` for extreme values (`src/consensus/err.cpp:120-128`, commits `55926c372a` / `9cca6970ae`).
  - `GetAdjustedRedemption(normalRedemption, systemHealth)` → DEPRECATED identity passthrough; returns `normalRedemption` unchanged. Kept for ABI compatibility; new code MUST use `GetRequiredDDBurn` (collateral return is always 100% under V1 ERR semantics, commit `55926c372a`).
  - `HasOracleConsensus(bundle)` → validates the configured oracle signature threshold for ERR activation
  - `GetCurrentState()` → returns current ERRState
  - `GetERRQueue()` → returns pending ERR redemption outpoints
  - `QueueERRRedemption(outpoint, ddAmount, requestHeight)` → adds to FIFO ERR queue
  - `ProcessERRQueue(maxRedemptions)` → processes queue in FIFO order with current ERR ratio
  - `ActivateERR(oracleBundle, activationHeight)` → activates ERR with oracle consensus
  - `DeactivateERR(currentHealth)` → deactivates when health recovers ≥ 100%
  - `ReconstructERRState(currentSystemHealth, currentHeight)` → rebuilds ERR state from chain data after restart
  - `ValidateERRRedemption(tx, expectedDDAmount, expectedCollateral)` → validates ERR redemption tx structure
  - `ShouldBlockMinting()` → returns true during ERR to prevent destabilization
  - `GetERRStatistics()` → formatted stats string
  - `ValidateERRConfig(error)` → validates ERR configuration
  - `FormatERRAdjustment(ratio)` → human-readable ratio string
  - `FormatERRHealth(health, isERRActive)` → formatted health status
  - Extreme scenario testing: `HandleHealthOscillation()`, `ValidatePrecisionBoundaries()`, `ValidateExtremeHealthValues()`, `ValidateThreadSafety()`, `HandleConsensusFailure()`, `ShouldActivateERRWithCorruptedState()`, `ShouldActivateERRAtHeight()`, `ShouldActivateERRAtTime()`, `HasActivationDelay()`, `ValidateRatioPrecision()`, `PreventCalculationOverflow()`, `ValidateCalculationConsistency()`, `HandleLargeOracleMessageCount()`, `ValidateMalformedMessageHandling()`, `ValidateConsensusPerformance()`

### src/consensus/err.cpp
- Implementation of ERR system with tiered adjustment ratios
- ERR queue management (FIFO with pro-rata distribution)

### src/consensus/volatility.h
- `DigiDollar::Volatility::PricePoint` (struct) → price, timestamp, height
- `DigiDollar::Volatility::VolatilityState` (struct) → hourly/daily/weekly volatility %, mintingFrozen, allOperationsFrozen, freezeHeight, cooldownEndHeight
- `DigiDollar::Volatility::VolatilityThresholds` (struct) → WARNING_1H (10%), FREEZE_MINT_1H (20%), FREEZE_ALL_24H (30%), EMERGENCY_7D (50%), COOLDOWN_BLOCKS (8640 ≈ 36 hours)
- `DigiDollar::Volatility::VolatilityMonitor` (class) → monitors price volatility and manages freeze mechanisms
  - `RecordPrice(price, timestamp, height)` → adds price point to history deque (max 30 days × 24h)
  - `GetPriceHistory()` → returns copy of all recorded price points
  - `ClearHistory()` → resets all price data (testing)
  - `CalculateVolatility(timeWindow)` → standard deviation-based volatility for time window in seconds
  - `GetCurrentState()` → returns current VolatilityState
  - `UpdateState(currentHeight)` → recalculates all volatility metrics, triggers/clears freezes
  - `ShouldFreezeMinting()` → true if 1-hour volatility > 20%
  - `ShouldFreezeAll()` → true if 24-hour volatility > 30%
  - `InCooldownPeriod()` → true during post-freeze cooldown
  - `GetCooldownEndHeight()` → block height when cooldown expires
  - `TriggerFreeze(freezeAll, height)` → manual freeze trigger
  - `ClearFreeze()` → manual freeze clear
  - `ReconstructFromBlockData(blockPrices, currentHeight)` → re-feeds price history after restart
  - `GetDiagnosticInfo()` → detailed diagnostic string
  - `IsInitialized()` → checks if system has price data
  - `GetDataAge()` → seconds since last price update
- `FormatVolatility(volatility)` → formatted percentage string
- `CalculatePercentageChange(oldPrice, newPrice)` → percentage change between two prices
- `ExceedsThreshold(oldPrice, newPrice, threshold)` → checks if change exceeds threshold

### src/consensus/volatility.cpp
- Implementation of volatility monitoring with sliding window calculations

### src/consensus/digidollar_transaction_validation.h
- **Mint Validation:**
  - `ValidateMintAmount(amount, ddParams)` → validates against consensus min/max
  - `ValidateCollateralRatio(ddAmount, collateralAmount, oraclePrice, requiredRatio)` → ensures sufficient collateral
  - `ValidateOraclePrice(price)` → positive and within reasonable bounds
- **Transfer Validation:**
  - `ValidateDDConservation(inputs, outputs, fee)` → inputs == outputs + fee
  - `ValidateDDAddress(address)` → address format validation
  - `ValidateNoDoubleSpend(inputs1, inputs2)` → checks for conflicting inputs
  - `SelectDDUTXOs(amounts, target)` → coin selection algorithm for DD
- **Redeem Validation:**
  - `ValidateRedemptionPath(type, currentHeight, lockHeight, errActive)` → validates path for current state
  - `ValidateRedemptionAmount(redeemAmount, totalHeld, isFullRedeem)` → validates against holdings
  - `ValidateTimelockForRedeem(type, currentHeight, lockHeight, errActive)` → timelock checks
  - `ShouldActivateERR(collateralPercentage)` → checks emergency threshold
- **Script Execution:**
  - `CreateDDOutputScript(amount, lockBlocks)` → creates P2TR DD output script
  - `ValidateDDScript(script)` → validates DD script structure
  - `ValidateDDWitnessStack(stack)` → validates witness data format
  - `ScriptExecutionResult` (struct) → success flag + stack size
  - `ExecuteDDScript(script)` → runs DD script with validation
  - `ValidateDDOpcode(opcode)` → checks opcode is allowed in DD context

### src/consensus/digidollar_transaction_validation.cpp
- Implementation of all transaction validation helper functions

### src/consensus/digidollar_tx.h
- `IsValidDigiDollarType(type)` → validates DD transaction type enum value
- `ValidateDigiDollarTxStructure(tx, strError)` → validates overall DD transaction structure

### src/consensus/digidollar_tx.cpp
- Implementation of DD transaction structure validation

---

## Oracle System

### src/oracle/bundle_manager.h
- `OracleBundleManager` (class) → manages oracle message collection, validation, block integration
  - **Configuration:** `SetEnabled()`, `IsEnabled()`, `SetMinOracleCount()`
  - **Message Management:**
    - `AddOracleMessage(message)` → validates and adds oracle price message to pending set
    - `RemoveOracleMessage(oracle_id)` → removes pending message by oracle ID
    - `GetPendingMessages()` → returns all pending messages awaiting bundling
    - `GetPendingMessageCount()` → count of pending messages
    - `ClearPendingMessages()` → clears all pending messages
    - `InjectTestMessage(message)` → bypass validation for testing
  - **Bundle Management:**
    - `GetCurrentBundle(epoch)` → returns bundle for specific epoch
    - `UpdateBundle(bundle)` → updates stored bundle
    - `HasValidBundle(epoch)` → checks if valid bundle exists for epoch
    - `CleanupOldBundles(current_epoch)` → removes expired bundles
    - `TryCreateBundle(epoch)` → explicitly creates bundle from pending messages
  - **Block Integration:**
    - `AddOracleBundleToBlock(block, height)` → embeds oracle data in coinbase transaction
    - `CreateOracleScript(bundle)` → creates OP_RETURN script encoding oracle bundle
    - `ExtractOracleBundle(coinbase_tx, bundle)` → parses oracle data from coinbase
  - **Price Functions:**
    - `GetConsensusPrice(epoch)` → returns median price for epoch
    - `GetLatestPrice()` → returns most recent consensus price
    - `UpdateCachedPrice(epoch)` → refreshes price cache
  - **Validation:**
    - `ValidateMuSig2Bundle(bundle, block_height, params, error)` → static: V1 validator; checks bitmap parses, participants ≥ `nOracleConsensusRequired`, members ∈ [0, `nOraclePubkeyCount`), runs `MuSig2OracleAggregator::ComputeAggregatePubkeyFromBitmap`, and BIP-340-verifies the aggregate signature against `ComputeOracleBundleHash(bundle)` (`src/oracle/bundle_manager.cpp:2430`)
    - `ValidateBundle(bundle, height, params)` → static: thin wrapper around `HasMuSig2Quorum` (used by tests/RPC)
    - `GetRequiredConsensus(height, params)` → returns `nOracleConsensusRequired`
    - `CalculateConsensusPrice(bundle, params)` → static: IQR-filtered median over the off-chain attestations; price-range checks only (no wall-clock dependence) so consensus is deterministic during IBD/replay
    - `HasMuSig2Quorum(bundle, params)` (free function in `bundle_manager.cpp:85`) → checks that a v0x03 bundle is complete (signature length = 64, bitmap parses, ≥ `nOracleConsensusRequired` participants)
  - **Network:**
    - `BroadcastMessage(message)` → broadcasts via P2P
    - `ProcessIncomingMessage(message)` → handles incoming P2P oracle message
    - `HasOracleMessage(hash)` → duplicate detection
    - `AddVersionHeartbeat(heartbeat)` / `GetVersionHeartbeat(oracle_id, out)` / `GetVersionHeartbeats()` → stores the latest verified oracle software/protocol heartbeat per oracle ID
    - `BroadcastVersionHeartbeat(heartbeat)` → broadcasts signed `oraclehb` telemetry over P2P
    - `SetConnman(connman)` → sets P2P connection manager
    - `BroadcastConsensusProposal(epoch, price, timestamp)` → off-chain consensus proposal (input to MuSig2). Tracked per-epoch to prevent spam.
    - `HasBroadcastConsensusProposal(epoch)` → checks if a proposal was already sent for `epoch`.
    - `RegisterSeenAttestation(hash)` → tracks attestation hashes for replay prevention
  - **Status:**
    - `OracleStats` (struct) → pending_messages, active_bundles, latest_price, latest_epoch, last_update, has_consensus
    - `GetStats()` → returns current oracle statistics
  - **Lifecycle:**
    - `GetInstance()` → singleton access
    - `Initialize()`, `Shutdown()` → lifecycle management
    - `LoadPricesFromChain(chainman)` → loads oracle prices from blockchain on startup
    - `Clear()` → clears all state (testing)
    - `ValidateConfiguration()` → validates oracle config
  - **Price Cache:**
    - `UpdatePriceCache(height, price_micro_usd)` → stores price at block height
    - `GetOraclePriceForHeight(height)` → retrieves cached price for height
    - `RemovePriceCache(height)` → removes price during block disconnect
- `OracleDataValidator` (class) → validates oracle data in blocks and transactions
  - `ValidateBlockOracleData(block, pindex_prev, params, state)` → V1 entry point. Returns true pre-activation; otherwise requires DD mint/redeem blocks to carry exactly one valid v0x03 bundle (`bad-oracle-missing`, `bad-oracle-multiple-outputs`, `bad-oracle-malformed`, `bad-oracle-legacy`, `bad-oracle-musig2`, `bad-oracle-timestamp`). DD transfer-only and non-DD blocks may omit the bundle. Implemented at `src/oracle/bundle_manager.cpp:2151`.
  - `ValidateOraclePriceForTx(tx, oracle_price, height)` → sanity-check the oracle price feeding a DD tx (range and non-zero)
  - `ValidateOracleMessage(message, params)` → checks `message.IsValid()` plus chainparams authorization and `VerifyAttestation()`
  - `ValidateOracleBundle(bundle, height, params)` → wraps `OracleBundleManager::ValidateMuSig2Bundle`; rejects non-MuSig2 bundles with `bad-oracle-legacy`-style logging
  - `CheckOracleSignatures(bundle, params)` / `CheckOracleConsensus(bundle, params)` → both delegate to `HasMuSig2Quorum`
  - `CheckOracleEpoch(bundle, current_epoch)` → epoch consistency check
- `OracleIntegration` (namespace) → utility functions for integration
  - `GetCurrentOraclePrice()` → legacy, returns cents (rounds sub-cent to 1)
  - `GetCurrentOraclePriceMicroUSD()` → full precision: 1,000,000 = $1.00
  - `GetOraclePriceForHeight(nHeight)` → price in micro-USD at specific height
  - `IsOracleSystemReady()` → checks if oracle system is operational
  - `GetOracleBundleForHeight(height)` → gets bundle for specific block
  - `ValidateOracleRequirements(tx, height)` → validates oracle data for DD tx

### src/oracle/bundle_manager.cpp
- Full implementation of OracleBundleManager, OracleDataValidator, and OracleIntegration
- Epoch-based bundle management with configurable consensus thresholds
- Price cache with per-height storage for block connect/disconnect
- **V1 validator:** `ValidateMuSig2Bundle()` verifies bitmap, threshold, key membership, aggregate-pubkey derivation, and BIP-340 Schnorr aggregate signature; runs identically on mainnet, testnet, and regtest (commit `f0d9a7b2c7`).
- **Legacy bundle rejection:** `ExtractOracleBundle()` short-circuits on raw v0x01/v0x02 OP_RETURN payloads by returning false (commits `bbb85cf363`, `fa29405adc`, `f2bb0a19a4`); the validator emits `bad-oracle-malformed`. The `bad-oracle-legacy` branch only fires when extraction succeeds with a non-MuSig2 version; since v0x03 returns true immediately after parsing, it is structurally unreachable from current wire payloads and is kept as defense-in-depth.
- **IQR consensus price:** `CalculateConsensusPrice()` sorts prices, computes Q1/Q3, filters outliers outside Q1−1.5×IQR to Q3+1.5×IQR, returns median of filtered set; falls back to unfiltered median if <4 prices or all filtered.
- **Consensus attestations:** `AddConsensusAttestation()` / `ClearPendingAttestations()` accumulate per-oracle attestations as off-chain inputs to MuSig2 (they do NOT update the canonical price cache; only a complete on-chain MuSig2 bundle does).

### src/oracle/exchange.h
- `ExchangeAPI::BaseExchangeFetcher` (base class) → persistent CURL handle to prevent socket exhaustion on Windows
  - `FetchPrice()` → pure virtual: returns price in micro-USD
  - `ConvertToMicroUSD(price_str)` / `ConvertToMicroUSD(double)` → converts price to micro-USD
  - `ExtractJsonValue(json, key)` → basic JSON value extraction
  - `HttpGet(url)` → makes HTTP GET request via reusable CURL handle
- **Exchange Fetchers (all override FetchPrice → micro-USD):**
  Active in `MultiExchangeAggregator::InitializeFetchers()` (`src/oracle/exchange.cpp:1075-1104`):
  - `BinanceFetcher` → DGBUSDT direct or DGBBTC→BTCUSDT cross-pair (`data-api.binance.vision`)
  - `CoinGeckoFetcher` → public API aggregator, no key required
  - `KuCoinFetcher` → DGB/USDT, no key required
  - `GateIOFetcher` → DGB/USDT, no key required
  - `HTXFetcher` → DGB/USDT (formerly Huobi), no key required
  - `CryptoComFetcher` → DGB/USD, no key required
  Defined but **NOT** initialized into `fetchers` (still compile, used only by tests / kept as future hooks):
  - `CoinbaseFetcher`, `KrakenFetcher`, `BittrexFetcher`, `PoloniexFetcher`, `MessariFetcher` — each marked as broken or paywalled in code comments; `CoinMarketCap` was removed entirely.
- `ExchangeAPI::MultiExchangeAggregator` (class) → fetches from the 6 initialized exchanges and calculates a consensus price
  - `ExchangePrice` (struct) → exchange name, price_micro_usd, timestamp, success, weight
  - `FetchAggregatePrice()` → fetches all, filters outliers, returns median
  - `FetchAllPrices()` → iterates the initialized fetchers sequentially and records successful responses
  - `CalculateMedianPrice(prices)` → simple median
  - `CalculateWeightedMedian(prices)` → weighted median by exchange reliability
  - `CalculateWeightedAverage(prices)` → weighted average
  - `FilterOutliers(prices)` → removes prices > 10% from median
  - `IsOutlier(price, prices)` → checks single price against outlier threshold
  - `HasSufficientData()` → checks min_required_sources (default 2)
  - `SetExchangeWeight(exchange, weight)` / `GetExchangeWeight(exchange)` → per-exchange weights

### src/oracle/exchange.cpp
- Implementation of fetchers and the `MultiExchangeAggregator`. Six fetchers (Binance, KuCoin, Gate.io, HTX, Crypto.com, CoinGecko) are pushed into `fetchers` by `InitializeFetchers()`; the remaining classes compile but are not initialized.
- `MultiExchangeAggregator::min_required_sources = 2` (`src/oracle/exchange.h:235`); aggregation requires at least two responsive sources both before and after outlier filtering. `OracleNode::FetchMedianPrice()` raises the live daemon floor to three sources via `SetMinRequiredSources(3)`.
- `FilterOutliers()` uses a percentage-threshold rule (10% deviation from median by default); `CalculateConsensusPrice` in `bundle_manager.cpp` is the IQR-based deterministic-consensus path used at validation time.

### src/oracle/mock_oracle.h
- `MockOracleManager` (class, singleton) → regtest helper for scripted price tests. `OP_CHECKPRICE` is reserved and deterministically disabled, so production script validation never reads mock oracle state.
  - `GetInstance()` → singleton access
  - `GetCurrentPrice()` → returns mockPriceMicroUSD (micro-USD)
  - `SetMockPrice(price_micro_usd)` → sets mock price
  - `IsEnabled()` / `SetEnabled(enable)` → toggle mock oracle
  - `GetLastUpdateHeight()` → block height of last update
  - `CreateMockBundle(height)` → creates bundle with deterministic test-key signed messages
  - `SimulateVolatility(percentChange)` → applies % change to current price
  - `GetTestKey(oracle_id)` → returns deterministic test private key for low-id regtest oracles
  - `Reset()` → restores default state

### src/oracle/mock_oracle.cpp
- Deterministic regtest helper. Test keys derived from `SHA256("digibyte_regtest_oracle_N")` (N=0..6); matching pubkeys are pushed into `consensus.vOraclePublicKeys` in `chainparams.cpp:1249-1255`.
- Builds bundles that satisfy the regtest 4-of-7 quorum (`consensus.nOracleConsensusRequired`).
- Used only by regtest. Production `OP_CHECKPRICE` is reserved and deterministically disabled; it never falls back to `MockOracleManager` or live node-local oracle state.

### src/oracle/node.h
- `OracleNode` (class) → oracle node daemon: price fetching, signing, broadcasting
  - `Initialize(oracle_id, private_key_hex)` → initializes with WIF-encoded key
  - `Initialize(oracle_id, key, pubkey)` → test-only: direct CKey initialization
  - `SetExchangeEndpoints(endpoints)` → configures price sources
  - `SetUpdateInterval(seconds)` / `SetBroadcastInterval(seconds)` → timing config
  - `Start()` / `Stop()` → thread lifecycle
  - `IsRunning()` / `IsEnabled()` / `SetEnabled(enable)` → state queries
  - `GetCurrentPrice()` / `GetLastUpdateTime()` / `HasValidPrice()` → price data
  - `GetOracleId()` / `GetPublicKey()` / `GetLastBroadcastTime()` / `GetStartTime()` → identity
  - `GetOraclePrivateKey()` → returns the initialized oracle signing key
  - `GetOraclePublicKey()` → returns XOnlyPubKey for Schnorr signatures
  - `ValidateOracleKey()` → verifies key is authorized in chainparams
  - `CreatePriceMessage(price, timestamp)` → creates Schnorr-signed COraclePriceMessage
  - `CreateConsensusAttestation(consensus_price, consensus_timestamp)` → creates signed attestation of agreed consensus price
  - `CreateVersionHeartbeat()` / `BroadcastVersionHeartbeat()` → signs and broadcasts `oraclehb` software/protocol heartbeat telemetry
  - `BroadcastPriceMessage(message)` → broadcasts via P2P network
- `ExchangePriceFetcher` (class) → legacy test/mock fetcher path; live oracle daemon uses `MultiExchangeAggregator::FetchAggregatePrice()`
  - `ExchangePrice` (struct) → exchange, price, timestamp, valid
  - `FetchAllPrices()` → returns mock Binance/Coinbase/Kraken/Bittrex/Poloniex-like prices for legacy tests
  - `GetMedianPrice()` → median of all valid prices
- `OracleManager` (class) → manages multiple oracle nodes
  - `Initialize()` / `Shutdown()` → lifecycle
  - `AddOracleNode(oracle_id, private_key_hex)` / `RemoveOracleNode(oracle_id)` → node management
  - `GetOracleNode(oracle_id)` → returns node by ID
  - `StartAll()` / `StopAll()` → batch control
  - `EnableOracle(oracle_id, enable)` → per-oracle enable/disable
  - `GetActiveOracleCount()` / `GetActiveOracleIds()` / `IsOracleRunning(oracle_id)` → status
  - `GetInstance()` → singleton
  - `StartOracleService()` / `StopOracleService()` → global control

### src/oracle/node.cpp
- Full implementation of OracleNode, ExchangePriceFetcher, and OracleManager
- Background price thread with configurable intervals
- Broadcasts price messages according to the price interval and version heartbeats every 300 seconds while enabled. `OracleNode::FetchMedianPrice()` uses `MultiExchangeAggregator`, sets the source floor to three, and publishes only when enough of the six active fetchers respond.

### src/oracle/musig2_session.h / .cpp
- `MuSig2SessionState` (enum) → CREATED, NONCES_COLLECTING, NONCES_COMPLETE, SIGNING, COMPLETE, FAILED
- `MuSig2SigningSession` (class) → in-process MuSig2 (BIP-327) signing state machine for a single epoch
  - Manages local signer's secret nonce and collects pubnonces and partial signatures from peers
  - Two-round protocol: nonce exchange → partial signature exchange → aggregation

### src/oracle/musig2_messages.h
- `ORACLE_MUSIG2_SESSION_CONTEXT_VERSION = 2` → current context transcript version
- `OracleMusigNonceMsg` (class) → MuSig2 round-1 P2P message: epoch, attempt_id, oracle_id, pubnonce (66 bytes), Schnorr signature (RH-24 authentication)
  - `GetHash()` → dedup hash; `GetSignatureHash()` → hash of fields signed; `Sign()` / `Verify()` → authentication
- `OracleMusigContextMsg` (class) → MuSig2 context proposal: epoch, attempt_id, context_version, epoch_selection_seed, proposer_id, participant_ids, nonce_set_hash, quote_set_hash, consensus_price, consensus_timestamp, session_context_id, nonce_evidence, price_evidence, Schnorr signature
- `OracleMusigPartialSigMsg` (class) → MuSig2 round-2 P2P message: epoch, attempt_id, context_version, session_context_id, oracle_id, partial_sig, Schnorr signature

### src/oracle/musig2_aggregator.h / .cpp
- `MuSig2OracleAggregator` (class) → BIP-327 compliant oracle key aggregation using secp256k1_musig_pubkey_agg
  - Variable-length bitmap encoding for oracle participation sets
  - Thread-safe cache keyed by bitmap hash

### src/oracle/musig2_orchestrator.h / .cpp
- `MuSig2CompletedResult` (struct) → aggregate_sig (64-byte BIP-340 Schnorr) + participation_bitmap
- MuSig2 Session Manager — orchestrates per-epoch signing session lifecycle: session creation, nonce generation, peer nonce/sig collection, session advancement, pruning

### src/oracle/musig2_oracle_participation.h / .cpp
- `MuSig2OracleParticipation` (class) → manages an oracle's participation in the two-round MuSig2 signing protocol for Phase 3 oracle bundles
  - Per-epoch lifecycle: nonce generation → nonce collection → consensus value signing → partial sig collection → bundle creation

### src/oracle/musig2_session_manager.h / .cpp
- `MuSig2SessionManager` (class) → manages per-epoch signing sessions for P2P collection
  - `OnNonceReceived()` / `OnPartialSigReceived()` → process incoming P2P messages
  - `CheckTimeouts()` → timeout detection per new block tip

### src/oracle/musig2_session_mining.h
- Re-declares the legacy `g_oracle_signing_sessions` / `g_oracle_signing_sessions_mutex` globals (defined in `musig2_orchestrator.cpp`) so P2P ingestion shims and tests can include them from a single header.
- IMPORTANT: this map is **not** the miner's source of truth. `OracleBundleManager::AddOracleBundleToBlock` queries `g_signing_orchestrator->GetCompletedSession(epoch, ...)` (which reads the orchestrator's private `m_signing_sessions`) for completed v0x03 bundles. The legacy globals and `OracleBundleManager::CompleteMuSig2Session` are retained only for the P2P ingestion shim and the `musig2_p2p_ingestion_tests` regression suite.

### src/oracle/signing_orchestrator.h / .cpp
- `OracleSigningOrchestrator` (class, extends `CValidationInterface`) → drives the per-epoch MuSig2 signing protocol on every `BlockConnected`/`UpdatedBlockTip` callback.
  - Oracle nodes: generate round-1 nonces (`oramusnonce`), ingest or propose a context (`oramusigctx`) that fixes signer/nonce/quote state, then issue round-2 partial signatures (`oramusigpsig`).
  - Non-oracle nodes: collect nonces, contexts, and partial sigs from peers, aggregate the final 64-byte BIP-340 signature plus participation bitmap.
  - Provides `GetCompletedSession(epoch, ...)` — `OracleBundleManager::AddOracleBundleToBlock` (`src/oracle/bundle_manager.cpp:760-887`, completed-session path at 839-858) calls this when assembling the coinbase template.
  - rh58 cap on partialsig DoS: orchestrator bounds the number of cached partial signatures per epoch to prevent memory amplification.

---

## Oracle Primitives

### src/primitives/oracle.h
- **Constants** (`src/primitives/oracle.h:19-24`):
  - `ORACLE_CONSENSUS_REQUIRED` = 7
  - `ORACLE_ACTIVE_COUNT` = 35 (active slot capacity; chainparams `nOraclePubkeyCount` is authoritative for active MuSig2 keys)
  - `ORACLE_TOTAL_COUNT` = 35
  - `ORACLE_MAX_AGE_SECONDS` = 3600 (1 hour)
  - `ORACLE_MIN_PRICE_MICRO_USD` = 100 ($0.0001)
  - `ORACLE_MAX_PRICE_MICRO_USD` = 100000000 ($100.00)
- `COraclePriceMessage` (class) → individual oracle price report with BIP-340 Schnorr signatures
  - Fields: oracle_id, price_micro_usd, timestamp, block_height, nonce, oracle_pubkey (XOnlyPubKey), schnorr_sig (64 bytes)
  - `IsValid(reference_time)` → validates structure and timing
  - `Sign(key, merkle_root, aux)` → BIP-340 Schnorr signature over `GetSignatureHash()`
  - `Verify()` → Schnorr signature verification using the full-field hash
  - `GetSignatureHash()` → SHA256d over oracle_id + price_micro_usd + timestamp + block_height + nonce (legacy full-field hash)
  - `GetAttestationSignatureHash()` → compact attestation hash (oracle_id + price + timestamp only); off-chain input to MuSig2
  - `SignAttestation(key)` / `VerifyAttestation()` → sign / verify using the attestation hash
  - `CheckForConflictingMessages(messages)` → detects duplicate/conflicting oracle submissions
- `COracleBundle` (class) → oracle consensus bundle; V1 on-chain bundles are MuSig2 v0x03
  - Fields: messages vector, epoch, median_price_micro_usd, timestamp, version, aggregate_sig, participation_bitmap
  - `IsValid(min_required, reference_time)` → validates bundle structure and signatures (min_required first, reference_time defaults to 0)
  - `AddMessage(message)` → adds validated message to bundle
  - `HasConsensus(min_required)` → checks if ≥ min_required valid messages exist
  - `GetConsensusPrice(min_required)` → calculates median price from valid messages
  - `ValidateEpoch(current_epoch)` → checks epoch consistency
- `OracleNodeInfo` (struct) → oracle node definition: id, pubkey, endpoint, is_active
- `SelectOraclesForEpoch(all_oracles, epoch)` → deterministic selection of active `OracleNodeInfo` entries across the configured 35-slot roster
- `GetCurrentEpoch(block_height)` → calculates epoch from block height
- `OracleP2P` (namespace) → unit-testable P2P validation helpers; production relay admission, per-peer rate limiting, stale-epoch rejection, and dedup live in `src/net_processing.cpp`
  - `ValidateIncomingMessage(message)` → comprehensive P2P message validation
  - `ValidateBundleMessage(bundle)` → bundle size limits and duplicate checks
  - `ValidateGetOracleRequest(request)` → request parameter bounds checking
  - `CheckRateLimit(oracle_id)` → sliding window rate limiting
  - `CheckMessageSize(message)` → oversized message prevention
  - `UpdateRateLimits()` → periodic cleanup of rate limit state
  - `ClearRateLimitState()` → reset for testing

---

## RPC Interface

### src/rpc/digidollar.h
- **System Monitoring:**
  - `getdigidollarstats()` → returns comprehensive DD system statistics (supply, collateral, health, tiers, oracle)
  - `getdcamultiplier()` → returns current DCA multiplier and system health
  - `calculatecollateralrequirement()` → calculates DGB needed for given DD amount and lock period
  - `getdigidollarstatus()` → stale header declaration only; no implementation or RPC registration exists in the current source tree
- **Core Transactions:**
  - `mintdigidollar()` → mints DD by locking DGB collateral with specified lock tier
  - `senddigidollar()` → sends DD to another address (confirmed inputs only, RC32+)
  - `sendmanydigidollar()` → sends DD to multiple recipients in one transaction (RC32+, commit `9143bed9b9`)
  - `redeemdigidollar()` → redeems DD to unlock DGB collateral
  - `listdigidollarpositions()` → lists all collateral positions and minted DD
- **Address Management:**
  - `getdigidollaraddress()` → generates new DD receive address
  - `validateddaddress()` → validates DD address format
  - `listdigidollaraddresses()` → lists all DD addresses in wallet
  - `importdigidollaraddress()` → validates an external DD address and returns an explicit V1 unsupported/no-op warning; it does not import or rescan wallet state
- **Utility:**
  - `getdigidollarbalance()` → returns DD balance
  - `estimatecollateral()` → estimates collateral needed without minting
  - `getredemptioninfo()` → redemption details for a position
  - `listdigidollartxs()` → lists DD transaction history
  - `listdigidollarunspent()` / `listdigidollarutxos()` → list spendable DD UTXOs for coin control / explicit input selection
  - `getoracleprice()` → returns current oracle price
  - `getprotectionstatus()` → returns DCA/ERR/volatility protection status
- **Oracle:**
  - `createoraclekey()` → generates oracle signing key pair
  - `exportoracleprivkey()` → exports a wallet-stored oracle private key for backup/migration
  - `importoracleprivkey()` → imports a wallet-stored oracle private key for recovery/migration
  - `startoracle()` → starts oracle node with given key
- `RegisterDigiDollarRPCCommands(t)` → registers 18 commands with the RPC table (4 system-monitoring, 1 unsupported address-validation/import stub, 1 utility, 1 oracle price, 1 protection-status, 1 multi-oracle price, 5 oracle management — including `getoraclesigners`, 4 mock oracle for regtest only). Wallet-context DD/oracle commands are registered separately via `GetWalletRPCCommands()` in `src/wallet/rpc/wallet.cpp`.

### src/rpc/digidollar.cpp
- Full implementation of all DD RPC commands
- Integrates with wallet, oracle, health monitoring systems

### src/rpc/digidollar_transactions.{h,cpp} *(legacy / unregistered)*
- 9-entry command table (`digidollar_transaction_commands` at `src/rpc/digidollar_transactions.cpp:384–395`) defining `getdigidollarinfo`, `getdigidollaraddress`, `getdigidollarbalance`, `mintdigidollar`, `transferdigidollar`, `redeemdigidollar`, `getredemptioninfo`, `listredeemablepositions`, `createrawddtransaction`. (`setmockoracleprice` is NOT in this table — comment at line 393 explicitly delegates to `rpc/digidollar.cpp`.)
- ⚠️ **NOT registered** anywhere in the build. `GetDigiDollarTransactionRPCCommands()` is defined but never called. The active versions of all callable RPCs live in `src/rpc/digidollar.cpp` (`RegisterDigiDollarRPCCommands`) and `src/wallet/rpc/wallet.cpp` (`GetWalletRPCCommands`). Treat this file as legacy until removed or rewired.

### src/wallet/rpc/wallet.cpp *(DigiDollar/oracle wallet-context registrations)*
- `GetWalletRPCCommands()` at `src/wallet/rpc/wallet.cpp:888` registers 17 wallet-context commands: `mintdigidollar`, `senddigidollar`, `sendmanydigidollar`, `redeemdigidollar`, `listdigidollarpositions`, `listdigidollaraddresses`, `getredemptioninfo`, `getdigidollarbalance`, `getdigidollaraddress`, `listdigidollartxs`, `listdigidollarunspent`, `listdigidollarutxos`, `validateddaddress`, `createoraclekey`, `exportoracleprivkey`, `importoracleprivkey`, `startoracle`. These require a loaded wallet because they read DD owner/address keys (`StoreOwnerKey`/`GetOwnerKey`), DD UTXO wallet state, or oracle private keys (`StoreOracleKey`/`GetOracleKey`).

---

## Wallet Integration

### src/wallet/digidollarwallet.h
- `DDTransaction` (struct) → wallet-facing DD transaction: txid, amount, timestamp, confirmations, incoming, address, category (send/receive/mint/redeem), blockheight, blockhash, fee, comment, abandoned, lock_tier
- `WalletDDBalance` (struct) → address → balance mapping with last_updated timestamp
- `WalletCollateralPosition` (struct) → dd_timelock_id, dd_minted, dgb_collateral, lock_tier, unlock_height, is_active, owner_keyid
- `DDUtxo` (struct) → spendable DD UTXO: outpoint, dd_amount, is_spendable
- `DigiDollarWallet` (class) → high-level wallet interface for DD operations
  - **Key Encryption (T4-03a):**
    - `EncryptDDKeys(vMasterKey, encrypted_batch)` → encrypts all plaintext DD keys, called from CWallet::EncryptWallet()
  - **Owner Key Management:**
    - `StoreOwnerKey(dd_timelock_id, key)` → persists DD owner key; encrypts if wallet is encrypted (T4-03a)
    - `LoadDDOwnerKeys()` → loads persisted owner keys during wallet init (handles both plaintext and encrypted)
    - `GetOwnerKey(dd_timelock_id, key)` → retrieves and decrypts owner key for signing (T4-03a)
  - **Address Key Management:**
    - `StoreAddressKey(output_key, key)` → persists P2TR address key; encrypts if wallet is encrypted (T4-03a)
    - `LoadDDAddressKeys()` → loads persisted address keys during wallet init (handles both plaintext and encrypted)
    - `GetAddressKey(output_key, key)` → retrieves and decrypts address key for signing (T4-03a)
    - `IsDDOutputMine(txout, txid)` → checks dd_owner_keys, dd_crypted_owner_keys, dd_address_keys, dd_crypted_address_keys, and IsMine for ownership
    - `IsDDOutputMine(outpoint)` → checks dd_utxos map (source of truth for owned DD)
  - **Database Extension (Task 5.1):**
    - `WriteDDBalance(addr, balance)` → persists DD balance to wallet.dat
    - `WriteDDTimeLock(position)` → persists DDTimeLock to wallet.dat
    - `UpdatePositionStatus(dd_timelock_id, active)` → updates position active flag in DB
  - **Balance Tracking (Task 5.2):**
    - `GetDDBalance(addr)` → balance for specific address or total
    - `GetTotalDDBalance()` → confirmed DD balance across all addresses
    - `GetPendingDDBalance()` → unconfirmed but trusted DD balance
    - `ScanForDDUTXOs()` → full UTXO scan at startup (expensive)
    - `ProcessTransactionForDD(tx, txid)` → incremental UTXO update per block
    - `GetLockedCollateral()` → total DGB locked in active positions
    - `GetDDTimeLocks(active_only)` → list of DDTimeLock (collateral) positions
    - `GetDDUTXOs()` → all spendable DD UTXOs
    - `GetDDFromUTXO(outpoint)` → DD amount for specific UTXO
    - `AddDDUTXO(outpoint, dd_amount)` / `RemoveDDUTXO(outpoint)` / `HasDDUTXO(outpoint)` → UTXO tracking
    - `IsDDTokenUnspent(dd_timelock_id)` → checks if DD token for position is spendable
    - `AddCollateralPosition(position)` → adds position to wallet
  - **Transaction Creation (Task 5.3):**
    - `MintDigiDollar(dd_amount, lock_tier, tx_out)` → creates mint transaction via MintTxBuilder
    - `TransferDigiDollar(to, amount, tx_out)` → creates transfer via TransferTxBuilder
    - `RedeemDigiDollar(dd_timelock_id, amount, tx_out)` → creates redemption via RedeemTxBuilder
    - `TransferDigiDollar(to, amount, txid, error)` → legacy overload returning txid string
  - **Wallet Restore:**
    - `ExtractDDAmountFromOpReturn(tx, dd_amount)` → static: parses OP_RETURN DD amount
    - `ExtractUnlockHeightFromOpReturn(tx, unlock_height)` → static: parses OP_RETURN lock height
    - `ExtractTierFromOpReturn(tx, lock_tier)` → static: parses OP_RETURN lock tier (new format)
    - `DeriveLockTierFromHeight(mint_height, unlock_height)` → static: DEPRECATED backward-compat tier derivation
    - `ExtractPositionFromMintTx(tx, block_height, pos_out)` → extracts full position data from mint tx
    - `ProcessDDTxForRescan(ptx, block_height)` → rebuilds positions during wallet rescan from MINT txs, marks inactive for REDEEM txs
  - **Redemption:**
    - `RedeemDigiDollar(collateralUtxo, ddAmount, path, txid, error)` → full redemption with string outputs
    - `GetRedeemablePositions()` → lists positions eligible for redemption
    - `CalculateRedemptionValue(position)` → estimates DGB return
    - `CanRedeem(position, availablePath)` → checks redeemability and best path
    - `GetRedemptionHistory()` → redemption transaction history
    - `EstimateRedemptionFee(position, path)` → estimated fee in satoshis
    - `GetDGBBalance()` → current DGB balance for fee payments
  - **State Management (Task 6):**
    - `BurnDigiDollars(amount, burnedUtxos)` → selects and marks DD UTXOs as spent
    - `CloseCollateralPosition(outpoint, partial, remainingDD)` → updates position after redemption
    - `MarkDDUTXOsSpent(spent_utxos)` → marks consumed DD UTXOs
    - `AddDDChangeUTXO(tx, change_vout, dd_amount)` → adds change UTXO from transfer
    - `UpdateDDUTXOSet(tx, input_utxos, change_vout, change_amount)` → comprehensive UTXO update
    - `UpdateDDTimeLockStatus(dd_timelock_id, new_status)` → updates active/inactive flag
    - `GetDDTimeLockStatus(dd_timelock_id)` → returns "active"/"partially_redeemed"/"fully_redeemed"/"not_found"
    - `IsDDTimeLockRedeemable(dd_timelock_id, current_height)` → checks unlock height + DD remaining
  - **Signing:**
    - `SignDDInputs(tx, dd_utxos, fee_utxos)` → Phase 3.1: Schnorr signatures for DD P2TR inputs
    - `SignFeeInputs(tx, fee_utxos, dd_input_count)` → Phase 3.2: signs standard DGB fee inputs
    - `SignTransaction(tx, dd_utxos, fee_utxos)` → Phase 3.3: coordinates DD + fee signing
    - `SignRedemptionTransaction(tx, collateral, dd_utxos, fee_utxos, owner_key)` → signs collateral + DD + fee inputs
  - **Submission:**
    - `CommitDDTransaction(tx, error)` → Phase 4.1: submits to mempool
    - `GetDDTransactionConfirmations(txid)` → confirmation count
    - `UpdateDDConfirmations(block_hash)` → updates all DD tx confirmations on new block
    - `GetUnconfirmedDDTransactions()` → list of unconfirmed DD txids
  - **Receive Operations (Task 6):**
    - `DetectIncomingDDOutputs(tx, our_dd_outputs)` → finds DD outputs belonging to this wallet
    - `AddReceivedDDUTXO(tx, vout_index, dd_amount)` → adds received DD to spendable set
    - `AddRedemptionToHistory(tx)` → records redemption in history
    - `ProcessIncomingDDTransaction(tx)` → coordinator: detect → add UTXO → update balance
    - `ProcessIncomingTransaction(tx, txid)` → processes any incoming DD tx and adds to history
  - **Coin Selection:**
    - `SelectDDCoins(target, selected_utxos, selected_total, amounts)` → selects DD UTXOs for target amount
    - `SelectFeeCoins(fee_amount, selected_utxos, selected_total, amounts, exclude, minimize_inputs=false, fee_rate=MIN_DD_FEE_RATE)` → selects DGB UTXOs for fees. Prices candidates by EFFECTIVE value (value − `EstimateInputSpendCost(fee_rate)`): UTXOs that cost at least as much to spend as they are worth are skipped, and `fee_amount` must be met by the sum of effective values, not the raw sum (`selected_total` is still the raw total). Smallest-first by default (spends small UTXOs down, at the cost of a larger fee); `minimize_inputs=true` sorts largest-first for the fewest inputs
    - `SelectRedemptionFeeCoins(params, error, projected_fee)` → `DDFeeSelectionResult` {OK, INSUFFICIENT_FUNDS, INVALID_TRANSACTION}. Fee-input selection for redemptions: excludes the collateral outpoint and the DD UTXOs being burned, then converges (≤6 rounds) by projecting the redemption tx, deriving the real fee from `EstimateTransactionVSize`, and re-selecting against it; enforces MAX_STANDARD_TX_WEIGHT and the MIN_DD_TX_FEE floor. Fills `params.feeUtxos`/`feeAmounts`; `projected_fee` is an upper bound
    - `CalculateTransactionFee(tx)` → estimates fee for transaction
  - **Utility:**
    - `IsLockedByDD(outpoint)` → checks if outpoint is locked by DD (protects from UnlockAllCoins)
    - `LoadFromDatabase()` → loads all DD data from wallet.dat on init
- `DigiDollarWalletUtils` (namespace)
  - `GetLockDaysForTier(tier)` → converts tier 0-9 to lock days
  - `GetMinCollateralRatio(tier)` → minimum collateral % for tier
  - `IsValidDDAddress(address)` → validates DD address format

### src/wallet/digidollarwallet.cpp
- Full implementation of DigiDollarWallet (~2000+ lines)
- Integrates with wallet database, transaction builders, signing, mempool

### src/wallet/ddcoincontrol.h / .cpp
- `wallet::DDCoinControl` (class) → manual DD UTXO selection for transactions
  - `m_allow_other_inputs` → if true, allows adding unselected inputs alongside selected ones
  - `m_min_depth` / `m_max_depth` → chain depth bounds for UTXO availability
  - `HasSelected()` → returns true if pre-selected UTXOs exist
  - `IsSelected(output)` → checks if specific UTXO is pre-selected
  - `Select(output)` → locks UTXO for spending
  - `UnSelect(output)` / `UnSelectAll()` → removes selections
  - `ListSelected()` → returns vector of selected outpoints

---

## Index

### src/index/digidollarstatsindex.h
- `DigiDollarStats` (struct) → network-wide DD statistics at a block height: total_dd_supply, total_collateral, vault_count, height, block_hash
- `DigiDollarStatsIndex` (class, extends BaseIndex) → maintains incremental DD statistics index
  - `DigiDollarStatsIndex(chain, cache_size, memory, wipe)` → constructor
  - `LookUpStats(block_index)` → queries DD statistics for specific block
  - `CustomInit(block)` → loads last known stats from database
  - `CustomCommit(batch)` → persists incremental state
  - `CustomAppend(block)` → processes new block: scans for DD txs, updates running totals
  - `CustomRewind(current_tip, new_tip)` → reverses block effects during reorg
- `g_digidollar_stats_index` → global index instance

### src/index/digidollarstatsindex.cpp
- Implementation of incremental DD statistics tracking during block processing

---

## Scattered References

Files outside the DigiDollar/Oracle directories that contain DD integration code:

### src/base58.h / src/base58.cpp
- `CDigiDollarAddress` (class) → DigiDollar address encoding/decoding with 2-byte version prefixes (mainnet/testnet/regtest)
  - `SetDigiDollar(dest, type)` → encodes CTxDestination as DD address
  - `GetDigiDollarDestination()` → decodes back to CTxDestination
  - `IsValidDigiDollarAddress(str)` → static: validates DD address format
- `EncodeDigiDollarAddress(dest)` / `DecodeDigiDollarAddress(str)` → free functions for DD address conversion

### src/chainparams.cpp
- ⚠️ Handles `-digidollaractivationheight` CLI arg for regtest; since the v9.26.5 burial it sets the buried `DigiDollarHeight` plus the static DD/oracle/MuSig2 gates (DD activates at exactly N)

### src/validation.cpp / src/validation.h
- ⚠️ `MemPoolAccept::PreChecks` → checks `DigiDollar::HasDigiDollarMarker()`, verifies buried-deployment activation via `IsDigiDollarEnabled()`, creates `ValidationContext` with oracle price from `GetOraclePriceForTransaction()`, calls `ValidateDigiDollarTransaction()`
- ⚠️ `ConnectBlock` → same DD validation during block connection with `skipOracleValidation` for historical blocks, includes `txLookup` callback for block-db DD amount extraction
- ⚠️ `GetBlockScriptFlags` → sets `SCRIPT_VERIFY_DIGIDOLLAR` flag when DEPLOYMENT_DIGIDOLLAR is active
- ⚠️ `DisconnectBlock` → calls `RemovePriceCache()` to revert oracle price data
- ⚠️ `GetOraclePriceForTransaction()` → helper: block validation uses the block-extracted oracle price only and returns 0 without local fallback; mempool uses the cached P2P price and can use mock oracle only on regtest when enabled

### src/init.cpp
- ⚠️ Registers `-digidollar`, `-digidollaractivationheight`, `-digidollarstatsindex` and all DD RPC args under OptionsCategory::DIGIDOLLAR
- ⚠️ Manages `g_digidollar_stats_index` lifecycle (init, interrupt, stop)

### src/script/script.h / src/script/script.cpp
- ⚠️ Defines DigiDollar Tapscript opcodes (consume BIP-342 OP_SUCCESSx slots, gated by `SCRIPT_VERIFY_DIGIDOLLAR`): `OP_DIGIDOLLAR` (0xbb), `OP_DDVERIFY` (0xbc), `OP_CHECKPRICE` (0xbd), `OP_CHECKCOLLATERAL` (0xbe), `OP_ORACLE` (0xbf)
- ⚠️ Opcode name mapping in `GetOpName()`

### src/script/interpreter.h / src/script/interpreter.cpp
- ⚠️ `SCRIPT_VERIFY_DIGIDOLLAR` flag enabling DD opcode execution
- ⚠️ `EvalScript` handles OP_DIGIDOLLAR, OP_DDVERIFY, OP_CHECKCOLLATERAL, OP_CHECKPRICE execution

### src/script/script_error.h / src/script/script_error.cpp
- ⚠️ `SCRIPT_ERR_INVALID_DD_AMOUNT` error code and its string mapping

### src/primitives/transaction.h / src/primitives/transaction.cpp
- ⚠️ `CMutableTransaction::SetDigiDollarType(type)` → encodes DD type into nVersion field (bits 24-31 = type, bits 0-15 = 0x0770)
- ⚠️ `CMutableTransaction::IsDigiDollar()` → checks version for DD marker (member function)
- ⚠️ `IsDigiDollarTransaction(tx)` → free function checking DD marker on CTransaction
- ⚠️ `GetDigiDollarTxType(tx)` → extracts DD type from version bits
- ⚠️ `MakeDigiDollarVersion(type, flags)` → constructs DD version field
- ⚠️ `GetDigiDollarTxTypeName(type)` → human-readable DD type name
- ⚠️ DD version constants and helper methods

### src/node/miner.cpp
- ⚠️ Calls `OracleBundleManager::AddOracleBundleToBlock()` to embed oracle data in coinbase during block assembly

### src/consensus/tx_check.cpp
- ⚠️ `CheckTransaction` → defers DD validation to ConnectBlock (context-free, cannot check deployment activation); 0-value P2TR outputs pass the `nValue >= 0` consensus check

### src/consensus/tx_verify.cpp
- ⚠️ Skips DD-specific validation that requires chain state (deferred to ConnectBlock)

### src/consensus/params.h
- ⚠️ `Consensus::DEPLOYMENT_DIGIDOLLAR` buried deployment (BIP90 since v9.26.5): `BuriedDeployment` enumerator + `DigiDollarHeight` via `DeploymentHeight()`; formerly a BIP9 `vDeployments` entry

### src/policy/policy.cpp
- ⚠️ `IsStandardTx` → exempts DD transactions from standard dust/size checks via version marker detection
- ⚠️ Allows 0-value P2TR outputs for DD token transfers

### src/protocol.h
- ⚠️ Oracle CInv / wire message types: `MSG_ORACLE_PRICE` (0x40000000), `MSG_ORACLE_BUNDLE` (0x40000001), `MSG_GET_ORACLE_DATA` (0x40000002), `MSG_ORACLE_CONSENSUS` (0x40000003), `MSG_ORACLE_ATTESTATION` (0x40000004), `MSG_ORACLE_MUSIG_NONCE` (0x40000005), `MSG_ORACLE_MUSIG_PARTIALSIG` (0x40000006), `MSG_ORACLE_MUSIG_CONTEXT` (0x40000007), `MSG_ORACLE_HEARTBEAT` (0x40000008)
- ⚠️ `OracleConsensusMsg` (class) → off-chain oracle consensus proposal: epoch, consensus_price, consensus_timestamp with `GetHash()` for dedup
- ⚠️ `OracleAttestationMsg` (class) → off-chain oracle attestation wrapper: `COraclePriceMessage` signed over consensus values with `GetHash()` for dedup
- ⚠️ `OracleVersionHeartbeatMsg` (class) → signed oracle software/protocol heartbeat: version fields, timestamp, nonce, software/subversion strings, chain-bound signature hash

### src/deploymentinfo.cpp
- ⚠️ `DEPLOYMENT_DIGIDOLLAR` buried-deployment name registration (`DeploymentName(BuriedDeployment)` + `GetBuriedDeployment()` for `-testactivationheight`); removed from `VersionBitsDeploymentInfo[]` in the v9.26.5 burial

### src/core_write.cpp
- ⚠️ DD-aware transaction serialization for `decoderawtransaction` RPC output

### src/common/args.cpp
- ⚠️ `OptionsCategory::DIGIDOLLAR` category definition for CLI args

### src/logging.cpp
- ⚠️ `BCLog::DIGIDOLLAR` log category registration

### src/net_processing.cpp
- ⚠️ Handles `ORACLEPRICE`, accepted-and-dropped `ORACLEBUNDLE`, `ORACLECONSENSUS`, `ORACLEATTESTATION`, `ORACLEMUSIGNONCE`, `ORACLEMUSIGCONTEXT`, `ORACLEMUSIGPARTIALSIG`, `ORACLEHEARTBEAT`, and `GETORACLES`
- ⚠️ Price/consensus/MuSig2/getoracles/heartbeat handlers all use `IsOracleP2PActive`; `ORACLEHEARTBEAT` is also signed, roster-limited, deduplicated, and rate-limited
- ⚠️ `GETORACLES` is rate-limited to 10 requests/minute/peer, accepts epochs in `[current_epoch - 24, current_epoch + 1]`, and replies with matching fresh `ORACLEPRICE` messages plus recent `ORACLEHEARTBEAT` messages
- ⚠️ Oracle message validation, rate limiting, and Misbehaving scoring for invalid MuSig2/heartbeat messages

### src/node/transaction.cpp
- ⚠️ DD-aware transaction broadcast handling, oracle data relay

### src/rpc/client.cpp
- ⚠️ Registers all DD RPC command parameter types (54 `CRPCConvertParam` rows at `src/rpc/client.cpp:306-363`)

### src/rpc/register.h
- ⚠️ Calls `RegisterDigiDollarRPCCommands()` during RPC table setup

### src/kernel/chainparams.h / src/kernel/chainparams.cpp
- ⚠️ `CChainParams::GetDigiDollarParams()` → returns `DigiDollar::ConsensusParams`
- ⚠️ `DIGIDOLLAR_ADDRESS`, `DIGIDOLLAR_ADDRESS_TESTNET`, `DIGIDOLLAR_ADDRESS_REGTEST` version byte constants
- ⚠️ DD consensus params for mainnet, testnet, signet, regtest (88 references)

### src/wallet/wallet.h / src/wallet/wallet.cpp
- ⚠️ `CWallet::GetDDWallet()` → returns `DigiDollarWallet*` accessor
- ⚠️ `SyncTransaction` → calls `ProcessDDTxForRescan()` and `ProcessIncomingDDTransaction()` on DD transactions
- ⚠️ `UnlockAllCoins` → respects `IsLockedByDD()` to protect DD locks

### src/wallet/walletdb.h / src/wallet/walletdb.cpp
- ⚠️ `WalletBatch` DD persistence methods: `WriteDDBalance()`, `WriteDDTimeLock()`, `ReadDDTimeLock()`, `WriteDDTransaction()`, `WriteDDOwnerKey()`, `WriteDDAddressKey()`, `EraseDDTimeLock()` (74 references)
- ⚠️ `WalletBatch` encrypted DD key methods (T4-03a): `WriteCryptedDDOwnerKey()`, `ReadCryptedDDOwnerKey()`, `EraseCryptedDDOwnerKey()`, `WriteCryptedDDAddressKey()`, `ReadCryptedDDAddressKey()`, `EraseCryptedDDAddressKey()`
- ⚠️ DB keys: `DD_CRYPTED_ADDRESS_KEY` ("ddcaddrkey"), `DD_CRYPTED_OWNER_KEY` ("ddcownerkey")

### src/wallet/spend.cpp
- ⚠️ Coin selection excludes DD-locked UTXOs from regular DGB spending

### src/wallet/interfaces.cpp
- ⚠️ Wallet interface extensions for DD balance queries

### src/wallet/rpc/coins.cpp
- ⚠️ DD-aware `lockunspent` protection (prevents unlocking DD collateral/token UTXOs via RPC)

### src/wallet/rpc/wallet.cpp
- ⚠️ Registers the 17 wallet-context DD/oracle RPC commands in the wallet RPC table, including `sendmanydigidollar`, `listdigidollarunspent`, `listdigidollarutxos`, `exportoracleprivkey`, and `importoracleprivkey`

### src/wallet/scriptpubkeyman.h
- ⚠️ Forward declaration of DD key management interface

### src/wallet/types.h
- ⚠️ DD-related wallet type definitions

### src/interfaces/wallet.h
- ⚠️ `interfaces::Wallet` DD balance query methods

---

## Qt GUI

### src/qt/digidollartab.cpp/h
- `DigiDollarTab` → main DD tab widget containing the activation overlay and 7 tabs: DD Overview, Send DD, Receive DD, Mint DD, Redeem DD, DD Vault, DD Transactions

### src/qt/digidollaroverviewwidget.cpp/h
- `DigiDollarOverviewWidget` → DD balance overview, system health display

### src/qt/digidollarmintwidget.cpp/h
- `DigiDollarMintWidget` → mint DD interface with tier selection and collateral calculator

### src/qt/digidollarpositionswidget.cpp/h
- `DigiDollarPositionsWidget` → displays DDTimeLock positions with lock status and health

### src/qt/digidollarsendwidget.cpp/h
- `DigiDollarSendWidget` → send DD to address with amount validation

### src/qt/digidollarreceivewidget.cpp/h
- `DigiDollarReceiveWidget` → generate/display DD receive addresses

### src/qt/digidollarreceiverequest.cpp/h
- `DigiDollarReceiveRequest` → DD payment request generation

### src/qt/digidollarredeemwidget.cpp/h
- `DigiDollarRedeemWidget` → redeem DD interface with path selection

### src/qt/digidollartransactionswidget.cpp/h
- `DigiDollarTransactionsWidget` → DD transaction history display

### src/qt/digidollarcoincontroldialog.cpp/h
- `DigiDollarCoinControlDialog` → DD UTXO selection dialog for advanced users

### src/qt/ddaddressbookpage.cpp/h
- `DDAddressBookPage` → DD address selection/edit dialog used by the send flow

### src/qt/digidollar_qt_translate.h
- `TranslateMintRejectReasonForUser()` → maps DD/oracle mempool reject tokens to Qt user-facing mint errors

---

## Tests

### C++ Unit Tests (`src/test/`)

Representative source inventory is listed below. Authoritative C++ unit
registration is `src/Makefile.test.include`; rows marked source-only are
present in the tree but not compiled into the current unit-test binary.

| File | Coverage Area |
|------|--------------|
| `digidollar_activation_tests.cpp` | Buried-height activation gating and deployment status checks (BIP9 state-machine cases removed in the v9.26.5 burial) |
| `digidollar_activation_wave12_tests.cpp` | Wave 12 activation boundary and deployment predicate regressions |
| `digidollar_address_tests.cpp` | DD address encoding/decoding, version bytes, network-specific prefixes, validation |
| `digidollar_burn_enforcement_tests.cpp` | Collateral-vault burn enforcement and non-DD spend guard coverage |
| `digidollar_change_tests.cpp` | DD change output creation, dust handling, balance conservation in transfers |
| `digidollar_consensus_tests.cpp` | Consensus parameter validation, collateral ratios, tier calculations, DCA levels |
| `digidollar_dca_tests.cpp` | DCA health tiers, multiplier calculations, system health boundaries, rapid transitions |
| `digidollar_err_tests.cpp` | ERR activation/deactivation, adjustment ratios, extra DD burn requirement, mint blocking, and fail-closed behavior |
| `digidollar_health_tests.cpp` | SystemHealthMonitor metrics, tier breakdown, alert thresholds, health history, UTXO scanning |
| `digidollar_health_dca_tests.cpp` | Combined health and DCA state coverage |
| `digidollar_hot_path_logging_tests.cpp` | Hot-path logging safety and spam-boundary coverage |
| `digidollar_locktier_tests.cpp` | Lock-tier parser and canonical tier enforcement |
| `digidollar_mint_tests.cpp` | Mint tx building, collateral calculation, OP_RETURN metadata, NUMS verification, tier/lock validation |
| `digidollar_opcodes_tests.cpp` | OP_DIGIDOLLAR, OP_DDVERIFY, OP_CHECKCOLLATERAL, OP_CHECKPRICE execution in script interpreter |
| `digidollar_oracle_tests.cpp` | Oracle integration with DD: price feeding, bundle creation, consensus price extraction |
| `digidollar_oracle_bundle_matrix_tests.cpp` | V1 oracle bundle reject/accept matrix across missing, malformed, legacy, and valid v0x03 shapes |
| `digidollar_oracle_domain_separation_tests.cpp` | Oracle bundle hash domain separation and chain binding |
| `digidollar_oracle_feed_safety_tests.cpp` | Exchange feed safety, bounds, and stale data handling |
| `digidollar_oracle_feeds_wave11_tests.cpp` | Wave 11 oracle feed regression coverage |
| `digidollar_oracle_musig2_tests.cpp` | MuSig2 oracle bundle signing, bitmap, and validation coverage |
| `digidollar_oracle_quorum_domain_tests.cpp` | Quorum/domain separation invariants for oracle bundles |
| `digidollar_oracle_roster_tests.cpp` | Consensus roster and reserve-slot rejection coverage |
| `digidollar_p2p_tests.cpp` | DD transaction relay, mempool acceptance, network propagation rules |
| `digidollar_persistence_keys_tests.cpp` | DD owner key and address key persistence across wallet restart |
| `digidollar_persistence_serialization_tests.cpp` | DD data structure serialization/deserialization for wallet database |
| `digidollar_persistence_walletbatch_tests.cpp` | WalletBatch DD read/write operations, database integrity |
| `digidollar_redeem_tests.cpp` | Redemption tx building, collateral release, timelock validation, ERR path, full burn requirement |
| `digidollar_redteam_tests.cpp` | Security-focused (~9000+ lines): NUMS bypass, inflation attacks, cross-mint burns, partial burn exploits, RED HORNET audit T5–T10 (activation boundaries, zero-amount ops, MAX_MONEY overflow, mempool ancestor limits, rapid mint/redeem, oracle partition/sybil/eclipse, miner reordering/censorship/timestamp, reorg collateral theft, double-spend) |
| `digidollar_restore_tests.cpp` | Wallet restore from blockchain rescan, position reconstruction, key recovery |
| `digidollar_rpc_tests.cpp` | RPC command validation: mintdigidollar, senddigidollar, redeemdigidollar, getdigidollarbalance |
| `digidollar_rpc_unit_tests.cpp` | DD RPC schema and unit-level request/response validation |
| `digidollar_scripts_tests.cpp` | P2TR script creation, MAST tree construction, NUMS point, Taproot tweak, metadata registry |
| `digidollar_structures_tests.cpp` | CDigiDollarOutput, CCollateralPosition constructors, serialization, validation, equality |
| `digidollar_timelock_tests.cpp` | Lock period calculations, tier boundaries, CLTV enforcement, block-to-days conversion |
| `digidollar_transaction_tests.cpp` | End-to-end tx creation and validation for mint, transfer, redeem with full pipeline |
| `digidollar_transfer_tests.cpp` | DD transfer conservation, multi-recipient, DD change, dust handling, OP_RETURN amounts |
| `digidollar_txbuilder_tests.cpp` | TxBuilder classes: parameter validation, fee calculation, coin selection, output construction |
| `digidollar_utxo_lifecycle_tests.cpp` | DD UTXO tracking from mint through transfer(s) to redemption, spent detection |
| `digidollar_validation_tests.cpp` | ValidateMintTransaction, ValidateTransferTransaction, ValidateRedemptionTransaction with full context |
| `digidollar_volatility_tests.cpp` | Volatility monitoring, freeze triggers, cooldown periods, price history, threshold checks |
| `digidollar_wallet_tests.cpp` | DigiDollarWallet: balance tracking, UTXO management, signing, commit, confirmation tracking |
| `digidollar_t2_05_tests.cpp` | T2-05 task-specific tests for DD validation edge cases |
| `digidollar_key_encryption_tests.cpp` | DD owner key and address key encryption/decryption with wallet encryption |
| `digidollar_skip_oracle_tests.cpp` | DD validation with skipOracleValidation flag for historical block processing |
| `digidollar_musig2_session_state_tests.cpp` | MuSig2 session state persistence and transition coverage |
| `digidollar_wallet_hd_tests.cpp` | HD wallet owner-key derivation and DD wallet key management |
| `digidollar_wave13_parity_tests.cpp` | Wave 13 mainnet/testnet/regtest parity coverage |
| `digidollar_wave14_ibd_security_tests.cpp` | IBD and historical DD/oracle validation security coverage |
| `digidollar_wave14_reorg_replay_tests.cpp` | Reorg replay behavior for DD/oracle state |
| `digidollar_wave15_parser_tests.cpp` | Wave 15 parser hardening unit coverage |
| `digidollar_wave18_rpc_schema_tests.cpp` | Wave 18 DD RPC schema regression coverage |
| `digidollar_wave20_p2p_pending_tests.cpp` | Oracle P2P pending-message and recovery coverage |
| `digidollar_wave21_dos_resource_tests.cpp` | DD/oracle DoS and resource-bound coverage |
| `digidollar_wave26_compat_tests.cpp` | Mixed-node compatibility coverage for DD/oracle activation surfaces |
| `digidollar_wave6_health_dca_volatility_tests.cpp` | Combined health, DCA, and volatility regression coverage |
| `oracle_block_validation_tests.cpp` | Oracle data validation during block processing, bundle extraction from coinbase |
| `oracle_bundle_manager_tests.cpp` | Bundle creation, message aggregation, epoch management, consensus price calculation |
| `oracle_consensus_threshold_tests.cpp` | Oracle consensus threshold validation, minimum message requirements |
| `oracle_config_tests.cpp` | Oracle configuration validation, parameter bounds, consensus thresholds |
| `oracle_exchange_tests.cpp` | Exchange fetcher HTTP/JSON parsing, multi-exchange aggregation, outlier filtering |
| `oracle_integration_tests.cpp` | End-to-end oracle → DD integration: price feed through to collateral calculation |
| `oracle_message_tests.cpp` | COraclePriceMessage signing, verification, Schnorr signatures, conflict detection |
| `oracle_miner_tests.cpp` | Oracle bundle embedding in coinbase, miner integration |
| `oracle_p2p_tests.cpp` | Oracle P2P message validation, rate limiting, DOS protection |
| `oracle_phase2_tests.cpp` | Legacy Phase 2 / MuSig2 oracle validation regressions: on-chain format, signature hash changes, multi-oracle Schnorr consensus |
| `redteam_phase2_audit_tests.cpp` | **RED HORNET legacy Phase 2** exploit regressions: Schnorr sig bypass, selective price inclusion, consensus fork vectors, oracle identity attacks, signature replay, version downgrade, IQR outlier gaming, consensus price determinism |
| `oracle_rpc_tests.cpp` | Oracle RPC commands: getoracleprice, createoraclekey, export/import oracle private key, startoracle |
| `oracle_wallet_key_tests.cpp` | Oracle key generation, storage, validation against chainparams |
| `oracle_bundle_timing_tests.cpp` | Oracle bundle timing edge cases and epoch boundary behavior |
| `oracle_price_feed_rh09_tests.cpp` | RH-09 oracle price feed attack vectors and edge cases |
| `oracle_price_staleness_tests.cpp` | Oracle price staleness detection and stale price rejection |
| `oracle_wallet_autostart_tests.cpp` | Oracle wallet auto-start behavior on node initialization |
| `digidollar_err_attack_tests.cpp` | RH-27: ERR path attack tests — calculation exploits, volatility freeze bypass, TOCTOU races |
| `digidollar_integration_attack_tests.cpp` | Integration-level attack tests across DD subsystems |
| `digidollar_lock_height_tests.cpp` | Lock height calculation, validation, and edge cases |
| `digidollar_no_partial_redeem_tests.cpp` | Enforcement of full DD burn requirement — no partial redemption allowed |
| `digidollar_script_attacks_tests.cpp` | RH-38: Script interpreter DD opcode exploit tests (security audit round 2) |
| `digidollar_txindex_tests.cpp` | DD transaction index lookups and block-db integration |
| `digidollar_rh06_mint_attacks_tests.cpp` | RH-06: Mint transaction attack vectors |
| `digidollar_rh07_redemption_attacks_tests.cpp` | RH-07: Redemption transaction attack vectors |
| `digidollar_rh11_consensus_tests.cpp` | RH-11: Deep consensus edge cases — adversarial red-team tests |
| `digidollar_rh12_script_attacks_tests.cpp` | RH-12: Script-level attack vectors for DD opcodes |
| `digidollar_rh13_economic_tests.cpp` | RH-13: Economic attack vectors (arbitrage, manipulation) |
| `digidollar_rh16_reorg_attacks_tests.cpp` | RH-16: Reorg attack vectors targeting DD state |
| `digidollar_rh17_mempool_attacks_tests.cpp` | RH-17: Mempool-level DD attack vectors |
| `digidollar_rh18_cross_feature_tests.cpp` | RH-18: Cross-feature interaction attack tests |
| `digidollar_rh19_serialization_tests.cpp` | RH-19: Serialization edge cases and malformed data |
| `digidollar_rh20_time_ordering_tests.cpp` | RH-20: Time-dependent validation and tx ordering attacks |
| `digidollar_rh21_boundary_tests.cpp` | RH-21: Boundary value and off-by-one tests |
| `digidollar_rh25_serialization_cache_tests.cpp` | RH-25: Serialization cache consistency and invalidation |
| `digidollar_rh26_tests.cpp` | RH-26: Additional red-team tests |
| `digidollar_rh28_wallet_chains_tests.cpp` | RH-28: Wallet chain interaction and state consistency |
| `digidollar_rh31_consensus_fork_tests.cpp` | RH-31: Consensus fork attack vectors (testnet/mainnet confusion) |
| `digidollar_rh32_collateral_dca_tests.cpp` | RH-32: Collateral and DCA interaction edge cases |
| `digidollar_rh33_mempool_relay_tests.cpp` | RH-33: Mempool relay and propagation attack vectors |
| `digidollar_rh34_multiblock_state_tests.cpp` | RH-34: Multi-block state transition attacks |
| `digidollar_rh35_chaos_tests.cpp` | RH-35: Chaos/fuzz-style randomized testing |
| `digidollar_rh40_regression_tests.cpp` | RH-40: Regression tests for previously found bugs |
| `digidollar_rh41_timewarp_difficulty_tests.cpp` | RH-41: Time-warp and 5-algo difficulty interaction attacks |
| `digidollar_rh42_formal_invariant_tests.cpp` | RH-42: Formal invariant verification tests |
| `digidollar_rh43_digiassets_interaction_tests.cpp` | RH-43: DigiAssets interaction and coexistence tests |
| `digidollar_rh44_thread_safety_tests.cpp` | RH-44: Thread safety and concurrent access tests |
| `digidollar_rh46_rpc_input_validation_tests.cpp` | RH-46: RPC input validation and DoS surface tests |
| `digidollar_rh47_consensus_fork_deep_tests.cpp` | RH-47: Consensus fork scenario deep dive (builds on RH-31) |
| `digidollar_rh49_find_opreturn_tests.cpp` | Registered RH-49: FindDDOpReturn helper validation and dynamic OP_RETURN detection |
| `rh05_bundle_validation_attacks_tests.cpp` | RH-05: oracle bundle validation — v0x02 downgrade, epoch mismatch, oversized data, bitmap attacks, zero-length bypass |
| `rh15_crypto_primitives_tests.cpp` | RH-15: hash domain separation, __int128 edge cases, version-marker ambiguity, MuSig2 nonce/key validation |
| `rh29_coinbase_oracle_manipulation_tests.cpp` | RH-29: coinbase OP_RETURN injection, multiple oracle bundles, version confusion, signature replay, withholding |
| `rh39_eclipse_attack_tests.cpp` | RH-39: eclipse + oracle suppression, selective relay, message ordering, INV/GETDATA withholding, sybil spoofing |
| `rh50_oracle_keyset_alignment_tests.cpp` | RH-50: oracle keyset alignment invariant — vOracleNodes ↔ vOraclePublicKeys slot 0-34 ordering |
| `rh51_checkphase3_v1_split_tests.cpp` | RH-51: regtest activation-gate asymmetry hardening (regtest/mainnet divergence at heights 0–649) |
| `rh52_bip34_scriptnum_escape_tests.cpp` | RH-52: BIP34 coinbase-height CScriptNum escape in oracle validators (Wave-1 PoC; fixed in `2b37384e79`) |
| `rh53_op_checkprice_mock_weaponization_tests.cpp` | RH-53: OP_CHECKPRICE mock-price weaponization regression; current code keeps `OP_CHECKPRICE` reserved and deterministically disabled |
| `rh54_op_oracle_opsuccess_tests.cpp` | RH-54: OP_ORACLE incorrectly classified OP_SUCCESS in Tapscript (Wave-2 PoC; fixed in `20d56c34da`) |
| `rh55_musig2_partial_sig_unverified_aggregation_tests.cpp` | RH-55: MuSig2 partial-sig aggregation accepted unverified scalars (Wave-3 PoC; fixed in `986eca83ce`) |
| `rh56_oversized_bitmap_message_inflation_tests.cpp` | RH-56: oversized participation-bitmap inflates `bundle.messages` before consensus (Wave-4 PoC) |
| `rh57_musig2_trim_aggregate_toctou_tests.cpp` | RH-57: TOCTOU between TrimNoncesToThreshold and AggregateNonces; pubnonce injection (Wave-5 documentation) |
| `rh58_pending_partialsigs_unbounded_growth_tests.cpp` | RH-58: `m_pending_partialsigs` unbounded growth DoS (Wave-6; capped/pruned in `35561d598b`) |
| `rh60_mempool_dd_scriptnum_escape_tests.cpp` | RH-60: unhandled CScriptNum escape on mempool DD path (Wave-8; wrapped in `c66c853479`) |
| `rh61_coinbase_price_cache_poisoning_tests.cpp` | RH-61: miner coinbase OP_ORACLE price-cache poisoning (Wave-9; UpdatePriceCache gated on BIP9 in `fd1ac41424`) |
| `rh62_senddigidollar_amount_parser_tests.cpp` | RH-62: senddigidollar amount-parser pathology — exception-unsafe `std::stod`, NaN/Inf-to-int64 UB cast (Wave-10) |
| `rh63_oracle_validator_escape_hatches_tests.cpp` | RH-63: ValidateBlockOracleData "transition period" escape-hatch weaponization (Wave-11; W1-M-05) |
| `rh64_dca_table_disagreement_tests.cpp` | RH-64: DCA multiplier table disagreement (Wave-12 H2 weaponization) |
| `rh65_mainnet_testnet_validator_parity_tests.cpp` | RH-65: mainnet ≡ testnet oracle block validation parity (mainnet short-circuit removed in `f0d9a7b2c7`) |
| `musig2_basic_tests.cpp` | MuSig2 basic key aggregation and signing protocol |
| `musig2_session_tests.cpp` | MuSig2 signing session state machine lifecycle |
| `musig2_aggregator_tests.cpp` | MuSig2 oracle key aggregation, bitmap encoding, cache |
| `musig2_orchestration_tests.cpp` | MuSig2 session orchestrator per-epoch management |
| `musig2_orchestrator_exploits_tests.cpp` | MuSig2 orchestrator exploit and attack vectors |
| `musig2_signing_orchestration_tests.cpp` | OracleSigningOrchestrator BlockConnected-driven signing |
| `musig2_oracle_node_tests.cpp` | MuSig2 oracle node participation and nonce generation |
| `musig2_activation_tests.cpp` | MuSig2/Phase 3 activation height and feature gating |
| `musig2_phase3_activation_tests.cpp` | Phase 3 activation boundary and transition tests |
| `musig2_p2p_message_tests.cpp` | MuSig2 P2P nonce and partial sig message serialization |
| `musig2_p2p_handling_tests.cpp` | MuSig2 P2P message handling in net_processing |
| `musig2_p2p_collection_tests.cpp` | MuSig2 nonce/sig collection from P2P peers |
| `musig2_p2p_ingestion_tests.cpp` | MuSig2 P2P message ingestion and validation |
| `musig2_p2p_network_attacks_tests.cpp` | MuSig2 P2P network-level attack vectors |
| `musig2_net_processing_tests.cpp` | MuSig2 net_processing integration tests |
| `musig2_bundle_creation_tests.cpp` | MuSig2 v0x03 bundle creation from completed sessions |
| `musig2_bundle_format_tests.cpp` | MuSig2 bundle serialization format and version handling |
| `musig2_bundle_manager_tests.cpp` | MuSig2 bundle manager integration with signing sessions |
| `musig2_bundle_mining_tests.cpp` | MuSig2 bundle embedding in coinbase during mining |

### Wallet Tests (`src/wallet/test/`)

| File | Coverage Area |
|------|--------------|
| `digidollar_persistence_wallet_tests.cpp` | Full wallet DD persistence: balances, positions, transactions, keys across restart |
| `digidollar_wallet_security_tests.cpp` | Wallet-level DD security: key protection, unauthorized access, encryption boundaries |
| `rh59_coincontrol_dd_lock_bypass_tests.cpp` | RH-59: coin-control / lockunspent bypass on preset DD inputs (W7; partially reverted in `ce0abf4e3a`) |

### Qt Tests (`src/qt/test/`)

| File | Coverage Area |
|------|--------------|
| `digidollarwidgettests.cpp/h` | Qt widget unit tests for DD UI components |
| `digidollarwave19widgettests.cpp/h` | Wave 19 Qt unit/signal-slot pins for the release-critical DD UX surface (mint tier dropdown, etc.) |

### Python Functional Tests (`test/functional/`)

Authoritative registration is `test/functional/test_runner.py:261-340`.
As of the Wave 23 rerun the standard runner contains 80 DD/oracle/wallet
functional entries. `feature_oracle_p2p.py` remains registered for historical
compatibility but is a legacy/superseded scaffold; the live oracle P2P proof is
`digidollar_wave20_oracle_p2p.py`.

| File | Coverage Area |
|------|--------------|
| `digidollar_activation.py` | Basic activation of DD features at the buried height on regtest (BIP9 signaling lifecycle removed in the v9.26.5 burial) |
| `digidollar_activation_boundary.py` | Activation edge cases: exact height, off-by-one, pre/post activation behavior |
| `digidollar_activation_multinode.py` | Multi-node activation state and deployment synchronization |
| `digidollar_basic.py` | End-to-end DD workflow: mint → transfer → redeem on regtest |
| `digidollar_collateral_spend_guards.py` | Mempool/RPC/submitblock collateral-spend burn-enforcement guards |
| `digidollar_encrypted_wallet.py` | DD operations with encrypted wallet: unlock, mint, send, lock |
| `digidollar_getoracles_consensus_field.py` | `getoracles` consensus field and pending-message status regression coverage |
| `digidollar_listunspent.py` | DD-aware `listunspent` output and wallet UTXO reporting |
| `digidollar_lock_tier_canonical.py` | Canonical lock tier acceptance/rejection in wallet/functional flows |
| `digidollar_mint.py` | Mint transaction creation, collateral validation, tier selection |
| `digidollar_musig2_session_status.py` | MuSig2 oracle session status and aggregate signing progress |
| `digidollar_network_relay.py` | DD transaction propagation across multi-node network |
| `digidollar_network_tracking.py` | Network-wide DD supply tracking and consistency |
| `digidollar_oracle.py` | Basic oracle price feeding and DD integration |
| `digidollar_oracle_block_rules_relay.py` | DD mint/redeem oracle-bundle relay/mining rules and transfer-only omission behavior |
| `digidollar_oracle_keygen.py` | Oracle key generation and authorization |
| `digidollar_oracle_bundle_reject_matrix.py` | Oracle bundle reject matrix: malformed, stale, missing, legacy formats |
| `digidollar_oracle_reorg_cache.py` | Oracle cache behavior across reorgs |
| `digidollar_oracle_rpc_staleness.py` | Oracle RPC stale-price/status display regression coverage |
| `digidollar_persistence.py` | DD data persistence across node restart |
| `digidollar_mempool_miner_parity.py` | DD mempool/miner/ConnectBlock parity for oracle quote and validation paths |
| `digidollar_pending_position_status.py` | Pending DD position/balance status reporting |
| `digidollar_protection.py` | DCA, ERR, and volatility protection mechanisms |
| `digidollar_redeem.py` | Basic redemption: timelock expiry, collateral release |
| `digidollar_redeem_stats.py` | Redemption statistics tracking via DigiDollarStatsIndex |
| `digidollar_redemption_amounts.py` | Redemption amount calculations: full, ERR-adjusted, fee deductions |
| `digidollar_redemption_e2e.py` | End-to-end redemption: mint → wait → redeem → verify balance |
| `digidollar_rpc.py` | General DD RPC command testing |
| `digidollar_rpc_addresses.py` | getdigidollaraddress, validateddaddress, listdigidollaraddresses |
| `digidollar_rpc_amount_filters.py` | Fixed-precision DD cent parsing and amount-filter regressions |
| `digidollar_rpc_collateral.py` | calculatecollateralrequirement, estimatecollateral |
| `digidollar_rpc_dca.py` | getdcamultiplier, DCA-related RPC |
| `digidollar_rpc_deployment.py` | DD deployment status RPC queries |
| `digidollar_rpc_estimate.py` | Collateral estimation RPC with various parameters |
| `digidollar_rpc_gating.py` | RPC command availability gating based on activation status |
| `digidollar_rpc_oracle.py` | getoracleprice, setmockoracleprice, oracle status |
| `digidollar_rpc_protection.py` | getprotectionstatus, DCA/ERR/volatility via RPC |
| `digidollar_rpc_redemption.py` | redeemdigidollar, getredemptioninfo |
| `digidollar_stress.py` | Stress testing: high tx volume, many positions, rapid mints/transfers |
| `digidollar_stats_reorg.py` | DigiDollar stats index behavior across reorgs |
| `digidollar_stats_reordered_mint.py` | Stats index handling for reordered mint outputs |
| `digidollar_transactions.py` | Transaction structure validation, version markers, OP_RETURN parsing |
| `digidollar_transfer.py` | DD transfer: single/multi recipient, conservation, change outputs |
| `digidollar_tx_amounts_debug.py` | Debugging tool for DD amount extraction and validation |
| `digidollar_verifychain_cache_side_effect.py` | Proves `verifychain` memory-only checks do not mutate live DD/oracle caches |
| `digidollar_wallet.py` | Wallet DD integration: balance, history, UTXO management |
| `digidollar_watchonly_rescan.py` | Watch-only wallet DD rescan and position detection |
| `digidollar_rpc_display_bugs.py` | RPC display formatting bug regression tests |
| `wallet_digidollar_backup.py` | DD wallet backup and restore from backup file |
| `wallet_digidollar_descriptors.py --descriptors` | Descriptor wallet compatibility with DD keys |
| `wallet_digidollar_encryption.py` | Wallet encryption impact on DD operations |
| `wallet_digidollar_persistence_restart.py --descriptors` | DD data survival across wallet/node restart cycles |
| `wallet_digidollar_rc33_regressions.py` | RC33 DD wallet regression coverage |
| `wallet_digidollar_rescan.py` | Wallet rescan reconstructs DD positions from blockchain |
| `digidollar_bug11_bug13_regression.py` | Bug #11 and #13 regression tests (fee display, listdigidollartxs) |
| `digidollar_oracle_consistency.py` | Oracle price consistency across multiple nodes |
| `digidollar_oracle_price.py` | Oracle price feed RPC and integration |
| `digidollar_protection_status.py` | getprotectionstatus RPC output validation |
| `digidollar_rpc_position_fields.py` | listdigidollarpositions field completeness and correctness |
| `digidollar_send.py` | senddigidollar RPC end-to-end testing |
| `digidollar_transaction_fees.py` | DD transaction fee calculation and display |
| `digidollar_validate_address.py` | validateddaddress RPC address format validation |
| `digidollar_wallet_restore_redeem.py` | Wallet restore followed by redemption of restored positions |
| `wallet_digidollar_restore.py --descriptors` | Full wallet restore from seed with DD position reconstruction |
| `feature_oracle_p2p.py` | Legacy/superseded oracle P2P scaffold retained in the runner; real coverage is `digidollar_wave20_oracle_p2p.py` |
| `digidollar_wave14_multinode_ibd_reorg.py` | Multi-node IBD/reorg replay for DD/oracle state |
| `digidollar_wave17_spendability.py --descriptors` | Descriptor-wallet DD spendability/watch-only/locked-wallet regression coverage |
| `digidollar_wave18_rpc_matrix.py --descriptors` | DD RPC matrix across wallet states, networks, and activation gates |
| `digidollar_wave20_oracle_p2p.py` | Oracle P2P pending-bundle flow and recovery |
| `digidollar_wave21_musig2_p2p_dos.py` | MuSig2 nonce/partial-sig stale-epoch relay rejection and current+1 boundary liveness |
| `digidollar_wave21_dos_paging.py` | DD RPC paging/resource-bound functional coverage |
| `digidollar_wave26_mixed_node_compat.py` | Mixed upgraded/legacy-behavior compatibility proof for ordinary DGB transactions |
| `wallet_digidollar_active_restore_redeem.py` | Active DD restore followed by redemption |
| `wallet_digidollar_encrypted_received_redeem.py` | Encrypted wallet receive/redeem path |
| `wallet_digidollar_mint_reorg.py` | Wallet DD mint state across reorgs |
| `wallet_digidollar_mixed_output_accounting.py` | Wallet accounting for mixed DD/non-DD outputs |
| `wallet_digidollar_pending_redeem_restart.py` | Pending redemption state across wallet/node restart |
| `wallet_digidollar_reindex.py` | DD wallet state reconstruction during reindex |
| `wallet_digidollar_reorg.py` | Wallet DD position/balance state across reorgs |
| `wallet_digidollar_transfer_ancestor_reorg.py` | DD transfer ancestor reorg replay |
| `wallet_digidollar_transfer_reorg.py` | DD transfer reorg replay |
| `wallet_digidollar_wave16_load_rescan.py` | Wave 16 wallet load/rescan persistence coverage |

### Fuzz Targets (`src/test/fuzz/`)

Wave 23 registered 247 total fuzz targets in the active fuzz binary. Of those,
52 target names currently match DigiDollar/oracle/MuSig2/DD surfaces. The tree
contains additional DD/oracle fuzz source files and helpers; authoritative
target registration is the `PRINT_ALL_FUZZ_TARGETS_AND_ABORT=1` output from
`src/test/fuzz/fuzz` plus `src/Makefile.test.include`.

Current DigiDollar-specific fuzz source inventory:

| File | Coverage Area |
|------|--------------|
| `digidollar_consensus.cpp` | DD consensus parameter and transaction-shape invariants |
| `digidollar_dca_volatility.cpp` | DCA and volatility interaction boundaries |
| `digidollar_health.cpp` | System health calculations and alert thresholds |
| `digidollar_integer_math.cpp` | Integer/overflow/rounding behavior for DD math |
| `digidollar_lock_tier.cpp` | Canonical lock-tier parsing and boundary handling |
| `digidollar_logic.cpp` | Core DD transaction logic and parser behavior |
| `digidollar_scripts.cpp` | DD script construction/parsing and opcode-facing surfaces |
| `digidollar_txbuilder.cpp` | TxBuilder parameter and construction fuzzing |
| `digidollar_txbuilder_validate.cpp` | TxBuilder output validation and reject-path fuzzing |
| `digidollar_validation_deep.cpp` | Deep mint/transfer/redeem validation fuzzing |
| `digidollar_wave15_parsers.cpp` | Wave 15 parser hardening for DD metadata and script formats |

Current oracle/MuSig2 fuzz source inventory:

| File | Coverage Area |
|------|--------------|
| `oracle_bundle_hash_domain_sep.cpp` | Oracle bundle hash domain separation and chain binding |
| `oracle_bundle_validation.cpp` | Oracle bundle validation and malformed-input handling |
| `oracle_bundle_version_reject.cpp` | Legacy/unknown oracle bundle version rejection |
| `oracle_id_bitmap_mutations.cpp` | Oracle ID bitmap mutation and signer-set invariants |
| `oracle_musig2_aggregation.cpp` | MuSig2 aggregation and signature assembly behavior |
| `oracle_musig2_auth_signature_domain.cpp` | Auth-signature domain separation for MuSig2 relay messages |
| `oracle_musig2_bitmap.cpp` | MuSig2 participation bitmap parsing |
| `oracle_musig2_bitmap_invariants.cpp` | Bitmap quorum and active-roster invariants |
| `oracle_musig2_bundle.cpp` | v0x03 MuSig2 bundle serialization and validation |
| `oracle_musig2_nonce_msg.cpp` | MuSig2 nonce message parsing/authentication |
| `oracle_musig2_partialsig_msg.cpp` | MuSig2 partial-signature message parsing/authentication |
| `oracle_musig2_session_manager_drive.cpp` | Session manager drive/state transitions |
| `oracle_musig2_session_real.cpp` | Real secp256k1 MuSig2 session paths |
| `oracle_musig2_session_state.cpp` | Session state machine persistence and invalid transitions |
| `oracle_p2p_wire_messages.cpp` | Oracle P2P wire-message serialization and bounds |
| `oracle_price_aggregator.cpp` | Multi-exchange price aggregation and outlier handling |
| `oracle_price_message.cpp` | `COraclePriceMessage` parsing, signing, and validation |
| `oracle_script_parsing.cpp` | OP_ORACLE script parsing and malformed payload handling |
| `oracle_validate_block_data.cpp` | `ValidateBlockOracleData` block-level oracle reject paths |
