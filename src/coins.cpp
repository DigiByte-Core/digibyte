// Copyright (c) 2012-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <coins.h>

#include <consensus/consensus.h>
#include <logging.h>
#include <random.h>
#include <streams.h>
#include <util/trace.h>
#include <version.h>

#include <algorithm>
#include <utility>
#include <vector>

bool CCoinsView::GetCoin(const COutPoint &outpoint, Coin &coin) const { return false; }
uint256 CCoinsView::GetBestBlock() const { return uint256(); }
std::optional<DigiDollar::ChainstateHealth> CCoinsView::GetDigiDollarState() const { return std::nullopt; }
std::vector<uint256> CCoinsView::GetHeadBlocks() const { return std::vector<uint256>(); }
bool CCoinsView::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, bool erase, const std::optional<DigiDollar::ChainstateHealth>& dd_state) { return false; }
std::unique_ptr<CCoinsViewCursor> CCoinsView::Cursor() const { return nullptr; }

bool CCoinsView::HaveCoin(const COutPoint &outpoint) const
{
    Coin coin;
    return GetCoin(outpoint, coin);
}

CCoinsViewBacked::CCoinsViewBacked(CCoinsView *viewIn) : base(viewIn) { }
bool CCoinsViewBacked::GetCoin(const COutPoint &outpoint, Coin &coin) const { return base->GetCoin(outpoint, coin); }
bool CCoinsViewBacked::HaveCoin(const COutPoint &outpoint) const { return base->HaveCoin(outpoint); }
uint256 CCoinsViewBacked::GetBestBlock() const { return base->GetBestBlock(); }
std::optional<DigiDollar::ChainstateHealth> CCoinsViewBacked::GetDigiDollarState() const { return base->GetDigiDollarState(); }
std::vector<uint256> CCoinsViewBacked::GetHeadBlocks() const { return base->GetHeadBlocks(); }
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) { base = &viewIn; }
bool CCoinsViewBacked::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, bool erase, const std::optional<DigiDollar::ChainstateHealth>& dd_state) { return base->BatchWrite(mapCoins, hashBlock, erase, dd_state); }
std::unique_ptr<CCoinsViewCursor> CCoinsViewBacked::Cursor() const { return base->Cursor(); }
size_t CCoinsViewBacked::EstimateSize() const { return base->EstimateSize(); }

CCoinsViewCache::CCoinsViewCache(CCoinsView* baseIn, bool deterministic) :
    CCoinsViewBacked(baseIn), m_deterministic(deterministic),
    cacheCoins(0, SaltedOutpointHasher(/*deterministic=*/deterministic), CCoinsMap::key_equal{}, &m_cache_coins_memory_resource)
{}

size_t CCoinsViewCache::DynamicMemoryUsage() const {
    return memusage::DynamicUsage(cacheCoins) + cachedCoinsUsage;
}

CCoinsMap::iterator CCoinsViewCache::FetchCoin(const COutPoint &outpoint) const {
    CCoinsMap::iterator it = cacheCoins.find(outpoint);
    if (it != cacheCoins.end())
        return it;
    Coin tmp;
    if (!base->GetCoin(outpoint, tmp))
        return cacheCoins.end();
    CCoinsMap::iterator ret = cacheCoins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint), std::forward_as_tuple(std::move(tmp))).first;
    if (ret->second.coin.IsSpent()) {
        // The parent only has an empty entry for this outpoint; we can consider our
        // version as fresh.
        ret->second.flags = CCoinsCacheEntry::FRESH;
    }
    cachedCoinsUsage += ret->second.coin.DynamicMemoryUsage();
    return ret;
}

bool CCoinsViewCache::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    if (it != cacheCoins.end()) {
        coin = it->second.coin;
        return !coin.IsSpent();
    }
    return false;
}

