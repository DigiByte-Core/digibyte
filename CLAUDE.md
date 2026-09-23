# CLAUDE.md — AI Agent Guide for DigiByte Core / DigiDollar

## What this repo is

DigiByte Core v9.26 (Bitcoin Core v26.2 lineage), augmented with the **DigiDollar** native USD-denominated token and a multi-oracle DGB/USD price feed using MuSig2 Schnorr signatures.

The v9.26.6 development line is `release/v9.26.6`. Work may use a separate branch and worktree; verify the actual checkout before editing. Upstream main branch: `develop`.

## Required reading order before any DigiDollar / oracle work

1. `CLAUDE.md` — This guide, branch rules, and current code-surface warnings
2. `ARCHITECTURE.md` — Core DigiByte system design
3. `REPO_MAP.md` — Core DigiByte file index (excludes DigiDollar/oracle subsystem)
4. `REPO_MAP_GUIDE.md` — How to maintain and interpret the repo maps
5. `DIGIDOLLAR_ARCHITECTURE.md` — DigiDollar mint/transfer/redeem/collateral/health/state
6. `DIGIDOLLAR_ORACLE_ARCHITECTURE.md` — Exchange aggregation, bundle lifecycle, MuSig2, P2P
7. `REPO_MAP_DIGIDOLLAR.md` — DigiDollar/oracle file index (production, wallet, RPC, Qt, tests, fuzz)
8. `DIGIDOLLAR_EXPLAINER.md` — User-facing DigiDollar V1 protocol summary
9. `DIGIDOLLAR_ORACLE_EXPLAINER.md` — User-facing oracle/MuSig2 summary
10. `DIGIDOLLAR_ACTIVATION_EXPLAINER.md` — Activation gating of DD/oracle surface (v9.26.5 BIP90 burial + BIP9 history)
11. `DIGIDOLLAR_WALLET_INTEGRATION.md` — Wallet/RPC integration guide
12. `DIGIDOLLAR_EXCHANGE_INTEGRATION.md` — Exchange/custody integration guide
13. `ORACLE_DISCOVERY_ARCHITECTURE.md` — Oracle endpoint discovery design
14. `DIGIDOLLAR_ORACLE_SETUP.md` — Oracle setup and migration runbook

Also read `docs/ORACLE_OPERATOR_GUIDE.md`, `doc/digidollar-operations.md` and `doc/release-notes/release-notes-9.26.6.md` before changing operator instructions or release claims. Historical RC notes do not establish current readiness.

## Repo layout (DigiDollar / oracle surface)

```
src/digidollar/      # 6 modules: amount, digidollar, health, scripts, txbuilder,
                     # validation
src/oracle/          # Oracle daemon + MuSig2: bundle_manager, exchange, mock_oracle,
                     # node, signing_orchestrator, musig2_{aggregator,session,
                     # session_manager,orchestrator,oracle_participation,messages,
                     # session_mining}
src/consensus/       # dca, err, digidollar, digidollar_state.h,
                     # digidollar_transaction_validation, digidollar_tx, volatility
                     # (DigiDollar/oracle consensus rules)
src/index/           # digidollarstatsindex (DD supply/health index)
src/primitives/      # oracle.h (price message + bundle types)
src/rpc/             # digidollar.cpp (18 base RPCs); digidollar_transactions.cpp is
                     # legacy / unregistered
src/wallet/          # digidollarwallet.{cpp,h}, ddcoincontrol.h; wallet/rpc/wallet.cpp
                     # registers 17 wallet-context DD/oracle RPCs
src/qt/              # DD tab + widgets/dialogs: digidollar{tab,mintwidget,sendwidget,
                     # receivewidget,redeemwidget,overviewwidget,positionswidget,
                     # transactionswidget,coincontroldialog,receiverequest},
                     # ddaddressbookpage, digidollar_qt_translate
src/test/            # ~160 DigiDollar/oracle/MuSig2/Red-Hornet unit tests + fuzz/
src/wallet/test/     # 7 DD wallet test sources (persistence, security, lock safety,
                     # Wave 16/17, rh59 lock-bypass)
                     # — linked into src/test/test_digibyte, not a separate binary
src/qt/test/         # digidollarwidgettests, digidollarwave19widgettests,
                     # digidollarmintrecordtests
test/functional/     # 104 DD/oracle Python test entries registered in
                     # test_runner.py
```

