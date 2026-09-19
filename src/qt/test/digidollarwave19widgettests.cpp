// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Wave 19 Agent B (Functional/test auditor) — Qt unit/signal-slot pins for
// release-critical UX. Two of the cases are TDD-red guards for findings
// confirmed during Wave 19:
//
//   * DD-FA-FUNC-030 — DigiDollarRedeemWidget rendered the lock tier as
//     "Tier %1" while the Mint widget and Positions widget render it as
//     human-readable strings ("30 days", "10 years"). The redeem flow is the
//     most safety-critical surface in DigiDollar Qt because it lives next to
//     the redeem-confirmation dialog. A user who reads "Tier 1" in the redeem
//     panel cannot easily cross-check it against the "30 days" they originally
//     selected at mint time, which is a real loss-of-funds-confusion vector
//     and conflicts with `getLockTierDisplayName` in `digidollarmintwidget.cpp`
//     and `lockPeriodName` in `digidollarpositionswidget.cpp`. The pin below
//     covers tiers 0..9 against the canonical labels used elsewhere.
//
//   * DD-FA-TEST-027 — the existing tier dropdown was only smoke-tested via
//     `mintWidgetTests()`; there was no assertion that the combo exposes all
//     ten canonical tiers (BIP9 lock tier set 0..9) and that changing the tier
//     re-evaluates the displayed collateral ratio. These two new pins guard
//     against future regressions in the tier dropdown wiring.
//
//   * DD-FA-TEST-028 — the type filter and txid search of the transactions
//     widget had no direct unit pin. These two tests inject mock DD history
//     rows and assert that selecting "Mints" hides the receive/send/redeem
//     rows, and that typing a txid prefix into the search edit prunes the
//     visible rows accordingly.
//
//   * DD-FA-TEST-029 — the DD coin-control dialog populates from
//     `DigiDollarWallet::GetDDUTXOs()`, which already filters out spent /
//     unconfirmed UTXOs. The dialog must therefore never display a UTXO that
//     is not in the spendable set, and it must show every entry that IS in
//     the spendable set. This pin is the Qt-side mirror of the rh59
//     wallet-side DD lock-bypass coverage (Wave 5 / Wave 17 fix path).

#include <qt/test/digidollarwave19widgettests.h>
#include <qt/test/util.h>

#include <base58.h>
#include <hash.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <kernel/chainparams.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <qt/clientmodel.h>
#include <qt/digidollarcoincontroldialog.h>
#include <qt/digidollarmintwidget.h>
#include <qt/digidollaroverviewwidget.h>
#include <qt/digidollarpositionswidget.h>
#include <qt/digidollarredeemwidget.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollartransactionswidget.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <pubkey.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <memory>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QSet>
#include <QTableWidget>
#include <QTreeWidget>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WalletContext;
using wallet::WalletRescanReserver;

