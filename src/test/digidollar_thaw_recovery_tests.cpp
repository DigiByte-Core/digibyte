// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/digidollar_state.h>
#include <digidollar/health.h>
#include <digidollar/validation.h>
#include <kernel/context.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <txdb.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

using CoinSnapshot = std::map<COutPoint, std::tuple<CAmount, CScript, uint32_t, bool>>;

CoinSnapshot SnapshotCoins(const CCoinsView& view)
{
    CoinSnapshot snapshot;
    const auto cursor = view.Cursor();
    BOOST_REQUIRE(cursor);
    BOOST_REQUIRE(cursor->GetBestBlock() == view.GetBestBlock());
    for (; cursor->Valid(); cursor->Next()) {
        COutPoint outpoint;
        Coin coin;
        BOOST_REQUIRE(cursor->GetKey(outpoint));
        BOOST_REQUIRE(cursor->GetValue(coin));
        BOOST_REQUIRE(!coin.IsSpent());
        snapshot.emplace(outpoint, std::make_tuple(coin.out.nValue, coin.out.scriptPubKey,
                                                  uint32_t{coin.nHeight}, coin.IsCoinBase()));
    }
    cursor->CheckStatus();
    return snapshot;
}

struct ThawRecoverySetup : TestChain100Setup {
    static constexpr int THAW_HEIGHT{120};
    static constexpr int TIP_HEIGHT{125};

    struct ChainSnapshot {
        uint256 active_tip;
        uint256 coins_tip;
        CoinSnapshot coins;
        std::vector<std::pair<const CBlockIndex*, uint32_t>> block_status;
    };

    ThawRecoverySetup()
        : TestChain100Setup(ChainType::REGTEST,
                           {"-digidollaractivationheight=100", "-ddthawdayheight=120"},
                           /*coins_db_in_memory=*/false, /*block_tree_db_in_memory=*/false)
    {
        mineBlocks(TIP_HEIGHT - 100);
        LOCK(cs_main);
        auto& chainstate = m_node.chainman->ActiveChainstate();
        BOOST_REQUIRE_EQUAL(chainstate.m_chain.Height(), TIP_HEIGHT);
        const auto state = chainstate.CoinsTip().GetDigiDollarState();
        BOOST_REQUIRE(state);
        BOOST_REQUIRE(state->Matches(Params().GetConsensus().hashGenesisBlock,
                                     chainstate.m_chain.Tip()->GetBlockHash()));
        BOOST_REQUIRE(state->history_checked);
        BOOST_REQUIRE_EQUAL(state->open_vault_principal, 0);
        BOOST_REQUIRE_EQUAL(state->collateral, 0);
        BOOST_REQUIRE_EQUAL(state->active_vaults, 0U);
        BOOST_REQUIRE(chainstate.CoinsTip().Flush());
    }

