DigiByte Core version 9.26.6

> **THIS IS A DEVELOPMENT CANDIDATE, NOT THE FINISHED RELEASE.**
>
> No public network has a Thaw Day height set in this source. Every network ships with the field disabled, and
> the option that would set one is refused on any public network. So installing this build activates nothing.
>
> That is correct for testing and wrong for distribution. The plan requires the agreed heights to be in the
> source that is tagged and shipped, as one coordinated release, because an announcement cannot activate binaries
> that people have already installed. Shipping these notes as written would leave every fix that waits on Thaw
> Day inactive on every node that installed it.
>
> Before distribution: the release owner puts the approved heights into the tagged source, keeps one height per
> network, retains the promised upgrade window, and the resulting candidate is tested again. These notes are then
> finalised around that release.
>
> The independent review of this candidate said **no go** for release, for replacing a live wallet, and for
> starting the final reindex. Its report and the open items are in the private review tree.
============================

DigiByte Core v9.26.6 is a repair release. It fixes crashes and hangs, repairs
DigiDollar redeeming, makes DigiDollar amounts mean one thing instead of two,
corrects what the wallet shows you, and cuts about 1.5 GB off the memory a
synced node uses, at a cost of about two and a half seconds more on startup.

It also adds Thaw Day. Thaw Day is a single block height, one per network, at
which a set of DigiDollar rule changes takes effect together. **No network has a
Thaw Day height set in this release.** Installing this software does not switch
anything on.

Please report problems using the issue tracker at GitHub:

  <https://github.com/DigiByte-Core/digibyte/issues>


Read this first
===============

Three things change what you can run or what your scripts must send. Read all
three before you upgrade.

1. A pruned mainnet node cannot run at the smallest setting
-----------------------------------------------------------

A node has to keep the DigiDollar part of the chain. When a DigiDollar coin is
spent, the node reads the block that created it to find out how much it is worth
and how long it is locked. A pruned node has no transaction index, so the only
place to read that from is the block itself. Any block from the DigiDollar
activation height upward can hold one, so all of them have to stay on disk.

On mainnet that floor is height **23,627,520**. Everything from there to the tip
is kept. Everything below it can still be deleted.

At height 24,195,289, which was the tip on 12 September 2026, that is about
568,000 blocks. It grows by 5,760 blocks a day, because a block takes 15
seconds. The rest of the chain, about 97 per cent of it by block count, can
still be deleted. **Pruning still works. `-prune=550` cannot be honoured.**

How much disk that is has not been measured on a real node. As an estimate from
the average block size of the whole chain, about 1.2 kB, those 568,000 blocks
come to roughly 0.7 GB, growing by about 7 MB a day. Undo data is kept for the
same window and adds to it. Recent blocks are larger than the chain average, so
treat 0.7 GB as a floor, not an answer.

**What to do.** Before you upgrade, set `-prune` to a few gigabytes rather than
550. Start the node, let it run for a day, then read `size_on_disk` and
`pruneheight` from `getblockchaininfo` and set the target from the real number.

This constraint is not new in v9.26.6. It arrived with DigiDollar and is already
in v9.26.5. What is new here is that the floor is now held correctly. Before
this release, a reorg on a node whose tip was still below the floor dragged the
floor down to the height of the disconnected block and left it there until the
next restart, after which the node stopped pruning above that point. A pruned
node doing initial block download hit that on the first reorg it saw. That is
fixed.

**If your node has already pruned DigiDollar-era blocks, it will not start.**
It stops with a message telling you to restore the missing block and undo files
or download that part of the chain again. This check is not new either, but it
is the failure an operator is most likely to meet, so it is worth saying plainly.
A node that has never pruned below the floor is not affected.

**A node that cannot meet its prune target does not say so.** It keeps the
blocks it must keep, goes over the target you set, and writes nothing that tells
you why. This is known and is not fixed in this release. If your disk use is
above your target, this is the reason.

2. Six DigiDollar commands no longer guess cents or dollars
-------------------------------------------------------------

`senddigidollar`, `sendmanydigidollar`, `redeemdigidollar` and
`getredemptioninfo` used to work out whether a number meant cents or dollars
from the shape of the number. `10000` meant 10,000 cents, which is $100.00.
`10000.00` meant $10,000.00, which is a hundred times more. A habitual decimal
point moved a hundred times the intended amount, and the node did it without a
word.

They now take the unit with the amount:

- **A whole number with no unit still means cents.** `10000` is $100.00, exactly
  as before. Anything already sending whole numbers of cents needs no change.
