// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockheadercache.h>

#include <exception>
#include <new>
#include <utility>

namespace node {

BlockHeaderReadResult BlockHeaderReadResult::Found(CBlockHeader header)
{
    return {BlockHeaderReadStatus::FOUND, std::move(header), {}};
}

BlockHeaderReadResult BlockHeaderReadResult::NotFound()
{
    return {};
}

BlockHeaderReadResult BlockHeaderReadResult::Error(std::string error)
{
    return {BlockHeaderReadStatus::ERROR, std::nullopt, std::move(error)};
}

BlockHeaderCache::BlockHeaderCache(ReadFn reader, size_t max_entries)
    : m_reader(std::move(reader)), m_max_entries(max_entries)
{
}

BlockHeaderReadResult BlockHeaderCache::Read(const uint256& hash)
{
    uint64_t generation;
    {
        LOCK(m_mutex);
        const auto cached = m_lookup.find(hash);
        if (cached != m_lookup.end()) {
            m_headers.splice(m_headers.begin(), m_headers, cached->second);
            return BlockHeaderReadResult::Found(cached->second->second);
        }
        generation = m_generation;
    }

    BlockHeaderReadResult result;
    try {
        result = m_reader(hash);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        return BlockHeaderReadResult::Error(std::string{"Cannot read block header: "} + error.what());
    }

    switch (result.status) {
    case BlockHeaderReadStatus::NOT_FOUND:
        if (!result.header && result.error.empty()) return result;
        return BlockHeaderReadResult::Error("Block header reader returned an inconsistent result");
    case BlockHeaderReadStatus::ERROR:
        return BlockHeaderReadResult::Error(result.error.empty() ? "Cannot read block header" : std::move(result.error));
    case BlockHeaderReadStatus::FOUND:
        if (result.header && result.error.empty()) break;
        return BlockHeaderReadResult::Error("Block header reader returned an inconsistent result");
    default:
        return BlockHeaderReadResult::Error("Block header reader returned an unknown status");
    }

    if (result.header->GetHash() != hash) {
        return BlockHeaderReadResult::Error("Stored block header does not match the requested block hash");
    }
    if (m_max_entries == 0) return result;

    LOCK(m_mutex);
    if (generation != m_generation) return result;

    // Another reader may have filled this entry while storage was being read.
    const auto cached = m_lookup.find(hash);
    if (cached != m_lookup.end()) {
        m_headers.splice(m_headers.begin(), m_headers, cached->second);
        return result;
    }

    if (m_headers.size() == m_max_entries) {
        m_lookup.erase(m_headers.back().first);
        m_headers.pop_back();
    }
    m_headers.emplace_front(hash, *result.header);
    try {
        m_lookup.emplace(hash, m_headers.begin());
    } catch (...) {
        // Keep the two containers consistent if allocation fails.
        m_headers.pop_front();
        throw;
    }
    return result;
}

void BlockHeaderCache::Clear()
{
    LOCK(m_mutex);
    ++m_generation;
    m_lookup.clear();
    m_headers.clear();
}

size_t BlockHeaderCache::Size() const
{
    LOCK(m_mutex);
    return m_headers.size();
}

} // namespace node
