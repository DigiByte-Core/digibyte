// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider policy, finite-budget, pool, and service-state invariants. */

#include <boost/test/unit_test.hpp>

#include <consensus/amount.h>
#include <hash.h>
#include <key.h>
#include <paymaster/directory.h>
#include <paymaster/provider.h>
#include <paymaster/wire.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>

#include <limits>

using namespace DigiDollar::Paymaster;

BOOST_FIXTURE_TEST_SUITE(paymaster_provider_tests, BasicTestingSetup)

ProviderPolicy UserPaidPolicy(uint32_t rate_bps)
{
    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_ALL;
    policy.fee_rate_bps = rate_bps;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{1000000};
    policy.maximum_network_fee = DGBSatoshis{20000000};
    return policy;
}

ProviderPoolEntry PoolEntry(uint32_t index,
                            PoolPurpose purpose,
                            PoolAsset asset,
                            int64_t value)
{
    CKey key;
    key.MakeNewKey(true);
    TaprootBuilder builder;
    builder.Finalize(XOnlyPubKey{key.GetPubKey()});
    ProviderPoolEntry entry;
    entry.outpoint = COutPoint{uint256::ONE, index};
    entry.purpose = purpose;
    entry.asset = asset;
    entry.script_pub_key = GetScriptForDestination(builder.GetOutput());
    if (asset == PoolAsset::DGB)
        entry.dgb_value = DGBSatoshis{value};
    else
        entry.carrier_value = DDCents{value};
    entry.confirmation_height = 90;
    entry.updated_at = 1;
    return entry;
}

template <typename T>
uint256 SerializedPaymasterHash(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    return Hash(MakeUCharSpan(stream));
}

uint256 RedactedQuoteRequestHash(PaymasterQuoteRequest request)
{
    request.restricted_descriptor.reset();
    request.restricted_capability.reset();
    return SerializedPaymasterHash(request);
}

struct CapacityContinuationFixture {
    PaymasterCapacityRequest capacity_request;
    PaymasterQuoteRequest quote_request;
};

CapacityContinuationFixture CapacityContinuation(
    int64_t now,
    FundingModel model = FundingModel::USER_PAID,
    bool requires_carrier = true)
{
    CapacityContinuationFixture fixture;
    PaymasterCapacityRequest& capacity = fixture.capacity_request;
    capacity.genesis_hash = uint256S("10");
    capacity.provider_id = uint256S("11");
    capacity.request_id = "550e8400-e29b-41d4-a716-446655440001";
    capacity.session_id = uint256S("12");
    capacity.client_nonce = uint256S("13");
    capacity.funding_model = model;
    capacity.requires_carrier = requires_carrier;
    capacity.created_at = now - 1;
    capacity.expires_at = now + 59;

    PaymentIntent& intent = fixture.quote_request.intent;
    intent.genesis_hash = capacity.genesis_hash;
    intent.provider_id = capacity.provider_id;
    intent.request_id = capacity.request_id;
    intent.session_id = capacity.session_id;
    intent.client_nonce = capacity.client_nonce;
    intent.user_dd_inputs = {COutPoint{uint256S("14"), 0}};
    intent.recipient_script =
        PoolEntry(99, PoolPurpose::OPERATIONAL, PoolAsset::DGB, 1)
            .script_pub_key;
    intent.recipient_amount = DDCents{100};
    intent.offer_id = uint256S("15");
    intent.funding_model = model;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = uint256S("16");
    intent.expires_at = capacity.expires_at;
    intent.user_input_proofs = {
        {intent.user_dd_inputs.front(), std::vector<unsigned char>(64, 1)}};
    intent.canonical_request_hash = uint256S("17");
    return fixture;
}

FundingSafetyLimits SafetyLimits(int64_t per_transaction = 100,
                                 int64_t reserved = 1000,
                                 int64_t per_hour = 1000,
                                 int64_t per_day = 2000,
                                 uint32_t completed_per_hour = 10,
                                 uint32_t completed_per_day = 20)
{
    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{per_transaction};
    limits.maximum_reserved_network_fee = DGBSatoshis{reserved};
    limits.maximum_network_fee_per_hour = DGBSatoshis{per_hour};
    limits.maximum_network_fee_per_day = DGBSatoshis{per_day};
    limits.maximum_completed_per_hour = completed_per_hour;
    limits.maximum_completed_per_day = completed_per_day;
    return limits;
}

ProviderSafetyPolicy SafetyPolicy(const FundingSafetyLimits& limits)
{
    ProviderSafetyPolicy safety;
    safety.user_paid = limits;
    safety.public_sponsored = limits;
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 5;
    safety.maximum_active_quotes_per_recipient = 3;
    safety.maximum_quote_requests_per_netgroup_per_minute = 10;
    safety.updated_at = 1000;
    return safety;
}

ProviderBudgetLedger ProviderLedger(int64_t accounting_time = 1000)
{
    ProviderBudgetLedger ledger;
    ledger.recipient_bucket_secret = uint256S("01");
    ledger.accounting_time_high_water = accounting_time;
    return ledger;
}

ProviderLiquidityPolicy LiquidityPolicy(bool paid_maintenance_approved = true)
{
    ProviderLiquidityPolicy policy;
    policy.paid_maintenance_approved = paid_maintenance_approved;
    policy.maximum_maintenance_fee_per_transaction = DGBSatoshis{100};
    policy.maximum_maintenance_fee_per_hour = DGBSatoshis{150};
    policy.maximum_maintenance_fee_per_day = DGBSatoshis{250};
    policy.updated_at = 1000;
    return policy;
}

ProviderMaintenanceOutput MaintenanceOutput(
    uint32_t index,
    PoolPurpose purpose = PoolPurpose::OPERATIONAL,
    PoolAsset asset = PoolAsset::DGB,
    int64_t value = 100)
{
    const ProviderPoolEntry pool_entry{PoolEntry(index, purpose, asset, value)};
    ProviderMaintenanceOutput output;
    output.purpose = purpose;
    output.asset = asset;
    output.script_pub_key = pool_entry.script_pub_key;
    if (asset == PoolAsset::DGB) {
        output.dgb_value = DGBSatoshis{value};
    } else {
        output.carrier_value = DDCents{value};
    }
    return output;
}

ProviderMaintenanceRecord MaintenanceRecord(const uint256& operation_id,
                                             const uint256& plan_id,
                                             uint32_t output_index,
                                             int64_t maximum_fee)
{
    ProviderMaintenanceRecord record;
    record.operation_id = operation_id;
    record.plan_id = plan_id;
    record.outputs.push_back(MaintenanceOutput(output_index));
    record.maximum_fee = DGBSatoshis{maximum_fee};
    return record;
}

ClientFeeLedger ClientLedger(int64_t accounting_time = 1000)
{
    ClientFeeLedger ledger;
    ledger.accounting_time_high_water = accounting_time;
    return ledger;
}

bool ReserveTestProviderBudget(ProviderBudgetLedger& ledger,
                               const ProviderSafetyPolicy& policy,
                               FundingModel model,
                               SponsorshipScope scope,
                               const uint256& commit_key,
                               DGBSatoshis network_fee,
                               const uint256& recipient_bucket,
                               int64_t now,
                               std::string& error)
{
    return DigiDollar::Paymaster::ReserveProviderBudget(
        ledger, policy, model, scope, commit_key, network_fee,
        recipient_bucket, now, error, uint256S("f00d"));
}

BOOST_AUTO_TEST_CASE(fee_output_boundaries_are_exact)
{
    std::string error;
    auto policy = UserPaidPolicy(0);
    auto fee = EvaluateServiceFee(policy, FundingModel::USER_PAID, DDCents{100}, error);
    BOOST_REQUIRE(fee);
    BOOST_CHECK_EQUAL(fee->service_fee.value, 0);
    BOOST_CHECK(fee->output_kind == FeeOutputKind::NONE);

    policy.fee_rate_bps = 10;
    fee = EvaluateServiceFee(policy, FundingModel::USER_PAID, DDCents{100}, error);
    BOOST_REQUIRE(fee);
    BOOST_CHECK_EQUAL(fee->service_fee.value, 1);
    BOOST_CHECK(fee->output_kind == FeeOutputKind::CARRIER_SUCCESSOR);

    policy.fee_rate_bps = 100;
    fee = EvaluateServiceFee(policy, FundingModel::USER_PAID, DDCents{9900}, error);
    BOOST_REQUIRE(fee);
    BOOST_CHECK_EQUAL(fee->service_fee.value, 99);
    BOOST_CHECK(fee->output_kind == FeeOutputKind::CARRIER_SUCCESSOR);

    fee = EvaluateServiceFee(policy, FundingModel::USER_PAID, DDCents{10000}, error);
    BOOST_REQUIRE(fee);
    BOOST_CHECK_EQUAL(fee->service_fee.value, 100);
    BOOST_CHECK(fee->output_kind == FeeOutputKind::PROVIDER_OUTPUT);
}