- **A number with a decimal point and no unit is refused.** Nothing is sent,
  redeemed or changed. The error is:
  `ambiguous amount: pass amount_unit=cents or amount_unit=dollars (an amount
  with a decimal point is not accepted without a unit)`
- **`amount_unit="cents"`** requires a whole number. `"10.00"` is refused.
- **`amount_unit="dollars"`** allows at most two decimal places. `"12.34"` is
  1,234 cents. `"12"` is 1,200 cents. `"12.345"` is refused.

The new argument goes last:

```
senddigidollar <address> <amount> [comment] [fee_rate] [selected_inputs] [amount_unit]
sendmanydigidollar <dummy> <amounts> [comment] [selected_inputs] [amount_unit]
redeemdigidollar <position_id> <dd_amount> [redemption_address] [fee_rate] [amount_unit]
getredemptioninfo <position_id> [dd_amount] [amount_unit]
```

Two more take it for the amounts they filter on, rather than an amount they
send:

```
listdigidollarpositions [minconf] [maxconf] [min_amount] [max_amount] [amount_unit]
listdigidollaraddresses [minconf] [include_empty] [amount_unit]
```

Both of these send $250.00:

```
digibyte-cli senddigidollar "<DigiDollar address>" 25000
digibyte-cli -named senddigidollar address="<DigiDollar address>" amount="250.00" amount_unit="dollars"
```

**Anything automated against these four commands has to be checked before you
upgrade.** If it sends whole numbers of cents, it keeps working. If it sends
decimals, it stops working until you add `amount_unit="dollars"`. It fails
loudly rather than moving the wrong amount, which is the point of the change.

Two list filters take the same argument for the same reason: `min_amount` on
`listdigidollarpositions` and `min_balance` on `listdigidollaraddresses`. A
negative value in either used to be accepted and then ignored; it is now
refused. `getredemptioninfo` now applies the same $100,000 limit that
`redeemdigidollar` applies, so asking about a redemption and doing it give the
same answer.

`mintdigidollar` is unchanged. It has always taken whole cents only.

3. This release does not switch anything on
--------------------------------------------

Thaw Day is a single block height per network. In this release every network
ships with it not scheduled: mainnet, the public test network, and signet. Only
a private regtest chain can set one, with `-ddthawdayheight=N`. On any public
network that option is a startup error.

A height is chosen and published separately, with its own notice and its own
upgrade period. Until then, none of the DigiDollar rule changes in this release
does anything, on any network. Upgrading is safe to do now and changes no rule.


How to upgrade
==============

Shut the old version down and wait until it has fully stopped. This can take a
few minutes. Then run the installer on Windows, or copy over
`/Applications/DigiByte-Qt` on macOS, or `digibyted` and `digibyte-qt` on Linux.

A reindex is **not** required. The wallet file format has not changed. Back up
your wallets before you upgrade, as you would for any upgrade, and keep the
backups you already have.

If you prune, read section 1 above first and raise the setting.

Older wallets still open. Minting a new DigiDollar needs a descriptor wallet
with private keys, HD support, a Taproot receiving descriptor and a bech32
change descriptor. A wallet without those can still hold, send and redeem
DigiDollar; it cannot mint. The wallet now says so before it asks for your
passphrase, instead of failing at the end.


Thaw Day
========

Thaw Day is one block height per network. At and above it, three DigiDollar
rules change together:

- **Minting uses a price taken from the chain**, not from each node's own
  running volatility state. The block's committed oracle quote is compared with
  the lower median of the nearest 15 eligible prices between 240 and 1,440
  blocks back. Transfers and redemptions stop using the old volatility freeze.
- **Health is measured from open vaults**, and those totals are stored in the
  coin database with the rest of chain state, so they survive a restart and are
  rebuilt the same way on every node.
- **A vault has one identity** that every node derives the same way.

The height of the block being checked decides which rules apply to it. Not the
height of your node's tip. That means a reindex, a reorg and a freshly synced
node all reach the same answer for the same block.

You can see the status at any time:

```
digibyte-cli getdigidollardeploymentinfo
```

The `thaw_day` object reports whether a height is scheduled, what it is, your
tip height, the next block height, and whether the rules apply at each.

**One thing to expect when a height is finally set.** The first block at or
above the Thaw Day height makes the node add up every unspent output once, to
build the starting vault totals. The node stops answering while that runs. On a
test-network-sized coin set the walk cost about 0.3 microseconds per output,
which would be roughly twelve seconds for forty million outputs with the data
already in memory, plus a one-off allocation of about three quarters of whatever
your coin cache holds. **This has not been measured on a mainnet-sized coin
set.** It happens once, at one block, not once per block.


What changed
============

Money and safety
----------------

