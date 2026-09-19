# Paymaster integration with DigiByte v9.26.6rc2

## Scope and base

This integration adapts Paymaster to upstream commit
`998140ec6ba951ceaea40b22ddcdc89e8fa7154e`, starting from Paymaster commit
`16da1f57f7d588fe571a10a6be4edf5316e8e250`. The local integration branch is
`integration/paymaster-v9.26.6rc2`.

Upstream is the authority for consensus, Thaw Day, wallet state, amount parsing,
and existing RPC argument positions. This is source integration, not release
acceptance. Historical v9.26.5 compatibility results do not certify this revision.

## Changes that keep the upstream surface small

- The provider console and setup wizard have moved out of `digidollartab.cpp`
  into `src/qt/paymasterwidget.cpp`. `paymasterwidget.h` exposes only the
  embedding interface. The moved implementation retains its behavior; the
  DigiDollar tab owns the widget through Qt parenting.
- Mint, redeem, positions and receive-request widgets use the release versions.
  Paymaster does not require cosmetic changes to these widgets.
- DD ownership discovery and history rendering use the release implementations.
  Paymaster-specific spendability, pool exclusions, idempotency and reservations
  remain integrated with the wallet.
- `ExtractDDAmountFromTransaction` checks the supplied transaction's hash and
  delegates to the unchanged upstream `ExtractDDAmountFromTxRef` parser. The
  original consensus callers remain unchanged.
- Existing payment-data redactions remain; routine transfer diagnostics follow
  upstream's `BCLog::DIGIDOLLAR` category.
- The mock wallet database retains upstream transaction and write callbacks,
  with the Paymaster write/commit failure hooks needed for atomicity tests.
- Existing MSVC portability support remains necessary. Newly introduced uses of
  native 128-bit arithmetic use `util::int128_t`, preserving expressions and
  bounds. This is a platform adaptation, not a Paymaster consensus rule; the
  existing cross-platform arithmetic review requirement remains open.
- MSVC project inputs are regenerated from Makefiles, including the separate
  Paymaster panel and new upstream Qt test suites. Duplicate portability
  includes in existing test sources were removed.

## Paymaster separation after integration commit 89325d45b9

- `wallet/paymasterdb.cpp` contains the unchanged 1,780-line codec block and
  the original Paymaster record keys. Only two previously file-local key names
  needed matching `extern` declarations; key bytes and record formats are unchanged.
- `PaymasterSendWidget` is one concrete Qt child. It owns the existing fee
  controls, session state, timers and test injection. The form retains editable
  fields, validation and ordinary sending. Read-only snapshots and narrow form
  presentation hooks avoid duplicated editable state. Generic dialogs/formatting,
  wallet RPC execution and confirmation guards are reused. The existing
  `DigiDollarSendWidget` translation context and control object names are preserved.
- `wallet/rpc/paymaster_send.cpp` contains free functions for options and fee
  funding. It resumes durable sessions before balance/coin selection, then either
  returns the existing Paymaster result or continues the ordinary transfer.
  There is no new public RPC, session store, database transaction or migration.
- 31 of the 133 Qt test methods moved to `PaymasterWidgetTests`; the other 102
  remain in `DigiDollarWidgetTests`. The shared wallet fixture has one definition
  in `digidollartestutil.cpp/h`. Seven Paymaster maintenance tests moved unchanged
  to `paymaster_wallet_load_tests`; ordinary wallet-load tests stay in place.
- The existing `init.cpp` LF conversion is outside this refactor. Against the
  release it accounts for a 2,419/2,411 added/deleted-line diff; ignoring line-end
  whitespace leaves eight additions. No further normalization or history rewrite
  is performed here.

The refactor follows the repository/`src` agent guidance, `CLAUDE.md`, the
DigiDollar architecture and maps, the refactoring rules in `CONTRIBUTING.md`,
`doc/developer-notes.md`, the test guides and MSVC build instructions. Mechanical
moves, interface rebinding and documentation are evaluated separately. Full
build/link and runtime acceptance remain subject to the operator gates below.

