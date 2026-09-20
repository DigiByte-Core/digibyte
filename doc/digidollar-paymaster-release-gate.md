# DigiDollar Paymaster V1 release gate

The v9.26.6rc2 integration, RPC migration and pending build/runtime gates are
recorded in the [integration notes](digidollar-paymaster-v9.26.6rc2-integration.md).

**Status review:** 2026-09-11, source commit `bd270044c1` on
`feature/digidollar-paymaster-v1`. See the [developer starting point](../PAYMASTER.md)
and [implementation reference](digidollar-paymaster-implementation.md).

This matrix maps the 28 acceptance criteria in
[`DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md`](../DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md)
and the post-proposal adversarial-counterparty hardening to their verification
surfaces. It is an audit aid, not a substitute for a clean release build and
test run.

> **Current gate: RELEASE ACCEPTANCE OPEN.** The implementation is present in
> the inspected branch. The recorded broad local matrix belongs to the
> 2026-07-30 candidate; additional functional and official-v9.26.5 compatibility
> results were recorded on 2026-08-08 and 2026-08-09. Later source changes mean
> those results must not be presented as a fresh complete pass of `bd270044c1`.
> A release revision, affected-surface reruns, real-Tor deployment testing and
> independent review still need to be resolved. The 2026-09-11 documentation
> update does not run or renew the executable test matrix.

Status meanings:

- **Covered**: implementation and a focused automated test exist; this does not
  imply that the test passed on the current source tree.
- **Candidate pass**: historical candidate evidence; the listed in-tree acceptance checks passed on the dated
  working-tree candidate; this does not by itself satisfy an unlisted release
  gate.
- **Partial**: important coverage exists, but the listed test gap remains.
- **External**: verification requires binaries or environments outside one
  in-tree test process.

The last column records the gate originally associated with each criterion.
The local MSVC, functional, broad-regression, Qt, and same-revision WSL reruns
covered by the 2026-07-30 matrix are satisfied for that working-tree candidate.
Real-Tor deployment and independent-review requirements remain open. Official
v9.26.5/current wallet-file, mixed-node relay, old-only bridge, and unchanged-
datadir upgrade compatibility have dated passing evidence from 2026-08-08/09. The historical
snapshot records only superseded pre-hardening states.

