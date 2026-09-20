# Paymaster workflow and edge-case review

Review date: 2026-09-20. Base commit:
`354b711f92524cfab57bc163fca2384d20b8e90e`, branch
`integration/paymaster-v9.26.6rc2`, including the uncommitted automatic pool-setup
implementation. This is a source and test-coverage review of that working tree,
not a runtime certification of the base commit or the pending changes.

## Scope and evidence

The review follows the Paymaster RPC, wallet/store, scheduler, protocol/network,
finance and Qt entry points and their integration boundaries. The inventory
contains 17 Paymaster core/wallet unit-test source files, the Qt
`PaymasterWidgetTests` suite, 10 registered Paymaster functional entries
(including the optional old-node bridge), and four registered Paymaster fuzz
targets. Counts describe source inventory, not execution or branch coverage.

Relevant guidance: root and `src/AGENTS.md`, `test/functional/AGENTS.md`,
`CLAUDE.md`, `CONTRIBUTING.md`, architecture/repo maps, developer notes,
functional/unit/Qt test documentation, and the existing Paymaster release gate.
The new Dandelion regression deliberately departs from the generic advice to
disable Dandelion in transaction tests: disabling the affected subsystem hides
this failure class.

All findings below are supported by source control flow. Their exact runtime
reproductions remain to be implemented and executed on freshly built binaries.
No live node, existing testnet wallet, balance, or transaction was changed in
this review. No production source was edited during this review.

## Local corrections and verification

The following fixes and regression sources were added after the review below.
The findings retain their original audit wording and line references; they are
not descriptions of the corrected working tree. Runtime acceptance is pending.

| Finding | Correction | Regression added or extended |
|---|---|---|
| EC-01 | Preserve FOUND/NOT_FOUND/read-error/version-error through AUTO dispatch. | `paymaster_wallet_store_tests/auto_dispatch_rejects_unreadable_session_before_funding_or_signing`: live records, tombstones, future/truncated versions, storage failure, no mutation. |
| EC-02 | Approve while disabled, execute only when enabled; both Qt entry points accept pending work and require acceptance/fee fields. | `PaymasterWidgetTests::paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep` now has funded and deferred rows; the liquidity workflow rejects missing fields. `wallet_paymaster_pool_setup.py` checks disabled approval without signing or implicit enablement. |
| EC-03 | Recurring DGB refill selects confirmed funding. | `wallet_paymaster_pool_setup.py` adds simultaneous carrier/DGB deficits under Dandelion and verifies no child signature before parent confirmation. |
| EC-04 | Reconcile failed signed setup again and restore only unspent invalidated outputs. | `paymaster_wallet_identity_tests/pool_setup_reconciles_conflict_reversal_atomically` calls actual reconciliation through conflict, abandonment, commit failure and reconfirmation. The functional setup test adds a real conflicting block and exact transaction reconfirmation. A still-conflicted signed transaction intentionally remains protected; it is not cancelled as unsigned. |
| EC-05 | Store a provider-bound marker with the signed wallet transaction before broadcast, reconstruct missing retirement events from that existing durable record, and check DD retirement fees before commit. | `paymaster_wallet_identity_tests/retirement_finance_recovers_wallet_commit_after_write_failure_and_restart` exercises failed finance persistence and wallet reopening; `wallet_paymaster_provider.py` checks one exact fee event for each retirement asset transaction across repeated reconciliation. |

Retirement also rejects a pending finite setup to avoid contradicting an older
approval (part of G04). A fresh preview of an unchanged outstanding setup now
returns its original plan ID and approved fee total, so a lost execute response
can be retried without a second job. The functional setup test covers that path.
The Dandelion success scenario explicitly approves 0.3 DGB per step to leave
room for the conservative multi-output DD estimate; it separately checks the
unchanged 0.2-DGB default and refusal under an insufficient fee cap. These tests do not close the complete G01-G14 matrix:
real disk exhaustion, process-kill windows, deterministic thread races, full
backup/clock/retention combinations, sustained network load and real Tor remain
separate release evidence. Historical unmarked retirement transactions are not
guessed into the finance ledger. No consensus, P2P or ordinary wallet-selection
code was changed for these corrections.

