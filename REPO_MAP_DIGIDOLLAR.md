# REPO_MAP_DIGIDOLLAR.md — DigiDollar + Oracle Subsystem v9.26

*Generated: 2026-02-12*

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
- `MAX_DIGIDOLLAR` → static constant: 21 billion DGB equivalent in cents (21B × 100), hard cap on total DD supply
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
- `DigiDollar::IsDigiDollarEnabled(pindexPrev, chainman)` → checks BIP9 DEPLOYMENT_DIGIDOLLAR activation via DeploymentActiveAfter
- `DigiDollar::IsDigiDollarEnabled(pindexPrev, params)` → overload using consensus params + temporary VersionBitsCache

### src/digidollar/digidollar.cpp
- Implementation of all `CDigiDollarOutput` and `CCollateralPosition` methods
- `DigiDollar::IsDigiDollarEnabled()` → both overloads delegate to `DeploymentActiveAfter` for BIP9 checking

### src/digidollar/health.h
- `DigiDollar::SystemMetrics` (struct) → aggregates all system-wide DD health data: supply, collateral, per-tier breakdown, DCA/ERR/volatility status, oracle status
  - `TierMetrics` (nested struct) → per-tier stats: lockDays, ddMinted, dgbLocked, positions count, healthRatio
- `DigiDollar::AlertThresholds` (struct) → static constexpr thresholds for alerts: MAX_DD_SUPPLY (100M), MIN_HEALTH_RATIO (120%), CRITICAL (110%), MIN_ORACLES (5), MAX_VOLATILITY (30%), STALE_ORACLE_BLOCKS (100)
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

### src/digidollar/scripts.h
- `DigiDollar::COLLATERAL_NUMS_POINT_BYTES` → BIP-341 NUMS (Nothing Up My Sleeve) point bytes: provably unspendable key for Taproot internal key, prevents key-path spending that bypasses CLTV
- `DigiDollar::GetCollateralNUMSKey()` → returns XOnlyPubKey from NUMS bytes
- `DigiDollar::MintParams` (struct) → parameters for minting: ddAmount, lockHeight, ownerKey, internalKey, oracleKeys vector
- `DigiDollar::CreateCollateralP2TR(params)` → creates Taproot output with 2-leaf MAST: normal redemption (CLTV + owner sig) and ERR path (CLTV + OP_CHECKCOLLATERAL + OP_DIGIDOLLAR + owner sig); uses TaprootBuilder with leaf version 0xC0
- `DigiDollar::CreateDigiDollarP2TR(owner, ddAmount)` → creates simple key-path-only P2TR for freely transferable DD tokens, applies standard Taproot tweak
- `DigiDollar::GetOracleKeys(count)` → generates deterministic oracle keys for Phase 1 testing (Phase 2 connects to real oracles)
- `DigiDollar::CreateNormalRedemptionPath(params)` → script: `<lockHeight> OP_CLTV OP_DROP <ownerKey> OP_CHECKSIG`
- `DigiDollar::CreateERRPath(params)` → script: `<lockHeight> OP_CLTV OP_DROP OP_CHECKCOLLATERAL <100> OP_LESSTHAN OP_VERIFY OP_DIGIDOLLAR OP_DDVERIFY <ownerKey> OP_CHECKSIG`
- `DigiDollar::RegisterScriptMetadata(script, type, ddAmount, lockHeight)` → Phase 1 workaround: stores script→metadata mapping in global map (max 10k entries, FIFO eviction)
- `DigiDollar::GetScriptMetadata(script, metadata)` → retrieves metadata by script hash from global registry

### src/digidollar/scripts.cpp
- Implementation of all script creation functions
- `CreateCollateralP2TR` finalizes TaprootBuilder with 2 leaves at depth 1, registers metadata
- `CreateDigiDollarP2TR` applies standard BIP-341 tweak (nullptr merkle root) for key-path only spending
- Global `g_scriptMetadataMap` protected by `RecursiveMutex`, capped at 10,000 entries

### src/digidollar/txbuilder.h
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

### src/digidollar/txbuilder.cpp
- Full implementation of all TxBuilder classes (~1,425 lines)
- Mint tx: OP_RETURN format = `OP_RETURN <"DD"> <1> <ddAmount> <lockHeight> <lockTier> <ownerXOnlyPubKey>`
- Transfer tx: OP_RETURN format = `OP_RETURN <"DD"> <2> <amount1> <amount2> ... <amountN>`
- Redeem tx: OP_RETURN format = `OP_RETURN <"DD"> <3> <ddChangeAmount>`
- Uses `__int128` for collateral calculations to prevent overflow at large amounts
- MAX_TX_INPUTS = 400 prevents exceeding MAX_STANDARD_TX_WEIGHT

