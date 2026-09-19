// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/paymasterwidgettests.h>
#include <qt/test/digidollartestutil.h>
#include <qt/test/util.h>

#include <consensus/digidollar.h>
#include <coins.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <paymaster/provider.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/paymasterconfirmation.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/digidollarsendwidget.h>
#include <qt/paymastersendwidget.h>
#include <qt/digidollarstatus.h>
#include <qt/digidollartab.h>
#include <qt/guiutil.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <validation.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <univalue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <memory>
#include <map>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <QApplication>
#include <QAccessible>
#include <QColor>
#include <QCheckBox>
#include <QCoreApplication>
#include <QFile>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QComboBox>
#include <QRadioButton>
#include <QTableWidget>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QWizard>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest/QtTestWidgets>
#include <QtWidgets/qtestsupport_widgets.h>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
using wallet::WalletContext;
using wallet::WalletRescanReserver;

using DigiDollarTest::DigiDollarMiniGUI;
using DigiDollarTest::SetupDescriptorsWallet;

namespace {

UniValue PaymasterLiquiditySlotStatus(int ready, int pending, int missing,
                                      int target)
{
    UniValue status{UniValue::VOBJ};
    status.pushKV("ready", ready);
    status.pushKV("pending", pending);
    status.pushKV("missing", missing);
    status.pushKV("target", target);
    status.pushKV("counted_toward_target",
                  std::min(target, ready + pending));
    return status;
}

UniValue PaymasterLiquidityStatus(const std::string& state, bool configured,
                                  bool approved, int missing,
                                  int pending = 0,
                                  bool targets_satisfy_provider_policy = true)
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("automatic_replenishment", true);
    policy.pushKV("paid_maintenance_approved", approved);
    policy.pushKV("target_admission_dgb", 3);
    policy.pushKV("target_operational_dgb", 1);
    policy.pushKV("target_admission_carriers", 3);
    policy.pushKV("target_operational_carriers",
                  targets_satisfy_provider_policy ? 1 : 0);
    policy.pushKV("maximum_maintenance_fee_per_transaction_satoshis", 10000000);
    policy.pushKV("maximum_maintenance_fee_per_hour_satoshis", 50000000);
    policy.pushKV("maximum_maintenance_fee_per_day_satoshis", 200000000);

    UniValue result{UniValue::VOBJ};
    result.pushKV("policy_configured", configured);
    result.pushKV("targets_satisfy_provider_policy",
                  targets_satisfy_provider_policy);
    result.pushKV("maintenance_state", state);
    result.pushKV("policy", std::move(policy));
    result.pushKV("admission_dgb",
                  PaymasterLiquiditySlotStatus(3, 0, 0, 3));
    result.pushKV("operational_dgb",
                  PaymasterLiquiditySlotStatus(
                      (missing > 0 || pending > 0) ? 0 : 1,
                      pending, missing, 1));
    result.pushKV("admission_carriers",
                  PaymasterLiquiditySlotStatus(3, 0, 0, 3));
    result.pushKV("operational_carriers",
                  PaymasterLiquiditySlotStatus(
                      targets_satisfy_provider_policy ? 1 : 0, 0, 0,
                      targets_satisfy_provider_policy ? 1 : 0));
    result.pushKV("maintenance_fee_reserved_satoshis", 1000000);
    result.pushKV("maintenance_fee_spent_last_hour_satoshis", 2000000);
    result.pushKV("maintenance_fee_spent_last_day_satoshis", 3000000);
    result.pushKV("carrier_base_cents", 100);
    result.pushKV("carrier_withdrawable_excess_cents", 6);
    result.pushKV("readiness_errors", UniValue{UniValue::VARR});
    return result;
}

UniValue PaymasterLiquidityPoolStatus()
{
    UniValue available{UniValue::VOBJ};
    available.pushKV("state", "available");
    available.pushKV("purpose", "operational");
    available.pushKV("asset", "dd_carrier");
    available.pushKV("txid",
                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    available.pushKV("vout", 1);
    available.pushKV("dgb_satoshis", 0);
    available.pushKV("dd_cents", 106);
    available.pushKV("confirmation_height", 120);
    available.pushKV("updated_at", 1000);

    UniValue pending_carrier{UniValue::VOBJ};
    pending_carrier.pushKV("state", "pending_successor");
    pending_carrier.pushKV("purpose", "operational");
    pending_carrier.pushKV("asset", "dd_carrier");
    pending_carrier.pushKV(
        "txid",
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pending_carrier.pushKV("vout", 2);
    pending_carrier.pushKV("dgb_satoshis", 0);
    pending_carrier.pushKV("dd_cents", 103);
    pending_carrier.pushKV("confirmation_height", 0);
    pending_carrier.pushKV("updated_at", 1001);

    UniValue pending_dgb{UniValue::VOBJ};
    pending_dgb.pushKV("state", "pending_successor");
    pending_dgb.pushKV("purpose", "operational");
    pending_dgb.pushKV("asset", "dgb");
    pending_dgb.pushKV(
        "txid",
        "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pending_dgb.pushKV("vout", 3);
    pending_dgb.pushKV("dgb_satoshis", 10000000);
    pending_dgb.pushKV("dd_cents", 0);
    pending_dgb.pushKV("confirmation_height", 0);
    pending_dgb.pushKV("updated_at", 1002);

    UniValue pool{UniValue::VARR};
    pool.push_back(std::move(available));
    pool.push_back(std::move(pending_carrier));
    pool.push_back(std::move(pending_dgb));
    UniValue result{UniValue::VOBJ};
    result.pushKV("pool", std::move(pool));
    return result;
}

UniValue PaymasterClientSafetyStatus()
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("maximum_service_fee_per_transaction_cents", 100);
    policy.pushKV("maximum_service_fee_per_day_cents", 1000);

    UniValue result{UniValue::VOBJ};
    result.pushKV("configured", true);
    result.pushKV("policy", std::move(policy));
    result.pushKV("active_reservations", 0);
    result.pushKV("reserved_service_fee_cents", 0);
    result.pushKV("spent_service_fee_last_day_cents", 0);
    result.pushKV("available_service_fee_today_cents", 1000);
    return result;
}

UniValue PaymasterOffer(const std::string& display_name,
                        const std::string& provider_id,
                        const std::string& funding_model,
                        int64_t service_fee_cents,
                        int64_t payment_cents,
                        bool established_reputation,
                        int64_t expires_at)
{
    UniValue offer{UniValue::VOBJ};
    offer.pushKV("display_name", display_name);
    offer.pushKV("provider_id", provider_id);
    offer.pushKV("offer_id", "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    offer.pushKV("policy_hash", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    offer.pushKV("funding_model", funding_model);
    offer.pushKV("service_fee_cents", service_fee_cents);
    offer.pushKV("payment_cents", payment_cents);
    offer.pushKV("user_total_cents", payment_cents + service_fee_cents);
    offer.pushKV("subtract_paymaster_fee_from_amount", false);
    offer.pushKV("reputation_sufficient_data", established_reputation);
    offer.pushKV("success_rate_basis_points", established_reputation ? 9875 : 0);
    offer.pushKV("expires_at", expires_at);
    return offer;
}

UniValue PaymasterSessionView(const std::string& state,
                              const std::string& artifact,
                              const std::string& attempt_state,
                              bool include_recipient = true,
                              int64_t recovery_expires_at = 2000000000,
                              bool recovery_expired = false)
{
    const std::string provider(64, 'b');
    UniValue session{UniValue::VOBJ};
    session.pushKV("request_id", "00000000-0000-4000-8000-000000000001");
    session.pushKV("session_id", std::string(64, '1'));
    session.pushKV("session_state", state);
    session.pushKV("pending_phase", "NONE");
    session.pushKV("broadcast_state",
                   state == "MEMPOOL" ? "accepted_mempool" :
                   state == "CONFIRMED" || state == "CANCELED_SAFE"
                       ? "confirmed" : "not_attempted");
    session.pushKV("confirmation_state",
                   state == "CONFIRMED" ? "payment_confirmed" :
                   state == "CANCELED_SAFE" ? "recovery_confirmed" :
                   state == "CONFLICTED" ? "conflicted" : "unconfirmed");
    session.pushKV("final", state == "CONFIRMED" ||
                                state == "CANCELED_SAFE" ||
                                state == "FAILED" || state == "CONFLICTED");
    if (include_recipient || !attempt_state.empty()) {
        session.pushKV("provider_id", provider);
        session.pushKV("policy_hash", std::string(64, 'a'));
    }
    session.pushKV("privacy_profile", "standard");
    if (include_recipient) {
        session.pushKV("to_address",
                       "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
        session.pushKV("requested_amount_cents", 325);
        session.pushKV("payment_cents", 325);
        session.pushKV("service_fee_cents", 2);
        session.pushKV("user_total_cents", 327);
    }
    session.pushKV("expires_at", int64_t{2000000000});
    if (state == "MEMPOOL" || state == "CONFIRMED") {
        session.pushKV("txid", std::string(64, 'e'));
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("action", "refresh");
    result.pushKV("session", std::move(session));
    if (attempt_state.empty()) {
        result.pushKV("attempt", UniValue{UniValue::VNULL});
    } else {
        UniValue attempt{UniValue::VOBJ};
        attempt.pushKV("attempt_state", attempt_state);
        attempt.pushKV("provider_id", provider);
        attempt.pushKV("privacy_profile", "standard");
        result.pushKV("attempt", std::move(attempt));
    }
    result.pushKV("artifact", artifact);
    if (artifact == "alternative_recovery") {
        UniValue recovery{UniValue::VOBJ};
        recovery.pushKV("expires_at", recovery_expires_at);
        recovery.pushKV("expired", recovery_expired);
        result.pushKV("recovery", std::move(recovery));
    } else {
        result.pushKV("recovery", UniValue{UniValue::VNULL});
    }
    result.pushKV("requires_attention",
                  state == "FAILED" || state == "CONFLICTED");
    UniValue actions{UniValue::VARR};
    actions.push_back("refresh");
    if (artifact == "none" && state != "FAILED" &&
        state != "CONFLICTED") {
        actions.push_back("resume");
        actions.push_back("abandon_unsigned");
        if (!attempt_state.empty()) actions.push_back("fallback");
    }
    if ((artifact == "user_psbt" || artifact == "final_transaction") &&
        state != "CONFIRMED") {
        actions.push_back("retry_same");
        actions.push_back("cancel_to_self");
    }
    if (artifact == "alternative_recovery") {
        actions.push_back("recover");
        actions.push_back("cancel_to_self");
    }
    result.pushKV("allowed_actions", std::move(actions));
    if (state == "MEMPOOL" || state == "CONFIRMED") {
        result.pushKV("result_status", "final_committed");
        result.pushKV("result_sequence", 1);
    } else {
        result.pushKV("result_status", UniValue{UniValue::VNULL});
        result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    }
    return result;
}

UniValue PaymasterAuthorizationResult(bool authorization_required,
                                      const std::string& commitment,
                                      const std::string& state,
                                      const std::string& artifact)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", "00000000-0000-4000-8000-000000000001");
    result.pushKV("session_id", std::string(64, '1'));
    result.pushKV("session_state", state);
    result.pushKV("pending_phase", "NONE");
    result.pushKV("broadcast_state", "not_attempted");
    result.pushKV("confirmation_state", "unconfirmed");
    result.pushKV("provider_id", std::string(64, 'b'));
    result.pushKV("offer_id", std::string(64, 'd'));
    result.pushKV("policy_hash", std::string(64, 'a'));
    result.pushKV("funding_model", "user_paid");
    result.pushKV("to_address",
                  "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    result.pushKV("privacy_profile", "standard");
    result.pushKV("artifact", artifact);
    result.pushKV("payment_cents", 325);
    result.pushKV("service_fee_cents", 2);
    result.pushKV("user_total_cents", 327);
    result.pushKV("expires_at", int64_t{2000000000});
    result.pushKV("result_status", UniValue{UniValue::VNULL});
    result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    result.pushKV("authorization_required", authorization_required);
    result.pushKV("authorization_commitment", commitment);
    return result;
}

} // namespace

void PaymasterWidgetTests::paymasterLiquidityPolicyDefaultsAndApprovalGuard()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QTabWidget* paymaster_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QWidget* liquidity_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterLiquidityPage"));
    QVERIFY(paymaster_tabs != nullptr);
    QVERIFY(liquidity_page != nullptr);
    const int liquidity_index = paymaster_tabs->indexOf(liquidity_page);
    QVERIFY(liquidity_index >= 0);
    paymaster_tabs->setTabEnabled(liquidity_index, true);
    QVERIFY(liquidity_page->isEnabled());

    QCheckBox* automatic = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterAutomaticReplenishment"));
    QCheckBox* approved = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QLineEdit* per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLineEdit* per_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerHour"));
    QLineEdit* per_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerDay"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QSpinBox* operational_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalDgbSlots"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* maintenance_fee_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"));
    QLabel* target_save_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityTargetSaveStatus"));
    QPushButton* save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QPushButton* restore = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterRestoreLiquidityDefaults"));
    QPushButton* advanced = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterAdvancedLiquidityToggle"));
    QPushButton* primary_save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicyPrimary"));

    QVERIFY(automatic != nullptr);
    QVERIFY(approved != nullptr);
    QVERIFY(per_transaction != nullptr);
    QVERIFY(per_hour != nullptr);
    QVERIFY(per_day != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(operational_dgb != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(target_save_status != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(restore != nullptr);
    QVERIFY(advanced != nullptr);
    QVERIFY(primary_save != nullptr);

    paymaster_tabs->setCurrentIndex(liquidity_index);
    QVERIFY(primary_save->isVisibleTo(liquidity_page));
    QCOMPARE(primary_save->text(), QStringLiteral("Save liquidity settings"));
    advanced->setChecked(true);
    QVERIFY(save->isVisibleTo(liquidity_page));
    QCOMPARE(save->text(), QStringLiteral("Save liquidity targets"));
    QVERIFY(target_save_status->text().contains(
        QStringLiteral("Not saved yet")));

    QVERIFY(automatic->isChecked());
    QVERIFY(!approved->isChecked());
    QCOMPARE(admission_dgb->value(), 3);
    QCOMPARE(operational_dgb->value(), 1);
    QCOMPARE(admission_carriers->value(), 3);
    QCOMPARE(operational_carriers->value(), 1);
    const qint64 transaction_limit = per_transaction->text().toLongLong();
    const qint64 hourly_limit = per_hour->text().toLongLong();
    const qint64 daily_limit = per_day->text().toLongLong();
    QVERIFY(transaction_limit > 0);
    QVERIFY(hourly_limit >= transaction_limit);
    QVERIFY(daily_limit >= hourly_limit);

    approved->setChecked(true);
    automatic->setChecked(false);
    admission_dgb->setValue(8);
    per_transaction->setText(QStringLiteral("42"));
    QVERIFY(status->text().contains(QStringLiteral("Unsaved")));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Not saved")));
    restore->click();
    QVERIFY(automatic->isChecked());
    QVERIFY(!approved->isChecked());
    QCOMPARE(admission_dgb->value(), 3);
    QCOMPARE(per_transaction->text(), QStringLiteral("10000000"));
    QVERIFY(status->text().contains(QStringLiteral("Paid maintenance is disabled")));
    QVERIFY(save->toolTip().contains(QStringLiteral("does not create a transaction")));
    QVERIFY(primary_save->toolTip().contains(
        QStringLiteral("does not create a transaction")));
}

void PaymasterWidgetTests::paymasterLiquidityPolicySavePersistsVisibleValues()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QStringList commands;
    std::vector<UniValue> parameters;
    bool malformed_ack{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterliquiditypolicy") {
                if (malformed_ack) return UniValue{UniValue::VOBJ};
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QCheckBox* automatic = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterAutomaticReplenishment"));
    QCheckBox* approved = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QSpinBox* operational_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalDgbSlots"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLineEdit* per_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerHour"));
    QLineEdit* per_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerDay"));
    QPushButton* save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QPushButton* primary_save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicyPrimary"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"));
    QLabel* target_save_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityTargetSaveStatus"));
    QVERIFY(automatic != nullptr);
    QVERIFY(approved != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(operational_dgb != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(per_transaction != nullptr);
    QVERIFY(per_hour != nullptr);
    QVERIFY(per_day != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(primary_save != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(target_save_status != nullptr);

    automatic->setChecked(true);
    // Keeping paid maintenance disabled avoids a modal approval dialog while
    // still verifying that all displayed targets and finite limits are sent.
    approved->setChecked(false);
    admission_dgb->setValue(5);
    operational_dgb->setValue(2);
    admission_carriers->setValue(4);
    operational_carriers->setValue(2);
    per_transaction->setText(QStringLiteral("12000000"));
    per_hour->setText(QStringLiteral("60000000"));
    per_day->setText(QStringLiteral("240000000"));
    primary_save->click();

    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterliquiditypolicy"));
    QVERIFY(parameters.at(0).isArray());
    const UniValue& policy = parameters.at(0)[0];
    QCOMPARE(policy.find_value("automatic_replenishment").get_bool(), true);
    QCOMPARE(policy.find_value("paid_maintenance_approved").get_bool(), false);
    QCOMPARE(policy.find_value("target_admission_dgb").getInt<int>(), 5);
    QCOMPARE(policy.find_value("target_operational_dgb").getInt<int>(), 2);
    QCOMPARE(policy.find_value("target_admission_carriers").getInt<int>(), 4);
    QCOMPARE(policy.find_value("target_operational_carriers").getInt<int>(), 2);
    QCOMPARE(policy.find_value(
                 "maximum_maintenance_fee_per_transaction_satoshis")
                 .getInt<qint64>(),
             12000000);
    QCOMPARE(policy.find_value("maximum_maintenance_fee_per_hour_satoshis")
                 .getInt<qint64>(),
             60000000);
    QCOMPARE(policy.find_value("maximum_maintenance_fee_per_day_satoshis")
                 .getInt<qint64>(),
             240000000);
    QVERIFY(status->text().contains(QStringLiteral("saved successfully")));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Saved")));
    QVERIFY(commands.contains(QStringLiteral("getpaymasterinfo")));

    // A transport-level success is not a persistence acknowledgement. Keep
    // the edit dirty when Core does not echo the complete canonical policy.
    malformed_ack = true;
    admission_dgb->setValue(6);
    primary_save->click();
    QVERIFY(status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Not saved")));
}

void PaymasterWidgetTests::paymasterLiquidityMaintenanceStatesAreReadable()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QLabel* state = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceState"));
    QLabel* next_step = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceNextStep"));
    QLabel* cost = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceCost"));
    QLabel* slot_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquiditySlotStatus"));
    QLabel* budget = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityBudgetStatus"));
    QPushButton* approve = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterApproveLiquidityMaintenance"));
    QGroupBox* maintenance_card = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterLiquidityMaintenanceCard"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* maintenance_fee_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QCheckBox* paid_maintenance = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QPushButton* maintenance_limits = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterMaintenanceLimitsToggle"));
    QPushButton* save_policy = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QVERIFY(state != nullptr);
    QVERIFY(next_step != nullptr);
    QVERIFY(cost != nullptr);
    QVERIFY(slot_status != nullptr);
    QVERIFY(budget != nullptr);
    QVERIFY(approve != nullptr);
    QVERIFY(maintenance_card != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(maintenance_fee_per_transaction != nullptr);
    QVERIFY(paid_maintenance != nullptr);
    QVERIFY(maintenance_limits != nullptr);
    QVERIFY(save_policy != nullptr);

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_target_configuration", true, true, 0, 0,
        /*targets_satisfy_provider_policy=*/false));
    QVERIFY(state->text().contains(
        QStringLiteral("cannot make this offer ready")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("one operational DigiDollar carrier")));
    QVERIFY(cost->text().contains(
        QStringLiteral("saved operational carrier target is zero")));
    QVERIFY(approve->text().contains(
        QStringLiteral("required liquidity targets")));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));

    // Repairing an old zero-carrier policy is one visible, reviewable save
    // step: load the minimum targets, expose the finite fee limits and wait
    // for the operator to use the explicit save/approval action.
    tab.setPaymasterRpcExecutorForTesting(
        [](const std::string&, const UniValue&) {
            return UniValue{UniValue::VOBJ};
        });
    admission_carriers->setValue(0);
    operational_carriers->setValue(0);
    paid_maintenance->setChecked(false);
    approve->click();
    QCOMPARE(admission_carriers->value(), 3);
    QCOMPARE(operational_carriers->value(), 1);
    QVERIFY(paid_maintenance->isChecked());
    QVERIFY(maintenance_limits->isChecked());
    QCOMPARE(save_policy->text(),
             QStringLiteral("Save targets and approve refill…"));
    QVERIFY(save_policy->isEnabledTo(save_policy->parentWidget()));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_maintenance_approval", true, false, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("targets are saved"), Qt::CaseInsensitive));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No target update is needed")));
    QVERIFY(cost->text().contains(QStringLiteral("not approved")));
    QVERIFY(approve->text().contains(
        QStringLiteral("bounded refill costs")));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("waiting for provider start")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("Start the provider")));
    QVERIFY(cost->text().contains(QStringLiteral("approved within finite limits")));
    QVERIFY(slot_status->text().contains(
        QStringLiteral("Operational DGB: 0 ready")));
    QVERIFY(budget->text().contains(QStringLiteral("reserved: 0.01000000 DGB")));
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("waiting"));

    // A stale Regtest tip prevents creation of the maintenance transaction.
    // The UI must not claim that replenishment is already happening while the
    // backend still reports zero pending slots.
    UniValue node_wait{UniValue::VOBJ};
    node_wait.pushKV("wallet_eligible", true);
    node_wait.pushKV("enabled", true);
    node_wait.pushKV("ready", false);
    node_wait.pushKV("wallet_locked", false);
    UniValue node_wait_errors{UniValue::VARR};
    node_wait_errors.push_back("PAYMASTER_NODE_NOT_READY");
    node_wait.pushKV("readiness_errors", std::move(node_wait_errors));
    tab.setPaymasterReadinessStatusForTesting(node_wait);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("waiting for a fresh Regtest block")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No refill transaction has been created yet")));
    QVERIFY(cost->text().contains(QStringLiteral("1 missing and 0 pending")));

    // The provider service may be running while Core safely rejects creation
    // of a refill whose estimated network fee exceeds the explicitly approved
    // per-transaction cap. This is an operator action, not an in-progress
    // transaction: zero pending slots must remain visible and the review
    // action must lead directly to the finite maintenance limits.
    UniValue fee_exceeded{UniValue::VOBJ};
    fee_exceeded.pushKV("wallet_eligible", true);
    fee_exceeded.pushKV("enabled", true);
    fee_exceeded.pushKV("ready", false);
    fee_exceeded.pushKV("running", true);
    fee_exceeded.pushKV("wallet_locked", false);
    fee_exceeded.pushKV("service_state", "replenishing_liquidity");
    fee_exceeded.pushKV("last_service_error",
                        "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    UniValue fee_errors{UniValue::VARR};
    fee_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    fee_exceeded.pushKV("readiness_errors", std::move(fee_errors));
    tab.setPaymasterReadinessStatusForTesting(fee_exceeded);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(QStringLiteral("refill paused"),
                                   Qt::CaseInsensitive));
    QVERIFY(next_step->text().contains(QStringLiteral("0.10000000 DGB")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No transaction was created")));
    QVERIFY(cost->text().contains(QStringLiteral("1 missing and 0 pending")));
    QCOMPARE(approve->text(), QStringLiteral("Review refill cost limit…"));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));
    approve->click();
    QVERIFY(maintenance_limits->isChecked());
    // Runtime settings remain locked while the provider is running. The
    // review action still exposes the exact finite limit and explains that
    // the provider must be stopped before that authorization can be changed.
    QVERIFY(!maintenance_fee_per_transaction->isEnabled());
    QVERIFY(tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"))->text().contains(
            QStringLiteral("no transaction has been created"),
            Qt::CaseInsensitive));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_liquidity_confirmation", true, true, 0, 1));
    QVERIFY(state->text().contains(QStringLiteral("waiting for confirmation")));
    QVERIFY(next_step->text().contains(QStringLiteral("1 pending slot")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.01000000 DGB in network fees")));
    QVERIFY(cost->text().contains(
        QStringLiteral("confirmed costs appear in Finances")));

    UniValue refill_ready{UniValue::VOBJ};
    refill_ready.pushKV("wallet_eligible", true);
    refill_ready.pushKV("enabled", true);
    refill_ready.pushKV("ready", true);
    refill_ready.pushKV("running", true);
    refill_ready.pushKV("wallet_locked", false);
    refill_ready.pushKV("service_state", "active");
    refill_ready.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(refill_ready);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "ready", true, true, 0));
    QVERIFY(state->text().contains(QStringLiteral("targets are satisfied")));
    QVERIFY(next_step->text().contains(QStringLiteral("reused automatically")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.02000000 DGB in the rolling hour")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.03000000 DGB in the rolling day")));
    QVERIFY(approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("ready"));
}