## DigiDollar opcode soft-fork additions

Defined in `src/script/script.h:209-220`:
- `OP_DIGIDOLLAR    = 0xbb` (Tapscript OP_SUCCESSx slot pre-activation)
- `OP_DDVERIFY      = 0xbc` (Tapscript OP_SUCCESSx slot pre-activation)
- `OP_CHECKPRICE    = 0xbd` — **deterministically DISABLED (reserved)** as of DD-FINAL-005 / AR-0. Post-activation it consumes its witness operand and always pushes `vchFalse` (fail closed); it no longer consults any oracle price. It previously consulted the live node-local/wall-clock price (`g_get_oracle_consensus_price` → `OracleBundleManager::GetLatestPrice`), which was non-deterministic across nodes and a chain-split risk; no DigiDollar script emits OP_CHECKPRICE, so disabling it has zero protocol impact. A future price opcode must bind to the block's own committed v0x03 bundle price (`src/script/interpreter.cpp:708`).
- `OP_CHECKCOLLATERAL = 0xbe` — compares stack ratio to threshold; consumes `<ratio> <threshold>` and pushes `ratio>=threshold`
- `OP_ORACLE        = 0xbf` — coinbase oracle bundle marker

These are OP_SUCCESSx-class opcodes that become functional only when `SCRIPT_VERIFY_DIGIDOLLAR` is set, which only happens when the buried `DEPLOYMENT_DIGIDOLLAR` deployment is active (BIP90 as of v9.26.5; historically BIP9 bit 23). See `IsDigiDollarOpcode` / `IsOpSuccessForFlags` at `src/script/interpreter.cpp:439-453`.

## Activation summary (buried deployment — BIP90, v9.26.5)

As of v9.26.5 the Taproot, DigiDollar, and AlgoLock deployments are **buried** (BIP90): all three are ACTIVE on mainnet, activation is a hardcoded per-network height returned by `Consensus::Params::DeploymentHeight()` (fields `TaprootHeight` / `DigiDollarHeight` / `AlgoLockHeight`), the BIP9 state machine no longer runs for them, and blocks no longer signal bits 2/23/0. `DeploymentPos` retains only `DEPLOYMENT_TESTDUMMY`. The burial heights are the empirically verified BIP9 `since` heights (live mainnet `getdeploymentinfo`; testnet26 verified block-by-block). The historical BIP9 parameters (bit 23, mainnet start 2026-06-01, 70% of a 40,320-block window, `min_activation_height` floor 23,627,520) are preserved in `DIGIDOLLAR_ACTIVATION_EXPLAINER.md`.

| Network | `TaprootHeight` | `DigiDollarHeight` | `AlgoLockHeight` |
|---------|-----------------|--------------------|------------------|
| Mainnet | 21,168,000 | 23,869,440 | 23,869,440 |
| Testnet (testnet26) | 0 | 600 | 0 |
| Signet (unsupported) / Regtest | 0 | 0 | 0 |

Static gates are unchanged: mainnet `nDDActivationHeight = nOracleActivationHeight = nDigiDollarMuSig2Height = 23,627,520` (the historical BIP9 floor, deliberately below the 23,869,440 burial height); testnet 600 / 600 / 600. Default regtest keeps the DD/oracle height gates at 650 / 650 while `nDigiDollarMuSig2Height = min(650, DigiDollarHeight) = 0`, so v0x03 quotes are valid whenever DigiDollar is active. `EarliestActivationFloor(params) = min(nDDActivationHeight, DigiDollarHeight)` (0 if the deployment is disabled) — value-preserving vs the pre-burial formula on every network. `IsDigiDollarEnabled` is a pure height compare against `DigiDollarHeight` (no `VersionBitsCache` anywhere in the DD path), and startup oracle-price reconstruction uses the same buried predicate as block connection, so DD-active blocks below the regtest 650 gate are still not dropped during restart/reindex cache rebuilds.