| # | Coverage status | Coverage or historical evidence | Original/remaining release gate |
|---:|---|---|---|
| 1 | Covered | `wallet_paymaster_provider.py` completes a USER_PAID transfer from a client with no DGB. | Final functional rerun. |
| 2 | Covered | Paymaster PSBT and wallet tests verify role-scoped signing and never transfer wallet secrets. | Final focused unit run. |
| 3 | Covered | Provider/directory tests verify disjoint admission proofs; the provider functional test exercises separate admission and operational pools. | Final focused unit and functional run. |
| 4 | Covered | Fee-boundary and transaction-builder tests cover carrier fees from 0.01 through 0.99 DD. | Final focused unit run. |
| 5 | Covered | Client selection tests use exact rounded totals and deterministic local sorting; `wallet_paymaster_offer_selection.py` additionally exposes differently priced offers from two independent provider daemons, applies monotonic repricing, expires stale announcements, and accepts a fresh replacement. | Final focused unit run and the registered multi-provider functional run. |
| 6 | Covered | PSBT tests prove the user and provider sign only their own inputs. | Final focused unit run. |
| 7 | Covered | Template-binding tests reject mutations and require a newly verified attempt for provider-dependent fallback changes. | Final focused unit run. |
| 8 | Covered | Persistent request IDs, monotonic attempt state, same-input recovery, and duplicate-payment guards are tested. `wallet_paymaster_failover.py` additionally proves the exact reserved DD input set survives a real provider change and produces one payment. | Crash-specific variants are tracked under 22 and 23. |
| 9 | Covered | A three-node functional rerun on 2026-08-08 used the official v9.26.5 daemon: the old node accepted the Paymaster-created transaction in its mempool and validated it in the same block/tip as the upgraded nodes. The unchanged-datadir upgrade and old-only bridge scenarios also passed against that official daemon on 2026-08-09. | Satisfied by the official v9.26.5/current functional reruns. |
| 10 | Covered | Paymaster transaction/protocol rules are wallet/P2P policy. The broader branch also changes DCA, ERR and volatility source to use portable 128-bit types; those changes require separate arithmetic-equivalence review. | Final diff review, cross-platform arithmetic checks and broad consensus regression run. |
| 11 | Covered | Reputation tests cover local-only storage, neutral failures, cooldown, and security-first eligibility. | Final focused unit run. |
| 12 | Covered | Headless selection, fee caps, durable sessions, and request-id idempotency are covered by client/store and provider functional tests. | Final focused and functional run. |
| 13 | Covered | Legacy `senddigidollar` without options remains on the wallet-funded path and has regression coverage. | Final broad wallet regression run. |
| 14 | Covered | The provider functional test completes public and restricted zero-service-fee sponsorship with the common protocol. | Final functional rerun. |
| 15 | Covered | Capability tests bind restricted authorization to the payment, store only its hash, reject reuse, and keep it out of gossip. | Final focused and functional run. |
| 16 | Covered | Builder tests require fresh internal DD/DGB change; the provider functional test scans production logs for payment artifacts and secrets. | Final functional rerun with `debug=net,rpc`. |
| 17 | Covered | Readiness tests enforce onion-only high privacy, Tor isolation, one attempt, and no v1/clearnet fallback. | Perform a deployment smoke test with a real Tor proxy/onion service. |
| 18 | Covered | `wallet_paymaster_offer_selection.py` proves automatic cheapest-provider selection and no mutation at the unselected provider. `wallet_paymaster_failover.py` adds two-client contention for one cheap operational slot and sequential completion through the standby provider. | Repeat both registered multi-provider functional tests on the release build. |
| 19 | Covered | Store tests cover retention redaction, tombstones, idempotency, mempool observation, confirmation, and reorg handling. `wallet_paymaster_reorg.py` adds a real longer-chain rollback/reconfirmation of client state, provider finance, and successor liquidity. | Final focused unit and registered reorg functional run. |
| 20 | Covered | Documentation states the pseudonymity boundary; the Qt privacy selector carries the same warning, and its assertion passed with Qt 5.15.10. | Final broad Qt regression run. |
| 21 | Covered | Recovery tests cover recheck, exact retry, same-input fallback/self-return, durable locks, and confirmation-dependent cancellation finality. The functional failover test pins both safe pre-signature fallback and rejection after a user PSBT exists. | Crash-specific variants remain under 22 and 23. |
| 22 | Covered | Every provider-commit write and the commit operation have deterministic rollback injection; a reopened post-commit snapshot contains the exact raw transaction, bindings, pool state, sponsorship state, and `PENDING_NETWORK` subphase used by retry/broadcast recovery. | Final focused and functional rerun. |
| 23 | Covered | Every user-authorization write and the commit operation have deterministic rollback injection; a reopened successful snapshot contains the exact persisted user-signed PSBT hash, attempt, authorization, and `USER_SIGNATURE_SENT` subphase without another variant. | Final focused and functional rerun. |
| 24 | Covered | Request parsing, analysis, builder, and boundary tests enforce one recipient and the 100–10,000,000-cent bounds on every DD output. The `paymaster_wire_envelopes` target completed 100,000 ASan/UBSan/libFuzzer runs after its timestamp-overflow finding was fixed and replayed successfully. | Final focused unit run. |
| 25 | Covered | Readiness tests enforce unpruned, txindex-complete client/provider operation. The official-v9.26.5 leaf scenario proves old-node validation of the ordinary final transaction. On 2026-08-09 `p2p_paymaster_v9_26_5_bridge.py` additionally passed ordinary DGB/DD relay across an old-only bridge while Paymaster discovery failed closed until a direct current link existed. | Satisfied by the official v9.26.5 leaf and bridge runs. |
| 26 | Covered | Signed monotonic `PMRESULT` and one commit key/raw transaction are tested; deterministic failure at each atomic provider-commit write and at commit proves complete rollback of the commit, sponsorship, pool, attempt, and session records. | Final focused unit run. |
| 27 | Covered | Store/RPC tests distinguish mempool from confirmation and expose the shared state/finality fields; Qt renders those fields. | Final RPC and Qt regression run. |
| 28 | Covered | Locked-wallet tests pause the same session without fallback, and provider policy requires a positive absolute DGB network-fee cap. | Final focused and functional run. |

