# HOUSEKEEPING TO-DOs

Audit date: 2026-06-25

Scope: formal v9.26.2 DigiDollar mainnet release readiness. This document now
tracks both the original audit findings and the housekeeping execution status.

## Confirmed In Code

- Version source is set to v9.26.2:
  `configure.ac` has major `9`, minor `26`, build `2`, release `true`, and no RC
  suffix.
- Normal public mainnet parameters are restored:
  `src/kernel/chainparams.cpp` uses magic `fa c3 b6 da` and P2P port `12024`;
  `src/chainparamsbase.cpp` uses mainnet data directory suffix `""`, RPC port
  `14022`, and onion target port `14122`.
- The PRE/mainnet rehearsal network settings are not active in source:
  the active source no longer uses `mainnet-pre`, `12046`, `14046`, or `14146`
  for mainnet.
- Historical DigiByte fork heights are still present in mainnet chainparams:
  DigiShield `67200`, MultiAlgo `145000`, MultiShield `400000`, DigiSpeed
  `1430000`, reserve algo bits `8547840`, algo swap `9100000`, Odocrypt
  `9112320`, and BIP34/BIP65/BIP66/CSV/SegWit at `4394880`.
- Mainnet DigiDollar activation is BIP9-gated:
  bit `23`, threshold `28224`, window `40320`, start time `1780272000`,
  timeout `1811808000`, and minimum activation height `23627520`.
- Mainnet DigiDollar, oracle validation, and MuSig2 are aligned:
  `nDDActivationHeight`, `nOracleActivationHeight`, and
  `nDigiDollarMuSig2Height` are all `23627520`.
- Mainnet oracle quorum is configured as 7-of-35:
  `nOraclePubkeyCount = 35`, `nOracleConsensusRequired = 7`, with 35 configured
  oracle pubkeys and 35 configured oracle node entries.
- The current oracle bundle validation path is shared by mainnet and testnet:
  a DigiDollar price-sensitive mint or redeem must have a valid v0x03 MuSig2
  oracle bundle after activation.
- Lock-tier policy is intentionally not a housekeeping item:
  the current build still contains the 1-hour and 10-year lock tiers, and this
  audit does not propose changing them.

Targeted validation already run during this audit:

- `./src/test/test_digibyte --run_test=musig2_activation_tests`
- `./src/test/test_digibyte --run_test=rh50_oracle_keyset_alignment_tests`
- `./src/test/test_digibyte --run_test=digidollar_activation_wave12_tests`
- `./src/test/test_digibyte --run_test=miner_dd_validation_tests`
- `./src/test/test_digibyte --run_test=rh65_mainnet_testnet_validator_parity_tests`

All five targeted test groups passed.

## 2026-06-26 Execution Update

Completed in this housekeeping pass:

- Commit `88b7ad81cd` (`release: prune stale mainnet seeders`)
  removed the four mainnet DNS seed domains that did not resolve from local and
  Cloudflare DNS checks:
  `seed.digibyteblockchain.org`, `eu.digibyteseed.com`,
  `seed.quakeguy.com`, and `seed.digibyte.services`.
- The retained mainnet DNS seed domains were rechecked and returned reachable
  sampled peers on TCP `12024`:
  `seed.digibyte.io`, `seed.diginode.tools`, `seed.digibyte.link`, and
  `seed.aroundtheblock.app`.
- `contrib/seeds/makeseeds.py` now accepts the v9.26.x user-agent family so a
  future fixed-seed refresh does not filter current v9 release nodes out.
- `src/chainparamsseeds.h` was regenerated from the current
  `contrib/seeds/nodes_main.txt` and `contrib/seeds/nodes_test.txt`; the
  generated output matched the checked-in file, so no fixed-seed byte change was
  needed.
- Commit `47b9ea3481` (`release: refresh mainnet chain metadata`) added a fresh
  block `23,500,000` mainnet checkpoint and updated release safety metadata.

Mainnet checkpoint data used from the local synced v9.26.2 Qt node:

- Height: `23,500,000`
- Block hash:
  `ade47d5ccbb92cb1d965b97a187bdbf65bf74be6a3709cb6a01339f8c2856deb`
