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
