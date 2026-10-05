# DigiByte Core v9.26.7 release notes

**Draft for review. Release packages have not been built or published.**

v9.26.7 is a small follow-up to v9.26.6. It fixes slow chain-information
requests, fixes a false fatal error during shutdown, and improves three
wallet details. It includes **five user-facing changes** and
**five test and CI corrections**, for **ten fixes and improvements**.

The block-validation rules, oracle requirements and Thaw Day heights are
unchanged. Mainnet Thaw Day remains at block **24,490,000**. Testnet26
activated at block **432,100**. These are block heights, not calendar dates.

All full-node and mining operators must run Thaw Day-compatible software
before mainnet reaches block 24,490,000. v9.26.6 already contains those rules;
v9.26.7 does not introduce another activation or require another reindex.

## What to do when the release is available

1. Back up your wallets and configuration. Keep the backups private.
2. Download the package for your system and compare its SHA-256 checksum with
   the published checksum file. A checksum detects changed bytes; it is not
   a digital signature.
3. Stop the old wallet or daemon normally. Replace the program and restart it.
4. Check that it reports v9.26.7 and finishes synchronizing.

A normal upgrade from a working v9.26.6 node does not need a reindex. Keep
the required DigiDollar block history and your existing wallet files.

## User-facing changes

1. **Fixed:** `getblockchaininfo` and `getchainstates` read the difficulty
   directly from the tip block for the chain they describe.
   **Why:** The old default could walk far back through retired Groestl
   history while holding the main chain lock, delaying other requests.

   The scalar `difficulty` field now means the difficulty of that tip block,
   matching `getblockheader`. The separate `difficulties` object still reports
   the active algorithms. Historical Groestl blocks remain supported.

2. **Fixed:** Unused DigiDollar receive addresses created in Qt appear in
   `listdigidollaraddresses` when `include_empty` is true, with their labels.
   **Why:** Qt saved these addresses in the address book, but the RPC did not
   include that source. Empty addresses remain hidden by default. Ordinary
   DGB receive addresses and foreign contacts are not added to the list.

3. **Improved:** The Send DigiDollar amount tooltip explains that change must
   be zero or at least 1.00 DD.
   **Why:** Users should see this rule before trying a send. The existing
   $1 minimum output rule has not changed.

4. **Fixed:** Pending DigiDollar statistics updates finish their supply checks
   when the node shuts down.
   **Why:** A stop request could interrupt an update and make a normal shutdown
   report a fatal error. Startup scans can still be cancelled. Supply checks
   and the rules for accepting blocks are unchanged.

5. **Improved:** The wallet overview draws its version number from the build
   settings.
   **Why:** The artwork previously contained a fixed version number that could
   become outdated. The displayed version now follows the software build.

## Test and CI corrections

These changes improve how the project checks its code. They do not change
the rules used to accept transactions or blocks.

1. **Fixed:** The block-map memory test checks that entries use pooled storage.
   **Why:** Its old percentage threshold depended on the operating system's
   allocator and failed on Mac despite working pooled allocation.
2. **Fixed:** CodeQL excludes the retired `qa/rpc-tests` framework from imports.
   **Why:** It confused that old framework with the current functional tests.
   The current tests and security checks remain enabled.
3. **Fixed:** Isolated mainnet and testnet schedule checks use a small prune budget.
   **Why:** These nodes only read settings at genesis. A warning about room
   for a full chain was incorrectly failing the test on smaller CI disks.
4. **Fixed:** The Thaw Day node comparison waits for connected followers to
   download mined blocks before advancing its simulated clock.
   **Why:** A clock jump during an unfinished download can disconnect a test
   peer. Production timeouts and all accounting and reorg checks remain intact.
5. **Fixed:** Failed Linux and Mac CI jobs retain their unit and functional
   test logs, including the failed functional test's node logs.
   **Why:** The workflow previously requested a log file that these commands
   did not create. Missing logs made intermittent failures harder to diagnose.
   A failing test still fails the job.

## Verification so far

The latest Linux checks used source commit
`b41399e61166ce265706cd7881f891dae946f01f`. Later documentation commits do not
change the tested code.

| Check | Result |
| --- | --- |
| Core unit tests | All 3,762 cases passed; two existing partial-fixture warnings |
| Base functional tests | 401 passed, 17 skipped, including one isolated rerun explained below |
| Additional crash and pruning tests | All four reported success |
| Native Qt tests | 180 reported cases passed, none skipped |
| Sanitizer fuzz tests | All 256 targets passed with address, leak and undefined-behavior checks enabled |
| Cryptography and supporting libraries | All six test programs passed |
| Utility and RPC authentication tests | Passed |
| Repeated Thaw Day restart check | All 20 runs passed with execution limited to one CPU |

The optional script-data unit case also passed with real external test vectors.
The functional skips require older binaries, tracing support, special network
interfaces, or unsupported Signet features. A skip is not a pass.

The first functional run had one setup failure: the separate crash test and
the fee-estimation test selected the same local RPC port. The fee test passed
when rerun alone on the same source and port. No product code was changed for
that failure. Independently launched test suites need separate port ranges.

The shutdown fix has two new regression tests. Both failed before the fix and
pass with it. They check shutdown during a pending chain reorganization and
during the first Thaw Day update, then check supply after reopening the index.

The final fuzz run checked 85,933 saved inputs. Each of the two targets without
saved inputs generated new inputs for ten seconds. The run reported no
sanitizer errors. This is a bounded test, not proof that every possible input
is safe.

Desktop checks covered the changed tooltip and version display in light and
dark themes. An unused receive address created in Qt kept its RPC entry and
label after a wallet restart. These checks used an isolated test wallet.

Release packages have not been built. They still need checks on their target
systems before publication. These source tests do not replace package testing.

The Apple Silicon launch report also needs a test of the final Mac package.
The Mac packaging guide now requires checking the whole app's signature and
opening the exact package intended for release. This is a release check, not
a claim that the reported launch problem has been fixed.

The reported 45-minute startup has not been reproduced or explained. The RPC
speed fix above must not be described as a fix for that separate startup report.

## Existing limits

- Use `validateddaddress` for DD, TD and RD addresses. `validateaddress`
  continues to handle ordinary DigiByte addresses.
- `importdigidollaraddress` remains an unsupported no-op. This release does
  not add a new watch-only import feature.
- Address creation dates remain empty where no reliable date was recorded.
- A transfer waiting on a parent mint can still wait for the wallet's normal
  rebroadcast schedule. This release does not promise immediate rebroadcast.

Report sensitive security findings privately through GitHub's security
reporting page or `security@digibyte.io`.
