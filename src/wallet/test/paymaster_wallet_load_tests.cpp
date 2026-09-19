// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <hash.h>
#include <key.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/provider.h>
#include <paymaster/reputation.h>
#include <paymaster/reservation.h>
#include <scheduler.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <wallet/context.h>
#include <wallet/load.h>
#include <wallet/paymasterstore.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <thread>
#include <utility>

namespace wallet {

BOOST_AUTO_TEST_SUITE(paymaster_wallet_load_tests)

namespace {
class ScopedWalletLoadMockTime
{
public:
    explicit ScopedWalletLoadMockTime(int64_t now)
        : m_previous{GetMockTime()}
    {
        SetMockTime(now);
    }

    ~ScopedWalletLoadMockTime() { SetMockTime(m_previous); }

private:
    const std::chrono::seconds m_previous;
};

template <typename T>
std::vector<unsigned char> SerializeWalletLoadPaymasterObject(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

DigiDollar::Paymaster::PaymasterEquivocationEvidence
MakeWalletLoadEquivocationEvidence(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::EquivocationKind kind,
    const uint256& semantic_key,
    std::vector<unsigned char> first_artifact,
    std::vector<unsigned char> second_artifact,
    int64_t observed_at)
{
    using namespace DigiDollar::Paymaster;
    PaymasterEquivocationEvidence evidence;
    evidence.kind = kind;
    evidence.provider_id = provider_id;
    evidence.semantic_key = semantic_key;
    evidence.first_artifact_hash = Hash(first_artifact);
    evidence.second_artifact_hash = Hash(second_artifact);
    evidence.first_artifact = std::move(first_artifact);
    evidence.second_artifact = std::move(second_artifact);
    if (evidence.second_artifact_hash < evidence.first_artifact_hash) {
        std::swap(evidence.first_artifact_hash,
                  evidence.second_artifact_hash);
        std::swap(evidence.first_artifact, evidence.second_artifact);
    }
    evidence.observed_at = observed_at;
    evidence.evidence_id = GetEquivocationEvidenceId(
        kind, provider_id, semantic_key, evidence.first_artifact_hash,
        evidence.second_artifact_hash);
    return evidence;
}
} // namespace

BOOST_FIXTURE_TEST_CASE(periodic_paymaster_maintenance_expires_unsigned_quotes_atomically,
                        TestingSetup)
{
    using namespace DigiDollar::Paymaster;

    constexpr int64_t expires_at{100};
    constexpr int64_t maintenance_time{101};
    const std::string request_id{"550e8400-e29b-41d4-a716-446655449900"};
    const uint256 session_id{uint256S("11")};
    const uint256 attempt_id{uint256S("12")};
    const uint256 commit_key{uint256S("13")};

    auto wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-maintenance",
        CreateMockableWalletDatabase());

    PaymentSession session;
    session.request_id = request_id;
    session.session_id = session_id;
    session.canonical_request_hash = uint256S("14");
    session.state = SessionState::INPUTS_RESERVED;
    session.attempt_ids = {attempt_id};
    session.created_at = 90;
    session.updated_at = 90;
    session.provider_side = true;

    ProviderAttempt attempt;
    attempt.session_id = session_id;
    attempt.attempt_id = attempt_id;
    attempt.provider_id = uint256S("15");
    attempt.state = AttemptState::QUOTED;
    attempt.commit_key = commit_key;
    attempt.quote_expires_at = expires_at;
    attempt.created_at = 90;
    attempt.updated_at = 90;

    CKey pool_key;
    pool_key.MakeNewKey(true);
    TaprootBuilder builder;
    builder.Finalize(XOnlyPubKey{pool_key.GetPubKey()});
    ProviderPoolEntry pool_entry;
    pool_entry.outpoint = COutPoint{uint256S("16"), 0};
    pool_entry.purpose = PoolPurpose::OPERATIONAL;
    pool_entry.asset = PoolAsset::DGB;
    pool_entry.state = PoolEntryState::RESERVED;
    pool_entry.script_pub_key = GetScriptForDestination(builder.GetOutput());
    pool_entry.dgb_value = DGBSatoshis{1};
    pool_entry.reservation_id = commit_key;
    pool_entry.updated_at = 90;

    {
        WalletBatch batch{wallet->GetDatabase()};
        BOOST_REQUIRE(batch.TxnBegin());
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterSessionId(session_id, request_id));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({pool_entry}));
        BOOST_REQUIRE(batch.TxnCommit());
    }

    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    BOOST_REQUIRE(AddWallet(context, wallet));
    ScopedWalletLoadMockTime mock_time{maintenance_time};

    auto& database = GetMockableDatabase(*wallet);
    const MockableData before_failure{database.m_records};
    database.FailWriteAt(0);
    RunPeriodicPaymasterMaintenance(context, maintenance_time);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_failure);

    CScheduler scheduler;
    SchedulePeriodicPaymasterMaintenance(context, scheduler);
    std::thread scheduler_thread{[&scheduler] { scheduler.serviceQueue(); }};
    scheduler.MockForward(std::chrono::hours{1});
    scheduler.scheduleFromNow([&scheduler] { scheduler.stop(); },
                              std::chrono::milliseconds{1});
    scheduler_thread.join();

    PaymentSession expired_session;
    ProviderAttempt expired_attempt;
    std::vector<ProviderPoolEntry> released_pool;
    {
        WalletBatch batch{wallet->GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterSession(request_id, expired_session));
        BOOST_REQUIRE(batch.ReadPaymasterAttempt(attempt_id, expired_attempt));
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(released_pool));
    }
    BOOST_CHECK(expired_session.state == SessionState::FAILED);
    BOOST_CHECK(expired_attempt.state == AttemptState::QUOTE_EXPIRED);
    BOOST_REQUIRE_EQUAL(released_pool.size(), 1U);
    BOOST_CHECK(released_pool.front().state == PoolEntryState::AVAILABLE);
    BOOST_CHECK(released_pool.front().reservation_id.IsNull());
    BOOST_CHECK_EQUAL(released_pool.front().updated_at, maintenance_time);

    const MockableData before_retry{database.m_records};
    RunPeriodicPaymasterMaintenance(context, maintenance_time + 1);
    BOOST_CHECK(database.m_records == before_retry);

    BOOST_REQUIRE(RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt));
}