BOOST_AUTO_TEST_CASE(sponsored_policy_always_charges_zero)
{
    std::string error;
    const auto policy = UserPaidPolicy(50);
    const auto fee = EvaluateServiceFee(policy, FundingModel::SPONSORED, DDCents{100}, error);
    BOOST_REQUIRE(fee);
    BOOST_CHECK_EQUAL(fee->service_fee.value, 0);
    BOOST_CHECK(fee->output_kind == FeeOutputKind::NONE);

    ProviderPolicy restricted = policy;
    restricted.funding_models = FUNDING_MODEL_SPONSORED;
    restricted.sponsorship_scope = SponsorshipScope::RESTRICTED;
    restricted.fee_rate_bps = 0;
    BOOST_CHECK(ValidateProviderPolicy(restricted, error));
    restricted.funding_models = FUNDING_MODEL_ALL;
    BOOST_CHECK(!ValidateProviderPolicy(restricted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RESTRICTED_POLICY");
}

BOOST_AUTO_TEST_CASE(policy_and_carrier_limits_fail_closed)
{
    std::string error;
    auto policy = UserPaidPolicy(50);
    policy.maximum_network_fee = DGBSatoshis{0};
    BOOST_CHECK(!ValidateProviderPolicy(policy, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_NETWORK_FEE_CAP");

    policy.maximum_network_fee = DGBSatoshis{MAX_MONEY + 1};
    BOOST_CHECK(!ValidateProviderPolicy(policy, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_NETWORK_FEE_CAP");

    policy = UserPaidPolicy(11);
    BOOST_CHECK(!ValidateProviderPolicy(policy, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RATE");

    policy = UserPaidPolicy(50);
    policy.sponsorship_scope = static_cast<SponsorshipScope>(255);
    BOOST_CHECK(!ValidateProviderPolicy(policy, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_SPONSORSHIP_SCOPE");
    BOOST_CHECK(!PolicyAllowsFundingModel(policy, static_cast<FundingModel>(255)));

    const auto successor = ComputeCarrierSuccessor(DDCents{100}, DDCents{99}, error);
    BOOST_REQUIRE(successor);
    BOOST_CHECK_EQUAL(successor->value, 199);
    BOOST_CHECK(!ComputeCarrierSuccessor(DDCents{MAX_DD_OUTPUT_CENTS}, DDCents{1}, error));
}

BOOST_AUTO_TEST_CASE(policy_hash_binds_every_operating_limit)
{
    auto policy = UserPaidPolicy(50);
    const uint256 original = GetProviderPolicyHash(policy);
    policy.maximum_network_fee.value += 1;
    BOOST_CHECK(original != GetProviderPolicyHash(policy));
    policy.maximum_network_fee.value -= 1;
    policy.quote_ttl -= 1;
    BOOST_CHECK(original != GetProviderPolicyHash(policy));
}

BOOST_AUTO_TEST_CASE(identity_record_binds_internal_key_id_and_bip86_script)
{
    CKey key;
    key.MakeNewKey(true);
    ProviderIdentityRecord identity;
    identity.identity_key = XOnlyPubKey{key.GetPubKey()};
    identity.provider_id = GetPaymasterId(identity.identity_key);
    TaprootBuilder builder;
    builder.Finalize(identity.identity_key);
    identity.identity_script = GetScriptForDestination(builder.GetOutput());
    identity.display_name = "Provider";
    identity.created_at = 1;
    BOOST_CHECK(ValidateProviderIdentityRecord(identity));
    identity.provider_id = uint256::ONE;
    BOOST_CHECK(!ValidateProviderIdentityRecord(identity));
}

BOOST_AUTO_TEST_CASE(admission_and_operational_pool_readiness_is_disjoint)
{
    const auto policy = UserPaidPolicy(50);
    std::vector<ProviderPoolEntry> entries;
    for (uint32_t index = 0; index < 3; ++index) {
        entries.push_back(PoolEntry(index, PoolPurpose::ADMISSION, PoolAsset::DGB,
                                    MIN_ADMISSION_DGB_SATOSHIS));
        entries.push_back(PoolEntry(index + 3, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER, 100));
    }
    entries.push_back(PoolEntry(6, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
                                policy.maximum_network_fee.value));
    entries.push_back(PoolEntry(7, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 100));

    std::string error;
    BOOST_CHECK(ValidateProviderPoolEntries(entries, error));
    auto readiness = EvaluateProviderPoolReadiness(entries, policy, 100, 1);
    BOOST_CHECK(readiness.ready);
    BOOST_CHECK_EQUAL(readiness.complete_operational_slots, 1U);

    auto duplicate = entries;
    duplicate.back().outpoint = duplicate.front().outpoint;
    BOOST_CHECK(!ValidateProviderPoolEntries(duplicate, error));

    auto unknown_enum = entries;
    unknown_enum.front().state = static_cast<PoolEntryState>(255);
    BOOST_CHECK(!ValidateProviderPoolEntries(unknown_enum, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_POOL_ENTRY");

    auto missing_carrier = entries;
    missing_carrier.erase(missing_carrier.begin() + 5);
    readiness = EvaluateProviderPoolReadiness(missing_carrier, policy, 100, 1);
    BOOST_CHECK(!readiness.ready);
    BOOST_CHECK_EQUAL(readiness.admission_carriers, 2U);

    auto reserved = entries;
    reserved.back().state = PoolEntryState::RESERVED;
    reserved.back().reservation_id = uint256S("02");
    readiness = EvaluateProviderPoolReadiness(reserved, policy, 100, 1);
    BOOST_CHECK(!readiness.ready);
    BOOST_CHECK_EQUAL(readiness.complete_operational_slots, 0U);
}

BOOST_AUTO_TEST_CASE(operational_dgb_below_policy_capacity_is_not_ready)
{
    const ProviderPolicy policy{UserPaidPolicy(50)};
    std::vector<ProviderPoolEntry> entries;
    for (uint32_t index = 0; index < 3; ++index) {
        entries.push_back(PoolEntry(
            index, PoolPurpose::ADMISSION, PoolAsset::DGB,
            MIN_ADMISSION_DGB_SATOSHIS));
        entries.push_back(PoolEntry(
            index + 3, PoolPurpose::ADMISSION,
            PoolAsset::DD_CARRIER, 100));
    }
    entries.push_back(PoolEntry(
        6, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
        policy.maximum_network_fee.value - 1));
    entries.push_back(PoolEntry(
        7, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 100));

    std::string error;
    BOOST_REQUIRE(ValidateProviderPoolEntries(entries, error));
    const ProviderPoolReadiness readiness{
        EvaluateProviderPoolReadiness(entries, policy, 100, 1)};
    BOOST_CHECK(!readiness.ready);
    BOOST_CHECK_EQUAL(readiness.operational_dgb, 0U);
    BOOST_CHECK_EQUAL(readiness.operational_carriers, 1U);
    BOOST_CHECK_EQUAL(readiness.complete_operational_slots, 0U);
    BOOST_REQUIRE_EQUAL(readiness.errors.size(), 1U);
    BOOST_CHECK_EQUAL(readiness.errors.front(),
                      "PAYMASTER_OPERATIONAL_SLOT_MISSING");

    entries[6].dgb_value = policy.maximum_network_fee;
    const ProviderPoolReadiness exact_capacity{
        EvaluateProviderPoolReadiness(entries, policy, 100, 1)};
    BOOST_CHECK(exact_capacity.ready);
    BOOST_CHECK_EQUAL(exact_capacity.operational_dgb, 1U);
    BOOST_CHECK_EQUAL(exact_capacity.complete_operational_slots, 1U);
}

BOOST_AUTO_TEST_CASE(pure_sponsor_needs_no_carrier_pool)
{
    ProviderPolicy policy = UserPaidPolicy(0);
    policy.funding_models = FUNDING_MODEL_SPONSORED;
    std::vector<ProviderPoolEntry> entries;
    for (uint32_t index = 0; index < 3; ++index) {
        entries.push_back(PoolEntry(index, PoolPurpose::ADMISSION, PoolAsset::DGB,
                                    MIN_ADMISSION_DGB_SATOSHIS));
    }
    entries.push_back(PoolEntry(3, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
                                policy.maximum_network_fee.value));
    const auto readiness = EvaluateProviderPoolReadiness(entries, policy, 100, 1);
    BOOST_CHECK(readiness.ready);
    BOOST_CHECK_EQUAL(readiness.admission_carriers, 0U);
}

BOOST_AUTO_TEST_CASE(pool_successor_provenance_and_active_states_are_fail_closed)
{
    ProviderPoolEntry successor{
        PoolEntry(20, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 103)};
    const uint256 origin_commit_key{uint256S("20")};
    successor.state = PoolEntryState::PENDING_SUCCESSOR;
    successor.reservation_id = origin_commit_key;
    successor.origin_commit_key = origin_commit_key;

    std::string error;
    BOOST_REQUIRE(ValidateProviderPoolEntries({successor}, error));
    BOOST_CHECK(IsActiveProviderPoolState(successor.state));

    ProviderPoolEntry missing_origin{successor};
    missing_origin.origin_commit_key.SetNull();
    BOOST_CHECK(!ValidateProviderPoolEntries({missing_origin}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_POOL_PROVENANCE");

    ProviderPoolEntry mismatched_origin{successor};
    mismatched_origin.origin_commit_key = uint256S("21");
    BOOST_CHECK(!ValidateProviderPoolEntries({mismatched_origin}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_POOL_PROVENANCE");

    ProviderPoolEntry confirmed_successor{successor};
    confirmed_successor.state = PoolEntryState::AVAILABLE;
    confirmed_successor.reservation_id.SetNull();
    BOOST_REQUIRE(ValidateProviderPoolEntries({confirmed_successor}, error));
    BOOST_CHECK_EQUAL(confirmed_successor.origin_commit_key,
                      origin_commit_key);

    BOOST_CHECK(IsActiveProviderPoolState(PoolEntryState::AVAILABLE));
    BOOST_CHECK(IsActiveProviderPoolState(PoolEntryState::RESERVED));
    BOOST_CHECK(IsActiveProviderPoolState(PoolEntryState::PENDING_SUCCESSOR));
    BOOST_CHECK(IsActiveProviderPoolState(PoolEntryState::COMMITTED));
    BOOST_CHECK(!IsActiveProviderPoolState(PoolEntryState::SPENT));
    BOOST_CHECK(!IsActiveProviderPoolState(PoolEntryState::RELEASED));
    BOOST_CHECK(!IsActiveProviderPoolState(PoolEntryState::INVALIDATED));

    ProviderPoolEntry released{confirmed_successor};
    released.state = PoolEntryState::RELEASED;
    BOOST_CHECK(ValidateProviderPoolEntries({released}, error));
    ProviderPoolEntry invalidated{confirmed_successor};
    invalidated.state = PoolEntryState::INVALIDATED;
    BOOST_CHECK(ValidateProviderPoolEntries({invalidated}, error));
}

BOOST_AUTO_TEST_CASE(liquidity_policy_requires_coherent_targets_and_finite_approval)
{
    std::string error;
    ProviderLiquidityPolicy policy{LiquidityPolicy(/*paid_maintenance_approved=*/false)};
    policy.maximum_maintenance_fee_per_transaction = {};
    policy.maximum_maintenance_fee_per_hour = {};
    policy.maximum_maintenance_fee_per_day = {};
    BOOST_REQUIRE(ValidateProviderLiquidityPolicy(policy, error));

    policy = LiquidityPolicy();
    BOOST_REQUIRE(ValidateProviderLiquidityPolicy(policy, error));

    const ProviderPolicy user_paid_policy{UserPaidPolicy(0)};
    BOOST_CHECK(!ProviderLiquidityTargetsSatisfyPolicy(
        policy, user_paid_policy));

    ProviderLiquidityPolicy invalid{policy};
    invalid.maximum_maintenance_fee_per_transaction = {};
    BOOST_CHECK(!ValidateProviderLiquidityPolicy(invalid, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_LIQUIDITY_POLICY");

    invalid = policy;
    invalid.maximum_maintenance_fee_per_hour = DGBSatoshis{99};
    BOOST_CHECK(!ValidateProviderLiquidityPolicy(invalid, error));

    invalid = policy;
    invalid.maximum_maintenance_fee_per_day = DGBSatoshis{149};
    BOOST_CHECK(!ValidateProviderLiquidityPolicy(invalid, error));

    invalid = policy;
    invalid.target_admission_dgb = 2;
    BOOST_CHECK(!ValidateProviderLiquidityPolicy(invalid, error));

    policy.target_admission_carriers = 3;
    BOOST_CHECK(!ProviderLiquidityTargetsSatisfyPolicy(
        policy, user_paid_policy));
    policy.target_operational_carriers = 1;
    BOOST_REQUIRE(ValidateProviderLiquidityPolicy(policy, error));
    BOOST_CHECK(ProviderLiquidityTargetsSatisfyPolicy(
        policy, user_paid_policy));

    ProviderPolicy sponsored_only_policy{user_paid_policy};
    sponsored_only_policy.funding_models = FUNDING_MODEL_SPONSORED;
    ProviderLiquidityPolicy sponsored_liquidity{LiquidityPolicy()};
    BOOST_REQUIRE(ValidateProviderPolicy(sponsored_only_policy, error));
    BOOST_CHECK(ProviderLiquidityTargetsSatisfyPolicy(
        sponsored_liquidity, sponsored_only_policy));

    invalid = policy;
    invalid.target_admission_carriers = 0;
    BOOST_CHECK(!ValidateProviderLiquidityPolicy(invalid, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_LIQUIDITY_POLICY");
}

BOOST_AUTO_TEST_CASE(provider_maintenance_budget_transitions_are_bounded_and_idempotent)
{
    const ProviderLiquidityPolicy policy{LiquidityPolicy()};
    ProviderMaintenanceLedger ledger;
    std::string error;

    ProviderMaintenanceRecord first{MaintenanceRecord(
        uint256S("31"), uint256S("a1"), 31, /*maximum_fee=*/80)};
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, first, /*now=*/1000, error));
    BOOST_CHECK_EQUAL(ledger.records.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1000);

    BOOST_CHECK(ReserveProviderMaintenanceBudget(
        ledger, policy, first, /*now=*/999, error));
    BOOST_CHECK_EQUAL(ledger.records.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1000);

    ProviderMaintenanceRecord over_hourly_limit{MaintenanceRecord(
        uint256S("36"), uint256S("a6"), 36, /*maximum_fee=*/71)};
    const uint256 ledger_before_hourly_failure{
        SerializedPaymasterHash(ledger)};
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        ledger, policy, over_hourly_limit, /*now=*/1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED");
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(ledger),
                      ledger_before_hourly_failure);

    ProviderMaintenanceRecord conflicting{first};
    conflicting.plan_id = uint256S("a2");
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        ledger, policy, conflicting, /*now=*/1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT");

    const uint256 first_txid{uint256S("b1")};
    BOOST_REQUIRE(SpendProviderMaintenanceBudget(
        ledger, first.operation_id, first_txid, DGBSatoshis{60},
        /*now=*/1001, error));
    BOOST_CHECK(SpendProviderMaintenanceBudget(
        ledger, first.operation_id, first_txid, DGBSatoshis{60},
        /*now=*/900, error));
    BOOST_CHECK(!SpendProviderMaintenanceBudget(
        ledger, first.operation_id, first_txid, DGBSatoshis{59},
        /*now=*/1002, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT");
    ledger.records[0].state = ProviderMaintenanceState::CONFIRMED;
    ledger.records[0].updated_at = 1002;

    ProviderMaintenanceRecord second{MaintenanceRecord(
        uint256S("32"), uint256S("a2"), 32, /*maximum_fee=*/90)};
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, second, /*now=*/5000, error));
    BOOST_REQUIRE(SpendProviderMaintenanceBudget(
        ledger, second.operation_id, uint256S("b2"), DGBSatoshis{80},
        /*now=*/5001, error));
    ledger.records[1].state = ProviderMaintenanceState::CONFIRMED;
    ledger.records[1].updated_at = 5001;

    ProviderMaintenanceRecord third{MaintenanceRecord(
        uint256S("33"), uint256S("a3"), 33, /*maximum_fee=*/100)};
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, third, /*now=*/9000, error));
    BOOST_REQUIRE(SpendProviderMaintenanceBudget(
        ledger, third.operation_id, uint256S("b3"), DGBSatoshis{90},
        /*now=*/9001, error));
    ledger.records[2].state = ProviderMaintenanceState::CONFIRMED;
    ledger.records[2].updated_at = 9001;

    ProviderMaintenanceRecord over_daily_limit{MaintenanceRecord(
        uint256S("34"), uint256S("a4"), 34, /*maximum_fee=*/21)};
    const uint256 ledger_before_failure{SerializedPaymasterHash(ledger)};
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        ledger, policy, over_daily_limit, /*now=*/13000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED");
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(ledger), ledger_before_failure);

    ProviderMaintenanceRecord releasable{MaintenanceRecord(
        uint256S("35"), uint256S("a5"), 35, /*maximum_fee=*/10)};
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, releasable, /*now=*/17000, error));
    BOOST_REQUIRE(ReleaseProviderMaintenanceBudget(
        ledger, releasable.operation_id, /*now=*/17001, error));
    BOOST_CHECK(ReleaseProviderMaintenanceBudget(
        ledger, releasable.operation_id, /*now=*/16000, error));

    ProviderMaintenanceLedger restarted{ledger};
    BOOST_CHECK(ValidateProviderMaintenanceLedger(restarted, error));
    BOOST_CHECK(ReleaseProviderMaintenanceBudget(
        restarted, releasable.operation_id, /*now=*/18000, error));
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(restarted),
                      SerializedPaymasterHash(ledger));
}

BOOST_AUTO_TEST_CASE(provider_maintenance_budget_enforces_every_operator_gate)
{
    ProviderLiquidityPolicy policy{LiquidityPolicy()};
    ProviderMaintenanceLedger ledger;
    std::string error;

    const ProviderMaintenanceRecord exact_limit{MaintenanceRecord(
        uint256S("361"), uint256S("a61"), 61,
        policy.maximum_maintenance_fee_per_transaction.value)};

    // Explicit approval is independent from otherwise finite limits. A
    // missing approval must leave both the accounting high-water mark and the
    // durable record set untouched.
    policy.paid_maintenance_approved = false;
    const uint256 before_approval_failure{SerializedPaymasterHash(ledger)};
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        ledger, policy, exact_limit, /*now=*/1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED");
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(ledger),
                      before_approval_failure);

    policy.paid_maintenance_approved = true;
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, exact_limit, /*now=*/1000, error));
    BOOST_REQUIRE_EQUAL(ledger.records.size(), 1U);

    ProviderMaintenanceRecord above_transaction{MaintenanceRecord(
        uint256S("362"), uint256S("a62"), 62,
        policy.maximum_maintenance_fee_per_transaction.value + 1)};
    const uint256 before_transaction_failure{SerializedPaymasterHash(ledger)};
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        ledger, policy, above_transaction, /*now=*/1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_RESERVATION");
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(ledger),
                      before_transaction_failure);

    // The first exact reservation consumes part of both rolling ceilings.
    // Reserving another operation is rejected before any record is appended,
    // even though that operation would independently satisfy the per-tx cap.
    ProviderMaintenanceRecord over_hour{MaintenanceRecord(
        uint256S("363"), uint256S("a63"), 63, /*maximum_fee=*/51)};
    const uint256 before_hour_failure{SerializedPaymasterHash(ledger)};
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        ledger, policy, over_hour, /*now=*/1002, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED");
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(ledger), before_hour_failure);

    // Releasing a merely planned operation removes the exposure exactly once
    // and permits a different operation without resetting historical spend.
    BOOST_REQUIRE(ReleaseProviderMaintenanceBudget(
        ledger, exact_limit.operation_id, /*now=*/1003, error));
    BOOST_CHECK(ReleaseProviderMaintenanceBudget(
        ledger, exact_limit.operation_id, /*now=*/1004, error));
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, over_hour, /*now=*/1005, error));
    BOOST_REQUIRE_EQUAL(ledger.records.size(), 2U);
    BOOST_CHECK(ledger.records.front().state ==
                ProviderMaintenanceState::RELEASED);
    BOOST_CHECK(ledger.records.back().state ==
                ProviderMaintenanceState::PLANNED);
    BOOST_CHECK_EQUAL(ledger.records.back().operation_id,
                      over_hour.operation_id);

    // A broadcast transaction is charged by its actual fee and remains an
    // open liability. Neither a backward clock nor expiry of the rolling
    // window can make the same operation spend a different amount.
    BOOST_REQUIRE(SpendProviderMaintenanceBudget(
        ledger, over_hour.operation_id, uint256S("b63"), DGBSatoshis{50},
        /*now=*/1006, error));
    BOOST_CHECK(SpendProviderMaintenanceBudget(
        ledger, over_hour.operation_id, uint256S("b63"), DGBSatoshis{50},
        /*now=*/900, error));
    BOOST_CHECK(!SpendProviderMaintenanceBudget(
        ledger, over_hour.operation_id, uint256S("b63"), DGBSatoshis{49},
        /*now=*/1007, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT");
}

BOOST_AUTO_TEST_CASE(old_unconfirmed_maintenance_broadcast_remains_open_budget_exposure)
{
    const ProviderLiquidityPolicy policy{LiquidityPolicy()};
    ProviderMaintenanceLedger ledger;
    std::string error;

    ProviderMaintenanceRecord old_broadcast{MaintenanceRecord(
        uint256S("37"), uint256S("a7"), 37, /*maximum_fee=*/80)};
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, old_broadcast, /*now=*/1000, error));
    BOOST_REQUIRE(SpendProviderMaintenanceBudget(
        ledger, old_broadcast.operation_id, uint256S("b7"),
        DGBSatoshis{60}, /*now=*/1001, error));
    BOOST_REQUIRE_EQUAL(ledger.records.size(), 1U);
    BOOST_CHECK(ledger.records.front().state ==
                ProviderMaintenanceState::BROADCAST);

    // An unconfirmed maintenance transaction remains an open liability even
    // after both rolling accounting windows have elapsed. Otherwise a stalled
    // transaction could be replaced repeatedly and exceed the configured cap.
    constexpr int64_t AFTER_DAY_WINDOW{1001 + 24 * 60 * 60 + 1};
    ProviderMaintenanceRecord replacement{MaintenanceRecord(
        uint256S("38"), uint256S("a8"), 38, /*maximum_fee=*/91)};
    const uint256 ledger_before_failure{SerializedPaymasterHash(ledger)};
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        ledger, policy, replacement, AFTER_DAY_WINDOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED");
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(ledger),
                      ledger_before_failure);

    // Once chain reconciliation marks the old transaction terminal, its old
    // spend falls outside the rolling windows and no longer blocks a new plan.
    ledger.records.front().state = ProviderMaintenanceState::CONFIRMED;
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        ledger, policy, replacement, AFTER_DAY_WINDOW, error));
    BOOST_REQUIRE_EQUAL(ledger.records.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.records.front().operation_id,
                      replacement.operation_id);
    BOOST_CHECK(ledger.records.front().state ==
                ProviderMaintenanceState::PLANNED);
}

BOOST_AUTO_TEST_CASE(provider_maintenance_records_reject_unknown_shapes)
{
    ProviderMaintenanceLedger ledger;
    ProviderMaintenanceRecord valid{MaintenanceRecord(
        uint256S("41"), uint256S("c1"), 41, /*maximum_fee=*/50)};
    valid.created_at = 1000;
    valid.updated_at = 1000;
    ledger.records.push_back(valid);
    std::string error;
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(ledger, error));

    ProviderMaintenanceLedger invalid{ledger};
    invalid.records.front().kind =
        static_cast<ProviderMaintenanceKind>(255);
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(invalid, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_RECORD");

    invalid = ledger;
    invalid.records.front().state =
        static_cast<ProviderMaintenanceState>(255);
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(invalid, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_RECORD");

    invalid = ledger;
    invalid.records.front().outputs.front().purpose =
        static_cast<PoolPurpose>(255);
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(invalid, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_OUTPUT");

    ProviderMaintenanceRecord malformed{MaintenanceRecord(
        uint256S("42"), uint256S("c2"), 42, /*maximum_fee=*/50)};
    malformed.operation_id.SetNull();
    ProviderMaintenanceLedger empty;
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        empty, LiquidityPolicy(), malformed, /*now=*/1000, error));
    BOOST_CHECK(empty.records.empty());
}

BOOST_AUTO_TEST_CASE(provider_maintenance_withdrawal_sources_are_versioned_and_bound)
{
    ProviderMaintenanceRecord withdrawal{MaintenanceRecord(
        uint256S("43"), uint256S("c3"), 43, /*maximum_fee=*/50)};
    withdrawal.kind = ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS;
    withdrawal.outputs.front() = MaintenanceOutput(
        43, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 100);
    withdrawal.withdrawal_excess_script_pub_key =
        MaintenanceOutput(44, PoolPurpose::OPERATIONAL,
                          PoolAsset::DD_CARRIER, 100)
            .script_pub_key;
    withdrawal.withdrawal_excess_amount = DDCents{6};
    withdrawal.created_at = 1000;
    withdrawal.updated_at = 1000;

    ProviderMaintenanceLedger ledger;
    ledger.records.push_back(withdrawal);
    std::string error;
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(ledger, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_SOURCE_INPUTS");

    const COutPoint first_source{uint256S("44"), 0};
    const COutPoint second_source{uint256S("45"), 1};
    ledger.records.front().source_inputs = {first_source, second_source};
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(ledger, error));

    ProviderMaintenanceLedger duplicate_excess_script{ledger};
    duplicate_excess_script.records.front()
        .withdrawal_excess_script_pub_key =
        duplicate_excess_script.records.front()
            .outputs.front().script_pub_key;
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(
        duplicate_excess_script, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_OUTPUT");

    ProviderMaintenanceLedger duplicate_source{ledger};
    duplicate_source.records.front().source_inputs.back() = first_source;
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(duplicate_source, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_SOURCE_INPUTS");

    ProviderMaintenanceLedger replenishment_with_source{ledger};
    replenishment_with_source.records.front().kind =
        ProviderMaintenanceKind::REPLENISH_CARRIER;
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(
        replenishment_with_source, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_SOURCE_INPUTS");

    // Version 3 remains supported for withdrawals; version 4 adds setup authorization.
    // Bind compatibility to explicit versions, not CURRENT_VERSION minus one.
    for (const uint16_t version : {0, 1, 2, 3, 4, 5}) {
        BOOST_TEST_CONTEXT("maintenance record version " << version) {
            ProviderMaintenanceLedger versioned{ledger};
            versioned.records.front().version = version;
            const bool supported = version == 3 || version == 4;
            const uint256 original_hash{SerializedPaymasterHash(versioned)};
            BOOST_CHECK_EQUAL(ValidateProviderMaintenanceLedger(versioned, error), supported);
            BOOST_CHECK_EQUAL(error, supported ? "" : "PAYMASTER_INVALID_MAINTENANCE_RECORD");
            BOOST_CHECK_EQUAL(SerializedPaymasterHash(versioned), original_hash);

            CDataStream encoded{SER_DISK, ::PROTOCOL_VERSION};
            encoded << versioned;
            ProviderMaintenanceLedger decoded;
            encoded >> decoded;
            BOOST_REQUIRE_EQUAL(decoded.records.size(), 1U);
            BOOST_CHECK_EQUAL(decoded.records.front().version, version);
            BOOST_CHECK_EQUAL(decoded.records.front().source_inputs.size(), 2U);
            BOOST_CHECK_EQUAL(SerializedPaymasterHash(decoded), original_hash);
            BOOST_CHECK_EQUAL(ValidateProviderMaintenanceLedger(decoded, error), supported);
            BOOST_CHECK_EQUAL(error, supported ? "" : "PAYMASTER_INVALID_MAINTENANCE_RECORD");
        }
    }

    ProviderMaintenanceLedger reservations;
    ProviderMaintenanceRecord first{withdrawal};
    first.source_inputs = {first_source};
    first.created_at = 0;
    first.updated_at = 0;
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(
        reservations, LiquidityPolicy(), first, /*now=*/1000, error));
    ProviderMaintenanceRecord conflicting{first};
    conflicting.source_inputs = {second_source};
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(
        reservations, LiquidityPolicy(), conflicting, /*now=*/1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT");
}

BOOST_AUTO_TEST_CASE(maintenance_source_reorg_recovery_states_are_persistable_and_unambiguous)
{
    const uint256 operation_id{uint256S("46")};
    const uint256 transaction_id{uint256S("47")};
    const COutPoint source_outpoint{uint256S("48"), 0};

    ProviderMaintenanceRecord record{MaintenanceRecord(
        operation_id, uint256S("c4"), 46, /*maximum_fee=*/50)};
    record.kind = ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS;
    record.outputs.front() = MaintenanceOutput(
        46, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 100);
    record.source_inputs = {source_outpoint};
    record.withdrawal_excess_script_pub_key =
        MaintenanceOutput(47, PoolPurpose::OPERATIONAL,
                          PoolAsset::DD_CARRIER, 100)
            .script_pub_key;
    record.withdrawal_excess_amount = DDCents{6};
    record.state = ProviderMaintenanceState::CONFIRMED;
    record.transaction_id = transaction_id;
    record.actual_fee = DGBSatoshis{40};
    record.created_at = 1000;
    record.updated_at = 1002;

    ProviderMaintenanceLedger ledger;
    ledger.accounting_time_high_water = 1002;
    ledger.records = {record};

    ProviderPoolEntry source{
        PoolEntry(70, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 106)};
    source.outpoint = source_outpoint;
    source.state = PoolEntryState::SPENT;
    source.reservation_id.SetNull();

    ProviderPoolEntry successor{
        PoolEntry(71, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 100)};
    successor.outpoint = COutPoint{transaction_id, 0};
    successor.state = PoolEntryState::AVAILABLE;
    successor.origin_commit_key = operation_id;
    std::vector<ProviderPoolEntry> pool{source, successor};

    std::string error;
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(ledger, error));
    BOOST_REQUIRE(ValidateProviderPoolEntries(pool, error));

    // The maintenance operation, its exact source and the successor origin
    // must survive persistence so startup reconciliation can make one
    // deterministic decision after a reorg.
    CDataStream encoded{SER_DISK, ::PROTOCOL_VERSION};
    encoded << ledger << pool;
    ProviderMaintenanceLedger decoded_ledger;
    std::vector<ProviderPoolEntry> decoded_pool;
    encoded >> decoded_ledger >> decoded_pool;
    BOOST_REQUIRE_EQUAL(decoded_ledger.records.size(), 1U);
    BOOST_REQUIRE_EQUAL(decoded_pool.size(), 2U);
    BOOST_REQUIRE_EQUAL(
        decoded_ledger.records.front().source_inputs.size(), 1U);
    BOOST_CHECK(decoded_ledger.records.front().source_inputs.front() ==
                source_outpoint);
    BOOST_CHECK_EQUAL(decoded_pool[1].origin_commit_key, operation_id);

    // A confirmation reorg keeps the source committed and makes the
    // successor pending again. It must not expose both entries as available.
    decoded_ledger.records.front().state =
        ProviderMaintenanceState::BROADCAST;
    decoded_ledger.records.front().updated_at = 1003;
    decoded_pool[0].state = PoolEntryState::COMMITTED;
    decoded_pool[0].reservation_id = operation_id;
    decoded_pool[1].state = PoolEntryState::PENDING_SUCCESSOR;
    decoded_pool[1].confirmation_height = 0;
    decoded_pool[1].reservation_id = operation_id;
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(decoded_ledger, error));
    BOOST_REQUIRE(ValidateProviderPoolEntries(decoded_pool, error));
    BOOST_CHECK(decoded_pool[0].state == PoolEntryState::COMMITTED);
    BOOST_CHECK(decoded_pool[1].state == PoolEntryState::PENDING_SUCCESSOR);
    BOOST_CHECK(IsActiveProviderPoolState(decoded_pool[0].state));
    BOOST_CHECK(IsActiveProviderPoolState(decoded_pool[1].state));

    // A terminal conflict releases the still-unspent source and invalidates
    // the never-confirmed successor. This is the only safe retry shape.
    decoded_ledger.records.front().state = ProviderMaintenanceState::FAILED;
    decoded_ledger.records.front().updated_at = 1004;
    decoded_pool[0].state = PoolEntryState::AVAILABLE;
    decoded_pool[0].reservation_id.SetNull();
    decoded_pool[1].state = PoolEntryState::INVALIDATED;
    decoded_pool[1].reservation_id.SetNull();
    decoded_pool[1].confirmation_height = 0;
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(decoded_ledger, error));
    BOOST_REQUIRE(ValidateProviderPoolEntries(decoded_pool, error));
    BOOST_CHECK(IsActiveProviderPoolState(decoded_pool[0].state));
    BOOST_CHECK(!IsActiveProviderPoolState(decoded_pool[1].state));
    BOOST_CHECK_EQUAL(decoded_pool[1].origin_commit_key, operation_id);
}

BOOST_AUTO_TEST_CASE(carrier_withdrawal_plan_binds_sources_outputs_and_fee)
{
    ProviderCarrierWithdrawalPlan plan;
    plan.plan_id = uint256S("51");
    plan.operation_id = uint256S("52");
    plan.source_carriers = {
        COutPoint{uint256S("53"), 0}, COutPoint{uint256S("54"), 0}};
    plan.replacement_carriers = {
        MaintenanceOutput(51, PoolPurpose::ADMISSION,
                          PoolAsset::DD_CARRIER, 100),
        MaintenanceOutput(52, PoolPurpose::OPERATIONAL,
                          PoolAsset::DD_CARRIER, 100)};
    plan.excess_script_pub_key =
        MaintenanceOutput(53, PoolPurpose::OPERATIONAL,
                          PoolAsset::DD_CARRIER, 100)
            .script_pub_key;
    plan.excess_amount = DDCents{6};
    plan.estimated_fee = DGBSatoshis{10};
    plan.liquidity_policy_updated_at = 900;
    plan.created_at = 1000;
    plan.expires_at = 1060;

    std::string error;
    BOOST_REQUIRE(ValidateProviderCarrierWithdrawalPlan(plan, error));

    ProviderCarrierWithdrawalPlan invalid{plan};
    invalid.source_carriers.back() = invalid.source_carriers.front();
    BOOST_CHECK(!ValidateProviderCarrierWithdrawalPlan(invalid, error));

    invalid = plan;
    invalid.replacement_carriers.front().carrier_value = DDCents{99};
    BOOST_CHECK(!ValidateProviderCarrierWithdrawalPlan(invalid, error));

    invalid = plan;
    invalid.replacement_carriers.front().script_pub_key =
        invalid.excess_script_pub_key;
    BOOST_CHECK(!ValidateProviderCarrierWithdrawalPlan(invalid, error));

    invalid = plan;
    invalid.replacement_carriers.front().purpose =
        static_cast<PoolPurpose>(255);
    BOOST_CHECK(!ValidateProviderCarrierWithdrawalPlan(invalid, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN");

    invalid = plan;
    invalid.estimated_fee = {};
    BOOST_CHECK(!ValidateProviderCarrierWithdrawalPlan(invalid, error));

    ProviderCarrierWithdrawalPlan release;
    release.plan_id = uint256S("61");
    release.operation_id = uint256S("62");
    release.mode = CarrierWithdrawalMode::RELEASE_SLOT;
    release.source_carriers = {COutPoint{uint256S("63"), 0}};
    release.target_operational_carriers_after_release = 0;
    release.liquidity_policy_updated_at = 900;
    release.created_at = 1000;
    release.expires_at = 1060;
    BOOST_REQUIRE(ValidateProviderCarrierWithdrawalPlan(release, error));

    release.target_operational_carriers_after_release = 16;
    BOOST_CHECK(!ValidateProviderCarrierWithdrawalPlan(release, error));
}

BOOST_AUTO_TEST_CASE(public_sponsorship_requires_explicit_finite_safety_limits)
{
    const ProviderPolicy advertised = UserPaidPolicy(0);
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    std::string error;

    safety.public_sponsored = {};
    BOOST_CHECK(!ValidateProviderSafetyPolicy(safety, advertised, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMITS_REQUIRED");

    safety.public_sponsored = SafetyLimits();
    BOOST_CHECK(ValidateProviderSafetyPolicy(safety, advertised, error));

    safety.public_sponsored.maximum_network_fee_per_day = DGBSatoshis{0};
    BOOST_CHECK(!ValidateProviderSafetyPolicy(safety, advertised, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_SAFETY_LIMITS");

    safety.public_sponsored = SafetyLimits();
    safety.public_sponsored.maximum_network_fee_per_hour = DGBSatoshis{0};
    BOOST_CHECK(!ValidateProviderSafetyPolicy(safety, advertised, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_SAFETY_LIMITS");

    safety.public_sponsored = SafetyLimits();
    safety.public_sponsored.maximum_network_fee_per_day =
        DGBSatoshis{MAX_MONEY + 1};
    BOOST_CHECK(!ValidateProviderSafetyPolicy(safety, advertised, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_SAFETY_LIMITS");

    ProviderPolicy user_paid_only = advertised;
    user_paid_only.funding_models = FUNDING_MODEL_USER_PAID;
    safety.public_sponsored = {};
    BOOST_CHECK(ValidateProviderSafetyPolicy(safety, user_paid_only, error));
}

BOOST_AUTO_TEST_CASE(provider_budget_enforces_per_transaction_reserved_and_quote_ceilings)
{
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits(100, 1000, 1000, 2000, 10, 20));
    safety.maximum_active_quotes_total = 2;
    safety.maximum_active_quotes_per_netgroup = 2;
    safety.maximum_active_quotes_per_recipient = 1;
    ProviderBudgetLedger ledger = ProviderLedger();
    const uint256 recipient_a{uint256S("a1")};
    const uint256 recipient_b{uint256S("b1")};
    const uint256 recipient_c{uint256S("c1")};
    std::string error;

    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("01"),
                                           DGBSatoshis{101}, recipient_a, 1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");

    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("02"),
                                            DGBSatoshis{80}, recipient_a, 1000, error));
    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("03"),
                                           DGBSatoshis{70}, recipient_a, 1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("03"),
                                            DGBSatoshis{70}, recipient_b, 1000, error));
    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("04"),
                                           DGBSatoshis{1}, recipient_c, 1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");

    ProviderBudgetLedger reserved_ledger = ProviderLedger();
    const ProviderSafetyPolicy reserved_safety =
        SafetyPolicy(SafetyLimits(100, 150, 1000, 2000, 10, 20));
    BOOST_REQUIRE(ReserveTestProviderBudget(reserved_ledger, reserved_safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("05"),
                                            DGBSatoshis{80}, recipient_a, 1000, error));
    BOOST_CHECK(!ReserveTestProviderBudget(reserved_ledger, reserved_safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("06"),
                                           DGBSatoshis{71}, recipient_b, 1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    BOOST_CHECK(ReserveTestProviderBudget(reserved_ledger, reserved_safety, FundingModel::USER_PAID,
                                          SponsorshipScope::PUBLIC, uint256S("06"),
                                          DGBSatoshis{70}, recipient_b, 1000, error));
}

BOOST_AUTO_TEST_CASE(provider_budget_enforces_pseudonymous_netgroup_quote_ceiling)
{
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 2;
    safety.maximum_active_quotes_per_recipient = 3;
    ProviderBudgetLedger ledger = ProviderLedger();
    const std::vector<unsigned char> first_canonical_group{1, 10, 20};
    const std::vector<unsigned char> second_canonical_group{1, 10, 21};
    const uint256 first_group = GetNetgroupBudgetBucket(
        ledger, first_canonical_group);
    const uint256 second_group = GetNetgroupBudgetBucket(
        ledger, second_canonical_group);
    BOOST_REQUIRE(!first_group.IsNull());
    BOOST_CHECK(first_group != second_group);
    BOOST_CHECK(GetNetgroupBudgetBucket(ledger, {}).IsNull());

    std::string error;
    BOOST_REQUIRE(DigiDollar::Paymaster::ReserveProviderBudget(
        ledger, safety, FundingModel::USER_PAID, SponsorshipScope::PUBLIC,
        uint256S("d1"), DGBSatoshis{10}, uint256S("e1"), 1000, error,
        first_group));
    BOOST_REQUIRE(DigiDollar::Paymaster::ReserveProviderBudget(
        ledger, safety, FundingModel::USER_PAID, SponsorshipScope::PUBLIC,
        uint256S("d2"), DGBSatoshis{10}, uint256S("e2"), 1000, error,
        first_group));
    BOOST_CHECK(!DigiDollar::Paymaster::ReserveProviderBudget(
        ledger, safety, FundingModel::USER_PAID, SponsorshipScope::PUBLIC,
        uint256S("d3"), DGBSatoshis{10}, uint256S("e3"), 1000, error,
        first_group));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    BOOST_CHECK(DigiDollar::Paymaster::ReserveProviderBudget(
        ledger, safety, FundingModel::USER_PAID, SponsorshipScope::PUBLIC,
        uint256S("d3"), DGBSatoshis{10}, uint256S("e3"), 1000, error,
        second_group));
}

BOOST_AUTO_TEST_CASE(provider_quote_request_rate_is_monotonic_persistent_and_idempotent)
{
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    safety.maximum_quote_requests_per_netgroup_per_minute = 2;
    ProviderBudgetLedger ledger = ProviderLedger();
    const std::vector<unsigned char> first_canonical_group{1, 10, 20};
    const std::vector<unsigned char> second_canonical_group{1, 10, 21};
    const uint256 first_group = GetNetgroupBudgetBucket(
        ledger, first_canonical_group);
    const uint256 second_group = GetNetgroupBudgetBucket(
        ledger, second_canonical_group);
    ProviderBudgetLedger restarted_bucket_ledger{ledger};
    BOOST_CHECK_EQUAL(GetNetgroupBudgetBucket(restarted_bucket_ledger,
                                              first_canonical_group),
                      first_group);
    BOOST_CHECK(GetNetgroupBudgetBucket(restarted_bucket_ledger,
                                        second_canonical_group) != first_group);
    std::string error;

    ProviderBudgetLedger outdated{ledger};
    --outdated.version;
    const uint256 outdated_hash{SerializedPaymasterHash(outdated)};
    BOOST_CHECK(!RecordProviderQuoteRequest(
        outdated, safety, uint256S("d0"), first_group, 999, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER");
    BOOST_CHECK_EQUAL(SerializedPaymasterHash(outdated), outdated_hash);

    BOOST_REQUIRE(RecordProviderQuoteRequest(
        ledger, safety, uint256S("d1"), first_group, 1000, error));
    BOOST_CHECK_EQUAL(ledger.version, ProviderBudgetLedger::CURRENT_VERSION);
    BOOST_CHECK_EQUAL(ledger.quote_requests.size(), 1U);

    BOOST_CHECK(RecordProviderQuoteRequest(
        ledger, safety, uint256S("d1"), first_group, 1001, error));
    BOOST_CHECK_EQUAL(ledger.quote_requests.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1000);
    BOOST_CHECK(!RecordProviderQuoteRequest(
        ledger, safety, uint256S("d1"), uint256S("d9"), first_group,
        1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_REQUEST_BUDGET_CONFLICT");
    BOOST_CHECK(!RecordProviderQuoteRequest(
        ledger, safety, uint256S("d1"), second_group, 1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_REQUEST_BUDGET_CONFLICT");

    BOOST_REQUIRE(RecordProviderQuoteRequest(
        ledger, safety, uint256S("d2"), first_group, 1001, error));
    BOOST_CHECK(!RecordProviderQuoteRequest(
        ledger, safety, uint256S("d3"), first_group, 1002, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_REQUEST_RATE_EXHAUSTED");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1001);
    BOOST_REQUIRE(RecordProviderQuoteRequest(
        ledger, safety, uint256S("d3"), second_group, 1002, error));

    BOOST_CHECK(!RecordProviderQuoteRequest(
        ledger, safety, uint256S("d4"), first_group, 1, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_REQUEST_RATE_EXHAUSTED");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1002);

    BOOST_REQUIRE(RecordProviderQuoteRequest(
        ledger, safety, uint256S("d4"), first_group, 1061, error));
    BOOST_CHECK_EQUAL(ledger.quote_requests.size(), 2U);
    ProviderBudgetLedger restarted{ledger};
    BOOST_REQUIRE(RecordProviderQuoteRequest(
        restarted, safety, uint256S("d5"), first_group, 1061, error));
    BOOST_CHECK(!RecordProviderQuoteRequest(
        restarted, safety, uint256S("d6"), first_group, 1061, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_REQUEST_RATE_EXHAUSTED");
    BOOST_CHECK(ValidateProviderBudgetLedger(restarted, error));

    restarted.quote_requests.back().admitted_at = 1062;
    BOOST_CHECK(!ValidateProviderBudgetLedger(restarted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_PROVIDER_QUOTE_REQUEST_EVENT");
}

BOOST_AUTO_TEST_CASE(provider_budget_rejects_unknown_classes_and_malformed_limits)
{
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    ProviderBudgetLedger ledger = ProviderLedger();
    std::string error;

    BOOST_CHECK(!ReserveTestProviderBudget(
        ledger, safety, static_cast<FundingModel>(255), SponsorshipScope::PUBLIC,
        uint256S("01"), DGBSatoshis{1}, uint256S("a1"), 1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_FUNDING_MODEL_OR_SCOPE");
    BOOST_CHECK(ledger.reservations.empty());
    BOOST_CHECK(!ReserveTestProviderBudget(
        ledger, safety, FundingModel::USER_PAID, SponsorshipScope::RESTRICTED,
        uint256S("01"), DGBSatoshis{1}, uint256S("a1"), 1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_FUNDING_MODEL_OR_SCOPE");
    BOOST_CHECK(ledger.reservations.empty());

    safety.user_paid.maximum_reserved_network_fee = DGBSatoshis{0};
    const ProviderSafetyStatus malformed = EvaluateProviderSafetyStatus(
        ledger, safety, FundingModel::USER_PAID, SponsorshipScope::PUBLIC,
        uint256S("a1"), DGBSatoshis{1}, 1000, uint256S("f00d"));
    BOOST_CHECK(!malformed.can_accept_quote);
    BOOST_REQUIRE_EQUAL(malformed.errors.size(), 1U);
    BOOST_CHECK_EQUAL(malformed.errors.front(), "PAYMASTER_INVALID_SAFETY_LIMITS");
}

BOOST_AUTO_TEST_CASE(provider_budget_enforces_rolling_fee_ceilings)
{
    const ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits(100, 1000, 150, 250, 20, 20));
    ProviderBudgetLedger ledger = ProviderLedger();
    const uint256 recipient{uint256S("a1")};
    std::string error;

    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("01"),
                                            DGBSatoshis{80}, recipient, 1000, error));
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("01"), 1000, error));
    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("02"),
                                            DGBSatoshis{70}, recipient, 1001, error));
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("02"), 1001, error));

    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("03"),
                                           DGBSatoshis{1}, recipient, 1002, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");

    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("03"),
                                            DGBSatoshis{100}, recipient, 4602, error));
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("03"), 4602, error));
    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("04"),
                                           DGBSatoshis{1}, recipient, 4603, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
}

BOOST_AUTO_TEST_CASE(provider_budget_enforces_rolling_completion_ceilings)
{
    const ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits(100, 1000, 1000, 2000, 2, 3));
    ProviderBudgetLedger ledger = ProviderLedger();
    const uint256 recipient{uint256S("a1")};
    std::string error;

    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("01"),
                                            DGBSatoshis{1}, recipient, 1000, error));
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("01"), 1000, error));
    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("02"),
                                            DGBSatoshis{1}, recipient, 1001, error));
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("02"), 1001, error));
    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("03"),
                                           DGBSatoshis{1}, recipient, 1002, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");

    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("03"),
                                            DGBSatoshis{1}, recipient, 4602, error));
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("03"), 4602, error));
    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("04"),
                                           DGBSatoshis{1}, recipient, 4603, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
}

