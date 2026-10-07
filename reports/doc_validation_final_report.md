# DigiByte v9.26.2 — Root-Markdown Release Validation: Final Report

Run date: 2026-06-25 · Branch: `feature/digidollar-v1` · Driver: `doc/v9.26.2/Z_DOC_VALIDATION_v9.26.2_PROMPT.md`

Archive note: Findings and work status below describe the June 2026 review. File references have been updated for the documentation archive; this report does not certify the current release.

## Verdict: **READY WITH 3 OPEN ITEMS FOR JARED**

All 21 Tier A/B root `.md` docs (incl. `doc/release-notes/RELEASE_v9.26.2.md`) are validated against
current source, cross-doc consistent, and free of stale roster figures, RC labels,
and Bitcoin-branding errors. Three items require Jared's decision (no code-derivable
fix; nothing was invented). Nothing was committed.

## Method
- **Wave 0:** 5 scouts code-derived the canonical-facts ledger (`reports/doc_validation_canonical_facts.md`).
- **Waves 1–5:** validate→fix→adversarially-verify pipeline over 20 docs (each agent owned one file, applied only minimal source-cited edits; an independent read-only agent on a different model re-checked every edit). +`doc/release-notes/RELEASE_v9.26.2.md` validated separately (created mid-run).
- **Consistency sweep + orchestrator reconciliation:** spot-checked critical numbers against code, fixed the one flagged bad edit, de-RC'd stale labels, bumped version headers, resolved cross-doc drift.
- Totals: 47 sub-agents, ~3.5M tokens, ~2,270 tool calls.

## Per-doc status
| Doc | Verdict | Notes |
|---|---|---|
| doc/release-notes/RELEASE_v9.26.2.md | VERIFIED (clean) | every claim matches code; ports/roster/activation/RPC confirmed |
| DIGIDOLLAR_WALLET_INTEGRATION.md | VERIFIED | RPC names/params confirmed |
| DIGIDOLLAR_EXCHANGE_INTEGRATION.md | VERIFIED | units/prefixes confirmed |
| DIGIDOLLAR_MINING_INTEGRATION_GUIDE.md | VERIFIED | GBT rule + miner path confirmed |
| ORACLE_DISCOVERY_ARCHITECTURE.md | VERIFIED | P2P getoracles confirmed |
| REPO_MAP_DIGIDOLLAR.md | VERIFIED | RPC count 18 + de-RC applied |
| DIGIDOLLAR_ORACLE_ARCHITECTURE.md | VERIFIED (reconciled) | duplicate heading fixed (5.1/5.4); de-RC; version header |
| DIGIDOLLAR_ARCHITECTURE.md | VERIFIED (reconciled) | version header → v9.26.2 |
| DIGIDOLLAR_ACTIVATION_EXPLAINER.md | VERIFIED | activation values confirmed |
| DIGIDOLLAR_EXPLAINER.md | VERIFIED | last-verified date synced |
| DIGIDOLLAR_ORACLE_EXPLAINER.md | VERIFIED | RC44 label removed from subtitle |
| DIGIDOLLAR_ORACLE_SETUP.md | VERIFIED | de-RC; roster confirmed |
| ORACLE_BUNDLE_EXPLAINER.md | VERIFIED | de-RC; 9-of-17 kept as history |
| ARCHITECTURE.md | VERIFIED | core constants confirmed |
| CLAUDE.md | VERIFIED | surface warnings confirmed; v9.26 lineage refs kept |
| REPO_MAP.md / REPO_MAP_GUIDE.md | VERIFIED | titles → v9.26.2 |
| README.md | VERIFIED w/ open item | see #2 |
| CONTRIBUTING.md | VERIFIED w/ open item | see #3 |
| SECURITY.md | VERIFIED w/ open item | see #1 |
| INSTALL.md | VERIFIED | trailing newline fixed |

## Corrections applied (orchestrator reconciliation, on top of agent edits)
- Fixed bad edit: duplicate "ConnectBlock() Integration" heading → `5.1 ConnectBlock() Oracle Validation` / `5.4 ConnectBlock() Price-Cache Update` (`DIGIDOLLAR_ORACLE_ARCHITECTURE.md`); both confirmed against `src/validation.cpp:2852`.
- De-RC sweep (current keys are "RC46" in code; final release): removed present-tense `RC44` labels in ORACLE_BUNDLE_EXPLAINER, DIGIDOLLAR_ORACLE_SETUP (×2), DIGIDOLLAR_ORACLE_ARCHITECTURE (×2), REPO_MAP_DIGIDOLLAR (×3), DIGIDOLLAR_ORACLE_EXPLAINER subtitle. Historical `RC38`/`RC41` references kept.
- Version headers/titles → v9.26.2: REPO_MAP, REPO_MAP_DIGIDOLLAR, REPO_MAP_GUIDE, DIGIDOLLAR_ARCHITECTURE, DIGIDOLLAR_ORACLE_ARCHITECTURE.
- Minor: INSTALL.md trailing newline; DIGIDOLLAR_EXPLAINER last-verified date 2026-06-25.

## Cross-doc consistency: PASS
Shared facts (v9.26.2; 7-of-35 / 4-of-7; bit 23 + heights; opcodes 0xbb–0xbf;
ports 12024/12033; HRPs dgb/dgbt/dgbrt; prefixes DD/TD/RD; 18+17 RPCs) are
consistent across all docs and equal the code value. No `21 active` / `9-of-17`
present-tense figures remain (only correct historical mentions).

## DigiByte/DGB correctness: PASS
No Bitcoin/BTC branding, unit, or port errors. Remaining "Bitcoin Core" mentions
are accurate lineage/coding-standard references; "40x faster than BTC" and
"DGB/BTC × BTC/USDT" are correct comparisons / real exchange pairs.

## OPEN ITEMS FOR JARED (not code-derivable — nothing invented)
1. **SECURITY.md PGP table** lists Bitcoin Core devs (Wuille, Ford, Chow), not
   DigiByte security key holders. Needs the real DigiByte disclosure keys —
   cannot be synthesized from code.
2. **README.md circulating supply** "14,293,304,147 DGB (May 2021)" is ~5 years
   stale (pre-existing). Needs a current figure from an external data source.
3. **CONTRIBUTING.md** references a `master` branch (lines 80, 169, 403) and a
   `github.com/digibyte/digibyte/.../master/...` URL; repo default is `develop`
   and README uses `github.com/DigiByte-Core/digibyte`. Inherited Bitcoin Core
   workflow text — confirm DigiByte's actual contributor branch/URL before fixing.

## CODE_DISCREPANCY_FOR_JARED
None. No doc was found to be correct against incorrect code; no code was edited.

## Human-check (out of code scope)
- External URL `https://digibyte.io/activation` (RELEASE notes) — confirm live.
- No `v9.26.2` git tag exists yet (latest: `v9.26.0-rc46`, `v9.26.1-pre2`) —
  expected for a not-yet-tagged release; tag at release time.

## Commit status
Uncommitted — left in working tree for review (`git diff -- '*.md'`). Proposed
split: (a) version/title bumps to v9.26.2; (b) oracle de-RC label cleanup;
(c) DIGIDOLLAR_ORACLE_ARCHITECTURE heading fix; (d) minor (INSTALL newline, dates).
