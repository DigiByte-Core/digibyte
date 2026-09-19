// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/int128.h>
#include <index/digidollarstatsindex.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/digidollar.h>
#include <consensus/merkle.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <node/context.h>
#include <digidollar/validation.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <serialize.h>
#include <undo.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <limits>
#include <map>
#include <string>

static constexpr uint8_t DB_BLOCK_HASH{'d'};
static constexpr uint8_t DB_BLOCK_HEIGHT{'h'};
static constexpr uint8_t DB_FORMAT_VERSION{'S'};
// Row keys also carry this version so old fork rows stay outside a rebuilt index.
static constexpr uint32_t SUPPLY_FORMAT_VERSION{3};

namespace {

/**
 * Database value structure for DigiDollar statistics.
 * Stores the complete state at a specific block height.
 */
struct DigiDollarDBVal {
    CAmount total_dd_supply;
    CAmount total_collateral;
    uint64_t vault_count;
    bool supply_known{true}; //!< total_dd_supply is usable only when this is true

    SERIALIZE_METHODS(DigiDollarDBVal, obj)
    {
        READWRITE(obj.total_dd_supply);
        READWRITE(obj.total_collateral);
        READWRITE(obj.vault_count);
        READWRITE(obj.supply_known);
    }
};

/**
 * Database key for height-based lookups.
 */
struct DBHeightKey {
    int height;

    explicit DBHeightKey(int height_in) : height(height_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_BLOCK_HEIGHT);
        ser_writedata32be(s, SUPPLY_FORMAT_VERSION);
        ser_writedata32be(s, height);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const uint8_t prefix{ser_readdata8(s)};
        if (prefix != DB_BLOCK_HEIGHT) {
            throw std::ios_base::failure("Invalid format for digidollarstatsindex DB height key");
        }
        if (ser_readdata32be(s) != SUPPLY_FORMAT_VERSION) {
            throw std::ios_base::failure("Invalid version for digidollarstatsindex DB height key");
        }
        height = ser_readdata32be(s);
    }
};

/**
 * Database key for hash-based lookups.
 */
struct DBHashKey {
    uint256 block_hash;

    explicit DBHashKey(const uint256& hash_in) : block_hash(hash_in) {}

    SERIALIZE_METHODS(DBHashKey, obj)
    {
        uint8_t prefix{DB_BLOCK_HASH};
        READWRITE(prefix);
        if (prefix != DB_BLOCK_HASH) {
            throw std::ios_base::failure("Invalid format for digidollarstatsindex DB hash key");
        }
        uint32_t version{SUPPLY_FORMAT_VERSION};
        READWRITE(version);
        if (version != SUPPLY_FORMAT_VERSION) {
            throw std::ios_base::failure("Invalid version for digidollarstatsindex DB hash key");
        }
        READWRITE(obj.block_hash);
    }
};

/** Read supply from chain data without changing either validation health cache. */
class SupplyVerification {
    Chainstate& m_chainstate;
    std::map<uint256, CBlock> m_source_blocks;

    bool Cancelled(std::string& error) const
    {
        if (!m_chainstate.m_chainman.m_interrupt) return false;
        error = "DigiDollar circulating supply verification interrupted";
        return true;
    }

    bool ReadBlock(const CBlockIndex& index, CBlock& block, std::string& error) const
    {
        bool mutated{false};
        if (!m_chainstate.m_blockman.ReadBlockFromDisk(block, index) ||
            block.GetHash() != index.GetBlockHash() ||
            BlockMerkleRoot(block, &mutated) != block.hashMerkleRoot || mutated) {
            error = strprintf("DigiDollar supply not ready: restore the block at height %d (%s)",
                              index.nHeight, index.GetBlockHash().ToString());
            return false;
        }
        return true;
    }

    DigiDollar::CanonicalTxLookup Lookup(const CBlockIndex& tip)
    {
        return [this, &tip](const uint256& txid, uint32_t height, CTransactionRef& out) {
            if (height > static_cast<uint32_t>(tip.nHeight)) return false;
            const auto* source = tip.GetAncestor(height);
            if (!source) return false;
            const uint256 hash = source->GetBlockHash();
            auto it = m_source_blocks.find(hash);
            if (it == m_source_blocks.end()) {
                CBlock block;
                std::string error;
                if (!ReadBlock(*source, block, error)) return false;
                if (m_source_blocks.size() >= 8) m_source_blocks.erase(m_source_blocks.begin());
                it = m_source_blocks.emplace(hash, std::move(block)).first;
            }
            for (const auto& tx : it->second.vtx) {
                if (tx->GetHash() == txid) { out = tx; return true; }
            }
            return false;
        };
    }

