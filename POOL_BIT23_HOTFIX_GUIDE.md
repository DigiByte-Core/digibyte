# DigiByte v9.26.x — Pool & Miner Hotfix Guide (bit-23 / version-rolling)

**Audience:** SHA256D pool operators and Bitaxe/NerdAxe-class miner users.
**TL;DR:** DigiByte v9.26.x sets BIP9 bit 23 (DigiDollar) inside the ASIC version-rolling
window (bits 13–28). Older firmware and some pool code assume that window is all zeros in the
job version — that assumption broke, causing share rejects (MiningCore) or silent ~45%
hashrate loss (NerdAxe/NerdQAxe). **The fix is one line at the pool: clear bit 23 from the
job version.** It is 100% consensus-safe — a signaling bit is never required for a block to
be valid.

---

## Who is affected

| Setup | Symptom |
|---|---|
| MiningCore + NerdQAxe/Bitaxe (BM1366/68/70) | ~100% `low difficulty share` rejects (issue #419) |
| OR-merge pools + NerdQAxe/Bitaxe | Clean accepts but credited hashrate ≈ 55% of chip speed |
| S19-class ASICs on some pool stacks | Reject storms until mask/merge fixed |
| Scrypt / Skein / Qubit / Odo miners | **Not affected** (no version rolling on those algos) |
| Node operators (non-mining) | **Not affected** — the node is behaving correctly |

## The one-line pool fix

Where your stratum builds the job from `getblocktemplate`, clear bit 23 before sending work
to SHA256D version-rolling clients:

```
job_version = gbt_version & ~0x00800000;   // clear bit 23 (DigiDollar BIP9 signal)
```

This restores the exact v8 job shape (all-zero rolling region), which every firmware and pool
merge convention handles correctly. Keep it in place permanently — it stays safe through the
BIP9 STARTED, LOCKED_IN and ACTIVE states.

**Why it's safe:** DigiByte consensus only *counts* bit 23 for BIP9 statistics
(`versionbits.cpp` `Condition()`); no validation rule ever requires it in a block. A block
with bit 23 cleared can never be orphaned or rejected for that reason.

**Why you don't lose the signal entirely:** BM13xx chips *replace* bits 13–28 with an internal
counter, so ~50% of blocks found by rolling miners will still carry bit 23 and still count
toward DigiDollar activation. Non-rolling algos are unaffected and signal deterministically.

### MiningCore — make the rolling mask per-coin first

MiningCore's `VersionRollingPoolMask` is a **global constant**, so changing it for DGB also
changes BTC/BCH/etc. on the same instance (field-tested in issue #419: DGB recovered, every
other SHA256 pool started rejecting). Patch `ConfigureVersionRolling` to read the mask from
the pool's config (default `0x1fffe000`), then pick ONE flavor for the DGB pool only:

**Flavor A — narrow mask, keeps full deterministic signaling (recommended if your DGB miners
honor masks — S19-class, current Bitaxe):**

```
"versionRollingMask": "1f7fe000"   // DGB pool only: bit 23 excluded from rolling
```

The job keeps bit 23 = 1 and MiningCore's rebuild takes it from the job, so every block
signals DigiDollar. Field-verified working (issue #419 comment, accepted shares at D=8192).
Caveat: mask-ignoring firmware (NerdQAxe, see #640) will hit "rolling-version mask
violation" until it's fixed — those users need flavor B or a firmware update.

**Flavor B — strip bit 23 from the job, maximum device compatibility:**

```csharp
// DGB pool only, where the job takes the template version:
version = BlockTemplate.Version & ~0x00800000u;
```

Restores exact v8 job semantics for every device including broken NerdQAxe firmware; blocks
then signal ~50% of the time (the chip's replace-roll re-inserts bit 23 randomly).

**⚠ Flavor B deployment checklist — a bare strip has failed in the field when these were
missed (one pool saw no NerdAxe recovery plus a duplicate-share storm and had to roll back):**

1. **Grant the FULL mask `0x1fffe000` with the strip.** Never combine the strip with a
   narrowed mask (e.g. a leftover `1f7fe000` from an earlier experiment). The chip rolls
   bit 23 regardless; with the job bit cleared those rolls are *legitimate* and must be
   inside the granted mask, or firmware clamps/violates and you're back to ~50% loss.
2. **Strip in exactly one place, atomically.** The stripped version must be what goes out in
   `mining.notify` AND what the stored job object uses for share validation AND what the
   block header serializer uses. If any path still sees `0x20800202`, the same work
   circulates in two variants — identical merkle root and nonce space — and a version-blind
   duplicate check flags the second copy: that is the duplicate-share storm.
3. **Include version_bits in the duplicate-share key.** Older MiningCore-era dedupe keys are
   (extranonce2, ntime, nonce) only; once the strip unblocks the previously-dropped ~50% of
   BM13xx shares, submission volume roughly doubles and version-blind dedupe starts
   rejecting valid shares as duplicates. (Long-standing issue: oliverw/miningcore#1709.)
4. **Roll out on a fresh job with `clean_jobs = true`** so no miner keeps working a
   pre-strip job whose shares validate against the wrong version.

If you cannot verify all four on your stack, prefer Flavor A (mask-honoring fleets) or wait
for the node-side fix below — that is precisely why it exists.

Do **not** narrow the global mask (breaks other coins: mask-ignoring NerdQAxe units violate
it everywhere, and Bitaxe collapses a holey mask into a smaller contiguous roll count and
starts producing duplicate shares). Do **not** disable version rolling: Bitaxe-class devices
depend on it for their entire hashrate (a BM1370 exhausts the 32-bit nonce space in under a
millisecond).

### Coming in DigiByte Core: no pool changes at all

A v9.26.5-candidate patch makes GBT stop force-setting bit 23 on **SHA256D templates** for
clients that don't declare the `digidollar` rule (3 lines in `rpc/mining.cpp`; the standard
`gbt_force` opt-in mechanism, scoped to the one algo where the bit is unusable). Once your
node runs it, stock MiningCore gets v8-clean SHA256D jobs automatically; Scrypt/Skein/
Qubit/Odo templates keep signaling automatically; pools that want deterministic SHA256D
signaling opt in via GBT `"rules": ["digidollar"]` or Flavor A above.

### ckpool / NOMP / yiimp / other stratums

Same concept: mask the version taken from GBT when constructing the job / `mining.notify`
params for the DGB sha256d port. One line wherever `template.version` is read.

### Verifying the fix

1. Reject rate for NerdQAxe/Bitaxe workers drops to ~0 immediately.
2. Credited hashrate returns to chip spec (e.g. NerdQAxe++ ≈ 11.3–11.4 TH/s).
3. `digibyte-cli getblockheader <hash>` on your found blocks shows version
   `0x20xxx202`-style values — with or without bit 23; both are valid.

## Firmware users (Bitaxe / NerdAxe / NerdQAxe)

Short term: mine on a pool that applies the fix above.
If you build firmware from source, the root-cause fix is one line in the result path —
reconstruct with a masked merge instead of OR:

```c
// components/asic/bm1370.c (and bm1366.c): was  job->version | (bits << 13)
uint32_t rolled_version = (job->version & ~0x1fffe000) | (version_bits << 13);
```

NerdQAxe additionally needs the granted mask actually written to the ASIC (issue #640).
Upstream refs: bitaxeorg/ESP-Miner#1169, shufps/ESP-Miner-NerdQAxePlus#640.

## While you're in there (all pool operators)

* **Upgrade your Scrypt/Skein/Qubit/Odo pool nodes to v9.26.4.** Those algos don't roll
  versions, so your blocks signal DigiDollar deterministically the moment the node is
  upgraded. Scrypt is currently the biggest missing share of signaling on mainnet.
* **Close any Myriad-Groestl port.** Groestl blocks are rejected unconditionally from height
  **23,808,000** (~days away) and until then they dilute the DigiDollar activation window.
* **Watch bit 0 (algolock)** from block **23,788,800**: it is outside the rolling window, so
  it is the clean "this pool runs v9.26.x" indicator on every algo. Live tracking:
  https://digibyte.io/pool-upgrades and https://digibyte.io/activation

Full root-cause analysis with source citations: `BIT_ISSUE.md` in this repo.
