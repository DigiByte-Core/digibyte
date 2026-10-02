# DigiByte Core v9.26.5 DigiDollar Release Notes

DigiByte Core v9.26.5 is a patch release on top of v9.26.4 in the formal
DigiByte v9 / DigiDollar mainnet release line, and it is the **first release
after DigiDollar activated on mainnet**. DigiDollar locked in through BIP9
miner signaling and became active at block **23,869,440** on 17 July 2026,
alongside the AlgoLock retired-algorithm rule. Taproot has been active since
block 21,168,000.

v9.26.5 contains two changes:

1. **A startup fix** — the DigiDollar oracle startup scan no longer hangs the
   node for ~15 minutes on mainnet; it now completes in a few seconds.
2. **Deployment burial (BIP90)** — the Taproot, DigiDollar, and AlgoLock soft
   forks, all now active on mainnet, are converted from live BIP9 version-bits
   deployments into buried deployments with hardcoded activation heights — the
   same housekeeping Bitcoin Core performed for CSV and SegWit after their
   activations.

Neither change alters which blocks or transactions are valid on the current
mainnet or testnet chains. The burial does change several RPC output shapes
and regtest options — see "Breaking Interface Changes" below before upgrading
anything that parses deployment status.

---

## Changes In v9.26.5 Since v9.26.4

- Fixed the ~15-minute DigiDollar oracle startup hang on mainnet (RPC error
  -28 / "Verifying blocks…" / stalled P2P while the node started). Startup is
  now a few seconds.
- Buried the Taproot, DigiDollar, and AlgoLock deployments (BIP90): activation
  is now a fixed per-network height; the BIP9 state machine no longer runs for
  them and blocks no longer signal bits 2, 23, or 0.
- `getdigidollardeploymentinfo` has a new, simpler "buried" output shape (the
  BIP9 signaling fields are gone), and `getdeploymentinfo` now reports the
  three deployments as `type: "buried"`.
- `getblocktemplate` version fields are permanently clean of deployment
  signaling bits; the `rules` array now always lists `taproot`, `digidollar`,
  and `algolock` once active.
- Regtest testing knobs changed: `-digidollaractivationheight=N` now activates
  DigiDollar at exactly height N; new
  `-testactivationheight=taproot|digidollar|algolock@HEIGHT` overrides exist;
  `-vbparams` for the three buried names is now a startup error.
- Versionbits warning heights raised so historical signaling does not trigger
  spurious "unknown new rules" warnings.
- Documentation updated across the DigiDollar doc set; full release notes in
  `doc/release-notes/release-notes-9.26.5.md`; new unit and functional tests
  pin the burial heights and boundary behavior.

---

## Read This First

**DigiDollar is live on mainnet.** There is nothing left to signal and nothing
to configure: any v9.26.5 node on mainnet enforces DigiDollar, AlgoLock, and
Taproot from their recorded activation heights.

Upgrading is drop-in: shut down, replace binaries, restart. **No reindex is
required** and no configuration changes are needed for normal full or pruned
nodes. Wallets, mining, pruning, and oracle operation continue to work as in
v9.26.4.

Mainnet is normal mainnet:

| Setting | Value |
| --- | --- |
| Network | DigiByte mainnet |
| P2P port | `12024` |
| RPC port | `14022` |
| Data directory | default DigiByte mainnet data directory |
| Genesis block | unchanged DigiByte mainnet genesis |

---

## Fix: DigiDollar Oracle Startup Hang (~15 Minutes → Seconds)

### What Happened

On startup, a DigiDollar node rebuilds its in-memory oracle price and
volatility state by scanning the last 172,800 blocks (~30 days). In v9.26.4,
every scanned block re-evaluated the DigiDollar activation gate through a code
path that allocated a *throwaway* versionbits cache and re-ran the BIP9
threshold state machine from scratch — roughly 172,800 times, with nothing
memoized. On mainnet, right after activation, this took ~15 minutes, held the
main lock before RPC warmup finished (so RPC returned error -28, the GUI sat
on "Verifying blocks…", and P2P stalled), and wrote 150,000+ per-block log
lines.

### The Fix

The startup per-block gate now goes through the node's shared, memoized
versionbits cache — the exact same lookup block validation already uses — and
the per-block log flood was replaced with a single aggregate line. Verified
live on mainnet: the oracle startup scan dropped from ~15 minutes to ~3
seconds, and the node came up healthy and caught up.

This is a pure performance change with no consensus impact: unit and
functional tests prove the startup gate computes the identical activation
result at every height, and that a restart equals a `-reindex` equals the
pre-restart oracle state.

With the deployment burial below, every DigiDollar activation check in the
node becomes a single height comparison, which removes this entire class of
cost permanently — including the per-block cost that block validation itself
was still paying during sync.

---

## Housekeeping: Taproot, DigiDollar, And AlgoLock Are Now Buried (BIP90)

### What "Buried" Means

