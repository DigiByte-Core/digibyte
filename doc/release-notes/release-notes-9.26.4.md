DigiByte Core version 9.26.4
============================

DigiByte Core v9.26.4 is a patch release on top of v9.26.3. It lets mining pools
and users run a **pruned** node with DigiDollar — a full-validating node that keeps
only a few gigabytes of blocks instead of the entire ~12-year, ~40 GB block history.

**In one line:** add `prune=2000` to your `digibyte.conf`, upgrade, and your node
runs DigiByte + everything DigiDollar needs on a fraction of the disk.

**This release adds one narrowly scoped consensus rule.** Redemption collateral
classification is now gated on the DigiDollar activation floor: a coin created below
the floor is never treated as vault collateral, whether it appears as the redemption's
input 0 (rejected as `bad-collateral-release-not-vault`) or as a fee input (see
`ValidateCollateralReleaseAmount` in `src/digidollar/validation.cpp`). This rule is
what keeps pruned and full nodes in agreement, and the pre-release mainnet scan found
no existing coins it affects (`V9.26.4_MAINNET_VALIDATION.md`). v9.26.2's Groestl
algolock and the DigiDollar BIP9 deployment are carried forward unchanged, and a
pruned v9.26.4 node accepts and rejects exactly the same blocks and transactions as a
full node. Upgrading is optional — a node that does not set `-prune` behaves like
v9.26.3 except for this one rule; node operators, pools, and exchanges should treat it
as a consensus rule addition when assessing upgrade urgency.


How to Upgrade
==============

Shut down DigiByte Core, replace the binaries, and restart. A reindex is **not**
required to upgrade a normal (full) node.

To run in pruned mode, add one line to `digibyte.conf`:

    prune=2000
    # no txindex line — prune turns the transaction index off automatically

`prune=N` is a target size in **MiB** (minimum 550; 2000 gives comfortable headroom).
See "How to enable pruning" below for the full steps.


Background: where v9.26.4 fits in the v9.26 line
================================================

If you are new to DigiByte v9, this section explains the moving parts. Nothing here
is new in v9.26.4 — it is the context every v9.26 node already runs under.

**DigiByte v9 = Bitcoin Core v26.2 base + DigiDollar.** The v9 line rebased DigiByte
onto the Bitcoin Core v26.2 generation and added **DigiDollar**, a native USD-pegged
stablecoin, together with a multi-oracle DGB/USD price feed secured by MuSig2 Schnorr
threshold signatures (35 oracle slots, 7 signatures required for consensus on
mainnet and testnet).

**DigiDollar is not active yet, and installing this software does not activate it.**
DigiDollar turns on through **BIP9 miner signaling** (deployment bit 23). Until the
network signals it in, all DigiDollar consensus, RPC, and P2P surface stays dormant;
the node follows the deployment automatically via `IsDigiDollarEnabled()`. The
`-digidollar` flag is not required and only affects regtest. Activation status:
https://digibyte.io/activation

**The activation floor.** DigiDollar can never activate below a fixed minimum height:
**23,627,520 on mainnet** (block 600 on testnet26). This single number is the key to
this release — see "Why pruning is safe" below. On mainnet the oracle, MuSig2, and
DigiDollar height gates all collapse to that same height.

**What each v9.26 patch added:**

- **v9.26.2** — the formal DigiDollar mainnet release, and an urgent consensus
  security fix: the **Groestl algolock**, which re-enforces the rejection of the
  retired Groestl mining algorithm (accidentally dropped during the v8 rebase, then
  exploited in June 2026). This was a mandatory, network-wide upgrade. It is carried
  forward unchanged in v9.26.4 and continues to reject retired-algorithm blocks.
- **v9.26.3** — fixed a fresh-node header-sync loop (mainnet `nMinimumChainWork`
  reverted to `0x00`) and made `-txindex` **on by default** so DigiDollar nodes start
  out of the box.
- **v9.26.4 (this release)** — makes DigiDollar compatible with pruning, so the
  txindex-and-full-history requirement introduced by v9.26.3 is no longer forced on
  operators who want a small node.


