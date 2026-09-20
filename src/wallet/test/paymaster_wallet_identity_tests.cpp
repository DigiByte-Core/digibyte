// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Persistent provider-identity ownership and signing tests. */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <hash.h>
#include <key.h>
#include <paymaster/directory.h>
#include <script/standard.h>
#include <streams.h>
#include <util/time.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/paymaster_internal.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>

namespace wallet {
using namespace DigiDollar::Paymaster;

namespace {
class ScopedPaymasterMockTime
{
public:
    explicit ScopedPaymasterMockTime(int64_t now)
        : m_previous{GetMockTime()}
    {
        SetMockTime(now);
    }

    ~ScopedPaymasterMockTime()
    {
        SetMockTime(m_previous);
    }

private:
    const std::chrono::seconds m_previous;
};

/** Authentic shortened provider-settings v1 disk shape. */
struct ProviderSettingsV1 {
    uint16_t version{1};
    bool enabled{false};
    uint256 policy_hash;
    int64_t updated_at{0};

    SERIALIZE_METHODS(ProviderSettingsV1, obj)
    {
        READWRITE(obj.version, obj.enabled, obj.policy_hash, obj.updated_at);
    }
};

/** Raw current-version shape used to prove that an out-of-range mode fails
 * closed while reading the wallet database. ProviderSettings itself refuses
 * to serialize such a value. */
struct RawProviderSettingsV2 {
    uint16_t version{ProviderSettings::CURRENT_VERSION};
    bool enabled{false};
    uint256 policy_hash;
    int64_t updated_at{0};
    uint8_t operation_mode{0};
    bool autostart{false};

    SERIALIZE_METHODS(RawProviderSettingsV2, obj)
    {
        READWRITE(obj.version, obj.enabled, obj.policy_hash, obj.updated_at,
                  obj.operation_mode, obj.autostart);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_wallet_identity_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(paymaster_dd_reservations_are_owned_but_not_spendable)
{
    m_wallet.EnsureDDWallet();
    DigiDollarWallet* const dd_wallet = m_wallet.GetDDWallet();
    BOOST_REQUIRE(dd_wallet != nullptr);

    const COutPoint ordinary{uint256::ONE, 0};
    const COutPoint provider_carrier{uint256S("02"), 0};
    const COutPoint client_input{uint256S("03"), 0};
    dd_wallet->AddDDUTXO(ordinary, 9000);
    dd_wallet->AddDDUTXO(provider_carrier, 400);
    dd_wallet->AddDDUTXO(client_input, 600);

    ProviderPoolEntry carrier_entry;
    carrier_entry.outpoint = provider_carrier;
    carrier_entry.purpose = PoolPurpose::OPERATIONAL;
    carrier_entry.asset = PoolAsset::DD_CARRIER;
    carrier_entry.state = PoolEntryState::AVAILABLE;
    carrier_entry.carrier_value = DDCents{400};
    CKey carrier_key;
    carrier_key.MakeNewKey(true);
    TaprootBuilder carrier_builder;
    carrier_builder.Finalize(XOnlyPubKey{carrier_key.GetPubKey()});
    carrier_entry.script_pub_key =
        GetScriptForDestination(carrier_builder.GetOutput());
    carrier_entry.confirmation_height = 1;
    carrier_entry.updated_at = 1;

    InputReservation client_reservation;
    client_reservation.outpoint = client_input;
    client_reservation.request_id = "550e8400-e29b-41d4-a716-446655440099";
    client_reservation.session_id = uint256S("04");
    client_reservation.role = ReservationRole::USER_DD;
    client_reservation.created_at = 1;

    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterProviderPool({carrier_entry}));
    BOOST_REQUIRE(batch.WritePaymasterReservation(client_reservation));

    const DigiDollarBalanceSummary reserved = dd_wallet->GetDDBalanceSummary();
    BOOST_CHECK_EQUAL(reserved.confirmed_total, 10000);
    BOOST_CHECK_EQUAL(reserved.spendable, 9000);
    BOOST_CHECK_EQUAL(reserved.paymaster_reserved, 1000);
    BOOST_CHECK_EQUAL(dd_wallet->GetTotalDDBalance(), 10000);
    BOOST_CHECK_EQUAL(dd_wallet->GetSpendableDDBalance(), 9000);

    const std::vector<DDUtxo> selectable = dd_wallet->GetDDUTXOs();
    BOOST_REQUIRE_EQUAL(selectable.size(), 1U);
    BOOST_CHECK(selectable.front().outpoint == ordinary);

    // Once both durable locks are gone, the exact same wallet-owned outputs
    // become ordinary spendable balance again.
    BOOST_REQUIRE(batch.ErasePaymasterReservation(client_input));
    carrier_entry.state = PoolEntryState::SPENT;
    BOOST_REQUIRE(batch.WritePaymasterProviderPool({carrier_entry}));
    const DigiDollarBalanceSummary released = dd_wallet->GetDDBalanceSummary();
    BOOST_CHECK_EQUAL(released.confirmed_total, 10000);
    BOOST_CHECK_EQUAL(released.spendable, 10000);
    BOOST_CHECK_EQUAL(released.paymaster_reserved, 0);
}

BOOST_AUTO_TEST_CASE(unreadable_paymaster_locks_fail_closed_for_coin_selection)
{
    const COutPoint reserved_input{uint256S("05"), 0};
    InputReservation reservation;
    reservation.version = 0;
    reservation.outpoint = reserved_input;
    reservation.request_id = "550e8400-e29b-41d4-a716-446655440098";
    reservation.session_id = uint256S("06");
    reservation.created_at = 1;
    {
        auto batch = m_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(batch->Write(
            std::make_pair(DBKeys::PAYMASTER_RESERVATION, reserved_input),
            reservation));
    }
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, reserved_input));

    ProviderPoolEntry outdated_pool_entry;
    outdated_pool_entry.version = 1;
    outdated_pool_entry.outpoint = COutPoint{uint256S("07"), 0};
    outdated_pool_entry.state = PoolEntryState::AVAILABLE;
    {
        auto batch = m_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(batch->Write(
            DBKeys::PAYMASTER_PROVIDER_POOL,
            std::vector<ProviderPoolEntry>{outdated_pool_entry}));
    }
    std::set<COutPoint> pool_inputs;
    BOOST_CHECK(GetPaymasterProviderPoolInputs(m_wallet, pool_inputs) ==
                DatabaseReadStatus::UNSUPPORTED_VERSION);
    BOOST_CHECK(pool_inputs.empty());
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet,
                                         outdated_pool_entry.outpoint));
}