A soft fork activates once. After it is active and buried deep in the chain,
there is no reason to keep re-running the BIP9 signaling state machine for it
on every node forever. "Burying" a deployment (BIP90) replaces the state
machine with the recorded activation height, hardcoded in chainparams —
exactly what DigiByte and Bitcoin already did for CSV, SegWit, ReserveAlgo,
and Odo.

### Activation Heights

The heights are the real, on-chain BIP9 activation heights — read from live
mainnet `getdeploymentinfo` and verified block-by-block on testnet26
(signaling in blocks 200–599, active at exactly 600):

| Deployment | BIP9 bit | Mainnet | Testnet26 | Signet / Regtest |
| --- | --- | --- | --- | --- |
| Taproot (BIPs 340-342) | 2 | 21,168,000 | 0 (always active) | 0 |
| DigiDollar | 23 | 23,869,440 | 600 | 0 |
| AlgoLock (retired-algo rejection) | 0 | 23,869,440 | 0 (always active) | 0 |

### Why Bury Them

- The BIP9 machinery serves no further purpose for these deployments: they can
  never re-signal, time out, or fail.
- It removes all remaining BIP9 threshold-state recomputation from block
  validation and startup. Every DigiDollar activation check is now one height
  comparison.
- Blocks stop signaling bits 2, 23, and 0, so `getblocktemplate` version
  fields are permanently clean for pool software (this also structurally
  resolves the bit-23 version-rolling interference some SHA256D pool stacks
  reported during signaling).
- It matches upstream practice and shrinks the consensus surface.

### What Deliberately Did NOT Change

- **The static DigiDollar gates keep their historical floors:**
  `nDDActivationHeight = nOracleActivationHeight = nDigiDollarMuSig2Height =
  23,627,520` on mainnet (600 on testnet26). The 23,627,520 floor is
  intentionally *below* the 23,869,440 activation height; it continues to
  drive the pruning floor, the prune lock, and the pre-floor collateral gate
  exactly as in v9.26.4, so existing pruned nodes are unaffected.
- **The AlgoLock static backstop** (`nGroestlDeactivationHeight = 23,808,000`)
  remains and is still enforced alongside the deployment check —
  retired-algorithm blocks are rejected over exactly the same height ranges as
  before.
- **Taproot script validation** was already applied unconditionally, so the
  Taproot burial has zero script-consensus effect.

### Consensus Compatibility

A v9.26.5 node accepts and rejects exactly the same blocks as a v9.26.4 node
for every block on the current mainnet and testnet chains. The standard BIP90
caveat applies, as it did for Bitcoin's burials: only in the hypothetical of
an astronomically expensive deep reorganization back below the activation
heights could pre-burial and post-burial nodes judge a re-mined chain
differently. This affects no real chain and does not change the security model
in practice.

### Versionbits Warnings

`MinBIP9WarningHeight` rises to 23,909,760 on mainnet (activation height plus
one 40,320-block confirmation window) and 800 on testnet26, so the historical
bit-2/23/0 signaling periods do not trigger spurious "unknown new rules
activated" warnings now that upgraded nodes no longer set those bits.

---

## Breaking Interface Changes

### RPC

- **`getdigidollardeploymentinfo`** has a new shape. It now returns:
  - `enabled` — whether DigiDollar is active at the current tip
  - `type` — always `"buried"`
  - `status` — `"active"` or `"defined"`
  - `activation_height` — the burial height (omitted only if the deployment
    is disabled on the network)
  - plus the unchanged oracle/MuSig2 fields (`oracle_activation_height`,
    `musig2_format_activation_height`, `oracle_pubkey_count`,
    `oracle_consensus_required`, `oracle_total_slots`, `oracle_seed_peers`,
    `musig2_session{...}`)

  The BIP9 fields are **removed**: `bit`, `start_time`, `timeout`,
  `min_activation_height`, `blocks_until_timeout`, `signaling_blocks`,
  `threshold`, `period_blocks`, `progress_percent`. `status` can no longer be
  `started`, `locked_in`, or `failed`. `activation_height` now reports the
  exact activation height (previously a back-scan that could differ by one).

- **`getdeploymentinfo`** (and REST `/deploymentinfo`): the `taproot`,
  `digidollar`, and `algolock` entries are now rendered as buried
  deployments — `{"type": "buried", "active": <bool>, "height": <N>}` — with
  no `bip9` sub-object. Anything reading
  `deployments.digidollar.bip9.status` must switch to `active` / `height`.

### getblocktemplate

- `rules` now always contains `"taproot"`, `"digidollar"`, and `"algolock"`
  once active (hardcoded, like `"csv"`; no `!` prefix, so clients that do not
  understand them may safely proceed).
- `vbavailable` no longer mentions them, and the template `version` never sets
  bits 2, 23, or 0. Block versions on the network stop carrying those bits.
- The `digidollar-oracle` opt-in rule and `default_oracle_commitment`
  mechanics are unchanged from v9.26.4.

### Command-Line Options (regtest only)

