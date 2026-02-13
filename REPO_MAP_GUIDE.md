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

DigiByte is a Bitcoin Core fork with **DigiDollar** (algorithmic stablecoin) built on top. This means two distinct layers need two separate repo maps:

```
digibyte/
├── ARCHITECTURE.md                        ← Exists: Core DigiByte design
├── REPO_MAP.md                            ← GENERATE THIS: Core DigiByte file index
├── REPO_MAP_DIGIDOLLAR.md                 ← GENERATE THIS: DigiDollar + Oracle file index
├── DIGIDOLLAR_ARCHITECTURE.md             ← Exists: DigiDollar system design (2,220 lines)
├── DIGIDOLLAR_ORACLE_ARCHITECTURE.md      ← Exists: Oracle network design (1,908 lines)
└── ORACLE_DISCOVERY_ARCHITECTURE.md       ← Exists: Oracle peer discovery
```

## What to Generate

### 1. `REPO_MAP.md` (Repo Root) — Core DigiByte

Index all C++ source code in `src/` and its subdirectories, plus tests.

**Include these directories:**

| Directory | Contents |
|-----------|----------|
| `src/` (root files) | ~167 top-level .cpp/.h — main.cpp, validation.cpp, miner.cpp, net.cpp, txmempool.cpp, chainparams, etc. |
| `src/bench/` | Benchmarks |
| `src/common/` | Common utilities |
| `src/compat/` | Platform compatibility |
| `src/consensus/` | Consensus rules — flag functions with DigiDollar additions using ⚠️ |
| `src/crypto/` | Crypto primitives — **all 5 mining algorithms** (SHA-256d, Scrypt, Groestl, Skein, Qubit) |
| `src/index/` | Block/transaction indexes |
| `src/init/` | Node initialization |
| `src/interfaces/` | Interface abstractions |
| `src/ipc/` | IPC / multiprocess |
| `src/kernel/` | Kernel interface, chainparams |
| `src/logging/` | Logging system |
| `src/node/` | Node management |
| `src/policy/` | Mempool/relay policy |
| `src/primitives/` | Block, transaction, oracle structures |
| `src/qt/` | Qt GUI (lighter coverage is fine) |
| `src/rpc/` | RPC interface — EXCLUDE `digidollar*.cpp/h` (those go in DigiDollar map) |
| `src/script/` | Script interpreter |
| `src/support/` | Memory allocators, utility |
| `src/util/` | Utility functions |
| `src/wallet/` | Wallet — EXCLUDE DigiDollar-specific wallet code |
| `src/zmq/` | ZeroMQ notifications |

**Include tests (separate section):**

| Directory | Contents |
|-----------|----------|
| `src/test/` | C++ unit tests (~288 files) — list by area, skip individual TEST_CASE names. EXCLUDE `digidollar_*` and `oracle_*` test files |
| `test/functional/` | Python functional tests (~311 files) — list by area, EXCLUDE `digidollar_*`, `oracle_*`, and `wallet_digidollar_*` test files |

**DO NOT index:**
- `src/crc32c/`, `src/leveldb/`, `src/secp256k1/`, `src/minisketch/`, `src/univalue/` — third-party libraries
- `src/digidollar/`, `src/oracle/` — covered by the DigiDollar map
- `src/.deps/`, `src/.libs/`, `src/obj/`, `src/config/` — build artifacts
- `depends/` — contains full copies of Bitcoin Core v26.2 and DigiByte v8.22.2 (thousands of dependency files, not the active codebase)
- `doc/`, `reports/`, `prompts/`, `presentation/`, `digidollar/` — documentation, not source code

### 2. `REPO_MAP_DIGIDOLLAR.md` — DigiDollar + Oracle Subsystem

Index ALL DigiDollar and Oracle source files, wherever they live in the repo.

**DigiDollar source files:**

| Location | Files |
|----------|-------|
| `src/digidollar/digidollar.cpp/h` | Core DigiDollar logic |
| `src/digidollar/health.cpp/h` | Health monitoring |
| `src/digidollar/scripts.cpp/h` | DigiDollar script operations |
| `src/digidollar/txbuilder.cpp/h` | Transaction builder |
| `src/digidollar/validation.cpp/h` | DigiDollar-specific validation |
| `src/rpc/digidollar.cpp/h` | DigiDollar RPC commands |
| `src/rpc/digidollar_transactions.cpp/h` | DigiDollar transaction RPCs |
| `src/consensus/dca.cpp` | Dollar Cost Averaging consensus rules |
| `src/consensus/err.cpp` | Exchange Rate Resolution consensus rules |

**Oracle source files:**

| Location | Files |
|----------|-------|
| `src/oracle/bundle_manager.cpp/h` | Oracle bundle management |
| `src/oracle/exchange.cpp/h` | Exchange rate handling |
| `src/oracle/mock_oracle.cpp/h` | Mock oracle for testing |
| `src/oracle/node.cpp/h` | Oracle node implementation |
| `src/primitives/oracle.h` | Oracle data structures |