void CCoinsViewCache::AddCoin(const COutPoint &outpoint, Coin&& coin, bool possible_overwrite) {
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    CCoinsMap::iterator it;
    bool inserted;
    std::tie(it, inserted) = cacheCoins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint), std::tuple<>());
    bool fresh = false;
    if (!inserted) {
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    }
    if (!possible_overwrite) {
        if (!it->second.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // If the coin exists in this cache as a spent coin and is DIRTY, then
        // its spentness hasn't been flushed to the parent cache. We're
        // re-adding the coin to this cache now but we can't mark it as FRESH.
        // If we mark it FRESH and then spend it before the cache is flushed
        // we would remove it from this cache and would never flush spentness
        // to the parent cache.
        //
        // Re-adding a spent coin can happen in the case of a re-org (the coin
        // is 'spent' when the block adding it is disconnected and then
        // re-added when it is also added in a newly connected block).
        //
        // If the coin doesn't exist in the current cache, or is spent but not
        // DIRTY, then it can be marked FRESH.
        fresh = !(it->second.flags & CCoinsCacheEntry::DIRTY);
    }
    it->second.coin = std::move(coin);
    it->second.flags |= CCoinsCacheEntry::DIRTY | (fresh ? CCoinsCacheEntry::FRESH : 0);
    cachedCoinsUsage += it->second.coin.DynamicMemoryUsage();
    TRACE5(utxocache, add,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
}

void CCoinsViewCache::EmplaceCoinInternalDANGER(COutPoint&& outpoint, Coin&& coin) {
    cachedCoinsUsage += coin.DynamicMemoryUsage();
    cacheCoins.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(std::move(outpoint)),
        std::forward_as_tuple(std::move(coin), CCoinsCacheEntry::DIRTY));
}

void AddCoins(CCoinsViewCache& cache, const CTransaction &tx, int nHeight, bool check_for_overwrite) {
    bool fCoinbase = tx.IsCoinBase();
    const uint256& txid = tx.GetHash();
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        bool overwrite = check_for_overwrite ? cache.HaveCoin(COutPoint(txid, i)) : fCoinbase;
        // Coinbase transactions can always be overwritten, in order to correctly
        // deal with the pre-BIP30 occurrences of duplicate coinbase transactions.
        cache.AddCoin(COutPoint(txid, i), Coin(tx.vout[i], nHeight, fCoinbase), overwrite);
    }
}

bool CCoinsViewCache::SpendCoin(const COutPoint &outpoint, Coin* moveout) {
    CCoinsMap::iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) return false;
    cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    TRACE5(utxocache, spent,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
    if (moveout) {
        *moveout = std::move(it->second.coin);
    }
    if (it->second.flags & CCoinsCacheEntry::FRESH) {
        cacheCoins.erase(it);
    } else {
        it->second.flags |= CCoinsCacheEntry::DIRTY;
        it->second.coin.Clear();
    }
    return true;
}

static const Coin coinEmpty;

const Coin& CCoinsViewCache::AccessCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) {
        return coinEmpty;
    } else {
        return it->second.coin;
    }
}

bool CCoinsViewCache::HaveCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

