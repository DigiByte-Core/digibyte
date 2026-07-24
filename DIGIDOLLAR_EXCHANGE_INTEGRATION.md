# DigiDollar Exchange Integration Guide

*For exchanges that already support DigiByte and want to add DigiDollar (DD) trading pairs.*

> **Status: live on mainnet.** DigiDollar locked in through BIP9 bit 23 miner
> signaling and activated at block **23,869,440** on **17 July 2026**, alongside
> the AlgoLock retired-algorithm rule. DD RPCs are available on any synced
> v9.26.2+ mainnet node today. Confirm on your own node with
> `getdigidollardeploymentinfo`.

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
| Backend required | DigiByte Core v9.26.2+ with DigiDollar built in (v9.26.5 current); **active on mainnet** since block 23,869,440 |
| Wallet | Spend-capable descriptor wallet required for generated deposit addresses and withdrawals |

---

## 1. Node Setup

Upgrade your existing DigiByte node to v9.26.2+ and enable DigiDollar. **v9.26.5
is the current release and is recommended for exchange nodes** — it fixes a
DigiDollar oracle startup scan that could hang mainnet nodes for 15 minutes or
more at launch.

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

`txindex=1` is not optional for DD chains because validation and wallet recovery need creating-transaction metadata.

On a network where DigiDollar has **not** yet activated, `getdigidollardeploymentinfo` remains available but the DD address, balance, history, send, mint, redeem, and running-oracle RPCs are gated. This no longer applies to mainnet, where DigiDollar activated at block 23,869,440.

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

- Per-output dust floor: $1 (100 cents) — see `src/consensus/digidollar.h:73`
- Maximum single transfer: **$100,000** (10,000,000 cents) per `maxMintAmount`-aligned policy in `src/consensus/digidollar.h:72`
- DD inputs must be **confirmed** (≥1 confirmation) before they can be re-spent. The wallet does not chain unconfirmed DigiDollar UTXOs, and consensus rejects DD transfer/redeem inputs that resolve from `MEMPOOL_HEIGHT` (commit `0b4959f563`). Plan withdrawal cadence around the 15-second block time, or batch with `sendmanydigidollar`.
- Integration code should pass integer cents with no decimal point. The send/redeem RPCs accept decimal-dollar input for CLI compatibility, so `25000` means $250.00 but `25000.00` means $25,000.00.
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

# BIP9 activation status (verifies DD is live)
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
- **Total DD supply** — all DigiDollars in circulation
- **Total collateral** — all DGB locked as backing
- **System health ratio** — collateral value / DD supply (should be >100%)

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

## 12. Common Pitfalls

| Pitfall | Solution |
|---------|----------|
| Sending to a DGB address instead of DD | Use `validateddaddress`; prefix-only checks are not enough |
| Running out of DGB for fees | Monitor DGB balance, auto-top-up from exchange reserves |
| Filtering out 0-sat outputs as dust | DD token outputs are 0-sat by design — don't discard them |
| Using wrong amount units | Backend code should send integer cents; any decimal point is interpreted as dollars |
| Not checking activation status | Call `getdigidollardeploymentinfo` — DD RPCs error before activation |
| Crediting deposits without reorg checks | Track `blockhash`, confirmations, `in_mempool`, and `wallet_state`; reverse or hold credits on conflicts |
| Ignoring system health | Monitor `getdigidollarstats` — ERR state affects the broader ecosystem |

---

## 13. Rehearse on Testnet First

Mainnet is live, so testnet is no longer where you wait for DigiDollar — it is where you rehearse the full deposit → credit → withdraw → reconcile loop before touching real customer funds.

The current public testnet in this source tree is **testnet26**. DigiDollar activation is BIP9-gated at/after block 600 once 140 of 200 blocks signal; verify status with `getdigidollardeploymentinfo`.

### Testnet Quick Start

1. **Download** the latest DigiByte Core v9.26.x release build from this branch
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
| Status | BIP9 bit 23, min activation height 600; check `getdigidollardeploymentinfo` for current status |
| Oracle | 35 active slots, 7 MuSig2 signatures required, 6 exchange sources |

---

## 14. Mainnet Activation

**DigiDollar is active on mainnet.** It locked in through BIP9 bit 23 miner signaling and activated at block **23,869,440** on **17 July 2026**, alongside the AlgoLock retired-algorithm rule.

The original signaling parameters were BIP9 bit 23, a start time of 2026-06-01, a 40,320-block signaling window, a 70% threshold (28,224 of 40,320), and a minimum activation height of 23,627,520.

As of **v9.26.5** the mainnet deployment is **buried** (BIP90): the activation height is hardcoded as `consensus.DigiDollarHeight = 23869440` in `src/kernel/chainparams.cpp` rather than evaluated as a live version-bits deployment.

Activation parameters remain branch- and network-specific. Always confirm the live state by calling `getdigidollardeploymentinfo` on your target release and network as the source of truth for status, window size, threshold, timeout, and activation height.

> **Note on reading the height from a pre-v9.26.5 node.** On v9.26.4 and earlier, `getdigidollardeploymentinfo` reports `activation_height: 23869439` — the final block of the BIP9 `LOCKED_IN` period. DigiDollar rules take effect at the *following* block, **23,869,440**, which is the value buried in consensus from v9.26.5 onward. Use 23,869,440 when recording the activation height.

---

## Questions & Support

- **Developer chat:** https://app.gitter.im/#/room/#digidollar:gitter.im
- **Testnet tracker:** https://digibyte.io/testnet/activation
- **GitHub:** https://github.com/DigiByte-Core/digibyte

💎 DigiDollar — the first truly decentralized stablecoin on a UTXO blockchain.