- Chainwork:
  `0000000000000000000000000000000000000000001cae290ed41eb2efd4804c`
- Block time: `1778906996`
- Chain transaction count from `getchaintxstats` at that block: `53,488,713`
- Transaction rate from the 8192-block stats window:
  `0.09148870167189133`

The placeholder mainnet AssumeUTXO entry was removed because it had a zero
serialized UTXO hash and zero transaction count. v9.26.2 now ships no mainnet
AssumeUTXO snapshot until a real snapshot is generated.

Taproot live state was also checked with the running mainnet node:

- Taproot status: `active`
- Taproot since height: `21,168,000`
- DigiDollar status during the check: `started`, minimum activation height
  `23,627,520`

Validation completed after the code changes:

- `make -j$(nproc)`
- `python3 contrib/seeds/generate-seeds.py contrib/seeds` produced no diff
  against `src/chainparamsseeds.h`
- Focused unit tests:
  `oracle_config_tests`, `rh50_oracle_keyset_alignment_tests`,
  `digidollar_oracle_tests/chainparams_oracle_endpoint_uniqueness`, and
  `validation_tests`
- Full unit test suite:
  `./src/test/test_digibyte --show_progress` passed `3401` test cases
- Full functional suite:
  `test/functional/test_runner.py --jobs=8` passed all `378` listed entries
  with expected skips; runtime `398s`
- Fuzz smoke with an empty input file passed for:
  `dd_txbuilder_validate_mint_params`, `dd_txbuilder_mint`,
  `dd_txbuilder_redeem`, `oracle_bundle_v03_roundtrip`,
  `oracle_bundle_validation`, `oracle_musig2_bundle`,
  `oracle_p2p_wire_messages`, `oracle_validate_block_data`, and
  `fuzz_block_algo_routing_phase2a`
- Commit `07a63a9586` (`test: refresh regtest assumeutxo vector`) refreshed the
  regtest height-299 AssumeUTXO vector and updated `feature_assumeutxo.py` to
  match the deterministic DigiByte regtest snapshot it builds.
- Commit `1fec3a51f9` (`test: restore assumeutxo functional coverage`) restored
  `feature_assumeutxo.py` to the default functional runner.
- `test/functional/feature_assumeutxo.py` passed directly.
- `test/functional/test_runner.py feature_assumeutxo.py --jobs=1` passed.
- `feature_assumevalid.py` remains intentionally disabled. The current test
  buries the invalid block by 2100 blocks, which is Bitcoin's two-week-work
  assumption, but only hours of DigiByte-equivalent work at 15-second spacing.
  Porting it correctly is a separate test-maintenance task.

Oracle peer-seeding decision:

- The 35 oracle public keys, 35 active oracle slots, and 7-of-35 MuSig2 quorum
  must stay intact.
- The release does not need 35 live oracle DNS endpoints for consensus. The
  35-slot endpoint list is roster/status metadata; oracle security comes from
  the hardcoded public keys and MuSig2 bundle validation.
- For launch operations, oracle operators should use the two public v9.26.2
  mainnet oracle seed peers we have now:
  `oracle1.digibyte.io:12024` and `digihash.digibyte.io:12024`.
- A third, fourth, and fifth stable public seed peer would be useful for
  redundancy, but this is operational bootstrap redundancy, not consensus.

## Release Housekeeping Status

### 1. Remaining: Verify Mainnet Oracle Seed Peers

Code currently defines 35 active mainnet oracle slots and endpoint strings, all
using port `12024`. Those endpoint strings are not consensus trust anchors.

Local DNS verification found only these oracle hostnames resolving:

- `oracle1.digibyte.io`
- `digihash.digibyte.io`

Local TCP reachability on port `12024` during the re-check:

- `digihash.digibyte.io:12024` accepted a TCP connection.
- `oracle1.digibyte.io:12024` did not accept a TCP connection from this host.

The `oracle2.digidollar.org` through `oracle35.digidollar.org` style hostnames
did not resolve locally during this audit.

Important: these configured oracle endpoints are not the same thing as mainnet
DNS seeders. They are oracle roster metadata used by RPC/status display and key
alignment checks. Oracle message validity is controlled by the configured oracle
public keys and MuSig2 signature validation, not by trusting DNS hostnames.