## RPC migration

The signature is:

```text
senddigidollar address amount [comment] [fee_rate] [selected_inputs] [amount_unit] [options]
```

Upstream owns argument six, `amount_unit`. Paymaster options now occupy argument
seven. A positional caller from the previous Paymaster development branch must
insert `"cents"` before its options object. Named `options` calls remain valid.
Fee caps and persisted payment amounts remain integer cents even when the RPC
request explicitly uses dollars. Qt and functional Paymaster callers have been
updated. The RPC contract test now checks that a dollars retry resumes the same
session as its equivalent cents request.

## Verification performed

- All 16 conflicted paths resolved; no unmerged index entries remain.
- Targeted MSVC `/Zs` checks cover the panel, tab, send widget, Qt widget tests,
  Qt test runner, wallet, RPC, shared mock database, transaction builder,
  validation adapter, updated arithmetic and affected consensus tests.
- Functional Python files parse successfully.
- MSVC generation is idempotent and project XML parses.
- A mechanical comparison confirms that the moved panel implementation differs
  only in its embedding interface and class naming.
- The integration diff relative to the release has no whitespace errors.
  A check relative to the older Paymaster HEAD also reports whitespace already
  present in imported upstream files; those files were not reformatted.

Syntax checks do not link binaries or execute wallet, network or Qt behavior.
Existing executables have not been used as evidence for the updated sources.

## Refactor verification and review size

Local checks for this refactor:

- MSVC v143 (14.43), C++20 `/Zs`, `/W3`, `/WX` and the repository's warning
  exclusions passed for 12 changed C++ translation units and four generated
  Qt metaobjects. These are compile checks, not a linked build or runtime test.
- Qt 5.15.10 MOC generated all four affected metaobjects. The MSVC generator
  was idempotent and all 29 discovered project XML files parsed successfully.
  New production/test sources and MOC entries are registered exactly once;
  MSVC wallet tests are also covered by the existing `*_tests.cpp` project glob.
- The full 1,780-line database codec block, all 40 record keys and all seven
  wallet maintenance test bodies matched their previous definitions.
- All 133 Qt test methods are present exactly once. Normalizing only the suite
  name and concrete-child test access reproduces every original test body.
- Both extracted RPC blocks matched after parameter/indentation normalization;
  their checks and order are unchanged. All 67 Paymaster/client-safety fields
  moved to the concrete child; the original form retains only its child pointer.
- The Qt string-literal comparison found no removed or changed literals; only
  the new child object name was added. Existing translation context and CSS
  ancestor selectors remain applicable. This does not replace visual tests.

Against the DigiByte release, the following original-file diffs shrink. Counts
are ordinary Git added/deleted lines, including comments and whitespace; moved
code still exists in the new files and is not counted as a reduction in total
implementation size.

| Original file | Before refactor | After refactor |
|---|---:|---:|
| `src/qt/digidollarsendwidget.cpp` | +4577 / -271 | +306 / -284 |
| `src/wallet/walletdb.cpp` | +2440 / -643 | +443 / -466 |
| `src/rpc/digidollar.cpp` | +390 / -27 | +145 / -36 |
| `src/qt/test/digidollarwidgettests.cpp` | +6428 / -101 | +1453 / -111 |
| `src/wallet/test/walletload_tests.cpp` | +1177 / -2 | +7 / -2 |

Across all affected production `.cpp`/`.h` files, including new files, physical
line count changes from 18,473 to 18,747 (+274). These additions establish the
headers, concrete-widget embedding, input snapshot and function boundaries;
there is no second implementation of the payment or persistence algorithms.
Affected test source/header files grow by 204 physical lines for separate test
classes, includes and the shared-fixture interface; no test body is duplicated.

