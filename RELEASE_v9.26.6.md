# DigiByte Core v9.26.6 release notes

**Current version: v9.26.6rc2.** This is the second release candidate: a test
build before the final release. It replaces rc1, which could reject a valid
DigiDollar mint block during a reindex on a node holding a DigiDollar wallet.
See "Changes since rc1" below.

## Summary

v9.26.6 includes **85 fixes and improvements**. It improves wallet recovery,
DigiDollar (DD) sending and redemption, wallet displays, node stability, price
services, memory use and startup. The count also includes tests and documentation.

**Node and mining operators must upgrade before mainnet block 24,490,000,
expected around November 1, 2026.** This applies even if you only use DGB.

Thaw Day changes consensus: the rules nodes use to agree on valid blocks.
Older software may follow a different chain after the change.

| When | What takes effect |
| --- | --- |
| When the upgraded program runs | Wallet, stability, memory and startup improvements |
| At Thaw Day | The new DigiDollar block rules, together at one height per network |

We are testing Thaw Day on **testnet first**. The final mainnet release follows
the remaining tests and release review.

## Changes since rc1

**Block validation no longer reads the wallet's script table.** The node keeps
an in-memory table that the wallet fills in with the DigiDollar amount behind
each output script. It is keyed by the script alone, so an owner address that
is used again keeps only the last amount. In rc1, block validation below Thaw
Day read that table before the chain data. A mainnet rc1 node whose wallet had
minted $100 and later held $98 of change at the same address rejected the
first DigiDollar mint block, 23,869,549, while reindexing, then stopped
following the chain. Nodes without such a wallet accepted the block. Validation
now reads amounts from the chain only. The Thaw Day heights and rules are
unchanged.

**If an rc1 node is stuck**, install rc2, start it, and run:

```bash
digibyte-cli reconsiderblock 00000000000000052cc3d211b3d16196a3585d46474202dfa42e9f770d02e9bd
```

The node validates the remaining blocks and catches up. A fresh `-reindex`
on rc2 also works. Do not start `-reindex` on an rc1 node that holds a
DigiDollar wallet.

**A wallet log line no longer repeats once per block** during a reindex or a
first sync. It now needs `-debug=digidollar`.

## What you need to do

1. **Read the upgrade notes below.** Check scripts that send DD amounts and
   make sure the node has enough disk space.
2. **Back up your wallets and keep existing backups.** Keep your configuration
   and oracle keys too.
3. **Verify the build, then replace the old program.** Check the release's
   signed checksums. Stop the old node normally and wait for it to exit first.
   RC2 is for the test exercise; use the approved final build for the mainnet rollout.
4. **Let startup finish.** The node may need to check or rebuild accounting.
   Do not mistake a long scan for a stopped program.
5. **Check the running node.** Confirm its version, network, sync progress and
   Thaw Day height. Check any wallet or oracle service you operate.

Full nodes, miners, pools, exchanges, oracle services and wallet backends need
the update. If a provider runs your wallet or exchange service, check its
upgrade notice.

## Thaw Day: testnet first, then mainnet

| Network | Activation block | Estimated date |
| --- | --- | --- |
| Testnet26 | **432,100** | September 18–19, 2026, before the September 21 test target |
| Mainnet | **24,490,000** | Around November 1, 2026 |

**The block number starts the change. The date is an estimate.** Faster or
slower block production can move that date. The old rules remain below the
height, including when old blocks are checked again.

Release files and instructions must be available at least **3 days before
testnet activation** and **14 days before mainnet activation**. Track block
progress during that time.

There is no miner vote or local public-network setting to change Thaw Day.
Postponing it requires a replacement build and coordinated upgrades before
the original height. Editing an announcement cannot postpone installed software.

The testnet exercise will check the switch itself, minting, transfers,
redemptions, oracle prices and agreement between independent nodes. Tests also
cover restarts and chain changes, where recent blocks are replaced by another
branch. Public testnet activation and observation have not yet finished.

To check your node's schedule:

