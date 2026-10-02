// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <addrman.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <crypto/sha256.h>
#include <key.h>
#include <netgroup.h>
#include <oracle/bundle_manager.h>
#include <oracle/node.h>
#include <primitives/block.h>
#include <protocol.h>
#include <span.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <validationinterface.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

/**
 * The order the node shuts the oracle objects down in.
 *
 * Shutdown stops the oracle price threads early, because those threads fetch
 * prices from exchanges and hand messages to the connection manager. The
 * oracle objects themselves have to live longer than that. Block
 * notifications, the remote procedure call server and the peer-to-peer
 * message handler all look oracles up through the oracle manager, and they
 * stop later in shutdown.
 *
 * Unregistering a block notification does not wait for a notification that is
 * already running. These tests hold a notification inside its callback, run
 * the early part of shutdown while it is held, and then check the callback
 * still has the oracle it was working with.
 */

namespace {

CKey ShutdownTestOracleKey(uint32_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    CKey key;
    key.Set(hash.begin(), hash.end(), true);
    return key;
}

std::string OracleKeyHex(const CKey& key)
{
    return HexStr(Span<const unsigned char>(key.begin(), key.end()));
}

/** A one-shot signal from one thread to another. */
class TestSignal
{
public:
    void Set()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_set = true;
        }
        m_cv.notify_all();
    }

    //! Wait for the signal. Returns false if it never arrived.
    bool Wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::seconds(30), [this] { return m_set; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_set{false};
};

/**
 * Wait for an oracle to send one more heartbeat, for up to two seconds.
 *
 * Every heartbeat carries a fresh random number, so a changed number means the
 * price thread ran again. This is only called when a price thread is still
 * running at a point in shutdown where there should be none. It gives that
 * thread the time to be caught using the connection manager that has already
 * been destroyed, instead of the test finishing first and hiding it.
 */
void WaitForAnotherHeartbeat(uint32_t oracle_id)
{
    OracleVersionHeartbeatMsg before;
    const bool had_one = OracleBundleManager::GetInstance().GetVersionHeartbeat(oracle_id, before);
    for (int attempt = 0; attempt < 200; ++attempt) {
        OracleVersionHeartbeatMsg latest;
        if (OracleBundleManager::GetInstance().GetVersionHeartbeat(oracle_id, latest) &&
            (!had_one || latest.nonce != before.nonce)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/**
 * A block notification that stops part way through, so the test can run the
 * early part of shutdown while the notification is still running.
 *
 * The body copies what the signing orchestrator does on every connected
 * block: it asks the oracle manager for an oracle, and then uses the oracle it
 * was given.
 */
class HeldOracleNotification final : public CValidationInterface
{
public:
    explicit HeldOracleNotification(uint32_t oracle_id) : m_oracle_id(oracle_id) {}

    //! Wait for the callback to start. Returns false if it never started.
    bool WaitUntilRunning()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_running_cv.wait_for(lock, std::chrono::seconds(30), [this] { return m_running; });
    }

    void LetItFinish()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_release = true;
        }
        m_release_cv.notify_all();
    }

    OracleNode* OracleWhenItStarted() const { return m_oracle_at_start; }
    OracleNode* OracleWhenItFinished() const { return m_oracle_at_end; }
    uint32_t IdReadFromHeldOracle() const { return m_id_from_held_oracle; }
    bool KeyReadFromHeldOracleIsValid() const { return m_key_from_held_oracle_valid; }

protected:
    void BlockChecked(const CBlock&, const BlockValidationState&) override
    {
        m_oracle_at_start = OracleManager::GetInstance().GetOracleNode(m_oracle_id);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running = true;
        }
        m_running_cv.notify_all();

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_release_cv.wait_for(lock, std::chrono::seconds(30), [this] { return m_release; });
        }

        // Use the oracle this callback already has. A signing callback asks for
        // an oracle and then reads its key, so this reads freed memory if the
        // oracle was destroyed while the callback was held here.
        if (m_oracle_at_start != nullptr) {
            m_id_from_held_oracle = m_oracle_at_start->GetOracleId();
            m_key_from_held_oracle_valid = m_oracle_at_start->GetOraclePrivateKey().IsValid();
        }

        // Ask for it again, the way a later step of the same callback does.
        m_oracle_at_end = OracleManager::GetInstance().GetOracleNode(m_oracle_id);
    }