Regtest knobs: `-digidollaractivationheight=N` sets `DigiDollarHeight` AND `nDDActivationHeight`/`nOracleActivationHeight`/`nDigiDollarMuSig2Height` to N, so DigiDollar activates at exactly height N (pre-burial the knob ran real BIP9 signaling and activated at the first 144-block window boundary >= max(432, N)). `-testactivationheight=taproot@H` / `digidollar@H` / `algolock@H` moves only the buried deployment height — the static DD/oracle gates keep their defaults, but `nDigiDollarMuSig2Height` is derived as `min(nDDActivationHeight, DigiDollarHeight)` and so follows `digidollar@H` below 650; `-digidollaractivationheight` takes precedence for DigiDollar. `-vbparams=digidollar/taproot/algolock` is now a startup error ("Invalid deployment") — only `testdummy` remains.

RPC/GBT surface: `getdeploymentinfo`/`getblockchaininfo` render the three deployments as `{"type":"buried","active":…,"height":…}` with no `bip9` sub-object. `getdigidollardeploymentinfo` now returns `{enabled, type:"buried", status:"active"|"defined", activation_height (omitted if the deployment is disabled), oracle_activation_height, musig2_format_activation_height, oracle_pubkey_count, oracle_consensus_required, oracle_total_slots, oracle_seed_peers, musig2_session{...}, thaw_day{scheduled, height (only when scheduled), tip_height, next_block_height, active_at_tip, active_next_block}}`; the BIP9 fields (`bit`, `start_time`, `timeout`, `min_activation_height`, `blocks_until_timeout`, `signaling_blocks`, `threshold`, `period_blocks`, `progress_percent`) were removed, and `activation_height` is now always the burial height (the old back-scan reported the first-active/last-LOCKED_IN block instead). `getblocktemplate` always lists `taproot`/`digidollar`/`algolock` in `rules` once active (hardcoded like `csv`); `vbavailable` no longer mentions them and the template `version` never sets bits 2/23/0. `MinBIP9WarningHeight` is 23,909,760 on mainnet and 800 on testnet. The variable in code is `nDigiDollarMuSig2Height`, not the older `nDigiDollarPhase3Height`. Once DigiDollar is active, v0x03 MuSig2 is the only on-chain bundle format ever accepted.

## Thaw Day and health accounting

`nDDThawDayHeight` is the shared height for the new mint-only volatility rule,
open-vault health accounting and canonical vault identity. Read it through
`DigiDollar::IsThawDayActive(params, candidate_height)` in
`src/digidollar/digidollar.cpp`. Mainnet is scheduled at **24,490,000**, estimated
for November 1, 2026. Testnet26 is scheduled at **432,100**, estimated for
September 18–19, 2026. Block heights are the triggers; dates are estimates.
Signet is unsupported and remains unscheduled. Regtest is disabled unless
`-ddthawdayheight=N` is set; the option is rejected outside regtest. Publish
the tagged source, binaries and heights at least 14 days before mainnet
activation and at least 3 days before testnet activation. Public activation
and soak testing have not occurred for this candidate. Do not describe
unfinished release work as ready to distribute.

Block validation uses the candidate's height, including when a node reindexes.
Mining, mempool and wallet construction use the next-block height. At tip H-1,
`thaw_day.active_at_tip` can be false while `active_next_block` is true. See
[DIGIDOLLAR_ACTIVATION_EXPLAINER.md](DIGIDOLLAR_ACTIVATION_EXPLAINER.md).

`getdigidollarstats.total_dd_supply` is the circulating-supply API field in
cents. Below Thaw Day, its legacy fallback without the stats index still scans
vault amounts; use a synchronized stats index for verified circulation there.
Do not describe that legacy fallback as an exact token count.
`canonical_health.open_vault_principal` separately counts original DD attached
to unspent vaults. At and above the height, health uses the chainstate-owned principal,
collateral and count, tied to the matching block and rules version. Extra burns
reduce circulation without reducing other vaults' principal. This can change
health bands and redemption burn requirements. It does not change balances at
activation. `canonical_health.ready=false` means unavailable, not zero.

Top-level `health_rule_height` and `selected_health_denominator` describe tip
health; `next_block_health` reports its own candidate height, quote and
readiness. Validate or reconstruct state before using it. The stats index and
wallet must not overwrite consensus state. Sources: `src/consensus/digidollar_state.h`,
`src/validation.cpp`, `src/digidollar/health.cpp`, `src/rpc/digidollar.cpp`.