```bash
digibyte-cli getdigidollardeploymentinfo
digibyte-cli -testnet getdigidollardeploymentinfo
```

The result shows the height and whether the rules apply to the current tip
or next block. The tip is the latest block on that node's chain.

## Upgrade details that matter

### Amounts sent by scripts

Four commands accept `amount_unit`: `senddigidollar`,
`sendmanydigidollar`, `redeemdigidollar` and `getredemptioninfo`.
The amount filters in `listdigidollarpositions` and
`listdigidollaraddresses` use it too.

| Amount supplied | Meaning |
| --- | --- |
| `25000`, with no unit | 25,000 cents, or $250.00 |
| A decimal, with no unit | Refused; choose cents or dollars |
| `amount_unit="cents"` | Whole cents only |
| `amount_unit="dollars"` | Dollars, with at most two decimal places |

**Existing scripts using whole cents keep working. Scripts using decimals
must specify dollars.** The new unit argument goes last in a positional call.
For example, this requests a $250.00 send:

```bash
digibyte-cli -named senddigidollar address="<DigiDollar address>" amount="250.00" amount_unit="dollars"
```

`mintdigidollar` still takes whole cents.

The **$1 minimum DD output is unchanged**. The **$100,000 request limit**
applies to a single send, each sendmany recipient and a requested redemption.
It does not limit the extra DD the rules may require burning in an emergency
redemption. A redemption still closes the whole vault.

### Minting and redemption after Thaw Day

A **vault** is DGB locked to back newly minted DigiDollars. **Oracles** provide
the signed DGB prices used by DigiDollar.

At Thaw Day, a new price check replaces the old mint freeze. It uses prices
already recorded in the chain. Transfers and redemptions stop using the old
volatility freeze, but their other requirements still apply.

New mints still need an accepted oracle price, enough collateral and a price
that passes the new check. Large price swings can still pause minting.

Health will count the original DD amounts attached to vaults that remain open.
DD still in circulation is a separate total. Extra DD burned during redemption
can make circulation smaller than the amount attached to open vaults.

**This can lower health, keep emergency restrictions in place longer or require
more DD to redeem than the old calculation.** Check the current estimate before
confirming. Thaw Day itself creates no tokens and changes no wallet balances.

### Wallets and service integrations

- Minting needs the modern descriptor wallet format, private keys and supported
  receiving and change addresses. The wallet explains missing support early.
- Existing wallets can still send and redeem if they hold the needed keys.
  Recovery of a missing key record cannot replace a missing private key.
- CSV exports add `Amount ($DD)`. Position status can include `expired_mint`,
  `abandoned_mint` and `conflicted_mint`.
- Statistics separate `open_vault_principal` from circulating supply.
  `selected_health_denominator` identifies the total used for health.
  An unavailable total is **not zero**.
- Oracle signing keys and required agreement stay the same.
- Use `-debug=digidollar` if your log tools need routine DD details.
  Errors and startup progress remain visible without it.

### Disk space, startup and recovery

Pruning deletes older block files to save space. A pruned node must still keep
DigiDollar's required history. On mainnet, the retention floor remains
**23,627,520**. Required blocks and the data used to reverse them must stay.

That history can exceed a small prune target. The target is not a hard disk
limit, and this release does not add a warning for every overrun. Leave room
for growth and check actual disk use.

An ordinary upgrade with the required data does not need a full reindex.
A reindex rebuilds the node's chain databases from block history. A late
upgrade may need extra checks; missing files need restoration or download.
Repeated restarts cannot supply missing data.

If recovery fails, keep the error, logs, wallets and block files. Pause any
automatic restart loop while the cause is checked. After Thaw Day, downgrading
to old software is not a repair. If nodes disagree, compare block hashes at
the same height and coordinate recovery before resuming settlement.

Feather's RAM improvements work on installation. On Unix, the new database
file allowance may expose a low system open-file limit. Check that limit if
startup reports it or reduces connections.

## All 85 fixes and improvements

Each entry states the change and the reason. Related repairs are grouped.
These are implemented changes, not a claim that every outside bug report is fixed.

