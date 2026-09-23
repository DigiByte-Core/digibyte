// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Internal helpers shared by the domain-specific Paymaster RPC units. */

#ifndef DIGIBYTE_WALLET_RPC_PAYMASTER_INTERNAL_H
#define DIGIBYTE_WALLET_RPC_PAYMASTER_INTERNAL_H

#include <logging.h>
#include <netaddress.h>
#include <paymaster/manager.h>
#include <paymaster/validation.h>
#include <rpc/util.h>
#include <streams.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <version.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/walletdb.h>

#include <exception>
#include <optional>
#include <string_view>
#include <vector>

namespace wallet::paymaster_rpc::internal {

using FinalTransactionPresence = ExactFinalTransactionPresence;

inline constexpr const char* INTERNAL_PAYMASTER_SERVICE_METHOD{
    "__paymaster_automatic_service"};
inline constexpr const char* INTERNAL_PAYMASTER_AUTOSTART_METHOD{
    "__paymaster_automatic_start"};
inline constexpr const char* INTERNAL_PAYMASTER_MAINTENANCE_METHOD{
    "__paymaster_automatic_maintenance"};

// Written with the signed wallet transaction, before broadcast. This marker
// allows finance reconciliation to recover a missing pool-retirement event.
inline constexpr const char* PAYMASTER_RETIREMENT_PROVIDER_KEY{"paymaster_retirement_provider"};

class ProviderWorkGuard final
{
public:
    ProviderWorkGuard(DigiDollar::Paymaster::Manager& manager,
                      const std::string& wallet_name,
                      const DigiDollar::Paymaster::PaymasterId& provider_id,
                      bool require_running = true)
        : m_manager{manager}, m_wallet_name{wallet_name},
          m_provider_id{provider_id},
          m_acquired{manager.TryBeginProviderWork(
              wallet_name, provider_id, require_running)}
    {
    }

    ProviderWorkGuard(const ProviderWorkGuard&) = delete;
    ProviderWorkGuard& operator=(const ProviderWorkGuard&) = delete;
    ~ProviderWorkGuard()
    {
        if (m_acquired) m_manager.EndProviderWork(m_wallet_name);
    }
    bool Acquired() const noexcept { return m_acquired; }
    bool CompleteProviderStart()
    {
        return m_acquired &&
            m_manager.CompleteProviderStart(m_wallet_name, m_provider_id);
    }

private:
    DigiDollar::Paymaster::Manager& m_manager;
    const std::string m_wallet_name;
    const DigiDollar::Paymaster::PaymasterId m_provider_id;
    const bool m_acquired;
};

struct ProviderReadiness {
    bool have_settings{false};
    bool have_identity{false};
    bool have_policy{false};
    bool have_safety_policy{false};
    bool have_budget_ledger{false};
    bool have_liquidity_policy{false};
    bool have_maintenance_ledger{false};
    bool have_pool{false};
    bool wallet_eligible{false};
    bool pool_ready{false};
    bool ready{false};
    uint8_t available_funding_models{0};
    DigiDollar::Paymaster::ProviderSettings settings;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    DigiDollar::Paymaster::ProviderPolicy policy;
    DigiDollar::Paymaster::ProviderSafetyPolicy safety_policy;
    DigiDollar::Paymaster::ProviderBudgetLedger budget_ledger;
    DigiDollar::Paymaster::ProviderLiquidityPolicy liquidity_policy;
    DigiDollar::Paymaster::ProviderMaintenanceLedger maintenance_ledger;
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry> pool_entries;
    DigiDollar::Paymaster::ProviderPoolReadiness pool;
    CService endpoint;
    std::vector<std::string> errors;
};

struct LiquiditySlotCounts {
    size_t target_counted{0};
    size_t ready{0};
    size_t pending{0};
    size_t missing{0};
};

enum class EquivocationBlockPolicy : uint8_t {
    OBSERVE_ONLY,
    ENFORCE,
};

class ProviderInboundMessageGuard final
{
public:
    ProviderInboundMessageGuard(
        DigiDollar::Paymaster::Manager& manager,
        const DigiDollar::Paymaster::DirectMessage& message) noexcept
        : m_manager{manager},
          m_message_id{message.message_id},
          m_uncaught_exceptions{std::uncaught_exceptions()}
    {
    }

    ProviderInboundMessageGuard(const ProviderInboundMessageGuard&) = delete;
    ProviderInboundMessageGuard& operator=(
        const ProviderInboundMessageGuard&) = delete;