At the initial refactor handoff, linking and runtime gates were pending. The
operator subsequently built fresh MSVC Release binaries and supplied the runtime
results below. Full runtime acceptance remains **pending**.

## Windows runtime follow-up (2026-09-19)

- The operator's full unit run executed 3,973 cases: 3,968 passed, three passed
  with warnings and two failed. `blockmap_tests/map_allocation_budget` also
  fails in isolation; `oracle_bundle_manager_tests/broadcast_consensus_proposal_no_spam`
  passes in isolation. These failures still require resolution or a documented
  baseline comparison; the full unit gate is not passed.
- With the test runner's default `QT_QPA_PLATFORM=minimal`, both
  `WalletTests::walletTests()` and
  `DigiDollarWidgetTests::redeemWidgetLockedWalletClickRequestsUnlock()` crash
  in `QMessageBox::showEvent()` with Windows exception `0xc0000005`.
- The installed Qt 5.15.10 source calls
  `QGuiApplication::platformNativeInterface()->nativeResourceForWindow()`
  without checking the interface pointer in `qt_getWindowsSystemMenu()`.
  The minimal integration inherits the null native interface implementation.
  Selecting the native Windows backend avoids this unsupported dialog path.
- Both individual cases complete with exit zero using
  `QT_QPA_PLATFORM=windows`. `QT_FORCE_STDERR_LOGGING=1` keeps Qt messages and
  QtTest text output in the console/log instead of the Windows debugger sink.
  This changes the test environment only; it does not change payment code.
- The subsequent operator run completed all 12 Qt suites with the Windows
  backend: 216 passed, three failed, zero skipped (including fixture and
  data-driven entries), exit 3. There was no access-violation crash.
- Two test drivers used `QMessageBox::done(Yes)` for static warning helpers.
  Those helpers return `standardButton(clickedButton())`, so the tests had not
  actually confirmed expert setup or external backup acknowledgement. They now
  click the real Yes button and retain the original state/result assertions.
- The mint persistence test used `MockableDatabase::m_pass=false`, which also
  fails reads. The Paymaster reservation guard correctly stopped coin selection
  before the intended write failure. The existing `m_refuse_write` hook now
  refuses the mint owner-key record while leaving reads available. An added
  assertion proves that the refusal was reached; the no-broadcast, empty-mempool
  and absent-position assertions remain in place. Production reservation checks
  and payment logic are unchanged.
- The Qt test executable was rebuilt and linked successfully. The three exact
  cases (`digiDollarAmountLabelsUseCurrencyPrefix`,
  `paymasterFinancesAndBackupWorkflow`,
  `mintDoesNotSendWhenTheWalletCannotSaveIt`) each pass in isolation with exit
  zero and no skips. These test-only follow-ups add 14 physical lines beyond
  the extraction snapshot above.
- The operator's full Qt rerun on 2026-09-19 completed all 12 suites with exit
  zero: 219 passed, zero failed, zero skipped and zero blacklisted (including
  fixture and data-driven entries). This includes all 45 PaymasterWidgetTests
  entries and all 110 DigiDollarWidgetTests entries. Evidence:
  `digibyte-qt-fixed-20260919-171705.log`, ending in `All tests passed.`;
  the operator also reported `Qt-Exitcode: 0`. The Windows Qt runtime gate is
  passed for this build. PowerShell's `NativeCommandError` rendering of native
  stderr did not indicate a failing Qt result in this run.
- Run Windows GUI tests in an interactive desktop session: dialogs can appear
  briefly. Overall runtime acceptance remains pending because the known unit
  failures and the functional-suite gate are still open. The successful Windows
  Qt run does not establish cross-platform acceptance.

## Extended functional run and failure analysis (2026-09-19)

The operator's extended Windows run completed with **424 entries: 384 passed,
12 failed and 28 skipped**, in 14,812 seconds. The original log is
`%TEMP%/digibyte-functional-tests.log`; retained failed-test data is under
`%TEMP%/test_runner_₿_🏃_20260919_173124`. This was a failed overall run.