Tasks:

- Confirm `oracle1.digibyte.io` and `digihash.digibyte.io` resolve from outside
  the local network.
- Confirm both public oracle seed peers accept inbound DigiByte P2P traffic on
  port `12024`.
- Add one to three more stable public v9.26.2 mainnet peers if community hosts
  volunteer them.
- Confirm the running oracle node for each slot uses the pubkey assigned to that
  slot.
- Run a mainnet smoke check after DNS is fixed:
  `listoracle`, `getoraclepubkey`, `getdigidollardeploymentinfo`, and one
  controlled oracle start/stop flow.

Why this matters:

The code-side 35-slot roster is internally consistent. Oracle operators still
need a few reliable public peers so their nodes can find each other and relay
oracle traffic cleanly through activation.

### 2. Completed: DNS Seed Cleanup And Fixed-Seed Check

Mainnet previously listed eight DNS seeds in `src/kernel/chainparams.cpp`.
Commit `88b7ad81cd` removed the four domains that did not resolve locally.
The formal v9.26.2 mainnet source now retains these four DNS seeds:

- `seed.digibyte.io`
- `seed.diginode.tools`
- `seed.digibyte.link`
- `seed.aroundtheblock.app`

Local TCP reachability on port `12024` during the re-check:

- Accepted `12024` in the sampled returned peers: `seed.digibyte.io`,
  `seed.diginode.tools`, `seed.digibyte.link`, `seed.aroundtheblock.app`

Current removal candidates because they did not resolve locally:

- `seed.digibyteblockchain.org`
- `eu.digibyteseed.com`
- `seed.quakeguy.com`
- `seed.digibyte.services`

Completed:

- The four non-resolving domains were removed from mainnet chainparams.
- The retained seed domains returned reachable sampled mainnet peers on TCP
  `12024` during the local audit.
- `contrib/seeds/makeseeds.py` now accepts v9.26.x user agents.
- Regenerating `src/chainparamsseeds.h` from the current seed files produced no
  diff, so the fixed-seed byte list did not need a source change.

Why this matters:

The release should not depend on stale seed infrastructure. New mainnet users
need reliable initial peers after installing v9.26.2.

### 3. Completed: Add Fresh Mainnet Checkpoint And Refresh Chain Safety Metadata

Commit `47b9ea3481` refreshed mainnet chain safety metadata from the synced
local v9.26.2 Qt node at block `23,500,000`.

Completed:

- Added checkpoint height `23,500,000`.
- Updated `defaultAssumeValid` to block `23,500,000`.
- Set `nMinimumChainWork` from that block's chainwork.
- Refreshed `chainTxData` from `getchaintxstats`.
- Removed the placeholder mainnet AssumeUTXO entry because it had zero values
  and was not a real release snapshot.

Why this matters:

This is normal release housekeeping, but it matters more for DigiDollar because
activation and oracle validation depend on nodes being cleanly synced on the
same mainnet chain.

### 4. Completed: Confirm Taproot Is Active On Live Mainnet

DigiDollar V1 uses P2TR vault scripts. The code has a separate Taproot BIP9
deployment and DigiDollar activation assumes Taproot script support is available
before DigiDollar activates.

Completed:

- The synced local mainnet v9.26.2 Qt node reported Taproot `active`.
- Taproot was active since height `21,168,000`.

Why this matters:

The code sets DigiDollar activation separately from Taproot. The release process
must confirm the live network state before asking wallets, exchanges, pools, and
oracles to prepare for DigiDollar activation.

### 5. Prepare Formal v9.26.2 Release Notes And User-Facing Docs

The source version is v9.26.2, but several public docs still read like RC,
testnet, or PRE rehearsal material.

Tasks:

- Add formal `doc/release-notes/RELEASE_v9.26.2.md` release notes.
- Update root `README.md` to explain that v9.26.2 is the formal DigiDollar
  mainnet activation release.
- Update `doc/release-notes.md`; it still contains stale DigiDollar language
  from older phases, including old oracle counts and old activation descriptions.
- Update `DIGIDOLLAR_ARCHITECTURE.md` and `DIGIDOLLAR_EXPLAINER.md` where they
  still describe validation as RC/testnet-focused instead of formal mainnet
  release-focused.
