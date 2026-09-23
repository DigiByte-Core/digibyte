// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <policy/fees.h>
#include <clientversion.h>
#include <policy/policy.h>
#include <streams.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(policyestimator_tests, ChainTestingSetup)

BOOST_AUTO_TEST_CASE(BlockPolicyEstimates)
{
    CBlockPolicyEstimator& feeEst = *Assert(m_node.fee_estimator);
    CTxMemPool& mpool = *Assert(m_node.mempool);
    LOCK2(cs_main, mpool.cs);
    TestMemPoolEntryHelper entry;
    CAmount basefee(2000);
    CAmount deltaFee(100);
    std::vector<CAmount> feeV;
    feeV.reserve(10);

    // Populate vectors of increasing fees
    for (int j = 0; j < 10; j++) {
        feeV.push_back(basefee * (j+1));
    }

    // Store the hashes of transactions that have been
    // added to the mempool by their associate fee
    // txHashes[j] is populated with transactions either of
    // fee = basefee * (j+1)
    std::vector<uint256> txHashes[10];

    // Create a transaction template
    CScript garbage;
    for (unsigned int i = 0; i < 128; i++)
        garbage.push_back('X');
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = garbage;
    tx.vout.resize(1);
    tx.vout[0].nValue=0LL;
    CFeeRate baseRate(basefee, GetVirtualTransactionSize(CTransaction(tx)));

    // Create a fake block
    std::vector<CTransactionRef> block;
    int blocknum = 0;

    // Loop through 200 blocks
    // At a decay .9952 and 4 fee transactions per block
    // This makes the tx count about 2.5 per bucket, well above the 0.1 threshold
    while (blocknum < 200) {
        for (int j = 0; j < 10; j++) { // For each fee
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k; // make transaction unique
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                txHashes[j].push_back(hash);
            }
        }
        //Create blocks where higher fee txs are included more often
        for (int h = 0; h <= blocknum%10; h++) {
            // 10/10 blocks add highest fee transactions
            // 9/10 blocks add 2nd highest and so on until ...
            // 1/10 blocks add lowest fee transactions
            while (txHashes[9-h].size()) {
                CTransactionRef ptx = mpool.get(txHashes[9-h].back());
                if (ptx)
                    block.push_back(ptx);
                txHashes[9-h].pop_back();
            }
        }
        mpool.removeForBlock(block, ++blocknum);
        block.clear();
        // Check after just a few txs that combining buckets works as expected
        if (blocknum == 3) {
            // At this point we should need to combine 3 buckets to get enough data points
            // So estimateFee(1) should fail and estimateFee(2) should return somewhere around
            // 9*baserate.  estimateFee(2) %'s are 100,100,90 = average 97%
            BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() < 9*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() > 9*baseRate.GetFeePerK() - deltaFee);
        }
    }

    std::vector<CAmount> origFeeEst;
    // Highest feerate is 10*baseRate and gets in all blocks,
    // second highest feerate is 9*baseRate and gets in 9/10 blocks = 90%,
    // third highest feerate is 8*base rate, and gets in 8/10 blocks = 80%,
    // so estimateFee(1) would return 10*baseRate but is hardcoded to return failure
    // Second highest feerate has 100% chance of being included by 2 blocks,
    // so estimateFee(2) should return 9*baseRate etc...
    for (int i = 1; i < 10;i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
        if (i > 2) { // Fee estimates should be monotonically decreasing
            BOOST_CHECK(origFeeEst[i-1] <= origFeeEst[i-2]);
        }
        int mult = 11-i;
        if (i % 2 == 0) { //At scale 2, test logic is only correct for even targets
            BOOST_CHECK(origFeeEst[i-1] < mult*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(origFeeEst[i-1] > mult*baseRate.GetFeePerK() - deltaFee);
        }
    }
    // Fill out rest of the original estimates
    for (int i = 10; i <= 48; i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
    }

    // Mine 50 more blocks with no transactions happening, estimates shouldn't change
    // We haven't decayed the moving average enough so we still have enough data points in every bucket
    while (blocknum < 250)
        mpool.removeForBlock(block, ++blocknum);

    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] + deltaFee);
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }


    // Mine 15 more blocks with lots of transactions happening and not getting mined
    // Estimates should go up
    while (blocknum < 265) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                txHashes[j].push_back(hash);
            }
        }
        mpool.removeForBlock(block, ++blocknum);
    }

    for (int i = 1; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine all those transactions
    // Estimates should still not be below original
    for (int j = 0; j < 10; j++) {
        while(txHashes[j].size()) {
            CTransactionRef ptx = mpool.get(txHashes[j].back());
            if (ptx)
                block.push_back(ptx);
            txHashes[j].pop_back();
        }
    }
    mpool.removeForBlock(block, 266);
    block.clear();
    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine 400 more blocks where everything is mined every block
    // Estimates should be below original estimates
    while (blocknum < 665) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                CTransactionRef ptx = mpool.get(hash);
                if (ptx)
                    block.push_back(ptx);

            }
        }
        mpool.removeForBlock(block, ++blocknum);
        block.clear();
    }
    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 9; i++) { // At 9, the original estimate was already at the bottom (b/c scale = 2)
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] - deltaFee);
    }
}

