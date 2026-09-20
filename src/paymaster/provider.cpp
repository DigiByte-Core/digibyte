// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Provider policy, pool lifecycle, and wallet-local economic safety limits.
 * Budget reservations are conservative before signing and become consumption
 * only with a durable commit; finite per-transfer and rolling limits can never
 * be overridden by a remote request.
 */

#include <paymaster/provider.h>

#include <consensus/amount.h>
#include <crypto/hmac_sha256.h>
#include <hash.h>
#include <paymaster/directory.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/reservation.h>
#include <paymaster/wire.h>
#include <script/standard.h>
#include <streams.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace DigiDollar::Paymaster {
namespace {

static constexpr int64_t SAFETY_HOUR_SECONDS{60 * 60};
static constexpr int64_t SAFETY_DAY_SECONDS{24 * SAFETY_HOUR_SECONDS};
static constexpr int64_t QUOTE_REQUEST_WINDOW_SECONDS{60};
static constexpr size_t MAX_BUDGET_LEDGER_ENTRIES{8192};
static constexpr size_t MAX_QUOTE_REQUEST_EVENTS{8192};
static constexpr size_t MAX_CAPACITY_ADMISSION_EVENTS{8192};
static constexpr size_t MAX_PROVIDER_FINANCE_EVENTS{1000000};

bool LimitsDisabled(const FundingSafetyLimits& limits)
{
    return limits.maximum_network_fee_per_transaction.value == 0 &&
           limits.maximum_reserved_network_fee.value == 0 &&
           limits.maximum_network_fee_per_hour.value == 0 &&
           limits.maximum_network_fee_per_day.value == 0 &&
           limits.maximum_completed_per_hour == 0 &&
           limits.maximum_completed_per_day == 0;
}

bool ValidateFundingSafetyLimits(const FundingSafetyLimits& limits,
                                 bool required,
                                 DGBSatoshis advertised_maximum,
                                 std::string& error)
{
    if (LimitsDisabled(limits)) {
        if (required) error = "PAYMASTER_SAFETY_LIMITS_REQUIRED";
        return !required;
    }
    if (limits.maximum_network_fee_per_transaction.value <= 0 ||
        !MoneyRange(limits.maximum_network_fee_per_transaction.value) ||
        !MoneyRange(limits.maximum_reserved_network_fee.value) ||
        !MoneyRange(limits.maximum_network_fee_per_hour.value) ||
        !MoneyRange(limits.maximum_network_fee_per_day.value) ||
        limits.maximum_network_fee_per_transaction.value > advertised_maximum.value ||
        limits.maximum_reserved_network_fee.value <
            limits.maximum_network_fee_per_transaction.value ||
        limits.maximum_network_fee_per_hour.value <
            limits.maximum_network_fee_per_transaction.value ||
        limits.maximum_network_fee_per_day.value <
            limits.maximum_network_fee_per_hour.value ||
        limits.maximum_completed_per_hour == 0 ||
        limits.maximum_completed_per_day < limits.maximum_completed_per_hour) {
        error = "PAYMASTER_INVALID_SAFETY_LIMITS";
        return false;
    }
    return true;
}

bool AddAmount(int64_t& total, int64_t value)
{
    if (value < 0 || total > std::numeric_limits<int64_t>::max() - value) return false;
    total += value;
    return true;
}

template <typename T>
uint256 CanonicalObjectHash(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    return Hash(MakeUCharSpan(stream));
}

bool ValidBudgetClass(FundingModel model, SponsorshipScope scope)
{
    if (model != FundingModel::USER_PAID && model != FundingModel::SPONSORED) return false;
    if (scope != SponsorshipScope::PUBLIC && scope != SponsorshipScope::RESTRICTED) return false;
    return model != FundingModel::USER_PAID || scope == SponsorshipScope::PUBLIC;
}

bool ValidateSafetyQuoteLimits(const ProviderSafetyPolicy& safety, std::string& error)
{
    if (safety.version != ProviderSafetyPolicy::CURRENT_VERSION || safety.updated_at <= 0) {
        error = "PAYMASTER_INVALID_SAFETY_POLICY";
        return false;
    }
    if (safety.maximum_active_quotes_total == 0 ||
        safety.maximum_active_quotes_total > MAX_BUDGET_LEDGER_ENTRIES ||
        safety.maximum_active_quotes_per_netgroup == 0 ||
        safety.maximum_active_quotes_per_recipient == 0 ||
        safety.maximum_quote_requests_per_netgroup_per_minute == 0 ||
        safety.maximum_quote_requests_per_netgroup_per_minute > MAX_QUOTE_REQUEST_EVENTS ||
        safety.maximum_active_quotes_per_netgroup > safety.maximum_active_quotes_total ||
        safety.maximum_active_quotes_per_recipient > safety.maximum_active_quotes_total) {
        error = "PAYMASTER_INVALID_SAFETY_QUOTE_LIMITS";
        return false;
    }
    return true;
}

bool SameBudgetClass(const ProviderBudgetReservation& reservation,
                     FundingModel model,
                     SponsorshipScope scope)
{
    return reservation.funding_model == model &&
           (model != FundingModel::SPONSORED || reservation.sponsorship_scope == scope);
}

int64_t EffectiveAccountingTime(int64_t now, int64_t high_water)
{
    return std::max(now, high_water);
}

void PruneProviderLedger(ProviderBudgetLedger& ledger, int64_t now)
{
    ledger.reservations.erase(
        std::remove_if(ledger.reservations.begin(), ledger.reservations.end(),
                       [now](const ProviderBudgetReservation& reservation) {
                           return reservation.state != BudgetReservationState::RESERVED &&
                                  TimeDeltaExceeds(now, reservation.updated_at, SAFETY_DAY_SECONDS);
                       }),
        ledger.reservations.end());
    ledger.quote_requests.erase(
        std::remove_if(ledger.quote_requests.begin(), ledger.quote_requests.end(),
                       [now](const ProviderQuoteRequestEvent& event) {
                           return TimeDeltaExceeds(now, event.admitted_at,
                                                   QUOTE_REQUEST_WINDOW_SECONDS - 1);
                       }),
        ledger.quote_requests.end());
    ledger.capacity_admissions.erase(
        std::remove_if(
            ledger.capacity_admissions.begin(),
            ledger.capacity_admissions.end(),
            [now](const ProviderCapacityAdmission& admission) {
                return admission.state != CapacityAdmissionState::RESERVED &&
                       TimeDeltaExceeds(now, admission.updated_at,
                                        SAFETY_DAY_SECONDS);
            }),
        ledger.capacity_admissions.end());
}

void PruneProviderMaintenanceLedger(ProviderMaintenanceLedger& ledger,
                                    int64_t now)
{
    ledger.records.erase(
        std::remove_if(
            ledger.records.begin(), ledger.records.end(),
            [now](const ProviderMaintenanceRecord& record) {
                const bool terminal =
                    record.state == ProviderMaintenanceState::CONFIRMED ||
                    record.state == ProviderMaintenanceState::RELEASED ||
                    record.state == ProviderMaintenanceState::FAILED;
                return terminal &&
                       TimeDeltaExceeds(now, record.updated_at,
                                        SAFETY_DAY_SECONDS);
            }),
        ledger.records.end());
}

void PruneClientLedger(ClientFeeLedger& ledger, int64_t now)
{
    ledger.reservations.erase(
        std::remove_if(ledger.reservations.begin(), ledger.reservations.end(),
                       [now](const ClientFeeReservation& reservation) {
                           return reservation.state != BudgetReservationState::RESERVED &&
                                  TimeDeltaExceeds(now, reservation.updated_at, SAFETY_DAY_SECONDS);
                       }),
        ledger.reservations.end());
}

} // namespace

bool PolicyAllowsFundingModel(const ProviderPolicy& policy, FundingModel model)
{
    if (model != FundingModel::USER_PAID && model != FundingModel::SPONSORED) return false;
    const uint8_t bit{static_cast<uint8_t>(1U << static_cast<uint8_t>(model))};
    return (policy.funding_models & bit) != 0;
}

bool ValidateProviderPolicy(const ProviderPolicy& policy, std::string& error)
{
    error.clear();
    if (policy.version != ProviderPolicy::CURRENT_VERSION || policy.funding_models == 0 ||
        (policy.funding_models & ~FUNDING_MODEL_ALL) != 0) {
        error = "PAYMASTER_INVALID_FUNDING_MODELS";
        return false;
    }
    if (policy.sponsorship_scope != SponsorshipScope::PUBLIC &&
        policy.sponsorship_scope != SponsorshipScope::RESTRICTED) {
        error = "PAYMASTER_INVALID_SPONSORSHIP_SCOPE";
        return false;
    }
    if (policy.sponsorship_scope == SponsorshipScope::RESTRICTED &&
        (policy.funding_models != FUNDING_MODEL_SPONSORED || policy.fee_rate_bps != 0)) {
        error = "PAYMASTER_INVALID_RESTRICTED_POLICY";
        return false;
    }
    if (!PolicyAllowsFundingModel(policy, FundingModel::USER_PAID) && policy.fee_rate_bps != 0) {
        error = "PAYMASTER_SPONSORED_FEE";
        return false;
    }
    if (policy.fee_rate_bps > MAX_RATE_BPS || policy.fee_rate_bps % 10 != 0) {
        error = "PAYMASTER_INVALID_RATE";
        return false;
    }
    if (policy.min_payment.value < 100 || policy.max_payment.value > MAX_DD_OUTPUT_CENTS ||
        policy.min_payment.value > policy.max_payment.value) {
        error = "PAYMASTER_INVALID_PAYMENT_RANGE";
        return false;
    }
    if (policy.quote_ttl <= 0 || policy.quote_ttl > MAX_QUOTE_TTL_SECONDS) {
        error = "PAYMASTER_INVALID_QUOTE_TTL";
        return false;
    }
    if (policy.maximum_network_fee.value <= 0 ||
        !MoneyRange(policy.maximum_network_fee.value)) {
        error = "PAYMASTER_INVALID_NETWORK_FEE_CAP";
        return false;
    }
    return true;
}

bool ValidateProviderSafetyPolicy(const ProviderSafetyPolicy& safety,
                                  const ProviderPolicy& advertised,
                                  std::string& error)
{
    error.clear();
    std::string policy_error;
    if (!ValidateProviderPolicy(advertised, policy_error)) {
        error = policy_error.empty() ? "PAYMASTER_INVALID_SAFETY_POLICY" : policy_error;
        return false;
    }
    if (!ValidateSafetyQuoteLimits(safety, error)) return false;
    const bool user_paid = PolicyAllowsFundingModel(advertised, FundingModel::USER_PAID);
    const bool sponsored = PolicyAllowsFundingModel(advertised, FundingModel::SPONSORED);
    const bool public_sponsored = sponsored &&
                                  advertised.sponsorship_scope == SponsorshipScope::PUBLIC;
    const bool restricted_sponsored = sponsored &&
                                      advertised.sponsorship_scope == SponsorshipScope::RESTRICTED;
    if (!ValidateFundingSafetyLimits(safety.user_paid, user_paid,
                                     advertised.maximum_network_fee, error) ||
        !ValidateFundingSafetyLimits(safety.public_sponsored, public_sponsored,
                                     advertised.maximum_network_fee, error) ||
        !ValidateFundingSafetyLimits(safety.restricted_sponsored, restricted_sponsored,
                                     advertised.maximum_network_fee, error)) {
        return false;
    }
    return true;
}

