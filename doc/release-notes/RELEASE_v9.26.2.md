# DigiByte Core v9.26.2 DigiDollar Release Notes

DigiByte Core v9.26.2 is the formal DigiByte v9 release and the official
DigiDollar mainnet activation release.

This is not another RC, PRE build, modified-mainnet rehearsal, or oracle-only
test build. This release returns to normal DigiByte mainnet settings and is
intended for the full public network. Everyone running DigiByte infrastructure
should upgrade from v8.26.2 or older.

## Read This First

DigiByte v9 is a major new release line since v8.26.2. It updates the DigiByte
Core base to the Bitcoin Core v26.2 generation and adds the DigiDollar soft-fork
deployment.

DigiDollar does not become usable on mainnet simply because this software is
released. It activates through BIP9 miner signaling. You can follow deployment
status at:

https://digibyte.io/activation

Mainnet is normal mainnet:

| Setting | Value |
| --- | --- |
| Network | DigiByte mainnet |
| P2P port | `12024` |
| RPC port | `14022` |
| Onion target port | `14122` |
| Message start bytes | `fa c3 b6 da` |
| Data directory | default DigiByte mainnet data directory |
| Genesis block | unchanged DigiByte mainnet genesis |

Do not use the old pre-mainnet test ports, temporary data directories, or
modified activation settings with this release.

## Critical Consensus Fix: Retired-Algorithm Enforcement (Groestl)

Separately from DigiDollar, v9.26.2 ships an urgent consensus security fix.
**Every node on the DigiByte network must upgrade to v9.26.2.** This is not
optional and is independent of whether you use DigiDollar.

### What Happened

DigiByte secures the chain with five mining algorithms (SHA256d, Scrypt, Skein,
Qubit, Odocrypt). A sixth algorithm, Groestl, was retired in 2019 at the Odocrypt
fork. The rule that rejected retired-algorithm blocks existed in the v7.17.3-era
software, but it was accidentally dropped during the v8 Bitcoin Core rebase in
2021/2022. The function that knew Groestl was retired still existed and was used
for difficulty and display, but the single line that enforced it when accepting a
block was gone. Because nobody mined Groestl, its difficulty fell to the lowest
possible setting and the gap stayed dormant for years.

Starting 2026-06-28 16:40:05 UTC, at block 23,751,096, an actor — using AI to
analyze DigiByte's consensus rules — reactivated Groestl and began mining a sixth
algorithm at floor difficulty.

No coins were stolen and no confirmed transactions were reversed. There is no
evidence of a successful 51% attack, though the cheap mining is consistent with an
attempt: across every competing branch the network has seen, none ever accumulated
more total work than the honest chain, and the deepest reorganization of the
active chain was 4 blocks. The damage was instability and a network split. Block
times dropped from the 15-second target to roughly 12-13 seconds, and the network
divided, because v8/v9 software accepted the Groestl blocks while old v7.17.3
software rejected them and forked onto a separate, slower chain.

### The Fix

v9.26.2 restores retired-algorithm enforcement, the right way:

- Blocks using a retired (Groestl) or unknown mining algorithm are rejected.
- The rule is enforced in both header validation and block connection, so that
  `-reindex` and `-reindex-chainstate` also enforce it. A node cannot carry a
  post-activation retired-algorithm block forward after upgrading.
- Existing Groestl blocks already buried in the chain are grandfathered (kept).
  No history is rewritten and no transactions are reversed.

### Mainnet Activation Parameters (Groestl Deactivation)

| Field | Value |
| --- | --- |
| Deployment name | `algolock` |
| Versionbit (readiness signal) | `0` |
| Signaling start | June 29, 2026 |
| Activation height (backstop) | `23,808,000` (~7 days after release) |
| Enforcement | reject retired (Groestl) and unknown algorithms |
| Existing Groestl blocks | grandfathered below the activation height |

The `algolock` versionbit is a readiness signal that lets miners advertise they
have upgraded; you can watch adoption with `getdeploymentinfo`. The rule activates
at block 23,808,000 regardless of signaling, giving the network roughly seven days
to upgrade before retired-algorithm blocks are rejected.

### All Nodes Must Upgrade

Every full node, miner, pool, exchange, explorer, wallet, and service must upgrade
to v9.26.2. The majority of mining power is currently on the v8 line. That is fine,
but all of it must move to v9.26.2 so that the upgraded chain is the strongest
chain at activation and the network converges back onto a single chain.

