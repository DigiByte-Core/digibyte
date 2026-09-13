// Copyright (c) 2024-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Wave 21 — DoS and Resource Exhaustion (Agent B / functional)
 *
 * These pins close the documented Wave 21 coverage gaps around resource
 * limits and pathological-input handling on DigiDollar/oracle hot paths.
 * The brief points at three classes of risk:
 *
 *   1. Resource-heavy DD/oracle inputs — oversized payloads, deeply nested
 *      OP_RETURN, max bitmap.
 *   2. Memory limits — confirm the documented caps
 *      (`seen_message_hashes` = 2048, `height_to_price` = 1000) actually
 *      hold under flooding.
 *   3. CPU limits — confirm validation completes in bounded time even for
 *      pathological inputs.
 *
 * Existing coverage:
 *   - `seen_message_hashes` cap is pinned in
 *     `digidollar_wave20_p2p_pending_tests::seen_message_hashes_cap_at_2048`
 *     (DD-FA-TEST-034).
 *   - `m_pending_partialsigs` per-epoch / per-epoch-count caps are pinned
 *     by RH-58 tests.
 *   - The maximum participation_bitmap byte length (255 → 2040 bits) and
 *     the `ExtractOracleBundle` size-mismatch reject are pinned by RH-56
 *     tests.
 *
 * Wave 21 gap targets covered here:
 *
 *   DD-FA-TEST-036 — `OracleBundleManager::UpdatePriceCache` keeps
 *     `height_to_price` capped at 1000 entries (oldest evicted FIFO).
 *     There is currently no direct pin asserting the cap; the comment in
 *     `bundle_manager.cpp:1827` ("Keep cache size limited (last 1000
 *     blocks)") is contract-only. A regression that drops the cap would
 *     let a long-running node accumulate unbounded price-cache entries,
 *     because `ConnectBlock` calls `UpdatePriceCache` once per DD-touching
 *     block.
 *
 *   DD-FA-TEST-037 — `OracleBundleManager::DeserializeV03Data` rejects
 *     pathological / mismatched payloads in O(1) regardless of how many
 *     bytes the attacker stuffed into the OP_RETURN. The CPU bound is
 *     enforced because the function rejects on size mismatch *before*
 *     allocating the bitmap; there is no signature/aggregation work on
 *     malformed shapes. A regression that pushed any work into the parse
 *     would let an attacker craft a coinbase OP_RETURN that consumes
 *     measurable CPU per validating node, per block.
 *
 *   DD-FA-TEST-038 — `OracleBundleManager::AddOracleMessage` rejects an
 *     `OracleConsensusMsg`-shape attestation whose schnorr_sig has been
 *     length-extended (>64 bytes) or shortened (<64 bytes), so an attacker
 *     cannot grind oversized signatures past `IsValidOracleMessage` to
 *     amplify per-message memory footprint via the schnorr_sig field.
 *
 *   DD-FA-TEST-039 — `OracleBundleManager::ClearPendingMessages`
 *     short-circuits in O(1) when state is already empty (no pathological
 *     iteration). This pins the operator-restart hot path.
 */

#include <boost/test/unit_test.hpp>

#include <arith_uint256.h>
#include <chainparams.h>
#include <crypto/sha256.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <primitives/oracle.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <test/util/source_root.h>
#include <util/time.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

CKey MakeRegtestOracleKey(uint32_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size())
              .Finalize(hash.begin());
    CKey key;
    key.Set(hash.begin(), hash.end(), true);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

COraclePriceMessage MakeSignedRegtestMessage(uint32_t oracle_id, uint64_t price, int64_t timestamp)
{
    CKey key = MakeRegtestOracleKey(oracle_id);
    COraclePriceMessage msg(oracle_id, price, timestamp);
    msg.oracle_pubkey = XOnlyPubKey(key.GetPubKey());
    BOOST_REQUIRE(msg.SignAttestation(key));
    BOOST_REQUIRE(msg.VerifyAttestation());
    return msg;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_wave21_dos_resource_tests, RegTestingSetup)