### Thaw Day and DigiDollar accounting

New block rules wait for Thaw Day. Status reporting and preparation are available before it.

1. **Change:** All new DigiDollar block rules start at one Thaw Day height on each network.\
   **Why:** Nodes need to change rules at the same point.

2. **Change:** Status shows the scheduled height and whether the current or next block uses the new rules.\
   **Why:** Operators can see when their node reaches Thaw Day.

3. **Change:** At Thaw Day, new mints use earlier block prices to check for large price swings.\
   **Why:** Nodes checking the same chain need the same minting reference.

4. **Change:** At Thaw Day, transfers and redemptions stop using the old volatility freeze.\
   **Why:** The minting price safeguard should not apply that freeze to these operations.

5. **Change:** At Thaw Day, checks use the prices, coins and history belonging to the block being checked.\
   **Why:** A restart or a different branch must not change the inputs to that block's checks.

6. **Change:** At Thaw Day, health counts the original DD amounts attached to vaults still open.\
   **Why:** The system needs a consistent measure of what those vaults back.

7. **Change:** Vault totals are saved with chain data and updated as blocks are added or reversed.\
   **Why:** Accounting must stay with the right chain through restarts and interrupted saves.

8. **Change:** At Thaw Day, validation and recovery give each vault the same transaction-based identity.\
   **Why:** Both paths must find the same vault.

9. **Change:** Recovery checks saved vault totals and unchecked Thaw Day history, and rebuilds totals from available records when needed.\
   **Why:** Late upgrades and interrupted recovery must finish their checks before using the results.

10. **Change:** Status separates DD still in circulation from the amounts attached to open vaults.\
    **Why:** Burning extra tokens can make these two totals differ.

11. **Change:** From Thaw Day, saved circulating-supply totals are checked against chain records and repaired when needed.\
    **Why:** An incorrect saved total should not keep carrying forward.

12. **Change:** Coin scans use a consistent order and read older accepted vault records consistently.\
    **Why:** Recent changes saved at different times must still give the same accounting totals.

### Node crashes, hangs and transaction sharing

Dandelion is DigiByte's system for passing transactions between nodes.

13. **Change:** Fixes a Dandelion crash when recent blocks are replaced by another chain branch.\
    **Why:** Pending transaction records must remain usable during a chain change.

14. **Change:** Fixes hangs during transaction-sharing checks, peer disconnections and incoming announcements.\
    **Why:** Tasks running at the same time must not get stuck waiting for each other.

15. **Change:** Protects shared transaction records when the usual Dandelion relay peer is unavailable.\
    **Why:** The fallback send path must handle those records safely too.

16. **Change:** After loading a saved chain state, the relay checks pending transactions against that chain.\
    **Why:** The relay must follow the chain the node is using.

17. **Change:** Fixes conflicting wallet and chain locks during wallet loading, imports, rescans and DigiDollar checks.\
    **Why:** Wallet work and chain work must not block each other indefinitely.

### Wallet recovery, sending and redemption

18. **Change:** Saves a mint's owner key and recovery records before sending the transaction.\
    **Why:** The recovery information must be saved before the mint can reach the network.

19. **Change:** Rebuilds a missing vault-key record from a matching key already in the wallet.\
    **Why:** A missing record should not stop redemption when the correct key still exists.

20. **Change:** Refuses to replace a vault's saved owner key with a different key.\
    **Why:** Existing vault ownership records must stay intact.

21. **Change:** Reports an error if the key for a new DigiDollar address cannot be saved.\
    **Why:** The wallet should save the required key before offering the address.

22. **Change:** Releases eligible coins reserved by failed, abandoned or expired mints, including after restart.\
    **Why:** Unsuccessful mints should not leave spendable DGB reserved forever.

23. **Change:** Keeps mint keys and recovery history during cleanup and checks whether the mint can still confirm.\
    **Why:** A later confirmation or chain change can make that history necessary again.