### src/digidollar/validation.h
- `DigiDollar::DD_TX_VERSION` → 0x44440000 marker in transaction version field
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
  - `ValidateEmergencyRedemption(script, sigs)` → validates 8-of-15 oracle signature threshold
  - `ValidateERRRedemption(script, systemCollateral)` → checks system < 100% collateralized
- **Amount/Collateral Validation:**
  - `ValidateMintAmount(amount, params, nHeight)` → validates against min/max with activation height awareness
  - `ValidateOutputAmount(amount, params)` → validates ≥ minOutputAmount and ≤ MAX_DIGIDOLLAR
  - `ValidateCollateralRatio(dgbLocked, ddMinted, lockTime, ctx)` → calculates required collateral, compares with actual, logs detailed breakdown
  - `CalculateRequiredCollateral(ddAmount, lockTime, ctx)` → uses __int128: (ddAmount × COIN × effectiveRatio × 100) / oraclePriceMicroUSD
  - `GetEffectiveCollateralRatio(baseRatio, systemCollateral, params)` → applies DCA multiplier to base ratio
- **Transaction Type Validation:**
  - `ValidateMintTransaction(tx, ctx, state)` → comprehensive: structural checks, volatility freeze, output analysis, OP_RETURN parsing (type/amount/lockHeight/lockTier/ownerPubKey), SECURITY: lock tier↔height consistency, NUMS key reconstruction + verification [T1-04], single collateral output [T1-04c], single DD output (inflation attack prevention), single OP_RETURN [T1-04f], collateral ratio validation using relative lock period [T2-01]
  - `ValidateTransferTransaction(tx, ctx, state)` → DD conservation: extracts amounts from OP_RETURN, validates P2TR outputs, looks up input DD amounts via txindex → block-db → metadata registry → coins view, enforces strict inputDD == outputDD
  - `ValidateRedemptionTransaction(tx, ctx, state)` → validates collateral + DD inputs, burns check (totalDDInputs > totalDDOutputs), path routing by system health, delegates to normal/ERR conditions
- **Redemption Helpers:**
  - `ValidateNormalRedemptionConditions(tx, ctx, state)` → checks nHeight ≥ nLockTime and system health ≥ 100%
  - `ValidateEmergencyRedemptionConditions(tx, ctx, state)` → validates ERR: timelock expired, system < 100%, calculates DD burn multiplier; currently rejects (RED phase incomplete)
  - `ValidateCollateralReleaseAmount(tx, ctx, ddBurned, state)` → SECURITY [T1-08]: looks up original DD via registry → txindex → block-db, REJECTS if unknown; SECURITY [T2-03]: requires full DD burn (ddBurned ≥ originalDDMinted) to prevent cross-mint collateral theft; validates total DGB release ≤ locked + tolerance
  - `ValidateScriptPathSpending(tx, ctx, state)` → Phase 1: allows key-path Schnorr spending
  - `ValidateCollateralOutput(output, tx, state)` → checks P2TR format via Solver, value ≥ 546 sats dust
  - `ValidateDDOutput(output, tx, state)` → checks nValue == 0, P2TR format via Solver
  - `ExtractLockTime(script)` → parses script for OP_CLTV, extracts lock period, defaults to 30 days
  - `GetSystemCollateralRatio()` → placeholder returning 150% (Phase 1)
- **ERR Validation:**
  - `ValidateERRRedemption(tx, ctx, state)` → framework: checks ERR active, placeholder DD burn/collateral calculation; currently returns "err-validation-incomplete"
  - `ShouldBlockMintingDuringERR(ctx)` → delegates to ERR::EmergencyRedemptionRatio::ShouldBlockMinting()
  - `ShouldBlockNormalRedemptionsDuringERR(ctx)` → checks ERRState.isActive
  - `ValidateERRAdjustmentAmount(original, adjusted, systemHealth)` → verifies ERR ratio within tolerance
  - `ValidateERROracleConsensus(tx, ctx)` → placeholder: always returns false (oracle consensus not implemented)
  - `CalculateExpectedERRAdjustment(systemHealth)` → delegates to ERR system

### src/digidollar/validation.cpp
- Full implementation (~2,049 lines)
- `ExtractDDAmountFromTxRef()` (static) → shared helper: parses OP_RETURN with type-aware field extraction (MINT: only first value is DD amount; TRANSFER: all values are DD amounts); SECURITY: verifies source tx has DD marker to prevent DD-from-nothing attacks
- `ExtractDDAmountFromBlockDb()` (static) → universal fallback: loads creating tx from block database using coin height
- `ValidationCache` (struct) → thread-safe cache for script types and amounts (max 10k entries)

---

## Consensus Rules