    ChainSnapshot Snapshot() const EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        AssertLockHeld(cs_main);
        auto& chainstate = m_node.chainman->ActiveChainstate();
        ChainSnapshot snapshot;
        snapshot.active_tip = chainstate.m_chain.Tip()->GetBlockHash();
        snapshot.coins_tip = chainstate.CoinsTip().GetBestBlock();
        snapshot.coins = SnapshotCoins(chainstate.CoinsTip());
        for (const CBlockIndex* index = chainstate.m_chain.Tip(); index; index = index->pprev) {
            snapshot.block_status.emplace_back(index, index->nStatus);
        }
        return snapshot;
    }

    void CheckChainUnchanged(const ChainSnapshot& before) const EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        AssertLockHeld(cs_main);
        auto& chainstate = m_node.chainman->ActiveChainstate();
        BOOST_CHECK(chainstate.m_chain.Tip()->GetBlockHash() == before.active_tip);
        BOOST_CHECK(chainstate.CoinsTip().GetBestBlock() == before.coins_tip);
        BOOST_CHECK(SnapshotCoins(chainstate.CoinsTip()) == before.coins);
        for (const auto& [index, status] : before.block_status) {
            BOOST_CHECK_EQUAL(index->nStatus, status);
        }
    }

    DigiDollar::ChainstateHealth WrongTotals() const EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        AssertLockHeld(cs_main);
        const auto state = m_node.chainman->ActiveChainstate().CoinsTip().GetDigiDollarState();
        BOOST_REQUIRE(state);
        auto wrong = *state;
        BOOST_REQUIRE(wrong.AddVault(100, 200 * COIN));
        BOOST_REQUIRE(wrong.IsValid());
        return wrong;
    }

    void SaveRecord(const std::optional<DigiDollar::ChainstateHealth>& state)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        AssertLockHeld(cs_main);
        auto& chainstate = m_node.chainman->ActiveChainstate();
        chainstate.CoinsTip().SetDigiDollarState(state);
        BOOST_REQUIRE(chainstate.CoinsTip().Flush());
        BOOST_REQUIRE(chainstate.CoinsDB().GetDigiDollarState() == state);
    }

    CBlockIndex* RewindPrivateViewToAnchor(CCoinsViewCache& view) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        auto& chainstate = m_node.chainman->ActiveChainstate();
        CBlockIndex* index = chainstate.m_chain.Tip();
        while (index->nHeight >= THAW_HEIGHT) {
            CBlock block;
            BOOST_REQUIRE(chainstate.m_blockman.ReadBlockFromDisk(block, *index));
            BOOST_REQUIRE_EQUAL(chainstate.DisconnectBlock(block, index, view, true), DISCONNECT_OK);
            index = index->pprev;
        }
        return index;
    }
};

// Change only the index's local read position; restore it even if an assertion fails.
struct UnavailableRecoveryData {
    unsigned int& position;
    const unsigned int saved_position;

    explicit UnavailableRecoveryData(unsigned int& value)
        : position(value), saved_position(value)
    {
        AssertLockHeld(cs_main);
        position = std::numeric_limits<unsigned int>::max();
    }

    ~UnavailableRecoveryData()
    {
        AssertLockHeld(cs_main);
        position = saved_position;
    }
};

struct RestoreHeight {
    int& height;
    const int saved;
    explicit RestoreHeight(int& value) : height(value), saved(value) {}
    ~RestoreHeight() { height = saved; }
};

void CheckActionableRecoveryError(const std::string& error, const std::string& missing_data)
{
    BOOST_CHECK_MESSAGE(error.find(missing_data) != std::string::npos, error);
    BOOST_CHECK_MESSAGE(error.find("restore") != std::string::npos ||
                        error.find("download") != std::string::npos, error);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_thaw_recovery_tests, ThawRecoverySetup)

BOOST_AUTO_TEST_CASE(verified_saved_totals_are_independently_repaired)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const auto correct = chainstate.CoinsTip().GetDigiDollarState();
    SaveRecord(WrongTotals());
    const auto before = Snapshot();
    std::string error;
    ASSERT_DEBUG_LOG("DigiDollar health totals repaired at block " + before.active_tip.ToString());
    BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
    BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == correct);
    CheckChainUnchanged(before);
    BOOST_REQUIRE(chainstate.CoinsTip().Flush());
    BOOST_CHECK(chainstate.CoinsDB().GetDigiDollarState() == correct);
}

BOOST_AUTO_TEST_CASE(unchanged_saved_totals_report_verification)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const auto before = Snapshot();
    const auto expected = chainstate.CoinsTip().GetDigiDollarState();
    std::vector<DigiDollarRecoveryPhase> phases;
    std::string error;
    ASSERT_DEBUG_LOG("DigiDollar health totals verified at block " + before.active_tip.ToString());
    BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error,
        [&](DigiDollarRecoveryPhase phase, uint64_t completed, uint64_t total) {
            BOOST_CHECK(total == 0 || completed <= total);
            if (phases.empty() || phases.back() != phase) phases.push_back(phase);
        }), error);
    const std::vector<DigiDollarRecoveryPhase> expected_phases{
        DigiDollarRecoveryPhase::VERIFY_TOTALS, DigiDollarRecoveryPhase::PERSIST};
    BOOST_CHECK(phases == expected_phases);
    BOOST_CHECK(chainstate.CoinsDB().GetDigiDollarState() == expected);
    CheckChainUnchanged(before);
}

