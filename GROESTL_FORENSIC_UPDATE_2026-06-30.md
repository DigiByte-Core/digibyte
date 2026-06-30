# Groestl Incident — Forensic Update (2026-06-30)

*Derived from a full block-by-block scan of a synced mainnet node, from the incident
onset (block 23,751,096) to block 23,766,084 (2026-06-30 21:09 UTC).*
*Companion to [`GROESTL_INCIDENT_AND_FIX.md`](GROESTL_INCIDENT_AND_FIX.md) and
[`GROESTL_INCIDENT_v9.26.2.md`](GROESTL_INCIDENT_v9.26.2.md).*

---

## Headline

**The original solo attacker stopped mining Groestl at 2026-06-30 04:21:05 UTC.**
Final tally: **1,514 Groestl blocks, 392,616.56 DGB.** They were displaced when legitimate
pools (led by **zpool**) turned on Groestl, driving its difficulty up **~5×** and ending the
cheap-mining window. **DigiHash** has since joined and mined its first Groestl block. The
retired algorithm is still being mined (now by pools, not the attacker), so block times
remain fast — the permanent fix is still the **v9.26.2 algolock at block 23,808,000**.

---

## Attacker totals (exact)

The "attacker" = direct solo mining to their own addresses (coinbase tag `SORG` and/or
payout to the two known addresses).

| Attacker payout address | Coinbase tag | Blocks | DGB |
|---|---|---:|---:|
| `dgb1qy5epvfs535a96tygn945a3a85lauh3ddu9v63y` | `SORG` | 922 | 239,093 |
| `D8S5JWaCrpFsryGG1c9AzWKhbS7e7VZ4r8` | (none) | 592 | 153,524 |
| **TOTAL** | | **1,514** | **392,616.56** |

- Average reward: **259.32 DGB/block** (block subsidy + fees).
- Active window: **2026-06-28 16:40:05 → 2026-06-30 04:21:05 UTC** (~35.7 hours).
- **No attacker Groestl block has appeared since h23,761,237 (04:21:05 UTC)** — confirmed
  across the ~4,850 blocks mined since.

> These final numbers supersede the in-progress figures in the earlier docs
> (~1,184–1,356 blocks / ~307k–351k DGB) — those were mid-incident snapshots. The attacker
> kept mining until 04:21 UTC on 06-30, reaching **1,514 blocks / 392,617 DGB**.

---

## Full Groestl breakdown since onset

Window: **14,983 blocks** (h23,751,096 → h23,766,084) · **2,323 Groestl blocks (15.5%)**.

| Miner | Groestl blocks | DGB | Active |
|---|---:|---:|---|
| **Attacker** (solo, `SORG` / `dgb1qy5e…` / `D8S5J…`) | 1,514 | 392,617 | onset → 06-30 04:21 UTC (**stopped**) |
| **zpool** (`/zpool.ca/`, pays `DC42uvyy…`) | 808 | 209,545 | 06-30 04:09 UTC → ongoing (**now dominant**) |
| **DigiHash** (`/DigiHashV2/`) | 1 | 259 | 06-30 21:02 UTC (**just joined**) |

---

## Timeline of the takeover

| Time (UTC) | Block | Event | Groestl difficulty |
|---|---|---|---|
| 2026-06-28 16:40:05 | 23,751,096 | Attacker reactivates Groestl (solo, floor difficulty) | 0.000244 (floor) |
| 2026-06-30 04:09:08 | 23,761,181 | **zpool turns on Groestl** | ~1,817 |
| 2026-06-30 04:21:05 | 23,761,237 | **Attacker's last Groestl block** (quits ~12 min after zpool ramps) | ~1,315 |
| 2026-06-30 21:02:26 | 23,766,054 | **DigiHash's first Groestl block** | ~4,170 |
| 2026-06-30 21:09:17 | 23,766,084 | Tip at time of this report | ~4,500 |

After 04:21 UTC, **zpool wins ~100% of Groestl blocks**; the attacker is absent.

---

## Difficulty and chain impact

- **Groestl difficulty rose from the floor (`0.000244`) to ~4,500** — roughly **5× the
  ~927 of the earlier docs**, and ~18-million× the onset floor. Mining Groestl is **no
  longer cheap.** This is the mechanism that ended the attacker's free-block window.
- **Block time is still ~12.5 s** (target 15 s). Because Groestl is *still being mined*
  (now by zpool), DigiByte is still effectively running a 6th algorithm, so blocks remain
  faster than target and emission is slightly accelerated. This persists until the algolock
  activates.

---

## What this means (and what it doesn't)

**Confirmed:**
- The attacker's **direct, cheap, solo mining is over.** They produced no Groestl blocks
  after 04:21 UTC on 06-30, and difficulty is ~5× higher.
- The defensive pressure **came overwhelmingly from zpool**, not DigiHash. zpool brought far
  more hashpower than the solo attacker had, which raised difficulty and crowded them out.
  DigiHash joined later and has won 1 block so far (proving the setup works on mainnet).

**Cannot be determined from the chain (stated honestly):**
- Whether the attacker **redirected their hashpower into zpool's public Groestl port**
  (mining Groestl *through* a pool instead of solo). The coinbase only shows zpool's address;
  it cannot reveal who is behind zpool's hashrate. What is certain: the attacker no longer
  gets cheap blocks at their own addresses, and the per-block cost (difficulty) is ~5× higher.

**Still true:**
- Pools mining Groestl is a **double-edged stopgap**: it captured the rewards away from the
  attacker's solo addresses and raised the cost, but it also **keeps the retired 6th
  algorithm alive** (fast blocks, extra emission) and adds more grandfathered Groestl blocks.
- **The permanent fix remains DigiByte Core v9.26.2.** Its `algolock` rule rejects *all*
  Groestl (and any retired/unknown algo) at block **23,808,000**, regardless of who mines it.
  Everyone must upgrade; once activated, Groestl mining ends network-wide and block times
  return to ~15 s.

---

## Forensic reference

| Item | Value |
|---|---|
| Onset block / time | 23,751,096 · 2026-06-28 16:40:05 UTC |
| Onset difficulty | 0.000244 (`1e0fffff` floor) |
| Attacker addresses | `dgb1qy5epvfs535a96tygn945a3a85lauh3ddu9v63y` (tag `SORG`), `D8S5JWaCrpFsryGG1c9AzWKhbS7e7VZ4r8` |
| Attacker total | **1,514 Groestl blocks · 392,616.56 DGB** |
| Attacker stop | 2026-06-30 04:21:05 UTC (block 23,761,237) |
| zpool address / tag | `DC42uvyywWHgK4yob7a8Yu4mnE5evYw8xs` · `/zpool.ca/` (legitimate pool, **not** the attacker) |
| zpool total (so far) | 808 Groestl blocks · 209,545 DGB |
| DigiHash tag | `/DigiHashV2/` · 1 Groestl block so far |
| Current Groestl difficulty | ~4,500 |
| Current block time | ~12.5 s (target 15 s) |
| Permanent fix | v9.26.2 algolock, activation height **23,808,000** |

*Exchanges/custodians: the two attacker addresses above are the ones to flag. `DC42uvyy…`
(zpool) and `/DigiHashV2/` are legitimate pools, not the attacker.*