bool ValidateClientSafetyPolicy(const ClientSafetyPolicy& policy, std::string& error)
{
    error.clear();
    if (policy.version != ClientSafetyPolicy::CURRENT_VERSION || policy.updated_at <= 0 ||
        policy.maximum_service_fee_per_transaction.value < 0 ||
        policy.maximum_service_fee_per_transaction.value > MAX_DD_OUTPUT_CENTS ||
        policy.maximum_service_fee_per_day.value <
            policy.maximum_service_fee_per_transaction.value ||
        policy.maximum_service_fee_per_day.value > MAX_DD_OUTPUT_CENTS) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_POLICY";
        return false;
    }
    return true;
}

bool ValidateProviderBudgetLedger(const ProviderBudgetLedger& ledger, std::string& error)
{
    error.clear();
    if (ledger.version != ProviderBudgetLedger::CURRENT_VERSION ||
        ledger.recipient_bucket_secret.IsNull() || ledger.accounting_time_high_water <= 0 ||
        ledger.reservations.size() > MAX_BUDGET_LEDGER_ENTRIES ||
        ledger.quote_requests.size() > MAX_QUOTE_REQUEST_EVENTS ||
        ledger.capacity_admissions.size() > MAX_CAPACITY_ADMISSION_EVENTS) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER";
        return false;
    }
    std::set<uint256> commit_keys;
    for (const ProviderBudgetReservation& reservation : ledger.reservations) {
        if (reservation.version != ProviderBudgetReservation::CURRENT_VERSION ||
            reservation.commit_key.IsNull() || !commit_keys.insert(reservation.commit_key).second ||
            reservation.network_fee.value <= 0 || reservation.recipient_bucket.IsNull() ||
            reservation.netgroup_bucket.IsNull() ||
            reservation.reserved_at <= 0 || reservation.updated_at < reservation.reserved_at ||
            reservation.updated_at > ledger.accounting_time_high_water ||
            (reservation.state != BudgetReservationState::RESERVED &&
             reservation.state != BudgetReservationState::SPENT &&
             reservation.state != BudgetReservationState::RELEASED) ||
            (reservation.funding_model != FundingModel::USER_PAID &&
             reservation.funding_model != FundingModel::SPONSORED) ||
            (reservation.sponsorship_scope != SponsorshipScope::PUBLIC &&
             reservation.sponsorship_scope != SponsorshipScope::RESTRICTED) ||
            (reservation.funding_model == FundingModel::USER_PAID &&
             reservation.sponsorship_scope != SponsorshipScope::PUBLIC)) {
            error = "PAYMASTER_INVALID_PROVIDER_BUDGET_RESERVATION";
            return false;
        }
    }
    std::set<uint256> request_keys;
    for (const ProviderQuoteRequestEvent& event : ledger.quote_requests) {
        if (event.version != ProviderQuoteRequestEvent::CURRENT_VERSION ||
            event.request_key.IsNull() || event.request_hash.IsNull() ||
            event.netgroup_bucket.IsNull() ||
            !request_keys.insert(event.request_key).second || event.admitted_at <= 0 ||
            event.admitted_at > ledger.accounting_time_high_water) {
            error = "PAYMASTER_INVALID_PROVIDER_QUOTE_REQUEST_EVENT";
            return false;
        }
    }
    std::set<uint256> capacity_request_keys;
    for (const ProviderCapacityAdmission& admission :
         ledger.capacity_admissions) {
        const bool valid_state =
            admission.state == CapacityAdmissionState::RESERVED ||
            admission.state == CapacityAdmissionState::PROMOTED ||
            admission.state == CapacityAdmissionState::RELEASED;
        const bool valid_model =
            admission.funding_model == FundingModel::USER_PAID ||
            admission.funding_model == FundingModel::SPONSORED;
        const bool valid_state_fields =
            (admission.state == CapacityAdmissionState::RESERVED &&
             admission.commit_key.IsNull()) ||
            (admission.state == CapacityAdmissionState::PROMOTED &&
             !admission.quote_request_hash.IsNull() &&
             !admission.commit_key.IsNull()) ||
            (admission.state == CapacityAdmissionState::RELEASED &&
             admission.commit_key.IsNull());
        if (admission.version != ProviderCapacityAdmission::CURRENT_VERSION ||
            admission.request_key.IsNull() || admission.request_hash.IsNull() ||
            admission.netgroup_bucket.IsNull() || !valid_model ||
            (admission.requires_carrier &&
             admission.funding_model != FundingModel::USER_PAID) ||
            !valid_state || !valid_state_fields ||
            !capacity_request_keys.insert(admission.request_key).second ||
            admission.admitted_at <= 0 ||
            admission.expires_at <= admission.admitted_at ||
            TimeDeltaExceeds(admission.expires_at, admission.admitted_at,
                             MAX_QUOTE_TTL_SECONDS) ||
            admission.updated_at < admission.admitted_at ||
            admission.updated_at > ledger.accounting_time_high_water) {
            error = "PAYMASTER_INVALID_CAPACITY_ADMISSION";
            return false;
        }
    }
    return true;
}

bool ValidateClientFeeLedger(const ClientFeeLedger& ledger, std::string& error)
{
    error.clear();
    if (ledger.version != ClientFeeLedger::CURRENT_VERSION ||
        ledger.accounting_time_high_water <= 0 ||
        ledger.reservations.size() > MAX_BUDGET_LEDGER_ENTRIES) {
        error = "PAYMASTER_INVALID_CLIENT_FEE_LEDGER";
        return false;
    }
    std::set<uint256> commit_keys;
    for (const ClientFeeReservation& reservation : ledger.reservations) {
        if (reservation.version != ClientFeeReservation::CURRENT_VERSION ||
            reservation.commit_key.IsNull() || !commit_keys.insert(reservation.commit_key).second ||
            reservation.service_fee.value < 0 ||
            reservation.service_fee.value > MAX_DD_OUTPUT_CENTS ||
            (reservation.state != BudgetReservationState::RESERVED &&
             reservation.state != BudgetReservationState::SPENT &&
             reservation.state != BudgetReservationState::RELEASED) ||
            reservation.reserved_at <= 0 || reservation.updated_at < reservation.reserved_at ||
            reservation.updated_at > ledger.accounting_time_high_water) {
            error = "PAYMASTER_INVALID_CLIENT_FEE_RESERVATION";
            return false;
        }
    }
    return true;
}

const FundingSafetyLimits& GetFundingSafetyLimits(const ProviderSafetyPolicy& policy,
                                                  FundingModel model,
                                                  SponsorshipScope scope)
{
    if (model == FundingModel::USER_PAID) return policy.user_paid;
    return scope == SponsorshipScope::RESTRICTED ? policy.restricted_sponsored : policy.public_sponsored;
}

uint256 GetRecipientBudgetBucket(const ProviderBudgetLedger& ledger,
                                 const CScript& recipient_script)
{
    if (ledger.recipient_bucket_secret.IsNull() || recipient_script.empty()) return {};
    CDataStream payload{SER_NETWORK, 0};
    payload << std::string{"DigiByte Paymaster Recipient Budget v1"}
            << recipient_script;
    uint256 bucket;
    CHMAC_SHA256{ledger.recipient_bucket_secret.begin(),
                 ledger.recipient_bucket_secret.size()}
        .Write(MakeUCharSpan(payload).data(), payload.size())
        .Finalize(bucket.begin());
    return bucket;
}

uint256 GetNetgroupBudgetBucket(const ProviderBudgetLedger& ledger,
                                const std::vector<unsigned char>& canonical_netgroup)
{
    if (ledger.recipient_bucket_secret.IsNull() || canonical_netgroup.empty()) {
        return {};
    }
    CDataStream payload{SER_NETWORK, 0};
    payload << std::string{"DigiByte Paymaster Netgroup Budget v2"}
            << canonical_netgroup;
    uint256 bucket;
    CHMAC_SHA256{ledger.recipient_bucket_secret.begin(),
                 ledger.recipient_bucket_secret.size()}
        .Write(MakeUCharSpan(payload).data(), payload.size())
        .Finalize(bucket.begin());
    return bucket;
}

uint256 GetProviderRequestSlotKey(const PaymasterId& provider_id,
                                  const std::string& request_id,
                                  const uint256& session_id)
{
    if (provider_id.IsNull() || request_id.empty() || session_id.IsNull()) {
        return {};
    }
    HashWriter hasher = TaggedHash("DigiByte Paymaster Request Slot v1");
    hasher << provider_id << request_id << session_id;
    return hasher.GetSHA256();
}

bool ReserveProviderCapacityAdmission(ProviderBudgetLedger& ledger,
                                      const ProviderSafetyPolicy& policy,
                                      const uint256& request_key,
                                      const uint256& request_hash,
                                      const uint256& netgroup_bucket,
                                      FundingModel funding_model,
                                      bool requires_carrier,
                                      int64_t expires_at,
                                      int64_t now,
                                      std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error) ||
        !ValidateSafetyQuoteLimits(policy, validation_error)) {
        error = validation_error;
        return false;
    }
    if (request_key.IsNull() || request_hash.IsNull() ||
        netgroup_bucket.IsNull() ||
        (funding_model != FundingModel::USER_PAID &&
         funding_model != FundingModel::SPONSORED) ||
        (requires_carrier && funding_model != FundingModel::USER_PAID) ||
        now <= 0 || expires_at <= now ||
        TimeDeltaExceeds(expires_at, now, MAX_QUOTE_TTL_SECONDS)) {
        error = "PAYMASTER_INVALID_CAPACITY_ADMISSION";
        return false;
    }
    for (const ProviderCapacityAdmission& admission :
         ledger.capacity_admissions) {
        if (admission.request_key != request_key) continue;
        if (admission.request_hash == request_hash &&
            admission.netgroup_bucket == netgroup_bucket &&
            admission.funding_model == funding_model &&
            admission.requires_carrier == requires_carrier &&
            admission.expires_at == expires_at) {
            return true;
        }
        error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
        return false;
    }

    const int64_t effective_now = EffectiveAccountingTime(
        now, ledger.accounting_time_high_water);
    if (expires_at <= effective_now) {
        error = "PAYMASTER_CAPACITY_ADMISSION_EXPIRED";
        return false;
    }
    // Prune on a copy so a rejected admission remains a strictly read-only
    // operation. This also prevents old released/promoted events from
    // permanently filling the ledger before pruning gets a chance to run.
    ProviderBudgetLedger candidate{ledger};
    candidate.version = ProviderBudgetLedger::CURRENT_VERSION;
    candidate.accounting_time_high_water = effective_now;
    PruneProviderLedger(candidate, effective_now);
    size_t active_total{0};
    size_t active_for_netgroup{0};
    size_t requests_for_netgroup{0};
    for (const ProviderBudgetReservation& reservation :
         candidate.reservations) {
        if (reservation.state != BudgetReservationState::RESERVED) continue;
        ++active_total;
        if (reservation.netgroup_bucket.IsNull() ||
            reservation.netgroup_bucket == netgroup_bucket) {
            ++active_for_netgroup;
        }
    }
    for (const ProviderCapacityAdmission& admission :
         candidate.capacity_admissions) {
        if (!TimeDeltaExceeds(effective_now, admission.admitted_at,
                              QUOTE_REQUEST_WINDOW_SECONDS - 1) &&
            admission.netgroup_bucket == netgroup_bucket) {
            ++requests_for_netgroup;
        }
        if (admission.state != CapacityAdmissionState::RESERVED ||
            admission.expires_at <= effective_now) {
            continue;
        }
        ++active_total;
        if (admission.netgroup_bucket == netgroup_bucket) {
            ++active_for_netgroup;
        }
    }
    if (requests_for_netgroup >=
        policy.maximum_quote_requests_per_netgroup_per_minute) {
        error = "PAYMASTER_QUOTE_REQUEST_RATE_EXHAUSTED";
        return false;
    }
    if (active_total >= policy.maximum_active_quotes_total ||
        active_for_netgroup >= policy.maximum_active_quotes_per_netgroup) {
        error = "PAYMASTER_SAFETY_LIMIT_EXHAUSTED";
        return false;
    }
    if (candidate.capacity_admissions.size() >=
        MAX_CAPACITY_ADMISSION_EVENTS) {
        error = "PAYMASTER_CAPACITY_ADMISSION_LEDGER_FULL";
        return false;
    }

    ProviderCapacityAdmission admission;
    admission.request_key = request_key;
    admission.request_hash = request_hash;
    admission.netgroup_bucket = netgroup_bucket;
    admission.funding_model = funding_model;
    admission.requires_carrier = requires_carrier;
    admission.state = CapacityAdmissionState::RESERVED;
    admission.admitted_at = effective_now;
    admission.expires_at = expires_at;
    admission.updated_at = effective_now;
    candidate.capacity_admissions.push_back(std::move(admission));
    ledger = std::move(candidate);
    return true;
}