void PaymasterWidgetTests::paymasterExternalReadinessIsSeparatedFromConfiguration()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QLabel* next_step = tab.findChild<QLabel*>(
        QStringLiteral("paymasterNextStep"));
    QLabel* operation = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOverviewOperationStatus"));
    QLabel* liquidity = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOverviewLiquidityStatus"));
    QPushButton* liquidity_action = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewLiquidityAction"));
    QPushButton* operation_action = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewOperationAction"));
    QLabel* blockchain = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalBlockchainStatus"));
    QLabel* txindex = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalTxIndexStatus"));
    QLabel* broadcast = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalBroadcastStatus"));
    QLabel* activation = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalActivationStatus"));
    QLabel* oracle = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOracleStatus"));
    QLabel* oracle_note = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOracleNote"));
    QGroupBox* prerequisites = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterOverviewNextStep"));
    QLabel* other_requirement = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOtherStatus"));
    QPushButton* wizard = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSetupWizard"));
    QPushButton* start = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterStartProvider"));
    QLabel* provider_id = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOfferIdentityId"));
    QVERIFY(provider_status != nullptr);
    QVERIFY(next_step != nullptr);
    QVERIFY(operation != nullptr);
    QVERIFY(liquidity != nullptr);
    QVERIFY(liquidity_action != nullptr);
    QVERIFY(operation_action != nullptr);
    QVERIFY(blockchain != nullptr);
    QVERIFY(txindex != nullptr);
    QVERIFY(broadcast != nullptr);
    QVERIFY(activation != nullptr);
    QVERIFY(oracle != nullptr);
    QVERIFY(oracle_note != nullptr);
    QVERIFY(prerequisites != nullptr);
    QVERIFY(other_requirement != nullptr);
    QVERIFY(wizard != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(provider_id != nullptr);

    UniValue waiting{UniValue::VOBJ};
    waiting.pushKV("wallet_eligible", true);
    waiting.pushKV("enabled", true);
    waiting.pushKV("ready", false);
    waiting.pushKV("wallet_locked", false);
    const QString full_provider_id{
        QStringLiteral("371aaaaeea392e01f786ca55030708320e0f85cfed0de0ffcc820faa14d9af3d")};
    waiting.pushKV("provider_id", full_provider_id.toStdString());
    UniValue waiting_errors{UniValue::VARR};
    waiting_errors.push_back("PAYMASTER_NODE_NOT_READY");
    waiting.pushKV("readiness_errors", std::move(waiting_errors));
    waiting.pushKV("oracle_price_micro_usd", 0);
    tab.setPaymasterReadinessStatusForTesting(waiting);

    QVERIFY(provider_status->text().contains(
        QStringLiteral("Provider fully configured")));
    QVERIFY(!provider_status->text().contains(QStringLiteral("Setup incomplete")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("Waiting for blockchain synchronization")));
    QVERIFY(operation->text().contains(QStringLiteral("Waiting")));
    QVERIFY(operation->text().contains(QStringLiteral("Regtest block")));
    QVERIFY(blockchain->text().contains(QStringLiteral("Waiting")));
    QVERIFY(txindex->text().contains(QStringLiteral("Ready")));
    QVERIFY(broadcast->text().contains(QStringLiteral("Waiting")));
    QVERIFY(activation->text().contains(QStringLiteral("Ready")));
    QVERIFY(oracle->text().contains(QStringLiteral("Not available yet")));
    QVERIFY(oracle_note->text().contains(
        QStringLiteral("Paymaster transfers do not require an Oracle price")));
    QVERIFY(!oracle_note->isHidden());
    QVERIFY(wizard->text().contains(QStringLiteral("Review setup")));
    QVERIFY(!start->isEnabled());
    QVERIFY(start->toolTip().contains(QStringLiteral("No provider setting")));
    QCOMPARE(provider_id->text(), full_provider_id);
    QVERIFY(provider_id->wordWrap());
    QVERIFY(provider_id->textInteractionFlags().testFlag(
        Qt::TextSelectableByMouse));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("waiting"));
    QVERIFY(other_requirement->isHidden());

    // A transient inventory shortage and a stale Regtest tip must not be
    // presented as unsaved provider settings. The Liquidity card owns the
    // refill action, while Operations explains that a fresh block is needed.
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_maintenance_approval", true, false, 1));
    UniValue stale_with_missing_slot{UniValue::VOBJ};
    stale_with_missing_slot.pushKV("wallet_eligible", true);
    stale_with_missing_slot.pushKV("enabled", true);
    stale_with_missing_slot.pushKV("ready", false);
    stale_with_missing_slot.pushKV("wallet_locked", false);
    UniValue stale_errors{UniValue::VARR};
    stale_errors.push_back("PAYMASTER_NODE_NOT_READY");
    stale_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    stale_with_missing_slot.pushKV("readiness_errors", std::move(stale_errors));
    stale_with_missing_slot.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(stale_with_missing_slot);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("fully configured")));
    QVERIFY(operation->text().contains(QStringLiteral("Regtest block")));
    QVERIFY(!operation->text().contains(
        QStringLiteral("complete the remaining setup")));
    QVERIFY(other_requirement->isHidden());

    UniValue ready{UniValue::VOBJ};
    ready.pushKV("wallet_eligible", true);
    ready.pushKV("enabled", true);
    ready.pushKV("ready", true);
    ready.pushKV("wallet_locked", false);
    ready.pushKV("readiness_errors", UniValue{UniValue::VARR});
    ready.pushKV("oracle_price_micro_usd", 0);
    tab.setPaymasterReadinessStatusForTesting(ready);
    QVERIFY(start->isEnabled());
    QVERIFY(oracle->text().contains(QStringLiteral("Not available yet")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));

    // Once external readiness is green and bounded maintenance is approved,
    // a stopped provider must be presented with the actual next action. It is
    // not still being configured and it cannot prepare a refill while stopped.
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    UniValue stopped_with_missing_slot{UniValue::VOBJ};
    stopped_with_missing_slot.pushKV("wallet_eligible", true);
    stopped_with_missing_slot.pushKV("enabled", true);
    stopped_with_missing_slot.pushKV("ready", false);
    stopped_with_missing_slot.pushKV("running", false);
    stopped_with_missing_slot.pushKV("wallet_locked", false);
    stopped_with_missing_slot.pushKV("service_state", "stopped");
    UniValue stopped_errors{UniValue::VARR};
    stopped_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    stopped_with_missing_slot.pushKV("readiness_errors",
                                     std::move(stopped_errors));
    stopped_with_missing_slot.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(stopped_with_missing_slot);
    QVERIFY(next_step->text().contains(
        QStringLiteral("All external prerequisites are ready")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(operation->text().contains(
        QStringLiteral("start the provider to restore missing liquidity")));
    QCOMPARE(operation_action->text(),
             QStringLiteral("Start and restore liquidity"));
    QVERIFY(start->isEnabled());
    QCOMPARE(start->text(), QStringLiteral("Start and restore liquidity"));

    // Both compact Overview actions promise the same operation as the primary
    // button. On a native platform, verify that they open the guarded start
    // confirmation instead of navigating to Liquidity or Operations. Qt 5.15
    // minimal/offscreen can crash in QMessageBox::showEvent, so headless runs
    // retain the state/action assertions above without opening a dialog.
    const bool native_dialogs_available =
        QApplication::platformName() != QLatin1String("minimal") &&
        QApplication::platformName() != QLatin1String("offscreen");
    if (native_dialogs_available) {
        const auto verify_start_action = [&](QPushButton* action) {
            bool confirmation_seen{false};
            QTimer::singleShot(0, [&] {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    auto* confirmation = qobject_cast<QMessageBox*>(widget);
                    if (!confirmation ||
                        confirmation->windowTitle() !=
                            QLatin1String("Start Paymaster provider")) {
                        continue;
                    }
                    confirmation_seen = true;
                    confirmation->button(QMessageBox::Cancel)->click();
                    break;
                }
            });
            const int overview_index = operator_tabs->currentIndex();
            action->click();
            QVERIFY(confirmation_seen);
            QCOMPARE(operator_tabs->currentIndex(), overview_index);
        };
        verify_start_action(liquidity_action);
        verify_start_action(operation_action);
    }

    // A running provider can still pause before creating a maintenance
    // transaction when its estimated fee exceeds the operator-approved cap.
    // Overview must stop claiming that a refill is being prepared and route
    // both cards to the cost-limit review instead.
    UniValue fee_limited{UniValue::VOBJ};
    fee_limited.pushKV("wallet_eligible", true);
    fee_limited.pushKV("enabled", true);
    fee_limited.pushKV("ready", false);
    fee_limited.pushKV("running", true);
    fee_limited.pushKV("wallet_locked", false);
    fee_limited.pushKV("service_state", "replenishing_liquidity");
    fee_limited.pushKV("last_service_error",
                       "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    UniValue fee_limited_errors{UniValue::VARR};
    fee_limited_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    fee_limited.pushKV("readiness_errors",
                       std::move(fee_limited_errors));
    tab.setPaymasterReadinessStatusForTesting(fee_limited);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(liquidity->text().contains(
        QStringLiteral("cost exceeds the approved limit")));
    QVERIFY(operation->text().contains(
        QStringLiteral("exceeds the approved cost limit")));
    QCOMPARE(liquidity_action->text(),
             QStringLiteral("Review refill cost limit"));
    QCOMPARE(operation_action->text(),
             QStringLiteral("Review refill cost limit"));

    UniValue activation_wait{UniValue::VOBJ};
    activation_wait.pushKV("wallet_eligible", true);
    activation_wait.pushKV("enabled", true);
    activation_wait.pushKV("ready", false);
    activation_wait.pushKV("wallet_locked", false);
    UniValue activation_errors{UniValue::VARR};
    activation_errors.push_back("PAYMASTER_NODE_NOT_READY");
    activation_wait.pushKV("readiness_errors", std::move(activation_errors));
    activation_wait.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(activation_wait);
    QVERIFY(!start->isEnabled());
    QVERIFY(oracle->text().contains(QStringLiteral("0.006500")));

    UniValue missing_policy{UniValue::VOBJ};
    missing_policy.pushKV("wallet_eligible", true);
    missing_policy.pushKV("enabled", true);
    missing_policy.pushKV("ready", false);
    missing_policy.pushKV("wallet_locked", false);
    UniValue setup_errors{UniValue::VARR};
    setup_errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
    missing_policy.pushKV("readiness_errors", std::move(setup_errors));
    missing_policy.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(missing_policy);
    QVERIFY(provider_status->text().contains(QStringLiteral("Setup incomplete")));
    QVERIFY(operation->text().contains(QStringLiteral("Action required")));
    // A provider-configuration error belongs to the task cards, not to the
    // independent node-prerequisites card. The latter may truthfully be ready
    // while Offer or Liquidity still needs operator action.
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(other_requirement->isHidden());

    UniValue incomplete_targets{UniValue::VOBJ};
    incomplete_targets.pushKV("wallet_eligible", true);
    incomplete_targets.pushKV("enabled", true);
    incomplete_targets.pushKV("ready", false);
    incomplete_targets.pushKV("wallet_locked", false);
    UniValue target_errors{UniValue::VARR};
    target_errors.push_back("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE");
    target_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    incomplete_targets.pushKV("readiness_errors", std::move(target_errors));
    incomplete_targets.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(incomplete_targets);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("liquidity change")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("saved liquidity targets")));
    QVERIFY(operation->text().contains(
        QStringLiteral("payment-carrier target")));
    QVERIFY(!start->isEnabled());
    QVERIFY(start->toolTip().contains(
        QStringLiteral("cannot make this provider ready")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(other_requirement->isHidden());

    UniValue unknown_failure{UniValue::VOBJ};
    unknown_failure.pushKV("wallet_eligible", true);
    unknown_failure.pushKV("enabled", true);
    unknown_failure.pushKV("ready", false);
    unknown_failure.pushKV("wallet_locked", false);
    UniValue unknown_errors{UniValue::VARR};
    unknown_errors.push_back("PAYMASTER_TEST_UNKNOWN_REQUIREMENT");
    unknown_failure.pushKV("readiness_errors", std::move(unknown_errors));
    unknown_failure.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(unknown_failure);
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("error"));
    QCOMPARE(other_requirement->property("statusKind").toString(),
             QStringLiteral("error"));
    QVERIFY(other_requirement->text().startsWith(QStringLiteral("!")));
}

void PaymasterWidgetTests::paymasterPendingStartUsesLiveStatusInsteadOfStaleModal()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(provider_status != nullptr);

    UniValue pending{UniValue::VOBJ};
    pending.pushKV("running", true);
    pending.pushKV("ready", false);
    pending.pushKV("operation_mode", "automatic");
    pending.pushKV("service_state", "replenishing_liquidity");

    const auto message_box_count = [] {
        const QWidgetList widgets = QApplication::topLevelWidgets();
        return std::count_if(
            widgets.cbegin(), widgets.cend(),
            [](QWidget* widget) { return qobject_cast<QMessageBox*>(widget); });
    };
    const int message_boxes_before = message_box_count();
    tab.setPaymasterStartResultForTesting(pending);
    const int message_boxes_after = message_box_count();

    QCOMPARE(message_boxes_after, message_boxes_before);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("Provider start accepted")));
    QVERIFY(provider_status->text().contains(
        QStringLiteral("restoring the saved liquidity targets")));

    tab.setPaymasterRpcExecutorForTesting(
        [](const std::string&, const UniValue&) {
            return UniValue{UniValue::VOBJ};
        });
    UniValue malformed{UniValue::VOBJ};
    malformed.pushKV("running", true);
    malformed.pushKV("ready", true);
    malformed.pushKV("service_state", "active");
    tab.setPaymasterStartResultForTesting(malformed);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
}

void PaymasterWidgetTests::paymasterCarrierWithdrawalActionsFailClosed()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QTabWidget* paymaster_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QWidget* liquidity_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterLiquidityPage"));
    QVERIFY(paymaster_tabs != nullptr);
    QVERIFY(liquidity_page != nullptr);
    const int liquidity_index = paymaster_tabs->indexOf(liquidity_page);
    QVERIFY(liquidity_index >= 0);
    paymaster_tabs->setTabEnabled(liquidity_index, true);
    QVERIFY(liquidity_page->isEnabled());

    QLabel* value = tab.findChild<QLabel*>(
        QStringLiteral("paymasterCarrierValueStatus"));
    QLabel* recycling = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityRecyclingStatus"));
    QComboBox* selection = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterReleaseCarrierSelection"));
    QPushButton* preview_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierExcess"));
    QPushButton* execute_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierExcess"));
    QPushButton* preview_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierRelease"));
    QPushButton* execute_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierRelease"));
    QVERIFY(value != nullptr);
    QVERIFY(recycling != nullptr);
    QVERIFY(selection != nullptr);
    QVERIFY(preview_excess != nullptr);
    QVERIFY(execute_excess != nullptr);
    QVERIFY(preview_release != nullptr);
    QVERIFY(execute_release != nullptr);

    QVERIFY(!preview_excess->isEnabled());
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(!execute_excess->isEnabled());
    QVERIFY(!execute_release->isEnabled());

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_liquidity_confirmation", true, true, 0, 1));
    QVERIFY(value->text().contains(QStringLiteral("1.00 DD")));
    QVERIFY(value->text().contains(QStringLiteral("0.06 DD")));
    QVERIFY(preview_excess->isEnabled());
    QVERIFY(!execute_excess->isEnabled());

    tab.setPaymasterLiquidityPoolForTesting(PaymasterLiquidityPoolStatus());
    QCOMPARE(selection->count(), 1);
    QVERIFY(selection->currentText().contains(QStringLiteral("1.06 DD")));
    QVERIFY(recycling->text().contains(QStringLiteral("Carrier is being reused: 1.03 DD")));
    QVERIFY(recycling->text().contains(QStringLiteral("DGB successor slot is being reused: 0.10000000 DGB")));
    QVERIFY(preview_release->isEnabled());
    QVERIFY(!execute_release->isEnabled());

    UniValue malformed_entry{UniValue::VOBJ};
    malformed_entry.pushKV("state", "available");
    malformed_entry.pushKV("purpose", "operational");
    malformed_entry.pushKV("asset", "dd_carrier");
    malformed_entry.pushKV("dd_cents", 106);
    UniValue malformed_entries{UniValue::VARR};
    malformed_entries.push_back(std::move(malformed_entry));
    UniValue malformed_pool{UniValue::VOBJ};
    malformed_pool.pushKV("pool", std::move(malformed_entries));
    tab.setPaymasterLiquidityPoolForTesting(malformed_pool);
    QCOMPARE(selection->count(), 0);
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(recycling->text().contains(
        QStringLiteral("incomplete"), Qt::CaseInsensitive));

    UniValue empty_pool{UniValue::VOBJ};
    empty_pool.pushKV("pool", UniValue{UniValue::VARR});
    tab.setPaymasterLiquidityPoolForTesting(empty_pool);
    QCOMPARE(selection->count(), 0);
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(!execute_release->isEnabled());
}

