// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider policies, finite budgets, liquidity, and runtime state. */

#ifndef DIGIBYTE_PAYMASTER_PROVIDER_H
#define DIGIBYTE_PAYMASTER_PROVIDER_H

#include <paymaster/types.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

struct ProviderAttempt;
struct ProviderAuthorizationManifest;
struct PaymasterCapacityRequest;
struct PaymasterQuoteRequest;

static constexpr uint8_t FUNDING_MODEL_USER_PAID{1U << static_cast<uint8_t>(FundingModel::USER_PAID)};
static constexpr uint8_t FUNDING_MODEL_SPONSORED{1U << static_cast<uint8_t>(FundingModel::SPONSORED)};
static constexpr uint8_t FUNDING_MODEL_ALL{FUNDING_MODEL_USER_PAID | FUNDING_MODEL_SPONSORED};
static constexpr int64_t MAX_QUOTE_TTL_SECONDS{60};

enum class FeeOutputKind : uint8_t {
    NONE,
    CARRIER_SUCCESSOR,
    PROVIDER_OUTPUT,
};

enum class PoolPurpose : uint8_t {
    ADMISSION,
    OPERATIONAL,
};

enum class PoolAsset : uint8_t {
    DGB,
    DD_CARRIER,
};

enum class PoolEntryState : uint8_t {
    AVAILABLE,
    RESERVED,
    PENDING_SUCCESSOR,
    SPENT,
    COMMITTED,
    RELEASED,
    INVALIDATED,
};

enum class BudgetReservationState : uint8_t {
    RESERVED,
    SPENT,
    RELEASED,
};

enum class CapacityAdmissionState : uint8_t {
    RESERVED,
    PROMOTED,
    RELEASED,
};

/** Local, non-advertised loss limits for one provider funding mode. A limit
 * set is either completely disabled (all fields zero) or completely finite.
 * There is deliberately no representation for an unlimited budget. */
struct FundingSafetyLimits {
    DGBSatoshis maximum_network_fee_per_transaction;
    DGBSatoshis maximum_reserved_network_fee;
    DGBSatoshis maximum_network_fee_per_hour;
    DGBSatoshis maximum_network_fee_per_day;
    uint32_t maximum_completed_per_hour{0};
    uint32_t maximum_completed_per_day{0};

    SERIALIZE_METHODS(FundingSafetyLimits, obj)
    {
        READWRITE(obj.maximum_network_fee_per_transaction,
                  obj.maximum_reserved_network_fee,
                  obj.maximum_network_fee_per_hour,
                  obj.maximum_network_fee_per_day,
                  obj.maximum_completed_per_hour,
                  obj.maximum_completed_per_day);
    }
};

/** Wallet-local provider policy. Unlike ProviderPolicy this object is never
 * advertised to peers and can therefore only reduce, never expand, remotely
 * visible terms. */
struct ProviderSafetyPolicy {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    FundingSafetyLimits user_paid;
    FundingSafetyLimits public_sponsored;
    FundingSafetyLimits restricted_sponsored;
    uint32_t maximum_active_quotes_total{0};
    uint32_t maximum_active_quotes_per_netgroup{0};
    uint32_t maximum_active_quotes_per_recipient{0};
    uint32_t maximum_quote_requests_per_netgroup_per_minute{0};
    int64_t updated_at{0};

    SERIALIZE_METHODS(ProviderSafetyPolicy, obj)
    {
        READWRITE(obj.version, obj.user_paid, obj.public_sponsored,
                  obj.restricted_sponsored, obj.maximum_active_quotes_total,
                  obj.maximum_active_quotes_per_netgroup,
                  obj.maximum_active_quotes_per_recipient,
                  obj.maximum_quote_requests_per_netgroup_per_minute,
                  obj.updated_at);
    }
};

/** Persistent reservation of the provider's worst-case DGB miner fee. */
struct ProviderBudgetReservation {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    uint256 commit_key;
    FundingModel funding_model{FundingModel::USER_PAID};
    SponsorshipScope sponsorship_scope{SponsorshipScope::PUBLIC};
    DGBSatoshis network_fee;
    uint256 recipient_bucket;
    uint256 netgroup_bucket;
    BudgetReservationState state{BudgetReservationState::RESERVED};
    int64_t reserved_at{0};
    int64_t updated_at{0};

