# DigiByte v9.26.0-rc21 Release Notes

**WARNING: This is a TESTNET-ONLY release. DO NOT use on mainnet.**

**Development Branch:** https://github.com/DigiByte-Core/digibyte/tree/feature/digidollar-v1

**Join the Developer Chat:** https://app.gitter.im/#/room/#digidollar:gitter.im

---

## ⚠️ SAME TESTNET — NO RESET

**RC21 uses the same testnet19 chain as RC19/RC20.** No data migration needed.

- **No chain reset** — your blockchain data, wallets, and oracle keys all carry over
- **Same ports:** P2P **12033**, RPC **14025**
- **Same oracle consensus:** **5-of-9** Schnorr threshold
- **Just update the binary** and restart your node

### 🔑 Oracle Operators — IMPORTANT

After upgrading to RC21, **you must reload your oracle wallet and restart your oracle:**

```bash
digibyte-cli -testnet loadwallet "oracle"
digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>
```

Qt users: **File → Open Wallet → oracle**, then **Help → Debug Window → Console** → `startoracle <your_oracle_id>`

> **⚠️ After every node restart, you must re-load your wallet and re-start the oracle.** Your key persists in the wallet — you never need to run `createoraclekey` again.

---

## What's New in RC21

RC21 is a **critical hotfix release** that fixes the IBD sync failure at block 7586 that was still affecting fresh nodes on RC20. Despite the fix in RC20 for the `bad-oracle-price` IBD path, a second code path — the ERR (Emergency Redemption Ratio) pre-validation check — was blocking mints during IBD when no oracle price was available.

**If you cannot sync past block 7586 on RC20, upgrade to RC21.**

---

## 🐛 Critical Bug Fix

### IBD Sync Still Stuck at Block 7586 — ERR Minting Block

**Reported by:** @beggsdl and community testers on Gitter (Feb 16, 2026)

**The problem:** Fresh nodes syncing the testnet chain still get permanently stuck at block 7586 with `minting-blocked-during-err`. The RC20 fix addressed the `bad-oracle-price` path, but a second validation check — `ShouldBlockMintingDuringERR()` — was also rejecting the block.

**Root cause:** During IBD, oracle price is unavailable (returns 0). The first few MINT transactions in the chain pass the ERR check because `totalDDSupply == 0` (no liabilities = minting allowed, early return). But once earlier mints are connected and `totalDDSupply > 0`, `ShouldBlockMinting()` sees:
1. `totalDDSupply > 0` → can't short-circuit
2. `oraclePriceMicroUSD == 0` → fail-closed → **blocks all mints**

The ERR pre-validation check at line 1881 of `validation.cpp` was the **only** oracle-dependent check that did NOT guard on `ctx.skipOracleValidation`. Every other oracle check (lines 650, 656, 905, 1005, 1071, 1271, 1488) correctly skips during IBD.

**The fix:** Added `!ctx.skipOracleValidation` guard to the ERR minting check, matching the pattern used everywhere else in DigiDollar validation:

```cpp
// Before (bug):
if (txType == DD_TX_MINT && ShouldBlockMintingDuringERR(ctx)) {

// After (fix):
if (txType == DD_TX_MINT && !ctx.skipOracleValidation && ShouldBlockMintingDuringERR(ctx)) {
```

**Security analysis:** This is safe because:
- During IBD, blocks were already validated and accepted by the network
- `skipOracleValidation` is ONLY set true during IBD or catch-up sync
- Once synced to tip, `skipOracleValidation=false` and ERR enforcement is fully active
- A malicious node cannot exploit this because IBD only processes blocks from the best chain

**Impact:** Fresh nodes now sync the entire chain without getting stuck. This was the last remaining IBD blocker.

---

## 🧪 Testing

### New Tests (TDD)
3 new test cases added to `digidollar_skip_oracle_tests.cpp`:

| Test | What It Verifies |
|------|-----------------|
| `ibd_err_blocks_minting_with_nonzero_supply` | IBD with zero oracle + nonzero DD supply passes (the exact bug scenario) |
| `ibd_err_check_with_valid_oracle_and_healthy_system` | Healthy system passes in both IBD and non-IBD |
| `non_ibd_err_still_blocks_when_active` | ERR still blocks minting in non-IBD mode (security preserved) |

### Test Results
- **1,961 C++ unit tests** — all passing
- **DigiDollar functional tests** — all passing (mint, activation, oracle, redeem, protection, persistence, redemption_e2e)
- **6 skip-oracle tests** — all passing (3 existing + 3 new)

---

## 📊 All Changes: RC20 → RC21