### Paymaster corrections

The four failed Paymaster entries exposed RPC documentation/contract mismatches
and stale test fixtures. Corrections preserve RPC values, validation order,
reservation enforcement, transaction construction and persistent formats:

- Document `privacy_profile` on `senddigidollar` replay, and `privacy_profile`
  plus `to_address` on `requestpaymasterquote` replay. Finalized requests can
  still carry their persisted authorization details.
- Share the full `SessionToJSON` result schema between `getdigidollarsendsession`
  and `resolvepaymastersession`, including provider identity, privacy, offer,
  policy and exact amount/fee fields. The previous duplicated schema omitted
  the authorization fields once a session advanced beyond the unsigned state.
- Preserve explicit null `attempt`, `recovery`, `result_status` and
  `result_sequence`. The upstream `RPCResult` has no nullable union type. Use
  its existing per-field `skip_type_check` facility, as `getwalletinfo` does
  for polymorphic `scanning`, only on those four fields. This relaxes help-schema
  checking for those fields, not input validation or authorization checks.
  Existing unsigned-session tests assert null results; the lifecycle test also
  checks the populated attempt and signed-result types explicitly.
- Fund the RPC fixture with two confirmed 750-cent DD outputs instead of one
  1,500-cent output. Two simultaneous sessions require disjoint reservations;
  the test now asserts that explicitly. Total funding remains unchanged.
- Check the authoritative action-set rejection after signing, including the
  absence of `fallback` and an unchanged session after the rejected RPC. The
  store's independent `PAYMASTER_FALLBACK_AUTHORIZATION_MAY_EXIST` guard and
  its unit tests remain intact.
- Fix provider-pool expectations: only two 0.30 DGB slots meet the raised fee
  ceiling, so a five-slot target needs three additional slots (0.90 DGB).
  Rebalancing retains two spent 0.20 DGB entries: the later pool has 14 records,
  of which 12 are available and two are spent. A side-effect-free preview does
  not return a `pool` key; compare `getpaymasterpoolinfo` before/after instead.

The changed RPC translation units (`rpc/digidollar.cpp`,
`wallet/rpc/paymaster_client.cpp`, `wallet/rpc/paymaster_discovery.cpp`) were
compiled with the installed MSVC Release configuration and the daemon relinked.
Full library archives were assembled from all project-listed existing object
files after the targeted compiles. Python syntax and `git diff --check` passed.
No full build or full test-suite rerun was performed for these corrections.
The GUI and unit-test executables still require the operator's normal rebuild
before claiming they contain the latest RPC descriptions.

Focused reruns retain `rpcdoccheck=1` and normal test timeouts:

| Test | Latest result | Evidence under `build_msvc/` |
| --- | --- | --- |
| `wallet_paymaster_lifecycle.py --descriptors` | Passed, 26 s; runner exit 0, including the additional signed-result type assertions | `paymaster-lifecycle-fixed.log` |
| `wallet_paymaster_rpc.py --descriptors` | Passed, 33 s | `paymaster-functional-fixes-third.log` |
| `wallet_paymaster_failover.py --descriptors` | Passed, 29 s | `paymaster-functional-fixes-third.log` |
| `wallet_paymaster_provider.py --descriptors` | Passed, 48 s; runner exit 0 | `paymaster-provider-fixed.log` |

Earlier diagnostic runs in those logs contain failures preceding the final
corrections; they are not successful full-matrix results.

