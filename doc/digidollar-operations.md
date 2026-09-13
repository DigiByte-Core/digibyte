# DigiDollar node operations

This guide describes the v9.26.6 development source. Mainnet, testnet26 and
signet have no Thaw Day height configured in this source. No public-network
Thaw Day activation or sustained network test has been completed for this candidate.
These instructions are not a release-readiness announcement.

## Upgrade and preserve recovery data

Use the release owner's reviewed build and verify its published checksums
when a release is available. Before upgrading, record the installed version,
network, tip height and block hash. Back up every wallet with `backupwallet`
to a protected location outside the data directory. Keep existing backups.
A chain database, transaction ID or DD address cannot replace private keys.

```bash
digibyte-cli -rpcwallet=your_wallet backupwallet "/secure/backup/your_wallet.dat"
digibyte-cli getblockchaininfo
digibyte-cli getdigidollardeploymentinfo
digibyte-cli stop
```

Use `-testnet` on each command for testnet26. Wait for the process to exit
before replacing binaries or copying a complete data directory. Preserve
wallet files, configuration, logs and retained blocks and undo data. Undo data
lets the node reverse a block when the chain changes. Start the
replacement with the same network and data directory. Check startup messages,
chain progress, loaded wallets and oracle status before resuming service.

An ordinary upgrade does not require deleting chain data or running a full
reindex. At or above a scheduled Thaw Day height, the node must establish the
matching health state and verify any previously unchecked history before
using it. Missing or unreadable required blocks need recovery; repeated
restarts or deleting indexes cannot supply missing source data. Preserve the
specific error and follow a reviewed repair plan. Do not treat incomplete
health data as zero or disable validation to get past a startup failure.

All full-node operators and block producers need the coordinated upgrade,
including services that handle only DGB. Older software may disagree with
blocks under the new rules. Once a height is distributed in binaries, changing
a notice cannot postpone it. After activation, an ordinary downgrade to old
software is not a recovery plan. If chain agreement becomes uncertain, retain
the last agreed hash and diagnostics and coordinate recovery with the release
owner. Exchanges and custodians should have a policy for holding deposits and
withdrawals during such an incident.

See the [activation guide](../DIGIDOLLAR_ACTIVATION_EXPLAINER.md) for the height
boundary and the [wallet guide](../DIGIDOLLAR_WALLET_INTEGRATION.md) for key
recovery and wallet support.

## Startup progress and interrupted recovery

Required oracle and health reconstruction finishes before RPC warmup ends.
Follow the startup log and Qt splash messages. Oracle reconstruction reports
the number of blocks checked out of its scan range. Legacy health
reconstruction reports unspent outputs checked; its total is unknown during
the scan. These are counts for the current stage, not an estimate of remaining
time. Updates are periodic, and a disk operation can delay the next update.
Normal logging includes this progress without `debug=digidollar`.

When canonical recovery is needed, its first log identifies the chain height,
block hash, genesis hash, record and rule versions, and configured DigiDollar
and accounting heights. Later messages identify the work being performed:

| Stage | Meaning |
|-------|---------|
| Verifying saved DigiDollar health totals | Reconstructing principal, collateral and vault count from the matching unspent outputs and creating transactions. |
| Checking retained DigiDollar history | Reading the required blocks and undo data before changing an unchecked chain. |
| Rewinding unchecked DigiDollar history | Moving back through history whose activated rules have not yet been checked. |
| Reconstructing DigiDollar health before activation | Establishing the accounting state from which the remaining history can be validated. |
| Verifying DigiDollar history in order | Validating the retained blocks in order under each block's applicable rules. |
| Saving DigiDollar recovery progress | Persisting the resulting chainstate and health record. |

A start with matching checked history need not visit every stage. Block stages
show completed/total counts; output scans report counts while the total is
unknown. A stage's completion or a cleared progress display does not establish
that startup succeeded. Check the final completion or error message and the
accepted height and hash. Recovery's final log includes elapsed time and the
height and hash where it completed or stopped.