bool CCoinsViewCache::HaveCoinInCache(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = cacheCoins.find(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

uint256 CCoinsViewCache::GetBestBlock() const {
    if (hashBlock.IsNull())
        hashBlock = base->GetBestBlock();
    return hashBlock;
}

void CCoinsViewCache::SetBestBlock(const uint256 &hashBlockIn) {
    hashBlock = hashBlockIn;
}

std::optional<DigiDollar::ChainstateHealth> CCoinsViewCache::GetDigiDollarState() const
{
    if (!m_dd_state_loaded) {
        m_dd_state = base->GetDigiDollarState();
        m_dd_state_loaded = true;
    }
    if (m_dd_state && (!m_dd_state->IsValid() || m_dd_state->best_block != GetBestBlock())) return std::nullopt;
    return m_dd_state;
}

void CCoinsViewCache::SetDigiDollarState(std::optional<DigiDollar::ChainstateHealth> state)
{
    m_dd_state = std::move(state);
    m_dd_state_loaded = true;
}

namespace {
// Match the coin database's byte order: transaction hash, then the output
// number encoded as VARINT. Numeric output order differs once the encoding
// grows, so using it here can miss a cache entry that spends a database coin.
bool CoinDatabaseKeyLess(const COutPoint& a, const COutPoint& b)
{
    const int hash_order = a.hash.Compare(b.hash);
    if (hash_order != 0) return hash_order < 0;
    if (a.n == b.n) return false;

    DataStream a_index, b_index;
    a_index << VARINT(a.n);
    b_index << VARINT(b.n);
    return std::lexicographical_compare(a_index.begin(), a_index.end(), b_index.begin(), b_index.end());
}

/** Walks every unspent coin of a coins cache exactly once.
 *
 * A cache holds only the changes made since the last write to the coins
 * database, so the coins it represents are the database contents with those
 * changed entries laid over the top. This walk merges the two: it steps through
 * the backing cursor and through a sorted copy of the changed entries side by
 * side, always taking whichever outpoint comes first. Where both hold the same
 * outpoint the cache entry wins, because it is the newer one. Entries the cache
 * records as spent hide the database copy and are never handed out.
 *
 * Merging keeps the outpoint order of the backing cursor, which matters because
 * how much of a cache has been written to disk is a local matter: two nodes at
 * the same block can hold quite different caches. Walking the cache entries
 * separately, in whatever order the hash table happens to hold them, would give
 * those two nodes different orders. Merging gives them the same one.
 *
 * The backing cursor has to use the database's serialized-key order. This
 * walk uses that order too, so a cache laid over another cache works the same
 * way. Numeric output order is not a substitute for serialized-key order.
 *
 * The changed entries are copied when the walk starts. Pointing at them would
 * be cheaper, but a caller may read a coin from the very view it is walking,
 * and reading a coin the cache has not held before adds an entry to it, which
 * moves the existing ones about. The DigiDollar vault scan does exactly that.
 */
class CacheCoinsCursor final : public CCoinsViewCursor {
    using Entry = std::pair<COutPoint, Coin>;
    std::unique_ptr<CCoinsViewCursor> m_base;
    //! The cache's changed entries, sorted in database key order. Spent ones are kept
    //! because they are what hides a coin the database still has.
    std::vector<Entry> m_changed;
    //! Index into m_changed of the entry being looked at, or the size when
    //! there are none left.
    size_t m_pos{0};
    //! True when the coin to hand out right now comes from m_changed rather
    //! than from the backing cursor.
    bool m_from_cache{false};

    //! Move to the next coin to hand out, or leave nothing to hand out.
    void Seek()
    {
        while (true) {
            COutPoint base_key;
            const bool base_has_key = m_base->Valid() && m_base->GetKey(base_key);
            if (m_base->Valid() && !base_has_key) {
                // The backing cursor says it has a coin but will not give up
                // its outpoint. Stop here and let the caller find that it
                // cannot read this coin, rather than quietly skipping the rest
                // of the database and reporting a short answer.
                m_from_cache = false;
                return;
            }
            if (m_pos == m_changed.size()) {
                // Only the backing cursor is left, valid or finished.
                m_from_cache = false;
                return;
            }
            if (base_has_key && CoinDatabaseKeyLess(base_key, m_changed[m_pos].first)) {
                m_from_cache = false;
                return;
            }
            if (base_has_key && base_key == m_changed[m_pos].first) {
                // The cache changed this coin, so its copy replaces the one in
                // the database. Step the database past it either way.
                m_base->Next();
            }
            if (!m_changed[m_pos].second.IsSpent()) {
                m_from_cache = true;
                return;
            }
            // The cache spent this coin, so nobody gets it.
            ++m_pos;
        }
    }

public:
    CacheCoinsCursor(std::unique_ptr<CCoinsViewCursor> base, const CCoinsMap& cache, const uint256& block)
        : CCoinsViewCursor(block), m_base(std::move(base))
    {
        size_t changed = 0;
        for (const auto& [outpoint, entry] : cache) {
            if (entry.flags & CCoinsCacheEntry::DIRTY) ++changed;
        }
        m_changed.reserve(changed);
        for (const auto& [outpoint, entry] : cache) {
            if (entry.flags & CCoinsCacheEntry::DIRTY) m_changed.emplace_back(outpoint, entry.coin);
        }
        std::sort(m_changed.begin(), m_changed.end(),
                  [](const Entry& a, const Entry& b) { return CoinDatabaseKeyLess(a.first, b.first); });
        Seek();
    }
    bool GetKey(COutPoint& key) const override
    {
        if (m_from_cache) {
            key = m_changed[m_pos].first;
            return true;
        }
        return m_base->Valid() && m_base->GetKey(key);
    }
    bool GetValue(Coin& coin) const override
    {
        if (m_from_cache) {
            coin = m_changed[m_pos].second;
            return true;
        }
        return m_base->Valid() && m_base->GetValue(coin);
    }
    bool Valid() const override { return m_from_cache || m_base->Valid(); }
    void CheckStatus() const override { m_base->CheckStatus(); }
    void Next() override
    {
        if (m_from_cache) ++m_pos;
        else if (m_base->Valid()) m_base->Next();
        Seek();
    }
};
} // namespace

std::unique_ptr<CCoinsViewCursor> CCoinsViewCache::Cursor() const
{
    auto cursor = base->Cursor();
    if (!cursor) return nullptr;
    return std::make_unique<CacheCoinsCursor>(std::move(cursor), cacheCoins, GetBestBlock());
}

bool CCoinsViewCache::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlockIn, bool erase, const std::optional<DigiDollar::ChainstateHealth>& dd_state) {
    if (dd_state && (!dd_state->IsValid() || dd_state->best_block != hashBlockIn)) return false;
    for (CCoinsMap::iterator it = mapCoins.begin();
            it != mapCoins.end();
            it = erase ? mapCoins.erase(it) : std::next(it)) {
        // Ignore non-dirty entries (optimization).
        if (!(it->second.flags & CCoinsCacheEntry::DIRTY)) {
            continue;
        }
        CCoinsMap::iterator itUs = cacheCoins.find(it->first);
        if (itUs == cacheCoins.end()) {
            // The parent cache does not have an entry, while the child cache does.
            // We can ignore it if it's both spent and FRESH in the child
            if (!(it->second.flags & CCoinsCacheEntry::FRESH && it->second.coin.IsSpent())) {
                // Create the coin in the parent cache, move the data up
                // and mark it as dirty.
                CCoinsCacheEntry& entry = cacheCoins[it->first];
                if (erase) {
                    // The `move` call here is purely an optimization; we rely on the
                    // `mapCoins.erase` call in the `for` expression to actually remove
                    // the entry from the child map.
                    entry.coin = std::move(it->second.coin);
                } else {
                    entry.coin = it->second.coin;
                }
                cachedCoinsUsage += entry.coin.DynamicMemoryUsage();
                entry.flags = CCoinsCacheEntry::DIRTY;
                // We can mark it FRESH in the parent if it was FRESH in the child
                // Otherwise it might have just been flushed from the parent's cache
                // and already exist in the grandparent
                if (it->second.flags & CCoinsCacheEntry::FRESH) {
                    entry.flags |= CCoinsCacheEntry::FRESH;
                }
            }
        } else {
            // Found the entry in the parent cache
            if ((it->second.flags & CCoinsCacheEntry::FRESH) && !itUs->second.coin.IsSpent()) {
                // The coin was marked FRESH in the child cache, but the coin
                // exists in the parent cache. If this ever happens, it means
                // the FRESH flag was misapplied and there is a logic error in
                // the calling code.
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }

            if ((itUs->second.flags & CCoinsCacheEntry::FRESH) && it->second.coin.IsSpent()) {
                // The grandparent cache does not have an entry, and the coin
                // has been spent. We can just delete it from the parent cache.
                cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                cacheCoins.erase(itUs);
            } else {
                // A normal modification.
                cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                if (erase) {
                    // The `move` call here is purely an optimization; we rely on the
                    // `mapCoins.erase` call in the `for` expression to actually remove
                    // the entry from the child map.
                    itUs->second.coin = std::move(it->second.coin);
                } else {
                    itUs->second.coin = it->second.coin;
                }
                cachedCoinsUsage += itUs->second.coin.DynamicMemoryUsage();
                itUs->second.flags |= CCoinsCacheEntry::DIRTY;
                // NOTE: It isn't safe to mark the coin as FRESH in the parent
                // cache. If it already existed and was spent in the parent
                // cache then marking it FRESH would prevent that spentness
                // from being flushed to the grandparent.
            }
        }
    }
    hashBlock = hashBlockIn;
    SetDigiDollarState(dd_state);
    return true;
}

bool CCoinsViewCache::Flush() {
    const auto state = GetDigiDollarState();
    auto failed = [&] {
        SetDigiDollarState(std::nullopt);
        cachedCoinsUsage = 0;
        for (const auto& [_, entry] : cacheCoins) cachedCoinsUsage += entry.coin.DynamicMemoryUsage();
    };
    bool fOk;
    try {
        fOk = base->BatchWrite(cacheCoins, GetBestBlock(), /*erase=*/true, state);
    } catch (...) {
        failed();
        throw;
    }
    if (fOk) {
        if (!cacheCoins.empty()) {
            /* BatchWrite must erase all cacheCoins elements when erase=true. */
            throw std::logic_error("Not all cached coins were erased");
        }
        ReallocateCache();
        cachedCoinsUsage = 0;
    } else {
        failed();
    }
    return fOk;
}

bool CCoinsViewCache::Sync()
{
    const auto state = GetDigiDollarState();
    bool fOk;
    try {
        fOk = base->BatchWrite(cacheCoins, GetBestBlock(), /*erase=*/false, state);
    } catch (...) {
        SetDigiDollarState(std::nullopt);
        throw;
    }
    if (!fOk) {
        SetDigiDollarState(std::nullopt);
        return false;
    }
    // Instead of clearing `cacheCoins` as we would in Flush(), just clear the
    // FRESH/DIRTY flags of any coin that isn't spent.
    for (auto it = cacheCoins.begin(); it != cacheCoins.end(); ) {
        if (it->second.coin.IsSpent()) {
            cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
            it = cacheCoins.erase(it);
        } else {
            it->second.flags = 0;
            ++it;
        }
    }
    return fOk;
}

void CCoinsViewCache::Uncache(const COutPoint& hash)
{
    CCoinsMap::iterator it = cacheCoins.find(hash);
    if (it != cacheCoins.end() && it->second.flags == 0) {
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
        TRACE5(utxocache, uncache,
               hash.hash.data(),
               (uint32_t)hash.n,
               (uint32_t)it->second.coin.nHeight,
               (int64_t)it->second.coin.out.nValue,
               (bool)it->second.coin.IsCoinBase());
        cacheCoins.erase(it);
    }
}

unsigned int CCoinsViewCache::GetCacheSize() const {
    return cacheCoins.size();
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            if (!HaveCoin(tx.vin[i].prevout)) {
                return false;
            }
        }
    }
    return true;
}