## Revision and evidence requirements

Before a release decision, attach the exact source commit, build configuration,
binary identification, command, exit code, date, and retained output for each
required surface. A test file or workflow being present is coverage, not a
successful run. The records below retain earlier reported counts without
claiming a new independent reproduction.

Later branch changes include current-only persistence handling, wallet metadata
documentation, RPC/store decomposition, compatibility extensions, and Qt
workflow hardening. Re-run the checks affected by those changes on the selected
release revision. Independent review and real-Tor testing remain explicit
external gates; documentation reconciliation does not close either one.

## Adversarial-counterparty hardening gate

These checks extend, rather than replace, the 28 proposal criteria. A release
must demonstrate the invariant that once an honest side signs, every movement
of its own DD or DGB exactly matches its locally persisted authorization
manifest.

| ID | Current status | Implementation/coverage surface | Required acceptance |
|---|---|---|---|
| H1 | Candidate pass | Protocol V5 requires `PMCAPREQ`/`PMCAPRESP`; the client validates provider identity, BIP86 control proofs, creating transactions, live Chainstate resources, reference block, nonce, request/session binding, and expiry before disclosing intent or capability. Validated resources are persisted and bound to the later quote. | Malicious-provider tests for foreign/spent/double-promised resources, bad control proofs, stale references, quote/resource mismatch, reconnect, and explicit no-downgrade behavior. |
| H2 | Candidate pass | `ClientAuthorizationManifest` and `ProviderAuthorizationManifest` are built from local authority and revalidated by the shared core immediately before client/provider signature, retry, recovery, and broadcast. Unknown PSBT fields, roles, outputs, and non-default sighashes fail closed. | Mutation matrix for inputs, outputs, amounts, recipients, fees, change ownership, PSBT fields, roles, sighashes, policy, capacity, and template commitments through RPC, Qt, and background paths. |
| H3 | Candidate pass | Final processing reconstructs the trusted PSBT, checks raw/non-witness transaction, txid, wtxid, exact inputs/outputs/all witnesses, runs the script interpreter with actual prevouts, and performs a mempool preflight when needed. Final state is monotonic and result/attempt/session writes are atomic. The registered functional reorg rolls one real final transaction back and reconfirms the same txid without a duplicate finance event. | Invalid-witness, same-txid/different-witness, mempool-conflict, late-result, replay, reorg, restart, failure-injection, and immutable-final-state tests. |
| H4 | Candidate pass | `ProviderSafetyPolicy` and `ProviderBudgetReservation` apply finite per-model transaction/reserved/hour/day fee and completion limits plus active-quote and request limits. Quote reserve, final spend, and safe release are atomic; ledger and accounting-time high-water survive restart. The functional contention case holds the only cheap slot for one client while a second safely moves to the standby provider. | Last-budget/last-slot races, concurrent quotes, clock rollback, crash at every write, restart, expiry, public-sponsorship drain, and balance-oracle tests proving no overspend. |
| H5 | Candidate pass | `ClientSafetyPolicy` durably limits service fee per transfer and rolling day; the effective cap is the minimum of wallet policy, RPC limit, and signed offer. | Per-transfer/daily boundary, concurrent reservation, policy change, restart, exact retry, rejection without policy, and RPC/Qt parity tests. |
| H6 | Candidate pass | Semantic replay keys are independent of peer ID; exact retries are idempotent and conflicting content is rejected. The first valid signed Capacity/quote claim is staged evidence-only before ACK/deep validation, with persisted-authority failures kept distinct from invalid remote artifacts. Generation-bound RAII leases protect inbox ownership across the database boundary. Critical replay state persists. Token buckets cover peer/netgroup/provider/session, work is fairly queued, cheap announcement/rate checks precede crypto, TTL math saturates, and signed equivocation evidence locally blocks the provider. | Cross-peer/reconnect/restart replay, DB-write failure and retry, lease overlap/ABA, exact request/session binding, queue fairness, netgroup/announcement flood, expiry race, sequential and simultaneous conflicting Capacity/quote, persistent block, and timestamp/amount boundary fuzz tests. |
| H7 | Candidate pass | `resolvepaymastersession ... cancel_to_self` can use a distinct `USER_PAID` provider that passes Capacity V5. The recovery manifest permits only the original user inputs, fresh wallet-owned DD returns, a locally capped recovery fee, and recovery-provider DGB inputs. | End-to-end recovery without client DGB, same-provider rejection, privacy inheritance, malicious outputs/fees, ambiguous original broadcast race, restart, exact retry, and confirmation-dependent release tests. |
| H8 | Candidate pass | Stateful fuzz targets cover Capacity → Intent → Quote → Submit → Result → Recovery; focused wallet/P2P tests cover manifests, budgets, replay, and finality. On 2026-07-30 the exact synchronized candidate was rebuilt with Clang ASan/UBSan/libFuzzer, all saved artifacts/corpora replayed successfully, and both fresh targets completed 100,000 runs. | Repeat this matrix after any affected source, build, or dependency change. |

