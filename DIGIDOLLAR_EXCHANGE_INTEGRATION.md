# DigiDollar Exchange Integration Guide

*For exchanges that already support DigiByte and want to add DigiDollar (DD) trading pairs.*

---

## What Is DigiDollar?

DigiDollar is a decentralized USD-denominated stablecoin system built natively into DigiByte Core. Each DD is designed to track $1.00 USD through over-collateralized DGB vaults, live oracle pricing, DCA, ERR, and volatility protections. It uses the same blockchain, same nodes, same infrastructure — just new transaction types and a dedicated address format.

If you already run a DigiByte node, you're most of the way there.

---

## Key Facts for Exchanges

| Feature | Detail |
|---------|--------|
| Asset type | Native UTXO on DigiByte blockchain |
| Value | USD-denominated; designed to track $1.00 |
| Amount unit in RPC | **Integer USD cents for integration code** (10000 = $100.00) |
| Address format | `DD...` (mainnet), `TD...` (testnet), `RD...` (regtest) — Base58Check P2TR with 2-byte version prefix |
| Transaction fees | Paid in **DGB** (not DD) |
| Minimum fee | 0.1 DGB for transfer builders; mint and redeem builders enforce DGB fee floors in their txbuilder paths |
| Fee unit | DGB/kB (DigiByte uses kB, not vB) |
| Block time | 15 seconds (same as DGB) |
| Confirmations | Same security model as DGB |
| Backend required | The reviewed release for the target network and coordinated activation height |
| Wallet | Spend-capable descriptor wallet required for generated deposit addresses and withdrawals |

---

## 1. Node Setup

Use the reviewed build for your network. Follow the [backup and upgrade instructions](doc/digidollar-operations.md) before replacing an existing backend. DigiDollar configuration:

```ini
# digibyte.conf
server=1
digidollar=1
txindex=1  # required for DigiDollar transaction lookups
rpcuser=youruser
rpcpassword=yourpassword

# For testnet testing:
# testnet=1
# [test]
# digidollar=1
# txindex=1
# addnode=oracle1.digibyte.io:12033
```

That's it. Your existing DGB infrastructure stays the same — DD runs alongside it.

`txindex=1` is not optional on an unpruned mainnet or testnet node, because validation and wallet recovery need creating-transaction metadata. Startup refuses to run without it. Before activation, `getdigidollardeploymentinfo` remains available, but DD address, balance, history, send, mint, redeem, and running-oracle RPCs are gated.

**If you prune instead.** `-prune` and `-txindex` cannot both be set, so a pruned node is not asked for the index: it reads a spent DigiDollar output's creating transaction straight out of the retained block at that coin's height. What it must have is the block. So a pruned node keeps every block from the DigiDollar activation floor to the tip, which on mainnet is height 23,627,520, about 870,000 blocks and growing by about 5,760 a day. A small prune target such as `prune=550` therefore cannot be reached on mainnet: the node starts and removes everything below the floor, but it will not shrink past the blocks it has to keep. If a block above the floor is already missing, startup stops with "DigiDollar state not ready: retained block history is incomplete". See [node operations](doc/digidollar-operations.md).

---

## 2. Wallet Setup

You'll need a wallet that holds both DGB and DD:
- **DGB** — for paying transaction fees on DD sends
- **DD** — for processing customer withdrawals

Use a descriptor wallet with private keys enabled for hot-wallet custody. A watch-only/private-key-disabled wallet can monitor state if it already has the right descriptors, but it cannot create DD deposit addresses with `getdigidollaraddress` or sign withdrawals. `importdigidollaraddress` is only a validation/no-op stub in V1; it does not import, mutate wallet state, or rescan.

```bash
# Create a spend-capable descriptor wallet (or use an existing one)
digibyte-cli createwallet "exchange-hot" false false "" false true

# Verify DD is active
digibyte-cli getdigidollardeploymentinfo
# status should be "active"
```

---

## 3. Generating Customer Deposit Addresses

Generate a unique DD address for each customer, just like you do for DGB:

```bash
digibyte-cli getdigidollaraddress
# Returns a Base58Check DD/TD/RD address for the current network
```

**Important:** DD addresses are NOT the same as DGB addresses. They are Base58Check strings beginning with `DD`, `TD`, or `RD`, wrapping a 32-byte Taproot output key with a 2-byte DigiDollar version prefix. Do not treat them as Bech32/Bech32m, and do not validate by prefix alone. Use `validateddaddress` and reject wrong-network, checksum-invalid, whitespace-padded, or malformed input.