void PaymasterWidgetTests::paymasterCarrierWithdrawalPreviewsArePlanBound()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::vector<UniValue> parameters;
    const qint64 expiry = QDateTime::currentSecsSinceEpoch() + 600;
    const std::string excess_plan(64, 'd');
    const std::string release_plan(64, 'e');
    bool malformed_preview{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command != "withdrawpaymastercarrier") {
                throw std::runtime_error("unexpected injected carrier RPC");
            }
            UniValue result{UniValue::VOBJ};
            const QString mode = QString::fromStdString(
                params[0].find_value("mode").get_str());
            result.pushKV("executed", false);
            result.pushKV("plan_id",
                          mode == QLatin1String("all_excess")
                              ? excess_plan
                              : release_plan);
            result.pushKV("mode", mode.toStdString());
            result.pushKV("source_carriers", 1);
            result.pushKV("expires_at", expiry);
            if (malformed_preview) return result;
            result.pushKV("withdrawable_excess_cents", 6);
            result.pushKV("retained_carrier_cents", 100);
            result.pushKV("estimated_network_fee_satoshis", 1000);
            result.pushKV("operational_carrier_target", 0);
            return result;
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "ready", true, true, 0));
    tab.setPaymasterLiquidityPoolForTesting(PaymasterLiquidityPoolStatus());

    QPushButton* preview_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierExcess"));
    QPushButton* execute_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierExcess"));
    QPushButton* preview_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierRelease"));
    QPushButton* execute_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierRelease"));
    QComboBox* selection = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterReleaseCarrierSelection"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterCarrierWithdrawalStatus"));
    QVERIFY(preview_excess != nullptr);
    QVERIFY(execute_excess != nullptr);
    QVERIFY(preview_release != nullptr);
    QVERIFY(execute_release != nullptr);
    QVERIFY(selection != nullptr);
    QVERIFY(status != nullptr);

    preview_excess->click();
    QCOMPARE(commands.value(0), QStringLiteral("withdrawpaymastercarrier"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("mode").get_str()),
             QStringLiteral("all_excess"));
    QCOMPARE(parameters.at(0)[0].find_value("execute").get_bool(), false);
    QVERIFY(execute_excess->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("Preview only")));
    QVERIFY(status->text().contains(QStringLiteral("0.06 DD")));

    preview_release->click();
    QCOMPARE(commands.value(1), QStringLiteral("withdrawpaymastercarrier"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(1)[0].find_value("mode").get_str()),
             QStringLiteral("release_slot"));
    QCOMPARE(parameters.at(1)[0].find_value("execute").get_bool(), false);
    QVERIFY(parameters.at(1)[0].find_value("txid").isStr());
    QVERIFY(parameters.at(1)[0].find_value("vout").isNum());
    QVERIFY(execute_release->isEnabled());
    QVERIFY(!execute_excess->isEnabled());

    // An object-shaped response is not sufficient: the preview must carry
    // every mode-specific amount and remain bound to a valid plan id.
    malformed_preview = true;
    preview_release->click();
    QVERIFY(!execute_release->isEnabled());
    QVERIFY(status->text().contains(
        QStringLiteral("complete response"), Qt::CaseInsensitive));

    // Changing the selected outpoint invalidates the reviewed plan before an
    // execution can be offered. Re-applying the same index is not a change, so
    // clear the pool to exercise the fail-closed invalidation path.
    UniValue empty_pool{UniValue::VOBJ};
    empty_pool.pushKV("pool", UniValue{UniValue::VARR});
    tab.setPaymasterLiquidityPoolForTesting(empty_pool);
    QVERIFY(!execute_release->isEnabled());
}

void PaymasterWidgetTests::paymasterSafetyControlsDefaultFailClosed()
{
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QWidget* safety_page = tab.findChild<QWidget*>(QStringLiteral("paymasterSafetyPolicyPage"));
    QLabel* warning = tab.findChild<QLabel*>(QStringLiteral("paymasterSafetyPolicyWarning"));
    QLabel* provider_status = tab.findChild<QLabel*>(QStringLiteral("paymasterProviderSafetyStatus"));
    QLabel* client_status = tab.findChild<QLabel*>(QStringLiteral("paymasterClientSafetyStatus"));
    QPushButton* enable_provider = tab.findChild<QPushButton*>(QStringLiteral("paymasterEnableProvider"));
    QPushButton* save_provider = tab.findChild<QPushButton*>(QStringLiteral("savePaymasterProviderSafetyPolicy"));
    QPushButton* save_client = tab.findChild<QPushButton*>(QStringLiteral("savePaymasterClientSafetyPolicy"));
    QLineEdit* public_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerHour"));
    QLineEdit* public_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerDay"));
    QSpinBox* quote_total = tab.findChild<QSpinBox*>(QStringLiteral("paymasterSafetyMaxActiveQuotesTotal"));
    QSpinBox* client_transfer = tab.findChild<QSpinBox*>(QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QSpinBox* client_day = tab.findChild<QSpinBox*>(QStringLiteral("paymasterClientSafetyMaxFeePerDay"));

    QVERIFY(safety_page != nullptr);
    QVERIFY(warning != nullptr);
    QVERIFY(provider_status != nullptr);
    QVERIFY(client_status != nullptr);
    QVERIFY(enable_provider != nullptr);
    QVERIFY(save_provider != nullptr);
    QVERIFY(save_client != nullptr);
    QVERIFY(public_hour != nullptr);
    QVERIFY(public_day != nullptr);
    QVERIFY(quote_total != nullptr);
    QVERIFY(client_transfer != nullptr);
    QVERIFY(client_day != nullptr);
    QVERIFY(warning->text().contains(QStringLiteral("spending brake")));
    QVERIFY(provider_status->text().contains(QStringLiteral("not configured")));
    QVERIFY(client_status->text().contains(QStringLiteral("not configured")));
    QCOMPARE(public_hour->text(), QStringLiteral("0.00000000"));
    QCOMPARE(public_day->text(), QStringLiteral("0.00000000"));
    QVERIFY(!enable_provider->isEnabled());
    QVERIFY(quote_total->value() > 0);
    QVERIFY(client_day->value() >= client_transfer->value());

    const QStringList funding_prefixes{
        QStringLiteral("paymasterSafetyUserPaid"),
        QStringLiteral("paymasterSafetyPublicSponsored"),
        QStringLiteral("paymasterSafetyRestrictedSponsored"),
    };
    const QStringList limit_suffixes{
        QStringLiteral("MaxNetworkFeePerTransaction"),
        QStringLiteral("MaxReservedNetworkFee"),
        QStringLiteral("MaxNetworkFeePerHour"),
        QStringLiteral("MaxNetworkFeePerDay"),
        QStringLiteral("MaxCompletedPerHour"),
        QStringLiteral("MaxCompletedPerDay"),
    };
    for (const QString& prefix : funding_prefixes) {
        QVERIFY2(tab.findChild<QPushButton*>(prefix + QStringLiteral("RestoreDefaults")) != nullptr,
                 qPrintable(QStringLiteral("Missing Paymaster safety reset for %1")
                                .arg(prefix)));
        for (const QString& suffix : limit_suffixes) {
            const QString object_name = prefix + suffix;
            QVERIFY2(tab.findChild<QWidget*>(object_name) != nullptr,
                     qPrintable(QStringLiteral("Missing Paymaster safety control %1")
                                    .arg(object_name)));
        }
    }
    const QStringList quote_controls{
        QStringLiteral("paymasterSafetyMaxActiveQuotesTotal"),
        QStringLiteral("paymasterSafetyMaxActiveQuotesPerNetgroup"),
        QStringLiteral("paymasterSafetyMaxActiveQuotesPerRecipient"),
        QStringLiteral("paymasterSafetyMaxQuoteRequestsPerNetgroupMinute"),
    };
    for (const QString& object_name : quote_controls) {
        QVERIFY2(tab.findChild<QSpinBox*>(object_name) != nullptr,
                 qPrintable(QStringLiteral("Missing Paymaster quote limit %1")
                                .arg(object_name)));
    }
}

void PaymasterWidgetTests::paymasterSafetyPolicyDisplaysFiniteDisabledSemantics()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QLabel* user_paid_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyUserPaidMode"));
    QLabel* public_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMode"));
    QLabel* restricted_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyRestrictedSponsoredMode"));
    QLabel* client_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterClientSafetyMode"));
    QLineEdit* public_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerTransaction"));
    QSpinBox* client_transfer = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QSpinBox* client_day = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerDay"));

    QVERIFY(user_paid_mode != nullptr);
    QVERIFY(public_mode != nullptr);
    QVERIFY(restricted_mode != nullptr);
    QVERIFY(client_mode != nullptr);
    QVERIFY(public_per_transaction != nullptr);
    QVERIFY(client_transfer != nullptr);
    QVERIFY(client_day != nullptr);

    QVERIFY(user_paid_mode->text().contains(QStringLiteral("Unsaved limits")));
    QVERIFY(user_paid_mode->text().contains(QStringLiteral("DGB per transfer")));
    QVERIFY(public_mode->text().contains(QStringLiteral("Disabled")));
    QVERIFY(public_mode->text().contains(QStringLiteral("never means unlimited")));
    QVERIFY(restricted_mode->text().contains(QStringLiteral("Disabled")));

    public_per_transaction->setText(QStringLiteral("1"));
    QVERIFY(public_mode->text().contains(QStringLiteral("Incomplete")));
    QVERIFY(!public_mode->text().contains(QStringLiteral("unlimited")));

    client_transfer->setValue(0);
    client_day->setValue(0);
    QVERIFY(client_mode->text().contains(QStringLiteral("Zero service-fee budget")));
    QVERIFY(client_mode->text().contains(QStringLiteral("zero-fee")));
    QVERIFY(client_mode->text().contains(QStringLiteral("never means unlimited")));
    client_transfer->setValue(1);
    QVERIFY(client_mode->text().contains(QStringLiteral("Unsaved limits")));
}

void PaymasterWidgetTests::paymasterInjectedRpcCoversConfigurationWorkflows()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::vector<UniValue> parameters;
    bool malformed_operating_ack{false};
    bool malformed_provider_safety_ack{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterpolicy") {
                if (malformed_operating_ack) {
                    return UniValue{UniValue::VOBJ};
                }
                UniValue result = params[0];
                result.pushKV(
                    "policy_hash",
                    "2222222222222222222222222222222222222222222222222222222222222222");
                return result;
            }
            if (command == "setpaymastersafetypolicy") {
                if (malformed_provider_safety_ack) {
                    return UniValue{UniValue::VOBJ};
                }
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "getpaymasterinfo" ||
                command == "getpaymastersafetystatus") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });

    // A new wallet intentionally keeps configuration tabs locked until the
    // operator chooses guided or expert setup. This contract test exercises
    // the controls behind that onboarding gate, so unlock only the local tab
    // container without altering persisted settings.
    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QPushButton* create_identity = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterCreateIdentity"));
    QLineEdit* display_name = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterDisplayName"));
    QPushButton* save_policy = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterPolicy"));
    QPushButton* save_safety = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterProviderSafetyPolicy"));
    QPushButton* save_client_safety = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterClientSafetyPolicy"));
    QSpinBox* client_per_transaction = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QLabel* client_safety_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterClientSafetyStatus"));
    QLabel* provider_safety_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderSafetyStatus"));
    QPushButton* enable_provider = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterEnableProvider"));
    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(create_identity != nullptr);
    QVERIFY(display_name != nullptr);
    QVERIFY(save_policy != nullptr);
    QVERIFY(save_safety != nullptr);
    QVERIFY(save_client_safety != nullptr);
    QVERIFY(client_per_transaction != nullptr);
    QVERIFY(client_safety_status != nullptr);
    QVERIFY(provider_safety_status != nullptr);
    QVERIFY(enable_provider != nullptr);
    QVERIFY(provider_status != nullptr);

    display_name->setText(QStringLiteral("test-provider"));
    create_identity->setEnabled(true);
    create_identity->click();
    QCOMPARE(commands.value(0), QStringLiteral("createpaymasteridentity"));
    QVERIFY(parameters.at(0).isArray());
    QCOMPARE(QString::fromStdString(parameters.at(0)[0].get_str()),
             QStringLiteral("test-provider"));
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));

    commands.clear();
    parameters.clear();
    save_policy->setEnabled(true);
    save_policy->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterpolicy"));
    QVERIFY(parameters.at(0)[0].find_value("funding_models").isArray());
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));

    commands.clear();
    parameters.clear();
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_safety->setEnabled(true);
    save_safety->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymastersafetypolicy"));
    QVERIFY(parameters.at(0)[0].find_value("user_paid").isObject());
    QCOMPARE(commands.value(1), QStringLiteral("getpaymastersafetystatus"));

    commands.clear();
    parameters.clear();
    malformed_operating_ack = true;
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_policy->setEnabled(true);
    save_policy->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterpolicy"));
    QVERIFY(provider_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    malformed_operating_ack = false;

    commands.clear();
    parameters.clear();
    malformed_provider_safety_ack = true;
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_safety->setEnabled(true);
    save_safety->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymastersafetypolicy"));
    QVERIFY(provider_safety_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    malformed_provider_safety_ack = false;

    commands.clear();
    parameters.clear();
    client_per_transaction->setValue(101);
    save_client_safety->setEnabled(true);
    save_client_safety->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterclientsafetypolicy"));
    QVERIFY(client_safety_status->text().contains(
        QStringLiteral("not confirmed"), Qt::CaseInsensitive));

    commands.clear();
    parameters.clear();
    UniValue running{UniValue::VOBJ};
    running.pushKV("wallet_eligible", true);
    running.pushKV("settings_present", true);
    running.pushKV("enabled", true);
    running.pushKV("running", true);
    running.pushKV("ready", true);
    running.pushKV("wallet_locked", false);
    running.pushKV("has_identity", true);
    running.pushKV("has_policy", true);
    running.pushKV("has_safety_policy", true);
    running.pushKV("operation_mode", "automatic");
    running.pushKV("autostart", false);
    running.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running);
    save_policy->setEnabled(true);
    save_policy->click();
    QVERIFY(commands.isEmpty());
    QVERIFY(provider_status->text().contains(
        QStringLiteral("Stop the Paymaster provider")));

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterenabled") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("enabled", false);
                result.pushKV("running", false);
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error(
                    "injected follow-up refresh unavailable");
            }
            throw std::runtime_error(
                "unexpected configuration workflow RPC");
        });
    UniValue enabled_stopped{UniValue::VOBJ};
    enabled_stopped.pushKV("wallet_eligible", true);
    enabled_stopped.pushKV("settings_present", true);
    enabled_stopped.pushKV("enabled", true);
    enabled_stopped.pushKV("running", false);
    enabled_stopped.pushKV("ready", true);
    enabled_stopped.pushKV("wallet_locked", false);
    enabled_stopped.pushKV("has_identity", true);
    enabled_stopped.pushKV("has_policy", true);
    enabled_stopped.pushKV("has_safety_policy", true);
    enabled_stopped.pushKV("operation_mode", "automatic");
    enabled_stopped.pushKV("autostart", false);
    enabled_stopped.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(enabled_stopped);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QVERIFY(enable_provider->text().contains(QStringLiteral("Disable")));
    enable_provider->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterenabled"));
    QCOMPARE(parameters.at(0)[0].get_bool(), false);
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));
}

void PaymasterWidgetTests::paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::vector<UniValue> parameters;

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            UniValue result{UniValue::VOBJ};
            if (command == "preparepaymasterpool") {
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "5555555555555555555555555555555555555555555555555555555555555555");
                result.pushKV("admission_dgb_slots",
                              params[0].find_value(
                                  "admission_dgb_slots").getInt<int>());
                result.pushKV("operational_dgb_slots",
                              params[0].find_value(
                                  "operational_dgb_slots").getInt<int>());
                result.pushKV("admission_carrier_slots",
                              params[0].find_value(
                                  "admission_carrier_slots").getInt<int>());
                result.pushKV("operational_carrier_slots",
                              params[0].find_value(
                                  "operational_carrier_slots").getInt<int>());
                result.pushKV("missing_admission_dgb_slots", 1);
                result.pushKV("missing_operational_dgb_slots", 0);
                result.pushKV("missing_admission_carrier_slots", 0);
                result.pushKV("missing_operational_carrier_slots", 0);
                result.pushKV("admission_dgb_satoshis_each", 10000000);
                result.pushKV("operational_dgb_satoshis_each", 20000000);
                result.pushKV("carrier_cents_each", 100);
                result.pushKV("total_output_satoshis", 10000000);
                result.pushKV("total_carrier_cents", 0);
            } else if (command == "rebalancepaymasterpool") {
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "6666666666666666666666666666666666666666666666666666666666666666");
                result.pushKV("retired_admission_dgb_slots", 1);
                result.pushKV("retired_operational_dgb_slots", 0);
                result.pushKV("retired_admission_carrier_slots", 0);
                result.pushKV("retired_operational_carrier_slots", 0);
                result.pushKV("retired_dgb_satoshis", 10000000);
                result.pushKV("retired_carrier_cents", 0);
            } else if (command == "setpaymasterruntimesettings") {
                result.pushKV("operation_mode", "automatic");
                result.pushKV("autostart", true);
            }
            return result;
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    QPushButton* preview = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewPoolPreparation"));
    QPushButton* execute_preparation = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecutePoolPreparation"));
    QPushButton* preview_retirement = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewPoolRetirement"));
    QPushButton* execute_retirement = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecutePoolRetirement"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QCheckBox* autostart = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterProviderAutostart"));
    QPushButton* save_runtime = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterRuntimeSettings"));
    QLabel* runtime_result = tab.findChild<QLabel*>(
        QStringLiteral("paymasterRuntimeSettingsResult"));
    QPushButton* stop = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterStopProvider"));
    QVERIFY(preview != nullptr);
    QVERIFY(execute_preparation != nullptr);
    QVERIFY(preview_retirement != nullptr);
    QVERIFY(execute_retirement != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(autostart != nullptr);
    QVERIFY(save_runtime != nullptr);
    QVERIFY(runtime_result != nullptr);
    QVERIFY(stop != nullptr);

    preview->setEnabled(true);
    preview->click();
    QCOMPARE(commands.value(0), QStringLiteral("preparepaymasterpool"));
    QCOMPARE(parameters.at(0)[0].find_value("execute").get_bool(), false);
    QVERIFY(execute_preparation->isEnabled());
    QVERIFY(!execute_retirement->isEnabled());

    // A preview authorizes only the exact targets that were displayed. Any
    // edit must disable execution until a fresh preview is obtained.
    const int reviewed_admission_dgb = admission_dgb->value();
    admission_dgb->setValue(reviewed_admission_dgb + 1);
    QVERIFY(!execute_preparation->isEnabled());
    admission_dgb->setValue(reviewed_admission_dgb);
    QVERIFY(!execute_preparation->isEnabled());
    preview_retirement->click();
    QCOMPARE(commands.value(1), QStringLiteral("rebalancepaymasterpool"));
    QCOMPARE(parameters.at(1)[0].find_value("execute").get_bool(), false);
    QVERIFY(execute_retirement->isEnabled());
    QVERIFY(!execute_preparation->isEnabled());

    commands.clear();
    parameters.clear();
    autostart->setChecked(true);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterruntimesettings"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("operation_mode").get_str()),
             QStringLiteral("automatic"));
    QCOMPARE(parameters.at(0)[0].find_value("autostart").get_bool(), true);
    QVERIFY(runtime_result->text().contains(QStringLiteral("autostart"),
                                            Qt::CaseInsensitive));
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasterinfo"));

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                UniValue malformed{UniValue::VOBJ};
                malformed.pushKV("operation_mode", "automatic");
                return malformed;
            }
            throw std::runtime_error(
                "unexpected malformed runtime workflow RPC");
        });
    autostart->setChecked(false);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterruntimesettings"));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("not confirmed"), Qt::CaseInsensitive));
    QVERIFY(save_runtime->isEnabled());

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                throw std::runtime_error("injected runtime rejection");
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });
    autostart->setChecked(false);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterruntimesettings"));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("not saved"), Qt::CaseInsensitive));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("injected runtime rejection")));
    QVERIFY(save_runtime->isEnabled());

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("operation_mode", "automatic");
                result.pushKV("autostart", false);
                result.pushKV("running", true);
                return result;
            }
            if (command == "stoppaymaster") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("running", false);
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error(
                    "injected follow-up refresh unavailable");
            }
            throw std::runtime_error("unexpected runtime workflow RPC");
        });
    UniValue running_with_autostart{UniValue::VOBJ};
    running_with_autostart.pushKV("wallet_eligible", true);
    running_with_autostart.pushKV("settings_present", true);
    running_with_autostart.pushKV("enabled", true);
    running_with_autostart.pushKV("running", true);
    running_with_autostart.pushKV("ready", true);
    running_with_autostart.pushKV("wallet_locked", false);
    running_with_autostart.pushKV("has_identity", true);
    running_with_autostart.pushKV("has_policy", true);
    running_with_autostart.pushKV("has_safety_policy", true);
    running_with_autostart.pushKV("operation_mode", "automatic");
    running_with_autostart.pushKV("autostart", true);
    running_with_autostart.pushKV("readiness_errors",
                                  UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running_with_autostart);
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QVERIFY(stop->text().contains(QStringLiteral("disable autostart"),
                                  Qt::CaseInsensitive));
    QVERIFY(stop->isEnabled());
    stop->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterruntimesettings"));
    QCOMPARE(parameters.at(0)[0].find_value("autostart").get_bool(),
             false);
    QCOMPARE(commands.value(1), QStringLiteral("stoppaymaster"));
    QCOMPARE(commands.value(2), QStringLiteral("getpaymasterinfo"));
}