BOOST_FIXTURE_TEST_CASE(periodic_paymaster_maintenance_persists_prebaseline_capacity_equivocation,
                        TestingSetup)
{
    using namespace DigiDollar::Paymaster;

    constexpr int64_t request_created_at{101};
    constexpr int64_t proof_created_at{102};
    constexpr int64_t baseline_received_at{103};
    constexpr int64_t conflict_received_at{104};
    constexpr int64_t maintenance_time{
        conflict_received_at + MAX_DIRECT_MESSAGE_TTL_SECONDS};
    constexpr int64_t expires_at{
        request_created_at + MAX_DIRECT_MESSAGE_TTL_SECONDS - 1};
    const std::string request_id{"550e8400-e29b-41d4-a716-446655449901"};
    const uint256 attempt_id{uint256S("21")};

    auto wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-equivocation-maintenance",
        CreateMockableWalletDatabase());
    PaymasterStore store{*wallet};
    PaymentSession session;
    std::string error;
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256S("22"), FeeMode::PAYMASTER, 100,
                      session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    const XOnlyPubKey provider_identity{provider_key.GetPubKey()};
    const PaymasterId provider_id{GetPaymasterId(provider_identity)};
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});

    PaymentIntent intent;
    intent.genesis_hash = uint256S("23");
    intent.provider_id = provider_id;
    intent.request_id = request_id;
    intent.session_id = session.session_id;
    intent.client_nonce = uint256S("24");
    intent.canonical_request_hash = session.canonical_request_hash;
    intent.requested_fee_mode = session.fee_mode_requested;
    intent.user_dd_inputs = {COutPoint{uint256S("25"), 0}};
    intent.recipient_script =
        GetScriptForDestination(recipient_builder.GetOutput());
    intent.recipient_amount = DDCents{100};
    intent.offer_id = uint256S("26");
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = uint256S("27");
    intent.expires_at = expires_at;

    ProviderAttempt attempt;
    attempt.attempt_id = attempt_id;
    attempt.provider_id = provider_id;
    attempt.provider_identity_key = provider_identity;
    attempt.client_nonce = intent.client_nonce;
    attempt.unsigned_intent = SerializeWalletLoadPaymasterObject(intent);
    attempt.created_at = attempt.updated_at = 100;
    BOOST_REQUIRE_MESSAGE(
        store.PreparePaymentIntent(request_id, attempt, 100, error), error);

    PaymasterCapacityRequest request;
    request.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    request.genesis_hash = intent.genesis_hash;
    request.provider_id = provider_id;
    request.request_id = request_id;
    request.session_id = session.session_id;
    request.client_nonce = intent.client_nonce;
    request.funding_model = intent.funding_model;
    request.requires_carrier = false;
    request.requested_slots = 1;
    request.created_at = request_created_at;
    request.expires_at = expires_at;
    const std::vector<unsigned char> request_bytes =
        SerializeWalletLoadPaymasterObject(request);
    BOOST_REQUIRE_MESSAGE(store.PrepareCapacityRequest(
                              request_id, attempt_id, request_bytes,
                              request_created_at, error),
                          error);

    CMutableTransaction capacity_funding;
    capacity_funding.vin.emplace_back(COutPoint{uint256S("28"), 0});
    capacity_funding.vout.emplace_back(1000, CScript{} << OP_TRUE);
    const auto make_proof = [&](const uint256& snapshot_id) {
        PaymasterCapacityProof proof;
        proof.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
        proof.genesis_hash = request.genesis_hash;
        proof.provider_id = request.provider_id;
        proof.request_id = request.request_id;
        proof.session_id = request.session_id;
        proof.client_nonce = request.client_nonce;
        proof.funding_model = request.funding_model;
        proof.requires_carrier = request.requires_carrier;
        proof.snapshot_id = snapshot_id;
        proof.created_at = proof_created_at;
        proof.expires_at = expires_at;
        CapacityDGBInput dgb;
        dgb.input.outpoint =
            COutPoint{CTransaction{capacity_funding}.GetHash(), 0};
        dgb.input.creating_tx = capacity_funding;
        dgb.input.value = DGBSatoshis{1000};
        dgb.control_proof.reference_block = uint256S("29");
        dgb.control_proof.expires_at = proof.expires_at;
        dgb.control_proof.signature.assign(64, 1);
        PaymasterLiquiditySlot slot;
        slot.dgb_inputs = {dgb};
        proof.liquidity_slots = {slot};
        proof.identity_signature.resize(64);
        BOOST_REQUIRE(provider_key.SignSchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature,
            nullptr, uint256{}));
        return proof;
    };
    const PaymasterCapacityProof baseline = make_proof(uint256S("2a"));
    const PaymasterCapacityProof conflict = make_proof(uint256S("2b"));

    const std::vector<unsigned char> baseline_bytes =
        SerializeWalletLoadPaymasterObject(baseline);
    const std::vector<unsigned char> conflict_bytes =
        SerializeWalletLoadPaymasterObject(conflict);
    Manager manager{/*enabled=*/true};
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, Hash(baseline_bytes), baseline_bytes.size(), baseline,
                    baseline_received_at) == DirectEnqueueResult::ACCEPTED);
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, Hash(conflict_bytes), conflict_bytes.size(), conflict,
                    conflict_received_at) == DirectEnqueueResult::CONFLICT);

    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    context.paymaster = &manager;
    auto unrelated_wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-equivocation-unrelated",
        CreateMockableWalletDatabase());
    BOOST_REQUIRE(AddWallet(context, unrelated_wallet));
    BOOST_REQUIRE(AddWallet(context, wallet));

    auto& database = GetMockableDatabase(*wallet);
    const MockableData before_failed_persistence{database.m_records};
    database.FailWriteAt(0);
    RunPeriodicPaymasterMaintenance(context, maintenance_time);
    database.ClearFailureInjection();

    // Until the phase-one pending record commits, the leased inbox pair is the
    // retry source. A failed staging write must neither leave partial state nor
    // consume either proof.
    BOOST_CHECK(database.m_records == before_failed_persistence);
    BOOST_CHECK(manager.HasDirectMessages());
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);
    PaymasterProviderBlock absent_block;
    BOOST_CHECK(store.GetProviderBlock(provider_id, absent_block, error) ==
                DatabaseReadStatus::NOT_FOUND);

    RunPeriodicPaymasterMaintenance(context, maintenance_time);

    PaymasterProviderBlock block;
    BOOST_REQUIRE(store.GetProviderBlock(provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::CAPACITY);
    BOOST_CHECK_EQUAL(block.blocked_at, conflict_received_at);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(store.GetEquivocationEvidence(block.evidence_id, evidence));
    BOOST_CHECK(evidence.provider_id == provider_id);
    BOOST_CHECK(evidence.semantic_key == Hash(request_bytes));
    BOOST_CHECK(
        (evidence.first_artifact_hash == Hash(baseline_bytes) &&
         evidence.second_artifact_hash == Hash(conflict_bytes)) ||
        (evidence.first_artifact_hash == Hash(conflict_bytes) &&
         evidence.second_artifact_hash == Hash(baseline_bytes)));

    PaymasterReliabilityRecord reliability;
    BOOST_REQUIRE(store.GetProviderReliability(provider_id, reliability));
    BOOST_CHECK_EQUAL(reliability.cooldown_until,
                      std::numeric_limits<int64_t>::max());
    BOOST_REQUIRE(store.ClearProviderReliability(provider_id, error));
    BOOST_REQUIRE(store.GetProviderReliability(provider_id, reliability));
    BOOST_CHECK_EQUAL(reliability.cooldown_until,
                      std::numeric_limits<int64_t>::max());
    BOOST_CHECK(!manager.HasDirectMessages());

    const MockableData after_first_run{database.m_records};
    RunPeriodicPaymasterMaintenance(context, maintenance_time + 1);
    BOOST_CHECK(database.m_records == after_first_run);

    BOOST_REQUIRE(RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt));
    BOOST_REQUIRE(RemoveWallet(context, unrelated_wallet,
                               /*load_on_start=*/std::nullopt));
}