- **A wallet with no passphrase could replace the key that opens a vault.**
  Saving a DigiDollar owner key for a vault that already had one overwrote the
  old key without a word. The owner key is how a wallet reaches its collateral,
  so the vault would have been left with no way to open it and the collateral
  would have been stuck. Every wallet now refuses to replace an owner key with a
  different one, and says so in the log.

- **Redeeming could send a whole vault to an address nobody can spend from.** A
  redemption hands the entire vault back in its first output. When the caller
  gave no address for it, the builder made one up from the owner key. No wallet
  watches that address and no wallet can spend from it. It was reached when the
  wallet could not supply a change address and the leftover after the fee was
  too small to make a change output, so the build did not stop earlier. The
  redemption now stops with a plain error and builds no transaction.

- **A mint is written to the wallet before it is sent, not after.** A node that
  stopped in between used to leave coins locked in a vault with nothing in the
  wallet to redeem it. Every write now reports whether it worked, and nothing is
  sent if one fails. This is true from the console and from the wallet window.

- **A missing owner key is recovered, never invented.** Redeeming a vault whose
  key record has gone missing now searches the wallet's own keys, including the
  unused ones it keeps ready, and accepts only a key that reproduces the vault's
  own output on the chain. Five distinct errors replace the single "owner key
  not found", so a locked wallet, a watch-only wallet and a genuinely missing
  key can be told apart.

- **Leftover DigiByte goes to your own wallet.** In a redemption it can no
  longer follow an address you typed in, such as an exchange deposit address. If
  a mint, a send or a redemption has nowhere safe to send leftover DigiByte, it
  now stops with an error and builds nothing, rather than paying it to an
  address nobody keeps the key for.

- **`senddigidollar` and `redeemdigidollar` refuse an amount over $100,000**
  before they select a single coin. `sendmanydigidollar` already did. The limit
  applies to what you type. It never limits the burn a wallet works out for
  itself when closing a vault.

- **Redeeming works again on a wallet whose DigiByte is in many small pieces.**
  The fee used to be estimated once against a fixed 400-byte guess, and the
  redemption then failed when the real transaction cost more. It now measures
  the transaction its candidate coins would produce and asks for more until they
  cover it. If a wallet holds only coins so small that each one adds more fee
  than value, the command says so and tells you to consolidate.

- **A mint that can no longer confirm releases what it reserved.** It is
  reported as expired, and the key and the record are kept, so a reorg or a late
  block brings the vault back.

- **Two mints, or two redemptions, sent at the same instant no longer pick the
  same coins** and knock each other out.

Crashes, hangs and things that would not stop
----------------------------------------------

- **The node always finishes shutting down.** It used to destroy its oracle
  objects as the first step of shutting down, while a scheduler thread was still
  running inside one of them. That thread then waited forever on a lock in freed
  memory, and when the memory was reused instead, the node crashed. On the test
  machine, 8 stops out of 30 hung on the old code. A test that stops a node
  twenty times in a row failed on every attempt: three of those runs hung and
  one ended in a segmentation fault. On the fixed code, 170 stops in a row all
  completed. A node that hangs on shutdown leaves its coin database unwritten
  and has to roll the chain forward again on the next start.

- **A crash during a reorg is fixed.** The mempool's index update was being run
  on entries belonging to the Dandelion stem pool, which wired the two pools
  into each other. The next block connect then walked a broken structure and the
  node died.

- **Three ways a node could freeze are fixed.** All three were two threads
  taking the same two locks in opposite order: the once-a-second Dandelion
  embargo check, a peer disconnecting with stem transactions still queued, and
  the inventory handler, which ran on every new connection.

- **Two more lock-order faults are fixed.** The wallet's fallback broadcast
  path, used when no Dandelion peer is available, changed the stem pool and ran
  mempool acceptance with no locks at all. And on UTXO snapshot activation the
  stem pool stayed on the old chain state. Both now take the same locks in the
  same order as every other writer.

- **The wallet can no longer freeze the node.** Loading a wallet, importing a
  descriptor or a wallet file, and `rescanblockchain` used to be able to hang a
  node that had a DigiDollar wallet, because the wallet asked the chain a
  question while holding a wallet lock and the chain was waiting for the wallet.
  Minting and redeeming had the same fault while checking that the chain had not
  moved.

- **Eight DigiDollar wallet commands now wait for the wallet to catch up with
  the newest block.** A balance asked for straight after a block is now right,
  and minting no longer fails over money that has already confirmed.

What the wallet shows you
--------------------------

- **A redemption always tells you what happened.** Every message from the redeem
  form now opens a dialog, with the transaction id, instead of going to a
  desktop notification service that may be switched off or absent. Users were
  seeing nothing at all after typing their passphrase. Every refusal from the
  mint form does the same.