    bool ApplyBlock(const CBlockIndex& index, bool undo, CAmount& supply, bool& known, std::string& error)
    {
        if (Cancelled(error)) return false;
        const auto& params = m_chainstate.m_chainman.GetConsensus();
        if (index.nHeight == 0 || index.nHeight < params.DigiDollarHeight) return true;
        CBlock block;
        if (!ReadBlock(index, block, error)) return false;
        CBlockUndo block_undo;
        if (!m_chainstate.m_blockman.UndoReadFromDisk(block_undo, index) ||
            block_undo.vtxundo.size() + 1 != block.vtx.size()) {
            error = strprintf("DigiDollar supply not ready: restore undo data at height %d (%s)",
                              index.nHeight, index.GetBlockHash().ToString());
            return false;
        }
        const auto lookup = Lookup(index);
        util::int128_t change{0};
        for (size_t i = 1; i < block.vtx.size(); ++i) {
            CAmount delta{0};
            const auto result = DigiDollar::GetCirculatingSupplyChange(*block.vtx[i], block_undo.vtxundo[i - 1].vprevout,
                    index.nHeight, params, lookup, delta, error);
            if (result == DigiDollar::SupplyChangeResult::FAILURE) return false;
            if (result == DigiDollar::SupplyChangeResult::UNKNOWN_METADATA) known = false;
            else change += delta;
        }
        if (!known) return true;
        const util::int128_t next = static_cast<util::int128_t>(supply) + (undo ? -change : change);
        if (next < 0 || next > std::numeric_limits<CAmount>::max()) {
            error = "DigiDollar supply not ready: reconstructed token total is out of range";
            return false;
        }
        supply = static_cast<CAmount>(next);
        return true;
    }

public:
    explicit SupplyVerification(Chainstate& chainstate) : m_chainstate{chainstate} {}

    bool Move(const CBlockIndex& from, const CBlockIndex& to, CAmount& supply, bool& known, std::string& error)
    {
        const auto* fork = LastCommonAncestor(&from, &to);
        if (!fork) { error = "DigiDollar supply not ready: index ancestry is unavailable"; return false; }
        auto last_progress = SteadyClock::now();
        uint64_t checked{0};
        const auto progress = [&] {
            ++checked;
            if (SteadyClock::now() - last_progress >= std::chrono::seconds{1}) {
                LogPrintf("DigiDollar supply verification: %u branch blocks checked\n", checked);
                last_progress = SteadyClock::now();
            }
        };
        for (auto* index = &from; index != fork; index = index->pprev) {
            if (!ApplyBlock(*index, true, supply, known, error)) return false;
            progress();
        }
        for (int height = fork->nHeight + 1; height <= to.nHeight; ++height) {
            if (!ApplyBlock(*to.GetAncestor(height), false, supply, known, error)) return false;
            progress();
        }
        return !Cancelled(error);
    }

    bool At(const CBlockIndex& target, CAmount& supply, bool& known, std::string& error)
    {
        AssertLockHeld(cs_main);
        if (Cancelled(error)) return false;
        const auto& params = m_chainstate.m_chainman.GetConsensus();
        if (target.nHeight == 0 || target.nHeight < params.DigiDollarHeight) {
            supply = 0;
            known = true;
            return true;
        }
        const auto* tip = m_chainstate.m_chain.Tip();
        if (!tip || m_chainstate.CoinsTip().GetBestBlock() != tip->GetBlockHash()) {
            error = "DigiDollar supply not ready: UTXO state does not match the chain tip";
            return false;
        }
        LogPrintf("DigiDollar supply verification: checking saved block %s from UTXO tip %s\n",
                  target.GetBlockHash().ToString(), tip->GetBlockHash().ToString());
        DigiDollar::ChainstateHealth unused_health;
        if (!DigiDollar::ReconstructChainstateHealth(m_chainstate.CoinsTip(), params, Lookup(*tip),
                unused_health, error, [&] { return Cancelled(error); }, &supply,
                [](uint64_t completed, uint64_t) {
                    LogPrintf("DigiDollar supply verification: %u outputs checked\n", completed);
                }, &known)) return false;
        // A saved index can lag or belong to a disconnected branch. Translate
        // the independently counted tip supply using actual block token deltas.
        return Move(*tip, target, supply, known, error);
    }
};

} // namespace

