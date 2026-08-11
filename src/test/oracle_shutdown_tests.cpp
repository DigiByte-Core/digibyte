// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Shutdown-ordering test for the MuSig2 signing orchestrator.
//
// OracleSigningOrchestrator is a validation interface subscriber, and its
// Shutdown() destroys the object. UnregisterValidationInterface() is
// non-blocking and can return while a notification is still in flight, so
// destroying without draining the queue lets a queued BlockConnected callback
// run on freed memory (OnBlockConnected -> CleanupOldSessions locks
// m_sessions_mutex), which deadlocks the scheduler thread on glibc and aborts
// on libc++.
//
// The use-after-free itself is a microsecond-wide race and cannot be asserted
// on directly, so this pins the contract that prevents it: Shutdown() must not
// return while a queued notification is still pending.

#include <oracle/signing_orchestrator.h>
#include <util/time.h>
#include <validationinterface.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>

BOOST_FIXTURE_TEST_SUITE(oracle_shutdown_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(shutdown_waits_for_in_flight_validation_callbacks)
{
    if (!g_signing_orchestrator) {
        OracleSigningOrchestrator::Initialize();
    }
    BOOST_REQUIRE(g_signing_orchestrator);

    // Occupy the validation interface queue with work that outlives a
    // non-draining Shutdown(). A queued orchestrator notification behaves the
    // same way; this stands in for one without depending on chain activity.
    std::atomic<bool> callback_finished{false};
    CallFunctionInValidationInterfaceQueue([&callback_finished] {
        UninterruptibleSleep(std::chrono::milliseconds{250});
        callback_finished = true;
    });

    OracleSigningOrchestrator::Shutdown();

    // Without the drain, Shutdown() returns while the callback is still
    // pending -- and the next queued notification would touch an orchestrator
    // that no longer exists.
    BOOST_CHECK(callback_finished.load());
}

BOOST_AUTO_TEST_SUITE_END()