`DigiDollar health totals verified` means the independently reconstructed
totals agree with the saved totals, or that no prior totals needed comparison.
`DigiDollar health totals repaired` reports the old and new principal in cents,
collateral in satoshis and open-vault count after saving the replacement.
This repairs derived accounting from available source data. It does not repair
missing or damaged block, undo or chainstate files.

To interrupt startup, press `q` on the Qt splash screen or use the service's
normal stop procedure. RPC commands may still be unavailable during warmup.
Allow the process to exit before copying data or starting it again. Cancellation
is checked between reconstruction steps; it does not interrupt an individual
disk read or an atomic database write immediately.

Cancelled oracle and legacy-health scans do not publish their partial results.
Canonical recovery can have already saved an earlier chain height. A restart
rechecks that saved state: a rewind leaves unchecked history, while a reindex
preserves only the history completed so far. The remaining blocks still need
normal validation. Do not assume an interrupted recovery kept the original
tip or that the previous stage's percentage is a durable restart position.

If startup reports unavailable required history, preserve the error, its stage,
the height and hash, and the build and configuration identity. Keep every
wallet and backup, the original data directory, logs, retained blocks and undo
files. Restore the required source data through a reviewed restore or download
procedure that keeps block files, chainstate and indexes consistent. After
repair, start normally with the same network and data directory. These startup
paths do not download or repair missing source files automatically.

Reported DigiDollar retained-history and canonical-recovery failures end the
current startup attempt without entering the generic database-rebuild prompt.
Reported oracle or late legacy-health reconstruction failures also stop
startup. A service supervisor can still launch another process, so pause its
restart loop while diagnosing a repeated failure. There is no persistent
retry marker to clear and no index to delete to unlock a retry. Repeated
restarts cannot supply missing data.

After successful startup, check `getdigidollarstats` against the accepted tip.
At and above Thaw Day, `canonical_health.ready` and `history_checked` must be
true, with the matching `block_hash`, genesis, rule versions and activation
heights. Unavailable state is not zero. `open_vault_principal` counts the
original DD amounts attached to vaults that remain open; `total_dd_supply`
reports circulating cents separately. Extra burns can make those values differ
without corrupting accounting. Below Thaw Day, use a synchronized stats index
for verified circulation because the legacy fallback scans vault amounts.
Tip health and `next_block_health` have separate heights, rules and readiness;
the next-block result also requires its applicable oracle quote. A ready tip
record alone does not establish next-block readiness at the activation boundary.

Source: [startup handling](../src/init.cpp),
[retained-history checks](../src/node/chainstate.cpp),
[canonical recovery](../src/validation.cpp),
[health reconstruction](../src/digidollar/health.cpp),
[oracle reconstruction](../src/oracle/bundle_manager.cpp),
[splash shutdown](../src/qt/splashscreen.cpp) and
[health reporting](../src/rpc/digidollar.cpp).

## Two nodes behind one router

Give each node its own peer-to-peer (P2P) listening port and matching TCP
forwarding rule.
For example, reserve these LAN addresses and configure:

| Node | LAN address | Core P2P port | Router TCP forward |
|------|-------------|---------------|--------------------|
| A | `192.168.1.10` | `12024` | external `12024` to `192.168.1.10:12024` |
| B | `192.168.1.11` | `12028` | external `12028` to `192.168.1.11:12028` |

Node A's mainnet configuration:

```ini
[main]
listen=1
port=12024
natpmp=0
upnp=0
```

Node B uses the same settings with `port=12028`. Allow its selected TCP port
in the host firewall too. Disable automatic mapping on both nodes for this
manual setup. One external address and port cannot forward to both nodes at
once. Test inbound reachability from outside the LAN; a successful local
connection does not test the router rule.

For testnet26, put the settings under `[test]`, select testnet with `testnet=1`
before all sections, and use distinct ports such as `12033` and `12034`.
If both processes run on one computer, they also need separate data directories
and distinct ports for the local remote procedure call (RPC) interface. Keep
RPC bound locally; these router rules are for P2P
traffic, and do not require exposing RPC to the internet.