### src/consensus/digidollar.h
- `DigiDollar::BLOCKS_PER_DAY` → 5760 blocks (15-second block time)
- `DigiDollarTxType` (enum) → DD_TX_NONE(0), DD_TX_MINT(1), DD_TX_TRANSFER(2), DD_TX_REDEEM(3), DD_TX_MAX(4)
- `DigiDollar::ConsensusParams` (struct) → collateral ratios map (1h:1000%, 30d:500%, 90d:400%, 180d:350%, 1y:300%, 2y:275%, 3y:250%, 5y:225%, 7y:212%, 10y:200%), mint limits ($100–$100k), minOutputAmount ($1), oracle config (30 total, 15 active, 8-of-15 threshold), DCA levels
- `GetCollateralRatioForLockTime(lockBlocks, params)` → returns collateral ratio % for lock period; uses higher ratio for between-tier periods
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
  - `GetDCAMultiplier(systemHealth)` → >150%: 1.0×, 120–150%: 1.2×, 100–120%: 1.5×, <100%: 2.0×
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
  - `FormatDCAMultiplier(multiplier)` → formatted multiplier string (e.g. "1.2x")
  - `HandleRapidTransition()`, `ValidateExtremeValues()`, `ValidateMultiplierPrecision()`, `PreventIntegerOverflow()`, `HandleConcurrentUpdates()`, `VerifyMemoryStability()`, `ValidateErrorHandling()`, `IsStateTransitionTracked()`, `HasHysteresis()`, `TrackSystemRecovery()`, `ValidateConcurrentCalculations()`, `SimulateResourceExhaustion()` → extreme scenario testing functions

### src/consensus/dca.cpp
- Implementation of DCA system with 4 health tiers
- Thread-safe health calculations

### src/consensus/err.h
- `DigiDollar::ERR::ERRState` (struct) → isActive, systemHealth, adjustmentRatio (0.8–1.0), activationHeight, oracleConsensusHash, activationTimestamp
- `DigiDollar::ERR::EmergencyRedemptionRatio` (class) → emergency protection when system < 100% collateralized
  - `ShouldActivateERR(systemHealth)` → true if health < 100%
  - `CalculateERRAdjustment(systemHealth)` → 95–100%: 0.95, 90–95%: 0.90, 85–90%: 0.85, <85%: 0.80
  - `GetRequiredDDBurn(originalDDMinted, systemHealth)` → originalDD / ERRRatio (e.g., at 80% health: burn 125 DD to get back 100 DD worth of collateral)
  - `GetAdjustedRedemption(normalRedemption, systemHealth)` → DEPRECATED alias for backwards compat
  - `HasOracleConsensus(bundle)` → validates 8-of-15 oracle signatures for ERR activation
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
- `DigiDollar::Volatility::VolatilityThresholds` (struct) → WARNING_1H (10%), FREEZE_MINT_1H (20%), FREEZE_ALL_24H (30%), EMERGENCY_7D (50%), COOLDOWN_BLOCKS (144 ≈ 36 hours)
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
    - `ValidateOracleBundle(bundle, height, params)` → validates bundle for block inclusion
    - `ValidateOracleDataInBlock(block, height, params)` → validates oracle data in a block
    - `ValidatePhaseTwoBundle(bundle, params)` → static: Phase 2 validation rules
    - `ValidatePhaseOneBundle(bundle, params)` → static: Phase 1 validation rules
    - `ValidateBundle(bundle, height, params)` → static: dispatches to correct phase validator
    - `GetRequiredConsensus(height, params)` → returns required oracle count for height
    - `CalculateConsensusPrice(bundle, params)` → static: calculates median with outlier filtering
  - **Network:**
    - `BroadcastMessage(message)` → broadcasts via P2P
    - `ProcessIncomingMessage(message)` → handles incoming P2P oracle message
    - `HasOracleMessage(hash)` → duplicate detection
    - `SetConnman(connman)` → sets P2P connection manager
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
  - `ValidateBlockOracleData(block, pindex_prev, params, state)` → validates block's oracle data
  - `ValidateOraclePriceForTx(tx, oracle_price, height)` → validates oracle price for DD tx
  - `ValidateOracleMessage(message, params)` → validates individual oracle message
  - `ValidateOracleBundle(bundle, epoch, params)` → validates bundle for epoch
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

### src/oracle/exchange.h
- `ExchangeAPI::BaseExchangeFetcher` (base class) → persistent CURL handle to prevent socket exhaustion on Windows
  - `FetchPrice()` → pure virtual: returns price in micro-USD
  - `ConvertToMicroUSD(price_str)` / `ConvertToMicroUSD(double)` → converts price to micro-USD
  - `ExtractJsonValue(json, key)` → basic JSON value extraction
  - `HttpGet(url)` → makes HTTP GET request via reusable CURL handle
