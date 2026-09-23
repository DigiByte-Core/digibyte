// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Atomic persistence, crash-replay, budget, and recovery invariants. */

#include <boost/test/unit_test.hpp>

#include <hash.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <wallet/digidollarwallet.h>
#include <wallet/rpc/paymaster_send.h>
#include <key.h>
#include <key_io.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/wire.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <util/time.h>
#include <validationinterface.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_internal.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <cstddef>
#include <limits>

using namespace DigiDollar::Paymaster;
using namespace wallet;

namespace {

class ScopedRecoveryMockTime
{
public:
    explicit ScopedRecoveryMockTime(int64_t now) : m_previous{GetMockTime()}
    {
        // Drain genesis/tip notifications before installing records with
        // deliberately small synthetic timestamps.
        SyncWithValidationInterfaceQueue();
        SetMockTime(now);
    }

    ~ScopedRecoveryMockTime()
    {
        SyncWithValidationInterfaceQueue();
        SetMockTime(m_previous);
    }

private:
    const std::chrono::seconds m_previous;
};

template <typename T>
std::vector<unsigned char> SerializePaymasterTestObject(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

PaymasterEquivocationEvidence MakeTestEquivocationEvidence(
    const PaymasterId& provider_id,
    EquivocationKind kind,
    const uint256& semantic_key,
    std::vector<unsigned char> first_artifact,
    std::vector<unsigned char> second_artifact,
    int64_t observed_at)
{
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

bool CommitTestCapacitySnapshot(PaymasterStore& store,
                                const std::string& request_id,
                                const uint256& attempt_id,
                                const PaymentIntent& intent,
                                const CKey& provider_key,
                                int64_t now,
                                std::string& error,
                                const VerifiedDGBInput* capacity_input = nullptr)
{
    PaymasterCapacityRequest request;
    request.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    request.genesis_hash = intent.genesis_hash;
    request.provider_id = intent.provider_id;
    request.request_id = request_id;
    request.session_id = intent.session_id;
    request.client_nonce = intent.client_nonce;
    request.funding_model = intent.funding_model;
    request.requires_carrier = false;
    request.created_at = now;
    request.expires_at = std::min(intent.expires_at, now + 20);
    const std::vector<unsigned char> request_bytes =
        SerializePaymasterTestObject(request);
    if (!store.PrepareCapacityRequest(request_id, attempt_id, request_bytes,
                                      now, error)) {
        return false;
    }

    PaymasterCapacityProof proof;
    proof.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    proof.genesis_hash = request.genesis_hash;
    proof.provider_id = request.provider_id;
    proof.request_id = request.request_id;
    proof.session_id = request.session_id;
    proof.client_nonce = request.client_nonce;
    proof.funding_model = request.funding_model;
    proof.requires_carrier = request.requires_carrier;
    proof.snapshot_id = attempt_id;
    proof.created_at = now + 1;
    proof.expires_at = request.expires_at;
    CapacityDGBInput dgb;
    if (capacity_input) {
        dgb.input = *capacity_input;
    } else {
        CMutableTransaction creating_tx;
        creating_tx.vin.emplace_back(COutPoint{attempt_id, 0});
        creating_tx.vout.emplace_back(1000, CScript{} << OP_TRUE);
        dgb.input.outpoint = COutPoint{CTransaction{creating_tx}.GetHash(), 0};
        dgb.input.creating_tx = std::move(creating_tx);
        dgb.input.value = DGBSatoshis{1000};
    }
    dgb.control_proof.reference_block = uint256S("01");
    dgb.control_proof.expires_at = proof.expires_at;
    dgb.control_proof.signature.assign(64, 1);
    PaymasterLiquiditySlot slot;
    slot.dgb_inputs = {dgb};
    proof.liquidity_slots = {slot};
    proof.identity_signature.resize(64);
    if (!provider_key.SignSchnorr(GetCapacityProofSignatureHash(proof),
                                  proof.identity_signature, nullptr,
                                  uint256{})) {
        error = "PAYMASTER_TEST_CAPACITY_SIGNING";
        return false;
    }

    ValidatedCapacitySnapshot snapshot;
    snapshot.snapshot_id = proof.snapshot_id;
    snapshot.resource_commitment = GetCapacityResourceCommitment(proof);
    snapshot.session_id = request.session_id;
    snapshot.attempt_id = attempt_id;
    snapshot.provider_id = proof.provider_id;
    snapshot.client_nonce = proof.client_nonce;
    snapshot.request_hash = Hash(request_bytes);
    snapshot.capacity_proof = SerializePaymasterTestObject(proof);
    snapshot.created_at = proof.created_at;
    snapshot.expires_at = proof.expires_at;
    snapshot.validated_at = now + 1;
    snapshot.funding_model = proof.funding_model;
    snapshot.requires_carrier = proof.requires_carrier;
    bool equivocation{false};
    if (!store.StageCapacityProofClaimCandidate(
            attempt_id, snapshot.capacity_proof, now + 1, equivocation,
            error) ||
        equivocation) {
        if (error.empty()) {
            error = "PAYMASTER_TEST_CAPACITY_CLAIM_CANDIDATE";
        }
        return false;
    }
    return store.CommitValidatedCapacitySnapshot(
        request_id, attempt_id, snapshot, now + 1, error);
}

struct ValidClientResultArtifacts {
    CKey provider_identity_key;
    ProviderAttempt attempt;
    CMutableTransaction final_transaction;
    COutPoint user_input;
    uint256 genesis_hash;
};

bool BuildValidClientResultArtifacts(wallet::CWallet& wallet,
                                     const PaymentSession& session,
                                     const std::string& request_id,
                                     const uint256& attempt_id,
                                     int64_t now,
                                     ValidClientResultArtifacts& artifacts,
                                     std::string& error,
                                     const ProviderPolicy* provider_policy = nullptr)
{
    artifacts.provider_identity_key = CKey{};
    artifacts.attempt = ProviderAttempt{};
    artifacts.final_transaction = CMutableTransaction{};
    artifacts.user_input = COutPoint{};
    artifacts.genesis_hash.SetNull();
    CKey wallet_key;
    wallet_key.MakeNewKey(true);
    FlatSigningProvider signing_provider;
    std::unique_ptr<Descriptor> descriptor = Parse(
        "tr(" + EncodeSecret(wallet_key) + ")", signing_provider, error,
        /*require_checksum=*/false);
    if (!descriptor) return false;
    WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 0};
    {
        LOCK(wallet.cs_wallet);
        wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        if (!wallet.AddWalletDescriptor(
                wallet_descriptor, signing_provider, "", false)) {
            error = "PAYMASTER_TEST_DESCRIPTOR";
            return false;
        }
    }

    TaprootBuilder taproot;
    taproot.Finalize(XOnlyPubKey{wallet_key.GetPubKey()});
    const CScript wallet_script = GetScriptForDestination(taproot.GetOutput());
    CMutableTransaction user_previous;
    user_previous.vin.emplace_back(COutPoint{attempt_id, 1});
    user_previous.vout.emplace_back(4000, wallet_script);
    const CTransactionRef user_tx = MakeTransactionRef(std::move(user_previous));
    {
        LOCK(wallet.cs_wallet);
        if (!wallet.AddToWallet(user_tx, TxStateInMempool{})) {
            error = "PAYMASTER_TEST_WALLET_TX";
            return false;
        }
    }
    CMutableTransaction provider_previous;
    provider_previous.vin.emplace_back(COutPoint{attempt_id, 2});
    provider_previous.vout.emplace_back(2000, wallet_script);
    const CTransactionRef provider_tx =
        MakeTransactionRef(std::move(provider_previous));

    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{user_tx->GetHash(), 0});
    transaction.vin.emplace_back(COutPoint{provider_tx->GetHash(), 0});
    transaction.vout.emplace_back(5500, wallet_script);
    CollaborativePSBTTemplate trusted;
    if (!CreateCollaborativePSBTTemplate(
            transaction,
            {{transaction.vin[0].prevout, user_tx, InputRole::USER_DD},
             {transaction.vin[1].prevout, provider_tx, InputRole::PROVIDER_DGB}},
            trusted, error)) {
        return false;
    }

    artifacts.provider_identity_key.MakeNewKey(true);
    const XOnlyPubKey provider_identity{
        artifacts.provider_identity_key.GetPubKey()};
    const PaymasterId provider_id{GetPaymasterId(provider_identity)};
    artifacts.genesis_hash = uint256S("53");

    PaymentIntent intent;
    intent.genesis_hash = artifacts.genesis_hash;
    intent.provider_id = provider_id;
    intent.request_id = request_id;
    intent.session_id = session.session_id;
    intent.client_nonce = uint256S("5101");
    intent.canonical_request_hash = session.canonical_request_hash;
    intent.requested_fee_mode = session.fee_mode_requested;
    intent.user_dd_inputs = {transaction.vin[0].prevout};
    intent.user_input_proofs = {
        {transaction.vin[0].prevout, std::vector<unsigned char>(64, 1)}};
    intent.recipient_script = wallet_script;
    intent.recipient_amount = DDCents{1000};
    intent.user_dd_change_script = wallet_script;
    intent.offer_id = uint256S("5102");
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = provider_policy ? GetProviderPolicyHash(*provider_policy) : uint256S("5103");
    intent.expires_at = now + 60;

    const VerifiedDGBInput provider_input{
        transaction.vin[1].prevout, CMutableTransaction{*provider_tx},
        DGBSatoshis{2000}};
    PaymasterCapacityProof capacity_proof;
    capacity_proof.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    capacity_proof.genesis_hash = intent.genesis_hash;
    capacity_proof.provider_id = intent.provider_id;
    capacity_proof.request_id = request_id;
    capacity_proof.session_id = intent.session_id;
    capacity_proof.client_nonce = intent.client_nonce;
    capacity_proof.funding_model = intent.funding_model;
    capacity_proof.requires_carrier = false;
    capacity_proof.snapshot_id = attempt_id;
    capacity_proof.created_at = now;
    capacity_proof.expires_at = now + 60;
    CapacityDGBInput capacity_input;
    capacity_input.input = provider_input;
    capacity_input.control_proof.reference_block = uint256::ONE;
    capacity_input.control_proof.expires_at = capacity_proof.expires_at;
    capacity_input.control_proof.signature.assign(64, 1);
    PaymasterLiquiditySlot capacity_slot;
    capacity_slot.dgb_inputs = {capacity_input};
    capacity_proof.liquidity_slots = {capacity_slot};
    capacity_proof.identity_signature.assign(64, 0);
    if (!artifacts.provider_identity_key.SignSchnorr(
            GetCapacityProofSignatureHash(capacity_proof),
            capacity_proof.identity_signature, nullptr, uint256{})) {
        error = "PAYMASTER_TEST_CAPACITY_SIGNING";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    capacity_request.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    capacity_request.genesis_hash = intent.genesis_hash;
    capacity_request.provider_id = intent.provider_id;
    capacity_request.request_id = request_id;
    capacity_request.session_id = intent.session_id;
    capacity_request.client_nonce = intent.client_nonce;
    capacity_request.funding_model = intent.funding_model;
    capacity_request.requires_carrier = capacity_proof.requires_carrier;
    capacity_request.created_at = capacity_proof.created_at;
    capacity_request.expires_at = capacity_proof.expires_at;
    const std::vector<unsigned char> capacity_request_bytes =
        SerializePaymasterTestObject(capacity_request);

    ValidatedCapacitySnapshot capacity;
    capacity.snapshot_id = capacity_proof.snapshot_id;
    capacity.resource_commitment =
        GetCapacityResourceCommitment(capacity_proof);
    capacity.session_id = intent.session_id;
    capacity.attempt_id = attempt_id;
    capacity.provider_id = intent.provider_id;
    capacity.client_nonce = intent.client_nonce;
    capacity.request_hash = Hash(capacity_request_bytes);
    capacity.capacity_proof = SerializePaymasterTestObject(capacity_proof);
    capacity.created_at = capacity_proof.created_at;
    capacity.expires_at = capacity_proof.expires_at;
    capacity.validated_at = now;
    capacity.funding_model = capacity_proof.funding_model;
    capacity.requires_carrier = capacity_proof.requires_carrier;

    PaymasterQuote quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = uint256S("5105");
    quote.intent_hash = GetPaymentIntentHash(intent);
    quote.offer_id = intent.offer_id;
    quote.policy_hash = intent.policy_hash;
    quote.funding_model = intent.funding_model;
    quote.sponsorship_scope = intent.sponsorship_scope;
    quote.service_fee = DDCents{0};
    quote.reserved_dgb_inputs = {provider_input};
    quote.dgb_change_script = wallet_script;
    quote.network_fee = DGBSatoshis{500};
    quote.created_at = now;
    quote.expires_at = now + 60;
    quote.retry_until = now + 600;
    quote.unsigned_transaction = transaction;
    quote.unsigned_txid = CTransaction{transaction}.GetHash();
    quote.template_commitment =
        GetCollaborativeTemplateCommitment(intent, quote, trusted);
    quote.identity_signature.assign(64, 0);
    if (!artifacts.provider_identity_key.SignSchnorr(
            GetPaymasterQuoteSignatureHash(quote), quote.identity_signature,
            nullptr, uint256{})) {
        error = "PAYMASTER_TEST_QUOTE_SIGNING";
        return false;
    }

    PaymasterQuoteRequest request;
    request.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    request.intent = intent;
    PaymasterQuoteResponse response;
    response.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    response.request_id = request_id;
    response.session_id = session.session_id;
    response.quote = quote;

    ProviderAttempt& attempt = artifacts.attempt;
    attempt.session_id = session.session_id;
    attempt.attempt_id = attempt_id;
    attempt.provider_id = provider_id;
    attempt.provider_identity_key = provider_identity;
    attempt.state = AttemptState::USER_SIGNED;
    attempt.client_nonce = intent.client_nonce;
    attempt.intent_hash = quote.intent_hash;
    attempt.quote_id = quote.quote_id;
    attempt.unsigned_txid = quote.unsigned_txid;
    attempt.template_commitment = quote.template_commitment;
    attempt.commit_key = GetPaymasterCommitKey(
        attempt.provider_id, attempt.client_nonce, attempt.intent_hash,
        attempt.quote_id, attempt.template_commitment);
    attempt.capacity_request = capacity_request_bytes;
    attempt.quote_request = SerializePaymasterTestObject(request);
    attempt.signed_quote = SerializePaymasterTestObject(response);
    attempt.unsigned_transaction = SerializePaymasterTestObject(transaction);
    attempt.unsigned_psbt = SerializePaymasterTestObject(trusted.psbt);
    attempt.input_roles = {
        ReservationRole::USER_DD, ReservationRole::PROVIDER_DGB};
    attempt.capacity_snapshot = capacity;
    attempt.quote_expires_at = quote.expires_at;
    attempt.retry_until = quote.retry_until;
    attempt.created_at = now;
    attempt.updated_at = now;
    if (!BuildClientAuthorizationManifest(
            intent, quote, capacity, trusted, DDCents{0},
            attempt.client_manifest, error)) {
        return false;
    }
    attempt.accepted_client_manifest_id =
        attempt.client_manifest.manifest_id;
    attempt.client_manifest_accepted_at = now;

    PartiallySignedTransaction signed_psbt{trusted.psbt};
    if (!SignCollaborativePSBTForParty(
            wallet, signed_psbt, trusted, SigningParty::USER, error)) {
        return false;
    }
    attempt.user_signed_psbt = SerializePaymasterTestObject(signed_psbt);
    if (!SignCollaborativePSBTForParty(
            wallet, signed_psbt, trusted, SigningParty::PROVIDER, error) ||
        !FinalizeAndExtractPSBT(signed_psbt, artifacts.final_transaction)) {
        if (error.empty()) error = "PAYMASTER_TEST_FINALIZE";
        return false;
    }
    artifacts.user_input = transaction.vin[0].prevout;
    return true;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_wallet_store_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(capacity_replay_readiness_requires_exact_stored_response)
{
    using namespace wallet::paymaster_rpc::internal;
    ProviderReadiness readiness;
    BOOST_CHECK(!CapacityRequestMayProceed(readiness, false));
    BOOST_CHECK(!CapacityRequestMayProceed(readiness, true));
    for (const auto* error : {"PAYMASTER_OPERATIONAL_SLOT_MISSING",
                             "PAYMASTER_SAFETY_LIMIT_EXHAUSTED"}) {
        readiness.errors = {error};
        BOOST_CHECK(!CapacityRequestMayProceed(readiness, false));
        BOOST_CHECK(CapacityRequestMayProceed(readiness, true));
    }
    readiness.errors = {"PAYMASTER_OPERATIONAL_SLOT_MISSING",
                        "PAYMASTER_SAFETY_LIMIT_EXHAUSTED"};
    BOOST_CHECK(CapacityRequestMayProceed(readiness, true));
    for (const auto* error : {"PAYMASTER_REQUIRES_READY_TXINDEX",
                             "PAYMASTER_POLICY_NOT_FOUND",
                             "PAYMASTER_POOL_RECONCILIATION_FAILED",
                             "UNKNOWN_READINESS_FAILURE"}) {
        readiness.errors = {"PAYMASTER_OPERATIONAL_SLOT_MISSING", error};
        BOOST_CHECK(!CapacityRequestMayProceed(readiness, true));
    }
    readiness.errors.clear();
    readiness.ready = true;
    BOOST_CHECK(CapacityRequestMayProceed(readiness, false));
    BOOST_CHECK(CapacityRequestMayProceed(readiness, true));
}

BOOST_AUTO_TEST_CASE(provider_attempt_serialization_is_current_only)
{
    ProviderAttempt current;
    current.session_id = uint256S("01");
    current.attempt_id = uint256S("02");
    current.accepted_client_manifest_id = uint256S("03");
    current.client_manifest_accepted_at = 100;
    current.capacity_proof_claim_candidate = {4, 5, 6};
    current.quote_response_claim_candidate = {7, 8, 9};
    CDataStream current_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    current_stream << current;
    ProviderAttempt current_roundtrip;
    current_stream >> current_roundtrip;
    BOOST_CHECK_EQUAL(current_roundtrip.version,
                      ProviderAttempt::CURRENT_VERSION);
    BOOST_CHECK_EQUAL(current_roundtrip.accepted_client_manifest_id,
                      current.accepted_client_manifest_id);
    BOOST_CHECK_EQUAL(current_roundtrip.client_manifest_accepted_at,
                      current.client_manifest_accepted_at);
    BOOST_CHECK(current_roundtrip.capacity_proof_claim_candidate ==
                current.capacity_proof_claim_candidate);
    BOOST_CHECK(current_roundtrip.quote_response_claim_candidate ==
                current.quote_response_claim_candidate);

    for (const uint16_t outdated_version :
         {uint16_t{12},
          uint16_t{ProviderAttempt::CURRENT_VERSION - 1}}) {
        ProviderAttempt outdated{current};
        outdated.version = outdated_version;
        CDataStream outdated_stream{SER_NETWORK, ::PROTOCOL_VERSION};
        outdated_stream << outdated;
        ProviderAttempt decoded;
        outdated_stream >> decoded;
        BOOST_CHECK(outdated_stream.empty());
        BOOST_CHECK_EQUAL(decoded.version, outdated_version);
        BOOST_CHECK(decoded.accepted_client_manifest_id ==
                    current.accepted_client_manifest_id);
        BOOST_CHECK(decoded.capacity_proof_claim_candidate ==
                    current.capacity_proof_claim_candidate);
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!WalletBatch{m_wallet.GetDatabase()}
                         .WritePaymasterAttempt(decoded));
    }
}

BOOST_AUTO_TEST_CASE(outdated_session_records_fail_without_mutation)
{
    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-44665544f001";
    PaymentSession outdated;
    outdated.version = PaymentSession::CURRENT_VERSION - 1;
    outdated.request_id = request_id;
    outdated.session_id = uint256S("f001");
    outdated.canonical_request_hash = uint256S("f002");
    outdated.created_at = 100;
    outdated.updated_at = 100;

    DataStream key_stream;
    key_stream << std::make_pair(DBKeys::PAYMASTER_SESSION,
                                 std::string{request_id});
    const SerializeData key{key_stream.begin(), key_stream.end()};
    auto& database = GetMockableDatabase(m_wallet);
    PaymasterStore store{m_wallet};

    for (const bool shortened_layout : {false, true}) {
        CDataStream value_stream{SER_DISK, CLIENT_VERSION};
        value_stream << outdated;
        SerializeData value{value_stream.begin(), value_stream.end()};
        if (shortened_layout) {
            // PaymentSession v3 ended before the v4 amount and two flags.
            BOOST_REQUIRE_GE(value.size(), 10U);
            value.resize(value.size() - 10);
        }
        database.m_records[key] = std::move(value);
        const MockableData before{database.m_records};

        PaymentSession loaded;
        std::string error;
        {
            LOCK(m_wallet.cs_wallet);
            WalletBatch batch{m_wallet.GetDatabase()};
            BOOST_CHECK(batch.ReadPaymasterSessionWithStatus(
                            request_id, loaded) ==
                        DatabaseReadStatus::UNSUPPORTED_VERSION);
            BOOST_CHECK_EQUAL(loaded.version,
                              PaymentSession::CURRENT_VERSION - 1);
        }
        BOOST_CHECK(store.CreateOrJoinSession(
                        request_id, outdated.canonical_request_hash,
                        FeeMode::DGB, 101, loaded, error) ==
                    CreatePaymasterSessionResult::DATABASE_ERROR);
        BOOST_CHECK_EQUAL(
            error,
            "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=PaymentSession found=3 expected=4");
        BOOST_CHECK(database.m_records == before);
        BOOST_CHECK(!store.GetSessionByRequestId(request_id, loaded));
        BOOST_CHECK(database.m_records == before);
    }

    CDataStream current_but_truncated{SER_DISK, CLIENT_VERSION};
    current_but_truncated << PaymentSession::CURRENT_VERSION;
    database.m_records[key] = SerializeData{current_but_truncated.begin(),
                                            current_but_truncated.end()};
    const MockableData corrupt_before{database.m_records};
    PaymentSession corrupt;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_CHECK(batch.ReadPaymasterSessionWithStatus(
                        request_id, corrupt) ==
                    DatabaseReadStatus::READ_ERROR);
    }
    std::string error;
    BOOST_CHECK(store.CreateOrJoinSession(
                    request_id, outdated.canonical_request_hash,
                    FeeMode::DGB, 101, corrupt, error) ==
                CreatePaymasterSessionResult::DATABASE_ERROR);
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_PERSISTED_SESSION");
    BOOST_CHECK(database.m_records == corrupt_before);
}


BOOST_AUTO_TEST_CASE(auto_dispatch_rejects_unreadable_session_before_funding_or_signing)
{
    constexpr auto request_id = "550e8400-e29b-41d4-a716-44665544f003";
    m_wallet.EnsureDDWallet();
    auto& dd_wallet = *m_wallet.GetDDWallet();
    dd_wallet.AddDDUTXO(COutPoint{uint256S("f004"), 0}, 10000);
    PaymasterSendOptions options;
    options.fee_mode = FeeMode::AUTO;
    options.send_options.pushKV("request_id", request_id);
    options.send_options.pushKV("maximum_paymaster_fee_cents", 100);
    JSONRPCRequest request;
    std::vector<COutPoint> inputs;
    const std::vector<COutPoint>* preset{nullptr};
    auto& database = GetMockableDatabase(m_wallet);
    PaymasterStore store{m_wallet};
    const auto tx_count = m_wallet.mapWallet.size();
    const auto dispatch = [&] {
        return PrepareDigiDollarFeeFunding(request, m_wallet, dd_wallet, {},
            CDigiDollarAddress{}, 1, options, inputs, preset);
    };
    const auto expect_error = [&](const char* message) {
        const auto before = database.m_records;
        BOOST_CHECK_EXCEPTION(dispatch(), UniValue, [&](const UniValue& error) {
            return error.find_value("code").getInt<int>() == RPC_WALLET_ERROR &&
                   error.find_value("message").get_str() == message;
        });
        BOOST_CHECK(database.m_records == before);
        BOOST_CHECK_EQUAL(m_wallet.mapWallet.size(), tx_count);
        BOOST_CHECK(inputs.empty());
        BOOST_CHECK(preset == nullptr);
    };
    PaymentSession session;
    BOOST_CHECK(store.GetSessionByRequestIdWithStatus(request_id, session) == DatabaseReadStatus::NOT_FOUND);
    for (const bool tombstone : {false, true}) {
        DataStream key_stream;
        key_stream << std::make_pair(tombstone ? DBKeys::PAYMASTER_TOMBSTONE : DBKeys::PAYMASTER_SESSION,
                                    std::string{request_id});
        const SerializeData key{key_stream.begin(), key_stream.end()};
        for (const bool unknown_version : {false, true}) {
            CDataStream value{SER_DISK, CLIENT_VERSION};
            const uint16_t version = tombstone ? IdempotencyTombstone::CURRENT_VERSION : PaymentSession::CURRENT_VERSION;
            // A readable future version and a truncated current record must
            // both stop AUTO before it can choose the ordinary payment path.
            value << static_cast<uint16_t>(version + (unknown_version ? 1 : 0));
            database.m_records[key] = SerializeData{value.begin(), value.end()};
            expect_error(unknown_version ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_SESSION_READ_FAILED");
        }
        database.m_records.erase(key);
    }
    database.m_pass = false;
    expect_error("PAYMASTER_SESSION_READ_FAILED");
    database.m_pass = true;
    BOOST_CHECK(store.GetSessionByRequestIdWithStatus(request_id, session) == DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK_EQUAL(dd_wallet.GetTotalDDBalance(), 10000);
}

BOOST_AUTO_TEST_CASE(prune_rejects_unreadable_attempt_without_mutation)
{
    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-44665544f002";
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256S("f010"), FeeMode::DGB, 100,
                      session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("f011");
    attempt.provider_id = uint256S("f012");
    attempt.created_at = attempt.updated_at = 101;
    BOOST_REQUIRE(store.AddAttempt(request_id, attempt, error));
    BOOST_REQUIRE(store.TransitionSession(
        request_id, SessionState::FAILED, PendingPhase::NONE, {}, 102,
        error));
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, attempt));

    DataStream attempt_key_stream;
    attempt_key_stream << std::make_pair(DBKeys::PAYMASTER_ATTEMPT,
                                         attempt.attempt_id);
    const SerializeData attempt_key{attempt_key_stream.begin(),
                                    attempt_key_stream.end()};
    auto& database = GetMockableDatabase(m_wallet);
    const MockableData valid_state{database.m_records};

    ProviderAttempt outdated{attempt};
    outdated.version = ProviderAttempt::CURRENT_VERSION - 1;
    CDataStream outdated_stream{SER_DISK, CLIENT_VERSION};
    outdated_stream << outdated;
    database.m_records[attempt_key] = SerializeData{outdated_stream.begin(),
                                                    outdated_stream.end()};
    const MockableData outdated_state{database.m_records};
    BOOST_CHECK(!store.PruneFinalSession(request_id, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=ProviderAttempt found=14 expected=15");
    BOOST_CHECK(database.m_records == outdated_state);

    database.m_records = valid_state;
    CDataStream current_but_truncated{SER_DISK, CLIENT_VERSION};
    current_but_truncated << ProviderAttempt::CURRENT_VERSION;
    database.m_records[attempt_key] =
        SerializeData{current_but_truncated.begin(),
                      current_but_truncated.end()};
    const MockableData corrupt_state{database.m_records};
    BOOST_CHECK(!store.PruneFinalSession(request_id, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PRUNE_ATTEMPT_READ_FAILED");
    BOOST_CHECK(database.m_records == corrupt_state);
}

BOOST_AUTO_TEST_CASE(announcement_sequence_is_durable_and_nonzero)
{
    WalletBatch batch{m_wallet.GetDatabase()};
    uint64_t sequence{0};
    BOOST_CHECK(!batch.ReadPaymasterAnnouncementSequence(sequence));
    BOOST_CHECK(!batch.WritePaymasterAnnouncementSequence(0));
    BOOST_REQUIRE(batch.WritePaymasterAnnouncementSequence(1));
    sequence = 0;
    BOOST_REQUIRE(batch.ReadPaymasterAnnouncementSequence(sequence));
    BOOST_CHECK_EQUAL(sequence, 1U);
    BOOST_REQUIRE(batch.WritePaymasterAnnouncementSequence(sequence + 1));
    BOOST_REQUIRE(batch.ReadPaymasterAnnouncementSequence(sequence));
    BOOST_CHECK_EQUAL(sequence, 2U);
}

BOOST_AUTO_TEST_CASE(create_or_join_is_idempotent_and_indexed)
{
    PaymasterStore store{m_wallet};
    PaymentSession created;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440000";

    BOOST_CHECK(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::AUTO, 100,
                                          created, error) == CreatePaymasterSessionResult::CREATED);
    BOOST_CHECK(!created.session_id.IsNull());

    PaymentSession joined;
    BOOST_CHECK(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::AUTO, 101,
                                          joined, error) == CreatePaymasterSessionResult::JOINED);
    BOOST_CHECK_EQUAL(joined.session_id, created.session_id);

    PaymentSession by_session_id;
    BOOST_CHECK(store.GetSessionBySessionId(created.session_id, by_session_id));
    BOOST_CHECK_EQUAL(by_session_id.request_id, request_id);

    PaymentSession conflict;
    BOOST_CHECK(store.CreateOrJoinSession(request_id, uint256S("02"), FeeMode::AUTO, 102,
                                          conflict, error) == CreatePaymasterSessionResult::CONFLICT);
    BOOST_CHECK_EQUAL(error, "PAYMASTER_REQUEST_ID_CONFLICT");
}

BOOST_AUTO_TEST_CASE(client_gross_payment_order_is_durable_and_conflict_checked)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440090";

    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                      session, error) == CreatePaymasterSessionResult::CREATED);
    BOOST_REQUIRE(store.BindClientPaymentOrder(
        request_id, DDCents{5000},
        /*subtract_paymaster_fee_from_amount=*/true,
        /*send_all_spendable_dd=*/true, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK_EQUAL(session.requested_amount.value, 5000);
    BOOST_CHECK(session.subtract_paymaster_fee_from_amount);
    BOOST_CHECK(session.send_all_spendable_dd);

    BOOST_CHECK(store.BindClientPaymentOrder(
        request_id, DDCents{5000}, true, true, error));
    BOOST_CHECK(!store.BindClientPaymentOrder(
        request_id, DDCents{4999}, true, true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
}

BOOST_AUTO_TEST_CASE(reservations_share_standard_wallet_locks)
{
    PaymasterStore store{m_wallet};
    PaymentSession first;
    std::string error;
    constexpr auto first_id = "550e8400-e29b-41d4-a716-446655440001";
    constexpr auto second_id = "550e8400-e29b-41d4-a716-446655440002";
    const COutPoint input{uint256::ONE, 7};

    BOOST_REQUIRE(store.CreateOrJoinSession(first_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            first, error) == CreatePaymasterSessionResult::CREATED);
    BOOST_CHECK(store.ReserveInputs(first_id, {{input, ReservationRole::USER_DD}},
                                    FeeMode::PAYMASTER, 101, error));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(m_wallet.IsLockedCoin(input));
    }
    BOOST_CHECK(store.ReserveInputs(first_id, {{input, ReservationRole::USER_DD}},
                                    FeeMode::PAYMASTER, 102, error));
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, input));

    PaymentSession second;
    BOOST_REQUIRE(store.CreateOrJoinSession(second_id, uint256S("02"), FeeMode::PAYMASTER, 100,
                                            second, error) == CreatePaymasterSessionResult::CREATED);
    BOOST_CHECK(!store.ReserveInputs(second_id, {{input, ReservationRole::USER_DD}},
                                     FeeMode::PAYMASTER, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INPUT_ALREADY_RESERVED");
}

BOOST_AUTO_TEST_CASE(capacity_gated_quote_request_and_user_reservations_are_committed_atomically)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440020";
    constexpr int64_t now = 100;
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256S("20"), FeeMode::PAYMASTER,
                                            now, session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key, recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});
    const COutPoint user_input{uint256S("21"), 1};
    PaymentIntent draft;
    draft.genesis_hash = uint256S("22");
    draft.provider_id = GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()});
    draft.request_id = request_id;
    draft.session_id = session.session_id;
    draft.client_nonce = uint256S("23");
    draft.canonical_request_hash = session.canonical_request_hash;
    draft.requested_fee_mode = session.fee_mode_requested;
    draft.user_dd_inputs = {user_input};
    draft.recipient_script = GetScriptForDestination(recipient_builder.GetOutput());
    draft.recipient_amount = DDCents{1000};
    draft.offer_id = uint256S("24");
    draft.funding_model = FundingModel::SPONSORED;
    draft.sponsorship_scope = SponsorshipScope::PUBLIC;
    draft.policy_hash = uint256S("25");
    draft.expires_at = now + 30;

    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("26");
    attempt.provider_id = draft.provider_id;
    attempt.provider_identity_key = XOnlyPubKey{provider_key.GetPubKey()};
    attempt.client_nonce = draft.client_nonce;
    attempt.unsigned_intent = SerializePaymasterTestObject(draft);
    attempt.created_at = attempt.updated_at = now;
    BOOST_REQUIRE_MESSAGE(store.PreparePaymentIntent(request_id, attempt, now, error), error);

    PaymasterCapacityRequest mismatched_capacity_request;
    mismatched_capacity_request.genesis_hash = draft.genesis_hash;
    mismatched_capacity_request.provider_id = draft.provider_id;
    mismatched_capacity_request.request_id = draft.request_id;
    mismatched_capacity_request.session_id = draft.session_id;
    mismatched_capacity_request.client_nonce = draft.client_nonce;
    mismatched_capacity_request.funding_model = FundingModel::USER_PAID;
    mismatched_capacity_request.requires_carrier = false;
    mismatched_capacity_request.created_at = now;
    mismatched_capacity_request.expires_at = now + 20;
    BOOST_CHECK(!store.PrepareCapacityRequest(
        request_id, attempt.attempt_id,
        SerializePaymasterTestObject(mismatched_capacity_request), now,
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_REQUEST_BINDING_MISMATCH");

    BOOST_REQUIRE_MESSAGE(CommitTestCapacitySnapshot(
                              store, request_id, attempt.attempt_id, draft, provider_key, now + 1,
                              error),
                          error);
    PaymasterQuoteRequest request;
    request.intent = draft;
    request.intent.user_input_proofs = {
        {user_input, std::vector<unsigned char>(64, 1)}};
    const auto request_bytes = SerializePaymasterTestObject(request);
    BOOST_REQUIRE_MESSAGE(store.FinalizePaymentIntent(
                              request_id, attempt.attempt_id, request_bytes, now + 3, error),
                          error);
    BOOST_CHECK(store.FinalizePaymentIntent(
        request_id, attempt.attempt_id, request_bytes, now + 4, error));

    PaymentSession stored_session;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, stored_session));
    BOOST_CHECK(stored_session.state == SessionState::INPUTS_RESERVED);
    BOOST_CHECK(stored_session.user_inputs == std::vector<COutPoint>{user_input});
    BOOST_REQUIRE_EQUAL(stored_session.attempt_ids.size(), 1U);
    BOOST_CHECK(stored_session.attempt_ids.front() == attempt.attempt_id);
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, user_input));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(m_wallet.IsLockedCoin(user_input));
    }
    ProviderAttempt stored_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, stored_attempt));
    BOOST_CHECK(stored_attempt.quote_request == request_bytes);
    BOOST_CHECK(stored_attempt.intent_hash == GetPaymentIntentHash(request.intent));

    // Sequential fallback is safe only before a user PSBT exists. Closing the
    // first attempt retains the exact USER_DD reservation for the next one.
    BOOST_REQUIRE_MESSAGE(store.AbandonClientAttemptForFallback(
                              request_id, attempt.attempt_id, now + 1, error),
                          error);
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, stored_attempt));
    BOOST_CHECK(stored_attempt.state == AttemptState::REJECTED);
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, stored_session));
    BOOST_CHECK(stored_session.state == SessionState::INPUTS_RESERVED);
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, user_input));

    // Explicitly abandoning the whole unsigned session is distinct from
    // fallback: it atomically releases the USER_DD reservation and leaves a
    // durable terminal failure record. This covers a UI cancel or a provider
    // directory becoming empty after the last candidate was rejected.
    auto abandon_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet abandon_wallet{m_node.chain.get(),
                                   "client-abandon-unsigned",
                                   std::move(abandon_database)};
    PaymasterStore abandon_store{abandon_wallet};
    BOOST_REQUIRE_MESSAGE(abandon_store.AbandonUnsignedClientSession(
                              request_id, now + 2, error),
                          error);
    PaymentSession abandoned_session;
    ProviderAttempt abandoned_attempt;
    BOOST_REQUIRE(abandon_store.GetSessionByRequestId(request_id,
                                                       abandoned_session));
    BOOST_REQUIRE(abandon_store.GetAttempt(attempt.attempt_id,
                                            abandoned_attempt));
    BOOST_CHECK(abandoned_session.state == SessionState::FAILED);
    BOOST_CHECK(abandoned_attempt.state == AttemptState::REJECTED);
    BOOST_CHECK(!IsPaymasterInputReserved(abandon_wallet, user_input));
    {
        LOCK(abandon_wallet.cs_wallet);
        BOOST_CHECK(!abandon_wallet.IsLockedCoin(user_input));
    }
    BOOST_CHECK(abandon_store.AbandonUnsignedClientSession(
        request_id, now + 3, error));

    CKey fallback_provider_key;
    fallback_provider_key.MakeNewKey(true);
    draft.provider_id = GetPaymasterId(XOnlyPubKey{fallback_provider_key.GetPubKey()});
    draft.client_nonce = uint256S("27");
    ProviderAttempt fallback;
    fallback.attempt_id = uint256S("28");
    fallback.provider_id = draft.provider_id;
    fallback.provider_identity_key = XOnlyPubKey{fallback_provider_key.GetPubKey()};
    fallback.client_nonce = draft.client_nonce;
    fallback.unsigned_intent = SerializePaymasterTestObject(draft);
    fallback.created_at = fallback.updated_at = now + 2;
    BOOST_REQUIRE_MESSAGE(store.PreparePaymentIntent(
                              request_id, fallback, now + 2, error),
                          error);
    BOOST_REQUIRE_MESSAGE(CommitTestCapacitySnapshot(
                              store, request_id, fallback.attempt_id, draft, fallback_provider_key,
                              now + 3, error),
                          error);
    request.intent = draft;
    request.intent.user_input_proofs = {
        {user_input, std::vector<unsigned char>(64, 1)}};
    const auto fallback_request_bytes = SerializePaymasterTestObject(request);
    BOOST_REQUIRE_MESSAGE(store.FinalizePaymentIntent(
                              request_id, fallback.attempt_id, fallback_request_bytes, now + 5,
                              error),
                          error);
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, stored_session));
    BOOST_REQUIRE_EQUAL(stored_session.attempt_ids.size(), 2U);
    BOOST_CHECK(stored_session.user_inputs == std::vector<COutPoint>{user_input});
}