Local checks: targeted MSVC `/Zs` compilation of the changed wallet/RPC units,
the two affected wallet test files, and the Qt production/test units. Qt uses
the installed v143 toolset required by the project; the newer compiler default
is incompatible with the installed Qt 5.15 headers. Fresh Qt MOC generation and syntax checking of that generated unit passed.
Both modified functional tests passed Python AST parsing and `--help` import/
argument registration checks; `git diff --check` passed. No rebuilt executable test or live
node action is claimed by syntax checks.

WSL follow-up (2026-09-20): the operator run passed 234 Paymaster unit cases
and all 46 Qt cases, but failed one maintenance-version unit case. That test
incorrectly rejected version 3 after the current record version became 4.
The corrected test explicitly accepts versions 3 and 4 and rejects versions
0, 1, 2 and 5, before and after serialization, without changing production
validation. An incremental WSL build and the corrected single test passed
(66 assertions). This initial result was superseded by the combined run below.

The functional runner stopped before its scenarios because `digibyte_scrypt`
was missing. Install the CI Python dependencies (`digibyte_scrypt`, `pyzmq`,
`pypandoc`) in a virtual environment and run the runner with that environment's
Python. On Ubuntu, creating the environment may require the operator to install
`python3-venv`; building Python extensions may require `python3-dev`.
Use `--disable-external-signer` when the configure Boost.Process probe fails;
the selected Paymaster scenarios do not require external signing support.

The subsequent operator run passed the Paymaster unit and Qt suites and four
of nine functional scenarios. Follow-up corrections cover stale setup-journal
state before retirement and internal retirement metadata leaking into wallet
RPC results. Functional fixtures now inspect the reconciled setup response,
capture read-only pool snapshots after readiness reconciliation, provide spent
prevouts when signing the competing reorg transaction, reconsider the original
invalidated block, and assert liquidity confirmation before ACTIVE after unlock.
The recovery-provider fixture explicitly approves the same 0.3-DGB setup fee
ceiling as the other multi-output DD fixture; production defaults remain intact.
All five previously failing functional scenarios passed targeted WSL reruns.
Incremental daemon builds, Python AST parsing and `git diff --check` passed.
The combined operator WSL run completed on 2026-09-20 at 20:19:37 +02:00:
Paymaster unit tests, all 46 Paymaster Qt cases and all nine selected functional
scenarios passed (exit codes 0/0/0). Evidence: operator log
`paymaster-retest-20260920-201621.log`. This verifies the selected Paymaster
matrix, not the complete DigiByte test suite or outstanding security release gates.

Operator runtime verification must use fresh binaries containing these changes.
In a configured Linux/WSL checkout, run the build and existing Paymaster matrix
at the end of this document. For a narrow first pass after that build:

```bash
./src/test/test_digibyte --run_test=paymaster_wallet_store_tests/auto_dispatch_rejects_unreadable_session_before_funding_or_signing,paymaster_wallet_identity_tests/pool_setup_reconciles_conflict_reversal_atomically,paymaster_wallet_identity_tests/retirement_finance_recovers_wallet_commit_after_write_failure_and_restart --log_level=test_suite --report_level=short
env -u DIGIBYTE_QT_TEST_OUTPUT QT_QPA_PLATFORM=offscreen DIGIBYTE_QT_TEST_SUITE=PaymasterWidgetTests DIGIBYTE_QT_TEST_FUNCTION=paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep ./src/qt/test/test_digibyte-qt
python3 test/functional/test_runner.py wallet_paymaster_pool_setup.py wallet_paymaster_provider.py -j1
```

Do not run an old executable against these names and count absence/skips as
success. The full build and provider functional test are delegated to the
operator under the repository's compute policy. Expect minutes to tens of
minutes or longer; success requires exit zero and all selected tests actually
executed. Existing Testnet data directories must not be used.

### Windows operator commands

Run in PowerShell from `D:\Digibyte\digibyte-fork` after other build/test jobs
finish. Prerequisites are the installed Visual Studio v143 toolset, static Qt
at `D:\Qt51510\install`, the existing vcpkg dependencies and configured
`test/config.ini`. This builds the four required programs and then runs the
focused wallet suites, Paymaster Qt suite and two functional scenarios. All
node processes belong to temporary functional-test directories.

