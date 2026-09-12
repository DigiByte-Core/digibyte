// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Regression tests for the RC30 "254 GB debug.log" incident.
//
// Two stuck DigiDollar TRANSFER transactions in the mempool caused four
// LogPrintf() call sites to fire thousands of times per second on the same
// revalidation path, filling the disk and crashing the host. This suite
// codifies the invariant that those specific hot-path messages must be
// gated behind BCLog::DIGIDOLLAR so they stay silent unless the operator
// opts in with -debug=digidollar.
//

#include <addresstype.h>
#include <chainparams.h>
#include <consensus/digidollar.h>
#include <consensus/validation.h>
#include <consensus/volatility.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <logging.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs_test = std::filesystem;

namespace {

// Mirrors the LogSetup helper in logging_tests.cpp: redirects the process
// log to a per-test temp file so we can read back what the code wrote.
struct HotPathLogSetup : public BasicTestingSetup {
    fs::path prev_log_path;
    fs::path tmp_log_path;
    bool prev_reopen_file;
    bool prev_print_to_file;
    bool prev_log_timestamps;
    bool prev_log_threadnames;
    bool prev_log_sourcelocations;
    std::unordered_map<BCLog::LogFlags, BCLog::Level> prev_category_levels;
    BCLog::Level prev_log_level;
    uint32_t prev_categories;

    HotPathLogSetup()
        : prev_log_path{LogInstance().m_file_path},
          tmp_log_path{m_args.GetDataDirBase() / "tmp_hot_path_debug.log"},
          prev_reopen_file{LogInstance().m_reopen_file},
          prev_print_to_file{LogInstance().m_print_to_file},
          prev_log_timestamps{LogInstance().m_log_timestamps},
          prev_log_threadnames{LogInstance().m_log_threadnames},
          prev_log_sourcelocations{LogInstance().m_log_sourcelocations},
          prev_category_levels{LogInstance().CategoryLevels()},
          prev_log_level{LogInstance().LogLevel()},
          prev_categories{LogInstance().GetCategoryMask()}
    {
        LogInstance().m_file_path = tmp_log_path;
        LogInstance().m_reopen_file = true;
        LogInstance().m_print_to_file = true;
        LogInstance().m_log_timestamps = false;
        LogInstance().m_log_threadnames = false;
        LogInstance().m_log_sourcelocations = false;
        LogInstance().SetLogLevel(BCLog::Level::Debug);
        LogInstance().SetCategoryLogLevel({});
        // Start with no categories enabled — the default production state.
        LogInstance().DisableCategory(BCLog::LogFlags::ALL);
    }

    ~HotPathLogSetup()
    {
        LogInstance().m_file_path = prev_log_path;
        LogPrintf("Sentinel log to reopen log file\n");
        LogInstance().m_print_to_file = prev_print_to_file;
        LogInstance().m_reopen_file = prev_reopen_file;
        LogInstance().m_log_timestamps = prev_log_timestamps;
        LogInstance().m_log_threadnames = prev_log_threadnames;
        LogInstance().m_log_sourcelocations = prev_log_sourcelocations;
        LogInstance().SetLogLevel(prev_log_level);
        LogInstance().SetCategoryLogLevel(prev_category_levels);
        LogInstance().DisableCategory(BCLog::LogFlags::ALL);
        // Restore previously-enabled categories by flag-by-flag re-enable.
        for (uint32_t bit = 0; bit < 32; ++bit) {
            const auto flag = static_cast<BCLog::LogFlags>(1U << bit);
            if (prev_categories & (1U << bit)) {
                LogInstance().EnableCategory(flag);
            }
        }
    }

    std::string ReadLog() const
    {
        std::ifstream file{tmp_log_path};
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }
};

struct VolatilityLogSetup : public HotPathLogSetup {
    VolatilityLogSetup()
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    }

    ~VolatilityLogSetup()
    {
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    }
};