### v7.17.3 And Older Nodes Must Reindex On Upgrade

Nodes running the ~7-year-old v7.17.3 line reject every post-Odocrypt Groestl
block, including the grandfathered blocks the healed chain keeps. They therefore
cannot follow the unified chain on their own. Operators on v7.17.3 (or older) must:

1. Upgrade to v9.26.2.
2. Reindex or resync (start with `-reindex`, or perform a fresh sync) so the node
   accepts the existing chain history and reorganizes onto the correct chain.

Upgrading also provides Taproot, DigiDollar, and every other improvement added
since v7.17.3.

### Miners And Pools: The 7-Day Window

- Upgrade to v9.26.2 within the ~7-day window before block 23,808,000.
- Use the block version returned by DigiByte Core and do not strip the `algolock`
  readiness signal.
- Once the majority of mining power is upgraded, retired-algorithm blocks are
  orphaned, the attack ends, and the network heals into one chain.
- Optional, during the window only: miners who wish to actively push back can point
  hash power at Groestl themselves. This captures block rewards that would
  otherwise go to the attacker and drives Groestl difficulty up, removing the
  cheap-mining advantage. This is secondary to upgrading and keeps block times fast
  while it lasts; after activation, Groestl is rejected regardless.

Forensic detail (as of this release, mining was still ongoing): the Groestl blocks
account for roughly 14-15% of all blocks since onset and are paid to two attacker
payout addresses — `dgb1qy5epvfs535a96tygn945a3a85lauh3ddu9v63y` (coinbase tag
`SORG`) and `D8S5JWaCrpFsryGG1c9AzWKhbS7e7VZ4r8`. Exchanges and custodians should
flag deposits originating from these addresses until the network has healed.

## Who Should Upgrade

All full nodes, miners, mining pools, exchanges, explorers, wallets, service
providers, and oracle operators should upgrade.

Old nodes can continue to see normal DigiByte blocks, but they are not
DigiDollar-aware. They cannot fully validate, display, index, mine, or serve the
new DigiDollar functionality. Miners and pools must upgrade and signal for the
soft fork if they want DigiDollar to activate.

Back up wallets before upgrading, especially wallets that hold oracle keys or
will hold DigiDollar positions.

## What Is DigiDollar?

DigiDollar is a native USD-denominated asset system built into DigiByte Core.
Each DD is designed to track one US dollar by using over-collateralized DGB
vaults, live oracle pricing, Dynamic Collateral Adjustment (DCA), Emergency
Reserve Ratio (ERR), and volatility protections.

DigiDollar is not an external smart contract, wrapped token, or custodial bank
reserve product. It is built into DigiByte consensus and wallet logic. Users
keep their own keys. DGB remains the collateral and the fee asset.

The basic user flow is:

1. Mint DD by locking DGB collateral in a Taproot vault.
2. Send DD between DigiDollar addresses.
3. Redeem DD by burning it and unlocking the corresponding DGB collateral under
   the consensus rules.

## Major Changes Since v8.26.2

### DigiByte Core v26.2 Foundation

v9.26.2 moves DigiByte onto the Bitcoin Core v26.2 generation of code. That
brings a large base-layer modernization compared with v8.26.2, including newer
networking, wallet, descriptor, RPC, mempool, build, and testing
infrastructure.

Notable inherited Core-era changes include BIP324 v2 transport support,
descriptor and Taproot wallet improvements, Taproot Miniscript support,
stricter wallet loading behavior, newer package/RPC tools such as
`submitpackage`, `importmempool`, `getaddrmaninfo`,
`getprioritisedtransactions`, and the v26-era RPC deprecations and serialization
cleanup.

DigiByte-specific consensus history remains intact. Historical DigiByte fork
heights, multi-algo behavior, DigiShield, MultiShield, DigiSpeed, Odocrypt, and
existing mainnet identity are not reset by this release.

### DigiDollar V1

v9.26.2 adds the first mainnet DigiDollar release:

