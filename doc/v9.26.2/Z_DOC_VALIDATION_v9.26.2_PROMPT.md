# v9.26.2 — Final Root-Markdown Release Validation Prompt

**Archive note:** This prompt records the v9.26.2 documentation review. Its root-directory scope, branch, and release settings describe that historical review. File references now point to their current locations. This archived copy is retained in version control. Repository file paths below are relative to the repository root.

## Goal

Validate that **every Markdown (`.md`) file in the repository root directory** is
accurate, current, internally consistent, and ready to ship with the formal,
full, final **DigiByte Core v9.26.2 / DigiDollar v1** release.

"Ready to ship" means each root `.md` file is proven, against live source on the
current `feature/digidollar-v1` tree, to:

1. **Match the code.** Every factual, technical, numeric, or behavioral claim is
   backed by current source (`src/`), build files, chainparams, tests, scripts,
   or RPC/Qt registration — not by memory, not by an older release note, not by
   a stale sibling doc.
2. **Reflect DigiByte and DGB correctly.** The docs describe DigiByte (DGB), the
   DigiByte chain values, DigiByte ports/addresses/magic, and DigiDollar — not
   Bitcoin (BTC), Bitcoin defaults, or Bitcoin Core lineage values left over
   from the upstream fork. Inherited Bitcoin terminology, BTC units, Bitcoin
   block times, Bitcoin ports, Bitcoin RPC examples, or "Bitcoin Core" branding
   that should read "DigiByte Core" are defects.
3. **Read clearly for both AI agents and humans.** Each doc is well-structured,
   unambiguous, navigable, free of contradictions, free of dead/broken internal
   references, and easy for an AI agent or a human reader (developer, exchange,
   miner, oracle operator, or end user) to understand and act on.
4. **State the correct version and release status.** Where a doc references a
   version, it agrees with the source of truth: `configure.ac`
   (`9` / `26` / `2`, `RC=0`, `IS_RELEASE=true`) and `src/clientversion.h`.

This is a **documentation-validation** prompt, not a code audit. Do not hunt for
code bugs. Do not change `src/`, tests, build files, or scripts. The only files
you may modify are root-directory `.md` files, and only to make them match the
code that already exists. **Code is the source of truth; the docs must conform to
the code, never the reverse.** If a doc and the code disagree, the doc is wrong
unless the discrepancy reveals an actual code defect — in which case you record
it as `CODE_DISCREPANCY_FOR_JARED` and do **not** edit code to fit the doc.

This rule from `CLAUDE.md` governs the entire run:

> Avoid widening doc claims beyond what code shows. Treat code as truth.

## Scope — root `.md` files only

In scope: **only `.md` files in the repository root** (`/home/jared/Code/digibyte/*.md`).
No subdirectories. No `docs/`, no `src/`, no `doc/`, no `contrib/`. Re-enumerate
the live set at the start of the run; do not trust this list blindly:

```bash
cd ~/Code/digibyte
ls -1 *.md | sort
git status --short -- '*.md'
```

The root `.md` set at the time this prompt was written (re-confirm live — files
may have been added or removed):

**Tier A — Public / release-facing docs (highest bar; ship with the release):**