BOOST_FIXTURE_TEST_CASE(periodic_paymaster_maintenance_stages_single_capacity_claim_before_ack,
                        TestingSetup)
{
    using namespace DigiDollar::Paymaster;

    constexpr int64_t request_created_at{101};
    constexpr int64_t first_proof_created_at{102};
    constexpr int64_t second_proof_created_at{103};
    constexpr int64_t first_received_at{104};
    constexpr int64_t restarted_received_at{
        first_received_at + MAX_DIRECT_MESSAGE_TTL_SECONDS + 1};
    constexpr int64_t request_expires_at{150};
    constexpr int64_t first_proof_expires_at{140};
    constexpr int64_t second_proof_expires_at{139};
    const std::string request_id{
        "550e8400-e29b-41d4-a716-446655449902"};
    const uint256 attempt_id{uint256S("41")};

    auto wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-capacity-candidate-maintenance",
        CreateMockableWalletDatabase());
    PaymasterStore store{*wallet};
    PaymentSession session;
    std::string error;
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256S("42"), FeeMode::PAYMASTER, 100,
                      session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    const XOnlyPubKey provider_identity{provider_key.GetPubKey()};
    const PaymasterId provider_id{GetPaymasterId(provider_identity)};
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});

    PaymentIntent intent;
    intent.genesis_hash = uint256S("43");
    intent.provider_id = provider_id;
    intent.request_id = request_id;
    intent.session_id = session.session_id;
    intent.client_nonce = uint256S("44");
    intent.canonical_request_hash = session.canonical_request_hash;
    intent.requested_fee_mode = session.fee_mode_requested;
    intent.user_dd_inputs = {COutPoint{uint256S("45"), 0}};
    intent.recipient_script =
        GetScriptForDestination(recipient_builder.GetOutput());
    intent.recipient_amount = DDCents{100};
    intent.offer_id = uint256S("46");
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = uint256S("47");
    intent.expires_at = request_expires_at;

    ProviderAttempt attempt;
    attempt.attempt_id = attempt_id;
    attempt.provider_id = provider_id;
    attempt.provider_identity_key = provider_identity;
    attempt.client_nonce = intent.client_nonce;
    attempt.unsigned_intent = SerializeWalletLoadPaymasterObject(intent);
    attempt.created_at = attempt.updated_at = 100;
    BOOST_REQUIRE_MESSAGE(
        store.PreparePaymentIntent(request_id, attempt, 100, error), error);

    PaymasterCapacityRequest request;
    request.genesis_hash = intent.genesis_hash;
    request.provider_id = provider_id;
    request.request_id = request_id;
    request.session_id = session.session_id;
    request.client_nonce = intent.client_nonce;
    request.funding_model = intent.funding_model;
    request.requires_carrier = false;
    request.requested_slots = 1;
    request.created_at = request_created_at;
    request.expires_at = request_expires_at;
    const std::vector<unsigned char> request_bytes =
        SerializeWalletLoadPaymasterObject(request);
    BOOST_REQUIRE_MESSAGE(
        store.PrepareCapacityRequest(request_id, attempt_id, request_bytes,
                                     request_created_at, error),
        error);

    CMutableTransaction capacity_funding;
    capacity_funding.vin.emplace_back(COutPoint{uint256S("48"), 0});
    capacity_funding.vout.emplace_back(1000, CScript{} << OP_TRUE);
    const auto make_proof = [&](const uint256& snapshot_id,
                                int64_t created_at,
                                int64_t expires_at) {
        PaymasterCapacityProof proof;
        proof.genesis_hash = request.genesis_hash;
        proof.provider_id = request.provider_id;
        proof.request_id = request.request_id;
        proof.session_id = request.session_id;
        proof.client_nonce = request.client_nonce;
        proof.funding_model = request.funding_model;
        proof.requires_carrier = request.requires_carrier;
        proof.snapshot_id = snapshot_id;
        proof.created_at = created_at;
        proof.expires_at = expires_at;
        CapacityDGBInput dgb;
        dgb.input.outpoint =
            COutPoint{CTransaction{capacity_funding}.GetHash(), 0};
        dgb.input.creating_tx = capacity_funding;
        dgb.input.value = DGBSatoshis{1000};
        dgb.control_proof.reference_block = uint256S("49");
        dgb.control_proof.expires_at = proof.expires_at;
        dgb.control_proof.signature.assign(64, 1);
        PaymasterLiquiditySlot slot;
        slot.dgb_inputs = {dgb};
        proof.liquidity_slots = {slot};
        proof.identity_signature.resize(64);
        BOOST_REQUIRE(provider_key.SignSchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature,
            nullptr, uint256{}));
        return proof;
    };
    const PaymasterCapacityProof first_proof = make_proof(
        uint256S("4a"), first_proof_created_at, first_proof_expires_at);
    const PaymasterCapacityProof second_proof = make_proof(
        uint256S("4b"), second_proof_created_at,
        second_proof_expires_at);
    const std::vector<unsigned char> first_bytes =
        SerializeWalletLoadPaymasterObject(first_proof);
    const std::vector<unsigned char> second_bytes =
        SerializeWalletLoadPaymasterObject(second_proof);

    Manager manager{/*enabled=*/true};
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, Hash(first_bytes), first_bytes.size(), first_proof,
                    first_received_at) == DirectEnqueueResult::ACCEPTED);

    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    context.paymaster = &manager;
    BOOST_REQUIRE(AddWallet(context, wallet));

    auto& database = GetMockableDatabase(*wallet);
    database.FailWriteAt(0);
    RunPeriodicPaymasterMaintenance(context, first_received_at);
    database.ClearFailureInjection();

    ProviderAttempt persisted_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.capacity_proof_claim_candidate.empty());
    BOOST_CHECK(manager.HasDirectMessages());
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    RunPeriodicPaymasterMaintenance(context, first_received_at);
    BOOST_REQUIRE(store.GetAttempt(attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.capacity_proof_claim_candidate ==
                first_bytes);
    BOOST_CHECK(persisted_attempt.capacity_snapshot.snapshot_id.IsNull());
    BOOST_CHECK(persisted_attempt.capacity_snapshot.capacity_proof.empty());
    BOOST_CHECK(!manager.HasDirectMessages());

    auto restarted_database = DuplicateMockDatabase(wallet->GetDatabase());
    BOOST_REQUIRE(RemoveWallet(context, wallet,
                               /*load_on_start=*/std::nullopt));
    auto restarted_wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-capacity-candidate-restart",
        std::move(restarted_database));
    BOOST_REQUIRE(restarted_wallet->LoadWallet() == DBErrors::LOAD_OK);
    BOOST_REQUIRE(AddWallet(context, restarted_wallet));
    PaymasterStore restarted_store{*restarted_wallet};
    BOOST_REQUIRE(restarted_store.GetAttempt(attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.capacity_proof_claim_candidate ==
                first_bytes);

    Manager restarted_manager{/*enabled=*/true};
    context.paymaster = &restarted_manager;
    BOOST_CHECK(restarted_manager.EnqueueDirectMessageResult(
                    2, Hash(second_bytes), second_bytes.size(), second_proof,
                    restarted_received_at) == DirectEnqueueResult::ACCEPTED);
    RunPeriodicPaymasterMaintenance(context, restarted_received_at);

    PaymasterProviderBlock block;
    BOOST_REQUIRE(restarted_store.GetProviderBlock(provider_id, block,
                                                   error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::CAPACITY);
    BOOST_CHECK_EQUAL(block.blocked_at, restarted_received_at);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(
        restarted_store.GetEquivocationEvidence(block.evidence_id, evidence));
    BOOST_CHECK(evidence.semantic_key == Hash(request_bytes));
    BOOST_CHECK(
        (evidence.first_artifact_hash == Hash(first_bytes) &&
         evidence.second_artifact_hash == Hash(second_bytes)) ||
        (evidence.first_artifact_hash == Hash(second_bytes) &&
         evidence.second_artifact_hash == Hash(first_bytes)));
    BOOST_CHECK(!restarted_manager.HasDirectMessages());

    BOOST_REQUIRE(RemoveWallet(context, restarted_wallet,
                               /*load_on_start=*/std::nullopt));
}

BOOST_FIXTURE_TEST_CASE(periodic_paymaster_maintenance_stages_single_quote_claim_before_ack,
                        TestingSetup)
{
    using namespace DigiDollar::Paymaster;

    constexpr int64_t first_quote_created_at{102};
    constexpr int64_t second_quote_created_at{103};
    constexpr int64_t first_received_at{104};
    constexpr int64_t restarted_received_at{
        first_received_at + MAX_DIRECT_MESSAGE_TTL_SECONDS + 1};
    constexpr int64_t first_quote_expires_at{140};
    constexpr int64_t second_quote_expires_at{139};
    const std::string request_id{
        "550e8400-e29b-41d4-a716-446655449903"};

    auto wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-quote-candidate-maintenance",
        CreateMockableWalletDatabase());
    PaymasterStore store{*wallet};
    PaymentSession session;
    std::string error;
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256S("51"), FeeMode::PAYMASTER, 100,
                      session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});
    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("52");
    attempt.provider_identity_key = XOnlyPubKey{provider_key.GetPubKey()};
    attempt.provider_id = GetPaymasterId(attempt.provider_identity_key);
    attempt.created_at = attempt.updated_at = 100;
    BOOST_REQUIRE_MESSAGE(store.AddAttempt(request_id, attempt, error), error);
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, attempt));
    attempt.client_nonce = uint256S("53");

    PaymasterQuoteRequest authoritative_request;
    authoritative_request.intent.genesis_hash = uint256S("54");
    authoritative_request.intent.provider_id = attempt.provider_id;
    authoritative_request.intent.request_id = request_id;
    authoritative_request.intent.session_id = session.session_id;
    authoritative_request.intent.client_nonce = attempt.client_nonce;
    authoritative_request.intent.canonical_request_hash =
        session.canonical_request_hash;
    authoritative_request.intent.requested_fee_mode =
        session.fee_mode_requested;
    authoritative_request.intent.user_dd_inputs = {
        COutPoint{uint256S("541"), 0}};
    authoritative_request.intent.user_input_proofs = {
        {authoritative_request.intent.user_dd_inputs.front(),
         std::vector<unsigned char>(64, 1)}};
    authoritative_request.intent.recipient_script =
        GetScriptForDestination(recipient_builder.GetOutput());
    authoritative_request.intent.recipient_amount = DDCents{100};
    authoritative_request.intent.offer_id = uint256S("55");
    authoritative_request.intent.funding_model = FundingModel::SPONSORED;
    authoritative_request.intent.sponsorship_scope =
        SponsorshipScope::PUBLIC;
    authoritative_request.intent.policy_hash = uint256S("56");
    authoritative_request.intent.expires_at = first_quote_expires_at;
    attempt.intent_hash =
        GetPaymentIntentHash(authoritative_request.intent);
    attempt.quote_request =
        SerializeWalletLoadPaymasterObject(authoritative_request);
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(WalletBatch{wallet->GetDatabase()}
                          .WritePaymasterAttempt(attempt));
    }

    const auto make_quote = [&](const uint256& quote_id,
                                const uint256& unsigned_txid,
                                const uint256& template_commitment,
                                int64_t created_at,
                                int64_t expires_at) {
        PaymasterQuoteResponse response;
        response.request_id = request_id;
        response.session_id = session.session_id;
        response.quote.genesis_hash =
            authoritative_request.intent.genesis_hash;
        response.quote.provider_id = attempt.provider_id;
        response.quote.quote_id = quote_id;
        response.quote.intent_hash = attempt.intent_hash;
        response.quote.offer_id = uint256S("55");
        response.quote.policy_hash = uint256S("56");
        response.quote.funding_model = FundingModel::SPONSORED;
        response.quote.sponsorship_scope = SponsorshipScope::PUBLIC;
        response.quote.fee_rate_bps = 0;
        response.quote.service_fee = DDCents{0};
        VerifiedDGBInput input;
        input.outpoint = COutPoint{uint256S("57"), 0};
        input.value = DGBSatoshis{1000};
        response.quote.reserved_dgb_inputs = {input};
        response.quote.network_fee = DGBSatoshis{10};
        response.quote.created_at = created_at;
        response.quote.expires_at = expires_at;
        response.quote.retry_until = 200;
        response.quote.unsigned_txid = unsigned_txid;
        response.quote.template_commitment = template_commitment;
        response.quote.identity_signature.resize(64);
        BOOST_REQUIRE(provider_key.SignSchnorr(
            GetPaymasterQuoteSignatureHash(response.quote),
            response.quote.identity_signature, nullptr, uint256{}));
        return response;
    };
    const PaymasterQuoteResponse first_quote = make_quote(
        uint256S("58"), uint256S("59"), uint256S("5a"),
        first_quote_created_at, first_quote_expires_at);
    const PaymasterQuoteResponse second_quote = make_quote(
        uint256S("5b"), uint256S("5c"), uint256S("5d"),
        second_quote_created_at, second_quote_expires_at);
    const std::vector<unsigned char> first_bytes =
        SerializeWalletLoadPaymasterObject(first_quote);
    const std::vector<unsigned char> second_bytes =
        SerializeWalletLoadPaymasterObject(second_quote);

    Manager manager{/*enabled=*/true};
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    3, Hash(first_bytes), first_bytes.size(), first_quote,
                    first_received_at) == DirectEnqueueResult::ACCEPTED);

    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    context.paymaster = &manager;
    BOOST_REQUIRE(AddWallet(context, wallet));

    auto& database = GetMockableDatabase(*wallet);
    database.FailWriteAt(0);
    RunPeriodicPaymasterMaintenance(context, first_received_at);
    database.ClearFailureInjection();

    ProviderAttempt persisted_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.quote_response_claim_candidate.empty());
    BOOST_CHECK(manager.HasDirectMessages());
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    RunPeriodicPaymasterMaintenance(context, first_received_at);
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.quote_response_claim_candidate ==
                first_bytes);
    BOOST_CHECK(persisted_attempt.signed_quote.empty());
    BOOST_CHECK(!manager.HasDirectMessages());

    auto restarted_database = DuplicateMockDatabase(wallet->GetDatabase());
    BOOST_REQUIRE(RemoveWallet(context, wallet,
                               /*load_on_start=*/std::nullopt));
    auto restarted_wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-quote-candidate-restart",
        std::move(restarted_database));
    BOOST_REQUIRE(restarted_wallet->LoadWallet() == DBErrors::LOAD_OK);
    BOOST_REQUIRE(AddWallet(context, restarted_wallet));
    PaymasterStore restarted_store{*restarted_wallet};
    BOOST_REQUIRE(
        restarted_store.GetAttempt(attempt.attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.quote_response_claim_candidate ==
                first_bytes);

    Manager restarted_manager{/*enabled=*/true};
    context.paymaster = &restarted_manager;
    BOOST_CHECK(restarted_manager.EnqueueDirectMessageResult(
                    4, Hash(second_bytes), second_bytes.size(), second_quote,
                    restarted_received_at) == DirectEnqueueResult::ACCEPTED);
    RunPeriodicPaymasterMaintenance(context, restarted_received_at);

    PaymasterProviderBlock block;
    BOOST_REQUIRE(restarted_store.GetProviderBlock(attempt.provider_id, block,
                                                   error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::QUOTE);
    BOOST_CHECK_EQUAL(block.blocked_at, restarted_received_at);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(
        restarted_store.GetEquivocationEvidence(block.evidence_id, evidence));
    BOOST_CHECK(evidence.provider_id == attempt.provider_id);
    BOOST_CHECK(
        (evidence.first_artifact_hash == Hash(first_bytes) &&
         evidence.second_artifact_hash == Hash(second_bytes)) ||
        (evidence.first_artifact_hash == Hash(second_bytes) &&
         evidence.second_artifact_hash == Hash(first_bytes)));
    BOOST_CHECK(!restarted_manager.HasDirectMessages());

    BOOST_REQUIRE(RemoveWallet(context, restarted_wallet,
                               /*load_on_start=*/std::nullopt));
}

BOOST_FIXTURE_TEST_CASE(periodic_paymaster_maintenance_only_stages_exact_capacity_binding,
                        TestingSetup)
{
    using namespace DigiDollar::Paymaster;

    constexpr int64_t request_created_at{101};
    constexpr int64_t proof_created_at{102};
    constexpr int64_t received_at{104};
    constexpr int64_t request_expires_at{150};
    constexpr int64_t proof_expires_at{140};
    const std::string first_request_id{
        "550e8400-e29b-41d4-a716-446655449904"};
    const std::string second_request_id{
        "550e8400-e29b-41d4-a716-446655449905"};
    const uint256 shared_client_nonce{uint256S("61")};

    auto wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-exact-capacity-maintenance",
        CreateMockableWalletDatabase());
    PaymasterStore store{*wallet};
    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    const XOnlyPubKey provider_identity{provider_key.GetPubKey()};
    const PaymasterId provider_id{GetPaymasterId(provider_identity)};
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});

    struct PreparedAttempt {
        PaymentSession session;
        ProviderAttempt attempt;
        PaymasterCapacityRequest capacity_request;
    };
    std::string error;
    const auto prepare_attempt = [&](const std::string& request_id,
                                     const uint256& canonical_request_hash,
                                     const uint256& attempt_id,
                                     const uint256& genesis_hash,
                                     const uint256& user_outpoint_hash,
                                     const uint256& offer_id,
                                     const uint256& policy_hash) {
        PreparedAttempt prepared;
        BOOST_REQUIRE(store.CreateOrJoinSession(
                          request_id, canonical_request_hash,
                          FeeMode::PAYMASTER, 100, prepared.session, error) ==
                      CreatePaymasterSessionResult::CREATED);

        PaymentIntent intent;
        intent.genesis_hash = genesis_hash;
        intent.provider_id = provider_id;
        intent.request_id = request_id;
        intent.session_id = prepared.session.session_id;
        intent.client_nonce = shared_client_nonce;
        intent.canonical_request_hash =
            prepared.session.canonical_request_hash;
        intent.requested_fee_mode = prepared.session.fee_mode_requested;
        intent.user_dd_inputs = {
            COutPoint{user_outpoint_hash, 0}};
        intent.recipient_script =
            GetScriptForDestination(recipient_builder.GetOutput());
        intent.recipient_amount = DDCents{100};
        intent.offer_id = offer_id;
        intent.funding_model = FundingModel::SPONSORED;
        intent.sponsorship_scope = SponsorshipScope::PUBLIC;
        intent.policy_hash = policy_hash;
        intent.expires_at = request_expires_at;

        prepared.attempt.attempt_id = attempt_id;
        prepared.attempt.provider_id = provider_id;
        prepared.attempt.provider_identity_key = provider_identity;
        prepared.attempt.client_nonce = shared_client_nonce;
        prepared.attempt.unsigned_intent =
            SerializeWalletLoadPaymasterObject(intent);
        prepared.attempt.created_at = prepared.attempt.updated_at = 100;
        BOOST_REQUIRE_MESSAGE(
            store.PreparePaymentIntent(request_id, prepared.attempt, 100,
                                       error),
            error);

        prepared.capacity_request.genesis_hash = genesis_hash;
        prepared.capacity_request.provider_id = provider_id;
        prepared.capacity_request.request_id = request_id;
        prepared.capacity_request.session_id = prepared.session.session_id;
        prepared.capacity_request.client_nonce = shared_client_nonce;
        prepared.capacity_request.funding_model = FundingModel::SPONSORED;
        prepared.capacity_request.requires_carrier = false;
        prepared.capacity_request.requested_slots = 1;
        prepared.capacity_request.created_at = request_created_at;
        prepared.capacity_request.expires_at = request_expires_at;
        const std::vector<unsigned char> request_bytes =
            SerializeWalletLoadPaymasterObject(prepared.capacity_request);
        BOOST_REQUIRE_MESSAGE(
            store.PrepareCapacityRequest(
                request_id, attempt_id, request_bytes, request_created_at,
                error),
            error);
        BOOST_REQUIRE(store.GetAttempt(attempt_id, prepared.attempt));
        return prepared;
    };

    const PreparedAttempt first = prepare_attempt(
        first_request_id, uint256S("62"), uint256S("63"), uint256S("64"),
        uint256S("65"), uint256S("66"), uint256S("67"));
    const PreparedAttempt second = prepare_attempt(
        second_request_id, uint256S("68"), uint256S("69"), uint256S("6a"),
        uint256S("6b"), uint256S("6c"), uint256S("6d"));

    CMutableTransaction capacity_funding;
    capacity_funding.vin.emplace_back(COutPoint{uint256S("6e"), 0});
    capacity_funding.vout.emplace_back(1000, CScript{} << OP_TRUE);
    PaymasterCapacityProof second_proof;
    second_proof.genesis_hash = second.capacity_request.genesis_hash;
    second_proof.provider_id = provider_id;
    second_proof.request_id = second_request_id;
    second_proof.session_id = second.session.session_id;
    second_proof.client_nonce = shared_client_nonce;
    second_proof.funding_model = FundingModel::SPONSORED;
    second_proof.requires_carrier = false;
    second_proof.snapshot_id = uint256S("6f");
    second_proof.created_at = proof_created_at;
    second_proof.expires_at = proof_expires_at;
    CapacityDGBInput dgb;
    dgb.input.outpoint =
        COutPoint{CTransaction{capacity_funding}.GetHash(), 0};
    dgb.input.creating_tx = capacity_funding;
    dgb.input.value = DGBSatoshis{1000};
    dgb.control_proof.reference_block = uint256S("70");
    dgb.control_proof.expires_at = proof_expires_at;
    dgb.control_proof.signature.assign(64, 1);
    PaymasterLiquiditySlot slot;
    slot.dgb_inputs = {dgb};
    second_proof.liquidity_slots = {slot};
    second_proof.identity_signature.resize(64);
    BOOST_REQUIRE(provider_key.SignSchnorr(
        GetCapacityProofSignatureHash(second_proof),
        second_proof.identity_signature, nullptr, uint256{}));
    const std::vector<unsigned char> second_bytes =
        SerializeWalletLoadPaymasterObject(second_proof);

    Manager manager{/*enabled=*/true};
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    5, Hash(second_bytes), second_bytes.size(), second_proof,
                    received_at) == DirectEnqueueResult::ACCEPTED);
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    context.paymaster = &manager;
    BOOST_REQUIRE(AddWallet(context, wallet));

    std::vector<PaymentSession> maintenance_order;
    BOOST_REQUIRE(store.ListClientSessions(maintenance_order, error));
    BOOST_REQUIRE_EQUAL(maintenance_order.size(), 2U);
    BOOST_CHECK_EQUAL(maintenance_order.front().request_id,
                      first_request_id);
    BOOST_CHECK_EQUAL(maintenance_order.back().request_id,
                      second_request_id);

    RunPeriodicPaymasterMaintenance(context, received_at);

    ProviderAttempt first_persisted;
    ProviderAttempt second_persisted;
    BOOST_REQUIRE(store.GetAttempt(first.attempt.attempt_id,
                                   first_persisted));
    BOOST_REQUIRE(store.GetAttempt(second.attempt.attempt_id,
                                   second_persisted));
    BOOST_CHECK(first_persisted.capacity_proof_claim_candidate.empty());
    BOOST_CHECK(second_persisted.capacity_proof_claim_candidate ==
                second_bytes);
    BOOST_CHECK(!manager.HasDirectMessages());
    PaymasterProviderBlock absent_block;
    BOOST_CHECK(store.GetProviderBlock(provider_id, absent_block, error) ==
                DatabaseReadStatus::NOT_FOUND);

    BOOST_REQUIRE(RemoveWallet(context, wallet,
                               /*load_on_start=*/std::nullopt));
}