```powershell
& {
    Set-Location 'D:\Digibyte\digibyte-fork'
    $msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
    foreach ($target in @('digibyted', 'digibyte-cli', 'test_digibyte', 'test_digibyte-qt')) {
        & $msbuild "build_msvc\$target\$target.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:QTBASEDIR=D:\Qt51510\install '/p:SolutionDir=D:\Digibyte\digibyte-fork\build_msvc\' /m:1 /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $target ($LASTEXITCODE)" }
    }
    $env:PYTHONUTF8 = '1'
    $env:DIGIBYTED = (Resolve-Path 'build_msvc\x64\Release\digibyted.exe').Path
    $env:DIGIBYTECLI = (Resolve-Path 'build_msvc\x64\Release\digibyte-cli.exe').Path
    & .\build_msvc\x64\Release\test_digibyte.exe --run_test=paymaster_wallet_store_tests,paymaster_wallet_identity_tests --log_level=test_suite --report_level=short
    if ($LASTEXITCODE -ne 0) { throw "Wallet tests failed: $LASTEXITCODE" }
    Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
    $env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw "Qt tests failed: $LASTEXITCODE" }
    python -u test/functional/test_runner.py wallet_paymaster_pool_setup.py wallet_paymaster_provider.py -j1
    if ($LASTEXITCODE -ne 0) { throw "Functional tests failed: $LASTEXITCODE" }
}
```

## Original findings requiring correction

### EC-01 — P1: AUTO can treat an unreadable existing session as a new ordinary send

Sources: `src/wallet/paymasterstore_client.cpp:191`,
`src/wallet/rpc/paymaster_send.cpp:189`, `:259`, `:290`,
`src/rpc/digidollar.cpp:2464`.

`GetSessionByRequestId` returns `false` for both NOT_FOUND and a session read
error. A tombstone read error also becomes `false`. The AUTO entry point uses
that boolean to decide whether to resume a durable payment. If the read fails,
it can reach a successful ordinary DGB-funded preflight and return `nullopt`;
`senddigidollar` then calls the ordinary transfer implementation.

Trigger: retry a previously completed or authorized AUTO request, inject a
read error/unknown version for that request's session or tombstone, and provide
another eligible DD input plus sufficient DGB. The original input reservation
does not protect these other ordinary coins. The retry can become a second
payment instead of failing closed. This finding concerns a previously existing
Paymaster session, not the intended direct-DGB behavior of a genuinely new AUTO
request.

Existing coverage: store tests reject corrupt/old session records and test
CreateOrJoinSession rollback. They do not exercise the AUTO dispatcher with
unreadable session state and independently sufficient direct funding.

Required regression: parameterize live-session and tombstone errors, malformed
and future versions, sufficient/insufficient direct DGB, and terminal/nonterminal
sessions. Invoke the real send boundary; require an actionable database/version
error, identical wallet transaction count, no new signature/broadcast, unchanged
balances and reservations. A genuine NOT_FOUND case must retain normal AUTO
behavior. Preserve the read status across the dispatch boundary; do not add a
second session store.

### EC-02 — P2: the Qt setup wizard and automatic-setup RPC have incompatible contracts

Sources: `src/qt/paymasterwidget.cpp:10527`, `:10740`, `:10757`, `:10800`,
`src/wallet/rpc/paymaster_provider.cpp:2201`.

The wizard disables an existing provider at step 1 and executes pool funding at
step 8; it persists the chosen enabled state only at step 10. A new provider is
also initially disabled. The new execution RPC refuses a disabled provider.
Consequently, a wizard setup with missing pool outputs cannot complete this
sequence. A pool that already meets its targets takes the no-funding branch and
can hide the incompatibility.

Independently, the wizard requires `executed=true`. The new durable contract
legitimately returns `accepted=true, executed=false` while waiting for funds,
index synchronization, or an approved fee opportunity. The wizard treats that
accepted command as a failure. Its result validator still accepts the old
response shape without requiring the new acceptance/fee fields.

Existing coverage: `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep` uses an
injected funding response with zero missing outputs. Other injected RPC tests
exercise UI actions without enforcing the backend's new enabled-state gate.