BOOST_AUTO_TEST_CASE(recovery_progress_counts_only_activated_history)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    auto& params = const_cast<Consensus::Params&>(m_node.chainman->GetConsensus());
    RestoreHeight restore_dd{params.DigiDollarHeight};
    params.DigiDollarHeight = 123;
    SaveRecord(std::nullopt);
    const auto before = Snapshot();
    std::map<DigiDollarRecoveryPhase, uint64_t> completed_phases;
    std::string error;

    BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error,
        [&](DigiDollarRecoveryPhase phase, uint64_t completed, uint64_t total) {
            if (phase == DigiDollarRecoveryPhase::PREFLIGHT ||
                phase == DigiDollarRecoveryPhase::REWIND ||
                phase == DigiDollarRecoveryPhase::REPLAY) {
                // Only blocks 123, 124 and 125 need the activated rules.
                BOOST_CHECK_EQUAL(total, 3U);
                BOOST_CHECK_LE(completed, 3U);
                completed_phases[phase] = completed;
            }
        }), error);
    BOOST_REQUIRE_EQUAL(completed_phases.size(), 3U);
    for (const auto& [phase, completed] : completed_phases) {
        BOOST_CHECK_EQUAL(completed, 3U);
    }
    CheckChainUnchanged(before);
    const auto state = chainstate.CoinsTip().GetDigiDollarState();
    BOOST_REQUIRE(state);
    BOOST_CHECK(state->history_checked);
    BOOST_CHECK(state->Matches(params.hashGenesisBlock, before.active_tip,
                               params.nDDThawDayHeight, params.DigiDollarHeight));
}