bool BindProviderCapacityQuote(ProviderBudgetLedger& ledger,
                               const uint256& request_key,
                               const uint256& quote_request_hash,
                               const uint256& netgroup_bucket,
                               int64_t now,
                               std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (request_key.IsNull() || quote_request_hash.IsNull() ||
        netgroup_bucket.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_QUOTE_BINDING";
        return false;
    }
    const int64_t effective_now = EffectiveAccountingTime(
        now, ledger.accounting_time_high_water);
    for (ProviderCapacityAdmission& admission : ledger.capacity_admissions) {
        if (admission.request_key != request_key) continue;
        if (admission.netgroup_bucket != netgroup_bucket) {
            error = "PAYMASTER_CAPACITY_ADMISSION_NETGROUP_CONFLICT";
            return false;
        }
        if (admission.state == CapacityAdmissionState::RELEASED ||
            (admission.state == CapacityAdmissionState::RESERVED &&
             admission.expires_at <= effective_now)) {
            error = "PAYMASTER_CAPACITY_ADMISSION_EXPIRED";
            return false;
        }
        if (!admission.quote_request_hash.IsNull()) {
            if (admission.quote_request_hash == quote_request_hash) return true;
            error = "PAYMASTER_CAPACITY_QUOTE_CONFLICT";
            return false;
        }
        if (admission.state != CapacityAdmissionState::RESERVED) {
            error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
            return false;
        }
        ledger.version = ProviderBudgetLedger::CURRENT_VERSION;
        ledger.accounting_time_high_water = effective_now;
        admission.quote_request_hash = quote_request_hash;
        admission.updated_at = effective_now;
        PruneProviderLedger(ledger, effective_now);
        return true;
    }
    error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
    return false;
}

bool PromoteProviderCapacityAdmission(ProviderBudgetLedger& ledger,
                                      const uint256& request_key,
                                      const uint256& quote_request_hash,
                                      const uint256& commit_key,
                                      int64_t now,
                                      std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (request_key.IsNull() || quote_request_hash.IsNull() ||
        commit_key.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_PROMOTION";
        return false;
    }
    const int64_t effective_now = EffectiveAccountingTime(
        now, ledger.accounting_time_high_water);
    for (ProviderCapacityAdmission& admission : ledger.capacity_admissions) {
        if (admission.request_key != request_key) continue;
        if (admission.state == CapacityAdmissionState::PROMOTED) {
            if (admission.quote_request_hash == quote_request_hash &&
                admission.commit_key == commit_key) {
                return true;
            }
            error = "PAYMASTER_CAPACITY_PROMOTION_CONFLICT";
            return false;
        }
        if (admission.state != CapacityAdmissionState::RESERVED ||
            admission.expires_at <= effective_now) {
            error = "PAYMASTER_CAPACITY_ADMISSION_EXPIRED";
            return false;
        }
        if (admission.quote_request_hash != quote_request_hash) {
            error = "PAYMASTER_CAPACITY_QUOTE_CONFLICT";
            return false;
        }
        ledger.version = ProviderBudgetLedger::CURRENT_VERSION;
        ledger.accounting_time_high_water = effective_now;
        admission.state = CapacityAdmissionState::PROMOTED;
        admission.commit_key = commit_key;
        admission.updated_at = effective_now;
        PruneProviderLedger(ledger, effective_now);
        return true;
    }
    error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
    return false;
}

bool ReleaseProviderCapacityAdmission(ProviderBudgetLedger& ledger,
                                      const uint256& request_key,
                                      const uint256& request_hash,
                                      int64_t now,
                                      std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (request_key.IsNull() || request_hash.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_RELEASE";
        return false;
    }
    const int64_t effective_now = EffectiveAccountingTime(
        now, ledger.accounting_time_high_water);
    for (ProviderCapacityAdmission& admission : ledger.capacity_admissions) {
        if (admission.request_key != request_key) continue;
        if (admission.request_hash != request_hash) {
            error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
            return false;
        }
        if (admission.state == CapacityAdmissionState::RELEASED) return true;
        if (admission.state != CapacityAdmissionState::RESERVED) {
            error = "PAYMASTER_CAPACITY_ADMISSION_PROMOTED";
            return false;
        }
        ledger.version = ProviderBudgetLedger::CURRENT_VERSION;
        ledger.accounting_time_high_water = effective_now;
        admission.state = CapacityAdmissionState::RELEASED;
        admission.updated_at = effective_now;
        PruneProviderLedger(ledger, effective_now);
        return true;
    }
    error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
    return false;
}

bool RecordProviderQuoteRequest(ProviderBudgetLedger& ledger,
                                const ProviderSafetyPolicy& policy,
                                const uint256& request_key,
                                const uint256& netgroup_bucket,
                                int64_t now,
                                std::string& error)
{
    return RecordProviderQuoteRequest(ledger, policy, request_key, request_key,
                                      netgroup_bucket, now, error);
}

bool RecordProviderQuoteRequest(ProviderBudgetLedger& ledger,
                                const ProviderSafetyPolicy& policy,
                                const uint256& request_key,
                                const uint256& request_hash,
                                const uint256& netgroup_bucket,
                                int64_t now,
                                std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error) ||
        !ValidateSafetyQuoteLimits(policy, validation_error)) {
        error = validation_error;
        return false;
    }
    if (request_key.IsNull() || request_hash.IsNull() ||
        netgroup_bucket.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_REQUEST_BUDGET_EVENT";
        return false;
    }
    for (const ProviderQuoteRequestEvent& event : ledger.quote_requests) {
        if (event.request_key != request_key) continue;
        const bool exact =
            event.version == ProviderQuoteRequestEvent::CURRENT_VERSION &&
            event.request_hash == request_hash;
        if (exact &&
            event.netgroup_bucket == netgroup_bucket) {
            return true;
        }
        error = "PAYMASTER_QUOTE_REQUEST_BUDGET_CONFLICT";
        return false;
    }

    const int64_t effective_now = EffectiveAccountingTime(now, ledger.accounting_time_high_water);
    size_t active_for_netgroup{0};
    size_t retained_events{0};
    for (const ProviderQuoteRequestEvent& event : ledger.quote_requests) {
        if (TimeDeltaExceeds(effective_now, event.admitted_at,
                             QUOTE_REQUEST_WINDOW_SECONDS - 1)) {
            continue;
        }
        ++retained_events;
        if (event.netgroup_bucket == netgroup_bucket) ++active_for_netgroup;
    }
    if (active_for_netgroup >= policy.maximum_quote_requests_per_netgroup_per_minute) {
        error = "PAYMASTER_QUOTE_REQUEST_RATE_EXHAUSTED";
        return false;
    }
    if (retained_events >= MAX_QUOTE_REQUEST_EVENTS) {
        error = "PAYMASTER_QUOTE_REQUEST_LEDGER_FULL";
        return false;
    }

    ledger.version = ProviderBudgetLedger::CURRENT_VERSION;
    ledger.accounting_time_high_water = effective_now;
    PruneProviderLedger(ledger, effective_now);
    ledger.quote_requests.push_back({ProviderQuoteRequestEvent::CURRENT_VERSION,
                                     request_key, request_hash, netgroup_bucket,
                                     effective_now});
    return true;
}