Routine DigiDollar and oracle diagnostics use `LogPrint(BCLog::DIGIDOLLAR, ...)`.
Preserve useful failures and startup/recovery progress without requiring that
category. Keep wallet prefixes. Operator debug settings and startup-only log
shrinking are explained in [doc/digidollar-operations.md](doc/digidollar-operations.md).

## Oracle roster

| Network | Total slots | Active | Consensus |
|---------|-------------|--------|-----------|
| Mainnet | 35 (`vOracleNodes`) | 35 (`consensus.vOraclePublicKeys` slots 0-34) | 7 signatures from active keyset |
| Testnet | 35 | 35 (slots 0-34) | 7 signatures from active keyset |
| Regtest | 7 | 7 | 4-of-7 |

Oracle display names come from each network's `OracleNodeInfo.display_name`.
Mainnet ID 0 is `DigiByte.Io Oracle`; ID 11 is `Crypto Corner Shop`. Their
testnet names remain `Jared` and `hallvardo`. Names are local display metadata;
they are not serialized and do not change keys, IDs, ordering or quorum.
Source: `src/primitives/oracle.h`, `src/kernel/chainparams.cpp` and
`src/rpc/digidollar.cpp`.

Slots 0-34 on mainnet and testnet are active. Slot 28 uses the DigiHash Mining
Pool key, slot 31 uses the Peer2Peer / DigiRoos key, and all 35 configured
slots contain valid compressed secp256k1 oracle keys. ID 35 is outside the
configured roster and must be rejected by RPC/P2P/bitmap bounds checks.

`src/primitives/oracle.h:19-21` declares header defaults `ORACLE_CONSENSUS_REQUIRED=7`, `ORACLE_ACTIVE_COUNT=35`, `ORACLE_TOTAL_COUNT=35`. Chainparams sets `nOracleConsensusRequired`, `nOraclePubkeyCount`, and `nOracleTotalOracles` per network at startup, so those chainparams values are what the validator and MuSig2 aggregator use.

## DigiDollar critical constants

```python
# DigiByte chain values (do not use Bitcoin defaults)
BLOCK_TIME            = 15            # seconds
COINBASE_MATURITY     = 8             # blocks (100 after height threshold = COINBASE_MATURITY_2)
SUBSIDY               = 72000         # DGB
MAX_MONEY             = 21_000_000_000

# Fees (DigiByte uses DGB/kB, not DGB/vB)
MIN_RELAY_TX_FEE      = 0.001         # DGB/kB
DEFAULT_TRANSACTION_FEE = 0.1         # DGB/kB

# Network
P2P_PORT_MAINNET      = 12024
P2P_PORT_TESTNET      = 12033  # testnet26 per src/kernel/chainparams.cpp

# Address formats
REGTEST_BECH32        = 'dgbrt'
TESTNET_BECH32        = 'dgbt'
DD_ADDR_PREFIX        = 'DD'  / 'TD' / 'RD' (mainnet/testnet/regtest)

# DigiDollar-specific
DD_AMOUNT_UNIT        = 1 cent (10000 = $100.00)
DD_RPC_AMOUNT_UNIT    = senddigidollar, sendmanydigidollar, redeemdigidollar,
                        getredemptioninfo, listdigidollarpositions and
                        listdigidollaraddresses take an optional trailing
                        amount_unit of "cents" or "dollars". With no unit the
                        amount must be a whole number of cents; a decimal point
                        with no unit is refused instead of guessed
                        (src/digidollar/amount.cpp,
                        src/rpc/digidollar.cpp ParseDigiDollarRpcAmount)
DD_TX_VERSION_MARKER  = 0x0770 (low 16 bits of nVersion)
DD_TX_TYPE_FIELD      = (nVersion >> 24) & 0xFF  # 1=MINT, 2=TRANSFER, 3=REDEEM
MINT_MIN              = 10_000   cents
MINT_MAX              = 10_000_000 cents
LOCK_TIERS            = 0..9 (1h, 30d, 90d, 180d, 1y, 2y, 3y, 5y, 7y, 10y)
                        # canonical durations enforced at consensus —
                        # mint lock height must be in the claimed tier window
                        # [canonical_blocks, canonical_blocks + 100]
                        # under-locked/custom durations rejected
                        # (src/digidollar/validation.cpp)
LOCK_TIER_OPRETURN    = stored explicitly in mint OP_RETURN; consensus rejects
                        bad-mint-lock-tier-duration when remaining lock blocks
                        fall outside the canonical confirmation window for the
                        claimed tier
```

