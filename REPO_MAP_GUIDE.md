# REPO_MAP_GUIDE.md — AI Agent Codebase Navigation for DigiByte

## The Problem

AI coding agents wake up fresh every session with zero memory of where code lives. Without a map, agents waste thousands of tokens grepping around, reading wrong files, and fumbling before finding the right code to edit. This guide explains a documentation system that fixes that.

## The Two-File Standard

Every codebase (or major subsystem) gets two navigation documents:

| File | Purpose |
|------|---------|
| **ARCHITECTURE.md** | High-level design — what components exist, how they connect, why they're designed that way |
| **REPO_MAP.md** | Granular index — every file, class, and function with one-liner descriptions |

ARCHITECTURE.md tells you *what to look for*. REPO_MAP.md tells you *exactly where to find it*. Together they cut navigation token waste by ~3x.

## DigiByte Repo Structure

DigiByte is a Bitcoin Core fork with **DigiDollar** (over-collateralized USD-pegged stablecoin) built on top. This means two distinct layers need two separate repo maps:

```
digibyte/
├── ARCHITECTURE.md                        ← Exists: Core DigiByte design
├── REPO_MAP.md                            ← Exists: Core DigiByte file index
├── REPO_MAP_DIGIDOLLAR.md                 ← Exists: DigiDollar + Oracle file index
├── DIGIDOLLAR_ARCHITECTURE.md             ← Exists: DigiDollar system design
├── DIGIDOLLAR_ORACLE_ARCHITECTURE.md      ← Exists: Oracle network design
└── ORACLE_DISCOVERY_ARCHITECTURE.md       ← Exists: Oracle peer discovery
```

## What to Generate

### 1. `REPO_MAP.md` (Repo Root) — Core DigiByte

Index all C++ source code in `src/` and its subdirectories, plus tests.

**Include these directories:**