**List DD addresses:**
```bash
digibyte-cli listdigidollaraddresses

# Include generated but still-empty addresses:
digibyte-cli listdigidollaraddresses false 0 true
```

`listdigidollaraddresses` hides generated zero-balance addresses by default. Keep your own customer-to-address mapping when you allocate deposit addresses.

---

## 4. Detecting Customer Deposits

Poll for incoming DD transactions the same way you poll for DGB:

```bash
# List recent DD transactions (last 100, receive only)
digibyte-cli listdigidollartxs 100 0 "" "receive"
```

**Each transaction returns:**
```json
{
  "txid": "abc123...",
  "category": "receive",
  "amount": 50000,
  "address": "DDcustomerDepositAddr...",
  "confirmations": 12,
  "blockheight": 22100000,
  "blockhash": "def456...",
  "time": 1770934000,
  "fee": 0
}
```

**Match deposits to customers** by the `address` field (the unique DD address you generated for them).

Track `txid`, `vout`, `blockhash`, `blockheight`, `confirmations`, `in_mempool`, and `wallet_state` from `listdigidollartxs`. Credit only after your confirmation threshold. If a credited transaction becomes `conflicted`/`abandoned`, loses confirmations, or reappears with a different `blockhash`, hold or reverse the credit and rescan affected customer balances.

**Check a specific address balance:**
```bash
digibyte-cli getdigidollarbalance "DDcustomerAddr..." 6
# Second param = minimum confirmations (recommend 6+)
```

### Recommended Confirmation Thresholds

| Deposit Size | Confirmations | Wait Time |
|-------------|---------------|-----------|
| < $100 | 6 | ~90 seconds |
| $100 - $10,000 | 20 | ~5 minutes |
| > $10,000 | 60 | ~15 minutes |

Same security model as DGB — 15-second blocks with 5 mining algorithms.

---

## 5. Processing Customer Withdrawals

Send DD to a customer's DD address:

```bash
digibyte-cli senddigidollar "DDcustomerAddress..." 25000
# Sends $250.00 (25000 cents)
```

**Response:**
```json
{
  "txid": "ghi789...",
  "to_address": "DDcustomerAddress...",
  "amount": 25000,
  "status": "success",
  "fee_paid": "0.10000000",
  "inputs_used": 2,
  "change_amount": 25000
}
```

### Critical: You Need DGB for Fees

Every DD send requires DGB to pay the miner fee (minimum 0.1 DGB). **Always maintain a DGB balance in your hot wallet.** If you run out of DGB, DD withdrawals will fail.

**Check your DGB fee balance:**
```bash
digibyte-cli getbalance
```

### Withdrawal Limits

- Per-output dust floor: $1 (100 cents) — see `src/consensus/digidollar.h:88`
- Maximum single transfer: **$100,000** (10,000,000 cents) per `maxMintAmount`-aligned policy in `src/consensus/digidollar.h:87`
- DD inputs must be **confirmed** (≥1 confirmation) before they can be re-spent. The wallet does not chain unconfirmed DigiDollar UTXOs, and consensus rejects DD transfer/redeem inputs that resolve from `MEMPOOL_HEIGHT` (commit `0b4959f563`). Plan withdrawal cadence around the 15-second block time, or batch with `sendmanydigidollar`.
- Integration code can keep passing integer cents without an `amount_unit`: `25000` means $250.00. A decimal amount without a unit is rejected. To send dollar amounts, explicitly set `amount_unit` to `dollars`; then `250.00` means $250.00. The send, batch-send, and redeem RPCs share this rule. Explicit `cents` values must be integers; `dollars` values allow at most two decimal places.
- DD transfer withdrawals do not need a fresh oracle quote for mempool admission. Mint and redeem paths require recent valid MuSig2 oracle data; transfer-only exchange withdrawals are price-independent, but still require confirmed DD and DGB fee inputs.

### Batch withdrawals

`sendmanydigidollar` sends DD to multiple addresses in a single transaction (one fee, one set of inputs):

```bash
digibyte-cli sendmanydigidollar "" '{"DDcust1...":12500,"DDcust2...":7500}'
```