void PaymasterWidgetTests::paymasterManualActivityUsesExpectedRpcAndReadableStates()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            commands.push_back(QString::fromStdString(command));
            if (command == "processpaymasterrequests") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("processed", true);
                result.pushKV("message_type", "quote");
                result.pushKV("queued", true);
                return result;
            }
            if (command == "listpaymasterreservations") {
                UniValue result{UniValue::VARR};
                UniValue reservation{UniValue::VOBJ};
                reservation.pushKV("purpose", "operational");
                reservation.pushKV("asset", "dd_carrier");
                reservation.pushKV("dd_cents", 103);
                reservation.pushKV("state", "reserved");
                result.push_back(std::move(reservation));
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            throw std::runtime_error("unexpected injected activity RPC");
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }

    UniValue running{UniValue::VOBJ};
    running.pushKV("wallet_eligible", true);
    running.pushKV("enabled", true);
    running.pushKV("ready", true);
    running.pushKV("running", true);
    running.pushKV("wallet_locked", false);
    running.pushKV("operation_mode", "manual");
    running.pushKV("service_state", "manual");
    running.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running);

    QPushButton* process_request = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterProcessOneRequest"));
    QPushButton* refresh = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterRefreshReservations"));
    QLabel* action_result = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivityActionResult"));
    QLabel* summary = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivitySummary"));
    QVERIFY(process_request != nullptr);
    QVERIFY(refresh != nullptr);
    QVERIFY(action_result != nullptr);
    QVERIFY(summary != nullptr);
    QVERIFY(process_request->isEnabled());

    process_request->click();
    QCOMPARE(commands.value(0), QStringLiteral("processpaymasterrequests"));
    QVERIFY(action_result->text().contains(QStringLiteral("quote request")));
    QVERIFY(action_result->text().contains(QStringLiteral("queued")));

    commands.clear();
    refresh->click();
    QCOMPARE(commands.value(0), QStringLiteral("listpaymasterreservations"));
    QVERIFY(summary->text().contains(QStringLiteral("1 unavailable")));
    QVERIFY(summary->text().contains(QStringLiteral("1.03 DD carrier")));
    QVERIFY(summary->text().contains(
        QStringLiteral("reserved for in-progress work")));
}

void PaymasterWidgetTests::paymasterFinancesAndBackupWorkflow()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QStringList commands;
    std::vector<UniValue> parameters;

    const auto summary = [](qint64 income, qint64 cost,
                            qint64 transfers) {
        UniValue value{UniValue::VOBJ};
        value.pushKV("service_fee_income_cents", income);
        value.pushKV("dgb_operating_cost_satoshis", cost);
        value.pushKV("successful_transfers", transfers);
        return value;
    };
    const std::string provider_id(64, 'a');
    UniValue finance{UniValue::VOBJ};
    finance.pushKV("provider_id", provider_id);
    finance.pushKV("period", "30d");
    finance.pushKV("service_fee_income_cents", 125);
    finance.pushKV("dgb_operating_cost_satoshis", 100000);
    finance.pushKV("successful_transfers", 5);
    finance.pushKV("average_service_fee_cents", 25);
    finance.pushKV("user_paid_transfers", 3);
    finance.pushKV("public_sponsored_transfers", 1);
    finance.pushKV("restricted_sponsored_transfers", 1);
    const auto model_summary = [](qint64 transfers, qint64 income,
                                  qint64 cost) {
        UniValue value{UniValue::VOBJ};
        value.pushKV("successful_transfers", transfers);
        value.pushKV("service_fee_income_cents", income);
        value.pushKV("dgb_operating_cost_satoshis", cost);
        return value;
    };
    UniValue model_breakdown{UniValue::VOBJ};
    model_breakdown.pushKV("user_paid", model_summary(3, 125, 60000));
    model_breakdown.pushKV("public_sponsored", model_summary(1, 0, 20000));
    model_breakdown.pushKV("restricted_sponsored",
                           model_summary(1, 0, 20000));
    finance.pushKV("model_breakdown", std::move(model_breakdown));
    UniValue summaries{UniValue::VOBJ};
    summaries.pushKV("today", summary(25, 10000, 1));
    summaries.pushKV("7d", summary(75, 50000, 3));
    summaries.pushKV("30d", summary(125, 100000, 5));
    summaries.pushKV("all", summary(250, 200000, 10));
    finance.pushKV("period_summaries", std::move(summaries));
    finance.pushKV("history_partially_reconstructable", false);
    finance.pushKV("history_complete_from", 100);
    finance.pushKV("backup_required", true);
    finance.pushKV("last_successful_backup_at", 0);
    finance.pushKV("external_backup_acknowledged_at", 0);
    finance.pushKV("oracle_price_micro_usd", 500000);
    finance.pushKV("valuation_time", 200);
    finance.pushKV("estimated_result_usd", 1.2495);
    UniValue capital{UniValue::VOBJ};
    capital.pushKV("dgb_available_satoshis", 500000000);
    capital.pushKV("dgb_reserved_satoshis", 100000000);
    capital.pushKV("dgb_pending_satoshis", 50000000);
    capital.pushKV("carrier_base_cents", 400);
    capital.pushKV("carrier_earned_cents", 25);
    capital.pushKV("carrier_withdrawable_cents", 20);
    capital.pushKV("pending_maintenance_transactions", 1);
    finance.pushKV("pool_capital", std::move(capital));
    UniValue daily_totals{UniValue::VARR};
    UniValue day{UniValue::VOBJ};
    day.pushKV("day_start", 86400);
    day.pushKV("service_fee_income_cents", 25);
    day.pushKV("dgb_operating_cost_satoshis", 10000);
    day.pushKV("successful_transfers", 1);
    day.pushKV("maintenance_transactions", 2);
    daily_totals.push_back(std::move(day));
    finance.pushKV("daily_totals", std::move(daily_totals));
    UniValue events{UniValue::VARR};
    UniValue event{UniValue::VOBJ};
    event.pushKV("event_id", std::string(64, 'b'));
    event.pushKV("kind", "transfer");
    event.pushKV("state", "confirmed");
    event.pushKV("dd_income_cents", 25);
    event.pushKV("dgb_cost_satoshis", 10000);
    event.pushKV("created_at", 150);
    event.pushKV("confirmed_at", 160);
    event.pushKV("funding_model", "user_paid");
    event.pushKV("transaction_id", std::string(64, 'c'));
    events.push_back(std::move(event));
    finance.pushKV("events", std::move(events));

    UniValue finance_without_oracle{UniValue::VOBJ};
    UniValue finance_partial_history{UniValue::VOBJ};
    const std::vector<std::string>& finance_keys = finance.getKeys();
    const std::vector<UniValue>& finance_values = finance.getValues();
    for (size_t index = 0; index < finance_keys.size(); ++index) {
        if (finance_keys.at(index) == "oracle_price_micro_usd" ||
            finance_keys.at(index) == "valuation_time" ||
            finance_keys.at(index) == "estimated_result_usd") {
            continue;
        }
        finance_without_oracle.pushKV(finance_keys.at(index),
                                      finance_values.at(index));
        if (finance_keys.at(index) !=
                "history_partially_reconstructable" &&
            finance_keys.at(index) != "history_complete_from") {
            finance_partial_history.pushKV(finance_keys.at(index),
                                           finance_values.at(index));
        }
    }
    finance_partial_history.pushKV(
        "history_partially_reconstructable", true);
    finance_partial_history.pushKV("history_complete_from", 100);
    const auto finance_for_period = [](const UniValue& source,
                                       const std::string& period) {
        UniValue result{UniValue::VOBJ};
        const std::vector<std::string>& keys = source.getKeys();
        const std::vector<UniValue>& values = source.getValues();
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys.at(index) == "period") continue;
            result.pushKV(keys.at(index), values.at(index));
        }
        result.pushKV("period", period);
        return result;
    };
    const auto finance_page_for_period = [](
        const UniValue& source, const std::string& period,
        UniValue page_events, const std::string& next_cursor) {
        UniValue result{UniValue::VOBJ};
        const std::vector<std::string>& keys = source.getKeys();
        const std::vector<UniValue>& values = source.getValues();
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys.at(index) == "period" || keys.at(index) == "events" ||
                keys.at(index) == "next_cursor") {
                continue;
            }
            result.pushKV(keys.at(index), values.at(index));
        }
        result.pushKV("period", period);
        result.pushKV("events", std::move(page_events));
        if (next_cursor.empty()) {
            result.pushKV("next_cursor", UniValue{UniValue::VNULL});
        } else {
            result.pushKV("next_cursor", next_cursor);
        }
        return result;
    };
    constexpr qint64 LARGE_EXPORT_EVENTS{10251};
    bool oracle_available{true};
    bool partial_history{false};
    bool malformed_finance{false};
    bool large_export{false};
    bool switch_period_during_request{false};
    QComboBox* period_control{nullptr};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasterfinancestatus") {
                const std::string requested_period =
                    params[0].find_value("period").get_str();
                if (switch_period_during_request && period_control) {
                    switch_period_during_request = false;
                    period_control->setCurrentIndex(period_control->findData(
                        QStringLiteral("today")));
                }
                if (malformed_finance) {
                    UniValue malformed{UniValue::VOBJ};
                    malformed.pushKV("period", requested_period);
                    return malformed;
                }
                if (large_export) {
                    const UniValue& cursor_value =
                        params[0].find_value("cursor");
                    qulonglong first_index{0};
                    if (!cursor_value.isNull()) {
                        if (!cursor_value.isStr()) {
                            throw std::runtime_error(
                                "finance cursor is not a string");
                        }
                        bool cursor_ok{false};
                        first_index = QString::fromStdString(
                            cursor_value.get_str()).toULongLong(
                                &cursor_ok, 16);
                        if (!cursor_ok || first_index >=
                                static_cast<qulonglong>(
                                    LARGE_EXPORT_EVENTS)) {
                            throw std::runtime_error(
                                "finance cursor is outside the test corpus");
                        }
                    }
                    const qulonglong end_index = std::min(
                        first_index + qulonglong{250},
                        static_cast<qulonglong>(LARGE_EXPORT_EVENTS));
                    UniValue page_events{UniValue::VARR};
                    for (qulonglong index = first_index;
                         index < end_index; ++index) {
                        const std::string event_id =
                            QStringLiteral("%1")
                                .arg(index + 1, 64, 16,
                                     QLatin1Char('0'))
                                .toStdString();
                        UniValue event{UniValue::VOBJ};
                        event.pushKV("event_id", event_id);
                        event.pushKV("kind", "transfer");
                        event.pushKV("state", "confirmed");
                        event.pushKV("dd_income_cents", 1);
                        event.pushKV("dgb_cost_satoshis", 2);
                        event.pushKV("created_at",
                                     static_cast<int64_t>(1000 + index));
                        event.pushKV("confirmed_at",
                                     static_cast<int64_t>(1001 + index));
                        event.pushKV("funding_model", "user_paid");
                        event.pushKV("transaction_id",
                                     std::string(64, 'c'));
                        page_events.push_back(std::move(event));
                    }
                    const std::string next_cursor =
                        end_index < static_cast<qulonglong>(
                                        LARGE_EXPORT_EVENTS)
                        ? QStringLiteral("%1")
                              .arg(end_index, 64, 16,
                                   QLatin1Char('0'))
                              .toStdString()
                        : std::string{};
                    return finance_page_for_period(
                        finance, requested_period,
                        std::move(page_events), next_cursor);
                }
                const UniValue& selected = partial_history
                    ? finance_partial_history
                    : oracle_available ? finance : finance_without_oracle;
                return finance_for_period(selected, requested_period);
            }
            if (command == "acknowledgepaymasterproviderbackup") {
                UniValue malformed{UniValue::VOBJ};
                malformed.pushKV("acknowledged", true);
                return malformed;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error(
                    "injected follow-up provider refresh unavailable");
            }
            throw std::runtime_error("unexpected injected finance RPC");
        });

    QTabWidget* operator_tabs = tab.findChild<QTabWidget*>(
        QStringLiteral("paymasterOperatorTabs"));
    QWidget* finance_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterFinancesPage"));
    QVERIFY(operator_tabs != nullptr);
    QVERIFY(finance_page != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->setTabEnabled(index, true);
    }
    operator_tabs->setCurrentWidget(finance_page);
    QVERIFY(commands.contains(QStringLiteral("getpaymasterfinancestatus")));

    QLabel* period_income = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinancePeriodIncome2"));
    QLabel* result_estimate = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceResultEstimate"));
    QLabel* model_breakdown_label = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceModelBreakdown"));
    QLabel* pool_dgb = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinancePoolDgb"));
    QGroupBox* history_notice = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterFinanceHistoryNotice"));
    QLabel* history_notice_text = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceHistoryNoticeText"));
    QTableWidget* booking_table = tab.findChild<QTableWidget*>(
        QStringLiteral("paymasterFinanceEvents"));
    QTableWidget* daily_table = tab.findChild<QTableWidget*>(
        QStringLiteral("paymasterFinanceDailyTotals"));
    QWidget* details = tab.findChild<QWidget*>(
        QStringLiteral("paymasterFinanceDetailsPanel"));
    QPushButton* details_toggle = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceDetailsToggle"));
    QGroupBox* backup_notice = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterFinanceBackupNotice"));
    QLabel* backup_provider = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceBackupProviderId"));
    QPushButton* backup_now = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceBackupNow"));
    QPushButton* external_backup = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceExternalBackup"));
    QPushButton* finance_refresh = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceRefresh"));
    QPushButton* finance_export = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceExport"));
    QComboBox* period = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterFinancePeriod"));
    QLabel* history_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceHistoryStatus"));
    QVERIFY(period_income != nullptr);
    QVERIFY(result_estimate != nullptr);
    QVERIFY(model_breakdown_label != nullptr);
    QVERIFY(pool_dgb != nullptr);
    QVERIFY(history_notice != nullptr);
    QVERIFY(history_notice_text != nullptr);
    QVERIFY(booking_table != nullptr);
    QVERIFY(daily_table != nullptr);
    QVERIFY(details != nullptr);
    QVERIFY(details_toggle != nullptr);
    QVERIFY(backup_notice != nullptr);
    QVERIFY(backup_provider != nullptr);
    QVERIFY(backup_now != nullptr);
    QVERIFY(external_backup != nullptr);
    QVERIFY(finance_refresh != nullptr);
    QVERIFY(finance_export != nullptr);
    QVERIFY(period != nullptr);
    QVERIFY(history_status != nullptr);
    period_control = period;
    QCOMPARE(QString::fromStdString(
                 parameters.front()[0].find_value("period").get_str()),
             QStringLiteral("30d"));
    QCOMPARE(parameters.front()[0].find_value("include_events").get_bool(),
             true);
    QCOMPARE(parameters.front()[0].find_value("limit").getInt<int>(), 250);
    QVERIFY(period_income->text().contains(QStringLiteral("Service fees")));
    QVERIFY(result_estimate->text().contains(QStringLiteral("USD")));
    QVERIFY(model_breakdown_label->text().contains(
        QStringLiteral("User paid: 3 transfer")));
    QVERIFY(model_breakdown_label->text().contains(
        QStringLiteral("0.00060000 DGB cost")));
    QVERIFY(pool_dgb->text().contains(QStringLiteral("available")));
    QVERIFY(history_notice->isHidden());
    QCOMPARE(booking_table->rowCount(), 1);
    QCOMPARE(booking_table->columnCount(), 6);
    QCOMPARE(daily_table->rowCount(), 1);
    QCOMPARE(daily_table->columnCount(), 5);
    QCOMPARE(daily_table->item(0, 1)->text(), QStringLiteral("0.25"));
    QCOMPARE(daily_table->item(0, 4)->text(), QStringLiteral("2"));
    QVERIFY(details->isHidden());
    details_toggle->click();
    QVERIFY(!details->isHidden());
    QVERIFY(!backup_notice->isHidden());
    QVERIFY(backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(!backup_provider->text().contains(QStringLiteral("…")));
    tab.setPrivacy(true);
    QVERIFY(!backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(backup_provider->toolTip().isEmpty());
    QVERIFY(backup_provider->accessibleName().isEmpty());
    QVERIFY(backup_provider->accessibleDescription().isEmpty());
    QVERIFY(booking_table->isHidden());
    QVERIFY(daily_table->isHidden());
    QVERIFY(!finance_export->isEnabled());
    tab.setPrivacy(false);
    // This isolated fixture has no configured provider, so the normal setup
    // access refresh closes the tab that the test opened manually above.
    // Reopen only that presentation surface before checking cached export.
    operator_tabs->setTabEnabled(
        operator_tabs->indexOf(finance_page), true);
    operator_tabs->setCurrentWidget(finance_page);
    QVERIFY(backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(!booking_table->isHidden());
    QVERIFY(!daily_table->isHidden());
    QTRY_VERIFY(finance_export->isEnabled());
    commands.clear();
    parameters.clear();
    period->setCurrentIndex(period->findData(QStringLiteral("today")));
    QCOMPARE(commands.value(0),
             QStringLiteral("getpaymasterfinancestatus"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("period").get_str()),
             QStringLiteral("today"));
    QSignalSpy backup_requested(
        &tab, &DigiDollarTab::providerWalletBackupRequested);
    backup_now->click();
    QCOMPARE(backup_requested.count(), 1);
    oracle_available = false;
    finance_refresh->click();
    QVERIFY(result_estimate->text().contains(
        QStringLiteral("No current Oracle price")));
    partial_history = true;
    finance_refresh->click();
    QVERIFY(!history_notice->isHidden());
    QVERIFY(history_notice_text->text().contains(
        QStringLiteral("Earlier Paymaster transfers may be absent")));
    QVERIFY(history_notice_text->text().contains(
        QStringLiteral("no earlier income is estimated")));
    // If the selection changes during an asynchronous request, the old
    // period must never be rendered under the new combo-box label. The first
    // result is discarded and a replacement request is issued immediately.
    partial_history = false;
    oracle_available = true;
    period->setCurrentIndex(period->findData(QStringLiteral("30d")));
    commands.clear();
    parameters.clear();
    switch_period_during_request = true;
    finance_refresh->click();
    QCOMPARE(commands.size(), 2);
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("period").get_str()),
             QStringLiteral("30d"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(1)[0].find_value("period").get_str()),
             QStringLiteral("today"));
    QCOMPARE(period->currentData().toString(), QStringLiteral("today"));
    malformed_finance = true;
    finance_refresh->click();
    QVERIFY(history_status->text().contains(
        QStringLiteral("incomplete"), Qt::CaseInsensitive));
    // Retain the last complete authoritative rows instead of replacing them
    // with plausible-looking zero totals from a malformed object.
    QCOMPARE(booking_table->rowCount(), 1);
    QCOMPARE(daily_table->rowCount(), 1);
    malformed_finance = false;
    const bool native_dialogs_available =
        QApplication::platformName() != QLatin1String("minimal") &&
        QApplication::platformName() != QLatin1String("offscreen");
    if (native_dialogs_available) {
        bool confirmation_seen{false};
        QTimer::singleShot(0, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* confirmation = qobject_cast<QMessageBox*>(widget);
                if (!confirmation ||
                    confirmation->windowTitle() !=
                        QLatin1String("Acknowledge external wallet backup")) {
                    continue;
                }
                confirmation_seen = true;
                // Exercise the real confirmation: the static helper returns
                // clickedButton(), not the value supplied to done().
                QAbstractButton* yes = confirmation->button(QMessageBox::Yes);
                QVERIFY(yes != nullptr);
                yes->click();
                break;
            }
        });
        external_backup->click();
        QVERIFY(confirmation_seen);
        QVERIFY(!backup_notice->isHidden());
        QVERIFY(history_status->text().contains(
            QStringLiteral("not confirm"), Qt::CaseInsensitive));
    }

    // Technical event and transaction identifiers are intentionally absent
    // from the normal booking table and remain backend-only detail data.
    for (int column = 0; column < booking_table->columnCount(); ++column) {
        const QTableWidgetItem* item = booking_table->item(0, column);
        QVERIFY(item != nullptr);
        QVERIFY(!item->text().contains(QString(16, QLatin1Char('b'))));
        QVERIFY(!item->text().contains(QString(16, QLatin1Char('c'))));
    }

    QTemporaryDir export_directory;
    QVERIFY(export_directory.isValid());
    const QString export_filename =
        export_directory.filePath(QStringLiteral("paymaster-finance.csv"));
    tab.setPaymasterFinanceExportFilenameForTesting(export_filename);
    large_export = true;
    commands.clear();
    parameters.clear();
    bool event_loop_yielded{false};
    QTimer::singleShot(0, &tab,
                       [&event_loop_yielded] { event_loop_yielded = true; });
    finance_export->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        history_status->text().contains(
            QStringLiteral("10251 booking(s)")),
        30000);
    QVERIFY(event_loop_yielded);
    QCOMPARE(commands.count(QStringLiteral("getpaymasterfinancestatus")),
             42);
    QCOMPARE(commands.size(), static_cast<int>(parameters.size()));
    for (size_t index = 0; index < parameters.size(); ++index) {
        // Provider refreshes may legitimately be queued between export pages.
        // Only the finance calls participate in the cursor contract below.
        if (commands.at(static_cast<int>(index)) !=
            QLatin1String("getpaymasterfinancestatus")) {
            continue;
        }
        const UniValue& page_parameters = parameters.at(index);
        QVERIFY2(page_parameters.isArray() && page_parameters.size() == 1,
                 qPrintable(QStringLiteral(
                     "Finance export RPC %1 has malformed parameters")
                                .arg(index)));
        const UniValue& limit = page_parameters[0].find_value("limit");
        QVERIFY2(limit.isNum(),
                 qPrintable(QStringLiteral(
                     "Finance export RPC %1 omitted its page limit")
                                .arg(index)));
        QCOMPARE(limit.getInt<int>(), 250);
    }
    QFile export_file(export_filename);
    QVERIFY(export_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray exported = export_file.readAll();
    QCOMPARE(exported.count('\n'),
             static_cast<int>(LARGE_EXPORT_EVENTS + 1));
}