BOOST_AUTO_TEST_CASE(unsigned_intent_survives_wallet_unlock_boundary_without_reselection)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440029";
    constexpr int64_t now = 200;
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256S("30"), FeeMode::PAYMASTER,
                                            now, session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key, recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});
    const COutPoint user_input{uint256S("31"), 0};
    PaymentIntent draft;
    draft.genesis_hash = uint256S("32");
    draft.provider_id = GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()});
    draft.request_id = request_id;
    draft.session_id = session.session_id;
    draft.client_nonce = uint256S("33");
    draft.canonical_request_hash = session.canonical_request_hash;
    draft.requested_fee_mode = session.fee_mode_requested;
    draft.user_dd_inputs = {user_input};
    draft.recipient_script = GetScriptForDestination(recipient_builder.GetOutput());
    draft.recipient_amount = DDCents{1000};
    draft.offer_id = uint256S("34");
    draft.funding_model = FundingModel::SPONSORED;
    draft.sponsorship_scope = SponsorshipScope::PUBLIC;
    draft.policy_hash = uint256S("35");
    draft.expires_at = now + 30;
    CDataStream encoded_draft{SER_NETWORK, ::PROTOCOL_VERSION};
    encoded_draft << draft;

    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("36");
    attempt.provider_id = draft.provider_id;
    attempt.provider_identity_key = XOnlyPubKey{provider_key.GetPubKey()};
    attempt.client_nonce = draft.client_nonce;
    const auto draft_bytes = MakeUCharSpan(encoded_draft);
    attempt.unsigned_intent.assign(draft_bytes.begin(), draft_bytes.end());
    attempt.created_at = attempt.updated_at = now;
    BOOST_REQUIRE_MESSAGE(store.PreparePaymentIntent(request_id, attempt, now, error), error);
    BOOST_REQUIRE_MESSAGE(CommitTestCapacitySnapshot(
                              store, request_id, attempt.attempt_id, draft, provider_key, now + 1,
                              error),
                          error);
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::AWAITING_WALLET_UNLOCK,
                                          PendingPhase::NONE, {}, now + 3, error));

    PaymasterQuoteRequest signed_request;
    signed_request.intent = draft;
    signed_request.intent.user_input_proofs = {
        {user_input, std::vector<unsigned char>(64, 1)}};
    CDataStream encoded_request{SER_NETWORK, ::PROTOCOL_VERSION};
    encoded_request << signed_request;
    const auto request_bytes_span = MakeUCharSpan(encoded_request);
    const std::vector<unsigned char> request_bytes{
        request_bytes_span.begin(), request_bytes_span.end()};
    BOOST_REQUIRE_MESSAGE(store.FinalizePaymentIntent(
                              request_id, attempt.attempt_id, request_bytes, now + 4, error),
                          error);
    BOOST_CHECK(store.FinalizePaymentIntent(
        request_id, attempt.attempt_id, request_bytes, now + 5, error));

    ProviderAttempt stored;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, stored));
    BOOST_CHECK(stored.unsigned_intent == attempt.unsigned_intent);
    BOOST_CHECK(stored.quote_request == request_bytes);
    BOOST_CHECK(stored.intent_hash == GetPaymentIntentHash(signed_request.intent));
    PaymentSession stored_session;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, stored_session));
    BOOST_CHECK(stored_session.state == SessionState::AWAITING_WALLET_UNLOCK);
    BOOST_CHECK(stored_session.user_inputs == std::vector<COutPoint>{user_input});
}

BOOST_AUTO_TEST_CASE(ambiguous_authorization_is_protected_and_prunes_to_tombstone)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440003";
    const COutPoint input{uint256::ONE, 8};

    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            session, error) == CreatePaymasterSessionResult::CREATED);
    BOOST_REQUIRE(store.ReserveInputs(request_id, {{input, ReservationRole::USER_DD}},
                                      FeeMode::PAYMASTER, 101, error));
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::AUTHORIZED, PendingPhase::NONE,
                                          {}, 102, error));
    auto& database = GetMockableDatabase(m_wallet);
    // Session, wallet flag, and reservation are the only durable writes. Keep
    // the next failure armed to prove RAM flag publication does not perform a
    // second database mutation after commit.
    database.FailWriteAt(3);
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::PENDING_PROVIDER,
                                          PendingPhase::USER_SIGNATURE_SENT, {}, 103, error));

    BOOST_CHECK_EQUAL(database.m_write_count, 3U);
    database.ClearFailureInjection();
    BOOST_CHECK(m_wallet.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    BOOST_CHECK(!store.PruneFinalSession(request_id, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SESSION_NOT_FINAL");

    const uint256 txid = uint256S("03");
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::CONFIRMED,
                                          PendingPhase::NONE, txid, 104, error));
    BOOST_REQUIRE(store.PruneFinalSession(request_id, error));
    BOOST_CHECK(!IsPaymasterInputReserved(m_wallet, input));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_CHECK(!m_wallet.IsLockedCoin(input));
    }

    PaymentSession tombstoned;
    BOOST_CHECK(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 200,
                                          tombstoned, error) == CreatePaymasterSessionResult::FINAL_TOMBSTONE);
    BOOST_CHECK(tombstoned.state == SessionState::CONFIRMED);
    BOOST_CHECK_EQUAL(tombstoned.final_txid, txid);
    BOOST_CHECK(tombstoned.fee_mode_used == FeeMode::PAYMASTER);
    PaymentSession tombstoned_by_session_id;
    BOOST_CHECK(store.GetSessionBySessionId(session.session_id, tombstoned_by_session_id));
    BOOST_CHECK_EQUAL(tombstoned_by_session_id.request_id, request_id);
}