BOOST_AUTO_TEST_CASE(recovery_phase_cancellation_keeps_a_resumable_prefix)
{
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const std::vector<DigiDollarRecoveryPhase> cancel_phases{
        DigiDollarRecoveryPhase::PREFLIGHT, DigiDollarRecoveryPhase::REWIND,
        DigiDollarRecoveryPhase::REBUILD_ANCHOR, DigiDollarRecoveryPhase::REPLAY,
        DigiDollarRecoveryPhase::PERSIST};
    for (const auto cancel_phase : cancel_phases) {
        ChainSnapshot before;
        std::optional<DigiDollar::ChainstateHealth> expected;
        {
            LOCK(cs_main);
            before = Snapshot();
            expected = chainstate.CoinsTip().GetDigiDollarState();
            SaveRecord(std::nullopt);
            std::optional<DigiDollarRecoveryPhase> current_phase;
            std::string error;
            const auto interrupted = [&] {
                if (current_phase != cancel_phase) return false;
                if (cancel_phase == DigiDollarRecoveryPhase::REWIND) return chainstate.m_chain.Height() < TIP_HEIGHT;
                if (cancel_phase == DigiDollarRecoveryPhase::REPLAY) return chainstate.m_chain.Height() >= THAW_HEIGHT;
                return true;
            };
            BOOST_CHECK(!chainstate.InitializeDigiDollarState(interrupted, error,
                [&](DigiDollarRecoveryPhase phase, uint64_t, uint64_t) {
                    // Preserve the requested phase through the mandatory prefix write.
                    if (current_phase != cancel_phase) current_phase = phase;
                }));
            BOOST_CHECK(error.find("interrupted") != std::string::npos || error.find("cancelled") != std::string::npos);
            const int expected_height = cancel_phase == DigiDollarRecoveryPhase::PREFLIGHT ? TIP_HEIGHT :
                cancel_phase == DigiDollarRecoveryPhase::REWIND ? TIP_HEIGHT - 1 :
                cancel_phase == DigiDollarRecoveryPhase::REPLAY ? THAW_HEIGHT : THAW_HEIGHT - 1;
            BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), expected_height);
            BOOST_CHECK(chainstate.CoinsDB().GetBestBlock() == chainstate.CoinsTip().GetBestBlock());
            const auto prefix = chainstate.CoinsDB().GetDigiDollarState();
            if (cancel_phase == DigiDollarRecoveryPhase::REPLAY) {
                BOOST_REQUIRE(prefix);
                BOOST_CHECK(prefix->history_checked);
                BOOST_CHECK(prefix->best_block == chainstate.CoinsDB().GetBestBlock());
            } else {
                BOOST_CHECK(!prefix);
            }

            chainstate.ResetCoinsViews();
            chainstate.InitCoinsDB(1 << 20, false, false);
            BOOST_REQUIRE(chainstate.ReplayBlocks());
            chainstate.InitCoinsCache(1 << 20);
            BOOST_REQUIRE(chainstate.LoadChainTip());
            error.clear();
            BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
        }
        BlockValidationState accepted;
        BOOST_REQUIRE_MESSAGE(chainstate.ActivateBestChain(accepted), accepted.ToString());
        {
            LOCK(cs_main);
            CheckChainUnchanged(before);
            BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(legacy_reconstruction_cancels_without_publishing_and_holds_the_snapshot_lock)
{
    using DigiDollar::HealthScanResult;
    using DigiDollar::SystemHealthMonitor;
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();
    auto& params = const_cast<Consensus::Params&>(chainman.GetConsensus());
    RestoreHeight restore_thaw{params.nDDThawDayHeight};
    params.nDDThawDayHeight = std::numeric_limits<int>::max();
    const auto original = SystemHealthMonitor::GetCachedMetrics();
    struct RestoreMetrics {
        DigiDollar::SystemMetrics value;
        ~RestoreMetrics() { SystemHealthMonitor::RestoreLegacyMetrics(value); }
    } restore_metrics{original};
    DigiDollar::SystemMetrics sentinel = original;
    sentinel.totalDDSupply = 12345;
    sentinel.totalCollateral = 17 * COIN;
    sentinel.totalActivePositions = 3;
    sentinel.systemHealth = 138;
    sentinel.hasCanonicalHealth = true;
    sentinel.tiers = {{30, 456, 7 * COIN, 2, 144}};
    SystemHealthMonitor::RestoreLegacyMetrics(sentinel);
    const auto expected_tip = WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash());
    unsigned int polls{0};
    unsigned int progress_calls{0};
    DigiDollar::HealthScanCallbacks callbacks;
    callbacks.cancelled = [&] {
        AssertLockHeld(cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip()->GetBlockHash() == expected_tip);
        return ++polls >= 4;
    };
    callbacks.progress = [&](uint64_t, uint64_t) {
        AssertLockHeld(cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip()->GetBlockHash() == expected_tip);
        BOOST_CHECK(chainstate.CoinsDB().GetBestBlock() == expected_tip);
        ++progress_calls;
    };
    const auto cancelled = SystemHealthMonitor::ReconstructFromChain(chainman, callbacks);
    BOOST_CHECK(cancelled.status == HealthScanResult::Status::CANCELLED);
    BOOST_CHECK_GE(polls, 4U);
    const auto unchanged = SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(unchanged.totalDDSupply, sentinel.totalDDSupply);
    BOOST_CHECK_EQUAL(unchanged.totalCollateral, sentinel.totalCollateral);
    BOOST_CHECK_EQUAL(unchanged.totalActivePositions, sentinel.totalActivePositions);
    BOOST_CHECK_EQUAL(unchanged.systemHealth, sentinel.systemHealth);
    BOOST_CHECK_EQUAL(unchanged.hasCanonicalHealth, sentinel.hasCanonicalHealth);
    BOOST_REQUIRE_EQUAL(unchanged.tiers.size(), 1U);
    BOOST_CHECK_EQUAL(unchanged.tiers[0].ddMinted, sentinel.tiers[0].ddMinted);
    BOOST_CHECK_EQUAL(unchanged.tiers[0].dgbLocked, sentinel.tiers[0].dgbLocked);
    BOOST_CHECK_EQUAL(unchanged.tiers[0].positions, sentinel.tiers[0].positions);
    BOOST_CHECK_EQUAL(unchanged.tiers[0].healthRatio, sentinel.tiers[0].healthRatio);
    callbacks.cancelled = {};
    const auto complete = SystemHealthMonitor::ReconstructFromChain(chainman, callbacks);
    BOOST_REQUIRE_MESSAGE(complete.status == HealthScanResult::Status::COMPLETE, complete.error);
    BOOST_CHECK_LE(progress_calls, 4U);
    const auto rebuilt = SystemHealthMonitor::GetCachedMetrics();
    BOOST_CHECK_EQUAL(rebuilt.totalDDSupply, 0);
    BOOST_CHECK_EQUAL(rebuilt.totalCollateral, 0);
    BOOST_CHECK_EQUAL(rebuilt.totalActivePositions, 0);
    BOOST_CHECK_EQUAL(rebuilt.tiers[0].lockDays, sentinel.tiers[0].lockDays);
    BOOST_CHECK_EQUAL(rebuilt.tiers[0].ddMinted, 0);
}