If you explicitly advertise an address using `externalip=public-address:port`,
use the reachable external P2P port for that node. This setting advertises an
address; it does not create a router mapping. A router behind carrier NAT may
also need an upstream mapping or a reachable host supplied by its provider.

Source: [listening port](../src/net.cpp), [port mapping](../src/mapport.cpp)
and [network options](../src/init.cpp).

## Pruning with DigiDollar

A DigiDollar spend has to read the block that created the coin it spends. Those
blocks are always at or above the DigiDollar activation floor, so a pruned node
has to keep that whole stretch of chain.

When `-prune` is set, the node registers a retention floor at startup and keeps
every block from that height to the tip. Blocks below it may still be removed.
The floor is fixed per network and does not move when the chain reorganises.

| Network | Retention floor | Roughly |
|---------|-----------------|---------|
| Mainnet | 23,627,520 | about 568,000 blocks at the tip recorded on 12 September 2026, growing by about 5,760 a day |
| Testnet26 | 600 | almost the whole chain |
| Signet, regtest | 0 | the whole chain |

Two things follow on mainnet. A small `-prune` target such as `prune=550` cannot
be reached: the node still starts and still removes everything below the floor,
but it will not shrink past the blocks it has to keep. And if a block above the
floor is already missing when a pruned node starts, startup stops with:

```
DigiDollar state not ready: retained block history is incomplete. Restore the
required block and undo files or download the missing DigiDollar-era history.
```

Restore the missing block and undo files from a backup, or resync. Do not delete
retained history to save space.

Source: [chainstate load](../src/node/chainstate.cpp),
[prune range](../src/validation.cpp), [pruning](../src/node/blockstorage.cpp).

## Serve compact block filters

Compact block filters let supporting light wallets select blocks relevant to
their wallet. Core's peer service is disabled by default. An index alone does
not enable the peer service. Add both settings to the chosen network section:

```ini
blockfilterindex=basic
peerblockfilters=1
```

Restart the node cleanly after changing its configuration. It needs the source
blocks to build the index. Do not remove retained history to make room for the
index; first check available disk space and the node's storage settings.
Core refuses `peerblockfilters=1` without the basic filter index.

Check locally:

```bash
digibyte-cli getindexinfo
digibyte-cli getnetworkinfo
digibyte-cli getbestblockhash
# Use the returned block hash below:
digibyte-cli getblockfilter "block_hash" "basic"
```

In `getindexinfo`, check the basic filter index's `synced` flag and
`best_block_height` against the chain tip. `getnetworkinfo.localservicesnames`
should include `COMPACT_FILTERS`. Successful `getblockfilter` proves a local
lookup works. It does not prove that a phone can reach the server or retrieve
filters over P2P.

Before publishing an endpoint, use an actual supported client on another
network to connect, request filter headers and filters, and continue syncing.
Record the client and Core versions, network, endpoint, date, tested height
range and outcome. Verify the listening port and router rules above. Available
inbound capacity and client retention need separate checks; advertising a
service does not reserve a mobile client's connection. No real-client result
is claimed by this guide.

Source: [service default](../src/net_processing.h), [startup requirements](../src/init.cpp),
[P2P filter handling](../src/net_processing.cpp), [index status](../src/rpc/node.cpp)
and [local filter lookup](../src/rpc/blockchain.cpp).

## Routine logs and temporary diagnostics

Use normal logging for everyday operation. For a diagnostic session, enable
`debug=digidollar` in the selected network section or start with
`-debug=digidollar`. Routine DigiDollar and oracle messages use this category.
Useful errors, warnings and startup/recovery progress still appear without it.
Add `debug=net` only when investigating network traffic; it can be verbose.
Remove temporary debug settings when finished.

Enabling a debug category changes the default startup log-shrink behavior.
Set `shrinkdebugfile=1` explicitly if startup shrinking is wanted while debug
logging is enabled. This acts at startup, not as a continuous log-size limit.
Monitor free disk space and use the host's log-retention policy for long runs.
Keep relevant diagnostics before rotating or removing logs.

Source: [DigiDollar category](../src/logging.cpp),
[startup logging options](../src/init/common.cpp) and
[oracle logging](../src/oracle/node.cpp).
