// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_TEST_UTIL_VALIDATION_H
#define DIGIBYTE_TEST_UTIL_VALIDATION_H

#include <validation.h>

#include <optional>
#include <string>
#include <utility>

class CValidationInterface;

/** Inspect a test prune lock and restore its original value on every exit. */
class BlockManagerTest
{
    node::BlockManager& m_blockman;
    const std::string m_name;
    std::optional<node::PruneLockInfo> m_original;

public:
    BlockManagerTest(const BlockManagerTest&) = delete;
    BlockManagerTest& operator=(const BlockManagerTest&) = delete;

    BlockManagerTest(node::BlockManager& blockman, std::string name)
        : m_blockman(blockman), m_name(std::move(name))
    {
        LOCK(cs_main);
        const auto found = m_blockman.m_prune_locks.find(m_name);
        if (found != m_blockman.m_prune_locks.end()) m_original = found->second;
    }

    ~BlockManagerTest()
    {
        LOCK(cs_main);
        if (m_original) m_blockman.m_prune_locks[m_name] = *m_original;
        else m_blockman.m_prune_locks.erase(m_name);
    }

    node::PruneLockInfo PruneLock() const
    {
        LOCK(cs_main);
        return m_blockman.m_prune_locks.at(m_name);
    }
};

struct TestChainstateManager : public ChainstateManager {
    /** Reset the ibd cache to its initial state */
    void ResetIbd();
    /** Toggle IsInitialBlockDownload from true to false */
    void JumpOutOfIbd();
};

class ValidationInterfaceTest
{
public:
    static void BlockConnected(
        ChainstateRole role,
        CValidationInterface& obj,
        const std::shared_ptr<const CBlock>& block,
        const CBlockIndex* pindex);
};

#endif // DIGIBYTE_TEST_UTIL_VALIDATION_H
