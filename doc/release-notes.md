26.2 Release Notes
==================

DigiByte Core version 26.2 is now available from:

  <https://digibytecore.org/bin/digibyte-core-26.2/>

This release includes new features, various bug fixes and performance
improvements, as well as updated translations.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/digibyte/digibyte/issues>

To receive security and update notifications, please subscribe to:

  <https://digibytecore.org/en/list/announcements/join/>

How to Upgrade
==============


DigiDollar RPC changes in v9.26.6
---------------------------------

### Explicit amount units (`amount_unit`) — compatibility change

`senddigidollar`, `sendmanydigidollar`, `redeemdigidollar` and the read-only
`getredemptioninfo` no longer guess whether an amount is cents or dollars from
the shape of the number. Previously `10000` meant 10,000 cents ($100.00) while
`10000.00` meant $10,000.00, so a habitual decimal point requested one hundred
times the intended amount.

Each of these RPCs takes a new optional trailing string argument `amount_unit`
with the values `"cents"` or `"dollars"`:

- **No `amount_unit` and an integer** (`5000`, `"5000"`): cents, exactly as
  before. Existing integer-cents callers need no change.
- **No `amount_unit` and a decimal point** (`"50.00"`, `50.5`): rejected with
  error `-8` and the message
  `ambiguous amount: pass amount_unit=cents or amount_unit=dollars ...`.
  Nothing is sent, redeemed or changed. Callers that relied on decimal input
  meaning dollars must add `amount_unit="dollars"`.
- **`amount_unit="cents"`**: the amount must be an integer; `"10.00"` is
  rejected.
- **`amount_unit="dollars"`**: at most two decimal places; `"12.34"` is 1,234
  cents, `"12"` is 1,200 cents, `"12.345"` is rejected.

Amounts are parsed as plain decimal numbers with checked integer arithmetic
(never floating point): signs, whitespace, exponents (`1e3`), thousands
separators and leading zeros are rejected. The $100,000 per-request cap on
`senddigidollar`, per recipient on `sendmanydigidollar`, and on the
`redeemdigidollar` principal is unchanged and applies under either unit; it
never limits the emergency-redemption burn a wallet computes from a vault.

For `sendmanydigidollar` one `amount_unit` applies to every recipient, and a
rejected amount names the recipient it came from. The `min_amount` filter of
`listdigidollarpositions` and the `min_balance` filter of
`listdigidollaraddresses` share the parser and gained the same optional
`amount_unit` argument. Two smaller changes come with that: a negative
`min_amount` or `min_balance` used to be accepted and then ignored, and is now
rejected like any other negative amount; and `getredemptioninfo` now applies
the same $100,000 limit to `dd_amount` that `redeemdigidollar` applies, so
checking a redemption and performing it give the same answer.

Positional order of the new argument: `senddigidollar <address> <amount>
[comment] [fee_rate] [selected_inputs] [amount_unit]`,
`sendmanydigidollar <dummy> <amounts> [comment] [selected_inputs] [amount_unit]`,
`redeemdigidollar <position_id> <dd_amount> [redemption_address] [fee_rate]
[amount_unit]`, `getredemptioninfo <position_id> [dd_amount] [amount_unit]`.
Named arguments (`amount_unit=dollars`) work with `digibyte-cli -named` and
JSON-RPC named parameters. `mintdigidollar` is unchanged: it has always taken
integer cents only.

### Redemption fee coins are chosen from the real transaction size

`redeemdigidollar` used to pick DGB coins for the fee once, against a fixed
400-byte size guess, and then fail with `Insufficient fee inputs for DD
redemption fee` when the real transaction (which grows with every fee coin)
cost more. Wallets whose DGB was split into many small coins could not redeem
at all. The fee coins are now selected from the projected size of the
transaction they produce, re-selecting a bounded number of times until they
cover it. The 0.1 DGB DigiDollar minimum fee and the 0.35 DGB/kB DigiDollar
fee rate are unchanged. If a wallet holds only coins so small that each adds
more fee than value, the RPC now says so and advises consolidating DGB
coins.

### Leftover DGB no longer follows a redemption address you supply

`redeemdigidollar` sends the returned collateral to `redemption_address` when
you give one, and that address may belong to someone else, such as an exchange
deposit address. The DGB left over after the fee went to a change address from
your own wallet, except in one case: if the wallet could not produce a change
address, the leftover fell back to the collateral address and left the wallet
for good. The redemption now stops with a clear error in that case. Redemptions
that do not name a `redemption_address` are unaffected, and so is every
redemption where the wallet can produce a change address.