    SERIALIZE_METHODS(ProviderBudgetReservation, obj)
    {
        READWRITE(obj.version, obj.commit_key,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipScope::RESTRICTED)>>(obj.sponsorship_scope),
                  obj.network_fee, obj.recipient_bucket, obj.netgroup_bucket,
            Using<EnumByteFormatter<static_cast<uint8_t>(BudgetReservationState::RELEASED)>>(obj.state),
            obj.reserved_at, obj.updated_at);
    }
};

/** Persisted, pseudonymous admission event for the provider's wallet-local
 * quote-request rate limit. request_key identifies a semantic protocol slot;
 * request_hash binds its canonical contents, so an exact replay cannot
 * consume a second token while changed contents fail closed. */
struct ProviderQuoteRequestEvent {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    /** Stable protocol slot (provider, request and session), independent of
     * the peer connection and of the request contents. */
    uint256 request_key;
    /** Canonical request contents. This is separate from request_key so a
     * semantic slot reused with different contents fails closed. */
    uint256 request_hash;
    /** Wallet-HMAC of canonical NetGroupManager bytes. */
    uint256 netgroup_bucket;
    int64_t admitted_at{0};

    SERIALIZE_METHODS(ProviderQuoteRequestEvent, obj)
    {
        READWRITE(obj.version, obj.request_key, obj.request_hash,
                  obj.netgroup_bucket, obj.admitted_at);
    }
};

/** Durable admission record created before an operational pool slot is
 * reserved for PMCAPREQ. It is keyed by the semantic provider/request/session
 * tuple rather than by a connection, binds the canonical request bytes, and
 * survives reconnects and restarts. PROMOTED means the capacity reservation
 * was atomically rebound to a committed quote budget; RELEASED is a durable
 * replay barrier after a safe pre-signature expiry. */
struct ProviderCapacityAdmission {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 request_key;
    uint256 request_hash;
    /** Wallet-HMAC of canonical NetGroupManager bytes; never a raw address,
     * canonical group, or process-local keyed group. */
    uint256 netgroup_bucket;
    FundingModel funding_model{static_cast<FundingModel>(0xff)};
    bool requires_carrier{false};
    uint256 quote_request_hash;
    uint256 commit_key;
    CapacityAdmissionState state{CapacityAdmissionState::RESERVED};
    int64_t admitted_at{0};
    int64_t expires_at{0};
    int64_t updated_at{0};

    SERIALIZE_METHODS(ProviderCapacityAdmission, obj)
    {
        READWRITE(obj.version, obj.request_key, obj.request_hash,
                  obj.netgroup_bucket,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                  obj.requires_carrier, obj.quote_request_hash, obj.commit_key,
                  Using<EnumByteFormatter<static_cast<uint8_t>(CapacityAdmissionState::RELEASED)>>(obj.state),
                  obj.admitted_at, obj.expires_at, obj.updated_at);
    }
};

struct ProviderBudgetLedger {
    static constexpr uint16_t CURRENT_VERSION{4};

    uint16_t version{CURRENT_VERSION};
    uint256 recipient_bucket_secret;
    int64_t accounting_time_high_water{0};
    std::vector<ProviderBudgetReservation> reservations;
    std::vector<ProviderQuoteRequestEvent> quote_requests;
    std::vector<ProviderCapacityAdmission> capacity_admissions;

    SERIALIZE_METHODS(ProviderBudgetLedger, obj)
    {
        READWRITE(obj.version, obj.recipient_bucket_secret,
                  obj.accounting_time_high_water, obj.reservations,
                  obj.quote_requests, obj.capacity_admissions);
    }
};

struct ProviderSafetyStatus {
    DGBSatoshis reserved_network_fee;
    DGBSatoshis spent_network_fee_last_hour;
    DGBSatoshis spent_network_fee_last_day;
    uint32_t active_quotes{0};
    uint32_t active_quotes_for_netgroup{0};
    uint32_t completed_last_hour{0};
    uint32_t completed_last_day{0};
    bool can_accept_quote{false};
    std::vector<std::string> errors;
};

/** Wallet-local client fee ceiling. Presence of this record is the explicit
 * opt-in; zero values intentionally permit only zero-service-fee offers. */
struct ClientSafetyPolicy {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    DDCents maximum_service_fee_per_transaction;
    DDCents maximum_service_fee_per_day;
    int64_t updated_at{0};

    SERIALIZE_METHODS(ClientSafetyPolicy, obj)
    {
        READWRITE(obj.version, obj.maximum_service_fee_per_transaction,
                  obj.maximum_service_fee_per_day, obj.updated_at);
    }
};