std::unique_ptr<DigiDollarStatsIndex> g_digidollar_stats_index;

DigiDollarStatsIndex::DigiDollarStatsIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "digidollarstatsindex")
{
    fs::path path{gArgs.GetDataDirNet() / "indexes" / "digidollarstats"};
    fs::create_directories(path);

    m_db = std::make_unique<DigiDollarStatsIndex::DB>(path / "db", n_cache_size, f_memory, f_wipe);
    uint32_t format{0};
    if (!m_db->Read(DB_FORMAT_VERSION, format) || format != SUPPLY_FORMAT_VERSION) {
        // Only the derived index is rebuilt. No pre-DigiDollar block can hold
        // tokens, so retained DigiDollar history is sufficient on pruned nodes.
        CDBBatch batch(*m_db);
        CBlockLocator locator;
        if (auto* node = m_chain->context(); node && node->chainman) {
            LOCK(cs_main);
            auto& chain = node->chainman->GetChainstateForIndexing().m_chain;
            const int floor = node->chainman->GetConsensus().DigiDollarHeight;
            if (chain.Tip()) {
                const int anchor_height = std::min(chain.Height(), std::max(0, floor - 1));
                const auto* anchor = chain[anchor_height];
                locator.vHave.push_back(anchor->GetBlockHash());
                batch.Write(DBHeightKey(anchor_height), std::make_pair(anchor->GetBlockHash(), DigiDollarDBVal{0, 0, 0}));
            }
        }
        m_db->WriteBestBlock(batch, locator);
        batch.Write(DB_FORMAT_VERSION, SUPPLY_FORMAT_VERSION);
        if (!m_db->WriteBatch(batch)) throw std::runtime_error("Cannot initialize DigiDollar supply index migration");
        LogPrintf("DigiDollar stats index: rebuilding token supply from retained DigiDollar history\n");
    }

}

bool DigiDollarStatsIndex::CustomInit(const std::optional<interfaces::BlockKey>& block)
{
    m_supply_verified = false;
    if (block) {
        // Load existing state from database
        std::pair<uint256, DigiDollarDBVal> read_out;
        if (!m_db->Read(DBHeightKey(block->height), read_out)) {
            return error("%s: Cannot read current %s state; index may be corrupted",
                         __func__, GetName());
        }

        // Verify block hash matches
        if (read_out.first != block->hash) {
            LogPrintf("WARNING: %s height index has unexpected block %s; expected %s\n",
                      GetName(), read_out.first.ToString(), block->hash.ToString());

            // Try hash-based lookup
            if (!m_db->Read(DBHashKey(block->hash), read_out.second)) {
                return error("%s: Cannot read current %s state; index may be corrupted",
                             __func__, GetName());
            }
        }

        // Restore state from database
        m_total_dd_supply = read_out.second.total_dd_supply;
        m_supply_known = read_out.second.supply_known;
        m_total_collateral = read_out.second.total_collateral;
        m_vault_count = read_out.second.vault_count;

        const auto* tip = m_chainstate->m_chain.Tip();
        if (tip && DigiDollar::IsThawDayActive(m_chainstate->m_chainman.GetConsensus(), tip->nHeight)) {
            const auto* indexed = m_chainstate->m_blockman.LookupBlockIndex(block->hash);
            CAmount verified{0};
            bool known{false};
            std::string reason;
            if (!indexed || indexed->nHeight != block->height ||
                !SupplyVerification{*m_chainstate}.At(*indexed, verified, known, reason))
                return error("DigiDollar stats index: %s", reason);
            if (m_supply_known != known || (known && m_total_dd_supply != verified)) {
                if (known) read_out.second.total_dd_supply = verified;
                read_out.second.supply_known = known;
                const bool written = read_out.first == block->hash ?
                    m_db->Write(DBHeightKey(block->height), read_out) :
                    m_db->Write(DBHashKey(block->hash), read_out.second);
                if (!written) return error("DigiDollar stats index: cannot save repaired circulating supply");
                if (known) {
                    LogPrintf("DigiDollar circulating supply repaired at block %s: %d -> %d cents\n",
                              block->hash.ToString(), m_total_dd_supply, verified);
                    m_total_dd_supply = verified;
                }
                m_supply_known = known;
            } else if (known) {
                LogPrintf("DigiDollar circulating supply verified at block %s: %d cents\n",
                          block->hash.ToString(), verified);
            }
            if (!known) LogPrintf("DigiDollar circulating supply remains unavailable from retained token metadata at block %s\n", block->hash.ToString());
            m_supply_verified = true;
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollarStatsIndex: Initialized from height %d - DD Supply: %s, Collateral: %d, Vaults: %d\n",
                 block->height, m_supply_known ? std::to_string(m_total_dd_supply) : "unavailable", m_total_collateral, m_vault_count);
    } else {
        // Starting from genesis - initialize to zero
        m_total_dd_supply = 0;
        m_supply_known = true;
        m_total_collateral = 0;
        m_vault_count = 0;
        m_supply_verified = true;

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollarStatsIndex: Initialized from genesis\n");
    }

    return true;
}