24. **Change:** Labels expired, abandoned and conflicted mints separately from completed redemptions.\
    **Why:** Users need to distinguish a failed mint from a closed vault.

25. **Change:** Saves affected transaction states together when abandoning a transaction, and reports failed saves.\
    **Why:** An incomplete save must not look successful or release coins incorrectly.

26. **Change:** Coordinates simultaneous mint and redemption requests so they do not select the same coins.\
    **Why:** One request should not conflict with another before either has finished.

27. **Change:** Calculates redemption fees from the transaction being built and selects more fee coins when needed.\
    **Why:** Many small DGB coins can make a transaction larger than a fixed estimate.

28. **Change:** Explains when small DGB coins cost more in fees than they can contribute.\
    **Why:** Users need to know when to combine small coins instead of retrying.

29. **Change:** Stops a mint or send if required DGB change has no usable destination.\
    **Why:** Leftover funds must not go to an address invented by the builder.

30. **Change:** Checks the redemption address and keeps leftover fee money in the sending wallet.\
    **Why:** Collateral may go to a chosen external address; fee change should stay in the wallet.

31. **Change:** Adds explicit cents or dollars to four amount commands and two filters, with checks for invalid amounts.\
    **Why:** A decimal point must not silently change the amount being requested.

32. **Change:** Rejects sends, requested redemptions and redemption estimates above $100,000; sendmany keeps its per-recipient cap.\
    **Why:** The limit should be checked early and match between estimates and submitted requests.

33. **Change:** Prevents large amounts from overflowing wallet totals, displayed collateral ratios and collateral diagnostic calculations.\
    **Why:** Overflow can turn a large number into a misleading small or negative number.

34. **Change:** Explains missing minting support before confirmation or passphrase prompts.\
    **Why:** Users should know their wallet cannot mint before approving the operation.

35. **Change:** Eight more DigiDollar wallet commands wait for the wallet to process the latest block.\
    **Why:** A newly confirmed balance or transaction should not be missed.

36. **Change:** Mint and redemption estimates follow the next block's rules and explain unavailable or restricted conditions.\
    **Why:** The estimate should use the same rules as the transaction builder.

37. **Change:** Loads compatible saved fee estimates correctly after restart.\
    **Why:** The node should keep the useful fee history it already learned.

### Desktop wallet displays and buttons

38. **Change:** The Redeem button on a locked wallet opens the passphrase prompt.\
    **Why:** An owner needs to unlock the wallet to redeem; wallets without private keys still cannot sign.

39. **Change:** Shows redemption results and mint refusals in dialogs.\
    **Why:** Important messages must be visible even without desktop notifications.

40. **Change:** Redemption success messages show the transaction ID and explain that collateral returns after confirmation.\
    **Why:** Sending a transaction is different from confirming it.

41. **Change:** Shows wallet-created mints with separate rows for DD created, DGB locked and the network fee.\
    **Why:** A mint should not look like money sent out and back, or count its fee as collateral.

42. **Change:** Shows the DGB fee for a DigiDollar transfer on its own row.\
    **Why:** A DGB fee should not disappear into a dollar amount.

43. **Change:** Gives DGB and DigiDollar separate columns in transaction tables and CSV exports.\
    **Why:** Each currency needs its own value for sorting and exporting.

44. **Change:** Uses the correct currency for copied amounts, overview entries and notifications.\
    **Why:** A DigiDollar payment should not appear as zero DGB.

45. **Change:** Keeps DigiDollar rows visible when the DGB minimum-amount filter is used.\
    **Why:** A filter for DGB amounts should not hide dollar transactions.

46. **Change:** Restores saved column widths after loading the table and keeps the DD column visible.\
    **Why:** Opening the wallet should preserve the layout and show both currencies.

47. **Change:** Transaction details show DD amounts, vault information, lock periods and the actual mint collateral, even with reordered outputs.\
    **Why:** Users need the actual transaction details instead of misleading zero amounts.

48. **Change:** Gives returned DD its own row, labelled “DigiDollar change returned,” with an explanation.\
    **Why:** Extra selected DD comes back as change; the vault still closes in full.

