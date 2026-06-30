# Fresh Node Header Pre-sync Reset: Root Cause and Binary Fix

Date investigated: 2026-06-30

## Summary

Fresh v9.26.2 DigiByte nodes can appear to sync headers to ~99.98%, then drop
back to an older header height and start climbing again. This is not normal block
sync progress. It is the low-work header pre-synchronization phase repeatedly
failing before the node commits headers into the block index.

The local fresh Qt node reproduced the issue even while connected to current
manual v9.26.2 peers. Good peers improved the experiment, but did not fix the
failure. The confirmed failure was:

```text
2026-06-30T20:38:34Z Pre-synchronizing blockheaders, height: 23760000 (~99.98%)
2026-06-30T20:38:37Z [net] Initial headers sync aborted with peer=1: incomplete headers message at height=23765928 (presync phase)
2026-06-30T20:38:39Z Pre-synchronizing blockheaders, height: 16840000 (~70.92%)
```

At the same time, RPC still showed:

```json
{
  "blocks": 0,
  "headers": 0,
  "initialblockdownload": true
}
```

That means the node never accepted the downloaded headers. It only discarded the
failed presync candidate and fell back to another peer's partially progressed
presync state.

## What We Changed During the Test

The running Qt process was restarted with this `digibyte.conf`:

```ini
[main]
server=1
listen=1
txindex=1
digidollar=1

addnode=oracle1.digibyte.io:12024
addnode=digihash.digibyte.io:12024
addnode=digiexplorer.info:12024
addnode=64.182.71.56
```

The manual peers connected and were current v9.26.2 peers. The leading presync
stream was carried by `oracle1.digibyte.io`, then by `digihash.digibyte.io`.
Both reached the high 99% range and then failed in the same way.

This proves the addnodes are useful for diagnosis and peer quality, but they are
not a complete fix. The failure can happen with good current peers.

## What This Is Not

This is not evidence that v9.26.2 is rejecting grandfathered Groestl history.
The observed logs did not show `bad-algo`, `InvalidChainFound`, or a block
validation failure. The Groestl incident created a split and left stale peers on
the network, but this fresh-sync failure is in the header presync chainwork
calculation path.

This is also not a Qt GUI display-only bug. The GUI/log percentage is showing
presync progress. RPC confirmed the real block index still had zero accepted
headers.

## How Header Presync Is Supposed To Work

Fresh nodes do not immediately store a long unknown header chain. To avoid memory
DoS from low-work headers, v9.26.2 first downloads headers in a `PRESYNC` phase.
During presync, it calculates cumulative work and stores sparse commitments.

Only after the candidate chain reaches `nMinimumChainWork` should the node switch
to `REDOWNLOAD`, download the same headers again, verify the commitments, and
then feed headers into normal validation.

If a peer reaches the end of its headers while the node is still in `PRESYNC`,
the code intentionally aborts:

```cpp
// src/headerssync.cpp
// If we're in PRESYNC and we get a non-full headers message, then the peer's
// chain has ended and definitely doesn't have enough work, so we can stop our sync.
LogPrint(BCLog::NET, "Initial headers sync aborted ... incomplete headers message ...");
```

That exact abort happened at height `23,765,928`.

## Root Cause

The presync chainwork calculation is contextless, but DigiByte's chainwork
calculation is context-sensitive after the DigiSpeed/MultiShield work changes.

Current presync code:

```cpp
// src/headerssync.cpp
m_current_chain_work += GetBlockProof(CBlockIndex(current));
```

Related batch helper:

```cpp
// src/validation.cpp
arith_uint256 CalculateHeadersWork(const std::vector<CBlockHeader>& headers)
{
    arith_uint256 total_work{0};
    for (const CBlockHeader& header : headers) {
        CBlockIndex dummy(header);
        total_work += GetBlockProof(dummy);
    }
    return total_work;
}
```

But `CBlockIndex(const CBlockHeader&)` only copies header fields. It does not set
the real previous block pointer, height, skip pointer, max time, chainwork, or
last-per-algo links:

```cpp
// src/chain.cpp
CBlockIndex::CBlockIndex(const CBlockHeader& block)
    : nVersion(block.nVersion),
      hashMerkleRoot(block.hashMerkleRoot),
      nTime(block.nTime),
      nBits(block.nBits),
      nNonce(block.nNonce)
{
    ...
}
```

`GetBlockProof()` then reads `block.nHeight` and `block.pprev`:

```cpp
// src/chain.cpp
int nHeight = block.nHeight;

if (nHeight < params.workComputationChangeTarget) {
    ...
} else {
    for (int i = 0; i < NUM_ALGOS_IMPL; i++) {
        if (!IsAlgoActive(block.pprev, params, i))
            continue;
        unsigned int nBits = GetNextWorkRequired(block.pprev, &header, params, i);
        ...
    }
}
```

For dummy header indexes, `nHeight` is `0` and `pprev` is null. That means fresh
header presync can compute the wrong work for modern DigiByte headers. The peer
can be honest and current, but the local presync work total never reaches
`nMinimumChainWork` before the peer's header chain ends. The code then correctly
aborts from its own point of view, causing the visible 99% -> 70% style reset.

## Correct Binary Fix

The correct binary fix is to make presync header work calculation contextual.
Do not lower `nMinimumChainWork` in the release binary as the primary fix. That
would hide the symptom by weakening the anti-DoS gate and would make fresh nodes
easier to feed low-work header chains.

The fix should preserve enough temporary header context during presync and
redownload to compute DigiByte work the same way normal block-index acceptance
does.

## Code Validation: Triple Check

The observed runtime failure matches the source path exactly:

1. `src/net_processing.cpp` starts low-work headers sync when the first header
   batch does not prove enough work:

   ```cpp
   arith_uint256 total_work = chain_start_header->nChainWork + CalculateHeadersWork(headers);
   ...
   peer.m_headers_sync.reset(new HeadersSyncState(...));
   ```

2. `src/headerssync.cpp` only transitions out of `PRESYNC` if the accumulated
   work reaches `m_minimum_required_work`:

   ```cpp
   if (m_current_chain_work >= m_minimum_required_work) {
       m_download_state = State::REDOWNLOAD;
   }
   ```

3. If the peer reaches the end of headers and the node is still in `PRESYNC`,
   the exact logged abort is emitted:

   ```cpp
   LogPrint(BCLog::NET,
       "Initial headers sync aborted with peer=%d: incomplete headers message at height=%i (presync phase)\n",
       m_id, m_current_height);
   ```

4. The work value that decides the transition is currently computed from a dummy
   `CBlockIndex`:

   ```cpp
   m_current_chain_work += GetBlockProof(CBlockIndex(current));
   ```

5. That dummy index has `nHeight == 0`, `pprev == nullptr`, and no real
   `lastAlgoBlocks[]` chain. `GetBlockProof()` reads those fields, so this is
   the wrong input for modern mainnet DigiByte headers.

This is why the node can reach real current peer height, still fail the
`m_current_chain_work >= m_minimum_required_work` check, and then abort with the
observed presync message.

### Required Behavior

For every presync header, the node needs to compute work using:

- the correct height,
- the previous header/index context,
- the active algorithm set at that height,
- the previous block for each algorithm,
- the recent previous-header window needed by `GetNextWorkRequiredV4()`,
- the same last-per-algo behavior used by normal `AddToBlockIndex()`.

Normal block-index insertion does this in `src/node/blockstorage.cpp`:

```cpp
pindexNew->pprev = &(*miPrev).second;
pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
memcpy(pindexNew->lastAlgoBlocks, pindexNew->pprev->lastAlgoBlocks, sizeof(pindexNew->lastAlgoBlocks));
...
pindexNew->lastAlgoBlocks[algo] = pindexNew;
pindexNew->nChainWork = pindexNew->pprev->nChainWork + GetBlockProof(*pindexNew);
```

Presync needs an equivalent temporary context, without permanently storing every
header in the block index during the anti-DoS phase.

### KISS: Smallest Viable Patch

Keep the fix local to header-work accounting. Do not change peer selection,
Groestl rules, BIP9 activation, Qt progress display, or `nMinimumChainWork`.

Add one small helper, for example `HeadersSyncWorkCursor`, owned by
`HeadersSyncState` and reusable by `CalculateHeadersWork()`.