## Recorded candidate verification (2026-07-30 through 2026-08-09)

These are previously recorded results for the candidates and dates named below.
They do not certify the current branch head or a tagged release artifact:

| Surface | Verified result |
|---|---|
| MSVC Debug | `test_digibyte`, `digibyted`, and `digibyte-cli` built successfully. The 16 focused Paymaster suites plus `walletload_tests` passed 196/196, and the complete suite passed 3,605/3,605 with exit code 0. The MSVC Debug CRT still emits its known process-shutdown allocation dump after the successful result; this remains diagnostic follow-up rather than a hidden test failure. |
| MSVC Release | `test_digibyte`, `digibyted`, `digibyte-cli`, `digibyte-qt`, and `test_digibyte-qt` built successfully. The focused Paymaster plus `walletload_tests` selection passed 196/196, and the complete core suite passed 3,605/3,605 with exit code 0. |
| Functional Debug | `wallet_paymaster_provider.py` and `wallet_paymaster_readiness.py` completed successfully against the Debug candidate. Explicit safe port seeds avoided locally reserved Windows TCP ranges. |
| Functional Release | `wallet_paymaster_provider.py` and `wallet_paymaster_readiness.py` completed successfully against the Release candidate. |
| Multi-provider selection (2026-08-08) | On 2026-08-08 the registered `wallet_paymaster_offer_selection.py --descriptors` scenario passed all 17 framework unit tests and its 1/1 functional test in 27 seconds. Two independent current-version provider daemons advertised 50-bps and 200-bps USER_PAID offers to a DGB-less client; the client ordered both exact totals, selected only the cheaper provider, completed and confirmed one transfer, and left the expensive provider's pool, safety budget, and finance ledger unchanged. |
| Extended multi-provider/reorg checks (2026-08-08) | On 2026-08-08 one registered local Windows `test_runner.py --jobs=1` invocation passed all 17 framework unit tests plus `wallet_paymaster_offer_selection.py --descriptors` in 21 seconds, `wallet_paymaster_failover.py --descriptors` in 47 seconds, and `wallet_paymaster_reorg.py --descriptors` in 9 seconds (77 seconds accumulated). The same registered group then passed under x86_64 WSL with all 17 framework tests and durations of 12, 24, and 3 seconds (39 seconds accumulated). The scenarios cover monotonic repricing and expiry/refresh; one-slot contention, same-input standby fallback, post-signature rejection, and exact single-provider accounting; plus finance/pool rollback and same-txid reconfirmation. |
| Official v9.26.5/current compatibility | On 2026-08-08 an x86_64 WSL build ran both registered `wallet_v9_26_5_compatibility.py` variants and the three-node `wallet_paymaster_provider.py --descriptors` scenario against the official v9.26.5 Linux daemon. Each invocation passed all 17 framework unit tests. The legacy and descriptor wallet variants passed in 36 accumulated seconds; the provider scenario passed in 27 seconds after its restart path was corrected to reconnect the third node. |
| New official-v9.26.5 scenarios | On 2026-08-09 `p2p_paymaster_v9_26_5_bridge.py --descriptors` passed against the official Linux daemon in 38 seconds after all 17 framework tests passed. It relayed and confirmed ordinary DGB and DD through the old-only bridge, proved through raw message capture that the old node received but did not relay `pmannounce`, failed Paymaster selection closed, and restored discovery over a direct current-current link. `wallet_v9_26_5_inplace_upgrade.py --descriptors` subsequently passed in 7 seconds after all 17 framework tests passed. It created encrypted v9.26.5 wallets, two active positions, and isolated pending DGB/DD transfers; loaded the exact datadir's blocks, Chainstate, txindex, and mempool with the current daemon while wallets were disabled; then loaded, reannounced, confirmed, spent, redeemed, and restarted the inherited wallet state. |
| Native Qt | Qt 5.15.10 was rebuilt and installed with the bundled-zlib namespace backport. Inspection found the three affected `z_crc32_combine_*` symbols prefixed and no conflicting unprefixed definitions in `Qt5Core.lib`; the Release Qt targets linked and all eight registered suites completed with exit code 0. |
| WSL2 sanitizers/fuzzing | The synchronized 2026-07-30 source candidate rebuilt successfully with Clang `-fsanitize=address,fuzzer,undefined`. One saved wire artifact plus the 710-file wire and 2,365-file stateful corpora replayed successfully. Fresh `paymaster_wire_envelopes` and `paymaster_stateful_security` campaigns each completed 100,000 executions with exit code 0. Wire added 289 units in 12 seconds at 373 MB peak RSS; stateful added 628 units in 2,004 seconds at 562 MB peak RSS. |

