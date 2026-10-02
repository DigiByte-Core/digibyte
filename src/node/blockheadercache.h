// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_NODE_BLOCKHEADERCACHE_H
#define DIGIBYTE_NODE_BLOCKHEADERCACHE_H

#include <primitives/block.h>
#include <sync.h>
#include <util/hasher.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace node {

enum class BlockHeaderReadStatus { FOUND, NOT_FOUND, ERROR };

struct BlockHeaderReadResult {
    BlockHeaderReadStatus status{BlockHeaderReadStatus::NOT_FOUND};
    std::optional<CBlockHeader> header;
    std::string error;

    static BlockHeaderReadResult Found(CBlockHeader header);
    static BlockHeaderReadResult NotFound();
    static BlockHeaderReadResult Error(std::string error);
};

/** Read cache for immutable headers that have already been written to storage.
 *
 * The entry limit includes fixed-size headers and bounds their container
 * bookkeeping. It is not a byte limit on the reader or on returned values.
 * A zero limit disables storage in the cache. Dirty headers belong to the caller.
 *
 * Read returns an owned value, so callers never pin cache entries. The reader
 * runs outside the cache lock and must support concurrent calls. It must return
 * the same header for a given hash. Clear prevents earlier reads from filling
 * the cache again, but does not cancel their results. Destroy only after all
 * calls have finished. Standard reader exceptions become ERROR results, except
 * allocation failures, which propagate. This cache does not validate proof of
 * work or decide whether a header is acceptable to the chain.
 */
class BlockHeaderCache {
public:
    using ReadFn = std::function<BlockHeaderReadResult(const uint256&)>;

    BlockHeaderCache(ReadFn reader, size_t max_entries);
    BlockHeaderReadResult Read(const uint256& hash);
    void Clear();
    size_t Size() const;
    size_t Capacity() const { return m_max_entries; }

private:
    using Headers = std::list<std::pair<uint256, CBlockHeader>>;

    const ReadFn m_reader;
    const size_t m_max_entries;
    mutable Mutex m_mutex;
    uint64_t m_generation GUARDED_BY(m_mutex){0};
    Headers m_headers GUARDED_BY(m_mutex);
    std::unordered_map<uint256, Headers::iterator, BlockHasher> m_lookup GUARDED_BY(m_mutex);
};

} // namespace node

#endif // DIGIBYTE_NODE_BLOCKHEADERCACHE_H