BOOST_AUTO_TEST_CASE(FeeEstimateFileRoundTrip)
{
    const fs::path saved_path = m_path_root / "fee-estimates.dat";
    CBlockPolicyEstimator original{saved_path, /*read_stale_estimates=*/false};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
    for (unsigned int height = 0; height < 100; ++height) {
        tx.vin[0].prevout.n = height;
        const auto entry = TestMemPoolEntryHelper().Fee(100000).Height(height).FromTx(tx);
        original.processTransaction(entry, /*validFeeEstimate=*/true);
        std::vector<const CTxMemPoolEntry*> entries{&entry};
        original.processBlock(height + 1, entries);
    }

    const CAmount expected_smart = original.estimateSmartFee(2, nullptr, /*conservative=*/false).GetFeePerK();
    const CAmount expected_raw = original.estimateRawFee(2, 0.85, FeeEstimateHorizon::SHORT_HALFLIFE).GetFeePerK();
    BOOST_REQUIRE_GT(expected_smart, 0);
    BOOST_REQUIRE_GT(expected_raw, 0);
    {
        AutoFile file{fsbridge::fopen(saved_path, "wb")};
        BOOST_REQUIRE(!file.IsNull());
        BOOST_REQUIRE(original.Write(file));
    }
    {
        AutoFile file{fsbridge::fopen(saved_path, "rb")};
        int required_version, writer_version;
        file >> required_version >> writer_version;
        BOOST_CHECK_EQUAL(required_version, 149900);
        BOOST_CHECK_EQUAL(writer_version, CLIENT_VERSION);
    }

    const auto check_estimates = [&](const CBlockPolicyEstimator& estimator) {
        BOOST_CHECK_EQUAL(estimator.estimateSmartFee(2, nullptr, /*conservative=*/false).GetFeePerK(), expected_smart);
        BOOST_CHECK_EQUAL(estimator.estimateRawFee(2, 0.85, FeeEstimateHorizon::SHORT_HALFLIFE).GetFeePerK(), expected_raw);
    };
    CBlockPolicyEstimator restored{m_path_root / "no-estimates.dat", /*read_stale_estimates=*/false};
    {
        AutoFile file{fsbridge::fopen(saved_path, "rb")};
        BOOST_REQUIRE(restored.Read(file));
    }
    check_estimates(restored);
    // A fresh startup must restore the learned estimates before seeing another block.
    CBlockPolicyEstimator restarted{saved_path, /*read_stale_estimates=*/false};
    check_estimates(restarted);

    const auto serialized_state = [&](const CBlockPolicyEstimator& estimator) {
        const fs::path path = m_path_root / "fee-estimates-state.dat";
        {
            AutoFile file{fsbridge::fopen(path, "wb")};
            BOOST_REQUIRE(estimator.Write(file));
        }
        std::vector<std::byte> data(fs::file_size(path));
        AutoFile file{fsbridge::fopen(path, "rb")};
        file.read(data);
        return data;
    };
    const auto saved_state = serialized_state(restored);
    {
        AutoFile file{fsbridge::fopen(saved_path, "r+b")};
        file << 149900 << 999999; // A newer producer can still write the supported format.
    }
    {
        AutoFile file{fsbridge::fopen(saved_path, "rb")};
        BOOST_CHECK(restored.Read(file));
    }
    BOOST_CHECK(serialized_state(restored) == saved_state);
    check_estimates(restored);
    const fs::path other_path = m_path_root / "other-estimates.dat";
    for (const int required_version : {149899, 149900, 149901}) {
        {
            AutoFile file{fsbridge::fopen(other_path, "wb")};
            // The supported header is truncated; the other headers select unsupported formats.
            file << required_version << CLIENT_VERSION << 100U;
        }
        AutoFile file{fsbridge::fopen(other_path, "rb")};
        if (required_version == 149899) {
            BOOST_CHECK(restored.Read(file)); // Old formats are ignored without replacing history.
        } else {
            BOOST_CHECK(!restored.Read(file));
        }
        BOOST_CHECK(serialized_state(restored) == saved_state);
        check_estimates(restored);
    }
    {
        AutoFile file{fsbridge::fopen(other_path, "wb")};
        file << 149900 << CLIENT_VERSION << 100U << 101U << 100U;
    }
    {
        AutoFile file{fsbridge::fopen(other_path, "rb")};
        BOOST_CHECK(!restored.Read(file)); // The historical start cannot exceed its end.
    }
    BOOST_CHECK(serialized_state(restored) == saved_state);
    check_estimates(restored);
}

BOOST_AUTO_TEST_SUITE_END()
