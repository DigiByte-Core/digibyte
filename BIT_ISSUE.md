# BIT_ISSUE.md — The DigiDollar Bit-23 / Version-Rolling Collision

**Date:** 2026-07-03 · **Status:** Root cause confirmed at every layer (consensus, pool, firmware, silicon) with live mainnet data and source citations.

---

## Executive summary

DigiByte's DigiDollar BIP9 deployment signals on **version bit 23**, which sits **inside the
BIP310/BIP320 version-rolling window (bits 13–28, mask `0x1fffe000`)** that SHA256D ASICs use for
overt AsicBoost. BIP320 says explicitly: *"future soft forks SHOULD NOT utilise those bits for
activation signalling."* Bitcoin never has (CSV bit 0, SegWit bit 1, Taproot bit 2 — Taproot's
proposal explicitly chose bit 2 because it is *"unaffected by ongoing miner ASICBoost false-bit-spam"*).

Because v8 nodes produced job versions whose rolling region was **all zero**, and v9.26.x sets
bit 23 = 1 inside it, three long-latent bugs detonated at once:

1. **NerdQAxe / Bitaxe-class miners (BM1366/68/70) lose ~50% of their work silently** — the
   ~55%-of-chip-speed credited hashrate observed in A/B testing.
2. **MiningCore pools reject 100% of NerdQAxe-class shares** as `low difficulty share`
   ([DigiByte-Core/digibyte#419](https://github.com/DigiByte-Core/digibyte/issues/419)).
3. **Even healthy setups silently un-signal:** on-chain, version-rolled SHA256D blocks carry
   bit 23 exactly **53%** of the time — a coin flip, not a signal.

**Separately**, the revived Groestl algorithm has re-entered the chain as a full sixth lane
(~16% of blocks) and dilutes the BIP9 window until the backstop kills it at height **23,808,000**.
There is **no bit-level conflict** between Groestl and bit 23 — the interaction is purely
statistical window dilution.

**Bottom line (the good news):** activation on bit 23 is still achievable **with zero consensus
changes and zero risk to SHA256D mining**, because SHA256D is only ~20% of blocks and the
threshold is 70%. The immediate fixes are pool-side (one line) and firmware-side (one line),
plus getting **Scrypt pools upgraded** — Scrypt, at 1.5% signaling, is the actual binding
constraint, not SHA256D.

---

## 1. DigiByte block-version anatomy (v9.26.x)

```
bit 31..28   27    26..24   23        22..13        12    11..8    7..3   2        1        0
─────────  ─────  ──────  ────────  ─────────────  ────  ───────  ─────  ───────  ───────  ────────
top bits   test-  (free)  DIGI-     BIP310/320     free  ALGO     free   taproot  BLOCK_   ALGOLOCK
001x       dummy          DOLLAR    ASIC rolling         NIBBLE          (buried) VERSION  (bit 0)
(0xF mask)                (bit 23)  ◄─── bits 13–28 = 0x1fffe000 ───►             DEFAULT
```

* Algo nibble (bits 8–11, `primitives/block.h:30-44`): Scrypt `0x0`, SHA256D `0x2`,
  Groestl `0x4` (retired), Skein `0x6`, Qubit `0x8`, Odo `0xE`.
* Template version assembled at `node/miner.cpp:496` via `ComputeBlockVersion`
  (`versionbits.cpp:231-249`): `0x20000000 | 0x2 | deployment-masks | algo-nibble`
  → a SHA256D template today is **`0x20800202`**.
* **DigiByte quirk:** `VERSIONBITS_TOP_MASK = 0xF0000000` (`versionbits.h:18`) — stricter than
  Bitcoin's `0xE0000000`. A block whose rolled **bit 28** landed on 1 signals **nothing** —
  ~2% of all blocks are currently voided this way.
* Signaling bits are **never a validity requirement**. `Condition()` (`versionbits.cpp:191-194`)
  is a raw bit test used only for counting: no algo filter, no rolling awareness. A rolled block
  that happens to carry bit 23 **is counted**; one that dropped it is not. Stripping bit 23 from
  a job can never orphan a block.

### Mainnet deployments (`kernel/chainparams.cpp`)

| Deployment | Bit | Start | Window / threshold | Notes |
|---|---|---|---|---|
| taproot | 2 | active/buried since 21,168,000 | — | outside rolling range ✔ |
| **digidollar** | **23** | 2026-06-01 → 2027-06-01 | 40,320 / 28,224 (70%) | **inside rolling range ✖**; min_activation_height 23,627,520 |
| **algolock** | **0** | 2026-06-29 → 2027-06-29 | 40,320 / 28,224 (70%) | outside rolling range ✔; window opens at block **23,788,800** |
| testdummy | 27 | never active | — | inside rolling range (moot) |

Groestl/unknown-algo rejection is dual-gated (`validation.cpp:4820-4826`, duplicated at
`2852-2868`): rejected once **algolock is BIP9-ACTIVE _or_ height ≥ 23,808,000**
(`nGroestlDeactivationHeight`, `chainparams.cpp:127`) — whichever comes first. Until then,
Groestl blocks are consensus-valid (grandfathered).

---

## 2. Live mainnet evidence (1,200 blocks ending 23,784,934, ~4.2 h)

Measured with the consensus mask `0xF0000000`:

| Algo | Share of blocks | Signals bit 23 (as consensus counts) | Version-rolled |
|---|---|---|---|
| Odo | 16.8% | **100%** | 0% |
| Skein | 16.6% | **100%** | 0% |
| Qubit | 16.2% | 95% | 0% |
| **Scrypt** | 17.1% | **1.5%** | 0% |
| **SHA256D** | 17.0% | **44%** | **97%** |
| **Groestl (revived)** | 16.2% | 38% | 0% |

* **Network signal rate: 62.8% vs the 70% threshold.** The node reports the current window
  (started 23,748,480) at 10,808 / 36,377 elapsed with **`possible: false`** — this window
  cannot lock in; the count resets at **23,788,800**.
* Rolled SHA256D blocks carry bit 23 **53.3%** of the time (105/197) — the coin-flip signature.
* Blocks are arriving every **12.6 s** instead of 15 s (Groestl added a sixth lane).
* Groestl versions split **120× `0x20000402`** (v8-style node, not signaling — zpool.ca tags)
  vs **75× `0x20800402`** (an upgraded v9.26.x node's own template — DigiHashV2 tags:
  **DigiHash itself still has a myr-gr port open** and is feeding the dilution with blocks that
  die at the backstop).

---

## 3. Failure mechanics, layer by layer

### 3.1 The silicon (BM1366 / BM1368 / BM1370)

The chip rolls a **contiguous 16-bit counter over header bits 13–28** and **replaces** that
field. The driver interface cannot express a mask with a hole:
`BM1370_set_version_mask`: `int versions_to_roll = version_mask >> 13;` → register `0xA4`
(ESP-Miner `components/asic/bm1370.c:133-140`, same in `bm1366.c:141-147`). Independent proof of
replace-semantics: Litecoin Cash's POW_TYPE bits 16–23 get swept to arbitrary values by these
chips regardless of the granted mask (shufps/ESP-Miner-NerdQAxePlus#640).

### 3.2 The firmware (ESP-Miner / Bitaxe / NerdQAxe)

* Sends the **full job version including bit 23** to the chip: `memcpy(&job.version,
  &next_bm_job->version, 4);` (`bm1370.c:339`) — no masking.
* Reconstructs with a **bitwise OR** that assumes the rolling region was zero:
  `rolled_version = job->version | (version_bits << 13);` (`bm1370.c:388-399`).
* Locally validates shares against the OR-reconstructed version and **silently drops** failures
  (`main/tasks/asic_result_task.c:68-78`).
* **NerdQAxe additionally never writes the pool-granted mask to the ASIC at all** — init
  hardcodes `0xFFFF` roll five times (`components/bm1397/bm1370.cpp`), confirmed by the
  maintainer in issue #640 ("seems it's never set in the ASICs", still unfixed). **This is why
  the narrow-mask A/B test could not restore NerdAxe hashrate.**

Consequence with bit 23 = 1 in the job:
* Chip results whose counter left header-bit 23 **clear** (~50%): chip hashed bit 23 = 0,
  firmware reconstructs 1 → garbage hash → **silently dropped**. → *the ~55% credited hashrate*.
* Chip results whose counter left bit 23 **set** (~50%): reconstruct correctly, but the firmware
  submits `version_delta = rolled XOR job` — bit 23 XORs to **0**.

### 3.3 The pool (MiningCore)

MiningCore grants the full mask (`VersionRollingPoolMask = 0x1fffe000`) and rebuilds the header
with the BIP310 **replace** merge: `version = (version & ~mask) | (versionBits & mask)`
(`BitcoinJob.cs:293`). Fed the XOR-delta with bit 23 = 0, it reconstructs a header the chip never
hashed → **every submitted share fails the hash check** → `low difficulty share (2.8e-10)` —
issue #419's exact symptom, and why v8.x pools "work". Net on MiningCore: 50% dropped in
firmware + 50% rejected at the pool = **100% loss**. On OR-semantics pools: the surviving 50%
is accepted cleanly = the 55% yield case.

**Also:** MiningCore's correct replace-merge means even *healthy* S19-class rolled shares get
bit 23 taken from the miner's rolled bits — mined blocks signal ~50/50. That is the coin flip
visible on-chain; it is a *measurement* loss, not a mining loss.

### 3.4 The docs made it worse

`DIGIDOLLAR_ACTIVATION_EXPLAINER.md` contains **zero mention** of version rolling, BIP310/320,
stratum, or ASIC compatibility; no rationale for choosing bit 23; a worked version example
(`0x20800004`) that matches no real block (omits the algo nibble and bit 1); and the claim
that GBT miners "signal automatically — no configuration needed," which is false for
stratum-served SHA256D. `gbt_force=true` (`deploymentinfo.cpp:14-30`) means pools can't opt out
via GBT rules, and **LOCKED_IN force-sets bit 23 into every template** — maximizing firmware
breakage exactly when activation succeeds, unless pools strip the bit (safe — see §5).

---

## 4. Groestl: attack or opportunism, and does it block DigiDollar?

* **No bit conflict.** Groestl is algo-nibble `0x0400` (bits 8–11); version rolling can't touch
  the nibble (`GetAlgo` exact-matches it), and bit 23 is unrelated.
* **The real interaction is window dilution.** BIP9 counts *all* accepted blocks. Groestl's
  ~16% lane is ~62% non-signaling (the zpool-tagged v8-style blocks), pushing the network rate
  down. Combined with Scrypt (~0%) and the SHA256D coin flip, that is what makes this window
  impossible — as the node itself reports (`possible: false`).
* **Attack or profit?** Chain data cannot prove intent. Simple economics fully explains it:
  the retired algo's difficulty collapsed to near-zero, the v8 rebase accidentally dropped the
  rejection, and pools (zpool.ca — and DigiHashV2) still had myr-gr ports. No modified client is
  needed to *mine* it; the 75 upgraded-node-version blocks show a stock v9.26.x node happily
  templates Groestl pre-enforcement.
* **It is time-bounded.** Backstop at **23,808,000** (~4 days). Note the backstop lands
  *mid-window* (window 23,788,800–23,829,119 contains up to 19,200 groestl-able blocks), so the
  **first fully clean window starts at 23,829,120**. BIP9 algolock cannot fire earlier than the
  backstop (earliest lock-in evaluation is the 23,829,120 boundary), so the height backstop is
  what actually kills Groestl.

---

## 5. The solution — fix signaling without breaking SHA256D, fully v9.26.x-compatible

**Keep the deployment on bit 23. No consensus change. Fix the job layer, and let the algo math
carry activation.** A bit-move (v9.26.5 redeployment) is *not* required and is the only option
that risks old/new client divergence — see §6.

The key enabling facts:

* A signaling bit is **never required for block validity** — stripping bit 23 from a stratum job
  can never orphan a block or violate consensus (`versionbits.cpp:191`, `validation.cpp:4861-4863`).
* SHA256D is only ~20% of blocks (17% today). The 70% threshold is reachable **without a single
  SHA256D signal** once Groestl dies and Scrypt upgrades:

| Scenario (post-backstop, 5 algos ≈ 20% each) | Expected signal rate | ≥ 70%? |
|---|---|---|
| Today's behavior (Scrypt dark, SHA256D coin flip) | ~68–70% | knife-edge ✖ |
| **+ Scrypt pools upgraded** | **~87%** | ✔ comfortably |
| + Scrypt upgraded, SHA256D fully stripped (0%) | ~78% | ✔ |
| + firmware fixed (SHA256D ~50–100%) | 88–97% | ✔ |

### Action list (ordered)

1. **Pool-side hotfix — deploy today, per-coin, zero consensus impact.**
   MiningCore's rolling mask is a global constant, so first make it per-pool config
   (field confirmation in issue #419: narrowing it globally fixed DGB but broke BTC/BCH on
   the same instance). Then, for the DGB sha256d pool only, either:
   * **narrow mask `0x1f7fe000`** — job keeps bit 23; mask-honoring firmware mines at full
     speed AND signals deterministically (field-verified: accepted shares at D=8192); or
   * **strip bit 23 from the job** (`job.version &= ~0x00800000`) — exact v8 semantics,
     works for every device including mask-ignoring NerdQAxe firmware; blocks still signal
     ~50% (the chip's replace-roll re-inserts the bit randomly). **Field caveat:** a bare
     strip has failed on a live pool (no NerdAxe recovery + duplicate-share storm). It must
     be paired with the FULL `0x1fffe000` mask (never a leftover narrowed mask), applied
     atomically to notify + validation + header serialization so only one variant of each
     job exists, combined with a duplicate-share key that includes version_bits
     (miningcore#1709), and rolled out with `clean_jobs=true`. Checklist in
     `POOL_BIT23_HOTFIX_GUIDE.md`.
   Both are consensus-safe through STARTED, LOCKED_IN and ACTIVE — the bit is never a
   validity rule — but the deployment pitfalls above are why the node-side fix (1b) is the
   preferred systemic answer.

1b. **Node-side systemic fix (v9.26.5 candidate, ~3 lines, no consensus change).**
   GBT already has the standard opt-in mechanism (strip a deployment's bit for clients that
   don't declare its rule) — it's just disabled by `gbt_force=true`. Scope it to the broken
   algo in `rpc/mining.cpp` (version-bits loop):
   ```cpp
   if (!vbinfo.gbt_force ||
       (pos == Consensus::DEPLOYMENT_DIGIDOLLAR && pblock->GetAlgo() == ALGO_SHA256D)) {
       pblock->nVersion &= ~chainman.m_versionbitscache.Mask(consensusParams, pos);
   }
   ```
   Effect: SHA256D templates ship v8-clean versions by default — zero pool changes after a
   node upgrade — while all other algos keep signaling automatically. SHA256D signaling
   remains available three ways: GBT `"rules": ["digidollar"]` opt-in, the per-coin narrow
   mask (deterministic), or the free ~50% chip coin flip. Fully compatible with deployed
   v9.26.2/3/4 nodes (which simply keep force-setting the bit until upgraded).

2. **Get Scrypt pools upgraded — this is the actual activation blocker.**
   Mining-Dutch and the other Scrypt operators are at **1.5%** signaling. Scrypt rigs do not
   version-roll (0% rolled observed), so their bit 23 is deterministic the moment the pool's
   node is v9.26.x. Target: before window **23,829,120** (the first Groestl-free window).

3. **DigiHash: close the Groestl port now.** It is mining blocks that become invalid in ~4 days
   and diluting DigiByte's own activation window (75 of the last 195 groestl blocks).

4. **Upstream firmware PRs (parallel; fixes the ecosystem, not just DGB):**
   * ESP-Miner: replace the OR-merge with the mask-correct merge —
     `rolled_version = (job->version & ~0x1fffe000) | (version_bits << 13);`
     (`bm1370.c:388-399`, `bm1366.c` equivalent) — and submit BIP310 value-bits rather than a
     job-XOR so pools reconstruct exactly what the chip hashed.
   * NerdQAxe: additionally fix issue #640 (granted mask never programmed into the ASIC).
   * With fixed firmware, pools can stop stripping bit 23 and SHA256D signaling returns.

5. **Algolock bit 0 becomes the clean upgrade metric at block 23,788,800 (hours away).**
   Bit 0 is outside the rolling window — every v9.26.x node signals it deterministically on
   every algo, ASICs cannot corrupt it. Use it (not bit 23) to measure *who has upgraded*;
   digibyte.io/pool-upgrades has been updated accordingly (raw-vs-clean bit 23, rolling
   called out, bit 0 tracking, consensus `0xF0000000` mask, pool-identity and dedup fixes).

6. **Documentation:** add a "version rolling / BIP320" section to
   `DIGIDOLLAR_ACTIVATION_EXPLAINER.md`, fix the worked version example, document algolock, and
   adopt a rule for all future deployments: **signal bits must come from {3, 4, 5, 6, 7, 12}**
   (outside BIP320 bits 13–28, outside the algo nibble 8–11, not 0/1/2, not 27/28; bits 3–4 are
   also safe under draft BIP323, which reserves 5–28). Verified free in consensus code by grep.

### Fallback if signaling still stalls

If the clean windows (23,829,120+) still miss 70% because Scrypt operators won't move, ship a
**v9.26.5 with a height-based activation** (the same mechanism as the Groestl backstop, and the
same coordinated-mandatory-upgrade playbook the network just executed for v9.26.2). That
requires a new release for everyone but keeps one unambiguous rule-set — unlike a bit-move.

---

## 6. Why NOT to move the deployment off bit 23 (for this deployment)

Moving to a safe bit is only a 4-line `kernel/chainparams.cpp` change (lines 178-181 mainnet,
plus the other three networks) and nothing else reads bit 23 (verified: no consensus, wallet, or
oracle code depends on it). **But it is the one option that is *not* "compatible with all other
v9.26.x clients":**

* Deployed v9.26.2/3/4 nodes track bit 23; patched nodes would track the new bit. BIP9 state is
  then computed **independently per cohort**, and with mixed miners both cohorts can reach
  different states at different heights (or one activates and the other times out in June 2027).
  DigiDollar's consensus rules would switch on at different heights for different nodes — a
  rule-divergence window and a support nightmare.
* And it isn't needed: the table in §5 shows bit 23 activates fine without SHA256D.

Reserve the safe bits (3, 4) for **future** deployments, and use a coordinated new deployment
only in the fallback case above — where the release is mandatory anyway, so the cohort problem
collapses.

---

## 7. Verification / monitoring

* **Bench (NerdAxe):** flash the one-line firmware merge fix; A/B against a bit-23-set job —
  credited hashrate should jump ~55% → ~100% with zero pool rejects. Alternatively, point stock
  firmware at a pool that strips bit 23: same result (that A/B already exists — the v8 pool).
* **Pool:** after stripping bit 23 from sha256d jobs, MiningCore reject rate for
  NerdQAxe workers drops to ~0 immediately (issue #419 reproducer).
* **Chain, per window:** `digibyte-cli getdeploymentinfo` → `deployments.digidollar.bip9.statistics`
  ({count, elapsed, threshold, possible}) and `deployments.algolock.bip9.status` (flips to
  `started` at 23,788,800). Groestl share: count `pow_algo=="groestl"` headers — must go to zero
  at 23,808,000.
* **Dashboards:** digibyte.io/pool-upgrades (per-pool bit 23 raw/clean + bit 0),
  digibyte.io/activation (window progress vs 28,224 threshold, `possible` flag, reset countdown).

## Appendix: primary sources

* DigiByte: `versionbits.h:14-20` (TOP_MASK `0xF0000000`), `versionbits.cpp:191-198,225-249`,
  `node/miner.cpp:493-501`, `kernel/chainparams.cpp:115-116,166-188` (+127 backstop),
  `validation.cpp:2204-2231,2732-2753,2852-2868,4820-4826`, `primitives/block.h:30-74`,
  `deploymentinfo.cpp:14-30`, `rpc/mining.cpp:1042-1115`.
* BIPs: [BIP310](https://github.com/bitcoin/bips/blob/master/bip-0310.mediawiki) (mask
  negotiation), [BIP320](https://github.com/bitcoin/bips/blob/master/bip-0320.mediawiki)
  (bits 13–28 reserved; "SHOULD NOT utilise ... for activation signalling"), draft BIP323.
* Firmware: bitaxeorg/ESP-Miner `components/asic/bm1370.c:133-140,339,388-399`,
  `main/tasks/asic_result_task.c:68-78`, `components/stratum/include/utils.h:38`; ESP-Miner
  issue #1169, PRs #1713/#1740 (closed unmerged); shufps/ESP-Miner-NerdQAxePlus issue #640.
* Pool: oliverw/miningcore `BitcoinJob.cs` (`VersionRollingPoolMask = 0x1fffe000`, replace-merge,
  mask-violation check); MiningCore issue #1709.
* Reports: DigiByte-Core/digibyte issue #419; NerdAxe sandbox A/B (~55% credited at
  `0x20800202`); Litecoin Cash POW_TYPE precedent (FenixPool docs, NerdQAxe #640).
* Chain data: 1,200-block mainnet sample ending 23,784,934 (this report, §2);
  `getdeploymentinfo` snapshot: digidollar `started`, 10,808/36,377, `possible: false`;
  algolock `defined` until 23,788,800.
