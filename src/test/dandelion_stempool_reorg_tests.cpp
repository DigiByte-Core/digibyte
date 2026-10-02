// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The Dandelion stempool must survive a reorg with its internal indexes
// intact. This is the regression test for a node crash (a null read inside
// removeForBlock on the message-handler thread during a block connect).
//
// Chainstate::MaybeUpdateMempoolForReorg runs one "is this entry still valid?"
// filter over the regular mempool and then over the stempool. When the filter
// has to recompute an entry's lock points (its highest confirmed input was in a
// block that has just been disconnected) it updates the entry in place through
// boost::multi_index::modify(). That call must go to the container the
// iterator belongs to. Before the fix it always went to the regular mempool,
// even for stempool iterators, which silently moved the entry's index nodes
// from the stempool into the mempool and cross-wired the two red-black trees.
// The next tree rebalance in either pool (typically removeForBlock during the
// next block connect) then walked a broken tree and crashed the node.
//
// The test builds exactly that situation: a parent confirmed in the tip block,
// stem-phase children of it in the stempool, then InvalidateBlock on the tip.

#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <kernel/mempool_removal_reason.h>
#include <key.h>
#include <node/context.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <util/check.h>
#include <util/translation.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace {

// Build and sign a one-input, one-output spend of input_tx:vout (paid to
// key's P2PK script) with the given nSequence. nSequence matters here: the
// reorg filter only recomputes an entry's lock points when they are pinned to
// a block, and CalculateSequenceLocks pins them only for inputs with BIP68
// enabled (nVersion >= 2 and the SEQUENCE_LOCKTIME_DISABLE_FLAG clear). The
// wallet's own sends and DigiDollar transactions use 0xfffffffd/0xfffffffe
// (flag set); MiniWallet, CSV users and third-party builders use values such
// as 0. Stem transactions arrive from the whole network, so the stempool sees
// both kinds.
CMutableTransaction CreateSignedSpend(const CTransactionRef& input_tx, uint32_t vout, int input_height,
                                      const CKey& key, const CScript& destination, CAmount amount, uint32_t sequence)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint(input_tx->GetHash(), vout), CScript(), sequence);
    mtx.vout.emplace_back(amount, destination);

    FillableSigningProvider keystore;
    keystore.AddKey(key);
    CCoinsView dummy;
    CCoinsViewCache coins(&dummy);
    AddCoins(coins, *input_tx, input_height);
    Coin utxo;
    BOOST_REQUIRE(coins.GetCoin(mtx.vin[0].prevout, utxo));
    std::map<COutPoint, Coin> input_coins{{mtx.vin[0].prevout, utxo}};
    std::map<int, bilingual_str> errors;
    BOOST_REQUIRE(SignTransaction(mtx, &keystore, input_coins, SIGHASH_ALL, errors));
    return mtx;
}

// Walk an index and count its entries, giving up once the count exceeds the
// bound so a cross-wired tree cannot loop forever.
template <typename Index>
size_t CountEntries(const Index& index, size_t bound)
{
    size_t count{0};
    for (auto it = index.begin(); it != index.end(); ++it) {
        if (++count > bound) break;
    }
    return count;
}

