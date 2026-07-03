# The Bit-23 Math, Explained Simply

*Why `version & ~0x00800000` removes only the DigiDollar signal — and cannot touch anything else.*

---

## 1. A block version is just 32 on/off switches

Every DigiByte block header carries a **version**: a 32-bit number, usually written in hex
(like `0x20800202`). Think of it as **32 light switches in a row**, numbered 31 down to 0.
Different groups of switches mean different things:

```
bit:   31 30 29 | 28 ......... 13 | 12 | 11 10 9 8 | 7..3 | 2 | 1 | 0
       ---------+----------------+----+------------+------+---+---+---
role:  "001"    | ASIC version-  |free| ALGORITHM  | free |tap|base|algo-
       BIP9     | rolling window |    | ID nibble  |      |root|ver|lock
       marker   | (bits 13–28)   |    | (bits 8–11)|      |   |   |(bit 0)
                | ← bit 23 lives |    |            |      |   |   |
                |   in HERE      |    |            |      |   |   |
```

* **Bits 31–29 = `001`** — tells every node "this version uses BIP9 signaling."
* **Bit 23** — the **DigiDollar vote**. On = "my node is ready for DigiDollar."
* **Bits 11–8** — the **algorithm ID** (which of the 5 mining algos made this block).
* **Bit 1** — the base version (always on).
* **Bit 0** — the **algolock vote** (starts at block 23,788,800).

## 2. Decoding the real SHA256D job version: `0x20800202`

This is exactly what a v9.26.4 node hands a SHA256D pool (verified live via
`getblocktemplate`, and byte-for-byte the job version in the issue #419 NerdAxe logs).
Written out in binary:

```
hex:      2    0    8    0    0    2    0    2
binary: 0010 0000 1000 0000 0000 0010 0000 0010
bit #:  31─28│27─24│23─20│19─16│15─12│11─8 │7─4 │3─0
```

Now pick out the ON switches:

| Switch | Value | Meaning |
|---|---|---|
| bits 31–29 = `001` | `0x20000000` | BIP9 marker — "I signal with version bits" |
| **bit 23 = 1** | **`0x00800000`** | **DigiDollar vote: YES** |
| bit 9 = 1 (nibble `0010` = 2) | `0x00000200` | Algorithm ID **2 = SHA256D** |
| bit 1 = 1 | `0x00000002` | Base version |

Add them up: `0x20000000 + 0x00800000 + 0x00000200 + 0x00000002 = 0x20800202`. ✔

## 3. It checks out on every algorithm

Same decoding, straight from the live node and from 1,200 real mainnet blocks — only the
**algorithm nibble** (bits 11–8) changes:

| Algo | Nibble (bits 11–8) | Template version | = marker + bit 23 + nibble + base |
|---|---|---|---|
| Scrypt | `0x0` | `0x20800002` | `0x20000000 + 0x00800000 + 0x000 + 0x2` |
| SHA256D | `0x2` | `0x20800202` | `… + 0x200 + 0x2` |
| Skein | `0x6` | `0x20800602` | `… + 0x600 + 0x2` |
| Qubit | `0x8` | `0x20800802` | `… + 0x800 + 0x2` |
| Odo | `0xE` | `0x20800e02` | `… + 0xE00 + 0x2` |

## 4. What `& ~0x00800000` actually does

`0x00800000` is a number with **exactly one switch on: bit 23**:

```
0x00800000 = 0000 0000 1000 0000 0000 0000 0000 0000
                       ↑
                     bit 23 only
```

The `~` flips it (every switch ON **except** bit 23), and `&` keeps only switches that are
on in **both** numbers. Net effect: **bit 23 is forced OFF; all 31 other bits pass through
completely untouched.**

```
  0x20800202   = 0010 0000 1000 0000 0000 0010 0000 0010   (SHA256D job, voting yes)
& ~0x00800000  = 1111 1111 0111 1111 1111 1111 1111 1111   (everything except bit 23)
  ───────────────────────────────────────────────────────
  0x20000202   = 0010 0000 0000 0000 0000 0010 0000 0010   (same job, vote removed)
```

Check the pieces after the operation: BIP9 marker `001` — still there. Algorithm nibble
`0x2` (SHA256D) — still there. Base bit — still there. **Only the DigiDollar vote is gone.**
It is mathematically impossible for this operation to change the algorithm, because the
algorithm lives in bits 11–8 and the mask only touches bit 23 — different switches entirely.

## 5. Why bit 23 matters (the actual problem)

SHA256D ASICs use bits **13–28** as scratch space ("version rolling" / AsicBoost — mask
`0x1fffe000`). **Bit 23 sits inside that scratch space:**

```
rolling window:  bits 28 ←──────────────────── 13
                  1111 1111 1111 1111 ×  (0x1fffe000)
DigiDollar bit:            bit 23 = inside ⚠
algo nibble:     bits 11–8  = OUTSIDE ✔ (chips can never corrupt the algo)
algolock bit 0:  OUTSIDE ✔ (always trustworthy)
```

Old (v8) jobs had all zeros in that window; v9 puts a 1 there (bit 23), which is what
confuses older miner firmware and pool share-checks. Clearing bit 23 from the job simply
restores the v8 shape — and since BIP9 only ever **counts** bit 23 (it is never required
for a block to be valid), removing it can never get a block rejected.

## 6. Check it yourself

```bash
# Ask your node for a SHA256D template (algo is the SECOND argument):
digibyte-cli getblocktemplate '{"rules":["segwit"]}' sha256d | grep '"version"'
#  → 545260034  ( = 0x20800202 )

# Decode any version number:
python3 -c "v=0x20800202; print(f'marker={v>>29:03b} bit23={(v>>23)&1} algo={hex((v>>8)&0xF)} base_bit1={(v>>1)&1}')"
#  → marker=001 bit23=1 algo=0x2 base_bit1=1

# And after the mask:
python3 -c "v=0x20800202 & ~0x00800000; print(hex(v), f'bit23={(v>>23)&1}', f'algo={hex((v>>8)&0xF)}')"
#  → 0x20000202 bit23=0 algo=0x2   ← vote off, algorithm untouched
```

*Companion docs: `BIT_ISSUE.md` (full root-cause analysis) and
`POOL_BIT23_HOTFIX_GUIDE.md` (deployment checklist for pools).*