namespace
{

void Wave19SyncUpWallet(const std::shared_ptr<wallet::CWallet>& wallet,
                        interfaces::Node& node)
{
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    wallet::CWallet::ScanResult result = wallet->ScanForWalletTransactions(
        Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, true, false);
    QCOMPARE(result.status, wallet::CWallet::ScanResult::SUCCESS);
}

std::shared_ptr<wallet::CWallet> Wave19SetupDescriptorsWallet(
    interfaces::Node& node, TestChain100Setup& test, const std::string& wallet_name)
{
    std::shared_ptr<wallet::CWallet> wallet = std::make_shared<wallet::CWallet>(
        node.context()->chain.get(), wallet_name, CreateMockableWalletDatabase());
    wallet->LoadWallet();
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet->SetupDescriptorScriptPubKeyMans();

    FlatSigningProvider provider;
    std::string error;
    std::unique_ptr<Descriptor> desc = Parse(
        "combo(" + EncodeSecret(test.coinbaseKey) + ")", provider, error, false);
    assert(desc);
    wallet::WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    if (!wallet->AddWalletDescriptor(w_desc, provider, "", false)) assert(false);
    CTxDestination dest = GetDestinationForKey(test.coinbaseKey.GetPubKey(),
                                               wallet->m_default_address_type);
    wallet->SetAddressBook(dest, "", wallet::AddressPurpose::RECEIVE);
    wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(),
        return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    Wave19SyncUpWallet(wallet, node);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

struct Wave19MiniGUI {
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;
    std::unique_ptr<const PlatformStyle> platformStyle;

    explicit Wave19MiniGUI(interfaces::Node& node) : optionsModel(node)
    {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
        platformStyle.reset(PlatformStyle::instantiate("other"));
    }

    void initModelForWallet(interfaces::Node& node,
                            const std::shared_ptr<wallet::CWallet>& wallet)
    {
        WalletContext& context = *node.walletLoader().context();
        AddWallet(context, wallet);
        walletModel = std::make_unique<WalletModel>(
            interfaces::MakeWallet(context, wallet), *clientModel,
            platformStyle.get());
        RemoveWallet(context, wallet, std::nullopt);
    }
};

void Wave19AddMockPosition(const std::shared_ptr<wallet::CWallet>& wallet,
                           const uint256& id, CAmount dd_amount,
                           CAmount collateral, uint32_t tier,
                           int64_t unlock_height)
{
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);
    dd_wallet->AddCollateralPosition(WalletCollateralPosition(
        id, dd_amount, collateral, tier, unlock_height));
}

std::string Wave19EncodeDigiDollarAddressFor(int networkType)
{
    uint256 hash;
    hash.SetHex("89abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567");
    XOnlyPubKey xonly(hash);
    WitnessV1Taproot taproot(xonly);
    CTxDestination dest = taproot;

    CDigiDollarAddress addr;
    assert(addr.SetDigiDollar(dest, networkType));
    return addr.ToString();
}

bool MaybeSkipMacMinimal()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping DigiDollarWave19WidgetTests on mac minimal platform.");
        return true;
    }
#endif
    return false;
}

} // namespace

void DigiDollarWave19WidgetTests::redeemWidgetLockTierShowsHumanReadableLabel()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-redeem-tier-label");
    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    struct TierExpectation {
        uint32_t tier;
        const char* expected;
    };
    const TierExpectation expectations[] = {
        {0, "1 hour"},   {1, "30 days"},  {2, "3 months"},
        {3, "6 months"}, {4, "1 year"},   {5, "2 years"},
        {6, "3 years"},  {7, "5 years"},  {8, "7 years"},
        {9, "10 years"},
    };

    for (const auto& exp : expectations) {
        uint256 pos_id;
        // Use a valid hex digit per tier so SetHex parses each pos_id; the
        // earlier 'a' + tier mapping silently produced 'g'..'j' for tiers 6-9
        // which left pos_id zeroed and the redeem widget rendered "N/A".
        // Offset by 1 so tier 0 doesn't yield the null uint256.
        static constexpr char kHexDigits[16] =
            {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
        std::string hex(63, '0');
        hex.push_back(kHexDigits[(exp.tier + 1) % 16]);
        pos_id.SetHex(hex);

        Wave19AddMockPosition(wallet, pos_id, 10000, 300 * COIN, exp.tier, /*unlock_height=*/200);
        wallet->GetDDWallet()->AddDDUTXO(COutPoint(pos_id, 1), 10000);

        DigiDollarRedeemWidget redeemWidget;
        redeemWidget.setWalletModel(mini_gui.walletModel.get());
        redeemWidget.setClientModel(mini_gui.clientModel.get());
        redeemWidget.setPosition(QString::fromStdString(pos_id.GetHex()));
        QCoreApplication::processEvents();

        QLabel* lockTierValue = redeemWidget.findChild<QLabel*>("lockTierValue");
        QVERIFY(lockTierValue != nullptr);
        QVERIFY2(
            lockTierValue->text().contains(exp.expected, Qt::CaseInsensitive),
            qPrintable(QString("DD-FA-FUNC-030: redeem widget tier %1 should "
                               "render as '%2', got '%3'")
                           .arg(exp.tier)
                           .arg(exp.expected)
                           .arg(lockTierValue->text())));
        QVERIFY2(!lockTierValue->text().startsWith("Tier "),
                 qPrintable(QString("DD-FA-FUNC-030 regression: redeem widget "
                                    "must not show 'Tier N' for tier %1, got '%2'")
                                .arg(exp.tier)
                                .arg(lockTierValue->text())));
        if (exp.tier == 0) {
            QVERIFY2(!lockTierValue->text().contains("test", Qt::CaseInsensitive),
                     qPrintable(QString("DD-FA-DOC-034: tier 0 is canonical on "
                                        "all networks and must not render as test-only, got '%1'")
                                    .arg(lockTierValue->text())));
        }
    }

    RemoveWallet(context, wallet, std::nullopt);
}

