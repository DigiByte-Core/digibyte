# DigiByte: The Groestl Mining Incident and the v9.26.2 Fix

*A plain-language explainer for the DigiByte community, miners, pools, and exchanges.*

**Status: Everyone must upgrade to v9.26.2.**

---

## The short version

Someone used **AI to analyze DigiByte's consensus code** and discovered they could
still mine blocks with **Groestl** — one of DigiByte's **old, retired mining
algorithms** — because of a **safety check that went missing during the v8 code rebase
in 2021/2022**.

DigiByte is supposed to run **5 mining algorithms**. Starting **2026-06-28 at 16:40:05
UTC (block 23,751,096)**, it was effectively running a **6th** (Groestl) that was retired
back in 2019 and should no longer be accepted.

This did **not** steal anyone's coins and did **not** reverse confirmed transactions.
But it destabilized the network and caused it to **split**: some software accepted the
Groestl blocks and some rejected them, so different nodes and explorers ended up on
different versions of the chain.

The fix is **v9.26.2**. It permanently stops new Groestl (and any other retired/unknown
algorithm) blocks, while keeping existing chain history intact so nobody's transactions
are reversed. **Everyone needs to upgrade to v9.26.2** for the network to heal back into
one chain.

---

## What actually happened

### Background: DigiByte's 5 algorithms
DigiByte is secured by **five** mining algorithms at once (SHA256d, Scrypt, Skein,
Qubit, and Odocrypt). Using several algorithms spreads out mining power and makes the
chain harder to attack. A sixth algorithm, **Groestl**, was **retired in 2019** when
Odocrypt replaced it.

### The hidden gap (a missing check from the 2021/2022 v8 rebase)
When Groestl was retired in 2019, the rule that said *"don't accept Groestl blocks
anymore"* was enforced by the software of that era (v7.17.3). That old client checked,
for every block, that the mining algorithm was still active — and rejected blocks from
retired algorithms.

During the major **v8 rebase in 2021/2022** (when DigiByte was rebuilt on a newer
Bitcoin Core base), the validation code was heavily rewritten and **that one enforcement
check was accidentally left out**. The function that *knew* Groestl was retired still
existed and was still used for difficulty and display — but the single line that
*enforced it when accepting a new block* was gone.

For years nobody noticed, because nobody was mining Groestl. Its mining difficulty
quietly fell to the lowest possible setting.

### The discovery and the incident
Someone — using AI to scan DigiByte's consensus rules for weak spots — found this dormant
gap and switched Groestl back on. Because its difficulty had collapsed, they could produce
Groestl blocks very cheaply.

**The exact moment the network split:**
- **Last shared block (fork point):** height **23,751,095**, mined 2026-06-28 16:40:01 UTC.
- **First Groestl block (the split):** height **23,751,096**, mined **2026-06-28 16:40:05
  UTC** (block hash `b2749cf0462eb6c35bf5f8f1d75f4891e62d3ca1d9861fac48b46c05ba078283`).

From that block onward, software that accepted Groestl (the v8/v9 line) and software that
rejected it (the old v7.17.3 line) were building two different chains. Over the roughly
**32 hours** since, the attacker has mined a large share of all blocks on this retired 6th
algorithm.

**Stated honestly:**
- **No coins were stolen.** The attacker only collected normal block rewards for the
  blocks they mined. No user funds were taken.
- **No confirmed transactions were reversed.** The honest chain always had more total
  work, so no deep rewrite of history succeeded. The biggest disturbance was a brief
  4-block reorganization at the very start — within normal limits.
- **The real damage was instability and a split.** Blocks came noticeably faster than
  the 15-second target, and the network divided: software that still accepted Groestl
  stayed on one chain, while software that rejected it (including very old v7.17.3 nodes
  and some block explorers) forked onto a separate, slower chain.

### Was this a 51% attack?

**There is no evidence of a successful 51% attack — but one was very likely attempted.**

Mining a retired algorithm at rock-bottom difficulty produces blocks extremely cheaply,
and cheap blocks are exactly the raw material an attacker would use to try to out-build
the honest chain and rewrite recent history (a "51% attack"). So the capability was being
assembled. But the records show it did not succeed:

- **No competing chain ever had more total work than the honest chain.** Across **106**
  alternative branches the network has seen, **zero** ever accumulated more work — so
  nodes never switched away from the honest chain.
