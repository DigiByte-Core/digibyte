# DigiDollar Wallet Integration Guide

*For wallet providers who already support DigiByte and want to add DigiDollar (DD) support.*

---

## What Is DigiDollar?

DigiDollar is a decentralized USD-denominated stablecoin system built natively into DigiByte Core. Each DD is designed to track $1.00 USD through over-collateralized DGB vaults, live oracle pricing, DCA, ERR, and volatility protections. No company controls it — everything runs inside the DigiByte protocol.

There are only 4 operations: **Mint**, **Transfer**, **Redeem**, and regular DGB transactions.

---

## Quick Overview

| Feature | Detail |
|---------|--------|
| Token type | Native UTXO (not a layer-2 token) |
| Address format | `DD...` (mainnet), `TD...` (testnet), `RD...` (regtest) — Base58Check with 2-byte version prefix wrapping a P2TR x-only key |
| Amount unit | **USD cents** for integration code (10000 = $100.00) |
| Fees | Always paid in **DGB** (not DD) |
| Minimum fee | 0.1 DGB per DD transaction for transfer builders; mint and redeem builders enforce their own DGB fee floors in `src/digidollar/txbuilder.cpp` |
| Signing | Schnorr (BIP-340) for DD inputs, ECDSA/Schnorr for DGB fee inputs |
| Wallet type | Mint needs private keys, HD support, Taproot receiving and bech32 change descriptors; see wallet support below |
| Confirmations | Same as DGB — 15-second blocks |
| Backend | Use the coordinated release for the target network; see the activation and upgrade guidance below |

---

## 1. Prerequisites

Your wallet must:
- Use the reviewed release selected for the target network and its activation height
- Set `digidollar=1` in `digibyte.conf`
- Set `txindex=1`; startup enforces this on mainnet/testnet DigiDollar chains and on regtest when DD testing is enabled
- Check `getdigidollardeploymentinfo`: DigiDollar uses a buried activation height; Thaw Day has its own separately reported height
- Use a descriptor/bech32m HD wallet with private keys enabled. DD mint
  requires deriving an HD owner key for the time-lock; encryption is
  supported, in which case the wallet must be unlocked with
  `walletpassphrase` before private-key DD actions. Legacy BDB wallets and
  watch-only/private-key-disabled wallets cannot create DD addresses, mint,
  send, redeem, or sign.

Before activation, `getdigidollardeploymentinfo` remains available and wallet-local oracle key setup can be prepared with `createoraclekey`, `exportoracleprivkey`, and `importoracleprivkey`. DD address, balance, history, mint, send, redeem, and running-oracle status/operation RPCs are activation-gated.

```ini
# digibyte.conf
server=1
digidollar=1
txindex=1  # required for DigiDollar transaction lookups
```

---

## 2. Address Generation

DigiDollar uses its **own address format** — standard DGB addresses won't work for DD operations.

| Network | Prefix | Example |
|---------|--------|---------|
| Mainnet | `DD` | `DD<base58check-taproot-key>` |
| Testnet | `TD` | `TD<base58check-taproot-key>` |
| Regtest | `RD` | `RD<base58check-taproot-key>` |

**Generate a DD address:**
```bash
digibyte-cli getdigidollaraddress
# Returns a Base58Check DD/TD/RD address for the current network
```

**List DD addresses in wallet:**
```bash
digibyte-cli listdigidollaraddresses

# Include generated but still-empty addresses:
digibyte-cli listdigidollaraddresses false 0 true
```

DD addresses are P2TR (Taproot) under the hood, encoded with a 2-byte Base58Check version prefix (`0x52, 0x85` mainnet -> "DD"; `0xb1, 0x29` testnet -> "TD"; `0xa3, 0xa4` regtest -> "RD" — see `src/base58.cpp:180-182`). They are not Bech32/Bech32m strings even though the underlying output is Taproot. Prefix alone is not validation: use `validateddaddress` or the current-network Base58Check validator before accepting or sending to an address. Invalid, whitespace-padded, wrong-network, wrong-size, or checksum-invalid input returns `isvalid=false` and a blank canonical `address`.

`listdigidollaraddresses` hides generated zero-balance addresses by default to avoid leaking wallet/keypool size. Pass `include_empty=true` as the third argument when an operator needs a full generated-address inventory.

---

## 3. Checking Balances

DigiDollar balances are tracked **separately** from DGB balances. The wallet maintains its own DD UTXO set.

**Get total DD balance:**
```bash
digibyte-cli getdigidollarbalance
# Returns: { "confirmed": 50000, "unconfirmed": 0, "total": 50000 }
# (amounts in cents — 50000 = $500.00)
```

**Get balance for a specific address:**
```bash
digibyte-cli getdigidollarbalance "DDaddress..."
```

**Key points:**
- `confirmed` — DD in confirmed transactions
- `unconfirmed` — DD in unconfirmed but trusted transactions when queried with `minconf=0`, for example `getdigidollarbalance "" 0`
- Default `minconf` is 1, so confirmed-only accounting is the default
- Users need BOTH a DD balance (to send DD) AND a DGB balance (to pay fees)

---

## 4. Minting DigiDollars

Minting locks DGB as collateral and creates new DD tokens.

### Lock Tiers

The 10 canonical lock tiers (defined in `src/consensus/digidollar.h:57-68`):