    ~ProviderInboundMessageGuard()
    {
        if (std::uncaught_exceptions() != m_uncaught_exceptions) return;
        if (!m_manager.AcknowledgeDirectMessagesIfPresent({m_message_id})) {
            LogPrintf("Paymaster provider inbox acknowledgement failed\n");
        }
    }

private:
    DigiDollar::Paymaster::Manager& m_manager;
    const uint256 m_message_id;
    const int m_uncaught_exceptions;
};

struct RecoveryQueueState {
    bool queued{false};
    bool connection_pending{false};
    bool route_available{false};
};

struct PaymasterSubmitQueueState {
    bool queued{false};
    bool connection_pending{false};
    bool route_available{false};
};

struct ProviderCommitRecoveryResult {
    bool broadcast{false};
    bool already_confirmed{false};
    DigiDollar::Paymaster::PaymasterResult provider_result;
    std::string error;
};

// Provider lifecycle and readiness helpers. These functions reconcile durable
// wallet state before exposing an operator action or public announcement.
std::string ProviderPoolReadError(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries);
bool ReadOptionalProviderPool(
    WalletBatch& batch,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    std::string& error);
const char* ProviderOperationModeName(
    DigiDollar::Paymaster::ProviderOperationMode mode);
const char* ProviderServiceStateName(
    DigiDollar::Paymaster::ProviderServiceState state);
std::string StableProviderServiceError(std::string error);
std::string WalletEndpointURI(const std::string& wallet_name);
bool ReconcileProviderMaintenance(CWallet& wallet,
                                  size_t& recovered,
                                  std::string& error);
bool ReconcileProviderFinances(CWallet& wallet,
                               size_t& changed_events,
                               std::string& error);
bool AutomaticProviderStateIsSynchronized(CWallet& wallet);
bool ProviderTxIndexIsReady(CWallet& wallet, bool wait_for_sync);
bool CapacityContinuationMayProceed(const ProviderReadiness& readiness);
bool CapacityRequestMayProceed(const ProviderReadiness& readiness,
                               bool have_stored_response);
bool CheckPaymasterClientReadiness(CWallet& wallet,
                                   WalletContext& context,
                                   std::string& error);
DigiDollar::Paymaster::DDCents EffectiveClientServiceFeeCap(
    CWallet& wallet,
    DigiDollar::Paymaster::DDCents requested,
    std::string& error);
bool CheckPaymasterPrivacyReadiness(
    DigiDollar::Paymaster::PrivacyProfile privacy,
    WalletContext& context,
    std::string& error);
bool RefreshPoolConfirmationHeights(
    CWallet& wallet,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    std::string& error);
ProviderReadiness GetProviderReadiness(CWallet& wallet,
                                       WalletContext& context,
                                       bool wait_for_sync = true);
UniValue ReadinessErrorsToJSON(const std::vector<std::string>& errors);
bool HasPendingPoolPreparation(CWallet& wallet);
RPCResult PoolPreparationResult();
UniValue PoolPreparationToJSON(CWallet& wallet);
UniValue PoolEntryToJSON(
    const DigiDollar::Paymaster::ProviderPoolEntry& entry);
bool PublishCurrentProviderAnnouncement(
    CWallet& wallet,
    WalletContext& context,
    const ProviderReadiness& readiness,
    int64_t now,
    DigiDollar::Paymaster::Announcement* published,
    std::string& error);

// Liquidity planning and accounting. Maintenance builders must operate on a
// reconciled pool and persist restartable plans before broadcasting.
LiquiditySlotCounts CountLiquiditySlots(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    DigiDollar::Paymaster::PoolPurpose purpose,
    DigiDollar::Paymaster::PoolAsset asset,
    size_t target,
    int64_t minimum_dgb_value = 0);
UniValue LiquidityPolicyToJSON(
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy);
DigiDollar::Paymaster::ProviderLiquidityPolicy SuggestedLiquidityPolicy(
    const ProviderReadiness& readiness,
    int64_t now);
UniValue ProviderLiquidityStatusToJSON(const ProviderReadiness& readiness,
                                       int64_t now);
uint256 CarrierWithdrawalPlanId(
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan);
std::optional<uint32_t> FindMaintenanceOutputIndex(
    const CTransaction& transaction,
    const DigiDollar::Paymaster::ProviderMaintenanceOutput& output);
bool MakeProviderPoolRoomForMaintenance(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    size_t additional,
    std::string& error);
bool SaveCarrierWithdrawalPreview(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan,
    std::string& error);
std::optional<DigiDollar::Paymaster::DGBSatoshis>
GetProviderFinanceTransactionFee(CWallet& wallet,
                                 const CTransactionRef& transaction);
bool RecordProviderFinanceTransaction(
    CWallet& wallet,
    DigiDollar::Paymaster::ProviderFinanceEventKind kind,
    const CTransactionRef& transaction,
    DigiDollar::Paymaster::DGBSatoshis dgb_cost,
    int64_t now,
    std::string& error);
bool RunAutomaticDGBReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error);
bool RunAutomaticCarrierReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error);