The separate timing diagnostic passed both selected tests (runner exit 0):

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:PYTHONUTF8 = '1'
python -u test/functional/test_runner.py wallet_digidollar_rc33_regressions.py feature_config_args.py -j1 --timeout-factor=3
$LASTEXITCODE
```

Evidence: `build_msvc/nonpaymaster-timeout-diagnostic.log`. This command changes
only test time budgets, not consensus, wallet checks or the default test setup.
The runner reported 273 seconds for both tests together.

### Other failures and relationship to Paymaster

Source comparisons use the exact upstream release commit
`998140ec6ba951ceaea40b22ddcdc89e8fa7154e`. An unchanged error path is evidence
about origin, not a substitute for running an upstream binary with the same
compiler, flags and machine load.

| Failure | Evidence and attribution |
| --- | --- |
| `feature_filelock.py` | Test expects `DigiByte Core is probably already running`; binary emits `DigiByte is probably already running`. Test, lock-error expression and `CLIENT_NAME_RAW` match upstream. This is an upstream test/message mismatch, not a Paymaster change. |
| `rpc_blockchain.py`, both transport variants | Test removes undo data while the DigiDollar stats index still needs it. Node logs `CustomAppend: DigiDollar supply index needs retained block/undo data at height 127`, followed by fatal shutdown. Test, blockchain RPC and the failing index branch are upstream code; integration changes in the index only substitute portable 128-bit arithmetic elsewhere. No Paymaster request is involved in this failure. |
| `feature_dbcrash.py` | Startup exceeds 480 s while the legacy DigiDollar health reconstruction scans P2TR UTXOs and fetches transactions. The test and complete `ScanUTXOSet` implementation match upstream. The retained log shows scanning still progressing, not a Paymaster database failure. Exact comparative runtime remains unmeasured. |
| `digidollar_testnet26_oracle_roster_rpc.py` | Mining batches of 25 testnet26/easypow blocks exceed the 120 s RPC wait; the node continues mining to height 175. Test, mining RPC, PoW and chain parameters match upstream. Both the upstream and local MSVC Release configurations disable optimization. No direct Paymaster cause identified; performance attribution needs an equivalent baseline run. |
| `wallet_digidollar_rc33_regressions.py` | Original mint RPC exceeds 30 s while consolidating 2,000 inputs in two passes (about 31.2 s). It also performs Paymaster reservation reads; their performance contribution cannot be ruled out. With the same corrected daemon, unchanged test and `--timeout-factor=3`, the complete test passes in 145 s. This establishes timing sensitivity, not Paymaster-independent performance. |
| `feature_config_args.py` | Fixed-seed expectation races startup: the log context's two-second deadline begins before node startup, while `ThreadOpenConnections` first sleeps 500 ms. The failed node is stopped only about 173 ms after that thread starts. The test/helper and relevant seed-loop code match upstream. With the same daemon and unchanged test, `--timeout-factor=3` passes in 128 s; this is a timing-sensitive test assumption, with possible startup overhead not quantified. |
| `feature_pruning.py` | Node 2 remains at height 1319 while node 0 reaches 1553; logs reject low-work headers. The headers filter matches upstream. However, the integration merge added a direct-block-submission workaround to this test, and that workaround still does not complete the reorg. Treat this as an unresolved integration-test adaptation, not as a proven unrelated upstream failure or a proven Paymaster production regression. |
| Unit: `blockmap_tests/map_allocation_budget` | 5,244,112 bytes versus 5,505,040 gives about 95.26%, exceeding the 95% assertion. Test, block-index structures and memory estimator match upstream. This is reproducible in isolation and has no Paymaster-dependent input. |
| Unit: `oracle_bundle_manager_tests/broadcast_consensus_proposal_no_spam` | Fails in the full run but passes in isolation. The unchanged broadcast function retains a static 30-second throttle across `manager.Clear()`. This explains order/time sensitivity; no Paymaster-specific modification exists in the failing method or test body. A full upstream comparison has not been run. |

Do not describe all remaining failures as unrelated to the integration.
In particular, the pruning-test adaptation and possible minting overhead remain
open. Expensive crash/pruning/testnet baseline builds and reruns are delegated
to the operator; none are marked passed on source inspection alone.

For a focused continuation after rebuilding, the operator can run the remaining
long cases from this repository root (existing Python, daemon and CLI required;
expect tens of minutes to hours):

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:PYTHONUTF8 = '1'
python -u test/functional/test_runner.py --extended feature_pruning.py feature_dbcrash.py digidollar_testnet26_oracle_roster_rpc.py -j1 --timeout-factor=3 2>&1 |
    Tee-Object -FilePath (Join-Path $env:TEMP 'digibyte-long-failure-diagnostics.log')
$diagnosticExit = $LASTEXITCODE
Write-Host "Diagnostic exit code: $diagnosticExit"
```

