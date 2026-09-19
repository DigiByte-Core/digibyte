// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/paymasterwidget.h>

#include <qt/digidollaroverviewwidget.h>
#include <qt/digidollarreceivewidget.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarmintwidget.h>
#include <qt/optionsmodel.h>
#include <qt/digidollarredeemwidget.h>
#include <qt/digidollarpositionswidget.h>
#include <qt/digidollartransactionswidget.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>

#include <QTabWidget>
#include <QApplication>
#include <QTabBar>
#include <QVBoxLayout>
#include <QTimer>
#include <QLabel>
#include <QStackedWidget>
#include <QFont>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QTextStream>
#include <QVariant>
#include <QWizard>
#include <QWizardPage>
#include <QWheelEvent>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <paymaster/directory.h>
#include <paymaster/types.h>
#include <validation.h>
#include <versionbits.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>

namespace {
constexpr size_t MAX_PAYMASTER_RPC_ARRAY_ENTRIES{128};
constexpr size_t MAX_PAYMASTER_RPC_TEXT_BYTES{1024};
constexpr size_t MAX_PAYMASTER_POOL_ENTRIES{8192};
constexpr size_t MAX_PAYMASTER_FINANCE_DAYS{366};
constexpr size_t MAX_PAYMASTER_FINANCE_PAGE_EVENTS{250};

enum class PaymasterSetupMode {
    UNDECIDED,
    GUIDED,
    EXPERT,
    EXISTING,
};

QString PaymasterSetupModeSettingsKey(const WalletModel* model)
{
    if (!model) return {};
    const QByteArray wallet_identity =
        QByteArray::number(static_cast<int>(Params().GetChainType())) + '\0' +
        model->getWalletName().toUtf8();
    const QByteArray wallet_id = QCryptographicHash::hash(
        wallet_identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("Paymaster/Wallet/%1/SetupMode")
        .arg(QString::fromLatin1(wallet_id));
}

class NoWheelSpinBox final : public QSpinBox
{
public:
    explicit NoWheelSpinBox(QWidget* parent) : QSpinBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelDoubleSpinBox final : public QDoubleSpinBox
{
public:
    explicit NoWheelDoubleSpinBox(QWidget* parent) : QDoubleSpinBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelComboBox final : public QComboBox
{
public:
    explicit NoWheelComboBox(QWidget* parent) : QComboBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelScaledSpinBox final : public QSpinBox
{
public:
    NoWheelScaledSpinBox(double scale, int decimals, QWidget* parent)
        : QSpinBox(parent), m_scale(scale), m_decimals(decimals)
    {
    }

protected:
    QString textFromValue(int value) const override
    {
        return locale().toString(value / m_scale, 'f', m_decimals);
    }

    int valueFromText(const QString& text) const override
    {
        QString number = text;
        number.remove(suffix());
        bool ok{false};
        const double value = locale().toDouble(number.trimmed(), &ok);
        if (!ok) return minimum();
        return qBound(minimum(), qRound(value * m_scale), maximum());
    }

    void wheelEvent(QWheelEvent* event) override { event->ignore(); }

private:
    const double m_scale;
    const int m_decimals;
};

class DgbAmountLineEdit final : public QLineEdit
{
public:
    explicit DgbAmountLineEdit(qint64 satoshis, QWidget* parent)
        : QLineEdit(parent)
    {
        setMaxLength(24);
        setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]{1,11}([.,][0-9]{0,8})?")), this));
        setSatoshis(satoshis);
    }

    bool satoshis(qint64& value) const
    {
        QString amount = text().trimmed();
        amount.replace(',', '.');
        const QStringList parts = amount.split('.');
        if (parts.isEmpty() || parts.size() > 2) return false;
        bool whole_ok{false};
        const qint64 whole = parts.at(0).toLongLong(&whole_ok);
        if (!whole_ok || whole < 0 || whole > 92233720368LL) return false;
        QString fraction = parts.size() == 2 ? parts.at(1) : QString{};
        if (fraction.size() > 8) return false;
        fraction = fraction.leftJustified(8, QLatin1Char('0'));
        bool fraction_ok{true};
        const qint64 fractional = fraction.isEmpty() ? 0 : fraction.toLongLong(&fraction_ok);
        if (!fraction_ok) return false;
        if (whole > (std::numeric_limits<qint64>::max() - fractional) / 100000000) return false;
        value = whole * COIN + fractional;
        return MoneyRange(value);
    }

    void setSatoshis(qint64 satoshis)
    {
        const qint64 whole = satoshis / COIN;
        const qint64 fractional = satoshis % COIN;
        setText(QStringLiteral("%1.%2")
                    .arg(whole)
                    .arg(fractional, 8, 10, QLatin1Char('0')));
    }
};

class PaymasterDisplayNameValidator final : public QValidator
{
public:
    explicit PaymasterDisplayNameValidator(QObject* parent)
        : QValidator(parent)
    {
    }

    State validate(QString& input, int&) const override
    {
        return DigiDollar::Paymaster::IsValidPaymasterDisplayName(
                   input.toStdString())
            ? Acceptable
            : Invalid;
    }
};

class PaymasterSetupWizard final : public QWizard
{
public:
    explicit PaymasterSetupWizard(QWidget* parent) : QWizard(parent) {}

    void setCloseBlocked(bool blocked) { m_close_blocked = blocked; }

public:
    void reject() override
    {
        if (!m_close_blocked) QWizard::reject();
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (m_close_blocked) {
            event->ignore();
            return;
        }
        QWizard::closeEvent(event);
    }

private:
    bool m_close_blocked{false};
};

class CompletableWizardPage : public QWizardPage
{
public:
    explicit CompletableWizardPage(QWidget* parent = nullptr) : QWizardPage(parent) {}

    bool isComplete() const override { return m_complete; }

    void setComplete(bool complete)
    {
        if (m_complete == complete) return;
        m_complete = complete;
        Q_EMIT completeChanged();
    }

private:
    bool m_complete{false};
};

class ValidatedWizardPage final : public CompletableWizardPage
{
public:
    explicit ValidatedWizardPage(QWidget* parent = nullptr) : CompletableWizardPage(parent) {}

    void setValidator(std::function<bool()> validator)
    {
        m_validator = std::move(validator);
    }

    bool validatePage() override
    {
        return !m_validator || m_validator();
    }

private:
    std::function<bool()> m_validator;
};

void ConfigureScrollArea(QScrollArea* scroll, QWidget* contents)
{
    // Call this after QScrollArea::setWidget(). Qt deliberately enables
    // autoFillBackground on an inserted widget, which would otherwise restore
    // the native Windows window colour on top of the active QSS theme.
    scroll->setFrameShape(QFrame::NoFrame);
    // Do not fill the viewport from the native Windows palette. The Paymaster
    // pages are themed through QSS; forcing QPalette::Window here otherwise
    // leaves a bright surface behind dark-theme controls.
    scroll->setAttribute(Qt::WA_StyledBackground, true);
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    contents->setAttribute(Qt::WA_StyledBackground, true);
    scroll->viewport()->setAutoFillBackground(false);
    contents->setAutoFillBackground(false);
}

QWidget* CreatePaymasterPageColumn(QWidget* page, QVBoxLayout*& layout)
{
    auto* shell = new QHBoxLayout(page);
    shell->setContentsMargins(18, 14, 18, 18);
    shell->setSpacing(0);
    auto* column = new QWidget(page);
    column->setObjectName(QStringLiteral("paymasterPageColumn"));
    column->setMaximumWidth(1100);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    shell->addStretch(1);
    shell->addWidget(column, 8);
    shell->addStretch(1);
    return column;
}

void AddPaymasterPageHeading(QVBoxLayout* layout, QWidget* parent,
                             const QString& title, const QString& description,
                             const QString& object_prefix)
{
    auto* heading = new QLabel(title, parent);
    heading->setObjectName(object_prefix + QStringLiteral("Heading"));
    heading->setProperty("paymasterRole", QStringLiteral("pageHeading"));
    auto* help = new QLabel(description, parent);
    help->setObjectName(object_prefix + QStringLiteral("Introduction"));
    help->setProperty("paymasterRole", QStringLiteral("pageIntroduction"));
    help->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(help);
}

QPushButton* AddPaymasterDisclosure(QVBoxLayout* layout, QWidget* parent,
                                    QWidget* panel, const QString& show_text,
                                    const QString& hide_text,
                                    const QString& object_name,
                                    bool initially_open = false)
{
    auto* button = new QPushButton(initially_open ? hide_text : show_text, parent);
    button->setObjectName(object_name);
    button->setProperty("paymasterRole", QStringLiteral("disclosure"));
    button->setCheckable(true);
    button->setChecked(initially_open);
    button->setAccessibleName(show_text);
    panel->setVisible(initially_open);
    layout->addWidget(button, 0, Qt::AlignLeft);
    layout->addWidget(panel);
    QObject::connect(button, &QPushButton::toggled, panel,
                     [button, panel, show_text, hide_text](bool visible) {
                         panel->setVisible(visible);
                         button->setText(visible ? hide_text : show_text);
                     });
    return button;
}

QFrame* CreatePaymasterStatusCard(QWidget* parent, const QString& title,
                                  QLabel*& status, QPushButton*& action)
{
    auto* card = new QFrame(parent);
    card->setProperty("paymasterRole", QStringLiteral("statusCard"));
    card->setMinimumHeight(142);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(16, 14, 16, 14);
    card_layout->setSpacing(8);
    auto* heading = new QLabel(title, card);
    heading->setProperty("paymasterRole", QStringLiteral("cardHeading"));
    status = new QLabel(QObject::tr("Checking…"), card);
    status->setProperty("paymasterRole", QStringLiteral("statusText"));
    status->setWordWrap(true);
    action = new QPushButton(QObject::tr("Review"), card);
    action->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    action->setCursor(Qt::PointingHandCursor);
    card_layout->addWidget(heading);
    card_layout->addWidget(status);
    card_layout->addStretch();
    card_layout->addWidget(action, 0, Qt::AlignLeft);
    return card;
}

class PaymasterResponsiveCards final : public QWidget
{
public:
    explicit PaymasterResponsiveCards(QWidget* parent = nullptr)
        : QWidget(parent), m_layout(new QGridLayout(this))
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setHorizontalSpacing(12);
        m_layout->setVerticalSpacing(12);
    }

    void addCard(QWidget* card)
    {
        m_cards.push_back(card);
        relayout();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        relayout();
    }

private:
    void relayout()
    {
        const int columns = width() > 0 && width() < 760 ? 1 : 2;
        for (QWidget* card : m_cards) m_layout->removeWidget(card);
        for (int index = 0; index < m_cards.size(); ++index) {
            m_layout->addWidget(m_cards.at(index), index / columns,
                                index % columns);
        }
        for (int column = 0; column < 2; ++column) {
            m_layout->setColumnStretch(column, column < columns ? 1 : 0);
        }

        // A responsive grid changes its row count after the page receives its
        // final width. Keep that new minimum height visible to the surrounding
        // page layout; otherwise Qt can retain the old, shorter allocation and
        // the cards paint over the widget that follows the grid.
        m_layout->invalidate();
        const int required_height = m_layout->minimumSize().height();
        if (minimumHeight() != required_height) {
            setMinimumHeight(required_height);
            updateGeometry();
        }
    }

    QGridLayout* const m_layout;
    QList<QWidget*> m_cards;
};

bool UseDarkPaymasterWizardTheme(const WalletModel* model, const QWidget* widget)
{
    if (model && model->getOptionsModel()) {
        const QString theme = model->getOptionsModel()->data(
            model->getOptionsModel()->index(OptionsModel::Theme), Qt::EditRole).toString();
        return theme.isEmpty() || theme == QLatin1String("dark");
    }
    const QPalette palette = widget ? widget->palette() : QPalette{};
    return palette.color(QPalette::Window).lightness() < 128;
}

struct GuidedPaymasterSafetyLimits {
    qint64 per_transaction;
    qint64 reserved;
    qint64 per_hour;
    qint64 per_day;
    int completed_per_hour;
    int completed_per_day;
};

GuidedPaymasterSafetyLimits GuidedSafetyLimits(bool conservative,
                                               qint64 advertised_network_fee)
{
    const qint64 reserved = conservative ? 20000000 : 100000000;
    const qint64 per_hour = conservative ? 50000000 : 200000000;
    const qint64 per_day = conservative ? 200000000 : 1000000000;
    return {
        advertised_network_fee,
        std::max(reserved, advertised_network_fee),
        std::max(per_hour, advertised_network_fee),
        std::max(per_day, advertised_network_fee),
        conservative ? 5 : 10,
        conservative ? 25 : 100,
    };
}

bool GetInt64Field(const UniValue& object, const char* name, qint64& value)
{
    const UniValue& field = object.find_value(name);
    if (!field.isNum()) return false;
    try {
        value = field.getInt<qint64>();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool HasInt64Fields(const UniValue& object,
                    std::initializer_list<const char*> names)
{
    if (!object.isObject()) return false;
    return std::all_of(names.begin(), names.end(), [&object](const char* name) {
        qint64 value{0};
        return GetInt64Field(object, name, value);
    });
}

bool GetIntField(const UniValue& object, const char* name, int& value)
{
    qint64 parsed{0};
    if (!GetInt64Field(object, name, parsed) ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool HasIntFields(const UniValue& object,
                  std::initializer_list<const char*> names)
{
    if (!object.isObject()) return false;
    return std::all_of(names.begin(), names.end(), [&object](const char* name) {
        int value{0};
        return GetIntField(object, name, value);
    });
}

bool IsStringArray(const UniValue& value)
{
    if (!value.isArray() ||
        value.size() > MAX_PAYMASTER_RPC_ARRAY_ENTRIES) return false;
    for (const UniValue& entry : value.getValues()) {
        if (!entry.isStr() || entry.get_str().empty() ||
            entry.get_str().size() > MAX_PAYMASTER_RPC_TEXT_BYTES) {
            return false;
        }
    }
    return true;
}

bool IsEnumString(const UniValue& value,
                  std::initializer_list<const char*> allowed)
{
    if (!value.isStr() || value.get_str().empty() ||
        value.get_str().size() > MAX_PAYMASTER_RPC_TEXT_BYTES) {
        return false;
    }
    return std::any_of(allowed.begin(), allowed.end(),
                       [&value](const char* candidate) {
                           return value.get_str() == candidate;
                       });
}

bool IsBoundedOptionalString(const UniValue& value, size_t maximum,
                             bool allow_empty = false)
{
    return value.isNull() ||
        (value.isStr() && value.get_str().size() <= maximum &&
         (allow_empty || !value.get_str().empty()));
}

bool IsCompleteProviderPolicy(const UniValue& policy)
{
    const UniValue& models = policy.find_value("funding_models");
    if (!policy.isObject() || !IsStringArray(models) || models.empty() ||
        models.size() > 2 ||
        !IsEnumString(policy.find_value("sponsorship_scope"),
                      {"public", "restricted"})) {
        return false;
    }
    std::set<std::string> unique_models;
    for (const UniValue& model : models.getValues()) {
        if (!IsEnumString(model, {"sponsored", "user_paid"}) ||
            !unique_models.insert(model.get_str()).second) {
            return false;
        }
    }
    return HasIntFields(policy,
                     {"fee_rate_bps", "min_amount_cents",
                      "max_amount_cents", "quote_ttl"}) &&
        HasInt64Fields(policy,
                       {"maximum_network_fee_dgb_satoshis"});
}

bool IsCompleteFundingSafety(const UniValue& limits)
{
    return HasInt64Fields(
               limits,
               {"maximum_network_fee_per_transaction_satoshis",
                "maximum_reserved_network_fee_satoshis",
                "maximum_network_fee_per_hour_satoshis",
                "maximum_network_fee_per_day_satoshis"}) &&
        HasIntFields(limits,
                     {"maximum_completed_per_hour",
                      "maximum_completed_per_day"});
}

bool IsCompleteProviderSafetyPolicy(const UniValue& policy)
{
    return policy.isObject() &&
        IsCompleteFundingSafety(policy.find_value("user_paid")) &&
        IsCompleteFundingSafety(policy.find_value("public_sponsored")) &&
        IsCompleteFundingSafety(policy.find_value("restricted_sponsored")) &&
        HasIntFields(
            policy,
            {"maximum_active_quotes_total",
             "maximum_active_quotes_per_netgroup",
             "maximum_active_quotes_per_recipient",
             "maximum_quote_requests_per_netgroup_per_minute"});
}

bool IsCompleteLiquidityPolicy(const UniValue& policy)
{
    return policy.isObject() &&
        policy.find_value("automatic_replenishment").isBool() &&
        policy.find_value("paid_maintenance_approved").isBool() &&
        HasIntFields(
            policy,
            {"target_admission_dgb", "target_operational_dgb",
             "target_admission_carriers", "target_operational_carriers"}) &&
        HasInt64Fields(
            policy,
            {"maximum_maintenance_fee_per_transaction_satoshis",
             "maximum_maintenance_fee_per_hour_satoshis",
             "maximum_maintenance_fee_per_day_satoshis"});
}

bool IsHex256Field(const UniValue& object, const char* name);

bool IsCompleteLiquiditySlotStatus(const UniValue& status)
{
    return HasInt64Fields(
        status,
        {"target", "ready", "pending", "counted_toward_target",
         "missing"});
}

bool IsCompleteLiquidityStatus(const UniValue& result)
{
    return result.isObject() &&
        result.find_value("policy_configured").isBool() &&
        result.find_value("targets_satisfy_provider_policy").isBool() &&
        result.find_value("maintenance_state").isStr() &&
        !result.find_value("maintenance_state").get_str().empty() &&
        IsCompleteLiquidityPolicy(result.find_value("policy")) &&
        IsCompleteLiquiditySlotStatus(result.find_value("admission_dgb")) &&
        IsCompleteLiquiditySlotStatus(
            result.find_value("operational_dgb")) &&
        IsCompleteLiquiditySlotStatus(
            result.find_value("admission_carriers")) &&
        IsCompleteLiquiditySlotStatus(
            result.find_value("operational_carriers")) &&
        HasInt64Fields(
            result,
            {"maintenance_fee_reserved_satoshis",
             "maintenance_fee_spent_last_hour_satoshis",
             "maintenance_fee_spent_last_day_satoshis",
             "carrier_base_cents",
             "carrier_withdrawable_excess_cents"}) &&
        IsStringArray(result.find_value("readiness_errors"));
}

bool IsCompleteProviderPoolInfo(const UniValue& result)
{
    const UniValue& pool = result.find_value("pool");
    if (!result.isObject() || !pool.isArray() ||
        pool.size() > MAX_PAYMASTER_POOL_ENTRIES) return false;
    const std::set<std::string> purposes{"admission", "operational"};
    const std::set<std::string> assets{"dgb", "dd_carrier"};
    const std::set<std::string> states{
        "available", "reserved", "pending_successor", "spent",
        "committed", "released", "invalidated"};
    for (const UniValue& entry : pool.getValues()) {
        const UniValue& purpose = entry.find_value("purpose");
        const UniValue& asset = entry.find_value("asset");
        const UniValue& state = entry.find_value("state");
        const UniValue& reservation = entry.find_value("reservation_id");
        const UniValue& origin = entry.find_value("origin_commit_key");
        qint64 vout{0};
        qint64 dgb{0};
        qint64 dd{0};
        qint64 height{0};
        qint64 updated_at{0};
        if (!entry.isObject() || !IsHex256Field(entry, "txid") ||
            !purpose.isStr() || purposes.count(purpose.get_str()) == 0 ||
            !asset.isStr() || assets.count(asset.get_str()) == 0 ||
            !state.isStr() || states.count(state.get_str()) == 0 ||
            !GetInt64Field(entry, "vout", vout) || vout < 0 ||
            vout > std::numeric_limits<uint32_t>::max() ||
            !GetInt64Field(entry, "dgb_satoshis", dgb) || dgb < 0 ||
            !MoneyRange(dgb) || !GetInt64Field(entry, "dd_cents", dd) ||
            dd < 0 || dd > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
            !GetInt64Field(entry, "confirmation_height", height) ||
            height < 0 || !GetInt64Field(entry, "updated_at", updated_at) ||
            updated_at < 0 ||
            (!reservation.isNull() &&
             !IsHex256Field(entry, "reservation_id")) ||
            (!origin.isNull() && !IsHex256Field(entry, "origin_commit_key"))) {
            return false;
        }
        if ((asset.get_str() == "dgb" && (dgb <= 0 || dd != 0)) ||
            (asset.get_str() == "dd_carrier" && (dd <= 0 || dgb != 0))) {
            return false;
        }
    }
    return true;
}

bool SameNumericField(const UniValue& expected, const UniValue& actual,
                      const char* name)
{
    const UniValue& expected_value = expected.find_value(name);
    const UniValue& actual_value = actual.find_value(name);
    if (!expected_value.isNum() || !actual_value.isNum()) return false;
    try {
        return expected_value.getInt<int64_t>() ==
               actual_value.getInt<int64_t>();
    } catch (const std::exception&) {
        return false;
    }
}

bool SameNumericFields(const UniValue& expected, const UniValue& actual,
                       std::initializer_list<const char*> names)
{
    return std::all_of(names.begin(), names.end(),
                       [&expected, &actual](const char* name) {
                           return SameNumericField(expected, actual, name);
                       });
}

bool SameBooleanField(const UniValue& expected, const UniValue& actual,
                      const char* name)
{
    const UniValue& expected_value = expected.find_value(name);
    const UniValue& actual_value = actual.find_value(name);
    return expected_value.isBool() && actual_value.isBool() &&
           expected_value.get_bool() == actual_value.get_bool();
}

bool SameStringField(const UniValue& expected, const UniValue& actual,
                     const char* name)
{
    const UniValue& expected_value = expected.find_value(name);
    const UniValue& actual_value = actual.find_value(name);
    return expected_value.isStr() && actual_value.isStr() &&
           expected_value.get_str() == actual_value.get_str();
}

bool SameStringSet(const UniValue& expected, const UniValue& actual,
                   const char* name)
{
    const UniValue& expected_values = expected.find_value(name);
    const UniValue& actual_values = actual.find_value(name);
    if (!IsStringArray(expected_values) || !IsStringArray(actual_values) ||
        expected_values.size() != actual_values.size()) {
        return false;
    }
    std::set<std::string> expected_set;
    std::set<std::string> actual_set;
    for (const UniValue& value : expected_values.getValues()) {
        expected_set.insert(value.get_str());
    }
    for (const UniValue& value : actual_values.getValues()) {
        actual_set.insert(value.get_str());
    }
    return expected_set.size() == expected_values.size() &&
           actual_set.size() == actual_values.size() &&
           expected_set == actual_set;
}

bool IsNonNullHex256(const UniValue& value)
{
    if (!value.isStr() || value.get_str().size() != 64) return false;
    static const QRegularExpression hex256{
        QStringLiteral("^[0-9a-fA-F]{64}$")};
    return value.get_str().find_first_not_of('0') != std::string::npos &&
        hex256.match(QString::fromStdString(value.get_str())).hasMatch();
}

bool IsHex256Field(const UniValue& object, const char* name)
{
    return IsNonNullHex256(object.find_value(name));
}

bool IsExactProviderPolicyAcknowledgement(const UniValue& requested,
                                          const UniValue& persisted)
{
    return IsCompleteProviderPolicy(requested) &&
        IsCompleteProviderPolicy(persisted) &&
        IsHex256Field(persisted, "policy_hash") &&
        SameStringSet(requested, persisted, "funding_models") &&
        SameStringField(requested, persisted, "sponsorship_scope") &&
        SameNumericFields(
            requested, persisted,
            {"fee_rate_bps", "min_amount_cents", "max_amount_cents",
             "quote_ttl", "maximum_network_fee_dgb_satoshis"});
}

bool IsExactFundingSafetyAcknowledgement(const UniValue& requested,
                                         const UniValue& persisted)
{
    return IsCompleteFundingSafety(requested) &&
        IsCompleteFundingSafety(persisted) &&
        SameNumericFields(
            requested, persisted,
            {"maximum_network_fee_per_transaction_satoshis",
             "maximum_reserved_network_fee_satoshis",
             "maximum_network_fee_per_hour_satoshis",
             "maximum_network_fee_per_day_satoshis",
             "maximum_completed_per_hour", "maximum_completed_per_day"});
}

bool IsExactProviderSafetyAcknowledgement(const UniValue& requested,
                                          const UniValue& persisted)
{
    return IsCompleteProviderSafetyPolicy(requested) &&
        IsCompleteProviderSafetyPolicy(persisted) &&
        HasInt64Fields(persisted, {"updated_at"}) &&
        IsExactFundingSafetyAcknowledgement(
            requested.find_value("user_paid"),
            persisted.find_value("user_paid")) &&
        IsExactFundingSafetyAcknowledgement(
            requested.find_value("public_sponsored"),
            persisted.find_value("public_sponsored")) &&
        IsExactFundingSafetyAcknowledgement(
            requested.find_value("restricted_sponsored"),
            persisted.find_value("restricted_sponsored")) &&
        SameNumericFields(
            requested, persisted,
            {"maximum_active_quotes_total",
             "maximum_active_quotes_per_netgroup",
             "maximum_active_quotes_per_recipient",
             "maximum_quote_requests_per_netgroup_per_minute"});
}

bool IsExactLiquidityPolicyAcknowledgement(const UniValue& requested,
                                           const UniValue& persisted)
{
    return IsCompleteLiquidityPolicy(requested) &&
        IsCompleteLiquidityPolicy(persisted) &&
        HasInt64Fields(persisted, {"updated_at"}) &&
        SameBooleanField(requested, persisted, "automatic_replenishment") &&
        SameBooleanField(requested, persisted, "paid_maintenance_approved") &&
        SameNumericFields(
            requested, persisted,
            {"target_admission_dgb", "target_operational_dgb",
             "target_admission_carriers", "target_operational_carriers",
             "maximum_maintenance_fee_per_transaction_satoshis",
             "maximum_maintenance_fee_per_hour_satoshis",
             "maximum_maintenance_fee_per_day_satoshis"});
}

bool IsCompletePoolPreparationResult(const UniValue& result)
{
    return result.isObject() && result.find_value("executed").isBool() &&
        IsHex256Field(result, "plan_id") &&
        HasInt64Fields(
            result,
            {"admission_dgb_slots", "operational_dgb_slots",
             "admission_carrier_slots", "operational_carrier_slots",
             "missing_admission_dgb_slots",
             "missing_operational_dgb_slots",
             "missing_admission_carrier_slots",
             "missing_operational_carrier_slots",
             "admission_dgb_satoshis_each",
             "operational_dgb_satoshis_each", "carrier_cents_each",
             "total_output_satoshis", "total_carrier_cents"});
}

bool IsCompletePoolRebalanceResult(const UniValue& result)
{
    return result.isObject() && result.find_value("executed").isBool() &&
        IsHex256Field(result, "plan_id") &&
        HasInt64Fields(
            result,
            {"retired_admission_dgb_slots",
             "retired_operational_dgb_slots",
             "retired_admission_carrier_slots",
             "retired_operational_carrier_slots",
             "retired_dgb_satoshis", "retired_carrier_cents"});
}

bool IsCompleteProviderPool(const UniValue& pool)
{
    return HasIntFields(
        pool,
        {"entries", "reserved", "admission_dgb", "admission_carriers",
         "operational_dgb", "operational_carriers",
         "complete_operational_slots"});
}

bool IsCompleteProviderRuntimeStatus(const UniValue& result)
{
    const UniValue& operation_mode = result.find_value("operation_mode");
    const UniValue& service_state = result.find_value("service_state");
    const UniValue& last_service_error =
        result.find_value("last_service_error");
    const UniValue& service_queue = result.find_value("service_queue");
    qint64 waiting_requests{0};
    qint64 waiting_submits{0};
    if (!IsEnumString(operation_mode, {"automatic", "manual"}) ||
        !result.find_value("autostart").isBool() ||
        !IsEnumString(
            service_state,
            {"stopped", "waiting_for_unlock", "waiting_for_readiness",
             "waiting_for_maintenance_approval",
             "replenishing_liquidity",
             "waiting_for_liquidity_confirmation", "active", "manual",
             "drain_only", "error"}) ||
        !IsBoundedOptionalString(last_service_error,
                                 MAX_PAYMASTER_RPC_TEXT_BYTES,
                                 /*allow_empty=*/true) ||
        !GetInt64Field(service_queue, "waiting_requests",
                       waiting_requests) ||
        !GetInt64Field(service_queue, "waiting_submits",
                       waiting_submits) ||
        waiting_requests < 0 || waiting_submits < 0) {
        return false;
    }
    return true;
}

bool IsCompleteProviderInfoSnapshot(const UniValue& result)
{
    const auto required_bool = [&result](const char* name) {
        return result.find_value(name).isBool();
    };
    const UniValue& provider_id = result.find_value("provider_id");
    const UniValue& identity_key = result.find_value("identity_key");
    const UniValue& endpoint = result.find_value("endpoint");
    const UniValue& display_name = result.find_value("display_name");
    const UniValue& policy = result.find_value("policy");
    const UniValue& pool = result.find_value("pool");
    const UniValue& readiness_errors = result.find_value("readiness_errors");
    const UniValue& backup = result.find_value("backup_status");
    const UniValue& finance = result.find_value("finance_summary");

    if (!result.isObject() || !required_bool("settings_present") ||
        !required_bool("wallet_eligible") || !required_bool("enabled") ||
        !required_bool("running") || !required_bool("ready") ||
        !required_bool("wallet_locked") || !required_bool("pool_ready") ||
        !IsCompleteProviderRuntimeStatus(result) ||
        !IsStringArray(readiness_errors) ||
        (!provider_id.isNull() && !IsHex256Field(result, "provider_id")) ||
        (!identity_key.isNull() && !IsHex256Field(result, "identity_key")) ||
        !IsBoundedOptionalString(endpoint,
                                 MAX_PAYMASTER_RPC_TEXT_BYTES) ||
        !IsBoundedOptionalString(display_name, 32,
                                 /*allow_empty=*/true) ||
        (!policy.isNull() && !IsCompleteProviderPolicy(policy)) ||
        !IsCompleteProviderPool(pool)) {
        return false;
    }
    if (!backup.isNull() &&
        (!backup.isObject() ||
         !backup.find_value("required").isBool() ||
         !HasInt64Fields(
             backup,
             {"reminder_updated_at", "last_successful_backup_at",
              "external_backup_acknowledged_at"}))) {
        return false;
    }
    if (!finance.isNull() &&
        (!finance.isObject() ||
         !finance.find_value("history_partially_reconstructable").isBool() ||
         !HasInt64Fields(
             finance,
             {"service_fee_income_cents", "dgb_operating_cost_satoshis",
              "successful_transfers"}))) {
        return false;
    }
    return !result.find_value("running").get_bool() ||
        (result.find_value("enabled").get_bool() && provider_id.isStr());
}

bool IsCompleteBackupAcknowledgement(const UniValue& result)
{
    qint64 acknowledged_at{0};
    return result.isObject() &&
        result.find_value("acknowledged").isBool() &&
        result.find_value("acknowledged").get_bool() &&
        result.find_value("backup_required").isBool() &&
        GetInt64Field(result, "acknowledged_at", acknowledged_at) &&
        acknowledged_at >= 0;
}

bool IsCompleteFinanceSummary(const UniValue& summary)
{
    qint64 income{0};
    qint64 cost{0};
    qint64 transfers{0};
    return GetInt64Field(summary, "service_fee_income_cents", income) &&
        GetInt64Field(summary, "dgb_operating_cost_satoshis", cost) &&
        GetInt64Field(summary, "successful_transfers", transfers) &&
        income >= 0 && cost >= 0 && transfers >= 0;
}

bool IsCompleteFinanceStatus(const UniValue& result,
                             const QString& expected_period)
{
    const UniValue& period = result.find_value("period");
    const UniValue& model_breakdown = result.find_value("model_breakdown");
    const UniValue& period_summaries = result.find_value("period_summaries");
    const UniValue& pool = result.find_value("pool_capital");
    const UniValue& daily_totals = result.find_value("daily_totals");
    const UniValue& events = result.find_value("events");
    if (!result.isObject() || !IsHex256Field(result, "provider_id") ||
        !period.isStr() ||
        QString::fromStdString(period.get_str()) != expected_period ||
        !HasInt64Fields(
            result,
            {"service_fee_income_cents", "dgb_operating_cost_satoshis",
             "successful_transfers", "average_service_fee_cents",
             "user_paid_transfers", "public_sponsored_transfers",
             "restricted_sponsored_transfers", "history_complete_from",
             "last_successful_backup_at",
             "external_backup_acknowledged_at"}) ||
        !result.find_value("history_partially_reconstructable").isBool() ||
        !result.find_value("backup_required").isBool() ||
        !model_breakdown.isObject() || !period_summaries.isObject() ||
        !HasInt64Fields(
            pool,
            {"dgb_available_satoshis", "dgb_reserved_satoshis",
             "dgb_pending_satoshis", "carrier_base_cents",
             "carrier_earned_cents", "carrier_withdrawable_cents",
             "pending_maintenance_transactions"}) ||
        !daily_totals.isArray() ||
        daily_totals.size() > MAX_PAYMASTER_FINANCE_DAYS ||
        !events.isArray() ||
        events.size() > MAX_PAYMASTER_FINANCE_PAGE_EVENTS) {
        return false;
    }
    for (const char* key : {"user_paid", "public_sponsored",
                            "restricted_sponsored"}) {
        if (!IsCompleteFinanceSummary(model_breakdown.find_value(key))) {
            return false;
        }
    }
    for (const char* key : {"today", "7d", "30d", "all"}) {
        if (!IsCompleteFinanceSummary(period_summaries.find_value(key))) {
            return false;
        }
    }
    for (const UniValue& total : daily_totals.getValues()) {
        qint64 day_start{0};
        qint64 income{0};
        qint64 cost{0};
        qint64 transfers{0};
        qint64 maintenance{0};
        if (!GetInt64Field(total, "day_start", day_start) ||
            !GetInt64Field(total, "service_fee_income_cents", income) ||
            !GetInt64Field(total, "dgb_operating_cost_satoshis", cost) ||
            !GetInt64Field(total, "successful_transfers", transfers) ||
            !GetInt64Field(total, "maintenance_transactions", maintenance) ||
            day_start < 0 || income < 0 || cost < 0 || transfers < 0 ||
            maintenance < 0 || !HasInt64Fields(
                total,
                {"day_start", "service_fee_income_cents",
                 "dgb_operating_cost_satoshis", "successful_transfers",
                 "maintenance_transactions"})) {
            return false;
        }
    }
    for (const UniValue& event : events.getValues()) {
        const UniValue& kind = event.find_value("kind");
        const UniValue& state = event.find_value("state");
        const UniValue& funding_model = event.find_value("funding_model");
        const UniValue& sponsorship_scope =
            event.find_value("sponsorship_scope");
        const UniValue& confirmed_at = event.find_value("confirmed_at");
        qint64 dd_income{0};
        qint64 dgb_cost{0};
        qint64 created_at{0};
        qint64 confirmed_time{0};
        if (!event.isObject() || !IsHex256Field(event, "event_id") ||
            !IsHex256Field(event, "transaction_id") ||
            !IsEnumString(kind, {"transfer", "setup", "replenishment",
                                 "retirement", "withdrawal"}) ||
            !IsEnumString(state,
                          {"pending", "confirmed", "invalidated"}) ||
            (!funding_model.isNull() &&
             !IsEnumString(funding_model,
                           {"user_paid", "sponsored"})) ||
            (!sponsorship_scope.isNull() &&
             !IsEnumString(sponsorship_scope,
                           {"public", "restricted"})) ||
            !GetInt64Field(event, "dd_income_cents", dd_income) ||
            !GetInt64Field(event, "dgb_cost_satoshis", dgb_cost) ||
            !GetInt64Field(event, "created_at", created_at) ||
            dd_income < 0 || dgb_cost < 0 || created_at <= 0 ||
            (!confirmed_at.isNull() &&
             (!GetInt64Field(event, "confirmed_at", confirmed_time) ||
              confirmed_time <= 0))) {
            return false;
        }
        const bool transfer = kind.get_str() == "transfer";
        const bool sponsored = transfer && funding_model.isStr() &&
            funding_model.get_str() == "sponsored";
        if ((transfer && !funding_model.isStr()) ||
            (!transfer && (!funding_model.isNull() ||
                           !sponsorship_scope.isNull())) ||
            (sponsored && !sponsorship_scope.isStr()) ||
            (transfer && !sponsored && !sponsorship_scope.isNull()) ||
            ((state.get_str() == "confirmed") !=
             confirmed_at.isNum())) {
            return false;
        }
    }

    const UniValue& oracle = result.find_value("oracle_price_micro_usd");
    const UniValue& valuation_time = result.find_value("valuation_time");
    const UniValue& estimate = result.find_value("estimated_result_usd");
    const bool any_valuation =
        !oracle.isNull() || !valuation_time.isNull() || !estimate.isNull();
    if (any_valuation) {
        qint64 oracle_value{0};
        qint64 valuation_time_value{0};
        if (!GetInt64Field(result, "oracle_price_micro_usd", oracle_value) ||
            !GetInt64Field(result, "valuation_time", valuation_time_value) ||
            !estimate.isNum()) {
            return false;
        }
    }
    const UniValue& next_cursor = result.find_value("next_cursor");
    return next_cursor.isNull() || IsNonNullHex256(next_cursor);
}

bool IsCompleteCarrierWithdrawalResult(const UniValue& result,
                                       const QString& expected_mode,
                                       bool expected_execution,
                                       const QString& expected_plan)
{
    const UniValue& executed = result.find_value("executed");
    const UniValue& mode = result.find_value("mode");
    if (!result.isObject() || !executed.isBool() ||
        executed.get_bool() != expected_execution || !mode.isStr() ||
        QString::fromStdString(mode.get_str()) != expected_mode ||
        !IsHex256Field(result, "plan_id")) {
        return false;
    }
    const QString plan = QString::fromStdString(
        result.find_value("plan_id").get_str());
    if (expected_execution && plan != expected_plan) return false;

    qint64 value{0};
    if (!expected_execution) {
        if (!GetInt64Field(result, "source_carriers", value) || value <= 0 ||
            !GetInt64Field(result, "expires_at", value) || value <= 0) {
            return false;
        }
    }
    if (expected_mode == QLatin1String("all_excess")) {
        qint64 withdrawable{0};
        qint64 fee{0};
        if (!GetInt64Field(result, "withdrawable_excess_cents",
                           withdrawable) ||
            !GetInt64Field(result, "estimated_network_fee_satoshis", fee) ||
            withdrawable <= 0 || fee <= 0 ||
            (expected_execution && !IsHex256Field(result, "txid"))) {
            return false;
        }
        const UniValue& retained = result.find_value("retained_carrier_cents");
        if (!retained.isNull() &&
            (!GetInt64Field(result, "retained_carrier_cents", value) ||
             value < 0)) {
            return false;
        }
        return true;
    }
    return expected_mode == QLatin1String("release_slot") &&
        GetInt64Field(result, "operational_carrier_target", value) &&
        value >= 0;
}

bool IsCompleteProviderStartResult(const UniValue& result)
{
    const UniValue& mode = result.find_value("operation_mode");
    const UniValue& state = result.find_value("service_state");
    return result.isObject() && result.find_value("running").isBool() &&
        result.find_value("ready").isBool() && mode.isStr() &&
        (mode.get_str() == "automatic" || mode.get_str() == "manual") &&
        IsEnumString(
            state,
            {"stopped", "waiting_for_unlock", "waiting_for_readiness",
             "waiting_for_maintenance_approval",
             "replenishing_liquidity",
             "waiting_for_liquidity_confirmation", "active", "manual",
             "drain_only", "error"});
}

bool IsCompleteClientSafetyPolicy(const UniValue& policy)
{
    return HasInt64Fields(
        policy,
        {"maximum_service_fee_per_transaction_cents",
         "maximum_service_fee_per_day_cents"});
}

bool IsCompleteClientSafetyStatus(const UniValue& result)
{
    const UniValue& configured = result.find_value("configured");
    const UniValue& policy = result.find_value("policy");
    if (!result.isObject() || !configured.isBool() ||
        (!policy.isNull() && !IsCompleteClientSafetyPolicy(policy))) {
        return false;
    }
    return !configured.get_bool() ||
        (IsCompleteClientSafetyPolicy(policy) &&
         HasInt64Fields(
             result,
             {"active_reservations", "reserved_service_fee_cents",
              "spent_service_fee_last_day_cents",
              "available_service_fee_today_cents"}));
}

QString PaymasterWizardStyleSheet(bool dark_theme)
{
    if (dark_theme) {
        return QStringLiteral(
            "QWizard#PaymasterSetupWizard,"
            "QWizard#PaymasterSetupWizard QWizardPage {"
            "  background-color: #0b2419;"
            "  color: #ffffff;"
            "}"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupRequirementsScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupRequirementsContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupSafetyScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupSafetyContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupLiquidityScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupLiquidityContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupReviewScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupReviewContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupProgressScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupProgressContent {"
            "  background-color: #0b2419;"
            "  color: #ffffff;"
            "  border: none;"
            "}"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupWalletCard {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-radius: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel {"
            "  background-color: transparent;"
            "  color: #ffffff;"
            "}"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupFieldHelp,"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupLiquidityFieldHelp {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-left: 5px solid #42d884;"
            "  border-radius: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpTitle,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpTitle {"
            "  color: #ffffff;"
            "  font-size: 11pt;"
            "  font-weight: bold;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpText,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpText {"
            "  color: #c6ddcf;"
            "  font-size: 10pt;"
            "  font-weight: normal;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityPreviewDetails,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupProgressResult {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-radius: 5px;"
            "  color: #ffffff;"
            "  padding: 10px;"
            "}"
            "QWizard#PaymasterSetupWizard QLineEdit,"
            "QWizard#PaymasterSetupWizard QSpinBox,"
            "QWizard#PaymasterSetupWizard QDoubleSpinBox,"
            "QWizard#PaymasterSetupWizard QComboBox {"
            "  background-color: #113a29;"
            "  color: #ffffff;"
            "  border: 2px solid #42d884;"
            "  border-radius: 4px;"
            "  padding: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator {"
            "  width: 16px;"
            "  height: 16px;"
            "  background-color: #09261b;"
            "  border: 2px solid #42d884;"
            "  border-radius: 3px;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator:checked {"
            "  background-color: #1f9d57;"
            "  border-color: #74e5a7;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator:unchecked:hover {"
            "  background-color: #164934;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-button {"
            "  subcontrol-origin: border;"
            "  subcontrol-position: top right;"
            "  width: 24px;"
            "  background-color: #dff5e7;"
            "  border-left: 1px solid #42d884;"
            "  border-bottom: 1px solid #8bc9a6;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-button {"
            "  subcontrol-origin: border;"
            "  subcontrol-position: bottom right;"
            "  width: 24px;"
            "  background-color: #dff5e7;"
            "  border-left: 1px solid #42d884;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-arrow {"
            "  image: url(:/icons/spin_up);"
            "  width: 9px;"
            "  height: 6px;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-arrow {"
            "  image: url(:/icons/spin_down);"
            "  width: 9px;"
            "  height: 6px;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton {"
            "  background-color: #16804f;"
            "  color: #ffffff;"
            "  border: 2px solid #16804f;"
            "  border-radius: 4px;"
            "  padding: 7px 14px;"
            "  font-weight: bold;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:hover {"
            "  background-color: #11683e;"
            "  border-color: #42d884;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:focus,"
            "QWizard#PaymasterSetupWizard QLineEdit:focus,"
            "QWizard#PaymasterSetupWizard QSpinBox:focus,"
            "QWizard#PaymasterSetupWizard QDoubleSpinBox:focus,"
            "QWizard#PaymasterSetupWizard QComboBox:focus {"
            "  border-color: #f4d35e;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:disabled {"
            "  background-color: #294b3b;"
            "  border-color: #294b3b;"
            "  color: #9ab5a7;"
            "}");
    }
    return QStringLiteral(
        "QWizard#PaymasterSetupWizard,"
        "QWizard#PaymasterSetupWizard QWizardPage {"
        "  background-color: #eef9f2;"
        "  color: #123f2b;"
        "}"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupRequirementsScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupRequirementsContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupSafetyScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupSafetyContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupLiquidityScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupLiquidityContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupReviewScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupReviewContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupProgressScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupProgressContent {"
        "  background-color: #eef9f2;"
        "  color: #123f2b;"
        "  border: none;"
        "}"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupWalletCard {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-radius: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel {"
        "  background-color: transparent;"
        "  color: #123f2b;"
        "}"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupFieldHelp,"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupLiquidityFieldHelp {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-left: 5px solid #1f9d57;"
        "  border-radius: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpTitle,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpTitle {"
        "  color: #123f2b;"
        "  font-size: 11pt;"
        "  font-weight: bold;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpText,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpText {"
        "  color: #416553;"
        "  font-size: 10pt;"
        "  font-weight: normal;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityPreviewDetails,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupProgressResult {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-radius: 5px;"
        "  color: #123f2b;"
        "  padding: 10px;"
        "}"
        "QWizard#PaymasterSetupWizard QLineEdit,"
        "QWizard#PaymasterSetupWizard QSpinBox,"
        "QWizard#PaymasterSetupWizard QDoubleSpinBox,"
        "QWizard#PaymasterSetupWizard QComboBox {"
        "  background-color: #ffffff;"
        "  color: #123f2b;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator {"
        "  width: 16px;"
        "  height: 16px;"
        "  background-color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 3px;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator:checked {"
        "  background-color: #1f9d57;"
        "  border-color: #146c3a;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator:unchecked:hover {"
        "  background-color: #e8f7ed;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-button {"
        "  subcontrol-origin: border;"
        "  subcontrol-position: top right;"
        "  width: 24px;"
        "  background-color: #e8f7ed;"
        "  border-left: 1px solid #1f9d57;"
        "  border-bottom: 1px solid #9bcbb0;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-button {"
        "  subcontrol-origin: border;"
        "  subcontrol-position: bottom right;"
        "  width: 24px;"
        "  background-color: #e8f7ed;"
        "  border-left: 1px solid #1f9d57;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-arrow {"
        "  image: url(:/icons/spin_up);"
        "  width: 9px;"
        "  height: 6px;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-arrow {"
        "  image: url(:/icons/spin_down);"
        "  width: 9px;"
        "  height: 6px;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton {"
        "  background-color: #146c3a;"
        "  color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 7px 14px;"
        "  font-weight: bold;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:hover {"
        "  background-color: #0f5a30;"
        "  border-color: #0f5a30;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:focus,"
        "QWizard#PaymasterSetupWizard QLineEdit:focus,"
        "QWizard#PaymasterSetupWizard QSpinBox:focus,"
        "QWizard#PaymasterSetupWizard QDoubleSpinBox:focus,"
        "QWizard#PaymasterSetupWizard QComboBox:focus {"
        "  border-color: #8a5a00;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:disabled {"
        "  background-color: #b9d5c3;"
        "  border-color: #b9d5c3;"
        "  color: #60776a;"
        "}");
}
} // namespace

/** Wallet-scoped Paymaster operator console. Protocol and persistence remain
 * in Core; this widget is only an asynchronous presentation layer. */
class PaymasterWidgetImpl final : public DigiDollarPaymasterWidget
{
    struct FundingSafetyControls {
        QLineEdit* per_transaction{nullptr};
        QLineEdit* reserved{nullptr};
        QLineEdit* per_hour{nullptr};
        QLineEdit* per_day{nullptr};
        QSpinBox* completed_per_hour{nullptr};
        QSpinBox* completed_per_day{nullptr};
        QLabel* mode{nullptr};
    };

    struct FundingSafetyValues {
        qint64 per_transaction{0};
        qint64 reserved{0};
        qint64 per_hour{0};
        qint64 per_day{0};
        int completed_per_hour{0};
        int completed_per_day{0};

        bool allZero() const
        {
            return per_transaction == 0 && reserved == 0 && per_hour == 0 &&
                   per_day == 0 && completed_per_hour == 0 &&
                   completed_per_day == 0;
        }

        bool hasZero() const
        {
            return per_transaction == 0 || reserved == 0 || per_hour == 0 ||
                   per_day == 0 || completed_per_hour == 0 ||
                   completed_per_day == 0;
        }

    };

    struct LiquidityPolicyValues {
        bool automatic_replenishment{true};
        bool paid_maintenance_approved{false};
        int admission_dgb{3};
        int operational_dgb{1};
        int admission_carriers{0};
        int operational_carriers{0};
        qint64 fee_per_transaction{0};
        qint64 fee_per_hour{0};
        qint64 fee_per_day{0};
    };

public:
    explicit PaymasterWidgetImpl(QWidget* parent = nullptr) : DigiDollarPaymasterWidget(parent)
    {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(16, 14, 16, 16);
        outer->setSpacing(12);

        auto* operator_header = new QFrame(this);
        operator_header->setObjectName("paymasterOperatorHeader");
        operator_header->setProperty("paymasterRole", QStringLiteral("operatorHeader"));
        auto* operator_header_layout = new QVBoxLayout(operator_header);
        operator_header_layout->setContentsMargins(18, 14, 18, 14);
        operator_header_layout->setSpacing(4);
        auto* heading = new QLabel(tr("Paymaster provider"), operator_header);
        heading->setObjectName("paymasterOperatorHeading");
        heading->setProperty("paymasterRole", QStringLiteral("operatorHeading"));
        QFont heading_font = heading->font();
        heading_font.setPointSize(18);
        heading_font.setBold(true);
        heading->setFont(heading_font);
        operator_header_layout->addWidget(heading);

        m_wallet = new QLabel(tr("No wallet selected"), operator_header);
        m_wallet->setObjectName("paymasterSelectedWallet");
        m_wallet->setProperty("paymasterRole", QStringLiteral("walletHeading"));
        m_status = new QLabel(tr("Loading provider status…"), operator_header);
        m_status->setObjectName("paymasterProviderStatus");
        m_status->setProperty("paymasterRole", QStringLiteral("globalStatus"));
        m_status->setWordWrap(true);
        operator_header_layout->addWidget(m_wallet);
        operator_header_layout->addWidget(m_status);
        outer->addWidget(operator_header);

        m_tabs = new QTabWidget(this);
        m_tabs->setObjectName("paymasterOperatorTabs");
        outer->addWidget(m_tabs);

        auto* overview_scroll = new QScrollArea(m_tabs);
        overview_scroll->setObjectName("paymasterOverviewPage");
        overview_scroll->setWidgetResizable(true);
        auto* overview = new QWidget(overview_scroll);
        overview->setObjectName("paymasterOverviewContents");
        QVBoxLayout* overview_layout{nullptr};
        auto* overview_column = CreatePaymasterPageColumn(overview, overview_layout);

        m_setup_choice = new QGroupBox(tr("Set up this Paymaster wallet"), overview);
        m_setup_choice->setObjectName("paymasterSetupChoice");
        auto* setup_choice_layout = new QVBoxLayout(m_setup_choice);
        auto* setup_choice_intro = new QLabel(tr(
            "This is the first time Paymaster operator controls have been opened for this wallet. "
            "Choose how you want to configure the provider. No provider setting is changed until "
            "you explicitly save it, and no funds move without a separate transaction preview and confirmation."),
            m_setup_choice);
        setup_choice_intro->setObjectName("paymasterSetupChoiceIntroduction");
        setup_choice_intro->setWordWrap(true);
        setup_choice_layout->addWidget(setup_choice_intro);

        auto* guided_group = new QGroupBox(tr("Guided setup — recommended"), m_setup_choice);
        auto* guided_layout = new QVBoxLayout(guided_group);
        auto* guided_help = new QLabel(tr(
            "Explains each decision, checks the current provider prerequisites and prepares "
            "recommended policy, safety and liquidity values for your review."), guided_group);
        guided_help->setWordWrap(true);
        m_guided_setup = new QPushButton(tr("Start guided setup…"), guided_group);
        m_guided_setup->setObjectName("paymasterChooseGuidedSetup");
        m_guided_setup->setAccessibleDescription(tr(
            "Open the recommended step-by-step Paymaster provider setup assistant."));
        guided_layout->addWidget(guided_help);
        guided_layout->addWidget(m_guided_setup, 0, Qt::AlignLeft);
        setup_choice_layout->addWidget(guided_group);

        auto* expert_group = new QGroupBox(tr("Manual setup — experts"), m_setup_choice);
        auto* expert_layout = new QVBoxLayout(expert_group);
        auto* expert_help = new QLabel(tr(
            "Unlock all configuration pages immediately. Choose this only if you already understand "
            "provider identities, funding models, finite safety budgets and pool preparation."), expert_group);
        expert_help->setWordWrap(true);
        m_expert_setup = new QPushButton(tr("Use manual expert setup"), expert_group);
        m_expert_setup->setObjectName("paymasterChooseExpertSetup");
        m_expert_setup->setAccessibleDescription(tr(
            "Unlock the advanced Paymaster configuration pages without running the assistant."));
        expert_layout->addWidget(expert_help);
        expert_layout->addWidget(m_expert_setup, 0, Qt::AlignLeft);
        setup_choice_layout->addWidget(expert_group);
        setup_choice_layout->addStretch();
        overview_layout->addWidget(m_setup_choice);

        m_setup_content = new QWidget(overview);
        m_setup_content->setObjectName("paymasterConfiguredOverview");
        auto* configured_layout = new QVBoxLayout(m_setup_content);
        configured_layout->setContentsMargins(0, 0, 0, 0);

        auto* introduction = new QGroupBox(tr("Your provider at a glance"), overview_column);
        introduction->setObjectName("paymasterOverviewIntroduction");
        introduction->setProperty("paymasterRole", QStringLiteral("heroCard"));
        auto* introduction_layout = new QVBoxLayout(introduction);
        auto* introduction_text = new QLabel(tr(
            "Help other wallets send DigiDollar when they do not hold DGB. "
            "This wallet supplies the network fee within the limits shown below."), introduction);
        introduction_text->setObjectName("paymasterIntroduction");
        introduction_text->setWordWrap(true);
        introduction_layout->addWidget(introduction_text);
        auto* responsibility = new QLabel(tr(
            "Core validates every payment locally. Your configured spending limits remain the final financial safeguard."), introduction);
        responsibility->setObjectName("paymasterResponsibilityNotice");
        responsibility->setProperty("paymasterRole", QStringLiteral("mutedText"));
        responsibility->setWordWrap(true);
        introduction_layout->addWidget(responsibility);
        m_reopen_wizard = new QPushButton(tr("Review setup with assistant…"), introduction);
        m_reopen_wizard->setObjectName("paymasterSetupWizard");
        m_reopen_wizard->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        introduction_layout->addWidget(m_reopen_wizard, 0, Qt::AlignLeft);
        configured_layout->addWidget(introduction);

        m_overview_backup_notice = new QGroupBox(
            tr("Protect this provider identity"), overview_column);
        m_overview_backup_notice->setObjectName(
            "paymasterOverviewBackupNotice");
        m_overview_backup_notice->setProperty(
            "paymasterRole", QStringLiteral("notice"));
        m_overview_backup_notice->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* overview_backup_layout = new QVBoxLayout(
            m_overview_backup_notice);
        auto* overview_backup_text = new QLabel(tr(
            "This provider identity, its private key, configuration, pool state and finance history are stored in this wallet. "
            "Create a new full-wallet backup and keep it offline and confidential. A seed or descriptor export alone is not a complete Paymaster backup."),
            m_overview_backup_notice);
        overview_backup_text->setWordWrap(true);
        overview_backup_layout->addWidget(overview_backup_text);
        m_overview_backup_provider_id = new QLabel(
            m_overview_backup_notice);
        m_overview_backup_provider_id->setObjectName(
            "paymasterOverviewBackupProviderId");
        m_overview_backup_provider_id->setWordWrap(true);
        m_overview_backup_provider_id->setTextInteractionFlags(
            Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        overview_backup_layout->addWidget(m_overview_backup_provider_id);
        auto* overview_backup_actions = new QHBoxLayout();
        m_overview_backup_now = new QPushButton(
            tr("Back up provider wallet now…"), m_overview_backup_notice);
        m_overview_backup_now->setObjectName("paymasterOverviewBackupNow");
        m_overview_backup_now->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_overview_backup_external = new QPushButton(
            tr("I use another full-wallet backup method…"),
            m_overview_backup_notice);
        m_overview_backup_external->setObjectName(
            "paymasterOverviewExternalBackup");
        overview_backup_actions->addWidget(m_overview_backup_now);
        overview_backup_actions->addWidget(m_overview_backup_external);
        overview_backup_actions->addStretch();
        overview_backup_layout->addLayout(overview_backup_actions);
        m_overview_backup_notice->hide();
        configured_layout->addWidget(m_overview_backup_notice);

        m_external_prerequisites_card = new QGroupBox(
            tr("External prerequisites"), overview_column);
        m_external_prerequisites_card->setObjectName("paymasterOverviewNextStep");
        m_external_prerequisites_card->setProperty(
            "paymasterRole", QStringLiteral("nextStepCard"));
        m_external_prerequisites_card->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* readiness_layout = new QVBoxLayout(m_external_prerequisites_card);
        m_next_step = new QLabel(tr("Checking what is required next…"),
                                 m_external_prerequisites_card);
        m_next_step->setObjectName("paymasterNextStep");
        m_next_step->setProperty("paymasterRole", QStringLiteral("statusText"));
        m_next_step->setWordWrap(true);
        QFont next_step_font = m_next_step->font();
        next_step_font.setBold(true);
        m_next_step->setFont(next_step_font);
        m_readiness_summary = new QLabel(m_external_prerequisites_card);
        m_readiness_summary->setObjectName("paymasterReadinessSummary");
        m_readiness_summary->setWordWrap(true);
        readiness_layout->addWidget(m_next_step);
        readiness_layout->addWidget(m_readiness_summary);
        auto* prerequisite_grid = new QGridLayout();
        prerequisite_grid->setColumnStretch(1, 1);
        const auto add_prerequisite = [this, prerequisite_grid](
                                          int row, const QString& title,
                                          QLabel*& value,
                                          const QString& object_name) {
            auto* heading = new QLabel(title, m_external_prerequisites_card);
            heading->setProperty("paymasterRole", QStringLiteral("mutedText"));
            value = new QLabel(m_external_prerequisites_card);
            value->setObjectName(object_name);
            value->setWordWrap(true);
            prerequisite_grid->addWidget(heading, row, 0, Qt::AlignTop);
            prerequisite_grid->addWidget(value, row, 1);
        };
        add_prerequisite(0, tr("Blockchain synchronization"),
                         m_external_blockchain_status,
                         QStringLiteral("paymasterExternalBlockchainStatus"));
        add_prerequisite(1, tr("Transaction index"),
                         m_external_txindex_status,
                         QStringLiteral("paymasterExternalTxIndexStatus"));
        add_prerequisite(2, tr("Transactions ready"),
                         m_external_broadcast_status,
                         QStringLiteral("paymasterExternalBroadcastStatus"));
        add_prerequisite(3, tr("DigiDollar activation"),
                         m_external_activation_status,
                         QStringLiteral("paymasterExternalActivationStatus"));
        add_prerequisite(4, tr("Oracle price"), m_external_oracle_status,
                         QStringLiteral("paymasterExternalOracleStatus"));
        m_external_other_heading = new QLabel(
            tr("Additional provider requirement"),
            m_external_prerequisites_card);
        m_external_other_heading->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        m_external_other_status = new QLabel(m_external_prerequisites_card);
        m_external_other_status->setObjectName(
            "paymasterExternalOtherStatus");
        m_external_other_status->setWordWrap(true);
        prerequisite_grid->addWidget(m_external_other_heading, 5, 0,
                                     Qt::AlignTop);
        prerequisite_grid->addWidget(m_external_other_status, 5, 1);
        m_external_other_heading->hide();
        m_external_other_status->hide();
        readiness_layout->addLayout(prerequisite_grid);
        m_external_oracle_note = new QLabel(tr(
            "Informational: Paymaster transfers do not require an Oracle price. "
            "Minting, redemption and DigiDollar system displays remain limited until one is available."),
            m_external_prerequisites_card);
        m_external_oracle_note->setObjectName("paymasterExternalOracleNote");
        m_external_oracle_note->setProperty("paymasterRole", QStringLiteral("mutedText"));
        m_external_oracle_note->setWordWrap(true);
        readiness_layout->addWidget(m_external_oracle_note);
        configured_layout->addWidget(m_external_prerequisites_card);

        auto* dashboard = new PaymasterResponsiveCards(overview_column);
        dashboard->setObjectName("paymasterOverviewDashboard");
        auto* offer_card = CreatePaymasterStatusCard(
            dashboard, tr("Offer"), m_overview_offer_status,
            m_overview_offer_action);
        offer_card->setObjectName("paymasterOverviewOfferCard");
        auto* safety_card = CreatePaymasterStatusCard(
            dashboard, tr("Spending limits"), m_overview_safety_status,
            m_overview_safety_action);
        safety_card->setObjectName("paymasterOverviewSafetyCard");
        auto* liquidity_card = CreatePaymasterStatusCard(
            dashboard, tr("Liquidity"), m_overview_liquidity_status,
            m_overview_liquidity_action);
        liquidity_card->setObjectName("paymasterOverviewLiquidityCard");
        auto* operation_card = CreatePaymasterStatusCard(
            dashboard, tr("Provider operation"), m_overview_operation_status,
            m_overview_operation_action);
        operation_card->setObjectName("paymasterOverviewOperationCard");
        auto* finance_card = CreatePaymasterStatusCard(
            dashboard, tr("Finances"), m_overview_finance_status,
            m_overview_finance_action);
        finance_card->setObjectName("paymasterOverviewFinanceCard");
        m_overview_liquidity_status->setObjectName(
            "paymasterOverviewLiquidityStatus");
        m_overview_operation_status->setObjectName(
            "paymasterOverviewOperationStatus");
        m_overview_liquidity_action->setObjectName(
            "paymasterOverviewLiquidityAction");
        m_overview_operation_action->setObjectName(
            "paymasterOverviewOperationAction");
        dashboard->addCard(offer_card);
        dashboard->addCard(safety_card);
        dashboard->addCard(liquidity_card);
        dashboard->addCard(finance_card);
        dashboard->addCard(operation_card);
        configured_layout->addWidget(dashboard);

        m_liquidity_maintenance_card = new QGroupBox(
            tr("Automatic liquidity maintenance"), overview_column);
        m_liquidity_maintenance_card->setObjectName(
            "paymasterLiquidityMaintenanceCard");
        m_liquidity_maintenance_card->setProperty(
            "paymasterRole", QStringLiteral("maintenanceCard"));
        auto* maintenance_layout = new QVBoxLayout(
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_state = new QLabel(
            tr("Liquidity maintenance status has not been loaded yet."),
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_state->setObjectName(
            "paymasterLiquidityMaintenanceState");
        m_liquidity_maintenance_state->setProperty(
            "paymasterRole", QStringLiteral("statusText"));
        m_liquidity_maintenance_state->setWordWrap(true);
        QFont maintenance_state_font = m_liquidity_maintenance_state->font();
        maintenance_state_font.setBold(true);
        m_liquidity_maintenance_state->setFont(maintenance_state_font);
        m_liquidity_maintenance_next_step = new QLabel(
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_next_step->setObjectName(
            "paymasterLiquidityMaintenanceNextStep");
        m_liquidity_maintenance_next_step->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        m_liquidity_maintenance_next_step->setWordWrap(true);
        m_liquidity_maintenance_cost = new QLabel(
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_cost->setObjectName(
            "paymasterLiquidityMaintenanceCost");
        m_liquidity_maintenance_cost->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        m_liquidity_maintenance_cost->setWordWrap(true);
        m_approve_liquidity_maintenance = new QPushButton(
            tr("Review and approve maintenance limits…"),
            m_liquidity_maintenance_card);
        m_approve_liquidity_maintenance->setObjectName(
            "paymasterApproveLiquidityMaintenance");
        m_approve_liquidity_maintenance->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        maintenance_layout->addWidget(m_liquidity_maintenance_state);
        maintenance_layout->addWidget(m_liquidity_maintenance_next_step);
        maintenance_layout->addWidget(m_liquidity_maintenance_cost);
        maintenance_layout->addWidget(m_approve_liquidity_maintenance,
                                      0, Qt::AlignLeft);
        configured_layout->addWidget(m_liquidity_maintenance_card);

        auto* details_toggle = new QPushButton(tr("Show technical details"), overview);
        details_toggle->setObjectName("paymasterTechnicalDetailsToggle");
        details_toggle->setCheckable(true);
        details_toggle->setFlat(true);
        configured_layout->addWidget(details_toggle, 0, Qt::AlignLeft);

        auto* details = new QWidget(overview);
        details->setObjectName("paymasterTechnicalDetails");
        auto* details_layout = new QVBoxLayout(details);
        details_layout->setContentsMargins(0, 0, 0, 0);
        m_identity = new QLabel(tr("Identity: not configured"), overview);
        m_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_pool = new QLabel(tr("Liquidity pool: not configured"), overview);
        m_readiness = new QPlainTextEdit(overview);
        m_readiness->setObjectName("paymasterReadinessCodes");
        m_readiness->setReadOnly(true);
        m_readiness->setMaximumHeight(130);
        m_readiness->setPlaceholderText(tr("No technical readiness information available"));
        details_layout->addWidget(m_identity);
        details_layout->addWidget(m_pool);
        details_layout->addWidget(m_readiness);
        details->setVisible(false);
        configured_layout->addWidget(details);

        auto* runtime_buttons = new QHBoxLayout();
        auto* refresh = new QPushButton(tr("Refresh"), overview);
        refresh->setObjectName("paymasterOverviewRefresh");
        m_start = new QPushButton(tr("Start provider"), overview);
        m_start->setObjectName("paymasterStartProvider");
        m_stop = new QPushButton(tr("Stop provider"), overview);
        m_stop->setObjectName("paymasterStopProvider");
        m_start->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        m_stop->setProperty("paymasterRole", QStringLiteral("dangerAction"));
        runtime_buttons->addWidget(refresh);
        runtime_buttons->addStretch();
        runtime_buttons->addWidget(m_start);
        runtime_buttons->addWidget(m_stop);
        configured_layout->addLayout(runtime_buttons);
        configured_layout->addStretch();
        overview_layout->addWidget(m_setup_content);
        overview_scroll->setWidget(overview);
        ConfigureScrollArea(overview_scroll, overview);
        m_tabs->addTab(overview_scroll, tr("Overview"));

        auto* configuration_scroll = new QScrollArea(m_tabs);
        configuration_scroll->setObjectName("paymasterConfigurationPage");
        configuration_scroll->setWidgetResizable(true);
        auto* configuration = new QWidget(configuration_scroll);
        configuration->setObjectName("paymasterConfigurationContents");
        QVBoxLayout* configuration_layout{nullptr};
        auto* configuration_column = CreatePaymasterPageColumn(
            configuration, configuration_layout);
        AddPaymasterPageHeading(
            configuration_layout, configuration_column, tr("Offer and identity"),
            tr("Choose what this provider offers. These settings do not move funds; liquidity is managed separately."),
            QStringLiteral("paymasterConfiguration"));

        auto* identity_group = new QGroupBox(tr("Provider identity"), configuration_column);
        identity_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* identity_form = new QFormLayout(identity_group);
        identity_form->setHorizontalSpacing(18);
        identity_form->setVerticalSpacing(10);
        identity_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto* identity_help = new QLabel(tr(
            "The identity lets clients verify that offers come from the same provider. It is stored in this wallet and should normally be created only once."), identity_group);
        identity_help->setWordWrap(true);
        identity_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        m_display_name = new QLineEdit(identity_group);
        m_display_name->setObjectName("paymasterDisplayName");
        m_display_name->setMaxLength(32);
        m_display_name->setValidator(
            new PaymasterDisplayNameValidator(m_display_name));
        m_display_name->setPlaceholderText(tr("Name shown to clients"));
        m_create_identity = new QPushButton(tr("Create provider identity"), identity_group);
        auto* create_identity = m_create_identity;
        create_identity->setObjectName("paymasterCreateIdentity");
        create_identity->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        m_offer_identity_status = new QLabel(
            tr("Action required · no provider identity has been created yet."),
            identity_group);
        m_offer_identity_status->setObjectName("paymasterOfferIdentityStatus");
        m_offer_identity_status->setProperty("paymasterRole", QStringLiteral("statusText"));
        m_offer_identity_status->setWordWrap(true);
        m_offer_identity_id = new QLabel(tr("Not created yet"), identity_group);
        m_offer_identity_id->setObjectName("paymasterOfferIdentityId");
        m_offer_identity_id->setProperty("paymasterRole", QStringLiteral("mutedText"));
        m_offer_identity_id->setWordWrap(true);
        m_offer_identity_id->setTextInteractionFlags(
            Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_offer_identity_id->setAccessibleName(tr("Full provider identity"));
        identity_form->addRow(identity_help);
        identity_form->addRow(tr("Status:"), m_offer_identity_status);
        identity_form->addRow(tr("Provider ID:"), m_offer_identity_id);
        identity_form->addRow(tr("Display name:"), m_display_name);
        identity_form->addRow(create_identity);
        configuration_layout->addWidget(identity_group);

        auto* policy_group = new QGroupBox(tr("Services offered"), configuration_column);
        policy_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* policy_form = new QFormLayout(policy_group);
        policy_form->setHorizontalSpacing(18);
        policy_form->setVerticalSpacing(10);
        policy_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto* policy_help = new QLabel(tr(
            "The policy defines which transfers you are willing to support. Clients still verify every quote and transaction locally."), policy_group);
        policy_help->setWordWrap(true);
        policy_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        policy_form->addRow(policy_help);
        m_sponsored = new QCheckBox(tr("Sponsored · provider pays the DGB fee"), policy_group);
        m_sponsored->setObjectName("paymasterPolicySponsored");
        m_sponsored->setProperty("paymasterRole", QStringLiteral("choiceCard"));
        m_user_paid = new QCheckBox(tr("User paid · client pays a DD service fee"), policy_group);
        m_user_paid->setObjectName("paymasterPolicyUserPaid");
        m_user_paid->setProperty("paymasterRole", QStringLiteral("choiceCard"));
        m_user_paid->setChecked(true);
        auto* user_paid_help = new QLabel(tr(
            "The client pays a service fee in DigiDollar; your provider wallet supplies the DGB network fee."), policy_group);
        user_paid_help->setWordWrap(true);
        user_paid_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        auto* sponsored_help = new QLabel(tr(
            "Your provider pays the DGB network fee without charging a DigiDollar service fee. Use strict safety budgets to cap the cost."), policy_group);
        sponsored_help->setWordWrap(true);
        sponsored_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        auto* models = new PaymasterResponsiveCards(policy_group);
        models->setObjectName("paymasterOfferModelCards");
        auto* user_paid_card = new QFrame(models);
        user_paid_card->setObjectName("paymasterUserPaidOfferCard");
        user_paid_card->setProperty("paymasterRole", QStringLiteral("choicePanel"));
        auto* user_paid_layout = new QVBoxLayout(user_paid_card);
        user_paid_layout->setContentsMargins(14, 12, 14, 12);
        user_paid_layout->addWidget(m_user_paid);
        user_paid_layout->addWidget(user_paid_help);
        user_paid_layout->addStretch();
        auto* sponsored_card = new QFrame(models);
        sponsored_card->setObjectName("paymasterSponsoredOfferCard");
        sponsored_card->setProperty("paymasterRole", QStringLiteral("choicePanel"));
        auto* sponsored_layout = new QVBoxLayout(sponsored_card);
        sponsored_layout->setContentsMargins(14, 12, 14, 12);
        sponsored_layout->addWidget(m_sponsored);
        sponsored_layout->addWidget(sponsored_help);
        sponsored_layout->addStretch();
        models->addCard(user_paid_card);
        models->addCard(sponsored_card);
        const auto update_model_card = [](QFrame* card, bool selected) {
            card->setProperty("selected", selected);
            if (card->style()) {
                card->style()->unpolish(card);
                card->style()->polish(card);
            }
        };
        connect(m_user_paid, &QCheckBox::toggled, user_paid_card,
                [user_paid_card, update_model_card](bool checked) {
                    update_model_card(user_paid_card, checked);
                });
        connect(m_sponsored, &QCheckBox::toggled, sponsored_card,
                [sponsored_card, update_model_card](bool checked) {
                    update_model_card(sponsored_card, checked);
                });
        update_model_card(user_paid_card, m_user_paid->isChecked());
        update_model_card(sponsored_card, m_sponsored->isChecked());
        m_scope = new NoWheelComboBox(policy_group);
        m_scope->setObjectName("paymasterPolicySponsorshipScope");
        m_scope->addItem(tr("Public — available to discovered clients"), QStringLiteral("public"));
        m_scope->addItem(tr("Restricted — invitation only"), QStringLiteral("restricted"));
        m_scope->setToolTip(tr(
            "Public sponsorship can be offered together with user-paid service. Restricted sponsorship is a sponsored-only invitation mode."));
        m_fee_bps = scaledSpin(policy_group, 0, 10000, 50, 100.0, 2);
        m_fee_bps->setObjectName("paymasterPolicyFeeBps");
        m_fee_bps->setSingleStep(10);
        m_fee_bps->setSuffix(tr(" %"));
        m_fee_bps->setToolTip(tr(
            "Service fee for user-paid transfers. 100 basis points equal 1%. Sponsored transfers always charge zero DigiDollar service fee."));
        m_min_amount = scaledSpin(policy_group, 100, 10000000, 100, 100.0, 2);
        m_min_amount->setObjectName("paymasterPolicyMinimumCents");
        m_min_amount->setSuffix(tr(" DD"));
        m_min_amount->setToolTip(tr("Smallest DigiDollar payment this provider will accept."));
        m_max_amount = scaledSpin(policy_group, 100, 10000000, 100000, 100.0, 2);
        m_max_amount->setObjectName("paymasterPolicyMaximumCents");
        m_max_amount->setSuffix(tr(" DD"));
        m_max_amount->setToolTip(tr("Largest DigiDollar payment this provider will accept."));
        m_quote_ttl = spin(policy_group, 1, 60, 60);
        m_quote_ttl->setObjectName("paymasterPolicyQuoteLifetime");
        m_quote_ttl->setSuffix(tr(" seconds"));
        m_quote_ttl->setToolTip(tr("How long a client may accept a quote before it expires."));
        m_network_fee = scaledSpin(policy_group, 1, 2000000000, 20000000,
                                   100000000.0, 8);
        m_network_fee->setObjectName("paymasterPolicyMaximumNetworkFee");
        m_network_fee->setSuffix(tr(" DGB"));
        m_network_fee->setToolTip(tr(
            "Absolute DGB network-fee ceiling for one Paymaster transaction. Guided setup keeps the active per-transaction safety value aligned with this ceiling and uses the other safety fields for aggregate spam protection."));
        policy_form->addRow(tr("Payment models:"), models);
        m_funding_model_status = new QLabel(policy_group);
        m_funding_model_status->setObjectName("paymasterFundingModelExplanation");
        m_funding_model_status->setWordWrap(true);
        policy_form->addRow(QString(), m_funding_model_status);
        policy_form->addRow(tr("Who may use sponsorship:"), m_scope);
        policy_form->addRow(tr("User-paid service fee:"), m_fee_bps);
        policy_form->addRow(tr("Smallest payment:"), m_min_amount);
        policy_form->addRow(tr("Largest payment:"), m_max_amount);
        policy_form->addRow(tr("Quote validity:"), m_quote_ttl);
        policy_form->addRow(tr("Network-fee ceiling per transfer:"), m_network_fee);
        auto* limits_help = new QLabel(tr(
            "The payment range filters requests before a quote is created. Quote validity limits how long resources remain offered to one client. The network-fee ceiling is the absolute per-transfer guard; guided safety profiles keep their per-transfer value aligned with it and limit repeated requests through aggregate budgets and counters."), policy_group);
        limits_help->setObjectName("paymasterPolicyLimitsExplanation");
        limits_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        limits_help->setWordWrap(true);
        policy_form->addRow(QString(), limits_help);
        m_policy_summary = new QLabel(policy_group);
        m_policy_summary->setObjectName("paymasterPolicySummary");
        m_policy_summary->setProperty("paymasterRole", QStringLiteral("summaryText"));
        m_policy_summary->setWordWrap(true);
        policy_form->addRow(tr("Policy summary:"), m_policy_summary);
        m_save_policy = new QPushButton(tr("Save policy"), policy_group);
        m_save_policy->setObjectName("savePaymasterPolicy");
        m_save_policy->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        m_enable = new QPushButton(tr("Enable provider configuration"), policy_group);
        m_enable->setObjectName("paymasterEnableProvider");
        m_enable->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        m_restore_policy_defaults = new QPushButton(
            tr("Restore recommended defaults"), policy_group);
        m_restore_policy_defaults->setObjectName("paymasterRestorePolicyDefaults");
        m_restore_policy_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        m_restore_policy_defaults->setToolTip(tr(
            "Reset only the unsaved policy form. This does not change the provider identity, safety limits or liquidity."));
        auto* policy_buttons = new QHBoxLayout();
        policy_buttons->addWidget(m_restore_policy_defaults);
        policy_buttons->addStretch();
        policy_buttons->addWidget(m_save_policy);
        policy_buttons->addWidget(m_enable);
        policy_form->addRow(policy_buttons);
        m_enable_status = new QLabel(
            tr("Provider configuration is currently disabled for this wallet."),
            policy_group);
        m_enable_status->setObjectName("paymasterEnableStatus");
        m_enable_status->setWordWrap(true);
        policy_form->addRow(QString(), m_enable_status);
        configuration_layout->addWidget(policy_group);
        configuration_layout->addStretch();
        configuration_scroll->setWidget(configuration);
        ConfigureScrollArea(configuration_scroll, configuration);
        m_configuration_page = configuration_scroll;
        m_tabs->addTab(configuration_scroll, tr("Offer"));

        auto* safety_scroll = new QScrollArea(m_tabs);
        safety_scroll->setObjectName("paymasterSafetyPolicyPage");
        safety_scroll->setWidgetResizable(true);
        auto* safety = new QWidget(safety_scroll);
        safety->setObjectName("paymasterSafetyPolicyContents");
        QVBoxLayout* safety_layout{nullptr};
        auto* safety_column = CreatePaymasterPageColumn(safety, safety_layout);
        AddPaymasterPageHeading(
            safety_layout, safety_column, tr("Provider spending limits"),
            tr("Set the maximum DGB this provider may spend. These wallet-local limits are the final brake even when a remote client sends repeated requests."),
            QStringLiteral("paymasterSafetyPolicy"));
        auto* safety_warning = new QLabel(tr(
            "Final spending brake · every active model needs finite private wallet limits per transfer, hour and day."), safety_column);
        safety_warning->setObjectName("paymasterSafetyPolicyWarning");
        safety_warning->setProperty("paymasterRole", QStringLiteral("notice"));
        safety_warning->setWordWrap(true);
        safety_layout->addWidget(safety_warning);
        auto* safety_instructions = new QLabel(tr(
            "Configure the models you actually offer, then save the provider safety policy. All six limits for an active model must be finite and greater than zero. Setting all six values to zero safely disables an unused model; zero never means unlimited. Start with the recommended values and lower them if the potential DGB cost is too high."), safety);
        safety_instructions->setObjectName("paymasterSafetyPolicyInstructions");
        safety_instructions->setWordWrap(true);
        safety_layout->addWidget(safety_instructions);

        auto* provider_safety = new QGroupBox(tr("Budgets by payment model"), safety_column);
        provider_safety->setProperty("paymasterRole", QStringLiteral("card"));
        auto* provider_safety_layout = new QVBoxLayout(provider_safety);
        m_provider_safety_status = new QLabel(
            tr("Provider safety policy: not configured (provider not ready)"),
            provider_safety);
        m_provider_safety_status->setObjectName("paymasterProviderSafetyStatus");
        m_provider_safety_status->setWordWrap(true);
        provider_safety_layout->addWidget(m_provider_safety_status);
        auto* provider_limits_help = new QLabel(tr(
            "Each tab controls one payment model. Per-transaction limits stop one costly transfer; reserved limits cap simultaneous open quotes; hourly and daily limits cap cumulative spending and completed transfers."), provider_safety);
        provider_limits_help->setObjectName("paymasterProviderSafetyExplanation");
        provider_limits_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        provider_limits_help->setWordWrap(true);
        provider_safety_layout->addWidget(provider_limits_help);
        auto* funding_tabs = new QTabWidget(provider_safety);
        funding_tabs->setObjectName("paymasterFundingSafetyModels");
        m_user_paid_safety = createFundingSafetyControls(
            funding_tabs, tr("User paid"), QStringLiteral("paymasterSafetyUserPaid"),
            /*recommended_defaults=*/true);
        m_public_sponsored_safety = createFundingSafetyControls(
            funding_tabs, tr("Public sponsored"), QStringLiteral("paymasterSafetyPublicSponsored"),
            /*recommended_defaults=*/false);
        m_restricted_sponsored_safety = createFundingSafetyControls(
            funding_tabs, tr("Restricted sponsored"), QStringLiteral("paymasterSafetyRestrictedSponsored"),
            /*recommended_defaults=*/false);
        provider_safety_layout->addWidget(funding_tabs);

        auto* quote_limits = new QGroupBox(tr("Quote and request limits"), provider_safety);
        auto* quote_form = new QFormLayout(quote_limits);
        auto* quote_help = new QLabel(tr(
            "These limits reduce resource exhaustion. Total limits protect the wallet, netgroup limits restrict groups of related network addresses, and recipient buckets stop repeated quotes aimed at the same destination. Reaching a limit rejects new work but does not invalidate an already authorized transfer."), quote_limits);
        quote_help->setObjectName("paymasterQuoteLimitsExplanation");
        quote_help->setWordWrap(true);
        quote_form->addRow(quote_help);
        m_max_active_quotes_total = spin(quote_limits, 1, 8192, 16);
        m_max_active_quotes_total->setObjectName("paymasterSafetyMaxActiveQuotesTotal");
        m_max_active_quotes_per_netgroup = spin(quote_limits, 1, 8192, 4);
        m_max_active_quotes_per_netgroup->setObjectName("paymasterSafetyMaxActiveQuotesPerNetgroup");
        m_max_active_quotes_per_recipient = spin(quote_limits, 1, 8192, 2);
        m_max_active_quotes_per_recipient->setObjectName("paymasterSafetyMaxActiveQuotesPerRecipient");
        m_max_quote_requests_per_netgroup = spin(quote_limits, 1, 8192, 10);
        m_max_quote_requests_per_netgroup->setObjectName("paymasterSafetyMaxQuoteRequestsPerNetgroupMinute");
        quote_form->addRow(tr("Active quotes (total):"), m_max_active_quotes_total);
        quote_form->addRow(tr("Active quotes per netgroup:"), m_max_active_quotes_per_netgroup);
        quote_form->addRow(tr("Active quotes per recipient bucket:"), m_max_active_quotes_per_recipient);
        quote_form->addRow(tr("Quote requests per netgroup/minute:"), m_max_quote_requests_per_netgroup);
        auto* restore_quote_defaults = new QPushButton(
            tr("Restore recommended quote limits"), quote_limits);
        restore_quote_defaults->setObjectName("paymasterRestoreQuoteSafetyDefaults");
        restore_quote_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        quote_form->addRow(restore_quote_defaults);
        // Technical anti-abuse limits are available through the advanced
        // disclosure below; the normal view stays focused on financial risk.

        m_provider_safety_group = provider_safety;
        m_save_provider_safety = new QPushButton(tr("Save provider safety policy"), provider_safety);
        m_save_provider_safety->setObjectName("savePaymasterProviderSafetyPolicy");
        m_save_provider_safety->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        provider_safety_layout->addWidget(m_save_provider_safety);
        auto* usage_group = new QGroupBox(tr("Current budget usage"), provider_safety);
        usage_group->setProperty("paymasterRole", QStringLiteral("technicalCard"));
        auto* usage_layout = new QVBoxLayout(usage_group);
        auto* usage_help = new QLabel(tr(
            "This read-only status shows what is currently reserved or spent. Saving new limits does not reset existing accounting."), usage_group);
        usage_help->setWordWrap(true);
        usage_layout->addWidget(usage_help);
        m_provider_safety_usage = new QLabel(usage_group);
        m_provider_safety_usage->setObjectName("paymasterProviderSafetyUsage");
        m_provider_safety_usage->setWordWrap(true);
        m_provider_safety_usage->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_provider_safety_usage->setText(tr("Budget usage is not available yet."));
        usage_layout->addWidget(m_provider_safety_usage);
        safety_layout->addWidget(provider_safety);

        auto* advanced_safety = new QWidget(safety_column);
        advanced_safety->setObjectName("paymasterAdvancedSafetyPanel");
        auto* advanced_safety_layout = new QVBoxLayout(advanced_safety);
        advanced_safety_layout->setContentsMargins(0, 0, 0, 0);
        advanced_safety_layout->addWidget(safety_instructions);
        advanced_safety_layout->addWidget(usage_group);
        advanced_safety_layout->addWidget(quote_limits);
        AddPaymasterDisclosure(
            safety_layout, safety_column, advanced_safety,
            tr("Show advanced protection limits"),
            tr("Hide advanced protection limits"),
            QStringLiteral("paymasterAdvancedSafetyToggle"));

        auto* client_safety = new QGroupBox(tr("Client service-fee limits"), safety);
        m_client_safety_group = client_safety;
        client_safety->setObjectName("paymasterClientSafetyGroup");
        auto* client_safety_form = new QFormLayout(client_safety);
        auto* client_safety_help = new QLabel(tr(
            "These limits protect this wallet when it uses another Paymaster. They cap the DigiDollar service fee for one transfer and across a rolling day. The effective per-transfer limit is always the lowest applicable wallet, request and signed-offer limit."), client_safety);
        client_safety_help->setObjectName("paymasterClientSafetyExplanation");
        client_safety_help->setWordWrap(true);
        client_safety_form->addRow(client_safety_help);
        m_client_fee_per_transaction = spin(client_safety, 0, 10000000, 100);
        m_client_fee_per_transaction->setObjectName("paymasterClientSafetyMaxFeePerTransaction");
        m_client_fee_per_day = spin(client_safety, 0, 10000000, 1000);
        m_client_fee_per_day->setObjectName("paymasterClientSafetyMaxFeePerDay");
        m_client_safety_status = new QLabel(
            tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"),
            client_safety);
        m_client_safety_status->setObjectName("paymasterClientSafetyStatus");
        m_client_safety_status->setWordWrap(true);
        m_client_safety_mode = new QLabel(client_safety);
        m_client_safety_mode->setObjectName("paymasterClientSafetyMode");
        m_client_safety_mode->setWordWrap(true);
        m_save_client_safety = new QPushButton(tr("Save client safety policy"), client_safety);
        m_save_client_safety->setObjectName("savePaymasterClientSafetyPolicy");
        auto* restore_client_defaults = new QPushButton(
            tr("Restore recommended client limits"), client_safety);
        restore_client_defaults->setObjectName("paymasterRestoreClientSafetyDefaults");
        client_safety_form->addRow(tr("Maximum service fee per transfer (cents):"), m_client_fee_per_transaction);
        client_safety_form->addRow(tr("Maximum service fees per rolling day (cents):"), m_client_fee_per_day);
        client_safety_form->addRow(m_client_safety_mode);
        client_safety_form->addRow(m_client_safety_status);
        auto* client_buttons = new QHBoxLayout();
        client_buttons->addWidget(restore_client_defaults);
        client_buttons->addStretch();
        client_buttons->addWidget(m_save_client_safety);
        client_safety_form->addRow(client_buttons);
        // Client-side DD fee protection belongs to Send $DD. Keep these
        // widgets alive for compatibility with the existing wallet status
        // refresh path, but never expose them in the provider console.
        client_safety->setVisible(false);
        safety_layout->addStretch();
        safety_scroll->setWidget(safety);
        ConfigureScrollArea(safety_scroll, safety);
        m_safety_page = safety_scroll;
        m_tabs->addTab(safety_scroll, tr("Spending limits"));

        auto* liquidity_scroll = new QScrollArea(m_tabs);
        liquidity_scroll->setObjectName("paymasterLiquidityPage");
        liquidity_scroll->setWidgetResizable(true);
        auto* liquidity = new QWidget(liquidity_scroll);
        liquidity->setObjectName("paymasterLiquidityContents");
        QVBoxLayout* liquidity_layout{nullptr};
        auto* liquidity_column = CreatePaymasterPageColumn(liquidity, liquidity_layout);
        AddPaymasterPageHeading(
            liquidity_layout, liquidity_column, tr("Provider liquidity"),
            tr("These wallet-owned reserves let clients verify this provider and let Core pay the network fee without selecting ordinary wallet funds for every request."),
            QStringLiteral("paymasterLiquidity"));

        auto* capacity_cards = new PaymasterResponsiveCards(liquidity_column);
        capacity_cards->setObjectName("paymasterLiquidityCapacityCards");
        capacity_cards->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto* dgb_capacity_card = CreatePaymasterStatusCard(
            capacity_cards, tr("DGB network-fee reserve"),
            m_liquidity_dgb_capacity_status, m_liquidity_dgb_capacity_action);
        auto* carrier_capacity_card = CreatePaymasterStatusCard(
            capacity_cards, tr("DigiDollar service-fee reserve"),
            m_liquidity_carrier_capacity_status,
            m_liquidity_carrier_capacity_action);
        // Both old Review buttons opened the same advanced panel and implied
        // two separate workflows. Capacity is one pool-management task, so the
        // normal view has one clearly named entry point instead.
        m_liquidity_dgb_capacity_action->hide();
        m_liquidity_carrier_capacity_action->hide();
        dgb_capacity_card->setMinimumHeight(0);
        carrier_capacity_card->setMinimumHeight(0);
        dgb_capacity_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        carrier_capacity_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        capacity_cards->addCard(dgb_capacity_card);
        capacity_cards->addCard(carrier_capacity_card);
        liquidity_layout->addWidget(capacity_cards);

        auto* capacity_explanation = new QLabel(tr(
            "Why two counts? Before making an offer, Core proves to a client that this provider has dedicated funds. Separate payment capacity is then kept ready for accepted transfers. “Ready” is available now; “target” is the amount Core maintains. These funds always remain under this wallet's control."),
            liquidity_column);
        capacity_explanation->setObjectName("paymasterLiquidityCapacityExplanation");
        capacity_explanation->setProperty("paymasterRole", QStringLiteral("mutedText"));
        capacity_explanation->setWordWrap(true);
        liquidity_layout->addWidget(capacity_explanation);
        auto* liquidity_steps = new QLabel(tr(
            "First-time setup: 1. Restore or choose target slot counts. 2. Preview pool preparation; no funds move during a preview. 3. Review the exact DGB and DD amounts below. 4. Execute the reviewed preparation. 5. Wait for the new outputs to confirm, then check Overview for readiness."), liquidity);
        liquidity_steps->setObjectName("paymasterLiquiditySetupSteps");
        liquidity_steps->setWordWrap(true);
        liquidity_layout->addWidget(liquidity_steps);

        auto* automatic_maintenance = new QGroupBox(
            tr("Keep reserves ready automatically"), liquidity_column);
        automatic_maintenance->setProperty("paymasterRole", QStringLiteral("card"));
        automatic_maintenance->setObjectName(
            "paymasterAutomaticLiquidityPolicy");
        auto* automatic_maintenance_layout = new QFormLayout(
            automatic_maintenance);
        auto* automatic_maintenance_help = new QLabel(tr(
            "After a completed transfer, Core first reuses the wallet-owned DGB change and DigiDollar carrier. "
            "If that is not enough, Core may create a refill transaction only when you have explicitly approved finite fee limits below."),
            automatic_maintenance);
        automatic_maintenance_help->setWordWrap(true);
        automatic_maintenance_layout->addRow(automatic_maintenance_help);
        m_automatic_replenishment = new QCheckBox(
            tr("Automatically refill missing reserves while this provider is running"),
            automatic_maintenance);
        m_automatic_replenishment->setObjectName(
            "paymasterAutomaticReplenishment");
        m_automatic_replenishment->setChecked(true);
        m_paid_maintenance_approved = new QCheckBox(tr(
            "Permit paid maintenance within the finite limits below"),
            automatic_maintenance);
        m_paid_maintenance_approved->setObjectName(
            "paymasterPaidMaintenanceApproved");
        m_maintenance_fee_per_transaction = integerField(
            automatic_maintenance, 10000000);
        m_maintenance_fee_per_transaction->setObjectName(
            "paymasterMaintenanceFeePerTransaction");
        m_maintenance_fee_per_hour = integerField(
            automatic_maintenance, 50000000);
        m_maintenance_fee_per_hour->setObjectName(
            "paymasterMaintenanceFeePerHour");
        m_maintenance_fee_per_day = integerField(
            automatic_maintenance, 200000000);
        m_maintenance_fee_per_day->setObjectName(
            "paymasterMaintenanceFeePerDay");
        m_liquidity_policy_status = new QLabel(tr(
            "Liquidity policy has not been loaded yet."), automatic_maintenance);
        m_liquidity_policy_status->setObjectName(
            "paymasterLiquidityPolicyStatus");
        m_liquidity_policy_status->setWordWrap(true);
        automatic_maintenance_layout->addRow(m_automatic_replenishment);
        automatic_maintenance_layout->addRow(m_liquidity_policy_status);
        m_maintenance_limits_toggle = new QPushButton(
            tr("Set optional paid-refill limits"), automatic_maintenance);
        m_maintenance_limits_toggle->setObjectName(
            "paymasterMaintenanceLimitsToggle");
        m_maintenance_limits_toggle->setProperty(
            "paymasterRole", QStringLiteral("disclosure"));
        m_maintenance_limits_toggle->setCheckable(true);
        auto* maintenance_limits_panel = new QWidget(automatic_maintenance);
        maintenance_limits_panel->setObjectName("paymasterMaintenanceLimitsPanel");
        auto* maintenance_limits_form = new QFormLayout(maintenance_limits_panel);
        maintenance_limits_form->setContentsMargins(0, 6, 0, 0);
        maintenance_limits_form->addRow(m_paid_maintenance_approved);
        maintenance_limits_form->addRow(
            tr("Maximum fee per transaction (satoshis):"),
            m_maintenance_fee_per_transaction);
        maintenance_limits_form->addRow(
            tr("Maximum fees per rolling hour (satoshis):"),
            m_maintenance_fee_per_hour);
        maintenance_limits_form->addRow(
            tr("Maximum fees per rolling day (satoshis):"),
            m_maintenance_fee_per_day);
        maintenance_limits_panel->setVisible(false);
        automatic_maintenance_layout->addRow(m_maintenance_limits_toggle);
        automatic_maintenance_layout->addRow(maintenance_limits_panel);
        m_save_liquidity_policy_primary = new QPushButton(
            tr("Save liquidity settings"), automatic_maintenance);
        m_save_liquidity_policy_primary->setObjectName(
            "paymasterSaveLiquidityPolicyPrimary");
        m_save_liquidity_policy_primary->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_save_liquidity_policy_primary->setToolTip(tr(
            "Save the automatic-refill setting, finite maintenance limits and displayed pool targets. This does not create a transaction or start the provider."));
        automatic_maintenance_layout->addRow(m_save_liquidity_policy_primary);
        connect(m_maintenance_limits_toggle, &QPushButton::toggled,
                maintenance_limits_panel,
                [this, maintenance_limits_panel](bool visible) {
                    maintenance_limits_panel->setVisible(visible);
                    m_maintenance_limits_toggle->setText(
                        visible ? tr("Hide paid-refill limits")
                                : tr("Set optional paid-refill limits"));
                });
        liquidity_layout->addWidget(automatic_maintenance);

        auto* targets = new QGroupBox(tr("Pool targets"), liquidity);
        auto* targets_layout = new QVBoxLayout(targets);
        auto* pool_explanation = new QLabel(tr(
            "Admission slots are separate, confirmed outputs used to demonstrate provider capacity. Operational slots are the outputs reserved for real transfers. DGB slots fund network fees. DD carrier slots are required only for user-paid service because they carry the provider's DigiDollar fee flow; sponsored-only providers can leave both carrier targets at zero."), targets);
        pool_explanation->setObjectName("paymasterLiquiditySlotExplanation");
        pool_explanation->setWordWrap(true);
        targets_layout->addWidget(pool_explanation);
        m_liquidity_current_status = new QLabel(
            tr("Current confirmed pool: status not loaded yet."), targets);
        m_liquidity_current_status->setObjectName("paymasterLiquidityCurrentStatus");
        m_liquidity_current_status->setWordWrap(true);
        targets_layout->addWidget(m_liquidity_current_status);
        auto* pool_form = new QFormLayout();
        m_admission_dgb = spin(liquidity, 3, 16, 3);
        m_admission_dgb->setObjectName("paymasterAdmissionDgbSlots");
        m_operational_dgb = spin(liquidity, 1, 16, 1);
        m_operational_dgb->setObjectName("paymasterOperationalDgbSlots");
        m_admission_carriers = spin(liquidity, 0, 16, 3);
        m_admission_carriers->setObjectName("paymasterAdmissionCarrierSlots");
        m_operational_carriers = spin(liquidity, 0, 16, 1);
        m_operational_carriers->setObjectName("paymasterOperationalCarrierSlots");
        m_admission_dgb->setToolTip(tr("At least three confirmed admission DGB slots are required."));
        m_operational_dgb->setToolTip(tr("Each operational DGB slot can support one concurrent transfer."));
        m_admission_carriers->setToolTip(tr("User-paid providers require at least three admission DD carriers; sponsored-only providers need none."));
        m_operational_carriers->setToolTip(tr("User-paid transfers require an operational DD carrier paired with operational DGB liquidity."));
        pool_form->addRow(tr("Admission DGB slots:"), m_admission_dgb);
        pool_form->addRow(tr("Operational DGB slots:"), m_operational_dgb);
        pool_form->addRow(tr("Admission carrier slots:"), m_admission_carriers);
        pool_form->addRow(tr("Operational carrier slots:"), m_operational_carriers);
        targets_layout->addLayout(pool_form);
        m_liquidity_summary = new QLabel(targets);
        m_liquidity_summary->setObjectName("paymasterLiquidityTargetSummary");
        m_liquidity_summary->setWordWrap(true);
        targets_layout->addWidget(m_liquidity_summary);
        m_liquidity_target_save_status = new QLabel(targets);
        m_liquidity_target_save_status->setObjectName(
            "paymasterLiquidityTargetSaveStatus");
        m_liquidity_target_save_status->setProperty(
            "paymasterRole", QStringLiteral("statusText"));
        m_liquidity_target_save_status->setWordWrap(true);
        targets_layout->addWidget(m_liquidity_target_save_status);
        m_save_liquidity_policy = new QPushButton(
            tr("Save liquidity targets and refill policy"), targets);
        m_save_liquidity_policy->setObjectName(
            "paymasterSaveLiquidityPolicy");
        m_save_liquidity_policy->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_save_liquidity_policy->setToolTip(tr(
            "Save the displayed slot targets, automatic-refill setting and finite maintenance limits. This does not create a transaction or start the provider."));
        m_restore_liquidity_defaults = new QPushButton(
            tr("Restore recommended liquidity defaults"), targets);
        m_restore_liquidity_defaults->setObjectName("paymasterRestoreLiquidityDefaults");
        m_restore_liquidity_defaults->setToolTip(tr(
            "Reset the displayed targets and finite maintenance limits, and disable paid maintenance approval. This does not save the policy or create, spend or retire any wallet output."));
        auto* liquidity_target_actions = new QHBoxLayout();
        liquidity_target_actions->addWidget(m_restore_liquidity_defaults);
        liquidity_target_actions->addStretch();
        liquidity_target_actions->addWidget(m_save_liquidity_policy);
        targets_layout->addLayout(liquidity_target_actions);
        liquidity_layout->addWidget(targets);

        auto* maintenance_status_group = new QGroupBox(
            tr("Current replenishment status"), liquidity);
        maintenance_status_group->setObjectName(
            "paymasterLiquidityMaintenanceStatusGroup");
        auto* maintenance_status_layout = new QVBoxLayout(
            maintenance_status_group);
        m_liquidity_slot_status = new QLabel(
            tr("Ready, pending and missing target counts have not been loaded yet."),
            maintenance_status_group);
        m_liquidity_slot_status->setObjectName("paymasterLiquiditySlotStatus");
        m_liquidity_slot_status->setWordWrap(true);
        m_liquidity_recycling_status = new QLabel(
            tr("No recyclable successor information is available yet."),
            maintenance_status_group);
        m_liquidity_recycling_status->setObjectName(
            "paymasterLiquidityRecyclingStatus");
        m_liquidity_recycling_status->setWordWrap(true);
        m_liquidity_budget_status = new QLabel(
            tr("Maintenance budget use has not been loaded yet."),
            maintenance_status_group);
        m_liquidity_budget_status->setObjectName(
            "paymasterLiquidityBudgetStatus");
        m_liquidity_budget_status->setWordWrap(true);
        maintenance_status_layout->addWidget(m_liquidity_slot_status);
        maintenance_status_layout->addWidget(m_liquidity_recycling_status);
        maintenance_status_layout->addWidget(m_liquidity_budget_status);

        auto* carrier_management = new QGroupBox(
            tr("Carrier value and withdrawal"), liquidity);
        carrier_management->setObjectName("paymasterCarrierManagement");
        auto* carrier_management_layout = new QVBoxLayout(carrier_management);
        auto* carrier_management_help = new QLabel(tr(
            "Every active carrier keeps exactly 1.00 DD as its reusable base. User-paid "
            "service fees can accumulate above that base. Previewing excess withdrawal "
            "keeps every base intact. Releasing a whole operational carrier instead "
            "reduces its target and is allowed only while the provider is stopped."),
            carrier_management);
        carrier_management_help->setWordWrap(true);
        m_carrier_value_status = new QLabel(
            tr("Carrier base and withdrawable excess have not been loaded yet."),
            carrier_management);
        m_carrier_value_status->setObjectName("paymasterCarrierValueStatus");
        m_carrier_value_status->setWordWrap(true);
        auto* excess_buttons = new QHBoxLayout();
        m_preview_carrier_excess = new QPushButton(
            tr("Preview bundled excess withdrawal"), carrier_management);
        m_preview_carrier_excess->setObjectName(
            "paymasterPreviewCarrierExcess");
        m_preview_carrier_excess->setEnabled(false);
        m_execute_carrier_excess = new QPushButton(
            tr("Execute reviewed excess withdrawal…"), carrier_management);
        m_execute_carrier_excess->setObjectName(
            "paymasterExecuteCarrierExcess");
        m_execute_carrier_excess->setEnabled(false);
        excess_buttons->addWidget(m_preview_carrier_excess);
        excess_buttons->addWidget(m_execute_carrier_excess);
        auto* release_form = new QFormLayout();
        m_release_carrier_select = new NoWheelComboBox(carrier_management);
        m_release_carrier_select->setObjectName(
            "paymasterReleaseCarrierSelection");
        m_release_carrier_select->setAccessibleName(tr(
            "Confirmed operational carrier slot to release"));
        release_form->addRow(tr("Operational carrier to release:"),
                             m_release_carrier_select);
        auto* release_buttons = new QHBoxLayout();
        m_preview_carrier_release = new QPushButton(
            tr("Preview full slot release"), carrier_management);
        m_preview_carrier_release->setObjectName(
            "paymasterPreviewCarrierRelease");
        m_preview_carrier_release->setEnabled(false);
        m_execute_carrier_release = new QPushButton(
            tr("Execute reviewed slot release…"), carrier_management);
        m_execute_carrier_release->setObjectName(
            "paymasterExecuteCarrierRelease");
        m_execute_carrier_release->setEnabled(false);
        release_buttons->addWidget(m_preview_carrier_release);
        release_buttons->addWidget(m_execute_carrier_release);
        m_carrier_withdrawal_status = new QLabel(
            tr("No carrier withdrawal has been previewed."), carrier_management);
        m_carrier_withdrawal_status->setObjectName(
            "paymasterCarrierWithdrawalStatus");
        m_carrier_withdrawal_status->setWordWrap(true);
        carrier_management_layout->addWidget(carrier_management_help);
        carrier_management_layout->addWidget(m_carrier_value_status);
        carrier_management_layout->addLayout(excess_buttons);
        carrier_management_layout->addLayout(release_form);
        carrier_management_layout->addLayout(release_buttons);
        carrier_management_layout->addWidget(m_carrier_withdrawal_status);
        liquidity_layout->addWidget(carrier_management);

        auto* preparation = new QGroupBox(tr("Create missing pool liquidity"), liquidity);
        auto* preparation_layout = new QVBoxLayout(preparation);
        auto* preparation_help = new QLabel(tr(
            "Preparation is additive: existing live pool entries count toward the targets, so only missing outputs are proposed. Always preview the current targets before executing."), preparation);
        preparation_help->setWordWrap(true);
        preparation_layout->addWidget(preparation_help);
        m_prepare_preview = new QPushButton(tr("Preview pool preparation"), liquidity);
        auto* prepare_preview = m_prepare_preview;
        m_prepare_execute = new QPushButton(tr("Execute reviewed preparation…"), liquidity);
        auto* prepare_execute = m_prepare_execute;
        prepare_preview->setObjectName("paymasterPreviewPoolPreparation");
        prepare_execute->setObjectName("paymasterExecutePoolPreparation");
        auto* preparation_buttons = new QHBoxLayout();
        preparation_buttons->addWidget(prepare_preview);
        preparation_buttons->addWidget(prepare_execute);
        preparation_layout->addLayout(preparation_buttons);
        liquidity_layout->addWidget(preparation);

        auto* retirement = new QGroupBox(tr("Return excess available liquidity"), liquidity);
        auto* retirement_layout = new QVBoxLayout(retirement);
        auto* retirement_help = new QLabel(tr(
            "Retirement is for deliberately lowering an already prepared pool. It returns only available excess outputs to ordinary wallet liquidity; active reservations are never touched. New operators normally do not need this step."), retirement);
        retirement_help->setObjectName("paymasterLiquidityRetirementExplanation");
        retirement_help->setWordWrap(true);
        retirement_layout->addWidget(retirement_help);
        m_rebalance_preview = new QPushButton(tr("Preview excess retirement"), liquidity);
        auto* rebalance_preview = m_rebalance_preview;
        m_rebalance_execute = new QPushButton(tr("Execute reviewed retirement…"), liquidity);
        auto* rebalance_execute = m_rebalance_execute;
        rebalance_preview->setObjectName("paymasterPreviewPoolRetirement");
        rebalance_execute->setObjectName("paymasterExecutePoolRetirement");
        auto* retirement_buttons = new QHBoxLayout();
        retirement_buttons->addWidget(rebalance_preview);
        retirement_buttons->addWidget(rebalance_execute);
        retirement_layout->addLayout(retirement_buttons);
        liquidity_layout->addWidget(retirement);

        auto* result_group = new QGroupBox(tr("Latest preview or execution result"), liquidity);
        auto* result_layout = new QVBoxLayout(result_group);
        auto* result_help = new QLabel(tr(
            "A preview is side-effect free. Execution buttons remain guarded by confirmation and use the target values currently shown above."), result_group);
        result_help->setWordWrap(true);
        result_layout->addWidget(result_help);
        m_liquidity_preview_status = new QLabel(result_group);
        m_liquidity_preview_status->setObjectName("paymasterLiquidityPreviewStatus");
        m_liquidity_preview_status->setWordWrap(true);
        result_layout->addWidget(m_liquidity_preview_status);
        m_liquidity_output = new QPlainTextEdit(result_group);
        m_liquidity_output->setObjectName("paymasterLiquidityResult");
        m_liquidity_output->setReadOnly(true);
        m_liquidity_output->setMaximumHeight(260);
        m_liquidity_output->setPlaceholderText(tr("Choose Preview pool preparation to calculate the first setup transaction."));
        m_liquidity_output->viewport()->setBackgroundRole(QPalette::Window);
        m_liquidity_output->viewport()->setAutoFillBackground(true);
        result_layout->addWidget(m_liquidity_output);
        liquidity_layout->addWidget(result_group);

        auto* advanced_liquidity = new QWidget(liquidity_column);
        advanced_liquidity->setObjectName("paymasterAdvancedLiquidityPanel");
        auto* advanced_liquidity_layout = new QVBoxLayout(advanced_liquidity);
        advanced_liquidity_layout->setContentsMargins(0, 0, 0, 0);
        advanced_liquidity_layout->setSpacing(12);
        advanced_liquidity_layout->addWidget(maintenance_status_group);
        advanced_liquidity_layout->addWidget(liquidity_steps);
        advanced_liquidity_layout->addWidget(targets);
        advanced_liquidity_layout->addWidget(carrier_management);
        advanced_liquidity_layout->addWidget(preparation);
        advanced_liquidity_layout->addWidget(retirement);
        advanced_liquidity_layout->addWidget(result_group);
        m_liquidity_advanced_toggle = AddPaymasterDisclosure(
            liquidity_layout, liquidity_column, advanced_liquidity,
            tr("Change liquidity settings"),
            tr("Hide liquidity settings"),
            QStringLiteral("paymasterAdvancedLiquidityToggle"));
        liquidity_layout->addStretch();
        liquidity_scroll->setWidget(liquidity);
        ConfigureScrollArea(liquidity_scroll, liquidity);
        m_liquidity_page = liquidity_scroll;
        m_tabs->addTab(liquidity_scroll, tr("Liquidity"));

        auto* finance_scroll = new QScrollArea(m_tabs);
        finance_scroll->setObjectName("paymasterFinancesPage");
        finance_scroll->setWidgetResizable(true);
        auto* finance = new QWidget(finance_scroll);
        finance->setObjectName("paymasterFinancesContents");
        QVBoxLayout* finance_layout{nullptr};
        auto* finance_column = CreatePaymasterPageColumn(finance, finance_layout);
        AddPaymasterPageHeading(
            finance_layout, finance_column, tr("Provider finances"),
            tr("Track DigiDollar service-fee income, DigiByte operating costs and wallet-owned pool capital without treating reserved liquidity as an expense."),
            QStringLiteral("paymasterFinances"));

        auto* finance_toolbar = new QGroupBox(
            tr("Reporting period"), finance_column);
        finance_toolbar->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_toolbar_layout = new QHBoxLayout(finance_toolbar);
        auto* finance_period_help = new QLabel(tr(
            "The four summary cards use one consistent wallet snapshot. The selected period controls the detailed booking list and CSV export."),
            finance_toolbar);
        finance_period_help->setWordWrap(true);
        finance_toolbar_layout->addWidget(finance_period_help, 1);
        m_finance_period_select = new NoWheelComboBox(finance_toolbar);
        m_finance_period_select->setObjectName("paymasterFinancePeriod");
        m_finance_period_select->addItem(tr("Today"), QStringLiteral("today"));
        m_finance_period_select->addItem(tr("Last 7 days"), QStringLiteral("7d"));
        m_finance_period_select->addItem(tr("Last 30 days"), QStringLiteral("30d"));
        m_finance_period_select->addItem(tr("All recorded history"), QStringLiteral("all"));
        m_finance_period_select->setCurrentIndex(2);
        m_finance_refresh = new QPushButton(tr("Refresh"), finance_toolbar);
        m_finance_refresh->setObjectName("paymasterFinanceRefresh");
        m_finance_export = new QPushButton(tr("Export CSV…"), finance_toolbar);
        m_finance_export->setObjectName("paymasterFinanceExport");
        m_finance_export->setEnabled(false);
        finance_toolbar_layout->addWidget(m_finance_period_select);
        finance_toolbar_layout->addWidget(m_finance_refresh);
        finance_toolbar_layout->addWidget(m_finance_export);
        finance_layout->addWidget(finance_toolbar);

        // Legacy provider commits may predate the manifest-bound finance
        // ledger. Keep that coverage boundary next to the headline totals,
        // rather than hiding it in booking details where a zero income value
        // could otherwise be mistaken for a complete historical statement.
        m_finance_history_notice = new QGroupBox(
            tr("Recorded-history coverage"), finance_column);
        m_finance_history_notice->setObjectName(
            "paymasterFinanceHistoryNotice");
        m_finance_history_notice->setProperty(
            "paymasterRole", QStringLiteral("notice"));
        m_finance_history_notice->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* finance_history_notice_layout =
            new QVBoxLayout(m_finance_history_notice);
        m_finance_history_notice_text = new QLabel(
            m_finance_history_notice);
        m_finance_history_notice_text->setObjectName(
            "paymasterFinanceHistoryNoticeText");
        m_finance_history_notice_text->setWordWrap(true);
        finance_history_notice_layout->addWidget(
            m_finance_history_notice_text);
        m_finance_history_notice->hide();
        finance_layout->addWidget(m_finance_history_notice);

        auto* finance_period_cards = new PaymasterResponsiveCards(finance_column);
        finance_period_cards->setObjectName("paymasterFinancePeriodCards");
        const std::array<QString, 4> period_titles{
            tr("Today"), tr("7 days"), tr("30 days"), tr("Total")};
        for (size_t index = 0; index < period_titles.size(); ++index) {
            auto* card = new QFrame(finance_period_cards);
            card->setProperty("paymasterRole", QStringLiteral("statusCard"));
            card->setMinimumHeight(154);
            auto* card_layout = new QVBoxLayout(card);
            card_layout->setContentsMargins(16, 14, 16, 14);
            auto* title = new QLabel(period_titles.at(index), card);
            title->setProperty("paymasterRole", QStringLiteral("cardHeading"));
            m_finance_period_income.at(index) = new QLabel(
                tr("Service fees: — DD"), card);
            m_finance_period_income.at(index)->setObjectName(
                QStringLiteral("paymasterFinancePeriodIncome%1").arg(index));
            m_finance_period_income.at(index)->setProperty(
                "paymasterRole", QStringLiteral("statusText"));
            m_finance_period_cost.at(index) = new QLabel(
                tr("Operating costs: — DGB"), card);
            m_finance_period_cost.at(index)->setObjectName(
                QStringLiteral("paymasterFinancePeriodCost%1").arg(index));
            m_finance_period_transfers.at(index) = new QLabel(
                tr("Successful transfers: —"), card);
            m_finance_period_transfers.at(index)->setObjectName(
                QStringLiteral("paymasterFinancePeriodTransfers%1").arg(index));
            card_layout->addWidget(title);
            card_layout->addWidget(m_finance_period_income.at(index));
            card_layout->addWidget(m_finance_period_cost.at(index));
            card_layout->addWidget(m_finance_period_transfers.at(index));
            card_layout->addStretch();
            finance_period_cards->addCard(card);
        }
        finance_layout->addWidget(finance_period_cards);

        auto* finance_result_group = new QGroupBox(
            tr("Current-price result estimate"), finance_column);
        finance_result_group->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_result_layout = new QVBoxLayout(finance_result_group);
        m_finance_result_estimate = new QLabel(
            tr("Load finance data to calculate the current estimate."),
            finance_result_group);
        m_finance_result_estimate->setObjectName(
            "paymasterFinanceResultEstimate");
        m_finance_result_estimate->setWordWrap(true);
        m_finance_model_breakdown = new QLabel(finance_result_group);
        m_finance_model_breakdown->setObjectName(
            "paymasterFinanceModelBreakdown");
        m_finance_model_breakdown->setWordWrap(true);
        auto* finance_valuation_note = new QLabel(tr(
            "Native DD income and DGB costs are the accounting source of truth. Any USD result uses only the current Oracle price and is not a historical exchange-rate calculation."),
            finance_result_group);
        finance_valuation_note->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        finance_valuation_note->setWordWrap(true);
        finance_result_layout->addWidget(m_finance_result_estimate);
        finance_result_layout->addWidget(m_finance_model_breakdown);
        finance_result_layout->addWidget(finance_valuation_note);
        finance_layout->addWidget(finance_result_group);

        auto* finance_pool_group = new QGroupBox(
            tr("Wallet-owned pool capital"), finance_column);
        finance_pool_group->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_pool_layout = new QVBoxLayout(finance_pool_group);
        auto* finance_pool_help = new QLabel(tr(
            "Prepared DGB and the 1.00 DD base of each carrier remain your wallet's operating capital. They are shown separately from income and expenses."),
            finance_pool_group);
        finance_pool_help->setWordWrap(true);
        m_finance_pool_dgb = new QLabel(
            tr("DGB capacity has not been loaded yet."), finance_pool_group);
        m_finance_pool_dgb->setObjectName("paymasterFinancePoolDgb");
        m_finance_pool_dgb->setWordWrap(true);
        m_finance_pool_carrier = new QLabel(
            tr("DD carrier capital has not been loaded yet."),
            finance_pool_group);
        m_finance_pool_carrier->setObjectName("paymasterFinancePoolCarrier");
        m_finance_pool_carrier->setWordWrap(true);
        m_finance_pending_maintenance = new QLabel(finance_pool_group);
        m_finance_pending_maintenance->setObjectName(
            "paymasterFinancePendingMaintenance");
        m_finance_pending_maintenance->setWordWrap(true);
        finance_pool_layout->addWidget(finance_pool_help);
        finance_pool_layout->addWidget(m_finance_pool_dgb);
        finance_pool_layout->addWidget(m_finance_pool_carrier);
        finance_pool_layout->addWidget(m_finance_pending_maintenance);
        finance_layout->addWidget(finance_pool_group);

        m_finance_backup_notice = new QGroupBox(
            tr("Provider-wallet backup recommended"), finance_column);
        m_finance_backup_notice->setObjectName("paymasterFinanceBackupNotice");
        m_finance_backup_notice->setProperty(
            "paymasterRole", QStringLiteral("notice"));
        m_finance_backup_notice->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* finance_backup_layout = new QVBoxLayout(m_finance_backup_notice);
        m_finance_backup_text = new QLabel(m_finance_backup_notice);
        m_finance_backup_text->setWordWrap(true);
        m_finance_backup_provider_id = new QLabel(m_finance_backup_notice);
        m_finance_backup_provider_id->setObjectName(
            "paymasterFinanceBackupProviderId");
        m_finance_backup_provider_id->setWordWrap(true);
        m_finance_backup_provider_id->setTextInteractionFlags(
            Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        auto* finance_backup_actions = new QHBoxLayout();
        m_finance_backup_now = new QPushButton(
            tr("Back up provider wallet now…"), m_finance_backup_notice);
        m_finance_backup_now->setObjectName("paymasterFinanceBackupNow");
        m_finance_backup_now->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_finance_backup_external = new QPushButton(
            tr("Acknowledge another full-wallet backup…"),
            m_finance_backup_notice);
        m_finance_backup_external->setObjectName(
            "paymasterFinanceExternalBackup");
        finance_backup_actions->addWidget(m_finance_backup_now);
        finance_backup_actions->addWidget(m_finance_backup_external);
        finance_backup_actions->addStretch();
        finance_backup_layout->addWidget(m_finance_backup_text);
        finance_backup_layout->addWidget(m_finance_backup_provider_id);
        finance_backup_layout->addLayout(finance_backup_actions);
        m_finance_backup_notice->hide();
        finance_layout->addWidget(m_finance_backup_notice);

        auto* finance_actions_group = new QGroupBox(
            tr("Pool finance actions"), finance_column);
        finance_actions_group->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_actions_layout = new QGridLayout(finance_actions_group);
        auto* finance_actions_help = new QLabel(tr(
            "These shortcuts open the existing preview-first liquidity controls. No funds move until you review a fresh plan and separately confirm its execution."),
            finance_actions_group);
        finance_actions_help->setWordWrap(true);
        finance_actions_layout->addWidget(finance_actions_help, 0, 0, 1, 2);
        m_finance_withdraw_fees = new QPushButton(
            tr("Withdraw DD service fees"), finance_actions_group);
        m_finance_release_carrier = new QPushButton(
            tr("Release carrier capital"), finance_actions_group);
        m_finance_review_dgb = new QPushButton(
            tr("Review excess DGB liquidity"), finance_actions_group);
        m_finance_add_liquidity = new QPushButton(
            tr("Add provider liquidity"), finance_actions_group);
        m_finance_withdraw_fees->setObjectName(
            "paymasterFinanceWithdrawFees");
        m_finance_release_carrier->setObjectName(
            "paymasterFinanceReleaseCarrier");
        m_finance_review_dgb->setObjectName(
            "paymasterFinanceReviewDgb");
        m_finance_add_liquidity->setObjectName(
            "paymasterFinanceAddLiquidity");
        finance_actions_layout->addWidget(m_finance_withdraw_fees, 1, 0);
        finance_actions_layout->addWidget(m_finance_release_carrier, 1, 1);
        finance_actions_layout->addWidget(m_finance_review_dgb, 2, 0);
        finance_actions_layout->addWidget(m_finance_add_liquidity, 2, 1);
        finance_layout->addWidget(finance_actions_group);

        auto* finance_details = new QWidget(finance_column);
        finance_details->setObjectName("paymasterFinanceDetailsPanel");
        auto* finance_details_layout = new QVBoxLayout(finance_details);
        finance_details_layout->setContentsMargins(0, 0, 0, 0);
        m_finance_history_status = new QLabel(finance_details);
        m_finance_history_status->setObjectName(
            "paymasterFinanceHistoryStatus");
        m_finance_history_status->setWordWrap(true);
        auto* finance_daily_title = new QLabel(
            tr("Daily progression (UTC)"), finance_details);
        finance_daily_title->setProperty(
            "paymasterRole", QStringLiteral("cardHeading"));
        m_finance_daily_totals = new QTableWidget(0, 5, finance_details);
        m_finance_daily_totals->setObjectName(
            "paymasterFinanceDailyTotals");
        m_finance_daily_totals->setHorizontalHeaderLabels({
            tr("Day"), tr("DD income"), tr("DGB cost"),
            tr("Transfers"), tr("Maintenance")});
        m_finance_daily_totals->setEditTriggers(
            QAbstractItemView::NoEditTriggers);
        m_finance_daily_totals->setSelectionBehavior(
            QAbstractItemView::SelectRows);
        m_finance_daily_totals->setAlternatingRowColors(true);
        m_finance_daily_totals->verticalHeader()->hide();
        m_finance_daily_totals->horizontalHeader()->setSectionResizeMode(
            QHeaderView::Stretch);
        m_finance_daily_totals->setMinimumHeight(180);
        auto* finance_bookings_title = new QLabel(
            tr("Bookings"), finance_details);
        finance_bookings_title->setProperty(
            "paymasterRole", QStringLiteral("cardHeading"));
        m_finance_events = new QTableWidget(0, 6, finance_details);
        m_finance_events->setObjectName("paymasterFinanceEvents");
        m_finance_events->setHorizontalHeaderLabels({
            tr("Date (UTC)"), tr("Booking"), tr("Status"),
            tr("DD income"), tr("DGB cost"), tr("Payment model")});
        m_finance_events->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_finance_events->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_finance_events->setAlternatingRowColors(true);
        m_finance_events->verticalHeader()->hide();
        m_finance_events->horizontalHeader()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
        m_finance_events->horizontalHeader()->setStretchLastSection(true);
        m_finance_events->setMinimumHeight(260);
        m_finance_events->setAccessibleName(tr("Paymaster finance bookings"));
        m_finance_previous_page = new QPushButton(
            tr("← Previous 250"), finance_details);
        m_finance_previous_page->setObjectName(
            "paymasterFinancePreviousPage");
        m_finance_page_status = new QLabel(
            tr("No booking page loaded"), finance_details);
        m_finance_page_status->setObjectName("paymasterFinancePageStatus");
        m_finance_page_status->setAlignment(Qt::AlignCenter);
        m_finance_page_status->setAccessibleName(
            tr("Paymaster finance booking page"));
        m_finance_next_page = new QPushButton(
            tr("Next 250 →"), finance_details);
        m_finance_next_page->setObjectName("paymasterFinanceNextPage");
        auto* finance_page_navigation = new QHBoxLayout();
        finance_page_navigation->addWidget(m_finance_previous_page);
        finance_page_navigation->addStretch();
        finance_page_navigation->addWidget(m_finance_page_status);
        finance_page_navigation->addStretch();
        finance_page_navigation->addWidget(m_finance_next_page);
        finance_details_layout->addWidget(m_finance_history_status);
        finance_details_layout->addWidget(finance_daily_title);
        finance_details_layout->addWidget(m_finance_daily_totals);
        finance_details_layout->addWidget(finance_bookings_title);
        finance_details_layout->addWidget(m_finance_events);
        finance_details_layout->addLayout(finance_page_navigation);
        m_finance_details_toggle = AddPaymasterDisclosure(
            finance_layout, finance_column, finance_details,
            tr("Show booking details"), tr("Hide booking details"),
            QStringLiteral("paymasterFinanceDetailsToggle"));
        finance_layout->addStretch();
        finance_scroll->setWidget(finance);
        ConfigureScrollArea(finance_scroll, finance);
        m_finance_page = finance_scroll;
        m_tabs->addTab(finance_scroll, tr("Finances"));

        auto* activity_scroll = new QScrollArea(m_tabs);
        activity_scroll->setObjectName("paymasterActivityPage");
        activity_scroll->setWidgetResizable(true);
        auto* activity = new QWidget(activity_scroll);
        activity->setObjectName("paymasterActivityContents");
        QVBoxLayout* activity_layout{nullptr};
        auto* activity_column = CreatePaymasterPageColumn(activity, activity_layout);
        AddPaymasterPageHeading(
            activity_layout, activity_column, tr("Provider operation"),
            tr("Monitor the automatic provider service, queued work and any durable reservations that require attention."),
            QStringLiteral("paymasterActivity"));

        auto* activity_intro_group = new QGroupBox(
            tr("Current service status"), activity_column);
        activity_intro_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* activity_intro_layout = new QVBoxLayout(activity_intro_group);
        auto* activity_intro = new QLabel(tr(
            "This page is used after setup. In the recommended automatic mode, Core answers "
            "capacity and quote requests and completes validated client submissions while the "
            "provider is running. The client must still confirm its exact provider and fee before signing."), activity_intro_group);
        activity_intro->setObjectName("paymasterActivityAutomaticExplanation");
        activity_intro->setWordWrap(true);
        activity_intro_layout->addWidget(activity_intro);
        auto* activity_responsibility = new QLabel(tr(
            "Keep the provider running and the wallet unlocked while serving clients. If the wallet "
            "is locked, automatic processing pauses without consuming queued messages and resumes "
            "after unlock. Wallet-local safety limits and full transaction validation remain enforced."), activity_intro_group);
        activity_responsibility->setObjectName("paymasterActivityResponsibility");
        activity_responsibility->setWordWrap(true);
        activity_intro_layout->addWidget(activity_responsibility);
        m_activity_runtime_status = new QLabel(
            tr("Provider status has not been loaded yet."), activity_intro_group);
        m_activity_runtime_status->setObjectName("paymasterActivityRuntimeStatus");
        m_activity_runtime_status->setWordWrap(true);
        QFont activity_status_font = m_activity_runtime_status->font();
        activity_status_font.setBold(true);
        m_activity_runtime_status->setFont(activity_status_font);
        activity_intro_layout->addWidget(m_activity_runtime_status);
        m_operation_primary = new QPushButton(
            tr("Check provider status"), activity_intro_group);
        m_operation_primary->setObjectName("paymasterOperationPrimaryAction");
        m_operation_primary->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        activity_intro_layout->addWidget(m_operation_primary, 0, Qt::AlignLeft);
        m_runtime_settings_toggle = new QPushButton(
            tr("Show advanced provider runtime settings"), activity_intro_group);
        m_runtime_settings_toggle->setObjectName("paymasterRuntimeSettingsToggle");
        m_runtime_settings_toggle->setCheckable(true);
        m_runtime_settings_toggle->setFlat(true);
        activity_intro_layout->addWidget(m_runtime_settings_toggle, 0, Qt::AlignLeft);
        activity_layout->addWidget(activity_intro_group);

        m_runtime_settings_panel = new QGroupBox(
            tr("Advanced provider runtime settings"), activity);
        m_runtime_settings_panel->setObjectName("paymasterRuntimeSettingsPanel");
        auto* runtime_settings_form = new QFormLayout(m_runtime_settings_panel);
        auto* runtime_settings_help = new QLabel(tr(
            "Automatic is recommended and processes bounded queue work after you consciously start "
            "the provider. Manual mode is intended only for diagnosis or expert-controlled operation. "
            "Changing modes requires the provider to be stopped."), m_runtime_settings_panel);
        runtime_settings_help->setObjectName("paymasterRuntimeSettingsExplanation");
        runtime_settings_help->setWordWrap(true);
        runtime_settings_form->addRow(runtime_settings_help);
        m_operation_mode_select = new NoWheelComboBox(m_runtime_settings_panel);
        m_operation_mode_select->setObjectName("paymasterOperationMode");
        m_operation_mode_select->addItem(
            tr("Automatic — recommended"), QStringLiteral("automatic"));
        m_operation_mode_select->addItem(
            tr("Manual — expert mode"), QStringLiteral("manual"));
        runtime_settings_form->addRow(tr("Processing mode:"), m_operation_mode_select);
        m_autostart = new QCheckBox(
            tr("Start this provider automatically after loading its wallet"),
            m_runtime_settings_panel);
        m_autostart->setObjectName("paymasterProviderAutostart");
        m_autostart->setToolTip(tr(
            "Autostart never stores a passphrase. An encrypted wallet waits for you to unlock it."));
        runtime_settings_form->addRow(m_autostart);
        m_save_runtime_settings = new QPushButton(
            tr("Save runtime settings"), m_runtime_settings_panel);
        m_save_runtime_settings->setObjectName("savePaymasterRuntimeSettings");
        m_save_runtime_settings->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        runtime_settings_form->addRow(m_save_runtime_settings);
        m_runtime_settings_result = new QLabel(
            tr("Automatic processing is the recommended default. Provider autostart is off by default."),
            m_runtime_settings_panel);
        m_runtime_settings_result->setObjectName("paymasterRuntimeSettingsResult");
        m_runtime_settings_result->setWordWrap(true);
        runtime_settings_form->addRow(m_runtime_settings_result);
        activity_layout->addWidget(m_runtime_settings_panel);
        // The Operations page itself is still hidden while it is built. Force
        // an explicit show/hide transition so Qt remembers that this expert
        // panel must remain collapsed when the page is shown later.
        m_runtime_settings_panel->show();
        m_runtime_settings_panel->hide();

        m_request_processing_group = new QGroupBox(
            tr("1. Answer one waiting request"), activity);
        m_request_processing_group->setObjectName("paymasterManualRequestGroup");
        auto* request_layout = new QVBoxLayout(m_request_processing_group);
        auto* request_explanation = new QLabel(tr(
            "Handles one capacity check, quote request, or recovery-provider request. "
            "Core verifies the message and may sign a response and reserve a pool slot and "
            "part of the safety budget. It does not yet authorize the client's final payment."),
            m_request_processing_group);
        request_explanation->setObjectName("paymasterRequestProcessingExplanation");
        request_explanation->setWordWrap(true);
        request_layout->addWidget(request_explanation);
        m_process_requests = new QPushButton(
            tr("Process one waiting request"), m_request_processing_group);
        m_process_requests->setObjectName("paymasterProcessOneRequest");
        request_layout->addWidget(m_process_requests, 0, Qt::AlignLeft);
        activity_layout->addWidget(m_request_processing_group);

        m_submit_processing_group = new QGroupBox(
            tr("2. Complete one submitted payment"), activity);
        m_submit_processing_group->setObjectName("paymasterManualSubmitGroup");
        auto* submit_layout = new QVBoxLayout(m_submit_processing_group);
        auto* submit_explanation = new QLabel(tr(
            "Handles one client-signed payment or recovery submission. Core revalidates the "
            "exact authorized transaction before signing provider inputs. A successful action "
            "can spend reserved provider DGB, commit the transaction durably, and broadcast it."),
            m_submit_processing_group);
        submit_explanation->setObjectName("paymasterSubmitProcessingExplanation");
        submit_explanation->setWordWrap(true);
        submit_layout->addWidget(submit_explanation);
        m_process_submits = new QPushButton(
            tr("Review and process one submitted payment…"), m_submit_processing_group);
        m_process_submits->setObjectName("paymasterProcessOneSubmit");
        submit_layout->addWidget(m_process_submits, 0, Qt::AlignLeft);
        activity_layout->addWidget(m_submit_processing_group);

        m_activity_result_group = new QGroupBox(
            tr("Last processing result"), activity);
        m_activity_result_group->setObjectName("paymasterManualResultGroup");
        auto* activity_result_layout = new QVBoxLayout(m_activity_result_group);
        m_activity_action_result = new QLabel(
            tr("No request or submitted payment has been processed from this screen yet."),
            m_activity_result_group);
        m_activity_action_result->setObjectName("paymasterActivityActionResult");
        m_activity_action_result->setWordWrap(true);
        m_activity_action_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        activity_result_layout->addWidget(m_activity_action_result);
        activity_layout->addWidget(m_activity_result_group);

        m_reservations_group = new QGroupBox(
            tr("Reserved and committed pool outputs"), activity);
        auto* reservations_group = m_reservations_group;
        reservations_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* reservations_layout = new QVBoxLayout(reservations_group);
        auto* reservations_explanation = new QLabel(tr(
            "A reservation is a wallet pool output temporarily assigned to an in-progress "
            "quote, payment, or recovery. It is not an additional charge. This list also "
            "shows committed, spent, or replacement-pending outputs while Core preserves "
            "their durable state. They cannot be offered to another client."),
            reservations_group);
        reservations_explanation->setObjectName("paymasterReservationsExplanation");
        reservations_explanation->setWordWrap(true);
        reservations_layout->addWidget(reservations_explanation);
        auto* refresh_activity = new QPushButton(
            tr("Refresh provider activity"), reservations_group);
        refresh_activity->setObjectName("paymasterRefreshReservations");
        reservations_layout->addWidget(refresh_activity, 0, Qt::AlignLeft);
        m_activity_summary = new QLabel(
            tr("Reservations have not been checked yet."), reservations_group);
        m_activity_summary->setObjectName("paymasterActivitySummary");
        m_activity_summary->setWordWrap(true);
        m_activity_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        reservations_layout->addWidget(m_activity_summary);
        activity_layout->addWidget(reservations_group);

        m_recovery_group = new QGroupBox(
            tr("What recovery means here"), activity);
        auto* recovery_group = m_recovery_group;
        recovery_group->setProperty("paymasterRole", QStringLiteral("notice"));
        auto* recovery_layout = new QVBoxLayout(recovery_group);
        auto* recovery_explanation = new QLabel(tr(
            "Recovery is not a manual way to rewrite or release a payment. Core keeps signed "
            "and committed work durable across restarts. Recovery requests use the first queue; "
            "recovery submissions use the second. Exact committed transactions are also "
            "reconciled when the provider starts. If an item remains reserved, inspect the "
            "technical result before attempting any cancellation through the RPC console."),
            recovery_group);
        recovery_explanation->setObjectName("paymasterRecoveryExplanation");
        recovery_explanation->setWordWrap(true);
        recovery_layout->addWidget(recovery_explanation);
        activity_layout->addWidget(recovery_group);

        auto* activity_details_toggle = new QPushButton(
            tr("Show last technical result"), activity);
        activity_details_toggle->setObjectName("paymasterActivityDetailsToggle");
        activity_details_toggle->setCheckable(true);
        activity_details_toggle->setFlat(true);
        activity_layout->addWidget(activity_details_toggle, 0, Qt::AlignLeft);
        m_activity_output = new QPlainTextEdit(activity);
        m_activity_output->setObjectName("paymasterActivityTechnicalResult");
        m_activity_output->setReadOnly(true);
        m_activity_output->setMaximumHeight(240);
        m_activity_output->setPlaceholderText(tr("No technical result available"));
        m_activity_output->viewport()->setBackgroundRole(QPalette::Window);
        m_activity_output->viewport()->setAutoFillBackground(true);
        m_activity_output->setVisible(false);
        activity_layout->addWidget(m_activity_output);
        activity_layout->addStretch();
        activity_scroll->setWidget(activity);
        ConfigureScrollArea(activity_scroll, activity);
        m_activity_page = activity_scroll;
        m_tabs->addTab(activity_scroll, tr("Operations"));

        connect(refresh, &QPushButton::clicked, this, [this] { refreshStatus(); });
        connect(m_guided_setup, &QPushButton::clicked, this, [this] {
            if (showSetupWizard()) {
                setSetupMode(PaymasterSetupMode::GUIDED);
                m_tabs->setCurrentIndex(0);
                refreshStatus();
            }
        });
        connect(m_reopen_wizard, &QPushButton::clicked, this, [this] {
            if (showSetupWizard()) {
                m_tabs->setCurrentIndex(0);
                refreshStatus();
            }
        });
        connect(m_expert_setup, &QPushButton::clicked, this, [this] {
            if (QMessageBox::warning(
                    this, tr("Manual Paymaster setup"),
                    tr("Manual setup exposes every provider control immediately. Incomplete settings keep the provider offline, while overly broad sponsored budgets can spend more DGB than intended.\n\nContinue with expert setup?"),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) == QMessageBox::Yes) {
                setSetupMode(PaymasterSetupMode::EXPERT);
                m_tabs->setCurrentWidget(m_configuration_page);
            }
        });
        connect(details_toggle, &QPushButton::toggled, this,
                [this, details_toggle, details](bool visible) {
                    details->setVisible(visible);
                    details_toggle->setText(visible ? tr("Hide technical details")
                                                    : tr("Show technical details"));
                });
        connect(activity_details_toggle, &QPushButton::toggled, this,
                [this, activity_details_toggle](bool visible) {
                    m_activity_output->setVisible(visible);
                    activity_details_toggle->setText(
                        visible ? tr("Hide last technical result")
                                 : tr("Show last technical result"));
                });
        connect(m_runtime_settings_toggle, &QPushButton::toggled, this,
                [this](bool visible) {
                    m_runtime_settings_panel->setVisible(visible);
                    m_runtime_settings_toggle->setText(
                        visible ? tr("Hide advanced provider runtime settings")
                                : tr("Show advanced provider runtime settings"));
                });
        connect(m_operation_mode_select,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] {
                    if (!m_loading_runtime_settings) {
                        m_runtime_settings_dirty = true;
                        updateProviderButtons();
                    }
                });
        connect(m_autostart, &QCheckBox::toggled, this, [this] {
            if (!m_loading_runtime_settings) {
                m_runtime_settings_dirty = true;
                updateProviderButtons();
            }
        });
        connect(m_save_runtime_settings, &QPushButton::clicked, this,
                [this] { saveRuntimeSettings(); });
        connect(m_start, &QPushButton::clicked, this, [this] {
            if (!requirePrivacyOffForSensitiveAction(
                    tr("Starting the Paymaster provider"))) {
                return;
            }
            if (!hasCompleteMutationSnapshots()) {
                m_status->setText(tr(
                    "Provider start was not attempted because a complete current provider, safety and liquidity snapshot is unavailable."));
                updateProviderButtons();
                return;
            }
            if (askPlainTextQuestion(
                    this, tr("Start Paymaster provider"),
                    providerStartConfirmationText()) != QMessageBox::Yes) {
                return;
            }
            call("startpaymaster", {}, false, nullptr,
                 [this](const UniValue& result) {
                     presentProviderStartResult(result);
                     refreshStatus();
                  });
        });
        connect(m_stop, &QPushButton::clicked, this,
                [this] { stopProvider(); });
        connect(m_operation_primary, &QPushButton::clicked, this, [this] {
            if (m_core_running) {
                m_stop->click();
            } else if (m_start->isEnabled()) {
                m_start->click();
            } else {
                refreshStatus();
            }
        });
        connect(m_overview_offer_action, &QPushButton::clicked, this,
                [this] { m_tabs->setCurrentWidget(m_configuration_page); });
        connect(m_overview_safety_action, &QPushButton::clicked, this,
                [this] { m_tabs->setCurrentWidget(m_safety_page); });
        connect(m_overview_liquidity_action, &QPushButton::clicked, this,
                [this] {
                    if (maintenanceFeeLimitExceeded()) {
                        reviewMaintenanceFeeLimit();
                        return;
                    }
                    if (m_maintenance_state ==
                        QLatin1String("waiting_for_maintenance_approval")) {
                        approveSuggestedLiquidityMaintenance();
                        return;
                    }
                    // This label is an action, not a navigation shortcut. If
                    // the bounded refill is already approved and the provider
                    // is stopped, start the provider through the same guarded
                    // confirmation path as the primary Overview button.
                    if (shouldStartAndRestoreLiquidity()) {
                        m_start->click();
                        return;
                    }
                    m_tabs->setCurrentWidget(m_liquidity_page);
                    if (!m_liquidity_targets_satisfy_provider_policy &&
                        m_liquidity_advanced_toggle) {
                        m_liquidity_advanced_toggle->setChecked(true);
                    }
                });
        connect(m_overview_operation_action, &QPushButton::clicked, this,
                [this] {
                    if (maintenanceFeeLimitExceeded()) {
                        reviewMaintenanceFeeLimit();
                    } else if (!m_liquidity_targets_satisfy_provider_policy) {
                        m_tabs->setCurrentWidget(m_liquidity_page);
                        if (m_liquidity_advanced_toggle) {
                            m_liquidity_advanced_toggle->setChecked(true);
                        }
                    } else if (!m_core_running && providerConfigurationComplete() &&
                        hasPassiveExternalWait()) {
                        refreshStatus();
                    } else if (m_maintenance_state ==
                               QLatin1String("waiting_for_maintenance_approval")) {
                        approveSuggestedLiquidityMaintenance();
                    } else if (shouldStartAndRestoreLiquidity()) {
                        m_start->click();
                    } else {
                        m_tabs->setCurrentWidget(m_activity_page);
                    }
                });
        connect(m_overview_finance_action, &QPushButton::clicked, this,
                [this] {
                    m_tabs->setCurrentWidget(m_finance_page);
                    if (!m_busy) refreshFinanceStatus();
                });
        connect(m_overview_backup_now, &QPushButton::clicked, this,
                [this] { requestProviderBackup(); });
        connect(m_overview_backup_external, &QPushButton::clicked, this,
                [this] { acknowledgeExternalBackup(); });
        connect(m_finance_backup_now, &QPushButton::clicked, this,
                [this] { requestProviderBackup(); });
        connect(m_finance_backup_external, &QPushButton::clicked, this,
                [this] { acknowledgeExternalBackup(); });
        connect(m_finance_refresh, &QPushButton::clicked, this,
                [this] { refreshFinanceStatus(); });
        connect(m_finance_period_select,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] {
                    if (m_tabs->currentWidget() == m_finance_page && !m_busy) {
                        refreshFinanceStatus();
                    }
                });
        connect(m_finance_export, &QPushButton::clicked, this,
                [this] { exportFinanceCsv(); });
        connect(m_finance_previous_page, &QPushButton::clicked, this,
                [this] { showPreviousFinancePage(); });
        connect(m_finance_next_page, &QPushButton::clicked, this,
                [this] { showNextFinancePage(); });
        connect(m_finance_withdraw_fees, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_preview_carrier_excess); });
        connect(m_finance_release_carrier, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_preview_carrier_release); });
        connect(m_finance_review_dgb, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_rebalance_preview); });
        connect(m_finance_add_liquidity, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_prepare_preview); });
        connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
            if (m_tabs->currentWidget() == m_finance_page &&
                hasRpcTransport() && !m_busy) {
                refreshFinanceStatus();
            }
        });
        connect(create_identity, &QPushButton::clicked, this, [this] {
            if (!m_display_name->hasAcceptableInput()) {
                QMessageBox::warning(
                    this, tr("Paymaster provider identity"),
                    tr("The display name must contain at most 32 printable ASCII characters and cannot contain '/' or '@'."));
                return;
            }
            UniValue params{UniValue::VARR};
            params.push_back(m_display_name->text().trimmed().toStdString());
            call("createpaymasteridentity", std::move(params), true);
        });
        connect(m_display_name, &QLineEdit::textChanged, this,
                [create_identity, this] {
                    create_identity->setEnabled(
                        !m_core_has_identity &&
                        m_display_name->hasAcceptableInput() && !m_busy);
                });
        connect(m_save_policy, &QPushButton::clicked, this,
                [this] { savePolicy(); });
        connect(m_save_provider_safety, &QPushButton::clicked, this, [this] { saveProviderSafetyPolicy(); });
        connect(m_save_client_safety, &QPushButton::clicked, this, [this] { saveClientSafetyPolicy(); });
        connect(restore_quote_defaults, &QPushButton::clicked,
                this, [this] { restoreQuoteSafetyDefaults(); });
        connect(restore_client_defaults, &QPushButton::clicked,
                this, [this] { restoreClientSafetyDefaults(); });
        connect(m_enable, &QPushButton::clicked, this, [this] { enableProvider(); });
        connect(m_approve_liquidity_maintenance, &QPushButton::clicked,
                this, [this] {
                    if (maintenanceFeeLimitExceeded()) {
                        reviewMaintenanceFeeLimit();
                    } else {
                        approveSuggestedLiquidityMaintenance();
                    }
                });
        connect(m_restore_policy_defaults, &QPushButton::clicked,
                this, [this] { restorePolicyDefaults(); });
        connect(m_restore_liquidity_defaults, &QPushButton::clicked,
                this, [this] { restoreLiquidityDefaults(); });
        connect(m_save_liquidity_policy, &QPushButton::clicked,
                this, [this] { saveLiquidityPolicy(); });
        connect(m_save_liquidity_policy_primary, &QPushButton::clicked,
                this, [this] { saveLiquidityPolicy(); });
        connect(m_automatic_replenishment, &QCheckBox::toggled,
                this, [this] { markLiquidityPolicyDirty(); });
        connect(m_paid_maintenance_approved, &QCheckBox::toggled,
                this, [this] { markLiquidityPolicyDirty(); });
        for (QLineEdit* control : {m_maintenance_fee_per_transaction,
                                   m_maintenance_fee_per_hour,
                                   m_maintenance_fee_per_day}) {
            connect(control, &QLineEdit::textChanged, this,
                    [this] { markLiquidityPolicyDirty(); });
        }
        connect(m_preview_carrier_excess, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("all_excess", false); });
        connect(m_execute_carrier_excess, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("all_excess", true); });
        connect(m_preview_carrier_release, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("release_slot", false); });
        connect(m_execute_carrier_release, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("release_slot", true); });
        connect(m_release_carrier_select,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { invalidateCarrierWithdrawalPreviews(); });
        connect(prepare_preview, &QPushButton::clicked, this, [this] { poolAction("preparepaymasterpool", false); });
        connect(prepare_execute, &QPushButton::clicked, this, [this] { poolAction("preparepaymasterpool", true); });
        connect(rebalance_preview, &QPushButton::clicked, this, [this] { poolAction("rebalancepaymasterpool", false); });
        connect(rebalance_execute, &QPushButton::clicked, this, [this] { poolAction("rebalancepaymasterpool", true); });
        connect(refresh_activity, &QPushButton::clicked, this, [this] {
            refreshActivity();
        });
        connect(m_process_requests, &QPushButton::clicked, this, [this] {
            processActivityRequest();
        });
        connect(m_process_submits, &QPushButton::clicked, this, [this] {
            processActivitySubmit();
        });
        watchFundingSafetyControls(m_user_paid_safety);
        watchFundingSafetyControls(m_public_sponsored_safety);
        watchFundingSafetyControls(m_restricted_sponsored_safety);
        for (QSpinBox* control : {m_max_active_quotes_total,
                                  m_max_active_quotes_per_netgroup,
                                  m_max_active_quotes_per_recipient,
                                  m_max_quote_requests_per_netgroup}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
                if (!m_loading_provider_safety) markProviderSafetyDirty();
                updateProviderButtons();
            });
        }
        connect(m_sponsored, &QCheckBox::toggled, this, [this] {
            if (!m_loading_policy) {
                m_policy_dirty = true;
                invalidatePoolPreviews();
            }
            updatePolicyDisplay();
            updateLiquidityDisplay();
            updateProviderButtons();
        });
        connect(m_user_paid, &QCheckBox::toggled, this, [this] {
            if (!m_loading_policy) {
                m_policy_dirty = true;
                invalidatePoolPreviews();
            }
            updatePolicyDisplay();
            updateLiquidityDisplay();
            updateProviderButtons();
        });
        connect(m_scope, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            if (!m_loading_policy) {
                m_policy_dirty = true;
                invalidatePoolPreviews();
            }
            updatePolicyDisplay();
            updateProviderButtons();
        });
        for (QSpinBox* control : {m_fee_bps, m_min_amount, m_max_amount,
                                  m_quote_ttl, m_network_fee}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged),
                    this, [this] {
                        if (!m_loading_policy) {
                            m_policy_dirty = true;
                            invalidatePoolPreviews();
                        }
                        updatePolicyDisplay();
                    });
        }
        for (QSpinBox* control : {m_admission_dgb, m_operational_dgb,
                                  m_admission_carriers, m_operational_carriers}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged),
                    this, [this] {
                        markLiquidityPolicyDirty();
                        updateLiquidityDisplay();
                    });
        }
        connect(m_client_fee_per_transaction, qOverload<int>(&QSpinBox::valueChanged),
                this, [this] {
                    if (!m_loading_client_safety) markClientSafetyDirty();
                    updateClientSafetyDisplay();
                });
        connect(m_client_fee_per_day, qOverload<int>(&QSpinBox::valueChanged),
                this, [this] {
                    if (!m_loading_client_safety) markClientSafetyDirty();
                    updateClientSafetyDisplay();
                });
        // Classify every retained operator control so the generic bright-blue
        // wallet button style cannot leak into these pages. Explicit primary,
        // danger and disclosure roles assigned above remain untouched.
        for (QGroupBox* group : findChildren<QGroupBox*>()) {
            if (!group->property("paymasterRole").isValid()) {
                group->setProperty("paymasterRole", QStringLiteral("card"));
            }
        }
        for (QPushButton* button : findChildren<QPushButton*>()) {
            if (!button->property("paymasterRole").isValid()) {
                button->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
            }
            button->setCursor(Qt::PointingHandCursor);
        }
        // QLabel defaults to AutoText. Provider endpoints and backend error
        // strings can contain angle brackets, so treating every operator label
        // as plain text prevents those values from being interpreted as rich
        // text while preserving selectable status output.
        for (QLabel* label : findChildren<QLabel*>()) {
            label->setTextFormat(Qt::PlainText);
        }
        updateFundingSafetyDisplay(m_user_paid_safety);
        updateFundingSafetyDisplay(m_public_sponsored_safety);
        updateFundingSafetyDisplay(m_restricted_sponsored_safety);
        updateClientSafetyDisplay();
        updatePolicyDisplay();
        updateLiquidityDisplay();
        updateFinanceControls();
        updateSetupAccess();
        updateProviderButtons();

        m_setup_status_timer = new QTimer(this);
        m_setup_status_timer->setInterval(10000);
        connect(m_setup_status_timer, &QTimer::timeout, this, [this] {
            const bool maintenance_wait =
                m_maintenance_state == QLatin1String("replenishing_liquidity") ||
                m_maintenance_state ==
                    QLatin1String("waiting_for_liquidity_confirmation");
            if ((m_setup_waiting_for_confirmations || maintenance_wait ||
                 hasPassiveExternalWait()) &&
                m_model && !m_busy && !m_setup_wizard_active) {
                refreshStatus();
            }
        });
    }

    /**
     * Install the application-level full-wallet backup action.
     *
     * The provider console deliberately reuses WalletView's established file
     * picker and backup reporting instead of duplicating backup mechanics in
     * this presentation-only widget.
     */
    void setProviderBackupRequestHandler(std::function<void()> handler) override
    {
        m_provider_backup_request_handler = std::move(handler);
    }

    void setWalletModel(WalletModel* model) override
    {
        m_setup_wizard_active = false;
        if (m_setup_status_timer) m_setup_status_timer->stop();
        if (m_setup_wizard) {
            // A modal assistant belongs to exactly one wallet. Closing its
            // nested event loop before changing m_model guarantees that no
            // review, unlock lease or progress page survives a wallet switch.
            m_setup_wizard->setCloseBlocked(false);
            m_setup_wizard->reject();
            m_setup_wizard.clear();
        }
        ++m_wallet_generation;
        m_pending_handler_calls.clear();
        m_rpc_handler_depth = 0;
        m_active_rpc_handler_token = 0;
        m_next_rpc_handler_token = 0;
        m_model = model;
        m_busy = false;
        m_setup_mode = loadSetupMode();
        applyRecommendedPolicyDefaults(/*mark_dirty=*/false);
        restoreLiquidityDefaults(/*announce=*/false);
        restoreFundingSafetyDefaults(m_user_paid_safety,
                                     /*recommended_defaults=*/true,
                                     /*mark_dirty=*/false);
        restoreFundingSafetyDefaults(m_public_sponsored_safety,
                                     /*recommended_defaults=*/false,
                                     /*mark_dirty=*/false);
        restoreFundingSafetyDefaults(m_restricted_sponsored_safety,
                                     /*recommended_defaults=*/false,
                                     /*mark_dirty=*/false);
        restoreQuoteSafetyDefaults(/*mark_dirty=*/false);
        restoreClientSafetyDefaults(/*mark_dirty=*/false);
        m_loading_runtime_settings = true;
        m_operation_mode = QStringLiteral("automatic");
        m_autostart_enabled = false;
        m_service_state = QStringLiteral("stopped");
        m_last_service_error.clear();
        m_waiting_provider_requests = 0;
        m_waiting_provider_submits = 0;
        m_runtime_settings_dirty = false;
        m_operation_mode_select->setCurrentIndex(
            m_operation_mode_select->findData(m_operation_mode));
        m_autostart->setChecked(false);
        m_loading_runtime_settings = false;
        m_runtime_settings_result->setText(tr(
            "Automatic processing is the recommended default. Provider autostart is off by default."));
        m_provider_safety_configured = false;
        m_provider_safety_snapshot_representable = true;
        m_unrepresentable_provider_safety_snapshot =
            UniValue{UniValue::VOBJ};
        setProviderSafetyMutationEnabled(true);
        m_policy_snapshot_representable = true;
        m_unrepresentable_policy_snapshot = UniValue{UniValue::VOBJ};
        setPolicyMutationEnabled(true);
        m_provider_info_snapshot_available = false;
        m_provider_safety_snapshot_available = false;
        m_liquidity_snapshot_available = false;
        m_provider_settings_present = false;
        m_client_safety_configured = false;
        m_client_safety_snapshot_representable = true;
        m_unrepresentable_client_safety_snapshot =
            UniValue{UniValue::VOBJ};
        if (m_client_safety_group) {
            m_client_safety_group->setEnabled(!m_busy);
        }
        m_core_eligible = false;
        m_core_enabled = false;
        m_core_running = false;
        m_core_ready = false;
        m_core_locked = true;
        m_core_has_identity = false;
        m_provider_id.clear();
        m_provider_endpoint.clear();
        m_display_name->clear();
        updateIdentityLabels();
        m_backup_required = false;
        m_finance_last_result = UniValue{UniValue::VOBJ};
        m_finance_pages.clear();
        m_finance_page_index = -1;
        m_finance_loaded = false;
        m_finance_loading = false;
        m_finance_export_loading = false;
        m_finance_refresh->setText(tr("Refresh"));
        m_finance_export->setText(tr("Export CSV…"));
        updateBackupReminder(false, {});
        m_finance_daily_totals->setRowCount(0);
        m_finance_events->setRowCount(0);
        updateFinanceControls();
        m_finance_history_status->setText(
            tr("Finance history has not been loaded yet."));
        m_finance_result_estimate->setText(
            tr("Load finance data to calculate the current estimate."));
        m_finance_model_breakdown->clear();
        m_activity_output->clear();
        m_liquidity_output->clear();
        m_activity_action_result->setText(
            tr("No manual operation has been run for this wallet."));
        m_activity_summary->setText(
            tr("Waiting for an authoritative provider status."));
        m_overview_finance_status->setText(
            tr("Waiting · finance history is available after provider identity creation"));
        setStatusLabel(m_overview_finance_status,
                       m_overview_finance_status->text(),
                       QStringLiteral("waiting"));
        m_oracle_state = OracleState::UNKNOWN;
        m_oracle_price_micro_usd = 0;
        m_pool_admission_dgb = 0;
        m_pool_admission_carriers = 0;
        m_pool_operational_dgb = 0;
        m_pool_operational_carriers = 0;
        m_pool_status_loaded = false;
        m_readiness_errors.clear();
        m_liquidity_policy_configured = false;
        m_liquidity_snapshot_representable = true;
        m_unrepresentable_liquidity_snapshot = UniValue{UniValue::VOBJ};
        setLiquidityPolicyMutationEnabled(true);
        m_liquidity_targets_satisfy_provider_policy = false;
        m_liquidity_policy_dirty = false;
        m_loading_liquidity_policy = false;
        m_maintenance_state = QStringLiteral("unavailable");
        m_carrier_base_cents = 0;
        m_carrier_excess_cents = 0;
        m_pending_successor_carrier_cents = 0;
        m_pending_successor_dgb_satoshis = 0;
        invalidateCarrierWithdrawalPreviews();
        m_setup_waiting_for_confirmations = false;
        if (m_setup_status_timer) m_setup_status_timer->stop();
        m_enable_status->setText(
            tr("Provider configuration is currently disabled for this wallet."));
        m_provider_safety_status->setText(
            tr("Provider safety policy: not configured (provider not ready)"));
        m_client_safety_status->setText(
            tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"));
        m_provider_safety_usage->setText(tr("Budget usage is not available yet."));
        m_liquidity_current_status->setText(
            tr("Current confirmed pool: status not loaded yet."));
        m_liquidity_policy_status->setText(
            tr("Liquidity policy has not been loaded yet."));
        m_liquidity_slot_status->setText(
            tr("Ready, pending and missing target counts have not been loaded yet."));
        m_liquidity_recycling_status->setText(
            tr("No recyclable successor information is available yet."));
        m_liquidity_budget_status->setText(
            tr("Maintenance budget use has not been loaded yet."));
        m_carrier_value_status->setText(
            tr("Carrier base and withdrawable excess have not been loaded yet."));
        m_liquidity_maintenance_state->setText(
            tr("Automatic liquidity status is not available yet."));
        m_liquidity_maintenance_next_step->setText(
            tr("Refresh after selecting a provider wallet."));
        m_liquidity_maintenance_cost->clear();
        updateExternalPrerequisites();
        m_approve_liquidity_maintenance->setVisible(false);
        m_release_carrier_select->clear();
        m_preview_carrier_excess->setEnabled(false);
        m_preview_carrier_release->setEnabled(false);
        m_wallet->setText(model ? tr("Selected wallet: %1").arg(model->getDisplayName())
                                : tr("No wallet selected"));
        setEnabled(model != nullptr);
        updateSetupAccess();
        updateProviderButtons();
        if (model) refreshStatus();
    }

    void setPrivacy(bool privacy) override
    {
        if (m_privacy == privacy) return;
        m_privacy = privacy;

        if (privacy) {
            // Privacy can be toggled by an application-level setting while a
            // nested modal loop is active. Close every Paymaster message box
            // before redacting the underlying pages so an exact funding
            // preview or backend detail cannot remain visible above them.
            for (QMessageBox* dialog : findChildren<QMessageBox*>()) {
                dialog->reject();
            }
        }

        if (privacy && m_setup_wizard) {
            // The wizard's review pages intentionally expose exact provider
            // identity, policy and funding values. Closing it is the only
            // fail-closed response when privacy mode changes mid-session.
            m_setup_wizard_active = false;
            m_setup_wizard->setCloseBlocked(false);
            m_setup_wizard->reject();
            m_setup_wizard.clear();
        }

        // Detailed records and raw expert-operation output are more useful
        // hidden than partially redacted: dates, counts and identifiers can be
        // sensitive even when currency digits themselves are masked.
        m_finance_daily_totals->setVisible(!privacy);
        m_finance_events->setVisible(!privacy);
        m_activity_output->setVisible(!privacy);
        m_liquidity_output->setVisible(!privacy);
        m_display_name->setEchoMode(
            privacy ? QLineEdit::Password : QLineEdit::Normal);
        setSensitivePagePrivacy(privacy);

        if (privacy) {
            if (m_core_has_identity) {
                m_identity->setText(tr(
                    "Identity: hidden by privacy mode\nEndpoint: hidden by privacy mode"));
                m_offer_identity_id->setText(tr("Hidden by privacy mode"));
            }
            m_overview_finance_status->setText(
                maskNumericText(m_overview_finance_status->text()));
            m_pool->setText(maskNumericText(m_pool->text()));
            m_wallet->setText(tr("Selected wallet hidden by privacy mode"));
            m_activity_summary->setText(tr(
                "Provider activity details are hidden by privacy mode."));
            m_activity_action_result->setText(tr(
                "Operation results are hidden by privacy mode."));
            for (QLabel* label : {
                     m_overview_offer_status, m_overview_safety_status,
                     m_overview_liquidity_status,
                     m_overview_operation_status,
                     m_overview_finance_status,
                     m_liquidity_maintenance_state,
                     m_liquidity_maintenance_next_step,
                     m_liquidity_maintenance_cost}) {
                if (label) label->setText(maskNumericText(label->text()));
            }
        } else {
            updateIdentityLabels();
            m_wallet->setText(m_model
                ? tr("Selected wallet: %1").arg(m_model->getDisplayName())
                : tr("No wallet selected"));
        }
        if (m_activity_result_group) {
            m_activity_result_group->setVisible(
                !privacy && m_operation_mode == QLatin1String("manual"));
        }

        if (m_finance_page_index >= 0 &&
            m_finance_page_index < static_cast<int>(m_finance_pages.size())) {
            renderFinancePage(m_finance_page_index);
        }
        updateBackupReminder(m_backup_required, m_provider_id);
        updateFinanceControls();
        updateSetupAccess();
        updateAutomaticRefreshTimer();
        redactVisiblePrivacyText();

        // The overview contains pool counts not retained by the finance-page
        // cache. Re-read them when privacy mode is removed rather than trying
        // to reverse a visual redaction.
        if (!privacy && hasRpcTransport() && !m_busy) refreshStatus();
    }

    void refreshStatus() override
    {
        // Retain the last authoritative snapshot while the asynchronous RPCs
        // are in flight. Temporarily forcing ready=false made every passive
        // ten-second refresh switch buttons and cards to an error-looking
        // state before immediately restoring the same result.
        call("getpaymasterinfo", {}, false, nullptr, [this](const UniValue& result) {
            const UniValue& policy = result.find_value("policy");
            const UniValue& pool = result.find_value("pool");
            if (!IsCompleteProviderInfoSnapshot(result)) {
                m_provider_info_snapshot_available = false;
                m_status->setText(tr(
                    "Provider status unavailable: Core returned an incomplete Paymaster provider record."));
                updateProviderButtons();
                return;
            }
            m_provider_info_snapshot_available = true;
            const UniValue& settings_present =
                result.find_value("settings_present");
            m_provider_settings_present = settings_present.isBool() &&
                                          settings_present.get_bool();
            const bool eligible = result.find_value("wallet_eligible").get_bool();
            const bool enabled = result.find_value("enabled").get_bool();
            const bool running = result.find_value("running").get_bool();
            const bool ready = result.find_value("ready").get_bool();
            const bool locked = result.find_value("wallet_locked").get_bool();
            m_core_eligible = eligible;
            m_core_enabled = enabled;
            m_core_running = running;
            m_core_ready = ready;
            m_core_locked = locked;
            const UniValue& operation_mode = result.find_value("operation_mode");
            const UniValue& autostart = result.find_value("autostart");
            const UniValue& service_state = result.find_value("service_state");
            const UniValue& last_service_error = result.find_value("last_service_error");
            const UniValue& service_queue = result.find_value("service_queue");
            m_operation_mode = operation_mode.isStr()
                ? QString::fromStdString(operation_mode.get_str())
                : QStringLiteral("automatic");
            m_autostart_enabled = autostart.isBool() && autostart.get_bool();
            m_service_state = service_state.isStr()
                ? QString::fromStdString(service_state.get_str())
                : QStringLiteral("stopped");
            m_last_service_error = last_service_error.isStr()
                ? QString::fromStdString(last_service_error.get_str())
                : QString{};
            const UniValue& waiting_requests =
                service_queue.find_value("waiting_requests");
            const UniValue& waiting_submits =
                service_queue.find_value("waiting_submits");
            m_waiting_provider_requests = waiting_requests.isNum()
                ? waiting_requests.getInt<qint64>()
                : 0;
            m_waiting_provider_submits = waiting_submits.isNum()
                ? waiting_submits.getInt<qint64>()
                : 0;
            if (!m_runtime_settings_dirty) {
                m_loading_runtime_settings = true;
                const int mode_index = m_operation_mode_select->findData(m_operation_mode);
                if (mode_index >= 0) m_operation_mode_select->setCurrentIndex(mode_index);
                m_autostart->setChecked(m_autostart_enabled);
                m_loading_runtime_settings = false;
            }
            m_enable_status->setText(enabled
                ? m_autostart_enabled
                    ? tr("Provider configuration is enabled and saved in this wallet. Autostart may bring it online whenever all readiness requirements pass.")
                    : tr("Provider configuration is enabled and saved in this wallet. Autostart is disabled, so it remains offline until you start it from Overview.")
                : tr("Provider configuration is currently disabled for this wallet."));
            if (m_service_state == QLatin1String("active")) {
                m_status->setText(tr("Automatic provider operation active — eligible requests and validated payments are processed by Core within the saved limits."));
            } else if (m_service_state == QLatin1String("manual")) {
                m_status->setText(tr("Provider running in manual expert mode — waiting messages require manual processing from Activity and recovery."));
            } else if (m_service_state == QLatin1String("waiting_for_unlock")) {
                m_status->setText(tr("Provider paused — unlock this wallet to resume automatic processing. No queued message is consumed while locked."));
            } else if (m_service_state == QLatin1String("waiting_for_readiness")) {
                m_status->setText(tr("Provider autostart is waiting for the remaining readiness requirements."));
            } else if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
                m_status->setText(tr("Provider started safely but is waiting for your one-time approval of finite paid liquidity-maintenance limits."));
            } else if (m_service_state == QLatin1String("replenishing_liquidity")) {
                m_status->setText(tr("Provider is automatically restoring missing liquidity within the approved maintenance limits."));
            } else if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
                m_status->setText(tr("Provider is waiting for replacement liquidity to confirm before accepting new requests."));
            } else if (m_service_state == QLatin1String("drain_only")) {
                m_status->setText(tr("Provider in recovery-only operation — no new quotes are accepted while existing durable work is completed safely."));
            } else if (m_service_state == QLatin1String("error")) {
                m_status->setText(tr("Automatic provider operation requires attention. Open Activity and recovery for the stable error code."));
            } else if (running) {
                m_status->setText(tr("Provider running — this wallet is currently available to eligible DigiDollar clients."));
            } else if (ready) {
                m_status->setText(tr("Setup complete — review the settings, then start the provider when you are ready."));
            } else {
                m_status->setText(tr("Setup incomplete — no provider service is currently running."));
            }
            const UniValue& provider = result.find_value("provider_id");
            m_core_has_identity = provider.isStr() && !provider.get_str().empty();
            m_provider_id = m_core_has_identity
                ? QString::fromStdString(provider.get_str()) : QString{};
            const UniValue& backup_status = result.find_value("backup_status");
            const bool backup_required = backup_status.isObject() &&
                backup_status.find_value("required").isBool() &&
                backup_status.find_value("required").get_bool();
            updateBackupReminder(backup_required, m_provider_id);
            const UniValue& finance_summary = result.find_value("finance_summary");
            m_overview_finance_action->setText(tr("Open finances"));
            if (finance_summary.isObject()) {
                const qint64 income = financeNumber(
                    finance_summary, "service_fee_income_cents");
                const qint64 cost = financeNumber(
                    finance_summary, "dgb_operating_cost_satoshis");
                const qint64 transfers = financeNumber(
                    finance_summary, "successful_transfers");
                setStatusLabel(
                    m_overview_finance_status,
                    maskNumericText(
                        tr("%1 DD service fees · %2 DGB costs · %3 successful transfer(s)")
                            .arg(ddAmount(income), dgbAmount(cost))
                            .arg(transfers)),
                    QStringLiteral("ready"));
            } else if (m_core_has_identity) {
                setStatusLabel(
                    m_overview_finance_status,
                    tr("Waiting · finance ledger has not been initialized yet"),
                    QStringLiteral("waiting"));
            } else {
                setStatusLabel(
                    m_overview_finance_status,
                    tr("Available after the provider identity is created"),
                    QStringLiteral("waiting"));
            }
            const UniValue& endpoint = result.find_value("endpoint");
            m_provider_endpoint = endpoint.isStr()
                ? QString::fromStdString(endpoint.get_str()) : QString{};
            const UniValue& display_name = result.find_value("display_name");
            if (display_name.isStr()) {
                m_display_name->setText(QString::fromStdString(display_name.get_str()));
            }
            if (policy.isObject() && !m_policy_dirty) loadPolicy(policy);
            updateIdentityLabels();
            bool has_pool_entries{false};
            if (pool.isObject()) {
                m_pool_status_loaded = true;
                m_pool_admission_dgb = pool.find_value("admission_dgb").getInt<int>();
                m_pool_admission_carriers = pool.find_value("admission_carriers").getInt<int>();
                m_pool_operational_dgb = pool.find_value("operational_dgb").getInt<int>();
                m_pool_operational_carriers = pool.find_value("operational_carriers").getInt<int>();
                has_pool_entries =
                    m_pool_admission_dgb > 0 ||
                    m_pool_admission_carriers > 0 ||
                    m_pool_operational_dgb > 0 ||
                    m_pool_operational_carriers > 0 ||
                    pool.find_value("reserved").getInt<int>() > 0;
                m_pool->setText(maskNumericText(
                    tr("Pool: admission DGB %1, carriers %2; operational DGB %3, carriers %4; reserved %5")
                        .arg(m_pool_admission_dgb)
                        .arg(m_pool_admission_carriers)
                        .arg(m_pool_operational_dgb)
                        .arg(m_pool_operational_carriers)
                        .arg(pool.find_value("reserved").getInt<int>())));
                m_liquidity_current_status->setText(tr(
                    "Current confirmed and available pool — admission DGB: %1; admission carriers: %2; operational DGB: %3; operational carriers: %4; usable complete operational slots: %5; currently reserved: %6.")
                    .arg(pool.find_value("admission_dgb").getInt<int>())
                    .arg(pool.find_value("admission_carriers").getInt<int>())
                    .arg(pool.find_value("operational_dgb").getInt<int>())
                    .arg(pool.find_value("operational_carriers").getInt<int>())
                    .arg(pool.find_value("complete_operational_slots").getInt<int>())
                    .arg(pool.find_value("reserved").getInt<int>()));
            }
            const bool has_existing_configuration =
                m_provider_settings_present ||
                (provider.isStr() && !provider.get_str().empty()) || policy.isObject() ||
                enabled || running || ready || has_pool_entries;
            if (m_setup_mode == PaymasterSetupMode::UNDECIDED &&
                has_existing_configuration) {
                setSetupMode(PaymasterSetupMode::EXISTING);
            }
            QStringList errors;
            for (const UniValue& error : result.find_value("readiness_errors").getValues()) {
                errors.push_back(QString::fromStdString(error.get_str()));
            }
            m_readiness_errors = errors;
            m_readiness->setPlainText(errors.isEmpty() ? tr("All readiness gates pass.") : errors.join('\n'));
            updateReadinessSummary(errors, running, ready);
            updateProviderHeaderStatus();
            if (m_setup_waiting_for_confirmations) {
                if (ready || running) {
                    m_setup_waiting_for_confirmations = false;
                    m_setup_status_timer->stop();
                } else if (errors.contains(QStringLiteral("PAYMASTER_POOLS_NOT_PREPARED"))) {
                    m_next_step->setText(tr("Waiting for liquidity confirmations"));
                    m_readiness_summary->setText(tr(
                        "All settings have been saved and no further Save buttons are required. "
                        "The newly prepared pool outputs must confirm before the provider can start. "
                        "This status refreshes automatically."));
                }
            }
            m_reopen_wizard->setText(has_existing_configuration
                ? tr("Review setup with assistant…")
                : tr("Continue guided setup…"));
            if (m_offer_identity_status) {
                setStatusLabel(
                    m_offer_identity_status,
                    m_core_has_identity
                        ? tr("Ready · persistent identity is stored in this wallet")
                        : tr("Action required · no provider identity has been created yet."),
                    m_core_has_identity ? QStringLiteral("ready")
                                        : QStringLiteral("action"));
                m_offer_identity_id->setText(
                    m_core_has_identity && provider.isStr()
                        ? visibleProviderId(m_provider_id)
                        : tr("Not created yet"));
                m_create_identity->setVisible(!m_core_has_identity);
                m_create_identity->setEnabled(
                    !m_core_has_identity &&
                    m_display_name->hasAcceptableInput() && !m_busy);
            }
            updateProviderButtons();
            updateAutomaticRefreshTimer();
            refreshOracleStatus();
            refreshLiquidityStatus();
        }, false, [this](const QString& error) {
            m_provider_info_snapshot_available = false;
            m_status->setText(
                tr("Provider status unavailable: %1").arg(error));
            updateProviderButtons();
        });
    }

    void setReadinessStatusForTesting(const UniValue& status) override
    {
        m_provider_info_snapshot_available = true;
        const UniValue& settings_present =
            status.find_value("settings_present");
        m_provider_settings_present = settings_present.isBool()
            ? settings_present.get_bool()
            : status.find_value("has_policy").isBool() &&
                status.find_value("has_policy").get_bool();
        m_core_eligible = status.find_value("wallet_eligible").isBool()
            ? status.find_value("wallet_eligible").get_bool() : true;
        m_core_enabled = status.find_value("enabled").isBool()
            ? status.find_value("enabled").get_bool() : true;
        m_core_running = status.find_value("running").isBool() &&
                         status.find_value("running").get_bool();
        m_core_ready = status.find_value("ready").isBool() &&
                       status.find_value("ready").get_bool();
        m_core_locked = status.find_value("wallet_locked").isBool() &&
                        status.find_value("wallet_locked").get_bool();
        const UniValue& operation_mode = status.find_value("operation_mode");
        const UniValue& autostart = status.find_value("autostart");
        const UniValue& service_state = status.find_value("service_state");
        const UniValue& last_service_error =
            status.find_value("last_service_error");
        const UniValue& service_queue = status.find_value("service_queue");
        m_operation_mode = operation_mode.isStr()
            ? QString::fromStdString(operation_mode.get_str())
            : QStringLiteral("automatic");
        m_autostart_enabled = autostart.isBool() && autostart.get_bool();
        m_service_state = service_state.isStr()
            ? QString::fromStdString(service_state.get_str())
            : QStringLiteral("stopped");
        m_last_service_error = last_service_error.isStr()
            ? QString::fromStdString(last_service_error.get_str())
            : QString{};
        const UniValue& waiting_requests =
            service_queue.find_value("waiting_requests");
        const UniValue& waiting_submits =
            service_queue.find_value("waiting_submits");
        m_waiting_provider_requests = waiting_requests.isNum()
            ? waiting_requests.getInt<qint64>()
            : 0;
        m_waiting_provider_submits = waiting_submits.isNum()
            ? waiting_submits.getInt<qint64>()
            : 0;
        if (!m_runtime_settings_dirty) {
            m_loading_runtime_settings = true;
            const int mode_index =
                m_operation_mode_select->findData(m_operation_mode);
            if (mode_index >= 0) {
                m_operation_mode_select->setCurrentIndex(mode_index);
            }
            m_autostart->setChecked(m_autostart_enabled);
            m_loading_runtime_settings = false;
        }
        m_core_has_identity = status.find_value("has_identity").isBool()
            ? status.find_value("has_identity").get_bool() : true;
        const UniValue& provider_id = status.find_value("provider_id");
        if (m_core_has_identity && provider_id.isStr()) {
            m_provider_id = QString::fromStdString(provider_id.get_str());
        }
        if (m_offer_identity_id) {
            m_offer_identity_id->setText(
                m_core_has_identity && provider_id.isStr()
                    ? visibleProviderId(m_provider_id)
                    : tr("Not created yet"));
        }
        m_policy_loaded = status.find_value("has_policy").isBool()
            ? status.find_value("has_policy").get_bool() : true;
        m_provider_safety_configured =
            status.find_value("has_safety_policy").isBool()
                ? status.find_value("has_safety_policy").get_bool() : true;
        // A readiness-only fixture has no pool object. Seed a neutral pool
        // snapshot only when no more specific liquidity fixture was applied
        // before it; otherwise preserve the authoritative missing/pending
        // counts just like the production asynchronous refresh does.
        if (!m_pool_status_loaded) {
            m_pool_status_loaded = true;
            m_pool_admission_dgb = m_admission_dgb->value();
            m_pool_operational_dgb = m_operational_dgb->value();
            m_pool_admission_carriers = m_admission_carriers->value();
            m_pool_operational_carriers = m_operational_carriers->value();
        }
        m_readiness_errors.clear();
        const UniValue& errors = status.find_value("readiness_errors");
        if (errors.isArray()) {
            for (const UniValue& error : errors.getValues()) {
                if (error.isStr()) {
                    m_readiness_errors.push_back(
                        QString::fromStdString(error.get_str()));
                }
            }
        }
        qint64 oracle_price{0};
        m_oracle_price_micro_usd =
            GetInt64Field(status, "oracle_price_micro_usd", oracle_price)
            ? oracle_price : 0;
        m_oracle_state = m_oracle_price_micro_usd > 0
            ? OracleState::AVAILABLE : OracleState::UNAVAILABLE;
        updateReadinessSummary(m_readiness_errors, m_core_running, m_core_ready);
        updateProviderHeaderStatus();
        updateProviderButtons();
    }

    void setStartResultForTesting(const UniValue& result) override
    {
        presentProviderStartResult(result);
    }

    void setLiquidityStatusForTesting(const UniValue& status) override
    {
        applyLiquidityStatus(status);
    }

    void setLiquidityPoolForTesting(const UniValue& pool_info) override
    {
        applyLiquidityPoolEntries(pool_info, /*refresh_safety=*/false);
    }

    void setMutationSnapshotsAvailableForTesting(
        bool provider_info, bool provider_safety, bool liquidity) override
    {
        // Workflow tests use intentionally narrow fixtures. Keep production's
        // fail-closed decoder path intact while allowing those tests to state
        // explicitly which authoritative snapshots are assumed available.
        m_provider_info_snapshot_available = provider_info;
        m_provider_safety_snapshot_available = provider_safety;
        m_liquidity_snapshot_available = liquidity;
        m_policy_snapshot_representable = provider_info;
        m_provider_safety_snapshot_representable = provider_safety;
        m_liquidity_snapshot_representable = liquidity;
        setPolicyMutationEnabled(provider_info);
        setProviderSafetyMutationEnabled(provider_safety);
        setLiquidityPolicyMutationEnabled(liquidity);
        updateProviderButtons();
        updateLiquidityDisplay();
        updateCarrierWithdrawalButtons();
        updateFinanceControls();
    }

    void setRpcExecutorForTesting(
        RpcExecutor executor) override
    {
        m_rpc_executor_for_testing = std::move(executor);
    }

    void setFinanceExportFilenameForTesting(QString filename) override
    {
        m_finance_export_filename_for_testing = std::move(filename);
    }

private:
    static constexpr int FINANCE_PAGE_SIZE{250};

    struct FinancePage {
        UniValue result{UniValue::VOBJ};
        std::vector<UniValue> events;
        QString request_cursor;
        QString next_cursor;
    };

    struct FinanceExportState {
        QString filename;
        QString period;
        QString provider_id;
        UniValue summary{UniValue::VOBJ};
        std::vector<UniValue> events;
        std::set<QString> requested_cursors;
        std::set<QString> event_ids;
    };

    enum class OracleState {
        UNKNOWN,
        CHECKING,
        AVAILABLE,
        UNAVAILABLE,
    };

    QString maskNumericText(QString value) const
    {
        if (!m_privacy) return value;
        for (int index = 0; index < value.size(); ++index) {
            if (value.at(index).isDigit()) value[index] = QLatin1Char('#');
        }
        return value;
    }

    static bool spinCanRepresent(const QSpinBox* control, qint64 value)
    {
        return control && value >= control->minimum() &&
            value <= control->maximum();
    }

    static bool dgbAmountCanRepresent(qint64 value)
    {
        return value >= 0 && MoneyRange(value);
    }

    static void showPlainTextWarning(QWidget* parent, const QString& title,
                                     const QString& message)
    {
        QMessageBox box(QMessageBox::Warning, title, message,
                        QMessageBox::Ok, parent);
        box.setTextFormat(Qt::PlainText);
        box.exec();
    }

    static void showPlainTextInformation(QWidget* parent,
                                         const QString& title,
                                         const QString& message)
    {
        QMessageBox box(QMessageBox::Information, title, message,
                        QMessageBox::Ok, parent);
        box.setTextFormat(Qt::PlainText);
        box.exec();
    }

    static QMessageBox::StandardButton askPlainTextQuestion(
        QWidget* parent, const QString& title, const QString& message,
        QMessageBox::StandardButtons buttons =
            QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::StandardButton default_button = QMessageBox::Cancel)
    {
        QMessageBox box(QMessageBox::Question, title, message, buttons,
                        parent);
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(default_button);
        return static_cast<QMessageBox::StandardButton>(box.exec());
    }

    bool requirePrivacyOffForSensitiveAction(const QString& action)
    {
        if (!m_privacy) return true;
        const QString message = tr(
            "%1 is unavailable while privacy mode is active because its required review contains provider identifiers, amounts or technical transaction details. Disable privacy mode and review the exact values before continuing.")
                                    .arg(action);
        m_status->setText(message);
        if (!m_rpc_executor_for_testing) {
            showPlainTextInformation(this, tr("Privacy mode active"),
                                     message);
        }
        return false;
    }

    QString privacySafeBackendError(const QString& error) const
    {
        return m_privacy
            ? tr("The Paymaster operation failed. Disable privacy mode to inspect the technical error and retry only after reviewing it.")
            : error;
    }

    void setSensitivePagePrivacy(bool privacy)
    {
        const QList<QWidget*> pages{
            m_configuration_page, m_safety_page, m_liquidity_page,
            m_finance_page, m_activity_page};
        for (QWidget* page : pages) {
            if (!page) continue;
            QList<QWidget*> controls{page};
            controls.append(page->findChildren<QWidget*>());
            for (QWidget* control : controls) {
                if (privacy) {
                    control->setProperty(
                        "paymasterPrivacySavedToolTip",
                        control->toolTip());
                    control->setProperty(
                        "paymasterPrivacySavedAccessibleName",
                        control->accessibleName());
                    control->setProperty(
                        "paymasterPrivacySavedAccessibleDescription",
                        control->accessibleDescription());
                    control->setToolTip({});
                    control->setAccessibleName({});
                    control->setAccessibleDescription({});
                } else {
                    const QVariant tooltip = control->property(
                        "paymasterPrivacySavedToolTip");
                    const QVariant name = control->property(
                        "paymasterPrivacySavedAccessibleName");
                    const QVariant description = control->property(
                        "paymasterPrivacySavedAccessibleDescription");
                    if (tooltip.isValid()) {
                        control->setToolTip(tooltip.toString());
                        control->setProperty(
                            "paymasterPrivacySavedToolTip", QVariant{});
                    }
                    if (name.isValid()) {
                        control->setAccessibleName(name.toString());
                        control->setProperty(
                            "paymasterPrivacySavedAccessibleName",
                            QVariant{});
                    }
                    if (description.isValid()) {
                        control->setAccessibleDescription(
                            description.toString());
                        control->setProperty(
                            "paymasterPrivacySavedAccessibleDescription",
                            QVariant{});
                    }
                }
            }
        }
    }

    void redactVisiblePrivacyText()
    {
        if (!m_privacy) return;
        m_identity->setText(tr(
            "Identity: hidden by privacy mode\nEndpoint: hidden by privacy mode"));
        m_offer_identity_id->setText(tr("Hidden by privacy mode"));
        m_wallet->setText(tr("Selected wallet hidden by privacy mode"));
        m_activity_summary->setText(tr(
            "Provider activity details are hidden by privacy mode."));
        m_activity_action_result->setText(tr(
            "Operation results are hidden by privacy mode."));
        for (QLabel* label : {
                 m_pool, m_overview_offer_status,
                 m_overview_safety_status, m_overview_liquidity_status,
                 m_overview_operation_status, m_overview_finance_status,
                 m_liquidity_maintenance_state,
                 m_liquidity_maintenance_next_step,
                 m_liquidity_maintenance_cost}) {
            if (label) label->setText(maskNumericText(label->text()));
        }
    }

    QString visibleProviderId(const QString& provider_id) const
    {
        if (provider_id.isEmpty()) return tr("Not created yet");
        return m_privacy ? tr("Hidden by privacy mode") : provider_id;
    }

    void updateIdentityLabels()
    {
        m_offer_identity_id->setText(
            m_core_has_identity ? visibleProviderId(m_provider_id)
                                : tr("Not created yet"));
        m_identity->setText(tr("Identity: %1\nEndpoint: %2")
            .arg(m_core_has_identity ? visibleProviderId(m_provider_id)
                                     : tr("not configured"),
                 m_privacy && !m_provider_endpoint.isEmpty()
                     ? tr("hidden by privacy mode")
                     : !m_provider_endpoint.isEmpty()
                         ? m_provider_endpoint
                         : tr("set -paymasterendpoint=<ip:port> and restart")));
    }

    void presentProviderStartResult(const UniValue& result)
    {
        if (!IsCompleteProviderStartResult(result)) {
            const QString message = tr(
                "Core did not confirm a complete provider-start result. The runtime state will be refreshed before another action is offered.");
            if (m_rpc_executor_for_testing) {
                m_status->setText(message);
            } else {
                QMessageBox::warning(
                    this, tr("Paymaster provider start not confirmed"),
                    message);
            }
            return;
        }
        const UniValue& result_mode = result.find_value("operation_mode");
        const QString mode = QString::fromStdString(result_mode.get_str());
        const bool running = result.find_value("running").get_bool();
        const bool ready = result.find_value("ready").get_bool();
        const UniValue& result_service_state =
            result.find_value("service_state");
        const QString service_state =
            QString::fromStdString(result_service_state.get_str());

        if (!running) {
            QMessageBox::warning(
                this, tr("Paymaster provider not started"),
                !m_liquidity_targets_satisfy_provider_policy
                    ? tr("The saved liquidity targets cannot satisfy the active offer. A user-paid provider needs at least three admission DigiDollar carriers and one operational DigiDollar carrier. Review and save the liquidity targets before starting the provider.")
                    : tr("The provider is not running because one or more readiness requirements are still unmet. Refresh the Overview for the next safe action."));
            return;
        }

        if (ready && (service_state == QLatin1String("active") ||
                      service_state == QLatin1String("manual"))) {
            QMessageBox::information(
                this, tr("Paymaster provider started"),
                mode == QLatin1String("manual")
                    ? tr("The provider is now online in manual expert mode. Queue items must be processed from Operations.")
                    : tr("Automatic provider operation is active. Core now processes eligible requests and validated submissions within the saved safety budgets. No provider-side confirmation is required for each transfer."));
            return;
        }

        QString pending;
        if (service_state ==
            QLatin1String("waiting_for_maintenance_approval")) {
            pending = tr("waiting for approval of the finite refill limits");
        } else if (service_state ==
                   QLatin1String("waiting_for_liquidity_confirmation")) {
            pending = tr("replacement liquidity is waiting for confirmation");
        } else if (service_state ==
                   QLatin1String("replenishing_liquidity")) {
            pending = tr("restoring the saved liquidity targets");
        } else if (service_state == QLatin1String("drain_only")) {
            pending = tr("finishing previously authorized work in recovery-only mode");
        } else {
            pending = tr("checking the remaining readiness requirement");
        }

        // A pending start is normal background progress, not a result the
        // operator must acknowledge. A modal message box runs a nested event
        // loop, so the periodic refresh can make the Overview ready while the
        // old "startup pending" text remains in front of it. Keep progress in
        // the live status area instead; the caller immediately refreshes the
        // authoritative Core state after this short transitional message.
        m_status->setText(
            tr("Provider start accepted — %1.").arg(pending));
    }

    bool hasRpcTransport() const
    {
        return m_model || static_cast<bool>(m_rpc_executor_for_testing);
    }

    bool hasCompleteMutationSnapshots() const
    {
        return m_provider_info_snapshot_available &&
            m_provider_safety_snapshot_available &&
            m_liquidity_snapshot_available;
    }

    static bool isPassiveExternalReadinessError(const QString& error)
    {
        return error == QLatin1String("PAYMASTER_NODE_NOT_READY") ||
               error == QLatin1String("PAYMASTER_REQUIRES_READY_TXINDEX") ||
               error == QLatin1String("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE");
    }

    static bool isProviderSetupReadinessError(const QString& error)
    {
        return error == QLatin1String("PAYMASTER_PROVIDER_NOT_ENABLED") ||
               error == QLatin1String("PAYMASTER_IDENTITY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_POLICY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_SAFETY_POLICY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_POLICY_BINDING_MISMATCH") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED") ||
               error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED") ||
               error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE");
    }

    static bool isLiquidityReadinessError(const QString& error)
    {
        // These errors describe the live inventory, not unsaved provider
        // configuration. Keeping the distinction explicit prevents a spent
        // or not-yet-confirmed slot from sending the operator back through
        // Offer or setup even though the saved targets are already correct.
        return error == QLatin1String("PAYMASTER_POOLS_NOT_PREPARED") ||
               error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
               error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
               error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING") ||
               error == QLatin1String("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED");
    }

    bool providerConfigurationComplete() const
    {
        return std::none_of(m_readiness_errors.cbegin(),
                            m_readiness_errors.cend(),
                            isProviderSetupReadinessError);
    }

    bool hasPassiveExternalWait() const
    {
        return !m_readiness_errors.isEmpty() &&
               std::any_of(m_readiness_errors.cbegin(),
                           m_readiness_errors.cend(),
                           isPassiveExternalReadinessError);
    }

    bool shouldStartAndRestoreLiquidity() const
    {
        return !m_core_running && m_start && m_start->isEnabled() &&
               !hasPassiveExternalWait() &&
               m_liquidity_targets_satisfy_provider_policy &&
               m_maintenance_state ==
                   QLatin1String("replenishing_liquidity");
    }

    bool maintenanceFeeLimitExceeded() const
    {
        return m_last_service_error ==
                   QLatin1String("PAYMASTER_MAINTENANCE_FEE_EXCEEDED") ||
               m_last_service_error ==
                   QLatin1String("PAYMASTER_MAINTENANCE_FEE_CHANGED");
    }

    void updateAutomaticRefreshTimer()
    {
        if (!m_setup_status_timer) return;
        const bool maintenance_wait =
            m_setup_waiting_for_confirmations ||
            m_maintenance_state == QLatin1String("replenishing_liquidity") ||
            m_maintenance_state ==
                QLatin1String("waiting_for_liquidity_confirmation");
        if (m_model && !m_busy && !m_setup_wizard_active &&
            (maintenance_wait || hasPassiveExternalWait())) {
            m_setup_status_timer->start();
        } else {
            m_setup_status_timer->stop();
        }
    }

    void updateProviderHeaderStatus()
    {
        const bool liquidity_attention = std::any_of(
            m_readiness_errors.cbegin(), m_readiness_errors.cend(),
            [](const QString& error) { return isLiquidityReadinessError(error); });
        if (m_service_state == QLatin1String("active")) {
            m_status->setText(tr("Automatic provider operation active — eligible requests and validated payments are processed by Core within the saved limits."));
        } else if (m_service_state == QLatin1String("manual")) {
            m_status->setText(tr("Provider running in manual expert mode — waiting messages require manual processing from Operations."));
        } else if (m_service_state == QLatin1String("waiting_for_unlock")) {
            m_status->setText(tr("Provider paused — unlock this wallet to resume automatic processing. No queued message is consumed while locked."));
        } else if (m_readiness_errors.contains(QStringLiteral(
                       "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
                   (!m_liquidity_targets_satisfy_provider_policy &&
                    (m_last_service_error == QLatin1String(
                         "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
                     m_last_service_error == QLatin1String(
                         "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
            m_status->setText(tr("Provider configuration needs one liquidity change — the active user-paid offer has no saved operational DigiDollar carrier target."));
        } else if (!m_core_ready && providerConfigurationComplete() &&
                   hasPassiveExternalWait()) {
            m_status->setText(tr("Provider fully configured — waiting for blockchain and node readiness. You do not need to change provider settings."));
        } else if (providerConfigurationComplete() && liquidity_attention) {
            m_status->setText(tr("Provider settings are saved — restore the missing wallet liquidity before the service can accept transfers."));
        } else if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
            m_status->setText(tr("Provider started safely but is waiting for your one-time approval of finite paid liquidity-maintenance limits."));
        } else if (m_service_state == QLatin1String("replenishing_liquidity") &&
                   maintenanceFeeLimitExceeded()) {
            m_status->setText(tr(
                "Provider running, but automatic liquidity refill is paused because the approved per-transaction maintenance-fee limit is too low. No refill transaction was created."));
        } else if (m_service_state == QLatin1String("replenishing_liquidity")) {
            m_status->setText(tr("Provider is automatically restoring missing liquidity within the approved maintenance limits."));
        } else if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
            m_status->setText(tr("Provider is waiting for replacement liquidity to confirm before accepting new requests."));
        } else if (m_service_state == QLatin1String("drain_only")) {
            m_status->setText(tr("Provider in recovery-only operation — no new quotes are accepted while existing durable work is completed safely."));
        } else if (m_service_state == QLatin1String("error")) {
            m_status->setText(tr("Automatic provider operation requires attention. Open Operations for details."));
        } else if (m_core_running) {
            m_status->setText(tr("Provider running — this wallet is currently available to eligible DigiDollar clients."));
        } else if (m_core_ready) {
            m_status->setText(tr("Provider fully configured — review the saved limits, then start it when you are ready."));
        } else {
            m_status->setText(tr("Setup incomplete — no provider service is currently running."));
        }
    }

    void updateExternalPrerequisites()
    {
        const bool node_wait = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_NODE_NOT_READY"));
        const bool txindex_missing = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"));
        const bool txindex_wait = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"));
        const bool activation_wait = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"));

        // Readiness errors not represented by the explicit rows must remain
        // visible. Otherwise every listed prerequisite could appear green
        // while Core is still correctly refusing to start the provider.
        const QStringList represented_errors{
            QStringLiteral("PAYMASTER_NODE_NOT_READY"),
            QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"),
            QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"),
            QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"),
        };
        const bool has_other_requirement = std::any_of(
            m_readiness_errors.cbegin(), m_readiness_errors.cend(),
            [&represented_errors](const QString& error) {
                // Provider setup problems are presented by the task cards and
                // the next-action banner. Do not mislabel them as an external
                // node prerequisite merely because they have no row here.
                return !represented_errors.contains(error) &&
                       !isProviderSetupReadinessError(error) &&
                       !isLiquidityReadinessError(error);
            });
        const bool has_unclassified_failure = std::any_of(
            m_readiness_errors.cbegin(), m_readiness_errors.cend(),
            [&represented_errors](const QString& error) {
                return !represented_errors.contains(error) &&
                       !isProviderSetupReadinessError(error) &&
                       !isLiquidityReadinessError(error);
            });

        setStatusLabel(m_external_blockchain_status,
                       node_wait
                           ? Params().GetChainType() == ChainType::REGTEST
                                 ? tr("Waiting — mine a fresh Regtest block so the node leaves initial synchronization")
                                 : tr("Waiting — blockchain synchronization is not complete")
                                 : tr("Ready"),
                       node_wait ? QStringLiteral("waiting")
                                 : QStringLiteral("ready"));
        setStatusLabel(m_external_txindex_status,
                       txindex_missing
                           ? tr("Action required — transaction index is disabled")
                           : txindex_wait
                                 ? tr("Waiting — transaction index is synchronizing")
                                 : tr("Ready"),
                       txindex_missing ? QStringLiteral("action")
                                       : txindex_wait ? QStringLiteral("waiting")
                                                      : QStringLiteral("ready"));
        setStatusLabel(m_external_broadcast_status,
                       node_wait
                           ? Params().GetChainType() == ChainType::REGTEST
                                 ? tr("Waiting — Regtest has no sufficiently recent confirmed tip")
                                 : tr("Waiting — node is not ready to broadcast transactions")
                                 : tr("Ready"),
                       node_wait ? QStringLiteral("waiting")
                                 : QStringLiteral("ready"));
        setStatusLabel(m_external_activation_status,
                       activation_wait ? tr("Waiting — DigiDollar is not active yet")
                                       : tr("Ready"),
                       activation_wait ? QStringLiteral("waiting")
                                       : QStringLiteral("ready"));

        QString oracle_text;
        QString oracle_kind{QStringLiteral("waiting")};
        if (m_oracle_state == OracleState::AVAILABLE) {
            oracle_text = tr("Available — %1 $USD")
                              .arg(QString::number(
                                  m_oracle_price_micro_usd / 1000000.0,
                                  'f', 6));
            oracle_kind = QStringLiteral("ready");
        } else if (m_oracle_state == OracleState::CHECKING) {
            oracle_text = tr("Checking…");
        } else if (m_oracle_state == OracleState::UNAVAILABLE) {
            oracle_text = tr("Not available yet");
        } else {
            oracle_text = tr("Not checked yet");
        }
        setStatusLabel(m_external_oracle_status, oracle_text, oracle_kind);
        if (m_external_oracle_note) {
            m_external_oracle_note->setVisible(
                m_oracle_state != OracleState::AVAILABLE);
        }

        if (m_external_other_heading && m_external_other_status) {
            m_external_other_heading->setVisible(has_other_requirement);
            m_external_other_status->setVisible(has_other_requirement);
            if (has_other_requirement) {
                setStatusLabel(
                    m_external_other_status,
                    has_unclassified_failure
                        ? tr("Provider cannot start — review the next safe action and technical details")
                        : tr("Action required — complete the provider setup item shown above"),
                    has_unclassified_failure ? QStringLiteral("error")
                                             : QStringLiteral("action"));
            }
        }

        QString card_kind{QStringLiteral("ready")};
        if (has_unclassified_failure) {
            card_kind = QStringLiteral("error");
        } else if (has_other_requirement || txindex_missing) {
            card_kind = QStringLiteral("action");
        } else if (node_wait || txindex_wait || activation_wait) {
            card_kind = QStringLiteral("waiting");
        }
        setStatusKind(m_external_prerequisites_card, card_kind);
    }

    void refreshOracleStatus()
    {
        if (!m_model) return;
        // Keep an already displayed Oracle result stable while refreshing it.
        // Switching AVAILABLE -> CHECKING -> AVAILABLE every ten seconds made
        // the status card resize and visibly flash even when nothing changed.
        if (m_oracle_state == OracleState::UNKNOWN) {
            m_oracle_state = OracleState::CHECKING;
            updateExternalPrerequisites();
        }
        QPointer<PaymasterWidgetImpl> guard{this};
        WalletModel* request_model = m_model;
        const uint64_t wallet_generation = m_wallet_generation;
        const uint64_t oracle_generation = ++m_oracle_request_generation;
        m_model->executeRpcAsync(
            "getoracleprice", UniValue{UniValue::VARR},
            [guard, request_model, wallet_generation,
             oracle_generation](UniValue result, QString error) {
                if (!guard || guard->m_model != request_model ||
                    guard->m_wallet_generation != wallet_generation ||
                    guard->m_oracle_request_generation !=
                        oracle_generation) {
                    return;
                }
                qint64 price{0};
                guard->m_oracle_price_micro_usd =
                    error.isEmpty() &&
                        GetInt64Field(result, "price_micro_usd", price)
                    ? price : 0;
                guard->m_oracle_state = guard->m_oracle_price_micro_usd > 0
                    ? OracleState::AVAILABLE : OracleState::UNAVAILABLE;
                guard->updateExternalPrerequisites();
            });
    }

    static QString dgbAmount(qint64 satoshis)
    {
        return QString::number(satoshis / 100000000.0, 'f', 8);
    }

    static QString ddAmount(qint64 cents)
    {
        return QString::number(cents / 100.0, 'f', 2);
    }

    static qint64 financeNumber(const UniValue& object, const char* name)
    {
        qint64 value{0};
        return GetInt64Field(object, name, value) ? value : 0;
    }

    static QString financeEventKind(const QString& kind)
    {
        if (kind == QLatin1String("transfer")) return tr("Paymaster transfer");
        if (kind == QLatin1String("setup")) return tr("Pool setup");
        if (kind == QLatin1String("replenishment")) return tr("Liquidity replenishment");
        if (kind == QLatin1String("retirement")) return tr("Liquidity retirement");
        if (kind == QLatin1String("withdrawal")) return tr("Carrier withdrawal");
        return tr("Unknown booking");
    }

    static QString financeEventState(const QString& state)
    {
        if (state == QLatin1String("confirmed")) return tr("Confirmed");
        if (state == QLatin1String("pending")) return tr("Pending");
        if (state == QLatin1String("invalidated")) return tr("Reversed");
        return tr("Unknown");
    }

    static QString financeFundingModel(const UniValue& event)
    {
        const UniValue& model = event.find_value("funding_model");
        if (!model.isStr()) return tr("Maintenance");
        const QString funding = QString::fromStdString(model.get_str());
        if (funding == QLatin1String("user_paid")) return tr("User paid");
        const UniValue& scope = event.find_value("sponsorship_scope");
        return scope.isStr() && scope.get_str() == "restricted"
            ? tr("Restricted sponsored") : tr("Public sponsored");
    }

    void updateBackupReminder(bool required, const QString& provider_id)
    {
        m_backup_required = required;
        m_provider_id = provider_id;
        const bool visible = required && !provider_id.isEmpty();
        const QString explanation = tr(
            "Back up this complete provider wallet now. The provider identity key, "
            "configuration, pool state and finance ledger are stored in the wallet. "
            "A seed-only or descriptor-only export is not a complete Paymaster backup. "
            "Keep the wallet backup offline and confidential. This reminder does not "
            "prevent you from starting the provider.");
        const QString identity = tr("Provider ID: %1").arg(
            visibleProviderId(provider_id));
        if (m_overview_backup_notice) {
            m_overview_backup_notice->setVisible(visible);
            m_overview_backup_provider_id->setText(identity);
            m_overview_backup_provider_id->setTextInteractionFlags(
                m_privacy ? Qt::NoTextInteraction
                          : Qt::TextSelectableByKeyboard |
                                Qt::TextSelectableByMouse);
            m_overview_backup_now->setEnabled(!m_privacy && !m_busy);
            m_overview_backup_external->setEnabled(!m_privacy && !m_busy);
        }
        if (m_finance_backup_notice) {
            m_finance_backup_notice->setVisible(visible);
            m_finance_backup_text->setText(explanation);
            m_finance_backup_provider_id->setText(identity);
            m_finance_backup_provider_id->setTextInteractionFlags(
                m_privacy ? Qt::NoTextInteraction
                          : Qt::TextSelectableByKeyboard |
                                Qt::TextSelectableByMouse);
            m_finance_backup_now->setEnabled(!m_privacy && !m_busy);
            m_finance_backup_external->setEnabled(!m_privacy && !m_busy);
        }
        // The setup wizard is a child of this widget while it is open. Hide
        // its completion reminder as soon as the ordinary WalletView backup
        // workflow has successfully updated the wallet-scoped status.
        if (auto* setup_notice =
                findChild<QGroupBox*>(
                    QStringLiteral("paymasterSetupBackupReminder"))) {
            setup_notice->setVisible(visible);
        }
    }

    void requestProviderBackup()
    {
        if (!requirePrivacyOffForSensitiveAction(
                tr("Provider-wallet backup"))) {
            return;
        }
        if (!m_provider_backup_request_handler) {
            QMessageBox::warning(
                this, tr("Provider-wallet backup"),
                tr("The wallet backup action is not available in this window."));
            return;
        }
        m_provider_backup_request_handler();
        // WalletView records a successful full-wallet backup in the same
        // wallet database. Refresh only after its modal file workflow returns.
        QTimer::singleShot(0, this, [this] {
            if (hasRpcTransport() && !m_busy) refreshStatus();
        });
    }

    void acknowledgeExternalBackup()
    {
        if (!requirePrivacyOffForSensitiveAction(
                tr("Provider-wallet backup acknowledgement"))) {
            return;
        }
        if (!hasRpcTransport() || m_busy) {
            m_finance_history_status->setText(!hasRpcTransport()
                ? tr("External-backup acknowledgement was not started because no wallet RPC transport is available.")
                : tr("External-backup acknowledgement was not started because another wallet operation is still running."));
            return;
        }
        const QString prompt = tr(
            "Only acknowledge this if another process has created a current, "
            "complete backup of this wallet file. Seed phrases, descriptor exports, "
            "CSV files and copied addresses do not preserve the complete Paymaster "
            "identity and accounting state.\n\nAcknowledge an external full-wallet backup?");
        if (QMessageBox::warning(
                this, tr("Acknowledge external wallet backup"), prompt,
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        UniValue options{UniValue::VOBJ};
        options.pushKV("external_backup", true);
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        call("acknowledgepaymasterproviderbackup", std::move(params), false,
             nullptr, [this](const UniValue& result) {
                 if (!IsCompleteBackupAcknowledgement(result)) {
                     const QString message = tr(
                         "Core did not confirm a complete external-backup acknowledgement. The reminder remains visible until authoritative status proves that it was cleared.");
                     m_finance_history_status->setText(message);
                     if (!m_rpc_executor_for_testing) {
                         QMessageBox::warning(
                             this, tr("External backup not confirmed"),
                             message);
                     }
                     refreshStatus();
                     return;
                 }
                 const bool backup_required =
                     result.find_value("backup_required").get_bool();
                 updateBackupReminder(backup_required, m_provider_id);
                 QMessageBox::information(
                     this, tr("External backup acknowledged"),
                     backup_required
                         ? tr("The external backup was acknowledged, but Core reports that a newer provider change still requires another complete wallet backup.")
                         : tr("The backup reminder has been cleared for the current provider configuration."));
                 refreshStatus();
             });
    }

    void refreshFinanceStatus()
    {
        if (!hasRpcTransport() || m_busy || !m_finance_period_select) {
            if (m_finance_history_status) {
                m_finance_history_status->setText(!hasRpcTransport()
                    ? tr("Finance refresh was not started because no wallet RPC transport is available.")
                    : m_busy
                        ? tr("Finance refresh was not started because another wallet operation is still running.")
                        : tr("Finance refresh was not started because the reporting-period control is unavailable."));
            }
            return;
        }
        requestSelectedFinanceStatus();
    }

    void requestSelectedFinanceStatus()
    {
        const QString requested_period =
            m_finance_period_select->currentData().toString();
        if (!m_finance_pages.empty()) {
            const UniValue& displayed_period =
                m_finance_pages.front().result.find_value("period");
            if (!displayed_period.isStr() ||
                QString::fromStdString(displayed_period.get_str()) !=
                    requested_period) {
                // Never leave a successfully loaded page visible under a
                // different period selector while its replacement is pending
                // or after that replacement fails validation.
                m_finance_pages.clear();
                m_finance_page_index = -1;
                m_finance_loaded = false;
                m_finance_last_result = UniValue{UniValue::VOBJ};
                m_finance_daily_totals->setRowCount(0);
                m_finance_events->setRowCount(0);
            }
        }
        requestFinancePage(requested_period, {}, /*replace_pages=*/true);
    }

    void requestFinancePage(const QString& requested_period,
                            const QString& cursor, bool replace_pages)
    {
        UniValue options{UniValue::VOBJ};
        options.pushKV("period", requested_period.toStdString());
        options.pushKV("include_events", true);
        options.pushKV("limit", FINANCE_PAGE_SIZE);
        if (!cursor.isEmpty()) options.pushKV("cursor", cursor.toStdString());
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        setFinanceLoading(true, /*exporting=*/false);
        call("getpaymasterfinancestatus", std::move(params), false, nullptr,
             [this, requested_period, cursor,
              replace_pages](const UniValue& result) {
                 if (m_finance_period_select->currentData().toString() !=
                     requested_period) {
                    // Never label an old asynchronous response as the newly
                    // selected period. Queue the authoritative replacement as
                    // this handler's single serialized continuation.
                    setFinanceLoading(false, /*exporting=*/false);
                    requestSelectedFinanceStatus();
                    return;
                 }
                 if (!IsCompleteFinanceStatus(result, requested_period)) {
                     finishFinancePageFailure(tr(
                         "Finance data could not be loaded: Core returned an incomplete or mismatched provider-finance record."));
                     return;
                 }
                 const QString provider_id = QString::fromStdString(
                     result.find_value("provider_id").get_str());
                 if (!replace_pages &&
                     (m_finance_pages.empty() ||
                      QString::fromStdString(
                          m_finance_pages.front().result
                              .find_value("provider_id").get_str()) !=
                          provider_id ||
                      m_finance_pages.back().next_cursor != cursor)) {
                     finishFinancePageFailure(tr(
                         "Finance data could not be loaded: Core returned a page for a different provider or cursor."));
                     return;
                 }

                 FinancePage page;
                 page.result = result;
                 page.request_cursor = cursor;
                 std::set<QString> page_event_ids;
                 for (const UniValue& event :
                      result.find_value("events").getValues()) {
                     const QString event_id = QString::fromStdString(
                         event.find_value("event_id").get_str());
                     if (!page_event_ids.insert(event_id).second) {
                         finishFinancePageFailure(tr(
                             "Finance data could not be loaded: Core returned a duplicate event in one page."));
                         return;
                     }
                     if (!replace_pages) {
                         for (const FinancePage& existing_page :
                              m_finance_pages) {
                             const bool duplicate = std::any_of(
                                 existing_page.events.cbegin(),
                                 existing_page.events.cend(),
                                 [&event_id](const UniValue& existing) {
                                     return QString::fromStdString(
                                         existing.find_value("event_id")
                                             .get_str()) == event_id;
                                 });
                             if (duplicate) {
                                 finishFinancePageFailure(tr(
                                     "Finance data could not be loaded: Core repeated an event across cursor pages."));
                                 return;
                             }
                         }
                     }
                     page.events.push_back(event);
                 }
                 const UniValue& next_cursor =
                     result.find_value("next_cursor");
                 page.next_cursor = next_cursor.isStr()
                     ? QString::fromStdString(next_cursor.get_str())
                     : QString{};
                 if (!page.next_cursor.isEmpty() &&
                     (page.events.empty() ||
                      QString::fromStdString(
                          page.events.back().find_value("event_id").get_str()) !=
                          page.next_cursor ||
                      page.next_cursor == cursor)) {
                     finishFinancePageFailure(tr(
                         "Finance data could not be loaded: Core returned an invalid next-page cursor."));
                     return;
                 }

                 if (replace_pages) {
                     m_finance_pages.clear();
                     m_finance_pages.push_back(std::move(page));
                     m_finance_page_index = 0;
                     m_finance_loaded = true;
                 } else {
                     m_finance_pages.push_back(std::move(page));
                     m_finance_page_index =
                         static_cast<int>(m_finance_pages.size()) - 1;
                 }
                 setFinanceLoading(false, /*exporting=*/false);
                 renderFinancePage(m_finance_page_index);
             }, true, [this, requested_period](const QString& error) {
                 if (m_finance_period_select->currentData().toString() !=
                     requested_period) {
                     setFinanceLoading(false, /*exporting=*/false);
                     requestSelectedFinanceStatus();
                     return;
                 }
                 finishFinancePageFailure(
                     tr("Finance data could not be loaded: %1").arg(error));
             });
    }

    void showPreviousFinancePage()
    {
        if (m_finance_loading || m_privacy || m_finance_page_index <= 0) return;
        renderFinancePage(--m_finance_page_index);
    }

    void showNextFinancePage()
    {
        if (m_finance_loading || m_privacy || m_finance_page_index < 0 ||
            m_finance_page_index >= static_cast<int>(m_finance_pages.size())) {
            return;
        }
        if (m_finance_page_index + 1 <
            static_cast<int>(m_finance_pages.size())) {
            renderFinancePage(++m_finance_page_index);
            return;
        }
        const QString cursor =
            m_finance_pages.at(m_finance_page_index).next_cursor;
        if (!cursor.isEmpty()) {
            requestFinancePage(
                m_finance_period_select->currentData().toString(), cursor,
                /*replace_pages=*/false);
        }
    }

    void setFinanceLoading(bool loading, bool exporting)
    {
        m_finance_loading = loading;
        m_finance_export_loading = loading && exporting;
        m_finance_refresh->setText(
            loading && !exporting ? tr("Loading…") : tr("Refresh"));
        m_finance_export->setText(
            loading && exporting ? tr("Preparing export…")
                                 : tr("Export CSV…"));
        updateFinanceControls();
    }

    void updateFinanceControls()
    {
        const bool has_page = m_finance_page_index >= 0 &&
            m_finance_page_index < static_cast<int>(m_finance_pages.size());
        m_finance_period_select->setEnabled(!m_finance_loading && !m_busy);
        m_finance_refresh->setEnabled(!m_finance_loading && !m_busy);
        m_finance_export->setEnabled(
            m_finance_loaded && !m_finance_loading && !m_privacy &&
            !m_busy);
        m_finance_previous_page->setEnabled(
            has_page && !m_finance_loading && !m_privacy && !m_busy &&
            m_finance_page_index > 0);
        const bool cached_next = has_page && m_finance_page_index + 1 <
            static_cast<int>(m_finance_pages.size());
        const bool remote_next = has_page &&
            !m_finance_pages.at(m_finance_page_index).next_cursor.isEmpty();
        m_finance_next_page->setEnabled(
            !m_finance_loading && !m_privacy && !m_busy &&
            (cached_next || remote_next));

        if (m_finance_export_loading) {
            m_finance_page_status->setText(
                tr("Loading all pages for export…"));
        } else if (m_privacy) {
            m_finance_page_status->setText(
                tr("Booking details hidden by privacy mode"));
        } else if (!has_page) {
            m_finance_page_status->setText(tr("No booking page loaded"));
        } else {
            const int rows = static_cast<int>(
                m_finance_pages.at(m_finance_page_index).events.size());
            m_finance_page_status->setText(
                tr("Page %1 · %2 booking(s)")
                    .arg(m_finance_page_index + 1)
                    .arg(rows));
        }
    }

    void finishFinancePageFailure(const QString& message)
    {
        setFinanceLoading(false, /*exporting=*/false);
        m_finance_history_status->setText(message);
    }

    void renderFinancePage(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_finance_pages.size())) {
            return;
        }
        m_finance_page_index = index;
        const FinancePage& page = m_finance_pages.at(index);
        applyFinanceStatus(page.result, page.events);
        updateFinanceControls();
    }

    void applyFinanceStatus(const UniValue& result,
                            const std::vector<UniValue>& events)
    {
        if (!result.isObject()) return;
        m_finance_last_result = result;
        const UniValue& summaries = result.find_value("period_summaries");
        const std::array<const char*, 4> keys{{"today", "7d", "30d", "all"}};
        for (size_t index = 0; index < keys.size(); ++index) {
            const UniValue& summary = summaries.find_value(keys.at(index));
            const qint64 income = financeNumber(summary, "service_fee_income_cents");
            const qint64 cost = financeNumber(summary, "dgb_operating_cost_satoshis");
            const qint64 transfers = financeNumber(summary, "successful_transfers");
            m_finance_period_income.at(index)->setText(maskNumericText(
                tr("Service fees: %1 DD").arg(ddAmount(income))));
            m_finance_period_cost.at(index)->setText(maskNumericText(
                tr("Operating costs: %1 DGB").arg(dgbAmount(cost))));
            m_finance_period_transfers.at(index)->setText(maskNumericText(
                tr("Successful transfers: %1").arg(transfers)));
        }

        const qint64 income = financeNumber(result, "service_fee_income_cents");
        const qint64 cost = financeNumber(result, "dgb_operating_cost_satoshis");
        const qint64 transfers = financeNumber(result, "successful_transfers");
        const qint64 average = financeNumber(result, "average_service_fee_cents");
        const UniValue& estimate = result.find_value("estimated_result_usd");
        const UniValue& oracle = result.find_value("oracle_price_micro_usd");
        const UniValue& valuation_time = result.find_value("valuation_time");
        if (estimate.isNum() && oracle.isNum()) {
            const QString time = valuation_time.isNum()
                ? QDateTime::fromSecsSinceEpoch(
                      valuation_time.getInt<qint64>(), Qt::UTC)
                      .toString(Qt::ISODate)
                : tr("now");
            m_finance_result_estimate->setText(maskNumericText(tr(
                "Selected period: %1 DD income minus %2 DGB cost ≈ %3 USD "
                "at %4 USD/DGB (valuation %5).")
                .arg(ddAmount(income), dgbAmount(cost))
                .arg(estimate.get_real(), 0, 'f', 2)
                .arg(oracle.getInt<qint64>() / 1000000.0, 0, 'f', 6)
                .arg(time)));
        } else {
            m_finance_result_estimate->setText(maskNumericText(tr(
                "Selected period: %1 DD income and %2 DGB cost. No current "
                "Oracle price is available, so no USD estimate is shown.")
                .arg(ddAmount(income), dgbAmount(cost))));
        }
        const UniValue& model_breakdown = result.find_value(
            "model_breakdown");
        const auto model_line = [&](const QString& title,
                                    const char* key) {
            const UniValue& model = model_breakdown.find_value(key);
            return tr("%1: %2 transfer(s) · %3 DD income · %4 DGB cost")
                .arg(title)
                .arg(financeNumber(model, "successful_transfers"))
                .arg(ddAmount(financeNumber(
                    model, "service_fee_income_cents")))
                .arg(dgbAmount(financeNumber(
                    model, "dgb_operating_cost_satoshis")));
        };
        m_finance_model_breakdown->setText(maskNumericText(
            tr("%1 successful transfer(s) · average service-fee income %2 DD\n%3\n%4\n%5")
                .arg(transfers)
                .arg(ddAmount(average))
                .arg(model_line(tr("User paid"), "user_paid"))
                .arg(model_line(tr("Public sponsored"),
                                "public_sponsored"))
                .arg(model_line(tr("Restricted sponsored"),
                                "restricted_sponsored"))));

        const UniValue& pool = result.find_value("pool_capital");
        m_finance_pool_dgb->setText(maskNumericText(tr(
            "DGB capacity: %1 available · %2 reserved or committed · %3 pending confirmation")
            .arg(dgbAmount(financeNumber(pool, "dgb_available_satoshis")),
                 dgbAmount(financeNumber(pool, "dgb_reserved_satoshis")),
                 dgbAmount(financeNumber(pool, "dgb_pending_satoshis")))));
        m_finance_pool_carrier->setText(maskNumericText(tr(
            "DD carriers: %1 DD base capital · %2 DD earned above the base · %3 DD currently withdrawable")
            .arg(ddAmount(financeNumber(pool, "carrier_base_cents")),
                 ddAmount(financeNumber(pool, "carrier_earned_cents")),
                 ddAmount(financeNumber(pool, "carrier_withdrawable_cents")))));
        const qint64 pending = financeNumber(
            pool, "pending_maintenance_transactions");
        m_finance_pending_maintenance->setText(maskNumericText(
            pending == 0
                ? tr("No pool-maintenance or withdrawal transaction is pending.")
                : tr("%1 pool-maintenance or withdrawal transaction(s) are pending.")
                      .arg(pending)));

        const bool partial = result.find_value(
            "history_partially_reconstructable").isBool() &&
            result.find_value("history_partially_reconstructable").get_bool();
        const qint64 complete_from = financeNumber(
            result, "history_complete_from");
        const QString complete_date = complete_from > 0
            ? QDateTime::fromSecsSinceEpoch(complete_from, Qt::UTC)
                  .toString(Qt::ISODate)
            : tr("unknown");
        m_finance_history_notice->setVisible(partial);
        if (partial) {
            m_finance_history_notice_text->setText(maskNumericText(tr(
                "The totals below are complete only from %1 UTC. Earlier Paymaster transfers may be absent because their exact provider fee and transaction role cannot be proven from the remaining wallet records. New transfers are recorded automatically; no earlier income is estimated.")
                                                       .arg(complete_date)));
        }
        m_finance_history_status->setText(m_privacy
            ? tr("Finance totals and booking details are hidden by privacy mode.")
            : partial
                  ? tr("History is complete since %1 UTC. Earlier unprovable transfers are excluded from every total and export.")
                        .arg(complete_date)
                  : tr("History is complete since %1 UTC.").arg(complete_date));

        const UniValue& daily_totals = result.find_value("daily_totals");
        m_finance_daily_totals->setRowCount(
            daily_totals.isArray()
                ? static_cast<int>(daily_totals.size()) : 0);
        if (daily_totals.isArray()) {
            int row{0};
            for (const UniValue& total : daily_totals.getValues()) {
                const qint64 day_start = financeNumber(total, "day_start");
                const std::array<QString, 5> cells{{
                    QDateTime::fromSecsSinceEpoch(day_start, Qt::UTC)
                        .date().toString(Qt::ISODate),
                    ddAmount(financeNumber(
                        total, "service_fee_income_cents")),
                    dgbAmount(financeNumber(
                        total, "dgb_operating_cost_satoshis")),
                    QString::number(financeNumber(
                        total, "successful_transfers")),
                    QString::number(financeNumber(
                        total, "maintenance_transactions")),
                }};
                for (int column = 0;
                     column < static_cast<int>(cells.size()); ++column) {
                    m_finance_daily_totals->setItem(
                        row, column,
                        new QTableWidgetItem(cells.at(column)));
                }
                ++row;
            }
        }

        m_finance_events->setRowCount(static_cast<int>(events.size()));
        {
            int row{0};
            for (const UniValue& event : events) {
                const qint64 created_at = financeNumber(event, "created_at");
                const QString kind = event.find_value("kind").isStr()
                    ? QString::fromStdString(event.find_value("kind").get_str())
                    : QString{};
                const QString state = event.find_value("state").isStr()
                    ? QString::fromStdString(event.find_value("state").get_str())
                    : QString{};
                const std::array<QString, 6> cells{{
                    QDateTime::fromSecsSinceEpoch(created_at, Qt::UTC)
                        .toString(Qt::ISODate),
                    financeEventKind(kind), financeEventState(state),
                    ddAmount(financeNumber(event, "dd_income_cents")),
                    dgbAmount(financeNumber(event, "dgb_cost_satoshis")),
                    financeFundingModel(event),
                }};
                for (int column = 0; column < static_cast<int>(cells.size()); ++column) {
                    m_finance_events->setItem(
                        row, column, new QTableWidgetItem(cells.at(column)));
                }
                ++row;
            }
        }
        m_finance_daily_totals->setVisible(!m_privacy);
        m_finance_events->setVisible(!m_privacy);
        const UniValue& provider = result.find_value("provider_id");
        const QString provider_id = provider.isStr()
            ? QString::fromStdString(provider.get_str()) : m_provider_id;
        const bool backup_required = result.find_value("backup_required").isBool() &&
                                     result.find_value("backup_required").get_bool();
        updateBackupReminder(backup_required, provider_id);
    }

    static QString csvCell(QString value)
    {
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"") + value + QStringLiteral("\"");
    }

    void exportFinanceCsv()
    {
        if (m_privacy) {
            QMessageBox::information(
                this, tr("Export provider accounting"),
                tr("Disable privacy mode before exporting provider accounting."));
            return;
        }
        if (!m_finance_loaded || m_finance_pages.empty()) {
            QMessageBox::information(
                this, tr("Export provider accounting"),
                tr("Refresh the selected reporting period before exporting."));
            return;
        }
        QString filename = m_finance_export_filename_for_testing;
        if (filename.isEmpty()) {
            if (QMessageBox::information(
                    this, tr("Export provider accounting"),
                    tr("This CSV contains accounting figures for the selected period. "
                       "It is not a wallet backup and cannot restore the provider identity, "
                       "keys, pool state or ledger."),
                    QMessageBox::Ok | QMessageBox::Cancel,
                    QMessageBox::Ok) != QMessageBox::Ok) {
                return;
            }
            filename = GUIUtil::getSaveFileName(
                this, tr("Export Paymaster accounting"), QString(),
                tr("Comma-separated values") + QLatin1String(" (*.csv)"), nullptr);
        }
        if (filename.isEmpty()) return;

        auto state = std::make_shared<FinanceExportState>();
        state->filename = filename;
        state->period = m_finance_period_select->currentData().toString();
        setFinanceLoading(true, /*exporting=*/true);
        m_finance_history_status->setText(tr(
            "Loading every booking page for a complete export…"));
        requestFinanceExportPage(state, {});
    }

    void requestFinanceExportPage(
        const std::shared_ptr<FinanceExportState>& state,
        const QString& cursor)
    {
        if (!cursor.isEmpty() &&
            !state->requested_cursors.insert(cursor).second) {
            finishFinanceExportFailure(tr(
                "Export stopped: Core repeated a finance-page cursor. No file was written."));
            return;
        }

        UniValue options{UniValue::VOBJ};
        options.pushKV("period", state->period.toStdString());
        options.pushKV("include_events", true);
        options.pushKV("limit", FINANCE_PAGE_SIZE);
        if (!cursor.isEmpty()) options.pushKV("cursor", cursor.toStdString());
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        call("getpaymasterfinancestatus", std::move(params), false, nullptr,
             [this, state, cursor](const UniValue& result) {
                 if (m_privacy) {
                     finishFinanceExportFailure(tr(
                         "Export stopped because privacy mode was enabled. No file was written."));
                     return;
                 }
                 if (!IsCompleteFinanceStatus(result, state->period)) {
                     finishFinanceExportFailure(tr(
                         "Export stopped: Core returned an incomplete or mismatched provider-finance page. No file was written."));
                     return;
                 }
                 const QString provider_id = QString::fromStdString(
                     result.find_value("provider_id").get_str());
                 if (state->provider_id.isEmpty()) {
                     state->provider_id = provider_id;
                     state->summary = result;
                 } else if (state->provider_id != provider_id) {
                     finishFinanceExportFailure(tr(
                         "Export stopped: the provider identity changed between finance pages. No file was written."));
                     return;
                 }

                 const UniValue& page_events = result.find_value("events");
                 for (const UniValue& event : page_events.getValues()) {
                     const QString event_id = QString::fromStdString(
                         event.find_value("event_id").get_str());
                     if (!state->event_ids.insert(event_id).second) {
                         finishFinanceExportFailure(tr(
                             "Export stopped: Core repeated an event across finance pages. No file was written."));
                         return;
                     }
                     state->events.push_back(event);
                 }

                 const UniValue& next_value =
                     result.find_value("next_cursor");
                 const QString next_cursor = next_value.isStr()
                     ? QString::fromStdString(next_value.get_str())
                     : QString{};
                 if (!next_cursor.isEmpty() &&
                     (page_events.empty() ||
                      QString::fromStdString(
                          page_events.getValues().back()
                              .find_value("event_id").get_str()) != next_cursor ||
                      next_cursor == cursor ||
                      state->requested_cursors.count(next_cursor) != 0)) {
                     finishFinanceExportFailure(tr(
                         "Export stopped: Core returned an invalid next-page cursor. No file was written."));
                     return;
                 }

                 m_finance_history_status->setText(tr(
                     "Preparing complete export… %1 booking(s) loaded")
                     .arg(static_cast<qulonglong>(state->events.size())));
                 if (!next_cursor.isEmpty()) {
                     const uint64_t wallet_generation =
                         m_wallet_generation;
                     // Yield between pages even with the synchronous widget-
                     // test executor. Large exports therefore stay responsive
                     // and never build a recursive handler stack.
                     QTimer::singleShot(
                         0, this,
                         [this, state, next_cursor, wallet_generation] {
                             if (m_wallet_generation != wallet_generation) {
                                 return;
                             }
                             if (m_privacy) {
                                 finishFinanceExportFailure(tr(
                                     "Export stopped because privacy mode was enabled. No file was written."));
                                 return;
                             }
                             requestFinanceExportPage(state, next_cursor);
                         });
                     return;
                 }

                 setFinanceLoading(false, /*exporting=*/true);
                 writeFinanceCsv(state->filename, state->summary,
                                 state->events);
             }, /*show_error=*/false,
             [this](const QString& error) {
                 finishFinanceExportFailure(tr(
                     "Export stopped while loading finance history: %1. No file was written.")
                     .arg(error));
             });
    }

    void finishFinanceExportFailure(const QString& message)
    {
        setFinanceLoading(false, /*exporting=*/true);
        m_finance_history_status->setText(message);
    }

    void writeFinanceCsv(const QString& filename, const UniValue& summary,
                         const std::vector<UniValue>& events)
    {
        QSaveFile file(filename);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            m_finance_history_status->setText(tr(
                "Complete export could not be opened for writing. No destination file was replaced."));
            showPlainTextWarning(
                this, tr("Export failed"),
                tr("Could not open %1 for writing.").arg(filename));
            return;
        }
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        const UniValue& oracle = summary.find_value("oracle_price_micro_usd");
        const bool include_valuation = oracle.isNum();
        stream << "utc_time,booking,status,dd_income,dgb_cost,payment_model";
        if (include_valuation) stream << ",current_value_usd";
        stream << "\n";
        for (const UniValue& event : events) {
            const qint64 created_at = financeNumber(event, "created_at");
            const qint64 dd_income = financeNumber(event, "dd_income_cents");
            const qint64 dgb_cost = financeNumber(event, "dgb_cost_satoshis");
            const QString kind = event.find_value("kind").isStr()
                ? QString::fromStdString(event.find_value("kind").get_str())
                : QString{};
            const QString state = event.find_value("state").isStr()
                ? QString::fromStdString(event.find_value("state").get_str())
                : QString{};
            stream << csvCell(QDateTime::fromSecsSinceEpoch(
                                  created_at, Qt::UTC).toString(Qt::ISODate))
                   << ',' << csvCell(financeEventKind(kind))
                   << ',' << csvCell(financeEventState(state))
                   << ',' << ddAmount(dd_income)
                   << ',' << dgbAmount(dgb_cost)
                   << ',' << csvCell(financeFundingModel(event));
            if (include_valuation) {
                const long double current_value =
                    static_cast<long double>(dd_income) / 100.0L -
                    static_cast<long double>(dgb_cost) *
                        oracle.getInt<qint64>() / 100000000.0L / 1000000.0L;
                stream << ',' << QString::number(
                    static_cast<double>(current_value), 'f', 8);
            }
            stream << '\n';
        }
        if (!file.commit()) {
            m_finance_history_status->setText(tr(
                "Complete export could not be committed atomically. No partial destination file was accepted."));
            showPlainTextWarning(
                this, tr("Export failed"),
                tr("Could not finish writing %1.").arg(filename));
            return;
        }
        m_finance_history_status->setText(
            tr("Complete export written: %1 booking(s).")
                .arg(static_cast<qulonglong>(events.size())));
        if (m_finance_export_filename_for_testing.isEmpty()) {
            QMessageBox::information(
                this, tr("Export complete"),
                tr("The selected accounting period was exported successfully."));
        }
    }

    void openLiquidityFinanceControl(QWidget* control)
    {
        m_tabs->setCurrentWidget(m_liquidity_page);
        if (m_liquidity_advanced_toggle &&
            !m_liquidity_advanced_toggle->isChecked()) {
            m_liquidity_advanced_toggle->setChecked(true);
        }
        if (control) {
            control->setFocus(Qt::OtherFocusReason);
            if (auto* scroll = qobject_cast<QScrollArea*>(m_liquidity_page)) {
                scroll->ensureWidgetVisible(control, 24, 24);
            }
        }
    }

    void reviewMaintenanceFeeLimit()
    {
        m_tabs->setCurrentWidget(m_liquidity_page);
        if (m_liquidity_advanced_toggle) {
            m_liquidity_advanced_toggle->setChecked(true);
        }
        if (m_maintenance_limits_toggle) {
            m_maintenance_limits_toggle->setChecked(true);
        }
        if (m_maintenance_fee_per_transaction) {
            m_maintenance_fee_per_transaction->setFocus(
                Qt::OtherFocusReason);
        }
        m_liquidity_policy_status->setText(tr(
            "Automatic refill is paused. Its estimated network fee is above the saved per-transaction maintenance limit, so no transaction has been created. Review all three finite limits and increase them only if the cost is acceptable; the new limits do not become active until you confirm and save them."));
    }

    bool readLiquidityPolicy(LiquidityPolicyValues& values) const
    {
        bool ok_transaction{false};
        bool ok_hour{false};
        bool ok_day{false};
        values.automatic_replenishment =
            m_automatic_replenishment->isChecked();
        values.paid_maintenance_approved =
            m_paid_maintenance_approved->isChecked();
        values.admission_dgb = m_admission_dgb->value();
        values.operational_dgb = m_operational_dgb->value();
        values.admission_carriers = m_admission_carriers->value();
        values.operational_carriers = m_operational_carriers->value();
        values.fee_per_transaction =
            m_maintenance_fee_per_transaction->text().toLongLong(
                &ok_transaction);
        values.fee_per_hour =
            m_maintenance_fee_per_hour->text().toLongLong(&ok_hour);
        values.fee_per_day =
            m_maintenance_fee_per_day->text().toLongLong(&ok_day);
        return ok_transaction && ok_hour && ok_day;
    }

    static UniValue liquidityPolicyToJSON(
        const LiquidityPolicyValues& values)
    {
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("automatic_replenishment",
                      values.automatic_replenishment);
        policy.pushKV("paid_maintenance_approved",
                      values.paid_maintenance_approved);
        policy.pushKV("target_admission_dgb", values.admission_dgb);
        policy.pushKV("target_operational_dgb", values.operational_dgb);
        policy.pushKV("target_admission_carriers",
                      values.admission_carriers);
        policy.pushKV("target_operational_carriers",
                      values.operational_carriers);
        policy.pushKV(
            "maximum_maintenance_fee_per_transaction_satoshis",
            values.fee_per_transaction);
        policy.pushKV("maximum_maintenance_fee_per_hour_satoshis",
                      values.fee_per_hour);
        policy.pushKV("maximum_maintenance_fee_per_day_satoshis",
                      values.fee_per_day);
        return policy;
    }

    bool liquidityPolicyCanRepresent(const UniValue& policy) const
    {
        qint64 admission_dgb{0};
        qint64 operational_dgb{0};
        qint64 admission_carriers{0};
        qint64 operational_carriers{0};
        qint64 fee_per_transaction{0};
        qint64 fee_per_hour{0};
        qint64 fee_per_day{0};
        return IsCompleteLiquidityPolicy(policy) &&
            GetInt64Field(policy, "target_admission_dgb",
                          admission_dgb) &&
            GetInt64Field(policy, "target_operational_dgb",
                          operational_dgb) &&
            GetInt64Field(policy, "target_admission_carriers",
                          admission_carriers) &&
            GetInt64Field(policy, "target_operational_carriers",
                          operational_carriers) &&
            GetInt64Field(
                policy,
                "maximum_maintenance_fee_per_transaction_satoshis",
                fee_per_transaction) &&
            GetInt64Field(
                policy,
                "maximum_maintenance_fee_per_hour_satoshis",
                fee_per_hour) &&
            GetInt64Field(
                policy,
                "maximum_maintenance_fee_per_day_satoshis",
                fee_per_day) &&
            spinCanRepresent(m_admission_dgb, admission_dgb) &&
            spinCanRepresent(m_operational_dgb, operational_dgb) &&
            spinCanRepresent(m_admission_carriers,
                             admission_carriers) &&
            spinCanRepresent(m_operational_carriers,
                             operational_carriers) &&
            fee_per_transaction >= 0 && fee_per_hour >= 0 &&
            fee_per_day >= 0;
    }

    void setLiquidityPolicyMutationEnabled(bool enabled)
    {
        const bool controls_enabled = enabled && !m_busy;
        m_automatic_replenishment->setEnabled(controls_enabled);
        m_paid_maintenance_approved->setEnabled(controls_enabled);
        for (QSpinBox* control : {
                 m_admission_dgb, m_operational_dgb,
                 m_admission_carriers, m_operational_carriers}) {
            control->setEnabled(controls_enabled);
        }
        for (QLineEdit* control : {
                 m_maintenance_fee_per_transaction,
                 m_maintenance_fee_per_hour,
                 m_maintenance_fee_per_day}) {
            control->setReadOnly(!controls_enabled);
        }
        if (m_restore_liquidity_defaults) {
            m_restore_liquidity_defaults->setEnabled(controls_enabled);
        }
        if (m_save_liquidity_policy) {
            m_save_liquidity_policy->setEnabled(enabled && !m_busy);
        }
        if (m_save_liquidity_policy_primary) {
            m_save_liquidity_policy_primary->setEnabled(
                enabled && !m_busy);
        }
    }

    void loadLiquidityPolicy(const UniValue& policy, bool configured)
    {
        if (!liquidityPolicyCanRepresent(policy)) return;
        m_loading_liquidity_policy = true;
        const UniValue& automatic =
            policy.find_value("automatic_replenishment");
        const UniValue& approved =
            policy.find_value("paid_maintenance_approved");
        if (automatic.isBool()) {
            m_automatic_replenishment->setChecked(automatic.get_bool());
        }
        if (approved.isBool()) {
            m_paid_maintenance_approved->setChecked(approved.get_bool());
        }
        const auto load_spin = [&policy](QSpinBox* control,
                                         const char* name) {
            const UniValue& value = policy.find_value(name);
            if (value.isNum()) control->setValue(value.getInt<int>());
        };
        load_spin(m_admission_dgb, "target_admission_dgb");
        load_spin(m_operational_dgb, "target_operational_dgb");
        load_spin(m_admission_carriers, "target_admission_carriers");
        load_spin(m_operational_carriers, "target_operational_carriers");
        const auto load_amount = [&policy](QLineEdit* control,
                                           const char* name) {
            const UniValue& value = policy.find_value(name);
            if (value.isNum()) {
                control->setText(QString::number(value.getInt<qint64>()));
            }
        };
        load_amount(m_maintenance_fee_per_transaction,
                    "maximum_maintenance_fee_per_transaction_satoshis");
        load_amount(m_maintenance_fee_per_hour,
                    "maximum_maintenance_fee_per_hour_satoshis");
        load_amount(m_maintenance_fee_per_day,
                    "maximum_maintenance_fee_per_day_satoshis");
        m_loading_liquidity_policy = false;
        m_liquidity_policy_configured = configured;
        m_liquidity_policy_dirty = false;
        updateLiquidityDisplay();
        m_liquidity_policy_status->setText(configured
            ? approved.isBool() && approved.get_bool()
                ? tr("Automatic liquidity policy saved. Paid replenishment is permitted only within the displayed finite limits.")
                : tr("Liquidity targets are saved, but paid replenishment remains disabled. Free successor recycling still works.")
            : tr("Suggested migration values are shown. Review them and explicitly approve finite paid maintenance before Core can create a replenishment transaction."));
    }

    void markLiquidityPolicyDirty()
    {
        if (m_loading_liquidity_policy) return;
        m_liquidity_policy_dirty = true;
        invalidatePoolPreviews();
        invalidateCarrierWithdrawalPreviews();
        if (m_liquidity_policy_status) {
            m_liquidity_policy_status->setText(tr(
                "Unsaved liquidity-policy changes. The previously saved targets and limits remain active until these values are saved."));
        }
        updateLiquidityDisplay();
        updateProviderButtons();
    }

    QString liquidityApprovalText(
        const LiquidityPolicyValues& values) const
    {
        const qint64 admission_value =
            DigiDollar::Paymaster::MIN_ADMISSION_DGB_SATOSHIS;
        const qint64 operational_value = std::max<qint64>(
            admission_value, m_network_fee->value());
        const qint64 bound_dgb =
            admission_value * values.admission_dgb +
            operational_value * values.operational_dgb;
        const qint64 bound_carriers =
            100LL * (values.admission_carriers +
                     values.operational_carriers);
        return tr(
            "Approve automatic paid liquidity maintenance for this wallet?\n\n"
            "Targets: %1 admission DGB, %2 operational DGB, %3 admission carriers and %4 operational carriers.\n"
            "Approximate target capital at the current provider ceiling: %5 DGB and %6 DD carrier base.\n\n"
            "Maintenance network-fee limits:\n"
            "• %7 DGB per transaction\n"
            "• %8 DGB per rolling hour\n"
            "• %9 DGB per rolling day\n\n"
            "Core can spend maintenance fees only while automatic provider operation or autostart is active. Zero never means unlimited.")
            .arg(values.admission_dgb)
            .arg(values.operational_dgb)
            .arg(values.admission_carriers)
            .arg(values.operational_carriers)
            .arg(dgbAmount(bound_dgb))
            .arg(QString::number(bound_carriers / 100.0, 'f', 2))
            .arg(dgbAmount(values.fee_per_transaction))
            .arg(dgbAmount(values.fee_per_hour))
            .arg(dgbAmount(values.fee_per_day));
    }

    void saveLiquidityPolicy()
    {
        if (!m_liquidity_snapshot_representable) {
            m_liquidity_policy_status->setText(tr(
                "Liquidity settings were not changed: the persisted Core policy cannot be represented exactly by this interface."));
            return;
        }
        if (!hasRpcTransport() || m_busy) {
            m_liquidity_policy_status->setText(!hasRpcTransport()
                ? tr("Liquidity settings were not saved because no wallet RPC transport is available.")
                : tr("Liquidity settings were not saved because another wallet operation is still running."));
            return;
        }
        LiquidityPolicyValues values;
        if (!readLiquidityPolicy(values)) {
            showPlainTextWarning(
                this, tr("Automatic liquidity policy"),
                tr("One or more maintenance-fee amounts are outside the supported whole-satoshi range."));
            return;
        }
        if (values.paid_maintenance_approved &&
            !requirePrivacyOffForSensitiveAction(
                tr("Approving paid liquidity maintenance"))) {
            return;
        }
        if (values.paid_maintenance_approved &&
            askPlainTextQuestion(
                this, tr("Approve finite paid maintenance"),
                liquidityApprovalText(values)) != QMessageBox::Yes) {
            return;
        }
        const UniValue requested_policy = liquidityPolicyToJSON(values);
        UniValue params{UniValue::VARR};
        params.push_back(requested_policy);
        call("setpaymasterliquiditypolicy", std::move(params), false,
             nullptr, [this, requested_policy](const UniValue& result) {
                 if (!IsExactLiquidityPolicyAcknowledgement(
                         requested_policy, result)) {
                     m_liquidity_policy_status->setText(tr(
                         "Core did not confirm the exact automatic liquidity policy. The displayed edit remains unsaved; refresh before retrying."));
                     updateLiquidityDisplay();
                     return;
                 }
                 loadLiquidityPolicy(result, /*configured=*/true);
                 m_liquidity_policy_status->setText(tr(
                     "Automatic liquidity policy saved successfully. The provider still cannot spend beyond its separate provider safety limits."));
                 invalidateCarrierWithdrawalPreviews();
                 refreshStatus();
             });
    }

    void approveSuggestedLiquidityMaintenance()
    {
        if (!hasRpcTransport() || m_busy) {
            m_liquidity_policy_status->setText(!hasRpcTransport()
                ? tr("Liquidity maintenance review was not started because no wallet RPC transport is available.")
                : tr("Liquidity maintenance review was not started because another wallet operation is still running."));
            return;
        }
        m_tabs->setCurrentWidget(m_liquidity_page);
        if (!m_liquidity_targets_satisfy_provider_policy) {
            // Restore only the visible minimums here. The operator must still
            // review and save the policy explicitly; this button never creates
            // a pool transaction or silently reverses an intentional slot
            // release.
            if (m_liquidity_advanced_toggle) {
                m_liquidity_advanced_toggle->setChecked(true);
            }
            m_admission_carriers->setValue(
                std::max(3, m_admission_carriers->value()));
            m_operational_carriers->setValue(
                std::max(1, m_operational_carriers->value()));
            // One explicit save can now persist both the corrected targets and
            // the operator's finite paid-maintenance approval. Keep the limits
            // expanded so the financial authority being granted is visible
            // before the confirmation dialog appears.
            m_paid_maintenance_approved->setChecked(true);
            if (m_maintenance_limits_toggle) {
                m_maintenance_limits_toggle->setChecked(true);
            }
            m_liquidity_policy_status->setText(tr(
                "The required carrier targets and finite refill limits are now shown. Review them and choose Save targets and approve refill. No funds have moved."));
            return;
        }
        m_paid_maintenance_approved->setChecked(true);
        saveLiquidityPolicy();
    }

    static QString slotStatusText(const QString& name,
                                  const UniValue& status)
    {
        if (!status.isObject()) return {};
        return QStringLiteral("%1: %2 ready, %3 pending, %4 missing (target %5)")
            .arg(name)
            .arg(poolNumber(status, "ready"))
            .arg(poolNumber(status, "pending"))
            .arg(poolNumber(status, "missing"))
            .arg(poolNumber(status, "target"));
    }

    void applyLiquidityStatus(const UniValue& result)
    {
        const UniValue& policy = result.find_value("policy");
        const bool configured =
            result.find_value("policy_configured").isBool() &&
            result.find_value("policy_configured").get_bool();
        const bool representable = liquidityPolicyCanRepresent(policy);
        m_liquidity_snapshot_representable = representable;
        m_liquidity_snapshot_available = representable;
        if (!representable) {
            // Keep the complete Core object separate from the controls. A
            // QSpinBox would clamp future or otherwise out-of-range target
            // values and could make an innocent Save overwrite wallet state.
            m_unrepresentable_liquidity_snapshot = policy;
            m_liquidity_policy_configured = false;
            m_liquidity_policy_dirty = false;
            setLiquidityPolicyMutationEnabled(false);
            m_liquidity_policy_status->setText(tr(
                "Core returned a complete liquidity policy containing a value that this interface cannot represent exactly. The persisted policy was retained unchanged and all liquidity-policy mutation is disabled."));
        } else if (policy.isObject() && !m_liquidity_policy_dirty) {
            m_unrepresentable_liquidity_snapshot =
                UniValue{UniValue::VOBJ};
            setLiquidityPolicyMutationEnabled(true);
            loadLiquidityPolicy(policy, configured);
        } else {
            m_liquidity_policy_configured = configured;
        }
        const UniValue& state = result.find_value("maintenance_state");
        m_maintenance_state = state.isStr()
            ? QString::fromStdString(state.get_str())
            : QStringLiteral("unavailable");
        const UniValue& targets_satisfy =
            result.find_value("targets_satisfy_provider_policy");
        // Treat the field as true when talking to an older test fixture, but
        // use the explicit Core result whenever it is present. Core owns this
        // invariant because a locally valid zero carrier target can still be
        // insufficient for an active USER_PAID offer.
        m_liquidity_targets_satisfy_provider_policy =
            !targets_satisfy.isBool() || targets_satisfy.get_bool();

        const QStringList slot_lines{
            slotStatusText(tr("Admission DGB"),
                           result.find_value("admission_dgb")),
            slotStatusText(tr("Operational DGB"),
                           result.find_value("operational_dgb")),
            slotStatusText(tr("Admission carriers"),
                           result.find_value("admission_carriers")),
            slotStatusText(tr("Operational carriers"),
                           result.find_value("operational_carriers")),
        };
        m_liquidity_slot_status->setText(slot_lines.join('\n'));
        // The liquidity RPC is the authoritative source for the current
        // ready/pending/missing breakdown. Keep the Overview counters in the
        // same snapshot so an out-of-order getpaymasterinfo reply cannot make
        // one card report a missing slot while another treats the target as
        // satisfied. Pending outputs intentionally do not count as ready.
        const UniValue& admission_dgb = result.find_value("admission_dgb");
        const UniValue& operational_dgb = result.find_value("operational_dgb");
        const UniValue& admission_carriers =
            result.find_value("admission_carriers");
        const UniValue& operational_carriers =
            result.find_value("operational_carriers");
        if (admission_dgb.isObject() && operational_dgb.isObject() &&
            admission_carriers.isObject() && operational_carriers.isObject()) {
            m_pool_status_loaded = true;
            m_pool_admission_dgb = poolNumber(admission_dgb, "ready");
            m_pool_operational_dgb = poolNumber(operational_dgb, "ready");
            m_pool_admission_carriers =
                poolNumber(admission_carriers, "ready");
            m_pool_operational_carriers =
                poolNumber(operational_carriers, "ready");
        }
        const auto total_field = [&result](const char* object_name,
                                           const char* field_name) {
            return poolNumber(result.find_value(object_name), field_name);
        };
        const qint64 missing =
            total_field("admission_dgb", "missing") +
            total_field("operational_dgb", "missing") +
            total_field("admission_carriers", "missing") +
            total_field("operational_carriers", "missing");
        const qint64 pending =
            total_field("admission_dgb", "pending") +
            total_field("operational_dgb", "pending") +
            total_field("admission_carriers", "pending") +
            total_field("operational_carriers", "pending");
        const bool approved = policy.isObject() &&
            policy.find_value("paid_maintenance_approved").isBool() &&
            policy.find_value("paid_maintenance_approved").get_bool();
        // The wallet backend distinguishes an unconfirmed maintenance-fee
        // exposure from fees that are already confirmed and charged to the
        // rolling budgets. Keep both visible in the ordinary operator view;
        // otherwise a self-transfer that rebuilds a 1 DD carrier misleadingly
        // looks free because its two DD transaction rows net to zero.
        const qint64 reserved = poolNumber(
            result, "maintenance_fee_reserved_satoshis");
        const qint64 spent_hour = poolNumber(
            result, "maintenance_fee_spent_last_hour_satoshis");
        const qint64 spent_day = poolNumber(
            result, "maintenance_fee_spent_last_day_satoshis");

        QString state_text;
        QString next_step;
        QString status_kind;
        if (m_maintenance_state ==
            QLatin1String("waiting_for_target_configuration")) {
            status_kind = QStringLiteral("action");
            state_text = tr("Saved liquidity targets cannot make this offer ready");
            next_step = tr(
                "User-paid service needs at least three admission DigiDollar carriers and one operational DigiDollar carrier. Review the displayed targets, save them, and then prepare only the missing output.");
        } else if (m_maintenance_state ==
            QLatin1String("waiting_for_maintenance_approval")) {
            status_kind = QStringLiteral("action");
            state_text = tr("Liquidity targets are saved — refill spending is not yet approved");
            next_step = tr(
                "No target update is needed. To create the missing pool output, review the finite transaction, hourly and daily maintenance limits and explicitly approve that bounded refill cost.");
        } else if (m_maintenance_state ==
                   QLatin1String("replenishing_liquidity")) {
            if (maintenanceFeeLimitExceeded()) {
                status_kind = QStringLiteral("action");
                state_text = tr(
                    "Automatic refill paused — approved fee limit is too low");
                const qint64 fee_limit = policy.isObject()
                    ? poolNumber(policy,
                                 "maximum_maintenance_fee_per_transaction_satoshis")
                    : 0;
                next_step = tr(
                    "No transaction was created. The estimated network fee is above the saved per-transaction maintenance limit of %1 DGB. Review the limit and increase it only if that bounded cost is acceptable.")
                                .arg(dgbAmount(fee_limit));
            } else if (pending > 0) {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Replacement liquidity is waiting for confirmation");
                next_step = tr(
                    "%1 pending slot(s) already count toward the target. Mine or wait for a confirming block; Core will not create a duplicate refill.")
                    .arg(pending);
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
                status_kind = QStringLiteral("waiting");
                state_text = Params().GetChainType() == ChainType::REGTEST
                    ? tr("Refill approved — waiting for a fresh Regtest block")
                    : tr("Refill approved — waiting for node readiness");
                next_step = Params().GetChainType() == ChainType::REGTEST
                    ? tr("No refill transaction has been created yet. Mine a fresh Regtest block so Core can leave initial synchronization and safely create the missing pool output.")
                    : tr("No refill transaction has been created yet. Core will create it automatically after synchronization and transaction-broadcast readiness are restored.");
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_WALLET_LOCKED"))) {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Refill approved — waiting for wallet unlock");
                next_step = tr(
                    "No refill transaction has been created yet. Unlock the provider wallet; automatic maintenance will then resume without another approval.");
            } else if (!m_core_running) {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Refill approved — waiting for provider start");
                next_step = tr(
                    "No refill transaction has been created yet. Start the provider with “Start and restore liquidity”; Core will then create the bounded refill automatically.");
            } else {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Refill approved — preparing missing liquidity");
                next_step = tr(
                    "Core may now create at most one bounded maintenance transaction. Keep the provider service running and the wallet unlocked; the display changes to pending confirmation as soon as that transaction exists.");
            }
        } else if (m_maintenance_state ==
                   QLatin1String("waiting_for_liquidity_confirmation")) {
            status_kind = QStringLiteral("waiting");
            state_text = tr("Replacement liquidity is waiting for confirmation");
            next_step = tr(
                "%1 pending slot(s) count toward the target, so Core will not create duplicates. Provider readiness resumes after confirmation.")
                .arg(pending);
        } else if (m_maintenance_state == QLatin1String("ready")) {
            status_kind = QStringLiteral("ready");
            state_text = tr("Liquidity targets are satisfied");
            next_step = tr(
                "No maintenance transaction is required. Confirmed successor outputs are reused automatically.");
        } else {
            status_kind = QStringLiteral("error");
            state_text = tr("Liquidity maintenance status unavailable");
            next_step = tr("Refresh the provider status before changing liquidity.");
        }
        setStatusLabel(m_liquidity_maintenance_state, state_text,
                       status_kind);
        // Re-polishing an unchanged card on every poll caused the complete
        // Overview layout to flash even though no user-visible state changed.
        setStatusKind(m_liquidity_maintenance_card, status_kind);
        m_liquidity_maintenance_next_step->setText(next_step);
        QString maintenance_cost;
        if (!m_liquidity_targets_satisfy_provider_policy) {
            maintenance_cost = tr(
                "Automatic restoration is paused because the saved operational carrier target is zero. No maintenance transaction has been created.");
        } else {
            maintenance_cost = tr(
                "%1 missing and %2 pending slot(s). Refill spending authorization: %3.")
                    .arg(missing)
                    .arg(pending)
                    .arg(approved ? tr("approved within finite limits")
                                  : tr("not approved"));
            if (reserved > 0) {
                maintenance_cost += tr(
                    " Pending maintenance transactions currently expose %1 DGB in network fees; confirmed costs appear in Finances.")
                                        .arg(dgbAmount(reserved));
            }
            if (spent_hour > 0 || spent_day > 0) {
                maintenance_cost += tr(
                    " Confirmed maintenance cost: %1 DGB in the rolling hour and %2 DGB in the rolling day.")
                                        .arg(dgbAmount(spent_hour),
                                             dgbAmount(spent_day));
            }
        }
        m_liquidity_maintenance_cost->setText(maintenance_cost);
        m_approve_liquidity_maintenance->setText(
            maintenanceFeeLimitExceeded()
                ? tr("Review refill cost limit…")
            : m_liquidity_targets_satisfy_provider_policy
                ? tr("Approve bounded refill costs…")
                : tr("Review required liquidity targets…"));
        m_approve_liquidity_maintenance->setVisible(
            !m_liquidity_targets_satisfy_provider_policy || !configured ||
            (missing > 0 && !approved) || maintenanceFeeLimitExceeded());
        // Keep polling for either liquidity confirmation or an external node
        // gate. A successful liquidity refresh must not accidentally stop the
        // synchronization poll used by the Overview page.
        updateAutomaticRefreshTimer();

        m_liquidity_budget_status->setText(tr(
            "Maintenance fees — reserved: %1 DGB; spent in the rolling hour: %2 DGB; spent in the rolling day: %3 DGB.")
            .arg(dgbAmount(reserved), dgbAmount(spent_hour),
                 dgbAmount(spent_day)));
        m_carrier_base_cents = poolNumber(result, "carrier_base_cents");
        m_carrier_excess_cents = poolNumber(
            result, "carrier_withdrawable_excess_cents");
        m_carrier_value_status->setText(tr(
            "Confirmed carrier base: %1 DD · currently withdrawable excess: %2 DD. The base remains wallet-owned and reserved only while its slot is active.")
            .arg(QString::number(m_carrier_base_cents / 100.0, 'f', 2),
                 QString::number(m_carrier_excess_cents / 100.0, 'f', 2)));
        m_preview_carrier_excess->setEnabled(
            configured && hasCompleteMutationSnapshots() &&
            m_carrier_excess_cents > 0 && !m_busy);
        updateProviderButtons();
    }

    void refreshLiquidityStatus()
    {
        call("getpaymasterliquiditystatus", {}, false, nullptr,
             [this](const UniValue& result) {
                 if (!IsCompleteLiquidityStatus(result)) {
                     m_liquidity_snapshot_available = false;
                     m_liquidity_snapshot_representable = false;
                     setLiquidityPolicyMutationEnabled(false);
                     m_liquidity_policy_status->setText(tr(
                         "Core returned an incomplete liquidity status. Guided setup remains unavailable until a complete refresh succeeds."));
                     refreshProviderSafetyStatus();
                     return;
                 }
                 applyLiquidityStatus(result);
                 refreshLiquidityPoolEntries();
             }, false,
             [this](const QString& error) {
                 m_liquidity_snapshot_available = false;
                 m_liquidity_snapshot_representable = false;
                 setLiquidityPolicyMutationEnabled(false);
                 m_maintenance_state = QStringLiteral("unavailable");
                 m_liquidity_maintenance_state->setText(tr(
                     "Liquidity maintenance status unavailable"));
                 m_liquidity_maintenance_next_step->setText(error);
                 m_liquidity_policy_status->setText(tr(
                     "Core did not return a liquidity policy. No paid maintenance action is available."));
                 refreshProviderSafetyStatus();
             });
    }

    void applyLiquidityPoolEntries(const UniValue& result,
                                   bool refresh_safety)
    {
        const QString previous = m_release_carrier_select->currentData().toString();
        m_release_carrier_select->blockSignals(true);
        m_release_carrier_select->clear();
        m_pending_successor_carrier_cents = 0;
        m_pending_successor_dgb_satoshis = 0;
        if (!IsCompleteProviderPoolInfo(result)) {
            m_release_carrier_select->blockSignals(false);
            invalidateCarrierWithdrawalPreviews();
            m_preview_carrier_release->setEnabled(false);
            m_liquidity_recycling_status->setText(tr(
                "Core returned incomplete Paymaster pool entries. Carrier release remains unavailable until a complete refresh succeeds."));
            if (refresh_safety) refreshProviderSafetyStatus();
            return;
        }
        const UniValue& pool = result.find_value("pool");
        for (const UniValue& entry : pool.getValues()) {
            const QString state = activityText(entry, "state");
            const QString purpose = activityText(entry, "purpose");
            const QString asset = activityText(entry, "asset");
            if (state == QLatin1String("pending_successor")) {
                if (asset == QLatin1String("dd_carrier")) {
                    m_pending_successor_carrier_cents +=
                        poolNumber(entry, "dd_cents");
                } else if (asset == QLatin1String("dgb")) {
                    m_pending_successor_dgb_satoshis +=
                        poolNumber(entry, "dgb_satoshis");
                }
            }
            if (state != QLatin1String("available") ||
                purpose != QLatin1String("operational") ||
                asset != QLatin1String("dd_carrier") ||
                poolNumber(entry, "confirmation_height") <= 0) {
                continue;
            }
            const QString txid = activityText(entry, "txid");
            const qint64 vout = poolNumber(entry, "vout");
            const qint64 cents = poolNumber(entry, "dd_cents");
            const QString key = QStringLiteral("%1:%2").arg(txid).arg(vout);
            m_release_carrier_select->addItem(
                tr("%1 DD — %2…:%3")
                    .arg(QString::number(cents / 100.0, 'f', 2),
                         txid.left(12))
                    .arg(vout),
                key);
        }
        const int previous_index =
            m_release_carrier_select->findData(previous);
        if (previous_index >= 0) {
            m_release_carrier_select->setCurrentIndex(previous_index);
        }
        m_release_carrier_select->blockSignals(false);
        QStringList recycling;
        if (m_pending_successor_carrier_cents > 0) {
            recycling.push_back(tr(
                "Carrier is being reused: %1 DD, waiting for confirmation.")
                .arg(QString::number(
                    m_pending_successor_carrier_cents / 100.0,
                    'f', 2)));
        }
        if (m_pending_successor_dgb_satoshis > 0) {
            recycling.push_back(tr(
                "DGB successor slot is being reused: %1 DGB, waiting for confirmation.")
                .arg(dgbAmount(m_pending_successor_dgb_satoshis)));
        }
        if (recycling.isEmpty() &&
            m_maintenance_state == QLatin1String("replenishing_liquidity")) {
            recycling.push_back(tr(
                "A missing DGB slot will be replenished within the approved limits."));
        }
        m_liquidity_recycling_status->setText(
            recycling.isEmpty()
                ? tr("No replacement output is currently waiting for confirmation.")
                : recycling.join('\n'));
        const bool release_available =
            hasCompleteMutationSnapshots() &&
            m_liquidity_policy_configured && !m_core_running &&
            m_release_carrier_select->count() > 0;
        m_preview_carrier_release->setEnabled(release_available);
        if (previous_index < 0 && !previous.isEmpty()) {
            m_release_plan_id.clear();
            m_release_plan_expires_at = 0;
        }
        updateCarrierWithdrawalButtons();
        if (refresh_safety) refreshProviderSafetyStatus();
    }

    void refreshLiquidityPoolEntries()
    {
        call("getpaymasterpoolinfo", {}, false, nullptr,
             [this](const UniValue& result) {
                 applyLiquidityPoolEntries(result, /*refresh_safety=*/true);
             }, false,
             [this](const QString& error) {
                 m_release_carrier_select->clear();
                 m_preview_carrier_release->setEnabled(false);
                 m_liquidity_recycling_status->setText(tr(
                     "Pool entry details are unavailable: %1").arg(error));
                 refreshProviderSafetyStatus();
             });
    }

    void invalidateCarrierWithdrawalPreviews()
    {
        m_excess_plan_id.clear();
        m_release_plan_id.clear();
        m_excess_plan_expires_at = 0;
        m_release_plan_expires_at = 0;
        m_excess_preview_cents = 0;
        m_excess_preview_fee_satoshis = 0;
        if (m_carrier_withdrawal_status) {
            m_carrier_withdrawal_status->setText(tr(
                "No carrier withdrawal has been previewed."));
        }
        updateCarrierWithdrawalButtons();
    }

    bool carrierPlanCurrent(const QString& plan_id, qint64 expires_at) const
    {
        return !plan_id.isEmpty() && expires_at > 0 &&
               QDateTime::currentSecsSinceEpoch() <= expires_at;
    }

    void updateCarrierWithdrawalButtons()
    {
        if (!m_execute_carrier_excess || !m_execute_carrier_release) return;
        m_execute_carrier_excess->setEnabled(
            hasCompleteMutationSnapshots() &&
            carrierPlanCurrent(m_excess_plan_id,
                               m_excess_plan_expires_at));
        m_execute_carrier_release->setEnabled(
            hasCompleteMutationSnapshots() && !m_core_running &&
            carrierPlanCurrent(m_release_plan_id,
                               m_release_plan_expires_at));
    }

    UniValue carrierWithdrawalOptions(const QString& mode, bool execute,
                                      const QString& plan_id = {}) const
    {
        UniValue options{UniValue::VOBJ};
        options.pushKV("mode", mode.toStdString());
        options.pushKV("execute", execute);
        if (execute) {
            options.pushKV("plan_id", plan_id.toStdString());
        } else if (mode == QLatin1String("release_slot")) {
            const QString selected =
                m_release_carrier_select->currentData().toString();
            const int separator = selected.lastIndexOf(':');
            if (separator > 0) {
                options.pushKV("txid",
                               selected.left(separator).toStdString());
                options.pushKV("vout",
                               selected.mid(separator + 1).toInt());
            }
        }
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        return params;
    }

    void carrierWithdrawalAction(const QString& mode, bool execute)
    {
        // Accept either the production wallet transport or the synchronous
        // test transport. Keeping this guard aligned with call() lets widget
        // tests exercise the same preview/plan-binding path without a live
        // wallet, while production still fails closed when no RPC transport
        // is available.
        if (!requirePrivacyOffForSensitiveAction(
                tr("Reviewing a carrier withdrawal"))) {
            return;
        }
        if (!hasCompleteMutationSnapshots()) {
            m_carrier_withdrawal_status->setText(tr(
                "Carrier liquidity was not changed because a complete current provider, safety and liquidity snapshot is unavailable."));
            updateProviderButtons();
            return;
        }
        if (!hasRpcTransport() || m_busy) {
            m_carrier_withdrawal_status->setText(!hasRpcTransport()
                ? tr("Carrier withdrawal was not started because no wallet RPC transport is available.")
                : tr("Carrier withdrawal was not started because another wallet operation is still running."));
            return;
        }
        const bool excess = mode == QLatin1String("all_excess");
        QString plan_id = excess ? m_excess_plan_id : m_release_plan_id;
        const qint64 expires_at = excess ? m_excess_plan_expires_at
                                         : m_release_plan_expires_at;
        if (!execute && !excess &&
            m_release_carrier_select->currentData().toString().isEmpty()) {
            QMessageBox::warning(this, tr("Release carrier slot"),
                                 tr("Select a confirmed available operational carrier first."));
            return;
        }
        if (execute && !carrierPlanCurrent(plan_id, expires_at)) {
            QMessageBox::warning(
                this, tr("Carrier plan expired"),
                tr("The reviewed plan is no longer current. Preview the action again before executing."));
            invalidateCarrierWithdrawalPreviews();
            return;
        }
        if (execute) {
            const QString confirmation = excess
                ? tr("Execute the reviewed withdrawal of %1 DD carrier excess?\n\nEvery selected carrier keeps exactly 1.00 DD. The reviewed DGB network fee is %2 DGB.")
                      .arg(QString::number(
                          m_excess_preview_cents / 100.0, 'f', 2),
                           dgbAmount(m_excess_preview_fee_satoshis))
                : tr("Release the reviewed operational carrier slot completely?\n\nThis costs no network fee, makes the carrier value ordinary wallet balance, and reduces the saved operational-carrier target by one.");
            if (askPlainTextQuestion(
                    this, tr("Confirm carrier withdrawal"), confirmation) !=
                QMessageBox::Yes) {
                return;
            }
        }
        call("withdrawpaymastercarrier",
             carrierWithdrawalOptions(mode, execute, plan_id),
             /*needs_unlock=*/execute && excess, nullptr,
             [this, mode, execute, plan_id](const UniValue& result) {
                 const bool excess = mode == QLatin1String("all_excess");
                 if (!IsCompleteCarrierWithdrawalResult(
                         result, mode, execute, plan_id) ||
                     (!execute &&
                      poolNumber(result, "expires_at") <=
                          QDateTime::currentSecsSinceEpoch())) {
                     invalidateCarrierWithdrawalPreviews();
                     m_carrier_withdrawal_status->setText(tr(
                         "Core did not return a complete response bound to this carrier-withdrawal action. Preview the action again after refreshing provider status."));
                     return;
                 }
                 const QString returned_plan = activityText(result, "plan_id");
                 if (!execute) {
                     const qint64 expiry = poolNumber(result, "expires_at");
                     if (excess) {
                         m_excess_plan_id = returned_plan;
                         m_excess_plan_expires_at = expiry;
                         m_excess_preview_cents = poolNumber(
                             result, "withdrawable_excess_cents");
                         m_excess_preview_fee_satoshis = poolNumber(
                             result, "estimated_network_fee_satoshis");
                         m_release_plan_id.clear();
                         m_release_plan_expires_at = 0;
                         m_carrier_withdrawal_status->setText(tr(
                             "Preview only — withdraw %1 DD excess, retain %2 DD carrier base and pay an estimated %3 DGB network fee. Plan valid until %4.")
                             .arg(QString::number(
                                      m_excess_preview_cents / 100.0,
                                      'f', 2),
                                  QString::number(
                                      poolNumber(result,
                                                 "retained_carrier_cents") /
                                          100.0,
                                      'f', 2),
                                  dgbAmount(
                                      m_excess_preview_fee_satoshis),
                                  QDateTime::fromSecsSinceEpoch(expiry)
                                      .toString(Qt::ISODate)));
                     } else {
                         m_release_plan_id = returned_plan;
                         m_release_plan_expires_at = expiry;
                         m_excess_plan_id.clear();
                         m_excess_plan_expires_at = 0;
                         m_carrier_withdrawal_status->setText(tr(
                             "Preview only — release the selected carrier without a network fee and reduce the operational-carrier target to %1. Plan valid until %2.")
                             .arg(poolNumber(
                                      result,
                                      "operational_carrier_target"))
                             .arg(QDateTime::fromSecsSinceEpoch(expiry)
                                      .toString(Qt::ISODate)));
                     }
                     updateCarrierWithdrawalButtons();
                     return;
                 }
                 const QString txid = activityText(result, "txid");
                 m_carrier_withdrawal_status->setText(excess
                     ? tr("Carrier excess withdrawal submitted successfully.%1")
                           .arg(txid.isEmpty()
                                    ? QString{}
                                    : tr(" Transaction: %1").arg(txid))
                     : tr("Carrier slot released successfully. New operational-carrier target: %1.")
                           .arg(poolNumber(
                               result,
                               "operational_carrier_target")));
                 m_excess_plan_id.clear();
                 m_release_plan_id.clear();
                 m_excess_plan_expires_at = 0;
                 m_release_plan_expires_at = 0;
                 refreshStatus();
             });
    }

    PaymasterSetupMode loadSetupMode() const
    {
        const QString key = PaymasterSetupModeSettingsKey(m_model);
        if (key.isEmpty()) return PaymasterSetupMode::UNDECIDED;
        QSettings settings;
        const QString value = settings.value(key).toString();
        if (value == QLatin1String("guided")) return PaymasterSetupMode::GUIDED;
        if (value == QLatin1String("expert")) return PaymasterSetupMode::EXPERT;
        if (value == QLatin1String("existing")) return PaymasterSetupMode::EXISTING;
        return PaymasterSetupMode::UNDECIDED;
    }

    void setSetupMode(PaymasterSetupMode mode)
    {
        m_setup_mode = mode;
        const QString key = PaymasterSetupModeSettingsKey(m_model);
        if (!key.isEmpty()) {
            QString value;
            if (mode == PaymasterSetupMode::GUIDED) value = QStringLiteral("guided");
            if (mode == PaymasterSetupMode::EXPERT) value = QStringLiteral("expert");
            if (mode == PaymasterSetupMode::EXISTING) value = QStringLiteral("existing");
            QSettings settings;
            if (value.isEmpty()) {
                settings.remove(key);
            } else {
                settings.setValue(key, value);
            }
        }
        updateSetupAccess();
        const bool expert = mode == PaymasterSetupMode::EXPERT;
        if (m_liquidity_advanced_toggle) {
            m_liquidity_advanced_toggle->setChecked(expert);
        }
        if (m_runtime_settings_toggle) {
            m_runtime_settings_toggle->setChecked(expert);
        }
        if (m_runtime_settings_panel) {
            // Visibility is part of the selected setup mode, not merely a
            // side effect of a toggled signal. Qt may suppress that signal
            // when the button already has the requested state while its
            // stacked parent was hidden.
            m_runtime_settings_panel->setVisible(expert);
        }
    }

    void updateSetupAccess()
    {
        const bool unlocked = m_setup_mode != PaymasterSetupMode::UNDECIDED;
        if (m_setup_choice) m_setup_choice->setVisible(!unlocked);
        if (m_setup_content) m_setup_content->setVisible(unlocked);
        if (!m_tabs) return;
        for (QWidget* page : {m_configuration_page, m_safety_page,
                              m_liquidity_page, m_finance_page}) {
            const int index = m_tabs->indexOf(page);
            if (index >= 0) {
                m_tabs->setTabEnabled(index, unlocked && !m_privacy);
                m_tabs->setTabToolTip(index, m_privacy
                    ? tr("This provider page is hidden while privacy mode is active.")
                    : unlocked ? QString() : tr(
                          "Complete guided setup or choose manual expert setup to unlock this page."));
            }
        }
        // Recovery and durable reservation inspection must never be hidden by
        // a local onboarding preference.
        const int activity_index = m_tabs->indexOf(m_activity_page);
        if (activity_index >= 0) {
            m_tabs->setTabEnabled(activity_index, !m_privacy);
            m_tabs->setTabToolTip(activity_index, m_privacy
                ? tr("Provider activity details are hidden while privacy mode is active.")
                : !unlocked ? tr(
                      "Recovery remains available so existing reservations can always be inspected.")
                : QString());
        }
        if (!unlocked || m_privacy) m_tabs->setCurrentIndex(0);
    }

    void restorePolicyDefaults()
    {
        applyRecommendedPolicyDefaults(/*mark_dirty=*/true);
    }

    void applyRecommendedPolicyDefaults(bool mark_dirty)
    {
        m_loading_policy = true;
        m_scope->setCurrentIndex(m_scope->findData(QStringLiteral("public")));
        m_user_paid->setChecked(true);
        m_sponsored->setChecked(false);
        m_fee_bps->setValue(50);
        m_min_amount->setValue(100);
        m_max_amount->setValue(100000);
        m_quote_ttl->setValue(60);
        m_network_fee->setValue(20000000);
        m_loading_policy = false;
        m_policy_loaded = false;
        m_policy_dirty = mark_dirty;
        updatePolicyDisplay();
    }

    void setPolicyMutationEnabled(bool enabled)
    {
        const bool controls_enabled = enabled && !m_busy;
        m_sponsored->setEnabled(controls_enabled);
        m_user_paid->setEnabled(controls_enabled);
        m_scope->setEnabled(controls_enabled && m_sponsored->isChecked());
        for (QSpinBox* control : {
                 m_fee_bps, m_min_amount, m_max_amount,
                 m_quote_ttl, m_network_fee}) {
            control->setReadOnly(!controls_enabled);
            control->setProperty("paymasterInvalidPersistedValue",
                                 !enabled);
        }
        m_fee_bps->setEnabled(controls_enabled && m_user_paid->isChecked());
        if (m_restore_policy_defaults) {
            m_restore_policy_defaults->setEnabled(controls_enabled);
        }
        if (m_save_policy) {
            m_save_policy->setEnabled(enabled && !m_busy);
        }
    }

    void loadPolicy(const UniValue& policy)
    {
        if (!IsCompleteProviderPolicy(policy)) return;
        int fee_rate_bps{0};
        int min_amount_cents{0};
        int max_amount_cents{0};
        int quote_ttl{0};
        qint64 maximum_network_fee{0};
        const bool representable =
            GetIntField(policy, "fee_rate_bps", fee_rate_bps) &&
            GetIntField(policy, "min_amount_cents", min_amount_cents) &&
            GetIntField(policy, "max_amount_cents", max_amount_cents) &&
            GetIntField(policy, "quote_ttl", quote_ttl) &&
            GetInt64Field(policy, "maximum_network_fee_dgb_satoshis",
                          maximum_network_fee) &&
            spinCanRepresent(m_fee_bps, fee_rate_bps) &&
            spinCanRepresent(m_min_amount, min_amount_cents) &&
            spinCanRepresent(m_max_amount, max_amount_cents) &&
            spinCanRepresent(m_quote_ttl, quote_ttl) &&
            spinCanRepresent(m_network_fee, maximum_network_fee);
        if (!representable) {
            // Preserve the complete authoritative object instead of feeding an
            // out-of-range qint64 through QSpinBox::setValue(), which would
            // silently clamp it and make a later Save overwrite Core state.
            m_unrepresentable_policy_snapshot = policy;
            m_policy_snapshot_representable = false;
            m_policy_loaded = false;
            m_policy_dirty = false;
            // Populate only fields that round-trip exactly. The complete raw
            // object remains authoritative and no setter is enabled, while
            // the wizard can still describe the unaffected imported choices.
            m_loading_policy = true;
            bool raw_sponsored{false};
            bool raw_user_paid{false};
            for (const UniValue& model :
                 policy.find_value("funding_models").getValues()) {
                raw_sponsored |= model.get_str() == "sponsored";
                raw_user_paid |= model.get_str() == "user_paid";
            }
            m_sponsored->setChecked(raw_sponsored);
            m_user_paid->setChecked(raw_user_paid);
            const int scope_index = m_scope->findData(
                QString::fromStdString(
                    policy.find_value("sponsorship_scope").get_str()));
            if (scope_index >= 0) m_scope->setCurrentIndex(scope_index);
            if (spinCanRepresent(m_fee_bps, fee_rate_bps)) {
                m_fee_bps->setValue(fee_rate_bps);
            }
            if (spinCanRepresent(m_min_amount, min_amount_cents)) {
                m_min_amount->setValue(min_amount_cents);
            }
            if (spinCanRepresent(m_max_amount, max_amount_cents)) {
                m_max_amount->setValue(max_amount_cents);
            }
            if (spinCanRepresent(m_quote_ttl, quote_ttl)) {
                m_quote_ttl->setValue(quote_ttl);
            }
            if (spinCanRepresent(m_network_fee, maximum_network_fee)) {
                m_network_fee->setValue(
                    static_cast<int>(maximum_network_fee));
            }
            m_loading_policy = false;
            setPolicyMutationEnabled(false);
            QString invalid_field;
            QString raw_value;
            if (!spinCanRepresent(m_fee_bps, fee_rate_bps)) {
                invalid_field = tr("service fee");
                raw_value = QString::number(fee_rate_bps);
            } else if (!spinCanRepresent(m_min_amount,
                                         min_amount_cents)) {
                invalid_field = tr("minimum payment");
                raw_value = QString::number(min_amount_cents);
            } else if (!spinCanRepresent(m_max_amount,
                                         max_amount_cents)) {
                invalid_field = tr("maximum payment");
                raw_value = QString::number(max_amount_cents);
            } else if (!spinCanRepresent(m_quote_ttl, quote_ttl)) {
                invalid_field = tr("quote lifetime");
                raw_value = QString::number(quote_ttl);
            } else {
                invalid_field = tr("maximum network fee");
                raw_value = QString::number(maximum_network_fee);
            }
            m_funding_model_status->setText(tr(
                "The persisted provider policy contains a %1 value (%2) that this interface cannot represent exactly. The original Core policy is retained and all policy mutation is disabled.")
                .arg(invalid_field, raw_value));
            updateProviderButtons();
            return;
        }
        const UniValue& funding_models = policy.find_value("funding_models");

        bool sponsored{false};
        bool user_paid{false};
        for (const UniValue& model : funding_models.getValues()) {
            if (!model.isStr()) continue;
            sponsored |= model.get_str() == "sponsored";
            user_paid |= model.get_str() == "user_paid";
        }

        m_loading_policy = true;
        m_policy_snapshot_representable = true;
        m_unrepresentable_policy_snapshot = UniValue{UniValue::VOBJ};
        setPolicyMutationEnabled(true);
        m_sponsored->setChecked(sponsored);
        m_user_paid->setChecked(user_paid);
        const UniValue& scope = policy.find_value("sponsorship_scope");
        if (scope.isStr()) {
            const int scope_index = m_scope->findData(QString::fromStdString(scope.get_str()));
            if (scope_index >= 0) m_scope->setCurrentIndex(scope_index);
        }
        m_fee_bps->setValue(fee_rate_bps);
        m_min_amount->setValue(min_amount_cents);
        m_max_amount->setValue(max_amount_cents);
        m_quote_ttl->setValue(quote_ttl);
        m_network_fee->setValue(static_cast<int>(maximum_network_fee));
        m_loading_policy = false;
        m_policy_loaded = true;
        m_policy_dirty = false;
        updatePolicyDisplay();
    }

    void updatePolicyDisplay()
    {
        const bool user_paid = m_user_paid->isChecked();
        const bool sponsored = m_sponsored->isChecked();
        const bool restricted = sponsored &&
                                m_scope->currentData().toString() == QLatin1String("restricted");
        // A complete but future/out-of-range Core policy is deliberately
        // retained outside the QSpinBox controls. Never let a later display
        // refresh re-enable individual editors from that read-only snapshot.
        m_scope->setEnabled(!m_busy && m_policy_snapshot_representable &&
                            sponsored);
        m_fee_bps->setEnabled(!m_busy && m_policy_snapshot_representable &&
                              user_paid);

        QString model_text;
        if (user_paid && sponsored) {
            model_text = tr("User paid and public sponsored");
            m_funding_model_status->setText(tr(
                "Both models may be active together. User-paid transfers charge the configured DigiDollar service fee; public sponsored transfers charge no DigiDollar service fee. Each model has separate safety budgets."));
        } else if (user_paid) {
            model_text = tr("User paid only");
            m_funding_model_status->setText(tr(
                "Clients pay the configured DigiDollar service fee while this provider supplies the DGB network fee."));
        } else if (sponsored) {
            model_text = restricted ? tr("Restricted sponsored only")
                                    : tr("Public sponsored only");
            m_funding_model_status->setText(restricted
                ? tr("Only clients with a valid invitation may use this sponsored service. No DigiDollar service fee is charged.")
                : tr("Discovered clients may use this sponsored service. No DigiDollar service fee is charged; finite safety budgets are essential."));
        } else {
            m_funding_model_status->setText(tr("Select at least one payment model."));
            m_policy_summary->setText(tr(
                "No payment model selected. Select at least one model before saving."));
            return;
        }

        const QString minimum = QString::number(m_min_amount->value() / 100.0, 'f', 2);
        const QString maximum = QString::number(m_max_amount->value() / 100.0, 'f', 2);
        const QString service_fee = QString::number(m_fee_bps->value() / 100.0, 'f', 2);
        const QString network_fee = QString::number(m_network_fee->value() / 100000000.0, 'f', 8);
        const QString persistence = m_policy_dirty
            ? tr("Unsaved changes — save the policy to activate them.")
            : m_policy_loaded
                ? tr("These are the currently saved settings.")
                : tr("Recommended starting values — save the policy to activate them.");
        m_policy_summary->setText(tr(
            "%1; payments from %2 to %3 DD; user-paid service fee %4%; quotes valid for %5 seconds; DGB network-fee ceiling %6 DGB per transfer. %7")
            .arg(model_text, minimum, maximum, service_fee)
            .arg(m_quote_ttl->value())
            .arg(network_fee, persistence));
    }

    QString readinessExplanation(const QString& error) const
    {
        if (error == QLatin1String("PAYMASTER_DISABLED"))
            return tr("Paymaster support is disabled for this node. Enable it and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_NOT_ENABLED"))
            return tr("The provider configuration has not been enabled yet.");
        if (error == QLatin1String("PAYMASTER_IDENTITY_NOT_FOUND"))
            return tr("Create the provider identity in the Configuration tab.");
        if (error == QLatin1String("PAYMASTER_POLICY_NOT_FOUND"))
            return tr("Choose and save an operating policy in the Configuration tab.");
        if (error == QLatin1String("PAYMASTER_SAFETY_POLICY_NOT_FOUND") ||
            error == QLatin1String("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND"))
            return tr("Review and save finite spending and rate limits in the Safety limits tab.");
        if (error == QLatin1String("PAYMASTER_POOLS_NOT_PREPARED"))
            return tr("Preview and prepare the required wallet liquidity in the Liquidity tab.");
        if (error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
            error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
            error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING"))
            return tr("The saved targets are correct, but the wallet does not yet contain every confirmed pool output. Restore the missing liquidity; no target update is required.");
        if (error == QLatin1String("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED") ||
            error == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND"))
            return tr("Review and explicitly approve finite automatic liquidity-maintenance limits in the Liquidity tab.");
        if (error == QLatin1String("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING"))
            return tr("Wait for the automatically prepared replacement liquidity to confirm.");
        if (error == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE"))
            return tr("The saved liquidity targets cannot support the active user-paid offer. Review Liquidity and save at least three carrier outputs for capacity checks and one carrier output for payments.");
        if (error == QLatin1String("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"))
            return tr("The finite automatic-maintenance budget is exhausted. Wait for the rolling limit to recover or review the saved limits in the Liquidity tab.");
        if (error == QLatin1String("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"))
            return tr("Automatic replenishment could not create or resume a safe maintenance transaction. Review the detailed liquidity status before retrying.");
        if (error == QLatin1String("PAYMASTER_WALLET_LOCKED"))
            return tr("Unlock the wallet before starting the provider or signing provider transactions.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_REQUIRES_DESCRIPTOR_WALLET"))
            return tr("Select a descriptor wallet. Legacy wallets cannot hold the Paymaster provider identity.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_REQUIRES_PRIVATE_KEYS"))
            return tr("Select a wallet with local private keys. Watch-only wallets cannot operate a Paymaster provider.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_REJECTS_EXTERNAL_SIGNER"))
            return tr("Select a wallet without an external signer. Provider identity and transaction signing must be available locally.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED"))
            return tr("Configure a reachable provider endpoint and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE"))
            return tr("The configured provider endpoint is not reachable by clients on this network.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_PRUNE_0"))
            return tr("Provider operation requires an unpruned node.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_TXINDEX"))
            return tr("Enable the transaction index and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_READY_TXINDEX"))
            return tr("Wait until the transaction index has finished synchronizing.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_V2_TRANSPORT"))
            return tr("Enable BIP324 version 2 transport and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_NODE_NOT_READY"))
            return tr("Wait until the node has synchronized and is ready to broadcast transactions.");
        if (error == QLatin1String("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"))
            return tr("DigiDollar is not active on the current chain yet.");
        if (error == QLatin1String("PAYMASTER_SAFETY_LIMIT_EXHAUSTED")) {
            qint64 applicable_per_transaction{std::numeric_limits<qint64>::max()};
            const auto include_model = [&applicable_per_transaction](
                    const FundingSafetyControls& controls, bool selected) {
                if (!selected) return;
                FundingSafetyValues values;
                if (readFundingSafety(controls, values) && values.per_transaction > 0) {
                    applicable_per_transaction = std::min(
                        applicable_per_transaction, values.per_transaction);
                }
            };
            include_model(m_user_paid_safety, m_user_paid->isChecked());
            include_model(
                m_public_sponsored_safety,
                m_sponsored->isChecked() &&
                    m_scope->currentData().toString() == QLatin1String("public"));
            include_model(
                m_restricted_sponsored_safety,
                m_sponsored->isChecked() &&
                    m_scope->currentData().toString() == QLatin1String("restricted"));
            if (applicable_per_transaction != std::numeric_limits<qint64>::max() &&
                m_network_fee->value() > applicable_per_transaction) {
                return tr(
                    "The advertised network-fee ceiling (%1 DGB) exceeds the applicable safety limit per transfer (%2 DGB). Lower the ceiling in Configuration or increase the limit in Safety limits, then save the changed setting.")
                    .arg(QString::number(m_network_fee->value() / 100000000.0, 'f', 8))
                    .arg(QString::number(applicable_per_transaction / 100000000.0, 'f', 8));
            }
            return tr("The configured provider budget or rate limit has been reached.");
        }
        if (error == QLatin1String("PAYMASTER_MESSAGE_CAPTURE_ENABLED"))
            return tr("Disable network message capture before operating a mainnet provider.");
        if (error.contains(QLatin1String("WALLET")))
            return tr("The selected wallet does not meet the provider requirements.");
        return tr("An additional provider requirement is not satisfied. See technical details below.");
    }

    void updateReadinessSummary(const QStringList& errors, bool running, bool ready)
    {
        updateExternalPrerequisites();
        if (running) {
            setStatusLabel(m_next_step, tr("Provider is running"),
                           QStringLiteral("ready"));
            m_readiness_summary->setText(tr(
                "Keep this node online and monitor reservations, liquidity and safety-budget usage."));
            return;
        }
        if (ready) {
            setStatusLabel(m_next_step,
                           tr("All external prerequisites are ready"),
                           QStringLiteral("ready"));
            m_readiness_summary->setText(tr(
                "All requirements are satisfied and all setup settings are saved. No additional Save action is required. Start the provider only when you are ready for it to accept client requests within the displayed safety limits."));
            return;
        }

        if (providerConfigurationComplete() && hasPassiveExternalWait()) {
            setStatusLabel(m_next_step,
                           tr("Waiting for blockchain synchronization"),
                           QStringLiteral("waiting"));
            m_readiness_summary->setText(tr(
                "Provider configuration is complete. You do not need to change any provider settings. "
                "Core will enable provider start automatically when the node is synchronized, the transaction index is ready and transactions can be broadcast."));
            return;
        }

        const bool liquidity_attention = std::any_of(
            errors.cbegin(), errors.cend(),
            [](const QString& error) { return isLiquidityReadinessError(error); });
        if (providerConfigurationComplete() && liquidity_attention) {
            setStatusLabel(
                m_next_step,
                tr("All external prerequisites are ready"),
                QStringLiteral("ready"));
            m_readiness_summary->setText(tr(
                "Blockchain synchronization, transaction indexing, transaction broadcast and DigiDollar activation are ready. The remaining wallet-liquidity action is shown separately below."));
            return;
        }

        QString next;
        const QStringList priority{
            QStringLiteral("PAYMASTER_DISABLED"),
            QStringLiteral("PAYMASTER_IDENTITY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_POLICY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_SAFETY_POLICY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND"),
            QStringLiteral("PAYMASTER_PROVIDER_NOT_ENABLED"),
            QStringLiteral("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED"),
            QStringLiteral("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE"),
            QStringLiteral("PAYMASTER_POOLS_NOT_PREPARED"),
            QStringLiteral("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING"),
            QStringLiteral("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"),
            QStringLiteral("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"),
            QStringLiteral("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED"),
            QStringLiteral("PAYMASTER_REQUIRES_PRUNE_0"),
            QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"),
            QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"),
            QStringLiteral("PAYMASTER_REQUIRES_V2_TRANSPORT"),
            QStringLiteral("PAYMASTER_NODE_NOT_READY"),
            QStringLiteral("PAYMASTER_WALLET_LOCKED"),
        };
        for (const QString& candidate : priority) {
            if (errors.contains(candidate)) {
                next = readinessExplanation(candidate);
                break;
            }
        }
        if (next.isEmpty() && !errors.isEmpty()) next = readinessExplanation(errors.first());
        const bool unknown_error = std::any_of(
            errors.cbegin(), errors.cend(), [](const QString& error) {
                return !isProviderSetupReadinessError(error) &&
                       !isLiquidityReadinessError(error) &&
                       !isPassiveExternalReadinessError(error) &&
                       error != QLatin1String("PAYMASTER_REQUIRES_TXINDEX");
            });
        setStatusLabel(
            m_next_step,
            next.isEmpty() ? tr("Setup status unavailable")
                           : tr("Next step: %1").arg(next),
            unknown_error ? QStringLiteral("error")
                          : QStringLiteral("action"));

        QStringList explanations;
        for (const QString& error : errors) {
            const QString explanation = readinessExplanation(error);
            if (!explanations.contains(explanation)) explanations.push_back(explanation);
        }
        m_readiness_summary->setText(explanations.isEmpty()
            ? tr("Refresh the status to check the provider requirements.")
            : QStringLiteral("• ") + explanations.join(QStringLiteral("\n• ")));
    }

    static QLineEdit* integerField(QWidget* parent, qint64 value)
    {
        auto* result = new QLineEdit(QString::number(value), parent);
        result->setMaxLength(19);
        result->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]{1,19}")), result));
        return result;
    }

    FundingSafetyControls createFundingSafetyControls(
        QTabWidget* tabs, const QString& title, const QString& object_prefix,
        bool recommended_defaults)
    {
        auto* page = new QWidget(tabs);
        page->setObjectName(object_prefix + QStringLiteral("Page"));
        auto* form = new QFormLayout(page);
        form->setContentsMargins(14, 14, 14, 14);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(9);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        QString explanation;
        if (object_prefix == QLatin1String("paymasterSafetyUserPaid")) {
            explanation = tr(
                "Use these limits when clients compensate this provider with a DigiDollar service fee. The provider still spends DGB for network fees, so finite DGB budgets remain necessary.");
        } else if (object_prefix == QLatin1String("paymasterSafetyPublicSponsored")) {
            explanation = tr(
                "Public sponsorship is available to discovered clients and therefore has the greatest budget-exhaustion risk. Its safe default is disabled; enable it only with finite values you can afford to lose.");
        } else {
            explanation = tr(
                "Restricted sponsorship requires a provider-issued invitation but still needs finite limits. Its safe default is disabled until you deliberately configure a budget.");
        }
        auto* help = new QLabel(explanation, page);
        help->setObjectName(object_prefix + QStringLiteral("Explanation"));
        help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        help->setWordWrap(true);
        form->addRow(help);
        FundingSafetyControls controls;
        controls.per_transaction = dgbAmountField(
            page, recommended_defaults ? m_network_fee->value() : 0);
        controls.reserved = dgbAmountField(page, recommended_defaults ? 100000000 : 0);
        controls.per_hour = dgbAmountField(page, recommended_defaults ? 200000000 : 0);
        controls.per_day = dgbAmountField(page, recommended_defaults ? 1000000000 : 0);
        controls.completed_per_hour = spin(page, 0, 1000000, recommended_defaults ? 10 : 0);
        controls.completed_per_day = spin(page, 0, 1000000, recommended_defaults ? 100 : 0);
        controls.per_transaction->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerTransaction"));
        controls.reserved->setObjectName(object_prefix + QStringLiteral("MaxReservedNetworkFee"));
        controls.per_hour->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerHour"));
        controls.per_day->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerDay"));
        controls.completed_per_hour->setObjectName(object_prefix + QStringLiteral("MaxCompletedPerHour"));
        controls.completed_per_day->setObjectName(object_prefix + QStringLiteral("MaxCompletedPerDay"));
        controls.per_transaction->setToolTip(tr("Maximum DGB network fee committed by one transfer."));
        controls.reserved->setToolTip(tr("Maximum DGB network fees reserved across simultaneous open quotes."));
        controls.per_hour->setToolTip(tr("Maximum cumulative DGB network fees in any rolling hour."));
        controls.per_day->setToolTip(tr("Maximum cumulative DGB network fees in any rolling 24 hours."));
        controls.completed_per_hour->setToolTip(tr("Maximum completed transfers in any rolling hour."));
        controls.completed_per_day->setToolTip(tr("Maximum completed transfers in any rolling 24 hours."));
        form->addRow(tr("Maximum per transfer (DGB):"), controls.per_transaction);
        form->addRow(tr("Maximum reserved at once (DGB):"), controls.reserved);
        form->addRow(tr("Maximum per rolling hour (DGB):"), controls.per_hour);
        form->addRow(tr("Maximum per rolling day (DGB):"), controls.per_day);
        form->addRow(tr("Completed transfers per rolling hour:"), controls.completed_per_hour);
        form->addRow(tr("Completed transfers per rolling day:"), controls.completed_per_day);
        controls.mode = new QLabel(page);
        controls.mode->setObjectName(object_prefix + QStringLiteral("Mode"));
        controls.mode->setWordWrap(true);
        form->addRow(controls.mode);
        auto* restore_defaults = new QPushButton(
            recommended_defaults ? tr("Restore recommended limits")
                                 : tr("Restore safe disabled defaults"), page);
        restore_defaults->setObjectName(object_prefix + QStringLiteral("RestoreDefaults"));
        restore_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        restore_defaults->setToolTip(recommended_defaults
            ? tr("Restore the recommended finite starting limits for this model. Save the provider safety policy to activate them.")
            : tr("Set all six limits to zero so this unused model is safely disabled. Save the provider safety policy to activate the change."));
        form->addRow(restore_defaults);
        connect(restore_defaults, &QPushButton::clicked, this,
                [this, controls, recommended_defaults] {
                    restoreFundingSafetyDefaults(controls, recommended_defaults);
                });
        tabs->addTab(page, title);
        return controls;
    }

    void restoreFundingSafetyDefaults(const FundingSafetyControls& controls,
                                      bool recommended_defaults,
                                      bool mark_dirty = true)
    {
        m_loading_provider_safety = true;
        static_cast<DgbAmountLineEdit*>(controls.per_transaction)->setSatoshis(
            recommended_defaults ? m_network_fee->value() : 0);
        static_cast<DgbAmountLineEdit*>(controls.reserved)->setSatoshis(
            recommended_defaults ? 100000000 : 0);
        static_cast<DgbAmountLineEdit*>(controls.per_hour)->setSatoshis(
            recommended_defaults ? 200000000 : 0);
        static_cast<DgbAmountLineEdit*>(controls.per_day)->setSatoshis(
            recommended_defaults ? 1000000000 : 0);
        controls.completed_per_hour->setValue(recommended_defaults ? 10 : 0);
        controls.completed_per_day->setValue(recommended_defaults ? 100 : 0);
        m_loading_provider_safety = false;
        if (mark_dirty) {
            markProviderSafetyDirty();
        } else {
            m_provider_safety_dirty = false;
        }
        updateFundingSafetyDisplay(controls);
    }

    void restoreQuoteSafetyDefaults(bool mark_dirty = true)
    {
        m_loading_provider_safety = true;
        m_max_active_quotes_total->setValue(16);
        m_max_active_quotes_per_netgroup->setValue(4);
        m_max_active_quotes_per_recipient->setValue(2);
        m_max_quote_requests_per_netgroup->setValue(10);
        m_loading_provider_safety = false;
        if (mark_dirty) {
            markProviderSafetyDirty();
        } else {
            m_provider_safety_dirty = false;
        }
    }

    void restoreClientSafetyDefaults(bool mark_dirty = true)
    {
        m_loading_client_safety = true;
        m_client_fee_per_transaction->setValue(100);
        m_client_fee_per_day->setValue(1000);
        m_loading_client_safety = false;
        if (mark_dirty) {
            markClientSafetyDirty();
        } else {
            m_client_safety_dirty = false;
        }
        updateClientSafetyDisplay();
    }

    void markProviderSafetyDirty()
    {
        m_provider_safety_dirty = true;
        m_provider_safety_status->setText(tr(
            "Unsaved provider safety changes — the previously saved limits remain active until these values are saved."));
        updateProviderButtons();
    }

    void markClientSafetyDirty()
    {
        m_client_safety_dirty = true;
        m_client_safety_status->setText(tr(
            "Unsaved client safety changes — the previously saved limits remain active until these values are saved."));
    }

    void updateFundingSafetyDisplay(const FundingSafetyControls& controls,
                                    bool persisted = false)
    {
        if (!controls.mode) return;
        FundingSafetyValues values;
        if (!readFundingSafety(controls, values)) {
            controls.mode->setText(tr(
                "Not saved — one or more amounts are outside the supported whole-satoshi range."));
        } else if (values.allZero()) {
            controls.mode->setText(tr(
                "Disabled — all limits are zero; zero never means unlimited."));
        } else if (values.hasZero()) {
            controls.mode->setText(tr(
                "Incomplete — an enabled model requires all six limits to be greater than zero. Use all zero values only to disable the model."));
        } else {
            const QString per_transaction = QString::number(
                values.per_transaction / 100000000.0, 'f', 8);
            const QString per_hour = QString::number(
                values.per_hour / 100000000.0, 'f', 8);
            const QString per_day = QString::number(
                values.per_day / 100000000.0, 'f', 8);
            controls.mode->setText(persisted
                ? tr("Active finite limits — at most %1 DGB per transfer, %2 DGB per rolling hour and %3 DGB per rolling day.")
                      .arg(per_transaction, per_hour, per_day)
                : tr("Unsaved limits — at most %1 DGB per transfer, %2 DGB per rolling hour and %3 DGB per rolling day. Save the provider safety policy to activate them.")
                      .arg(per_transaction, per_hour, per_day));
        }
    }

    void updateClientSafetyDisplay(bool persisted = false)
    {
        if (!m_client_safety_mode) return;
        if (m_client_fee_per_transaction->value() == 0 &&
            m_client_fee_per_day->value() == 0) {
            m_client_safety_mode->setText(tr(
                "Zero service-fee budget — only zero-fee authorizations fit; zero never means unlimited."));
        } else if (persisted) {
            m_client_safety_mode->setText(tr(
                "Active limits — at most %1 DD per transfer and %2 DD in a rolling day.")
                .arg(QString::number(m_client_fee_per_transaction->value() / 100.0, 'f', 2),
                     QString::number(m_client_fee_per_day->value() / 100.0, 'f', 2)));
        } else {
            m_client_safety_mode->setText(tr(
                "Unsaved limits — at most %1 DD per transfer and %2 DD in a rolling day. Save the client safety policy to activate them.")
                .arg(QString::number(m_client_fee_per_transaction->value() / 100.0, 'f', 2),
                     QString::number(m_client_fee_per_day->value() / 100.0, 'f', 2)));
        }
    }

    void watchFundingSafetyControls(const FundingSafetyControls& controls)
    {
        for (QLineEdit* control : {controls.per_transaction, controls.reserved,
                                   controls.per_hour, controls.per_day}) {
            connect(control, &QLineEdit::textChanged, this, [this, controls] {
                if (!m_loading_provider_safety) markProviderSafetyDirty();
                updateFundingSafetyDisplay(controls);
                updateProviderButtons();
            });
        }
        for (QSpinBox* control : {controls.completed_per_hour,
                                  controls.completed_per_day}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), this, [this, controls] {
                if (!m_loading_provider_safety) markProviderSafetyDirty();
                updateFundingSafetyDisplay(controls);
                updateProviderButtons();
            });
        }
    }

    static bool readFundingSafety(const FundingSafetyControls& controls,
                                  FundingSafetyValues& values)
    {
        bool ok_per_transaction{false};
        bool ok_reserved{false};
        bool ok_per_hour{false};
        bool ok_per_day{false};
        ok_per_transaction = static_cast<DgbAmountLineEdit*>(
            controls.per_transaction)->satoshis(values.per_transaction);
        ok_reserved = static_cast<DgbAmountLineEdit*>(
            controls.reserved)->satoshis(values.reserved);
        ok_per_hour = static_cast<DgbAmountLineEdit*>(
            controls.per_hour)->satoshis(values.per_hour);
        ok_per_day = static_cast<DgbAmountLineEdit*>(
            controls.per_day)->satoshis(values.per_day);
        values.completed_per_hour = controls.completed_per_hour->value();
        values.completed_per_day = controls.completed_per_day->value();
        return ok_per_transaction && ok_reserved && ok_per_hour && ok_per_day;
    }

    static UniValue fundingSafetyToJSON(const FundingSafetyValues& values)
    {
        UniValue result{UniValue::VOBJ};
        result.pushKV("maximum_network_fee_per_transaction_satoshis", values.per_transaction);
        result.pushKV("maximum_reserved_network_fee_satoshis", values.reserved);
        result.pushKV("maximum_network_fee_per_hour_satoshis", values.per_hour);
        result.pushKV("maximum_network_fee_per_day_satoshis", values.per_day);
        result.pushKV("maximum_completed_per_hour", values.completed_per_hour);
        result.pushKV("maximum_completed_per_day", values.completed_per_day);
        return result;
    }

    static bool fundingSafetyCanRepresent(
        const UniValue& value, const FundingSafetyControls& controls)
    {
        if (!IsCompleteFundingSafety(value)) return false;
        qint64 per_transaction{0};
        qint64 reserved{0};
        qint64 per_hour{0};
        qint64 per_day{0};
        int completed_per_hour{0};
        int completed_per_day{0};
        return GetInt64Field(
                   value,
                   "maximum_network_fee_per_transaction_satoshis",
                   per_transaction) &&
            GetInt64Field(value,
                          "maximum_reserved_network_fee_satoshis",
                          reserved) &&
            GetInt64Field(value,
                          "maximum_network_fee_per_hour_satoshis",
                          per_hour) &&
            GetInt64Field(value,
                          "maximum_network_fee_per_day_satoshis",
                          per_day) &&
            GetIntField(value, "maximum_completed_per_hour",
                        completed_per_hour) &&
            GetIntField(value, "maximum_completed_per_day",
                        completed_per_day) &&
            dgbAmountCanRepresent(per_transaction) &&
            dgbAmountCanRepresent(reserved) &&
            dgbAmountCanRepresent(per_hour) &&
            dgbAmountCanRepresent(per_day) &&
            spinCanRepresent(controls.completed_per_hour,
                             completed_per_hour) &&
            spinCanRepresent(controls.completed_per_day,
                             completed_per_day);
    }

    bool providerSafetyCanRepresent(const UniValue& policy) const
    {
        int maximum_active_quotes_total{0};
        int maximum_active_quotes_per_netgroup{0};
        int maximum_active_quotes_per_recipient{0};
        int maximum_quote_requests_per_netgroup{0};
        return IsCompleteProviderSafetyPolicy(policy) &&
            fundingSafetyCanRepresent(policy.find_value("user_paid"),
                                      m_user_paid_safety) &&
            fundingSafetyCanRepresent(
                policy.find_value("public_sponsored"),
                m_public_sponsored_safety) &&
            fundingSafetyCanRepresent(
                policy.find_value("restricted_sponsored"),
                m_restricted_sponsored_safety) &&
            GetIntField(policy, "maximum_active_quotes_total",
                        maximum_active_quotes_total) &&
            GetIntField(policy, "maximum_active_quotes_per_netgroup",
                        maximum_active_quotes_per_netgroup) &&
            GetIntField(policy, "maximum_active_quotes_per_recipient",
                        maximum_active_quotes_per_recipient) &&
            GetIntField(
                policy,
                "maximum_quote_requests_per_netgroup_per_minute",
                maximum_quote_requests_per_netgroup) &&
            spinCanRepresent(m_max_active_quotes_total,
                             maximum_active_quotes_total) &&
            spinCanRepresent(m_max_active_quotes_per_netgroup,
                             maximum_active_quotes_per_netgroup) &&
            spinCanRepresent(m_max_active_quotes_per_recipient,
                             maximum_active_quotes_per_recipient) &&
            spinCanRepresent(m_max_quote_requests_per_netgroup,
                             maximum_quote_requests_per_netgroup);
    }

    void setProviderSafetyMutationEnabled(bool enabled)
    {
        const bool controls_enabled = enabled && !m_busy;
        if (m_provider_safety_group) {
            m_provider_safety_group->setEnabled(controls_enabled);
        }
        for (const FundingSafetyControls* controls : {
                 &m_user_paid_safety, &m_public_sponsored_safety,
                 &m_restricted_sponsored_safety}) {
            for (QLineEdit* amount : {
                     controls->per_transaction, controls->reserved,
                     controls->per_hour, controls->per_day}) {
                amount->setReadOnly(!controls_enabled);
            }
            controls->completed_per_hour->setEnabled(controls_enabled);
            controls->completed_per_day->setEnabled(controls_enabled);
        }
        for (QSpinBox* control : {
                 m_max_active_quotes_total,
                 m_max_active_quotes_per_netgroup,
                 m_max_active_quotes_per_recipient,
                 m_max_quote_requests_per_netgroup}) {
            control->setEnabled(controls_enabled);
        }
        if (m_save_provider_safety) {
            m_save_provider_safety->setEnabled(enabled && !m_busy);
        }
    }

    static void loadFundingSafety(const UniValue& value,
                                  const FundingSafetyControls& controls)
    {
        if (!IsCompleteFundingSafety(value)) return;
        static_cast<DgbAmountLineEdit*>(controls.per_transaction)->setSatoshis(
            value.find_value("maximum_network_fee_per_transaction_satoshis").getInt<qint64>());
        static_cast<DgbAmountLineEdit*>(controls.reserved)->setSatoshis(
            value.find_value("maximum_reserved_network_fee_satoshis").getInt<qint64>());
        static_cast<DgbAmountLineEdit*>(controls.per_hour)->setSatoshis(
            value.find_value("maximum_network_fee_per_hour_satoshis").getInt<qint64>());
        static_cast<DgbAmountLineEdit*>(controls.per_day)->setSatoshis(
            value.find_value("maximum_network_fee_per_day_satoshis").getInt<qint64>());
        controls.completed_per_hour->setValue(
            value.find_value("maximum_completed_per_hour").getInt<int>());
        controls.completed_per_day->setValue(
            value.find_value("maximum_completed_per_day").getInt<int>());
    }

    bool providerSafetyAllowsSelectedModels() const
    {
        // Core is authoritative for funding-model applicability and all limit
        // relationships. Qt only prevents activation with missing or unsaved
        // wallet-local policy data.
        return m_provider_safety_configured && !m_provider_safety_dirty;
    }

    QString providerStartConfirmationText() const
    {
        QStringList budgets;
        const auto append_budget = [this, &budgets](const QString& name,
                                                     const FundingSafetyControls& controls) {
            FundingSafetyValues values;
            if (!readFundingSafety(controls, values) || values.allZero()) return;
            budgets.push_back(tr(
                "%1: at most %2 DGB per transfer, %3 DGB per rolling hour and %4 DGB per rolling day; %5 transfers/hour and %6 transfers/day.")
                .arg(name)
                .arg(QString::number(values.per_transaction / 100000000.0, 'f', 8))
                .arg(QString::number(values.per_hour / 100000000.0, 'f', 8))
                .arg(QString::number(values.per_day / 100000000.0, 'f', 8))
                .arg(values.completed_per_hour)
                .arg(values.completed_per_day));
        };
        if (m_user_paid->isChecked()) {
            append_budget(tr("User paid"), m_user_paid_safety);
        }
        if (m_sponsored->isChecked() &&
            m_scope->currentData().toString() == QLatin1String("restricted")) {
            append_budget(tr("Restricted sponsored"),
                          m_restricted_sponsored_safety);
        } else if (m_sponsored->isChecked()) {
            append_budget(tr("Public sponsored"), m_public_sponsored_safety);
        }

        const bool automatic = m_operation_mode != QLatin1String("manual");
        QString liquidity_budget;
        LiquidityPolicyValues liquidity_values;
        if (automatic && m_liquidity_policy_configured &&
            !m_liquidity_policy_dirty &&
            readLiquidityPolicy(liquidity_values) &&
            liquidity_values.paid_maintenance_approved) {
            liquidity_budget = tr(
                "\nAutomatic liquidity maintenance: at most %1 DGB per maintenance transaction, %2 DGB per rolling hour and %3 DGB per rolling day.")
                .arg(QString::number(
                    liquidity_values.fee_per_transaction / 100000000.0, 'f', 8))
                .arg(QString::number(
                    liquidity_values.fee_per_hour / 100000000.0, 'f', 8))
                .arg(QString::number(
                    liquidity_values.fee_per_day / 100000000.0, 'f', 8));
        }
        return tr(
            "Start this provider now in %1 mode?\n\n%2%3\n\n"
            "Once online, the provider may accept client requests and spend DGB for network fees only within these persisted wallet-local limits. "
            "%4")
            .arg(automatic ? tr("automatic") : tr("manual expert"),
                  budgets.isEmpty() ? tr("No active funding-model budget is available.")
                                   : budgets.join('\n'),
                  liquidity_budget,
                  automatic
                     ? tr("Core will process eligible provider-side work without a separate dialog for every transfer. Clients must still confirm the exact provider and fee before signing.")
                     : tr("You must process waiting requests and submitted payments manually from Activity and recovery."));
    }

    QString providerServiceStatusText() const
    {
        if (m_service_state == QLatin1String("waiting_for_readiness") &&
            (m_last_service_error ==
                 QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
             (!m_liquidity_targets_satisfy_provider_policy &&
              m_last_service_error ==
                  QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
            return tr("Automatic start is waiting because the saved user-paid liquidity targets do not include an operational DigiDollar carrier. Review Liquidity, save a target of at least one payment carrier and prepare the missing output.");
        }
        if (m_service_state == QLatin1String("replenishing_liquidity") &&
            maintenanceFeeLimitExceeded()) {
            return tr(
                "Automatic refill is paused because its estimated network fee exceeds the saved per-transaction maintenance limit. No transaction was created and no DGB was spent. Review the refill cost limit in Liquidity before retrying.");
        }
        const QString error_suffix = m_last_service_error.isEmpty()
            ? QString{}
            : tr(" Stable error: %1").arg(m_last_service_error);
        const QString queue_suffix = tr(
            " Queue now: %1 request(s), %2 submitted payment(s).")
            .arg(m_waiting_provider_requests)
            .arg(m_waiting_provider_submits);
        if (!m_model) {
            return tr("No wallet is selected. Select a provider wallet first.");
        }
        if (m_service_state == QLatin1String("active")) {
            return tr("Automatic operation active — Core processes bounded request and submission work in the background. No manual queue action is required.") + queue_suffix;
        }
        if (m_service_state == QLatin1String("manual")) {
            return tr("Manual expert mode active — use the two processing actions below in order as messages arrive.") + queue_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_unlock")) {
            return tr("Automatic operation paused while the wallet is locked. Queued messages remain untouched and processing resumes after unlock.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_readiness")) {
            return tr("Autostart is enabled, but Core is waiting for provider readiness. Review Overview for the next required action.") + error_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
            return tr("Automatic operation is paused until finite paid liquidity-maintenance limits are explicitly approved. No maintenance transaction is created before approval.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("replenishing_liquidity")) {
            return tr("Automatic operation is restoring missing DGB or carrier slots within the saved maintenance budget. New requests wait until confirmed capacity is ready.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
            return tr("Replacement liquidity has been prepared and is waiting for blockchain confirmation. Existing authorized work remains recoverable.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("drain_only")) {
            return tr("Recovery-only operation active — Core rejects new quotes and automatically completes only previously authorized durable work.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("error")) {
            return tr("Automatic operation stopped on a safe, privacy-neutral error. Review the stable code and provider readiness before retrying.") + error_suffix;
        }
        if (!m_core_running) {
            if (!m_core_enabled) {
                return tr(
                    "Provider configuration is disabled. Autostart cannot bring this wallet online until the provider configuration is enabled again.");
            }
            return m_autostart_enabled
                ? tr("Provider stopped. Autostart is enabled and will start it when the wallet and readiness requirements permit.")
                : tr("Provider stopped — start it on Overview after completing setup. Automatic processing begins only after that conscious start.");
        }
        if (m_core_locked) {
            return tr("Provider paused while the wallet is locked. No queued message is consumed.");
        }
        return tr("Provider runtime state is being refreshed. Core exposes no manual action until a known safe state is available.");
    }

    void saveRuntimeSettings()
    {
        if (!hasRpcTransport() || m_busy || !m_runtime_settings_dirty) {
            m_runtime_settings_result->setText(!hasRpcTransport()
                ? tr("Runtime settings were not saved because no wallet RPC transport is available.")
                : m_busy
                    ? tr("Runtime settings were not saved because another wallet operation is still running.")
                    : tr("Runtime settings were not saved because there are no unsaved changes."));
            return;
        }
        const QString selected_mode =
            m_operation_mode_select->currentData().toString();
        const bool requested_autostart = m_autostart->isChecked();
        if (m_core_running && selected_mode != m_operation_mode) {
            QMessageBox::warning(
                this, tr("Stop provider before changing mode"),
                tr("Stop the provider from Overview before switching between automatic and manual operation. Autostart may still be changed while it is running."));
            return;
        }
        UniValue settings{UniValue::VOBJ};
        settings.pushKV("operation_mode", selected_mode.toStdString());
        settings.pushKV("autostart", requested_autostart);
        UniValue params{UniValue::VARR};
        params.push_back(std::move(settings));
        call("setpaymasterruntimesettings", std::move(params), false, nullptr,
             [this, selected_mode,
              requested_autostart](const UniValue& result) {
                 const UniValue& operation_mode =
                     result.find_value("operation_mode");
                 const UniValue& autostart = result.find_value("autostart");
                 if (!result.isObject() || !operation_mode.isStr() ||
                     !autostart.isBool() ||
                     QString::fromStdString(operation_mode.get_str()) !=
                         selected_mode ||
                     autostart.get_bool() != requested_autostart) {
                     m_runtime_settings_result->setText(tr(
                         "Runtime settings were not confirmed by Core. The displayed edit remains unsaved; refresh the provider status before retrying."));
                     updateProviderButtons();
                     return;
                 }
                 m_operation_mode = selected_mode;
                 m_autostart_enabled = requested_autostart;
                 m_provider_settings_present = true;
                 m_runtime_settings_dirty = false;
                 m_runtime_settings_result->setText(
                     m_operation_mode == QLatin1String("manual")
                         ? tr("Saved: manual expert processing. The provider must be stopped before returning to automatic mode.")
                         : m_autostart_enabled
                               ? tr("Saved: automatic processing with provider autostart. No passphrase is stored; a locked wallet waits for unlock.")
                               : tr("Saved: automatic processing after an explicit provider start. Provider autostart remains off."));
                 refreshStatus();
             }, /*show_error=*/false,
             [this](const QString& error) {
                 // Preserve the unsaved edit after a backend rejection and
                 // report the failure beside the affected controls. This
                 // prevents a failed RPC from looking like a successful save.
                 m_runtime_settings_result->setText(
                     tr("Runtime settings were not saved: %1").arg(error));
                 updateProviderButtons();
             });
    }

    void updateProviderButtons()
    {
        const bool complete_snapshots = hasCompleteMutationSnapshots();
        const bool safety_ready = complete_snapshots &&
            providerSafetyAllowsSelectedModels();
        const bool manual_mode = m_operation_mode == QLatin1String("manual");
        // Starting into maintenance is safe only when the saved policy can
        // actually restore every slot required by the active offer. In
        // particular, an intentionally released zero carrier target must not
        // be presented as a repairable shortfall.
        const bool automatic_liquidity_can_restore =
            m_liquidity_policy_configured &&
            m_liquidity_targets_satisfy_provider_policy &&
            m_automatic_replenishment->isChecked() &&
            m_paid_maintenance_approved->isChecked();
        const bool repairable_liquidity_gap =
            !m_readiness_errors.isEmpty() && !manual_mode &&
            automatic_liquidity_can_restore &&
            std::all_of(m_readiness_errors.cbegin(), m_readiness_errors.cend(),
                        [](const QString& error) {
                            return error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
                                   error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
                                   error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING");
                        });
        const bool can_start = m_core_ready || repairable_liquidity_gap;
        if (m_enable) {
            m_enable->setText(m_core_enabled
                ? tr("Disable provider configuration")
                : tr("Enable provider configuration"));
            m_enable->setEnabled(complete_snapshots && !m_busy &&
                (m_core_enabled || safety_ready));
            m_enable->setToolTip(m_core_enabled
                ? tr("Persistently disable this provider configuration and stop its runtime")
                : safety_ready
                ? tr("Enable the provider using the persisted finite safety limits")
                : tr("Save a complete finite safety policy for every selected funding model first"));
        }
        if (m_start) {
            m_start->setText(repairable_liquidity_gap && !m_core_running
                ? tr("Start and restore liquidity")
                : m_core_ready && !m_core_running
                    ? tr("Start provider now")
                    : tr("Start provider"));
            m_start->setEnabled(complete_snapshots && !m_busy &&
                                m_core_eligible && m_core_enabled && can_start &&
                                !m_core_running && !m_core_locked && safety_ready &&
                                !m_policy_dirty && !m_provider_safety_dirty &&
                                !m_runtime_settings_dirty &&
                                !m_liquidity_policy_dirty);
            if (m_policy_dirty || m_provider_safety_dirty ||
                m_runtime_settings_dirty) {
                m_start->setToolTip(tr(
                    "Save or discard the pending provider setting changes before starting. The start confirmation must show the persisted effective limits."));
            } else if (m_readiness_errors.contains(QStringLiteral(
                           "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
                       (!m_liquidity_targets_satisfy_provider_policy &&
                        m_readiness_errors.contains(QStringLiteral(
                            "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
                m_start->setToolTip(tr(
                    "The saved user-paid liquidity targets cannot make this provider ready. Review Liquidity and save at least three carrier targets for capacity checks and one carrier target for payments."));
            } else if (!repairable_liquidity_gap &&
                       std::any_of(
                           m_readiness_errors.cbegin(),
                           m_readiness_errors.cend(),
                           [](const QString& error) {
                               return error == QLatin1String(
                                          "PAYMASTER_ADMISSION_DGB_MISSING") ||
                                      error == QLatin1String(
                                          "PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
                                      error == QLatin1String(
                                          "PAYMASTER_OPERATIONAL_SLOT_MISSING");
                           })) {
                m_start->setToolTip(tr(
                    "Liquidity is missing, but automatic restoration is not fully configured and approved. Review the Liquidity page before starting."));
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
                m_start->setToolTip(tr(
                    "Provider start is waiting for blockchain synchronization and transaction broadcast readiness. No provider setting needs changing."));
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"))) {
                m_start->setToolTip(tr(
                    "Provider start is waiting for the transaction index to finish synchronizing."));
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"))) {
                m_start->setToolTip(tr(
                    "Provider start is waiting for DigiDollar activation on this chain."));
            } else {
                m_start->setToolTip(tr(
                    "Review the persisted effective budgets, then consciously start this provider runtime."));
            }
        }
        if (m_stop) {
            m_stop->setText(m_autostart_enabled
                ? tr("Stop and disable autostart")
                : tr("Stop provider"));
            m_stop->setToolTip(m_autostart_enabled
                ? tr("Persistently disable autostart, then stop the current provider runtime so it cannot restart on the next scheduler check.")
                : tr("Stop the current provider runtime."));
            m_stop->setEnabled(m_core_running && !m_busy);
        }
        if (m_request_processing_group) {
            m_request_processing_group->setVisible(manual_mode);
        }
        if (m_submit_processing_group) {
            m_submit_processing_group->setVisible(manual_mode);
        }
        if (m_activity_result_group) {
            m_activity_result_group->setVisible(manual_mode && !m_privacy);
        }
        if (m_process_requests) {
            m_process_requests->setEnabled(
                complete_snapshots && manual_mode && m_core_running &&
                !m_core_locked && !m_busy);
        }
        if (m_process_submits) {
            m_process_submits->setEnabled(
                complete_snapshots && manual_mode && m_core_running &&
                !m_core_locked && !m_busy);
        }
        if (m_operation_mode_select) {
            m_operation_mode_select->setEnabled(!m_busy && !m_core_running);
        }
        if (m_autostart) {
            m_autostart->setEnabled(!m_busy && hasRpcTransport());
        }
        if (m_save_runtime_settings) {
            m_save_runtime_settings->setEnabled(
                hasRpcTransport() && m_runtime_settings_dirty && !m_busy &&
                (!m_core_running ||
                 m_operation_mode_select->currentData().toString() ==
                     m_operation_mode));
        }
        if (m_activity_runtime_status) {
            m_activity_runtime_status->setText(providerServiceStatusText());
        }
        if (!complete_snapshots) {
            // A malformed response must revoke every transaction-producing
            // action immediately instead of leaving controls enabled from an
            // older valid snapshot.
            for (QPushButton* action : {
                     m_prepare_preview, m_prepare_execute,
                     m_rebalance_preview, m_rebalance_execute,
                     m_preview_carrier_excess,
                     m_execute_carrier_excess,
                     m_preview_carrier_release,
                     m_execute_carrier_release}) {
                if (action) action->setEnabled(false);
            }
        }
        if (m_operation_primary) {
            if (m_core_running) {
                m_operation_primary->setText(m_autostart_enabled
                    ? tr("Stop and disable autostart")
                    : tr("Stop provider"));
                m_operation_primary->setEnabled(!m_busy);
            } else if (m_start && m_start->isEnabled()) {
                m_operation_primary->setText(m_start->text());
                m_operation_primary->setEnabled(!m_busy);
            } else {
                m_operation_primary->setText(tr("Refresh provider status"));
                m_operation_primary->setEnabled(m_model && !m_busy);
            }
        }
        updateOperatorDashboard();
    }

    void setStatusLabel(QLabel* label, const QString& text,
                        const QString& kind) const
    {
        if (!label) return;
        QString symbol{QStringLiteral("•")};
        if (kind == QLatin1String("ready")) symbol = QStringLiteral("✓");
        if (kind == QLatin1String("waiting")) symbol = QStringLiteral("…");
        if (kind == QLatin1String("action")) symbol = QStringLiteral("!");
        if (kind == QLatin1String("error")) symbol = QStringLiteral("!");
        const QString rendered = QStringLiteral("%1  %2").arg(symbol, text);
        const bool kind_changed =
            label->property("statusKind").toString() != kind;
        if (label->text() != rendered) label->setText(rendered);
        if (kind_changed) {
            label->setProperty("statusKind", kind);
            if (label->style()) {
                label->style()->unpolish(label);
                label->style()->polish(label);
            }
        }
    }

    void setStatusKind(QWidget* widget, const QString& kind) const
    {
        if (!widget) return;
        if (widget->property("statusKind").toString() == kind) return;
        widget->setProperty("statusKind", kind);
        if (widget->style()) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
    }

    void updateOperatorDashboard()
    {
        const bool offer_ready = m_core_has_identity && m_policy_loaded &&
                                 !m_policy_dirty && m_core_enabled;
        setStatusLabel(
            m_overview_offer_status,
            offer_ready ? tr("Ready · identity and offer are saved")
                        : m_policy_dirty
                              ? tr("Action required · save the offer changes")
                              : !m_core_has_identity
                                    ? tr("Action required · create a provider identity")
                                    : tr("Action required · complete and enable the offer"),
            offer_ready ? QStringLiteral("ready") : QStringLiteral("action"));
        if (m_overview_offer_action) {
            m_overview_offer_action->setText(offer_ready ? tr("Review offer")
                                                         : tr("Complete offer"));
        }

        const bool safety_ready = providerSafetyAllowsSelectedModels() &&
                                  !m_provider_safety_dirty;
        setStatusLabel(
            m_overview_safety_status,
            safety_ready ? tr("Ready · finite spending limits are active")
                         : m_provider_safety_dirty
                               ? tr("Action required · save the changed limits")
                               : tr("Action required · set finite spending limits"),
            safety_ready ? QStringLiteral("ready") : QStringLiteral("action"));
        if (m_overview_safety_action) {
            m_overview_safety_action->setText(safety_ready ? tr("Review limits")
                                                           : tr("Set limits"));
        }

        const int ready_dgb = m_pool_admission_dgb + m_pool_operational_dgb;
        const int target_dgb = m_admission_dgb->value() + m_operational_dgb->value();
        const int ready_carriers = m_pool_admission_carriers + m_pool_operational_carriers;
        const int target_carriers = m_user_paid->isChecked()
            ? m_admission_carriers->value() + m_operational_carriers->value()
            : 0;
        const bool liquidity_ready = m_pool_status_loaded &&
                                     m_liquidity_targets_satisfy_provider_policy &&
                                     ready_dgb >= target_dgb &&
                                     ready_carriers >= target_carriers;
        const bool liquidity_waiting = m_maintenance_state == QLatin1String("replenishing") ||
                                       m_maintenance_state == QLatin1String("waiting_for_confirmation") ||
                                       m_service_state == QLatin1String("replenishing_liquidity") ||
                                       m_service_state == QLatin1String("waiting_for_liquidity_confirmation");
        const bool refill_approval_needed =
            m_maintenance_state ==
            QLatin1String("waiting_for_maintenance_approval");
        const bool refill_waiting_for_start =
            shouldStartAndRestoreLiquidity() && !liquidity_ready;
        const bool refill_fee_limit_exceeded =
            maintenanceFeeLimitExceeded() && !liquidity_ready;
        setStatusLabel(
            m_overview_liquidity_status,
            liquidity_ready ? tr("Ready · target capacity is available")
                            : !m_liquidity_targets_satisfy_provider_policy
                                  ? tr("Action required · saved carrier targets cannot support the active offer")
                            : refill_approval_needed
                                  ? tr("Action required · targets are saved; approve the bounded refill cost")
                            : refill_fee_limit_exceeded
                                  ? tr("Action required · refill cost exceeds the approved limit")
                            : refill_waiting_for_start
                                  ? tr("Action required · refill approved; start the provider to restore liquidity")
                            : liquidity_waiting
                                  ? tr("Waiting · replacement liquidity is being prepared")
                                  : tr("Action required · liquidity is below its target"),
            liquidity_ready ? QStringLiteral("ready")
                            : !m_liquidity_targets_satisfy_provider_policy
                                  ? QStringLiteral("action")
                                  : refill_fee_limit_exceeded
                                        ? QStringLiteral("action")
                                  : liquidity_waiting
                                        ? QStringLiteral("waiting")
                                        : QStringLiteral("action"));
        if (m_overview_liquidity_action) {
            m_overview_liquidity_action->setText(
                liquidity_ready
                    ? tr("Review liquidity")
                    : !m_liquidity_targets_satisfy_provider_policy
                          ? tr("Review liquidity targets")
                    : refill_approval_needed
                          ? tr("Approve refill costs")
                    : refill_fee_limit_exceeded
                          ? tr("Review refill cost limit")
                    : refill_waiting_for_start
                          ? tr("Start and restore liquidity")
                          : tr("Restore liquidity"));
        }

        QString operation_kind{QStringLiteral("action")};
        QString operation_text;
        if (m_service_state == QLatin1String("active")) {
            operation_kind = QStringLiteral("ready");
            operation_text = tr("Ready · automatic provider operation is active");
        } else if (m_readiness_errors.contains(QStringLiteral(
                       "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
                   (!m_liquidity_targets_satisfy_provider_policy &&
                    (m_last_service_error == QLatin1String(
                         "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
                     m_last_service_error == QLatin1String(
                         "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
            operation_text = tr(
                "Action required · save a payment-carrier target before starting");
        } else if (refill_fee_limit_exceeded) {
            operation_kind = QStringLiteral("action");
            operation_text = tr(
                "Paused · automatic refill exceeds the approved cost limit");
        } else if (m_service_state.startsWith(QLatin1String("waiting_")) ||
                   m_service_state == QLatin1String("replenishing_liquidity")) {
            operation_kind = QStringLiteral("waiting");
            operation_text = providerServiceStatusText();
        } else if (m_service_state == QLatin1String("error")) {
            operation_kind = QStringLiteral("error");
            operation_text = tr("Error · provider operation requires attention");
        } else if (providerConfigurationComplete() &&
                   m_readiness_errors.contains(
                       QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
            operation_kind = QStringLiteral("waiting");
            operation_text = Params().GetChainType() == ChainType::REGTEST
                ? tr("Waiting · mine a fresh Regtest block; provider settings are already saved")
                : tr("Waiting · node is not ready to broadcast transactions");
        } else if (refill_approval_needed) {
            operation_kind = QStringLiteral("action");
            operation_text = tr(
                "Action required · approve bounded refill costs; targets are already saved");
        } else if (refill_waiting_for_start) {
            operation_kind = QStringLiteral("action");
            operation_text = tr(
                "Action required · start the provider to restore missing liquidity");
        } else if (m_core_ready) {
            operation_text = tr("Action required · provider is ready to start");
        } else {
            operation_text = tr("Action required · complete the remaining setup steps");
        }
        setStatusLabel(m_overview_operation_status, operation_text, operation_kind);
        if (m_overview_operation_action) {
            if (!m_liquidity_targets_satisfy_provider_policy) {
                m_overview_operation_action->setText(
                    tr("Review liquidity targets"));
            } else if (m_service_state == QLatin1String("active")) {
                m_overview_operation_action->setText(tr("Open operations"));
            } else if (providerConfigurationComplete() &&
                       hasPassiveExternalWait()) {
                m_overview_operation_action->setText(tr("Refresh status"));
            } else if (refill_approval_needed) {
                m_overview_operation_action->setText(
                    tr("Approve refill costs"));
            } else if (refill_fee_limit_exceeded) {
                m_overview_operation_action->setText(
                    tr("Review refill cost limit"));
            } else if (refill_waiting_for_start) {
                m_overview_operation_action->setText(
                    tr("Start and restore liquidity"));
            } else {
                m_overview_operation_action->setText(tr("Review operation"));
            }
        }

        setStatusLabel(
            m_liquidity_dgb_capacity_status,
            (ready_dgb >= target_dgb
                 ? tr("Ready — enough DGB is prepared.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)")
                 : tr("Action required — more DGB capacity is needed.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)"))
                .arg(m_pool_admission_dgb)
                .arg(m_admission_dgb->value())
                .arg(m_pool_operational_dgb)
                .arg(m_operational_dgb->value()),
            ready_dgb >= target_dgb ? QStringLiteral("ready")
                                    : QStringLiteral("action"));
        const bool carrier_capacity_ready =
            !m_user_paid->isChecked() ||
            (m_liquidity_targets_satisfy_provider_policy &&
             ready_carriers >= target_carriers);
        QString carrier_capacity_text;
        if (!m_user_paid->isChecked()) {
            carrier_capacity_text = tr(
                "Not required. This provider currently offers sponsored transfers without a DigiDollar service fee.");
        } else if (!m_liquidity_targets_satisfy_provider_policy) {
            carrier_capacity_text = tr(
                "Action required — the saved user-paid targets omit payment-carrier capacity. Save at least three carrier targets for capacity checks and one carrier target for payments before starting.");
        } else {
            carrier_capacity_text =
                (ready_carriers >= target_carriers
                     ? tr("Ready — DigiDollar service fees can be received.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)")
                     : tr("Action required — more DigiDollar fee capacity is needed.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)"))
                    .arg(m_pool_admission_carriers)
                    .arg(m_admission_carriers->value())
                    .arg(m_pool_operational_carriers)
                    .arg(m_operational_carriers->value());
        }
        setStatusLabel(m_liquidity_carrier_capacity_status,
                       carrier_capacity_text,
                       carrier_capacity_ready ? QStringLiteral("ready")
                                              : QStringLiteral("action"));
    }

    void stopProvider()
    {
        if (!m_core_running || m_busy) return;
        const auto stop_runtime = [this] {
            call("stoppaymaster", {}, false, nullptr,
                 [this](const UniValue& result) {
                     const UniValue& running = result.find_value("running");
                     if (!result.isObject() || !running.isBool() ||
                         running.get_bool()) {
                         const QString message = tr(
                             "Core did not confirm that the provider runtime stopped. The displayed state will be refreshed before another action is allowed.");
                         if (m_rpc_executor_for_testing) {
                             m_status->setText(message);
                         } else {
                             QMessageBox::warning(
                                 this, tr("Paymaster provider not stopped"),
                                 message);
                         }
                         refreshStatus();
                         return;
                     }
                     m_core_running = false;
                     m_service_state = QStringLiteral("stopped");
                     updateProviderButtons();
                     refreshStatus();
                 }, false,
                 [this](const QString& error) {
                     showPlainTextWarning(
                         this, tr("Paymaster provider not stopped"),
                         tr("The provider could not be stopped.\n\n%1")
                             .arg(error));
                     refreshStatus();
                 });
        };
        if (!m_autostart_enabled) {
            stop_runtime();
            return;
        }
        if (!m_rpc_executor_for_testing &&
            askPlainTextQuestion(
                this, tr("Stop Paymaster provider"),
                tr("Autostart is currently enabled. Stopping only the in-memory runtime would allow it to start again automatically. Disable autostart persistently and then stop the provider?")) !=
                QMessageBox::Yes) {
            return;
        }
        UniValue settings{UniValue::VOBJ};
        settings.pushKV("operation_mode", m_operation_mode.toStdString());
        settings.pushKV("autostart", false);
        UniValue params{UniValue::VARR};
        params.push_back(std::move(settings));
        call("setpaymasterruntimesettings", std::move(params), false, nullptr,
             [this, stop_runtime](const UniValue& result) {
                 const UniValue& operation_mode =
                     result.find_value("operation_mode");
                 const UniValue& autostart = result.find_value("autostart");
                 if (!result.isObject() || !operation_mode.isStr() ||
                     QString::fromStdString(operation_mode.get_str()) !=
                         m_operation_mode ||
                     !autostart.isBool() || autostart.get_bool()) {
                     const QString message = tr(
                         "Core did not confirm the unchanged operation mode and disabled autostart, so the provider was not stopped.");
                     if (m_rpc_executor_for_testing) {
                         m_status->setText(message);
                     } else {
                         QMessageBox::warning(
                             this, tr("Autostart not disabled"), message);
                     }
                     refreshStatus();
                     return;
                 }
                 m_autostart_enabled = false;
                 m_runtime_settings_dirty = false;
                 m_loading_runtime_settings = true;
                 m_autostart->setChecked(false);
                 m_loading_runtime_settings = false;
                 stop_runtime();
             }, false,
             [this](const QString& error) {
                 showPlainTextWarning(
                     this, tr("Autostart not disabled"),
                     tr("Autostart could not be disabled, so the provider was left running rather than being stopped only transiently.\n\n%1")
                         .arg(error));
                 refreshStatus();
             });
    }

    void enableProvider()
    {
        if (!hasCompleteMutationSnapshots()) {
            m_enable_status->setText(tr(
                "Provider configuration was not changed because a complete current provider, safety and liquidity snapshot is unavailable."));
            updateProviderButtons();
            return;
        }
        if (m_core_enabled) {
            if (!m_rpc_executor_for_testing &&
                askPlainTextQuestion(
                    this, tr("Disable Paymaster provider"),
                    tr("Disable this wallet's provider configuration persistently? Core will also stop its current runtime and autostart will no longer bring it online while disabled.")) !=
                    QMessageBox::Yes) {
                return;
            }
            if (m_busy) return;
            m_enable_status->setText(
                tr("Disabling provider configuration…"));
            UniValue params{UniValue::VARR};
            params.push_back(false);
            call("setpaymasterenabled", std::move(params), false, nullptr,
                 [this](const UniValue& result) {
                     const UniValue& enabled = result.find_value("enabled");
                     if (!enabled.isBool() || enabled.get_bool()) {
                         m_enable_status->setText(tr(
                             "Provider configuration was not disabled: the wallet returned an unexpected result."));
                         updateProviderButtons();
                         return;
                     }
                     m_core_enabled = false;
                     m_core_running = false;
                     m_provider_settings_present = true;
                     m_enable_status->setText(tr(
                         "Provider configuration is disabled and saved in this wallet."));
                     updateProviderButtons();
                     refreshStatus();
                 }, false,
                 [this](const QString& error) {
                     m_enable_status->setText(tr(
                         "Provider configuration could not be disabled: %1")
                             .arg(error));
                     updateProviderButtons();
                 });
            return;
        }
        if (!providerSafetyAllowsSelectedModels()) {
            m_enable_status->setText(tr(
                "Provider configuration was not enabled: save a complete finite safety policy first."));
            QMessageBox::warning(
                this, tr("Paymaster safety policy required"),
                tr("The provider cannot be enabled until a complete wallet-local safety policy "
                   "has been saved. Public sponsorship requires explicit finite non-zero "
                   "hourly and daily budgets."));
            return;
        }
        if (m_busy) {
            m_enable_status->setText(tr(
                "Provider configuration was not enabled because another wallet operation is still running. Please try again."));
            QMessageBox::information(
                this, tr("Paymaster operation in progress"),
                tr("Wait for the current wallet operation to finish, then enable the provider configuration again."));
            return;
        }
        m_enable_status->setText(tr("Enabling provider configuration…"));
        m_enable->setEnabled(false);
        UniValue params{UniValue::VARR};
        params.push_back(true);
        call("setpaymasterenabled", std::move(params), false, nullptr,
             [this](const UniValue& result) {
                 const UniValue& enabled = result.find_value("enabled");
                 if (!enabled.isBool() || !enabled.get_bool()) {
                     m_enable_status->setText(tr(
                         "Provider configuration was not enabled: the wallet returned an unexpected result."));
                     QMessageBox::warning(
                         this, tr("Provider configuration not enabled"),
                         tr("The wallet did not confirm that provider configuration was enabled."));
                     updateProviderButtons();
                     return;
                 }
                 m_core_enabled = true;
                 m_provider_settings_present = true;
                 m_enable_status->setText(m_autostart_enabled
                     ? tr("Provider configuration enabled successfully. It is saved in this wallet and may start automatically as soon as every readiness requirement passes.")
                     : tr("Provider configuration enabled successfully. It is saved in this wallet. Autostart is disabled, so it remains offline until you start it from Overview."));
                 updateProviderButtons();
                 QMessageBox::information(
                     this, tr("Provider configuration enabled"),
                     m_autostart_enabled
                         ? tr("Provider configuration was enabled successfully and saved in this wallet. Autostart may bring it online as soon as all readiness checks pass.")
                         : tr("Provider configuration was enabled successfully and saved in this wallet. Autostart is disabled; start it from Overview after all readiness checks pass."));
                 refreshStatus();
             }, false,
             [this](const QString& error) {
                 m_enable_status->setText(tr(
                     "Provider configuration could not be enabled: %1").arg(error));
                 updateProviderButtons();
                 showPlainTextWarning(
                     this, tr("Provider configuration not enabled"),
                     tr("The provider configuration could not be enabled.\n\n%1").arg(error));
             });
    }

    void saveProviderSafetyPolicy()
    {
        if (!m_provider_safety_snapshot_representable) {
            m_provider_safety_status->setText(tr(
                "Provider safety settings were not changed: the persisted Core policy cannot be represented exactly by this interface."));
            return;
        }
        FundingSafetyValues user_paid;
        FundingSafetyValues public_sponsored;
        FundingSafetyValues restricted_sponsored;
        if (!readFundingSafety(m_user_paid_safety, user_paid) ||
            !readFundingSafety(m_public_sponsored_safety, public_sponsored) ||
            !readFundingSafety(m_restricted_sponsored_safety,
                               restricted_sponsored)) {
            QMessageBox::warning(
                this, tr("Paymaster provider safety policy"),
                tr("One or more amounts are outside the supported whole-satoshi range."));
            return;
        }
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("user_paid", fundingSafetyToJSON(user_paid));
        policy.pushKV("public_sponsored", fundingSafetyToJSON(public_sponsored));
        policy.pushKV("restricted_sponsored", fundingSafetyToJSON(restricted_sponsored));
        policy.pushKV("maximum_active_quotes_total", m_max_active_quotes_total->value());
        policy.pushKV("maximum_active_quotes_per_netgroup", m_max_active_quotes_per_netgroup->value());
        policy.pushKV("maximum_active_quotes_per_recipient", m_max_active_quotes_per_recipient->value());
        policy.pushKV("maximum_quote_requests_per_netgroup_per_minute",
                      m_max_quote_requests_per_netgroup->value());
        const UniValue requested_policy = policy;
        UniValue params{UniValue::VARR};
        params.push_back(std::move(policy));
        call("setpaymastersafetypolicy", std::move(params), false, nullptr,
             [this, requested_policy](const UniValue& result) {
                 if (!IsExactProviderSafetyAcknowledgement(
                         requested_policy, result)) {
                     m_provider_safety_status->setText(tr(
                         "Core did not confirm the exact provider safety policy. The displayed edit remains unsaved; refresh before retrying."));
                     return;
                 }
                 m_provider_safety_dirty = false;
                 refreshProviderSafetyStatus();
             });
    }

    void saveClientSafetyPolicy()
    {
        if (!m_client_safety_snapshot_representable) {
            m_client_safety_status->setText(tr(
                "Client safety limits were not changed: the persisted Core policy cannot be represented exactly by this interface."));
            return;
        }
        const int requested_per_transaction =
            m_client_fee_per_transaction->value();
        const int requested_per_day = m_client_fee_per_day->value();
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("maximum_service_fee_per_transaction_cents",
                      requested_per_transaction);
        policy.pushKV("maximum_service_fee_per_day_cents",
                      requested_per_day);
        UniValue params{UniValue::VARR};
        params.push_back(std::move(policy));
        call("setpaymasterclientsafetypolicy", std::move(params), false, nullptr,
             [this, requested_per_transaction,
              requested_per_day](const UniValue& result) {
                 if (!IsCompleteClientSafetyPolicy(result)) {
                     m_client_safety_status->setText(tr(
                         "Client safety limits were not confirmed by Core. The displayed edit remains unsaved."));
                     return;
                 }
                 int persisted_per_transaction{0};
                 int persisted_per_day{0};
                 try {
                     persisted_per_transaction = result.find_value(
                         "maximum_service_fee_per_transaction_cents")
                                                    .getInt<int>();
                     persisted_per_day = result.find_value(
                         "maximum_service_fee_per_day_cents").getInt<int>();
                 } catch (const std::exception&) {
                     m_client_safety_status->setText(tr(
                         "Client safety limits were outside the supported range. The displayed edit remains unsaved."));
                     return;
                 }
                 if (persisted_per_transaction != requested_per_transaction ||
                     persisted_per_day != requested_per_day) {
                     m_client_safety_status->setText(tr(
                         "Core did not confirm the exact client safety limits. The displayed edit remains unsaved."));
                     return;
                 }
                 m_client_safety_dirty = false;
                 refreshClientSafetyStatus();
             });
    }

    QString safetyClassStatus(const QString& name, const UniValue& value) const
    {
        if (!HasInt64Fields(
                value,
                {"active_quotes", "reserved_network_fee_satoshis",
                 "spent_network_fee_last_hour_satoshis",
                 "spent_network_fee_last_day_satoshis",
                 "completed_last_hour", "completed_last_day"}) ||
            !value.find_value("can_accept_minimum_quote").isBool()) {
            return tr("%1: unavailable").arg(name);
        }
        QString status = tr("%1: %2 active, %3 sat reserved, %4 sat spent/hour, %5 sat spent/day, "
                            "%6 completed/hour, %7 completed/day, accepting: %8")
            .arg(name)
            .arg(value.find_value("active_quotes").getInt<qint64>())
            .arg(value.find_value("reserved_network_fee_satoshis").getInt<qint64>())
            .arg(value.find_value("spent_network_fee_last_hour_satoshis").getInt<qint64>())
            .arg(value.find_value("spent_network_fee_last_day_satoshis").getInt<qint64>())
            .arg(value.find_value("completed_last_hour").getInt<qint64>())
            .arg(value.find_value("completed_last_day").getInt<qint64>())
            .arg(yesNo(value.find_value("can_accept_minimum_quote").get_bool()));
        QStringList errors;
        const UniValue& error_values = value.find_value("errors");
        if (error_values.isArray()) {
            for (const UniValue& error : error_values.getValues()) {
                if (error.isStr()) errors.push_back(QString::fromStdString(error.get_str()));
            }
        }
        if (!errors.isEmpty()) status += tr(" · limits: %1").arg(errors.join(tr(", ")));
        return status;
    }

    void refreshProviderSafetyStatus()
    {
        if (!m_client_safety_dirty) {
            m_client_safety_status->setText(
                tr("Client safety policy: unavailable (automatic Paymaster transfers unavailable)"));
        }
        updateProviderButtons();
        call("getpaymastersafetystatus", {}, false, nullptr,
             [this](const UniValue& result) {
                 const UniValue& configured = result.find_value("configured");
                 const UniValue& policy = result.find_value("policy");
                 if (!result.isObject() || !configured.isBool() ||
                     (configured.get_bool() &&
                      !IsCompleteProviderSafetyPolicy(policy))) {
                     m_provider_safety_snapshot_available = false;
                     m_provider_safety_snapshot_representable = false;
                     setProviderSafetyMutationEnabled(false);
                     m_provider_safety_status->setText(tr(
                         "Core returned an incomplete provider safety status. Guided setup remains unavailable until a complete refresh succeeds."));
                     updateProviderButtons();
                     refreshClientSafetyStatus();
                     return;
                 }
                 if (configured.get_bool() &&
                     !providerSafetyCanRepresent(policy)) {
                     m_unrepresentable_provider_safety_snapshot = policy;
                     m_provider_safety_snapshot_representable = false;
                     m_provider_safety_snapshot_available = false;
                     m_provider_safety_configured = false;
                     m_provider_safety_dirty = false;
                     setProviderSafetyMutationEnabled(false);
                     m_provider_safety_status->setText(tr(
                         "Core returned a complete provider safety policy containing a value that this interface cannot represent exactly. The persisted policy was retained unchanged and all provider-safety mutation is disabled."));
                     updateProviderButtons();
                     refreshClientSafetyStatus();
                     return;
                 }
                 m_unrepresentable_provider_safety_snapshot =
                     UniValue{UniValue::VOBJ};
                 m_provider_safety_snapshot_representable = true;
                 setProviderSafetyMutationEnabled(true);
                 m_provider_safety_snapshot_available = true;
                 m_provider_safety_configured =
                     result.find_value("configured").isBool() &&
                     result.find_value("configured").get_bool();
                 if (policy.isObject() && !m_provider_safety_dirty) {
                     m_loading_provider_safety = true;
                     loadFundingSafety(policy.find_value("user_paid"), m_user_paid_safety);
                     loadFundingSafety(policy.find_value("public_sponsored"), m_public_sponsored_safety);
                     loadFundingSafety(policy.find_value("restricted_sponsored"), m_restricted_sponsored_safety);
                     m_max_active_quotes_total->setValue(
                         policy.find_value("maximum_active_quotes_total").getInt<int>());
                     m_max_active_quotes_per_netgroup->setValue(
                         policy.find_value("maximum_active_quotes_per_netgroup").getInt<int>());
                     m_max_active_quotes_per_recipient->setValue(
                         policy.find_value("maximum_active_quotes_per_recipient").getInt<int>());
                     m_max_quote_requests_per_netgroup->setValue(
                         policy.find_value("maximum_quote_requests_per_netgroup_per_minute").getInt<int>());
                     m_loading_provider_safety = false;
                     m_provider_safety_dirty = false;
                     updateFundingSafetyDisplay(m_user_paid_safety,
                                                m_provider_safety_configured);
                     updateFundingSafetyDisplay(m_public_sponsored_safety,
                                                m_provider_safety_configured);
                     updateFundingSafetyDisplay(m_restricted_sponsored_safety,
                                                m_provider_safety_configured);
                 }
                 if (!m_provider_safety_dirty) {
                     m_provider_safety_status->setText(m_provider_safety_configured
                         ? tr("Provider safety policy: configured and persisted")
                         : tr("Provider safety policy: not configured (provider not ready)"));
                 }
                 QStringList usage;
                 usage.push_back(safetyClassStatus(tr("User paid"), result.find_value("user_paid")));
                 usage.push_back(safetyClassStatus(tr("Public sponsored"), result.find_value("public_sponsored")));
                 usage.push_back(safetyClassStatus(tr("Restricted sponsored"), result.find_value("restricted_sponsored")));
                 m_provider_safety_usage->setText(usage.join('\n'));
                 updateProviderButtons();
                 refreshClientSafetyStatus();
             }, false, [this](const QString& error) {
                 m_provider_safety_snapshot_available = false;
                 m_provider_safety_snapshot_representable = false;
                 setProviderSafetyMutationEnabled(false);
                 m_provider_safety_status->setText(tr(
                     "Provider safety status unavailable: %1. Saved values are retained locally but cannot be used by guided setup until a complete refresh succeeds.")
                         .arg(error));
                 updateProviderButtons();
                 refreshClientSafetyStatus();
             });
    }

    void refreshClientSafetyStatus()
    {
        if (!m_client_safety_dirty) {
            m_client_safety_status->setText(
                tr("Client safety policy: unavailable (automatic Paymaster transfers unavailable)"));
        }
        call("getpaymasterclientsafetystatus", {}, false, nullptr,
             [this](const UniValue& result) {
                 if (!IsCompleteClientSafetyStatus(result)) {
                     m_client_safety_configured = false;
                     m_client_safety_snapshot_representable = false;
                     if (m_client_safety_group) {
                         m_client_safety_group->setEnabled(false);
                     }
                     m_client_safety_status->setText(tr(
                         "Core returned an incomplete client-safety status. Automatic Paymaster transfers remain unavailable until a complete refresh succeeds."));
                     return;
                 }
                 const bool configured =
                     result.find_value("configured").get_bool();
                 m_client_safety_configured = configured;
                 const UniValue& policy = result.find_value("policy");
                 if (!policy.isObject()) {
                     m_unrepresentable_client_safety_snapshot =
                         UniValue{UniValue::VOBJ};
                     m_client_safety_snapshot_representable = true;
                     if (m_client_safety_group) {
                         m_client_safety_group->setEnabled(!m_busy);
                     }
                 }
                 if (policy.isObject() && !m_client_safety_dirty) {
                     qint64 per_transaction{0};
                     qint64 per_day{0};
                     try {
                         per_transaction = policy.find_value(
                             "maximum_service_fee_per_transaction_cents")
                                               .getInt<qint64>();
                         per_day = policy.find_value(
                             "maximum_service_fee_per_day_cents")
                                       .getInt<qint64>();
                     } catch (const std::exception&) {
                         m_client_safety_configured = false;
                         m_client_safety_status->setText(tr(
                             "Core returned client-safety values outside the supported range. Automatic Paymaster transfers remain unavailable."));
                         return;
                     }
                     if (!spinCanRepresent(m_client_fee_per_transaction,
                                           per_transaction) ||
                         !spinCanRepresent(m_client_fee_per_day, per_day)) {
                         m_unrepresentable_client_safety_snapshot = policy;
                         m_client_safety_snapshot_representable = false;
                         m_client_safety_configured = false;
                         if (m_client_safety_group) {
                             m_client_safety_group->setEnabled(false);
                         }
                         m_client_safety_status->setText(tr(
                             "Core returned client-safety limits that this interface cannot represent exactly. The persisted values were not changed and automatic Paymaster transfers remain unavailable here."));
                         return;
                     }
                     m_unrepresentable_client_safety_snapshot =
                         UniValue{UniValue::VOBJ};
                     m_client_safety_snapshot_representable = true;
                     if (m_client_safety_group) {
                         m_client_safety_group->setEnabled(!m_busy);
                     }
                     m_loading_client_safety = true;
                     m_client_fee_per_transaction->setValue(
                         static_cast<int>(per_transaction));
                     m_client_fee_per_day->setValue(
                         static_cast<int>(per_day));
                     m_loading_client_safety = false;
                     m_client_safety_dirty = false;
                     updateClientSafetyDisplay(configured);
                 }
                 if (m_client_safety_dirty) return;
                 if (!m_client_safety_configured) {
                     m_client_safety_status->setText(
                         tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"));
                     return;
                 }
                 qint64 active_reservations{0};
                 qint64 reserved_cents{0};
                 qint64 spent_cents{0};
                 qint64 available_cents{0};
                 try {
                     active_reservations = result.find_value(
                         "active_reservations").getInt<qint64>();
                     reserved_cents = result.find_value(
                         "reserved_service_fee_cents").getInt<qint64>();
                     spent_cents = result.find_value(
                         "spent_service_fee_last_day_cents").getInt<qint64>();
                     available_cents = result.find_value(
                         "available_service_fee_today_cents").getInt<qint64>();
                 } catch (const std::exception&) {
                     m_client_safety_configured = false;
                     m_client_safety_status->setText(tr(
                         "Core returned client-safety usage outside the supported range. Automatic Paymaster transfers remain unavailable."));
                     return;
                 }
                 if (active_reservations < 0 || reserved_cents < 0 ||
                     spent_cents < 0 || available_cents < 0) {
                     m_client_safety_configured = false;
                     m_client_safety_status->setText(tr(
                         "Core returned invalid client-safety usage. Automatic Paymaster transfers remain unavailable."));
                     return;
                 }
                  m_client_safety_status->setText(
                      tr("Client safety policy: configured · active reservations %1 · "
                         "reserved %2 cents · spent today %3 cents · available today %4 cents")
                          .arg(active_reservations)
                          .arg(reserved_cents)
                          .arg(spent_cents)
                          .arg(available_cents));
             }, false);
    }

    static QSpinBox* spin(QWidget* parent, int minimum, int maximum, int value)
    {
        auto* result = new NoWheelSpinBox(parent);
        result->setRange(minimum, maximum);
        result->setValue(value);
        return result;
    }

    static QLineEdit* dgbAmountField(QWidget* parent, qint64 satoshis)
    {
        return new DgbAmountLineEdit(satoshis, parent);
    }

    static QSpinBox* scaledSpin(QWidget* parent, int minimum, int maximum,
                                int value, double scale, int decimals)
    {
        auto* result = new NoWheelScaledSpinBox(scale, decimals, parent);
        result->setRange(minimum, maximum);
        result->setValue(value);
        return result;
    }

    QString yesNo(bool value) const { return value ? tr("yes") : tr("no"); }

    QString liquidityTargetKey() const
    {
        return QStringLiteral("%1:%2:%3:%4")
            .arg(m_admission_dgb->value())
            .arg(m_operational_dgb->value())
            .arg(m_admission_carriers->value())
            .arg(m_operational_carriers->value());
    }

    void invalidatePoolPreviews()
    {
        m_prepare_preview_target.clear();
        m_rebalance_preview_target.clear();
        m_prepare_plan_id.clear();
        m_rebalance_plan_id.clear();
    }

    void restoreLiquidityDefaults(bool announce = true)
    {
        const bool needs_carriers = m_user_paid->isChecked();
        m_loading_liquidity_policy = true;
        m_automatic_replenishment->setChecked(true);
        m_paid_maintenance_approved->setChecked(false);
        m_admission_dgb->setValue(3);
        m_operational_dgb->setValue(1);
        m_admission_carriers->setValue(needs_carriers ? 3 : 0);
        m_operational_carriers->setValue(needs_carriers ? 1 : 0);
        m_maintenance_fee_per_transaction->setText(
            QString::number(10000000));
        m_maintenance_fee_per_hour->setText(QString::number(50000000));
        m_maintenance_fee_per_day->setText(QString::number(200000000));
        m_loading_liquidity_policy = false;
        m_liquidity_policy_dirty = announce;
        invalidatePoolPreviews();
        invalidateCarrierWithdrawalPreviews();
        updateLiquidityDisplay();
        if (announce) {
            m_liquidity_policy_status->setText(tr(
                "Recommended targets and conservative finite maintenance limits restored in the form. Paid maintenance is disabled until you review, approve and save these values."));
            m_liquidity_output->setPlainText(tr(
                "Recommended target fields restored. No wallet output was created or retired. Save the automatic liquidity policy, then preview pool preparation only if you are using manual expert operation."));
        } else {
            m_liquidity_output->clear();
        }
        updateProviderButtons();
    }

    void updateLiquidityDisplay()
    {
        if (!m_liquidity_summary || !m_liquidity_preview_status) return;
        const bool needs_carriers = m_user_paid->isChecked();
        const bool sponsored = m_sponsored->isChecked();
        QString summary = tr(
            "Displayed targets: %1 admission DGB, %2 operational DGB, %3 admission carrier and %4 operational carrier slots.")
            .arg(m_admission_dgb->value())
            .arg(m_operational_dgb->value())
            .arg(m_admission_carriers->value())
            .arg(m_operational_carriers->value());
        if (!needs_carriers && !sponsored) {
            summary += tr(
                " No payment model is selected; choose and save an operating policy before preparing liquidity.");
        } else if (needs_carriers &&
            (m_admission_carriers->value() < 3 ||
             m_operational_carriers->value() < 1)) {
            summary += tr(
                " User paid is selected, so at least three admission and one operational carrier slots are required.");
        } else if (!needs_carriers &&
                   (m_admission_carriers->value() > 0 ||
                    m_operational_carriers->value() > 0)) {
            summary += tr(
                " User paid is not selected; these carrier targets are optional and can be reset to zero.");
        } else if (needs_carriers) {
            summary += tr(" These targets satisfy the minimum user-paid pool shape.");
        } else {
            summary += tr(" These targets satisfy the minimum sponsored-only pool shape.");
        }
        m_liquidity_summary->setText(summary);

        if (m_liquidity_target_save_status) {
            if (m_liquidity_policy_dirty) {
                setStatusLabel(
                    m_liquidity_target_save_status,
                    tr("Not saved — Core is still using the previous wallet targets. Save these values before attempting to refill or start the provider."),
                    QStringLiteral("action"));
            } else if (m_liquidity_policy_configured) {
                setStatusLabel(
                    m_liquidity_target_save_status,
                    tr("Saved in this provider wallet."),
                    QStringLiteral("ready"));
            } else {
                setStatusLabel(
                    m_liquidity_target_save_status,
                    tr("Not saved yet — review these suggested targets before continuing."),
                    QStringLiteral("action"));
            }
        }
        if (m_save_liquidity_policy && m_save_liquidity_policy_primary) {
            const bool needs_approval =
                m_paid_maintenance_approved->isChecked();
            const bool can_save = hasRpcTransport() && !m_busy &&
                m_liquidity_snapshot_representable &&
                (m_liquidity_policy_dirty ||
                 !m_liquidity_policy_configured);
            const QString primary_text = needs_approval
                ? tr("Save settings and approve bounded refill…")
                : tr("Save liquidity settings");
            const QString target_text = needs_approval
                ? tr("Save targets and approve refill…")
                : tr("Save liquidity targets");
            m_save_liquidity_policy_primary->setText(primary_text);
            m_save_liquidity_policy_primary->setEnabled(can_save);
            m_save_liquidity_policy->setText(target_text);
            m_save_liquidity_policy->setEnabled(can_save);
        }

        const QString target = liquidityTargetKey();
        const bool preparation_current =
            m_prepare_preview_target == target && !m_prepare_plan_id.isEmpty();
        const bool retirement_current =
            m_rebalance_preview_target == target && !m_rebalance_plan_id.isEmpty();
        const bool complete_snapshots = hasCompleteMutationSnapshots();
        if (m_prepare_preview) {
            m_prepare_preview->setEnabled(
                complete_snapshots && m_liquidity_snapshot_representable &&
                !m_busy);
        }
        if (m_rebalance_preview) {
            m_rebalance_preview->setEnabled(
                complete_snapshots && m_liquidity_snapshot_representable &&
                !m_busy);
        }
        if (m_prepare_execute) {
            m_prepare_execute->setEnabled(
                complete_snapshots && m_liquidity_snapshot_representable &&
                preparation_current && !m_busy);
        }
        if (m_rebalance_execute) {
            m_rebalance_execute->setEnabled(
                complete_snapshots && m_liquidity_snapshot_representable &&
                retirement_current && !m_busy);
        }
        if (preparation_current || retirement_current) {
            m_liquidity_preview_status->setText(preparation_current
                ? tr("A preparation preview exists for the current targets.")
                : tr("A retirement preview exists for the current targets."));
        } else {
            m_liquidity_preview_status->setText(tr(
                "No reviewed preview matches the current targets. Preview before executing."));
        }
    }

    static qint64 poolNumber(const UniValue& result, const char* name)
    {
        qint64 value{0};
        return GetInt64Field(result, name, value) ? value : 0;
    }

    QString formatPoolResult(const char* command, const UniValue& result) const
    {
        const bool executed = result.find_value("executed").isBool() &&
                              result.find_value("executed").get_bool();
        QStringList lines;
        if (std::string{command} == "preparepaymasterpool") {
            lines.push_back(executed
                ? tr("Preparation executed: the required pool transaction(s) were created. Wait for confirmation before expecting provider readiness.")
                : tr("Preparation preview only: no wallet funds were moved."));
            lines.push_back(tr(
                "Missing outputs to create — admission DGB: %1; operational DGB: %2; admission carriers: %3; operational carriers: %4.")
                .arg(poolNumber(result, "missing_admission_dgb_slots"))
                .arg(poolNumber(result, "missing_operational_dgb_slots"))
                .arg(poolNumber(result, "missing_admission_carrier_slots"))
                .arg(poolNumber(result, "missing_operational_carrier_slots")));
            lines.push_back(tr(
                "Value per output — admission DGB: %1 DGB; operational DGB: %2 DGB; DD carrier: %3 DD.")
                .arg(QString::number(poolNumber(result, "admission_dgb_satoshis_each") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "operational_dgb_satoshis_each") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "carrier_cents_each") / 100.0, 'f', 2)));
            lines.push_back(tr("New pool total — %1 DGB and %2 DD.")
                .arg(QString::number(poolNumber(result, "total_output_satoshis") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "total_carrier_cents") / 100.0, 'f', 2)));
        } else {
            lines.push_back(executed
                ? tr("Retirement executed: selected excess available liquidity is being returned to the wallet.")
                : tr("Retirement preview only: no wallet funds were moved."));
            lines.push_back(tr(
                "Outputs selected for retirement — admission DGB: %1; operational DGB: %2; admission carriers: %3; operational carriers: %4.")
                .arg(poolNumber(result, "retired_admission_dgb_slots"))
                .arg(poolNumber(result, "retired_operational_dgb_slots"))
                .arg(poolNumber(result, "retired_admission_carrier_slots"))
                .arg(poolNumber(result, "retired_operational_carrier_slots")));
            lines.push_back(tr("Returned before network fee — %1 DGB and %2 DD.")
                .arg(QString::number(poolNumber(result, "retired_dgb_satoshis") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "retired_carrier_cents") / 100.0, 'f', 2)));
        }
        if (result.find_value("network_fee_satoshis").isNum()) {
            lines.push_back(tr("DGB network fee paid: %1 DGB.")
                .arg(QString::number(poolNumber(result, "network_fee_satoshis") / 100000000.0, 'f', 8)));
        }
        const UniValue& dgb_txid = result.find_value("dgb_txid");
        const UniValue& dd_txid = result.find_value("dd_txid");
        if (dgb_txid.isStr()) lines.push_back(tr("DGB transaction: %1").arg(QString::fromStdString(dgb_txid.get_str())));
        if (dd_txid.isStr()) lines.push_back(tr("DD transaction: %1").arg(QString::fromStdString(dd_txid.get_str())));
        if (!executed) {
            lines.push_back(tr(
                "Review these values. If they are acceptable, use the matching Execute reviewed action without changing the targets."));
        }
        return lines.join('\n');
    }

    UniValue poolOptions(bool execute, const QString& plan_id = {}) const
    {
        UniValue options{UniValue::VOBJ};
        options.pushKV("admission_dgb_slots", m_admission_dgb->value());
        options.pushKV("operational_dgb_slots", m_operational_dgb->value());
        options.pushKV("admission_carrier_slots", m_admission_carriers->value());
        options.pushKV("operational_carrier_slots", m_operational_carriers->value());
        options.pushKV("execute", execute);
        if (execute && !plan_id.isEmpty()) {
            options.pushKV("plan_id", plan_id.toStdString());
        }
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        return params;
    }

    void poolAction(const char* command, bool execute)
    {
        if (!requirePrivacyOffForSensitiveAction(
                tr("Reviewing a Paymaster pool change"))) {
            return;
        }
        if (!hasCompleteMutationSnapshots()) {
            m_liquidity_preview_status->setText(tr(
                "The pool action was not started because a complete current provider, safety and liquidity snapshot is unavailable."));
            updateProviderButtons();
            return;
        }
        const bool preparation = std::string{command} == "preparepaymasterpool";
        const QString target = liquidityTargetKey();
        const QString reviewed_target = preparation ? m_prepare_preview_target
                                                    : m_rebalance_preview_target;
        const QString reviewed_plan = preparation ? m_prepare_plan_id
                                                  : m_rebalance_plan_id;
        if (execute &&
            (reviewed_target != target || reviewed_plan.isEmpty())) {
            QMessageBox::warning(
                this, tr("Current pool targets have not been reviewed"),
                tr("Preview this action with the currently displayed target values before executing it."));
            return;
        }
        if (execute && askPlainTextQuestion(
                this, tr("Confirm Paymaster pool change"),
                tr("Execute the exact pool change shown by the most recent preview?\n\n"
                   "This creates or retires wallet outputs and may pay a network fee.")) != QMessageBox::Yes) {
            return;
        }
        call(command, poolOptions(execute, reviewed_plan), execute, nullptr,
             [this, command = std::string{command}, execute, preparation,
              target, reviewed_plan](
                 const UniValue& result) {
                 const bool complete = preparation
                     ? IsCompletePoolPreparationResult(result)
                     : IsCompletePoolRebalanceResult(result);
                 const bool result_executed =
                     result.find_value("executed").isBool() &&
                     result.find_value("executed").get_bool();
                 const QString returned_plan = complete
                     ? QString::fromStdString(
                           result.find_value("plan_id").get_str())
                     : QString{};
                 if (!complete || (!execute && result_executed) ||
                     (execute &&
                      (!result_executed || returned_plan != reviewed_plan))) {
                     if (preparation) {
                         m_prepare_preview_target.clear();
                         m_prepare_plan_id.clear();
                     } else {
                         m_rebalance_preview_target.clear();
                         m_rebalance_plan_id.clear();
                     }
                     m_liquidity_output->setPlainText(tr(
                         "Core did not return a complete response bound to the reviewed pool plan. Refresh the pool status and preview again before executing."));
                     updateLiquidityDisplay();
                     return;
                 }
                 m_liquidity_output->setPlainText(
                     formatPoolResult(command.c_str(), result));
                 const qint64 affected_slots = preparation
                     ? poolNumber(result, "missing_admission_dgb_slots") +
                           poolNumber(result, "missing_operational_dgb_slots") +
                           poolNumber(result, "missing_admission_carrier_slots") +
                           poolNumber(result, "missing_operational_carrier_slots")
                     : poolNumber(result, "retired_admission_dgb_slots") +
                           poolNumber(result, "retired_operational_dgb_slots") +
                           poolNumber(result, "retired_admission_carrier_slots") +
                           poolNumber(result, "retired_operational_carrier_slots");
                 if (preparation) {
                     m_prepare_preview_target =
                         !execute && affected_slots > 0 ? target : QString{};
                     m_prepare_plan_id =
                         !execute && affected_slots > 0
                         ? returned_plan : QString{};
                 } else {
                     m_rebalance_preview_target =
                         !execute && affected_slots > 0 ? target : QString{};
                     m_rebalance_plan_id =
                         !execute && affected_slots > 0
                         ? returned_plan : QString{};
                 }
                 updateLiquidityDisplay();
                 if (execute) refreshStatus();
             });
    }

    static QString activityText(const UniValue& object, const char* name)
    {
        const UniValue& value = object.find_value(name);
        return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
    }

    QString reservationPurpose(const UniValue& entry) const
    {
        const QString purpose = activityText(entry, "purpose");
        if (purpose == QLatin1String("admission")) return tr("admission reserve");
        if (purpose == QLatin1String("operational")) return tr("operational payment slot");
        return purpose.isEmpty() ? tr("unknown purpose") : purpose;
    }

    QString reservationAsset(const UniValue& entry) const
    {
        const QString asset = activityText(entry, "asset");
        if (asset == QLatin1String("dgb")) {
            return tr("%1 DGB").arg(QString::number(
                poolNumber(entry, "dgb_satoshis") / 100000000.0, 'f', 8));
        }
        if (asset == QLatin1String("dd_carrier")) {
            return tr("%1 DD carrier").arg(QString::number(
                poolNumber(entry, "dd_cents") / 100.0, 'f', 2));
        }
        return asset.isEmpty() ? tr("unknown asset") : asset;
    }

    QString reservationState(const UniValue& entry) const
    {
        const QString state = activityText(entry, "state");
        if (state == QLatin1String("reserved")) return tr("reserved for in-progress work");
        if (state == QLatin1String("pending_successor")) return tr("waiting for a replacement output");
        if (state == QLatin1String("committed")) return tr("committed to an exact payment");
        if (state == QLatin1String("spent")) return tr("spent and retained as durable history");
        return state.isEmpty() ? tr("unknown") : state;
    }

    void showReservations(const UniValue& result)
    {
        m_activity_output->setPlainText(QString::fromStdString(result.write(2)));
        if (!result.isArray()) {
            if (m_reservations_group) m_reservations_group->setVisible(true);
            if (m_recovery_group) m_recovery_group->setVisible(true);
            m_activity_summary->setText(tr(
                "The wallet returned an unexpected reservation result. See the technical result."));
            return;
        }
        const auto& entries = result.getValues();
        if (entries.empty()) {
            if (m_reservations_group) m_reservations_group->setVisible(false);
            if (m_recovery_group) m_recovery_group->setVisible(false);
            m_activity_summary->setText(tr(
                "Healthy · no reservations or recovery actions require attention."));
            return;
        }
        if (m_reservations_group) m_reservations_group->setVisible(true);
        if (m_recovery_group) m_recovery_group->setVisible(true);
        QStringList lines;
        lines.push_back(tr(
            "%1 unavailable or durable pool record(s). Core controls when each output can safely return to the available pool.")
                            .arg(static_cast<qulonglong>(entries.size())));
        const size_t visible_entries = std::min<size_t>(entries.size(), 8);
        for (size_t i = 0; i < visible_entries; ++i) {
            const UniValue& entry = entries[i];
            lines.push_back(tr("• %1 — %2 — state: %3")
                                .arg(reservationPurpose(entry),
                                     reservationAsset(entry),
                                     reservationState(entry)));
        }
        if (entries.size() > visible_entries) {
            lines.push_back(tr(
                "%1 more reservation(s) are listed in the technical result.")
                                .arg(static_cast<qulonglong>(entries.size() - visible_entries)));
        }
        m_activity_summary->setText(lines.join('\n'));
    }

    QString formatActivityResult(const UniValue& result, bool submitted_payment) const
    {
        if (!result.isObject()) {
            return tr("Core returned an unexpected processing result. See the technical result.");
        }
        const UniValue& processed_value = result.find_value("processed");
        const bool processed = processed_value.isBool() && processed_value.get_bool();
        if (!processed) {
            return submitted_payment
                ? tr("No submitted payment is waiting in this provider wallet.")
                : tr("No capacity, quote, or recovery request is waiting in this provider wallet.");
        }

        const QString type = activityText(result, "message_type");
        QString type_description;
        if (type == QLatin1String("capacity")) {
            type_description = tr("capacity check");
        } else if (type == QLatin1String("quote")) {
            type_description = tr("quote request");
        } else if (type == QLatin1String("recovery_request")) {
            type_description = tr("recovery-provider request");
        } else if (type == QLatin1String("recovery_submit")) {
            type_description = tr("recovery submission");
        } else if (type == QLatin1String("submit")) {
            type_description = tr("submitted payment");
        } else {
            type_description = type.isEmpty() ? tr("provider message") : type;
        }

        const UniValue& queued_value = result.find_value("queued");
        const bool queued = queued_value.isBool() && queued_value.get_bool();
        QStringList lines;
        lines.push_back(tr("Processed one %1.").arg(type_description));
        lines.push_back(queued
            ? tr("The signed provider response was queued for delivery to the client.")
            : tr("The provider response was not queued. Check the technical result before retrying."));
        const QString result_status = activityText(result, "result_status");
        const QString attempt_state = activityText(result, "attempt_state");
        const QString rejection_code = activityText(result, "rejection_code");
        const QString txid = activityText(result, "txid");
        if (!result_status.isEmpty()) {
            lines.push_back(tr("Result status: %1").arg(result_status));
        }
        if (!attempt_state.isEmpty()) {
            lines.push_back(tr("Payment state: %1").arg(attempt_state));
        }
        if (!rejection_code.isEmpty()) {
            lines.push_back(tr("Rejected safely: %1").arg(rejection_code));
        }
        if (!txid.isEmpty()) {
            lines.push_back(tr("Transaction: %1").arg(txid));
        }
        if (!submitted_payment) {
            lines.push_back(tr(
                "If more clients are waiting, process another request. A quote may now hold a reservation."));
        }
        return lines.join('\n');
    }

    void refreshActivity()
    {
        call("listpaymasterreservations", {}, false, m_activity_output,
             [this](const UniValue& result) {
                 showReservations(result);
                 refreshStatus();
             });
    }

    void processActivityRequest()
    {
        if (!requirePrivacyOffForSensitiveAction(
                tr("Processing Paymaster requests"))) {
            return;
        }
        if (!hasCompleteMutationSnapshots() || !m_core_running ||
            m_core_locked) {
            m_activity_action_result->setText(tr(
                "No request was processed because the current provider snapshots or unlocked runtime state are incomplete."));
            updateProviderButtons();
            return;
        }
        call("processpaymasterrequests", {}, true, m_activity_output,
             [this](const UniValue& result) {
                 m_activity_action_result->setText(
                     formatActivityResult(result, /*submitted_payment=*/false));
                 refreshStatus();
             });
    }

    void processActivitySubmit()
    {
        if (!requirePrivacyOffForSensitiveAction(
                tr("Processing Paymaster submissions"))) {
            return;
        }
        if (!hasCompleteMutationSnapshots() || !m_core_running ||
            m_core_locked) {
            m_activity_action_result->setText(tr(
                "No submission was processed because the current provider snapshots or unlocked runtime state are incomplete."));
            updateProviderButtons();
            return;
        }
        if (askPlainTextQuestion(
                this, tr("Process one submitted Paymaster payment?"),
                tr("Core will validate one waiting client-signed submission. If it exactly "
                   "matches the durable authorization, this action may sign provider inputs, "
                   "spend reserved DGB within your safety policy, and broadcast the transaction.\n\n"
                   "Continue?")) != QMessageBox::Yes) {
            return;
        }
        call("processpaymastersubmits", {}, true, m_activity_output,
             [this](const UniValue& result) {
                 m_activity_action_result->setText(
                     formatActivityResult(result, /*submitted_payment=*/true));
                 refreshStatus();
             });
    }

    void savePolicy()
    {
        if (!m_policy_snapshot_representable) {
            m_status->setText(tr(
                "Provider policy was not changed: the persisted Core value cannot be represented exactly by this interface."));
            return;
        }
        if (m_core_running) {
            const QString message = tr(
                "Stop the Paymaster provider before saving an operating-policy change. This prevents new work from being accepted under the previous announcement while the wallet commits the replacement policy.");
            if (m_rpc_executor_for_testing) {
                m_status->setText(message);
            } else {
                QMessageBox::warning(
                    this, tr("Stop provider before changing policy"),
                    message);
            }
            return;
        }
        UniValue funding{UniValue::VARR};
        if (m_sponsored->isChecked()) funding.push_back("sponsored");
        if (m_user_paid->isChecked()) funding.push_back("user_paid");
        if (funding.empty()) {
            QMessageBox::warning(this, tr("Paymaster policy"), tr("Select at least one funding model."));
            return;
        }
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("funding_models", std::move(funding));
        policy.pushKV("sponsorship_scope", m_scope->currentData().toString().toStdString());
        policy.pushKV("fee_rate_bps", m_fee_bps->value());
        policy.pushKV("min_amount_cents", m_min_amount->value());
        policy.pushKV("max_amount_cents", m_max_amount->value());
        policy.pushKV("quote_ttl", m_quote_ttl->value());
        policy.pushKV("maximum_network_fee_dgb_satoshis", m_network_fee->value());
        const UniValue requested_policy = policy;
        UniValue params{UniValue::VARR};
        params.push_back(std::move(policy));
        call("setpaymasterpolicy", std::move(params), false, nullptr,
             [this, requested_policy](const UniValue& result) {
                 if (!IsExactProviderPolicyAcknowledgement(
                         requested_policy, result)) {
                     m_status->setText(tr(
                         "Core did not confirm the exact provider operating policy. The displayed edit remains unsaved; refresh before retrying."));
                     return;
                 }
                 m_policy_loaded = true;
                 m_provider_settings_present = true;
                 m_policy_dirty = false;
                 updatePolicyDisplay();
                 refreshStatus();
             });
    }

    bool showSetupWizard()
    {
        const auto report_unavailable = [this](const QString& title,
                                                const QString& message) {
            // Native modal message boxes are unreliable on the minimal and
            // offscreen Qt platforms used by widget tests. Production keeps
            // the explicit dialog; tests can assert the same message through
            // the persistent provider status label.
            if (m_rpc_executor_for_testing) {
                m_status->setText(message);
                return;
            }
            QMessageBox::information(this, title, message);
        };
        if (!requirePrivacyOffForSensitiveAction(
                tr("Guided Paymaster setup"))) {
            return false;
        }
        if (m_busy) {
            report_unavailable(
                tr("Paymaster setup"),
                tr("Provider settings are still being loaded or another Paymaster operation is in progress. Wait for it to finish, then reopen the assistant so it can use the authoritative saved values."));
            return false;
        }
        if (m_core_running) {
            report_unavailable(
                tr("Paymaster setup"),
                tr("Stop the Paymaster provider before changing its saved configuration. This keeps active requests bound to one operating and safety policy while the assistant applies the new settings."));
            return false;
        }
        if (m_model && (!m_provider_info_snapshot_available ||
                        !m_provider_safety_snapshot_available ||
                        !m_liquidity_snapshot_available)) {
            report_unavailable(
                tr("Paymaster setup data incomplete"),
                tr("The assistant needs complete, exactly representable safety and liquidity snapshots before it can preserve saved values safely. Refresh the Paymaster status and reopen the assistant after those checks succeed."));
            return false;
        }
        if (m_policy_dirty || m_provider_safety_dirty ||
            m_liquidity_policy_dirty || m_runtime_settings_dirty) {
            report_unavailable(
                tr("Unsaved Paymaster changes"),
                tr("Save or discard the unsaved Expert-mode changes before opening guided setup. The assistant only imports authoritative values already persisted in the wallet."));
            return false;
        }
        const QPointer<WalletModel> setup_model{m_model};
        const QString setup_wallet_name = setup_model
            ? setup_model->getDisplayName()
            : tr("No wallet selected");
        const QString setup_wallet_id = setup_model
            ? setup_model->getWalletName()
            : QString{};

        PaymasterSetupWizard wizard(this);
        m_setup_wizard = &wizard;
        auto replace_unrepresentable_policy = std::make_shared<bool>(
            m_policy_snapshot_representable);
        wizard.setObjectName("PaymasterSetupWizard");
        wizard.setWindowTitle(tr("Guided Paymaster provider setup"));
        wizard.setWizardStyle(QWizard::ClassicStyle);
        wizard.setOption(QWizard::NoBackButtonOnStartPage);
        wizard.setOption(QWizard::HaveHelpButton, false);
        wizard.setSizeGripEnabled(true);
        wizard.setMinimumSize(560, 440);
        wizard.resize(760, 600);
        wizard.setStyleSheet(PaymasterWizardStyleSheet(
            UseDarkPaymasterWizardTheme(m_model, this)));

        auto* intro = new CompletableWizardPage(&wizard);
        intro->setObjectName("paymasterSetupRequirementsPage");
        intro->setTitle(tr("Choose the provider wallet"));
        intro->setSubTitle(tr("Confirm which wallet will own the provider identity, liquidity and safety settings."));
        auto* intro_page_layout = new QVBoxLayout(intro);
        auto* intro_scroll = new QScrollArea(intro);
        intro_scroll->setObjectName("paymasterSetupRequirementsScroll");
        intro_scroll->setWidgetResizable(true);
        intro_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* intro_content = new QWidget(intro_scroll);
        intro_content->setObjectName("paymasterSetupRequirementsContent");
        auto* intro_layout = new QVBoxLayout(intro_content);
        // Word-wrapped labels otherwise report an overly small vertical size
        // hint while the hidden wizard page is being laid out. Preserve the
        // complete content height and let the page scroll on compact displays.
        intro_layout->setSizeConstraint(QLayout::SetMinimumSize);
        intro_scroll->setWidget(intro_content);
        // setWidget() enables the content widget's native background fill.
        // Disable it so Windows cannot paint a light system surface through
        // the wizard's dark theme.
        ConfigureScrollArea(intro_scroll, intro_content);
        intro_page_layout->addWidget(intro_scroll);
        auto* intro_text = new QLabel(tr(
            "A Paymaster supplies DGB network fees for DigiDollar transfers. This assistant "
            "prepares a complete, conservative starting configuration and explains every step. "
            "Before anything is saved, the assistant shows the complete plan and a current liquidity estimate. "
            "After your confirmation it saves the identity, policy and safety limits and creates only missing pool outputs. "
            "The selected runtime, enabled and autostart settings are shown in the final review; autostart is applied only after liquidity has been rechecked."), intro);
        intro_text->setObjectName("paymasterSetupIntroduction");
        intro_text->setWordWrap(true);
        intro_layout->addWidget(intro_text);

        auto* wallet_frame = new QFrame(intro);
        wallet_frame->setObjectName("paymasterSetupWalletCard");
        wallet_frame->setFrameShape(QFrame::StyledPanel);
        auto* wallet_layout = new QVBoxLayout(wallet_frame);
        auto* wallet_name = new QLabel(
            tr("Provider wallet: %1").arg(setup_wallet_name), wallet_frame);
        wallet_name->setObjectName("paymasterSetupWalletName");
        wallet_name->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                             Qt::TextSelectableByMouse);
        wallet_name->setAccessibleName(tr("Selected Paymaster provider wallet"));
        wallet_name->setAccessibleDescription(tr(
            "The wallet whose identity, prepared liquidity, budgets and provider history will be used by this setup."));
        QFont wallet_name_font = wallet_name->font();
        wallet_name_font.setBold(true);
        wallet_name->setFont(wallet_name_font);
        wallet_layout->addWidget(wallet_name);

        auto* wallet_status = new QLabel(wallet_frame);
        wallet_status->setObjectName("paymasterSetupWalletStatus");
        wallet_status->setWordWrap(true);
        if (!setup_model) {
            wallet_status->setText(tr(
                "No wallet is selected. Close this assistant, create or open a suitable wallet and select it before continuing."));
        } else if (m_core_has_identity) {
            wallet_status->setText(tr(
                "This wallet already has a Paymaster provider identity. The assistant will retain it unchanged and update only this wallet's configuration."));
        } else if (m_core_eligible) {
            wallet_status->setText(tr(
                "Core currently reports this wallet as eligible. Eligibility is checked again immediately before any setting is written."));
        } else {
            wallet_status->setText(tr(
                "Eligibility has not yet been confirmed. Before changing anything, Core will require a descriptor wallet with local private keys and no external signer."));
        }
        wallet_layout->addWidget(wallet_status);

        auto* wallet_recommendation = new QLabel(tr(
            "Recommended: use a dedicated provider wallet. This keeps provider identity, prepared DGB/DD outputs, safety budgets and transaction history separate from everyday personal funds. A dedicated wallet is recommended for operations and accounting, but is not a protocol requirement."),
            wallet_frame);
        wallet_recommendation->setObjectName("paymasterSetupWalletRecommendation");
        wallet_recommendation->setWordWrap(true);
        wallet_layout->addWidget(wallet_recommendation);

        auto* wallet_consequence = new QLabel(tr(
            "Continuing does not create another wallet. The assistant will create or reuse the provider identity and prepare the selected liquidity inside the wallet named above. Paymaster carrier DD remains wallet-owned but is shown as reserved until it is safely retired."),
            wallet_frame);
        wallet_consequence->setObjectName("paymasterSetupWalletConsequence");
        wallet_consequence->setWordWrap(true);
        wallet_layout->addWidget(wallet_consequence);

        auto* wallet_confirmation = new QCheckBox(
            tr("Use \"%1\" as the provider wallet").arg(setup_wallet_name),
            wallet_frame);
        wallet_confirmation->setObjectName("paymasterSetupWalletConfirmation");
        wallet_confirmation->setAccessibleName(tr("Confirm Paymaster provider wallet"));
        wallet_confirmation->setAccessibleDescription(tr(
            "Required confirmation that the assistant may configure only the named wallet."));
        wallet_confirmation->setEnabled(setup_model != nullptr);
        wallet_layout->addWidget(wallet_confirmation);

        auto* choose_another_wallet = new QPushButton(
            tr("Close and choose or create another wallet…"), wallet_frame);
        choose_another_wallet->setObjectName("paymasterSetupChooseAnotherWallet");
        choose_another_wallet->setToolTip(tr(
            "Use DigiByte Core's normal wallet menu and wallet selector, then reopen Paymaster Network in that wallet."));
        wallet_layout->addWidget(choose_another_wallet, 0, Qt::AlignLeft);
        intro_layout->addWidget(wallet_frame);

        auto* wallet_scope_note = new QLabel(tr(
            "This assistant cannot switch wallets and will not create a wallet or move balances. To use another wallet, close the assistant, use the normal wallet menu to create or open it, select that wallet, and reopen Paymaster Network."), intro);
        wallet_scope_note->setObjectName("paymasterSetupWalletScopeNote");
        wallet_scope_note->setWordWrap(true);
        intro_layout->addWidget(wallet_scope_note);

        connect(wallet_confirmation, &QCheckBox::toggled, intro,
                [intro, setup_model](bool checked) {
                    intro->setComplete(checked && setup_model);
                });
        connect(choose_another_wallet, &QPushButton::clicked, &wizard,
                [&wizard] { wizard.reject(); });
        intro->setComplete(false);

        auto* current_status = new QLabel(
            m_readiness_summary->text().isEmpty()
                ? tr("Current check: refresh the provider status after opening the detailed pages.")
                : tr("Current check:\n%1").arg(m_readiness_summary->text()), intro);
        current_status->setObjectName("paymasterSetupCurrentReadiness");
        current_status->setWordWrap(true);
        current_status->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                                Qt::TextSelectableByMouse);
        intro_layout->addWidget(current_status);
        auto* requirements = new QLabel(tr(
            "Required before the provider can run:\n\n"
            "• a descriptor wallet with local private keys\n"
            "• DigiDollar active, a synchronized node, txindex=1 and pruning disabled\n"
            "• BIP324 version 2 transport enabled\n"
            "• a reachable -paymasterendpoint configured before node startup\n"
            "• network message capture disabled for mainnet operation\n"
            "• confirmed DGB liquidity and, for user-paid service, confirmed DD carriers\n"
            "• a saved finite provider safety policy"), intro);
        requirements->setObjectName("paymasterSetupRequirements");
        requirements->setWordWrap(true);
        intro_layout->addWidget(requirements);
        intro_layout->addStretch();
        wizard.addPage(intro);

        auto* identity_page = new QWizardPage(&wizard);
        identity_page->setObjectName("paymasterSetupIdentityPage");
        identity_page->setTitle(tr("Identity"));
        identity_page->setSubTitle(tr("Choose the public name clients will see for this provider."));
        auto* identity_layout = new QFormLayout(identity_page);
        auto* display = new QLineEdit(m_display_name->text(), identity_page);
        display->setObjectName("paymasterSetupDisplayName");
        display->setMaxLength(32);
        display->setValidator(new PaymasterDisplayNameValidator(display));
        display->setPlaceholderText(tr("Optional provider name"));
        display->setAccessibleName(tr("Public provider display name"));
        display->setAccessibleDescription(tr(
            "An optional informational name shown to clients. The signed provider identity remains authoritative."));
        display->setEnabled(!m_core_has_identity);
        identity_layout->addRow(tr("Public display name:"), display);
        auto* identity_note = new QLabel(tr(
            "The name is only a human-readable label. The assistant creates the persistent wallet-managed provider identity when the plan is applied. "
            "If this wallet already has an identity, it is retained unchanged. Clients authenticate its signed BIP86 key, not this display name."),
            identity_page);
        identity_note->setObjectName("paymasterSetupIdentityExplanation");
        identity_note->setWordWrap(true);
        identity_layout->addRow(identity_note);
        auto* restore_identity_defaults = new QPushButton(tr("Restore defaults"), identity_page);
        restore_identity_defaults->setObjectName("paymasterSetupRestoreIdentityDefaults");
        restore_identity_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        restore_identity_defaults->setEnabled(!m_core_has_identity);
        restore_identity_defaults->setToolTip(m_core_has_identity
            ? tr("The existing provider identity and its saved display name are retained unchanged.")
            : tr("Clear the optional display name."));
        identity_layout->addRow(restore_identity_defaults);
        connect(restore_identity_defaults, &QPushButton::clicked, identity_page,
                [display] { display->clear(); });
        wizard.addPage(identity_page);

        auto* model_page = new QWizardPage(&wizard);
        model_page->setObjectName("paymasterSetupFundingPage");
        model_page->setTitle(tr("Choose the service model"));
        model_page->setSubTitle(tr("Decide who pays for the service. Both public models may be offered together."));
        auto* model_layout = new QVBoxLayout(model_page);
        auto* wizard_user_paid = new QCheckBox(tr("User paid — recommended for general service"), model_page);
        wizard_user_paid->setObjectName("paymasterSetupUserPaid");
        wizard_user_paid->setChecked(m_user_paid->isChecked());
        auto* user_paid_note = new QLabel(tr(
            "The client pays your configured service fee in DigiDollar. Your wallet still supplies the DGB network fee."), model_page);
        user_paid_note->setWordWrap(true);
        auto* wizard_sponsored = new QCheckBox(tr("Sponsored — no DigiDollar service fee"), model_page);
        wizard_sponsored->setObjectName("paymasterSetupSponsored");
        wizard_sponsored->setChecked(m_sponsored->isChecked());
        auto* sponsored_note = new QLabel(tr(
            "Your wallet pays the DGB fee without compensation. Public sponsorship can be consumed by unknown clients, so finite budgets are essential."), model_page);
        sponsored_note->setWordWrap(true);
        auto* wizard_scope = new NoWheelComboBox(model_page);
        wizard_scope->setObjectName("paymasterSetupSponsorshipScope");
        wizard_scope->addItem(tr("Public — discovered clients may request sponsorship"), QStringLiteral("public"));
        wizard_scope->addItem(tr("Restricted — invitation required"), QStringLiteral("restricted"));
        const int selected_scope = wizard_scope->findData(m_scope->currentData());
        if (selected_scope >= 0) wizard_scope->setCurrentIndex(selected_scope);
        wizard_scope->setEnabled(wizard_sponsored->isChecked());
        auto* scope_note = new QLabel(tr(
            "Core validates whether the selected sponsorship scope and funding-model combination is supported. The assistant preserves your exact selection and reports any rejection without rewriting it."), model_page);
        scope_note->setWordWrap(true);
        model_layout->addWidget(wizard_user_paid);
        model_layout->addWidget(user_paid_note);
        model_layout->addSpacing(8);
        model_layout->addWidget(wizard_sponsored);
        model_layout->addWidget(sponsored_note);
        model_layout->addWidget(wizard_scope);
        model_layout->addWidget(scope_note);
        auto* restore_model_defaults = new QPushButton(tr("Restore defaults"), model_page);
        restore_model_defaults->setObjectName("paymasterSetupRestoreFundingDefaults");
        restore_model_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        model_layout->addWidget(restore_model_defaults, 0, Qt::AlignLeft);
        model_layout->addStretch();
        wizard.addPage(model_page);

        connect(wizard_sponsored, &QCheckBox::toggled, wizard_scope, &QWidget::setEnabled);
        connect(restore_model_defaults, &QPushButton::clicked, model_page,
                [wizard_user_paid, wizard_sponsored, wizard_scope] {
                    wizard_scope->setCurrentIndex(
                        wizard_scope->findData(QStringLiteral("public")));
                    wizard_sponsored->setChecked(false);
                    wizard_user_paid->setChecked(true);
                });

        auto* policy_page = new QWizardPage(&wizard);
        policy_page->setObjectName("paymasterSetupPolicyPage");
        policy_page->setTitle(tr("Offer limits"));
        policy_page->setSubTitle(tr("Set the payment range, quote lifetime and absolute fee ceiling."));
        auto* policy_layout = new QFormLayout(policy_page);
        auto* fee = new NoWheelDoubleSpinBox(policy_page);
        fee->setObjectName("paymasterSetupServiceFeePercent");
        fee->setRange(0.0, 100.0);
        fee->setDecimals(2);
        fee->setSingleStep(0.10);
        fee->setValue(m_fee_bps->value() / 100.0);
        fee->setSuffix(tr(" %"));
        fee->setAccessibleName(tr("User-paid service fee in percent"));
        fee->setToolTip(tr(
            "Percentage charged on a user-paid DigiDollar transfer. Changes in 0.10% steps; Core stores the exact equivalent in basis points."));
        const auto update_wizard_fee_model =
            [fee](bool user_paid) {
                fee->setEnabled(user_paid);
            };
        connect(wizard_user_paid, &QCheckBox::toggled, policy_page,
                update_wizard_fee_model);
        update_wizard_fee_model(wizard_user_paid->isChecked());
        auto* minimum = spin(policy_page, 100, 10000000,
                             std::max(100, m_min_amount->value()));
        minimum->setObjectName("paymasterSetupMinimumPayment");
        minimum->setSuffix(tr(" cents"));
        minimum->setAccessibleName(tr("Smallest supported payment"));
        auto* maximum = spin(policy_page, 100, 10000000,
                             std::max(100, m_max_amount->value()));
        maximum->setObjectName("paymasterSetupMaximumPayment");
        maximum->setSuffix(tr(" cents"));
        maximum->setAccessibleName(tr("Largest supported payment"));
        auto* lifetime = spin(policy_page, 1, 60, m_quote_ttl->value());
        lifetime->setObjectName("paymasterSetupQuoteLifetime");
        lifetime->setSuffix(tr(" seconds"));
        lifetime->setAccessibleName(tr("Quote validity in seconds"));
        auto* network_fee = spin(
            policy_page, 1, 2000000000, m_network_fee->value());
        network_fee->setObjectName("paymasterSetupNetworkFee");
        network_fee->setSuffix(tr(" sat"));
        network_fee->setAccessibleName(tr("Maximum DGB network fee per transfer"));
        qint64 retained_network_fee = m_network_fee->value();
        if (!m_policy_snapshot_representable) {
            GetInt64Field(m_unrepresentable_policy_snapshot,
                          "maximum_network_fee_dgb_satoshis",
                          retained_network_fee);
        }
        const auto selected_network_fee =
            [network_fee, retained_network_fee,
             replace_unrepresentable_policy] {
                return *replace_unrepresentable_policy
                    ? static_cast<qint64>(network_fee->value())
                    : retained_network_fee;
            };
        policy_layout->addRow(tr("User-paid service fee:"), fee);
        policy_layout->addRow(tr("Smallest payment:"), minimum);
        policy_layout->addRow(tr("Largest payment:"), maximum);
        policy_layout->addRow(tr("Quote validity:"), lifetime);
        policy_layout->addRow(tr("Maximum DGB network fee per transfer:"), network_fee);
        auto* imported_policy = new QLineEdit(policy_page);
        imported_policy->setObjectName("paymasterSetupImportedRawPolicy");
        imported_policy->setReadOnly(true);
        imported_policy->setVisible(!m_policy_snapshot_representable);
        imported_policy->setText(!m_policy_snapshot_representable
            ? QString::fromStdString(
                  m_unrepresentable_policy_snapshot.write())
            : QString{});
        imported_policy->setAccessibleName(
            tr("Exact persisted provider policy retained unchanged"));
        if (!m_policy_snapshot_representable) {
            policy_layout->addRow(
                tr("Persisted policy (read-only):"), imported_policy);
        }
        auto* restore_policy_defaults = new QPushButton(tr("Restore defaults"), policy_page);
        restore_policy_defaults->setObjectName("paymasterSetupRestorePolicyDefaults");
        restore_policy_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        policy_layout->addRow(restore_policy_defaults);
        auto* field_help = new QFrame(policy_page);
        field_help->setObjectName("paymasterSetupFieldHelp");
        field_help->setFrameShape(QFrame::StyledPanel);
        auto* field_help_layout = new QVBoxLayout(field_help);
        field_help_layout->setContentsMargins(12, 9, 12, 9);
        field_help_layout->setSpacing(3);
        auto* field_help_title = new QLabel(field_help);
        field_help_title->setObjectName("paymasterSetupFieldHelpTitle");
        auto* field_help_text = new QLabel(field_help);
        field_help_text->setObjectName("paymasterSetupFieldHelpText");
        field_help_text->setWordWrap(true);
        field_help_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field_help_layout->addWidget(field_help_title);
        field_help_layout->addWidget(field_help_text);
        policy_layout->addRow(field_help);

        const auto update_policy_help = [this, fee, minimum, maximum, lifetime, network_fee,
                                         field_help_title, field_help_text](QWidget* field) {
            if (field == fee) {
                field_help_title->setText(tr("User-paid service fee"));
                field_help_text->setText(tr(
                    "For user-paid offers, the provider charges %1% of the transferred $DD amount. "
                    "Core stores this exactly as %2 basis points. Sponsored transfers always charge 0.00% service fee.")
                    .arg(QString::number(fee->value(), 'f', 2))
                    .arg(qRound(fee->value() * 100.0)));
            } else if (field == minimum) {
                field_help_title->setText(tr("Smallest supported payment"));
                field_help_text->setText(tr(
                    "Your provider will not offer service for payments below %1 $DD. "
                    "A sensible minimum avoids spending provider resources on very small requests.")
                    .arg(QString::number(minimum->value() / 100.0, 'f', 2)));
            } else if (field == maximum) {
                field_help_title->setText(tr("Largest supported payment"));
                field_help_text->setText(tr(
                    "Your provider will reject payments above %1 $DD. Keep this limit aligned with the amount of liquidity and financial exposure you intend to provide.")
                    .arg(QString::number(maximum->value() / 100.0, 'f', 2)));
            } else if (field == lifetime) {
                field_help_title->setText(tr("Quote validity"));
                field_help_text->setText(tr(
                    "A client has %1 seconds to accept this provider quote. Shorter validity releases unused reservations sooner; longer validity gives slower clients more time.")
                    .arg(lifetime->value()));
            } else if (field == network_fee) {
                field_help_title->setText(tr("Maximum DGB network fee per transfer"));
                field_help_text->setText(tr(
                    "The provider will never fund more than %1 DGB of network fee for one transfer. "
                    "The wallet safety profile does not lower this individual-transfer ceiling; it limits repeated requests through finite hourly, daily and completed-transfer budgets.")
                    .arg(QString::number(network_fee->value() / 100000000.0, 'f', 8)));
            }
        };
        connect(qApp, &QApplication::focusChanged, policy_page,
                [policy_page, fee, minimum, maximum, lifetime, network_fee,
                 update_policy_help](QWidget*, QWidget* focused) {
                    QWidget* candidate = focused;
                    while (candidate && candidate != policy_page) {
                        if (candidate == fee || candidate == minimum || candidate == maximum ||
                            candidate == lifetime || candidate == network_fee) {
                            update_policy_help(candidate);
                            return;
                        }
                        candidate = candidate->parentWidget();
                    }
                });
        connect(fee, qOverload<double>(&QDoubleSpinBox::valueChanged), policy_page,
                [fee, update_policy_help] { update_policy_help(fee); });
        connect(restore_policy_defaults, &QPushButton::clicked, policy_page,
                [fee, minimum, maximum, lifetime, network_fee,
                 wizard_user_paid, wizard_sponsored, wizard_scope,
                 restore_model_defaults, imported_policy,
                 replace_unrepresentable_policy] {
                    *replace_unrepresentable_policy = true;
                    imported_policy->hide();
                    wizard_user_paid->setEnabled(true);
                    wizard_sponsored->setEnabled(true);
                    wizard_scope->setEnabled(
                        wizard_sponsored->isChecked());
                    restore_model_defaults->setEnabled(true);
                    fee->setEnabled(wizard_user_paid->isChecked());
                    minimum->setEnabled(true);
                    maximum->setEnabled(true);
                    lifetime->setEnabled(true);
                    network_fee->setEnabled(true);
                    fee->setValue(wizard_user_paid->isChecked() ? 0.50 : 0.0);
                    minimum->setValue(100);
                    maximum->setValue(100000);
                    lifetime->setValue(60);
                    network_fee->setValue(20000000);
                });
        if (!m_policy_snapshot_representable) {
            wizard_user_paid->setEnabled(false);
            wizard_sponsored->setEnabled(false);
            wizard_scope->setEnabled(false);
            restore_model_defaults->setEnabled(false);
            fee->setEnabled(false);
            minimum->setEnabled(false);
            maximum->setEnabled(false);
            lifetime->setEnabled(false);
            network_fee->setEnabled(false);
            field_help_text->setText(tr(
                "The exact persisted Core policy is retained read-only because at least one value is outside this interface's range. You may edit other setup pages without replacing it. Choose Restore defaults explicitly only if you intend the assistant to replace the complete operating policy."));
        }
        for (QSpinBox* control : {minimum, maximum, lifetime, network_fee}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), policy_page,
                    [control, update_policy_help] { update_policy_help(control); });
        }
        update_policy_help(fee);
        wizard.addPage(policy_page);

        auto* safety_page = new QWizardPage(&wizard);
        safety_page->setObjectName("paymasterSetupSafetyPage");
        safety_page->setTitle(tr("Safety profile"));
        safety_page->setSubTitle(tr("Choose finite wallet-local spending and request limits."));
        auto* safety_page_layout = new QVBoxLayout(safety_page);
        auto* safety_scroll = new QScrollArea(safety_page);
        safety_scroll->setObjectName("paymasterSetupSafetyScroll");
        safety_scroll->setWidgetResizable(true);
        safety_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* safety_content = new QWidget(safety_scroll);
        safety_content->setObjectName("paymasterSetupSafetyContent");
        auto* safety_layout = new QVBoxLayout(safety_content);
        safety_layout->setSizeConstraint(QLayout::SetMinimumSize);
        safety_scroll->setWidget(safety_content);
        ConfigureScrollArea(safety_scroll, safety_content);
        safety_page_layout->addWidget(safety_scroll);
        auto* safety_text = new QLabel(tr(
            "Safety limits are the final brake against repeated or expensive remote requests. "
            "They are never advertised and never mean unlimited. Selected service models receive "
            "finite limits; models not selected by the operating policy remain unavailable even "
            "when their previously saved limits are retained. When you "
            "explicitly choose or edit a profile, its transfer and aggregate limits are shared by all "
            "selected funding models; otherwise distinct saved model limits are retained."), safety_page);
        safety_text->setObjectName("paymasterSetupSafetyExplanation");
        safety_text->setWordWrap(true);
        auto* safety_profile = new NoWheelComboBox(safety_page);
        safety_profile->setObjectName("paymasterSetupSafetyProfile");
        safety_profile->addItem(tr("Conservative — lower initial hourly and daily exposure"), QStringLiteral("conservative"));
        safety_profile->addItem(tr("Recommended — standard finite starting limits"), QStringLiteral("recommended"));
        safety_profile->addItem(tr("Custom — configure every aggregate and request limit"), QStringLiteral("custom"));
        FundingSafetyValues existing_safety;
        const FundingSafetyControls* existing_safety_controls = &m_user_paid_safety;
        if (!m_user_paid->isChecked() && m_sponsored->isChecked()) {
            existing_safety_controls =
                m_scope->currentData().toString() == QLatin1String("restricted")
                ? &m_restricted_sponsored_safety
                : &m_public_sponsored_safety;
        }
        const bool have_existing_safety =
            m_provider_safety_configured &&
            readFundingSafety(*existing_safety_controls, existing_safety) &&
            !existing_safety.allZero();
        const GuidedPaymasterSafetyLimits conservative_safety = GuidedSafetyLimits(
            /*conservative=*/true, selected_network_fee());
        auto* custom_safety = new QWidget(safety_page);
        custom_safety->setObjectName("paymasterSetupCustomSafety");
        auto* custom_safety_form = new QFormLayout(custom_safety);
        const GuidedPaymasterSafetyLimits recommended_safety = GuidedSafetyLimits(
            /*conservative=*/false, selected_network_fee());
        const GuidedPaymasterSafetyLimits initial_custom_safety = have_existing_safety
            ? GuidedPaymasterSafetyLimits{
                  existing_safety.per_transaction, existing_safety.reserved,
                  existing_safety.per_hour, existing_safety.per_day,
                  existing_safety.completed_per_hour,
                  existing_safety.completed_per_day}
            : recommended_safety;
        auto* custom_per_transaction = new DgbAmountLineEdit(
            initial_custom_safety.per_transaction, custom_safety);
        custom_per_transaction->setObjectName(
            "paymasterSetupCustomPerTransaction");
        auto* custom_reserved = new DgbAmountLineEdit(initial_custom_safety.reserved, custom_safety);
        custom_reserved->setObjectName("paymasterSetupCustomReserved");
        auto* custom_per_hour = new DgbAmountLineEdit(initial_custom_safety.per_hour, custom_safety);
        custom_per_hour->setObjectName("paymasterSetupCustomPerHour");
        auto* custom_per_day = new DgbAmountLineEdit(initial_custom_safety.per_day, custom_safety);
        custom_per_day->setObjectName("paymasterSetupCustomPerDay");
        auto* custom_completed_per_hour = spin(custom_safety, 1, 1000000,
                                               initial_custom_safety.completed_per_hour);
        custom_completed_per_hour->setObjectName("paymasterSetupCustomCompletedPerHour");
        auto* custom_completed_per_day = spin(custom_safety, 1, 1000000,
                                              initial_custom_safety.completed_per_day);
        custom_completed_per_day->setObjectName("paymasterSetupCustomCompletedPerDay");
        const int initial_max_active_quotes_total = have_existing_safety
            ? m_max_active_quotes_total->value() : 16;
        const int initial_max_active_quotes_per_netgroup = have_existing_safety
            ? m_max_active_quotes_per_netgroup->value() : 4;
        const int initial_max_active_quotes_per_recipient = have_existing_safety
            ? m_max_active_quotes_per_recipient->value() : 2;
        const int initial_max_quote_requests_per_netgroup = have_existing_safety
            ? m_max_quote_requests_per_netgroup->value() : 10;
        auto* custom_max_active_quotes_total = spin(
            custom_safety, 1, 8192, initial_max_active_quotes_total);
        custom_max_active_quotes_total->setObjectName(
            "paymasterSetupCustomMaxActiveQuotesTotal");
        auto* custom_max_active_quotes_per_netgroup = spin(
            custom_safety, 1, 8192, initial_max_active_quotes_per_netgroup);
        custom_max_active_quotes_per_netgroup->setObjectName(
            "paymasterSetupCustomMaxActiveQuotesPerNetgroup");
        auto* custom_max_active_quotes_per_recipient = spin(
            custom_safety, 1, 8192, initial_max_active_quotes_per_recipient);
        custom_max_active_quotes_per_recipient->setObjectName(
            "paymasterSetupCustomMaxActiveQuotesPerRecipient");
        auto* custom_max_quote_requests_per_netgroup = spin(
            custom_safety, 1, 8192, initial_max_quote_requests_per_netgroup);
        custom_max_quote_requests_per_netgroup->setObjectName(
            "paymasterSetupCustomMaxQuoteRequestsPerNetgroupMinute");
        LiquidityPolicyValues existing_liquidity;
        const bool have_existing_liquidity =
            m_liquidity_policy_configured &&
            readLiquidityPolicy(existing_liquidity);
        auto* custom_maintenance_per_transaction = new DgbAmountLineEdit(
            have_existing_liquidity ? existing_liquidity.fee_per_transaction : 20000000,
            custom_safety);
        custom_maintenance_per_transaction->setObjectName("paymasterSetupCustomMaintenancePerTransaction");
        auto* custom_maintenance_per_hour = new DgbAmountLineEdit(
            have_existing_liquidity ? existing_liquidity.fee_per_hour : 200000000,
            custom_safety);
        custom_maintenance_per_hour->setObjectName("paymasterSetupCustomMaintenancePerHour");
        auto* custom_maintenance_per_day = new DgbAmountLineEdit(
            have_existing_liquidity ? existing_liquidity.fee_per_day : 1000000000,
            custom_safety);
        custom_maintenance_per_day->setObjectName("paymasterSetupCustomMaintenancePerDay");
        custom_safety_form->addRow(tr("Network-fee budget per transfer (DGB):"), custom_per_transaction);
        custom_safety_form->addRow(tr("Reserved network-fee budget (DGB):"), custom_reserved);
        custom_safety_form->addRow(tr("Network-fee budget per rolling hour (DGB):"), custom_per_hour);
        custom_safety_form->addRow(tr("Network-fee budget per rolling day (DGB):"), custom_per_day);
        custom_safety_form->addRow(tr("Completed transfers per hour:"), custom_completed_per_hour);
        custom_safety_form->addRow(tr("Completed transfers per day:"), custom_completed_per_day);
        custom_safety_form->addRow(tr("Active quotes across all peers:"), custom_max_active_quotes_total);
        custom_safety_form->addRow(tr("Active quotes per netgroup:"), custom_max_active_quotes_per_netgroup);
        custom_safety_form->addRow(tr("Active quotes per recipient bucket:"), custom_max_active_quotes_per_recipient);
        custom_safety_form->addRow(tr("Quote requests per netgroup/minute:"), custom_max_quote_requests_per_netgroup);
        custom_safety_form->addRow(tr("Paid maintenance per transaction (DGB):"), custom_maintenance_per_transaction);
        custom_safety_form->addRow(tr("Paid maintenance per rolling hour (DGB):"), custom_maintenance_per_hour);
        custom_safety_form->addRow(tr("Paid maintenance per rolling day (DGB):"), custom_maintenance_per_day);
        const auto safety_matches = [](const FundingSafetyValues& values,
                                       const GuidedPaymasterSafetyLimits& limits) {
            return values.per_transaction == limits.per_transaction &&
                values.reserved == limits.reserved &&
                values.per_hour == limits.per_hour &&
                values.per_day == limits.per_day &&
                values.completed_per_hour == limits.completed_per_hour &&
                values.completed_per_day == limits.completed_per_day;
        };
        const auto selected_models_match_safety =
            [this, safety_matches](const GuidedPaymasterSafetyLimits& limits) {
                FundingSafetyValues values;
                if (m_user_paid->isChecked() &&
                    (!readFundingSafety(m_user_paid_safety, values) ||
                     !safety_matches(values, limits))) {
                    return false;
                }
                if (m_sponsored->isChecked()) {
                    const FundingSafetyControls& controls =
                        m_scope->currentData().toString() ==
                            QLatin1String("restricted")
                        ? m_restricted_sponsored_safety
                        : m_public_sponsored_safety;
                    if (!readFundingSafety(controls, values) ||
                        !safety_matches(values, limits)) {
                        return false;
                    }
                }
                return true;
            };
        QString initial_safety_profile{QStringLiteral("recommended")};
        const bool default_quote_limits =
            initial_max_active_quotes_total == 16 &&
            initial_max_active_quotes_per_netgroup == 4 &&
            initial_max_active_quotes_per_recipient == 2 &&
            initial_max_quote_requests_per_netgroup == 10;
        if (have_existing_safety || have_existing_liquidity) {
            const bool conservative_maintenance = !have_existing_liquidity ||
                (existing_liquidity.fee_per_transaction == 10000000 &&
                 existing_liquidity.fee_per_hour == 50000000 &&
                 existing_liquidity.fee_per_day == 200000000);
            const bool recommended_maintenance = !have_existing_liquidity ||
                (existing_liquidity.fee_per_transaction == 20000000 &&
                 existing_liquidity.fee_per_hour == 200000000 &&
                 existing_liquidity.fee_per_day == 1000000000);
            initial_safety_profile =
                (!have_existing_safety ||
                 selected_models_match_safety(conservative_safety)) &&
                    conservative_maintenance && default_quote_limits
                ? QStringLiteral("conservative")
                : (!have_existing_safety ||
                   selected_models_match_safety(recommended_safety)) &&
                      recommended_maintenance && default_quote_limits
                    ? QStringLiteral("recommended")
                    : QStringLiteral("custom");
        }
        safety_profile->setCurrentIndex(
            safety_profile->findData(initial_safety_profile));
        auto safety_profile_edited = std::make_shared<bool>(false);
        custom_safety->setVisible(false);
        auto* safety_summary = new QLabel(safety_page);
        safety_summary->setObjectName("paymasterSetupSafetySummary");
        safety_summary->setWordWrap(true);
        const auto current_safety_limits = [safety_profile,
                                            selected_network_fee,
                                            custom_per_transaction, custom_reserved,
                                            custom_per_hour, custom_per_day,
                                            custom_completed_per_hour,
                                            custom_completed_per_day] {
            const bool conservative = safety_profile->currentData().toString() ==
                QLatin1String("conservative");
            GuidedPaymasterSafetyLimits limits = GuidedSafetyLimits(
                conservative, selected_network_fee());
            if (safety_profile->currentData().toString() == QLatin1String("custom")) {
                if (!custom_per_transaction->satoshis(limits.per_transaction) ||
                    !custom_reserved->satoshis(limits.reserved) ||
                    !custom_per_hour->satoshis(limits.per_hour) ||
                    !custom_per_day->satoshis(limits.per_day)) {
                    limits.per_transaction = limits.reserved =
                        limits.per_hour = limits.per_day = -1;
                }
                limits.completed_per_hour = custom_completed_per_hour->value();
                limits.completed_per_day = custom_completed_per_day->value();
            }
            return limits;
        };
        const auto update_safety_summary = [this, safety_profile, safety_summary,
                                            custom_safety, current_safety_limits,
                                            custom_max_active_quotes_total,
                                            custom_max_active_quotes_per_netgroup,
                                            custom_max_active_quotes_per_recipient,
                                            custom_max_quote_requests_per_netgroup] {
            custom_safety->setVisible(safety_profile->currentData().toString() ==
                                      QLatin1String("custom"));
            const GuidedPaymasterSafetyLimits limits = current_safety_limits();
            if (limits.reserved < 0) {
                safety_summary->setText(tr("Enter valid DGB amounts for all custom safety limits."));
                return;
            }
            safety_summary->setText(tr(
                "The selected wallet limit allows %1 DGB per transfer. Spam protection allows "
                "%2 DGB reserved, %3 DGB per rolling hour and %4 DGB per rolling day; "
                "%5 completed transfers per hour and %6 per day. Request admission permits at most "
                "%7 active quotes total, %8 per netgroup, %9 per recipient bucket and %10 quote "
                "requests per netgroup/minute.")
                .arg(QString::number(limits.per_transaction / 100000000.0, 'f', 8),
                     QString::number(limits.reserved / 100000000.0, 'f', 8),
                     QString::number(limits.per_hour / 100000000.0, 'f', 8),
                     QString::number(limits.per_day / 100000000.0, 'f', 8))
                .arg(limits.completed_per_hour)
                .arg(limits.completed_per_day)
                .arg(custom_max_active_quotes_total->value())
                .arg(custom_max_active_quotes_per_netgroup->value())
                .arg(custom_max_active_quotes_per_recipient->value())
                .arg(custom_max_quote_requests_per_netgroup->value()));
        };
        connect(safety_profile, qOverload<int>(&QComboBox::currentIndexChanged),
                safety_page,
                [update_safety_summary, safety_profile_edited, safety_profile,
                 custom_max_active_quotes_total,
                 custom_max_active_quotes_per_netgroup,
                 custom_max_active_quotes_per_recipient,
                 custom_max_quote_requests_per_netgroup] {
                    *safety_profile_edited = true;
                    if (safety_profile->currentData().toString() !=
                        QLatin1String("custom")) {
                        custom_max_active_quotes_total->setValue(16);
                        custom_max_active_quotes_per_netgroup->setValue(4);
                        custom_max_active_quotes_per_recipient->setValue(2);
                        custom_max_quote_requests_per_netgroup->setValue(10);
                    }
                    update_safety_summary();
                });
        connect(network_fee, qOverload<int>(&QSpinBox::valueChanged), safety_page,
                [update_safety_summary](int) { update_safety_summary(); });
        for (QLineEdit* control : {custom_per_transaction, custom_reserved, custom_per_hour,
                                   custom_per_day}) {
            connect(control, &QLineEdit::textChanged, safety_page,
                    [update_safety_summary, safety_profile_edited] {
                        *safety_profile_edited = true;
                        update_safety_summary();
                    });
        }
        for (QSpinBox* control : {custom_completed_per_hour, custom_completed_per_day}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), safety_page,
                    [update_safety_summary, safety_profile_edited](int) {
                        *safety_profile_edited = true;
                        update_safety_summary();
                    });
        }
        for (QLineEdit* control : {custom_maintenance_per_transaction,
                                   custom_maintenance_per_hour,
                                   custom_maintenance_per_day}) {
            connect(control, &QLineEdit::textChanged, safety_page,
                    [update_safety_summary] { update_safety_summary(); });
        }
        for (QSpinBox* control : {custom_max_active_quotes_total,
                                  custom_max_active_quotes_per_netgroup,
                                  custom_max_active_quotes_per_recipient,
                                  custom_max_quote_requests_per_netgroup}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), safety_page,
                    [update_safety_summary](int) { update_safety_summary(); });
        }
        auto* restore_safety_defaults = new QPushButton(tr("Restore defaults"), safety_page);
        restore_safety_defaults->setObjectName("paymasterSetupRestoreSafetyDefaults");
        restore_safety_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        connect(restore_safety_defaults, &QPushButton::clicked, safety_page,
                [safety_profile, network_fee, custom_per_transaction,
                 custom_reserved, custom_per_hour,
                 custom_per_day, custom_completed_per_hour,
                 custom_completed_per_day, custom_maintenance_per_transaction,
                 custom_maintenance_per_hour, custom_maintenance_per_day,
                 custom_max_active_quotes_total,
                 custom_max_active_quotes_per_netgroup,
                 custom_max_active_quotes_per_recipient,
                 custom_max_quote_requests_per_netgroup,
                 safety_profile_edited] {
                    *safety_profile_edited = true;
                    const GuidedPaymasterSafetyLimits defaults = GuidedSafetyLimits(
                        /*conservative=*/false, network_fee->value());
                    custom_per_transaction->setSatoshis(
                        defaults.per_transaction);
                    custom_reserved->setSatoshis(defaults.reserved);
                    custom_per_hour->setSatoshis(defaults.per_hour);
                    custom_per_day->setSatoshis(defaults.per_day);
                    custom_completed_per_hour->setValue(defaults.completed_per_hour);
                    custom_completed_per_day->setValue(defaults.completed_per_day);
                    custom_max_active_quotes_total->setValue(16);
                    custom_max_active_quotes_per_netgroup->setValue(4);
                    custom_max_active_quotes_per_recipient->setValue(2);
                    custom_max_quote_requests_per_netgroup->setValue(10);
                    custom_maintenance_per_transaction->setSatoshis(20000000);
                    custom_maintenance_per_hour->setSatoshis(200000000);
                    custom_maintenance_per_day->setSatoshis(1000000000);
                    safety_profile->setCurrentIndex(
                        safety_profile->findData(QStringLiteral("recommended")));
                });
        update_safety_summary();
        safety_layout->addWidget(safety_text);
        safety_layout->addWidget(safety_profile);
        safety_layout->addWidget(custom_safety);
        safety_layout->addWidget(safety_summary);
        safety_layout->addWidget(restore_safety_defaults, 0, Qt::AlignLeft);
        safety_layout->addStretch();
        wizard.addPage(safety_page);

        auto* pool_page = new QWizardPage(&wizard);
        pool_page->setObjectName("paymasterSetupLiquidityPage");
        pool_page->setTitle(tr("Liquidity targets"));
        pool_page->setSubTitle(tr("Choose how many reserve and immediately usable payment slots to prepare."));
        auto* pool_page_layout = new QVBoxLayout(pool_page);
        auto* pool_scroll = new QScrollArea(pool_page);
        pool_scroll->setObjectName("paymasterSetupLiquidityScroll");
        pool_scroll->setWidgetResizable(true);
        pool_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* pool_content = new QWidget(pool_scroll);
        pool_content->setObjectName("paymasterSetupLiquidityContent");
        auto* pool_layout = new QFormLayout(pool_content);
        pool_layout->setSizeConstraint(QLayout::SetMinimumSize);
        pool_scroll->setWidget(pool_content);
        ConfigureScrollArea(pool_scroll, pool_content);
        pool_page_layout->addWidget(pool_scroll);
        auto* automatic_replenishment = new QCheckBox(
            tr("Automatically replenish missing liquidity"), pool_page);
        automatic_replenishment->setObjectName("paymasterSetupAutomaticReplenishment");
        automatic_replenishment->setChecked(
            have_existing_liquidity ? existing_liquidity.automatic_replenishment : true);
        auto* paid_maintenance_approved = new QCheckBox(
            tr("Allow paid automatic liquidity maintenance within the selected limits"),
            pool_page);
        paid_maintenance_approved->setObjectName("paymasterSetupPaidMaintenanceApproved");
        paid_maintenance_approved->setChecked(
            have_existing_liquidity ? existing_liquidity.paid_maintenance_approved : true);
        auto* operation_mode = new NoWheelComboBox(pool_page);
        operation_mode->setObjectName("paymasterSetupOperationMode");
        operation_mode->addItem(tr("Automatic — recommended"), QStringLiteral("automatic"));
        operation_mode->addItem(tr("Manual — expert mode"), QStringLiteral("manual"));
        const int existing_operation_mode = operation_mode->findData(m_operation_mode);
        operation_mode->setCurrentIndex(existing_operation_mode >= 0
            ? existing_operation_mode
            : operation_mode->findData(QStringLiteral("automatic")));
        operation_mode->setEnabled(!m_core_running);
        operation_mode->setToolTip(m_core_running
            ? tr("Stop the provider before changing its processing mode.")
            : tr("Automatic is recommended; manual mode is intended for expert diagnosis."));
        auto* autostart = new QCheckBox(
            tr("Start this provider automatically after loading its wallet"), pool_page);
        autostart->setObjectName("paymasterSetupAutostart");
        autostart->setChecked(m_autostart->isChecked());
        // An identity can be created before any ProviderSettings record exists.
        // Treat that identity-only state as a fresh setup so the usable default
        // remains enabled; preserve an explicit disabled state only for an
        // existing policy/configuration.
        const bool have_existing_provider_configuration =
            m_provider_settings_present || m_policy_loaded ||
            m_provider_safety_configured ||
            m_liquidity_policy_configured;
        auto* provider_enabled = new QCheckBox(
            tr("Enable this wallet's provider configuration"), pool_page);
        provider_enabled->setObjectName("paymasterSetupProviderEnabled");
        provider_enabled->setChecked(
            have_existing_provider_configuration ? m_core_enabled : true);
        auto* admission_dgb = spin(
            pool_page, 3, 16,
            have_existing_liquidity ? existing_liquidity.admission_dgb : 3);
        admission_dgb->setObjectName("paymasterSetupAdmissionDgbSlots");
        admission_dgb->setAccessibleName(tr("Reserve DGB slots for network admission"));
        auto* operational_dgb = spin(
            pool_page, 1, 16,
            have_existing_liquidity ? existing_liquidity.operational_dgb : 1);
        operational_dgb->setObjectName("paymasterSetupOperationalDgbSlots");
        operational_dgb->setAccessibleName(tr("Immediately usable DGB slots"));
        auto* admission_carriers = spin(
            pool_page, 0, 16,
            have_existing_liquidity
                ? existing_liquidity.admission_carriers
                : wizard_user_paid->isChecked() ? 3 : 0);
        admission_carriers->setObjectName("paymasterSetupAdmissionCarrierSlots");
        admission_carriers->setAccessibleName(tr("Reserve DigiDollar carrier slots"));
        auto* operational_carriers = spin(
            pool_page, 0, 16,
            have_existing_liquidity
                ? existing_liquidity.operational_carriers
                : wizard_user_paid->isChecked() ? 1 : 0);
        operational_carriers->setObjectName("paymasterSetupOperationalCarrierSlots");
        operational_carriers->setAccessibleName(tr("Immediately usable DigiDollar carrier slots"));
        pool_layout->addRow(tr("Reserve DGB slots for network admission:"), admission_dgb);
        pool_layout->addRow(tr("Immediately usable DGB slots:"), operational_dgb);
        pool_layout->addRow(tr("Reserve DigiDollar carrier slots:"), admission_carriers);
        pool_layout->addRow(tr("Immediately usable carrier slots:"), operational_carriers);
        pool_layout->addRow(automatic_replenishment);
        pool_layout->addRow(paid_maintenance_approved);
        pool_layout->addRow(tr("Provider operation:"), operation_mode);
        pool_layout->addRow(autostart);
        pool_layout->addRow(provider_enabled);
        auto* restore_liquidity_defaults = new QPushButton(tr("Restore defaults"), pool_page);
        restore_liquidity_defaults->setObjectName("paymasterSetupRestoreLiquidityDefaults");
        restore_liquidity_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        pool_layout->addRow(restore_liquidity_defaults);
        connect(restore_liquidity_defaults, &QPushButton::clicked, pool_page,
                [wizard_user_paid, admission_dgb, operational_dgb,
                 admission_carriers, operational_carriers,
                 automatic_replenishment, paid_maintenance_approved,
                 operation_mode, autostart, provider_enabled, this] {
                    admission_dgb->setValue(3);
                    operational_dgb->setValue(1);
                    admission_carriers->setValue(wizard_user_paid->isChecked() ? 3 : 0);
                    operational_carriers->setValue(wizard_user_paid->isChecked() ? 1 : 0);
                    automatic_replenishment->setChecked(true);
                    paid_maintenance_approved->setChecked(true);
                    if (!m_core_running) {
                        operation_mode->setCurrentIndex(
                            operation_mode->findData(QStringLiteral("automatic")));
                    }
                    autostart->setChecked(false);
                    provider_enabled->setChecked(true);
                });
        auto* liquidity_help = new QFrame(pool_page);
        liquidity_help->setObjectName("paymasterSetupLiquidityFieldHelp");
        liquidity_help->setFrameShape(QFrame::StyledPanel);
        auto* liquidity_help_layout = new QVBoxLayout(liquidity_help);
        liquidity_help_layout->setContentsMargins(12, 9, 12, 9);
        liquidity_help_layout->setSpacing(3);
        auto* liquidity_help_title = new QLabel(liquidity_help);
        liquidity_help_title->setObjectName("paymasterSetupLiquidityFieldHelpTitle");
        auto* liquidity_help_text = new QLabel(liquidity_help);
        liquidity_help_text->setObjectName("paymasterSetupLiquidityFieldHelpText");
        liquidity_help_text->setWordWrap(true);
        liquidity_help_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        liquidity_help_layout->addWidget(liquidity_help_title);
        liquidity_help_layout->addWidget(liquidity_help_text);
        pool_layout->addRow(liquidity_help);

        const auto update_liquidity_help = [this, wizard_user_paid, admission_dgb,
                                            operational_dgb, admission_carriers,
                                            operational_carriers, liquidity_help_title,
                                            liquidity_help_text](QWidget* field) {
            if (field == admission_dgb) {
                liquidity_help_title->setText(tr("Reserve DGB slots for network admission"));
                liquidity_help_text->setText(tr(
                    "%1 separate confirmed DGB outputs will be targeted as the provider's admission reserve. "
                    "They prove that the provider has minimum capacity before a client reveals payment details. "
                    "At least three are required, and they are separate from immediately spendable payment slots.")
                    .arg(admission_dgb->value()));
            } else if (field == operational_dgb) {
                liquidity_help_title->setText(tr("Immediately usable DGB slots"));
                liquidity_help_text->setText(tr(
                    "%1 DGB slot(s) will be targeted for active payments. Each slot can be reserved for one transfer at a time and supplies its DigiByte network fee. "
                    "A higher value permits more parallel work but keeps more wallet funds prepared for Paymaster use.")
                    .arg(operational_dgb->value()));
            } else if (field == admission_carriers) {
                liquidity_help_title->setText(tr("Reserve DigiDollar carrier slots"));
                liquidity_help_text->setText(wizard_user_paid->isChecked()
                    ? tr("%1 confirmed DigiDollar carrier output(s) will be targeted for user-paid admission. At least three are required when the provider accepts service fees in $DD.")
                          .arg(admission_carriers->value())
                    : admission_carriers->value() == 0
                        ? tr("Carrier outputs are not needed for sponsored-only service, so a fresh setup leaves this target at zero.")
                        : tr("Carrier outputs are not needed for sponsored-only service. This existing saved target is retained and can be reused if user-paid service is enabled later."));
            } else if (field == operational_carriers) {
                liquidity_help_title->setText(tr("Immediately usable DigiDollar carrier slots"));
                liquidity_help_text->setText(wizard_user_paid->isChecked()
                    ? tr("%1 carrier slot(s) will be targeted for active user-paid transfers. A complete user-paid operational slot needs both one DGB slot and one carrier slot; the lower count therefore limits parallel user-paid transfers.")
                          .arg(operational_carriers->value())
                    : operational_carriers->value() == 0
                        ? tr("Sponsored transfers charge no $DD service fee and therefore need no operational carrier slots.")
                        : tr("Sponsored transfers do not use carrier slots. This existing saved target is retained for a possible later return to user-paid service."));
            }
        };
        connect(qApp, &QApplication::focusChanged, pool_page,
                [pool_page, admission_dgb, operational_dgb, admission_carriers,
                 operational_carriers, update_liquidity_help](QWidget*, QWidget* focused) {
                    QWidget* candidate = focused;
                    while (candidate && candidate != pool_page) {
                        if (candidate == admission_dgb || candidate == operational_dgb ||
                            candidate == admission_carriers || candidate == operational_carriers) {
                            update_liquidity_help(candidate);
                            return;
                        }
                        candidate = candidate->parentWidget();
                    }
                });
        for (QSpinBox* control : {admission_dgb, operational_dgb,
                                  admission_carriers, operational_carriers}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), pool_page,
                    [control, update_liquidity_help] {
                        if (control->hasFocus()) update_liquidity_help(control);
                    });
        }
        update_liquidity_help(admission_dgb);
        wizard.addPage(pool_page);

        auto* review_page = new ValidatedWizardPage(&wizard);
        review_page->setObjectName("paymasterSetupReviewPage");
        review_page->setTitle(tr("Review the setup plan"));
        review_page->setSubTitle(tr("Review the complete configuration and current liquidity estimate before applying it."));
        auto* review_page_layout = new QVBoxLayout(review_page);
        auto* review_scroll = new QScrollArea(review_page);
        review_scroll->setObjectName("paymasterSetupReviewScroll");
        review_scroll->setWidgetResizable(true);
        review_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* review_content = new QWidget(review_scroll);
        review_content->setObjectName("paymasterSetupReviewContent");
        auto* review_layout = new QVBoxLayout(review_content);
        review_layout->setSizeConstraint(QLayout::SetMinimumSize);
        review_scroll->setWidget(review_content);
        ConfigureScrollArea(review_scroll, review_content);
        review_page_layout->addWidget(review_scroll);
        auto* review = new QLabel(review_page);
        review->setObjectName("paymasterSetupReview");
        review->setWordWrap(true);
        review->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                        Qt::TextSelectableByMouse);
        auto* next_actions = new QLabel(tr(
            "When you apply this plan, the assistant first places an existing enabled provider safely offline, then creates or reuses the identity, saves the selected operating, safety and automatic-liquidity policies, and asks Core to recheck and create only missing pool outputs. Runtime, autostart and the requested enabled state are restored last; an enabled provider with autostart may start afterward as soon as all readiness gates pass."), review_page);
        next_actions->setObjectName("paymasterSetupNextActions");
        next_actions->setWordWrap(true);
        auto* preview_status = new QLabel(tr("Liquidity requirement calculated from the current wallet state."), review_page);
        preview_status->setObjectName("paymasterSetupLiquidityPreviewStatus");
        preview_status->setWordWrap(true);
        auto* preview_details = new QLabel(review_page);
        preview_details->setObjectName("paymasterSetupLiquidityPreviewDetails");
        preview_details->setWordWrap(true);
        preview_details->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                                 Qt::TextSelectableByMouse);
        auto* preview_retry = new QPushButton(tr("Recalculate liquidity requirement"), review_page);
        preview_retry->setObjectName("paymasterSetupLiquidityPreviewRetry");
        preview_retry->setVisible(false);
        auto* maintenance_confirmation = new QCheckBox(tr(
            "I approve automatic paid liquidity maintenance only within the finite limits shown in this review."),
            review_page);
        maintenance_confirmation->setObjectName(
            "paymasterSetupMaintenanceApproval");
        const auto invalidate_maintenance_confirmation =
            [maintenance_confirmation] {
                maintenance_confirmation->setChecked(false);
            };
        for (QSpinBox* control : {minimum, maximum, lifetime, network_fee,
                                  custom_completed_per_hour,
                                  custom_completed_per_day,
                                  custom_max_active_quotes_total,
                                  custom_max_active_quotes_per_netgroup,
                                  custom_max_active_quotes_per_recipient,
                                  custom_max_quote_requests_per_netgroup,
                                  admission_dgb, operational_dgb,
                                  admission_carriers, operational_carriers}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged),
                    review_page,
                    [invalidate_maintenance_confirmation](int) {
                        invalidate_maintenance_confirmation();
                    });
        }
        connect(fee, qOverload<double>(&QDoubleSpinBox::valueChanged),
                review_page,
                [invalidate_maintenance_confirmation](double) {
                    invalidate_maintenance_confirmation();
                });
        const QList<QLineEdit*> review_line_edits{
            display, custom_per_transaction, custom_reserved, custom_per_hour,
            custom_per_day,
            custom_maintenance_per_transaction, custom_maintenance_per_hour,
            custom_maintenance_per_day};
        for (QLineEdit* control : review_line_edits) {
            connect(control, &QLineEdit::textChanged, review_page,
                    [invalidate_maintenance_confirmation] {
                        invalidate_maintenance_confirmation();
                    });
        }
        for (QCheckBox* control : {wizard_user_paid, wizard_sponsored,
                                   automatic_replenishment,
                                   paid_maintenance_approved, autostart,
                                   provider_enabled}) {
            connect(control, &QCheckBox::toggled, review_page,
                    [invalidate_maintenance_confirmation](bool) {
                        invalidate_maintenance_confirmation();
                    });
        }
        for (QComboBox* control : {wizard_scope, safety_profile,
                                   operation_mode}) {
            connect(control, qOverload<int>(&QComboBox::currentIndexChanged),
                    review_page,
                    [invalidate_maintenance_confirmation](int) {
                        invalidate_maintenance_confirmation();
                    });
        }
        review_layout->addWidget(review);
        review_layout->addWidget(next_actions);
        review_layout->addWidget(preview_status);
        review_layout->addWidget(preview_details);
        review_layout->addWidget(maintenance_confirmation);
        review_layout->addWidget(preview_retry, 0, Qt::AlignLeft);
        review_layout->addStretch();
        const int review_id = wizard.addPage(review_page);

        auto* progress_page = new CompletableWizardPage(&wizard);
        progress_page->setObjectName("paymasterSetupProgressPage");
        progress_page->setTitle(tr("Apply Paymaster setup"));
        progress_page->setSubTitle(tr("The assistant saves each setting in a crash-safe order and applies runtime/autostart last."));
        auto* progress_page_layout = new QVBoxLayout(progress_page);
        auto* progress_scroll = new QScrollArea(progress_page);
        progress_scroll->setObjectName("paymasterSetupProgressScroll");
        progress_scroll->setWidgetResizable(true);
        progress_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* progress_content = new QWidget(progress_scroll);
        progress_content->setObjectName("paymasterSetupProgressContent");
        auto* progress_layout = new QVBoxLayout(progress_content);
        progress_layout->setSizeConstraint(QLayout::SetMinimumSize);
        progress_scroll->setWidget(progress_content);
        ConfigureScrollArea(progress_scroll, progress_content);
        progress_page_layout->addWidget(progress_scroll);
        auto* progress_intro = new QLabel(tr(
            "Do not close DigiByte Core while a step is running. Completed steps remain safely persisted if a later step needs to be retried."), progress_page);
        progress_intro->setWordWrap(true);
        progress_layout->addWidget(progress_intro);
        const QStringList progress_names{
            tr("Verify selected provider wallet"),
            tr("Place an existing enabled provider safely offline"),
            tr("Unlock wallet when required"),
            tr("Create or reuse provider identity"),
            tr("Prepare a compatible safety transition when required"),
            tr("Save or retain operating policy"),
            tr("Save provider safety policy"),
            tr("Save automatic liquidity policy"),
            tr("Recheck and create missing pool liquidity"),
            tr("Save provider runtime settings"),
            tr("Save provider enabled state"),
        };
        QList<QLabel*> progress_steps;
        for (int index = 0; index < progress_names.size(); ++index) {
            const QString& name = progress_names.at(index);
            auto* label = new QLabel(tr("○ Pending — %1").arg(name), progress_page);
            label->setObjectName(QStringLiteral("paymasterSetupProgressStep%1").arg(index));
            label->setWordWrap(true);
            progress_layout->addWidget(label);
            progress_steps.push_back(label);
        }
        auto* progress_result = new QLabel(
            tr("The setup has not started yet."), progress_page);
        progress_result->setObjectName("paymasterSetupProgressResult");
        progress_result->setWordWrap(true);
        progress_result->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                                 Qt::TextSelectableByMouse);
        auto* setup_retry = new QPushButton(tr("Retry failed step"), progress_page);
        setup_retry->setObjectName("paymasterSetupRetry");
        setup_retry->setVisible(false);
        auto* setup_backup = new QGroupBox(
            tr("Protect the new provider identity"), progress_page);
        setup_backup->setObjectName("paymasterSetupBackupReminder");
        setup_backup->setProperty("paymasterRole", QStringLiteral("notice"));
        setup_backup->setProperty("statusKind", QStringLiteral("waiting"));
        auto* setup_backup_layout = new QVBoxLayout(setup_backup);
        auto* setup_backup_text = new QLabel(tr(
            "The provider identity key, Paymaster settings, pool state and finance ledger are stored in this wallet. "
            "Create a complete wallet backup and keep it offline and confidential. A seed or descriptor export alone is not a complete Paymaster backup. "
            "The reminder is important but does not block provider start."), setup_backup);
        setup_backup_text->setWordWrap(true);
        auto* setup_backup_id = new QLabel(setup_backup);
        setup_backup_id->setObjectName("paymasterSetupBackupProviderId");
        setup_backup_id->setWordWrap(true);
        setup_backup_id->setTextInteractionFlags(
            Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        auto* setup_backup_actions = new QHBoxLayout();
        auto* setup_backup_now = new QPushButton(
            tr("Back up provider wallet now…"), setup_backup);
        setup_backup_now->setObjectName("paymasterSetupBackupNow");
        setup_backup_now->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        auto* setup_backup_external = new QPushButton(
            tr("I use another full-wallet backup method…"), setup_backup);
        setup_backup_external->setObjectName("paymasterSetupExternalBackup");
        setup_backup_actions->addWidget(setup_backup_now);
        setup_backup_actions->addWidget(setup_backup_external);
        setup_backup_actions->addStretch();
        setup_backup_layout->addWidget(setup_backup_text);
        setup_backup_layout->addWidget(setup_backup_id);
        setup_backup_layout->addLayout(setup_backup_actions);
        setup_backup->hide();
        connect(setup_backup_now, &QPushButton::clicked, progress_page,
                [this] { requestProviderBackup(); });
        connect(setup_backup_external, &QPushButton::clicked, progress_page,
                [this] { acknowledgeExternalBackup(); });
        progress_layout->addSpacing(10);
        progress_layout->addWidget(progress_result);
        progress_layout->addWidget(setup_backup);
        progress_layout->addWidget(setup_retry, 0, Qt::AlignLeft);
        progress_layout->addStretch();
        const int progress_id = wizard.addPage(progress_page);

        auto reviewed_pool = std::make_shared<UniValue>();
        const auto wizard_pool_options = [admission_dgb, operational_dgb,
                                          admission_carriers,
                                          operational_carriers](bool execute) {
            UniValue options{UniValue::VOBJ};
            options.pushKV("admission_dgb_slots", admission_dgb->value());
            options.pushKV("operational_dgb_slots", operational_dgb->value());
            options.pushKV("admission_carrier_slots", admission_carriers->value());
            options.pushKV("operational_carrier_slots", operational_carriers->value());
            options.pushKV("execute", execute);
            UniValue params{UniValue::VARR};
            params.push_back(std::move(options));
            return params;
        };
        const auto wizard_pool_preview_text = [this](const UniValue& result) {
            QString text = formatPoolResult("preparepaymasterpool", result);
            text.replace(
                tr("Review these values. If they are acceptable, use the matching Execute reviewed action without changing the targets."),
                tr("These targets form the current estimate. Core will recheck the exact missing outputs before any funding transaction."));
            return text;
        };
        auto refresh_preview = std::make_shared<std::function<void()>>();
        *refresh_preview = [this, review_page, preview_status, preview_details,
                            preview_retry, reviewed_pool, admission_dgb,
                            operational_dgb, admission_carriers,
                            operational_carriers, selected_network_fee,
                            maintenance_confirmation,
                            wizard_pool_preview_text] {
            const int missing_admission_dgb =
                std::max(0, admission_dgb->value() - m_pool_admission_dgb);
            const int missing_operational_dgb =
                std::max(0, operational_dgb->value() - m_pool_operational_dgb);
            const int missing_admission_carriers =
                std::max(0, admission_carriers->value() - m_pool_admission_carriers);
            const int missing_operational_carriers =
                std::max(0, operational_carriers->value() - m_pool_operational_carriers);
            const qint64 admission_value =
                DigiDollar::Paymaster::MIN_ADMISSION_DGB_SATOSHIS;
            const qint64 operational_value = std::max<qint64>(
                admission_value, selected_network_fee());
            UniValue estimate{UniValue::VOBJ};
            estimate.pushKV("executed", false);
            estimate.pushKV("missing_admission_dgb_slots", missing_admission_dgb);
            estimate.pushKV("missing_operational_dgb_slots", missing_operational_dgb);
            estimate.pushKV("missing_admission_carrier_slots", missing_admission_carriers);
            estimate.pushKV("missing_operational_carrier_slots", missing_operational_carriers);
            estimate.pushKV("admission_dgb_satoshis_each", admission_value);
            estimate.pushKV("operational_dgb_satoshis_each", operational_value);
            estimate.pushKV("carrier_cents_each", 100);
            estimate.pushKV("total_output_satoshis",
                admission_value * missing_admission_dgb +
                operational_value * missing_operational_dgb);
            estimate.pushKV("total_carrier_cents",
                100 * (missing_admission_carriers + missing_operational_carriers));
            *reviewed_pool = std::move(estimate);
            preview_status->setText(m_pool_status_loaded
                ? tr("Funding estimate complete — no funds have moved. Core will recheck all existing and pending pool outputs after saving the policies and before any funding transaction.")
                : tr("Conservative funding estimate — provider pool status has not loaded yet, so this estimate assumes no existing confirmed outputs. Core will recheck the wallet before any funding transaction."));
            preview_details->setText(wizard_pool_preview_text(*reviewed_pool) + tr(
                "\nThe pool transaction network fee is calculated by Core immediately before funding and requires a separate confirmation. Pending wallet-owned pool outputs can only reduce the amount shown here."));
            preview_retry->setVisible(false);
            review_page->setComplete(
                maintenance_confirmation->isChecked());
        };
        connect(preview_retry, &QPushButton::clicked, review_page,
                [refresh_preview] { (*refresh_preview)(); });
        connect(maintenance_confirmation, &QCheckBox::toggled, review_page,
                [review_page](bool checked) {
                    review_page->setComplete(checked);
                });

        connect(&wizard, &QWizard::currentIdChanged, &wizard,
                [=, &wizard](int current_id) {
                    wizard.setButtonText(QWizard::NextButton,
                        current_id == review_id ? tr("Apply setup…") : tr("Next >"));
                    if (current_id != review_id) return;
                    QString model;
                    if (wizard_user_paid->isChecked() && wizard_sponsored->isChecked()) {
                        model = tr("User paid and public sponsored");
                    } else if (wizard_user_paid->isChecked()) {
                        model = tr("User paid");
                    } else if (wizard_scope->currentData().toString() == QLatin1String("restricted")) {
                        model = tr("Restricted sponsored");
                    } else {
                        model = tr("Public sponsored");
                    }
                    QString review_text = tr(
                        "Provider wallet: %1\n"
                        "Provider name: %2\n"
                        "Service model: %3\n"
                        "Payment range: %4 to %5 DD\n"
                        "User-paid service fee: %6%\n"
                        "Quote validity: %7 seconds\n"
                        "Network-fee ceiling: %8 DGB per transfer\n"
                        "Safety profile: %9\n"
                        "Provider operation: %10; autostart %11; configuration %12")
                        .arg(setup_wallet_name)
                        .arg(display->text().trimmed().isEmpty() ? tr("No public name") : display->text().trimmed())
                        .arg(model)
                        .arg(QString::number(minimum->value() / 100.0, 'f', 2))
                        .arg(QString::number(maximum->value() / 100.0, 'f', 2))
                        .arg(QString::number(fee->value(), 'f', 2))
                        .arg(lifetime->value())
                        .arg(QString::number(selected_network_fee() / 100000000.0, 'f', 8))
                        .arg(safety_profile->currentText())
                        .arg(operation_mode->currentText(),
                             autostart->isChecked() ? tr("on") : tr("off"),
                             provider_enabled->isChecked() ? tr("enabled")
                                                           : tr("disabled"));
                    if (!*replace_unrepresentable_policy) {
                        review_text += tr(
                            "\nOperating policy: the exact persisted Core value remains unchanged because it is outside this interface's numeric range. Exact retained record: %1")
                            .arg(QString::fromStdString(
                                m_unrepresentable_policy_snapshot.write()));
                    }
                    review_text += tr(
                        "\nLiquidity targets: admission DGB %1, operational DGB %2, admission carriers %3, operational carriers %4")
                        .arg(admission_dgb->value())
                        .arg(operational_dgb->value())
                        .arg(admission_carriers->value())
                        .arg(operational_carriers->value());
                    review_text += tr(
                        "\nRequest limits: %1 active quotes total, %2 per netgroup, %3 per recipient bucket and %4 quote requests per netgroup/minute")
                        .arg(custom_max_active_quotes_total->value())
                        .arg(custom_max_active_quotes_per_netgroup->value())
                        .arg(custom_max_active_quotes_per_recipient->value())
                        .arg(custom_max_quote_requests_per_netgroup->value());
                    const bool conservative =
                        safety_profile->currentData().toString() ==
                        QLatin1String("conservative");
                    qint64 maintenance_per_transaction =
                        conservative ? 10000000 : 20000000;
                    qint64 maintenance_per_hour =
                        conservative ? 50000000 : 200000000;
                    qint64 maintenance_per_day =
                        conservative ? 200000000 : 1000000000;
                    if (safety_profile->currentData().toString() ==
                        QLatin1String("custom")) {
                        custom_maintenance_per_transaction->satoshis(
                            maintenance_per_transaction);
                        custom_maintenance_per_hour->satoshis(maintenance_per_hour);
                        custom_maintenance_per_day->satoshis(maintenance_per_day);
                    }
                    review_text += tr(
                        "\nAutomatic replenishment: %1; paid maintenance: %2, limited to %3 DGB per maintenance transaction, %4 DGB per rolling hour and %5 DGB per rolling day")
                        .arg(automatic_replenishment->isChecked() ? tr("enabled") : tr("disabled"),
                             paid_maintenance_approved->isChecked() ? tr("approved") : tr("disabled"),
                             dgbAmount(maintenance_per_transaction),
                             dgbAmount(maintenance_per_hour),
                             dgbAmount(maintenance_per_day));
                    maintenance_confirmation->setText(
                        paid_maintenance_approved->isChecked()
                        ? tr("I approve automatic paid liquidity maintenance for wallet \"%1\" up to %2 DGB per transaction, %3 DGB per rolling hour and %4 DGB per rolling day. This approval alone does not start the provider; the enabled and autostart choices shown above determine whether Core may start it after setup.")
                              .arg(setup_wallet_name,
                                   dgbAmount(maintenance_per_transaction),
                                   dgbAmount(maintenance_per_hour),
                                   dgbAmount(maintenance_per_day))
                        : tr("I understand that paid automatic liquidity maintenance remains disabled for wallet \"%1\". The finite limits are retained but cannot authorize a paid refill until I explicitly enable and save that approval.")
                              .arg(setup_wallet_name));
                    if (m_core_has_identity) {
                        review_text += tr(
                            "\nExisting provider identity: retained unchanged; its saved display name remains authoritative.");
                    } else {
                        review_text += tr("\nProvider identity: a new persistent BIP86 identity will be created.");
                    }
                    review->setText(review_text);
                    (*refresh_preview)();
                });

        review_page->setValidator([&, reviewed_pool,
                                   wizard_pool_preview_text] {
            QString validation_error;
            const bool custom_profile = safety_profile->currentData().toString() ==
                QLatin1String("custom");
            const GuidedPaymasterSafetyLimits selected_safety = current_safety_limits();
            qint64 custom_maintenance_transaction{0};
            qint64 custom_maintenance_hour{0};
            qint64 custom_maintenance_day{0};
            const bool custom_maintenance_valid = !custom_profile ||
                (custom_maintenance_per_transaction->satoshis(custom_maintenance_transaction) &&
                 custom_maintenance_per_hour->satoshis(custom_maintenance_hour) &&
                 custom_maintenance_per_day->satoshis(custom_maintenance_day));
            if (!m_core_has_identity &&
                !DigiDollar::Paymaster::IsValidPaymasterDisplayName(
                    display->text().toStdString())) {
                validation_error = tr(
                    "The provider display name must contain at most 32 printable ASCII characters and cannot contain '/' or '@'.");
            } else if (*replace_unrepresentable_policy &&
                       !wizard_user_paid->isChecked() &&
                       !wizard_sponsored->isChecked()) {
                validation_error = tr("Select at least one service model.");
            } else if (custom_profile && selected_safety.per_transaction < 0) {
                validation_error = tr(
                    "One or more custom provider-safety amounts are outside the supported whole-satoshi range.");
            } else if (!custom_maintenance_valid) {
                validation_error = tr(
                    "One or more custom maintenance-fee amounts are outside the supported whole-satoshi range.");
            } else if (!maintenance_confirmation->isChecked()) {
                validation_error = tr(
                    "Review and explicitly approve the finite automatic liquidity-maintenance limits before applying setup.");
            }
            if (!validation_error.isEmpty()) {
                QMessageBox::warning(&wizard, tr("Review Paymaster setup"),
                                     validation_error);
                return false;
            }
            QString confirmation = tr(
                "Apply and save this complete Paymaster setup in wallet \"%1\"?\n\n%2\n\n"
                "Only missing pool outputs will be created. Before any funds move, Core will recheck the exact missing outputs and ask you to confirm the resulting DGB/DD funding separately. Future paid replenishment is limited by the maintenance ceilings you explicitly approved above.")
                .arg(setup_wallet_name, wizard_pool_preview_text(*reviewed_pool));
            confirmation += provider_enabled->isChecked() && autostart->isChecked()
                ? tr("\n\nThe selected enabled/autostart combination may start the provider automatically after the configuration and liquidity steps finish and all readiness gates pass.")
                : tr("\n\nThis assistant will not start the provider automatically because either the provider configuration is disabled or autostart is off.");
            // The injected RPC transport exists only in widget tests, where
            // native modal message boxes are not reliable on headless Qt
            // platforms. Production always requires the explicit approval.
            if (!m_rpc_executor_for_testing &&
                askPlainTextQuestion(
                    &wizard, tr("Confirm complete Paymaster setup"),
                    confirmation) != QMessageBox::Yes) {
                return false;
            }
            setSetupMode(PaymasterSetupMode::GUIDED);
            return true;
        });

        auto setup_step = std::make_shared<int>(0);
        auto setup_running = std::make_shared<bool>(false);
        auto setup_started = std::make_shared<bool>(false);
        auto setup_completed = std::make_shared<bool>(false);
        auto setup_needs_quiesce = std::make_shared<bool>(false);
        auto setup_needs_identity = std::make_shared<bool>(false);
        auto setup_needs_liquidity = std::make_shared<bool>(false);
        auto setup_provider_id = std::make_shared<QString>();
        auto identity_params = std::make_shared<UniValue>();
        auto policy_params = std::make_shared<UniValue>();
        auto safety_params = std::make_shared<UniValue>();
        auto transition_safety_params = std::make_shared<UniValue>();
        auto liquidity_policy_params = std::make_shared<UniValue>();
        auto liquidity_preview_params = std::make_shared<UniValue>();
        auto liquidity_params = std::make_shared<UniValue>();
        auto setup_needs_safety_transition = std::make_shared<bool>(false);

        const auto set_progress = [progress_steps, progress_names](int step,
                                                                  const QString& marker,
                                                                  const QString& state) {
            progress_steps.at(step)->setText(
                QStringLiteral("%1 %2 — %3").arg(marker, state, progress_names.at(step)));
        };
        const auto set_wizard_running = [&wizard](bool running) {
            wizard.setCloseBlocked(running);
            if (wizard.button(QWizard::BackButton)) {
                wizard.button(QWizard::BackButton)->setEnabled(false);
            }
            if (wizard.button(QWizard::CancelButton)) {
                wizard.button(QWizard::CancelButton)->setEnabled(!running);
            }
        };
        const auto fail_setup = [&, setup_running, setup_retry, progress_result,
                                 setup_step, setup_needs_quiesce, set_progress,
                                 set_wizard_running](
                                        const QString& error) {
            *setup_running = false;
            set_progress(*setup_step, QStringLiteral("✕"), tr("Failed"));
            progress_result->setText(tr(
                "Setup stopped at this step: %1\n\nCompleted earlier steps remain saved. Correct the problem and retry; no completed step needs to be repeated manually.%2")
                .arg(error,
                     *setup_needs_quiesce && *setup_step > 1
                         ? tr(" The existing provider was placed safely offline before configuration changed and remains disabled until the final enabled-state step succeeds.")
                         : QString{}));
            setup_retry->setEnabled(true);
            setup_retry->setVisible(true);
            set_wizard_running(false);
        };

        auto advance_setup = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak_advance_setup{advance_setup};
        *advance_setup = [this, &wizard, progress_page, progress_result,
                          setup_backup, setup_backup_id,
                          progress_steps, progress_names, setup_retry,
                          setup_step, setup_running, setup_completed,
                          setup_needs_quiesce,
                          setup_needs_identity, setup_needs_liquidity,
                          setup_provider_id,
                          identity_params, policy_params,
                          safety_params, transition_safety_params,
                          setup_needs_safety_transition,
                          liquidity_policy_params,
                          liquidity_preview_params, liquidity_params, set_progress,
                          set_wizard_running, fail_setup, weak_advance_setup,
                          setup_model, setup_wallet_id, setup_wallet_name,
                          display, operation_mode, autostart,
                          provider_enabled,
                          replace_unrepresentable_policy] {
            const auto advance = weak_advance_setup.lock();
            if (!advance) return;
            if (*setup_step >= progress_steps.size()) {
                *setup_running = false;
                *setup_completed = true;
                m_policy_loaded = *replace_unrepresentable_policy;
                m_policy_dirty = false;
                m_provider_safety_configured = true;
                m_provider_safety_dirty = false;
                m_liquidity_policy_configured = true;
                m_liquidity_policy_dirty = false;
                m_core_enabled = provider_enabled->isChecked();
                const bool requested_enabled = provider_enabled->isChecked();
                const bool requested_autostart = autostart->isChecked();
                const bool may_autostart = requested_enabled &&
                                           requested_autostart;
                const QString start_outcome = may_autostart
                    ? tr("Autostart is enabled, so Core may start the provider automatically as soon as every readiness gate passes.")
                    : !requested_enabled
                        ? tr("The provider configuration is disabled. Its saved autostart preference cannot bring it online until the configuration is enabled again.")
                        : tr("Autostart is disabled, so the provider remains stopped until you start it explicitly.");
                progress_result->setText(m_setup_waiting_for_confirmations
                    ? tr("All settings were saved successfully in wallet \"%1\". You do not need to click any additional Save buttons. New pool outputs are waiting for blockchain confirmations; continue to Overview. %2")
                          .arg(setup_wallet_name,
                               may_autostart
                                   ? tr("Autostart is enabled, so Core may start the provider automatically once those confirmations and every other readiness gate are complete.")
                                   : start_outcome)
                    : tr("All settings were saved successfully in wallet \"%1\". You do not need to click any additional Save buttons. All setup requirements can now be checked on Overview. %2")
                          .arg(setup_wallet_name, start_outcome));
                if (*setup_needs_identity && !setup_provider_id->isEmpty()) {
                    setup_backup_id->setText(
                        tr("Provider ID: %1").arg(*setup_provider_id));
                    setup_backup->show();
                    updateBackupReminder(true, *setup_provider_id);
                }
                setup_retry->setVisible(false);
                progress_page->setComplete(true);
                wizard.setButtonText(QWizard::FinishButton, tr("Go to Overview"));
                set_wizard_running(false);
                if (m_setup_waiting_for_confirmations) m_setup_status_timer->start();
                refreshStatus();
                return;
            }

            *setup_running = true;
            setup_retry->setVisible(false);
            set_wizard_running(true);
            set_progress(*setup_step, QStringLiteral("…"), tr("Running"));
            const auto succeed = [this, setup_step, set_progress,
                                  weak_advance_setup] {
                set_progress(*setup_step, QStringLiteral("✓"), tr("Done"));
                ++*setup_step;
                if (const auto advance = weak_advance_setup.lock()) (*advance)();
            };

            switch (*setup_step) {
            case 0: {
                if (!setup_model || m_model != setup_model ||
                    m_model->getWalletName() != setup_wallet_id) {
                    fail_setup(tr(
                        "The selected wallet changed or was unloaded. No setup setting was written. Close the assistant, select the intended wallet and start again."));
                    return;
                }
                call("getpaymasterinfo", {}, false, nullptr,
                     [this, setup_needs_quiesce, setup_needs_identity,
                      setup_provider_id, succeed, fail_setup](const UniValue& result) {
                         if (!IsCompleteProviderInfoSnapshot(result)) {
                             fail_setup(tr(
                                 "Core returned an incomplete provider status while setup was starting. No setup setting was written; refresh status before retrying."));
                             return;
                         }
                         const UniValue& eligible = result.find_value("wallet_eligible");
                         if (!eligible.get_bool()) {
                             fail_setup(tr(
                                 "Core rejected this provider wallet. Use a descriptor wallet with local private keys and no external signer."));
                             return;
                         }
                         const UniValue& enabled = result.find_value("enabled");
                         const UniValue& running = result.find_value("running");
                         const bool currently_enabled = enabled.get_bool();
                         const bool currently_running = running.get_bool();
                         *setup_needs_quiesce = currently_enabled ||
                                                 currently_running;
                         const UniValue& settings_present =
                             result.find_value("settings_present");
                         m_provider_settings_present =
                             settings_present.get_bool();
                         m_core_eligible = true;
                         const UniValue& provider = result.find_value("provider_id");
                         m_core_has_identity = provider.isStr() &&
                                               !provider.get_str().empty();
                         *setup_provider_id = m_core_has_identity
                             ? QString::fromStdString(provider.get_str())
                             : QString{};
                         *setup_needs_identity = !m_core_has_identity;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 1: {
                if (!*setup_needs_quiesce) {
                    set_progress(1, QStringLiteral("✓"), tr("Not required"));
                    ++*setup_step;
                    (*advance)();
                    return;
                }
                UniValue disable_params{UniValue::VARR};
                disable_params.push_back(false);
                call("setpaymasterenabled", std::move(disable_params), false,
                     nullptr,
                     [this, succeed, fail_setup](const UniValue& result) {
                         const UniValue& enabled = result.find_value("enabled");
                         if (!enabled.isBool() || enabled.get_bool()) {
                             fail_setup(tr(
                                 "The wallet did not confirm that the existing provider configuration is disabled."));
                             return;
                         }
                         m_provider_settings_present = true;
                         m_core_enabled = false;
                         m_core_running = false;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 2: {
                // Unlock is deliberately not retained as a setup-wide lease.
                // call() obtains it immediately before the two signing RPCs
                // (identity creation and pool execution) and releases it
                // before their result handlers run.
                set_progress(2, QStringLiteral("✓"), tr("Deferred until signing"));
                ++*setup_step;
                (*advance)();
                return;
            }
            case 3:
                if (!*setup_needs_identity) {
                    set_progress(3, QStringLiteral("✓"), tr("Existing identity retained"));
                    ++*setup_step;
                    (*advance)();
                    return;
                }
                call("createpaymasteridentity", *identity_params, true, nullptr,
                     [this, display, setup_provider_id,
                      succeed, fail_setup](const UniValue& result) {
                         const UniValue& display_name =
                             result.find_value("display_name");
                         if (!result.isObject() ||
                             !IsHex256Field(result, "provider_id") ||
                             !IsHex256Field(result, "identity_key") ||
                             !display_name.isStr() ||
                             QString::fromStdString(display_name.get_str()) !=
                                 display->text().trimmed() ||
                             !result.find_value("created_at").isNum()) {
                             fail_setup(tr(
                                 "Core did not confirm the exact new provider identity. Refresh the wallet before retrying setup."));
                             return;
                         }
                         m_core_has_identity = true;
                         const UniValue& provider = result.find_value("provider_id");
                         *setup_provider_id =
                             QString::fromStdString(provider.get_str());
                         m_provider_id = *setup_provider_id;
                         m_display_name->setText(
                             QString::fromStdString(display_name.get_str()));
                         succeed();
                     }, false, fail_setup);
                return;
            case 4:
                if (!*setup_needs_safety_transition) {
                    set_progress(4, QStringLiteral("✓"), tr("Not required"));
                    ++*setup_step;
                    (*advance)();
                    return;
                }
                call("setpaymastersafetypolicy", *transition_safety_params,
                     false, nullptr,
                     [transition_safety_params, succeed,
                      fail_setup](const UniValue& result) {
                         if (!IsExactProviderSafetyAcknowledgement(
                                 (*transition_safety_params)[0], result)) {
                             fail_setup(tr(
                                 "Core did not confirm the temporary provider safety policy."));
                             return;
                         }
                         succeed();
                     },
                     false, fail_setup);
                return;
            case 5:
                if (!*replace_unrepresentable_policy) {
                    set_progress(5, QStringLiteral("✓"),
                                 tr("Persisted policy retained unchanged"));
                    ++*setup_step;
                    (*advance)();
                    return;
                }
                call("setpaymasterpolicy", *policy_params, false, nullptr,
                     [this, policy_params, succeed,
                      fail_setup](const UniValue& result) {
                         if (!IsExactProviderPolicyAcknowledgement(
                                 (*policy_params)[0], result)) {
                             fail_setup(tr(
                                 "Core did not confirm the exact provider operating policy."));
                             return;
                         }
                         loadPolicy((*policy_params)[0]);
                         m_provider_settings_present = true;
                         succeed();
                     }, false, fail_setup);
                return;
            case 6:
                call("setpaymastersafetypolicy", *safety_params, false, nullptr,
                     [this, safety_params, succeed,
                      fail_setup](const UniValue& result) {
                         const UniValue& policy = (*safety_params)[0];
                         if (!IsExactProviderSafetyAcknowledgement(
                                 policy, result)) {
                             fail_setup(tr(
                                 "Core did not confirm the exact final provider safety policy."));
                             return;
                         }
                         m_loading_provider_safety = true;
                         loadFundingSafety(policy.find_value("user_paid"),
                                           m_user_paid_safety);
                         loadFundingSafety(
                             policy.find_value("public_sponsored"),
                             m_public_sponsored_safety);
                         loadFundingSafety(
                             policy.find_value("restricted_sponsored"),
                             m_restricted_sponsored_safety);
                         m_max_active_quotes_total->setValue(
                             policy.find_value(
                                 "maximum_active_quotes_total").getInt<int>());
                         m_max_active_quotes_per_netgroup->setValue(
                             policy.find_value(
                                 "maximum_active_quotes_per_netgroup").getInt<int>());
                         m_max_active_quotes_per_recipient->setValue(
                             policy.find_value(
                                 "maximum_active_quotes_per_recipient").getInt<int>());
                         m_max_quote_requests_per_netgroup->setValue(
                             policy.find_value(
                                 "maximum_quote_requests_per_netgroup_per_minute").getInt<int>());
                         m_loading_provider_safety = false;
                         m_provider_safety_configured = true;
                         m_provider_safety_snapshot_available = true;
                         m_provider_safety_dirty = false;
                         updateFundingSafetyDisplay(m_user_paid_safety, true);
                         updateFundingSafetyDisplay(
                             m_public_sponsored_safety, true);
                         updateFundingSafetyDisplay(
                             m_restricted_sponsored_safety, true);
                         succeed();
                     }, false, fail_setup);
                return;
            case 7: {
                call("setpaymasterliquiditypolicy", *liquidity_policy_params,
                     false, nullptr,
                     [this, liquidity_policy_params,
                      succeed, fail_setup](const UniValue& result) {
                         if (!IsExactLiquidityPolicyAcknowledgement(
                                 (*liquidity_policy_params)[0], result)) {
                             fail_setup(tr(
                                 "Core did not confirm the exact automatic liquidity policy."));
                             return;
                         }
                         loadLiquidityPolicy(result, true);
                         m_liquidity_snapshot_available = true;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 9: {
                UniValue runtime{UniValue::VOBJ};
                runtime.pushKV("operation_mode",
                               operation_mode->currentData().toString().toStdString());
                runtime.pushKV("autostart", autostart->isChecked());
                UniValue runtime_params{UniValue::VARR};
                runtime_params.push_back(std::move(runtime));
                call("setpaymasterruntimesettings", std::move(runtime_params),
                     false, nullptr,
                     [this, operation_mode, autostart, succeed,
                      fail_setup](const UniValue& result) {
                         const QString requested_mode =
                             operation_mode->currentData().toString();
                         const bool requested_autostart = autostart->isChecked();
                         const UniValue& persisted_mode =
                             result.find_value("operation_mode");
                         const UniValue& persisted_autostart =
                             result.find_value("autostart");
                         if (!result.isObject() || !persisted_mode.isStr() ||
                             QString::fromStdString(persisted_mode.get_str()) !=
                                 requested_mode ||
                             !persisted_autostart.isBool() ||
                             persisted_autostart.get_bool() !=
                                 requested_autostart) {
                             fail_setup(tr(
                                 "Core did not confirm the requested provider runtime settings."));
                             return;
                         }
                         m_operation_mode = requested_mode;
                         m_autostart_enabled = requested_autostart;
                         m_runtime_settings_dirty = false;
                         m_loading_runtime_settings = true;
                         m_operation_mode_select->setCurrentIndex(
                             m_operation_mode_select->findData(
                                 m_operation_mode));
                         m_autostart->setChecked(m_autostart_enabled);
                         m_loading_runtime_settings = false;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 10: {
                UniValue enable_params{UniValue::VARR};
                const bool requested_enabled = provider_enabled->isChecked();
                enable_params.push_back(requested_enabled);
                call("setpaymasterenabled", std::move(enable_params), false, nullptr,
                     [this, requested_enabled, succeed, fail_setup](const UniValue& result) {
                         const UniValue& enabled = result.find_value("enabled");
                         if (!enabled.isBool() || enabled.get_bool() != requested_enabled) {
                             fail_setup(tr("The wallet did not confirm the requested provider enabled state."));
                             return;
                         }
                         m_provider_settings_present = true;
                         m_core_enabled = requested_enabled;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 8:
                call("preparepaymasterpool", *liquidity_preview_params, false, nullptr,
                     [this, &wizard, liquidity_params, succeed,
                      fail_setup](const UniValue& preview) {
                         if (!IsCompletePoolPreparationResult(preview) ||
                             preview.find_value("executed").get_bool()) {
                             fail_setup(tr(
                                 "Core returned an incomplete or mutating pool preview. No pool execution was started."));
                             return;
                         }
                         m_liquidity_output->setPlainText(
                             formatPoolResult("preparepaymasterpool", preview));
                         const qint64 missing_outputs =
                             poolNumber(preview, "missing_admission_dgb_slots") +
                             poolNumber(preview, "missing_operational_dgb_slots") +
                             poolNumber(preview, "missing_admission_carrier_slots") +
                             poolNumber(preview, "missing_operational_carrier_slots");
                         if (missing_outputs == 0) {
                             succeed();
                             return;
                         }
                         const QString exact_preview =
                             formatPoolResult("preparepaymasterpool", preview);
                         if (askPlainTextQuestion(
                                 &wizard, tr("Confirm exact pool funding"),
                                  tr("Core has rechecked the current wallet and policies. Create exactly the following missing liquidity?\n\n%1\n\nThe wallet will create only these still-missing outputs. The assistant saves the selected runtime, autostart and enabled settings only after this funding step.")
                                     .arg(exact_preview)) != QMessageBox::Yes) {
                             fail_setup(tr(
                                 "Pool funding was not approved. The operating, safety and automatic-liquidity policies are already saved, but no new liquidity was created. Runtime, autostart and enabled state remain unchanged from the safe offline transition until this step is retried successfully."));
                             return;
                         }
                         const QString preview_plan = QString::fromStdString(
                             preview.find_value("plan_id").get_str());
                         UniValue execute_options = (*liquidity_params)[0];
                         execute_options.pushKV(
                             "plan_id", preview_plan.toStdString());
                         UniValue execute_params{UniValue::VARR};
                         execute_params.push_back(std::move(execute_options));
                          call("preparepaymasterpool", std::move(execute_params),
                               true, nullptr,
                              [this, preview_plan, succeed,
                               fail_setup](const UniValue& result) {
                                  if (!IsCompletePoolPreparationResult(result) ||
                                      !result.find_value("executed").get_bool() ||
                                      QString::fromStdString(
                                          result.find_value("plan_id").get_str()) !=
                                          preview_plan) {
                                      fail_setup(tr(
                                          "Core did not confirm the pool funding execution. Inspect the wallet pool status before retrying."));
                                      return;
                                  }
                                  m_liquidity_output->setPlainText(
                                      formatPoolResult("preparepaymasterpool", result));
                                  m_setup_waiting_for_confirmations = true;
                                  succeed();
                              }, false, fail_setup);
                     }, false, fail_setup);
                return;
            }
        };

        const auto prepare_setup_execution = [&, setup_step, setup_started,
                                              setup_needs_identity,
                                               setup_needs_liquidity,
                                               identity_params, policy_params,
                                               safety_params,
                                               transition_safety_params,
                                               setup_needs_safety_transition,
                                              liquidity_policy_params,
                                              liquidity_preview_params,
                                              liquidity_params,
                                              reviewed_pool, advance_setup,
                                              setup_model, setup_wallet_id,
                                              wallet_confirmation,
                                              replace_unrepresentable_policy] {
            if (*setup_started) return;

            if (!wallet_confirmation->isChecked() || !setup_model ||
                m_model != setup_model ||
                m_model->getWalletName() != setup_wallet_id) {
                *setup_started = true;
                fail_setup(tr(
                    "The confirmed provider wallet is no longer selected. No setup setting was written. Close the assistant and reopen it in the intended wallet."));
                setup_retry->setVisible(false);
                return;
            }
            *setup_started = true;

            const bool needs_safety_transition =
                *replace_unrepresentable_policy && m_policy_loaded &&
                m_provider_safety_configured;
            const qint64 previous_network_fee = m_network_fee->value();
            FundingSafetyValues previous_user_paid;
            FundingSafetyValues previous_public_sponsored;
            FundingSafetyValues previous_restricted_sponsored;
            if (!readFundingSafety(m_user_paid_safety, previous_user_paid) ||
                !readFundingSafety(m_public_sponsored_safety,
                                   previous_public_sponsored) ||
                !readFundingSafety(m_restricted_sponsored_safety,
                                   previous_restricted_sponsored)) {
                fail_setup(tr(
                    "The saved provider safety policy could not be read exactly. No setup setting was written; refresh the Paymaster status before retrying."));
                return;
            }

            const bool conservative =
                safety_profile->currentData().toString() == QLatin1String("conservative");
            const GuidedPaymasterSafetyLimits guided_safety = current_safety_limits();
            FundingSafetyValues user_paid = previous_user_paid;
            FundingSafetyValues public_sponsored = previous_public_sponsored;
            FundingSafetyValues restricted_sponsored =
                previous_restricted_sponsored;
            const auto apply_safety_profile = [guided_safety,
                                                safety_profile_edited,
                                                this](
                    FundingSafetyValues& values, bool selected) {
                if (!selected ||
                    (!*safety_profile_edited &&
                     m_provider_safety_configured)) return;
                values.per_transaction = guided_safety.per_transaction;
                values.reserved = guided_safety.reserved;
                values.per_hour = guided_safety.per_hour;
                values.per_day = guided_safety.per_day;
                values.completed_per_hour = guided_safety.completed_per_hour;
                values.completed_per_day = guided_safety.completed_per_day;
            };
            apply_safety_profile(user_paid, wizard_user_paid->isChecked());
            apply_safety_profile(
                public_sponsored,
                wizard_sponsored->isChecked() &&
                    wizard_scope->currentData().toString() == QLatin1String("public"));
            apply_safety_profile(
                restricted_sponsored,
                wizard_sponsored->isChecked() &&
                    wizard_scope->currentData().toString() == QLatin1String("restricted"));

            UniValue identity{UniValue::VARR};
            identity.push_back(display->text().trimmed().toStdString());
            *identity_params = std::move(identity);

            UniValue funding{UniValue::VARR};
            if (wizard_sponsored->isChecked()) funding.push_back("sponsored");
            if (wizard_user_paid->isChecked()) funding.push_back("user_paid");
            UniValue policy{UniValue::VOBJ};
            policy.pushKV("funding_models", std::move(funding));
            policy.pushKV("sponsorship_scope", wizard_scope->currentData().toString().toStdString());
            policy.pushKV("fee_rate_bps", qRound(fee->value() * 100.0));
            policy.pushKV("min_amount_cents", minimum->value());
            policy.pushKV("max_amount_cents", maximum->value());
            policy.pushKV("quote_ttl", lifetime->value());
            policy.pushKV("maximum_network_fee_dgb_satoshis", network_fee->value());
            UniValue policy_array{UniValue::VARR};
            policy_array.push_back(std::move(policy));
            *policy_params = std::move(policy_array);

            UniValue safety{UniValue::VOBJ};
            safety.pushKV("user_paid", fundingSafetyToJSON(user_paid));
            safety.pushKV("public_sponsored", fundingSafetyToJSON(public_sponsored));
            safety.pushKV("restricted_sponsored", fundingSafetyToJSON(restricted_sponsored));
            safety.pushKV("maximum_active_quotes_total",
                          custom_max_active_quotes_total->value());
            safety.pushKV("maximum_active_quotes_per_netgroup",
                          custom_max_active_quotes_per_netgroup->value());
            safety.pushKV("maximum_active_quotes_per_recipient",
                          custom_max_active_quotes_per_recipient->value());
            safety.pushKV("maximum_quote_requests_per_netgroup_per_minute",
                          custom_max_quote_requests_per_netgroup->value());
            UniValue safety_array{UniValue::VARR};
            safety_array.push_back(std::move(safety));
            *safety_params = std::move(safety_array);

            *setup_needs_safety_transition = needs_safety_transition;
            if (needs_safety_transition) {
                // The operating and safety policies have separate atomic Core
                // setters, and each setter validates against the other policy
                // already in the wallet. A temporary bridge therefore keeps
                // every budget class needed by either policy valid under the
                // lower of both advertised fee ceilings. This is an explicit
                // durable wizard step; it never changes safety_params, whose
                // final Custom values are sent and acknowledged byte-for-byte.
                const qint64 transition_cap = std::min(
                    previous_network_fee,
                    static_cast<qint64>(network_fee->value()));
                const auto bridge_limits =
                    [transition_cap](const FundingSafetyValues& previous,
                                     const FundingSafetyValues& target) {
                        FundingSafetyValues bridge = target.allZero()
                            ? previous : target;
                        if (bridge.allZero()) return bridge;
                        bridge.per_transaction = std::min(
                            bridge.per_transaction, transition_cap);
                        bridge.reserved = std::max(
                            bridge.reserved, bridge.per_transaction);
                        bridge.per_hour = std::max(
                            bridge.per_hour, bridge.per_transaction);
                        bridge.per_day = std::max(
                            bridge.per_day, bridge.per_hour);
                        bridge.completed_per_day = std::max(
                            bridge.completed_per_day,
                            bridge.completed_per_hour);
                        return bridge;
                    };
                UniValue transition{UniValue::VOBJ};
                transition.pushKV(
                    "user_paid",
                    fundingSafetyToJSON(bridge_limits(
                        previous_user_paid, user_paid)));
                transition.pushKV(
                    "public_sponsored",
                    fundingSafetyToJSON(bridge_limits(
                        previous_public_sponsored, public_sponsored)));
                transition.pushKV(
                    "restricted_sponsored",
                    fundingSafetyToJSON(bridge_limits(
                        previous_restricted_sponsored,
                        restricted_sponsored)));
                // Retain the already persisted quote-rate limits for the
                // bridge. User-entered Custom quote values belong only to the
                // exact final policy and are left for Core to validate there.
                transition.pushKV("maximum_active_quotes_total",
                                  m_max_active_quotes_total->value());
                transition.pushKV("maximum_active_quotes_per_netgroup",
                                  m_max_active_quotes_per_netgroup->value());
                transition.pushKV("maximum_active_quotes_per_recipient",
                                  m_max_active_quotes_per_recipient->value());
                transition.pushKV(
                    "maximum_quote_requests_per_netgroup_per_minute",
                    m_max_quote_requests_per_netgroup->value());
                UniValue transition_array{UniValue::VARR};
                transition_array.push_back(std::move(transition));
                *transition_safety_params = std::move(transition_array);
            }

            LiquidityPolicyValues liquidity_policy;
            liquidity_policy.automatic_replenishment =
                automatic_replenishment->isChecked();
            liquidity_policy.paid_maintenance_approved =
                paid_maintenance_approved->isChecked();
            liquidity_policy.admission_dgb = admission_dgb->value();
            liquidity_policy.operational_dgb = operational_dgb->value();
            liquidity_policy.admission_carriers = admission_carriers->value();
            liquidity_policy.operational_carriers =
                operational_carriers->value();
            liquidity_policy.fee_per_transaction =
                conservative ? 10000000 : 20000000;
            liquidity_policy.fee_per_hour =
                conservative ? 50000000 : 200000000;
            liquidity_policy.fee_per_day =
                conservative ? 200000000 : 1000000000;
            if (safety_profile->currentData().toString() ==
                    QLatin1String("custom") &&
                (!custom_maintenance_per_transaction->satoshis(
                     liquidity_policy.fee_per_transaction) ||
                 !custom_maintenance_per_hour->satoshis(
                     liquidity_policy.fee_per_hour) ||
                 !custom_maintenance_per_day->satoshis(
                     liquidity_policy.fee_per_day))) {
                fail_setup(tr(
                    "The selected automatic-liquidity targets or maintenance ceilings are outside the supported range."));
                return;
            }
            UniValue liquidity_policy_array{UniValue::VARR};
            liquidity_policy_array.push_back(
                liquidityPolicyToJSON(liquidity_policy));
            *liquidity_policy_params = std::move(liquidity_policy_array);
            *liquidity_preview_params = wizard_pool_options(false);
            *liquidity_params = wizard_pool_options(true);

            const qint64 missing_outputs =
                poolNumber(*reviewed_pool, "missing_admission_dgb_slots") +
                poolNumber(*reviewed_pool, "missing_operational_dgb_slots") +
                poolNumber(*reviewed_pool, "missing_admission_carrier_slots") +
                poolNumber(*reviewed_pool, "missing_operational_carrier_slots");
            *setup_needs_identity = !m_core_has_identity;
            *setup_needs_liquidity = missing_outputs > 0;
            *setup_step = 0;
            (*advance_setup)();
        };

        connect(setup_retry, &QPushButton::clicked, progress_page,
                [this, progress_page, progress_result, setup_retry,
                 setup_running, advance_setup] {
                    if (*setup_running) return;
                    setup_retry->setEnabled(false);
                    progress_result->setText(tr("Retrying the failed setup step…"));
                    // Leave the RPC error callback before starting the next
                    // attempt. This also gives Qt a chance to paint immediate
                    // retry feedback before an unusually fast failure.
                    QTimer::singleShot(0, progress_page,
                        [setup_retry, setup_running, advance_setup] {
                            if (*setup_running) return;
                            setup_retry->setEnabled(true);
                            (*advance_setup)();
                        });
                });
        connect(&wizard, &QWizard::currentIdChanged, progress_page,
                [&, progress_id, prepare_setup_execution](int current_id) {
                    if (current_id != progress_id) return;
                    wizard.setButtonText(QWizard::FinishButton, tr("Applying setup…"));
                    // Applying is intentionally incremental and durable. Once
                    // this page is entered, closing cannot roll back completed
                    // RPC steps, so do not continue to present it as Cancel.
                    wizard.setButtonText(QWizard::CancelButton, tr("Close"));
                    if (wizard.button(QWizard::FinishButton)) {
                        wizard.button(QWizard::FinishButton)->setEnabled(false);
                    }
                    prepare_setup_execution();
                });

        // The wizard owns a stable wallet snapshot. Pause background provider
        // and finance refreshes for the whole modal session so they cannot
        // overwrite reviewed values or contend with an applying RPC step.
        for (QLabel* label : wizard.findChildren<QLabel*>()) {
            label->setTextFormat(Qt::PlainText);
        }
        m_setup_wizard_active = true;
        updateAutomaticRefreshTimer();
        wizard.exec();
        m_setup_wizard_active = false;
        m_setup_wizard.clear();
        updateAutomaticRefreshTimer();
        const bool completed = *setup_completed;
        if (!completed && m_model == setup_model && !m_busy) {
            // A failed or deliberately closed durable phase can leave a
            // bridge policy persisted. Reload every authoritative component
            // instead of carrying an intermediate value forward as an
            // unsaved Expert-mode edit on the next wizard run.
            m_policy_dirty = false;
            m_provider_safety_dirty = false;
            m_liquidity_policy_dirty = false;
            m_runtime_settings_dirty = false;
            refreshStatus();
        }
        return completed;
    }

    using ResultHandler = std::function<void(const UniValue&)>;
    using ErrorHandler = std::function<void(const QString&)>;

    struct PendingRpcCall {
        std::string command;
        UniValue params;
        bool needs_unlock{false};
        QPointer<QPlainTextEdit> output;
        ResultHandler handler;
        bool show_error{true};
        ErrorHandler error_handler;
    };

    void setRpcBusyState(bool busy)
    {
        m_busy = busy;

        // Stop automatic refresh before an unlock dialog or RPC can enter a
        // nested event loop. Mutation controls remain disabled until every
        // result/error handler and any modal review it opens have returned.
        updateAutomaticRefreshTimer();
        setPolicyMutationEnabled(m_policy_snapshot_representable);
        setProviderSafetyMutationEnabled(
            m_provider_safety_snapshot_representable);
        setLiquidityPolicyMutationEnabled(
            m_liquidity_snapshot_representable);
        if (m_client_safety_group) {
            m_client_safety_group->setEnabled(
                !m_busy && m_client_safety_snapshot_representable);
        }
        if (m_create_identity) {
            m_create_identity->setEnabled(
                !m_busy && !m_core_has_identity &&
                m_display_name->hasAcceptableInput());
        }
        for (QPushButton* setup_action : {
                 m_guided_setup, m_expert_setup, m_reopen_wizard}) {
            if (setup_action) setup_action->setEnabled(!m_busy);
        }
        updateProviderButtons();
        updateLiquidityDisplay();
        updateCarrierWithdrawalButtons();
        updateFinanceControls();
        updateBackupReminder(m_backup_required, m_provider_id);
    }

    void finishRpcCall(uint64_t wallet_generation,
                       QPointer<QPlainTextEdit> output,
                       ResultHandler handler, bool show_error,
                       ErrorHandler error_handler, UniValue result,
                       QString error)
    {
        if (m_wallet_generation != wallet_generation) return;

        uint64_t handler_token = ++m_next_rpc_handler_token;
        if (handler_token == 0) handler_token = ++m_next_rpc_handler_token;
        const uint64_t previous_token = m_active_rpc_handler_token;
        ++m_rpc_handler_depth;
        m_active_rpc_handler_token = handler_token;

        if (!error.isEmpty()) {
            const QString visible_error = privacySafeBackendError(error);
            if (output) output->setPlainText(visible_error);
            if (error_handler) {
                error_handler(visible_error);
            } else if (show_error) {
                showPlainTextWarning(
                    this, tr("Paymaster operation failed"), visible_error);
            } else {
                m_status->setText(
                    tr("Provider status unavailable: %1")
                        .arg(visible_error));
            }
        } else {
            if (output) {
                output->setPlainText(QString::fromStdString(result.write(2)));
            }
            if (handler) {
                handler(result);
            } else {
                refreshStatus();
            }
        }

        // A wallet switch clears the handler and pending-call state. Do not
        // decrement or dispatch against the newly selected wallet.
        if (m_wallet_generation != wallet_generation) return;
        m_active_rpc_handler_token = previous_token;
        --m_rpc_handler_depth;
        redactVisiblePrivacyText();

        if (m_rpc_handler_depth == 0 && !m_pending_handler_calls.empty()) {
            PendingRpcCall next =
                std::move(m_pending_handler_calls.front());
            m_pending_handler_calls.pop_front();
            dispatchRpcCall(std::move(next));
            return;
        }
        if (m_rpc_handler_depth == 0) setRpcBusyState(false);
    }

    void dispatchRpcCall(PendingRpcCall request)
    {
        const uint64_t wallet_generation = m_wallet_generation;

        // Widget tests use a synchronous executor so each user action and its
        // resulting state can be asserted deterministically. Follow-up calls
        // still pass through the same busy/handler-token lifecycle.
        if (m_rpc_executor_for_testing) {
            UniValue result;
            QString error;
            try {
                result = m_rpc_executor_for_testing(request.command,
                                                    request.params);
            } catch (const UniValue& rpc_error) {
                const UniValue& message = rpc_error.find_value("message");
                error = message.isStr()
                    ? QString::fromStdString(message.get_str())
                    : QString::fromStdString(rpc_error.write());
            } catch (const std::exception& exception) {
                error = QString::fromUtf8(exception.what());
            } catch (...) {
                error = tr("Unknown RPC error");
            }
            finishRpcCall(wallet_generation, request.output,
                          std::move(request.handler), request.show_error,
                          std::move(request.error_handler),
                          std::move(result), std::move(error));
            return;
        }

        WalletModel* request_model = m_model;
        if (!request_model) {
            finishRpcCall(
                wallet_generation, request.output,
                std::move(request.handler), request.show_error,
                std::move(request.error_handler), UniValue{},
                tr("No wallet is selected, so the Paymaster RPC was not started."));
            return;
        }

        std::shared_ptr<WalletModel::UnlockContext> unlock;
        if (request.needs_unlock) {
            unlock = request_model->requestUnlockForAsync();
            if (!unlock || !unlock->isValid()) {
                unlock.reset();
                finishRpcCall(
                    wallet_generation, request.output,
                    std::move(request.handler), request.show_error,
                    std::move(request.error_handler), UniValue{},
                    tr("The wallet was not unlocked, so the signing operation was not started."));
                return;
            }
            if (m_model != request_model ||
                m_wallet_generation != wallet_generation) {
                unlock.reset();
                return;
            }
        }

        QPointer<PaymasterWidgetImpl> guard{this};
        request_model->executeRpcAsync(
            std::move(request.command), std::move(request.params),
            [guard, request_model, wallet_generation,
             output = request.output, handler = std::move(request.handler),
             unlock = std::move(unlock), show_error = request.show_error,
             error_handler = std::move(request.error_handler)](
                UniValue result, QString error) mutable {
                // Signing authority is always released before a handler can
                // update controls or open a result/error dialog.
                unlock.reset();
                if (!guard || guard->m_model != request_model ||
                    guard->m_wallet_generation != wallet_generation) {
                    return;
                }
                guard->finishRpcCall(
                    wallet_generation, output, std::move(handler),
                    show_error, std::move(error_handler),
                    std::move(result), std::move(error));
            });
    }

    void call(std::string command, UniValue params, bool needs_unlock,
              QPlainTextEdit* output = nullptr, ResultHandler handler = {},
              bool show_error = true, ErrorHandler error_handler = {})
    {
        const auto report_not_started =
            [this, output, show_error,
             &error_handler](const QString& error) {
                if (output) output->setPlainText(error);
                if (error_handler) {
                    error_handler(error);
                } else if (show_error) {
                    showPlainTextWarning(
                        this, tr("Paymaster operation not started"), error);
                } else {
                    m_status->setText(error);
                }
            };
        if (!m_model && !m_rpc_executor_for_testing) {
            report_not_started(tr(
                "No wallet is selected, so the Paymaster RPC was not started."));
            return;
        }
        if (params.isNull()) params = UniValue{UniValue::VARR};

        PendingRpcCall request;
        request.command = std::move(command);
        request.params = std::move(params);
        request.needs_unlock = needs_unlock;
        request.output = output;
        request.handler = handler;
        request.show_error = show_error;
        request.error_handler = error_handler;

        if (m_busy) {
            QWidget* const modal = QApplication::activeModalWidget();
            const bool only_setup_wizard_is_modal =
                !modal || modal == m_setup_wizard.data();
            if (m_rpc_handler_depth > 0 &&
                m_active_rpc_handler_token != 0 &&
                only_setup_wizard_is_modal &&
                m_pending_handler_calls.size() < 32) {
                m_pending_handler_calls.push_back(std::move(request));
                return;
            }
            report_not_started(tr(
                "Another Paymaster wallet operation is still running, so this RPC was not started."));
            return;
        }
        setRpcBusyState(true);
        dispatchRpcCall(std::move(request));
    }

    WalletModel* m_model{nullptr};
    uint64_t m_wallet_generation{0};
    uint64_t m_oracle_request_generation{0};
    RpcExecutor m_rpc_executor_for_testing;
    bool m_busy{false};
    int m_rpc_handler_depth{0};
    uint64_t m_active_rpc_handler_token{0};
    uint64_t m_next_rpc_handler_token{0};
    // Follow-up status calls requested by the currently executing handler are
    // serialized in-order. The bounded queue supports compound authoritative
    // refreshes without reopening the UI to external re-entrant mutations.
    std::deque<PendingRpcCall> m_pending_handler_calls;
    PaymasterSetupMode m_setup_mode{PaymasterSetupMode::UNDECIDED};
    QTabWidget* m_tabs{nullptr};
    QGroupBox* m_setup_choice{nullptr};
    QWidget* m_setup_content{nullptr};
    QWidget* m_configuration_page{nullptr};
    QWidget* m_safety_page{nullptr};
    QWidget* m_liquidity_page{nullptr};
    QWidget* m_finance_page{nullptr};
    QWidget* m_activity_page{nullptr};
    QPushButton* m_guided_setup{nullptr};
    QPushButton* m_expert_setup{nullptr};
    QPushButton* m_reopen_wizard{nullptr};
    QLabel* m_overview_offer_status{nullptr};
    QLabel* m_overview_safety_status{nullptr};
    QLabel* m_overview_liquidity_status{nullptr};
    QLabel* m_overview_operation_status{nullptr};
    QLabel* m_overview_finance_status{nullptr};
    QPushButton* m_overview_offer_action{nullptr};
    QPushButton* m_overview_safety_action{nullptr};
    QPushButton* m_overview_liquidity_action{nullptr};
    QPushButton* m_overview_operation_action{nullptr};
    QPushButton* m_overview_finance_action{nullptr};
    QGroupBox* m_overview_backup_notice{nullptr};
    QLabel* m_overview_backup_provider_id{nullptr};
    QPushButton* m_overview_backup_now{nullptr};
    QPushButton* m_overview_backup_external{nullptr};
    QLabel* m_wallet;
    QLabel* m_status;
    QLabel* m_identity;
    QLabel* m_pool;
    QLabel* m_next_step;
    QLabel* m_readiness_summary;
    QGroupBox* m_external_prerequisites_card{nullptr};
    QLabel* m_external_blockchain_status{nullptr};
    QLabel* m_external_txindex_status{nullptr};
    QLabel* m_external_broadcast_status{nullptr};
    QLabel* m_external_activation_status{nullptr};
    QLabel* m_external_oracle_status{nullptr};
    QLabel* m_external_oracle_note{nullptr};
    QLabel* m_external_other_heading{nullptr};
    QLabel* m_external_other_status{nullptr};
    QPlainTextEdit* m_readiness;
    QGroupBox* m_liquidity_maintenance_card{nullptr};
    QLabel* m_liquidity_maintenance_state{nullptr};
    QLabel* m_liquidity_maintenance_next_step{nullptr};
    QLabel* m_liquidity_maintenance_cost{nullptr};
    QPushButton* m_approve_liquidity_maintenance{nullptr};
    QPushButton* m_start;
    QPushButton* m_stop;
    QPushButton* m_enable;
    QLabel* m_enable_status;
    QPushButton* m_create_identity{nullptr};
    QLabel* m_offer_identity_status{nullptr};
    QLabel* m_offer_identity_id{nullptr};
    QLineEdit* m_display_name;
    QCheckBox* m_sponsored;
    QCheckBox* m_user_paid;
    QComboBox* m_scope;
    QSpinBox* m_fee_bps;
    QSpinBox* m_min_amount;
    QSpinBox* m_max_amount;
    QSpinBox* m_quote_ttl;
    QSpinBox* m_network_fee;
    QPushButton* m_save_policy{nullptr};
    QPushButton* m_restore_policy_defaults{nullptr};
    QGroupBox* m_provider_safety_group{nullptr};
    QPushButton* m_save_provider_safety{nullptr};
    QLabel* m_funding_model_status;
    QLabel* m_policy_summary;
    QSpinBox* m_admission_dgb;
    QSpinBox* m_operational_dgb;
    QSpinBox* m_admission_carriers;
    QSpinBox* m_operational_carriers;
    QLabel* m_liquidity_summary;
    QLabel* m_liquidity_current_status;
    QLabel* m_liquidity_preview_status;
    QLabel* m_liquidity_dgb_capacity_status{nullptr};
    QLabel* m_liquidity_carrier_capacity_status{nullptr};
    QPushButton* m_liquidity_dgb_capacity_action{nullptr};
    QPushButton* m_liquidity_carrier_capacity_action{nullptr};
    QPushButton* m_liquidity_advanced_toggle{nullptr};
    QCheckBox* m_automatic_replenishment{nullptr};
    QCheckBox* m_paid_maintenance_approved{nullptr};
    QPushButton* m_maintenance_limits_toggle{nullptr};
    QLineEdit* m_maintenance_fee_per_transaction{nullptr};
    QLineEdit* m_maintenance_fee_per_hour{nullptr};
    QLineEdit* m_maintenance_fee_per_day{nullptr};
    QPushButton* m_save_liquidity_policy_primary{nullptr};
    QPushButton* m_save_liquidity_policy{nullptr};
    QPushButton* m_restore_liquidity_defaults{nullptr};
    QLabel* m_liquidity_policy_status{nullptr};
    QLabel* m_liquidity_target_save_status{nullptr};
    QLabel* m_liquidity_slot_status{nullptr};
    QLabel* m_liquidity_recycling_status{nullptr};
    QLabel* m_liquidity_budget_status{nullptr};
    QLabel* m_carrier_value_status{nullptr};
    QLabel* m_carrier_withdrawal_status{nullptr};
    QPushButton* m_preview_carrier_excess{nullptr};
    QPushButton* m_execute_carrier_excess{nullptr};
    QComboBox* m_release_carrier_select{nullptr};
    QPushButton* m_preview_carrier_release{nullptr};
    QPushButton* m_execute_carrier_release{nullptr};
    QPushButton* m_prepare_execute;
    QPushButton* m_rebalance_execute;
    QPushButton* m_prepare_preview{nullptr};
    QPushButton* m_rebalance_preview{nullptr};
    QPlainTextEdit* m_liquidity_output;
    QString m_prepare_preview_target;
    QString m_rebalance_preview_target;
    QString m_prepare_plan_id;
    QString m_rebalance_plan_id;
    QComboBox* m_finance_period_select{nullptr};
    QPushButton* m_finance_refresh{nullptr};
    QPushButton* m_finance_export{nullptr};
    std::array<QLabel*, 4> m_finance_period_income{};
    std::array<QLabel*, 4> m_finance_period_cost{};
    std::array<QLabel*, 4> m_finance_period_transfers{};
    QLabel* m_finance_result_estimate{nullptr};
    QLabel* m_finance_model_breakdown{nullptr};
    QLabel* m_finance_pool_dgb{nullptr};
    QLabel* m_finance_pool_carrier{nullptr};
    QLabel* m_finance_pending_maintenance{nullptr};
    QGroupBox* m_finance_backup_notice{nullptr};
    QLabel* m_finance_backup_text{nullptr};
    QLabel* m_finance_backup_provider_id{nullptr};
    QPushButton* m_finance_backup_now{nullptr};
    QPushButton* m_finance_backup_external{nullptr};
    QPushButton* m_finance_withdraw_fees{nullptr};
    QPushButton* m_finance_release_carrier{nullptr};
    QPushButton* m_finance_review_dgb{nullptr};
    QPushButton* m_finance_add_liquidity{nullptr};
    QPushButton* m_finance_details_toggle{nullptr};
    QPushButton* m_finance_previous_page{nullptr};
    QPushButton* m_finance_next_page{nullptr};
    QLabel* m_finance_page_status{nullptr};
    QGroupBox* m_finance_history_notice{nullptr};
    QLabel* m_finance_history_notice_text{nullptr};
    QLabel* m_finance_history_status{nullptr};
    QTableWidget* m_finance_daily_totals{nullptr};
    QTableWidget* m_finance_events{nullptr};
    UniValue m_finance_last_result{UniValue::VOBJ};
    std::vector<FinancePage> m_finance_pages;
    int m_finance_page_index{-1};
    bool m_finance_loaded{false};
    bool m_finance_loading{false};
    bool m_finance_export_loading{false};
    QString m_finance_export_filename_for_testing;
    QString m_provider_id;
    QString m_provider_endpoint;
    bool m_backup_required{false};
    std::function<void()> m_provider_backup_request_handler;
    QLabel* m_activity_runtime_status;
    QLabel* m_activity_action_result;
    QLabel* m_activity_summary;
    QPushButton* m_operation_primary{nullptr};
    QGroupBox* m_reservations_group{nullptr};
    QGroupBox* m_recovery_group{nullptr};
    QPushButton* m_runtime_settings_toggle{nullptr};
    QGroupBox* m_runtime_settings_panel{nullptr};
    QComboBox* m_operation_mode_select{nullptr};
    QCheckBox* m_autostart{nullptr};
    QPushButton* m_save_runtime_settings{nullptr};
    QLabel* m_runtime_settings_result{nullptr};
    QGroupBox* m_request_processing_group{nullptr};
    QGroupBox* m_submit_processing_group{nullptr};
    QGroupBox* m_activity_result_group{nullptr};
    QPushButton* m_process_requests;
    QPushButton* m_process_submits;
    QPlainTextEdit* m_activity_output;
    FundingSafetyControls m_user_paid_safety;
    FundingSafetyControls m_public_sponsored_safety;
    FundingSafetyControls m_restricted_sponsored_safety;
    QSpinBox* m_max_active_quotes_total;
    QSpinBox* m_max_active_quotes_per_netgroup;
    QSpinBox* m_max_active_quotes_per_recipient;
    QSpinBox* m_max_quote_requests_per_netgroup;
    QLabel* m_provider_safety_status;
    QLabel* m_provider_safety_usage;
    QSpinBox* m_client_fee_per_transaction;
    QSpinBox* m_client_fee_per_day;
    QLabel* m_client_safety_status;
    QLabel* m_client_safety_mode;
    QGroupBox* m_client_safety_group{nullptr};
    QPushButton* m_save_client_safety{nullptr};
    bool m_loading_policy{false};
    bool m_policy_loaded{false};
    bool m_policy_dirty{false};
    bool m_policy_snapshot_representable{true};
    UniValue m_unrepresentable_policy_snapshot{UniValue::VOBJ};
    bool m_provider_safety_configured{false};
    bool m_provider_safety_snapshot_representable{true};
    UniValue m_unrepresentable_provider_safety_snapshot{UniValue::VOBJ};
    bool m_provider_safety_dirty{false};
    bool m_loading_provider_safety{false};
    bool m_loading_liquidity_policy{false};
    bool m_liquidity_policy_configured{false};
    bool m_liquidity_snapshot_representable{true};
    UniValue m_unrepresentable_liquidity_snapshot{UniValue::VOBJ};
    bool m_liquidity_targets_satisfy_provider_policy{false};
    bool m_liquidity_policy_dirty{false};
    bool m_client_safety_configured{false};
    bool m_client_safety_snapshot_representable{true};
    UniValue m_unrepresentable_client_safety_snapshot{UniValue::VOBJ};
    bool m_client_safety_dirty{false};
    bool m_loading_client_safety{false};
    bool m_core_eligible{false};
    bool m_core_enabled{false};
    bool m_core_running{false};
    bool m_core_ready{false};
    bool m_core_locked{true};
    bool m_core_has_identity{false};
    bool m_provider_info_snapshot_available{false};
    bool m_provider_safety_snapshot_available{false};
    bool m_liquidity_snapshot_available{false};
    bool m_provider_settings_present{false};
    OracleState m_oracle_state{OracleState::UNKNOWN};
    qint64 m_oracle_price_micro_usd{0};
    bool m_loading_runtime_settings{false};
    bool m_runtime_settings_dirty{false};
    bool m_autostart_enabled{false};
    QString m_operation_mode{QStringLiteral("automatic")};
    QString m_service_state{QStringLiteral("stopped")};
    QString m_last_service_error;
    QString m_maintenance_state{QStringLiteral("unavailable")};
    QStringList m_readiness_errors;
    qint64 m_carrier_base_cents{0};
    qint64 m_carrier_excess_cents{0};
    qint64 m_pending_successor_carrier_cents{0};
    qint64 m_pending_successor_dgb_satoshis{0};
    QString m_excess_plan_id;
    QString m_release_plan_id;
    qint64 m_excess_plan_expires_at{0};
    qint64 m_release_plan_expires_at{0};
    qint64 m_excess_preview_cents{0};
    qint64 m_excess_preview_fee_satoshis{0};
    qint64 m_waiting_provider_requests{0};
    qint64 m_waiting_provider_submits{0};
    int m_pool_admission_dgb{0};
    int m_pool_admission_carriers{0};
    int m_pool_operational_dgb{0};
    int m_pool_operational_carriers{0};
    bool m_pool_status_loaded{false};
    bool m_setup_waiting_for_confirmations{false};
    bool m_privacy{false};
    bool m_setup_wizard_active{false};
    QTimer* m_setup_status_timer{nullptr};
    QPointer<PaymasterSetupWizard> m_setup_wizard;
};

DigiDollarPaymasterWidget* CreatePaymasterWidget(QWidget* parent)
{
    return new PaymasterWidgetImpl(parent);
}

void ConfigurePaymasterScrollArea(QScrollArea* scroll, QWidget* contents)
{
    ConfigureScrollArea(scroll, contents);
}