| Directory | Contents |
|-----------|----------|
| `src/` (root files) | ~167 top-level .cpp/.h/.hpp — `validation.cpp`, `net.cpp`, `net_processing.cpp`, `txmempool.cpp`, `chainparams.cpp`, `chainparamsbase.cpp`, `pow.cpp`, `dandelion.cpp`, `bip324.cpp`, etc. |
| `src/bench/` | Benchmarks |
| `src/common/` | Common utilities (args, settings, init, system, bloom, run_command) |
| `src/compat/` | Platform compatibility (assumptions, endian, glibc shims, stdin) |
| `src/consensus/` | Consensus rules — flag functions with DigiDollar additions using ⚠️. EXCLUDE the DD-only files (`dca`, `err`, `volatility`, `digidollar`, `digidollar_tx`, `digidollar_transaction_validation`) — those go in `REPO_MAP_DIGIDOLLAR.md`. |
| `src/crypto/` | Crypto primitives — **all 5 active mining algorithms** (SHA-256d, Scrypt, Skein, Qubit, and either Groestl pre-Odo or Odocrypt post-Odo). Odocrypt replaces Groestl; Qubit remains active. |
| `src/index/` | Block/transaction indexes — EXCLUDE `digidollarstatsindex.{cpp,h}` (DD map) |
| `src/init/` | Node initialization (digibyted, digibyte-gui, digibyte-node, digibyte-qt, digibyte-wallet entry points) |
| `src/interfaces/` | Interface abstractions (chain, node, wallet, ipc, init, handler, echo) |
| `src/ipc/` | IPC / multiprocess (process, protocol, context, exception; Cap'n Proto bridge under `src/ipc/capnp/`) |
| `src/kernel/` | Kernel interface, chainparams, mempool entry/options/limits/persistence/removal |
| `src/logging/` | Logging system (timer) |
| `src/node/` | Node management (miner, blockstorage, chainstate, transaction, mini_miner, eviction, txreconciliation, kernel_notifications, ui_interface) |
| `src/policy/` | Mempool/relay policy (policy, packages, fees, rbf, settings) |
| `src/primitives/` | Block, transaction. NOTE: `oracle.{cpp,h}` is documented in `REPO_MAP_DIGIDOLLAR.md`. |
| `src/qt/` | Qt GUI (lighter coverage is fine) — EXCLUDE `digidollar*`, `digidollar_qt_translate.h`, and `ddaddressbookpage*` widgets (DD map); exclude generated `moc_*.cpp` and `forms/ui_*.h`. |
| `src/rpc/` | RPC interface — EXCLUDE `digidollar*.cpp/h` (those go in DigiDollar map) |
| `src/script/` | Script interpreter (`script.h` includes DigiDollar opcodes 0xbb..0xbf) |
| `src/support/` | Memory allocators, utility (`lockedpool`, `cleanse`, `events`) |
| `src/util/` | Utility functions |
| `src/wallet/` | Wallet — EXCLUDE DigiDollar-specific wallet code (`digidollarwallet.*`, `ddcoincontrol.*`, the `wallet/test/digidollar_*` and `rh59` tests) |
| `src/zmq/` | ZeroMQ notifications |

**Include tests (separate section):**

| Directory | Contents |
|-----------|----------|
| `src/test/` | C++ unit tests — list by area, skip individual TEST_CASE names. EXCLUDE `digidollar_*`, `oracle_*`, `musig2_*`, `redteam_*`, and `rh*` test files (DD map). |
| `test/functional/` | Python functional tests — list by area. EXCLUDE `digidollar_*`, `wallet_digidollar_*`, `feature_oracle_*`, and `rpc_getoracles_*` test files (DD map). |

**DO NOT index:**
- `src/crc32c/`, `src/leveldb/`, `src/secp256k1/`, `src/minisketch/`, `src/univalue/` — third-party libraries
- `src/digidollar/`, `src/oracle/` — covered by the DigiDollar map
- `src/.deps/`, `src/.libs/`, `src/obj/`, `src/config/` — build artifacts
- `*.o`, `*.lo`, built binaries, Qt `moc_*.cpp`, `src/qt/test/moc_*.cpp`, and `src/qt/forms/ui_*.h` — generated/build products
- `depends/`, `digibyte-v8.22.2/`, `bitcoin-v26.2-for-digibyte/`, `guix-build-*` — historical/external trees and build outputs (not the active codebase)
- `doc/`, `reports/`, `prompts/`, `presentation/`, `digidollar/` (top-level docs subdirectory) — documentation, not source code

### 2. `REPO_MAP_DIGIDOLLAR.md` — DigiDollar + Oracle Subsystem

Index ALL DigiDollar and Oracle source files, wherever they live in the repo.

**DigiDollar source files:**

| Location | Files |
|----------|-------|
| `src/digidollar/digidollar.cpp/h` | Core DigiDollar logic |
| `src/digidollar/health.cpp/h` | Health monitoring |
| `src/digidollar/scripts.cpp/h` | DigiDollar Taproot script construction |
| `src/digidollar/txbuilder.cpp/h` | Transaction builder (mint/transfer/redeem) |
| `src/digidollar/validation.cpp/h` | DigiDollar-specific validation |
| `src/rpc/digidollar.cpp/h` | DigiDollar / oracle RPC commands |
| `src/rpc/digidollar_transactions.cpp/h` | Legacy / unregistered DD transaction RPCs |
| `src/consensus/dca.cpp/h` | Dynamic Collateral Adjustment |
| `src/consensus/err.cpp/h` | Emergency Redemption Ratio |
| `src/consensus/volatility.cpp/h` | Volatility monitor |
| `src/consensus/digidollar.cpp/h` | DigiDollar consensus utilities (collateral tiers, marker, deployment helpers) |
| `src/consensus/digidollar_tx.cpp/h` | DD transaction structural rules |
| `src/consensus/digidollar_transaction_validation.cpp/h` | Full DD tx validation |
| `src/index/digidollarstatsindex.cpp/h` | DD supply/health stats index |
| `src/wallet/digidollarwallet.cpp/h` | Wallet-side DigiDollar handling |
| `src/wallet/ddcoincontrol.cpp/h` | DD-aware coin selection |
| Qt: `src/qt/digidollar*.{cpp,h}` and `src/qt/ddaddressbookpage.{cpp,h}` | DD GUI widgets |

**Oracle source files:**

| Location | Files |
|----------|-------|
| `src/oracle/bundle_manager.cpp/h` | Oracle bundle management |
| `src/oracle/exchange.cpp/h` | Exchange-rate fetchers (11 classes deriving from `BaseExchangeFetcher`; 6 are initialized in production via `MultiExchangeAggregator::InitializeFetchers`) |
| `src/oracle/mock_oracle.cpp/h` | Mock oracle for regtest |
| `src/oracle/node.cpp/h` | Oracle node implementation |
| `src/oracle/signing_orchestrator.cpp/h` | Signing-side orchestration for MuSig2 v0x03 nonce/context/partial-sig rounds |
| `src/oracle/musig2_aggregator.{cpp,h}`, `musig2_session.{cpp,h}`, `musig2_session_manager.{cpp,h}`, `musig2_orchestrator.{cpp,h}`, `musig2_oracle_participation.{cpp,h}`, `musig2_messages.h`, `musig2_session_mining.h` | MuSig2 v0x03 nonce/partial-sig session lifecycle |
| `src/primitives/oracle.cpp/h` | Oracle data structures (`COraclePriceMessage`, `COracleBundle`, `OracleNodeInfo`, `SelectOraclesForEpoch`, MuSig2 v0x03 fields) |

**Also find scattered references:**
```bash
rg -l "digidollar|DigiDollar" src \
  -g "*.cpp" -g "*.h" -g "*.hpp" \
  -g "!**/.deps/**" -g "!**/.libs/**" -g "!*.o" -g "!*.lo" \
  -g "!src/qt/moc_*.cpp" -g "!src/qt/test/moc_*.cpp" -g "!src/qt/forms/ui_*.h" \
  | sort
```
This catches files like `src/kernel/chainparams.cpp`, `src/primitives/transaction.cpp`, `src/base58.cpp`, etc. that contain DigiDollar integration points. List these with a note about what DigiDollar code they contain.

**DigiDollar + Oracle tests** (refer to `REPO_MAP_DIGIDOLLAR.md` for the canonical inventory; counts here are guidance only):

| Location | Approx. Count | Coverage Areas |
|----------|--------------|----------------|
| `src/test/digidollar_*.cpp`, `src/test/oracle_*.cpp`, `src/test/musig2_*.cpp`, `src/test/redteam_*.cpp`, `src/test/rh*.cpp` | ~160 files | activation, address, DCA, ERR, Thaw Day, chainstate health, amount parsing, MuSig2 nonce/partial-sig sessions, bundle creation/validation/format/mining, redteam audit (RH series), wallet integration, reindex and reorg, GUI/widget |
| `src/wallet/test/digidollar_*.cpp`, `src/wallet/test/rh59_*.cpp` | 7 registered files | persistence, wallet security, lock safety, Wave 16/17 load-rescan/spendability/helper-asymmetry coverage, lock-bypass |
| `src/qt/test/digidollarwidgettests.cpp`, `src/qt/test/digidollarwave19widgettests.cpp`, `src/qt/test/digidollarmintrecordtests.cpp` | 3 files | Qt widget behaviour, Wave 19 address/widget coverage, and saving a mint record before the transaction is sent |
| `test/functional/digidollar_*.py`, `wallet_digidollar_*.py`, `feature_digidollar_*.py`, `feature_oracle_*.py`, `rpc_getoracles_*.py` | 104 registered entries | activation, mint, transfer, redeem, amount units, Thaw Day, pruning, oracle P2P, wallet restore, MuSig2 integration, mempool ordering |

## REPO_MAP.md Format

```markdown
# REPO_MAP.md — DigiByte Core v9.26.2

*Generated: YYYY-MM-DD*

## Source Files

### src/validation.cpp
- `CheckBlock()` → validates block structure, merkle root, size limits, algo-specific PoW
- `ConnectBlock()` → connects validated block to chain, updates UTXO set
- `AcceptBlock()` → accepts block from network, checks PoW for correct algo, writes to disk
- `ActivateBestChain()` → selects and activates the best chain tip
- ⚠️ Contains DigiDollar activation height checks

### src/crypto/groestl.cpp
- `Groestl()` → Groestl hash function (1 of 5 DigiByte mining algorithms)
- `CGroestlHasher` (class)
  - `Write()` → feeds data into hasher
  - `Finalize()` → produces final hash

## Tests

### src/test/validation_tests.cpp
- Block validation edge cases, chain tip selection, reorg handling
```

### Description Rules
- ❌ `CheckBlock()` → "checks block" (useless — just restating the name)
- ✅ `CheckBlock()` → "validates block structure, merkle root, size limits, algo-specific PoW" (useful)
- Use ⚠️ to flag functions that contain DigiDollar-specific additions
- Note which of the 5 mining algorithms something relates to
- Every public class and function must appear; private helpers can be skipped

## How to Generate

### Step 1: List source files in scope
```bash
# Core DigiByte source surface (excluding third-party libs and DigiDollar/oracle modules)
rg --files src \
  -g '*.h' -g '*.cpp' -g '*.hpp' \
  -g '!crc32c/**' -g '!leveldb/**' -g '!secp256k1/**' -g '!minisketch/**' -g '!univalue/**' \
  -g '!digidollar/**' -g '!oracle/**' \
  -g '!**/.deps/**' -g '!**/.libs/**' -g '!*.o' -g '!*.lo' \
  -g '!src/qt/moc_*.cpp' -g '!src/qt/test/moc_*.cpp' -g '!src/qt/forms/ui_*.h' \
  | sort

# DigiDollar + Oracle source surface
rg --files src/digidollar src/oracle -g '*.cpp' -g '*.h' | sort
rg --files src/rpc -g 'digidollar*' | sort
rg -l "digidollar|DigiDollar|oracle|MuSig|musig" src \
  -g '*.cpp' -g '*.h' -g '*.hpp' \
  -g '!**/.deps/**' -g '!**/.libs/**' -g '!*.o' -g '!*.lo' \
  -g '!src/qt/moc_*.cpp' -g '!src/qt/test/moc_*.cpp' -g '!src/qt/forms/ui_*.h' \
  | sort

# Avoid the historical / external trees:
#   depends/ digibyte-v8.22.2/ bitcoin-v26.2-for-digibyte/ guix-build-*

# Generation note: raw `find ... | grep` and `grep -R` commands can pick up generated
# files in an already-built tree. Prefer the pruned `rg --files` forms above
# when refreshing either repo map.
```

### Step 2: Read each file, extract public classes and function signatures

### Step 3: Write useful one-liner descriptions

### Step 4: Organize by directory with tests in a separate section

## Existing Architecture Docs

These already exist — **do not recreate them.** The repo maps complement them.

**Core DigiByte:**
- `ARCHITECTURE.md` — System architecture (active maintained version)

**DigiDollar:**
- `DIGIDOLLAR_ARCHITECTURE.md` — Full DigiDollar system design
- `DIGIDOLLAR_EXPLAINER.md` — Plain language explanation
- `DIGIDOLLAR_ACTIVATION_EXPLAINER.md` — Activation mechanism (BIP9 bit 23)
- `DIGIDOLLAR_WALLET_INTEGRATION.md` / `DIGIDOLLAR_EXCHANGE_INTEGRATION.md` — Integration guides

**Oracle:**
- `DIGIDOLLAR_ORACLE_ARCHITECTURE.md` — Oracle network design
- `DIGIDOLLAR_ORACLE_EXPLAINER.md` — How oracles work
- `DIGIDOLLAR_ORACLE_SETUP.md` — Setup instructions
- `ORACLE_DISCOVERY_ARCHITECTURE.md` — Peer discovery
- `docs/ORACLE_OPERATOR_GUIDE.md` — Operator guide

**DigiDollar Subdirectory (`digidollar/`):**
- `whitepaper.md` — Full whitepaper
- `TECHNICAL_SPECIFICATION.md` — Technical spec
- Other supporting documents (flowcharts, collateral specs, oracle phase specs)

## How to Use Repo Maps

### Before any DigiByte work, read in order:
1. `ARCHITECTURE.md` — Core DigiByte design
2. `REPO_MAP.md` — Core file index
3. `DIGIDOLLAR_ARCHITECTURE.md` — if doing DigiDollar work
4. `DIGIDOLLAR_ORACLE_ARCHITECTURE.md` — if doing oracle work
5. `REPO_MAP_DIGIDOLLAR.md` — DigiDollar/Oracle file index

### When spawning sub-agents:
Always start their task prompt with:
```
Before writing any code, read these files in order:
1. ARCHITECTURE.md — DigiByte system design
2. REPO_MAP.md — Core file index
3. DIGIDOLLAR_ARCHITECTURE.md (if DigiDollar work)
4. DIGIDOLLAR_ORACLE_ARCHITECTURE.md (if oracle work)
5. REPO_MAP_DIGIDOLLAR.md (if DigiDollar/oracle work)
Then go to the specific files you need.
```

### After changing code:
If you add, remove, or rename files or functions, update the affected REPO_MAP.md and commit it with your changes.

## DigiByte-Specific Tips

- **5 Mining Algorithms:** SHA-256d, Scrypt, Skein, Qubit, and either Groestl before Odo activation or Odocrypt after `OdoHeight = 9,112,320`. Difficulty adjustment is via MultiShield/DigiSpeed and parameters in `src/kernel/chainparams.cpp`.
- **DigiDollar activation:** A buried deployment since v9.26.5 (BIP90; historically BIP9 bit 23). `Consensus::Params::DigiDollarHeight` is a plain per-network height and the version-bits state machine no longer runs for it: mainnet 23,869,440, testnet26 600, signet and regtest 0 (`src/kernel/chainparams.cpp:196, 549, 1000, 1135`). Separate static gates keep their historical floors: mainnet `nDDActivationHeight = 23,627,520` (`chainparams.cpp:342`), testnet26 600 (`chainparams.cpp:721`), regtest 650 (`chainparams.cpp:1266`). Testnet26 uses port 12033 and reset genesis timestamp 1780156800. `nDigiDollarMuSig2Height` equals `nDDActivationHeight` on mainnet and testnet26 (`chainparams.cpp:351, 723`); on regtest it is `std::min(nDDActivationHeight, DigiDollarHeight)` = 0 (`chainparams.cpp:1294-1296`). MuSig2 v0x03 oracle bundles are required as soon as DigiDollar activates. Full detail is in `DIGIDOLLAR_ACTIVATION_EXPLAINER.md`.
- **Thaw Day:** A separate coordinated change with one height per network, `Consensus::Params::nDDThawDayHeight`. Mainnet is scheduled at 24,490,000, estimated for November 1, 2026. Testnet26 is scheduled at 432,100, estimated for September 18–19, 2026. The heights trigger activation. Signet is unsupported and remains unscheduled (`std::numeric_limits<int>::max()`). Regtest can set it with `-ddthawdayheight=N`, which is a startup error elsewhere. Read it through `DigiDollar::IsThawDayActive(params, candidate_height)`. See `src/kernel/chainparams.cpp` and the rollout requirements in `DIGIDOLLAR_ACTIVATION_EXPLAINER.md`.
- **Pruning:** With `-prune` set, the node registers a prune lock named `digidollar` at `DigiDollar::EarliestActivationFloor(params)` and keeps every block from there to the tip (`src/node/chainstate.cpp:159-190`). On mainnet that floor is 23,627,520, so a small prune target cannot be reached there.
- **Oracle integration:** `src/primitives/oracle.h` defines structures, `src/oracle/` has the network/MuSig2 layer, and `src/consensus/{dca,err,volatility}.cpp` contain protection rules. The `g_get_oracle_consensus_price` hook exists in `src/script/interpreter.cpp` but is left null in production (`src/init.cpp:2283-2291`); `OP_CHECKPRICE` is deterministically disabled (DD-FINAL-005) and no longer consults it — it consumes its operand and pushes FALSE (`src/script/interpreter.cpp:708-735`).
- **BIP324 V2 P2P transport:** Implemented in `src/bip324.cpp`/`src/net.cpp`; opt in with `-v2transport=1`.
- **Bitcoin lineage:** Most of `src/` is inherited from Bitcoin Core v26.2. When writing descriptions, note DigiByte-specific modifications vs inherited Bitcoin code; the historical reference trees are in `digibyte-v8.22.2/` and `bitcoin-v26.2-for-digibyte/` (do **not** index those).