Any source, build, or dependency change that can affect a verification surface
invalidates that surface's dated evidence and requires the affected checks to
be repeated.

## Required closing sequence

Steps 1, 2, 4, 5, 6, and 8 have dated candidate evidence above. Step 3 has
passing local and WSL evidence, but still requires the listed release-build
rerun. Steps 7 and 9 remain open. Completed steps must be repeated after any
affected source, build, or dependency change.

1. **Dated candidate evidence — final review:** The final Paymaster security
   diff was reviewed for consensus isolation, protocol V5/no-downgrade,
   manifest call-site completeness, atomic wallet writes, finite limits, and
   absence of raw IP/payment material in persistent rate/accounting keys. No
   obvious release-blocking counterparty theft path was found; this does not
   replace the independent review in step 9.
2. **Dated candidate evidence — MSVC:** Build `test_digibyte`, `digibyted`, and `digibyte-cli` with MSVC in Debug and
   Release. Run the complete focused Paymaster unit/wallet suites in both
   configurations, including malicious-counterparty, expiry, replay,
   last-budget/slot, crash, restart, and final-witness regressions.
3. **Dated local evidence — functional:** Run
   `wallet_paymaster_provider.py`, `wallet_paymaster_offer_selection.py`,
   `wallet_paymaster_failover.py`, `wallet_paymaster_reorg.py`, and
   `wallet_paymaster_readiness.py` with
   candidate binaries. The provider scenario must cover USER_PAID, public and
   restricted SPONSORED, finite budgets, drain-only completion, distinct-provider
   no-client-DGB recovery, restart, exact retry, capability replay, and balance
   invariants. The selection scenario must expose two differently priced offers
   from independent provider daemons, choose the lowest exact total without a
   provider override, and leave the unselected provider untouched. On
   2026-08-08 the newly registered descriptor selection scenario passed its
   17 framework unit tests and 1/1 functional test in 27 seconds. The extended
   selection, failover/contention, and reorg scripts subsequently passed one
   registered local current-build `test_runner.py` run including all 17
   framework unit tests; repeat it on the release build before closing this
   step.