void DigiDollarWave19WidgetTests::mintWidgetTierComboShowsAllCanonicalTiers()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-tier-combo");
    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarMintWidget mintWidget;
    mintWidget.setWalletModel(mini_gui.walletModel.get());
    mintWidget.setClientModel(mini_gui.clientModel.get());

    QComboBox* combo = mintWidget.findChild<QComboBox*>("lockTierCombo");
    QVERIFY(combo != nullptr);

    // Wave 7 invariant: lock tiers are canonical only (0..9). The mint dropdown
    // must therefore expose exactly ten entries and each entry's data() must be
    // the integer tier index used by the consensus mint validator.
    QCOMPARE(combo->count(), 10);
    const QStringList canonical = {
        QStringLiteral("1 hour"),  QStringLiteral("30 days"),
        QStringLiteral("3 months"), QStringLiteral("6 months"),
        QStringLiteral("1 year"),  QStringLiteral("2 years"),
        QStringLiteral("3 years"), QStringLiteral("5 years"),
        QStringLiteral("7 years"), QStringLiteral("10 years"),
    };
    for (int i = 0; i < combo->count(); ++i) {
        QCOMPARE(combo->itemData(i).toInt(), i);
        const QString text = combo->itemText(i);
        QVERIFY2(text.contains(canonical.at(i), Qt::CaseInsensitive),
                 qPrintable(QString("DD-FA-TEST-027: mint tier %1 must render "
                                    "with canonical label '%2', got '%3'")
                                .arg(i)
                                .arg(canonical.at(i))
                                .arg(text)));
        if (i == 0) {
            QVERIFY2(!text.contains("test", Qt::CaseInsensitive),
                     qPrintable(QString("DD-FA-DOC-034: mint tier 0 is a "
                                        "canonical funds-locking tier, not a "
                                        "test-only tier; got '%1'")
                                    .arg(text)));
        }
    }

    QCOMPARE(combo->currentData().toInt(), 1);
}

void DigiDollarWave19WidgetTests::mintWidgetTierChangeRefreshesCollateralRatio()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MockOracleManager::GetInstance().SetEnabled(true);
    MockOracleManager::GetInstance().SetMockPrice(500000); // $0.50/DGB micro-USD

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-tier-ratio");
    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarMintWidget mintWidget;
    mintWidget.setWalletModel(mini_gui.walletModel.get());
    mintWidget.setClientModel(mini_gui.clientModel.get());
    mintWidget.show();
    mintWidget.updateView();

    QComboBox* combo = mintWidget.findChild<QComboBox*>("lockTierCombo");
    QVERIFY(combo != nullptr);
    QLabel* ratioValue = mintWidget.findChild<QLabel*>("lockTierInfoValue");
    QVERIFY(ratioValue != nullptr);
    QLabel* currentRatioValue = mintWidget.findChild<QLabel*>("ratioValue");
    QVERIFY(currentRatioValue != nullptr);
    QProgressBar* ratioBar = mintWidget.findChild<QProgressBar*>("ratioBar");
    QVERIFY(ratioBar != nullptr);

    struct TierRatio { int tier; const char* ratio; };
    const TierRatio tier_ratios[] = {
        {0, "1000%"}, {1, "500%"}, {2, "400%"}, {3, "350%"},
    };
    for (const auto& tr : tier_ratios) {
        combo->setCurrentIndex(tr.tier);
        QCoreApplication::processEvents();
        QVERIFY2(ratioValue->text().contains(tr.ratio),
                 qPrintable(QString("DD-FA-TEST-027: mint tier %1 must render "
                                    "ratio '%2', got '%3'")
                                .arg(tr.tier)
                                .arg(tr.ratio)
                                .arg(ratioValue->text())));
    }

    QCOMPARE(ratioBar->minimum(), 200);
    QCOMPARE(ratioBar->maximum(), 500);
    QVERIFY2(!ratioBar->isTextVisible(),
             "The collateral ratio bar must not print the clamped visual value as if it were the exact ratio");

    combo->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QCOMPARE(currentRatioValue->text(), QStringLiteral("500%"));
    QCOMPARE(ratioBar->value(), 500);

    combo->setCurrentIndex(0);
    QCoreApplication::processEvents();
    QCOMPARE(currentRatioValue->text(), QStringLiteral("1000%"));
    QCOMPARE(ratioBar->value(), 500);

    MockOracleManager::GetInstance().Reset();
}