BOOST_AUTO_TEST_CASE(absent_and_unchecked_records_revalidate_the_same_post_activation_chain)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const auto correct = chainstate.CoinsTip().GetDigiDollarState();
    auto unchecked = WrongTotals();
    unchecked.history_checked = false;
    const std::vector<std::optional<DigiDollar::ChainstateHealth>> records{std::nullopt, unchecked};
    for (const auto& record : records) {
        SaveRecord(record);
        const auto before = Snapshot();
        std::string error;
        BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
        BOOST_REQUIRE(chainstate.CoinsTip().GetDigiDollarState());
        BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == correct);
        CheckChainUnchanged(before);
        BOOST_REQUIRE(chainstate.CoinsTip().Flush());
        BOOST_CHECK(chainstate.CoinsDB().GetDigiDollarState() == correct);
    }
}

BOOST_AUTO_TEST_CASE(unchecked_history_requires_available_blocks_and_undo)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const auto correct = chainstate.CoinsTip().GetDigiDollarState();
    CBlockIndex* missing = chainstate.m_chain[THAW_HEIGHT + 1];
    BOOST_REQUIRE(missing);
    BOOST_REQUIRE(missing->nStatus & BLOCK_HAVE_DATA);
    BOOST_REQUIRE(missing->nStatus & BLOCK_HAVE_UNDO);
    for (const bool missing_undo : {false, true}) {
        SaveRecord(std::nullopt);
        const auto before = Snapshot();
        std::string error;
        {
            UnavailableRecoveryData unavailable{missing_undo ? missing->nUndoPos : missing->nDataPos};
            BOOST_CHECK(!chainstate.InitializeDigiDollarState({}, error));
            const auto first_error = error;
            BOOST_CHECK(!chainstate.InitializeDigiDollarState({}, error));
            BOOST_CHECK_EQUAL(error, first_error);
        }
        CheckActionableRecoveryError(error, missing_undo ? "undo" : "block");
        BOOST_CHECK(error.find(missing->GetBlockHash().ToString()) != std::string::npos);
        CheckChainUnchanged(before);
        BOOST_CHECK(!chainstate.CoinsTip().GetDigiDollarState());
        BOOST_CHECK(!chainstate.CoinsDB().GetDigiDollarState());

        error.clear();
        BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
        BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == correct);
        CheckChainUnchanged(before);
    }
}