BOOST_AUTO_TEST_CASE(legacy_wallet_is_rejected)
{
    ProviderIdentityRecord identity;
    std::string error;
    BOOST_CHECK(!CreatePaymasterIdentity(m_wallet, "Provider", 100, identity, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_REQUIRES_DESCRIPTOR_WALLET");
}

BOOST_AUTO_TEST_CASE(provider_runtime_settings_are_current_only_and_fail_closed)
{
    constexpr int64_t base_time{100};
    const uint256 policy_hash{uint256S("01")};
    {
        ProviderSettingsV1 outdated;
        outdated.enabled = true;
        outdated.policy_hash = policy_hash;
        outdated.updated_at = base_time;
        auto batch = m_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(batch->Write(DBKeys::PAYMASTER_SETTINGS, outdated));
    }

    ProviderSettings settings;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_CHECK(batch.ReadPaymasterSettingsWithStatus(settings) ==
                    DatabaseReadStatus::UNSUPPORTED_VERSION);
        BOOST_CHECK_EQUAL(settings.version, 1U);
    }
    BOOST_CHECK(!GetPaymasterProviderSettings(m_wallet, settings));

    std::string error;
    const auto before_outdated =
        GetMockableDatabase(m_wallet).m_records;
    BOOST_CHECK(!SetPaymasterProviderRuntimeSettings(
        m_wallet, ProviderOperationMode::MANUAL, true,
        base_time + 1, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=ProviderSettings found=1 expected=2");
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == before_outdated);
    BOOST_CHECK(!SetPaymasterProviderEnabled(
        m_wallet, false, base_time + 1, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=ProviderSettings found=1 expected=2");
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == before_outdated);

    settings = {};
    settings.enabled = false;
    settings.policy_hash = policy_hash;
    settings.updated_at = base_time;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .WritePaymasterSettings(settings));
    }
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, settings));

    BOOST_CHECK(!SetPaymasterProviderRuntimeSettings(
        m_wallet, static_cast<ProviderOperationMode>(255), true,
        base_time + 1, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_OPERATION_MODE");
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, settings));
    BOOST_CHECK(settings.operation_mode == ProviderOperationMode::AUTOMATIC);
    BOOST_CHECK(!settings.autostart);
    BOOST_CHECK_EQUAL(settings.updated_at, base_time);

    BOOST_REQUIRE_MESSAGE(SetPaymasterProviderRuntimeSettings(
                              m_wallet, ProviderOperationMode::MANUAL, true,
                              base_time + 2, error),
                          error);
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, settings));
    BOOST_CHECK(settings.operation_mode == ProviderOperationMode::MANUAL);
    BOOST_CHECK(settings.autostart);
    BOOST_CHECK_EQUAL(settings.updated_at, base_time + 2);

    BOOST_REQUIRE_MESSAGE(SetPaymasterProviderRuntimeSettings(
                              m_wallet, ProviderOperationMode::AUTOMATIC,
                              false, base_time + 3, error),
                          error);
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, settings));
    BOOST_CHECK(settings.operation_mode == ProviderOperationMode::AUTOMATIC);
    BOOST_CHECK(!settings.autostart);
    BOOST_CHECK_EQUAL(settings.updated_at, base_time + 3);

    {
        RawProviderSettingsV2 malformed;
        malformed.enabled = true;
        malformed.policy_hash = policy_hash;
        malformed.updated_at = base_time + 4;
        malformed.operation_mode = 2;
        malformed.autostart = true;
        auto batch = m_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(batch->Write(DBKeys::PAYMASTER_SETTINGS, malformed));
    }
    BOOST_CHECK(!GetPaymasterProviderSettings(m_wallet, settings));
}

BOOST_AUTO_TEST_CASE(descriptor_wallet_identity_uses_untweaked_bip86_key)
{
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetupDescriptorScriptPubKeyMans();
    }

    ProviderIdentityRecord identity;
    std::string error;
    BOOST_REQUIRE(CreatePaymasterIdentity(m_wallet, "Community Provider", 100, identity, error));
    BOOST_CHECK_EQUAL(identity.provider_id, GetPaymasterId(identity.identity_key));
    BOOST_CHECK_EQUAL(identity.display_name, "Community Provider");
    CTxDestination destination;
    BOOST_REQUIRE(ExtractDestination(identity.identity_script, destination));
    BOOST_CHECK(std::holds_alternative<WitnessV1Taproot>(destination));

    ProviderIdentityRecord persisted;
    BOOST_REQUIRE(GetPaymasterIdentity(m_wallet, persisted));
    BOOST_CHECK_EQUAL(persisted.provider_id, identity.provider_id);
    CKey key;
    BOOST_REQUIRE(GetPaymasterIdentityKey(m_wallet, key, persisted, error));
    BOOST_CHECK(XOnlyPubKey{key.GetPubKey()} == identity.identity_key);

    ProviderIdentityRecord replay;
    BOOST_REQUIRE(CreatePaymasterIdentity(m_wallet, "Ignored replacement", 101, replay, error));
    BOOST_CHECK_EQUAL(replay.provider_id, identity.provider_id);
    BOOST_CHECK_EQUAL(replay.display_name, identity.display_name);
}