void DigiDollarWave19WidgetTests::positionsWidgetTierZeroTooltipIsCanonical()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-tier-zero-tooltip");
    uint256 pos_id;
    pos_id.SetHex("0000000000000000000000000000000000000000000000000000000000000001");
    Wave19AddMockPosition(wallet, pos_id, 10000, 1000 * COIN,
                          /*tier=*/0, /*unlock_height=*/200);

    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updatePositions();
    QCoreApplication::processEvents();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QTableWidgetItem* tierItem = table->item(0, DigiDollarPositionsWidget::COL_LOCK_TIER);
    QVERIFY(tierItem != nullptr);
    QCOMPARE(tierItem->text(), QString("1 hour + 100 block buffer"));
    QVERIFY2(tierItem->toolTip().contains("100-block confirmation buffer"),
             qPrintable(QString("tier 0 tooltip must explain the confirmation buffer, got '%1'")
                            .arg(tierItem->toolTip())));
    QVERIFY2(!tierItem->toolTip().contains("test", Qt::CaseInsensitive),
             qPrintable(QString("DD-FA-DOC-034: tier 0 tooltip must not call "
                                "the canonical 1-hour tier test-only, got '%1'")
                            .arg(tierItem->toolTip())));
}

void DigiDollarWave19WidgetTests::overviewWidgetUsesProtocolAcronymTooltips()
{
    if (MaybeSkipMacMinimal()) return;

    DigiDollarOverviewWidget overviewWidget;
    QLabel* dcaValue = overviewWidget.findChild<QLabel*>("dcaLevelValue");
    QVERIFY(dcaValue != nullptr);
    QVERIFY2(dcaValue->toolTip().contains("Dynamic Collateral Adjustment"),
             qPrintable(QString("DD-FA-DOC-035: DCA tooltip must use the "
                                "protocol expansion, got '%1'")
                            .arg(dcaValue->toolTip())));

    QLabel* errValue = overviewWidget.findChild<QLabel*>("errLevelValue");
    QVERIFY(errValue != nullptr);
    QVERIFY2(errValue->toolTip().contains("Emergency Redemption Ratio"),
             qPrintable(QString("DD-FA-DOC-035: ERR tooltip must use the "
                                "protocol expansion, got '%1'")
                            .arg(errValue->toolTip())));
}

void DigiDollarWave19WidgetTests::walletModelAndSendValidatorRejectCrossNetworkDDAddresses()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-address-validator");
    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    const QString regtestAddr = QString::fromStdString(
        Wave19EncodeDigiDollarAddressFor(CChainParams::DIGIDOLLAR_ADDRESS_REGTEST));
    const QString mainnetAddr = QString::fromStdString(
        Wave19EncodeDigiDollarAddressFor(CChainParams::DIGIDOLLAR_ADDRESS));
    QString corruptedAddr = regtestAddr;
    corruptedAddr[corruptedAddr.size() - 1] =
        corruptedAddr.endsWith('1') ? QChar('2') : QChar('1');

    QVERIFY2(mini_gui.walletModel->validateDigiDollarAddress(regtestAddr),
             "DD-FA-FUNC-063: wallet model must accept current-network DD address");
    QVERIFY2(!mini_gui.walletModel->validateDigiDollarAddress(mainnetAddr),
             qPrintable(QString("DD-FA-FUNC-063: wallet model must reject "
                                "cross-network DD address '%1' on regtest")
                            .arg(mainnetAddr)));
    QVERIFY2(!mini_gui.walletModel->validateDigiDollarAddress(corruptedAddr),
             qPrintable(QString("DD-FA-FUNC-063: wallet model must reject "
                                "checksum-corrupted DD address '%1'")
                            .arg(corruptedAddr)));

    DigiDollarAddressValidator validator;
    int pos = 0;
    QString currentCopy = regtestAddr;
    QCOMPARE(validator.validate(currentCopy, pos), QValidator::Acceptable);
    pos = 0;
    QString crossCopy = mainnetAddr;
    QVERIFY2(validator.validate(crossCopy, pos) != QValidator::Acceptable,
             "DD-FA-FUNC-063: send widget validator must reject cross-network DD address");
    pos = 0;
    QString corruptedCopy = corruptedAddr;
    QVERIFY2(validator.validate(corruptedCopy, pos) != QValidator::Acceptable,
             "DD-FA-FUNC-063: send widget validator must reject checksum-corrupted DD address");
}