The first argument is the required `sendmany` compatibility dummy string. Use integer cents in integration code to avoid decimal display/rounding ambiguity. Large batches are limited by standard OP_RETURN relay size because one amount is committed for every DD output plus possible change; split large withdrawals and handle the RPC's "Too many DigiDollar recipients" error.

---

## 6. Balance Monitoring

### Hot Wallet Balances

```bash
# DD balance (confirmed only by default)
digibyte-cli getdigidollarbalance
# Returns: { "confirmed": 500000, "unconfirmed": 0, "total": 500000 }
# (amounts in cents — 500000 = $5,000.00)

# Include trusted mempool DD for monitoring only:
digibyte-cli getdigidollarbalance "" 0

# DGB balance (for fees)
digibyte-cli getbalance

# Buried activation status (verifies DD is live)
digibyte-cli getdigidollardeploymentinfo
```

### Watch-Only (Cold Wallet Monitoring)

Both `getdigidollarbalance` (third arg) and `listdigidollaraddresses` (first arg) accept `include_watchonly` for wallets that already contain watch-only DD state. DigiDollar V1 does **not** support importing a DD address for watch-only tracking: `importdigidollaraddress <addr> <label>` validates the address shape and returns an unsupported/no-op warning without mutating wallet state or rescanning. Cold-wallet monitoring for V1 requires a descriptor/watch-only wallet setup outside that stub, and spending DD still requires private keys in a spend-capable descriptor wallet.

---

## 7. Oracle Price Data

The DGB/USD price is provided by a decentralized oracle network. This is useful for display purposes and understanding collateral mechanics:

```bash
digibyte-cli getoracleprice
# Returns: { "price_usd": "0.00631", "price_micro_usd": 6310, ... }
```

**Oracle details:**
- 35 active oracle slots on testnet/mainnet, 7 MuSig2 signatures required
- 4-of-7 MuSig2 on regtest
- Active price sources: Binance, CoinGecko, KuCoin, Gate.io, HTX, Crypto.com (6 feeders, registered in `src/oracle/exchange.cpp:1092-1097`)
- Median-based aggregation with median-distance outlier rejection (`MultiExchangeAggregator::FilterOutliers` at `src/oracle/exchange.cpp:1225`); the live oracle daemon (`OracleNode::FetchMedianPrice` in `src/oracle/node.cpp:445-450`) requires **3** valid exchange responses before publishing, even though the aggregator's library default is 2 (`src/oracle/exchange.h:235`)
- Coinbase/Kraken/Messari are *not* used: DGB is unlisted on those venues and Messari now requires a paid API key (see the in-source comment at `src/oracle/exchange.cpp:1087-1089`)
- Oracle prices are derived **only** from live exchange aggregation; the `sendoracleprice` RPC was intentionally removed as a fake-price-injection vector

---

## 8. Network Health Monitoring

```bash
digibyte-cli getdigidollarstats
```

Returns system-wide metrics:
- **`total_dd_supply`** — circulating-supply field in cents: accepted issuance minus actual burns, subject to the legacy fallback limit below.
- **`canonical_health.open_vault_principal`** — original DD attached to unspent vaults; use only when `canonical_health.ready` is true.
- **`canonical_health.collateral`** — DGB satoshis locked in those vaults, with `active_vaults` and the matching `block_hash`.
- **`selected_health_denominator` / `health_rule_height`** — the liability definition and height used for tip-rule health.
- **`next_block_health`** — separate candidate height, readiness, health denominator and quote for next-block construction.

Before Thaw Day, the legacy fallback used when `digidollarstatsindex` is
disabled scans vault amounts. Do not treat that fallback's `total_dd_supply`
as a verified circulating token count. Use a synchronized stats index for
circulation reporting below the transition. At and above Thaw Day, the
fallback reconstructs tokens separately from vault principal.

At and above Thaw Day, health divides collateral value by open-vault principal.
It keeps circulating tokens separate. Closing a 100-DD vault while burning
125 DD reduces principal by 100 and circulation by 125. Their difference is
expected and does not itself show corruption. This health change can prolong
emergency mint restrictions or increase required redemption burn. It does not
alter customer balances at activation.

At tip H-1, tip health can still use legacy rules while the next candidate
uses the new rules. Unknown or unready values are not zero. A later quote or
chain change can change a construction estimate.