BOOST_FIXTURE_TEST_CASE(periodic_paymaster_maintenance_stages_single_recovery_capacity_claim_before_ack,
                        TestingSetup)
{
    using namespace DigiDollar::Paymaster;

    constexpr int64_t request_created_at{101};
    constexpr int64_t first_proof_created_at{102};
    constexpr int64_t second_proof_created_at{103};
    constexpr int64_t first_received_at{104};
    constexpr int64_t restarted_received_at{
        first_received_at + MAX_DIRECT_MESSAGE_TTL_SECONDS + 1};
    constexpr int64_t request_expires_at{150};
    constexpr int64_t first_proof_expires_at{140};
    constexpr int64_t second_proof_expires_at{139};

    auto wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-recovery-candidate-maintenance",
        CreateMockableWalletDatabase());
    PaymasterStore store{*wallet};
    CKey recovery_provider_key;
    recovery_provider_key.MakeNewKey(true);
    const XOnlyPubKey recovery_identity{
        recovery_provider_key.GetPubKey()};
    const PaymasterId recovery_provider_id{
        GetPaymasterId(recovery_identity)};

    AlternativeRecoveryRecord recovery;
    recovery.request_id =
        "550e8400-e29b-41d4-a716-446655449906";
    recovery.session_id = uint256S("71");
    recovery.original_provider_id = uint256S("72");
    recovery.recovery_provider_id = recovery_provider_id;
    recovery.offer_id = uint256S("73");
    recovery.policy_hash = uint256S("74");
    recovery.recovery_provider_identity_key = recovery_identity;
    recovery.recovery_provider_endpoint = "127.0.0.1:14022";
    recovery.original_commit_key = uint256S("75");
    recovery.original_template_commitment = uint256S("76");
    recovery.client_nonce = uint256S("77");
    recovery.created_at = recovery.updated_at = 100;
    recovery.phase = AlternativeRecoveryPhase::CAPACITY_PENDING;
    recovery.recovery_id = GetAlternativeRecoveryId(
        recovery.request_id, recovery.session_id,
        recovery.recovery_provider_id, recovery.client_nonce);
    recovery.capacity_request.genesis_hash = uint256S("78");
    recovery.capacity_request.provider_id = recovery_provider_id;
    recovery.capacity_request.request_id = recovery.request_id;
    recovery.capacity_request.session_id = recovery.session_id;
    recovery.capacity_request.client_nonce = recovery.client_nonce;
    recovery.capacity_request.funding_model = FundingModel::USER_PAID;
    recovery.capacity_request.requires_carrier = false;
    recovery.capacity_request.requested_slots = 1;
    recovery.capacity_request.created_at = request_created_at;
    recovery.capacity_request.expires_at = request_expires_at;
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(WalletBatch{wallet->GetDatabase()}
                          .WritePaymasterAlternativeRecovery(recovery));
    }

    CMutableTransaction capacity_funding;
    capacity_funding.vin.emplace_back(COutPoint{uint256S("79"), 0});
    capacity_funding.vout.emplace_back(1000, CScript{} << OP_TRUE);
    const auto make_proof = [&](const uint256& snapshot_id,
                                int64_t created_at,
                                int64_t expires_at) {
        PaymasterCapacityProof proof;
        proof.genesis_hash = recovery.capacity_request.genesis_hash;
        proof.provider_id = recovery_provider_id;
        proof.request_id = recovery.request_id;
        proof.session_id = recovery.session_id;
        proof.client_nonce = recovery.client_nonce;
        proof.funding_model = recovery.capacity_request.funding_model;
        proof.requires_carrier = false;
        proof.snapshot_id = snapshot_id;
        proof.created_at = created_at;
        proof.expires_at = expires_at;
        CapacityDGBInput dgb;
        dgb.input.outpoint =
            COutPoint{CTransaction{capacity_funding}.GetHash(), 0};
        dgb.input.creating_tx = capacity_funding;
        dgb.input.value = DGBSatoshis{1000};
        dgb.control_proof.reference_block = uint256S("7a");
        dgb.control_proof.expires_at = proof.expires_at;
        dgb.control_proof.signature.assign(64, 1);
        PaymasterLiquiditySlot slot;
        slot.dgb_inputs = {dgb};
        proof.liquidity_slots = {slot};
        proof.identity_signature.resize(64);
        BOOST_REQUIRE(recovery_provider_key.SignSchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature,
            nullptr, uint256{}));
        return proof;
    };
    const PaymasterCapacityProof first_proof = make_proof(
        uint256S("7b"), first_proof_created_at, first_proof_expires_at);
    const PaymasterCapacityProof second_proof = make_proof(
        uint256S("7c"), second_proof_created_at,
        second_proof_expires_at);
    const std::vector<unsigned char> first_bytes =
        SerializeWalletLoadPaymasterObject(first_proof);
    const std::vector<unsigned char> second_bytes =
        SerializeWalletLoadPaymasterObject(second_proof);

    Manager manager{/*enabled=*/true};
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    6, Hash(first_bytes), first_bytes.size(), first_proof,
                    first_received_at) == DirectEnqueueResult::ACCEPTED);
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    context.paymaster = &manager;
    BOOST_REQUIRE(AddWallet(context, wallet));

    auto& database = GetMockableDatabase(*wallet);
    database.FailWriteAt(0);
    RunPeriodicPaymasterMaintenance(context, first_received_at);
    database.ClearFailureInjection();

    AlternativeRecoveryRecord persisted_recovery;
    BOOST_REQUIRE(store.GetAlternativeRecoveryById(
        recovery.recovery_id, persisted_recovery));
    BOOST_CHECK(persisted_recovery.capacity_proof_claim_candidate.empty());
    BOOST_CHECK(manager.HasDirectMessages());
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    RunPeriodicPaymasterMaintenance(context, first_received_at);
    BOOST_REQUIRE(store.GetAlternativeRecoveryById(
        recovery.recovery_id, persisted_recovery));
    BOOST_CHECK(persisted_recovery.capacity_proof_claim_candidate ==
                first_bytes);
    BOOST_CHECK(persisted_recovery.capacity_snapshot.snapshot_id.IsNull());
    BOOST_CHECK(persisted_recovery.capacity_snapshot.capacity_proof.empty());
    BOOST_CHECK(persisted_recovery.phase ==
                AlternativeRecoveryPhase::CAPACITY_PENDING);
    BOOST_CHECK(!manager.HasDirectMessages());

    auto restarted_database = DuplicateMockDatabase(wallet->GetDatabase());
    BOOST_REQUIRE(RemoveWallet(context, wallet,
                               /*load_on_start=*/std::nullopt));
    auto restarted_wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-recovery-candidate-restart",
        std::move(restarted_database));
    BOOST_REQUIRE(restarted_wallet->LoadWallet() == DBErrors::LOAD_OK);
    BOOST_REQUIRE(AddWallet(context, restarted_wallet));
    PaymasterStore restarted_store{*restarted_wallet};
    BOOST_REQUIRE(restarted_store.GetAlternativeRecoveryById(
        recovery.recovery_id, persisted_recovery));
    BOOST_CHECK(persisted_recovery.capacity_proof_claim_candidate ==
                first_bytes);

    Manager restarted_manager{/*enabled=*/true};
    context.paymaster = &restarted_manager;
    BOOST_CHECK(restarted_manager.EnqueueDirectMessageResult(
                    7, Hash(second_bytes), second_bytes.size(), second_proof,
                    restarted_received_at) == DirectEnqueueResult::ACCEPTED);
    RunPeriodicPaymasterMaintenance(context, restarted_received_at);

    std::string error;
    PaymasterProviderBlock block;
    BOOST_REQUIRE(restarted_store.GetProviderBlock(recovery_provider_id, block,
                                                   error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::CAPACITY);
    BOOST_CHECK_EQUAL(block.blocked_at, restarted_received_at);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(
        restarted_store.GetEquivocationEvidence(block.evidence_id, evidence));
    const std::vector<unsigned char> request_bytes =
        SerializeWalletLoadPaymasterObject(recovery.capacity_request);
    BOOST_CHECK(evidence.semantic_key == Hash(request_bytes));
    BOOST_CHECK(
        (evidence.first_artifact_hash == Hash(first_bytes) &&
         evidence.second_artifact_hash == Hash(second_bytes)) ||
        (evidence.first_artifact_hash == Hash(second_bytes) &&
         evidence.second_artifact_hash == Hash(first_bytes)));
    BOOST_CHECK(!restarted_manager.HasDirectMessages());

    BOOST_REQUIRE(RemoveWallet(context, restarted_wallet,
                               /*load_on_start=*/std::nullopt));
}