struct ClientFeeReservation {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 commit_key;
    DDCents service_fee;
    BudgetReservationState state{BudgetReservationState::RESERVED};
    int64_t reserved_at{0};
    int64_t updated_at{0};

    SERIALIZE_METHODS(ClientFeeReservation, obj)
    {
        READWRITE(obj.version, obj.commit_key, obj.service_fee,
                  Using<EnumByteFormatter<static_cast<uint8_t>(BudgetReservationState::RELEASED)>>(obj.state),
                  obj.reserved_at, obj.updated_at);
    }
};

struct ClientFeeLedger {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    int64_t accounting_time_high_water{0};
    std::vector<ClientFeeReservation> reservations;

    SERIALIZE_METHODS(ClientFeeLedger, obj)
    {
        READWRITE(obj.version, obj.accounting_time_high_water, obj.reservations);
    }
};

struct ProviderPolicy {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint8_t funding_models{0};
    SponsorshipScope sponsorship_scope{SponsorshipScope::PUBLIC};
    uint32_t fee_rate_bps{0};
    DDCents min_payment;
    DDCents max_payment;
    int64_t quote_ttl{DEFAULT_QUOTE_TTL_SECONDS};
    DGBSatoshis maximum_network_fee;

    SERIALIZE_METHODS(ProviderPolicy, obj)
    {
        READWRITE(obj.version, obj.funding_models,
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipScope::RESTRICTED)>>(obj.sponsorship_scope),
                  obj.fee_rate_bps, obj.min_payment, obj.max_payment,
                  obj.quote_ttl, obj.maximum_network_fee);
    }
};

struct ProviderIdentityRecord {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    PaymasterId provider_id;
    XOnlyPubKey identity_key;
    CScript identity_script;
    std::string display_name;
    int64_t created_at{0};

    SERIALIZE_METHODS(ProviderIdentityRecord, obj)
    {
        READWRITE(obj.version, obj.provider_id, obj.identity_key,
                  obj.identity_script, obj.display_name, obj.created_at);
    }
};

/** Economic event recorded by the provider wallet. The ledger deliberately
 * keeps native DD and DGB amounts separate: pool capital is not income or an
 * expense, and an optional fiat estimate must always be derived at display
 * time from the current oracle price. */
enum class ProviderFinanceEventKind : uint8_t {
    TRANSFER,
    POOL_SETUP,
    LIQUIDITY_REPLENISHMENT,
    POOL_RETIREMENT,
    CARRIER_WITHDRAWAL,
};

enum class ProviderFinanceEventState : uint8_t {
    PENDING,
    CONFIRMED,
    INVALIDATED,
};

struct ProviderFinanceEvent {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 event_id;
    uint256 genesis_hash;
    PaymasterId provider_id;
    ProviderFinanceEventKind kind{ProviderFinanceEventKind::TRANSFER};
    ProviderFinanceEventState state{ProviderFinanceEventState::PENDING};
    FundingModel funding_model{FundingModel::USER_PAID};
    SponsorshipScope sponsorship_scope{SponsorshipScope::PUBLIC};
    uint256 transaction_id;
    DDCents dd_income;
    DGBSatoshis dgb_cost;
    int64_t created_at{0};
    int64_t confirmed_at{0};
    int64_t updated_at{0};

    SERIALIZE_METHODS(ProviderFinanceEvent, obj)
    {
        READWRITE(
            obj.version, obj.event_id, obj.genesis_hash, obj.provider_id,
            Using<EnumByteFormatter<static_cast<uint8_t>(
                ProviderFinanceEventKind::CARRIER_WITHDRAWAL)>>(obj.kind),
            Using<EnumByteFormatter<static_cast<uint8_t>(
                ProviderFinanceEventState::INVALIDATED)>>(obj.state),
            Using<EnumByteFormatter<static_cast<uint8_t>(
                FundingModel::SPONSORED)>>(obj.funding_model),
            Using<EnumByteFormatter<static_cast<uint8_t>(
                SponsorshipScope::RESTRICTED)>>(obj.sponsorship_scope),
            obj.transaction_id, obj.dd_income, obj.dgb_cost,
            obj.created_at, obj.confirmed_at, obj.updated_at);
    }
};

/** UTC-day materialization of confirmed events. These totals are derived
 * from ProviderFinanceEvent and are rebuilt whenever an event changes, so a
 * replay or reorg cannot increment an accounting counter twice. */