Success requires every selected entry to pass and exit code 0. A timeout-only
rerun does not establish causality. Compare against a separately built pinned
upstream daemon using the same compiler settings and equivalent test fixtures;
in particular, disclose the integration-only pruning workaround when comparing.
Do not reuse the old run's data directories for these fresh tests.

The 28 original skips include unsupported Windows/POSIX/USDT or signet cases,
interface-address prerequisites and unavailable historical release binaries.
The Paymaster v9.26.5 bridge and v9.26.5 wallet compatibility/in-place-upgrade
checks lacked the required historical binary; they remain unverified.

## Operator build and runtime gates

Run from the repository root in a Visual Studio developer PowerShell with the
v143 x64 toolchain, Python, the existing static Qt 5.15.10 build and installed
vcpkg dependencies. Set `QTBASEDIR` to the static Qt installation and
`PAYMASTER_VCPKG_INSTALLED` to the directory containing `x64-windows-static`.
The full build and runtime matrix are intentionally delegated to the operator.

The currently installed workspace dependencies are available at the following
paths; no package installation is required for this refactor:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:QTBASEDIR = 'D:\Qt51510\install'
$env:PAYMASTER_VCPKG_INSTALLED = 'D:\Digibyte\digibyte-fork\build_msvc\vcpkg_installed\x64-windows-static\'
python build_msvc/msvc-autogen.py
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build_msvc/digibyte.sln -m:1 -verbosity:minimal -p:Configuration=Release -p:Platform=x64 -p:QtBaseDir="$env:QTBASEDIR" -p:VcpkgInstalledDir="$env:PAYMASTER_VCPKG_INSTALLED" -p:VcpkgManifestInstall=false
$LASTEXITCODE
```

Expect a full build to take tens of minutes or longer. Success means exit zero
and fresh daemon, CLI, unit-test and Qt-test binaries. Do not run the following
against binaries left over from the earlier branch.

```powershell
.\build_msvc\x64\Release\test_digibyte.exe '--run_test=paymaster_*,walletload_tests,digidollar_amount_tests,digidollar_txbuilder_change_tests,digidollar_wallet_lock_safety_tests,digidollar_mint_cleanup_tests' --report_level=short
$LASTEXITCODE
$env:PYTHONUTF8 = '1'
python test/functional/test_runner.py -j1 wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_failover.py wallet_paymaster_reorg.py wallet_paymaster_offer_selection.py digidollar_rpc_amount_units.py
$LASTEXITCODE
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests,DigiDollarWidgetTests,DigiDollarMintRecordTests,DDTransactionRecordTests,DDTransactionTableTests,RPCNestedTests'
.\build_msvc\x64\Release\test_digibyte-qt.exe
$LASTEXITCODE
Remove-Item Env:DIGIBYTE_QT_TEST_SUITE
```

Expect minutes to tens of minutes for this focused runtime matrix. Success means
all selected tests pass without unexpected skips. Before release, also run the
broader consensus/wallet regression and Thaw Day matrix, lock-order/debug checks,
Linux arithmetic parity, and mixed-node interoperability with the official
v9.26.6rc2 daemon. Real Tor deployment and independent review remain in the
[Paymaster release gate](digidollar-paymaster-release-gate.md).

The local integration commit records source reconciliation; the runtime gates
above remain incomplete and are required before release approval.
Follow CONTRIBUTING.md for upstream review and the signed merge workflow. The
two pre-existing untracked operator documents are not part of this integration.