DigiDollar Oracle Phase 3: MuSig2 Aggregate Signatures
-------------------------------------------------------

Phase 3 of the DigiDollar oracle system introduces MuSig2 (BIP-327) aggregate
signatures, replacing the individual per-oracle Schnorr signatures used in
Phase 2. This reduces on-chain oracle data from ~277 bytes (Phase 2, 4 oracles)
to ~84 bytes (Phase 3, 17 oracles) by combining all participant signatures into
a single 64-byte aggregate signature with a compact participation bitmap.

### Activation Heights

- **Mainnet**: TBD (will be set after final testnet validation)
- **Testnet**: Block 1,000
- **Regtest**: Block 10

### Bundle Format (v0x03)

The new v0x03 on-chain format is:
`OP_RETURN OP_ORACLE <0x03> <bitmap_len> <bitmap> <price_8B> <timestamp_8B> <aggregate_sig_64B>`

### Version Gating

v0x03 bundles are rejected before the Phase 3 activation height on each network.
The `nDigiDollarPhase3Height` consensus parameter controls activation. Nodes
running this version will correctly parse and validate v0x03 bundles once Phase 3
activates, while continuing to accept v0x01 and v0x02 bundles from earlier phases.


Performance Improvements
--------------

Validation speed and network propagation performance have been greatly
improved, leading to much shorter sync and initial block download times.

- The script signature cache has been reimplemented as a "cuckoo cache",
  allowing for more signatures to be cached and faster lookups.
- Assumed-valid blocks have been introduced which allows script validation to
  be skipped for ancestors of known-good blocks, without changing the security
  model. See below for more details.
- In some cases, compact blocks are now relayed before being fully validated as
  per BIP152.
- P2P networking has been refactored with a focus on concurrency and
  throughput. Network operations are no longer bottlenecked by validation. As a
  result, block fetching is several times faster than previous releases in many
  cases.
- The UTXO cache now claims unused mempool memory. This speeds up initial block
  download as UTXO lookups are a major bottleneck there, and there is no use for
  the mempool at that stage.


Manual Pruning
--------------

DigiByte Core has supported automatically pruning the blockchain since 0.11. Pruning
the blockchain allows for significant storage space savings as the vast majority of
the downloaded data can be discarded after processing so very little of it remains
on the disk.

Manual block pruning can now be enabled by setting `-prune=1`. Once that is set,
the RPC command `pruneblockchain` can be used to prune the blockchain up to the
specified height or timestamp.

`getinfo` Deprecated
--------------------

The `getinfo` RPC command has been deprecated. Each field in the RPC call
has been moved to another command's output with that command also giving
additional information that `getinfo` did not provide. The following table
shows where each field has been moved to:

|`getinfo` field   | Moved to                                  |
|------------------|-------------------------------------------|
`"version"`	   | `getnetworkinfo()["version"]`
`"protocolversion"`| `getnetworkinfo()["protocolversion"]`
`"walletversion"`  | `getwalletinfo()["walletversion"]`
`"balance"`	   | `getwalletinfo()["balance"]`
`"blocks"`	   | `getblockchaininfo()["blocks"]`
`"timeoffset"`	   | `getnetworkinfo()["timeoffset"]`
`"connections"`	   | `getnetworkinfo()["connections"]`
`"proxy"`	   | `getnetworkinfo()["networks"][0]["proxy"]`
`"difficulty"`	   | `getblockchaininfo()["difficulty"]`
`"testnet"`	   | `getblockchaininfo()["chain"] == "test"`
`"keypoololdest"`  | `getwalletinfo()["keypoololdest"]`
`"keypoolsize"`	   | `getwalletinfo()["keypoolsize"]`
`"unlocked_until"` | `getwalletinfo()["unlocked_until"]`
`"paytxfee"`	   | `getwalletinfo()["paytxfee"]`
`"relayfee"`	   | `getnetworkinfo()["relayfee"]`
`"errors"`	   | `getnetworkinfo()["warnings"]`

ZMQ On Windows
--------------