- **Exchange Fetchers (all override FetchPrice → micro-USD):**
  - `BinanceFetcher` → DGBUSDT direct or DGBBTC→BTCUSDT cross-pair
  - `CoinbaseFetcher` → Coinbase Pro or regular API
  - `KrakenFetcher` → DGB/USD with custom response parsing
  - `CoinGeckoFetcher` → public API, no key required
  - `BittrexFetcher` → DGB/USD
  - `PoloniexFetcher` → DGB/USDT
  - `MessariFetcher` → public API, no key required
  - `KuCoinFetcher` → DGB/USDT, no key required
  - `CryptoComFetcher` → DGB/USD, no key required
  - `GateIOFetcher` → DGB/USDT, no key required
  - `HTXFetcher` → DGB/USDT (formerly Huobi), no key required
- `ExchangeAPI::MultiExchangeAggregator` (class) → fetches from all 11 exchanges, calculates consensus price
  - `ExchangePrice` (struct) → exchange name, price_micro_usd, timestamp, success, weight
  - `FetchAggregatePrice()` → fetches all, filters outliers, returns median
  - `FetchAllPrices()` → fetches from all exchanges in parallel
  - `CalculateMedianPrice(prices)` → simple median
  - `CalculateWeightedMedian(prices)` → weighted median by exchange reliability
  - `CalculateWeightedAverage(prices)` → weighted average
  - `FilterOutliers(prices)` → removes prices > 10% from median
  - `IsOutlier(price, prices)` → checks single price against outlier threshold
  - `HasSufficientData()` → checks min_required_sources (default 2)
  - `SetExchangeWeight(exchange, weight)` / `GetExchangeWeight(exchange)` → per-exchange weights

### src/oracle/exchange.cpp
- Implementation of all 11 exchange fetchers with HTTP/JSON parsing
- Aggregator with outlier filtering and configurable consensus

### src/oracle/mock_oracle.h
- `MockOracleManager` (class, singleton) → provides mock oracle for RegTest testing
  - `GetInstance()` → singleton access
  - `GetCurrentPrice()` → returns mockPriceMicroUSD (micro-USD)
  - `SetMockPrice(price_micro_usd)` → sets mock price
  - `IsEnabled()` / `SetEnabled(enable)` → toggle mock oracle
  - `GetLastUpdateHeight()` → block height of last update
  - `CreateMockBundle(height)` → creates bundle with 8 signed messages from deterministic test keys
  - `SimulateVolatility(percentChange)` → applies % change to current price
  - `GetTestKey(oracle_id)` → returns deterministic test private key for oracle 0-4
  - `Reset()` → restores default state

### src/oracle/mock_oracle.cpp
- Implementation with deterministic keys derived from SHA256("digibyte_regtest_oracle_N")
- Creates bundles with ORACLE_CONSENSUS_REQUIRED (8) signatures

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
  - `GetOraclePrivateKey()` → returns key (hardcoded for Phase 1 testnet)
  - `GetOraclePublicKey()` → returns XOnlyPubKey for Schnorr signatures
  - `ValidateOracleKey()` → verifies key is authorized in chainparams
  - `CreatePriceMessage(price, timestamp)` → creates signed COraclePriceMessage
  - `BroadcastPriceMessage(message)` → broadcasts via P2P network
- `ExchangePriceFetcher` (class) → fetches from multiple exchanges, calculates median
  - `ExchangePrice` (struct) → exchange, price, timestamp, valid
  - `FetchAllPrices()` → fetches from Binance, Coinbase, Kraken, Bittrex, Poloniex
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
- Broadcast every 60 seconds (12–25× redundancy per epoch)

---

## Oracle Primitives

### src/primitives/oracle.h
- **Constants:**
  - `ORACLE_CONSENSUS_REQUIRED` = 8 (8 of 15)
  - `ORACLE_ACTIVE_COUNT` = 15
  - `ORACLE_TOTAL_COUNT` = 30
  - `ORACLE_MAX_AGE_SECONDS` = 3600 (1 hour)
  - `ORACLE_OUTLIER_THRESHOLD_PCT` = 10 (10%)
  - `ORACLE_MIN_PRICE_MICRO_USD` = 100 ($0.0001)
  - `ORACLE_MAX_PRICE_MICRO_USD` = 100000000 ($100.00)