struct ProviderFinanceDailyTotals {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    int64_t day_start{0};
    DDCents dd_income;
    DGBSatoshis dgb_cost;
    uint32_t successful_transfers{0};
    uint32_t user_paid_transfers{0};
    uint32_t public_sponsored_transfers{0};
    uint32_t restricted_sponsored_transfers{0};
    uint32_t maintenance_transactions{0};

    SERIALIZE_METHODS(ProviderFinanceDailyTotals, obj)
    {
        READWRITE(obj.version, obj.day_start, obj.dd_income, obj.dgb_cost,
                  obj.successful_transfers, obj.user_paid_transfers,
                  obj.public_sponsored_transfers,
                  obj.restricted_sponsored_transfers,
                  obj.maintenance_transactions);
    }
};

/** Wallet-local accounting authority for exactly one provider identity on
 * one chain. Ledgers with another identity or genesis are never merged. */
struct ProviderFinanceLedger {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    int64_t history_complete_from{0};
    bool earlier_history_partial{false};
    int64_t updated_at{0};
    std::vector<ProviderFinanceEvent> events;
    std::vector<ProviderFinanceDailyTotals> daily_totals;

    SERIALIZE_METHODS(ProviderFinanceLedger, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id,
                  obj.history_complete_from, obj.earlier_history_partial,
                  obj.updated_at, obj.events, obj.daily_totals);
    }
};

/** Chain- and provider-bound backup reminder metadata. No path or filename is
 * retained. A restore from a backup made before last_successful_backup_at
 * naturally asks for a fresh backup on the restored system. */
struct ProviderBackupStatus {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    int64_t identity_created_at{0};
    int64_t reminder_updated_at{0};
    int64_t last_successful_backup_at{0};
    int64_t external_backup_acknowledged_at{0};

    SERIALIZE_METHODS(ProviderBackupStatus, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id,
                  obj.identity_created_at,
                  obj.reminder_updated_at, obj.last_successful_backup_at,
                  obj.external_backup_acknowledged_at);
    }
};

/** How a running provider wallet services its bounded direct-message queues.
 * Automatic operation never weakens the provider policy, safety budget, or
 * authorization-manifest checks; it only removes the operator's per-message
 * RPC trigger. */
enum class ProviderOperationMode : uint8_t {
    AUTOMATIC,
    MANUAL,
};

struct ProviderSettings {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    bool enabled{false};
    uint256 policy_hash;
    int64_t updated_at{0};
    ProviderOperationMode operation_mode{ProviderOperationMode::AUTOMATIC};
    bool autostart{false};

    SERIALIZE_METHODS(ProviderSettings, obj)
    {
        READWRITE(obj.version, obj.enabled, obj.policy_hash, obj.updated_at,
                  Using<EnumByteFormatter<static_cast<uint8_t>(
                      ProviderOperationMode::MANUAL)>>(obj.operation_mode),
                  obj.autostart);
    }
};

/** Wallet-local targets and strictly finite spending ceilings for automatic
 * pool maintenance. Free successor recycling does not consume these limits;
 * every transaction that creates new liquidity does. */
struct ProviderLiquidityPolicy {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    bool automatic_replenishment{true};
    bool paid_maintenance_approved{false};
    uint16_t target_admission_dgb{3};
    uint16_t target_operational_dgb{1};
    uint16_t target_admission_carriers{0};
    uint16_t target_operational_carriers{0};
    DGBSatoshis maximum_maintenance_fee_per_transaction;
    DGBSatoshis maximum_maintenance_fee_per_hour;
    DGBSatoshis maximum_maintenance_fee_per_day;
    int64_t updated_at{0};

    SERIALIZE_METHODS(ProviderLiquidityPolicy, obj)
    {
        READWRITE(obj.version, obj.automatic_replenishment,
                  obj.paid_maintenance_approved,
                  obj.target_admission_dgb,
                  obj.target_operational_dgb,
                  obj.target_admission_carriers,
                  obj.target_operational_carriers,
                  obj.maximum_maintenance_fee_per_transaction,
                  obj.maximum_maintenance_fee_per_hour,
                  obj.maximum_maintenance_fee_per_day,
                  obj.updated_at);
    }
};

enum class ProviderMaintenanceKind : uint8_t {
    REPLENISH_DGB,
    REPLENISH_CARRIER,
    WITHDRAW_CARRIER_EXCESS,
    PREPARE_DGB,
    PREPARE_CARRIER,
};