This is useful for risk monitoring. It is not a customer-deposit index. For custody, use wallet-local `listdigidollartxs`, `getdigidollarbalance`, and `listdigidollarunspent`, or build a raw indexer using the parsing rules below. If system health drops significantly, new minting gets more expensive (Dynamic Collateral Adjustment / DCA) and the Emergency Redemption Ratio (ERR) may activate (`src/consensus/err.cpp`).

---

## 9. Identifying DD Transactions in Raw Data

If your backend processes raw transactions:

**Detect DD transactions:**
```
(tx.nVersion & 0x0000FFFF) == 0x0770
```

**Extract type:**
```
(tx.nVersion >> 24) & 0xFF
  1 = MINT
  2 = TRANSFER  ← most common for deposits/withdrawals
  3 = REDEEM
```

**Parse the DD OP_RETURN by transaction type:**
- Mint: `OP_RETURN "DD" 1 <dd_amount_cents> <unlock_height> <lock_tier> <owner_xonly_pubkey_32b>`
- Transfer: `OP_RETURN "DD" 2 <amount1> <amount2> ...`; assign amounts to zero-value DD P2TR outputs in output order, including change
- Redeem: `OP_RETURN "DD" 3 <dd_change_amount>` only when DD change exists; full redemption may have no DD OP_RETURN
- All DD amounts are integer cents

**DD outputs have 0-satoshi value** — the DD amount is encoded in the script/OP_RETURN, not in `nValue`. Don't filter these out as dust!

For exchange deposits and withdrawals, prefer the wallet RPCs unless you are deliberately building an independent indexer. The wallet already maps DD output amounts, ownership, confirmation depth, mempool state, and reorg status.

---

## 10. API Integration Summary

### Essential RPCs for an Exchange

| Operation | RPC | Notes |
|-----------|-----|-------|
| **Generate deposit address** | `getdigidollaraddress` | One per customer |
| **Check deposit balance** | `getdigidollarbalance "addr" 6` | Use minconf |
| **Detect deposits** | `listdigidollartxs 100 0 "" "receive"` | Poll regularly |
| **Process withdrawal** | `senddigidollar "addr" <cents>` | Need DGB for fees |
| **Batch withdrawals** | `sendmanydigidollar "" {"addr":<cents>,...}` | One DD transaction, one DGB fee input set |
| **Inspect DD UTXOs** | `listdigidollarunspent` / `listdigidollarutxos` | Hot-wallet inventory and selected-input withdrawals |
| **Check DGB fee balance** | `getbalance` | Keep funded! |
| **Transaction history** | `listdigidollartxs` | Filter by category/address |
| **System status** | `getdigidollardeploymentinfo` | Verify DD is active |
| **Network health** | `getdigidollarstats` | Monitor collateral health |
| **Oracle price** | `getoracleprice` | Current DGB/USD |
| **Oracle signers** | `getoraclesigners [blocks]` | Recent on-chain MuSig2 signer audit |

### RPCs You Probably DON'T Need

Exchanges typically handle deposits and withdrawals — not minting or redeeming. These are for users who want to create/destroy DD:

| RPC | Purpose |
|-----|---------|
| `mintdigidollar` | Lock DGB → create DD (user operation) |
| `redeemdigidollar` | Burn DD → unlock DGB (user operation) |
| `listdigidollarpositions` | View collateral positions |
| `calculatecollateralrequirement` | Estimate collateral for minting |

---

## 11. Hot Wallet Architecture

```
┌─────────────────────────────────────────────┐
│                Exchange Backend              │
│                                             │
│  ┌──────────┐    ┌──────────┐              │
│  │ DGB Hot   │    │ DD Hot    │              │
│  │ Wallet    │    │ Wallet    │              │
│  │ (fees)    │    │ (withdraws)│             │
│  └─────┬─────┘    └─────┬─────┘             │
│        │                │                    │
│        └───────┬────────┘                    │
│                │                             │
│     ┌──────────▼──────────┐                 │
│     │  DigiByte Core Node  │                 │
│     │  v9.26.2+            │                 │
│     │  digidollar=1        │                 │
│     └──────────────────────┘                 │
│                                             │
│  Monitor:                                    │
│  • getdigidollarbalance (DD deposits)       │
│  • getbalance (DGB fee reserve)             │
│  • listdigidollartxs (deposit detection)    │
│                                             │
│  Process:                                    │
│  • senddigidollar (DD withdrawals)          │
│  • Ensure DGB balance covers fees           │
└─────────────────────────────────────────────┘
```