- **The deepest the main chain ever reorganized was 4 blocks**, right at the start. That
  is within normal range for a 15-second blockchain and only ever affects transactions
  with very few confirmations.
- **The largest alternative chain seen was 16 blocks of headers** — and it was **rejected**
  because the honest chain out-worked it.
- **No double-spends and no deep rewrite of history occurred.**

In plain terms: the attacker had the ingredients to attempt a 51%-style rewrite and the
behavior is consistent with an attempt, but DigiByte's multi-algorithm design kept the
honest chain ahead at all times, so nothing was reversed.

### Why blocks sped up to ~12-13 seconds

DigiByte aims for **one block every 15 seconds**, balanced across its **5** algorithms.
The difficulty system assumes there are 5. When a **6th** algorithm (Groestl) is forced
on, it adds an extra stream of blocks the system never planned for — so blocks start
arriving **faster than 15 seconds.**

- Block times dropped to roughly **12-13 seconds on average (about 15% faster than
  target)**, and as fast as **~11-12 seconds** during the heaviest Groestl bursts.
- A side effect: while this lasts, new DGB is emitted **slightly faster** than the
  intended schedule.
- This speed-up is itself **direct evidence of the extra 6th algorithm** — it is the
  visible symptom of the problem.

When v9.26.2 removes Groestl at activation, the chain returns to 5 algorithms and block
times settle back to ~15 seconds.

### Forensic details

For transparency, here is what the chain records show (as of 2026-06-30, ongoing):

- **Onset / split block:** height **23,751,096**, 2026-06-28 16:40:05 UTC.
- **Scale so far:** about **1,356 Groestl blocks** — roughly **14.7%** of all blocks since
  the onset — earning approximately **351,000 DGB** in block rewards (259.31 DGB per
  block). The mining is **still ongoing.**
- **Difficulty:** started at the **lowest possible setting** (the reason it was cheap to
  start), and has since **risen to ~927** as the attacker kept mining — so it is **no
  longer free**; they are now spending real mining power to continue.
- **Block "signature":** the Groestl blocks carry a distinctive version marker — early
  blocks used a bare legacy form (`0x400`), later blocks the standard form (`0x20000402`)
  — both identifying the block as Groestl.
- **Payout addresses (the attacker switched setups partway through):**
  - `dgb1qy5epvfs535a96tygn945a3a85lauh3ddu9v63y` — **922 blocks**, 2026-06-28 16:40 →
    2026-06-29 17:42 UTC. These blocks carry a custom coinbase tag, **"SORG"** (a private
    label the miner wrote into their own blocks; it matches no known pool or software).
  - `D8S5JWaCrpFsryGG1c9AzWKhbS7e7VZ4r8` — **407+ blocks**, 2026-06-29 17:44 UTC →
    ongoing.
- The separate, slower **Groestl-rejecting chain** (what some explorers show) is mined by
  a **different** address and is **not** the attacker — it is the minority side of the
  split that rejected Groestl.

Exchanges and custodians should flag/withhold deposits originating from the two attacker
payout addresses above until the network has healed.

---

## Why this matters even though no funds were stolen

A blockchain only works if everyone agrees on **one** history. This incident caused
different participants to disagree:

- Most miners and nodes (on the v8 line) stayed on the chain **with** the Groestl blocks.
- Some nodes and explorers (on the 7-year-old v7.17.3 line) rejected Groestl and ended up
  on a **separate** chain that fell behind.

Multiple Exchanges **froze DigiByte deposits and withdrawals** after the split, so users were not
put at direct risk — but the network needs to be reunited into a single agreed chain
before normal activity can safely resume.

---

## The fix: v9.26.2

v9.26.2 restores the protection that was lost in the 2021/2022 rebase, the right way.

### What it does
1. **Stops new retired-algorithm blocks.** From the activation point onward, any block
   using Groestl — or any other retired or unknown algorithm — is **rejected**. This also
   closes the door on an attacker simply switching to a different unused algorithm slot.

2. **Keeps existing history intact (grandfathering).** The Groestl blocks already buried
   in the chain are **kept**, not erased. This is deliberate: removing them would mean
   reversing a full day of everyone's legitimate transactions. We keep the existing chain
   and only block **new** retired-algorithm blocks going forward. **No transactions are
   reversed.**

3. **Enforces the rule everywhere.** The check now runs both when a block first arrives
   **and** when blocks are replayed or reindexed — so a node can't accidentally carry a
   bad block forward after upgrading. This restores the exact protection the 2019 software
   had, in modern code.