ProviderSafetyStatus EvaluateProviderSafetyStatus(const ProviderBudgetLedger& ledger,
                                                  const ProviderSafetyPolicy& policy,
                                                  FundingModel model,
                                                  SponsorshipScope scope,
                                                  const uint256& recipient_bucket,
                                                  DGBSatoshis proposed_network_fee,
                                                  int64_t now,
                                                  const uint256& netgroup_bucket)
{
    ProviderSafetyStatus status;
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error) ||
        !ValidateSafetyQuoteLimits(policy, validation_error)) {
        status.errors.push_back(validation_error);
        return status;
    }
    if (!ValidBudgetClass(model, scope)) {
        status.errors.push_back("PAYMASTER_INVALID_FUNDING_MODEL_OR_SCOPE");
        return status;
    }
    const FundingSafetyLimits& limits = GetFundingSafetyLimits(policy, model, scope);
    if (!ValidateFundingSafetyLimits(
            limits, /*required=*/true,
            DGBSatoshis{std::numeric_limits<int64_t>::max()}, validation_error)) {
        status.errors.push_back(validation_error);
        return status;
    }
    if (now <= 0 || proposed_network_fee.value <= 0 || recipient_bucket.IsNull()) {
        status.errors.push_back("PAYMASTER_INVALID_BUDGET_RESERVATION");
        return status;
    }
    const int64_t effective_now = EffectiveAccountingTime(now, ledger.accounting_time_high_water);
    int64_t reserved_for_class{0};
    int64_t projected_hour{0};
    int64_t projected_day{0};
    uint32_t projected_count_hour{0};
    uint32_t projected_count_day{0};
    uint32_t recipient_active{0};
    bool arithmetic_ok{true};
    for (const ProviderCapacityAdmission& admission :
         ledger.capacity_admissions) {
        if (admission.state != CapacityAdmissionState::RESERVED ||
            admission.expires_at <= effective_now) {
            continue;
        }
        ++status.active_quotes;
        if (!netgroup_bucket.IsNull() &&
            admission.netgroup_bucket == netgroup_bucket) {
            ++status.active_quotes_for_netgroup;
        }
    }
    for (const ProviderBudgetReservation& reservation : ledger.reservations) {
        if (reservation.state == BudgetReservationState::RESERVED) {
            ++status.active_quotes;
            if (reservation.recipient_bucket == recipient_bucket) ++recipient_active;
            if (!netgroup_bucket.IsNull() &&
                (reservation.netgroup_bucket.IsNull() ||
                 reservation.netgroup_bucket == netgroup_bucket)) {
                ++status.active_quotes_for_netgroup;
            }
        }
        if (!SameBudgetClass(reservation, model, scope)) continue;
        if (reservation.state == BudgetReservationState::RESERVED) {
            arithmetic_ok &= AddAmount(reserved_for_class, reservation.network_fee.value);
            arithmetic_ok &= AddAmount(projected_hour, reservation.network_fee.value);
            arithmetic_ok &= AddAmount(projected_day, reservation.network_fee.value);
            if (projected_count_hour != std::numeric_limits<uint32_t>::max()) ++projected_count_hour;
            if (projected_count_day != std::numeric_limits<uint32_t>::max()) ++projected_count_day;
        } else if (reservation.state == BudgetReservationState::SPENT) {
            if (!TimeDeltaExceeds(effective_now, reservation.updated_at, SAFETY_DAY_SECONDS)) {
                arithmetic_ok &= AddAmount(status.spent_network_fee_last_day.value,
                                           reservation.network_fee.value);
                arithmetic_ok &= AddAmount(projected_day, reservation.network_fee.value);
                if (status.completed_last_day != std::numeric_limits<uint32_t>::max()) {
                    ++status.completed_last_day;
                }
                if (projected_count_day != std::numeric_limits<uint32_t>::max()) ++projected_count_day;
            }
            if (!TimeDeltaExceeds(effective_now, reservation.updated_at, SAFETY_HOUR_SECONDS)) {
                arithmetic_ok &= AddAmount(status.spent_network_fee_last_hour.value,
                                           reservation.network_fee.value);
                arithmetic_ok &= AddAmount(projected_hour, reservation.network_fee.value);
                if (status.completed_last_hour != std::numeric_limits<uint32_t>::max()) {
                    ++status.completed_last_hour;
                }
                if (projected_count_hour != std::numeric_limits<uint32_t>::max()) ++projected_count_hour;
            }
        }
    }
    status.reserved_network_fee.value = reserved_for_class;
    arithmetic_ok &= AddAmount(reserved_for_class, proposed_network_fee.value);
    arithmetic_ok &= AddAmount(projected_hour, proposed_network_fee.value);
    arithmetic_ok &= AddAmount(projected_day, proposed_network_fee.value);
    const bool count_overflow = projected_count_hour == std::numeric_limits<uint32_t>::max() ||
                                projected_count_day == std::numeric_limits<uint32_t>::max();
    if (!count_overflow) {
        ++projected_count_hour;
        ++projected_count_day;
    }
    if (!arithmetic_ok || count_overflow ||
        proposed_network_fee.value > limits.maximum_network_fee_per_transaction.value ||
        reserved_for_class > limits.maximum_reserved_network_fee.value ||
        projected_hour > limits.maximum_network_fee_per_hour.value ||
        projected_day > limits.maximum_network_fee_per_day.value ||
        projected_count_hour > limits.maximum_completed_per_hour ||
        projected_count_day > limits.maximum_completed_per_day ||
        status.active_quotes >= policy.maximum_active_quotes_total ||
        (!netgroup_bucket.IsNull() &&
         status.active_quotes_for_netgroup >= policy.maximum_active_quotes_per_netgroup) ||
        recipient_active >= policy.maximum_active_quotes_per_recipient) {
        status.errors.push_back("PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
        return status;
    }
    status.can_accept_quote = true;
    return status;
}

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
    const uint256& netgroup_bucket)
{
    const auto rejected = [](std::string error) {
        ProviderSafetyStatus status;
        status.errors.push_back(std::move(error));
        return status;
    };

    std::string error;
    if (!ValidateProviderBudgetLedger(ledger, error)) {
        return rejected(std::move(error));
    }
    const int64_t effective_now = EffectiveAccountingTime(
        now, ledger.accounting_time_high_water);
    if (!ValidateCapacityRequestEnvelope(
            capacity_request, quote_request.intent.genesis_hash,
            effective_now, error) ||
        !ValidateQuoteRequestEnvelope(
            quote_request, capacity_request.genesis_hash,
            effective_now, error)) {
        return rejected(std::move(error));
    }

    PaymasterQuoteRequest redacted_request{quote_request};
    redacted_request.restricted_descriptor.reset();
    redacted_request.restricted_capability.reset();
    const uint256 canonical_quote_request_hash{
        CanonicalObjectHash(redacted_request)};
    const uint256 capacity_request_hash{
        CanonicalObjectHash(capacity_request)};
    const uint256 request_key{GetProviderRequestSlotKey(
        capacity_request.provider_id, capacity_request.request_id,
        capacity_request.session_id)};
    const uint256 expected_recipient_bucket{GetRecipientBudgetBucket(
        ledger, quote_request.intent.recipient_script)};
    if (request_key.IsNull() || quote_request_hash.IsNull() ||
        quote_request_hash != canonical_quote_request_hash ||
        capacity_request_hash.IsNull() || netgroup_bucket.IsNull() ||
        recipient_bucket.IsNull() ||
        recipient_bucket != expected_recipient_bucket ||
        capacity_request.genesis_hash != quote_request.intent.genesis_hash ||
        capacity_request.provider_id != quote_request.intent.provider_id ||
        capacity_request.request_id != quote_request.intent.request_id ||
        capacity_request.session_id != quote_request.intent.session_id ||
        capacity_request.client_nonce != quote_request.intent.client_nonce ||
        capacity_request.funding_model !=
            quote_request.intent.funding_model ||
        capacity_request.requires_carrier != requires_carrier ||
        capacity_request.expires_at > quote_request.intent.expires_at) {
        return rejected("PAYMASTER_CAPACITY_CONTINUATION_MISMATCH");
    }

    const auto admission = std::find_if(
        ledger.capacity_admissions.begin(),
        ledger.capacity_admissions.end(),
        [&](const ProviderCapacityAdmission& candidate) {
            return candidate.request_key == request_key;
        });
    if (admission == ledger.capacity_admissions.end()) {
        return rejected("PAYMASTER_CAPACITY_ADMISSION_MISSING");
    }
    if (admission->netgroup_bucket != netgroup_bucket) {
        return rejected("PAYMASTER_CAPACITY_ADMISSION_NETGROUP_CONFLICT");
    }
    if (admission->state != CapacityAdmissionState::RESERVED) {
        return rejected("PAYMASTER_CAPACITY_ADMISSION_CONFLICT");
    }
    if (admission->expires_at <= effective_now) {
        return rejected("PAYMASTER_CAPACITY_ADMISSION_EXPIRED");
    }
    if (admission->request_hash != capacity_request_hash ||
        admission->funding_model != capacity_request.funding_model ||
        admission->requires_carrier != capacity_request.requires_carrier ||
        admission->expires_at != capacity_request.expires_at) {
        return rejected("PAYMASTER_CAPACITY_ADMISSION_CONFLICT");
    }
    if (!admission->quote_request_hash.IsNull() &&
        admission->quote_request_hash != quote_request_hash) {
        return rejected("PAYMASTER_CAPACITY_QUOTE_CONFLICT");
    }

    ProviderBudgetLedger candidate{ledger};
    if (!BindProviderCapacityQuote(
            candidate, request_key, quote_request_hash,
            netgroup_bucket, effective_now, error)) {
        return rejected(std::move(error));
    }
    HashWriter simulated_commit = TaggedHash(
        "DigiByte Paymaster Capacity Continuation Preflight v1");
    simulated_commit << request_key << capacity_request_hash
                     << quote_request_hash;
    if (!PromoteProviderCapacityAdmission(
            candidate, request_key, quote_request_hash,
            simulated_commit.GetSHA256(), effective_now, error)) {
        return rejected(std::move(error));
    }
    return EvaluateProviderSafetyStatus(
        candidate, policy, quote_request.intent.funding_model,
        quote_request.intent.sponsorship_scope, recipient_bucket,
        proposed_network_fee, effective_now, netgroup_bucket);
}

bool ReserveProviderBudget(ProviderBudgetLedger& ledger,
                           const ProviderSafetyPolicy& policy,
                           FundingModel model,
                           SponsorshipScope scope,
                           const uint256& commit_key,
                           DGBSatoshis network_fee,
                           const uint256& recipient_bucket,
                           int64_t now,
                           std::string& error,
                           const uint256& netgroup_bucket)
{
    error.clear();
    std::string ledger_error;
    if (!ValidateProviderBudgetLedger(ledger, ledger_error)) {
        error = ledger_error;
        return false;
    }
    if (!ValidBudgetClass(model, scope)) {
        error = "PAYMASTER_INVALID_FUNDING_MODEL_OR_SCOPE";
        return false;
    }
    if (commit_key.IsNull() || netgroup_bucket.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_BUDGET_RESERVATION";
        return false;
    }
    for (const ProviderBudgetReservation& reservation : ledger.reservations) {
        if (reservation.commit_key != commit_key) continue;
        if (reservation.funding_model == model && reservation.sponsorship_scope == scope &&
            reservation.network_fee == network_fee &&
            reservation.recipient_bucket == recipient_bucket &&
            reservation.netgroup_bucket == netgroup_bucket &&
            reservation.state != BudgetReservationState::RELEASED) return true;
        error = "PAYMASTER_BUDGET_RESERVATION_CONFLICT";
        return false;
    }
    const ProviderSafetyStatus status = EvaluateProviderSafetyStatus(
        ledger, policy, model, scope, recipient_bucket, network_fee, now,
        netgroup_bucket);
    if (!status.can_accept_quote) {
        error = status.errors.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : status.errors.front();
        return false;
    }
    const int64_t effective_now = EffectiveAccountingTime(now, ledger.accounting_time_high_water);
    const size_t retained_reservations = std::count_if(
        ledger.reservations.begin(), ledger.reservations.end(),
        [effective_now](const ProviderBudgetReservation& reservation) {
            return reservation.state == BudgetReservationState::RESERVED ||
                   !TimeDeltaExceeds(effective_now, reservation.updated_at,
                                     SAFETY_DAY_SECONDS);
        });
    if (retained_reservations >= MAX_BUDGET_LEDGER_ENTRIES) {
        error = "PAYMASTER_BUDGET_LEDGER_FULL";
        return false;
    }
    ledger.version = ProviderBudgetLedger::CURRENT_VERSION;
    ledger.accounting_time_high_water = effective_now;
    PruneProviderLedger(ledger, effective_now);
    ProviderBudgetReservation reservation;
    reservation.commit_key = commit_key;
    reservation.funding_model = model;
    reservation.sponsorship_scope = scope;
    reservation.network_fee = network_fee;
    reservation.recipient_bucket = recipient_bucket;
    reservation.netgroup_bucket = netgroup_bucket;
    reservation.state = BudgetReservationState::RESERVED;
    reservation.reserved_at = effective_now;
    reservation.updated_at = effective_now;
    ledger.reservations.push_back(std::move(reservation));
    return true;
}