// RPC presentation and canonical wire encodings. Redacted serializers are the
// only variants suitable for durable client-side diagnostic state.
UniValue ReliabilityToJSON(
    const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
    int64_t now);
std::string FeeModeName(DigiDollar::Paymaster::FeeMode mode);
std::string ResultStatusName(
    DigiDollar::Paymaster::PaymasterResultStatus status);
std::optional<std::string> DigiDollarAddressForScript(const CScript& script);
UniValue SessionToJSON(const DigiDollar::Paymaster::PaymentSession& session,
                       const PaymasterStore* store = nullptr);
bool FindSession(const UniValue& lookup,
                 PaymasterStore& store,
                 DigiDollar::Paymaster::PaymentSession& session);
UniValue AttemptToJSON(const DigiDollar::Paymaster::ProviderAttempt& attempt);
std::vector<COutPoint> ParsePaymasterInputs(const UniValue& value);
std::vector<unsigned char> SerializeQuoteRequest(
    const DigiDollar::Paymaster::PaymasterQuoteRequest& quote_request);
std::vector<unsigned char> SerializeCapacityRequest(
    const DigiDollar::Paymaster::PaymasterCapacityRequest& request);
std::vector<unsigned char> SerializeCapacityProof(
    const DigiDollar::Paymaster::PaymasterCapacityProof& proof);
std::vector<DigiDollar::Paymaster::DirectMessage> SelectCapacityProofMessages(
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const std::string& request_id,
    const uint256& session_id,
    const uint256& client_nonce);

template <typename T>
bool DeserializePaymasterHex(const UniValue& value,
                             const char* field,
                             T& decoded,
                             std::string& error)
{
    try {
        const std::vector<unsigned char> bytes = ParseHexV(value, field);
        CDataStream stream{bytes, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> decoded;
        if (!stream.empty()) {
            throw std::ios_base::failure("trailing paymaster data");
        }
    } catch (const std::ios_base::failure&) {
        error = strprintf("PAYMASTER_INVALID_%s_ENCODING", field);
        return false;
    }
    error.clear();
    return true;
}

std::vector<unsigned char> SerializeRedactedQuoteRequest(
    DigiDollar::Paymaster::PaymasterQuoteRequest quote_request);
std::vector<unsigned char> SerializeQuoteResponse(
    const DigiDollar::Paymaster::PaymasterQuoteResponse& quote_response);
bool AcknowledgeEquivocationMessages(
    DigiDollar::Paymaster::Manager& manager,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    std::string& error);
bool AcknowledgeRejectedDirectMessage(
    DigiDollar::Paymaster::Manager& manager,
    const DigiDollar::Paymaster::DirectMessage& message,
    std::string& error);

// Drain conflicting signed messages into durable evidence before acknowledging
// their inbox leases, so a crash cannot erase an observed equivocation.
bool IsPermanentCapacityContinuationError(const std::string& error);
bool PersistPendingCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error);
bool PersistPendingQuoteEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& quotes,
    bool& found,
    std::string& error);
bool PersistPendingAlternativeRecoveryCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& recovery_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error);
bool DrainClientAttemptEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error,
    EquivocationBlockPolicy block_policy = EquivocationBlockPolicy::ENFORCE,
    bool lease_messages = true);
bool DrainClientSessionEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    std::string& error,
    EquivocationBlockPolicy active_block_policy =
        EquivocationBlockPolicy::OBSERVE_ONLY);
bool DrainAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    std::string& error,
    EquivocationBlockPolicy block_policy = EquivocationBlockPolicy::ENFORCE,
    bool lease_messages = true);
bool DrainPendingAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const std::vector<unsigned char>& first_capacity_proof,
    std::string& error);