Notable changes
===============

Pruning with DigiDollar
-----------------------

Before v9.26.4, a DigiDollar node had to run `-txindex` and could not prune. The two
are mutually exclusive in Bitcoin Core, and DigiDollar validation resolved a spent
DigiDollar output's amount and lock term through the transaction index — which needs
the full chain. That meant every DigiDollar node, including a mining pool that only
cares about the tip, had to store the entire history plus the index. v9.26.4 removes
that restriction.

**Why pruning is safe (the core idea).** A DigiDollar coin can only be *created* at or
after DigiDollar activates, and activation can never happen below the activation floor
(mainnet 23,627,520). Therefore every block that DigiDollar validation will *ever* need
to read lives in the window from that floor to the chain tip. Everything below the
floor is ordinary pre-DigiDollar history that DigiDollar never needs — so it is safe to
delete forever. A pruned v9.26.4 node keeps the `[floor, tip]` window and prunes the
rest, using three small pieces of wiring:

1. **A prune lock at the activation floor.** At startup the node registers a
   "digidollar" prune lock at the floor height. Both automatic (size-target) pruning
   **and** the `pruneblockchain` RPC are clamped below it, so a pruned node can never
   delete a DigiDollar-era block — even if you explicitly ask it to.

2. **No transaction index needed under prune.** DigiDollar amount/lock lookups read the
   creating transaction directly from the retained block at the coin's height instead
   of from the index. Every lookup fails *closed*: if data cannot be found, the
   transaction or block is rejected — the node never accepts something it could not
   verify.

3. **A startup safety guard.** If a pruned data directory is ever missing or cannot
   read a DigiDollar-era block (for example, it was pruned under different rules or a
   block file is damaged), the node **refuses to start** and asks you to restart with
   `-reindex`, rather than validating DigiDollar with incomplete data.

**Result:** a pruned node validates, mines, mints, sends, redeems, and can even run a
DigiDollar oracle exactly like a full node, on a fraction of the disk.

**Pruned nodes validate identically to full nodes.** DigiDollar's collateral checks
are gated on the activation floor so a pruned node (which does not keep pre-floor
blocks) and a full node (which does) reach the same verdict on every redeem. A coin
created below the floor can never be real collateral, and both node types now treat it
that way. There is no reachable state in which a pruned node accepts or rejects a
DigiDollar transaction differently from a full node.


How to enable pruning (`digibyte.conf`)
---------------------------------------

**Upgrading an existing v9.26.3 node (the mining-pool path):**

1. Shut the node down.
2. Edit `digibyte.conf`: **add** `prune=2000`, and **remove** any `txindex=1` line.
   With no `txindex` line present, v9.26.4 turns the index off automatically under
   prune. (Testnet options go under a `[test]` section — remove `txindex=1` there too.)
3. Replace the binaries with v9.26.4 and start. The node prunes **in place** — no
   resync and no reindex. The first start spends a few extra minutes, one time, marking
   the old blocks as pruned. Confirm it worked by looking for this line in `debug.log`:

       DigiDollar: pruning enabled; retaining all blocks at/above height 23627520 (DigiDollar activation floor)

**A fresh node:** use the same config; it syncs from the network and prunes as it goes.

Notes: `prune=1` selects manual mode (pruning only happens when you call the
`pruneblockchain` RPC). Wallets and `getblocktemplate` mining work normally. To return
to a full archival node later, remove `prune=`, set `txindex=1`, and restart with
`-reindex` (the node re-downloads the full chain).


Notes and limitations for pruned nodes
--------------------------------------

- **Disk grows after activation.** A pruned node keeps the DigiDollar-era window plus
  the chainstate instead of the full chain plus a transaction index. That window grows
  with the chain: once DigiDollar activates, every block from just below the activation
  height to the tip is retained permanently, so disk usage grows over time and can
  exceed the `prune=N` target. What pruning saves is the ~12 years of pre-DigiDollar
  history (and the index) — a one-time saving, not a fixed size cap.