BOOST_AUTO_TEST_CASE(provider_budget_transitions_are_idempotent_and_fail_closed)
{
    const ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    ProviderBudgetLedger ledger = ProviderLedger();
    const uint256 recipient{uint256S("a1")};
    std::string error;

    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::SPONSORED,
                                            SponsorshipScope::PUBLIC, uint256S("01"),
                                            DGBSatoshis{10}, recipient, 1000, error));
    BOOST_CHECK(ReserveTestProviderBudget(ledger, safety, FundingModel::SPONSORED,
                                          SponsorshipScope::PUBLIC, uint256S("01"),
                                          DGBSatoshis{10}, recipient, 1001, error));
    BOOST_CHECK_EQUAL(ledger.reservations.size(), 1U);
    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::SPONSORED,
                                           SponsorshipScope::PUBLIC, uint256S("01"),
                                           DGBSatoshis{11}, recipient, 1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_CONFLICT");
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("01"), 1002, error));
    BOOST_CHECK(SpendProviderBudget(ledger, uint256S("01"), 1003, error));
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1002);
    BOOST_CHECK(!ReleaseProviderBudget(ledger, uint256S("01"), 1004, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_ALREADY_SPENT");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1002);

    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::SPONSORED,
                                            SponsorshipScope::PUBLIC, uint256S("02"),
                                            DGBSatoshis{10}, recipient, 1005, error));
    BOOST_REQUIRE(ReleaseProviderBudget(ledger, uint256S("02"), 1006, error));
    BOOST_CHECK(ReleaseProviderBudget(ledger, uint256S("02"), 1007, error));
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1006);
    BOOST_CHECK(!SpendProviderBudget(ledger, uint256S("02"), 1008, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_RELEASED");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1006);

    BOOST_CHECK(!SpendProviderBudget(ledger, uint256S("03"), 2000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1006);
}

BOOST_AUTO_TEST_CASE(provider_budget_clock_and_recipient_buckets_are_monotonic_and_private)
{
    ProviderBudgetLedger ledger = ProviderLedger(5000);
    CScript recipient_a;
    recipient_a << OP_TRUE;
    CScript recipient_b;
    recipient_b << OP_FALSE;
    const uint256 bucket_a = GetRecipientBudgetBucket(ledger, recipient_a);
    const uint256 bucket_b = GetRecipientBudgetBucket(ledger, recipient_b);
    BOOST_CHECK(!bucket_a.IsNull());
    BOOST_CHECK(bucket_a == GetRecipientBudgetBucket(ledger, recipient_a));
    BOOST_CHECK(bucket_a != bucket_b);
    BOOST_CHECK(GetRecipientBudgetBucket(ledger, CScript{}).IsNull());

    ProviderBudgetLedger other_wallet = ledger;
    other_wallet.recipient_bucket_secret = uint256S("02");
    BOOST_CHECK(bucket_a != GetRecipientBudgetBucket(other_wallet, recipient_a));

    const ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits(10, 10, 10, 10, 10, 10));
    std::string error;
    BOOST_REQUIRE(ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                            SponsorshipScope::PUBLIC, uint256S("01"),
                                            DGBSatoshis{10}, bucket_a, 4000, error));
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 5000);
    BOOST_CHECK_EQUAL(ledger.reservations.back().reserved_at, 5000);
    BOOST_REQUIRE(SpendProviderBudget(ledger, uint256S("01"), 1, error));
    BOOST_CHECK_EQUAL(ledger.reservations.back().updated_at, 5000);
    BOOST_CHECK(!ReserveTestProviderBudget(ledger, safety, FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, uint256S("02"),
                                           DGBSatoshis{1}, bucket_b, 2, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 5000);
}