| Category | Count | Summary |
|----------|-------|---------|
| 🐛 Bug fixes | 1 | ERR minting check blocks IBD sync at block 7586 |
| 🧪 Tests | 3 | TDD tests for IBD ERR minting bypass |
| 📦 Version | 1 | Bump to v9.26.0-rc21, update wallet image |

**Total: 2 commits**

---

## Technical Changes

| File | Change |
|------|--------|
| `src/digidollar/validation.cpp` | Add `!ctx.skipOracleValidation` guard to ERR minting check (line 1881) |
| `src/test/digidollar_skip_oracle_tests.cpp` | 3 new IBD ERR test cases |
| `configure.ac` | Version bump RC20 → RC21 |
| `src/qt/res/icons/digibyte_wallet.png` | Updated wallet splash image |

---

## Commits Since RC20

### Bug Fixes
```
e4fbe10d39 fix: skip ERR minting check during IBD to prevent sync failure at block 7586
```

### Version
```
<pending>  Bump version to v9.26.0-rc21, update wallet image
```

---

## What is DigiDollar?

DigiDollar is a USD-pegged stablecoin built natively into DigiByte. It uses an over-collateralized model where users lock DGB to mint DUSD at the current oracle price of DGB.

The world's first truly decentralized stablecoin native on a UTXO blockchain, enabling stable value transactions without centralized control.

DGB becomes the strategic reserve asset (21B max, only ~1.94 DGB per person on Earth). Everything happens inside DigiByte Core wallet. You never give up control of your private keys. No centralized company, fund or pool. Pure decentralization.

**Learn more:** https://digibyte.io/digidollar

---

## Wallet GUI Guide

### How to Use DigiDollar in the Wallet

1. **Open the DigiDollar Tab** — Click "DigiDollar" in the top navigation bar (will show activation status until BIP9 activates)
2. **Mint DigiDollars** — Lock DGB as collateral to create DUSD. Enter the amount and confirm.
3. **Send DigiDollars** — Use the DigiDollar tab's send function or "Send DGB" with a DD address
4. **Receive DigiDollars** — Click "Receive DGB" to get your DD-capable address
5. **View History** — DD Transactions tab shows complete transaction history (auto-refreshes)
6. **Redeem DigiDollars** — Burn DUSD to unlock your DGB collateral after the lock period expires
7. **Coin Control** — Use manual DD input selection for advanced redemptions
8. **Address Book** — Save frequently used DD addresses for quick sending
9. **Export History** — Export DD transactions to CSV for record keeping
10. **Mask Values** — Enable "Mask Values" to hide all balances and amounts across DGB and DigiDollar tabs for privacy

---

## Oracle Operator Setup

### Prerequisites
- DigiByte Core RC21 built from source with curl support (or download the binary)
- An assigned oracle ID (0–8 for testnet, contact the maintainer)

### New Oracle Setup (First Time)

```bash
# Step 1: Start your node
digibyted -testnet -daemon

# Step 2: Create a wallet
digibyte-cli -testnet createwallet "oracle"

# Step 3: Generate your oracle key (one-time)
digibyte-cli -testnet -rpcwallet=oracle createoraclekey <your_oracle_id>

# Step 4: Start the oracle
digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>
```

### Existing Oracle — Upgrading from RC20

**No chain reset.** Just update the binary and restart:

```bash
# Step 1: Stop your node
digibyte-cli -testnet stop

# Step 2: Replace the binary with RC21

# Step 3: Start your node
digibyted -testnet -daemon

# Step 4: Load your wallet and start oracle
digibyte-cli -testnet loadwallet "oracle"
digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>
```

### Existing Oracle — Upgrading from RC18 or Earlier

**Your oracle keys are safe.** Copy your oracle wallet from the old testnet directory to `testnet19`:

1. **Before deleting old data**, copy your oracle wallet:
   - **Linux:** Copy `~/.digibyte/testnet18/wallets/oracle/` to a safe location
   - **Windows:** Copy `%APPDATA%\DigiByte\testnet18\wallets\oracle\` to a safe location
   - **macOS:** Copy `~/Library/Application Support/DigiByte/testnet18/wallets/oracle/` to a safe location
2. **Install RC21** and start the node (it uses the `testnet19` directory)
3. **Copy your saved oracle wallet** into testnet19:
   - **Linux:** `~/.digibyte/testnet19/wallets/oracle/`
   - **Windows:** `%APPDATA%\DigiByte\testnet19\wallets\oracle\`
   - **macOS:** `~/Library/Application Support/DigiByte/testnet19/wallets/oracle/`
4. **Load and start your oracle:**
   ```bash
   digibyte-cli -testnet loadwallet "oracle"
   digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>
   ```

### Qt Wallet Users

1. Start DigiByte Qt with `-testnet`
2. Go to **File → Open Wallet → oracle** to load your wallet
3. Go to **Help → Debug Window → Console**
4. Type: `startoracle <your_oracle_id>`

### Current Oracle Operators (Testnet)

| ID | Operator | Status |
|----|----------|--------|
| 0 | Jared | ✅ Active |
| 1 | Green Candle | ✅ Active |
| 2 | Bastian | ✅ Active |
| 3 | DanGB | ✅ Active |
| 4 | Shenger | ✅ Active |
| 5 | Ycagel | ✅ Active |
| 6 | Aussie Epic | ✅ Active |
| 7 | LookIntoMyEyes | ✅ Active |
| 8 | JohnnyLawDGB | ✅ Active |

For the complete guide, see **`DIGIDOLLAR_ORACLE_SETUP.md`**.

---

## Complete RPC Command Reference

### DigiDollar Commands (Wallet)

| Command | Description |
|---------|-------------|
| `mintdigidollar` | Mint DigiDollars by locking DGB as collateral |
| `senddigidollar` | Send DigiDollars to another address |
| `redeemdigidollar` | Redeem DigiDollars to unlock DGB collateral |
| `getdigidollarbalance` | Show your DigiDollar balance |
| `listdigidollarpositions` | List your active collateral positions |
| `listdigidollartxs` | List your DigiDollar transaction history |
| `getdigidollaraddress` | Get or create a DigiDollar receive address |
| `validateddaddress` | Validate a DigiDollar address |
| `listdigidollaraddresses` | List all DigiDollar addresses in your wallet |
| `importdigidollaraddress` | Import a DigiDollar address for watch-only |
| `getdigidollarstats` | Get network-wide DigiDollar statistics |
| `getdigidollardeploymentinfo` | Get DigiDollar activation/deployment status |
| `calculatecollateralrequirement` | Calculate DGB collateral needed for a DD mint |
| `estimatecollateral` | Estimate collateral requirement by tier |
| `getdcamultiplier` | Get the current DCA multiplier for collateral |
| `getredemptioninfo` | Get info about redeeming a specific position |
| `getprotectionstatus` | Check if liquidation protection is active |

### Oracle Commands

| Command | Description |
|---------|-------------|
| `createoraclekey <id>` | Generate a new oracle Schnorr keypair (one-time) |
| `getoraclepubkey <id>` | Show the oracle public key stored in your wallet |
| `startoracle <id>` | Start running as an oracle operator |
| `stoporacle <id>` | Stop your oracle |
| `getoracleprice` | Get the consensus price |
| `getalloracleprices` | Per-oracle breakdown |
| `getoracles` | Network-wide oracle status |
| `listoracle` | Show your local oracle status |
| `sendoracleprice` | Manually submit a price (testing) |

---

## Upgrade Notes

### Upgrading from RC20

**No chain reset.** Simply replace the binary and restart:

1. **Stop your node:** `digibyte-cli -testnet stop`
2. **Replace the binary** with the RC21 download
3. **Start your node:** `digibyted -testnet -daemon` (or launch Qt)
4. **Reload your oracle** (if applicable — see Oracle section above)

Your blockchain data, wallets, oracle keys, and all configuration carry over unchanged.

### Upgrading from RC19

Same as above — RC21 is fully backward-compatible with the testnet19 chain.

### Upgrading from RC18 or Earlier

**RC19 reset the testnet chain.** If you're coming from RC18 or earlier, follow the RC19 migration steps:

1. **Save your oracle wallet** (if you run an oracle — see Oracle migration section above)
2. **Delete old testnet data:**
   - **Linux:** Delete `~/.digibyte/testnet18/` (and any older testnet directories)
   - **Windows:** Delete `%APPDATA%\DigiByte\testnet18\`
   - **macOS:** Delete `~/Library/Application Support/DigiByte/testnet18/`
3. **Download and install RC21**
4. **Update your `digibyte.conf`** — the `addnode` addresses remain the same:
   ```ini
   testnet=1
   
   [test]
   digidollar=1
   addnode=oracle1.digibyte.io
   ```
5. **Launch with `-testnet`** — a new `testnet19` directory will be created automatically
6. **Restore your oracle wallet** if applicable (see migration steps above)

---

## Configuration

### Minimum digibyte.conf for DigiDollar Testing:
```ini
testnet=1