| Tier | Lock Period | Collateral Ratio |
|------|-----------|-----------------|
| 0 | 1 hour (240 blocks) | 1000% (testing/onboarding) |
| 1 | 30 days | 500% |
| 2 | 90 days | 400% |
| 3 | 180 days | 350% |
| 4 | 1 year | 300% |
| 5 | 2 years | 275% |
| 6 | 3 years | 250% |
| 7 | 5 years | 225% |
| 8 | 7 years | 212% |
| 9 | 10 years | 200% |

These tiers are consensus-enforced. **Custom (non-canonical) lock durations are rejected by the validator** — `src/digidollar/validation.cpp` checks `bad-mint-lock-tier`, `bad-mint-lock-tier-duration`, and `bad-mint-lock-period` paths so wallets must select a tier 0–9. The OP_RETURN encodes the tier explicitly so it can be cross-checked against the locktime.

Longer lock = lower collateral requirement. The collateral stays in YOUR wallet — you never give up your keys.

### Mint Limits

- **Minimum:** $100 (10,000 cents)
- **Maximum:** $100,000 per transaction (10,000,000 cents)

### How to Mint

**Step 1: Check the oracle price**
```bash
digibyte-cli getoracleprice
# Returns current DGB/USD price in micro-USD
```

**Step 2: Estimate collateral needed**
```bash
digibyte-cli calculatecollateralrequirement 10000 180
# 10000 cents ($100), 180 days lock (350% ratio)
# Returns: required DGB amount

# Or use estimatecollateral with tier (both args required):
digibyte-cli estimatecollateral 10000 3
# 10000 cents ($100), tier 3 (180 days)

# Optionally pass a custom DGB price (micro-USD) for what-if calculations:
digibyte-cli estimatecollateral 50000 5 6500
```

**Step 3: Mint**
```bash
digibyte-cli mintdigidollar 10000 3
# Locks DGB collateral, creates $100 DD
```

**Response:**
```json
{
  "txid": "abc123...",
  "dd_minted": 10000,
  "dgb_collateral": "55468.12345678",
  "lock_tier": 3,
  "unlock_height": 1234567,
  "collateral_ratio": 350,
  "fee_paid": "0.10000000",
  "position_id": "abc123..."
}
```

### What Happens Under the Hood

The mint transaction creates:
1. **Collateral output** (P2TR) — Your DGB locked with a CLTV timelock. Uses a NUMS internal key (mathematically unspendable via key-path), ensuring it can only be unlocked via the script-path after the timelock expires.
2. **DD token output** (P2TR) — A 0-satoshi Taproot output representing your DigiDollars. Freely transferable.
3. **OP_RETURN metadata** — Records the DD amount, lock height, and tier for network-wide tracking.
4. **DGB change** — Any leftover DGB returned to your wallet.

---

## 5. Sending DigiDollars

Sending DD is straightforward — it works like sending any UTXO.

### Amount units: cents by default, `amount_unit` to say otherwise

DigiDollar amounts are integer **cents** inside the node. `senddigidollar`,
`sendmanydigidollar`, `redeemdigidollar` and `getredemptioninfo` take an
optional trailing `amount_unit` argument, `"cents"` or `"dollars"`, and follow
one contract (`src/digidollar/amount.cpp`):

| Request | Result |
|---------|--------|
| `5000` or `"5000"`, no unit | 5,000 cents = $50.00 (unchanged for existing callers) |
| `"50.00"` or `50.5`, no unit | **rejected**, error `-8`: `ambiguous amount: pass amount_unit=cents or amount_unit=dollars ...`; nothing is sent |
| `"50.00"`, `amount_unit="dollars"` | 5,000 cents; at most two decimals (`"12.345"` is rejected) |
| `5000`, `amount_unit="cents"` | 5,000 cents; must be an integer (`"50.00"` with `cents` is rejected) |
| `"1e3"`, `"+5"`, `" 5"`, `"5,000"`, `-5` | rejected: only a plain decimal number (digits, optional `.digits`) is accepted |
| more than 10,000,000 cents ($100,000) | rejected at the RPC boundary, under either unit |

Before v9.26.6 a decimal point silently meant dollars, so `5000.00` sent
$5,000.00 when the caller meant 5,000 cents. That guess is gone: pass integer
cents, or say `amount_unit="dollars"` explicitly. Named arguments work with
`digibyte-cli -named` and JSON-RPC named parameters.

The `min_amount` filter of `listdigidollarpositions` and the `min_balance`
filter of `listdigidollaraddresses` read their amounts the same way and take
the same `amount_unit`. One other change there: a negative `min_amount` or
`min_balance` used to be accepted and then ignored, so the call returned
everything and the caller never learned its filter had been dropped. It is now
rejected like any other negative amount.

```bash
digibyte-cli senddigidollar "DDrecipientAddress..." 5000
# Sends $50.00 worth of DD (integer cents, no unit needed)

digibyte-cli -named senddigidollar address="DDrecipientAddress..." amount="50.00" amount_unit="dollars"
# Also $50.00; the unit makes the decimal unambiguous

digibyte-cli senddigidollar "DDrecipientAddress..." 50.00
# error code -8: ambiguous amount: pass amount_unit=cents or amount_unit=dollars ...
```

**With optional comment:**
```bash
digibyte-cli senddigidollar "DDrecipientAddress..." 5000 "Payment for services"
```

**Response:**
```json
{
  "txid": "def456...",
  "to_address": "DDrecipientAddress...",
  "amount": 5000,
  "status": "success",
  "fee_paid": "0.10000000",
  "change_amount": 5000
}
```

### Important Notes