// Resolve the src/ root from __FILE__ so the invariant test can read
// the hot-path .cpp files regardless of the cwd chosen by `make check`.
fs_test::path ResolveSrcRoot()
{
    fs_test::path p = fs_test::absolute(fs_test::path(__FILE__)).parent_path().parent_path();
    if (fs_test::exists(p / "digidollar" / "validation.cpp")) return p;
    fs_test::path cwd = fs_test::current_path();
    for (fs_test::path d = cwd; !d.empty(); d = d.parent_path()) {
        if (fs_test::exists(d / "digidollar" / "validation.cpp")) return d;
        if (fs_test::exists(d / "src" / "digidollar" / "validation.cpp")) return d / "src";
        if (d == d.parent_path()) break;
    }
    return {};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_hot_path_logging_tests, HotPathLogSetup)

BOOST_FIXTURE_TEST_CASE(volatility_price_logging_follows_debug_category, VolatilityLogSetup)
{
    using DigiDollar::Volatility::VolatilityMonitor;

    for (const bool debug_enabled : {false, true}) {
        VolatilityMonitor::ClearHistory();
        if (debug_enabled) LogInstance().EnableCategory(BCLog::DIGIDOLLAR);

        LogPrintf("volatility-log-start\n");
        const size_t log_start = ReadLog().size();

        VolatilityMonitor::RecordPrice(50000, 10000, 10);
        VolatilityMonitor::RecordPrice(50000, 10001, 11);
        VolatilityMonitor::RecordPrice(-1, 13600, 12);

        const auto history = VolatilityMonitor::GetPriceHistory();
        BOOST_REQUIRE_EQUAL(history.size(), 1U);
        BOOST_CHECK_EQUAL(history[0].price, 50000);
        BOOST_CHECK_EQUAL(history[0].height, 10U);
        BOOST_CHECK(!VolatilityMonitor::GetCurrentState().mintingFrozen);

        const std::string log = ReadLog().substr(log_start);
        BOOST_CHECK_EQUAL(log.find("VolatilityMonitor: Recorded price") != std::string::npos, debug_enabled);
        BOOST_CHECK_EQUAL(log.find("VolatilityMonitor: Skipping price update") != std::string::npos, debug_enabled);
        BOOST_CHECK(log.find("VolatilityMonitor: Rejecting invalid candidate price -1") != std::string::npos);
    }
}