BOOST_AUTO_TEST_CASE(final_transaction_observations_handle_mempool_reorg_and_retention)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440039";
    const COutPoint input{uint256S("39"), 1};
    CMutableTransaction transaction;
    transaction.SetDigiDollarType(::DD_TX_TRANSFER);
    transaction.vin.emplace_back(input);
    transaction.vout.emplace_back(0, CScript{} << OP_TRUE);
    const uint256 txid{CTransaction{transaction}.GetHash()};

    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256S("39"),
                                            FeeMode::PAYMASTER, 100, session, error) ==
                  CreatePaymasterSessionResult::CREATED);
    BOOST_REQUIRE(store.ReserveInputs(request_id, {{input, ReservationRole::USER_DD}},
                                      FeeMode::PAYMASTER, 101, error));
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::AUTHORIZED,
                                          PendingPhase::NONE, {}, 102, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    ProviderAttempt attempt;
    attempt.session_id = session.session_id;
    attempt.attempt_id = uint256S("40");
    attempt.state = AttemptState::FINAL_COMMITTED;
    attempt.final_txid = txid;
    CDataStream encoded{SER_NETWORK, ::PROTOCOL_VERSION};
    encoded << transaction;
    const auto encoded_span = MakeUCharSpan(encoded);
    attempt.final_transaction.assign(encoded_span.begin(), encoded_span.end());
    session.attempt_ids = {attempt.attempt_id};
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
    }
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::PENDING_PROVIDER,
                                          PendingPhase::PROVIDER_SIGNED_KNOWN,
                                          txid, 103, error));

    CMutableTransaction wrong_witness{transaction};
    wrong_witness.vin.front().scriptWitness.stack = {{0x01}};
    BOOST_REQUIRE_EQUAL(CTransaction{wrong_witness}.GetHash(), txid);
    BOOST_CHECK(!store.ReconcileFinalTransaction(
        CTransaction{wrong_witness}, 0, true, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_OBSERVED_FINAL_WITNESS_MISMATCH");
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::PENDING_PROVIDER);

    BOOST_REQUIRE(store.ReconcileFinalTransaction(CTransaction{transaction}, 0, true, 104, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::MEMPOOL);

    BOOST_REQUIRE(store.ReconcileFinalTransaction(CTransaction{transaction}, 1, false, 105, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::CONFIRMED);

    BOOST_REQUIRE(store.ReconcileFinalTransaction(CTransaction{transaction}, 0, false, 106, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(session.pending_phase == PendingPhase::PENDING_NETWORK);
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, input));

    BOOST_REQUIRE(store.ReconcileFinalTransaction(
        CTransaction{transaction}, DEFAULT_REORG_SAFETY_DEPTH, false, 107, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::CONFIRMED);
    BOOST_CHECK_EQUAL(session.final_txid, txid);
    BOOST_CHECK(session.user_inputs.empty());
    BOOST_CHECK(!IsPaymasterInputReserved(m_wallet, input));

    PaymentSession tombstone;
    BOOST_CHECK(store.CreateOrJoinSession(request_id, uint256S("39"),
                                          FeeMode::PAYMASTER, 108,
                                          tombstone, error) ==
                CreatePaymasterSessionResult::FINAL_TOMBSTONE);
    BOOST_CHECK(tombstone.state == SessionState::CONFIRMED);
    BOOST_CHECK_EQUAL(tombstone.final_txid, txid);
}

BOOST_AUTO_TEST_CASE(self_recovery_is_same_input_idempotent_and_reorg_safe)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440040";
    const COutPoint input{uint256S("41"), 2};

    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256S("42"),
                                            FeeMode::PAYMASTER, 100, session, error) ==
                  CreatePaymasterSessionResult::CREATED);
    BOOST_REQUIRE(store.ReserveInputs(request_id, {{input, ReservationRole::USER_DD}},
                                      FeeMode::PAYMASTER, 101, error));
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::AUTHORIZED,
                                          PendingPhase::NONE, {}, 102, error));
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::PENDING_PROVIDER,
                                          PendingPhase::USER_SIGNATURE_SENT, {}, 103, error));

    ClientSafetyPolicy client_policy;
    client_policy.maximum_service_fee_per_transaction = DDCents{10};
    client_policy.maximum_service_fee_per_day = DDCents{20};
    client_policy.updated_at = 100;
    ClientFeeLedger client_ledger;
    client_ledger.accounting_time_high_water = 100;
    const uint256 reserved_commit_key{uint256S("43")};
    const uint256 spent_commit_key{uint256S("44")};
    BOOST_REQUIRE(ReserveClientFee(client_ledger, client_policy,
                                   reserved_commit_key, DDCents{3}, 103,
                                   error));
    BOOST_REQUIRE(ReserveClientFee(client_ledger, client_policy,
                                   spent_commit_key, DDCents{4}, 103,
                                   error));
    BOOST_REQUIRE(SpendClientFee(client_ledger, spent_commit_key, 103, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    ProviderAttempt reserved_attempt;
    reserved_attempt.session_id = session.session_id;
    reserved_attempt.attempt_id = uint256S("45");
    reserved_attempt.commit_key = reserved_commit_key;
    ProviderAttempt spent_attempt;
    spent_attempt.session_id = session.session_id;
    spent_attempt.attempt_id = uint256S("46");
    spent_attempt.commit_key = spent_commit_key;
    session.attempt_ids = {reserved_attempt.attempt_id, spent_attempt.attempt_id};
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(reserved_attempt));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(spent_attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterClientSafetyPolicy(client_policy));
        BOOST_REQUIRE(batch.WritePaymasterClientFeeLedger(client_ledger));
    }

    CKey recovery_key;
    recovery_key.MakeNewKey(true);
    {
        LegacyScriptPubKeyMan* spk_man = m_wallet.GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(m_wallet.cs_wallet, spk_man->cs_KeyStore);
        BOOST_REQUIRE(spk_man->AddKeyPubKey(recovery_key, recovery_key.GetPubKey()));
    }
    const CScript recovery_script = GetScriptForDestination(
        WitnessV0KeyHash{recovery_key.GetPubKey().GetID()});
    CMutableTransaction transaction;
    transaction.SetDigiDollarType(::DD_TX_TRANSFER);
    transaction.vin.emplace_back(input);
    transaction.vout.emplace_back(0, recovery_script);
    CDataStream encoded{SER_NETWORK, ::PROTOCOL_VERSION};
    encoded << transaction;
    const auto encoded_span = MakeUCharSpan(encoded);

    SelfRecoveryRecord recovery;
    recovery.request_id = request_id;
    recovery.session_id = session.session_id;
    recovery.user_inputs = {input};
    recovery.recovery_txid = CTransaction{transaction}.GetHash();
    recovery.final_transaction.assign(encoded_span.begin(), encoded_span.end());
    recovery.raw_transaction_hash = Hash(recovery.final_transaction);
    recovery.created_at = 104;
    BOOST_REQUIRE_MESSAGE(store.CommitSelfRecovery(request_id, recovery, error), error);
    BOOST_CHECK(store.CommitSelfRecovery(request_id, recovery, error));

    SelfRecoveryRecord persisted;
    BOOST_REQUIRE(store.GetSelfRecovery(request_id, persisted));
    BOOST_CHECK_EQUAL(persisted.recovery_txid, recovery.recovery_txid);
    BOOST_CHECK(persisted.final_transaction == recovery.final_transaction);
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK_EQUAL(session.recovery_txid, recovery.recovery_txid);
    BOOST_CHECK(session.pending_phase == PendingPhase::PENDING_NETWORK);

    SelfRecoveryRecord conflict = recovery;
    conflict.created_at = 105;
    conflict.final_transaction.push_back(0);
    conflict.raw_transaction_hash = Hash(conflict.final_transaction);
    BOOST_CHECK(!store.CommitSelfRecovery(request_id, conflict, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_SELF_RECOVERY");

    BOOST_REQUIRE(store.ReconcileFinalTransaction(CTransaction{transaction}, 0, true,
                                                  106, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(session.pending_phase == PendingPhase::CANCEL_MEMPOOL);

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_confirmed_recovery = database.m_records;
    for (size_t failing_write = 0; failing_write < 2; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.ReconcileFinalTransaction(
            CTransaction{transaction}, 1, false, 107, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_confirmed_recovery);
    }
    database.FailCommit();
    BOOST_CHECK(!store.ReconcileFinalTransaction(
        CTransaction{transaction}, 1, false, 107, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_confirmed_recovery);

    BOOST_REQUIRE(store.ReconcileFinalTransaction(CTransaction{transaction}, 1,
                                                  false, 107, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::CANCELED_SAFE);
    {
        LOCK(m_wallet.cs_wallet);
        ClientFeeLedger persisted_ledger;
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .ReadPaymasterClientFeeLedger(persisted_ledger));
        const auto reserved_entry = std::find_if(
            persisted_ledger.reservations.begin(),
            persisted_ledger.reservations.end(),
            [&](const ClientFeeReservation& reservation) {
                return reservation.commit_key == reserved_commit_key;
            });
        const auto spent_entry = std::find_if(
            persisted_ledger.reservations.begin(),
            persisted_ledger.reservations.end(),
            [&](const ClientFeeReservation& reservation) {
                return reservation.commit_key == spent_commit_key;
            });
        BOOST_REQUIRE(reserved_entry != persisted_ledger.reservations.end());
        BOOST_REQUIRE(spent_entry != persisted_ledger.reservations.end());
        BOOST_CHECK(reserved_entry->state == BudgetReservationState::RELEASED);
        BOOST_CHECK(spent_entry->state == BudgetReservationState::SPENT);
    }
    const MockableData after_confirmed_recovery = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.ReconcileFinalTransaction(
        CTransaction{transaction}, 1, false, 108, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == after_confirmed_recovery);

    BOOST_REQUIRE(store.ReconcileFinalTransaction(CTransaction{transaction}, 0, false,
                                                  109, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(session.pending_phase == PendingPhase::PENDING_NETWORK);
    BOOST_CHECK(store.GetSelfRecovery(request_id, persisted));
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, input));

    BOOST_REQUIRE(store.ReconcileFinalTransaction(
        CTransaction{transaction}, DEFAULT_REORG_SAFETY_DEPTH, false, 110, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(session.state == SessionState::CANCELED_SAFE);
    BOOST_CHECK_EQUAL(session.recovery_txid, recovery.recovery_txid);
    BOOST_CHECK(session.user_inputs.empty());
    BOOST_CHECK(!store.GetSelfRecovery(request_id, persisted));
    BOOST_CHECK(!IsPaymasterInputReserved(m_wallet, input));

    PaymentSession tombstone;
    BOOST_CHECK(store.CreateOrJoinSession(request_id, uint256S("42"),
                                          FeeMode::PAYMASTER, 111,
                                          tombstone, error) ==
                CreatePaymasterSessionResult::FINAL_TOMBSTONE);
    BOOST_CHECK(tombstone.state == SessionState::CANCELED_SAFE);
    BOOST_CHECK_EQUAL(tombstone.recovery_txid, recovery.recovery_txid);
}

BOOST_AUTO_TEST_CASE(client_attempt_artifacts_and_authorization_are_append_only_and_atomic)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440004";
    CMutableTransaction user_prev;
    user_prev.vin.emplace_back(COutPoint{uint256::ONE, 0});
    user_prev.vout.emplace_back(10, CScript{} << OP_TRUE);
    const COutPoint user_input{CTransaction{user_prev}.GetHash(), 0};

    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            session, error) == CreatePaymasterSessionResult::CREATED);

    ProviderAttempt candidate;
    candidate.attempt_id = uint256S("10");
    CKey provider_identity_key;
    provider_identity_key.MakeNewKey(true);
    ProviderIdentityRecord provider_identity;
    provider_identity.identity_key = XOnlyPubKey{provider_identity_key.GetPubKey()};
    provider_identity.provider_id = GetPaymasterId(provider_identity.identity_key);
    TaprootBuilder identity_builder;
    identity_builder.Finalize(provider_identity.identity_key);
    provider_identity.identity_script = GetScriptForDestination(identity_builder.GetOutput());
    provider_identity.display_name = "Provider";
    provider_identity.created_at = 100;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterIdentity(provider_identity));
    }
    candidate.provider_id = provider_identity.provider_id;
    candidate.provider_identity_key = provider_identity.identity_key;
    candidate.client_nonce = uint256S("13");
    candidate.privacy_profile = PrivacyProfile::HIGH;
    candidate.created_at = candidate.updated_at = 103;

    CMutableTransaction provider_prev;
    provider_prev.vin.emplace_back(COutPoint{uint256S("02"), 0});
    CKey provider_pool_key;
    provider_pool_key.MakeNewKey(true);
    TaprootBuilder provider_pool_builder;
    provider_pool_builder.Finalize(XOnlyPubKey{provider_pool_key.GetPubKey()});
    provider_prev.vout.emplace_back(
        20, GetScriptForDestination(provider_pool_builder.GetOutput()));
    const VerifiedDGBInput provider_capacity_input{
        COutPoint{CTransaction{provider_prev}.GetHash(), 0}, provider_prev,
        DGBSatoshis{20}};

    CKey recipient_key;
    recipient_key.MakeNewKey(true);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});
    PaymentIntent draft;
    draft.genesis_hash = uint256S("11");
    draft.provider_id = provider_identity.provider_id;
    draft.request_id = request_id;
    draft.session_id = session.session_id;
    draft.client_nonce = candidate.client_nonce;
    draft.canonical_request_hash = session.canonical_request_hash;
    draft.requested_fee_mode = session.fee_mode_requested;
    draft.privacy_profile = candidate.privacy_profile;
    draft.user_dd_inputs = {user_input};
    draft.recipient_script = GetScriptForDestination(recipient_builder.GetOutput());
    draft.recipient_amount = DDCents{100};
    draft.offer_id = uint256S("12");
    draft.funding_model = FundingModel::SPONSORED;
    draft.sponsorship_scope = SponsorshipScope::PUBLIC;
    draft.policy_hash = uint256S("14");
    draft.expires_at = 150;
    candidate.unsigned_intent = SerializePaymasterTestObject(draft);
    BOOST_REQUIRE_MESSAGE(store.PreparePaymentIntent(
                              request_id, candidate, 103, error),
                          error);
    BOOST_REQUIRE_MESSAGE(CommitTestCapacitySnapshot(
                              store, request_id, candidate.attempt_id, draft,
                              provider_identity_key, 104, error, &provider_capacity_input),
                          error);

    PaymasterQuoteRequest signed_request;
    signed_request.intent = draft;
    signed_request.intent.user_input_proofs = {
        {user_input, std::vector<unsigned char>(64, 1)}};
    const auto signed_request_bytes = SerializePaymasterTestObject(signed_request);
    BOOST_REQUIRE_MESSAGE(store.FinalizePaymentIntent(
                              request_id, candidate.attempt_id, signed_request_bytes, 106, error),
                          error);

    ProviderAttempt quoted;
    BOOST_REQUIRE(store.GetAttempt(candidate.attempt_id, quoted));
    BOOST_CHECK(quoted.privacy_profile == PrivacyProfile::HIGH);

    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{CTransaction{user_prev}.GetHash(), 0});
    transaction.vin.emplace_back(COutPoint{CTransaction{provider_prev}.GetHash(), 0});
    transaction.vout.emplace_back(29, CScript{} << OP_TRUE);
    const std::vector<CollaborativeInput> inputs{
        {transaction.vin[0].prevout, MakeTransactionRef(user_prev), InputRole::USER_DD},
        {transaction.vin[1].prevout, MakeTransactionRef(provider_prev), InputRole::PROVIDER_DGB},
    };
    CollaborativePSBTTemplate trusted_template;
    BOOST_REQUIRE(CreateCollaborativePSBTTemplate(transaction, inputs, trusted_template, error));
    CDataStream serialized_transaction{SER_NETWORK, ::PROTOCOL_VERSION};
    serialized_transaction << transaction;
    const auto transaction_bytes = MakeUCharSpan(serialized_transaction);
    CDataStream serialized_psbt{SER_NETWORK, ::PROTOCOL_VERSION};
    serialized_psbt << trusted_template.psbt;
    const auto psbt_bytes = MakeUCharSpan(serialized_psbt);

    quoted.state = AttemptState::QUOTED;
    quoted.quote_id = uint256S("15");
    quoted.unsigned_txid = CTransaction{transaction}.GetHash();
    PaymasterQuoteResponse fee_quote;
    fee_quote.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    fee_quote.request_id = request_id;
    fee_quote.session_id = session.session_id;
    fee_quote.quote.genesis_hash = signed_request.intent.genesis_hash;
    fee_quote.quote.provider_id = quoted.provider_id;
    fee_quote.quote.quote_id = quoted.quote_id;
    fee_quote.quote.intent_hash = GetPaymentIntentHash(signed_request.intent);
    fee_quote.quote.offer_id = signed_request.intent.offer_id;
    fee_quote.quote.policy_hash = signed_request.intent.policy_hash;
    fee_quote.quote.funding_model = signed_request.intent.funding_model;
    fee_quote.quote.sponsorship_scope = signed_request.intent.sponsorship_scope;
    fee_quote.quote.service_fee = DDCents{0};
    fee_quote.quote.reserved_dgb_inputs = {provider_capacity_input};
    fee_quote.quote.network_fee = DGBSatoshis{1};
    fee_quote.quote.created_at = 106;
    fee_quote.quote.expires_at = 120;
    fee_quote.quote.retry_until = 86460;
    fee_quote.quote.unsigned_transaction = transaction;
    fee_quote.quote.unsigned_txid = quoted.unsigned_txid;
    fee_quote.quote.template_commitment = GetCollaborativeTemplateCommitment(
        signed_request.intent, fee_quote.quote, trusted_template);
    fee_quote.quote.identity_signature.resize(64);
    BOOST_REQUIRE(provider_identity_key.SignSchnorr(
        GetPaymasterQuoteSignatureHash(fee_quote.quote),
        fee_quote.quote.identity_signature, nullptr, uint256{}));
    quoted.intent_hash = fee_quote.quote.intent_hash;
    quoted.template_commitment = fee_quote.quote.template_commitment;
    quoted.commit_key = GetPaymasterCommitKey(quoted.provider_id, quoted.client_nonce,
                                              quoted.intent_hash, quoted.quote_id,
                                              quoted.template_commitment);
    quoted.signed_quote = SerializePaymasterTestObject(fee_quote);
    quoted.unsigned_transaction.assign(transaction_bytes.begin(), transaction_bytes.end());
    quoted.unsigned_psbt.assign(psbt_bytes.begin(), psbt_bytes.end());
    quoted.input_roles = {ReservationRole::USER_DD, ReservationRole::PROVIDER_DGB};
    quoted.quote_expires_at = fee_quote.quote.expires_at;
    quoted.retry_until = fee_quote.quote.retry_until;
    quoted.updated_at = 106;
    BOOST_REQUIRE_MESSAGE(BuildClientAuthorizationManifest(
                              signed_request.intent, fee_quote.quote, quoted.capacity_snapshot,
                              trusted_template, DDCents{0}, quoted.client_manifest, error),
                          error);
    bool quote_equivocation{false};
    BOOST_REQUIRE_MESSAGE(store.StageQuoteResponseClaimCandidate(
                              quoted.attempt_id, quoted.signed_quote, 106,
                              quote_equivocation, error),
                          error);
    BOOST_CHECK(!quote_equivocation);
    quoted.quote_response_claim_candidate = quoted.signed_quote;
    ProviderAttempt invalid_roles = quoted;
    invalid_roles.input_roles = {ReservationRole::USER_DD, ReservationRole::USER_DGB};
    BOOST_CHECK(!store.UpdateAttempt(request_id, invalid_roles, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_TEMPLATE_INPUT_ROLE");
    ProviderAttempt invalid_commit_key = quoted;
    invalid_commit_key.commit_key = uint256S("12");
    BOOST_CHECK(!store.UpdateAttempt(request_id, invalid_commit_key, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_COMMIT_KEY");
    ProviderAttempt invalid_privacy = quoted;
    invalid_privacy.privacy_profile = PrivacyProfile::STANDARD;
    BOOST_CHECK(!store.UpdateAttempt(request_id, invalid_privacy, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_ATTEMPT_TRANSITION");
    BOOST_REQUIRE(store.UpdateAttempt(request_id, quoted, error));
    BOOST_REQUIRE(store.TransitionSession(
        request_id, SessionState::AUTHORIZED, PendingPhase::NONE, {}, 106,
        error));

    // Explicit client approval and the corresponding fee budget must be one
    // durable pre-signature operation. A zero-fee quote is still recorded so
    // retries, expiry, and fallback exercise the same accounting path as a
    // non-zero service fee without weakening this transaction fixture.
    ClientSafetyPolicy quote_client_policy;
    quote_client_policy.maximum_service_fee_per_transaction = DDCents{0};
    quote_client_policy.maximum_service_fee_per_day = DDCents{0};
    quote_client_policy.updated_at = 106;
    ClientFeeLedger quote_client_ledger;
    quote_client_ledger.accounting_time_high_water = 106;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterClientSafetyPolicy(
            quote_client_policy));
        BOOST_REQUIRE(batch.WritePaymasterClientFeeLedger(
            quote_client_ledger));
    }

    ProviderAttempt unapproved_signature = quoted;
    unapproved_signature.state = AttemptState::USER_SIGNED;
    unapproved_signature.user_signed_psbt = {7, 8};
    unapproved_signature.updated_at = 107;
    BOOST_CHECK(!store.UpdateAttempt(
        request_id, unapproved_signature, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED");

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_client_acceptance = database.m_records;
    BOOST_CHECK(!store.AcceptClientAuthorization(
        request_id, quoted.attempt_id, uint256S("16"), 106, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
    BOOST_CHECK(database.m_records == before_client_acceptance);
    BOOST_CHECK(!store.AcceptClientAuthorization(
        request_id, quoted.attempt_id, quoted.client_manifest.manifest_id,
        121, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED");
    BOOST_CHECK(database.m_records == before_client_acceptance);

    ClientFeeLedger orphan_fee_ledger{quote_client_ledger};
    orphan_fee_ledger.reservations.push_back(
        {ClientFeeReservation::CURRENT_VERSION, quoted.commit_key,
         fee_quote.quote.service_fee, BudgetReservationState::RESERVED,
         106, 106});
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterClientFeeLedger(
                              orphan_fee_ledger));
    }
    const MockableData with_orphan_fee = database.m_records;
    BOOST_CHECK(!store.AcceptClientAuthorization(
        request_id, quoted.attempt_id,
        quoted.client_manifest.manifest_id, 106, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_ORPHAN");
    BOOST_CHECK(database.m_records == with_orphan_fee);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterClientFeeLedger(
                              quote_client_ledger));
    }
    BOOST_CHECK(database.m_records == before_client_acceptance);

    for (size_t failing_write = 0; failing_write < 3; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.AcceptClientAuthorization(
            request_id, quoted.attempt_id,
            quoted.client_manifest.manifest_id, 106, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_client_acceptance);
    }
    database.FailCommit();
    BOOST_CHECK(!store.AcceptClientAuthorization(
        request_id, quoted.attempt_id,
        quoted.client_manifest.manifest_id, 106, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_client_acceptance);

    BOOST_REQUIRE_MESSAGE(store.AcceptClientAuthorization(
                              request_id, quoted.attempt_id,
                              quoted.client_manifest.manifest_id, 106, error),
                          error);
    const MockableData after_client_acceptance = database.m_records;
    BOOST_CHECK(store.AcceptClientAuthorization(
        request_id, quoted.attempt_id, quoted.client_manifest.manifest_id,
        105, error));
    BOOST_CHECK(database.m_records == after_client_acceptance);

    ProviderAttempt accepted_quote;
    BOOST_REQUIRE(store.GetAttempt(quoted.attempt_id, accepted_quote));
    BOOST_CHECK_EQUAL(accepted_quote.accepted_client_manifest_id,
                      quoted.client_manifest.manifest_id);
    BOOST_CHECK_EQUAL(accepted_quote.client_manifest_accepted_at, 106);

    ClientFeeLedger accepted_fee_ledger;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .ReadPaymasterClientFeeLedger(
                              accepted_fee_ledger));
    }
    const auto accepted_fee = std::find_if(
        accepted_fee_ledger.reservations.begin(),
        accepted_fee_ledger.reservations.end(),
        [&](const ClientFeeReservation& reservation) {
            return reservation.commit_key == quoted.commit_key;
        });
    BOOST_REQUIRE(accepted_fee != accepted_fee_ledger.reservations.end());
    BOOST_CHECK(accepted_fee->state == BudgetReservationState::RESERVED);

    // An unsigned accepted quote may be abandoned for another provider. The
    // attempt/session/fee update is atomic and deliberately retains the user
    // input reservation for the next candidate.
    auto fallback_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet fallback_wallet{m_node.chain.get(),
                                    "client-fallback-preauthorization",
                                    std::move(fallback_database)};
    PaymasterStore fallback_store{fallback_wallet};
    BOOST_REQUIRE_MESSAGE(fallback_store.AbandonClientAttemptForFallback(
                              request_id, quoted.attempt_id, 107, error),
                          error);
    ProviderAttempt fallback_attempt;
    PaymentSession fallback_session;
    BOOST_REQUIRE(fallback_store.GetAttempt(quoted.attempt_id,
                                            fallback_attempt));
    BOOST_REQUIRE(fallback_store.GetSessionByRequestId(request_id,
                                                       fallback_session));
    BOOST_CHECK(fallback_attempt.state == AttemptState::REJECTED);
    BOOST_CHECK(fallback_session.state == SessionState::INPUTS_RESERVED);
    BOOST_CHECK(IsPaymasterInputReserved(fallback_wallet, user_input));
    ClientFeeLedger fallback_fee_ledger;
    {
        LOCK(fallback_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{fallback_wallet.GetDatabase()}
                          .ReadPaymasterClientFeeLedger(
                              fallback_fee_ledger));
    }
    const auto fallback_fee = std::find_if(
        fallback_fee_ledger.reservations.begin(),
        fallback_fee_ledger.reservations.end(),
        [&](const ClientFeeReservation& reservation) {
            return reservation.commit_key == quoted.commit_key;
        });
    BOOST_REQUIRE(fallback_fee != fallback_fee_ledger.reservations.end());
    BOOST_CHECK(fallback_fee->state == BudgetReservationState::RELEASED);

    // Expiry has the same all-or-nothing guarantee and, when no other live
    // attempt remains, releases the still-unsigned DD inputs as well.
    auto expiry_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet expiry_wallet{m_node.chain.get(),
                                  "client-quote-expiry",
                                  std::move(expiry_database)};
    PaymasterStore expiry_store{expiry_wallet};
    auto& expiry_mock = GetMockableDatabase(expiry_wallet);
    const MockableData before_expiry = expiry_mock.m_records;
    size_t expired_quotes{0};
    expiry_mock.FailWriteAt(2);
    BOOST_CHECK(!expiry_store.ExpireProviderQuotes(
        121, expired_quotes, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    expiry_mock.ClearFailureInjection();
    BOOST_CHECK(expiry_mock.m_records == before_expiry);
    BOOST_CHECK_EQUAL(expired_quotes, 0U);
    BOOST_REQUIRE_MESSAGE(expiry_store.ExpireProviderQuotes(
                              121, expired_quotes, error),
                          error);
    BOOST_CHECK_EQUAL(expired_quotes, 1U);
    ProviderAttempt expired_attempt;
    PaymentSession expired_session;
    BOOST_REQUIRE(expiry_store.GetAttempt(quoted.attempt_id,
                                          expired_attempt));
    BOOST_REQUIRE(expiry_store.GetSessionByRequestId(request_id,
                                                     expired_session));
    BOOST_CHECK(expired_attempt.state == AttemptState::QUOTE_EXPIRED);
    BOOST_CHECK(expired_session.state == SessionState::FAILED);
    BOOST_CHECK(!IsPaymasterInputReserved(expiry_wallet, user_input));
    ClientFeeLedger expired_fee_ledger;
    {
        LOCK(expiry_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{expiry_wallet.GetDatabase()}
                          .ReadPaymasterClientFeeLedger(
                              expired_fee_ledger));
    }
    const auto expired_fee = std::find_if(
        expired_fee_ledger.reservations.begin(),
        expired_fee_ledger.reservations.end(),
        [&](const ClientFeeReservation& reservation) {
            return reservation.commit_key == quoted.commit_key;
        });
    BOOST_REQUIRE(expired_fee != expired_fee_ledger.reservations.end());
    BOOST_CHECK(expired_fee->state == BudgetReservationState::RELEASED);
    BOOST_REQUIRE(expiry_store.ExpireProviderQuotes(
        122, expired_quotes, error));
    BOOST_CHECK_EQUAL(expired_quotes, 0U);

    // The attempt keeps its embedded proof for forensic/recovery purposes,
    // while the global snapshot and outpoint indexes are bounded after the
    // replay/evidence horizon. A failed compaction must not leave half-erased
    // indexes behind.
    const MockableData before_capacity_compaction = expiry_mock.m_records;
    size_t compacted_snapshots{0};
    // Compaction is erase-only, so inject the fallible transaction commit
    // rather than a write that this path intentionally never performs.
    expiry_mock.FailCommit();
    BOOST_CHECK(!expiry_store.CompactClientCapacitySnapshots(
        86525, compacted_snapshots, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    expiry_mock.ClearFailureInjection();
    BOOST_CHECK(expiry_mock.m_records == before_capacity_compaction);
    BOOST_CHECK_EQUAL(compacted_snapshots, 0U);
    BOOST_REQUIRE_MESSAGE(expiry_store.CompactClientCapacitySnapshots(
                              86525, compacted_snapshots, error),
                          error);
    BOOST_CHECK_EQUAL(compacted_snapshots, 1U);
    {
        LOCK(expiry_wallet.cs_wallet);
        WalletBatch batch{expiry_wallet.GetDatabase()};
        ValidatedCapacitySnapshot removed_snapshot;
        uint256 removed_slot;
        CapacityResourceBinding removed_resource;
        BOOST_CHECK(!batch.ReadPaymasterCapacitySnapshot(
            quoted.capacity_snapshot.snapshot_id, removed_snapshot));
        BOOST_CHECK(!batch.ReadPaymasterCapacitySlot(
            quoted.capacity_snapshot.resource_commitment, removed_slot));
        BOOST_CHECK(!batch.ReadPaymasterCapacityResource(
            quoted.provider_id, provider_capacity_input.outpoint,
            removed_resource));
    }

    // A stale generic update cannot clear the dedicated approval fields.
    ProviderAttempt stale_quote = quoted;
    stale_quote.updated_at = 107;
    BOOST_CHECK(!store.UpdateAttempt(request_id, stale_quote, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_CONFLICT");
    BOOST_CHECK(database.m_records == after_client_acceptance);

    auto approval_restart = DuplicateMockDatabase(m_wallet.GetDatabase());
    WalletBatch approval_batch{*approval_restart};
    ProviderAttempt restarted_approved_quote;
    BOOST_REQUIRE(approval_batch.ReadPaymasterAttempt(
        quoted.attempt_id, restarted_approved_quote));
    BOOST_CHECK_EQUAL(restarted_approved_quote.accepted_client_manifest_id,
                      quoted.client_manifest.manifest_id);
    BOOST_CHECK_EQUAL(restarted_approved_quote.client_manifest_accepted_at,
                      106);
    quoted = std::move(accepted_quote);

    ProviderAttempt by_template;
    BOOST_REQUIRE(store.GetAttemptByTemplateCommitment(quoted.template_commitment, by_template));
    BOOST_CHECK_EQUAL(by_template.attempt_id, quoted.attempt_id);
    ProviderAttempt by_unsigned_txid;
    BOOST_REQUIRE(store.GetAttemptByUnsignedTxid(quoted.unsigned_txid, by_unsigned_txid));
    BOOST_CHECK_EQUAL(by_unsigned_txid.attempt_id, quoted.attempt_id);

    ProviderAttempt tampered = quoted;
    tampered.state = AttemptState::USER_SIGNED;
    tampered.signed_quote = {9};
    tampered.user_signed_psbt = {7, 8};
    tampered.updated_at = 107;
    BOOST_CHECK(!store.UpdateAttempt(request_id, tampered, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_CONFLICT");

    ProviderAttempt user_signed = quoted;
    user_signed.state = AttemptState::USER_SIGNED;
    user_signed.user_signed_psbt = {7, 8};
    user_signed.updated_at = 107;
    // Attempt, reservation, wallet flag, and session are committed together.
    // A fifth write would be the forbidden post-commit SetWalletFlag path.
    database.FailWriteAt(4);
    BOOST_REQUIRE(store.UpdateAttempt(request_id, user_signed, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 4U);
    database.ClearFailureInjection();
    BOOST_CHECK(m_wallet.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    BOOST_CHECK(!store.AbandonClientAttemptForFallback(
        request_id, candidate.attempt_id, 107, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FALLBACK_AUTHORIZATION_MAY_EXIST");
    BOOST_CHECK(!store.AbandonUnsignedClientSession(request_id, 107, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_UNSIGNED_ABANDON_AUTHORIZATION_MAY_EXIST");
    PaymentSession pending;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, pending));
    BOOST_CHECK(pending.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(pending.pending_phase == PendingPhase::USER_SIGNATURE_SENT);
}

BOOST_AUTO_TEST_CASE(unsigned_quote_cancellation_atomically_releases_provider_pool)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440005";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            session, error) == CreatePaymasterSessionResult::CREATED);
    ProviderAttempt candidate;
    candidate.attempt_id = uint256S("31");
    candidate.provider_id = uint256S("32");
    candidate.created_at = candidate.updated_at = 101;
    BOOST_REQUIRE(store.AddAttempt(request_id, candidate, error));

    ProviderAttempt quoted = candidate;
    quoted.session_id = session.session_id;
    quoted.state = AttemptState::QUOTED;
    quoted.commit_key = uint256S("33");
    quoted.updated_at = 102;
    CKey pool_key;
    pool_key.MakeNewKey(true);
    TaprootBuilder pool_builder;
    pool_builder.Finalize(XOnlyPubKey{pool_key.GetPubKey()});
    ProviderPoolEntry pool_entry;
    pool_entry.outpoint = COutPoint{uint256S("34"), 0};
    pool_entry.purpose = PoolPurpose::OPERATIONAL;
    pool_entry.asset = PoolAsset::DGB;
    pool_entry.state = PoolEntryState::RESERVED;
    pool_entry.script_pub_key = GetScriptForDestination(pool_builder.GetOutput());
    pool_entry.dgb_value = DGBSatoshis{1000};
    pool_entry.confirmation_height = 1;
    pool_entry.reservation_id = quoted.commit_key;
    pool_entry.updated_at = 102;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(quoted));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({pool_entry}));
    }

    ProviderAttempt canceled;
    BOOST_REQUIRE(store.CancelProviderQuote(quoted.attempt_id, 103, canceled, error));
    BOOST_CHECK(canceled.state == AttemptState::REJECTED);
    BOOST_CHECK(store.CancelProviderQuote(quoted.attempt_id, 104, canceled, error));
    std::vector<ProviderPoolEntry> pool;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterProviderPool(pool));
    }
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
    BOOST_CHECK(pool.front().reservation_id.IsNull());
    BOOST_CHECK_EQUAL(pool.front().updated_at, 103);

    ProviderAttempt signed_attempt = candidate;
    signed_attempt.attempt_id = uint256S("35");
    signed_attempt.created_at = signed_attempt.updated_at = 105;
    BOOST_REQUIRE(store.AddAttempt(request_id, signed_attempt, error));
    signed_attempt.session_id = session.session_id;
    signed_attempt.state = AttemptState::USER_SIGNED;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterAttempt(signed_attempt));
    }
    BOOST_CHECK(!store.CancelProviderQuote(signed_attempt.attempt_id, 106, canceled, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_AUTHORIZATION_MAY_EXIST");
}

BOOST_AUTO_TEST_CASE(expired_provider_quotes_release_pool_and_budget_atomically)
{
    PaymasterStore store{m_wallet};
    std::string error;

    ProviderPolicy advertised;
    advertised.funding_models = FUNDING_MODEL_USER_PAID;
    advertised.sponsorship_scope = SponsorshipScope::PUBLIC;
    advertised.fee_rate_bps = 10;
    advertised.min_payment = DDCents{100};
    advertised.max_payment = DDCents{10000};
    advertised.quote_ttl = 30;
    advertised.maximum_network_fee = DGBSatoshis{100};

    ProviderSafetyPolicy safety;
    safety.user_paid.maximum_network_fee_per_transaction = DGBSatoshis{100};
    safety.user_paid.maximum_reserved_network_fee = DGBSatoshis{1000};
    safety.user_paid.maximum_network_fee_per_hour = DGBSatoshis{1000};
    safety.user_paid.maximum_network_fee_per_day = DGBSatoshis{10000};
    safety.user_paid.maximum_completed_per_hour = 10;
    safety.user_paid.maximum_completed_per_day = 100;
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 5;
    safety.maximum_active_quotes_per_recipient = 5;
    safety.maximum_quote_requests_per_netgroup_per_minute = 10;
    safety.updated_at = 100;

    ProviderBudgetLedger ledger;
    ledger.recipient_bucket_secret = uint256S("70");
    ledger.accounting_time_high_water = 100;

    CKey pool_key;
    pool_key.MakeNewKey(true);
    TaprootBuilder pool_builder;
    pool_builder.Finalize(XOnlyPubKey{pool_key.GetPubKey()});
    const CScript pool_script = GetScriptForDestination(pool_builder.GetOutput());

    struct QuoteFixture {
        PaymentSession session;
        ProviderAttempt attempt;
        ProviderPoolEntry pool_entry;
    };
    const auto make_quote = [&](const std::string& request_id,
                                const uint256& session_id,
                                const uint256& attempt_id,
                                const uint256& commit_key,
                                const uint256& pool_txid) {
        QuoteFixture fixture;
        fixture.session.request_id = request_id;
        fixture.session.session_id = session_id;
        fixture.session.canonical_request_hash = Hash(request_id);
        fixture.session.fee_mode_requested = FeeMode::PAYMASTER;
        fixture.session.fee_mode_used = FeeMode::PAYMASTER;
        fixture.session.state = SessionState::INPUTS_RESERVED;
        fixture.session.pending_phase = PendingPhase::NONE;
        fixture.session.attempt_ids = {attempt_id};
        fixture.session.created_at = 100;
        fixture.session.updated_at = 101;
        fixture.session.provider_side = true;

        fixture.attempt.session_id = session_id;
        fixture.attempt.attempt_id = attempt_id;
        fixture.attempt.provider_id = uint256S("71");
        fixture.attempt.state = AttemptState::QUOTED;
        fixture.attempt.commit_key = commit_key;
        fixture.attempt.quote_expires_at = 110;
        fixture.attempt.retry_until = 200;
        fixture.attempt.created_at = fixture.attempt.updated_at = 101;

        fixture.pool_entry.outpoint = COutPoint{pool_txid, 0};
        fixture.pool_entry.purpose = PoolPurpose::OPERATIONAL;
        fixture.pool_entry.asset = PoolAsset::DGB;
        fixture.pool_entry.state = PoolEntryState::RESERVED;
        fixture.pool_entry.script_pub_key = pool_script;
        fixture.pool_entry.dgb_value = DGBSatoshis{1000};
        fixture.pool_entry.confirmation_height = 1;
        fixture.pool_entry.reservation_id = commit_key;
        fixture.pool_entry.updated_at = 101;
        return fixture;
    };

    const QuoteFixture expired = make_quote(
        "550e8400-e29b-41d4-a716-446655440101", uint256S("72"),
        uint256S("73"), uint256S("74"), uint256S("75"));
    QuoteFixture authorized = make_quote(
        "550e8400-e29b-41d4-a716-446655440102", uint256S("76"),
        uint256S("77"), uint256S("78"), uint256S("79"));
    QuoteFixture state_protected = make_quote(
        "550e8400-e29b-41d4-a716-446655440103", uint256S("7a"),
        uint256S("7b"), uint256S("7c"), uint256S("7d"));
    state_protected.session.state = SessionState::AUTHORIZED;

    const uint256 recipient_bucket = uint256S("7e");
    const uint256 netgroup_bucket = uint256S("7f");
    for (const uint256& commit_key : {expired.attempt.commit_key,
                                      authorized.attempt.commit_key,
                                      state_protected.attempt.commit_key}) {
        BOOST_REQUIRE_MESSAGE(
            ReserveProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                  SponsorshipScope::PUBLIC, commit_key,
                                  DGBSatoshis{50}, recipient_bucket, 101, error,
                                  netgroup_bucket),
            error);
    }

    UserAuthorizationRecord durable_authorization;
    durable_authorization.commit_key = authorized.attempt.commit_key;
    durable_authorization.attempt_id = authorized.attempt.attempt_id;
    durable_authorization.canonical_psbt_hash = uint256S("80");
    durable_authorization.accepted_at = 102;
    durable_authorization.retry_until = 200;

    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterPolicy(advertised));
        BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(safety));
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(ledger));
        const QuoteFixture* fixtures[]{&expired, &authorized, &state_protected};
        for (const QuoteFixture* fixture : fixtures) {
            BOOST_REQUIRE(batch.WritePaymasterSession(fixture->session));
            BOOST_REQUIRE(batch.WritePaymasterSessionId(
                fixture->session.session_id, fixture->session.request_id));
            BOOST_REQUIRE(batch.WritePaymasterAttempt(fixture->attempt));
        }
        BOOST_REQUIRE(batch.WritePaymasterUserAuthorization(durable_authorization));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool(
            {expired.pool_entry, authorized.pool_entry, state_protected.pool_entry}));
    }

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_expiry = database.m_records;
    size_t expired_count{0};
    for (size_t failing_write = 0; failing_write < 4; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.ExpireProviderQuotes(111, expired_count, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_expiry);
        BOOST_CHECK_EQUAL(expired_count, 0U);
    }
    database.FailCommit();
    BOOST_CHECK(!store.ExpireProviderQuotes(111, expired_count, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_expiry);
    BOOST_CHECK_EQUAL(expired_count, 0U);

    BOOST_REQUIRE_MESSAGE(store.ExpireProviderQuotes(111, expired_count, error), error);
    BOOST_CHECK_EQUAL(expired_count, 1U);
    ProviderAttempt persisted_attempt;
    BOOST_REQUIRE(store.GetAttempt(expired.attempt.attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.state == AttemptState::QUOTE_EXPIRED);
    PaymentSession persisted_session;
    BOOST_REQUIRE(store.GetSessionByRequestId(expired.session.request_id, persisted_session));
    BOOST_CHECK(persisted_session.state == SessionState::FAILED);

    std::vector<ProviderPoolEntry> pool;
    ProviderBudgetLedger persisted_ledger;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(persisted_ledger));
    }
    const auto pool_state = [&](const uint256& commit_key) {
        return std::find_if(pool.begin(), pool.end(), [&](const ProviderPoolEntry& entry) {
                   return entry.reservation_id == commit_key ||
                          (commit_key == expired.attempt.commit_key &&
                           entry.outpoint == expired.pool_entry.outpoint);
               })
            ->state;
    };
    BOOST_CHECK(pool_state(expired.attempt.commit_key) == PoolEntryState::AVAILABLE);
    BOOST_CHECK(pool_state(authorized.attempt.commit_key) == PoolEntryState::RESERVED);
    BOOST_CHECK(pool_state(state_protected.attempt.commit_key) == PoolEntryState::RESERVED);
    for (const ProviderBudgetReservation& reservation : persisted_ledger.reservations) {
        if (reservation.commit_key == expired.attempt.commit_key) {
            BOOST_CHECK(reservation.state == BudgetReservationState::RELEASED);
        } else {
            BOOST_CHECK(reservation.state == BudgetReservationState::RESERVED);
        }
    }

    BOOST_REQUIRE(store.ExpireProviderQuotes(112, expired_count, error));
    BOOST_CHECK_EQUAL(expired_count, 0U);

    const QuoteFixture unreadable_authorization = make_quote(
        "550e8400-e29b-41d4-a716-446655440104", uint256S("81"),
        uint256S("82"), uint256S("83"), uint256S("84"));
    BOOST_REQUIRE_MESSAGE(
        ReserveProviderBudget(
            persisted_ledger, safety, FundingModel::USER_PAID,
            SponsorshipScope::PUBLIC,
            unreadable_authorization.attempt.commit_key, DGBSatoshis{50},
            recipient_bucket, 101, error, netgroup_bucket),
        error);
    pool.push_back(unreadable_authorization.pool_entry);
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(
            batch.WritePaymasterSession(unreadable_authorization.session));
        BOOST_REQUIRE(batch.WritePaymasterSessionId(
            unreadable_authorization.session.session_id,
            unreadable_authorization.session.request_id));
        BOOST_REQUIRE(
            batch.WritePaymasterAttempt(unreadable_authorization.attempt));
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(
            persisted_ledger));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool(pool));
    }
    UserAuthorizationRecord outdated_authorization;
    outdated_authorization.version = 0;
    outdated_authorization.commit_key =
        unreadable_authorization.attempt.commit_key;
    outdated_authorization.attempt_id =
        unreadable_authorization.attempt.attempt_id;
    outdated_authorization.canonical_psbt_hash = uint256S("85");
    outdated_authorization.accepted_at = 102;
    outdated_authorization.retry_until = 200;
    {
        auto batch = m_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(batch->Write(
            std::make_pair(DBKeys::PAYMASTER_USER_AUTH,
                           outdated_authorization.commit_key),
            outdated_authorization));
    }
    const auto before_unreadable_authorization = database.m_records;
    BOOST_CHECK(!store.ExpireProviderQuotes(113, expired_count, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=UserAuthorizationRecord found=0 expected=1");
    BOOST_CHECK_EQUAL(expired_count, 0U);
    BOOST_CHECK(database.m_records == before_unreadable_authorization);
}

BOOST_AUTO_TEST_CASE(provider_quote_preflight_is_read_only_and_commit_is_atomic)
{
    constexpr int64_t now{1000};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440201";
    PaymasterStore store{m_wallet};
    std::string error;

    ProviderPolicy advertised;
    advertised.funding_models = FUNDING_MODEL_SPONSORED;
    advertised.sponsorship_scope = SponsorshipScope::PUBLIC;
    advertised.fee_rate_bps = 0;
    advertised.min_payment = DDCents{100};
    advertised.max_payment = DDCents{10000};
    advertised.quote_ttl = 60;
    advertised.maximum_network_fee = DGBSatoshis{500};

    ProviderSafetyPolicy safety;
    safety.public_sponsored.maximum_network_fee_per_transaction =
        DGBSatoshis{500};
    safety.public_sponsored.maximum_reserved_network_fee = DGBSatoshis{5000};
    safety.public_sponsored.maximum_network_fee_per_hour = DGBSatoshis{5000};
    safety.public_sponsored.maximum_network_fee_per_day = DGBSatoshis{50000};
    safety.public_sponsored.maximum_completed_per_hour = 10;
    safety.public_sponsored.maximum_completed_per_day = 100;
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 5;
    safety.maximum_active_quotes_per_recipient = 5;
    safety.maximum_quote_requests_per_netgroup_per_minute = 2;
    safety.updated_at = now - 10;

    PaymentSession session;
    session.request_id = request_id;
    session.session_id = uint256S("91");
    session.canonical_request_hash = uint256S("92");
    session.fee_mode_requested = FeeMode::PAYMASTER;
    session.fee_mode_used = FeeMode::PAYMASTER;
    session.created_at = now;
    session.updated_at = now;

    ValidClientResultArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(
        BuildValidClientResultArtifacts(
            m_wallet, session, request_id, uint256S("93"), now, artifacts,
            error, &advertised),
        error);

    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    CollaborativePSBTTemplate trusted;
    {
        CDataStream request_stream{artifacts.attempt.quote_request,
                                   SER_NETWORK, ::PROTOCOL_VERSION};
        request_stream >> request;
        BOOST_REQUIRE(request_stream.empty());
        SpanReader response_stream{::PROTOCOL_VERSION,
                                   artifacts.attempt.signed_quote};
        response_stream >> response;
        BOOST_REQUIRE(response_stream.empty());
        SpanReader capacity_request_stream{
            ::PROTOCOL_VERSION, artifacts.attempt.capacity_request};
        capacity_request_stream >> capacity_request;
        BOOST_REQUIRE(capacity_request_stream.empty());
        SpanReader capacity_proof_stream{
            ::PROTOCOL_VERSION,
            artifacts.attempt.capacity_snapshot.capacity_proof};
        capacity_proof_stream >> capacity_proof;
        BOOST_REQUIRE(capacity_proof_stream.empty());
        SpanReader psbt_stream{::PROTOCOL_VERSION,
                               artifacts.attempt.unsigned_psbt};
        psbt_stream >> trusted.psbt;
        BOOST_REQUIRE(psbt_stream.empty());
        trusted.input_roles = {
            InputRole::USER_DD, InputRole::PROVIDER_DGB};
    }

    ProviderIdentityRecord identity;
    identity.identity_key =
        XOnlyPubKey{artifacts.provider_identity_key.GetPubKey()};
    identity.provider_id = GetPaymasterId(identity.identity_key);
    TaprootBuilder identity_builder;
    identity_builder.Finalize(identity.identity_key);
    identity.identity_script =
        GetScriptForDestination(identity_builder.GetOutput());
    identity.display_name = "Atomic quote provider";
    identity.created_at = now - 20;
    BOOST_REQUIRE_EQUAL(identity.provider_id, request.intent.provider_id);

    ProviderSettings settings;
    settings.enabled = true;
    settings.policy_hash = GetProviderPolicyHash(advertised);
    settings.updated_at = now - 10;

    ProviderBudgetLedger ledger;
    ledger.recipient_bucket_secret = uint256S("94");
    ledger.accounting_time_high_water = now - 10;
    const std::vector<unsigned char> canonical_netgroup{
        5, 0x13, 0x37, 0x42, 0x56, 0x68, 0x79, 0x8a, 0x9b, 0xac, 0xbd};
    const uint256 expected_netgroup_bucket =
        GetNetgroupBudgetBucket(ledger, canonical_netgroup);
    const uint256 capacity_request_hash{Hash(
        SerializePaymasterTestObject(capacity_request))};
    const uint256 request_key = GetProviderRequestSlotKey(
        identity.provider_id, request_id, session.session_id);
    BOOST_REQUIRE(!expected_netgroup_bucket.IsNull());
    BOOST_REQUIRE_EQUAL(capacity_request_hash,
                        artifacts.attempt.capacity_snapshot.request_hash);
    BOOST_REQUIRE_MESSAGE(
        ReserveProviderCapacityAdmission(
            ledger, safety, request_key, capacity_request_hash,
            expected_netgroup_bucket, request.intent.funding_model,
            /*requires_carrier=*/false, capacity_request.expires_at, now,
            error),
        error);

    BOOST_REQUIRE_EQUAL(response.quote.reserved_dgb_inputs.size(), 1U);
    const VerifiedDGBInput& provider_input =
        response.quote.reserved_dgb_inputs.front();
    const CTransaction provider_transaction{provider_input.creating_tx};
    BOOST_REQUIRE(provider_input.outpoint.n < provider_transaction.vout.size());
    ProviderPoolEntry pool_entry;
    pool_entry.outpoint = provider_input.outpoint;
    pool_entry.purpose = PoolPurpose::OPERATIONAL;
    pool_entry.asset = PoolAsset::DGB;
    pool_entry.state = PoolEntryState::RESERVED;
    pool_entry.script_pub_key =
        provider_transaction.vout[provider_input.outpoint.n].scriptPubKey;
    pool_entry.dgb_value = provider_input.value;
    pool_entry.confirmation_height = 1;
    pool_entry.reservation_id = request.intent.client_nonce;
    pool_entry.updated_at = now;

    ProviderAttempt attempt{artifacts.attempt};
    attempt.state = AttemptState::QUOTED;
    attempt.provider_netgroup_bucket = expected_netgroup_bucket;
    attempt.client_manifest = {};
    attempt.accepted_client_manifest_id.SetNull();
    attempt.client_manifest_accepted_at = 0;
    attempt.user_signed_psbt.clear();
    BOOST_REQUIRE_MESSAGE(
        BuildProviderAuthorizationManifest(
            request.intent, response.quote, trusted,
            GetProviderSafetyPolicyHash(safety), attempt.provider_manifest,
            error),
        error);

    HashWriter capacity_session_hasher =
        TaggedHash("DigiByte Paymaster Capacity Session v1");
    capacity_session_hasher << capacity_proof.provider_id
                            << capacity_proof.request_id
                            << capacity_proof.session_id;
    const uint256 capacity_session_key{capacity_session_hasher.GetSHA256()};
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterIdentity(identity));
        BOOST_REQUIRE(batch.WritePaymasterPolicy(advertised));
        BOOST_REQUIRE(batch.WritePaymasterSettings(settings));
        BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(safety));
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(ledger));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({pool_entry}));
        BOOST_REQUIRE(batch.WritePaymasterCapacityNonce(
            capacity_request.client_nonce, capacity_request_hash));
        BOOST_REQUIRE(batch.WritePaymasterCapacitySession(
            capacity_session_key, capacity_request_hash));
        BOOST_REQUIRE(batch.WritePaymasterCapacityResponse(
            capacity_request_hash,
            SerializePaymasterTestObject(capacity_proof)));
    }

    const uint256 quote_request_hash{Hash(attempt.quote_request)};
    uint256 netgroup_bucket;
    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_preflight = database.m_records;

    database.FailWriteAt(0);
    BOOST_CHECK(store.CheckProviderQuoteAdmission(
        request, quote_request_hash, canonical_netgroup,
        /*requires_carrier=*/false, response.quote.network_fee, now,
        netgroup_bucket, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_preflight);
    BOOST_CHECK_EQUAL(netgroup_bucket, expected_netgroup_bucket);

    database.FailCommit();
    BOOST_CHECK(store.CheckProviderQuoteAdmission(
        request, quote_request_hash, canonical_netgroup,
        /*requires_carrier=*/false, response.quote.network_fee, now,
        netgroup_bucket, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_preflight);

    BOOST_CHECK(!store.CheckProviderQuoteAdmission(
        request, uint256S("95"), canonical_netgroup,
        /*requires_carrier=*/false, response.quote.network_fee, now,
        netgroup_bucket, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH");
    BOOST_CHECK(database.m_records == before_preflight);

    PaymasterQuoteRequest alternate_request{request};
    alternate_request.intent.offer_id = uint256S("96");
    const uint256 alternate_request_hash{
        Hash(SerializePaymasterTestObject(alternate_request))};
    BOOST_REQUIRE_NE(alternate_request_hash, quote_request_hash);
    BOOST_CHECK(store.CheckProviderQuoteAdmission(
        alternate_request, alternate_request_hash, canonical_netgroup,
        /*requires_carrier=*/false, response.quote.network_fee, now,
        netgroup_bucket, error));
    BOOST_CHECK(database.m_records == before_preflight);
    BOOST_CHECK(store.CheckProviderQuoteAdmission(
        request, quote_request_hash, canonical_netgroup,
        /*requires_carrier=*/false, response.quote.network_fee, now,
        netgroup_bucket, error));
    BOOST_CHECK(database.m_records == before_preflight);

    const auto check_uncommitted = [&] {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        ProviderBudgetLedger persisted_ledger;
        std::vector<ProviderPoolEntry> persisted_pool;
        ProviderAttempt persisted_attempt;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(
            persisted_ledger));
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(persisted_pool));
        const auto admission = std::find_if(
            persisted_ledger.capacity_admissions.begin(),
            persisted_ledger.capacity_admissions.end(),
            [&](const ProviderCapacityAdmission& candidate) {
                return candidate.request_key == request_key;
            });
        BOOST_REQUIRE(admission !=
                      persisted_ledger.capacity_admissions.end());
        BOOST_CHECK(admission->state == CapacityAdmissionState::RESERVED);
        BOOST_CHECK(admission->quote_request_hash.IsNull());
        BOOST_CHECK(admission->commit_key.IsNull());
        BOOST_CHECK(persisted_ledger.reservations.empty());
        BOOST_REQUIRE_EQUAL(persisted_pool.size(), 1U);
        BOOST_CHECK(persisted_pool.front().state == PoolEntryState::RESERVED);
        BOOST_CHECK_EQUAL(persisted_pool.front().reservation_id,
                          request.intent.client_nonce);
        BOOST_CHECK(!batch.ReadPaymasterAttempt(attempt.attempt_id,
                                                persisted_attempt));
    };

    const MockableData before_commit = database.m_records;
    ProviderAttempt poisoned_attempt{attempt};
    poisoned_attempt.capacity_proof_claim_candidate = {1, 2, 3};
    BOOST_CHECK(!store.CommitProviderQuote(
        poisoned_attempt, request.intent.genesis_hash, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_PROVIDER_QUOTE");
    BOOST_CHECK(database.m_records == before_commit);
    BOOST_CHECK(!store.CommitProviderQuote(
        attempt, uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"),
        now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN");
    BOOST_CHECK(database.m_records == before_commit);
    for (size_t failing_write = 0; failing_write < 7; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.CommitProviderQuote(
            attempt, request.intent.genesis_hash, now, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_commit);
        check_uncommitted();
    }
    database.FailCommit();
    BOOST_CHECK(!store.CommitProviderQuote(
        attempt, request.intent.genesis_hash, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_commit);
    check_uncommitted();

    BOOST_REQUIRE_MESSAGE(
        store.CommitProviderQuote(
            attempt, request.intent.genesis_hash, now, error),
        error);
    ProviderBudgetLedger committed_ledger;
    std::vector<ProviderPoolEntry> committed_pool;
    ProviderAttempt committed_attempt;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(
            committed_ledger));
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(committed_pool));
        BOOST_REQUIRE(batch.ReadPaymasterAttempt(attempt.attempt_id,
                                                 committed_attempt));
    }
    const auto committed_admission = std::find_if(
        committed_ledger.capacity_admissions.begin(),
        committed_ledger.capacity_admissions.end(),
        [&](const ProviderCapacityAdmission& candidate) {
            return candidate.request_key == request_key;
        });
    BOOST_REQUIRE(committed_admission !=
                  committed_ledger.capacity_admissions.end());
    BOOST_CHECK(committed_admission->state ==
                CapacityAdmissionState::PROMOTED);
    BOOST_CHECK_EQUAL(committed_admission->quote_request_hash,
                      quote_request_hash);
    BOOST_CHECK_EQUAL(committed_admission->commit_key, attempt.commit_key);
    BOOST_REQUIRE_EQUAL(committed_ledger.reservations.size(), 1U);
    BOOST_CHECK_EQUAL(committed_ledger.reservations.front().commit_key,
                      attempt.commit_key);
    BOOST_CHECK(committed_ledger.reservations.front().state ==
                BudgetReservationState::RESERVED);
    BOOST_REQUIRE_EQUAL(committed_pool.size(), 1U);
    BOOST_CHECK_EQUAL(committed_pool.front().reservation_id,
                      attempt.commit_key);
    BOOST_CHECK_EQUAL(committed_attempt.commit_key, attempt.commit_key);

    const MockableData before_exact_replay = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.CommitProviderQuote(
        attempt, request.intent.genesis_hash, now, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_exact_replay);

    auto restarted_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet restarted_wallet{m_node.chain.get(), "quote-atomic-restart",
                                     std::move(restarted_database)};
    PaymasterStore restarted_store{restarted_wallet};
    auto& restarted_mock = GetMockableDatabase(restarted_wallet);
    const MockableData before_conflict = restarted_mock.m_records;
    BOOST_CHECK(!restarted_store.CheckProviderQuoteAdmission(
        alternate_request, alternate_request_hash, canonical_netgroup,
        /*requires_carrier=*/false, response.quote.network_fee, now,
        netgroup_bucket, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_ADMISSION_CONFLICT");
    BOOST_CHECK(restarted_mock.m_records == before_conflict);
}

BOOST_AUTO_TEST_CASE(unavailable_submit_rejection_and_pool_release_are_atomic)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440008";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            session, error) == CreatePaymasterSessionResult::CREATED);

    CKey identity_key;
    identity_key.MakeNewKey(true);
    ProviderIdentityRecord identity;
    identity.identity_key = XOnlyPubKey{identity_key.GetPubKey()};
    identity.provider_id = GetPaymasterId(identity.identity_key);
    TaprootBuilder identity_builder;
    identity_builder.Finalize(identity.identity_key);
    identity.identity_script = GetScriptForDestination(identity_builder.GetOutput());
    identity.display_name = "Provider";
    identity.created_at = 100;

    ProviderAttempt attempt;
    attempt.session_id = session.session_id;
    attempt.attempt_id = uint256S("61");
    attempt.provider_id = identity.provider_id;
    attempt.provider_identity_key = identity.identity_key;
    attempt.state = AttemptState::QUOTED;
    attempt.commit_key = uint256S("62");
    attempt.created_at = attempt.updated_at = 101;

    ProviderPoolEntry pool_entry;
    pool_entry.outpoint = COutPoint{uint256S("63"), 0};
    pool_entry.purpose = PoolPurpose::OPERATIONAL;
    pool_entry.asset = PoolAsset::DGB;
    pool_entry.state = PoolEntryState::RESERVED;
    pool_entry.script_pub_key = identity.identity_script;
    pool_entry.dgb_value = DGBSatoshis{1000};
    pool_entry.confirmation_height = 1;
    pool_entry.reservation_id = attempt.commit_key;
    pool_entry.updated_at = 101;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterIdentity(identity));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({pool_entry}));
    }

    PaymasterResult rejection;
    rejection.genesis_hash = uint256S("64");
    rejection.provider_id = identity.provider_id;
    rejection.commit_key = attempt.commit_key;
    rejection.result_sequence = 1;
    rejection.status = PaymasterResultStatus::REJECTED;
    rejection.updated_at = 102;
    rejection.identity_signature.resize(64);
    BOOST_REQUIRE(identity_key.SignSchnorr(GetPaymasterResultSignatureHash(rejection),
                                           rejection.identity_signature, nullptr, uint256{}));

    ProviderAttempt rejected;
    BOOST_REQUIRE(store.RejectUnavailableProviderSubmit(
        attempt.attempt_id, rejection, rejection.genesis_hash, rejected, error));
    BOOST_CHECK(rejected.state == AttemptState::REJECTED);
    BOOST_CHECK(store.RejectUnavailableProviderSubmit(
        attempt.attempt_id, rejection, rejection.genesis_hash, rejected, error));

    PaymasterResult persisted;
    BOOST_REQUIRE(store.GetProviderResult(attempt.commit_key, persisted));
    BOOST_CHECK(persisted.status == PaymasterResultStatus::REJECTED);
    BOOST_CHECK_EQUAL(persisted.result_sequence, 1U);
    std::vector<ProviderPoolEntry> pool;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterProviderPool(pool));
    }
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
    BOOST_CHECK(pool.front().reservation_id.IsNull());
    BOOST_CHECK_EQUAL(pool.front().updated_at, rejection.updated_at);
}

BOOST_AUTO_TEST_CASE(provider_outcome_is_recorded_once_without_payment_details)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440006";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            session, error) == CreatePaymasterSessionResult::CREATED);
    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("41");
    attempt.provider_id = uint256S("42");
    attempt.created_at = attempt.updated_at = 101;
    BOOST_REQUIRE(store.AddAttempt(request_id, attempt, error));
    attempt.session_id = session.session_id;
    attempt.state = AttemptState::REJECTED;
    attempt.updated_at = 102;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterAttempt(attempt));
    }
    BOOST_REQUIRE(store.RecordProviderOutcome(attempt.attempt_id,
                                              ReliabilityOutcome::PROVIDER_FAILURE,
                                              103, 0, error));
    BOOST_CHECK(store.RecordProviderOutcome(attempt.attempt_id,
                                            ReliabilityOutcome::PROVIDER_FAILURE,
                                            104, 0, error));
    BOOST_CHECK(!store.RecordProviderOutcome(attempt.attempt_id,
                                             ReliabilityOutcome::NEUTRAL_FAILURE,
                                             104, 0, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_OUTCOME_ALREADY_RECORDED");
    PaymasterReliabilityRecord record;
    BOOST_REQUIRE(store.GetProviderReliability(attempt.provider_id, record));
    const ReliabilitySummary summary = SummarizeReliability(record, 105);
    BOOST_CHECK_EQUAL(summary.provider_failures, 1U);
    BOOST_REQUIRE(store.ClearProviderReliability(attempt.provider_id, error));
    BOOST_CHECK(!store.GetProviderReliability(attempt.provider_id, record));
    BOOST_CHECK(store.RecordProviderOutcome(attempt.attempt_id,
                                            ReliabilityOutcome::PROVIDER_FAILURE,
                                            106, 0, error));
    BOOST_CHECK(!store.GetProviderReliability(attempt.provider_id, record));

    PaymasterOutcomeMarker outdated_outcome;
    outdated_outcome.version = 0;
    outdated_outcome.attempt_id = attempt.attempt_id;
    outdated_outcome.provider_id = attempt.provider_id;
    outdated_outcome.outcome = ReliabilityOutcome::PROVIDER_FAILURE;
    outdated_outcome.observed_at = 103;
    {
        auto batch = m_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(batch->Write(
            std::make_pair(DBKeys::PAYMASTER_OUTCOME, attempt.attempt_id),
            outdated_outcome));
    }
    const auto before_outdated_outcome =
        GetMockableDatabase(m_wallet).m_records;
    BOOST_CHECK(!store.RecordProviderOutcome(
        attempt.attempt_id, ReliabilityOutcome::PROVIDER_FAILURE, 107, 0,
        error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=PaymasterOutcomeMarker found=0 expected=1");
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records ==
                before_outdated_outcome);

    PaymasterReliabilityRecord outdated_reliability;
    outdated_reliability.version = 0;
    outdated_reliability.provider_id = attempt.provider_id;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ErasePaymasterOutcomeMarker(attempt.attempt_id));
    }
    {
        auto raw_batch = m_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(raw_batch->Write(
            std::make_pair(DBKeys::PAYMASTER_RELIABILITY,
                           attempt.provider_id),
            outdated_reliability));
    }
    const auto before_outdated_reliability =
        GetMockableDatabase(m_wallet).m_records;
    BOOST_CHECK(!store.RecordProviderOutcome(
        attempt.attempt_id, ReliabilityOutcome::PROVIDER_FAILURE, 108, 0,
        error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=PaymasterReliabilityRecord found=0 expected=1");
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records ==
                before_outdated_reliability);
    BOOST_CHECK(!store.ClearProviderReliability(attempt.provider_id, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=PaymasterReliabilityRecord found=0 expected=1");
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records ==
                before_outdated_reliability);
}

BOOST_AUTO_TEST_CASE(provider_block_lookup_is_tristate_and_rpc_gate_fails_closed)
{
    PaymasterStore store{m_wallet};
    const PaymasterId provider_id{uint256S("7e01")};
    PaymasterProviderBlock block;
    std::string error;

    BOOST_CHECK(store.GetProviderBlock(provider_id, block, error) ==
                DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(error.empty());
    BOOST_CHECK(EnsureProviderNotEquivocationBlocked(store, provider_id,
                                                     error));
    BOOST_CHECK(error.empty());

    block.provider_id = provider_id;
    block.evidence_id = uint256S("7e02");
    block.kind = EquivocationKind::CAPACITY;
    block.blocked_at = 100;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterProviderBlock(block, false));
    }

    PaymasterProviderBlock persisted;
    BOOST_CHECK(store.GetProviderBlock(provider_id, persisted, error) ==
                DatabaseReadStatus::FOUND);
    BOOST_CHECK(error.empty());
    BOOST_CHECK(!EnsureProviderNotEquivocationBlocked(store, provider_id,
                                                      error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_EQUIVOCATION_BLOCKED");

    auto& database = GetMockableDatabase(m_wallet);
    database.m_pass = false;
    BOOST_CHECK(store.GetProviderBlock(provider_id, persisted, error) ==
                DatabaseReadStatus::READ_ERROR);
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ");
    BOOST_CHECK(!EnsureProviderNotEquivocationBlocked(store, provider_id,
                                                      error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ");
    database.m_pass = true;

    DataStream block_key_stream;
    block_key_stream << std::make_pair(DBKeys::PAYMASTER_PROVIDER_BLOCK,
                                       provider_id);
    const SerializeData block_key{block_key_stream.begin(),
                                  block_key_stream.end()};
    auto stored_block = database.m_records.find(block_key);
    BOOST_REQUIRE(stored_block != database.m_records.end());

    PaymasterProviderBlock invalid_block = block;
    invalid_block.blocked_at = 0;
    CDataStream invalid_block_stream{SER_DISK, CLIENT_VERSION};
    invalid_block_stream << invalid_block;
    stored_block->second = SerializeData{invalid_block_stream.begin(),
                                         invalid_block_stream.end()};
    BOOST_CHECK(store.GetProviderBlock(provider_id, persisted, error) ==
                DatabaseReadStatus::READ_ERROR);
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ");
    BOOST_CHECK(!EnsureProviderNotEquivocationBlocked(store, provider_id,
                                                      error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ");

    stored_block->second = SerializeData{std::byte{0xff}};

    BOOST_CHECK(store.GetProviderBlock(provider_id, persisted, error) ==
                DatabaseReadStatus::READ_ERROR);
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ");
    BOOST_CHECK(!EnsureProviderNotEquivocationBlocked(store, provider_id,
                                                      error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ");
}

BOOST_AUTO_TEST_CASE(pending_equivocation_stage_is_durable_idempotent_and_fail_closed)
{
    PaymasterStore store{m_wallet};
    const PaymasterId provider_id{uint256S("7f01")};
    const PaymasterEquivocationEvidence evidence =
        MakeTestEquivocationEvidence(
            provider_id, EquivocationKind::CAPACITY, uint256S("7f02"),
            {1, 2, 3}, {4, 5, 6}, 100);
    BOOST_REQUIRE(ValidateEquivocationEvidence(evidence));

    std::string error;
    PaymasterEquivocationEvidence pending;
    BOOST_CHECK(store.GetPendingEquivocation(provider_id, pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(error.empty());

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData initial_state = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StagePendingEquivocation(evidence, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == initial_state);
    BOOST_CHECK(store.GetPendingEquivocation(provider_id, pending, error) ==
                DatabaseReadStatus::NOT_FOUND);

    database.FailCommit();
    BOOST_CHECK(!store.StagePendingEquivocation(evidence, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == initial_state);

    BOOST_REQUIRE_MESSAGE(store.StagePendingEquivocation(evidence, error),
                          error);
    BOOST_REQUIRE(store.GetPendingEquivocation(provider_id, pending, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(pending.evidence_id == evidence.evidence_id);
    BOOST_CHECK(pending.provider_id == evidence.provider_id);
    BOOST_CHECK(!EnsureProviderNotEquivocationBlocked(store, provider_id,
                                                      error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_EQUIVOCATION_PENDING");
    const MockableData staged_state = database.m_records;

    BOOST_REQUIRE_MESSAGE(store.StagePendingEquivocation(evidence, error),
                          error);
    BOOST_CHECK(database.m_records == staged_state);

    PaymasterEquivocationEvidence conflicting = MakeTestEquivocationEvidence(
        provider_id, EquivocationKind::QUOTE, uint256S("7f03"),
        {7, 8, 9}, {10, 11, 12}, 101);
    BOOST_REQUIRE(ValidateEquivocationEvidence(conflicting));
    BOOST_CHECK(!store.StagePendingEquivocation(conflicting, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PENDING_EQUIVOCATION_CONFLICT");
    BOOST_CHECK(database.m_records == staged_state);
    BOOST_REQUIRE(store.GetPendingEquivocation(provider_id, pending, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(pending.evidence_id == evidence.evidence_id);

    database.m_pass = false;
    BOOST_CHECK(store.GetPendingEquivocation(provider_id, pending, error) ==
                DatabaseReadStatus::READ_ERROR);
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PENDING_EQUIVOCATION_DATABASE_READ");
    database.m_pass = true;

    PaymasterEquivocationEvidence invalid = evidence;
    invalid.provider_id.SetNull();
    BOOST_CHECK(!store.StagePendingEquivocation(invalid, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_EQUIVOCATION_EVIDENCE");
    BOOST_CHECK(database.m_records == staged_state);
}

BOOST_AUTO_TEST_CASE(pending_equivocation_promotion_is_atomic_and_restart_recoverable)
{
    PaymasterStore store{m_wallet};
    const PaymasterId provider_id{uint256S("7f11")};
    const PaymasterEquivocationEvidence evidence =
        MakeTestEquivocationEvidence(
            provider_id, EquivocationKind::QUOTE, uint256S("7f12"),
            {13, 14, 15}, {16, 17, 18}, 110);
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.StagePendingEquivocation(evidence, error),
                          error);

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData staged_state = database.m_records;
    for (size_t failing_write = 0; failing_write < 2; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.PromotePendingEquivocation(provider_id, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == staged_state);
        PaymasterEquivocationEvidence pending;
        BOOST_CHECK(store.GetPendingEquivocation(provider_id, pending, error) ==
                    DatabaseReadStatus::FOUND);
        PaymasterEquivocationEvidence absent_evidence;
        BOOST_CHECK(!store.GetEquivocationEvidence(evidence.evidence_id,
                                                   absent_evidence));
        PaymasterProviderBlock absent;
        BOOST_CHECK(store.GetProviderBlock(provider_id, absent, error) ==
                    DatabaseReadStatus::NOT_FOUND);
    }

    database.FailCommit();
    BOOST_CHECK(!store.PromotePendingEquivocation(provider_id, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == staged_state);

    auto restarted_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet restarted_wallet{
        m_node.chain.get(), "pending-equivocation-restart",
        std::move(restarted_database)};
    BOOST_REQUIRE(restarted_wallet.LoadWallet() == DBErrors::LOAD_OK);
    PaymasterStore restarted_store{restarted_wallet};
    PaymasterEquivocationEvidence restarted_pending;
    BOOST_REQUIRE(restarted_store.GetPendingEquivocation(
                      provider_id, restarted_pending, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(restarted_pending.evidence_id == evidence.evidence_id);

    BOOST_REQUIRE_MESSAGE(restarted_store.PromotePendingEquivocations(error),
                          error);
    BOOST_CHECK(restarted_store.GetPendingEquivocation(
                    provider_id, restarted_pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    PaymasterEquivocationEvidence persisted;
    BOOST_REQUIRE(restarted_store.GetEquivocationEvidence(
        evidence.evidence_id, persisted));
    BOOST_CHECK(persisted.provider_id == provider_id);
    PaymasterProviderBlock block;
    BOOST_REQUIRE(restarted_store.GetProviderBlock(provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.evidence_id == evidence.evidence_id);
    BOOST_CHECK(block.kind == evidence.kind);

    // Re-staging the same verified claim after final promotion is safe. The
    // second phase recognizes both final records and atomically erases only
    // the redundant pending marker.
    BOOST_REQUIRE_MESSAGE(
        restarted_store.StagePendingEquivocation(evidence, error), error);
    BOOST_REQUIRE(restarted_store.GetPendingEquivocation(
                      provider_id, restarted_pending, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_REQUIRE_MESSAGE(
        restarted_store.PromotePendingEquivocation(provider_id, error), error);
    BOOST_CHECK(restarted_store.GetPendingEquivocation(
                    provider_id, restarted_pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    PaymasterProviderBlock idempotent_block;
    BOOST_REQUIRE(restarted_store.GetProviderBlock(
                      provider_id, idempotent_block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(idempotent_block.evidence_id == block.evidence_id);
    BOOST_CHECK_EQUAL(idempotent_block.blocked_at, block.blocked_at);

    const PaymasterId second_provider{uint256S("7f13")};
    const PaymasterEquivocationEvidence second =
        MakeTestEquivocationEvidence(
            second_provider, EquivocationKind::CAPACITY, uint256S("7f14"),
            {19, 20, 21}, {22, 23, 24}, 111);
    BOOST_REQUIRE_MESSAGE(
        restarted_store.StagePendingEquivocation(evidence, error), error);
    BOOST_REQUIRE_MESSAGE(
        restarted_store.StagePendingEquivocation(second, error), error);
    BOOST_REQUIRE_MESSAGE(restarted_store.PromotePendingEquivocations(error),
                          error);
    BOOST_CHECK(restarted_store.GetPendingEquivocation(
                    provider_id, restarted_pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(restarted_store.GetPendingEquivocation(
                    second_provider, restarted_pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    BOOST_REQUIRE(restarted_store.GetProviderBlock(
                      second_provider, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.evidence_id == second.evidence_id);
}

BOOST_AUTO_TEST_CASE(pending_equivocation_key_value_mismatch_is_read_error)
{
    PaymasterStore store{m_wallet};
    const PaymasterId value_provider{uint256S("7f21")};
    const PaymasterId key_provider{uint256S("7f22")};
    const PaymasterEquivocationEvidence evidence =
        MakeTestEquivocationEvidence(
            value_provider, EquivocationKind::CAPACITY, uint256S("7f23"),
            {25, 26, 27}, {28, 29, 30}, 120);
    BOOST_REQUIRE(ValidateEquivocationEvidence(evidence));

    DataStream key_stream;
    key_stream << std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING,
                                 key_provider);
    CDataStream value_stream{SER_DISK, CLIENT_VERSION};
    value_stream << evidence;
    auto& database = GetMockableDatabase(m_wallet);
    database.m_records.emplace(
        SerializeData{key_stream.begin(), key_stream.end()},
        SerializeData{value_stream.begin(), value_stream.end()});

    std::string error;
    PaymasterEquivocationEvidence pending;
    BOOST_CHECK(store.GetPendingEquivocation(key_provider, pending, error) ==
                DatabaseReadStatus::READ_ERROR);
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PENDING_EQUIVOCATION_DATABASE_READ");
    BOOST_CHECK(!store.PromotePendingEquivocations(error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PENDING_EQUIVOCATION_DATABASE_READ");
}

BOOST_AUTO_TEST_CASE(pending_capacity_pair_is_bound_and_phase_two_failure_is_fail_closed)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440204";
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256S("8001"), FeeMode::PAYMASTER, 100,
                      session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    const XOnlyPubKey provider_identity{provider_key.GetPubKey()};
    const PaymasterId provider_id = GetPaymasterId(provider_identity);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});

    PaymentIntent intent;
    intent.genesis_hash = uint256S("8002");
    intent.provider_id = provider_id;
    intent.request_id = request_id;
    intent.session_id = session.session_id;
    intent.client_nonce = uint256S("8003");
    intent.canonical_request_hash = session.canonical_request_hash;
    intent.requested_fee_mode = session.fee_mode_requested;
    intent.user_dd_inputs = {COutPoint{uint256S("8004"), 0}};
    intent.recipient_script =
        GetScriptForDestination(recipient_builder.GetOutput());
    intent.recipient_amount = DDCents{100};
    intent.offer_id = uint256S("8005");
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = uint256S("8006");
    intent.expires_at = 150;

    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("8007");
    attempt.provider_id = provider_id;
    attempt.provider_identity_key = provider_identity;
    attempt.client_nonce = intent.client_nonce;
    attempt.unsigned_intent = SerializePaymasterTestObject(intent);
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
    request.created_at = 101;
    request.expires_at = 145;
    const std::vector<unsigned char> request_bytes =
        SerializePaymasterTestObject(request);
    BOOST_REQUIRE_MESSAGE(
        store.PrepareCapacityRequest(request_id, attempt.attempt_id,
                                     request_bytes, 101, error),
        error);

    CMutableTransaction capacity_funding;
    capacity_funding.vin.emplace_back(COutPoint{uint256S("8008"), 0});
    capacity_funding.vout.emplace_back(1000, CScript{} << OP_TRUE);
    const auto make_proof = [&](const PaymasterCapacityRequest& binding,
                                const uint256& snapshot_id,
                                int64_t created_at,
                                int64_t expires_at) {
        PaymasterCapacityProof proof;
        proof.genesis_hash = binding.genesis_hash;
        proof.provider_id = binding.provider_id;
        proof.request_id = binding.request_id;
        proof.session_id = binding.session_id;
        proof.client_nonce = binding.client_nonce;
        proof.funding_model = binding.funding_model;
        proof.requires_carrier = binding.requires_carrier;
        proof.snapshot_id = snapshot_id;
        proof.created_at = created_at;
        proof.expires_at = expires_at;
        CapacityDGBInput dgb;
        dgb.input.outpoint =
            COutPoint{CTransaction{capacity_funding}.GetHash(), 0};
        dgb.input.creating_tx = capacity_funding;
        dgb.input.value = DGBSatoshis{1000};
        dgb.control_proof.reference_block = uint256S("8009");
        dgb.control_proof.expires_at = expires_at;
        dgb.control_proof.signature.assign(64, 2);
        PaymasterLiquiditySlot slot;
        slot.dgb_inputs = {dgb};
        proof.liquidity_slots = {slot};
        proof.identity_signature.resize(64);
        BOOST_CHECK(provider_key.SignSchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature,
            nullptr, uint256{}));
        return SerializePaymasterTestObject(proof);
    };

    const std::vector<unsigned char> first_proof =
        make_proof(request, uint256S("800a"), 102, 140);
    const std::vector<unsigned char> second_proof =
        make_proof(request, uint256S("800b"), 103, 139);
    PaymasterCapacityRequest wrong_request = request;
    wrong_request.client_nonce = uint256S("800c");
    const std::vector<unsigned char> wrong_binding_proof =
        make_proof(wrong_request, uint256S("800d"), 103, 139);

    BOOST_CHECK(!store.RecordPendingCapacityEquivocation(
        attempt.attempt_id, first_proof, wrong_binding_proof, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION");
    BOOST_CHECK(!store.RecordPendingCapacityEquivocation(
        attempt.attempt_id, {1, 2, 3}, second_proof, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION");
    PaymasterEquivocationEvidence pending;
    BOOST_CHECK(store.GetPendingEquivocation(provider_id, pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    PaymasterProviderBlock absent;
    BOOST_CHECK(store.GetProviderBlock(provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);

    ProviderAttempt persisted_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, persisted_attempt));
    const ProviderAttempt valid_attempt = persisted_attempt;
    CKey other_provider_key;
    other_provider_key.MakeNewKey(true);
    persisted_attempt.provider_identity_key =
        XOnlyPubKey{other_provider_key.GetPubKey()};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(persisted_attempt));
    }
    BOOST_CHECK(!store.RecordPendingCapacityEquivocation(
        attempt.attempt_id, first_proof, second_proof, 104, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PERSISTED_CAPACITY_REQUEST_CORRUPT");
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(valid_attempt));
    }

    ProviderAttempt corrupt_request_attempt{valid_attempt};
    corrupt_request_attempt.capacity_request = {1, 2, 3};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(corrupt_request_attempt));
    }
    bool equivocation{false};
    BOOST_CHECK(!store.StageCapacityProofClaimCandidate(
        attempt.attempt_id, first_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PERSISTED_CAPACITY_REQUEST_CORRUPT");
    BOOST_CHECK(!store.RecordPendingCapacityEquivocation(
        attempt.attempt_id, first_proof, second_proof, 104, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PERSISTED_CAPACITY_REQUEST_CORRUPT");
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(valid_attempt));
    }

    PaymasterCapacityProof decoded_first_proof;
    CDataStream first_proof_stream{first_proof, SER_NETWORK,
                                   ::PROTOCOL_VERSION};
    first_proof_stream >> decoded_first_proof;
    BOOST_REQUIRE(first_proof_stream.empty());
    ValidatedCapacitySnapshot first_snapshot;
    first_snapshot.snapshot_id = decoded_first_proof.snapshot_id;
    first_snapshot.resource_commitment =
        GetCapacityResourceCommitment(decoded_first_proof);
    first_snapshot.session_id = session.session_id;
    first_snapshot.attempt_id = attempt.attempt_id;
    first_snapshot.provider_id = provider_id;
    first_snapshot.client_nonce = request.client_nonce;
    first_snapshot.request_hash = Hash(request_bytes);
    first_snapshot.capacity_proof = first_proof;
    first_snapshot.created_at = decoded_first_proof.created_at;
    first_snapshot.expires_at = decoded_first_proof.expires_at;
    first_snapshot.validated_at = 104;
    first_snapshot.funding_model = decoded_first_proof.funding_model;
    first_snapshot.requires_carrier = decoded_first_proof.requires_carrier;

    BOOST_CHECK(!store.CommitValidatedCapacitySnapshot(
        request_id, attempt.attempt_id, first_snapshot, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_CLAIM_CANDIDATE_MISSING");

    ProviderAttempt corrupt_candidate_attempt{valid_attempt};
    corrupt_candidate_attempt.capacity_proof_claim_candidate = {1, 2, 3};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(corrupt_candidate_attempt));
    }
    BOOST_CHECK(!store.CommitValidatedCapacitySnapshot(
        request_id, attempt.attempt_id, first_snapshot, 104, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!store.StageCapacityProofClaimCandidate(
        attempt.attempt_id, {4, 5, 6}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!equivocation);

    ProviderAttempt differently_bound_candidate_attempt{valid_attempt};
    differently_bound_candidate_attempt.capacity_proof_claim_candidate =
        wrong_binding_proof;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(
            WalletBatch{m_wallet.GetDatabase()}.WritePaymasterAttempt(
                differently_bound_candidate_attempt));
    }
    BOOST_CHECK(!store.CommitValidatedCapacitySnapshot(
        request_id, attempt.attempt_id, first_snapshot, 104, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!store.StageCapacityProofClaimCandidate(
        attempt.attempt_id, {4, 5, 6}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!equivocation);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(valid_attempt));
    }

    auto& database = GetMockableDatabase(m_wallet);
    DataStream attempt_key_stream;
    attempt_key_stream << std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt.attempt_id);
    const SerializeData attempt_key{attempt_key_stream.begin(),
                                    attempt_key_stream.end()};
    ProviderAttempt outdated_attempt{valid_attempt};
    outdated_attempt.version = ProviderAttempt::CURRENT_VERSION - 1;
    CDataStream version14_value{SER_DISK, CLIENT_VERSION};
    version14_value << outdated_attempt;
    SerializeData shortened_version14{version14_value.begin(),
                                      version14_value.end()};
    BOOST_REQUIRE(outdated_attempt.capacity_proof_claim_candidate.empty());
    BOOST_REQUIRE(outdated_attempt.quote_response_claim_candidate.empty());
    BOOST_REQUIRE_GE(shortened_version14.size(), 2U);
    shortened_version14.resize(shortened_version14.size() - 2);
    database.m_records[attempt_key] = std::move(shortened_version14);

    BOOST_CHECK(!store.StageCapacityProofClaimCandidate(
        attempt.attempt_id, {1, 2, 3}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=ProviderAttempt found=14 expected=15");
    BOOST_CHECK(!equivocation);
    BOOST_CHECK(!store.StageCapacityProofClaimCandidate(
        attempt.attempt_id, wrong_binding_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=ProviderAttempt found=14 expected=15");
    BOOST_CHECK(!equivocation);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(valid_attempt));
    }

    const MockableData before_candidate = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StageCapacityProofClaimCandidate(
        attempt.attempt_id, first_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_candidate);
    database.FailCommit();
    BOOST_CHECK(!store.StageCapacityProofClaimCandidate(
        attempt.attempt_id, first_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_candidate);

    BOOST_REQUIRE_MESSAGE(store.StageCapacityProofClaimCandidate(
                              attempt.attempt_id, first_proof, 104,
                              equivocation, error),
                          error);
    BOOST_CHECK(!equivocation);
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, persisted_attempt));
    BOOST_CHECK_EQUAL(persisted_attempt.version,
                      ProviderAttempt::CURRENT_VERSION);
    BOOST_CHECK(persisted_attempt.capacity_proof_claim_candidate ==
                first_proof);
    BOOST_CHECK(persisted_attempt.capacity_snapshot.snapshot_id.IsNull());
    BOOST_CHECK(persisted_attempt.capacity_snapshot.capacity_proof.empty());

    // Exact network retries are read-only and therefore remain successful
    // even while the next database write is fault-injected.
    database.FailWriteAt(0);
    BOOST_REQUIRE_MESSAGE(store.StageCapacityProofClaimCandidate(
                              attempt.attempt_id, first_proof, 200,
                              equivocation, error),
                          error);
    BOOST_CHECK(!equivocation);
    database.ClearFailureInjection();

    // The pending candidate is phase one (write 0). Failing the first final
    // evidence write leaves that verified candidate durable and therefore
    // blocks use of the provider until maintenance can promote it.
    database.FailWriteAt(1);
    BOOST_CHECK(!store.RecordPendingCapacityEquivocation(
        attempt.attempt_id, first_proof, second_proof, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_REQUIRE(store.GetPendingEquivocation(provider_id, pending, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(pending.kind == EquivocationKind::CAPACITY);
    BOOST_CHECK(pending.semantic_key == Hash(request_bytes));
    BOOST_CHECK(!EnsureProviderNotEquivocationBlocked(store, provider_id,
                                                      error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_EQUIVOCATION_PENDING");
    BOOST_CHECK(store.GetProviderBlock(provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);

    BOOST_REQUIRE_MESSAGE(store.StageCapacityProofClaimCandidate(
                              attempt.attempt_id, second_proof, 200,
                              equivocation, error),
                          error);
    BOOST_CHECK(equivocation);
    BOOST_CHECK(store.GetPendingEquivocation(provider_id, pending, error) ==
                DatabaseReadStatus::NOT_FOUND);
    PaymasterProviderBlock block;
    BOOST_REQUIRE(store.GetProviderBlock(provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::CAPACITY);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(store.GetEquivocationEvidence(block.evidence_id, evidence));
    BOOST_CHECK(evidence.semantic_key == Hash(request_bytes));
    BOOST_CHECK((evidence.first_artifact_hash == Hash(first_proof) &&
                 evidence.second_artifact_hash == Hash(second_proof)) ||
                (evidence.first_artifact_hash == Hash(second_proof) &&
                 evidence.second_artifact_hash == Hash(first_proof)));

    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.capacity_snapshot.snapshot_id.IsNull());
    BOOST_CHECK(persisted_attempt.capacity_snapshot.capacity_proof.empty());
}

BOOST_AUTO_TEST_CASE(alternative_recovery_capacity_candidate_is_durable_and_non_authorizing)
{
    ScopedRecoveryMockTime mock_time{104};
    PaymasterStore store{m_wallet};
    std::string error;

    CKey recovery_provider_key;
    recovery_provider_key.MakeNewKey(true);
    const XOnlyPubKey recovery_identity{
        recovery_provider_key.GetPubKey()};
    const PaymasterId recovery_provider_id =
        GetPaymasterId(recovery_identity);

    AlternativeRecoveryRecord recovery;
    recovery.version = AlternativeRecoveryRecord::CURRENT_VERSION;
    recovery.request_id = "550e8400-e29b-41d4-a716-446655440207";
    recovery.session_id = uint256S("8201");
    recovery.original_provider_id = uint256S("8202");
    recovery.recovery_provider_id = recovery_provider_id;
    recovery.offer_id = uint256S("8203");
    recovery.policy_hash = uint256S("8204");
    recovery.recovery_provider_identity_key = recovery_identity;
    recovery.recovery_provider_endpoint = "127.0.0.1:14022";
    recovery.original_commit_key = uint256S("8205");
    recovery.original_template_commitment = uint256S("8206");
    recovery.client_nonce = uint256S("8207");
    recovery.created_at = recovery.updated_at = 100;
    recovery.phase = AlternativeRecoveryPhase::CAPACITY_PENDING;
    recovery.recovery_id = GetAlternativeRecoveryId(
        recovery.request_id, recovery.session_id,
        recovery.recovery_provider_id, recovery.client_nonce);
    recovery.capacity_request.genesis_hash = uint256S("8208");
    recovery.capacity_request.provider_id = recovery_provider_id;
    recovery.capacity_request.request_id = recovery.request_id;
    recovery.capacity_request.session_id = recovery.session_id;
    recovery.capacity_request.client_nonce = recovery.client_nonce;
    recovery.capacity_request.funding_model = FundingModel::USER_PAID;
    recovery.capacity_request.requires_carrier = false;
    recovery.capacity_request.requested_slots = 1;
    recovery.capacity_request.created_at = 101;
    recovery.capacity_request.expires_at = 150;
    AlternativeRecoveryRecord poisoned_recovery{recovery};
    poisoned_recovery.capacity_proof_claim_candidate = {1, 2, 3};
    BOOST_CHECK(!store.PrepareAlternativeRecovery(poisoned_recovery, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_ALTERNATIVE_RECOVERY");
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAlternativeRecovery(recovery));
    }

    CMutableTransaction creating_tx;
    creating_tx.vin.emplace_back(COutPoint{uint256S("8209"), 0});
    creating_tx.vout.emplace_back(1000, CScript{} << OP_TRUE);
    const auto make_proof = [&](const PaymasterCapacityRequest& request,
                                const uint256& snapshot_id,
                                int64_t created_at) {
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
        proof.expires_at = 145;
        CapacityDGBInput dgb;
        dgb.input.outpoint =
            COutPoint{CTransaction{creating_tx}.GetHash(), 0};
        dgb.input.creating_tx = creating_tx;
        dgb.input.value = DGBSatoshis{1000};
        dgb.control_proof.reference_block = uint256S("820a");
        dgb.control_proof.expires_at = proof.expires_at;
        dgb.control_proof.signature.assign(64, 1);
        PaymasterLiquiditySlot slot;
        slot.dgb_inputs = {dgb};
        proof.liquidity_slots = {slot};
        proof.identity_signature.resize(64);
        BOOST_CHECK(recovery_provider_key.SignSchnorr(
            GetCapacityProofSignatureHash(proof),
            proof.identity_signature, nullptr, uint256{}));
        return SerializePaymasterTestObject(proof);
    };

    const std::vector<unsigned char> first_proof =
        make_proof(recovery.capacity_request, uint256S("820b"), 102);
    const std::vector<unsigned char> second_proof =
        make_proof(recovery.capacity_request, uint256S("820c"), 103);
    PaymasterCapacityRequest wrong_request{recovery.capacity_request};
    wrong_request.client_nonce = uint256S("820d");
    const std::vector<unsigned char> wrong_proof =
        make_proof(wrong_request, uint256S("820e"), 103);

    PaymasterCapacityProof decoded_first_proof;
    CDataStream first_proof_stream{first_proof, SER_NETWORK,
                                   ::PROTOCOL_VERSION};
    first_proof_stream >> decoded_first_proof;
    BOOST_REQUIRE(first_proof_stream.empty());
    AlternativeRecoveryRecord request_ready{recovery};
    request_ready.phase = AlternativeRecoveryPhase::REQUEST_READY;
    request_ready.updated_at = 104;
    request_ready.capacity_snapshot.snapshot_id =
        decoded_first_proof.snapshot_id;
    request_ready.capacity_snapshot.resource_commitment =
        GetCapacityResourceCommitment(decoded_first_proof);
    request_ready.capacity_snapshot.session_id = recovery.session_id;
    request_ready.capacity_snapshot.attempt_id = recovery.recovery_id;
    request_ready.capacity_snapshot.provider_id = recovery_provider_id;
    request_ready.capacity_snapshot.client_nonce = recovery.client_nonce;
    request_ready.capacity_snapshot.request_hash =
        Hash(SerializePaymasterTestObject(recovery.capacity_request));
    request_ready.capacity_snapshot.capacity_proof = first_proof;
    request_ready.capacity_snapshot.created_at =
        decoded_first_proof.created_at;
    request_ready.capacity_snapshot.expires_at =
        decoded_first_proof.expires_at;
    request_ready.capacity_snapshot.validated_at = 104;
    request_ready.capacity_snapshot.funding_model =
        decoded_first_proof.funding_model;
    request_ready.capacity_snapshot.requires_carrier =
        decoded_first_proof.requires_carrier;

    AlternativeRecoveryRequest& recovery_request =
        request_ready.recovery_request;
    recovery_request.genesis_hash = recovery.capacity_request.genesis_hash;
    recovery_request.request_id = recovery.request_id;
    recovery_request.session_id = recovery.session_id;
    recovery_request.original_provider_id = recovery.original_provider_id;
    recovery_request.recovery_provider_id = recovery.recovery_provider_id;
    recovery_request.privacy_profile = recovery.privacy_profile;
    recovery_request.offer_id = recovery.offer_id;
    recovery_request.policy_hash = recovery.policy_hash;
    recovery_request.original_commit_key = recovery.original_commit_key;
    recovery_request.original_template_commitment =
        recovery.original_template_commitment;
    recovery_request.client_nonce = recovery.client_nonce;
    recovery_request.capacity_request = recovery.capacity_request;
    recovery_request.capacity_snapshot_id =
        request_ready.capacity_snapshot.snapshot_id;
    recovery_request.capacity_resource_commitment =
        request_ready.capacity_snapshot.resource_commitment;
    const COutPoint user_input{uint256S("820f"), 0};
    recovery_request.user_dd_inputs = {user_input};
    AlternativeRecoveryInputProof input_proof;
    input_proof.outpoint = user_input;
    input_proof.signature.assign(64, 1);
    recovery_request.user_input_proofs = {input_proof};
    CKey return_key;
    return_key.MakeNewKey(true);
    TaprootBuilder return_builder;
    return_builder.Finalize(XOnlyPubKey{return_key.GetPubKey()});
    AlternativeRecoveryReturn wallet_return;
    wallet_return.script_pub_key =
        GetScriptForDestination(return_builder.GetOutput());
    wallet_return.amount = DDCents{100};
    recovery_request.wallet_returns = {wallet_return};
    recovery_request.maximum_service_fee =
        recovery.selected_maximum_service_fee;
    recovery_request.service_fee = recovery.selected_service_fee;
    recovery_request.created_at = 103;
    recovery_request.expires_at = 140;
    request_ready.recovery_request_hash =
        GetAlternativeRecoveryRequestHash(recovery_request);

    BOOST_CHECK(!store.UpdateAlternativeRecovery(request_ready, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_RECOVERY_CAPACITY_CLAIM_CANDIDATE_MISSING");

    AlternativeRecoveryRecord corrupt_candidate{recovery};
    corrupt_candidate.version = AlternativeRecoveryRecord::CURRENT_VERSION;
    corrupt_candidate.capacity_proof_claim_candidate = {1, 2, 3};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAlternativeRecovery(
                              corrupt_candidate));
    }
    AlternativeRecoveryRecord corrupt_candidate_update{request_ready};
    corrupt_candidate_update.version =
        AlternativeRecoveryRecord::CURRENT_VERSION;
    corrupt_candidate_update.capacity_proof_claim_candidate = {1, 2, 3};
    BOOST_CHECK(!store.UpdateAlternativeRecovery(corrupt_candidate_update,
                                                 error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    bool equivocation{false};
    BOOST_CHECK(!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
        recovery.recovery_id, {4, 5, 6}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!equivocation);

    AlternativeRecoveryRecord differently_bound_candidate{recovery};
    differently_bound_candidate.version =
        AlternativeRecoveryRecord::CURRENT_VERSION;
    differently_bound_candidate.capacity_proof_claim_candidate = wrong_proof;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAlternativeRecovery(
                              differently_bound_candidate));
    }
    AlternativeRecoveryRecord differently_bound_update{request_ready};
    differently_bound_update.version =
        AlternativeRecoveryRecord::CURRENT_VERSION;
    differently_bound_update.capacity_proof_claim_candidate = wrong_proof;
    BOOST_CHECK(!store.UpdateAlternativeRecovery(differently_bound_update,
                                                 error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
        recovery.recovery_id, {4, 5, 6}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!equivocation);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAlternativeRecovery(recovery));
    }

    AlternativeRecoveryRecord corrupt_authority{recovery};
    corrupt_authority.capacity_request.client_nonce = uint256S("82ff");
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAlternativeRecovery(
                              corrupt_authority));
    }
    BOOST_CHECK(!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
        recovery.recovery_id, first_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_REQUEST_CORRUPT");
    BOOST_CHECK(!store.RecordPendingAlternativeRecoveryCapacityEquivocation(
        recovery.recovery_id, first_proof, second_proof, 104, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_REQUEST_CORRUPT");
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAlternativeRecovery(recovery));
    }
    BOOST_CHECK(!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
        recovery.recovery_id, wrong_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE");
    BOOST_CHECK(!equivocation);

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_candidate = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
        recovery.recovery_id, first_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_candidate);
    database.FailCommit();
    BOOST_CHECK(!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
        recovery.recovery_id, first_proof, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_candidate);

    BOOST_REQUIRE_MESSAGE(
        store.StageAlternativeRecoveryCapacityProofClaimCandidate(
            recovery.recovery_id, first_proof, 104, equivocation, error),
        error);
    BOOST_CHECK(!equivocation);
    AlternativeRecoveryRecord persisted;
    BOOST_REQUIRE(store.GetAlternativeRecoveryById(recovery.recovery_id,
                                                   persisted));
    BOOST_CHECK_EQUAL(persisted.version,
                      AlternativeRecoveryRecord::CURRENT_VERSION);
    BOOST_CHECK(persisted.capacity_proof_claim_candidate == first_proof);
    BOOST_CHECK(persisted.capacity_snapshot.snapshot_id.IsNull());
    BOOST_CHECK(persisted.capacity_snapshot.capacity_proof.empty());
    BOOST_CHECK(persisted.phase ==
                AlternativeRecoveryPhase::CAPACITY_PENDING);

    database.FailWriteAt(0);
    BOOST_REQUIRE_MESSAGE(
        store.StageAlternativeRecoveryCapacityProofClaimCandidate(
            recovery.recovery_id, first_proof, 200, equivocation, error),
        error);
    BOOST_CHECK(!equivocation);
    database.ClearFailureInjection();

    BOOST_REQUIRE_MESSAGE(
        store.StageAlternativeRecoveryCapacityProofClaimCandidate(
            recovery.recovery_id, second_proof, 200, equivocation, error),
        error);
    BOOST_CHECK(equivocation);
    PaymasterProviderBlock block;
    BOOST_REQUIRE(store.GetProviderBlock(recovery_provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::CAPACITY);
}

BOOST_AUTO_TEST_CASE(conflicting_capacity_proofs_are_evidence_and_block_provider)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440104";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256S("81"),
                                            FeeMode::PAYMASTER, 100, session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    const XOnlyPubKey provider_identity{provider_key.GetPubKey()};
    const PaymasterId provider_id = GetPaymasterId(provider_identity);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});

    PaymentIntent intent;
    intent.genesis_hash = uint256S("82");
    intent.provider_id = provider_id;
    intent.request_id = request_id;
    intent.session_id = session.session_id;
    intent.client_nonce = uint256S("83");
    intent.canonical_request_hash = session.canonical_request_hash;
    intent.requested_fee_mode = session.fee_mode_requested;
    intent.user_dd_inputs = {COutPoint{uint256S("84"), 0}};
    intent.recipient_script = GetScriptForDestination(recipient_builder.GetOutput());
    intent.recipient_amount = DDCents{100};
    intent.offer_id = uint256S("85");
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = uint256S("86");
    intent.expires_at = 150;
    CDataStream intent_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    intent_stream << intent;

    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("87");
    attempt.provider_id = provider_id;
    attempt.provider_identity_key = provider_identity;
    attempt.client_nonce = intent.client_nonce;
    const auto intent_bytes = MakeUCharSpan(intent_stream);
    attempt.unsigned_intent.assign(intent_bytes.begin(), intent_bytes.end());
    attempt.created_at = attempt.updated_at = 100;
    BOOST_REQUIRE_MESSAGE(store.PreparePaymentIntent(request_id, attempt, 100, error), error);

    PaymasterCapacityRequest request;
    request.genesis_hash = intent.genesis_hash;
    request.provider_id = provider_id;
    request.request_id = request_id;
    request.session_id = session.session_id;
    request.client_nonce = intent.client_nonce;
    request.funding_model = intent.funding_model;
    request.requires_carrier = false;
    request.created_at = 101;
    request.expires_at = 145;
    CDataStream request_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    request_stream << request;
    const auto request_span = MakeUCharSpan(request_stream);
    const std::vector<unsigned char> request_bytes{request_span.begin(), request_span.end()};
    BOOST_REQUIRE_MESSAGE(store.PrepareCapacityRequest(
                              request_id, attempt.attempt_id, request_bytes, 101, error),
                          error);

    CMutableTransaction capacity_funding;
    // A zero-input mutable transaction is ambiguous with the witness marker
    // during decoding. Give the synthetic creating transaction one input so
    // the embedded capacity proof has a canonical round trip.
    capacity_funding.vin.emplace_back(COutPoint{uint256S("88"), 0});
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
        dgb.input.outpoint = COutPoint{CTransaction{capacity_funding}.GetHash(), 0};
        dgb.input.creating_tx = capacity_funding;
        dgb.input.value = DGBSatoshis{1000};
        dgb.control_proof.reference_block = uint256S("89");
        dgb.control_proof.expires_at = proof.expires_at;
        dgb.control_proof.signature.assign(64, 2);
        PaymasterLiquiditySlot slot;
        slot.dgb_inputs = {dgb};
        proof.liquidity_slots = {slot};
        proof.identity_signature.resize(64);
        BOOST_CHECK(provider_key.SignSchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature,
            nullptr, uint256{}));
        return proof;
    };
    const auto make_snapshot = [&](const PaymasterCapacityProof& proof, int64_t validated_at) {
        CDataStream proof_stream{SER_NETWORK, ::PROTOCOL_VERSION};
        proof_stream << proof;
        const auto proof_span = MakeUCharSpan(proof_stream);
        ValidatedCapacitySnapshot snapshot;
        snapshot.snapshot_id = proof.snapshot_id;
        snapshot.resource_commitment = GetCapacityResourceCommitment(proof);
        snapshot.session_id = request.session_id;
        snapshot.attempt_id = attempt.attempt_id;
        snapshot.provider_id = proof.provider_id;
        snapshot.client_nonce = proof.client_nonce;
        snapshot.request_hash = Hash(request_bytes);
        snapshot.capacity_proof.assign(proof_span.begin(), proof_span.end());
        snapshot.created_at = proof.created_at;
        snapshot.expires_at = proof.expires_at;
        snapshot.validated_at = validated_at;
        snapshot.funding_model = proof.funding_model;
        snapshot.requires_carrier = proof.requires_carrier;
        return snapshot;
    };

    const PaymasterCapacityProof first_proof =
        make_proof(uint256S("8a"), 102, 140);
    const ValidatedCapacitySnapshot first_snapshot = make_snapshot(first_proof, 102);
    bool equivocation{false};
    BOOST_REQUIRE_MESSAGE(store.StageCapacityProofClaimCandidate(
                              attempt.attempt_id,
                              first_snapshot.capacity_proof, 102,
                              equivocation, error),
                          error);
    BOOST_CHECK(!equivocation);
    BOOST_REQUIRE_MESSAGE(store.CommitValidatedCapacitySnapshot(
                              request_id, attempt.attempt_id, first_snapshot, 102, error),
                          error);
    {
        LOCK(m_wallet.cs_wallet);
        CapacityResourceBinding binding;
        const COutPoint capacity_outpoint{
            CTransaction{capacity_funding}.GetHash(), 0};
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .ReadPaymasterCapacityResource(
                              provider_id, capacity_outpoint, binding));
        BOOST_CHECK(binding.outpoint == capacity_outpoint);
        BOOST_CHECK_EQUAL(binding.snapshot_id, first_snapshot.snapshot_id);
        BOOST_CHECK(!binding.carrier);
        BOOST_CHECK_EQUAL(binding.creating_txid, capacity_outpoint.hash);
        BOOST_CHECK_EQUAL(binding.value, 1000);
    }

    // A later signed proof whose validity begins exactly when the durable
    // proof expires is not an equivocation. Sequential reuse must not create a
    // permanent provider block.
    const PaymasterCapacityProof non_overlapping_proof =
        make_proof(uint256S("8c"), 140, 145);
    const ValidatedCapacitySnapshot non_overlapping_snapshot =
        make_snapshot(non_overlapping_proof, 141);
    BOOST_CHECK(!store.RecordCapacityEquivocation(
        attempt.attempt_id, non_overlapping_snapshot.capacity_proof, 141,
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION");
    PaymasterProviderBlock absent;
    BOOST_CHECK(store.GetProviderBlock(provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);

    // The conflicting proof was signed before the durable proof but observed
    // afterwards. Its overlapping signed validity window still makes the pair
    // an equivocation independent of arrival order.
    const PaymasterCapacityProof second_proof =
        make_proof(uint256S("8b"), 101, 130);
    const ValidatedCapacitySnapshot second_snapshot = make_snapshot(second_proof, 103);
    PaymasterCapacityProof invalid_signature_proof = second_proof;
    invalid_signature_proof.identity_signature.front() ^= 1;
    const ValidatedCapacitySnapshot invalid_signature_snapshot =
        make_snapshot(invalid_signature_proof, 103);
    BOOST_CHECK(!store.RecordCapacityEquivocation(
        attempt.attempt_id, invalid_signature_snapshot.capacity_proof, 103,
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION");
    BOOST_CHECK(store.GetProviderBlock(provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);
    // The durable first snapshot is authoritative here. Do not stage the
    // conflicting second proof as a new candidate before exercising the
    // snapshot-level equivocation firewall.
    BOOST_CHECK(!store.CommitValidatedCapacitySnapshot(
        request_id, attempt.attempt_id, second_snapshot, 103, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_EQUIVOCATION");

    PaymasterProviderBlock block;
    BOOST_REQUIRE(store.GetProviderBlock(provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::CAPACITY);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(store.GetEquivocationEvidence(block.evidence_id, evidence));
    BOOST_CHECK(evidence.provider_id == provider_id);
    BOOST_CHECK(evidence.semantic_key == Hash(request_bytes));
    BOOST_CHECK((evidence.first_artifact_hash == Hash(first_snapshot.capacity_proof) &&
                 evidence.second_artifact_hash == Hash(second_snapshot.capacity_proof)) ||
                (evidence.first_artifact_hash == Hash(second_snapshot.capacity_proof) &&
                 evidence.second_artifact_hash == Hash(first_snapshot.capacity_proof)));
    BOOST_REQUIRE_MESSAGE(store.RecordCapacityEquivocation(
                              attempt.attempt_id,
                              second_snapshot.capacity_proof, 103, error),
                          error);

    auto restarted_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet restarted_wallet{
        m_node.chain.get(), "capacity-equivocation-restart",
        std::move(restarted_database)};
    BOOST_REQUIRE(restarted_wallet.LoadWallet() == DBErrors::LOAD_OK);
    PaymasterStore restarted_store{restarted_wallet};
    PaymasterProviderBlock restarted_block;
    BOOST_REQUIRE(restarted_store.GetProviderBlock(provider_id,
                                                   restarted_block,
                                                   error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(restarted_block.evidence_id == block.evidence_id);
    BOOST_CHECK(restarted_block.kind == EquivocationKind::CAPACITY);
    PaymasterEquivocationEvidence restarted_evidence;
    BOOST_REQUIRE(restarted_store.GetEquivocationEvidence(
        restarted_block.evidence_id, restarted_evidence));
    BOOST_CHECK(restarted_evidence.provider_id == evidence.provider_id);
    BOOST_CHECK(restarted_evidence.semantic_key == evidence.semantic_key);
    BOOST_CHECK(restarted_evidence.first_artifact_hash ==
                evidence.first_artifact_hash);
    BOOST_CHECK(restarted_evidence.second_artifact_hash ==
                evidence.second_artifact_hash);

    PaymasterReliabilityRecord reliability;
    BOOST_REQUIRE(store.GetProviderReliability(provider_id, reliability));
    BOOST_CHECK_EQUAL(reliability.cooldown_until, std::numeric_limits<int64_t>::max());
    BOOST_REQUIRE(store.ClearProviderReliability(provider_id, error));
    BOOST_REQUIRE(store.GetProviderReliability(provider_id, reliability));
    BOOST_CHECK_EQUAL(reliability.cooldown_until, std::numeric_limits<int64_t>::max());
}

BOOST_AUTO_TEST_CASE(conflicting_signed_quotes_are_persisted_atomically)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440105";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256S("91"),
                                            FeeMode::PAYMASTER, 100, session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});
    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("92");
    attempt.provider_identity_key = XOnlyPubKey{provider_key.GetPubKey()};
    attempt.provider_id = GetPaymasterId(attempt.provider_identity_key);
    attempt.created_at = attempt.updated_at = 100;
    BOOST_REQUIRE(store.AddAttempt(request_id, attempt, error));
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, attempt));
    attempt.client_nonce = uint256S("93");
    PaymasterQuoteRequest authoritative_request;
    authoritative_request.intent.genesis_hash = uint256S("95");
    authoritative_request.intent.provider_id = attempt.provider_id;
    authoritative_request.intent.request_id = request_id;
    authoritative_request.intent.session_id = session.session_id;
    authoritative_request.intent.client_nonce = attempt.client_nonce;
    authoritative_request.intent.canonical_request_hash =
        session.canonical_request_hash;
    authoritative_request.intent.requested_fee_mode =
        session.fee_mode_requested;
    authoritative_request.intent.user_dd_inputs = {
        COutPoint{uint256S("94"), 0}};
    authoritative_request.intent.user_input_proofs = {
        {authoritative_request.intent.user_dd_inputs.front(),
         std::vector<unsigned char>(64, 1)}};
    authoritative_request.intent.recipient_script =
        GetScriptForDestination(recipient_builder.GetOutput());
    authoritative_request.intent.recipient_amount = DDCents{100};
    authoritative_request.intent.offer_id = uint256S("96");
    authoritative_request.intent.funding_model = FundingModel::SPONSORED;
    authoritative_request.intent.sponsorship_scope =
        SponsorshipScope::PUBLIC;
    authoritative_request.intent.policy_hash = uint256S("97");
    authoritative_request.intent.expires_at = 150;
    BOOST_REQUIRE_MESSAGE(ValidateRedactedQuoteRequestEnvelope(
                              authoritative_request,
                              authoritative_request.intent.genesis_hash,
                              attempt.created_at, error),
                          error);
    attempt.intent_hash =
        GetPaymentIntentHash(authoritative_request.intent);
    attempt.quote_request =
        SerializePaymasterTestObject(authoritative_request);

    const auto make_quote = [&](const uint256& quote_id,
                                const uint256& unsigned_txid,
                                const uint256& template_commitment,
                                int64_t created_at,
                                int64_t expires_at) {
        PaymasterQuoteResponse response;
        response.request_id = request_id;
        response.session_id = session.session_id;
        response.quote.genesis_hash = uint256S("95");
        response.quote.provider_id = attempt.provider_id;
        response.quote.quote_id = quote_id;
        response.quote.intent_hash = attempt.intent_hash;
        response.quote.offer_id = uint256S("96");
        response.quote.policy_hash = uint256S("97");
        response.quote.funding_model = FundingModel::SPONSORED;
        response.quote.sponsorship_scope = SponsorshipScope::PUBLIC;
        response.quote.fee_rate_bps = 0;
        response.quote.service_fee = DDCents{0};
        VerifiedDGBInput input;
        input.outpoint = COutPoint{uint256S("98"), 0};
        input.value = DGBSatoshis{1000};
        response.quote.reserved_dgb_inputs = {input};
        response.quote.network_fee = DGBSatoshis{10};
        response.quote.created_at = created_at;
        response.quote.expires_at = expires_at;
        response.quote.retry_until = 200;
        response.quote.unsigned_txid = unsigned_txid;
        response.quote.template_commitment = template_commitment;
        response.quote.identity_signature.resize(64);
        BOOST_CHECK(provider_key.SignSchnorr(
            GetPaymasterQuoteSignatureHash(response.quote),
            response.quote.identity_signature, nullptr, uint256{}));
        CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
        stream << response;
        const auto span = MakeUCharSpan(stream);
        return std::vector<unsigned char>{span.begin(), span.end()};
    };

    attempt.signed_quote = make_quote(uint256S("99"), uint256S("9a"),
                                      uint256S("9b"), 102, 140);
    attempt.updated_at = 101;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterAttempt(attempt));
    }
    const std::vector<unsigned char> non_overlapping_quote =
        make_quote(uint256S("9f"), uint256S("a0"), uint256S("a1"), 140,
                   160);
    BOOST_CHECK(!store.RecordQuoteEquivocation(
        attempt.attempt_id, non_overlapping_quote, 141, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_EQUIVOCATION");
    PaymasterProviderBlock absent;
    BOOST_CHECK(store.GetProviderBlock(attempt.provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);

    // As with capacity evidence, an earlier-signed quote received after the
    // durable quote is still conflicting while their validity windows overlap.
    const std::vector<unsigned char> conflicting_quote =
        make_quote(uint256S("9c"), uint256S("9d"), uint256S("9e"), 101,
                   130);

    BOOST_CHECK(!store.RecordQuoteEquivocation(attempt.attempt_id, {1, 2, 3}, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_EQUIVOCATION");
    BOOST_CHECK(store.GetProviderBlock(attempt.provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_evidence = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.RecordQuoteEquivocation(
        attempt.attempt_id, conflicting_quote, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_evidence);
    BOOST_CHECK(store.GetProviderBlock(attempt.provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);
    database.FailCommit();
    BOOST_CHECK(!store.RecordQuoteEquivocation(
        attempt.attempt_id, conflicting_quote, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_evidence);

    BOOST_REQUIRE_MESSAGE(store.RecordQuoteEquivocation(
                              attempt.attempt_id, conflicting_quote, 102, error),
                          error);
    PaymasterProviderBlock block;
    BOOST_REQUIRE(store.GetProviderBlock(attempt.provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::QUOTE);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(store.GetEquivocationEvidence(block.evidence_id, evidence));
    BOOST_CHECK(evidence.provider_id == attempt.provider_id);
    BOOST_CHECK(evidence.first_artifact_hash != evidence.second_artifact_hash);

    std::vector<PaymasterReliabilityRecord> reliability;
    BOOST_REQUIRE(store.ListProviderReliability(reliability));
    const auto blocked = std::find_if(
        reliability.begin(), reliability.end(), [&](const PaymasterReliabilityRecord& record) {
            return record.provider_id == attempt.provider_id;
        });
    BOOST_REQUIRE(blocked != reliability.end());
    BOOST_CHECK_EQUAL(blocked->cooldown_until, std::numeric_limits<int64_t>::max());
}

BOOST_AUTO_TEST_CASE(pending_quote_pair_requires_exact_client_binding)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440205";
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256S("8101"), FeeMode::PAYMASTER, 100,
                      session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    CKey provider_key;
    CKey recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    TaprootBuilder recipient_builder;
    recipient_builder.Finalize(XOnlyPubKey{recipient_key.GetPubKey()});
    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("8102");
    attempt.provider_identity_key = XOnlyPubKey{provider_key.GetPubKey()};
    attempt.provider_id = GetPaymasterId(attempt.provider_identity_key);
    attempt.created_at = attempt.updated_at = 100;
    ProviderAttempt poisoned_attempt{attempt};
    poisoned_attempt.quote_response_claim_candidate = {1, 2, 3};
    BOOST_CHECK(!store.AddAttempt(request_id, poisoned_attempt, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_ATTEMPT");
    BOOST_REQUIRE(store.AddAttempt(request_id, attempt, error));
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, attempt));
    attempt.client_nonce = uint256S("8103");
    PaymasterQuoteRequest authoritative_request;
    authoritative_request.intent.genesis_hash = uint256S("8105");
    authoritative_request.intent.provider_id = attempt.provider_id;
    authoritative_request.intent.request_id = request_id;
    authoritative_request.intent.session_id = session.session_id;
    authoritative_request.intent.client_nonce = attempt.client_nonce;
    authoritative_request.intent.canonical_request_hash =
        session.canonical_request_hash;
    authoritative_request.intent.requested_fee_mode =
        session.fee_mode_requested;
    authoritative_request.intent.user_dd_inputs = {
        COutPoint{uint256S("8104"), 0}};
    authoritative_request.intent.user_input_proofs = {
        {authoritative_request.intent.user_dd_inputs.front(),
         std::vector<unsigned char>(64, 1)}};
    authoritative_request.intent.recipient_script =
        GetScriptForDestination(recipient_builder.GetOutput());
    authoritative_request.intent.recipient_amount = DDCents{100};
    authoritative_request.intent.offer_id = uint256S("8106");
    authoritative_request.intent.funding_model = FundingModel::SPONSORED;
    authoritative_request.intent.sponsorship_scope =
        SponsorshipScope::PUBLIC;
    authoritative_request.intent.policy_hash = uint256S("8107");
    authoritative_request.intent.expires_at = 150;
    attempt.intent_hash =
        GetPaymentIntentHash(authoritative_request.intent);
    attempt.quote_request =
        SerializePaymasterTestObject(authoritative_request);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(attempt));
    }

    const auto make_quote = [&](const std::string& bound_request_id,
                                const uint256& bound_session_id,
                                const PaymasterId& bound_provider_id,
                                const uint256& bound_intent_hash,
                                const uint256& bound_genesis_hash,
                                const uint256& quote_id,
                                int64_t created_at,
                                int64_t expires_at) {
        PaymasterQuoteResponse response;
        response.request_id = bound_request_id;
        response.session_id = bound_session_id;
        response.quote.genesis_hash = bound_genesis_hash;
        response.quote.provider_id = bound_provider_id;
        response.quote.quote_id = quote_id;
        response.quote.intent_hash = bound_intent_hash;
        response.quote.offer_id = uint256S("8106");
        response.quote.policy_hash = uint256S("8107");
        response.quote.funding_model = FundingModel::SPONSORED;
        response.quote.sponsorship_scope = SponsorshipScope::PUBLIC;
        response.quote.fee_rate_bps = 0;
        response.quote.service_fee = DDCents{0};
        VerifiedDGBInput input;
        input.outpoint = COutPoint{uint256S("8108"), 0};
        input.value = DGBSatoshis{1000};
        response.quote.reserved_dgb_inputs = {input};
        response.quote.network_fee = DGBSatoshis{10};
        response.quote.created_at = created_at;
        response.quote.expires_at = expires_at;
        response.quote.retry_until = 200;
        response.quote.unsigned_txid = quote_id;
        response.quote.template_commitment = Hash(quote_id);
        response.quote.identity_signature.resize(64);
        BOOST_CHECK(provider_key.SignSchnorr(
            GetPaymasterQuoteSignatureHash(response.quote),
            response.quote.identity_signature, nullptr, uint256{}));
        return SerializePaymasterTestObject(response);
    };

    const std::vector<unsigned char> first_quote = make_quote(
        request_id, session.session_id, attempt.provider_id,
        attempt.intent_hash, uint256S("8105"), uint256S("8109"), 102,
        140);
    const std::vector<unsigned char> second_quote = make_quote(
        request_id, session.session_id, attempt.provider_id,
        attempt.intent_hash, uint256S("8105"), uint256S("810a"), 103,
        139);
    const std::vector<unsigned char> wrong_request_quote = make_quote(
        "550e8400-e29b-41d4-a716-446655440206", session.session_id,
        attempt.provider_id, attempt.intent_hash, uint256S("8105"),
        uint256S("810b"), 103, 139);
    const std::vector<unsigned char> wrong_genesis_quote = make_quote(
        request_id, session.session_id, attempt.provider_id,
        attempt.intent_hash, uint256S("81ff"), uint256S("810c"), 103,
        139);

    ProviderAttempt corrupt_authority{attempt};
    corrupt_authority.quote_request = {1, 2, 3};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(corrupt_authority));
    }
    bool equivocation{false};
    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, first_quote, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PERSISTED_QUOTE_REQUEST_CORRUPT");
    BOOST_CHECK(!store.RecordPendingQuoteEquivocation(
        attempt.attempt_id, first_quote, second_quote, 104, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PERSISTED_QUOTE_REQUEST_CORRUPT");
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(attempt));
    }

    ProviderAttempt missing_candidate_update{attempt};
    missing_candidate_update.signed_quote = first_quote;
    missing_candidate_update.updated_at = 104;
    BOOST_CHECK(!store.UpdateAttempt(request_id, missing_candidate_update,
                                     error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_CLAIM_CANDIDATE_MISSING");

    ProviderAttempt corrupt_candidate{attempt};
    corrupt_candidate.quote_response_claim_candidate = {1, 2, 3};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(corrupt_candidate));
    }
    ProviderAttempt corrupt_candidate_update{missing_candidate_update};
    corrupt_candidate_update.quote_response_claim_candidate = {1, 2, 3};
    BOOST_CHECK(!store.UpdateAttempt(request_id, corrupt_candidate_update,
                                     error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_QUOTE_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, {4, 5, 6}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_QUOTE_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!equivocation);

    ProviderAttempt differently_bound_candidate{attempt};
    differently_bound_candidate.quote_response_claim_candidate =
        wrong_request_quote;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(
                              differently_bound_candidate));
    }
    ProviderAttempt differently_bound_candidate_update{
        missing_candidate_update};
    differently_bound_candidate_update.quote_response_claim_candidate =
        wrong_request_quote;
    BOOST_CHECK(!store.UpdateAttempt(
        request_id, differently_bound_candidate_update, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_QUOTE_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, {4, 5, 6}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PERSISTED_QUOTE_CLAIM_CANDIDATE_CORRUPT");
    BOOST_CHECK(!equivocation);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(attempt));
    }

    BOOST_CHECK(!store.RecordPendingQuoteEquivocation(
        attempt.attempt_id, first_quote, wrong_request_quote, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_EQUIVOCATION");
    PaymasterEquivocationEvidence pending;
    BOOST_CHECK(store.GetPendingEquivocation(attempt.provider_id, pending,
                                             error) ==
                DatabaseReadStatus::NOT_FOUND);
    PaymasterProviderBlock absent;
    BOOST_CHECK(store.GetProviderBlock(attempt.provider_id, absent, error) ==
                DatabaseReadStatus::NOT_FOUND);

    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, {1, 2, 3}, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE");
    BOOST_CHECK(!equivocation);
    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, wrong_genesis_quote, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE");
    BOOST_CHECK(!equivocation);
    ProviderAttempt unpoisoned;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, unpoisoned));
    BOOST_CHECK(unpoisoned.quote_response_claim_candidate.empty());
    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, wrong_request_quote, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE");
    BOOST_CHECK(!equivocation);

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_candidate = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, first_quote, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_candidate);
    database.FailCommit();
    BOOST_CHECK(!store.StageQuoteResponseClaimCandidate(
        attempt.attempt_id, first_quote, 104, equivocation, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_candidate);

    BOOST_REQUIRE_MESSAGE(store.StageQuoteResponseClaimCandidate(
                              attempt.attempt_id, first_quote, 104,
                              equivocation, error),
                          error);
    BOOST_CHECK(!equivocation);
    ProviderAttempt candidate_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, candidate_attempt));
    BOOST_CHECK(candidate_attempt.quote_response_claim_candidate ==
                first_quote);
    BOOST_CHECK(candidate_attempt.signed_quote.empty());

    database.FailWriteAt(0);
    BOOST_REQUIRE_MESSAGE(store.StageQuoteResponseClaimCandidate(
                              attempt.attempt_id, first_quote, 250,
                              equivocation, error),
                          error);
    BOOST_CHECK(!equivocation);
    database.ClearFailureInjection();

    BOOST_REQUIRE_MESSAGE(store.StageQuoteResponseClaimCandidate(
                              attempt.attempt_id, second_quote, 250,
                              equivocation, error),
                          error);
    BOOST_CHECK(equivocation);
    PaymasterProviderBlock block;
    BOOST_REQUIRE(store.GetProviderBlock(attempt.provider_id, block, error) ==
                  DatabaseReadStatus::FOUND);
    BOOST_CHECK(block.kind == EquivocationKind::QUOTE);
    PaymasterEquivocationEvidence evidence;
    BOOST_REQUIRE(store.GetEquivocationEvidence(block.evidence_id, evidence));
    BOOST_CHECK(evidence.provider_id == attempt.provider_id);
    BOOST_CHECK((evidence.first_artifact_hash == Hash(first_quote) &&
                 evidence.second_artifact_hash == Hash(second_quote)) ||
                (evidence.first_artifact_hash == Hash(second_quote) &&
                 evidence.second_artifact_hash == Hash(first_quote)));

    ProviderAttempt unchanged;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, unchanged));
    BOOST_CHECK(unchanged.signed_quote.empty());
    BOOST_CHECK(unchanged.quote_response_claim_candidate == first_quote);
}

BOOST_AUTO_TEST_CASE(client_result_is_atomic_monotonic_and_final_artifacts_are_immutable)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440007";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            session, error) == CreatePaymasterSessionResult::CREATED);

    ValidClientResultArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(BuildValidClientResultArtifacts(
                              m_wallet, session, request_id, uint256S("51"),
                              101, artifacts, error),
                          error);
    ProviderAttempt& attempt = artifacts.attempt;
    CMutableTransaction& transaction = artifacts.final_transaction;
    BOOST_REQUIRE(store.ReserveInputs(
        request_id, {{artifacts.user_input, ReservationRole::USER_DD}},
        FeeMode::PAYMASTER, 101, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    session.attempt_ids.push_back(attempt.attempt_id);
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
    session.updated_at = 101;

    ClientSafetyPolicy client_policy;
    client_policy.maximum_service_fee_per_transaction = DDCents{0};
    client_policy.maximum_service_fee_per_day = DDCents{0};
    client_policy.updated_at = 100;
    ClientFeeLedger client_fee_ledger;
    client_fee_ledger.accounting_time_high_water = 100;
    BOOST_REQUIRE(ReserveClientFee(
        client_fee_ledger, client_policy, attempt.commit_key,
        DDCents{0}, 101, error));
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterClientSafetyPolicy(client_policy));
        BOOST_REQUIRE(batch.WritePaymasterClientFeeLedger(client_fee_ledger));
    }
    BOOST_CHECK(!m_wallet.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));

    PaymasterResult result;
    result.genesis_hash = artifacts.genesis_hash;
    result.provider_id = attempt.provider_id;
    result.commit_key = attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = attempt.unsigned_txid;
    result.raw_transaction_hash = CTransaction{transaction}.GetWitnessHash();
    result.final_transaction = transaction;
    result.updated_at = 102;
    const auto sign_result = [&](PaymasterResult& signed_result) {
        signed_result.identity_signature.assign(64, 0);
        BOOST_REQUIRE(artifacts.provider_identity_key.SignSchnorr(
            GetPaymasterResultSignatureHash(signed_result),
            signed_result.identity_signature, nullptr, uint256{}));
    };
    sign_result(result);

    auto& database = GetMockableDatabase(m_wallet);
    DataStream result_key_stream;
    result_key_stream << std::make_pair(DBKeys::PAYMASTER_RESULT,
                                        attempt.commit_key);
    const SerializeData result_key{result_key_stream.begin(),
                                   result_key_stream.end()};
    database.m_records[result_key] = SerializeData{std::byte{0xff}};
    const MockableData with_corrupt_result = database.m_records;
    database.ClearFailureInjection();
    BOOST_CHECK(!store.StoreClientResult(
        result, result.genesis_hash, attempt.attempt_id, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PERSISTED_RESULT_CORRUPT");
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    BOOST_CHECK(database.m_records == with_corrupt_result);
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        PaymasterResult unreadable;
        BOOST_CHECK(!batch.ReadPaymasterResult(attempt.commit_key, unreadable));
        BOOST_CHECK(batch.HasPaymasterResult(attempt.commit_key));
    }
    database.m_records.erase(result_key);

    const MockableData before_result = database.m_records;
    const auto check_atomic_state_unchanged = [&] {
        BOOST_CHECK(database.m_records == before_result);
        BOOST_CHECK(!m_wallet.IsWalletFlagSet(
            WALLET_FLAG_PAYMASTER_AUTHORIZATION));
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        ClientFeeLedger persisted_ledger;
        BOOST_REQUIRE(batch.ReadPaymasterClientFeeLedger(persisted_ledger));
        BOOST_REQUIRE_EQUAL(persisted_ledger.reservations.size(), 1U);
        BOOST_CHECK(persisted_ledger.reservations.front().state ==
                    BudgetReservationState::RESERVED);
        InputReservation persisted_reservation;
        BOOST_REQUIRE(batch.ReadPaymasterReservation(
            artifacts.user_input, persisted_reservation));
        BOOST_CHECK(!persisted_reservation.authorization_may_exist);
    };
    constexpr size_t transactional_writes{7};
    for (size_t failing_write = 0; failing_write < transactional_writes;
         ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.StoreClientResult(result, result.genesis_hash, attempt.attempt_id, 102, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        check_atomic_state_unchanged();
    }
    database.FailCommit();
    BOOST_CHECK(!store.StoreClientResult(result, result.genesis_hash, attempt.attempt_id, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    check_atomic_state_unchanged();

    // The seven durable updates include the wallet flag. Keeping the next
    // write armed proves the post-commit RAM publication performs no second
    // database write.
    database.FailWriteAt(transactional_writes);
    BOOST_REQUIRE(store.StoreClientResult(
        result, result.genesis_hash, attempt.attempt_id, 102, error));
    BOOST_CHECK_EQUAL(database.m_write_count, transactional_writes);
    BOOST_CHECK(m_wallet.IsWalletFlagSet(
        WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    database.ClearFailureInjection();

    DataStream flags_key_stream;
    flags_key_stream << DBKeys::FLAGS;
    const SerializeData flags_key{flags_key_stream.begin(),
                                  flags_key_stream.end()};
    const auto flags_record = database.m_records.find(flags_key);
    BOOST_REQUIRE(flags_record != database.m_records.end());
    CDataStream flags_value{SER_DISK, CLIENT_VERSION};
    flags_value.write(flags_record->second);
    uint64_t persisted_wallet_flags{0};
    flags_value >> persisted_wallet_flags;
    BOOST_CHECK((persisted_wallet_flags &
                 WALLET_FLAG_PAYMASTER_AUTHORIZATION) != 0);

    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        ClientFeeLedger persisted_ledger;
        BOOST_REQUIRE(batch.ReadPaymasterClientFeeLedger(persisted_ledger));
        BOOST_REQUIRE_EQUAL(persisted_ledger.reservations.size(), 1U);
        BOOST_CHECK(persisted_ledger.reservations.front().state ==
                    BudgetReservationState::SPENT);
        InputReservation persisted_reservation;
        BOOST_REQUIRE(batch.ReadPaymasterReservation(
            artifacts.user_input, persisted_reservation));
        BOOST_CHECK(persisted_reservation.authorization_may_exist);
    }

    const MockableData after_result = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.StoreClientResult(
        result, result.genesis_hash, attempt.attempt_id, 102, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == after_result);

    PaymasterResult persisted;
    BOOST_REQUIRE(store.GetProviderResult(attempt.commit_key, persisted));
    BOOST_REQUIRE(persisted.txid);
    BOOST_REQUIRE(result.txid);
    BOOST_CHECK_EQUAL(*persisted.txid, *result.txid);
    BOOST_CHECK(persisted.status == PaymasterResultStatus::FINAL_COMMITTED);
    ProviderAttempt persisted_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.state == AttemptState::PROVIDER_SIGNED);
    BOOST_CHECK_EQUAL(persisted_attempt.final_txid, attempt.unsigned_txid);
    CDataStream final_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    final_stream << transaction;
    const auto final_bytes = MakeUCharSpan(final_stream);
    BOOST_CHECK(persisted_attempt.final_transaction ==
                std::vector<unsigned char>(final_bytes.begin(), final_bytes.end()));
    PaymentSession persisted_session;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, persisted_session));
    BOOST_CHECK_EQUAL(persisted_session.final_txid, attempt.unsigned_txid);
    BOOST_CHECK(persisted_session.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(persisted_session.pending_phase == PendingPhase::PROVIDER_SIGNED_KNOWN);
    UserAuthorizationRecord persisted_authorization;
    BOOST_REQUIRE(store.GetUserAuthorization(attempt.commit_key, persisted_authorization));
    BOOST_CHECK_EQUAL(persisted_authorization.canonical_psbt_hash,
                      Hash(attempt.user_signed_psbt));

    PaymasterResult rejected = result;
    rejected.result_sequence = 2;
    rejected.status = PaymasterResultStatus::REJECTED;
    rejected.txid.reset();
    rejected.raw_transaction_hash.reset();
    rejected.final_transaction.reset();
    rejected.updated_at = 103;
    sign_result(rejected);
    BOOST_CHECK(!store.StoreClientResult(rejected, rejected.genesis_hash, attempt.attempt_id, 103, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RESULT_SEQUENCE_REGRESSION");

    CMutableTransaction conflicting_transaction = transaction;
    conflicting_transaction.vin[0].scriptWitness.stack = {{0x02}};
    BOOST_CHECK_EQUAL(CTransaction{conflicting_transaction}.GetHash(), attempt.unsigned_txid);
    BOOST_CHECK(CTransaction{conflicting_transaction}.GetWitnessHash() !=
                CTransaction{transaction}.GetWitnessHash());
    PaymasterResult conflicting = result;
    conflicting.result_sequence = 2;
    conflicting.raw_transaction_hash = CTransaction{conflicting_transaction}.GetWitnessHash();
    conflicting.final_transaction = conflicting_transaction;
    conflicting.updated_at = 104;
    sign_result(conflicting);
    BOOST_CHECK(!store.StoreClientResult(conflicting, conflicting.genesis_hash,
                                         attempt.attempt_id, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINAL_TX_CONFLICT");

    PaymasterResult broadcast = result;
    broadcast.result_sequence = 2;
    broadcast.status = PaymasterResultStatus::BROADCAST_ATTEMPTED;
    broadcast.updated_at = 105;
    sign_result(broadcast);
    BOOST_REQUIRE(store.StoreClientResult(broadcast, broadcast.genesis_hash,
                                          attempt.attempt_id, 105, error));
    BOOST_REQUIRE(store.GetProviderResult(attempt.commit_key, persisted));
    BOOST_CHECK(persisted.status == PaymasterResultStatus::BROADCAST_ATTEMPTED);
    BOOST_CHECK_EQUAL(persisted.result_sequence, 2U);

    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterSession(request_id, persisted_session));
        persisted_session.state = SessionState::CONFIRMED;
        persisted_session.pending_phase = PendingPhase::NONE;
        BOOST_REQUIRE(batch.WritePaymasterSession(persisted_session));
    }
    rejected.result_sequence = 3;
    rejected.updated_at = 106;
    sign_result(rejected);
    BOOST_CHECK(!store.StoreClientResult(rejected, rejected.genesis_hash,
                                         attempt.attempt_id, 106, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RESULT_SEQUENCE_REGRESSION");
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, persisted_session));
    BOOST_CHECK(persisted_session.state == SessionState::CONFIRMED);
    BOOST_REQUIRE(store.GetProviderResult(attempt.commit_key, persisted));
    BOOST_CHECK(persisted.status == PaymasterResultStatus::BROADCAST_ATTEMPTED);
    BOOST_CHECK_EQUAL(persisted.result_sequence, 2U);
}

BOOST_AUTO_TEST_CASE(manifestless_client_final_exact_replay_is_rejected_read_only)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-44665544000b";
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                      session, error) == CreatePaymasterSessionResult::CREATED);

    ValidClientResultArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(BuildValidClientResultArtifacts(
                              m_wallet, session, request_id, uint256S("7b"),
                              101, artifacts, error),
                          error);
    ProviderAttempt& attempt = artifacts.attempt;
    const ClientAuthorizationManifest current_manifest{
        attempt.client_manifest};
    const CTransaction final_transaction{artifacts.final_transaction};
    const std::vector<unsigned char> final_bytes =
        SerializePaymasterTestObject(artifacts.final_transaction);

    BOOST_REQUIRE(store.ReserveInputs(
        request_id, {{artifacts.user_input, ReservationRole::USER_DD}},
        FeeMode::PAYMASTER, 101, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    session.attempt_ids = {attempt.attempt_id};
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
    session.updated_at = 101;

    // Model an old durable attempt that predates manifest acceptance. A new
    // remote result must never be allowed to turn this into fresh authority.
    attempt.client_manifest = ClientAuthorizationManifest{};
    attempt.accepted_client_manifest_id.SetNull();
    attempt.client_manifest_accepted_at = 0;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
    }

    PaymasterResult result;
    result.genesis_hash = artifacts.genesis_hash;
    result.provider_id = attempt.provider_id;
    result.commit_key = attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = final_transaction.GetHash();
    result.raw_transaction_hash = final_transaction.GetWitnessHash();
    result.final_transaction = artifacts.final_transaction;
    result.updated_at = 102;
    const auto sign_result = [&](PaymasterResult& signed_result) {
        signed_result.identity_signature.assign(64, 0);
        BOOST_REQUIRE(artifacts.provider_identity_key.SignSchnorr(
            GetPaymasterResultSignatureHash(signed_result),
            signed_result.identity_signature, nullptr, uint256{}));
    };
    sign_result(result);

    auto& database = GetMockableDatabase(m_wallet);
    MockableData before_rejected = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StoreClientResult(
        result, result.genesis_hash, attempt.attempt_id, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED");
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_rejected);

    // A current manifest is also insufficient until its exact acceptance has
    // been durably committed by the client wallet.
    attempt.client_manifest = current_manifest;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
    }
    before_rejected = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StoreClientResult(
        result, result.genesis_hash, attempt.attempt_id, 102, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED");
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_rejected);

    // Seed an exact result/attempt/session triple without current durable
    // authorization. Exact replay must not synthesize authorization state.
    attempt.client_manifest = ClientAuthorizationManifest{};
    attempt.state = AttemptState::PROVIDER_SIGNED;
    attempt.final_txid = final_transaction.GetHash();
    attempt.final_transaction = final_bytes;
    attempt.updated_at = 102;
    session.pending_phase = PendingPhase::PROVIDER_SIGNED_KNOWN;
    session.final_txid = final_transaction.GetHash();
    session.updated_at = 102;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterResult(result, false));
    }

    const MockableData before_exact_replay = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StoreClientResult(
        result, result.genesis_hash, attempt.attempt_id, 103, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED");
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_exact_replay);
    UserAuthorizationRecord authorization;
    BOOST_CHECK(!store.GetUserAuthorization(attempt.commit_key, authorization));
    ProviderAttempt persisted_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, persisted_attempt));
    BOOST_CHECK(persisted_attempt.client_manifest.manifest_id.IsNull());
    BOOST_CHECK(persisted_attempt.accepted_client_manifest_id.IsNull());
    BOOST_CHECK_EQUAL(persisted_attempt.client_manifest_accepted_at, 0);

    // A validly signed follow-up is likewise unable to create authority.
    PaymasterResult follow_up{result};
    follow_up.result_sequence = 2;
    follow_up.status = PaymasterResultStatus::BROADCAST_ATTEMPTED;
    follow_up.updated_at = 104;
    sign_result(follow_up);
    database.FailWriteAt(0);
    BOOST_CHECK(!store.StoreClientResult(
        follow_up, follow_up.genesis_hash, attempt.attempt_id, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED");
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_exact_replay);
}

BOOST_AUTO_TEST_CASE(client_final_observation_is_atomic_and_idempotent)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-44665544000a";
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                      session, error) == CreatePaymasterSessionResult::CREATED);

    ValidClientResultArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(BuildValidClientResultArtifacts(
                              m_wallet, session, request_id, uint256S("71"),
                              101, artifacts, error),
                          error);
    ProviderAttempt& attempt = artifacts.attempt;
    const CTransaction final_transaction{artifacts.final_transaction};
    attempt.state = AttemptState::PROVIDER_SIGNED;
    attempt.final_txid = final_transaction.GetHash();
    attempt.final_transaction =
        SerializePaymasterTestObject(artifacts.final_transaction);
    session.attempt_ids = {attempt.attempt_id};
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::PROVIDER_SIGNED_KNOWN;
    session.final_txid = attempt.final_txid;
    session.updated_at = 102;

    PaymasterResult result;
    result.genesis_hash = artifacts.genesis_hash;
    result.provider_id = attempt.provider_id;
    result.commit_key = attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = final_transaction.GetHash();
    result.raw_transaction_hash = final_transaction.GetWitnessHash();
    result.final_transaction = artifacts.final_transaction;
    result.updated_at = 102;
    result.identity_signature.assign(64, 0);
    BOOST_REQUIRE(artifacts.provider_identity_key.SignSchnorr(
        GetPaymasterResultSignatureHash(result), result.identity_signature,
        nullptr, uint256{}));
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterResult(result, false));
    }

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_observation = database.m_records;
    for (size_t failing_write = 0; failing_write < 2; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.RecordClientFinalObservation(
            request_id, attempt.attempt_id, false, 103, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_observation);
    }
    database.FailCommit();
    BOOST_CHECK(!store.RecordClientFinalObservation(
        request_id, attempt.attempt_id, false, 103, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_observation);

    BOOST_REQUIRE(store.RecordClientFinalObservation(
        request_id, attempt.attempt_id, false, 103, error));
    ProviderAttempt observed_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, observed_attempt));
    BOOST_CHECK(observed_attempt.state == AttemptState::MEMPOOL);
    PaymentSession observed_session;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, observed_session));
    BOOST_CHECK(observed_session.state == SessionState::MEMPOOL);
    BOOST_CHECK(observed_session.pending_phase == PendingPhase::NONE);

    const MockableData after_mempool = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.RecordClientFinalObservation(
        request_id, attempt.attempt_id, false, 104, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == after_mempool);

    database.FailWriteAt(0);
    BOOST_CHECK(!store.RecordClientFinalObservation(
        request_id, attempt.attempt_id, true, 105, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == after_mempool);
    database.FailCommit();
    BOOST_CHECK(!store.RecordClientFinalObservation(
        request_id, attempt.attempt_id, true, 105, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == after_mempool);

    BOOST_REQUIRE(store.RecordClientFinalObservation(
        request_id, attempt.attempt_id, true, 105, error));
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, observed_attempt));
    BOOST_CHECK(observed_attempt.state == AttemptState::MEMPOOL);
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, observed_session));
    BOOST_CHECK(observed_session.state == SessionState::CONFIRMED);
    BOOST_CHECK(observed_session.pending_phase == PendingPhase::NONE);

    const MockableData after_confirmed = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.RecordClientFinalObservation(
        request_id, attempt.attempt_id, true, 106, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == after_confirmed);
}

BOOST_AUTO_TEST_CASE(confirmed_session_rejects_first_negative_client_result)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440008";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                                            session, error) == CreatePaymasterSessionResult::CREATED);

    CKey identity_key;
    identity_key.MakeNewKey(true);
    ProviderAttempt attempt;
    attempt.session_id = session.session_id;
    attempt.attempt_id = uint256S("54");
    attempt.provider_identity_key = XOnlyPubKey{identity_key.GetPubKey()};
    attempt.provider_id = GetPaymasterId(attempt.provider_identity_key);
    attempt.commit_key = uint256S("55");
    attempt.unsigned_txid = uint256S("56");
    attempt.state = AttemptState::AMBIGUOUS;
    attempt.created_at = attempt.updated_at = 101;
    session.attempt_ids.push_back(attempt.attempt_id);
    session.state = SessionState::CONFIRMED;
    session.final_txid = attempt.unsigned_txid;
    session.updated_at = 102;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
    }

    PaymasterResult rejected;
    rejected.genesis_hash = uint256S("57");
    rejected.provider_id = attempt.provider_id;
    rejected.commit_key = attempt.commit_key;
    rejected.result_sequence = 1;
    rejected.status = PaymasterResultStatus::REJECTED;
    rejected.updated_at = 103;
    rejected.identity_signature.assign(64, 0);
    BOOST_REQUIRE(identity_key.SignSchnorr(GetPaymasterResultSignatureHash(rejected),
                                           rejected.identity_signature, nullptr, uint256{}));
    BOOST_CHECK(!store.StoreClientResult(rejected, rejected.genesis_hash,
                                         attempt.attempt_id, 104, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RESULT_SEQUENCE_REGRESSION");
    PaymasterResult absent;
    BOOST_CHECK(!store.GetProviderResult(attempt.commit_key, absent));
    ProviderAttempt unchanged_attempt;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, unchanged_attempt));
    BOOST_CHECK(unchanged_attempt.state == AttemptState::AMBIGUOUS);
    PaymentSession unchanged_session;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, unchanged_session));
    BOOST_CHECK(unchanged_session.state == SessionState::CONFIRMED);
    BOOST_CHECK_EQUAL(unchanged_session.final_txid, attempt.unsigned_txid);
}

BOOST_AUTO_TEST_CASE(negative_result_after_user_signature_remains_recoverable)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655440009";
    BOOST_REQUIRE(store.CreateOrJoinSession(request_id, uint256::ONE,
                                            FeeMode::PAYMASTER, 100,
                                            session, error) ==
                  CreatePaymasterSessionResult::CREATED);

    ValidClientResultArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(BuildValidClientResultArtifacts(
                              m_wallet, session, request_id, uint256S("61"),
                              101, artifacts, error),
                          error);
    ProviderAttempt& attempt = artifacts.attempt;
    CMutableTransaction& transaction = artifacts.final_transaction;
    session.attempt_ids.push_back(attempt.attempt_id);
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
    session.updated_at = 101;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
    }

    const auto sign_result = [&](PaymasterResult& result) {
        result.identity_signature.assign(64, 0);
        BOOST_REQUIRE(artifacts.provider_identity_key.SignSchnorr(
            GetPaymasterResultSignatureHash(result), result.identity_signature,
            nullptr, uint256{}));
    };
    PaymasterResult rejected;
    rejected.genesis_hash = artifacts.genesis_hash;
    rejected.provider_id = attempt.provider_id;
    rejected.commit_key = attempt.commit_key;
    rejected.result_sequence = 1;
    rejected.status = PaymasterResultStatus::REJECTED;
    rejected.updated_at = 102;
    sign_result(rejected);
    BOOST_REQUIRE(store.StoreClientResult(
        rejected, rejected.genesis_hash, attempt.attempt_id, 102, error));

    ProviderAttempt ambiguous;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, ambiguous));
    BOOST_CHECK(ambiguous.state == AttemptState::AMBIGUOUS);
    PaymentSession pending;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, pending));
    BOOST_CHECK(pending.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(pending.pending_phase == PendingPhase::USER_SIGNATURE_SENT);

    PaymasterResult final{rejected};
    final.result_sequence = 2;
    final.status = PaymasterResultStatus::FINAL_COMMITTED;
    final.txid = attempt.unsigned_txid;
    final.raw_transaction_hash = CTransaction{transaction}.GetWitnessHash();
    final.final_transaction = transaction;
    final.updated_at = 103;
    sign_result(final);
    BOOST_REQUIRE_MESSAGE(store.StoreClientResult(
                              final, final.genesis_hash, attempt.attempt_id,
                              103, error),
                          error);

    ProviderAttempt recovered;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, recovered));
    BOOST_CHECK(recovered.state == AttemptState::PROVIDER_SIGNED);
    BOOST_CHECK_EQUAL(recovered.final_txid, attempt.unsigned_txid);
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, pending));
    BOOST_CHECK(pending.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(pending.pending_phase == PendingPhase::PROVIDER_SIGNED_KNOWN);
    UserAuthorizationRecord authorization;
    BOOST_REQUIRE(store.GetUserAuthorization(attempt.commit_key, authorization));
    BOOST_CHECK_EQUAL(authorization.canonical_psbt_hash,
                      Hash(attempt.user_signed_psbt));
}

BOOST_AUTO_TEST_CASE(client_pre_store_final_validation_failure_is_atomic_and_recoverable)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-44665544aa0f";
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                      session, error) == CreatePaymasterSessionResult::CREATED);

    ValidClientResultArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(BuildValidClientResultArtifacts(
                              m_wallet, session, request_id, uint256S("aa0f"),
                              101, artifacts, error),
                          error);
    ProviderAttempt attempt{artifacts.attempt};
    BOOST_REQUIRE(store.ReserveInputs(
        request_id, {{artifacts.user_input, ReservationRole::USER_DD}},
        FeeMode::PAYMASTER, 101, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    session.attempt_ids = {attempt.attempt_id};
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
    session.updated_at = 102;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        InputReservation reservation;
        BOOST_REQUIRE(batch.ReadPaymasterReservation(
            artifacts.user_input, reservation));
        BOOST_CHECK(!reservation.authorization_may_exist);
    }
    BOOST_CHECK(!m_wallet.IsWalletFlagSet(
        WALLET_FLAG_PAYMASTER_AUTHORIZATION));

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_failure = database.m_records;
    constexpr size_t transactional_writes{4};
    for (size_t failing_write = 0; failing_write < transactional_writes;
         ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.MarkClientFinalValidationFailureRecoverable(
            request_id, attempt.attempt_id, 103, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_failure);
        BOOST_CHECK(!m_wallet.IsWalletFlagSet(
            WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    }
    database.FailCommit();
    BOOST_CHECK(!store.MarkClientFinalValidationFailureRecoverable(
        request_id, attempt.attempt_id, 103, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_failure);
    BOOST_CHECK(!m_wallet.IsWalletFlagSet(
        WALLET_FLAG_PAYMASTER_AUTHORIZATION));

    database.FailWriteAt(transactional_writes);
    BOOST_REQUIRE_MESSAGE(store.MarkClientFinalValidationFailureRecoverable(
                              request_id, attempt.attempt_id, 103, error),
                          error);
    BOOST_CHECK_EQUAL(database.m_write_count, transactional_writes);
    database.ClearFailureInjection();
    BOOST_CHECK(m_wallet.IsWalletFlagSet(
        WALLET_FLAG_PAYMASTER_AUTHORIZATION));

    ProviderAttempt ambiguous;
    PaymentSession pending;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, ambiguous));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, pending));
    BOOST_CHECK(ambiguous.state == AttemptState::AMBIGUOUS);
    BOOST_CHECK(pending.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(pending.pending_phase == PendingPhase::PENDING_NETWORK);
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        InputReservation reservation;
        BOOST_REQUIRE(batch.ReadPaymasterReservation(
            artifacts.user_input, reservation));
        BOOST_CHECK(reservation.authorization_may_exist);
    }

    // The exact retry is a read-only idempotent success.
    const MockableData recoverable_state = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.MarkClientFinalValidationFailureRecoverable(
        request_id, attempt.attempt_id, 104, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == recoverable_state);

    // A stale failure racing with a chain-proved state cannot regress either
    // record, even if reconciliation has not yet advanced both in lockstep.
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        ambiguous.state = AttemptState::MEMPOOL;
        pending.state = SessionState::CONFIRMED;
        pending.pending_phase = PendingPhase::NONE;
        BOOST_REQUIRE(batch.WritePaymasterAttempt(ambiguous));
        BOOST_REQUIRE(batch.WritePaymasterSession(pending));
    }
    const MockableData confirmed_state = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.MarkClientFinalValidationFailureRecoverable(
        request_id, attempt.attempt_id, 105, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == confirmed_state);
}

BOOST_AUTO_TEST_CASE(client_post_store_final_conflict_is_atomic_and_keeps_authority_locked)
{
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    constexpr auto request_id = "550e8400-e29b-41d4-a716-44665544aa10";
    BOOST_REQUIRE(store.CreateOrJoinSession(
                      request_id, uint256::ONE, FeeMode::PAYMASTER, 100,
                      session, error) == CreatePaymasterSessionResult::CREATED);

    ValidClientResultArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(BuildValidClientResultArtifacts(
                              m_wallet, session, request_id, uint256S("aa10"),
                              101, artifacts, error),
                          error);
    ProviderAttempt attempt{artifacts.attempt};
    const CTransaction final_transaction{artifacts.final_transaction};
    const std::vector<unsigned char> final_bytes =
        SerializePaymasterTestObject(artifacts.final_transaction);
    BOOST_REQUIRE(store.ReserveInputs(
        request_id, {{artifacts.user_input, ReservationRole::USER_DD}},
        FeeMode::PAYMASTER, 101, error));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    attempt.state = AttemptState::PROVIDER_SIGNED;
    attempt.final_txid = final_transaction.GetHash();
    attempt.final_transaction = final_bytes;
    attempt.updated_at = 102;
    session.attempt_ids = {attempt.attempt_id};
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::PROVIDER_SIGNED_KNOWN;
    session.final_txid = final_transaction.GetHash();
    session.updated_at = 102;

    PaymasterResult result;
    result.genesis_hash = artifacts.genesis_hash;
    result.provider_id = attempt.provider_id;
    result.commit_key = attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = final_transaction.GetHash();
    result.raw_transaction_hash = final_transaction.GetWitnessHash();
    result.final_transaction = artifacts.final_transaction;
    result.updated_at = 102;
    result.identity_signature.assign(64, 0);
    BOOST_REQUIRE(artifacts.provider_identity_key.SignSchnorr(
        GetPaymasterResultSignatureHash(result), result.identity_signature,
        nullptr, uint256{}));
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        InputReservation reservation;
        BOOST_REQUIRE(batch.ReadPaymasterReservation(
            artifacts.user_input, reservation));
        reservation.authorization_may_exist = true;
        BOOST_REQUIRE(batch.WritePaymasterReservation(reservation));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterResult(result, false));
    }

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_conflict = database.m_records;
    for (size_t failing_write = 0; failing_write < 2; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.RecordClientFinalConflict(
            request_id, attempt.attempt_id, final_transaction.GetHash(),
            final_transaction.GetWitnessHash(), 103, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_conflict);
    }
    database.FailCommit();
    BOOST_CHECK(!store.RecordClientFinalConflict(
        request_id, attempt.attempt_id, final_transaction.GetHash(),
        final_transaction.GetWitnessHash(), 103, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_conflict);

    BOOST_REQUIRE_MESSAGE(store.RecordClientFinalConflict(
                              request_id, attempt.attempt_id,
                              final_transaction.GetHash(),
                              final_transaction.GetWitnessHash(), 103, error),
                          error);
    ProviderAttempt conflicted_attempt;
    PaymentSession conflicted_session;
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, conflicted_attempt));
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, conflicted_session));
    BOOST_CHECK(conflicted_attempt.state == AttemptState::CONFLICTED);
    BOOST_CHECK(conflicted_session.state == SessionState::CONFLICTED);
    BOOST_CHECK(conflicted_session.pending_phase == PendingPhase::NONE);
    BOOST_CHECK_EQUAL(conflicted_attempt.final_txid,
                      final_transaction.GetHash());
    BOOST_CHECK(conflicted_attempt.final_transaction == final_bytes);
    BOOST_CHECK_EQUAL(conflicted_session.final_txid,
                      final_transaction.GetHash());
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        InputReservation reservation;
        BOOST_REQUIRE(batch.ReadPaymasterReservation(
            artifacts.user_input, reservation));
        BOOST_CHECK(reservation.authorization_may_exist);
        PaymasterResult persisted_result;
        BOOST_REQUIRE(batch.ReadPaymasterResult(
            attempt.commit_key, persisted_result));
        BOOST_REQUIRE(persisted_result.raw_transaction_hash);
        BOOST_CHECK_EQUAL(*persisted_result.raw_transaction_hash,
                          final_transaction.GetWitnessHash());
    }

    const MockableData after_conflict = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.RecordClientFinalConflict(
        request_id, attempt.attempt_id, final_transaction.GetHash(),
        final_transaction.GetWitnessHash(), 104, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == after_conflict);

    // Chain-proved terminal states win a race with a stale local preflight
    // failure and are never overwritten, even when the attempt itself has not
    // yet been reconciled to the same observation.
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        conflicted_attempt.state = AttemptState::MEMPOOL;
        conflicted_session.state = SessionState::CONFIRMED;
        BOOST_REQUIRE(batch.WritePaymasterAttempt(conflicted_attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(conflicted_session));
    }
    const MockableData confirmed_state = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.RecordClientFinalConflict(
        request_id, attempt.attempt_id, final_transaction.GetHash(),
        final_transaction.GetWitnessHash(), 105, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == confirmed_state);
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, conflicted_session));
    BOOST_CHECK(conflicted_session.state == SessionState::CONFIRMED);

    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        conflicted_session.state = SessionState::CANCELED_SAFE;
        BOOST_REQUIRE(batch.WritePaymasterSession(conflicted_session));
    }
    const MockableData canceled_state = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(store.RecordClientFinalConflict(
        request_id, attempt.attempt_id, final_transaction.GetHash(),
        final_transaction.GetWitnessHash(), 106, error));
    BOOST_CHECK_EQUAL(database.m_write_count, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == canceled_state);
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, conflicted_session));
    BOOST_CHECK(conflicted_session.state == SessionState::CANCELED_SAFE);
}

BOOST_AUTO_TEST_CASE(budgetless_exact_commit_is_never_executable)
{
    PaymasterStore store{m_wallet};
    std::string error;

    ProviderPolicy advertised;
    advertised.funding_models = FUNDING_MODEL_USER_PAID;
    advertised.sponsorship_scope = SponsorshipScope::PUBLIC;
    advertised.fee_rate_bps = 10;
    advertised.min_payment = DDCents{100};
    advertised.max_payment = DDCents{10000};
    advertised.quote_ttl = 30;
    advertised.maximum_network_fee = DGBSatoshis{100};

    ProviderSafetyPolicy safety;
    safety.user_paid.maximum_network_fee_per_transaction = DGBSatoshis{100};
    safety.user_paid.maximum_reserved_network_fee = DGBSatoshis{1000};
    safety.user_paid.maximum_network_fee_per_hour = DGBSatoshis{1000};
    safety.user_paid.maximum_network_fee_per_day = DGBSatoshis{10000};
    safety.user_paid.maximum_completed_per_hour = 10;
    safety.user_paid.maximum_completed_per_day = 100;
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 5;
    safety.maximum_active_quotes_per_recipient = 5;
    safety.maximum_quote_requests_per_netgroup_per_minute = 10;
    safety.updated_at = 100;

    ProviderBudgetLedger ledger;
    ledger.recipient_bucket_secret = uint256S("b101");
    ledger.accounting_time_high_water = 100;

    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{uint256S("b102"), 0});
    transaction.vout.emplace_back(1, CScript{} << OP_TRUE);
    const std::vector<unsigned char> final_transaction =
        SerializePaymasterTestObject(transaction);
    const uint256 final_txid{CTransaction{transaction}.GetHash()};

    ProviderAttempt attempt;
    attempt.attempt_id = uint256S("b103");
    attempt.provider_id = uint256S("b104");
    attempt.client_nonce = uint256S("b105");
    attempt.intent_hash = uint256S("b106");
    attempt.quote_id = uint256S("b107");
    attempt.template_commitment = uint256S("b108");
    attempt.commit_key = GetPaymasterCommitKey(
        attempt.provider_id, attempt.client_nonce, attempt.intent_hash,
        attempt.quote_id, attempt.template_commitment);
    attempt.state = AttemptState::FINAL_COMMITTED;
    attempt.unsigned_txid = final_txid;
    attempt.final_txid = final_txid;
    attempt.final_transaction = final_transaction;
    attempt.retry_until = 200;
    attempt.created_at = 100;
    attempt.updated_at = 150;

    ProviderCommitRecord commit;
    commit.commit_key = attempt.commit_key;
    commit.provider_id = attempt.provider_id;
    commit.quote_id = attempt.quote_id;
    commit.template_commitment = attempt.template_commitment;
    commit.final_txid = final_txid;
    commit.raw_transaction_hash = Hash(final_transaction);
    commit.final_transaction = final_transaction;
    commit.provider_inputs = {transaction.vin.front().prevout};
    commit.committed_at = 150;
    commit.retry_until = attempt.retry_until;

    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterPolicy(advertised));
        BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(safety));
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(ledger));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        BOOST_REQUIRE(batch.WritePaymasterProviderCommit(commit));
    }

    // A budgetless attempt has no reservation row. Even an exact durable
    // commit cannot cross the current authorization boundary.
    BOOST_CHECK(!store.ValidateProviderBudgetAuthorization(
        attempt, BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");
    BOOST_CHECK(!store.ValidateProviderBudgetAuthorization(
        attempt, BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");
    BOOST_CHECK(!store.ValidateProviderBudgetAuthorization(
        attempt, BudgetReservationState::RESERVED,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");

    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ErasePaymasterProviderCommit(commit.commit_key));
    }
    BOOST_CHECK(!store.ValidateProviderBudgetAuthorization(
        attempt, BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");

    ProviderCommitRecord conflicting{commit};
    conflicting.quote_id = uint256S("b109");
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterProviderCommit(conflicting));
    }
    BOOST_CHECK(!store.ValidateProviderBudgetAuthorization(
        attempt, BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterProviderCommit(commit, true));
    }

    ProviderAuthorizationManifest outdated_manifest;
    outdated_manifest.version =
        ProviderAuthorizationManifest::CURRENT_VERSION - 1;
    outdated_manifest.safety_policy_hash = uint256S("b10a");
    outdated_manifest.manifest_id =
        GetProviderAuthorizationManifestId(outdated_manifest);
    attempt.provider_manifest = outdated_manifest;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(attempt));
    }
    BOOST_CHECK(!store.ValidateProviderBudgetAuthorization(
        attempt, BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");

    ProviderAuthorizationManifest current_manifest{outdated_manifest};
    current_manifest.version = ProviderAuthorizationManifest::CURRENT_VERSION;
    current_manifest.manifest_id =
        GetProviderAuthorizationManifestId(current_manifest);
    attempt.provider_manifest = current_manifest;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterAttempt(attempt));
    }
    BOOST_CHECK(!store.ValidateProviderBudgetAuthorization(
        attempt, BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");
}

BOOST_AUTO_TEST_CASE(transient_finalization_errors_remain_retryable)
{
    BOOST_CHECK(IsTransientPaymasterFinalizationError(
        "PAYMASTER_NODE_CONTEXT_UNAVAILABLE"));
    BOOST_CHECK(IsTransientPaymasterFinalizationError(
        "PAYMASTER_NODE_RUNTIME_UNAVAILABLE"));
    BOOST_CHECK(IsTransientPaymasterFinalizationError(
        "PAYMASTER_TXINDEX_NOT_READY"));
    BOOST_CHECK(IsTransientPaymasterFinalizationError(
        "PAYMASTER_FINAL_BLOCK_READ"));
    BOOST_CHECK(IsTransientPaymasterFinalizationError(
        "PAYMASTER_WALLET_TRANSACTION_DATABASE_WRITE"));
    BOOST_CHECK(IsTransientPaymasterFinalizationError(
        "PAYMASTER_RECOVERY_USER_PREVOUT_NOT_FOUND"));

    BOOST_CHECK(!IsTransientPaymasterFinalizationError(
        "PAYMASTER_FINAL_TRANSACTION_CONFLICT"));
    BOOST_CHECK(!IsTransientPaymasterFinalizationError(
        "PAYMASTER_FINAL_WITNESS_CONFLICT"));
    BOOST_CHECK(!IsTransientPaymasterFinalizationError(
        "PAYMASTER_FINAL_MEMPOOL_REJECTED: bad-txns-inputs-missingorspent"));
    BOOST_CHECK(!IsTransientPaymasterFinalizationError(""));
}

BOOST_AUTO_TEST_CASE(shared_final_preflight_rejects_missing_transaction)
{
    ExactFinalTransactionPreflight preflight;
    std::string error;
    BOOST_CHECK(!PreflightExactPaymasterFinalTransaction(
        m_wallet, {}, ExactFinalTxIndexMode::NONBLOCKING, preflight, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINAL_TRANSACTION_MISSING");
    BOOST_CHECK(preflight.presence == ExactFinalTransactionPresence::NONE);

    error.clear();
    BOOST_CHECK(!PreflightExactPaymasterFinalTransaction(
        m_wallet, {}, ExactFinalTxIndexMode::WAIT_FOR_SYNC, preflight, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINAL_TRANSACTION_MISSING");
    BOOST_CHECK(preflight.presence == ExactFinalTransactionPresence::NONE);
}

BOOST_AUTO_TEST_CASE(provider_successor_reconciliation_recycles_carrier_and_dgb_idempotently)
{
    constexpr int64_t now{7000};
    const PaymasterId provider_id{uint256S("7000")};
    const uint256 first_commit_key{uint256S("7101")};
    const uint256 second_commit_key{uint256S("7201")};
    const COutPoint initial_carrier{uint256S("7001"), 0};
    const COutPoint initial_dgb{uint256S("7002"), 0};

    const auto fresh_script = [] {
        CKey key;
        key.MakeNewKey(true);
        TaprootBuilder builder;
        builder.Finalize(XOnlyPubKey{key.GetPubKey()});
        return GetScriptForDestination(builder.GetOutput());
    };
    const CScript initial_carrier_script{fresh_script()};
    const CScript initial_dgb_script{fresh_script()};
    const CScript first_carrier_script{fresh_script()};
    const CScript first_dgb_script{fresh_script()};
    const CScript second_carrier_script{fresh_script()};
    const CScript second_dgb_script{fresh_script()};

    const auto source_entry = [](const COutPoint& outpoint,
                                 PoolAsset asset,
                                 const CScript& script,
                                 int64_t value,
                                 const uint256& reservation_id,
                                 int64_t updated_at) {
        ProviderPoolEntry entry;
        entry.outpoint = outpoint;
        entry.purpose = PoolPurpose::OPERATIONAL;
        entry.asset = asset;
        entry.state = PoolEntryState::COMMITTED;
        entry.script_pub_key = script;
        if (asset == PoolAsset::DGB) {
            entry.dgb_value = DGBSatoshis{value};
        } else {
            entry.carrier_value = DDCents{value};
        }
        entry.confirmation_height = 1;
        entry.reservation_id = reservation_id;
        entry.updated_at = updated_at;
        return entry;
    };

    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_USER_PAID;
    policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    policy.fee_rate_bps = 50;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{100000};
    policy.quote_ttl = 60;
    policy.maximum_network_fee = DGBSatoshis{100};

    struct ReconciliationArtifacts {
        PaymentSession session;
        ProviderAttempt attempt;
        ProviderCommitRecord commit;
        COutPoint carrier_successor;
        COutPoint dgb_successor;
    };
    const auto make_artifacts = [&](const std::string& request_id,
                                    const uint256& session_id,
                                    const uint256& attempt_id,
                                    const uint256& commit_key,
                                    const uint256& quote_id,
                                    const uint256& template_commitment,
                                    const COutPoint& carrier_source,
                                    const COutPoint& dgb_source,
                                    const CScript& carrier_return_script,
                                    const CScript& dgb_return_script,
                                    CAmount carrier_return_value,
                                    CAmount dgb_return_value,
                                    int64_t committed_at) {
        ReconciliationArtifacts artifacts;
        CMutableTransaction transaction;
        transaction.SetDigiDollarType(::DD_TX_TRANSFER);
        transaction.vin.emplace_back(carrier_source);
        transaction.vin.emplace_back(dgb_source);
        transaction.vout.emplace_back(/*nValue=*/0,
                                      carrier_return_script);
        transaction.vout.emplace_back(dgb_return_value,
                                      dgb_return_script);
        transaction.vout.emplace_back(
            /*nValue=*/0,
            CScript{} << OP_RETURN
                      << std::vector<unsigned char>{'D', 'D'}
                      << CScriptNum(DD_TX_TRANSFER)
                      << CScriptNum(carrier_return_value));
        const CTransaction final_transaction{transaction};
        const std::vector<unsigned char> final_bytes =
            SerializePaymasterTestObject(transaction);

        artifacts.attempt.session_id = session_id;
        artifacts.attempt.attempt_id = attempt_id;
        artifacts.attempt.provider_id = provider_id;
        artifacts.attempt.commit_key = commit_key;
        artifacts.attempt.quote_id = quote_id;
        artifacts.attempt.template_commitment = template_commitment;
        artifacts.attempt.final_txid = final_transaction.GetHash();
        artifacts.attempt.final_transaction = final_bytes;
        artifacts.attempt.provider_manifest.provider_id = provider_id;
        artifacts.attempt.provider_manifest.provider_carrier_inputs = {
            carrier_source};
        artifacts.attempt.provider_manifest.provider_dgb_inputs = {
            dgb_source};
        artifacts.attempt.provider_manifest.carrier_return_scripts = {
            carrier_return_script};
        artifacts.attempt.provider_manifest.dgb_change_scripts = {
            dgb_return_script};
        artifacts.attempt.provider_manifest.service_fee = DDCents{3};
        artifacts.attempt.provider_manifest.manifest_id =
            GetProviderAuthorizationManifestId(
                artifacts.attempt.provider_manifest);

        artifacts.commit.commit_key = commit_key;
        artifacts.commit.provider_id = provider_id;
        artifacts.commit.quote_id = quote_id;
        artifacts.commit.template_commitment = template_commitment;
        artifacts.commit.final_txid = final_transaction.GetHash();
        artifacts.commit.raw_transaction_hash = Hash(final_bytes);
        artifacts.commit.final_transaction = final_bytes;
        artifacts.commit.provider_inputs = {carrier_source, dgb_source};
        artifacts.commit.committed_at = committed_at;
        artifacts.commit.retry_until = committed_at;

        artifacts.session.request_id = request_id;
        artifacts.session.session_id = session_id;
        artifacts.session.attempt_ids = {attempt_id};
        artifacts.session.created_at = committed_at - 1;
        artifacts.session.updated_at = committed_at;
        artifacts.session.provider_side = true;

        artifacts.carrier_successor =
            COutPoint{artifacts.commit.final_txid, 0};
        artifacts.dgb_successor =
            COutPoint{artifacts.commit.final_txid, 1};
        return artifacts;
    };

    const ReconciliationArtifacts first{make_artifacts(
        "550e8400-e29b-41d4-a716-446655450001", uint256S("7102"),
        uint256S("7103"), first_commit_key, uint256S("7104"),
        uint256S("7105"), initial_carrier, initial_dgb,
        first_carrier_script, first_dgb_script,
        /*carrier_return_value=*/103,
        /*dgb_return_value=*/500, now)};
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterPolicy(policy));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({
            source_entry(initial_carrier, PoolAsset::DD_CARRIER,
                         initial_carrier_script, 100, first_commit_key,
                         now - 1),
            source_entry(initial_dgb, PoolAsset::DGB,
                         initial_dgb_script, 1000, first_commit_key,
                         now - 1),
        }));
        BOOST_REQUIRE(batch.WritePaymasterSession(first.session));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(first.attempt));
        BOOST_REQUIRE(batch.WritePaymasterProviderCommit(first.commit));
    }

    PaymasterStore store{m_wallet};
    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_reconciliation{database.m_records};
    size_t recovered{0};
    std::string error;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.ReconcileProviderPoolSuccessors(recovered, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_reconciliation);

    database.FailCommit();
    BOOST_CHECK(!store.ReconcileProviderPoolSuccessors(recovered, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_reconciliation);

    BOOST_REQUIRE_MESSAGE(
        store.ReconcileProviderPoolSuccessors(recovered, error), error);
    BOOST_CHECK_EQUAL(recovered, 1U);
    std::vector<ProviderPoolEntry> pool;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .ReadPaymasterProviderPool(pool));
    }
    const auto find_pool_entry = [](const std::vector<ProviderPoolEntry>& entries,
                                    const COutPoint& outpoint) {
        return std::find_if(
            entries.begin(), entries.end(),
            [&](const ProviderPoolEntry& entry) {
                return entry.outpoint == outpoint;
            });
    };
    auto first_carrier = find_pool_entry(pool, first.carrier_successor);
    BOOST_REQUIRE(first_carrier != pool.end());
    BOOST_CHECK(first_carrier->state == PoolEntryState::PENDING_SUCCESSOR);
    BOOST_CHECK_EQUAL(first_carrier->carrier_value.value, 103);
    BOOST_CHECK_EQUAL(first_carrier->origin_commit_key, first_commit_key);
    auto first_dgb = find_pool_entry(pool, first.dgb_successor);
    BOOST_REQUIRE(first_dgb != pool.end());
    BOOST_CHECK(first_dgb->state == PoolEntryState::PENDING_SUCCESSOR);
    BOOST_CHECK_EQUAL(first_dgb->dgb_value.value, 500);
    BOOST_CHECK_EQUAL(first_dgb->origin_commit_key, first_commit_key);

    const ReconciliationArtifacts second{make_artifacts(
        "550e8400-e29b-41d4-a716-446655450002", uint256S("7202"),
        uint256S("7203"), second_commit_key, uint256S("7204"),
        uint256S("7205"), first.carrier_successor, first.dgb_successor,
        second_carrier_script, second_dgb_script,
        /*carrier_return_value=*/106,
        /*dgb_return_value=*/300, now + 10)};
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterSession(second.session));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(second.attempt));
        BOOST_REQUIRE(batch.WritePaymasterProviderCommit(second.commit));
    }
    BOOST_REQUIRE_MESSAGE(
        store.ReconcileProviderPoolSuccessors(recovered, error), error);
    BOOST_CHECK_EQUAL(recovered, 1U);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .ReadPaymasterProviderPool(pool));
    }
    auto second_carrier = find_pool_entry(pool, second.carrier_successor);
    BOOST_REQUIRE(second_carrier != pool.end());
    BOOST_CHECK(second_carrier->state == PoolEntryState::PENDING_SUCCESSOR);
    BOOST_CHECK_EQUAL(second_carrier->carrier_value.value, 106);
    BOOST_CHECK_EQUAL(second_carrier->origin_commit_key, second_commit_key);
    auto second_dgb = find_pool_entry(pool, second.dgb_successor);
    BOOST_REQUIRE(second_dgb != pool.end());
    BOOST_CHECK(second_dgb->state == PoolEntryState::PENDING_SUCCESSOR);
    BOOST_CHECK_EQUAL(second_dgb->dgb_value.value, 300);
    BOOST_CHECK_EQUAL(second_dgb->origin_commit_key, second_commit_key);

    const size_t exact_pool_size{pool.size()};
    BOOST_REQUIRE_MESSAGE(
        store.ReconcileProviderPoolSuccessors(recovered, error), error);
    BOOST_CHECK_EQUAL(recovered, 0U);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .ReadPaymasterProviderPool(pool));
    }
    BOOST_CHECK_EQUAL(pool.size(), exact_pool_size);

    auto restarted_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet restarted_wallet{m_node.chain.get(),
                                     "successor-reconciliation-restart",
                                     std::move(restarted_database)};
    BOOST_REQUIRE(restarted_wallet.LoadWallet() == DBErrors::LOAD_OK);
    PaymasterStore restarted_store{restarted_wallet};
    BOOST_REQUIRE_MESSAGE(
        restarted_store.ReconcileProviderPoolSuccessors(recovered, error),
        error);
    BOOST_CHECK_EQUAL(recovered, 0U);
    std::vector<ProviderPoolEntry> restarted_pool;
    {
        LOCK(restarted_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{restarted_wallet.GetDatabase()}
                          .ReadPaymasterProviderPool(restarted_pool));
    }
    BOOST_CHECK_EQUAL(restarted_pool.size(), exact_pool_size);

    std::vector<ProviderPoolEntry> conflicting_pool;
    {
        LOCK(restarted_wallet.cs_wallet);
        WalletBatch batch{restarted_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(conflicting_pool));
        auto conflicting = std::find_if(
            conflicting_pool.begin(), conflicting_pool.end(),
            [&](const ProviderPoolEntry& entry) {
                return entry.outpoint == second.carrier_successor;
            });
        BOOST_REQUIRE(conflicting != conflicting_pool.end());
        ++conflicting->carrier_value.value;
        BOOST_REQUIRE(batch.WritePaymasterProviderPool(
            conflicting_pool, /*overwrite=*/true));
    }
    BOOST_CHECK(!restarted_store.ReconcileProviderPoolSuccessors(
        recovered, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_SUCCESSOR_CONFLICT");
}

BOOST_AUTO_TEST_SUITE_END()