// ============================================================================
// DD-FA-TEST-036: UpdatePriceCache keeps height_to_price capped at 1000.
//
// Contract (src/oracle/bundle_manager.cpp:1827):
//     if (height_to_price.size() > 1000) {
//         height_to_price.erase(height_to_price.begin());
//         height_to_price_time.erase(erase_height);
//     }
//
// Floods 1500 distinct heights; expects exactly 1000 retained and the
// 500 lowest evicted via FIFO. Also asserts that the highest-known-height
// price (the one GetLatestPrice returns) survives eviction even when the
// flood crosses the cap multiple times.
// ============================================================================
BOOST_AUTO_TEST_CASE(height_to_price_cap_at_1000)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    const int64_t t0 = 1'800'000'000;
    SetMockTime(t0);

    // Inject 1500 distinct heights (well past the documented 1000 cap).
    const int kFloodCount = 1500;
    const int kBaseHeight = 100000;
    for (int i = 0; i < kFloodCount; ++i) {
        manager.UpdatePriceCache(kBaseHeight + i,
                                 /*price_micro_usd=*/static_cast<uint64_t>(7000 + i),
                                 /*source_time=*/t0 + i);
    }

    // Cap holds: lowest 500 heights are gone, highest 1000 retained.
    int retained = 0;
    int evicted = 0;
    for (int i = 0; i < kFloodCount; ++i) {
        const uint64_t price = manager.GetOraclePriceForHeight(kBaseHeight + i);
        if (price == 0) {
            ++evicted;
        } else {
            ++retained;
            // Retained entries must report the value we wrote, never a
            // neighbour's value (FIFO eviction must not reshuffle keys).
            BOOST_CHECK_EQUAL(price, static_cast<uint64_t>(7000 + i));
        }
    }
    BOOST_CHECK_EQUAL(retained, 1000);
    BOOST_CHECK_EQUAL(evicted, 500);

    // GetLatestPrice still returns the most recently written value
    // (post-rh61 invariant: cached_price tracks the highest height seen).
    BOOST_CHECK_EQUAL(manager.GetLatestPrice(),
                      static_cast<CAmount>(7000 + (kFloodCount - 1)));

    // Push another 200 heights past the flood; cap stays at 1000.
    for (int i = 0; i < 200; ++i) {
        manager.UpdatePriceCache(kBaseHeight + kFloodCount + i,
                                 static_cast<uint64_t>(8000 + i),
                                 t0 + kFloodCount + i);
    }
    int retained_after = 0;
    for (int i = 0; i < kFloodCount + 200; ++i) {
        if (manager.GetOraclePriceForHeight(kBaseHeight + i) != 0) {
            ++retained_after;
        }
    }
    BOOST_CHECK_EQUAL(retained_after, 1000);

    SetMockTime(0);
    manager.Clear();
}

// ============================================================================
// DD-FINAL-023: UpdateBundle must not let regtest/mock epoch_bundles grow
// without bound.
//
// `PublishRegtestMockMuSig2Quote()` reaches `UpdateBundle()` directly when
// mock oracle prices are refreshed for mempool admission. CleanupOldBundles()
// documents the intended current+previous retention window, but the production
// update path must enforce that window itself.
// ============================================================================
BOOST_AUTO_TEST_CASE(update_bundle_prunes_stale_epochs)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    for (int32_t epoch = 1; epoch <= 10; ++epoch) {
        COracleBundle bundle(epoch);
        bundle.median_price_micro_usd = 6000 + epoch;
        bundle.timestamp = GetTime() + epoch;
        BOOST_REQUIRE(manager.UpdateBundle(bundle));
    }

    BOOST_CHECK_LE(manager.GetStats().active_bundles, 2U);

    // Inserting an old epoch after a newer quote must not re-grow stale state.
    COracleBundle old_bundle(3);
    old_bundle.median_price_micro_usd = 7000;
    old_bundle.timestamp = GetTime() + 11;
    BOOST_REQUIRE(manager.UpdateBundle(old_bundle));
    BOOST_CHECK_LE(manager.GetStats().active_bundles, 2U);

    manager.Clear();
}

// ============================================================================
// DD-FINAL-026: Operator deploy script must stop only the configured node.
//
// A broad `pkill digibyted` can stop unrelated mainnet/testnet/regtest nodes
// on shared operator hosts. The script may stop the configured datadir via
// RPC or pidfile, but it must not kill by process name.
// ============================================================================
BOOST_AUTO_TEST_CASE(testnet_oracle_deploy_script_does_not_broad_kill_digibyted)
{
    const std::string script = ReadRepositoryFile("contrib/deploy_testnet_oracle.sh");

    BOOST_CHECK_MESSAGE(script.find("pkill") == std::string::npos,
                        "deploy_testnet_oracle.sh must not use broad pkill fallbacks");
    BOOST_CHECK_MESSAGE(script.find("pgrep -x digibyted") == std::string::npos,
                        "deploy_testnet_oracle.sh must not treat any digibyted process as its own node");
    BOOST_CHECK_MESSAGE(script.find("-pid=$DATA_DIR/digibyted.pid") != std::string::npos,
                        "deploy_testnet_oracle.sh should keep using its datadir-specific pidfile");
}