- **Serving history.** A pruned node advertises `NODE_NETWORK_LIMITED` and serves only
  the most recent blocks to peers; it does not help new nodes with their initial sync.
  The network's full (archival) nodes continue to do that. Older DigiByte peers
  (v9.26.2/v9.26.3/v8) understand this service bit and interoperate normally.
- **`getrawtransaction`** for a transaction in a deleted (pre-DigiDollar-era) block is
  unavailable without a transaction index, as on any pruned node. Use `-txindex` on an
  archival node for arbitrary historical lookups.
- **`getdigidollarstats`** still reports current supply / collateral / health / position
  counts on a pruned node via a live UTXO scan; the optional DigiDollar stats index
  (used only for historical per-height queries) is left off under `-prune`. That scan
  walks the whole UTXO set on each call, so avoid polling it at high frequency on a
  pruned node.
- **Wallets.** A wallet whose birthday predates the pruned history requires `-reindex`
  to rescan (standard pruned-node behavior). Wallets created on or after DigiDollar
  activation are entirely within the retained window and rescan normally.
- **`-reindex` on a pruned node redownloads from the network.** It deletes local block
  files and rebuilds by re-syncing, so it needs connectivity. Pools should keep one
  archival node (or a datadir snapshot) available for recovery.
- **Damaged block data fails closed.** If DigiDollar-era block data is unreadable at
  startup (for example a truncated or partially-restored block file), the node refuses
  to start and asks for `-reindex` rather than running DigiDollar validation on
  incomplete data.
- **Miners: restart after a long outage.** A freshly restarted node correctly holds
  `getblocktemplate` until it is back in sync. Checking that `getblockchaininfo` reports
  `headers` == `blocks` before dispatching work is a cheap safety habit.
- Setting `-prune` together with an explicit `-txindex=1`, or with
  `-reindex-chainstate`, is rejected at startup, as before. Dropping `-prune` on a
  datadir that was previously pruned requires `-reindex`, as in upstream Bitcoin Core.


Carried forward from v9.26.2 / v9.26.3 (unchanged)
--------------------------------------------------

- **Groestl algolock** (v9.26.2): retired-algorithm blocks are still rejected. The
  check reads only block headers and heights, so it behaves identically on pruned and
  full nodes.
- **DigiDollar BIP9 deployment** (bit 23): unchanged activation schedule, floor, oracle
  roster (35 slots, 7-of-35), and MuSig2 v0x03 bundle format.
- **Headers-sync fix and `-txindex` default** (v9.26.3): unchanged. On a full node
  `-txindex` still defaults on; `-prune` is what turns it off.


Testing and validation
=======================

This release was validated at every level before tagging:

- **Unit tests:** the full `test_digibyte` suite passes, including new coverage for the
  no-txindex predicate, the activation-floor collateral gate, and the fail-closed
  startup paths.
- **Functional tests:** the full `test_runner.py` suite passes, including the Groestl
  algolock boundary tests, the DigiDollar activation tests, and a new 15-phase
  `feature_digidollar_pruning.py` that runs a full node and a pruned (no-txindex) node
  side by side through activation, mining, mint/send/redeem, pruning, reorg, offline
  catch-up, reindex recovery, damaged-data refusal, and running an oracle.
- **Fuzzing:** new fuzz targets for the DigiDollar block-db amount extraction, the
  activation-floor computation, and pre-floor coin gating.
- **Live mainnet:** a pruned node built from a real mainnet datadir dropped from ~42 GB
  to ~0.21 GB; `pruneblockchain` was proven to clamp below the activation floor, keeping
  the floor block and deleting everything older, and restarts passed the startup guard.
- **Live testnet26** (where DigiDollar is active): a pruned node matched a full node's
  DigiDollar state to the cent, re-validated its entire redeem history under `-reindex`
  with no spurious rejects, recovered from damaged data via `-reindex`, served a wallet
  with real DigiDollar, and ran the real DigiSwarm oracle.


Credits
=======

Thanks to the mining pools and node operators who asked for a smaller DigiDollar node,
and to everyone who tested pruned-mode operation on live mainnet and testnet ahead of
DigiDollar activation.
