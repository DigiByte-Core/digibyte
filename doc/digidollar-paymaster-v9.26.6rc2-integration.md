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

Linking and runtime gates remain pending for the refactor handoff.
The compile and structural checks above do not establish release acceptance.

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