BOOST_AUTO_TEST_CASE(provider_finance_and_backup_metadata_follow_the_identity)
{
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetupDescriptorScriptPubKeyMans();
    }

    ProviderIdentityRecord identity;
    std::string error;
    BOOST_REQUIRE(CreatePaymasterIdentity(
        m_wallet, "Finance Provider", 100, identity, error));

    ProviderFinanceLedger finance;
    ProviderBackupStatus backup_status;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterFinanceLedger(finance));
        BOOST_REQUIRE(batch.ReadPaymasterBackupStatus(backup_status));
    }
    BOOST_CHECK_EQUAL(finance.provider_id, identity.provider_id);
    BOOST_CHECK_EQUAL(finance.genesis_hash, Params().GenesisBlock().GetHash());
    BOOST_CHECK_EQUAL(finance.history_complete_from, 100);
    BOOST_CHECK(finance.events.empty());
    BOOST_CHECK_EQUAL(backup_status.genesis_hash,
                      Params().GenesisBlock().GetHash());
    BOOST_CHECK_EQUAL(backup_status.provider_id, identity.provider_id);
    BOOST_CHECK(ProviderBackupRequired(backup_status));

    ProviderFinanceEvent event;
    event.event_id = uint256S("31");
    event.genesis_hash = finance.genesis_hash;
    event.provider_id = identity.provider_id;
    event.kind = ProviderFinanceEventKind::TRANSFER;
    event.state = ProviderFinanceEventState::CONFIRMED;
    event.funding_model = FundingModel::USER_PAID;
    event.transaction_id = uint256S("32");
    event.dd_income = DDCents{3};
    event.dgb_cost = DGBSatoshis{7};
    event.created_at = 101;
    event.confirmed_at = 102;
    event.updated_at = 102;
    BOOST_REQUIRE(UpsertProviderFinanceEvent(finance, event, error));
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterFinanceLedger(finance));

        // Store representative operator configuration and pool state before
        // taking the database snapshot. This proves that a full-wallet copy
        // preserves more than the public provider id and accounting totals.
        ProviderPolicy policy;
        policy.funding_models = FUNDING_MODEL_USER_PAID;
        policy.fee_rate_bps = 40;
        policy.min_payment = DDCents{100};
        policy.max_payment = DDCents{100000};
        policy.maximum_network_fee = DGBSatoshis{1000};
        BOOST_REQUIRE(batch.WritePaymasterPolicy(policy));
        ProviderSettings settings;
        settings.enabled = true;
        settings.policy_hash = GetProviderPolicyHash(policy);
        settings.updated_at = 103;
        BOOST_REQUIRE(batch.WritePaymasterSettings(settings));
        FundingSafetyLimits limits;
        limits.maximum_network_fee_per_transaction = DGBSatoshis{1000};
        limits.maximum_reserved_network_fee = DGBSatoshis{2000};
        limits.maximum_network_fee_per_hour = DGBSatoshis{5000};
        limits.maximum_network_fee_per_day = DGBSatoshis{10000};
        limits.maximum_completed_per_hour = 5;
        limits.maximum_completed_per_day = 25;
        ProviderSafetyPolicy safety;
        safety.user_paid = limits;
        safety.maximum_active_quotes_total = 16;
        safety.maximum_active_quotes_per_netgroup = 4;
        safety.maximum_active_quotes_per_recipient = 2;
        safety.maximum_quote_requests_per_netgroup_per_minute = 10;
        safety.updated_at = 103;
        BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(safety));
        ProviderLiquidityPolicy liquidity;
        liquidity.target_admission_carriers = 3;
        liquidity.target_operational_carriers = 1;
        liquidity.updated_at = 103;
        BOOST_REQUIRE(batch.WritePaymasterLiquidityPolicy(liquidity));
        ProviderPoolEntry pool_entry;
        pool_entry.outpoint = COutPoint{uint256S("33"), 0};
        pool_entry.purpose = PoolPurpose::ADMISSION;
        pool_entry.asset = PoolAsset::DGB;
        pool_entry.script_pub_key = identity.identity_script;
        pool_entry.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
        pool_entry.confirmation_height = 1;
        pool_entry.updated_at = 103;
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({pool_entry}));
    }

    // A full wallet-database copy carries identity and accounting records. It
    // is taken before the source marks backup completion, so restoring it will
    // deliberately request a fresh backup on the new system.
    auto backup_snapshot = DuplicateMockDatabase(m_wallet.GetDatabase());
    WalletBatch backup_batch{*backup_snapshot};
    ProviderIdentityRecord restored_identity;
    ProviderFinanceLedger restored_finance;
    ProviderBackupStatus restored_backup;
    ProviderPolicy restored_policy;
    ProviderSettings restored_settings;
    ProviderSafetyPolicy restored_safety;
    ProviderLiquidityPolicy restored_liquidity;
    std::vector<ProviderPoolEntry> restored_pool;
    BOOST_REQUIRE(backup_batch.ReadPaymasterIdentity(restored_identity));
    BOOST_REQUIRE(backup_batch.ReadPaymasterFinanceLedger(restored_finance));
    BOOST_REQUIRE(backup_batch.ReadPaymasterBackupStatus(restored_backup));
    BOOST_REQUIRE(backup_batch.ReadPaymasterPolicy(restored_policy));
    BOOST_REQUIRE(backup_batch.ReadPaymasterSettings(restored_settings));
    BOOST_REQUIRE(backup_batch.ReadPaymasterProviderSafetyPolicy(
        restored_safety));
    BOOST_REQUIRE(backup_batch.ReadPaymasterLiquidityPolicy(
        restored_liquidity));
    BOOST_REQUIRE(backup_batch.ReadPaymasterProviderPool(restored_pool));
    BOOST_CHECK_EQUAL(restored_identity.provider_id, identity.provider_id);
    BOOST_CHECK_EQUAL(restored_backup.genesis_hash,
                      Params().GenesisBlock().GetHash());
    BOOST_REQUIRE_EQUAL(restored_finance.events.size(), 1U);
    BOOST_CHECK_EQUAL(restored_finance.events.front().dd_income.value, 3);
    BOOST_CHECK_EQUAL(restored_settings.policy_hash,
                      GetProviderPolicyHash(restored_policy));
    BOOST_CHECK(restored_settings.enabled);
    BOOST_CHECK_EQUAL(
        restored_safety.user_paid.maximum_network_fee_per_transaction.value,
        1000);
    BOOST_CHECK_EQUAL(restored_liquidity.target_operational_carriers, 1);
    BOOST_REQUIRE_EQUAL(restored_pool.size(), 1U);
    BOOST_CHECK(restored_pool.front().outpoint == COutPoint(uint256S("33"), 0));
    BOOST_CHECK(ProviderBackupRequired(restored_backup));

    BOOST_REQUIRE(MarkPaymasterProviderBackupCompleted(m_wallet, 103, error));
    BOOST_REQUIRE(GetPaymasterProviderBackupStatus(
        m_wallet, backup_status, 103, error));
    BOOST_CHECK(!ProviderBackupRequired(backup_status));

    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_USER_PAID;
    policy.fee_rate_bps = 50;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{100000};
    policy.maximum_network_fee = DGBSatoshis{1000};
    BOOST_REQUIRE(SetPaymasterProviderPolicy(m_wallet, policy, 104, error));
    BOOST_REQUIRE(GetPaymasterProviderBackupStatus(
        m_wallet, backup_status, 104, error));
    BOOST_CHECK_EQUAL(backup_status.reminder_updated_at, 104);
    BOOST_CHECK(ProviderBackupRequired(backup_status));

    // Even a material change recorded in the same timestamp second as a
    // completed backup must request a new backup.
    BOOST_REQUIRE(MarkPaymasterProviderBackupCompleted(m_wallet, 104, error));
    policy.fee_rate_bps = 60;
    BOOST_REQUIRE(SetPaymasterProviderPolicy(m_wallet, policy, 104, error));
    BOOST_REQUIRE(GetPaymasterProviderBackupStatus(
        m_wallet, backup_status, 104, error));
    BOOST_CHECK_EQUAL(backup_status.reminder_updated_at, 105);
    BOOST_CHECK(ProviderBackupRequired(backup_status));

    BOOST_REQUIRE(AcknowledgePaymasterProviderExternalBackup(
        m_wallet, 105, error));
    BOOST_REQUIRE(GetPaymasterProviderBackupStatus(
        m_wallet, backup_status, 105, error));
    BOOST_CHECK(!ProviderBackupRequired(backup_status));
    BOOST_CHECK_EQUAL(backup_status.external_backup_acknowledged_at, 105);

    BOOST_REQUIRE(SetPaymasterProviderRuntimeSettings(
        m_wallet, ProviderOperationMode::MANUAL, true, 106, error));
    BOOST_REQUIRE(GetPaymasterProviderBackupStatus(
        m_wallet, backup_status, 106, error));
    BOOST_CHECK_EQUAL(backup_status.reminder_updated_at, 106);
    BOOST_CHECK(ProviderBackupRequired(backup_status));

    BOOST_REQUIRE(MarkPaymasterProviderBackupCompleted(m_wallet, 106, error));
    BOOST_REQUIRE(SetPaymasterProviderEnabled(m_wallet, false, 107, error));
    BOOST_REQUIRE(GetPaymasterProviderBackupStatus(
        m_wallet, backup_status, 107, error));
    BOOST_CHECK_EQUAL(backup_status.reminder_updated_at, 107);
    BOOST_CHECK(ProviderBackupRequired(backup_status));
}