enum class ProviderMaintenanceState : uint8_t {
    PLANNED,
    BROADCAST,
    CONFIRMED,
    RELEASED,
    FAILED,
};

struct ProviderMaintenanceOutput {
    PoolPurpose purpose{PoolPurpose::OPERATIONAL};
    PoolAsset asset{PoolAsset::DGB};
    CScript script_pub_key;
    DGBSatoshis dgb_value;
    DDCents carrier_value;

    friend bool operator==(const ProviderMaintenanceOutput& lhs,
                           const ProviderMaintenanceOutput& rhs)
    {
        return lhs.purpose == rhs.purpose && lhs.asset == rhs.asset &&
               lhs.script_pub_key == rhs.script_pub_key &&
               lhs.dgb_value == rhs.dgb_value &&
               lhs.carrier_value == rhs.carrier_value;
    }

    SERIALIZE_METHODS(ProviderMaintenanceOutput, obj)
    {
        READWRITE(
            Using<EnumByteFormatter<static_cast<uint8_t>(PoolPurpose::OPERATIONAL)>>(obj.purpose),
            Using<EnumByteFormatter<static_cast<uint8_t>(PoolAsset::DD_CARRIER)>>(obj.asset),
            obj.script_pub_key, obj.dgb_value, obj.carrier_value);
    }
};

/** Restartable record written before a paid maintenance transaction is
 * committed. Persisting the exact output scripts makes a broadcast recoverable
 * even if shutdown occurs before its txid is attached to this record. */
struct ProviderMaintenanceRecord {
    static constexpr uint16_t CURRENT_VERSION{4};

    uint16_t version{CURRENT_VERSION};
    uint256 operation_id;
    uint256 plan_id;
    ProviderMaintenanceKind kind{ProviderMaintenanceKind::REPLENISH_DGB};
    ProviderMaintenanceState state{ProviderMaintenanceState::PLANNED};
    std::vector<ProviderMaintenanceOutput> outputs;
    /** Exact pool inputs consumed by a maintenance operation. This is empty
     * for replenishment, and binds carrier-withdrawal recovery across the
     * crash window between broadcast and the final pool update. */
    std::vector<COutPoint> source_inputs;
    /** Non-pool DigiDollar output of a carrier-excess withdrawal. V3 binds
     * this output as part of the restartable transaction template without
     * registering it as a replacement carrier slot. */
    CScript withdrawal_excess_script_pub_key;
    DDCents withdrawal_excess_amount;
    uint256 transaction_id;
    DGBSatoshis maximum_fee;
    DGBSatoshis actual_fee;
    int64_t created_at{0};
    int64_t updated_at{0};

    // A finite setup approval is independent of recurring maintenance budgets.
    // Older V3 records remain readable and never authorize setup implicitly.
    uint256 preparation_authorization;
    uint256 preparation_request;
    std::string preparation_error;

    bool IsPreparation() const
    {
        return kind == ProviderMaintenanceKind::PREPARE_DGB ||
               kind == ProviderMaintenanceKind::PREPARE_CARRIER;
    }

    SERIALIZE_METHODS(ProviderMaintenanceRecord, obj)
    {
        READWRITE(obj.version, obj.operation_id, obj.plan_id,
            Using<EnumByteFormatter<static_cast<uint8_t>(ProviderMaintenanceKind::PREPARE_CARRIER)>>(obj.kind),
            Using<EnumByteFormatter<static_cast<uint8_t>(ProviderMaintenanceState::FAILED)>>(obj.state),
            obj.outputs, obj.source_inputs,
            obj.withdrawal_excess_script_pub_key,
            obj.withdrawal_excess_amount, obj.transaction_id,
            obj.maximum_fee, obj.actual_fee, obj.created_at,
            obj.updated_at);
        if (obj.version >= 4) {
            READWRITE(obj.preparation_authorization, obj.preparation_request, obj.preparation_error);
        }
    }
};

struct ProviderMaintenanceLedger {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    int64_t accounting_time_high_water{0};
    std::vector<ProviderMaintenanceRecord> records;

    SERIALIZE_METHODS(ProviderMaintenanceLedger, obj)
    {
        READWRITE(obj.version, obj.accounting_time_high_water, obj.records);
    }
};

enum class CarrierWithdrawalMode : uint8_t {
    ALL_EXCESS,
    RELEASE_SLOT,
};