4. **Activates on a clear schedule with a built-in upgrade tracker.**
   - There is a **~7-day window** before the rule switches on, giving miners and pools
     time to upgrade safely.
   - A **signaling flag** lets miners advertise that they've upgraded, so everyone can
     watch adoption climb in real time (via `getdeploymentinfo`).
   - The rule then activates automatically at the scheduled block height.

### Why the 7-day window
A consensus change can't be rushed. Pools need time to install and safely deploy the new
software without disrupting their operations. The 7-day window lets the majority of mining
power upgrade first, so that when the rule activates, the upgraded majority is already
enforcing it — and the network converges cleanly onto one chain instead of splitting
further.

---

## What everyone needs to do: upgrade to v9.26.2

### Miners and pools — **most important**
- **Upgrade to v9.26.2 as soon as it is available.** The **majority of mining power is
  currently on the v8 line** — that is fine; you simply need to move to v9.26.2.
- You are the ones who make the fix work: once enough mining power is upgraded, the
  upgraded chain becomes the strongest chain, the Groestl blocks stop, and the network
  heals into one chain.

**Optional — actively pushing back during the 7-day window.** Until the fix activates,
Groestl blocks are still valid (they are being grandfathered). Miners who want to blunt
the attacker can **point hash power at Groestl themselves** during this window. Doing so:
- **takes block rewards away from the attacker** (you win Groestl blocks instead of them), and
- **drives Groestl's difficulty up**, removing the cheap-mining advantage that made the
  exploit attractive.

This is a **voluntary tactic, secondary to upgrading.** Be aware it keeps block times fast
while it lasts, and all such blocks are grandfathered; once v9.26.2 activates, Groestl is
rejected entirely regardless. **Upgrading to v9.26.2 is the action that actually fixes the
network — competing on Groestl only weakens the attacker in the meantime.**

### Exchanges and custodians
- **Upgrade your nodes to v9.26.2.**
- Keep deposits/withdrawals paused until your node is upgraded and the chain has
  stabilized past the activation point, then resume with high confirmation counts.

### Node operators and businesses
- **Upgrade to v9.26.2.** Your node keeps the existing chain history and simply begins
  rejecting new retired-algorithm blocks at activation.

### Anyone still running very old software (v7.17.3, ~7 years old)
If you are still running a **7-year-old v7.17.3 node**, you need to upgrade for **several
reasons**, not just this one:
- **Taproot** (Schnorr signatures / BIP340-342) — added since then.
- **DigiDollar** — DigiByte's native stablecoin features.
- **This consensus-check fix** — and the fact that the healed chain keeps the
  grandfathered Groestl blocks, which old v7.17.3 software rejects.

Because the healed chain keeps that grandfathered history, a stock v7.17.3 node cannot
follow it on its own. **Upgrade to v9.26.2 and reindex (or resync)** so your node accepts
the existing history and reorganizes onto the correct, unified chain. You will also gain
Taproot, DigiDollar, and every other improvement from the last seven years.

---

## How the network heals

1. v9.26.2 is released.
2. Miners and pools upgrade during the ~7-day window (watch adoption via the signaling
   flag).
3. At the activation height, upgraded nodes reject **new** Groestl blocks. Because the
   majority of mining power is upgraded, the Groestl-free chain becomes the strongest
   chain.
4. Nodes that were on the separate (Groestl-rejecting) chain upgrade, accept the existing
   history, and **reorganize back onto the main chain.**
5. The two chains become **one** again. Exchanges can safely resume.

---

## Honest summary

- **What happened:** someone used AI to find a safety check that went missing in
  DigiByte's 2021/2022 v8 rebase, and used it to switch the retired Groestl algorithm
  back on — briefly turning DigiByte into a 6-algorithm chain and splitting the network.
- **What was NOT affected:** no coins were stolen, and no confirmed transactions were
  reversed.
- **What the fix does:** v9.26.2 permanently blocks retired/unknown algorithms going
  forward, keeps all existing history, and reunites the network on a clear ~7-day
  schedule.
- **What you must do:** **upgrade to v9.26.2.** Miners and pools first and foremost — the
  fix only works when the mining majority runs it.

DigiByte's multi-algorithm design did its job: even with a 6th algorithm forced on, the
honest chain was never deeply rewritten and no funds were lost. v9.26.2 closes the gap
for good.