bool SpendProviderBudget(ProviderBudgetLedger& ledger,
                         const uint256& commit_key,
                         int64_t now,
                         std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (commit_key.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_BUDGET_COMMIT";
        return false;
    }
    for (ProviderBudgetReservation& reservation : ledger.reservations) {
        if (reservation.commit_key != commit_key) continue;
        if (reservation.state == BudgetReservationState::SPENT) return true;
        if (reservation.state != BudgetReservationState::RESERVED) {
            error = "PAYMASTER_BUDGET_RESERVATION_RELEASED";
            return false;
        }
        const int64_t effective_now = EffectiveAccountingTime(
            now, ledger.accounting_time_high_water);
        ledger.version = ProviderBudgetLedger::CURRENT_VERSION;
        ledger.accounting_time_high_water = effective_now;
        reservation.state = BudgetReservationState::SPENT;
        reservation.updated_at = effective_now;
        PruneProviderLedger(ledger, effective_now);
        return true;
    }
    error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
    return false;
}

bool ReleaseProviderBudget(ProviderBudgetLedger& ledger,
                           const uint256& commit_key,
                           int64_t now,
                           std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateProviderBudgetLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (commit_key.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_BUDGET_RELEASE";
        return false;
    }
    for (ProviderBudgetReservation& reservation : ledger.reservations) {
        if (reservation.commit_key != commit_key) continue;
        if (reservation.state == BudgetReservationState::RELEASED) return true;
        if (reservation.state != BudgetReservationState::RESERVED) {
            error = "PAYMASTER_BUDGET_ALREADY_SPENT";
            return false;
        }
        const int64_t effective_now = EffectiveAccountingTime(
            now, ledger.accounting_time_high_water);
        ledger.version = ProviderBudgetLedger::CURRENT_VERSION;
        ledger.accounting_time_high_water = effective_now;
        reservation.state = BudgetReservationState::RELEASED;
        reservation.updated_at = effective_now;
        PruneProviderLedger(ledger, effective_now);
        return true;
    }
    error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
    return false;
}

bool ValidateProviderBudgetReservationBinding(
    const ProviderAuthorizationManifest& manifest,
    const ProviderAttempt& attempt,
    const ProviderBudgetReservation& reservation,
    const ProviderSafetyPolicy& policy,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error)
{
    error.clear();
    if (manifest.version != ProviderAuthorizationManifest::CURRENT_VERSION ||
        manifest.manifest_id.IsNull() ||
        manifest.manifest_id != GetProviderAuthorizationManifestId(manifest)) {
        error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID";
        return false;
    }

    const uint256 expected_commit_key = GetPaymasterCommitKey(
        attempt.provider_id, attempt.client_nonce, attempt.intent_hash,
        attempt.quote_id, attempt.template_commitment);
    if (attempt.provider_manifest.manifest_id != manifest.manifest_id ||
        expected_commit_key.IsNull() ||
        attempt.commit_key != expected_commit_key ||
        reservation.commit_key != expected_commit_key ||
        reservation.funding_model != manifest.funding_model ||
        reservation.sponsorship_scope != manifest.sponsorship_scope ||
        !(reservation.network_fee == manifest.network_fee) ||
        reservation.state != expected_state ||
        manifest.safety_policy_hash.IsNull() ||
        (!allow_historical_policy &&
         manifest.safety_policy_hash != GetProviderSafetyPolicyHash(policy))) {
        error = "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH";
        return false;
    }

    if (reservation.version != ProviderBudgetReservation::CURRENT_VERSION ||
        manifest.budget_reservation_id != expected_commit_key ||
        !(manifest.maximum_network_fee == reservation.network_fee) ||
        !(manifest.maximum_network_fee == manifest.network_fee) ||
        manifest.maximum_network_fee.value <= 0 ||
        reservation.netgroup_bucket.IsNull() ||
        reservation.netgroup_bucket != attempt.provider_netgroup_bucket) {
        error = "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH";
        return false;
    }
    return true;
}

bool ReserveClientFee(ClientFeeLedger& ledger,
                      const ClientSafetyPolicy& policy,
                      const uint256& commit_key,
                      DDCents service_fee,
                      int64_t now,
                      std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateClientFeeLedger(ledger, validation_error) ||
        !ValidateClientSafetyPolicy(policy, validation_error)) {
        error = validation_error;
        return false;
    }
    if (commit_key.IsNull() || now <= 0 || service_fee.value < 0 ||
        service_fee.value > policy.maximum_service_fee_per_transaction.value) {
        error = "PAYMASTER_CLIENT_FEE_LIMIT_EXCEEDED";
        return false;
    }
    for (const ClientFeeReservation& reservation : ledger.reservations) {
        if (reservation.commit_key != commit_key) continue;
        if (reservation.service_fee == service_fee &&
            reservation.state != BudgetReservationState::RELEASED) return true;
        error = "PAYMASTER_CLIENT_FEE_RESERVATION_CONFLICT";
        return false;
    }
    const int64_t effective_now = EffectiveAccountingTime(now, ledger.accounting_time_high_water);
    int64_t projected{service_fee.value};
    for (const ClientFeeReservation& reservation : ledger.reservations) {
        if (reservation.state == BudgetReservationState::RELEASED ||
            TimeDeltaExceeds(effective_now, reservation.updated_at, SAFETY_DAY_SECONDS)) {
            continue;
        }
        if (!AddAmount(projected, reservation.service_fee.value)) {
            error = "PAYMASTER_CLIENT_FEE_LIMIT_EXCEEDED";
            return false;
        }
    }
    if (projected > policy.maximum_service_fee_per_day.value) {
        error = "PAYMASTER_CLIENT_DAILY_FEE_LIMIT_EXCEEDED";
        return false;
    }
    const size_t retained_reservations = std::count_if(
        ledger.reservations.begin(), ledger.reservations.end(),
        [effective_now](const ClientFeeReservation& reservation) {
            return reservation.state == BudgetReservationState::RESERVED ||
                   !TimeDeltaExceeds(effective_now, reservation.updated_at,
                                     SAFETY_DAY_SECONDS);
        });
    if (retained_reservations >= MAX_BUDGET_LEDGER_ENTRIES) {
        error = "PAYMASTER_CLIENT_FEE_LEDGER_FULL";
        return false;
    }
    ledger.accounting_time_high_water = effective_now;
    PruneClientLedger(ledger, effective_now);
    ledger.reservations.push_back({ClientFeeReservation::CURRENT_VERSION, commit_key,
                                   service_fee, BudgetReservationState::RESERVED,
                                   effective_now, effective_now});
    return true;
}

bool SpendClientFee(ClientFeeLedger& ledger,
                    const uint256& commit_key,
                    int64_t now,
                    std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateClientFeeLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (commit_key.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_CLIENT_FEE_COMMIT";
        return false;
    }
    for (ClientFeeReservation& reservation : ledger.reservations) {
        if (reservation.commit_key != commit_key) continue;
        if (reservation.state == BudgetReservationState::SPENT) return true;
        if (reservation.state != BudgetReservationState::RESERVED) {
            error = "PAYMASTER_CLIENT_FEE_RESERVATION_RELEASED";
            return false;
        }
        const int64_t effective_now = EffectiveAccountingTime(
            now, ledger.accounting_time_high_water);
        ledger.accounting_time_high_water = effective_now;
        reservation.state = BudgetReservationState::SPENT;
        reservation.updated_at = effective_now;
        PruneClientLedger(ledger, effective_now);
        return true;
    }
    error = "PAYMASTER_CLIENT_FEE_RESERVATION_MISSING";
    return false;
}