BOOST_AUTO_TEST_CASE(provider_budget_arithmetic_and_ledger_state_fail_closed)
{
    ProviderBudgetLedger ledger = ProviderLedger();
    ProviderBudgetReservation first;
    first.commit_key = uint256S("01");
    first.network_fee = DGBSatoshis{std::numeric_limits<int64_t>::max()};
    first.recipient_bucket = uint256S("a1");
    first.netgroup_bucket = uint256S("f00d");
    first.reserved_at = 1000;
    first.updated_at = 1000;
    ProviderBudgetReservation second = first;
    second.commit_key = uint256S("02");
    second.network_fee = DGBSatoshis{1};
    ledger.reservations = {first, second};

    std::string error;
    BOOST_REQUIRE(ValidateProviderBudgetLedger(ledger, error));
    const ProviderSafetyStatus status = EvaluateProviderSafetyStatus(
        ledger, SafetyPolicy(SafetyLimits()), FundingModel::USER_PAID,
        SponsorshipScope::PUBLIC, uint256S("b1"), DGBSatoshis{1}, 1000);
    BOOST_CHECK(!status.can_accept_quote);
    BOOST_REQUIRE_EQUAL(status.errors.size(), 1U);
    BOOST_CHECK_EQUAL(status.errors.front(), "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");

    ledger.reservations.front().state = static_cast<BudgetReservationState>(255);
    BOOST_CHECK(!ValidateProviderBudgetLedger(ledger, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_PROVIDER_BUDGET_RESERVATION");
    const int64_t high_water_before_failed_spend = ledger.accounting_time_high_water;
    BOOST_CHECK(!SpendProviderBudget(ledger, uint256S("01"), 2000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_PROVIDER_BUDGET_RESERVATION");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, high_water_before_failed_spend);
    ledger.reservations.front().state = BudgetReservationState::RESERVED;
    ledger.reservations.back().commit_key = ledger.reservations.front().commit_key;
    BOOST_CHECK(!ValidateProviderBudgetLedger(ledger, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_PROVIDER_BUDGET_RESERVATION");
}

BOOST_AUTO_TEST_CASE(client_fee_budget_enforces_limits_and_idempotent_transitions)
{
    ClientSafetyPolicy policy;
    policy.maximum_service_fee_per_transaction = DDCents{50};
    policy.maximum_service_fee_per_day = DDCents{100};
    policy.updated_at = 1000;
    ClientFeeLedger ledger = ClientLedger();
    std::string error;
    BOOST_REQUIRE(ValidateClientSafetyPolicy(policy, error));

    BOOST_CHECK(!ReserveClientFee(ledger, policy, uint256S("01"), DDCents{51}, 1000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_FEE_LIMIT_EXCEEDED");
    BOOST_REQUIRE(ReserveClientFee(ledger, policy, uint256S("01"), DDCents{50}, 1000, error));
    BOOST_CHECK(ReserveClientFee(ledger, policy, uint256S("01"), DDCents{50}, 1001, error));
    BOOST_CHECK_EQUAL(ledger.reservations.size(), 1U);
    ClientSafetyPolicy tightened{policy};
    tightened.maximum_service_fee_per_transaction = DDCents{49};
    tightened.updated_at = 1001;
    BOOST_CHECK(!ReserveClientFee(
        ledger, tightened, uint256S("01"), DDCents{50}, 1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_FEE_LIMIT_EXCEEDED");
    BOOST_REQUIRE(SpendClientFee(ledger, uint256S("01"), 1001, error));
    BOOST_CHECK(SpendClientFee(ledger, uint256S("01"), 1002, error));
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1001);
    BOOST_REQUIRE(ReserveClientFee(ledger, policy, uint256S("02"), DDCents{50}, 1002, error));
    BOOST_CHECK(!ReserveClientFee(ledger, policy, uint256S("03"), DDCents{1}, 1003, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_DAILY_FEE_LIMIT_EXCEEDED");
    BOOST_REQUIRE(ReleaseClientFee(ledger, uint256S("02"), 1004, error));
    BOOST_CHECK(ReleaseClientFee(ledger, uint256S("02"), 1005, error));
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1004);
    BOOST_CHECK(!SpendClientFee(ledger, uint256S("02"), 1006, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_FEE_RESERVATION_RELEASED");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 1004);
    BOOST_CHECK(ReserveClientFee(ledger, policy, uint256S("03"), DDCents{1}, 1006, error));

    policy.maximum_service_fee_per_transaction = DDCents{-1};
    BOOST_CHECK(!ValidateClientSafetyPolicy(policy, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CLIENT_SAFETY_POLICY");

    ledger.reservations.front().state = static_cast<BudgetReservationState>(255);
    BOOST_CHECK(!ValidateClientFeeLedger(ledger, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CLIENT_FEE_RESERVATION");
    const int64_t high_water_before_failed_spend = ledger.accounting_time_high_water;
    BOOST_CHECK(!SpendClientFee(ledger, uint256S("01"), 2000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CLIENT_FEE_RESERVATION");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, high_water_before_failed_spend);
}

BOOST_AUTO_TEST_CASE(client_fee_clock_high_water_prevents_window_rollback)
{
    ClientSafetyPolicy policy;
    policy.maximum_service_fee_per_transaction = DDCents{10};
    policy.maximum_service_fee_per_day = DDCents{10};
    policy.updated_at = 5000;
    ClientFeeLedger ledger = ClientLedger(5000);
    std::string error;

    BOOST_REQUIRE(ReserveClientFee(ledger, policy, uint256S("01"), DDCents{10}, 4000, error));
    BOOST_CHECK_EQUAL(ledger.reservations.back().reserved_at, 5000);
    BOOST_REQUIRE(SpendClientFee(ledger, uint256S("01"), 1, error));
    BOOST_CHECK_EQUAL(ledger.reservations.back().updated_at, 5000);
    BOOST_CHECK(!ReserveClientFee(ledger, policy, uint256S("02"), DDCents{1}, 2, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_DAILY_FEE_LIMIT_EXCEEDED");
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, 5000);
}

BOOST_AUTO_TEST_CASE(capacity_admission_is_persistent_idempotent_and_promoted_once)
{
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    safety.maximum_active_quotes_total = 2;
    safety.maximum_active_quotes_per_netgroup = 1;
    safety.maximum_active_quotes_per_recipient = 2;
    safety.maximum_quote_requests_per_netgroup_per_minute = 2;
    ProviderBudgetLedger ledger = ProviderLedger();
    const uint256 first_group{uint256S("f1")};
    const uint256 second_group{uint256S("f2")};
    const uint256 first_key{uint256S("a1")};
    const uint256 first_hash{uint256S("b1")};
    const uint256 quote_hash{uint256S("c1")};
    const uint256 commit_key{uint256S("d1")};
    std::string error;

    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        ledger, safety, first_key, first_hash, first_group,
        FundingModel::USER_PAID, /*requires_carrier=*/true,
        /*expires_at=*/1060, /*now=*/1000, error));
    BOOST_CHECK_EQUAL(ledger.capacity_admissions.size(), 1U);

    ProviderBudgetLedger restarted{ledger};
    BOOST_CHECK(ReserveProviderCapacityAdmission(
        restarted, safety, first_key, first_hash, first_group,
        FundingModel::USER_PAID, /*requires_carrier=*/true,
        /*expires_at=*/1060, /*now=*/1001, error));
    BOOST_CHECK_EQUAL(restarted.capacity_admissions.size(), 1U);
    BOOST_CHECK_EQUAL(restarted.accounting_time_high_water, 1000);

    BOOST_CHECK(!ReserveProviderCapacityAdmission(
        restarted, safety, first_key, uint256S("b2"), first_group,
        FundingModel::USER_PAID, /*requires_carrier=*/true,
        /*expires_at=*/1060, /*now=*/1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_ADMISSION_CONFLICT");
    BOOST_CHECK(!ReserveProviderCapacityAdmission(
        restarted, safety, uint256S("a2"), uint256S("b2"), first_group,
        FundingModel::SPONSORED, /*requires_carrier=*/false,
        /*expires_at=*/1060, /*now=*/1001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        restarted, safety, uint256S("a2"), uint256S("b2"), second_group,
        FundingModel::SPONSORED, /*requires_carrier=*/false,
        /*expires_at=*/1060, /*now=*/1001, error));

    BOOST_REQUIRE(BindProviderCapacityQuote(
        restarted, first_key, quote_hash, first_group, 1002, error));
    BOOST_CHECK(BindProviderCapacityQuote(
        restarted, first_key, quote_hash, first_group, 1003, error));
    BOOST_CHECK(!BindProviderCapacityQuote(
        restarted, first_key, uint256S("c2"), first_group, 1003, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_QUOTE_CONFLICT");

    BOOST_REQUIRE(PromoteProviderCapacityAdmission(
        restarted, first_key, quote_hash, commit_key, 1004, error));
    BOOST_CHECK(PromoteProviderCapacityAdmission(
        restarted, first_key, quote_hash, commit_key, 1005, error));
    BOOST_CHECK(!PromoteProviderCapacityAdmission(
        restarted, first_key, quote_hash, uint256S("d2"), 1005, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_PROMOTION_CONFLICT");
    BOOST_CHECK(!ReleaseProviderCapacityAdmission(
        restarted, first_key, first_hash, 1005, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_ADMISSION_PROMOTED");
    BOOST_CHECK(ValidateProviderBudgetLedger(restarted, error));
}

BOOST_AUTO_TEST_CASE(capacity_continuation_preflight_excludes_only_its_own_admission)
{
    constexpr int64_t now{1001};
    const CapacityContinuationFixture fixture{CapacityContinuation(now)};
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    safety.maximum_active_quotes_total = 1;
    safety.maximum_active_quotes_per_netgroup = 1;
    safety.maximum_active_quotes_per_recipient = 1;
    ProviderBudgetLedger ledger = ProviderLedger();
    const uint256 group{uint256S("f1")};
    const uint256 request_key{GetProviderRequestSlotKey(
        fixture.capacity_request.provider_id,
        fixture.capacity_request.request_id,
        fixture.capacity_request.session_id)};
    const uint256 request_hash{
        SerializedPaymasterHash(fixture.capacity_request)};
    const uint256 quote_hash{
        RedactedQuoteRequestHash(fixture.quote_request)};
    const uint256 recipient{GetRecipientBudgetBucket(
        ledger, fixture.quote_request.intent.recipient_script)};
    std::string error;

    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        ledger, safety, request_key, request_hash, group,
        FundingModel::USER_PAID, /*requires_carrier=*/true,
        fixture.capacity_request.expires_at, now - 1, error));
    const ProviderSafetyStatus preflight = EvaluateProviderSafetyStatus(
        ledger, safety, FundingModel::USER_PAID, SponsorshipScope::PUBLIC,
        recipient, DGBSatoshis{10}, now, group);
    BOOST_CHECK(!preflight.can_accept_quote);
    BOOST_REQUIRE_EQUAL(preflight.errors.size(), 1U);
    BOOST_CHECK_EQUAL(preflight.errors.front(),
                      "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");

    const ProviderSafetyStatus continuation =
        EvaluateProviderCapacityContinuationPreflight(
            ledger, safety, fixture.capacity_request,
            fixture.quote_request, quote_hash,
            /*requires_carrier=*/true, recipient, DGBSatoshis{10}, now,
            group);
    BOOST_CHECK(continuation.can_accept_quote);
    BOOST_CHECK(continuation.errors.empty());
    BOOST_CHECK_EQUAL(continuation.active_quotes, 0U);

    // The preflight is pure: binding and promotion happened only on its copy.
    BOOST_REQUIRE_EQUAL(ledger.capacity_admissions.size(), 1U);
    BOOST_CHECK(ledger.capacity_admissions.front().state ==
                CapacityAdmissionState::RESERVED);
    BOOST_CHECK(ledger.capacity_admissions.front().quote_request_hash.IsNull());
    BOOST_CHECK(ledger.capacity_admissions.front().commit_key.IsNull());
    BOOST_CHECK_EQUAL(ledger.accounting_time_high_water, now - 1);

    ProviderBudgetLedger already_bound{ledger};
    BOOST_REQUIRE(BindProviderCapacityQuote(
        already_bound, request_key, quote_hash, group, now, error));
    const ProviderSafetyStatus bound_continuation =
        EvaluateProviderCapacityContinuationPreflight(
            already_bound, safety, fixture.capacity_request,
            fixture.quote_request, quote_hash,
            /*requires_carrier=*/true, recipient, DGBSatoshis{10}, now,
            group);
    BOOST_CHECK(bound_continuation.can_accept_quote);
    BOOST_CHECK(already_bound.capacity_admissions.front().state ==
                CapacityAdmissionState::RESERVED);
    BOOST_CHECK_EQUAL(already_bound.capacity_admissions.front().quote_request_hash,
                      quote_hash);

    ProviderBudgetLedger conflicting_bound{ledger};
    BOOST_REQUIRE(BindProviderCapacityQuote(
        conflicting_bound, request_key, uint256S("c1"), group, now,
        error));
    const ProviderSafetyStatus conflicting_continuation =
        EvaluateProviderCapacityContinuationPreflight(
            conflicting_bound, safety, fixture.capacity_request,
            fixture.quote_request, quote_hash,
            /*requires_carrier=*/true, recipient, DGBSatoshis{10}, now,
            group);
    BOOST_CHECK(!conflicting_continuation.can_accept_quote);
    BOOST_REQUIRE_EQUAL(conflicting_continuation.errors.size(), 1U);
    BOOST_CHECK_EQUAL(conflicting_continuation.errors.front(),
                      "PAYMASTER_CAPACITY_QUOTE_CONFLICT");
}

BOOST_AUTO_TEST_CASE(capacity_continuation_preflight_fails_closed_on_binding_mismatch)
{
    constexpr int64_t now{1001};
    const CapacityContinuationFixture fixture{CapacityContinuation(now)};
    const ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    ProviderBudgetLedger ledger = ProviderLedger();
    const uint256 group{uint256S("f1")};
    const uint256 request_key{GetProviderRequestSlotKey(
        fixture.capacity_request.provider_id,
        fixture.capacity_request.request_id,
        fixture.capacity_request.session_id)};
    const uint256 request_hash{
        SerializedPaymasterHash(fixture.capacity_request)};
    const uint256 quote_hash{
        RedactedQuoteRequestHash(fixture.quote_request)};
    const uint256 recipient{GetRecipientBudgetBucket(
        ledger, fixture.quote_request.intent.recipient_script)};
    std::string error;

    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        ledger, safety, request_key, request_hash, group,
        fixture.capacity_request.funding_model,
        fixture.capacity_request.requires_carrier,
        fixture.capacity_request.expires_at, now - 1, error));

    const ProviderSafetyStatus wrong_quote_hash =
        EvaluateProviderCapacityContinuationPreflight(
            ledger, safety, fixture.capacity_request,
            fixture.quote_request, uint256S("c1"),
            fixture.capacity_request.requires_carrier, recipient,
            DGBSatoshis{10}, now, group);
    BOOST_CHECK(!wrong_quote_hash.can_accept_quote);
    BOOST_REQUIRE_EQUAL(wrong_quote_hash.errors.size(), 1U);
    BOOST_CHECK_EQUAL(wrong_quote_hash.errors.front(),
                      "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH");

    PaymasterQuoteRequest wrong_nonce{fixture.quote_request};
    wrong_nonce.intent.client_nonce = uint256S("14");
    const ProviderSafetyStatus nonce_mismatch =
        EvaluateProviderCapacityContinuationPreflight(
            ledger, safety, fixture.capacity_request, wrong_nonce,
            RedactedQuoteRequestHash(wrong_nonce),
            fixture.capacity_request.requires_carrier, recipient,
            DGBSatoshis{10}, now, group);
    BOOST_CHECK(!nonce_mismatch.can_accept_quote);
    BOOST_REQUIRE_EQUAL(nonce_mismatch.errors.size(), 1U);
    BOOST_CHECK_EQUAL(nonce_mismatch.errors.front(),
                      "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH");

    const ProviderSafetyStatus carrier_mismatch =
        EvaluateProviderCapacityContinuationPreflight(
            ledger, safety, fixture.capacity_request,
            fixture.quote_request, quote_hash,
            /*requires_carrier=*/false, recipient, DGBSatoshis{10}, now,
            group);
    BOOST_CHECK(!carrier_mismatch.can_accept_quote);
    BOOST_REQUIRE_EQUAL(carrier_mismatch.errors.size(), 1U);
    BOOST_CHECK_EQUAL(carrier_mismatch.errors.front(),
                      "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH");

    PaymasterCapacityRequest changed_capacity{fixture.capacity_request};
    PaymasterQuoteRequest changed_quote{fixture.quote_request};
    changed_capacity.client_nonce = uint256S("14");
    changed_quote.intent.client_nonce = changed_capacity.client_nonce;
    const ProviderSafetyStatus capacity_hash_mismatch =
        EvaluateProviderCapacityContinuationPreflight(
            ledger, safety, changed_capacity, changed_quote,
            RedactedQuoteRequestHash(changed_quote),
            changed_capacity.requires_carrier, recipient, DGBSatoshis{10},
            now, group);
    BOOST_CHECK(!capacity_hash_mismatch.can_accept_quote);
    BOOST_REQUIRE_EQUAL(capacity_hash_mismatch.errors.size(), 1U);
    BOOST_CHECK_EQUAL(capacity_hash_mismatch.errors.front(),
                      "PAYMASTER_CAPACITY_ADMISSION_CONFLICT");

    const ProviderSafetyStatus netgroup_mismatch =
        EvaluateProviderCapacityContinuationPreflight(
            ledger, safety, fixture.capacity_request,
            fixture.quote_request, quote_hash,
            fixture.capacity_request.requires_carrier, recipient,
            DGBSatoshis{10}, now, uint256S("f2"));
    BOOST_CHECK(!netgroup_mismatch.can_accept_quote);
    BOOST_REQUIRE_EQUAL(netgroup_mismatch.errors.size(), 1U);
    BOOST_CHECK_EQUAL(netgroup_mismatch.errors.front(),
                      "PAYMASTER_CAPACITY_ADMISSION_NETGROUP_CONFLICT");

    ProviderBudgetLedger promoted{ledger};
    BOOST_REQUIRE(BindProviderCapacityQuote(
        promoted, request_key, quote_hash, group, now, error));
    BOOST_REQUIRE(PromoteProviderCapacityAdmission(
        promoted, request_key, quote_hash, uint256S("d1"), now, error));
    const ProviderSafetyStatus no_longer_reserved =
        EvaluateProviderCapacityContinuationPreflight(
            promoted, safety, fixture.capacity_request,
            fixture.quote_request, quote_hash,
            fixture.capacity_request.requires_carrier, recipient,
            DGBSatoshis{10}, now, group);
    BOOST_CHECK(!no_longer_reserved.can_accept_quote);
    BOOST_REQUIRE_EQUAL(no_longer_reserved.errors.size(), 1U);
    BOOST_CHECK_EQUAL(no_longer_reserved.errors.front(),
                      "PAYMASTER_CAPACITY_ADMISSION_CONFLICT");
    BOOST_CHECK(promoted.capacity_admissions.front().state ==
                CapacityAdmissionState::PROMOTED);
}

BOOST_AUTO_TEST_CASE(capacity_continuation_preflight_preserves_economic_limits)
{
    constexpr int64_t now{5001};
    const CapacityContinuationFixture fixture{CapacityContinuation(now)};
    ProviderSafetyPolicy admission_safety = SafetyPolicy(SafetyLimits());
    admission_safety.maximum_active_quotes_total = 10;
    admission_safety.maximum_active_quotes_per_netgroup = 10;
    admission_safety.maximum_active_quotes_per_recipient = 10;
    ProviderBudgetLedger base = ProviderLedger(1000);
    const uint256 group{uint256S("f1")};
    const uint256 request_key{GetProviderRequestSlotKey(
        fixture.capacity_request.provider_id,
        fixture.capacity_request.request_id,
        fixture.capacity_request.session_id)};
    const uint256 request_hash{
        SerializedPaymasterHash(fixture.capacity_request)};
    const uint256 quote_hash{
        RedactedQuoteRequestHash(fixture.quote_request)};
    const uint256 recipient{GetRecipientBudgetBucket(
        base, fixture.quote_request.intent.recipient_script)};
    std::string error;

    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        base, admission_safety, request_key, request_hash, group,
        fixture.capacity_request.funding_model,
        fixture.capacity_request.requires_carrier,
        fixture.capacity_request.expires_at, now - 1, error));

    const auto evaluate = [&](const ProviderBudgetLedger& ledger,
                              const ProviderSafetyPolicy& safety,
                              DGBSatoshis fee = DGBSatoshis{10}) {
        return EvaluateProviderCapacityContinuationPreflight(
            ledger, safety, fixture.capacity_request,
            fixture.quote_request, quote_hash,
            fixture.capacity_request.requires_carrier, recipient, fee, now,
            group);
    };
    const auto check_exhausted = [](const ProviderSafetyStatus& status) {
        BOOST_CHECK(!status.can_accept_quote);
        BOOST_REQUIRE_EQUAL(status.errors.size(), 1U);
        BOOST_CHECK_EQUAL(status.errors.front(),
                          "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    };

    check_exhausted(evaluate(
        base, SafetyPolicy(SafetyLimits(/*per_transaction=*/100)),
        DGBSatoshis{101}));

    ProviderBudgetLedger rolling = ProviderLedger(1000);
    BOOST_REQUIRE(DigiDollar::Paymaster::ReserveProviderBudget(
        rolling, admission_safety, FundingModel::USER_PAID,
        SponsorshipScope::PUBLIC, uint256S("d1"), DGBSatoshis{95},
        uint256S("e1"), /*now=*/1000, error, uint256S("f2")));
    BOOST_REQUIRE(SpendProviderBudget(
        rolling, uint256S("d1"), /*now=*/1000, error));
    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        rolling, admission_safety, request_key, request_hash, group,
        fixture.capacity_request.funding_model,
        fixture.capacity_request.requires_carrier,
        fixture.capacity_request.expires_at, now - 1, error));

    // The historical spend is outside the hourly window but still in the day.
    check_exhausted(evaluate(
        rolling,
        SafetyPolicy(SafetyLimits(/*per_transaction=*/100,
                                  /*reserved=*/1000,
                                  /*per_hour=*/100,
                                  /*per_day=*/100))));

    ProviderBudgetLedger recent_spend{base};
    BOOST_REQUIRE(DigiDollar::Paymaster::ReserveProviderBudget(
        recent_spend, admission_safety, FundingModel::USER_PAID,
        SponsorshipScope::PUBLIC, uint256S("d2"), DGBSatoshis{95},
        uint256S("e2"), now - 1, error, uint256S("f2")));
    BOOST_REQUIRE(SpendProviderBudget(
        recent_spend, uint256S("d2"), now - 1, error));
    check_exhausted(evaluate(
        recent_spend,
        SafetyPolicy(SafetyLimits(/*per_transaction=*/100,
                                  /*reserved=*/1000,
                                  /*per_hour=*/100,
                                  /*per_day=*/1000))));

    ProviderBudgetLedger same_recipient{base};
    BOOST_REQUIRE(DigiDollar::Paymaster::ReserveProviderBudget(
        same_recipient, admission_safety, FundingModel::USER_PAID,
        SponsorshipScope::PUBLIC, uint256S("d3"), DGBSatoshis{1},
        recipient, now - 1, error, uint256S("f2")));
    ProviderSafetyPolicy recipient_limited =
        SafetyPolicy(SafetyLimits());
    recipient_limited.maximum_active_quotes_per_recipient = 1;
    check_exhausted(evaluate(same_recipient, recipient_limited));

    ProviderBudgetLedger other_capacity{base};
    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        other_capacity, admission_safety, uint256S("a2"), uint256S("b2"),
        uint256S("f2"), FundingModel::USER_PAID,
        /*requires_carrier=*/true, fixture.capacity_request.expires_at,
        now - 1, error));
    ProviderSafetyPolicy quote_limited = SafetyPolicy(SafetyLimits());
    quote_limited.maximum_active_quotes_total = 1;
    quote_limited.maximum_active_quotes_per_netgroup = 1;
    quote_limited.maximum_active_quotes_per_recipient = 1;
    check_exhausted(evaluate(other_capacity, quote_limited));
}

BOOST_AUTO_TEST_CASE(capacity_admission_release_and_clock_rollback_fail_closed)
{
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    ProviderBudgetLedger ledger = ProviderLedger(2000);
    const uint256 request_key{uint256S("a1")};
    const uint256 request_hash{uint256S("b1")};
    const uint256 group{uint256S("f1")};
    std::string error;

    BOOST_CHECK(!ReserveProviderCapacityAdmission(
        ledger, safety, request_key, request_hash, group,
        FundingModel::USER_PAID, /*requires_carrier=*/false,
        /*expires_at=*/1999, /*now=*/1940, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_ADMISSION_EXPIRED");
    BOOST_CHECK(ledger.capacity_admissions.empty());

    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        ledger, safety, request_key, request_hash, group,
        FundingModel::USER_PAID, /*requires_carrier=*/false,
        /*expires_at=*/2059, /*now=*/1999, error));
    BOOST_CHECK_EQUAL(ledger.capacity_admissions.front().admitted_at, 2000);
    BOOST_REQUIRE(ReleaseProviderCapacityAdmission(
        ledger, request_key, request_hash, /*now=*/2, error));
    BOOST_CHECK(ReleaseProviderCapacityAdmission(
        ledger, request_key, request_hash, /*now=*/3, error));
    BOOST_CHECK_EQUAL(ledger.capacity_admissions.front().updated_at, 2000);
    BOOST_CHECK(!BindProviderCapacityQuote(
        ledger, request_key, uint256S("c1"), group, 2001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_ADMISSION_EXPIRED");
    BOOST_CHECK(ValidateProviderBudgetLedger(ledger, error));

    ledger.capacity_admissions.front().state = CapacityAdmissionState::PROMOTED;
    BOOST_CHECK(!ValidateProviderBudgetLedger(ledger, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_ADMISSION");
}

BOOST_AUTO_TEST_CASE(capacity_admission_prunes_full_stale_ledger_before_limit)
{
    static constexpr size_t CAPACITY_EVENT_LIMIT{8192};
    ProviderSafetyPolicy safety = SafetyPolicy(SafetyLimits());
    ProviderBudgetLedger stale = ProviderLedger(100000);
    stale.capacity_admissions.reserve(CAPACITY_EVENT_LIMIT);
    for (size_t index = 0; index < CAPACITY_EVENT_LIMIT; ++index) {
        ProviderCapacityAdmission admission;
        admission.request_key = uint256S(strprintf(
            "%064llx", static_cast<unsigned long long>(index + 1)));
        admission.request_hash = uint256S("a1");
        admission.netgroup_bucket = uint256S("b1");
        admission.funding_model = FundingModel::USER_PAID;
        admission.state = CapacityAdmissionState::RELEASED;
        admission.admitted_at = 1;
        admission.expires_at = 61;
        admission.updated_at = 1;
        stale.capacity_admissions.push_back(std::move(admission));
    }
    std::string error;
    BOOST_REQUIRE(ValidateProviderBudgetLedger(stale, error));
    BOOST_REQUIRE(ReserveProviderCapacityAdmission(
        stale, safety, uint256S("ffff"), uint256S("eeee"),
        uint256S("dddd"), FundingModel::USER_PAID,
        /*requires_carrier=*/false, /*expires_at=*/100060,
        /*now=*/100000, error));
    BOOST_CHECK_EQUAL(stale.capacity_admissions.size(), 1U);
    BOOST_CHECK_EQUAL(stale.capacity_admissions.front().request_key,
                      uint256S("ffff"));

    ProviderBudgetLedger live = ProviderLedger(100000);
    live.capacity_admissions.reserve(CAPACITY_EVENT_LIMIT);
    for (size_t index = 0; index < CAPACITY_EVENT_LIMIT; ++index) {
        ProviderCapacityAdmission admission;
        admission.request_key = uint256S(strprintf(
            "%064llx", static_cast<unsigned long long>(index + 1)));
        admission.request_hash = uint256S("a1");
        admission.netgroup_bucket = admission.request_key;
        admission.funding_model = FundingModel::USER_PAID;
        admission.state = CapacityAdmissionState::RESERVED;
        admission.admitted_at = 99990;
        admission.expires_at = 100050;
        admission.updated_at = 99990;
        live.capacity_admissions.push_back(std::move(admission));
    }
    BOOST_REQUIRE(ValidateProviderBudgetLedger(live, error));
    const int64_t original_high_water = live.accounting_time_high_water;
    BOOST_CHECK(!ReserveProviderCapacityAdmission(
        live, safety, uint256S("ffff"), uint256S("eeee"),
        uint256S("dddd"), FundingModel::USER_PAID,
        /*requires_carrier=*/false, /*expires_at=*/100060,
        /*now=*/100000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    BOOST_CHECK_EQUAL(live.capacity_admissions.size(),
                      CAPACITY_EVENT_LIMIT);
    BOOST_CHECK_EQUAL(live.accounting_time_high_water,
                      original_high_water);
}

BOOST_AUTO_TEST_CASE(provider_finance_events_are_idempotent_and_reorg_safe)
{
    ProviderFinanceLedger ledger;
    ledger.genesis_hash = uint256S("01");
    ledger.provider_id = uint256S("02");
    ledger.history_complete_from = 100;
    ledger.updated_at = 100;

    ProviderFinanceEvent event;
    event.event_id = uint256S("03");
    event.genesis_hash = ledger.genesis_hash;
    event.provider_id = ledger.provider_id;
    event.kind = ProviderFinanceEventKind::TRANSFER;
    event.state = ProviderFinanceEventState::CONFIRMED;
    event.funding_model = FundingModel::USER_PAID;
    event.transaction_id = uint256S("04");
    event.dd_income = DDCents{25};
    event.dgb_cost = DGBSatoshis{10};
    event.created_at = 100;
    event.confirmed_at = 200;
    event.updated_at = 200;

    std::string error;
    BOOST_REQUIRE(UpsertProviderFinanceEvent(ledger, event, error));
    BOOST_REQUIRE(ValidateProviderFinanceLedger(ledger, error));
    BOOST_REQUIRE_EQUAL(ledger.events.size(), 1U);
    BOOST_REQUIRE_EQUAL(ledger.daily_totals.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.daily_totals.front().dd_income.value, 25);
    BOOST_CHECK_EQUAL(ledger.daily_totals.front().dgb_cost.value, 10);
    BOOST_CHECK_EQUAL(ledger.daily_totals.front().successful_transfers, 1U);
    BOOST_CHECK_EQUAL(ledger.daily_totals.front().user_paid_transfers, 1U);

    // An exact replay replaces no economic data and cannot double-count it.
    BOOST_REQUIRE(UpsertProviderFinanceEvent(ledger, event, error));
    BOOST_CHECK_EQUAL(ledger.events.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.daily_totals.front().dd_income.value, 25);

    // A delayed pre-confirmation snapshot is an idempotent no-op. It must not
    // roll a newer confirmed event back to pending or remove its daily total.
    ProviderFinanceEvent stale{event};
    stale.state = ProviderFinanceEventState::PENDING;
    stale.confirmed_at = 0;
    stale.updated_at = 150;
    BOOST_REQUIRE(UpsertProviderFinanceEvent(ledger, stale, error));
    BOOST_CHECK(ledger.events.front().state ==
                ProviderFinanceEventState::CONFIRMED);
    BOOST_CHECK_EQUAL(ledger.daily_totals.front().dd_income.value, 25);

    ProviderFinanceLedger duplicate{ledger};
    duplicate.events.push_back(duplicate.events.front());
    BOOST_CHECK(!ValidateProviderFinanceLedger(duplicate, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DUPLICATE_FINANCE_EVENT");

    ProviderFinanceLedger stale_ledger_time{ledger};
    stale_ledger_time.updated_at = 199;
    BOOST_CHECK(!ValidateProviderFinanceLedger(stale_ledger_time, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_FINANCE_LEDGER_TIME");

    // A reorg moves the same event back to pending and removes it from the
    // confirmed result until it is included again.
    event.state = ProviderFinanceEventState::PENDING;
    event.confirmed_at = 0;
    event.updated_at = 300;
    BOOST_REQUIRE(UpsertProviderFinanceEvent(ledger, event, error));
    BOOST_CHECK(ledger.daily_totals.empty());
    BOOST_REQUIRE(ValidateProviderFinanceLedger(ledger, error));

    event.state = ProviderFinanceEventState::CONFIRMED;
    event.confirmed_at = 400;
    event.updated_at = 400;
    BOOST_REQUIRE(UpsertProviderFinanceEvent(ledger, event, error));
    BOOST_REQUIRE_EQUAL(ledger.daily_totals.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.daily_totals.front().dd_income.value, 25);

    ProviderFinanceEvent conflict{event};
    conflict.dd_income = DDCents{26};
    BOOST_CHECK(!UpsertProviderFinanceEvent(ledger, conflict, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINANCE_EVENT_CONFLICT");
    BOOST_CHECK_EQUAL(ledger.events.front().dd_income.value, 25);
}

BOOST_AUTO_TEST_CASE(provider_finance_totals_keep_models_and_capital_separate)
{
    ProviderFinanceLedger ledger;
    ledger.genesis_hash = uint256S("11");
    ledger.provider_id = uint256S("12");
    ledger.history_complete_from = 100;
    ledger.updated_at = 100;

    const auto add_event = [&](uint64_t id,
                               ProviderFinanceEventKind kind,
                               FundingModel model,
                               SponsorshipScope scope,
                               int64_t income,
                               int64_t cost) {
        ProviderFinanceEvent event;
        event.event_id = uint256S(strprintf(
            "%064llx", static_cast<unsigned long long>(id)));
        event.genesis_hash = ledger.genesis_hash;
        event.provider_id = ledger.provider_id;
        event.kind = kind;
        event.state = ProviderFinanceEventState::CONFIRMED;
        event.funding_model = model;
        event.sponsorship_scope = scope;
        event.transaction_id = uint256S(strprintf(
            "%064llx", static_cast<unsigned long long>(id + 100)));
        event.dd_income = DDCents{income};
        event.dgb_cost = DGBSatoshis{cost};
        event.created_at = 100 + id;
        event.confirmed_at = 200 + id;
        event.updated_at = 200 + id;
        std::string error;
        return UpsertProviderFinanceEvent(ledger, event, error);
    };

    BOOST_REQUIRE(add_event(1, ProviderFinanceEventKind::TRANSFER,
                            FundingModel::USER_PAID,
                            SponsorshipScope::PUBLIC, 30, 5));
    BOOST_REQUIRE(add_event(2, ProviderFinanceEventKind::TRANSFER,
                            FundingModel::SPONSORED,
                            SponsorshipScope::PUBLIC, 0, 7));
    BOOST_REQUIRE(add_event(3, ProviderFinanceEventKind::TRANSFER,
                            FundingModel::SPONSORED,
                            SponsorshipScope::RESTRICTED, 0, 11));
    BOOST_REQUIRE(add_event(4, ProviderFinanceEventKind::POOL_SETUP,
                            FundingModel::USER_PAID,
                            SponsorshipScope::PUBLIC, 0, 13));

    std::string error;
    BOOST_REQUIRE(ValidateProviderFinanceLedger(ledger, error));
    BOOST_REQUIRE_EQUAL(ledger.daily_totals.size(), 1U);
    const ProviderFinanceDailyTotals& totals = ledger.daily_totals.front();
    BOOST_CHECK_EQUAL(totals.dd_income.value, 30);
    BOOST_CHECK_EQUAL(totals.dgb_cost.value, 36);
    BOOST_CHECK_EQUAL(totals.successful_transfers, 3U);
    BOOST_CHECK_EQUAL(totals.user_paid_transfers, 1U);
    BOOST_CHECK_EQUAL(totals.public_sponsored_transfers, 1U);
    BOOST_CHECK_EQUAL(totals.restricted_sponsored_transfers, 1U);
    BOOST_CHECK_EQUAL(totals.maintenance_transactions, 1U);

    // Pool principal never appears in finance events. Only the actual native
    // transaction cost above contributes to the provider result.
    const ProviderPoolEntry capital{
        PoolEntry(50, PoolPurpose::OPERATIONAL, PoolAsset::DGB, 5000000)};
    BOOST_CHECK_EQUAL(capital.dgb_value.value, 5000000);
    BOOST_CHECK_EQUAL(totals.dgb_cost.value, 36);
}

BOOST_AUTO_TEST_CASE(provider_backup_status_requires_a_fresh_acknowledgement)
{
    ProviderBackupStatus status;
    status.genesis_hash = uint256S("20");
    status.provider_id = uint256S("21");
    status.identity_created_at = 100;
    status.reminder_updated_at = 100;

    std::string error;
    BOOST_REQUIRE(ValidateProviderBackupStatus(status, error));
    BOOST_CHECK(ProviderBackupRequired(status));

    status.last_successful_backup_at = 100;
    BOOST_CHECK(!ProviderBackupRequired(status));

    // A material configuration change advances the reminder and makes an old
    // backup insufficient without ever retaining its filesystem path.
    status.reminder_updated_at = 200;
    BOOST_REQUIRE(ValidateProviderBackupStatus(status, error));
    BOOST_CHECK(ProviderBackupRequired(status));
    status.external_backup_acknowledged_at = 200;
    BOOST_CHECK(!ProviderBackupRequired(status));

    ProviderBackupStatus wrong_identity{status};
    wrong_identity.provider_id.SetNull();
    BOOST_CHECK(!ValidateProviderBackupStatus(wrong_identity, error));
    ProviderBackupStatus wrong_chain{status};
    wrong_chain.genesis_hash.SetNull();
    BOOST_CHECK(!ValidateProviderBackupStatus(wrong_chain, error));
}

BOOST_AUTO_TEST_CASE(pool_preparation_authorization_and_legacy_encoding)
{
    std::string error;
    ProviderMaintenanceLedger ledger;
    auto record = MaintenanceRecord(uint256S("91"), uint256S("92"), 91, 100);
    record.created_at = record.updated_at = 1000;
    record.kind = ProviderMaintenanceKind::PREPARE_DGB;
    ledger.records = {record};
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(ledger, error));
    record.preparation_authorization = uint256S("93");
    record.preparation_request = uint256S("99");
    ledger.records = {record};
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(ledger, error));
    CDataStream encoded{SER_NETWORK, ::PROTOCOL_VERSION};
    encoded << ledger;
    ProviderMaintenanceLedger decoded;
    encoded >> decoded;
    BOOST_REQUIRE(encoded.empty());
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(decoded, error));
    BOOST_CHECK(decoded.records.front().IsPreparation());
    BOOST_CHECK(decoded.records.front().preparation_authorization == record.preparation_authorization);

    // Authentic V3 bytes omit the new fields and cannot gain setup authority.
    record.kind = ProviderMaintenanceKind::REPLENISH_DGB;
    record.version = 3;
    record.preparation_authorization.SetNull();
    record.preparation_request.SetNull();
    ledger.records = {record};
    encoded << ledger;
    encoded >> decoded;
    BOOST_REQUIRE(encoded.empty());
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(decoded, error));
    BOOST_CHECK(!decoded.records.front().IsPreparation());
    BOOST_CHECK(decoded.records.front().preparation_authorization.IsNull());
    decoded.records.front().kind = ProviderMaintenanceKind::PREPARE_DGB;
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(decoded, error));
    decoded.records.front().version = ProviderMaintenanceRecord::CURRENT_VERSION + 1;
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(decoded, error));
}

BOOST_AUTO_TEST_CASE(pool_preparation_preserves_finite_fee_and_asset_boundaries)
{
    std::string error;
    auto record = MaintenanceRecord(uint256S("94"), uint256S("95"), 94, 100);
    record.created_at = record.updated_at = 1000;
    record.kind = ProviderMaintenanceKind::PREPARE_DGB;
    record.preparation_authorization = uint256S("96");
    record.preparation_request = uint256S("99");
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    BOOST_REQUIRE(ValidateProviderMaintenanceLedger(ledger, error));
    ledger.records.front().actual_fee = DGBSatoshis{101};
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(ledger, error));
    ledger.records = {record};
    ledger.records.front().outputs.front() = MaintenanceOutput(94, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER);
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(ledger, error));
    ledger.records = {record};
    ledger.records.front().outputs.front().dgb_value = DGBSatoshis{MAX_MONEY};
    ledger.records.front().outputs.push_back(MaintenanceOutput(95));
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(ledger, error));
    ledger.records = {record};
    ledger.records.front().preparation_error.assign(257, 'x');
    BOOST_CHECK(!ValidateProviderMaintenanceLedger(ledger, error));

    // Explicit one-shot setup fees do not consume or authorize recurring fees.
    ledger.records = {record};
    const auto policy = LiquidityPolicy();
    auto recurring = MaintenanceRecord(uint256S("97"), uint256S("98"), 97, 100);
    BOOST_REQUIRE(ReserveProviderMaintenanceBudget(ledger, policy, recurring, 1000, error));
    BOOST_CHECK_EQUAL(ledger.records.size(), 2U);
    ProviderMaintenanceLedger empty;
    BOOST_CHECK(!ReserveProviderMaintenanceBudget(empty, policy, record, 1000, error));
    BOOST_CHECK(empty.records.empty());
}

BOOST_AUTO_TEST_SUITE_END()
