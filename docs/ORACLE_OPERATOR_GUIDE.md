# DigiDollar Oracle Operator Guide
*How to become an oracle operator and get your key into DigiByte Core*

---

## Overview

DigiDollar requires oracle operators to provide real-time DGB/USD price feeds. Oracle public keys are **hardcoded in `src/kernel/chainparams.cpp`** — every oracle operator must:

1. Use the reviewed build for your network and create a descriptor wallet
2. Run `createoraclekey` to generate their oracle keypair inside the wallet
3. Export an offline recovery copy with `exportoracleprivkey` after unlocking encrypted wallets with `walletpassphrase`
4. Send their **public key only** to the DigiByte Core maintainer
5. The maintainer adds their key to `chainparams.cpp` and ships a new release
6. The operator runs `startoracle` — the wallet provides the private key automatically

For current testnet release/migration mechanics (testnet26, P2P port 12033, RPC port 14026) and retired testnet decommissioning notes, follow [DIGIDOLLAR_ORACLE_SETUP.md](../DIGIDOLLAR_ORACLE_SETUP.md).
For upgrades, two nodes behind one router, compact filters and temporary logging,
read [node operations](../doc/digidollar-operations.md). Mainnet Thaw Day is
scheduled at 24,490,000, estimated for November 1, 2026. Testnet26 is scheduled
at 432,100, estimated for September 18–19, 2026. The heights trigger activation.
Signet is unsupported and remains unscheduled. No public-network Thaw Day
activation or soak test is claimed.

---

## Step-by-Step: For Oracle Operators

### Step 1: Compile and Run the Selected Build

```bash
cd ~/Code/digibyte
./autogen.sh
./configure
make -j$(nproc)
```

Start on testnet:
```bash
./src/digibyted -testnet
```

### Step 2: Create a Descriptor Wallet

Current releases create descriptor wallets by default. No special flags needed.

```bash
./src/digibyte-cli -testnet createwallet "oracle"
```

### Step 3: Generate Your Oracle Key

```bash
./src/digibyte-cli -testnet -rpcwallet=oracle createoraclekey 0
```

Replace `0` with the oracle ID slot assigned to you by the maintainer.

Mainnet and testnet chainparams allocate 35 oracle slots (IDs 0–34), and all 35 are part of the configured consensus-active MuSig2 roster (`consensus.vOraclePublicKeys`). Slot ID 35 is outside the configured roster. Regtest has 7 slots (IDs 0–6) with 4-of-7 consensus.

**Output:**
```json
{
  "oracle_id": 0,
  "pubkey": "0398720f6d15252fb2c3501107d46129589d8ab56e0f967be2e470f40675eb7b57",
  "pubkey_xonly": "98720f6d15252fb2c3501107d46129589d8ab56e0f967be2e470f40675eb7b57",
  "stored_in_wallet": true,
  "message": "Oracle key generated and stored in wallet. Share ONLY the pubkey..."
}
```

**What happened:**
- A new secp256k1 keypair was generated using the wallet's secure random number generator
- The **private key** was stored securely in the wallet database (key: `oraclekey`)
- The **compressed public key** (33 bytes, 02/03 prefix) was returned for you to share
- The **x-only public key** (32 bytes, for Schnorr) was also returned

### Step 4: Export an Offline Recovery Copy

Back up the wallet with `backupwallet` and also store one offline copy of the oracle private key:

```bash
./src/digibyte-cli -testnet -rpcwallet=oracle exportoracleprivkey 0
```

If the wallet is encrypted, unlock it first:

```bash
./src/digibyte-cli -testnet -rpcwallet=oracle walletpassphrase "<passphrase>" 60
```

`exportoracleprivkey` returns sensitive signing material. Store it offline, keep it out of chat, and do not use it as a `startoracle` argument. To restore onto a replacement wallet, load or create the wallet, unlock it if encrypted, then use `importoracleprivkey <oracle_id> <private_key_hex> [replace]`. Importing stores the key for later `startoracle`; it does not start an oracle, sign prices, relay messages, or change consensus state.

### Step 5: Send Your Public Key to the Maintainer

Send **only these two things**:
1. Your **pubkey** from the output above (66-char hex starting with `02` or `03`)
2. Your **server endpoint** (e.g., `myserver.com:12033` for testnet26, or `myserver.com:12024` for mainnet)

