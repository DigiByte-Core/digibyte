# DigiByte Core v8.26.2 Release Notes

**Release Date:** January 2026
**Full Changelog:** https://github.com/DigiByte-Core/digibyte/compare/v8.26.1...v8.26.2

## Summary

DigiByte Core v8.26.2 is a maintenance release containing critical bug fixes for RPC performance and correctness issues. **All users are strongly encouraged to upgrade.**

This release addresses two bugs that were introduced during the Bitcoin Core v26.2 merge:

1. **Performance regression** in `getblockchaininfo` RPC (6-10x slowdown)
2. **Incorrect difficulty reporting** in `getmininginfo` RPC

## Notable Changes

### Fix: `getblockchaininfo` RPC Performance Regression (#345)

**Issue:** The `getblockchaininfo` RPC call was taking approximately 3 seconds to complete on nodes with fully synced chains, compared to sub-second responses in previous versions.

**Root Cause:** The Bitcoin Core v26.2 merge inadvertently changed the difficulty calculation to use `GetLastBlockIndexForAlgo()`, which performs an O(n) linear search through the blockchain to find the last block for each mining algorithm. With DigiByte's ~23 million blocks and 5-6 difficulty lookups per RPC call, this caused significant latency.

**Fix:** Changed to use `GetLastBlockIndexForAlgoFast()`, which uses DigiByte's pre-existing O(1) cached algorithm lookup infrastructure. This function is already proven in PoW validation code paths.

**Impact:**
- Before: ~3 seconds per RPC call
- After: ~0.3-0.5 seconds per RPC call
- **6-10x performance improvement**

### Fix: `getmininginfo` Difficulty Returns Wrong Algorithm (#346)

**Issue:** The `difficulty` field in `getmininginfo` RPC was returning a fixed value that didn't change regardless of the current mining algorithm. Investigation revealed it was always returning Groestl's difficulty (algo=2).

**Root Cause:** During the Bitcoin Core v26.2 merge, the `miningAlgo` parameter was accidentally omitted from the `GetDifficulty()` call, causing it to default to algorithm 2 (Groestl) instead of using the node's configured mining algorithm.

**Fix:** Restored the `miningAlgo` parameter to the `GetDifficulty()` call so that the reported difficulty correctly reflects the current mining algorithm.

**Impact:** Mining pools and monitoring tools now receive correct difficulty values for their configured algorithm.

## Files Changed

| File | Change |
|------|--------|
| `src/rpc/blockchain.cpp` | Use fast O(1) algorithm lookup for difficulty calculation |
| `src/rpc/mining.cpp` | Restore miningAlgo parameter to GetDifficulty() call |

## Upgrade Instructions

### For Node Operators

1. Stop your DigiByte node: `digibyte-cli stop`
2. Replace binaries with v8.26.2 release
3. Start node: `digibyted`

No configuration changes or reindex required.

### For Miners and Mining Pools

This release is **critical** for mining operations. The `getmininginfo` bug caused incorrect difficulty reporting which could affect stratum difficulty calculations.

1. Update to v8.26.2 immediately
2. Verify correct difficulty reporting: `digibyte-cli getmininginfo`
3. Confirm the `difficulty` field matches your configured mining algorithm

### For Exchanges and Services

The performance improvement in `getblockchaininfo` may significantly reduce RPC latency for monitoring and status checks. No action required beyond the standard upgrade process.

## Testing

Both fixes include regression tests to prevent future occurrences:
- Performance benchmarks for `getblockchaininfo` RPC
- Algorithm-specific difficulty verification in `feature_digibyte_multialgo_mining.py`

## Credits

Thanks to the community members who reported these issues and contributed to the investigation:

- Issue #345 reported by community members experiencing RPC slowdowns
- Issue #346 reported by mining pool operators noticing incorrect difficulty values

## SHA256 Checksums

SHA256 checksums for all release binaries are published on the [GitHub Releases page](https://github.com/DigiByte-Core/digibyte/releases/tag/v8.26.2).

## Previous Release

For changes in v8.26.1, see: https://github.com/DigiByte-Core/digibyte/releases/tag/v8.26.1