| File | Purpose | Primary code surface to validate against |
|------|---------|------------------------------------------|
| `README.md` | Project front page, links, version badge | `configure.ac`, repo links, `INSTALL.md` |
| `INSTALL.md` | Build/install entry | `doc/build-*`, `configure.ac` |
| `SECURITY.md` | Security policy / disclosure | low code coupling — validate contacts/keys/links/policy are current; "low coupling" is not an exemption from checking |
| `CONTRIBUTING.md` | Contributor workflow | branch model, build/test commands |
| `ARCHITECTURE.md` | Core DigiByte system design | `src/` core: chainparams, consensus, net, pow |
| `DIGIDOLLAR_EXPLAINER.md` | User-facing DigiDollar V1 summary | `src/digidollar/`, `src/consensus/digidollar*` |
| `DIGIDOLLAR_ARCHITECTURE.md` | DD mint/transfer/redeem/collateral/health/state | `src/digidollar/`, `src/consensus/`, `src/script/` |
| `DIGIDOLLAR_ACTIVATION_EXPLAINER.md` | BIP9 gating of DD/oracle surface | `src/consensus/params.h`, `src/kernel/chainparams.cpp`, `src/deploymentinfo*` |
| `DIGIDOLLAR_ORACLE_EXPLAINER.md` | User-facing oracle/MuSig2 summary | `src/oracle/`, `src/primitives/oracle.h` |
| `DIGIDOLLAR_ORACLE_ARCHITECTURE.md` | Exchange aggregation, bundle lifecycle, MuSig2, P2P | `src/oracle/`, `src/net_processing.cpp`, `src/protocol.cpp` |
| `ORACLE_BUNDLE_EXPLAINER.md` | Oracle bundle wire/lifecycle explainer | `src/primitives/oracle.h`, `src/oracle/bundle_manager.cpp`, `src/consensus/` |
| `ORACLE_DISCOVERY_ARCHITECTURE.md` | Oracle endpoint discovery design | `src/oracle/`, discovery/P2P code |
| `DIGIDOLLAR_ORACLE_SETUP.md` | Oracle setup + migration runbook | oracle RPCs, `src/oracle/`, operator flags |
| `DIGIDOLLAR_WALLET_INTEGRATION.md` | Wallet/RPC integration guide | `src/wallet/`, `src/wallet/rpc/wallet.cpp`, `src/rpc/digidollar.cpp` |
| `DIGIDOLLAR_EXCHANGE_INTEGRATION.md` | Exchange/custody integration guide | DD RPCs, address formats, units |
| `DIGIDOLLAR_MINING_INTEGRATION_GUIDE.md` | Miner integration for DD/oracle bundles | `src/node/miner.cpp`, `getblocktemplate`, bundle injection |

**Tier B — AI-agent / maintainer guides (must be code-accurate; AI relies on them):**

| File | Purpose | Primary code surface to validate against |
|------|---------|------------------------------------------|
| `CLAUDE.md` | AI agent guide, branch rules, code-surface warnings | the entire DD/oracle surface it summarizes |
| `REPO_MAP.md` | Core DigiByte file index | `src/` core file tree |
| `REPO_MAP_DIGIDOLLAR.md` | DD/oracle file index | `src/digidollar/`, `src/oracle/`, DD RPC/Qt/test tree |
| `REPO_MAP_GUIDE.md` | How to maintain/interpret the repo maps | the two repo maps |

**Tier C — Internal / non-release (verify they are NOT misleading; lightest bar,
do not ship-block on prose polish):**

| File | Purpose |
|------|---------|
| `digidollar/HOUSEKEEPING_TO-DOs.md` | Internal task list |
| `Z_RED_HORNET_FINAL_PROMPT.md` | Internal audit prompt (gitignored) |
| `Z_RED_HORNET_v2.md` | Internal audit prompt (gitignored) |
| `doc/v9.26.2/Z_DOC_VALIDATION_v9.26.2_PROMPT.md` | This archived prompt (tracked) |

For Tier C: confirm tier classification is correct, confirm gitignored internal
prompts are still gitignored, and confirm none of them are misleading — meaning
no statement that contradicts current code, no outdated procedure that could
confuse a maintainer, and no public-facing falsehood that could leak into a
release. Verify by spot-checking the critical claims in each Tier-C file against
code, not by skimming for obvious lies. Do not polish their prose and do not
ship-block on them. If a Tier-C file is actually stale internal cruft, recommend
(do not perform) deletion and record it under `HOUSEKEEPING`.

Re-derive each file's tier from its actual content during Wave 0; do not assume
the table is still correct.

## Operating principles

- **Code is truth.** Open the cited source and read it with your own eyes. A claim
  is validated only when you have read the current source and can cite the exact
  `file:line` that backs it. Reading the source is mandatory, not optional.
- **Do not hallucinate.** Never invent or "remember" a constant, RPC name, file
  path, port, height, oracle count, quorum, opcode value, address prefix, version
  string, or behavior. If you cannot find it in code, the claim is `UNVERIFIED`
  until proven, and you must not assert it as fact in a doc.