- `-vbparams=taproot:...`, `-vbparams=digidollar:...`, and
  `-vbparams=algolock:...` are now a **startup error** ("Invalid
  deployment"). Only `testdummy` remains a versionbits deployment.
- `-digidollaractivationheight=N` now activates DigiDollar at **exactly
  height N** (it sets the buried height together with the static
  DD/oracle/MuSig2 gates). Pre-burial, this knob ran real BIP9 signaling and
  activation landed on the first 144-block window boundary at or above
  max(432, N) — e.g. N=650 used to activate at 720. Scripts relying on the
  old window-boundary timing must be updated.
- New: `-testactivationheight=taproot@H` / `digidollar@H` / `algolock@H` set
  only the buried deployment height. For `digidollar@H` the static DD/oracle
  gates keep their regtest defaults (650), while `nDigiDollarMuSig2Height` is
  derived as `min(nDDActivationHeight, DigiDollarHeight)` and so follows H
  below 650. `-digidollaractivationheight` takes precedence when both are
  given.

None of these affect mainnet or testnet operation.

---

## Mining And Pools

No action is required. Templates keep working as in v9.26.4; the only visible
differences are that the template `version` field no longer carries any
deployment signaling bits and that `rules` explicitly lists the three active
soft forks. Pools that narrowed version-rolling masks or filtered bit 23
during the signaling period can drop those workarounds.

DigiDollar-aware mining is unchanged:

```bash
digibyte-cli getblocktemplate '{"rules":["segwit","digidollar-oracle"]}' sha256d
```

---

## Who Should Upgrade

Everyone, at normal operational convenience — this release contains no
consensus rule change for the live chains, so there is no coordination
deadline. Upgrade sooner if:

- You restart nodes regularly (the ~15-minute startup hang is fixed).
- You run infrastructure that parses `getdigidollardeploymentinfo` or
  `getdeploymentinfo` (block explorers, dashboards, exchange tooling) — update
  your parsers for the new buried shapes at the same time.
- You run regtest-based CI that uses `-vbparams` or
  `-digidollaractivationheight` — migrate per the section above.

---

## What Does Not Change

- Consensus validity of every block on mainnet and testnet26.
- Ports, data directories, network magic, genesis, addresses.
- Wallet files, DigiDollar positions, mint/send/redeem behavior, lock tiers,
  collateral rules, DCA/ERR/volatility protection.
- The oracle roster (35 slots, 7-signature MuSig2 quorum on mainnet/testnet)
  and the v0x03 on-chain bundle format.
- Pruning behavior and the DigiDollar prune floor (still anchored at
  23,627,520 on mainnet).
- P2P protocol, Dandelion++, BIP324 v2 transport option.

---

## Useful Status Commands

Check chain and deployment state (note the new buried shapes):

```bash
digibyte-cli getblockchaininfo
digibyte-cli getdeploymentinfo
digibyte-cli getdigidollardeploymentinfo
```

Expected on mainnet: every deployment reports `"type": "buried"` and
`"active": true` (only `testdummy` remains BIP9), and
`getdigidollardeploymentinfo` reports `"status": "active"` with
`"activation_height": 23869440`.

Check oracle and price state:

```bash
digibyte-cli getoracleprice
digibyte-cli getalloracleprices
digibyte-cli getoracles
digibyte-cli getdigidollarstats
```

---

## Testing And Validation Coverage

- Full unit suite: 3,413 test cases, no errors — including a new
  `deployment_burial_tests` suite that pins the per-network burial heights,
  their confirmation-window alignment, the preserved activation floors, the
  regtest knob semantics, and the exact activation boundary.
- Full functional test suite: all tests pass. The DigiDollar activation tests
  were rewritten from BIP9 signaling ladders to buried-boundary lifecycles
  (pre-activation rejection, exact boundary flip, post-activation
  mint/send/redeem, reorg-below-activation purge, multinode/IBD/reindex
  parity, pruning).
- The startup fix's consensus neutrality is pinned by unit and functional
  tests (restart == `-reindex` == pre-restart oracle state) and was verified
  live on mainnet.
- The burial heights were verified against the live mainnet chain and a fully
  synced testnet26 node before hardcoding, and the change was reviewed by
  independent adversarial passes over the consensus diff, the RPC/GBT surface,
  and the test migration.

---

## Documentation

- `doc/release-notes/release-notes-9.26.5.md` — full release notes for this
  version.
- `DIGIDOLLAR_ACTIVATION_EXPLAINER.md` — now documents the burial and
  preserves the complete BIP9 activation history as a historical record.
- `DIGIDOLLAR_EXPLAINER.md`, `DIGIDOLLAR_ARCHITECTURE.md`,
  `DIGIDOLLAR_ORACLE_ARCHITECTURE.md`, `DIGIDOLLAR_WALLET_INTEGRATION.md`,
  `DIGIDOLLAR_EXCHANGE_INTEGRATION.md` — updated where the burial changed
  interfaces or predicates.

---

## Final Reminder

DigiDollar is live on DigiByte mainnet. v9.26.5 is the post-activation
maintenance release: it makes node startup fast again and retires the
now-finished activation machinery the safe, precedented way. Upgrade at your
convenience, and update anything that parses deployment status before you do.