void PaymasterWidgetTests::paymasterConfirmationGuardDetectsMaterialChanges()
{
    // A terminal high-level response may omit presentation echoes, but it is
    // still a success once Core returns the validated txid and result status.
    // This is the regression case that previously displayed an authorization
    // error after the exact transaction had already reached the mempool.
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("final_committed")));
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QString{}, QStringLiteral("success"),
        QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("FAILED"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString{}, QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString{}, QString{}, QStringLiteral("success"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('0')), QString{}, QStringLiteral("success"),
        QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QStringLiteral("not-a-transaction-id"), QString{},
        QStringLiteral("success"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QString{},
        QStringLiteral("unexpected"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("PENDING_PROVIDER"),
        QString{}, QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('0')), QStringLiteral("CONFIRMED"),
        QString{}, QStringLiteral("final_committed")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFLICTED"),
        QString{}, QStringLiteral("final_committed")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CANCELED_SAFE"),
        QString{}, QStringLiteral("final_committed")));

    // `final` is a persistence/terminal marker, not a payment-success marker.
    // Exercise every terminal state with both marker values and both txid
    // shapes so a future decoder cannot accidentally reintroduce that trust.
    const QString valid_txid(64, QLatin1Char('7'));
    const QStringList terminal_states{
        QStringLiteral("MEMPOOL"), QStringLiteral("CONFIRMED"),
        QStringLiteral("FAILED"), QStringLiteral("CONFLICTED"),
        QStringLiteral("CANCELED_SAFE")};
    for (const QString& terminal_state : terminal_states) {
        for (const bool reported_final : {false, true}) {
            for (const bool has_txid : {false, true}) {
                const bool expected = has_txid &&
                    (terminal_state == QStringLiteral("MEMPOOL") ||
                     terminal_state == QStringLiteral("CONFIRMED"));
                QCOMPARE(IsValidatedPaymasterCompletion(
                             has_txid ? valid_txid : QString{},
                             terminal_state, QString{},
                             QStringLiteral("final_committed"),
                             reported_final),
                         expected);
            }
        }
    }
    for (const bool reported_final : {false, true}) {
        QCOMPARE(IsValidatedPaymasterCompletion(
                     valid_txid, QString{}, QStringLiteral("success"),
                     QString{}, reported_final),
                 true);
        QCOMPARE(IsValidatedPaymasterCompletion(
                     QString{}, QString{}, QStringLiteral("success"),
                     QString{}, reported_final),
                 false);
    }

    PaymasterConfirmationGuard guard;
    PaymasterConfirmationSelection incomplete;
    QVERIFY(guard.RequiresConfirmation(incomplete));
    QCOMPARE(guard.ChangedFields(incomplete), QStringList{QStringLiteral("incomplete")});
    QVERIFY(!guard.Accept(incomplete));
    QVERIFY(!guard.HasAcceptedSelection());

    PaymasterConfirmationSelection accepted;
    accepted.provider_id = QString(64, QLatin1Char('1'));
    accepted.offer_id = QString(64, QLatin1Char('2'));
    accepted.policy_hash = QString(64, QLatin1Char('3'));
    accepted.funding_model = QStringLiteral("user_paid");
    accepted.recipient = QStringLiteral("dgb1-recipient");
    accepted.authorization_commitment = QString(64, QLatin1Char('4'));
    accepted.payment_cents = 1250;
    accepted.service_fee_cents = 25;
    accepted.user_total_cents = 1275;
    accepted.expires_at = 2000000000;
    QVERIFY(guard.RequiresConfirmation(accepted));
    QCOMPARE(guard.ChangedFields(accepted),
              QStringList({QStringLiteral("provider"), QStringLiteral("offer"),
                           QStringLiteral("policy"), QStringLiteral("funding_model"),
                           QStringLiteral("recipient"), QStringLiteral("amount"),
                           QStringLiteral("service_fee"),
                           QStringLiteral("total"),
                           QStringLiteral("expiry"),
                           QStringLiteral("authorization_commitment")}));
    QVERIFY(guard.Accept(accepted));
    QVERIFY(guard.HasAcceptedSelection());
    QVERIFY(!guard.RequiresConfirmation(accepted));
    QVERIFY(guard.ChangedFields(accepted).isEmpty());

    PaymasterConfirmationSelection changed = accepted;
    changed.provider_id = QString(64, QLatin1Char('5'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("provider")});
    changed = accepted;
    changed.offer_id = QString(64, QLatin1Char('6'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("offer")});
    changed = accepted;
    changed.policy_hash = QString(64, QLatin1Char('7'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("policy")});
    changed = accepted;
    changed.funding_model = QStringLiteral("sponsored");
    changed.service_fee_cents = 0;
    changed.user_total_cents = changed.payment_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("funding_model"),
                          QStringLiteral("service_fee"),
                          QStringLiteral("total")}));
    changed = accepted;
    changed.recipient = QStringLiteral("dgb1-other");
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("recipient")});
    changed = accepted;
    ++changed.payment_cents;
    ++changed.user_total_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("amount"), QStringLiteral("total")}));
    changed = accepted;
    ++changed.service_fee_cents;
    ++changed.user_total_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("service_fee"), QStringLiteral("total")}));
    changed = accepted;
    ++changed.user_total_cents;
    QVERIFY(!changed.IsComplete());
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("incomplete")});
    changed = accepted;
    changed.funding_model = QStringLiteral("sponsored");
    QVERIFY(!changed.IsComplete());
    changed = accepted;
    ++changed.expires_at;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("expiry")});
    changed = accepted;
    changed.authorization_commitment = QString(64, QLatin1Char('8'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList{QStringLiteral("authorization_commitment")});

    guard.Reset();
    QVERIFY(!guard.HasAcceptedSelection());
    QVERIFY(guard.RequiresConfirmation(accepted));
}

void PaymasterWidgetTests::paymasterRecoveryConfirmationGuardDetectsMaterialChanges()
{
    PaymasterRecoveryConfirmationGuard guard;
    PaymasterRecoveryConfirmationSelection incomplete;
    QVERIFY(guard.RequiresConfirmation(incomplete));
    QCOMPARE(guard.ChangedFields(incomplete),
             QStringList{QStringLiteral("incomplete")});
    QVERIFY(!guard.Accept(incomplete));
    QVERIFY(!guard.HasAcceptedSelection());

    PaymasterRecoveryConfirmationSelection accepted;
    accepted.recovery_provider_id = QString(64, QLatin1Char('1'));
    accepted.privacy_profile = QStringLiteral("high");
    accepted.offer_id = QString(64, QLatin1Char('2'));
    accepted.policy_hash = QString(64, QLatin1Char('3'));
    accepted.original_commit_key = QString(64, QLatin1Char('4'));
    accepted.original_template_commitment =
        QString(64, QLatin1Char('5'));
    accepted.wallet_returns = QStringList{
        QStringLiteral("0:wallet-script-a:1000"),
        QStringLiteral("1:wallet-script-b:250")};
    accepted.authorization_commitment = QString(64, QLatin1Char('6'));
    accepted.maximum_service_fee_cents = 25;
    accepted.service_fee_cents = 20;
    accepted.network_fee_satoshis = 1500;
    accepted.expires_at = 2000000000;

    QVERIFY(guard.RequiresConfirmation(accepted));
    QCOMPARE(guard.ChangedFields(accepted),
             QStringList({QStringLiteral("recovery_provider"),
                          QStringLiteral("privacy"), QStringLiteral("offer"),
                          QStringLiteral("policy"),
                          QStringLiteral("original_commit"),
                          QStringLiteral("original_template_commitment"),
                          QStringLiteral("wallet_returns"),
                          QStringLiteral("maximum_service_fee"),
                          QStringLiteral("service_fee"),
                          QStringLiteral("network_fee"),
                          QStringLiteral("expiry"),
                          QStringLiteral("authorization_commitment")}));
    QVERIFY(guard.Accept(accepted));
    QVERIFY(guard.HasAcceptedSelection());
    QVERIFY(!guard.RequiresConfirmation(accepted));
    QVERIFY(guard.ChangedFields(accepted).isEmpty());

    const auto expect_change = [&guard, &accepted](
                                   PaymasterRecoveryConfirmationSelection changed,
                                   const QString& field) {
        QVERIFY(guard.RequiresConfirmation(changed));
        QCOMPARE(guard.ChangedFields(changed), QStringList{field});
        QVERIFY(!changed.IsComplete() || guard.HasAcceptedSelection());
        QVERIFY(!guard.RequiresConfirmation(accepted));
    };

    PaymasterRecoveryConfirmationSelection changed = accepted;
    changed.recovery_provider_id = QString(64, QLatin1Char('7'));
    expect_change(changed, QStringLiteral("recovery_provider"));
    changed = accepted;
    changed.privacy_profile = QStringLiteral("standard");
    expect_change(changed, QStringLiteral("privacy"));
    changed = accepted;
    changed.offer_id = QString(64, QLatin1Char('8'));
    expect_change(changed, QStringLiteral("offer"));
    changed = accepted;
    changed.policy_hash = QString(64, QLatin1Char('9'));
    expect_change(changed, QStringLiteral("policy"));
    changed = accepted;
    changed.original_commit_key = QString(64, QLatin1Char('a'));
    expect_change(changed, QStringLiteral("original_commit"));
    changed = accepted;
    changed.original_template_commitment = QString(64, QLatin1Char('b'));
    expect_change(changed, QStringLiteral("original_template_commitment"));
    changed = accepted;
    changed.wallet_returns[0] = QStringLiteral("0:other-wallet-script:1000");
    expect_change(changed, QStringLiteral("wallet_returns"));
    changed = accepted;
    ++changed.maximum_service_fee_cents;
    expect_change(changed, QStringLiteral("maximum_service_fee"));
    changed = accepted;
    ++changed.service_fee_cents;
    expect_change(changed, QStringLiteral("service_fee"));
    changed = accepted;
    ++changed.network_fee_satoshis;
    expect_change(changed, QStringLiteral("network_fee"));
    changed = accepted;
    ++changed.expires_at;
    expect_change(changed, QStringLiteral("expiry"));
    changed = accepted;
    changed.authorization_commitment = QString(64, QLatin1Char('c'));
    expect_change(changed, QStringLiteral("authorization_commitment"));

    guard.Reset();
    QVERIFY(!guard.HasAcceptedSelection());
    QVERIFY(guard.RequiresConfirmation(accepted));
}

void PaymasterWidgetTests::paymasterClientOfferPreviewUsesExactRpcAndPlainText()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-offers");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    std::vector<UniValue> parameters;
    int offer_calls{0};
    bool waiting_seen{false};
    bool refresh_disabled_while_busy{false};
    bool stale_rows_cleared_before_rpc{false};
    bool duplicate_refresh_blocked{false};
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasteroffers") {
                ++offer_calls;
                QLabel* status = send_widget.findChild<QLabel*>(
                    QStringLiteral("paymasterOffersStatus"));
                QPushButton* refresh = send_widget.findChild<QPushButton*>(
                    QStringLiteral("refreshPaymasterOffers"));
                QTableWidget* table = send_widget.findChild<QTableWidget*>(
                    QStringLiteral("paymasterOffers"));
                waiting_seen = status &&
                    status->property("statusKind").toString() == QStringLiteral("waiting");
                refresh_disabled_while_busy = refresh && !refresh->isEnabled();
                stale_rows_cleared_before_rpc = table && table->rowCount() == 0;
                const int calls_before_reentry = offer_calls;
                QMetaObject::invokeMethod(
                    send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection);
                duplicate_refresh_blocked = offer_calls == calls_before_reentry;

                UniValue offers{UniValue::VARR};
                offers.push_back(PaymasterOffer(
                    "<b>Alice & Co</b>",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "user_paid",
                    2, 325, true, 2000000000));
                offers.push_back(PaymasterOffer(
                    "Sponsor",
                    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                    "sponsored",
                    0, 325, false, 2000000100));
                return offers;
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    commands.clear();
    parameters.clear();

    QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingPaymaster"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QVERIFY(paymaster != nullptr);
    QVERIFY(amount != nullptr);
    QVERIFY(table != nullptr);
    QVERIFY(status != nullptr);

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    table->setRowCount(1); // Must disappear before the next RPC is entered.
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));

    QCOMPARE(offer_calls, 1);
    QVERIFY(waiting_seen);
    QVERIFY(refresh_disabled_while_busy);
    QVERIFY(stale_rows_cleared_before_rpc);
    QVERIFY(duplicate_refresh_blocked);
    QCOMPARE(commands, QStringList{QStringLiteral("getpaymasteroffers")});
    QCOMPARE(parameters.size(), size_t{1});
    QVERIFY(parameters.front().isArray());
    QCOMPARE(parameters.front().size(), size_t{2});
    QCOMPARE(parameters.front()[0].getInt<int64_t>(), int64_t{325});
    const UniValue& preview_options = parameters.front()[1];
    QVERIFY(preview_options.isObject());
    QCOMPARE(preview_options.find_value(
                 "subtract_paymaster_fee_from_amount").get_bool(), false);

    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("<b>Alice & Co</b>"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Service fee in $DD"));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("0.02 $DD (2 cents)"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("3.27 $DD (327 cents)"));
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("98.75%"));
    QVERIFY(table->item(0, 5)->text().contains(QStringLiteral("2033")));
    QVERIFY(table->item(0, 0)->toolTip().contains(QString(64, QLatin1Char('b'))));
    QCOMPARE(table->item(1, 1)->text(), QStringLiteral("Sponsored by provider"));
    QCOMPARE(table->item(1, 4)->text(),
             QStringLiteral("New provider — not enough history yet"));
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("ready"));
    QVERIFY(status->text().contains(QStringLiteral("2 eligible offer(s)")));
    QCOMPARE(status->accessibleDescription(), status->text());
}

void PaymasterWidgetTests::paymasterClientOfferPreviewInvalidatesAndHandlesFailures()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-offer-states");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    int offer_response{0};
    QLineEdit* in_flight_amount{nullptr};
    bool switch_wallet_during_request{false};
    QString dialog_title;
    QString dialog_message;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& title, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            dialog_title = title;
            dialog_message = message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command != "getpaymasteroffers") {
                throw std::runtime_error("unexpected Paymaster RPC");
            }
            if (offer_response == 2) {
                throw std::runtime_error("preview transport failed");
            }
            if (offer_response == 3) {
                UniValue malformed{UniValue::VARR};
                UniValue offer{UniValue::VOBJ};
                offer.pushKV("display_name", "Malformed provider");
                offer.pushKV("funding_model", "user_paid");
                offer.pushKV("service_fee_cents", 2);
                // Missing payment, total, reputation, expiry and bindings.
                malformed.push_back(std::move(offer));
                return malformed;
            }
            if (offer_response == 4 && in_flight_amount) {
                // Simulate a user edit while the production asynchronous RPC
                // is in flight. The returned offer is bound to the old amount
                // and must therefore remain invisible.
                in_flight_amount->setText(QStringLiteral("3.26"));
            }
            if (offer_response == 5 && switch_wallet_during_request) {
                // A late reply owned by the previously selected wallet must
                // not repopulate the reset state after a wallet switch.
                switch_wallet_during_request = false;
                send_widget.setWalletModel(nullptr);
                send_widget.setWalletModel(mini_gui.walletModel.get());
            }
            UniValue offers{UniValue::VARR};
            if (offer_response == 0 || offer_response == 4 ||
                offer_response == 5) {
                offers.push_back(PaymasterOffer(
                    "Provider",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "user_paid",
                    2, 325, true, 2000000000));
            }
            return offers;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());

    QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingPaymaster"));
    QRadioButton* direct_dgb = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingDgb"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QCheckBox* subtract = send_widget.findChild<QCheckBox*>(
        QStringLiteral("subtractPaymasterFeeFromAmount"));
    QSpinBox* fee_cap = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterFeeCap"));
    QSpinBox* attempts = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterMaximumAttempts"));
    QComboBox* privacy = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterPrivacy"));
    QComboBox* selection = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterSelection"));
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QPushButton* refresh = send_widget.findChild<QPushButton*>(
        QStringLiteral("refreshPaymasterOffers"));
    QVERIFY(paymaster && direct_dgb && amount && subtract && fee_cap && attempts);
    QVERIFY(privacy && selection && table && status && refresh);
    in_flight_amount = amount;

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 1);

    const auto seed_stale_row = [&] {
        table->setRowCount(1);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("stale")));
    };
    amount->setText(QStringLiteral("3.26"));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("info"));
    seed_stale_row();
    subtract->setChecked(true);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    fee_cap->setValue(fee_cap->value() - 1);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    privacy->setCurrentIndex(privacy->findData(QStringLiteral("high")));
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    selection->setCurrentIndex(
        selection->findData(QStringLiteral("privacy_weighted")));
    QCOMPARE(table->rowCount(), 0);
    privacy->setCurrentIndex(privacy->findData(QStringLiteral("standard")));
    seed_stale_row();
    attempts->setValue(2);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    direct_dgb->setChecked(true);
    QCOMPARE(table->rowCount(), 0);

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    subtract->setChecked(false);
    offer_response = 1;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("waiting"));
    QVERIFY(status->text().contains(QStringLiteral("No eligible public offer")));

    offer_response = 2;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("error"));
    QVERIFY(refresh->isEnabled());
    QCOMPARE(dialog_title, QStringLiteral("Paymaster offers unavailable"));
    QCOMPARE(dialog_message, QStringLiteral("preview transport failed"));

    offer_response = 3;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("error"));
    QVERIFY(status->text().contains(QStringLiteral("malformed")));
    QCOMPARE(dialog_title, QStringLiteral("Paymaster offers unavailable"));
    QVERIFY(dialog_message.contains(QStringLiteral("invalid Paymaster offer preview")));

    offer_response = 4;
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(amount->text(), QStringLiteral("3.26"));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(),
             QStringLiteral("info"));
    QVERIFY(status->text().contains(QStringLiteral("inputs changed"),
                                    Qt::CaseInsensitive));
    QVERIFY(refresh->isEnabled());

    offer_response = 5;
    switch_wallet_during_request = true;
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(refresh->isEnabled());

    offer_response = 0;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 1);

    seed_stale_row();
    send_widget.setWalletModel(nullptr);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("info"));
    QVERIFY(!refresh->isEnabled());

    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [](const std::string& command, const UniValue&) {
            if (command != "getpaymasterclientsafetystatus") {
                throw std::runtime_error("unexpected client-safety RPC");
            }
            UniValue malformed{UniValue::VOBJ};
            malformed.pushKV("configured", true);
            malformed.pushKV("policy", UniValue{UniValue::VOBJ});
            return malformed;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    QLabel* client_safety = send_widget.findChild<QLabel*>(
        QStringLiteral("sendPaymasterClientSafetyStatus"));
    QVERIFY(client_safety != nullptr);
    QVERIFY(client_safety->toolTip().contains(
        QStringLiteral("incomplete Paymaster client-safety status")));
}