/** The last reviewed carrier-withdrawal preview. Keeping this wallet-local
 * record makes execution require the exact immediately preceding plan even
 * across a GUI restart. */
struct ProviderCarrierWithdrawalPlan {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 plan_id;
    uint256 operation_id;
    CarrierWithdrawalMode mode{CarrierWithdrawalMode::ALL_EXCESS};
    std::vector<COutPoint> source_carriers;
    std::vector<ProviderMaintenanceOutput> replacement_carriers;
    CScript excess_script_pub_key;
    DDCents excess_amount;
    DGBSatoshis estimated_fee;
    uint16_t target_operational_carriers_after_release{0};
    int64_t liquidity_policy_updated_at{0};
    int64_t created_at{0};
    int64_t expires_at{0};

    SERIALIZE_METHODS(ProviderCarrierWithdrawalPlan, obj)
    {
        READWRITE(
            obj.version, obj.plan_id, obj.operation_id,
            Using<EnumByteFormatter<static_cast<uint8_t>(
                CarrierWithdrawalMode::RELEASE_SLOT)>>(obj.mode),
            obj.source_carriers, obj.replacement_carriers,
            obj.excess_script_pub_key, obj.excess_amount,
            obj.estimated_fee,
            obj.target_operational_carriers_after_release,
            obj.liquidity_policy_updated_at,
            obj.created_at, obj.expires_at);
    }
};

struct ProviderPoolEntry {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    COutPoint outpoint;
    PoolPurpose purpose{PoolPurpose::ADMISSION};
    PoolAsset asset{PoolAsset::DGB};
    PoolEntryState state{PoolEntryState::AVAILABLE};
    CScript script_pub_key;
    DGBSatoshis dgb_value;
    DDCents carrier_value;
    int32_t confirmation_height{0};
    uint256 reservation_id;
    /** Durable provenance for transaction-created pool successors. This is
     * the provider commit key for payment successors and the maintenance
     * operation id for maintenance outputs. It is retained after confirmation
     * so a reorg can move the entry back to PENDING_SUCCESSOR without guessing
     * which durable operation created it. */
    uint256 origin_commit_key;
    int64_t updated_at{0};

    SERIALIZE_METHODS(ProviderPoolEntry, obj)
    {
        READWRITE(obj.version, obj.outpoint,
                  Using<EnumByteFormatter<static_cast<uint8_t>(PoolPurpose::OPERATIONAL)>>(obj.purpose),
                  Using<EnumByteFormatter<static_cast<uint8_t>(PoolAsset::DD_CARRIER)>>(obj.asset),
                  Using<EnumByteFormatter<static_cast<uint8_t>(PoolEntryState::INVALIDATED)>>(obj.state),
                  obj.script_pub_key, obj.dgb_value, obj.carrier_value,
                  obj.confirmation_height, obj.reservation_id,
                  obj.origin_commit_key, obj.updated_at);
    }
};

struct ProviderPoolReadiness {
    size_t admission_dgb{0};
    size_t admission_carriers{0};
    size_t operational_dgb{0};
    size_t operational_carriers{0};
    size_t complete_operational_slots{0};
    bool ready{false};
    std::vector<std::string> errors;
};

struct ServiceFeePlan {
    DDCents payment;
    DDCents service_fee;
    DDCents total_user_charge;
    FeeOutputKind output_kind{FeeOutputKind::NONE};
};

bool PolicyAllowsFundingModel(const ProviderPolicy& policy, FundingModel model);
bool ValidateProviderPolicy(const ProviderPolicy& policy, std::string& error);
bool ValidateProviderSafetyPolicy(const ProviderSafetyPolicy& safety,
                                  const ProviderPolicy& advertised,
                                  std::string& error);
bool ValidateClientSafetyPolicy(const ClientSafetyPolicy& policy, std::string& error);
bool ValidateProviderBudgetLedger(const ProviderBudgetLedger& ledger, std::string& error);
bool ValidateClientFeeLedger(const ClientFeeLedger& ledger, std::string& error);
const FundingSafetyLimits& GetFundingSafetyLimits(const ProviderSafetyPolicy& policy,
                                                  FundingModel model,
                                                  SponsorshipScope scope);
uint256 GetRecipientBudgetBucket(const ProviderBudgetLedger& ledger,
                                 const CScript& recipient_script);
/** Pseudonymize canonical NetGroupManager::GetGroup() bytes before
 * persistence. The canonical bytes are stable across process restarts but
 * must never themselves be stored in the wallet database. */