- Native DD, TD, and RD DigiDollar address formats.
- Mint, send, batch-send, and redeem flows.
- Taproot collateral vaults.
- Confirmed-only DigiDollar UTXO spending.
- Canonical lock tiers, including the current short and long lock options.
- Minimum mint, maximum mint, output floor, and maximum transfer enforcement.
- DCA, ERR, and volatility protection logic.
- Network-wide DigiDollar statistics and optional stats indexing.
- Wallet restore, rescan, reindex, and reorg handling for DigiDollar state.
- OP_CHECKPRICE reserved and disabled for V1.

Mainnet mint/redeem safety depends on fresh MuSig2 oracle price bundles.
DigiDollar transfers are price-independent, but still require valid DD inputs
and DGB fee inputs.

### Oracle And MuSig2 System

DigiDollar mainnet uses a 7-of-35 oracle quorum. The oracle set publishes DGB/USD
price data using MuSig2 v0x03 aggregate bundles. The aggregate signature is
committed into the mined block when price-dependent DigiDollar mint or redeem
transactions need it.

Mainnet oracle parameters:

| Setting | Value |
| --- | --- |
| Oracle slots | `35` |
| Required signatures | `7` |
| Bundle type | MuSig2 v0x03 |
| Oracle epoch length | `40` blocks |
| Oracle update interval | `4` blocks |
| Active price sources | Binance, CoinGecko, KuCoin, Gate.io, HTX, Crypto.com |

Oracle peer seeding is operational, not consensus. The release consensus depends
on the 35 hardcoded oracle public keys and valid 7-of-35 MuSig2 bundles; it does
not depend on 35 public oracle DNS endpoints being live. For launch, oracle
operators should connect to the public v9.26.2 mainnet oracle seed peers we have
online, and more can be added later:

```ini
addnode=oracle1.digibyte.io:12024
addnode=digihash.digibyte.io:12024
```

Legacy fake-price RPCs and legacy oracle bundle paths are not part of mainnet
V1. Do not use old documentation that references `sendoracleprice`,
`submitoracleprice`, production mock-price controls, or legacy v0x01/v0x02
oracle bundles.

### Mainnet Activation Parameters

DigiDollar is deployed through BIP9.

| Field | Value |
| --- | --- |
| Deployment name | `digidollar` |
| Versionbit | `23` |
| Mainnet signaling start | June 1, 2026 00:00:00 UTC |
| Mainnet timeout | June 1, 2027 00:00:00 UTC |
| Signaling window | `40,320` blocks |
| Activation threshold | `28,224` blocks |
| Threshold percent | `70%` |
| Minimum activation height | `23,627,520` |

The start and timeout times are BIP9 median-time-past gates. They are not
automatic activation dates. DigiDollar only reaches `ACTIVE` after miners signal
enough support, the deployment reaches `LOCKED_IN`, and the next valid period
boundary reaches the minimum activation height or later.

DigiDollar, the oracle system, and DigiDollar MuSig2 are aligned to activate at
the same mainnet point. This avoids a split where DigiDollar rules are live but
the required oracle signing path is not.

### Testnet26

Public DigiDollar testing uses `testnet26`. Old testnet chain data should not be
reused for testnet26.

| Setting | Value |
| --- | --- |
| Testnet name | `testnet26` |
| Testnet P2P port | `12033` |
| Testnet RPC port | `14026` |
| Testnet onion target port | `14126` |
| Testnet DigiDollar activation | block `600` |

Testnet26 is for testing. Mainnet v9.26.2 uses normal DigiByte mainnet settings.

### Wallet And Qt

The DigiByte Qt wallet now includes DigiDollar wallet surfaces for receiving,
minting, sending, redeeming, viewing balances, viewing positions, and managing
DigiDollar state.

DigiDollar private-key actions require a descriptor/bech32m HD wallet with
private keys enabled. Legacy, blank, watch-only, or private-key-disabled wallets
can be useful for monitoring, but they cannot create DigiDollar deposit
addresses or sign DigiDollar withdrawals.

Encrypted wallets must be unlocked with `walletpassphrase` before private-key
DigiDollar actions such as minting, sending, redeeming, creating oracle keys, or
starting a wallet-backed oracle.

### RPC And Integration Surface

v9.26.2 adds DigiDollar and oracle RPC support for node operators, wallets,
exchanges, explorers, mining pools, and oracle operators.

Common node-level RPCs include:

