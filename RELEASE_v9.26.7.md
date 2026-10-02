# DigiByte Core v9.26.7 release notes

**Draft for review. Release packages have not been built or published.**

v9.26.7 is a small follow-up to v9.26.6. It fixes slow chain-information
requests and improves two DigiDollar wallet details. It includes **three
user-facing changes** and **four test and CI corrections**.

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