bool DigiDollarStatsIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    assert(block.data);
    const CBlock& cblock = *block.data;
    const auto& params = m_chainstate->m_chainman.GetConsensus();
    const CBlockIndex* pindex = WITH_LOCK(cs_main, return m_chainstate->m_blockman.LookupBlockIndex(block.hash));
    bool mutated{false};
    if (!pindex || pindex->nHeight != block.height || cblock.GetHash() != block.hash ||
        BlockMerkleRoot(cblock, &mutated) != cblock.hashMerkleRoot || mutated) {
        return error("%s: DigiDollar supply index current block failed integrity verification at height %d", __func__, block.height);
    }
    if (block.height == 0 || block.height < params.DigiDollarHeight) {
        return m_db->Write(DBHeightKey(block.height), std::make_pair(block.hash, DigiDollarDBVal{0, 0, 0}));
    }
    CBlockUndo undo;
    if (!m_chainstate->m_blockman.UndoReadFromDisk(undo, *pindex) || undo.vtxundo.size() + 1 != cblock.vtx.size()) {
        return error("%s: DigiDollar supply index needs retained block/undo data at height %d", __func__, block.height);
    }
    std::map<uint32_t, CBlock> source_blocks;
    DigiDollar::CanonicalTxLookup lookup = [&](const uint256& txid, uint32_t height, CTransactionRef& out) {
        if (height > static_cast<uint32_t>(pindex->nHeight)) return false;
        const CBlock* source = &cblock;
        if (height != static_cast<uint32_t>(pindex->nHeight)) {
            auto it = source_blocks.find(height);
            if (it == source_blocks.end()) {
                CBlock disk;
                const auto* ancestor = pindex->GetAncestor(height);
                bool mutated{false};
                if (!ancestor || !m_chainstate->m_blockman.ReadBlockFromDisk(disk, *ancestor) ||
                    disk.GetHash() != ancestor->GetBlockHash() || BlockMerkleRoot(disk, &mutated) != disk.hashMerkleRoot || mutated) return false;
                // Bound retained source blocks independently of input count.
                if (source_blocks.size() >= 8) source_blocks.erase(source_blocks.begin());
                it = source_blocks.emplace(height, std::move(disk)).first;
            }
            source = &it->second;
        }
        for (const auto& tx : source->vtx) {
            if (tx->GetHash() == txid) { out = tx; return true; }
        }
        return false;
    };
    CAmount supply = m_total_dd_supply;
    bool supply_known = m_supply_known;
    bool supply_verified = m_supply_verified;
    CAmount collateral = m_total_collateral;
    uint64_t vaults = m_vault_count;
    {
        LOCK(cs_main);
        if (!supply_verified && DigiDollar::IsThawDayActive(params, block.height)) {
            CAmount verified{0};
            bool known{false};
            std::string reason;
            if (!pindex->pprev || !SupplyVerification{*m_chainstate}.At(*pindex->pprev, verified, known, reason))
                return error("%s: %s", __func__, reason);
            if (known && (!supply_known || supply != verified)) {
                LogPrintf("DigiDollar circulating supply repaired before block %s: %d -> %d cents\n",
                          block.hash.ToString(), supply, verified);
            }
            if (known) supply = verified;
            else LogPrintf("DigiDollar circulating supply remains unavailable from retained token metadata before block %s\n", block.hash.ToString());
            supply_known = known;
            supply_verified = true;
        }
    }
    for (size_t i = 1; i < cblock.vtx.size(); ++i) {
        const auto& tx = *cblock.vtx[i];
        const auto& inputs = undo.vtxundo[i - 1].vprevout;
        CAmount delta{0};
        std::string reason;
        const auto result = DigiDollar::GetCirculatingSupplyChange(tx, inputs, block.height, params, lookup, delta, reason);
        if (result == DigiDollar::SupplyChangeResult::FAILURE) {
            return error("%s: %s", __func__, reason);
        }
        if (result == DigiDollar::SupplyChangeResult::UNKNOWN_METADATA) {
            if (supply_known) LogPrintf("DigiDollar stats index: circulating supply unavailable from retained metadata at height %d\n", block.height);
            supply_known = false;
        }
        if (supply_known) {
            const util::int128_t next_supply = static_cast<util::int128_t>(supply) + delta;
            if (next_supply < 0 || next_supply > std::numeric_limits<CAmount>::max()) return error("DigiDollar supply index total out of range");
            supply = static_cast<CAmount>(next_supply);
        }
        for (size_t j = 0; j < inputs.size(); ++j) {
            DigiDollar::CanonicalVault vault;
            const auto result = DigiDollar::LookupCanonicalVault(tx.vin[j].prevout, inputs[j], params, lookup, vault, reason);
            if (result == DigiDollar::VaultLookupResult::NOT_READY) return error("%s: %s", __func__, reason);
            if (result == DigiDollar::VaultLookupResult::VAULT) {
                if (collateral < vault.collateral || vaults == 0) return error("DigiDollar index collateral underflow");
                collateral -= vault.collateral;
                --vaults;
            }
        }
        if (DigiDollar::HasDigiDollarMarker(tx) && DigiDollar::GetDigiDollarTxType(tx) == DigiDollar::DD_TX_MINT) {
            CAmount principal{0}, value{0};
            if (!DigiDollar::ExtractMintAccountingAmounts(tx, principal, value, false) || value > MAX_MONEY - collateral ||
                vaults == std::numeric_limits<uint64_t>::max()) return error("DigiDollar index mint accounting out of range");
            collateral += value;
            ++vaults;
        }
    }
    if (!m_db->Write(DBHeightKey(block.height), std::make_pair(block.hash, DigiDollarDBVal{supply, collateral, vaults, supply_known}))) return false;
    m_total_dd_supply = supply;
    m_supply_known = supply_known;
    m_supply_verified = supply_verified;
    m_total_collateral = collateral;
    m_vault_count = vaults;
    return true;
}