- **A mint appears as a mint.** The transaction list used to show it as money
  sent away and then received back, and folded the fee into the collateral
  figure, so the wallet claimed more DigiByte was locked than really was. A mint
  now has its own row showing the DigiDollars it created, with the collateral
  and the fee on their own rows and the fee shown once.

- **DigiByte and DigiDollar amounts have separate columns.** One column used to
  hold DigiByte on some rows and dollars on others, so it could not be sorted,
  added up or exported. Each column now sorts on its own number and exports
  under its own heading.

- **The transaction details window understands DigiDollar.** It shows the
  amount, the collateral locked or returned, the lock period, the block the
  collateral unlocks at, and the vault. Where a number is genuinely not in the
  transaction it says so, instead of printing a zero that reads like a real
  amount.

- **Returned change is named correctly.** What a redemption hands back used to
  be called "Redemption Change", which says the opposite of what happens. It is
  now "DigiDollar change returned", with one sentence saying where it came from:
  the wallet spends whole DigiDollar inputs, and if they add up to more than the
  redemption burns, the extra comes back. A vault is always closed in full,
  never in part.

- **The Redeem button on a locked wallet asks for the passphrase** instead of
  being greyed out so the prompt never appeared. A wallet with no private keys
  is still refused, and now says why.

- **The send form says why it will not take an amount**, and keeps the number
  you typed instead of silently dropping the digit that would take it over the
  limit.

- **The transaction list always shows a confirmation count**, instead of
  switching to the word "Confirmed" after five, and the details window labels
  that count "Confirmations".

- **Paying DigiDollar to your own wallet no longer writes errors to the log** on
  a successful send.

Speed and memory
----------------

- **A synced mainnet node keeps about 1.5 GB less in memory, and pays about two
  and a half seconds more on startup for it.** Every block header the node held
  carried an array of eight pointers, one per mining algorithm, recording the
  last block that used it. It cost 64 bytes for every one of the chain's 24
  million headers, and two of its eight slots were for algorithms that were
  never switched on. It is gone. The two places that read it now use the same
  walk the difficulty rules have always used for everything else.

  The saving was measured three independent ways that agree within one per cent:
  the block header record read out of each compiled binary, 208 bytes before and
  144 after; 1.56 GB less on a real synced mainnet node, which is 14 per cent of
  everything that node was holding; and 62.9 bytes saved per block on the real
  test network chain reindexed from the first block, against the 64 bytes
  predicted.

  **The cost.** Startup takes about two and a half seconds longer on a mainnet
  node, out of roughly two minutes. That is a real cost, paid on every start.
  **Connecting blocks shows no measurable difference.** Three hundred blocks
  were taken off and put back again, five rounds a start, two starts a build, on
  a copy of the real mainnet chain; the spread from one run to the next is
  twenty times larger than anything this change could contribute.

  The cost exists because the deleted array answered "which was the previous
  block using this mining algorithm" in one jump, and the node now walks back
  down the chain to find it. On mainnet the five algorithms are mined evenly, so
  that walk averages under five steps. On the test network one algorithm can go
  a long time without a block, and the same walk averages 127 steps, which is
  why the test network shows a much larger slowdown than mainnet does.

  **An earlier figure was wrong and is withdrawn.** A first measurement said
  startup got 3 to 4 per cent faster. A second measurement, with timing code
  compiled into both builds, re-split those same runs and found the difference
  was well inside the noise. Startup is slightly slower, not faster, and the two
  and a half seconds above is the figure to use.

  **Difficulty is unchanged.** The walk that remains is byte for byte the code
  that was already there. Before the array was deleted, the two lookups were run
  side by side over every mining algorithm, every difficulty rule, every era and
  the minimum-difficulty special case, and their answers were written down as
  plain numbers that the tests still check. The node reports exactly the same
  difficulty for all five algorithms on mainnet and on the test network, and
  reaches exactly the same chain tip. Nothing on disk changed: the stored block
  record never held these pointers.

- **Routine DigiDollar and oracle log lines are quiet by default.** Three lines
  were written for every transaction on every node regardless of settings. They
  are gone, and routine DigiDollar and oracle messages now need
  `-debug=digidollar`. Errors, warnings and startup progress are still shown
  without it. One line is a known exception and still prints without the
  category: a note that an owner key is already saved, written when a wallet is
  paid DigiDollar at one of its own addresses. Anything that reads the log for
  routine DigiDollar lines has to add the category.