- `COraclePriceMessage` (class) → individual oracle price report with BIP-340 Schnorr signatures
  - Fields: oracle_id, price_micro_usd, timestamp, block_height, nonce, oracle_pubkey (XOnlyPubKey), schnorr_sig (64 bytes)
  - `IsValid(reference_time)` → validates structure and timing
  - `Sign(key, merkle_root, aux)` → BIP-340 Schnorr signature creation
  - `Verify()` → Schnorr signature verification
  - `GetSignatureHash()` → Phase 1 hash (all fields)
  - `GetPhase2SignatureHash()` → Phase 2 hash (oracle_id + price + timestamp only)
  - `SignPhase2(key)` / `VerifyPhase2()` → Phase 2 signing/verification
  - `CheckForConflictingMessages(messages)` → detects duplicate/conflicting oracle submissions
- `COracleBundle` (class) → collection of oracle messages for consensus
  - Fields: messages vector, epoch, median_price_micro_usd, timestamp
  - `IsValid(reference_time, min_required)` → validates bundle structure and signatures
  - `AddMessage(message)` → adds validated message to bundle
  - `HasConsensus(min_required)` → checks if ≥ min_required valid messages exist
  - `GetConsensusPrice(min_required)` → calculates median price from valid messages
  - `ValidateEpoch(current_epoch)` → checks epoch consistency
  - `FilterOutliers()` → basic median ± threshold filtering
  - `FilterOutliersAdvanced()` → Modified Z-Score (MAD-based) outlier detection
  - `FilterOutliersIQR()` → Interquartile Range (1.5× IQR) outlier detection
- `OracleNodeInfo` (struct) → oracle node definition: id, pubkey, endpoint, is_active
- `SelectOraclesForEpoch(all_oracles, epoch)` → deterministic selection of 15 active oracles for epoch
- `GetCurrentEpoch(block_height)` → calculates epoch from block height
- `OracleP2P` (namespace) → P2P validation with DOS protection
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
  - `getdigidollarstatus()` → overall DD system status
- **Core Transactions:**
  - `mintdigidollar()` → mints DD by locking DGB collateral with specified lock tier
  - `senddigidollar()` → sends DD to another address
  - `redeemdigidollar()` → redeems DD to unlock DGB collateral
  - `listdigidollarpositions()` → lists all collateral positions and minted DD
- **Address Management:**
  - `getdigidollaraddress()` → generates new DD receive address
  - `validateddaddress()` → validates DD address format
  - `listdigidollaraddresses()` → lists all DD addresses in wallet
  - `importdigidollaraddress()` → imports external DD address
- **Utility:**
  - `getdigidollarbalance()` → returns DD balance
  - `estimatecollateral()` → estimates collateral needed without minting
  - `getredemptioninfo()` → redemption details for a position
  - `listdigidollartxs()` → lists DD transaction history
  - `getoracleprice()` → returns current oracle price
  - `getprotectionstatus()` → returns DCA/ERR/volatility protection status
- **Oracle:**
  - `createoraclekey()` → generates oracle signing key pair
  - `startoracle()` → starts oracle node with given key
- `RegisterDigiDollarRPCCommands(t)` → registers all DD RPC commands with the RPC table

### src/rpc/digidollar.cpp
- Full implementation of all DD RPC commands
- Integrates with wallet, oracle, health monitoring systems

### src/rpc/digidollar_transactions.h
- `getdigidollarinfo(request)` → DD system information
- `getdigidollaraddress(request)` → generate DD address
- `getdigidollarbalance(request)` → DD balance
- `mintdigidollar(request)` → mint DD tokens
- `transferdigidollar(request)` → transfer DD tokens
- `redeemdigidollar(request)` → redeem DD tokens
- `getredemptioninfo(request)` → redemption info for position
- `listredeemablepositions(request)` → list redeemable positions
- `setmockoracleprice(request)` → set mock oracle price (testing)
- `createrawddtransaction(request)` → create raw DD transaction
- `GetDigiDollarTransactionRPCCommands()` → returns Span of CRPCCommand entries

### src/rpc/digidollar_transactions.cpp
- Implementation of transaction-focused DD RPC commands

---

## Wallet Integration

### src/wallet/digidollarwallet.h
- `DDTransaction` (struct) → wallet-facing DD transaction: txid, amount, timestamp, confirmations, incoming, address, category (send/receive/mint/redeem), blockheight, blockhash, fee, comment, abandoned, lock_tier
- `WalletDDBalance` (struct) → address → balance mapping with last_updated timestamp
- `WalletCollateralPosition` (struct) → dd_timelock_id, dd_minted, dgb_collateral, lock_tier, unlock_height, is_active, owner_keyid
- `DDUtxo` (struct) → spendable DD UTXO: outpoint, dd_amount, is_spendable
- `DigiDollarWallet` (class) → high-level wallet interface for DD operations
  - **Owner Key Management:**
    - `StoreOwnerKey(dd_timelock_id, key)` → persists DD owner key to wallet database for restart survival
    - `LoadDDOwnerKeys()` → loads persisted owner keys during wallet init
    - `GetOwnerKey(dd_timelock_id, key)` → retrieves owner key for signing
  - **Address Key Management:**
    - `StoreAddressKey(output_key, key)` → persists P2TR address key for received DD spending
    - `LoadDDAddressKeys()` → loads persisted address keys during wallet init
    - `GetAddressKey(output_key, key)` → retrieves address key for signing
    - `IsDDOutputMine(txout, txid)` → checks dd_owner_keys, dd_address_keys, and IsMine for ownership
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
    - `SelectFeeCoins(fee_amount, selected_utxos, selected_total, amounts, exclude)` → selects DGB UTXOs for fees
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