// A DigiDollar address must be typeable, not just pasteable. The box used to
// refuse the 42nd character, so anyone typing an address got stuck at 41 and
// the form told them the address was invalid.
void DigiDollarWave19WidgetTests::sendAddressValidatorTakesAnAddressTypedOneCharacterAtATime()
{
    if (MaybeSkipMacMinimal()) return;
    BasicTestingSetup setup{ChainType::REGTEST};

    const QString address = QString::fromStdString(
        Wave19EncodeDigiDollarAddressFor(CChainParams::DIGIDOLLAR_ADDRESS_REGTEST));
    QVERIFY2(address.length() == 52,
             qPrintable(QString("a DigiDollar address is 52 characters, this one is %1")
                            .arg(address.length())));

    DigiDollarAddressValidator validator;

    // Every keystroke on the way to a whole address must be taken.
    for (int typed = 1; typed < address.length(); ++typed) {
        QString sofar = address.left(typed);
        int pos = typed;
        QVERIFY2(validator.validate(sofar, pos) != QValidator::Invalid,
                 qPrintable(QString("the address box refused character %1 of %2 ('%3')")
                                .arg(typed)
                                .arg(address.length())
                                .arg(sofar)));
    }

    // The finished address is accepted.
    QString whole = address;
    int pos = whole.length();
    QCOMPARE(validator.validate(whole, pos), QValidator::Acceptable);

    // Text that does not start the way an address starts can never become one,
    // so it is refused.
    QString wrongStart("XD3");
    pos = wrongStart.length();
    QCOMPARE(validator.validate(wrongStart, pos), QValidator::Invalid);

    // Text longer than an address can never become one either.
    QString tooLong = address + QStringLiteral("A");
    pos = tooLong.length();
    QCOMPARE(validator.validate(tooLong, pos), QValidator::Invalid);

    // A finished address with a mistake in it is kept, not refused, so the
    // person can correct it in place and the form can say what is wrong. It is
    // never acceptable.
    QString typo = address;
    typo[20] = (typo.at(20) == QChar('a')) ? QChar('b') : QChar('a');
    pos = typo.length();
    QCOMPARE(validator.validate(typo, pos), QValidator::Intermediate);
}

void DigiDollarWave19WidgetTests::sendAmountValidatorUsesCentsPrecisionAndBounds()
{
    AmountValidator validator(1.00, 100000.00, 2);
    int pos = 0;

    QString minAmount("1.00");
    QCOMPARE(validator.validate(minAmount, pos), QValidator::Acceptable);
    pos = 0;
    QString maxAmount("100000.00");
    QCOMPARE(validator.validate(maxAmount, pos), QValidator::Acceptable);
    pos = 0;
    QString extraPrecision("1.00000001");
    QCOMPARE(validator.validate(extraPrecision, pos), QValidator::Invalid);
    pos = 0;
    // An amount over the limit can be typed but can never be sent. If the box
    // refused the keystroke, typing 200000 would leave 20000 sitting there and
    // nothing would say why, so the form takes the number and then explains it.
    QString overMax("999999999");
    QCOMPARE(validator.validate(overMax, pos), QValidator::Intermediate);
    QVERIFY2(validator.validate(overMax, pos) != QValidator::Acceptable,
             "an amount over the limit must never be acceptable");
}

void DigiDollarWave19WidgetTests::mintWidgetUsdEquivalentUsesCentsPrecision()
{
    if (MaybeSkipMacMinimal()) return;

    DigiDollarMintWidget mintWidget;
    QLineEdit* amountEdit = mintWidget.findChild<QLineEdit*>("amountEdit");
    QVERIFY(amountEdit != nullptr);
    QLabel* usdValue = mintWidget.findChild<QLabel*>("usdValueValue");
    QVERIFY(usdValue != nullptr);

    QCOMPARE(usdValue->text(), QStringLiteral("0.00 $USD"));

    amountEdit->setText(QStringLiteral("100"));
    QCoreApplication::processEvents();
    QCOMPARE(usdValue->text(), QStringLiteral("100.00 $USD"));

    amountEdit->setText(QStringLiteral("100.1"));
    QCoreApplication::processEvents();
    QCOMPARE(usdValue->text(), QStringLiteral("100.10 $USD"));
}