bool ReleaseClientFee(ClientFeeLedger& ledger,
                      const uint256& commit_key,
                      int64_t now,
                      std::string& error)
{
    error.clear();
    std::string validation_error;
    if (!ValidateClientFeeLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (commit_key.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_CLIENT_FEE_RELEASE";
        return false;
    }
    for (ClientFeeReservation& reservation : ledger.reservations) {
        if (reservation.commit_key != commit_key) continue;
        if (reservation.state == BudgetReservationState::RELEASED) return true;
        if (reservation.state != BudgetReservationState::RESERVED) {
            error = "PAYMASTER_CLIENT_FEE_ALREADY_SPENT";
            return false;
        }
        const int64_t effective_now = EffectiveAccountingTime(
            now, ledger.accounting_time_high_water);
        ledger.accounting_time_high_water = effective_now;
        reservation.state = BudgetReservationState::RELEASED;
        reservation.updated_at = effective_now;
        PruneClientLedger(ledger, effective_now);
        return true;
    }
    error = "PAYMASTER_CLIENT_FEE_RESERVATION_MISSING";
    return false;
}

bool ValidateProviderIdentityRecord(const ProviderIdentityRecord& identity)
{
    if (identity.version != ProviderIdentityRecord::CURRENT_VERSION ||
        !identity.identity_key.IsFullyValid() ||
        identity.provider_id != GetPaymasterId(identity.identity_key) ||
        !IsValidPaymasterDisplayName(identity.display_name) || identity.created_at <= 0) return false;
    TaprootBuilder builder;
    builder.Finalize(identity.identity_key);
    return identity.identity_script == GetScriptForDestination(builder.GetOutput());
}

bool ValidateProviderFinanceEvent(const ProviderFinanceEvent& event,
                                  std::string& error)
{
    error.clear();
    const bool transfer = event.kind == ProviderFinanceEventKind::TRANSFER;
    if (event.version != ProviderFinanceEvent::CURRENT_VERSION ||
        event.event_id.IsNull() || event.genesis_hash.IsNull() ||
        event.provider_id.IsNull() || event.transaction_id.IsNull() ||
        event.dd_income.value < 0 || event.dgb_cost.value < 0 ||
        event.dd_income.value > MAX_DD_OUTPUT_CENTS ||
        !MoneyRange(event.dgb_cost.value) || event.created_at <= 0 ||
        event.updated_at < event.created_at ||
        (event.state == ProviderFinanceEventState::CONFIRMED &&
         (event.confirmed_at < event.created_at ||
          event.confirmed_at > event.updated_at)) ||
        (event.state != ProviderFinanceEventState::CONFIRMED &&
         event.confirmed_at != 0) ||
        (!transfer && event.dd_income.value != 0) ||
        (transfer && event.funding_model == FundingModel::SPONSORED &&
         event.dd_income.value != 0)) {
        error = "PAYMASTER_INVALID_FINANCE_EVENT";
        return false;
    }
    return true;
}

bool RebuildProviderFinanceDailyTotals(ProviderFinanceLedger& ledger,
                                       std::string& error)
{
    error.clear();
    const auto increment = [&](uint32_t& counter) {
        if (counter == std::numeric_limits<uint32_t>::max()) {
            error = "PAYMASTER_FINANCE_TOTAL_OVERFLOW";
            return false;
        }
        ++counter;
        return true;
    };
    std::map<int64_t, ProviderFinanceDailyTotals> by_day;
    std::set<uint256> event_ids;
    for (const ProviderFinanceEvent& event : ledger.events) {
        if (!ValidateProviderFinanceEvent(event, error) ||
            event.genesis_hash != ledger.genesis_hash ||
            event.provider_id != ledger.provider_id) {
            if (error.empty()) error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
            return false;
        }
        if (!event_ids.insert(event.event_id).second) {
            error = "PAYMASTER_DUPLICATE_FINANCE_EVENT";
            return false;
        }
        if (event.state != ProviderFinanceEventState::CONFIRMED) continue;
        const int64_t day_start = (event.confirmed_at / SAFETY_DAY_SECONDS) *
                                  SAFETY_DAY_SECONDS;
        ProviderFinanceDailyTotals& total = by_day[day_start];
        total.day_start = day_start;
        if (event.dd_income.value >
                std::numeric_limits<int64_t>::max() - total.dd_income.value ||
            event.dgb_cost.value >
                std::numeric_limits<int64_t>::max() - total.dgb_cost.value) {
            error = "PAYMASTER_FINANCE_TOTAL_OVERFLOW";
            return false;
        }
        total.dd_income.value += event.dd_income.value;
        total.dgb_cost.value += event.dgb_cost.value;
        if (event.kind == ProviderFinanceEventKind::TRANSFER) {
            if (!increment(total.successful_transfers)) return false;
            if (event.funding_model == FundingModel::USER_PAID) {
                if (!increment(total.user_paid_transfers)) return false;
            } else if (event.sponsorship_scope == SponsorshipScope::PUBLIC) {
                if (!increment(total.public_sponsored_transfers)) return false;
            } else {
                if (!increment(total.restricted_sponsored_transfers)) return false;
            }
        } else {
            if (!increment(total.maintenance_transactions)) return false;
        }
    }
    ledger.daily_totals.clear();
    ledger.daily_totals.reserve(by_day.size());
    for (auto& [day, totals] : by_day) {
        (void)day;
        ledger.daily_totals.push_back(std::move(totals));
    }
    return true;
}

bool ValidateProviderFinanceLedger(const ProviderFinanceLedger& ledger,
                                   std::string& error)
{
    error.clear();
    if (ledger.version != ProviderFinanceLedger::CURRENT_VERSION ||
        ledger.genesis_hash.IsNull() || ledger.provider_id.IsNull() ||
        ledger.history_complete_from <= 0 ||
        ledger.updated_at < ledger.history_complete_from ||
        ledger.events.size() > MAX_PROVIDER_FINANCE_EVENTS) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (std::any_of(ledger.events.begin(), ledger.events.end(),
                    [&](const ProviderFinanceEvent& event) {
                        return event.updated_at > ledger.updated_at;
                    })) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER_TIME";
        return false;
    }
    ProviderFinanceLedger rebuilt{ledger};
    if (!RebuildProviderFinanceDailyTotals(rebuilt, error) ||
        rebuilt.daily_totals.size() != ledger.daily_totals.size()) {
        if (error.empty()) error = "PAYMASTER_INVALID_FINANCE_TOTALS";
        return false;
    }
    for (size_t index = 0; index < ledger.daily_totals.size(); ++index) {
        const ProviderFinanceDailyTotals& stored = ledger.daily_totals[index];
        const ProviderFinanceDailyTotals& expected = rebuilt.daily_totals[index];
        if (stored.version != ProviderFinanceDailyTotals::CURRENT_VERSION ||
            stored.day_start != expected.day_start ||
            stored.dd_income.value != expected.dd_income.value ||
            stored.dgb_cost.value != expected.dgb_cost.value ||
            stored.successful_transfers != expected.successful_transfers ||
            stored.user_paid_transfers != expected.user_paid_transfers ||
            stored.public_sponsored_transfers != expected.public_sponsored_transfers ||
            stored.restricted_sponsored_transfers != expected.restricted_sponsored_transfers ||
            stored.maintenance_transactions != expected.maintenance_transactions) {
            error = "PAYMASTER_INVALID_FINANCE_TOTALS";
            return false;
        }
    }
    return true;
}

bool UpsertProviderFinanceEvent(ProviderFinanceLedger& ledger,
                                const ProviderFinanceEvent& event,
                                std::string& error)
{
    if (!ValidateProviderFinanceEvent(event, error) ||
        event.genesis_hash != ledger.genesis_hash ||
        event.provider_id != ledger.provider_id) {
        if (error.empty()) error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }
    auto existing = std::find_if(
        ledger.events.begin(), ledger.events.end(),
        [&](const ProviderFinanceEvent& candidate) {
            return candidate.event_id == event.event_id;
        });
    if (existing == ledger.events.end()) {
        if (ledger.events.size() >= MAX_PROVIDER_FINANCE_EVENTS) {
            error = "PAYMASTER_FINANCE_LEDGER_FULL";
            return false;
        }
        ledger.events.push_back(event);
    } else {
        const bool same_economic_event =
            existing->genesis_hash == event.genesis_hash &&
            existing->provider_id == event.provider_id &&
            existing->kind == event.kind &&
            existing->funding_model == event.funding_model &&
            existing->sponsorship_scope == event.sponsorship_scope &&
            existing->transaction_id == event.transaction_id &&
            existing->dd_income == event.dd_income &&
            existing->dgb_cost == event.dgb_cost &&
            existing->created_at == event.created_at;
        if (!same_economic_event) {
            error = "PAYMASTER_FINANCE_EVENT_CONFLICT";
            return false;
        }
        // A delayed replay can legitimately carry an older pending or
        // confirmed snapshot. Treat it as an idempotent no-op instead of
        // letting it roll back the more recent chain-derived state.
        if (event.updated_at < existing->updated_at) return true;
        if (event.updated_at == existing->updated_at &&
            (event.state != existing->state ||
             event.confirmed_at != existing->confirmed_at)) {
            error = "PAYMASTER_FINANCE_EVENT_STATE_CONFLICT";
            return false;
        }
        *existing = event;
    }
    ledger.updated_at = std::max(ledger.updated_at, event.updated_at);
    return RebuildProviderFinanceDailyTotals(ledger, error);
}

bool ValidateProviderBackupStatus(const ProviderBackupStatus& status,
                                  std::string& error)
{
    error.clear();
    if (status.version != ProviderBackupStatus::CURRENT_VERSION ||
        status.genesis_hash.IsNull() || status.provider_id.IsNull() ||
        status.identity_created_at <= 0 ||
        status.reminder_updated_at < status.identity_created_at ||
        status.last_successful_backup_at < 0 ||
        status.external_backup_acknowledged_at < 0) {
        error = "PAYMASTER_INVALID_BACKUP_STATUS";
        return false;
    }
    return true;
}

bool ProviderBackupRequired(const ProviderBackupStatus& status)
{
    std::string error;
    if (!ValidateProviderBackupStatus(status, error)) return true;
    return std::max(status.last_successful_backup_at,
                    status.external_backup_acknowledged_at) <
           status.reminder_updated_at;
}

bool IsActiveProviderPoolState(PoolEntryState state)
{
    return state == PoolEntryState::AVAILABLE ||
           state == PoolEntryState::RESERVED ||
           state == PoolEntryState::PENDING_SUCCESSOR ||
           state == PoolEntryState::COMMITTED;
}

bool ValidateProviderLiquidityPolicy(const ProviderLiquidityPolicy& policy,
                                     std::string& error)
{
    error.clear();
    const bool carrier_targets_valid =
        (policy.target_admission_carriers == 0
             ? policy.target_operational_carriers == 0
             : policy.target_admission_carriers >= 3) &&
        policy.target_operational_carriers <= 16;
    const bool finite_limits =
        policy.maximum_maintenance_fee_per_transaction.value > 0 &&
        policy.maximum_maintenance_fee_per_hour.value >=
            policy.maximum_maintenance_fee_per_transaction.value &&
        policy.maximum_maintenance_fee_per_day.value >=
            policy.maximum_maintenance_fee_per_hour.value;
    if (policy.version != ProviderLiquidityPolicy::CURRENT_VERSION ||
        policy.updated_at <= 0 ||
        policy.target_admission_dgb < 3 ||
        policy.target_operational_dgb < 1 ||
        policy.target_admission_dgb > 16 ||
        policy.target_operational_dgb > 16 ||
        policy.target_admission_carriers > 16 ||
        policy.target_operational_carriers > 16 ||
        !carrier_targets_valid ||
        !MoneyRange(policy.maximum_maintenance_fee_per_transaction.value) ||
        !MoneyRange(policy.maximum_maintenance_fee_per_hour.value) ||
        !MoneyRange(policy.maximum_maintenance_fee_per_day.value) ||
        (policy.paid_maintenance_approved && !finite_limits)) {
        error = "PAYMASTER_INVALID_LIQUIDITY_POLICY";
        return false;
    }
    return true;
}

bool ProviderLiquidityTargetsSatisfyPolicy(
    const ProviderLiquidityPolicy& liquidity_policy,
    const ProviderPolicy& provider_policy)
{
    std::string error;
    if (!ValidateProviderLiquidityPolicy(liquidity_policy, error) ||
        !ValidateProviderPolicy(provider_policy, error)) {
        return false;
    }

    // The generic liquidity-policy format permits zero carrier targets so an
    // operator can deliberately release the last carrier while retaining a
    // USER_PAID offer for later use. That is a valid stored state, but it
    // cannot restore provider readiness until the operator raises the targets
    // again. Keep this policy-dependent invariant separate from serialization
    // validation so sponsored-only providers still require no DD carriers.
    if (PolicyAllowsFundingModel(provider_policy, FundingModel::USER_PAID)) {
        return liquidity_policy.target_admission_carriers >=
                   REQUIRED_ADMISSION_SLOTS &&
               liquidity_policy.target_operational_carriers >= 1;
    }
    return true;
}