- Transfers require **DD UTXOs** (for the value) AND **DGB UTXOs** (for the miner fee)
- Minimum fee: **0.1 DGB** for transfer builders; mint and redeem builders enforce DGB fee floors in their txbuilder paths
- DigiByte uses **DGB/kB** for fee rates (not DGB/vB). The default DD fee rate is 35,000,000 sat/kB (≈0.35 DGB/kB), which yields ≈0.1 DGB on a typical ~300-vB tx
- Maximum single transfer: **$100,000** (10,000,000 cents)
- DD change is automatically returned to your wallet
- Transfers are **confirmed-only**: a DD UTXO must have at least one confirmation before it can be spent in a subsequent transfer or redeem. Consensus refuses to resolve DD amounts from `MEMPOOL_HEIGHT` inputs for transfer/redeem, and the wallet no longer chains unconfirmed DigiDollar outputs (commit `0b4959f563`). Plan throughput around the 15-second block time, or batch with `sendmanydigidollar`.
- Advanced wallet coin control can pass `selected_inputs` matching `listdigidollarunspent` rows. The deprecated `fee_rate` argument on send/redeem RPCs is ignored by the fixed DD fee policy.

### Which fee settings DigiDollar transactions actually use

DigiDollar mint, send and redeem do **not** use the wallet's general fee
settings. None of `-mintxfee`, `-paytxfee`, `-fallbackfee`, `-minrelaytxfee`
or `estimatesmartfee` is read anywhere in `src/rpc/digidollar.cpp`,
`src/wallet/digidollarwallet.cpp`, `src/digidollar/txbuilder.cpp` or the Qt
DigiDollar widgets. Changing them cannot fix or cause a DigiDollar fee
failure. What the code does use:

| Path | Fee rate | Where |
|------|----------|-------|
| `mintdigidollar` | 0.35 DGB/kB (`MIN_DD_FEE_RATE` = 35,000,000 sat/kB); the optional `fee_rate` argument is floored to it | `src/rpc/digidollar.cpp` (`MIN_DD_FEE_RATE`, the `fee_rate` floor) |
| `senddigidollar`, `sendmanydigidollar`, Qt Send $DD | 0.35 DGB/kB, fixed (`MIN_DD_TRANSFER_FEE_RATE`); the `fee_rate` argument is ignored | `src/wallet/digidollarwallet.cpp` (`TransferDigiDollarMany`, `PreflightDDTransferCapacity`) |
| `redeemdigidollar`, Qt Redeem (which calls the RPC) | 0.35 DGB/kB, fixed (`MIN_DD_FEE_RATE`); the `fee_rate` argument is ignored | `src/rpc/digidollar.cpp` (`redeemdigidollar`), `src/qt/walletmodel.cpp` (`executeRpc("redeemdigidollar")`) |
| Qt Mint | requests 500,000 sat/kB, but the builder's floor below applies | `src/qt/walletmodel.cpp` (`params.feeRate = 500000`) |

On top of the rate, every DigiDollar transaction pays at least the absolute
floor `MIN_DD_TX_FEE` = 0.1 DGB (`src/digidollar/txbuilder.cpp`, applied in
the mint, transfer and redeem builders and in the redemption fee estimate).
The size the rate is applied to comes from `EstimateTransactionVSize` in the
same file, which counts 110 witness bytes per input and adds a 35% safety
margin, so a one-fee-input redemption is about 400 vB and pays about 0.14 DGB;
each additional DGB fee coin adds roughly 90 vB, about 0.03 DGB.

The one DigiDollar-related transaction that *does* follow the wallet's
general fee settings is the automatic DGB consolidation sweep a mint performs
first when the collateral would need more inputs than one transaction may
carry: that is an ordinary DGB transaction built by `wallet::CreateTransaction`
(`src/rpc/digidollar.cpp` and `src/qt/walletmodel.cpp` mint paths), so
`-paytxfee`, fee estimation, `-fallbackfee` and `-mintxfee` apply to it as to
any DGB send.

The 0.1 DGB DigiDollar floor happens to equal the wallet's default
`-mintxfee` of 0.1 DGB/kB, which is why the two were confused in reports of
failed redemptions; those failures came from the old fixed-size fee-coin
guess described under "Fee coins for a redemption" in section 9, not from any
fee setting.

### Sending to many recipients in one transaction

Use `sendmanydigidollar` to fan out DD to many addresses with a single fee:

```bash
digibyte-cli -rpcwallet=hot sendmanydigidollar "" '{"DDaddr1...":1500,"DDaddr2...":2500}'
# amounts in cents

digibyte-cli -rpcwallet=hot -named sendmanydigidollar dummy="" amounts='{"DDaddr1...":"15.00","DDaddr2...":"25.00"}' amount_unit="dollars"
# one amount_unit applies to every recipient
```

This is the DigiDollar analogue of `sendmany`; the first argument must be the compatibility dummy string `""`. Every recipient amount follows the unit contract above, and the $100,000 cap applies per recipient. Like `senddigidollar`, it requires confirmed DD inputs and pays the fee in DGB. Large batches are limited by standard OP_RETURN relay size because one amount is committed for every DD output plus possible change; split large withdrawal batches and handle the RPC's "Too many DigiDollar recipients" error.

---

## 6. Receiving DigiDollars

Receiving DD works like receiving DGB — share your DD address and wait for the transaction.

**Watch for incoming DD:**
```bash
digibyte-cli listdigidollartxs 10 0 "" "receive"
# Lists last 10 received DD transactions
```