49. **Change:** Keeps showing the confirmation count after a DigiDollar transaction is confirmed.\
    **Why:** The count remains useful after the first few confirmations.

50. **Change:** Keeps amounts typed above the send limit and explains why they cannot be sent.\
    **Why:** Dropping the final digit could leave a smaller amount than intended.

51. **Change:** Allows full DigiDollar addresses to be typed and corrected in the send form.\
    **Why:** The old partial-address cutoff could prevent entry of a valid address.

### Oracle price services

52. **Change:** Fixes an alternate way of combining oracle public keys and keeps cached results tied to the right network settings.\
    **Why:** Price verification must use the intended keys and settings.

53. **Change:** Starts oracle services before other tasks use them and waits for those tasks before destroying the services.\
    **Why:** Background work must not use a service that is not ready or has already stopped.

54. **Change:** Retries stored public signing messages when the first send had no connected peer.\
    **Why:** Signing should recover when a connection becomes available.

55. **Change:** Protects oracle settings and the current signing period from conflicting reads and changes.\
    **Why:** Tasks running together must not read settings while another task changes them.

56. **Change:** Shows oracle operator names from the selected network's list.\
    **Why:** Operators should be easy to recognize without changing their keys or signing rules.

### RAM and disk use

The block index is the node's directory of known blocks. A header is a block's short summary.

57. **Change:** Removes eight mining lookup pointers from each block's memory record.\
    **Why:** Finding earlier blocks through existing links saves permanent RAM.

58. **Change:** Reads less-used saved block details from disk and keeps a limited set of recent reads in RAM.\
    **Why:** Every block no longer needs those details in memory all the time.

59. **Change:** Stores mining work scores in less space, with an exact full-size value when needed.\
    **Why:** Saving RAM must not round the score used to choose a chain.

60. **Change:** Gives block records space in shared memory chunks and removes unnecessary lookup bookkeeping.\
    **Why:** Millions of separate allocations waste space.

61. **Change:** Reads needed parts of database files while still checking for incomplete or damaged data.\
    **Why:** The node can avoid whole-file memory mappings without dropping those checks.

62. **Change:** Reserves open-file capacity for databases before assigning it to network connections.\
    **Why:** Database and network work need to share the system's file limit.

63. **Change:** Uses a smaller sorted copy of changed coins during accounting scans.\
    **Why:** The scan needs a stable copy, but not the previous extra lookup table.

64. **Change:** Keeps unsaved headers in RAM and saves block and reversal data before recording their disk locations.\
    **Why:** A failed save must not lose pending data or point to an unfinished file write.

65. **Change:** Handles missing or unreadable local block headers as storage errors.\
    **Why:** A local disk failure must not be blamed on another node's block.

### Startup, recovery and mining

66. **Change:** Loads the block directory without first reading it only to count its entries.\
    **Why:** Startup can begin loading straight away and report the count as it goes.

67. **Change:** Reads the last saved block before building lists of possible chain ends.\
    **Why:** Startup avoids making a large list it would immediately discard.

68. **Change:** Avoids repeated warning calculations for old periods that cannot trigger the warning.\
    **Why:** Startup does less work while keeping checks at the boundary and afterward.

69. **Change:** Reuses processed oracle price records and avoids repeating completed startup work.\
    **Why:** The same records should not need repeated parsing and setup.

70. **Change:** Shows reconstruction progress, allows cancellation and keeps incomplete results marked as not ready.\
    **Why:** Users need to see progress without mistaking a partial scan for a finished one.

71. **Change:** Stops startup with specific recovery information when required history cannot be read.\
    **Why:** Repeated restarts cannot replace missing block or reversal data.

72. **Change:** Keeps the required DigiDollar history through chain changes, including when no blocks can yet be pruned.\
    **Why:** Deleting old files must not remove records still needed to check transactions.

73. **Change:** Prepares starting Thaw Day totals when the preceding block is added, rather than for every mining request.\
    **Why:** Miners should not repeatedly scan all coins while asking for work.