## Where DigiDollar/oracle is gated at runtime

| Layer | Gate | Reference |
|-------|------|-----------|
| RPC | `DigiDollar::IsDigiDollarEnabled(tip, chainman)` at the top of each DD/oracle RPC | `src/rpc/digidollar.cpp` (10+ callsites) |
| Mempool | `IsDigiDollarEnabled` + `HasDigiDollarMarker`; DD mint and redeem txs additionally require a recent valid MuSig2 oracle quote via `HasRecentValidMuSig2OracleQuote`. DD transfers are price-independent and need no quote (`DigiDollarMempoolTxRequiresOracleQuote`) | `src/validation.cpp:422-491, 1234-1249` |
| Block | `IsDigiDollarEnabled` + `HasDigiDollarMarker` during `ConnectBlock`; coinbase oracle bundles require V1 MuSig2 version via `CheckMuSig2OracleBundleVersion` | `src/validation.cpp:268-304, 3091-3768` |
| Script | `SCRIPT_VERIFY_DIGIDOLLAR` flag | `src/validation.cpp` |
| P2P | `IsOracleP2PActive` at the top of all oracle handlers, including `oraclehb`, `oracleprice`, `oraclebundle` (accepted-and-dropped — V1 puts the bundle on-chain), `oracleconsns`, `oracleattest`, `oramusnonce`, `oramusigctx`, `oramusigpsig`, and `getoracles`. Wire names defined in `src/protocol.cpp:53-62`. | `src/net_processing.cpp` |
| Qt | `DigiDollarTab` activation overlay; widgets check `isVisible()` before polling | `src/qt/digidollartab.cpp` |
| Price cache | `UpdatePriceCache` gated on `DEPLOYMENT_DIGIDOLLAR` | `src/validation.cpp` (rh61 fix) |

## RPC surface (35 commands)

18 commands registered via `RegisterDigiDollarRPCCommands()` in `src/rpc/digidollar.cpp:7046`:
`getdigidollarstats`, `getdcamultiplier`, `calculatecollateralrequirement`, `getdigidollardeploymentinfo`, `importdigidollaraddress`, `estimatecollateral`, `getoracleprice`, `getalloracleprices`, `getprotectionstatus`, `getoracles`, `getoraclesigners`, `listoracle`, `stoporacle`, `getoraclepubkey`, `setmockoracleprice` (regtest), `getmockoracleprice` (regtest), `simulatepricevolatility` (regtest), `enablemockoracle` (regtest).

17 wallet-context commands registered in `GetWalletRPCCommands()` at `src/wallet/rpc/wallet.cpp:888`:
`mintdigidollar`, `senddigidollar`, `sendmanydigidollar`, `redeemdigidollar`, `listdigidollarpositions`, `listdigidollaraddresses`, `getredemptioninfo`, `getdigidollarbalance`, `getdigidollaraddress`, `listdigidollartxs`, `listdigidollarunspent`, `listdigidollarutxos`, `validateddaddress`, `createoraclekey`, `exportoracleprivkey`, `importoracleprivkey`, `startoracle`.

`createoraclekey`, `exportoracleprivkey`, and `importoracleprivkey` are local wallet key-management RPCs and are intentionally usable before DigiDollar activation. They do not start an oracle, sign prices, relay oracle data, or change consensus state. `startoracle` and the price/DD operational RPCs remain activation-gated.

`sendoracleprice` and `submitoracleprice` are intentionally **absent** from the registration tables: `sendoracleprice` was removed as a fake-price-injection vulnerability, and `submitoracleprice` does not exist anywhere in the source tree. Oracle prices come exclusively from live exchange aggregation.

`src/rpc/digidollar_transactions.cpp` declares legacy entry points (`getdigidollarinfo`, `transferdigidollar`, `createrawddtransaction`, `listredeemablepositions`) plus duplicate names that are also defined in `src/rpc/digidollar.cpp` and `src/wallet/rpc/wallet.cpp` (`getdigidollaraddress`, `getdigidollarbalance`, `mintdigidollar`, `redeemdigidollar`, `getredemptioninfo`). Its `GetDigiDollarTransactionRPCCommands()` is **never called** — no caller exists in the build — so all RPCs in this file are inert. Treat as legacy/dead code unless rewired.