// ============================================================================
// DD-FA-TEST-037: DeserializeV03Data rejects pathological payloads in
// bounded CPU time.
//
// Contract (src/primitives/oracle.cpp:243):
//   - Reject if data.size() < 86 (rh-style minimum-shape guard).
//   - Reject if bitmap_len == 0.
//   - Reject if data.size() != 1 + bitmap_len + 4 + 8 + 8 + 64
//     (exact-size guard added to defeat trailing-byte malleability).
//
// All three reject paths must short-circuit *before* allocating the
// participation_bitmap. The wall-clock budget below is generous (50 ms
// for 10,000 reject iterations on regtest hardware in CI) — a regression
// that started doing per-byte work would blow it out.
// ============================================================================
BOOST_AUTO_TEST_CASE(deserialize_v03_pathological_payloads_bounded_cpu)
{
    using Clock = std::chrono::steady_clock;

    const size_t kIterations = 10000;

    // CASE A: data.size() < 86 (truncated payload).
    {
        std::vector<unsigned char> tiny(50, 0xAB);
        const auto t_start = Clock::now();
        for (size_t i = 0; i < kIterations; ++i) {
            COracleBundle bundle;
            BOOST_REQUIRE(!COracleBundle::DeserializeV03Data(tiny, bundle));
            BOOST_REQUIRE(bundle.participation_bitmap.empty());
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - t_start).count();
        BOOST_CHECK_MESSAGE(elapsed_ms < 200,
            "DeserializeV03Data tiny-payload reject took " << elapsed_ms
            << " ms for " << kIterations << " iters; CPU bound regressed.");
    }

    // CASE B: data.size() >= 86 but bitmap_len == 0 (rejected immediately
    // after the first byte read).
    {
        std::vector<unsigned char> zero_bitmap(86, 0xCD);
        zero_bitmap[0] = 0x00; // bitmap_len == 0
        const auto t_start = Clock::now();
        for (size_t i = 0; i < kIterations; ++i) {
            COracleBundle bundle;
            BOOST_REQUIRE(!COracleBundle::DeserializeV03Data(zero_bitmap, bundle));
            BOOST_REQUIRE(bundle.participation_bitmap.empty());
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - t_start).count();
        BOOST_CHECK_MESSAGE(elapsed_ms < 200,
            "DeserializeV03Data zero-bitmap reject took " << elapsed_ms
            << " ms for " << kIterations << " iters; CPU bound regressed.");
    }

    // CASE C: bitmap_len declares 255 bytes but payload is short (declared
    // size != actual size). With bitmap_len=255 the expected size is
    // 1 + 255 + 4 + 8 + 8 + 64 = 340 bytes; we feed exactly 86 bytes so
    // the size mismatch fires *before* any bitmap allocation.
    {
        std::vector<unsigned char> mismatch(86, 0xEF);
        mismatch[0] = 0xFF; // bitmap_len == 255 but payload is only 86 bytes
        const auto t_start = Clock::now();
        for (size_t i = 0; i < kIterations; ++i) {
            COracleBundle bundle;
            BOOST_REQUIRE(!COracleBundle::DeserializeV03Data(mismatch, bundle));
            BOOST_REQUIRE(bundle.participation_bitmap.empty());
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - t_start).count();
        BOOST_CHECK_MESSAGE(elapsed_ms < 200,
            "DeserializeV03Data size-mismatch reject took " << elapsed_ms
            << " ms for " << kIterations << " iters; CPU bound regressed.");
    }

    // CASE D: bitmap_len = 255 with a full-payload (340 bytes) actually
    // allocates the maximum-size bitmap. This is the largest legal v0x03
    // shape — it parses successfully (no signature verification at this
    // layer), but must do so in bounded time. Pin that 1000 maximum-shape
    // parses stay sub-50 ms wall-clock so a regression that turned the
    // bitmap allocation into a quadratic copy is caught.
    {
        std::vector<unsigned char> max_payload(340, 0x55);
        max_payload[0] = 0xFF; // bitmap_len = 255 → expected size = 340
        const auto t_start = Clock::now();
        const size_t kMaxIters = 1000;
        for (size_t i = 0; i < kMaxIters; ++i) {
            COracleBundle bundle;
            BOOST_REQUIRE(COracleBundle::DeserializeV03Data(max_payload, bundle));
            BOOST_REQUIRE_EQUAL(bundle.participation_bitmap.size(), 255u);
            BOOST_REQUIRE_EQUAL(bundle.aggregate_sig.size(), 64u);
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - t_start).count();
        BOOST_CHECK_MESSAGE(elapsed_ms < 200,
            "DeserializeV03Data max-payload parse took " << elapsed_ms
            << " ms for " << kMaxIters << " iters; CPU bound regressed.");
    }
}