4. **Dated candidate evidence — official
   v9.26.5/current compatibility:** On
   2026-08-08 both registered variants of
   `wallet_v9_26_5_compatibility.py` passed against the official v9.26.5 Linux
   daemon. The run covered raw legacy and descriptor wallet-directory loading
   without an explicit upgrade, encryption and wallet state across restart,
   post-load signing and spending, active DD records in the descriptor wallet,
   and reopening in v9.26.5 after current-version use without Paymaster. Both
   versions sent and received DGB directly, and the descriptor wallets sent and
   received DD directly. The test also required cross-version address
   validation, both-mempool relay with identical raw transaction bytes,
   confirmation by the receiving version, and exact amount/direction history
   after restart. The three-node `wallet_paymaster_provider.py --descriptors`
   scenario also passed with the official v9.26.5 daemon: the Paymaster result
   remained an ordinary `DD_TX_TRANSFER`, reached the old node's mempool with
   identical raw bytes, was validated in the same block/tip, and no Paymaster
   announcement was received or sent by the old node.
   `p2p_paymaster_v9_26_5_bridge.py --descriptors` now covers the old-only P2P
   bridge and passed against the official daemon on 2026-08-09.
   `wallet_v9_26_5_inplace_upgrade.py --descriptors` adds the unchanged-datadir
   pending-state/redeem path and also passed against the official daemon on
   2026-08-09.
5. **Dated candidate evidence — Qt 5.15.10:** Build and run the complete native Qt 5.15.10 suite, including client/provider
   safety-policy RPC parity, authorization confirmation, warning persistence,
   recovery, and authoritative finality rendering.
6. **Dated candidate evidence — WSL2:** The exact synchronized candidate was
   rebuilt in WSL2/Ubuntu with Clang/libFuzzer plus ASan/UBSan. Saved artifacts
   and both corpora replayed successfully; fresh `paymaster_wire_envelopes` and
   stateful Capacity → Recovery campaigns each completed 100,000 runs with
   clean exit codes.
7. **Open — real Tor deployment:** Exercise high-privacy mode against a real Tor proxy and onion service,
   including stream isolation, `-logips=0`, capture rejection, one-attempt
   enforcement, and no clearnet/v1 fallback.
8. **Dated candidate evidence — broad local regressions:** The complete MSVC
   Debug and Release core suites each passed 3,605/3,605, and the complete
   native Qt suite passed all eight registered suites. Platform- or
   configuration-specific release matrices beyond these local surfaces remain
   separate release-review responsibilities.
9. **Open — independent review:** Have the counterparty model, manifest and
   budget invariants, and race/crash behavior assessed independently. Until
   this is completed or explicitly dispositioned by release review, the
   implementation must not be described as formally proven or independently
   audited.

## Historical verification snapshot (2026-07-28; superseded)

The following results are retained for provenance only. They predate the
adversarial-counterparty hardening and **do not satisfy the current gate**:

- MSVC Debug and Release core builds passed; the test project used `/bigobj`
  for existing large DigiDollar test translation units.
- The then-current focused Paymaster unit set reported 75/75 passing in Debug
  and Release.
- The then-current provider/readiness functional tests and frozen/current
  mixed-version runs reported passing.
- The then-current native Qt 5.15.10 targeted and complete suites reported
  passing exit codes.
- `paymaster_wire_envelopes` first found a signed timestamp overflow. After the
  corresponding saturating-time fix, its saved artifact and a fresh 100,000-run
  ASan/UBSan campaign reported success.