bool DigiDollarStatsIndex::CustomRewind(const interfaces::BlockKey& current_tip, const interfaces::BlockKey& new_tip)
{
    CDBBatch batch(*m_db);
    std::unique_ptr<CDBIterator> db_it(m_db->NewIterator());

    // During a reorg, copy hash digests for disconnected blocks
    // from height index to hash index
    DBHeightKey key{std::max(new_tip.height, m_chainstate->m_chainman.GetConsensus().DigiDollarHeight)};
    db_it->Seek(key);

    for (int height = key.height; height <= current_tip.height; ++height) {
        if (!db_it->GetKey(key) || key.height != height) {
            return error("%s: unexpected key in %s: expected (%c, %d)",
                         __func__, GetName(), DB_BLOCK_HEIGHT, height);
        }

        std::pair<uint256, DigiDollarDBVal> value;
        if (!db_it->GetValue(value)) {
            return error("%s: unable to read value in %s at key (%c, %d)",
                         __func__, GetName(), DB_BLOCK_HEIGHT, height);
        }

        batch.Write(DBHashKey(value.first), std::move(value.second));
        db_it->Next();
    }

    // Load state from new_tip
    std::pair<uint256, DigiDollarDBVal> new_tip_state;
    if (new_tip.height < m_chainstate->m_chainman.GetConsensus().DigiDollarHeight) {
        new_tip_state = {new_tip.hash, DigiDollarDBVal{0, 0, 0}};
    } else if (!m_db->Read(DBHeightKey(new_tip.height), new_tip_state)) {
        return error("%s: Cannot read state at new tip height %d", __func__, new_tip.height);
    }

    if (new_tip_state.first != new_tip.hash) {
        // Try hash-based lookup
        if (!m_db->Read(DBHashKey(new_tip.hash), new_tip_state.second)) {
            return error("%s: Cannot read state at new tip %s", __func__, new_tip.hash.ToString());
        }
    }

    if (m_supply_verified) {
        LOCK(cs_main);
        const auto* from = m_chainstate->m_blockman.LookupBlockIndex(current_tip.hash);
        const auto* to = m_chainstate->m_blockman.LookupBlockIndex(new_tip.hash);
        CAmount verified = m_total_dd_supply;
        bool known = m_supply_known;
        std::string reason;
        if (!from || !to) return error("DigiDollar stats index rewind: block ancestry is unavailable");
        SupplyVerification verification{*m_chainstate};
        // An unavailable fork may have disappeared from the UTXO set. Rescan
        // then, so readable replacement history can make supply known again.
        if (!(known ? verification.Move(*from, *to, verified, known, reason) :
                      verification.At(*to, verified, known, reason)))
            return error("DigiDollar stats index rewind: %s", reason);
        // Do not reintroduce an older incorrect row after repairing the saved tip.
        if (known) new_tip_state.second.total_dd_supply = verified;
        new_tip_state.second.supply_known = known;
        if (new_tip_state.first == new_tip.hash) batch.Write(DBHeightKey(new_tip.height), new_tip_state);
        else batch.Write(DBHashKey(new_tip.hash), new_tip_state.second);
    }
    if (!m_db->WriteBatch(batch)) return false;

    // Restore running state from new_tip
    m_total_dd_supply = new_tip_state.second.total_dd_supply;
    m_supply_known = new_tip_state.second.supply_known;
    m_total_collateral = new_tip_state.second.total_collateral;
    m_vault_count = new_tip_state.second.vault_count;

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollarStatsIndex: Rewound to height %d - DD Supply: %s, Collateral: %d, Vaults: %d\n",
             new_tip.height, m_supply_known ? std::to_string(m_total_dd_supply) : "unavailable", m_total_collateral, m_vault_count);

    return true;
}