BOOST_AUTO_TEST_CASE(watch_only_and_external_signer_wallets_are_rejected)
{
    std::string error;
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    }
    BOOST_CHECK(!CheckPaymasterProviderWallet(m_wallet, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_REQUIRES_PRIVATE_KEYS");
}

BOOST_AUTO_TEST_CASE(external_signer_wallet_is_rejected)
{
    std::string error;
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetWalletFlag(WALLET_FLAG_EXTERNAL_SIGNER);
    }
    BOOST_CHECK(!CheckPaymasterProviderWallet(m_wallet, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_REJECTS_EXTERNAL_SIGNER");
}

BOOST_AUTO_TEST_CASE(policy_and_enablement_are_atomically_bound)
{
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetupDescriptorScriptPubKeyMans();
    }
    ProviderIdentityRecord identity;
    std::string error;
    BOOST_REQUIRE(CreatePaymasterIdentity(m_wallet, "Provider", 100, identity, error));
    BOOST_CHECK(!SetPaymasterProviderEnabled(m_wallet, true, 101, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_POLICY_NOT_FOUND");

    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_ALL;
    policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    policy.fee_rate_bps = 50;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{100000};
    policy.maximum_network_fee = DGBSatoshis{20000000};
    BOOST_REQUIRE(SetPaymasterProviderPolicy(m_wallet, policy, 102, error));
    ProviderSettings settings;
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, settings));
    BOOST_CHECK_EQUAL(settings.policy_hash, GetProviderPolicyHash(policy));
    BOOST_CHECK(!settings.enabled);

    BOOST_CHECK(!SetPaymasterProviderEnabled(m_wallet, true, 103, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SAFETY_POLICY_NOT_FOUND");
    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{20000000};
    limits.maximum_reserved_network_fee = DGBSatoshis{40000000};
    limits.maximum_network_fee_per_hour = DGBSatoshis{200000000};
    limits.maximum_network_fee_per_day = DGBSatoshis{2000000000};
    limits.maximum_completed_per_hour = 10;
    limits.maximum_completed_per_day = 100;
    ProviderSafetyPolicy safety;
    safety.user_paid = limits;
    safety.public_sponsored = limits;
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 5;
    safety.maximum_active_quotes_per_recipient = 2;
    safety.maximum_quote_requests_per_netgroup_per_minute = 30;
    BOOST_REQUIRE(SetPaymasterProviderSafetyPolicy(m_wallet, safety, 103, error));

    BOOST_REQUIRE(SetPaymasterProviderEnabled(m_wallet, true, 104, error));
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, settings));
    BOOST_CHECK(settings.enabled);
    ProviderPolicy persisted;
    BOOST_REQUIRE(GetPaymasterProviderPolicy(m_wallet, persisted));
    BOOST_CHECK_EQUAL(GetProviderPolicyHash(persisted), settings.policy_hash);

    std::vector<ProviderPoolEntry> pool;
    for (uint32_t index = 0; index < 3; ++index) {
        ProviderPoolEntry dgb;
        dgb.outpoint = COutPoint{uint256::ONE, index};
        dgb.purpose = PoolPurpose::ADMISSION;
        dgb.asset = PoolAsset::DGB;
        dgb.script_pub_key = identity.identity_script;
        dgb.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
        dgb.confirmation_height = 90;
        dgb.updated_at = 106;
        pool.push_back(dgb);
        ProviderPoolEntry carrier = dgb;
        carrier.outpoint = COutPoint{uint256S("02"), index};
        carrier.asset = PoolAsset::DD_CARRIER;
        carrier.dgb_value = DGBSatoshis{0};
        carrier.carrier_value = DDCents{100};
        pool.push_back(carrier);
    }
    ProviderPoolEntry operational_dgb = pool.front();
    operational_dgb.outpoint = COutPoint{uint256S("03"), 0};
    operational_dgb.purpose = PoolPurpose::OPERATIONAL;
    operational_dgb.dgb_value = policy.maximum_network_fee;
    pool.push_back(operational_dgb);
    ProviderPoolEntry operational_carrier = pool[1];
    operational_carrier.outpoint = COutPoint{uint256S("04"), 0};
    operational_carrier.purpose = PoolPurpose::OPERATIONAL;
    pool.push_back(operational_carrier);
    BOOST_REQUIRE(SetPaymasterProviderPoolEntries(m_wallet, pool, error));
    std::vector<ProviderPoolEntry> persisted_pool;
    BOOST_REQUIRE(GetPaymasterProviderPoolEntries(m_wallet, persisted_pool));
    BOOST_CHECK_EQUAL(persisted_pool.size(), pool.size());
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, pool.front().outpoint));
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, operational_dgb.outpoint));
    std::vector<ProviderPoolEntry> reserved;
    const uint256 reservation_id = uint256S("05");
    BOOST_REQUIRE(ReservePaymasterOperationalSlot(m_wallet, reservation_id, true,
                                                  100, 1, 107, reserved, error));
    BOOST_CHECK_EQUAL(reserved.size(), 2U);
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, operational_dgb.outpoint));
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, operational_carrier.outpoint));
    std::vector<ProviderPoolEntry> retry;
    BOOST_REQUIRE(ReservePaymasterOperationalSlot(m_wallet, reservation_id, true,
                                                  100, 1, 108, retry, error));
    BOOST_CHECK_EQUAL(retry.size(), 2U);
    BOOST_CHECK(!SetPaymasterProviderPoolEntries(m_wallet, pool, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_ACTIVE_POOL_ENTRY_CONFLICT");
    BOOST_CHECK(!ReservePaymasterOperationalSlot(m_wallet, uint256S("06"), true,
                                                 100, 1, 108, retry, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_OPERATIONAL_SLOT_MISSING");

    ProviderPolicy invalid = policy;
    invalid.maximum_network_fee = DGBSatoshis{0};
    BOOST_CHECK(!SetPaymasterProviderPolicy(m_wallet, invalid, 104, error));
    BOOST_REQUIRE(GetPaymasterProviderPolicy(m_wallet, persisted));
    BOOST_CHECK_EQUAL(GetProviderPolicyHash(persisted), settings.policy_hash);
    BOOST_REQUIRE(SetPaymasterProviderEnabled(m_wallet, false, 105, error));
}

BOOST_AUTO_TEST_CASE(provider_safety_policy_time_never_regresses_below_accounting_high_water)
{
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetupDescriptorScriptPubKeyMans();
    }
    ProviderIdentityRecord identity;
    std::string error;
    BOOST_REQUIRE(CreatePaymasterIdentity(m_wallet, "Provider", 100, identity, error));

    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_USER_PAID;
    policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    policy.fee_rate_bps = 50;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{100000};
    policy.maximum_network_fee = DGBSatoshis{1000};
    BOOST_REQUIRE(SetPaymasterProviderPolicy(m_wallet, policy, 101, error));

    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{1000};
    limits.maximum_reserved_network_fee = DGBSatoshis{2000};
    limits.maximum_network_fee_per_hour = DGBSatoshis{10000};
    limits.maximum_network_fee_per_day = DGBSatoshis{100000};
    limits.maximum_completed_per_hour = 10;
    limits.maximum_completed_per_day = 100;
    ProviderSafetyPolicy safety;
    safety.user_paid = limits;
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 5;
    safety.maximum_active_quotes_per_recipient = 2;
    safety.maximum_quote_requests_per_netgroup_per_minute = 30;
    BOOST_REQUIRE(SetPaymasterProviderSafetyPolicy(m_wallet, safety, 102, error));

    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        ProviderBudgetLedger ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(ledger));
        ledger.accounting_time_high_water = 500;
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(ledger));
    }

    BOOST_REQUIRE(SetPaymasterProviderSafetyPolicy(m_wallet, safety, 250, error));
    ProviderSafetyPolicy persisted;
    ProviderBudgetLedger persisted_ledger;
    BOOST_REQUIRE(GetPaymasterProviderSafetyPolicy(m_wallet, persisted));
    BOOST_REQUIRE(GetPaymasterProviderBudgetLedger(m_wallet, persisted_ledger));
    BOOST_CHECK_EQUAL(persisted.updated_at, 500);
    BOOST_CHECK_EQUAL(persisted_ledger.accounting_time_high_water, 500);
}

