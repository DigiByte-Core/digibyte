# DigiByte Core v9.26.6 — development notes

This is an unfinished release candidate. Mainnet, testnet26 and signet Thaw Day
heights remain disabled in this source. No public Thaw Day activation or soak
test has been completed for this candidate. Final release verification,
coordinated heights and distribution are still pending.

## Coordinated upgrade

Thaw Day selects the new DigiDollar validation rules at one block height per
network. The candidate block's own height controls validation, including replay
and reorganizations. Installing the software does not enable those rules early.
All full-node operators and block producers need the upgrade information,
including services that only handle DGB. Older software may disagree after the
transition. The [activation guide](../../DIGIDOLLAR_ACTIVATION_EXPLAINER.md)
explains tip versus next-block status and the separate original DigiDollar
activation height.

Before an upgrade, back up every wallet, keep existing backups, shut down
cleanly and preserve retained block/undo data. An ordinary upgrade does not
require deleting chain data or a full reindex. Missing recovery data needs a
specific repair plan. After activation, an ordinary downgrade to old software
is not a recovery procedure. Follow [node operations](../digidollar-operations.md).

## Accounting and protection status

Circulating DD remains issuance minus actual burns. Open-vault principal is
reported separately. At and above Thaw Day, health uses the original amounts
attached to vaults that remain open. Extra burns can leave fewer circulating
tokens than open-vault principal. With the same collateral, this can lower
health, prolong emergency mint restrictions or increase the DD needed to
redeem. Activation itself does not create tokens or change wallet balances.

`getdigidollarstats` exposes canonical accounting and readiness separately from
`total_dd_supply`. Tip health and `next_block_health` can use different rules
when the tip is one block below Thaw Day. Unknown state is not a zero balance.
Before Thaw Day, use a synchronized stats index for circulating-supply
reporting: the legacy fallback without that index still scans vault amounts.
At and above Thaw Day, fallback token reporting is separate from principal.
See the [architecture guide](../../DIGIDOLLAR_ARCHITECTURE.md).

At Thaw Day, mints use the chain-derived volatility reference. Transfers and
redemptions no longer use the old volatility freezes there. Other oracle,
health, timelock, burn and fee requirements remain. A large price move can
still restrict minting, with no exact wall-clock recovery promise.

## Mint wallet support and oracle names

Qt checks mint-wallet capability before either confirmation dialog. RPC mint
checks before unlock/key derivation. Minting needs a descriptor wallet with
private keys, HD support, a Taproot receiving descriptor and a bech32 change
descriptor. Supported encrypted wallets keep the normal unlock flow. The
[wallet guide](../../DIGIDOLLAR_WALLET_INTEGRATION.md) describes unsupported
wallets and backup/recovery choices without promising automatic migration.

Oracle names come from each network's roster. Mainnet ID 0 displays
`DigiByte.Io Oracle` and ID 11 displays `Crypto Corner Shop`. The change is
local display metadata; IDs, keys, ordering, signatures and quorum are unchanged.

## Operator guidance

The [operations guide](../digidollar-operations.md) covers distinct ports for
nodes behind one router and the two settings needed to serve compact filters.
Local filter availability still needs a real-client reachability and sync
check before an endpoint is advertised as tested.

Routine DigiDollar and oracle messages require `-debug=digidollar`. Useful
errors, warnings and startup/recovery progress remain visible without that
category. `shrinkdebugfile=1` acts at startup; it is not continuous log rotation.
Use temporary debug settings and monitor disk space.

External mobile wallet, explorer, backend and miner/pool reports need their
own version-specific investigation. These notes do not claim that those
projects have shipped fixes or that their tickets are resolved.