BOOST_FIXTURE_TEST_CASE(periodic_paymaster_maintenance_promotes_pending_equivocation_without_inbox,
                        TestingSetup)
{
    using namespace DigiDollar::Paymaster;

    constexpr int64_t maintenance_time{201};
    const PaymasterId provider_id{uint256S("31")};
    const PaymasterEquivocationEvidence pending_evidence =
        MakeWalletLoadEquivocationEvidence(
            provider_id, EquivocationKind::QUOTE, uint256S("32"),
            {1, 2, 3}, {4, 5, 6}, 200);
    BOOST_REQUIRE(ValidateEquivocationEvidence(pending_evidence));

    auto wallet = std::make_shared<CWallet>(
        m_node.chain.get(), "paymaster-pending-equivocation-maintenance",
        CreateMockableWalletDatabase());
    PaymasterStore store{*wallet};
    std::string error;
    BOOST_REQUIRE_MESSAGE(
        store.StagePendingEquivocation(pending_evidence, error), error);

    Manager manager{/*enabled=*/true};
    BOOST_CHECK(!manager.HasEquivocationCandidates());
    BOOST_CHECK(!manager.HasDirectMessages());

    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    context.paymaster = &manager;
    BOOST_REQUIRE(AddWallet(context, wallet));

    RunPeriodicPaymasterMaintenance(context, maintenance_time);

    PaymasterEquivocationEvidence pending;
    BOOST_CHECK(store.GetPendingEquivocation(provider_id, pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    PaymasterProviderBlock block;
    BOOST_REQUIRE(store.GetProviderBlock(provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.evidence_id == pending_evidence.evidence_id);
    BOOST_CHECK(block.kind == pending_evidence.kind);
    PaymasterEquivocationEvidence persisted;
    BOOST_REQUIRE(store.GetEquivocationEvidence(block.evidence_id, persisted));
    BOOST_CHECK(persisted.provider_id == provider_id);
    BOOST_CHECK(!manager.HasDirectMessages());

    BOOST_REQUIRE(RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