Previously the ZeroMQ notification system was unavailable on Windows
due to various issues with ZMQ. These have been fixed upstream and
now ZMQ can be used on Windows. Please see [this document](https://github.com/digibyte-core/digibyte/blob/master/doc/zmq.md) for
help with using ZMQ in general.

Nested RPC Commands in Debug Console
------------------------------------

The ability to nest RPC commands has been added to the debug console. This
allows users to have the output of a command become the input to another
command without running the commands separately.

The nested RPC commands use bracket syntax (i.e. `getwalletinfo()`) and can
be nested (i.e. `getblock(getblockhash(1))`). Simple queries can be
done with square brackets where object values are accessed with either an 
array index or a non-quoted string (i.e. `listunspent()[0][txid]`). Both
commas and spaces can be used to separate parameters in both the bracket syntax
and normal RPC command syntax.

Network Activity Toggle
-----------------------

A RPC command and GUI toggle have been added to enable or disable all p2p
network activity. The network status icon in the bottom right hand corner 
is now the GUI toggle. Clicking the icon will either enable or disable all
p2p network activity. If network activity is disabled, the icon will 
be grayed out with an X on top of it.

Additionally the `setnetworkactive` RPC command has been added which does
the same thing as the GUI icon. The command takes one boolean parameter,
`true` enables networking and `false` disables it.

Out-of-sync Modal Info Layer
----------------------------

When DigiByte Core is out-of-sync on startup, a semi-transparent information
layer will be shown over top of the normal display. This layer contains
details about the current sync progress and estimates the amount of time
remaining to finish syncing. This layer can also be hidden and subsequently
unhidden by clicking on the progress bar at the bottom of the window.

Support for JSON-RPC Named Arguments
------------------------------------

Commands sent over the JSON-RPC interface and through the `digibyte-cli` binary
can now use named arguments. This follows the [JSON-RPC specification](http://www.jsonrpc.org/specification)
for passing parameters by-name with an object.

`digibyte-cli` has been updated to support this by parsing `name=value` arguments
when the `-named` option is given.

Some examples:

    src/digibyte-cli -named help command="help"
    src/digibyte-cli -named getblockhash height=0
    src/digibyte-cli -named getblock blockhash=000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f
    src/digibyte-cli -named sendtoaddress address="(snip)" amount="1.0" subtractfeefromamount=true

The order of arguments doesn't matter in this case. Named arguments are also
useful to leave out arguments that should stay at their default value. The
rarely-used arguments `comment` and `comment_to` to `sendtoaddress`, for example, can
be left out. However, this is not yet implemented for many RPC calls, this is
expected to land in a later release.

The RPC server remains fully backwards compatible with positional arguments.

Opt into RBF When Sending
-------------------------

A new startup option, `-walletrbf`, has been added to allow users to have all
transactions sent opt into RBF support. The default value for this option is
currently `false`, so transactions will not opt into RBF by default. The new
`bumpfee` RPC can be used to replace transactions that opt into RBF.

Sensitive Data Is No Longer Stored In Debug Console History
-----------------------------------------------------------

The debug console maintains a history of previously entered commands that can be
accessed by pressing the Up-arrow key so that users can easily reuse previously
entered commands. Commands which have sensitive information such as passphrases and
private keys will now have a `(...)` in place of the parameters when accessed through
the history.

Retaining the Mempool Across Restarts
-------------------------------------

The mempool will be saved to the data directory prior to shutdown
to a `mempool.dat` file. This file preserves the mempool so that when the node
restarts the mempool can be filled with transactions without waiting for new transactions
to be created. This will also preserve any changes made to a transaction through
commands such as `prioritisetransaction` so that those changes will not be lost.

Final Alert
-----------

The Alert System was [disabled and deprecated](https://digibyte.org/en/alert/2016-11-01-alert-retirement) in DigiByte Core 0.12.1 and removed in 0.13.0. 
The Alert System was retired with a maximum sequence final alert which causes any nodes
supporting the Alert System to display a static hard-coded "Alert Key Compromised" message which also
prevents any other alerts from overriding it. This final alert is hard-coded into this release
so that all old nodes receive the final alert.

GUI Changes
-----------

 - After resetting the options by clicking the `Reset Options` button 
   in the options dialog or with the `-resetguioptions` startup option, 
   the user will be prompted to choose the data directory again. This 
   is to ensure that custom data directories will be kept after the 
   option reset which clears the custom data directory set via the choose 
   datadir dialog.

 - Multiple peers can now be selected in the list of peers in the debug 
   window. This allows for users to ban or disconnect multiple peers 
   simultaneously instead of banning them one at a time.

 - An indicator has been added to the bottom right hand corner of the main
   window to indicate whether the wallet being used is a HD wallet. This
   icon will be grayed out with an X on top of it if the wallet is not a
   HD wallet.

Low-level RPC changes
----------------------

 - `importprunedfunds` only accepts two required arguments. Some versions accept
   an optional third arg, which was always ignored. Make sure to never pass more
   than two arguments.

 - The first boolean argument to `getaddednodeinfo` has been removed. This is 
   an incompatible change.

 - RPC command `getmininginfo` loses the "testnet" field in favor of the more
   generic "chain" (which has been present for years).

 - A new RPC command `preciousblock` has been added which marks a block as
   precious. A precious block will be treated as if it were received earlier
   than a competing block.

 - A new RPC command `importmulti` has been added which receives an array of 
   JSON objects representing the intention of importing a public key, a 
   private key, an address and script/p2sh

 - Use of `getrawtransaction` for retrieving confirmed transactions with unspent
   outputs has been deprecated. For now this will still work, but in the future
   it may change to only be able to retrieve information about transactions in
   the mempool or if `txindex` is enabled.

 - A new RPC command `getmemoryinfo` has been added which will return information
   about the memory usage of DigiByte Core. This was added in conjunction with
   optimizations to memory management. See [Pull #8753](https://github.com/digibyte-core/digibyte/pull/8753)
   for more information.

 - A new RPC command `bumpfee` has been added which allows replacing an
   unconfirmed wallet transaction that signaled RBF (see the `-walletrbf`
   startup option above) with a new transaction that pays a higher fee, and
   should be more likely to get confirmed quickly.

HTTP REST Changes
-----------------

 - UTXO set query (`GET /rest/getutxos/<checkmempool>/<txid>-<n>/<txid>-<n>
   /.../<txid>-<n>.<bin|hex|json>`) responses were changed to return status 
   code `HTTP_BAD_REQUEST` (400) instead of `HTTP_INTERNAL_SERVER_ERROR` (500)
   when requests contain invalid parameters.

Minimum Fee Rate Policies
-------------------------

Since the changes in 0.12 to automatically limit the size of the mempool and improve the performance of block creation in mining code it has not been important for relay nodes or miners to set `-minrelaytxfee`. With this release the following concepts that were tied to this option have been separated out:
- incremental relay fee used for calculating BIP 125 replacement and mempool limiting. (1000 satoshis/kB)
- calculation of threshold for a dust output. (effectively 3 * 1000 satoshis/kB)
- minimum fee rate of a package of transactions to be included in a block created by the mining code. If miners wish to set this minimum they can use the new `-blockmintxfee` option.  (defaults to 1000 satoshis/kB)

The `-minrelaytxfee` option continues to exist but is recommended to be left unset.
=======
If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/DigiByte-Qt` (on macOS)
or `digibyted`/`digibyte-qt` (on Linux).

Upgrading directly from a version of DigiByte Core that has reached its EOL is
possible, but it might take some time if the data directory needs to be migrated. Old
wallet versions of DigiByte Core are generally supported.

Compatibility
==============

DigiByte Core is supported and extensively tested on operating systems
using the Linux kernel, macOS 11.0+, and Windows 7 and newer.  DigiByte
Core should also work on most other Unix-like systems but is not as
frequently tested on them.  It is not recommended to use DigiByte Core on
unsupported systems.

Notable changes
===============

### Script

- #29853: sign: don't assume we are parsing a sane TapMiniscript

### P2P and network changes

- #29691: Change Luke Dashjr seed to dashjr-list-of-p2p-nodes.us
- #30085: p2p: detect addnode cjdns peers in GetAddedNodeInfo()

### RPC

- #29869: rpc, bugfix: Enforce maximum value for setmocktime
- #28554: bugfix: throw an error if an invalid parameter is passed to getnetworkhashps RPC
- #30094: rpc: move UniValue in blockToJSON
- #29870: rpc: Reword SighashFromStr error message

### Build

- #29747: depends: fix mingw-w64 Qt DEBUG=1 build
- #29985: depends: Fix build of Qt for 32-bit platforms with recent glibc
- #30151: depends: Fetch miniupnpc sources from an alternative website
- #30283: upnp: fix build with miniupnpc 2.2.8

### Misc

- #29776: ThreadSanitizer: Fix #29767
- #29856: ci: Bump s390x to ubuntu:24.04
- #29764: doc: Suggest installing dev packages for debian/ubuntu qt5 build
- #30149: contrib: Renew Windows code signing certificate

Credits
=======

Thanks to everyone who directly contributed to this release:

- Antoine Poinsot
- Ava Chow
- Cory Fields
- dergoegge
- fanquake
- glozow
- Hennadii Stepanov
- Jameson Lopp
- jonatack
- laanwj
- Luke Dashjr
- MarcoFalke
- nanlour
- willcl-ark

As well as to everyone that helped with translations on
[Transifex](https://www.transifex.com/digibyte/digibyte/).