BOOST_AUTO_TEST_CASE(capacity_proof_is_signed_from_exact_available_operational_slot)
{
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetupDescriptorScriptPubKeyMans();
    }
    ProviderIdentityRecord identity;
    std::string error;
    BOOST_REQUIRE(CreatePaymasterIdentity(m_wallet, "Capacity Provider", 100, identity, error));

    CMutableTransaction creating;
    creating.vin.emplace_back(COutPoint{uint256::ONE, 9});
    creating.vout.emplace_back(20000000, identity.identity_script);
    creating.vout.emplace_back(0, identity.identity_script);
    const CTransactionRef creating_tx{MakeTransactionRef(std::move(creating))};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(creating_tx, TxStateInMempool{}));
    }

    ProviderPoolEntry dgb;
    dgb.outpoint = COutPoint{creating_tx->GetHash(), 0};
    dgb.purpose = PoolPurpose::OPERATIONAL;
    dgb.asset = PoolAsset::DGB;
    dgb.state = PoolEntryState::AVAILABLE;
    dgb.script_pub_key = identity.identity_script;
    dgb.dgb_value = DGBSatoshis{20000000};
    dgb.confirmation_height = 90;
    dgb.updated_at = 101;
    ProviderPoolEntry carrier;
    carrier.outpoint = COutPoint{creating_tx->GetHash(), 1};
    carrier.purpose = PoolPurpose::OPERATIONAL;
    carrier.asset = PoolAsset::DD_CARRIER;
    carrier.state = PoolEntryState::AVAILABLE;
    carrier.script_pub_key = identity.identity_script;
    carrier.carrier_value = DDCents{100};
    carrier.confirmation_height = 90;
    carrier.updated_at = 101;

    constexpr int64_t now{110};
    const uint256 genesis{uint256S("a1")};
    const uint256 reference_block{uint256S("a2")};
    PaymasterCapacityRequest request;
    request.genesis_hash = genesis;
    request.provider_id = identity.provider_id;
    request.request_id = "550e8400-e29b-41d4-a716-4466554400a3";
    request.session_id = uint256S("a6");
    request.client_nonce = uint256S("a3");
    request.funding_model = FundingModel::USER_PAID;
    request.requires_carrier = true;
    request.created_at = 100;
    request.expires_at = 160;

    PaymasterCapacityProof proof;
    BOOST_REQUIRE_MESSAGE(BuildPaymasterCapacityProof(
                              m_wallet, identity, request, {carrier, dgb}, genesis,
                              reference_block, now, proof, error),
                          error);
    BOOST_CHECK_EQUAL(proof.provider_id, identity.provider_id);
    BOOST_CHECK_EQUAL(proof.request_id, request.request_id);
    BOOST_CHECK_EQUAL(proof.session_id, request.session_id);
    BOOST_CHECK_EQUAL(proof.client_nonce, request.client_nonce);
    BOOST_CHECK(proof.funding_model == request.funding_model);
    BOOST_CHECK_EQUAL(proof.requires_carrier, request.requires_carrier);
    BOOST_CHECK(!proof.snapshot_id.IsNull());
    BOOST_CHECK_EQUAL(proof.created_at, request.created_at);
    BOOST_CHECK_EQUAL(proof.expires_at, request.expires_at);
    BOOST_REQUIRE_EQUAL(proof.liquidity_slots.size(), 1U);
    BOOST_REQUIRE_EQUAL(proof.liquidity_slots.front().dgb_inputs.size(), 1U);
    BOOST_REQUIRE(proof.liquidity_slots.front().carrier.has_value());
    BOOST_CHECK(proof.liquidity_slots.front().dgb_inputs.front().input.outpoint == dgb.outpoint);
    BOOST_CHECK(proof.liquidity_slots.front().carrier->carrier.outpoint == carrier.outpoint);

    CTxDestination destination;
    BOOST_REQUIRE(ExtractDestination(identity.identity_script, destination));
    const auto* taproot{std::get_if<WitnessV1Taproot>(&destination)};
    BOOST_REQUIRE(taproot != nullptr);
    const XOnlyPubKey output_key{*taproot};
    CapacityChainstateCallbacks chainstate;
    chainstate.validate_reference_block = [&](const uint256& block, std::string&) {
        return block == reference_block;
    };
    chainstate.validate_dgb_input = [&](const VerifiedDGBInput& input,
                                        XOnlyPubKey& actual_key, std::string&) {
        actual_key = output_key;
        return input.outpoint == dgb.outpoint && input.value == dgb.dgb_value;
    };
    chainstate.validate_dd_carrier = [&](const VerifiedDDCarrier& input,
                                         XOnlyPubKey& actual_key, std::string&) {
        actual_key = output_key;
        return input.outpoint == carrier.outpoint && input.value == carrier.carrier_value;
    };
    BOOST_CHECK(ValidateCapacityProof(proof, request, genesis, identity.identity_key,
                                      chainstate, now, error));

    ProviderPoolEntry unavailable{dgb};
    unavailable.state = PoolEntryState::RESERVED;
    unavailable.reservation_id = uint256S("a4");
    BOOST_CHECK(!BuildPaymasterCapacityProof(
        m_wallet, identity, request, {unavailable}, genesis,
        reference_block, now, proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_SLOT_NOT_AVAILABLE");

    ProviderPoolEntry wrong_value{dgb};
    ++wrong_value.dgb_value.value;
    BOOST_CHECK(!BuildPaymasterCapacityProof(
        m_wallet, identity, request, {wrong_value, carrier}, genesis,
        reference_block, now, proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_DGB_PREVOUT_MISMATCH");

    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_ALL;
    policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    policy.fee_rate_bps = 50;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{100000};
    policy.maximum_network_fee = DGBSatoshis{10000000};
    BOOST_REQUIRE(SetPaymasterProviderPolicy(m_wallet, policy, 111, error));
    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{10000000};
    limits.maximum_reserved_network_fee = DGBSatoshis{20000000};
    limits.maximum_network_fee_per_hour = DGBSatoshis{100000000};
    limits.maximum_network_fee_per_day = DGBSatoshis{1000000000};
    limits.maximum_completed_per_hour = 10;
    limits.maximum_completed_per_day = 100;
    ProviderSafetyPolicy safety;
    safety.user_paid = limits;
    safety.public_sponsored = limits;
    safety.maximum_active_quotes_total = 10;
    safety.maximum_active_quotes_per_netgroup = 5;
    safety.maximum_active_quotes_per_recipient = 2;
    safety.maximum_quote_requests_per_netgroup_per_minute = 30;
    BOOST_REQUIRE(SetPaymasterProviderSafetyPolicy(
        m_wallet, safety, 111, error));
    BOOST_REQUIRE(SetPaymasterProviderPoolEntries(m_wallet, {carrier, dgb}, error));

    const std::vector<unsigned char> canonical_netgroup{
        5, 0x13, 0x37, 0x42, 0x56, 0x68, 0x79, 0x8a, 0x9b, 0xac, 0xbd};
    PaymasterStore store{m_wallet};

    PaymasterCapacityRequest reserved_request{request};
    reserved_request.client_nonce = uint256S("a5");
    reserved_request.created_at = 112;
    reserved_request.expires_at = 172;
    PaymasterCapacityProof reserved_proof;
    std::vector<ProviderPoolEntry> reserved;
    BOOST_REQUIRE_MESSAGE(ReserveAndBuildPaymasterCapacityProof(
                              m_wallet, identity, reserved_request, genesis,
                              reference_block, 100, 1, canonical_netgroup, 112,
                              reserved_proof,
                              reserved, error),
                          error);
    BOOST_REQUIRE_EQUAL(reserved.size(), 2U);
    BOOST_CHECK(std::all_of(reserved.begin(), reserved.end(), [&](const auto& entry) {
        return entry.state == PoolEntryState::RESERVED &&
               entry.reservation_id == reserved_request.client_nonce;
    }));
    {
        auto restarted_database =
            DuplicateMockDatabase(m_wallet.GetDatabase());
        CWallet restarted_wallet{m_node.chain.get(),
                                 "capacity-drain-restart",
                                 std::move(restarted_database)};
        BOOST_REQUIRE(restarted_wallet.LoadWallet() == DBErrors::LOAD_OK);
        PaymasterStore restarted_store{restarted_wallet};
        ScopedPaymasterMockTime mock_time{113};
        bool has_drain_work{false};
        BOOST_REQUIRE_MESSAGE(restarted_store.HasProviderDrainWork(
                                  identity.provider_id, has_drain_work, error),
                              error);
        BOOST_CHECK(has_drain_work);
    }
    ProviderBudgetLedger admission_ledger;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}
                          .ReadPaymasterProviderBudgetLedger(admission_ledger));
    }
    BOOST_REQUIRE_EQUAL(admission_ledger.capacity_admissions.size(), 1U);
    BOOST_CHECK_EQUAL(admission_ledger.capacity_admissions.front().netgroup_bucket,
                      GetNetgroupBudgetBucket(admission_ledger,
                                              canonical_netgroup));

    {
        // A persisted proof is only a replay barrier when the budget admission
        // from its atomic reservation is absent. It must neither authorize a
        // quote continuation nor prevent an automatic provider from starting
        // solely to replenish unrelated missing liquidity.
        auto orphan_database =
            DuplicateMockDatabase(m_wallet.GetDatabase());
        WalletBatch orphan_batch{*orphan_database};
        ProviderBudgetLedger orphan_ledger;
        BOOST_REQUIRE(orphan_batch.ReadPaymasterProviderBudgetLedger(
            orphan_ledger));
        orphan_ledger.capacity_admissions.clear();
        BOOST_REQUIRE(orphan_batch.WritePaymasterProviderBudgetLedger(
            orphan_ledger));

        CWallet orphan_wallet{m_node.chain.get(),
                              "orphan-capacity-drain-restart",
                              std::move(orphan_database)};
        BOOST_REQUIRE(orphan_wallet.LoadWallet() == DBErrors::LOAD_OK);
        PaymasterStore orphan_store{orphan_wallet};
        ScopedPaymasterMockTime mock_time{113};
        bool has_drain_work{true};
        BOOST_REQUIRE_MESSAGE(orphan_store.HasProviderDrainWork(
                                  identity.provider_id, has_drain_work, error),
                              error);
        BOOST_CHECK(!has_drain_work);
    }

    CDataStream admission_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    admission_stream << admission_ledger;
    const auto admission_bytes = MakeUCharSpan(admission_stream);
    BOOST_CHECK(std::search(admission_bytes.begin(), admission_bytes.end(),
                            canonical_netgroup.begin(), canonical_netgroup.end()) ==
                admission_bytes.end());

    PaymasterCapacityProof retry_proof;
    std::vector<ProviderPoolEntry> retry_reserved;
    BOOST_REQUIRE_MESSAGE(ReserveAndBuildPaymasterCapacityProof(
                              m_wallet, identity, reserved_request, genesis,
                              reference_block, 100, 1, canonical_netgroup, 113,
                              retry_proof,
                              retry_reserved, error),
                          error);
    BOOST_CHECK_EQUAL(retry_proof.snapshot_id, reserved_proof.snapshot_id);
    BOOST_CHECK(retry_proof.identity_signature == reserved_proof.identity_signature);
    BOOST_CHECK(retry_proof.liquidity_slots.front().dgb_inputs.front().control_proof.signature ==
                reserved_proof.liquidity_slots.front().dgb_inputs.front().control_proof.signature);
    BOOST_CHECK(retry_proof.liquidity_slots.front().carrier->control_proof.signature ==
                reserved_proof.liquidity_slots.front().carrier->control_proof.signature);
    CDataStream first_serialized{SER_NETWORK, ::PROTOCOL_VERSION};
    CDataStream retry_serialized{SER_NETWORK, ::PROTOCOL_VERSION};
    first_serialized << reserved_proof;
    retry_serialized << retry_proof;
    const auto first_bytes = MakeUCharSpan(first_serialized);
    const auto retry_bytes = MakeUCharSpan(retry_serialized);
    BOOST_CHECK(std::vector<unsigned char>(first_bytes.begin(), first_bytes.end()) ==
                std::vector<unsigned char>(retry_bytes.begin(), retry_bytes.end()));
    BOOST_REQUIRE_EQUAL(retry_reserved.size(), reserved.size());
    BOOST_CHECK_EQUAL(retry_reserved.front().reservation_id,
                      reserved_request.client_nonce);

    PaymasterCapacityRequest session_conflict{reserved_request};
    session_conflict.client_nonce = uint256S("aa");
    BOOST_CHECK(!ReserveAndBuildPaymasterCapacityProof(
        m_wallet, identity, session_conflict, genesis, reference_block,
        100, 1, canonical_netgroup, 113, retry_proof, retry_reserved, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_SESSION_CONFLICT");

    PaymasterCapacityRequest competing_request{reserved_request};
    competing_request.request_id = "550e8400-e29b-41d4-a716-4466554400a4";
    competing_request.session_id = uint256S("a8");
    competing_request.client_nonce = uint256S("a9");
    BOOST_CHECK(!ReserveAndBuildPaymasterCapacityProof(
        m_wallet, identity, competing_request, genesis, reference_block,
        100, 1, canonical_netgroup, 113, retry_proof, retry_reserved, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_OPERATIONAL_SLOT_MISSING");

    PaymasterCapacityRequest conflicting_request{reserved_request};
    conflicting_request.session_id = uint256S("a7");
    BOOST_CHECK(!ReserveAndBuildPaymasterCapacityProof(
        m_wallet, identity, conflicting_request, genesis, reference_block,
        100, 1, canonical_netgroup, 113, retry_proof, retry_reserved, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_REQUEST_CONFLICT");

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_expiry = database.m_records;
    size_t expired_reservations{0};
    {
        ScopedPaymasterMockTime mock_time{173};
        bool has_drain_work{true};
        BOOST_REQUIRE_MESSAGE(store.HasProviderDrainWork(
                                  identity.provider_id, has_drain_work, error),
                              error);
        BOOST_CHECK(!has_drain_work);
    }
    for (size_t failing_write = 0; failing_write < 2; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.ExpireProviderCapacityReservations(
            173, expired_reservations, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        BOOST_CHECK_EQUAL(expired_reservations, 0U);
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_expiry);
    }
    database.FailCommit();
    BOOST_CHECK(!store.ExpireProviderCapacityReservations(
        173, expired_reservations, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    BOOST_CHECK_EQUAL(expired_reservations, 0U);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_expiry);

    BOOST_REQUIRE_MESSAGE(store.ExpireProviderCapacityReservations(
                              173, expired_reservations, error),
                          error);
    BOOST_CHECK_EQUAL(expired_reservations, 1U);
    std::vector<ProviderPoolEntry> released_pool;
    ProviderCapacityReleaseRecord release;
    CDataStream reserved_request_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    reserved_request_stream << reserved_request;
    const uint256 reserved_request_hash{Hash(MakeUCharSpan(reserved_request_stream))};
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(released_pool));
        BOOST_REQUIRE(batch.ReadPaymasterCapacityRelease(reserved_request_hash,
                                                         release));
    }
    BOOST_REQUIRE_EQUAL(released_pool.size(), 2U);
    BOOST_CHECK(std::all_of(released_pool.begin(), released_pool.end(),
                            [](const auto& entry) {
                                return entry.state == PoolEntryState::AVAILABLE &&
                                       entry.reservation_id.IsNull() &&
                                       entry.updated_at == 173;
                            }));
    BOOST_CHECK_EQUAL(release.client_nonce, reserved_request.client_nonce);
    BOOST_CHECK_EQUAL(release.released_at, 173);
    {
        // A release remains terminal even if the wall clock subsequently
        // appears to move back inside the old proof window.
        ScopedPaymasterMockTime mock_time{171};
        bool has_drain_work{true};
        BOOST_REQUIRE_MESSAGE(store.HasProviderDrainWork(
                                  identity.provider_id, has_drain_work, error),
                              error);
        BOOST_CHECK(!has_drain_work);
    }
    auto expiry_restart = DuplicateMockDatabase(m_wallet.GetDatabase());
    WalletBatch expiry_restart_batch{*expiry_restart};
    std::vector<ProviderPoolEntry> restarted_released_pool;
    ProviderCapacityReleaseRecord restarted_release;
    BOOST_REQUIRE(expiry_restart_batch.ReadPaymasterProviderPool(
        restarted_released_pool));
    BOOST_REQUIRE(expiry_restart_batch.ReadPaymasterCapacityRelease(
        reserved_request_hash, restarted_release));
    BOOST_REQUIRE_EQUAL(restarted_released_pool.size(), released_pool.size());
    for (size_t index = 0; index < released_pool.size(); ++index) {
        BOOST_CHECK(restarted_released_pool[index].outpoint ==
                    released_pool[index].outpoint);
        BOOST_CHECK(restarted_released_pool[index].state ==
                    released_pool[index].state);
        BOOST_CHECK_EQUAL(restarted_released_pool[index].reservation_id,
                          released_pool[index].reservation_id);
        BOOST_CHECK_EQUAL(restarted_released_pool[index].updated_at,
                          released_pool[index].updated_at);
    }
    BOOST_CHECK_EQUAL(restarted_release.request_hash, reserved_request_hash);
    BOOST_CHECK_EQUAL(restarted_release.client_nonce,
                      reserved_request.client_nonce);
    BOOST_REQUIRE(store.ExpireProviderCapacityReservations(
        174, expired_reservations, error));
    BOOST_CHECK_EQUAL(expired_reservations, 0U);

    // The durable response and indexes remain replay barriers. Even with a
    // simulated wall-clock rollback into the old proof window, releasing its
    // pool reservation cannot reactivate it.
    BOOST_CHECK(!ReserveAndBuildPaymasterCapacityProof(
        m_wallet, identity, reserved_request, genesis, reference_block,
        100, 1, canonical_netgroup, 171, retry_proof, retry_reserved, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESPONSE_DGB_MISMATCH");

    PaymasterCapacityRequest rebound_request{reserved_request};
    rebound_request.request_id = "550e8400-e29b-41d4-a716-4466554400b3";
    rebound_request.session_id = uint256S("b6");
    rebound_request.client_nonce = uint256S("b5");
    rebound_request.created_at = 174;
    rebound_request.expires_at = 234;
    PaymasterCapacityProof rebound_proof;
    std::vector<ProviderPoolEntry> rebound_reserved;
    BOOST_REQUIRE_MESSAGE(ReserveAndBuildPaymasterCapacityProof(
                              m_wallet, identity, rebound_request, genesis,
                              reference_block, 100, 1, canonical_netgroup, 174,
                              rebound_proof,
                              rebound_reserved, error),
                          error);
    BOOST_REQUIRE_EQUAL(rebound_reserved.size(), 2U);
    {
        ScopedPaymasterMockTime mock_time{175};
        bool has_drain_work{false};
        BOOST_REQUIRE_MESSAGE(store.HasProviderDrainWork(
                                  identity.provider_id, has_drain_work, error),
                              error);
        BOOST_CHECK(has_drain_work);
    }

    // Reusing the same physical slot for a new nonce must not make an older
    // persisted response valid again.
    BOOST_CHECK(!ReserveAndBuildPaymasterCapacityProof(
        m_wallet, identity, reserved_request, genesis, reference_block,
        100, 1, canonical_netgroup, 171, retry_proof, retry_reserved, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESPONSE_DGB_MISMATCH");

    // Model the crash boundary at which an accepted user PSBT already proves
    // that an authorization flow bound this nonce while the pool still
    // carries the original capacity reservation. Cleanup must retain every
    // slot without manufacturing provider-signature authority.
    PaymentSession bound_session;
    bound_session.request_id = rebound_request.request_id;
    bound_session.session_id = rebound_request.session_id;
    bound_session.canonical_request_hash = uint256S("b7");
    bound_session.state = SessionState::PENDING_PROVIDER;
    bound_session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
    bound_session.created_at = 175;
    bound_session.updated_at = 175;
    bound_session.provider_side = true;
    ProviderAttempt bound_attempt;
    bound_attempt.session_id = bound_session.session_id;
    bound_attempt.attempt_id = uint256S("b8");
    bound_attempt.provider_id = identity.provider_id;
    bound_attempt.provider_identity_key = identity.identity_key;
    bound_attempt.state = AttemptState::USER_PSBT_ACCEPTED;
    bound_attempt.client_nonce = rebound_request.client_nonce;
    bound_attempt.created_at = 175;
    bound_attempt.updated_at = 175;
    bound_session.attempt_ids = {bound_attempt.attempt_id};
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.TxnBegin());
        BOOST_REQUIRE(batch.WritePaymasterSession(bound_session));
        BOOST_REQUIRE(batch.WritePaymasterSessionId(bound_session.session_id,
                                                    bound_session.request_id));
        BOOST_REQUIRE(batch.WritePaymasterAttempt(bound_attempt));
        BOOST_REQUIRE(batch.TxnCommit());
    }
    BOOST_REQUIRE(store.ExpireProviderCapacityReservations(
        235, expired_reservations, error));
    BOOST_CHECK_EQUAL(expired_reservations, 0U);
    std::vector<ProviderPoolEntry> bound_pool;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(bound_pool));
        CDataStream request_stream{SER_NETWORK, ::PROTOCOL_VERSION};
        request_stream << rebound_request;
        BOOST_CHECK(!batch.HasPaymasterCapacityRelease(
            Hash(MakeUCharSpan(request_stream))));
    }
    BOOST_REQUIRE_EQUAL(bound_pool.size(), 2U);
    BOOST_CHECK(std::all_of(bound_pool.begin(), bound_pool.end(),
                            [&](const auto& entry) {
                                return entry.state == PoolEntryState::RESERVED &&
                                       entry.reservation_id ==
                                           rebound_request.client_nonce;
                            }));
}

BOOST_AUTO_TEST_CASE(pool_setup_journal_survives_abort_and_rejects_unknown_records)
{
    LOCK(m_wallet.cs_wallet);
    CKey key;
    key.MakeNewKey(true);
    ProviderMaintenanceOutput output;
    output.asset = PoolAsset::DGB;
    output.purpose = PoolPurpose::ADMISSION;
    output.script_pub_key = GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    output.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
    ProviderMaintenanceRecord record;
    record.kind = ProviderMaintenanceKind::PREPARE_DGB;
    record.operation_id = uint256S("a1");
    record.plan_id = uint256S("a2");
    record.preparation_authorization = uint256S("a3");
    record.preparation_request = uint256S("a4");
    record.maximum_fee = DGBSatoshis{20000000};
    record.created_at = record.updated_at = 1000;
    record.outputs = {output};
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    ProviderMaintenanceLedger loaded;
    BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(loaded));
    BOOST_REQUIRE_EQUAL(loaded.records.size(), 1U);
    BOOST_CHECK(loaded.records.front().preparation_request == record.preparation_request);
    const auto original = GetMockableDatabase(m_wallet).m_records;

    ledger.records.front().preparation_error = "PAYMASTER_POOL_WAITING_DGB";
    BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    BOOST_REQUIRE(batch.TxnAbort());
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == original);
    GetMockableDatabase(m_wallet).FailWriteAt(0);
    BOOST_CHECK(!batch.WritePaymasterMaintenanceLedger(ledger));
    GetMockableDatabase(m_wallet).ClearFailureInjection();
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == original);
    BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(loaded));
    BOOST_CHECK(loaded.records.front().preparation_error.empty());

    // Bypass the writer only to emulate an unsupported/corrupt disk record.
    ledger.records.front().version = ProviderMaintenanceRecord::CURRENT_VERSION + 1;
    auto raw = m_wallet.GetDatabase().MakeBatch();
    BOOST_REQUIRE(raw->Write(DBKeys::PAYMASTER_MAINTENANCE_LEDGER, ledger));
    BOOST_CHECK(batch.ReadPaymasterMaintenanceLedgerWithStatus(loaded) == DatabaseReadStatus::READ_ERROR);
}


BOOST_AUTO_TEST_CASE(pool_setup_reconciles_conflict_reversal_atomically)
{
    ScopedPaymasterMockTime clock{2000};
    LOCK(m_wallet.cs_wallet);
    m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    m_wallet.SetupDescriptorScriptPubKeyMans();
    const auto destination = m_wallet.GetNewDestination(OutputType::BECH32M, "setup recovery test");
    BOOST_REQUIRE(destination);
    const CScript script = GetScriptForDestination(*destination);
    CMutableTransaction funding;
    funding.vin.emplace_back(COutPoint{uint256S("b1"), 0});
    funding.vout.emplace_back(MIN_ADMISSION_DGB_SATOSHIS + 1000, script);
    const auto parent = MakeTransactionRef(funding);
    BOOST_REQUIRE(m_wallet.AddToWallet(parent, TxStateInactive{}));
    CMutableTransaction setup;
    setup.vin.emplace_back(COutPoint{parent->GetHash(), 0});
    setup.vout.emplace_back(MIN_ADMISSION_DGB_SATOSHIS, script);
    const auto tx = MakeTransactionRef(setup);
    auto* wtx = m_wallet.AddToWallet(tx, TxStateInactive{});
    BOOST_REQUIRE(wtx);
    const TxStateConfirmed confirmed{Params().GenesisBlock().GetHash(), 1, 0};
    wtx->m_state = confirmed;
    ProviderMaintenanceRecord record;
    record.kind = ProviderMaintenanceKind::PREPARE_DGB;
    record.operation_id = uint256S("b2");
    record.plan_id = uint256S("b3");
    record.preparation_authorization = uint256S("b4");
    record.preparation_request = uint256S("b5");
    record.maximum_fee = DGBSatoshis{2000};
    record.created_at = record.updated_at = 1000;
    ProviderMaintenanceOutput output;
    output.asset = PoolAsset::DGB;
    output.purpose = PoolPurpose::ADMISSION;
    output.script_pub_key = script;
    output.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
    record.outputs = {output};
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    size_t recovered{0};
    std::string error;
    BOOST_REQUIRE_MESSAGE(ReconcilePaymasterProviderMaintenance(m_wallet, recovered, error), error);
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);

    for (const TxState& failure : {TxState{TxStateConflicted{uint256S("b6"), 1}}, TxState{TxStateInactive{true}}}) {
        wtx->m_state = failure;
        BOOST_REQUIRE_MESSAGE(ReconcilePaymasterProviderMaintenance(m_wallet, recovered, error), error);
        BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
        BOOST_CHECK(ledger.records.front().state == ProviderMaintenanceState::FAILED);
        BOOST_CHECK(paymaster_rpc::internal::HasPendingPoolPreparation(m_wallet));
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        BOOST_CHECK(pool.front().state == PoolEntryState::INVALIDATED);
        // Reconfirmation must update both the journal and the invalidated
        // outputs atomically; failure at commit may expose neither change.
        wtx->m_state = confirmed;
        auto& database = GetMockableDatabase(m_wallet);
        const auto before = database.m_records;
        database.m_fail_commit = true;
        BOOST_CHECK(!ReconcilePaymasterProviderMaintenance(m_wallet, recovered, error));
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before);
        BOOST_REQUIRE_MESSAGE(ReconcilePaymasterProviderMaintenance(m_wallet, recovered, error), error);
        BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
        BOOST_CHECK(ledger.records.front().state == ProviderMaintenanceState::CONFIRMED);
        BOOST_CHECK(ledger.records.front().transaction_id == tx->GetHash());
        BOOST_CHECK_EQUAL(ledger.records.front().actual_fee.value, 1000);
        BOOST_CHECK(!paymaster_rpc::internal::HasPendingPoolPreparation(m_wallet));
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        BOOST_REQUIRE_EQUAL(pool.size(), 1U);
        BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
        BOOST_CHECK(pool.front().reservation_id.IsNull());
    }
    BOOST_CHECK_EQUAL(m_wallet.mapWallet.size(), 2U);
    BOOST_CHECK(*wtx->tx == *tx);
}

