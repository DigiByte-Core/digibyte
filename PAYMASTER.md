# DigiDollar Paymaster: developer starting point

The v9.26.6rc2 integration, RPC migration and pending build/runtime gates are
recorded in the [integration notes](doc/digidollar-paymaster-v9.26.6rc2-integration.md).

The Paymaster feature lets a wallet transfer DigiDollar (DD) without owning DGB
for the miner fee. A separate provider contributes DGB inputs to the same
transaction. The user signs its DD inputs, the provider signs its own inputs,
and the result is an ordinary `DD_TX_TRANSFER` validated by existing DigiDollar
rules. A provider may sponsor the fee or charge an explicitly authorized DD
service fee.

This is the entry point for reviewing `feature/digidollar-paymaster-v1`.
Documentation was reconciled on **2026-09-11** against source commit
**`bd270044c1`**. The feature is implemented in this branch; release approval,
an independent review, and a real Tor deployment check remain outstanding in
the recorded release gate. Updating documentation does not renew old test
results or establish that a release candidate has passed.

## Read these documents in order

| Document | What it answers |
|---|---|
| [Implementation reference](doc/digidollar-paymaster-implementation.md) | How does the current code work? Transaction example, protocol sequence, trust boundaries, state and persistence rules, and source map. |
| [Implementation plan](DIGIDOLLAR_PAYMASTER_PLAN.md) | Why was the work divided this way? Delivered work packages, review order, remaining work, and acceptance expectations. |
| [Hardening plan](DIGIDOLLAR_PAYMASTER_HARDENING_PLAN.md) | What must hold when either remote party is malicious? Required controls and their implementation/test locations. |
| [Operator and RPC guide](doc/digidollar-paymaster.md) | How are clients and providers configured and used? Includes fee selection, liquidity, finance, and recovery. |
| [Release gate](doc/digidollar-paymaster-release-gate.md) | Which checks have dated evidence, which still need a release-build rerun, and what remains open? |
| [Design specification](DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md) | What are the current architectural decisions, constraints, and 28 numbered V1 acceptance criteria? |

The implementation reference describes the inspected source. The release gate
records verification evidence. The design specification preserves criterion
numbering while updating the requirements to the current implementation.
Changes from the initial design are listed explicitly in the reference. These
documents are not evidence of an upstream merge or a production release.

## Why this involves more than a fee helper

The ordinary wallet path selects and signs its own DD and DGB inputs. A
Paymaster payment crosses two wallets and can stop after one wallet has signed
but before either party knows whether the transaction was broadcast. Supporting
that safely requires explicit transaction construction, selective signing,
durable input reservations, exact retries, and confirmation-aware recovery.

Public provider discovery adds identity and liquidity proofs, a bounded P2P
directory, and a separate direct session. Provider operation adds finite fee
budgets, pool maintenance, and restart handling. RPC and Qt expose the same
wallet authority and state. Tests cover those boundaries and their failure
cases, which explains the spread across networking, wallet, GUI, and build
files.

## Review the branch in bounded pieces

The inspected branch has merge base
`16159311b34449cd970871bcd1532561b0d92cdd` with the locally available upstream
`develop`. Recompute the base if reviewing a later revision.

```sh
git log --reverse --oneline 16159311b34449cd970871bcd1532561b0d92cdd..HEAD
git diff --stat 16159311b34449cd970871bcd1532561b0d92cdd...HEAD
git diff 16159311b34449cd970871bcd1532561b0d92cdd...HEAD -- src/paymaster
git diff 16159311b34449cd970871bcd1532561b0d92cdd...HEAD -- src/wallet
```

| Review slice | Initial implementation commits | Review focus |
|---|---|---|
| Build and portability | `15b5ebcd8e` | MSVC/static Qt support and arithmetic portability. |
| Protocol and peers | `3883c3a7ca` | Deterministic templates, protocol records, discovery, direct transport and bounded queues. |
| Wallet and RPC | `652ff3a3fc` | Local authorization, atomic persistence, restart and recovery. |
| Qt | `6de567e570` | Explicit confirmation, asynchronous operations, shared Core state. |
| Tests | `c046701d05` | Positive transfers and malicious-message, failure, and lifecycle scenarios. |
| Initial documentation | `be7a3acbf4` | Original architectural and operational explanation. |

These are navigation anchors, not an exhaustive list: later commits add
liquidity maintenance, finance, exact-outflow payments, compatibility coverage,
persistence hardening, RPC/store file splits, and Qt fixes. Review the final
diff for each slice as well as its initial commit.

The Paymaster protocol does not require new consensus rules. The broader
branch nevertheless **does modify consensus source files**: the portability
work replaces native `__int128` uses in DCA, ERR, and volatility calculations
with the types in [src/util/int128.h](src/util/int128.h). Review arithmetic
equivalence and platform behavior separately. “No Paymaster consensus change”
must not be used to skip these changes.

## What the first review should establish

1. Every outgoing DD/DGB amount and destination matches local authority before
   signing, including fees, change, carriers, and recovery outputs.
2. Capacity is authenticated before the client discloses its payment intent.
3. A crash or retry cannot create a second authorization, free signed inputs,
   overspend a provider budget, or replace an exact committed transaction.
4. Mempool acceptance, confirmation, and reorganization are represented
   consistently in the wallet, RPC, and GUI.
5. The release gate is tied to the revision and binaries being proposed for
   release, with real-Tor and independent-review results recorded separately.

Start code reading with the component table in the implementation reference.
For a finer inventory, use the
[Paymaster section of the repository map](REPO_MAP_DIGIDOLLAR.md#paymaster-network).
