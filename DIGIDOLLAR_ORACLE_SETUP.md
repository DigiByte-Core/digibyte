# DigiDollar Oracle Setup Guide

*The single source of truth for oracle operator setup — multi-oracle MuSig2 (V1 activates alongside DigiDollar; `nDigiDollarMuSig2Height = nDDActivationHeight` on mainnet and testnet26. On default regtest `nDigiDollarMuSig2Height = std::min(nDDActivationHeight, DigiDollarHeight) = 0`.)*

---

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [New Oracle Setup](#new-oracle-setup)
- [Upgrading to a New Release](#upgrading-to-a-new-release)
- [Restarting After a Reboot or Crash](#restarting-after-a-reboot-or-crash)
- [What Your Oracle Does](#what-your-oracle-does)
- [Consensus Parameters](#consensus-parameters)
- [Monitoring](#monitoring)
- [Troubleshooting](#troubleshooting)
- [RPC Command Reference](#rpc-command-reference)
  - [Oracle RPCs](#oracle-rpcs)
  - [DigiDollar RPCs](#digidollar-rpcs)
  - [Mock Oracle RPCs (regtest only)](#mock-oracle-rpcs-regtest-only)
- [For the Maintainer](#for-the-maintainer)
- [Server Requirements](#server-requirements)
- [File Locations](#file-locations)
- [Fixing Wallet Name](#fixing-wallet-name)

---

## Overview

DigiDollar requires oracle operators to provide real-time DGB/USD price feeds. Oracle public keys are hardcoded in `src/kernel/chainparams.cpp`. The current source tree uses `testnet26` (P2P port 12033, RPC port 14026, data dir `~/.digibyte/testnet26/`); historical testnet25/testnet24/testnet23/testnet21 details are preserved only for operators decommissioning an old install. The workflow:

1. Generate an oracle keypair via `createoraclekey` (stored in your wallet)
2. Send your **public key only** to the maintainer
3. Maintainer adds it to chainparams and ships a new release
4. On startup, load the wallet and verify the oracle is running. If auto-start does not trigger, run `startoracle` manually.

---

## Prerequisites

### Build DigiByte Core

```bash
cd ~/Code/digibyte
./autogen.sh
./configure
make -j$(nproc)
```

Verify curl support (required for exchange price fetching):
```bash
ldd ./src/digibyted | grep curl
# If missing: sudo apt install libcurl4-openssl-dev
```

### Configuration

Create or edit `~/.digibyte/digibyte.conf`:

```ini
testnet=1

[test]
digidollar=1
txindex=1
server=1
listen=1
addnode=oracle1.digibyte.io
```

> **`testnet=1` goes at the top** (not under any section). Everything else under `[test]`.

`addnode=oracle1.digibyte.io` resolves onto the current `testnet26` chain on port **12033**. If you want to pin the port explicitly:

```ini
addnode=oracle1.digibyte.io:12033
```

(The retired `testnet25` chain used port `12032`; `testnet24` used port `12031`; `testnet23` used port `12030`; the older `testnet21` chain used port `12035`. See the historical footnote at the end if you are decommissioning an old install.)

Optional:
```ini
[test]
digidollarstatsindex=1
algo=sha256d
```

---

For two nodes behind one router, compact filter service and temporary debug
logging, see [node operations](doc/digidollar-operations.md). Routine operation
does not need `debug=digidollar` or `debug=net`.

## New Oracle Setup

For first-time oracle operators. You need an assigned active oracle ID (slot **0–34**) from the maintainer; mainnet and testnet26 both use 35 consensus-active MuSig2 public keys with a 7-signature quorum. Slot ID 35 is outside the configured 35-slot roster.

```bash
# 1. Start your node
digibyted -testnet -daemon

# 2. Create a wallet (use just the name, NOT a full file path!)
digibyte-cli -testnet createwallet "oracle"

# 3. Generate your oracle key (one-time only)
digibyte-cli -testnet -rpcwallet=oracle createoraclekey <your_oracle_id>

# 4. Optional: export an offline recovery copy of your oracle private key
#    This prints sensitive signing material. Run only on a trusted local machine.
digibyte-cli -testnet -rpcwallet=oracle exportoracleprivkey <your_oracle_id>

# 5. Send your pubkey (66-char hex starting with 02/03) to the maintainer
#    NEVER share your private key.

# 6. After the maintainer ships a release with your key, start your oracle:
digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>

# 7. Verify
digibyte-cli -testnet getoracles true
```

**Qt wallet users:** Create wallet via **File → Create Wallet**, name it `oracle`. Then **Help → Debug Window → Console** to run `createoraclekey` and `startoracle`.

> ⚠️ **Use just the wallet name** (`"oracle"`), not a full path like `"/home/user/.digibyte/testnet26/wallets/oracle/"`. See [Fixing Wallet Name](#fixing-wallet-name) if you already did this.

---

## Upgrading to a New Release

Your oracle key persists in your wallet across upgrades. You do **not** need to generate a new key.

### Current testnet26 restart / upgrade

Follow the [upgrade and recovery guide](doc/digidollar-operations.md). Keep
existing wallet backups and retained block/undo data. Mainnet Thaw Day is
scheduled at 24,490,000, estimated for November 1, 2026. Testnet26 is scheduled
at 432,100, estimated for September 18–19, 2026. The heights trigger activation.
Signet is unsupported and remains unscheduled. Public activation, soak testing
and release verification still need to be completed.

```bash
# 1. Back up the loaded wallet, then stop cleanly
digibyte-cli -testnet -rpcwallet=oracle backupwallet "/secure/backup/oracle.dat"
digibyte-cli -testnet stop

# 2. Wait for shutdown, then install the reviewed replacement binaries

# 3. Start your node
digibyted -testnet -daemon

# 4. Load your wallet
digibyte-cli -testnet loadwallet "oracle"

# 5. If needed, manually start your oracle
digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>

# 6. Verify
digibyte-cli -testnet getoracles true
```

**Qt wallet users:** Start Qt → **File → Open Wallet → oracle**. If the oracle does not come up automatically, open **Console** and run `startoracle <your_oracle_id>`.

> **Auto-start behavior:** since RC25, unencrypted oracle wallets auto-start when the wallet loads (`CWallet::TryAutoStartOracles` in `src/wallet/wallet.cpp:4904`), and encrypted wallets auto-start after `walletpassphrase` unlock. Keep the manual `startoracle` command handy anyway, because it remains the safest fallback if the oracle is not already running.

### Decommissioning retired testnets

If you are still running the retired `testnet25` chain (port **12032**, data dir `~/.digibyte/testnet25/`), `testnet24` chain (port **12031**, data dir `~/.digibyte/testnet24/`), `testnet23` chain (port **12030**, data dir `~/.digibyte/testnet23/`), or older `testnet21` chain (port **12035**, data dir `~/.digibyte/testnet21/`), migration to current `testnet26` (port **12033**, data dir `~/.digibyte/testnet26/`) is a **fresh chain**: do **not** copy old `blocks/` or `chainstate/`. Migrate only the wallet that holds your oracle key, then follow the New Oracle Setup steps above against the fresh testnet26 directory.

### Restoring an oracle key into a fresh wallet

Use this only when normal wallet restore is not available or you need to move an assigned oracle key into a new wallet:

```bash
digibyte-cli -testnet createwallet "oracle"

# If the wallet is encrypted, unlock it before importing:
# digibyte-cli -testnet -rpcwallet=oracle walletpassphrase "<passphrase>" 600

digibyte-cli -testnet -rpcwallet=oracle importoracleprivkey <your_oracle_id> <private_key_hex>
digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>
```

`importoracleprivkey` rejects an existing key unless you pass `true` for the optional `replace` argument. The RPC returns `authorized=false` when the imported key does not match the current chainparams slot; in that case the key is stored, but `startoracle` will not run until the public key is authorized for that oracle ID.

---

## Restarting After a Reboot or Crash

Use this sequence after a reboot or crash:

```bash
digibyted -testnet -daemon
digibyte-cli -testnet loadwallet "oracle"
digibyte-cli -testnet -rpcwallet=oracle startoracle <your_oracle_id>
```

If the oracle auto-started when the wallet loaded, the explicit `startoracle` may say it is already running. That is fine.

---

## What Your Oracle Does

Once running, the oracle automatically:
- Fetches DGB/USD prices from multiple exchanges every 60 seconds
- Calculates median price with median-distance outlier filtering. The `MultiExchangeAggregator` default `min_required_sources` is **2** (`src/oracle/exchange.h:235`), but the live oracle daemon (`OracleNode::FetchMedianPrice`, `src/oracle/node.cpp:445-450`) overrides this to **3** valid exchange responses before publishing
- Signs the price with BIP-340 Schnorr using your wallet-stored private key
- Broadcasts the signed message to the P2P network

### Exchange Sources (no API keys required)

Active feeders, registered in `src/oracle/exchange.cpp:1092-1097`:

- Binance (DGB/USDT plus DGB/BTC × BTC/USDT cross)
- CoinGecko
- KuCoin
- Gate.io
- HTX
- Crypto.com

The fetcher classes for Coinbase, Kraken, Bittrex, Poloniex, and Messari still
exist in `src/oracle/exchange.h` but are **not enabled** — the in-source comment
at `src/oracle/exchange.cpp:1087-1089` documents the reasons (DGB unlisted /
removed / paid API key required).

### Price Format

| Format | Unit | Example |
|--------|------|---------|
| Internal (wire) | micro-USD | `50000` = $0.05 |
| `setmockoracleprice` RPC input | micro-USD | `50000` = $0.05 |
| Consensus valid range | micro-USD | 100 – 100,000,000 (`$0.0001` – `$100.00`) |
| Production exchange sanity cap | micro-USD | 10,000,000 (`$10.00`) |
| Regtest mock RPC range | micro-USD | 100 – 1,000,000,000 (`$0.0001` – `$1000.00`) |

---

## Consensus Parameters

### Current testnet settings

| Release | Chain | Testnet P2P Port | Active Oracles | Consensus Required |
|---------|-------|------------------|----------------|--------------------|
| Current source tree | `testnet26` | **12033** | 35 active | 7 signatures |
| Retired | `testnet24` | 12031 | n/a (chain retired) | n/a |
| Retired | `testnet23` | 12030 | n/a (chain retired) | n/a |
| Retired | `testnet21` | 12035 | n/a (chain retired) | n/a |

### Current source tree consensus values

| Parameter | Testnet | Regtest | Mainnet |
|-----------|---------|---------|---------|
| Active Oracles (`nOraclePubkeyCount`) | 35 | 7 | 35 |
| Reserved slots (`nOracleTotalOracles`) | 35 | 7 | 35 |
| Consensus Required (`nOracleConsensusRequired`) | 7 | 4-of-7 | 7 |
| Static DD height (`nDDActivationHeight`) | 600 | 650 | 23,627,520 |
| Buried DigiDollar height (`DigiDollarHeight`) | 600 | 0 | 23,869,440 |
| Thaw Day (`nDDThawDayHeight`) | 432,100 | Disabled unless configured | 24,490,000 |
| Rotation Interval (`nDDOracleEpochBlocks`) | 40 blocks | 40 blocks | 40 blocks |
| Price Update Interval (`nDDOracleUpdateInterval`) | 2 blocks | 1 block | 4 blocks |
| Bundle/MuSig2 Epoch (`nOracleEpochLength`) | 40 blocks | 40 blocks | 40 blocks |
| Oracle Broadcast Interval | 60 seconds | 60 seconds | 60 seconds |
| MuSig2 Activation (`nDigiDollarMuSig2Height`) | 600 | 0 | 23,627,520 |

Values verified against `src/kernel/chainparams.cpp`. Mainnet and testnet have
35 `vOracleNodes` metadata entries and slots 0-34 are in
`consensus.vOraclePublicKeys` for the current V1 quorum. Slot 28 uses
the DigiHash Mining Pool key, slot 31 uses the Peer2Peer / DigiRoos key, and
all 35 configured slots contain valid compressed secp256k1 oracle keys. Regtest
has 7 active slots.
The active oracle pubkey count (`nOraclePubkeyCount`)
and consensus threshold
(`nOracleConsensusRequired`) are configured per-network in
`src/kernel/chainparams.cpp` and override the legacy constants in
`src/primitives/oracle.h`.

---

## Monitoring

```bash
# Current testnet26 log path
tail -f ~/.digibyte/testnet26/debug.log | grep -i "oracle\|digidollar"

# Check current oracle price
digibyte-cli -testnet getoracleprice

# List all oracles and their status
digibyte-cli -testnet getoracles true

# Check local oracle status
digibyte-cli -testnet listoracle

# Get your oracle's public key and status
digibyte-cli -testnet getoraclepubkey <oracle_id>
```

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `"No wallet is loaded"` | Run `loadwallet "oracle"` first, then add `-rpcwallet=oracle` to commands |
| `"Oracle key already exists"` | Key is already in your wallet — no need to recreate |
| Need to move a key into a fresh wallet | Use `exportoracleprivkey` from the source wallet and `importoracleprivkey` into the new wallet; unlock encrypted wallets first |
| `"already has an oracle key"` during import | The wallet already has a key for that oracle ID; verify the existing key or pass `true` as the `replace` argument |
| `"Oracle ID not found in chain parameters"` | Your key isn't in chainparams yet — wait for next release |
| `"Oracle not configured"` | Run `createoraclekey` first (new operators) or `loadwallet` (existing) |
| Oracle not running after restart | Since RC25 the wallet auto-starts the oracle (`CWallet::TryAutoStartOracles`); if it doesn't, run `loadwallet` + `walletpassphrase` (if encrypted) + `startoracle` |
| Price fetch failures | Check `ldd digibyted | grep curl` and internet connectivity |
| Wallet name shows full file path | See [Fixing Wallet Name](#fixing-wallet-name) |

---

## RPC Command Reference

### Oracle RPCs

#### `createoraclekey` *(wallet RPC)*
Generate an oracle Schnorr keypair and store it in your wallet. One-time only.

```
digibyte-cli -testnet -rpcwallet=oracle createoraclekey <oracle_id>
```
Returns `pubkey` (33-byte compressed) and `pubkey_xonly` (32-byte x-only). Send the `pubkey` to the maintainer. Rejects if a key already exists for that ID.

#### `exportoracleprivkey` *(wallet RPC)*
Export a wallet-stored oracle private key as 32-byte hex for backup or migration. The wallet must be unlocked if encrypted.

```
digibyte-cli -testnet -rpcwallet=oracle exportoracleprivkey <oracle_id>
```

The returned `private_key` is sensitive oracle signing material. Store it offline and never share it with the maintainer or other operators.

#### `importoracleprivkey` *(wallet RPC)*
Import a wallet-stored oracle private key into the loaded wallet. The wallet must be unlocked if encrypted.

```
digibyte-cli -testnet -rpcwallet=oracle importoracleprivkey <oracle_id> <private_key_hex> [replace]
```

The optional `replace` argument defaults to `false`. Importing a key that does not match the current chainparams slot succeeds but returns `authorized=false`; the key is stored for recovery, but `startoracle` will refuse to run until that public key is authorized for the oracle ID.

#### `startoracle` *(wallet RPC)*
Start the oracle price feed thread. Loads the private key from your wallet.

```
digibyte-cli -testnet -rpcwallet=oracle startoracle <oracle_id>
```
Normal setup loads the oracle private key from the wallet. Must be re-run after
every node restart.

#### `stoporacle`
Stop a running oracle.

```
digibyte-cli -testnet stoporacle <oracle_id>
```

#### `getoraclepubkey`
Get an oracle's public key and running status. When called through a wallet RPC
path, this can show the wallet-stored oracle key before `startoracle`; in that
case `is_running` is `false` until the oracle runtime is started.

```
digibyte-cli -testnet getoraclepubkey <oracle_id>
```
Returns: `oracle_id`, `pubkey`, `pubkey_full`, `valid`, `authorized`, `is_running`.

#### `getoracles`
List all oracles from chainparams with their status.

```
digibyte-cli -testnet getoracles [active_only] [blocks]
```
Returns an array, one object per oracle slot, with: `oracle_id`, `name`,
`pubkey`, `endpoint`, `is_active`, `active_oracle_count`, `total_oracle_slots`,
`consensus_threshold`, `in_consensus`, `last_price_micro_usd`, `last_price_usd`,
`last_update`, `price_source`, `status`, `selected_for_epoch`,
`is_running_locally`, and the signed heartbeat fields `heartbeat_status`,
`software_version`, `client_version`, `p2p_protocol_version`,
`oracle_protocol_version`, `musig2_context_version`, `heartbeat_timestamp`,
`heartbeat_age_seconds` and `heartbeat_signature_valid`.

#### `listoracle`
Show the status of the oracle running on this local node (no parameters).

```
digibyte-cli -testnet listoracle
```

#### `getalloracleprices`
Get price data from all active oracles.

```
digibyte-cli -testnet getalloracleprices
```

#### `sendoracleprice` — REMOVED
> **Security note:** `sendoracleprice` was removed as a security vulnerability. Oracle operators must NOT be able to inject arbitrary prices. Oracle prices come exclusively from live exchange aggregation via `startoracle`. There is no operator-facing RPC for direct price submission anywhere in the current source tree.

### DigiDollar RPCs

> **Amounts.** Every DigiDollar amount below is a whole number of cents: `10000`
> is $100.00. `senddigidollar`, `sendmanydigidollar`, `redeemdigidollar`,
> `getredemptioninfo`, `listdigidollarpositions` and `listdigidollaraddresses`
> also take an optional trailing `amount_unit` of `"cents"` or `"dollars"`. With
> `dollars` the amount may have up to two decimals, so `100.00` is $100.00. An
> amount written with a decimal point and no unit is refused, not guessed,
> because `10000.00` could mean $100.00 or $10,000.00. A single typed amount may
> not exceed 10,000,000 cents ($100,000.00).

#### `getoracleprice`
Get the current consensus DGB/USD oracle price.

```
digibyte-cli -testnet getoracleprice
```
Returns: `price_micro_usd`, `price_cents`, `price_usd`, `last_update_height`, `last_update_time`, `validity_blocks`, `is_stale`, `oracle_count`, `status`, `24h_high`, `24h_low`, `volatility`.

#### `mintdigidollar` *(wallet RPC)*
Mint DigiDollars by locking DGB as collateral.

```
digibyte-cli -testnet -rpcwallet=<wallet> mintdigidollar <amount> <lock_tier>
```

#### `senddigidollar` *(wallet RPC)*
Send DigiDollars to an address.

```
digibyte-cli -testnet -rpcwallet=<wallet> senddigidollar <address> <amount>
```

#### `sendmanydigidollar` *(wallet RPC)*
Send DigiDollars to multiple addresses. The first argument is the required `sendmany` compatibility dummy string; use integer cents in operator scripts.

```
digibyte-cli -testnet -rpcwallet=<wallet> sendmanydigidollar "" '{"TDaddr1...":1500,"TDaddr2...":2500}'
```

#### `redeemdigidollar` *(wallet RPC)*
Redeem DigiDollars to unlock collateral (after lock period expires).

```
digibyte-cli -testnet -rpcwallet=<wallet> redeemdigidollar <position_txid> <dd_amount>
```

#### `getdigidollarbalance` *(wallet RPC)*
Get your DigiDollar balance (`confirmed`, `unconfirmed`, and `total`; amounts are cents).

```
digibyte-cli -testnet -rpcwallet=<wallet> getdigidollarbalance
```

#### `listdigidollarunspent` / `listdigidollarutxos` *(wallet RPC)*
List spendable DigiDollar UTXOs for coin control or selected-input sends.

```
digibyte-cli -testnet -rpcwallet=<wallet> listdigidollarunspent
digibyte-cli -testnet -rpcwallet=<wallet> listdigidollarutxos
```

#### `listdigidollarpositions` *(wallet RPC)*
List all your active DD positions (minted, locked collateral).

```
digibyte-cli -testnet -rpcwallet=<wallet> listdigidollarpositions
```

#### `listdigidollartxs` *(wallet RPC)*
List DigiDollar transaction history.

```
digibyte-cli -testnet -rpcwallet=<wallet> listdigidollartxs [count]
```

#### `getdigidollaraddress` *(wallet RPC)*
Get a new DigiDollar receiving address.

```
digibyte-cli -testnet -rpcwallet=<wallet> getdigidollaraddress
```

#### `listdigidollaraddresses` *(wallet RPC)*
List all DigiDollar addresses in a wallet.

```
digibyte-cli -testnet -rpcwallet=<wallet> listdigidollaraddresses
```

#### `importdigidollaraddress`
Validate a DigiDollar address and return the V1 unsupported/no-op warning.
This command does not import, mutate wallet state, or rescan.

```
digibyte-cli -testnet importdigidollaraddress <address> [label]
```

#### `validateddaddress`
Validate a DigiDollar address.

```
digibyte-cli -testnet validateddaddress <address>
```

#### `getdigidollarstats`
Get network-wide DigiDollar statistics (total supply, collateral locked, etc.).

```
digibyte-cli -testnet getdigidollarstats
```

#### `getdigidollardeploymentinfo`
Get buried DigiDollar activation status and separate Thaw Day tip/next-block status.

```
digibyte-cli -testnet getdigidollardeploymentinfo
```

#### `getdcamultiplier`
Get the current DCA (Dynamic Collateral Adjustment) multiplier.

```
digibyte-cli -testnet getdcamultiplier
```

#### `calculatecollateralrequirement`
Calculate collateral required for a given DD amount and lock duration in days.

```
digibyte-cli -testnet calculatecollateralrequirement <dd_amount_cents> <lock_days> [oracle_price_micro_usd]
```
Note: this RPC takes lock duration in **days**, not tier index. Use
`estimatecollateral` if you want to pass a tier number.

#### `estimatecollateral`
Estimate collateral needed at current oracle price.

```
digibyte-cli -testnet estimatecollateral <dd_amount_cents> <lock_tier> [oracle_price_micro_usd]
```
Both `dd_amount_cents` and `lock_tier` (0–9) are required. Optional third
argument forces a hypothetical DGB price for what-if estimates instead of the
live oracle consensus price.

#### `getredemptioninfo`
Get redemption details for a position.

```
digibyte-cli -testnet getredemptioninfo <position_txid>
```

#### `getprotectionstatus`
Get the current collateral protection status.

```
digibyte-cli -testnet getprotectionstatus
```

### Mock Oracle RPCs (regtest only)

| Command | Description |
|---------|-------------|
| `setmockoracleprice <micro_usd>` | Set mock price (e.g. 50000 = $0.05) |
| `getmockoracleprice` | Get current mock price |
| `simulatepricevolatility <percent>` | Simulate volatility (e.g. 50 = +50%) |
| `enablemockoracle <true\|false>` | Enable/disable mock oracle |

---

## For the Maintainer

When an operator sends their `pubkey` (33-byte compressed, e.g. `0398720f...eb7b57`), add it to **two locations** in `src/kernel/chainparams.cpp`:

**1. `vOracleNodes`** — use the full 33-byte compressed key. The testnet26 P2P port is `12033` (mainnet `12024`):
```cpp
{5, ParsePubKey("0398720f6d15252fb2c3501107d46129589d8ab56e0f967be2e470f40675eb7b57"), "operator.server.com:12033", true},
```

**2. `consensus.vOraclePublicKeys`** — strip the `02`/`03` prefix to get the 32-byte x-only key:
```cpp
consensus.vOraclePublicKeys.push_back("98720f6d15252fb2c3501107d46129589d8ab56e0f967be2e470f40675eb7b57");
```

Both locations MUST match the same key. If they don't, `ValidateOracleKey()` will reject the oracle at startup.

---

## Server Requirements

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| Uptime | 95% | 99.9% |
| RAM | 2 GB | 4+ GB |
| Disk | 20 GB | 50+ GB SSD |
| Network | Outbound HTTPS | Static IP or DNS |
| Ports | 12033 (testnet26 P2P), 12024 (mainnet P2P) | Open inbound + outbound |

---

## File Locations

| Component | Current testnet26 | Mainnet |
|-----------|-------------------|---------|
| Config | `~/.digibyte/digibyte.conf` | `~/.digibyte/digibyte.conf` |
| Data dir | `~/.digibyte/testnet26/` | `~/.digibyte/` |
| Debug log | `~/.digibyte/testnet26/debug.log` | `~/.digibyte/debug.log` |
| Wallets | `~/.digibyte/testnet26/wallets/` | `~/.digibyte/wallets/` |
| RPC cookie | `~/.digibyte/testnet26/.cookie` | `~/.digibyte/.cookie` |

> **Historical retired testnets:** `testnet25` used `~/.digibyte/testnet25/` with port 12032; `testnet24` used `~/.digibyte/testnet24/` with port 12031; `testnet23` used `~/.digibyte/testnet23/` with port 12030; older `testnet21` used `~/.digibyte/testnet21/` with port 12035. Those chains are offline/retired; preserved here only so operators know which directories to archive or delete.

---

## Fixing Wallet Name

If `getwalletinfo` shows the full path as wallet name:

```bash
# Current testnet26 example
digibyte-cli -testnet unloadwallet "/home/user/.digibyte/testnet26/wallets/oracle/"

# Reload with just the name
digibyte-cli -testnet loadwallet "oracle"

# Verify
digibyte-cli -testnet -rpcwallet=oracle getwalletinfo
```

---

*Verified against the current `feature/digidollar-v1` source tree. New operators should target `testnet26` (port 12033) or mainnet (port 12024). Retired `testnet25`/port 12032, `testnet24`/port 12031, `testnet23`/port 12030, and `testnet21`/port 12035 details are documented only as decommissioning footnotes.*