void PaymasterWidgetTests::paymasterClientAuthorizationIsTwoStageAndFailClosed()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-authorization");
    const SecureString passphrase{"qt-paymaster-client-authorization"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    bool unlock_succeeded{false};
    QObject::connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock,
                     [&] {
                         unlock_succeeded =
                             mini_gui.walletModel->setWalletLocked(
                                 false, passphrase);
                     });

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QMessageBox::StandardButton authorization_answer{QMessageBox::Cancel};
    QStringList dialog_titles;
    std::vector<bool> wallet_locked_during_dialog;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& title, const QString&,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            dialog_titles.push_back(title);
            wallet_locked_during_dialog.push_back(wallet->IsLocked());
            return title == QStringLiteral("Confirm exact Paymaster authorization")
                ? authorization_answer
                : QMessageBox::Ok;
        });

    std::vector<UniValue> send_parameters;
    int send_calls{0};
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "resolvepaymastersession") {
                return PaymasterSessionView(
                    "AWAITING_USER_SIGNATURE", "none", "QUOTED");
            }
            if (command != "senddigidollar") {
                throw std::runtime_error("unexpected Paymaster RPC");
            }
            ++send_calls;
            send_parameters.push_back(params);
            if (send_calls <= 2) {
                return PaymasterAuthorizationResult(
                    true, std::string(64, 'c'),
                    "AWAITING_USER_SIGNATURE", "none");
            }
            return PaymasterAuthorizationResult(
                false, std::string(64, 'f'),
                "PENDING_PROVIDER", "user_psbt");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("none"),
        true, QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("QUOTED"), QString{},
        {QStringLiteral("refresh"), QStringLiteral("resume")}, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QLabel* state = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionState"));
    QVERIFY(primary != nullptr);
    QVERIFY(state != nullptr);
    QCOMPARE(primary->text(), QStringLiteral("Review exact offer"));

    primary->click();
    QCOMPARE(send_calls, 1);
    QVERIFY(state->text().contains(QStringLiteral("paused by user")));
    QCOMPARE(send_parameters.at(0)[5].get_str(), std::string{"cents"});
    const UniValue& canceled_options = send_parameters.at(0)[6];
    QCOMPARE(canceled_options.find_value("prepare_only").get_bool(), true);
    QVERIFY(canceled_options.find_value("authorization_commitment").isNull());

    authorization_answer = QMessageBox::Yes;
    // The prepare-only response is not an authoritative action envelope.
    // Re-read the durable session before offering another signing attempt.
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    primary->click();
    QCOMPARE(send_calls, 1);
    QCOMPARE(primary->text(), QStringLiteral("Review exact offer"));
    primary->click();
    QCOMPARE(send_calls, 3);
    const UniValue& reviewed_options = send_parameters.at(1)[6];
    const UniValue& authorized_options = send_parameters.at(2)[6];
    QCOMPARE(reviewed_options.find_value("prepare_only").get_bool(), true);
    QVERIFY(reviewed_options.find_value("authorization_commitment").isNull());
    QVERIFY(authorized_options.find_value("prepare_only").isNull());
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("authorization_commitment").get_str()),
             QString(64, QLatin1Char('c')));
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("selection").get_str()),
             QStringLiteral("lowest_total_cost"));
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("privacy").get_str()),
             QStringLiteral("standard"));
    QVERIFY(state->text().contains(
        QStringLiteral("authorization blocked: commitment changed or missing")));
    QCOMPARE(dialog_titles.count(
                 QStringLiteral("Confirm exact Paymaster authorization")), 2);
    QVERIFY(dialog_titles.contains(QStringLiteral("Paymaster authorization blocked")));
    QVERIFY(unlock_succeeded);
    QVERIFY(!wallet_locked_during_dialog.empty());
    QVERIFY(std::all_of(wallet_locked_during_dialog.begin(),
                        wallet_locked_during_dialog.end(),
                        [](bool locked) { return locked; }));
    QVERIFY(wallet->IsLocked());
}

void PaymasterWidgetTests::paymasterClientSessionActionMatrix_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<QString>("artifact");
    QTest::addColumn<QString>("attempt_state");
    QTest::addColumn<QString>("pending_phase");
    QTest::addColumn<QStringList>("allowed_actions");
    QTest::addColumn<QString>("primary_text");
    QTest::addColumn<bool>("retry_visible");
    QTest::addColumn<bool>("fallback_visible");
    QTest::addColumn<bool>("recover_visible");
    QTest::addColumn<bool>("abandon_visible");
    QTest::addColumn<bool>("more_visible");

    QTest::newRow("created-unsigned")
        << QStringLiteral("CREATED") << QStringLiteral("none") << QString{}
        << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Resume preparation")
        << false << false << false << true << true;
    QTest::newRow("created-core-allows-refresh-only")
        << QStringLiteral("CREATED") << QStringLiteral("none") << QString{}
        << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh")}
        << QStringLiteral("Check current status")
        << false << false << false << false << false;
    QTest::newRow("awaiting-unlock-unsigned")
        << QStringLiteral("AWAITING_WALLET_UNLOCK") << QStringLiteral("none")
        << QStringLiteral("CANDIDATE") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("fallback"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Unlock and continue")
        << false << true << false << true << true;
    QTest::newRow("exact-offer-review")
        << QStringLiteral("AWAITING_USER_SIGNATURE") << QStringLiteral("none")
        << QStringLiteral("QUOTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("fallback"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Review exact offer")
        << false << true << false << true << true;
    QTest::newRow("authorized-signed")
        << QStringLiteral("AUTHORIZED") << QStringLiteral("user_psbt")
        << QStringLiteral("QUOTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("provider-pending-signed")
        << QStringLiteral("PENDING_PROVIDER") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("mempool-final")
        << QStringLiteral("MEMPOOL") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("failed-unsigned")
        << QStringLiteral("FAILED") << QStringLiteral("none")
        << QStringLiteral("REJECTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Check current status")
        << false << false << false << true << true;
    QTest::newRow("failed-signed")
        << QStringLiteral("FAILED") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Start safe recovery")
        << true << false << false << false << true;
    QTest::newRow("failed-signed-core-denies-recovery")
        << QStringLiteral("FAILED") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same")}
        << QStringLiteral("Check current status")
        << true << false << false << false << true;
    QTest::newRow("conflicted-final")
        << QStringLiteral("CONFLICTED") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Start safe recovery")
        << true << false << false << false << true;
    QTest::newRow("contradictory-unsigned-provider-pending")
        << QStringLiteral("PENDING_PROVIDER") << QStringLiteral("none")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh")}
        << QStringLiteral("Check current status")
        << false << false << false << false << false;
    QTest::newRow("confirmed-terminal")
        << QStringLiteral("CONFIRMED") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{}
        << QStringLiteral("Start a new transfer")
        << false << false << false << false << false;
    QTest::newRow("canceled-terminal")
        << QStringLiteral("CANCELED_SAFE") << QStringLiteral("none")
        << QStringLiteral("REJECTED") << QStringLiteral("NONE")
        << QStringList{}
        << QStringLiteral("Start a new transfer")
        << false << false << false << false << false;
}

void PaymasterWidgetTests::paymasterClientSessionActionMatrix()
{
    QFETCH(QString, state);
    QFETCH(QString, artifact);
    QFETCH(QString, attempt_state);
    QFETCH(QString, pending_phase);
    QFETCH(QStringList, allowed_actions);
    QFETCH(QString, primary_text);
    QFETCH(bool, retry_visible);
    QFETCH(bool, fallback_visible);
    QFETCH(bool, recover_visible);
    QFETCH(bool, abandon_visible);
    QFETCH(bool, more_visible);

    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarSendWidget send_widget(platform_style.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        state, artifact, true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, attempt_state, pending_phase, allowed_actions, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* retry = send_widget.findChild<QPushButton*>(
        QStringLiteral("retryPaymasterSession"));
    QPushButton* fallback = send_widget.findChild<QPushButton*>(
        QStringLiteral("fallbackPaymasterSession"));
    QPushButton* recover = send_widget.findChild<QPushButton*>(
        QStringLiteral("recoverPaymasterSession"));
    QPushButton* abandon = send_widget.findChild<QPushButton*>(
        QStringLiteral("abandonUnsignedPaymasterSession"));
    QPushButton* more = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionMoreOptions"));
    QLineEdit* address = send_widget.findChild<QLineEdit*>(
        QStringLiteral("addressEdit"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QVERIFY(primary && retry && fallback && recover && abandon && more);
    QVERIFY(address && amount);

    QCOMPARE(primary->text(), primary_text);
    QCOMPARE(!retry->isHidden(), retry_visible);
    QCOMPARE(!fallback->isHidden(), fallback_visible);
    QCOMPARE(!recover->isHidden(), recover_visible);
    QCOMPARE(!abandon->isHidden(), abandon_visible);
    QCOMPARE(!more->isHidden(), more_visible);
    QVERIFY(address->isReadOnly());
    QVERIFY(amount->isReadOnly());
}

void PaymasterWidgetTests::paymasterClientSessionRpcActionsAreBound()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-session-actions");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    std::vector<UniValue> send_parameters;
    QString action_warning;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString&, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            action_warning = message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "INPUTS_RESERVED", "none", "CANDIDATE");
            }
            if (command == "senddigidollar") {
                send_parameters.push_back(params);
                return PaymasterSessionView(
                    "INPUTS_RESERVED", "none", "CANDIDATE");
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    commands.clear();
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"));

    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "retryPaymasterSession", Qt::DirectConnection));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QVERIFY(primary != nullptr);
    QCOMPARE(primary->text(), QStringLiteral("Resume preparation"));
    primary->click();

    QCOMPARE(resolve_actions,
             QStringList({QStringLiteral("refresh"),
                          QStringLiteral("refresh"),
                          QStringLiteral("refresh")}));
    QVERIFY(action_warning.contains(
        QStringLiteral("no longer allows"), Qt::CaseInsensitive));
    QCOMPARE(commands,
             QStringList({QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("senddigidollar")}));
    QCOMPARE(send_parameters.size(), size_t{1});
    const UniValue& options = send_parameters.front()[6];
    QCOMPARE(options.find_value("prepare_only").get_bool(), true);
    QVERIFY(options.find_value("authorization_commitment").isNull());
    QCOMPARE(QString::fromStdString(options.find_value("request_id").get_str()),
             QStringLiteral("00000000-0000-4000-8000-000000000001"));
}

void PaymasterWidgetTests::paymasterClientRestartCreatedSessionWithoutRecipientIsReadOnly()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-recipientless-restart");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 1);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                UniValue sessions{UniValue::VARR};
                UniValue session{UniValue::VOBJ};
                session.pushKV(
                    "request_id",
                    "00000000-0000-4000-8000-000000000001");
                session.pushKV("session_id", std::string(64, '1'));
                session.pushKV("session_state", "CREATED");
                sessions.push_back(std::move(session));
                listed.pushKV("sessions", std::move(sessions));
                return listed;
            }
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "CREATED", "none", "", /*include_recipient=*/false);
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });

    send_widget.setWalletModel(mini_gui.walletModel.get());

    QLabel* transfer = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionTransfer"));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* abandon = send_widget.findChild<QPushButton*>(
        QStringLiteral("abandonUnsignedPaymasterSession"));
    QVERIFY(transfer && primary && abandon);
    QVERIFY(transfer->text().contains(
        QStringLiteral("not yet available"), Qt::CaseInsensitive));
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    QVERIFY(primary->isEnabled());
    QVERIFY(!abandon->isHidden());
    QVERIFY(!commands.contains(QStringLiteral("senddigidollar")));

    primary->click();
    QCOMPARE(resolve_actions,
             QStringList({QStringLiteral("refresh"),
                          QStringLiteral("refresh")}));
    QVERIFY(!commands.contains(QStringLiteral("senddigidollar")));
}

void PaymasterWidgetTests::paymasterClientMultipleRestartSessionsRequireSelection()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-multiple-restart");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 2);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                UniValue sessions{UniValue::VARR};
                for (int index = 1; index <= 2; ++index) {
                    UniValue session{UniValue::VOBJ};
                    session.pushKV(
                        "request_id",
                        index == 1
                            ? "00000000-0000-4000-8000-000000000001"
                            : "00000000-0000-4000-8000-000000000002");
                    session.pushKV("session_id",
                                   std::string(64, index == 1 ? '1' : '2'));
                    session.pushKV("session_state",
                                   index == 1 ? "CREATED" : "INPUTS_RESERVED");
                    session.pushKV("requested_amount_cents", 300 + index);
                    sessions.push_back(std::move(session));
                }
                listed.pushKV("sessions", std::move(sessions));
                return listed;
            }
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "CREATED", "none", "", /*include_recipient=*/false);
            }
            throw std::runtime_error(
                "persisted-session selection must not sign or recover");
        });

    send_widget.setWalletModel(mini_gui.walletModel.get());

    QFrame* chooser = send_widget.findChild<QFrame*>(
        QStringLiteral("persistedPaymasterSessionsFrame"));
    QComboBox* sessions = send_widget.findChild<QComboBox*>(
        QStringLiteral("persistedPaymasterSessions"));
    QPushButton* review = send_widget.findChild<QPushButton*>(
        QStringLiteral("loadPersistedPaymasterSession"));
    QVERIFY(chooser && sessions && review);
    QVERIFY(!chooser->isHidden());
    QCOMPARE(sessions->count(), 2);
    QCOMPARE(commands, QStringList{QStringLiteral(
                           "listdigidollarsendsessions")});
    QVERIFY(resolve_actions.isEmpty());

    // Selecting a persisted item performs exactly one read-only refresh. It
    // never resumes preparation, unlocks, signs, broadcasts or recovers.
    review->click();
    QCOMPARE(resolve_actions, QStringList{QStringLiteral("refresh")});
    QCOMPARE(commands,
             QStringList({QStringLiteral("listdigidollarsendsessions"),
                          QStringLiteral("resolvepaymastersession")}));
}

void PaymasterWidgetTests::paymasterClientRecoveryExpiryIsFailClosed()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-recovery-expiry");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    bool inconsistent_expiry{false};
    QString warning;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString&, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            warning = message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 0);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                listed.pushKV("sessions", UniValue{UniValue::VARR});
                return listed;
            }
            if (command == "resolvepaymastersession") {
                return PaymasterSessionView(
                    "FAILED", "alternative_recovery", "CONFLICTED",
                    /*include_recipient=*/true,
                    /*recovery_expires_at=*/1,
                    /*recovery_expired=*/!inconsistent_expiry);
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("FAILED"), QStringLiteral("alternative_recovery"),
        true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CONFLICTED"), QStringLiteral("NONE"),
        {QStringLiteral("refresh"), QStringLiteral("recover"),
         QStringLiteral("cancel_to_self")}, true);

    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* recover = send_widget.findChild<QPushButton*>(
        QStringLiteral("recoverPaymasterSession"));
    QVERIFY(primary && recover);
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    QVERIFY(recover->isHidden());

    inconsistent_expiry = true;
    warning.clear();
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(warning.contains(
        QStringLiteral("inconsistent recovery expiry"),
        Qt::CaseInsensitive));
}

void PaymasterWidgetTests::paymasterClientDelayedCallbackIgnoresWalletClose()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-delayed-callback");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    const auto initial_rpc = [](const std::string& command,
                                const UniValue&) {
        if (command == "getpaymasterclientsafetystatus") {
            return PaymasterClientSafetyStatus();
        }
        if (command == "listdigidollarsendsessions") {
            UniValue listed{UniValue::VOBJ};
            listed.pushKV("active_only", true);
            listed.pushKV("count", 0);
            listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
            listed.pushKV("sessions", UniValue{UniValue::VARR});
            return listed;
        }
        throw std::runtime_error("unexpected Paymaster RPC");
    };

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(initial_rpc);
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"), QStringLiteral("NONE"),
        {QStringLiteral("refresh")}, true);
    WalletModel::RpcCallback delayed_callback;
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterAsyncRpcExecutorForTesting(
        [&](const std::string&, const UniValue&,
            WalletModel::RpcCallback callback) {
            delayed_callback = std::move(callback);
        });
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(static_cast<bool>(delayed_callback));

    QLabel* state = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionState"));
    QVERIFY(state != nullptr);
    send_widget.setWalletModel(nullptr);
    QCOMPARE(state->text(), QStringLiteral("No active session"));
    delayed_callback(
        PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE"),
        QString{});
    QCOMPARE(state->text(), QStringLiteral("No active session"));

    auto* destroyed_widget = new DigiDollarSendWidget(
        mini_gui.platformStyle.get());
    destroyed_widget->findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(initial_rpc);
    destroyed_widget->setWalletModel(mini_gui.walletModel.get());
    destroyed_widget->findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"), QStringLiteral("NONE"),
        {QStringLiteral("refresh")}, true);
    WalletModel::RpcCallback destroyed_callback;
    destroyed_widget->findChild<PaymasterSendWidget*>()->setPaymasterAsyncRpcExecutorForTesting(
        [&](const std::string&, const UniValue&,
            WalletModel::RpcCallback callback) {
            destroyed_callback = std::move(callback);
        });
    QVERIFY(QMetaObject::invokeMethod(
        destroyed_widget->findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState",
        Qt::DirectConnection));
    QVERIFY(static_cast<bool>(destroyed_callback));
    QPointer<DigiDollarSendWidget> destroyed_guard{destroyed_widget};
    delete destroyed_widget;
    QVERIFY(destroyed_guard.isNull());
    destroyed_callback(
        PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE"),
        QString{});
}

void PaymasterWidgetTests::paymasterClientUnlockLeaseSurvivesWalletModelClose()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-unlock-model-close");
    const SecureString passphrase{"qt-paymaster-unlock-model-close"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    bool unlock_succeeded{false};
    QObject::connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock,
                     [&] {
                         unlock_succeeded =
                             mini_gui.walletModel->setWalletLocked(
                                 false, passphrase);
                     });

    std::shared_ptr<WalletModel::UnlockContext> unlock =
        mini_gui.walletModel->requestUnlockForAsync();
    QVERIFY(unlock_succeeded);
    QVERIFY(unlock->isValid());
    QVERIFY(!wallet->IsLocked());

    // The asynchronous lease owns the wallet interface directly. Destroying
    // the GUI model first must neither dereference it later nor leave the
    // encrypted wallet unlocked when the callback finally releases its lease.
    mini_gui.walletModel.reset();
    unlock.reset();
    QVERIFY(wallet->IsLocked());
}

void PaymasterWidgetTests::paymasterClientDatabaseReadErrorIsActionable()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-database-error");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QString title;
    QString message;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& dialog_title,
            const QString& dialog_message, QMessageBox::StandardButtons,
            QMessageBox::StandardButton) {
            title = dialog_title;
            message = dialog_message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "resolvepaymastersession") {
                return PaymasterSessionView(
                    "AWAITING_USER_SIGNATURE", "none", "QUOTED");
            }
            if (command == "senddigidollar") {
                throw std::runtime_error("PAYMASTER_SESSION_DATABASE_READ");
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("none"),
        true, QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("QUOTED"), QString{},
        {QStringLiteral("refresh"), QStringLiteral("resume")}, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QVERIFY(primary != nullptr);
    primary->click();

    QCOMPARE(title, QStringLiteral("Paymaster wallet data unavailable"));
    QVERIFY(message.contains(QStringLiteral("authoritative persisted session state")));
    QVERIFY(message.contains(QStringLiteral("existing reservations or prior authorizations")));
    QVERIFY(message.contains(QStringLiteral("Preserve the wallet backup and debug log")));
    QVERIFY(message.contains(QStringLiteral("PAYMASTER_SESSION_DATABASE_READ")));
    QVERIFY(!message.contains(QStringLiteral("Wallet synchronization status")));
    QVERIFY(!message.contains(QStringLiteral("Network connection")));
    QVERIFY(!message.contains(QStringLiteral("Available DigiDollar balance")));
}

void PaymasterWidgetTests::paymasterClientControlsAreAccessible()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarSendWidget send_widget(platform_style.get());
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QPushButton* refresh = send_widget.findChild<QPushButton*>(
        QStringLiteral("refreshPaymasterOffers"));
    QSpinBox* fee_cap = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterFeeCap"));
    QSpinBox* attempts = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterMaximumAttempts"));
    QComboBox* privacy = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterPrivacy"));
    QComboBox* selection = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterSelection"));
    QVERIFY(table && status && refresh && fee_cap && attempts && privacy && selection);

    QCOMPARE(table->selectionMode(), QAbstractItemView::NoSelection);
    QCOMPARE(table->focusPolicy(), Qt::StrongFocus);
    QVERIFY(table->alternatingRowColors());
    QVERIFY(table->verticalHeader()->isHidden());
    QCOMPARE(table->horizontalHeader()->objectName(),
             QStringLiteral("paymasterOffersHeader"));
    QVERIFY(!table->accessibleName().isEmpty());
    QVERIFY(!table->accessibleDescription().isEmpty());
    QCOMPARE(status->accessibleDescription(), status->text());
    QVERIFY(!refresh->accessibleDescription().isEmpty());
    QVERIFY(!fee_cap->accessibleName().isEmpty());
    QVERIFY(!attempts->accessibleName().isEmpty());
    QVERIFY(!privacy->accessibleName().isEmpty());
    QVERIFY(!selection->accessibleName().isEmpty());
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Provider"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Valid until"));

    QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(table);
    QVERIFY(interface != nullptr);
    QCOMPARE(interface->role(), QAccessible::Table);

    table->setRowCount(2);
    table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Provider A")));
    table->setItem(1, 0, new QTableWidgetItem(QStringLiteral("Provider B")));
    send_widget.resize(1200, 900);
    send_widget.show();
    QFrame* advanced = send_widget.findChild<QFrame*>(
        QStringLiteral("advancedPaymasterSettings"));
    QVERIFY(advanced != nullptr);
    advanced->show();
    table->show();
    QCoreApplication::processEvents();
    table->setCurrentCell(0, 0);
    QTest::keyClick(table, Qt::Key_Down);
    QCOMPARE(table->currentRow(), 1);
}