**⚠️ NEVER share your private key. It stays in your wallet.**

### Step 6: Wait for Updated Release

The maintainer adds your key to `chainparams.cpp` and releases an updated binary.

### Step 7: Download the Updated Binary and Start Your Oracle

```bash
# Start the node
./src/digibyted -testnet

# Start your oracle — key loads automatically from wallet!
./src/digibyte-cli -testnet -rpcwallet=oracle startoracle 0
```

**No private key argument needed.** The `startoracle` command automatically retrieves your private key from the wallet where `createoraclekey` stored it.

**Output (testnet):**
```json
{
  "success": true,
  "oracle_id": 0,
  "status": "running",
  "message": "Oracle started with key loaded from wallet 'oracle'"
}
```

Do not paste oracle private keys into shell commands. Use the wallet-stored key
created by `createoraclekey`; it keeps the secret out of shell history and
process listings.

### Step 8: Verify Your Oracle is Running

```bash
# Check oracle status
./src/digibyte-cli -testnet getoraclepubkey 0

# Expected output:
# "authorized": true    ← Your key matches chainparams
# "is_running": true    ← Price thread is active
```

### Step 9: Monitor

Routine diagnostics require `-debug=digidollar`. Enable it only while diagnosing
a problem; useful errors and startup/recovery progress remain visible without
it. See [logging guidance](../doc/digidollar-operations.md#routine-logs-and-temporary-diagnostics)
for startup shrinking and disk retention.

```bash
tail -f ~/.digibyte/testnet26/debug.log | grep -i oracle
```

---

## What Your Oracle Does

Once running, your oracle automatically:
- Fetches DGB/USD prices from 6 active exchanges every 60 seconds (Binance, CoinGecko, KuCoin, Gate.io, HTX, Crypto.com — see `src/oracle/exchange.cpp:1042-1071`)
- Calculates median price with median-distance outlier filtering (`MultiExchangeAggregator::FilterOutliers` at `src/oracle/exchange.cpp:1192`); the live oracle daemon requires **at least 3 valid exchange responses** to publish — `OracleNode::FetchMedianPrice` overrides the aggregator's default floor of 2 (`src/oracle/node.cpp:445-450`, `src/oracle/exchange.h:235`)
- Signs the price with BIP-340 Schnorr using your wallet-stored private key
- Broadcasts the signed price message to the P2P network; in V1, MuSig2 bundle attestations are the consensus path

---

## Important Notes

- **Key persists in wallet** — Your oracle key survives wallet unload/reload. Since RC25, an unencrypted wallet auto-starts its oracle on load (`CWallet::TryAutoStartOracles` in `src/wallet/wallet.cpp:4814`); an encrypted wallet auto-starts after `walletpassphrase` unlock. Running `startoracle` manually after restart is still a safe fallback.
- **One key per oracle ID** — `createoraclekey` rejects if a key already exists for that ID. This prevents accidental overwrites.
- **Descriptor wallets** are the default in current releases. `dumpprivkey` exists for legacy wallets; descriptor-wallet operators do not need it because the oracle private key is stored under the `oraclekey` record (see `WriteOracleKey/ReadOracleKey` in `src/wallet/walletdb.cpp` and `StoreOracleKey/GetOracleKey` in `src/wallet/wallet.cpp`).
- **Backup your wallet** — `backupwallet` includes your oracle key. Losing the wallet means losing your oracle key.
- **P2P anti-abuse is enforced by the node** — oracle price, attestation, consensus, and MuSig2 nonce/partial-signature messages are admission checked in `src/net_processing.cpp` before relay/session ingestion. Stale MuSig2 epochs are dropped; the current and next epoch are the only accepted relay windows.

---

## Reset and Recovery

The oracle daemon is stateless beyond the wallet-resident private key plus the in-memory MuSig2 session. There is no on-disk session/secnonce store, so a clean restart cannot leak nonces. Use the steps below when the local oracle stops broadcasting, gets stuck on a stale price, or after a node restart.

### 1. Confirm the local oracle status

```bash
digibyte-cli -testnet listoracle
digibyte-cli -testnet getoraclepubkey <id>
digibyte-cli -testnet getdigidollardeploymentinfo   # check musig2_session.state for current epoch
```

Healthy output: `running=true`, `authorized=true`, and `musig2_session.state` cycles through `NONCES_COLLECTING` → `NONCES_COMPLETE` → `SIGNING` → `COMPLETE` once per epoch.

### 2. Confirm the network sees your oracle

```bash
digibyte-cli -testnet getoracles true 20 | jq '.[] | select(.is_running_locally==true)'
digibyte-cli -testnet getalloracleprices 20 | jq '.oracles[] | select(.oracle_id==<id>)'
```

Look for `price_source` values. `local` means the daemon has a fresh exchange quote; `pending` means an attestation is gossiping; `on-chain` means a recent v0x03 bundle included your slot. `none` plus `status=no_data` for several blocks means the daemon is not contributing — go to step 3.

### 3. Restart cleanly

```bash
digibyte-cli -testnet stoporacle <id>
digibyte-cli -testnet -rpcwallet=oracle walletpassphrase "<passphrase>" 600   # encrypted wallets only
digibyte-cli -testnet -rpcwallet=oracle startoracle <id>
```

`stoporacle` joins the price thread and clears pending oracle-message state; `startoracle` rebuilds the price thread from the wallet-resident key. A full daemon restart drops in-memory MuSig2 sessions, and the current-epoch session is recreated lazily on the next remote nonce or block tick (see `OracleSigningOrchestrator::OnBlockConnected`). Pre-restart secnonce material is unrecoverable by design — a recreated session generates fresh nonces.

### 4. Diagnose exchange-feed issues

The live oracle requires **3 of 6 exchange responses** to publish (see "What Your Oracle Does" above). Search the debug log for fetcher errors:

```bash
tail -n 2000 ~/.digibyte/testnet26/debug.log | grep -E "Oracle: (Insufficient|Exception|Failed to fetch|Initialized)"
```

Specifically:
- `Oracle: Insufficient price sources (N < 3 required)` — fewer than 3 exchanges responded; check outbound HTTPS, DNS, and CA bundle path.
- `Oracle: Exception fetching from <Exchange>` — that exchange's API or the local TLS path failed for that round.
- `Oracle: WARNING - 5 consecutive price fetch failures` / `ALERT - 15` / `CRITICAL - %d` — escalating reachability problems (`src/oracle/node.cpp:427-437`).
- `HttpGet: SECURITY - No CA bundle found` — your host has no recognized system CA store; install `ca-certificates` (Debian/Ubuntu) or equivalent so libcurl can verify TLS.

A single broken endpoint (bad URL or quota-blocked) is non-fatal; the round logs the per-fetcher error and proceeds with the remaining sources as long as ≥3 succeed. There is no API-key configuration: the daemon uses only public unauthenticated endpoints, so missing-key is not a startup failure mode.

### 5. Recover from wallet corruption

An unreadable wallet does not establish that its key is lost. Preserve the file
and diagnostics before repair, and use a known-good backup or exported key if
available. A replacement public key requires coordinated chainparams changes;
it cannot take over the existing slot through a local setting.

- Restore the wallet from `backupwallet` if one exists; the `oraclekey` record carries the private key.
- If you exported the private key, create or load a replacement wallet, unlock it with `walletpassphrase` if encrypted, then run `importoracleprivkey <oracle_id> <private_key_hex> [replace]`. Start the oracle only after the imported public key matches the slot configured in chainparams.
- If no wallet backup or private-key export exists, run `createoraclekey <id>` to generate a new key, send the new pubkey to the maintainer, and wait for the next release before resuming as that slot. Until the chainparams update ships, the slot stays inactive.

---

## For the Maintainer: Adding an Operator's Key

When an operator sends you their 33-byte compressed public key, add it to **two places** in `src/kernel/chainparams.cpp`:

### 1. vOracleNodes (33-byte compressed CPubKey)

In `InitializeOracleNodes()` — match the network's P2P port (mainnet 12024, testnet26 12033):
```cpp
{5, ParsePubKey("0398720f6d15252fb2c3501107d46129589d8ab56e0f967be2e470f40675eb7b57"), "operator.server.com:12033", true},
```

### 2. consensus.vOraclePublicKeys (32-byte x-only key — strip the 02/03 prefix)

```cpp
consensus.vOraclePublicKeys.push_back("98720f6d15252fb2c3501107d46129589d8ab56e0f967be2e470f40675eb7b57");
```

**⚠️ Both locations must be updated.** `vOracleNodes` uses 33-byte compressed keys. `consensus.vOraclePublicKeys` uses 32-byte x-only keys. They must match.

Then recompile and distribute the updated binary.

---

## Oracle Slots

| Network | Total Slots | Active (in MuSig2 quorum) | Consensus | Notes |
|---------|------------|---------------------------|-----------|-------|
| Mainnet | 35 (IDs 0-34) | 35 (slots 0-34) | 7 signatures from active keyset | DigiDollar buried activation is 23,869,440; static MuSig2 floor is 23,627,520. Slot 28 uses the DigiHash Mining Pool key; slot 31 uses the Peer2Peer / DigiRoos key. |
| Testnet (testnet26) | 35 (IDs 0-34) | 35 (slots 0-34) | 7 signatures from active keyset | Active from height 600. Slot 28 uses the DigiHash Mining Pool key; slot 31 uses the Peer2Peer / DigiRoos key. |
| Regtest | 7 (IDs 0–6) | 7 | 4-of-7 MuSig2 | Always active. |

`getoracles` reads names from the selected network's roster. Mainnet slot 0
is `DigiByte.Io Oracle` and slot 11 is `Crypto Corner Shop`; testnet uses `Jared`
and `hallvardo` for those slots. These are display names. They do not change
operator IDs, public keys or the signing quorum. Verify authorization with the
configured ID and key, not a name alone.

To confirm a slot is in the active quorum at runtime, call
`getoracles` and inspect the `in_consensus` field, or call
`getdigidollardeploymentinfo` and read `oracle_pubkey_count` /
`oracle_total_slots`. A slot is in the active quorum iff
`oracle_id < oracle_pubkey_count`.

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

## RPC Command Reference

| Command | Description |
|---------|-------------|
| `createoraclekey <oracle_id>` | Generate oracle keypair in wallet (wallet-context RPC) |
| `exportoracleprivkey <oracle_id>` | Export an offline recovery copy of a wallet-stored oracle private key; encrypted wallets require `walletpassphrase` first |
| `importoracleprivkey <oracle_id> <private_key_hex> [replace]` | Import a recovery key into the wallet for later `startoracle`; rejects existing keys unless `replace=true` |
| `startoracle <id>` | Start oracle from the key stored by `createoraclekey` (wallet-context RPC) |
| `stoporacle <id>` | Stop oracle price thread |
| `getoraclepubkey <id>` | Check oracle key and status |
| `getoracles [active_only] [scan_blocks]` | List all configured oracles with chain status |
| `listoracle` | Show this node's running oracle |
| `getoracleprice` | Get current consensus DGB/USD oracle price |
| `getalloracleprices` | Per-oracle price view (debug/status) |

---

## Code References

| Component | File | Key Lines |
|-----------|------|-----------|
| Oracle key RPCs (`createoraclekey`, `exportoracleprivkey`, `importoracleprivkey`, `startoracle`) | `src/rpc/digidollar.cpp` | RPC helpman definitions |
| Wallet RPC registration | `src/wallet/rpc/wallet.cpp` | `commands` table under DigiDollar/oracle wallet commands |
| Base RPC registration (other oracle commands) | `src/rpc/digidollar.cpp` | `RegisterDigiDollarRPCCommands` |
| Wallet DB storage | `src/wallet/walletdb.cpp` | `WriteOracleKey`, `ReadOracleKey` |
| CWallet key methods | `src/wallet/wallet.cpp` | `StoreOracleKey`, `GetOracleKey` |
| OracleNodeInfo struct | `src/primitives/oracle.h` | OracleNodeInfo |
| chainparams oracle slots | `src/kernel/chainparams.cpp` | `InitializeOracleNodes()`, `vOraclePublicKeys` |
| Unit tests | `src/test/oracle_wallet_key_tests.cpp` | Wallet key generation / persistence |
| Functional test | `test/functional/digidollar_oracle_keygen.py` | End-to-end keygen + start |

---

*Instructions target the current source and testnet26 / P2P 12033 / RPC 14026.
Documentation checks do not establish runtime, network or release readiness.*
