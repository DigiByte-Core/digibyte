// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_INDEX_DIGIDOLLARSTATSINDEX_H
#define DIGIBYTE_INDEX_DIGIDOLLARSTATSINDEX_H

#include <consensus/amount.h>
#include <consensus/digidollar_state.h>
#include <index/base.h>
#include <uint256.h>

#include <cstdint>
#include <optional>

class CBlockIndex;
class CDBBatch;

static constexpr bool DEFAULT_DIGIDOLLARSTATSINDEX{true};

/**
 * DigiDollarStats holds network-wide DigiDollar statistics at a specific block height.
 * These stats track the total DigiDollar supply, locked collateral, and active vaults.
 */
struct DigiDollarStats {
    CAmount total_dd_supply{0};      //!< Total DigiDollar supply in circulation (in cents)
    CAmount total_collateral{0};     //!< Total DGB locked as collateral (in satoshis)
    uint64_t vault_count{0};         //!< Number of active DigiDollar vaults
    int height{0};                   //!< Block height for these statistics
    uint256 block_hash;              //!< Block hash for these statistics
    //! Available only when the chainstate record represents this exact block.
    //! Old serialized index rows never supply canonical validation inputs.
    std::optional<DigiDollar::ChainstateHealth> canonical_health;

    DigiDollarStats() = default;
    DigiDollarStats(CAmount dd_supply, CAmount collateral, uint64_t vaults, int block_height, const uint256& hash)
        : total_dd_supply(dd_supply), total_collateral(collateral), vault_count(vaults), height(block_height), block_hash(hash) {}
};

/**
 * DigiDollarStatsIndex maintains network-wide statistics on the DigiDollar stablecoin system.
 *
 * This index tracks:
 * - Total DigiDollar supply in circulation
 * - Total DGB locked as collateral
 * - Number of active vaults
 *
 * The index is updated incrementally during block processing, similar to CoinStatsIndex,
 * allowing efficient queries of historical DigiDollar statistics at any block height.
 */
class DigiDollarStatsIndex final : public BaseIndex
{
private:
    std::unique_ptr<BaseIndex::DB> m_db;

    // Network-wide DigiDollar statistics (incremental state)
    CAmount m_total_dd_supply{0};        //!< Running total of DigiDollar supply
    bool m_supply_known{true};          //!< False once retained metadata leaves circulation unknown
    //! A saved baseline is independently checked once the chain reaches Thaw Day.
    bool m_supply_verified{false};
    CAmount m_total_collateral{0};       //!< Running total of locked collateral
    uint64_t m_vault_count{0};           //!< Running count of active vaults

    bool AllowPrune() const override { return true; }

protected:
    /**
     * Initialize internal state from the database and block index.
     * Loads the last known statistics from the database.
     */
    bool CustomInit(const std::optional<interfaces::BlockKey>& block) override;

    /**
     * Commit current statistics to the database.
     * Called periodically to persist the incremental state.
     */
    bool CustomCommit(CDBBatch& batch) override;

    /**
     * Process a newly connected block and update DigiDollar statistics.
     * Scans the block for DigiDollar transactions and updates running totals.
     */
    bool CustomAppend(const interfaces::BlockInfo& block) override;

    /**
     * Rewind index to an earlier chain tip during a chain reorganization.
     * Reverses the effects of blocks that are no longer in the active chain.
     */
    bool CustomRewind(const interfaces::BlockKey& current_tip, const interfaces::BlockKey& new_tip) override;

    BaseIndex::DB& GetDB() const override { return *m_db; }

public:
    /**
     * Constructs the DigiDollarStatsIndex, which becomes available to be queried.
     *
     * @param chain Interface to the blockchain
     * @param n_cache_size Size of the database cache
     * @param f_memory If true, use an in-memory database
     * @param f_wipe If true, wipe the existing database
     */
    explicit DigiDollarStatsIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    /**
     * Look up DigiDollar statistics for a specific block.
     *
     * @param block_index The block to query statistics for
     * @return Statistics only when circulation is known; otherwise std::nullopt,
     * including indexed history with an amount absent from retained metadata.
     */
    std::optional<DigiDollarStats> LookUpStats(const CBlockIndex& block_index) const;
};

/// The global DigiDollar statistics index object.
extern std::unique_ptr<DigiDollarStatsIndex> g_digidollar_stats_index;

#endif // DIGIBYTE_INDEX_DIGIDOLLARSTATSINDEX_H