- **Minimal, surgical edits.** Fix only what is wrong or unclear. Do not rewrite
  docs wholesale, do not restructure for taste, do not "improve" passing prose.
  Every edit closes a validated drift, ambiguity, or DigiByte/DGB-correctness
  defect. No drive-by rewrites.
- **Preserve voice and format.** Match each doc's existing heading style, tone,
  table format, and code-fence conventions. A reader should not be able to tell a
  validation pass happened, except that the content is now correct.
- **Don't fix code.** If a doc accurately describes code and the code looks wrong,
  that is out of scope — log it as `CODE_DISCREPANCY_FOR_JARED` with evidence and
  move on. This run never edits non-`.md` files.
- **No proof-by-proxy.** The only acceptable proof for a technical claim is a
  current `file:line` from **original source** (`src/`, `configure.ac`, build
  files, registered tests). You may **not** cite, as proof: the canonical-facts
  ledger, `CLAUDE.md`, a sibling doc, an old report, an RC release note, memory,
  or training knowledge. Those are leads; source is proof. A command/test result
  may be cited **only as supplementary** evidence, and only with a one-line
  explanation of exactly what the output proves and which source it corresponds
  to — never as a standalone substitute for reading the code.
- **Stop and ask Jared** before: deleting any doc, merging/splitting docs,
  changing a doc's public meaning in a way that implies a protocol/release
  decision, or "fixing" a doc to match code where the code itself appears wrong.

## Canonical facts ledger (build this FIRST, in Wave 0)

Most doc drift is a number or term that disagrees with code or with a sibling
doc. Before validating any doc, extract the **canonical values from source** into
a single ledger (`reports/doc_validation_canonical_facts.md`). Every later wave
checks each doc against this one ledger so all 20+ docs end up internally
consistent with each other and with code. At minimum, pin from code:

- **Version / release:** `configure.ac` major/minor/build/RC/suffix/IS_RELEASE;
  `src/clientversion.h`. Confirm the release name every doc should use is
  **v9.26.2** (final, not RC).
- **Chain constants** (`src/chainparams*`, `src/consensus/`, `src/kernel/chainparams.cpp`,
  `src/validation.cpp`, `src/amount`/`policy`): block time, `SUBSIDY`,
  `COINBASE_MATURITY` / `COINBASE_MATURITY_2`, `MAX_MONEY`, min relay / default
  tx fee in **DGB/kB** (not BTC/vB), P2P ports (mainnet `12024`, testnet26
  `12033`), RPC ports, network magic, bech32 HRPs (`dgb`/`dgbt`/`dgbrt`), DD
  address prefixes (`DD`/`TD`/`RD`).
  - **Block-time nuance — validate carefully, do not blindly "correct".** DigiByte
    is multi-algo. The effective/average block time is **15s** (`BLOCK_TIME_SECONDS`
    in `src/validation.h`), while the per-algorithm target spacing is **60s**
    (`nTargetSpacing` in `src/kernel/chainparams.cpp`). A doc saying "15 second
    blocks" is correct for the effective rate; a doc saying "60s" may be correct
    if it explicitly means per-algorithm spacing. Confirm which the doc means
    against both source locations before changing anything.
- **DigiDollar consensus** (`src/digidollar/`, `src/consensus/digidollar*`,
  `src/script/script.h`): DD tx version marker `0x0770`, type field encoding
  (1=MINT/2=TRANSFER/3=REDEEM), mint min/max cents, the 10 lock tiers and their
  canonical durations + confirmation-window rule, DD amount unit (1 cent),
  `MAX_DIGIDOLLAR`, opcode values `OP_DIGIDOLLAR=0xbb` … `OP_ORACLE=0xbf` and
  the `OP_CHECKPRICE` deterministically-disabled status.
- **Activation** (`src/consensus/params.h`, `src/kernel/chainparams.cpp`,
  `src/deploymentinfo.cpp`): BIP9 bit (23), `DEPLOYMENT_DIGIDOLLAR` start /
  min-activation-height / window / threshold per network (mainnet, testnet26,
  regtest), and the `nDDActivationHeight` / `nOracleActivationHeight` /
  `nDigiDollarMuSig2Height` values per network.