## Build / test commands

```bash
# Build
./autogen.sh && ./configure && make -j$(nproc)

# Run a specific C++ test suite
./src/test/test_digibyte --run_test=digidollar_validation_tests
./src/test/test_digibyte --list_content | grep -Ei 'digidollar|oracle|musig|^rh'

# Wallet-context DD tests (require ENABLE_WALLET; built into the same
# test_digibyte binary via src/Makefile.test.include when --enable-wallet is on,
# *not* a separate test_wallet_digibyte binary):
./src/test/test_digibyte --run_test=digidollar_persistence_wallet_tests
./src/test/test_digibyte --run_test=digidollar_wallet_security_tests
./src/test/test_digibyte --run_test=rh59_coincontrol_dd_lock_bypass_tests

# Run a functional test
./test/functional/digidollar_basic.py

# Find DigiDollar/oracle source without generated/build products
rg -n 'digidollar|DigiDollar|oracle|MuSig|musig' src/ test/ \
  -g '*.cpp' -g '*.h' -g '*.hpp' -g '*.py' \
  -g '!**/.deps/**' -g '!**/.libs/**' -g '!*.o' -g '!*.lo' \
  -g '!src/qt/moc_*.cpp' -g '!src/qt/test/moc_*.cpp' -g '!src/qt/forms/ui_*.h'
```

## Important notes