// Alternative-recovery messages are persisted and retried byte-for-byte;
// decoding therefore rejects both trailing bytes and non-canonical encodings.
template <typename T>
std::vector<unsigned char> SerializeRecoveryMessage(const T& message)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << message;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename T>
bool DeserializeCanonicalRecoveryMessage(
    const std::vector<unsigned char>& encoded,
    T& message,
    std::string& error)
{
    try {
        SpanReader stream{::PROTOCOL_VERSION, encoded};
        stream >> message;
        if (!stream.empty() || SerializeRecoveryMessage(message) != encoded) {
            throw std::ios_base::failure("non-canonical recovery message");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PERSISTED_RECOVERY_CORRUPT";
        return false;
    }
    error.clear();
    return true;
}

template <typename T>
bool QueueRecoveryMessage(
    WalletContext& context,
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const T& message,
    int64_t now,
    RecoveryQueueState& state,
    std::string& error);

std::string AlternativeRecoveryPhaseName(
    DigiDollar::Paymaster::AlternativeRecoveryPhase phase);
UniValue AlternativeRecoveryToJSON(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery);
bool BuildRecoveryParametersFromRecord(
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    DigiDollar::Paymaster::AlternativeRecoveryParameters& parameters,
    std::string& error);
bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error);
bool HasDurableClientAuthorization(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t observation_time);
bool QueuePersistedPaymasterSubmit(
    WalletContext& context,
    CWallet& wallet,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    PaymasterSubmitQueueState& state,
    std::string& error);

// Durable authorization reconstruction and the final pre-broadcast firewall.
// These helpers must reload persisted artifacts instead of trusting earlier
// RPC-local objects across a database commit or wallet-signing boundary.
std::vector<unsigned char> SerializePaymentIntent(
    const DigiDollar::Paymaster::PaymentIntent& intent);
bool LoadTrustedTemplate(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error);
bool ValidateDurableProviderUserAuthorization(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error);
bool FindDurableClientAuthorizationAttempt(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    int64_t observation_time,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    std::string& error);
UniValue DurableClientAuthorizationToJSON(
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    const DigiDollar::Paymaster::PaymentIntent& intent,
    const DigiDollar::Paymaster::PaymasterQuote& quote);
bool GetValidatedClientAuthorizationCommitment(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    uint256& commitment,
    std::string& error);
std::vector<unsigned char> SerializePSBT(
    const PartiallySignedTransaction& psbt);
std::vector<unsigned char> SerializeTransaction(
    const CMutableTransaction& transaction);
bool PreflightFinalPaymasterTransaction(
    CWallet& wallet,
    const CTransactionRef& transaction,
    FinalTransactionPresence& presence,
    std::string& error,
    ExactFinalTxIndexMode txindex_mode = ExactFinalTxIndexMode::WAIT_FOR_SYNC);
bool MarkFinalValidationFailureRecoverable(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    std::string& error);
std::vector<COutPoint> ProviderInputs(
    const CMutableTransaction& transaction,
    const std::vector<DigiDollar::Paymaster::ReservationRole>& roles);
bool TemplateInputsAvailable(CWallet& wallet,
                             const CMutableTransaction& transaction);
bool RejectUnavailableTemplateInputs(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    const CMutableTransaction& transaction,
    int64_t now,
    bool& rejected,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error);
bool BuildFinalProviderResult(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error);
bool InsertAndBroadcastPaymasterTransaction(CWallet& wallet,
                                            const CTransactionRef& transaction,
                                            bool& already_confirmed,
                                            std::string& error);
bool InsertAndBroadcastProviderCommit(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    bool& already_confirmed,
    std::string& error,
    CTransactionRef* exact_transaction = nullptr);
ProviderCommitRecoveryResult RecoverProviderCommit(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now);

// Policy conversion is kept at the RPC boundary; PaymasterStore consumes only
// validated typed policies and never UniValue supplied by a peer or caller.
UniValue ProviderPolicyToJSON(
    const DigiDollar::Paymaster::ProviderPolicy& policy);
DigiDollar::Paymaster::ProviderPolicy ParseProviderPolicy(
    const UniValue& value);
UniValue ProviderSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy);
DigiDollar::Paymaster::ProviderSafetyPolicy ParseProviderSafetyPolicy(
    const UniValue& value);
UniValue ProviderSafetyClassStatusToJSON(
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy,
    DigiDollar::Paymaster::FundingModel model,
    DigiDollar::Paymaster::SponsorshipScope scope,
    int64_t now);
UniValue ClientSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy);
DigiDollar::Paymaster::ClientSafetyPolicy ParseClientSafetyPolicy(
    const UniValue& value);
std::vector<RPCArg> FundingSafetyLimitArgs();
std::vector<RPCResult> ProviderSafetyClassStatusResults();
std::vector<RPCResult> ProviderSafetyPolicyResults();
std::vector<RPCResult> ProviderLiquidityPolicyResults();
std::vector<RPCResult> ProviderLiquidityStatusResults();

} // namespace wallet::paymaster_rpc::internal

#endif // DIGIBYTE_WALLET_RPC_PAYMASTER_INTERNAL_H