private:
    const uint32_t m_oracle_id;
    std::mutex m_mutex;
    std::condition_variable m_running_cv;
    std::condition_variable m_release_cv;
    bool m_running{false};
    bool m_release{false};
    OracleNode* m_oracle_at_start{nullptr};
    OracleNode* m_oracle_at_end{nullptr};
    uint32_t m_id_from_held_oracle{0xffffffff};
    bool m_key_from_held_oracle_valid{false};
};

struct OracleShutdownOrderSetup : public ChainTestingSetup {
    OracleShutdownOrderSetup() : ChainTestingSetup(ChainType::REGTEST)
    {
        // Other suites share the process-wide oracle manager. Start clean.
        OracleManager::StopOracleService();
    }

    ~OracleShutdownOrderSetup()
    {
        OracleManager::StopOracleService();
        // One test below gives the bundle manager a connection manager of its
        // own. Nothing must be left pointing at it once that test is over.
        OracleBundleManager::GetInstance().SetConnman(nullptr);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(oracle_shutdown_order_tests, OracleShutdownOrderSetup)

BOOST_AUTO_TEST_CASE(oracle_outlives_a_running_block_notification)
{
    static constexpr uint32_t oracle_id{0};
    const CKey oracle_key = ShutdownTestOracleKey(oracle_id);
    BOOST_REQUIRE(oracle_key.IsValid());

    OracleManager& manager = OracleManager::GetInstance();
    BOOST_REQUIRE(manager.AddOracleNode(oracle_id, OracleKeyHex(oracle_key)));
    OracleNode* const oracle = manager.GetOracleNode(oracle_id);
    BOOST_REQUIRE(oracle != nullptr);

    HeldOracleNotification held(oracle_id);
    RegisterValidationInterface(&held);

    std::thread notifier([] {
        const CBlock block;
        const BlockValidationState state;
        GetMainSignals().BlockChecked(block, state);
    });

    const bool callback_started = held.WaitUntilRunning();

    // From here to LetItFinish() is the window shutdown runs in, in the same
    // order shutdown uses it. Unregistering stops new notifications; the one
    // running right now keeps running.
    UnregisterValidationInterface(&held);
    OracleManager::StopOraclePriceThreads();

    held.LetItFinish();
    notifier.join();
    UnregisterValidationInterface(&held);

    BOOST_REQUIRE(callback_started);
    BOOST_CHECK(held.OracleWhenItStarted() == oracle);
    BOOST_CHECK_MESSAGE(held.OracleWhenItFinished() == oracle,
                        "the oracle disappeared while a block notification was still running");
    BOOST_CHECK_EQUAL(held.IdReadFromHeldOracle(), oracle_id);
    BOOST_CHECK(held.KeyReadFromHeldOracleIsValid());

    // The rest of shutdown. Nothing can look an oracle up any more, so the
    // objects can go now.
    OracleManager::StopOracleService();
    BOOST_CHECK(g_oracle_manager == nullptr);
}

BOOST_AUTO_TEST_CASE(stopping_the_price_threads_keeps_the_oracles)
{
    static constexpr uint32_t oracle_id{1};
    const CKey oracle_key = ShutdownTestOracleKey(oracle_id);
    BOOST_REQUIRE(oracle_key.IsValid());

    OracleManager& manager = OracleManager::GetInstance();
    BOOST_REQUIRE(manager.AddOracleNode(oracle_id, OracleKeyHex(oracle_key)));
    OracleNode* const oracle = manager.GetOracleNode(oracle_id);
    BOOST_REQUIRE(oracle != nullptr);

    // The early step of shutdown: the price threads stop, the oracles stay.
    OracleManager::StopOraclePriceThreads();

    BOOST_REQUIRE(g_oracle_manager != nullptr);
    BOOST_CHECK(OracleManager::GetInstance().GetOracleNode(oracle_id) == oracle);
    BOOST_CHECK_EQUAL(oracle->GetOracleId(), oracle_id);
    BOOST_CHECK(!OracleManager::GetInstance().IsOracleRunning(oracle_id));
    BOOST_CHECK_EQUAL(OracleManager::GetInstance().GetActiveOracleCount(), size_t{0});

    // The late step of shutdown: nothing can look an oracle up any more, so
    // now the oracles and the manager go.
    OracleManager::StopOracleService();
    BOOST_CHECK(g_oracle_manager == nullptr);
}

/**
 * A start that arrives while the node is shutting down must be refused.
 *
 * Shutdown stops the oracle price threads early, because those threads reach
 * exchanges over the network and hand messages to the connection manager. The
 * command that starts an oracle runs on a different thread, and the command
 * server is still serving requests at that point in shutdown. So a start can
 * be part way through when the price threads are stopped, and finish
 * afterwards. Nothing may be running once the threads have been stopped.
 */
BOOST_AUTO_TEST_CASE(a_start_arriving_while_shutting_down_is_refused)
{
    static constexpr uint32_t running_oracle_id{2};
    static constexpr uint32_t late_oracle_id{3};

    OracleManager& manager = OracleManager::GetInstance();
    BOOST_REQUIRE(manager.AddOracleNode(running_oracle_id, OracleKeyHex(ShutdownTestOracleKey(running_oracle_id))));
    BOOST_REQUIRE(manager.AddOracleNode(late_oracle_id, OracleKeyHex(ShutdownTestOracleKey(late_oracle_id))));

    OracleNode* const worker = manager.GetOracleNode(running_oracle_id);
    OracleNode* const late = manager.GetOracleNode(late_oracle_id);
    BOOST_REQUIRE(worker != nullptr);
    BOOST_REQUIRE(late != nullptr);

    // A test must not ask a real exchange for a price. Everything else about
    // these price threads is real.
    worker->SetSkipExchangeFetchForTesting(true);
    late->SetSkipExchangeFetchForTesting(true);

    // A price thread is running when shutdown begins.
    worker->Start();
    BOOST_REQUIRE_MESSAGE(worker->IsRunning(), "this test needs a price thread running before shutdown begins");

    // A start that has finished its checks and is about to start the oracle. It
    // waits here so that shutdown goes first.
    TestSignal start_is_in_flight;
    TestSignal price_threads_have_been_stopped;
    std::thread starter([&] {
        start_is_in_flight.Set();
        price_threads_have_been_stopped.Wait();
        late->Start();
    });

    const bool start_was_in_flight = start_is_in_flight.Wait();

    // The early step of shutdown, with a start in flight.
    OracleManager::StopOraclePriceThreads();

    price_threads_have_been_stopped.Set();
    starter.join();

    BOOST_REQUIRE(start_was_in_flight);
    BOOST_CHECK_MESSAGE(!manager.IsOracleRunning(running_oracle_id),
                        "the price thread that was already running did not stop");
    BOOST_CHECK_MESSAGE(!manager.IsOracleRunning(late_oracle_id),
                        "an oracle started a price thread after shutdown had stopped the price threads");
    BOOST_CHECK_EQUAL(manager.GetActiveOracleCount(), size_t{0});

    // The rest of shutdown.
    OracleManager::StopOracleService();
    BOOST_CHECK(g_oracle_manager == nullptr);
}

/**
 * No price thread may still be running when networking is destroyed.
 *
 * A price thread hands messages to the connection manager. Shutdown destroys
 * the connection manager a few steps after it stops the price threads, so a
 * price thread that is still running by then reads memory that has been freed.
 * This test holds a start in flight over the stop, the way the command server
 * can, and then destroys the connection manager the way shutdown does.
 */
BOOST_AUTO_TEST_CASE(no_price_thread_outlives_the_connection_manager)
{
    static constexpr uint32_t running_oracle_id{4};
    static constexpr uint32_t late_oracle_id{5};

    // The connection manager the price threads send their messages to. It is
    // created here so this test controls exactly when it goes away.
    NetGroupManager netgroupman{std::vector<bool>()};
    AddrMan addrman{netgroupman, /*deterministic=*/true, /*consistency_check_ratio=*/0};
    auto connman = std::make_unique<ConnmanTestMsg>(0x1337, 0x1337, addrman, netgroupman, Params());
    OracleBundleManager::GetInstance().SetConnman(connman.get());

    OracleManager& manager = OracleManager::GetInstance();
    BOOST_REQUIRE(manager.AddOracleNode(running_oracle_id, OracleKeyHex(ShutdownTestOracleKey(running_oracle_id))));
    BOOST_REQUIRE(manager.AddOracleNode(late_oracle_id, OracleKeyHex(ShutdownTestOracleKey(late_oracle_id))));

    OracleNode* const worker = manager.GetOracleNode(running_oracle_id);
    OracleNode* const late = manager.GetOracleNode(late_oracle_id);
    BOOST_REQUIRE(worker != nullptr);
    BOOST_REQUIRE(late != nullptr);

    for (OracleNode* oracle : {worker, late}) {
        oracle->SetSkipExchangeFetchForTesting(true);
        // Send a heartbeat on every pass of the loop, and do not wait between
        // passes. The thread then reaches the connection manager constantly,
        // which is what makes a thread that should not exist visible.
        oracle->SetHeartbeatInterval(0);
        oracle->SetUpdateInterval(0);
    }

    worker->Start();
    BOOST_REQUIRE_MESSAGE(worker->IsRunning(), "this test needs a price thread running before shutdown begins");

    TestSignal start_is_in_flight;
    TestSignal price_threads_have_been_stopped;
    std::thread starter([&] {
        start_is_in_flight.Set();
        price_threads_have_been_stopped.Wait();
        late->Start();
    });

    const bool start_was_in_flight = start_is_in_flight.Wait();

    // The early step of shutdown, with a start in flight.
    OracleManager::StopOraclePriceThreads();

    price_threads_have_been_stopped.Set();
    starter.join();

    // Nothing should be running here. If something is, let it use the
    // connection manager once, then take the connection manager away the way
    // shutdown does, then let it run again. That second run is the fault: a
    // sanitizer build reports it reading freed memory.
    const bool still_running = manager.IsOracleRunning(late_oracle_id) ||
                               manager.IsOracleRunning(running_oracle_id);
    if (still_running) WaitForAnotherHeartbeat(late_oracle_id);

    // What shutdown does next: networking goes away.
    connman.reset();

    if (still_running) WaitForAnotherHeartbeat(late_oracle_id);

    BOOST_REQUIRE(start_was_in_flight);
    BOOST_CHECK_MESSAGE(!still_running,
                        "an oracle price thread was still running when shutdown destroyed the connection manager");

    OracleBundleManager::GetInstance().SetConnman(nullptr);
    OracleManager::StopOracleService();
    BOOST_CHECK(g_oracle_manager == nullptr);
}

/**
 * Adding an oracle whose key cannot be used changes nothing, and the manager
 * still works afterwards.
 *
 * Adding one used to hold the manager's lock across building the oracle. Building
 * it can fail, a failed oracle is destroyed on the spot, destroying one stops it,
 * and stopping takes the start-and-stop lock. That made the manager's lock be held
 * while the start-and-stop lock was taken, which is the opposite order from a stop
 * that is waiting for a price thread, because such a thread needs the manager's
 * lock before it can finish. Building now happens with the manager's lock
 * released.
 *
 * This case covers the behaviour of that rewrite: a refused key leaves the manager
 * exactly as it was. The lock order itself is not what this case proves; see the
 * note at the end of this file.
 */
BOOST_AUTO_TEST_CASE(adding_an_oracle_whose_key_is_bad_changes_nothing)
{
    // The ids have to be ones this network actually configures. Regtest configures
    // 0 to 6. An oracle outside that range can be added and can never be started:
    // ValidateOracleId is skipped on regtest but ValidateOracleKey is not, and it
    // needs a configured public key to compare against. The fixture destroys the
    // manager before and after every case, so the same ids are free to reuse here.
    static constexpr uint32_t good_oracle_id{0};
    static constexpr uint32_t bad_oracle_id{1};
    static constexpr uint32_t second_good_oracle_id{2};

    OracleManager& manager = OracleManager::GetInstance();
    BOOST_REQUIRE(manager.AddOracleNode(good_oracle_id, OracleKeyHex(ShutdownTestOracleKey(good_oracle_id))));
    BOOST_REQUIRE(manager.GetOracleNode(good_oracle_id) != nullptr);

    // A key that cannot be read at all, so building the oracle fails and the
    // half-built oracle is destroyed inside the call.
    BOOST_CHECK(!manager.AddOracleNode(bad_oracle_id, "not a private key"));

    BOOST_CHECK_MESSAGE(manager.GetOracleNode(bad_oracle_id) == nullptr,
                        "an oracle that failed to be built was kept anyway");
    BOOST_CHECK_MESSAGE(manager.GetOracleNode(good_oracle_id) != nullptr,
                        "a refused add disturbed an oracle that was already there");

    // The manager still works: another good oracle can be added after the refusal.
    BOOST_CHECK(manager.AddOracleNode(second_good_oracle_id,
                                      OracleKeyHex(ShutdownTestOracleKey(second_good_oracle_id))));
    BOOST_CHECK(manager.GetOracleNode(second_good_oracle_id) != nullptr);

    // Adding the same oracle twice is still refused. That check now runs twice,
    // once before the oracle is built and once before it is kept, because the lock
    // is released in between.
    BOOST_CHECK(!manager.AddOracleNode(good_oracle_id, OracleKeyHex(ShutdownTestOracleKey(good_oracle_id))));
    BOOST_CHECK_MESSAGE(manager.GetOracleNode(good_oracle_id) != nullptr,
                        "a refused duplicate add removed the oracle that was already there");
    BOOST_CHECK_MESSAGE(manager.GetOracleNode(second_good_oracle_id) != nullptr,
                        "a refused duplicate add disturbed another oracle");

    OracleManager::StopOracleService();
    BOOST_CHECK(g_oracle_manager == nullptr);
}

/**
 * Removing an oracle takes it out of the manager and stops its price thread.
 *
 * Removing one used to erase it from the manager while holding the manager's lock,
 * and erasing it destroyed it, which stopped it, which took the start-and-stop
 * lock. The oracle is now moved out under the lock and destroyed after the lock is
 * released. This case covers the behaviour of that rewrite.
 */
BOOST_AUTO_TEST_CASE(removing_an_oracle_takes_it_out_and_stops_it)
{
    // Ids this network configures, and ones the cases above already start
    // successfully. An unconfigured id cannot be started at all.
    static constexpr uint32_t oracle_id{0};
    static constexpr uint32_t other_oracle_id{1};

    OracleManager& manager = OracleManager::GetInstance();
    BOOST_REQUIRE(manager.AddOracleNode(oracle_id, OracleKeyHex(ShutdownTestOracleKey(oracle_id))));
    BOOST_REQUIRE(manager.AddOracleNode(other_oracle_id, OracleKeyHex(ShutdownTestOracleKey(other_oracle_id))));

    OracleNode* const oracle = manager.GetOracleNode(oracle_id);
    BOOST_REQUIRE(oracle != nullptr);

    // A test must not ask a real exchange for a price. Everything else about this
    // price thread is real, and it runs without pausing so that it is genuinely
    // busy when the remove happens.
    oracle->SetSkipExchangeFetchForTesting(true);
    oracle->SetHeartbeatInterval(0);
    oracle->SetUpdateInterval(0);
    oracle->Start();
    BOOST_REQUIRE_MESSAGE(oracle->IsRunning(), "this case needs a running price thread to remove");

    // Only this one was started, so it is the only one counted as active.
    BOOST_REQUIRE_EQUAL(manager.GetActiveOracleCount(), size_t{1});

    BOOST_CHECK(manager.RemoveOracleNode(oracle_id));

    BOOST_CHECK_MESSAGE(manager.GetOracleNode(oracle_id) == nullptr, "the removed oracle is still in the manager");
    BOOST_CHECK_MESSAGE(!manager.IsOracleRunning(oracle_id), "the removed oracle still reports a running price thread");
    BOOST_CHECK_EQUAL(manager.GetActiveOracleCount(), size_t{0});

    // The other oracle is untouched, and removing something that is not there is
    // refused rather than treated as a success.
    BOOST_CHECK_MESSAGE(manager.GetOracleNode(other_oracle_id) != nullptr, "removing one oracle disturbed another");
    BOOST_CHECK(!manager.RemoveOracleNode(oracle_id));

    OracleManager::StopOracleService();
    BOOST_CHECK(g_oracle_manager == nullptr);
}

/**
 * What these two cases do not prove.
 *
 * They prove the behaviour of the two rewritten functions. They do not prove the
 * lock order, because proving that needs the test to hold either the manager's
 * lock or the start-and-stop lock while the other is taken, and neither is
 * reachable from a test without adding a hook to the shipped code. A version that
 * raced the two threads and hoped to catch it would hang this runner instead of
 * failing it, which is not acceptable in a release suite. The lock order rests on
 * reading every place an oracle is destroyed, which is written down in the
 * round-five record.
 */

BOOST_AUTO_TEST_SUITE_END()