- **Confirmed-only DigiDollar transfers.** Unconfirmed DD chaining was removed (commit `0b4959f563`). Consensus refuses to resolve DD amounts from `MEMPOOL_HEIGHT` inputs for transfer/redeem; wallets must wait for confirmation between sends.
- **OP_CHECKPRICE is deterministically disabled (DD-FINAL-005).** Post-activation the opcode consumes its operand and unconditionally pushes `vchFalse` (`src/script/interpreter.cpp:708-735`); it no longer calls `g_get_oracle_consensus_price` (the hook is left null in production — see `src/init.cpp` Step 13). This removed the AR-0 fork vector (the old live-price consult was node-local/wall-clock dependent). The standalone `libdigibyteconsensus.so` behaviour is unchanged (also fails closed).
- **Mainnet/testnet validator parity.** The mainnet oracle-validation short-circuit was removed (commit `f0d9a7b2c7`). `OracleDataValidator::ValidateBlockOracleData` in [src/oracle/bundle_manager.cpp](src/oracle/bundle_manager.cpp) now runs identically on mainnet and testnet, and only v0x03 MuSig2 bundles are accepted (commits `bbb85cf363`, `fa29405adc`, `f2bb0a19a4`). Raw v0x01/v0x02 OP_RETURN payloads short-circuit inside `ExtractOracleBundle`, so the validator emits `bad-oracle-malformed`. The `bad-oracle-legacy` branch only fires when extraction succeeds with a non-MuSig2 version, which is structurally unreachable for current v0x03 wire payloads — it remains as a defense-in-depth gate.
- **Price-dependent DD blocks must include exactly one v0x03 MuSig2 bundle.** Commit `1e08bd811f`: `ValidateBlockOracleData` returns `bad-oracle-missing` if a DD mint/redeem block has no oracle output, `bad-oracle-multiple-outputs` if it has more than one, and `bad-oracle-malformed` if extraction fails (which is the canonical reason raw v0x01/v0x02 produce). DD transfer-only and non-DD blocks may omit the bundle entirely.
- **Mempool requires an oracle quote for DD mint and redeem.** Commit `81bf974f40`: a DD mint or redeem is not accepted into the mempool unless `HasRecentValidMuSig2OracleQuote` finds a recent valid v0x03 quote. `DigiDollarMempoolTxRequiresOracleQuote` limits the check to those two types, so a DD transfer, which needs no price, is admitted without one (`src/validation.cpp:422-491, 1234-1249`).
- **Custom lock durations rejected.** Consensus enforces canonical lock tiers with a 100-block confirmation buffer: `bad-mint-lock-period` for non-canonical periods, `bad-mint-lock-tier` for tier outside 0–9, and `bad-mint-lock-tier-duration` when remaining lock blocks fall outside `[canonical_blocks, canonical_blocks + 100]` for the claimed tier (commits `e1dd69f99b`, `11728a6980`).
- **DD supply alert, not a cap.** `AlertThresholds::ALERT_DD_SUPPLY` (`src/digidollar/health.h:170`, 10000000000 = 100M DD) is a monitoring threshold, not a consensus cap. `MAX_DIGIDOLLAR` is a per-output serialization bound. There is no global circulating-supply cap; total DD is constrained only by available collateral and the per-block minting rate.
- **Collateral vault spends require DD burn.** Non-DD transactions that try to spend a registered DigiDollar collateral vault are rejected with `bad-collateral-spend-missing-dd-burn`. Inside DD redemptions, `ValidateCollateralReleaseAmount` rejects partial burns with `bad-collateral-release-partial-burn` (`src/digidollar/validation.cpp`).
- **Mint wallet capability.** `src/wallet/digidollarmintcapability.h` checks descriptor/private-key flags, HD support, an active ranged Taproot receiving descriptor and an active ranged bech32 internal change descriptor with private keys. It does not allocate keys. Qt checks before its confirmation dialogs; RPC checks before unlock/key derivation. A supported encrypted wallet remains eligible for the normal unlock flow. Legacy, watch-only, blank and restricted wallets receive a capability error when required support is missing. Preserve existing backups and never promise that migration recreates missing keys. See `DIGIDOLLAR_WALLET_INTEGRATION.md`.
- **Mining graceful degradation.** `CreateNewBlock` strips price-dependent DD mint/redeem txs when no valid oracle bundle is available, keeps ordinary DGB and price-independent DD transfer candidates flowing when valid, and continues block assembly instead of hanging (commit `6b5ff516c3`).
- **A pruned node keeps the whole DigiDollar era.** A DigiDollar spend has to read the block that created its input, and that block is always at or above the DigiDollar floor. So when `-prune` is set, `LoadChainstate` registers a prune lock named `digidollar` at `DigiDollar::EarliestActivationFloor(params)` and the node keeps every block from there to the tip (`src/node/chainstate.cpp:159-190`). On mainnet that floor is 23,627,520, which at the tip of 24,195,289 recorded on 12 September 2026 is 567,769 blocks, about 2.3 per cent of the chain, and grows by roughly 5,760 a day. A small `-prune` target such as 550 MiB is therefore not reachable on mainnet: the node still starts and still prunes everything below the floor, but it cannot shrink past the retained window. If a DigiDollar-era block is already missing when a pruned node starts, startup fails with "DigiDollar state not ready: retained block history is incomplete".
- **Transaction decoding declares its DigiDollar fields.** `TxToUniv` adds a `digidollar` object (`type`, `type_id`, `flags`, `flags_hex`) to any transaction carrying the marker (`src/core_write.cpp:186-193`), and `decoderawtransaction`/`getrawtransaction` now declare that object in their result (`src/rpc/rawtransaction.cpp:107-113`). Before this release the field was returned but not declared, so a node started with `-rpcdoccheck` raised an internal error on the call.
- **BIP324 V2 P2P transport** is supported and enabled with `-v2transport=1` (off by default).
- **Three-way comparison still applies for non-DigiDollar work.** Compare v9.26 ↔ v8.22.2 ↔ Bitcoin v26.2 in `digibyte-v8.22.2/` and `bitcoin-v26.2-for-digibyte/` when making changes to inherited code.
- **Avoid widening doc claims beyond what code shows.** All material claims in the approved docs (16 listed in `Z_PROMPTS.md`) must match `src/`. Treat code as truth.

## Quick orientation for sub-agents

When spawning a sub-agent on DigiDollar/oracle work, point it at this file plus the four most relevant docs (`DIGIDOLLAR_ARCHITECTURE.md`, `DIGIDOLLAR_ORACLE_ARCHITECTURE.md`, `REPO_MAP_DIGIDOLLAR.md`, `DIGIDOLLAR_ACTIVATION_EXPLAINER.md`) and an explicit list of files it should read/modify. Do not ask sub-agents to redesign architecture; treat existing approved docs as the contract.