BOOST_FIXTURE_TEST_CASE(volatility_warning_and_recovery_logs_remain_visible, VolatilityLogSetup)
{
    using DigiDollar::Volatility::VolatilityMonitor;

    VolatilityMonitor::RecordPrice(50000, 10000, 10);
    VolatilityMonitor::RecordPrice(100000, 13600, 11);
    BOOST_CHECK(VolatilityMonitor::GetCurrentState().allOperationsFrozen);
    const auto history = VolatilityMonitor::GetPriceHistory();
    VolatilityMonitor::ReconstructFromBlockData(history, 11);
    BOOST_CHECK(VolatilityMonitor::GetCurrentState().allOperationsFrozen);

    const std::string log = ReadLog();
    BOOST_CHECK(log.find("VolatilityMonitor: WARNING - High volatility detected") != std::string::npos);
    BOOST_CHECK(log.find("VolatilityMonitor: FREEZE - All DigiDollar operations frozen") != std::string::npos);
    BOOST_CHECK(log.find("VolatilityMonitor: Reconstructing state from") != std::string::npos);
    BOOST_CHECK(log.find("VolatilityMonitor: Reconstruction complete") != std::string::npos);
    BOOST_CHECK(log.find("VolatilityMonitor: Recorded price") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(mint_parameter_logs_keep_failures_visible)
{
    DigiDollar::MintTxBuilder builder(Params(), 1000, 50000);
    DigiDollar::TxBuilderMintParams mint_params;
    mint_params.ddAmount = 10000;
    mint_params.lockDays = 365;
    const int tier = DigiDollar::GetLockTierIndex(builder.LockDaysToBlocks(mint_params.lockDays), Params().GetDigiDollarParams());
    BOOST_REQUIRE_GE(tier, 0);
    mint_params.lockTier = static_cast<uint32_t>(tier);
    mint_params.ownerKey.MakeNewKey(true);
    mint_params.feeRate = 100000;
    mint_params.utxos.emplace_back(uint256::ONE, 0);

    for (const bool debug_enabled : {false, true}) {
        if (debug_enabled) LogInstance().EnableCategory(BCLog::DIGIDOLLAR);

        LogPrintf("mint-parameter-log-start\n");
        const size_t log_start = ReadLog().size();
        BOOST_CHECK(builder.ValidateMintParams(mint_params));
        auto invalid_params = mint_params;
        invalid_params.ddAmount = 0;
        BOOST_CHECK(!builder.ValidateMintParams(invalid_params));

        const std::string log = ReadLog().substr(log_start);
        BOOST_CHECK_EQUAL(log.find("ValidateMintParams PASSED") != std::string::npos, debug_enabled);
        BOOST_CHECK(log.find("ValidateMintParams FAILED: ddAmount <= 0 (0)") != std::string::npos);
    }
}

BOOST_FIXTURE_TEST_CASE(mint_change_logging_preserves_validity, VolatilityLogSetup)
{
    struct HealthGuard {
        DigiDollar::SystemMetrics previous{DigiDollar::SystemHealthMonitor::GetCachedMetrics()};
        HealthGuard() { DigiDollar::SystemHealthMonitor::ResetMetrics(); }
        ~HealthGuard() { DigiDollar::SystemHealthMonitor::RestoreLegacyMetrics(previous); }
    } health_guard;

    const auto chain_params = CChainParams::RegTest({});
    const DigiDollar::ValidationContext context(1000, 500000, 150, *chain_params);
    const CAmount dd_amount{10000};
    const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(30);
    const int tier = DigiDollar::GetLockTierIndex(lock_blocks, chain_params->GetDigiDollarParams());
    BOOST_REQUIRE_GE(tier, 0);
    const CAmount collateral = DigiDollar::CalculateRequiredCollateral(dd_amount, lock_blocks, context);
    BOOST_REQUIRE_GT(collateral, 1);

    CKey owner_key;
    owner_key.MakeNewKey(true);
    const XOnlyPubKey owner(owner_key.GetPubKey());
    DigiDollar::MintParams mint_params;
    mint_params.ddAmount = dd_amount;
    mint_params.lockHeight = context.nHeight + lock_blocks;
    mint_params.ownerKey = owner;
    mint_params.internalKey = DigiDollar::GetCollateralNUMSKey();
    mint_params.oracleKeys = DigiDollar::GetOracleKeys(15);

    CMutableTransaction mint;
    mint.nVersion = 0x01000770;
    mint.vin.emplace_back(COutPoint(uint256::ONE, 0));
    mint.vout.emplace_back(collateral, DigiDollar::CreateCollateralP2TR(mint_params));
    mint.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(owner, dd_amount));
    mint.vout.emplace_back(COIN, GetScriptForDestination(WitnessV0KeyHash(owner_key.GetPubKey())));
    mint.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                      << CScriptNum(1) << CScriptNum(dd_amount)
                                      << CScriptNum(mint_params.lockHeight) << CScriptNum(tier)
                                      << std::vector<unsigned char>(owner.begin(), owner.end()));
    int change_version{-1};
    std::vector<unsigned char> change_program;
    BOOST_REQUIRE(mint.vout[2].scriptPubKey.IsWitnessProgram(change_version, change_program));
    BOOST_REQUIRE_EQUAL(change_version, 0);
    BOOST_REQUIRE_EQUAL(change_program.size(), 20U);
    const CTransaction valid_mint(mint);

    // DGB change cannot make up for an underfunded collateral output.
    mint.vout[0].nValue = collateral - 1;
    const CTransaction insufficient_mint(mint);

    for (const bool debug_enabled : {false, true}) {
        if (debug_enabled) LogInstance().EnableCategory(BCLog::DIGIDOLLAR);

        LogPrintf("mint-change-log-start\n");
        const size_t log_start = ReadLog().size();
        TxValidationState valid_state;
        BOOST_REQUIRE_MESSAGE(DigiDollar::ValidateMintTransaction(valid_mint, context, valid_state),
                              valid_state.ToString());
        BOOST_CHECK(valid_state.IsValid());
        const std::string valid_log = ReadLog().substr(log_start);
        BOOST_CHECK_EQUAL(valid_log.find("DigiDollar: Output 2 is non-P2TR change output") != std::string::npos,
                          debug_enabled);

        TxValidationState insufficient_state;
        BOOST_CHECK(!DigiDollar::ValidateMintTransaction(insufficient_mint, context, insufficient_state));
        BOOST_CHECK_EQUAL(insufficient_state.GetRejectReason(), "insufficient-collateral");
        BOOST_CHECK(ReadLog().substr(log_start).find("DigiDollar: Insufficient collateral:") != std::string::npos);
    }
}