void PaymasterWidgetTests::paymasterOfferTableRendersGreenTheme()
{
    struct RenderStats {
        bool stylesheet_loaded{false};
        bool table_visible{false};
        int total_pixels{0};
        int green_pixels{0};
        int blue_pixels{0};
    };

    const QString original_stylesheet = qApp->styleSheet();
    const QPalette original_palette = qApp->palette();
    const auto render_theme = [](const QString& resource) {
        RenderStats stats;
        QFile file(resource);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return stats;
        stats.stylesheet_loaded = true;
        qApp->setStyleSheet(QString::fromUtf8(file.readAll()));

        std::unique_ptr<const PlatformStyle> platform_style(
            PlatformStyle::instantiate("other"));
        DigiDollarSendWidget send_widget(platform_style.get());
        QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
            QStringLiteral("feeFundingPaymaster"));
        QPushButton* advanced_toggle = send_widget.findChild<QPushButton*>(
            QStringLiteral("advancedPaymasterSettingsToggle"));
        QTableWidget* table = send_widget.findChild<QTableWidget*>(
            QStringLiteral("paymasterOffers"));
        if (!paymaster || !advanced_toggle || !table) return stats;
        paymaster->setChecked(true);
        advanced_toggle->setChecked(true);
        table->setRowCount(1);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Provider")));
        send_widget.resize(1500, 1200);
        send_widget.show();
        table->resize(1200, 160);
        QCoreApplication::processEvents();
        QTest::qWait(25);
        stats.table_visible = table->isVisibleTo(&send_widget);

        QHeaderView* horizontal_header = table->horizontalHeader();
        const QImage header = horizontal_header->grab().toImage()
                                  .convertToFormat(QImage::Format_RGB32);
        // The minimal Qt platform can leave an unpainted viewport strip after
        // the last section. Restrict the sample to the actual header sections
        // so that the window size does not dilute their rendered colour.
        const int painted_width = std::min(header.width(), horizontal_header->length());
        for (int y = 1; y + 1 < header.height(); ++y) {
            for (int x = 1; x + 1 < painted_width; ++x) {
                const QColor color = QColor::fromRgb(header.pixel(x, y));
                ++stats.total_pixels;
                if (color.green() > color.red() + 20 &&
                    color.green() > color.blue() + 10) {
                    ++stats.green_pixels;
                }
                if (color.blue() > color.green() + 30) {
                    ++stats.blue_pixels;
                }
            }
        }
        return stats;
    };

    const RenderStats light = render_theme(QStringLiteral(":/css/light"));
    const RenderStats dark = render_theme(QStringLiteral(":/css/dark"));
    qApp->setStyleSheet(original_stylesheet);
    qApp->setPalette(original_palette);
    QCoreApplication::processEvents();

    const auto require_green_header = [](const RenderStats& stats,
                                         const QString& theme) {
        QVERIFY2(stats.stylesheet_loaded,
                 qPrintable(QStringLiteral("could not load %1 stylesheet resource").arg(theme)));
        QVERIFY2(stats.table_visible,
                 qPrintable(QStringLiteral("Paymaster offer table was not visible under %1").arg(theme)));
        QVERIFY(stats.total_pixels > 0);
        QVERIFY2(stats.green_pixels * 100 >= stats.total_pixels * 40,
                 qPrintable(QStringLiteral(
                     "%1 rendered Paymaster header is not predominantly green (%2/%3 green, %4 blue pixels)")
                                .arg(theme)
                                .arg(stats.green_pixels)
                                .arg(stats.total_pixels)
                                .arg(stats.blue_pixels)));
        QVERIFY2(stats.blue_pixels * 100 < stats.total_pixels * 5,
                 qPrintable(QStringLiteral(
                     "%1 rendered Paymaster header leaked the DGB blue palette (%2/%3 blue pixels)")
                                .arg(theme)
                                .arg(stats.blue_pixels)
                                .arg(stats.total_pixels)));
    };
    require_green_header(light, QStringLiteral("light"));
    require_green_header(dark, QStringLiteral("dark"));
}

void PaymasterWidgetTests::paymasterGuidedSetupRendersConsistentTheme()
{
    struct RenderStats {
        bool wizard_opened{false};
        bool expected_local_theme{false};
        bool scroll_visible{false};
        bool surfaces_use_qss{false};
        bool native_fill_disabled{false};
        int total_pixels{0};
        int dark_pixels{0};
        int light_pixels{0};
    };

    const QString original_stylesheet = qApp->styleSheet();
    const QPalette original_palette = qApp->palette();
    qApp->setStyleSheet(QString{});

    const auto render_theme = [&](const QColor& window, const QColor& text,
                                  const QString& expected_background) {
        RenderStats stats;
        QPalette palette = original_palette;
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, window);
        palette.setColor(QPalette::Text, text);
        qApp->setPalette(palette);

        std::unique_ptr<const PlatformStyle> platform_style(
            PlatformStyle::instantiate("other"));
        DigiDollarTab tab(platform_style.get());
        QPushButton* guided_setup = tab.findChild<QPushButton*>(
            QStringLiteral("paymasterChooseGuidedSetup"));
        if (!guided_setup) return stats;

        QTimer::singleShot(0, [&] {
            auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
            if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
                wizard = nullptr;
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    auto* candidate = qobject_cast<QWizard*>(widget);
                    if (candidate &&
                        candidate->objectName() == QLatin1String("PaymasterSetupWizard")) {
                        wizard = candidate;
                        break;
                    }
                }
            }
            if (!wizard) return;

            stats.wizard_opened = true;
            stats.expected_local_theme = wizard->styleSheet().contains(
                expected_background, Qt::CaseInsensitive);
            QScrollArea* scroll = wizard->findChild<QScrollArea*>(
                QStringLiteral("paymasterSetupRequirementsScroll"));
            QWidget* contents = wizard->findChild<QWidget*>(
                QStringLiteral("paymasterSetupRequirementsContent"));
            QFrame* wallet_card = wizard->findChild<QFrame*>(
                QStringLiteral("paymasterSetupWalletCard"));
            if (scroll && contents && wallet_card) {
                QCoreApplication::processEvents();
                stats.scroll_visible = scroll->isVisibleTo(wizard);
                stats.surfaces_use_qss =
                    scroll->testAttribute(Qt::WA_StyledBackground) &&
                    scroll->viewport()->testAttribute(Qt::WA_StyledBackground) &&
                    contents->testAttribute(Qt::WA_StyledBackground);
                stats.native_fill_disabled =
                    !scroll->viewport()->autoFillBackground() &&
                    !contents->autoFillBackground();

                const QImage image = scroll->viewport()->grab().toImage()
                                         .convertToFormat(QImage::Format_RGB32);
                for (int y = 1; y + 1 < image.height(); ++y) {
                    for (int x = 1; x + 1 < image.width(); ++x) {
                        const QColor color = QColor::fromRgb(image.pixel(x, y));
                        ++stats.total_pixels;
                        if (color.lightness() < 100) ++stats.dark_pixels;
                        if (color.lightness() > 210) ++stats.light_pixels;
                    }
                }
            }
            wizard->reject();
        });
        guided_setup->click();
        return stats;
    };

    const RenderStats dark = render_theme(
        QColor(QStringLiteral("#0b2419")), QColor(QStringLiteral("#ffffff")),
        QStringLiteral("#0b2419"));
    const RenderStats light = render_theme(
        QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#123f2b")),
        QStringLiteral("#eef9f2"));

    qApp->setStyleSheet(original_stylesheet);
    qApp->setPalette(original_palette);
    QCoreApplication::processEvents();

    const auto require_common_contract = [](const RenderStats& stats,
                                            const QString& theme) {
        QVERIFY2(stats.wizard_opened,
                 qPrintable(QStringLiteral("Paymaster setup wizard did not open under %1").arg(theme)));
        QVERIFY2(stats.expected_local_theme,
                 qPrintable(QStringLiteral("Paymaster setup wizard selected the wrong %1 theme").arg(theme)));
        QVERIFY2(stats.scroll_visible,
                 qPrintable(QStringLiteral("Paymaster setup content was not visible under %1").arg(theme)));
        QVERIFY2(stats.surfaces_use_qss,
                 qPrintable(QStringLiteral("Paymaster setup surfaces bypassed QSS under %1").arg(theme)));
        QVERIFY2(stats.native_fill_disabled,
                 qPrintable(QStringLiteral("Paymaster setup leaked a native background under %1").arg(theme)));
        QVERIFY(stats.total_pixels > 0);
    };
    require_common_contract(dark, QStringLiteral("dark"));
    require_common_contract(light, QStringLiteral("light"));

    QVERIFY2(dark.dark_pixels * 100 >= dark.total_pixels * 60,
             qPrintable(QStringLiteral(
                 "dark Paymaster setup surface was not predominantly dark (%1/%2 dark, %3 light pixels)")
                            .arg(dark.dark_pixels)
                            .arg(dark.total_pixels)
                            .arg(dark.light_pixels)));
    QVERIFY2(dark.light_pixels * 100 < dark.total_pixels * 25,
             qPrintable(QStringLiteral(
                 "dark Paymaster setup leaked a large light surface (%1/%2 light pixels)")
                            .arg(dark.light_pixels)
                            .arg(dark.total_pixels)));
    QVERIFY2(light.light_pixels * 100 >= light.total_pixels * 60,
             qPrintable(QStringLiteral(
                 "light Paymaster setup surface was not predominantly light (%1/%2 light, %3 dark pixels)")
                            .arg(light.light_pixels)
                            .arg(light.total_pixels)
                            .arg(light.dark_pixels)));
}