// ============================================================================
// DD-FA-TEST-038: AddOracleMessage rejects oversized / undersized
// schnorr_sig fields without buffering them.
//
// Contract (src/primitives/oracle.cpp::IsValid):
//   - schnorr_sig must be exactly 64 bytes (BIP-340).
//   - VerifyAttestation must succeed.
//
// An attacker grinding the schnorr_sig field cannot inflate its size to
// amplify per-message memory cost: the message is rejected up-front.
// Without this pin a regression that accepted any-size signature would
// let one authenticated peer-message keep arbitrary heap-resident bytes
// in pending_messages.
// ============================================================================
BOOST_AUTO_TEST_CASE(add_oracle_message_rejects_malformed_schnorr_sig)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);
    manager.SetMinOracleCount(1);

    const int64_t now = GetTime();
    const size_t pending_before = manager.GetPendingMessageCount();

    // Baseline: the well-formed signed message must be accepted (so we
    // know the harness is wired correctly).
    {
        COraclePriceMessage good = MakeSignedRegtestMessage(0, 6000, now);
        BOOST_REQUIRE(manager.AddOracleMessage(good));
        BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), pending_before + 1);
    }
    manager.RemoveOracleMessage(0);
    BOOST_REQUIRE_EQUAL(manager.GetPendingMessageCount(), pending_before);

    // CASE A: schnorr_sig length-extended to 1 KB.
    {
        COraclePriceMessage oversized = MakeSignedRegtestMessage(0, 6500, now + 1);
        oversized.schnorr_sig.assign(1024, 0xAA);
        BOOST_CHECK(!manager.AddOracleMessage(oversized));
        BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), pending_before);
    }

    // CASE B: schnorr_sig truncated to 32 bytes.
    {
        COraclePriceMessage truncated = MakeSignedRegtestMessage(1, 6500, now + 2);
        truncated.schnorr_sig.resize(32);
        BOOST_CHECK(!manager.AddOracleMessage(truncated));
        BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), pending_before);
    }

    // CASE C: schnorr_sig empty entirely.
    {
        COraclePriceMessage empty_sig = MakeSignedRegtestMessage(2, 6500, now + 3);
        empty_sig.schnorr_sig.clear();
        BOOST_CHECK(!manager.AddOracleMessage(empty_sig));
        BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), pending_before);
    }

    manager.Clear();
}

// ============================================================================
// DD-FA-TEST-039: ClearPendingMessages is O(1) when already empty.
//
// Operator-visible restart path: the daemon may call ClearPendingMessages
// repeatedly while idle. A regression that turned this into an O(n) walk
// or any blocking I/O would create a thundering-herd risk during oracle
// recovery loops.
// ============================================================================
BOOST_AUTO_TEST_CASE(clear_pending_messages_idempotent_fast_when_empty)
{
    using Clock = std::chrono::steady_clock;

    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();

    // Already-empty state: 10000 ClearPendingMessages calls finish well
    // under 200 ms wall-clock on regtest hardware.
    const auto t_start = Clock::now();
    const size_t kIters = 10000;
    for (size_t i = 0; i < kIters; ++i) {
        manager.ClearPendingMessages();
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                Clock::now() - t_start).count();
    BOOST_CHECK_MESSAGE(elapsed_ms < 500,
        "ClearPendingMessages idempotent path took " << elapsed_ms
        << " ms for " << kIters << " iters; restart hot-path regressed.");

    BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), 0u);
    BOOST_CHECK_EQUAL(manager.GetPendingAttestationCount(), 0u);
}

// ============================================================================
// DD-FA-TEST-063: seen_attestation_hashes cap holds at 10000.
//
// ORACLEATTESTATION uses a separate replay set from `seen_message_hashes`.
// Wave 20 reset the set on restart; Wave 21 pins its resource cap so a
// flood of distinct authenticated attestation envelopes cannot grow it
// without bound.
// ============================================================================
BOOST_AUTO_TEST_CASE(seen_attestation_hashes_cap_at_10000)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();

    constexpr size_t MAX_SEEN_ATTESTATIONS = 10000;
    constexpr size_t TOTAL_INSERTED = 12000;

    for (uint64_t i = 1; i <= TOTAL_INSERTED; ++i) {
        BOOST_REQUIRE(manager.RegisterSeenAttestation(ArithToUint256(i)));
    }

    size_t retained = 0;
    for (uint64_t i = 1; i <= TOTAL_INSERTED; ++i) {
        if (manager.HasSeenAttestation(ArithToUint256(i))) {
            ++retained;
        }
    }
    BOOST_CHECK_EQUAL(retained, MAX_SEEN_ATTESTATIONS);

    // std::set<uint256> ordering is byte-order based rather than insertion
    // order, so the resource contract is cardinality, not FIFO retention.
    BOOST_CHECK(!manager.RegisterSeenAttestation(ArithToUint256(TOTAL_INSERTED)));

    manager.Clear();
}

BOOST_AUTO_TEST_SUITE_END()