**Response includes:**
```json
{
  "txid": "ghi789...",
  "category": "receive",
  "amount": 5000,
  "address": "DDyourAddress...",
  "confirmations": 6,
  "blockheight": 12345,
  "time": 1770934000
}
```

---

## 7. Viewing Transaction History

```bash
# All DD transactions (last 20)
digibyte-cli listdigidollartxs 20

# Filter by category
digibyte-cli listdigidollartxs 10 0 "" "mint"
digibyte-cli listdigidollartxs 10 0 "" "send"
digibyte-cli listdigidollartxs 10 0 "" "receive"
digibyte-cli listdigidollartxs 10 0 "" "redeem"
digibyte-cli listdigidollartxs 10 0 "" "redeem_change"

# Filter by address
digibyte-cli listdigidollartxs 10 0 "DDspecificAddress..."
```

**Transaction categories:**
- `mint` — You minted new DD (locked DGB collateral)
- `send` — You sent DD to someone
- `receive` — You received DD from someone
- `redeem` — You redeemed DD back to DGB
- `redeem_change` — DD change returned to the wallet during an ERR/full redeem flow

History rows include `in_mempool` and `wallet_state` (`local`, `pending`, `confirmed`, `conflicted`, or `abandoned`). Send rows are negative amounts; receive rows are positive. `count` is capped at 1000 and `skip` must be non-negative.

---

## 8. Managing Collateral Positions

When you mint DD, you create a collateral position. You can view and manage these:

```bash
# List all your positions (filterable by tier / amount / active)
digibyte-cli listdigidollarpositions

# Check if a position can be redeemed
digibyte-cli getredemptioninfo "position_id"
```

