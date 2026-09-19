// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/tx_check.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <validation.h>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
const TestingSetup* g_setup;
const Coin EMPTY_COIN{};

bool operator==(const Coin& a, const Coin& b)
{
    if (a.IsSpent() && b.IsSpent()) return true;
    return a.fCoinBase == b.fCoinBase && a.nHeight == b.nHeight && a.out == b.out;
}

//! A distinct hash value made from a number, so the test data is easy to follow.
//! Write the number into the first four hash bytes.
uint256 HashFromNumber(uint32_t n)
{
    uint256 out;
    unsigned char* bytes = out.begin();
    bytes[0] = static_cast<unsigned char>(n);
    bytes[1] = static_cast<unsigned char>(n >> 8);
    bytes[2] = static_cast<unsigned char>(n >> 16);
    bytes[3] = static_cast<unsigned char>(n >> 24);
    return out;
}

} // namespace

void initialize_coins_view()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>();
    g_setup = testing_setup.get();
}

FUZZ_TARGET(coins_view, .init = initialize_coins_view)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    CCoinsView backend_coins_view;
    CCoinsViewCache coins_view_cache{&backend_coins_view, /*deterministic=*/true};
    COutPoint random_out_point;
    Coin random_coin;
    CMutableTransaction random_mutable_transaction;
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 10000) {
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                if (random_coin.IsSpent()) {
                    return;
                }
                Coin coin = random_coin;
                bool expected_code_path = false;
                const bool possible_overwrite = fuzzed_data_provider.ConsumeBool();
                try {
                    coins_view_cache.AddCoin(random_out_point, std::move(coin), possible_overwrite);
                    expected_code_path = true;
                } catch (const std::logic_error& e) {
                    if (e.what() == std::string{"Attempted to overwrite an unspent coin (when possible_overwrite is false)"}) {
                        assert(!possible_overwrite);
                        expected_code_path = true;
                    }
                }
                assert(expected_code_path);
            },
            [&] {
                (void)coins_view_cache.Flush();
            },
            [&] {
                (void)coins_view_cache.Sync();
            },
            [&] {
                coins_view_cache.SetBestBlock(ConsumeUInt256(fuzzed_data_provider));
            },
            [&] {
                Coin move_to;
                (void)coins_view_cache.SpendCoin(random_out_point, fuzzed_data_provider.ConsumeBool() ? &move_to : nullptr);
            },
            [&] {
                coins_view_cache.Uncache(random_out_point);
            },
            [&] {
                if (fuzzed_data_provider.ConsumeBool()) {
                    backend_coins_view = CCoinsView{};
                }
                coins_view_cache.SetBackend(backend_coins_view);
            },
            [&] {
                const std::optional<COutPoint> opt_out_point = ConsumeDeserializable<COutPoint>(fuzzed_data_provider);
                if (!opt_out_point) {
                    return;
                }
                random_out_point = *opt_out_point;
            },
            [&] {
                const std::optional<Coin> opt_coin = ConsumeDeserializable<Coin>(fuzzed_data_provider);
                if (!opt_coin) {
                    return;
                }
                random_coin = *opt_coin;
            },
            [&] {
                const std::optional<CMutableTransaction> opt_mutable_transaction = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider);
                if (!opt_mutable_transaction) {
                    return;
                }
                random_mutable_transaction = *opt_mutable_transaction;
            },
            [&] {
                CCoinsMapMemoryResource resource;
                CCoinsMap coins_map{0, SaltedOutpointHasher{/*deterministic=*/true}, CCoinsMap::key_equal{}, &resource};
                LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 10000) {
                    CCoinsCacheEntry coins_cache_entry;
                    coins_cache_entry.flags = fuzzed_data_provider.ConsumeIntegral<unsigned char>();
                    if (fuzzed_data_provider.ConsumeBool()) {
                        coins_cache_entry.coin = random_coin;
                    } else {
                        const std::optional<Coin> opt_coin = ConsumeDeserializable<Coin>(fuzzed_data_provider);
                        if (!opt_coin) {
                            return;
                        }
                        coins_cache_entry.coin = *opt_coin;
                    }
                    coins_map.emplace(random_out_point, std::move(coins_cache_entry));
                }
                bool expected_code_path = false;
                try {
                    coins_view_cache.BatchWrite(coins_map, fuzzed_data_provider.ConsumeBool() ? ConsumeUInt256(fuzzed_data_provider) : coins_view_cache.GetBestBlock());
                    expected_code_path = true;
                } catch (const std::logic_error& e) {
                    if (e.what() == std::string{"FRESH flag misapplied to coin that exists in parent cache"}) {
                        expected_code_path = true;
                    }
                }
                assert(expected_code_path);
            });
    }

    {
        const Coin& coin_using_access_coin = coins_view_cache.AccessCoin(random_out_point);
        const bool exists_using_access_coin = !(coin_using_access_coin == EMPTY_COIN);
        const bool exists_using_have_coin = coins_view_cache.HaveCoin(random_out_point);
        const bool exists_using_have_coin_in_cache = coins_view_cache.HaveCoinInCache(random_out_point);
        Coin coin_using_get_coin;
        const bool exists_using_get_coin = coins_view_cache.GetCoin(random_out_point, coin_using_get_coin);
        if (exists_using_get_coin) {
            assert(coin_using_get_coin == coin_using_access_coin);
        }
        assert((exists_using_access_coin && exists_using_have_coin_in_cache && exists_using_have_coin && exists_using_get_coin) ||
               (!exists_using_access_coin && !exists_using_have_coin_in_cache && !exists_using_have_coin && !exists_using_get_coin));
        // If HaveCoin on the backend is true, it must also be on the cache if the coin wasn't spent.
        const bool exists_using_have_coin_in_backend = backend_coins_view.HaveCoin(random_out_point);
        if (!coin_using_access_coin.IsSpent() && exists_using_have_coin_in_backend) {
            assert(exists_using_have_coin);
        }
        Coin coin_using_backend_get_coin;
        if (backend_coins_view.GetCoin(random_out_point, coin_using_backend_get_coin)) {
            assert(exists_using_have_coin_in_backend);
            // Note we can't assert that `coin_using_get_coin == coin_using_backend_get_coin` because the coin in
            // the cache may have been modified but not yet flushed.
        } else {
            assert(!exists_using_have_coin_in_backend);
        }
    }

    {
        // This cache sits on a backing view that cannot be walked at all, so it
        // has nothing to walk either and says so. Callers must check for that.
        // A cache over a real coins database is walked further down.
        assert(coins_view_cache.Cursor() == nullptr);
        (void)coins_view_cache.DynamicMemoryUsage();
        (void)coins_view_cache.EstimateSize();
        (void)coins_view_cache.GetBestBlock();
        (void)coins_view_cache.GetCacheSize();
        (void)coins_view_cache.GetHeadBlocks();
        (void)coins_view_cache.HaveInputs(CTransaction{random_mutable_transaction});
    }

    {
        std::unique_ptr<CCoinsViewCursor> coins_view_cursor = backend_coins_view.Cursor();
        assert(!coins_view_cursor);
        (void)backend_coins_view.EstimateSize();
        (void)backend_coins_view.GetBestBlock();
        (void)backend_coins_view.GetHeadBlocks();
    }

    {
        // Walking a cache has to hand out every unspent coin exactly once and
        // nothing else. A cache holds only the changes made since the last write
        // to the database, so the walk has to combine the database contents with
        // those changes. Build both, make and spend coins, then compare the walk
        // against a plain map of what should be there.
        CCoinsViewDB database{{.path = "coins-fuzz", .cache_bytes = 1 << 20, .memory_only = true}, {}};
        CCoinsViewCache db_backed_cache{&database, /*deterministic=*/true};
        std::map<COutPoint, Coin> model;
        std::vector<COutPoint> known;
        uint32_t block_counter = 0;
        db_backed_cache.SetBestBlock(HashFromNumber(++block_counter));

        auto make_coin = [&](uint32_t tag) {
            std::vector<unsigned char> key_hash(20, 0);
            key_hash[0] = static_cast<unsigned char>(tag);
            CScript script;
            script << OP_DUP << OP_HASH160 << key_hash << OP_EQUALVERIFY << OP_CHECKSIG;
            return Coin{CTxOut{CAmount{1 + tag}, script}, int(1 + tag % 100), false};
        };
        auto add = [&](const COutPoint& outpoint, uint32_t tag) {
            Coin coin = make_coin(tag);
            model[outpoint] = coin;
            db_backed_cache.AddCoin(outpoint, std::move(coin), /*possible_overwrite=*/true);
            known.push_back(outpoint);
        };

        // Always do some work, so an input that has run out of bytes still
        // checks a real mix: some coins only in the database, some only in the
        // cache, and one the cache has hidden. The hidden one is the lowest
        // outpoint, so it is the first thing the walk has to get right.
        for (uint32_t i = 0; i < 8; ++i) add(COutPoint{HashFromNumber(i + 1), i % 3}, i);
        const uint256 shared_hash = HashFromNumber(0x20);
        for (uint32_t n : {128, 16512, 256}) add(COutPoint{shared_hash, n}, n);
        db_backed_cache.SetBestBlock(HashFromNumber(++block_counter));
        assert(db_backed_cache.Flush());
        for (uint32_t i = 8; i < 12; ++i) add(COutPoint{HashFromNumber(i + 1), i % 3}, i);
        db_backed_cache.SpendCoin(known.front());
        model.erase(known.front());
        const COutPoint spent{shared_hash, 256};
        assert(db_backed_cache.SpendCoin(spent));
        model.erase(spent);

        LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 300) {
            CallOneOf(
                fuzzed_data_provider,
                [&] {
                    const uint32_t tag = fuzzed_data_provider.ConsumeIntegral<uint8_t>();
                    const uint32_t n = fuzzed_data_provider.ConsumeBool()
                        ? fuzzed_data_provider.PickValueInArray<uint32_t>({0, 127, 128, 255, 256, 16511, 16512, 2113663, 2113664, 270549119, 270549120})
                        : fuzzed_data_provider.ConsumeIntegral<uint32_t>();
                    add(COutPoint{HashFromNumber(tag % 8 + 1), n}, tag);
                },
                [&] {
                    if (known.empty()) return;
                    const COutPoint outpoint = known[fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, known.size() - 1)];
                    db_backed_cache.SpendCoin(outpoint);
                    model.erase(outpoint);
                },
                [&] {
                    db_backed_cache.SetBestBlock(HashFromNumber(++block_counter));
                    assert(db_backed_cache.Flush());
                },
                [&] {
                    db_backed_cache.SetBestBlock(HashFromNumber(++block_counter));
                    assert(db_backed_cache.Sync());
                });
        }

        std::unique_ptr<CCoinsViewCursor> cursor = db_backed_cache.Cursor();
        assert(cursor);
        // The walk covers the cache's own block, not the database's older one.
        assert(cursor->GetBestBlock() == db_backed_cache.GetBestBlock());
        std::map<COutPoint, Coin> walked;
        std::vector<COutPoint> order;
        size_t handed_out = 0;
        for (; cursor->Valid(); cursor->Next()) {
            COutPoint key;
            Coin coin;
            // A cursor that says it has a coin must be able to name it.
            assert(cursor->GetKey(key));
            assert(cursor->GetValue(coin));
            assert(!coin.IsSpent());
            order.push_back(key);
            ++handed_out;
            walked.emplace(key, coin);
        }
        cursor->CheckStatus();
        // No coin handed out twice, and exactly the coins that should be there.
        assert(handed_out == walked.size());
        assert(walked.size() == model.size());
        for (const auto& [outpoint, coin] : model) {
            const auto it = walked.find(outpoint);
            assert(it != walked.end());
            assert(it->second == coin);
        }

        // Writing the cache must leave both the coin set and its order unchanged.
        // The real database defines that order, including large output numbers.
        cursor.reset();
        assert(db_backed_cache.Flush());
        cursor = database.Cursor();
        assert(cursor);
        for (const auto& expected : order) {
            COutPoint key;
            Coin coin;
            assert(cursor->Valid());
            assert(cursor->GetKey(key) && key == expected);
            assert(cursor->GetValue(coin) && coin == model.at(expected));
            cursor->Next();
        }
        assert(!cursor->Valid());
        cursor->CheckStatus();
    }

    if (fuzzed_data_provider.ConsumeBool()) {
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                const CTransaction transaction{random_mutable_transaction};
                bool is_spent = false;
                for (const CTxOut& tx_out : transaction.vout) {
                    if (Coin{tx_out, 0, transaction.IsCoinBase()}.IsSpent()) {
                        is_spent = true;
                    }
                }
                if (is_spent) {
                    // Avoid:
                    // coins.cpp:69: void CCoinsViewCache::AddCoin(const COutPoint &, Coin &&, bool): Assertion `!coin.IsSpent()' failed.
                    return;
                }
                bool expected_code_path = false;
                const int height{int(fuzzed_data_provider.ConsumeIntegral<uint32_t>() >> 1)};
                const bool possible_overwrite = fuzzed_data_provider.ConsumeBool();
                try {
                    AddCoins(coins_view_cache, transaction, height, possible_overwrite);
                    expected_code_path = true;
                } catch (const std::logic_error& e) {
                    if (e.what() == std::string{"Attempted to overwrite an unspent coin (when possible_overwrite is false)"}) {
                        assert(!possible_overwrite);
                        expected_code_path = true;
                    }
                }
                assert(expected_code_path);
            },
            [&] {
                (void)AreInputsStandard(CTransaction{random_mutable_transaction}, coins_view_cache);
            },
            [&] {
                TxValidationState state;
                CAmount tx_fee_out;
                const CTransaction transaction{random_mutable_transaction};
                if (ContainsSpentInput(transaction, coins_view_cache)) {
                    // Avoid:
                    // consensus/tx_verify.cpp:171: bool Consensus::CheckTxInputs(const CTransaction &, TxValidationState &, const CCoinsViewCache &, int, CAmount &): Assertion `!coin.IsSpent()' failed.
                    return;
                }
                TxValidationState dummy;
                if (!CheckTransaction(transaction, dummy)) {
                    // It is not allowed to call CheckTxInputs if CheckTransaction failed
                    return;
                }
                if (Consensus::CheckTxInputs(transaction, state, coins_view_cache, fuzzed_data_provider.ConsumeIntegralInRange<int>(0, std::numeric_limits<int>::max()), tx_fee_out)) {
                    assert(MoneyRange(tx_fee_out));
                }
            },
            [&] {
                const CTransaction transaction{random_mutable_transaction};
                if (ContainsSpentInput(transaction, coins_view_cache)) {
                    // Avoid:
                    // consensus/tx_verify.cpp:130: unsigned int GetP2SHSigOpCount(const CTransaction &, const CCoinsViewCache &): Assertion `!coin.IsSpent()' failed.
                    return;
                }
                (void)GetP2SHSigOpCount(transaction, coins_view_cache);
            },
            [&] {
                const CTransaction transaction{random_mutable_transaction};
                if (ContainsSpentInput(transaction, coins_view_cache)) {
                    // Avoid:
                    // consensus/tx_verify.cpp:130: unsigned int GetP2SHSigOpCount(const CTransaction &, const CCoinsViewCache &): Assertion `!coin.IsSpent()' failed.
                    return;
                }
                const auto flags{fuzzed_data_provider.ConsumeIntegral<uint32_t>()};
                if (!transaction.vin.empty() && (flags & SCRIPT_VERIFY_WITNESS) != 0 && (flags & SCRIPT_VERIFY_P2SH) == 0) {
                    // Avoid:
                    // script/interpreter.cpp:1705: size_t CountWitnessSigOps(const CScript &, const CScript &, const CScriptWitness *, unsigned int): Assertion `(flags & SCRIPT_VERIFY_P2SH) != 0' failed.
                    return;
                }
                (void)GetTransactionSigOpCost(transaction, coins_view_cache, flags);
            },
            [&] {
                (void)IsWitnessStandard(CTransaction{random_mutable_transaction}, coins_view_cache);
            });
    }
}