- **Oracle roster** (`src/kernel/chainparams.cpp` `vOracleNodes` /
  `consensus.vOraclePublicKeys` / `nOraclePubkeyCount` / `nOracleConsensusRequired`,
  `src/primitives/oracle.h`): per network — total slots, active count, and
  consensus/quorum required. **This is the single highest-risk drift area.**
  Re-derive the values yourself from `src/kernel/chainparams.cpp` and cite the
  lines. As of this writing the code declares **mainnet 35 slots / 7-of-35**,
  **testnet26 35 slots / 7-of-35**, and **regtest 7 slots / 4-of-7** (chainparams
  `nOraclePubkeyCount` / `nOracleConsensusRequired`, with `vOraclePublicKeys`
  holding 35 / 35 / 7 keys respectively) — confirm these are still current, then
  treat **any** doc that says otherwise (e.g. stale "21 active" or "9-of-17"
  figures from the RC era) as a defect to correct to the code's actual numbers.
- **RPC surface** (`src/rpc/digidollar.cpp` `RegisterDigiDollarRPCCommands`,
  `src/wallet/rpc/wallet.cpp` `GetWalletRPCCommands`): the exact list and count
  of registered DD/oracle RPCs (base vs wallet-context), and which named RPCs are
  intentionally absent/legacy/dead (`sendoracleprice`, `submitoracleprice`,
  `src/rpc/digidollar_transactions.cpp` entries).
- **P2P / wire** (`src/protocol.cpp`, `src/net_processing.cpp`): oracle message
  wire names and their gating.

When two docs disagree, the ledger value (from code) wins, and **both** docs are
corrected. Record in the ledger any place where the code itself is ambiguous or
where two source locations disagree — that is a `CODE_DISCREPANCY_FOR_JARED`.

## Fleet model — waves of up to 5 sub-agents

Run in **waves**, **up to 5 sub-agents per wave**, at **maximum ultracode effort**.
The main agent is the **orchestrator**: it owns the canonical-facts ledger, wave
dispatch, synthesis, conflict resolution between sub-agents, applying edits,
re-verification, the running ledger, and final signoff. Sub-agents investigate
and propose; the orchestrator decides and edits. If two sub-agents disagree about
a fact, the orchestrator re-derives it from code and is the tiebreaker.

**Orchestrator must spot-check, not trust.** Before applying a sub-agent's
corrections or marking any doc `VERIFIED`, the orchestrator independently re-reads
at least ~20% of that sub-agent's `file:line` citations (and 100% of any citation
behind a consensus/roster/version/activation/RPC-surface claim) by opening the
cited source itself. If any cited line does not actually say what the sub-agent
claimed, or is too vague to prove the claim, the whole doc is bounced back for
re-validation. Sub-agent assertions are leads until the orchestrator has seen the
code.

**Default per-agent unit of work: one document, validated end to end.** Each
sub-agent owns one root `.md` file (or a small cluster of tightly related ones),
reads it line by line, and for every claim either (a) cites the current
`file:line` that proves it, (b) proposes the corrected text with the code
citation that justifies the correction, or (c) marks it `UNVERIFIED` and records
the search performed (specific files opened, search terms used, and why the claim
could not be confirmed). Larger docs (e.g. `DIGIDOLLAR_ARCHITECTURE.md`,
`DIGIDOLLAR_ORACLE_ARCHITECTURE.md`, `REPO_MAP*.md`) may warrant a whole agent
each; tiny docs (`SECURITY.md`, `INSTALL.md`) can be clustered — but clustering is
a grouping for efficiency only, never an excuse for less depth: every clustered
doc gets the full D1–D4 treatment with the same rigor as a solo doc.

A **material technical claim** — the thing that always requires a source citation
— is any statement a reader could act on or that affects correctness: a version,
number, constant, port, address/HRP/prefix, opcode, height, oracle count/quorum,
RPC/CLI/config name, API field, behavior, procedure step, or feature-availability
claim. Expository prose that depends on such a claim must be backed by the same
citation. Pure narrative with no actable content needs no citation but also must
not contradict code.