BOOST_AUTO_TEST_CASE(fixed_retention_floor_survives_disconnect_while_index_locks_move)
{
    auto& chainstate = m_node.chainman->ActiveChainstate();
    auto& blockman = chainstate.m_blockman;
    BlockManagerTest fixed_lock{blockman, "digidollar"};
    BlockManagerTest index_lock{blockman, "test_index"};
    CBlockIndex* tip;
    constexpr int future_floor{TIP_HEIGHT + 200};
    {
        LOCK(cs_main);
        tip = chainstate.m_chain.Tip();
        blockman.UpdatePruneLock("digidollar", {.height_first = future_floor, .reorg_sensitive = false});
        blockman.UpdatePruneLock("test_index", {.height_first = future_floor});
    }
    BlockValidationState disconnected;
    BOOST_REQUIRE_MESSAGE(chainstate.InvalidateBlock(disconnected, tip), disconnected.ToString());
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), TIP_HEIGHT - 1);
        BOOST_CHECK_EQUAL(fixed_lock.PruneLock().height_first, future_floor);
        BOOST_CHECK_EQUAL(index_lock.PruneLock().height_first, TIP_HEIGHT - 1);
        chainstate.ResetBlockFailureFlags(tip);
    }
    BlockValidationState reconnected;
    BOOST_REQUIRE_MESSAGE(chainstate.ActivateBestChain(reconnected), reconnected.ToString());
    {
        LOCK(cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip() == tip);
        BOOST_CHECK_EQUAL(fixed_lock.PruneLock().height_first, future_floor);
        BOOST_CHECK_EQUAL(index_lock.PruneLock().height_first, TIP_HEIGHT - 1);
    }
}

BOOST_AUTO_TEST_CASE(cancelled_preflight_or_totals_scan_does_not_publish_state)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const auto correct = chainstate.CoinsTip().GetDigiDollarState();
    for (const bool checked_history : {true, false}) {
        auto wrong = WrongTotals();
        wrong.history_checked = checked_history;
        SaveRecord(wrong);
        const auto before = Snapshot();
        // A checked record is independently scanned; unchecked history is preflighted before rewind.
        unsigned int polls{0};
        std::string error;
        BOOST_CHECK(!chainstate.InitializeDigiDollarState([&] { return ++polls >= 4; }, error));
        BOOST_CHECK_GE(polls, 4U);
        BOOST_CHECK_MESSAGE(error.find("cancel") != std::string::npos ||
                            error.find("interrupted") != std::string::npos, error);
        CheckChainUnchanged(before);
        BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == wrong);
        BOOST_CHECK(chainstate.CoinsDB().GetDigiDollarState() == wrong);

        error.clear();
        BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
        BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == correct);
        CheckChainUnchanged(before);
    }
}