void CCoinsViewCache::ReallocateCache()
{
    // Cache should be empty when we're calling this.
    assert(cacheCoins.size() == 0);
    cacheCoins.~CCoinsMap();
    m_cache_coins_memory_resource.~CCoinsMapMemoryResource();
    ::new (&m_cache_coins_memory_resource) CCoinsMapMemoryResource{};
    ::new (&cacheCoins) CCoinsMap{0, SaltedOutpointHasher{/*deterministic=*/m_deterministic}, CCoinsMap::key_equal{}, &m_cache_coins_memory_resource};
}

void CCoinsViewCache::SanityCheck() const
{
    size_t recomputed_usage = 0;
    for (const auto& [_, entry] : cacheCoins) {
        unsigned attr = 0;
        if (entry.flags & CCoinsCacheEntry::DIRTY) attr |= 1;
        if (entry.flags & CCoinsCacheEntry::FRESH) attr |= 2;
        if (entry.coin.IsSpent()) attr |= 4;
        // Only 5 combinations are possible.
        assert(attr != 2 && attr != 4 && attr != 7);

        // Recompute cachedCoinsUsage.
        recomputed_usage += entry.coin.DynamicMemoryUsage();
    }
    assert(recomputed_usage == cachedCoinsUsage);
}

static const size_t MIN_TRANSACTION_OUTPUT_WEIGHT = WITNESS_SCALE_FACTOR * ::GetSerializeSize(CTxOut(), PROTOCOL_VERSION);
static const size_t MAX_OUTPUTS_PER_BLOCK = MAX_BLOCK_WEIGHT / MIN_TRANSACTION_OUTPUT_WEIGHT;