Each sub-agent must check **all four dimensions** for its doc(s):

- **D1 — Code accuracy:** every material technical claim matches current source;
  cite `file:line` from **original source**, never from the canonical-facts
  ledger or a sibling doc (the ledger is a consistency cross-check, not proof).
  Flag every value that disagrees with the canonical-facts ledger.
- **D2 — DigiByte/DGB correctness:** no stray Bitcoin/BTC branding, units,
  defaults, ports, block times, or "Bitcoin Core" labels that should be DigiByte;
  DGB units and DigiByte-specific values used throughout; address/HRP/prefix
  examples are DigiByte, not Bitcoin.
- **D3 — Readability for AI + humans:** clear structure, working internal links
  and file references, no contradictions within the doc or across docs, correct
  cross-references, terminology consistent with the canonical ledger, no
  confusing or stale instructions, examples that actually run/parse.
- **D4 — Release-readiness:** version/status correct (v9.26.2 final), no
  "RC"/"draft"/"TODO"/"coming soon"/placeholder language in Tier A/B docs, no
  references to removed features or renamed symbols, no dead links to deleted
  files.

Give each sub-agent the context bundle below plus the canonical-facts ledger plus
its explicit file list and the specific code paths to read. Sub-agents **propose
diffs with citations**; they do not edit files. The orchestrator applies and
re-verifies. This keeps edits consistent across all docs and prevents two agents
from making contradictory changes.

### Sub-agent context bundle (every sub-agent must read first)

1. `CLAUDE.md`
2. The canonical-facts ledger (`reports/doc_validation_canonical_facts.md`)
3. The specific doc(s) it owns
4. The code paths listed for that doc in the Scope table
5. For DD/oracle docs, also: `DIGIDOLLAR_ARCHITECTURE.md`,
   `DIGIDOLLAR_ORACLE_ARCHITECTURE.md`, `REPO_MAP_DIGIDOLLAR.md` as cross-refs
   (but remember those are themselves under validation — trust code over them,
   and never cite them as proof in your report).

Sub-agents must not edit any file. Sub-agents must not redesign docs or invent
new sections. If a doc section describes a feature, the sub-agent's job is to
confirm the description matches code, not to reimagine the feature. A sub-agent
may not report a claim as `VERIFIED` unless its report carries the exact
original-source `file:line` it read; "looks right", "consistent with the ledger",
or "matches CLAUDE.md" are not verification.

## Per-document validation checklist

For each root `.md` file, the owning sub-agent produces a structured report:

- **File + tier.**
- **Claim inventory:** the material technical claims, each marked
  `VERIFIED (file:line)`, `DRIFT → corrected text (file:line)`, or `UNVERIFIED`.
- **DigiByte/DGB findings:** any Bitcoin/BTC leftovers, wrong units, wrong ports,
  wrong branding, with the corrected DigiByte value and its code citation.
- **Readability findings:** contradictions, broken links, dead file references,
  ambiguous instructions, cross-doc term mismatches.
- **Release-readiness findings:** version/status/placeholder/removed-feature
  issues.
- **Proposed diff:** exact before→after text for each correction, each with a
  code citation. Minimal edits only.
- **Cross-doc notes:** any value that must also change in another doc to stay
  consistent (hand to orchestrator).
- **CODE_DISCREPANCY_FOR_JARED:** any place the doc looks right but the code looks
  wrong, or two code locations disagree. Evidence only; no code edits.

## Suggested wave plan (adapt to the live file set)

Re-enumerate first; add or split waves as needed. Do not exceed 5 sub-agents per
wave. The orchestrator applies all corrections from a wave, re-verifies against
code, updates the ledger, and (if authorized) commits before starting the next
wave.

- **Wave 0 — Ground truth & inventory (orchestrator + up to 5 scouts).**
  Re-enumerate root `.md` files. Build the canonical-facts ledger from code.
  Resolve the high-risk oracle-roster / quorum / version numbers from source.
  Classify each file into Tier A/B/C. Produce the per-doc → code-surface map.
  Output: `reports/doc_validation_canonical_facts.md` and the wave plan.