BOOST_AUTO_TEST_CASE(history_proof_is_bound_to_both_configured_rule_heights)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    auto& params = const_cast<Consensus::Params&>(m_node.chainman->GetConsensus());
    RestoreHeight restore_thaw{params.nDDThawDayHeight};
    RestoreHeight restore_dd{params.DigiDollarHeight};
    const auto before = Snapshot();
    std::string error;

    // A proof made under a later boundary cannot cover the newly activated gap.
    params.nDDThawDayHeight = THAW_HEIGHT + 4;
    BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
    auto saved = chainstate.CoinsTip().GetDigiDollarState();
    BOOST_REQUIRE(saved);
    BOOST_REQUIRE(saved->history_checked);
    BOOST_REQUIRE_EQUAL(saved->activation_height, THAW_HEIGHT + 4);
    params.nDDThawDayHeight = THAW_HEIGHT;

    for (const bool change_dd_height : {false, true}) {
        if (change_dd_height) params.DigiDollarHeight += 4;
        CBlockIndex* missing = chainstate.m_chain[THAW_HEIGHT + 1];
        BOOST_REQUIRE(missing);
        error.clear();
        {
            UnavailableRecoveryData unavailable{missing->nDataPos};
            BOOST_CHECK(!chainstate.InitializeDigiDollarState({}, error));
        }
        CheckActionableRecoveryError(error, "block");
        BOOST_CHECK_MESSAGE(error.find(std::to_string(THAW_HEIGHT + 1)) != std::string::npos, error);
        CheckChainUnchanged(before);
        BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == saved);

        error.clear();
        BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
        saved = chainstate.CoinsTip().GetDigiDollarState();
        BOOST_REQUIRE(saved);
        BOOST_REQUIRE(saved->history_checked);
        BOOST_CHECK(saved->Matches(params.hashGenesisBlock, before.active_tip,
                                  params.nDDThawDayHeight, params.DigiDollarHeight));
        CheckChainUnchanged(before);
    }
}

BOOST_AUTO_TEST_CASE(interrupted_rewind_persists_unchecked_progress_and_can_resume)
{
    auto& chainstate = m_node.chainman->ActiveChainstate();
    ChainSnapshot before;
    std::optional<DigiDollar::ChainstateHealth> correct;
    {
        LOCK(cs_main);
        before = Snapshot();
        correct = chainstate.CoinsTip().GetDigiDollarState();
        SaveRecord(std::nullopt);
        std::string error;
        BOOST_CHECK(!chainstate.InitializeDigiDollarState([&] {
            return chainstate.m_chain.Height() < TIP_HEIGHT;
        }, error));
        BOOST_CHECK_MESSAGE(error.find("interrupted") != std::string::npos, error);
        BOOST_REQUIRE_EQUAL(chainstate.m_chain.Height(), TIP_HEIGHT - 1);
        BOOST_CHECK(chainstate.CoinsTip().GetBestBlock() == chainstate.m_chain.Tip()->GetBlockHash());
        BOOST_CHECK(chainstate.CoinsDB().GetBestBlock() == chainstate.CoinsTip().GetBestBlock());
        BOOST_CHECK(!chainstate.CoinsTip().GetDigiDollarState());
        BOOST_CHECK(!chainstate.CoinsDB().GetDigiDollarState());

        // Drop every coins cache and reopen the disk database before resuming.
        chainstate.ResetCoinsViews();
        chainstate.InitCoinsDB(1 << 20, /*in_memory=*/false, /*should_wipe=*/false);
        BOOST_REQUIRE(chainstate.ReplayBlocks());
        chainstate.InitCoinsCache(1 << 20);
        BOOST_REQUIRE(chainstate.LoadChainTip());
        BOOST_REQUIRE_EQUAL(chainstate.m_chain.Height(), TIP_HEIGHT - 1);
        BOOST_CHECK(!chainstate.CoinsTip().GetDigiDollarState());
        BOOST_CHECK(!chainstate.CoinsDB().GetDigiDollarState());

        error.clear();
        BOOST_REQUIRE_MESSAGE(chainstate.InitializeDigiDollarState({}, error), error);
        const auto resumed = chainstate.CoinsTip().GetDigiDollarState();
        BOOST_REQUIRE(resumed);
        BOOST_REQUIRE(resumed->history_checked);
        BOOST_CHECK(resumed->best_block == chainstate.m_chain.Tip()->GetBlockHash());
    }
    // Ordinary activation must validate the remaining candidate above the saved prefix.
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chainstate.ActivateBestChain(state), state.ToString());
    {
        LOCK(cs_main);
        CheckChainUnchanged(before);
        BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == correct);
    }
}