void DigiDollarWave19WidgetTests::positionsWidgetMissingOracleHealthIsUnavailable()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    MockOracleManager::GetInstance().Reset();
    MockOracleManager::GetInstance().SetEnabled(false);
    OracleBundleManager::GetInstance().Clear();

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-missing-oracle-health");
    uint256 pos_id;
    pos_id.SetHex("0000000000000000000000000000000000000000000000000000000000000002");
    Wave19AddMockPosition(wallet, pos_id, 10000, 300 * COIN,
                          /*tier=*/1, /*unlock_height=*/200);

    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarPositionsWidget positionsWidget;
    positionsWidget.setWalletModel(mini_gui.walletModel.get());
    positionsWidget.setClientModel(mini_gui.clientModel.get());
    positionsWidget.show();
    QCoreApplication::processEvents();
    positionsWidget.updatePositions();
    QCoreApplication::processEvents();

    QTableWidget* table = positionsWidget.findChild<QTableWidget*>("positionsTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QWidget* healthWidget = table->cellWidget(0, DigiDollarPositionsWidget::COL_HEALTH);
    QVERIFY(healthWidget != nullptr);
    QProgressBar* healthBar = healthWidget->findChild<QProgressBar*>();
    QVERIFY(healthBar != nullptr);
    QCOMPARE(healthBar->format(), QString("N/A"));
    QVERIFY2(healthBar->toolTip().contains("Oracle price unavailable"),
             qPrintable(QString("DD-FA-FUNC-064: missing oracle price must "
                                "not render as a false 0.0%% At Risk state, "
                                "tooltip was '%1'")
                            .arg(healthBar->toolTip())));

    MockOracleManager::GetInstance().Reset();
    OracleBundleManager::GetInstance().Clear();
}

void DigiDollarWave19WidgetTests::transactionsWidgetTypeFilterFiltersRows()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-tx-filter");
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    auto pushTx = [&](const std::string& txid, CAmount amount, bool incoming,
                      const std::string& category, int lock_tier, int64_t offset) {
        DDTransaction tx;
        tx.txid = txid;
        tx.amount = amount;
        tx.timestamp = GetTime() + offset;
        tx.confirmations = 1;
        tx.incoming = incoming;
        tx.address = "TDfilterrowaddress";
        tx.category = category;
        tx.lock_tier = lock_tier;
        tx.fee = 0;
        tx.abandoned = false;
        dd_wallet->AddMockTransaction(tx);
    };
    pushTx("c000000000000000000000000000000000000000000000000000000000000001", 100, true,  "mint",    1, 4);
    pushTx("c000000000000000000000000000000000000000000000000000000000000002", 200, false, "send",   -1, 3);
    pushTx("c000000000000000000000000000000000000000000000000000000000000003", 300, true,  "receive", -1, 2);
    pushTx("c000000000000000000000000000000000000000000000000000000000000004", 400, false, "redeem",  1, 1);

    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());
    transactionsWidget.show();
    QCoreApplication::processEvents();
    transactionsWidget.updateView();
    QCoreApplication::processEvents();

    QComboBox* typeFilter = transactionsWidget.findChild<QComboBox*>();
    QVERIFY(typeFilter != nullptr);
    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);

    QCOMPARE(table->rowCount(), 4);

    struct FilterCase { const char* data; const char* type_prefix; };
    const FilterCase cases[] = {
        {"mint",    "Mint"},
        {"send",    "Send"},
        {"receive", "Receive"},
        {"redeem",  "Redeem"},
    };
    for (const auto& fc : cases) {
        const int idx = typeFilter->findData(QString::fromUtf8(fc.data));
        QVERIFY2(idx >= 0, qPrintable(QString("missing filter entry for %1").arg(fc.data)));
        typeFilter->setCurrentIndex(idx);
        QCoreApplication::processEvents();
        QCOMPARE(table->rowCount(), 1);
        QVERIFY2(table->item(0, 1)->text().startsWith(fc.type_prefix),
                 qPrintable(QString("DD-FA-TEST-028: type filter '%1' should "
                                    "leave only %2 rows, got '%3'")
                                .arg(fc.data)
                                .arg(fc.type_prefix)
                                .arg(table->item(0, 1)->text())));
    }

    typeFilter->setCurrentIndex(typeFilter->findData(QString()));
    QCoreApplication::processEvents();
    QCOMPARE(table->rowCount(), 4);

    RemoveWallet(context, wallet, std::nullopt);
}