[test]
digidollar=1
addnode=oracle1.digibyte.io
```

### Optional Settings:
```ini
[test]
txindex=1
digidollarstatsindex=1
algo=sha256d
```

---

## Known Issues

- `startoracle` must be re-run after every node restart
- DigiDollar features are disabled until BIP9 activation completes (~block 600 with continuous mining)
- Oracle prices show as 0 or "Not Reporting" until enough oracle operators restart their oracles
- Restoring wallets via `listdescriptors`/`importdescriptors` may show stale redeem buttons for already-redeemed DigiDollars (cosmetic — attempting to redeem shows proper error)

---

## Network Information

| Setting | Value |
|---------|-------|
| Network | Testnet (testnet19) |
| Default P2P Port | **12033** |
| Default RPC Port | **14025** |
| Oracle Node | oracle1.digibyte.io |
| Address Prefix | dgbt1... (bech32) |
| Multi-Algo Activation | Block 100 |
| BIP9 Signaling Start | Block 200 |
| BIP9 Threshold | 70% (140 of 200 blocks) |
| DD Activation (earliest) | Block 600 |
| Oracle Consensus | **5-of-9** Schnorr threshold |
| Exchange Sources | 6 (Binance, CoinGecko, KuCoin, Gate.io, HTX, Crypto.com) |

---

## Downloads

| Platform | File |
|----------|------|
| Windows 64-bit (Installer) | `digibyte-9.26.0-rc21-win64-setup.exe` |
| Windows 64-bit (Portable) | `digibyte-9.26.0-rc21-win64.zip` |
| macOS Apple Silicon (M1/M2/M3/M4) | `digibyte-9.26.0-rc21-arm64-apple-darwin.dmg` |
| macOS Intel | `digibyte-9.26.0-rc21-x86_64-apple-darwin.dmg` |
| Linux x86_64 | `digibyte-9.26.0-rc21-x86_64-linux-gnu.tar.gz` |
| Linux ARM64 (Raspberry Pi) | `digibyte-9.26.0-rc21-aarch64-linux-gnu.tar.gz` |

---

## Quick Start

### New to DigiDollar?

1. Download the binary for your platform (see Downloads above)
2. Create your config file (see Configuration section)
3. Launch with `-testnet` flag: `./digibyte-qt -testnet`
4. Wait for the blockchain to sync
5. Monitor BIP9 activation: `digibyte-cli -testnet getdigidollardeploymentinfo`
6. Once activated, the DigiDollar tab becomes fully functional
7. You need DGB to mint — ask in Gitter and someone can send you testnet DGB

### Want to Run an Oracle?

See the **Oracle Operator Setup** section above, or read `DIGIDOLLAR_ORACLE_SETUP.md` for the complete guide.

---

## Troubleshooting

### "DigiDollar tab says Not Activated"
- This is expected if BIP9 hasn't activated yet. DigiDollar features unlock after miners signal support and the activation threshold is reached.
- Monitor progress: `digibyte-cli -testnet getdigidollardeploymentinfo`

### "Sync stuck at block 7586" (FIXED in RC21)
- This was caused by the ERR minting check failing during IBD when no oracle price was available. Upgrade to RC21 and resync.
- If you copied blocks/chainstate from another node as a workaround, that still works too.

### "Can't connect to network"
- RC19/RC20/RC21 uses **port 12033** (not 12032 or earlier). Check your firewall.
- Make sure `addnode=oracle1.digibyte.io` is under `[test]` in your config

### "Old testnet data"
- If coming from RC18 or earlier, delete ALL old testnet directories (testnet10 through testnet18) — RC19+ uses testnet19

### "No wallet is loaded" when running oracle commands
- Load your wallet first: `digibyte-cli -testnet loadwallet "oracle"`
- For Qt: **File → Open Wallet → oracle**

### "Mining not working"
- Blocks 0-99 are scrypt-only. After block 100, multi-algo activates.
- Set `algo=sha256d` in config for fastest CPU mining after block 100

### "Oracle price shows 0 or N/A"
- Oracle prices require BIP9 activation first
- Wait for activation and sufficient oracle operators to come online after upgrading

### "Redeem button shows for already-redeemed DigiDollars"
- Known cosmetic issue when restoring wallets via descriptor import. The button shows but attempting to redeem displays the correct error message.

---

## Feedback & Community

Please report issues and feedback to help us prepare for mainnet launch.

When reporting bugs, start your message with **BUG:** and include: what happened, steps to reproduce, platform (Windows/Linux/Mac), and any error messages from your debug.log file.

- **Developer Chat (Gitter):** https://app.gitter.im/#/room/#digidollar:gitter.im
- **GitHub Issues:** https://github.com/DigiByte-Core/digibyte/issues