bool ValidateProviderCarrierWithdrawalPlan(
    const ProviderCarrierWithdrawalPlan& plan,
    std::string& error)
{
    error.clear();
    if (plan.version != ProviderCarrierWithdrawalPlan::CURRENT_VERSION ||
        plan.plan_id.IsNull() || plan.operation_id.IsNull() ||
        plan.source_carriers.empty() || plan.source_carriers.size() > 64 ||
        plan.liquidity_policy_updated_at <= 0 || plan.created_at <= 0 ||
        plan.expires_at <= plan.created_at ||
        plan.expires_at - plan.created_at > 15 * 60) {
        error = "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN";
        return false;
    }
    std::set<COutPoint> sources;
    for (const COutPoint& source : plan.source_carriers) {
        if (source.IsNull() || !sources.insert(source).second) {
            error = "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN";
            return false;
        }
    }
    if (plan.mode == CarrierWithdrawalMode::RELEASE_SLOT) {
        if (plan.source_carriers.size() != 1 ||
            !plan.replacement_carriers.empty() ||
            !plan.excess_script_pub_key.empty() ||
            plan.excess_amount.value != 0 || plan.estimated_fee.value != 0 ||
            plan.target_operational_carriers_after_release > 15) {
            error = "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN";
            return false;
        }
        return true;
    }
    if (plan.mode != CarrierWithdrawalMode::ALL_EXCESS ||
        plan.replacement_carriers.size() != plan.source_carriers.size() ||
        plan.excess_amount.value <= 0 ||
        plan.excess_amount.value > MAX_DD_OUTPUT_CENTS ||
        plan.estimated_fee.value <= 0 ||
        !MoneyRange(plan.estimated_fee.value) ||
        plan.target_operational_carriers_after_release != 0) {
        error = "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN";
        return false;
    }
    int witness_version{-1};
    std::vector<unsigned char> witness_program;
    if (!plan.excess_script_pub_key.IsWitnessProgram(
            witness_version, witness_program) ||
        witness_version != 1 || witness_program.size() != 32 ||
        !XOnlyPubKey{witness_program}.IsFullyValid()) {
        error = "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN";
        return false;
    }
    std::set<CScript> scripts{plan.excess_script_pub_key};
    for (const ProviderMaintenanceOutput& output :
         plan.replacement_carriers) {
        witness_version = -1;
        witness_program.clear();
        if ((output.purpose != PoolPurpose::ADMISSION &&
             output.purpose != PoolPurpose::OPERATIONAL) ||
            output.asset != PoolAsset::DD_CARRIER ||
            output.carrier_value.value != 100 ||
            output.dgb_value.value != 0 ||
            !scripts.insert(output.script_pub_key).second ||
            !output.script_pub_key.IsWitnessProgram(
                witness_version, witness_program) ||
            witness_version != 1 || witness_program.size() != 32 ||
            !XOnlyPubKey{witness_program}.IsFullyValid()) {
            error = "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN";
            return false;
        }
    }
    return true;
}

bool ValidateProviderMaintenanceLedger(const ProviderMaintenanceLedger& ledger,
                                       std::string& error)
{
    error.clear();
    if (ledger.version != ProviderMaintenanceLedger::CURRENT_VERSION ||
        ledger.accounting_time_high_water < 0 || ledger.records.size() > 256) {
        error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER";
        return false;
    }
    std::set<uint256> operation_ids;
    for (const ProviderMaintenanceRecord& record : ledger.records) {
        const bool valid_kind =
            record.kind == ProviderMaintenanceKind::REPLENISH_DGB ||
            record.kind == ProviderMaintenanceKind::REPLENISH_CARRIER ||
            record.kind == ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS ||
            record.IsPreparation();
        const bool valid_state =
            record.state == ProviderMaintenanceState::PLANNED ||
            record.state == ProviderMaintenanceState::BROADCAST ||
            record.state == ProviderMaintenanceState::CONFIRMED ||
            record.state == ProviderMaintenanceState::RELEASED ||
            record.state == ProviderMaintenanceState::FAILED;
        const bool current_record =
            (record.version == 3 || record.version == ProviderMaintenanceRecord::CURRENT_VERSION);
        if (!current_record ||
            !valid_kind || !valid_state ||
            record.operation_id.IsNull() || record.plan_id.IsNull() ||
            !operation_ids.insert(record.operation_id).second ||
            record.outputs.empty() || record.outputs.size() > 64 ||
            record.maximum_fee.value <= 0 ||
            !MoneyRange(record.maximum_fee.value) ||
            !MoneyRange(record.actual_fee.value) ||
            record.actual_fee.value > record.maximum_fee.value ||
            record.created_at <= 0 || record.updated_at < record.created_at ||
            ((record.state == ProviderMaintenanceState::BROADCAST ||
              record.state == ProviderMaintenanceState::CONFIRMED) &&
             record.transaction_id.IsNull()) ||
            ((record.state == ProviderMaintenanceState::PLANNED ||
              record.state == ProviderMaintenanceState::RELEASED) &&
             !record.transaction_id.IsNull())) {
            error = "PAYMASTER_INVALID_MAINTENANCE_RECORD";
            return false;
        }
        if ((record.IsPreparation() &&
             (record.version < 4 || record.preparation_authorization.IsNull() || record.preparation_request.IsNull())) ||
            (!record.IsPreparation() &&
             (!record.preparation_authorization.IsNull() || !record.preparation_request.IsNull() || !record.preparation_error.empty())) ||
            record.preparation_error.size() > 256) {
            error = "PAYMASTER_INVALID_PREPARATION_AUTHORIZATION";
            return false;
        }
        std::set<COutPoint> source_inputs;
        if (record.source_inputs.size() > 64 ||
            std::any_of(record.source_inputs.begin(),
                        record.source_inputs.end(),
                        [&](const COutPoint& source) {
                            return source.IsNull() ||
                                   !source_inputs.insert(source).second;
                        }) ||
            (record.kind ==
                     ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS
                 ? record.source_inputs.empty()
                 : !record.source_inputs.empty())) {
            error = "PAYMASTER_INVALID_MAINTENANCE_SOURCE_INPUTS";
            return false;
        }
        if (current_record) {
            const bool withdrawal =
                record.kind ==
                ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS;
            int witness_version{-1};
            std::vector<unsigned char> witness_program;
            const bool valid_excess =
                record.withdrawal_excess_amount.value > 0 &&
                record.withdrawal_excess_amount.value <=
                    MAX_DD_OUTPUT_CENTS &&
                record.withdrawal_excess_script_pub_key.IsWitnessProgram(
                    witness_version, witness_program) &&
                witness_version == 1 && witness_program.size() == 32 &&
                XOnlyPubKey{witness_program}.IsFullyValid();
            if ((withdrawal && !valid_excess) ||
                (!withdrawal &&
                 (!record.withdrawal_excess_script_pub_key.empty() ||
                  record.withdrawal_excess_amount.value != 0))) {
                error = "PAYMASTER_INVALID_MAINTENANCE_EXCESS_OUTPUT";
                return false;
            }
        }
        std::set<CScript> scripts;
        if (current_record &&
            record.kind ==
                ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS) {
            scripts.insert(record.withdrawal_excess_script_pub_key);
        }
        int64_t total_dgb{0};
        for (const ProviderMaintenanceOutput& output : record.outputs) {
            int witness_version{-1};
            std::vector<unsigned char> witness_program;
            if (!AddAmount(total_dgb, output.dgb_value.value) || !MoneyRange(total_dgb) ||
                (record.kind == ProviderMaintenanceKind::PREPARE_DGB && output.asset != PoolAsset::DGB) ||
                (record.kind == ProviderMaintenanceKind::PREPARE_CARRIER && output.asset != PoolAsset::DD_CARRIER) ||
                (output.purpose != PoolPurpose::ADMISSION &&
                 output.purpose != PoolPurpose::OPERATIONAL) ||
                (output.asset != PoolAsset::DGB &&
                 output.asset != PoolAsset::DD_CARRIER) ||
                !scripts.insert(output.script_pub_key).second ||
                !output.script_pub_key.IsWitnessProgram(
                    witness_version, witness_program) ||
                witness_version != 1 || witness_program.size() != 32 ||
                !XOnlyPubKey{witness_program}.IsFullyValid() ||
                (output.asset == PoolAsset::DGB
                     ? !MoneyRange(output.dgb_value.value) ||
                           output.dgb_value.value == 0 ||
                           output.carrier_value.value != 0
                     : output.dgb_value.value != 0 ||
                           output.carrier_value.value < 100 ||
                           output.carrier_value.value >
                               MAX_DD_OUTPUT_CENTS)) {
                error = "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
                return false;
            }
        }
    }
    return true;
}

bool ReserveProviderMaintenanceBudget(ProviderMaintenanceLedger& ledger,
                                      const ProviderLiquidityPolicy& policy,
                                      ProviderMaintenanceRecord record,
                                      int64_t now,
                                      std::string& error)
{
    if (!ValidateProviderLiquidityPolicy(policy, error) ||
        !ValidateProviderMaintenanceLedger(ledger, error)) {
        return false;
    }
    if (record.IsPreparation() || !policy.paid_maintenance_approved ||
        record.version != ProviderMaintenanceRecord::CURRENT_VERSION ||
        record.maximum_fee.value <= 0 ||
        record.maximum_fee.value >
            policy.maximum_maintenance_fee_per_transaction.value ||
        record.state != ProviderMaintenanceState::PLANNED ||
        !record.transaction_id.IsNull() || record.actual_fee.value != 0 ||
        now <= 0) {
        error = policy.paid_maintenance_approved
            ? "PAYMASTER_INVALID_MAINTENANCE_RESERVATION"
            : "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
        return false;
    }
    const auto existing = std::find_if(
        ledger.records.begin(), ledger.records.end(),
        [&](const ProviderMaintenanceRecord& candidate) {
            return candidate.operation_id == record.operation_id;
        });
    if (existing != ledger.records.end()) {
        if (existing->plan_id == record.plan_id &&
            existing->kind == record.kind &&
            existing->outputs == record.outputs &&
            existing->source_inputs == record.source_inputs &&
            existing->withdrawal_excess_script_pub_key ==
                record.withdrawal_excess_script_pub_key &&
            existing->withdrawal_excess_amount ==
                record.withdrawal_excess_amount &&
            existing->maximum_fee == record.maximum_fee) {
            return true;
        }
        error = "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT";
        return false;
    }
    const int64_t effective_now = EffectiveAccountingTime(
        now, ledger.accounting_time_high_water);
    record.created_at = effective_now;
    record.updated_at = effective_now;
    ProviderMaintenanceLedger shape_check;
    shape_check.accounting_time_high_water = effective_now;
    shape_check.records.push_back(record);
    if (!ValidateProviderMaintenanceLedger(shape_check, error)) {
        return false;
    }
    int64_t hour_total{record.maximum_fee.value};
    int64_t day_total{record.maximum_fee.value};
    for (const ProviderMaintenanceRecord& candidate : ledger.records) {
        if (candidate.IsPreparation()) continue;
        if (candidate.state == ProviderMaintenanceState::RELEASED ||
            candidate.state == ProviderMaintenanceState::FAILED) continue;
        const bool outstanding =
            candidate.state == ProviderMaintenanceState::PLANNED ||
            candidate.state == ProviderMaintenanceState::BROADCAST;
        const int64_t value =
            candidate.state == ProviderMaintenanceState::PLANNED
                ? candidate.maximum_fee.value
                : candidate.actual_fee.value;
        // Planned work and a live broadcast remain worst-case exposure until
        // they become terminal, regardless of age. Only confirmed spend ages
        // out of the rolling hour/day windows.
        const bool counts_hour = outstanding ||
            (candidate.state == ProviderMaintenanceState::CONFIRMED &&
             candidate.updated_at >= effective_now - 60 * 60);
        const bool counts_day = outstanding ||
            (candidate.state == ProviderMaintenanceState::CONFIRMED &&
             candidate.updated_at >= effective_now - 24 * 60 * 60);
        if (counts_hour && !AddAmount(hour_total, value)) {
            error = "PAYMASTER_MAINTENANCE_BUDGET_OVERFLOW";
            return false;
        }
        if (counts_day && !AddAmount(day_total, value)) {
            error = "PAYMASTER_MAINTENANCE_BUDGET_OVERFLOW";
            return false;
        }
    }
    if (hour_total > policy.maximum_maintenance_fee_per_hour.value ||
        day_total > policy.maximum_maintenance_fee_per_day.value) {
        error = "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED";
        return false;
    }
    ProviderMaintenanceLedger updated{ledger};
    updated.accounting_time_high_water = effective_now;
    PruneProviderMaintenanceLedger(updated, effective_now);
    updated.records.push_back(std::move(record));
    if (!ValidateProviderMaintenanceLedger(updated, error)) return false;
    ledger = std::move(updated);
    return true;
}