- Python syntax checks and `git diff --check` reported success.
- A broad Release run that excluded separately tracked BIP324 vectors was not
  clean: it reported network-dependent oracle, DigiDollar performance, RBF, and
  Windows command-text failures. That historical disposition remains context,
  not a waiver for the new common-revision rerun.

## Additional security boundary checks (2026-09-19)

The integration working tree adds tests and CI coverage without changing the
production protocol, RPC permissions, wallet schema, or consensus. These results
do not close the overall release gate or the previously recorded unrelated and
unresolved integration failures.

| Check | Local evidence | Limit |
|---|---|---|
| RPC access and wallet isolation | `wallet_paymaster_rpc.py --descriptors`: passed, 40 s. Missing/bad authentication returns HTTP 401; a read-only method whitelist rejects provider changes, signing and withdrawal with HTTP 403, including mixed batches. Request/session IDs cannot read or cancel records in another loaded wallet. | Existing node RPC authority is reused; RPC users are not per-wallet tenants. |
| Real database recovery | `wallet_paymaster_lifecycle.py --descriptors`: passed, 35 s. Abruptly terminates the test provider child process after durable quote/client authorization, resumes one payment, and restores an older authorized-client SQLite backup after confirmation. | The older backup safely rejects the spent Capacity inputs; it does not automatically reconstruct the missing provider result. No power-loss/disk-full or stale-provider-ledger guarantee is inferred. |
| Bounded V2 adversarial traffic | `p2p_paymaster.py`: passed, 14 s. Two real V2 daemons send 32 malformed announcements each across three connection rounds; invalid offers never enter the directory, ping and block relay remain functional. | Not a sustained CPU/RSS test or a test of expensive, well-formed invalid signatures. |
| Extracted wallet load suite | `paymaster_wallet_load_tests`: 7 cases, 172 assertions passed in the existing Windows unit binary. | This binary predates the latest RPC result-schema fixes; no fresh full-build or sanitizer pass is claimed. |
| Security CI | Selects every registered `paymaster_*` suite, including wallet load; adds the send-RPC/build/helper triggers and uploads the actual unit-test log. | Current-revision Linux ASan/UBSan/fuzz execution is still pending. Qt is excluded by the headless configuration. |

The new functional scenarios ran with the local daemon built after the RPC schema
fixes. No production source or public interface was changed for these checks.

### Focused operator rerun