- Update oracle setup and integration guides so exchanges, wallets, pools, and
  oracle operators see v9.26.2 as the formal baseline.
- Clearly mark `digidollar/MAINNET_TEST.md` and PRE release notes as historical
  rehearsal documentation, not formal mainnet configuration.

Why this matters:

The code may be correct, but stale docs can cause operators to use the wrong
version, wrong network assumptions, wrong oracle count, or wrong activation
expectations.

### 6. Add A Release Version Regression Test

The build source says v9.26.2, but the current tests only check that version
output exists. They do not assert the exact formal release version.

Tasks:

- Add or update a focused test that verifies the exact v9.26.2 release string.
- Include the GUI splash/version text if the release image is part of the
  expected user-facing version surface.

Why this matters:

The formal release should not ship with mismatched binary version, GUI version,
release notes, or tag name.

### 7. Clean Up Or Register Stray Test Sources

Tracked test files exist that are not wired into the normal test build lists.
These should be intentionally registered or intentionally removed before the
formal release.

Unit-test files found tracked but not registered in the current test list:

- `src/test/bip324_tests.cpp`
- `src/test/limitedmap_tests.cpp`
- `src/test/rbf_tests.cpp`
- `src/wallet/test/accounting_tests.cpp`

Additional cleanup finding:

- `src/test/crypto_tests_backup.cpp` defines a duplicate `crypto_tests` suite
  name and appears to be a backup artifact, not an intended release test.

Fuzz files found tracked but not wired into the normal fuzz source list:

- `src/test/fuzz/crypto_chacha20_poly1305_aead.cpp`
- `src/test/fuzz/data_stream.cpp`
- `src/test/fuzz/odocrypt.cpp`

Tasks:

- Decide whether each file should be active release coverage or removed as
  stale source.
- Register intended tests in the appropriate makefile lists.
- Remove true backup/stale artifacts in a focused cleanup commit.
- Re-run unit, functional, and fuzz build/test gates after cleanup.

Why this matters:

Unregistered tests create false confidence. Backup test artifacts can also break
future test discovery or duplicate-suite cleanup.

### 8. Run Full Final Release Gates After Remaining Release Decisions

After the remaining oracle endpoint and release-document decisions are fixed,
run one final release validation pass.

Required gates:

- Full unit test suite.
- Full functional test suite:
  `test/functional/test_runner.py --jobs=8`
- DigiDollar/oracle-focused functional subset if the full suite fails for an
  unrelated environment reason.
- Fuzz target build and at least a short smoke run for DigiDollar/oracle/mining
  related targets.
- Clean build from a fresh checkout or clean build directory.
- GUI launch smoke check showing the v9.26.2 DigiDollar release branding.
- Mainnet startup smoke check with no PRE network parameters active.

Why this matters:

The formal DigiDollar release needs one clean final proof pass after metadata,
seed, docs, and version housekeeping are complete.

## Should Do Before Or Shortly After Release

### 9. Review Headerssync Parameters

`doc/release-process.md` calls out headerssync review as part of release
maintenance. `contrib/devtools/headerssync-params.py` should be checked against
current DigiByte mainnet assumptions before the formal release.

Tasks:

- Confirm the script is still used by this release process.
- If used, update parameters from current mainnet data.
- If not used, document that clearly so future release work does not treat stale
  values as active release policy.

### 10. Fix Stale Comments Around DigiDollar Format Gates

One RPC comment still describes V1 as setting the format gate to zero, while
current mainnet code sets MuSig2/DigiDollar activation at height `23627520`.

Tasks:

- Update stale comments in `src/rpc/digidollar.cpp` and nearby docs so they
  match the current v0x03 MuSig2-only release model.

### 11. Review No-libcurl Oracle Build Wording

The exchange-price code fails closed when libcurl support is unavailable.
Release documentation and build messaging should make that clear for oracle
operators.

Tasks:

- Confirm configure/build output describes oracle price fetching accurately.
- Document that production oracle operators need libcurl-enabled builds.
- Make clear that no-libcurl builds are not valid production oracle-signing
  builds.

### 12. Refresh Repo Maps After Final Housekeeping