bool SpendProviderMaintenanceBudget(ProviderMaintenanceLedger& ledger,
                                    const uint256& operation_id,
                                    const uint256& transaction_id,
                                    DGBSatoshis actual_fee,
                                    int64_t now,
                                    std::string& error)
{
    if (!ValidateProviderMaintenanceLedger(ledger, error) ||
        operation_id.IsNull() || transaction_id.IsNull() ||
        !MoneyRange(actual_fee.value) || now <= 0) {
        if (error.empty()) error = "PAYMASTER_INVALID_MAINTENANCE_SPEND";
        return false;
    }
    for (ProviderMaintenanceRecord& record : ledger.records) {
        if (record.operation_id != operation_id) continue;
        if (record.state == ProviderMaintenanceState::BROADCAST ||
            record.state == ProviderMaintenanceState::CONFIRMED) {
            if (record.transaction_id == transaction_id &&
                record.actual_fee == actual_fee) return true;
            error = "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT";
            return false;
        }
        if (record.state != ProviderMaintenanceState::PLANNED ||
            actual_fee.value > record.maximum_fee.value) {
            error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
            return false;
        }
        const int64_t effective_now = EffectiveAccountingTime(
            now, ledger.accounting_time_high_water);
        ledger.accounting_time_high_water = effective_now;
        record.transaction_id = transaction_id;
        record.actual_fee = actual_fee;
        record.state = ProviderMaintenanceState::BROADCAST;
        record.updated_at = effective_now;
        return true;
    }
    error = "PAYMASTER_MAINTENANCE_RESERVATION_MISSING";
    return false;
}

bool ReleaseProviderMaintenanceBudget(ProviderMaintenanceLedger& ledger,
                                      const uint256& operation_id,
                                      int64_t now,
                                      std::string& error)
{
    if (!ValidateProviderMaintenanceLedger(ledger, error) ||
        operation_id.IsNull() || now <= 0) {
        if (error.empty()) error = "PAYMASTER_INVALID_MAINTENANCE_RELEASE";
        return false;
    }
    for (ProviderMaintenanceRecord& record : ledger.records) {
        if (record.operation_id != operation_id) continue;
        if (record.state == ProviderMaintenanceState::RELEASED) return true;
        if (record.state != ProviderMaintenanceState::PLANNED) {
            error = "PAYMASTER_MAINTENANCE_ALREADY_BROADCAST";
            return false;
        }
        const int64_t effective_now = EffectiveAccountingTime(
            now, ledger.accounting_time_high_water);
        ledger.accounting_time_high_water = effective_now;
        record.state = ProviderMaintenanceState::RELEASED;
        record.updated_at = effective_now;
        return true;
    }
    error = "PAYMASTER_MAINTENANCE_RESERVATION_MISSING";
    return false;
}

bool ValidateProviderPoolEntries(const std::vector<ProviderPoolEntry>& entries,
                                 std::string& error)
{
    error.clear();
    if (entries.size() > 256) {
        error = "PAYMASTER_POOL_TOO_LARGE";
        return false;
    }
    std::set<COutPoint> outpoints;
    for (const ProviderPoolEntry& entry : entries) {
        int witness_version{-1};
        std::vector<unsigned char> witness_program;
        if (entry.version != ProviderPoolEntry::CURRENT_VERSION ||
            (entry.purpose != PoolPurpose::ADMISSION && entry.purpose != PoolPurpose::OPERATIONAL) ||
            (entry.asset != PoolAsset::DGB && entry.asset != PoolAsset::DD_CARRIER) ||
            (entry.state != PoolEntryState::AVAILABLE && entry.state != PoolEntryState::RESERVED &&
             entry.state != PoolEntryState::PENDING_SUCCESSOR && entry.state != PoolEntryState::SPENT &&
             entry.state != PoolEntryState::COMMITTED && entry.state != PoolEntryState::RELEASED &&
             entry.state != PoolEntryState::INVALIDATED) ||
            entry.outpoint.IsNull() ||
            !outpoints.insert(entry.outpoint).second || entry.updated_at <= 0 ||
            !entry.script_pub_key.IsWitnessProgram(witness_version, witness_program) ||
            witness_version != 1 || witness_program.size() != 32 ||
            !XOnlyPubKey{witness_program}.IsFullyValid()) {
            error = "PAYMASTER_INVALID_POOL_ENTRY";
            return false;
        }
        const bool requires_binding = entry.state == PoolEntryState::RESERVED ||
                                      entry.state == PoolEntryState::PENDING_SUCCESSOR ||
                                      entry.state == PoolEntryState::COMMITTED;
        if (requires_binding != !entry.reservation_id.IsNull()) {
            error = "PAYMASTER_INVALID_POOL_RESERVATION";
            return false;
        }
        const bool successor_state = entry.state == PoolEntryState::PENDING_SUCCESSOR;
        if ((successor_state && entry.origin_commit_key.IsNull()) ||
            (successor_state && entry.reservation_id != entry.origin_commit_key)) {
            error = "PAYMASTER_INVALID_POOL_PROVENANCE";
            return false;
        }
        if (entry.asset == PoolAsset::DGB) {
            if (!MoneyRange(entry.dgb_value.value) ||
                entry.dgb_value.value == 0 ||
                entry.carrier_value.value != 0 ||
                (entry.purpose == PoolPurpose::ADMISSION &&
                 entry.dgb_value.value < MIN_ADMISSION_DGB_SATOSHIS)) {
                error = "PAYMASTER_INVALID_POOL_DGB";
                return false;
            }
        } else if (entry.dgb_value.value != 0 || entry.carrier_value.value < 100 ||
                   entry.carrier_value.value > MAX_DD_OUTPUT_CENTS) {
            error = "PAYMASTER_INVALID_POOL_CARRIER";
            return false;
        }
    }
    return true;
}

ProviderPoolReadiness EvaluateProviderPoolReadiness(
    const std::vector<ProviderPoolEntry>& entries,
    const ProviderPolicy& policy,
    int32_t tip_height,
    int32_t minimum_confirmations)
{
    ProviderPoolReadiness result;
    std::string error;
    if (!ValidateProviderPolicy(policy, error)) {
        result.errors.push_back(error);
        return result;
    }
    if (!ValidateProviderPoolEntries(entries, error)) {
        result.errors.push_back(error);
        return result;
    }
    if (tip_height < 0 || minimum_confirmations <= 0) {
        result.errors.push_back("PAYMASTER_INVALID_CONFIRMATION_POLICY");
        return result;
    }
    for (const ProviderPoolEntry& entry : entries) {
        if (entry.state != PoolEntryState::AVAILABLE || entry.confirmation_height <= 0 ||
            tip_height - entry.confirmation_height + 1 < minimum_confirmations) continue;
        if (entry.purpose == PoolPurpose::ADMISSION && entry.asset == PoolAsset::DGB) {
            ++result.admission_dgb;
        } else if (entry.purpose == PoolPurpose::ADMISSION) {
            ++result.admission_carriers;
        } else if (entry.asset == PoolAsset::DGB &&
                   entry.dgb_value.value >= policy.maximum_network_fee.value) {
            ++result.operational_dgb;
        } else if (entry.purpose == PoolPurpose::OPERATIONAL && entry.asset == PoolAsset::DD_CARRIER) {
            ++result.operational_carriers;
        }
    }
    const bool user_paid = PolicyAllowsFundingModel(policy, FundingModel::USER_PAID);
    result.complete_operational_slots = user_paid ? std::min(result.operational_dgb, result.operational_carriers) : result.operational_dgb;
    if (result.admission_dgb < REQUIRED_ADMISSION_SLOTS) {
        result.errors.push_back("PAYMASTER_ADMISSION_DGB_MISSING");
    }
    if (user_paid && result.admission_carriers < REQUIRED_ADMISSION_SLOTS) {
        result.errors.push_back("PAYMASTER_ADMISSION_CARRIERS_MISSING");
    }
    if (result.complete_operational_slots == 0) {
        result.errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    }
    result.ready = result.errors.empty();
    return result;
}

uint256 GetProviderPolicyHash(const ProviderPolicy& policy)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Policy v1");
    hasher << policy;
    return hasher.GetSHA256();
}

uint256 GetProviderSafetyPolicyHash(const ProviderSafetyPolicy& policy)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Safety Policy v1");
    hasher << policy;
    return hasher.GetSHA256();
}

std::optional<ServiceFeePlan> EvaluateServiceFee(const ProviderPolicy& policy,
                                                 FundingModel model,
                                                 DDCents payment,
                                                 std::string& error)
{
    if (!ValidateProviderPolicy(policy, error)) return std::nullopt;
    if (!PolicyAllowsFundingModel(policy, model)) {
        error = "PAYMASTER_FUNDING_MODEL_NOT_ALLOWED";
        return std::nullopt;
    }
    if (payment.value < policy.min_payment.value || payment.value > policy.max_payment.value) {
        error = "PAYMASTER_PAYMENT_OUTSIDE_POLICY";
        return std::nullopt;
    }

    DDCents fee{0};
    if (model == FundingModel::USER_PAID) {
        const auto computed = ComputePaymasterFee(payment, policy.fee_rate_bps);
        if (!computed) {
            error = "PAYMASTER_INVALID_SERVICE_FEE";
            return std::nullopt;
        }
        fee = *computed;
    }
    ServiceFeePlan result;
    result.payment = payment;
    result.service_fee = fee;
    result.total_user_charge = DDCents{payment.value + fee.value};
    if (fee.value == 0)
        result.output_kind = FeeOutputKind::NONE;
    else if (fee.value < 100)
        result.output_kind = FeeOutputKind::CARRIER_SUCCESSOR;
    else
        result.output_kind = FeeOutputKind::PROVIDER_OUTPUT;
    error.clear();
    return result;
}

std::optional<DDCents> ComputeCarrierSuccessor(DDCents carrier,
                                               DDCents service_fee,
                                               std::string& error)
{
    if (carrier.value < 100 || carrier.value > MAX_DD_OUTPUT_CENTS ||
        service_fee.value < 1 || service_fee.value >= 100 ||
        carrier.value > MAX_DD_OUTPUT_CENTS - service_fee.value) {
        error = "PAYMASTER_INVALID_CARRIER_SUCCESSOR";
        return std::nullopt;
    }
    error.clear();
    return DDCents{carrier.value + service_fee.value};
}

} // namespace DigiDollar::Paymaster