### Logs, tests and instructions

74. **Change:** Moves routine DigiDollar, oracle and wallet messages into debug logs and removes three repeated transaction messages.\
    **Why:** Normal logs should stay readable while retaining errors and startup progress.

75. **Change:** Stops reporting a false key-write error when re-saving the same key during a payment to the same wallet.\
    **Why:** A successful payment should not look like a failed save.

76. **Change:** Documents the DD fields returned by raw-transaction commands so their result checks also work.\
    **Why:** Command help and automated checks need to match the actual output.

77. **Change:** Lets source-reading tests find the checkout from a different working directory.\
    **Why:** The directory used to launch a test should not cause a false failure.

78. **Change:** Corrects test setup for pruning, low-numbered ports, fees, script rules and Thaw Day.\
    **Why:** Tests must use the intended network rules and data.

79. **Change:** Captures and checks errors from tests that deliberately cause storage failures.\
    **Why:** Successful error tests should not print unexplained fatal-error messages.

80. **Change:** Adds checks for activation, accounting, extra burns, restarts, chain changes, wallet recovery, oracle retries and memory use.\
    **Why:** These tests help catch future changes that break the repairs.

81. **Change:** Fixes the supplied configuration example and refreshes command help and manuals.\
    **Why:** The example must load correctly and describe the available settings.

82. **Change:** Updates node, wallet, exchange and oracle instructions for upgrades, recovery, amounts and accounting.\
    **Why:** Operators need instructions for the behavior added in this release.

83. **Change:** Adds setup steps for nodes sharing a router and for block-filter services used by lightweight wallets.\
    **Why:** Correct ports and service settings help those setups work; this does not claim a mobile-wallet code fix.

### Reindex and block validation (rc2)

84. **Change:** Block and mempool validation read DigiDollar amounts from the chain only, never from the wallet's in-memory script table.\
    **Why:** A node's answer about a block must not depend on what its own wallet has done since.

85. **Change:** The wallet's "ReconcilePositionStates skipped" message moves into the DigiDollar debug category.\
    **Why:** It was written once per block during a reindex and filled the log.

## Tests completed and work still ahead

These are the recorded results for the rc2 source commit
`653484decd` (the block validation repair) with the rc2 version bump on top:

| Check | Result |
| --- | --- |
| C++ unit tests | 3,742 passed, none failed |
| Extended functional tests | 396 passed, 17 skipped, none failed |
| Desktop wallet tests | Passed |
| Mainnet node with the affected wallet | Accepted block 23,869,549 after `reconsiderblock` and caught up to the tip, 24,222,044, with 0 rejected blocks |
| Thaw Day testnet lab exercise | Passed: isolated run `rc2-rehearsal-09` crossed Thaw Day at lab height 5,000, ran every DigiDollar feature before and after it, and finished at height 5,658 with 143 checks and 0 failures |

The rc1 results for commit `d2097819f260f4d82409643fe9f7263cdd7e3eaa` were
3,739 unit tests passed, 394 functional tests passed with 17 skipped, desktop
wallet tests passed, and fuzz tests over 255 public targets with 28 longer
runs. The fuzz targets were not rerun for rc2.

Unit tests check pieces of the program. Functional tests run nodes and check
how they behave. Fuzz tests try many inputs to find unexpected behavior.
A skipped test is not a pass.

One RC1 desktop run settled at about **3.65–3.67 GiB of RAM** and started in
**108 seconds**. These figures describe that workload. They do not predict
startup on every computer or the cost of the future Thaw Day accounting scan.

Before the final mainnet release, complete the full mainnet history reindex,
public testnet activation and observation, remaining platform checks and final
release review.

Some reports about mobile wallets, pools and outside services still need those
projects' investigation. Mining difficulty, block rewards and the $1 DD output
minimum are unchanged by this release.

## Reporting a problem

Include the version, network, block height and hash, what you expected, what
happened and relevant log lines. Remove private information from logs first.
Report security-sensitive details through the project's private security
channel, rather than a public bug report.