### Key Points

1. **One node handles both DGB and DD** — same daemon, same wallet
2. **Keep DGB funded** — DD withdrawals fail without DGB for fees
3. **DD and DGB are separate balances** — track both independently
4. **Standard confirmation logic** — same security as DGB deposits
5. **DD addresses are different from DGB addresses** — don't mix them up

---

### Address encoding in external systems

DGB witness version 0 addresses use Bech32. Witness version 1 Taproot addresses
use Bech32m. Decode the output script and witness version before labeling its
address; a display label alone is not evidence of its encoding. Core enforces
this distinction in [src/key_io.cpp](src/key_io.cpp). DD/TD/RD addresses use a
separate Base58Check encoding of the Taproot key, as described above. Record
the network, transaction ID, output index and exact displayed address when
investigating an explorer disagreement.

## 12. Common Pitfalls

| Pitfall | Solution |
|---------|----------|
| Sending to a DGB address instead of DD | Use `validateddaddress`; prefix-only checks are not enough |
| Running out of DGB for fees | Monitor DGB balance, auto-top-up from exchange reserves |
| Filtering out 0-sat outputs as dust | DD token outputs are 0-sat by design — don't discard them |
| Using wrong amount units | Keep integer cents, or explicitly set `amount_unit=dollars`; decimal amounts without a unit are rejected |
| Not checking activation status | Call `getdigidollardeploymentinfo` — DD RPCs error before activation |
| Crediting deposits without reorg checks | Track `blockhash`, confirmations, `in_mempool`, and `wallet_state`; reverse or hold credits on conflicts |
| Ignoring system health | Monitor `getdigidollarstats` — ERR state affects the broader ecosystem |

---

## 13. Test on Testnet Now!

The public testnet configured in this source is **testnet26**, with buried
DigiDollar activation at 600. Thaw Day is scheduled at **432,100**, estimated
for September 18–19, 2026. The block height triggers the change. Check
`getdigidollardeploymentinfo` on the installed build and follow the
[upgrade window](DIGIDOLLAR_ACTIVATION_EXPLAINER.md). No public Thaw Day
activation or soak test is claimed.

### Testnet Quick Start

1. **Obtain** the reviewed test build and network instructions from the release owner
2. **Configure:**
   ```ini
   testnet=1
   [test]
   digidollar=1
   txindex=1
   addnode=oracle1.digibyte.io:12033
   server=1
   rpcuser=youruser
   rpcpassword=yourpassword
   ```
3. **Launch:** `digibyted -testnet -daemon`
4. **Get testnet DGB:** Ask in https://app.gitter.im/#/room/#digidollar:gitter.im
5. **Generate DD address:** `digibyte-cli -testnet getdigidollaraddress`
6. **Receive test DD** from the community
7. **Practice withdrawals:** `digibyte-cli -testnet senddigidollar "TDaddr..." 1000`

### Testnet Details

| Parameter | Value |
|-----------|-------|
| Testnet name | testnet26 |
| P2P Port | 12033 |
| DD Address Prefix | `TD` |
| Status | DigiDollar buried at 600; Thaw Day scheduled at 432,100 and reported separately by `getdigidollardeploymentinfo` |
| Oracle | 35 active slots, 7 MuSig2 signatures required, 6 exchange sources |

---

## 14. Mainnet Activation

DigiDollar's buried mainnet activation height is 23,869,440. Thaw Day is a
separate consensus transition scheduled at 24,490,000, estimated for
November 1, 2026. The block height triggers the change.
Use `getdigidollardeploymentinfo.thaw_day` to distinguish scheduled, active at
tip and active next block. There are no current DigiDollar BIP9 signaling
statistics in that RPC.

Upgrade custody nodes before that height. Older nodes may
disagree under the new rules. Agree on how deposits and withdrawals will be
held if chain agreement becomes uncertain. Installation alone is not proof of
activation or readiness. See the [activation guide](DIGIDOLLAR_ACTIVATION_EXPLAINER.md)
and [node operations](doc/digidollar-operations.md).

---

## Questions & Support

- **Developer chat:** https://app.gitter.im/#/room/#digidollar:gitter.im
- **Testnet tracker:** https://digibyte.io/testnet/activation
- **GitHub:** https://github.com/DigiByte-Core/digibyte

💎 DigiDollar — the first truly decentralized stablecoin on a UTXO blockchain.