Prerequisites: freshly built daemon/CLI/unit binaries for the reviewed revision,
Python, and the existing descriptor-wallet/SQLite support. Working directory is
the repository root. Expect minutes for this focused selection, with exit code 0
and all selected cases passing (no unexpected skips):

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:PYTHONUTF8 = '1'
python -u test/functional/test_runner.py wallet_paymaster_rpc.py wallet_paymaster_lifecycle.py p2p_paymaster.py -j1
if ($LASTEXITCODE -ne 0) { throw "Paymaster boundary tests failed: $LASTEXITCODE" }
.\build_msvc\x64\Release\test_digibyte.exe '--run_test=paymaster_*' --report_level=short
if ($LASTEXITCODE -ne 0) { throw "Paymaster unit tests failed: $LASTEXITCODE" }
```

After the reviewed changes are committed and pushed, the operator can dispatch
the existing GitHub workflow from this directory with an authenticated GitHub
CLI. Verify that the selected run's head SHA is the intended revision. Expect
tens of minutes or longer; require both jobs to pass and retain the test log:

```powershell
gh workflow run paymaster-security.yml --ref integration/paymaster-v9.26.6rc2
gh run list --workflow paymaster-security.yml --branch integration/paymaster-v9.26.6rc2 --limit 5
gh run watch --exit-status
```

The Qt and full-build commands remain in the [integration notes](digidollar-paymaster-v9.26.6rc2-integration.md#operator-build-and-runtime-gates).
A successful headless workflow does not replace those Qt checks. The local
Python runtime checks and `git diff --check` do not replace flake8 or actionlint;
those tools were unavailable in the inspected environment.

### Remaining deployment and artifact evidence

- Repeat real Tor/onion testing with the reviewed binaries: stream isolation,
  proxy failure and authentication failure must retain the existing no-clearnet/
  no-V1 fallback and secret-redaction guarantees. Keep the independent review
  and current-revision sanitizer/fuzz gates open until evidence is returned.
- In an isolated fault-injection environment, cover interruption during database
  writes, actual storage exhaustion and older provider backups. Verify live
  outpoints before use and reconcile post-backup sponsorship and budget records;
  ordinary wallet rescanning alone is not evidence of ledger restoration.
- Extend the bounded network case with sustained, correctly encoded invalid
  proofs while legitimate Paymaster payments and ordinary DGB sends run. Record
  CPU/RSS, completion and reservation/budget counts. Unit queue/rate-limit tests
  already cover deterministic limits; do not duplicate those algorithms in a
  second production limiter.
- Verify Windows wallet/backup/diagnostic access controls with disposable wallet
  data and a separate unprivileged OS account. Encrypted private keys do not
  imply encrypted Paymaster metadata. Test backup destinations and any actual
  database sidecar files, not only the original wallet directory.
- Tie the release inventory to the exact binaries and link inputs. The existing
  SPDX workflow exports GitHub's repository dependency graph; it is not proof
  of the dependencies embedded in a Windows executable. Include static Qt,
  bundled Qt third-party code, vcpkg packages and externally supplied libraries.
  Reconcile MSBuild link records with versions/source hashes and the executable
  hashes, then review vendor advisories for those exact versions. A DLL listing
  alone omits static dependencies. The local vcpkg status file did not enumerate
  every manifest dependency, so no complete artifact inventory is claimed.

From a Visual Studio developer PowerShell in the repository root, these read-only
commands collect starting evidence after the final build (seconds, except hashing
large artifacts). Keep compiler/link logs from that same build alongside it:

```powershell
$env:QTBASEDIR = 'D:\Qt51510\install'
& "$env:QTBASEDIR\bin\qmake.exe" -query QT_VERSION
Get-Content build_msvc/vcpkg.json
Get-Content build_msvc/vcpkg_installed/vcpkg/status
Get-FileHash .\build_msvc\x64\Release\digibyte-qt.exe, .\src\digibyted.exe, .\src\digibyte-cli.exe -Algorithm SHA256
dumpbin /dependents .\build_msvc\x64\Release\digibyte-qt.exe
```

AI-assisted audit input (web pages, peer-provided strings, logs and code comments)
is untrusted data, not permission to execute instructions or weaken checks. Use
only disposable regtest secrets in shared diagnostics; do not submit production
wallets, keys, capabilities or credentials to external audit services. This is a
development workflow rule, not a reason to add an LLM filter to the wallet.

### Automatic setup follow-up (2026-09-20)

The testnet partial setup exposed a missing Dandelion scenario: a carrier parent
was stem-only when the immediate DGB child's normal fee precheck ran. Finite
setup now journals both output plans, waits for confirmed funding, and reuses
saved transactions after restart or rejection. V4 journal validation and explicit
legacy adoption add verification obligations. The registered
`wallet_paymaster_pool_setup.py` regression and affected unit suites must pass
on freshly rebuilt binaries before live recovery. Prior green results do not
cover this change; runtime acceptance is pending operator execution.

### Workflow edge-case review (2026-09-20)

The [working-tree edge-case review](digidollar-paymaster-edge-case-review.md)
records five source-level findings and a cross-workflow regression matrix.
It includes AUTO session-read failure, the setup wizard/RPC contract, recurring
Dandelion funding, setup conflict recovery and retirement accounting. These
findings now have local code corrections and targeted regression sources, as
listed in the review's remediation section. Runtime validation on freshly built
binaries remains open. This does not renew prior runtime results or approve the
pending automatic-setup changes.