BOOST_AUTO_TEST_CASE(next_block_health_reconstruction_can_be_cancelled)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const auto before = Snapshot();
    const auto saved = chainstate.CoinsTip().GetDigiDollarState();
    CCoinsViewCache candidate{&chainstate.CoinsTip()};
    const auto* parent = RewindPrivateViewToAnchor(candidate);
    const auto expected = candidate.GetDigiDollarState();
    BOOST_REQUIRE(expected);
    candidate.SetDigiDollarState(std::nullopt);
    auto output = *expected;
    BOOST_REQUIRE(output.AddVault(100, COIN));
    const auto unchanged_output = output;
    int health{-1};
    unsigned int polls{0};
    std::string error;
    BOOST_CHECK(!DigiDollar::GetChainstateHealthForNextBlock(parent, Params().GetConsensus(),
        chainstate.m_blockman, candidate, 1000000, health, output, error, [&] { return ++polls >= 4; }));
    BOOST_CHECK_GE(polls, 4U);
    BOOST_CHECK_MESSAGE(error.find("cancel") != std::string::npos, error);
    BOOST_CHECK_EQUAL(health, -1);
    BOOST_CHECK(output == unchanged_output);
    BOOST_CHECK(!candidate.GetDigiDollarState());
    CheckChainUnchanged(before);
    BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == saved);
    BOOST_CHECK(chainstate.CoinsDB().GetDigiDollarState() == saved);

    BOOST_REQUIRE_MESSAGE(DigiDollar::GetChainstateHealthForNextBlock(parent, Params().GetConsensus(),
        chainstate.m_blockman, candidate, 1000000, health, output, error), error);
    BOOST_CHECK_EQUAL(health, 30000);
    BOOST_CHECK(output == *expected);
    BOOST_CHECK(!candidate.GetDigiDollarState());
}

BOOST_AUTO_TEST_CASE(block_preparation_observes_chainstate_interruption)
{
    LOCK(cs_main);
    auto& chainstate = m_node.chainman->ActiveChainstate();
    const auto before = Snapshot();
    const auto saved = chainstate.CoinsTip().GetDigiDollarState();
    CCoinsViewCache candidate{&chainstate.CoinsTip()};
    RewindPrivateViewToAnchor(candidate);
    const auto anchor_coins = SnapshotCoins(candidate);
    const auto anchor_hash = candidate.GetBestBlock();
    candidate.SetDigiDollarState(std::nullopt);
    CBlockIndex* index = chainstate.m_chain[THAW_HEIGHT];
    CBlock block;
    BOOST_REQUIRE(chainstate.m_blockman.ReadBlockFromDisk(block, *index));
    {
        struct ResetInterrupt {
            util::SignalInterrupt& signal;
            ~ResetInterrupt() { signal.reset(); }
        } reset{m_node.kernel->interrupt};
        m_node.kernel->interrupt();
        BlockValidationState state;
        BOOST_CHECK(!chainstate.ConnectBlock(block, state, index, candidate, true));
        BOOST_CHECK(state.IsError());
        BOOST_CHECK_MESSAGE(state.ToString().find("cancel") != std::string::npos, state.ToString());
        BOOST_CHECK(!candidate.GetDigiDollarState());
        BOOST_CHECK(candidate.GetBestBlock() == anchor_hash);
        BOOST_CHECK(SnapshotCoins(candidate) == anchor_coins);
    }
    BlockValidationState accepted;
    BOOST_REQUIRE_MESSAGE(chainstate.ConnectBlock(block, accepted, index, candidate, true), accepted.ToString());
    BOOST_REQUIRE(candidate.GetDigiDollarState());
    BOOST_CHECK(candidate.GetDigiDollarState()->history_checked);
    BOOST_CHECK(candidate.GetBestBlock() == index->GetBlockHash());
    CheckChainUnchanged(before);
    BOOST_CHECK(chainstate.CoinsTip().GetDigiDollarState() == saved);
    BOOST_CHECK(chainstate.CoinsDB().GetDigiDollarState() == saved);
}

BOOST_AUTO_TEST_SUITE_END()