void DigiDollarWave19WidgetTests::transactionsWidgetSearchFilterMatchesByTxid()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-tx-search");
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    auto pushTx = [&](const std::string& txid, CAmount amount, bool incoming,
                      const std::string& category, int64_t offset) {
        DDTransaction tx;
        tx.txid = txid;
        tx.amount = amount;
        tx.timestamp = GetTime() + offset;
        tx.confirmations = 1;
        tx.incoming = incoming;
        tx.address = "TDsearchaddress";
        tx.category = category;
        tx.lock_tier = -1;
        tx.fee = 0;
        tx.abandoned = false;
        dd_wallet->AddMockTransaction(tx);
    };
    pushTx("d111111111111111111111111111111111111111111111111111111111111111", 100, true,  "mint", 4);
    pushTx("d222222222222222222222222222222222222222222222222222222222222222", 200, false, "send", 3);
    pushTx("d333333333333333333333333333333333333333333333333333333333333333", 300, true,  "receive", 2);

    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());
    transactionsWidget.show();
    transactionsWidget.updateView();
    QCoreApplication::processEvents();

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 3);

    QLineEdit* searchEdit = transactionsWidget.findChild<QLineEdit*>();
    QVERIFY(searchEdit != nullptr);

    searchEdit->setText("d2222222");
    QCoreApplication::processEvents();
    QCOMPARE(table->rowCount(), 1);
    const QString remainingTxid = table->item(0, 5)->data(Qt::UserRole).toString();
    QVERIFY2(remainingTxid.startsWith("d2222222"),
             qPrintable(QString("DD-FA-TEST-028: txid search must keep matching "
                                "row, got '%1'")
                            .arg(remainingTxid)));

    searchEdit->setText("zzzzzzzz");
    QCoreApplication::processEvents();
    QCOMPARE(table->rowCount(), 0);

    searchEdit->clear();
    QCoreApplication::processEvents();
    QCOMPARE(table->rowCount(), 3);

    RemoveWallet(context, wallet, std::nullopt);
}

void DigiDollarWave19WidgetTests::transactionsWidgetPreservesUserSortAcrossRefresh()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-tx-sort");
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    auto pushTx = [&](const std::string& txid, CAmount amount, bool incoming,
                      const std::string& category, int64_t timestamp) {
        DDTransaction tx;
        tx.txid = txid;
        tx.amount = amount;
        tx.timestamp = timestamp;
        tx.confirmations = 1;
        tx.incoming = incoming;
        tx.address = "TDsortaddress";
        tx.category = category;
        tx.lock_tier = -1;
        tx.fee = 0;
        tx.abandoned = false;
        dd_wallet->AddMockTransaction(tx);
    };
    // Use dates whose localized display strings have a different lexical and
    // chronological order. This catches regressions where the Date column
    // sorts "Aug" before "Jul" instead of comparing Unix timestamps.
    const int64_t jul_30_2144 =
        QDateTime::fromString(QStringLiteral("2026-07-30T21:44:00Z"), Qt::ISODate).toSecsSinceEpoch();
    const int64_t aug_02_0031 =
        QDateTime::fromString(QStringLiteral("2026-08-02T00:31:00Z"), Qt::ISODate).toSecsSinceEpoch();
    const int64_t aug_02_0102 =
        QDateTime::fromString(QStringLiteral("2026-08-02T01:02:00Z"), Qt::ISODate).toSecsSinceEpoch();
    const int64_t aug_02_2330 =
        QDateTime::fromString(QStringLiteral("2026-08-02T23:30:00Z"), Qt::ISODate).toSecsSinceEpoch();
    pushTx("e111111111111111111111111111111111111111111111111111111111111111", 300, true, "mint", jul_30_2144);
    pushTx("e222222222222222222222222222222222222222222222222222222222222222", 100, false, "send", aug_02_0031);
    pushTx("e333333333333333333333333333333333333333333333333333333333333333", 200, true, "receive", aug_02_0102);
    pushTx("e444444444444444444444444444444444444444444444444444444444444444", 400, true, "receive", aug_02_2330);

    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    DigiDollarTransactionsWidget transactionsWidget;
    transactionsWidget.setWalletModel(mini_gui.walletModel.get());
    transactionsWidget.setClientModel(mini_gui.clientModel.get());
    transactionsWidget.show();
    transactionsWidget.updateView();
    QCoreApplication::processEvents();

    QTableWidget* table = transactionsWidget.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 4);
    QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 0);
    QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);

    auto timestampAt = [&](int row) {
        return table->item(row, 0)->data(Qt::UserRole).toLongLong();
    };
    QCOMPARE(timestampAt(0), aug_02_2330);
    QCOMPARE(timestampAt(1), aug_02_0102);
    QCOMPARE(timestampAt(2), aug_02_0031);
    QCOMPARE(timestampAt(3), jul_30_2144);

    table->sortByColumn(0, Qt::AscendingOrder);
    QCoreApplication::processEvents();
    QCOMPARE(timestampAt(0), jul_30_2144);
    QCOMPARE(timestampAt(1), aug_02_0031);
    QCOMPARE(timestampAt(2), aug_02_0102);
    QCOMPARE(timestampAt(3), aug_02_2330);

    table->sortByColumn(2, Qt::AscendingOrder);
    QCoreApplication::processEvents();
    QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 2);
    QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::AscendingOrder);

    transactionsWidget.updateView();
    QCoreApplication::processEvents();
    QCOMPARE(table->rowCount(), 4);
    QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 2);
    QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::AscendingOrder);

    RemoveWallet(context, wallet, std::nullopt);
}