bool DigiDollarStatsIndex::CustomCommit(CDBBatch& batch)
{
    // No additional state to commit beyond what's in CustomAppend
    return true;
}

static bool LookUpOne(const CDBWrapper& db, const interfaces::BlockKey& block, int dd_activation, DigiDollarDBVal& result)
{
    if (block.height < dd_activation) {
        result = {0, 0, 0};
        return true;
    }
    // First check height index
    std::pair<uint256, DigiDollarDBVal> read_out;
    if (!db.Read(DBHeightKey(block.height), read_out)) {
        return false;
    }

    if (read_out.first == block.hash) {
        result = std::move(read_out.second);
        return true;
    }

    // Fall back to hash index for reorged blocks
    return db.Read(DBHashKey(block.hash), result);
}

std::optional<DigiDollarStats> DigiDollarStatsIndex::LookUpStats(const CBlockIndex& block_index) const
{
    if (!m_chainstate || block_index.nHeight > GetSummary().best_block_height) return std::nullopt;
    DigiDollarDBVal entry;
    if (!LookUpOne(*m_db, {block_index.GetBlockHash(), block_index.nHeight}, m_chainstate->m_chainman.GetConsensus().DigiDollarHeight, entry) ||
        !entry.supply_known) {
        return std::nullopt;
    }

    DigiDollarStats stats;
    stats.total_dd_supply = entry.total_dd_supply;
    stats.total_collateral = entry.total_collateral;
    stats.vault_count = entry.vault_count;
    stats.height = block_index.nHeight;
    stats.block_hash = block_index.GetBlockHash();
    {
        LOCK(cs_main);
        if (m_chainstate) {
            auto canonical = m_chainstate->CoinsTip().GetDigiDollarState();
            const auto& params = m_chainstate->m_chainman.GetConsensus();
            if (canonical && canonical->Matches(params.hashGenesisBlock, stats.block_hash,
                                                  params.nDDThawDayHeight, params.DigiDollarHeight))
                stats.canonical_health = std::move(canonical);
        }
    }

    return stats;
}
