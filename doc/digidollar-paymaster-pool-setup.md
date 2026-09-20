# Automatic finite Paymaster pool setup

`preparepaymasterpool` authorizes a finite setup, independently of recurring
liquidity maintenance and provider service startup. Its default call remains a
read-only preview. Execution persists the exact output scripts before either
transaction is created. The existing wallet stores signed transactions; the
existing maintenance ledger stores their pool purpose, fee ceiling, request
binding, policy/identity/network binding, and progress.

## Preview, authorize, observe

Example options for USER_PAID (amounts in integer satoshis or DD cents):

```json
{
  "admission_dgb_slots": 3,
  "operational_dgb_slots": 1,
  "admission_carrier_slots": 3,
  "operational_carrier_slots": 1,
  "maximum_fee_satoshis": 20000000
}
```

1. Call `preparepaymasterpool` with these options and inspect `plan_id`, pool
   principal, `maximum_fee_satoshis` and `maximum_total_fee_satoshis`.
2. Send the same options with `execute=true` and the returned `plan_id`.
3. Inspect `getpaymasterpoolinfo.preparation`. `accepted=true` acknowledges a
   durable command; `executed=true` only reports a transaction committed during
   that particular call. Acceptance alone is not evidence of a finished pool.

The default fee ceiling is **0.2 DGB per setup transaction**. A new command
creates at most one DD-carrier transaction and one DGB transaction, so its
maximum total fee is the displayed ceiling times the number of missing asset
classes. A retry cannot increase this ceiling, change targets, or replace a
saved transaction. Changed inventory produces a new preview identity.

An explicit execute call may save the reviewed approval while the provider
setting is disabled. It returns `accepted=true, executed=false` and reports
`PAYMASTER_PROVIDER_DISABLED`; it does not enable the provider or sign a new
transaction. This permits the Qt setup wizard to save funding approval before
applying the operator's chosen enabled/runtime settings. Both the wizard and
manual pool controls distinguish acceptance from execution and readiness.

The existing 30-second wallet-maintenance tick continues the command when
Paymaster and the wallet's provider setting are enabled and wallet/txindex are
synchronized. It works with `running=false`, manual service mode, and
`autostart=false`. It does not start the provider or enable recurring maintenance.
New signatures require an unlocked wallet. Disabling the provider setting
pauses setup; ordinary wallet rebroadcast of already signed transactions remains
possible, as before.

DGB setup coin selection requires confirmed inputs. DD setup also checks its
planned parents for confirmation. Therefore, a carrier transaction's stempool
change cannot immediately become the input of a DGB setup transaction whose
normal fee precheck cannot see that change. Other suitable confirmed wallet
coins may fund the DGB step. DD setup reuses the existing build-and-sign API,
checks the exact signed fee before commit, and retains rejected transactions
without invoking the ordinary DD send helper's automatic-abandon behavior.
Ordinary wallet coin-selection and Dandelion rules are unchanged.

## Waiting, failures, and cancellation

Each step reports its asset, operation ID, plan ID, state, last diagnostic,
fee ceiling, actual saved fee, and transaction ID when present.

- `pending_creation`: no transaction has been saved for that step. A missing
  balance, locked wallet, or fee exceeding the approved ceiling pauses work.
- `pending_confirmation`: a specific transaction is saved and tracked. Recovery
  reuses those exact bytes, including after an immediate broadcast rejection.
- `complete`: the transaction is confirmed and its pool outputs are reconciled.
- `conflict`: a saved transaction is abandoned or conflicted. No automatic
  replacement is authorized; the other uncompleted steps of that plan pause.
- `cancelled`: an explicitly cancelled step had no saved transaction.

Conflict and abandonment observations are not terminal journal states: every
later reconciliation checks the same saved transaction again. If it becomes
valid/confirmed after a reorg, its existing outputs are restored without a new
transaction. An output already spent by a successor is never made available
again. A still-conflicted or abandoned signed transaction continues to block
replacement setup; cancellation only removes genuinely unsigned work.

Uncreated DGB steps expose eligible confirmed funding, eligible funding including
safe unconfirmed change, required pool principal, and
`funding_shortfall_upper_bound_satoshis`. The shortfall includes the full fee
ceiling, so it is a conservative estimate rather than an exact funding demand.
Unconfirmed change is counted separately from immediately usable confirmed coins.
Additional funding can resume the original command without creating more slots
than approved.

If an execute response is lost, repeat the preview with the unchanged options.
An outstanding matching approval keeps its original plan ID and fee total; use
that ID for the retry. This does not grant a new approval or create another set
of slots. A changed request or provider policy cannot inherit the old approval.

A provider-policy change pauses execution with `PAYMASTER_POOL_POLICY_CHANGED`.
Unknown versions, unreadable records, ambiguous transaction matches and invalid
fees fail closed. No signature or reservation is discarded on a timeout.