void DigiDollarWave19WidgetTests::coinControlDialogShowsOnlySpendableDDUtxos()
{
    if (MaybeSkipMacMinimal()) return;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);

    const std::shared_ptr<wallet::CWallet>& wallet =
        Wave19SetupDescriptorsWallet(m_node, test, "qt-dd-cc-filter");
    wallet->EnsureDDWallet();
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    QVERIFY(dd_wallet != nullptr);

    // Hash() over a const char* yields a deterministic uint256.
    const COutPoint outpoint_a(Hash("dd-fa-test-029-cc-a"), 1);
    const COutPoint outpoint_b(Hash("dd-fa-test-029-cc-b"), 1);
    const COutPoint outpoint_c(Hash("dd-fa-test-029-cc-c"), 1);
    dd_wallet->AddDDUTXO(outpoint_a, 12345);
    dd_wallet->AddDDUTXO(outpoint_b, 67890);
    dd_wallet->AddDDUTXO(outpoint_c, 100000);

    Wave19MiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    WalletContext& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);

    wallet::DDCoinControl coin_control;
    DigiDollarCoinControlDialog dialog(coin_control, mini_gui.walletModel.get(),
                                       mini_gui.platformStyle.get());
    dialog.show();
    QCoreApplication::processEvents();

    QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
    QVERIFY(tree != nullptr);

    // Mirror DigiDollarCoinControlDialog's private anonymous column enum:
    // CHECKBOX, AMOUNT, LABEL, ADDRESS, DATE, CONFIRMATIONS, TXID_VOUT.
    // The test reads the rendered cell text rather than reaching into private
    // class members, so the column index is hard-coded.
    constexpr int kColumnTxidVout = 6;

    QSet<QString> seen;
    auto collect = [&](QTreeWidgetItem* root, auto&& self) -> void {
        for (int i = 0; i < root->childCount(); ++i) {
            QTreeWidgetItem* child = root->child(i);
            self(child, self);
            const QString outpoint = child->text(kColumnTxidVout);
            if (!outpoint.isEmpty()) seen.insert(outpoint);
        }
    };
    collect(tree->invisibleRootItem(), collect);

    auto formatOutpoint = [](const COutPoint& op) -> QString {
        return QString::fromStdString(op.hash.GetHex()) + ":" + QString::number(op.n);
    };
    QVERIFY2(seen.contains(formatOutpoint(outpoint_a)),
             "DD-FA-TEST-029: coin control dialog must show outpoint A");
    QVERIFY2(seen.contains(formatOutpoint(outpoint_b)),
             "DD-FA-TEST-029: coin control dialog must show outpoint B");
    QVERIFY2(seen.contains(formatOutpoint(outpoint_c)),
             "DD-FA-TEST-029: coin control dialog must show outpoint C");
    QCOMPARE(seen.size(), 3);

    RemoveWallet(context, wallet, std::nullopt);
}