Required regressions: fresh disabled wallet and existing enabled wallet, both
with missing slots; accepted-but-deferred funding; partial DD-only completion;
wallet relock after acceptance; wizard retry, close and wallet switch after a
lost response. Assert exact approval, one durable plan, correct pending text,
and no unintended autostart. Align approval while disabled and execution while
enabled deliberately; do not silently enable provider operation just to make
the wizard pass. Acceptance must not be rendered as confirmed readiness.

### EC-03 — P2: recurring DGB replenishment retains the original stem-parent failure

Sources: `src/wallet/rpc/paymaster.cpp:2028`, `:2181`, `:2193`,
`src/wallet/rpc/paymaster_runtime.cpp:459`, `src/wallet/coincontrol.h:21`,
`src/node/transaction.cpp:98`.

The finite-setup path now selects confirmed DGB inputs. Recurring
`RunAutomaticDGBReplenishment` still uses default coin control with minimum
depth zero. When carrier replenishment or another wallet send has consumed the
only confirmed funding coin, safe wallet change can be stem-only. A following
DGB replenishment can sign and save a child that the ordinary mempool fee
precheck rejects as missing/spent, reproducing the original setup failure in a
different workflow.

The maintenance journal helps prevent another replacement from being created;
that does not prove prompt continuation or rebroadcast of the saved child.
Current lifecycle/replenishment tests disable Dandelion and generally fund a
mining wallet with many confirmed coins.

Required regression: enable Dandelion with an inert stem-capable peer, use one
confirmed non-pool DGB input, and simultaneously create a DD-carrier and DGB
liquidity deficit. Cover parent confirmation, stem eviction/fluff, restart and
lost RPC response. Assert exact transaction identity, bounded fees and eventual
readiness without unrelated replenishments. Reuse the confirmed-funding rule or
an equally explicit existing-parent continuation rule within Paymaster code.

### EC-04 — P2: a failed setup step can permanently block further setup and service

Sources: `src/wallet/rpc/paymaster.cpp:1269`, `:1397`,
`src/wallet/rpc/paymaster_provider.cpp:1742`, `:2175`, `:2195`,
`src/wallet/rpc/paymaster_runtime.cpp:450`.

Reconciliation moves an abandoned/conflicted setup transaction to FAILED, then
skips FAILED records on later passes. `HasPendingPoolPreparation` nevertheless
counts FAILED as pending. Cancellation only releases PLANNED records without a
transaction ID. New setup commands are rejected while pending, and automatic
provider service stops accepting new work/replenishing while setup is pending.

This makes a normal cancellation insufficient to clear the blockage. More
seriously, if a conflicting branch disappears and the exact original setup
transaction later confirms, the skipped FAILED record cannot be promoted to
complete. A conflict observation is not necessarily irreversible chain truth.
The periodic continuation keeps reporting the conflict; there is no normal
setup-level resolution path in the inspected implementation.

Required regressions: setup -> confirmation -> conflicting reorg -> reconfirm
original; abandoned signed transaction; one conflicted step with one unsigned
sibling; explicit cancellation followed by a newly approved plan. Keep saved
bytes and ambiguous obligations protected. Re-observe exact signed transactions
and define an explicit safe resolution rather than releasing signatures based
only on timeout or absence from a mempool.

### EC-05 — P2: pool retirement can lose fee accounting across a partial failure

Sources: `src/wallet/rpc/paymaster_provider.cpp:2532`, `:2586`, `:2616`,
`src/wallet/rpc/paymaster.cpp:1740`, `:1762`, `:1890`.

`rebalancepaymasterpool` commits/broadcasts DD retirement and then DGB retirement,
and only afterwards writes their POOL_RETIREMENT finance events. It does not
persist a maintenance operation containing those retirement transaction
bindings first. If the DGB step fails after DD retirement, the process exits, or
the finance write fails, an already committed retirement can lack its fee event.

Finance reconciliation refreshes existing events and reconstructs setup,
provider-commit and maintenance events. That is not reconstruction of a missing
retirement event. A retry may see already-spent inputs and a changed plan, so it
does not necessarily revisit the missing accounting write. The wallet retains
the transaction; the finding is incomplete recovery/accounting, not evidence
that the principal disappeared.

Required regression: interrupt/fail each boundary after DD commit, after DGB
commit, after pool write and before/at finance write. Restart and retry must
recover each original transaction and one fee event per transaction, with
unchanged targets and no duplicate retirement. Extend the existing durable
operation mechanism instead of adding a separate retirement database.