- `getdigidollardeploymentinfo`
- `getdigidollarstats`
- `getdcamultiplier`
- `calculatecollateralrequirement`
- `estimatecollateral`
- `getoracleprice`
- `getalloracleprices`
- `getprotectionstatus`
- `getoracles`
- `getoraclesigners`
- `listoracle`
- `stoporacle`
- `getoraclepubkey`

Common wallet-level RPCs include:

- `getdigidollaraddress`
- `listdigidollaraddresses`
- `validateddaddress`
- `getdigidollarbalance`
- `listdigidollartxs`
- `listdigidollarpositions`
- `getredemptioninfo`
- `listdigidollarunspent`
- `listdigidollarutxos`
- `mintdigidollar`
- `senddigidollar`
- `sendmanydigidollar`
- `redeemdigidollar`
- `createoraclekey`
- `exportoracleprivkey`
- `importoracleprivkey`
- `startoracle`

For automated integrations, treat DigiDollar amounts as integer USD cents.
Decimal CLI compatibility exists, but exchanges and services should avoid
ambiguous decimal handling in production systems.

`importdigidollaraddress` is not a watch-only import feature in V1. It is a
validation/no-op stub and should not be used as an exchange custody workflow.

### Node Configuration

Services that need DigiDollar wallet, explorer, exchange, or oracle behavior
should run with DigiDollar and transaction indexing enabled:

```ini
server=1
txindex=1
digidollar=1
```

`txindex=1` is required for DigiDollar-capable mainnet/testnet nodes and for
explicit DigiDollar regtest runs because wallet recovery, validation, and
history queries need creating-transaction metadata.

Operators that need network-wide DigiDollar statistics can also enable the
optional stats index:

```ini
digidollarstatsindex=1
```

### Mining And Pools

DigiDollar mining has two separate pieces:

1. BIP9 signaling for the `digidollar` deployment.
2. DigiDollar-aware block templates using the `digidollar-oracle` GBT rule.

Miners and pools should use the block version returned by DigiByte Core and must
not strip versionbit 23 during `STARTED` or `LOCKED_IN`.

DigiDollar-aware pools and solo miners must request templates like this:

```bash
digibyte-cli getblocktemplate '{"rules":["segwit","digidollar-oracle"]}' scrypt
digibyte-cli getblocktemplate '{"rules":["segwit","digidollar-oracle"]}' sha256d
digibyte-cli getblocktemplate '{"rules":["segwit","digidollar-oracle"]}' skein
digibyte-cli getblocktemplate '{"rules":["segwit","digidollar-oracle"]}' qubit
digibyte-cli getblocktemplate '{"rules":["segwit","digidollar-oracle"]}' odo
```

When Core returns `default_oracle_commitment`, the miner or pool must copy that
exact script into the coinbase as one zero-value output. If Core does not return
`default_oracle_commitment`, do not invent one and do not force DigiDollar
mint/redeem transactions into the block.

Legacy miners that only request `{"rules":["segwit"]}` can keep mining normal
DGB blocks. They will not receive `default_oracle_commitment` and will not
receive oracle-priced DigiDollar mint/redeem work. Downstream ASIC and GPU
miners do not need DigiDollar-specific changes when the pool server builds the
coinbase correctly.

For direct CPU-mining tests with the patched cpuminer, use the explicit
`--digidollar` flag in solo GBT mode. That mode requests
`["segwit","digidollar-oracle"]` and preserves `default_oracle_commitment`.

Pool and miner implementers should read:

- `DIGIDOLLAR_MINING_INTEGRATION_GUIDE.md`
- `DIGIDOLLAR_WALLET_INTEGRATION.md`
- `DIGIDOLLAR_EXCHANGE_INTEGRATION.md`

Reference implementation examples are linked from the mining integration guide
for `cpuminer-multi`, `node-stratum-pool`, and `digihashv2`.

### Exchange And Wallet Integration

DigiDollar addresses are not normal DGB addresses. Mainnet DigiDollar addresses
begin with `DD`. Testnet uses `TD`. Regtest uses `RD`.

Exchanges should:

- Run DigiByte Core v9.26.2 or newer with `digidollar=1` and `txindex=1`.
- Use descriptor wallets with private keys enabled for custody.
- Allocate one DigiDollar address per customer with `getdigidollaraddress`.
- Validate withdrawals with `validateddaddress`.
- Detect deposits with `listdigidollartxs` and `getdigidollarbalance`.
- Track `txid`, `vout`, `blockhash`, `blockheight`, `confirmations`,
  `in_mempool`, and `wallet_state`.