uint256 GetNetgroupBudgetBucket(const ProviderBudgetLedger& ledger,
                                const std::vector<unsigned char>& canonical_netgroup);
/** Stable semantic flow key shared by capacity admission and the later quote.
 * It deliberately excludes peer_id, netgroup and request contents. */
uint256 GetProviderRequestSlotKey(const PaymasterId& provider_id,
                                  const std::string& request_id,
                                  const uint256& session_id);
bool ReserveProviderCapacityAdmission(ProviderBudgetLedger& ledger,
                                      const ProviderSafetyPolicy& policy,
                                      const uint256& request_key,
                                      const uint256& request_hash,
                                      const uint256& netgroup_bucket,
                                      FundingModel funding_model,
                                      bool requires_carrier,
                                      int64_t expires_at,
                                      int64_t now,
                                      std::string& error);
bool BindProviderCapacityQuote(ProviderBudgetLedger& ledger,
                               const uint256& request_key,
                               const uint256& quote_request_hash,
                               const uint256& netgroup_bucket,
                               int64_t now,
                               std::string& error);
bool PromoteProviderCapacityAdmission(ProviderBudgetLedger& ledger,
                                      const uint256& request_key,
                                      const uint256& quote_request_hash,
                                      const uint256& commit_key,
                                      int64_t now,
                                      std::string& error);
bool ReleaseProviderCapacityAdmission(ProviderBudgetLedger& ledger,
                                      const uint256& request_key,
                                      const uint256& request_hash,
                                      int64_t now,
                                      std::string& error);
/** Atomically consume one wallet-local quote-request token. Exact semantic
 * retries are idempotent; reusing a request key for another netgroup fails
 * closed. */
bool RecordProviderQuoteRequest(ProviderBudgetLedger& ledger,
                                const ProviderSafetyPolicy& policy,
                                const uint256& request_key,
                                const uint256& netgroup_bucket,
                                int64_t now,
                                std::string& error);
/** Variant that binds a stable semantic slot to its canonical contents. */
bool RecordProviderQuoteRequest(ProviderBudgetLedger& ledger,
                                const ProviderSafetyPolicy& policy,
                                const uint256& request_key,
                                const uint256& request_hash,
                                const uint256& netgroup_bucket,
                                int64_t now,
                                std::string& error);
bool ReserveProviderBudget(ProviderBudgetLedger& ledger,
                           const ProviderSafetyPolicy& policy,
                           FundingModel model,
                           SponsorshipScope scope,
                           const uint256& commit_key,
                           DGBSatoshis network_fee,
                           const uint256& recipient_bucket,
                           int64_t now,
                           std::string& error,
                           const uint256& netgroup_bucket = {});
bool SpendProviderBudget(ProviderBudgetLedger& ledger,
                         const uint256& commit_key,
                         int64_t now,
                         std::string& error);
bool ReleaseProviderBudget(ProviderBudgetLedger& ledger,
                           const uint256& commit_key,
                           int64_t now,
                           std::string& error);
/** Require one persistent provider-budget row to be the exact reservation
 * authorized by the immutable provider manifest. A historical policy hash is
 * accepted only while completing or recovering an already durable exact
 * provider signature; it cannot authorize a new signature. */
bool ValidateProviderBudgetReservationBinding(
    const ProviderAuthorizationManifest& manifest,
    const ProviderAttempt& attempt,
    const ProviderBudgetReservation& reservation,
    const ProviderSafetyPolicy& policy,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error);
ProviderSafetyStatus EvaluateProviderSafetyStatus(const ProviderBudgetLedger& ledger,
                                                  const ProviderSafetyPolicy& policy,
                                                  FundingModel model,
                                                  SponsorshipScope scope,
                                                  const uint256& recipient_bucket,
                                                  DGBSatoshis proposed_network_fee,
                                                  int64_t now,
                                                  const uint256& netgroup_bucket = {});
/** Pure preflight for the Capacity -> quote continuation. The exact live
 * RESERVED admission is bound and promoted on a copy before applying the
 * ordinary quote limits, so its own active-capacity slot is not counted
 * twice. All other economic limits and active entries remain authoritative.
 * quote_request_hash is the canonical request with restricted secrets
 * redacted; requires_carrier is the actual quote-template requirement. */