// RED→GREEN behaviour test: exercises bundle_manager.cpp's
// GetCurrentOraclePriceMicroUSD() no-price fallback (the line that fired
// in the 254 GB incident). Pre-fix this wrote a line every call via
// LogPrintf; post-fix it must be silent unless DIGIDOLLAR is enabled.
BOOST_AUTO_TEST_CASE(get_current_oracle_price_no_price_log_is_gated_by_default)
{
    // Force the mock oracle to report "no price" so we fall through to the
    // real OracleBundleManager path; clear the manager so its cache is empty
    // and GetLatestPrice() returns 0, which triggers the fallback log.
    MockOracleManager::GetInstance().SetMockPrice(0);
    OracleBundleManager::GetInstance().Clear();

    // Simulate a hot-path revalidation loop. Pre-fix this writes ~50 lines;
    // post-fix the category gate keeps the log empty.
    for (int i = 0; i < 50; ++i) {
        (void)OracleIntegration::GetCurrentOraclePriceMicroUSD();
    }

    // Flush by forcing a reopen and emitting a sentinel line.
    LogInstance().m_reopen_file = true;
    LogPrintf("hot-path-log-flush-sentinel\n");

    const std::string log = ReadLog();
    BOOST_CHECK_MESSAGE(
        log.find("No oracle price available in GetCurrentOraclePriceMicroUSD") == std::string::npos,
        "RC30 incident regression: 'No oracle price available' must be gated behind -debug=digidollar. "
        "Found " << std::count(log.begin(), log.end(), '\n') << " log lines; first 200 chars: "
        << log.substr(0, 200));
}

// Complement: when the operator opts into -debug=digidollar, the message
// must still appear so debugging visibility is not lost.
BOOST_AUTO_TEST_CASE(get_current_oracle_price_no_price_log_visible_when_digidollar_enabled)
{
    LogInstance().EnableCategory(BCLog::DIGIDOLLAR);

    MockOracleManager::GetInstance().SetMockPrice(0);
    OracleBundleManager::GetInstance().Clear();

    (void)OracleIntegration::GetCurrentOraclePriceMicroUSD();

    LogInstance().m_reopen_file = true;
    LogPrintf("hot-path-log-flush-sentinel\n");

    const std::string log = ReadLog();
    BOOST_CHECK_MESSAGE(
        log.find("No oracle price available in GetCurrentOraclePriceMicroUSD") != std::string::npos,
        "Oracle no-price message must still be emitted when DIGIDOLLAR category is enabled. "
        "Log first 200 chars: " << log.substr(0, 200));
}