- Maintain a DGB hot-wallet reserve for fees.
- Use `senddigidollar` or `sendmanydigidollar` for withdrawals.

DD transfer withdrawals do not need a fresh oracle quote. Mint and redeem paths
do need recent valid MuSig2 oracle data.

### Limits And Amount Rules

DigiDollar V1 enforces simple amount boundaries so wallets, exchanges, and users
do not need to guess.

| Rule | Mainnet value |
| --- | --- |
| Minimum DD output | `$1` / `100` cents |
| Minimum mint | `$100` / `10,000` cents |
| Maximum mint | `$100,000` / `10,000,000` cents |
| Maximum single transfer | `$100,000` / `10,000,000` cents |
| DD transaction fee floor | `0.1 DGB` |

DigiDollar amounts are represented internally as integer cents.

### Privacy And Network Changes

The v9 line includes modernized networking and privacy-related infrastructure,
including the Bitcoin Core v26-era P2P stack and DigiByte's Dandelion++ privacy
work. These changes are part of the broader DigiByte v9 upgrade and are
separate from DigiDollar activation.

### Testing And Validation Coverage

The v9 DigiDollar development cycle added broad validation coverage across:

- DigiDollar consensus and policy unit tests.
- Oracle and MuSig2 unit tests.
- BIP9 activation and height-gate tests.
- Functional tests for mint, send, redeem, wallet restore, wallet encryption,
  rescan, reindex, reorg, pending states, DCA, ERR, volatility, oracle pricing,
  mining, GBT, and network relay behavior.
- Fuzz coverage for DigiDollar parsing and validation paths.
- Qt wallet coverage for DigiDollar user workflows.
- Multi-oracle testnet and modified-mainnet rehearsal testing.

Final release tagging should include the exact signed-binary validation results
for the published build.

## What Does Not Change

- DigiByte remains DigiByte.
- Mainnet genesis is unchanged.
- Mainnet message-start bytes are unchanged.
- Mainnet P2P/RPC ports are unchanged.
- Historical DigiByte fork heights are retained.
- Multi-algo mining remains part of DigiByte.
- DGB remains the native fee and collateral asset.
- Normal DGB transactions continue to work.

## Important Upgrade Notes

- Back up wallets before upgrading.
- Enable `txindex=1` for DigiDollar-capable services.
- Use descriptor wallets with private keys enabled for DigiDollar custody.
- Do not rely on obsolete PRE, RC, testnet25, or modified-mainnet documentation.
- Do not use removed oracle RPC names such as `sendoracleprice` or
  `submitoracleprice`.
- Do not treat production mock-price controls as mainnet oracle behavior.
- Do not assume DigiDollar is active until BIP9 shows `ACTIVE`.
- Miners and pools must preserve `default_oracle_commitment` exactly when Core
  returns it.

## Useful Status Commands

Check chain and activation state:

```bash
digibyte-cli getblockchaininfo
digibyte-cli getdeploymentinfo
digibyte-cli getdigidollardeploymentinfo
```

Check oracle and price state after activation:

```bash
digibyte-cli getoracleprice
digibyte-cli getalloracleprices
digibyte-cli getoracles
digibyte-cli getoraclesigners
digibyte-cli getdigidollarstats
```

Check a DigiDollar-aware mining template:

```bash
digibyte-cli getblocktemplate '{"rules":["segwit","digidollar-oracle"]}' scrypt
```

If a fresh oracle bundle is ready, the template may include
`default_oracle_commitment`. If it is absent, mine the returned normal template
or wait for a fresh oracle-aware template.

## Documentation

Read these integration documents before deploying production DigiDollar
infrastructure:

- `DIGIDOLLAR_EXPLAINER.md`
- `DIGIDOLLAR_ARCHITECTURE.md`
- `DIGIDOLLAR_WALLET_INTEGRATION.md`
- `DIGIDOLLAR_EXCHANGE_INTEGRATION.md`
- `DIGIDOLLAR_MINING_INTEGRATION_GUIDE.md`

## Final Reminder

v9.26.2 is the formal DigiByte v9 DigiDollar release. Upgrade, signal, and
verify activation status from the network. DigiDollar mainnet activation is a
BIP9 process, not a manual switch and not a testnet-only feature.
