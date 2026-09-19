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

## Operator build and runtime gates

Run from the repository root in a Visual Studio developer PowerShell with the
v143 x64 toolchain, Python, the existing static Qt 5.15.10 build and installed
vcpkg dependencies. Set `QTBASEDIR` to the static Qt installation and
`PAYMASTER_VCPKG_INSTALLED` to the directory containing `x64-windows-static`.
The full build and runtime matrix are intentionally delegated to the operator.

```powershell
python build_msvc/msvc-autogen.py
msbuild build_msvc/digibyte.sln -m:1 -verbosity:minimal -p:Configuration=Release -p:Platform=x64 -p:QtBaseDir="$env:QTBASEDIR" -p:VcpkgInstalledDir="$env:PAYMASTER_VCPKG_INSTALLED" -p:VcpkgManifestInstall=false
$LASTEXITCODE
```

Expect a full build to take tens of minutes or longer. Success means exit zero
and fresh daemon, CLI, unit-test and Qt-test binaries. Do not run the following
against binaries left over from the earlier branch.

```powershell
.\src\test_digibyte.exe '--run_test=paymaster_*,digidollar_amount_tests,digidollar_txbuilder_change_tests,digidollar_wallet_lock_safety_tests,digidollar_mint_cleanup_tests' --report_level=short
$LASTEXITCODE
$env:PYTHONUTF8 = '1'
python test/functional/test_runner.py -j1 wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_failover.py wallet_paymaster_reorg.py wallet_paymaster_offer_selection.py digidollar_rpc_amount_units.py
$LASTEXITCODE
$env:DIGIBYTE_QT_TEST_SUITE = 'DigiDollarWidgetTests,DigiDollarMintRecordTests,DDTransactionRecordTests,DDTransactionTableTests,RPCNestedTests'
.\src\test_digibyte-qt.exe
$LASTEXITCODE
Remove-Item Env:DIGIBYTE_QT_TEST_SUITE
```

Expect minutes to tens of minutes for this focused runtime matrix. Success means
all selected tests pass without unexpected skips. Before release, also run the
broader consensus/wallet regression and Thaw Day matrix, lock-order/debug checks,
Linux arithmetic parity, and mixed-node interoperability with the official
v9.26.6rc2 daemon. Real Tor deployment and independent review remain in the
[Paymaster release gate](digidollar-paymaster-release-gate.md).

The local integration commit records source reconciliation; the full build
and runtime gates above remain pending and are required before release approval.
Follow CONTRIBUTING.md for upstream review and the signed merge workflow. The
two pre-existing untracked operator documents are not part of this integration.