### src/wallet/ddcoincontrol.h
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
- ⚠️ Handles `-digidollaractivationheight` CLI arg for regtest, sets BIP9 DEPLOYMENT_DIGIDOLLAR parameters

### src/validation.cpp / src/validation.h
- ⚠️ `AcceptToMemoryPoolWorker` → checks `DigiDollar::HasDigiDollarMarker()`, verifies BIP9 activation via `IsDigiDollarEnabled()`, creates `ValidationContext` with oracle price from `GetOraclePriceForDD()`, calls `ValidateDigiDollarTransaction()`
- ⚠️ `ConnectBlock` → same DD validation during block connection with `skipOracleValidation` for historical blocks, includes `txLookup` callback for block-db DD amount extraction
- ⚠️ `GetBlockScriptFlags` → sets `SCRIPT_VERIFY_DIGIDOLLAR` flag when DEPLOYMENT_DIGIDOLLAR is active
- ⚠️ `DisconnectBlock` → calls `RemovePriceCache()` to revert oracle price data
- ⚠️ `GetOraclePriceForDD()` → helper: queries oracle bundle manager, falls back to mock oracle for regtest

### src/init.cpp
- ⚠️ Registers `-digidollar`, `-digidollaractivationheight`, `-digidollarstatsindex` and all DD RPC args under OptionsCategory::DIGIDOLLAR
- ⚠️ Manages `g_digidollar_stats_index` lifecycle (init, interrupt, stop)

### src/script/script.h / src/script/script.cpp
- ⚠️ Defines `OP_DIGIDOLLAR` (0xbb), `OP_DDVERIFY` (0xbc), `OP_CHECKCOLLATERAL` (0xbd), `OP_CHECKPRICE` (0xbe) opcodes
- ⚠️ Opcode name mapping in `GetOpName()`

### src/script/interpreter.h / src/script/interpreter.cpp
- ⚠️ `SCRIPT_VERIFY_DIGIDOLLAR` flag enabling DD opcode execution
- ⚠️ `EvalScript` handles OP_DIGIDOLLAR, OP_DDVERIFY, OP_CHECKCOLLATERAL, OP_CHECKPRICE execution

### src/script/script_error.h / src/script/script_error.cpp
- ⚠️ `SCRIPT_ERR_INVALID_DD_AMOUNT` error code and its string mapping

### src/primitives/transaction.h / src/primitives/transaction.cpp
- ⚠️ `CMutableTransaction::SetDigiDollarType(type)` → encodes DD type into nVersion field (bits 24-31 = type, bits 0-15 = 0x0770)
- ⚠️ `CTransaction::IsDigiDollar()` → checks version for DD marker
- ⚠️ DD version constants and helper methods

### src/node/miner.cpp
- ⚠️ Calls `OracleBundleManager::AddOracleBundleToBlock()` to embed oracle data in coinbase during block assembly

### src/consensus/tx_check.cpp
- ⚠️ `CheckTransaction` → skips dust checks for DD transactions (0-value P2TR outputs are valid DD tokens)

### src/consensus/tx_verify.cpp
- ⚠️ Skips DD-specific validation that requires chain state (deferred to ConnectBlock)

### src/consensus/params.h
- ⚠️ `Consensus::DEPLOYMENT_DIGIDOLLAR` BIP9 deployment definition

### src/policy/policy.cpp
- ⚠️ `IsStandardTx` → exempts DD transactions from standard dust/size checks via version marker detection
- ⚠️ Allows 0-value P2TR outputs for DD token transfers

### src/protocol.h
- ⚠️ `MSG_ORACLE_PRICE` and `MSG_ORACLE_BUNDLE` P2P message types for oracle network

### src/deploymentinfo.cpp
- ⚠️ `DEPLOYMENT_DIGIDOLLAR` name and GBT name registration

### src/core_write.cpp
- ⚠️ DD-aware transaction serialization for `decoderawtransaction` RPC output

### src/common/args.cpp
- ⚠️ `OptionsCategory::DIGIDOLLAR` category definition for CLI args