void PaymasterWidgetTests::paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep()
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({},
            GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupDescriptorsWallet(m_node, test, "qt-paymaster-guided-retry");

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarTab tab(mini_gui.platformStyle.get());

    int operating_policy_attempts{0};
    int safety_attempts{0};
    UniValue advertised_policy;
    UniValue safety_policy;
    UniValue liquidity_policy;
    QList<UniValue> safety_payloads;
    QList<bool> enabled_requests;
    QStringList setup_write_order;
    bool fail_safety_snapshot{true};
    bool malformed_safety_snapshot{false};
    bool malformed_liquidity_snapshot{false};
    bool rpc_settings_present{false};
    bool rpc_enabled{false};
    bool rpc_running{false};
    bool rpc_ready{false};
    bool malformed_provider_snapshot{false};
    int mutation_rpc_calls{0};
    std::string rpc_operation_mode{"automatic"};
    bool rpc_autostart{false};
    const auto suggested_liquidity_policy = [] {
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("automatic_replenishment", true);
        policy.pushKV("paid_maintenance_approved", false);
        policy.pushKV("target_admission_dgb", 3);
        policy.pushKV("target_operational_dgb", 1);
        policy.pushKV("target_admission_carriers", 0);
        policy.pushKV("target_operational_carriers", 0);
        policy.pushKV(
            "maximum_maintenance_fee_per_transaction_satoshis", 20000000);
        policy.pushKV(
            "maximum_maintenance_fee_per_hour_satoshis", 200000000);
        policy.pushKV(
            "maximum_maintenance_fee_per_day_satoshis", 1000000000);
        return policy;
    };
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "startpaymaster" ||
                command == "setpaymasterenabled" ||
                command == "preparepaymasterpool" ||
                command == "rebalancepaymasterpool") {
                ++mutation_rpc_calls;
            }
            if (command == "getpaymasterinfo") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("wallet_eligible", true);
                result.pushKV("settings_present", rpc_settings_present);
                result.pushKV("provider_id",
                              "1111111111111111111111111111111111111111111111111111111111111111");
                result.pushKV("enabled", rpc_enabled);
                result.pushKV("running", rpc_running);
                result.pushKV("ready", rpc_ready);
                result.pushKV("wallet_locked", false);
                result.pushKV("pool_ready", false);
                result.pushKV("operation_mode", rpc_operation_mode);
                if (malformed_provider_snapshot) {
                    result.pushKV("autostart", "not-a-boolean");
                } else {
                    result.pushKV("autostart", rpc_autostart);
                }
                result.pushKV("service_state",
                              rpc_running ? "active" : "stopped");
                UniValue service_queue{UniValue::VOBJ};
                service_queue.pushKV("waiting_requests", 0);
                service_queue.pushKV("waiting_submits", 0);
                result.pushKV("service_queue", std::move(service_queue));
                UniValue pool{UniValue::VOBJ};
                pool.pushKV("entries", 0);
                pool.pushKV("reserved", 0);
                pool.pushKV("admission_dgb", 0);
                pool.pushKV("admission_carriers", 0);
                pool.pushKV("operational_dgb", 0);
                pool.pushKV("operational_carriers", 0);
                pool.pushKV("complete_operational_slots", 0);
                result.pushKV("pool", std::move(pool));
                result.pushKV("readiness_errors", UniValue{UniValue::VARR});
                if (advertised_policy.isObject()) {
                    result.pushKV("policy", advertised_policy);
                }
                return result;
            }
            if (command == "getpaymasterliquiditystatus") {
                UniValue result{UniValue::VOBJ};
                if (malformed_liquidity_snapshot) {
                    result.pushKV("policy_configured", true);
                    result.pushKV(
                        "targets_satisfy_provider_policy", true);
                    result.pushKV("policy", UniValue{UniValue::VOBJ});
                    return result;
                }
                const bool configured = liquidity_policy.isObject();
                const UniValue policy = configured
                    ? liquidity_policy
                    : suggested_liquidity_policy();
                result.pushKV("policy_configured", configured);
                result.pushKV("policy", policy);
                result.pushKV("targets_satisfy_provider_policy", true);
                result.pushKV(
                    "maintenance_state",
                    configured ? "ready"
                               : "waiting_for_maintenance_approval");
                const auto add_slot = [&](const char* name,
                                          const char* target_name) {
                    const int target =
                        policy.find_value(target_name).getInt<int>();
                    result.pushKV(
                        name,
                        PaymasterLiquiditySlotStatus(
                            target, 0, 0, target));
                };
                add_slot("admission_dgb", "target_admission_dgb");
                add_slot("operational_dgb", "target_operational_dgb");
                add_slot("admission_carriers",
                         "target_admission_carriers");
                add_slot("operational_carriers",
                         "target_operational_carriers");
                result.pushKV("maintenance_fee_reserved_satoshis", 0);
                result.pushKV(
                    "maintenance_fee_spent_last_hour_satoshis", 0);
                result.pushKV(
                    "maintenance_fee_spent_last_day_satoshis", 0);
                result.pushKV("carrier_base_cents", 0);
                result.pushKV("carrier_withdrawable_excess_cents", 0);
                result.pushKV("readiness_errors", UniValue{UniValue::VARR});
                return result;
            }
            if (command == "getpaymasterpoolinfo") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("pool", UniValue{UniValue::VARR});
                return result;
            }
            if (command == "getpaymastersafetystatus") {
                if (fail_safety_snapshot) {
                    throw std::runtime_error("injected safety snapshot unavailable");
                }
                UniValue result{UniValue::VOBJ};
                if (malformed_safety_snapshot) {
                    result.pushKV("configured", true);
                    result.pushKV("policy", UniValue{UniValue::VOBJ});
                    return result;
                }
                const bool configured = safety_policy.isObject();
                result.pushKV("configured", configured);
                if (configured) result.pushKV("policy", safety_policy);
                return result;
            }
            if (command == "getpaymasterclientsafetystatus") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("configured", false);
                return result;
            }
            if (command == "setpaymasterpolicy") {
                ++operating_policy_attempts;
                setup_write_order.push_back(QString::fromStdString(command));
                advertised_policy = params[0];
                UniValue result = params[0];
                result.pushKV(
                    "policy_hash",
                    "3333333333333333333333333333333333333333333333333333333333333333");
                return result;
            }
            if (command == "setpaymastersafetypolicy") {
                ++safety_attempts;
                setup_write_order.push_back(QString::fromStdString(command));
                safety_payloads.push_back(params[0]);
                if (safety_attempts == 1) {
                    throw std::runtime_error("injected safety retry");
                }
                safety_policy = params[0];
                UniValue result = params[0];
                result.pushKV("updated_at", safety_attempts);
                return result;
            }
            if (command == "setpaymasterenabled") {
                setup_write_order.push_back(QString::fromStdString(command));
                enabled_requests.push_back(params[0].get_bool());
                rpc_settings_present = true;
                rpc_enabled = params[0].get_bool();
                if (!rpc_enabled) rpc_running = false;
                UniValue result{UniValue::VOBJ};
                result.pushKV("enabled", params[0].get_bool());
                return result;
            }
            if (command == "setpaymasterliquiditypolicy") {
                setup_write_order.push_back(QString::fromStdString(command));
                liquidity_policy = params[0];
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "setpaymasterruntimesettings") {
                setup_write_order.push_back(QString::fromStdString(command));
                rpc_operation_mode =
                    params[0].find_value("operation_mode").get_str();
                rpc_autostart = params[0].find_value("autostart").get_bool();
                return params[0];
            }
            if (command == "preparepaymasterpool") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "4444444444444444444444444444444444444444444444444444444444444444");
                result.pushKV("admission_dgb_slots",
                              params[0].find_value(
                                  "admission_dgb_slots").getInt<int>());
                result.pushKV("operational_dgb_slots",
                              params[0].find_value(
                                  "operational_dgb_slots").getInt<int>());
                result.pushKV("admission_carrier_slots",
                              params[0].find_value(
                                  "admission_carrier_slots").getInt<int>());
                result.pushKV("operational_carrier_slots",
                              params[0].find_value(
                                  "operational_carrier_slots").getInt<int>());
                result.pushKV("missing_admission_dgb_slots", 0);
                result.pushKV("missing_operational_dgb_slots", 0);
                result.pushKV("missing_admission_carrier_slots", 0);
                result.pushKV("missing_operational_carrier_slots", 0);
                result.pushKV("admission_dgb_satoshis_each", 10000000);
                result.pushKV("operational_dgb_satoshis_each", 10000000);
                result.pushKV("carrier_cents_each", 100);
                result.pushKV("total_output_satoshis", 0);
                result.pushKV("total_carrier_cents", 0);
                return result;
            }
            throw std::runtime_error("injected unrelated status unavailable");
        });
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());

    QPushButton* guided_setup = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterChooseGuidedSetup"));
    QVERIFY(guided_setup != nullptr);

    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(provider_status != nullptr);
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    fail_safety_snapshot = false;
    malformed_safety_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_safety_snapshot = false;
    malformed_liquidity_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_liquidity_snapshot = false;
    malformed_provider_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_provider_snapshot = false;
    tab.setWalletModel(mini_gui.walletModel.get());

    // Model the unconfigured status that previously replaced the UI's 3/1
    // carrier preset with zeroes before the wizard was opened.
    UniValue unconfigured_liquidity_policy{UniValue::VOBJ};
    unconfigured_liquidity_policy.pushKV("automatic_replenishment", true);
    unconfigured_liquidity_policy.pushKV("paid_maintenance_approved", false);
    unconfigured_liquidity_policy.pushKV("target_admission_dgb", 3);
    unconfigured_liquidity_policy.pushKV("target_operational_dgb", 1);
    unconfigured_liquidity_policy.pushKV("target_admission_carriers", 0);
    unconfigured_liquidity_policy.pushKV("target_operational_carriers", 0);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_transaction_satoshis", 20000000);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_hour_satoshis", 200000000);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_day_satoshis", 1000000000);
    UniValue unconfigured_liquidity{UniValue::VOBJ};
    unconfigured_liquidity.pushKV("policy_configured", false);
    unconfigured_liquidity.pushKV(
        "targets_satisfy_provider_policy", true);
    unconfigured_liquidity.pushKV("policy",
                                  unconfigured_liquidity_policy);
    tab.setPaymasterLiquidityStatusForTesting(unconfigured_liquidity);
    QSpinBox* configured_admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* configured_operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QVERIFY(configured_admission_carriers != nullptr);
    QVERIFY(configured_operational_carriers != nullptr);
    QCOMPARE(configured_admission_carriers->value(), 0);
    QCOMPARE(configured_operational_carriers->value(), 0);

    bool review_cancelled_without_writes{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        QCheckBox* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        if (!wallet_confirmation) {
            wizard->reject();
            return;
        }
        wallet_confirmation->setChecked(true);
        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            wizard->next();
        }
        review_cancelled_without_writes = wizard->currentPage() &&
            wizard->currentPage()->objectName() ==
                QLatin1String("paymasterSetupReviewPage");
        wizard->reject();
    });
    guided_setup->click();
    QVERIFY(review_cancelled_without_writes);
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);
    QVERIFY(liquidity_policy.isNull());

    bool wizard_opened{false};
    bool user_paid_defaults_ready{false};
    bool recommended_default{false};
    bool identity_name_validation_ready{false};
    bool sponsored_policy_defaults_preserve_liquidity{false};
    bool profile_keeps_network_fee{false};
    bool approval_invalidated_by_custom_limit{false};
    bool maintenance_start_copy_consistent{false};
    bool safety_page_visited{false};
    bool compact_pages_scroll{false};
    bool first_failure_visible{false};
    bool durable_phase_is_close_only{false};
    bool retry_feedback_visible{false};
    bool retry_completed{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        wizard_opened = true;

        QCheckBox* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        QCheckBox* user_paid = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupUserPaid"));
        QCheckBox* sponsored = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupSponsored"));
        QComboBox* sponsorship_scope = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSponsorshipScope"));
        QLineEdit* display_name = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupDisplayName"));
        QDoubleSpinBox* service_fee = wizard->findChild<QDoubleSpinBox*>(
            QStringLiteral("paymasterSetupServiceFeePercent"));
        QSpinBox* minimum_payment = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupMinimumPayment"));
        QSpinBox* network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        QComboBox* safety_profile = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSafetyProfile"));
        QLineEdit* custom_per_transaction = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerTransaction"));
        QLineEdit* custom_reserved = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomReserved"));
        QLineEdit* custom_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerHour"));
        QLineEdit* custom_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerDay"));
        QSpinBox* custom_completed_per_hour = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomCompletedPerHour"));
        QSpinBox* custom_completed_per_day = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomCompletedPerDay"));
        QSpinBox* custom_quotes_total = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesTotal"));
        QSpinBox* custom_quotes_per_netgroup = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesPerNetgroup"));
        QSpinBox* custom_quotes_per_recipient = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesPerRecipient"));
        QSpinBox* custom_requests_per_netgroup = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxQuoteRequestsPerNetgroupMinute"));
        QLineEdit* custom_maintenance_per_transaction = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerTransaction"));
        QLineEdit* custom_maintenance_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerHour"));
        QLineEdit* custom_maintenance_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerDay"));
        QSpinBox* admission_dgb = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionDgbSlots"));
        QSpinBox* operational_dgb = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalDgbSlots"));
        QSpinBox* admission_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionCarrierSlots"));
        QSpinBox* operational_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalCarrierSlots"));
        QPushButton* restore_funding_defaults = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreFundingDefaults"));
        QPushButton* restore_policy_defaults = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        QCheckBox* maintenance_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupMaintenanceApproval"));
        QPushButton* retry = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRetry"));
        QLabel* result = wizard->findChild<QLabel*>(
            QStringLiteral("paymasterSetupProgressResult"));
        auto* progress_page = wizard->findChild<QWizardPage*>(
            QStringLiteral("paymasterSetupProgressPage"));
        auto* safety_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupSafetyScroll"));
        auto* liquidity_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupLiquidityScroll"));
        auto* review_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupReviewScroll"));
        auto* progress_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupProgressScroll"));
        if (!wallet_confirmation || !user_paid || !sponsored ||
            !sponsorship_scope || !display_name || !service_fee ||
            !minimum_payment || !network_fee ||
            !safety_profile || !custom_per_transaction ||
            !custom_reserved || !custom_per_hour ||
            !custom_per_day || !custom_completed_per_hour ||
            !custom_completed_per_day || !custom_quotes_total ||
            !custom_quotes_per_netgroup || !custom_quotes_per_recipient ||
            !custom_requests_per_netgroup ||
            !custom_maintenance_per_transaction ||
            !custom_maintenance_per_hour || !custom_maintenance_per_day ||
            !admission_dgb || !operational_dgb ||
            !admission_carriers || !operational_carriers ||
            !restore_funding_defaults || !restore_policy_defaults ||
            !maintenance_confirmation || !retry || !result || !progress_page ||
            !safety_scroll || !liquidity_scroll || !review_scroll ||
            !progress_scroll) {
            wizard->reject();
            return;
        }
        wizard->resize(560, 440);
        compact_pages_scroll = safety_scroll->widgetResizable() &&
            liquidity_scroll->widgetResizable() &&
            review_scroll->widgetResizable() &&
            progress_scroll->widgetResizable() &&
            safety_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            liquidity_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            review_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            progress_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff;

        user_paid_defaults_ready = user_paid->isChecked() &&
            !sponsored->isChecked() && admission_dgb->value() >= 3 &&
            operational_dgb->value() >= 1 && admission_carriers->value() >= 3 &&
            operational_carriers->value() >= 1;
        recommended_default = safety_profile->currentData().toString() ==
            QLatin1String("recommended");

        const QString valid_display_name(32, QLatin1Char('A'));
        display_name->setText(valid_display_name);
        const bool valid_name_accepted = display_name->hasAcceptableInput() &&
            display_name->maxLength() == 32;
        display_name->setText(QStringLiteral("invalid/name"));
        const bool slash_rejected = !display_name->hasAcceptableInput();
        display_name->setText(QStringLiteral("invalid@name"));
        const bool at_rejected = !display_name->hasAcceptableInput();
        display_name->setText(QString::fromUtf8("N\xC3\xA4me"));
        const bool unicode_rejected = !display_name->hasAcceptableInput();
        display_name->clear();
        identity_name_validation_ready = valid_name_accepted &&
            slash_rejected && at_rejected && unicode_rejected;

        user_paid->setChecked(false);
        sponsored->setChecked(true);
        sponsorship_scope->setCurrentIndex(sponsorship_scope->findData(
            QStringLiteral("restricted")));
        restore_policy_defaults->click();
        sponsored_policy_defaults_preserve_liquidity =
            !user_paid->isChecked() &&
            sponsored->isChecked() && !service_fee->isEnabled() &&
            service_fee->value() == 0.0 && minimum_payment->minimum() == 100 &&
            admission_carriers->value() == 3 &&
            operational_carriers->value() == 1;
        restore_funding_defaults->click();
        restore_policy_defaults->click();

        wallet_confirmation->setChecked(true);
        user_paid->setChecked(true);
        sponsored->setChecked(false);
        // Safety profiles constrain aggregate/request exposure, not the
        // operator's independently chosen ceiling for one valid transfer.
        network_fee->setValue(30000000);
        const bool fee_before_profile_change =
            network_fee->value() == 30000000;
        safety_profile->setCurrentIndex(
            safety_profile->findData(QStringLiteral("custom")));
        profile_keeps_network_fee = fee_before_profile_change &&
            network_fee->value() == 30000000;
        custom_per_transaction->setText(QStringLiteral("0.30000000"));
        custom_reserved->setText(QStringLiteral("0.40000000"));
        custom_per_hour->setText(QStringLiteral("90071992.54740993"));
        custom_per_day->setText(QStringLiteral("21000000000.00000000"));
        custom_completed_per_hour->setValue(7);
        custom_completed_per_day->setValue(30);
        custom_quotes_total->setValue(20);
        custom_quotes_per_netgroup->setValue(5);
        custom_quotes_per_recipient->setValue(3);
        custom_requests_per_netgroup->setValue(12);
        custom_maintenance_per_transaction->setText(QStringLiteral("0.15000000"));
        custom_maintenance_per_hour->setText(QStringLiteral("0.50000000"));
        custom_maintenance_per_day->setText(QStringLiteral("2.00000000"));

        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            if (wizard->currentPage()->objectName() ==
                QLatin1String("paymasterSetupSafetyPage")) {
                safety_page_visited = true;
            }
            wizard->next();
        }
        if (!wizard->currentPage() ||
            wizard->currentPage()->objectName() !=
                QLatin1String("paymasterSetupReviewPage")) {
            wizard->reject();
            return;
        }
        maintenance_start_copy_consistent =
            maintenance_confirmation->text().contains(
                QStringLiteral("approval alone does not start"),
                Qt::CaseInsensitive) &&
            !maintenance_confirmation->text().contains(
                QStringLiteral("until I start it separately"),
                Qt::CaseInsensitive);
        maintenance_confirmation->setChecked(true);
        custom_maintenance_per_day->setText(QStringLiteral("2.10000000"));
        approval_invalidated_by_custom_limit =
            !maintenance_confirmation->isChecked();
        custom_maintenance_per_day->setText(QStringLiteral("2.00000000"));
        maintenance_confirmation->setChecked(true);

        wizard->next();

        first_failure_visible =
            wizard->currentPage() == progress_page && retry->isVisible() &&
            result->text().contains(QStringLiteral("injected safety retry"));
        durable_phase_is_close_only =
            wizard->buttonText(QWizard::CancelButton) == QStringLiteral("Close");
        retry->click();
        retry_feedback_visible = !retry->isEnabled() &&
            result->text().contains(QStringLiteral("Retrying"));
        QCoreApplication::processEvents();
        retry_completed = progress_page->isComplete() && !retry->isVisible();
        wizard->reject();
    });
    guided_setup->click();

    QVERIFY(wizard_opened);
    QVERIFY(user_paid_defaults_ready);
    QVERIFY(recommended_default);
    QVERIFY(identity_name_validation_ready);
    QVERIFY(sponsored_policy_defaults_preserve_liquidity);
    QVERIFY(profile_keeps_network_fee);
    QVERIFY(approval_invalidated_by_custom_limit);
    QVERIFY(maintenance_start_copy_consistent);
    QVERIFY(safety_page_visited);
    QVERIFY(compact_pages_scroll);
    QVERIFY(first_failure_visible);
    QVERIFY(durable_phase_is_close_only);
    QVERIFY(retry_feedback_visible);
    QVERIFY(retry_completed);
    QCOMPARE(operating_policy_attempts, 1);
    QCOMPARE(safety_attempts, 2);
    QCOMPARE(enabled_requests, QList<bool>{true});

    // A complete status may enable provider and funding actions. Any later
    // malformed provider snapshot must revoke all of them immediately and a
    // click on the now-disabled controls must not reach a mutation RPC.
    rpc_ready = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    const QStringList fail_closed_actions{
        QStringLiteral("paymasterStartProvider"),
        QStringLiteral("paymasterEnableProvider"),
        QStringLiteral("paymasterPreviewPoolPreparation"),
        QStringLiteral("paymasterPreviewPoolRetirement"),
    };
    for (const QString& object_name : fail_closed_actions) {
        QPushButton* action = tab.findChild<QPushButton*>(object_name);
        QVERIFY2(action != nullptr, qPrintable(object_name));
        QVERIFY2(action->isEnabled(),
                 qPrintable(QStringLiteral("Expected a valid snapshot to enable %1")
                                .arg(object_name)));
    }
    malformed_provider_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    const int mutation_calls_before_disabled_clicks = mutation_rpc_calls;
    for (const QString& object_name : fail_closed_actions) {
        QPushButton* action = tab.findChild<QPushButton*>(object_name);
        QVERIFY2(action != nullptr, qPrintable(object_name));
        QVERIFY2(!action->isEnabled(),
                 qPrintable(QStringLiteral("Malformed status left %1 enabled")
                                .arg(object_name)));
        action->click();
    }
    QCOMPARE(mutation_rpc_calls, mutation_calls_before_disabled_clicks);
    malformed_provider_snapshot = false;
    rpc_ready = false;
    tab.setWalletModel(mini_gui.walletModel.get());

    const qint64 advertised_fee = advertised_policy.find_value(
        "maximum_network_fee_dgb_satoshis").getInt<qint64>();
    const UniValue& user_paid_safety = safety_policy.find_value("user_paid");
    const qint64 safety_fee = user_paid_safety.find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>();
    QCOMPARE(advertised_fee, 30000000);
    QCOMPARE(safety_fee, advertised_fee);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_reserved_network_fee_satoshis").getInt<qint64>(), 40000000);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_network_fee_per_hour_satoshis").getInt<qint64>(),
        9007199254740993LL);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_network_fee_per_day_satoshis").getInt<qint64>(),
        2100000000000000000LL);
    QCOMPARE(user_paid_safety.find_value("maximum_completed_per_hour").getInt<int>(), 7);
    QCOMPARE(user_paid_safety.find_value("maximum_completed_per_day").getInt<int>(), 30);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_total").getInt<int>(), 20);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_per_netgroup").getInt<int>(), 5);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_per_recipient").getInt<int>(), 3);
    QCOMPARE(safety_policy.find_value(
        "maximum_quote_requests_per_netgroup_per_minute").getInt<int>(), 12);
    QCOMPARE(liquidity_policy.find_value("target_admission_dgb").getInt<int>(), 3);
    QCOMPARE(liquidity_policy.find_value("target_operational_dgb").getInt<int>(), 1);
    QCOMPARE(liquidity_policy.find_value("target_admission_carriers").getInt<int>(), 3);
    QCOMPARE(liquidity_policy.find_value("target_operational_carriers").getInt<int>(), 1);
    QVERIFY(liquidity_policy.find_value("automatic_replenishment").get_bool());
    QVERIFY(liquidity_policy.find_value("paid_maintenance_approved").get_bool());
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_transaction_satoshis").getInt<qint64>(), 15000000);
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_hour_satoshis").getInt<qint64>(), 50000000);
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_day_satoshis").getInt<qint64>(), 200000000);

    UniValue disabled_existing{UniValue::VOBJ};
    disabled_existing.pushKV("wallet_eligible", true);
    disabled_existing.pushKV("settings_present", true);
    disabled_existing.pushKV("enabled", false);
    disabled_existing.pushKV("running", false);
    disabled_existing.pushKV("ready", false);
    disabled_existing.pushKV("wallet_locked", false);
    disabled_existing.pushKV("has_identity", true);
    disabled_existing.pushKV("has_policy", true);
    disabled_existing.pushKV("has_safety_policy", true);
    disabled_existing.pushKV("operation_mode", "automatic");
    disabled_existing.pushKV("autostart", false);
    disabled_existing.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(disabled_existing);

    bool reopened_existing_custom_values{false};
    bool restore_default_actions_available{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        auto* profile = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSafetyProfile"));
        auto* reopened_network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        auto* reopened_reserved = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomReserved"));
        auto* reopened_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerHour"));
        auto* reopened_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerDay"));
        auto* reopened_quotes_total = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesTotal"));
        auto* reopened_maintenance_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerDay"));
        auto* reopened_automatic = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutomaticReplenishment"));
        auto* reopened_operation_mode = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupOperationMode"));
        auto* reopened_autostart = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutostart"));
        auto* reopened_provider_enabled = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupProviderEnabled"));
        auto* restore_identity = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreIdentityDefaults"));
        auto* restore_funding = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreFundingDefaults"));
        auto* restore_policy = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        auto* restore_safety = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreSafetyDefaults"));
        auto* restore_liquidity = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreLiquidityDefaults"));
        if (!profile || !reopened_network_fee || !reopened_reserved ||
            !reopened_per_hour || !reopened_per_day || !reopened_quotes_total ||
            !reopened_maintenance_day || !reopened_automatic ||
            !reopened_operation_mode || !reopened_autostart ||
            !reopened_provider_enabled ||
            !restore_identity || !restore_funding || !restore_policy ||
            !restore_safety || !restore_liquidity) {
            wizard->reject();
            return;
        }
        reopened_existing_custom_values =
            profile->currentData().toString() == QLatin1String("custom") &&
            reopened_network_fee->value() == 30000000 &&
            reopened_reserved->text() == QLatin1String("0.40000000") &&
            reopened_per_hour->text() ==
                QLatin1String("90071992.54740993") &&
            reopened_per_day->text() ==
                QLatin1String("21000000000.00000000") &&
            reopened_quotes_total->value() == 20 &&
            reopened_maintenance_day->text() == QLatin1String("2.00000000") &&
            reopened_automatic->isChecked() &&
            reopened_operation_mode->currentData().toString() ==
                QLatin1String("automatic") &&
            !reopened_autostart->isChecked() &&
            !reopened_provider_enabled->isChecked();

        restore_identity->click();
        restore_funding->click();
        restore_policy->click();
        restore_safety->click();
        restore_liquidity->click();
        restore_default_actions_available =
            profile->currentData().toString() == QLatin1String("recommended") &&
            reopened_network_fee->value() == 20000000 &&
            reopened_quotes_total->value() == 16 &&
            reopened_provider_enabled->isChecked();
        wizard->reject();
    });
    guided_setup->click();
    QVERIFY(reopened_existing_custom_values);
    QVERIFY(restore_default_actions_available);

    setup_write_order.clear();
    rpc_settings_present = true;
    rpc_enabled = true;
    UniValue enabled_existing{UniValue::VOBJ};
    enabled_existing.pushKV("wallet_eligible", true);
    enabled_existing.pushKV("settings_present", true);
    enabled_existing.pushKV("enabled", true);
    enabled_existing.pushKV("running", false);
    enabled_existing.pushKV("ready", false);
    enabled_existing.pushKV("wallet_locked", false);
    enabled_existing.pushKV("has_identity", true);
    enabled_existing.pushKV("has_policy", true);
    enabled_existing.pushKV("has_safety_policy", true);
    enabled_existing.pushKV("operation_mode", "automatic");
    enabled_existing.pushKV("autostart", true);
    enabled_existing.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(enabled_existing);

    bool reconfiguration_completed{false};
    bool existing_carriers_retained{false};
    bool disabled_target_retained{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(
            QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() !=
                QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        auto* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        auto* user_paid = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupUserPaid"));
        auto* sponsored = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupSponsored"));
        auto* scope = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSponsorshipScope"));
        auto* fee = wizard->findChild<QDoubleSpinBox*>(
            QStringLiteral("paymasterSetupServiceFeePercent"));
        auto* minimum = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupMinimumPayment"));
        auto* network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        auto* admission_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionCarrierSlots"));
        auto* operational_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalCarrierSlots"));
        auto* provider_enabled = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupProviderEnabled"));
        auto* autostart = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutostart"));
        auto* restore_policy = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        auto* restore_safety = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreSafetyDefaults"));
        auto* approval = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupMaintenanceApproval"));
        auto* progress_page = wizard->findChild<QWizardPage*>(
            QStringLiteral("paymasterSetupProgressPage"));
        if (!wallet_confirmation || !user_paid || !sponsored || !scope ||
            !fee || !minimum || !network_fee || !admission_carriers ||
            !operational_carriers || !provider_enabled || !autostart ||
            !restore_policy || !restore_safety || !approval ||
            !progress_page) {
            wizard->reject();
            return;
        }

        wallet_confirmation->setChecked(true);
        user_paid->setChecked(false);
        sponsored->setChecked(true);
        scope->setCurrentIndex(scope->findData(QStringLiteral("restricted")));
        restore_policy->click();
        network_fee->setValue(10000000);
        restore_safety->click();
        provider_enabled->setChecked(false);
        autostart->setChecked(true);
        existing_carriers_retained = admission_carriers->value() == 3 &&
            operational_carriers->value() == 1;
        disabled_target_retained = !provider_enabled->isChecked();

        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            wizard->next();
        }
        if (!wizard->currentPage() ||
            wizard->currentPage()->objectName() !=
                QLatin1String("paymasterSetupReviewPage")) {
            wizard->reject();
            return;
        }
        approval->setChecked(true);
        wizard->next();
        QCoreApplication::processEvents();
        reconfiguration_completed = progress_page->isComplete();
        wizard->reject();
    });
    guided_setup->click();

    QVERIFY(reconfiguration_completed);
    QVERIFY(existing_carriers_retained);
    QVERIFY(disabled_target_retained);
    QCOMPARE(operating_policy_attempts, 2);
    QCOMPARE(safety_attempts, 4);
    QCOMPARE(enabled_requests, QList<bool>({true, false, false}));
    QCOMPARE(setup_write_order, QStringList({
        QStringLiteral("setpaymasterenabled"),
        QStringLiteral("setpaymastersafetypolicy"),
        QStringLiteral("setpaymasterpolicy"),
        QStringLiteral("setpaymastersafetypolicy"),
        QStringLiteral("setpaymasterliquiditypolicy"),
        QStringLiteral("setpaymasterruntimesettings"),
        QStringLiteral("setpaymasterenabled"),
    }));
    QCOMPARE(advertised_policy.find_value("sponsorship_scope").get_str(),
             std::string{"restricted"});
    QCOMPARE(advertised_policy.find_value("fee_rate_bps").getInt<int>(), 0);
    QCOMPARE(advertised_policy.find_value(
        "maximum_network_fee_dgb_satoshis").getInt<qint64>(), 10000000);
    const UniValue& final_models =
        advertised_policy.find_value("funding_models");
    QCOMPARE(final_models.size(), 1U);
    QCOMPARE(final_models[0].get_str(), std::string{"sponsored"});
    const UniValue& bridge = safety_payloads.at(2);
    QCOMPARE(bridge.find_value("user_paid").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);
    QCOMPARE(bridge.find_value("restricted_sponsored").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);
    // The final policy preserves the already persisted budget of the model
    // that is no longer selected. Only the temporary bridge is capped while
    // the two independently validated Core policies change order.
    QCOMPARE(safety_policy.find_value("user_paid").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        30000000);
    QCOMPARE(safety_policy.find_value("restricted_sponsored").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);

    UniValue disabled_with_autostart{UniValue::VOBJ};
    disabled_with_autostart.pushKV("wallet_eligible", true);
    disabled_with_autostart.pushKV("settings_present", true);
    disabled_with_autostart.pushKV("enabled", false);
    disabled_with_autostart.pushKV("running", false);
    disabled_with_autostart.pushKV("ready", false);
    disabled_with_autostart.pushKV("wallet_locked", false);
    disabled_with_autostart.pushKV("has_identity", true);
    disabled_with_autostart.pushKV("has_policy", true);
    disabled_with_autostart.pushKV("has_safety_policy", true);
    disabled_with_autostart.pushKV("operation_mode", "automatic");
    disabled_with_autostart.pushKV("autostart", true);
    disabled_with_autostart.pushKV("service_state", "stopped");
    disabled_with_autostart.pushKV("readiness_errors",
                                   UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(disabled_with_autostart);
    QLabel* runtime_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivityRuntimeStatus"));
    QVERIFY(runtime_status != nullptr);
    QVERIFY(runtime_status->text().contains(
        QStringLiteral("configuration is disabled"), Qt::CaseInsensitive));
    QVERIFY(!runtime_status->text().contains(
        QStringLiteral("will start it"), Qt::CaseInsensitive));
}