const Coin& AccessByTxid(const CCoinsViewCache& view, const uint256& txid)
{
    COutPoint iter(txid, 0);
    while (iter.n < MAX_OUTPUTS_PER_BLOCK) {
        const Coin& alternate = view.AccessCoin(iter);
        if (!alternate.IsSpent()) return alternate;
        ++iter.n;
    }
    return coinEmpty;
}

template <typename Func>
static bool ExecuteBackedWrapper(Func func, const std::vector<std::function<void()>>& err_callbacks)
{
    try {
        return func();
    } catch(const std::runtime_error& e) {
        for (const auto& f : err_callbacks) {
            f();
        }
        LogPrintf("Error reading from database: %s\n", e.what());
        // Starting the shutdown sequence and returning false to the caller would be
        // interpreted as 'entry not found' (as opposed to unable to read data), and
        // could lead to invalid interpretation. Just exit immediately, as we can't
        // continue anyway, and all writes should be atomic.
        std::abort();
    }
}
bool CCoinsViewErrorCatcher::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    return ExecuteBackedWrapper([&]() { return CCoinsViewBacked::GetCoin(outpoint, coin); }, m_err_callbacks);
}

bool CCoinsViewErrorCatcher::HaveCoin(const COutPoint &outpoint) const {
    return ExecuteBackedWrapper([&]() { return CCoinsViewBacked::HaveCoin(outpoint); }, m_err_callbacks);
}