The helper should do only this:

1. Start from `m_chain_start`.
2. For each header, create one temporary `CBlockIndex`-like entry.
3. Set the fields that normal `AddToBlockIndex()` sets before computing work:
   `phashBlock`, `pprev`, `nHeight`, `nTimeMax`, and `lastAlgoBlocks[]`.
4. Return `GetBlockProof(temp_index)`.
5. Advance the cursor tip to that temporary entry.
6. Prune old temporary entries while keeping pointers valid.

Important implementation detail: temporary indexes must have stable block hashes.
`CBlockIndex::GetBlockHeader()` calls `pprev->GetBlockHash()`, so each temporary
entry must own or otherwise retain its hash and set `phashBlock` to that stable
hash.

A minimal helper shape:

```cpp
struct HeaderWorkEntry {
    CBlockIndex index;
    uint256 hash;

    explicit HeaderWorkEntry(const CBlockHeader& header)
        : index(header), hash(header.GetHash())
    {
        index.phashBlock = &hash;
    }
};

class HeadersSyncWorkCursor {
public:
    explicit HeadersSyncWorkCursor(const CBlockIndex* start);

    arith_uint256 AddHeader(const CBlockHeader& header)
    {
        auto entry = std::make_unique<HeaderWorkEntry>(header);
        CBlockIndex& idx = entry->index;

        idx.pprev = const_cast<CBlockIndex*>(m_tip);
        idx.nHeight = m_tip->nHeight + 1;
        idx.nTimeMax = std::max<unsigned int>(m_tip->nTimeMax, idx.nTime);

        std::memcpy(idx.lastAlgoBlocks, m_tip->lastAlgoBlocks,
                    sizeof(idx.lastAlgoBlocks));
        const int algo = idx.GetAlgo();
        if (algo >= 0 && algo < NUM_ALGOS_IMPL) {
            idx.lastAlgoBlocks[algo] = &idx;
        }

        const arith_uint256 work = GetBlockProof(idx);
        idx.nChainWork = m_tip->nChainWork + work;

        m_tip = &idx;
        m_entries.emplace_back(std::move(entry));
        PruneButKeepRequiredContext();
        return work;
    }

private:
    const CBlockIndex* m_tip;
    std::deque<std::unique_ptr<HeaderWorkEntry>> m_entries;
};
```

Use `std::unique_ptr` entries, or another stable-allocation container, so
`pprev`, `phashBlock`, and `lastAlgoBlocks[]` pointers are not invalidated by
container moves.

The minimum retained history should cover:

- `NUM_ALGOS * consensus.nAveragingInterval` previous blocks for V4 retarget,
- `CBlockIndex::nMedianTimeSpan` extra history for median-time calls,
- the current last block for each algorithm, even if it falls outside the recent
  pprev window.

For mainnet defaults, this is a small bounded window plus at most one pinned
last block per algorithm. It is not 23 million retained headers.

### Exact Minimal Call-Site Changes

Change these work additions:

```cpp
// current
m_current_chain_work += GetBlockProof(CBlockIndex(current));
m_redownload_chain_work += GetBlockProof(CBlockIndex(header));
```

to:

```cpp
// proposed
m_current_chain_work += m_presync_work_cursor.AddHeader(current);
m_redownload_chain_work += m_redownload_work_cursor.AddHeader(header);
```

On transition to `REDOWNLOAD`, reset the redownload cursor to `m_chain_start`,
the same way `m_redownload_chain_work` is reset today.

Also replace the initial anti-DoS batch estimate:

```cpp
// current
chain_start_header->nChainWork + CalculateHeadersWork(headers)
```

with a contextual overload:

```cpp
// proposed
chain_start_header->nChainWork + CalculateHeadersWork(*chain_start_header, headers)
```

where the overload starts a cursor at `chain_start_header`, adds the headers with
context, and returns the delta work for those headers. This keeps the existing
helper contract: `CalculateHeadersWork(...)` returns the work contributed by the
headers passed in, not the total chainwork including the start block.

### Functions To Stop Using Contextlessly

These should not compute modern DigiByte work from `CBlockIndex(header)` alone:

- `HeadersSyncState::ValidateAndProcessSingleHeader()`
- `HeadersSyncState::ValidateAndStoreRedownloadedHeader()`
- `CalculateHeadersWork(const std::vector<CBlockHeader>& headers)`

The batch helper probably needs a contextual overload:

```cpp
arith_uint256 CalculateHeadersWork(const CBlockIndex& chain_start,
                                   const std::vector<CBlockHeader>& headers);
```

Callers that already know the previous block should use that overload. The
contextless overload should either be removed, limited to cases where context is
provably irrelevant, or clearly marked unsafe for mainnet modern DGB headers.

## Smallest Files To Touch

The smallest correct patch should be limited to:

- `src/headerssync.h`
- `src/headerssync.cpp`
- `src/validation.h`
- `src/validation.cpp`
- `src/net_processing.cpp` only to pass `chain_start_header` into contextual
  `CalculateHeadersWork()`.

No consensus parameter, chainparam, Groestl activation, peer discovery, wallet,
Qt, or block validation rule changes are required for this bug.

## What Not To Ship As The Fix

Do not ship a release whose real fix is only:

- more `addnode=` entries,
- `connect=` only to trusted peers,
- `dnsseed=0`,
- lowering or zeroing `nMinimumChainWork`,
- deleting `peers.dat`,
- changing Qt display code,
- disabling header presync entirely without replacing the DoS protection.

Those can help isolate or temporarily work around behavior in a controlled
environment, but they do not fix fresh-node sync for normal users.

## Temporary Operator Workarounds

Until a fixed binary is released:

- A datadir that already has accepted headers/blocks past `nMinimumChainWork`
  should not hit this exact fresh-presync loop.
- A trusted bootstrap or snapshot can get a node past the broken fresh presync
  stage.
- Good `addnode=` entries improve peer quality and reduce stale-chain noise, but
  they do not guarantee success because the confirmed failure occurs with current
  v9.26.2 peers.

For public user guidance, be careful not to promise that adding peers fixes the
fresh sync loop. The correct user-facing fix is a new binary with contextual
presync work calculation.

## Validation Plan For The Fixed Binary

A fixed binary should be validated with all of the following:

1. Fresh mainnet datadir.
2. Known current v9.26.2+ peers.
3. `-debug=net` enabled for the test run.
4. Confirm presync reaches near tip and logs an `Initial headers sync transition`
   into redownload instead of:
   `Initial headers sync aborted ... incomplete headers message ... (presync phase)`.
5. Confirm `getblockchaininfo` changes from `headers=0` to accepted headers
   greater than zero.
6. Confirm block download begins.
7. Confirm no `bad-algo` or `InvalidChainFound` appears while syncing the
   grandfathered Groestl history.
8. Confirm intentionally low-work header chains are still rejected before being
   stored, preserving the anti-DoS goal.

## Regression Tests To Add

Add a focused unit test around `HeadersSyncState`:

- Construct or fixture-load a chain of headers whose work is above
  `nMinimumChainWork` only when DigiByte contextual work is computed correctly.
- Feed it through presync in max-size header batches.
- Assert that the state transitions to `REDOWNLOAD` before an incomplete final
  headers message arrives.
- Assert that the old contextless dummy work path would not reach the threshold.

Add a smaller synthetic test around the work calculator:

- Use a chain crossing `workComputationChangeTarget`.
- Verify contextual header work matches normal `AddToBlockIndex()` chainwork.
- Verify `CBlockIndex(header)` alone is not used for post-change mainnet work.

Add an integration test if practical:

- Start a fresh node against a controlled peer serving a mainnet-like header
  sequence.
- Require that the node accepts headers and begins block download.
- Fail the test if the log contains
  `Initial headers sync aborted ... incomplete headers message ... (presync phase)`.

## Bottom Line

The fresh-node reset is caused by presync never proving enough work before the
peer reaches the current header tip. The reason it fails to prove enough work is
that the binary computes presync header work without the height and previous
header context required by DigiByte's modern multi-algo work rules.

The correct binary fix is contextual presync work calculation with bounded
temporary header-index state, not peer-list tuning and not weakening
`nMinimumChainWork`.