To revoke unexecuted steps, pass the target options, the **original** `plan_id`,
`execute=true`, and `cancel=true`. Recovery first checks for wallet-saved
transactions; only genuinely unsigned steps become `cancelled`. Already saved
transactions remain tracked and may still be broadcast. The response reports
whether any unsigned step was cancelled. To change a fee ceiling or targets,
cancel eligible steps, obtain a fresh preview and explicitly authorize it.

Recurring automatic replenishment uses its existing separate consent and rolling
budgets. DGB replenishment also selects confirmed funding, so the same stem-only
parent cannot cause a signed child to fail the normal mempool fee precheck. It cannot construct replacement liquidity while finite setup is pending;
existing payment submissions can still complete. Pool retirement is rejected
while finite setup is pending; cancel eligible unsigned work before changing the
pool in a contradictory direction.

## Explicit recovery of a transaction created by the older implementation

Do not run another plain setup for the reported legacy partial failure. The
old transaction may still be valid even when it is absent from the mempool.
Do not abandon it or modify the wallet database to force another attempt.

After validating the updated build on isolated regtest, inspect the transaction
and current pool with the same wallet and data directory used for setup. The
operator must supply:

- `recover_dgb_txid`: the existing **wallet** transaction ID;
- `recover_dgb_outputs`: every intended DGB pool output as
  `{"vout": <actual index>, "purpose": "admission" | "operational"}`;
- the original desired pool totals and a sufficient finite fee ceiling.

Do not guess indices from ordering. Decode the wallet transaction and identify
its exact amounts and scripts. The remaining ordinary change is not a pool slot.
First preview these options, then execute the identical options with the returned
plan ID. The preview does not register or broadcast anything.

Adoption requires owned inputs and outputs, no Paymaster input reservation or
pool-input consumption, exact missing-slot values and counts, unspent mapped
outputs, no abandonment/conflict, and an actual fee within the ceiling. It
binds the transaction's witness hash and exact mapping to the preview. No
replacement transaction is constructed. Registration is recovered before an
attempt to broadcast the already saved transaction; outputs remain unavailable
for provider service until confirmed.

For the 2026-09-20 incident, the candidate is
`b9c9b9ca0e18a8a8b9e59edb8e93b1cbaf3a99f4f532be31d3fea2ef064b7f30`.
The already registered DD carriers must remain in the pool. Their confirmed
change is sufficient according to the supplied diagnostic snapshot, but current
transaction, spend, and registration state must be checked again by the operator.
No command in development has been run against that existing testnet node.

## Persistence and verification

The wallet maintenance record advances from V3 to V4. V3 records remain readable
without gaining setup authority. V4 adds two setup kinds plus authorization,
request binding and diagnostic fields. Older binaries cannot interpret V4
records; do not downgrade a wallet after this build writes such records. There is no second
journal, transaction-byte copy, service thread or RPC endpoint.

Run from the root of an already configured Linux checkout containing these
changes, with wallet, SQLite and tests enabled. The prepared WSL runner still
pins commit `354b711f92`; that older snapshot does **not** contain this fix.
After these changes are committed, update its pinned revision before using it
for this acceptance run. Do not run stale binaries against the new tests.

Build first (typically tens of minutes; the focused tests take minutes):

```bash
make -j2 -C src digibyted digibyte-cli test/test_digibyte
```

Focused tests are registered in the existing suites. Require exit code zero and
no unexpected skips for each command:

```bash
./src/test/test_digibyte --run_test=paymaster_provider_tests,paymaster_wallet_identity_tests --log_level=test_suite --report_level=short
python3 test/functional/test_runner.py wallet_paymaster_pool_setup.py wallet_paymaster_provider.py wallet_paymaster_rpc.py wallet_paymaster_lifecycle.py wallet_paymaster_failover.py wallet_paymaster_offer_selection.py -j1
```

Use freshly rebuilt binaries. The functional regression intentionally enables
Dandelion and uses isolated regtest wallets and an inert stem-capable peer. It
covers a single DGB funding input, deferred child creation, restart, identical
retry, changed request rejection, missing funding, disabling/re-enabling setup,
wallet locking, fee limits, unsigned cancellation, and explicit legacy adoption.
Existing funded fixtures share a bounded confirmation helper so the ordinary
provider, offer-selection, failover and legacy-bridge tests also allow a second
setup transaction after confirmation of its parent. The optional bridge test
requires its separately documented v9.26.5 binary.
Unit coverage checks V3/V4 compatibility, authorization, wrong assets, amount
bounds, write failure, transaction abort and unknown record versions.

Full builds and these executable tests are operator verification steps. Static
syntax checks alone do not establish that the runtime matrix passed.

Local verification on 2026-09-20: targeted MSVC syntax checks passed for the
changed C++ translation units and the wallet database codec; the Qt check used
the installed compatible MSVC 14.43 toolset. Python AST checks passed for all
seven affected scripts, the new test imports through `--help`, and
`git diff --check` passed. No newly rebuilt executable tests have run. The
optional Python flake8 check was not run because that module is not installed
in the current Windows Python environment.