## Additional test gaps, without a demonstrated defect claim

| ID | Workflow / boundary | Existing protection and coverage | Additional case and invariant |
|---|---|---|---|
| G01 | DD replenishment and carrier excess withdrawal | Planner fee caps, maintenance reservations and exact-plan calls exist. | These paths still call `TransferDigiDollarMany`, which can auto-abandon a rejected wallet transaction; actual-fee checks run after commit. Inject transient oracle/policy/relay failures and test fee cap -1/equal/+1 and dust-sized DGB change. Establish that no over-cap transaction reaches commit and that rejected signed bytes have an explicit recovery policy. The conservative planner alone is not a test of that boundary; no actual cap overrun was reproduced here. |
| G02 | Finite setup crash consistency | V3/V4 codec tests, journal write/abort tests and graceful restart are present. | Kill/fail after approval persistence, after each wallet commit, after broadcast but before pool reconciliation, and between journal/pool writes. Reopen real SQLite and verify one step/tx/output set. Existing provider-final-commit rollback tests do not cover these setup transitions. |
| G03 | Concurrent setup, stop, cancel and policy changes | Work guard and wallet locks exist; concurrent quote/last-slot tests already exist. | Race execute/execute, tick/execute, cancel/commit, disable/tick, policy update/tick and unload/tick using deterministic barriers. A losing operation must not overwrite a newer journal state or create an additional signature. |
| G04 | Manual pool edits during a deferred setup | Rebalance blocks active pool reservations; pending unsigned setup is a separate durable condition. | Start a funded-partial or unfunded setup, then retire slots/change liquidity targets/release a carrier. Define whether to reject, cancel unsigned work or require a revised explicit approval. Verify that later funding does not silently undo the operator's newer pool decision. |
| G05 | Exact legacy-transaction adoption | Positive adoption of a saved DGB child is in the new regression. | Wrong wallet, DD transaction, foreign input/output, reserved pool input, duplicate/out-of-range vout, wrong amount/purpose, spent output, fee excess, conflict between preview and execute, and repeated adoption after registration. Require no mutation for every rejection and no replacement transaction. |
| G06 | Funding boundaries | Missing DGB, one funding input, normal recovery and a tiny fee cap are covered. | No DD, exact 1-DD carriers, subminimum DD change, fragmented DD, selected/reserved/locked/immature coins, balance equal to principal but below fees, and sufficient total balance with insufficient eligible confirmed balance. Diagnostics must distinguish confirmation, unlock, fee cap and genuine shortfall. |
| G07 | Confirmation and reorg lifecycle | The functional reorg test rolls back/reconfirms a payment; store tests cover successor states. | Actual chain reorgs of setup, replenishment, retirement and withdrawal, including parent/child and an already-spent successor. The maintenance-source unit test constructs valid state shapes; it does not execute the reconciliation function through a real reorg. Never expose both source and successor as spendable. |
| G08 | Full payment finalization under Dandelion | Exact-final validation explicitly checks both pools and compares witness bytes. | Run USER_PAID, public/restricted SPONSORED, retry_same and cancel_to_self with stem acceptance, delayed result, disconnect, eviction and parent reorg. Require no finality before confirmation and no premature reservation release. Do not infer this coverage from the new setup-only stem test. |
| G09 | Client AUTO, selected inputs and send-all | Two-stage Paymaster authorization, fee rounding, balance snapshots and ordinary DGB sends have focused coverage. | Vary direct funding between retries and between a preview and authorization; inject session/tombstone failures (EC-01), delayed incoming DD and an ordinary competing wallet send. Keep the intended distinction: a new AUTO request may directly pay with DGB; an existing Paymaster session must not switch to another payment. |
| G10 | Settings, readiness and scheduler | Locked autostart, disabled Paymaster, no txindex, pruning and activation gates are tested. | Deferred setup with walletbroadcast=0, network disabled, txindex temporarily behind, reindex/rescan, activation/oracle availability transitions, and unlock timeout during construction. Test `--enable-debug` lock order while callbacks run; no synchronous queue wait on the scheduler or while holding wallet locks. |
| G11 | Time, retention and bounded journals | Budget high-water, token buckets and serialization bounds are tested. | NTP rollback during reconciliation, forward jumps past expiry, 255/256/257 maintenance records, repeated unsigned cancel/reapprove, pruning older completed setup records and retry of their old plan IDs. Open signatures stay protected; bounded history must fail clearly without duplicate execution. |
| G12 | Persistence, backups and versions | Provider/client write-failure tests and one older-client-backup functional case exist. | Real disk-full/commit failure at maintenance/setup/retirement boundaries; provider restore with budget and sponsorship records older than on-chain payments; V3-only, mixed V3/V4, unknown/truncated V4. Backup rollback limits are already documented and are not solved by a rescan. Never run original and restored provider copies concurrently. |
| G13 | Discovery, private transport and malformed peers | Extensive Capacity/quote binding, cross-peer replay, lease/ABA, rate-limit, fairness and payload-bound tests exist. | Actual Tor proxy/authentication failure and reconnect at every private protocol stage, then sustained validly encoded invalid proofs while ordinary DGB and legitimate Paymaster payments run. Measure bounded CPU/RSS and progress, not only rejection counts. Existing real-Tor/load gates remain open. |
| G14 | Qt and operator observations | Wallet-switch callbacks, unlock leases, material-change confirmation, recovery and finance UI tests exist. | New accepted/pending/error setup statuses, missing acceptance/fee fields, lost execute response and later restart; keep exact approved fee limits visible. Use contract-faithful injected responses and at least one backend integration smoke test; a mock returning zero missing outputs cannot test funding. |