### src/logging.cpp
- ⚠️ `BCLog::DIGIDOLLAR` log category registration

### src/node/transaction.cpp
- ⚠️ DD-aware transaction broadcast handling, oracle data relay

### src/rpc/client.cpp
- ⚠️ Registers all DD RPC command parameter types (22 entries)

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

### src/wallet/spend.cpp
- ⚠️ Coin selection excludes DD-locked UTXOs from regular DGB spending

### src/wallet/interfaces.cpp
- ⚠️ Wallet interface extensions for DD balance queries

### src/wallet/rpc/coins.cpp
- ⚠️ DD-aware `listunspent` filtering (excludes DD UTXOs from standard listing)

### src/wallet/rpc/wallet.cpp
- ⚠️ `getwalletinfo` includes DD balance and position counts

### src/wallet/scriptpubkeyman.h
- ⚠️ Forward declaration of DD key management interface

### src/wallet/types.h
- ⚠️ DD-related wallet type definitions

### src/interfaces/wallet.h
- ⚠️ `interfaces::Wallet` DD balance query methods

---

## Qt GUI

### src/qt/digidollartab.cpp/h
- `DigiDollarTab` → main DD tab widget containing all DD sub-widgets

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

---

## Tests

### C++ Unit Tests (`src/test/`)

| File | Coverage Area |
|------|--------------|
| `digidollar_activation_tests.cpp` | BIP9 activation logic, height-based feature gating, deployment status checks |
| `digidollar_address_tests.cpp` | DD address encoding/decoding, version bytes, network-specific prefixes, validation |
| `digidollar_bughunt_tests.cpp` | Regression tests for specific bugs: overflow, rounding, edge cases |
| `digidollar_change_tests.cpp` | DD change output creation, dust handling, balance conservation in transfers |
| `digidollar_consensus_tests.cpp` | Consensus parameter validation, collateral ratios, tier calculations, DCA levels |
| `digidollar_dca_tests.cpp` | DCA health tiers, multiplier calculations, system health boundaries, rapid transitions |
| `digidollar_err_tests.cpp` | ERR activation/deactivation, adjustment ratios, DD burn multiplier, queue management, oracle consensus |
| `digidollar_gui_tests.cpp` | Qt widget integration tests for DD tab and sub-widgets |
| `digidollar_health_tests.cpp` | SystemHealthMonitor metrics, tier breakdown, alert thresholds, health history, UTXO scanning |
| `digidollar_mint_tests.cpp` | Mint tx building, collateral calculation, OP_RETURN metadata, NUMS verification, tier/lock validation |
| `digidollar_opcodes_tests.cpp` | OP_DIGIDOLLAR, OP_DDVERIFY, OP_CHECKCOLLATERAL, OP_CHECKPRICE execution in script interpreter |
| `digidollar_oracle_tests.cpp` | Oracle integration with DD: price feeding, bundle creation, consensus price extraction |
| `digidollar_p2p_tests.cpp` | DD transaction relay, mempool acceptance, network propagation rules |
| `digidollar_persistence_keys_tests.cpp` | DD owner key and address key persistence across wallet restart |
| `digidollar_persistence_serialization_tests.cpp` | DD data structure serialization/deserialization for wallet database |
| `digidollar_persistence_walletbatch_tests.cpp` | WalletBatch DD read/write operations, database integrity |
| `digidollar_redeem_tests.cpp` | Redemption tx building, collateral release, timelock validation, ERR path, full burn requirement |
| `digidollar_redteam_tests.cpp` | Security-focused: NUMS bypass attempts, inflation attacks, cross-mint burns, partial burn exploits |
| `digidollar_restore_tests.cpp` | Wallet restore from blockchain rescan, position reconstruction, key recovery |
| `digidollar_rpc_tests.cpp` | RPC command validation: mintdigidollar, senddigidollar, redeemdigidollar, getdigidollarbalance |
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
| `oracle_block_validation_tests.cpp` | Oracle data validation during block processing, bundle extraction from coinbase |
| `oracle_bundle_manager_tests.cpp` | Bundle creation, message aggregation, epoch management, consensus price calculation |
| `oracle_config_tests.cpp` | Oracle configuration validation, parameter bounds, consensus thresholds |
| `oracle_exchange_tests.cpp` | Exchange fetcher HTTP/JSON parsing, multi-exchange aggregation, outlier filtering |
| `oracle_integration_tests.cpp` | End-to-end oracle → DD integration: price feed through to collateral calculation |
| `oracle_message_tests.cpp` | COraclePriceMessage signing, verification, Schnorr signatures, conflict detection |
| `oracle_miner_tests.cpp` | Oracle bundle embedding in coinbase, miner integration |
| `oracle_p2p_tests.cpp` | Oracle P2P message validation, rate limiting, DOS protection |
| `oracle_phase2_tests.cpp` | Phase 2 oracle validation rules, on-chain format, signature hash changes |
| `oracle_rpc_tests.cpp` | Oracle RPC commands: getoracleprice, createoraclekey, startoracle |
| `oracle_wallet_key_tests.cpp` | Oracle key generation, storage, validation against chainparams |