The repo maps are useful for future agents and release audits, but they should
match the final v9.26.2 tree after housekeeping changes.

Tasks:

- Update `REPO_MAP.md`.
- Update `REPO_MAP_DIGIDOLLAR.md`.
- Remove or clearly mark obsolete PRE/testnet-only references.

## Not Current To-DOs

- Do not change the mainnet magic bytes.
- Do not change the normal mainnet ports.
- Do not change the normal mainnet data directory.
- Do not change historical DigiByte fork heights.
- Do not move DigiDollar, oracle validation, and MuSig2 to different mainnet
  heights.
- Do not remove the current 1-hour or 10-year lock tiers as part of this
  housekeeping pass.
- Do not keep PRE/mainnet rehearsal settings in the formal release.

## Summary

The formal v9.26.2 source tree now has the main PRE rehearsal reversion done,
normal mainnet network identity restored, stale DNS seeds pruned, fresh
mainnet chain safety metadata added, Taproot verified active, and DigiDollar,
oracle validation, and MuSig2 activation aligned.

The highest-risk remaining release item is operational oracle/bootstrap
connectivity: the 35 oracle public keys and 7-of-35 quorum are correct in code,
and the launch plan is to use DigiByte.io and DigiHash as public v9.26.2
mainnet oracle seed peers while asking for one to three more for redundancy.
Formal public docs are being handled by the separate docs team.

## Dev Chat Post Draft

Quick v9.26.2 DigiDollar housekeeping update before we do the formal mainnet
release.

The main PRE rehearsal changes have been reverted. The current source tree is
back on normal DigiByte mainnet identity: same mainnet magic bytes, same normal
mainnet ports, same normal mainnet data directory, and the historical DigiByte
fork heights are still intact. DigiDollar, oracle validation, and MuSig2 are
all aligned at the same mainnet activation height in chainparams.

Taproot is already active, and the release/docs team is handling the formal
public release notes and integration docs separately. The core housekeeping
work completed in this pass:

1. Fresh mainnet checkpoint and chain metadata are updated.
   We added block `23,500,000` as the current checkpoint, updated
   `defaultAssumeValid`, set real `nMinimumChainWork`, refreshed
   `chainTxData`, and removed the placeholder mainnet AssumeUTXO entry.

2. Mainnet DNS seeds are cleaned up.
   The seeds that resolved and returned reachable sampled mainnet peers were:
   `seed.digibyte.io`, `seed.diginode.tools`, `seed.digibyte.link`, and
   `seed.aroundtheblock.app`.

   The seed domains that did not resolve locally were removed:
   `seed.digibyteblockchain.org`, `eu.digibyteseed.com`,
   `seed.quakeguy.com`, and `seed.digibyte.services`.

   `src/chainparamsseeds.h` was regenerated from the current seed files and had
   no diff. The seed-generation user-agent filter now accepts v9.26.x nodes.

3. Regtest AssumeUTXO coverage is restored.
   The stale regtest AssumeUTXO vector was updated and `feature_assumeutxo.py`
   is back in the default functional runner.

Remaining item:

1. Verify oracle/bootstrap connectivity.
   Oracle hostnames are not consensus trust anchors. The actual oracle security
   comes from the hardcoded oracle public keys and 7-of-35 MuSig2 validation.
   The hostnames are only bootstrap/status/operator metadata.

   For the formal release we only need stable public bootstrap/connectivity for
   DigiHash and DigiByte.io style nodes, with one to three more useful but not
   consensus-required. Those seed peers should accept normal mainnet P2P on TCP
   `12024` before we put them in operator instructions.

2. Run final release gates after the remaining release decisions.
   Once oracle endpoint/docs decisions are finalized, we need the full unit
   suite, full functional suite with `test/functional/test_runner.py --jobs=8`,
   relevant fuzz build/smoke coverage, Qt launch/version verification, and a
   final mainnet startup smoke check.

The important part: nothing in this housekeeping changes DigiDollar economics,
mainnet identity, historical fork heights, or the DigiDollar/oracle/MuSig2
activation alignment. This is the final cleanup pass to make sure the formal
v9.26.2 DigiDollar release starts from clean mainnet settings and reliable
network bootstrap data.