ProviderSafetyStatus EvaluateProviderCapacityContinuationPreflight(
    const ProviderBudgetLedger& ledger,
    const ProviderSafetyPolicy& policy,
    const PaymasterCapacityRequest& capacity_request,
    const PaymasterQuoteRequest& quote_request,
    const uint256& quote_request_hash,
    bool requires_carrier,
    const uint256& recipient_bucket,
    DGBSatoshis proposed_network_fee,
    int64_t now,
    const uint256& netgroup_bucket);
bool ReserveClientFee(ClientFeeLedger& ledger,
                      const ClientSafetyPolicy& policy,
                      const uint256& commit_key,
                      DDCents service_fee,
                      int64_t now,
                      std::string& error);
bool SpendClientFee(ClientFeeLedger& ledger,
                    const uint256& commit_key,
                    int64_t now,
                    std::string& error);
bool ReleaseClientFee(ClientFeeLedger& ledger,
                      const uint256& commit_key,
                      int64_t now,
                      std::string& error);
bool ValidateProviderIdentityRecord(const ProviderIdentityRecord& identity);
bool ValidateProviderFinanceEvent(const ProviderFinanceEvent& event,
                                  std::string& error);
bool ValidateProviderFinanceLedger(const ProviderFinanceLedger& ledger,
                                   std::string& error);
bool ValidateProviderBackupStatus(const ProviderBackupStatus& status,
                                  std::string& error);
bool RebuildProviderFinanceDailyTotals(ProviderFinanceLedger& ledger,
                                       std::string& error);
bool UpsertProviderFinanceEvent(ProviderFinanceLedger& ledger,
                                const ProviderFinanceEvent& event,
                                std::string& error);
bool ProviderBackupRequired(const ProviderBackupStatus& status);
bool ValidateProviderPoolEntries(const std::vector<ProviderPoolEntry>& entries,
                                 std::string& error);
/** States that still dedicate an outpoint to Paymaster operation and must
 * therefore be excluded from ordinary wallet coin selection. */
bool IsActiveProviderPoolState(PoolEntryState state);
bool ValidateProviderLiquidityPolicy(const ProviderLiquidityPolicy& policy,
                                     std::string& error);
/** Return whether the saved targets can ever satisfy the advertised
 * provider policy. A USER_PAID offer needs carrier capacity for both
 * admission proofs and an actual payment; a deliberate zero operational
 * carrier target therefore keeps that offer configured but not startable. */
bool ProviderLiquidityTargetsSatisfyPolicy(
    const ProviderLiquidityPolicy& liquidity_policy,
    const ProviderPolicy& provider_policy);
bool ValidateProviderMaintenanceLedger(const ProviderMaintenanceLedger& ledger,
                                       std::string& error);
bool ValidateProviderCarrierWithdrawalPlan(
    const ProviderCarrierWithdrawalPlan& plan,
    std::string& error);
bool ReserveProviderMaintenanceBudget(ProviderMaintenanceLedger& ledger,
                                      const ProviderLiquidityPolicy& policy,
                                      ProviderMaintenanceRecord record,
                                      int64_t now,
                                      std::string& error);
bool SpendProviderMaintenanceBudget(ProviderMaintenanceLedger& ledger,
                                    const uint256& operation_id,
                                    const uint256& transaction_id,
                                    DGBSatoshis actual_fee,
                                    int64_t now,
                                    std::string& error);
bool ReleaseProviderMaintenanceBudget(ProviderMaintenanceLedger& ledger,
                                      const uint256& operation_id,
                                      int64_t now,
                                      std::string& error);
ProviderPoolReadiness EvaluateProviderPoolReadiness(
    const std::vector<ProviderPoolEntry>& entries,
    const ProviderPolicy& policy,
    int32_t tip_height,
    int32_t minimum_confirmations);
uint256 GetProviderPolicyHash(const ProviderPolicy& policy);
/** Domain-separated commitment to the wallet-local, non-advertised provider
 * loss limits used when an authorization manifest was created. */
uint256 GetProviderSafetyPolicyHash(const ProviderSafetyPolicy& policy);

std::optional<ServiceFeePlan> EvaluateServiceFee(const ProviderPolicy& policy,
                                                 FundingModel model,
                                                 DDCents payment,
                                                 std::string& error);

/** Return the exact carrier successor amount for a sub-1-DD fee. */
std::optional<DDCents> ComputeCarrierSuccessor(DDCents carrier,
                                               DDCents service_fee,
                                               std::string& error);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_PROVIDER_H