### Wallet Tests (`src/wallet/test/`)

| File | Coverage Area |
|------|--------------|
| `digidollar_persistence_wallet_tests.cpp` | Full wallet DD persistence: balances, positions, transactions, keys across restart |

### Qt Tests (`src/qt/test/`)

| File | Coverage Area |
|------|--------------|
| `digidollarwidgettests.cpp/h` | Qt widget unit tests for DD UI components |

### Python Functional Tests (`test/functional/`)

| File | Coverage Area |
|------|--------------|
| `digidollar_activation.py` | Basic BIP9 activation of DD features on regtest |
| `digidollar_activation_boundary.py` | Activation edge cases: exact height, off-by-one, pre/post activation behavior |
| `digidollar_basic.py` | End-to-end DD workflow: mint → transfer → redeem on regtest |
| `digidollar_encrypted_wallet.py` | DD operations with encrypted wallet: unlock, mint, send, lock |
| `digidollar_mint.py` | Mint transaction creation, collateral validation, tier selection |
| `digidollar_network_relay.py` | DD transaction propagation across multi-node network |
| `digidollar_network_tracking.py` | Network-wide DD supply tracking and consistency |
| `digidollar_oracle.py` | Basic oracle price feeding and DD integration |
| `digidollar_oracle_keygen.py` | Oracle key generation and authorization |
| `digidollar_oracle_phase2.py` | Phase 2 oracle protocol: on-chain bundles, Schnorr verification |
| `digidollar_persistence.py` | DD data persistence across node restart |
| `digidollar_phase2_integration.py` | Phase 2 integration: oracle + DD mint + transfer + redeem pipeline |
| `digidollar_protection.py` | DCA, ERR, and volatility protection mechanisms |
| `digidollar_redeem.py` | Basic redemption: timelock expiry, collateral release |
| `digidollar_redeem_stats.py` | Redemption statistics tracking via DigiDollarStatsIndex |
| `digidollar_redemption_amounts.py` | Redemption amount calculations: full, ERR-adjusted, fee deductions |
| `digidollar_redemption_e2e.py` | End-to-end redemption: mint → wait → redeem → verify balance |
| `digidollar_rpc.py` | General DD RPC command testing |
| `digidollar_rpc_addresses.py` | getdigidollaraddress, validateddaddress, listdigidollaraddresses |
| `digidollar_rpc_collateral.py` | calculatecollateralrequirement, estimatecollateral |
| `digidollar_rpc_dca.py` | getdcamultiplier, DCA-related RPC |
| `digidollar_rpc_deployment.py` | DD deployment status RPC queries |
| `digidollar_rpc_estimate.py` | Collateral estimation RPC with various parameters |
| `digidollar_rpc_gating.py` | RPC command availability gating based on activation status |
| `digidollar_rpc_oracle.py` | getoracleprice, setmockoracleprice, oracle status |
| `digidollar_rpc_protection.py` | getprotectionstatus, DCA/ERR/volatility via RPC |
| `digidollar_rpc_redemption.py` | redeemdigidollar, getredemptioninfo, listredeemablepositions |
| `digidollar_stress.py` | Stress testing: high tx volume, many positions, rapid mints/transfers |
| `digidollar_transactions.py` | Transaction structure validation, version markers, OP_RETURN parsing |
| `digidollar_transfer.py` | DD transfer: single/multi recipient, conservation, change outputs |
| `digidollar_tx_amounts_debug.py` | Debugging tool for DD amount extraction and validation |
| `digidollar_wallet.py` | Wallet DD integration: balance, history, UTXO management |
| `wallet_digidollar_backup.py` | DD wallet backup and restore from backup file |
| `wallet_digidollar_descriptors.py` | Descriptor wallet compatibility with DD keys |
| `wallet_digidollar_encryption.py` | Wallet encryption impact on DD operations |
| `wallet_digidollar_persistence_restart.py` | DD data survival across wallet/node restart cycles |
| `wallet_digidollar_rescan.py` | Wallet rescan reconstructs DD positions from blockchain |
| `wallet_digidollar_restore.py` | Full wallet restore from seed with DD position reconstruction |
| `feature_oracle_p2p.py` | Oracle P2P networking: message broadcast, relay, validation |
| `rpc_getoracles_pending.py` | Oracle pending messages RPC query |