// A healthy multi_index container enumerates exactly size() entries through
// every one of its indexes. After the cross-container modify the hashed
// indexes of the two pools disagree with their size() counters.
void CheckEveryIndexEnumeratesAllEntries(const CTxMemPool& pool, const std::string& name) EXCLUSIVE_LOCKS_REQUIRED(pool.cs)
{
    AssertLockHeld(pool.cs);
    const size_t expected{pool.size()};
    const size_t bound{expected + 1};
    BOOST_CHECK_MESSAGE(CountEntries(pool.mapTx, bound) == expected, name << ": txid index does not enumerate size()=" << expected << " entries");
    BOOST_CHECK_MESSAGE(CountEntries(pool.mapTx.get<index_by_wtxid>(), bound) == expected, name << ": wtxid index does not enumerate size()=" << expected << " entries");
    BOOST_CHECK_MESSAGE(CountEntries(pool.mapTx.get<descendant_score>(), bound) == expected, name << ": descendant_score index does not enumerate size()=" << expected << " entries");
    BOOST_CHECK_MESSAGE(CountEntries(pool.mapTx.get<entry_time>(), bound) == expected, name << ": entry_time index does not enumerate size()=" << expected << " entries");
    BOOST_CHECK_MESSAGE(CountEntries(pool.mapTx.get<ancestor_score>(), bound) == expected, name << ": ancestor_score index does not enumerate size()=" << expected << " entries");
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(dandelion_stempool_reorg_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(stempool_entries_survive_reorg_with_intact_indexes)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CTxMemPool& mempool = *Assert(m_node.mempool);
    CTxMemPool& stempool = *Assert(m_node.stempool);
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Both pools start empty and the test fixture enables consistency checks
    // (MemPoolOptionsForTest sets check_ratio = 1 for both pools).
    BOOST_REQUIRE_EQUAL(mempool.size(), 0U);
    BOOST_REQUIRE_EQUAL(stempool.size(), 0U);

    // Height of the block a stempool entry's cached lock points are pinned to
    // (LockPoints::maxInputBlock), or -1 if they are not pinned to any block.
    const auto stem_lockpoint_height = [&](const CTransactionRef& tx) -> int {
        LOCK(stempool.cs);
        const std::optional<CTxMemPool::txiter> it{stempool.GetIter(tx->GetHash())};
        BOOST_REQUIRE_MESSAGE(it.has_value(), tx->GetHash().ToString() << " is not in the stempool");
        const CBlockIndex* max_input_block{(*it)->GetLockPoints().maxInputBlock};
        return max_input_block ? max_input_block->nHeight : -1;
    };

    // Runs the structural check on both pools followed by each pool's own
    // consistency check. The stempool's check sees mempool outputs too because
    // stem transactions may spend them. Lock order: cs_main, mempool.cs,
    // stempool.cs (the order ActivateBestChain uses).
    const auto check_both_pools = [&](const std::string& stage) {
        BOOST_TEST_MESSAGE("checking both pools: " << stage);
        LOCK(cs_main);
        LOCK2(mempool.cs, stempool.cs);
        CheckEveryIndexEnumeratesAllEntries(mempool, stage + " mempool");
        CheckEveryIndexEnumeratesAllEntries(stempool, stage + " stempool");
        const int64_t spend_height{chainstate.m_chain.Height() + 1};
        mempool.check(chainstate.CoinsTip(), spend_height);
        CCoinsViewMemPool view_with_mempool(&chainstate.CoinsTip(), mempool);
        CCoinsViewCache coins_with_mempool(&view_with_mempool);
        stempool.check(coins_with_mempool, spend_height);
    };

    // DigiByte regtest applies the 100-block coinbase maturity, so the first
    // two coinbases (heights 1 and 2) are spendable from height 102 on. Mine
    // two more blocks (tip 102) so both parents are valid in block 103.
    mineBlocks(2);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Height()), 102);

    // Parents A1 and A2 spend mature coinbases and get confirmed in block 103.
    const CMutableTransaction tx_a1 = CreateSignedSpend(m_coinbase_txns[0], 0, /*input_height=*/1, coinbaseKey, coinbase_script, 50 * COIN, CTxIn::SEQUENCE_FINAL);
    const CMutableTransaction tx_a2 = CreateSignedSpend(m_coinbase_txns[1], 0, /*input_height=*/2, coinbaseKey, coinbase_script, 50 * COIN, CTxIn::SEQUENCE_FINAL);
    const CTransactionRef ptx_a1 = MakeTransactionRef(tx_a1);
    const CTransactionRef ptx_a2 = MakeTransactionRef(tx_a2);
    const CBlock parents_block = CreateAndProcessBlock({tx_a1, tx_a2}, coinbase_script);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Height()), 103);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), parents_block.GetHash());
    BOOST_REQUIRE_EQUAL(mempool.size(), 0U);

    // Stem-phase children: B1 spends A1, B2 spends A2, C spends B1. All three
    // are accepted into the stempool only, exactly like inbound dandeliontx.
    // B1 and B2 use BIP68-enabled inputs (nSequence 0, a zero relative lock),
    // so their lock points are pinned to block 103. C spends an unconfirmed
    // stem output with a final sequence, like a wallet transaction would; a
    // BIP68-enabled child of an unconfirmed parent is legitimately dropped by
    // the reorg filter once the tip moves back (its cached lock height is no
    // longer reachable), which is upstream mempool policy and not the subject
    // of this test.
    const CMutableTransaction tx_b1 = CreateSignedSpend(ptx_a1, 0, /*input_height=*/103, coinbaseKey, coinbase_script, 49 * COIN, /*sequence=*/0);
    const CMutableTransaction tx_b2 = CreateSignedSpend(ptx_a2, 0, /*input_height=*/103, coinbaseKey, coinbase_script, 49 * COIN, /*sequence=*/0);
    const CTransactionRef ptx_b1 = MakeTransactionRef(tx_b1);
    const CTransactionRef ptx_b2 = MakeTransactionRef(tx_b2);
    const CMutableTransaction tx_c = CreateSignedSpend(ptx_b1, 0, /*input_height=*/104, coinbaseKey, coinbase_script, 48 * COIN, CTxIn::SEQUENCE_FINAL);
    const CTransactionRef ptx_c = MakeTransactionRef(tx_c);
    for (const CTransactionRef& stem_tx : {ptx_b1, ptx_b2, ptx_c}) {
        LOCK(cs_main);
        const MempoolAcceptResult result = AcceptToMemoryPoolForStempool(chainstate, stempool, mempool, stem_tx, /*bypass_limits=*/false);
        BOOST_REQUIRE_MESSAGE(result.m_result_type == MempoolAcceptResult::ResultType::VALID,
                              "stempool rejected " << stem_tx->GetHash().ToString() << ": " << result.m_state.ToString());
    }
    BOOST_CHECK_EQUAL(stempool.size(), 3U);
    BOOST_CHECK_EQUAL(mempool.size(), 0U);
    // B1 and B2 are pinned to block 103 (their input is confirmed there); C
    // spends an unconfirmed stem output, so it is pinned to genesis.
    BOOST_CHECK_EQUAL(stem_lockpoint_height(ptx_b1), 103);
    BOOST_CHECK_EQUAL(stem_lockpoint_height(ptx_b2), 103);
    BOOST_CHECK_EQUAL(stem_lockpoint_height(ptx_c), 0);
    check_both_pools("before reorg");

    // Disconnect block 103. A1 and A2 go back to the regular mempool, and the
    // reorg filter has to recompute B1's and B2's lock points inside the
    // stempool. Before the fix this is where the entries were re-indexed into
    // the wrong pool.
    CBlockIndex* parents_index = WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(parents_block.GetHash()));
    BOOST_REQUIRE(parents_index != nullptr);
    {
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.InvalidateBlock(state, parents_index));
        BOOST_REQUIRE_MESSAGE(state.IsValid(), state.ToString());
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Height()), 102);

    // Each transaction must be exactly where it belongs: the parents in the
    // mempool, the stem children in the stempool, and neither in the other.
    BOOST_CHECK(mempool.exists(GenTxid::Txid(ptx_a1->GetHash())));
    BOOST_CHECK(mempool.exists(GenTxid::Txid(ptx_a2->GetHash())));
    BOOST_CHECK(!mempool.exists(GenTxid::Txid(ptx_b1->GetHash())));
    BOOST_CHECK(!mempool.exists(GenTxid::Txid(ptx_b2->GetHash())));
    BOOST_CHECK(!mempool.exists(GenTxid::Txid(ptx_c->GetHash())));
    BOOST_CHECK(stempool.exists(GenTxid::Txid(ptx_b1->GetHash())));
    BOOST_CHECK(stempool.exists(GenTxid::Txid(ptx_b2->GetHash())));
    BOOST_CHECK(stempool.exists(GenTxid::Txid(ptx_c->GetHash())));
    BOOST_CHECK(!stempool.exists(GenTxid::Txid(ptx_a1->GetHash())));
    BOOST_CHECK(!stempool.exists(GenTxid::Txid(ptx_a2->GetHash())));
    BOOST_CHECK_EQUAL(mempool.size(), 2U);
    BOOST_CHECK_EQUAL(stempool.size(), 3U);
    // The reorg filter had to recompute B1's and B2's lock points: their
    // inputs are now unconfirmed mempool outputs, so they are pinned to
    // genesis like C. (This recomputation is the write that used to go to
    // the wrong container.)
    BOOST_CHECK_EQUAL(stem_lockpoint_height(ptx_b1), 0);
    BOOST_CHECK_EQUAL(stem_lockpoint_height(ptx_b2), 0);
    BOOST_CHECK_EQUAL(stem_lockpoint_height(ptx_c), 0);
    check_both_pools("after disconnect");

    // Reconnect block 103: removeForBlock runs on both pools (this is where
    // the crash used to happen). The parents leave the mempool; the stem
    // children stay put.
    {
        LOCK(cs_main);
        chainstate.ResetBlockFailureFlags(parents_index);
    }
    {
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.ActivateBestChain(state));
        BOOST_REQUIRE_MESSAGE(state.IsValid(), state.ToString());
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), parents_block.GetHash());
    BOOST_CHECK_EQUAL(mempool.size(), 0U);
    BOOST_CHECK_EQUAL(stempool.size(), 3U);
    BOOST_CHECK(stempool.exists(GenTxid::Txid(ptx_b1->GetHash())));
    BOOST_CHECK(stempool.exists(GenTxid::Txid(ptx_b2->GetHash())));
    BOOST_CHECK(stempool.exists(GenTxid::Txid(ptx_c->GetHash())));
    check_both_pools("after reconnect");

    // Embargo expiry removes B1 the way CheckDandelionEmbargoes does; its
    // stem child C goes with it.
    WITH_LOCK(stempool.cs, stempool.removeRecursive(*ptx_b1, MemPoolRemovalReason::EXPIRY));
    BOOST_CHECK_EQUAL(stempool.size(), 1U);
    BOOST_CHECK(!stempool.exists(GenTxid::Txid(ptx_b1->GetHash())));
    BOOST_CHECK(!stempool.exists(GenTxid::Txid(ptx_c->GetHash())));
    BOOST_CHECK(stempool.exists(GenTxid::Txid(ptx_b2->GetHash())));
    check_both_pools("after expiry");

    // A further block confirms B2: removeForBlock on the stempool again.
    CreateAndProcessBlock({tx_b2}, coinbase_script);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Height()), 104);
    BOOST_CHECK_EQUAL(stempool.size(), 0U);
    BOOST_CHECK_EQUAL(mempool.size(), 0U);
    check_both_pools("after confirming B2");
}

BOOST_AUTO_TEST_SUITE_END()