- **Wave 1 — Core repo docs (Tier A/B).** `README.md`, `INSTALL.md`,
  `SECURITY.md`, `CONTRIBUTING.md`, `ARCHITECTURE.md` (+ `CLAUDE.md` if not held
  for Wave 5). Validate version badge/links, build/install steps, branch model,
  and core architecture against `src/` core, `configure.ac`, and build docs.

- **Wave 2 — DigiDollar core docs.** `DIGIDOLLAR_EXPLAINER.md`,
  `DIGIDOLLAR_ARCHITECTURE.md`, `DIGIDOLLAR_ACTIVATION_EXPLAINER.md`. Validate
  mint/transfer/redeem/collateral/health, lock tiers, opcodes, DD tx marker, and
  BIP9 activation against `src/digidollar/`, `src/consensus/`, `src/script/`,
  `src/kernel/chainparams.cpp`, `src/consensus/params.h`.

- **Wave 3 — Oracle docs.** `DIGIDOLLAR_ORACLE_EXPLAINER.md`,
  `DIGIDOLLAR_ORACLE_ARCHITECTURE.md`, `ORACLE_BUNDLE_EXPLAINER.md`,
  `ORACLE_DISCOVERY_ARCHITECTURE.md`, `DIGIDOLLAR_ORACLE_SETUP.md`. Validate
  exchange aggregation, bundle v0x03 lifecycle, MuSig2, P2P wire names/gating,
  roster/quorum, discovery, and operator setup against `src/oracle/`,
  `src/primitives/oracle.h`, `src/net_processing.cpp`, `src/protocol.cpp`, and
  the oracle RPCs.

- **Wave 4 — Integration / operator guides.** `DIGIDOLLAR_WALLET_INTEGRATION.md`,
  `DIGIDOLLAR_EXCHANGE_INTEGRATION.md`, `DIGIDOLLAR_MINING_INTEGRATION_GUIDE.md`.
  Validate every RPC name/param/return, address format, unit, and miner/bundle
  step against `src/wallet/rpc/wallet.cpp`, `src/rpc/digidollar.cpp`,
  `src/node/miner.cpp`, and registration tables.

- **Wave 5 — AI-agent maps, internal docs, and cross-doc consistency.**
  `CLAUDE.md`, `REPO_MAP.md`, `REPO_MAP_DIGIDOLLAR.md`, `REPO_MAP_GUIDE.md`, and
  Tier C files. Validate that the repo maps match the actual file tree (no listed
  file is missing, no major file is unlisted), that `CLAUDE.md`'s surface
  warnings match code, and run a **final cross-doc consistency sweep**: every
  shared number/term agrees across all docs and with the canonical ledger.

- **Wave 6+ (add-on) — Reconciliation.** Any doc that failed, any cross-doc
  conflict surfaced late, any `CODE_DISCREPANCY_FOR_JARED` needing a second pass.
  Add waves until every Tier A/B doc is `VERIFIED` or has an explicit, recorded
  open item.

## Fix discipline (verify → correct → re-verify)

For every correction:

1. **Prove the drift** against current code with an original-source `file:line`
   citation. No source citation, no edit.
2. **Apply the minimal edit** to the `.md` file — change only the wrong words,
   numbers, links, or branding. Preserve surrounding prose and formatting.
3. **Re-verify** the corrected text against the same code citation. Confirm you
   did not introduce a new inconsistency with a sibling doc or the ledger.
4. **Propagate** the same fact to every other doc that repeats it (orchestrator
   tracks shared facts so all docs stay consistent).
5. **Record** the correction in the running ledger with before/after and citation.

Do not "fix" by deleting hard-to-verify content. If a claim cannot be verified,
either prove it from code, soften it to exactly what code supports, or escalate
it — do not silently drop or invent. Do not introduce new claims a reader cannot
verify from code.

## Anti-hallucination rules

- Cite current original-source `file:line` for every material claim that ends up
  in a doc or that you assert in a report. Command/test output may accompany a
  citation as supplementary proof, with a note on what it demonstrates, but never
  replaces reading the source.
- Never invent filenames, RPC names, test names, config keys, line numbers,
  commit hashes, release facts, oracle slots, quorum values, ports, opcodes,
  address prefixes, or activation states.