**Also find scattered references:**
```bash
grep -rl "digidollar\|DigiDollar" src/ --include="*.cpp" --include="*.h" | sort
```
This catches files like `src/kernel/chainparams.cpp`, `src/primitives/transaction.cpp`, `src/base58.cpp`, etc. that contain DigiDollar integration points. List these with a note about what DigiDollar code they contain.

**DigiDollar + Oracle tests:**

| Location | Count | Coverage Areas |
|----------|-------|----------------|
| `src/test/digidollar_*.cpp` | ~30 files | activation, address, bughunt, change, consensus, DCA, ERR, GUI, health, mint, opcodes, oracle, P2P, persistence (keys/serialization/walletbatch), redeem, restore, RPC, scripts, structures, timelock, transaction, transfer, txbuilder, UTXO lifecycle, validation, volatility, wallet |
| `src/test/oracle_*.cpp` | ~11 files | block validation, bundle manager, config, exchange, integration, message, miner, P2P, phase2, RPC, wallet keys |
| `test/functional/digidollar_*.py` | ~35 files | activation (basic + boundary), basic, encrypted wallet, mint, network (relay + tracking), oracle (basic + keygen + phase2), persistence, phase2 integration, protection, redeem (basic + stats + amounts + e2e), RPC (addresses/collateral/DCA/deployment/estimate/gating/oracle/protection/redemption), stress, transactions, transfer, wallet |
| `test/functional/wallet_digidollar_*.py` | ~6 files | backup, descriptors, encryption, persistence restart, rescan, restore |
| `test/functional/feature_oracle_p2p.py` | 1 file | Oracle P2P networking |
| `test/functional/rpc_getoracles_pending.py` | 1 file | Oracle pending RPC |

## REPO_MAP.md Format

```markdown
# REPO_MAP.md — DigiByte Core v9.26

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
# Core DigiByte (exclude third-party libs and DigiDollar)
find src/ -maxdepth 1 \( -name "*.cpp" -o -name "*.h" \) | sort
find src/bench src/common src/compat src/consensus src/crypto src/index src/init \
     src/interfaces src/ipc src/kernel src/logging src/node src/policy src/primitives \
     src/rpc src/script src/support src/util src/wallet src/zmq \
     \( -name "*.cpp" -o -name "*.h" \) 2>/dev/null | sort

# DigiDollar + Oracle
find src/digidollar src/oracle \( -name "*.cpp" -o -name "*.h" \) | sort
find src/rpc -name "digidollar*" | sort
grep -rl "digidollar\|DigiDollar" src/ --include="*.cpp" --include="*.h" | sort
```

### Step 2: Read each file, extract public classes and function signatures

### Step 3: Write useful one-liner descriptions

### Step 4: Organize by directory with tests in a separate section

## Existing Architecture Docs

These already exist — **do not recreate them.** The repo maps complement them.

**Core DigiByte:**
- `ARCHITECTURE.md` — System architecture (1,425 lines)

**DigiDollar:**
- `DIGIDOLLAR_ARCHITECTURE.md` — Full DigiDollar system design (2,220 lines)
- `DIGIDOLLAR_EXPLAINER.md` — Plain language explanation
- `DIGIDOLLAR_ACTIVATION_EXPLAINER.md` — Activation mechanism
- `DIGIDOLLAR_MVP_STATUS.md` — Current implementation status
- `DIGIDOLLAR_BUGS.md` — Known bugs

**Oracle:**
- `DIGIDOLLAR_ORACLE_ARCHITECTURE.md` — Oracle network design (1,908 lines)
- `DIGIDOLLAR_ORACLE_EXPLAINER.md` — How oracles work
- `DIGIDOLLAR_ORACLE_SETUP.md` — Setup instructions
- `ORACLE_DISCOVERY_ARCHITECTURE.md` — Peer discovery

**DigiDollar Subdirectory (`digidollar/`):**
- `whitepaper.md` — Full whitepaper
- `TECHNICAL_SPECIFICATION.md` — Technical spec
- `DIGIDOLLAR_FLOWCHART.md` — Transaction flow
- `4_tier_collateral.md` — 4-tier collateral system
- `DIGIDOLLAR_ORACLE_PHASE_ONE_SPEC.md` — Oracle Phase 1 spec
- `ORACLE_PHASE_2_SPEC_PRD.md` — Oracle Phase 2 PRD

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

- **5 Mining Algorithms:** SHA-256d, Scrypt, Groestl, Skein, Qubit — each has crypto functions in `src/crypto/`, difficulty adjustment via MultiShield/DigiShield, and parameters in `src/kernel/chainparams.cpp`
- **DigiDollar activation:** Controlled by consensus params (`nDigiDollarActivationHeight`) in chainparams
- **Oracle integration:** `src/primitives/oracle.h` defines structures, `src/oracle/` has the network layer, `src/consensus/dca.cpp` and `src/consensus/err.cpp` have consensus rules
- **Bitcoin lineage:** Most of `src/` is inherited from Bitcoin Core v26.2. When writing descriptions, note DigiByte-specific modifications vs inherited Bitcoin code