BOOST_AUTO_TEST_CASE(retirement_finance_recovers_wallet_commit_after_write_failure_and_restart)
{
    ScopedPaymasterMockTime clock{2000};
    LOCK(m_wallet.cs_wallet);
    m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    m_wallet.SetupDescriptorScriptPubKeyMans();
    m_wallet.SetBroadcastTransactions(false);
    ProviderIdentityRecord identity;
    std::string error;
    BOOST_REQUIRE(CreatePaymasterIdentity(m_wallet, "retirement test", 1000, identity, error));
    const auto destination = m_wallet.GetNewDestination(OutputType::BECH32M, "retirement test");
    BOOST_REQUIRE(destination);
    const CScript script = GetScriptForDestination(*destination);
    CMutableTransaction funding;
    funding.vin.emplace_back(COutPoint{uint256S("c1"), 0});
    funding.vout.emplace_back(100000, script);
    const auto parent = MakeTransactionRef(funding);
    BOOST_REQUIRE(m_wallet.AddToWallet(parent, TxStateInactive{}));
    CMutableTransaction retirement;
    retirement.vin.emplace_back(COutPoint{parent->GetHash(), 0});
    retirement.vout.emplace_back(98000, script);
    const auto tx = MakeTransactionRef(retirement);
    BOOST_REQUIRE(m_wallet.CommitTransaction(tx,
        {{paymaster_rpc::internal::PAYMASTER_RETIREMENT_PROVIDER_KEY, identity.provider_id.GetHex()}}, {}, &error));
    auto& database = GetMockableDatabase(m_wallet);
    // Model the real post-commit gap: the wallet has signed bytes but the
    // subsequent finance write fails. Reopening must discover its own marker.
    const auto before = database.m_records;
    database.FailWriteAt(0);
    size_t changed{0};
    BOOST_CHECK(!ReconcilePaymasterProviderFinances(m_wallet, changed, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before);
    CWallet reopened{m_node.chain.get(), "retirement-restart", DuplicateMockDatabase(database)};
    BOOST_REQUIRE(reopened.LoadWallet() == DBErrors::LOAD_OK);
    BOOST_REQUIRE_MESSAGE(ReconcilePaymasterProviderFinances(reopened, changed, error), error);
    WalletBatch batch{reopened.GetDatabase()};
    ProviderFinanceLedger finance;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceLedger(finance));
    BOOST_REQUIRE_EQUAL(finance.events.size(), 1U);
    BOOST_CHECK(finance.events.front().kind == ProviderFinanceEventKind::POOL_RETIREMENT);
    BOOST_CHECK(finance.events.front().transaction_id == tx->GetHash());
    BOOST_CHECK_EQUAL(finance.events.front().dgb_cost.value, 2000);
    BOOST_REQUIRE_MESSAGE(ReconcilePaymasterProviderFinances(reopened, changed, error), error);
    BOOST_CHECK_EQUAL(changed, 0U);
    BOOST_REQUIRE(batch.ReadPaymasterFinanceLedger(finance));
    BOOST_CHECK_EQUAL(finance.events.size(), 1U);
    BOOST_CHECK_EQUAL(reopened.mapWallet.size(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