// Source-invariant test: the four messages that filled the 254 GB
// debug.log must be emitted through LogPrint(BCLog::DIGIDOLLAR, ...)
// rather than a raw LogPrintf(). This catches regressions that the two
// behaviour tests above can't reach (e.g. the stale-cache branch inside
// OracleBundleManager::GetLatestPrice, whose internal state requires
// friend access to force from a unit test).
BOOST_AUTO_TEST_CASE(rc30_hot_path_logs_are_gated_in_source)
{
    const fs_test::path src_root = ResolveSrcRoot();
    BOOST_REQUIRE_MESSAGE(!src_root.empty(),
                          "could not locate src/ from __FILE__ or cwd");

    struct Site {
        fs_test::path file;
        std::string marker;
    };
    const std::vector<Site> sites = {
        {src_root / "digidollar" / "validation.cpp",
         "DigiDollar: Validating %s transaction (txid: %s)"},
        {src_root / "digidollar" / "validation.cpp",
         "Could not determine input DD amounts for conservation check"},
        {src_root / "oracle" / "bundle_manager.cpp",
         "Rejecting stale cached price %lld micro-USD"},
        {src_root / "oracle" / "bundle_manager.cpp",
         "No oracle price available in GetCurrentOraclePriceMicroUSD"},
        // ATMP wrapper site missed by the original gating pass. Fires from
        // AcceptToMemoryPool → BroadcastTransaction → Dandelion stempool on
        // every rejected DD tx; matches shenger's repro log line exactly.
        {src_root / "validation.cpp",
         "DigiDollar: Transaction validation failed (txid: %s): %s"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: TickEpochSession h=%d epoch=%d state=%d is_oracle=%d"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: Step 1 - local_ids.size()=%zu for epoch %d"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: Step 1 - all_oracle_ids.size()=%zu"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: Step 1 - key aggregation succeeded for %zu oracle IDs"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: Skipping oracle %d epoch %d - already broadcast"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: Skipping oracle %d - GetOracleNode returned null"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: Skipping oracle %d - invalid private key"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: oracle %d pubkey size=%d hex=%s"},
        {src_root / "oracle" / "signing_orchestrator.cpp",
         "Oracle: Skipping oracle %d - secp256k1_ec_pubkey_parse failed"},
    };

    for (const auto& site : sites) {
        std::ifstream f(site.file);
        BOOST_REQUIRE_MESSAGE(f.is_open(), "cannot open " << site.file.string());
        std::stringstream buf;
        buf << f.rdbuf();
        const std::string content = buf.str();

        const size_t marker_pos = content.find(site.marker);
        BOOST_REQUIRE_MESSAGE(marker_pos != std::string::npos,
                              "marker not found in " << site.file.filename().string()
                              << ": '" << site.marker << "'");

        // Inspect the ~200 chars preceding the marker. The enclosing
        // logging macro must be LogPrint(BCLog::DIGIDOLLAR, ...), never a
        // raw LogPrintf(.
        const size_t scan_start = marker_pos > 200 ? marker_pos - 200 : 0;
        const std::string before = content.substr(scan_start, marker_pos - scan_start);

        const size_t logprint_pos = before.rfind("LogPrint(BCLog::DIGIDOLLAR");
        const size_t logprintf_pos = before.rfind("LogPrintf(");

        const bool wrapped_in_logprint = logprint_pos != std::string::npos;
        const bool wrapped_in_logprintf =
            logprintf_pos != std::string::npos &&
            (!wrapped_in_logprint || logprintf_pos > logprint_pos);

        BOOST_CHECK_MESSAGE(
            !wrapped_in_logprintf,
            "RC30 incident regression: '" << site.marker << "' in "
            << site.file.filename().string()
            << " is emitted through LogPrintf — must use LogPrint(BCLog::DIGIDOLLAR, ...)");
        BOOST_CHECK_MESSAGE(
            wrapped_in_logprint,
            "'" << site.marker << "' in " << site.file.filename().string()
            << " must be wrapped in LogPrint(BCLog::DIGIDOLLAR, ...)");
    }
}

BOOST_AUTO_TEST_SUITE_END()