`listdigidollarpositions` reports `unlock_height`, `status`, `spendable`, and `can_redeem` for each position (with optional filters: `[active_only=true] [tier_filter] [min_amount] [count] [skip]`); clients should filter on those fields rather than calling a separate "redeemable only" RPC. (The legacy `listredeemablepositions` symbol exists in `src/rpc/digidollar_transactions.cpp` but is not registered — that file's command table is never wired into the RPC server.)

Each position tracks:
- DD amount minted
- DGB collateral locked
- Lock tier and unlock height
- Whether it is pending, active, unlocked, pending redeem, or redeemed

---

## 8a. Wallet Lifecycle: load, unload, restart, rescan, restore

DigiDollar position state and DD owner keys are persisted inside the
wallet database as side tables (`dd_position`, `dd_balance`, `dd_owner_key`,
`dd_address_key`, `dd_transaction`) loaded by `DigiDollarWallet::LoadFromDatabase`.
Every standard wallet management
command works with DigiDollar wallets:

| Operation | What survives | Notes |
|-----------|---------------|-------|
| `unloadwallet "name"` | All DD records on disk | DD wallet RPCs return `-18 RPC_WALLET_NOT_FOUND` while no wallet is loaded. |
| `loadwallet "name"` | Positions, balances, DD owner keys, dd_transactions | After load, `listdigidollarpositions` and `getdigidollarbalance` reflect the same on-chain state. |
| `digibyted` stop / start | Same as above | `postInitProcess` re-runs `ScanForDDUTXOs()` to validate vault UTXO state against the active chain. |
| `rescanblockchain` | Idempotent — no double counting | Triggers a post-rescan call to `ScanForDDUTXOs()` -> `ValidatePositionStates()` so any vault that was redeemed off-wallet is correctly marked inactive. |
| `-reindex=1` | Same as restart | Wallet replays the chain; confirmed mints remain active until a real redeem/transfer spends the collateral on the active chain. |
| `backupwallet path` / `restorewallet new_name path` | Full DD state including owner keys | Restored wallet is loaded under `new_name`; existing wallet is untouched. |
| `importdescriptors` into a fresh wallet + `rescanblockchain` | Reconstructs DD positions from on-chain OP_RETURN metadata after proving ownership of the zero-value DD P2TR output | If the imported descriptors can provide the Taproot spending key, the wallet recovers and indexes that key for redemption. |

### Operator wallet recovery (descriptor wallet)

```bash
# 1. From the original (still-loaded) wallet, export descriptors with private keys
digibyte-cli -rpcwallet=mywallet listdescriptors true > /secure/path/dd_descriptors.json

# 2. On the recovery host, create a blank descriptor wallet and import them
digibyte-cli createwallet "restored" false true "" false true
digibyte-cli -rpcwallet=restored importdescriptors "$(cat /secure/path/dd_descriptors.json | jq '.descriptors')"

# 3. Run a full rescan so DigiDollar positions are reconstructed from the chain
digibyte-cli -rpcwallet=restored rescanblockchain

# 4. Verify
digibyte-cli -rpcwallet=restored listdigidollarpositions
digibyte-cli -rpcwallet=restored getdigidollarbalance
```

After step 4, check the restored positions and whether the wallet has their
matching spending keys. A visible position and an expired timelock alone do
not establish that redemption can succeed. It also needs the required DD burn,
DGB fees and a valid quote. Rescan can recover the Taproot spending key only
when the imported private descriptors provide it. Keep the original backups;
final wallet recovery integration and restore verification remain pending for
this development candidate.

### If redeeming reports a missing owner key

The owner key of a mint comes from the wallet's own chain of keys. The wallet
also writes that key down against the position, so redeeming can find it
straight away. That row can go missing: a database write failed, a restore
skipped it, or the node stopped at the wrong moment.

When it is missing, `redeemdigidollar` now searches the wallet's own keys for
the one that matches the DigiDollar token output of the mint. It includes the
unused addresses the wallet keeps ready beyond the last one it handed out. When
it finds the key it writes it down again and carries on with the redemption. It
never makes up a new key: only the key the collateral was locked with can
release it. The message you get tells you what is needed:

| Error | Meaning | What to do |
|-------|---------|------------|
| `DigiDollar redemption requires the wallet to be unlocked ... walletpassphrase` | The wallet is encrypted and locked. | `walletpassphrase "<passphrase>" 600`, then redeem again. |
| `Private keys are disabled for this wallet` | This is a watch-only wallet. It can show positions but cannot sign. | Redeem from the wallet that holds the private keys. |
| `This wallet does not hold the mint transaction of this position` | The position record exists but the mint transaction was never scanned into this wallet. | `rescanblockchain <height before the first mint>`, then redeem again. |
| `No key in this wallet matches the owner of this DigiDollar vault` | None of this wallet's keys locked this collateral. A different wallet or a different seed minted it. | Restore the wallet that minted it **with its private keys** (the wallet file itself, or `importdescriptors` with the private descriptors from `listdescriptors true`), run `rescanblockchain` from a height before the first mint, then redeem from that wallet. |
| `... could not be written to the wallet database` | The key was found but the wallet file refused the write. | Check free disk space and the wallet file, then try again. |

Restoring with public descriptors alone gives you a wallet that can watch but
cannot sign. Moving a wallet to a new machine needs the wallet file, or the
private descriptors, plus a rescan that starts before the first mint.

### Mint attempts that can no longer confirm

A mint names the exact height at which its collateral unlocks. A block can only
include that mint while the lock still left to run is at least the full length
of the tier the mint claims. That leaves about 100 blocks after the wallet built
it. Once the chain is past that, a mint that never confirmed and is in no
mempool can never be mined.

The wallet now gives such an attempt up by itself. It marks the transaction
abandoned, so the DGB it spent can be spent again. It removes the coin locks on
the collateral and token outputs. `listdigidollarpositions false` then shows the
position as `expired_mint`, and `listdigidollartxs` shows `wallet_state` as
`expired_mint`. A mint the network refused when it was sent shows as
`abandoned_mint`. One pushed out by a conflicting transaction shows as
`conflicted_mint`.

The owner key, the transaction and the position record are all kept. If a reorg
or a late block does confirm the mint after all, the position becomes active
again and its outputs are locked again as usual. A mint still sitting in the
mempool is never treated as expired.

From v9.26.6 `mintdigidollar` writes the owner key and the position record
**before** it sends the transaction, and every wallet write reports whether it
worked. If a write fails, the mint is not sent and the RPC returns
`Mint not broadcast: ...`.

### Recovering from a lost wallet file

If the wallet file is lost but the BIP39 seed / extended private key is
preserved, re-derive the descriptors with your wallet stack's seed-restore
tool, import them with
`importdescriptors`, and `rescanblockchain` from genesis. Active and redeemed
DD position state is reconstructed from chain data, and spend keys are cached
only when the imported descriptors can prove ownership of the DD output.

### Encrypted wallets

Encrypted wallets must call `walletpassphrase` before any RPC that derives,
exports, imports, or uses private DD/oracle keys: `getdigidollaraddress`,
`mintdigidollar`, `senddigidollar`, `sendmanydigidollar`,
`redeemdigidollar`, `createoraclekey`, `exportoracleprivkey`,
`importoracleprivkey`, and `startoracle` when it uses a wallet-stored key.
`loadwallet` and `unloadwallet` do not require the passphrase. Existing DD
positions remain visible to read-only RPCs while locked, but
`listdigidollarpositions` reports them as not spendable/not redeemable until
the wallet is unlocked.

### Wallet support for minting

Minting requires a descriptor wallet with private keys and HD key support.
A descriptor describes the keys and script for a wallet's addresses.
It needs an active ranged Taproot (`bech32m`) receiving descriptor and an
active ranged SegWit (`bech32`) internal change descriptor, both with private
keys. A ranged descriptor can derive a sequence of wallet addresses. Merely
having `descriptors=true` in `getwalletinfo` does not prove those capabilities.

| Wallet | Mint support |
|--------|--------------|
| Ordinary descriptor wallet with private keys | Supported if the required receiving/change descriptors are available |
| Encrypted supported wallet | Supported through the normal unlock flow |
| Legacy/BDB wallet | Refused; use a supported descriptor wallet for new mints |
| Watch-only or private keys disabled | Refused; public keys alone cannot sign |
| Blank or restricted descriptor wallet | Refused if the required key/address capability is absent |

Qt checks this before either mint confirmation dialog. RPC mint checks it
before requesting an unlock or deriving a key. The check itself does not
reserve an address or consume a key. Construction and signing can still fail
later, and confirmation remains subject to the applicable chain rules.
For complex imported descriptors, finding stored private keys does not prove
that the wallet can derive the required Taproot owner key. That later step
must succeed too.

Back up the existing wallet before changing its setup. For new mints, create
a normal descriptor wallet and transfer spendable DGB to it, or restore a
backup containing the required descriptors and private keys. Keep the original
wallet and its backups for existing funds and vaults. Creating a replacement
wallet does not recover a missing vault key.

`migratewallet` is marked experimental by its RPC help. It creates a legacy
backup, requires the passphrase for an encrypted wallet and requires a new
backup after migration. Do not run it automatically or assume every migrated
wallet supplies the mint capabilities above. Check the result before funding
new mints. A migration or rescan cannot recreate private keys absent from the
recovery material.

Source: [mint capability check](src/wallet/digidollarmintcapability.h),
[Qt mint flow](src/qt/digidollarmintwidget.cpp),
[RPC mint flow](src/rpc/digidollar.cpp) and
[migration help](src/wallet/rpc/wallet.cpp).

---

## 9. Redeeming DigiDollars

Redeeming burns DD tokens and unlocks your DGB collateral. The timelock must have expired.

```bash
# Redeem a position (must redeem full vault amount)
digibyte-cli redeemdigidollar "position_id" 10000
# position_id = the mint transaction hash
# amount = DD cents to redeem (must match full vault amount)

# The same, stated in dollars
digibyte-cli -named redeemdigidollar position_id="position_id" dd_amount="100.00" amount_unit="dollars"
# "100.00" without amount_unit is rejected as ambiguous (see section 5)
```

The amount names the vault principal and must equal it exactly; it is capped at
$100,000 like a send. During an emergency (ERR) the wallet computes the extra
DD it must burn from the vault itself, and that computed burn is not limited by
the cap.

### Fee coins for a redemption

A redemption pays its DGB fee from separate DGB coins in the wallet, never from
the collateral. The wallet chooses those coins from the projected size of the
transaction they produce and re-selects, a bounded number of times, when adding
a coin raises the fee past what the coins cover (`RedeemTxBuilder::EstimateRedemptionFee`
and `SelectRedemptionFeeInputs`, `src/digidollar/txbuilder.cpp`). A wallet
whose DGB is split into many small coins therefore redeems normally, using more
of them. If every coin is so small that it adds more fee than value, the RPC
fails with `Insufficient DGB fee inputs ... Consolidate small DGB coins into one
larger coin and retry`; send yourself one larger DGB payment first.

### Where the returned collateral and the leftover DGB go

The returned collateral goes to `redemption_address` when you pass one, and to
a new address of the redeeming wallet when you do not. The DGB left over after
the fee always goes to a separate change address of the redeeming wallet. If
the wallet cannot produce a change address, a redemption that names a
`redemption_address` now fails with a clear error instead of sending the
leftover to that address: the address you supply may be an exchange deposit
address or otherwise not yours, and money sent there does not come back.

### Two Redemption Paths

- **Normal** (system health ≥ 100%): Burn your original DD amount → get 100% of your collateral back
- **ERR** (system health < 100%): You may need to burn extra DD (up to 125%) to get your full collateral back. This creates buying pressure on DD during crises, helping stabilize the peg.

**Critical rule:** Collateral cannot be spent through the DD redemption paths before the timelock expires. There are no early liquidations or margin calls, but the collateral remains illiquid until expiry and redemption still requires the normal or ERR-adjusted DD burn.

---

## 10. Network Health & Oracle Data

```bash
# System-wide DD stats
digibyte-cli getdigidollarstats
# Returns: total DD supply, total collateral, system health ratio

# Current oracle price
digibyte-cli getoracleprice
# Returns: DGB/USD price, staleness info

# Check activation status
digibyte-cli getdigidollardeploymentinfo
# Returns: buried DigiDollar activation and separate tip/next-block Thaw Day status
```

---

### Circulating tokens, vault principal and estimates

`getdigidollarstats.total_dd_supply` is the circulating-supply field in cents:
issuance minus actual burns, subject to the legacy fallback limit below. `canonical_health.open_vault_principal` is the original DD
attached to vaults that remain open. It is not a wallet balance or a count of
all historical mints. Read canonical values only when `ready` is true.

Before Thaw Day, the legacy fallback used when `digidollarstatsindex` is
disabled scans vault amounts. Do not treat that fallback's `total_dd_supply`
as a verified circulating token count. Use a synchronized stats index for
circulation reporting below the transition. At and above Thaw Day, the
fallback reconstructs tokens separately from vault principal.

After Thaw Day, health uses open-vault principal. Closing a 100-DD vault with
a 125-DD burn reduces these totals by different amounts. The resulting health
can keep emergency mint restrictions active longer or require extra DD to
redeem another vault. The switch does not create tokens or change balances.

Top-level `health_rule_height` and `selected_health_denominator` describe tip
health. `next_block_health` describes the next candidate, with its own quote,
readiness and denominator. At tip H-1 these may legitimately use different
rules. Use current construction/estimate RPCs and handle a retry if chain state
or the quote changes; a displayed estimate does not bind the confirming block.
The [architecture guide](DIGIDOLLAR_ARCHITECTURE.md) explains the accounting.

## 11. Identifying DD Transactions (Raw Parsing)

If your wallet parses raw transactions, here's how to identify DD transactions:

**Check the transaction version:**
```
(tx.nVersion & 0x0000FFFF) == 0x0770  → It's a DD transaction
```

**Extract the type:**
```
(tx.nVersion & 0xFF000000) >> 24
  1 = MINT
  2 = TRANSFER
  3 = REDEEM
```

**Parse the DD OP_RETURN by transaction type:**
- Mint: `OP_RETURN "DD" 1 <dd_amount_cents> <unlock_height> <lock_tier> <owner_xonly_pubkey_32b>`
- Transfer: `OP_RETURN "DD" 2 <amount1> <amount2> ...`; assign amounts to zero-value DD P2TR outputs in output order, including change
- Redeem: `OP_RETURN "DD" 3 <dd_change_amount>` only when DD change exists; full redemption may have no DD OP_RETURN
- All DD amounts are integer cents

**DD token outputs** have 0-satoshi value with P2TR scripts. The actual DD value is in the OP_RETURN.

**Custom opcodes (Tapscript OP_SUCCESSx soft-fork, defined in `src/script/script.h:209-220`):**
| Opcode | Hex | Purpose |
|--------|-----|---------|
| `OP_DIGIDOLLAR` | `0xbb` | Marks DD outputs (Tapscript OP_SUCCESSx slot pre-activation) |
| `OP_DDVERIFY` | `0xbc` | Verify DD conditions (Tapscript OP_SUCCESSx slot pre-activation) |
| `OP_CHECKPRICE` | `0xbd` | Reserved and deterministically disabled; consumes one operand and pushes false |
| `OP_CHECKCOLLATERAL` | `0xbe` | Collateral ratio check |
| `OP_ORACLE` | `0xbf` | Coinbase oracle bundle marker (Tapscript OP_SUCCESSx slot pre-activation) |

Non-DD-aware wallets can safely ignore these — they behave as Tapscript OP_SUCCESSx (BIP-342) until `SCRIPT_VERIFY_DIGIDOLLAR` is set, which only happens after BIP9 `DEPLOYMENT_DIGIDOLLAR` is ACTIVE.

`OP_CHECKPRICE` is reserved and deterministically disabled; mint/redeem validation reads authenticated coinbase oracle bundles and the oracle price cache instead of a script-local price opcode. Oracle P2P messages, including `ORACLEHEARTBEAT` use `IsOracleP2PActive`.

---

## 12. RPC Quick Reference

### Wallet RPCs (require loaded wallet)

Registered in `GetWalletRPCCommands()` at `src/wallet/rpc/wallet.cpp`:

| Command | Description |
|---------|-------------|
| `getdigidollaraddress [label]` | Generate new DD deposit address |
| `listdigidollaraddresses [include_watchonly] [min_balance] [include_empty] [amount_unit]` | List DD addresses; empty generated addresses are hidden unless `include_empty=true`; `min_balance` follows the amount-unit contract |
| `getdigidollarbalance [addr] [minconf] [include_watchonly]` | Get DD balance (`confirmed`, `unconfirmed`, `total`) |
| `mintdigidollar <cents> <tier> [fee_rate]` | Mint DD by locking DGB collateral; amount is integer cents |
| `senddigidollar <addr> <amount> [comment] [fee_rate_ignored] [selected_inputs] [amount_unit]` | Send DD to a DD address; integer cents by default, `amount_unit` = `cents` or `dollars`, a decimal without a unit is rejected |
| `sendmanydigidollar "" <amounts_obj> [comment] [selected_inputs] [amount_unit]` | Send DD to multiple DD addresses in one tx; one `amount_unit` for all recipients |
| `listdigidollartxs [count] [skip] [addr] [category]` | List DD transaction history; categories include `mint`, `send`, `receive`, `redeem`, `redeem_change` |
| `listdigidollarunspent [minconf] [maxconf] [addresses] [include_unsafe]` | List DD UTXOs with `spendable` and `safe` flags |
| `listdigidollarutxos [minconf] [maxconf] [addresses] [include_unsafe]` | Alias for DD UTXO listing |
| `listdigidollarpositions [active_only] [tier_filter] [min_amount] [count] [skip] [amount_unit]` | List collateral positions; `min_amount` follows the amount-unit contract |
| `getredemptioninfo <position_id> [amount] [amount_unit]` | Check redemption status; optional amount must equal the full vault amount |
| `redeemdigidollar <position_id> <amount> [redemption_address] [fee_rate_ignored] [amount_unit]` | Redeem DD -> unlock DGB collateral; integer cents by default, `amount_unit` = `cents` or `dollars`, a decimal without a unit is rejected |
| `validateddaddress <address>` | Validate a DD address |
| `createoraclekey <oracle_id>` | Wallet-scoped oracle key generation |
| `exportoracleprivkey <oracle_id>` | Export a wallet-stored oracle private key for backup/migration |
| `importoracleprivkey <oracle_id> <private_key_hex> [replace]` | Import a wallet-stored oracle private key for recovery/migration |
| `startoracle <oracle_id> [private_key_hex]` | Start local oracle from a wallet-stored or supplied key |

### Information RPCs (no wallet needed)

Registered in `RegisterDigiDollarRPCCommands()` at `src/rpc/digidollar.cpp`:

| Command | Description |
|---------|-------------|
| `getdigidollardeploymentinfo` | BIP9 activation status, signaling progress |
| `getdigidollarstats` | Network-wide DD supply and health |
| `getdcamultiplier` | Current Dynamic Collateral Adjustment multiplier |
| `getoracleprice` | Current DGB/USD oracle price (from MuSig2 consensus) |
| `getalloracleprices` | Per-oracle price view (debug/status) |
| `getoraclesigners [blocks]` | Recent on-chain MuSig2 oracle-bundle signer IDs and metadata |
| `getprotectionstatus` | DCA / ERR / volatility protection state |
| `getoracles [active_only] [blocks]` | Oracle roster and local/remote status |
| `listoracle` | Local oracle status |
| `stoporacle <oracle_id>` | Stop a local oracle |
| `getoraclepubkey <oracle_id>` | Local oracle public key/status; wallet RPC paths can show the stored key before `startoracle` |
| `calculatecollateralrequirement <cents> <lock_days> [oracle_price_micro_usd]` | Calculate needed collateral by lock days (NOT tier) |
| `estimatecollateral <cents> <tier> [oracle_price_micro_usd]` | Estimate collateral; both `cents` and `tier` are required |
| `importdigidollaraddress <address> [label] [rescan] [p2sh]` | Validate a DD address and return the V1 unsupported/no-op warning; it does not import, mutate wallet state, or rescan |
| `setmockoracleprice <micro_usd>` | Regtest-only mock oracle price setter |
| `getmockoracleprice` | Regtest-only mock oracle price reader |
| `simulatepricevolatility <percent_change>` | Regtest-only volatility simulation |
| `enablemockoracle <enabled>` | Regtest-only mock oracle toggle |

### Qt GUI integration

DigiByte Core ships a DD lifecycle tab plus Qt widgets/dialogs/helpers: `digidollartab`, `digidollaroverviewwidget`, `digidollarsendwidget`, `digidollarreceivewidget`, `digidollarmintwidget`, `digidollarredeemwidget`, `digidollarpositionswidget`, `digidollartransactionswidget`, `digidollarcoincontroldialog`, `digidollarreceiverequest`, `ddaddressbookpage`, and `digidollar_qt_translate`. `DigiDollarTab` exposes the tabs `$DD Overview`, `Send $DD`, `Receive $DD`, `Mint $DD`, `Redeem $DD`, `$DD Vault`, and `$DD Transactions`, with an activation overlay until the buried DigiDollar height is reached. The Qt mint flow uses an HD-derived owner key and the wallet capability checks described above. Key storage is requested before wallet commit; that sequence alone does not establish durable recovery. See `REPO_MAP_DIGIDOLLAR.md` (Qt GUI section) for individual widget responsibilities.

#### Qt mint reject-reason translation (`DD-FA-DOC-010`)

The Qt mint widget calls `WalletModel::mintDigiDollar`. The wallet model
constructs and signs the transaction, then calls `CWallet::CommitTransaction`
through the wallet relay path. The widget checks wallet capabilities before
either confirmation dialog, retains the normal unlock flow and refreshes
estimates before construction. Commit or relay can still fail
if a quote or the chain changes, or if a remaining validation requirement is
not met. The wallet displays the returned reason through the translation
helper below.

In v9.26.2 (`DD-FA-FUNC-032`), the mint widget routes the broadcast reason through `qt/digidollar_qt_translate.h::TranslateMintRejectReasonForUser` before display. Known DD/oracle reject tokens are rewritten with a plain-English explanation and a remediation hint (e.g. `minting-blocked-during-err` is shown as "DigiDollar minting is paused because the system is in Emergency Redemption Ratio (ERR) recovery mode."). Unknown reasons pass through unchanged so operators retain forensic detail. The translator is unit-tested by `src/test/digidollar_qt_translate_tests.cpp` and is buildable without enabling Qt.

For wallet integrators that bypass the Qt widget and submit raw mint transactions via `sendrawtransaction`, the canonical consensus reject tokens above are stable and may be matched directly by RPC consumers; see `src/digidollar/validation.cpp` (`ValidateDigiDollarTransaction`) for the authoritative list.

---

## 13. Test on Testnet Now!

The public testnet configured in this source is **testnet26**. DigiDollar uses buried activation height 600. Thaw Day remains unscheduled in this development source; no public activation or soak result is claimed. Check `getdigidollardeploymentinfo` on the installed build.

### Quick Setup

1. Obtain the reviewed test build and network instructions from the release owner
2. Configure for testnet:
   ```ini
   testnet=1
   [test]
   digidollar=1
   txindex=1
   addnode=oracle1.digibyte.io:12033
   server=1
   rpcuser=yourusername
   rpcpassword=yourpassword
   ```
3. Launch: `digibyted -testnet -daemon`
4. Get testnet DGB from the dev chat: https://app.gitter.im/#/room/#digidollar:gitter.im
5. Start minting, sending, and receiving DD!

### Testnet Details

| Parameter | Value |
|-----------|-------|
| Testnet name | testnet26 |
| P2P Port | 12033 (set in `src/kernel/chainparams.cpp`) |
| DD Address Prefix | `TD` |
| Oracle Consensus | 35 active slots, 7 signatures required |
| Exchange Sources | Binance, CoinGecko, KuCoin, Gate.io, HTX, Crypto.com (6 active feeders, see `src/oracle/exchange.cpp:1092-1097`) |
| Outlier filter | Median-distance: a price is dropped when its distance from the median exceeds `outlier_threshold × median` (`MultiExchangeAggregator::FilterOutliers` at `src/oracle/exchange.cpp:1225`) |
| Activation | DigiDollar buried at 600; Thaw Day separately reported by `getdigidollardeploymentinfo` |

### Mainnet Activation

DigiDollar's buried mainnet activation height is 23,869,440. The separate
Thaw Day height is disabled in this development source. Before a coordinated
activation, upgrade the backend using the [node operations guide](doc/digidollar-operations.md).
See the [activation guide](DIGIDOLLAR_ACTIVATION_EXPLAINER.md) for the candidate
height boundary and old-node compatibility limits.

---

## Questions?

Join the developer chat: https://app.gitter.im/#/room/#digidollar:gitter.im

Track testnet activation: https://digibyte.io/testnet/activation

💎 DigiDollar — the first truly decentralized stablecoin on a UTXO blockchain.