- If unsure, open the file and read it. If still unsure, mark `UNVERIFIED` and
  record what evidence is missing (files opened, terms searched) — never guess
  into a doc.
- Old reports, RC release notes, the canonical-facts ledger, `CLAUDE.md`, and
  sibling docs are **leads, not proof**. Reconfirm against current source.
- Separate observation from inference; label inferences as inferences and cite
  the source/observation each inference rests on. An inference that affects code
  correctness or release readiness must link to the code it depends on.

## Commit discipline

Never push. Before any edit: `git status --short -- '*.md'` — do not overwrite
unrelated local work. Only `.md` files in root may be staged by this run.

If commits are authorized, commit cleanly:

- Group by doc or by a single shared fact corrected across docs.
- One logical correction set per commit; no unrelated mega-commits.
- Commit message names the doc(s) and the nature of the fix, e.g.
  `docs: correct oracle roster/quorum numbers to match chainparams (v9.26.2)`.
- Commit body cites the code that justifies the change.
- Do not mix Tier-C housekeeping with Tier-A release-doc corrections.

If commits are not authorized, leave edits uncommitted and record the exact
proposed commit split.

## Reporting

Maintain a durable ledger from Wave 0 to signoff:

- Canonical facts: `reports/doc_validation_canonical_facts.md`
- Running ledger: `reports/doc_validation_ledger.md`
- Final report: `reports/doc_validation_final_report.md`

Each wave summary records: wave number; docs covered; sub-agent assignments; per
doc D1–D4 findings; corrections applied (before→after + citation); cross-doc
facts propagated; `UNVERIFIED` items; `CODE_DISCREPANCY_FOR_JARED` items;
re-verification result; commit hash/status.

Final report records: the canonical-facts ledger; per-doc final status
(`VERIFIED` / `VERIFIED WITH OPEN ITEMS` / `BLOCKED`); every correction with its
original-source citation; every `UNVERIFIED` item still open; every
`CODE_DISCREPANCY_FOR_JARED` gathered into one section for Jared; the cross-doc
consistency result; and a single release verdict. A doc may be marked `VERIFIED`
**only** if the report carries, or references in the running ledger, the
`file:line` citations behind its material claims — and the orchestrator has
spot-checked a sample of them against source. "VERIFIED" with no recorded
citations is not allowed.

## Final signoff criteria for v9.26.2

The doc set is release-ready only when **all** are true:

- Every Tier A and Tier B root `.md` file is `VERIFIED` against current code —
  each material claim backed by a recorded original-source `file:line` the
  orchestrator spot-checked — or has an explicitly listed, Jared-accepted open
  item. No doc is `VERIFIED` on intuition or proxy citations.
- Every shared fact (version, oracle roster/quorum, activation heights, ports,
  opcodes, RPC counts, constants, addresses, units) is identical across all docs
  and equals the code-derived canonical value.
- No Tier A/B doc contains Bitcoin/BTC branding, units, ports, or defaults that
  should be DigiByte/DGB.
- No Tier A/B doc contains RC/draft/TODO/placeholder/"coming soon" language,
  references to removed features, or dead links to deleted files.
- Every doc reads clearly for both AI agents and humans (structure, links, no
  contradictions).
- All `CODE_DISCREPANCY_FOR_JARED` items are gathered in one section for Jared's
  decision; none were silently patched, and no code was edited.
- Tier C internal/gitignored files are confirmed non-misleading and still
  correctly classified/ignored.

## Final output to Jared

Keep it short enough to read and complete enough to act on:

- Overall verdict: READY / READY WITH OPEN ITEMS / NOT READY.
- Root `.md` files validated (count) and per-doc status table.
- Corrections applied (count) and commit status.
- Cross-doc consistency result.
- DigiByte/DGB-correctness result.
- Open `UNVERIFIED` items.
- `CODE_DISCREPANCY_FOR_JARED` items needing Jared's decision.
- Exact report paths.

Do not hide risk. Do not rely on memory. Code is the source of truth; the docs
must conform to it. Validate, do not rewrite. Fix only what is proven wrong.