- **Walking the coin database no longer copies every changed cache entry into a
  second table first**, which on a node with a large cache was close to a
  gigabyte allocated while a block was being connected. The set of coins it
  hands out, and every value in it, is unchanged.

- **Mining no longer grinds at the block before Thaw Day.** The starting
  accounting record is built once, when the block is really connected, instead
  of once for every block template a miner asks for.

Startup and recovery
--------------------

- **Startup does not repeat oracle work it has already done**, and reuses parsed
  bundles instead of parsing them again.

- **A long DigiDollar rebuild reports progress and can be cancelled.** A scan
  that is interrupted no longer publishes half-finished figures. This rebuild
  only runs once a Thaw Day height is set and reached. With no height set, it
  does nothing.

- **A pruned node keeps the block history validation needs.** See section 1
  above.

Tests, tools and documents
---------------------------

- `decoderawtransaction` and `getrawtransaction` work on DigiDollar transactions
  on a node started with `-rpcdoccheck`, and their help lists the DigiDollar
  fields they return. They could not before, so no test could decode one.
- Two inherited pruning tests now build ordinary pre-DigiDollar history first,
  so they can test pruning on a chain where DigiDollar starts at the first
  block.
- Seven unit tests no longer pass or fail depending on which directory the test
  program was started from.
- The anchors test writes a port with its leading zero, so it no longer reports
  a false failure on a machine using low port numbers.
- The operator, wallet, exchange and oracle guides are updated.
- Oracle display names come from each network's roster. Mainnet ID 0 shows
  "DigiByte.Io Oracle" and ID 11 shows "Crypto Corner Shop". This is local
  display text only. Oracle IDs, keys, ordering, signatures and quorum are
  unchanged, and nothing about what is signed or sent over the wire changed.


For exchanges and custody
=========================

- **The four amount commands are the breaking change.** See section 2. Check
  every script before you upgrade.
- **Circulating supply can now report that it is unknown** instead of returning
  a number it cannot stand behind. `getdigidollarstats` returns an error,
  "DigiDollar circulating supply is unavailable from retained metadata", when
  the historical amounts cannot be recovered reliably. Treat that as unknown.
  Do not treat it as zero.
- **Open vault principal is reported separately from circulating tokens.**
  `getdigidollarstats` reports `open_vault_principal`, and
  `selected_health_denominator` says which of the two the health figure was
  computed from. At and above Thaw Day it is `open_vault_principal`; below it,
  `legacy_supply`.
- Extra burns can leave fewer circulating tokens than open vault principal. With
  the same collateral, that can lower health, prolong a restriction on minting,
  or raise the DigiDollar needed to redeem. Thaw Day itself creates no tokens
  and changes no wallet balance.
- **Leftover DigiByte from a redemption no longer follows a redemption address
  you supply.** If you pass a deposit address for the returned collateral, the
  change after the fee stays in the sending wallet, and the redemption stops
  with an error rather than sending the change somewhere the wallet cannot
  reach.


For miners and pools
====================

Nothing in this release changes difficulty or the block reward.

Every change in this release that touches the rules a node applies to a block is
gated on the Thaw Day height, and no network has one set. With no height set,
this release should accept and reject exactly the blocks that v9.26.5 does. The
evidence for that is the test network chain, reindexed from the first block on
this code, reaching the same block and the same hash as before. The same check
on mainnet has not been run.

Once a Thaw Day height is set and reached, a block that mints DigiDollar is
checked against a price taken from the chain rather than from each node's own
state. A miner still running old software after that height can build blocks the
rest of the network rejects. Plan to upgrade before the height, not after it.


What has been checked, and what has not
=======================================

Checked:

- The whole public test network chain was reindexed from the first block, with
  every signature checked, on this code and on the code without the memory
  change. Both reached the same block and the same hash.
- The unit tests, the wallet window tests and the node tests were run on the
  tree with all nine commits, and the recorded result was a clean run of all
  three. The full result for the build that ships is recorded separately and
  goes out with it.

Not run:

- **The full mainnet reindex from genesis has not been run.** It is deliberately
  handed to a separate team to run against the final build.
- **No reindex has crossed a Thaw Day height**, because no network has one set.
- **The seven-day test network run has not been done.**
- **Independent review of the whole change, the build and the results is not
  finished.**

Passing a reindex is evidence about one build and one history that already
exists. It says this software validates the blocks that are already on the
chain. It says nothing about a block someone mines next month.


Compatibility
=============

DigiByte Core is supported and extensively tested on operating systems using the
Linux kernel, macOS 11.0 and newer, and Windows 7 and newer. It should work on
most other Unix-like systems but is not tested on them as often.

Notes for earlier releases are under `doc/release-notes/`.