These gaps are combinations of existing boundaries. Reuse the current fixtures,
wallet/store fault injection and balance oracles; avoid another implementation of
coin selection, fee calculation, reservation logic or session state in tests.

## Suggested execution order and acceptance

1. Add failing regressions for EC-01 and EC-02, then correct the dispatch/UI
   contracts. Add the recurring Dandelion case EC-03 alongside the setup case.
2. Cover EC-04 and EC-05 with actual reconciliation and durable transaction
   boundaries, not only serialization or hand-assigned final states.
3. Add the focused G01-G07 cases before treating pool automation as accepted.
   Retain the existing client/protocol/security suites; they already test
   important adversarial behavior and should not be rewritten wholesale.
4. Run lifecycle/environment combinations G08-G14, the ordinary wallet/DD
   regressions and the existing sanitizer/fuzz and deployment release gates.

For every financial case record eligible balances, wallet/mempool/stempool
transaction identities, live chain outpoints, pool states, reservations, fee
ledgers and finance events before/after. Assert the expected changes exactly;
RPC success or test-case counts alone do not prove the money/state invariant.

Full builds and suites remain operator tasks under `AGENTS.md`. Use a configured
Linux/WSL checkout containing the reviewed changes, wallet/SQLite/Qt tests
enabled, freshly rebuilt binaries and isolated regtest directories. The old
prepared WSL runner pins the pre-change commit and does not test this tree.
Builds typically take tens of minutes; the full provider functional scenario and
combined Paymaster matrix can also take tens of minutes or longer.

Existing baseline commands from the checkout root (these do not add the missing
regressions above):

```bash
make -j2 -C src digibyted digibyte-cli test/test_digibyte qt/test/test_digibyte-qt
./src/test/test_digibyte --run_test='paymaster_*' --log_level=test_suite --report_level=short
env -u DIGIBYTE_QT_TEST_FUNCTION -u DIGIBYTE_QT_TEST_OUTPUT QT_QPA_PLATFORM=offscreen DIGIBYTE_QT_TEST_SUITE=PaymasterWidgetTests ./src/qt/test/test_digibyte-qt
python3 test/functional/test_runner.py p2p_paymaster.py wallet_paymaster_readiness.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_pool_setup.py wallet_paymaster_lifecycle.py wallet_paymaster_offer_selection.py wallet_paymaster_failover.py wallet_paymaster_reorg.py -j1
```

The optional `p2p_paymaster_v9_26_5_bridge.py` requires the separately documented
old binary. Require exit code zero, no unexpected skips, matching source/binary
identification and the per-case invariants. RPC `--coverage` measures method
invocation, not edge-case or branch coverage. No executable suite, live-node
experiment, new fault-injection regression or sanitizer run was performed in
this review; there is no new runtime pass claim.
