# DigiByte Groestl Reactivation — What Happened, in Plain Terms

**Incident window:** June 28–29, 2026 (ongoing at time of writing) · **Fix:** DigiByte Core **v9.26.2**

*All figures below were re-derived directly from a fully-synced mainnet node and
its complete debug log.*

---

## In one paragraph

Someone reactivated one of DigiByte's old, **retired** mining algorithms —
**Groestl** — and started using it to mine blocks at almost zero cost. DigiByte is
supposed to run **5** algorithms; for this window it was effectively running a
**6th**. The attacker had cheap "work," which is the raw material for trying to
rewrite recent history (a **51% attack**). They could **not** pull that off:
**no competing chain ever built up more total work than the honest chain**, so
there was **no sustained takeover and no deep rewrite of history.** There was real
turbulence right at the start (the chain briefly reorganized up to **4 blocks
deep**), but DigiByte's multi-algorithm design kept the honest chain ahead. The
gap that let the old algorithm back in is closed in **v9.26.2**.

---

## What actually happened

- DigiByte mines with **5 algorithms**: SHA256d, Scrypt, Skein, Qubit, Odocrypt.
- A 6th, **Groestl**, was **retired in 2019** at the Odocrypt upgrade
  (block **9,112,320**).
- "Retired" was honored by miners and noted in helper code — but DigiByte's
  **block-acceptance rules never actually rejected Groestl blocks.** They stayed
  technically valid, just unused, for about **7 years**.
- With nobody mining it, Groestl's difficulty drifted to the **absolute minimum**.
  That made a Groestl block **nearly free to produce.**
- On **June 28, 2026 at 16:40 UTC (block 23,751,096)** a miner switched Groestl
  back on.

### By the numbers (as of block 23,759,257)
- **~1,184 Groestl blocks** mined — about **14.5%** of all blocks in the window.
- Roughly **307,000 DGB** collected in those (nearly free) block rewards.
- **Still ongoing** at the time of writing.
- **Two payout addresses, used one after the other:**
  - **Phase 1** — `dgb1qy5epvfs535a96tygn945a3a85lauh3ddu9v63y`, every block tagged
    **`SORG`**: ~922 blocks (Jun 28 16:40 → Jun 29 17:42).
  - **Phase 2** — `D8S5JWaCrpFsryGG1c9AzWKhbS7e7VZ4r8`, no tag: ~248 blocks
    (Jun 29 17:44 → ongoing).
  - The clean switch with no overlap points to the **same actor rotating its
    address/configuration** (or a direct handoff).

### What is "SORG"?
`SORG` is simply a **label the miner wrote into its own blocks** (the "coinbase
tag" — a free-text field miners use to sign their work). It is **not** a known
mining pool, coin, or software; a public search turns up nothing, so it is a
**private/vanity tag** chosen by this miner. It only appears in Phase 1; the
Phase 2 address dropped it. (Read backwards, `SORG` is `GROS` — possibly a nod to
**GRO**estl, but that is just a guess.)

### Who, and how they likely found it
Finding this required noticing that an algorithm **retired 7 years and ~14 million
blocks ago** was still technically accepted by the network — a subtle gap buried in
the consensus code that no ordinary observer would spot. That depth of analysis is
**consistent with an attacker using advanced AI to scan DigiByte's consensus rules
for latent, exploitable weaknesses.** The reactivation itself, the choice of a
block version that slips past other checks, and the floor-difficulty mining all
point to a sophisticated, deliberate actor rather than a misconfiguration.

---

## The 51% question — what was attempted, and why it failed

A 51% attack means building a **heavier alternative chain** and using it to erase or
replace recent transactions. Free Groestl blocks gave the attacker cheap work, so
the ingredients were there. Here is exactly what the chain shows:

- **No competing chain ever out-worked the honest chain.** Of the **103**
  alternative branches the node has seen, **zero** ever accumulated more total work
  — so the network never switched away from the honest chain. This is the decisive
  proof that **no sustained takeover happened.**
- **There was real turbulence at the very start.** As the exploit began, the chain
  reorganized up to **4 blocks deep** (around block 23,751,083). A 4-block
  reorganization can only affect transactions with **fewer than ~4 confirmations**;
  exchanges and large transfers wait for many more, so well-confirmed funds were
  never at risk.
- **No deep or lasting rewrite of history occurred**, and the honest chain stayed
  the most-work chain at all times.

In short: the attacker had the means to try, caused a brief disturbance, but
**could not build a chain that beat the honest network's combined work.**

---

## Why DigiByte held: the multi-algorithm shield

DigiByte spreads mining across 5 algorithms, and a chain's "weight" is the
**combined work of all of them**. To rewrite history an attacker must out-work the
**entire honest network across every algorithm at once** — not just dominate one
cheap algorithm. One free algorithm is nowhere near enough. The episode was an
unplanned, real-world stress test of that design, and the design held.

---

## The fix: v9.26.2

- Block acceptance now **rejects any block that uses a retired or otherwise invalid
  mining algorithm** (`bad-algo`). It closes **every** unused algorithm slot, not
  just Groestl, so the attacker cannot simply hop to another one.
- The rule activates together with the upcoming **DigiDollar hard fork**, so it
  switches on network-wide on one coordinated date.
- **Existing blocks are grandfathered** (they sit below the activation point) →
  **no chain split.**
- It is locked in by automated tests that reproduce the exact exploit and confirm
  it is now blocked.

---

## Bottom line

- A retired algorithm was reactivated; DigiByte briefly ran 6 algorithms instead of 5.
- An attempt to leverage that into a 51% rewrite **failed** — no competing chain
  ever out-worked the honest chain, and the deepest disturbance was a 4-block reorg
  at the onset.
- **No well-confirmed funds were lost and no lasting history was rewritten.**
- **v9.26.2 closes the gap for good** — but it only takes effect once the network
  (pools, exchanges, node operators) upgrades to it.

---

## Technical appendix

- **Root cause:** `IsAlgoActive()` correctly marks Groestl inactive after Odocrypt,
  but it was never consulted on the block-acceptance path
  (`ContextualCheckBlockHeader`). The fix adds a `bad-algo` rejection there, gated
  on `DEPLOYMENT_DIGIDOLLAR`.
- **Exploit signature:** block version `0x00000400` (Groestl), difficulty at the
  floor `1e0fffff`, onset block **23,751,096** (2026-06-28 16:40:05 UTC).
- **Payout addresses:** `dgb1qy5epvfs535a96tygn945a3a85lauh3ddu9v63y` (tag `SORG`,
  ~922 blocks) then `D8S5JWaCrpFsryGG1c9AzWKhbS7e7VZ4r8` (~248 blocks).
- **Scale:** ~1,184 Groestl blocks / ~307,000 DGB across ~8,160 blocks (~14.5%),
  ongoing.
- **No-takeover proof:** 0 of 103 competing branches ever exceeded the honest
  chain's cumulative work; deepest actual reorg = 4 blocks (at onset).
- **Regression tests:** `test/functional/feature_digibyte_groestl_deactivation.py`
  (fails without the fix → passes with it) and `pow_tests/digibyte_isalgoactive_matrix`.
